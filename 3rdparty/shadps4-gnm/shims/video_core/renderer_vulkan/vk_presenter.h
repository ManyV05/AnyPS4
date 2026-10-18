// AnyPS4 shim: gnmdriver declares a presenter it never creates in headless mode.
#pragma once

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
class Liverpool;
}

namespace Vulkan {

class Presenter {
public:
    Presenter(Frontend::WindowSDL&, AmdGpu::Liverpool*) {}
};

} // namespace Vulkan
