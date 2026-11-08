#pragma once

#include <functional>
#include <string>

namespace launcher {

struct SlotChoice {
    int slot = 0;
    bool withdrawn = false;
};

SlotChoice PickSaveSlot(const std::wstring& installDirectory, int activeSlot, const std::function<bool()>& stillWanted);

}  // namespace launcher
