#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cwchar>

namespace anyps4 {

struct LiveSaveBlock {
    std::atomic<std::int32_t> state;
    std::atomic<std::int32_t> requestedSlot;
    std::atomic<std::int32_t> activeSlot;
    std::atomic<std::int32_t> savesDone;
    std::atomic<std::int32_t> lastSavedSlot;
};

static_assert(std::atomic<std::int32_t>::is_always_lock_free, "the live save block is shared between processes");

inline void LiveSaveName(unsigned long processId, wchar_t (&name)[64]) {
    std::swprintf(name, 64, L"Local\\AnyPS4.LiveSave.%lu", processId);
}

inline LiveSaveBlock* LiveSaveBlockOfThisProcess(int initialSlot) {
    static LiveSaveBlock* const block = [initialSlot]() -> LiveSaveBlock* {
        wchar_t name[64];
        LiveSaveName(GetCurrentProcessId(), name);
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LiveSaveBlock), name);
        if (mapping == nullptr) return nullptr;
        void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LiveSaveBlock));
        if (view == nullptr) return nullptr;
        auto* shared = static_cast<LiveSaveBlock*>(view);
        std::int32_t expected = 0;
        if (shared->state.compare_exchange_strong(expected, 1)) {
            shared->requestedSlot.store(initialSlot);
            shared->activeSlot.store(initialSlot);
            shared->savesDone.store(0);
            shared->lastSavedSlot.store(0);
            shared->state.store(2);
        } else {
            while (shared->state.load() != 2) Sleep(0);
        }
        return shared;
    }();
    return block;
}

}  // namespace anyps4
