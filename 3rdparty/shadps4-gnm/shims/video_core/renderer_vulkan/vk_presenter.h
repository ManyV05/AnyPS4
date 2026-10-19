// AnyPS4 shim: gnmdriver declares a presenter it never creates; the host passes need the Frame they render into.
#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include <vk_mem_alloc.h>

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
class Liverpool;
}

namespace Vulkan {

struct Frame {
    u32 width;
    u32 height;
    vk::Image image;
    vk::ImageView image_view;
    VmaAllocation allocation{};
    vk::Fence present_done{};
    vk::Semaphore ready_semaphore{};
    u64 ready_tick{};
    bool is_hdr{false};
    u8 id{};
};

class Presenter {
public:
    Presenter(Frontend::WindowSDL&, AmdGpu::Liverpool*) {}
};

} // namespace Vulkan
