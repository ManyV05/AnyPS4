#pragma once

#include <string>

#include "Process.hpp"

namespace launcher {

// A standalone package ships relinker.exe and a prebuilt libs\ folder next to the launcher. With both present, Install
// needs no Python, compiler, CMake or shadPS4 checkout: it relinks the game's eboot.bin, rebases the result, copies the
// prebuilt libraries and links app0 to the game folder.
bool BundleAvailable(std::wstring& bundleDirectory);

// Returns 0 on success; every step is reported line by line through the sink.
int InstallFromBundle(const std::wstring& bundleDirectory, const std::wstring& gameDirectory, const std::wstring& installDirectory, LineSink sink);

// Gives a relinked executable a fixed image base with its relocations applied and ASLR off (the same transformation as
// tools/anyps4_rebase.py). Returns an empty string on success, otherwise the reason.
std::wstring RebaseImage(const std::wstring& path, unsigned long long newBase, unsigned long long& oldBase, unsigned long long& relocated);

}  // namespace launcher
