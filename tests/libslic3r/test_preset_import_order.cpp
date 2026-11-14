#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;

// File > Import > Import Configs: a user preset whose parent is another preset of the same import (later in
// the archive, or a later file) was skipped ("can not find inherit preset ... just skip"); a preset whose
// version cannot be read was skipped without a word.

namespace {

// A data folder of its own (never the user's) and a folder for the files to import.
struct ImportScratch
{
    fs::path     dir;
    std::string  saved_data_dir;
    PresetBundle bundle;

    ImportScratch() : dir(fs::temp_directory_path() / fs::unique_path("edgeslicer_import_order_%%%%-%%%%")), saved_data_dir(data_dir())
    {
        fs::create_directories(dir / "src");
        set_data_dir((dir / "data").string());
        bundle.update_user_presets_directory("default");
    }
    ~ImportScratch()
    {
        set_data_dir(saved_data_dir);
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }

    // A user process preset; `parent` empty: a root of its own.
    static std::string process_json(const std::string &name, const std::string &parent, const std::string &version = "1.0.0.0")
    {
        nlohmann::json j;
        j["type"]              = "process";
        j["name"]              = name;
        j["from"]              = "User";
        j["version"]           = version;
        j["print_settings_id"] = name;
        j["layer_height"]      = parent.empty() ? "0.3" : "0.25";
        if (!parent.empty())
            j["inherits"] = parent;
        return j.dump();
    }

    std::string write(const std::string &file, const std::string &body)
    {
        const fs::path path = dir / "src" / file;
        boost::nowide::ofstream out(path.string(), std::ios::binary);
        out << body;
        return path.string();
    }

    std::string write_zip(const std::string &file, const std::vector<std::pair<std::string, std::string>> &entries)
    {
        const fs::path path = dir / "src" / file;
        mz_zip_archive zip;
        mz_zip_zero_struct(&zip);
        REQUIRE(open_zip_writer(&zip, path.string()));
        for (const auto &entry : entries)
            REQUIRE(mz_zip_writer_add_mem(&zip, entry.first.c_str(), entry.second.data(), entry.second.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        REQUIRE(close_zip_writer(&zip));
        return path.string();
    }

    size_t import(std::vector<std::string> files)
    {
        bundle.import_presets(files, [](const std::string &) { return 1; }, ForwardCompatibilitySubstitutionRule::Disable);
        return files.size();
    }

    bool has(const std::string &name) { return bundle.prints.find_preset(name, false) != nullptr; }
};

} // namespace

TEST_CASE("Importing a preset before its parent imports both", "[Preset][Import][Regression]")
{
    ImportScratch s;

    SECTION("in a bundle, the child's entry first") {
        const std::string zip = s.write_zip("bundle.zip", { { "process/Child.json", ImportScratch::process_json("Child", "Parent") },
                                                            { "process/Parent.json", ImportScratch::process_json("Parent", "") } });
        CHECK(s.import({ zip }) == 2);
        CHECK(s.has("Parent"));
        REQUIRE(s.has("Child"));
        CHECK(s.bundle.prints.find_preset("Child", false)->inherits() == "Parent");
    }
    SECTION("as two files, the child's first") {
        const std::string child  = s.write("child.json", ImportScratch::process_json("Child", "Parent"));
        const std::string parent = s.write("parent.json", ImportScratch::process_json("Parent", ""));
        CHECK(s.import({ child, parent }) == 2);
        CHECK(s.has("Parent"));
        CHECK(s.has("Child"));
    }
    SECTION("a chain of three, last to first") {
        const std::string zip = s.write_zip("chain.zip", { { "process/C.json", ImportScratch::process_json("C", "B") },
                                                           { "process/B.json", ImportScratch::process_json("B", "A") },
                                                           { "process/A.json", ImportScratch::process_json("A", "") } });
        CHECK(s.import({ zip }) == 3);
        CHECK(s.has("A"));
        CHECK(s.has("B"));
        CHECK(s.has("C"));
    }
    SECTION("a parent that is nowhere is still not imported") {
        const std::string zip = s.write_zip("orphan.zip", { { "process/Orphan.json", ImportScratch::process_json("Orphan", "Not there") },
                                                            { "process/Root.json", ImportScratch::process_json("Root", "") } });
        CHECK(s.import({ zip }) == 1);
        CHECK(s.has("Root"));
        CHECK_FALSE(s.has("Orphan"));
    }
}

TEST_CASE("An import reports the presets it left out", "[Preset][Import][Regression]")
{
    ImportScratch s;
    const std::string zip = s.write_zip("bundle.zip", { { "process/Bad version.json", ImportScratch::process_json("Bad version", "", "one") },
                                                        { "process/Orphan.json", ImportScratch::process_json("Orphan", "Not there") },
                                                        { "process/Good.json", ImportScratch::process_json("Good", "") } });
    std::vector<std::string>         files{ zip };
    PresetBundle::PresetImportReport report;
    s.bundle.import_presets(files, [](const std::string &) { return 1; }, ForwardCompatibilitySubstitutionRule::Disable, &report);
    CHECK(files.size() == 1);
    CHECK(s.has("Good"));
    REQUIRE(report.bad_version.size() == 1);
    CHECK(fs::path(report.bad_version.front()).filename().string() == "Bad version.json");
    REQUIRE(report.missing_parent.size() == 1);
    CHECK(fs::path(report.missing_parent.front()).filename().string() == "Orphan.json");
}
