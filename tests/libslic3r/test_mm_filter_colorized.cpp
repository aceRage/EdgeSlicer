#include <catch2/catch.hpp>

#include "libslic3r/Line.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"

#include <vector>

using namespace Slic3r;

// filter_colorized_polygon's last pass: a run of 0.5 mm or less along the wall between two runs of
// one painted colour takes that colour (PrusaSlicer #7112, "Default color sometimes appeared between
// two multi-material painted areas"). Until this fix it recoloured the run AFTER the short one,
// which has that colour already, so the pass changed nothing.
namespace {

// A closed outline, one ColoredLine per (point, colour), each line running to the next point.
ColoredLines outline(const std::vector<std::pair<Vec2d, int>> &pts)
{
    ColoredLines lines;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Vec2d &a = pts[i].first;
        const Vec2d &b = pts[(i + 1) % pts.size()].first;
        lines.push_back(ColoredLine{Line(Point::new_scale(a.x(), a.y()), Point::new_scale(b.x(), b.y())), pts[i].second});
    }
    return lines;
}

// A 20 x 10 mm rectangle: the bottom edge in colour 1, then up the right edge a short run of
// `mid_colour` of `run_mm` cut into three lines (so the line-level passes, which look at one or two
// lines, do not see it), the rest of the right edge and the top edge in colour 1, the left edge in
// colour 2 (so the two colour-1 runs are not one run across the wrap).
ColoredLines short_run_between_painted(int mid_colour, double run_mm)
{
    const double s = run_mm / 3.;
    return outline({
        {{0., 0.}, 1},
        {{20., 0.}, mid_colour},
        {{20., s}, mid_colour},
        {{20., 2. * s}, mid_colour},
        {{20., run_mm}, 1},
        {{20., 10.}, 1},
        {{0., 10.}, 2},
    });
}

} // namespace

TEST_CASE("filter_colorized_polygon: a short run between two runs of one painted colour takes that colour", "[MultiMaterialSegmentation]")
{
    SECTION("a 0.4 mm unpainted run between two colour-1 runs") {
        const ColoredLines out = filter_colorized_polygon(short_run_between_painted(0, 0.4), 1.f);
        REQUIRE(out.size() == 7);
        CHECK(out[1].color == 1);
        CHECK(out[2].color == 1);
        CHECK(out[3].color == 1);
        CHECK(out[6].color == 2);
    }
    SECTION("a 0.4 mm colour-3 run between two colour-1 runs") {
        const ColoredLines out = filter_colorized_polygon(short_run_between_painted(3, 0.4), 1.f);
        REQUIRE(out.size() == 7);
        CHECK(out[1].color == 1);
        CHECK(out[2].color == 1);
        CHECK(out[3].color == 1);
        CHECK(out[6].color == 2);
    }
    SECTION("a 0.6 mm run is longer than the limit and keeps its colour") {
        const ColoredLines out = filter_colorized_polygon(short_run_between_painted(0, 0.6), 1.f);
        REQUIRE(out.size() == 7);
        CHECK(out[1].color == 0);
        CHECK(out[2].color == 0);
        CHECK(out[3].color == 0);
    }
    SECTION("the limit scales with run_scale: 0.4 mm is over it at a 0.2 mm nozzle (scale 0.5)") {
        const ColoredLines out = filter_colorized_polygon(short_run_between_painted(0, 0.4), 0.5f);
        REQUIRE(out.size() == 7);
        CHECK(out[1].color == 0);
        CHECK(out[2].color == 0);
        CHECK(out[3].color == 0);
    }
    SECTION("between two runs of the default colour nothing changes") {
        ColoredLines in = short_run_between_painted(3, 0.4);
        for (ColoredLine &l : in)
            if (l.color == 1) l.color = 0;
        const ColoredLines out = filter_colorized_polygon(std::move(in), 1.f);
        REQUIRE(out.size() == 7);
        CHECK(out[1].color == 3);
        CHECK(out[2].color == 3);
        CHECK(out[3].color == 3);
    }
}

