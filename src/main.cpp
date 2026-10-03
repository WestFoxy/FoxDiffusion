#include "app.hpp"

#ifdef _WIN32
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    App app;
    if (!app.initialize()) return 1;
    app.run();
    app.shutdown();
    return 0;
}
