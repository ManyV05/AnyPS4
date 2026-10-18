// AnyPS4 shim: the time stamp counter helpers pm4_cmds.h uses, served by libkernel.prx.
#pragma once

#include <cstdint>
#include "common/types.h"

extern "C" std::uint64_t PS4_SYSV_ABI sceKernelReadTsc(void);
extern "C" std::uint64_t PS4_SYSV_ABI sceKernelGetTscFrequency(void);

namespace Libraries::Kernel {

inline u64 PS4_SYSV_ABI sceKernelReadTsc() {
    return ::sceKernelReadTsc();
}

inline u64 PS4_SYSV_ABI sceKernelGetTscFrequency() {
    return ::sceKernelGetTscFrequency();
}

} // namespace Libraries::Kernel
