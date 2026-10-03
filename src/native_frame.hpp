#pragma once

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windowsx.h>
#include <functional>

// Keep WS_THICKFRAME and the system move/size loop. Only replace the caption.
class NativeFrame {
public:
    HWND hwnd = nullptr;
    WNDPROC previous = nullptr;
    std::function<void()> repaint;
    RECT drag{};
    bool drawing = false;
    bool resizing = false;
    GLFWwindow* glfw_window = nullptr;
    static constexpr UINT_PTR resize_timer = 0xF0D1;

    void install(GLFWwindow* window, std::function<void()> callback) {
        hwnd = glfwGetWin32Window(window);
        glfw_window = window;
        repaint = std::move(callback);
        SetPropW(hwnd, L"FoxNativeFrame", this);
        previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(procedure)));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    void remove() {
        if (!hwnd) return;
        KillTimer(hwnd, resize_timer);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
        RemovePropW(hwnd, L"FoxNativeFrame");
        hwnd = nullptr;
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = static_cast<NativeFrame*>(GetPropW(window, L"FoxNativeFrame"));
        if (!self) return DefWindowProcW(window, message, w, l);
        switch (message) {
        case WM_NCCALCSIZE:
            if (w) {
                auto& rect = reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0];
                const RECT original = rect;
                CallWindowProcW(self->previous, window, message, w, l);
                rect.top = original.top;
                if (IsZoomed(window)) {
                    MONITORINFO info{sizeof(info)};
                    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info))
                        rect.top = info.rcWork.top;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            const auto result = DefWindowProcW(window, message, w, l);
            if (result != HTCLIENT && result != HTCAPTION && result != HTMINBUTTON &&
                result != HTMAXBUTTON && result != HTCLOSE && result != HTSYSMENU)
                return result;
            POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(window, &point);
            const UINT dpi = GetDpiForWindow(window);
            const int border = GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            if (!IsZoomed(window) && point.y < border) {
                RECT client{}; GetClientRect(window, &client);
                if (point.x < border) return HTTOPLEFT;
                if (point.x >= client.right - border) return HTTOPRIGHT;
                return HTTOP;
            }
            if (PtInRect(&self->drag, point) && ImGui::GetCurrentContext() &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_ENTERSIZEMOVE:
            self->resizing = true;
            // SwapBuffers must not block the system move/size message loop.
            glfwMakeContextCurrent(self->glfw_window);
            glfwSwapInterval(0);
            SetTimer(window, resize_timer, USER_TIMER_MINIMUM, nullptr);
            break;
        case WM_EXITSIZEMOVE:
            KillTimer(window, resize_timer);
            self->resizing = false;
            glfwMakeContextCurrent(self->glfw_window);
            glfwSwapInterval(1);
            break;
        case WM_SIZE: {
            // GLFW updates the framebuffer dimensions before we render the new size.
            const auto result = CallWindowProcW(self->previous, window, message, w, l);
            if (self->resizing && w != SIZE_MINIMIZED && !self->drawing && self->repaint)
                self->repaint();
            return result;
        }
        case WM_TIMER:
            if (w == resize_timer) {
                if (!self->drawing && self->repaint) self->repaint();
                return 0;
            }
            break;
        }
        return CallWindowProcW(self->previous, window, message, w, l);
    }
};
