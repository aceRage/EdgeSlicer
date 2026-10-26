#include <catch2/catch.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/PrintConfig.hpp"

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
