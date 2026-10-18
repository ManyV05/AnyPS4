// AnyPS4 shim: a std::vector stands in for boost::container::small_vector.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace boost::container {

template <typename T, std::size_t N, typename Allocator = std::allocator<T>>
using small_vector = std::vector<T, Allocator>;

} // namespace boost::container
