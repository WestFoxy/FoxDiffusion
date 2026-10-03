#include "img2img_editor.hpp"
#include "image_io.hpp"
#include <windows.h>
#include "image_texture.hpp"
#include <GLFW/glfw3.h>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <iomanip>
#include <shobjidl.h>

namespace {
std::filesystem::path dialog(bool save, bool folder = false, bool text = false, bool model = false) {
    const auto init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileDialog *picker = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) {
        DWORD flags = 0;
        picker->GetOptions(&flags);
        picker->SetOptions(flags | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
        if (!folder) {
            COMDLG_FILTERSPEC types[] = {{model  ? L"Upscaler models"
                                          : text ? L"Text files"
                                                 : L"Images",
                                          model  ? L"*.pth;*.safetensors;*.gguf"
                                          : text ? L"*.txt"
                                          : save ? L"*.png"
                                                 : L"*.png;*.jpg;*.jpeg;*.bmp;*.tga"}};
            picker->SetFileTypes(1, types);
            if (save)
                picker->SetDefaultExtension(L"png");
        }
        if (SUCCEEDED(picker->Show(nullptr))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(picker->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        picker->Release();
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return result;
}
void directory_input(const char *label, std::string &path) {
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-90);
    ImGui::InputText("##path", &path);
    ImGui::SameLine();
    if (ImGui::Button("Browse")) {
        auto p = dialog(false, true);
        auto u = p.u8string();
        if (!p.empty())
            path.assign(reinterpret_cast<const char *>(u.data()), u.size());
    }
    ImGui::TextDisabled("%s", label);
    ImGui::PopID();
}
} // namespace
void Img2ImgEditor::set_image(Raster image) {
    if (image.empty())
        return;
    Raster new_original, new_mask;
    std::shared_ptr<Raster> new_baseline;
    try {
        require_image_memory(checked_image_bytes(image.w, image.h) * 4);
        new_original = image;
        new_mask = Raster(image.w, image.h, 0);
        new_baseline = std::make_shared<Raster>(image);
    } catch (const std::exception &e) {
        message_ = e.what();
        return;
    }
    original_ = std::move(new_original);
    image_ = std::move(image);
    mask_ = std::move(new_mask);
    baseline_ = std::move(new_baseline);
    undo_.clear();
    redo_.clear();
    pending_stroke_ = State{};
    pending_stroke_.full = false;
    captured_tiles_.clear();
    upload_region_ = {};
    zoom_ = 1;
    pan_x_ = pan_y_ = 0;
    dirty_ = true;
    stroke_ = false;
    message_.clear();
    parameters_.clear();
}
void Img2ImgEditor::load_path(const std::filesystem::path &path) {
    Raster image;
    std::string error;
    if (load_image_rgba(path, image.pixels, image.w, image.h, error)) {
        set_image(std::move(image));
        parameters_ = read_png_parameters(path);
    } else
        message_ = error;
}
void Img2ImgEditor::accept_result(Raster image) {
    if (image.empty())
        return;
    if (image_.empty()) {
        set_image(std::move(image));
        return;
    }
    Raster new_mask;
    std::shared_ptr<Raster> new_baseline;
    try {
        require_image_memory(checked_image_bytes(image.w, image.h) * 2 +
                             checked_image_bytes(image_.w, image_.h) * 2);
        new_mask = Raster(image.w, image.h, 0);
        new_baseline = std::make_shared<Raster>(image);
        checkpoint();
    } catch (const std::exception &e) {
        message_ = e.what();
        return;
    }
    if (image.w != image_.w || image.h != image_.h) {
        zoom_ = 1;
        pan_x_ = pan_y_ = 0;
    }
    image_ = std::move(image);
    baseline_ = std::move(new_baseline);
    mask_ = std::move(new_mask);
    dirty_ = true;
    stroke_ = false;
    message_ = "Result applied. Undo restores the previous image and mask.";
}
void Img2ImgEditor::choose_image(bool mask) {
    auto path = dialog(false);
    if (path.empty())
        return;
    if (!mask) {
        load_path(path);
        return;
    }
    if (image_.empty()) {
        message_ = "Load a source image first.";
        return;
    }
    Raster loaded;
    std::string error;
    if (!load_image_rgba(path, loaded.pixels, loaded.w, loaded.h, error)) {
        message_ = error;
        return;
    }
    if (loaded.w != image_.w || loaded.h != image_.h) {
        message_ = "The mask must have the same dimensions as the source.";
        return;
    }
    checkpoint();
    mask_ = std::move(loaded);
    for (size_t i = 0; i < mask_.pixels.size(); i += 4) {
        auto v = std::uint8_t((int(mask_.pixels[i]) + mask_.pixels[i + 1] + mask_.pixels[i + 2]) / 3);
        mask_.pixels[i] = mask_.pixels[i + 1] = mask_.pixels[i + 2] = v;
        mask_.pixels[i + 3] = 255;
    }
    dirty_ = true;
}
void Img2ImgEditor::trim_history() {
    auto bytes = [](const State &state) {
        size_t result = state.image.pixels.size() + state.mask.pixels.size();
        for (const auto &p : state.patches)
            result += p.image.pixels.size() + p.mask.pixels.size();
        return result;
    };
    size_t total = 0;
    for (const auto &state : undo_)
        total += bytes(state);
    for (const auto &state : redo_)
        total += bytes(state);
    while (undo_.size() > 1 && (undo_.size() + redo_.size() > 256 || total > 512ULL * 1024 * 1024)) {
        total -= bytes(undo_.front());
        undo_.pop_front();
    }
}
void Img2ImgEditor::finish_stroke() {
    if (!pending_stroke_.patches.empty()) {
        redo_.clear();
        undo_.push_back(std::move(pending_stroke_));
        trim_history();
    }
    pending_stroke_ = State{};
    pending_stroke_.full = false;
    captured_tiles_.clear();
    stroke_ = false;
}
void Img2ImgEditor::checkpoint() {
    finish_stroke();
    if (image_.empty())
        return;
    undo_.push_back({image_, mask_, baseline_});
    redo_.clear();
    trim_history();
}
void Img2ImgEditor::capture_tiles(Rect area, bool save_image, bool save_mask) {
    if (area.w <= 0 || area.h <= 0)
        return;
    constexpr int tile = 128;
    const int columns = (image_.w + tile - 1) / tile;
    for (int ty = area.y / tile; ty <= (area.y + area.h - 1) / tile; ++ty)
        for (int tx = area.x / tile; tx <= (area.x + area.w - 1) / tile; ++tx) {
            if (!captured_tiles_.insert(ty * columns + tx).second)
                continue;
            Patch p;
            p.rect = {tx * tile, ty * tile, std::min(tile, image_.w - tx * tile),
                      std::min(tile, image_.h - ty * tile)};
            if (save_image)
                p.image = crop_raster(image_, p.rect);
            if (save_mask)
                p.mask = crop_raster(mask_, p.rect);
            pending_stroke_.patches.push_back(std::move(p));
        }
}
void Img2ImgEditor::mark_region(Rect area) {
    if (area.w <= 0 || area.h <= 0)
        return;
    if (upload_region_.w == 0)
        upload_region_ = area;
    else {
        int right = std::max(upload_region_.x + upload_region_.w, area.x + area.w);
        int bottom = std::max(upload_region_.y + upload_region_.h, area.y + area.h);
        upload_region_.x = std::min(upload_region_.x, area.x);
        upload_region_.y = std::min(upload_region_.y, area.y);
        upload_region_.w = right - upload_region_.x;
        upload_region_.h = bottom - upload_region_.y;
    }
}
void Img2ImgEditor::undo(bool redo) {
    finish_stroke();
    auto &from = redo ? redo_ : undo_;
    auto &to = redo ? undo_ : redo_;
    if (from.empty())
        return;
    State state = std::move(from.back());
    from.pop_back();
    if (state.full) {
        std::swap(image_, state.image);
        std::swap(mask_, state.mask);
        std::swap(baseline_, state.baseline);
        dirty_ = true;
    } else {
        for (auto &patch : state.patches) {
            auto exchange = [&](Raster &dst, Raster &saved) {
                if (saved.empty())
                    return;
                for (int y = 0; y < patch.rect.h; ++y) {
                    auto *pixels = dst.at(patch.rect.x, patch.rect.y + y);
                    auto *before = saved.at(0, y);
                    for (size_t i = 0; i < size_t(patch.rect.w) * 4; ++i)
                        std::swap(pixels[i], before[i]);
                }
            };
            exchange(image_, patch.image);
            exchange(mask_, patch.mask);
            mark_region(patch.rect);
        }
    }
    to.push_back(std::move(state));
}
void Img2ImgEditor::brush_segment(float x0, float y0, float x1, float y1) {
    const float radius = std::max(.5f, brush_ * .5f);
    int left = int(std::floor(std::clamp(std::min(x0, x1) - radius, 0.f, float(image_.w))));
    int top = int(std::floor(std::clamp(std::min(y0, y1) - radius, 0.f, float(image_.h))));
    int right = int(std::ceil(std::clamp(std::max(x0, x1) + radius, 0.f, float(image_.w))));
    int bottom = int(std::ceil(std::clamp(std::max(y0, y1) + radius, 0.f, float(image_.h))));
    Rect area{left, top, right - left, bottom - top};
    if (area.w <= 0 || area.h <= 0)
        return;
    bool masked = options.mode >= 2 && options.mode <= 4;
    capture_tiles(area, !masked || options.mode == 3, masked);
    float white[3] = {1, 1, 1}, black[3] = {0, 0, 0};
    if (tool_ == 1 && (!masked || options.mode == 3)) {
        // Erase only the touched pixels. No full-canvas alpha allocation or resize.
        float dx = x1 - x0, dy = y1 - y0, len = dx * dx + dy * dy;
        for (int y = top; y < bottom; ++y)
            for (int x = left; x < right; ++x) {
                float t = len ? std::clamp(((x + .5f - x0) * dx + (y + .5f - y0) * dy) / len, 0.f, 1.f) : 0;
                float distance = std::hypot(x + .5f - x0 - t * dx, y + .5f - y0 - t * dy) / radius;
                float alpha = opacity_ * std::clamp((1 - distance) / std::max(.01f, 1 - hardness_), 0.f, 1.f);
                auto *target = image_.at(x, y);
                const auto *source = baseline_->at(x, y);
                for (int c = 0; c < 3; ++c)
                    target[c] = std::uint8_t(target[c] * (1 - alpha) + source[c] * alpha);
            }
        if (masked)
            paint_segment(mask_, x0, y0, x1, y1, radius, 1.f, hardness_, black);
    } else {
        paint_segment(masked ? mask_ : image_, x0, y0, x1, y1, radius, masked ? 1.f : opacity_, hardness_,
                      masked ? (tool_ == 1 ? black : white) : color_);
        if (options.mode == 3)
            paint_segment(image_, x0, y0, x1, y1, radius, opacity_, hardness_, color_);
    }
    mark_region(area);
}
void Img2ImgEditor::shutdown() {
    clear_image_texture(texture_);
}
void Img2ImgEditor::paste_clipboard() {
    if (!OpenClipboard(nullptr)) {
        message_ = "Clipboard is busy.";
        return;
    }
    HANDLE handle = GetClipboardData(CF_DIB);
    auto *data = handle ? static_cast<const unsigned char *>(GlobalLock(handle)) : nullptr;
    if (data) {
        const auto *header = reinterpret_cast<const BITMAPINFOHEADER *>(data);
        const size_t bytes = GlobalSize(handle);
        if (bytes >= sizeof(BITMAPINFOHEADER) && header->biSize >= 40 && header->biSize <= bytes &&
            header->biWidth > 0 && header->biHeight != 0 && header->biHeight != LONG_MIN &&
            (header->biBitCount == 24 || header->biBitCount == 32) && header->biCompression == BI_RGB &&
            header->biClrUsed == 0 &&
            size_t(header->biWidth) * std::abs(header->biHeight) <= uint64_t(INT_MAX) / 4) {
            int w = header->biWidth, h = std::abs(header->biHeight), channels = header->biBitCount / 8;
            size_t stride = (size_t(w) * channels + 3) & ~size_t(3);
            if (header->biSize + stride * h <= bytes) {
                Raster image(w, h);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        auto p = data + header->biSize + (header->biHeight > 0 ? h - 1 - y : y) * stride +
                                 x * channels;
                        auto q = image.at(x, y);
                        q[0] = p[2];
                        q[1] = p[1];
                        q[2] = p[0];
                    }
                set_image(std::move(image));
            }
        } else
            message_ = "Clipboard bitmap format is unsupported. Save it as PNG and use Open image.";
        GlobalUnlock(handle);
    } else
        message_ = "No bitmap in clipboard.";
    CloseClipboard();
}
void Img2ImgEditor::export_image(bool mask) {
    if (image_.empty())
        return;
    auto path = dialog(true);
    if (path.empty())
        return;
    const auto &image = mask ? mask_ : image_;
    std::string error;
    if (!save_image(path, image.pixels, image.w, image.h, 95, error))
        message_ = error;
    else
        message_ = "Saved.";
}
void Img2ImgEditor::transform(int action) {
    if (image_.empty())
        return;
    checkpoint();
    auto apply = [&](Raster &image) {
        Raster result(action == 2 ? image.h : image.w, action == 2 ? image.w : image.h);
        for (int y = 0; y < image.h; ++y)
            for (int x = 0; x < image.w; ++x) {
                int xx = action == 0 ? image.w - 1 - x : action == 2 ? image.h - 1 - y : x;
                int yy = action == 1 ? image.h - 1 - y : action == 2 ? x : y;
                std::copy_n(image.at(x, y), 4, result.at(xx, yy));
            }
        image = std::move(result);
    };
    apply(image_);
    apply(mask_);
    baseline_ = std::make_shared<Raster>(*baseline_);
    apply(*baseline_);
    dirty_ = true;
    zoom_ = 1;
    pan_x_ = pan_y_ = 0;
}
Img2ImgOptions Img2ImgEditor::capture() const {
    auto result = options;
    result.enabled = true;
    result.source = image_;
    if (options.mode >= 2 && options.mode <= 4)
        result.mask = mask_;
    return result;
}
void Img2ImgEditor::upload() {
    if (image_.empty())
        return;
    const bool full = dirty_ || texture_.width != image_.w || texture_.height != image_.h;
    Rect area = full ? Rect{0, 0, image_.w, image_.h} : upload_region_;
    if (area.w > 0 && area.h > 0) {
        const bool overlay = (options.mode == 3 ? sketch_overlay_ : overlay_) &&
                             ((options.mode >= 2 && options.mode <= 4) || (tool_ >= 2 && tool_ <= 4));
        upload_pixels_.resize(size_t(area.w) * area.h * 4);
        for (int y = 0; y < area.h; ++y) {
            auto *dst = upload_pixels_.data() + size_t(y) * area.w * 4;
            const auto *src = image_.at(area.x, area.y + y);
            std::copy_n(src, size_t(area.w) * 4, dst);
            if (overlay) {
                const auto *mask = mask_.at(area.x, area.y + y);
                for (int x = 0; x < area.w; ++x) {
                    float a = mask[x * 4] / 255.f * .48f;
                    dst[x * 4] = std::uint8_t(dst[x * 4] * (1 - a) + 180 * a);
                    dst[x * 4 + 1] = std::uint8_t(dst[x * 4 + 1] * (1 - a) + 75 * a);
                    dst[x * 4 + 2] = std::uint8_t(dst[x * 4 + 2] * (1 - a) + 255 * a);
                }
            }
        }
        allocate_image_texture(texture_, image_.w, image_.h);
        update_image_texture_region(texture_, upload_pixels_.data(), area.x, area.y, area.w, area.h);
        mipmaps_stale_ = true;
        dirty_ = false;
        upload_region_ = {};
    }
    // While dragging, show the updated full-resolution level immediately.
    // Rebuild the minified viewing levels once at stroke end, not every sample.
    if (mipmaps_stale_ && !stroke_) {
        finish_image_texture(texture_);
        mipmaps_stale_ = false;
    }
}
void Img2ImgEditor::canvas() {
    ImGui::BeginChild("editorCanvas", ImVec2(0, std::max(1.f, ImGui::GetContentRegionAvail().y)),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 pos = ImGui::GetCursorScreenPos(), size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool hover = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    auto &io = ImGui::GetIO();
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(22, 24, 32, 255));
    if (image_.empty()) {
        draw->AddText(ImVec2(pos.x + 24, pos.y + 36), IM_COL32(175, 180, 200, 255),
                      "Open an image, paste from clipboard, or create a blank canvas.");
        ImGui::EndChild();
        return;
    }
    float fit = std::min(size.x / image_.w, size.y / image_.h), scale = fit * zoom_;
    ImVec2 origin(pos.x + (size.x - image_.w * scale) * .5f + pan_x_,
                  pos.y + (size.y - image_.h * scale) * .5f + pan_y_);
    if (hover && io.MouseWheel != 0) {
        float old = scale;
        zoom_ = std::clamp(zoom_ * std::pow(1.2f, io.MouseWheel), .25f, std::max(16.f, 8.f / fit));
        scale = fit * zoom_;
        pan_x_ = (io.MousePos.x - pos.x - size.x * .5f) * (1 - scale / old) + pan_x_ * scale / old;
        pan_y_ = (io.MousePos.y - pos.y - size.y * .5f) * (1 - scale / old) + pan_y_ * scale / old;
    }
    if ((hover || active) &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
         (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))) {
        pan_x_ += io.MouseDelta.x;
        pan_y_ += io.MouseDelta.y;
    }
    origin = ImVec2(pos.x + (size.x - image_.w * scale) * .5f + pan_x_,
                    pos.y + (size.y - image_.h * scale) * .5f + pan_y_);
    float x = (io.MousePos.x - origin.x) / scale, y = (io.MousePos.y - origin.y) / scale;
    bool inside = x >= 0 && y >= 0 && x < image_.w && y < image_.h;
    const bool picking = tool_ == 5 || (tool_ <= 1 && io.KeyAlt && !stroke_);
    bool started = false;
    if (hover && inside && ImGui::IsMouseClicked(0) && !ImGui::IsKeyDown(ImGuiKey_Space)) {
        if (picking) {
            const auto *pixel = image_.at(int(x), int(y));
            for (int c = 0; c < 3; ++c)
                color_[c] = pixel[c] / 255.f;
            if (tool_ != 0) {
                tool_ = 0;
                dirty_ = true;
            }
        } else {
            finish_stroke();
            stroke_ = started = true;
            start_x_ = last_x_ = x;
            start_y_ = last_y_ = y;
            lasso_ = {{x, y}};
        }
    }
    if (stroke_ && ImGui::IsMousePosValid() && (ImGui::IsMouseDown(0) || ImGui::IsMouseReleased(0)) &&
        !ImGui::IsKeyDown(ImGuiKey_Space)) {
        if (tool_ <= 1 && (started || x != last_x_ || y != last_y_))
            brush_segment(last_x_, last_y_, x, y);
        if (tool_ == 4 && (std::hypot(x - lasso_.back().first, y - lasso_.back().second) > 2 / scale ||
                           ImGui::IsMouseReleased(0)))
            lasso_.emplace_back(x, y);
        last_x_ = x;
        last_y_ = y;
    }
    if (stroke_ && !ImGui::IsMouseDown(0)) {
        if (tool_ >= 2 && tool_ <= 4) {
            float x0 = std::min(start_x_, last_x_), x1 = std::max(start_x_, last_x_),
                  y0 = std::min(start_y_, last_y_), y1 = std::max(start_y_, last_y_);
            if (tool_ == 4)
                for (const auto &point : lasso_) {
                    x0 = std::min(x0, point.first);
                    x1 = std::max(x1, point.first);
                    y0 = std::min(y0, point.second);
                    y1 = std::max(y1, point.second);
                }
            const int left = int(std::clamp(std::floor(x0), 0.f, float(mask_.w)));
            const int top = int(std::clamp(std::floor(y0), 0.f, float(mask_.h)));
            const int right = int(std::clamp(std::ceil(x1) + 1, 0.f, float(mask_.w)));
            const int bottom = int(std::clamp(std::ceil(y1) + 1, 0.f, float(mask_.h)));
            Rect area{left, top, right - left, bottom - top};
            capture_tiles(area, false, true);
            for (int py = top; py < bottom; ++py)
                for (int px = left; px < right; ++px) {
                    bool selected = false;
                    if (tool_ == 2)
                        selected = px >= x0 && px <= x1 && py >= y0 && py <= y1;
                    if (tool_ == 3 && x1 > x0 && y1 > y0)
                        selected = std::pow((px - (x0 + x1) * .5f) / ((x1 - x0) * .5f), 2) +
                                       std::pow((py - (y0 + y1) * .5f) / ((y1 - y0) * .5f), 2) <=
                                   1;
                    if (tool_ == 4 && lasso_.size() > 2)
                        for (size_t i = 0, j = lasso_.size() - 1; i < lasso_.size(); j = i++) {
                            auto a = lasso_[i], b = lasso_[j];
                            if ((a.second > py) != (b.second > py) &&
                                px < (b.first - a.first) * (py - a.second) / (b.second - a.second) + a.first)
                                selected = !selected;
                        }
                    if (selected) {
                        auto *pixel = mask_.at(px, py);
                        pixel[0] = pixel[1] = pixel[2] = io.KeyAlt ? 0 : 255;
                    }
                }
            mark_region(area);
        }
        finish_stroke();
    }
    upload();
    draw->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
    draw_image_texture(texture_, draw, origin, scale);
    if (stroke_ && tool_ >= 2) {
        ImVec2 a(origin.x + start_x_ * scale, origin.y + start_y_ * scale),
            b(origin.x + last_x_ * scale, origin.y + last_y_ * scale);
        if (tool_ == 2)
            draw->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)),
                          ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(220, 180, 255, 255));
        if (tool_ == 3)
            draw->AddEllipse(ImVec2((a.x + b.x) * .5f, (a.y + b.y) * .5f),
                             ImVec2(std::abs(a.x - b.x) * .5f, std::abs(a.y - b.y) * .5f),
                             IM_COL32(220, 180, 255, 255));
        if (tool_ == 4)
            for (size_t i = 1; i < lasso_.size(); ++i)
                draw->AddLine(
                    ImVec2(origin.x + lasso_[i - 1].first * scale, origin.y + lasso_[i - 1].second * scale),
                    ImVec2(origin.x + lasso_[i].first * scale, origin.y + lasso_[i].second * scale),
                    IM_COL32(220, 180, 255, 255));
    }
    if (hover && inside && tool_ <= 1 && !picking)
        draw->AddCircle(io.MousePos, brush_ * .5f * scale, IM_COL32(255, 255, 255, 225), 48, 1.5f);
    if (hover && inside && picking) {
        draw->AddLine(ImVec2(io.MousePos.x - 8, io.MousePos.y), ImVec2(io.MousePos.x + 8, io.MousePos.y),
                      IM_COL32_WHITE);
        draw->AddLine(ImVec2(io.MousePos.x, io.MousePos.y - 8), ImVec2(io.MousePos.x, io.MousePos.y + 8),
                      IM_COL32_WHITE);
        const auto *pixel = image_.at(int(x), int(y));
        ImGui::SetTooltip("Pick #%02X%02X%02X | B: brush", unsigned(pixel[0]), unsigned(pixel[1]),
                          unsigned(pixel[2]));
    }
    draw->PopClipRect();
    if (ImGui::IsWindowFocused() && !io.WantTextInput) {
        if (!stroke_ && !io.KeyCtrl && !io.KeyAlt) {
            int next = tool_;
            if (ImGui::IsKeyPressed(ImGuiKey_B))
                next = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_E))
                next = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_I))
                next = 5;
            if (next != tool_) {
                tool_ = next;
                dirty_ = true;
            }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
            undo(io.KeyShift);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
            undo(true);
    }
    ImGui::EndChild();
}

void Img2ImgEditor::draw(int &width, int &height) {
    ImGui::Begin("Img2img editor");
    ImGuiErrorRecoveryState stack_state;
    ImGui::ErrorRecoveryStoreState(&stack_state);
    try {
        const char *modes[] = {"Img2img", "Sketch", "Inpaint", "Inpaint sketch", "Inpaint upload", "Batch"};
        if (ImGui::BeginTabBar("modes")) {
            for (int i = 0; i < 6; ++i)
                if (ImGui::BeginTabItem(modes[i])) {
                    if (options.mode != i) {
                        options.mode = i;
                        dirty_ = true;
                    }
                    ImGui::EndTabItem();
                }
            ImGui::EndTabBar();
        }
        if (ImGui::BeginTable("editorLayoutCompact", 2, ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Canvas", ImGuiTableColumnFlags_WidthStretch, 1.f);
            ImGui::TableSetupColumn("Settings", ImGuiTableColumnFlags_WidthFixed, 250.f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::BeginChild("img2imgSettings");
            ImGui::SeparatorText("Image to image");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##denoise", &options.strength, 0, 1, "Denoising strength  %.2f");
            ImGui::SetItemTooltip("0 keeps the source; 1 redraws it most strongly.");
            if (ImGui::CollapsingHeader("Size & resize")) {
                ImGui::Checkbox("Scale by", &options.scale_by);
                if (options.scale_by) {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderFloat("##scale", &options.scale, .25f, 4, "Scale  %.2fx");
                } else {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputInt("##width", &width, 64);
                    ImGui::TextDisabled("Width");
                    ImGui::SetNextItemWidth(-1);
                    ImGui::InputInt("##height", &height, 64);
                    ImGui::TextDisabled("Height");
                    width = std::clamp(width, 64, 4096);
                    height = std::clamp(height, 64, 4096);
                }
                if (!image_.empty() && ImGui::Button("Use source size")) {
                    width = std::clamp(image_.w / 8 * 8, 64, 4096);
                    height = std::clamp(image_.h / 8 * 8, 64, 4096);
                    options.scale_by = false;
                }
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##resize", &options.resize_mode,
                             "Just resize\0Crop and resize\0Resize and fill\0");
            }
            if (options.mode >= 2 && options.mode <= 4) {
                ImGui::SeparatorText("Inpainting");
                ImGui::Checkbox("Inpaint unmasked", &options.invert_mask);
                ImGui::Checkbox("Only masked area", &options.only_masked);
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##blur", &options.mask_blur, 0, 64, "Mask blur  %d px");
                if (options.only_masked) {
                    ImGui::SetNextItemWidth(-1);
                    ImGui::SliderInt("##padding", &options.padding, 0, 256, "Padding  %d px");
                }
                ImGui::TextDisabled("Masked content");
                ImGui::SetNextItemWidth(-1);
                ImGui::Combo("##fill", &options.fill,
                             "Fill (surrounding color)\0Original\0Pixel noise\0Neutral pixels\0");
                bool &mode_overlay = options.mode == 3 ? sketch_overlay_ : overlay_;
                if (ImGui::Checkbox("Show mask overlay", &mode_overlay))
                    dirty_ = true;
                ImGui::SetItemTooltip(
                    "Preview tint only; does not change painted colors or generation input.");
                if (ImGui::Button("Load mask"))
                    choose_image(true);
                ImGui::SameLine();
                if (ImGui::Button("Save mask"))
                    export_image(true);
                if (ImGui::Button("Clear mask") && !image_.empty()) {
                    checkpoint();
                    mask_ = Raster(image_.w, image_.h, 0);
                    dirty_ = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Invert")) {
                    checkpoint();
                    for (size_t i = 0; i < mask_.pixels.size(); i += 4)
                        mask_.pixels[i] = mask_.pixels[i + 1] = mask_.pixels[i + 2] = 255 - mask_.pixels[i];
                    dirty_ = true;
                }
            }
            if (ImGui::CollapsingHeader("Batch & output",
                                        options.mode == 5 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##count", &options.batch_count, 1, 32, "Batch count  %d");
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##batchsize", &options.batch_size, 1, 8, "Batch size  %d");
                ImGui::TextWrapped("Images run sequentially to limit VRAM usage.");
                if (options.mode == 5) {
                    directory_input("Input directory", options.input_dir);
                    directory_input("Mask directory (optional)", options.mask_dir);
                    ImGui::Checkbox("Read matching .txt prompts", &options.batch_prompt_files);
                    ImGui::Checkbox("Use embedded PNG parameters", &options.batch_png_info);
                }
                if (ImGui::CollapsingHeader("Output directory"))
                    directory_input("Empty: portable output folder", options.output_dir);
            }
            ImGui::SeparatorText("Script");
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##script", &options.script,
                         "None\0SD upscale\0Loopback\0Outpainting\0X/Y/Z plot\0Prompts from textbox / "
                         "file\0Prompt matrix\0");
            if (options.script == 1) {
                ImGui::TextWrapped("Upscale first, diffuse overlapping tiles, then blend seams. Width / "
                                   "height set tile size.");
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##factor", &options.upscale, 1, 4, "Upscale  %.2fx");
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##overlap", &options.overlap, 0, 256, "Overlap  %d px");
                ImGui::TextWrapped(
                    "Upscaler: %s",
                    options.upscaler_model.empty()
                        ? "Bilinear"
                        : std::filesystem::u8path(options.upscaler_model).filename().string().c_str());
                if (ImGui::Button("Choose ESRGAN model")) {
                    auto path = dialog(false, false, false, true);
                    if (!path.empty()) {
                        auto u = path.u8string();
                        options.upscaler_model.assign(reinterpret_cast<const char *>(u.data()), u.size());
                    }
                }
                if (!options.upscaler_model.empty() && ImGui::Button("Use bilinear"))
                    options.upscaler_model.clear();
            }
            if (options.script == 2) {
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##loops", &options.loops, 1, 32, "Loops  %d");
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderFloat("##final", &options.final_strength, 0, 1, "Final denoising  %.2f");
                ImGui::TextWrapped("Every iteration is saved. Strength changes linearly.");
            }
            if (options.script == 3) {
                const char *names[] = {"Left", "Top", "Right", "Bottom"};
                for (int i = 0; i < 4; ++i) {
                    ImGui::SetNextItemWidth(90);
                    ImGui::InputInt(names[i], &options.expand[i], 64);
                    options.expand[i] = std::clamp(options.expand[i], 0, 2048);
                }
            }
            if (options.script == 4) {
                const char *names[] = {"X", "Y", "Z"};
                for (int i = 0; i < 3; ++i) {
                    ImGui::PushID(i);
                    ImGui::TextUnformatted(names[i]);
                    ImGui::SetNextItemWidth(-1);
                    ImGui::Combo("##axis", &options.axis[i],
                                 "Nothing\0Denoising strength\0CFG scale\0Steps\0Seed\0");
                    if (options.axis[i]) {
                        ImGui::SetNextItemWidth(-1);
                        ImGui::InputText("##values", &options.values[i]);
                    }
                    ImGui::PopID();
                }
                ImGui::TextWrapped("Comma-separated numbers. Each combination is saved with its settings.");
                ImGui::Checkbox("Save labelled grid", &options.save_grid);
            }
            if (options.script == 6) {
                ImGui::InputTextMultiline("##matrix", &options.prompts, ImVec2(-1, 100));
                ImGui::TextWrapped(
                    "base prompt | optional part | another part. All combinations use the same seed.");
            }
            if (options.script == 5) {
                ImGui::InputTextMultiline("##prompts", &options.prompts, ImVec2(-1, 140));
                if (ImGui::Button("Load text file")) {
                    auto path = dialog(false, false, true);
                    std::ifstream in(path);
                    if (in)
                        options.prompts = std::string(std::istreambuf_iterator<char>(in), {});
                }
                ImGui::TextWrapped("One complete positive prompt per line.");
            }
            if (ImGui::CollapsingHeader("Advanced##image_guidance_section")) {
                ImGui::SliderFloat("Image CFG##image_guidance_value", &options.image_cfg, 0, 10);
                ImGui::TextWrapped("For checkpoints that support image guidance (InstructPix2Pix).");
            }
            ImGui::EndChild();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, 3));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5, 4));
            if (ImGui::Button("Image"))
                ImGui::OpenPopup("image_actions");
            if (ImGui::BeginPopup("image_actions")) {
                if (ImGui::MenuItem("Open image...", "Ctrl+O"))
                    choose_image(false);
                if (ImGui::MenuItem("Paste", "Ctrl+V"))
                    paste_clipboard();
                if (ImGui::MenuItem("New canvas"))
                    set_image(Raster(width, height));
                if (ImGui::MenuItem("Export image...", nullptr, false, !image_.empty()))
                    export_image(false);
                if (!parameters_.empty() && ImGui::MenuItem("Use PNG generation parameters"))
                    pending_parameters = parameters_;
                ImGui::Separator();
                ImGui::BeginDisabled(image_.empty());
                if (ImGui::MenuItem("Flip horizontal"))
                    transform(0);
                if (ImGui::MenuItem("Flip vertical"))
                    transform(1);
                if (ImGui::MenuItem("Rotate clockwise"))
                    transform(2);
                if (ImGui::MenuItem("Crop to selection")) {
                    try {
                        auto r = mask_bounds(mask_, 0, float(image_.w) / image_.h);
                        checkpoint();
                        image_ = crop_raster(image_, r);
                        mask_ = crop_raster(mask_, r);
                        baseline_ = std::make_shared<Raster>(crop_raster(*baseline_, r));
                        dirty_ = true;
                        zoom_ = 1;
                        pan_x_ = pan_y_ = 0;
                    } catch (const std::exception &e) {
                        message_ = e.what();
                    }
                }
                if (ImGui::MenuItem("Reset to imported image")) {
                    checkpoint();
                    image_ = original_;
                    baseline_ = std::make_shared<Raster>(original_);
                    mask_ = Raster(image_.w, image_.h, 0);
                    dirty_ = true;
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(undo_.empty());
            if (ImGui::Button("Undo"))
                undo(false);
            ImGui::SetItemTooltip("Undo (Ctrl+Z), including an applied generation result");
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(redo_.empty());
            if (ImGui::Button("Redo"))
                undo(true);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Fit")) {
                zoom_ = 1;
                pan_x_ = pan_y_ = 0;
            }
            ImGui::SameLine();
            const char *tool_names[] = {"Brush##tool_menu",   "Eraser##tool_menu", "Rectangle##tool_menu",
                                        "Ellipse##tool_menu", "Lasso##tool_menu",  "Eyedropper##tool_menu"};
            if (ImGui::Button(tool_names[tool_]))
                ImGui::OpenPopup("paint_tools");
            ImGui::SetItemTooltip("Brush, eraser, selections, color, opacity and hardness");
            if (ImGui::BeginPopup("paint_tools")) {
                ImGui::BeginDisabled(image_.empty());
                ImGui::SetNextItemWidth(170);
                if (ImGui::Combo("Tool##picker", &tool_,
                                 "Brush\0Eraser\0Rectangle\0Ellipse\0Lasso\0Eyedropper\0"))
                    dirty_ = true;
                ImGui::SetNextItemWidth(300);
                ImGui::SliderFloat("Size", &brush_, 1, 512, "%.0f px",
                                   ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip("[ / ]: smaller / larger | Ctrl+click: type exact size");
                if (options.mode != 2 && options.mode != 4) {
                    ImGui::SetNextItemWidth(170);
                    ImGui::SliderFloat("Color opacity", &opacity_, .01f, 1, "%.2f");
                } else
                    ImGui::TextDisabled("Mask opacity: 100%%");
                ImGui::SetNextItemWidth(170);
                ImGui::SliderFloat("Hardness", &hardness_, 0, 1, "%.2f");
                ImGui::ColorEdit3("Color", color_, ImGuiColorEditFlags_NoInputs);
                if (ImGui::Button("Pick from image (I)")) {
                    tool_ = 5;
                    dirty_ = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndDisabled();
                ImGui::Separator();
                ImGui::TextDisabled("Wheel: zoom | Middle drag / Space: pan");
                ImGui::TextDisabled("Alt+click: pick color | B: brush | I: eyedropper");
                ImGui::TextDisabled("Alt with selection: subtract | Ctrl+Z: undo");
                ImGui::EndPopup();
            }
            if (tool_ <= 1 || tool_ == 5) {
                const float color_width = ImGui::GetFrameHeight();
                ImGui::SameLine();
                if (ImGui::GetContentRegionAvail().x < 180.f)
                    ImGui::NewLine();
                ImGui::SetNextItemWidth(
                    std::clamp(ImGui::GetContentRegionAvail().x - color_width - 10.f, 110.f, 360.f));
                ImGui::SliderFloat("##quick_brush", &brush_, 1, 512, "Size %.0f px",
                                   ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
                ImGui::SetItemTooltip("[ / ]: smaller / larger | Ctrl+click: type exact size");
                ImGui::SameLine();
                ImGui::ColorEdit3("##toolbar_color", color_,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
                ImGui::SetItemTooltip("Brush color | Alt+click image: pick color");
            }
            ImGui::PopStyleVar(2);
            if (!message_.empty()) {
                ImGui::TextWrapped("%s", message_.c_str());
                if (ImGui::IsItemClicked())
                    message_.clear();
                ImGui::SetItemTooltip("Click to dismiss");
            }
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl) {
                if (ImGui::IsKeyPressed(ImGuiKey_O))
                    choose_image(false);
                if (ImGui::IsKeyPressed(ImGuiKey_V))
                    paste_clipboard();
            }
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt) {
                static constexpr int sizes[] = {
                    1,  2,  3,  4,  5,  6,  7,   8,   9,   10,  12,  14,  16,  18,  20,  24,  28,  32,  36,
                    40, 50, 60, 70, 80, 90, 100, 120, 140, 160, 180, 200, 250, 300, 350, 400, 450, 500, 512};
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) {
                    float smaller = 1;
                    for (int value : sizes)
                        if (value < brush_)
                            smaller = float(value);
                    brush_ = smaller;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) {
                    for (int value : sizes)
                        if (value > brush_) {
                            brush_ = float(value);
                            break;
                        }
                }
            }
            canvas();
            ImGui::EndTable();
        }
    } catch (const std::exception &e) {
        message_ = e.what();
        ImGui::ErrorRecoveryTryToRecoverState(&stack_state);
    }
    ImGui::End();
}
void Img2ImgEditor::save_settings(std::ostream &out) const {
    out << "img2img_settings " << options.strength << ' ' << options.resize_mode << ' ' << options.mask_blur
        << ' ' << options.only_masked << ' ' << options.padding << ' ' << options.fill << ' ' << brush_ << ' '
        << opacity_ << ' ' << hardness_ << '\n';
    out << "img2img_workflow " << options.scale_by << ' ' << options.scale << ' ' << options.invert_mask
        << ' ' << options.batch_count << ' ' << options.batch_size << ' ' << options.script << ' '
        << options.upscale << ' ' << options.overlap << ' ' << options.loops << ' ' << options.final_strength
        << ' ' << options.image_cfg << ' ' << options.save_grid << ' ' << options.batch_prompt_files;
    for (auto v : options.expand)
        out << ' ' << v;
    for (int i = 0; i < 3; ++i)
        out << ' ' << options.axis[i] << ' ' << std::quoted(options.values[i]);
    out << ' ' << std::quoted(options.prompts) << ' ' << std::quoted(options.input_dir) << ' '
        << std::quoted(options.mask_dir) << ' ' << std::quoted(options.output_dir) << ' '
        << std::quoted(options.upscaler_model) << '\n';
    out << "img2img_png_info " << options.batch_png_info << '\n';
}
void Img2ImgEditor::load_setting(const std::string &key, std::istream &in) {
    if (key == "img2img_png_info") {
        in >> options.batch_png_info;
        return;
    }
    if (key == "img2img_workflow") {
        in >> options.scale_by >> options.scale >> options.invert_mask >> options.batch_count >>
            options.batch_size >> options.script >> options.upscale >> options.overlap >> options.loops >>
            options.final_strength >> options.image_cfg >> options.save_grid >> options.batch_prompt_files;
        for (auto &v : options.expand) {
            in >> v;
            v = std::clamp(v, 0, 2048);
        }
        for (int i = 0; i < 3; ++i) {
            in >> options.axis[i] >> std::quoted(options.values[i]);
            options.axis[i] = std::clamp(options.axis[i], 0, 4);
        }
        in >> std::quoted(options.prompts) >> std::quoted(options.input_dir) >>
            std::quoted(options.mask_dir) >> std::quoted(options.output_dir) >>
            std::quoted(options.upscaler_model);
        options.scale = std::clamp(options.scale, .25f, 4.f);
        options.batch_count = std::clamp(options.batch_count, 1, 32);
        options.batch_size = std::clamp(options.batch_size, 1, 8);
        options.script = std::clamp(options.script, 0, 6);
        options.upscale = std::clamp(options.upscale, 1.f, 4.f);
        options.overlap = std::clamp(options.overlap, 0, 256);
        options.loops = std::clamp(options.loops, 1, 32);
        options.final_strength = std::clamp(options.final_strength, 0.f, 1.f);
        options.image_cfg = std::clamp(options.image_cfg, 0.f, 10.f);
        return;
    }
    in >> options.strength >> options.resize_mode >> options.mask_blur >> options.only_masked >>
        options.padding >> options.fill >> brush_ >> opacity_ >> hardness_;
    options.strength = std::clamp(options.strength, 0.f, 1.f);
    options.resize_mode = std::clamp(options.resize_mode, 0, 2);
    options.mask_blur = std::clamp(options.mask_blur, 0, 64);
    options.padding = std::clamp(options.padding, 0, 256);
    options.fill = std::clamp(options.fill, 0, 3);
    brush_ = std::clamp(brush_, 1.f, 512.f);
    opacity_ = std::clamp(opacity_, .01f, 1.f);
    hardness_ = std::clamp(hardness_, 0.f, 1.f);
}
