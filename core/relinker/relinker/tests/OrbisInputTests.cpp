#include <relinker/parsing/OrbisInput.hpp>
#include <relinker/parsing/ElfReader.hpp>
#include <relinker/domain/Types.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint64_t kCodeOffset = 0x4000;
constexpr std::uint64_t kCodeSize = 0x300;
constexpr std::uint64_t kRelroOffset = 0x8000;
constexpr std::uint64_t kRelroVaddr = 0x4000;
constexpr std::uint64_t kRelroFileSize = 0x100;
constexpr std::uint64_t kRelroMemSize = 0x4000;
constexpr std::uint64_t kDataOffset = 0xc000;
constexpr std::uint64_t kDataVaddr = 0x8000;
constexpr std::uint64_t kDataSize = 0x200;
constexpr std::uint64_t kDynamicOffset = kDataOffset + 0x100;
constexpr std::uint64_t kDynamicSize = 0x40;
constexpr std::uint16_t kProgramHeaderCount = 4;

template<typename TValue>
void Write(Bytes& bytes, const std::size_t offset, const TValue value) {
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) throw std::runtime_error("Fixture write is out of bounds");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<typename TValue>
TValue Read(const Bytes& bytes, const std::size_t offset) {
    TValue value;
    if (offset > bytes.size() || sizeof(value) > bytes.size() - offset) throw std::runtime_error("Fixture read is out of bounds");
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void WriteProgramHeader(Bytes& image, const std::size_t base, const std::uint16_t index, const std::uint32_t type, const std::uint32_t flags, const std::uint64_t offset, const std::uint64_t vaddr, const std::uint64_t fileSize, const std::uint64_t memSize) {
    const std::size_t header = base + 64 + index * 56;
    Write<std::uint32_t>(image, header, type);
    Write<std::uint32_t>(image, header + 4, flags);
    Write<std::uint64_t>(image, header + 8, offset);
    Write<std::uint64_t>(image, header + 16, vaddr);
    Write<std::uint64_t>(image, header + 24, vaddr);
    Write<std::uint64_t>(image, header + 32, fileSize);
    Write<std::uint64_t>(image, header + 40, memSize);
    Write<std::uint64_t>(image, header + 48, 0x4000);
}

Bytes OrbisElfHeaders() {
    Bytes image(64 + kProgramHeaderCount * 56);
    const std::uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 9};
    std::memcpy(image.data(), ident, sizeof(ident));
    Write<std::uint16_t>(image, 16, 0xfe10);
    Write<std::uint16_t>(image, 18, 62);
    Write<std::uint32_t>(image, 20, 1);
    Write<std::uint64_t>(image, 24, 0x100);
    Write<std::uint64_t>(image, 32, 64);
    Write<std::uint16_t>(image, 52, 64);
    Write<std::uint16_t>(image, 54, 56);
    Write<std::uint16_t>(image, 56, kProgramHeaderCount);
    WriteProgramHeader(image, 0, 0, 1, 5, kCodeOffset, 0, kCodeSize, kCodeSize);
    WriteProgramHeader(image, 0, 1, 0x61000010, 4, kRelroOffset, kRelroVaddr, kRelroFileSize, kRelroMemSize);
    WriteProgramHeader(image, 0, 2, 1, 6, kDataOffset, kDataVaddr, kDataSize, kDataSize);
    WriteProgramHeader(image, 0, 3, 2, 6, kDynamicOffset, 0, kDynamicSize, kDynamicSize);
    return image;
}

Bytes Pattern(const std::size_t size, const std::uint8_t seed) {
    Bytes bytes(size);
    for (std::size_t index = 0; index < size; ++index) bytes[index] = static_cast<std::uint8_t>(seed + index * 7);
    return bytes;
}

Bytes FlatOrbisElf() {
    Bytes flat(kDataOffset + kDataSize);
    const auto headers = OrbisElfHeaders();
    std::copy(headers.begin(), headers.end(), flat.begin());
    const auto code = Pattern(kCodeSize, 0x11);
    const auto relro = Pattern(kRelroFileSize, 0x22);
    const auto data = Pattern(kDataSize, 0x33);
    std::copy(code.begin(), code.end(), flat.begin() + kCodeOffset);
    std::copy(relro.begin(), relro.end(), flat.begin() + kRelroOffset);
    std::copy(data.begin(), data.end(), flat.begin() + kDataOffset);
    return flat;
}

struct SelfSegmentSpec {
    std::uint64_t Flags;
    Bytes Data;
};

Bytes WrapSelf(const Bytes& headers, const std::vector<SelfSegmentSpec>& segments) {
    const std::size_t segmentTable = 0x20 + segments.size() * 0x20;
    Bytes self(segmentTable + headers.size());
    Write<std::uint32_t>(self, 0, 0x1d3d154f);
    self[4] = 0;
    self[5] = 1;
    self[6] = 1;
    self[7] = 0x12;
    self[8] = 1;
    self[9] = 1;
    Write<std::uint16_t>(self, 0x0c, static_cast<std::uint16_t>(segmentTable + headers.size()));
    Write<std::uint16_t>(self, 0x18, static_cast<std::uint16_t>(segments.size()));
    Write<std::uint16_t>(self, 0x1a, 0x22);
    std::copy(headers.begin(), headers.end(), self.begin() + static_cast<std::ptrdiff_t>(segmentTable));
    std::size_t cursor = self.size();
    for (std::size_t index = 0; index < segments.size(); ++index) {
        const std::size_t entry = 0x20 + index * 0x20;
        Write<std::uint64_t>(self, entry, segments[index].Flags);
        Write<std::uint64_t>(self, entry + 8, cursor);
        Write<std::uint64_t>(self, entry + 16, segments[index].Data.size());
        Write<std::uint64_t>(self, entry + 24, segments[index].Data.size());
        self.insert(self.end(), segments[index].Data.begin(), segments[index].Data.end());
        cursor = self.size();
    }
    Write<std::uint32_t>(self, 0x10, static_cast<std::uint32_t>(self.size()));
    return self;
}

std::uint64_t BlockedFlags(const std::uint32_t programHeader, const std::uint64_t extra = 0) {
    return (static_cast<std::uint64_t>(programHeader) << 20) | 0x800 | 0x4 | extra;
}

Bytes FakeSelfFixture(const std::uint64_t extraFlags = 0) {
    std::vector<SelfSegmentSpec> segments;
    segments.push_back({0x110004, Pattern(0x40, 0x99)});
    segments.push_back({BlockedFlags(0, extraFlags), Pattern(kCodeSize, 0x11)});
    segments.push_back({BlockedFlags(1), Pattern(kRelroFileSize, 0x22)});
    segments.push_back({BlockedFlags(2), Pattern(kDataSize, 0x33)});
    return WrapSelf(OrbisElfHeaders(), segments);
}

void Require(const bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

void ExpectFailure(const std::function<void()>& action, const std::string& fragment) {
    try {
        action();
    } catch (const Relinker::RelinkerException& error) {
        if (std::string(error.what()).find(fragment) == std::string::npos)
            throw std::runtime_error("Unexpected failure message: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Expected failure containing: " + fragment);
}

void TestUnwrapsFakeSelf() {
    Bytes bytes = FakeSelfFixture();
    Require(Relinker::IsSelfContainer(bytes), "fixture is a SELF container");
    const auto report = Relinker::PrepareOrbisInput(bytes);
    Require(report.UnwrappedSelf, "report notes the unwrapped SELF");
    Require(report.SelfSegmentCount == 4, "report counts the SELF segments");
    Require(report.PromotedRelroSegments == 1, "report counts the promoted RELRO segment");
    Require(!Relinker::IsSelfContainer(bytes), "result is not a SELF container");
    const auto expected = FlatOrbisElf();
    Require(bytes.size() == expected.size(), "flat image size matches the program header extent");
    Require(std::equal(bytes.begin(), bytes.begin() + 64, expected.begin()), "ELF header is copied verbatim");
    Require(std::equal(bytes.begin() + kCodeOffset, bytes.begin() + kCodeOffset + kCodeSize, expected.begin() + kCodeOffset), "code segment lands at its program header offset");
    Require(std::equal(bytes.begin() + kRelroOffset, bytes.begin() + kRelroOffset + kRelroFileSize, expected.begin() + kRelroOffset), "RELRO segment lands at its program header offset");
    Require(std::equal(bytes.begin() + kDataOffset, bytes.begin() + kDataOffset + kDataSize, expected.begin() + kDataOffset), "data segment lands at its program header offset");
    Require(std::all_of(bytes.begin() + 64 + kProgramHeaderCount * 56, bytes.begin() + kCodeOffset, [](const std::uint8_t value) { return value == 0; }), "gaps between segments are zero");

    const Relinker::ElfReader reader(bytes);
    const auto header = reader.ReadHeader();
    Require(header.Type == 0xfe10 && header.OsAbi == 9 && header.EntryPoint == 0x100, "reader parses the embedded header");
    const auto programHeaders = reader.ReadProgramHeaders();
    Require(programHeaders.size() == kProgramHeaderCount, "reader parses every program header");
    Require(programHeaders[1].Type == 1 && programHeaders[1].Flags == 6, "standalone RELRO becomes a writable PT_LOAD");
    Require(programHeaders[1].MappedAddress == kRelroVaddr && programHeaders[1].FileSize == kRelroFileSize && programHeaders[1].MemorySize == kRelroMemSize, "promoted RELRO keeps its ranges");
    Require(reader.TranslateVirtualAddress(kRelroVaddr + 0x10) == kRelroOffset + 0x10, "promoted RELRO addresses translate through PT_LOAD");
    Require(programHeaders[3].Type == 2, "dynamic segment is untouched");
    const auto dynamic = reader.ReadSegment(programHeaders[3]);
    Require(dynamic.size() == kDynamicSize && std::equal(dynamic.begin(), dynamic.end(), expected.begin() + kDynamicOffset), "dynamic segment reads from inside the data segment");
}

void TestPlainElfPassesThrough() {
    Bytes bytes = FlatOrbisElf();
    Write<std::uint32_t>(bytes, 64 + 56 + 0, 1);
    Write<std::uint32_t>(bytes, 64 + 56 + 4, 6);
    const Bytes original = bytes;
    const auto report = Relinker::PrepareOrbisInput(bytes);
    Require(!report.UnwrappedSelf && report.SelfSegmentCount == 0 && report.PromotedRelroSegments == 0, "plain ELF reports no work");
    Require(bytes == original, "plain ELF is unchanged");
}

void TestCoveredRelroIsKept() {
    Bytes bytes = FlatOrbisElf();
    Write<std::uint64_t>(bytes, 64 + 2 * 56 + 16, kRelroVaddr);
    Write<std::uint64_t>(bytes, 64 + 2 * 56 + 40, kRelroMemSize + kDataSize);
    const auto promoted = Relinker::PromoteStandaloneRelro(bytes);
    Require(promoted == 0, "RELRO inside a PT_LOAD is not promoted");
    Require(Read<std::uint32_t>(bytes, 64 + 56) == 0x61000010, "covered RELRO keeps its type");
}

void TestRejectsEncryptedSegment() {
    Bytes bytes = FakeSelfFixture(0x2);
    ExpectFailure([&] { Relinker::PrepareOrbisInput(bytes); }, "is encrypted");
}

void TestRejectsCompressedSegment() {
    Bytes bytes = FakeSelfFixture(0x8);
    ExpectFailure([&] { Relinker::PrepareOrbisInput(bytes); }, "is compressed");
}

void TestRejectsSegmentBeyondProgramHeaders() {
    std::vector<SelfSegmentSpec> segments;
    segments.push_back({BlockedFlags(kProgramHeaderCount), Pattern(0x20, 0x44)});
    Bytes bytes = WrapSelf(OrbisElfHeaders(), segments);
    ExpectFailure([&] { Relinker::PrepareOrbisInput(bytes); }, "beyond the table");
}

void TestRejectsTruncatedContainer() {
    Bytes bytes = FakeSelfFixture();
    bytes.resize(bytes.size() - 0x10);
    ExpectFailure([&] { Relinker::PrepareOrbisInput(bytes); }, "exceeds the container");
    Bytes header = FakeSelfFixture();
    header.resize(0x30);
    ExpectFailure([&] { Relinker::PrepareOrbisInput(header); }, "exceeds the input file");
}

void TestRejectsForeignEmbeddedImage() {
    Bytes bytes = FakeSelfFixture();
    const std::size_t elfOffset = 0x20 + 4 * 0x20;
    Write<std::uint16_t>(bytes, elfOffset + 18, 40);
    ExpectFailure([&] { Relinker::PrepareOrbisInput(bytes); }, "x86-64");
}

void TestUnwrapRequiresSelf() {
    const Bytes bytes = FlatOrbisElf();
    ExpectFailure([&] { Relinker::UnwrapFakeSelf(bytes); }, "not a SELF container");
}

}

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"unwraps fake SELF", TestUnwrapsFakeSelf},
        {"plain ELF passes through", TestPlainElfPassesThrough},
        {"covered RELRO is kept", TestCoveredRelroIsKept},
        {"rejects encrypted segment", TestRejectsEncryptedSegment},
        {"rejects compressed segment", TestRejectsCompressedSegment},
        {"rejects segment beyond program headers", TestRejectsSegmentBeyondProgramHeaders},
        {"rejects truncated container", TestRejectsTruncatedContainer},
        {"rejects foreign embedded image", TestRejectsForeignEmbeddedImage},
        {"unwrap requires SELF", TestUnwrapRequiresSelf},
    };
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
