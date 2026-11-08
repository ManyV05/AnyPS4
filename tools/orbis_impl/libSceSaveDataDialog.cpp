#include <common/OrbisHle.hpp>
#include "../../3rdparty/shadps4-ngs2/shims/AnyPS4LiveSave.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using u64 = std::uint64_t;

constexpr s32 kStatusNone = 0;
constexpr s32 kStatusInitialized = 1;
constexpr s32 kStatusRunning = 2;
constexpr s32 kStatusFinished = 3;
constexpr s32 kErrorNotInitialized = static_cast<s32>(0x80B80004);
constexpr s32 kErrorNotFinished = static_cast<s32>(0x80B80005);
constexpr s32 kErrorInvalidPointer = static_cast<s32>(0x80B80006);

constexpr u32 kModeList = 1;
constexpr u32 kModeUserMsg = 2;
constexpr u32 kModeSystemMsg = 3;
constexpr u32 kModeErrorCode = 4;
constexpr u32 kModeProgressBar = 5;
constexpr u32 kButtonTypeNone = 2;
constexpr u32 kSystemMsgProgress = 5;
constexpr u32 kFocusPosDirName = 6;
constexpr u32 kButtonIdInvalid = 0;
constexpr u32 kButtonIdOk = 1;
constexpr u32 kAutoAnswerUpdates = 10;
constexpr u32 kDispTypeSave = 1;
constexpr s32 kResultOk = 0;
constexpr s32 kResultUserCanceled = 1;
constexpr int kSaveSlotCount = 5;
constexpr ULONGLONG kPromptAnswerMilliseconds = 2000;
constexpr std::int32_t kPromptIdle = 0;
constexpr std::int32_t kPromptAsked = 1;
constexpr std::int32_t kPromptShown = 2;
constexpr std::int32_t kPromptAnswered = 3;

struct DirName {
    char data[32];
};

struct BaseParam {
    u64 size;
    u8 reserved[36];
    u32 magic;
};

struct Items {
    s32 userId;
    s32 pad0;
    const void* titleId;
    const DirName* dirName;
    u32 dirNameNum;
    s32 pad1;
    const void* newItem;
    u32 focusPos;
    s32 pad2;
    const DirName* focusPosDirName;
    u32 itemStyle;
    u8 reserved[32];
};

struct UserMessageParam {
    u32 buttonType;
    u32 msgType;
    const char* msg;
    u8 reserved[32];
};

struct SystemMessageParam {
    u32 msgType;
    s32 pad0;
    u64 value;
    u8 reserved[32];
};

struct ErrorCodeParam {
    u32 errorCode;
    u8 reserved[32];
};

struct ProgressBarParam {
    u32 barType;
    s32 pad0;
    const char* msg;
    u32 sysMsgType;
    u8 reserved[28];
};

struct DialogParam {
    BaseParam base;
    s32 size;
    u32 mode;
    u32 dispType;
    s32 pad0;
    const void* animParam;
    const Items* items;
    const UserMessageParam* userMsgParam;
    const SystemMessageParam* sysMsgParam;
    const ErrorCodeParam* errorCodeParam;
    const ProgressBarParam* progressBarParam;
    void* userData;
    const void* optionParam;
    u8 reserved[24];
};

static_assert(offsetof(DialogParam, mode) == 52, "OrbisSaveDataDialogParam layout");
static_assert(offsetof(DialogParam, items) == 72, "OrbisSaveDataDialogParam layout");
static_assert(offsetof(DialogParam, userData) == 112, "OrbisSaveDataDialogParam layout");
static_assert(offsetof(Items, focusPosDirName) == 48, "OrbisSaveDataDialogItems layout");

struct DialogResult {
    u32 mode;
    s32 result;
    u32 buttonId;
    s32 pad0;
    DirName* dirName;
    void* param;
    void* userData;
    u8 reserved[32];
};

s32 status = kStatusNone;
u32 openedMode = 0;
u32 answerButton = kButtonIdInvalid;
bool answersItself = false;
bool haveSelection = false;
char selection[32] = {};
void* openedUserData = nullptr;
u32 updatesSinceOpen = 0;
u32 progressValue = 0;
bool prompting = false;
bool canceled = false;
ULONGLONG promptAskedAt = 0;

void Trace(const char* function, const char* nid) {
    static std::uint64_t calls = 0;
    OrbisHleTrace("libSceSaveDataDialog.prx", function, nid, ++calls, __builtin_return_address(0));
}

void Log(const char* text) {
    std::fprintf(stderr, "[SaveDataDialog] %s\n", text);
    std::fflush(stderr);
}

anyps4::LiveSaveBlock* PromptBlock() {
    const char* text = std::getenv("ORBIS_SAVE_SLOT");
    const int slot = text != nullptr ? std::atoi(text) : 1;
    return anyps4::LiveSaveBlockOfThisProcess(slot >= 1 && slot <= kSaveSlotCount ? slot : 1);
}

bool LauncherAlive(const anyps4::LiveSaveBlock& block) {
    const auto pid = static_cast<DWORD>(block.launcherPid.load());
    if (pid == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

bool StartSlotPrompt() {
    const char* enabled = std::getenv("ORBIS_SAVE_PROMPT");
    if (enabled == nullptr || enabled[0] != '1') return false;
    anyps4::LiveSaveBlock* block = PromptBlock();
    if (block == nullptr) return false;
    AllowSetForegroundWindow(ASFW_ANY);
    block->promptChoice.store(0);
    block->promptState.store(kPromptAsked);
    promptAskedAt = GetTickCount64();
    prompting = true;
    return true;
}

void DropSlotPrompt(const char* reason) {
    anyps4::LiveSaveBlock* block = PromptBlock();
    if (block != nullptr) block->promptState.store(kPromptIdle);
    prompting = false;
    answersItself = true;
    char text[160];
    std::snprintf(text, sizeof text, "%s: saving in the current slot", reason);
    Log(text);
}

void PollSlotPrompt() {
    anyps4::LiveSaveBlock* block = PromptBlock();
    if (block == nullptr) {
        DropSlotPrompt("no live save block");
        return;
    }
    switch (block->promptState.load()) {
    case kPromptAsked:
        if (GetTickCount64() - promptAskedAt > kPromptAnswerMilliseconds) DropSlotPrompt("the launcher did not answer the slot question");
        break;
    case kPromptShown:
        if (!LauncherAlive(*block)) DropSlotPrompt("the launcher closed while asking for a slot");
        break;
    case kPromptAnswered: {
        const int choice = block->promptChoice.load();
        block->promptState.store(kPromptIdle);
        prompting = false;
        canceled = choice < 1 || choice > kSaveSlotCount;
        if (!canceled) block->requestedSlot.store(choice);
        status = kStatusFinished;
        char text[120];
        if (canceled) std::snprintf(text, sizeof text, "finished: the save was canceled in the slot question");
        else std::snprintf(text, sizeof text, "finished: save slot %d chosen", choice);
        Log(text);
        break;
    }
    default:
        DropSlotPrompt("the slot question was withdrawn");
        break;
    }
}

const char* ModeName(u32 mode) {
    switch (mode) {
    case kModeList: return "list";
    case kModeUserMsg: return "user message";
    case kModeSystemMsg: return "system message";
    case kModeErrorCode: return "error code";
    case kModeProgressBar: return "progress bar";
    default: return "invalid";
    }
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
    if (status != kStatusNone) Log("terminate");
    status = kStatusNone;
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogOpen(const DialogParam* param) {
    Trace("sceSaveDataDialogOpen", "4tPhsP6FpDI");
    if (status == kStatusNone) return kErrorNotInitialized;
    if (param == nullptr) return kErrorInvalidPointer;
    openedMode = param->mode;
    openedUserData = param->userData;
    updatesSinceOpen = 0;
    progressValue = 0;
    haveSelection = false;
    std::memset(selection, 0, sizeof selection);
    answerButton = kButtonIdInvalid;
    answersItself = true;
    prompting = false;
    canceled = false;
    char detail[200] = {};
    const Items* items = param->items;
    if (items != nullptr) {
        const DirName* chosen = nullptr;
        if (items->focusPos == kFocusPosDirName && items->focusPosDirName != nullptr) chosen = items->focusPosDirName;
        else if (items->dirName != nullptr && items->dirNameNum > 0) chosen = &items->dirName[0];
        if (chosen != nullptr) {
            std::memcpy(selection, chosen->data, sizeof selection - 1);
            haveSelection = true;
        }
        std::snprintf(detail, sizeof detail, ", %u existing item(s)%s, selection \"%s\"", items->dirNameNum, items->newItem != nullptr ? " + new item" : "",
                      selection);
    }
    switch (openedMode) {
    case kModeList:
        answerButton = kButtonIdInvalid;
        if (param->dispType == kDispTypeSave && StartSlotPrompt()) answersItself = false;
        break;
    case kModeUserMsg: {
        const u32 buttonType = param->userMsgParam != nullptr ? param->userMsgParam->buttonType : 0;
        answersItself = buttonType != kButtonTypeNone;
        answerButton = answersItself ? kButtonIdOk : kButtonIdInvalid;
        if (param->userMsgParam != nullptr && param->userMsgParam->msg != nullptr) {
            std::snprintf(detail, sizeof detail, ", buttons %u, text \"%.80s\"", buttonType, param->userMsgParam->msg);
        }
        break;
    }
    case kModeSystemMsg: {
        const u32 msgType = param->sysMsgParam != nullptr ? param->sysMsgParam->msgType : 0;
        answersItself = msgType != kSystemMsgProgress;
        answerButton = answersItself ? kButtonIdOk : kButtonIdInvalid;
        std::snprintf(detail, sizeof detail, ", system message type %u", msgType);
        break;
    }
    case kModeErrorCode:
        answerButton = kButtonIdOk;
        if (param->errorCodeParam != nullptr) std::snprintf(detail, sizeof detail, ", error code 0x%08x", param->errorCodeParam->errorCode);
        break;
    case kModeProgressBar:
        answersItself = false;
        break;
    default:
        break;
    }
    status = kStatusRunning;
    char text[400];
    std::snprintf(text, sizeof text, "open: mode %u (%s), display type %u%s; %s", openedMode, ModeName(openedMode), param->dispType, detail,
                  prompting ? "asks the launcher which save slot to use" : answersItself ? "answered with the positive button after a few status updates" : "stays open until the title closes it");
    Log(text);
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogUpdateStatus() {
    Trace("sceSaveDataDialogUpdateStatus", "KK3Bdg1RWK0");
    if (status == kStatusRunning && prompting) PollSlotPrompt();
    if (status == kStatusRunning && answersItself && ++updatesSinceOpen > kAutoAnswerUpdates) {
        status = kStatusFinished;
        char text[120];
        std::snprintf(text, sizeof text, "finished: mode %s, button %u, selection \"%s\"", ModeName(openedMode), answerButton, selection);
        Log(text);
    }
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
    if (status != kStatusFinished) return kErrorNotFinished;
    result->mode = openedMode;
    result->result = canceled ? kResultUserCanceled : kResultOk;
    result->buttonId = answerButton;
    result->userData = openedUserData;
    if (result->dirName != nullptr) {
        std::memset(result->dirName->data, 0, sizeof result->dirName->data);
        if (haveSelection) std::memcpy(result->dirName->data, selection, sizeof result->dirName->data - 1);
    }
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogClose() {
    Trace("sceSaveDataDialogClose", "fH46Lag88XY");
    if (prompting) {
        anyps4::LiveSaveBlock* block = PromptBlock();
        if (block != nullptr) block->promptState.store(kPromptIdle);
        prompting = false;
    }
    if (status == kStatusRunning) {
        // Like the system (and shadPS4's SaveDialogUi::Finish): closing a running
        // dialog ends it with no button, FINISHED, and the title then polls
        // UpdateStatus for FINISHED and reads the result. Leaving it at
        // INITIALIZED kept the title waiting forever after its load progress bar.
        answerButton = kButtonIdInvalid;
        status = kStatusFinished;
        char text[120];
        std::snprintf(text, sizeof text, "close: mode %s after %u status updates -> finished", ModeName(openedMode), updatesSinceOpen);
        Log(text);
    }
    return 0;
}

ORBIS_SYSV_ABI s32 orbis_sceSaveDataDialogProgressBarSetValue(u32, u32 value) {
    Trace("sceSaveDataDialogProgressBarSetValue", "hay1CfTmLyA");
    progressValue = value;
    return 0;
}

}
