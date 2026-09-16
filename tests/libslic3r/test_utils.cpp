#include <catch2/catch.hpp>

#include "libslic3r/Utils.hpp"
#include <test_utils.hpp>

#include <boost/filesystem.hpp>

#include <fstream>
#include <string>

using namespace Slic3r;

TEST_CASE("A resolved input path still names the same file after the working directory changes", "[utils]")
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() /
                                        boost::filesystem::unique_path("cli_input_%%%%%%%%");
    boost::filesystem::create_directories(dir);
    const boost::filesystem::path model = dir / "model.3mf";
    {
        std::ofstream out(model.string());
        out << "3mf";
    }

    struct Cleanup
    {
        boost::filesystem::path path;
        ~Cleanup()
        {
            boost::system::error_code ec;
            boost::filesystem::remove_all(path, ec);
        }
    } cleanup{dir};

    // Resolve the bare name from the directory holding the file, then move away from it. The guard
    // restores the directory the test started in, wherever this leaves it.
    ScopedWorkingDirectory cwd(dir);
    const std::string      resolved = resolve_cli_input_path(model.filename().string());
    boost::filesystem::current_path(boost::filesystem::path(TEST_DATA_DIR));

    REQUIRE(boost::filesystem::exists(resolved));
    REQUIRE(boost::filesystem::equivalent(resolved, model));
    // Control: the bare name finds nothing from here, so resolving it this late would have failed.
    REQUIRE_FALSE(boost::filesystem::exists(model.filename().string()));
}

TEST_CASE("resolve_cli_input_path completes a relative path against the working directory", "[utils]")
{
    ScopedWorkingDirectory        cwd(boost::filesystem::temp_directory_path());
    // Read back rather than reusing temp_directory_path(): changing to it resolves any symlink.
    const boost::filesystem::path here = boost::filesystem::current_path();

    SECTION("a bare name") {
        REQUIRE(resolve_cli_input_path("model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ./ prefix is dropped") {
        REQUIRE(resolve_cli_input_path("./model.3mf") == (here / "model.3mf").make_preferred().string());
    }
    SECTION("a ../ traversal is collapsed") {
        REQUIRE(resolve_cli_input_path("../model.3mf") == (here.parent_path() / "model.3mf").make_preferred().string());
    }
}

TEST_CASE("resolve_cli_input_path leaves inputs that must not be completed unchanged", "[utils]")
{
    SECTION("an absolute path") {
        const boost::filesystem::path absolute = (boost::filesystem::temp_directory_path() / "model.3mf").make_preferred();
        REQUIRE(resolve_cli_input_path(absolute.string()) == absolute.string());
    }
#ifdef _WIN32
    // Every absolute form Windows accepts opens today, so each must come back byte for byte:
    // normalizing them would rewrite the forward slashes and rebuild the \\?\ and UNC prefixes.
    SECTION("an absolute Windows path of any form") {
        for (const std::string absolute : {R"(C:\models\model.3mf)",
                                           R"(C:/models/model.3mf)",
                                           R"(\\server\share\model.3mf)",
                                           R"(\\?\C:\models\model.3mf)"})
            REQUIRE(resolve_cli_input_path(absolute) == absolute);
    }
#endif
    // These are downloaded rather than opened, and completing one would produce a path, not a URL.
    // Edge registers edgeslicer:// and still accepts every older scheme this fork has shipped.
    SECTION("a custom open protocol URL") {
        for (const std::string url : {"edgeslicer://open/?file=https://example.com/model.3mf",
                                      "ultraone://open/?file=https://example.com/model.3mf",
                                      "Snapmaker_Orca://open/?file=https://example.com/model.3mf",
                                      "snapmaker-orca://open/?file=https://example.com/model.3mf",
                                      "orcaslicer://open/?file=https://example.com/model.3mf",
                                      "prusaslicer://open/?file=https://example.com/model.3mf",
                                      "bambustudio://open/?file=https://example.com/model.3mf",
                                      "cura://open/?file=https://example.com/model.3mf"})
            REQUIRE(resolve_cli_input_path(url) == url);
    }
    SECTION("an empty argument") { REQUIRE(resolve_cli_input_path("").empty()); }
}
