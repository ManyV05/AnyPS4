// AnyPS4 shim: shadPS4's Tracy profiler hooks become no-ops.
#pragma once

#if defined(_MSC_VER)
#define BREAKPOINT __debugbreak
#else
#define BREAKPOINT __builtin_trap
#endif

static inline bool IsProfilerConnected() {
    return false;
}

#define TRACY_GPU_ENABLED 0
#define CUSTOM_LOCK(type, varname) type varname
#define TRACK_ALLOC(ptr, size, pool)
#define TRACK_FREE(ptr, pool)
#define EMULATOR_TRACE
#define RENDERER_TRACE
#define HLE_TRACE
#define TRACE_HINT(str)
#define TRACE_WARN(msg)
#define TRACE_ERROR(msg)
#define TRACE_CRIT(msg)
#define GPU_SCOPE_LOCATION(name, color)
#define FRAME_END
#define FIBER_ENTER(name)
#define FIBER_EXIT
