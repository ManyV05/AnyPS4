#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <common/OrbisHle.hpp>
#undef ORBIS_SYSV_ABI
#define ORBIS_SYSV_ABI
#include "../orbis_impl/libSceSaveDataDialog.cpp"

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

s32 Poll(int times) {
    s32 current = kStatusNone;
    for (int k = 0; k < times; ++k) current = orbis_sceSaveDataDialogUpdateStatus();
    return current;
}

DialogParam SaveList() {
    DialogParam param{};
    param.mode = kModeList;
    param.dispType = kDispTypeSave;
    return param;
}

}  // namespace

int RunTests() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _putenv_s("ORBIS_SAVE_SLOT", "1");
    anyps4::LiveSaveBlock* block = anyps4::LiveSaveBlockOfThisProcess(1);
    Check(block != nullptr, "the live save block exists");
    Check(orbis_sceSaveDataDialogInitialize() == 0, "initialize");
    DialogResult result{};

    DialogParam save = SaveList();
    Check(orbis_sceSaveDataDialogOpen(&save) == 0 && block->promptState.load() == kPromptIdle, "without the setting a save list dialog asks nothing");
    Check(Poll(kAutoAnswerUpdates + 2) == kStatusFinished && orbis_sceSaveDataDialogGetResult(&result) == 0 && result.result == kResultOk, "it answers itself as before");
    Check(orbis_sceSaveDataDialogClose() == 0, "close");

    _putenv_s("ORBIS_SAVE_PROMPT", "1");
    DialogParam load = SaveList();
    load.dispType = 2;
    Check(orbis_sceSaveDataDialogOpen(&load) == 0 && block->promptState.load() == kPromptIdle, "a load dialog never asks for a slot");
    Check(Poll(kAutoAnswerUpdates + 2) == kStatusFinished, "the load dialog answers itself");
    Check(orbis_sceSaveDataDialogClose() == 0, "close the load dialog");

    Check(orbis_sceSaveDataDialogOpen(&save) == 0 && block->promptState.load() == kPromptAsked, "a save list dialog asks the launcher");
    Check(Poll(kAutoAnswerUpdates + 5) == kStatusRunning, "the dialog keeps waiting while nobody answers");
    block->launcherPid.store(static_cast<std::int32_t>(GetCurrentProcessId()));
    block->promptState.store(kPromptShown);
    Check(Poll(kAutoAnswerUpdates + 5) == kStatusRunning, "the dialog keeps waiting while the question is on screen");
    block->promptChoice.store(3);
    block->promptState.store(kPromptAnswered);
    Check(Poll(1) == kStatusFinished, "the answer finishes the dialog");
    Check(block->requestedSlot.load() == 3 && block->promptState.load() == kPromptIdle, "the chosen slot is requested and the question is cleared");
    Check(orbis_sceSaveDataDialogGetResult(&result) == 0 && result.result == kResultOk, "a chosen slot is an OK result");
    Check(orbis_sceSaveDataDialogClose() == 0, "close after the answer");

    Check(orbis_sceSaveDataDialogOpen(&save) == 0 && block->promptState.load() == kPromptAsked, "the next save asks again");
    block->promptChoice.store(0);
    block->promptState.store(kPromptAnswered);
    Check(Poll(1) == kStatusFinished && block->requestedSlot.load() == 3, "a canceled question keeps the slot");
    Check(orbis_sceSaveDataDialogGetResult(&result) == 0 && result.result == kResultUserCanceled, "a canceled question is a canceled result");
    Check(orbis_sceSaveDataDialogClose() == 0, "close after the cancel");

    Check(orbis_sceSaveDataDialogOpen(&save) == 0 && block->promptState.load() == kPromptAsked, "ask once more");
    std::this_thread::sleep_for(std::chrono::milliseconds(kPromptAnswerMilliseconds + 300));
    Check(Poll(1) == kStatusRunning && block->promptState.load() == kPromptIdle, "a launcher that never answers is given up on");
    Check(Poll(kAutoAnswerUpdates + 2) == kStatusFinished && orbis_sceSaveDataDialogGetResult(&result) == 0 && result.result == kResultOk, "the save then goes ahead in the current slot");
    Check(orbis_sceSaveDataDialogClose() == 0, "close the fallback dialog");

    Check(orbis_sceSaveDataDialogOpen(&save) == 0, "ask a last time");
    block->launcherPid.store(0x7FFFFFF0);
    block->promptState.store(kPromptShown);
    Check(Poll(1) == kStatusRunning && block->promptState.load() == kPromptIdle, "a launcher that closed is given up on");
    Check(orbis_sceSaveDataDialogClose() == 0, "close");

    Check(orbis_sceSaveDataDialogOpen(&save) == 0 && block->promptState.load() == kPromptAsked, "ask and close at once");
    Check(orbis_sceSaveDataDialogClose() == 0 && block->promptState.load() == kPromptIdle, "closing the dialog withdraws the question");
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

int main() {
    return RunTests();
}
