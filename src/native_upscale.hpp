#pragma once
#include "raster.hpp"
#include <stable-diffusion.h>
#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>

// Keep backend modules resident: GGML's registry retains their function pointers.
// Model tensors and compute buffers are released with each upscaler context.
struct NativeUpscalerApi {
    HMODULE module = nullptr;
    decltype(&new_upscaler_ctx) create = nullptr;
    decltype(&free_upscaler_ctx) destroy = nullptr;
    decltype(&upscale) run = nullptr;
    decltype(&get_upscale_factor) factor = nullptr;
    decltype(&free_sd_images) free_images = nullptr;
    explicit NativeUpscalerApi(const std::filesystem::path &directory) {
        // GGML opens CUDA/CPU plugins using LoadLibraryW. Include the added
        // backend directory when Windows resolves their CUDA/OpenMP dependencies.
        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
            throw std::runtime_error("Cannot initialize backend DLL search paths");
        const auto cookie = AddDllDirectory(directory.c_str());
        if (!cookie) throw std::runtime_error("Cannot register backend DLL directory");
        module = LoadLibraryExW((directory / L"stable-diffusion.dll").c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            if (cookie)
                RemoveDllDirectory(cookie);
            throw std::runtime_error("Cannot load native upscaler DLL, Windows error " +
                                     std::to_string(GetLastError()));
        }
        create = reinterpret_cast<decltype(create)>(GetProcAddress(module, "new_upscaler_ctx"));
        destroy = reinterpret_cast<decltype(destroy)>(GetProcAddress(module, "free_upscaler_ctx"));
        run = reinterpret_cast<decltype(run)>(GetProcAddress(module, "upscale"));
        factor = reinterpret_cast<decltype(factor)>(GetProcAddress(module, "get_upscale_factor"));
        free_images = reinterpret_cast<decltype(free_images)>(GetProcAddress(module, "free_sd_images"));
        if (!create || !destroy || !run || !factor || !free_images)
            throw std::runtime_error("Installed backend does not expose the native upscaler API");
        using LoadBackends = void (*)(const char *);
        auto load = reinterpret_cast<LoadBackends>(
            GetProcAddress(GetModuleHandleW(L"ggml.dll"), "ggml_backend_load_all_from_path"));
        if (!load)
            throw std::runtime_error("Cannot locate GGML backend loader");
        const auto utf8 = directory.u8string();
        load(reinterpret_cast<const char *>(utf8.c_str()));
    }
};

using UpscalePreview = std::function<void(const Raster &, int, int)>;
inline Raster native_tiled_upscale(const Raster &source, int width, int height,
                                   const std::filesystem::path &model, const std::filesystem::path &backend,
                                   std::stop_token stop, const UpscalePreview &publish) {
    checked_image_bytes(width, height);
    require_image_memory(checked_image_bytes(width, height) * 2);
    auto check = [&] {
        if (stop.stop_requested())
            throw std::runtime_error("Cancelled");
    };
    check();
    static NativeUpscalerApi api(backend);
    const auto model_utf8 = model.u8string();
    std::unique_ptr<upscaler_ctx_t, decltype(api.destroy)> context(
        api.create(reinterpret_cast<const char *>(model_utf8.c_str()), false,
                   int(std::max(1u, std::thread::hardware_concurrency() / 2)), 128, "CUDA", "CUDA"),
        api.destroy);
    if (!context)
        throw std::runtime_error("Could not load ESRGAN model on CUDA");
    const int factor = api.factor(context.get());
    if (factor < 1 || factor > 16)
        throw std::runtime_error("Unsupported ESRGAN model scale");
    check();
    Raster result(width, height);
    const float preview_scale = std::min(1.f, 1536.f / std::max(width, height));
    Raster preview = resize_raster(source, std::max(1, int(width * preview_scale)),
                                   std::max(1, int(height * preview_scale)));
    constexpr int core = 256, halo = 32;
    const int total = ((source.w + core - 1) / core) * ((source.h + core - 1) / core);
    int completed = 0;
    publish(preview, 0, total);
    for (int sy = 0; sy < source.h; sy += core)
        for (int sx = 0; sx < source.w; sx += core) {
            check();
            const int ex = std::min(source.w, sx + core), ey = std::min(source.h, sy + core);
            const int ix = std::max(0, sx - halo), iy = std::max(0, sy - halo);
            const int iw = std::min(source.w, ex + halo) - ix, ih = std::min(source.h, ey + halo) - iy;
            // Backend consumes RGB. Only this padded tile is converted/upscaled.
            std::vector<uint8_t> rgb(size_t(iw) * ih * 3);
            for (int y = 0; y < ih; ++y)
                for (int x = 0; x < iw; ++x)
                    std::copy_n(source.at(ix + x, iy + y), 3, rgb.data() + (size_t(y) * iw + x) * 3);
            struct Output {
                sd_image_t *images = nullptr;
                int count = 0;
                ~Output() {
                    if (images)
                        api.free_images(images, count);
                }
            } output;
            const sd_image_t input{uint32_t(iw), uint32_t(ih), 3, rgb.data()};
            if (!api.run(context.get(), input, uint32_t(factor), &output.images, &output.count) ||
                output.count != 1 || !output.images[0].data)
                throw std::runtime_error("ESRGAN tile failed; try closing GPU-heavy applications");
            check();
            const auto &tile = output.images[0];
            if (tile.width != uint32_t(iw * factor) || tile.height != uint32_t(ih * factor) ||
                tile.channel < 3)
                throw std::runtime_error("ESRGAN returned unexpected tile dimensions");
            const int x0 = int(int64_t(sx) * width / source.w), x1 = int(int64_t(ex) * width / source.w);
            const int y0 = int(int64_t(sy) * height / source.h), y1 = int(int64_t(ey) * height / source.h);
            const double step_x = double(source.w) * factor / width,
                         step_y = double(source.h) * factor / height;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    if (step_x < 1. || step_y < 1.) {
                        const double fx =
                            std::clamp((x + .5) * step_x - ix * factor - .5, 0., double(tile.width - 1));
                        const double fy =
                            std::clamp((y + .5) * step_y - iy * factor - .5, 0., double(tile.height - 1));
                        const int px = int(fx), py = int(fy);
                        const double dx = fx - px, dy = fy - py;
                        auto sample = [&](int xx, int yy, int c) {
                            return tile.data[(size_t(yy) * tile.width + xx) * tile.channel + c];
                        };
                        auto *dst = result.at(x, y);
                        for (int c = 0; c < 3; ++c)
                            dst[c] = uint8_t(std::round(
                                (1 - dy) * ((1 - dx) * sample(px, py, c) +
                                            dx * sample(std::min(px + 1, int(tile.width) - 1), py, c)) +
                                dy * ((1 - dx) * sample(px, std::min(py + 1, int(tile.height) - 1), c) +
                                      dx * sample(std::min(px + 1, int(tile.width) - 1),
                                                  std::min(py + 1, int(tile.height) - 1), c))));
                        continue;
                    }
                    // Area resample in global coordinates: 4x model -> requested 2x,
                    // without a full 4x canvas. Context halos are discarded, not stitched.
                    const double ax = x * step_x - ix * factor, bx = (x + 1) * step_x - ix * factor;
                    const double ay = y * step_y - iy * factor, by = (y + 1) * step_y - iy * factor;
                    double sum[3]{}, weight = 0;
                    for (int py = int(std::floor(ay)); py < int(std::ceil(by)); ++py)
                        for (int px = int(std::floor(ax)); px < int(std::ceil(bx)); ++px) {
                            const double a = (std::min(bx, double(px + 1)) - std::max(ax, double(px))) *
                                             (std::min(by, double(py + 1)) - std::max(ay, double(py)));
                            if (a <= 0)
                                continue;
                            const auto *p =
                                tile.data + (size_t(std::clamp(py, 0, int(tile.height) - 1)) * tile.width +
                                             std::clamp(px, 0, int(tile.width) - 1)) *
                                                tile.channel;
                            for (int c = 0; c < 3; ++c)
                                sum[c] += p[c] * a;
                            weight += a;
                        }
                    auto *dst = result.at(x, y);
                    for (int c = 0; c < 3; ++c)
                        dst[c] = uint8_t(std::clamp(std::round(sum[c] / weight), 0., 255.));
                }
            // Refresh only preview pixels whose full-resolution sample is complete.
            for (int y = int(int64_t(y0) * preview.h / height);
                 y < std::min(preview.h, int(int64_t(y1) * preview.h / height) + 1); ++y)
                for (int x = int(int64_t(x0) * preview.w / width);
                     x < std::min(preview.w, int(int64_t(x1) * preview.w / width) + 1); ++x) {
                    int gx = std::min(width - 1, int((int64_t(x) * 2 + 1) * width / (preview.w * 2)));
                    int gy = std::min(height - 1, int((int64_t(y) * 2 + 1) * height / (preview.h * 2)));
                    if (gx >= x0 && gx < x1 && gy >= y0 && gy < y1)
                        std::copy_n(result.at(gx, gy), 4, preview.at(x, y));
                }
            Raster display = preview;
            const int px0 = std::clamp(int(int64_t(x0) * preview.w / width), 0, preview.w - 1);
            const int px1 = std::clamp(int(int64_t(x1) * preview.w / width) - 1, px0, preview.w - 1);
            const int py0 = std::clamp(int(int64_t(y0) * preview.h / height), 0, preview.h - 1);
            const int py1 = std::clamp(int(int64_t(y1) * preview.h / height) - 1, py0, preview.h - 1);
            auto border = [&](int x, int y) {
                auto *p = display.at(x, y);
                p[0] = 255;
                p[1] = 185;
                p[2] = 50;
            };
            for (int x = px0; x <= px1; ++x) {
                border(x, py0);
                border(x, py1);
            }
            for (int y = py0; y <= py1; ++y) {
                border(px0, y);
                border(px1, y);
            }
            publish(display, ++completed, total);
        }
    return result;
}
