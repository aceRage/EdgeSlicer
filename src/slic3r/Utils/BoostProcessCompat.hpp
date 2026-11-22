#pragma once

// Orca's Boost 1.88 moved the v1 process API to boost/process/v1.hpp.
// Edge ships Boost 1.84, which still exposes it as boost/process.hpp.
#include <boost/version.hpp>
#if BOOST_VERSION >= 108600
#include <boost/process/v1.hpp>
namespace boost {
namespace process = ::boost::process::v1;
}
#else
#include <boost/process.hpp>
#endif
