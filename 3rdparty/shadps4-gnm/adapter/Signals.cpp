// AnyPS4 adapter: Core::SignalDispatch over a vectored exception handler. The
// handler runs before AnyPS5's own crash reporter and swallows the faults the
// page manager resolves (GPU page tracking).
#include <windows.h>
#include "core/signals.h"

namespace Core {

static LONG WINAPI AnyPs4ExceptionHandler(EXCEPTION_POINTERS* pointers) noexcept {
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto* signals = Signals::Instance();
    switch (pointers->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        if (signals->DispatchAccessViolation(
                pointers, reinterpret_cast<void*>(pointers->ExceptionRecord->ExceptionInformation[1]))) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        if (signals->DispatchIllegalInstruction(pointers)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        break;
    default:
        break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

SignalDispatch::SignalDispatch() {
    handle = AddVectoredExceptionHandler(1, AnyPs4ExceptionHandler);
}

SignalDispatch::~SignalDispatch() {
    RemoveHandlers();
}

void SignalDispatch::RemoveHandlers() {
    if (handle != nullptr) {
        RemoveVectoredExceptionHandler(handle);
        handle = nullptr;
    }
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, priority] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, priority] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core
