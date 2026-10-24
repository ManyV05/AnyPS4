// AnyPS4 shim: the settings the renderer consults, fixed to shadPS4's defaults.
#pragma once

#include <cstdlib>
#include <string>
#include "common/types.h"

enum GpuReadbacksMode : int {
    Disabled,
    Relaxed,
    Precise,
};

struct EmulatorSettingsImpl {
    bool IsCopyGpuBuffers() const { return false; }
    bool IsVkHostMarkersEnabled() const { return false; }
    bool IsVkGuestMarkersEnabled() const { return false; }
    bool IsPipelineCacheArchived() const { return false; }
    bool IsPipelineCacheEnabled() const { return std::getenv("ORBIS_GNM_NO_PIPELINE_CACHE") == nullptr; }
    bool IsDumpShaders() const { return std::getenv("ORBIS_GNM_DUMP_SHADERS") != nullptr; }
    bool IsShaderCollect() const { return false; }
    bool IsRenderdocEnabled() const { return false; }
    bool IsNullGPU() const { return false; }
    bool IsHdrAllowed() const { return false; }
    bool IsDirectMemoryAccessEnabled() const { return false; }
    bool IsVkValidationEnabled() const { return std::getenv("ORBIS_GNM_VALIDATION") != nullptr; }
    bool IsVkValidationSyncEnabled() const { return false; }
    bool IsVkValidationGpuEnabled() const { return false; }
    bool IsVkValidationCoreEnabled() const { return IsVkValidationEnabled(); }
    bool IsVkCrashDiagnosticEnabled() const { return false; }
    bool IsReadbackLinearImagesEnabled() const { return false; }
    bool IsRcasEnabled() const { return false; }
    bool IsPatchShaders() const { return false; }
    bool IsFsrEnabled() const { return false; }
    GpuReadbacksMode GetReadbacksMode() const { return GpuReadbacksMode::Disabled; }
    std::string GetPresentMode() const {
        const char* mode = std::getenv("ORBIS_GNM_PRESENT_MODE");
        return mode != nullptr ? mode : "Fifo";
    }
    float GetRcasAttenuation() const { return 0.25f; }
    s32 GetGpuId() const { return -1; }
};

extern EmulatorSettingsImpl EmulatorSettings;
