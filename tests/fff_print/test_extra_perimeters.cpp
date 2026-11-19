// Extra perimeters on overhangs (the code ported from PrusaSlicer in PerimeterGenerator.cpp), called
// directly on hand-made layers:
// - a closed first perimeter that crosses its anchor is turned to start over the lower layer. It must
//   stay a closed loop, repeat no point, and start at the point furthest onto the lower layer;
// - reconnect_polylines returns its polylines in the order of its input, whatever the standard library.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

// Defined in PerimeterGenerator.cpp, not declared in a header.
namespace Slic3r {
Polylines reconnect_polylines(const Polylines &polylines, double limit_distance);
std::tuple<std::vector<ExtrusionPaths>, Polygons> generate_extra_perimeters_over_overhangs(ExPolygons               infill_area,
                                                                                           const Polygons          &lower_slices_polygons,
                                                                                           int                      perimeter_count,
                                                                                           const Flow              &overhang_flow,
                                                                                           double                   scaled_resolution,
                                                                                           const PrintObjectConfig &object_config,
                                                                                           const PrintConfig       &print_config);
} // namespace Slic3r

using namespace Slic3r;

namespace {

// A counter-clockwise regular polygon around the origin; vertex k lies at the angle 360 * k / vertices.
Polygon disc(double radius, int vertices)
{
    Polygon out;
    for (int k = 0; k < vertices; ++k) {
        const double a = 2. * PI * k / vertices;
        out.points.emplace_back(scaled<coord_t>(radius * std::cos(a)), scaled<coord_t>(radius * std::sin(a)));
    }
    return out;
}

Polygon rectangle(double x0, double y0, double x1, double y1)
{
    return Polygon{{scaled<coord_t>(x0), scaled<coord_t>(y0)},
                   {scaled<coord_t>(x1), scaled<coord_t>(y0)},
                   {scaled<coord_t>(x1), scaled<coord_t>(y1)},
                   {scaled<coord_t>(x0), scaled<coord_t>(y1)}};
}

Polyline segment(double x0, double y0, double x1, double y1)
{
    return Polyline{{scaled<coord_t>(x0), scaled<coord_t>(y0)}, {scaled<coord_t>(x1), scaled<coord_t>(y1)}};
}

} // namespace

TEST_CASE("A closed extra perimeter over its anchor stays closed and starts furthest onto the lower layer", "[ExtraPerimeters]")
{
    // A 20mm disc whose only support is a strip 0.8mm deep under its left edge (x <= -9.2mm), like a
    // roof standing on a thin wall. The strip is narrower than the anchor the extra perimeters ask for,
    // so no anchor area is left and the first extra perimeter is a full loop around the disc. That loop
    // runs over the strip, which makes it "closed and anchored".
    const double  lower_edge = -9.2;
    const Polygon infill     = disc(10., 72);
    const Polygon lower      = rectangle(-30., -30., lower_edge, 30.);
    const Flow    flow(0.45f, 0.2f, 0.4f);

    const FullPrintConfig &config = FullPrintConfig::defaults();
    auto [regions, filled] = generate_extra_perimeters_over_overhangs({ExPolygon(infill)}, {lower}, 2, flow, scaled<double>(0.0125), config,
                                                                      config);
    REQUIRE(regions.size() == 1);
    const ExtrusionPaths &paths = regions.front();
    REQUIRE(paths.size() >= 2);

    // No path repeats a point. Turning the loop without dropping its closing point first left that
    // point twice in a row in the middle of the path.
    for (const ExtrusionPath &path : paths)
        for (size_t i = 1; i < path.polyline.size(); ++i) {
            INFO("path " << (&path - paths.data()) << ", point " << i);
            REQUIRE(path.polyline.points[i] != path.polyline.points[i - 1]);
        }

    // The loop around the disc comes first and is still a loop: one vertex per vertex of the disc, plus
    // the closing point.
    const Points &loop = paths.front().polyline.points;
    CHECK(loop.size() == infill.size() + 1);
    CHECK(loop.front() == loop.back());
    for (const Point &p : loop) {
        const double r = unscaled<double>(p.cast<double>().norm());
        CHECK(r > 9.7);
        CHECK(r < 9.9);
    }

    // It starts at its point furthest onto the lower layer. The strip's edge is the line x = -9.2mm, so
    // that is the leftmost vertex, the one at 180 degrees; its neighbours are 0.04mm less deep.
    const Point leftmost      = *std::min_element(loop.begin(), loop.end(), [](const Point &a, const Point &b) { return a.x() < b.x(); });
    const auto  leftmost_seen = std::count_if(loop.begin(), loop.end(), [&leftmost](const Point &p) { return p.x() == leftmost.x(); });
    CHECK(leftmost_seen == 2);
    CHECK(loop.front() == leftmost);
    CHECK(unscaled<double>(loop.front().x()) < lower_edge - 0.5);
    CHECK(std::abs(unscaled<double>(loop.front().y())) < 0.01);
}

TEST_CASE("reconnect_polylines keeps the order of its input", "[ExtraPerimeters]")
{
    const double limit = scaled<double>(0.4);

    SECTION("nothing to join") {
        // Six pieces 5mm apart. The result used to come out of a hash table: last piece first with
        // libc++ and libstdc++, first piece first with the MSVC library.
        Polylines pieces;
        for (int i = 0; i < 6; ++i)
            pieces.emplace_back(segment(0., 5. * i, 3., 5. * i));

        const Polylines joined = reconnect_polylines(pieces, limit);
        REQUIRE(joined.size() == pieces.size());
        for (size_t i = 0; i < pieces.size(); ++i) {
            INFO("piece " << i);
            CHECK(joined[i].points == pieces[i].points);
        }
    }

    SECTION("joined pieces take the place of the first of them") {
        // 0 and 2 meet end to start, 1 and 4 end to end; 3 stands alone.
        const Polylines pieces = {segment(0., 0., 3., 0.), segment(0., 10., 3., 10.), segment(3.1, 0., 6., 0.), segment(0., 20., 3., 20.),
                                  segment(6., 10., 3.1, 10.)};

        const Polylines joined = reconnect_polylines(pieces, limit);
        REQUIRE(joined.size() == 3);
        CHECK(joined[0].points == Points{pieces[0].points[0], pieces[0].points[1], pieces[2].points[0], pieces[2].points[1]});
        CHECK(joined[1].points == Points{pieces[1].points[0], pieces[1].points[1], pieces[4].points[1], pieces[4].points[0]});
        CHECK(joined[2].points == pieces[3].points);
    }
}
