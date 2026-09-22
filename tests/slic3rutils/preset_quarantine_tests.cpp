// Tests for the non-destructive preset loader (src/libslic3r/PresetQuarantine.*, used by
// PresetCollection::load_presets in src/libslic3r/Preset.cpp).
//
// Background: upstream Bambu (commit 2b6937acbe) answers "this preset file did not parse" by
// deleting the user's .json AND its sibling .info from disk. On 2026-09-21 that destroyed 208 of
// this user's presets: Bambu Studio writes a literal "nil" into the per-extruder slots a setting
// does not apply to, this fork typed those options as non-nullable scalars, deserialize() threw,
// and the loader erased the files. The log recorded 43 "parse config ... failed" lines naming
// files that no longer existed.
//
// BambuConfigCompat (merged separately) closes that specific trigger by translating nil arrays
// before they reach deserialize(). These tests cover the REMAINING hazard: any preset that fails
// to parse for any OTHER reason - a hand-edited file, one from a newer Bambu Studio, one caught by
// a future config change - must still survive. The rule: the loader may refuse to load a preset,
// but it may never destroy one.

#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetQuarantine.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <cstdlib>
#include <iterator>
#include <string>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

// A scratch preset directory that cleans itself up. Never points at the user's real config.
struct ScratchDir
{
    fs::path root;

    explicit ScratchDir(const std::string &tag)
    {
        root = fs::temp_directory_path() / fs::unique_path("orca_quarantine_" + tag + "_%%%%%%%%");
        fs::create_directories(root / "filament");
    }
    ~ScratchDir()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path presets_dir() const { return root / "filament"; }

    void write(const std::string &rel, const std::string &content) const
    {
        const fs::path p = root / rel;
        fs::create_directories(p.parent_path());
        std::ofstream ofs(p.string(), std::ios::binary);
        ofs << content;
    }
};

// A filament preset the loader accepts. Keys mirror what the fork writes for a user preset.
std::string valid_preset_json(const std::string &name)
{
    return std::string("{\n")
        + "    \"type\": \"filament\",\n"
        + "    \"name\": \"" + name + "\",\n"
        + "    \"from\": \"User\",\n"
        + "    \"version\": \"1.9.0.0\",\n"
        + "    \"instantiation\": \"true\",\n"
        + "    \"filament_type\": [\"PLA\"],\n"
        + "    \"nozzle_temperature\": [\"220\"]\n"
        + "}\n";
}

// Truncated mid-object: nlohmann throws, load_from_json sets `reason`, and the loader used to
// delete the file at Preset.cpp's `if (!reason.empty())` branch.
std::string unparseable_preset_json()
{
    return "{\n    \"type\": \"filament\",\n    \"name\": \"Broken\",\n    \"nozzle_temp";
}

std::string read_file(const fs::path &p)
{
    std::ifstream ifs(p.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

// Runs the real loader over dir.
void run_loader(const fs::path &dir)
{
    PresetCollection filaments(Preset::TYPE_FILAMENT, Preset::filament_options(),
                               static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Filament");
    PresetsConfigSubstitutions substitutions;
    // The loader appends the subdir itself, so hand it the parent and the leaf separately.
    filaments.load_presets(dir.parent_path().string(), dir.filename().string(), substitutions,
                           ForwardCompatibilitySubstitutionRule::EnableSilent);
}

fs::path quarantine_dir(const ScratchDir &scratch)
{
    return scratch.presets_dir() / PresetQuarantine::dir_name;
}

} // namespace

TEST_CASE("an unparseable preset is preserved, not deleted", "[PresetQuarantine]")
{
    ScratchDir scratch("unparseable");
    scratch.write("filament/Broken.json", unparseable_preset_json());
    const fs::path original = scratch.presets_dir() / "Broken.json";
    REQUIRE(fs::exists(original));

    PresetQuarantine::take(); // drain anything a previous test left behind
    run_loader(scratch.presets_dir());

    // The cardinal guarantee: the bytes still exist somewhere.
    CHECK_FALSE(fs::exists(original));               // it was moved out of the preset directory
    const fs::path quarantined = quarantine_dir(scratch) / "Broken.json";
    REQUIRE(fs::exists(quarantined));                // ... and it landed in quarantine

    // Byte-for-byte: quarantine moves, it does not rewrite.
    CHECK(read_file(quarantined) == unparseable_preset_json());
    PresetQuarantine::take();
}

TEST_CASE("a valid preset still loads normally", "[PresetQuarantine]")
{
    ScratchDir scratch("valid");
    scratch.write("filament/Good One.json", valid_preset_json("Good One"));

    PresetQuarantine::take();
    run_loader(scratch.presets_dir());

    // Untouched, and nothing was quarantined.
    CHECK(fs::exists(scratch.presets_dir() / "Good One.json"));
    CHECK_FALSE(fs::exists(quarantine_dir(scratch)));
    CHECK(PresetQuarantine::peek().empty());
    PresetQuarantine::take();
}

TEST_CASE("a valid preset survives alongside a broken one", "[PresetQuarantine]")
{
    ScratchDir scratch("mixed");
    scratch.write("filament/Good One.json", valid_preset_json("Good One"));
    scratch.write("filament/Broken.json", unparseable_preset_json());

    PresetQuarantine::take();
    run_loader(scratch.presets_dir());

    CHECK(fs::exists(scratch.presets_dir() / "Good One.json"));
    CHECK(fs::exists(quarantine_dir(scratch) / "Broken.json"));
    // One casualty, not two: a broken neighbour must not take the good preset with it.
    CHECK(PresetQuarantine::peek().size() == 1);
    PresetQuarantine::take();
}

TEST_CASE("the .info travels with the .json", "[PresetQuarantine]")
{
    ScratchDir scratch("info");
    scratch.write("filament/Broken.json", unparseable_preset_json());
    scratch.write("filament/Broken.info", "sync_info = \nuser_id = 12345\nsetting_id = PFUS001\nbase_id = \nupdated_time = 1700000000\n");

    PresetQuarantine::take();
    run_loader(scratch.presets_dir());

    // The .info carries the preset's cloud identity; quarantining the .json without it would
    // leave an orphan that can never be restored.
    CHECK(fs::exists(quarantine_dir(scratch) / "Broken.json"));
    REQUIRE(fs::exists(quarantine_dir(scratch) / "Broken.info"));
    CHECK(read_file(quarantine_dir(scratch) / "Broken.info").find("PFUS001") != std::string::npos);

    // Neither half is left behind in the preset directory.
    CHECK_FALSE(fs::exists(scratch.presets_dir() / "Broken.json"));
    CHECK_FALSE(fs::exists(scratch.presets_dir() / "Broken.info"));
    PresetQuarantine::take();
}

TEST_CASE("a name collision in quarantine does not clobber the earlier file", "[PresetQuarantine]")
{
    ScratchDir scratch("collision");

    // First run: the original broken preset is quarantined under its own name.
    scratch.write("filament/Broken.json", "{\"first\": ");
    scratch.write("filament/Broken.info", "sync_info = \nuser_id = 1\nsetting_id = GEN_ONE\nbase_id = \nupdated_time = 1700000001\n");
    PresetQuarantine::take();
    run_loader(scratch.presets_dir());
    REQUIRE(fs::exists(quarantine_dir(scratch) / "Broken.json"));

    // The user restores from a backup / Bambu Studio writes the name again, and it is broken too.
    scratch.write("filament/Broken.json", "{\"second\": ");
    scratch.write("filament/Broken.info", "sync_info = \nuser_id = 2\nsetting_id = GEN_TWO\nbase_id = \nupdated_time = 1700000002\n");
    PresetQuarantine::take();
    run_loader(scratch.presets_dir());

    // The FIRST casualty must still be intact - overwriting it would be the same data loss this
    // whole change exists to prevent.
    REQUIRE(fs::exists(quarantine_dir(scratch) / "Broken.json"));
    REQUIRE(fs::exists(quarantine_dir(scratch) / "Broken.1.json"));
    CHECK(read_file(quarantine_dir(scratch) / "Broken.json")   == "{\"first\": ");
    CHECK(read_file(quarantine_dir(scratch) / "Broken.1.json") == "{\"second\": ");

    // The .info pair keeps the same suffix as its .json, so the two halves stay matched up.
    REQUIRE(fs::exists(quarantine_dir(scratch) / "Broken.info"));
    REQUIRE(fs::exists(quarantine_dir(scratch) / "Broken.1.info"));
    CHECK(read_file(quarantine_dir(scratch) / "Broken.info").find("GEN_ONE")   != std::string::npos);
    CHECK(read_file(quarantine_dir(scratch) / "Broken.1.info").find("GEN_TWO") != std::string::npos);
    PresetQuarantine::take();
}

TEST_CASE("the reported count matches what was quarantined", "[PresetQuarantine]")
{
    ScratchDir scratch("count");
    scratch.write("filament/Broken A.json", unparseable_preset_json());
    scratch.write("filament/Broken B.json", "{ not json at all");
    scratch.write("filament/Broken C.json", "");
    scratch.write("filament/Good One.json", valid_preset_json("Good One"));

    PresetQuarantine::take();
    run_loader(scratch.presets_dir());

    const PresetQuarantine::Report report = PresetQuarantine::peek();
    CHECK(report.size() == 3);

    // The count the dialog shows must equal the number of files actually sitting in the
    // directory it points the user at - a count that overstates is as bad as a silent delete.
    size_t on_disk = 0;
    for (const auto &e : fs::directory_iterator(quarantine_dir(scratch)))
        if (e.path().extension() == ".json")
            ++ on_disk;
    CHECK(on_disk == report.size());

    // The directory the dialog names is the one the files are in.
    CHECK(fs::path(report.directory()) == quarantine_dir(scratch));

    // Every entry names a real file that exists right now.
    for (const PresetQuarantine::Entry &entry : report.entries()) {
        CHECK_FALSE(entry.quarantined_path.empty());
        CHECK(fs::exists(entry.quarantined_path));
        CHECK_FALSE(entry.reason.empty());
    }

    // take() drains, so a second load does not double-count.
    CHECK(PresetQuarantine::take().size() == 3);
    CHECK(PresetQuarantine::peek().empty());
}

TEST_CASE("quarantining never deletes when the destination cannot be created", "[PresetQuarantine]")
{
    ScratchDir scratch("nodest");
    scratch.write("filament/Broken.json", unparseable_preset_json());

    // Block the quarantine directory by occupying its name with a regular file, so
    // create_directories() fails. The preset must then be left exactly where it is.
    scratch.write(std::string("filament/") + PresetQuarantine::dir_name, "not a directory");

    PresetQuarantine::take();
    run_loader(scratch.presets_dir());

    // Left in place rather than deleted: falling back to a delete is the bug.
    CHECK(fs::exists(scratch.presets_dir() / "Broken.json"));

    const PresetQuarantine::Report report = PresetQuarantine::peek();
    REQUIRE(report.size() == 1);
    CHECK(report.entries().front().quarantined_path.empty());
    PresetQuarantine::take();
}

// End-to-end harness: point the REAL loader at a directory named by ORCA_E2E_PRESET_DIR.
// Skipped unless that variable is set, so it costs nothing in a normal test run. Used by
// scratchpad/e2e_check.py to prove on disk that a deliberately corrupted preset directory
// survives a load. Hidden so it never runs as part of the default suite.
TEST_CASE("end-to-end: load a corrupted preset directory", "[.][PresetQuarantineE2E]")
{
    const char *dir = std::getenv("ORCA_E2E_PRESET_DIR");
    if (dir == nullptr || *dir == '\0') {
        WARN("ORCA_E2E_PRESET_DIR not set; skipping");
        return;
    }

    const fs::path presets(dir);
    REQUIRE(fs::exists(presets));

    PresetQuarantine::take();
    run_loader(presets);

    const PresetQuarantine::Report report = PresetQuarantine::peek();
    WARN("quarantined " << report.size() << " preset(s) into " << report.directory());
    for (const PresetQuarantine::Entry &entry : report.entries())
        WARN("  " << entry.original_path << " -> " << entry.quarantined_path);

    // Nothing was deleted: every quarantined file is readable at its new home.
    for (const PresetQuarantine::Entry &entry : report.entries()) {
        CHECK_FALSE(fs::exists(entry.original_path));
        if (! entry.quarantined_path.empty())
            CHECK(fs::exists(entry.quarantined_path));
    }
    PresetQuarantine::take();
}
