#pragma once

namespace launcher {

struct SaveActivity {
    int savesDone = 0;
    int lastSavedSlot = 0;
    int activeSlot = 0;
};

bool PushLiveSaveSlot(unsigned long processId, int slot);

bool ReadLiveSave(unsigned long processId, SaveActivity& activity);

bool SavePromptPending(unsigned long processId, int& activeSlot);

bool AcceptSavePrompt(unsigned long processId);

bool SavePromptOpen(unsigned long processId);

void AnswerSavePrompt(unsigned long processId, int choice);

}  // namespace launcher
