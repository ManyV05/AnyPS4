// AnyPS4 shim: no SDL. The window belongs to libSceVideoOut.prx; this object carries
// its HWND for the Vulkan surface, or reports a headless system when there is none.
#pragma once

#include <windows.h>
#include "common/types.h"

namespace Frontend {

enum class WindowSystemType : u8 {
    Headless,
    Windows,
    X11,
    Wayland,
    Metal,
};

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    explicit WindowSDL(HWND window_ = nullptr) : window{window_} {}
    s32 GetWidth() const { return Client().right; }
    s32 GetHeight() const { return Client().bottom; }
    bool IsOpen() const { return window == nullptr || IsWindow(window); }
    WindowSystemInfo GetWindowInfo() const {
        WindowSystemInfo info{};
        if (window != nullptr) {
            info.render_surface = window;
            info.type = WindowSystemType::Windows;
        }
        return info;
    }
    HWND Handle() const { return window; }

private:
    RECT Client() const {
        RECT rect{0, 0, 1920, 1080};
        if (window != nullptr) GetClientRect(window, &rect);
        if (rect.right <= 0) rect.right = 1;
        if (rect.bottom <= 0) rect.bottom = 1;
        return rect;
    }
    HWND window;
};

} // namespace Frontend
