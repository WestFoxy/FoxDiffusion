#include "img2img_pipeline.hpp"
#include "image_io.hpp"
#include "parameters.hpp"
#include <cctype>
#include <fstream>
#include <random>
#include <sstream>

namespace {
void check(std::stop_token stop) {
    if (stop.stop_requested())
        throw std::runtime_error("Cancelled");
}
Raster load(const std::filesystem::path &path) {
    Raster image;
    std::string error;
    if (!load_image_rgba(path, image.pixels, image.w, image.h, error))
        throw std::runtime_error("Cannot load input: " + error);
    checked_image_bytes(image.w, image.h);
    return image;
}
bool image_file(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    for (auto &c : ext)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga";
}
std::vector<std::string> split(const std::string &text, char delimiter) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line, delimiter)) {
        auto a = line.find_first_not_of(" \r\t");
        auto b = line.find_last_not_of(" \r\t");
        if (a != std::string::npos)
            out.push_back(line.substr(a, b - a + 1));
    }
    return out;
}
void set_axis(GenerationRequest &r, int axis, const std::string &value) {
    if (!axis)
        return;
    size_t used = 0;
    const double n = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(n))
        throw std::runtime_error("XYZ values must be finite numbers");
    if (axis == 1) {
        if (n < 0 || n > 1)
            throw std::runtime_error("Denoising must be 0..1");
        r.img2img.strength = float(n);
    }
    if (axis == 2) {
        if (n < 1 || n > 30)
            throw std::runtime_error("CFG must be 1..30");
        r.cfg_scale = float(n);
    }
    if (axis == 3) {
        if (n < 1 || n > 150 || std::floor(n) != n)
            throw std::runtime_error("Steps must be integers 1..150");
        r.steps = int(n);
    }
    if (axis == 4) {
        if (n < 0 || n > 4294967295.0 || std::floor(n) != n)
            throw std::runtime_error("Seed must be a 32-bit unsigned integer");
        r.seed = std::uint32_t(n);
    }
}
Raster process(GenerationRequest r, Raster source, Raster mask, const InferImage &infer) {
    const auto &o = r.img2img;
    int w = r.width, h = r.height;
    if (o.scale_by) {
        w = std::max(64, int(source.w * o.scale) / 8 * 8);
        h = std::max(64, int(source.h * o.scale) / 8 * 8);
    }
    if (w < 64 || h < 64 || w > 4096 || h > 4096)
        throw std::runtime_error("Generation dimensions must be 64..4096");
    if(r.qwen_image) { w=std::max(64,w/32*32); h=std::max(64,h/32*32); }
    r.width = w;
    r.height = h;
    r.strength = o.strength;
    r.image_cfg = o.image_cfg;
    const bool masked = !mask.empty();
    Raster base;
    Rect rect;
    if (masked) {
        if (mask.w != source.w || mask.h != source.h)
            throw std::runtime_error("Mask dimensions must match the source image");
        for (size_t i = 0; i < mask.pixels.size(); i += 4) {
            int a = (int(mask.pixels[i]) + mask.pixels[i + 1] + mask.pixels[i + 2]) / 3;
            if (o.invert_mask)
                a = 255 - a;
            mask.pixels[i] = mask.pixels[i + 1] = mask.pixels[i + 2] = std::uint8_t(a);
        }
        mask_bounds(mask, 0, 1); // reject empty masks, including after inversion
        // Feather outward without weakening the selected interior. Blurring
        // thin opaque strokes alone used to blend the painted source back in.
        auto feathered = blur_mask(mask, o.mask_blur);
        for (size_t i = 0; i < mask.pixels.size(); i += 4) {
            const auto coverage = std::max(mask.pixels[i], feathered.pixels[i]);
            mask.pixels[i] = mask.pixels[i + 1] = mask.pixels[i + 2] = coverage;
        }
        if (o.only_masked) {
            base = source;
            rect = mask_bounds(mask, o.padding, float(w) / h);
            source = crop_raster(source, rect);
            mask = crop_raster(mask, rect);
        }
    }
    Raster composite_mask;
    if (masked && o.only_masked)
        composite_mask = mask;
    source = resize_raster(source, w, h, o.resize_mode);
    if (masked)
        mask = resize_raster(mask, w, h, o.resize_mode);
    if (base.empty()) {
        base = source;
        rect = {0, 0, w, h};
    }
    Raster prepared = source;
    if (masked && o.fill != 1) {
        std::mt19937 rng(r.seed);
        float average[3]{};
        double weight = 0;
        for (size_t i = 0; i < source.pixels.size(); i += 4) {
            const float a = 1 - mask.pixels[i] / 255.f;
            weight += a;
            for (int c = 0; c < 3; ++c)
                average[c] += source.pixels[i + c] * a;
        }
        for (auto &v : average)
            v = weight ? v / float(weight) : 127;
        Raster filled(w, h);
        for (size_t i = 0; i < filled.pixels.size(); i += 4)
            for (int c = 0; c < 3; ++c)
                filled.pixels[i + c] = o.fill == 0   ? std::uint8_t(average[c])
                                       : o.fill == 2 ? std::uint8_t(rng() % 256)
                                                     : 127;
        composite_raster(prepared, filled, mask, {0, 0, w, h});
    }
    if (o.strength == 0)
        return base;
    if(r.qwen_image) r.reference_image=std::move(prepared);
    else { r.init_image = std::move(prepared); r.mask_image = mask; }
    // The engine expects white for the region to regenerate.
    Raster result = infer(std::move(r));
    if (result.empty())
        throw std::runtime_error("Backend returned no image");
    if (masked) {
        composite_raster(base, result, composite_mask.empty() ? mask : composite_mask, rect);
        return base;
    }
    return result;
}
} // namespace

void execute_img2img(GenerationRequest r, std::stop_token stop, const InferImage &infer,
                     const PublishImage &publish) {
    auto &o = r.img2img;
    if (o.batch_count < 1 || o.batch_count > 32 || o.batch_size < 1 || o.batch_size > 8)
        throw std::runtime_error("Batch limit is 32 x 8 images");
    if (o.mode < 0 || o.mode > 5 || o.script < 0 || o.script > 6 || o.resize_mode < 0 || o.resize_mode > 2 ||
        o.fill < 0 || o.fill > 3)
        throw std::runtime_error("Invalid img2img mode");
    auto range = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!range(o.strength, 0, 1) || !range(o.final_strength, 0, 1) || !range(o.scale, .25f, 4) ||
        !range(o.upscale, 1, 4) || !range(o.image_cfg, 0, 10))
        throw std::runtime_error("Invalid strength or scale");
    if (o.loops < 1 || o.loops > 32 || o.mask_blur < 0 || o.mask_blur > 64 || o.padding < 0 ||
        o.padding > 256)
        throw std::runtime_error("Invalid loop or mask settings");
    if (r.width < 64 || r.width > 4096 || r.height < 64 || r.height > 4096)
        throw std::runtime_error("Width and height must be 64..4096");
    r.width = r.width / 8 * 8;
    r.height = r.height / 8 * 8;
    if (o.script == 1 && (o.overlap < 0 || o.overlap >= std::min(r.width, r.height)))
        throw std::runtime_error("Overlap must be smaller than the tile dimensions");
    for (int i = 0; i < 3; ++i) {
        if (o.axis[i] < 0 || o.axis[i] > 4)
            throw std::runtime_error("Invalid plot axis");
        for (int j = 0; j < i; ++j)
            if (o.script == 4 && o.axis[i] && o.axis[i] == o.axis[j])
                throw std::runtime_error("Choose different parameters for each plot axis");
    }
    std::vector<std::filesystem::path> inputs;
    if (o.mode == 5) {
        if (o.input_dir.empty())
            throw std::runtime_error("Choose a batch input directory");
        for (const auto &e : std::filesystem::directory_iterator(std::filesystem::u8path(o.input_dir)))
            if (e.is_regular_file() && image_file(e.path()))
                inputs.push_back(e.path());
        std::sort(inputs.begin(), inputs.end());
        if (inputs.empty())
            throw std::runtime_error("No supported images in the batch directory");
    } else {
        if (o.source.empty())
            throw std::runtime_error("Load or draw a source image first");
        inputs.emplace_back();
    }
    std::vector<std::string> axes[3];
    for (int i = 0; i < 3; ++i) {
        axes[i] = o.script == 4 && o.axis[i] ? split(o.values[i], ',') : std::vector<std::string>{"0"};
        if (axes[i].empty())
            throw std::runtime_error("An XYZ axis has no values");
    }
    auto prompts = o.script == 5 ? split(o.prompts, '\n') : std::vector<std::string>{r.positive_prompt};
    if (o.script == 6) {
        const auto parts = split(o.prompts, '|');
        if (parts.empty() || parts.size() > 9)
            throw std::runtime_error("Prompt matrix needs a base and at most eight optional parts");
        prompts.clear();
        for (size_t bits = 0; bits < (size_t(1) << (parts.size() - 1)); ++bits) {
            std::string p = parts[0];
            for (size_t i = 1; i < parts.size(); ++i)
                if (bits & (size_t(1) << (i - 1)))
                    p += ", " + parts[i];
            prompts.push_back(p);
        }
    }
    if (prompts.empty())
        throw std::runtime_error("Enter one prompt per line");
    const size_t samples = inputs.size() * o.batch_count * o.batch_size * axes[0].size() * axes[1].size() *
                           axes[2].size() * prompts.size() * (o.script == 2 ? o.loops : 1);
    const size_t count =
        samples +
        (o.script == 4 && o.save_grid ? inputs.size() * o.batch_count * o.batch_size * axes[2].size() : 0);
    if (count == 0 || count > 1024)
        throw std::runtime_error("This request exceeds the limit of 1024 outputs");
    Raster initial_source = std::move(o.source), initial_mask = std::move(o.mask);
    // Validate the whole grid before starting expensive inference.
    for (int i = 0; i < 3; ++i)
        for (const auto &v : axes[i]) {
            auto trial = r;
            if (o.script == 4)
                set_axis(trial, o.axis[i], v);
        }
    int index = 0;
    Raster grid;
    int grid_cell = 0;
    const char *axis_names[] = {"None", "Denoise", "CFG", "Steps", "Seed"};
    for (const auto &path : inputs) {
        check(stop);
        Raster source = path.empty() ? initial_source : load(path), mask;
        if (o.mode >= 2 && o.mode <= 4) {
            mask = initial_mask;
            if (mask.empty())
                throw std::runtime_error("Load or paint a mask first");
        }
        if (o.mode == 5 && !o.mask_dir.empty()) {
            for (const auto &e : std::filesystem::directory_iterator(std::filesystem::u8path(o.mask_dir)))
                if (e.is_regular_file() && e.path().stem() == path.stem() && image_file(e.path())) {
                    mask = load(e.path());
                    break;
                }
            if (mask.empty())
                throw std::runtime_error("Missing batch mask: " + path.stem().string());
        }
        for (int batch = 0; batch < o.batch_count * o.batch_size; ++batch)
            for (const auto &z : axes[2])
                for (const auto &y : axes[1])
                    for (const auto &x : axes[0])
                        for (const auto &prompt : prompts) {
                            check(stop);
                            auto job = r;
                            job.seed += std::uint32_t(o.script == 4 || o.script == 6 ? batch : index);
                            job.positive_prompt = prompt;
                            if (o.mode == 5 && o.batch_prompt_files) {
                                auto text = path;
                                text.replace_extension(".txt");
                                std::ifstream in(text);
                                if (in)
                                    job.positive_prompt = std::string(std::istreambuf_iterator<char>(in), {});
                            }
                            if (o.mode == 5 && o.batch_png_info)
                                apply_parameters(job, read_png_parameters(path));
                            if (o.script == 4) {
                                set_axis(job, o.axis[0], x);
                                set_axis(job, o.axis[1], y);
                                set_axis(job, o.axis[2], z);
                            }
                            Raster result;
                            if (o.script == 1) {
                                if (!mask.empty())
                                    throw std::runtime_error(
                                        "SD upscale uses an unmasked image; select img2img or Sketch");
                                const double scaled_w = double(source.w) * o.upscale,
                                             scaled_h = double(source.h) * o.upscale;
                                if (scaled_w > INT_MAX || scaled_h > INT_MAX)
                                    throw std::runtime_error(
                                        "Requested upscale dimensions overflow the image codec");
                                const int w = int(scaled_w), h = int(scaled_h);
                                require_image_memory(checked_image_bytes(w, h) * 10);
                                const int tw = std::min(w, r.width), th = std::min(h, r.height);
                                auto xs = tile_positions(w, tw, o.overlap),
                                     ys = tile_positions(h, th, o.overlap);
                                Raster enlarged;
                                if (!o.upscaler_model.empty()) {
                                    auto upscalejob = job;
                                    upscalejob.upscale_only = true;
                                    upscalejob.init_image = source;
                                    upscalejob.width = w;
                                    upscalejob.height = h;
                                    enlarged = infer(std::move(upscalejob));
                                    if (enlarged.w != w || enlarged.h != h)
                                        throw std::runtime_error("Upscaler returned unexpected dimensions");
                                } else
                                    enlarged = resize_raster(source, w, h);
                                std::vector<float> sum(size_t(w) * h * 3, 0), weights(size_t(w) * h, 0);
                                int upscale_tile_index = 0;
                                for (int ty : ys)
                                    for (int tx : xs) {
                                        check(stop);
                                        auto tilejob = job;
                                        tilejob.output_label = "SD upscale tile " +
                                                               std::to_string(++upscale_tile_index) + " / " +
                                                               std::to_string(xs.size() * ys.size());
                                        tilejob.img2img.scale_by = false;
                                        auto tile = process(tilejob, crop_raster(enlarged, {tx, ty, tw, th}),
                                                            {}, infer);
                                        tile = resize_raster(tile, tw, th);
                                        for (int py = 0; py < th; ++py)
                                            for (int px = 0; px < tw; ++px) {
                                                float a = 1.f;
                                                const float feather = float(std::max(1, o.overlap));
                                                if (tx > 0)
                                                    a *= std::min(1.f, (px + 1) / feather);
                                                if (tx + tw < w)
                                                    a *= std::min(1.f, (tw - px) / feather);
                                                if (ty > 0)
                                                    a *= std::min(1.f, (py + 1) / feather);
                                                if (ty + th < h)
                                                    a *= std::min(1.f, (th - py) / feather);
                                                size_t p = size_t(ty + py) * w + tx + px;
                                                weights[p] += a;
                                                for (int c = 0; c < 3; ++c)
                                                    sum[p * 3 + c] += tile.at(px, py)[c] * a;
                                            }
                                    }
                                result = Raster(w, h);
                                for (size_t p = 0; p < weights.size(); ++p)
                                    for (int c = 0; c < 3; ++c)
                                        result.pixels[p * 4 + c] =
                                            std::uint8_t(std::clamp(sum[p * 3 + c] / weights[p], 0.f, 255.f));
                            } else if (o.script == 3) {
                                const int l = o.expand[0], t = o.expand[1], rr = o.expand[2], b = o.expand[3];
                                if (l < 0 || t < 0 || rr < 0 || b < 0 || l + t + rr + b == 0)
                                    throw std::runtime_error("Set a positive outpaint extension");
                                Raster canvas(source.w + l + rr, source.h + t + b), area(canvas.w, canvas.h);
                                for (int py = 0; py < canvas.h; ++py)
                                    for (int px = 0; px < canvas.w; ++px) {
                                        std::copy_n(source.at(std::clamp(px - l, 0, source.w - 1),
                                                              std::clamp(py - t, 0, source.h - 1)),
                                                    4, canvas.at(px, py));
                                        if (px >= l && px < l + source.w && py >= t && py < t + source.h)
                                            area.at(px, py)[0] = area.at(px, py)[1] = area.at(px, py)[2] = 0;
                                    }
                                job.img2img.only_masked = true;
                                job.img2img.invert_mask = false;
                                result = process(job, canvas, area, infer);
                            } else if (o.script == 2) {
                                Raster loop = source, loop_mask = mask;
                                for (int n = 0; n < o.loops; ++n) {
                                    check(stop);
                                    auto iteration = job;
                                    iteration.seed += n;
                                    iteration.img2img.strength =
                                        o.loops > 1
                                            ? o.strength + (o.final_strength - o.strength) * n / (o.loops - 1)
                                            : o.strength;
                                    loop = process(iteration, loop, loop_mask, infer);
                                    publish(loop, iteration, ++index, int(count));
                                    if (!loop_mask.empty() &&
                                        (loop_mask.w != loop.w || loop_mask.h != loop.h))
                                        loop_mask = resize_raster(loop_mask, loop.w, loop.h, o.resize_mode);
                                }
                                continue;
                            } else
                                result = process(job, source, mask, infer);
                            check(stop);
                            publish(result, job, ++index, int(count));
                            if (o.script == 4 && o.save_grid) {
                                const int columns = int(axes[0].size()), rows = int(axes[1].size());
                                const int cell = std::min(384, 4096 / std::max(columns, rows)),
                                          label_height = 40;
                                if (cell < 32)
                                    throw std::runtime_error("Grid has too many rows or columns");
                                if (grid_cell == 0)
                                    grid = Raster(columns * cell, rows * (cell + label_height), 0);
                                auto thumb = resize_raster(result, cell, cell, 2);
                                int gx = grid_cell % columns * cell,
                                    gy = grid_cell / columns * (cell + label_height);
                                std::string label = std::string(axis_names[o.axis[0]]) + " " + x + " | " +
                                                    axis_names[o.axis[1]] + " " + y;
                                if (o.axis[2])
                                    label += " | " + std::string(axis_names[o.axis[2]]) + " " + z;
                                raster_label(grid, label, gx, gy, cell, label_height);
                                for (int py = 0; py < cell; ++py)
                                    std::copy_n(thumb.at(0, py), size_t(cell) * 4,
                                                grid.at(gx, gy + label_height + py));
                                if (++grid_cell == columns * rows) {
                                    auto gridjob = job;
                                    gridjob.output_label = "XYZ grid";
                                    publish(grid, gridjob, ++index, int(count));
                                    grid_cell = 0;
                                }
                            }
                        }
    }
}
