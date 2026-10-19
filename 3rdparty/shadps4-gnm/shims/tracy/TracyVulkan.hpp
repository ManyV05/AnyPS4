// AnyPS4 shim: Tracy Vulkan profiler disabled.
#pragma once
#include "common/debug.h"

using TracyVkCtx = void*;
#define TracyVkContextHostCalibrated(...) nullptr
#define TracyVkContextName(ctx, name, size)
#define TracyVkCollect(ctx, cmdbuf)
#define TracyVkNamedZoneC(ctx, varname, cmdbuf, name, color, active)
#define TracyVkDestroy(ctx)
