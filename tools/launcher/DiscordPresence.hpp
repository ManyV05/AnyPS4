#pragma once

#include <windows.h>

#include <string>
#include <thread>

#include "Process.hpp"

namespace launcher {

class DiscordPresence {
public:
    DiscordPresence() = default;
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;
    ~DiscordPresence();

    bool Start(const std::wstring& clientId, HANDLE gameProcess, LineSink log);
    void Stop();

private:
    std::thread worker_;
    HANDLE stop_ = nullptr;
};

bool ValidDiscordClientId(const std::wstring& clientId);

}  // namespace launcher
