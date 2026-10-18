// AnyPS4 shim: a VideoOut port whose display labels live in adapter/Runtime.cpp.
// Phase 3.1 replaces it with the real port.
#pragma once

#include <functional>
#include "common/types.h"

namespace Libraries::VideoOut {

struct VideoOutPort {
    bool IsVoLabel(const u64* address) const;
    void WaitVoLabel(const std::function<bool()>& ready);
};

VideoOutPort* HeadlessVideoOutPort();
u64 HeadlessCompletedFlips();

} // namespace Libraries::VideoOut
