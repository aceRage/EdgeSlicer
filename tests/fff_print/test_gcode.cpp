#include <catch2/catch.hpp>

#include <cctype>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#include "libslic3r/GCode.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/PrintConfig.hpp"
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
