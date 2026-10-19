#include <common/OrbisHle.hpp>
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

void Trace(const char* function, const char* nid) {
    static std::uint64_t calls = 0;
    OrbisHleTrace("libScePad.prx", function, nid, ++calls, __builtin_return_address(0));
}

bool Down(int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

u8 Axis(int negative, int positive) {
    if (Down(negative) && !Down(positive)) return 0;
    if (Down(positive) && !Down(negative)) return 255;
    return 128;
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
    std::fprintf(stderr, "[Pad] scePadOpen(user %d, type %d, index %d) -> handle %d; keyboard: arrows=D-pad, Z=Cross, X=Circle, C=Square, V=Triangle, Enter=Options, Q/E=L1/R1, 1/3=L2/R2, WASD/IJKL=sticks\n",
                 userId, type, index, kHandle);
    std::fflush(stderr);
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
    u32 buttons = 0;
    if (Down(VK_RETURN)) buttons |= kOptions;
    if (Down(VK_UP)) buttons |= kUp;
    if (Down(VK_RIGHT)) buttons |= kRight;
    if (Down(VK_DOWN)) buttons |= kDown;
    if (Down(VK_LEFT)) buttons |= kLeft;
    if (Down('Z') || Down(VK_SPACE)) buttons |= kCross;
    if (Down('X')) buttons |= kCircle;
    if (Down('C')) buttons |= kSquare;
    if (Down('V')) buttons |= kTriangle;
    if (Down('Q')) buttons |= kL1;
    if (Down('E')) buttons |= kR1;
    if (Down('1')) buttons |= kL2;
    if (Down('3')) buttons |= kR2;
    if (Down('T')) buttons |= kTouchPad;
    data->buttons = buttons;
    data->leftStick = {Axis('A', 'D'), Axis('W', 'S')};
    data->rightStick = {Axis('J', 'L'), Axis('I', 'K')};
    data->analogButtons.l2 = Down('1') ? 255 : 0;
    data->analogButtons.r2 = Down('3') ? 255 : 0;
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
