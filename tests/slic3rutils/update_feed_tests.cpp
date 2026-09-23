// Snapmaker's update feeds are off (2026-09-22).
//
// meta-cfg.snapmaker.com/.cn served two feeds the app polled at every start: /upgrade/flutter/,
// whose package replaced the web pages in data_dir/web/flutter_web - the copy the local page
// server actually served - and /upgrade/profile/, whose package replaced data_dir/system/Snapmaker
// whenever its version was higher than the installed one, reverting the profiles we ship. Now:
//  - neither feed has a default URL; only an ini override ("profile_upgrade_url") names a server,
//  - the page server reads /web/flutter_web/ from the installed resources, so a copy left in the
//    data dir by an earlier version (or by Snapmaker's feed) is never served.

#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/HttpServer.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

namespace fs = boost::filesystem;

TEST_CASE("no default profile or web-resource update server", "[UpdateFeed]")
{
    Slic3r::AppConfig cfg;
    CHECK(cfg.get_preset_upgrade_url().empty());
    CHECK(cfg.get_web_resource_upgrade_url().empty());

    // The ini override is the one way to name a (self-hosted) profile server.
    cfg.set("profile_upgrade_url", "https://updates.example.org/profile/en/version.json");
    CHECK(cfg.get_preset_upgrade_url() == "https://updates.example.org/profile/en/version.json");
}

TEST_CASE("flutter pages are served from the installed resources, never from a data-dir copy", "[UpdateFeed][HttpServer]")
{
    const std::string old_resources = Slic3r::resources_dir();
    const std::string old_data      = Slic3r::data_dir();

    const fs::path root      = fs::temp_directory_path() / fs::unique_path("edgeslicer-flutter-%%%%-%%%%");
    const fs::path resources = root / "resources";
    const fs::path data      = root / "data";
    fs::create_directories(resources / "web" / "flutter_web");
    fs::create_directories(data / "web" / "flutter_web");
    boost::nowide::ofstream((resources / "web" / "flutter_web" / "index.html").string()) << "bundled";
    boost::nowide::ofstream((resources / "web" / "flutter_web" / "version.json").string()) << R"({"version":"2.3.38"})";
    // What Snapmaker's feed left behind: a newer build in the data dir.
    boost::nowide::ofstream((data / "web" / "flutter_web" / "index.html").string()) << "downloaded";
    boost::nowide::ofstream((data / "web" / "flutter_web" / "version.json").string())
        << R"({"version":"9.9.99","build_number":"29991231235959"})";

    Slic3r::set_resources_dir(resources.string());
    Slic3r::set_data_dir(data.string());

    using Slic3r::GUI::HttpServer;
    const fs::path bundled_index = resources / "web" / "flutter_web" / "index.html";
    for (const char *url : { "/web/flutter_web/index.html", "/web/flutter_web/index.html?path=2", "/" }) {
        INFO(url);
        const fs::path mapped(HttpServer::map_url_to_file_path(url));
        CHECK(fs::equivalent(mapped, bundled_index));
    }
    CHECK(fs::equivalent(fs::path(HttpServer::map_url_to_file_path("/web/flutter_web/version.json")),
                         resources / "web" / "flutter_web" / "version.json"));
    // Nothing escapes through "..".
    CHECK(HttpServer::map_url_to_file_path("/web/flutter_web/../../data/web/flutter_web/index.html").empty());
    // The data-dir copy is left alone - it is the user's file - just never read.
    CHECK(fs::exists(data / "web" / "flutter_web" / "index.html"));

    Slic3r::set_resources_dir(old_resources);
    Slic3r::set_data_dir(old_data);
    boost::system::error_code ig;
    fs::remove_all(root, ig);
}
