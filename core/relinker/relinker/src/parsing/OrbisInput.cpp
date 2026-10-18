#include <relinker/parsing/OrbisInput.hpp>
#include <elfpatcher/general/ElfConstants.hpp>
#include <relinker/domain/Types.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace Relinker {

using namespace Elfpatcher;

namespace {

constexpr std::size_t kSelfHeaderSize = 0x20;
constexpr std::size_t kSelfSegmentHeaderSize = 0x20;
constexpr std::size_t kSelfSegmentCountOffset = 0x18;
constexpr std::size_t kElfHeaderSize = 0x40;
constexpr std::size_t kPhdrEntrySize = 56;
constexpr std::uint64_t kSelfSegmentBlocked = 0x800;
constexpr std::uint64_t kSelfSegmentEncrypted = 0x2;
constexpr std::uint64_t kSelfSegmentCompressed = 0x8;
constexpr std::uint64_t kMaxFlatImageSize = std::uint64_t{1} << 31;

bool rangeFits(const std::uint64_t offset, const std::uint64_t length, const std::size_t size) {
    return offset <= size && length <= size - offset;
}

template<typename TValue>
TValue readAt(const std::vector<std::uint8_t>& bytes, const std::uint64_t offset, const char* what) {
    if (!rangeFits(offset, sizeof(TValue), bytes.size()))
        throw RelinkerException(std::string(what) + " exceeds the input file", offset);
    TValue value;
    std::memcpy(&value, bytes.data() + offset, sizeof(TValue));
    return value;
}

template<typename TValue>
void writeAt(std::vector<std::uint8_t>& bytes, const std::uint64_t offset, const TValue value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(TValue));
}

struct SelfSegment {
    std::uint64_t Flags;
    std::uint64_t FileOffset;
    std::uint64_t FileSize;
    std::uint64_t MemorySize;

    bool IsBlocked() const { return (Flags & kSelfSegmentBlocked) != 0; }
    bool IsEncrypted() const { return (Flags & kSelfSegmentEncrypted) != 0; }
    bool IsCompressed() const { return (Flags & kSelfSegmentCompressed) != 0; }
    std::uint32_t ProgramHeaderIndex() const { return static_cast<std::uint32_t>((Flags >> 20) & 0xfff); }
};

struct FlatProgramHeader {
    std::uint32_t Type;
    std::uint32_t Flags;
    std::uint64_t Offset;
    std::uint64_t MappedAddress;
    std::uint64_t FileSize;
    std::uint64_t MemorySize;
};

std::vector<FlatProgramHeader> readProgramHeaders(const std::vector<std::uint8_t>& bytes, const std::uint64_t tableOffset, const std::uint16_t count) {
    std::vector<FlatProgramHeader> headers;
    headers.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        const std::uint64_t entry = tableOffset + static_cast<std::uint64_t>(index) * kPhdrEntrySize;
        FlatProgramHeader header{};
        header.Type = readAt<std::uint32_t>(bytes, entry + kPhdrTypeOffset, "Program header");
        header.Flags = readAt<std::uint32_t>(bytes, entry + kPhdrFlagsOffset, "Program header");
        header.Offset = readAt<std::uint64_t>(bytes, entry + kPhdrOffsetOffset, "Program header");
        header.MappedAddress = readAt<std::uint64_t>(bytes, entry + kPhdrVaddrOffset, "Program header");
        header.FileSize = readAt<std::uint64_t>(bytes, entry + kPhdrFileSizeOffset, "Program header");
        header.MemorySize = readAt<std::uint64_t>(bytes, entry + kPhdrMemSizeOffset, "Program header");
        headers.push_back(header);
    }
    return headers;
}

void validateEmbeddedElf(const std::vector<std::uint8_t>& bytes, const std::uint64_t elfOffset) {
    if (!rangeFits(elfOffset, kElfHeaderSize, bytes.size()))
        throw RelinkerException("SELF container is too small for its embedded ELF header", elfOffset);
    if (readAt<std::uint32_t>(bytes, elfOffset, "ELF magic") != 0x464c457fu)
        throw RelinkerException("SELF container does not embed an ELF image", elfOffset);
    if (bytes[elfOffset + 4] != 2 || bytes[elfOffset + 5] != 1 || bytes[elfOffset + 6] != 1)
        throw RelinkerException("SELF container must embed a little-endian ELF64 version 1 image", elfOffset + 4);
    if (readAt<std::uint16_t>(bytes, elfOffset + 0x12, "ELF machine") != 62)
        throw RelinkerException("SELF container must embed an x86-64 ELF image", elfOffset + 0x12);
    const auto entrySize = readAt<std::uint16_t>(bytes, elfOffset + kEhdrPhEntSizeOffset, "ELF program header entry size");
    const auto count = readAt<std::uint16_t>(bytes, elfOffset + kEhdrPhNumOffset, "ELF program header count");
    if (count != 0 && entrySize != kPhdrEntrySize)
        throw RelinkerException("Embedded ELF program header entry size must be 56 bytes", elfOffset + kEhdrPhEntSizeOffset);
}

}

bool IsSelfContainer(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4) return false;
    std::uint32_t magic;
    std::memcpy(&magic, bytes.data(), sizeof(magic));
    return magic == kSelfMagic;
}

std::vector<std::uint8_t> UnwrapFakeSelf(const std::vector<std::uint8_t>& self) {
    if (!IsSelfContainer(self))
        throw RelinkerException("Input is not a SELF container");
    if (self.size() < kSelfHeaderSize)
        throw RelinkerException("SELF header is truncated");
    const auto segmentCount = readAt<std::uint16_t>(self, kSelfSegmentCountOffset, "SELF segment count");
    if (segmentCount == 0)
        throw RelinkerException("SELF container declares no segments", kSelfSegmentCountOffset);

    std::vector<SelfSegment> segments;
    segments.reserve(segmentCount);
    for (std::uint16_t index = 0; index < segmentCount; ++index) {
        const std::uint64_t entry = kSelfHeaderSize + static_cast<std::uint64_t>(index) * kSelfSegmentHeaderSize;
        SelfSegment segment{};
        segment.Flags = readAt<std::uint64_t>(self, entry, "SELF segment header");
        segment.FileOffset = readAt<std::uint64_t>(self, entry + 8, "SELF segment header");
        segment.FileSize = readAt<std::uint64_t>(self, entry + 16, "SELF segment header");
        segment.MemorySize = readAt<std::uint64_t>(self, entry + 24, "SELF segment header");
        segments.push_back(segment);
    }

    const std::uint64_t elfOffset = kSelfHeaderSize + static_cast<std::uint64_t>(segmentCount) * kSelfSegmentHeaderSize;
    validateEmbeddedElf(self, elfOffset);
    const auto phdrOffset = readAt<std::uint64_t>(self, elfOffset + kEhdrPhOffOffset, "ELF program header offset");
    const auto phdrCount = readAt<std::uint16_t>(self, elfOffset + kEhdrPhNumOffset, "ELF program header count");
    if (phdrCount == 0)
        throw RelinkerException("Embedded ELF image has no program headers", elfOffset + kEhdrPhNumOffset);
    if (phdrOffset < kElfHeaderSize)
        throw RelinkerException("Embedded ELF program header table overlaps the ELF header", elfOffset + kEhdrPhOffOffset);
    const std::uint64_t phdrTableSize = static_cast<std::uint64_t>(phdrCount) * kPhdrEntrySize;
    if (phdrOffset > std::numeric_limits<std::uint64_t>::max() - phdrTableSize - elfOffset || !rangeFits(elfOffset + phdrOffset, phdrTableSize, self.size()))
        throw RelinkerException("Embedded ELF program header table exceeds the SELF container", elfOffset + kEhdrPhOffOffset);
    const auto headers = readProgramHeaders(self, elfOffset + phdrOffset, phdrCount);

    std::uint64_t flatSize = phdrOffset + phdrTableSize;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto& header = headers[index];
        if (header.Offset > std::numeric_limits<std::uint64_t>::max() - header.FileSize)
            throw RelinkerException("Embedded ELF program header file range overflows", elfOffset + phdrOffset + index * kPhdrEntrySize);
        flatSize = std::max(flatSize, header.Offset + header.FileSize);
    }
    if (flatSize > kMaxFlatImageSize)
        throw RelinkerException("Embedded ELF image is too large to reconstruct", flatSize);

    std::vector<std::uint8_t> flat(static_cast<std::size_t>(flatSize));
    std::copy_n(self.begin() + static_cast<std::ptrdiff_t>(elfOffset), static_cast<std::ptrdiff_t>(kElfHeaderSize), flat.begin());
    std::copy_n(self.begin() + static_cast<std::ptrdiff_t>(elfOffset + phdrOffset), static_cast<std::ptrdiff_t>(phdrTableSize), flat.begin() + static_cast<std::ptrdiff_t>(phdrOffset));

    for (std::size_t index = 0; index < segments.size(); ++index) {
        const auto& segment = segments[index];
        const std::uint64_t entry = kSelfHeaderSize + index * kSelfSegmentHeaderSize;
        if (!segment.IsBlocked()) continue;
        if (segment.IsEncrypted())
            throw RelinkerException("SELF segment " + std::to_string(index) + " is encrypted; only decrypted (fake) SELF containers are supported", entry);
        if (segment.IsCompressed())
            throw RelinkerException("SELF segment " + std::to_string(index) + " is compressed; only uncompressed SELF segments are supported", entry);
        const auto target = segment.ProgramHeaderIndex();
        if (target >= headers.size())
            throw RelinkerException("SELF segment " + std::to_string(index) + " references program header " + std::to_string(target) + " beyond the table", entry);
        if (!rangeFits(segment.FileOffset, segment.FileSize, self.size()))
            throw RelinkerException("SELF segment " + std::to_string(index) + " exceeds the container", entry + 8);
        const auto& header = headers[target];
        const std::uint64_t length = std::min(segment.FileSize, header.FileSize);
        if (length == 0) continue;
        std::copy_n(self.begin() + static_cast<std::ptrdiff_t>(segment.FileOffset), static_cast<std::ptrdiff_t>(length), flat.begin() + static_cast<std::ptrdiff_t>(header.Offset));
    }

    return flat;
}

std::size_t PromoteStandaloneRelro(std::vector<std::uint8_t>& elf) {
    if (elf.size() < kElfHeaderSize || readAt<std::uint32_t>(elf, 0, "ELF magic") != 0x464c457fu) return 0;
    const auto phdrOffset = readAt<std::uint64_t>(elf, kEhdrPhOffOffset, "ELF program header offset");
    const auto phdrCount = readAt<std::uint16_t>(elf, kEhdrPhNumOffset, "ELF program header count");
    const auto entrySize = readAt<std::uint16_t>(elf, kEhdrPhEntSizeOffset, "ELF program header entry size");
    if (phdrCount == 0 || entrySize != kPhdrEntrySize || !rangeFits(phdrOffset, static_cast<std::uint64_t>(phdrCount) * kPhdrEntrySize, elf.size())) return 0;
    const auto headers = readProgramHeaders(elf, phdrOffset, phdrCount);

    const auto coveredByLoad = [&](const FlatProgramHeader& candidate) {
        for (const auto& header : headers) {
            if (header.Type != PT_LOAD) continue;
            if (candidate.MappedAddress >= header.MappedAddress && candidate.MappedAddress - header.MappedAddress < header.MemorySize)
                return true;
        }
        return false;
    };

    std::size_t promoted = 0;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        const auto& header = headers[index];
        if (header.Type != PT_OS_RELRO || header.MemorySize == 0 || coveredByLoad(header)) continue;
        const std::uint64_t entry = phdrOffset + index * kPhdrEntrySize;
        if (header.FileSize > header.MemorySize)
            throw RelinkerException("PT_SCE_RELRO file size exceeds its memory size", entry);
        writeAt<std::uint32_t>(elf, entry + kPhdrTypeOffset, PT_LOAD);
        writeAt<std::uint32_t>(elf, entry + kPhdrFlagsOffset, (header.Flags & (PF_R | PF_W | PF_X)) | PF_R | PF_W);
        ++promoted;
    }
    return promoted;
}

OrbisInputReport PrepareOrbisInput(std::vector<std::uint8_t>& bytes) {
    OrbisInputReport report;
    if (IsSelfContainer(bytes)) {
        try {
            report.SelfSegmentCount = readAt<std::uint16_t>(bytes, kSelfSegmentCountOffset, "SELF segment count");
            bytes = UnwrapFakeSelf(bytes);
        } catch (const RelinkerException& error) {
            throw RelinkerException(std::string("The input is a SELF container, not an ELF, and cannot be unwrapped: ") + error.what(), error.FailureOffset);
        }
        report.UnwrappedSelf = true;
    }
    report.PromotedRelroSegments = PromoteStandaloneRelro(bytes);
    return report;
}

}
