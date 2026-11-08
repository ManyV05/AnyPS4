#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

std::uint32_t Crc32Of(const unsigned char* data, std::size_t size, std::uint32_t crc = 0);

std::vector<unsigned char> Deflate(const unsigned char* data, std::size_t size);

bool Inflate(const unsigned char* data, std::size_t size, std::size_t expected, std::vector<unsigned char>& out);

bool CreateZip(const std::filesystem::path& zipFile, const std::filesystem::path& root, const std::string& prefix, int& fileCount, std::wstring& error);

bool ExtractZip(const std::filesystem::path& zipFile, const std::filesystem::path& destination, const std::string& stripPrefix, int& fileCount, std::wstring& error);

}  // namespace launcher
