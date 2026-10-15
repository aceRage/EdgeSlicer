// The Device page's Storage tab without a display (Orca #16156 part A): the Moonraker file API
// parsers, which printers have a Storage tab and which say why not, the Moonraker backend against an
// in-process mock Moonraker (list, metadata, thumbnail, delete, the guarded print start), and the
// file-grid model the tab is drawn from (order, details fetched a few at a time, stale answers
// dropped, delete and print).
//
// A hidden end-to-end case runs the real Http transport against tests/slic3rutils/fake_moonraker.py:
//   python fake_moonraker.py --port 18201 --control 18202
//   FAKE_MOONRAKER_A=18201:18202 slic3rutils_tests "[PrinterStorageE2E]"

#include <catch2/catch.hpp>

#include "slic3r/GUI/Printer/RemoteFileGridModel.h"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrinterStorage.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::PrinterStorage;
using Slic3r::GUI::FileGridChange;
using Slic3r::GUI::FileGridExecutor;
using Slic3r::GUI::FileGridSort;
using Slic3r::GUI::FileGridStatus;
using Slic3r::GUI::RemoteFileGridModel;
using Slic3r::GUI::ThumbnailState;
using nlohmann::json;

namespace {

std::string url_decode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char) std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

std::string query_value(const std::string& pq, const std::string& key)
{
    const size_t q = pq.find('?');
    if (q == std::string::npos)
        return {};
    const std::string query = pq.substr(q + 1);
    size_t            start = 0;
    while (start < query.size()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos)
            end = query.size();
        const std::string kv = query.substr(start, end - start);
        const size_t      eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key)
            return url_decode(kv.substr(eq + 1));
        start = end + 1;
    }
    return {};
}

std::string path_of(const std::string& pq) { return pq.substr(0, pq.find('?')); }

// A Moonraker in memory: what the Storage tab asks, answered the way Moonraker 0.9 answers it.
class MockMoonraker : public Transport
{
public:
    struct File
    {
        std::uint64_t size { 0 };
        double        modified { 0 };
        int           estimated_time { 0 };
        double        filament_mm { 0 }, filament_g { 0 };
        bool          thumbnail { true };
    };
    std::map<std::string, File> files;
    std::string                 state { "standby" };
    std::string                 current;
    bool                        offline { false };
    unsigned                    list_status { 200 };
    std::string                 require_key; // non-empty: anything without it is 401
    std::string                 sent_key;    // what the test transport was given
    std::vector<std::string>    log;

    static std::string thumb_of(const std::string& path)
    {
        const size_t      slash = path.rfind('/');
        const std::string dir   = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
        const std::string base  = slash == std::string::npos ? path : path.substr(slash + 1);
        return dir + ".thumbs/" + base.substr(0, base.rfind('.')) + "-300x300.png";
    }
    static std::string png_for(const std::string& path) { return "PNG:" + path; }

    HttpReply request(const std::string& method, const std::string& pq, std::size_t, int) override
    {
        log.push_back(method + " " + pq);
        HttpReply r;
        if (offline) {
            r.error = "Couldn't connect to server";
            return r;
        }
        if (!require_key.empty() && sent_key != require_key) {
            r.status = 401;
            r.body   = R"({"error": {"code": 401, "message": "Unauthorized"}})";
            return r;
        }
        const std::string p = path_of(pq);
        if (method == "GET" && p == "/server/files/list") {
            if (list_status != 200) {
                r.status = list_status;
                r.body   = R"({"error": {"code": 404, "message": "Not Found"}})";
                return r;
            }
            json list = json::array();
            for (const auto& kv : files) {
                list.push_back({ { "path", kv.first }, { "modified", kv.second.modified }, { "size", kv.second.size }, { "permissions", "rw" } });
                if (kv.second.thumbnail) // Moonraker lists the .thumbs pictures too
                    list.push_back({ { "path", thumb_of(kv.first) }, { "modified", kv.second.modified }, { "size", 1234 } });
            }
            r.status = 200;
            r.body   = json{ { "result", list } }.dump();
            return r;
        }
        if (method == "GET" && p == "/server/files/metadata") {
            const std::string name = query_value(pq, "filename");
            auto              it   = files.find(name);
            if (it == files.end()) {
                r.status = 404;
                r.body   = json{ { "error", { { "code", 404 }, { "message", "Metadata not available for <" + name + ">" } } } }.dump();
                return r;
            }
            json res = { { "filename", name }, { "size", it->second.size }, { "modified", it->second.modified }, { "slicer", "EdgeSlicer" },
                         { "estimated_time", it->second.estimated_time }, { "filament_total", it->second.filament_mm },
                         { "filament_weight_total", it->second.filament_g }, { "filament_type", "PLA;PETG" }, { "layer_height", 0.2 },
                         { "object_height", 48.0 } };
            if (it->second.thumbnail) {
                const std::string base = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
                const std::string stem = base.substr(0, base.rfind('.'));
                res["thumbnails"]      = json::array({ { { "width", 32 }, { "height", 32 }, { "size", 900 }, { "relative_path", ".thumbs/" + stem + "-32x32.png" } },
                                                       { { "width", 300 }, { "height", 300 }, { "size", 9000 }, { "relative_path", ".thumbs/" + stem + "-300x300.png" } } });
            }
            r.status = 200;
            r.body   = json{ { "result", res } }.dump();
            return r;
        }
        if (p.compare(0, 21, "/server/files/gcodes/") == 0) {
            const std::string path = url_decode(p.substr(21));
            if (method == "GET") {
                for (const auto& kv : files)
                    if (kv.second.thumbnail && thumb_of(kv.first) == path) {
                        r.status = 200;
                        r.body   = png_for(path);
                        return r;
                    }
                r.status = 404;
                r.body   = R"({"error": {"code": 404, "message": "File not found"}})";
                return r;
            }
            if (method == "DELETE") {
                if (!files.erase(path)) {
                    r.status = 404;
                    r.body   = json{ { "error", { { "code", 404 }, { "message", "File does not exist: " + path } } } }.dump();
                    return r;
                }
                r.status = 200;
                r.body   = json{ { "result", { { "item", { { "path", path }, { "root", "gcodes" } } }, { "action", "delete_file" } } } }.dump();
                return r;
            }
        }
        if (method == "GET" && p == "/printer/objects/query") {
            r.status = 200;
            r.body   = json{ { "result", { { "eventtime", 1.0 }, { "status", { { "print_stats", { { "state", state }, { "filename", current } } } } } } } }.dump();
            return r;
        }
        if (method == "POST" && p == "/printer/print/start") {
            const std::string name = query_value(pq, "filename");
            if (state == "printing" || state == "paused") {
                r.status = 400;
                r.body   = R"({"error": {"code": 400, "message": "Printer is busy"}})";
                return r;
            }
            if (!files.count(name)) {
                r.status = 400;
                r.body   = json{ { "error", { { "code", 400 }, { "message", "File not found: " + name } } } }.dump();
                return r;
            }
            state   = "printing";
            current = name;
            r.status = 200;
            r.body   = R"({"result": "ok"})";
            return r;
        }
        r.status = 404;
        r.body   = R"({"error": {"code": 404, "message": "Not Found"}})";
        return r;
    }

    size_t count(const std::string& prefix) const
    {
        size_t n = 0;
        for (const std::string& l : log)
            if (l.compare(0, prefix.size(), prefix) == 0)
                ++n;
        return n;
    }
};

std::shared_ptr<MockMoonraker> sample_printer()
{
    auto m = std::make_shared<MockMoonraker>();
    m->files["benchy.gcode"]        = { 1500000, 1760000300.0, 4980, 4560.0, 13.6, true };
    m->files["cube.gcode"]          = { 20000, 1760000100.0, 600, 800.0, 2.4, true };
    m->files["parts/bracket.gcode"] = { 300000, 1760000200.0, 3700, 2000.0, 6.0, false };
    m->files["notes.txt"]           = { 10, 1760000000.0, 0, 0, 0, false }; // not listed
    return m;
}

// Runs the work and its completion at once, on this thread.
struct InlineExecutor : FileGridExecutor
{
    void run(std::function<void()> work, std::function<void()> then) override
    {
        work();
        then();
    }
};

// Holds every job until the test lets it go: what a slow printer looks like to the model.
struct QueuedExecutor : FileGridExecutor
{
    std::deque<std::pair<std::function<void()>, std::function<void()>>> jobs;
    void run(std::function<void()> work, std::function<void()> then) override { jobs.emplace_back(std::move(work), std::move(then)); }
    bool step()
    {
        if (jobs.empty())
            return false;
        auto job = std::move(jobs.front());
        jobs.pop_front();
        job.first();
        job.second();
        return true;
    }
    void drain()
    {
        while (step()) {}
    }
};

} // namespace

// ---- parsers ------------------------------------------------------------------------------------

TEST_CASE("Storage: Moonraker's file list", "[PrinterStorage]")
{
    const json answer = json::parse(R"({"result": [
        {"path": "benchy.gcode", "modified": 1760000300.5, "size": 1500000, "permissions": "rw"},
        {"path": "parts/bracket.GCODE", "modified": 1760000200, "size": 300000},
        {"path": ".thumbs/benchy-300x300.png", "modified": 1, "size": 9000},
        {"path": "parts/.thumbs/bracket-32x32.png", "modified": 1, "size": 900},
        {"path": "notes.txt", "modified": 1, "size": 10},
        {"path": "../escape.gcode", "modified": 1, "size": 10},
        {"filename": "/old_moonraker.g", "modified": 5, "size": 7},
        {"path": "plate.gcode.3mf", "modified": 6, "size": 70},
        {"path": "", "modified": 1, "size": 1},
        42
    ]})");
    const std::vector<FileEntry> files = parse_file_list(answer);
    REQUIRE(files.size() == 4);
    CHECK(files[0].path == "benchy.gcode");
    CHECK(files[0].name == "benchy.gcode");
    CHECK(files[0].size == 1500000);
    CHECK(files[0].modified == 1760000300);
    CHECK(files[1].path == "parts/bracket.GCODE");
    CHECK(files[1].name == "bracket.GCODE");
    CHECK(files[2].path == "old_moonraker.g"); // "filename" of old releases, leading "/" dropped
    CHECK(files[3].path == "plate.gcode.3mf");

    INFO("the bare array is accepted too");
    CHECK(parse_file_list(json::parse(R"([{"path": "a.gcode", "size": 1}])")).size() == 1);
    CHECK(parse_file_list(json::parse(R"({"error": {"code": 404}})")).empty());
}

TEST_CASE("Storage: what is listed and what is printable", "[PrinterStorage]")
{
    CHECK(is_printable_name("a.gcode"));
    CHECK(is_printable_name("A.GCODE"));
    CHECK(is_printable_name("a.g"));
    CHECK(is_printable_name("a.gco"));
    CHECK_FALSE(is_printable_name("a.bgcode"));
    CHECK_FALSE(is_printable_name("a.gcode.3mf"));
    CHECK(is_listed_name("a.bgcode"));
    CHECK(is_listed_name("a.gcode.3mf"));
    CHECK_FALSE(is_listed_name("a.png"));
    CHECK_FALSE(is_listed_name("printer.cfg"));
}

TEST_CASE("Storage: paths handed back to the printer", "[PrinterStorage]")
{
    CHECK(valid_path("a.gcode"));
    CHECK(valid_path("sub dir/a b.gcode"));
    CHECK_FALSE(valid_path(""));
    CHECK_FALSE(valid_path("/a.gcode"));
    CHECK_FALSE(valid_path("a/"));
    CHECK_FALSE(valid_path("a//b.gcode"));
    CHECK_FALSE(valid_path("../a.gcode"));
    CHECK_FALSE(valid_path("sub/../../a.gcode"));
    CHECK_FALSE(valid_path("./a.gcode"));
    CHECK_FALSE(valid_path("a\\b.gcode"));
    CHECK_FALSE(valid_path(std::string("a\nb.gcode")));
    CHECK_FALSE(valid_path(std::string(1025, 'a')));

    CHECK(encode_path("a.gcode") == "a.gcode");
    CHECK(encode_path("sub dir/a b#1?.gcode") == "sub%20dir/a%20b%231%3F.gcode");
    CHECK(encode_path("Zeitraffer \xC3\xA4.gcode") == "Zeitraffer%20%C3%A4.gcode");
}

TEST_CASE("Storage: a file's metadata", "[PrinterStorage]")
{
    const json answer = json::parse(R"({"result": {
        "filename": "parts/bracket.gcode", "size": 300000, "modified": 1760000200,
        "slicer": "EdgeSlicer", "slicer_version": "2.4.6.0", "estimated_time": 3700.4,
        "filament_total": 2000.5, "filament_weight_total": 6.25, "filament_type": "PLA;PETG",
        "filament_name": "Generic PLA", "layer_height": 0.2, "first_layer_height": 0.25,
        "object_height": 48, "nozzle_diameter": 0.4, "layer_count": 240,
        "thumbnails": [
            {"width": 32, "height": 32, "size": 900, "relative_path": ".thumbs/bracket-32x32.png"},
            {"width": 300, "height": 300, "size": 9000, "relative_path": ".thumbs/bracket-300x300.png"},
            {"width": 500, "height": 500, "size": 19000, "relative_path": "../../../etc/passwd"},
            {"width": 400, "height": 400, "size": 12000, "thumbnail_path": "gcodes/parts/.thumbs/bracket-400.png"}
        ]}})");
    const FileMetadata m = parse_metadata(answer, "parts/bracket.gcode");
    CHECK(m.valid);
    CHECK(m.estimated_time_s == 3700);
    CHECK(m.filament_total_mm == Approx(2000.5));
    CHECK(m.filament_weight_g == Approx(6.25));
    CHECK(m.filament_type == "PLA;PETG");
    CHECK(m.filament_name == "Generic PLA");
    CHECK(m.layer_height == Approx(0.2));
    CHECK(m.first_layer_height == Approx(0.25));
    CHECK(m.object_height == Approx(48));
    CHECK(m.nozzle_diameter == Approx(0.4));
    CHECK(m.layer_count == 240);
    CHECK(m.slicer == "EdgeSlicer");
    CHECK(m.slicer_version == "2.4.6.0");
    INFO("thumbnails come back relative to the gcodes root; one that climbs out is dropped");
    REQUIRE(m.thumbnails.size() == 3);
    CHECK(m.thumbnails[0].path == "parts/.thumbs/bracket-32x32.png");
    CHECK(m.thumbnails[1].path == "parts/.thumbs/bracket-300x300.png");
    CHECK(m.thumbnails[2].path == "parts/.thumbs/bracket-400.png");

    INFO("a file in the root, and an answer with nothing in it");
    const FileMetadata root = parse_metadata(json::parse(R"({"thumbnails": [{"width": 32, "relative_path": ".thumbs/a-32x32.png"}]})"), "a.gcode");
    REQUIRE(root.thumbnails.size() == 1);
    CHECK(root.thumbnails[0].path == ".thumbs/a-32x32.png");
    CHECK(root.estimated_time_s == 0);
    CHECK_FALSE(parse_metadata(json::parse("[]"), "a.gcode").valid);
}

TEST_CASE("Storage: which thumbnail is shown", "[PrinterStorage]")
{
    std::vector<Thumbnail> t;
    CHECK(pick_thumbnail(t) == nullptr);
    t.push_back({ 32, 32, 900, "a-32.png" });
    CHECK(pick_thumbnail(t)->path == "a-32.png"); // the only one
    t.push_back({ 400, 300, 9000, "a-400.png" });
    t.push_back({ 300, 300, 9000, "a-300.png" });
    t.push_back({ 600, 600, 9000, "a-600.png" });
    CHECK(pick_thumbnail(t)->path == "a-300.png");     // the smallest wide enough
    CHECK(pick_thumbnail(t, 1000)->path == "a-600.png"); // none wide enough: the widest
}

TEST_CASE("Storage: the printer's print state", "[PrinterStorage]")
{
    std::string file;
    CHECK(print_state_of(json::parse(R"({"result": {"status": {"print_stats": {"state": "printing", "filename": "a.gcode"}}}})"), &file) ==
          "printing");
    CHECK(file == "a.gcode");
    CHECK(print_state_of(json::parse(R"({"result": {"status": {}}})")).empty());
    CHECK(is_busy_state("printing"));
    CHECK(is_busy_state("paused"));
    CHECK_FALSE(is_busy_state("standby"));
    CHECK_FALSE(is_busy_state("complete"));
    CHECK_FALSE(is_busy_state("error"));
}

// ---- targets ------------------------------------------------------------------------------------

TEST_CASE("Storage: the address Moonraker is asked at", "[PrinterStorage]")
{
    CHECK(base_url_of("192.168.1.50") == "http://192.168.1.50");
    CHECK(base_url_of("  voron.local:7125/ ") == "http://voron.local:7125");
    CHECK(base_url_of("http://pi.local/fluidd/#/") == "http://pi.local");
    CHECK(base_url_of("HTTPS://printer.example:7130/mainsail") == "https://printer.example:7130");
    CHECK(base_url_of("http://user:secret@10.0.0.5") == "http://10.0.0.5");
    CHECK(base_url_of("10.0.0.7:1884") == "http://10.0.0.7");
    CHECK(base_url_of("10.0.0.7:8883") == "http://10.0.0.7");
    CHECK(base_url_of("ws://10.0.0.7:3030/websocket").empty());
    CHECK(base_url_of("").empty());
    CHECK(base_url_of("http://").empty());
}

TEST_CASE("Storage: which printers have a Storage tab", "[PrinterStorage]")
{
    SECTION("a Klipper printer (the fork's Octo/Klipper host type) is asked as Moonraker")
    {
        const Target t = target_for_host("octoprint", "voron.local:7125", "KEY", "Voron", "ph:1");
        CHECK(t.kind == Target::Kind::Moonraker);
        CHECK(t.supported());
        CHECK(t.base_url == "http://voron.local:7125");
        CHECK(t.apikey == "KEY");
        CHECK(t.name == "Voron");
        CHECK(t.key == "ph:1");
        CHECK(target_for_host("moonraker", "10.0.0.2", "", "", "host").supported());
        CHECK(target_for_host("moonraker_mqtt", "10.0.0.2:1884", "", "", "host").base_url == "http://10.0.0.2");
        INFO("no name: the host stands in");
        CHECK(target_for_host("octoprint", "10.0.0.2", "", "", "host").name == "10.0.0.2");
    }
    SECTION("an Elegoo Centauri (Elegoo Link, SDCP) says why not")
    {
        const Target t = target_for_host("elegoolink", "10.0.0.9", "", "Centauri Carbon", "ph:2");
        CHECK(t.kind == Target::Kind::Unsupported);
        CHECK_FALSE(t.supported());
        CHECK(t.reason.find("SDCP") != std::string::npos);
        CHECK(t.base_url.empty());
    }
    SECTION("other hosts say why not")
    {
        CHECK(target_for_host("prusalink", "10.0.0.3", "", "MK4", "host").kind == Target::Kind::Unsupported);
        CHECK(target_for_host("flashforge", "10.0.0.4", "", "AD5M", "host").kind == Target::Kind::Unsupported);
        const Target duet = target_for_host("duet", "10.0.0.5", "", "Duet", "host");
        CHECK(duet.kind == Target::Kind::Unsupported);
        CHECK(duet.reason.find("duet") != std::string::npos);
    }
    SECTION("no address, no tab")
    {
        CHECK(target_for_host("octoprint", "", "", "Voron", "host").kind == Target::Kind::None);
        CHECK(target_for_host("elegoolink", " ", "", "CC", "host").kind == Target::Kind::None);
    }
    SECTION("a Snapmaker on the LAN")
    {
        CHECK(target_for_snapmaker("10.0.0.6", 80, "U1", "sm:SN1").base_url == "http://10.0.0.6");
        CHECK(target_for_snapmaker("10.0.0.6", 7125, "U1", "sm:SN1").base_url == "http://10.0.0.6:7125");
        CHECK(target_for_snapmaker("10.0.0.6", 0, "U1", "sm:SN1").supported());
    }
    CHECK_FALSE(make_files(target_for_host("elegoolink", "10.0.0.9", "", "CC", "ph:2")));
}

// ---- the Moonraker backend ----------------------------------------------------------------------

TEST_CASE("Storage: Moonraker backend against a mock printer", "[PrinterStorage]")
{
    auto           printer = sample_printer();
    MoonrakerFiles files(printer);

    std::vector<FileEntry> list;
    REQUIRE(files.list(list).ok());
    REQUIRE(list.size() == 3); // the .thumbs pictures and notes.txt are not listed
    CHECK(printer->log.back() == "GET /server/files/list?root=gcodes");

    FileMetadata meta;
    REQUIRE(files.metadata("parts/bracket.gcode", meta).ok());
    CHECK(printer->log.back() == "GET /server/files/metadata?filename=parts%2Fbracket.gcode");
    CHECK(meta.estimated_time_s == 3700);
    CHECK(meta.thumbnails.empty());
    REQUIRE(files.metadata("benchy.gcode", meta).ok());
    REQUIRE(meta.thumbnails.size() == 2);
    const Thumbnail* t = pick_thumbnail(meta.thumbnails);
    REQUIRE(t);
    std::string bytes;
    REQUIRE(files.thumbnail(t->path, bytes).ok());
    CHECK(printer->log.back() == "GET /server/files/gcodes/.thumbs/benchy-300x300.png");
    CHECK(bytes == MockMoonraker::png_for(".thumbs/benchy-300x300.png"));

    INFO("a file that is not there");
    const Result missing = files.metadata("nope.gcode", meta);
    CHECK(missing.status == Status::NotFound);
    CHECK(missing.message.find("nope.gcode") != std::string::npos);

    INFO("delete: one DELETE of the encoded path, and the file is gone");
    printer->files["sub dir/old #1.gcode"] = { 10, 1, 0, 0, 0, false };
    REQUIRE(files.remove("sub dir/old #1.gcode").ok());
    CHECK(printer->log.back() == "DELETE /server/files/gcodes/sub%20dir/old%20%231.gcode");
    CHECK(printer->files.count("sub dir/old #1.gcode") == 0);
    CHECK(files.remove("sub dir/old #1.gcode").status == Status::NotFound);
    INFO("never a path that leaves the root");
    const size_t before = printer->log.size();
    CHECK(files.remove("../printer.cfg").status == Status::NotFound);
    CHECK(files.thumbnail("../../moonraker.conf", bytes).status == Status::NotFound);
    CHECK(printer->log.size() == before);
}

TEST_CASE("Storage: starting a print the printer already has", "[PrinterStorage]")
{
    auto           printer = sample_printer();
    MoonrakerFiles files(printer);

    SECTION("an idle printer starts the file")
    {
        REQUIRE(files.start_print("cube.gcode").ok());
        CHECK(printer->count("GET /printer/objects/query") == 1);
        CHECK(printer->log.back() == "POST /printer/print/start?filename=cube.gcode");
        CHECK(printer->state == "printing");
        CHECK(printer->current == "cube.gcode");
    }
    SECTION("a printer that is printing is not sent a start at all")
    {
        printer->state   = "printing";
        printer->current = "benchy.gcode";
        const Result r   = files.start_print("cube.gcode");
        CHECK(r.status == Status::Busy);
        CHECK(r.message.find("benchy.gcode") != std::string::npos);
        CHECK(printer->count("POST") == 0);
    }
    SECTION("nor one that is paused")
    {
        printer->state = "paused";
        CHECK(files.start_print("cube.gcode").status == Status::Busy);
        CHECK(printer->count("POST") == 0);
    }
    SECTION("a finished or cancelled job does not block a new one")
    {
        printer->state = "complete";
        CHECK(files.start_print("cube.gcode").ok());
        printer->state = "cancelled";
        CHECK(files.start_print("benchy.gcode").ok());
    }
    SECTION("only G-code is started")
    {
        printer->files["plate.gcode.3mf"] = { 10, 1, 0, 0, 0, false };
        CHECK(files.start_print("plate.gcode.3mf").status == Status::NotFound);
        CHECK(printer->log.empty());
    }
    SECTION("the printer's own refusal comes through")
    {
        const Result r = files.start_print("gone.gcode");
        CHECK(r.status == Status::Failed);
        CHECK(r.message.find("File not found: gone.gcode") != std::string::npos);
    }
}

TEST_CASE("Storage: what a failure says", "[PrinterStorage]")
{
    auto           printer = sample_printer();
    MoonrakerFiles files(printer);
    std::vector<FileEntry> list;

    SECTION("nothing answers")
    {
        printer->offline = true;
        const Result r   = files.list(list);
        CHECK(r.status == Status::Offline);
        CHECK(r.message.find("did not answer") != std::string::npos);
    }
    SECTION("an OctoPrint (or anything else) at a Klipper preset's address")
    {
        printer->list_status = 404;
        CHECK(files.list(list).status == Status::NotSupported);
    }
    SECTION("an API key is needed")
    {
        printer->require_key = "SECRET";
        const Result r       = files.list(list);
        CHECK(r.status == Status::Unauthorized);
        CHECK(r.message.find("API key") != std::string::npos);
        INFO("the key itself never ends up in a message");
        CHECK(r.message.find("SECRET") == std::string::npos);
    }
    SECTION("result_of, piece by piece")
    {
        CHECK(result_of(HttpReply{ 204, "", "" }, "x").ok());
        CHECK(result_of(HttpReply{ 403, "{}", "" }, "x").status == Status::Unauthorized);
        CHECK(result_of(HttpReply{ 500, "<html>Internal</html>", "" }, "x").message == "The printer could not x: HTTP 500");
        CHECK(result_of(HttpReply{ 503, R"({"error": "Klippy Disconnected"})", "" }, "x").message == "The printer could not x: Klippy Disconnected");
    }
}

// ---- the file-grid model ------------------------------------------------------------------------

TEST_CASE("Storage grid: list, order and details", "[PrinterStorage][FileGrid]")
{
    auto printer = sample_printer();
    auto exec    = std::make_shared<InlineExecutor>();
    RemoteFileGridModel model(std::make_shared<MoonrakerFiles>(printer), exec);

    std::vector<std::pair<FileGridChange, std::string>> changes;
    model.SetChangeHandler([&changes](FileGridChange c, const std::string& id) { changes.emplace_back(c, id); });

    CHECK(model.GetStatus() == FileGridStatus::Idle);
    model.Refresh();
    CHECK(model.GetStatus() == FileGridStatus::Ready);
    REQUIRE(model.GetCount() == 3);
    INFO("newest first");
    CHECK(model.GetCard(0)->id == "benchy.gcode");
    CHECK(model.GetCard(1)->id == "parts/bracket.gcode");
    CHECK(model.GetCard(2)->id == "cube.gcode");
    CHECK(model.GetCard(3) == nullptr);
    CHECK(model.GetCard(0)->printable);
    CHECK(model.GetCard(0)->size_text() == "1.4 MB");
    CHECK_FALSE(model.GetCard(0)->details_loaded);
    CHECK(model.IndexOf("cube.gcode") == 2);
    CHECK(model.IndexOf("nope.gcode") == 3);
    CHECK(printer->count("GET /server/files/metadata") == 0); // nothing until a view shows rows

    INFO("the rows on screen get their details and pictures");
    changes.clear();
    model.SetFocusRange(0, 2);
    const auto* benchy = model.FindCard("benchy.gcode");
    REQUIRE(benchy);
    CHECK(benchy->details_loaded);
    CHECK(benchy->meta.estimated_time_s == 4980);
    CHECK(benchy->print_time_text() == "1h 23m");
    CHECK(benchy->filament_text() == "4.56 m, 14 g");
    CHECK(benchy->thumbnail_state == ThumbnailState::Ready);
    REQUIRE(benchy->thumbnail);
    CHECK(*benchy->thumbnail == MockMoonraker::png_for(".thumbs/benchy-300x300.png"));
    const auto* bracket = model.FindCard("parts/bracket.gcode");
    CHECK(bracket->details_loaded);
    CHECK(bracket->thumbnail_state == ThumbnailState::Missing); // no picture in that file
    CHECK_FALSE(model.FindCard("cube.gcode")->details_loaded);
    CHECK(changes.size() == 2);
    CHECK(changes[0].first == FileGridChange::Card);
    CHECK(changes[0].second == "benchy.gcode");

    INFO("asking again for rows that are loaded costs nothing");
    const size_t requests = printer->log.size();
    model.SetFocusRange(0, 2);
    CHECK(printer->log.size() == requests);

    INFO("sorting");
    model.SetSort(FileGridSort::Name);
    CHECK(model.GetCard(0)->id == "benchy.gcode");
    CHECK(model.GetCard(1)->id == "cube.gcode");
    CHECK(model.GetCard(2)->id == "parts/bracket.gcode");
    model.SetSort(FileGridSort::Size);
    CHECK(model.GetCard(0)->id == "benchy.gcode");
    CHECK(model.GetCard(2)->id == "cube.gcode");
    model.SetSort(FileGridSort::Oldest);
    CHECK(model.GetCard(0)->id == "cube.gcode");

    INFO("a refresh keeps what was fetched for a file that did not change, and drops a changed one");
    printer->files["benchy.gcode"].modified = 1760009999.0; // re-uploaded
    model.Refresh();
    CHECK_FALSE(model.FindCard("benchy.gcode")->details_loaded);
    CHECK(model.FindCard("parts/bracket.gcode")->details_loaded);
    CHECK(model.GetSort() == FileGridSort::Oldest);
}

TEST_CASE("Storage grid: delete and print", "[PrinterStorage][FileGrid]")
{
    auto printer = sample_printer();
    auto exec    = std::make_shared<InlineExecutor>();
    RemoteFileGridModel model(std::make_shared<MoonrakerFiles>(printer), exec);
    model.Refresh();
    REQUIRE(model.GetCount() == 3);
    CHECK(model.CanDelete());
    CHECK(model.CanPrint());

    Result got;
    model.DeleteFile("cube.gcode", [&got](const Result& r) { got = r; });
    CHECK(got.ok());
    CHECK(model.GetCount() == 2);
    CHECK(model.FindCard("cube.gcode") == nullptr);
    CHECK(printer->count("DELETE /server/files/gcodes/cube.gcode") == 1);

    INFO("a file somebody else deleted meanwhile just goes away");
    printer->files.erase("benchy.gcode");
    model.DeleteFile("benchy.gcode", [&got](const Result& r) { got = r; });
    CHECK(got.status == Status::NotFound);
    CHECK(model.GetCount() == 1);

    INFO("printing: the state first, then the start");
    std::string state;
    model.QueryPrinter([&](const Result& r, const std::string& s, const std::string&) {
        got   = r;
        state = s;
    });
    CHECK(got.ok());
    CHECK(state == "standby");
    model.StartPrint("parts/bracket.gcode", [&got](const Result& r) { got = r; });
    CHECK(got.ok());
    CHECK(printer->current == "parts/bracket.gcode");
    INFO("a second start while it prints is refused");
    model.StartPrint("parts/bracket.gcode", [&got](const Result& r) { got = r; });
    CHECK(got.status == Status::Busy);
    CHECK(printer->count("POST /printer/print/start") == 1);

    INFO("the last file deleted: Empty");
    printer->state = "standby";
    model.DeleteFile("parts/bracket.gcode", [&got](const Result& r) { got = r; });
    CHECK(model.GetCount() == 0);
    CHECK(model.GetStatus() == FileGridStatus::Empty);
    model.StartPrint("parts/bracket.gcode", [&got](const Result& r) { got = r; });
    CHECK(got.status == Status::NotFound);
}

TEST_CASE("Storage grid: failures and printers without a file API", "[PrinterStorage][FileGrid]")
{
    SECTION("no backend: NotSupported with the reason, and no request ever")
    {
        RemoteFileGridModel model(nullptr, std::make_shared<InlineExecutor>(), "This printer is reached through Elegoo Link (SDCP).");
        CHECK(model.GetStatus() == FileGridStatus::NotSupported);
        model.Refresh();
        CHECK(model.GetStatus() == FileGridStatus::NotSupported);
        CHECK(model.GetMessage().find("SDCP") != std::string::npos);
        CHECK_FALSE(model.CanDelete());
        CHECK_FALSE(model.CanPrint());
        Result got;
        model.DeleteFile("a.gcode", [&got](const Result& r) { got = r; });
        CHECK(got.status == Status::NotSupported);
        model.StartPrint("a.gcode", [&got](const Result& r) { got = r; });
        CHECK(got.status == Status::NotSupported);
    }
    SECTION("an offline printer: Failed, and the old list is not left on screen")
    {
        auto printer = sample_printer();
        RemoteFileGridModel model(std::make_shared<MoonrakerFiles>(printer), std::make_shared<InlineExecutor>());
        model.Refresh();
        REQUIRE(model.GetCount() == 3);
        printer->offline = true;
        model.Refresh();
        CHECK(model.GetStatus() == FileGridStatus::Failed);
        CHECK(model.GetCount() == 0);
        CHECK(model.GetMessage().find("did not answer") != std::string::npos);
        printer->offline = false;
        model.Refresh();
        CHECK(model.GetStatus() == FileGridStatus::Ready);
    }
    SECTION("not a Moonraker: NotSupported")
    {
        auto printer         = sample_printer();
        printer->list_status = 404;
        RemoteFileGridModel model(std::make_shared<MoonrakerFiles>(printer), std::make_shared<InlineExecutor>());
        model.Refresh();
        CHECK(model.GetStatus() == FileGridStatus::NotSupported);
    }
    SECTION("an empty printer")
    {
        auto printer = std::make_shared<MockMoonraker>();
        RemoteFileGridModel model(std::make_shared<MoonrakerFiles>(printer), std::make_shared<InlineExecutor>());
        model.Refresh();
        CHECK(model.GetStatus() == FileGridStatus::Empty);
    }
}

TEST_CASE("Storage grid: a slow printer", "[PrinterStorage][FileGrid]")
{
    auto printer = std::make_shared<MockMoonraker>();
    for (int i = 0; i < 10; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "part_%02d.gcode", i);
        printer->files[name] = { 1000, 1760000000.0 + i, 60, 10, 1, true };
    }
    auto exec = std::make_shared<QueuedExecutor>();
    auto model = std::make_unique<RemoteFileGridModel>(std::make_shared<MoonrakerFiles>(printer), exec);

    INFO("only the newest list answer counts");
    model->Refresh();
    REQUIRE(exec->jobs.size() == 1);
    exec->jobs.front().first(); // the printer lists 10 files, but the answer is slow to come back
    printer->files.erase("part_09.gcode");
    model->Refresh();
    CHECK(model->GetStatus() == FileGridStatus::Loading);
    REQUIRE(exec->jobs.size() == 2);
    exec->jobs.back().first();
    exec->jobs.back().second();
    REQUIRE(model->GetCount() == 9);
    exec->jobs.front().second(); // ... and is dropped
    CHECK(model->GetCount() == 9);
    exec->jobs.clear();

    INFO("at most MAX_IN_FLIGHT detail requests at a time, the rows on screen first");
    model->SetFocusRange(0, 9);
    CHECK(model->in_flight() == RemoteFileGridModel::MAX_IN_FLIGHT);
    CHECK(exec->jobs.size() == RemoteFileGridModel::MAX_IN_FLIGHT);
    CHECK(model->queued() == 9 - RemoteFileGridModel::MAX_IN_FLIGHT);
    CHECK(model->GetCard(0)->thumbnail_state == ThumbnailState::Loading);
    CHECK(model->GetCard(8)->thumbnail_state == ThumbnailState::None);
    model->SetFocusRange(8, 1); // scrolled to the bottom: that row jumps the queue
    exec->step();
    CHECK(model->in_flight() == RemoteFileGridModel::MAX_IN_FLIGHT);
    CHECK(model->GetCard(8)->thumbnail_state == ThumbnailState::Loading);
    exec->drain();
    CHECK(model->in_flight() == 0);
    for (size_t i = 0; i < model->GetCount(); ++i) {
        CHECK(model->GetCard(i)->details_loaded);
        CHECK(model->GetCard(i)->thumbnail_state == ThumbnailState::Ready);
    }
    CHECK(printer->count("GET /server/files/metadata") == 9);

    INFO("answers arriving after the view (and its model) went away are dropped");
    model->Refresh();
    model->DeleteFile("part_00.gcode", nullptr);
    REQUIRE(exec->jobs.size() == 2);
    model.reset();
    exec->drain(); // must not touch the dead model
    CHECK(printer->files.count("part_00.gcode") == 0); // the request itself still went out
}

TEST_CASE("Storage: display text", "[PrinterStorage]")
{
    CHECK(format_duration(0).empty());
    CHECK(format_duration(-5).empty());
    CHECK(format_duration(30) == "< 1m");
    CHECK(format_duration(45 * 60) == "45m");
    CHECK(format_duration(4980) == "1h 23m");
    CHECK(format_duration(2 * 86400 + 3 * 3600 + 59) == "2d 3h");
    CHECK(format_size(512) == "512 B");
    CHECK(format_size(1536) == "1.5 KB");
    CHECK(format_size(12900000) == "12.3 MB");
    CHECK(format_size(1288490189ull) == "1.2 GB");
    CHECK(format_filament(0, 0).empty());
    CHECK(format_filament(4560, 0) == "4.56 m");
    CHECK(format_filament(0, 13.6) == "14 g");
    CHECK(format_filament(0, 2.44) == "2.4 g");
    CHECK(format_filament(4560, 13.6) == "4.56 m, 14 g");
}

// ---- end to end (hidden) ------------------------------------------------------------------------

namespace {

bool fake_ports(int& port, int& control)
{
    const char* v = std::getenv("FAKE_MOONRAKER_A");
    return v && std::sscanf(v, "%d:%d", &port, &control) == 2 && port > 0 && control > 0;
}

json fake_get(int control, const std::string& what)
{
    std::string body;
    Http::get("http://127.0.0.1:" + std::to_string(control) + "/fake/" + what)
        .timeout_connect(2)
        .timeout_max(5)
        .on_complete([&body](std::string b, unsigned) { body = b; })
        .perform_sync();
    REQUIRE_FALSE(body.empty());
    return json::parse(body);
}

} // namespace

TEST_CASE("Storage end to end against fake_moonraker.py", "[.][PrinterStorageE2E]")
{
    int port = 0, control = 0;
    REQUIRE(fake_ports(port, control));
    fake_get(control, "reset");
    fake_get(control, "add?name=cube.gcode&size=2048");
    fake_get(control, "add?name=parts%2Fbracket%20v2.gcode&size=4096");

    const Target target = target_for_snapmaker("127.0.0.1", port, "fake", "sm:fake");
    REQUIRE(target.supported());
    auto files = make_files(target);
    REQUIRE(files);

    std::vector<FileEntry> list;
    REQUIRE(files->list(list).ok());
    CHECK(list.size() == 2);

    FileMetadata meta;
    REQUIRE(files->metadata("parts/bracket v2.gcode", meta).ok());
    CHECK(meta.estimated_time_s == 4980);
    REQUIRE(meta.thumbnails.size() == 1);
    CHECK(meta.thumbnails[0].path == "parts/.thumbs/bracket v2-32x32.png");
    std::string png;
    REQUIRE(files->thumbnail(meta.thumbnails[0].path, png).ok());
    CHECK(png.compare(0, 4, "\x89PNG") == 0);

    REQUIRE(files->remove("parts/bracket v2.gcode").ok());
    CHECK(fake_get(control, "files")["deleted"][0] == "parts/bracket v2.gcode");
    CHECK(files->remove("parts/bracket v2.gcode").status == Status::NotFound);

    REQUIRE(files->start_print("cube.gcode").ok());
    CHECK(fake_get(control, "files")["started"][0] == "cube.gcode");
    CHECK(files->start_print("cube.gcode").status == Status::Busy);

    fake_get(control, "mode?m=down");
    CHECK(files->list(list).status == Status::Offline);
    fake_get(control, "mode?m=up");
}
