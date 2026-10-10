#include <catch2/catch.hpp>

#include "slic3r/GUI/SvgSplitDialog.hpp"

using namespace Slic3r::GUI;

// File > Import (Ctrl+I) and dropping several files: which files go to the SVG gizmo
// ("SVG" / "SVG (Split)" question) instead of the model loader (Model::read_from_file).
TEST_CASE("SVG import routing: .svg files go to the SVG gizmo", "[SvgImportRouting]")
{
    CHECK(is_svg_file("C:/logos/badge.svg"));
    CHECK(is_svg_file("C:/logos/BADGE.SVG"));
    CHECK(is_svg_file("relative/path with spaces/a.Svg"));
    CHECK_FALSE(is_svg_file("C:/models/part.stl"));
    CHECK_FALSE(is_svg_file("C:/models/project.3mf"));
    CHECK_FALSE(is_svg_file("C:/logos/badge.svgz"));
    CHECK_FALSE(is_svg_file("C:/logos/badge.svg.3mf"));
    CHECK_FALSE(is_svg_file("svg"));
}
