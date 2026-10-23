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
