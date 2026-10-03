#pragma once

#include "backend_process.hpp"
#include "img2img_editor.hpp"

#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <array>

struct GLFWwindow;
struct ImVec2;

class App {
public:
    bool initialize();
    void run();
    void shutdown();

private:
    void render_frame();
    void draw_caption_buttons();
    void draw_menu_bar();
    void draw_settings();
    void choose_component(std::string& path);
    void draw_workspace();
    std::string panel_title(const char* name) const;
    bool qwen_selected() const;
    void draw_prompt_panel();
    void draw_preview_panel();
    void draw_output_canvas(const std::vector<std::uint8_t>& pixels, int width, int height, ImVec2 size);
    void draw_models_panel();
    void draw_model_loading();
    void draw_status_bar();
    void update_preview_texture();
    void refresh_models();
    void filter_models();
    void refresh_embeddings();
    void choose_model_folder();
    void choose_embedding_folder();
    void choose_lora_folder();
    void save_current_image();
    void start_generation();
    void apply_theme();
    void load_settings();
    void save_settings() const;

    bool settings_open_ = false, settings_capture_ = false;
    ImageTexture settings_blur_;
    bool auto_save_txt_ = true, auto_save_img_ = false;
    int model_type_ = 0;
    std::string qwen_vae_, qwen_llm_, qwen_vision_;
    bool qwen_offload_ = true, settings_blur_enabled_ = true;
    bool show_telemetry_ = true;
    int idle_fps_ = 30;
    GLFWwindow* window_ = nullptr;
    ImageTexture preview_texture_;
    std::uint64_t uploaded_revision_ = 0;
    BackendProcess backend_;
    GenerationSnapshot frame_snapshot_;
    Img2ImgEditor img2img_;
    bool img2img_active_ = false, workspace_mode_ = false;
    std::string model_filter_;
    std::vector<int> filtered_models_;
    int gallery_index_ = -1;
    Raster gallery_image_;
    float output_zoom_ = 1.0f, output_pan_x_ = 0.0f, output_pan_y_ = 0.0f;
    bool output_actual_size_ = false;
    int output_view_width_ = 0, output_view_height_ = 0;

    std::string positive_prompt_ = "a small fox in a moonlit forest, detailed fur";
    std::string negative_prompt_ = "blurry, low quality, artifacts";
    std::string model_directory_;
    std::string embedding_directory_;
    std::string lora_directory_;
    std::vector<std::filesystem::path> models_;
    std::vector<std::string> model_labels_;
    std::vector<std::string> model_sizes_;
    std::vector<std::string> model_paths_;
    std::map<std::string,std::array<float,4>> ui_colors_;
    std::vector<std::string> embedding_names_;
    std::string model_hash_ = "70b33002f4";
    int selected_model_ = -1;
    int steps_ = 24;
    int width_ = 512;
    int height_ = 512;
    std::uint32_t seed_ = 1;
    bool random_seed_ = false;
    int sampler_ = 5;
    int scheduler_ = 1;
    float cfg_scale_ = 7.0f;
    int preview_mode_ = 1;
    int preview_interval_ = 2;
    bool preview_denoised_ = true;
    bool preview_noisy_ = false;
    float preview_area_height_ = 0.0f;
    bool prompt_one_per_line_ = true;
    bool prompt_layout_initialized_ = false;
    bool show_demo_window_ = false;
    bool reset_workspace_layout_ = false;
    std::string save_message_;
    std::string imgui_ini_path_;
};
