// The print-host device store: <datadir>/hub/print_host_devices.json.
//
// The store is wx-free on purpose, so it can be exercised here rather than through the dialog.
// Every case points it at its own temporary file (set_store_path) - nothing here reads or writes
// the user's data dir.

#include <catch2/catch.hpp>

#include "slic3r/Utils/PrintHostDevices.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <ctime>
#include <string>

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
