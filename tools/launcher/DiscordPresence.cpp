#include "DiscordPresence.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>

namespace launcher {

namespace {

constexpr std::int32_t kOpHandshake = 0;
constexpr std::int32_t kOpFrame = 1;
constexpr std::int32_t kOpClose = 2;
constexpr std::int32_t kOpPing = 3;
constexpr std::int32_t kOpPong = 4;
constexpr DWORD kReadyWaitMilliseconds = 6000;
constexpr DWORD kPollMilliseconds = 400;
constexpr std::uint32_t kMaxPayload = 1u << 16;

constexpr const char* kDetails = "Playing The Witch and the Hundred Knight: Revival Edition";
constexpr const char* kState = "AnyPS4 Runtime";

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t c : text) out.push_back(static_cast<char>(c));
    return out;
}

HANDLE OpenPipe() {
    wchar_t overrideName[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"ANYPS4_DISCORD_PIPE", overrideName, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return CreateFileW(overrideName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    for (int index = 0; index < 10; ++index) {
        wchar_t name[64];
        swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", index);
        HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
    }
    return INVALID_HANDLE_VALUE;
}

bool WriteFrame(HANDLE pipe, std::int32_t opcode, const std::string& payload) {
    std::vector<char> frame(8 + payload.size());
    const std::int32_t length = static_cast<std::int32_t>(payload.size());
    std::copy_n(reinterpret_cast<const char*>(&opcode), 4, frame.begin());
    std::copy_n(reinterpret_cast<const char*>(&length), 4, frame.begin() + 4);
    std::copy(payload.begin(), payload.end(), frame.begin() + 8);
    DWORD written = 0;
    return WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) && written == frame.size();
}

enum class Read { Nothing, Frame, Broken };

Read ReadFrame(HANDLE pipe, std::int32_t& opcode, std::string& payload) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return Read::Broken;
    if (available < 8) return Read::Nothing;
    std::int32_t header[2] = {};
    DWORD read = 0;
    if (!ReadFile(pipe, header, sizeof header, &read, nullptr) || read != sizeof header) return Read::Broken;
    opcode = header[0];
    if (header[1] < 0 || static_cast<std::uint32_t>(header[1]) > kMaxPayload) return Read::Broken;
    payload.assign(static_cast<std::size_t>(header[1]), '\0');
    DWORD got = 0;
    while (got < payload.size()) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, payload.data() + got, static_cast<DWORD>(payload.size() - got), &chunk, nullptr) || chunk == 0) return Read::Broken;
        got += chunk;
    }
    return Read::Frame;
}

std::string ActivityFrame(DWORD processId, std::int64_t start, int nonce, bool clear) {
    std::string json = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(processId);
    if (!clear) {
        json += std::string(",\"activity\":{\"details\":\"") + kDetails + "\",\"state\":\"" + kState + "\",\"timestamps\":{\"start\":" + std::to_string(start) + "}}";
    }
    json += "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
    return json;
}

void Run(std::wstring clientId, HANDLE process, HANDLE stop, LineSink log) {
    HANDLE pipe = OpenPipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        log(L"Discord Rich Presence: Discord is not running (no IPC pipe).");
        CloseHandle(process);
        return;
    }
    bool ready = false;
    const std::string handshake = "{\"v\":1,\"client_id\":\"" + Narrow(clientId) + "\"}";
    if (WriteFrame(pipe, kOpHandshake, handshake)) {
        const DWORD began = GetTickCount();
        while (!ready && GetTickCount() - began < kReadyWaitMilliseconds) {
            std::int32_t opcode = 0;
            std::string payload;
            const Read result = ReadFrame(pipe, opcode, payload);
            if (result == Read::Broken || (result == Read::Frame && opcode == kOpClose)) break;
            if (result == Read::Frame && opcode == kOpFrame && payload.find("\"READY\"") != std::string::npos) ready = true;
            if (result == Read::Nothing && WaitForSingleObject(stop, 50) == WAIT_OBJECT_0) break;
        }
    }
    if (!ready) {
        log(L"Discord Rich Presence: Discord did not accept the application id; check it in Settings.");
        CloseHandle(pipe);
        CloseHandle(process);
        return;
    }
    const DWORD processId = GetCurrentProcessId();
    const std::int64_t start = static_cast<std::int64_t>(std::time(nullptr));
    int nonce = 1;
    bool alive = WriteFrame(pipe, kOpFrame, ActivityFrame(processId, start, nonce++, false));
    if (alive) log(L"Discord Rich Presence: showing the game with a session timer.");
    const HANDLE handles[2] = {process, stop};
    while (alive) {
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, kPollMilliseconds);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) break;
        for (Read result = Read::Frame; result == Read::Frame;) {
            std::int32_t opcode = 0;
            std::string payload;
            result = ReadFrame(pipe, opcode, payload);
            if (result == Read::Broken) alive = false;
            if (result == Read::Frame && opcode == kOpPing) WriteFrame(pipe, kOpPong, payload);
            if (result == Read::Frame && opcode == kOpClose) alive = false;
        }
    }
    WriteFrame(pipe, kOpFrame, ActivityFrame(processId, start, nonce, true));
    CloseHandle(pipe);
    CloseHandle(process);
}

}  // namespace

bool ValidDiscordClientId(const std::wstring& clientId) {
    return clientId.size() >= 5 && clientId.size() <= 24 && std::all_of(clientId.begin(), clientId.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

DiscordPresence::~DiscordPresence() {
    Stop();
}

bool DiscordPresence::Start(const std::wstring& clientId, HANDLE gameProcess, LineSink log) {
    Stop();
    if (!ValidDiscordClientId(clientId) || gameProcess == nullptr) return false;
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), gameProcess, GetCurrentProcess(), &copy, SYNCHRONIZE, FALSE, 0)) return false;
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker_ = std::thread(Run, clientId, copy, stop_, std::move(log));
    return true;
}

void DiscordPresence::Stop() {
    if (stop_ != nullptr) SetEvent(stop_);
    if (worker_.joinable()) worker_.join();
    if (stop_ != nullptr) {
        CloseHandle(stop_);
        stop_ = nullptr;
    }
}

}  // namespace launcher
