#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace anyps4 {

struct PadKeyName {
    const char* name;
    int key;
};

inline const PadKeyName* PadKeyTable(std::size_t& count) {
    static const PadKeyName table[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"RETURN", VK_RETURN}, {"SPACE", VK_SPACE},
        {"TAB", VK_TAB}, {"ESCAPE", VK_ESCAPE}, {"BACK", VK_BACK}, {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"SHIFT", VK_SHIFT},
        {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL}, {"CTRL", VK_CONTROL}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"ALT", VK_MENU},
        {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END}, {"PRIOR", VK_PRIOR}, {"NEXT", VK_NEXT},
        {"CAPITAL", VK_CAPITAL}, {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1}, {"NUMPAD2", VK_NUMPAD2}, {"NUMPAD3", VK_NUMPAD3},
        {"NUMPAD4", VK_NUMPAD4}, {"NUMPAD5", VK_NUMPAD5}, {"NUMPAD6", VK_NUMPAD6}, {"NUMPAD7", VK_NUMPAD7}, {"NUMPAD8", VK_NUMPAD8},
        {"NUMPAD9", VK_NUMPAD9}, {"ADD", VK_ADD}, {"SUBTRACT", VK_SUBTRACT}, {"MULTIPLY", VK_MULTIPLY}, {"DIVIDE", VK_DIVIDE},
        {"DECIMAL", VK_DECIMAL}, {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6},
        {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12}, {"COMMA", VK_OEM_COMMA},
        {"PERIOD", VK_OEM_PERIOD}, {"MINUS", VK_OEM_MINUS}, {"PLUS", VK_OEM_PLUS}, {"NONE", 0},
    };
    count = sizeof table / sizeof table[0];
    return table;
}

inline int PadKeyFromName(const std::string& text) {
    std::string name;
    for (char c : text) name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    while (!name.empty() && (name.back() == ' ' || name.back() == '\r' || name.back() == '\n')) name.pop_back();
    if (name.empty()) return 0;
    if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) return name[0];
    if (name.rfind("VK:", 0) == 0) return std::atoi(name.c_str() + 3);
    std::size_t count = 0;
    const PadKeyName* table = PadKeyTable(count);
    for (std::size_t k = 0; k < count; ++k) {
        if (name == table[k].name) return table[k].key;
    }
    return 0;
}

inline std::string PadKeyName_(int key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, static_cast<char>(key));
    std::size_t count = 0;
    const PadKeyName* table = PadKeyTable(count);
    for (std::size_t k = 0; k < count; ++k) {
        if (key == table[k].key) return table[k].name;
    }
    char text[16];
    std::snprintf(text, sizeof text, "VK:%d", key);
    return text;
}

struct PadBinding {
    const char* setting;
    const char* label;
    int defaultKey;
};

inline const PadBinding* PadBindings(std::size_t& count) {
    static const PadBinding bindings[] = {
        {"cross", "Cross", 'Z'}, {"circle", "Circle", 'X'}, {"square", "Square", 'C'}, {"triangle", "Triangle", 'V'},
        {"up", "D-pad up", VK_UP}, {"down", "D-pad down", VK_DOWN}, {"left", "D-pad left", VK_LEFT}, {"right", "D-pad right", VK_RIGHT},
        {"l1", "L1", 'Q'}, {"r1", "R1", 'E'}, {"l2", "L2", '1'}, {"r2", "R2", '3'}, {"l3", "L3", 'F'}, {"r3", "R3", 'G'},
        {"options", "OPTIONS", VK_RETURN}, {"touchpad", "Touch pad", 'T'},
        {"ls_up", "Left stick up", 'W'}, {"ls_down", "Left stick down", 'S'}, {"ls_left", "Left stick left", 'A'}, {"ls_right", "Left stick right", 'D'},
        {"rs_up", "Right stick up", 'I'}, {"rs_down", "Right stick down", 'K'}, {"rs_left", "Right stick left", 'J'}, {"rs_right", "Right stick right", 'L'},
    };
    count = sizeof bindings / sizeof bindings[0];
    return bindings;
}

}  // namespace anyps4
