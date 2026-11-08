#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "Bundle.hpp"
#include "DiscordPresence.hpp"
#include "LiveAudio.hpp"
#include "LiveSave.hpp"
#include "SavePicker.hpp"
#include "PadKeyNames.hpp"
#include "Process.hpp"
#include "SaveBackup.hpp"
#include "Settings.hpp"
#include "SteamIntegration.hpp"
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
constexpr int kIdSteam = 150;
constexpr int kIdResolution = 190;
constexpr int kIdBorderless = 191;
constexpr int kIdResolutionItem = 195;
constexpr int kIdSaveSlot = 192;
constexpr int kIdAskSaveSlot = 193;
constexpr int kIdUncapped = 194;
constexpr int kIdSlotItem = 230;
constexpr int kIdDiscord = 160;
constexpr int kIdDiscordId = 161;
constexpr int kIdExportSaves = 170;
constexpr int kIdImportSaves = 171;
constexpr int kIdTabHome = 130;
constexpr int kIdTabSettings = 131;
constexpr int kIdPresentBase = 140;
constexpr int kIdLanguageBase = 180;
constexpr int kIdFpsBase = 145;
constexpr int kIdKeyBase = 200;
constexpr UINT kMessageLogLine = WM_APP + 1;
constexpr UINT kMessageInstallDone = WM_APP + 2;
constexpr UINT_PTR kTimerHover = 1;

constexpr int kClientWidth = 1180;
constexpr int kClientHeight = 690;
constexpr int kHeaderHeight = 96;

constexpr int kPageHome = 0;
constexpr int kPageSettings = 1;

const wchar_t* const kPresentModes[3] = {L"Fifo", L"Mailbox", L"Immediate"};
const wchar_t* const kPresentCaptions[3] = {L"Fifo", L"Mailbox", L"Immediate"};
const int kFpsValues[2] = {30, 60};
const wchar_t* const kFpsCaptions[2] = {L"30 fps", L"60 fps"};
const int kLanguageValues[2] = {1, 0};
constexpr int kResolutionCount = 5;
const wchar_t* const kResolutionValues[kResolutionCount] = {L"1280x720", L"1920x1080", L"2560x1440", L"2560x1600", L"3840x2160"};
const wchar_t* const kResolutionCaptions[kResolutionCount] = {L"1280x720", L"1920x1080 (Default)", L"2560x1440", L"2560x1600 (16:10)", L"3840x2160"};
const wchar_t* const kLanguageCaptions[2] = {L"English (US)", L"Japanese"};
const wchar_t* const kSaveSlotCaptions[launcher::kSaveSlotCount] = {L"Save Slot 1 (Default)", L"Save Slot 2", L"Save Slot 3", L"Save Slot 4", L"Save Slot 5"};

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
bool g_onSteam = false;
bool g_gameRunning = false;
HANDLE g_gameProcess = nullptr;
unsigned long g_gamePid = 0;
unsigned long g_saveSeenPid = 0;
int g_saveSeen = 0;
launcher::DiscordPresence g_presence;
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
    return id == kIdXInput || id == kIdBorderless || id == kIdDiscord || id == kIdValidation || id == kIdPipelineCache || id == kIdHeadless || id == kIdDiagnostics || id == kIdAskSaveSlot || id == kIdUncapped;
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
    g_settings.borderless = g_checked[kIdBorderless];
    g_settings.discordEnabled = g_checked[kIdDiscord];
    std::wstring clientId = WindowText(Control(kIdDiscordId));
    clientId.erase(std::remove_if(clientId.begin(), clientId.end(), [](wchar_t c) { return iswspace(c) != 0; }), clientId.end());
    g_settings.discordClientId = clientId;
    g_settings.validation = g_checked[kIdValidation];
    g_settings.pipelineCache = g_checked[kIdPipelineCache];
    g_settings.headless = g_checked[kIdHeadless];
    g_settings.diagnostics = g_checked[kIdDiagnostics];
    g_settings.askSaveSlot = g_checked[kIdAskSaveSlot];
    g_settings.uncappedFps = g_checked[kIdUncapped];
    g_settings.bgmVolume = static_cast<int>(SendMessageW(Control(kIdBgm), TBM_GETPOS, 0, 0));
    g_settings.mainVolume = static_cast<int>(SendMessageW(Control(kIdMain), TBM_GETPOS, 0, 0));
    g_settings.musicFloor = static_cast<int>(SendMessageW(Control(kIdFloor), TBM_GETPOS, 0, 0));
}

bool IsTitleExecutable(const std::filesystem::path& path) {
    std::wstring name = path.filename().wstring();
    for (wchar_t& c : name) c = static_cast<wchar_t>(towlower(c));
    return path.extension() == L".exe" && name.rfind(L"anyps4_launcher", 0) != 0 && name.rfind(L"relinker", 0) != 0 && name.find(L"_dbg") == std::wstring::npos;
}

std::wstring FindExecutable(const Settings& settings) {
    std::error_code error;
    if (!settings.executable.empty() && std::filesystem::exists(settings.executable, error) && IsTitleExecutable(settings.executable) &&
        std::filesystem::equivalent(std::filesystem::path(settings.executable).parent_path(), settings.installDirectory, error)) {
        return settings.executable;
    }
    std::filesystem::path newest;
    std::filesystem::file_time_type newestTime{};
    for (const auto& entry : std::filesystem::directory_iterator(settings.installDirectory, error)) {
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

std::wstring FindExecutable() {
    return FindExecutable(g_settings);
}

void ForgetGame() {
    if (g_gameProcess != nullptr) CloseHandle(g_gameProcess);
    g_gameProcess = nullptr;
    g_gamePid = 0;
}

void RefreshGameRunning() {
    bool running = false;
    if (g_gameProcess != nullptr) {
        if (WaitForSingleObject(g_gameProcess, 0) == WAIT_TIMEOUT) running = true;
        else ForgetGame();
    }
    if (g_gameProcess != nullptr) {
        g_gamePid = GetProcessId(g_gameProcess);
    } else {
        g_gamePid = launcher::FindRunningTitle(FindExecutable());
        running = g_gamePid != 0;
    }
    if (running == g_gameRunning) return;
    g_gameRunning = running;
    RECT audio = ui::Rect(376, 492, 340, 176);
    InvalidateRect(g_window, &audio, FALSE);
}

void PollSaveActivity() {
    if (g_gamePid == 0) {
        g_saveSeenPid = 0;
        return;
    }
    launcher::SaveActivity activity;
    if (!launcher::ReadLiveSave(g_gamePid, activity)) return;
    if (g_saveSeenPid != g_gamePid) {
        g_saveSeenPid = g_gamePid;
        g_saveSeen = activity.savesDone;
        return;
    }
    if (activity.savesDone == g_saveSeen) return;
    g_saveSeen = activity.savesDone;
    AppendLog(L"The game saved in Save Slot " + std::to_wstring(activity.lastSavedSlot) + L".");
}

int AnswerSavePrompt(unsigned long pid, const std::wstring& installDirectory) {
    static bool busy = false;
    int activeSlot = 0;
    if (busy || pid == 0 || !launcher::SavePromptPending(pid, activeSlot) || !launcher::AcceptSavePrompt(pid)) return -1;
    busy = true;
    const launcher::SlotChoice choice = launcher::PickSaveSlot(installDirectory, activeSlot, [pid] { return launcher::SavePromptOpen(pid); });
    busy = false;
    if (choice.withdrawn) return -1;
    launcher::AnswerSavePrompt(pid, choice.slot);
    return choice.slot;
}

std::wstring LauncherPath() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::wstring(buffer, length);
}

void RefreshStatus() {
    if (!g_ready) return;
    ReadControlsIntoSettings();
    std::error_code error;
    const std::wstring executable = g_settings.installDirectory.empty() ? std::wstring() : FindExecutable();
    g_installed = !executable.empty() && std::filesystem::exists(executable, error) &&
                  std::filesystem::exists(std::filesystem::path(g_settings.installDirectory) / L"libs" / L"libkernel.prx", error);
    g_onSteam = launcher::IsOnSteam(LauncherPath());
    SetWindowTextW(Control(kIdSteam), g_onSteam ? L"Update Steam entry" : L"Add to Steam");
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

bool LaunchTitle(Settings& settings, HANDLE* process, const std::function<void(const std::wstring&)>& log) {
    const std::wstring executable = FindExecutable(settings);
    if (executable.empty()) {
        log(L"No executable in the install folder yet: run Install first.");
        return false;
    }
    settings.executable = executable;
    wchar_t number[32];
    std::vector<std::pair<std::wstring, std::wstring>> environment;
    environment.emplace_back(L"APS5_NO_WRITE_WATCH", settings.writeWatch ? L"" : L"1");
    environment.emplace_back(L"ORBIS_GNM_PRESENT_MODE", settings.presentMode);
    environment.emplace_back(L"ORBIS_RENDER_FPS_CAP", std::to_wstring(settings.fpsCap));
    environment.emplace_back(L"ORBIS_VIDEO_UNCAPPED", settings.uncappedFps ? L"1" : L"");
    environment.emplace_back(L"ORBIS_GNM_VALIDATION", settings.validation ? L"1" : L"");
    environment.emplace_back(L"ORBIS_GNM_NO_PIPELINE_CACHE", settings.pipelineCache ? L"" : L"1");
    environment.emplace_back(L"ORBIS_GNM_HEADLESS", settings.headless ? L"1" : L"");
    swprintf(number, 32, L"%.2f", settings.bgmVolume / 100.0);
    environment.emplace_back(L"ORBIS_AUDIO_BGM_GAIN", number);
    swprintf(number, 32, L"%.2f", settings.mainVolume / 100.0);
    environment.emplace_back(L"ORBIS_AUDIO_MAIN_GAIN", number);
    swprintf(number, 32, L"%.2f", settings.musicFloor / 100.0);
    environment.emplace_back(L"ORBIS_NGS2_MUSIC_FLOOR", number);
    environment.emplace_back(L"ORBIS_PAD_CONFIG", launcher::PadConfigurationPath(settings));
    environment.emplace_back(L"ORBIS_SYSTEM_PARAM_LANGUAGE", std::to_wstring(settings.language));
    environment.emplace_back(L"ORBIS_SYSTEM_PARAM_ENTER_BUTTON_ASSIGN", L"1");
    unsigned windowWidth = 0;
    unsigned windowHeight = 0;
    if (swscanf(settings.resolution.c_str(), L"%ux%u", &windowWidth, &windowHeight) != 2) windowWidth = windowHeight = 0;
    environment.emplace_back(L"ORBIS_WINDOW_WIDTH", windowWidth != 0 ? std::to_wstring(windowWidth) : std::wstring());
    environment.emplace_back(L"ORBIS_WINDOW_HEIGHT", windowHeight != 0 ? std::to_wstring(windowHeight) : std::wstring());
    environment.emplace_back(L"ORBIS_WINDOW_BORDERLESS", settings.borderless ? L"1" : L"");
    const int saveSlot = settings.saveSlot >= 1 && settings.saveSlot <= launcher::kSaveSlotCount ? settings.saveSlot : 1;
    environment.emplace_back(L"ORBIS_SAVE_SLOT", std::to_wstring(saveSlot));
    environment.emplace_back(L"ORBIS_SAVE_PROMPT", settings.askSaveSlot ? L"1" : L"");
    std::error_code slotError;
    std::filesystem::create_directories(std::filesystem::path(settings.installDirectory) / L"savedata" / (L"slot_" + std::to_wstring(saveSlot)), slotError);
    environment.emplace_back(L"ORBIS_NGS2_TRACE", settings.diagnostics ? L"1" : L"");
    environment.emplace_back(L"ORBIS_GNM_DUMP_EVERY", settings.diagnostics ? L"3000" : L"");
    environment.emplace_back(L"ORBIS_GNM_LOG", settings.diagnostics ? L"info" : L"");
    std::error_code logError;
    const std::filesystem::path logDirectory = std::filesystem::path(settings.installDirectory) / L"anyps4" / L"log";
    std::filesystem::create_directories(logDirectory, logError);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::wstring logFile = (logDirectory / (std::wstring(L"session-") + stamp + L".log")).wstring();
    std::wstring error;
    if (launcher::StartDetached(executable, settings.installDirectory, environment, logFile, error, process)) {
        log(L"Started " + executable + (settings.uncappedFps ? std::wstring(L" (FPS unlocked") : L" (present " + settings.presentMode + L", frame limit " +
                  (std::to_wstring(settings.fpsCap) + L" fps")) + L", BGM " + std::to_wstring(settings.bgmVolume) + L" %, voices " +
                  std::to_wstring(settings.mainVolume) + L" %, save slot " + std::to_wstring(saveSlot) + (settings.diagnostics ? L", diagnostics on" : L"") + L"); log " + logFile);
        return true;
    }
    log(L"Could not start the title: " + error);
    return false;
}

void StartPresence(launcher::DiscordPresence& presence, const Settings& settings, HANDLE process, const launcher::LineSink& log) {
    if (!settings.discordEnabled) return;
    wchar_t fromEnvironment[64];
    const DWORD length = GetEnvironmentVariableW(L"ANYPS4_DISCORD_CLIENT_ID", fromEnvironment, 64);
    const std::wstring clientId = length > 0 && length < 64 ? std::wstring(fromEnvironment) : settings.discordClientId;
    if (!launcher::ValidDiscordClientId(clientId)) {
        log(L"Discord Rich Presence is on but there is no valid application id: create one at discord.com/developers and paste it in Settings.");
        return;
    }
    presence.Start(clientId, process, log);
}

void Play() {
    ReadControlsIntoSettings();
    launcher::SaveSettings(g_settings);
    launcher::SavePadConfiguration(g_settings);
    HANDLE process = nullptr;
    if (!LaunchTitle(g_settings, &process, AppendLog)) return;
    ForgetGame();
    g_gameProcess = process;
    g_gamePid = GetProcessId(process);
    g_saveSeenPid = g_gamePid;
    g_saveSeen = 0;
    StartPresence(g_presence, g_settings, process, PostLog);
    RefreshGameRunning();
}

int PlayDirect() {
    Settings settings = launcher::LoadSettings();
    launcher::SavePadConfiguration(settings);
    HANDLE process = nullptr;
    std::wstring last;
    if (!LaunchTitle(settings, &process, [&last](const std::wstring& line) { last = line; })) {
        MessageBoxW(nullptr, last.c_str(), L"AnyPS4", MB_ICONERROR);
        return 1;
    }
    launcher::DiscordPresence presence;
    StartPresence(presence, settings, process, [](const std::wstring&) {});
    const unsigned long pid = GetProcessId(process);
    ui::Init();
    while (WaitForSingleObject(process, 50) == WAIT_TIMEOUT) {
        if (settings.askSaveSlot) AnswerSavePrompt(pid, settings.installDirectory);
    }
    ui::Shutdown();
    presence.Stop();
    CloseHandle(process);
    return 0;
}

void Say(const std::wstring& text) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) return;
    const std::wstring line = text + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(), bytes, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

int ExportSavesDirect(const std::wstring& zipFile) {
    const Settings settings = launcher::LoadSettings();
    int count = 0;
    std::wstring error;
    if (!launcher::ExportSaves(settings.installDirectory, zipFile, count, error)) {
        Say(L"Export failed: " + error);
        return 1;
    }
    Say(std::to_wstring(count) + L" files saved to " + zipFile);
    return 0;
}

int ImportSavesDirect(const std::wstring& zipFile) {
    Settings settings = launcher::LoadSettings();
    if (launcher::FindRunningTitle(FindExecutable(settings)) != 0) {
        Say(L"Close the game before importing saves.");
        return 1;
    }
    int count = 0;
    std::wstring error;
    std::wstring previous;
    if (!launcher::ImportSaves(settings.installDirectory, zipFile, count, previous, error)) {
        Say(L"Import failed: " + error);
        return 1;
    }
    Say(std::to_wstring(count) + L" files restored; the previous saves are in " + previous);
    return 0;
}

std::wstring PickZip(bool save) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    const COMDLG_FILTERSPEC filters[] = {{L"ZIP archive (*.zip)", L"*.zip"}, {L"All files", L"*.*"}};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    dialog->SetFileTypes(2, filters);
    dialog->SetDefaultExtension(L"zip");
    dialog->SetTitle(save ? L"Export saves to" : L"Import saves from");
    if (save) dialog->SetFileName(launcher::DefaultBackupName().c_str());
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

void ExportSavesClicked() {
    ReadControlsIntoSettings();
    if (g_settings.installDirectory.empty()) {
        MessageBoxW(g_window, L"Choose the install folder first.", L"Export Saves", MB_ICONINFORMATION);
        return;
    }
    const std::wstring file = PickZip(true);
    if (file.empty()) return;
    int count = 0;
    std::wstring error;
    if (launcher::ExportSaves(g_settings.installDirectory, file, count, error)) {
        const std::wstring text = std::to_wstring(count) + L" files saved to " + file;
        AppendLog(text);
        MessageBoxW(g_window, text.c_str(), L"Export Saves", MB_ICONINFORMATION);
    } else {
        AppendLog(L"Export failed: " + error);
        MessageBoxW(g_window, error.c_str(), L"Export Saves", MB_ICONERROR);
    }
}

void ImportSavesClicked() {
    ReadControlsIntoSettings();
    if (g_settings.installDirectory.empty()) {
        MessageBoxW(g_window, L"Choose the install folder first.", L"Import Saves", MB_ICONINFORMATION);
        return;
    }
    RefreshGameRunning();
    if (g_gamePid != 0) {
        MessageBoxW(g_window, L"Close the game before importing saves.", L"Import Saves", MB_ICONINFORMATION);
        return;
    }
    const std::wstring file = PickZip(false);
    if (file.empty()) return;
    const std::wstring question = L"Replace the saves in " + g_settings.installDirectory + L"\\savedata with the contents of " + file +
                                  L"?\n\nThe current saves are kept in a savedata_backup_<date> folder next to them.";
    if (MessageBoxW(g_window, question.c_str(), L"Import Saves", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    int count = 0;
    std::wstring error;
    std::wstring previous;
    if (launcher::ImportSaves(g_settings.installDirectory, file, count, previous, error)) {
        const std::wstring text = std::to_wstring(count) + L" files restored." + (previous.empty() ? std::wstring() : L" The previous saves are in " + previous);
        AppendLog(text);
        MessageBoxW(g_window, text.c_str(), L"Import Saves", MB_ICONINFORMATION);
    } else {
        AppendLog(L"Import failed: " + error);
        MessageBoxW(g_window, error.c_str(), L"Import Saves", MB_ICONERROR);
    }
}

void AddToSteam() {
    ReadControlsIntoSettings();
    const launcher::SteamResult result = launcher::AddToSteam(LauncherPath(), g_settings.installDirectory, g_settings.gameDirectory, AppendLog);
    AppendLog(result.message);
    RefreshStatus();
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
    const std::wstring titleExecutable = launcher::TitleExecutablePath(g_settings.gameDirectory, g_settings.installDirectory);
    std::thread([command, repo, titleExecutable] {
        int exitCode = -1;
        if (launcher::PrepareOutputFile(titleExecutable, PostLog)) launcher::RunCaptured(command, repo, PostLog, exitCode);
        PostMessageW(g_window, kMessageInstallDone, static_cast<WPARAM>(exitCode), 0);
    }).detach();
}

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

int ResolutionIndex() {
    for (int k = 0; k < kResolutionCount; ++k) {
        if (g_settings.resolution == kResolutionValues[k]) return k;
    }
    return 1;
}

int SaveSlotIndex() {
    return g_settings.saveSlot >= 1 && g_settings.saveSlot <= launcher::kSaveSlotCount ? g_settings.saveSlot - 1 : 0;
}

void UpdateUncappedControls() {
    const BOOL enabled = g_checked[kIdUncapped] ? FALSE : TRUE;
    for (int k = 0; k < 3; ++k) {
        EnableWindow(Control(kIdPresentBase + k), enabled);
        InvalidateRect(Control(kIdPresentBase + k), nullptr, FALSE);
    }
    for (int k = 0; k < 2; ++k) {
        EnableWindow(Control(kIdFpsBase + k), enabled);
        InvalidateRect(Control(kIdFpsBase + k), nullptr, FALSE);
    }
}

void AddSlider(int page, int id, int labelY, std::function<std::wstring()> label, int value) {
    AddLabel(page, 396, labelY, 300, 18, std::move(label), ui::kFontSmall, ui::kText);
    HWND bar = AddChild(page, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, 392, labelY + 16, 308, 22, id);
    SendMessageW(bar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 150));
    SendMessageW(bar, TBM_SETPOS, TRUE, value);
}

void AddCheckbox(int page, const wchar_t* text, int x, int y, int width, int id, bool checked) {
    g_checked[id] = checked;
    AddButton(page, text, x, y, width, 26, id);
}

void CreateControls() {
    AddButton(-1, L"Home", 912, 32, 108, 36, kIdTabHome);
    AddButton(-1, L"⚙  Settings", 1028, 32, 128, 36, kIdTabSettings);

    AddCard(kPageHome, 24, 112, 1132, 216, L"GAME");
    AddLabel(kPageHome, 44, 150, 600, 18, L"Game folder (the one that contains eboot.bin)");
    AddField(kPageHome, g_settings.gameDirectory, 44, 172, 932, 38, kIdGame);
    AddButton(kPageHome, L"Browse...", 988, 172, 148, 38, kIdGameBrowse);
    AddLabel(kPageHome, 44, 216, 1092, 20, [] {
        return g_installed ? std::wstring(L"Ready to play. Install again only if you move the game folder or change its files.")
                           : std::wstring(L"Pick your game folder, then press Install (it takes a few seconds). Your game files are never copied.");
    });
    AddLabel(kPageHome, 652, 150, 484, 18, [] { return L"Active save slot: " + std::wstring(kSaveSlotCaptions[SaveSlotIndex()]); }, ui::kFontSmall, ui::kVioletLight,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    AddButton(kPageHome, L"PLAY", 44, 246, 400, 64, kIdPlay);
    AddButton(kPageHome, L"Install", 456, 246, 200, 64, kIdInstall);
    AddButton(kPageHome, L"Open install folder", 668, 246, 210, 64, kIdOpenFolder);
    AddButton(kPageHome, L"Add to Steam", 890, 246, 246, 64, kIdSteam);

    AddCard(kPageHome, 24, 344, 1132, 324, L"ACTIVITY");
    {
        g_fields.push_back({kPageHome, ui::Rect(44, 384, 1092, 268), nullptr});
        g_log = AddChild(kPageHome, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 52, 390, 1076, 256, kIdLog);
        g_fields.back().edit = g_log;
        SendMessageW(g_log, WM_SETFONT, reinterpret_cast<WPARAM>(ui::FontHandle(ui::kFontMono)), TRUE);
        ui::StyleScrollBars(g_log);
    }

    AddCard(kPageSettings, 24, 112, 340, 140, L"PATHS AND SAVES");
    AddLabel(kPageSettings, 44, 146, 300, 18, L"Install folder (outside the game folder)");
    AddField(kPageSettings, g_settings.installDirectory, 44, 168, 190, 36, kIdInstallDir);
    AddButton(kPageSettings, L"Browse...", 242, 168, 102, 36, kIdInstallBrowse);
    AddButton(kPageSettings, L"Open folder", 44, 212, 92, 30, kIdOpenFolder + 1000);
    AddButton(kPageSettings, L"Export Saves", 142, 212, 98, 30, kIdExportSaves);
    AddButton(kPageSettings, L"Import Saves", 246, 212, 98, 30, kIdImportSaves);

    AddCard(kPageSettings, 24, 264, 340, 186, L"GAME AND DISCORD");
    AddLabel(kPageSettings, 44, 300, 92, 28, L"Game language", ui::kFontSmall, ui::kText);
    for (int k = 0; k < 2; ++k) AddButton(kPageSettings, kLanguageCaptions[k], 140 + k * 102, 300, 98, 28, kIdLanguageBase + k);
    AddCheckbox(kPageSettings, L"Discord Rich Presence while the game runs", 44, 342, 300, kIdDiscord, g_settings.discordEnabled);
    AddLabel(kPageSettings, 44, 374, 300, 16, L"Discord application id (discord.com/developers)");
    AddField(kPageSettings, g_settings.discordClientId, 44, 394, 300, 30, kIdDiscordId);

    AddCard(kPageSettings, 376, 112, 340, 368, L"GRAPHICS");
    AddLabel(kPageSettings, 396, 146, 88, 28, L"Present mode", ui::kFontSmall, ui::kText);
    for (int k = 0; k < 3; ++k) AddButton(kPageSettings, kPresentCaptions[k], 488 + k * 70, 146, 66, 28, kIdPresentBase + k);
    AddLabel(kPageSettings, 396, 180, 88, 28, L"Frame limit", ui::kFontSmall, ui::kText);
    for (int k = 0; k < 2; ++k) AddButton(kPageSettings, kFpsCaptions[k], 488 + k * 70, 180, 66, 28, kIdFpsBase + k);
    AddLabel(kPageSettings, 396, 216, 300, 16, L"Window Size (Output)", ui::kFontSmall, ui::kText);
    AddButton(kPageSettings, kResolutionCaptions[ResolutionIndex()], 396, 234, 300, 28, kIdResolution);
    AddLabel(kPageSettings, 396, 264, 300, 32, L"Scales display window. Internal game render stays 1080p.", ui::kFontSmall, ui::kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    AddCheckbox(kPageSettings, L"Borderless Fullscreen", 396, 302, 300, kIdBorderless, g_settings.borderless);
    AddCheckbox(kPageSettings, L"Unlock FPS (no frame limit, no V-Sync)", 396, 332, 300, kIdUncapped, g_settings.uncappedFps);
    AddCheckbox(kPageSettings, L"Persistent pipeline cache", 396, 362, 300, kIdPipelineCache, g_settings.pipelineCache);
    AddCheckbox(kPageSettings, L"Vulkan validation layer", 396, 392, 300, kIdValidation, g_settings.validation);
    AddCheckbox(kPageSettings, L"Headless (no rendering)", 396, 422, 300, kIdHeadless, g_settings.headless);
    AddCheckbox(kPageSettings, L"Diagnostics (writes logs, slows the game)", 396, 452, 300, kIdDiagnostics, g_settings.diagnostics);
    EnableWindow(Control(kIdResolution), g_settings.borderless ? FALSE : TRUE);
    UpdateUncappedControls();

    AddCard(kPageSettings, 24, 462, 340, 206, L"SAVE SLOT");
    AddLabel(kPageSettings, 44, 496, 300, 16, L"Slot the game loads and saves in", ui::kFontSmall, ui::kText);
    AddButton(kPageSettings, kSaveSlotCaptions[SaveSlotIndex()], 44, 516, 300, 28, kIdSaveSlot);
    AddCheckbox(kPageSettings, L"Ask which slot when saving at the fairy", 44, 552, 300, kIdAskSaveSlot, g_settings.askSaveSlot);
    AddLabel(kPageSettings, 44, 584, 300, 44, L"Each slot has its own saves. A running game uses a new slot from its next save.",
             ui::kFontSmall, ui::kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);

    AddCard(kPageSettings, 376, 492, 340, 176, L"AUDIO");
    AddLabel(kPageSettings, 456, 504, 240, 20, [] { return g_gameRunning ? std::wstring(L"Live: the running game follows these sliders") : std::wstring(L"Applied live while the game runs"); },
             ui::kFontSmall, ui::kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    AddSlider(kPageSettings, kIdBgm, 530, [] { return L"Music (BGM): " + std::to_wstring(SendMessageW(Control(kIdBgm), TBM_GETPOS, 0, 0)) + L" %"; }, g_settings.bgmVolume);
    AddSlider(kPageSettings, kIdMain, 570, [] { return L"Voices and effects: " + std::to_wstring(SendMessageW(Control(kIdMain), TBM_GETPOS, 0, 0)) + L" %"; },
              g_settings.mainVolume);
    AddSlider(kPageSettings, kIdFloor, 610,
              [] { return L"Music floor when the title mutes it: " + std::to_wstring(SendMessageW(Control(kIdFloor), TBM_GETPOS, 0, 0)) + L" %"; },
              g_settings.musicFloor);

    AddCard(kPageSettings, 728, 112, 428, 556, L"CONTROLS");
    AddLabel(kPageSettings, 748, 146, 388, 18, L"Click a key to change it. Escape cancels.");
    std::size_t count = 0;
    const anyps4::PadBinding* bindings = anyps4::PadBindings(count);
    for (std::size_t k = 0; k < count; ++k) {
        const int column = static_cast<int>(k / 12);
        const int row = static_cast<int>(k % 12);
        const int x = 748 + column * 196;
        const int y = 176 + row * 28;
        AddLabel(kPageSettings, x, y, 104, 24, Widen(bindings[k].label).c_str(), ui::kFontSmall, ui::kText);
        AddButton(kPageSettings, Widen(anyps4::PadKeyName_(g_settings.keys[k]).c_str()).c_str(), x + 106, y, 82, 24, kIdKeyBase + static_cast<int>(k));
    }
    AddCheckbox(kPageSettings, L"Use an XInput gamepad (slot 1) when connected", 748, 520, 380, kIdXInput, g_settings.xinput);

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
        const std::wstring text = g_installed ? L"Ready" : L"Not installed";
        const COLORREF color = g_installed ? ui::kGreen : ui::kAmber;
        RECT measure = {0, 0, 0, 0};
        HGDIOBJ previous = SelectObject(dc, ui::FontHandle(ui::kFontButton));
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measure, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(dc, previous);
        const int textWidth = measure.right - measure.left;
        const int dot = S(12);
        const int right = S(1136);
        const int top = S(124);
        const int rowHeight = S(24);
        const int left = right - textWidth - S(8) - dot;
        if (g_onSteam) {
            const std::wstring badge = L"On Steam";
            RECT badgeMeasure = {0, 0, 0, 0};
            HGDIOBJ badgePrevious = SelectObject(dc, ui::FontHandle(ui::kFontSmall));
            DrawTextW(dc, badge.c_str(), static_cast<int>(badge.size()), &badgeMeasure, DT_CALCRECT | DT_SINGLELINE);
            SelectObject(dc, badgePrevious);
            const int check = S(12);
            const int badgeWidth = S(10) + check + S(6) + (badgeMeasure.right - badgeMeasure.left) + S(12);
            const int badgeRight = left - S(18);
            const RECT pill = {badgeRight - badgeWidth, top + S(1), badgeRight, top + rowHeight - S(1)};
            ui::FillRound(dc, pill, S(11), RGB(38, 34, 74), ui::kViolet);
            ui::DrawCheck(dc, RECT{pill.left + S(10), pill.top + (pill.bottom - pill.top - check) / 2, pill.left + S(10) + check, pill.top + (pill.bottom - pill.top - check) / 2 + check}, ui::kVioletLight);
            ui::DrawTextLine(dc, badge, RECT{pill.left + S(10) + check + S(6), pill.top, pill.right, pill.bottom}, ui::kFontSmall, ui::kVioletLight, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        ui::FillEllipse(dc, RECT{left, top + (rowHeight - dot) / 2, left + dot, top + (rowHeight - dot) / 2 + dot}, color);
        ui::DrawTextLine(dc, text, RECT{left + dot + S(8), top, right + S(2), top + rowHeight}, ui::kFontButton, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

enum class Kind { Primary, Secondary, Tab, Segment, Check, Key, Dropdown };

Kind KindOf(int id) {
    if (id == kIdPlay) return Kind::Primary;
    if (id == kIdResolution || id == kIdSaveSlot) return Kind::Dropdown;
    if (id == kIdTabHome || id == kIdTabSettings) return Kind::Tab;
    if ((id >= kIdPresentBase && id < kIdPresentBase + 3) || (id >= kIdFpsBase && id < kIdFpsBase + 2) || (id >= kIdLanguageBase && id < kIdLanguageBase + 2)) return Kind::Segment;
    if (IsCheckbox(id)) return Kind::Check;
    if (id >= kIdKeyBase && id < kIdKeyBase + 24) return Kind::Key;
    return Kind::Secondary;
}

bool IsSegmentSelected(int id) {
    if (id >= kIdPresentBase && id < kIdPresentBase + 3) return g_settings.presentMode == kPresentModes[id - kIdPresentBase];
    if (id >= kIdLanguageBase && id < kIdLanguageBase + 2) return g_settings.language == kLanguageValues[id - kIdLanguageBase];
    return g_settings.fpsCap == kFpsValues[id - kIdFpsBase];
}

const wchar_t* const* g_menuCaptions = nullptr;
int g_menuSelected = 0;

void DrawMenuItem(const DRAWITEMSTRUCT* item) {
    const int index = static_cast<int>(item->itemData);
    HDC dc = item->hDC;
    const RECT r = item->rcItem;
    ui::FillSolid(dc, r, (item->itemState & ODS_SELECTED) != 0 ? ui::kButtonHover : ui::kField);
    if (index == g_menuSelected) {
        const int size = S(14);
        ui::DrawCheck(dc, RECT{r.left + S(10), r.top + (r.bottom - r.top - size) / 2, r.left + S(10) + size, r.top + (r.bottom - r.top - size) / 2 + size}, ui::kVioletLight);
    }
    ui::DrawTextLine(dc, g_menuCaptions[index], RECT{r.left + S(34), r.top, r.right, r.bottom}, ui::kFontSmall, ui::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
        ui::DrawTextLine(dc, caption, r, width < S(130) ? ui::kFontSmall : ui::kFontButton, disabled ? ui::kDisabled : ui::kText, center);
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
        if (disabled) {
            ui::FillRound(dc, r, S(8), ui::kButton, ui::kCardBorder);
            ui::DrawTextLine(dc, caption, r, ui::kFontSmall, IsSegmentSelected(id) ? ui::kMuted : ui::kDisabled, center);
        } else if (IsSegmentSelected(id)) {
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
    case Kind::Dropdown: {
        ui::FillRound(dc, r, S(8), pressed ? ui::kCard : (hover ? ui::kButtonHover : ui::kButton), ui::kCardBorder);
        const COLORREF color = disabled ? ui::kDisabled : ui::kText;
        ui::DrawTextLine(dc, caption, RECT{S(12), 0, width - S(30), height}, ui::kFontSmall, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const int size = S(10);
        ui::DrawChevron(dc, RECT{width - S(24), (height - size) / 2, width - S(24) + size, (height - size) / 2 + size}, color);
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

int ChooseFromMenu(int control, int firstItem, int count, const wchar_t* const* captions, int selected) {
    g_menuCaptions = captions;
    g_menuSelected = selected;
    HMENU menu = CreatePopupMenu();
    for (int k = 0; k < count; ++k) AppendMenuW(menu, MF_OWNERDRAW, static_cast<UINT_PTR>(firstItem + k), reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(k)));
    MENUINFO info{};
    info.cbSize = sizeof info;
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = g_fieldBrush;
    SetMenuInfo(menu, &info);
    RECT button{};
    GetWindowRect(Control(control), &button);
    const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, button.left, button.bottom + S(2), 0, g_window, nullptr);
    DestroyMenu(menu);
    if (chosen < firstItem || chosen >= firstItem + count) return -1;
    SetWindowTextW(Control(control), captions[chosen - firstItem]);
    InvalidateRect(Control(control), nullptr, FALSE);
    return chosen - firstItem;
}

void ChooseResolution() {
    const int index = ChooseFromMenu(kIdResolution, kIdResolutionItem, kResolutionCount, kResolutionCaptions, ResolutionIndex());
    if (index < 0) return;
    g_settings.resolution = kResolutionValues[index];
    PersistSettings();
}

void ChooseSaveSlot() {
    const int index = ChooseFromMenu(kIdSaveSlot, kIdSlotItem, launcher::kSaveSlotCount, kSaveSlotCaptions, SaveSlotIndex());
    if (index < 0) return;
    g_settings.saveSlot = index + 1;
    InvalidateRect(g_window, nullptr, FALSE);
    PersistSettings();
    if (g_gamePid == 0) return;
    const std::wstring name = L"Save Slot " + std::to_wstring(g_settings.saveSlot);
    if (launcher::PushLiveSaveSlot(g_gamePid, g_settings.saveSlot)) {
        AppendLog(name + L" selected. The running game uses it from its next save, autosaves included.");
    } else {
        AppendLog(name + L" selected, but the running game has not opened its save system yet: close it and press PLAY to use the new slot.");
    }
}

void AnswerSavePromptInWindow() {
    const int slot = AnswerSavePrompt(g_gamePid, g_settings.installDirectory);
    if (slot < 0) return;
    if (slot == 0) {
        AppendLog(L"The save was canceled.");
        return;
    }
    g_settings.saveSlot = slot;
    SetWindowTextW(Control(kIdSaveSlot), kSaveSlotCaptions[SaveSlotIndex()]);
    InvalidateRect(g_window, nullptr, FALSE);
    PersistSettings();
    AppendLog(L"Saving in Save Slot " + std::to_wstring(slot) + L".");
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
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure->CtlType == ODT_MENU) {
            measure->itemWidth = static_cast<UINT>(S(206));
            measure->itemHeight = static_cast<UINT>(S(30));
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM:
        if (reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_BUTTON) {
            DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        if (reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_MENU) {
            DrawMenuItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
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
            static int ticks = 0;
            UpdateHover();
            if (++ticks % 20 == 0) RefreshGameRunning();
            if (ticks % 10 == 0) PollSaveActivity();
            if (g_gamePid != 0) AnswerSavePromptInWindow();
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
        if (id == kIdGame || id == kIdInstallDir || id == kIdDiscordId) {
            if (code == EN_CHANGE && id != kIdDiscordId) RefreshStatus();
            if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(window, nullptr, FALSE);
            if (code == EN_KILLFOCUS && id == kIdDiscordId) PersistSettings();
            return 0;
        }
        if (code != BN_CLICKED) break;
        if (id == kIdResolution) {
            ChooseResolution();
            return 0;
        }
        if (id == kIdSaveSlot) {
            ChooseSaveSlot();
            return 0;
        }
        if (IsCheckbox(id)) {
            g_checked[id] = !g_checked[id];
            if (id == kIdBorderless) EnableWindow(Control(kIdResolution), g_checked[id] ? FALSE : TRUE);
            if (id == kIdUncapped) UpdateUncappedControls();
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
        if (id >= kIdLanguageBase && id < kIdLanguageBase + 2) {
            g_settings.language = kLanguageValues[id - kIdLanguageBase];
            for (int k = 0; k < 2; ++k) InvalidateRect(Control(kIdLanguageBase + k), nullptr, FALSE);
            PersistSettings();
            return 0;
        }
        if (id >= kIdFpsBase && id < kIdFpsBase + 2) {
            g_settings.fpsCap = kFpsValues[id - kIdFpsBase];
            for (int k = 0; k < 2; ++k) InvalidateRect(Control(kIdFpsBase + k), nullptr, FALSE);
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
        case kIdSteam:
            AddToSteam();
            return 0;
        case kIdExportSaves:
            ExportSavesClicked();
            return 0;
        case kIdImportSaves:
            ImportSavesClicked();
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
            RECT audio = ui::Rect(376, 492, 340, 176);
            InvalidateRect(window, &audio, FALSE);
        }
        if (g_gamePid != 0) {
            launcher::PushLiveAudio(g_gamePid, static_cast<float>(SendMessageW(Control(kIdBgm), TBM_GETPOS, 0, 0)) / 100.0f,
                                    static_cast<float>(SendMessageW(Control(kIdMain), TBM_GETPOS, 0, 0)) / 100.0f,
                                    static_cast<float>(SendMessageW(Control(kIdFloor), TBM_GETPOS, 0, 0)) / 100.0f);
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
        g_presence.Stop();
        ForgetGame();
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
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    bool direct = false;
    std::wstring exportFile;
    std::wstring importFile;
    for (int k = 1; arguments && k < argumentCount; ++k) {
        if (wcscmp(arguments[k], L"--play") == 0) direct = true;
        else if (wcscmp(arguments[k], L"--export-saves") == 0 && k + 1 < argumentCount) exportFile = arguments[++k];
        else if (wcscmp(arguments[k], L"--import-saves") == 0 && k + 1 < argumentCount) importFile = arguments[++k];
    }
    if (arguments) LocalFree(arguments);
    if (!exportFile.empty()) return ExportSavesDirect(exportFile);
    if (!importFile.empty()) return ImportSavesDirect(importFile);
    if (direct) return PlayDirect();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    ui::Init();
    g_settings = launcher::LoadSettings();
    if (g_settings.fpsCap != 30) g_settings.fpsCap = 60;
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
