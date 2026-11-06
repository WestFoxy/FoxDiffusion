#include "backend_process.hpp"
#include "image_io.hpp"
#include "img2img_pipeline.hpp"
#include "native_upscale.hpp"

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace {
std::wstring quote(const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; }

std::filesystem::path executable_directory() {
    wchar_t value[MAX_PATH]{};
    GetModuleFileNameW(nullptr, value, MAX_PATH);
    return std::filesystem::path(value).parent_path();
}

std::filesystem::path find_engine(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) return {};
    for (const auto& directory : {root / L"cuda-bin", root / L"backend", root}) {
        const auto server = directory / L"sd-server.exe";
        if (std::filesystem::is_regular_file(server, error)) return server;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, error)) {
        if (!error && entry.path().filename() == L"sd-server.exe") return entry.path();
        error.clear();
    }
    return {};
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string json_escape(const std::string& text) {
    std::ostringstream out;
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
            else out << static_cast<char>(c);
        }
    }
    return out.str();
}

struct PromptLora { std::string path; float weight = 1.0f; };

std::string resolve_lora_path(const std::string& directory, const std::string& name) {
    if (directory.empty()) return name;
    const auto root = std::filesystem::u8path(directory);
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) { error.clear(); continue; }
        if (!entry.is_regular_file(error) || entry.path().stem().u8string() != std::u8string(
                reinterpret_cast<const char8_t*>(name.data()), name.size())) continue;
        const auto relative = std::filesystem::relative(entry.path(), root, error).generic_u8string();
        if (!error) return {reinterpret_cast<const char*>(relative.data()), relative.size()};
    }
    return name;
}

void extract_loras(std::string& prompt, const std::string& directory, std::vector<PromptLora>& loras) {
    static const std::regex pattern(R"(<lora:([^:>]+):([-+]?(?:\d+(?:\.\d*)?|\.\d+))>)",
                                    std::regex::icase);
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

bool http_request(unsigned short port, const wchar_t* method, const std::wstring& path,
                  const std::string& body, std::string& response, DWORD& status) {
    HINTERNET session = WinHttpOpen(L"FoxDiffusion/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 2000, 2000, 5000, 5000);
    HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", port, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, method, path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    bool ok = false;
    if (request) {
        const wchar_t* headers = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
        const DWORD header_length = body.empty() ? 0 : static_cast<DWORD>(-1L);
        if (WinHttpSendRequest(request, headers, header_length,
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            response.clear();
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                const std::size_t offset = response.size();
                response.resize(offset + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, response.data() + offset, available, &read)) break;
                response.resize(offset + read);
            }
            ok = true;
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}

std::string json_string(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    std::size_t pos = json.find(marker);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    std::string value;
    for (++pos; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (c == '"') return value;
        if (c == '\\' && pos + 1 < json.size()) {
            const char escaped = json[++pos];
            if (escaped == 'n') value += '\n';
            else if (escaped == 'r') value += '\r';
            else if (escaped == 't') value += '\t';
            else value += escaped;
        } else value += c;
    }
    return {};
}

bool decode_base64(const std::string& encoded, std::vector<std::uint8_t>& bytes) {
    DWORD size = 0;
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) return false;
    bytes.resize(size);
    return CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                                CRYPT_STRING_BASE64, bytes.data(), &size, nullptr, nullptr) != FALSE;
}

std::string encode_image(const Raster& image) {
    const auto png=encode_png(image.pixels,image.w,image.h);
    if(png.empty())throw std::runtime_error("Could not encode the input image");
    DWORD size=0;
    if(!CryptBinaryToStringA(png.data(),DWORD(png.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&size))throw std::runtime_error("Could not encode base64");
    std::string out(size,'\0');
    CryptBinaryToStringA(png.data(),DWORD(png.size()),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,out.data(),&size);
    out.resize(size);while(!out.empty()&&out.back()=='\0')out.pop_back();return out;
}

bool inject_library(HANDLE process, const std::filesystem::path& library) {
    const std::wstring path = library.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    bool ok = WriteProcessMemory(process, remote, path.c_str(), bytes, nullptr) != FALSE;
    HANDLE thread = nullptr;
    if (ok) {
        auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
        ok = thread != nullptr;
    }
    if (thread) { WaitForSingleObject(thread, 10000); CloseHandle(thread); }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}
}

BackendProcess::BackendProcess() {
    server_port_ = static_cast<unsigned short>(39000 + GetCurrentProcessId() % 1000);
    if (!find_engine(executable_directory()).empty()) backend_status_ = "CUDA backend ready";
    save_worker_ = std::jthread([this] { save_outputs(); });
}
BackendProcess::~BackendProcess() {
    stop();
    { std::scoped_lock lock(mutex_); close_saver_ = true; }
    save_cv_.notify_all();
    if (save_worker_.joinable()) save_worker_.join();
}

void BackendProcess::queue_save(SaveJob job) {
    std::unique_lock lock(mutex_);
    // Bound queued pixels; only the generation worker waits if disk falls behind.
    save_cv_.wait(lock, [&] { return save_queue_.empty() || queued_save_bytes_ + job.image.pixels.size() <= 256ULL * 1024 * 1024; });
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
            if (save_queue_.empty()) return;
            job = std::move(save_queue_.front()); save_queue_.pop_front();
            queued_save_bytes_ -= job.image.pixels.size();
        }
        save_cv_.notify_all();
        std::string error;
        bool image_saved = false;
        try {
            std::filesystem::create_directories(job.path.parent_path());
            image_saved = save_png_parameters(job.path, job.image.pixels, job.image.w, job.image.h, job.metadata, error);
            if (image_saved) {
                auto info = job.path; info.replace_extension(L".txt");
                std::ofstream sidecar(info, std::ios::binary);
                sidecar << job.metadata; sidecar.close();
                if (!sidecar) error = "PNG saved, but could not save generation metadata sidecar";
            } else if (error.empty()) error = "Could not save PNG";
        } catch (const std::exception& e) { error = e.what(); }
        {
            const auto saved_path = job.path.u8string();
            std::scoped_lock lock(mutex_);
            --pending_saves_;
            if (!error.empty()) save_error_ = std::string(reinterpret_cast<const char*>(saved_path.data()), saved_path.size()) + ": " + error;
            if (image_saved && job.workflow == workflow_id_) {
                const auto utf8 = job.path.u8string();
                snapshot_.outputs.emplace_back(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            }
        }
    }
}

void BackendProcess::start(GenerationRequest request) {
    if(workflow_running_)return;
    if (worker_.joinable()) worker_.join();
    cancel_requested_ = false;
    {
        std::scoped_lock lock(mutex_);
        ++workflow_id_;
        const auto revision=snapshot_.revision;
        snapshot_ = {};
        snapshot_.revision=revision+1;
        snapshot_.running = true;
        snapshot_.total_steps = request.steps;
        snapshot_.status = "Preparing persistent backend...";
    }
    workflow_running_=true;
    worker_ = std::jthread([this, request = std::move(request)](std::stop_token token) mutable {
        try { run(std::move(request), token); }
        catch(const std::exception& e){set_status(e.what());std::scoped_lock lock(mutex_);snapshot_.running=false;snapshot_.finished=false;}
        if(token.stop_requested())stop_server();
        workflow_running_=false;
    });
}

void BackendProcess::cancel() {
    cancel_requested_ = true;
    if (worker_.joinable()) worker_.request_stop();
    if(workflow_running_)set_status("Cancelling...");
}
void BackendProcess::stop() { cancel();if(worker_.joinable())worker_.join();stop_server(); }

bool BackendProcess::ready() const { return !find_engine(executable_directory()).empty(); }
std::string BackendProcess::status() const { std::scoped_lock lock(mutex_); return backend_status_; }
GenerationSnapshot BackendProcess::snapshot() const { std::scoped_lock lock(mutex_); auto value=snapshot_;value.running=workflow_running_;value.pending_saves=pending_saves_;value.save_error=save_error_;return value; }

void BackendProcess::poll_snapshot(GenerationSnapshot& cached) const {
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
    cached.total_steps = snapshot_.total_steps;
    cached.status = snapshot_.status;
    cached.job_index = snapshot_.job_index;
    cached.job_count = snapshot_.job_count;
    if (cached.outputs != snapshot_.outputs) cached.outputs = snapshot_.outputs;
}

void BackendProcess::set_status(std::string value) {
    std::scoped_lock lock(mutex_); backend_status_ = value; snapshot_.status = std::move(value);
}

void BackendProcess::run(GenerationRequest request, std::stop_token token) {
    if(!request.img2img.enabled){run_single(std::move(request),token);return;}
    const auto infer=[&](GenerationRequest job) {
        if(token.stop_requested())throw std::runtime_error("Cancelled");
        if(job.upscale_only)return run_upscale(job,token);
        {std::scoped_lock lock(mutex_);snapshot_.finished=false;snapshot_.progress=0;snapshot_.running=true;snapshot_.current_step=0;snapshot_.total_steps=job.steps;}
        run_single(std::move(job),token);
        std::scoped_lock lock(mutex_);
        if(token.stop_requested())throw std::runtime_error("Cancelled");
        if(!snapshot_.finished)throw std::runtime_error(snapshot_.status);
        Raster result;result.w=snapshot_.width;result.h=snapshot_.height;result.pixels=snapshot_.rgba;return result;
    };
    const auto publish=[&](const Raster& image,const GenerationRequest& job,int index,int total){
        auto dir=job.img2img.output_dir.empty()?executable_directory()/L"output":std::filesystem::u8path(job.img2img.output_dir);
        auto path=dir/(L"img2img-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(index)+L".png");
        std::ostringstream metadata;
        metadata<<job.positive_prompt<<"\nNegative prompt: "<<job.negative_prompt<<"\nSteps: "<<job.steps
            <<", Sampler: "<<job.sampling_method<<", Schedule type: "<<job.scheduler<<", CFG scale: "<<job.cfg_scale
            <<", Seed: "<<job.seed<<", Size: "<<image.w<<'x'<<image.h<<", Denoising strength: "<<job.img2img.strength
            <<", Model: "<<job.model_path<<", Script: "<<job.img2img.script<<", Output: "<<job.output_label<<"\n";
        {
            std::scoped_lock lock(mutex_);
            snapshot_.rgba=image.pixels;snapshot_.width=image.w;snapshot_.height=image.h;
            snapshot_.job_index=index;snapshot_.job_count=total;snapshot_.progress=float(index)/total;
            snapshot_.status=auto_save_img_ ? "Result ready; saving in background" : "Result ready (auto-save off)";++snapshot_.revision;
        }
        if (auto_save_img_) queue_save({image, path, metadata.str(), workflow_id_});
    };
    execute_img2img(std::move(request),token,infer,publish);
    std::scoped_lock lock(mutex_);snapshot_.running=false;snapshot_.finished=true;snapshot_.progress=1;snapshot_.status="Img2img completed";
}

Raster BackendProcess::run_upscale(const GenerationRequest& request,std::stop_token token) {
    stop_server();
    const auto engine=ensure_backend(token);
    if(engine.empty())throw std::runtime_error("CUDA backend not found");
    const auto model=std::filesystem::u8path(request.img2img.upscaler_model);
    if(!std::filesystem::is_regular_file(model))throw std::runtime_error("Select an existing ESRGAN model");
    set_status("Loading ESRGAN model...");
    return native_tiled_upscale(request.init_image,request.width,request.height,model,engine.parent_path(),token,
        [&](const Raster& preview,int done,int total) {
            std::scoped_lock lock(mutex_);
            snapshot_.rgba=preview.pixels;snapshot_.width=preview.w;snapshot_.height=preview.h;
            snapshot_.current_step=done;snapshot_.total_steps=total;snapshot_.progress=float(done)/total;
            snapshot_.status="ESRGAN tile "+std::to_string(done)+" / "+std::to_string(total)+
                " | target "+std::to_string(request.width)+" x "+std::to_string(request.height);
            ++snapshot_.revision;
        });
}

void BackendProcess::run_single(GenerationRequest request, std::stop_token token) {
    if (request.model_path.empty()) {
        set_status("Select an SD 1.x or SDXL model first");
        std::scoped_lock lock(mutex_); snapshot_.running = false; return;
    }
    if (!ensure_server(request, token) || token.stop_requested()) {
        std::scoped_lock lock(mutex_); snapshot_.running = false;
        if (token.stop_requested()) snapshot_.status = "Cancelled";
        return;
    }

    std::vector<PromptLora> loras;
    extract_loras(request.positive_prompt, request.lora_directory, loras);
    extract_loras(request.negative_prompt, request.lora_directory, loras);
    std::ostringstream lora_json;
    lora_json << '[';
    for (std::size_t i = 0; i < loras.size(); ++i) {
        if (i) lora_json << ',';
        lora_json << "{\"path\":\"" << json_escape(loras[i].path) << "\",\"multiplier\":"
                  << std::setprecision(9) << loras[i].weight << ",\"is_high_noise\":false}";
    }
    lora_json << ']';

    std::ostringstream json;
    json << std::setprecision(9)
         << "{\"prompt\":\"" << json_escape(request.positive_prompt)
         << "\",\"negative_prompt\":\"" << json_escape(request.negative_prompt)
         << "\",\"width\":" << request.width << ",\"height\":" << request.height
         << ",\"seed\":" << request.seed << ",\"batch_count\":1,\"sample_params\":{"
         << "\"scheduler\":\"" << json_escape(request.scheduler)
         << "\",\"sample_method\":\"" << json_escape(request.sampling_method)
         << "\",\"sample_steps\":" << request.steps
         << ",\"guidance\":{\"txt_cfg\":" << request.cfg_scale << ",\"img_cfg\":" << request.image_cfg << "}},\"lora\":" << lora_json.str() << ',';
    if(!request.init_image.empty())json<<"\"init_image\":\""<<encode_image(request.init_image)<<"\",\"strength\":"<<request.strength<<',';
    if(!request.reference_image.empty())json<<"\"ref_images\":[\""<<encode_image(request.reference_image)<<"\"],";
    if(!request.mask_image.empty())json<<"\"mask_image\":\""<<encode_image(request.mask_image)<<"\",";
    json
         << "\"output_format\":\"png\",\"output_compression\":100}";

    set_status(request.output_label.empty() ? "Generating (model stays loaded)..." : request.output_label + " | Generating...");
    std::string response; DWORD status = 0;
    if (!http_request(server_port_, L"POST", L"/sdcpp/v1/img_gen", json.str(), response, status) || status != 202) {
        set_status("Could not submit generation job (HTTP " + std::to_string(status) + ")");
        std::scoped_lock lock(mutex_); snapshot_.running = false; return;
    }
    const std::string job_id = json_string(response, "id");
    if (job_id.empty()) {
        set_status("Backend returned an invalid job id");
        std::scoped_lock lock(mutex_); snapshot_.running = false; return;
    }

    std::string last_preview;
    std::filesystem::file_time_type preview_time{};
    int failures=0;
    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        response.clear(); status = 0;
        const std::wstring path = L"/sdcpp/v1/jobs/" + utf8_to_wide(job_id);
        if (!http_request(server_port_, L"GET", path, {}, response, status) || status != 200) {
            if(++failures>=12){set_status("Backend connection lost");std::scoped_lock lock(mutex_);snapshot_.running=false;return;}
            continue;
        }
        failures=0;
        const std::string state = json_string(response, "status");
        if (!preview_path_.empty()) {
            std::error_code time_error;
            const auto changed = std::filesystem::last_write_time(preview_path_, time_error);
            if (!time_error && changed != preview_time) {
                std::vector<std::uint8_t> pixels; int width = 0, height = 0; std::string error;
                if (load_image_rgba(preview_path_, pixels, width, height, error)) {
                    preview_time = changed;
                    std::scoped_lock lock(mutex_);
                    snapshot_.rgba = std::move(pixels); snapshot_.width = width; snapshot_.height = height;
                    ++snapshot_.revision;
                }
            }
        }
        const std::string preview_encoded = json_string(response, "preview_b64");
        if (!preview_encoded.empty() && preview_encoded != last_preview) {
            last_preview = preview_encoded;
            std::vector<std::uint8_t> preview_bytes;
            std::vector<std::uint8_t> preview_pixels;
            int preview_width = 0, preview_height = 0;
            std::string preview_error;
            if (decode_base64(preview_encoded, preview_bytes) &&
                load_image_rgba_memory(preview_bytes, preview_pixels, preview_width, preview_height, preview_error)) {
                std::scoped_lock lock(mutex_);
                snapshot_.rgba = std::move(preview_pixels);
                snapshot_.width = preview_width;
                snapshot_.height = preview_height;
                ++snapshot_.revision;
            }
        }
        if (state == "queued") { set_status("Generation queued..."); continue; }
        if (state == "generating") { set_status("Generating (model stays loaded)..."); continue; }
        if (state == "failed" || state == "cancelled") {
            std::string message = json_string(response, "message");
            set_status(message.empty() ? "Generation failed" : message);
            std::scoped_lock lock(mutex_); snapshot_.running = false; return;
        }
        if (state != "completed") continue;

        const std::string encoded = json_string(response, "b64_json");
        std::vector<std::uint8_t> png;
        if (encoded.empty() || !decode_base64(encoded, png)) {
            set_status("Backend returned an invalid image");
            std::scoped_lock lock(mutex_); snapshot_.running = false; return;
        }
        std::vector<std::uint8_t> pixels; int width = 0, height = 0; std::string error;
        if (!load_image_rgba_memory(png, pixels, width, height, error)) {
            set_status("Output error: " + error);
        } else {
            if(!request.img2img.enabled && auto_save_txt_){
                const auto output_dir=executable_directory()/L"output";std::filesystem::create_directories(output_dir);
                const auto output=output_dir/(L"generation-"+std::to_wstring(GetTickCount64())+L".png");
                if(!save_image(output,pixels,width,height,95,error))throw std::runtime_error(error);
            }
            std::scoped_lock lock(mutex_);
            snapshot_.rgba = std::move(pixels); snapshot_.width = width; snapshot_.height = height;
            snapshot_.progress = 1.0f; snapshot_.current_step = snapshot_.total_steps;
            snapshot_.running = false; snapshot_.finished = true; snapshot_.status = "Generation completed";
            backend_status_ = "CUDA model loaded (persistent)"; ++snapshot_.revision;
        }
        return;
    }
    std::scoped_lock lock(mutex_); snapshot_.running = false; snapshot_.status = "Cancelled";
}

std::filesystem::path BackendProcess::ensure_backend(std::stop_token token) {
    (void)token;
    auto engine = find_engine(executable_directory());
    set_status(engine.empty() ? "Put sd-server.exe and its DLLs in cuda-bin next to the app" : "CUDA backend ready");
    return engine;
}

bool BackendProcess::ensure_server(const GenerationRequest& request, std::stop_token token) {
    const auto engine = ensure_backend(token);
    if (engine.empty()) return false;
    if (request.qwen_image) {
        for (const auto& path : {request.vae_path,request.llm_path})
            if(path.empty() || !std::filesystem::is_regular_file(std::filesystem::u8path(path)))
                throw std::runtime_error("Qwen requires its 2.1 VAE and Qwen3-VL encoder: Models > Model loading");
        if(request.img2img.enabled && std::filesystem::u8path(request.llm_path).extension()==".gguf" && (request.vision_path.empty() || !std::filesystem::is_regular_file(std::filesystem::u8path(request.vision_path))))
            throw std::runtime_error("Qwen image editing requires the Qwen3-VL vision projector: Models > Model loading");
    }
    const std::string key = request.vae_path + request.llm_path + request.vision_path + std::to_string(request.qwen_image) + std::to_string(request.offload_to_cpu) + request.model_path + "\n" + request.lora_directory + "\n" + request.embedding_directory +
        "\n" + std::to_string(request.preview_mode) + ":" + std::to_string(request.preview_interval) + ":" +
        std::to_string(request.preview_denoised) + ":" + std::to_string(request.preview_noisy);
    {
        std::scoped_lock lock(mutex_);
        if (server_process_ && server_key_ == key && WaitForSingleObject(server_process_, 0) == WAIT_TIMEOUT)
            return true;
    }
    stop_server();
    set_status("Loading CUDA runtime and model (one time)...");

    preview_path_ = executable_directory() / L"output" / L"live-preview.bmp";
    std::filesystem::create_directories(preview_path_.parent_path());
    std::error_code preview_error;
    std::filesystem::remove(preview_path_, preview_error);
    if (request.preview_mode > 0) {
        const auto utf8 = preview_path_.u8string();
        const auto config_directory = executable_directory() / L"config";
        std::filesystem::create_directories(config_directory);
        std::ofstream config(config_directory / L"preview-bridge.cfg", std::ios::trunc);
        config << std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()) << '\n'
               << request.preview_mode << ' ' << request.preview_interval << ' '
               << request.preview_denoised << ' ' << request.preview_noisy << '\n';
        if (!config) throw std::runtime_error("Could not write preview settings");
    }

    std::wstring command = quote(engine) + (request.qwen_image ? L" --diffusion-model " : L" --model ") + quote(std::filesystem::u8path(request.model_path)) +
        L" --listen-ip 127.0.0.1 --listen-port " + std::to_wstring(server_port_) +
        L" --diffusion-fa --log-level info";
    if (request.qwen_image) {
        command += L" --vae " + quote(std::filesystem::u8path(request.vae_path)) + L" --llm " + quote(std::filesystem::u8path(request.llm_path));
        if(!request.vision_path.empty()) command += L" --llm_vision " + quote(std::filesystem::u8path(request.vision_path));
        if(request.offload_to_cpu) command += L" --offload-to-cpu";
        command += L" --vae-tiling";
    }
    if (!request.lora_directory.empty())
        command += L" --lora-model-dir " + quote(std::filesystem::u8path(request.lora_directory));
    if (!request.embedding_directory.empty())
        command += L" --embd-dir " + quote(std::filesystem::u8path(request.embedding_directory));

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE output_read = nullptr, output_write = nullptr;
    if (!CreatePipe(&output_read, &output_write, &security, 0)) {
        set_status("Could not create backend output pipe");
        return false;
    }
    SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output_write; startup.hStdError = output_write; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end()); mutable_command.push_back(0);
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, engine.parent_path().c_str(), &startup, &info);
    CloseHandle(output_write);
    if (!created) { CloseHandle(output_read); set_status("Could not start sd-server.exe"); return false; }
    CloseHandle(info.hThread);
    {
        std::scoped_lock lock(mutex_);
        server_process_ = info.hProcess; server_output_read_ = output_read; server_key_ = key;
    }
    server_output_worker_ = std::jthread([this](std::stop_token output_token) { read_server_output(output_token); });

    for (int attempt = 0; attempt < (request.qwen_image ? 3600 : 600) && !token.stop_requested(); ++attempt) {
        std::string response; DWORD status = 0;
        if (http_request(server_port_, L"GET", L"/v1/models", {}, response, status) && status == 200) {
            if (request.preview_mode > 0 &&
                !inject_library(info.hProcess, executable_directory() / L"preview-bridge.dll")) {
                set_status("Model loaded, but preview bridge could not be attached");
                return true;
            }
            set_status("CUDA model loaded (persistent)");
            return true;
        }
        if (WaitForSingleObject(info.hProcess, 0) != WAIT_TIMEOUT) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!token.stop_requested()) set_status("Backend stopped while loading the model");
    stop_server();
    return false;
}

void BackendProcess::stop_server() {
    HANDLE process = nullptr;
    HANDLE output = nullptr;
    {
        std::scoped_lock lock(mutex_);
        process = server_process_; output = server_output_read_;
        server_process_ = nullptr; server_output_read_ = nullptr; server_key_.clear();
    }
    if (process) {
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) TerminateProcess(process, 2);
        WaitForSingleObject(process, 5000);
        CloseHandle(process);
    }
    if (server_output_worker_.joinable()) {
        server_output_worker_.request_stop();
        if (output) CancelIoEx(output, nullptr);
        server_output_worker_.join();
    }
    if (output) CloseHandle(output);
}

void BackendProcess::read_server_output(std::stop_token token) {
    HANDLE output = nullptr;
    {
        std::scoped_lock lock(mutex_); output = server_output_read_;
    }
    if (!output) return;
    const std::regex progress_pattern(R"((\d+)\s*/\s*(\d+))");
    std::string pending;
    char buffer[4096];
    while (!token.stop_requested()) {
        DWORD read = 0;
        if (!ReadFile(output, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
        std::string chunk(buffer, read);
        std::string lower = chunk;
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool before_sampling = false;
        {
            std::scoped_lock lock(mutex_);
            before_sampling = snapshot_.running && snapshot_.current_step == 0;
        }
        if (before_sampling) {
            if (lower.find("loading tensors") != std::string::npos)
                set_status("Loading model tensors into RAM/VRAM...");
            else if (lower.find("ggml_cuda_init") != std::string::npos || lower.find("cuda runtime") != std::string::npos)
                set_status("Initializing CUDA runtime and GPU kernels...");
            else if (lower.find("compute buffer") != std::string::npos || lower.find("allocat") != std::string::npos)
                set_status("Allocating CUDA compute buffers...");
            else if (lower.find("loading model") != std::string::npos || lower.find("load model") != std::string::npos)
                set_status("Reading model weights from disk...");
        }
        pending.append(buffer, read);
        std::smatch match;
        while (std::regex_search(pending, match, progress_pattern)) {
            const int current = std::stoi(match[1].str());
            const int total = std::stoi(match[2].str());
            {
                std::scoped_lock lock(mutex_);
                if (snapshot_.running && total > 0 && total == snapshot_.total_steps && current <= total) {
                    snapshot_.current_step = current;
                    snapshot_.total_steps = total;
                    snapshot_.progress = static_cast<float>(current) / static_cast<float>(total);
                }
            }
            pending.erase(0, static_cast<std::size_t>(match.position() + match.length()));
        }
        if (pending.size() > 16384) pending.erase(0, pending.size() - 8192);
    }
}
