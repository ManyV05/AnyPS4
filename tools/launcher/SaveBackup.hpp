#pragma once

#include <string>

namespace launcher {

std::wstring DefaultBackupName();

bool ExportSaves(const std::wstring& installDirectory, const std::wstring& zipFile, int& fileCount, std::wstring& error);

bool ImportSaves(const std::wstring& installDirectory, const std::wstring& zipFile, int& fileCount, std::wstring& previousFolder, std::wstring& error);

}  // namespace launcher
