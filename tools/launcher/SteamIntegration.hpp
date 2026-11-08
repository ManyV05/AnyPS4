#pragma once

#include <cstdint>
#include <string>

#include "Process.hpp"

namespace launcher {

struct SteamResult {
    bool ok = false;
    bool changed = false;
    std::uint32_t appId = 0;
    std::wstring message;
};

std::uint32_t ShortcutAppId(const std::string& quotedExecutable, const std::string& name);

bool IsOnSteam(const std::wstring& launcherPath);

SteamResult AddToSteam(const std::wstring& launcherPath, const std::wstring& installDirectory, const std::wstring& gameDirectory, LineSink sink);

}  // namespace launcher
