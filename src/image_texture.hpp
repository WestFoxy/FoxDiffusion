#pragma once
#include "image_texture_types.hpp"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

inline void clear_image_texture(ImageTexture &texture) {
    for (auto &tile : texture.tiles)
        if (tile.id)
            glDeleteTextures(1, &tile.id);
    texture.tiles.clear();
    texture.width = texture.height = 0;
}
inline void allocate_image_texture(ImageTexture &texture, int w, int h) {
    if (texture.width == w && texture.height == h)
        return;
    clear_image_texture(texture);
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (limit <= 0)
        throw std::runtime_error("GPU texture size is unavailable");
    const int chunk = std::min(limit, 4096);
    while (glGetError() != GL_NO_ERROR) {
    }
    try {
        const int halo = 16, core = chunk - 2 * halo;
        for (int y = 0; y < h; y += core)
            for (int x = 0; x < w; x += core) {
                ImageTexture::Tile tile;
                tile.cx = x;
                tile.cy = y;
                tile.cw = std::min(core, w - x);
                tile.ch = std::min(core, h - y);
                tile.x = std::max(0, x - halo);
                tile.y = std::max(0, y - halo);
                tile.w = std::min(w, x + tile.cw + halo) - tile.x;
                tile.h = std::min(h, y + tile.ch + halo) - tile.y;
                glGenTextures(1, &tile.id);
                texture.tiles.push_back(tile);
                glBindTexture(GL_TEXTURE_2D, tile.id);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tile.w, tile.h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                             nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, 0x812F);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, 0x812F);
                if (glGetError() != GL_NO_ERROR)
                    throw std::runtime_error(
                        "Not enough GPU memory to display this image at full resolution");
            }
        texture.width = w;
        texture.height = h;
    } catch (...) {
        clear_image_texture(texture);
        throw;
    }
}
inline void finish_image_texture(ImageTexture &texture) {
    using Generate = void(APIENTRY *)(GLenum);
    static auto generate = reinterpret_cast<Generate>(glfwGetProcAddress("glGenerateMipmap"));
    for (auto &tile : texture.tiles)
        if (tile.dirty) {
            glBindTexture(GL_TEXTURE_2D, tile.id);
            if (generate)
                generate(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            generate ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            tile.dirty = false;
        }
}
inline void update_image_texture_region(ImageTexture &texture, const std::uint8_t *pixels, int x, int y,
                                        int w, int h) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
    for (auto &tile : texture.tiles) {
        int left = std::max(x, tile.x), top = std::max(y, tile.y);
        int right = std::min(x + w, tile.x + tile.w), bottom = std::min(y + h, tile.y + tile.h);
        if (right <= left || bottom <= top)
            continue;
        glBindTexture(GL_TEXTURE_2D, tile.id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, left - tile.x, top - tile.y, right - left, bottom - top, GL_RGBA,
                        GL_UNSIGNED_BYTE, pixels + (size_t(top - y) * w + left - x) * 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        tile.dirty = true;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}
inline void upload_image_texture(ImageTexture &texture, const std::uint8_t *pixels, int w, int h) {
    allocate_image_texture(texture, w, h);
    update_image_texture_region(texture, pixels, 0, 0, w, h);
    finish_image_texture(texture);
}
inline void draw_image_texture(const ImageTexture &texture, ImDrawList *draw, ImVec2 origin, float scale) {
    for (const auto &tile : texture.tiles) {
        const ImVec2 a(origin.x + tile.cx * scale, origin.y + tile.cy * scale),
            b(a.x + tile.cw * scale, a.y + tile.ch * scale);
        const auto clip0 = draw->GetClipRectMin(), clip1 = draw->GetClipRectMax();
        if (b.x < clip0.x || b.y < clip0.y || a.x > clip1.x || a.y > clip1.y)
            continue;
        draw->AddImage(
            static_cast<ImTextureID>(tile.id), a, b,
            ImVec2(float(tile.cx - tile.x) / tile.w, float(tile.cy - tile.y) / tile.h),
            ImVec2(float(tile.cx + tile.cw - tile.x) / tile.w, float(tile.cy + tile.ch - tile.y) / tile.h));
    }
}
