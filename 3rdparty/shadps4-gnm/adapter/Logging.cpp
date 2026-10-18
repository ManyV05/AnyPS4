// AnyPS4 adapter: fmt-based log backend behind shims/common/logging/log.h.
#include "common/logging/log.h"
#include "common/assert.h"
#include "common/thread.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <windows.h>

namespace Common::Log {

namespace {

Level ThresholdFromEnvironment() {
    const char* value = std::getenv("ORBIS_GNM_LOG");
    if (value == nullptr) {
        return Level::Warning;
    }
    const std::string text(value);
    if (text == "trace") return Level::Trace;
    if (text == "debug") return Level::Debug;
    if (text == "info") return Level::Info;
    if (text == "warning") return Level::Warning;
    if (text == "error") return Level::Error;
    if (text == "critical") return Level::Critical;
    if (text == "off") return static_cast<Level>(static_cast<int>(Level::Critical) + 1);
    return Level::Warning;
}

const char* LevelName(Level level) {
    switch (level) {
    case Level::Trace: return "Trace";
    case Level::Debug: return "Debug";
    case Level::Info: return "Info";
    case Level::Warning: return "Warning";
    case Level::Error: return "Error";
    case Level::Critical: return "Critical";
    default: return "?";
    }
}

} // namespace

bool Enabled(Level level) {
    static const Level threshold = ThresholdFromEnvironment();
    return static_cast<int>(level) >= static_cast<int>(threshold);
}

void Message(Level level, std::string_view log_class, const char* file, int line, const char* function, std::string_view text) {
    const char* base = std::strrchr(file, '/');
    const char* base_backslash = std::strrchr(file, '\\');
    if (base_backslash != nullptr && (base == nullptr || base_backslash > base)) base = base_backslash;
    base = base != nullptr ? base + 1 : file;
    std::fprintf(stderr, "[GNM] <%s> [%.*s] %s:%d %s: %.*s\n", LevelName(level), static_cast<int>(log_class.size()), log_class.data(), base, line,
                 function, static_cast<int>(text.size()), text.data());
    std::fflush(stderr);
}

} // namespace Common::Log

void assert_fail_impl() {
    std::fputs("[GNM] assertion failed\n", stderr);
    std::fflush(stderr);
    std::abort();
}

[[noreturn]] void unreachable_impl() {
    std::fputs("[GNM] unreachable code\n", stderr);
    std::fflush(stderr);
    std::abort();
}

namespace Common {

void SetCurrentThreadRealtime(std::chrono::nanoseconds) {}

void SetCurrentThreadPriority(ThreadPriority priority) {
    int native = THREAD_PRIORITY_NORMAL;
    switch (priority) {
    case ThreadPriority::Low: native = THREAD_PRIORITY_BELOW_NORMAL; break;
    case ThreadPriority::Normal: native = THREAD_PRIORITY_NORMAL; break;
    case ThreadPriority::High: native = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case ThreadPriority::VeryHigh: native = THREAD_PRIORITY_HIGHEST; break;
    case ThreadPriority::Critical: native = THREAD_PRIORITY_TIME_CRITICAL; break;
    }
    SetThreadPriority(GetCurrentThread(), native);
}

void SetCurrentThreadName(const char* name) {
    SetThreadName(GetCurrentThread(), name);
}

void SetThreadName(void* thread, const char* name) {
    wchar_t wide[128];
    const int length = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 128);
    if (length > 0) SetThreadDescription(static_cast<HANDLE>(thread), wide);
}

bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining, bool) {
    std::this_thread::sleep_for(duration);
    if (remaining != nullptr) *remaining = std::chrono::nanoseconds{0};
    return true;
}

AccurateTimer::AccurateTimer(std::chrono::nanoseconds target_interval) : target_interval(target_interval) {}

void AccurateTimer::Start() {
    start_time = std::chrono::high_resolution_clock::now();
}

void AccurateTimer::End() {
    const auto elapsed = std::chrono::high_resolution_clock::now() - start_time;
    if (elapsed < target_interval) {
        std::this_thread::sleep_for(target_interval - elapsed);
        total_wait += target_interval - elapsed;
    }
}

std::string GetCurrentThreadName() {
    PWSTR description = nullptr;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &description)) || description == nullptr) return {};
    char narrow[128];
    const int length = WideCharToMultiByte(CP_UTF8, 0, description, -1, narrow, sizeof(narrow), nullptr, nullptr);
    LocalFree(description);
    return length > 0 ? std::string(narrow) : std::string{};
}

} // namespace Common
