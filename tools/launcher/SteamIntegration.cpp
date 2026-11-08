#include "SteamIntegration.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <objidl.h>
#include <vector>

using std::max;
using std::min;
#include <gdiplus.h>

namespace launcher {

namespace {

namespace fs = std::filesystem;

constexpr const char* kTitle = "The Witch and the Hundred Knight: Revival Edition";
constexpr const char* kLaunchOptions = "--play";
constexpr int kMaxDepth = 8;

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr, nullptr);
    return narrow;
}

bool EqualsIgnoreCase(const std::string& left, const char* right) {
    return _stricmp(left.c_str(), right) == 0;
}

std::uint32_t Crc32(const std::string& text) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            values[n] = c;
        }
        return values;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char byte : text) crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool ReadRegistryString(HKEY root, const wchar_t* path, const wchar_t* name, std::wstring& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD size = sizeof buffer - sizeof(wchar_t);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(key);
    if (ok) value = buffer;
    return ok;
}

bool ReadRegistryNumber(HKEY root, const wchar_t* path, const wchar_t* name, DWORD& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD size = sizeof value;
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD;
    RegCloseKey(key);
    return ok;
}

bool IsNumber(const std::wstring& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

struct SteamUser {
    fs::path root;
    fs::path directory;
    bool overridden = false;
};

bool FindSteamUser(SteamUser& user, std::wstring& problem) {
    wchar_t overrideRoot[MAX_PATH];
    const DWORD overrideLength = GetEnvironmentVariableW(L"ANYPS4_STEAM_DIR", overrideRoot, MAX_PATH);
    user.overridden = overrideLength > 0 && overrideLength < MAX_PATH;
    if (user.overridden) {
        user.root = overrideRoot;
    } else {
        std::wstring path;
        if (!ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", path) || path.empty()) {
            problem = L"Steam was not found: HKCU\\Software\\Valve\\Steam\\SteamPath is missing.";
            return false;
        }
        user.root = fs::path(path).lexically_normal();
    }
    std::error_code error;
    const fs::path userdata = user.root / L"userdata";
    if (!fs::is_directory(userdata, error)) {
        problem = L"Steam has no userdata folder yet (" + userdata.wstring() + L"): sign in to Steam once and try again.";
        return false;
    }
    DWORD active = 0;
    if (!user.overridden && ReadRegistryNumber(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", active) && active != 0) {
        const fs::path candidate = userdata / std::to_wstring(active);
        if (fs::is_directory(candidate, error)) {
            user.directory = candidate;
            return true;
        }
    }
    fs::file_time_type newestTime{};
    for (const auto& entry : fs::directory_iterator(userdata, error)) {
        const std::wstring name = entry.path().filename().wstring();
        if (!entry.is_directory(error) || !IsNumber(name) || name == L"0") continue;
        const auto time = fs::last_write_time(entry.path(), error);
        if (user.directory.empty() || time > newestTime) {
            user.directory = entry.path();
            newestTime = time;
        }
    }
    if (user.directory.empty()) {
        problem = L"No Steam account folder in " + userdata.wstring() + L": sign in to Steam once and try again.";
        return false;
    }
    return true;
}

bool SteamRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    bool running = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !running; more = Process32NextW(snapshot, &entry)) {
        running = _wcsicmp(entry.szExeFile, L"steam.exe") == 0;
    }
    CloseHandle(snapshot);
    return running;
}

struct Shortcut {
    std::size_t begin = 0;
    std::size_t end = 0;
    int index = 0;
    std::string name;
    std::string executable;
    std::string options;
    std::uint32_t appId = 0;
    bool hasAppId = false;
};

struct Shortcuts {
    std::vector<Shortcut> entries;
    std::size_t insertAt = 0;
};

bool ReadKey(const std::string& data, std::size_t& position, std::string& key) {
    const std::size_t end = data.find('\0', position);
    if (end == std::string::npos) return false;
    key.assign(data, position, end - position);
    position = end + 1;
    return true;
}

bool ParseMap(const std::string& data, std::size_t& position, Shortcut* shortcut, int depth) {
    if (depth > kMaxDepth) return false;
    while (true) {
        if (position >= data.size()) return false;
        const unsigned char type = static_cast<unsigned char>(data[position++]);
        if (type == 8) return true;
        std::string key;
        if (!ReadKey(data, position, key)) return false;
        switch (type) {
        case 0:
            if (!ParseMap(data, position, nullptr, depth + 1)) return false;
            break;
        case 1: {
            std::string value;
            if (!ReadKey(data, position, value)) return false;
            if (shortcut) {
                if (EqualsIgnoreCase(key, "AppName")) shortcut->name = value;
                else if (EqualsIgnoreCase(key, "Exe")) shortcut->executable = value;
                else if (EqualsIgnoreCase(key, "LaunchOptions")) shortcut->options = value;
            }
            break;
        }
        case 2:
        case 3:
        case 4:
        case 6:
            if (position + 4 > data.size()) return false;
            if (shortcut && type == 2 && EqualsIgnoreCase(key, "appid")) {
                std::memcpy(&shortcut->appId, data.data() + position, 4);
                shortcut->hasAppId = true;
            }
            position += 4;
            break;
        case 7:
        case 10:
            if (position + 8 > data.size()) return false;
            position += 8;
            break;
        default:
            return false;
        }
    }
}

bool ParseShortcuts(const std::string& data, Shortcuts& result) {
    if (data.size() < 2 || data[0] != '\0') return false;
    std::size_t position = 1;
    std::string root;
    if (!ReadKey(data, position, root) || !EqualsIgnoreCase(root, "shortcuts")) return false;
    while (true) {
        if (position >= data.size()) return false;
        const std::size_t begin = position;
        const unsigned char type = static_cast<unsigned char>(data[position++]);
        if (type == 8) {
            result.insertAt = begin;
            return true;
        }
        if (type != 0) return false;
        std::string index;
        if (!ReadKey(data, position, index)) return false;
        Shortcut shortcut;
        shortcut.begin = begin;
        shortcut.index = std::atoi(index.c_str());
        if (!ParseMap(data, position, &shortcut, 1)) return false;
        shortcut.end = position;
        result.entries.push_back(shortcut);
    }
}

void AddField(std::string& out, char type, const char* key) {
    out.push_back(type);
    out.append(key);
    out.push_back('\0');
}

void AddText(std::string& out, const char* key, const std::string& value) {
    AddField(out, 1, key);
    out.append(value);
    out.push_back('\0');
}

void AddInteger(std::string& out, const char* key, std::uint32_t value) {
    AddField(out, 2, key);
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

std::string BuildEntry(int index, std::uint32_t appId, const std::string& name, const std::string& executable, const std::string& startDirectory, const std::string& icon) {
    std::string out;
    out.push_back('\0');
    out.append(std::to_string(index));
    out.push_back('\0');
    AddInteger(out, "appid", appId);
    AddText(out, "AppName", name);
    AddText(out, "Exe", executable);
    AddText(out, "StartDir", startDirectory);
    AddText(out, "icon", icon);
    AddText(out, "ShortcutPath", "");
    AddText(out, "LaunchOptions", kLaunchOptions);
    AddInteger(out, "IsHidden", 0);
    AddInteger(out, "AllowDesktopConfig", 1);
    AddInteger(out, "AllowOverlay", 1);
    AddInteger(out, "OpenVR", 0);
    AddInteger(out, "Devkit", 0);
    AddText(out, "DevkitGameID", "");
    AddInteger(out, "DevkitOverrideAppID", 0);
    AddInteger(out, "LastPlayTime", 0);
    AddText(out, "FlatpakAppID", "");
    AddField(out, 0, "tags");
    out.push_back('\b');
    out.push_back('\b');
    return out;
}

bool ReadFile(const fs::path& path, std::string& data) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFileAtomically(const fs::path& path, const std::string& data) {
    const fs::path temporary = path.wstring() + L".anyps4.tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!file) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string Quoted(const std::wstring& text) {
    return "\"" + Utf8(text) + "\"";
}

bool Matches(const Shortcut& shortcut) {
    return shortcut.name == kTitle && shortcut.options == kLaunchOptions;
}

struct Role {
    const wchar_t* suffix;
    std::vector<std::wstring> names;
};

const std::vector<Role>& Roles() {
    static const std::vector<Role> roles = {
        {L"p", {L"vertical", L"grid_vertical", L"capsule", L"poster"}},
        {L"", {L"horizontal", L"grid_horizontal", L"header", L"banner"}},
        {L"_hero", {L"hero"}},
        {L"_logo", {L"logo"}},
    };
    return roles;
}

const std::array<const wchar_t*, 7> kExtensions = {L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff"};

fs::path FindUserArt(const std::vector<fs::path>& directories, const Role& role) {
    std::error_code error;
    for (const fs::path& directory : directories) {
        for (const std::wstring& name : role.names) {
            for (const wchar_t* extension : kExtensions) {
                const fs::path candidate = directory / (name + extension);
                if (fs::is_regular_file(candidate, error)) return candidate;
            }
        }
    }
    return {};
}

bool PngEncoder(CLSID& clsid) {
    UINT count = 0;
    UINT size = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0) return false;
    std::vector<unsigned char> buffer(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, size, encoders) != Gdiplus::Ok) return false;
    for (UINT k = 0; k < count; ++k) {
        if (wcscmp(encoders[k].MimeType, L"image/png") == 0) {
            clsid = encoders[k].Clsid;
            return true;
        }
    }
    return false;
}

bool SavePng(Gdiplus::Image& image, const fs::path& path) {
    CLSID clsid{};
    if (!PngEncoder(clsid)) return false;
    return image.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

std::unique_ptr<Gdiplus::Bitmap> Crop(Gdiplus::Bitmap& source, double x, double y, double width, double height, int outWidth, int outHeight) {
    auto out = std::make_unique<Gdiplus::Bitmap>(outWidth, outHeight, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(out.get());
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.DrawImage(&source, Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(outWidth), static_cast<float>(outHeight)), static_cast<float>(x), static_cast<float>(y),
                       static_cast<float>(width), static_cast<float>(height), Gdiplus::UnitPixel);
    return out;
}

void LightToAlpha(Gdiplus::Bitmap& bitmap) {
    Gdiplus::BitmapData data;
    const Gdiplus::Rect area(0, 0, static_cast<INT>(bitmap.GetWidth()), static_cast<INT>(bitmap.GetHeight()));
    if (bitmap.LockBits(&area, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) != Gdiplus::Ok) return;
    for (UINT row = 0; row < data.Height; ++row) {
        unsigned char* pixel = static_cast<unsigned char*>(data.Scan0) + static_cast<std::ptrdiff_t>(row) * data.Stride;
        for (UINT column = 0; column < data.Width; ++column, pixel += 4) {
            const unsigned char alpha = std::max({pixel[0], pixel[1], pixel[2]});
            for (int channel = 0; channel < 3; ++channel) pixel[channel] = alpha == 0 ? 0 : static_cast<unsigned char>(std::min(255, pixel[channel] * 255 / alpha));
            pixel[3] = alpha;
        }
    }
    bitmap.UnlockBits(&data);
}

void RemoveArtwork(const fs::path& grid, std::uint32_t appId, const Role& role) {
    std::error_code error;
    for (const wchar_t* extension : {L".png", L".jpg"}) fs::remove(grid / (std::to_wstring(appId) + role.suffix + extension), error);
}

bool InstallUserArt(const fs::path& source, const fs::path& grid, std::uint32_t appId, const Role& role) {
    std::error_code error;
    std::wstring extension = source.extension().wstring();
    for (wchar_t& c : extension) c = static_cast<wchar_t>(towlower(c));
    RemoveArtwork(grid, appId, role);
    const std::wstring stem = std::to_wstring(appId) + role.suffix;
    if (extension == L".png" || extension == L".jpg" || extension == L".jpeg") {
        const fs::path target = grid / (stem + (extension == L".png" ? L".png" : L".jpg"));
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, error);
        return !error;
    }
    Gdiplus::Bitmap bitmap(source.c_str());
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;
    return SavePng(bitmap, grid / (stem + L".png"));
}

void PlaceArtwork(const std::vector<fs::path>& artDirectories, const fs::path& gameDirectory, const fs::path& grid, std::uint32_t appId, LineSink& sink) {
    std::error_code error;
    fs::create_directories(grid, error);
    std::unique_ptr<Gdiplus::Bitmap> key;
    const fs::path keyPath = gameDirectory / L"sce_sys" / L"pic1.png";
    if (fs::is_regular_file(keyPath, error)) {
        key = std::make_unique<Gdiplus::Bitmap>(keyPath.c_str());
        if (key->GetLastStatus() != Gdiplus::Ok || key->GetWidth() < 640 || key->GetHeight() < 360) key.reset();
    }
    const auto generate = [&](std::size_t index) -> std::unique_ptr<Gdiplus::Bitmap> {
        if (!key) return nullptr;
        const double fx = key->GetWidth() / 1920.0;
        const double fy = key->GetHeight() / 1080.0;
        switch (index) {
        case 0:
            return Crop(*key, 1100 * fx, 0, 720 * fx, 1080 * fy, 600, 900);
        case 1:
            return Crop(*key, 0, 20 * fy, 1920 * fx, 898 * fy, 920, 430);
        case 2:
            return Crop(*key, 217 * fx, 0, 1703 * fx, 550 * fy, 1920, 620);
        default: {
            auto logo = Crop(*key, 130 * fx, 550 * fy, 650 * fx, 370 * fy, 650, 370);
            LightToAlpha(*logo);
            return logo;
        }
        }
    };
    static const wchar_t* const captions[] = {L"vertical capsule", L"horizontal header", L"hero banner", L"logo"};
    for (std::size_t index = 0; index < Roles().size(); ++index) {
        const Role& role = Roles()[index];
        const fs::path user = FindUserArt(artDirectories, role);
        if (!user.empty() && InstallUserArt(user, grid, appId, role)) {
            sink(std::wstring(L"  ") + captions[index] + L": " + user.wstring());
            continue;
        }
        if (auto image = generate(index)) {
            RemoveArtwork(grid, appId, role);
            if (SavePng(*image, grid / (std::to_wstring(appId) + role.suffix + L".png"))) {
                sink(std::wstring(L"  ") + captions[index] + L": generated from sce_sys\\pic1.png");
                continue;
            }
        }
        sink(std::wstring(L"  ") + captions[index] + L": no image found (add one to a steam_art folder)");
    }
}

}  // namespace

std::uint32_t ShortcutAppId(const std::string& quotedExecutable, const std::string& name) {
    return Crc32(quotedExecutable + name) | 0x80000000u;
}

bool IsOnSteam(const std::wstring& launcherPath) {
    SteamUser user;
    std::wstring problem;
    if (!FindSteamUser(user, problem)) return false;
    std::string data;
    if (!ReadFile(user.directory / L"config" / L"shortcuts.vdf", data)) return false;
    Shortcuts parsed;
    if (!ParseShortcuts(data, parsed)) return false;
    const std::string executable = Quoted(launcherPath);
    return std::any_of(parsed.entries.begin(), parsed.entries.end(), [&](const Shortcut& s) { return Matches(s) && s.executable == executable; });
}

SteamResult AddToSteam(const std::wstring& launcherPath, const std::wstring& installDirectory, const std::wstring& gameDirectory, LineSink sink) {
    SteamResult result;
    SteamUser user;
    std::wstring problem;
    if (!FindSteamUser(user, problem)) {
        result.message = problem;
        return result;
    }
    if (!user.overridden && SteamRunning()) {
        result.message = L"Steam is running. Close it completely (Steam menu > Exit) and press Add to Steam again: Steam rewrites its shortcuts when it closes.";
        return result;
    }
    std::error_code error;
    const fs::path config = user.directory / L"config";
    fs::create_directories(config, error);
    const fs::path vdf = config / L"shortcuts.vdf";
    std::string data;
    const bool existed = ReadFile(vdf, data);
    if (!existed || data.empty()) data = std::string("\0shortcuts\0\b\b", 13);
    Shortcuts parsed;
    if (!ParseShortcuts(data, parsed)) {
        result.message = L"shortcuts.vdf has an unexpected layout; it was left untouched: " + vdf.wstring();
        return result;
    }

    const std::string executable = Quoted(launcherPath);
    const std::string startDirectory = Quoted(fs::path(launcherPath).parent_path().wstring());
    const std::string icon = Utf8(launcherPath);
    const auto found = std::find_if(parsed.entries.begin(), parsed.entries.end(), [](const Shortcut& s) { return Matches(s); });
    if (found != parsed.entries.end() && found->executable == executable && found->hasAppId) {
        result.appId = found->appId;
        result.message = L"Already on Steam.";
    } else {
        result.appId = ShortcutAppId(executable, kTitle);
        int index = 0;
        for (const Shortcut& s : parsed.entries) index = std::max(index, s.index + 1);
        if (found != parsed.entries.end()) index = found->index;
        const std::string entry = BuildEntry(index, result.appId, kTitle, executable, startDirectory, icon);
        if (found != parsed.entries.end()) data.replace(found->begin, found->end - found->begin, entry);
        else data.insert(parsed.insertAt, entry);
        if (existed) {
            const fs::path backup = vdf.wstring() + L".anyps4.bak";
            if (!fs::exists(backup, error)) fs::copy_file(vdf, backup, error);
        }
        if (!WriteFileAtomically(vdf, data)) {
            result.message = L"Could not write " + vdf.wstring();
            return result;
        }
        result.changed = true;
        result.message = found != parsed.entries.end() ? L"Steam shortcut updated." : L"Added to Steam.";
    }

    std::vector<fs::path> artDirectories;
    for (const fs::path& base : {fs::path(installDirectory), fs::path(launcherPath).parent_path()}) {
        if (!base.empty()) artDirectories.push_back(base / L"steam_art");
    }
    sink(L"Steam artwork for app " + std::to_wstring(result.appId) + L" in " + (user.directory / L"config" / L"grid").wstring() + L":");
    PlaceArtwork(artDirectories, fs::path(gameDirectory), user.directory / L"config" / L"grid", result.appId, sink);
    result.ok = true;
    result.message += L" Restart Steam to see \"" + std::wstring(kTitle, kTitle + std::strlen(kTitle)) + L"\" in your library.";
    return result;
}

}  // namespace launcher
