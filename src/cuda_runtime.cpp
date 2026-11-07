#include "cuda_runtime.hpp"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <zip.h>
#include <array>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
constexpr int required_driver = 12080;
constexpr wchar_t archive_path[] =
    L"/compute/cuda/redist/libcublas/windows-x86_64/libcublas-windows-x86_64-12.8.4.1-archive.zip";
constexpr char archive_hash[] = "57a470112cec7e112c95253dde8b3c7184d795dbd92b0bde77a4cb7f8c94c8aa";
constexpr uint64_t archive_size = 563660944;
struct Driver {
    HMODULE module = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    int version = 0;
    Driver() {
        if (module) {
            auto get = reinterpret_cast<int(WINAPI *)(int *)>(GetProcAddress(module, "cuDriverGetVersion"));
            if (!get || get(&version) != 0)
                version = 0;
        }
    }
    // Resident for delayed imports. Querying the version does not initialize CUDA.
};
Driver &driver() {
    static Driver instance;
    return instance;
}
void check(std::stop_token stop) {
    if (stop.stop_requested())
        throw std::runtime_error("Cancelled");
}
struct Internet {
    HINTERNET handle = nullptr;
    ~Internet() {
        if (handle)
            WinHttpCloseHandle(handle);
    }
    operator HINTERNET() const {
        return handle;
    }
};
std::string sha256(const std::filesystem::path &path, std::stop_token stop) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    struct Cleanup {
        BCRYPT_ALG_HANDLE &a;
        BCRYPT_HASH_HANDLE &h;
        ~Cleanup() {
            if (h)
                BCryptDestroyHash(h);
            if (a)
                BCryptCloseAlgorithmProvider(a, 0);
        }
    } cleanup{alg, hash};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot initialize SHA-256");
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read CUDA archive");
    std::array<char, 1024 * 64> buffer;
    while (in) {
        check(stop);
        in.read(buffer.data(), buffer.size());
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(in.gcount()), 0) < 0)
            throw std::runtime_error("SHA-256 failed");
    }
    if (!in.eof())
        throw std::runtime_error("CUDA archive read failed");
    std::array<unsigned char, 32> bytes;
    if (BCryptFinishHash(hash, bytes.data(), ULONG(bytes.size()), 0) < 0)
        throw std::runtime_error("SHA-256 failed");
    std::string result;
    for (auto c : bytes) {
        result += "0123456789abcdef"[c >> 4];
        result += "0123456789abcdef"[c & 15];
    }
    return result;
}
void download(const std::filesystem::path &path, std::stop_token stop,
              const std::function<void(std::string)> &status) {
    status("Connecting to NVIDIA to download CUDA cuBLAS...");
    Internet session{
        WinHttpOpen(L"FoxDiffusion/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
    if (!session.handle)
        throw std::runtime_error("Cannot start CUDA download");
    WinHttpSetTimeouts(session, 15000, 15000, 15000, 15000);
    Internet connection{
        WinHttpConnect(session, L"developer.download.nvidia.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    Internet request{connection.handle ? WinHttpOpenRequest(connection, L"GET", archive_path, nullptr,
                                                            nullptr, nullptr, WINHTTP_FLAG_SECURE)
                                       : nullptr};
    if (!request.handle || !WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr))
        throw std::runtime_error("CUDA download failed. Check Internet access and retry.");
    DWORD code = 0, size = sizeof(code);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code,
                             &size, nullptr) ||
        code != 200)
        throw std::runtime_error("NVIDIA download returned HTTP " + std::to_string(code));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::array<char, 65536> bytes;
    uint64_t total = 0;
    int last = -1;
    for (;;) {
        check(stop);
        DWORD received = 0;
        if (!WinHttpReadData(request, bytes.data(), DWORD(bytes.size()), &received))
            throw std::runtime_error("CUDA download interrupted; retry to download again");
        if (!received)
            break;
        total += received;
        if (total > archive_size)
            throw std::runtime_error("Unexpected CUDA archive size");
        out.write(bytes.data(), received);
        if (!out)
            throw std::runtime_error("Cannot write CUDA cache; check free disk space");
        const int percent = int(total * 100 / archive_size);
        if (percent != last) {
            status("Downloading CUDA cuBLAS: " + std::to_string(percent) + "%");
            last = percent;
        }
    }
    out.close();
    if (!out || total != archive_size)
        throw std::runtime_error("Incomplete CUDA download; retry");
}
void extract(const std::filesystem::path &archive, const std::filesystem::path &directory,
             std::stop_token stop) {
    // Memory-map the verified archive: no ANSI path conversion, no full archive copy.
    HANDLE file =
        CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot open CUDA archive");
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const char *data =
        mapping ? static_cast<const char *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    struct Mapping {
        HANDLE f, m;
        const char *d;
        ~Mapping() {
            if (d)
                UnmapViewOfFile(d);
            if (m)
                CloseHandle(m);
            CloseHandle(f);
        }
    } mapped{file, mapping, data};
    if (!data)
        throw std::runtime_error("Cannot map CUDA archive");
    std::unique_ptr<zip_t, decltype(&zip_stream_close)> zip(zip_stream_open(data, archive_size, 0, 'r'),
                                                            zip_stream_close);
    if (!zip)
        throw std::runtime_error("Invalid CUDA ZIP archive");
    const std::array<const char *, 3> names = {"cublasLt64_12.dll", "cublas64_12.dll", "LICENSE"};
    for (const auto *name : names) {
        check(stop);
        const std::string entry = std::string("libcublas-windows-x86_64-12.8.4.1-archive/") +
                                  (std::string(name) == "LICENSE" ? "LICENSE" : std::string("bin/") + name);
        if (zip_entry_open(zip.get(), entry.c_str()) < 0)
            throw std::runtime_error("CUDA archive entry missing: " + entry);
        auto dest = directory / std::filesystem::u8path(name);
        auto temporary = dest;
        temporary += L".tmp";
        struct Writer {
            std::ofstream out;
            std::stop_token stop;
        };
        Writer writer{std::ofstream(temporary, std::ios::binary | std::ios::trunc), stop};
        const int result = zip_entry_extract(
            zip.get(),
            [](void *arg, unsigned long long, const void *bytes, size_t size) -> size_t {
                auto &w = *static_cast<Writer *>(arg);
                if (w.stop.stop_requested())
                    return 0;
                w.out.write(static_cast<const char *>(bytes), size);
                return w.out ? size : 0;
            },
            &writer);
        writer.out.close();
        zip_entry_close(zip.get());
        check(stop);
        if (result < 0 || !writer.out)
            throw std::runtime_error("Cannot extract CUDA DLL; check free disk space");
        if (!MoveFileExW(temporary.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot install CUDA runtime file");
    }
}
} // namespace

std::filesystem::path executable_directory() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!length || length >= path.size())
        throw std::runtime_error("Cannot resolve executable directory");
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}
bool cuda_driver_compatible() {
    return driver().version >= required_driver;
}
std::string cuda_version_label() {
    const int v = driver().version;
    return "CUDA [ nv " + (v ? std::to_string(v / 1000) + "." + std::to_string(v % 1000 / 10) : "--") +
           " | lib 12.8 ]";
}
std::string cuda_driver_status() {
    const int v = driver().version;
    if (!v)
        return "CUDA: NVIDIA driver not found; install a driver supporting CUDA 12.8 or newer";
    return "CUDA driver " + std::to_string(v / 1000) + "." + std::to_string(v % 1000 / 10) +
           (v < required_driver ? " - incompatible: CUDA 12.8 or newer required" : " | engine CUDA 12.8");
}
void ensure_cuda_runtime(std::stop_token stop, const std::function<void(std::string)> &status) {
    static std::mutex mutex;
    std::scoped_lock lock(mutex);
    static bool ready = false;
    check(stop);
    if (!cuda_driver_compatible())
        throw std::runtime_error(cuda_driver_status());
    if (ready)
        return;
    const auto compute_cache = executable_directory() / L"cache" / L"cuda";
    std::filesystem::create_directories(compute_cache);
    if (!SetEnvironmentVariableW(L"CUDA_CACHE_PATH", compute_cache.c_str()))
        throw std::runtime_error("Cannot configure portable CUDA cache");
    auto init = reinterpret_cast<int(WINAPI *)(unsigned)>(GetProcAddress(driver().module, "cuInit"));
    auto count = reinterpret_cast<int(WINAPI *)(int *)>(GetProcAddress(driver().module, "cuDeviceGetCount"));
    int devices = 0;
    if (!init || !count || init(0) != 0 || count(&devices) != 0 || devices < 1)
        throw std::runtime_error("No compatible NVIDIA GPU is available");
    const auto directory = executable_directory() / L"cuda-bin";
    std::filesystem::create_directories(directory);
    status("Checking local CUDA libraries...");
    if (!std::filesystem::is_regular_file(directory / L"cublas64_12.dll") ||
        !std::filesystem::is_regular_file(directory / L"cublasLt64_12.dll") ||
        sha256(directory / L"cublas64_12.dll", stop) !=
            "9513540e4ec4c51ee9e7304138c2cc255c29a8c181f9e80c38efa25738becd99" ||
        sha256(directory / L"cublasLt64_12.dll", stop) !=
            "b199d1ff892a81b7fd3d57ba1781549609b41500b36008fef326038393ad46c7") {
        const auto cache = executable_directory() / L"cache";
        std::filesystem::create_directories(cache);
        const auto archive = cache / L"cublas-12.8.4.1.zip";
        if (!std::filesystem::exists(archive) || std::filesystem::file_size(archive) != archive_size ||
            sha256(archive, stop) != archive_hash)
            download(archive, stop, status);
        status("Verifying CUDA download...");
        if (sha256(archive, stop) != archive_hash)
            throw std::runtime_error("CUDA SHA-256 mismatch; retry download");
        status("Extracting CUDA libraries...");
        extract(archive, directory, stop);
    }
    check(stop);
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS) ||
        !AddDllDirectory(directory.c_str()))
        throw std::runtime_error("Cannot configure CUDA DLL search directory");
    for (const auto *name : {L"cublasLt64_12.dll", L"cublas64_12.dll"}) {
        if (!LoadLibraryExW((directory / name).c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32 |
                                LOAD_LIBRARY_SEARCH_USER_DIRS))
            throw std::runtime_error("Cannot load local CUDA libraries (Windows error " +
                                     std::to_string(GetLastError()) +
                                     "). Remove incompatible DLLs from cuda-bin and retry.");
    }
    ready = true;
    status("Embedded CUDA engine ready");
}
