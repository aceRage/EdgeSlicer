#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <string>
#include <vector>

using namespace Slic3r;

// The recent projects are kept under the keys "01", "02", ... in a map ordered by string. With more
// than 99 of them (Preferences allow up to 10000) "100" sorted between "10" and "11", and the list
// came back out of order, in memory and after a restart.

namespace {
std::vector<std::string> numbered_paths(int count)
{
    std::vector<std::string> paths;
    for (int i = 1; i <= count; ++i)
        paths.push_back("/projects/p" + std::to_string(i) + ".3mf");
    return paths;
}
} // namespace

TEST_CASE("Recent projects keep their order past 99 entries", "[AppConfig][Regression]")
{
    const std::vector<std::string> paths = numbered_paths(120);

    SECTION("set, then read back") {
        AppConfig config;
        config.set_recent_projects(paths);
        CHECK(config.get_recent_projects() == paths);
    }

    SECTION("read from the config file") {
        std::string body = "{\"app\":{\"some_key\":\"some_value\"},\"recent_projects\":{";
        for (size_t i = 0; i < paths.size(); ++i) {
            std::string key = std::to_string(i + 1);
            if (key.size() == 1)
                key = "0" + key;
            body += (i ? "," : "") + std::string("\"") + key + "\":\"" + paths[i] + "\"";
        }
        body += "}}";
        const boost::filesystem::path file = boost::filesystem::temp_directory_path() /
                                             boost::filesystem::unique_path("appconfig_recent_%%%%-%%%%.conf");
        {
            boost::nowide::ofstream ofs(file.string(), std::ios::binary);
            ofs << body;
        }
        AppConfig config;
        config.set_loading_path(file.string());
        const std::string error = config.load();
        boost::filesystem::remove(file);
        REQUIRE(error.empty());
        CHECK(config.get_recent_projects() == paths);
    }

    SECTION("up to 99 entries read as before") {
        AppConfig config;
        const std::vector<std::string> short_list = numbered_paths(18);
        config.set_recent_projects(short_list);
        CHECK(config.get_recent_projects() == short_list);
    }
}
