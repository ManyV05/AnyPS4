// AnyPS4 shim: shadPS4's debugger state is permanently idle.
#pragma once

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
    bool DumpingCurrentFrame() const { return false; }
    bool DumpingCurrentReg() { return false; }
    bool ShouldPauseInSubmit() const { return false; }
    void PauseGuestThreads() {}
    void IncGnmFrameNum() { ++frame_count; }
    u64 GetFrameCount() const { return frame_count; }
    void PushQueueDump(QueueDump) {}
    template <typename TRegs>
    void PushRegsDump(uintptr_t, uintptr_t, const TRegs&) {}
    template <typename TState>
    void PushRegsDumpCompute(uintptr_t, uintptr_t, const TState&) {}

private:
    u64 frame_count = 0;
};

} // namespace DebugStateType

extern DebugStateType::DebugStateImpl DebugState;
