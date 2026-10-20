#include "Process.hpp"

#include <string>
#include <vector>

namespace launcher {

namespace {

std::wstring Decode(const std::string& bytes) {
    if (bytes.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    if (length == 0) {
        codePage = CP_ACP;
        length = MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), length);
    return wide;
}

}  // namespace

std::wstring Quote(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring::npos) return argument;
    std::wstring quoted = L"\"";
    for (wchar_t c : argument) {
        if (c == L'"') quoted += L"\\\"";
        else quoted += c;
    }
    return quoted + L"\"";
}

bool RunCaptured(const std::wstring& commandLine, const std::wstring& workingDirectory, LineSink sink, int& exitCode) {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &attributes, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = commandLine;
    const BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startup, &process);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        sink(L"error: could not start " + commandLine);
        return false;
    }
    std::string pending;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readEnd, buffer, sizeof buffer, &read, nullptr) && read > 0) {
        pending.append(buffer, read);
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            sink(Decode(line));
            pending.erase(0, newline + 1);
        }
    }
    if (!pending.empty()) sink(Decode(pending));
    CloseHandle(readEnd);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    exitCode = static_cast<int>(code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return true;
}

bool StartDetached(const std::wstring& executable, const std::wstring& workingDirectory, const std::vector<std::pair<std::wstring, std::wstring>>& environment,
                   const std::wstring& logFile, std::wstring& error) {
    for (const auto& [name, value] : environment) {
        SetEnvironmentVariableW(name.c_str(), value.empty() ? nullptr : value.c_str());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    HANDLE log = INVALID_HANDLE_VALUE;
    BOOL inherit = FALSE;
    if (!logFile.empty()) {
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        log = CreateFileW(logFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log != INVALID_HANDLE_VALUE) {
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = log;
            startup.hStdError = log;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            inherit = TRUE;
        }
    }
    PROCESS_INFORMATION process{};
    std::wstring commandLine = Quote(executable);
    const BOOL started = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, inherit, inherit ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE,
                                        nullptr, workingDirectory.c_str(), &startup, &process);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!started) {
        error = L"CreateProcess failed with code " + std::to_wstring(GetLastError());
        return false;
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return true;
}

}  // namespace launcher
