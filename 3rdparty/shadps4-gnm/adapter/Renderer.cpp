// AnyPS4 adapter: the renderer without shadPS4's presenter. A headless Vulkan
// instance, one scheduler and the rasterizer are created on the first submit;
// at flip time the video-out image is read back from the texture cache and
// handed to libSceVideoOut.prx, which owns the Win32 window.
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"
#include <vk_mem_alloc.h>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/videoout/buffer.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/texture_cache.h"

namespace AnyPs4 {

struct ReadbackFrame {
    const void* pixels;
    u32 width;
    u32 height;
    u32 stride;
    bool swap_red_blue;
};

namespace {

struct Renderer {
    explicit Renderer(AmdGpu::Liverpool* liverpool)
        : window{}, instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
                             EmulatorSettings.IsVkCrashDiagnosticEnabled()},
          scheduler{instance}, rasterizer{std::make_unique<Vulkan::Rasterizer>(instance, scheduler, liverpool)} {}

    ~Renderer() {
        ReleaseDownload();
    }

    void ReleaseDownload() {
        if (download_buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(instance.GetAllocator(), download_buffer, download_allocation);
            download_buffer = VK_NULL_HANDLE;
            download_allocation = nullptr;
            download_mapped = nullptr;
            download_size = 0;
        }
    }

    bool EnsureDownload(u64 size) {
        if (download_size >= size) {
            return true;
        }
        ReleaseDownload();
        VkBufferCreateInfo buffer_ci{};
        buffer_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_ci.size = size;
        buffer_ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &download_buffer, &download_allocation, &info) != VK_SUCCESS) {
            LOG_ERROR(Render_Vulkan, "cannot allocate the {} byte readback buffer", size);
            download_buffer = VK_NULL_HANDLE;
            return false;
        }
        download_mapped = info.pMappedData;
        download_size = size;
        return true;
    }

    Frontend::WindowSDL window;
    Vulkan::Instance instance;
    Vulkan::Scheduler scheduler;
    std::unique_ptr<Vulkan::Rasterizer> rasterizer;
    VkBuffer download_buffer{VK_NULL_HANDLE};
    VmaAllocation download_allocation{};
    void* download_mapped{};
    u64 download_size{};
    std::vector<u8> pixels;
    u64 flips{};
};

std::unique_ptr<Renderer> renderer;
std::once_flag renderer_once;
bool renderer_failed = false;

} // namespace

void EnsureRenderer(AmdGpu::Liverpool* liverpool) {
    std::call_once(renderer_once, [liverpool] {
        if (std::getenv("ORBIS_GNM_HEADLESS") != nullptr) {
            LOG_WARNING(Render_Vulkan, "ORBIS_GNM_HEADLESS set: draws and dispatches stay skipped");
            renderer_failed = true;
            return;
        }
        try {
            renderer = std::make_unique<Renderer>(liverpool);
            const auto properties = renderer->instance.GetPhysicalDevice().getProperties();
            LOG_INFO(Render_Vulkan, "renderer ready on {} (Vulkan {}.{}.{}): draws and dispatches execute from now on",
                     std::string_view{properties.deviceName}, VK_API_VERSION_MAJOR(properties.apiVersion),
                     VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion));
        } catch (const std::exception& error) {
            LOG_ERROR(Render_Vulkan, "renderer initialization failed, staying headless: {}", error.what());
            renderer.reset();
            renderer_failed = true;
        }
    });
}

bool RendererReady() {
    return renderer != nullptr;
}

bool ReadbackFlip(const Libraries::VideoOut::BufferAttribute& attribute, VAddr address, ReadbackFrame& out) {
    if (renderer == nullptr) {
        return false;
    }
    auto& r = *renderer;
    try {
        const Libraries::VideoOut::BufferAttributeGroup group{true, attribute};
        auto& texture_cache = r.rasterizer->GetTextureCache();
        VideoCore::TextureCache::ImageDesc desc{group, address};
        const auto image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
        auto& image = texture_cache.GetImage(image_id);
        const u32 width = image.info.size.width;
        const u32 height = image.info.size.height;
        const u64 size = static_cast<u64>(width) * height * 4;
        if (width == 0 || height == 0 || !r.EnsureDownload(size)) {
            return false;
        }
        r.scheduler.EndRendering();
        const auto cmdbuf = r.scheduler.CommandBuffer();
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {}, cmdbuf);
        const vk::BufferImageCopy copy{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, 1},
        };
        cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, vk::Buffer{r.download_buffer}, copy);
        r.scheduler.Finish();
        r.pixels.resize(size);
        std::memcpy(r.pixels.data(), r.download_mapped, size);
        const auto format = image.info.pixel_format;
        out.pixels = r.pixels.data();
        out.width = width;
        out.height = height;
        out.stride = width * 4;
        out.swap_red_blue = format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eR8G8B8A8Unorm ||
                            format == vk::Format::eA8B8G8R8SrgbPack32 || format == vk::Format::eA8B8G8R8UnormPack32;
        if (++r.flips == 1 || r.flips % 600 == 0) {
            LOG_INFO(Render_Vulkan, "flip #{} read back: {}x{} {} from image {:#x}", r.flips, width, height, vk::to_string(format),
                     address);
        }
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR(Render_Vulkan, "flip readback failed: {}", error.what());
        return false;
    }
}

} // namespace AnyPs4
