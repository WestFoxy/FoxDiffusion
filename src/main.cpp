#include "app.hpp"
#include "cuda_runtime.hpp"
#include "app_log.hpp"

#ifdef _WIN32
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    // All implicit library paths (including ImGui logs) are portable.
    std::filesystem::current_path(executable_directory());
    app_log::Capture capture;
    App app;
    if (!app.initialize()) return 1;
    app.run();
    app.shutdown();
    return 0;
}
