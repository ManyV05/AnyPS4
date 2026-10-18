#ifndef RELINKER_DOMAIN_ICALLSITERESOLVER_HPP
#define RELINKER_DOMAIN_ICALLSITERESOLVER_HPP

#include <relinker/domain/Types.hpp>
#include <utility>
#include <vector>

namespace Relinker {

class ICallSiteResolver {
public:
    virtual ~ICallSiteResolver() = default;

    virtual std::vector<FileByteOffset> ResolveCallSites(
        const std::vector<std::uint8_t>& textSection,
        FileByteOffset textSectionVAddr,
        VirtualAddress targetGotOrPltAddress,
        ByteCount targetGotOrPltSize,
        const std::vector<std::pair<std::size_t, std::size_t>>& skipRanges = {}
    ) = 0;
};

}

#endif
