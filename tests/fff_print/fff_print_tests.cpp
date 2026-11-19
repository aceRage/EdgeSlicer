#include <catch_main.hpp>

#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include <boost/filesystem.hpp>

#include <string>

namespace {

// The application sets temporary_dir() at startup; without it the 3MF reader puts its backup
// folder under "/snapmaker_orca_model", which the read-only root on macOS refuses (on Windows it
// lands in %TEMP%\snapmaker_orca_model). Give the whole run its own folder under the system temp
// dir. A listener, not a static initializer: temporary_dir() is a namespace-scope std::string in
// another translation unit.
struct TemporaryDirListener : Catch::TestEventListenerBase
{
    using TestEventListenerBase::TestEventListenerBase;

    void testRunStarting(Catch::TestRunInfo const &info) override
    {
        TestEventListenerBase::testRunStarting(info);
        m_dir = boost::filesystem::temp_directory_path() /
            ("edgeslicer_fff_print_tests_" + std::to_string(Slic3r::get_current_pid()));
        boost::filesystem::create_directories(m_dir);
        Slic3r::set_temporary_dir(m_dir.string());
    }

    void testRunEnded(Catch::TestRunStats const &stats) override
    {
        try {
            boost::system::error_code ec;
            boost::filesystem::remove_all(m_dir, ec);
        } catch (...) {
            // Cleanup must never throw out of a Catch2 listener.
        }
        TestEventListenerBase::testRunEnded(stats);
    }

    boost::filesystem::path m_dir;
};

} // namespace

CATCH_REGISTER_LISTENER(TemporaryDirListener)
