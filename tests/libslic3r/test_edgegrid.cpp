#include <catch2/catch.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/EdgeGrid.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Snapmaker #752 / Orca #12806: visit_cells_intersecting_line used to walk (ix, iy)
// past the grid on painted-line projection (closed bbox.max, or an exact cell-corner
// crossing). Release builds stripped the asserts, so PaintedLineVisitor then
// OOB-read m_cells / m_contours. The runtime bounds check must keep every visitor
// call inside the grid.

namespace {

struct InBoundsVisitor
{
    const EdgeGrid::Grid *grid = nullptr;
    int                   visits = 0;
    bool                  out_of_bounds = false;

    bool operator()(coord_t iy, coord_t ix)
    {
        ++visits;
        if (iy < 0 || ix < 0 || size_t(iy) >= grid->rows() || size_t(ix) >= grid->cols()) {
            out_of_bounds = true;
            return false;
        }
        // Touch the cell payload the same way PaintedLineVisitor does.
        (void) grid->cell_data_range(iy, ix);
        return true;
    }
};

EdgeGrid::Grid make_square_grid()
{
    Polygon square{{0, 0}, {1000, 0}, {1000, 1000}, {0, 1000}};
    EdgeGrid::Grid grid;
    grid.create(Polygons{square}, coord_t(100));
    REQUIRE(grid.cols() > 0);
    REQUIRE(grid.rows() > 0);
    return grid;
}

} // namespace

TEST_CASE("EdgeGrid painted-line walk stays inside the grid", "[EdgeGrid][PaintOverflow]")
{
    EdgeGrid::Grid grid = make_square_grid();
    const BoundingBox &bbox = grid.bbox();

    auto walk = [&](Point p1, Point p2) {
        REQUIRE(bbox.contains(p1));
        REQUIRE(bbox.contains(p2));
        InBoundsVisitor visitor;
        visitor.grid = &grid;
        REQUIRE_NOTHROW(grid.visit_cells_intersecting_line(p1, p2, visitor));
        CHECK_FALSE(visitor.out_of_bounds);
        CHECK(visitor.visits > 0);
    };

    SECTION("closed bbox.max endpoint (the painted clip case)") {
        walk(bbox.min, bbox.max);
    }

    SECTION("45-degree cell-corner crossing") {
        // Equal dx/dy so the Bresenham error terms hit ex == ey.
        Point p1 = bbox.min;
        Point p2 = bbox.min + Point{grid.resolution() * 4, grid.resolution() * 4};
        if (!bbox.contains(p2))
            p2 = bbox.max;
        walk(p1, p2);
    }

    SECTION("max-edge horizontal") {
        walk(Point{bbox.min.x(), bbox.max.y()}, bbox.max);
    }

    SECTION("max-edge vertical") {
        walk(Point{bbox.max.x(), bbox.min.y()}, bbox.max);
    }
}

// Orca #16626: Danielsson propagation used `addr_delta << 1` on signed neighbour
// offsets (-1, -ncols). Left-shifting a negative signed integer is UB in C++17.
// Distances beyond the initialized edge band require that backward step.
TEST_CASE("EdgeGrid propagates signed distances across the interior and exterior", "[EdgeGrid][Regression]")
{
    static constexpr double kAbsTol    = 0.01;        // static: MSVC C++17 lambda use (C3493)
    static constexpr double kSigns[2]  = {-1., 1.};

    const Polygon rectangle{Point::new_scale(-10., -12.), Point::new_scale(10., -12.),
                            Point::new_scale(10., 12.), Point::new_scale(-10., 12.)};
    const Polygons outlines{rectangle};
    const BoundingBox bounds(Point::new_scale(-30., -32.), Point::new_scale(30., 32.));
    EdgeGrid::Grid grid(bounds);
    grid.create(outlines, coord_t(scale_(1.)));
    grid.calculate_sdf();
    const auto distance = [&](double x, double y) {
        return grid.signed_distance_bilinear(Point::new_scale(x, y)) * SCALING_FACTOR;
    };
    REQUIRE_THAT(distance(0., 0.), WithinAbs(-10., kAbsTol));
    for (double sign : kSigns) {
        REQUIRE_THAT(distance(sign * 5., 0.), WithinAbs(-5., kAbsTol));
        REQUIRE_THAT(distance(0., sign * 7.), WithinAbs(-5., kAbsTol));
        REQUIRE_THAT(distance(sign * 20., 0.), WithinAbs(10., kAbsTol));
        REQUIRE_THAT(distance(0., sign * 22.), WithinAbs(10., kAbsTol));
    }
}
