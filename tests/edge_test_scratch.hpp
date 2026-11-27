#ifndef EDGE_TEST_SCRATCH_HPP
#define EDGE_TEST_SCRATCH_HPP

// Scratch folder for the files a unit test writes (project 3MFs, preset trees, G-code).
//
// It lives inside the test run's own folder, which TemporaryDirListener (libslic3r_tests.cpp,
// fff_print_tests.cpp) creates per process at the start of the run and removes at the end. A fixed
// %TEMP%/snorca_tests was shared by every test executable, and by every concurrent run of the same
// one (the edgebuild broker runs test executables in parallel), with fixed file names inside it.

#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

#include <string>

namespace edge_test {

// The run's own folder, set by TemporaryDirListener::testRunStarting.
inline boost::filesystem::path &run_dir_slot()
{
    static boost::filesystem::path dir;
    return dir;
}

inline void set_run_dir(const boost::filesystem::path &dir) { run_dir_slot() = dir; }

// <run folder>/snorca_tests, created if missing. Without a listener (an executable that does not
// register one) it falls back to a per-process folder under the system temp folder.
inline boost::filesystem::path snorca_tests_dir()
{
    boost::filesystem::path root = run_dir_slot();
    if (root.empty())
        root = boost::filesystem::temp_directory_path() / ("edgeslicer_tests_" + std::to_string(Slic3r::get_current_pid()));
    const boost::filesystem::path dir = root / "snorca_tests";
    boost::system::error_code ec;
    boost::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace edge_test

#endif // EDGE_TEST_SCRATCH_HPP
