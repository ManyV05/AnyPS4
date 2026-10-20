// AnyPS4 adapter: the renderer without shadPS4's presenter. A Vulkan instance,
// one scheduler and the rasterizer are created on the first submit. When the
// Win32 window of libSceVideoOut.prx exists, a swapchain is created on it and
// each flip blits the video-out image straight into it; otherwise (or with
// ORBIS_GNM_READBACK=1) the image is read back and handed to the window's GDI path.
#include "video_core/renderer_vulkan/vk_common.h"
#include <vk_mem_alloc.h>
#include <windows.h>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>
#include "common/logging/log.h"
#include "common/path_util.h"
#include <cstdio>
#include <cstdlib>
#include "core/emulator_settings.h"
#include "core/libraries/videoout/buffer.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/texture_cache/image_view.h"
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

bool ReadbackFlip(const Libraries::VideoOut::BufferAttribute& attribute, VAddr address, ReadbackFrame& out);

namespace {

using WindowHandleFunction = void* (*)();

HWND VideoOutWindow() {
    if (std::getenv("ORBIS_GNM_READBACK") != nullptr) {
        return nullptr;
    }
    const HMODULE module = GetModuleHandleW(L"libSceVideoOut.prx");
    const auto function = module != nullptr ? reinterpret_cast<WindowHandleFunction>(GetProcAddress(module, "OrbisVideoOutWindowHandle")) : nullptr;
    return function != nullptr ? static_cast<HWND>(function()) : nullptr;
}

struct Renderer {
    explicit Renderer(AmdGpu::Liverpool* liverpool, HWND hwnd)
        : window{hwnd}, instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
                                 EmulatorSettings.IsVkCrashDiagnosticEnabled()},
          scheduler{instance}, rasterizer{std::make_unique<Vulkan::Rasterizer>(instance, scheduler, liverpool)} {
        if (hwnd != nullptr) {
            swapchain = std::make_unique<Vulkan::Swapchain>(instance, window);
            pp_pass.Create(instance.GetDevice(), swapchain->GetSurfaceFormat().format);
        }
    }

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
    std::unique_ptr<Vulkan::Swapchain> swapchain;
    Vulkan::HostPasses::PostProcessingPass pp_pass;
    VkBuffer download_buffer{VK_NULL_HANDLE};
    VmaAllocation download_allocation{};
    void* download_mapped{};
    u64 download_size{};
    std::vector<u8> pixels;
    u64 flips{};
    u64 readbacks{};
};

std::unique_ptr<Renderer> renderer;
std::once_flag renderer_once;
bool renderer_failed = false;

vk::Format FrameViewFormat(const Libraries::VideoOut::PixelFormat format, const vk::Format image_format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        return image_format;
    }
}

VideoCore::Image& FindVideoOutImage(Renderer& r, const Libraries::VideoOut::BufferAttribute& attribute, VAddr address) {
    const Libraries::VideoOut::BufferAttributeGroup group{true, attribute};
    auto& texture_cache = r.rasterizer->GetTextureCache();
    VideoCore::TextureCache::ImageDesc desc{group, address};
    const auto image_id = texture_cache.FindImage(desc);
    texture_cache.UpdateImage(image_id);
    return texture_cache.GetImage(image_id);
}

} // namespace

void EnsureRenderer(AmdGpu::Liverpool* liverpool) {
    std::call_once(renderer_once, [liverpool] {
        if (std::getenv("ORBIS_GNM_HEADLESS") != nullptr) {
            LOG_WARNING(Render_Vulkan, "ORBIS_GNM_HEADLESS set: draws and dispatches stay skipped");
            renderer_failed = true;
            return;
        }
        const HWND hwnd = VideoOutWindow();
        try {
            renderer = std::make_unique<Renderer>(liverpool, hwnd);
            const auto properties = renderer->instance.GetPhysicalDevice().getProperties();
            LOG_INFO(Render_Vulkan, "renderer ready on {} (Vulkan {}.{}.{}), presentation {}: draws and dispatches execute from now on",
                     std::string_view{properties.deviceName}, VK_API_VERSION_MAJOR(properties.apiVersion),
                     VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
                     renderer->swapchain ? "through a swapchain on the title window" : "by readback");
            if (renderer->swapchain) {
                LOG_INFO(Render_Vulkan, "swapchain {}x{} {} ({} images)", renderer->swapchain->GetWidth(), renderer->swapchain->GetHeight(),
                         vk::to_string(renderer->swapchain->GetSurfaceFormat().format), renderer->swapchain->GetImageCount());
            }
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

bool PresentFlip(const Libraries::VideoOut::BufferAttribute& attribute, VAddr address) {
    if (renderer == nullptr || !renderer->swapchain) {
        return false;
    }
    auto& r = *renderer;
    auto& swapchain = *r.swapchain;
    try {
        auto& image = FindVideoOutImage(r, attribute, address);
        const u32 width = image.info.size.width;
        const u32 height = image.info.size.height;
        if (width == 0 || height == 0) {
            return false;
        }
        const u32 window_width = static_cast<u32>(r.window.GetWidth());
        const u32 window_height = static_cast<u32>(r.window.GetHeight());
        if (window_width != swapchain.GetWidth() || window_height != swapchain.GetHeight()) {
            swapchain.Recreate(window_width, window_height);
        }
        if (!swapchain.AcquireNextImage()) {
            swapchain.Recreate(window_width, window_height);
            if (!swapchain.AcquireNextImage()) {
                return false;
            }
        }
        VideoCore::ImageViewInfo view_info{};
        view_info.format = FrameViewFormat(attribute.pixel_format, image.info.pixel_format);
        view_info.mapping.a = vk::ComponentSwizzle::eOne;
        auto& view = image.FindView(view_info);
        r.scheduler.EndRendering();
        const auto cmdbuf = r.scheduler.CommandBuffer();
        image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {}, cmdbuf);
        const vk::Image target = swapchain.Image();
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const vk::ImageMemoryBarrier2 to_attachment{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = vk::AccessFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = target,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_attachment});
        const vk::Extent2D extent = swapchain.GetExtent();
        Vulkan::Frame frame{
            .width = extent.width,
            .height = extent.height,
            .image = target,
            .image_view = swapchain.ImageView(),
        };
        r.pp_pass.Render(cmdbuf, *view.image_view, vk::Extent2D{width, height}, frame, Vulkan::HostPasses::PostProcessingPass::Settings{});
        const vk::ImageMemoryBarrier2 to_present{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput | vk::PipelineStageFlagBits2::eFragmentShader,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
            .dstAccessMask = vk::AccessFlagBits2::eNone,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .image = target,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_present});
        Vulkan::SubmitInfo info{};
        info.AddWait(swapchain.GetImageAcquiredSemaphore());
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        r.scheduler.Flush(info);
        {
            std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
            if (!swapchain.Present()) {
                swapchain.Recreate(window_width, window_height);
            }
        }
        if (++r.flips == 1 || r.flips % 600 == 0) {
            LOG_INFO(Render_Vulkan, "flip #{} presented: {}x{} {} viewed as {} from image {:#x} into the {}x{} {} swapchain", r.flips, width,
                     height, vk::to_string(image.info.pixel_format), vk::to_string(view_info.format), address, extent.width, extent.height,
                     vk::to_string(swapchain.GetSurfaceFormat().format));
        }
        static const u64 dump_flip = [] { const char* v = std::getenv("ORBIS_GNM_DUMP_FRAME"); return v != nullptr ? std::strtoull(v, nullptr, 10) : 0; }();
        static const u64 dump_every = [] { const char* v = std::getenv("ORBIS_GNM_DUMP_EVERY"); return v != nullptr ? std::strtoull(v, nullptr, 10) : 0; }();
        if ((dump_flip != 0 && r.flips == dump_flip) || (dump_every != 0 && r.flips % dump_every == 0)) {
            ReadbackFrame frame{};
            if (ReadbackFlip(attribute, address, frame)) {
                const auto path = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir) / fmt::format("frame-{}.ppm", r.flips);
                if (FILE* file = std::fopen(path.string().c_str(), "wb")) {
                    std::fprintf(file, "P6\n%u %u\n255\n", frame.width, frame.height);
                    const auto* px = static_cast<const u8*>(frame.pixels);
                    for (u64 i = 0; i < static_cast<u64>(frame.width) * frame.height; ++i) {
                        const u8 r0 = frame.swap_red_blue ? px[i * 4 + 0] : px[i * 4 + 2];
                        const u8 b0 = frame.swap_red_blue ? px[i * 4 + 2] : px[i * 4 + 0];
                        const u8 rgb[3] = {r0, px[i * 4 + 1], b0};
                        std::fwrite(rgb, 1, 3, file);
                    }
                    std::fclose(file);
                    LOG_INFO(Render_Vulkan, "flip #{} dumped to {}", r.flips, path.string());
                }
            }
        }
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR(Render_Vulkan, "direct presentation failed, falling back to readback: {}", error.what());
        r.swapchain.reset();
        return false;
    }
}

bool ReadbackFlip(const Libraries::VideoOut::BufferAttribute& attribute, VAddr address, ReadbackFrame& out) {
    if (renderer == nullptr) {
        return false;
    }
    auto& r = *renderer;
    try {
        auto& image = FindVideoOutImage(r, attribute, address);
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
        out.swap_red_blue = attribute.pixel_format == Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb;
        if (++r.readbacks == 1 || r.readbacks % 600 == 0) {
            LOG_INFO(Render_Vulkan, "flip #{} read back: {}x{} {} from image {:#x}", r.readbacks, width, height, vk::to_string(format), address);
        }
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR(Render_Vulkan, "flip readback failed: {}", error.what());
        return false;
    }
}

} // namespace AnyPs4
