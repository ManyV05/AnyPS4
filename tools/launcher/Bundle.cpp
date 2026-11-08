#include "Bundle.hpp"

#include <windows.h>

#include "LiveAudio.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace launcher {

namespace {

namespace fs = std::filesystem;

constexpr unsigned long long kImageBase = 0x400000;
constexpr std::uint16_t kDynamicBase = 0x0040;
constexpr std::uint16_t kHighEntropyVa = 0x0020;

template <typename T>
bool Read(const std::vector<std::uint8_t>& data, std::size_t offset, T& value) {
    if (offset + sizeof(T) > data.size()) return false;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return true;
}

template <typename T>
bool Write(std::vector<std::uint8_t>& data, std::size_t offset, T value) {
    if (offset + sizeof(T) > data.size()) return false;
    std::memcpy(data.data() + offset, &value, sizeof(T));
    return true;
}

struct Section {
    std::uint32_t virtualAddress;
    std::uint32_t rawPointer;
    std::uint32_t rawSize;
};

fs::path ExecutableDirectory() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return fs::path(std::wstring(buffer, length)).parent_path();
}

}  // namespace

bool BundleAvailable(std::wstring& bundleDirectory) {
    const fs::path directory = ExecutableDirectory();
    std::error_code error;
    if (!fs::is_regular_file(directory / L"relinker.exe", error)) return false;
    if (!fs::is_regular_file(directory / L"libs" / L"libkernel.prx", error)) return false;
    bundleDirectory = directory.wstring();
    return true;
}

std::wstring TitleExecutablePath(const std::wstring& gameDirectory, const std::wstring& installDirectory) {
    std::error_code error;
    const fs::path game = fs::weakly_canonical(gameDirectory, error);
    std::wstring name = game.filename().wstring();
    if (name.empty()) name = game.parent_path().filename().wstring();
    return (fs::weakly_canonical(installDirectory, error) / (name + L".exe")).wstring();
}

bool PrepareOutputFile(const std::wstring& path, LineSink sink) {
    std::error_code ec;
    const fs::path file = path;
    const std::wstring leftover = path + L".old";
    fs::remove(leftover, ec);
    if (!fs::exists(file, ec)) return true;
    const std::wstring name = file.filename().wstring();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(path.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
        sink(L"  " + name + L" was read-only; the attribute was removed.");
    }
    DWORD error = ERROR_SUCCESS;
    const auto writable = [&]() {
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            return false;
        }
        CloseHandle(handle);
        return true;
    };
    if (writable()) return true;
    const unsigned long running = FindRunningTitle(path);
    if (running != 0) {
        sink(L"  " + name + L" is in use by the game (process " + std::to_wstring(running) + L"). Close it; waiting up to 30 seconds for it to exit.");
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, running);
        if (process != nullptr) {
            WaitForSingleObject(process, 30000);
            CloseHandle(process);
        }
        if (FindRunningTitle(path) != 0) {
            sink(L"error: " + name + L" is still running. Close the game (it can take about 15 seconds to finish closing) and press Install again.");
            return false;
        }
    }
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (writable()) return true;
        Sleep(500);
    }
    if (MoveFileExW(path.c_str(), leftover.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        sink(L"  " + name + L" could not be overwritten, so it was renamed to " + fs::path(leftover).filename().wstring() + L"; it is removed at the next Install.");
        return true;
    }
    const bool sharing = error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
    sink(L"error: cannot overwrite " + path + L" (Windows error " + std::to_wstring(static_cast<unsigned long>(error)) + L"). " +
         (sharing ? L"Another program has it open: close the game and anything that is using the install folder, then press Install again."
                  : L"Access was denied: check that the folder is not protected (Windows controlled folder access, antivirus) and that you may write there."));
    return false;
}

std::wstring RebaseImage(const std::wstring& path, unsigned long long newBase, unsigned long long& oldBase, unsigned long long& relocated) {
    std::vector<std::uint8_t> data;
    {
        std::ifstream file(fs::path(path), std::ios::binary);
        if (!file) return L"cannot open " + path;
        data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    std::uint16_t magic = 0;
    std::uint32_t peOffset = 0;
    if (!Read(data, 0, magic) || magic != 0x5A4D || !Read(data, 0x3C, peOffset)) return L"not a PE file";
    std::uint32_t signature = 0;
    if (!Read(data, peOffset, signature) || signature != 0x00004550) return L"bad PE signature";
    std::uint16_t sectionCount = 0;
    std::uint16_t optionalSize = 0;
    if (!Read(data, peOffset + 6, sectionCount) || !Read(data, peOffset + 20, optionalSize)) return L"truncated PE header";
    const std::size_t optional = peOffset + 24;
    std::uint16_t optionalMagic = 0;
    if (!Read(data, optional, optionalMagic) || optionalMagic != 0x20B) return L"not a PE32+ image";
    std::uint64_t previousBase = 0;
    std::uint16_t characteristics = 0;
    if (!Read(data, optional + 24, previousBase) || !Read(data, optional + 70, characteristics)) return L"truncated optional header";
    if (newBase == 0 || newBase % 0x10000 != 0) return L"the image base must be a non-zero multiple of 64 KB";

    std::vector<Section> sections;
    std::size_t sectionOffset = optional + optionalSize;
    for (std::uint16_t k = 0; k < sectionCount; ++k, sectionOffset += 40) {
        Section section{};
        if (!Read(data, sectionOffset + 12, section.virtualAddress) || !Read(data, sectionOffset + 16, section.rawSize) || !Read(data, sectionOffset + 20, section.rawPointer)) {
            return L"truncated section table";
        }
        sections.push_back(section);
    }
    const auto fileOffset = [&](std::uint32_t rva, std::size_t& offset) {
        for (const Section& section : sections) {
            if (rva >= section.virtualAddress && rva < section.virtualAddress + section.rawSize) {
                offset = section.rawPointer + (rva - section.virtualAddress);
                return true;
            }
        }
        return false;
    };

    const std::uint64_t delta = newBase - previousBase;
    relocated = 0;
    std::uint32_t relocRva = 0;
    std::uint32_t relocSize = 0;
    if (!Read(data, optional + 112 + 5 * 8, relocRva) || !Read(data, optional + 112 + 5 * 8 + 4, relocSize)) return L"truncated data directories";
    if (delta != 0 && relocRva != 0 && relocSize != 0) {
        std::size_t block = 0;
        if (!fileOffset(relocRva, block)) return L".reloc directory outside the file";
        const std::size_t end = block + relocSize;
        while (block + 8 <= end) {
            std::uint32_t page = 0;
            std::uint32_t size = 0;
            if (!Read(data, block, page) || !Read(data, block + 4, size) || size < 8) break;
            for (std::size_t entryOffset = block + 8; entryOffset + 2 <= block + size; entryOffset += 2) {
                std::uint16_t entry = 0;
                if (!Read(data, entryOffset, entry)) return L"truncated relocation block";
                const unsigned kind = entry >> 12;
                const std::uint32_t rva = page + (entry & 0xFFF);
                if (kind == 0) continue;
                std::size_t target = 0;
                if (!fileOffset(rva, target)) return L"relocation target outside the file";
                if (kind == 10) {
                    std::uint64_t value = 0;
                    if (!Read(data, target, value) || !Write(data, target, value + delta)) return L"relocation target past the end of the file";
                } else if (kind == 3) {
                    std::uint32_t value = 0;
                    if (!Read(data, target, value) || !Write(data, target, static_cast<std::uint32_t>(value + delta))) return L"relocation target past the end of the file";
                } else {
                    return L"unsupported relocation type " + std::to_wstring(kind);
                }
                ++relocated;
            }
            block += size;
        }
    }
    Write<std::uint64_t>(data, optional + 24, newBase);
    characteristics = static_cast<std::uint16_t>(characteristics & ~(kDynamicBase | kHighEntropyVa));
    Write<std::uint16_t>(data, optional + 70, characteristics);
    Write<std::uint32_t>(data, optional + 64, 0);
    oldBase = previousBase;

    std::ofstream file(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!file) return L"cannot write " + path;
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) return L"short write to " + path;
    return {};
}

int InstallFromBundle(const std::wstring& bundleDirectory, const std::wstring& gameDirectory, const std::wstring& installDirectory, LineSink sink) {
    std::error_code error;
    const fs::path bundle = bundleDirectory;
    const fs::path game = fs::weakly_canonical(gameDirectory, error);
    const fs::path out = fs::weakly_canonical(installDirectory, error);
    const fs::path eboot = game / L"eboot.bin";
    std::wstring name = game.filename().wstring();
    if (name.empty()) name = game.parent_path().filename().wstring();
    if (name.empty()) {
        sink(L"error: cannot derive a name from the game folder " + game.wstring());
        return 2;
    }
    const fs::path executable = out / (name + L".exe");
    fs::create_directories(out, error);
    if (!PrepareOutputFile(executable.wstring(), sink)) return 8;

    sink(L"== 1/4 relinking " + eboot.wstring() + L" -> " + executable.wstring());
    fs::create_directories(out, error);
    int code = 0;
    const std::wstring command = Quote((bundle / L"relinker.exe").wstring()) + L" --windows --registry " + Quote(eboot.wstring()) + L" " + Quote(executable.wstring());
    if (!RunCaptured(command, game.parent_path().wstring(), [&](const std::wstring& line) { sink(L"  " + line); }, code)) return 3;
    if (code != 0) {
        sink(L"error: the relinker exited with " + std::to_wstring(code));
        return code;
    }
    if (!fs::is_regular_file(out / (name + L".registry.json"), error)) {
        sink(L"error: " + (out / (name + L".registry.json")).wstring() + L" was not written");
        return 4;
    }

    sink(L"== 2/4 fixing the image base");
    unsigned long long oldBase = 0;
    unsigned long long relocated = 0;
    const std::wstring problem = RebaseImage(executable.wstring(), kImageBase, oldBase, relocated);
    if (!problem.empty()) {
        sink(L"error: " + problem);
        return 5;
    }
    wchar_t text[160];
    swprintf(text, 160, L"  image base 0x%llx -> 0x%llx, %llu pointers relocated, ASLR off", oldBase, kImageBase, relocated);
    sink(text);

    sink(L"== 3/4 copying the prebuilt libraries");
    const fs::path libs = out / L"libs";
    fs::create_directories(libs, error);
    int copied = 0;
    for (const auto& entry : fs::directory_iterator(bundle / L"libs", error)) {
        const fs::path extension = entry.path().extension();
        if (!entry.is_regular_file(error) || (extension != L".prx" && extension != L".dll")) continue;
        for (int attempt = 0; attempt < 40; ++attempt) {
            fs::copy_file(entry.path(), libs / entry.path().filename(), fs::copy_options::overwrite_existing, error);
            if (!error || (error != std::errc::permission_denied && error != std::errc::device_or_resource_busy && error != std::errc::text_file_busy)) break;
            if (attempt == 0) sink(L"  " + entry.path().filename().wstring() + L" is in use; waiting for the game to finish closing.");
            Sleep(500);
        }
        if (error) {
            sink(L"error: cannot copy " + entry.path().filename().wstring() + L": " + std::wstring(error.message().begin(), error.message().end()) +
                 L" (is the game still running?)");
            return 6;
        }
        ++copied;
    }
    sink(L"  " + std::to_wstring(copied) + L" libraries copied to " + libs.wstring());

    sink(L"== 4/4 laying out the install folder");
    const fs::path app0 = out / L"app0";
    if (!fs::exists(app0, error)) {
        code = 0;
        RunCaptured(L"cmd.exe /c mklink /J " + Quote(app0.wstring()) + L" " + Quote(game.wstring()), out.wstring(), [&](const std::wstring& line) { sink(L"  " + line); }, code);
        if (code != 0) {
            sink(L"error: could not create the app0 junction");
            return 7;
        }
    }
    sink(L"  app0 -> " + game.wstring());
    fs::create_directories(out / L"anyps4", error);
    fs::create_directories(out / L"savedata", error);
    sink(L"done: " + executable.wstring());
    return 0;
}

}  // namespace launcher
