// Organic tree support: how far a branch is pushed off the part in one smoothing iteration
// (fix/tree-support-nudge).

#include <catch2/catch.hpp>

#include "libslic3r/Support/TreeSupport3D.hpp"

using namespace Slic3r;
using namespace Slic3r::TreeSupport3D;
using Catch::Matchers::WithinAbs;

// organic_smooth_branches_avoid_collisions() pushes a colliding sphere away from the part by the
// collision depth plus a 0.1 mm gap, at most 0.5 mm per iteration. The direction was already scaled
// to that distance and then scaled by it again: 0.3 mm became 0.09 mm, 0.1 mm 0.01 mm.
TEST_CASE("Organic smoothing pushes a branch off the part by the collision depth and the gap", "[TreeSupport][TreeSupportNudge]")
{
    const double gap = 0.1, max_nudge = 0.5;
    const Vec2d  position(10., 5.), collision(9., 5.);
    for (const double depth : { 0., 0.2, 0.35 }) {
        INFO("collision depth " << depth << " mm");
        const Vec2d moved = organic_collision_nudge(position, collision, depth, gap, max_nudge);
        CHECK_THAT(moved.x(), WithinAbs(10. + depth + gap, 1e-9));
        CHECK_THAT(moved.y(), WithinAbs(5., 1e-9));
    }
    // Capped at max_nudge per iteration, along the direction away from the collision.
    const Vec2d moved = organic_collision_nudge(position, Vec2d(7., 1.), 2., gap, max_nudge);
    CHECK_THAT((moved - position).norm(), WithinAbs(max_nudge, 1e-9));
    CHECK_THAT(moved.x() - position.x(), WithinAbs(0.6 * max_nudge, 1e-9));
    CHECK_THAT(moved.y() - position.y(), WithinAbs(0.8 * max_nudge, 1e-9));
    // A negative depth beyond the gap does not pull the branch towards the part.
    CHECK((organic_collision_nudge(position, collision, -1., gap, max_nudge) - position).norm() == 0.);
}
