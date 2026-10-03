#include "backend_process.hpp"
#include "image_io.hpp"
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: img2img-smoke MODEL_PATH\n";
        return 2;
    }
    BackendProcess backend;
    GenerationRequest r;
    r.model_path = argv[1];
    r.positive_prompt = "a small red ceramic teapot on a wooden table, studio photograph";
    const int dim = argc > 2 ? std::stoi(argv[2]) : 256;
    r.negative_prompt = "blurry, text";
    r.steps = 8;
    r.width = r.height = dim;
    r.preview_mode = 0;
    r.sampling_method = "euler";
    r.scheduler = "discrete";
    r.img2img.enabled = true;
    r.img2img.strength = .65f;
    r.img2img.source = Raster(dim, dim, 160);
    r.img2img.output_dir = "smoke-results";
    auto run = [&]() {
        backend.start(r);
        const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(6);
        std::string last;
        for (;;) {
            auto s = backend.snapshot();
            if (last != s.status) {
                last = s.status;
                std::cout << last << std::endl;
            }
            if (!s.running)
                return s;
            if (std::chrono::steady_clock::now() > until) {
                backend.cancel();
                throw std::runtime_error("Smoke timeout");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    };
    try {
        auto plain = run();
        if (!plain.finished || plain.outputs.empty())
            throw std::runtime_error("Img2img failed: " + plain.status);
        r.img2img.source.pixels = plain.rgba;
        r.img2img.mode = 2;
        r.img2img.mask = Raster(dim, dim, 0);
        r.img2img.mask_blur = 0;
        r.img2img.padding = 16;
        float white[3] = {1, 1, 1};
        paint_segment(r.img2img.mask, dim * .4f, dim * .5f, dim * .6f, dim * .5f, dim * .12f, 1, 1, white);
        auto inpaint = run();
        if (!inpaint.finished)
            throw std::runtime_error("Inpaint failed: " + inpaint.status);
        size_t changed = 0;
        for (size_t i = 0; i < plain.rgba.size(); i += 4) {
            if (r.img2img.mask.pixels[i] == 0) {
                for (int c = 0; c < 3; ++c)
                    if (inpaint.rgba[i + c] != plain.rgba[i + c])
                        throw std::runtime_error("Protected pixels changed");
            } else if (inpaint.rgba[i] != plain.rgba[i])
                ++changed;
        }
        if (!changed)
            throw std::runtime_error("Masked pixels did not change");
        r.img2img.mode = 0;
        r.img2img.script = 1;
        r.img2img.upscale = 1.5f;
        r.img2img.overlap = 32;
        r.steps = 4;
        if (argc > 3)
            r.img2img.upscaler_model = argv[3];
        auto upscale = run();
        if (!upscale.finished || upscale.width != int(dim * 1.5f) || upscale.height != int(dim * 1.5f))
            throw std::runtime_error("Upscale failed");
        std::cout << "PASS: real CUDA img2img, inpaint pixel protection, multi-tile SD upscale\n";
        r.img2img.script = 0;
        r.steps = 100;
        backend.start(r);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        backend.cancel();
        const auto cancel_until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (backend.snapshot().running && std::chrono::steady_clock::now() < cancel_until)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (backend.snapshot().running || backend.snapshot().finished)
            throw std::runtime_error("Cancellation failed");
        std::cout << "PASS: cancellation terminates active inference\n";
        backend.stop();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        backend.stop();
        return 1;
    }
}
