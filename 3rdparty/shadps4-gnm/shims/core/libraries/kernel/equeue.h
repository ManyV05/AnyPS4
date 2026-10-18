// AnyPS4 shim: the shadPS4 event queue interface gnmdriver uses, bridged to the
// libkernel.prx event queues of AnyPS5 (see adapter/KernelBridge.cpp).
#pragma once

#include "common/types.h"

namespace Libraries::Kernel {

using OrbisKernelEqueue = s64;

struct OrbisKernelEvent {
    enum Filter : s16 {
        None = 0,
        Read = -1,
        Write = -2,
        Aio = -3,
        Vnode = -4,
        Proc = -5,
        Signal = -6,
        Timer = -7,
        Fs = -9,
        User = -11,
        Polling = -12,
        VideoOut = -13,
        GraphicsCore = -14,
        HrTimer = -15,
    };

    enum Flags : u16 {
        Add = 1u,
        Delete = 2u,
        Enable = 4u,
        Disable = 8u,
        OneShot = 0x10u,
        Clear = 0x20u,
        Receipt = 0x40u,
        Dispatch = 0x80u,
        Flag1 = 0x2000u,
        System = 0xf000u,
    };

    u64 ident = 0;
    Filter filter = Filter::None;
    u16 flags = 0;
    u32 fflags = 0;
    u64 data = 0;
    void* udata = nullptr;
};

struct EqueueEvent {
    OrbisKernelEvent event;
    void* data = nullptr;
};

class EqueueInternal {
public:
    explicit EqueueInternal(OrbisKernelEqueue handle) : handle(handle) {}
    bool AddEvent(EqueueEvent& event);
    bool RemoveEvent(u64 id, s16 filter);
    bool TriggerEvent(u64 ident, s16 filter, void* trigger_data);
    OrbisKernelEqueue Handle() const { return handle; }

private:
    OrbisKernelEqueue handle;
};

EqueueInternal* GetEqueue(OrbisKernelEqueue eq);

inline u64 PS4_SYSV_ABI sceKernelGetEventData(const OrbisKernelEvent* ev) {
    return ev->data;
}

} // namespace Libraries::Kernel
