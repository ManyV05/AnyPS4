// AnyPS4 shim: the title runs natively, so guest addresses are host addresses and
// there is no backing mapping to write through. The facade keeps the two entry
// points liverpool.cpp and gnmdriver.cpp use.
#pragma once

#include <cstddef>
#include "common/singleton.h"
#include "common/types.h"

namespace Core {

class AddressSpace {
public:
    uintptr_t SystemReservedVirtualBase();
    size_t SystemReservedVirtualSize();
};

class MemoryManager {
public:
    bool TryWriteBacking(void*, const void*, u32) { return false; }
    AddressSpace& GetAddressSpace() { return address_space; }

private:
    AddressSpace address_space;
};

using Memory = Common::Singleton<MemoryManager>;

} // namespace Core
