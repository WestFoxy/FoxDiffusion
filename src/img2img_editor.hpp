#pragma once
#include "generation.hpp"
#include "image_texture_types.hpp"
#include <deque>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <utility>
#include <unordered_set>

class Img2ImgEditor {
  public:
    bool has_source() const {return !image_.empty();}
    Img2ImgOptions options;
    std::string pending_parameters;
    void draw(int &width, int &height);
    void set_image(Raster image);
    void accept_result(Raster image);
    void load_path(const std::filesystem::path &path);
    Img2ImgOptions capture() const;
    void shutdown();
    void save_settings(std::ostream &out) const;
    void load_setting(const std::string &key, std::istream &in);

  private:
    struct Patch {
        Rect rect;
        Raster image, mask;
    };
    struct State {
        Raster image, mask;
        std::shared_ptr<Raster> baseline;
        bool full = true;
        std::vector<Patch> patches;
    };
    Raster original_, image_, mask_;
    std::shared_ptr<Raster> baseline_;
    std::deque<State> undo_, redo_;
    State pending_stroke_;
    std::unordered_set<int> captured_tiles_;
    Rect upload_region_;
    bool mipmaps_stale_ = false;
    std::vector<std::uint8_t> upload_pixels_;
    ImageTexture texture_;
    bool dirty_ = true, stroke_ = false;
    int tool_ = 0;
    float brush_ = 32, opacity_ = 1, hardness_ = .8f, color_[3] = {1, .25f, .15f};
    float zoom_ = 1, pan_x_ = 0, pan_y_ = 0, last_x_ = 0, last_y_ = 0, start_x_ = 0, start_y_ = 0;
    std::vector<std::pair<float, float>> lasso_;
    bool overlay_ = true;
    bool sketch_overlay_ = false;
    std::string message_;
    std::string parameters_;
    void checkpoint();
    void capture_tiles(Rect area, bool image, bool mask);
    void finish_stroke();
    void trim_history();
    void mark_region(Rect area);
    void brush_segment(float x0, float y0, float x1, float y1);
    void undo(bool redo);
    void canvas();
    void upload();
    void choose_image(bool mask);
    void paste_clipboard();
    void export_image(bool mask);
    void transform(int action);
};
