// The print-host device store: <datadir>/hub/print_host_devices.json.
//
// The store is wx-free on purpose, so it can be exercised here rather than through the dialog.
// Every case points it at its own temporary file (set_store_path) - nothing here reads or writes
// the user's data dir.

#include <catch2/catch.hpp>

#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/PrintHostDevices.hpp"
#include "slic3r/Utils/ElegooCc2.hpp"
#include "slic3r/Utils/PrintHostDeviceStatus.hpp"
#include "slic3r/Utils/PrusaLinkStatus.hpp"
#include "slic3r/Utils/CrealityPrint.hpp"
#include "slic3r/Utils/CrealityPrintModel.hpp"
#include "slic3r/GUI/ThirdPartyFilamentMap.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <algorithm>
#include <ctime>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::PrintHostDevices;
namespace fs = boost::filesystem;

// One temporary store file, removed again at the end of the case.
struct ScopedStore
{
    fs::path path;
    explicit ScopedStore(const std::string& tag)
    {
        path = fs::temp_directory_path() / ("print_host_devices_test_" + tag + "_" + std::to_string((long long) ::time(nullptr)) + ".json");
        boost::system::error_code ec;
        fs::remove(path, ec);
        set_store_path(path.string());
    }
    ~ScopedStore()
    {
        set_store_path("");
        boost::system::error_code ec;
        fs::remove(path, ec);
    }
    void write(const std::string& body) const
    {
        boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
        out << body;
    }
};

static Device make_device(const std::string& alias, const std::string& address, const std::string& host_type = "octoprint")
{
    Device d;
    d.alias         = alias;
    d.address       = address;
    d.host_type     = host_type;
    d.auth_type     = "key";
    d.apikey        = "k-" + alias;
    d.printer_model = "Elegoo Centauri Carbon";
    return d;
}

TEST_CASE("PrintHostDevices: a device survives save and load", "[PrintHostDevices]")
{
    ScopedStore store("roundtrip");
    const std::string key = "Elegoo Centauri Carbon";

    Device      d = make_device("Left bay", "192.168.1.41", "elegoolink");
    d.serial      = "SNADVA1234567";
    std::string error;
    REQUIRE(add(key, d, error));
    REQUIRE(error.empty());
    REQUIRE_FALSE(d.id.empty());
    REQUIRE(d.created > 0);

    const std::vector<Device> back = devices(key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].id == d.id);
    CHECK(back[0].alias == "Left bay");
    CHECK(back[0].address == "192.168.1.41");
    CHECK(back[0].host_type == "elegoolink");
    CHECK(back[0].auth_type == "key");
    CHECK(back[0].apikey == "k-Left bay");
    CHECK(back[0].serial == "SNADVA1234567");
    CHECK(back[0].printer_model == "Elegoo Centauri Carbon");
    CHECK(back[0].created == d.created);

    // The file really is the store: it is on disk, and it is JSON a person can read.
    REQUIRE(fs::exists(store.path));
    CHECK(fs::file_size(store.path) > 0);

    // An address already in this model's list is refused rather than silently doubled.
    Device dup = make_device("Left bay again", "192.168.1.41/");
    error.clear();
    CHECK_FALSE(add(key, dup, error));
    CHECK_FALSE(error.empty());
    CHECK(devices(key).size() == 1);
}

TEST_CASE("PrintHostDevices: an id is stable across edits and reloads", "[PrintHostDevices]")
{
    ScopedStore store("idstable");
    const std::string key = "Elegoo Centauri Carbon";

    Device      d = make_device("Left bay", "192.168.1.41");
    std::string error;
    REQUIRE(add(key, d, error));
    const std::string id = d.id;

    // Renaming and readdressing keeps the id, so anything holding it (an archived send, the phone's
    // device card) still points at the same printer.
    Device edited  = d;
    edited.alias   = "Garage";
    edited.address = "192.168.1.55";
    REQUIRE(update(key, edited, error));

    Device found;
    REQUIRE(find(key, id, found));
    CHECK(found.id == id);
    CHECK(found.alias == "Garage");
    CHECK(found.address == "192.168.1.55");
    CHECK(found.created == d.created); // created is not rewritten by an edit

    // And it is still that id after the store has been read from the file again.
    const std::vector<Device> back = devices(key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].id == id);

    // last_used is a memory of where the last plate went, not a setting: it names the device and
    // stamps that device's own timestamp in one go.
    CHECK(last_used_id(key).empty()); // nothing was ever sent
    set_last_used(key, id);
    CHECK(last_used_id(key) == id);
    REQUIRE(find(key, id, found));
    CHECK(found.last_used > 0);

    REQUIRE(remove(key, id));
    CHECK(devices(key).empty());
    CHECK(last_used_id(key).empty()); // the memory went with the device
    CHECK_FALSE(remove(key, id));
}

TEST_CASE("PrintHostDevices: devices are grouped by printer model", "[PrintHostDevices]")
{
    ScopedStore store("grouping");
    const std::string elegoo = "Elegoo Centauri Carbon";
    const std::string voron  = "Voron 2.4";

    std::string error;
    Device      a = make_device("Left bay", "192.168.1.41");
    Device      b = make_device("Right bay", "192.168.1.42");
    Device      c = make_device("Voron", "192.168.1.41"); // the same address, another model: fine
    REQUIRE(add(elegoo, a, error));
    REQUIRE(add(elegoo, b, error));
    REQUIRE(add(voron, c, error));

    CHECK(devices(elegoo).size() == 2);
    CHECK(devices(voron).size() == 1);
    CHECK(devices("Elegoo Centauri").empty()); // a model nobody added to has no devices

    const auto all = all_devices();
    REQUIRE(all.size() == 2);
    CHECK(all.at(elegoo).size() == 2);
    CHECK(all.at(voron).size() == 1);

    // A device of one model is not visible under another.
    Device found;
    CHECK(find(elegoo, a.id, found));
    CHECK_FALSE(find(voron, a.id, found));

    // Every variant of one machine shares a list: the nozzle is in the preset name, not the model.
    CHECK(model_key("Elegoo Centauri Carbon", "Elegoo Centauri Carbon 0.4 nozzle") == elegoo);
    CHECK(model_key("Elegoo Centauri Carbon", "Elegoo Centauri Carbon 0.2 nozzle") == elegoo);
    // A preset with no printer_model at all falls back to its own name.
    CHECK(model_key("", "My homebrew printer") == "preset:My homebrew printer");
}

TEST_CASE("PrintHostDevices: a broken file reads as an empty list", "[PrintHostDevices]")
{
    const std::string key = "Elegoo Centauri Carbon";

    SECTION("not JSON at all") {
        ScopedStore store("malformed");
        store.write("{ this is not json ]]");
        CHECK(devices(key).empty());
        CHECK(all_devices().empty());
        CHECK(last_used_id(key).empty());
        // ... and writing to it repairs it rather than failing.
        Device      d = make_device("Left bay", "192.168.1.41");
        std::string error;
        REQUIRE(add(key, d, error));
        CHECK(devices(key).size() == 1);
    }

    SECTION("JSON, but the wrong shape") {
        ScopedStore store("wrongshape");
        store.write("[1, 2, 3]");
        CHECK(devices(key).empty());
    }

    SECTION("the right shape with rubbish in it") {
        ScopedStore store("rubbish");
        store.write(R"({"version":1,"models":{"Elegoo Centauri Carbon":{"devices":[
            {"id":"d1","alias":"Good","address":"192.168.1.41"},
            {"id":"d2","alias":"No address"},
            "not an object",
            {"id":"d3","address":"192.168.1.42","port":"nonsense","created":"yesterday"}
        ]}}})");
        const std::vector<Device> back = devices(key);
        REQUIRE(back.size() == 2); // the entry with no address and the string are dropped
        CHECK(back[0].alias == "Good");
        CHECK(back[0].host_type == "octoprint"); // the default for a field that is not there
        CHECK(back[1].address == "192.168.1.42");
        CHECK(back[1].created == 0); // "yesterday" is not a number, so no date rather than a wrong one
    }

    SECTION("no file at all") {
        ScopedStore store("missing");
        CHECK(devices(key).empty());
    }
}

TEST_CASE("PrintHostDevices: a preset's own address becomes device 1, once", "[PrintHostDevices]")
{
    ScopedStore store("migration");
    const std::string key = "Elegoo Centauri Carbon";

    PresetHost p;
    p.model_key     = key;
    p.preset_name   = "Elegoo Centauri Carbon 0.4 nozzle";
    p.address       = "192.168.1.41";
    p.host_type     = "elegoolink";
    p.auth_type     = "key";
    p.apikey        = "secret";
    p.printer_model = "Elegoo Centauri Carbon";

    PresetHost none;             // a preset with no print_host contributes nothing
    none.model_key   = "Voron 2.4";
    none.preset_name = "Voron";
    none.address     = "";

    CHECK(migrate_from_presets({ p, none }) == 1);
    std::vector<Device> back = devices(key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].alias == "Elegoo Centauri Carbon 0.4 nozzle");
    CHECK(back[0].address == "192.168.1.41");
    CHECK(back[0].host_type == "elegoolink");
    CHECK(back[0].apikey == "secret");
    // An import is not a send: it does not make the imported device the model's last used one,
    // because this feature has no "main printer" for it to become.
    CHECK(last_used_id(key).empty());
    CHECK(devices("Voron 2.4").empty());

    // Idempotent: running it again changes nothing.
    const std::string id = back[0].id;
    CHECK(migrate_from_presets({ p, none }) == 0);
    back = devices(key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].id == id);

    // Another variant of the same machine, pointing at the same printer, adds nothing either.
    PresetHost variant = p;
    variant.preset_name = "Elegoo Centauri Carbon 0.2 nozzle";
    variant.address     = "192.168.1.41/";
    CHECK(migrate_from_presets({ variant }) == 0);
    CHECK(devices(key).size() == 1);

    // A device somebody deleted afterwards stays deleted - the pair was already imported.
    REQUIRE(remove(key, id));
    CHECK(migrate_from_presets({ p }) == 0);
    CHECK(devices(key).empty());

    // A second, genuinely different address of the same model does arrive.
    PresetHost second = p;
    second.preset_name = "Elegoo Centauri Carbon - garage";
    second.address     = "192.168.1.55";
    CHECK(migrate_from_presets({ second }) == 1);
    REQUIRE(devices(key).size() == 1);
    CHECK(devices(key)[0].address == "192.168.1.55");
}

TEST_CASE("PrintHostDevices: a phase-1 store's \"current\" is read as the last used one", "[PrintHostDevices]")
{
    // Phase 1 wrote a model-level "current": the device whose address its "Use this device" button
    // had copied into the preset. That button is gone, but the field is the best guess at "the one
    // you last sent to", so it is still read - and replaced the next time a send happens.
    ScopedStore store("phase1current");
    const std::string key = "Elegoo Centauri Carbon";
    store.write("{\"version\":1,\"models\":{\"" + key +
                "\":{\"current\":\"dcafe\",\"devices\":[{\"id\":\"dcafe\",\"address\":\"192.168.1.41\"},"
                "{\"id\":\"dbeef\",\"address\":\"192.168.1.42\"}]}}}");
    CHECK(last_used_id(key) == "dcafe");

    set_last_used(key, "dbeef");
    CHECK(last_used_id(key) == "dbeef");
    Device found;
    REQUIRE(find(key, "dbeef", found));
    CHECK(found.last_used > 0);

    // config_for is what the send builds its job from: a copy, never the preset itself.
    DynamicPrintConfig preset;
    preset.opt_string("print_host", true) = "192.168.1.99";
    REQUIRE(find(key, "dbeef", found));
    const DynamicPrintConfig job = config_for(found, preset);
    CHECK(job.opt_string("print_host") == "192.168.1.42");
    CHECK(preset.opt_string("print_host") == "192.168.1.99"); // untouched
}

TEST_CASE("PrintHostDevices: the send button follows the devices, not the preset", "[PrintHostDevices]")
{
    ScopedStore store("cansend");
    const std::string key = "Elegoo Centauri Carbon";

    // The preset the send path reads: a printer model, and no address of its own. This is exactly
    // the case the devices feature exists for, and the Print button has to light up for it.
    DynamicPrintConfig config;
    config.opt_string("printer_model", true) = key;
    config.opt_string("print_host", true)    = "";

    CHECK_FALSE(can_send_for(config, "Elegoo Centauri Carbon 0.4 nozzle"));
    CHECK_FALSE(has_devices(key));

    // One device with an address is enough - nothing is written to the preset.
    Device      d = make_device("Left bay", "192.168.1.41", "elegoolink");
    std::string error;
    REQUIRE(add(key, d, error));
    CHECK(has_devices(key));
    CHECK(can_send_for(config, "Elegoo Centauri Carbon 0.4 nozzle"));
    CHECK(config.opt_string("print_host").empty()); // still empty: no bridge writes it any more

    // Every nozzle variant of the machine shares the list, so they all send.
    CHECK(can_send_for(config, "Elegoo Centauri Carbon 0.2 nozzle"));

    // A different model does not borrow it.
    DynamicPrintConfig other;
    other.opt_string("printer_model", true) = "Voron 2.4";
    other.opt_string("print_host", true)    = "";
    CHECK_FALSE(can_send_for(other, "Voron"));
    // ... unless the preset carries its own address, the way it always worked.
    other.opt_string("print_host") = "192.168.1.90";
    CHECK(can_send_for(other, "Voron"));

    // A device with no address is not a place to send to. (add() refuses one, so this goes in by
    // hand, the way a hand-edited file would.)
    REQUIRE(remove(key, d.id));
    CHECK_FALSE(has_devices(key));
    store.write("{\"version\":1,\"models\":{\"" + key + "\":{\"devices\":[{\"id\":\"x\",\"address\":\"\"}]}}}");
    CHECK_FALSE(has_devices(key));
    CHECK_FALSE(can_send_for(config, "Elegoo Centauri Carbon 0.4 nozzle"));
}

TEST_CASE("PrintHostDevices: what a device says it has loaded", "[PrintHostDevices]")
{
    // No network here: parse_moonraker_status is the half of probe() that turns a Moonraker
    // `result.status` object into slots, so the parsing is exercised without a printer.

    SECTION("a Snapmaker-flavoured Moonraker names its filaments") {
        const std::string status =
            "{\"print_stats\":{\"state\":\"standby\"},"
            " \"print_task_config\":{"
            "   \"filament_type\":[\"PLA\",\"PETG\",\"\"],"
            "   \"filament_sub_type\":[\"Basic\",\"HF\",\"\"],"
            "   \"filament_vendor\":[\"Polymaker\",\"Generic\",\"\"],"
            "   \"filament_color_rgba\":[\"FF0000FF\",\"0000FFFF\",\"\"],"
            "   \"filament_exist\":[true,true,false]}}";
        const Status st = parse_moonraker_status(status, "octoprint");
        CHECK(st.probed);
        CHECK(st.online);
        CHECK(st.state == "standby");
        REQUIRE(st.slots.size() == 3);
        CHECK(st.slots_have_filaments);
        CHECK(st.slots[0].index == 0);
        CHECK(st.slots[0].type == "PLA");
        CHECK(st.slots[0].vendor == "Polymaker");
        CHECK(st.slots[0].color == "#FF0000"); // the alpha byte is dropped
        CHECK(st.slots[0].loaded);
        CHECK(st.slots[1].color == "#0000FF");
        CHECK_FALSE(st.slots[2].loaded);
        CHECK(st.note.empty()); // there is something to map, so nothing to apologise for
        CHECK(st.slots[0].label() == "T1 PLA Basic");
        CHECK_FALSE(st.qidi_box); // a U1 is never taken for a QIDI Box
        CHECK_FALSE(st.qidi);     // nor for QIDI firmware: no QIDI options in its send dialog
    }

    SECTION("a stock Klipper names its tools and nothing in them") {
        const std::string status =
            "{\"print_stats\":{\"state\":\"printing\"},"
            " \"extruder\":{\"nozzle_diameter\":0.4,\"temperature\":210},"
            " \"extruder1\":{\"nozzle_diameter\":0.4,\"temperature\":25}}";
        const Status st = parse_moonraker_status(status, "octoprint");
        CHECK(st.online);
        CHECK(st.state == "printing");
        REQUIRE(st.slots.size() == 2);
        CHECK_FALSE(st.slots_have_filaments); // tools, not filaments: no mapping table
        CHECK(st.slots[1].index == 1);
        CHECK(st.slots[0].nozzle == Approx(0.4));
        CHECK_FALSE(st.note.empty());
        CHECK_FALSE(st.qidi_box);
        CHECK_FALSE(st.qidi);
    }

    SECTION("a QIDI Box names its slots and what is in them") {
        // The shape QIDIStudio's QDSDevice::updateBoxDataByJson reads off an X-Max 4.
        const std::string status =
            "{\"print_stats\":{\"state\":\"standby\"},"
            " \"extruder\":{\"nozzle_diameter\":0.4},"
            " \"multi_color_controller\":{\"box_count\":1,\"box\":{\"boxes\":[{"
            "   \"box_inserted_0\":1,\"box_inserted_1\":1,\"box_inserted_2\":0,\"box_inserted_3\":true,"
            "   \"filament_info\":["
            "     {\"filament_type\":\"PLA\",\"filament_str\":\"PLA Rapido\",\"vendor_str\":\"QIDI\",\"color_rgb_str\":[\"#FFFFFF\"]},"
            "     {\"filament_type\":\"ASA\",\"filament_str\":\"ASA\",\"vendor_str\":\"QIDI\",\"color_rgb_str\":[\"#202020\"]},"
            "     {\"filament_type\":\"PETG\",\"color_rgb_str\":[\"#00FF00\"]},"
            "     {\"filament_type\":\"ABS\",\"color_rgb_str\":[]}]}]}}}";
        const Status st = parse_moonraker_status(status, "octoprint");
        CHECK(st.online);
        CHECK(st.qidi_box);
        CHECK(st.qidi);
        CHECK_FALSE(st.qidi_polar_cooler); // no polar cooler pin in the answer
        CHECK(st.slots_have_filaments);
        REQUIRE(st.slots.size() == 4); // the Box's slots, not the one extruder
        CHECK(st.slots[1].index == 1);
        CHECK(st.slots[1].type == "ASA");
        CHECK(st.slots[1].color == "#202020");
        CHECK(st.slots[1].loaded);
        CHECK(st.slots[1].label() == "Box 1 slot 2 ASA");
        CHECK_FALSE(st.slots[2].loaded); // a tag left in the firmware from an unloaded spool
        CHECK(st.slots[2].type.empty());
        CHECK(st.slots[3].loaded);
        CHECK(st.slots[3].color.empty());
        CHECK(st.note.empty());
        CHECK_FALSE(st.happy_hare);
        CHECK_FALSE(st.afc);
        CHECK(Slic3r::GUI::bambu_style_slot_label(st.slots[0].index / 4, st.slots[0].index % 4) == "1A");
        CHECK(Slic3r::GUI::bambu_style_slot_label(st.slots[3].index / 4, st.slots[3].index % 4) == "1D");
        CHECK(st.slots[0].color == "#FFFFFF");
    }

    SECTION("a QIDI printer with no Box plugged in is a plain Klipper") {
        const std::string status =
            "{\"print_stats\":{\"state\":\"standby\"},"
            " \"extruder\":{\"nozzle_diameter\":0.4},"
            " \"multi_color_controller\":{\"box_count\":0,\"boxes\":[]}}";
        const Status st = parse_moonraker_status(status, "octoprint");
        CHECK_FALSE(st.qidi_box);
        CHECK(st.qidi); // still QIDI firmware: the print options are offered without the Box
        CHECK_FALSE(st.slots_have_filaments);
        REQUIRE(st.slots.size() == 1);
    }

    SECTION("a QIDI printer with a polar cooler says so") {
        const std::string status =
            "{\"print_stats\":{\"state\":\"standby\"},"
            " \"output_pin polar_cooler\":{\"value\":0},"
            " \"multi_color_controller\":{\"box_count\":0}}";
        const Status st = parse_moonraker_status(status, "octoprint");
        CHECK(st.qidi);
        CHECK(st.qidi_polar_cooler);
    }

    SECTION("a stock Klipper is not taken for a QIDI") {
        const Status st = parse_moonraker_status("{\"print_stats\":{\"state\":\"standby\"},"
                                                 " \"output_pin polar_cooler\":{\"value\":0}}",
                                                 "octoprint");
        CHECK_FALSE(st.qidi);
        CHECK_FALSE(st.qidi_polar_cooler);
    }

    SECTION("the QIDI print options go out the way QIDIStudio sends them, leveling on its own") {
        QidiPrintOptions o;
        CHECK(qidi_start_script(o, "") == ""); // nothing offered, nothing mapped: nothing sent
        CHECK(qidi_leveling_script(o) == "");

        o.use_box      = 1;
        o.bed_leveling = 1;
        o.polar_cooler = 0;
        // The Box call carries the Box and the polar cooler, never G31/G32: a firmware that
        // refuses the leveling must not take the Box mapping down with it.
        CHECK(qidi_start_script(o, "0:4") ==
              "SAVE_VARIABLE VARIABLE=enable_box VALUE=1\n"
              "SAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot4'\"\n"
              "SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=0\n");
        CHECK(qidi_leveling_script(o) == "G31\n");
        // The Box on with nothing mapped still switches it on.
        CHECK(qidi_start_script(o, "") ==
              "SAVE_VARIABLE VARIABLE=enable_box VALUE=1\n"
              "SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=0\n");

        // The Box off: no mapping goes out, whatever the table said.
        o.use_box      = 0;
        o.bed_leveling = 0;
        o.polar_cooler = -1;
        CHECK(qidi_start_script(o, "0:4") == "SAVE_VARIABLE VARIABLE=enable_box VALUE=0\n");
        CHECK(qidi_leveling_script(o) == "G32\n");

        // Leveling only (no Box, no cooler offered): the Box call is empty, the leveling is not.
        QidiPrintOptions level_only;
        level_only.bed_leveling = 0;
        CHECK(qidi_start_script(level_only, "") == "");
        CHECK(qidi_leveling_script(level_only) == "G32\n");
    }

    SECTION("the QIDI options start off, the Box on whenever there is one") {
        Status st;
        st.qidi     = true;
        st.qidi_box = true;
        st.slots.resize(4);
        // Nothing remembered: bed leveling, timelapse and cooler start off. An empty Box starts on
        // too: printing from the toolhead spool is the person's choice (it may hold the last
        // print's material), never a silent default.
        QidiOptionDefaults d = qidi_option_defaults(st, "", "", "");
        CHECK_FALSE(d.bed_leveling);
        CHECK_FALSE(d.timelapse);
        CHECK_FALSE(d.polar_cooler);
        CHECK(d.use_box);
        // And with a loaded slot.
        st.slots[2].loaded = true;
        d = qidi_option_defaults(st, "", "", "");
        CHECK(d.use_box);
        // The person's last choice wins over the default, both ways.
        d = qidi_option_defaults(st, "1", "1", "1");
        CHECK(d.bed_leveling);
        CHECK(d.timelapse);
        CHECK(d.polar_cooler);
        d = qidi_option_defaults(st, "0", "0", "0");
        CHECK_FALSE(d.bed_leveling);
        CHECK_FALSE(d.timelapse);
        CHECK_FALSE(d.polar_cooler);
        // No Box at all: never on, loaded slots or not (those would be a U1's tools).
        st.qidi_box = false;
        d = qidi_option_defaults(st, "", "", "");
        CHECK_FALSE(d.use_box);
    }

    SECTION("the QIDI Box mapping goes out the way QIDIStudio sends it") {
        // QIDIStudio's own quoting, VALUE="'slotN'", character for character.
        CHECK(qidi_box_mapping_script("0:1,2:15") ==
              "SAVE_VARIABLE VARIABLE=enable_box VALUE=1\n"
              "SAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot1'\"\n"
              "SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot15'\"\n");
        // The firmware keeps 16 tools and 16 Box slots; anything past that is not sent.
        CHECK(qidi_box_mapping_script("16:0,0:16") == "");
        CHECK(qidi_box_mapping_script("") == "");
        CHECK(qidi_box_mapping_script("x:y,1:2") ==
              "SAVE_VARIABLE VARIABLE=enable_box VALUE=1\n"
              "SAVE_VARIABLE VARIABLE=value_t1 VALUE=\"'slot2'\"\n");
    }

    SECTION("a printing or paused printer is busy, anything else is not") {
        CHECK(busy_printing("printing"));
        CHECK(busy_printing("paused"));
        CHECK(busy_printing("Printing"));
        CHECK_FALSE(busy_printing("standby"));
        CHECK_FALSE(busy_printing("complete"));
        CHECK_FALSE(busy_printing("cancelled"));
        CHECK_FALSE(busy_printing("error"));
        CHECK_FALSE(busy_printing(""));
    }

    SECTION("the state comes out of a print_stats query reply") {
        CHECK(print_state_from_query("{\"result\":{\"eventtime\":1.0,\"status\":{\"print_stats\":{\"state\":\"printing\"}}}}") ==
              "printing");
        CHECK(print_state_from_query("{\"result\":{\"status\":{\"print_stats\":{\"state\":\"standby\"}}}}") == "standby");
        // Anything that does not say comes back "", which the send path takes as "could not tell": no send.
        CHECK(print_state_from_query("{\"result\":{\"status\":{}}}") == "");
        CHECK(print_state_from_query("{\"error\":{\"message\":\"nope\"}}") == "");
        CHECK(print_state_from_query("not json") == "");
        CHECK(print_state_from_query("") == "");
    }

    SECTION("auto-match: every host but a QIDI Box keeps the nearest-colour rule") {
        std::vector<Slot> slots(4);
        for (int i = 0; i < 4; ++i)
            slots[i].index = i;
        slots[0].loaded = true; slots[0].type = "PLA"; slots[0].color = "#FF0000";
        slots[1].loaded = true; slots[1].type = "ASA"; slots[1].color = "#202020";
        slots[2].loaded = true; slots[2].type = "PETG"; // no colour: never auto-picked here
        slots[3].loaded = false; slots[3].type = "ASA"; slots[3].color = "#000000";
        // The nearest colour wins whatever the material says: the ASA plate filament in red goes
        // to the red PLA, exactly as before the QIDI change.
        CHECK(auto_match_slot(slots, "#F01010", "ASA", false) == 1);
        CHECK(auto_match_slot(slots, "#101010", "PLA", false) == 2);
        // A colourless slot is skipped even when it holds the same material.
        CHECK(auto_match_slot(slots, "#101010", "PETG", false) == 2);
        // An unloaded slot is skipped even when its colour is the exact one.
        CHECK(auto_match_slot(slots, "#000000", "ASA", false) == 2);
        // Nothing loaded with a colour: "As sliced".
        std::vector<Slot> bare(1);
        bare[0].loaded = true;
        bare[0].type   = "PLA";
        CHECK(auto_match_slot(bare, "#FF0000", "PLA", false) == 0);
        CHECK(auto_match_slot({}, "#FF0000", "PLA", false) == 0);
        // Equal distances keep the first slot.
        std::vector<Slot> twins(2);
        twins[0].loaded = twins[1].loaded = true;
        twins[0].color = twins[1].color = "#00FF00";
        CHECK(auto_match_slot(twins, "#00FF00", "", false) == 1);
        // An unreadable colour is grey, as the dialog's swatch draws it.
        std::vector<Slot> greys(2);
        greys[0].loaded = greys[1].loaded = true;
        greys[0].color = "#FFFFFF";
        greys[1].color = "#A0A0A0";
        CHECK(auto_match_slot(greys, "not a colour", "", false) == 2);
    }

    SECTION("auto-match: a QIDI Box goes by material and never by colour alone") {
        std::vector<Slot> slots(4);
        for (int i = 0; i < 4; ++i)
            slots[i].index = i;
        slots[0].loaded = true; slots[0].type = "PLA"; slots[0].color = "#FF0000";
        slots[1].loaded = true; slots[1].type = "ASA"; slots[1].color = "#FFFFFF";
        slots[2].loaded = true; slots[2].type = "asa"; // the printer gave no colour
        slots[3].loaded = true; slots[3].type = "ASA"; slots[3].color = "#E00000";
        // The red ASA, never the red PLA.
        CHECK(auto_match_slot(slots, "#FF0000", "ASA", true) == 4);
        // Of the same material, a slot with a colour beats one without, however far the colour.
        slots[3].loaded = false;
        CHECK(auto_match_slot(slots, "#FF0000", "ASA", true) == 2);
        // Only a colourless match left: it still counts (case does not matter).
        slots[1].loaded = false;
        CHECK(auto_match_slot(slots, "#FF0000", "ASA", true) == 3);
        // No slot of that material, or no material on the plate filament: "As sliced".
        CHECK(auto_match_slot(slots, "#FF0000", "PETG", true) == 0);
        CHECK(auto_match_slot(slots, "#FF0000", "", true) == 0);
    }

    SECTION("Happy Hare names its gates and what is loaded in them") {
        // Shape of GET /printer/objects/query?mmu, as MoonrakerPrinterAgent::fetch_hh_filament_info reads it.
        const std::string query =
            R"({"result":{"status":{"mmu":{
                "num_gates":4,
                "gate_status":[1,2,0,-1],
                "gate_material":["PLA","PETG","ABS","TPU"],
                "gate_color":["#FF0000","0x00FF00","#0000FF","#FFFFFF"],
                "gate_temperature":[210,240,0,0]
            }}}})";
        const Status st = parse_happy_hare_mmu(query, "octoprint");
        CHECK(st.online);
        CHECK(st.slots_have_filaments);
        REQUIRE(st.slots.size() == 4);
        CHECK(st.slots[0].index == 0);
        CHECK(st.slots[0].type == "PLA");
        CHECK(st.slots[0].color == "#FF0000");
        CHECK(st.slots[0].loaded);
        CHECK(st.slots[1].type == "PETG");
        CHECK(st.slots[1].color == "#00FF00"); // 0x prefix
        CHECK(st.slots[1].loaded);
        CHECK_FALSE(st.slots[2].loaded); // gate_status 0
        CHECK(st.slots[2].type.empty());
        CHECK_FALSE(st.slots[3].loaded); // gate_status -1
        CHECK(st.slots[0].label() == "T1 PLA");
        CHECK(st.note.empty());
        CHECK(st.happy_hare);
        CHECK_FALSE(st.qidi_box);
        CHECK_FALSE(st.afc);
        // Panel tiles group gates by four: 1A–1D. Slot::label() (combo / sent index) is unchanged.
        CHECK(Slic3r::GUI::bambu_style_slot_label(st.slots[0].index / 4, st.slots[0].index % 4) == "1A");
        CHECK(Slic3r::GUI::bambu_style_slot_label(st.slots[3].index / 4, st.slots[3].index % 4) == "1D");

        // The same object inside a status snapshot is enough for parse_moonraker_status.
        const std::string status =
            R"({"print_stats":{"state":"standby"},
                "mmu":{"num_gates":2,"gate_status":[1,0],"gate_material":["ASA",""],"gate_color":["202020",""]}})";
        const Status from_status = parse_moonraker_status(status, "octoprint");
        CHECK(from_status.state == "standby");
        REQUIRE(from_status.slots.size() == 2);
        CHECK(from_status.slots_have_filaments);
        CHECK(from_status.slots[0].type == "ASA");
        CHECK(from_status.slots[0].color == "#202020");
        CHECK_FALSE(from_status.slots[1].loaded);
    }

    SECTION("an empty Happy Hare mmu is not a mapping table") {
        CHECK_FALSE(parse_happy_hare_mmu(R"({"result":{"status":{"mmu":{}}}})", "octoprint").slots_have_filaments);
        CHECK_FALSE(parse_happy_hare_mmu(R"({"mmu":{"num_gates":0}})", "octoprint").slots_have_filaments);
        const Status empty_gates = parse_happy_hare_mmu(
            R"({"num_gates":2,"gate_status":[0,0],"gate_material":["",""],"gate_color":["",""]})", "octoprint");
        REQUIRE(empty_gates.slots.size() == 2);
        CHECK_FALSE(empty_gates.slots_have_filaments);
        CHECK_FALSE(empty_gates.note.empty());
    }

    SECTION("Snapmaker print_task_config wins over a Happy Hare mmu in the same status") {
        const std::string status =
            R"({"print_task_config":{"filament_type":["PLA"],"filament_color_rgba":["FF0000FF"],"filament_exist":[true]},
                "mmu":{"num_gates":1,"gate_status":[1],"gate_material":["PETG"],"gate_color":["00FF00"]}})";
        const Status st = parse_moonraker_status(status, "octoprint");
        REQUIRE(st.slots.size() == 1);
        CHECK(st.slots[0].type == "PLA");
    }

    SECTION("AFC / BoxTurtle lane_data names its lanes") {
        // Shape of GET /server/database/item?namespace=lane_data, as
        // MoonrakerPrinterAgent::fetch_moonraker_filament_data and scripts/test_moonraker_lane_data.py.
        const std::string body =
            R"({"result":{"namespace":"lane_data","value":{
                "lane2":{"lane":"1","material":"PETG","color":"#00FF00","nozzle_temp":240,"bed_temp":80},
                "lane1":{"lane":"0","material":"PLA","color":"0xFF0000","nozzle_temp":210,"bed_temp":60},
                "lane3":{"lane":"2","material":"","color":"#None"}
            }}})";
        const Status st = parse_afc_lane_data(body, "octoprint");
        CHECK(st.online);
        CHECK(st.slots_have_filaments);
        REQUIRE(st.slots.size() == 3); // sorted by the `lane` tool number, not the JSON key
        CHECK(st.slots[0].index == 0);
        CHECK(st.slots[0].type == "PLA");
        CHECK(st.slots[0].color == "#FF0000");
        CHECK(st.slots[0].loaded);
        CHECK(st.slots[1].index == 1);
        CHECK(st.slots[1].type == "PETG");
        CHECK_FALSE(st.slots[2].loaded);
        CHECK(st.slots[2].type.empty());
        CHECK(st.slots[2].color.empty());
        CHECK(st.note.empty());
        CHECK(st.afc);
        CHECK_FALSE(st.happy_hare);
        CHECK(Slic3r::GUI::bambu_style_slot_label(st.slots[0].index / 4, st.slots[0].index % 4) == "1A");
        CHECK(Slic3r::GUI::bambu_style_slot_label(st.slots[1].index / 4, st.slots[1].index % 4) == "1B");
        CHECK(st.slots[0].color == "#FF0000");

        const std::string lanes_array =
            R"({"lanes":[{"lane":0,"material":"ASA","color":"#202020"},{"lane":"1","material":"TPU","color":"#111111"}]})";
        const Status from_array = parse_afc_lane_data(lanes_array, "octoprint");
        REQUIRE(from_array.slots.size() == 2);
        CHECK(from_array.slots[0].type == "ASA");
        CHECK(from_array.slots[1].type == "TPU");
    }

    SECTION("AFC lane_data with no materials is not a mapping table") {
        CHECK_FALSE(parse_afc_lane_data("{}", "octoprint").slots_have_filaments);
        CHECK_FALSE(parse_afc_lane_data(R"({"result":{"value":{}}})", "octoprint").slots_have_filaments);
        CHECK_FALSE(parse_afc_lane_data("not json", "octoprint").online);
    }

    SECTION("Happy Hare named colours map to hex; unparsable names stay empty") {
        CHECK(print_host_slot_colour("red") == "#FF0000");
        CHECK(print_host_slot_colour("0x00FF00") == "#00FF00");
        CHECK(print_host_slot_colour("#None").empty());
        CHECK(print_host_slot_colour("chartreuse").empty());
        CHECK(print_host_slot_colour("").empty());

        const Status st = parse_happy_hare_mmu(
            R"({"num_gates":4,"gate_status":[1,1,1,1],
                "gate_material":["PLA","PLA","PLA","PLA"],
                "gate_color":["red","Blue","chartreuse","#None"]})",
            "octoprint");
        REQUIRE(st.slots.size() == 4);
        CHECK(st.slots[0].color == "#FF0000");
        CHECK(st.slots[0].color_name.empty());
        CHECK(st.slots[1].color == "#0000FF");
        CHECK(st.slots[2].color.empty()); // unmapped CSS name: not '#RRGGBB', so not a colour
        CHECK(st.slots[2].color_name == "chartreuse");
        CHECK(st.slots[2].label().find("chartreuse") != std::string::npos);
        CHECK(st.slots[3].color.empty()); // #None
        CHECK(st.slots[3].color_name.empty());

        const Status afc = parse_afc_lane_data(
            R"({"lane1":{"lane":0,"material":"PLA","color":"chartreuse"}})", "octoprint");
        REQUIRE(afc.slots.size() == 1);
        CHECK(afc.slots[0].color.empty());
        CHECK(afc.slots[0].color_name == "chartreuse");
        CHECK(afc.slots[0].loaded);
    }

    SECTION("num_gates above 32 is clamped, not dropped") {
        const Status st = parse_happy_hare_mmu(
            R"({"num_gates":40,"gate_status":[1],"gate_material":["PLA"],"gate_color":["red"]})",
            "octoprint");
        REQUIRE(st.slots.size() == 32);
        CHECK(st.slots[0].type == "PLA");
        CHECK(st.slots[0].color == "#FF0000");
        CHECK(st.slots[0].loaded);
        CHECK_FALSE(st.slots[31].loaded);
    }

    SECTION("wrong-type Happy Hare fields do not throw") {
        CHECK(parse_happy_hare_mmu(R"({"num_gates":[],"gate_status":1,"gate_material":{"0":"PLA"}})", "octoprint")
                  .slots.empty());
        CHECK(parse_happy_hare_mmu(R"({"num_gates":"nope"})", "octoprint").slots.empty());
        const Status st = parse_happy_hare_mmu(
            R"({"num_gates":"2","gate_status":[1,1],"gate_material":["PLA",1],"gate_color":[true,"green"]})",
            "octoprint");
        REQUIRE(st.slots.size() == 2);
        CHECK(st.slots[0].type == "PLA");
        CHECK(st.slots[1].type.empty()); // material was a number
        CHECK(st.slots[1].color == "#008000");
    }

    SECTION("a huge gate_status array is truncated to num_gates") {
        // Two loaded gates, then padding. Starting from [1] plus zeros made gate 1
        // empty, so the parser (which skips leftover names on status <= 0) left type blank.
        std::string body = R"({"num_gates":2,"gate_status":[1,1)";
        for (int i = 0; i < 200; ++i)
            body += ",0";
        body += R"(],"gate_material":["PLA","PETG"],"gate_color":["red","blue"]})";
        const Status st = parse_happy_hare_mmu(body, "octoprint");
        REQUIRE(st.slots.size() == 2);
        CHECK(st.slots[0].type == "PLA");
        CHECK(st.slots[1].type == "PETG");
    }

    SECTION("a QIDI Box in the same status is not replaced by Happy Hare gates") {
        const std::string status =
            R"({"print_stats":{"state":"standby"},
                "multi_color_controller":{"box_count":1,"boxes":[]},
                "mmu":{"num_gates":1,"gate_status":[1],"gate_material":["PLA"],"gate_color":["red"]}})";
        const Status st = parse_moonraker_status(status, "octoprint");
        CHECK_FALSE(st.slots_have_filaments);
        CHECK(st.qidi);
        CHECK_FALSE(st.happy_hare); // QIDI firmware: its numbering is never replaced by gates
    }

    SECTION("AFC lanes cap at 32, de-duplicate, and reject out-of-range indices") {
        const Status dup = parse_afc_lane_data(
            R"({"lane1":{"lane":0,"material":"PLA","color":"red"},
                "lane2":{"lane":0,"material":"PETG","color":"blue"},
                "lane3":{"lane":99,"material":"ABS","color":"yellow"},
                "lane4":{"lane":-3,"material":"TPU","color":"white"}})",
            "octoprint");
        REQUIRE(dup.slots.size() == 1);
        CHECK(dup.slots[0].index == 0);
        CHECK(dup.slots[0].type == "PLA");

        std::string many = "{\"lanes\":[";
        for (int i = 0; i < 40; ++i) {
            if (i)
                many += ",";
            many += "{\"lane\":" + std::to_string(i) + ",\"material\":\"PLA\",\"color\":\"red\"}";
        }
        many += "]}";
        const Status capped = parse_afc_lane_data(many, "octoprint");
        REQUIRE(capped.slots.size() == 32);
        CHECK(capped.slots.back().index == 31);
    }

    SECTION("Happy Hare's own get_status keys beyond the gate arrays are ignored") {
        // What Happy Hare publishes to Moonraker (the public docs list these under printer.mmu):
        // alongside the gate arrays there are enabled / gate / tool / filament / ttg_map /
        // endless_spool_groups / gate_color_rgb. None of them may change the slot list.
        const std::string status =
            R"({"print_stats":{"state":"standby"},
                "mmu":{"enabled":true,"num_gates":4,"is_homed":true,"tool":-1,"gate":0,"filament":"Loaded",
                       "ttg_map":[0,1,2,3],"gate_status":[1,1,0,-1],
                       "gate_material":["PLA","PETG","","ABS"],
                       "gate_color":["ff0000","00ff00","","blue"],
                       "gate_color_rgb":[[1.0,0.0,0.0],[0.0,1.0,0.0],[0.5,0.5,0.5],[0.0,0.0,1.0]],
                       "endless_spool_groups":[0,1,2,3],"action":"Idle"}})";
        const Status st = parse_moonraker_status(status, "octoprint");
        REQUIRE(st.slots.size() == 4);
        CHECK(st.happy_hare);
        CHECK_FALSE(st.afc);
        CHECK(st.slots_have_filaments);
        CHECK(st.slots[0].color == "#FF0000");
        CHECK(st.slots[1].type == "PETG");
        CHECK_FALSE(st.slots[2].loaded);
        CHECK_FALSE(st.slots[3].loaded); // gate_status -1: unknown, never shown as loaded
        CHECK(st.mmu_grouped());
    }

    SECTION("AFC lane_data: the system entry and non-lane keys are ignored") {
        const std::string body =
            R"({"result":{"namespace":"lane_data","key":null,"value":{
                "system":{"cur_lane":null,"cur_extruder":"extruder","extruders":{"extruder":{"lane_loaded":null}}},
                "lane1":{"lane":"0","material":"PLA","color":"#ff0000","map":"T0"},
                "lane2":{"lane":"1","material":"PETG","color":"#00ff00","map":"T1"}
            }}})";
        const Status st = parse_afc_lane_data(body, "octoprint");
        REQUIRE(st.slots.size() == 2);
        CHECK(st.afc);
        CHECK(st.mmu_grouped());
        CHECK(st.slots[1].type == "PETG");
    }

    SECTION("only the MMU kinds are shown four to a unit") {
        // A plain Klipper tool list and a Snapmaker toolhead list keep their T<n> labels.
        const Status plain = parse_moonraker_status(R"({"print_stats":{"state":"standby"},"extruder":{"nozzle_diameter":0.4},"extruder1":{"nozzle_diameter":0.4}})", "octoprint");
        CHECK_FALSE(plain.happy_hare);
        CHECK_FALSE(plain.afc);
        CHECK_FALSE(plain.mmu_grouped());
        const auto tiles = Slic3r::GUI::third_party_slots_from(plain.slots, plain.mmu_grouped());
        REQUIRE(tiles.size() == 2);
        CHECK(Slic3r::GUI::slot_display_label(tiles[1]) == "T2");

        const Status hh = parse_happy_hare_mmu(
            R"({"num_gates":6,"gate_status":[1,1,1,1,1,1],"gate_material":["PLA","PLA","PLA","PLA","PLA","PLA"],
                "gate_color":["red","red","red","red","red","red"]})",
            "octoprint");
        const auto hh_tiles = Slic3r::GUI::third_party_slots_from(hh.slots, hh.mmu_grouped());
        REQUIRE(hh_tiles.size() == 6);
        CHECK(Slic3r::GUI::slot_display_label(hh_tiles[0]) == "1A");
        CHECK(Slic3r::GUI::slot_display_label(hh_tiles[5]) == "2B");
    }

    SECTION("something that is not a Moonraker printer") {
        const Status st = parse_moonraker_status("{}", "octoprint");
        CHECK_FALSE(st.online);
        CHECK(st.slots.empty());
        CHECK_FALSE(st.note.empty());
    }

    SECTION("Elegoo Link is asked only for a Centauri Carbon 1's CANVAS, and says so otherwise") {
        // A CC1 answers Cmd 324 with its CANVAS trays (ElegooCanvas.hpp, tested with a scripted
        // printer in elegoo_canvas_tests.cpp). Any other Elegoo model has nothing to read.
        CHECK(can_probe("elegoolink"));
        CHECK(can_probe("octoprint"));
        const std::string note = no_filament_note("elegoolink");
        CHECK(note.find("Elegoo Link") != std::string::npos);
        CHECK(note.find("sent exactly as it was sliced") != std::string::npos);

        Device d        = make_device("Neptune", "192.0.2.1", "elegoolink");
        d.printer_model = "Elegoo Neptune 4";
        Status  st = probe(d, 1); // no network is touched: not a model with a CANVAS
        CHECK_FALSE(st.probed);
        CHECK_FALSE(st.online);
        CHECK(st.slots.empty());
        CHECK(st.note == note);
    }

    SECTION("a device with no address is not probed either") {
        Device d;
        d.host_type = "octoprint";
        const Status st = probe(d, 1);
        CHECK_FALSE(st.probed);
        CHECK(st.note == "this device has no address");
    }
}

TEST_CASE("PrintHostDevices: which send a preset takes", "[PrintHostDevices]")
{
    // The regression this case exists for. `use_new_connect` is a GLOBAL app-config flag: SSWCP and
    // SMPhysicalPrinterDialog set it to "true" the moment any Snapmaker machine connects, and
    // nothing clears it when the user then selects a different printer preset. The send branch used
    // to read it on its own (`use_new_connect == "true" || is_snapmaker_u1`), so on a PC with a
    // Snapmaker U1 on the LAN, selecting an Elegoo Centauri Carbon and pressing Print handed the
    // plate to WebPreprintDialog - the Snapmaker pre-treat page - instead of uploading it to the
    // Elegoo host.

    SECTION("a print-host printer never takes the Snapmaker flow, flag or no flag")
    {
        // The owner's machine: an Elegoo Centauri Carbon with a U1 connected beside it.
        CHECK(send_flow_for("Elegoo Centauri Carbon", true) == SendFlow::PrintHost);
        CHECK(send_flow_for("Elegoo Centauri Carbon", false) == SendFlow::PrintHost);

        // And every other host type this store is for.
        for (const char* model : {"Prusa MK4", "Voron 2.4", "Duet Delta", "Creality K1", "Custom Printer"}) {
            CAPTURE(model);
            CHECK(send_flow_for(model, true) == SendFlow::PrintHost);
            CHECK(send_flow_for(model, false) == SendFlow::PrintHost);
        }
    }

    SECTION("a U1 always takes the connect flow, as it did before the fix")
    {
        // Unchanged behaviour: the U1's toolhead mapping page is the only way to pick its tools.
        CHECK(send_flow_for("Snapmaker U1", true) == SendFlow::SnapmakerConnect);
        CHECK(send_flow_for("Snapmaker U1", false) == SendFlow::SnapmakerConnect);
        // The model name is matched case-insensitively, the way the rest of the GUI matches it.
        CHECK(send_flow_for("snapmaker u1 (0.4 nozzle)", false) == SendFlow::SnapmakerConnect);
    }

    SECTION("another Snapmaker follows its own connection")
    {
        CHECK(send_flow_for("Snapmaker Artisan", true) == SendFlow::SnapmakerConnect);
        // Not connected: it is reached at whatever print_host it holds, like any other host.
        CHECK(send_flow_for("Snapmaker Artisan", false) == SendFlow::PrintHost);
    }

    SECTION("an empty model name is a print host, not a Snapmaker")
    {
        // A hand-made preset with no printer_model at all must not be swept into the connect flow.
        CHECK(send_flow_for("", true) == SendFlow::PrintHost);
        CHECK(send_flow_for("", false) == SendFlow::PrintHost);
    }

    SECTION("the model predicates themselves")
    {
        CHECK(is_snapmaker_model("Snapmaker U1"));
        CHECK(is_snapmaker_model("SNAPMAKER Artisan"));
        CHECK_FALSE(is_snapmaker_model("Elegoo Centauri Carbon"));
        CHECK_FALSE(is_snapmaker_model(""));

        CHECK(is_snapmaker_u1_model("Snapmaker U1"));
        CHECK_FALSE(is_snapmaker_u1_model("Snapmaker Artisan"));
        // "U1" without the vendor is not a U1: a model name is vendor-qualified in practice.
        CHECK_FALSE(is_snapmaker_u1_model("Elegoo U1"));
    }
}

TEST_CASE("PrintHostDevices: the preset bridge writes the fields the send path reads", "[PrintHostDevices]")
{
    // A bare config: apply_to_config creates the options it needs, so this works on a preset's
    // config and on an empty one alike.
    DynamicPrintConfig config;

    Device d      = make_device("Right bay", "192.168.1.42", "octoprint");
    d.auth_type   = "user";
    d.user        = "someone";
    d.password    = "pw";
    apply_to_config(d, config);

    CHECK(config.opt_string("print_host") == "192.168.1.42");
    CHECK(config.opt_string("printhost_user") == "someone");
    CHECK(config.opt_string("printhost_password") == "pw");
    CHECK(config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value == htOctoPrint);
    CHECK(config.option<ConfigOptionEnum<AuthorizationType>>("printhost_authorization_type")->value == atUserPassword);

    // And back out again, unchanged.
    const Device read = from_config(config);
    CHECK(read.address == d.address);
    CHECK(read.host_type == "octoprint");
    CHECK(read.auth_type == "user");
    CHECK(read.user == "someone");
    CHECK(read.password == "pw");

    CHECK(host_type_enum("elegoolink") == htElegooLink);
    CHECK(host_type_key(htElegooLink) == "elegoolink");
    CHECK(host_type_enum("nonsense") == htOctoPrint); // an unknown key is never a crash
    CHECK(speaks_moonraker("octoprint"));
    CHECK_FALSE(speaks_moonraker("elegoolink"));

    CHECK(normalize_address(" 192.168.1.41/ ") == "192.168.1.41");
    CHECK(normalize_address("HTTP://Printer.local/") == "http://printer.local");
}

TEST_CASE("PrintHostDevices: a device's serial reaches the Flashforge send, an empty one keeps the preset's", "[PrintHostDevices]")
{
    // The Flashforge HTTP local API is spoken with flashforge_serial_number + printhost_apikey (the
    // check code), both read from the per-device config the send builds. Two Creator 5s under one
    // preset must each send their own serial.
    DynamicPrintConfig preset;
    preset.opt_string("flashforge_serial_number", true) = "SNPRESET000001";

    Device left    = make_device("Left", "192.168.1.51", "flashforge");
    left.apikey    = "12345678";
    left.serial    = "SNLEFT00000001";
    Device right   = make_device("Right", "192.168.1.52", "flashforge");
    right.apikey   = "87654321";
    right.serial   = "SNRIGHT0000001";

    const DynamicPrintConfig l = config_for(left, preset);
    const DynamicPrintConfig r = config_for(right, preset);
    CHECK(l.opt_string("flashforge_serial_number") == "SNLEFT00000001");
    CHECK(l.opt_string("printhost_apikey") == "12345678");
    CHECK(l.option<ConfigOptionEnum<PrintHostType>>("host_type")->value == htFlashforge);
    CHECK(r.opt_string("flashforge_serial_number") == "SNRIGHT0000001");
    CHECK(r.opt_string("printhost_apikey") == "87654321");
    CHECK(from_config(l).serial == "SNLEFT00000001");

    // A device that was never given a serial (every non-Flashforge printer, and a Flashforge added
    // before this field existed) falls back to whatever the preset holds.
    Device bare = make_device("Bare", "192.168.1.53", "flashforge");
    CHECK(config_for(bare, preset).opt_string("flashforge_serial_number") == "SNPRESET000001");

    // And the migration carries the preset's serial into device 1.
    ScopedStore store("serial_migration");
    PresetHost  p;
    p.model_key     = "Flashforge Creator 5";
    p.preset_name   = "Creator 5";
    p.address       = "192.168.1.60";
    p.host_type     = "flashforge";
    p.apikey        = "11112222";
    p.serial        = "SNMIGRATED0001";
    p.printer_model = "Flashforge Creator 5";
    REQUIRE(migrate_from_presets(std::vector<PresetHost>{ p }) == 1);
    const std::vector<Device> back = devices(p.model_key);
    REQUIRE(back.size() == 1);
    CHECK(back[0].serial == "SNMIGRATED0001");
}

TEST_CASE("PrusaLinkStatus: what a PrusaLink printer says it is doing", "[PrintHostDevices]")
{
    // No network here: parse_status is the half of probe() that turns the two REST answers into a
    // Status, so the mapping is exercised without a Buddy board on somebody's desk. The bodies
    // below are the shapes PrusaLink 2.1.2 answers on an MK4S.
    using namespace Slic3r::PrusaLinkStatus;

    SECTION("an idle printer: a state and two heaters, no job") {
        const std::string body =
            "{\"printer\":{\"state\":\"IDLE\",\"temp_nozzle\":24.7,\"target_nozzle\":0.0,"
            " \"temp_bed\":23.9,\"target_bed\":0.0,\"axis_z\":10.0,\"flow\":100,\"speed\":100},"
            " \"storage\":{\"path\":\"/usb/\",\"name\":\"usb\",\"read_only\":false}}";
        const PrusaLinkStatus::Status st = parse_status(body);
        CHECK(st.answered);
        CHECK(st.authorized);
        CHECK(st.raw_state == "IDLE");
        CHECK(st.state == "standby");
        CHECK(st.has_bed);
        CHECK(st.bed_temp == Approx(23.9));
        CHECK(st.bed_target == Approx(0.0));
        CHECK(st.has_nozzle);
        CHECK(st.nozzle_temp == Approx(24.7));
        CHECK_FALSE(st.has_progress);       // nothing is printing, so no percentage is invented
        CHECK_FALSE(st.has_time_remaining);
        CHECK(st.filename.empty());
    }

    SECTION("a printing one, status plus job: progress, time left and the file name") {
        const std::string status =
            "{\"printer\":{\"state\":\"PRINTING\",\"temp_nozzle\":215.0,\"target_nozzle\":215.0,"
            " \"temp_bed\":60.1,\"target_bed\":60.0},"
            " \"job\":{\"id\":34,\"progress\":62.0,\"time_remaining\":1440,\"time_printing\":2400}}";
        // /api/v1/job is the only answer that names the file; the status one never does.
        const std::string job =
            "{\"id\":34,\"state\":\"PRINTING\",\"progress\":62.0,\"time_remaining\":1440,"
            " \"time_printing\":2400,\"file\":{\"name\":\"CUBE~1.BGC\",\"display_name\":\"calibration cube.bgcode\","
            " \"path\":\"/usb\",\"size\":91234}}";
        const PrusaLinkStatus::Status st = parse_status(status, job);
        CHECK(st.answered);
        CHECK(st.state == "printing");
        CHECK(st.has_progress);
        CHECK(st.progress == Approx(62.0));
        CHECK(st.has_time_remaining);
        CHECK(st.time_remaining == 1440);
        CHECK(st.has_time_printing);
        CHECK(st.time_printing == 2400);
        CHECK(st.job_id == 34);
        CHECK(st.filename == "calibration cube.bgcode"); // display_name wins over the 8.3 name
        CHECK(st.has_nozzle);
        CHECK(st.nozzle_target == Approx(215.0));
    }

    SECTION("a job with no display_name falls back to the on-disk one") {
        const std::string status = "{\"printer\":{\"state\":\"PAUSED\"}}";
        const std::string job    = "{\"state\":\"PAUSED\",\"file\":{\"name\":\"PART~1.GCO\"}}";
        const PrusaLinkStatus::Status st     = parse_status(status, job);
        CHECK(st.state == "paused");
        CHECK(st.filename == "PART~1.GCO");
    }

    SECTION("the state vocabulary is PrusaLink's, mapped onto this API's") {
        CHECK(to_klipper_state("IDLE") == "standby");
        CHECK(to_klipper_state("READY") == "standby");
        CHECK(to_klipper_state("PRINTING") == "printing");
        CHECK(to_klipper_state("PAUSED") == "paused");
        CHECK(to_klipper_state("FINISHED") == "complete");
        CHECK(to_klipper_state("STOPPED") == "cancelled");
        CHECK(to_klipper_state("ERROR") == "error");
        CHECK(to_klipper_state("ATTENTION") == "error");
        CHECK(to_klipper_state("BUSY") == "busy");
        CHECK(to_klipper_state("printing") == "printing");      // case does not matter
        CHECK(to_klipper_state("SOMETHING_NEW") == "something_new"); // never dropped, just lowered
        CHECK(to_klipper_state("").empty());
    }

    SECTION("something that is not a PrusaLink printer") {
        CHECK_FALSE(parse_status("{}").answered);
        CHECK_FALSE(parse_status("<html>404</html>").answered);
        CHECK_FALSE(parse_status("{\"state\":{\"text\":\"Operational\"}}").answered); // an OctoPrint box
        CHECK_FALSE(parse_status("").answered);
    }

    SECTION("progress is clamped, negative times are dropped") {
        const PrusaLinkStatus::Status st = parse_status(
            "{\"printer\":{\"state\":\"PRINTING\"},\"job\":{\"progress\":100.0001,\"time_remaining\":-1}}");
        CHECK(st.has_progress);
        CHECK(st.progress == Approx(100.0));
        CHECK_FALSE(st.has_time_remaining); // PrusaLink sends -1 for "no estimate yet"
    }

    SECTION("which host types this client speaks, and how it addresses them") {
        CHECK(speaks_prusalink("prusalink"));
        CHECK(speaks_prusalink("prusaconnect"));
        CHECK_FALSE(speaks_prusalink("octoprint"));
        CHECK_FALSE(speaks_prusalink("elegoolink"));
        CHECK_FALSE(speaks_prusalink(""));
        CHECK(base_url("192.168.1.50") == "http://192.168.1.50");
        CHECK(base_url("http://prusa.local/") == "http://prusa.local");
        CHECK(base_url("https://connect.prusa3d.com") == "https://connect.prusa3d.com");
        CHECK(base_url("").empty());
    }

    SECTION("can_probe now covers PrusaLink, and it still says it maps no filaments") {
        CHECK(PrintHostDevices::can_probe("prusalink"));
        CHECK(PrintHostDevices::can_probe("prusaconnect"));
        CHECK(PrintHostDevices::can_probe("octoprint"));
        CHECK(PrintHostDevices::can_probe("elegoolink")); // the CC1's CANVAS
        // It reports a state but never a filament, and the dialog has to say so.
        CHECK(PrintHostDevices::no_filament_note("prusalink").find("PrusaLink") != std::string::npos);
    }
}

TEST_CASE("PrintHost: get_err_code_from_body does not throw on malformed JSON", "[PrintHost]")
{
    // Duet/MKS (and the deleted ESP3D/Flashforge copies) used throwing pt::read_json.
    // Html from a non-Duet host, garbage, a JSON array, or a non-integer err must return -1
    // rather than throw out of an Http on_complete callback (Orca #15947).
    CHECK(PrintHost::get_err_code_from_body("") == -1);
    CHECK(PrintHost::get_err_code_from_body("<html>") == -1);
    CHECK(PrintHost::get_err_code_from_body("[]") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"x"})") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":0})") == 0);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":1})") == 1);
    // ptree lexical_cast accepted numeric strings; keep that via try_parse_json_err_int.
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"0"})") == 0);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"1"})") == 1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":" 2 "})") == 2);
    // "-7" not "-1": a real {"err":-1} cannot be told from a parse failure (accepted).
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"-7"})") == -7);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"+3"})") == 3);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"1x"})") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":" "})") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":true})") == -1);
    // INT_MIN/INT_MAX are in range; one past each is not. String forms match.
    CHECK(PrintHost::get_err_code_from_body(R"({"err":2147483647})") == 2147483647);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":-2147483648})") == (-2147483647 - 1));
    CHECK(PrintHost::get_err_code_from_body(R"({"err":2147483648})") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":-2147483649})") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"2147483647"})") == 2147483647);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"-2147483648"})") == (-2147483647 - 1));
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"2147483648"})") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"-2147483649"})") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"99999999999999999999"})") == -1);
    // Above 2^64 JSON has no integer type left; nlohmann stores a float, which we reject.
    CHECK(PrintHost::get_err_code_from_body(R"({"err":1e20})") == -1);
    // Out of int range must not truncate to 0 (success). 2^32 was the reported get<int> wrap.
    CHECK(PrintHost::get_err_code_from_body(R"({"err":4294967296})") == -1);
    CHECK(PrintHost::get_err_code_from_body(R"({"err":"4294967296"})") == -1);
    // Floats stay -1: stricter than ptree, which could coerce 1.0 → 1.
    CHECK(PrintHost::get_err_code_from_body(R"({"err":1.5})") == -1);
    // A successful Duet connect often has no err field; that is success (0), not unknown (-1).
    CHECK(PrintHost::get_err_code_from_body(R"({})") == 0);
    REQUIRE_NOTHROW(PrintHost::get_err_code_from_body("<html>404</html>"));
}

TEST_CASE("CrealityPrint: K1/K2 gcode root and host parse", "[CrealityPrint]")
{
    using namespace Slic3r::CrealityPrintModel;

    SECTION("K1-family uses /usr/data") {
        CHECK(is_k1_family("K1"));
        CHECK(is_k1_family("K1 SE"));
        CHECK(is_k1_family("K1C"));
        CHECK(is_k1_family("K1_CFS-C"));
        CHECK(gcode_root("K1") == "/usr/data");
        CHECK(gcode_path("K1C", "cube.gcode") == "/usr/data/printer_data/gcodes/cube.gcode");
        CHECK(omit_upload_path("K1"));
        CHECK(omit_upload_path("K1_CFS-C"));
    }

    SECTION("K2-platform uses /mnt/UDISK") {
        CHECK_FALSE(is_k1_family("F008"));
        CHECK_FALSE(is_k1_family("F012"));
        CHECK_FALSE(is_k1_family("F021"));
        CHECK_FALSE(is_k1_family("F022"));
        CHECK_FALSE(is_k1_family("K2"));
        CHECK(gcode_root("F008") == "/mnt/UDISK");
        CHECK(gcode_root("F012") == "/mnt/UDISK");
        CHECK(gcode_root("F021") == "/mnt/UDISK");
        CHECK(gcode_root("F022") == "/mnt/UDISK");
        CHECK(gcode_path("F021", "benchy.gcode") == "/mnt/UDISK/printer_data/gcodes/benchy.gcode");
        CHECK(omit_upload_path("F008"));
        CHECK(omit_upload_path("F012"));
        CHECK(omit_upload_path("F021"));
        CHECK(omit_upload_path("F022"));
        CHECK(model_display_name("F008") == "K2 Plus");
        CHECK(model_display_name("F012") == "K2 Pro");
        CHECK(model_display_name("F021") == "K2");
        CHECK(model_display_name("F022") == "SPARKX i7");
    }

    SECTION("an unknown model is not treated as K1, so the K2-platform root applies") {
        CHECK_FALSE(is_k1_family(""));
        CHECK_FALSE(is_k1_family("Ender-3"));
        CHECK(gcode_root("") == "/mnt/UDISK");
        CHECK_FALSE(omit_upload_path(""));
        CHECK_FALSE(model_supports_multi_color("Ender-3"));
    }

    SECTION("empty or unknown /info falls back to the preset printer_model") {
        CHECK(resolve_model("K1C", "Creality K2 Plus") == "K1C");
        CHECK(resolve_model("F021", "Creality K1C") == "F021");
        CHECK(resolve_model("", "Creality K1C") == "K1C");
        CHECK(resolve_model("unknown", "Creality K1") == "K1");
        CHECK(resolve_model("", "Creality K2 Plus") == "F008");
        CHECK(gcode_root_for("", "Creality K1C") == "/usr/data");
        CHECK(gcode_root_for("garbage", "Creality K1 SE") == "/usr/data");
        CHECK(gcode_root_for("", "Creality K1") == "/usr/data");
        CHECK(gcode_root_for("", "Creality K2 Plus") == "/mnt/UDISK");
        CHECK(gcode_root_for("K1C", "Creality K2 Plus") == "/usr/data");
        CHECK(gcode_root_for("F021", "Creality K1C") == "/mnt/UDISK");
        CHECK(looks_like_k1_family("Creality K1 Max"));
        CHECK(gcode_root_for("", "Creality K1 Max") == "/usr/data");
    }

    SECTION("ws_host strips scheme and port (Http::url_host)") {
        CHECK(ws_host("192.168.1.50") == "192.168.1.50");
        CHECK(ws_host("http://192.168.1.50") == "192.168.1.50");
        CHECK(ws_host("http://192.168.1.50/") == "192.168.1.50");
        CHECK(ws_host("http://192.168.1.50:80") == "192.168.1.50");
        CHECK(ws_host("https://k2.local:443/path") == "k2.local");
        CHECK(ws_host("http://Printer.Local:7125") == "printer.local");
        CHECK(ws_host("").empty());
    }

    SECTION("parse_model_from_info is hostile-JSON safe") {
        CHECK(parse_model_from_info(R"({"model":"K1C"})") == "K1C");
        CHECK(parse_model_from_info(R"({"model":"F021","mac":"aa"})") == "F021");
        CHECK(parse_model_from_info("").empty());
        CHECK(parse_model_from_info("<html>").empty());
        CHECK(parse_model_from_info("[]").empty());
        CHECK(parse_model_from_info(R"({"model":1})").empty());
        CHECK(parse_model_from_info(R"({"name":"K1"})").empty());
        REQUIRE_NOTHROW(parse_model_from_info(std::string(64, '{')));
    }
}

TEST_CASE("CrealityPrint: WebSocket budget 0 does not start and does not UAF", "[CrealityPrint]")
{
    SECTION("expired total budget fails before any I/O") {
        CrealityWsClient ws(200, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::string err;
        REQUIRE_FALSE(ws.open("127.0.0.1", "1", err));
        CHECK(err.find("timed out") != std::string::npos);
        CHECK_FALSE(ws.is_open());
        CHECK(ws.remaining_ms() == 0);
    }

    SECTION("local acceptor that never accepts times out and drains") {
        namespace net = boost::asio;
        using tcp     = net::ip::tcp;
        net::io_context ioc;
        tcp::acceptor   acc(ioc, tcp::endpoint(tcp::v4(), 0));
        const auto      port = std::to_string(acc.local_endpoint().port());
        CrealityWsClient ws(150, 150);
        std::string      err;
        REQUIRE_FALSE(ws.open("127.0.0.1", port, err));
        CHECK(err.find("timed out") != std::string::npos);
        CHECK_FALSE(ws.is_open());
    }

    SECTION("cancel from another thread before open") {
        CrealityWsClient ws(200, 200);
        std::thread t([&] { ws.cancel(); });
        t.join();
        std::string err;
        REQUIRE_FALSE(ws.open("127.0.0.1", "1", err));
        CHECK(ws.is_cancelled());
        CHECK(err.find("cancelled") != std::string::npos);
        CHECK_FALSE(ws.is_open());
    }

    SECTION("cancel from another thread during a never-accepting connect") {
        namespace net = boost::asio;
        using tcp     = net::ip::tcp;
        net::io_context ioc;
        tcp::acceptor   acc(ioc, tcp::endpoint(tcp::v4(), 0));
        const auto      port = std::to_string(acc.local_endpoint().port());
        CrealityWsClient ws(2000, 2000);
        std::string      err;
        std::thread      t([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            ws.cancel();
        });
        REQUIRE_FALSE(ws.open("127.0.0.1", port, err));
        t.join();
        CHECK(ws.is_cancelled());
        CHECK(err.find("cancelled") != std::string::npos);
        CHECK_FALSE(ws.is_open());
    }
}

// OrcaSlicer #16772 timeout cases, ported onto CrealityWsClient (this fork's
// AsioUtils/run_until_done equivalent). query_boxes_info stays on the #616 stack.
namespace {

namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

enum class SilentWs {
    BeforeHandshake, // Accepts TCP, reads the upgrade, never answers
    AfterHandshake,  // Completes the upgrade, then never reads or sends
};

class FakeSilentCrealityWs
{
public:
    explicit FakeSilentCrealityWs(SilentWs behavior) : m_behavior(behavior)
    {
        m_accept_thread = std::thread([this] { accept_loop(); });
    }

    ~FakeSilentCrealityWs()
    {
        release();
        m_stopping = true;
        tcp::socket       wake{m_accept_ioc};
        beast::error_code ec;
        wake.connect(m_acceptor.local_endpoint(), ec);
        if (m_accept_thread.joinable())
            m_accept_thread.join();
        for (std::thread& connection : m_connections)
            if (connection.joinable())
                connection.join();
    }

    std::string port() const { return std::to_string(m_acceptor.local_endpoint().port()); }

    void release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_changed.notify_all();
    }

private:
    void accept_loop()
    {
        for (;;) {
            auto              ioc = std::make_unique<net::io_context>();
            tcp::socket       socket{*ioc};
            beast::error_code ec;
            m_acceptor.accept(socket, ec);
            if (ec || m_stopping)
                return;
            m_connections.emplace_back([this, ioc = std::move(ioc), socket = std::move(socket)]() mutable { serve(std::move(socket)); });
        }
    }

    void serve(tcp::socket socket)
    {
        beast::flat_buffer               buffer;
        http::request<http::string_body> request;
        beast::error_code                ec;
        http::read(socket, buffer, request, ec);
        if (ec)
            return;
        if (!websocket::is_upgrade(request) || m_behavior == SilentWs::BeforeHandshake) {
            wait_for_release();
            return;
        }

        websocket::stream<tcp::socket> ws{std::move(socket)};
        ws.accept(request, ec);
        if (ec)
            return;
        wait_for_release();
    }

    void wait_for_release()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_released; });
    }

    const SilentWs           m_behavior;
    net::io_context          m_accept_ioc;
    tcp::acceptor            m_acceptor{m_accept_ioc, {net::ip::make_address("127.0.0.1"), 0}};
    std::atomic<bool>        m_stopping{false};
    std::thread              m_accept_thread;
    std::vector<std::thread> m_connections;
    std::mutex               m_mutex;
    std::condition_variable  m_changed;
    bool                     m_released = false;
};

// Runs `call`, giving it `limit` to return before the printer hangs up on it.
template<class Call> bool finishes_within(FakeSilentCrealityWs& printer, std::chrono::milliseconds limit, Call&& call)
{
    auto       done    = std::async(std::launch::async, std::forward<Call>(call));
    const bool in_time = done.wait_for(limit) == std::future_status::ready;
    printer.release();
    // The future destructor would wait forever for a call the hang-up cannot unblock.
    if (done.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
        std::cerr << "CrealityWsClient call still running 10 s after the printer hung up" << std::endl;
        std::_Exit(EXIT_FAILURE);
    }
    return in_time;
}

} // namespace

TEST_CASE("CrealityPrint: silent printer WebSocket steps stay inside their deadlines", "[CrealityPrint]")
{
    // start_print is open + write + best-effort read + close. A printer that
    // never answers the upgrade fails open (the start command is never sent).
    // One that upgrades and then goes silent still delivers the write; the
    // read times out and is not a start failure.
    SECTION("handshake never completes") {
        FakeSilentCrealityWs printer(SilentWs::BeforeHandshake);
        CrealityWsClient     ws(200);
        std::string          err;
        bool                 opened = true;
        CHECK(finishes_within(printer, std::chrono::seconds(2), [&] { opened = ws.open("127.0.0.1", printer.port(), err); }));
        CHECK_FALSE(opened);
        CHECK(err.find("timed out") != std::string::npos);
        CHECK_FALSE(ws.is_open());
    }

    SECTION("upgrade completes then the printer goes silent") {
        FakeSilentCrealityWs printer(SilentWs::AfterHandshake);
        CrealityWsClient     ws(200);
        std::string          err;
        std::string          frame;
        bool                 opened  = false;
        bool                 wrote   = false;
        bool                 read_ok = true;
        CHECK(finishes_within(printer, std::chrono::seconds(3), [&] {
            opened = ws.open("127.0.0.1", printer.port(), err);
            if (!opened)
                return;
            wrote   = ws.write(R"({"method":"set","params":{"opGcodeFile":"printprt:/mnt/UDISK/printer_data/gcodes/test.gcode"}})", err);
            read_ok = ws.read_once(frame, err);
            ws.close();
        }));
        CHECK(opened);
        CHECK(wrote);
        CHECK_FALSE(read_ok);
        CHECK(err.find("timed out") != std::string::npos);
        CHECK_FALSE(ws.is_open());
    }
}

// ---- the printer lists: every saved device, whatever preset is selected ----
//
// The owner's report (2026-10-19): an Elegoo Centauri Carbon saved as a print-host device only
// showed in the Printers tab and the app's /api/printers while a Centauri Carbon preset was
// selected; Bambu printers and LAN Snapmakers show whatever is selected. host_listing is the one
// rule RemoteSend::list_hosts (the rows) and RemoteControl::list_host_targets (their live state)
// now both follow.

static const ListedDevice* listed(const HostListing& l, const std::string& alias)
{
    for (const ListedDevice& d : l.devices)
        if (d.device.alias == alias) return &d;
    return nullptr;
}

// Two models' device lists, as print_host_devices.json holds them.
static void two_models()
{
    std::string error;
    for (const char* name : { "CC1", "CC2", "CC3" }) {
        Device d = make_device(name, std::string("192.0.2.") + (name[2] == '1' ? "11" : name[2] == '2' ? "12" : "13"), "elegoolink");
        REQUIRE(add("Elegoo Centauri Carbon", d, error));
    }
    Device v = make_device("Voron", "http://192.0.2.20:7125", "octoprint");
    v.printer_model = "Voron 2.4 350";
    REQUIRE(add("Voron 2.4 350", v, error));
}

TEST_CASE("PrintHostDevices: a Bambu preset selected still lists every saved device", "[PrintHostDevices][listing]")
{
    ScopedStore store("listing_bambu");
    two_models();
    // A Bambu preset on the Bambu network: no selected print-host model, no "host" row.
    const HostListing l = host_listing(all_devices(), "", "", "");
    REQUIRE(l.devices.size() == 4);
    CHECK_FALSE(l.host_row);
    for (const char* name : { "CC1", "CC2", "CC3", "Voron" }) {
        const ListedDevice* d = listed(l, name);
        REQUIRE(d != nullptr);
        CHECK_FALSE(d->selected_model); // listed for monitoring: no send for another model
    }
    CHECK(listed(l, "CC3")->model_key == "Elegoo Centauri Carbon");
    CHECK(listed(l, "Voron")->model_key == "Voron 2.4 350");
}

TEST_CASE("PrintHostDevices: another print-host model selected still lists the Elegoo printers", "[PrintHostDevices][listing]")
{
    ScopedStore store("listing_other");
    two_models();
    const HostListing l = host_listing(all_devices(), "Voron 2.4 350", "http://192.0.2.20:7125", "octoprint");
    REQUIRE(l.devices.size() == 4);
    // The selected model's devices first, and only they may offer a send.
    CHECK(l.devices.front().device.alias == "Voron");
    CHECK(l.devices.front().selected_model);
    for (const char* name : { "CC1", "CC2", "CC3" }) {
        const ListedDevice* d = listed(l, name);
        REQUIRE(d != nullptr);
        CHECK_FALSE(d->selected_model);
    }
    // A Moonraker preset keeps its "host" row (its controls live there), as before.
    CHECK(l.host_row);
}

TEST_CASE("PrintHostDevices: a selected model with no saved devices lists the others", "[PrintHostDevices][listing]")
{
    ScopedStore store("listing_u1");
    two_models();
    const HostListing l = host_listing(all_devices(), "Snapmaker U1", "", "");
    CHECK(l.devices.size() == 4);
    CHECK_FALSE(l.host_row);
    for (const ListedDevice& d : l.devices)
        CHECK_FALSE(d.selected_model);
}

TEST_CASE("PrintHostDevices: the Elegoo preset's own address is listed once", "[PrintHostDevices][listing]")
{
    ScopedStore store("listing_cc");
    two_models();

    SECTION("the preset points at a saved device: no separate host row")
    {
        const HostListing l = host_listing(all_devices(), "Elegoo Centauri Carbon", "192.0.2.13", "elegoolink");
        CHECK_FALSE(l.host_row);
        REQUIRE(l.devices.size() == 4);
        // Each saved device once, the selected model's three first.
        std::vector<std::string> ids;
        for (const ListedDevice& d : l.devices) ids.push_back(d.device.id);
        std::sort(ids.begin(), ids.end());
        CHECK(std::unique(ids.begin(), ids.end()) == ids.end());
        for (size_t i = 0; i < 3; ++i) {
            CHECK(l.devices[i].model_key == "Elegoo Centauri Carbon");
            CHECK(l.devices[i].selected_model);
        }
        CHECK_FALSE(listed(l, "Voron")->selected_model);
    }
    SECTION("the same printer written another way still matches")
    {
        CHECK_FALSE(host_listing(all_devices(), "Elegoo Centauri Carbon", "http://192.0.2.13:3030/", "elegoolink").host_row);
    }
    SECTION("an address no device has keeps the host row")
    {
        CHECK(host_listing(all_devices(), "Elegoo Centauri Carbon", "192.0.2.99", "elegoolink").host_row);
    }
    SECTION("another model's device at that address does not hide the host row")
    {
        CHECK(host_listing(all_devices(), "Voron 2.4 350", "192.0.2.13", "elegoolink").host_row);
    }
}

TEST_CASE("PrintHostDevices: an empty store lists nothing and keeps the host row", "[PrintHostDevices][listing]")
{
    ScopedStore store("listing_empty");
    const HostListing l = host_listing(all_devices(), "Elegoo Centauri Carbon", "192.0.2.13", "elegoolink");
    CHECK(l.devices.empty());
    CHECK(l.host_row);
    CHECK_FALSE(host_listing(all_devices(), "", "", "").host_row);
}

TEST_CASE("PrintHostDevices: bare_host reduces an address to its host", "[PrintHostDevices][listing]")
{
    CHECK(bare_host("192.0.2.13") == "192.0.2.13");
    CHECK(bare_host(" HTTP://192.0.2.13:3030/ ") == "192.0.2.13");
    CHECK(bare_host("ws://192.0.2.13:3030/websocket") == "192.0.2.13");
    CHECK(bare_host("user:pw@printer.local:80") == "printer.local");
    CHECK(bare_host("[fe80::1]:3030") == "fe80::1");
    CHECK(bare_host("fe80::1") == "fe80::1");
    CHECK(bare_host("").empty());
}

TEST_CASE("Elegoo CC2 device with no serial and an empty API key gets the shared device page (owner D6)", "[PrintHostDevices]")
{
    // The owner's hand test, 2026-10-26: a device "CC2-Combo" added in the sidebar's devices dialog -
    // host 127.0.0.22, Elegoo Link, authorization API key, API key empty (the factory access code),
    // serial empty - under the "Elegoo Centauri Carbon 2 0.4 nozzle" preset, whose own address is empty.
    ScopedStore       store("cc2page");
    const std::string key    = "Elegoo Centauri Carbon 2";
    const std::string preset = "Elegoo Centauri Carbon 2";
    Device            d      = make_device("CC2-Combo", "127.0.0.22", "elegoolink");
    d.auth_type     = "key";
    d.apikey        = "";
    d.serial        = "";
    d.printer_model = preset; // the devices dialog seeds a new device with the preset's model
    std::string error;
    REQUIRE(add(key, d, error));
    const std::vector<Device> listed = devices(key);
    REQUIRE(listed.size() == 1);
    const Device& got = listed.front();
    CHECK(got.apikey.empty());
    CHECK(got.serial.empty());
    // Neither the empty key nor the missing serial keeps it off; a device with no model of its own
    // (hand-edited store) takes the preset's.
    CHECK(ElegooCc2::wants_shared_device_page(got.host_type, got.address, got.printer_model, preset));
    CHECK(ElegooCc2::wants_shared_device_page(got.host_type, got.address, "", preset));
    CHECK(ElegooCc2::wants_shared_device_page(got.host_type, got.address, "Elegoo Centauri Carbon 2", ""));
    CHECK(ElegooCc2::wants_shared_device_page(got.host_type, "http://127.0.0.22:80/", "", preset));

    // The same printer as the preset's own address (Edit connection...), empty key and serial.
    DynamicPrintConfig config;
    config.opt_string("printer_model", true)            = preset;
    config.opt_string("print_host", true)               = "127.0.0.22";
    config.opt_string("printhost_apikey", true)         = "";
    config.opt_string("flashforge_serial_number", true) = "";
    config.set_key_value("host_type", new ConfigOptionEnum<PrintHostType>(htElegooLink));
    const Device own = from_config(config);
    CHECK(own.host_type == "elegoolink");
    CHECK(ElegooCc2::wants_shared_device_page(own.host_type, own.address, own.printer_model, preset));

    // Not for a CC1, another host type, or a device with no address.
    CHECK_FALSE(ElegooCc2::wants_shared_device_page("elegoolink", "127.0.0.22", "", "Elegoo Centauri Carbon"));
    CHECK_FALSE(ElegooCc2::wants_shared_device_page("octoprint", "127.0.0.22", "", preset));
    CHECK_FALSE(ElegooCc2::wants_shared_device_page("elegoolink", "", "", preset));
}
