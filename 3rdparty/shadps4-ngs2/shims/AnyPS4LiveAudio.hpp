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
#include <cstdlib>
#include <cwchar>

namespace anyps4 {

struct LiveAudioBlock {
    std::atomic<std::int32_t> state;
    std::atomic<float> bgm;
    std::atomic<float> main;
    std::atomic<float> musicFloor;
};

static_assert(std::atomic<float>::is_always_lock_free, "the live audio block is shared between processes");

inline float LiveEnvironmentValue(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? static_cast<float>(std::atof(value)) : fallback;
}

inline void LiveAudioName(unsigned long processId, wchar_t (&name)[64]) {
    std::swprintf(name, 64, L"Local\\AnyPS4.LiveAudio.%lu", processId);
}

inline LiveAudioBlock* LiveAudioBlockOfThisProcess() {
    static LiveAudioBlock* const block = []() -> LiveAudioBlock* {
        wchar_t name[64];
        LiveAudioName(GetCurrentProcessId(), name);
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LiveAudioBlock), name);
        if (mapping == nullptr) return nullptr;
        void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LiveAudioBlock));
        if (view == nullptr) return nullptr;
        auto* shared = static_cast<LiveAudioBlock*>(view);
        std::int32_t expected = 0;
        if (shared->state.compare_exchange_strong(expected, 1)) {
            shared->bgm.store(LiveEnvironmentValue("ORBIS_AUDIO_BGM_GAIN", 0.7f));
            shared->main.store(LiveEnvironmentValue("ORBIS_AUDIO_MAIN_GAIN", 1.0f));
            shared->musicFloor.store(LiveEnvironmentValue("ORBIS_NGS2_MUSIC_FLOOR", 0.0f));
            shared->state.store(2);
        } else {
            while (shared->state.load() != 2) Sleep(0);
        }
        return shared;
    }();
    return block;
}

inline float LiveBgmGain() {
    static const float fallback = LiveEnvironmentValue("ORBIS_AUDIO_BGM_GAIN", 0.7f);
    const LiveAudioBlock* block = LiveAudioBlockOfThisProcess();
    return block != nullptr ? block->bgm.load(std::memory_order_relaxed) : fallback;
}

inline float LiveMainGain() {
    static const float fallback = LiveEnvironmentValue("ORBIS_AUDIO_MAIN_GAIN", 1.0f);
    const LiveAudioBlock* block = LiveAudioBlockOfThisProcess();
    return block != nullptr ? block->main.load(std::memory_order_relaxed) : fallback;
}

inline float LiveMusicFloor() {
    static const float fallback = LiveEnvironmentValue("ORBIS_NGS2_MUSIC_FLOOR", 0.0f);
    const LiveAudioBlock* block = LiveAudioBlockOfThisProcess();
    return block != nullptr ? block->musicFloor.load(std::memory_order_relaxed) : fallback;
}

}  // namespace anyps4
