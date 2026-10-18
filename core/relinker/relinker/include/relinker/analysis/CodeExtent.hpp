#ifndef RELINKER_ANALYSIS_CODEEXTENT_HPP
#define RELINKER_ANALYSIS_CODEEXTENT_HPP

#include <relinker/domain/Types.hpp>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace Relinker {

struct CodeLayout {
    std::vector<ProgramHeader> Headers;
    std::map<VirtualAddress, VirtualAddress> DataRanges;
    std::vector<VirtualAddress> JumpTargets;
};

CodeLayout AnalyzeCodeLayout(const std::vector<std::uint8_t>& bytes, const std::vector<ProgramHeader>& headers, VirtualAddress gotAddress, ByteCount jumpRelocationBytes);

std::vector<std::pair<std::size_t, std::size_t>> SkipRangesWithin(const std::map<VirtualAddress, VirtualAddress>& ranges, VirtualAddress begin, ByteCount size);

}

#endif
