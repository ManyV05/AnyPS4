// AnyPS4 adapter: debugger state, settings, memory facade and the headless VideoOut port.
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/video_out.h"
#include "common/logging/log.h"
#include "core/platform.h"
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <windows.h>

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
    Platform::IrqC::Instance()->RegisterOnce(Platform::InterruptId::GfxFlip, [buf_id](Platform::InterruptId) {
        const s32 previous = previous_flip_index.exchange(static_cast<s32>(buf_id), std::memory_order_acq_rel);
        if (previous >= 0) flip_labels[static_cast<size_t>(previous)].store(0, std::memory_order_release);
        completed_flips.fetch_add(1, std::memory_order_acq_rel);
    });
    return 0;
}

} // namespace Libraries::VideoOut
