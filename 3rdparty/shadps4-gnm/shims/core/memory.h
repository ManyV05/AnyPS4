// AnyPS4 shim: guest addresses are host addresses. The facade answers the caches'
// questions with VirtualQuery and writes through protections with VirtualProtect.
#pragma once

#include <cstddef>
#include "common/singleton.h"
#include "common/types.h"
#include "core/address_space.h"

namespace Vulkan {
class Rasterizer;
}

namespace Core {

class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }
    AddressSpace& GetAddressSpace() {
        return address_space;
    }
    VAddr SystemReservedVirtualBase() noexcept {
        return address_space.SystemReservedVirtualBase();
    }
    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        const VAddr max_gpu_address{0x10000000000};
        return virtual_addr + size < max_gpu_address;
    }
    static void BeginFrame();
    bool IsValidMapping(VAddr virtual_addr, u64 size = 0);
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    bool TryWriteBacking(void* address, const void* data, u64 size);

private:
    AddressSpace address_space;
    Vulkan::Rasterizer* rasterizer{};
};

using Memory = Common::Singleton<MemoryManager>;

} // namespace Core
