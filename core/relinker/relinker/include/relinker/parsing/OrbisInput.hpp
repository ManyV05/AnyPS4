#ifndef RELINKER_PARSING_ORBISINPUT_HPP
#define RELINKER_PARSING_ORBISINPUT_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Relinker {

struct OrbisInputReport {
    bool UnwrappedSelf = false;
    std::size_t SelfSegmentCount = 0;
    std::size_t PromotedRelroSegments = 0;
};

bool IsSelfContainer(const std::vector<std::uint8_t>& bytes);

std::vector<std::uint8_t> UnwrapFakeSelf(const std::vector<std::uint8_t>& self);

std::size_t PromoteStandaloneRelro(std::vector<std::uint8_t>& elf);

OrbisInputReport PrepareOrbisInput(std::vector<std::uint8_t>& bytes);

}

#endif
