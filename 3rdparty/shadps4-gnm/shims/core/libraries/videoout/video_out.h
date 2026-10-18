// AnyPS4 shim: the two VideoOut entry points gnmdriver calls when a submission
// carries a flip request (adapter/Runtime.cpp).
#pragma once

#include "common/types.h"
#include "core/libraries/videoout/driver.h"

namespace Libraries::VideoOut {

s32 PS4_SYSV_ABI sceVideoOutGetBufferLabelAddress(s32 handle, uintptr_t* label_addr);
s32 sceVideoOutSubmitEopFlip(s32 handle, u32 buf_id, u32 mode, s64 flip_arg, void** unk);

} // namespace Libraries::VideoOut
