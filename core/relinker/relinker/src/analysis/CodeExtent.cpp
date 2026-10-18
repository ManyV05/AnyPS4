#include <relinker/analysis/CodeExtent.hpp>
#include <relinker/analysis/UnusedNidFilter/EhFrameReader.hpp>
#include <elfpatcher/general/ElfConstants.hpp>
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <codegen/CodegenException.hpp>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>

namespace Relinker {

using namespace Elfpatcher;

namespace {

struct CodeRange {
    VirtualAddress Begin;
    VirtualAddress End;
    FileByteOffset Offset;
};

std::vector<CodeRange> executableRanges(const std::vector<ProgramHeader>& headers) {
    std::vector<CodeRange> ranges;
    for (const auto& header : headers)
        if (header.Type == PT_LOAD && (header.Flags & PF_X) != 0 && header.FileSize != 0)
            ranges.push_back({header.MappedAddress, header.MappedAddress + header.FileSize, header.Offset});
    return ranges;
}

const CodeRange* rangeOf(const std::vector<CodeRange>& ranges, const VirtualAddress address) {
    for (const auto& range : ranges)
        if (address >= range.Begin && address < range.End) return &range;
    return nullptr;
}

std::vector<ProgramHeader> trimToFunctions(const std::vector<std::uint8_t>& bytes, const std::vector<ProgramHeader>& headers, const std::vector<UnusedNidFilter::StrictCodeRegion>& functions, const VirtualAddress gotAddress, const ByteCount jumpRelocationBytes) {
    constexpr ByteCount relaEntSize = 24;
    auto result = headers;
    for (auto& header : result) {
        if (header.Type != PT_LOAD || (header.Flags & PF_X) == 0 || header.FileSize == 0) continue;
        if (header.Offset > bytes.size() || header.FileSize > bytes.size() - header.Offset)
            throw RelinkerException("Executable segment exceeds the file", header.Offset);
        VirtualAddress codeEnd = 0;
        for (const auto& function : functions)
            if (function.Begin >= header.MappedAddress && function.Begin - header.MappedAddress < header.FileSize)
                codeEnd = std::max(codeEnd, function.End);
        if (codeEnd == 0) continue;
        if (codeEnd - header.MappedAddress > header.FileSize)
            throw RelinkerException("Unwind function exceeds executable segment", codeEnd);
        if (jumpRelocationBytes != 0) {
            const auto* segment = bytes.data() + header.Offset;
            const auto size = static_cast<std::size_t>(header.FileSize);
            const auto target = [&](const std::size_t at) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, segment + at + 2, sizeof(displacement));
                return header.MappedAddress + at + 6 + static_cast<VirtualAddress>(static_cast<std::int64_t>(displacement));
            };
            const auto lastFunctionEnd = static_cast<std::size_t>(codeEnd - header.MappedAddress);
            std::optional<std::size_t> plt;
            for (std::size_t at = lastFunctionEnd; size >= 16 && at <= size - 16; ++at) {
                if (segment[at] == 0xff && segment[at + 1] == 0x35 && segment[at + 6] == 0xff && segment[at + 7] == 0x25
                    && target(at) == gotAddress + 8 && target(at + 6) == gotAddress + 16) {
                    plt = at;
                    break;
                }
            }
            if (!plt)
                throw RelinkerException("PLT not found after the last unwind function", codeEnd);
            const ByteCount pltSize = 16 * (1 + jumpRelocationBytes / relaEntSize);
            if (pltSize > size - *plt)
                throw RelinkerException("PLT exceeds executable segment", header.MappedAddress + *plt);
            codeEnd = header.MappedAddress + *plt + pltSize;
        }
        header.FileSize = codeEnd - header.MappedAddress;
    }
    return result;
}

}

CodeLayout AnalyzeCodeLayout(const std::vector<std::uint8_t>& bytes, const std::vector<ProgramHeader>& headers, const VirtualAddress gotAddress, const ByteCount jumpRelocationBytes) {
    const auto functions = UnusedNidFilter::ReadExceptionFunctions(bytes, headers, {}, {});
    CodeLayout layout;
    layout.Headers = trimToFunctions(bytes, headers, functions, gotAddress, jumpRelocationBytes);
    const auto ranges = executableRanges(layout.Headers);
    const Codegen::X64InstructionDecoder decoder;
    const auto dataEnd = [&](const VirtualAddress address) -> std::optional<VirtualAddress> {
        const auto next = layout.DataRanges.upper_bound(address);
        if (next == layout.DataRanges.begin() || std::prev(next)->second <= address) return std::nullopt;
        return std::prev(next)->second;
    };
    std::map<VirtualAddress, std::pair<VirtualAddress, VirtualAddress>> candidates;
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& function : functions) {
            if (function.End <= function.Begin) continue;
            const auto* range = rangeOf(ranges, function.Begin);
            if (range == nullptr || function.End > range->End) continue;
            for (auto address = function.Begin; address < function.End;) {
                if (const auto skip = dataEnd(address)) {
                    address = *skip;
                    continue;
                }
                const auto offset = static_cast<std::size_t>(range->Offset + (address - range->Begin));
                Codegen::DecodedInstructionInfo info;
                try {
                    info = decoder.DecodeInstruction(bytes.data() + offset, static_cast<std::size_t>(function.End - address));
                } catch (const Codegen::CodegenException&) {
                    break;
                }
                if (info.Length == 0 || info.Length > function.End - address) break;
                if (info.HasRipRelativeDisp && bytes[offset + info.OpcodeOffset] == 0x8d && (info.RexPrefix & 8) != 0) {
                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, bytes.data() + offset + info.RipRelativeDispOffset, sizeof(displacement));
                    const auto target = static_cast<VirtualAddress>(static_cast<std::int64_t>(address + info.Length) + displacement);
                    if (target % 4 == 0 && rangeOf(ranges, target) != nullptr) candidates.emplace(target, std::make_pair(function.Begin, function.End));
                }
                address += info.Length;
            }
        }
        for (auto candidate = candidates.begin(); candidate != candidates.end(); ++candidate) {
            const auto table = candidate->first;
            if (dataEnd(table)) continue;
            const auto [functionBegin, functionEnd] = candidate->second;
            const auto* range = rangeOf(ranges, table);
            auto limit = range->End;
            if (const auto next = std::next(candidate); next != candidates.end() && next->first < limit) limit = next->first;
            std::vector<VirtualAddress> targets;
            for (auto entry = table; limit - entry >= 4; entry += 4) {
                std::int32_t displacement = 0;
                std::memcpy(&displacement, bytes.data() + range->Offset + (entry - range->Begin), sizeof(displacement));
                const auto target = static_cast<VirtualAddress>(static_cast<std::int64_t>(table) + displacement);
                if (target < functionBegin || target >= functionEnd || (target >= table && target < entry + 4)) break;
                targets.push_back(target);
            }
            if (targets.size() < 2) continue;
            layout.DataRanges.emplace(table, table + targets.size() * 4);
            layout.JumpTargets.insert(layout.JumpTargets.end(), targets.begin(), targets.end());
        }
    }
    return layout;
}

std::vector<std::pair<std::size_t, std::size_t>> SkipRangesWithin(const std::map<VirtualAddress, VirtualAddress>& ranges, const VirtualAddress begin, const ByteCount size) {
    std::vector<std::pair<std::size_t, std::size_t>> result;
    const auto end = begin + size;
    for (const auto& [rangeBegin, rangeEnd] : ranges) {
        if (rangeEnd <= begin || rangeBegin >= end) continue;
        result.emplace_back(static_cast<std::size_t>(std::max(rangeBegin, begin) - begin), static_cast<std::size_t>(std::min(rangeEnd, end) - begin));
    }
    return result;
}

}
