// The Stream tab page (resources/web/orca/stream_center.html) is served to the PC's own webview by
// the app's local page server, and to a phone by the hub. It declares its icons relative to
// itself, which the hub answers from its own routes; on the PC the same request lands on
// /web/orca/icon-192.png and the page server logged "file path is null for: ..." (an error line)
// every time the tab opened. The page server now answers the page's icon names from resources/images.

#include <catch2/catch.hpp>

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/HttpServer.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = boost::filesystem;
using Slic3r::GUI::HttpServer;

namespace {
struct ResourcesDir
{
    std::string old;
    ResourcesDir()
    {
        old = Slic3r::resources_dir();
        Slic3r::set_resources_dir(SLIC3R_TEST_RESOURCES_DIR);
    }
    ~ResourcesDir() { Slic3r::set_resources_dir(old); }
};
} // namespace

TEST_CASE("[StreamPageIcons] the page server answers the icon names the Stream page declares", "[StreamPageIcons]")
{
    ResourcesDir res;
    const fs::path images = fs::path(SLIC3R_TEST_RESOURCES_DIR) / "images";
    struct Row { const char* url; const char* file; };
    for (const Row& r : { Row { "/web/orca/icon-192.png", "Snapmaker_Orca_192px.png" },
                          Row { "/web/orca/icon-512.png", "Snapmaker_Orca_512px.png" },
                          Row { "/web/orca/icon-512-maskable.png", "Snapmaker_Orca_512px_maskable.png" },
                          Row { "/web/orca/apple-touch-icon.png", "Snapmaker_Orca_180px.png" },
                          Row { "/web/orca/icon-192.png?x=1", "Snapmaker_Orca_192px.png" } }) {
        INFO(r.url);
        const std::string mapped = HttpServer::map_url_to_file_path(r.url);
        REQUIRE_FALSE(mapped.empty());
        CHECK(fs::equivalent(fs::path(mapped), images / r.file));
    }
}

TEST_CASE("[StreamPageIcons] only the closed list of icon names is rewritten", "[StreamPageIcons]")
{
    ResourcesDir res;
    CHECK(HttpServer::app_icon_file_for_page("/web/orca/icon-999.png") == nullptr);
    CHECK(HttpServer::app_icon_file_for_page("/web/orca/../icon-192.png") == nullptr);
    CHECK(HttpServer::app_icon_file_for_page("/icon-192.png") == nullptr);
    CHECK(HttpServer::app_icon_file_for_page("/web/orca/hub.html") == nullptr);
    CHECK(HttpServer::map_url_to_file_path("/web/orca/icon-999.png").empty());
}

TEST_CASE("[StreamPageIcons] every icon the Stream page links is one the page server can answer", "[StreamPageIcons]")
{
    ResourcesDir res;
    std::ifstream f(std::string(SLIC3R_TEST_RESOURCES_DIR) + "/web/orca/stream_center.html", std::ios::binary);
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string html = ss.str();
    // <link rel="icon|apple-touch-icon" ... href="name.png">
    const std::regex      link(R"re(<link[^>]*rel="(?:icon|apple-touch-icon)"[^>]*href="([^"#?]+)")re");
    int                   seen = 0;
    for (auto it = std::sregex_iterator(html.begin(), html.end(), link); it != std::sregex_iterator(); ++it) {
        const std::string href = (*it)[1];
        INFO(href);
        // A relative href resolves next to the page.
        REQUIRE(href.find('/') == std::string::npos);
        CHECK_FALSE(HttpServer::map_url_to_file_path("/web/orca/" + href).empty());
        ++seen;
    }
    CHECK(seen >= 1);
}
