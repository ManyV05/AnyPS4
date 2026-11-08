#pragma once

#include <string>

#include "Process.hpp"

namespace launcher {

bool BundleAvailable(std::wstring& bundleDirectory);

int InstallFromBundle(const std::wstring& bundleDirectory, const std::wstring& gameDirectory, const std::wstring& installDirectory, LineSink sink);

std::wstring RebaseImage(const std::wstring& path, unsigned long long newBase, unsigned long long& oldBase, unsigned long long& relocated);

}  // namespace launcher
