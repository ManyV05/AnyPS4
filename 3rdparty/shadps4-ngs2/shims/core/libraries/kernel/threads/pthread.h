// AnyPS4 shim: ngs2 only declares PthreadMutex members, it never locks them.
#pragma once

namespace Libraries::Kernel {
struct PthreadMutex {
    void* opaque = nullptr;
};
} // namespace Libraries::Kernel
