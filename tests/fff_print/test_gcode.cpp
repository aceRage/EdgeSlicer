#include <catch2/catch.hpp>

#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <vector>
#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#include "libslic3r/GCode.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/calib.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "test_data.hpp"

#include <cmath>

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("Origin manipulation", "[GCode]") {
	Slic3r::GCode gcodegen;
	WHEN("set_origin to (10,0)") {
    	gcodegen.set_origin(Vec2d(10,0));
    	REQUIRE(gcodegen.origin() == Vec2d(10, 0));
    }
	WHEN("set_origin to (10,0) and translate by (5, 5)") {
		gcodegen.set_origin(Vec2d(10,0));
		gcodegen.set_origin(gcodegen.origin() + Vec2d(5, 5));
		THEN("origin returns reference to point") {
    		REQUIRE(gcodegen.origin() == Vec2d(15,5));
    	}
    }
}

// Orca #15755 (selective): U1 end-G-code metadata from the sliced per-filament flow type.
static std::string slice_volume_type_end_gcode(const std::vector<int> &filament_vts,
                                               const std::vector<int> &nozzle_vts = {})
{
    DynamicPrintConfig config    = DynamicPrintConfig::full_print_config();
    const unsigned     n         = unsigned(std::max<size_t>(filament_vts.size(), 2));
    const unsigned     extruders = unsigned(std::max<size_t>(nozzle_vts.size(), 2));
    config.set_num_extruders(extruders);
    config.set_num_filaments(n);
    // GCode.cpp sizes filament_volume_type_list by physical filament_type (not
    // MixedFilamentManager virtual IDs). set_num_filaments() does not resize
    // filament_type, so pin it to the physical count the helper is testing.
    {
        auto *ft = config.option<ConfigOptionStrings>("filament_type", true);
        REQUIRE(ft != nullptr);
        ft->values.assign(n, std::string("PLA"));
    }
    config.set_deserialize_strict({
        { "machine_end_gcode",              "; TEST_FVT = {filament_volume_type_list}" },
        { "machine_start_gcode",            "" },
        { "single_extruder_multi_material", "0" },
        { "layer_height",                   "0.2" },
        { "initial_layer_print_height",     "0.2" },
    });
    if (!filament_vts.empty()) {
        auto *fvt = config.option<ConfigOptionEnumsGeneric>("filament_volume_type", true);
        REQUIRE(fvt != nullptr);
        fvt->values = filament_vts;
    }
    if (!nozzle_vts.empty()) {
        auto *nvt = config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true);
        REQUIRE(nvt != nullptr);
        nvt->values = nozzle_vts;
    }

    TriangleMesh a = mesh(TestMesh::cube_20x20x20);
    TriangleMesh b = mesh(TestMesh::cube_20x20x20);
    b.translate(30.f, 0.f, 0.f);
    return slice({ a, b }, config);
}

TEST_CASE("filament_volume_type_list is emitted in end G-code from filament_volume_type", "[GCode][U1]")
{
    SECTION("mixed Standard / High Flow") {
        const std::string gcode = slice_volume_type_end_gcode({ int(fvtStandard), int(fvtHighFlow) });
        REQUIRE(gcode.find("; TEST_FVT = standard,high_flow\n") != std::string::npos);
    }
    SECTION("defaults are standard,standard") {
        const std::string gcode = slice_volume_type_end_gcode({});
        REQUIRE(gcode.find("; TEST_FVT = standard,standard\n") != std::string::npos);
    }
    SECTION("nozzle high_flow does not override filament standard") {
        const std::string gcode = slice_volume_type_end_gcode({ int(fvtStandard), int(fvtStandard) },
                                                             { int(nvtStandard), int(nvtHighFlow) });
        REQUIRE(gcode.find("; TEST_FVT = standard,standard\n") != std::string::npos);
    }
    SECTION("extra filaments follow filament_volume_type") {
        const std::string gcode = slice_volume_type_end_gcode({ int(fvtStandard), int(fvtHighFlow), int(fvtStandard) });
        REQUIRE(gcode.find("; TEST_FVT = standard,high_flow,standard\n") != std::string::npos);
    }
}

// Some firmwares only scan the last N lines of the file for "estimated printing time", so it
// must stay close to EOF regardless of the resolved-settings config block's size.
TEST_CASE("Estimated printing time comment follows the config block", "[GCode]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const std::string  gcode  = slice({TestMesh::cube_20x20x20}, config);
    const size_t       config_block_end = gcode.find("; CONFIG_BLOCK_END");
    const size_t       time_comment     = gcode.rfind("; estimated printing time");
    REQUIRE(config_block_end != std::string::npos);
    REQUIRE(time_comment != std::string::npos);
    REQUIRE(time_comment > config_block_end);
}

// FanMover (active when fan_speedup_time or fan_kickstart != 0) can split a G1 to insert an
// early fan command. GCode::set_extruder must bracket change_filament_gcode so those travels
// stay intact. FanMover keys off a "; custom gcode" prefix and ignores comments shorter than
// 17 chars, so the start marker is "; custom gcode start".
TEST_CASE("Toolchange custom gcode is not split by FanMover", "[GCode][FanMover]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    config.set_key_value("change_filament_gcode", new ConfigOptionString("G1 X10 F5000\nG1 X70 F5000"));
    config.set_deserialize_strict({
        {"skirt_loops",           0},
        {"brim_type",             "no_brim"},
        {"print_sequence",        "by object"},
        {"fan_speedup_time",      0.5},
        {"fan_kickstart",         0.5},
        {"fan_speedup_overhangs", 0},
        {"machine_start_gcode",   ""},
    });
    config.option<ConfigOptionInts>("close_fan_the_first_x_layers")->values = {0, 0};
    config.option<ConfigOptionFloats>("fan_min_speed")->values              = {50., 50.};

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(model.objects.size() == 2);
    model.objects[1]->volumes.front()->config.set("extruder", 2);
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);

    const size_t start = gcode.find("; custom gcode start");
    REQUIRE(start != std::string::npos);
    const size_t end = gcode.find("; custom gcode end", start);
    REQUIRE(end != std::string::npos);
    CHECK(gcode.substr(start, end - start).find("G1 X10 F5000\nG1 X70 F5000") != std::string::npos);
}

// Orca #15904 Type1 delta: WipeTowerIntegration::append_tcr must bracket change_filament_gcode
// the same way set_extruder already does (#170). Type1 is the Bambu planner (is_BBL_printer).
// ;_FORCE_RESUME_FAN_SPEED, the retract prefix and the #15441 Z-restore stay outside the span.
TEST_CASE("Type1 wipe-tower toolchange custom gcode is not split by FanMover", "[GCode][FanMover][WipeTower]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = true;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values                = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values                = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value             = 35.;
    config.set_key_value("change_filament_gcode", new ConfigOptionString("G1 X10 F5000\nG1 X70 F5000"));
    config.set_deserialize_strict({
        {"skirt_loops",                0},
        {"brim_type",                  "no_brim"},
        {"fan_speedup_time",           0.5},
        {"fan_kickstart",              0.5},
        {"fan_speedup_overhangs",      0},
        {"machine_start_gcode",        ""},
        {"wipe_tower_wall_type",       "rectangle"},
        {"layer_height",               "0.2"},
        {"initial_layer_print_height", "0.2"},
    });
    config.option<ConfigOptionInts>("close_fan_the_first_x_layers")->values = {0, 0};
    config.option<ConfigOptionFloats>("fan_min_speed")->values              = {50., 50.};

    Print print;
    Model model;
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 40., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 40., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);
    print.apply(model, config);
    print.is_BBL_printer() = true;
    print.set_status_silent();
    REQUIRE(print.has_wipe_tower());

    const std::string gcode = Test::gcode(print);
    REQUIRE(gcode.find("CP TOOLCHANGE") != std::string::npos);

    const size_t start = gcode.find("; custom gcode start");
    REQUIRE(start != std::string::npos);
    const size_t end = gcode.find("; custom gcode end", start);
    REQUIRE(end != std::string::npos);
    const std::string span = gcode.substr(start, end - start);
    CHECK(span.find("G1 X10 F5000\nG1 X70 F5000") != std::string::npos);
    CHECK(span.find(";_FORCE_RESUME_FAN_SPEED") == std::string::npos);
    const size_t resume = gcode.find(";_FORCE_RESUME_FAN_SPEED", end);
    REQUIRE(resume != std::string::npos);
}

TEST_CASE("Klipper object labels name each copy without the characters Klipper cannot parse", "[GCode]")
{
    const std::pair<const char *, const char *> cases[] = {
        {"my part (2)", "my_part_2"},
        {"(cube)",      "cube"},
    };
    for (const auto &[name, label] : cases) {
        DYNAMIC_SECTION(name) {
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_deserialize_strict({
                {"gcode_flavor",   "klipper"},
                {"exclude_object", "1"},
                {"printable_area", "0x0,400x0,400x400,0x400"},
            });
            Print print;
            Model model;
            init_print({TestMesh::cube_20x20x20}, print, model, config);
            REQUIRE(model.objects.size() == 1);
            model.objects.front()->name = name;
            model.objects.front()->add_instance()->set_offset(Vec3d(40., 0., 0.));
            print.apply(model, config);

            const std::string gcode = Test::gcode(print);
            for (const char *copy : {"0", "1"}) {
                const std::string instance_label = std::string(label) + "_id_0_copy_" + copy;
                INFO(instance_label);
                CHECK(gcode.find("EXCLUDE_OBJECT_DEFINE NAME=" + instance_label + " ") != std::string::npos);
                CHECK(gcode.find("EXCLUDE_OBJECT_START NAME=" + instance_label + "\n") != std::string::npos);
            }
        }
    }
}

namespace {

float second_move_extrusion(GCodeReader &reader)
{
    float extrusion = -1.f;
    reader.parse_buffer("G1 X1 E1\nG1 X2 E1\n", [&extrusion](GCodeReader &reader, const GCodeReader::GCodeLine &line) {
        extrusion = line.dist_E(reader);
    });
    return extrusion;
}

} // namespace

TEST_CASE("A G-code reader measures extrusion as relative or absolute as its config says", "[GCodeReader]")
{
    for (const bool relative : {false, true}) {
        DYNAMIC_SECTION((relative ? "relative E" : "absolute E")) {
            const float expected = relative ? 1.f : 0.f;

            GCodeConfig config;
            config.use_relative_e_distances.value = relative;
            GCodeReader applied;
            applied.apply_config(config);
            CHECK(applied.config().use_relative_e_distances.value == relative);
            CHECK(std::abs(second_move_extrusion(applied) - expected) < 1e-6f);

            DynamicPrintConfig dynamic;
            dynamic.set_key_value("use_relative_e_distances", new ConfigOptionBool(relative));
            GCodeReader applied_dynamic;
            applied_dynamic.apply_config(dynamic);
            CHECK(std::abs(second_move_extrusion(applied_dynamic) - expected) < 1e-6f);

            GCodeReader copy = applied;
            CHECK(copy.config().use_relative_e_distances.value == relative);
            CHECK(std::abs(second_move_extrusion(copy) - expected) < 1e-6f);
        }
    }
}

TEST_CASE("A G-code reader without a config uses the default one", "[GCodeReader]")
{
    const bool  relative = GCodeConfig().use_relative_e_distances.value;
    GCodeReader reader;
    CHECK(reader.config().use_relative_e_distances.value == relative);
    CHECK(std::abs(second_move_extrusion(reader) - (relative ? 1.f : 0.f)) < 1e-6f);
}

// Orca #11462: an outer wall whose speed falls inside the resonance avoidance band leaves it the
// nearer way: the lower half of the band is slowed down to "min", the upper half sped up to "max".
// EDGE: the speed-up only happens when the filament's max volumetric speed allows "max".
TEST_CASE("Resonance avoidance moves outer wall speeds out of the band the nearer way", "[GCode][ResonanceAvoidance]")
{
    // Feedrates (mm/min) of the outer wall extrusions above the first layers.
    auto outer_wall_feedrates = [](double outer_wall_speed, double max_volumetric_speed) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            {"resonance_avoidance",           "1"},
            {"min_resonance_avoidance_speed", "70"},
            {"max_resonance_avoidance_speed", "120"},
            {"outer_wall_speed",              std::to_string(outer_wall_speed)},
            {"filament_max_volumetric_speed", std::to_string(max_volumetric_speed)},
            {"slow_down_layer_time",          "0"},
            {"slow_down_layers",              "0"},
            {"skirt_loops",                   "0"},
            {"brim_type",                     "no_brim"},
            {"gcode_flavor",                  "marlin"},
        });
        Print print;
        Model model;
        init_print({TestMesh::cube_20x20x20}, print, model, config);
        print.is_BBL_printer() = false;
        const std::string gcode = Test::gcode(print);

        std::set<int> feedrates;
        int           layer       = 0;
        bool          outer_wall  = false;
        double        feedrate    = 0.;
        GCodeReader   reader;
        reader.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
            const std::string &raw = line.raw();
            if (raw.rfind(";LAYER_CHANGE", 0) == 0)
                ++layer;
            else if (raw.rfind(";TYPE:", 0) == 0)
                outer_wall = raw.rfind(";TYPE:Outer wall", 0) == 0;
            if (!line.cmd_is("G1"))
                return;
            if (line.has_f())
                feedrate = line.f();
            if (outer_wall && layer > 3 && line.has_e() && line.e() > 0. && (line.has_x() || line.has_y()))
                feedrates.insert(int(std::lround(feedrate)));
        });
        return feedrates;
    };

    // Lower half of the band (70..95): down to "min".
    CHECK(outer_wall_feedrates(80., 200.) == std::set<int>{70 * 60});
    // Upper half (95..120): up to "max" (it used to be slowed down to "min" too).
    CHECK(outer_wall_feedrates(110., 200.) == std::set<int>{120 * 60});
    // Above the band: untouched.
    CHECK(outer_wall_feedrates(130., 200.) == std::set<int>{130 * 60});
    // Upper half, but the filament cannot be pushed through at "max" (8 mm^3/s caps them at about 105 mm/s,
    // inside the upper half): down to "min" as before rather than over the volumetric limit.
    CHECK(outer_wall_feedrates(110., 8.) == std::set<int>{70 * 60});
}

// Orca #11491: bed_mesh_probe_count counts probe points, and n intervals of at most
// bed_mesh_probe_distance need n + 1 of them. The count used to be one short, so the points of an
// adaptive mesh were spaced wider than the distance asked for.
TEST_CASE("Adaptive bed mesh probe points are no further apart than the probe distance", "[GCode][BedMesh]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"bed_mesh_probe_distance", "5x5"},
        {"skirt_loops",             "0"},
        {"brim_type",               "no_brim"},
        {"gcode_flavor",            "marlin"},
    });
    config.set_key_value("machine_start_gcode",
                         new ConfigOptionString("; MESH_SIZE {first_layer_print_size[0]} {first_layer_print_size[1]}\n"
                                                "; PROBE_COUNT {bed_mesh_probe_count[0]} {bed_mesh_probe_count[1]}\n"));
    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    const std::string gcode = Test::gcode(print);

    const size_t size_pos  = gcode.find("; MESH_SIZE ");
    const size_t count_pos = gcode.find("; PROBE_COUNT ");
    REQUIRE(size_pos != std::string::npos);
    REQUIRE(count_pos != std::string::npos);
    double size_x = 0., size_y = 0.;
    int    count_x = 0, count_y = 0;
    {
        std::istringstream in(gcode.substr(size_pos + 12, gcode.find('\n', size_pos) - size_pos - 12));
        in >> size_x >> size_y;
    }
    {
        std::istringstream in(gcode.substr(count_pos + 14, gcode.find('\n', count_pos) - count_pos - 14));
        in >> count_x >> count_y;
    }
    CAPTURE(size_x, size_y, count_x, count_y);
    REQUIRE(size_x > 15.);
    REQUIRE(size_y > 15.);
    // Spacing within the distance, and no more points than that needs.
    CHECK(size_x / (count_x - 1) <= 5. + EPSILON);
    CHECK(size_y / (count_y - 1) <= 5. + EPSILON);
    CHECK(count_x == int(std::ceil(size_x / 5.)) + 1);
    CHECK(count_y == int(std::ceil(size_y / 5.)) + 1);
}

// Orca #13703: on a multi-toolhead printer (not SEMM, no multi-tool ramming) using the Type 2
// wipe tower (WipeTower2, every non-BBL printer such as the U1), the tool change is emitted in
// place unless "tool_change_on_wipe_tower" is on; then the head travels to the tower first.
// Default off, so the default G-code is unchanged.
static std::string slice_toolchanger_two_colour(bool tool_change_on_wipe_tower)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values    = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values     = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values      = {0.4, 0.4};
    config.option<ConfigOptionBools>("filament_multitool_ramming")->values = {false, false};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = true;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values                = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values                = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value             = 35.;
    config.option<ConfigOptionBool>("tool_change_on_wipe_tower")->value      = tool_change_on_wipe_tower;
    config.set_deserialize_strict({
        {"skirt_loops",                0},
        {"brim_type",                  "no_brim"},
        {"gcode_comments",             "1"},
        {"machine_start_gcode",        ""},
        {"change_filament_gcode",      ""},
        {"layer_height",               "0.2"},
        {"initial_layer_print_height", "0.2"},
    });

    Print print;
    Model model;
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 40., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 40., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE_FALSE(print.is_BBL_printer());
    REQUIRE(print.has_wipe_tower());
    return Test::gcode(print);
}

static size_t count_substr(const std::string &s, const std::string &needle)
{
    size_t n = 0;
    for (size_t pos = s.find(needle); pos != std::string::npos; pos = s.find(needle, pos + needle.size()))
        ++n;
    return n;
}

static size_t count_tool_lines(const std::string &gcode)
{
    size_t             n = 0;
    std::istringstream in(gcode);
    std::string        line;
    while (std::getline(in, line))
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])))
            ++n;
    return n;
}

TEST_CASE("Tool change on wipe tower forces the travel to the Type 2 tower before Tx", "[GCode][WipeTower][U1C]")
{
    const std::string off = slice_toolchanger_two_colour(false);
    const std::string on  = slice_toolchanger_two_colour(true);
    const size_t travels_off = count_substr(off, "Travel to a Wipe Tower");
    const size_t travels_on  = count_substr(on, "Travel to a Wipe Tower");
    const size_t tools_off   = count_tool_lines(off);
    const size_t tools_on    = count_tool_lines(on);
    CAPTURE(travels_off, travels_on, tools_off, tools_on);
    REQUIRE(tools_off > 2);
    // Same tool changes either way; the option only adds the travel.
    CHECK(tools_on == tools_off);
    CHECK(travels_on > travels_off);
    // Every tool change after the first one now goes through the tower.
    CHECK(travels_on + 1 >= tools_on);
}

// Orca #16422: firmwares read the compatibility header lines, so they must describe the filament
// the plate prints, not filament 1. Edge keeps first_layer_bed_temperature as the max of used
// filaments (U1 always heats to the highest bed temp).
static DynamicPrintConfig two_filament_header_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "nozzle_diameter",                   "0.4,0.4" },
        { "filament_diameter",                 "1.75,1.75" },
        { "enable_prime_tower",                "0" },
        { "single_extruder_multi_material",    "1" },
        { "nozzle_temperature_initial_layer",  "200,250" },
        { "nozzle_temperature",                "200,250" },
        { "supertack_plate_temp_initial_layer","60,100" },
        { "cool_plate_temp_initial_layer",     "60,100" },
        { "textured_cool_plate_temp_initial_layer", "60,100" },
        { "eng_plate_temp_initial_layer",      "60,100" },
        { "hot_plate_temp_initial_layer",      "60,100" },
        { "textured_plate_temp_initial_layer", "60,100" },
        { "skirt_loops",                       "0" },
        { "brim_type",                         "no_brim" },
        { "enable_support",                    "0" },
        { "layer_change_gcode",                "G92 E0\n" },
    });
    config.option<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#0000FF" };
    return config;
}

static int header_int(const std::string &gcode, const std::string &key)
{
    const std::string token = "; " + key + " = ";
    const size_t pos = gcode.find(token);
    REQUIRE(pos != std::string::npos);
    return std::stoi(gcode.substr(pos + token.size()));
}

TEST_CASE("The G-code header names the first layer temperatures of the filament the plate prints", "[GCode][U1]")
{
    DynamicPrintConfig config = two_filament_header_config();
    config.set_deserialize_strict({
        { "wall_filament",          "2" },
        { "sparse_infill_filament", "2" },
        { "solid_infill_filament",  "2" },
    });
    const std::string gcode = slice({ TestMesh::cube_20x20x20 }, config);
    CHECK(header_int(gcode, "first_layer_bed_temperature") == 100);
    CHECK(header_int(gcode, "first_layer_temperature") == 250);
}

TEST_CASE("The BBL G-code header names the first layer temperatures on a single-filament plate", "[GCode][U1]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_prime_tower",                     "0" },
        { "nozzle_temperature_initial_layer",       "215" },
        { "nozzle_temperature",                     "215" },
        { "supertack_plate_temp_initial_layer",     "55" },
        { "cool_plate_temp_initial_layer",          "55" },
        { "textured_cool_plate_temp_initial_layer", "55" },
        { "eng_plate_temp_initial_layer",           "55" },
        { "hot_plate_temp_initial_layer",           "55" },
        { "textured_plate_temp_initial_layer",      "55" },
        { "skirt_loops",                            "0" },
        { "brim_type",                              "no_brim" },
        { "enable_support",                         "0" },
        { "layer_change_gcode",                     "G92 E0\n" },
    });

    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config);
    print.is_BBL_printer() = true;
    REQUIRE(print.extruders(true).size() == 1);

    const std::string gcode = Test::gcode(print);
    CHECK(header_int(gcode, "first_layer_bed_temperature") == 55);
    CHECK(header_int(gcode, "first_layer_temperature") == 215);
}

TEST_CASE("The G-code header names the first object's filament when printing by object", "[GCode][U1]")
{
    DynamicPrintConfig config = two_filament_header_config();
    config.set_deserialize_strict({ { "print_sequence", "by object" } });

    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20, TestMesh::cube_20x20x20 }, print, model, config);
    REQUIRE(model.objects.size() == 2);
    model.objects[0]->config.set("extruder", 2);
    model.objects[1]->config.set("extruder", 1);
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);

    const size_t first_t1 = gcode.find("\nT1");
    const size_t first_t0 = gcode.find("\nT0");
    REQUIRE(first_t1 != std::string::npos);
    REQUIRE(first_t0 != std::string::npos);
    REQUIRE(first_t1 < first_t0);

    CHECK(header_int(gcode, "first_layer_bed_temperature") == 100);
    CHECK(header_int(gcode, "first_layer_temperature") == 250);
}

// Orca #12781: wipe_tower_type. Bambu Lab printers always get the Type1 tower; every other printer
// follows the option, Type2 by default, and a Type1 override slices with the Bambu-style tower.
namespace {

DynamicPrintConfig two_filament_tower_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = true;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values                = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values                = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value             = 35.;
    config.set_deserialize_strict({
        {"skirt_loops",                0},
        {"brim_type",                  "no_brim"},
        {"machine_start_gcode",        ""},
        {"wipe_tower_wall_type",       "rectangle"},
        {"layer_height",               "0.2"},
        {"initial_layer_print_height", "0.2"},
    });
    return config;
}

void two_cube_model(Model &model)
{
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 40., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 40., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);
}

} // namespace

TEST_CASE("Print::wipe_tower_type is Type1 on Bambu Lab printers and follows the option elsewhere", "[GCode][WipeTower][WipeTowerType]")
{
    for (const char *value : {"type1", "type2"}) {
        DYNAMIC_SECTION(value) {
            DynamicPrintConfig config = two_filament_tower_config();
            config.set_deserialize_strict("wipe_tower_type", value);
            Print print;
            Model model;
            two_cube_model(model);
            print.apply(model, config);

            print.is_BBL_printer() = true;
            CHECK(print.wipe_tower_type() == WipeTowerType::Type1);

            print.is_BBL_printer() = false;
            CHECK(print.wipe_tower_type() == (std::string(value) == "type1" ? WipeTowerType::Type1 : WipeTowerType::Type2));
        }
    }

    SECTION("default") {
        DynamicPrintConfig config = two_filament_tower_config();
        Print print;
        Model model;
        two_cube_model(model);
        print.apply(model, config);
        CHECK(print.wipe_tower_type() == WipeTowerType::Type2);
    }
}

TEST_CASE("A Type1 override on a non-Bambu printer slices with compatible G-code tags", "[GCode][WipeTower][WipeTowerType]")
{
    auto slice = [](const char *type, bool bbl) {
        DynamicPrintConfig config = two_filament_tower_config();
        config.set_deserialize_strict("wipe_tower_type", type);
        Print print;
        Model model;
        two_cube_model(model);
        print.apply(model, config);
        print.is_BBL_printer() = bbl;
        print.set_status_silent();
        REQUIRE(print.has_wipe_tower());
        return Test::gcode(print);
    };

    // The tower G-code of the whole print: every line between a toolchange start and its end. The
    // header (time stamp) and the config block (which names the option) are left out.
    auto tower = [](const std::string &gcode) {
        std::string out;
        for (size_t start = gcode.find("; CP TOOLCHANGE START"); start != std::string::npos;
             start = gcode.find("; CP TOOLCHANGE START", start + 1)) {
            const size_t end = gcode.find("; CP TOOLCHANGE END", start);
            if (end == std::string::npos)
                break;
            out += gcode.substr(start, end - start);
        }
        return out;
    };

    const std::string type1 = slice("type1", false);
    const std::string type2 = slice("type2", false);
    REQUIRE(! tower(type1).empty());
    REQUIRE(! tower(type2).empty());
    // The override changes the tower.
    CHECK(tower(type1) != tower(type2));
    // Edge: the Type1 tower writes the tags the non-Bambu export reads, so the preview and the
    // statistics see it as a prime tower.
    CHECK(type1.find(";TYPE:Prime tower") != std::string::npos);
    CHECK(type1.find("; FEATURE: Prime tower") == std::string::npos);

    // A Bambu Lab printer ignores the option and keeps its own tags.
    const std::string bbl_type2 = slice("type2", true);
    CHECK(bbl_type2.find("; FEATURE: Prime tower") != std::string::npos);
    CHECK(bbl_type2.find(";TYPE:Prime tower") == std::string::npos);
    CHECK(tower(bbl_type2) == tower(slice("type1", true)));
}

// ORCA #16719: upstream also rewrote the E conversion in _extrude; Edge keeps its division by the
// resolved flow ratio (per-filament flow, U1 high-flow design) and only adds the validation. The ratio
// must still scale the extruded length exactly once.
TEST_CASE("The filament flow ratio scales the extruded length once", "[GCode]")
{
    // Retraction off, so every positive relative E move is extrusion and scales with the ratio.
    auto extruded_length = [](double flow_ratio) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({ { "use_relative_e_distances", "1" }, { "retraction_length", "0" }, { "wipe", "0" } });
        config.option<ConfigOptionFloats>("filament_flow_ratio")->values = { flow_ratio };
        double total = 0.;
        GCodeReader reader;
        reader.parse_buffer(slice({ TestMesh::cube_20x20x20 }, config),
                            [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
                                if ((line.cmd_is("G1") || line.cmd_is("G2") || line.cmd_is("G3")) && line.has_e() && line.e() > 0.f)
                                    total += line.e();
                            });
        return total;
    };

    const double full = extruded_length(1.);
    REQUIRE(full > 0.);
    const double ratio = extruded_length(0.9) / full;
    CHECK(std::abs(ratio - 0.9) < 0.9e-3);
}

// Orca #16898: a temperature tower re-targets the hotend at every layer. On a multi-tool
// printer that command has to name the tool printing the tower: a bare M104 goes to
// whichever tool the firmware considers active. Edge maps the filament through
// temperature_tool_for_filament (physical hotend on U1 / Bambu). A single-tool printer
// and SEMM keep the bare form — the writer already drops T for those.
TEST_CASE("The temperature tower steps the temperature of the tool printing it", "[GCode][Calib]")
{
    const bool multi_tool = GENERATE(false, true);
    DYNAMIC_SECTION((multi_tool ? "two tools, the tower on the second one (filament 2 -> T1)" : "one tool")) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        if (multi_tool) {
            config.set_num_extruders(2);
            config.set_num_filaments(2);
            config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
            config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
            config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
        }
        config.set_deserialize_strict({
            {"skirt_loops",                      0},
            {"brim_type",                        "no_brim"},
            {"single_extruder_multi_material",   0},
            {"enable_prime_tower",               0},
            {"nozzle_temperature_initial_layer", multi_tool ? "230,230" : "230"},
            {"nozzle_temperature",               multi_tool ? "230,230" : "230"},
            // Without the end G-code's heater-off M104 S0, every temperature command after the
            // first layer change is the tower's. Edge steps 5 °C every 10.001 mm of Z
            // (start - floor(z / 10.001) * 5), so a 20 mm cube is the 230 and 225 bands.
            {"machine_end_gcode",                ""},
        });

        Print print;
        Model model;
        init_print({TestMesh::cube_20x20x20}, print, model, config);
        if (multi_tool) {
            REQUIRE(model.objects.size() == 1);
            model.objects.front()->config.set("extruder", 2);
            print.apply(model, config);
        }
        Calib_Params params;
        params.mode  = CalibMode::Calib_Temp_Tower;
        params.start = 230;
        params.end   = 190;
        params.step  = 5;
        print.set_calib_params(params);
        const std::string gcode = Test::gcode(print);

        const size_t layers_begin = gcode.find(";LAYER_CHANGE");
        REQUIRE(layers_begin != std::string::npos);
        const std::regex      temperature_command(R"(^M10[49] S(\d+)(?: T(\d+))?)");
        std::set<int>         temperatures;
        std::set<std::string> tools;
        std::istringstream    stream(gcode.substr(layers_begin));
        for (std::string line; std::getline(stream, line);) {
            std::smatch m;
            if (!std::regex_search(line, m, temperature_command))
                continue;
            temperatures.insert(std::stoi(m[1]));
            tools.insert(m[2].matched ? m[2].str() : "none");
        }
        CHECK(temperatures == std::set<int>{225, 230});
        CHECK(tools == std::set<std::string>{multi_tool ? "1" : "none"});
    }
}

static DynamicPrintConfig two_filament_bed_temp_config(const std::string &first_layer_temps, const std::string &later_temps)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        {"nozzle_diameter",                        "0.4,0.4"},
        {"filament_diameter",                      "1.75,1.75"},
        {"enable_prime_tower",                     "0"},
        {"single_extruder_multi_material",         "1"},
        {"curr_bed_type",                          "High Temp Plate"},
        {"supertack_plate_temp_initial_layer",     first_layer_temps},
        {"cool_plate_temp_initial_layer",          first_layer_temps},
        {"textured_cool_plate_temp_initial_layer", first_layer_temps},
        {"eng_plate_temp_initial_layer",           first_layer_temps},
        {"hot_plate_temp_initial_layer",           first_layer_temps},
        {"textured_plate_temp_initial_layer",      first_layer_temps},
        {"supertack_plate_temp",                   later_temps},
        {"cool_plate_temp",                        later_temps},
        {"textured_cool_plate_temp",               later_temps},
        {"eng_plate_temp",                         later_temps},
        {"hot_plate_temp",                         later_temps},
        {"textured_plate_temp",                    later_temps},
        {"skirt_loops",                            "0"},
        {"brim_type",                              "no_brim"},
        {"layer_height",                           "0.2"},
        {"initial_layer_print_height",             "0.2"},
        {"layer_change_gcode",                     "G92 E0\n"},
        {"machine_start_gcode",                    "M190 S{bed_temperature_initial_layer_single}\n"},
    });
    config.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000", "#0000FF"};
    return config;
}

static std::vector<int> bed_command_temps(const std::string &gcode)
{
    std::vector<int> temps;
    std::istringstream lines(gcode);
    for (std::string line; std::getline(lines, line);) {
        if (line.compare(0, 6, "M140 S") == 0 || line.compare(0, 6, "M190 S") == 0)
            temps.push_back(std::stoi(line.substr(6)));
    }
    return temps;
}

static std::string join_ints(const std::vector<int> &values)
{
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i)
            out << ',';
        out << values[i];
    }
    return out.str();
}

// Orca #16770: a filament that starts above the first layer, such as a support interface,
// never touches the bed. Heating the bed to its temperature would soften the filament the
// part stands on, so only first-layer filaments count.
TEST_CASE("The highest temperature bed policy ignores filaments that start above the first layer", "[GCode][BedTemperature]")
{
    // Filament 1: bed 65 on the first layer, 60 after it. Filament 2: bed 100. A 30 mm cap on
    // a 10 mm tall stem prints with filament 1 and is supported from the bed, the support with
    // filament 1 and its interface under the cap with filament 2.
    TriangleMesh mesh = make_cube(10, 10, 10);
    mesh.translate(10, 10, 0);
    TriangleMesh cap = make_cube(30, 30, 2);
    cap.translate(0, 0, 10);
    mesh.merge(cap);

    DynamicPrintConfig config = two_filament_bed_temp_config("65,100", "60,100");
    config.set_deserialize_strict({
        {"enable_support",             "1"},
        {"support_type",               "normal(auto)"},
        {"support_threshold_angle",    "30"},
        {"support_filament",           "1"},
        {"support_interface_filament", "2"},
    });

    const std::string gcode = Test::slice({mesh}, config);
    REQUIRE(gcode.find("\nT1") != std::string::npos);

    const std::vector<int> bed_temps = bed_command_temps(gcode);
    INFO("bed temperatures: " << join_ints(bed_temps));
    REQUIRE_FALSE(bed_temps.empty());
    CHECK(bed_temps.front() == 65);
    CHECK(std::find(bed_temps.begin(), bed_temps.end(), 60) != bed_temps.end());
    CHECK(std::find(bed_temps.begin(), bed_temps.end(), 100) == bed_temps.end());
}

// change_filament_gcode receives the flush both as a total, flush_length, and split into chunks of
// about 135 mm³, flush_length_1..4. Profiles purge the chunks, so a flush too small to round up to
// one chunk must still land in flush_length_1 instead of being dropped.
// Edge: no multifilament_config / slice_with_object_overrides harness; two cubes plus a re-apply,
// same as the by-object header case in this file. The type1 tower path also clamps purge volume to
// 100 mm³, so 30/50 mm³ only fail on the by-object path without the fix.
TEST_CASE("The flush chunks add up to the whole flush, however small", "[GCode][MultiFilament]")
{
    const std::string path   = GENERATE(as<std::string>{}, "direct", "type1");
    const double      volume = GENERATE(30., 50., 100., 300., 1000.);
    CAPTURE(path, volume);

    const std::string flush = "0," + std::to_string(volume) + "," + std::to_string(volume) + ",0";
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00"};
    config.set_deserialize_strict({
        { "change_filament_gcode", "; FLUSH {flush_length} {flush_length_1} {flush_length_2} {flush_length_3} {flush_length_4}\n" },
        { "flush_volumes_matrix",  flush },
        { "flush_multiplier",      1 },
        { "skirt_loops",           0 },
        { "brim_type",             "no_brim" },
        { "layer_change_gcode",    "G92 E0\n" },
        { "enable_support",        0 },
    });
    if (path == "direct")
        config.set_deserialize_strict({ { "print_sequence", "by object" }, { "enable_prime_tower", false } });
    else
        config.set_deserialize_strict({
            { "enable_prime_tower", true },
            { "wipe_tower_type",    "type1" },
            { "prime_tower_width",  35 },
            { "wipe_tower_x",       15 },
            { "wipe_tower_y",       15 },
        });

    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20, TestMesh::cube_20x20x20 }, print, model, config);
    REQUIRE(model.objects.size() == 2);
    model.objects[0]->config.set("extruder", 1);
    model.objects[1]->config.set("extruder", 2);
    print.apply(model, config);

    const std::string gcode = Test::gcode(print);

    size_t flushes = 0;
    std::istringstream lines(gcode);
    for (std::string line; std::getline(lines, line);) {
        double total, chunks[4];
        if (std::sscanf(line.c_str(), "; FLUSH %lf %lf %lf %lf %lf", &total, &chunks[0], &chunks[1], &chunks[2], &chunks[3]) != 5 || total <= 0.)
            continue;
        ++flushes;
        CAPTURE(line);
        CHECK(chunks[0] > 0.);
        CHECK_THAT(chunks[0] + chunks[1] + chunks[2] + chunks[3], Catch::Matchers::WithinRel(total, 1e-3));
    }
    CHECK(flushes > 0);
}

// Orca #16965: on one nozzle with several filaments, the lift limits come from the filament printing. Edge already
// read them that way (EXTRUDER_CONFIG and GCodeWriter::lift() index by the filament), so this guards the behaviour.
TEST_CASE("Z hop follows the lift settings of the filament printing", "[GCode][Retraction]")
{
    // One nozzle, two filaments. Filament 2 overrides one lift setting; filament 1 keeps the printer's default,
    // which lifts on every layer.
    const auto key_value = GENERATE(table<std::string, std::string>({
        {"filament_retract_lift_above", "nil,1"},             // only lift Z above 1 mm
        {"filament_retract_lift_enforce", "nil,Bottom Only"}, // only lift on the first layer
    }));
    const std::string &key   = std::get<0>(key_value);
    const std::string &value = std::get<1>(key_value);
    const int filament = GENERATE(1, 2);
    CAPTURE(key, filament);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    config.set_deserialize_strict({
        {"z_hop",                      "0.6"},
        {key,                          value},
        {"layer_height",               "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"skirt_loops",                0},
        {"brim_type",                  "no_brim"},
        {"machine_start_gcode",        ""},
    });

    Print print;
    Model model;
    // Two objects, so every layer travels between them with a retraction.
    init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(model.objects.size() == 2);
    for (ModelObject *object : model.objects)
        object->config.set("extruder", filament);
    print.apply(model, config);

    // The heights of the layers with a move that lifts Z above the layer.
    std::set<double>   lift_layers;
    double             layer_z = 0.;
    std::istringstream lines(Test::gcode(print));
    std::string        line;
    const std::regex   z_word(R"(\bZ([-0-9.]+))");
    while (std::getline(lines, line)) {
        if (line.rfind(";Z:", 0) == 0) {
            layer_z = std::stod(line.substr(3));
            continue;
        }
        if (layer_z <= 0. || (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0))
            continue;
        const std::string move = line.substr(0, line.find(';'));
        std::smatch       match;
        if (std::regex_search(move, match, z_word) && std::stod(match[1]) > layer_z + 0.3)
            lift_layers.insert(layer_z);
    }

    REQUIRE_FALSE(lift_layers.empty());
    const double first = *lift_layers.begin();
    const double last  = *lift_layers.rbegin();
    if (filament == 1) {
        CHECK(first < 0.2 + EPSILON);
        CHECK(last > 1.);
    } else if (key == "filament_retract_lift_above")
        CHECK(first > 1. - EPSILON);
    else
        CHECK(last < 0.2 + EPSILON);
}

// Orca #16982: a travel shorter than retraction_minimum_travel still retracts after an outer wall
// when retract_after_outer_wall is on (off by default in EdgeSlicer). Inner-outer-inner puts that short travel
// between the outer wall and the next inner wall.
TEST_CASE("Retract after outer wall retracts on a travel shorter than the threshold", "[GCode][Retraction]")
{
    const bool enabled = GENERATE(false, true);
    CAPTURE(enabled);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_flavor", "marlin2" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wall_loops", "3" },
        { "wall_sequence", "inner-outer-inner wall" },
        { "top_shell_layers", "0" },
        { "bottom_shell_layers", "0" },
        { "sparse_infill_density", "0%" },
        { "retraction_length", "0.8" },
        { "retraction_minimum_travel", "5" },
        { "retract_when_changing_layer", "0" },
        { "wipe", "0" },
        { "z_hop", "0" },
        { "gcode_comments", "1" },
        { "machine_start_gcode", "" },
        { "machine_end_gcode", "" },
        { "retract_after_outer_wall", enabled ? "1" : "0" },
    });
    const std::string output = slice({ make_cube(10., 10., 1.) }, config);

    const std::string &role_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    ExtrusionRole role = erNone;
    bool after_outer_wall = false;
    double retraction = 0.;
    size_t transitions = 0;
    GCodeReader parser;
    parser.apply_config(config);
    parser.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.comment().find(role_tag) == 0)
            role = ExtrusionEntity::string_to_role(line.comment().substr(role_tag.size()));
        if (line.extruding(self) && line.dist_XY(self) > EPSILON) {
            if (role == erExternalPerimeter) {
                after_outer_wall = true;
                retraction = 0.;
            } else if (after_outer_wall) {
                REQUIRE(role == erPerimeter);
                ++transitions;
                CHECK_THAT(retraction, Catch::Matchers::WithinAbs(enabled ? 0.8 : 0., 0.00005));
                after_outer_wall = false;
            }
        } else if (after_outer_wall && line.retracting(self)) {
            retraction -= line.dist_E(self);
        }
    });
    // The 1 mm cube has five 0.2 mm layers, each with one outer wall followed by an inner wall.
    REQUIRE(transitions == 5);
}

// EdgeSlicer keeps it off by default (owner, 2026-11-09); Orca #16982 turns it on.
TEST_CASE("Retract after outer wall is off by default", "[GCode][Retraction]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    CHECK_FALSE(config.opt_bool("retract_after_outer_wall"));
}

TEST_CASE("Retract after outer wall only invalidates G-code export", "[GCode][Retraction]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "retract_after_outer_wall", "0" },
        { "machine_start_gcode", "" },
        { "machine_end_gcode", "" },
    });
    Print print;
    Model model;
    init_print({ make_cube(10., 10., 1.) }, print, model, config);
    gcode(print);
    REQUIRE(print.objects().front()->is_step_done(posPerimeters));
    REQUIRE(print.is_step_done(psGCodeExport));

    DynamicPrintConfig changed = config;
    changed.set_deserialize_strict({{ "retract_after_outer_wall", "1" }});
    print.apply(model, changed);

    CHECK(print.objects().front()->is_step_done(posPerimeters));
    CHECK(print.objects().front()->is_step_done(posInfill));
    CHECK_FALSE(print.is_step_done(psGCodeExport));
}

// ---------------------------------------------------------------------------------------------------------------
// Z contouring (zaa_enabled) in the G-code: errors the Rust port turned up.

namespace {

// A 30 x 20 mm wedge whose top rises 4 degrees along +X, from 2 mm at x = 0 to 4.1 mm at x = 30.
TriangleMesh contour_wedge()
{
    const double len = 30., wid = 20., z0 = 2., z1 = z0 + len * std::tan(4. * M_PI / 180.);
    indexed_triangle_set its;
    auto V = [&its](double x, double y, double z) {
        its.vertices.emplace_back(stl_vertex(float(x), float(y), float(z)));
        return int(its.vertices.size()) - 1;
    };
    const int b00 = V(0, 0, 0), b10 = V(len, 0, 0), b11 = V(len, wid, 0), b01 = V(0, wid, 0);
    const int t00 = V(0, 0, z0), t10 = V(len, 0, z1), t11 = V(len, wid, z1), t01 = V(0, wid, z0);
    auto F = [&its](int a, int b, int c) { its.indices.emplace_back(stl_triangle_vertex_indices(a, b, c)); };
    F(b00, b11, b10); F(b00, b01, b11); // bottom
    F(t00, t10, t11); F(t00, t11, t01); // sloped top
    F(b00, b10, t10); F(b00, t10, t00); // y = 0
    F(b01, t01, t11); F(b01, t11, b11); // y = wid
    F(b00, t00, t01); F(b00, t01, b01); // x = 0
    F(b10, b11, t11); F(b10, t11, t10); // x = len
    return TriangleMesh(its);
}

std::string slice_contoured_wedge(std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"zaa_enabled", "1"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"wall_loops", "2"},
        {"enable_arc_fitting", "0"},
        {"use_relative_e_distances", "1"},
        {"skirt_loops", "0"},
        {"brim_type", "no_brim"},
        {"slow_down_for_layer_cooling", "0"},
    });
    config.set_deserialize_strict(extra);
    std::string gcode = Slic3r::Test::slice({contour_wedge()}, config);
    // With EDGE_TEST_DUMP_GCODE set to a directory, the G-code is written there for inspection.
    if (const char *dir = std::getenv("EDGE_TEST_DUMP_GCODE"); dir != nullptr && *dir != 0) {
        std::string name = "contour_wedge";
        for (const Slic3r::ConfigBase::SetDeserializeItem &item : extra)
            name += "_" + item.opt_key + "-" + item.opt_value;
        std::ofstream(std::string(dir) + "/" + name + ".gcode", std::ios::binary) << gcode;
    }
    return gcode;
}

// One extruding or wiping move of the G-code (a wipe may move no filament), with the feature it belongs to and the layer's nominal Z.
struct WedgeMove
{
    std::string type;
    double      layer_z, x0, y0, z0, x1, y1, z1, e, f;
    bool        wipe;
};

std::vector<WedgeMove> wedge_moves(const std::string &gcode)
{
    std::vector<WedgeMove> out;
    std::string            type;
    double                 layer_z = 0.;
    bool                   wipe    = false;
    GCodeReader            parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view raw = line.raw();
        if (raw.rfind(";TYPE:", 0) == 0)
            type = std::string(raw.substr(6));
        else if (raw.rfind(";Z:", 0) == 0)
            layer_z = std::atof(std::string(raw.substr(3)).c_str());
        else if (raw.rfind(";WIPE_START", 0) == 0)
            wipe = true;
        else if (raw.rfind(";WIPE_END", 0) == 0)
            wipe = false;
        if (line.cmd_is("G1") && (line.has_x() || line.has_y()) && (line.has_e() || wipe))
            out.push_back({type, layer_z, self.x(), self.y(), self.z(), line.new_X(self), line.new_Y(self), line.new_Z(self),
                           line.has_e() ? line.e() : 0., line.has_f() ? line.f() : self.f(), wipe});
    });
    return out;
}

bool is_wall(const std::string &type) { return type == "Outer wall" || type == "Inner wall" || type == "Overhang wall"; }

// A move laid off the layer's nominal Z, as only a contoured one is here.
bool contoured(const WedgeMove &m) { return std::abs(m.z1 - m.layer_z) > 0.001 || std::abs(m.z0 - m.layer_z) > 0.001; }

} // namespace

// A scarf seam on a contoured wall was laid twice. _extrude lays a contoured path by its contour before it looks for
// a slope, and the scarf's sloped paths kept the contour of the wall they were cut from, so both the scarf's way up
// and its way down were laid flat along the contour at the wall's full flow. A contoured wall now gets no scarf.
TEST_CASE("Z contouring: a contoured wall gets no scarf seam", "[GCode][ContourZ][Regression]")
{
    const std::vector<WedgeMove> scarf = wedge_moves(slice_contoured_wedge({{"seam_slope_type", "all"}}));
    const std::vector<WedgeMove> plain = wedge_moves(slice_contoured_wedge({{"seam_slope_type", "none"}}));

    // The layers with a contoured wall, found without the scarf (a scarf's way up is laid below the layer's Z too).
    std::set<double> layers;
    for (const WedgeMove &m : plain)
        if (is_wall(m.type) && !m.wipe && m.e > 0. && contoured(m))
            layers.insert(m.layer_z);
    // The walls of each of those layers: filament, and XY length.
    auto walls_of_contoured_layers = [&layers](const std::vector<WedgeMove> &moves) {
        std::map<double, std::pair<double, double>> out;
        for (const WedgeMove &m : moves)
            if (layers.count(m.layer_z) && is_wall(m.type) && !m.wipe && m.e > 0.) {
                out[m.layer_z].first += m.e;
                out[m.layer_z].second += std::hypot(m.x1 - m.x0, m.y1 - m.y0);
            }
        return out;
    };
    const auto with_scarf = walls_of_contoured_layers(scarf);
    const auto without    = walls_of_contoured_layers(plain);
    REQUIRE(with_scarf.size() >= 5);
    REQUIRE(with_scarf.size() == without.size());
    double extra_length = 0.;
    for (const auto &[z, scarfed] : with_scarf) {
        INFO("layer at " << z << ": walls " << scarfed.second << " mm with the scarf asked, " << without.at(z).second << " without");
        extra_length += scarfed.second - without.at(z).second;
        CHECK_THAT(scarfed.first, Catch::Matchers::WithinRel(without.at(z).first, 1e-4));
        CHECK_THAT(scarfed.second, Catch::Matchers::WithinRel(without.at(z).second, 1e-4));
    }
    INFO(with_scarf.size() << " layers with contoured walls, " << extra_length << " mm more wall with the scarf asked");
    CHECK(std::abs(extra_length) < 0.1);

    // The flat walls below the slope keep their scarf: its way up is laid below the layer's Z.
    std::set<double> scarfed_flat_layers;
    for (const WedgeMove &m : scarf)
        if (is_wall(m.type) && !m.wipe && m.e > 0. && !layers.count(m.layer_z) && m.layer_z > 0.3 && m.z1 < m.layer_z - 0.001)
            scarfed_flat_layers.insert(m.layer_z);
    INFO(scarfed_flat_layers.size() << " layers of flat walls with a scarf");
    CHECK(scarfed_flat_layers.size() >= 5);
}

// Ironing's speed was scaled for the local height as if it were a layer: the ratio of the contoured height to
// path.height, which for ironing is the ironing line's own few thousandths of a mm. Ironing's flow is not scaled for
// the height, so its speed is not either: it irons at its own speed.
TEST_CASE("Z contouring: ironing keeps its speed with constant flow speed scaling", "[GCode][ContourZ][Regression]")
{
    const std::vector<WedgeMove> moves =
        wedge_moves(slice_contoured_wedge({{"zaa_speed_scaling", "1"}, {"ironing_type", "top"}, {"ironing_speed", "30"}}));
    size_t ironing = 0, contoured_ironing = 0, off_speed = 0;
    double slowest = DBL_MAX;
    for (const WedgeMove &m : moves)
        if (m.type == "Ironing" && !m.wipe && m.e > 0.) {
            ++ironing;
            if (contoured(m))
                ++contoured_ironing;
            if (std::abs(m.f - 30. * 60.) > 0.5)
                ++off_speed;
            slowest = std::min(slowest, m.f / 60.);
        }
    INFO(ironing << " ironing moves, " << contoured_ironing << " contoured, " << off_speed << " not at 30 mm/s, slowest "
                 << slowest << " mm/s");
    REQUIRE(contoured_ironing > 20);
    CHECK(off_speed == 0);
}

// A wipe went back along a contoured line in X and Y alone, at the height the line ended at: where the line had been
// laid higher, the nozzle ploughed back through it. It now keeps to the line's heights.
TEST_CASE("Z contouring: a wipe over a contoured line keeps to the line's heights", "[GCode][ContourZ][Regression]")
{
    const std::vector<WedgeMove> moves = wedge_moves(slice_contoured_wedge({{"wipe", "1"}, {"wipe_distance", "2"}}));

    // The height of the line laid last before `wipe_start` under the point (x, y), or NaN if none passes there.
    auto line_height = [&moves](size_t wipe_start, double x, double y) {
        double best = std::numeric_limits<double>::quiet_NaN(), best_d = 0.02;
        for (size_t i = wipe_start; i > 0; --i) {
            const WedgeMove &m = moves[i - 1];
            if (m.wipe || m.e <= 0. || m.layer_z != moves[wipe_start].layer_z)
                break; // only the path the wipe follows, back to the last travel
            const Vec2d  a(m.x0, m.y0), b(m.x1, m.y1), p(x, y);
            const double len2 = (b - a).squaredNorm();
            const double t    = len2 > 0. ? std::clamp((p - a).dot(b - a) / len2, 0., 1.) : 0.;
            const double d    = (a + t * (b - a) - p).norm();
            if (d < best_d)
                best_d = d, best = m.z0 + t * (m.z1 - m.z0);
        }
        return best;
    };

    size_t wipes_over_contoured = 0, digging = 0;
    double deepest = 0.;
    for (size_t i = 0; i < moves.size(); ++i) {
        if (!moves[i].wipe || (i > 0 && moves[i - 1].wipe))
            continue;
        // The path before this wipe: was it contoured?
        if (i == 0 || !contoured(moves[i - 1]))
            continue;
        ++wipes_over_contoured;
        double depth = 0.;
        for (size_t j = i; j < moves.size() && moves[j].wipe; ++j) {
            const double z = line_height(i, moves[j].x1, moves[j].y1);
            if (!std::isnan(z))
                depth = std::max(depth, z - moves[j].z1);
        }
        if (depth > 0.001)
            ++digging;
        deepest = std::max(deepest, depth);
    }
    INFO(wipes_over_contoured << " wipes after a contoured line, " << digging << " of them below it, up to " << deepest << " mm");
    REQUIRE(wipes_over_contoured >= 5);
    CHECK(digging == 0);
}
