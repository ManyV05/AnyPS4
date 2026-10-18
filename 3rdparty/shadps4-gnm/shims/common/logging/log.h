// AnyPS4 shim: replaces shadPS4's spdlog-based logging with a small fmt-based backend.
#pragma once

#include <string_view>
#include <fmt/format.h>
#include "common/logging/classes.h"

namespace Common::Log {

enum class Level { Trace, Debug, Info, Warning, Error, Critical };

bool Enabled(Level level);
void Message(Level level, std::string_view log_class, const char* file, int line, const char* function, std::string_view text);

} // namespace Common::Log

#define LOG_GENERIC(log_class, log_level, format_string, ...)                                            \
    do {                                                                                          \
        if (Common::Log::Enabled(log_level)) {                                                    \
            Common::Log::Message(log_level, Common::Log::Class::log_class, __FILE__, __LINE__, __func__, \
                                 fmt::format(fmt::runtime(format_string) __VA_OPT__(, ) __VA_ARGS__));   \
        }                                                                                         \
    } while (false)

#define LOG_TRACE(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Trace, __VA_ARGS__)
#define LOG_DEBUG(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Debug, __VA_ARGS__)
#define LOG_INFO(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Info, __VA_ARGS__)
#define LOG_WARNING(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Warning, __VA_ARGS__)
#define LOG_ERROR(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Error, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...) LOG_GENERIC(log_class, Common::Log::Level::Critical, __VA_ARGS__)
