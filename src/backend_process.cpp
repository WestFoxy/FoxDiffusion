#include "backend_process.hpp"
#include "image_io.hpp"
#include "img2img_pipeline.hpp"
#include "native_upscale.hpp"
#include "cuda_runtime.hpp"
#include "app_log.hpp"
#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

namespace {
struct PromptLora {
    std::string path;
    float weight = 1.0f;
};

std::string resolve_lora_path(const std::string &directory, const std::string &name) {
    if (directory.empty())
        return name;
    const auto root = std::filesystem::u8path(directory);
    std::error_code error;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!entry.is_regular_file(error) ||
            entry.path().stem().u8string() !=
                std::u8string(reinterpret_cast<const char8_t *>(name.data()), name.size()))
            continue;
        const auto relative = std::filesystem::relative(entry.path(), root, error).generic_u8string();
        if (!error)
            return {reinterpret_cast<const char *>(relative.data()), relative.size()};
    }
    return name;
}

void extract_loras(std::string &prompt, const std::string &directory, std::vector<PromptLora> &loras) {
    static const std::regex pattern(R"(<lora:([^:>]+):([-+]?(?:\d+(?:\.\d*)?|\.\d+))>)", std::regex::icase);
    std::string clean;
    std::size_t copied = 0;
    for (std::sregex_iterator it(prompt.begin(), prompt.end(), pattern), end; it != end; ++it) {
        clean.append(prompt, copied, static_cast<std::size_t>(it->position()) - copied);
        loras.push_back({resolve_lora_path(directory, (*it)[1].str()), std::stof((*it)[2].str())});
        copied = static_cast<std::size_t>(it->position() + it->length());
    }
    clean.append(prompt, copied, std::string::npos);
    prompt = std::move(clean);
}

Raster copy_image(const sd_image_t &image) {
    if (!image.data || image.channel < 1 || image.channel > 4)
        throw std::runtime_error("Engine returned an invalid image");
    Raster result(image.width, image.height);
    for (size_t i = 0; i < size_t(image.width) * image.height; ++i) {
        for (int c = 0; c < 3; ++c)
            result.pixels[i * 4 + c] = image.data[i * image.channel + (image.channel < 3 ? 0 : c)];
    }
    return result;
}
struct InputImage {
    std::vector<uint8_t> bytes;
    sd_image_t image{};
    InputImage(const Raster &raster, int channels) {
        if (raster.empty())
            return;
        bytes.resize(size_t(raster.w) * raster.h * channels);
        for (size_t i = 0; i < size_t(raster.w) * raster.h; ++i)
            std::copy_n(raster.pixels.data() + i * 4, channels, bytes.data() + i * channels);
        image = {uint32_t(raster.w), uint32_t(raster.h), uint32_t(channels), bytes.data()};
    }
};
struct Images {
    sd_image_t *data = nullptr;
    int count = 0;
    ~Images() {
        if (data)
            free_sd_images(data, count);
    }
};
} // namespace
BackendProcess::BackendProcess() {
    sd_set_log_callback(app_log::engine, nullptr);
    backend_status_ = cuda_driver_status();
    save_worker_ = std::jthread([this] { save_outputs(); });
}
BackendProcess::~BackendProcess() {
    stop();
    {
        std::scoped_lock lock(mutex_);
        close_saver_ = true;
    }
    save_cv_.notify_all();
    if (save_worker_.joinable())
        save_worker_.join();
}

void BackendProcess::queue_save(SaveJob job) {
    std::unique_lock lock(mutex_);
    // Bound queued pixels; only the generation worker waits if disk falls behind.
    save_cv_.wait(lock, [&] {
        return save_queue_.empty() || queued_save_bytes_ + job.image.pixels.size() <= 256ULL * 1024 * 1024;
    });
    save_queue_.push_back(std::move(job));
    queued_save_bytes_ += save_queue_.back().image.pixels.size();
    ++pending_saves_;
    lock.unlock();
    save_cv_.notify_all();
}

void BackendProcess::save_outputs() {
    for (;;) {
        SaveJob job;
        {
            std::unique_lock lock(mutex_);
            save_cv_.wait(lock, [&] { return close_saver_ || !save_queue_.empty(); });
            if (save_queue_.empty())
                return;
            job = std::move(save_queue_.front());
            save_queue_.pop_front();
            queued_save_bytes_ -= job.image.pixels.size();
        }
        save_cv_.notify_all();
        std::string error;
        bool image_saved = false;
        try {
            std::filesystem::create_directories(job.path.parent_path());
            image_saved = save_png_parameters(job.path, job.image.pixels, job.image.w, job.image.h,
                                              job.metadata, error);
            if (image_saved) {
                auto info = job.path;
                info.replace_extension(L".txt");
                std::ofstream sidecar(info, std::ios::binary);
                sidecar << job.metadata;
                sidecar.close();
                if (!sidecar)
                    error = "PNG saved, but could not save generation metadata sidecar";
            } else if (error.empty())
                error = "Could not save PNG";
        } catch (const std::exception &e) {
            error = e.what();
        }
        {
            const auto saved_path = job.path.u8string();
            std::scoped_lock lock(mutex_);
            --pending_saves_;
            if (!error.empty())
                save_error_ =
                    std::string(reinterpret_cast<const char *>(saved_path.data()), saved_path.size()) + ": " +
                    error;
            if (image_saved && job.workflow == workflow_id_) {
                const auto utf8 = job.path.u8string();
                snapshot_.outputs.emplace_back(reinterpret_cast<const char *>(utf8.data()), utf8.size());
            }
        }
    }
}

void BackendProcess::start(GenerationRequest request) {
    if (workflow_running_)
        return;
    if (worker_.joinable())
        worker_.join();
    cancel_requested_ = false;
    {
        std::scoped_lock lock(mutex_);
        ++workflow_id_;
        const auto revision = snapshot_.revision;
        snapshot_ = {};
        snapshot_.revision = revision + 1;
        snapshot_.running = true;
        snapshot_.total_steps = request.steps;
        snapshot_.status = "Preparing persistent backend...";
    }
    workflow_running_ = true;
    worker_ = std::jthread([this, request = std::move(request)](std::stop_token token) mutable {
        try {
            run(std::move(request), token);
        } catch (const std::exception &e) {
            set_status(e.what());
            std::scoped_lock lock(mutex_);
            snapshot_.running = false;
            snapshot_.finished = false;
        }
        if (token.stop_requested())
            release_context();
        workflow_running_ = false;
    });
}

void BackendProcess::cancel() {
    cancel_requested_ = true;
    if (worker_.joinable())
        worker_.request_stop();
    {
        std::scoped_lock lock(context_mutex_);
        if (context_)
            sd_cancel_generation(context_, SD_CANCEL_ALL);
    }
    if (workflow_running_)
        set_status("Cancelling...");
}
void BackendProcess::stop() {
    cancel();
    if (worker_.joinable())
        worker_.join();
    release_context();
}

bool BackendProcess::ready() const {
    return cuda_driver_compatible();
}
std::string BackendProcess::status() const {
    std::scoped_lock lock(mutex_);
    return backend_status_;
}
GenerationSnapshot BackendProcess::snapshot() const {
    std::scoped_lock lock(mutex_);
    auto value = snapshot_;
    value.running = workflow_running_;
    value.pending_saves = pending_saves_;
    value.save_error = save_error_;
    return value;
}

void BackendProcess::poll_snapshot(GenerationSnapshot &cached) const {
    std::scoped_lock lock(mutex_);
    // Image ownership stays with the worker. Copy to the UI only on publication,
    // never for status polling, panning, zooming or drawing another panel.
    if (cached.revision != snapshot_.revision) {
        cached.rgba = snapshot_.rgba;
        cached.width = snapshot_.width;
        cached.height = snapshot_.height;
        cached.revision = snapshot_.revision;
    }
    cached.pending_saves = pending_saves_;
    cached.save_error = save_error_;
    cached.running = workflow_running_.load();
    cached.finished = snapshot_.finished;
    cached.progress = snapshot_.progress;
    cached.current_step = snapshot_.current_step;
    cached.preview_count = snapshot_.preview_count;
    cached.total_steps = snapshot_.total_steps;
    cached.status = snapshot_.status;
    cached.job_index = snapshot_.job_index;
    cached.job_count = snapshot_.job_count;
    if (cached.outputs != snapshot_.outputs)
        cached.outputs = snapshot_.outputs;
}

void BackendProcess::set_status(std::string value) {
    std::scoped_lock lock(mutex_);
    backend_status_ = value;
    snapshot_.status = std::move(value);
}

void BackendProcess::run(GenerationRequest request, std::stop_token token) {
    if (!request.img2img.enabled) {
        run_single(std::move(request), token);
        return;
    }
    const auto infer = [&](GenerationRequest job) {
        if (token.stop_requested())
            throw std::runtime_error("Cancelled");
        if (job.upscale_only)
            return run_upscale(job, token);
        {
            std::scoped_lock lock(mutex_);
            snapshot_.finished = false;
            snapshot_.progress = 0;
            snapshot_.running = true;
            snapshot_.current_step = 0;
            snapshot_.total_steps = job.steps;
        }
        run_single(std::move(job), token);
        std::scoped_lock lock(mutex_);
        if (token.stop_requested())
            throw std::runtime_error("Cancelled");
        if (!snapshot_.finished)
            throw std::runtime_error(snapshot_.status);
        Raster result;
        result.w = snapshot_.width;
        result.h = snapshot_.height;
        result.pixels = snapshot_.rgba;
        return result;
    };
    const auto publish = [&](const Raster &image, const GenerationRequest &job, int index, int total) {
        auto dir = job.img2img.output_dir.empty() ? executable_directory() / L"output"
                                                  : std::filesystem::u8path(job.img2img.output_dir);
        auto path = dir / (L"img2img-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(index) + L".png");
        std::ostringstream metadata;
        metadata << job.positive_prompt << "\nNegative prompt: " << job.negative_prompt
                 << "\nSteps: " << job.steps << ", Sampler: " << job.sampling_method
                 << ", Schedule type: " << job.scheduler << ", CFG scale: " << job.cfg_scale
                 << ", Seed: " << job.seed << ", Size: " << image.w << 'x' << image.h
                 << ", Denoising strength: " << job.img2img.strength << ", Model: " << job.model_path
                 << ", Script: " << job.img2img.script << ", Output: " << job.output_label << "\n";
        {
            std::scoped_lock lock(mutex_);
            snapshot_.rgba = image.pixels;
            snapshot_.width = image.w;
            snapshot_.height = image.h;
            snapshot_.job_index = index;
            snapshot_.job_count = total;
            snapshot_.progress = float(index) / total;
            snapshot_.status =
                auto_save_img_ ? "Result ready; saving in background" : "Result ready (auto-save off)";
            ++snapshot_.revision;
        }
        if (auto_save_img_)
            queue_save({image, path, metadata.str(), workflow_id_});
    };
    execute_img2img(std::move(request), token, infer, publish);
    std::scoped_lock lock(mutex_);
    snapshot_.running = false;
    snapshot_.finished = true;
    snapshot_.progress = 1;
    snapshot_.status = "Img2img completed";
}

Raster BackendProcess::run_upscale(const GenerationRequest &request, std::stop_token token) {
    release_context();
    ensure_cuda_runtime(token, [this](std::string s) { set_status(std::move(s)); });
    const auto model = std::filesystem::u8path(request.img2img.upscaler_model);
    if (!std::filesystem::is_regular_file(model))
        throw std::runtime_error("Select an existing ESRGAN model");
    set_status("Loading ESRGAN model...");
    return native_tiled_upscale(request.init_image, request.width, request.height, model, token,
                                [&](const Raster &preview, int done, int total) {
                                    std::scoped_lock lock(mutex_);
                                    snapshot_.rgba = preview.pixels;
                                    snapshot_.width = preview.w;
                                    snapshot_.height = preview.h;
                                    snapshot_.current_step = done;
                                    snapshot_.total_steps = total;
                                    snapshot_.progress = float(done) / total;
                                    snapshot_.status = "ESRGAN tile " + std::to_string(done) + " / " +
                                                       std::to_string(total) + " | target " +
                                                       std::to_string(request.width) + " x " +
                                                       std::to_string(request.height);
                                    ++snapshot_.revision;
                                });
}

void BackendProcess::release_context() {
    std::scoped_lock lock(context_mutex_);
    if (context_)
        free_sd_ctx(context_);
    context_ = nullptr;
    context_key_.clear();
}

void BackendProcess::ensure_context(const GenerationRequest &request, std::stop_token token) {
    ensure_cuda_runtime(token, [this](std::string s) { set_status(std::move(s)); });
    if (token.stop_requested())
        throw std::runtime_error("Cancelled");
    if (request.qwen_image && (request.vae_path.empty() || request.llm_path.empty()))
        throw std::runtime_error("Qwen requires its VAE and text encoder");
    std::ostringstream key;
    for (const auto &s : {request.model_path, request.vae_path, request.llm_path, request.vision_path,
                          request.embedding_directory})
        key << s.size() << ':' << s;
    key << request.qwen_image << request.offload_to_cpu;
    if (context_ && context_key_ == key.str())
        return;
    release_context();
    set_status("Loading model into the embedded CUDA engine...");
    sd_ctx_params_t params;
    sd_ctx_params_init(&params);
    if (request.qwen_image)
        params.diffusion_model_path = request.model_path.c_str();
    else
        params.model_path = request.model_path.c_str();
    params.vae_path = request.vae_path.c_str();
    params.llm_path = request.llm_path.c_str();
    params.llm_vision_path = request.vision_path.c_str();
    params.backend = "CUDA";
    params.params_backend = request.qwen_image && request.offload_to_cpu ? "cpu" : "CUDA";
    params.diffusion_flash_attn = true;
    params.n_threads = int(std::max(1u, std::thread::hardware_concurrency() / 2));
    std::vector<std::string> names, paths;
    if (!request.embedding_directory.empty()) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(
                 std::filesystem::u8path(request.embedding_directory))) {
            if (!entry.is_regular_file())
                continue;
            auto ext = entry.path().extension().string();
            if (ext != ".pt" && ext != ".safetensors" && ext != ".ckpt")
                continue;
            auto name = entry.path().stem().u8string(), path = entry.path().u8string();
            names.emplace_back(reinterpret_cast<const char *>(name.c_str()));
            paths.emplace_back(reinterpret_cast<const char *>(path.c_str()));
        }
    }
    std::vector<sd_embedding_t> embeddings;
    for (size_t i = 0; i < names.size(); ++i)
        embeddings.push_back({names[i].c_str(), paths[i].c_str()});
    params.embeddings = embeddings.data();
    params.embedding_count = uint32_t(embeddings.size());
    // Loading is synchronous on the worker. Cancellation never frees a live context.
    auto *loaded = new_sd_ctx(&params);
    {
        std::scoped_lock lock(context_mutex_);
        context_ = loaded;
    }
    if (!loaded)
        throw std::runtime_error("Could not load model on CUDA");
    context_key_ = key.str();
    if (token.stop_requested())
        throw std::runtime_error("Cancelled");
}

void BackendProcess::run_single(GenerationRequest request, std::stop_token token) {
    if (request.model_path.empty())
        throw std::runtime_error("Select a model first");
    ensure_context(request, token);
    generation_phase_ = "Generating";
    sd_set_log_callback([](sd_log_level_t level, const char* text, void* user) {
        app_log::engine(level, text, nullptr);
        if (!text) return;
        // Stage names from the pinned upstream; never count VAE work as sampling steps.
        if (std::string_view(text).find("decoding ") != std::string_view::npos) {
            auto& self = *static_cast<BackendProcess*>(user);
            std::scoped_lock lock(self.mutex_);
            self.generation_phase_ = "Decoding image (VAE)";
            self.snapshot_.status = self.generation_phase_;
        }
    }, this);
    sd_set_progress_callback(
        [](int step, int steps, float, void *user) {
            auto &self = *static_cast<BackendProcess *>(user);
            // Upstream resets its cancellation flag at the start of a sampling pass.
            // Reassert a pending UI cancellation if it raced that reset.
            if (self.cancel_requested_) {
                std::scoped_lock context_lock(self.context_mutex_);
                if (self.context_)
                    sd_cancel_generation(self.context_, SD_CANCEL_ALL);
            }
            std::scoped_lock lock(self.mutex_);
            // This API reports both sampling and tensor/tiling progress; img2img
            // can also shorten the sampling pass according to strength.
            self.snapshot_.current_step = step;
            self.snapshot_.total_steps = steps;
            self.snapshot_.progress = steps > 0 ? float(step) / steps : 0;
            self.snapshot_.status = self.generation_phase_ + "  " + std::to_string(step) + " / " + std::to_string(steps);
        },
        this);
    sd_set_preview_callback(
        [](int, int count, sd_image_t *frames, bool, void *user) {
            if (count < 1 || !frames)
                return;
            try {
                auto image = copy_image(frames[0]);
                auto &self = *static_cast<BackendProcess *>(user);
                std::scoped_lock lock(self.mutex_);
                self.snapshot_.rgba = std::move(image.pixels);
                self.snapshot_.width = image.w;
                self.snapshot_.height = image.h;
                ++self.snapshot_.preview_count;
                ++self.snapshot_.revision;
            } catch (...) {
            } // Never unwind through a C callback.
        },
        preview_t(std::clamp(request.preview_mode, 0, int(PREVIEW_COUNT) - 1)), request.preview_interval,
        request.preview_denoised, request.preview_noisy, this);
    struct Callbacks {
        ~Callbacks() {
            sd_set_log_callback(app_log::engine, nullptr);
            sd_set_progress_callback(nullptr, nullptr);
            sd_set_preview_callback(nullptr, PREVIEW_NONE, 1, false, false, nullptr);
        }
    } callbacks;
    std::vector<PromptLora> parsed;
    extract_loras(request.positive_prompt, request.lora_directory, parsed);
    extract_loras(request.negative_prompt, request.lora_directory, parsed);
    for (auto &lora : parsed) {
        auto path = std::filesystem::u8path(lora.path);
        if (path.is_relative())
            path = std::filesystem::u8path(request.lora_directory) / path;
        auto u8 = path.u8string();
        lora.path.assign(reinterpret_cast<const char *>(u8.c_str()));
    }
    std::vector<sd_lora_t> loras;
    for (const auto &lora : parsed)
        loras.push_back({false, lora.weight, lora.path.c_str()});
    InputImage init(request.init_image, 3), mask(request.mask_image, 1), ref(request.reference_image, 3);
    sd_img_gen_params_t params;
    sd_img_gen_params_init(&params);
    params.prompt = request.positive_prompt.c_str();
    params.negative_prompt = request.negative_prompt.c_str();
    params.width = request.width;
    params.height = request.height;
    params.seed = request.seed;
    params.batch_count = 1;
    params.sample_params.sample_method = str_to_sample_method(request.sampling_method.c_str());
    params.sample_params.scheduler = str_to_scheduler(request.scheduler.c_str());
    if (params.sample_params.sample_method == SAMPLE_METHOD_COUNT ||
        params.sample_params.scheduler == SCHEDULER_COUNT)
        throw std::runtime_error("Unknown sampler or scheduler");
    params.sample_params.sample_steps = request.steps;
    params.sample_params.guidance.txt_cfg = request.cfg_scale;
    params.sample_params.guidance.img_cfg = request.image_cfg;
    params.init_image = init.image;
    params.mask_image = mask.image;
    params.strength = request.strength;
    if (ref.image.data) {
        params.ref_images = &ref.image;
        params.ref_images_count = 1;
    }
    params.loras = loras.data();
    params.lora_count = uint32_t(loras.size());
    params.vae_tiling_params.enabled = request.qwen_image;
    {
        std::scoped_lock lock(context_mutex_);
        if (token.stop_requested())
            throw std::runtime_error("Cancelled");
        sd_cancel_generation(context_, SD_CANCEL_RESET);
    }
    set_status("Generating with embedded CUDA engine...");
    Images images;
    const bool ok = generate_image(context_, &params, &images.data, &images.count);
    if (token.stop_requested())
        throw std::runtime_error("Cancelled");
    if (!ok || images.count != 1)
        throw std::runtime_error("Image generation failed");
    auto result = copy_image(images.data[0]);
    if (!request.img2img.enabled && auto_save_txt_) {
        auto path = executable_directory() / L"output" /
                    (L"generation-" + std::to_wstring(GetTickCount64()) + L".png");
        queue_save({result, path, request.positive_prompt, workflow_id_});
    }
    std::scoped_lock lock(mutex_);
    snapshot_.rgba = std::move(result.pixels);
    snapshot_.width = result.w;
    snapshot_.height = result.h;
    snapshot_.progress = 1;
    snapshot_.running = false;
    snapshot_.finished = true;
    snapshot_.status = "Generation completed";
    ++snapshot_.revision;
}
