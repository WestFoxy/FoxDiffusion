#include "image_io.hpp"
#include "img2img_pipeline.hpp"
#include "parameters.hpp"
#include <fstream>
#include <iostream>

void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Expected validation failure");
}
int main() {
    try {
        Raster image(96, 64, 40), mask(96, 64, 0);
        float white[3] = {1, 1, 1};
        paint_segment(mask, 40, 32, 56, 32, 8, 1, 1, white);
        require(mask.at(48, 32)[0] == 255 && mask.at(0, 0)[0] == 0, "Brush geometry");
        auto blurred = blur_mask(mask, 4);
        require(blurred.at(48, 32)[0] > 200 && blurred.at(34, 32)[0] > 0, "Feathering");
        auto bounds = mask_bounds(mask, 4, 1);
        require(bounds.x >= 0 && bounds.y >= 0 && bounds.x + bounds.w <= 96, "Mask crop bounds");
        for (auto test : std::vector<std::vector<int>>{{101, 64, 16}, {64, 64, 0}, {120, 64, 63}}) {
            auto positions = tile_positions(test[0], test[1], test[2]);
            require(positions.front() == 0 && positions.back() + test[1] == test[0], "Tile edge coverage");
        }
        rejects([] { tile_positions(200, 64, 64); });
        rejects([] { mask_bounds(Raster(16, 16, 0), 0, 1); });
        auto small = resize_raster(image, 64, 64, 1);
        require(small.w == 64 && small.at(0, 0)[0] == 40, "Resize constant pixels");
        GenerationRequest r;
        r.width = 64;
        r.height = 64;
        r.img2img.enabled = true;
        r.img2img.source = image;
        r.img2img.mode = 2;
        r.img2img.mask = mask;
        r.img2img.mask_blur = 0;
        r.img2img.only_masked = true;
        int calls = 0, saves = 0;
        Raster result;
        auto infer = [&](GenerationRequest job) {
            ++calls;
            require(job.init_image.w == job.width && job.init_image.h == job.height, "Prepared size");
            require(job.strength == job.img2img.strength, "Strength forwarding");
            return Raster(job.width, job.height, 200);
        };
        auto save = [&](const Raster &im, const GenerationRequest &, int index, int total) {
            ++saves;
            result = im;
            require(index <= total, "Progress index");
        };
        execute_img2img(r, {}, infer, save);
        require(calls == 1 && saves == 1 && result.w == 96 && result.h == 64,
                "Only masked preserves canvas size");
        require(result.at(0, 0)[0] == 40 && result.at(48, 32)[0] == 200,
                "Only masked composites protected pixels");
        r.img2img.strength = 0;
        execute_img2img(r, {}, infer, save);
        require(calls == 1 && result.pixels == image.pixels, "Zero denoise is a no-op");
        r.img2img.strength = .6f;
        r.img2img.mode = 0;
        r.img2img.script = 1;
        r.img2img.upscale = 1.5f;
        r.img2img.overlap = 16;
        execute_img2img(r, {}, infer, save);
        require(result.w == 144 && result.h == 96, "Upscale output dimensions");
        for (size_t i = 0; i < result.pixels.size(); i += 4)
            require(result.pixels[i] >= 199 && result.pixels[i] <= 200, "Upscale has no empty seams");
        r.img2img.script = 2;
        r.img2img.loops = 3;
        int old = saves;
        execute_img2img(r, {}, infer, save);
        require(saves - old == 3, "Loopback saves all iterations");
        r.img2img.script = 4;
        r.img2img.axis[0] = 1;
        r.img2img.values[0] = "0.2, 0.8";
        r.img2img.axis[1] = 2;
        r.img2img.values[1] = "3, 7";
        old = saves;
        execute_img2img(r, {}, infer, save);
        require(saves - old == 5, "XYZ Cartesian product and labelled grid");
        r.img2img.values[0] = "NaN";
        rejects([&] { execute_img2img(r, {}, infer, save); });
        r.img2img.script = 5;
        r.img2img.prompts = "a tree\na lake";
        old = saves;
        execute_img2img(r, {}, infer, save);
        require(saves - old == 2, "Prompt list");
        r.img2img.script = 3;
        for (auto &v : r.img2img.expand)
            v = 16;
        execute_img2img(r, {}, infer, save);
        require(result.w == 128 && result.h == 96 && result.at(64, 48)[0] == 40,
                "Outpaint preserves interior");
        std::stop_source stop;
        stop.request_stop();
        rejects([&] { execute_img2img(r, stop.get_token(), infer, save); });
        auto dir = std::filesystem::current_path() / L"test-images";
        std::filesystem::create_directories(dir);
        std::string error;
        auto unicode = dir / L"\u043c\u0430\u0441\u043a\u0430.png";
        require(save_image(unicode, image.pixels, image.w, image.h, 95, error), "Unicode save");
        Raster read;
        require(load_image_rgba(unicode, read.pixels, read.w, read.h, error) && read.pixels == image.pixels,
                "Unicode lossless load");
        auto png = encode_png(image.pixels, image.w, image.h);
        require(load_image_rgba_memory(png, read.pixels, read.w, read.h, error), "PNG memory roundtrip");
        require(!load_image_rgba_memory({1, 2, 3}, read.pixels, read.w, read.h, error),
                "Corrupt file rejected");
        const std::string params =
            "a teapot\nNegative prompt: blurry\nSteps: 23, Sampler: Euler, Schedule type: Karras, CFG scale: "
            "6.5, Seed: 4294967295, Size: 1024x768, Denoising strength: 0.42";
        require(save_png_parameters(unicode, image.pixels, image.w, image.h, params, error),
                "PNG metadata write");
        require(read_png_parameters(unicode) == params, "PNG UTF-8 metadata roundtrip");
        GenerationRequest restored;
        apply_parameters(restored, params);
        require(restored.positive_prompt == "a teapot" && restored.negative_prompt == "blurry" &&
                    restored.steps == 23 && restored.seed == 4294967295u && restored.width == 1024 &&
                    restored.height == 768 && restored.sampling_method == "euler" &&
                    restored.scheduler == "karras" && std::abs(restored.img2img.strength - .42f) < .001,
                "A1111 metadata import");
        r.img2img.script = 6;
        r.img2img.prompts = "a teapot | red | outdoors";
        old = saves;
        execute_img2img(r, {}, infer, save);
        require(saves - old == 4, "Prompt matrix combinations");
        r.img2img.script = 0;
        r.img2img.mode = 5;
        r.img2img.input_dir = dir.string();
        r.img2img.batch_count = 2;
        old = saves;
        execute_img2img(r, {}, infer, save);
        require(saves - old == 2, "Batch directory");
        std::cout << "PASS: brush, blur, crops, resize, mask preservation, zero denoise, tiled upscale, "
                     "loopback, XYZ, prompts, outpaint, cancellation, Unicode IO, batch\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
