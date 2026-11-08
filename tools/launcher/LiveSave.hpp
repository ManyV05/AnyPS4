#pragma once

namespace launcher {

struct SaveActivity {
    int savesDone = 0;
    int lastSavedSlot = 0;
    int activeSlot = 0;
};

bool PushLiveSaveSlot(unsigned long processId, int slot);

bool ReadLiveSave(unsigned long processId, SaveActivity& activity);

}  // namespace launcher
