#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>
#include <climits>

inline size_t checked_image_bytes(int w, int h) {
    if (w <= 0 || h <= 0 || uint64_t(w) * uint64_t(h) > uint64_t(INT_MAX) / 4)
        throw std::runtime_error("Image dimensions exceed the image codec's addressable RGBA buffer (2 GiB)");
    return size_t(w) * h * 4;
}
void require_image_memory(size_t bytes);

struct Raster {
    int w = 0, h = 0;
    std::vector<std::uint8_t> pixels;
    bool empty() const {
        return w <= 0 || h <= 0 || pixels.size() != size_t(w) * h * 4;
    }
    Raster() = default;
    Raster(int width, int height, std::uint8_t value = 255) : w(width), h(height) {
        const auto bytes = checked_image_bytes(w, h);
        require_image_memory(bytes);
        pixels.assign(bytes, value);
        for (size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
    }
    std::uint8_t *at(int x, int y) {
        return pixels.data() + (size_t(y) * w + x) * 4;
    }
    const std::uint8_t *at(int x, int y) const {
        return pixels.data() + (size_t(y) * w + x) * 4;
    }
};
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};
Raster resize_raster(const Raster &src, int w, int h, int mode = 0);
Raster crop_raster(const Raster &src, Rect rect);
Raster blur_mask(const Raster &src, int radius);
Rect mask_bounds(const Raster &mask, int padding, float aspect);
void composite_raster(Raster &dst, const Raster &image, const Raster &mask, Rect rect);
void paint_segment(Raster &image, float x0, float y0, float x1, float y1, float radius, float opacity,
                   float hardness, const float *color);
std::vector<int> tile_positions(int length, int tile, int overlap);
void raster_label(Raster &image, const std::string &text, int x, int y, int width, int height);
