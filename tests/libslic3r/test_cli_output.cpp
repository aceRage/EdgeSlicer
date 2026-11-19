#include <catch2/catch.hpp>

#include "libslic3r/CliOutput.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Utils.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// libslic3r/CliOutput: output file names, the exit code table and the usage numbers of result.json.

TEST_CASE("--outputdir joins a relative name and leaves a rooted one alone", "[CLI][Output]")
{
    CHECK(cli_join_output_path("", "a.3mf") == "a.3mf");
    CHECK(cli_join_output_path("out", "a.3mf") == "out/a.3mf");
    CHECK(cli_join_output_path("out/", "a.3mf") == "out/a.3mf");
    CHECK(cli_join_output_path("out", "sub/a.3mf") == "out/sub/a.3mf");
    CHECK(cli_join_output_path("out", "") == "");
    // A rooted name is used as given (this used to be appended to the output directory).
    CHECK(cli_join_output_path("out", "/tmp/a.3mf") == "/tmp/a.3mf");
#ifdef _WIN32
    CHECK(cli_join_output_path("out", "C:/work/a.3mf") == "C:/work/a.3mf");
    CHECK(cli_join_output_path("out", "D:\\work\\a.3mf") == "D:\\work\\a.3mf");
    CHECK(cli_join_output_path("out", "\\\\server\\share\\a.3mf") == "\\\\server\\share\\a.3mf");
    CHECK(cli_join_output_path("out\\", "a.3mf") == "out\\a.3mf");
#endif
}

TEST_CASE("--export-gcode names one file per plate", "[CLI][Output]")
{
    SECTION("one plate keeps the name")
    {
        CHECK(cli_gcode_file_name("out.gcode", 1, 1) == "out.gcode");
        CHECK(cli_gcode_file_name("out.gcode", 3, 1) == "out.gcode");
    }
    SECTION("several plates get a suffix before the extension")
    {
        CHECK(cli_gcode_file_name("out.gcode", 1, 3) == "out_1.gcode");
        CHECK(cli_gcode_file_name("out.gcode", 2, 3) == "out_2.gcode");
        CHECK(cli_gcode_file_name("a/b.c/out.gcode", 2, 2) == "a/b.c/out_2.gcode");
        CHECK(cli_gcode_file_name("out.gcode.3mf", 2, 2) == "out.gcode_2.3mf");
    }
    SECTION("no extension: the suffix is appended")
    {
        CHECK(cli_gcode_file_name("out", 2, 2) == "out_2");
        CHECK(cli_gcode_file_name("a.b/out", 2, 2) == "a.b/out_2");
        CHECK(cli_gcode_file_name(".hidden", 2, 2) == ".hidden_2");
        CHECK(cli_gcode_file_name("out.", 2, 2) == "out._2");
    }
    SECTION("the {plate} token wins, for any plate count")
    {
        CHECK(cli_gcode_file_name("plate{plate}.gcode", 2, 1) == "plate2.gcode");
        CHECK(cli_gcode_file_name("plate{plate}.gcode", 2, 4) == "plate2.gcode");
        CHECK(cli_gcode_file_name("{plate}/{plate}.gcode", 12, 14) == "12/12.gcode");
    }
}

TEST_CASE("The exit code table lists every code once", "[CLI][Output]")
{
    const std::vector<CliExitCode> &table = cli_exit_code_names();
    REQUIRE(table.size() > 40);

    std::set<int>         codes;
    std::set<std::string> names;
    for (const CliExitCode &entry : table) {
        CHECK(codes.insert(entry.code).second);
        CHECK(names.insert(entry.name).second);
        CHECK(std::string(entry.name).rfind("CLI_", 0) == 0);
    }
    // From 0 downwards, each one lower than the one before.
    for (size_t i = 1; i < table.size(); ++i)
        CHECK(table[i].code < table[i - 1].code);

    CHECK(cli_exit_code_name(CLI_SUCCESS) == "CLI_SUCCESS");
    CHECK(cli_exit_code_name(CLI_INVALID_PARAMS) == "CLI_INVALID_PARAMS");
    CHECK(cli_exit_code_name(CLI_FILE_NOTFOUND) == "CLI_FILE_NOTFOUND");
    CHECK(cli_exit_code_name(CLI_NO_SUITABLE_OBJECTS) == "CLI_NO_SUITABLE_OBJECTS");
    CHECK(cli_exit_code_name(CLI_SPIRAL_MODE_INVALID_PARAMS) == "CLI_SPIRAL_MODE_INVALID_PARAMS");
    CHECK(cli_exit_code_name(CLI_MIXED_FILAMENT_INVALID) == "CLI_MIXED_FILAMENT_INVALID");
    CHECK(cli_exit_code_name(CLI_GCODE_PATH_CONFLICTS) == "CLI_GCODE_PATH_CONFLICTS");
    CHECK(cli_exit_code_name(-12345).empty());
}

TEST_CASE("The exit code list comes as JSON and as a table", "[CLI][Output]")
{
    const std::map<int, std::string> messages = {
        {CLI_SUCCESS, "Success."},
        {CLI_INVALID_PARAMS, "Invalid parameters to the slicer."},
        {CLI_GCODE_PATH_CONFLICTS, " G-code conflicts detected. "}, // the real map has this leading space
    };

    const nlohmann::json j = cli_exit_codes_json(messages);
    REQUIRE(j.contains("exit_codes"));
    const nlohmann::json &list = j["exit_codes"];
    REQUIRE(list.is_array());
    REQUIRE(list.size() == cli_exit_code_names().size());
    CHECK(list.front()["code"] == 0);
    CHECK(list.front()["name"] == "CLI_SUCCESS");
    CHECK(list.front()["message"] == "Success.");
    CHECK(list.back()["code"] == CLI_GCODE_PATH_CONFLICTS);
    CHECK(list.back()["message"] == "G-code conflicts detected."); // trimmed
    // A code the map has no text for still appears, with an empty message.
    bool found_unlisted = false;
    for (const nlohmann::json &item : list)
        if (item["code"] == CLI_FILE_NOTFOUND) {
            found_unlisted = true;
            CHECK(item["message"] == "");
        }
    CHECK(found_unlisted);

    const std::string text = cli_exit_codes_text(messages);
    CHECK(text.find("Code") != std::string::npos);
    CHECK(text.find("CLI_INVALID_PARAMS") != std::string::npos);
    CHECK(text.find("Invalid parameters to the slicer.") != std::string::npos);
    size_t lines = 0;
    for (char c : text)
        lines += c == '\n';
    CHECK(lines == cli_exit_code_names().size() + 1); // header + one line per code
}

namespace {

using Mode = PrintEstimatedStatistics::ETimeMode;

// Filament 0 prints the model (flush and tower too), filament 1 the support, filament 3 is a second
// extruder's slot. Volumes in mm3; 1.75 mm filament.
void fill_result(GCodeProcessorResult &r)
{
    r.reset();
    r.filament_densities = {1.24f, 1.27f, 1.0f, 1.24f};
    r.filament_diameters = {1.75f, 1.75f, 1.75f, 1.75f};
    r.filament_costs     = {20.f, 30.f, 0.f, 25.f};
    r.has_filament_costs = true;
    r.time_cost          = 1.5;
    r.has_time_cost      = true;
    r.extruder_colors    = {"#FF0000", "#00FF00", "#0000FF", "#FFFFFF"};
    auto &ps = r.print_statistics;
    ps.modes[size_t(Mode::Normal)].time          = 7200.f;
    ps.modes[size_t(Mode::Normal)].layers_times  = std::vector<float>(5, 1.f);
    ps.model_volumes_per_extruder      = {{0, 10000.}, {3, 1000.}};
    ps.support_volumes_per_extruder    = {{1, 500.}};
    ps.flush_per_filament              = {{0, 300.}};
    ps.wipe_tower_volumes_per_extruder = {{0, 100.}};
    ps.total_volumes_per_extruder      = {{0, 10400.}, {1, 500.}, {3, 1000.}};
}

} // namespace

TEST_CASE("result.json usage: layers, filaments, extruders and cost", "[CLI][Output]")
{
    GCodeProcessorResult r;
    fill_result(r);

    std::vector<CliFilamentSlot> slots(4);
    slots[0] = {"Generic PLA", "PLA", "#112233", 1};
    slots[1] = {"Generic PVA", "PVA", "", 1};
    slots[3] = {"Generic PETG", "PETG", "#445566", 2};

    const nlohmann::json j = cli_plate_usage_json(r, slots);

    CHECK(j["layers"] == 5);

    REQUIRE(j["filaments"].is_array());
    REQUIRE(j["filaments"].size() == 3); // slot 2 extruded nothing
    const nlohmann::json &f0 = j["filaments"][0];
    CHECK(f0["index"] == 0);
    CHECK(f0["preset"] == "Generic PLA");
    CHECK(f0["type"] == "PLA");
    CHECK(f0["colour"] == "#112233");
    CHECK(f0["extruder"] == 1);
    CHECK_THAT(f0["mm3"].get<double>(), WithinAbs(10400., 0.01));
    CHECK_THAT(f0["grams"].get<double>(), WithinAbs(10400. * 0.001 * 1.24, 1e-3));
    CHECK_THAT(f0["mm"].get<double>(), WithinAbs(10400. / (3.14159265358979 * 0.875 * 0.875), 0.01));
    CHECK_THAT(f0["cost"].get<double>(), WithinAbs(10400. * 0.001 * 1.24 * 20. * 0.001, 1e-4));
    CHECK_THAT(f0["price_per_kg"].get<double>(), WithinAbs(20., 1e-9));
    CHECK_THAT(f0["flush_g"].get<double>(), WithinAbs(300. * 0.001 * 1.24, 1e-3));
    CHECK_THAT(f0["tower_g"].get<double>(), WithinAbs(100. * 0.001 * 1.24, 1e-3));

    // No colour in the slot: the result's own extruder colour fills in.
    const nlohmann::json &f1 = j["filaments"][1];
    CHECK(f1["index"] == 1);
    CHECK(f1["colour"] == "#00FF00");
    CHECK_THAT(f1["support_g"].get<double>(), WithinAbs(500. * 0.001 * 1.27, 1e-3));
    CHECK(j["filaments"][2]["index"] == 3);
    CHECK(j["filaments"][2]["extruder"] == 2);

    // Per physical extruder: filaments 0 and 1 on extruder 1, filament 3 on extruder 2.
    REQUIRE(j["extruders"].size() == 2);
    CHECK(j["extruders"][0]["index"] == 1);
    CHECK(j["extruders"][0]["filaments"] == nlohmann::json::array({0, 1}));
    CHECK_THAT(j["extruders"][0]["mm3"].get<double>(), WithinAbs(10900., 0.01));
    CHECK(j["extruders"][1]["index"] == 2);
    CHECK(j["extruders"][1]["filaments"] == nlohmann::json::array({3}));
    CHECK_THAT(j["extruders"][1]["grams"].get<double>(), WithinAbs(1000. * 0.001 * 1.24, 1e-3));

    // Cost: material is the lines' sum, machine is 2 h * 1.5 per hour.
    CHECK(j["cost"]["prices_known"] == true);
    CHECK(j["cost"]["machine_rate_known"] == true);
    CHECK_THAT(j["cost"]["machine"].get<double>(), WithinAbs(3.0, 1e-9));
    double lines_cost = 0.;
    for (const nlohmann::json &f : j["filaments"])
        lines_cost += f["cost"].get<double>();
    CHECK_THAT(j["cost"]["material"].get<double>(), WithinAbs(lines_cost, 1e-3));
    CHECK_THAT(j["cost"]["total"].get<double>(), WithinAbs(lines_cost + 3.0, 1e-3));
}

TEST_CASE("result.json usage tolerates missing slot info and an empty result", "[CLI][Output]")
{
    GCodeProcessorResult r;
    fill_result(r);
    // No slot info at all: preset, type and colour come back empty or from the result; extruder defaults to 1.
    const nlohmann::json j = cli_plate_usage_json(r, {});
    REQUIRE(j["filaments"].size() == 3);
    CHECK(j["filaments"][0]["preset"] == "");
    CHECK(j["filaments"][0]["extruder"] == 1);
    CHECK(j["extruders"].size() == 1);

    GCodeProcessorResult empty;
    empty.reset();
    const nlohmann::json e = cli_plate_usage_json(empty, {});
    CHECK(e["layers"] == 0);
    CHECK(e["filaments"].empty());
    CHECK(e["extruders"].empty());
    CHECK(e["cost"]["prices_known"] == false);
}

TEST_CASE("result.json totals add the plates up", "[CLI][Output]")
{
    nlohmann::json plates = nlohmann::json::array();
    {
        nlohmann::json p;
        p["time_s"]       = 100.5;
        p["layers"]       = 10;
        p["filament_g"]   = 2.5;
        p["filament_mm3"] = 2000.;
        p["filaments"]    = nlohmann::json::array({nlohmann::json{{"mm", 800.}}, nlohmann::json{{"mm", 100.5}}});
        p["cost"]         = nlohmann::json{{"display_total", 1.25}};
        plates.push_back(p);
    }
    {
        nlohmann::json p; // an old-style entry without the new keys still counts
        p["time_s"]       = 50.;
        p["filament_g"]   = 1.;
        p["filament_mm3"] = 500.;
        plates.push_back(p);
    }
    const nlohmann::json t = cli_result_totals(plates);
    CHECK(t["plates"] == 2);
    CHECK_THAT(t["time_s"].get<double>(), WithinAbs(150.5, 1e-9));
    CHECK(t["layers"] == 10);
    CHECK_THAT(t["filament_g"].get<double>(), WithinAbs(3.5, 1e-9));
    CHECK_THAT(t["filament_mm"].get<double>(), WithinAbs(900.5, 1e-9));
    CHECK_THAT(t["filament_mm3"].get<double>(), WithinAbs(2500., 1e-9));
    CHECK_THAT(t["cost"].get<double>(), WithinAbs(1.25, 1e-9));

    const nlohmann::json none = cli_result_totals(nlohmann::json::array());
    CHECK(none["plates"] == 0);
    CHECK(none["cost"] == 0.);
    CHECK(cli_result_totals(nlohmann::json())["plates"] == 0);
}

TEST_CASE("--export-stls names each object's file with one .stl", "[CLI][Output][Regression]")
{
    // find_last_of(".stl") took the extension as a set of characters: a name ending in s, t or l got none.
    CHECK(cli_object_file_name("MC Arms", 1, ".stl") == "obj_1_MC Arms.stl");
    CHECK(cli_object_file_name("Bolt", 2, ".stl") == "obj_2_Bolt.stl");
    CHECK(cli_object_file_name("Wheel", 3, ".stl") == "obj_3_Wheel.stl");
    CHECK(cli_object_file_name("Part.", 4, ".stl") == "obj_4_Part..stl");
    CHECK(cli_object_file_name("Cube", 5, ".stl") == "obj_5_Cube.stl");
    // A name that already ends in the extension keeps it once, whatever its case.
    CHECK(cli_object_file_name("box.stl", 6, ".stl") == "obj_6_box.stl");
    CHECK(cli_object_file_name("BOX.STL", 7, ".stl") == "obj_7_BOX.STL");
    CHECK(cli_object_file_name("box.obj", 8, ".stl") == "obj_8_box.obj.stl");
    CHECK(cli_object_file_name("Arms", 9, ".obj") == "obj_9_Arms.obj");
}
