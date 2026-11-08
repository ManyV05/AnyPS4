#include "SavePicker.hpp"

#include <windows.h>

#include <chrono>
#include <ctime>
#include <filesystem>

#include "Settings.hpp"
#include "Ui.hpp"

namespace launcher {

namespace {

namespace fs = std::filesystem;

constexpr int kClientWidth = 440;
constexpr int kRowTop = 96;
constexpr int kRowHeight = 44;
constexpr int kRowGap = 8;
constexpr int kButtonTop = kRowTop + kSaveSlotCount * (kRowHeight + kRowGap) + 4;
constexpr int kClientHeight = kButtonTop + 36 + 24;
constexpr int kCancelFocus = kSaveSlotCount;
constexpr UINT_PTR kTimerWatch = 1;
const wchar_t* const kClassName = L"AnyPS4SlotPicker";

struct Picker {
    HWND window = nullptr;
    std::wstring status[kSaveSlotCount];
    int active = 1;
    int focus = 0;
    int result = 0;
    bool withdrawn = false;
    bool done = false;
    const std::function<bool()>* stillWanted = nullptr;
};

Picker g_picker;

std::wstring DescribeSlot(const fs::path& slotDirectory) {
    std::error_code error;
    fs::file_time_type newest{};
    bool found = false;
    for (fs::directory_iterator title(slotDirectory, error), end; !error && title != end; title.increment(error)) {
        std::error_code entryError;
        if (!title->is_directory(entryError)) continue;
        for (fs::directory_iterator save(title->path(), entryError); !entryError && save != end; save.increment(entryError)) {
            std::error_code timeError;
            if (!save->is_directory(timeError)) continue;
            const fs::path marker = save->path() / L"sce_sys" / L"anyps4.param";
            const fs::file_time_type time = fs::exists(marker, timeError) ? fs::last_write_time(marker, timeError) : save->last_write_time(timeError);
            if (timeError) continue;
            if (!found || time > newest) {
                newest = time;
                found = true;
            }
        }
    }
    if (!found) return L"Empty";
    const std::time_t stamp = std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(newest));
    std::tm local{};
    if (localtime_s(&local, &stamp) != 0) return L"Saved";
    wchar_t text[48];
    wcsftime(text, 48, L"Saved %d/%m/%Y %H:%M", &local);
    return text;
}

RECT RowRect(int index) {
    return ui::Rect(24, kRowTop + index * (kRowHeight + kRowGap), kClientWidth - 48, kRowHeight);
}

RECT CancelRect() {
    return ui::Rect(kClientWidth - 24 - 148, kButtonTop, 148, 36);
}

int HitTest(int x, int y) {
    const POINT point{x, y};
    for (int k = 0; k < kSaveSlotCount; ++k) {
        const RECT row = RowRect(k);
        if (PtInRect(&row, point)) return k;
    }
    const RECT cancel = CancelRect();
    return PtInRect(&cancel, point) ? kCancelFocus : -1;
}

void Finish(int slot) {
    g_picker.result = slot;
    g_picker.done = true;
    DestroyWindow(g_picker.window);
}

void Paint(HDC target, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    ui::FillSolid(dc, RECT{0, 0, width, height}, ui::kBackground);
    ui::DrawTextLine(dc, L"Save game", ui::Rect(24, 14, 392, 40), ui::kFontTitle, ui::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui::DrawTextLine(dc, L"Where do you want to save this progress? The other slots stay as they are.", ui::Rect(24, 56, 392, 34), ui::kFontSmall, ui::kMuted,
                     DT_LEFT | DT_TOP | DT_WORDBREAK);
    for (int k = 0; k < kSaveSlotCount; ++k) {
        const RECT row = RowRect(k);
        const bool focused = g_picker.focus == k;
        const bool active = g_picker.active == k + 1;
        ui::FillRound(dc, row, ui::S(10), focused ? ui::kButtonHover : ui::kCard, focused ? ui::kViolet : (active ? ui::kVioletLight : ui::kCardBorder));
        RECT name = row;
        name.left += ui::S(16);
        name.right = row.left + ui::S(150);
        ui::DrawTextLine(dc, L"Save Slot " + std::to_wstring(k + 1), name, ui::kFontButton, ui::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (active) {
            RECT tag = row;
            tag.left += ui::S(112);
            tag.right = row.left + ui::S(190);
            ui::DrawTextLine(dc, L"current", tag, ui::kFontSmall, ui::kVioletLight, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        RECT status = row;
        status.right -= ui::S(16);
        ui::DrawTextLine(dc, g_picker.status[k], status, ui::kFontSmall, g_picker.status[k] == L"Empty" ? ui::kDisabled : ui::kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    const RECT cancel = CancelRect();
    const bool cancelFocused = g_picker.focus == kCancelFocus;
    ui::FillRound(dc, cancel, ui::S(8), cancelFocused ? ui::kButtonHover : ui::kButton, cancelFocused ? ui::kViolet : ui::kCardBorder);
    ui::DrawTextLine(dc, L"Cancel save", cancel, ui::kFontButton, ui::kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    BitBlt(target, client.left, client.top, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

LRESULT CALLBACK PickerProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        Paint(dc, client);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int hit = HitTest(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
        if (hit >= 0 && hit != g_picker.focus) {
            g_picker.focus = hit;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const int hit = HitTest(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
        if (hit >= 0) Finish(hit == kCancelFocus ? 0 : hit + 1);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            Finish(0);
        } else if (wParam >= '1' && wParam < '1' + kSaveSlotCount) {
            Finish(static_cast<int>(wParam - '0'));
        } else if (wParam >= VK_NUMPAD1 && wParam < VK_NUMPAD1 + kSaveSlotCount) {
            Finish(static_cast<int>(wParam - VK_NUMPAD0));
        } else if (wParam == VK_UP || wParam == VK_DOWN) {
            const int count = kSaveSlotCount + 1;
            g_picker.focus = (g_picker.focus + (wParam == VK_DOWN ? 1 : count - 1)) % count;
            InvalidateRect(window, nullptr, FALSE);
        } else if (wParam == VK_RETURN || wParam == VK_SPACE) {
            Finish(g_picker.focus == kCancelFocus ? 0 : g_picker.focus + 1);
        }
        return 0;
    case WM_TIMER:
        if (wParam == kTimerWatch && g_picker.stillWanted != nullptr && !(*g_picker.stillWanted)()) {
            g_picker.withdrawn = true;
            Finish(0);
        }
        return 0;
    case WM_CLOSE:
        Finish(0);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kTimerWatch);
        g_picker.done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

SlotChoice PickSaveSlot(const std::wstring& installDirectory, int activeSlot, const std::function<bool()>& stillWanted) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = PickerProcedure;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kClassName;
        RegisterClassW(&windowClass);
        registered = true;
    }
    g_picker = Picker{};
    g_picker.active = activeSlot >= 1 && activeSlot <= kSaveSlotCount ? activeSlot : 1;
    g_picker.focus = g_picker.active - 1;
    g_picker.stillWanted = &stillWanted;
    for (int k = 0; k < kSaveSlotCount; ++k) g_picker.status[k] = DescribeSlot(fs::path(installDirectory) / L"savedata" / (L"slot_" + std::to_wstring(k + 1)));

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD extended = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
    RECT frame{0, 0, ui::S(kClientWidth), ui::S(kClientHeight)};
    AdjustWindowRectEx(&frame, style, FALSE, extended);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &monitor);
    const int x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
    const int y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
    g_picker.window = CreateWindowExW(extended, kClassName, L"AnyPS4 - Save game", style, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (g_picker.window == nullptr) return SlotChoice{0, true};
    ui::StyleWindow(g_picker.window);
    ShowWindow(g_picker.window, SW_SHOW);
    SetWindowPos(g_picker.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(g_picker.window);
    SetFocus(g_picker.window);
    SetTimer(g_picker.window, kTimerWatch, 100, nullptr);

    MSG message{};
    while (!g_picker.done) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (IsWindow(g_picker.window)) DestroyWindow(g_picker.window);
    return SlotChoice{g_picker.result, g_picker.withdrawn};
}

}  // namespace launcher
