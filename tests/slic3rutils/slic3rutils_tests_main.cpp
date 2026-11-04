#include <catch_main.hpp>

#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/filesystem.hpp>

#include <string>

namespace {

// The application sets temporary_dir() at startup; without it the 3MF reader puts its backup
// folder under "/snapmaker_orca_model", which the read-only root on macOS refuses (on Windows it
// lands in C:\). Give the whole run its own folder under the system temp dir. A listener, not a
// static initializer: temporary_dir() is a namespace-scope std::string in another translation unit.
struct TemporaryDirListener : Catch::TestEventListenerBase
{
    using TestEventListenerBase::TestEventListenerBase;

    void testRunStarting(Catch::TestRunInfo const &info) override
    {
        TestEventListenerBase::testRunStarting(info);
        m_dir = boost::filesystem::temp_directory_path() /
            ("edgeslicer_slic3rutils_tests_" + std::to_string(Slic3r::get_current_pid()));
        boost::filesystem::create_directories(m_dir);
        Slic3r::set_temporary_dir(m_dir.string());
    }

    void testRunEnded(Catch::TestRunStats const &stats) override
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(m_dir, ec);
        TestEventListenerBase::testRunEnded(stats);
    }

    boost::filesystem::path m_dir;
};

} // namespace

CATCH_REGISTER_LISTENER(TemporaryDirListener)

// These three cases talk to real hosts on the internet (github.com, jigsaw.w3.org), so their
// outcome says more about the network and those servers than about this code: jigsaw.w3.org's
// /HTTP/Basic/ answers 403 to every client now, curl included. They are hidden ([.network]) so a
// plain run and ctest skip them; run them on purpose with  slic3rutils_tests "[network]".

TEST_CASE("Check SSL certificates paths", "[Http][NotWorking][.network]") {
    
    Slic3r::Http g = Slic3r::Http::get("https://github.com/");
    
    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });
    
    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });
    
    g.perform_sync();
    
    REQUIRE(status == 200);
}

TEST_CASE("Http digest authentication", "[Http][NotWorking][.network]") {
    Slic3r::Http g = Slic3r::Http::get("https://jigsaw.w3.org/HTTP/Digest/");

    g.auth_digest("guest", "guest");

    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });

    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });

    g.perform_sync();

    REQUIRE(status == 200);
}

TEST_CASE("Http basic authentication", "[Http][NotWorking][.network]") {
    Slic3r::Http g = Slic3r::Http::get("https://jigsaw.w3.org/HTTP/Basic/");

    g.auth_basic("guest", "guest");

    unsigned status = 0;
    g.on_error([&status](std::string, std::string, unsigned http_status) {
        status = http_status;
    });

    g.on_complete([&status](std::string /* body */, unsigned http_status){
        status = http_status;
    });

    g.perform_sync();

    REQUIRE(status == 200);
}

