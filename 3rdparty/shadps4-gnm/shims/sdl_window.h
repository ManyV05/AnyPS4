// AnyPS4 shim: no SDL window. The Vulkan instance runs headless; presentation is a
// readback into libSceVideoOut.prx's Win32 window (adapter/Renderer.cpp).
#pragma once

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
    WindowSDL(s32 width_ = 1920, s32 height_ = 1080) : width{width_}, height{height_} {}
    s32 GetWidth() const { return width; }
    s32 GetHeight() const { return height; }
    bool IsOpen() const { return true; }
    WindowSystemInfo GetWindowInfo() const { return {}; }

private:
    s32 width;
    s32 height;
};

} // namespace Frontend
