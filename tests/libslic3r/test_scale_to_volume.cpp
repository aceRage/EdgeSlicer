#include <catch2/catch.hpp>

#include "libslic3r/Point.hpp"
#include "libslic3r/ScaleToVolume.hpp"

#include <cmath>

using namespace Slic3r;
using namespace Slic3r::scale_to_volume;

// A 256 x 256 x 256 bed, the shape most of these cases reason about.
static const Vec3d BED(256., 256., 256.);

static Settings uniform(double edge = 0., double top = 0., bool centre = true)
{
    Settings s;
    s.mode        = Mode::Uniform;
    s.edge_gap_mm = edge;
    s.top_gap_mm  = top;
    s.auto_center = centre;
    return s;
}

static Settings nonuniform(double edge = 0., double top = 0., bool centre = true)
{
    Settings s = uniform(edge, top, centre);
    s.mode     = Mode::NonUniform;
    return s;
}

TEST_CASE("scale_to_volume: usable size takes the edge gap off both sides and the top gap once", "[ScaleToVolume]")
{
    const Vec3d u = usable_size(BED, 5., 10.);
    REQUIRE(u.x() == Approx(246.));
    REQUIRE(u.y() == Approx(246.));
    // The object sits on the bed, so there is no bottom gap to subtract - only the top one.
    REQUIRE(u.z() == Approx(246.));
}

TEST_CASE("scale_to_volume: uniform picks the single most constrained axis", "[ScaleToVolume]")
{
    // Tall and thin: Z is the binding axis at 256/128 = 2, X would have allowed 8.
    const Vec3d f = factors(Vec3d(32., 32., 128.), BED, uniform());
    REQUIRE(f.x() == Approx(2.));
    REQUIRE(f.y() == Approx(2.));
    REQUIRE(f.z() == Approx(2.));
}

TEST_CASE("scale_to_volume: non-uniform gives each axis its own factor", "[ScaleToVolume]")
{
    const Vec3d f = factors(Vec3d(32., 64., 128.), BED, nonuniform());
    REQUIRE(f.x() == Approx(8.));
    REQUIRE(f.y() == Approx(4.));
    REQUIRE(f.z() == Approx(2.));
}

TEST_CASE("scale_to_volume: a non-uniform fit lands the box exactly on the gap-shrunk volume", "[ScaleToVolume]")
{
    const Vec3d box(40., 20., 10.);
    const Vec3d f      = factors(box, BED, nonuniform(5., 10.));
    const Vec3d fitted = box.cwiseProduct(f);
    REQUIRE(fitted.x() == Approx(246.)); // 256 - 2*5
    REQUIRE(fitted.y() == Approx(246.));
    REQUIRE(fitted.z() == Approx(246.)); // 256 - 10
}

TEST_CASE("scale_to_volume: a uniform fit stays inside the gap-shrunk volume on every axis", "[ScaleToVolume]")
{
    const Vec3d box(40., 20., 10.);
    const Vec3d f      = factors(box, BED, uniform(5., 10.));
    const Vec3d fitted = box.cwiseProduct(f);
    REQUIRE(fitted.x() <= Approx(246.));
    REQUIRE(fitted.y() <= Approx(246.));
    REQUIRE(fitted.z() <= Approx(246.));
    // ... and touches at least one of them, or it was not a fit.
    REQUIRE((fitted.x() == Approx(246.) || fitted.y() == Approx(246.) || fitted.z() == Approx(246.)));
    // Proportions preserved.
    REQUIRE(f.x() == Approx(f.y()));
    REQUIRE(f.y() == Approx(f.z()));
}

TEST_CASE("scale_to_volume: gaps that eat an axis are reported, not silently applied", "[ScaleToVolume]")
{
    REQUIRE_FALSE(target_degenerate(BED, 5., 10.));
    // 2 * 128 == the whole 256 mm width.
    REQUIRE(target_degenerate(BED, 128., 0.));
    REQUIRE(target_degenerate(BED, 130., 0.));
    REQUIRE(target_degenerate(BED, 0., 256.));
    REQUIRE(target_degenerate(BED, 0., 300.));

    // And the factor computation refuses rather than returning a negative scale.
    REQUIRE(factors(Vec3d(10., 10., 10.), BED, uniform(130., 0.)) == Vec3d::Zero());
}

TEST_CASE("scale_to_volume: a flat selection yields no factor rather than a division by zero", "[ScaleToVolume]")
{
    REQUIRE(factors(Vec3d(10., 10., 0.), BED, uniform()) == Vec3d::Zero());
    REQUIRE(factors(Vec3d(0., 10., 10.), BED, nonuniform()) == Vec3d::Zero());
}

TEST_CASE("scale_to_volume: clamp_translation leaves a box that already fits alone", "[ScaleToVolume]")
{
    const Vec2d d = clamp_translation(Vec2d(100., 100.), Vec2d(150., 150.),
                                      Vec2d(0., 0.), Vec2d(256., 256.), 0.);
    REQUIRE(d.x() == Approx(0.));
    REQUIRE(d.y() == Approx(0.));
}

TEST_CASE("scale_to_volume: clamp_translation moves only the violated axis, and only as far as needed", "[ScaleToVolume]")
{
    // Hangs 10 mm off the max-X edge; Y is comfortably inside.
    const Vec2d d = clamp_translation(Vec2d(200., 100.), Vec2d(266., 150.),
                                      Vec2d(0., 0.), Vec2d(256., 256.), 0.);
    REQUIRE(d.x() == Approx(-10.));
    REQUIRE(d.y() == Approx(0.));

    // Hangs off the min-Y edge instead.
    const Vec2d d2 = clamp_translation(Vec2d(100., -7.), Vec2d(150., 40.),
                                       Vec2d(0., 0.), Vec2d(256., 256.), 0.);
    REQUIRE(d2.x() == Approx(0.));
    REQUIRE(d2.y() == Approx(7.));
}

TEST_CASE("scale_to_volume: clamp_translation honours the edge gap", "[ScaleToVolume]")
{
    // Flush with the max-X bed edge, but a 5 mm gap is asked for.
    const Vec2d d = clamp_translation(Vec2d(200., 100.), Vec2d(256., 150.),
                                      Vec2d(0., 0.), Vec2d(256., 256.), 5.);
    REQUIRE(d.x() == Approx(-5.));
    REQUIRE(d.y() == Approx(0.));
}

TEST_CASE("scale_to_volume: a box too wide to fit ends up flush with the min edge", "[ScaleToVolume]")
{
    // 300 mm wide on a 256 mm bed: no translation makes it fit, so it is pinned to the min side
    // rather than oscillating between the two clamps.
    const Vec2d d = clamp_translation(Vec2d(-20., 100.), Vec2d(280., 150.),
                                      Vec2d(0., 0.), Vec2d(256., 256.), 0.);
    REQUIRE(d.x() == Approx(20.));
}

TEST_CASE("scale_to_volume: circular clamp only moves a box that leaves the bed circle", "[ScaleToVolume]")
{
    const Vec2d bed_centre(0., 0.);
    // Well inside: no move.
    REQUIRE(clamp_translation_circle(Vec2d(10., 0.), 5., bed_centre, 100., 0.).norm() == Approx(0.));

    // Centre 90 from the bed centre with a radius of 20 pokes 10 mm out of a 100 mm bed.
    const Vec2d d = clamp_translation_circle(Vec2d(90., 0.), 20., bed_centre, 100., 0.);
    REQUIRE(d.x() == Approx(-10.));
    REQUIRE(d.y() == Approx(0.));

    // The move is along the centre-to-centre vector, so a diagonal offset moves diagonally.
    const double k  = std::sqrt(0.5);
    const Vec2d  d2 = clamp_translation_circle(Vec2d(90. * k, 90. * k), 20., bed_centre, 100., 0.);
    REQUIRE(d2.x() == Approx(-10. * k));
    REQUIRE(d2.y() == Approx(-10. * k));
}

TEST_CASE("scale_to_volume: circular clamp gives up rather than thrash when the box cannot fit", "[ScaleToVolume]")
{
    REQUIRE(clamp_translation_circle(Vec2d(50., 0.), 200., Vec2d(0., 0.), 100., 0.).norm() == Approx(0.));
}

TEST_CASE("scale_to_volume: the circular uniform factor honours both gaps", "[ScaleToVolume]")
{
    // Radius 10 selection, 50 tall, on a 100 mm radius / 200 mm tall bed.
    // XY allows 100/10 = 10; Z allows 200/50 = 4 - Z binds.
    REQUIRE(circle_factor(10., 50., 100., 200., 0., 0.) == Approx(4.));
    // A 100 mm top gap halves the usable height, so Z now allows 2.
    REQUIRE(circle_factor(10., 50., 100., 200., 0., 100.) == Approx(2.));
    // An 80 mm edge gap leaves radius 20, so XY allows only 2 and now binds.
    REQUIRE(circle_factor(10., 50., 100., 200., 80., 0.) == Approx(2.));
    // Gaps that eat the target are refused.
    REQUIRE(circle_factor(10., 50., 100., 200., 100., 0.) == Approx(0.));
    REQUIRE(circle_factor(10., 50., 100., 200., 0., 200.) == Approx(0.));
    REQUIRE(circle_factor(0., 50., 100., 200., 0., 0.) == Approx(0.));
}

TEST_CASE("scale_to_volume: the non-uniform circular target is the inscribed square", "[ScaleToVolume]")
{
    const Vec3d v = inscribed_square_volume_size(100., 200., 0.);
    REQUIRE(v.x() == Approx(100. * std::sqrt(2.)));
    REQUIRE(v.y() == Approx(100. * std::sqrt(2.)));
    REQUIRE(v.z() == Approx(200.));

    // A square of that side really does fit inside the circle: its half-diagonal is the radius.
    const double half_diag = 0.5 * Vec2d(v.x(), v.y()).norm();
    REQUIRE(half_diag == Approx(100.));
}

TEST_CASE("scale_to_volume: the inscribed-square target does not double-count the edge gap", "[ScaleToVolume]")
{
    // inscribed_square_volume_size() shrinks the radius by the gap itself, and then hands back a
    // size that usable_size() will shrink by the same gap again - so the two must cancel.
    const double gap = 10.;
    const Vec3d  v   = inscribed_square_volume_size(100., 200., gap);
    const Vec3d  u   = usable_size(v, gap, 0.);
    REQUIRE(u.x() == Approx(90. * std::sqrt(2.)));
    REQUIRE(u.y() == Approx(90. * std::sqrt(2.)));

    // And that square fits inside the gap-shrunk circle exactly.
    REQUIRE(0.5 * Vec2d(u.x(), u.y()).norm() == Approx(90.));
}
