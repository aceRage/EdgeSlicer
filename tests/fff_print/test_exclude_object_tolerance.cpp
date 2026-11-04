#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

// Orca #16503: the outline written to EXCLUDE_OBJECT_DEFINE is simplified with exclude_object_tolerance.

namespace {

std::string slice_with_tolerance(const std::string &tolerance)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "gcode_flavor", "klipper" }, { "exclude_object", "1" } });
    if (!tolerance.empty())
        config.set_deserialize_strict({ { "exclude_object_tolerance", tolerance } });
    // Default facet angle (2 degrees): 180 hull points, well inside every tolerance below.
    return Test::slice({ make_cylinder(20., 5.) }, config);
}

// The POLYGON=[[x,y],...] on the first EXCLUDE_OBJECT_DEFINE line.
std::string first_polygon(const std::string &gcode)
{
    const size_t define = gcode.find("EXCLUDE_OBJECT_DEFINE");
    REQUIRE(define != std::string::npos);
    const size_t begin = gcode.find("POLYGON=", define);
    REQUIRE(begin != std::string::npos);
    return gcode.substr(begin, gcode.find('\n', begin) - begin);
}

size_t polygon_points(const std::string &polygon)
{
    size_t points = 1;
    for (size_t pos = polygon.find("],["); pos != std::string::npos; pos = polygon.find("],[", pos + 1))
        ++points;
    return points;
}

} // namespace

// A larger value must leave fewer points, which keeps the exclude-object array small on printers that stall
// while parsing it.
TEST_CASE("A larger exclude object tolerance writes fewer outline points", "[GCode][ExcludeObject][Orca16503]")
{
    const size_t fine   = polygon_points(first_polygon(slice_with_tolerance("0.1")));
    const size_t coarse = polygon_points(first_polygon(slice_with_tolerance("1")));
    INFO("points at 0.1 mm: " << fine << ", at 1 mm: " << coarse);
    CHECK(coarse >= 4);
    CHECK(coarse < fine);
}

// EDGE: existing projects must keep writing the outlines they wrote when the Douglas-Peucker tolerance was a
// constant 0.5 mm (upstream's constant was 0.1 mm, so its default is 0.1).
TEST_CASE("The exclude object tolerance defaults to the former fixed 0.5 mm", "[GCode][ExcludeObject][Orca16503]")
{
    CHECK(DynamicPrintConfig::full_print_config().opt_float("exclude_object_tolerance") == Approx(0.5));
    CHECK(first_polygon(slice_with_tolerance("")) == first_polygon(slice_with_tolerance("0.5")));
    CHECK(first_polygon(slice_with_tolerance("")) != first_polygon(slice_with_tolerance("0.1")));
}

// A user process preset saves and loads the options listed in Preset::print_options().
TEST_CASE("The exclude object tolerance is a process preset option", "[Preset][ExcludeObject][Orca16503]")
{
    const std::vector<std::string> &options = Preset::print_options();
    CHECK(std::find(options.begin(), options.end(), "exclude_object_tolerance") != options.end());
}
