#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "Bundle.hpp"
#include "PadKeyNames.hpp"
#include "Process.hpp"
#include "Settings.hpp"
#include "Ui.hpp"

namespace {

using launcher::Settings;
using ui::S;

constexpr int kIdGame = 100;
constexpr int kIdGameBrowse = 101;
constexpr int kIdInstallDir = 102;
constexpr int kIdInstallBrowse = 103;
constexpr int kIdInstall = 104;
constexpr int kIdPlay = 105;
constexpr int kIdOpenFolder = 106;
constexpr int kIdXInput = 107;
constexpr int kIdValidation = 109;
constexpr int kIdPipelineCache = 110;
constexpr int kIdHeadless = 111;
constexpr int kIdBgm = 112;
constexpr int kIdMain = 113;
constexpr int kIdFloor = 114;
constexpr int kIdLog = 118;
constexpr int kIdDiagnostics = 119;
constexpr int kIdTabHome = 130;
constexpr int kIdTabSettings = 131;
constexpr int kIdPresentBase = 140;  // three segments: Fifo, Mailbox, Immediate
constexpr int kIdFpsBase = 145;      // three segments: 30 fps, 60 fps, unlimited
constexpr int kIdKeyBase = 200;
constexpr UINT kMessageLogLine = WM_APP + 1;
constexpr UINT kMessageInstallDone = WM_APP + 2;
constexpr UINT_PTR kTimerHover = 1;

constexpr int kClientWidth = 900;
constexpr int kClientHeight = 656;
constexpr int kHeaderHeight = 96;

constexpr int kPageHome = 0;
constexpr int kPageSettings = 1;

const wchar_t* const kPresentModes[3] = {L"Fifo", L"Mailbox", L"Immediate"};
const wchar_t* const kPresentCaptions[3] = {L"Fifo", L"Mailbox", L"Immediate"};
const int kFpsValues[3] = {30, 60, 0};
const wchar_t* const kFpsCaptions[3] = {L"30 fps", L"60 fps", L"Unlimited"};

struct Label {
    int page;
    RECT rect;
    std::function<std::wstring()> text;
    ui::Font font;
    COLORREF color;
    UINT flags;
};

struct Card {
    int page;
    RECT rect;
    std::wstring title;
};

struct Field {
    int page;
    RECT rect;
    HWND edit;
};

Settings g_settings;
HWND g_window = nullptr;
HWND g_log = nullptr;
HBRUSH g_cardBrush = nullptr;
HBRUSH g_fieldBrush = nullptr;
HWND g_hover = nullptr;
int g_capturing = -1;
int g_page = kPageHome;
bool g_installing = false;
bool g_installed = false;
bool g_ready = false;
std::map<int, bool> g_checked;
std::vector<Label> g_labels;
std::vector<Card> g_cards;
std::vector<Field> g_fields;
std::vector<HWND> g_pageControls[2];

std::wstring Widen(const char* text) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length > 0 ? length - 1 : 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
    return wide;
}

std::wstring WindowText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    return text;
}

HWND Control(int id) {
    return GetDlgItem(g_window, id);
}

bool IsCheckbox(int id) {
    return id == kIdXInput || id == kIdValidation || id == kIdPipelineCache || id == kIdHeadless || id == kIdDiagnostics;
}

bool IsSlider(int id) {
    return id == kIdBgm || id == kIdMain || id == kIdFloor;
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
    g_settings.xinput = g_checked[kIdXInput];
    g_settings.validation = g_checked[kIdValidation];
    g_settings.pipelineCache = g_checked[kIdPipelineCache];
    g_settings.headless = g_checked[kIdHeadless];
    g_settings.diagnostics = g_checked[kIdDiagnostics];
    g_settings.bgmVolume = static_cast<int>(SendMessageW(Control(kIdBgm), TBM_GETPOS, 0, 0));
    g_settings.mainVolume = static_cast<int>(SendMessageW(Control(kIdMain), TBM_GETPOS, 0, 0));
    g_settings.musicFloor = static_cast<int>(SendMessageW(Control(kIdFloor), TBM_GETPOS, 0, 0));
}

bool IsTitleExecutable(const std::filesystem::path& path) {
    std::wstring name = path.filename().wstring();
    for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
    return path.extension() == L".exe" && name.rfind(L"anyps4_launcher", 0) != 0 && name.rfind(L"relinker", 0) != 0 && name.find(L"_dbg") == std::wstring::npos;
}

std::wstring FindExecutable() {
    std::error_code error;
    if (!g_settings.executable.empty() && std::filesystem::exists(g_settings.executable, error) && IsTitleExecutable(g_settings.executable) &&
        std::filesystem::equivalent(std::filesystem::path(g_settings.executable).parent_path(), g_settings.installDirectory, error)) {
        return g_settings.executable;
    }
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    for (const auto& entry : std::filesystem::directory_iterator(g_settings.installDirectory, error)) {
        if (!entry.is_regular_file(error) || !IsTitleExecutable(entry.path())) continue;
        std::filesystem::path registry = entry.path();
        registry.replace_extension(L".registry.json");
        if (std::filesystem::exists(registry, error)) return entry.path().wstring();
        const auto time = entry.last_write_time(error);
        if (newest.empty() || time > newestTime) {
            newest = entry.path();
            newestTime = time;
        }
    }
    return newest.wstring();
}

// The title is playable when the install folder holds its executable and the runtime libraries.
void RefreshStatus() {
    if (!g_ready) return;
    ReadControlsIntoSettings();
    std::error_code error;
    const std::wstring executable = g_settings.installDirectory.empty() ? std::wstring() : FindExecutable();
    g_installed = !executable.empty() && std::filesystem::exists(executable, error) &&
                  std::filesystem::exists(std::filesystem::path(g_settings.installDirectory) / L"libs" / L"libkernel.prx", error);
    EnableWindow(Control(kIdPlay), g_installed ? TRUE : FALSE);
    InvalidateRect(Control(kIdPlay), nullptr, FALSE);
    InvalidateRect(Control(kIdInstall), nullptr, FALSE);
    InvalidateRect(g_window, nullptr, FALSE);
}

std::wstring SuggestInstallDirectory(const std::wstring& gameFolder) {
    std::wstring bundle;
    const std::wstring name = std::filesystem::path(gameFolder).filename().wstring();
    return launcher::BundleAvailable(bundle) ? (std::filesystem::path(bundle) / name).wstring()
                                             : (std::filesystem::path(g_settings.repoRoot).parent_path() / (L"AnyPS4-" + name)).wstring();
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
    if (g_settings.installDirectory.empty() && !g_settings.gameDirectory.empty()) {
        g_settings.installDirectory = SuggestInstallDirectory(g_settings.gameDirectory);
        SetWindowTextW(Control(kIdInstallDir), g_settings.installDirectory.c_str());
    }
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
    std::wstring bundle;
    if (launcher::BundleAvailable(bundle)) {
        AppendLog(L"Installing from the standalone package in " + bundle);
        const std::wstring game = g_settings.gameDirectory;
        const std::wstring install = g_settings.installDirectory;
        std::thread([bundle, game, install] {
            const int exitCode = launcher::InstallFromBundle(bundle, game, install, PostLog);
            PostMessageW(g_window, kMessageInstallDone, static_cast<WPARAM>(exitCode), 0);
        }).detach();
        return;
    }
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

// ---------------------------------------------------------------------------------------------------------------------
// Layout. Coordinates are 96-dpi pixels; every helper scales them.
// ---------------------------------------------------------------------------------------------------------------------

void AddCard(int page, int x, int y, int width, int height, const wchar_t* title) {
    g_cards.push_back({page, ui::Rect(x, y, width, height), title});
}

void AddLabel(int page, int x, int y, int width, int height, std::function<std::wstring()> text, ui::Font font = ui::kFontSmall, COLORREF color = ui::kMuted,
              UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    g_labels.push_back({page, ui::Rect(x, y, width, height), std::move(text), font, color, flags});
}

void AddLabel(int page, int x, int y, int width, int height, const wchar_t* text, ui::Font font = ui::kFontSmall, COLORREF color = ui::kMuted,
              UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    const std::wstring copy = text;
    AddLabel(page, x, y, width, height, [copy] { return copy; }, font, color, flags);
}

HWND AddChild(int page, const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height, int id) {
    HWND child = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, S(x), S(y), S(width), S(height), g_window,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(ui::FontHandle(ui::kFontBody)), TRUE);
    if (page >= 0) g_pageControls[page].push_back(child);
    return child;
}

HWND AddButton(int page, const wchar_t* text, int x, int y, int width, int height, int id) {
    return AddChild(page, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, width, height, id);
}

HWND AddField(int page, const std::wstring& text, int x, int y, int width, int height, int id) {
    g_fields.push_back({page, ui::Rect(x, y, width, height), nullptr});
    const int editHeight = 20;
    HWND edit = AddChild(page, L"EDIT", text.c_str(), ES_AUTOHSCROLL, x + 12, y + (height - editHeight) / 2, width - 24, editHeight, id);
    g_fields.back().edit = edit;
    return edit;
}

void AddSlider(int page, int id, int labelY, std::function<std::wstring()> label, int value) {
    AddLabel(page, 44, labelY, 368, 18, std::move(label), ui::kFontSmall, ui::kText);
    HWND bar = AddChild(page, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 40, labelY + 18, 376, 26, id);
    SendMessageW(bar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 150));
    SendMessageW(bar, TBM_SETPOS, TRUE, value);
}

void AddCheckbox(int page, const wchar_t* text, int x, int y, int width, int id, bool checked) {
    g_checked[id] = checked;
    AddButton(page, text, x, y, width, 26, id);
}

void CreateControls() {
    // Header tabs.
    AddButton(-1, L"Home", 632, 32, 108, 36, kIdTabHome);
    AddButton(-1, L"⚙  Settings", 748, 32, 128, 36, kIdTabSettings);

    // ---- Home --------------------------------------------------------------------------------------------------
    AddCard(kPageHome, 24, 112, 852, 216, L"GAME");
    AddLabel(kPageHome, 44, 150, 500, 18, L"Game folder (the one that contains eboot.bin)");
    AddField(kPageHome, g_settings.gameDirectory, 44, 172, 652, 38, kIdGame);
    AddButton(kPageHome, L"Browse...", 708, 172, 148, 38, kIdGameBrowse);
    AddLabel(kPageHome, 44, 216, 812, 20, [] {
        return g_installed ? std::wstring(L"Ready to play. Install again only if you move the game folder or change its files.")
                           : std::wstring(L"Pick your game folder, then press Install (it takes a few seconds). Your game files are never copied.");
    });
    AddButton(kPageHome, L"PLAY", 44, 246, 408, 64, kIdPlay);
    AddButton(kPageHome, L"Install (relink + libraries)", 464, 246, 220, 64, kIdInstall);
    AddButton(kPageHome, L"Open install folder", 696, 246, 160, 64, kIdOpenFolder);

    AddCard(kPageHome, 24, 344, 852, 296, L"ACTIVITY");
    {
        g_fields.push_back({kPageHome, ui::Rect(44, 384, 812, 240), nullptr});
        g_log = AddChild(kPageHome, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 52, 390, 796, 228, kIdLog);
        g_fields.back().edit = g_log;
        SendMessageW(g_log, WM_SETFONT, reinterpret_cast<WPARAM>(ui::FontHandle(ui::kFontMono)), TRUE);
        ui::StyleScrollBars(g_log);
    }

    // ---- Settings: paths, graphics, audio (left column) --------------------------------------------------------
    AddCard(kPageSettings, 24, 112, 408, 140, L"PATHS");
    AddLabel(kPageSettings, 44, 146, 368, 18, L"Install folder (outside the game folder)");
    AddField(kPageSettings, g_settings.installDirectory, 44, 168, 250, 36, kIdInstallDir);
    AddButton(kPageSettings, L"Browse...", 302, 168, 110, 36, kIdInstallBrowse);
    AddButton(kPageSettings, L"Open install folder", 44, 212, 180, 30, 0 + kIdOpenFolder + 1000);

    AddCard(kPageSettings, 24, 264, 408, 186, L"GRAPHICS");
    AddLabel(kPageSettings, 44, 306, 100, 30, L"Present mode", ui::kFontSmall, ui::kText);
    for (int k = 0; k < 3; ++k) AddButton(kPageSettings, kPresentCaptions[k], 150 + k * 80, 306, 74, 30, kIdPresentBase + k);
    AddLabel(kPageSettings, 44, 346, 100, 30, L"Frame limit", ui::kFontSmall, ui::kText);
    for (int k = 0; k < 3; ++k) AddButton(kPageSettings, kFpsCaptions[k], 150 + k * 80, 346, 74, 30, kIdFpsBase + k);
    AddCheckbox(kPageSettings, L"Persistent pipeline cache", 44, 388, 184, kIdPipelineCache, g_settings.pipelineCache);
    AddCheckbox(kPageSettings, L"Vulkan validation layer", 232, 388, 192, kIdValidation, g_settings.validation);
    AddCheckbox(kPageSettings, L"Headless (no rendering)", 44, 418, 184, kIdHeadless, g_settings.headless);
    AddCheckbox(kPageSettings, L"Diagnostics", 232, 418, 192, kIdDiagnostics, g_settings.diagnostics);

    AddCard(kPageSettings, 24, 462, 408, 176, L"AUDIO");
    AddSlider(kPageSettings, kIdBgm, 500, [] { return L"Music (BGM): " + std::to_wstring(SendMessageW(Control(kIdBgm), TBM_GETPOS, 0, 0)) + L" %"; }, g_settings.bgmVolume);
    AddSlider(kPageSettings, kIdMain, 542, [] { return L"Voices and effects: " + std::to_wstring(SendMessageW(Control(kIdMain), TBM_GETPOS, 0, 0)) + L" %"; },
              g_settings.mainVolume);
    AddSlider(kPageSettings, kIdFloor, 584,
              [] { return L"Music floor when the title mutes it (advanced): " + std::to_wstring(SendMessageW(Control(kIdFloor), TBM_GETPOS, 0, 0)) + L" %"; },
              g_settings.musicFloor);

    // ---- Settings: controls (right column) ---------------------------------------------------------------------
    AddCard(kPageSettings, 448, 112, 428, 526, L"CONTROLS");
    AddLabel(kPageSettings, 468, 146, 388, 18, L"Click a key to change it. Escape cancels.");
    std::size_t count = 0;
    const anyps4::PadBinding* bindings = anyps4::PadBindings(count);
    for (std::size_t k = 0; k < count; ++k) {
        const int column = static_cast<int>(k / 12);
        const int row = static_cast<int>(k % 12);
        const int x = 468 + column * 196;
        const int y = 176 + row * 32;
        AddLabel(kPageSettings, x, y, 104, 28, Widen(bindings[k].label).c_str(), ui::kFontSmall, ui::kText);
        AddButton(kPageSettings, Widen(anyps4::PadKeyName_(g_settings.keys[k]).c_str()).c_str(), x + 106, y, 82, 28, kIdKeyBase + static_cast<int>(k));
    }
    AddCheckbox(kPageSettings, L"Use an XInput gamepad (slot 1) when connected", 468, 580, 380, kIdXInput, g_settings.xinput);

    AppendLog(L"AnyPS4 launcher. Settings: " + launcher::SettingsPath());
    std::wstring bundle;
    if (launcher::BundleAvailable(bundle)) {
        AppendLog(L"Standalone package: " + bundle + L" (Install needs no compiler, Python or CMake).");
    } else {
        AppendLog(L"Repository " + g_settings.repoRoot + L", build directory " + g_settings.buildDirectory +
                  (g_settings.shadps4Checkout.empty() ? L" (no shadPS4 checkout configured: functions keep NID names)" : L", shadPS4 " + g_settings.shadps4Checkout));
    }
}

void ShowPage(int page) {
    g_page = page;
    for (int p = 0; p < 2; ++p) {
        for (HWND control : g_pageControls[p]) ShowWindow(control, p == page ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(g_window, nullptr, FALSE);
    InvalidateRect(Control(kIdTabHome), nullptr, FALSE);
    InvalidateRect(Control(kIdTabSettings), nullptr, FALSE);
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------------------------------

void PaintWindow(HDC dc, const RECT& client) {
    const int width = client.right - client.left;
    ui::FillSolid(dc, client, ui::kBackground);
    ui::HeaderBackdrop(dc, RECT{0, 0, width, S(kHeaderHeight)}, width, S(kHeaderHeight), 0, 0);
    ui::DrawTextLine(dc, L"AnyPS4", ui::Rect(28, 8, 400, 40), ui::kFontTitle, RGB(255, 255, 255), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui::DrawTextLine(dc, L"The Witch and the Hundred Knight: Revival Edition  (CUSA02799)", ui::Rect(30, 50, 600, 20), ui::kFontSubtitle, RGB(214, 208, 246),
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ui::DrawTextLine(dc, L"Native Windows runtime  ·  developed and tested only for this game", ui::Rect(30, 68, 600, 18), ui::kFontSmall, RGB(160, 164, 214),
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    for (const Card& card : g_cards) {
        if (card.page != g_page) continue;
        ui::FillRound(dc, card.rect, S(14), ui::kCard, ui::kCardBorder);
        RECT title = card.rect;
        title.left += S(20);
        title.top += S(14);
        title.bottom = title.top + S(16);
        ui::DrawTextLine(dc, card.title, title, ui::kFontCardTitle, ui::kVioletLight, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    for (const Field& field : g_fields) {
        if (field.page != g_page) continue;
        const bool focused = field.edit && GetFocus() == field.edit && field.edit != g_log;
        ui::FillRound(dc, field.rect, S(9), ui::kField, focused ? ui::kViolet : ui::kFieldBorder);
    }
    for (const Label& label : g_labels) {
        if (label.page != g_page) continue;
        ui::DrawTextLine(dc, label.text(), label.rect, label.font, label.color, label.flags);
    }
    if (g_page == kPageHome) {
        // Status indicator, right-aligned in the title row of the game card.
        const std::wstring text = g_installed ? L"Ready" : L"Not installed";
        const COLORREF color = g_installed ? ui::kGreen : ui::kAmber;
        RECT measure = {0, 0, 0, 0};
        HGDIOBJ previous = SelectObject(dc, ui::FontHandle(ui::kFontButton));
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measure, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(dc, previous);
        const int textWidth = measure.right - measure.left;
        const int dot = S(12);
        const int right = S(856);
        const int top = S(124);
        const int rowHeight = S(24);
        const int left = right - textWidth - S(8) - dot;
        ui::FillEllipse(dc, RECT{left, top + (rowHeight - dot) / 2, left + dot, top + (rowHeight - dot) / 2 + dot}, color);
        ui::DrawTextLine(dc, text, RECT{left + dot + S(8), top, right + S(2), top + rowHeight}, ui::kFontButton, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

enum class Kind { Primary, Secondary, Tab, Segment, Check, Key };

Kind KindOf(int id) {
    if (id == kIdPlay) return Kind::Primary;
    if (id == kIdTabHome || id == kIdTabSettings) return Kind::Tab;
    if ((id >= kIdPresentBase && id < kIdPresentBase + 3) || (id >= kIdFpsBase && id < kIdFpsBase + 3)) return Kind::Segment;
    if (IsCheckbox(id)) return Kind::Check;
    if (id >= kIdKeyBase && id < kIdKeyBase + 24) return Kind::Key;
    return Kind::Secondary;
}

bool IsSegmentSelected(int id) {
    if (id >= kIdPresentBase && id < kIdPresentBase + 3) return g_settings.presentMode == kPresentModes[id - kIdPresentBase];
    return g_settings.fpsCap == kFpsValues[id - kIdFpsBase];
}

void DrawButton(const DRAWITEMSTRUCT* item) {
    const int id = static_cast<int>(item->CtlID);
    const int width = item->rcItem.right - item->rcItem.left;
    const int height = item->rcItem.bottom - item->rcItem.top;
    HDC target = item->hDC;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    const RECT r = {0, 0, width, height};
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool hover = item->hwndItem == g_hover && !disabled;
    wchar_t caption[160] = {};
    GetWindowTextW(item->hwndItem, caption, 160);
    const Kind kind = KindOf(id);
    const UINT center = DT_CENTER | DT_VCENTER | DT_SINGLELINE;

    if (kind == Kind::Tab) {
        POINT origin = {0, 0};
        MapWindowPoints(item->hwndItem, g_window, &origin, 1);
        ui::HeaderBackdrop(dc, r, S(kClientWidth), S(kHeaderHeight), origin.x, origin.y);
    } else {
        ui::FillSolid(dc, r, ui::kCard);
    }

    switch (kind) {
    case Kind::Primary: {
        if (disabled) {
            ui::FillRound(dc, r, S(14), ui::kButton, ui::kCardBorder);
        } else if (pressed) {
            ui::FillRoundGradient(dc, r, S(14), RGB(112, 70, 214), RGB(40, 104, 226));
        } else if (hover) {
            ui::FillRoundGradient(dc, r, S(14), RGB(160, 118, 255), RGB(84, 154, 255));
        } else {
            ui::FillRoundGradient(dc, r, S(14), ui::kViolet, ui::kBlue);
        }
        const COLORREF color = disabled ? ui::kDisabled : RGB(255, 255, 255);
        const int centerX = width / 2;
        const int triangle = S(20);
        const int startX = centerX - S(50);
        ui::FillTriangle(dc, RECT{startX, (height - triangle) / 2, startX + S(17), (height - triangle) / 2 + triangle}, color);
        ui::DrawTextLine(dc, caption, RECT{startX + S(32), 0, width, height}, ui::kFontPlay, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case Kind::Secondary: {
        const bool highlight = id == kIdInstall && !g_installed && !disabled;
        ui::FillRound(dc, r, S(12), pressed ? ui::kCard : (hover ? ui::kButtonHover : ui::kButton), highlight ? ui::kViolet : ui::kCardBorder);
        ui::DrawTextLine(dc, caption, r, ui::kFontButton, disabled ? ui::kDisabled : ui::kText, center);
        break;
    }
    case Kind::Tab: {
        const bool active = (id == kIdTabHome && g_page == kPageHome) || (id == kIdTabSettings && g_page == kPageSettings);
        if (active) ui::FillRound(dc, r, S(10), RGB(255, 255, 255), RGB(255, 255, 255), false, 40);
        else if (hover) ui::FillRound(dc, r, S(10), RGB(255, 255, 255), RGB(255, 255, 255), false, 18);
        ui::DrawTextLine(dc, caption, r, ui::kFontButton, active ? RGB(255, 255, 255) : RGB(206, 204, 238), center);
        break;
    }
    case Kind::Segment: {
        if (IsSegmentSelected(id)) {
            ui::FillRoundGradient(dc, r, S(8), ui::kViolet, ui::kBlue);
            ui::DrawTextLine(dc, caption, r, ui::kFontSmall, RGB(255, 255, 255), center);
        } else {
            ui::FillRound(dc, r, S(8), hover ? ui::kButtonHover : ui::kButton, ui::kCardBorder);
            ui::DrawTextLine(dc, caption, r, ui::kFontSmall, ui::kText, center);
        }
        break;
    }
    case Kind::Check: {
        const int box = S(18);
        const int top = (height - box) / 2;
        const RECT boxRect = {0, top, box, top + box};
        if (g_checked[id]) {
            ui::FillRoundGradient(dc, boxRect, S(5), ui::kViolet, ui::kBlue);
            ui::DrawCheck(dc, boxRect, RGB(255, 255, 255));
        } else {
            ui::FillRound(dc, boxRect, S(5), ui::kField, hover ? ui::kViolet : ui::kFieldBorder);
        }
        ui::DrawTextLine(dc, caption, RECT{box + S(10), 0, width, height}, ui::kFontSmall, ui::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case Kind::Key: {
        const bool capturing = g_capturing == id - kIdKeyBase;
        ui::FillRound(dc, r, S(8), capturing || hover ? ui::kButtonHover : ui::kButton, capturing ? ui::kViolet : ui::kCardBorder);
        ui::DrawTextLine(dc, caption, r, ui::kFontSmall, capturing ? ui::kVioletLight : ui::kText, center);
        break;
    }
    }

    BitBlt(target, item->rcItem.left, item->rcItem.top, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

LRESULT DrawSlider(const NMCUSTOMDRAW* draw) {
    if (draw->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HWND bar = draw->hdr.hwndFrom;
    RECT client = {};
    GetClientRect(bar, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) return CDRF_DODEFAULT;
    HDC dc = CreateCompatibleDC(draw->hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(draw->hdc, width, height);
    HGDIOBJ previousBitmap = SelectObject(dc, bitmap);
    ui::FillSolid(dc, RECT{0, 0, width, height}, ui::kCard);
    RECT thumb = {};
    RECT channel = {};
    SendMessageW(bar, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    SendMessageW(bar, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    const int centerY = height / 2;
    const int centerX = (thumb.left + thumb.right) / 2;
    const int trackHeight = S(6);
    const RECT track = {S(8), centerY - trackHeight / 2, width - S(8), centerY - trackHeight / 2 + trackHeight};
    ui::DrawTrack(dc, track, centerX, ui::kViolet, ui::kField);
    const int outer = S(18);
    const int inner = S(8);
    ui::FillEllipse(dc, RECT{centerX - outer / 2, centerY - outer / 2, centerX - outer / 2 + outer, centerY - outer / 2 + outer}, ui::kVioletLight);
    ui::FillEllipse(dc, RECT{centerX - inner / 2, centerY - inner / 2, centerX - inner / 2 + inner, centerY - inner / 2 + inner}, ui::kCard);
    BitBlt(draw->hdc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return CDRF_SKIPDEFAULT;
}

void AssignKey(int key) {
    if (g_capturing < 0) return;
    if (key != VK_ESCAPE) g_settings.keys[static_cast<std::size_t>(g_capturing)] = key;
    SetWindowTextW(Control(kIdKeyBase + g_capturing), Widen(anyps4::PadKeyName_(g_settings.keys[static_cast<std::size_t>(g_capturing)]).c_str()).c_str());
    g_capturing = -1;
    launcher::SaveSettings(g_settings);
    launcher::SavePadConfiguration(g_settings);
}

void UpdateHover() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HWND under = WindowFromPoint(cursor);
    HWND hovered = nullptr;
    if (under && GetParent(under) == g_window && (GetWindowLongW(under, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) hovered = under;
    if (hovered == g_hover) return;
    HWND previous = g_hover;
    g_hover = hovered;
    if (previous) InvalidateRect(previous, nullptr, FALSE);
    if (hovered) InvalidateRect(hovered, nullptr, FALSE);
}

void PersistSettings() {
    ReadControlsIntoSettings();
    launcher::SaveSettings(g_settings);
    launcher::SavePadConfiguration(g_settings);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        PaintWindow(memory, client);
        BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top, paint.rcPaint.right - paint.rcPaint.left, paint.rcPaint.bottom - paint.rcPaint.top, memory, paint.rcPaint.left,
               paint.rcPaint.top, SRCCOPY);
        SelectObject(memory, previous);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DRAWITEM:
        if (reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_BUTTON) {
            DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header->code == NM_CUSTOMDRAW && IsSlider(static_cast<int>(header->idFrom))) return DrawSlider(reinterpret_cast<const NMCUSTOMDRAW*>(lParam));
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        const bool editLike = message == WM_CTLCOLOREDIT || control == g_log;
        SetTextColor(dc, ui::kText);
        SetBkColor(dc, editLike ? ui::kField : ui::kCard);
        return reinterpret_cast<LRESULT>(editLike ? g_fieldBrush : g_cardBrush);
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            HWND under = reinterpret_cast<HWND>(wParam);
            if (under && under != window && (GetWindowLongW(under, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW && IsWindowEnabled(under)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_TIMER:
        if (wParam == kTimerHover) {
            UpdateHover();
            return 0;
        }
        break;
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
        if (id == kIdGame || id == kIdInstallDir) {
            if (code == EN_CHANGE) RefreshStatus();
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (code != BN_CLICKED) break;
        if (IsCheckbox(id)) {
            g_checked[id] = !g_checked[id];
            InvalidateRect(Control(id), nullptr, FALSE);
            PersistSettings();
            return 0;
        }
        if (id >= kIdPresentBase && id < kIdPresentBase + 3) {
            g_settings.presentMode = kPresentModes[id - kIdPresentBase];
            for (int k = 0; k < 3; ++k) InvalidateRect(Control(kIdPresentBase + k), nullptr, FALSE);
            PersistSettings();
            return 0;
        }
        if (id >= kIdFpsBase && id < kIdFpsBase + 3) {
            g_settings.fpsCap = kFpsValues[id - kIdFpsBase];
            for (int k = 0; k < 3; ++k) InvalidateRect(Control(kIdFpsBase + k), nullptr, FALSE);
            PersistSettings();
            return 0;
        }
        switch (id) {
        case kIdTabHome:
            ShowPage(kPageHome);
            return 0;
        case kIdTabSettings:
            ShowPage(kPageSettings);
            return 0;
        case kIdGameBrowse: {
            const std::wstring folder = BrowseFolder(L"Folder of the game (contains eboot.bin)");
            if (!folder.empty()) {
                SetWindowTextW(Control(kIdGame), folder.c_str());
                if (WindowText(Control(kIdInstallDir)).empty()) SetWindowTextW(Control(kIdInstallDir), SuggestInstallDirectory(folder).c_str());
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
        case kIdOpenFolder + 1000:
            ReadControlsIntoSettings();
            if (!g_settings.installDirectory.empty()) ShellExecuteW(window, L"open", g_settings.installDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_HSCROLL:
        for (const int id : {kIdBgm, kIdMain, kIdFloor}) InvalidateRect(Control(id), nullptr, FALSE);
        {
            RECT audio = ui::Rect(24, 462, 408, 176);
            InvalidateRect(window, &audio, FALSE);
        }
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
            ReadControlsIntoSettings();
            g_settings.executable.clear();
            g_settings.executable = FindExecutable();
            launcher::SaveSettings(g_settings);
            launcher::SavePadConfiguration(g_settings);
            AppendLog(L"Install finished: " + g_settings.executable);
        } else {
            AppendLog(L"Install failed with exit code " + std::to_wstring(exitCode));
        }
        RefreshStatus();
        return 0;
    }
    case WM_CLOSE:
        ReadControlsIntoSettings();
        launcher::SaveSettings(g_settings);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kTimerHover);
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
    ui::Init();
    g_settings = launcher::LoadSettings();
    if (g_settings.fpsCap != 30 && g_settings.fpsCap != 0) g_settings.fpsCap = 60;
    if (g_settings.presentMode != kPresentModes[1] && g_settings.presentMode != kPresentModes[2]) g_settings.presentMode = kPresentModes[0];
    g_cardBrush = CreateSolidBrush(ui::kCard);
    g_fieldBrush = CreateSolidBrush(ui::kField);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = L"AnyPS4Launcher";
    RegisterClassW(&windowClass);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT frame{0, 0, S(kClientWidth), S(kClientHeight)};
    AdjustWindowRect(&frame, style, FALSE);
    g_window = CreateWindowExW(0, windowClass.lpszClassName, L"AnyPS4 Launcher", style, CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
                               nullptr, nullptr, instance, nullptr);
    ui::StyleWindow(g_window);
    CreateControls();
    g_ready = true;
    ShowPage(kPageHome);
    RefreshStatus();
    SetTimer(g_window, kTimerHover, 50, nullptr);
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
    DeleteObject(g_cardBrush);
    DeleteObject(g_fieldBrush);
    ui::Shutdown();
    CoUninitialize();
    return 0;
}
