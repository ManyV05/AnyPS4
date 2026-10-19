// AnyPS4 shim: shadPS4's debugger state is permanently idle; counters are kept.
#pragma once

#include <atomic>
#include <utility>
#include <vector>
#include "common/types.h"

namespace DebugStateType {

enum class QueueType {
    dcb = 0,
    ccb = 1,
    acb = 2,
};

struct QueueDump {
    QueueType type;
    u32 submit_num;
    u32 num2;
    std::vector<u32> data;
    uintptr_t base_addr;
};

class DebugStateImpl {
public:
    std::pair<u32, u32> game_resolution{};
    std::pair<u32, u32> output_resolution{};
    bool is_using_fsr{};

    bool DumpingCurrentFrame() const { return false; }
    bool DumpingCurrentReg() { return false; }
    bool ShouldPauseInSubmit() const { return false; }
    void PauseGuestThreads() {}
    void IncGnmFrameNum() { ++frame_count; }
    void IncFlipFrameNum() { ++flip_frame_count; draw_call_count = 0; dispatch_count = 0; }
    void IncDrawCall() noexcept { ++draw_call_count; }
    void IncDispatch() noexcept { ++dispatch_count; }
    u64 GetFrameCount() const { return frame_count; }
    u64 GetFlipFrameCount() const { return flip_frame_count; }
    void PushQueueDump(QueueDump) {}
    template <typename TRegs>
    void PushRegsDump(uintptr_t, uintptr_t, const TRegs&) {}
    template <typename TState>
    void PushRegsDumpCompute(uintptr_t, uintptr_t, const TState&) {}
    template <typename... Args>
    void CollectShader(Args&&...) {}

private:
    u64 frame_count = 0;
    u64 flip_frame_count = 0;
    std::atomic<u32> draw_call_count{0};
    std::atomic<u32> dispatch_count{0};
};

} // namespace DebugStateType

extern DebugStateType::DebugStateImpl DebugState;
