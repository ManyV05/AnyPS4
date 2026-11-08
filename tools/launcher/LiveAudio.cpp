#include "LiveAudio.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>

#include "AnyPS4LiveAudio.hpp"

namespace launcher {

namespace {

struct Connection {
    unsigned long processId = 0;
    HANDLE mapping = nullptr;
    anyps4::LiveAudioBlock* block = nullptr;
};

Connection g_connection;

void Disconnect() {
    if (g_connection.block != nullptr) UnmapViewOfFile(g_connection.block);
    if (g_connection.mapping != nullptr) CloseHandle(g_connection.mapping);
    g_connection = Connection{};
}

bool Connect(unsigned long processId) {
    if (g_connection.processId == processId && g_connection.block != nullptr) return true;
    Disconnect();
    wchar_t name[64];
    anyps4::LiveAudioName(processId, name);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
    if (mapping == nullptr) return false;
    void* view = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(anyps4::LiveAudioBlock));
    if (view == nullptr) {
        CloseHandle(mapping);
        return false;
    }
    g_connection.processId = processId;
    g_connection.mapping = mapping;
    g_connection.block = static_cast<anyps4::LiveAudioBlock*>(view);
    return true;
}

}  // namespace

unsigned long FindRunningTitle(const std::wstring& executablePath) {
    if (executablePath.empty()) return 0;
    const std::filesystem::path wanted = executablePath;
    const std::wstring fileName = wanted.filename().wstring();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    unsigned long found = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && found == 0; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, fileName.c_str()) != 0) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr) continue;
        wchar_t image[MAX_PATH * 2];
        DWORD length = static_cast<DWORD>(std::size(image));
        if (QueryFullProcessImageNameW(process, 0, image, &length)) {
            std::error_code error;
            if (std::filesystem::equivalent(std::filesystem::path(std::wstring(image, length)), wanted, error)) found = entry.th32ProcessID;
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}

bool PushLiveAudio(unsigned long processId, float bgmGain, float mainGain, float musicFloor) {
    if (processId == 0 || !Connect(processId)) return false;
    if (g_connection.block->state.load() != 2) return false;
    g_connection.block->bgm.store(bgmGain);
    g_connection.block->main.store(mainGain);
    g_connection.block->musicFloor.store(musicFloor);
    return true;
}

}  // namespace launcher
