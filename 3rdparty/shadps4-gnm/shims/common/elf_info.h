// AnyPS4 shim: only the firmware version thresholds gnmdriver compares against.
#pragma once

#include "common/types.h"

namespace Common {

class ElfInfo {
public:
    static constexpr u32 FW_100 = 0x1000000;
    static constexpr u32 FW_150 = 0x1500000;
    static constexpr u32 FW_160 = 0x1600000;
    static constexpr u32 FW_170 = 0x1700000;
    static constexpr u32 FW_200 = 0x2000000;
    static constexpr u32 FW_250 = 0x2500000;
    static constexpr u32 FW_300 = 0x3000000;
    static constexpr u32 FW_350 = 0x3500000;
    static constexpr u32 FW_400 = 0x4000000;
    static constexpr u32 FW_450 = 0x4500000;
    static constexpr u32 FW_500 = 0x5000000;
};

} // namespace Common
