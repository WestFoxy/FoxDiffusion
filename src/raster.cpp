#include "raster.hpp"
#include <windows.h>

void require_image_memory(size_t bytes) {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        const auto available = std::min(memory.ullAvailPhys, memory.ullAvailPageFile);
        constexpr size_t reserve = 256ULL * 1024 * 1024;
        if (bytes > available || available - bytes < reserve)
            throw std::runtime_error(
                "Not enough available RAM for this image operation (needs approximately " +
                std::to_string((bytes + 1048575) / 1048576) + " MiB plus working headroom)");
    }
}

Raster resize_raster(const Raster &src, int w, int h, int mode) {
    if (src.empty())
        throw std::runtime_error("No source image");
    if (src.w == w && src.h == h)
        return src;
    Raster out(w, h);
    float sx = float(src.w) / w, sy = float(src.h) / h;
    if (mode == 1)
        sx = sy = std::min(sx, sy); // crop to fill
    if (mode == 2)
        sx = sy = std::max(sx, sy); // fit, extend edge pixels
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float px = std::clamp((x + .5f - w * .5f) * sx + src.w * .5f - .5f, 0.f, float(src.w - 1));
            const float py = std::clamp((y + .5f - h * .5f) * sy + src.h * .5f - .5f, 0.f, float(src.h - 1));
            const int ix = int(px), iy = int(py);
            const float ax = px - ix, ay = py - iy;
            for (int c = 0; c < 4; ++c)
                out.at(x, y)[c] = std::uint8_t(std::clamp(
                    (1 - ay) *
                            ((1 - ax) * src.at(ix, iy)[c] + ax * src.at(std::min(ix + 1, src.w - 1), iy)[c]) +
                        ay * ((1 - ax) * src.at(ix, std::min(iy + 1, src.h - 1))[c] +
                              ax * src.at(std::min(ix + 1, src.w - 1), std::min(iy + 1, src.h - 1))[c]),
                    0.f, 255.f));
        }
    return out;
}
Raster crop_raster(const Raster &src, Rect r) {
    if (src.empty() || r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 || r.x + r.w > src.w || r.y + r.h > src.h)
        throw std::runtime_error("Invalid crop");
    Raster out(r.w, r.h);
    for (int y = 0; y < r.h; ++y)
        std::copy_n(src.at(r.x, r.y + y), size_t(r.w) * 4, out.at(0, y));
    return out;
}
Raster blur_mask(const Raster &src, int radius) {
    if (radius <= 0)
        return src;
    // Three separable box passes approximate a Gaussian without a quadratic kernel.
    Raster a = src, b = src;
    const int r = std::clamp(int(std::round(radius * .58)), 1, 128);
    for (int pass = 0; pass < 3; ++pass)
        for (int axis = 0; axis < 2; ++axis) {
            const int outer = axis ? src.w : src.h, inner = axis ? src.h : src.w;
            for (int o = 0; o < outer; ++o) {
                auto p = [&](int i) {
                    return axis ? a.at(o, std::clamp(i, 0, inner - 1)) : a.at(std::clamp(i, 0, inner - 1), o);
                };
                int sum = 0;
                for (int i = -r; i <= r; ++i)
                    sum += p(i)[0];
                for (int i = 0; i < inner; ++i) {
                    auto q = axis ? b.at(o, i) : b.at(i, o);
                    q[0] = q[1] = q[2] = std::uint8_t(sum / (2 * r + 1));
                    q[3] = 255;
                    sum += p(i + r + 1)[0] - p(i - r)[0];
                }
            }
            std::swap(a, b);
        }
    return a;
}
Rect mask_bounds(const Raster &mask, int padding, float aspect) {
    int x0 = mask.w, y0 = mask.h, x1 = -1, y1 = -1;
    for (int y = 0; y < mask.h; ++y)
        for (int x = 0; x < mask.w; ++x)
            if (mask.at(x, y)[0]) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    if (x1 < 0)
        throw std::runtime_error("The mask is empty. Paint or select an area first.");
    x0 = std::max(0, x0 - padding);
    y0 = std::max(0, y0 - padding);
    x1 = std::min(mask.w, x1 + padding + 1);
    y1 = std::min(mask.h, y1 + padding + 1);
    int w = x1 - x0, h = y1 - y0;
    if (float(w) / h < aspect)
        w = std::min(mask.w, int(std::ceil(h * aspect)));
    else
        h = std::min(mask.h, int(std::ceil(w / aspect)));
    return {std::clamp((x0 + x1 - w) / 2, 0, mask.w - w), std::clamp((y0 + y1 - h) / 2, 0, mask.h - h), w, h};
}
void composite_raster(Raster &dst, const Raster &image, const Raster &mask, Rect r) {
    if (image.empty() || mask.empty() || r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x + r.w > dst.w ||
        r.y + r.h > dst.h)
        throw std::runtime_error("Invalid composite bounds");
    Raster scaled, alpha;
    if (image.w != r.w || image.h != r.h)
        scaled = resize_raster(image, r.w, r.h);
    if (mask.w != r.w || mask.h != r.h)
        alpha = resize_raster(mask, r.w, r.h);
    const Raster &colors = scaled.empty() ? image : scaled;
    const Raster &coverage = alpha.empty() ? mask : alpha;
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
            const int a = coverage.at(x, y)[0];
            if (a == 0)
                continue;
            auto d = dst.at(r.x + x, r.y + y);
            auto s = colors.at(x, y);
            for (int c = 0; c < 3; ++c)
                d[c] = std::uint8_t((s[c] * a + d[c] * (255 - a) + 127) / 255);
            d[3] = 255;
        }
}
void paint_segment(Raster &image, float x0, float y0, float x1, float y1, float radius, float opacity,
                   float hardness, const float *color) {
    radius = std::max(.5f, radius);
    const float dx = x1 - x0, dy = y1 - y0, len = dx * dx + dy * dy;
    for (int y = std::max(0, int(std::floor(std::min(y0, y1) - radius)));
         y < std::min(image.h, int(std::ceil(std::max(y0, y1) + radius))); ++y)
        for (int x = std::max(0, int(std::floor(std::min(x0, x1) - radius)));
             x < std::min(image.w, int(std::ceil(std::max(x0, x1) + radius))); ++x) {
            const float t = len ? std::clamp(((x + .5f - x0) * dx + (y + .5f - y0) * dy) / len, 0.f, 1.f) : 0;
            const float dist = std::hypot(x + .5f - x0 - t * dx, y + .5f - y0 - t * dy) / radius;
            const float a = opacity * std::clamp((1 - dist) / std::max(.01f, 1 - hardness), 0.f, 1.f);
            auto p = image.at(x, y);
            for (int c = 0; c < 3; ++c)
                p[c] = std::uint8_t(std::clamp(p[c] * (1 - a) + color[c] * 255 * a, 0.f, 255.f));
        }
}
std::vector<int> tile_positions(int length, int tile, int overlap) {
    if (length <= 0 || tile <= 0 || overlap < 0 || overlap >= tile)
        throw std::runtime_error("Overlap must be smaller than the tile");
    std::vector<int> out{0};
    while (out.back() + tile < length)
        out.push_back(std::min(out.back() + tile - overlap, length - tile));
    return out;
}

void raster_label(Raster &image, const std::string &text, int x, int y, int width, int height) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) {
        DeleteDC(dc);
        return;
    }
    auto old = SelectObject(dc, bitmap);
    RECT rect{0, 0, width, height};
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    HFONT font =
        CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    auto oldfont = SelectObject(dc, font);
    SetTextColor(dc, RGB(235, 235, 245));
    SetBkMode(dc, TRANSPARENT);
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
    DrawTextW(dc, wide.c_str(), size, &rect, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    GdiFlush();
    for (int py = 0; py < height && y + py < image.h; ++py)
        for (int px = 0; px < width && x + px < image.w; ++px) {
            if (x + px < 0 || y + py < 0)
                continue;
            auto p = static_cast<unsigned char *>(bits) + (size_t(py) * width + px) * 4;
            auto q = image.at(x + px, y + py);
            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
            q[3] = 255;
        }
    SelectObject(dc, oldfont);
    DeleteObject(font);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
}
