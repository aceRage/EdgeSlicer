#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifndef _WIN32
#ifdef __APPLE__
#include <xlocale.h>
#else
#include <locale.h>
#endif
#endif

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Orca #15133: on Klipper the wipe tower's planner flush is M400 and its dwell is G4 P<ms>.
// Klipper's G4 reads only P (milliseconds), so "G4 S0" neither synchronized nor waited there.

// Taken from the config enum map rather than hand-listed, so a flavor added to GCodeFlavor later
// is covered here without editing this file.
static std::vector<GCodeFlavor> non_klipper_flavors()
{
    std::vector<GCodeFlavor> flavors;
    for (const auto &kv : ConfigOptionEnum<GCodeFlavor>::get_enum_values())
        if (GCodeFlavor(kv.second) != gcfKlipper)
            flavors.push_back(GCodeFlavor(kv.second));
    return flavors;
}

static std::string flavor_name(GCodeFlavor flavor)
{
    return ConfigOptionEnum<GCodeFlavor>::get_enum_names()[int(flavor)];
}

TEST_CASE("Klipper flushes the wipe tower planner queue with M400", "[WipeTower][KlipperFlush]")
{
    CHECK(std::string(flush_planner_queue_command(gcfKlipper)) == "M400\n");
}

TEST_CASE("Other flavors flush the wipe tower planner queue with a zero dwell", "[WipeTower][KlipperFlush]")
{
    const GCodeFlavor flavor = GENERATE(from_range(non_klipper_flavors()));
    INFO("gcode flavor: " << flavor_name(flavor));
    CHECK(std::string(flush_planner_queue_command(flavor)) == "G4 S0\n");
}

// 1.5 s is exactly representable as a float, so neither form can drift when rounded.
TEST_CASE("Klipper waits in the wipe tower with a millisecond dwell", "[WipeTower][KlipperFlush]")
{
    CHECK(wait_command(gcfKlipper, 1.5f) == "G4 P1500\n");
}

TEST_CASE("Other flavors wait in the wipe tower with a seconds dwell", "[WipeTower][KlipperFlush]")
{
    const GCodeFlavor flavor = GENERATE(from_range(non_klipper_flavors()));
    INFO("gcode flavor: " << flavor_name(flavor));
    CHECK(wait_command(flavor, 1.5f) == "G4 S1.500\n");
}

// Orca #15146: the prime tower is validated against the real printable outline, so the placement clamps
// have to agree with it wherever that outline is not a rectangle. A regular hexagon inscribed in a 200mm
// circle stands in for the shipped delta beds.
TEST_CASE("The wipe tower placement clamp follows a non-rectangular bed outline", "[WipeTower][TowerPlacement]")
{
    const coord_t margin = scaled<coord_t>(1.);
    auto square_at = [](double x, double y, double side) {
        return BoundingBox(Point::new_scale(x, y), Point::new_scale(x + side, y + side));
    };
    // Does the footprint, padded by pad, sit inside the outline once the returned move is applied?
    auto lands_inside = [](BoundingBox box, const Polygons &bed, const Vec2f &move, coord_t pad) {
        box.translate(Point::new_scale(move.x(), move.y()));
        return diff(Polygons{box.inflated(pad).polygon()}, bed).empty();
    };

    const Polygons hex_bed{make_circle_num_segments(scaled<double>(100.), 6)};
    const Polygons square_bed{Polygon::new_scale(Pointfs{{0., 0.}, {200., 0.}, {200., 200.}, {0., 200.}})};

    SECTION("a rectangular bed is left to the bounding box clamp") {
        CHECK(WipeTower::outline_fills_bounding_box(square_bed));
        CHECK_FALSE(WipeTower::outline_fills_bounding_box(hex_bed));
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(50., 50., 30.), square_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    // Dragging the tower off one edge may not pull it away from the other, or it would jump out from
    // under the cursor instead of sliding along the edge.
    SECTION("only the violated axis is clamped") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(185., 50., 30.), square_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(-16., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    SECTION("a footprint already inside the outline is left alone") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(-15., -15., 30.), hex_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    SECTION("a footprint in the bounding box corner is pulled onto the bed") {
        const BoundingBox box = square_at(55., 50., 30.);
        REQUIRE_FALSE(lands_inside(box, hex_bed, Vec2f::Zero(), margin)); // in the bbox, off the hexagon
        CHECK(lands_inside(box, hex_bed, WipeTower::move_box_inside_polygon(box, hex_bed, margin), margin));
    }

    // An unresolved auto brim width reaches the drag clamp as a negative margin. Padding by it would
    // shrink the footprint and hand back a position the slice validation still rejects.
    SECTION("a negative margin still lands the footprint inside the outline") {
        const BoundingBox box  = square_at(55., 50., 30.);
        const coord_t     brim = scaled<coord_t>(-0.5);
        CHECK(lands_inside(box, hex_bed, WipeTower::move_box_inside_polygon(box, hex_bed, brim), 0));
    }

    SECTION("a footprint too large for the bed is left alone") {
        const Vec2f move = WipeTower::move_box_inside_polygon(square_at(-200., -200., 400.), hex_bed, margin);
        CHECK_THAT(move.x(), Catch::Matchers::WithinAbs(0., 1e-6));
        CHECK_THAT(move.y(), Catch::Matchers::WithinAbs(0., 1e-6));
    }

    // Edge: the FLSun S1 style bed, a 320 mm circle centred on the origin. A tower in its
    // bounding-box corner is pulled onto the disc.
    SECTION("a corner of a circular bed centred on the origin is pulled onto the disc") {
        const Polygons disc{make_circle_num_segments(scaled<double>(160.), 72)};
        const BoundingBox box = square_at(100., 100., 40.);
        REQUIRE_FALSE(lands_inside(box, disc, Vec2f::Zero(), margin));
        const Vec2f move = WipeTower::move_box_inside_polygon(box, disc, margin);
        CHECK(lands_inside(box, disc, move, margin));
        CHECK(move.x() < 0.f);
        CHECK(move.y() < 0.f);
    }
}

// Orca #15146: on a non-rectangular bed the plate's bounding box has corners the bed does not. A tower
// placed in one is reported before slicing by the last check of Print::validate(), from the body estimate
// alone; one on the bed passes. Checked directly and through validate().
static std::string validate_tower_on_round_bed(double tower_x, double tower_y, bool through_validate)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("purge_in_prime_tower")->value           = false;
    config.option<ConfigOptionInt>("outer_wall_filament")->value    = 2;
    config.option<ConfigOptionInt>("wall_filament")->value          = 1;
    config.option<ConfigOptionInt>("sparse_infill_filament")->value = 1;
    config.option<ConfigOptionInt>("solid_infill_filament")->value  = 1;
    config.option<ConfigOptionFloat>("prime_tower_width")->value    = 35.;
    // Relative E needs a G92 E0 per layer or validate() stops on that first.
    config.option<ConfigOptionString>("layer_change_gcode")->value  = "G92 E0";
    config.option<ConfigOptionFloats>("wipe_tower_x")->values       = {tower_x};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values       = {tower_y};
    // A 200 mm disc centred on the origin, as the delta profiles define their beds; the test cube is
    // arranged around the origin too.
    Pointfs disc;
    for (int i = 0; i < 72; ++i) {
        const double a = 2. * PI * double(i) / 72.;
        disc.emplace_back(100. * std::cos(a), 100. * std::sin(a));
    }
    config.option<ConfigOptionPoints>("printable_area")->values = disc;

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    // Re-applied as the GUI does on every change, so the tower is planned with the objects known.
    print.apply(model, config);
    REQUIRE(print.has_wipe_tower());
    return through_validate ? print.validate().string : print.prime_tower_off_bed_error();
}

TEST_CASE("A prime tower in a corner a round bed does not have fails validation", "[WipeTower][TowerPlacement]")
{
    // Body from (60, 60): its far corner is about 134 mm from the centre of a 100 mm radius bed.
    const bool through_validate = GENERATE(false, true);
    INFO("through validate(): " << through_validate);
    CHECK_THAT(validate_tower_on_round_bed(60., 60., through_validate), Catch::Matchers::Contains("partially outside the printable area"));
    // Body from (-55, -60), clear of the cube at the centre and well inside the disc.
    CHECK_THAT(validate_tower_on_round_bed(-55., -60., through_validate), !Catch::Matchers::Contains("partially outside the printable area"));
}

// The G-code inside each WIPE_TOWER_START / WIPE_TOWER_END pair, concatenated, so an M400 emitted
// outside the tower (start / end G-code, the pre-heat injector) cannot create a false match.
static std::string wipe_tower_regions(const std::string &gcode)
{
    const std::string &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_Start);
    const std::string &end_tag   = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Tower_End);
    std::string regions;
    size_t      pos = 0;
    while (true) {
        const size_t start = gcode.find(start_tag, pos);
        if (start == std::string::npos)
            break;
        const size_t end = gcode.find(end_tag, start);
        if (end == std::string::npos)
            break;
        regions.append(gcode, start, end - start);
        pos = end + 1;
    }
    return regions;
}

// A WipeTower2 (non-Bambu) two-filament print: outer wall in filament 2, the rest in filament 1,
// so every layer runs the toolchange path and its flush_planner_queue(). Same shape as
// test_print.cpp's "prepare and slice agree on the plate filaments" case (one apply keeps the tower).
static std::string slice_two_filament_tower(const std::string &gcode_flavor, bool snapmaker_u1)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionBool>("spiral_mode")->value          = false;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("purge_in_prime_tower")->value           = false;
    config.option<ConfigOptionInt>("outer_wall_filament")->value    = 2;
    config.option<ConfigOptionInt>("wall_filament")->value          = 1;
    config.option<ConfigOptionInt>("sparse_infill_filament")->value = 1;
    config.option<ConfigOptionInt>("solid_infill_filament")->value  = 1;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values       = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values       = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value    = 35.;
    config.set_deserialize_strict({ { "gcode_flavor", gcode_flavor } });
    if (snapmaker_u1)
        config.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(print.has_wipe_tower());
    return Test::gcode(print);
}

TEST_CASE("The wipe tower's toolchange planner flush follows the gcode flavor", "[WipeTower][KlipperFlush]")
{
    SECTION("klipper") {
        const std::string tower = wipe_tower_regions(slice_two_filament_tower("klipper", false));
        REQUIRE_FALSE(tower.empty());
        CHECK_THAT(tower, Catch::Matchers::Contains("M400"));
        CHECK_THAT(tower, !Catch::Matchers::Contains("G4 S"));
    }
    SECTION("marlin") {
        const std::string tower = wipe_tower_regions(slice_two_filament_tower("marlin", false));
        REQUIRE_FALSE(tower.empty());
        CHECK_THAT(tower, Catch::Matchers::Contains("G4 S0"));
        CHECK_THAT(tower, !Catch::Matchers::Contains("M400"));
    }
}

// EDGE: the U1 is a Klipper printer whose tower takes Edge's U1-only writer paths (is_snapmaker_u1:
// M220 B/R, SET_PRESSURE_ADVANCE, stagger). Its flush must be M400 too, and no seconds dwell may remain.
TEST_CASE("The Snapmaker U1 tower flushes with M400 and never a seconds dwell", "[WipeTower][KlipperFlush]")
{
    const std::string tower = wipe_tower_regions(slice_two_filament_tower("klipper", true));
    REQUIRE_FALSE(tower.empty());
    CHECK_THAT(tower, Catch::Matchers::Contains("CP TOOLCHANGE"));
    CHECK_THAT(tower, Catch::Matchers::Contains("M400"));
    CHECK_THAT(tower, !Catch::Matchers::Contains("G4 S"));
}

// ---------------------------------------------------------------------------------------------------
// Orca #15144: wait for the toolchange temperature on the wipe tower (wait_for_temp_on_wipe_tower).
// Ported from upstream's test_multifilament.cpp cases onto this file's two-filament WipeTower2 slice
// (Edge has no slice_with_object_overrides / multifilament_config harness): one cube, outer wall in
// filament 2 and the rest in filament 1, so every layer has a tool change on the tower.

static const char *const WAIT_TAG = "_WAIT_FOR_TEMP_ON_WIPE_TOWER";

static DynamicPrintConfig wait_for_temp_config(bool wait_on_tower)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00"};
    config.option<ConfigOptionBool>("enable_prime_tower")->value   = true;
    config.option<ConfigOptionBool>("enable_support")->value       = false;
    config.option<ConfigOptionBool>("spiral_mode")->value          = false;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("purge_in_prime_tower")->value           = false;
    config.option<ConfigOptionInt>("outer_wall_filament")->value    = 2;
    config.option<ConfigOptionInt>("wall_filament")->value          = 1;
    config.option<ConfigOptionInt>("sparse_infill_filament")->value = 1;
    config.option<ConfigOptionInt>("solid_infill_filament")->value  = 1;
    config.option<ConfigOptionFloats>("wipe_tower_x")->values       = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values       = {15.};
    config.option<ConfigOptionFloat>("prime_tower_width")->value    = 35.;
    config.set_deserialize_strict({
        // set_extruder's standby / restore pair (OozePrevention), which the option demotes and moves.
        { "ooze_prevention", "1" },
        { "standby_temperature_delta", "-40" },
        // The post-processor's own preheat pass also inserts an M104 for the incoming filament ahead
        // of the Tn; switch it off so the temperature commands under test are the only ones.
        { "preheat_time", "0" },
        { "wait_for_temp_on_wipe_tower", wait_on_tower ? "1" : "0" },
    });
    return config;
}

static std::string slice_wait_for_temp(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(print.has_wipe_tower());
    return Test::gcode(print);
}

static std::vector<std::string> split_lines(const std::string &gcode)
{
    std::vector<std::string> lines;
    std::istringstream       stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.emplace_back(std::move(line));
    }
    return lines;
}

static bool is_tool_line(const std::string &l) { return l.size() >= 2 && l[0] == 'T' && std::isdigit((unsigned char) l[1]); }
static bool is_tagged_wait(const std::string &l) { return l.find(WAIT_TAG) != std::string::npos; }
// Temperature of an M104/M109, or -1 when the line is neither.
static int temp_of(const std::string &l)
{
    if (l.rfind("M104", 0) != 0 && l.rfind("M109", 0) != 0)
        return -1;
    const size_t s = l.find(" S");
    return s == std::string::npos ? -1 : std::stoi(l.substr(s + 2));
}

// X where the nozzle sits while each tagged wait blocks: the nearest preceding G1 carrying an X
// (the park travel emitted just before the wait).
static std::vector<double> wait_park_xs(const std::string &gcode)
{
    const std::vector<std::string> lines = split_lines(gcode);
    std::vector<double>            xs;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("M109", 0) != 0 || !is_tagged_wait(lines[i]))
            continue;
        for (size_t j = i; j-- > 0;) {
            if (lines[j].rfind("G1 ", 0) != 0)
                continue;
            const size_t x_pos = lines[j].find('X');
            if (x_pos == std::string::npos)
                continue;
            xs.push_back(std::stod(lines[j].substr(x_pos + 1)));
            break;
        }
    }
    return xs;
}

// Upstream: "Toolchange temperature wait moves to the wipe tower when enabled". With the option the
// blocking M109 moves from right after the Tn to a stop beside the tower, tagged, after the tool change
// and before the first extrusion of the purge; the restore that used to block there becomes a
// non-blocking M104 ahead of the Tn, so the incoming tool heats up over the change itself.
TEST_CASE("Toolchange temperature wait moves to the wipe tower when enabled", "[WipeTower][WaitForTemp]")
{
    const bool wait_on_tower = GENERATE(false, true);
    DYNAMIC_SECTION("wait_for_temp_on_wipe_tower " << (wait_on_tower ? 1 : 0)) {
        const std::string              gcode = slice_wait_for_temp(wait_for_temp_config(wait_on_tower));
        const std::vector<std::string> lines = split_lines(gcode);

        const auto is_m109_line     = [](const std::string &l) { return l.rfind("M109", 0) == 0; };
        // A non-blocking set-temperature naming one specific tool, e.g. "M104 S255 T1".
        const auto is_m104_for_tool = [](const std::string &l, int tool) {
            if (l.rfind("M104", 0) != 0)
                return false;
            const std::string token = " T" + std::to_string(tool);
            const size_t      at    = l.find(token);
            return at != std::string::npos && (at + token.size() >= l.size() || !std::isdigit((unsigned char) l[at + token.size()]));
        };
        const auto is_extruding = [](const std::string &l) {
            if (l.rfind("G1 ", 0) != 0)
                return false;
            const size_t e = l.find(" E");
            return e != std::string::npos && l.find_first_of("XY") != std::string::npos && e + 2 < l.size() && l[e + 2] != '-';
        };

        int checked_blocks = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("; CP TOOLCHANGE START") == std::string::npos)
                continue;
            size_t block_end = i;
            while (block_end < lines.size() && lines[block_end].find("; CP TOOLCHANGE END") == std::string::npos)
                ++block_end;
            size_t tool_line = block_end;
            for (size_t j = i; j < block_end; ++j)
                if (is_tool_line(lines[j])) {
                    tool_line = j;
                    break;
                }
            if (tool_line == block_end) {
                i = block_end;
                continue; // a block without a tool change
            }
            ++checked_blocks;

            // Where the incoming tool's target temperature is raised, relative to its Tn.
            const int new_tool = std::stoi(lines[tool_line].substr(1));
            size_t    preheat = tool_line, restore = block_end;
            for (size_t j = i; j < tool_line; ++j)
                if (is_m104_for_tool(lines[j], new_tool)) {
                    preheat = j;
                    break;
                }
            for (size_t j = tool_line + 1; j < block_end; ++j)
                if (is_m104_for_tool(lines[j], new_tool)) {
                    restore = j;
                    break;
                }

            size_t tagged_wait = block_end, untagged_m109 = block_end, first_extrusion = block_end;
            for (size_t j = tool_line + 1; j < block_end; ++j) {
                if (is_m109_line(lines[j]) && tagged_wait == block_end && is_tagged_wait(lines[j]))
                    tagged_wait = j;
                if (is_m109_line(lines[j]) && untagged_m109 == block_end && !is_tagged_wait(lines[j]))
                    untagged_m109 = j;
                if (first_extrusion == block_end && is_extruding(lines[j]))
                    first_extrusion = j;
            }
            INFO("toolchange block at line " << i + 1);
            if (wait_on_tower) {
                // The only blocking wait is the tagged one, parked beside the tower before the purge.
                REQUIRE(tagged_wait < block_end);
                CHECK(untagged_m109 == block_end);
                // The target is raised ahead of the tool change and nothing sets it again afterwards.
                CHECK(preheat < tool_line);
                CHECK(restore == block_end);
                REQUIRE(first_extrusion < block_end);
                CHECK(tagged_wait < first_extrusion);
                // The pre-heat names the temperature the tower waits on.
                CHECK(temp_of(lines[preheat]) == temp_of(lines[tagged_wait]));
                // The travel preceding the wait parks outside the purge area of this block.
                size_t stop_line = block_end;
                for (size_t j = tagged_wait; j-- > tool_line;)
                    if (lines[j].rfind("G1 ", 0) == 0 && lines[j].find('X') != std::string::npos) {
                        stop_line = j;
                        break;
                    }
                REQUIRE(stop_line < block_end);
                const double stop_x = std::stod(lines[stop_line].substr(lines[stop_line].find('X') + 1));
                double purge_min_x = std::numeric_limits<double>::max(), purge_max_x = std::numeric_limits<double>::lowest();
                for (size_t j = tagged_wait; j < block_end; ++j) {
                    const size_t x_pos = lines[j].find('X');
                    if (!is_extruding(lines[j]) || x_pos == std::string::npos)
                        continue;
                    const double x = std::stod(lines[j].substr(x_pos + 1));
                    purge_min_x    = std::min(purge_min_x, x);
                    purge_max_x    = std::max(purge_max_x, x);
                }
                REQUIRE(purge_min_x <= purge_max_x);
                INFO("stop travel: " << lines[stop_line] << " purge x range: " << purge_min_x << ".." << purge_max_x);
                const bool beside_tower = stop_x < purge_min_x - 0.5 || stop_x > purge_max_x + 0.5;
                CHECK(beside_tower);
            } else {
                // Stock behaviour: the blocking wait follows the tool change command directly, and
                // nothing raises the incoming tool's target before it.
                REQUIRE(untagged_m109 < block_end);
                CHECK(tagged_wait == block_end);
                CHECK(preheat == tool_line);
                if (first_extrusion < block_end)
                    CHECK(untagged_m109 < first_extrusion);
            }
            i = block_end;
        }
        REQUIRE(checked_blocks > 0);
        if (!wait_on_tower)
            CHECK(gcode.find(WAIT_TAG) == std::string::npos);
    }
}

// Upstream: "Wipe tower priming pre-heats to the first layer temperature". Priming runs before the first
// layer is set up, so set_extruder sees no layer: on_first_layer() is false and print_z is the initial
// layer height rather than 0. The tower blocks on the first layer temperature there, so the pre-heat
// raised ahead of each priming Tn has to name that same temperature.
TEST_CASE("Wipe tower priming pre-heats to the first layer temperature", "[WipeTower][WaitForTemp]")
{
    DynamicPrintConfig config = wait_for_temp_config(true);
    config.set_deserialize_strict({
        { "single_extruder_multi_material_priming", "1" },
        // Distinct enough that picking the wrong one is unambiguous.
        { "nozzle_temperature_initial_layer", "215,215" },
        { "nozzle_temperature", "240,240" },
    });
    const std::vector<std::string> lines = split_lines(slice_wait_for_temp(config));

    size_t start = lines.size(), end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (start == lines.size() && lines[i].find("; CP PRIMING START") != std::string::npos)
            start = i;
        else if (start < lines.size() && lines[i].find("; CP PRIMING END") != std::string::npos) {
            end = i;
            break;
        }
    }
    REQUIRE(start < end);

    int checked_waits = 0;
    for (size_t i = start; i < end; ++i) {
        if (!is_tagged_wait(lines[i]))
            continue;
        ++checked_waits;
        INFO("priming wait at line " << i + 1 << ": " << lines[i]);
        CHECK(temp_of(lines[i]) == 215); // the tower waits on the first layer temperature
        // The most recent set-temperature before it is the pre-heat, and must agree with it.
        int preheat = -1;
        for (size_t j = i; j-- > start;)
            if ((preheat = temp_of(lines[j])) != -1)
                break;
        CHECK(preheat == 215);
    }
    REQUIRE(checked_waits > 0); // the feature under test is active
}

// Upstream: "Wipe tower temperature-wait park is regenerated when the tower moves". The park picks its
// side of the tower from the tower position at psWipeTower time, while the export shifts the cached
// moves by the current position. Moving the tower normally invalidates only psSkirtBrim, but the park
// makes the tower G-code bed-relative, so a GUI-style move and re-slice on the same Print must
// regenerate the tower. Contract: every tagged wait parks inside the printable area.
TEST_CASE("Wipe tower temperature-wait park is regenerated when the tower moves", "[WipeTower][WaitForTemp]")
{
    // Rectangle wall and no brim keep the tower-local footprint inside [0, 35], so the park sits at the
    // generator's 2 mm side gap. A 200 mm bed centred on the origin, where the test cube is arranged.
    DynamicPrintConfig config = wait_for_temp_config(true);
    config.set_deserialize_strict({
        { "wipe_tower_wall_type", "rectangle" },
        { "prime_tower_brim_width", "0" },
        { "printable_area", "-100x-100,100x-100,100x100,-100x100" },
        { "wipe_tower_x", "-100" },
        { "wipe_tower_y", "30" },
    });
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(print.has_wipe_tower());

    const std::vector<double> at_edge_parks = wait_park_xs(Test::gcode(print));
    REQUIRE(!at_edge_parks.empty()); // the feature under test is active
    for (double x : at_edge_parks) {
        INFO("wait park X " << x << " with the tower at x=-100 on a -100..100 bed");
        CHECK(x >= -100.05);
        CHECK(x <= 100.05);
    }
    REQUIRE(print.is_step_done(psWipeTower));

    // Move the tower to the right bed edge (64 + 35 = 99 keeps the body printable) and re-apply on the
    // SAME Print, assembled the way init_print assembled the first config so wipe_tower_x is the only
    // difference. The cached right-side park would export at 64 + 37 = 101, off the bed; regeneration
    // pulls it back against the bed edge.
    config.set_deserialize_strict({ { "wipe_tower_x", "64" } });
    DynamicPrintConfig moved_config = DynamicPrintConfig::full_print_config();
    moved_config.apply(config);
    print.apply(model, moved_config);
    CHECK_FALSE(print.is_step_done(psWipeTower)); // the move must regenerate the tower

    const std::vector<double> moved_parks = wait_park_xs(Test::gcode(print));
    REQUIRE(!moved_parks.empty()); // the waits must survive the re-slice
    for (double x : moved_parks) {
        INFO("wait park X " << x << " with the tower at x=64 on a -100..100 bed");
        CHECK(x >= -100.05);
        CHECK(x <= 100.05);
    }
}

// EDGE: the tower interface (#119) heats a filament to its interface temperature in interface_before_wipe.
// With the option the tool change already waited for that temperature beside the tower, so the tower
// waits once, tagged, at the interface temperature, and interface_before_wipe adds no second M109.
TEST_CASE("Wipe tower temperature wait covers the tower interface temperature", "[WipeTower][WaitForTemp]")
{
    const bool wait_on_tower = GENERATE(false, true);
    DynamicPrintConfig config = wait_for_temp_config(wait_on_tower);
    config.set_deserialize_strict({
        { "wipe_tower_interface_temp", "1" },
        { "wipe_tower_interface_trigger", "every_toolchange" },
        { "filament_tower_interface_print_temp", "250,250" },
        { "nozzle_temperature_initial_layer", "220,220" },
        { "nozzle_temperature", "220,220" },
    });
    const std::vector<std::string> lines = split_lines(slice_wait_for_temp(config));

    int interface_blocks = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find("; CP TOOLCHANGE START") == std::string::npos)
            continue;
        size_t block_end = i;
        while (block_end < lines.size() && lines[block_end].find("; CP TOOLCHANGE END") == std::string::npos)
            ++block_end;
        bool interface = false;
        int  waits_at_250 = 0, tagged_waits = 0;
        for (size_t j = i; j < block_end; ++j) {
            interface |= lines[j].find("; tower interface") != std::string::npos;
            if (lines[j].rfind("M109", 0) == 0 && temp_of(lines[j]) == 250)
                ++waits_at_250;
            if (lines[j].rfind("M109", 0) == 0 && is_tagged_wait(lines[j]))
                ++tagged_waits;
        }
        if (interface) {
            ++interface_blocks;
            INFO("interface toolchange block at line " << i + 1 << ", wait_for_temp_on_wipe_tower " << wait_on_tower);
            CHECK(waits_at_250 == 1);
            CHECK(tagged_waits == (wait_on_tower ? 1 : 0));
        }
        i = block_end;
    }
    REQUIRE(interface_blocks > 0);
}

// EDGE: the tower writes its wait for the G-code flavor, like the interface temperatures: RepRapFirmware
// sets the temperature with G10 and waits with M116 (upstream always writes M109).
TEST_CASE("Wipe tower temperature wait follows the gcode flavor", "[WipeTower][WaitForTemp]")
{
    DynamicPrintConfig config = wait_for_temp_config(true);
    config.set_deserialize_strict({ { "gcode_flavor", "reprapfirmware" } });
    const std::vector<std::string> lines = split_lines(slice_wait_for_temp(config));
    int tagged = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!is_tagged_wait(lines[i]))
            continue;
        ++tagged;
        INFO("line " << i + 1 << ": " << lines[i]);
        CHECK(lines[i].rfind("G10 S", 0) == 0);
        REQUIRE(i + 1 < lines.size());
        CHECK(lines[i + 1].rfind("M116", 0) == 0);
    }
    REQUIRE(tagged > 0);
}

// The option is off for single extruder multi-material whatever the profile says: SEMM runs its own
// unload / load temperature sequence on the tower.
TEST_CASE("Wipe tower temperature wait is off for single extruder multi-material", "[WipeTower][WaitForTemp]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    PrintConfig        print_config;
    print_config.apply(config, true);
    print_config.wait_for_temp_on_wipe_tower.value = true;
    print_config.single_extruder_multi_material.value = false;
    CHECK(WipeTower2::wait_for_temp_enabled(print_config));
    print_config.single_extruder_multi_material.value = true;
    CHECK_FALSE(WipeTower2::wait_for_temp_enabled(print_config));
    print_config.wait_for_temp_on_wipe_tower.value    = false;
    print_config.single_extruder_multi_material.value = false;
    CHECK_FALSE(WipeTower2::wait_for_temp_enabled(print_config));
}

// Orca #15035 + #15144 (8-3 review): the rib offset shifts the whole rib tower, so the heat-up park
// beside it has to be shifted the same way, or the park on the far side lands on the ribs. The park
// envelope is measured on generate_rib_polygon() and mapped through the writer, which adds the offset;
// this checks the result against the extrusions the tower actually prints on the same layer.
TEST_CASE("The temperature-wait park clears the shifted rib tower", "[WipeTower][WaitForTemp][RibOffset]")
{
    // Tower in the middle of the default 200 mm bed, so both sides are free and the near side is taken.
    const char *tower_x = GENERATE("15", "80", "150");
    CAPTURE(tower_x);
    DynamicPrintConfig config = wait_for_temp_config(true);
    config.set_deserialize_strict({
        { "wipe_tower_wall_type", "rib" },
        { "wipe_tower_rib_width", "8" },
        { "wipe_tower_fillet_wall", "1" },
        { "prime_tower_brim_width", "0" },
        { "wipe_tower_x", tower_x },
        { "wipe_tower_y", "120" },
    });
    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(print.has_wipe_tower());
    Test::gcode(print);

    const auto &layers = print.wipe_tower_data().tool_changes;
    REQUIRE(!layers.empty());
    int checked_parks = 0;
    for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx) {
        // The tower's printed envelope on this layer, tower-local like the tcr G-code.
        BoundingBoxf envelope;
        for (const WipeTower::ToolChangeResult &tcr : layers[layer_idx])
            for (const WipeTower::Extrusion &e : tcr.extrusions)
                if (e.width > 0.f)
                    envelope.merge(e.pos.cast<double>());
        if (!envelope.defined)
            continue;
        if (layer_idx == 0) {
            // The ribs start at the tower position: the wall's first-layer box begins at 0/0.
            CHECK_THAT(envelope.min.x(), Catch::Matchers::WithinAbs(0., 0.05));
            CHECK_THAT(envelope.min.y(), Catch::Matchers::WithinAbs(0., 0.05));
        }
        for (const WipeTower::ToolChangeResult &tcr : layers[layer_idx]) {
            const std::vector<std::string> lines = split_lines(tcr.gcode);
            for (size_t i = 0; i < lines.size(); ++i) {
                if (lines[i].rfind("M109", 0) != 0 || !is_tagged_wait(lines[i]))
                    continue;
                // The park travel is the never-skip G1 right before the wait.
                size_t park = i;
                while (park > 0 && lines[park - 1].find(WipeTower2::never_skip_tag()) == std::string::npos)
                    --park;
                REQUIRE(park > 0);
                const std::string &move = lines[park - 1];
                REQUIRE(move.find('X') != std::string::npos);
                const double park_x = std::stod(move.substr(move.find('X') + 1));
                INFO("layer " << layer_idx << " park " << move << " envelope x " << envelope.min.x() << ".." << envelope.max.x());
                CHECK((park_x < envelope.min.x() - 0.4 || park_x > envelope.max.x() + 0.4));
                ++checked_parks;
            }
        }
    }
    REQUIRE(checked_parks > 0); // the feature under test is active
}

// Orca #15035 (9292db2f9f, 36bd453ac8): on a multi-tool printer that does not ram (the U1), every purge
// block is planned as whole wipe rows that continue the previous block's row lattice, and the wipe fills
// its box. Before, the planner reserved one wipe line too few (it counted on a ramming row that is never
// printed) and the wipe started a pitch inside the box, so adjacent blocks were two row pitches apart
// (a blank band in the tower) and the last block of a layer was cut short at the box edge.
static std::vector<double> wipe_rows_of(const std::string &tcr_gcode)
{
    std::vector<double> ys;
    bool   in_wipe = false;
    double x = 0., y = 0.;
    for (const std::string &line : split_lines(tcr_gcode)) {
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos) { in_wipe = true; continue; }
        if (line.find("; CP TOOLCHANGE END") != std::string::npos) { in_wipe = false; continue; }
        if (line.rfind("G1 ", 0) != 0)
            continue;
        double nx = x, ny = y;
        bool   extrudes = false;
        std::istringstream words(line.substr(3, line.find(';') == std::string::npos ? std::string::npos : line.find(';') - 3));
        for (std::string w; words >> w;) {
            if (w.size() < 2) continue;
            const double v = std::atof(w.c_str() + 1);
            if (w[0] == 'X') nx = v;
            else if (w[0] == 'Y') ny = v;
            else if (w[0] == 'E' && v > 0.) extrudes = true;
        }
        if (in_wipe && extrudes && std::abs(ny - y) < 1e-6 && std::abs(nx - x) > 1e-6)
            ys.push_back(std::round(ny * 1000.) / 1000.);
        x = nx;
        y = ny;
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    return ys;
}

TEST_CASE("Purge blocks of a non-ramming toolchanger tile the tower without blank bands", "[WipeTower][WholeRowWipe]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF"};
    config.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "enable_support", "0" },
        { "single_extruder_multi_material", "0" },
        { "purge_in_prime_tower", "0" },
        { "filament_multitool_ramming", "0,0,0" },
        { "wipe_tower_wall_type", "rectangle" },
        { "wipe_tower_stagger_toolchange_start", "0" },
        { "prime_tower_width", "35" },
        { "prime_tower_brim_width", "0" },
        { "wipe_tower_x", "15" },
        { "wipe_tower_y", "120" },
        { "wall_filament", "1" },
        { "outer_wall_filament", "2" },
        { "sparse_infill_filament", "3" },
        { "solid_infill_filament", "3" },
        { "gcode_comments", "1" },
    });
    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);
    REQUIRE(print.has_wipe_tower());
    Test::gcode(print);

    int checked_joints = 0;
    for (const std::vector<WipeTower::ToolChangeResult> &layer : print.wipe_tower_data().tool_changes) {
        std::vector<std::vector<double>> blocks;
        for (const WipeTower::ToolChangeResult &tcr : layer)
            if (std::vector<double> rows = wipe_rows_of(tcr.gcode); rows.size() >= 2)
                blocks.emplace_back(std::move(rows));
        if (blocks.size() < 2)
            continue;
        const double dy = blocks.front()[1] - blocks.front()[0];
        REQUIRE(dy > 0.1);
        std::sort(blocks.begin(), blocks.end(), [](const auto &a, const auto &b) { return a.front() < b.front(); });
        for (size_t i = 0; i < blocks.size(); ++i) {
            for (size_t k = 1; k < blocks[i].size(); ++k)
                CHECK(blocks[i][k] - blocks[i][k - 1] == Approx(dy).margin(0.011));
            if (i + 1 < blocks.size()) {
                INFO("block " << i << " ends at " << blocks[i].back() << ", the next starts at " << blocks[i + 1].front() << ", pitch " << dy);
                CHECK(blocks[i + 1].front() - blocks[i].back() == Approx(dy).margin(0.011));
                ++checked_joints;
            }
        }
    }
    REQUIRE(checked_joints > 0);
}

// Orca #15035 (56810c8c7f, 5b475e5e98): with wall gaps (skip points) on, the travel to a toolchange's
// tower entry goes around the tower and enters through the wall opening instead of crossing the printed
// tower. On a toolchanger without ramming (the U1) the tool changes in place and the entry travel is
// appended to the change_filament_gcode substitution. The tower turns 180 degrees every layer, so every
// other layer's entry is on the far side of the tower from the part: a direct hop there crosses it.
TEST_CASE("The tower entry travel goes around the tower when wall gaps are on", "[WipeTower][SkipPoints]")
{
    const bool gaps  = GENERATE(false, true);
    // #700: a rotated tower's entry walls are not vertical; the routed hop still has to cross the wall.
    const int  angle = GENERATE(0, 90);
    DYNAMIC_SECTION("wipe_tower_wall_gap " << gaps << " rotation " << angle) {
        DynamicPrintConfig config = wait_for_temp_config(false);
        config.set_deserialize_strict({
            { "wipe_tower_wall_type", "rectangle" },
            { "wipe_tower_wall_gap", gaps ? "1" : "0" },
            { "wipe_tower_rotation_angle", angle },
            { "prime_tower_brim_width", "0" },
            { "filament_multitool_ramming", "0,0" },
            { "wipe_tower_x", "120" },
            { "wipe_tower_y", "60" },
        });
        Print print;
        Model model;
        init_print({TestMesh::cube_20x20x20}, print, model, config);
        REQUIRE(print.has_wipe_tower());
        const std::string gcode = Test::gcode(print);

        REQUIRE(print.wipe_tower_data().wipe_tower_mesh_data.has_value());
        Polygon envelope = print.wipe_tower_data().wipe_tower_mesh_data->bottom;
        envelope.rotate(double(angle) * M_PI / 180.);
        envelope.translate(Point::new_scale(120., 60.));
        const Eigen::Rotation2Dd to_local(-double(angle) * M_PI / 180.);
        const Polygons inner = offset(envelope, -scale_(1.));
        const Polygons outer = offset(envelope, scale_(1.));
        REQUIRE(!outer.empty());
        REQUIRE(!inner.empty());

        // Travels from outside the tower to a point on it (the entries), and how far each runs over the
        // tower's inside.
        double x = 0., y = 0.;
        int    entries = 0, crossing = 0;
        const std::vector<std::string> all_lines = split_lines(gcode);
        for (size_t line_idx = 0; line_idx < all_lines.size(); ++line_idx) {
            const std::string &line = all_lines[line_idx];
            if (line.rfind("G1 ", 0) != 0)
                continue;
            double nx = x, ny = y;
            bool   extrudes = false;
            std::istringstream words(line.substr(3, line.find(';') == std::string::npos ? std::string::npos : line.find(';') - 3));
            for (std::string w; words >> w;) {
                if (w.size() < 2)
                    continue;
                if (w[0] == 'X' || w[0] == 'Y')
                    (w[0] == 'X' ? nx : ny) = std::atof(w.c_str() + 1);
                else if (w[0] == 'E' && std::atof(w.c_str() + 1) > 0.)
                    extrudes = true;
            }
            const Point from = Point::new_scale(x, y), to = Point::new_scale(nx, ny);
            if (!extrudes && from != to && !envelope.contains(from) && envelope.contains(to)) {
                ++entries;
                double over = 0.;
                for (const Polyline &piece : intersection_pl(Polylines{ Polyline{ from, to } }, inner))
                    over += unscale<double>(piece.length());
                std::string context;
                for (size_t k = line_idx > 40 ? line_idx - 40 : 0; k <= line_idx; ++k)
                    context += all_lines[k] + "\n";
                INFO("entry travel " << x << "," << y << " -> " << nx << "," << ny << " runs " << over << " mm over the tower, after:\n" << context);
                if (gaps) {
                    CHECK(over < 0.5);
                    // A hop from away from the tower (not a move along its edge between two purges) enters
                    // across the entry's wall (tower-local X), not along it.
                    if (!outer.front().contains(from)) {
                        const Vec2d hop = to_local * Vec2d(nx - x, ny - y);
                        CHECK(std::abs(hop.y()) <= 0.05 + 0.1 * std::abs(hop.x()));
                    }
                }
                if (over > 5.)
                    ++crossing;
            }
            x = nx;
            y = ny;
        }
        REQUIRE(entries > 0);
        if (!gaps && angle == 0)
            CHECK(crossing > 0); // stock: the far-side entries hop straight over the tower
    }
}

// Orca #16789: the Type1 tower's HEIGHT/WIDTH tags keep a decimal point in any locale.

// Switches this thread's numeric locale to one whose decimal separator is a comma,
// when the host has one installed. Edge has no shared helper; keep it in this file.
// On Unix, apply() uses uselocale so it overrides a CNumericLocalesSetter already
// on the stack (setlocale alone would not: the setter holds the thread locale at C).
struct CommaNumericLocale
{
#ifdef _WIN32
    bool apply()
    {
        static const char *const names[] = {"de-DE", "German_Germany.1252", "German", "fr-FR", "French"};
        for (const char *name : names)
            if (std::setlocale(LC_NUMERIC, name) != nullptr)
                return true;
        return false;
    }
#else
    locale_t locale{(locale_t) 0};

    bool apply()
    {
        static const char *const names[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR"};
        for (const char *name : names) {
            if ((locale = newlocale(LC_NUMERIC_MASK, name, (locale_t) 0)) == (locale_t) 0)
                continue;
            uselocale(locale);
            return true;
        }
        return false;
    }

    // Freed once the setters around apply() have put the thread's own locale back.
    ~CommaNumericLocale()
    {
        if (locale != (locale_t) 0)
            freelocale(locale);
    }

    CommaNumericLocale()                                      = default;
    CommaNumericLocale(const CommaNumericLocale &)            = delete;
    CommaNumericLocale &operator=(const CommaNumericLocale &) = delete;
#endif
};

static DynamicPrintConfig wipe_tower_prime_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = {15.};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = true;
    config.option<ConfigOptionFloat>("prime_tower_width")->value             = 35.;
    config.set_deserialize_strict({
        {"skirt_loops",                0},
        {"brim_type",                  "no_brim"},
        {"machine_start_gcode",        ""},
        {"machine_end_gcode",          ""},
        {"wipe_tower_wall_type",       "rectangle"},
        {"layer_height",               0.3},
        {"initial_layer_print_height", 0.3},
        {"gcode_flavor",               "klipper"},
    });
    return config;
}

// init_print's auto_assign_extruders writes volume extruder=1, and that stamp overwrites
// print-level wall/infill filaments. Set the feature filaments on the volume after that
// so both tools actually print. Walls filament 2, infill filament 1, matching Orca #17204.
static void set_object_filaments(ModelObject *object, int wall, int infill)
{
    object->config.set("wall_filament", wall);
    object->config.set("outer_wall_filament", wall);
    object->config.set("sparse_infill_filament", infill);
    object->config.set("solid_infill_filament", infill);
    if (object->volumes.empty())
        return;
    ModelVolume *volume = object->volumes.front();
    volume->config.set("extruder", 0);
    volume->config.set("wall_filament", wall);
    volume->config.set("outer_wall_filament", wall);
    volume->config.set("sparse_infill_filament", infill);
    volume->config.set("solid_infill_filament", infill);
}

static void set_both_filaments(Model &model)
{
    for (ModelObject *object : model.objects)
        set_object_filaments(object, 2, 1);
}

TEST_CASE("The wipe tower's flow tags keep a decimal point in a comma-decimal locale", "[WipeTower]")
{
    const bool is_bbl = GENERATE(false, true);
    CAPTURE(is_bbl);
    const bool previous_bbl = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl([previous_bbl] { GCodeProcessor::s_IsBBLPrinter = previous_bbl; });
    // GCodeProcessor parses these values with the C locale: a comma makes it log an error and
    // ignore the tag, leaving the extrusion width and height it assumes to its own defaults.
    CommaNumericLocale    comma;
    CNumericLocalesSetter restore;
    if (!comma.apply() || is_decimal_separator_point()) {
        // Catch2 v2 has no SKIP(). Succeed-and-return is the skip this suite uses
        // (see test_printgcode.cpp): the case does not fail, and NoAssertions does not fire.
        SUCCEED("skipped: no locale with a comma decimal separator is installed");
        return;
    }
    DynamicPrintConfig config = wipe_tower_prime_config();
    config.set_deserialize_strict({{"wipe_tower_type", "type1"}});
    Print print;
    print.is_BBL_printer() = is_bbl;
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
    print.is_BBL_printer() = is_bbl;
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    std::string tower_gcode;
    for (const auto &layer : print.wipe_tower_data().tool_changes)
        for (const auto &change : layer)
            tower_gcode += change.gcode;
    REQUIRE_FALSE(tower_gcode.empty());
    // The width and height tags are both written by the writer's constructor, before the
    // tower's start tag, so scan the complete tower blocks without the model's tags.
    const std::string height_tag = is_bbl ? "; LAYER_HEIGHT: " : ";HEIGHT:";
    const std::string width_tag  = is_bbl ? "; LINE_WIDTH: " : ";WIDTH:";
    size_t            heights    = 0;
    size_t            widths     = 0;
    std::istringstream lines(tower_gcode);
    for (std::string line; std::getline(lines, line);) {
        const bool is_height = line.compare(0, height_tag.size(), height_tag) == 0;
        const bool is_width  = line.compare(0, width_tag.size(), width_tag) == 0;
        if (!is_height && !is_width)
            continue;
        if (is_height)
            ++heights;
        else
            ++widths;
        if (line.find(',') != std::string::npos)
            FAIL_CHECK(line);
    }
    CHECK(heights > 0);
    CHECK(widths > 0);
}

TEST_CASE("Tower consumption is classified separately for each printer kind", "[WipeTower][Regression]")
{
    const auto row = GENERATE(table<bool, const char *>({
        {false, "type1"}, {false, "type2"}, {true, "type1"}
    }));
    const bool is_bbl = std::get<0>(row);
    const char *tower_type = std::get<1>(row);
    CAPTURE(is_bbl, tower_type);
    const bool previous_bbl = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore_bbl([previous_bbl] { GCodeProcessor::s_IsBBLPrinter = previous_bbl; });

    DynamicPrintConfig config = wipe_tower_prime_config();
    config.set_deserialize_strict({{"wipe_tower_type", tower_type}});
    Print print;
    print.is_BBL_printer() = is_bbl;
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
    print.is_BBL_printer() = is_bbl;
    print.apply(model, config);
    print.set_status_silent();

    GCodeProcessor::s_IsBBLPrinter = !is_bbl;
    print.process();

    GCodeProcessorResult result;
    gcode(print, result);
    const auto &statistics = result.print_statistics;
    REQUIRE(statistics.wipe_tower_volumes_per_extruder.size() == 2);
    for (size_t filament_id : {size_t(0), size_t(1)}) {
        CAPTURE(filament_id);
        REQUIRE(statistics.wipe_tower_volumes_per_extruder.count(filament_id) == 1);
        CHECK(statistics.wipe_tower_volumes_per_extruder.at(filament_id) > 0.);
        REQUIRE(statistics.model_volumes_per_extruder.count(filament_id) == 1);
        CHECK(statistics.model_volumes_per_extruder.at(filament_id) > 0.);
    }
}

// Two cubes at 0.2 mm layers with precise Z height on: the 20 mm cube stays on the 0.2 mm grid, and
// precise Z height spreads the other's gap to `short_height` over its last five layers, moving them
// off the grid. Slices and exports the plate; the emitter throws if it loses step with the tower.
// Both filaments print on every layer of both cubes (walls 2, infill 1), matching Orca #17204, so
// every off-grid layer has a wipe tower and join_layer can fire. A second apply after the volume
// filament stamp is what keeps enable_prime_tower: the first apply from init_print still sees one
// volume extruder and normalize_fdm_2 would otherwise clear it.
static std::string slice_precise_z_objects(DynamicPrintConfig config, const char *tower_type, const std::vector<double> &heights, Print &print)
{
    config.set_deserialize_strict({
        {"precise_z_height", "1"},
        {"wipe_tower_type", tower_type},
        {"wipe_tower_x", "15"},
        {"wipe_tower_y", "15"},
        {"wall_filament", "2"},
        {"outer_wall_filament", "2"},
        {"sparse_infill_filament", "1"},
        {"solid_infill_filament", "1"},
        // Type2 reserves ramming depth that Type1 (Bambu) does not; the sliver
        // join assertion is on wipe depth staying under 30 mm, not ramming.
        {"enable_filament_ramming", "0"},
    });
    const bool is_bbl = std::string(tower_type) == "type1";
    Model model;
    std::vector<TriangleMesh> meshes;
    for (double height : heights)
        meshes.emplace_back(TriangleMesh(make_cube(20., 20., height)));
    init_print(std::move(meshes), print, model, config);
    set_both_filaments(model);
    print.is_BBL_printer() = is_bbl;
    print.apply(model, config);
    REQUIRE(print.has_wipe_tower());
    std::string out;
    REQUIRE_NOTHROW(out = gcode(print));
    CHECK_FALSE(out.empty());
    return out;
}

static std::string slice_precise_z_pair(DynamicPrintConfig config, const char *tower_type, double short_height, Print &print)
{
    return slice_precise_z_objects(std::move(config), tower_type, {20., short_height}, print);
}

static std::vector<double> tower_extrusion_z(const std::string &gcode)
{
    std::vector<double> zs;
    bool                in_tower = false;
    GCodeReader         parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view raw = line.raw();
        if (raw.rfind(";TYPE:", 0) == 0 || raw.rfind("; FEATURE: ", 0) == 0)
            in_tower = raw.find("Prime tower") != std::string_view::npos;
        else if (in_tower && line.extruding(self) && (line.has_x() || line.has_y()))
            zs.emplace_back(self.z());
    });
    return zs;
}

static size_t count_joined_layers(const Print &print)
{
    size_t joined = 0;
    for (const LayerTools &lt : print.wipe_tower_data().tool_ordering)
        joined += lt.joins_wipe_tower_layer_below() ? 1 : 0;
    return joined;
}

TEST_CASE("A layer less than the minimum layer height above another prints its tool changes on the tower layer below", "[WipeTower]")
{
    const char  *tower_type   = GENERATE("type1", "type2");
    const double short_height = GENERATE(10.004, 10.15);
    CAPTURE(tower_type, short_height);
    DynamicPrintConfig config = wipe_tower_prime_config();
    config.set_deserialize_strict({{"min_layer_height", "0.07"}, {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}});

    Print print;
    const std::string out = slice_precise_z_pair(config, tower_type, short_height, print);

    CHECK(count_joined_layers(print) > 0);
    std::vector<double> tower_layer_z;
    for (const LayerTools &lt : print.wipe_tower_data().tool_ordering)
        if (lt.joins_wipe_tower_layer_below())
            CHECK(lt.print_z - lt.wipe_tower_print_z < 0.07);
        else if (lt.has_wipe_tower)
            tower_layer_z.emplace_back(lt.print_z);
    CHECK(print.wipe_tower_data().depth < 30.f);

    const std::vector<double> zs = tower_extrusion_z(out);
    REQUIRE_FALSE(zs.empty());
    size_t off_layer = 0;
    for (double z : zs)
        if (std::none_of(tower_layer_z.begin(), tower_layer_z.end(), [z](double t) { return std::abs(z - t) < 0.002; }))
            ++off_layer;
    CHECK(off_layer == 0);

    BoundingBox tower_box(print.first_layer_wipe_tower_corners());
    tower_box.offset(scaled(5.));
    size_t descents = 0, off_tower = 0;
    double layer_z  = 0.;
    GCodeReader parser;
    parser.parse_buffer(out, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view raw = line.raw();
        // Type1 on a Bambu printer writes "; Z_HEIGHT:", everyone else writes ";Z:".
        if (raw.rfind(";Z:", 0) == 0)
            layer_z = std::stod(std::string(raw.substr(3)));
        else if (raw.rfind("; Z_HEIGHT:", 0) == 0)
            layer_z = std::stod(std::string(raw.substr(11)));
        else if (line.has_z() && ! line.has_x() && ! line.has_y() && line.z() < layer_z - 1e-3) {
            ++descents;
            if (! tower_box.contains(Point(scaled(self.x()), scaled(self.y())))) {
                INFO("descent to Z " << line.z() << " at X " << self.x() << " Y " << self.y() << ", layer Z " << layer_z);
                ++off_tower;
            }
        }
    });
    CHECK(descents > 0);
    CHECK(off_tower == 0);
}

TEST_CASE("A run of joined layers never spans more than the minimum layer height", "[WipeTower]")
{
    // Three cubes with precise Z height: near the top the 10.05 and 10.1 mm cubes' layers sit 0.04 to
    // 0.05 mm apart, each under the 0.07 mm minimum but together over it. Measuring each gap from the
    // layer just below instead of from the tower layer chained them all onto one tower layer, and the
    // next tower layer came out up to a whole layer too thick. The gap counts from the tower layer.
    const char *tower_type = GENERATE("type1", "type2");
    CAPTURE(tower_type);
    DynamicPrintConfig config = wipe_tower_prime_config();
    config.set_deserialize_strict({{"min_layer_height", "0.07"}, {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}});

    Print print;
    slice_precise_z_objects(config, tower_type, {20., 10.05, 10.1}, print);

    CHECK(count_joined_layers(print) > 0);
    for (const LayerTools &lt : print.wipe_tower_data().tool_ordering) {
        if (lt.joins_wipe_tower_layer_below()) {
            INFO("joined layer at " << lt.print_z << " onto the tower layer at " << lt.wipe_tower_print_z);
            CHECK(lt.print_z - lt.wipe_tower_print_z < 0.07);
        } else if (lt.has_wipe_tower) {
            INFO("tower layer at " << lt.print_z);
            CHECK(lt.wipe_tower_layer_height < 0.2 + 0.07);
        }
    }
}

TEST_CASE("Layers sliced at the printer's minimum layer height keep their own tower layers", "[WipeTower]")
{
    const char *tower_type = GENERATE("type1", "type2");
    CAPTURE(tower_type);
    DynamicPrintConfig config = wipe_tower_prime_config();
    config.set_deserialize_strict({
        {"layer_height", "0.08"},
        {"initial_layer_print_height", "0.08"},
        {"min_layer_height", "0.08"},
        {"wipe_tower_type", tower_type},
    });

    Print print;
    Model model;
    config.set_deserialize_strict({
        {"wall_filament", "2"},
        {"outer_wall_filament", "2"},
        {"sparse_infill_filament", "1"},
        {"solid_infill_filament", "1"},
    });
    init_print({make_cube(20., 20., 20.)}, print, model, config);
    set_both_filaments(model);
    print.is_BBL_printer() = std::string(tower_type) == "type1";
    print.apply(model, config);
    REQUIRE(print.has_wipe_tower());
    REQUIRE_NOTHROW(gcode(print));
    CHECK(count_joined_layers(print) == 0);
}

TEST_CASE("A joined layer that keeps its filament leaves finishing the tower layer to the one that changed", "[WipeTower]")
{
    const char *tower_type = GENERATE("type1", "type2");
    const int   filament   = GENERATE(1, 2);
    CAPTURE(tower_type, filament);
    DynamicPrintConfig config = wipe_tower_prime_config();

    Print print;
    Model model;
    config.set_deserialize_strict({
        {"precise_z_height", "1"},
        {"wipe_tower_type", tower_type},
        {"min_layer_height", "0.07"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"wall_filament", "2"},
        {"outer_wall_filament", "2"},
        {"sparse_infill_filament", "1"},
        {"solid_infill_filament", "1"},
    });
    init_print({make_cube(20., 20., 20.), make_cube(20., 20., 10.004)}, print, model, config);
    REQUIRE(model.objects.size() == 2);
    // The 20 mm cube prints both filaments on every layer; the short one only `filament`.
    // Setting both objects to filament 1 used to make normalize_fdm_2 clear the tower.
    set_object_filaments(model.objects[0], 2, 1);
    set_object_filaments(model.objects[1], filament, filament);
    print.is_BBL_printer() = std::string(tower_type) == "type1";
    print.apply(model, config);
    REQUIRE(print.has_wipe_tower());
    REQUIRE_NOTHROW(gcode(print));

    size_t       keeps_filament = 0;
    unsigned int last           = (unsigned int) -1;
    for (const LayerTools &lt : print.wipe_tower_data().tool_ordering) {
        if (lt.joins_wipe_tower_layer_below() && lt.extruders.size() == 1 && lt.extruders.front() == last)
            ++keeps_filament;
        if (! lt.extruders.empty())
            last = lt.extruders.back();
    }
    CHECK(keeps_filament > 0);
}

TEST_CASE("Smooth timelapse prints each tower layer's wall once when a layer joins it", "[WipeTower]")
{
    DynamicPrintConfig config = wipe_tower_prime_config();
    config.set_deserialize_strict({
        {"timelapse_type", "1"},
        {"min_layer_height", "0.07"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
    });

    Print print;
    slice_precise_z_pair(config, "type1", 10.004, print);
    CHECK(count_joined_layers(print) > 0);
}
