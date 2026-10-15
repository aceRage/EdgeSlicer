#include <catch2/catch.hpp>

#include "common_func/common_func.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <regex>
#include <sstream>
#include <string>

namespace fs = boost::filesystem;

// The Snapmaker web panel we ship (resources/web/flutter_web: the Device page, pre-print / pre-send
// pages) is Snapmaker's compiled Flutter bundle plus EdgeSlicer's edits, applied by
// scripts/patch_flutter_web_dark.py after every bundle update. These checks keep a bundle bump from
// dropping an edit or bringing back a download from the internet. Only files are read.

namespace {

fs::path bundle_dir()
{
    // TEST_DATA_DIR is <repo>/tests/data
    return fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources" / "web" / "flutter_web";
}

std::string slurp(const fs::path& p)
{
    boost::nowide::ifstream in(p.string(), std::ios::binary);
    std::ostringstream      ss;
    ss << in.rdbuf();
    return ss.str();
}

// The one file index.html / the bootstrap names with a content hash, e.g. main.<16 hex>.js.
std::string hashed_name(const std::string& text, const char* stem)
{
    std::smatch m;
    const std::regex re(std::string(stem) + R"(\.[0-9a-f]{16,}\.js)");
    if (!std::regex_search(text, m, re))
        return {};
    return m.str();
}

} // namespace

TEST_CASE("the bundled web panel is Snapmaker's 2.3.40", "[FlutterWeb]")
{
    const std::string v = slurp(bundle_dir() / "version.json");
    REQUIRE_FALSE(v.empty());
    CHECK(v.find("\"version\":\"2.3.40\"") != std::string::npos);
}

TEST_CASE("the web panel's index.html loads only its own files", "[FlutterWeb]")
{
    const std::string index = slurp(bundle_dir() / "index.html");
    REQUIRE_FALSE(index.empty());

    // No analytics script from a CDN (the page ships an empty PostHog key, but fetched the library anyway),
    // and no other <script src> that is not a file of the bundle.
    CHECK(index.find("posthog") == std::string::npos);
    CHECK(index.find("jsdelivr") == std::string::npos);
    const std::regex external_script(R"(<script[^>]+src="(https?:)?//)");
    CHECK_FALSE(std::regex_search(index, external_script));

    // EdgeSlicer's dark mode script runs ahead of the app.
    const size_t shim = index.find("edgeslicer:dark-mode");
    REQUIRE(shim != std::string::npos);
    CHECK(shim < index.find("flutter_overlay"));
}

TEST_CASE("the web panel's hashed files exist and carry EdgeSlicer's edits", "[FlutterWeb]")
{
    const fs::path    dir   = bundle_dir();
    const std::string index = slurp(dir / "index.html");

    const std::string boot = hashed_name(index, "flutter_bootstrap");
    REQUIRE_FALSE(boot.empty());
    REQUIRE(fs::exists(dir / boot));

    const std::string main_js = hashed_name(slurp(dir / boot), "main");
    REQUIRE_FALSE(main_js.empty());
    CHECK(index.find(main_js) != std::string::npos); // preloaded
    REQUIRE(fs::exists(dir / main_js));

    const std::string main_text = slurp(dir / main_js);
    CHECK(main_text.find("/*edgeslicer:system-theme*/") != std::string::npos);
    CHECK(main_text.find("/*edgeslicer:dark-widgets*/") != std::string::npos);
    CHECK(main_text.find("/*edgeslicer:dark-controls*/") != std::string::npos);
    CHECK(main_text.find("/*edgeslicer:dark-colours*/") != std::string::npos);

    // The patched file's old names are gone: a stale hashed file would be served as immutable.
    int mains = 0, boots = 0;
    for (fs::directory_iterator it(dir), end; it != end; ++it) {
        const std::string name = it->path().filename().string();
        if (std::regex_match(name, std::regex(R"(main\.[0-9a-f]{16,}\.js)")))
            ++mains;
        if (std::regex_match(name, std::regex(R"(flutter_bootstrap\.[0-9a-f]{16,}\.js)")))
            ++boots;
    }
    CHECK(mains == 1);
    CHECK(boots == 1);
}

TEST_CASE("the web panel's dark pictures are shipped", "[FlutterWeb]")
{
    const fs::path images = bundle_dir() / "assets" / "assets" / "images";
    for (const char* name : {"deviceNotConnected_dark.png", "controlDefault_dark.png", "printtaskDefault_dark.png"})
        CHECK(fs::exists(images / name));
}

#ifdef _WIN32
// The version the log and the about text report is the one of the bundled pages, which the page server
// serves; the data dir's old copy (%APPDATA%/EdgeSlicer/web/flutter_web) is not read.
TEST_CASE("the reported web panel version is the bundled one", "[FlutterWeb]")
{
    const std::string v = common::get_flutter_version((bundle_dir() / "version.json").string());
    CHECK(v.find("flutter_version: 2.3.40") == 0);
    CHECK(v.find("build_number: 20260930162817") != std::string::npos);
    CHECK(common::get_flutter_version((bundle_dir() / "no_such_version.json").string()).empty());
}
#endif
