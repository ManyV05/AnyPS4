// AnyPS4 shim: the settings the Gnm code consults, fixed to their defaults.
#pragma once

struct EmulatorSettingsImpl {
    bool IsCopyGpuBuffers() const { return false; }
    bool IsVkHostMarkersEnabled() const { return false; }
    bool IsVkGuestMarkersEnabled() const { return false; }
};

extern EmulatorSettingsImpl EmulatorSettings;
