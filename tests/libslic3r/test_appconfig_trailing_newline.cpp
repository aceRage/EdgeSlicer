#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

using namespace Slic3r;

// Regression tests for a startup crash: AppConfig::load() used to throw an uncaught
// std::out_of_range (from std::string::substr) whenever the on-disk EdgeSlicer.conf did not end
// with the exact "}\n# MD5 checksum <32 hex>\n" tail the writer normally produces. Any of: no
// trailing newline after the final '}', CRLF line endings, an empty file, or an empty/whitespace
// body must be handled by load() without throwing, either loading successfully or returning a
// non-empty error string so the caller's "corrupted config" recovery path can run.

namespace {

// Returns a path to a fresh temp file under the OS temp dir; not created on disk yet.
std::string make_temp_conf_path(const std::string &suffix)
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path();
    boost::filesystem::path file = dir / (std::string("appconfig_test_") + suffix + ".conf");
    return file.string();
}

void write_file_raw(const std::string &path, const std::string &content)
{
    boost::nowide::ofstream ofs(path, std::ios::binary);
    ofs << content;
    ofs.close();
}

// Loads `content` from a temp file via AppConfig::load() and returns {error_message, app_config}.
// The temp file is removed afterwards regardless of outcome.
std::string load_conf_content(AppConfig &config, const std::string &path, const std::string &content)
{
    write_file_raw(path, content);
    config.set_loading_path(path);
    std::string error;
    try {
        error = config.load();
    } catch (...) {
        boost::filesystem::remove(path);
        throw;
    }
    boost::filesystem::remove(path);
    return error;
}

} // namespace

TEST_CASE("AppConfig::load never throws on a malformed trailing checksum/newline", "[AppConfig]")
{
    AppConfig config;

    SECTION("Valid JSON with no trailing newline after the final brace does not throw") {
        std::string path = make_temp_conf_path("no_trailing_newline");
        std::string content = R"({"app":{"some_key":"some_value"}})"; // no trailing "\n" at all
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, content));
        // No checksum present -> logged as a mismatch, but the JSON body itself is valid,
        // so the config should still load successfully.
        CHECK(error.empty());
        CHECK(config.get("app", "some_key") == "some_value");
    }

    SECTION("Valid JSON followed by CRLF and a checksum-shaped comment does not throw") {
        std::string path = make_temp_conf_path("crlf");
        std::string content = "{\"app\":{\"some_key\":\"some_value\"}}\r\n# MD5 checksum deadbeefdeadbeefdeadbeefdeadbeef\r\n";
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, content));
        CHECK(error.empty());
        CHECK(config.get("app", "some_key") == "some_value");
    }

    SECTION("Valid JSON followed by LF and a checksum-shaped comment does not throw") {
        std::string path = make_temp_conf_path("lf_checksum");
        std::string content = "{\"app\":{\"some_key\":\"some_value\"}}\n# MD5 checksum deadbeefdeadbeefdeadbeefdeadbeef\n";
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, content));
        CHECK(error.empty());
        CHECK(config.get("app", "some_key") == "some_value");
    }

    SECTION("Truncated checksum line (cut off mid-comment) does not throw") {
        std::string path = make_temp_conf_path("truncated_checksum");
        std::string content = "{\"app\":{\"some_key\":\"some_value\"}}\n# MD5 checksum dead"; // cut off, no final newline
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, content));
        CHECK(error.empty());
        CHECK(config.get("app", "some_key") == "some_value");
    }

    SECTION("Empty file does not throw and fails gracefully") {
        std::string path = make_temp_conf_path("empty");
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, ""));
        // No JSON body at all: must be reported as an error, not silently accepted, and must
        // definitely not crash.
        CHECK_FALSE(error.empty());
    }

    SECTION("Whitespace-only file does not throw and fails gracefully") {
        std::string path = make_temp_conf_path("whitespace_only");
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, "   \r\n\t\n  "));
        CHECK_FALSE(error.empty());
    }

    SECTION("File with no closing brace at all does not throw and fails gracefully") {
        std::string path = make_temp_conf_path("no_brace");
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, "not even json"));
        CHECK_FALSE(error.empty());
    }

    SECTION("Single character file ('}' only, no newline) does not throw") {
        std::string path = make_temp_conf_path("just_brace");
        std::string error;
        REQUIRE_NOTHROW(error = load_conf_content(config, path, "}"));
        CHECK_FALSE(error.empty());
    }
}
