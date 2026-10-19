#include <common/OrbisHle.hpp>
#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "PadKeyNames.hpp"

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

constexpr s32 kHandle = 1;
constexpr s32 kErrorInvalidArg = static_cast<s32>(0x80920001);
constexpr s32 kErrorInvalidHandle = static_cast<s32>(0x80920003);

struct AnalogStick {
    u8 x;
    u8 y;
};

struct AnalogButtons {
    u8 l2;
    u8 r2;
    u8 padding[2];
};

struct Quaternion {
    float x, y, z, w;
};

struct Vector3 {
    float x, y, z;
};

struct Touch {
    u16 x;
    u16 y;
    u8 id;
    u8 reserve[3];
};

struct TouchData {
    u8 touchNum;
    u8 reserve[3];
    u32 timeSinceTouchHeldDown;
    Touch touch[2];
};

struct ExtensionUnitData {
    u32 extensionUnitId;
    u8 reserve[1];
    u8 dataLength;
    u8 data[10];
};

struct PadData {
    u32 buttons;
    AnalogStick leftStick;
    AnalogStick rightStick;
    AnalogButtons analogButtons;
    Quaternion orientation;
    Vector3 acceleration;
    Vector3 angularVelocity;
    TouchData touchData;
    bool connected;
    u64 timestamp;
    ExtensionUnitData extensionUnitData;
    u8 connectedCount;
    u8 reserve[2];
    u8 deviceUniqueDataLen;
    u8 deviceUniqueData[12];
};

struct ControllerInformation {
    float pixelDensity;
    struct {
        u16 x;
        u16 y;
    } resolution;
    u8 deadZoneLeft;
    u8 deadZoneRight;
    u8 connectionType;
    u8 connectedCount;
    bool connected;
    u32 deviceClass;
    u8 reserve[8];
};

enum Button : u32 {
    kL3 = 0x2,
    kR3 = 0x4,
    kOptions = 0x8,
    kUp = 0x10,
    kRight = 0x20,
    kDown = 0x40,
    kLeft = 0x80,
    kL2 = 0x100,
    kR2 = 0x200,
    kL1 = 0x400,
    kR1 = 0x800,
    kTriangle = 0x1000,
    kCircle = 0x2000,
    kCross = 0x4000,
    kSquare = 0x8000,
    kTouchPad = 0x100000,
};

bool open = false;

struct Mapping {
    std::array<int, 24> keys{};
    bool xinput = true;
    bool loaded = false;
};

Mapping mapping;
bool xinputConnected = false;
std::chrono::steady_clock::time_point xinputChecked{};

std::string ExecutableDirectory() {
    char buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::string path(buffer, length);
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

void LoadMapping() {
    if (mapping.loaded) return;
    mapping.loaded = true;
    std::size_t count = 0;
    const anyps4::PadBinding* bindings = anyps4::PadBindings(count);
    for (std::size_t k = 0; k < count && k < mapping.keys.size(); ++k) mapping.keys[k] = bindings[k].defaultKey;
    const char* configured = std::getenv("ORBIS_PAD_CONFIG");
    const std::string path = configured != nullptr ? std::string(configured) : ExecutableDirectory() + "\\anyps4\\pad.ini";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "[Pad] no %s: default keyboard mapping, XInput on\n", path.c_str());
        std::fflush(stderr);
        return;
    }
    char value[64];
    for (std::size_t k = 0; k < count && k < mapping.keys.size(); ++k) {
        GetPrivateProfileStringA("pad", bindings[k].setting, "", value, sizeof value, path.c_str());
        if (value[0] != '\0') mapping.keys[k] = anyps4::PadKeyFromName(value);
    }
    mapping.xinput = GetPrivateProfileIntA("pad", "xinput", 1, path.c_str()) != 0;
    std::string summary;
    for (std::size_t k = 0; k < count && k < mapping.keys.size(); ++k) summary += std::string(k == 0 ? "" : ", ") + bindings[k].setting + "=" + anyps4::PadKeyName_(mapping.keys[k]);
    std::fprintf(stderr, "[Pad] mapping from %s: %s; XInput %s\n", path.c_str(), summary.c_str(), mapping.xinput ? "on" : "off");
    std::fflush(stderr);
}

bool ReadXInput(PadData* data, u32& buttons) {
    if (!mapping.xinput) return false;
    const auto now = std::chrono::steady_clock::now();
    if (!xinputConnected && now - xinputChecked < std::chrono::seconds(2)) return false;
    XINPUT_STATE state{};
    const DWORD result = XInputGetState(0, &state);
    xinputChecked = now;
    const bool connected = result == ERROR_SUCCESS;
    if (connected != xinputConnected) {
        std::fprintf(stderr, "[Pad] XInput gamepad %s\n", connected ? "connected" : "disconnected");
        std::fflush(stderr);
        xinputConnected = connected;
    }
    if (!connected) return false;
    const WORD w = state.Gamepad.wButtons;
    if (w & XINPUT_GAMEPAD_A) buttons |= kCross;
    if (w & XINPUT_GAMEPAD_B) buttons |= kCircle;
    if (w & XINPUT_GAMEPAD_X) buttons |= kSquare;
    if (w & XINPUT_GAMEPAD_Y) buttons |= kTriangle;
    if (w & XINPUT_GAMEPAD_DPAD_UP) buttons |= kUp;
    if (w & XINPUT_GAMEPAD_DPAD_DOWN) buttons |= kDown;
    if (w & XINPUT_GAMEPAD_DPAD_LEFT) buttons |= kLeft;
    if (w & XINPUT_GAMEPAD_DPAD_RIGHT) buttons |= kRight;
    if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) buttons |= kL1;
    if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) buttons |= kR1;
    if (w & XINPUT_GAMEPAD_LEFT_THUMB) buttons |= kL3;
    if (w & XINPUT_GAMEPAD_RIGHT_THUMB) buttons |= kR3;
    if (w & XINPUT_GAMEPAD_START) buttons |= kOptions;
    if (w & XINPUT_GAMEPAD_BACK) buttons |= kTouchPad;
    if (state.Gamepad.bLeftTrigger > 30) buttons |= kL2;
    if (state.Gamepad.bRightTrigger > 30) buttons |= kR2;
    auto axis = [](SHORT value) {
        if (value > -7849 && value < 7849) return static_cast<u8>(128);
        const int scaled = (static_cast<int>(value) + 32768) * 255 / 65535;
        return static_cast<u8>(std::clamp(scaled, 0, 255));
    };
    data->leftStick = {axis(state.Gamepad.sThumbLX), static_cast<u8>(255 - axis(state.Gamepad.sThumbLY))};
    data->rightStick = {axis(state.Gamepad.sThumbRX), static_cast<u8>(255 - axis(state.Gamepad.sThumbRY))};
    data->analogButtons.l2 = state.Gamepad.bLeftTrigger;
    data->analogButtons.r2 = state.Gamepad.bRightTrigger;
    return true;
}

struct ScriptedPress {
    std::uint64_t startMs;
    u32 buttons;
    std::uint64_t durationMs;
};

std::vector<ScriptedPress> script;
bool scriptLoaded = false;
std::chrono::steady_clock::time_point firstRead{};
u32 lastButtons = 0;
std::uint64_t readCount = 0;

void LoadScript() {
    if (scriptLoaded) return;
    scriptLoaded = true;
    const char* path = std::getenv("ORBIS_PAD_SCRIPT");
    if (path == nullptr) return;
    std::ifstream file(path);
    std::uint64_t start = 0, duration = 0;
    std::string buttons;
    while (file >> start >> buttons >> duration) {
        script.push_back({start, static_cast<u32>(std::strtoul(buttons.c_str(), nullptr, 0)), duration});
    }
    std::fprintf(stderr, "[Pad] scripted input: %zu presses from %s\n", script.size(), path);
    std::fflush(stderr);
}

u32 ScriptedButtons(std::uint64_t elapsedMs) {
    u32 buttons = 0;
    for (const auto& press : script) {
        if (elapsedMs >= press.startMs && elapsedMs < press.startMs + press.durationMs) buttons |= press.buttons;
    }
    return buttons;
}

void Trace(const char* function, const char* nid) {
    static std::uint64_t calls = 0;
    OrbisHleTrace("libScePad.prx", function, nid, ++calls, __builtin_return_address(0));
}

bool GameWindowFocused() {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(foreground, &process);
    return process == GetCurrentProcessId();
}

bool Down(int key) {
    static bool focused = false;
    static std::uint64_t checks = 0;
    if (checks++ % 64 == 0) focused = GameWindowFocused();
    return focused && (GetAsyncKeyState(key) & 0x8000) != 0;
}

u8 Axis(int negative, int positive) {
    if (Down(negative) && !Down(positive)) return 0;
    if (Down(positive) && !Down(negative)) return 255;
    return 128;
}

int Key(std::size_t binding) {
    return mapping.keys[binding];
}

}  // namespace

extern "C" {

ORBIS_SYSV_ABI s32 orbis_scePadInit() {
    Trace("scePadInit", "hv1luiJrqQM");
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_scePadOpen(s32 userId, s32 type, s32 index, const void*) {
    Trace("scePadOpen", "xk0AcarP3V4");
    if (type != 0 && type != 1) return kErrorInvalidArg;
    std::fprintf(stderr, "[Pad] scePadOpen(user %d, type %d, index %d) -> handle %d\n", userId, type, index, kHandle);
    std::fflush(stderr);
    LoadMapping();
    open = true;
    return kHandle;
}

ORBIS_SYSV_ABI s32 orbis_scePadClose(s32 handle) {
    Trace("scePadClose", "6ncge5+l5Qs");
    if (handle != kHandle) return kErrorInvalidHandle;
    open = false;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_scePadReadState(s32 handle, PadData* data) {
    Trace("scePadReadState", "YndgXqQVV7c");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (data == nullptr) return kErrorInvalidArg;
    std::memset(data, 0, sizeof *data);
    LoadMapping();
    u32 buttons = 0;
    if (Down(Key(0))) buttons |= kCross;
    if (Down(Key(1))) buttons |= kCircle;
    if (Down(Key(2))) buttons |= kSquare;
    if (Down(Key(3))) buttons |= kTriangle;
    if (Down(Key(4))) buttons |= kUp;
    if (Down(Key(5))) buttons |= kDown;
    if (Down(Key(6))) buttons |= kLeft;
    if (Down(Key(7))) buttons |= kRight;
    if (Down(Key(8))) buttons |= kL1;
    if (Down(Key(9))) buttons |= kR1;
    if (Down(Key(10))) buttons |= kL2;
    if (Down(Key(11))) buttons |= kR2;
    if (Down(Key(12))) buttons |= kL3;
    if (Down(Key(13))) buttons |= kR3;
    if (Down(Key(14))) buttons |= kOptions;
    if (Down(Key(15))) buttons |= kTouchPad;
    data->leftStick = {Axis(Key(18), Key(19)), Axis(Key(16), Key(17))};
    data->rightStick = {Axis(Key(22), Key(23)), Axis(Key(20), Key(21))};
    data->analogButtons.l2 = Down(Key(10)) ? 255 : 0;
    data->analogButtons.r2 = Down(Key(11)) ? 255 : 0;
    ReadXInput(data, buttons);
    LoadScript();
    const auto now = std::chrono::steady_clock::now();
    if (readCount++ == 0) firstRead = now;
    const auto elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - firstRead).count());
    buttons |= ScriptedButtons(elapsedMs);
    if (buttons != lastButtons) {
        std::fprintf(stderr, "[Pad] t=%llu ms buttons %#x -> %#x (read #%llu)\n", static_cast<unsigned long long>(elapsedMs), lastButtons, buttons,
                     static_cast<unsigned long long>(readCount));
        std::fflush(stderr);
        lastButtons = buttons;
    }
    data->buttons = buttons;
    data->orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    data->acceleration = {0.0f, 1.0f, 0.0f};
    data->connected = true;
    data->timestamp = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    data->connectedCount = 1;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_scePadGetControllerInformation(s32 handle, ControllerInformation* info) {
    Trace("scePadGetControllerInformation", "gjP9-KQzoUk");
    if (handle != kHandle) return kErrorInvalidHandle;
    if (info == nullptr) return kErrorInvalidArg;
    std::memset(info, 0, sizeof *info);
    info->pixelDensity = 1.0f;
    info->resolution.x = 1920;
    info->resolution.y = 943;
    info->deadZoneLeft = 13;
    info->deadZoneRight = 13;
    info->connectionType = 0;
    info->connectedCount = 1;
    info->connected = true;
    info->deviceClass = 0;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_scePadSetLightBar(s32 handle, const void*) {
    Trace("scePadSetLightBar", "RR4novUEENY");
    return handle == kHandle ? 0 : kErrorInvalidHandle;
}

ORBIS_SYSV_ABI s32 orbis_scePadSetMotionSensorState(s32 handle, bool) {
    Trace("scePadSetMotionSensorState", "clVvL4ZDntw");
    return handle == kHandle ? 0 : kErrorInvalidHandle;
}

ORBIS_SYSV_ABI s32 orbis_scePadSetVibration(s32 handle, const void*) {
    Trace("scePadSetVibration", "yFVnOdGxvZY");
    return handle == kHandle ? 0 : kErrorInvalidHandle;
}

ORBIS_SYSV_ABI s32 orbis_scePadResetOrientation(s32 handle) {
    Trace("scePadResetOrientation", "rIZnR6eSpvk");
    return handle == kHandle ? 0 : kErrorInvalidHandle;
}

}
