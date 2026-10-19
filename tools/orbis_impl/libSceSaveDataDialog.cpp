#include <common/OrbisHle.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using u32 = std::uint32_t;
using s32 = std::int32_t;

constexpr s32 kStatusNone = 0;
constexpr s32 kStatusInitialized = 1;
constexpr s32 kStatusRunning = 2;
constexpr s32 kStatusFinished = 3;
constexpr s32 kErrorNotInitialized = static_cast<s32>(0x80B80004);
constexpr s32 kErrorInvalidPointer = static_cast<s32>(0x80B80006);

struct DialogResult {
    u32 mode;
    s32 result;
    u32 buttonId;
    u32 pad;
    void* dirName;
    void* param;
    void* userData;
    std::uint8_t reserved[32];
};

s32 status = kStatusNone;
u32 openedMode = 0;
void* openedUserData = nullptr;
u32 updatesSinceOpen = 0;

void Trace(const char* function, const char* nid) {
    static std::uint64_t calls = 0;
    OrbisHleTrace("libSceSaveDataDialog.prx", function, nid, ++calls, __builtin_return_address(0));
}

void Log(const char* text) {
    std::fprintf(stderr, "[SaveDataDialog] %s\n", text);
    std::fflush(stderr);
}

}  // namespace

extern "C" {

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogInitialize() {
    Trace("sceSaveDataDialogInitialize", "s9e3+YpRnzw");
    status = kStatusInitialized;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogTerminate() {
    Trace("sceSaveDataDialogTerminate", "YuH2FA7azqQ");
    status = kStatusNone;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogOpen(const u32* param) {
    Trace("sceSaveDataDialogOpen", "4tPhsP6FpDI");
    if (status == kStatusNone) return kErrorNotInitialized;
    openedMode = param != nullptr ? param[1] : 0;
    openedUserData = nullptr;
    updatesSinceOpen = 0;
    status = kStatusRunning;
    char text[160];
    std::snprintf(text, sizeof text, "sceSaveDataDialogOpen: mode %u (size %u); the dialog is answered automatically with OK after a few frames", openedMode,
                  param != nullptr ? param[0] : 0u);
    Log(text);
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogUpdateStatus() {
    Trace("sceSaveDataDialogUpdateStatus", "KK3Bdg1RWK0");
    if (status == kStatusRunning && ++updatesSinceOpen > 30) status = kStatusFinished;
    return status;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogGetStatus() {
    Trace("sceSaveDataDialogGetStatus", "iGzkT8L5dwg");
    return status;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogIsReadyToDisplay() {
    Trace("sceSaveDataDialogIsReadyToDisplay", "en7gNVnh878");
    return 1;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogGetResult(DialogResult* result) {
    Trace("sceSaveDataDialogGetResult", "yEiJ-qqr6Cg");
    if (result == nullptr) return kErrorInvalidPointer;
    if (status != kStatusFinished) return static_cast<s32>(0x80B80005);
    result->mode = openedMode;
    result->result = 0;
    result->buttonId = 1;
    result->userData = openedUserData;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogClose() {
    Trace("sceSaveDataDialogClose", "fH46Lag88XY");
    status = kStatusInitialized;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogProgressBarSetValue(u32, u32) {
    Trace("sceSaveDataDialogProgressBarSetValue", "hay1CfTmLyA");
    return 0;
}

}
