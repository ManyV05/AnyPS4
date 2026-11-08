#pragma once

#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace launcher {

using LineSink = std::function<void(const std::wstring&)>;

bool RunCaptured(const std::wstring& commandLine, const std::wstring& workingDirectory, LineSink sink, int& exitCode);

bool StartDetached(const std::wstring& executable, const std::wstring& workingDirectory, const std::vector<std::pair<std::wstring, std::wstring>>& environment,
                   const std::wstring& logFile, std::wstring& error, HANDLE* process = nullptr);

std::wstring Quote(const std::wstring& argument);

}  // namespace launcher
