// AnyPS4 adapter: bridges the shadPS4 kernel interfaces gnmdriver uses to the
// libkernel.prx of AnyPS5 (event queues) and to the title's process parameters.
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/process.h"
#include "common/logging/log.h"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <windows.h>

namespace Libraries::Kernel {

namespace {

constexpr u16 EvAdd = 0x0001;
constexpr u16 EvClear = 0x0020;

std::mutex equeue_mutex;
std::map<OrbisKernelEqueue, std::unique_ptr<EqueueInternal>> equeues;

} // namespace

bool EqueueInternal::AddEvent(EqueueEvent& event) {
    KernelEqueueEvent native{};
    native.event.ident = static_cast<uintptr_t>(event.event.ident);
    native.event.filter = static_cast<int16_t>(event.event.filter);
    native.event.flags = static_cast<uint16_t>(EvAdd | EvClear);
    native.event.fflags = event.event.fflags;
    native.event.data = static_cast<intptr_t>(event.event.data);
    native.event.udata = event.event.udata;
    native.filter.triggerFunc = [](KernelEqueueEvent* e, void* data) {
        e->triggered = true;
        e->event.data = reinterpret_cast<intptr_t>(data);
    };
    native.filter.resetFunc = [](KernelEqueueEvent* e) {
        e->triggered = false;
        e->event.fflags = 0;
    };
    const int result = EqueueAddEvent_nid_postfix(handle, native);
    if (result != 0) {
        LOG_ERROR(Lib_GnmDriver, "EqueueAddEvent failed: eq={} ident={} result={:#x}", handle, event.event.ident, static_cast<u32>(result));
    }
    return result == 0;
}

bool EqueueInternal::RemoveEvent(u64 id, s16 filter) {
    return EqueueDeleteEvent_nid_postfix(handle, static_cast<uintptr_t>(id), filter) == 0;
}

bool EqueueInternal::TriggerEvent(u64 ident, s16 filter, void* trigger_data) {
    return EqueueTriggerEvent_nid_postfix(handle, static_cast<uintptr_t>(ident), filter, trigger_data) == 0;
}

EqueueInternal* GetEqueue(OrbisKernelEqueue eq) {
    if (eq <= 0) return nullptr;
    std::lock_guard lock(equeue_mutex);
    auto& slot = equeues[eq];
    if (!slot) slot = std::make_unique<EqueueInternal>(eq);
    return slot.get();
}

s32 PS4_SYSV_ABI sceKernelIsNeoMode() {
    return 0;
}

namespace {

// The relinker stores the title's PT_SCE_PROCPARAM location in a `.procpar`
// section of the executable: a 32-bit RVA followed by a 32-bit size.
const unsigned char* ProcessParameters() {
    static const unsigned char* parameters = [] {
        const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        if (image == nullptr) return static_cast<const unsigned char*>(nullptr);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
            if (std::memcmp(section->Name, ".procpar", 8) != 0) continue;
            u32 rva = 0;
            std::memcpy(&rva, image + section->VirtualAddress, sizeof(rva));
            return image + rva;
        }
        return static_cast<const unsigned char*>(nullptr);
    }();
    return parameters;
}

} // namespace

s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    const auto* parameters = ProcessParameters();
    if (ver == nullptr || parameters == nullptr) return -1;
    u64 sdk = 0;
    std::memcpy(&sdk, parameters + 0x10, sizeof(sdk));
    *ver = static_cast<s32>(sdk);
    return 0;
}

} // namespace Libraries::Kernel
