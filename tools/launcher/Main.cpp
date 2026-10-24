#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "PadKeyNames.hpp"
#include "Process.hpp"
#include "Settings.hpp"

namespace {

using launcher::Settings;

constexpr int kIdGame = 100;
constexpr int kIdGameBrowse = 101;
constexpr int kIdInstallDir = 102;
constexpr int kIdInstallBrowse = 103;
constexpr int kIdInstall = 104;
constexpr int kIdPlay = 105;
constexpr int kIdOpenFolder = 106;
constexpr int kIdXInput = 107;
constexpr int kIdPresent = 108;
constexpr int kIdValidation = 109;
constexpr int kIdPipelineCache = 110;
constexpr int kIdHeadless = 111;
constexpr int kIdBgm = 112;
constexpr int kIdMain = 113;
constexpr int kIdFloor = 114;
constexpr int kIdBgmLabel = 115;
constexpr int kIdMainLabel = 116;
constexpr int kIdFloorLabel = 117;
constexpr int kIdLog = 118;
constexpr int kIdDiagnostics = 119;
constexpr int kIdFps = 120;
constexpr int kIdKeyBase = 200;
constexpr UINT kMessageLogLine = WM_APP + 1;
constexpr UINT kMessageInstallDone = WM_APP + 2;

Settings g_settings;
HWND g_window = nullptr;
HWND g_log = nullptr;
HFONT g_font = nullptr;
int g_capturing = -1;
bool g_installing = false;

std::wstring Widen(const char* text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length > 0 ? length - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
    return wide;
}

std::wstring Narrow(const std::wstring&) = delete;

std::wstring WindowText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    return text;
}

HWND Control(int id) {
    return GetDlgItem(g_window, id);
}

HWND Create(const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id, DWORD exStyle = 0) {
    HWND control = CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, g_window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    return control;
}

void AppendLog(const std::wstring& line) {
    const int length = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, length, length);
    const std::wstring text = line + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

void PostLog(const std::wstring& line) {
    PostMessageW(g_window, kMessageLogLine, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
}

std::wstring BrowseFolder(const std::wstring& title) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title.c_str());
    std::wstring result;
    if (SUCCEEDED(dialog->Show(g_window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

void ReadControlsIntoSettings() {
    g_settings.gameDirectory = WindowText(Control(kIdGame));
    g_settings.installDirectory = WindowText(Control(kIdInstallDir));
    g_settings.xinput = SendMessageW(Control(kIdXInput), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.validation = SendMessageW(Control(kIdValidation), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.pipelineCache = SendMessageW(Control(kIdPipelineCache), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.headless = SendMessageW(Control(kIdHeadless), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.diagnostics = SendMessageW(Control(kIdDiagnostics), BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_settings.presentMode = WindowText(Control(kIdPresent));
    const int fpsChoice = static_cast<int>(SendMessageW(Control(kIdFps), CB_GETCURSEL, 0, 0));
    g_settings.fpsCap = fpsChoice == 0 ? 30 : fpsChoice == 2 ? 0 : 60;
    g_settings.bgmVolume = static_cast<int>(SendMessageW(Control(kIdBgm), TBM_GETPOS, 0, 0));
    g_settings.mainVolume = static_cast<int>(SendMessageW(Control(kIdMain), TBM_GETPOS, 0, 0));
    g_settings.musicFloor = static_cast<int>(SendMessageW(Control(kIdFloor), TBM_GETPOS, 0, 0));
}

void UpdateSliderLabels() {
    SetWindowTextW(Control(kIdBgmLabel), (L"Music (BGM): " + std::to_wstring(SendMessageW(Control(kIdBgm), TBM_GETPOS, 0, 0)) + L" %").c_str());
    SetWindowTextW(Control(kIdMainLabel), (L"Voices and effects: " + std::to_wstring(SendMessageW(Control(kIdMain), TBM_GETPOS, 0, 0)) + L" %").c_str());
    SetWindowTextW(Control(kIdFloorLabel), (L"Music floor when the title mutes it: " + std::to_wstring(SendMessageW(Control(kIdFloor), TBM_GETPOS, 0, 0)) + L" %").c_str());
}

std::wstring FindExecutable() {
    if (!g_settings.executable.empty() && std::filesystem::exists(g_settings.executable)) return g_settings.executable;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(g_settings.installDirectory, error)) {
        const std::wstring name = entry.path().filename().wstring();
        if (entry.is_regular_file(error) && entry.path().extension() == L".exe" && name.find(L"_dbg") == std::wstring::npos) return entry.path().wstring();
    }
    return {};
}

void Play() {
    ReadControlsIntoSettings();
    launcher::SaveSettings(g_settings);
    launcher::SavePadConfiguration(g_settings);
    const std::wstring executable = FindExecutable();
    if (executable.empty()) {
        AppendLog(L"No executable in the install folder yet: run Install first.");
        return;
    }
    g_settings.executable = executable;
    wchar_t number[32];
    std::vector<std::pair<std::wstring, std::wstring>> environment;
    environment.emplace_back(L"APS5_NO_WRITE_WATCH", g_settings.writeWatch ? L"" : L"1");
    environment.emplace_back(L"ORBIS_GNM_PRESENT_MODE", g_settings.presentMode);
    environment.emplace_back(L"ORBIS_RENDER_FPS_CAP", std::to_wstring(g_settings.fpsCap));
    environment.emplace_back(L"ORBIS_GNM_VALIDATION", g_settings.validation ? L"1" : L"");
    environment.emplace_back(L"ORBIS_GNM_NO_PIPELINE_CACHE", g_settings.pipelineCache ? L"" : L"1");
    environment.emplace_back(L"ORBIS_GNM_HEADLESS", g_settings.headless ? L"1" : L"");
    swprintf(number, 32, L"%.2f", g_settings.bgmVolume / 100.0);
    environment.emplace_back(L"ORBIS_AUDIO_BGM_GAIN", number);
    swprintf(number, 32, L"%.2f", g_settings.mainVolume / 100.0);
    environment.emplace_back(L"ORBIS_AUDIO_MAIN_GAIN", number);
    swprintf(number, 32, L"%.2f", g_settings.musicFloor / 100.0);
    environment.emplace_back(L"ORBIS_NGS2_MUSIC_FLOOR", number);
    environment.emplace_back(L"ORBIS_PAD_CONFIG", launcher::PadConfigurationPath(g_settings));
    environment.emplace_back(L"ORBIS_NGS2_TRACE", g_settings.diagnostics ? L"1" : L"");
    environment.emplace_back(L"ORBIS_GNM_DUMP_EVERY", g_settings.diagnostics ? L"3000" : L"");
    environment.emplace_back(L"ORBIS_GNM_LOG", g_settings.diagnostics ? L"info" : L"");
    std::error_code logError;
    const std::filesystem::path logDirectory = std::filesystem::path(g_settings.installDirectory) / L"anyps4" / L"log";
    std::filesystem::create_directories(logDirectory, logError);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::wstring logFile = (logDirectory / (std::wstring(L"session-") + stamp + L".log")).wstring();
    std::wstring error;
    if (launcher::StartDetached(executable, g_settings.installDirectory, environment, logFile, error)) {
        AppendLog(L"Started " + executable + L" (present " + g_settings.presentMode + L", frame limit " +
                  (g_settings.fpsCap == 0 ? std::wstring(L"off") : std::to_wstring(g_settings.fpsCap) + L" fps") + L", BGM " + std::to_wstring(g_settings.bgmVolume) + L" %, voices " +
                  std::to_wstring(g_settings.mainVolume) + L" %" + (g_settings.diagnostics ? L", diagnostics on" : L"") + L"); log " + logFile);
    } else {
        AppendLog(L"Could not start the title: " + error);
    }
}

void Install() {
    if (g_installing) return;
    ReadControlsIntoSettings();
    if (g_settings.gameDirectory.empty() || g_settings.installDirectory.empty()) {
        AppendLog(L"Choose the game folder (with eboot.bin) and the install folder first.");
        return;
    }
    if (!std::filesystem::exists(std::filesystem::path(g_settings.gameDirectory) / L"eboot.bin")) {
        AppendLog(L"No eboot.bin in " + g_settings.gameDirectory);
        return;
    }
    {
        std::error_code error;
        const auto game = std::filesystem::weakly_canonical(g_settings.gameDirectory, error);
        const auto install = std::filesystem::weakly_canonical(g_settings.installDirectory, error);
        const auto rel = install.lexically_relative(game);
        if (install == game || (!rel.empty() && rel.native().rfind(L"..", 0) != 0)) {
            AppendLog(L"The install folder must be outside the game folder: app0 becomes a junction to the game, and the game files are never copied.");
            return;
        }
    }
    launcher::SaveSettings(g_settings);
    g_installing = true;
    EnableWindow(Control(kIdInstall), FALSE);
    std::wstring command = launcher::Quote(g_settings.python) + L" " + launcher::Quote(g_settings.repoRoot + L"\\tools\\anyps4_install.py") + L" --game " +
                           launcher::Quote(g_settings.gameDirectory) + L" --out " + launcher::Quote(g_settings.installDirectory) + L" --build-dir " +
                           launcher::Quote(g_settings.buildDirectory) + L" --cmake " + launcher::Quote(g_settings.cmake);
    if (!g_settings.shadps4Checkout.empty()) command += L" --shadps4 " + launcher::Quote(g_settings.shadps4Checkout);
    if (!g_settings.toolchainBin.empty()) command += L" --toolchain-bin " + launcher::Quote(g_settings.toolchainBin);
    if (!g_settings.relinker.empty()) command += L" --relinker " + launcher::Quote(g_settings.relinker);
    AppendLog(L"Installing: " + command);
    const std::wstring repo = g_settings.repoRoot;
    std::thread([command, repo] {
        int exitCode = -1;
        launcher::RunCaptured(command, repo, PostLog, exitCode);
        PostMessageW(g_window, kMessageInstallDone, static_cast<WPARAM>(exitCode), 0);
    }).detach();
}

void CreateControls() {
    Create(L"STATIC", L"Game folder (eboot.bin):", 0, 12, 14, 150, 20, 0);
    Create(L"EDIT", g_settings.gameDirectory.c_str(), WS_BORDER | ES_AUTOHSCROLL, 165, 12, 520, 24, kIdGame);
    Create(L"BUTTON", L"Browse...", 0, 695, 11, 100, 26, kIdGameBrowse);
    Create(L"STATIC", L"Install folder:", 0, 12, 44, 150, 20, 0);
    Create(L"EDIT", g_settings.installDirectory.c_str(), WS_BORDER | ES_AUTOHSCROLL, 165, 42, 520, 24, kIdInstallDir);
    Create(L"BUTTON", L"Browse...", 0, 695, 41, 100, 26, kIdInstallBrowse);
    Create(L"BUTTON", L"Install (relink + libraries)", 0, 165, 74, 200, 28, kIdInstall);
    Create(L"BUTTON", L"Play", BS_DEFPUSHBUTTON, 375, 74, 120, 28, kIdPlay);
    Create(L"BUTTON", L"Open install folder", 0, 505, 74, 180, 28, kIdOpenFolder);

    Create(L"BUTTON", L"Controls (click a key to change it, Escape cancels)", BS_GROUPBOX, 12, 112, 530, 330, 0);
    std::size_t count = 0;
    const anyps4::PadBinding* bindings = anyps4::PadBindings(count);
    for (std::size_t k = 0; k < count; ++k) {
        const int column = static_cast<int>(k / 8);
        const int row = static_cast<int>(k % 8);
        const int x = 24 + column * 172;
        const int y = 136 + row * 32;
        Create(L"STATIC", Widen(bindings[k].label).c_str(), 0, x, y + 4, 92, 20, 0);
        Create(L"BUTTON", Widen(anyps4::PadKeyName_(g_settings.keys[k]).c_str()).c_str(), 0, x + 96, y, 68, 26, kIdKeyBase + static_cast<int>(k));
    }
    Create(L"BUTTON", L"Use an XInput gamepad (slot 1) when connected", BS_AUTOCHECKBOX, 24, 402, 400, 24, kIdXInput);
    SendMessageW(Control(kIdXInput), BM_SETCHECK, g_settings.xinput ? BST_CHECKED : BST_UNCHECKED, 0);

    Create(L"BUTTON", L"Render and audio", BS_GROUPBOX, 555, 112, 240, 360, 0);
    Create(L"STATIC", L"Present mode:", 0, 567, 140, 100, 20, 0);
    HWND present = Create(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 667, 136, 115, 120, kIdPresent);
    for (const wchar_t* mode : {L"Fifo", L"Mailbox", L"Immediate"}) SendMessageW(present, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(mode));
    SendMessageW(present, CB_SELECTSTRING, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(g_settings.presentMode.c_str()));
    Create(L"STATIC", L"Frame limit:", 0, 567, 170, 100, 20, 0);
    HWND fps = Create(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 667, 166, 115, 120, kIdFps);
    for (const wchar_t* choice : {L"30 fps", L"60 fps", L"Unlimited"}) SendMessageW(fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
    SendMessageW(fps, CB_SETCURSEL, g_settings.fpsCap == 30 ? 0 : g_settings.fpsCap == 0 ? 2 : 1, 0);
    Create(L"BUTTON", L"Persistent pipeline cache", BS_AUTOCHECKBOX, 567, 200, 220, 24, kIdPipelineCache);
    SendMessageW(Control(kIdPipelineCache), BM_SETCHECK, g_settings.pipelineCache ? BST_CHECKED : BST_UNCHECKED, 0);
    Create(L"BUTTON", L"Vulkan validation layer", BS_AUTOCHECKBOX, 567, 226, 220, 24, kIdValidation);
    SendMessageW(Control(kIdValidation), BM_SETCHECK, g_settings.validation ? BST_CHECKED : BST_UNCHECKED, 0);
    Create(L"BUTTON", L"Headless (no rendering)", BS_AUTOCHECKBOX, 567, 252, 220, 24, kIdHeadless);
    SendMessageW(Control(kIdHeadless), BM_SETCHECK, g_settings.headless ? BST_CHECKED : BST_UNCHECKED, 0);
    Create(L"BUTTON", L"Diagnostics (trace, frame dumps)", BS_AUTOCHECKBOX, 567, 446, 220, 24, kIdDiagnostics);
    SendMessageW(Control(kIdDiagnostics), BM_SETCHECK, g_settings.diagnostics ? BST_CHECKED : BST_UNCHECKED, 0);
    const int sliders[3][3] = {{kIdBgm, kIdBgmLabel, g_settings.bgmVolume}, {kIdMain, kIdMainLabel, g_settings.mainVolume}, {kIdFloor, kIdFloorLabel, g_settings.musicFloor}};
    for (int k = 0; k < 3; ++k) {
        const int y = 284 + k * 54;
        Create(L"STATIC", L"", 0, 567, y, 220, 20, sliders[k][1]);
        HWND bar = Create(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS, 567, y + 20, 220, 30, sliders[k][0]);
        SendMessageW(bar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 150));
        SendMessageW(bar, TBM_SETTICFREQ, 25, 0);
        SendMessageW(bar, TBM_SETPOS, TRUE, sliders[k][2]);
    }
    UpdateSliderLabels();
    g_log = Create(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 482, 783, 200, kIdLog);
    AppendLog(L"AnyPS4 launcher. Settings: " + launcher::SettingsPath());
    AppendLog(L"Repository " + g_settings.repoRoot + L", build directory " + g_settings.buildDirectory +
              (g_settings.shadps4Checkout.empty() ? L" (no shadPS4 checkout configured: functions keep NID names)" : L", shadPS4 " + g_settings.shadps4Checkout));
}

void AssignKey(int key) {
    if (g_capturing < 0) return;
    if (key != VK_ESCAPE) g_settings.keys[static_cast<std::size_t>(g_capturing)] = key;
    SetWindowTextW(Control(kIdKeyBase + g_capturing), Widen(anyps4::PadKeyName_(g_settings.keys[static_cast<std::size_t>(g_capturing)]).c_str()).c_str());
    g_capturing = -1;
    launcher::SaveSettings(g_settings);
    launcher::SavePadConfiguration(g_settings);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id >= kIdKeyBase && id < kIdKeyBase + 24 && code == BN_CLICKED) {
            if (g_capturing >= 0) SetWindowTextW(Control(kIdKeyBase + g_capturing), Widen(anyps4::PadKeyName_(g_settings.keys[static_cast<std::size_t>(g_capturing)]).c_str()).c_str());
            g_capturing = id - kIdKeyBase;
            SetWindowTextW(Control(id), L"press...");
            SetFocus(window);
            return 0;
        }
        switch (id) {
        case kIdGameBrowse: {
            const std::wstring folder = BrowseFolder(L"Folder of the game (contains eboot.bin)");
            if (!folder.empty()) {
                SetWindowTextW(Control(kIdGame), folder.c_str());
                if (WindowText(Control(kIdInstallDir)).empty()) {
                    const std::wstring suggestion = (std::filesystem::path(g_settings.repoRoot).parent_path() / (L"AnyPS4-" + std::filesystem::path(folder).filename().wstring())).wstring();
                    SetWindowTextW(Control(kIdInstallDir), suggestion.c_str());
                }
            }
            return 0;
        }
        case kIdInstallBrowse: {
            const std::wstring folder = BrowseFolder(L"Install folder");
            if (!folder.empty()) SetWindowTextW(Control(kIdInstallDir), folder.c_str());
            return 0;
        }
        case kIdInstall:
            Install();
            return 0;
        case kIdPlay:
            Play();
            return 0;
        case kIdOpenFolder:
            ReadControlsIntoSettings();
            if (!g_settings.installDirectory.empty()) ShellExecuteW(window, L"open", g_settings.installDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case kIdXInput:
        case kIdValidation:
        case kIdPipelineCache:
        case kIdHeadless:
        case kIdDiagnostics:
        case kIdPresent:
        case kIdFps:
            ReadControlsIntoSettings();
            launcher::SaveSettings(g_settings);
            launcher::SavePadConfiguration(g_settings);
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_HSCROLL:
        UpdateSliderLabels();
        if (LOWORD(wParam) == TB_ENDTRACK || LOWORD(wParam) == TB_THUMBPOSITION) {
            ReadControlsIntoSettings();
            launcher::SaveSettings(g_settings);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_capturing >= 0) {
            AssignKey(static_cast<int>(wParam));
            return 0;
        }
        break;
    case kMessageLogLine: {
        auto* line = reinterpret_cast<std::wstring*>(lParam);
        AppendLog(*line);
        delete line;
        return 0;
    }
    case kMessageInstallDone: {
        g_installing = false;
        EnableWindow(Control(kIdInstall), TRUE);
        const int exitCode = static_cast<int>(wParam);
        if (exitCode == 0) {
            g_settings.executable.clear();
            g_settings.executable = FindExecutable();
            launcher::SaveSettings(g_settings);
            launcher::SavePadConfiguration(g_settings);
            AppendLog(L"Install finished: " + g_settings.executable);
        } else {
            AppendLog(L"Install failed with exit code " + std::to_wstring(exitCode));
        }
        return 0;
    }
    case WM_CLOSE:
        ReadControlsIntoSettings();
        launcher::SaveSettings(g_settings);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    g_settings = launcher::LoadSettings();
    NONCLIENTMETRICSW metrics{sizeof(NONCLIENTMETRICSW)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
    g_font = CreateFontIndirectW(&metrics.lfMessageFont);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"AnyPS4Launcher";
    RegisterClassW(&windowClass);
    RECT frame{0, 0, 808, 694};
    AdjustWindowRect(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_window = CreateWindowExW(0, windowClass.lpszClassName, L"AnyPS4 Launcher", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                               CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, nullptr);
    CreateControls();
    ShowWindow(g_window, showCommand);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (g_capturing >= 0 && (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN)) {
            AssignKey(static_cast<int>(message.wParam));
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return 0;
}
