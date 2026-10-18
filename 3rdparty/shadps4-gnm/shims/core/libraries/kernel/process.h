// AnyPS4 shim: process queries gnmdriver and liverpool make (adapter/KernelBridge.cpp).
#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

s32 PS4_SYSV_ABI sceKernelIsNeoMode();
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver);

} // namespace Libraries::Kernel
