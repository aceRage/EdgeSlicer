#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/SandwichLite.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

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

// Strip generation timestamp / M73 / time estimates so two exports of the same job can be compared.
static std::string sandwich_lite_strip_volatile(const std::string &gcode)
{
    std::string out;
    out.reserve(gcode.size());
    size_t pos = 0;
    while (pos < gcode.size()) {
        const size_t eol  = gcode.find('\n', pos);
        const size_t end  = eol == std::string::npos ? gcode.size() : eol + 1;
        const std::string line = gcode.substr(pos, end - pos);
        const bool volatile_line =
            line.find("; generated by") != std::string::npos ||
            line.find("M73") != std::string::npos ||
            line.find("estimated") != std::string::npos ||
            line.find("total estimated time") != std::string::npos;
        if (!volatile_line)
            out += line;
        pos = end;
    }
    return out;
}

static DynamicPrintConfig sandwich_lite_test_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",                 "0.2" },
        { "initial_layer_print_height",   "0.2" },
        { "wall_loops",                   "1" },
        { "top_shell_layers",             "2" },
        { "bottom_shell_layers",          "2" },
        { "sparse_infill_density",        "0%" },
        { "enable_support",               "0" },
        { "brim_type",                    "no_brim" },
        { "skirt_loops",                  "0" },
        { "ironing_type",                 "no ironing" },
        { "machine_start_gcode",          "" },
        { "machine_end_gcode",            "" },
        { "gcode_comments",               "1" },
        { "enable_prime_tower",           "0" },
    });
    return config;
}

static void collect_role_paths(const ExtrusionEntity *ee, ExtrusionRole role, std::vector<const ExtrusionPath *> &out)
{
    if (ee == nullptr)
        return;
    if (auto *path = dynamic_cast<const ExtrusionPath *>(ee)) {
        if (path->role() == role)
            out.push_back(path);
        return;
    }
    if (auto *mp = dynamic_cast<const ExtrusionMultiPath *>(ee)) {
        for (const ExtrusionPath &p : mp->paths)
            if (p.role() == role)
                out.push_back(&p);
        return;
    }
    if (auto *loop = dynamic_cast<const ExtrusionLoop *>(ee)) {
        for (const ExtrusionPath &p : loop->paths)
            if (p.role() == role)
                out.push_back(&p);
        return;
    }
    if (auto *col = dynamic_cast<const ExtrusionEntityCollection *>(ee)) {
        for (const ExtrusionEntity *child : col->entities)
            collect_role_paths(child, role, out);
    }
}

TEST_CASE("Sandwich-lite planner: off, min height, pin last Z", "[GCode][SandwichLite]")
{
    DynamicPrintConfig dyn = DynamicPrintConfig::full_print_config();
    REQUIRE_FALSE(dyn.opt_bool("sandwich_lite_enable"));
    REQUIRE_FALSE(dyn.opt_bool("sandwich_lite_include_bottom"));
    CHECK(dyn.opt_int("sandwich_lite_pass_count") == 2);

    PrintRegionConfig cfg;
    cfg.apply(dyn, true);
    CHECK(sandwich_lite_plan(cfg, erTopSolidInfill, 0.2, 20.0).empty());

    cfg.sandwich_lite_enable.value = true;
    const auto two = sandwich_lite_plan(cfg, erTopSolidInfill, 0.2, 20.0);
    REQUIRE(two.size() == 2);
    CHECK_THAT(two[0].height, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK_THAT(two[1].height, Catch::Matchers::WithinAbs(0.1, 1e-9));
    CHECK_THAT(two[0].print_z, Catch::Matchers::WithinAbs(19.9, 1e-9));
    CHECK_THAT(two[1].print_z, Catch::Matchers::WithinAbs(20.0, 1e-9));
    CHECK_THAT((two[0].print_z - 20.0) / two[0].height, Catch::Matchers::WithinAbs(-1.0, 1e-9));
}

TEST_CASE("Sandwich-lite planner skips a layer thinner than two 0.04 mm passes", "[GCode][SandwichLite]")
{
    DynamicPrintConfig dyn = DynamicPrintConfig::full_print_config();
    dyn.set("sandwich_lite_enable", true);
    PrintRegionConfig cfg;
    cfg.apply(dyn, true);
    CHECK(sandwich_lite_plan(cfg, erTopSolidInfill, 0.06, 1.0).empty());
    cfg.sandwich_lite_pass_count.value = 3;
    const auto three = sandwich_lite_plan(cfg, erTopSolidInfill, 0.2, 4.0);
    REQUIRE(three.size() == 3);
    CHECK_THAT(three.back().print_z, Catch::Matchers::WithinAbs(4.0, 1e-9));
    double sum_h = 0.;
    for (const auto &p : three)
        sum_h += p.height;
    CHECK_THAT(sum_h, Catch::Matchers::WithinAbs(0.2, 1e-9));
}

TEST_CASE("Sandwich-lite stadium flow is not a linear height scale", "[GCode][SandwichLite]")
{
    const double w = 0.42;
    const double n = 0.4;
    const double a_full = sandwich_lite_stadium_mm3_per_mm(w, 0.2, n);
    const double a_half = sandwich_lite_stadium_mm3_per_mm(w, 0.1, n);
    REQUIRE(a_full > 0.);
    REQUIRE(a_half > 0.);
    CHECK(a_half * 2. > a_full);
    CHECK_THAT(a_half, Catch::Matchers::WithinAbs(Flow(0.42f, 0.1f, 0.4f).mm3_per_mm(), 1e-12));
}

TEST_CASE("Sandwich-lite does not insert an ExtrusionRole mid-enum", "[GCode][SandwichLite]")
{
    // Stream, the phone API and the preview number roles. Keep erTopSolidInfill at 7.
    CHECK(int(erTopSolidInfill) == 7);
    CHECK(int(erBottomSurface) == 8);
}

TEST_CASE("Sandwich-lite off is byte-identical to an explicit disable", "[GCode][SandwichLite]")
{
    DynamicPrintConfig off = sandwich_lite_test_config();
    DynamicPrintConfig explicit_off = off;
    explicit_off.set("sandwich_lite_enable", false);

    const std::string a = sandwich_lite_strip_volatile(slice({ TestMesh::cube_20x20x20 }, off));
    const std::string b = sandwich_lite_strip_volatile(slice({ TestMesh::cube_20x20x20 }, explicit_off));
    REQUIRE_FALSE(a.empty());
    CHECK(a == b);
    CHECK(a.find("; sandwich-lite fan") == std::string::npos);
}

TEST_CASE("Sandwich-lite sub-passes: Z, flow, preview heights, no toolchange", "[GCode][SandwichLite]")
{
    DynamicPrintConfig config = sandwich_lite_test_config();
    config.set("sandwich_lite_enable", true);
    config.set("sandwich_lite_pass_count", 2);
    config.option<ConfigOptionInts>("sandwich_lite_fan_speeds")->values = { 80, 40, -1 };
    config.option<ConfigOptionFloats>("sandwich_lite_speed_percents")->values = { 50., 100., 100. };

    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config, false);

    GCodeProcessorResult result;
    const std::string    gcode = Test::gcode(print, result);
    REQUIRE_FALSE(gcode.empty());
    CHECK(gcode.find("\nT1") == std::string::npos);
    CHECK(gcode.find("; sandwich-lite fan 80") != std::string::npos);
    // Preview / Stream read HEIGHT tags. Sub-passes are 0.1 mm inside a 0.2 mm layer.
    CHECK((gcode.find(";HEIGHT:0.1") != std::string::npos ||
           gcode.find("; LAYER_HEIGHT: 0.1") != std::string::npos));

    const PrintObject &object = *print.objects().front();
    const Layer       &top    = *object.layers().back();
    std::vector<const ExtrusionPath *> top_paths;
    for (const LayerRegion *region : top.regions())
        for (const ExtrusionEntity *ee : region->fills.entities)
            collect_role_paths(ee, erTopSolidInfill, top_paths);
    REQUIRE_FALSE(top_paths.empty());

    std::set<int> height_um;
    std::set<int> zoff_milli;
    for (const ExtrusionPath *p : top_paths) {
        height_um.insert(int(std::lround(double(p->height) * 1000.)));
        zoff_milli.insert(int(std::lround(double(p->z_offset) * 1000.)));
        CHECK_THAT(double(p->height), Catch::Matchers::WithinAbs(0.1, 0.02));
        const double expected_mm3 = sandwich_lite_stadium_mm3_per_mm(p->width, p->height, 0.4);
        CHECK_THAT(p->mm3_per_mm, Catch::Matchers::WithinRel(expected_mm3, 0.25));
    }
    CHECK(height_um.count(100) + height_um.count(99) + height_um.count(101) >= 1);
    // First pass sits below nominal Z (z_offset < 0); last pass is pinned (z_offset == 0).
    CHECK(*zoff_milli.begin() < 0);
    CHECK(zoff_milli.count(0) == 1);

    std::set<int> preview_h_um;
    std::set<int> preview_z_um;
    unsigned      top_layer_id = 0;
    for (const auto &m : result.moves)
        if (m.type == EMoveType::Extrude)
            top_layer_id = std::max(top_layer_id, m.layer_id);
    for (const auto &m : result.moves) {
        if (m.type != EMoveType::Extrude || m.extrusion_role != erTopSolidInfill || m.layer_id != top_layer_id)
            continue;
        preview_h_um.insert(int(std::lround(double(m.height) * 1000.)));
        preview_z_um.insert(int(std::lround(double(m.position.z()) * 1000.)));
    }
    REQUIRE_FALSE(preview_h_um.empty());
    CHECK(preview_h_um.count(100) + preview_h_um.count(99) + preview_h_um.count(101) >= 1);
    REQUIRE(preview_z_um.size() >= 2);
    // Sub-pass Z moves inside the same layer: ~0.1 mm between the two top-surface bands.
    CHECK(*preview_z_um.rbegin() - *preview_z_um.begin() >= 90);

    const std::string on  = sandwich_lite_strip_volatile(gcode);
    config.set("sandwich_lite_enable", false);
    const std::string off = sandwich_lite_strip_volatile(slice({ TestMesh::cube_20x20x20 }, config));
    CHECK(on != off);
}
