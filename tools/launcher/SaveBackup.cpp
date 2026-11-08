#include "SaveBackup.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>

#include "Zip.hpp"

namespace launcher {

namespace {

namespace fs = std::filesystem;

constexpr const char* kPrefix = "savedata/";

std::wstring Stamp(bool withTime) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t text[32];
    if (withTime) swprintf(text, 32, L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    else swprintf(text, 32, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
    return text;
}

}  // namespace

std::wstring DefaultBackupName() {
    return L"AnyPS4_SaveBackup_" + Stamp(false) + L".zip";
}

bool ExportSaves(const std::wstring& installDirectory, const std::wstring& zipFile, int& fileCount, std::wstring& error) {
    fileCount = 0;
    if (installDirectory.empty()) {
        error = L"Choose the install folder first.";
        return false;
    }
    return CreateZip(zipFile, fs::path(installDirectory) / L"savedata", kPrefix, fileCount, error);
}

bool ImportSaves(const std::wstring& installDirectory, const std::wstring& zipFile, int& fileCount, std::wstring& previousFolder, std::wstring& error) {
    fileCount = 0;
    previousFolder.clear();
    if (installDirectory.empty()) {
        error = L"Choose the install folder first.";
        return false;
    }
    const fs::path install = installDirectory;
    const fs::path current = install / L"savedata";
    const fs::path staging = install / L"savedata.import";
    std::error_code ec;
    fs::remove_all(staging, ec);
    if (!ExtractZip(zipFile, staging, kPrefix, fileCount, error)) {
        fs::remove_all(staging, ec);
        return false;
    }
    if (fileCount == 0) {
        fs::remove_all(staging, ec);
        error = L"The ZIP holds no save files.";
        return false;
    }
    fs::path kept;
    if (fs::exists(current, ec)) {
        kept = install / (L"savedata_backup_" + Stamp(true));
        fs::rename(current, kept, ec);
        if (ec) {
            fs::remove_all(staging, ec);
            error = L"Cannot move the current saves aside (is the game running?): " + current.wstring();
            return false;
        }
    }
    fs::rename(staging, current, ec);
    if (ec) {
        if (!kept.empty()) fs::rename(kept, current, ec);
        error = L"Cannot put the imported saves in place.";
        return false;
    }
    previousFolder = kept.wstring();
    return true;
}

}  // namespace launcher
