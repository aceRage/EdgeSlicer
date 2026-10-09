#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <set>

#include "nlohmann/json.hpp"

#include "slic3r/GUI/DeviceModelCode.hpp"

using Slic3r::GUI::load_model_subseries;
using Slic3r::GUI::resolve_model_subseries;
using Slic3r::GUI::strip_model_revision;

namespace {
struct TempPrintersDir
{
    boost::filesystem::path dir;
    TempPrintersDir()
    {
        dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("printers-%%%%-%%%%");
        boost::filesystem::create_directories(dir);
    }
    ~TempPrintersDir()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(dir, ec);
    }
    void write(const char *name, const std::string &body)
    {
        boost::nowide::ofstream f((dir / name).string().c_str());
        f << body;
    }
};
} // namespace

TEST_CASE("A hardware revision suffix is dropped, a dash in the name is not", "[DeviceModelCode]")
{
    // The reported case: a later-batch H2C says O1C2-V2 and must land on the O1C2 definition.
    CHECK(strip_model_revision("O1C2-V2") == "O1C2");
    CHECK(strip_model_revision("O1D-V2") == "O1D");
    CHECK(strip_model_revision("N7-V12") == "N7");
    // Codes without a revision come back untouched, including the X1 family's dashed names.
    CHECK(strip_model_revision("O1C2") == "O1C2");
    CHECK(strip_model_revision("BL-P001") == "BL-P001");
    CHECK(strip_model_revision("C12") == "C12");
    CHECK(strip_model_revision("") == "");
    // Not a revision: no digits, a lower-case v, or nothing after the V.
    CHECK(strip_model_revision("O1C2-VX") == "O1C2-VX");
    CHECK(strip_model_revision("O1C2-v2") == "O1C2-v2");
    CHECK(strip_model_revision("O1C2-V") == "O1C2-V");
    CHECK(strip_model_revision("-V2") == "-V2");
}

TEST_CASE("The subseries table comes from the printer definitions and resolves a sub-series code", "[DeviceModelCode]")
{
    TempPrintersDir t;
    t.write("O1C2.json", R"({"00.00.00.00": {"model_id": "O1C2", "printer_type": "O1C2", "subseries": ["O1C2-V2", "O1C2-V3"]}})");
    t.write("O1D.json", R"({"00.00.00.00": {"model_id": "O1D", "printer_type": "O1D", "subseries": ["O1D-V2"]}})");
    // No subseries key: not in the table.
    t.write("C12.json", R"({"00.00.00.00": {"model_id": "C12", "printer_type": "C12"}})");
    // The blacklist that shares the folder has another shape and must be skipped, not fatal.
    t.write("filaments_blacklist.json", R"({"blacklist": [{"model_id": ["O1C2"]}]})");
    t.write("version.txt", "01.00.00.00");
    t.write("broken.json", "{ this is not json");

    const auto table = load_model_subseries(t.dir.string());
    REQUIRE(table.size() == 2);
    CHECK(resolve_model_subseries("O1C2-V2", table) == "O1C2");
    CHECK(resolve_model_subseries("O1C2-V3", table) == "O1C2");
    CHECK(resolve_model_subseries("O1D-V2", table) == "O1D");
    // A parent code is not its own sub-series; the file lookup handles it.
    CHECK(resolve_model_subseries("O1C2", table) == "");
    CHECK(resolve_model_subseries("O1S-V2", table) == "");
    CHECK(resolve_model_subseries("", table) == "");
}

TEST_CASE("A missing printers folder yields an empty table", "[DeviceModelCode]")
{
    const auto table = load_model_subseries((boost::filesystem::temp_directory_path() / "no-such-printers-dir-xyz").string());
    CHECK(table.empty());
    CHECK(resolve_model_subseries("O1C2-V2", table) == "");
}

// ---------------------------------------------------------------- the shipped printer definitions --
// A user's X2D could not be sent to (2026-10-09): it announces the model code N6, this build had no
// resources/printers/N6.json, so the code resolved to nothing, the device's model was stored empty
// and the send dialog called it incompatible with the X2D profile. These run against the real files.

namespace {
const std::string kResources = SLIC3R_TEST_RESOURCES_DIR;
const std::string kPrinters  = kResources + "/printers";

nlohmann::json read_json(const std::string &path)
{
    boost::nowide::ifstream f(path.c_str());
    if (! f.is_open())
        return nlohmann::json();
    return nlohmann::json::parse(f, nullptr, false);
}

std::string identify(const std::string &code, const std::string &serial)
{
    static const auto subseries = load_model_subseries(kPrinters);
    static const auto prefixes  = Slic3r::GUI::load_model_sn_prefixes(kPrinters);
    return Slic3r::GUI::identify_device_model(code, serial, kPrinters, subseries, prefixes);
}
} // namespace

TEST_CASE("The X2D's model code N6 and its sub-series resolve to the X2D definition", "[DeviceModelCode]")
{
    const nlohmann::json n6 = read_json(kPrinters + "/N6.json");
    REQUIRE(n6.is_object());
    const nlohmann::json &printer = n6["00.00.00.00"];
    CHECK(printer["model_id"] == "N6");
    CHECK(printer["printer_type"] == "N6");
    CHECK(printer["display_name"] == "Bambu Lab X2D");
    CHECK(printer["sn_prefix"] == "20P");
    // What the send dialog shows for an X2D: nozzle offset calibration, AMS, a chamber.
    CHECK(printer["print"]["support_nozzle_offset_calibration"] == true);
    CHECK(printer["print"]["support_chamber"] == true);
    CHECK(printer["use_ams_type"] == "generic");

    const auto subseries = load_model_subseries(kPrinters);
    CHECK(resolve_model_subseries("N6-V2", subseries) == "N6");

    using Slic3r::GUI::resolve_model_code;
    CHECK(resolve_model_code("N6", kPrinters, subseries) == "N6");
    CHECK(resolve_model_code("N6-V2", kPrinters, subseries) == "N6"); // listed sub-series
    CHECK(resolve_model_code("N6-V7", kPrinters, subseries) == "N6"); // a revision newer than the list
    // The earlier sub-series fix still holds against the shipped files.
    CHECK(resolve_model_code("O1C2-V2", kPrinters, subseries) == "O1C2");
    CHECK(resolve_model_code("O1D-V2", kPrinters, subseries) == "O1D");
    CHECK(resolve_model_code("C12", kPrinters, subseries) == "C12");
    CHECK(resolve_model_code("", kPrinters, subseries) == "");
    CHECK(resolve_model_code("ZZ9", kPrinters, subseries) == "");
    // A code is a file name, never a path.
    CHECK(resolve_model_code("../printers/N6", kPrinters, subseries) == "");
    CHECK(resolve_model_code("..", kPrinters, subseries) == "");
}

TEST_CASE("A printer that reports no usable model code is identified by its serial number", "[DeviceModelCode]")
{
    // Serials are made up; only the first three characters (Bambu's sn_prefix) matter.
    CHECK(identify("N6", "20P0000000000001") == "N6");
    CHECK(identify("", "20P0000000000001") == "N6");    // empty DevModel / a saved record with no model
    CHECK(identify("ZZ9", "20P0000000000001") == "N6"); // a code this build does not know
    CHECK(identify("", "0940000000000001") == "O1D");
    CHECK(identify("", "31B0000000000001") == "O1C2");
    CHECK(identify("", "00M0000000000001") == "BL-P001");
    // Nothing to go on: unidentified (the caller keeps the raw code).
    CHECK(identify("", "") == "");
    CHECK(identify("", "XYZ0000000000001") == "");
    CHECK(identify("", "20P") == ""); // a prefix alone is not a serial

    // Every shipped definition carries Bambu's prefix, and no two share one.
    const auto prefixes = Slic3r::GUI::load_model_sn_prefixes(kPrinters);
    CHECK(prefixes.size() == 11);
    CHECK(prefixes.at("20P") == "N6");
}

TEST_CASE("The X2D machine profile maps to the X2D definition and passes the send dialog's model check", "[DeviceModelCode]")
{
    const std::string machine_dir = kResources + "/profiles/BBL/machine";
    const nlohmann::json model = read_json(machine_dir + "/Bambu Lab X2D.json");
    REQUIRE(model.is_object());
    const std::string model_id = model.value("model_id", std::string());
    CHECK(model_id == "N6");
    for (const char *nozzle : { "0.2", "0.4", "0.6", "0.8" }) {
        INFO(nozzle);
        const nlohmann::json preset = read_json(machine_dir + "/Bambu Lab X2D " + nozzle + " nozzle.json");
        REQUIRE(preset.is_object());
        CHECK(preset.value("printer_model", std::string()) == "Bambu Lab X2D");
    }

    // Preset::get_printer_type gives the profile's model_id; the device side is the identified code.
    using Slic3r::GUI::device_matches_profile_model;
    const std::string device = identify("N6", "20P0000000000001");
    CHECK(device_matches_profile_model(model_id, device, {}));
    CHECK(device_matches_profile_model(model_id, identify("N6-V2", "20P0000000000001"), {}));
    // The 2.4.5.0 failure: the device's model was empty.
    CHECK_FALSE(device_matches_profile_model(model_id, "", {}));
    // A different printer stays refused unless its definition lists the profile as compatible.
    CHECK_FALSE(device_matches_profile_model(model_id, "O1D", {}));
    CHECK(device_matches_profile_model("O1C", "O1C2", { "O1C" }));
}

TEST_CASE("Every Bambu machine model has a printer definition, bar the known gaps", "[DeviceModelCode]")
{
    namespace fs = boost::filesystem;
    // P2S, A2L and H2D Pro profiles ship without a device definition yet: such a printer shows the
    // same "no printer definition in this build" refusal the X2D did. Shrink this set as they land.
    const std::set<std::string> known_gaps = { "N7", "N9", "O1E" };
    int checked = 0;
    for (fs::directory_iterator it(kResources + "/profiles/BBL/machine"), end; it != end; ++it) {
        if (it->path().extension() != ".json")
            continue;
        const nlohmann::json j = read_json(it->path().string());
        if (! j.is_object() || j.value("type", std::string()) != "machine_model")
            continue;
        const std::string id = j.value("model_id", std::string());
        INFO(it->path().filename().string() << " model_id " << id);
        if (known_gaps.count(id))
            continue;
        CHECK(Slic3r::GUI::read_definition_printer_type(kPrinters, id) == id);
        ++checked;
    }
    CHECK(checked >= 11);
}
