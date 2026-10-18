// AnyPS4 shim: shadPS4 includes only this Boost header explicitly and receives the
// repetition macros and boost::container::small_vector transitively; the shim brings
// the same three pieces into scope.
#pragma once

#define BOOST_PP_STRINGIZE(text) BOOST_PP_STRINGIZE_I(text)
#define BOOST_PP_STRINGIZE_I(text) #text

#include <boost/preprocessor/repetition/repeat.hpp>
#include <boost/container/small_vector.hpp>
