// Package 6A-5 of the Orca upstream catch-up: small print options.
#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Arachne/WallToolPaths.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

size_t count_occurrences(const std::string &haystack, const std::string &needle)
{
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
        ++n;
    return n;
}

} // namespace

// ---------------------------------------------------------------------------
// OrcaSlicer #11784: extrusion-role change G-code in process and filament
// ---------------------------------------------------------------------------
TEST_CASE("Role-change G-code options default to empty", "[SmallPrintOptions][RoleChangeGCode]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.has("process_change_extrusion_role_gcode"));
    REQUIRE(config.has("filament_change_extrusion_role_gcode"));
    CHECK(config.opt_string("process_change_extrusion_role_gcode").empty());
    CHECK(config.option<ConfigOptionStrings>("filament_change_extrusion_role_gcode")->get_at(0).empty());
}

TEST_CASE("Machine, filament and process role-change G-code run in that order", "[SmallPrintOptions][RoleChangeGCode]")
{
    const std::string gcode = slice({ TestMesh::cube_20x20x20 }, {
        { "change_extrusion_role_gcode",          ";ROLE_MACHINE {extrusion_role}" },
        // coStrings: quoted, as the serializer writes it (";" is the vector separator).
        { "filament_change_extrusion_role_gcode", "\";ROLE_FILAMENT {extrusion_role}\"" },
        { "process_change_extrusion_role_gcode",  ";ROLE_PROCESS {extrusion_role}" },
    });
    const size_t machine  = count_occurrences(gcode, ";ROLE_MACHINE ");
    const size_t filament = count_occurrences(gcode, ";ROLE_FILAMENT ");
    const size_t process  = count_occurrences(gcode, ";ROLE_PROCESS ");
    REQUIRE(machine > 0);
    CHECK(filament == machine);
    CHECK(process == machine);

    const size_t m = gcode.find(";ROLE_MACHINE ");
    const size_t f = gcode.find(";ROLE_FILAMENT ", m);
    const size_t p = gcode.find(";ROLE_PROCESS ", f);
    REQUIRE(f != std::string::npos);
    REQUIRE(p != std::string::npos);
    // The three blocks are written back to back at the same role change.
    CHECK(gcode.find(";ROLE_MACHINE ", m + 1) > p);
}

TEST_CASE("Filament or process role-change G-code works without the machine one", "[SmallPrintOptions][RoleChangeGCode]")
{
    const std::string gcode = slice({ TestMesh::cube_20x20x20 }, {
        { "process_change_extrusion_role_gcode", ";ROLE_PROCESS_ONLY {last_extrusion_role}->{extrusion_role}" },
    });
    CHECK(count_occurrences(gcode, ";ROLE_PROCESS_ONLY ") > 0);
}

// ---------------------------------------------------------------------------
// OrcaSlicer #11925: Arachne maximum wall resolution / deviation
// ---------------------------------------------------------------------------
TEST_CASE("Arachne wall resolution options default to the former constants", "[SmallPrintOptions][Arachne]")
{
    const PrintObjectConfig &object_defaults = PrintObjectConfig::defaults();
    CHECK(object_defaults.wall_maximum_resolution.value == Approx(0.5));
    CHECK(object_defaults.wall_maximum_deviation.value == Approx(0.025));

    PrintConfig print_config;
    const Arachne::WallToolPathsParams params = Arachne::make_paths_params(1, object_defaults, print_config);
    CHECK(params.wall_maximum_resolution == Arachne::meshfix_maximum_resolution());
    CHECK(params.wall_maximum_deviation == Arachne::meshfix_maximum_deviation());

    // A default-constructed parameter set (concentric fill) keeps the constants too.
    const Arachne::WallToolPathsParams plain;
    CHECK(plain.wall_maximum_resolution == Arachne::meshfix_maximum_resolution());
    CHECK(plain.wall_maximum_deviation == Arachne::meshfix_maximum_deviation());
}

TEST_CASE("Arachne wall resolution options reach the wall generator", "[SmallPrintOptions][Arachne]")
{
    PrintObjectConfig object_config = PrintObjectConfig::defaults();
    object_config.wall_maximum_resolution.value = 0.1;
    object_config.wall_maximum_deviation.value  = 0.01;
    PrintConfig print_config;
    const Arachne::WallToolPathsParams params = Arachne::make_paths_params(1, object_config, print_config);
    CHECK(params.wall_maximum_resolution == scaled<coord_t>(0.1));
    CHECK(params.wall_maximum_deviation == scaled<coord_t>(0.01));

    // A finer resolution keeps at least as many wall points on a curved model.
    auto wall_points = [](const char *resolution) {
        // A 15 mm sphere: curved walls without a long slice.
        TriangleMesh sphere = mesh(TestMesh::sphere_50mm);
        sphere.scale(0.3f);
        const std::string gcode = slice({ sphere }, {
            { "wall_generator", "arachne" },
            { "wall_maximum_resolution", resolution },
            { "wall_maximum_deviation", "0.025" },
        });
        return count_occurrences(gcode, "\nG1 X");
    };
    CHECK(wall_points("0.05") >= wall_points("0.5"));
}

// ---------------------------------------------------------------------------
// OrcaSlicer #13870: curled-perimeter slowdown off by default (owner accepted, 2026-10-15)
// ---------------------------------------------------------------------------
TEST_CASE("slowdown_for_curled_perimeters is off by default", "[SmallPrintOptions][Defaults]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const auto *opt = config.option<ConfigOptionBools>("slowdown_for_curled_perimeters");
    REQUIRE(opt != nullptr);
    REQUIRE_FALSE(opt->values.empty());
    for (const unsigned char v : opt->values)
        CHECK_FALSE(bool(v));
    CHECK_FALSE(PrintRegionConfig::defaults().slowdown_for_curled_perimeters.get_at(0));
}

// ---------------------------------------------------------------------------
// OrcaSlicer #13848: Lightning infill angles
// ---------------------------------------------------------------------------
TEST_CASE("Lightning infill angles default to 45 degrees", "[SmallPrintOptions][Lightning]")
{
    const PrintRegionConfig &defaults = PrintRegionConfig::defaults();
    CHECK(defaults.lightning_overhang_angle.value == Approx(45.));
    CHECK(defaults.lightning_prune_angle.value == Approx(45.));
    CHECK(defaults.lightning_straightening_angle.value == Approx(45.));
    // The generator converts with value * PI / 180; at the default that is exactly the former PI / 4.
    CHECK(defaults.lightning_overhang_angle.value * M_PI / 180.0 == M_PI / 4);
}

TEST_CASE("Lightning infill angles change the lightning infill", "[SmallPrintOptions][Lightning]")
{
    auto lightning = [](const char *angle) {
        return slice({ TestMesh::cube_20x20x20 }, {
            { "sparse_infill_pattern", "lightning" },
            { "sparse_infill_density", "15%" },
            { "lightning_overhang_angle", angle },
            { "lightning_prune_angle", angle },
            { "lightning_straightening_angle", angle },
        });
    };
    const std::string at_default = lightning("45");
    REQUIRE_FALSE(at_default.empty());
    CHECK(lightning("80").size() != at_default.size());
}

// ---------------------------------------------------------------------------
// OrcaSlicer #13459 (+ #15232): small support perimeter speed
// ---------------------------------------------------------------------------
TEST_CASE("Small support perimeter options default to off", "[SmallPrintOptions][SmallSupportPerimeters]")
{
    const PrintObjectConfig &defaults = PrintObjectConfig::defaults();
    CHECK(defaults.small_support_perimeter_threshold.get_at(0) == Approx(0.));
    CHECK(defaults.small_support_perimeter_speed.get_at(0).percent);
    CHECK(defaults.small_support_perimeter_speed.get_at(0).value == Approx(50.));
}

TEST_CASE("Small support perimeters print at the small support speed", "[SmallPrintOptions][SmallSupportPerimeters]")
{
    // 31 mm/s support, 50% for small support perimeters = 15.5 mm/s = F930. Layer-time
    // slowdown is off so the cooling buffer leaves the feed rates alone.
    auto support_gcode = [](const char *threshold) {
        return slice({ TestMesh::overhang }, {
            { "enable_support", "1" },
            { "support_speed", "31" },
            { "support_interface_speed", "31" },
            { "slow_down_for_layer_cooling", "0" },
            { "small_support_perimeter_speed", "50%" },
            { "small_support_perimeter_threshold", threshold },
        });
    };
    const std::string off = support_gcode("0");
    const std::string on  = support_gcode("1000");
    REQUIRE(off.find(";TYPE:Support") != std::string::npos);
    CHECK(count_occurrences(off, "F930\n") == 0);
    CHECK(count_occurrences(on, "F930\n") > 0);
}

TEST_CASE("An object-level small support perimeter speed is read as a percentage", "[SmallPrintOptions][SmallSupportPerimeters]")
{
    // OrcaSlicer #15232: Model::findMaxSpeed used to read the FloatOrPercent option as a plain float
    // and crash on object-level overrides.
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(mesh(TestMesh::cube_20x20x20));
    object->add_instance();
    object->config.set_key_value("support_speed", new ConfigOptionFloats{ 100. });
    object->config.set_key_value("small_support_perimeter_speed", new ConfigOptionFloatsOrPercents{ FloatOrPercent{300., true} });
    double max_speed = 0.;
    REQUIRE_NOTHROW(max_speed = Model::findMaxSpeed(object));
    // 300% of the object's 100 mm/s support speed (the global speed map may only add to it).
    CHECK(max_speed >= 300. - EPSILON);
}

// ---------------------------------------------------------------------------
// OrcaSlicer #12349: limit polyhole edges
// ---------------------------------------------------------------------------
namespace Slic3r {
// Defined in PrintObject.cpp.
Polygons create_polyholes(const Point center, const coord_t radius, const coord_t nozzle_diameter, bool multiple, int max_edges);
}

TEST_CASE("Polyhole edge count is capped by hole_to_polyhole_max_edges", "[SmallPrintOptions][Polyholes]")
{
    CHECK(PrintRegionConfig::defaults().hole_to_polyhole_max_edges.value == 50);

    // 4 * 20 * 0.4 / 0.4 = 80 edges without a cap.
    const Polygons uncapped = create_polyholes(Point(0, 0), scaled<coord_t>(20.), scaled<coord_t>(0.4), false, 1000);
    REQUIRE(uncapped.size() == 1);
    CHECK(uncapped.front().size() == 80);

    const Polygons capped = create_polyholes(Point(0, 0), scaled<coord_t>(20.), scaled<coord_t>(0.4), false, 50);
    REQUIRE(capped.size() == 1);
    CHECK(capped.front().size() == 50);

    // Small holes are below the cap and unchanged.
    const Polygons small = create_polyholes(Point(0, 0), scaled<coord_t>(2.), scaled<coord_t>(0.4), true, 50);
    REQUIRE(small.size() == 5);
    CHECK(small.front().size() == 8);
}
