#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

struct sd_image_t { std::uint32_t width, height, channel; std::uint8_t* data; };
using preview_cb_t = void (*)(int, int, sd_image_t*, bool, void*);
using set_preview_t = void (*)(preview_cb_t, int, int, bool, bool, void*);

namespace {
std::filesystem::path output_path;

#pragma pack(push, 1)
struct BmpHeader {
    std::uint16_t signature = 0x4D42;
    std::uint32_t file_size;
    std::uint32_t reserved = 0;
    std::uint32_t data_offset = 54;
    std::uint32_t info_size = 40;
    std::int32_t width;
    std::int32_t height;
    std::uint16_t planes = 1;
    std::uint16_t bits = 24;
    std::uint32_t compression = 0;
    std::uint32_t image_size;
    std::int32_t xppm = 2835, yppm = 2835;
    std::uint32_t colors = 0, important = 0;
};
#pragma pack(pop)

void preview_callback(int, int count, sd_image_t* images, bool, void*) {
    if (count < 1 || !images || !images[0].data || images[0].channel < 3 || output_path.empty()) return;
    const auto& image = images[0];
    const std::uint32_t stride = (image.width * 3 + 3) & ~3u;
    BmpHeader header{};
    header.width = static_cast<std::int32_t>(image.width);
    header.height = static_cast<std::int32_t>(image.height);
    header.image_size = stride * image.height;
    header.file_size = header.data_offset + header.image_size;
    const auto temporary = output_path.wstring() + L".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    std::string row(stride, '\0');
    for (std::int32_t y = static_cast<std::int32_t>(image.height) - 1; y >= 0; --y) {
        const auto* source = image.data + static_cast<std::size_t>(y) * image.width * image.channel;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            row[x * 3] = static_cast<char>(source[x * image.channel + 2]);
            row[x * 3 + 1] = static_cast<char>(source[x * image.channel + 1]);
            row[x * 3 + 2] = static_cast<char>(source[x * image.channel]);
        }
        out.write(row.data(), stride);
    }
    out.close();
    MoveFileExW(temporary.c_str(), output_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void initialize(HMODULE self) {
    wchar_t module_path[MAX_PATH]{};
    GetModuleFileNameW(self, module_path, MAX_PATH);
    const auto directory = std::filesystem::path(module_path).parent_path();
    std::ifstream config(directory / L"config" / L"preview-bridge.cfg");
    std::string path_utf8;
    int mode = 0, interval = 2, denoised = 1, noisy = 0;
    std::getline(config, path_utf8);
    config >> mode >> interval >> denoised >> noisy;
    if (mode <= 0 || path_utf8.empty()) return;
    const int wide_size = MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(wide_size > 0 ? wide_size : 0, L'\0');
    if (wide_size > 1) {
        MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, wide.data(), wide_size);
        wide.pop_back();
    }
    output_path = wide;
    HMODULE engine = nullptr;
    for (int i = 0; i < 100 && !engine; ++i) {
        engine = GetModuleHandleW(L"stable-diffusion.dll");
        if (!engine) Sleep(50);
    }
    if (!engine) return;
    auto set_preview = reinterpret_cast<set_preview_t>(GetProcAddress(engine, "sd_set_preview_callback"));
    if (set_preview) set_preview(preview_callback, mode, interval, denoised != 0, noisy != 0, nullptr);
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        std::thread(initialize, instance).detach();
    }
    return TRUE;
}
