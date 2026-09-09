#include <catch2/catch.hpp>

#include <cmath>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// A Bambu P1S project that reproduced the off-plate brim: two PLAs priming 30 and 45 mm3 in
// separate adhesiveness categories on a 35 mm tower, 0.21 mm layers, 0.4 nozzle (0.5 mm lines),
// 150 % infill gap (0.75 mm line pitch), rib width 8, 16 mm tall.
static std::vector<WipeTower::PurgeEstimate> cube_purges(int first_category = 100)
{
    return {{30.f, first_category}, {45.f, 0}};
}

TEST_CASE("Cone base polygon bulges past the body box", "[WipeTower]") {
    const Polygon box = WipeTower2::cone_base_polygon(35., 20., 100., 0.);
    CHECK(box.points.size() == 4);
    CHECK(get_extents(box).size() == Point::new_scale(Vec2d(35., 20.)));
    const Polygon     base = WipeTower2::cone_base_polygon(35., 20., 100., 25.);
    const BoundingBox bb   = get_extents(base);
    const double      R    = std::tan(25. / 2. * M_PI / 180.) * 100.;
    CHECK_THAT(unscaled(bb.min.y()), WithinAbs(10. - R, 0.1));
    CHECK_THAT(unscaled(bb.max.y()), WithinAbs(10. + R, 0.1));
    CHECK(diff(Polygons{box}, Polygons{base}).empty());
}

TEST_CASE("Type1 block-stack depth quantizes each purge to whole lines", "[WipeTower]") {
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(cube_purges(), 35.f, 0.21f, 0.4f, 1.5f), WithinAbs(18.5f, 0.01f));
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(cube_purges(0), 35.f, 0.21f, 0.4f, 1.5f), WithinAbs(11.0f, 0.01f));
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth({}, 35.f, 0.2f, 0.4f, 1.f), WithinAbs(0.f, 1e-6f));
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth({{45.f, 0}}, 0.9f, 0.2f, 0.4f, 1.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("A nozzle change adds its ramming lines to the block", "[WipeTower]") {
    std::vector<WipeTower::PurgeEstimate> purges{{100.f, 0}, {100.f, 0}};
    const float without_change = WipeTower::estimate_tower_blocks_depth(purges, 50.f, 0.2f, 0.4f, 1.f);
    purges.front().filament_change_length = 10.f;
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(purges, 50.f, 0.2f, 0.4f, 1.f) - without_change, WithinAbs(3.f, 1e-4f));
}

TEST_CASE("Rib tower footprint estimate covers the generated footprint", "[WipeTower]") {
    const float side = WipeTower::estimate_rib_tower_bbox_side(cube_purges(), 35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f);
    CHECK(side >= 29.56f);
    CHECK(side <= 29.56f + 4.f);
    CHECK(side >= WipeTower::estimate_rib_tower_bbox_side(cube_purges(0), 35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f));
    CHECK_THAT(WipeTower::estimate_rib_tower_bbox_side({}, 35.f, 0.2f, 0.4f, 1.f, 8.f, 0.f, 16.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("Rib footprint extends the ribs, not the body, below the stability minimum", "[WipeTower]") {
    const float min_depth = WipeTower::get_limit_depth_by_height(90.f);
    REQUIRE(min_depth > 10.f);
    CHECK_THAT(WipeTower::rib_footprint_side(10.f, 10.f, 8.f, 0.f, 90.f), WithinAbs(min_depth + 5.f / std::sqrt(2.f), 1e-4f));
    const float plain = WipeTower::rib_footprint_side(30.f, 30.f, 8.f, 0.f, 5.f);
    CHECK_THAT(plain, WithinAbs(30.f + 8.f / std::sqrt(2.f), 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(30.f, 30.f, 8.f, 4.f, 5.f) - plain, WithinAbs(4.f / std::sqrt(2.f), 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(30.f, 30.f, 8.f, -4.f, 5.f), WithinAbs(plain, 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(0.f, 30.f, 8.f, 0.f, 5.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("Brim width estimate matches each generator's loop quantization", "[WipeTower]") {
    const float spacing = 0.5f - 0.2f * float(1. - M_PI_4);
    CHECK_THAT(WipeTower::estimate_brim_real_width(3.f, 0.4f, 0.2f, true), WithinAbs(7.f * spacing, 1e-4f));
    CHECK_THAT(WipeTower::estimate_brim_real_width(3.f, 0.4f, 0.2f, false), WithinAbs(7.5f * spacing, 1e-4f));
    CHECK_THAT(WipeTower::estimate_brim_real_width(0.f, 0.4f, 0.2f, true), WithinAbs(0.f, 1e-6f));
}
