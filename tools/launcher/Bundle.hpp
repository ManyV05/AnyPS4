#pragma once

#include <string>

#include "Process.hpp"

namespace launcher {

bool BundleAvailable(std::wstring& bundleDirectory);

std::wstring TitleExecutablePath(const std::wstring& gameDirectory, const std::wstring& installDirectory);

bool PrepareOutputFile(const std::wstring& path, LineSink sink);

int InstallFromBundle(const std::wstring& bundleDirectory, const std::wstring& gameDirectory, const std::wstring& installDirectory, LineSink sink);

std::wstring RebaseImage(const std::wstring& path, unsigned long long newBase, unsigned long long& oldBase, unsigned long long& relocated);

}  // namespace launcher
