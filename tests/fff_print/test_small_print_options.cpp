// Package 6A-5 of the Orca upstream catch-up: small print options.
#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
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
