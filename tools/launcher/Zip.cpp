#include "Zip.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>

namespace launcher {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kWindow = 32768;
constexpr std::size_t kMaxMatch = 258;
constexpr std::size_t kMinMatch = 3;
constexpr int kMaxChain = 96;
constexpr std::uint64_t kMaxArchive = 1ull << 30;
constexpr std::uint64_t kMaxEntry = 1ull << 31;

const std::array<int, 29> kLengthBase = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::array<int, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::array<int, 30> kDistanceBase = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const std::array<int, 30> kDistanceExtra = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

class BitWriter {
public:
    explicit BitWriter(std::vector<unsigned char>& out) : out_(out) {}

    void Put(std::uint32_t value, int count) {
        accumulator_ |= static_cast<std::uint64_t>(value) << bits_;
        bits_ += count;
        while (bits_ >= 8) {
            out_.push_back(static_cast<unsigned char>(accumulator_ & 0xFF));
            accumulator_ >>= 8;
            bits_ -= 8;
        }
    }

    void PutCode(std::uint32_t code, int length) {
        std::uint32_t reversed = 0;
        for (int k = 0; k < length; ++k) reversed |= ((code >> k) & 1u) << (length - 1 - k);
        Put(reversed, length);
    }

    void Flush() {
        if (bits_ > 0) out_.push_back(static_cast<unsigned char>(accumulator_ & 0xFF));
        accumulator_ = 0;
        bits_ = 0;
    }

private:
    std::vector<unsigned char>& out_;
    std::uint64_t accumulator_ = 0;
    int bits_ = 0;
};

void PutLiteralOrLength(BitWriter& writer, int symbol) {
    if (symbol < 144) writer.PutCode(0x30 + symbol, 8);
    else if (symbol < 256) writer.PutCode(0x190 + (symbol - 144), 9);
    else if (symbol < 280) writer.PutCode(symbol - 256, 7);
    else writer.PutCode(0xC0 + (symbol - 280), 8);
}

void PutMatch(BitWriter& writer, int length, int distance) {
    int lengthCode = 28;
    while (kLengthBase[static_cast<std::size_t>(lengthCode)] > length) --lengthCode;
    PutLiteralOrLength(writer, 257 + lengthCode);
    if (kLengthExtra[static_cast<std::size_t>(lengthCode)] > 0) writer.Put(static_cast<std::uint32_t>(length - kLengthBase[static_cast<std::size_t>(lengthCode)]), kLengthExtra[static_cast<std::size_t>(lengthCode)]);
    int distanceCode = 29;
    while (kDistanceBase[static_cast<std::size_t>(distanceCode)] > distance) --distanceCode;
    writer.PutCode(static_cast<std::uint32_t>(distanceCode), 5);
    if (kDistanceExtra[static_cast<std::size_t>(distanceCode)] > 0) writer.Put(static_cast<std::uint32_t>(distance - kDistanceBase[static_cast<std::size_t>(distanceCode)]), kDistanceExtra[static_cast<std::size_t>(distanceCode)]);
}

class BitReader {
public:
    BitReader(const unsigned char* data, std::size_t size) : data_(data), size_(size) {}

    bool Bits(int count, std::uint32_t& value) {
        while (count_ < count) {
            if (position_ >= size_) return false;
            buffer_ |= static_cast<std::uint32_t>(data_[position_++]) << count_;
            count_ += 8;
        }
        value = buffer_ & ((1u << count) - 1u);
        buffer_ >>= count;
        count_ -= count;
        return true;
    }

    void AlignToByte() {
        buffer_ = 0;
        count_ = 0;
    }

    bool Copy(std::size_t count, std::vector<unsigned char>& out) {
        if (position_ + count > size_) return false;
        out.insert(out.end(), data_ + position_, data_ + position_ + count);
        position_ += count;
        return true;
    }

    bool ReadU16(std::uint32_t& value) {
        if (position_ + 2 > size_) return false;
        value = static_cast<std::uint32_t>(data_[position_]) | (static_cast<std::uint32_t>(data_[position_ + 1]) << 8);
        position_ += 2;
        return true;
    }

private:
    const unsigned char* data_;
    std::size_t size_;
    std::size_t position_ = 0;
    std::uint32_t buffer_ = 0;
    int count_ = 0;
};

struct Huffman {
    std::array<std::uint16_t, 16> count{};
    std::array<std::uint16_t, 320> symbol{};
};

bool BuildHuffman(Huffman& table, const std::uint16_t* lengths, int n) {
    table.count.fill(0);
    for (int k = 0; k < n; ++k) ++table.count[lengths[k]];
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= table.count[static_cast<std::size_t>(len)];
        if (left < 0) return false;
    }
    std::array<std::uint16_t, 16> offsets{};
    for (int len = 1; len < 15; ++len) offsets[static_cast<std::size_t>(len + 1)] = static_cast<std::uint16_t>(offsets[static_cast<std::size_t>(len)] + table.count[static_cast<std::size_t>(len)]);
    for (int k = 0; k < n; ++k) {
        if (lengths[k] != 0) table.symbol[offsets[lengths[k]]++] = static_cast<std::uint16_t>(k);
    }
    return true;
}

bool DecodeSymbol(BitReader& reader, const Huffman& table, int& symbol) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len < 16; ++len) {
        std::uint32_t bit = 0;
        if (!reader.Bits(1, bit)) return false;
        code |= static_cast<int>(bit);
        const int count = table.count[static_cast<std::size_t>(len)];
        if (code - count < first) {
            symbol = table.symbol[static_cast<std::size_t>(index + (code - first))];
            return true;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return false;
}

bool InflateCodes(BitReader& reader, const Huffman& literals, const Huffman& distances, std::size_t expected, std::vector<unsigned char>& out) {
    while (true) {
        int symbol = 0;
        if (!DecodeSymbol(reader, literals, symbol)) return false;
        if (symbol < 256) {
            if (out.size() >= expected) return false;
            out.push_back(static_cast<unsigned char>(symbol));
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            std::uint32_t extra = 0;
            if (!reader.Bits(kLengthExtra[static_cast<std::size_t>(symbol)], extra)) return false;
            const std::size_t length = static_cast<std::size_t>(kLengthBase[static_cast<std::size_t>(symbol)]) + extra;
            int distanceSymbol = 0;
            if (!DecodeSymbol(reader, distances, distanceSymbol) || distanceSymbol >= 30) return false;
            if (!reader.Bits(kDistanceExtra[static_cast<std::size_t>(distanceSymbol)], extra)) return false;
            const std::size_t distance = static_cast<std::size_t>(kDistanceBase[static_cast<std::size_t>(distanceSymbol)]) + extra;
            if (distance > out.size() || out.size() + length > expected) return false;
            for (std::size_t k = 0; k < length; ++k) out.push_back(out[out.size() - distance]);
        }
    }
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr, nullptr);
    return narrow;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

void Put16(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

void Put32(std::vector<unsigned char>& out, std::uint32_t value) {
    Put16(out, value & 0xFFFF);
    Put16(out, value >> 16);
}

std::uint32_t Get16(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
}

std::uint32_t Get32(const unsigned char* p) {
    return Get16(p) | (Get16(p + 2) << 16);
}

void DosTime(const fs::path& file, std::uint32_t& time, std::uint32_t& date) {
    time = 0;
    date = (1u << 5) | 1u;
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    FILETIME written{};
    FILETIME local{};
    WORD dosDate = 0;
    WORD dosTime = 0;
    if (GetFileTime(handle, nullptr, nullptr, &written) && FileTimeToLocalFileTime(&written, &local) && FileTimeToDosDateTime(&local, &dosDate, &dosTime)) {
        time = dosTime;
        date = dosDate;
    }
    CloseHandle(handle);
}

bool SafeRelativePath(const std::string& name, std::string& cleaned) {
    std::string path = name;
    std::replace(path.begin(), path.end(), '\\', '/');
    cleaned.clear();
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        const std::string part = path.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.find(':') != std::string::npos) return false;
        if (!cleaned.empty()) cleaned.push_back('/');
        cleaned += part;
    }
    return !cleaned.empty();
}

bool ReadWhole(const fs::path& path, std::vector<unsigned char>& data) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

std::uint32_t Crc32Of(const unsigned char* data, std::size_t size, std::uint32_t crc) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            values[n] = c;
        }
        return values;
    }();
    crc = ~crc;
    for (std::size_t k = 0; k < size; ++k) crc = table[(crc ^ data[k]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::vector<unsigned char> Deflate(const unsigned char* data, std::size_t size) {
    std::vector<unsigned char> out;
    out.reserve(size / 2 + 64);
    BitWriter writer(out);
    writer.Put(1, 1);
    writer.Put(1, 2);
    std::vector<std::int64_t> head(1 << 15, -1);
    std::vector<std::int64_t> previous(kWindow, -1);
    const auto hash = [&](std::size_t p) { return ((static_cast<std::uint32_t>(data[p]) << 10) ^ (static_cast<std::uint32_t>(data[p + 1]) << 5) ^ data[p + 2]) & 0x7FFF; };
    const auto insert = [&](std::size_t p) {
        if (p + 2 >= size) return;
        const std::uint32_t h = hash(p);
        previous[p & (kWindow - 1)] = head[h];
        head[h] = static_cast<std::int64_t>(p);
    };
    std::size_t position = 0;
    while (position < size) {
        std::size_t bestLength = 0;
        std::size_t bestDistance = 0;
        if (position + 2 < size) {
            std::int64_t candidate = head[hash(position)];
            const std::size_t limit = std::min(kMaxMatch, size - position);
            for (int chain = 0; candidate >= 0 && chain < kMaxChain; ++chain) {
                const std::size_t distance = position - static_cast<std::size_t>(candidate);
                if (distance == 0 || distance > kWindow) break;
                std::size_t length = 0;
                while (length < limit && data[static_cast<std::size_t>(candidate) + length] == data[position + length]) ++length;
                if (length > bestLength) {
                    bestLength = length;
                    bestDistance = distance;
                    if (length == limit) break;
                }
                const std::int64_t next = previous[static_cast<std::size_t>(candidate) & (kWindow - 1)];
                if (next >= candidate) break;
                candidate = next;
            }
        }
        if (bestLength >= kMinMatch) {
            PutMatch(writer, static_cast<int>(bestLength), static_cast<int>(bestDistance));
            for (std::size_t k = 0; k < bestLength; ++k) insert(position + k);
            position += bestLength;
        } else {
            PutLiteralOrLength(writer, data[position]);
            insert(position);
            ++position;
        }
    }
    PutLiteralOrLength(writer, 256);
    writer.Flush();
    return out;
}

bool Inflate(const unsigned char* data, std::size_t size, std::size_t expected, std::vector<unsigned char>& out) {
    out.clear();
    out.reserve(expected);
    BitReader reader(data, size);
    std::uint32_t last = 0;
    do {
        std::uint32_t type = 0;
        if (!reader.Bits(1, last) || !reader.Bits(2, type)) return false;
        if (type == 0) {
            reader.AlignToByte();
            std::uint32_t length = 0;
            std::uint32_t inverse = 0;
            if (!reader.ReadU16(length) || !reader.ReadU16(inverse) || (length ^ 0xFFFFu) != inverse) return false;
            if (out.size() + length > expected || !reader.Copy(length, out)) return false;
        } else if (type == 1) {
            std::array<std::uint16_t, 288> lengths{};
            for (int k = 0; k < 144; ++k) lengths[static_cast<std::size_t>(k)] = 8;
            for (int k = 144; k < 256; ++k) lengths[static_cast<std::size_t>(k)] = 9;
            for (int k = 256; k < 280; ++k) lengths[static_cast<std::size_t>(k)] = 7;
            for (int k = 280; k < 288; ++k) lengths[static_cast<std::size_t>(k)] = 8;
            Huffman literals;
            BuildHuffman(literals, lengths.data(), 288);
            std::array<std::uint16_t, 30> distanceLengths{};
            distanceLengths.fill(5);
            Huffman distances;
            BuildHuffman(distances, distanceLengths.data(), 30);
            if (!InflateCodes(reader, literals, distances, expected, out)) return false;
        } else if (type == 2) {
            std::uint32_t literalCount = 0;
            std::uint32_t distanceCount = 0;
            std::uint32_t codeCount = 0;
            if (!reader.Bits(5, literalCount) || !reader.Bits(5, distanceCount) || !reader.Bits(4, codeCount)) return false;
            literalCount += 257;
            distanceCount += 1;
            codeCount += 4;
            if (literalCount > 286 || distanceCount > 30) return false;
            static const std::array<int, 19> order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::array<std::uint16_t, 19> codeLengths{};
            for (std::uint32_t k = 0; k < codeCount; ++k) {
                std::uint32_t value = 0;
                if (!reader.Bits(3, value)) return false;
                codeLengths[static_cast<std::size_t>(order[k])] = static_cast<std::uint16_t>(value);
            }
            Huffman lengthCodes;
            if (!BuildHuffman(lengthCodes, codeLengths.data(), 19)) return false;
            std::array<std::uint16_t, 320> lengths{};
            std::uint32_t index = 0;
            while (index < literalCount + distanceCount) {
                int symbol = 0;
                if (!DecodeSymbol(reader, lengthCodes, symbol)) return false;
                if (symbol < 16) {
                    lengths[index++] = static_cast<std::uint16_t>(symbol);
                    continue;
                }
                std::uint16_t value = 0;
                std::uint32_t repeat = 0;
                if (symbol == 16) {
                    if (index == 0) return false;
                    value = lengths[index - 1];
                    if (!reader.Bits(2, repeat)) return false;
                    repeat += 3;
                } else if (symbol == 17) {
                    if (!reader.Bits(3, repeat)) return false;
                    repeat += 3;
                } else {
                    if (!reader.Bits(7, repeat)) return false;
                    repeat += 11;
                }
                if (index + repeat > literalCount + distanceCount) return false;
                while (repeat-- > 0) lengths[index++] = value;
            }
            if (lengths[256] == 0) return false;
            Huffman literals;
            Huffman distances;
            if (!BuildHuffman(literals, lengths.data(), static_cast<int>(literalCount)) || !BuildHuffman(distances, lengths.data() + literalCount, static_cast<int>(distanceCount))) return false;
            if (!InflateCodes(reader, literals, distances, expected, out)) return false;
        } else {
            return false;
        }
    } while (last == 0);
    return out.size() == expected;
}

bool CreateZip(const fs::path& zipFile, const fs::path& root, const std::string& prefix, int& fileCount, std::wstring& error) {
    fileCount = 0;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        error = L"The folder does not exist: " + root.wstring();
        return false;
    }
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        error = L"There are no files to back up in " + root.wstring();
        return false;
    }
    if (files.size() > 60000) {
        error = L"Too many files for a ZIP archive.";
        return false;
    }
    std::vector<unsigned char> archive;
    std::vector<unsigned char> directory;
    for (const fs::path& file : files) {
        std::vector<unsigned char> content;
        if (!ReadWhole(file, content)) {
            error = L"Cannot read " + file.wstring();
            return false;
        }
        std::string name = prefix + Utf8(file.lexically_relative(root).generic_wstring());
        std::vector<unsigned char> packed = Deflate(content.data(), content.size());
        std::uint32_t method = 8;
        if (packed.size() >= content.size()) {
            packed = content;
            method = 0;
        }
        const std::uint32_t crc = Crc32Of(content.data(), content.size());
        std::uint32_t time = 0;
        std::uint32_t date = 0;
        DosTime(file, time, date);
        const std::uint32_t offset = static_cast<std::uint32_t>(archive.size());
        Put32(archive, 0x04034b50);
        Put16(archive, 20);
        Put16(archive, 0x0800);
        Put16(archive, method);
        Put16(archive, time);
        Put16(archive, date);
        Put32(archive, crc);
        Put32(archive, static_cast<std::uint32_t>(packed.size()));
        Put32(archive, static_cast<std::uint32_t>(content.size()));
        Put16(archive, static_cast<std::uint32_t>(name.size()));
        Put16(archive, 0);
        archive.insert(archive.end(), name.begin(), name.end());
        archive.insert(archive.end(), packed.begin(), packed.end());
        Put32(directory, 0x02014b50);
        Put16(directory, 20);
        Put16(directory, 20);
        Put16(directory, 0x0800);
        Put16(directory, method);
        Put16(directory, time);
        Put16(directory, date);
        Put32(directory, crc);
        Put32(directory, static_cast<std::uint32_t>(packed.size()));
        Put32(directory, static_cast<std::uint32_t>(content.size()));
        Put16(directory, static_cast<std::uint32_t>(name.size()));
        Put16(directory, 0);
        Put16(directory, 0);
        Put16(directory, 0);
        Put16(directory, 0);
        Put32(directory, 0);
        Put32(directory, offset);
        directory.insert(directory.end(), name.begin(), name.end());
        if (archive.size() > 0xF0000000ull) {
            error = L"The backup would be larger than 3.75 GB.";
            return false;
        }
    }
    const std::uint32_t directoryOffset = static_cast<std::uint32_t>(archive.size());
    archive.insert(archive.end(), directory.begin(), directory.end());
    Put32(archive, 0x06054b50);
    Put16(archive, 0);
    Put16(archive, 0);
    Put16(archive, static_cast<std::uint32_t>(files.size()));
    Put16(archive, static_cast<std::uint32_t>(files.size()));
    Put32(archive, static_cast<std::uint32_t>(directory.size()));
    Put32(archive, directoryOffset);
    Put16(archive, 0);
    const fs::path temporary = zipFile.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = L"Cannot write " + temporary.wstring();
            return false;
        }
        output.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()));
        if (!output) {
            error = L"Short write to " + temporary.wstring();
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), zipFile.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"Cannot create " + zipFile.wstring();
        return false;
    }
    fileCount = static_cast<int>(files.size());
    return true;
}

bool ExtractZip(const fs::path& zipFile, const fs::path& destination, const std::string& stripPrefix, int& fileCount, std::wstring& error) {
    fileCount = 0;
    std::vector<unsigned char> archive;
    std::error_code ec;
    if (!fs::is_regular_file(zipFile, ec) || fs::file_size(zipFile, ec) > kMaxArchive || !ReadWhole(zipFile, archive)) {
        error = L"Cannot read the ZIP file " + zipFile.wstring();
        return false;
    }
    std::size_t endOfDirectory = std::string::npos;
    if (archive.size() >= 22) {
        const std::size_t lowest = archive.size() > 22 + 65535 ? archive.size() - 22 - 65535 : 0;
        for (std::size_t p = archive.size() - 22 + 1; p-- > lowest;) {
            if (Get32(&archive[p]) == 0x06054b50) {
                endOfDirectory = p;
                break;
            }
        }
    }
    if (endOfDirectory == std::string::npos) {
        error = L"This is not a ZIP file.";
        return false;
    }
    const std::uint32_t entries = Get16(&archive[endOfDirectory + 10]);
    std::uint64_t cursor = Get32(&archive[endOfDirectory + 16]);
    for (std::uint32_t entry = 0; entry < entries; ++entry) {
        if (cursor + 46 > archive.size() || Get32(&archive[cursor]) != 0x02014b50) {
            error = L"The ZIP directory is damaged.";
            return false;
        }
        const unsigned char* header = &archive[cursor];
        const std::uint32_t flags = Get16(header + 8);
        const std::uint32_t method = Get16(header + 10);
        const std::uint32_t crc = Get32(header + 16);
        const std::uint64_t packedSize = Get32(header + 20);
        const std::uint64_t size = Get32(header + 24);
        const std::uint32_t nameLength = Get16(header + 28);
        const std::uint32_t extraLength = Get16(header + 30);
        const std::uint32_t commentLength = Get16(header + 32);
        const std::uint64_t localOffset = Get32(header + 42);
        if (cursor + 46 + nameLength + extraLength + commentLength > archive.size()) {
            error = L"The ZIP directory is damaged.";
            return false;
        }
        const std::string rawName(reinterpret_cast<const char*>(header + 46), nameLength);
        cursor += 46ull + nameLength + extraLength + commentLength;
        if (!rawName.empty() && rawName.back() == '/') continue;
        std::string name;
        if (!SafeRelativePath(rawName, name)) {
            error = L"The ZIP holds an unsafe path: " + FromUtf8(rawName);
            return false;
        }
        if (!stripPrefix.empty() && name.compare(0, stripPrefix.size(), stripPrefix) == 0) name.erase(0, stripPrefix.size());
        if (name.empty()) continue;
        if ((flags & 1u) != 0) {
            error = L"The ZIP is encrypted.";
            return false;
        }
        if (size > kMaxEntry || localOffset + 30 > archive.size() || Get32(&archive[localOffset]) != 0x04034b50) {
            error = L"The ZIP entry is damaged: " + FromUtf8(name);
            return false;
        }
        const std::uint64_t dataStart = localOffset + 30 + Get16(&archive[localOffset + 26]) + Get16(&archive[localOffset + 28]);
        if (dataStart + packedSize > archive.size()) {
            error = L"The ZIP entry is truncated: " + FromUtf8(name);
            return false;
        }
        std::vector<unsigned char> content;
        if (method == 0) {
            if (packedSize != size) {
                error = L"The ZIP entry is damaged: " + FromUtf8(name);
                return false;
            }
            content.assign(archive.begin() + static_cast<std::ptrdiff_t>(dataStart), archive.begin() + static_cast<std::ptrdiff_t>(dataStart + packedSize));
        } else if (method == 8) {
            if (!Inflate(&archive[dataStart], static_cast<std::size_t>(packedSize), static_cast<std::size_t>(size), content)) {
                error = L"Cannot decompress " + FromUtf8(name);
                return false;
            }
        } else {
            error = L"Unsupported compression method in " + FromUtf8(name);
            return false;
        }
        if (Crc32Of(content.data(), content.size()) != crc) {
            error = L"Checksum mismatch in " + FromUtf8(name);
            return false;
        }
        const fs::path target = destination / fs::path(FromUtf8(name));
        fs::create_directories(target.parent_path(), ec);
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = L"Cannot write " + target.wstring();
            return false;
        }
        output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        if (!output) {
            error = L"Short write to " + target.wstring();
            return false;
        }
        ++fileCount;
    }
    return true;
}

}  // namespace launcher
