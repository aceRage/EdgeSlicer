// The Storage tab's Elegoo Centauri Carbon 1 backend (Utils/ElegooSdcpFiles): the SDCP file list
// (Cmd 258), delete (Cmd 259) and the start of a stored file (Cmd 128), with no printer and no socket.
//
// The answer fixtures follow the shapes the public sources show - pycentauri's verified CC1 answer
// (Data.Data.FileList of {name "/local/...", FileSize, TotalLayers, CreateTime}) and the SDCP V3.0.0
// specification's (name, type, usedSize, totalSize, storageType) - with made-up ids, names and
// numbers. The backend runs over a scripted fake channel that records what would be sent.

#include <catch2/catch.hpp>

#include "slic3r/GUI/Printer/RemoteFileGridModel.h"
#include "slic3r/Utils/ElegooSdcpFiles.hpp"
#include "slic3r/Utils/ElegooStartPrint.hpp"
#include "slic3r/Utils/PrinterStorage.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sf     = Slic3r::SdcpFiles;
namespace pstore = Slic3r::PrinterStorage;
namespace phd    = Slic3r::PrintHostDevices;
using nlohmann::json;

namespace {

constexpr int SF_PC_BED = 2; // stands in for BedType::btPC: only "same number or not" matters

// What a CC1 answers to Cmd 258, in pycentauri's verified shape, plus the entries the parser must skip.
const char* const SF_LIST_ANSWER = R"({
  "Id": "", "Topic": "sdcp/response/0000",
  "Data": {
    "Cmd": 258, "RequestID": "req-1", "MainboardID": "0000", "TimeStamp": 1729000000,
    "Data": {
      "Ack": 0,
      "FileList": [
        {"name": "/local/cube.gcode", "FileSize": 123456, "TotalLayers": 150, "CreateTime": 1729000000, "type": 1},
        {"name": "/local/old_prints", "type": 0},
        {"name": "/local/benchy PLA.gcode", "FileSize": "2048", "TotalLayers": "240", "CreateTime": 1729000500123},
        {"name": "/usb/stick.gcode", "FileSize": 1},
        {"name": "/local/.hidden.gcode", "FileSize": 1},
        {"name": "/local/notes.txt", "FileSize": 1},
        {"FileSize": 5},
        "not an object"
      ]
    }
  }
})";

// The specification's example shape: bare names, storage sizes that are not file sizes, no times.
const char* const SF_SPEC_ANSWER = R"({"Id":"","Data":{"Cmd":192,"Data":{"Ack":0,"FileList":[
  {"name":"part.gcode","usedSize":999999,"totalSize":8000000,"storageType":0,"type":1}
]},"RequestID":"req-2","MainboardID":"0000","TimeStamp":1}})";

std::string sf_list_answer_with(const std::vector<std::string>& names)
{
    json list = json::array();
    for (const std::string& n : names)
        list.push_back({ { "name", "/local/" + n }, { "FileSize", 100 }, { "TotalLayers", 10 }, { "CreateTime", 1729000000 } });
    json j;
    j["Data"]["Cmd"]          = 258;
    j["Data"]["Data"]["Ack"]  = 0;
    j["Data"]["Data"]["FileList"] = list;
    return j.dump();
}

std::string sf_ack_answer(int cmd, int ack, const json& extra = json::object())
{
    json j;
    j["Data"]["Cmd"]         = cmd;
    j["Data"]["Data"]        = extra;
    j["Data"]["Data"]["Ack"] = ack;
    return j.dump();
}

struct SfSent
{
    int  cmd;
    json data;
};

// A printer that answers from a script. `on_command` returns the reply; every command is recorded.
class SfFakeChannel : public sf::SdcpChannel
{
public:
    std::vector<SfSent>                         sent;
    std::function<sf::Reply(int, const json&)> on_command;
    sf::PrinterRead                             state;   // read_state()
    sf::PrinterRead                             printer; // read_printer()
    int                                         state_reads { 0 }, printer_reads { 0 };

    sf::Reply command(int cmd, const json& data, int) override
    {
        sent.push_back({ cmd, data });
        if (on_command)
            return on_command(cmd, data);
        sf::Reply r;
        r.error = "no script";
        return r;
    }
    sf::PrinterRead read_state() override
    {
        ++state_reads;
        return state;
    }
    sf::PrinterRead read_printer() override
    {
        ++printer_reads;
        return printer;
    }
};

sf::Reply sf_answered(const std::string& frame)
{
    sf::Reply r;
    r.sent     = true;
    r.answered = true;
    r.frame    = frame;
    r.ack      = sf::ack_of(frame);
    return r;
}

sf::PrinterRead sf_idle(bool canvas = false, bool loaded = false)
{
    sf::PrinterRead p;
    p.reached       = true;
    p.state         = "standby";
    p.status.probed = p.status.online = true;
    p.status.state  = "standby";
    p.status.canvas = canvas;
    if (canvas) {
        phd::Slot s;
        s.index  = 0;
        s.loaded = loaded;
        s.type   = loaded ? "PLA" : "";
        p.status.slots.push_back(s);
        p.status.slots_have_filaments = loaded;
    }
    return p;
}

sf::PrinterRead sf_printing(const std::string& file)
{
    sf::PrinterRead p = sf_idle();
    p.state           = "printing";
    p.status.state    = "printing";
    p.filename        = file;
    return p;
}

std::map<std::string, std::string> sf_options()
{
    return { { "timeLapse", "1" }, { "heatedBedLeveling", "0" }, { "bedType", std::to_string(SF_PC_BED) } };
}

class SfInlineExecutor : public Slic3r::GUI::FileGridExecutor
{
public:
    void run(std::function<void()> work, std::function<void()> then) override
    {
        work();
        then();
    }
};

bool sf_logged(const std::string& needle)
{
    for (const std::string& l : sf::logged_for_tests())
        if (l.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE("CC1 storage: the request bodies are exactly the verified ones", "[ElegooSdcpFiles]")
{
    CHECK(sf::CMD_FILE_LIST == 258);
    CHECK(sf::CMD_DELETE_FILES == 259);
    CHECK(sf::list_request_data().dump() == R"({"Url":"/local"})");
    CHECK(sf::delete_request_data({ "cube.gcode" }).dump() == R"({"FileList":["/local/cube.gcode"]})");
    CHECK(sf::delete_request_data({ "a b.gcode", "c.gcode" }).dump() == R"({"FileList":["/local/a b.gcode","/local/c.gcode"]})");
    CHECK(sf::full_path("x.gcode") == "/local/x.gcode");
}

TEST_CASE("CC1 storage: printer paths relative to the storage root", "[ElegooSdcpFiles]")
{
    CHECK(sf::relative_path("/local/cube.gcode") == "cube.gcode");
    CHECK(sf::relative_path("local/cube.gcode") == "cube.gcode");
    CHECK(sf::relative_path("cube.gcode") == "cube.gcode");
    CHECK(sf::relative_path(" /local/cube.gcode ") == "cube.gcode");
    CHECK(sf::relative_path("/usb/cube.gcode").empty());
    CHECK(sf::relative_path("/local").empty());
    CHECK(sf::relative_path("/local/../etc/passwd").empty());
    CHECK(sf::relative_path("").empty());
    CHECK(sf::same_file("/local/cube.gcode", "cube.gcode"));
    CHECK_FALSE(sf::same_file("/local/cube.gcode", "cube2.gcode"));
    CHECK_FALSE(sf::same_file("", ""));
}

TEST_CASE("CC1 storage: the file list answer", "[ElegooSdcpFiles]")
{
    SECTION("pycentauri's shape, with what must be skipped")
    {
        const sf::ListAnswer a = sf::parse_list_answer(SF_LIST_ANSWER);
        REQUIRE(a.parsed);
        CHECK(a.ack == 0);
        REQUIRE(a.files.size() == 2);
        CHECK(a.files[0].entry.path == "cube.gcode");
        CHECK(a.files[0].entry.name == "cube.gcode");
        CHECK(a.files[0].entry.size == 123456);
        CHECK(a.files[0].entry.modified == 1729000000);
        CHECK(a.files[0].total_layers == 150);
        INFO("numbers as strings, CreateTime in milliseconds");
        CHECK(a.files[1].entry.path == "benchy PLA.gcode");
        CHECK(a.files[1].entry.size == 2048);
        CHECK(a.files[1].total_layers == 240);
        CHECK(a.files[1].entry.modified == 1729000500);
        CHECK(a.folders == 1);
        CHECK(a.other_storage == 1);
        CHECK(a.unlisted == 4); // .hidden, notes.txt, no name, not an object
        CHECK(a.names_prefixed);
        CHECK(a.create_time_form == "seconds");
        CHECK(a.first_entry_keys == std::vector<std::string>{ "CreateTime", "FileSize", "TotalLayers", "name", "type" });
    }
    SECTION("the specification's shape: usedSize is the storage's, never the file's size")
    {
        const sf::ListAnswer a = sf::parse_list_answer(SF_SPEC_ANSWER);
        REQUIRE(a.parsed);
        REQUIRE(a.files.size() == 1);
        CHECK(a.files[0].entry.path == "part.gcode");
        CHECK(a.files[0].entry.size == 0);
        CHECK(a.files[0].entry.modified == 0);
        CHECK_FALSE(a.names_prefixed);
        CHECK(a.create_time_form == "absent");
    }
    SECTION("the list one level up, or at the top")
    {
        const sf::ListAnswer a = sf::parse_list_answer(R"({"Data":{"Cmd":258,"Ack":0,"FileList":[{"name":"/local/a.gcode"}]}})");
        REQUIRE(a.parsed);
        CHECK(a.ack == 0);
        REQUIRE(a.files.size() == 1);
        const sf::ListAnswer b = sf::parse_list_answer(R"({"FileList":[{"name":"b.gcode","CreateTime":"1729000000"}]})");
        REQUIRE(b.parsed);
        REQUIRE(b.files.size() == 1);
        CHECK(b.files[0].entry.modified == 1729000000);
        CHECK(b.create_time_form == "string");
    }
    SECTION("an unreadable CreateTime is no time")
    {
        const sf::ListAnswer a = sf::parse_list_answer(R"({"FileList":[{"name":"a.gcode","CreateTime":"2024-10-15 10:00"}]})");
        REQUIRE(a.files.size() == 1);
        CHECK(a.files[0].entry.modified == 0);
        CHECK(a.create_time_form == "other");
    }
    SECTION("no list")
    {
        const sf::ListAnswer a = sf::parse_list_answer(sf_ack_answer(258, 3));
        CHECK_FALSE(a.parsed);
        CHECK(a.ack == 3);
        CHECK_FALSE(sf::parse_list_answer("pong").parsed);
        CHECK_FALSE(sf::parse_list_answer("").parsed);
        CHECK_FALSE(sf::parse_list_answer("[1,2]").parsed);
    }
}

TEST_CASE("CC1 storage: the delete answer and Acks", "[ElegooSdcpFiles]")
{
    const sf::DeleteAnswer ok = sf::parse_delete_answer(sf_ack_answer(259, 0));
    CHECK(ok.parsed);
    CHECK(ok.ack == 0);
    CHECK(ok.failed.empty());
    const sf::DeleteAnswer bad = sf::parse_delete_answer(sf_ack_answer(259, 0, json{ { "ErrData", { "/local/cube.gcode", "/usb/x" } } }));
    REQUIRE(bad.failed.size() == 2);
    CHECK(bad.failed[0] == "cube.gcode");
    CHECK(bad.failed[1] == "/usb/x");
    CHECK_FALSE(sf::parse_delete_answer("nope").parsed);
    CHECK(sf::ack_of("nope") == -1);
    CHECK(sf::ack_of(sf_ack_answer(128, 2)) == 2);
    CHECK(sf::start_ack_text(0).empty());
    CHECK(sf::start_ack_text(1).find("busy") != std::string::npos);
    CHECK(sf::start_ack_text(7).find("another printer model") != std::string::npos);
    CHECK(sf::start_ack_text(42).find("Ack 42") != std::string::npos);
}

TEST_CASE("CC1 storage: the start of a stored file follows the send's rules", "[ElegooSdcpFiles]")
{
    using P = phd::SlotSendPolicy;
    SECTION("a printer that did not answer is never started")
    {
        phd::Status st;
        st.error              = "timeout";
        const sf::StartPlan p = sf::plan_start(st, sf_options(), SF_PC_BED, "CC");
        CHECK_FALSE(p.may_start);
        CHECK(p.refusal.find("did not answer") != std::string::npos);
    }
    SECTION("printing or paused: not started")
    {
        const sf::StartPlan p = sf::plan_start(sf_printing("x.gcode").status, sf_options(), SF_PC_BED, "CC");
        CHECK(p.policy == P::UploadOnlyBusy);
        CHECK_FALSE(p.may_start);
        CHECK(p.refusal.find("printing") != std::string::npos);
    }
    SECTION("no CANVAS: started, the command keeps its old shape")
    {
        const sf::StartPlan p = sf::plan_start(sf_idle().status, sf_options(), SF_PC_BED, "CC");
        CHECK(p.policy == P::Start);
        CHECK(p.may_start);
        CHECK_FALSE(p.as_sliced_confirmation);
        CHECK(p.extended_info.count("canvasAttached") == 0);
        CHECK(p.advice.find("Timelapse: on") != std::string::npos);
        CHECK(p.advice.find("Bed leveling before printing: off") != std::string::npos);
        CHECK(p.advice.find("smooth PC side") != std::string::npos);
        CHECK(p.advice.find("CANVAS") == std::string::npos);
    }
    SECTION("a CANVAS without loaded trays: started, an empty slot_map as the dialog sends it")
    {
        const sf::StartPlan p = sf::plan_start(sf_idle(true, false).status, sf_options(), SF_PC_BED, "CC");
        CHECK(p.policy == P::Start);
        CHECK(p.may_start);
        CHECK(p.extended_info.at("canvasAttached") == "1");
        CHECK(p.extended_info.at("slotMap").empty());
    }
    SECTION("a CANVAS with loaded trays: nothing to map from, the person is told the printer picks")
    {
        const sf::StartPlan p = sf::plan_start(sf_idle(true, true).status, sf_options(), SF_PC_BED, "CC");
        CHECK(p.policy == P::NeedsMapping);
        CHECK(p.may_start);
        CHECK(p.as_sliced_confirmation);
        CHECK(p.extended_info.at("canvasAttached") == "1");
        CHECK(p.extended_info.at("slotMap").empty());
        CHECK(p.advice.find("CANVAS") != std::string::npos);
        CHECK(p.advice.find("As sliced") != std::string::npos);
    }
    SECTION("no remembered choices: off, and the textured side")
    {
        const sf::StartPlan p = sf::plan_start(sf_idle().status, {}, SF_PC_BED, "CC");
        CHECK(p.advice.find("Timelapse: off") != std::string::npos);
        CHECK(p.advice.find("textured PTE side") != std::string::npos);
    }
}

TEST_CASE("CC1 storage: Cmd 128 from storage is the Upload and Print command", "[ElegooSdcpFiles]")
{
    const sf::StartPlan plain = sf::plan_start(sf_idle().status, sf_options(), SF_PC_BED, "CC");
    const json          d     = sf::start_request_data(plain.extended_info, SF_PC_BED, "cube.gcode");
    CHECK(d.at("Filename") == "/local/cube.gcode");
    CHECK(d.at("StartLayer") == 0);
    CHECK(d.at("Calibration_switch") == 0);
    CHECK(d.at("PrintPlatformType") == 1);
    CHECK(d.at("Tlp_Switch") == 1);
    CHECK_FALSE(d.contains("slot_map"));
    // Byte for byte what ElegooLink's send builds for the same choices.
    const json send = json::parse(Slic3r::ElegooStartPrint::build_payload(plain.extended_info, SF_PC_BED, "cube.gcode", "r", 1));
    CHECK(send["Data"]["Data"] == d);

    const sf::StartPlan canvas = sf::plan_start(sf_idle(true, true).status, sf_options(), SF_PC_BED, "CC");
    const json          dc     = sf::start_request_data(canvas.extended_info, SF_PC_BED, "cube.gcode");
    REQUIRE(dc.contains("slot_map"));
    CHECK(dc.at("slot_map") == json::array());
}

TEST_CASE("CC1 storage: the backend lists, describes and deletes", "[ElegooSdcpFiles]")
{
    sf::reset_logged_for_tests();
    auto ch = std::make_shared<SfFakeChannel>();
    std::vector<std::string> on_printer{ "cube.gcode", "junk.gcode" };
    int                      delete_ack = 0;
    json                     delete_extra = json::object();
    ch->on_command = [&](int cmd, const json& data) -> sf::Reply {
        if (cmd == 258)
            return sf_answered(sf_list_answer_with(on_printer));
        if (cmd == 259) {
            if (delete_ack == 0 && delete_extra.empty())
                for (const json& p : data.at("FileList"))
                    on_printer.erase(std::remove(on_printer.begin(), on_printer.end(), sf::relative_path(p.get<std::string>())),
                                     on_printer.end());
            return sf_answered(sf_ack_answer(259, delete_ack, delete_extra));
        }
        return sf::Reply();
    };
    ch->state = sf_idle();
    sf::Cc1Files files(ch, sf_options(), SF_PC_BED, "CC");

    std::vector<pstore::FileEntry> list;
    REQUIRE(files.list(list).ok());
    REQUIRE(list.size() == 2);
    CHECK(ch->sent.back().data.dump() == R"({"Url":"/local"})");
    CHECK(sf_logged("Cmd 258 answered: Ack 0, 2 listed"));

    pstore::FileMetadata meta;
    REQUIRE(files.metadata("cube.gcode", meta).ok());
    CHECK(meta.valid);
    CHECK(meta.layer_count == 10);
    CHECK(meta.thumbnails.empty());
    CHECK(files.metadata("gone.gcode", meta).status == pstore::Status::NotFound);
    std::string bytes;
    CHECK(files.thumbnail("cube.gcode", bytes).status == pstore::Status::NotFound);

    SECTION("a delete goes out as the full path, and is checked by listing again")
    {
        const size_t before = ch->sent.size();
        REQUIRE(files.remove("junk.gcode").ok());
        REQUIRE(ch->sent.size() == before + 2);
        CHECK(ch->sent[before].cmd == 259);
        CHECK(ch->sent[before].data.dump() == R"({"FileList":["/local/junk.gcode"]})");
        CHECK(ch->sent[before + 1].cmd == 258);
        CHECK(sf_logged("Cmd 259 answered: Ack 0, ErrData empty or absent"));
    }
    SECTION("the file being printed is never deleted, and no Cmd 259 goes out")
    {
        ch->state          = sf_printing("/local/junk.gcode");
        const size_t before = ch->sent.size();
        CHECK(files.remove("junk.gcode").status == pstore::Status::Busy);
        CHECK(ch->sent.size() == before);
        INFO("another file may go while it prints");
        CHECK(files.remove("cube.gcode").ok());
    }
    SECTION("an Ack that changes nothing is reported, not hidden")
    {
        delete_extra = json{ { "Note", "x" } }; // the fake keeps the file
        const pstore::Result r = files.remove("junk.gcode");
        CHECK(r.status == pstore::Status::Failed);
        CHECK(r.message.find("still lists") != std::string::npos);
        CHECK(sf_logged("still listed"));
    }
    SECTION("a refusal and an ErrData entry fail the delete")
    {
        delete_ack = 1;
        CHECK(files.remove("junk.gcode").status == pstore::Status::Failed);
        delete_ack   = 0;
        delete_extra = json{ { "ErrData", { "/local/junk.gcode" } } };
        CHECK(files.remove("junk.gcode").status == pstore::Status::Failed);
    }
    SECTION("a printer that cannot be asked is not sent a delete")
    {
        ch->state           = sf::PrinterRead();
        const size_t before = ch->sent.size();
        CHECK(files.remove("junk.gcode").status == pstore::Status::Offline);
        CHECK(ch->sent.size() == before);
    }
    SECTION("an invalid path never reaches the printer")
    {
        const size_t before = ch->sent.size();
        CHECK(files.remove("../x.gcode").status == pstore::Status::NotFound);
        CHECK(ch->sent.size() == before);
    }
}

TEST_CASE("CC1 storage: the list failing", "[ElegooSdcpFiles]")
{
    sf::reset_logged_for_tests();
    auto ch = std::make_shared<SfFakeChannel>();
    sf::Cc1Files files(ch, {}, SF_PC_BED, "CC");
    std::vector<pstore::FileEntry> list;

    ch->on_command = [](int, const json&) {
        sf::Reply r;
        r.error = "another EdgeSlicer window is connected to this printer";
        return r;
    };
    pstore::Result r = files.list(list);
    CHECK(r.status == pstore::Status::Offline);
    CHECK(r.message.find("another EdgeSlicer window") != std::string::npos);

    ch->on_command = [](int, const json&) {
        sf::Reply r;
        r.sent  = true;
        r.error = "the printer did not answer in time";
        return r;
    };
    CHECK(files.list(list).status == pstore::Status::Failed);
    CHECK(sf_logged("Cmd 258 (file list) got no answer"));

    ch->on_command = [](int, const json&) { return sf_answered(sf_ack_answer(258, 0)); };
    CHECK(files.list(list).status == pstore::Status::NotSupported);
    CHECK(sf_logged("no FileList"));

    ch->on_command = [](int, const json&) { return sf_answered(sf_ack_answer(258, 2)); };
    CHECK(files.list(list).status == pstore::Status::Failed);
}

TEST_CASE("CC1 storage: a start from storage needs the confirmation it was shown", "[ElegooSdcpFiles]")
{
    auto ch = std::make_shared<SfFakeChannel>();
    int  start_ack = 0;
    ch->on_command = [&](int cmd, const json&) -> sf::Reply {
        if (cmd == 128)
            return sf_answered(sf_ack_answer(128, start_ack));
        return sf::Reply();
    };
    ch->printer = sf_idle();
    sf::Cc1Files files(ch, sf_options(), SF_PC_BED, "CC");
    std::string state, current, advice;

    SECTION("never without a check first")
    {
        CHECK(files.start_print("cube.gcode").status == pstore::Status::Failed);
        CHECK(ch->sent.empty());
    }
    SECTION("checked, then started with the send's command")
    {
        REQUIRE(files.start_check("cube.gcode", state, current, advice).ok());
        CHECK(state == "standby");
        CHECK(advice.find("Timelapse: on") != std::string::npos);
        REQUIRE(files.start_print("cube.gcode").ok());
        REQUIRE(ch->sent.size() == 1);
        CHECK(ch->sent[0].cmd == 128);
        CHECK(ch->sent[0].data.at("Filename") == "/local/cube.gcode");
        CHECK_FALSE(ch->sent[0].data.contains("slot_map"));
        INFO("one confirmation, one start");
        CHECK(files.start_print("cube.gcode").status == pstore::Status::Failed);
        CHECK(ch->sent.size() == 1);
    }
    SECTION("a CANVAS with loaded trays: the confirmation says so, an empty slot_map goes out")
    {
        ch->printer = sf_idle(true, true);
        REQUIRE(files.start_check("cube.gcode", state, current, advice).ok());
        CHECK(advice.find("CANVAS") != std::string::npos);
        REQUIRE(files.start_print("cube.gcode").ok());
        REQUIRE(ch->sent.size() == 1);
        CHECK(ch->sent[0].data.at("slot_map") == json::array());
    }
    SECTION("a CANVAS loaded after the confirmation: not started")
    {
        REQUIRE(files.start_check("cube.gcode", state, current, advice).ok());
        ch->printer = sf_idle(true, true);
        const pstore::Result r = files.start_print("cube.gcode");
        CHECK(r.status == pstore::Status::Failed);
        CHECK(r.message.find("changed since you confirmed") != std::string::npos);
        CHECK(ch->sent.empty());
    }
    SECTION("busy at the check: the panel is told, nothing is confirmed")
    {
        ch->printer = sf_printing("/local/other.gcode");
        REQUIRE(files.start_check("cube.gcode", state, current, advice).ok());
        CHECK(state == "printing");
        CHECK(current == "other.gcode");
        CHECK(advice.empty());
        CHECK(files.start_print("cube.gcode").status == pstore::Status::Failed);
        CHECK(ch->sent.empty());
    }
    SECTION("a print begun between the confirmation and the start: Busy, nothing sent")
    {
        REQUIRE(files.start_check("cube.gcode", state, current, advice).ok());
        ch->printer = sf_printing("/local/other.gcode");
        CHECK(files.start_print("cube.gcode").status == pstore::Status::Busy);
        CHECK(ch->sent.empty());
    }
    SECTION("the printer's refusal is the printer's words")
    {
        start_ack = 2;
        REQUIRE(files.start_check("cube.gcode", state, current, advice).ok());
        const pstore::Result r = files.start_print("cube.gcode");
        CHECK(r.status == pstore::Status::NotFound);
        CHECK(r.message.find("could not find the file") != std::string::npos);
    }
    SECTION("a printer that does not answer the check is not offered the start")
    {
        ch->printer = sf::PrinterRead();
        CHECK(files.start_check("cube.gcode", state, current, advice).status == pstore::Status::Offline);
    }
    SECTION("only G-code is started")
    {
        CHECK(files.start_print("plate.3mf").status == pstore::Status::NotFound);
    }
}

TEST_CASE("CC1 storage: which printers get it", "[ElegooSdcpFiles]")
{
    const pstore::Target cc1 = pstore::target_for_host("elegoolink", "10.0.0.9", "", "CC", "ph:2", "Elegoo Centauri Carbon", sf_options());
    CHECK(cc1.kind == pstore::Target::Kind::ElegooSdcp);
    CHECK(cc1.supported());
    CHECK(cc1.base_url == "http://10.0.0.9");
    CHECK(cc1.start_options == sf_options());
    CHECK(pstore::make_files(cc1) != nullptr); // builds the channel, connects to nothing

    const pstore::Target cc2 = pstore::target_for_host("elegoolink", "10.0.0.9", "", "CC2", "ph:3", "Elegoo Centauri Carbon 2");
    CHECK(cc2.kind == pstore::Target::Kind::Unsupported);
    CHECK(cc2.reason.find("Centauri Carbon") != std::string::npos);
    CHECK(pstore::make_files(cc2) == nullptr);

    const pstore::Target unnamed = pstore::target_for_host("elegoolink", "10.0.0.9", "", "CC", "ph:2");
    CHECK(unnamed.kind == pstore::Target::Kind::Unsupported);

    INFO("the options are part of the target: a changed send choice is a new backend");
    pstore::Target other = cc1;
    other.start_options["timeLapse"] = "0";
    CHECK(other != cc1);
}

TEST_CASE("CC1 storage: the Storage tab's model over the CC1 backend", "[ElegooSdcpFiles]")
{
    auto ch        = std::make_shared<SfFakeChannel>();
    ch->on_command = [](int cmd, const json&) -> sf::Reply {
        if (cmd == 258)
            return sf_answered(sf_list_answer_with({ "cube.gcode" }));
        if (cmd == 128)
            return sf_answered(sf_ack_answer(128, 0));
        return sf::Reply();
    };
    ch->printer = sf_idle(true, true);
    Slic3r::GUI::RemoteFileGridModel model(std::make_shared<sf::Cc1Files>(ch, sf_options(), SF_PC_BED, "CC"),
                                           std::make_shared<SfInlineExecutor>());
    model.Refresh();
    REQUIRE(model.GetStatus() == Slic3r::GUI::FileGridStatus::Ready);
    REQUIRE(model.GetCount() == 1);
    model.SetFocusRange(0, 1);
    const Slic3r::GUI::FileGridCard* card = model.FindCard("cube.gcode");
    REQUIRE(card);
    CHECK(card->details_loaded);
    CHECK(card->meta.layer_count == 10);
    CHECK(card->thumbnail_state == Slic3r::GUI::ThumbnailState::Missing);

    std::string got_advice;
    bool        checked = false;
    model.CheckStart("cube.gcode", [&](const pstore::Result& r, const std::string& state, const std::string&, const std::string& advice) {
        checked = r.ok() && state == "standby";
        got_advice = advice;
    });
    CHECK(checked);
    CHECK(got_advice.find("CANVAS") != std::string::npos);
    bool started = false;
    model.StartPrint("cube.gcode", [&](const pstore::Result& r) { started = r.ok(); });
    CHECK(started);
    CHECK(ch->sent.back().cmd == 128);
}
