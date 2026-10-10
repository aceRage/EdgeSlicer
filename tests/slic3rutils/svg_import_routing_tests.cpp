#include <catch2/catch.hpp>

#include "slic3r/GUI/SvgSplitDialog.hpp"
#include "libslic3r/Model.hpp"

using namespace Slic3r;
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

TEST_CASE("SVG import routing: an import is split into SVG files and the others, in order", "[SvgImportRouting]")
{
    // e.g. File > Import of two SVGs and an STL
    const std::vector<std::string> files = {"C:/a/logo.svg", "C:/a/part.stl", "C:/a/BADGE.SVG", "C:/a/project.3mf"};
    SvgImportPartition p = partition_svg_files(files);
    CHECK(p.svg == std::vector<size_t>{0, 2});
    CHECK(p.other == std::vector<size_t>{1, 3});

    CHECK(partition_svg_files({}).svg.empty());
    CHECK(partition_svg_files({"C:/a/part.stl"}).svg.empty());
    CHECK(partition_svg_files({"C:/a/part.stl"}).other == std::vector<size_t>{0});
}

TEST_CASE("SVG import routing: which targets ask SVG / SVG (Split)", "[SvgImportRouting]")
{
    // new object (File > Import, toolbar, Home, recent files, drop) and Add part > Load... ask
    CHECK(svg_import_asks(ModelVolumeType::INVALID));
    CHECK(svg_import_asks(ModelVolumeType::MODEL_PART));
    // a split negative volume or modifier would act like the union: always the plain SVG
    CHECK_FALSE(svg_import_asks(ModelVolumeType::NEGATIVE_VOLUME));
    CHECK_FALSE(svg_import_asks(ModelVolumeType::PARAMETER_MODIFIER));
}
