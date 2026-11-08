#include <io/FileWriter.hpp>
#include <io/NativePath.hpp>
#include <domain/Types.hpp>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Io {

namespace {

constexpr int kSharingRetries = 10;
constexpr unsigned long kSharingRetryMilliseconds = 300;

bool OpenOutput(std::ofstream& file, const std::string& path, std::ios::openmode mode, std::string& reason) {
    const std::filesystem::path native = NativePath(path);
    file.open(native, mode);
    if (file)
        return true;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0 &&
        SetFileAttributesW(native.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY))) {
        file.clear();
        file.open(native, mode);
        if (file)
            return true;
    }
    DWORD error = ERROR_SUCCESS;
    for (int attempt = 0; attempt <= kSharingRetries; ++attempt) {
        HANDLE probe = CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        error = probe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (probe != INVALID_HANDLE_VALUE)
            CloseHandle(probe);
        if ((error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) || attempt == kSharingRetries)
            break;
        Sleep(kSharingRetryMilliseconds);
        file.clear();
        file.open(native, mode);
        if (file)
            return true;
    }
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION)
        reason = " (the file is in use by another process: close the program or the game that is running it and try again)";
    else if (error == ERROR_ACCESS_DENIED)
        reason = " (access denied: the file or its folder is protected, or a security program is blocking it)";
    else if (error == ERROR_PATH_NOT_FOUND)
        reason = " (the folder does not exist)";
    else if (error != ERROR_SUCCESS)
        reason = " (Windows error " + std::to_string(static_cast<unsigned long>(error)) + ")";
#else
    (void)reason;
#endif
    return false;
}

}

void FileWriter::Write(const std::string& path, const std::vector<std::uint8_t>& data) {
    std::ofstream f;
    std::string reason;
    if (!OpenOutput(f, path, std::ios::binary, reason))
        throw Domain::RelinkerException("Cannot open output file: " + path + reason);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    f.close();
    if (!f)
        throw Domain::RelinkerException("Failed to write file: " + path);
}

void FileWriter::Write(const std::string& path, const std::string& content) {
    std::ofstream f;
    std::string reason;
    if (!OpenOutput(f, path, std::ios::out, reason))
        throw Domain::RelinkerException("Cannot open output file: " + path + reason);
    f << content;
    f.close();
    if (!f)
        throw Domain::RelinkerException("Failed to write file: " + path);
}

}
