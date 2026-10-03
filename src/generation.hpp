#pragma once

#include "raster.hpp"
#include <cstdint>
#include <string>
#include <vector>

struct Img2ImgOptions {
    bool enabled = false;
    int mode = 0; // image, sketch, inpaint, inpaint sketch, uploaded mask, batch
    Raster source, mask;
    float strength = .75f;
    float image_cfg = 1.5f;
    int resize_mode = 0;
    bool scale_by = false;
    float scale = 1.f;
    int mask_blur = 4;
    bool invert_mask = false;
    int fill = 1; // fill, original, pixel noise, neutral pixels
    bool only_masked = true;
    int padding = 32;
    int batch_count = 1, batch_size = 1;
    int script = 0; // none, upscale, loopback, outpaint, XYZ, prompts
    float upscale = 2.f;
    std::string upscaler_model;
    int overlap = 64;
    int loops = 4;
    float final_strength = .5f;
    int expand[4] = {128, 128, 128, 128};
    int axis[3] = {0, 0, 0};
    std::string values[3] = {"0.3, 0.5, 0.7", "5, 7, 9", ""};
    std::string prompts;
    bool save_grid = true;
    std::string input_dir, mask_dir, output_dir;
    bool batch_prompt_files = false;
    bool batch_png_info = false;
};

struct GenerationRequest {
    std::string positive_prompt;
    std::string negative_prompt;
    std::string model_path;
    bool qwen_image = false;
    std::string vae_path, llm_path, vision_path;
    bool offload_to_cpu = true;
    std::string lora_directory;
    std::string embedding_directory;
    int steps = 24;
    int width = 512;
    int height = 512;
    std::uint32_t seed = 1;
    bool random_seed = false;
    std::string sampling_method = "dpm++2m";
    std::string scheduler = "karras";
    float cfg_scale = 7.0f;
    int preview_mode = 1;
    int preview_interval = 2;
    bool preview_denoised = true;
    bool preview_noisy = false;
    Img2ImgOptions img2img;
    // Already prepared native images; serialized to the server as lossless PNG.
    Raster init_image, mask_image, reference_image;
    float strength = .75f, image_cfg = 1.5f;
    std::string output_label;
    bool upscale_only = false;
};

struct GenerationSnapshot {
    float progress = 0.0f;
    int current_step = 0;
    int total_steps = 0;
    bool running = false;
    bool finished = false;
    std::string status = "Ready";
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    std::uint64_t revision = 0;
    std::vector<std::string> outputs;
    int job_index = 0, job_count = 0;
    int pending_saves = 0;
    std::string save_error;
};
