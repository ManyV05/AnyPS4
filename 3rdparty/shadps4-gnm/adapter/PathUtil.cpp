// AnyPS4 adapter: Common::FS paths relative to the executable (shader cache, dumps).
#include <windows.h>
#include <filesystem>
#include <string>
#include "common/path_util.h"

namespace Common::FS {

namespace {

std::filesystem::path ExecutableDirectory() {
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path Subdirectory(PathType type) {
    const auto base = ExecutableDirectory() / "anyps4";
    switch (type) {
    case PathType::ShaderDir:
        return base / "shaders";
    case PathType::CacheDir:
        return base / "cache";
    case PathType::LogDir:
        return base / "log";
    case PathType::ScreenshotsDir:
        return base / "screenshots";
    case PathType::CapturesDir:
        return base / "captures";
    default:
        return base;
    }
}

} // namespace

const std::filesystem::path& GetUserPath(PathType type) {
    static std::filesystem::path paths[64];
    auto& path = paths[static_cast<int>(type) & 63];
    if (path.empty()) {
        path = Subdirectory(type);
        std::error_code error;
        std::filesystem::create_directories(path, error);
    }
    return path;
}

std::string GetUserPathString(PathType type) {
    return GetUserPath(type).string();
}

void SetUserPath(PathType, const std::filesystem::path&) {}

std::string PathToUTF8String(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string{u8.begin(), u8.end()};
}

} // namespace Common::FS
