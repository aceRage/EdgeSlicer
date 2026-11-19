// The printer-switch fast path of the Bambu Studio preset mirror.
//
// Every printer switch used to run the whole mirror on the UI thread (~0.4 s: it read every
// Bambu Studio preset, .info and base file and every copy of them) although it nearly always
// found nothing to do. Now a switch first takes a listing-only fingerprint of the source folder
// and the manifest, and skips the run when it equals the one a clean run left behind.
//
// What these tests pin down:
//   * the fingerprint changes for every kind of source change the mirror cares about (new,
//     edited - also same size with only the time moved -, deleted, base/ cache, .info sidecar);
//   * it is empty ("must run") when the source cannot be listed;
//   * only a clean run whose source did not move while it ran is remembered;
//   * end to end, in a scratch data dir: a second run is a no-op that leaves the fingerprint
//     unchanged, and a preset edited in Bambu Studio afterwards changes it and is copied.
// Every test works in its own temp directory; nothing touches %APPDATA%.

#include <catch2/catch.hpp>

#include "slic3r/GUI/PresetMirror.hpp"
#include "slic3r/GUI/PresetMirrorCore.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <ctime>
#include <iterator>
#include <string>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::mirror;
namespace bfs = boost::filesystem;

namespace {

struct TempDir
{
    bfs::path path;
    TempDir() : path(bfs::temp_directory_path() / bfs::unique_path("mirror_skip_%%%%%%%%")) { bfs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        bfs::remove_all(path, ec);
    }
};

void write_text(const bfs::path& p, const std::string& text)
{
    bfs::create_directories(p.parent_path());
    boost::nowide::ofstream out(p.string(), std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text(const bfs::path& p)
{
    boost::nowide::ifstream in(p.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Moves a file's write time by `seconds` without changing its bytes.
void touch(const bfs::path& p, long seconds)
{
    const std::time_t t = bfs::last_write_time(p);
    bfs::last_write_time(p, t + seconds);
}

// Restores Slic3r::data_dir() when a test that points it at a scratch dir ends.
struct ScopedDataDir
{
    std::string old;
    explicit ScopedDataDir(const std::string& dir) : old(Slic3r::data_dir()) { Slic3r::set_data_dir(dir); }
    ~ScopedDataDir() { Slic3r::set_data_dir(old); }
};

std::string filament_json(const std::string& name, const std::string& temp)
{
    return "{\n    \"name\": \"" + name + "\",\n    \"from\": \"User\",\n    \"nozzle_temperature\": [\"" + temp + "\"]\n}\n";
}

std::string info_text(long long updated_time)
{
    return "sync_info = update\nuser_id = 123\nsetting_id = \nbase_id = GFSA00\nupdated_time = " + std::to_string(updated_time) + "\n";
}

} // namespace

// ---- the fingerprint ---------------------------------------------------------------------------

TEST_CASE("the source fingerprint is stable while nothing changes", "[PresetMirror][MirrorSkip]")
{
    TempDir d;
    write_text(d.path / "filament" / "My PLA.json", filament_json("My PLA", "215"));
    write_text(d.path / "filament" / "My PLA.info", info_text(100));
    write_text(d.path / "filament" / "base" / "Bambu PLA Basic @base.json", "{}");
    write_text(d.path / "process" / "Fine.json", "{\"name\":\"Fine\"}");

    const std::string a = source_tree_fingerprint(d.path.string());
    REQUIRE_FALSE(a.empty());
    CHECK(a == source_tree_fingerprint(d.path.string()));
    CHECK(a.find("filament/My PLA.json|") != std::string::npos);
    CHECK(a.find("filament/My PLA.info|") != std::string::npos);
    CHECK(a.find("filament/base/Bambu PLA Basic @base.json|") != std::string::npos);
    CHECK(a.find("process/Fine.json|") != std::string::npos);
}

TEST_CASE("every source change the mirror acts on changes the fingerprint", "[PresetMirror][MirrorSkip]")
{
    TempDir d;
    const bfs::path json = d.path / "filament" / "My PLA.json";
    const bfs::path info = d.path / "filament" / "My PLA.info";
    const bfs::path base = d.path / "filament" / "base" / "Bambu PLA Basic @base.json";
    write_text(json, filament_json("My PLA", "215"));
    write_text(info, info_text(100));
    write_text(base, "{}");
    const std::string before = source_tree_fingerprint(d.path.string());
    REQUIRE_FALSE(before.empty());

    SECTION("an edited preset with a different size")
    {
        write_text(json, filament_json("My PLA", "2150"));
        CHECK(source_tree_fingerprint(d.path.string()) != before);
    }
    SECTION("an edited preset with the same size, only its time moved")
    {
        write_text(json, filament_json("My PLA", "220"));
        touch(json, 10);
        CHECK(source_tree_fingerprint(d.path.string()) != before);
    }
    SECTION("only the .info sidecar was rewritten (a new updated_time)")
    {
        write_text(info, info_text(200));
        touch(info, 10);
        CHECK(source_tree_fingerprint(d.path.string()) != before);
    }
    SECTION("a new preset")
    {
        write_text(d.path / "process" / "New.json", "{\"name\":\"New\"}");
        CHECK(source_tree_fingerprint(d.path.string()) != before);
    }
    SECTION("a deleted preset")
    {
        bfs::remove(json);
        CHECK(source_tree_fingerprint(d.path.string()) != before);
    }
    SECTION("the base/ inheritance cache changed")
    {
        write_text(base, "{\"a\":1}");
        CHECK(source_tree_fingerprint(d.path.string()) != before);
    }
}

TEST_CASE("an unlistable source has no fingerprint, an empty one has a fixed one", "[PresetMirror][MirrorSkip]")
{
    CHECK(source_tree_fingerprint("").empty());
    TempDir d;
    CHECK(source_tree_fingerprint((d.path / "missing").string()).empty());
    // A uid folder with no preset folders at all: a settled state the gate may remember.
    const std::string empty = source_tree_fingerprint(d.path.string());
    CHECK_FALSE(empty.empty());
    CHECK(empty == source_tree_fingerprint(d.path.string()));
}

TEST_CASE("file_stamp follows size and time and marks a missing file", "[PresetMirror][MirrorSkip]")
{
    TempDir d;
    const bfs::path p = d.path / ".bs_mirror_manifest.json";
    CHECK(file_stamp(p.string()) == "-");
    write_text(p, "{}");
    const std::string a = file_stamp(p.string());
    CHECK(a != "-");
    touch(p, 10);
    CHECK(file_stamp(p.string()) != a);
}

// ---- the gate ----------------------------------------------------------------------------------

TEST_CASE("only a clean run whose source held still is remembered", "[PresetMirror][MirrorSkip]")
{
    const std::string before = "uid=\nsrc=X\nfilament/a.json|10|1\nmanifest=5|1\n";
    const std::string after  = "uid=\nsrc=X\nfilament/a.json|10|1\nmanifest=7|2\n"; // the run rewrote the manifest
    CHECK(fingerprint_source_part(before) == fingerprint_source_part(after));

    MirrorSkipGate gate;
    CHECK_FALSE(gate.can_skip(after)); // nothing remembered yet

    SECTION("clean run: the fingerprint it left behind is skipped next time")
    {
        gate.record_run(true, before, after);
        CHECK(gate.can_skip(after));
        CHECK_FALSE(gate.can_skip(before)); // the manifest has moved on since `before`
    }
    SECTION("a run with errors is not remembered and forgets the previous one")
    {
        gate.record_run(true, before, after);
        gate.record_run(false, before, after);
        CHECK_FALSE(gate.can_skip(after));
    }
    SECTION("a source that changed while the run read it is not remembered")
    {
        const std::string moved = "uid=\nsrc=X\nfilament/a.json|11|2\nmanifest=7|2\n";
        gate.record_run(true, before, moved);
        CHECK_FALSE(gate.can_skip(moved));
    }
    SECTION("an empty fingerprint (unlistable source) never skips")
    {
        gate.record_run(true, "", after);
        CHECK_FALSE(gate.can_skip(after));
        gate.record_run(true, before, "");
        CHECK_FALSE(gate.can_skip(""));
    }
    SECTION("a manual edit of the manifest (Re-pull all) is a change")
    {
        gate.record_run(true, before, after);
        CHECK_FALSE(gate.can_skip("uid=\nsrc=X\nfilament/a.json|10|1\nmanifest=6|3\n"));
    }
    SECTION("another logged-in uid is a change")
    {
        gate.record_run(true, before, after);
        CHECK_FALSE(gate.can_skip("uid=42\nsrc=X\nfilament/a.json|10|1\nmanifest=7|2\n"));
    }
}

// ---- end to end in a scratch data dir ----------------------------------------------------------

TEST_CASE("a skipped switch never hides a preset edited in Bambu Studio", "[PresetMirror][MirrorSkip]")
{
    // <tmp>/EdgeSlicer is the data dir; the mirror looks for its sibling <tmp>/BambuStudio/user/<uid>.
    TempDir         root;
    const bfs::path data = root.path / "EdgeSlicer";
    const bfs::path src  = root.path / "BambuStudio" / "user" / "123" / "filament";
    bfs::create_directories(data);
    ScopedDataDir scoped(data.string());

    write_text(src / "My PLA.json", filament_json("My PLA", "215"));
    write_text(src / "My PLA.info", info_text(100));
    const bfs::path dst = data / "user" / "default" / "filament" / "My PLA.json";

    MirrorSkipGate gate;

    // First run (startup): copies, clean, remembered.
    std::string before = mirror_change_fingerprint("");
    REQUIRE_FALSE(before.empty());
    MirrorRunResult r = mirror_bambu_user_presets_ex("");
    CHECK(r.clean);
    CHECK(r.copied == 1);
    REQUIRE(bfs::exists(dst));
    CHECK(read_text(dst) == filament_json("My PLA", "215"));
    gate.record_run(r.clean, before, mirror_change_fingerprint(""));

    // A switch with nothing changed: skipped, and a run would indeed have been a no-op.
    const std::string now = mirror_change_fingerprint("");
    CHECK(gate.can_skip(now));
    r = mirror_bambu_user_presets_ex("");
    CHECK(r.clean);
    CHECK(r.copied == 0);

    // The previous no-op run rewrote the manifest; remember what it left.
    gate.record_run(r.clean, now, mirror_change_fingerprint(""));
    CHECK(gate.can_skip(mirror_change_fingerprint("")));

    // The preset is edited in Bambu Studio (new bytes, newer updated_time).
    write_text(src / "My PLA.json", filament_json("My PLA", "225"));
    write_text(src / "My PLA.info", info_text(200));
    touch(src / "My PLA.json", 10);
    touch(src / "My PLA.info", 10);

    before = mirror_change_fingerprint("");
    CHECK_FALSE(gate.can_skip(before)); // the switch runs the mirror again
    r = mirror_bambu_user_presets_ex("");
    CHECK(r.clean);
    CHECK(r.copied == 1);
    CHECK(read_text(dst) == filament_json("My PLA", "225"));

    // The source folder is read only: nothing was written there.
    CHECK(read_text(src / "My PLA.json") == filament_json("My PLA", "225"));
    CHECK(read_text(src / "My PLA.info") == info_text(200));
}

TEST_CASE("with no Bambu Studio folder the run is clean and stays skipped until one appears", "[PresetMirror][MirrorSkip]")
{
    TempDir         root;
    const bfs::path data = root.path / "EdgeSlicer";
    bfs::create_directories(data);
    ScopedDataDir scoped(data.string());

    MirrorSkipGate    gate;
    const std::string before = mirror_change_fingerprint("");
    REQUIRE_FALSE(before.empty());
    const MirrorRunResult r = mirror_bambu_user_presets_ex("");
    CHECK(r.clean);
    CHECK(r.copied == 0);
    gate.record_run(r.clean, before, mirror_change_fingerprint(""));
    CHECK(gate.can_skip(mirror_change_fingerprint("")));

    write_text(root.path / "BambuStudio" / "user" / "123" / "filament" / "My PLA.json", filament_json("My PLA", "215"));
    CHECK_FALSE(gate.can_skip(mirror_change_fingerprint("")));
}

// ---- the gate across restarts (startup fast path) ----------------------------------------------

TEST_CASE("the kept gate record matches only its own fingerprint, version and format", "[PresetMirror][MirrorSkip][MirrorGatePersist]")
{
    const std::string fp     = "uid=\nsrc=X\nfilament/a.json|10|1\nmanifest=7|2\ndest=\nfilament/a.json|10|3\n";
    const std::string record = mirror_gate_record("2.4.7.0", fp);
    REQUIRE_FALSE(record.empty());

    CHECK(mirror_gate_record_allows_skip(record, "2.4.7.0", fp));
    SECTION("another fingerprint") { CHECK_FALSE(mirror_gate_record_allows_skip(record, "2.4.7.0", fp + "process/b.json|1|1\n")); }
    SECTION("another app version") { CHECK_FALSE(mirror_gate_record_allows_skip(record, "2.4.8.0", fp)); }
    SECTION("an empty fingerprint (unlistable) never skips")
    {
        CHECK(mirror_gate_record("2.4.7.0", "").empty());
        CHECK_FALSE(mirror_gate_record_allows_skip(record, "2.4.7.0", ""));
    }
    SECTION("an empty or damaged record never skips")
    {
        CHECK_FALSE(mirror_gate_record_allows_skip("", "2.4.7.0", fp));
        CHECK_FALSE(mirror_gate_record_allows_skip("garbage", "2.4.7.0", fp));
        CHECK_FALSE(mirror_gate_record_allows_skip(record.substr(0, record.size() / 2), "2.4.7.0", fp));
    }
    SECTION("another format number")
    {
        std::string other = record;
        const size_t pos  = other.find(" " + std::to_string(kMirrorGateFormat) + "\n");
        REQUIRE(pos != std::string::npos);
        other.replace(pos, 1 + std::to_string(kMirrorGateFormat).size(), " " + std::to_string(kMirrorGateFormat + 1));
        CHECK_FALSE(mirror_gate_record_allows_skip(other, "2.4.7.0", fp));
    }
    SECTION("CRLF line ends (a hand-opened file) still match")
    {
        std::string crlf;
        for (char c : record) {
            if (c == '\n')
                crlf += '\r';
            crlf += c;
        }
        CHECK(mirror_gate_record_allows_skip(crlf, "2.4.7.0", fp));
    }
}

TEST_CASE("the output-folder listing after the manifest line is not part of the source", "[PresetMirror][MirrorSkip][MirrorGatePersist]")
{
    const std::string source = "uid=\nsrc=X\nfilament/a.json|10|1\n";
    // A copy whose file name contains "manifest=" must not move the cut.
    const std::string a = source + "manifest=7|2\ndest=\nfilament/manifest=odd.json|1|1\n";
    const std::string b = source + "manifest=9|4\ndest=\nfilament/other.json|5|5\n";
    CHECK(fingerprint_source_part(a) == source);
    CHECK(fingerprint_source_part(a) == fingerprint_source_part(b));
}

TEST_CASE("a start after a clean run skips the mirror until something it reads changes", "[PresetMirror][MirrorSkip][MirrorGatePersist]")
{
    TempDir         root;
    const bfs::path data = root.path / "EdgeSlicer";
    const bfs::path src  = root.path / "BambuStudio" / "user" / "123" / "filament";
    bfs::create_directories(data);
    ScopedDataDir scoped(data.string());

    write_text(src / "My PLA.json", filament_json("My PLA", "215"));
    write_text(src / "My PLA.info", info_text(100));
    write_text(src / "My PETG.json", filament_json("My PETG", "240"));
    write_text(src / "My PETG.info", info_text(100));
    const bfs::path dst_pla  = data / "user" / "default" / "filament" / "My PLA.json";
    const bfs::path dst_petg = data / "user" / "default" / "filament" / "My PETG.json";
    const bfs::path record   = data / "cache" / "preset_mirror_gate.txt";

    // What startup does: no record yet, so it runs; the clean run is kept on disk.
    std::string before = mirror_change_fingerprint("");
    CHECK_FALSE(mirror_startup_can_skip(before));
    MirrorRunResult r = mirror_bambu_user_presets_ex("");
    REQUIRE(r.clean);
    CHECK(r.copied == 2);
    MirrorSkipGate gate;
    gate.record_run(r.clean, before, mirror_change_fingerprint(""));
    REQUIRE_FALSE(gate.last().empty());
    mirror_persist_gate(gate.last());
    REQUIRE(bfs::exists(record));

    // The next start: nothing changed, so it is skipped, and a run would indeed copy nothing.
    CHECK(mirror_startup_can_skip(mirror_change_fingerprint("")));
    {
        // (Run it anyway to prove the point, then put the record back as a real start would.)
        const std::string b2 = mirror_change_fingerprint("");
        MirrorRunResult   r2 = mirror_bambu_user_presets_ex("");
        CHECK(r2.copied == 0);
        gate.record_run(r2.clean, b2, mirror_change_fingerprint(""));
        mirror_persist_gate(gate.last());
        CHECK(mirror_startup_can_skip(mirror_change_fingerprint("")));
    }

    SECTION("a preset edited in Bambu Studio while EdgeSlicer was closed")
    {
        write_text(src / "My PLA.json", filament_json("My PLA", "225"));
        write_text(src / "My PLA.info", info_text(200));
        touch(src / "My PLA.json", 10);
        touch(src / "My PLA.info", 10);
        before = mirror_change_fingerprint("");
        CHECK_FALSE(mirror_startup_can_skip(before));
        r = mirror_bambu_user_presets_ex("");
        CHECK(r.copied == 1);
        CHECK(read_text(dst_pla) == filament_json("My PLA", "225"));
    }
    SECTION("a preset added in Bambu Studio")
    {
        write_text(src / "My ABS.json", filament_json("My ABS", "260"));
        CHECK_FALSE(mirror_startup_can_skip(mirror_change_fingerprint("")));
    }
    SECTION("a mirrored copy deleted here (the mirror's output changed)")
    {
        bfs::remove(dst_petg);
        CHECK_FALSE(mirror_startup_can_skip(mirror_change_fingerprint("")));
    }
    SECTION("a mirrored copy edited here")
    {
        write_text(dst_petg, filament_json("My PETG", "250"));
        touch(dst_petg, 10);
        CHECK_FALSE(mirror_startup_can_skip(mirror_change_fingerprint("")));
    }
    SECTION("the manifest changed (Re-pull all)")
    {
        repull_mirrored_presets();
        touch(data / "user" / "default" / ".bs_mirror_manifest.json", 10);
        CHECK_FALSE(mirror_startup_can_skip(mirror_change_fingerprint("")));
    }
    SECTION("another login")
    {
        CHECK_FALSE(mirror_startup_can_skip(mirror_change_fingerprint("123")));
    }
    SECTION("a run that was not clean removes the record")
    {
        gate.record_run(false, before, std::string());
        CHECK(gate.last().empty());
        mirror_persist_gate(gate.last());
        CHECK_FALSE(bfs::exists(record));
        CHECK_FALSE(mirror_startup_can_skip(mirror_change_fingerprint("")));
    }
    SECTION("a record written by another app version")
    {
        write_text(record, mirror_gate_record("0.0.0.1", mirror_change_fingerprint("")));
        CHECK_FALSE(mirror_startup_can_skip(mirror_change_fingerprint("")));
    }
    SECTION("the record was deleted (cache folder cleared)")
    {
        bfs::remove(record);
        CHECK_FALSE(mirror_startup_can_skip(mirror_change_fingerprint("")));
    }
    SECTION("a seeded gate lets a later printer switch skip, and still sees an edit")
    {
        MirrorSkipGate after_start;
        const std::string now = mirror_change_fingerprint("");
        REQUIRE(mirror_startup_can_skip(now));
        after_start.seed(now);
        CHECK(after_start.can_skip(mirror_change_fingerprint("")));
        write_text(src / "My PETG.json", filament_json("My PETG", "245"));
        touch(src / "My PETG.json", 10);
        CHECK_FALSE(after_start.can_skip(mirror_change_fingerprint("")));
    }
}
