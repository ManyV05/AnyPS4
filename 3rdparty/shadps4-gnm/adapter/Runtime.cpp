// AnyPS4 adapter: debugger state, settings, memory facade and the headless VideoOut port.
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/libraries/videoout/buffer.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/video_out.h"
#include "common/logging/log.h"
#include "core/platform.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <windows.h>
#include "prx/libc/include/GuestArena.hpp"

namespace AnyPs4 {
struct ReadbackFrame {
    const void* pixels;
    u32 width;
    u32 height;
    u32 stride;
    bool swap_red_blue;
};
bool RendererReady();
bool PresentFlip(const Libraries::VideoOut::BufferAttribute& attribute, VAddr address);
bool ReadbackFlip(const Libraries::VideoOut::BufferAttribute& attribute, VAddr address, ReadbackFrame& out);
} // namespace AnyPs4

extern "C" void OrbisHleTrace(const char* module, const char* function, const char* nid, std::uint64_t calls, const void* site);

DebugStateType::DebugStateImpl DebugState;
EmulatorSettingsImpl EmulatorSettings;

namespace Core {

namespace {

// The tessellation factor ring is placed near the end of the system reserved
// range on real hardware; the facade hands out a committed block of its own.
constexpr size_t ReservedSize = 0x10000000 + 0x1000000;
constexpr size_t ReservedBacking = 0x800000;

uintptr_t ReservedBase() {
    static const uintptr_t base = [] {
        void* block = VirtualAlloc(nullptr, ReservedBacking, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (block == nullptr) {
            LOG_CRITICAL(Core, "cannot allocate the system reserved block");
            return uintptr_t{0};
        }
        return reinterpret_cast<uintptr_t>(block) - 0x1000000;
    }();
    return base;
}

} // namespace

uintptr_t AddressSpace::SystemReservedVirtualBase() {
    return ReservedBase();
}

size_t AddressSpace::SystemReservedVirtualSize() {
    return ReservedSize;
}

namespace {

// AnyPS4: TryWriteBacking opens a protected page, copies and restores the protection it read; a
// page-manager protect landing in between was undone by the restore, leaving a read-only page with
// no watchers behind. Both take this lock (recursive: a fault inside the copy may re-enter Protect).
std::recursive_mutex protection_mutex;

} // namespace

void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    std::lock_guard lock{protection_mutex};
    DWORD protect = PAGE_NOACCESS;
    const bool read = True(perms & MemoryPermission::Read);
    const bool write = True(perms & MemoryPermission::Write);
    const bool exec = True(perms & MemoryPermission::Execute);
    if (write) {
        protect = exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    } else if (read) {
        protect = exec ? PAGE_EXECUTE_READ : PAGE_READONLY;
    } else if (exec) {
        protect = PAGE_EXECUTE;
    }
    VAddr cursor = virtual_addr;
    const VAddr end = virtual_addr + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof info) != sizeof info) {
            LOG_ERROR(Core, "VirtualQuery({:#x}) failed while protecting {:#x}+{:#x}", cursor, virtual_addr, size);
            return;
        }
        const VAddr region_end = std::min<VAddr>(end, reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize);
        DWORD previous = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(cursor), region_end - cursor, protect, &previous)) {
            LOG_ERROR(Core, "VirtualProtect({:#x}, {:#x}, {:#x}) failed: {}", cursor, region_end - cursor, protect, GetLastError());
            return;
        }
        cursor = region_end;
    }
}

u32 GuestPageProtection(VAddr address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info) == sizeof info
               ? (info.State == MEM_COMMIT ? info.Protect : 0xFFFFFFFFu)
               : 0xFFFFFFFEu;
}

std::string DescribeGuestPage(VAddr address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info) != sizeof info) {
        return "VirtualQuery failed";
    }
    std::uint32_t protection = 0;
    std::uint32_t aliases = 0;
    std::uint32_t pins = 0;
    std::uint32_t host_writes = 0;
    bool armed = false;
    const bool shared = GuestArena::GuestArenaDescribePage_nid_postfix(address, &protection, &aliases, &pins,
                                                                        &host_writes, &armed);
    char text[256];
    if (shared) {
        std::snprintf(text, sizeof text,
                      "state=%#lx protect=%#lx alloc_protect=%#lx type=%#lx | arena shared view: intended=%#x aliases=%u pins=%u "
                      "host_writes=%u armed=%d",
                      info.State, info.Protect, info.AllocationProtect, info.Type, protection, aliases, pins,
                      host_writes, armed ? 1 : 0);
    } else {
        std::snprintf(text, sizeof text,
                      "state=%#lx protect=%#lx alloc_protect=%#lx type=%#lx | arena: not a shared view",
                      info.State, info.Protect, info.AllocationProtect, info.Type);
    }
    return text;
}

namespace {

bool QueryRegion(VAddr address, MEMORY_BASIC_INFORMATION& info) {
    return VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof info) == sizeof info;
}

bool Committed(const MEMORY_BASIC_INFORMATION& info) {
    return info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0;
}

} // namespace

bool MemoryManager::IsValidMapping(VAddr virtual_addr, u64 size) {
    MEMORY_BASIC_INFORMATION info{};
    VAddr cursor = virtual_addr;
    const VAddr end = virtual_addr + (size == 0 ? 1 : size);
    while (cursor < end) {
        if (!QueryRegion(cursor, info) || !Committed(info)) {
            return false;
        }
        cursor = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    static constexpr u64 MinSizeToClamp = 1ull << 30;
    if (size < MinSizeToClamp) {
        return size;
    }
    MEMORY_BASIC_INFORMATION info{};
    VAddr cursor = virtual_addr;
    while (cursor < virtual_addr + size && QueryRegion(cursor, info) && Committed(info)) {
        cursor = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
    }
    return std::min<u64>(size, cursor - virtual_addr);
}

void MemoryManager::CopySparseMemory(VAddr virtual_addr, u8* dest, u64 size) {
    MEMORY_BASIC_INFORMATION info{};
    while (size != 0) {
        if (!QueryRegion(virtual_addr, info)) {
            std::memset(dest, 0, size);
            return;
        }
        const VAddr region_end = reinterpret_cast<VAddr>(info.BaseAddress) + info.RegionSize;
        const u64 chunk = std::min<u64>(size, region_end - virtual_addr);
        if (Committed(info) && (info.Protect & PAGE_NOACCESS) == 0) {
            std::memcpy(dest, reinterpret_cast<const void*>(virtual_addr), chunk);
        } else {
            std::memset(dest, 0, chunk);
        }
        size -= chunk;
        virtual_addr += chunk;
        dest += chunk;
    }
}

bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    MEMORY_BASIC_INFORMATION info{};
    auto* cursor = static_cast<u8*>(address);
    const auto* source = static_cast<const u8*>(data);
    while (size != 0) {
        std::lock_guard lock{protection_mutex};
        if (!QueryRegion(reinterpret_cast<VAddr>(cursor), info) || !Committed(info)) {
            return false;
        }
        const auto* region_end = static_cast<u8*>(info.BaseAddress) + info.RegionSize;
        const u64 chunk = std::min<u64>(size, static_cast<u64>(region_end - cursor));
        const bool writable = (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
        DWORD previous = 0;
        if (!writable && !VirtualProtect(cursor, chunk, PAGE_READWRITE, &previous)) {
            return false;
        }
        std::memcpy(cursor, source, chunk);
        if (!writable) {
            VirtualProtect(cursor, chunk, previous, &previous);
        }
        size -= chunk;
        cursor += chunk;
        source += chunk;
    }
    return true;
}

} // namespace Core

namespace Libraries::VideoOut {

namespace {

VideoOutPort headless_port;
std::array<std::atomic<u64>, 16> flip_labels{};
std::atomic<s32> previous_flip_index{-1};
std::atomic<u64> completed_flips{0};

} // namespace

bool VideoOutPort::IsVoLabel(const u64* address) const {
    const auto* first = reinterpret_cast<const u64*>(flip_labels.data());
    return address >= first && address < first + flip_labels.size();
}

void VideoOutPort::WaitVoLabel(const std::function<bool()>& ready) {
    const auto started = std::chrono::steady_clock::now();
    while (!ready()) {
        std::this_thread::sleep_for(std::chrono::microseconds(250));
        if (std::chrono::steady_clock::now() - started > std::chrono::seconds(2)) {
            LOG_WARNING(Lib_GnmDriver, "headless flip label wait timed out; continuing");
            return;
        }
    }
}

u64 HeadlessCompletedFlips() {
    return completed_flips.load(std::memory_order_acquire);
}

namespace {

using PresentFunction = void (*)(s32, s64);
using GetBufferFunction = s32 (*)(s32, const void**, Libraries::VideoOut::BufferAttribute*);
using PresentPixelsFunction = void (*)(s32, s64, const void*, u32, u32, u32, s32);
using PresentedFunction = void (*)(s32, s64);

template <typename T>
T VideoOutExportAs(const char* name) {
    const HMODULE module = GetModuleHandleW(L"libSceVideoOut.prx");
    return module != nullptr ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}

PresentFunction VideoOutExport(const char* name) {
    return VideoOutExportAs<PresentFunction>(name);
}


PresentFunction PresentExport() {
    static const PresentFunction function = VideoOutExport("OrbisVideoOutPresent");
    return function;
}

PresentFunction FlipRequestedExport() {
    static const PresentFunction function = VideoOutExport("OrbisVideoOutFlipRequested");
    return function;
}

void PresentThroughRenderer(s32 buffer, s64 flip_arg) {
    static const auto get_buffer = VideoOutExportAs<GetBufferFunction>("OrbisVideoOutGetBuffer");
    static const auto present_pixels = VideoOutExportAs<PresentPixelsFunction>("OrbisVideoOutPresentPixels");
    static const auto presented = VideoOutExportAs<PresentedFunction>("OrbisVideoOutFlipPresented");
    if (AnyPs4::RendererReady() && get_buffer != nullptr) {
        const void* address = nullptr;
        Libraries::VideoOut::BufferAttribute attribute{};
        if (get_buffer(buffer, &address, &attribute) == 0 && address != nullptr) {
            if (presented != nullptr && AnyPs4::PresentFlip(attribute, reinterpret_cast<VAddr>(address))) {
                presented(buffer, flip_arg);
                return;
            }
            AnyPs4::ReadbackFrame frame{};
            if (present_pixels != nullptr && AnyPs4::ReadbackFlip(attribute, reinterpret_cast<VAddr>(address), frame)) {
                present_pixels(buffer, flip_arg, frame.pixels, frame.width, frame.height, frame.stride, frame.swap_red_blue ? 1 : 0);
                return;
            }
        }
    }
    if (const auto present = PresentExport()) present(buffer, flip_arg);
}

} // namespace

VideoOutPort* HeadlessVideoOutPort() {
    return &headless_port;
}

s32 PS4_SYSV_ABI sceVideoOutGetBufferLabelAddress(s32, uintptr_t* label_addr) {
    if (label_addr == nullptr) return 0;
    *label_addr = reinterpret_cast<uintptr_t>(flip_labels.data());
    return static_cast<s32>(flip_labels.size());
}

s32 sceVideoOutSubmitEopFlip(s32 handle, u32 buf_id, u32 mode, s64 flip_arg, void**) {
    static std::uint64_t calls = 0;
    OrbisHleTrace("libSceVideoOut.prx", "sceVideoOutSubmitEopFlip", "headless", ++calls, __builtin_return_address(0));
    LOG_DEBUG(Lib_GnmDriver, "headless flip request: handle={} buffer={} mode={} arg={}", handle, buf_id, mode, flip_arg);
    if (buf_id >= flip_labels.size()) return 0x80290002;
    if (const auto requested = FlipRequestedExport()) requested(static_cast<s32>(buf_id), flip_arg);
    Platform::IrqC::Instance()->RegisterOnce(Platform::InterruptId::GfxFlip, [buf_id, flip_arg](Platform::InterruptId) {
        PresentThroughRenderer(static_cast<s32>(buf_id), flip_arg);
        const s32 previous = previous_flip_index.exchange(static_cast<s32>(buf_id), std::memory_order_acq_rel);
        if (previous >= 0) flip_labels[static_cast<size_t>(previous)].store(0, std::memory_order_release);
        completed_flips.fetch_add(1, std::memory_order_acq_rel);
    });
    return 0;
}

} // namespace Libraries::VideoOut
