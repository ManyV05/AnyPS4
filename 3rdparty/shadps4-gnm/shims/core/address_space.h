// AnyPS4 shim: the title runs natively, so the address space is the process's own.
// Protect() maps straight onto VirtualProtect; the facade lives in adapter/Runtime.cpp.
#pragma once

#include <cstddef>
#include "common/enum.h"
#include "common/types.h"

namespace Core {

enum class MemoryPermission : u32 {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadWrite = Read | Write,
    Execute = 1 << 2,
    ReadWriteExecute = Read | Write | Execute,
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

class AddressSpace {
public:
    uintptr_t SystemReservedVirtualBase();
    size_t SystemReservedVirtualSize();
    void Protect(VAddr virtual_addr, u64 size, MemoryPermission perms);
};

} // namespace Core
