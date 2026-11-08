#include "Settings.hpp"

#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>

#include "PadKeyNames.hpp"

namespace launcher {

namespace {

constexpr const wchar_t* kSection = L"anyps4";

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr, nullptr);
    return narrow;
}

std::wstring ReadString(const std::wstring& file, const wchar_t* key, const std::wstring& fallback) {
    wchar_t buffer[2048];
    GetPrivateProfileStringW(kSection, key, fallback.c_str(), buffer, 2048, file.c_str());
    return buffer;
}

int ReadInt(const std::wstring& file, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, file.c_str()));
}

void WriteString(const std::wstring& file, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(kSection, key, value.c_str(), file.c_str());
}

void WriteInt(const std::wstring& file, const wchar_t* key, int value) {
    WriteString(file, key, std::to_wstring(value));
}

}  // namespace

std::wstring SettingsPath() {
    wchar_t overrideDirectory[MAX_PATH];
    const DWORD overrideLength = GetEnvironmentVariableW(L"ANYPS4_CONFIG_DIR", overrideDirectory, MAX_PATH);
    if (overrideLength > 0 && overrideLength < MAX_PATH) {
        std::error_code error;
        std::filesystem::create_directories(overrideDirectory, error);
        return (std::filesystem::path(overrideDirectory) / L"launcher.ini").wstring();
    }
    wchar_t appData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        const std::filesystem::path directory = std::filesystem::path(appData) / L"AnyPS4";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return (directory / L"launcher.ini").wstring();
    }
    return L"launcher.ini";
}

Settings LoadSettings() {
    Settings settings;
    const std::wstring file = SettingsPath();
    settings.gameDirectory = ReadString(file, L"game", L"");
    settings.installDirectory = ReadString(file, L"install", L"");
    settings.executable = ReadString(file, L"executable", L"");
    settings.repoRoot = ReadString(file, L"repo", Widen(ANYPS4_REPO_ROOT));
    settings.buildDirectory = ReadString(file, L"build", Widen(ANYPS4_BUILD_DIR));
    settings.shadps4Checkout = ReadString(file, L"shadps4", L"");
    settings.toolchainBin = ReadString(file, L"toolchain", L"");
    settings.relinker = ReadString(file, L"relinker", L"");
    settings.python = ReadString(file, L"python", L"python");
    settings.cmake = ReadString(file, L"cmake", L"cmake");
    settings.presentMode = ReadString(file, L"present", L"Fifo");
    settings.fpsCap = ReadInt(file, L"fps_cap", 60);
    settings.validation = ReadInt(file, L"validation", 0) != 0;
    settings.pipelineCache = ReadInt(file, L"pipeline_cache", 1) != 0;
    settings.headless = ReadInt(file, L"headless", 0) != 0;
    settings.writeWatch = ReadInt(file, L"write_watch", 0) != 0;
    settings.diagnostics = ReadInt(file, L"diagnostics", 0) != 0;
    settings.bgmVolume = ReadInt(file, L"bgm_volume", 70);
    settings.mainVolume = ReadInt(file, L"main_volume", 100);
    settings.musicFloor = ReadInt(file, L"music_floor_v2", 0);
    settings.xinput = ReadInt(file, L"xinput", 1) != 0;
    settings.language = ReadInt(file, L"language", 1);
    settings.resolution = ReadString(file, L"resolution", L"1920x1080");
    settings.borderless = ReadInt(file, L"borderless", 0) != 0;
    settings.saveSlot = ReadInt(file, L"save_slot", 1);
    if (settings.saveSlot < 1 || settings.saveSlot > kSaveSlotCount) settings.saveSlot = 1;
    settings.askSaveSlot = ReadInt(file, L"ask_save_slot", 1) != 0;
    settings.uncappedFps = ReadInt(file, L"uncapped_fps", 0) != 0;
    settings.discordEnabled = ReadInt(file, L"discord", 1) != 0;
    settings.discordClientId = ReadString(file, L"discord_client_id", L"");
    std::size_t count = 0;
    const anyps4::PadBinding* bindings = anyps4::PadBindings(count);
    for (std::size_t k = 0; k < count && k < settings.keys.size(); ++k) {
        const std::wstring key = L"key_" + Widen(bindings[k].setting);
        const std::wstring name = ReadString(file, key.c_str(), Widen(anyps4::PadKeyName_(bindings[k].defaultKey)));
        settings.keys[k] = anyps4::PadKeyFromName(Narrow(name));
    }
    return settings;
}

void SaveSettings(const Settings& settings) {
    const std::wstring file = SettingsPath();
    WriteString(file, L"game", settings.gameDirectory);
    WriteString(file, L"install", settings.installDirectory);
    WriteString(file, L"executable", settings.executable);
    WriteString(file, L"repo", settings.repoRoot);
    WriteString(file, L"build", settings.buildDirectory);
    WriteString(file, L"shadps4", settings.shadps4Checkout);
    WriteString(file, L"toolchain", settings.toolchainBin);
    WriteString(file, L"relinker", settings.relinker);
    WriteString(file, L"python", settings.python);
    WriteString(file, L"cmake", settings.cmake);
    WriteString(file, L"present", settings.presentMode);
    WriteInt(file, L"fps_cap", settings.fpsCap);
    WriteInt(file, L"validation", settings.validation ? 1 : 0);
    WriteInt(file, L"pipeline_cache", settings.pipelineCache ? 1 : 0);
    WriteInt(file, L"headless", settings.headless ? 1 : 0);
    WriteInt(file, L"write_watch", settings.writeWatch ? 1 : 0);
    WriteInt(file, L"diagnostics", settings.diagnostics ? 1 : 0);
    WriteInt(file, L"bgm_volume", settings.bgmVolume);
    WriteInt(file, L"main_volume", settings.mainVolume);
    WriteInt(file, L"music_floor_v2", settings.musicFloor);
    WriteInt(file, L"xinput", settings.xinput ? 1 : 0);
    WriteInt(file, L"language", settings.language);
    WriteString(file, L"resolution", settings.resolution);
    WriteInt(file, L"borderless", settings.borderless ? 1 : 0);
    WriteInt(file, L"save_slot", settings.saveSlot);
    WriteInt(file, L"ask_save_slot", settings.askSaveSlot ? 1 : 0);
    WriteInt(file, L"uncapped_fps", settings.uncappedFps ? 1 : 0);
    WriteInt(file, L"discord", settings.discordEnabled ? 1 : 0);
    WriteString(file, L"discord_client_id", settings.discordClientId);
    std::size_t count = 0;
    const anyps4::PadBinding* bindings = anyps4::PadBindings(count);
    for (std::size_t k = 0; k < count && k < settings.keys.size(); ++k) {
        const std::wstring key = L"key_" + Widen(bindings[k].setting);
        WriteString(file, key.c_str(), Widen(anyps4::PadKeyName_(settings.keys[k])));
    }
}

std::wstring PadConfigurationPath(const Settings& settings) {
    if (settings.installDirectory.empty()) return {};
    return (std::filesystem::path(settings.installDirectory) / L"anyps4" / L"pad.ini").wstring();
}

void SavePadConfiguration(const Settings& settings) {
    const std::wstring path = PadConfigurationPath(settings);
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    std::ofstream file(std::filesystem::path(path), std::ios::trunc);
    file << "[pad]\n";
    file << "xinput=" << (settings.xinput ? 1 : 0) << "\n";
    std::size_t count = 0;
    const anyps4::PadBinding* bindings = anyps4::PadBindings(count);
    for (std::size_t k = 0; k < count && k < settings.keys.size(); ++k) {
        file << bindings[k].setting << "=" << anyps4::PadKeyName_(settings.keys[k]) << "\n";
    }
}

}  // namespace launcher
