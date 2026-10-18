// AnyPS4 shim: a rasterizer that is never instantiated. Liverpool checks its
// pointer before every use, so the headless command processor binds none.
#pragma once

#include <string_view>
#include "common/types.h"

namespace Vulkan {

class Rasterizer {
public:
    void Draw(bool, u32 = 0) {}
    void DrawIndirect(bool, VAddr, u32, u32, u32, VAddr) {}
    void DispatchDirect() {}
    void DispatchIndirect(VAddr, u32, u32) {}
    void ScopeMarkerBegin(const std::string_view&, bool = false) {}
    void ScopeMarkerEnd(bool = false) {}
    void ScopedMarkerInsert(const std::string_view&, bool = false) {}
    void ScopedMarkerInsertColor(const std::string_view&, u32, bool = false) {}
    void FillBuffer(VAddr, u32, u32, bool) {}
    void CopyBuffer(VAddr, VAddr, u32, bool, bool) {}
    u32 ReadDataFromGds(u32) { return 0; }
    bool InvalidateMemory(VAddr, u64) { return false; }
    bool ReadMemory(VAddr, u64) { return false; }
    void ProcessDownloadImages() {}
    bool IsMapped(VAddr, u64) { return true; }
    void MapMemory(VAddr, u64) {}
    void CpSync() {}
    u64 Flush() { return 0; }
    void Finish() {}
    void OnSubmit() {}
};

} // namespace Vulkan
