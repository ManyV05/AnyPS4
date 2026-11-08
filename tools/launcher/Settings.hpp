#pragma once

#include <array>
#include <string>

namespace launcher {

constexpr int kSaveSlotCount = 5;

struct Settings {
    std::wstring gameDirectory;
    std::wstring installDirectory;
    std::wstring executable;
    std::wstring repoRoot;
    std::wstring buildDirectory;
    std::wstring shadps4Checkout;
    std::wstring toolchainBin;
    std::wstring relinker;
    std::wstring python = L"python";
    std::wstring cmake = L"cmake";
    std::wstring presentMode = L"Fifo";
    int fpsCap = 60;
    bool validation = false;
    bool pipelineCache = true;
    bool headless = false;
    bool writeWatch = false;
    bool diagnostics = false;
    int bgmVolume = 70;
    int mainVolume = 100;
    int musicFloor = 0;
    bool xinput = true;
    int language = 1;
    std::wstring resolution = L"1920x1080";
    bool borderless = false;
    int saveSlot = 1;
    bool askSaveSlot = true;
    bool discordEnabled = true;
    std::wstring discordClientId;
    std::array<int, 24> keys{};
};

std::wstring SettingsPath();
Settings LoadSettings();
void SaveSettings(const Settings& settings);
void SavePadConfiguration(const Settings& settings);
std::wstring PadConfigurationPath(const Settings& settings);

}  // namespace launcher
