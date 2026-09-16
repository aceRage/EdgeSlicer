#include <catch2/catch.hpp>

#include <libslic3r/DrawCut.hpp>
#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/AABBMesh.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <limits>
#include <cstdlib>
#include <set>
#include <tuple>

using namespace Slic3r;

// The test article, the same one the [CurvedCut] suite uses: a 40 mm cube centred
// on the origin, so the cut plane frame (z == 0) runs through its middle and the
// top face sits at z == +20.
static const double CUBE = 40.0;

static indexed_triangle_set centred_cube(double s = CUBE)
{
    indexed_triangle_set its = its_make_cube(s, s, s);
    for (Vec3f& v : its.vertices)
        v -= Vec3f(float(0.5 * s), float(0.5 * s), float(0.5 * s));
    return its;
}

// A closed circular stroke on the cube's TOP face: every sample sits at
// z == +20 with the face's own +Z normal, which is what the raycast would have
// produced had a user dragged a loop there.
static DrawCutStroke circle_on_top(double radius, int n = 64, double z = 0.5 * CUBE, bool clockwise = false)
{
    DrawCutStroke stroke;
    for (int i = 0; i < n; ++ i) {
        const double a = (clockwise ? -1.0 : 1.0) * 2.0 * M_PI * double(i) / double(n);
        stroke.append(Vec3d(radius * std::cos(a), radius * std::sin(a), z), Vec3d::UnitZ(), size_t(i));
    }
    // Close the ring by coming back to (nearly) the start, the way a hand-drawn
    // loop does - finish() decides closed from the gap, so the test exercises
    // that decision rather than asserting past it.
    stroke.append(Vec3d(radius, 0.0, z), Vec3d::UnitZ(), 0);
    return stroke;
}

// An open straight stroke across the cube's top face, along +X, reaching from
// -half to +half of the span given.
static DrawCutStroke line_on_top(double half_span, int n = 40, double z = 0.5 * CUBE)
{
    DrawCutStroke stroke;
    for (int i = 0; i < n; ++ i) {
        const double t = double(i) / double(n - 1);
        stroke.append(Vec3d(-half_span + 2.0 * half_span * t, 0.0, z), Vec3d::UnitZ(), size_t(i));
    }
    return stroke;
}

static bool watertight(const indexed_triangle_set& its)
{
    return !its.empty() && its_num_open_edges(its) == 0;
}

// ---------------------------------------------------------------------------
// (1) The resampler. There is no 3D resampler in this codebase, so this is the
// new code's own contract: equal spacing, arc length preserved, and the LAST
// point actually emitted (the wart equally_spaced_points has and this must not).
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: the resampler spaces evenly and keeps the endpoint", "[DrawCut]")
{
    // A synthetic stroke with deliberately uneven input spacing: a helix sampled
    // at a varying rate, so a resampler that just copied points would fail.
    std::vector<DrawCutSample> in;
    double raw_len = 0.0;
    Vec3d  prev    = Vec3d::Zero();
    for (int i = 0; i <= 200; ++ i) {
        const double t = double(i) / 200.0;
        // Uneven parameterisation: t^1.5 bunches the samples at the start.
        const double s = std::pow(t, 1.5) * 4.0 * M_PI;
        DrawCutSample smp;
        smp.pos    = Vec3d(10.0 * std::cos(s), 10.0 * std::sin(s), 3.0 * s);
        smp.normal = Vec3d(std::cos(s), std::sin(s), 0.0);
        in.push_back(smp);
        if (i > 0)
            raw_len += (smp.pos - prev).norm();
        prev = smp.pos;
    }

    const double spacing = 1.0;
    const std::vector<DrawCutSample> out = draw_cut_resample(in, spacing, /*closed*/ false);

    REQUIRE(out.size() > 10);

    // Every interior span is `spacing` long. The samples sit at exact multiples of
    // spacing in ARC LENGTH along the source polyline, so a span that straddles a
    // source vertex is a chord slightly shorter than its arc - which is a property
    // of the input's own faceting, not a drift: it does not accumulate, and it
    // bounds at the sag of one input span. On a helix sampled 200 times that is
    // under 0.1% of a millimetre span, and it must stay there.
    for (size_t i = 1; i + 1 < out.size(); ++ i) {
        const double d = (out[i].pos - out[i - 1].pos).norm();
        REQUIRE(d <= spacing + 1e-9);        // never LONGER than the arc
        REQUIRE(std::abs(d - spacing) < 1e-3);
    }
    const double last = (out.back().pos - out[out.size() - 2].pos).norm();
    REQUIRE(last > 0.5 * spacing - 1e-9);
    REQUIRE(last < spacing + 1e-6);

    // The first input sample survives exactly, and the END OF THE STROKE IS
    // REACHED - the wart this test exists for. equally_spaced_points emits the
    // first point but not reliably the last, so a stroke resampled through it can
    // stop up to a full spacing short of where the user let go. Here the last
    // output sample is either the input endpoint itself or within half a spacing of
    // it (closer than that and appending it would leave a sliver span the
    // central-difference tangent would read as a near-zero direction).
    REQUIRE((out.front().pos - in.front().pos).norm() < 1e-12);
    REQUIRE((out.back().pos - in.back().pos).norm() <= 0.5 * spacing + 1e-9);

    // Arc length is preserved to better than 0.5%: resampling a curve at 1 mm
    // chords loses a little to the chord sag, and this pins how little.
    double out_len = 0.0;
    for (size_t i = 1; i < out.size(); ++ i)
        out_len += (out[i].pos - out[i - 1].pos).norm();
    REQUIRE(std::abs(out_len - raw_len) / raw_len < 0.005);

    // Normals come back unit length, interpolated rather than copied.
    for (const DrawCutSample& s : out)
        REQUIRE(std::abs(s.normal.norm() - 1.0) < 1e-9);
}

TEST_CASE("Draw cut: a resampled ring stays open and wraps", "[DrawCut]")
{
    // A circle of radius 10: circumference 62.83 mm, so at 1 mm spacing the ring
    // holds about 63 samples and the closing span is resampled too.
    std::vector<DrawCutSample> in;
    const int n = 128;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        DrawCutSample s;
        s.pos    = Vec3d(10.0 * std::cos(a), 10.0 * std::sin(a), 0.0);
        s.normal = Vec3d::UnitZ();
        in.push_back(s);
    }

    const std::vector<DrawCutSample> out = draw_cut_resample(in, 1.0, /*closed*/ true);

    // The output must NOT duplicate the first sample at the end - the strip
    // builder wraps, so a duplicate would give it a zero-length span.
    REQUIRE((out.back().pos - out.front().pos).norm() > 0.5);
    // ... and the wrap span must itself be about one spacing, i.e. the ring is
    // evenly covered all the way round.
    const double wrap = (out.front().pos - out.back().pos).norm();
    REQUIRE(wrap > 0.5);
    REQUIRE(wrap < 1.6);
    // Circumference within 0.5%.
    double len = 0.0;
    for (size_t i = 1; i < out.size(); ++ i)
        len += (out[i].pos - out[i - 1].pos).norm();
    len += wrap;
    REQUIRE(std::abs(len - 2.0 * M_PI * 10.0) / (2.0 * M_PI * 10.0) < 0.005);
}

TEST_CASE("Draw cut: the closing tolerance decides open from closed", "[DrawCut]")
{
    // max(3 * spacing, 2 mm).
    REQUIRE(draw_cut_closing_tolerance(1.0) == Approx(3.0));
    REQUIRE(draw_cut_closing_tolerance(0.1) == Approx(2.0));  // the 2 mm floor
    REQUIRE(draw_cut_closing_tolerance(2.0) == Approx(6.0));

    // A loop whose ends are 1 mm apart closes at spacing 1 (tolerance 3 mm)...
    DrawCutStroke nearly;
    const int n = 60;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * 0.99 * double(i) / double(n - 1);
        nearly.append(Vec3d(10.0 * std::cos(a), 10.0 * std::sin(a), 0.0), Vec3d::UnitZ());
    }
    const double gap = (nearly.samples().back().pos - nearly.samples().front().pos).norm();
    REQUIRE(gap < 3.0);
    REQUIRE(nearly.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(nearly.is_closed());

    // ... and an arc whose ends are far apart does not.
    DrawCutStroke arc;
    for (int i = 0; i < 40; ++ i) {
        const double a = M_PI * double(i) / 39.0; // half a circle
        arc.append(Vec3d(10.0 * std::cos(a), 10.0 * std::sin(a), 0.0), Vec3d::UnitZ());
    }
    REQUIRE(arc.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!arc.is_closed());

    // Force closed takes an open stroke and rings it anyway.
    REQUIRE(arc.finish(1.0, 0.0, /*force_closed*/ true) == DrawCutError::None);
    REQUIRE(arc.is_closed());
}

// ---------------------------------------------------------------------------
// (2) Smoothing.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: smoothing settles and is bounded", "[DrawCut]")
{
    // A zigzag: a straight line with alternating 1 mm spikes, which is the shape
    // a raw stroke over a faceted surface actually has.
    auto make = []() {
        std::vector<DrawCutSample> p;
        for (int i = 0; i < 41; ++ i) {
            DrawCutSample s;
            s.pos    = Vec3d(double(i), (i % 2 == 0 ? 1.0 : -1.0), 0.0);
            s.normal = Vec3d::UnitZ();
            p.push_back(s);
        }
        return p;
    };
    const std::vector<DrawCutSample> raw = make();

    std::vector<DrawCutSample> a = make();
    draw_cut_smooth(a, 2, /*closed*/ false);
    std::vector<DrawCutSample> b = a;
    draw_cut_smooth(b, 2, /*closed*/ false);

    // NO SAMPLE MOVES FURTHER THAN THE ZIGZAG'S OWN AMPLITUDE. A smoothing pass
    // is an average of neighbours, so it can never push a sample outside the
    // convex hull of its neighbourhood - which for this input is |y| <= 1.
    double max_move_first = 0.0, max_move_second = 0.0;
    for (size_t i = 0; i < raw.size(); ++ i) {
        max_move_first  = std::max(max_move_first,  (a[i].pos - raw[i].pos).norm());
        max_move_second = std::max(max_move_second, (b[i].pos - a[i].pos).norm());
        REQUIRE(std::abs(a[i].pos.y()) <= 1.0 + 1e-12);
        REQUIRE(std::abs(b[i].pos.y()) <= 1.0 + 1e-12);
    }
    REQUIRE(max_move_first > 1e-6);            // it did something
    REQUIRE(max_move_second < max_move_first); // and the second N passes did less

    // Endpoints are HELD for an open stroke: the ends are where the user decided
    // the cut should reach, and letting the average creep them inwards would
    // shorten the stroke on every pass.
    REQUIRE((a.front().pos - raw.front().pos).norm() < 1e-15);
    REQUIRE((a.back().pos  - raw.back().pos).norm()  < 1e-15);

    // Zero passes is a no-op, and the panel's 0..1 maps onto 0..10 passes.
    std::vector<DrawCutSample> z = make();
    draw_cut_smooth(z, 0, false);
    for (size_t i = 0; i < raw.size(); ++ i)
        REQUIRE((z[i].pos - raw[i].pos).norm() < 1e-15);
    REQUIRE(draw_cut_smooth_passes(0.0) == 0);
    REQUIRE(draw_cut_smooth_passes(1.0) == DrawCutStroke::MaxSmoothPasses);
    REQUIRE(draw_cut_smooth_passes(0.2) == 2);

    // A closed path wraps, so its "first" sample moves too.
    std::vector<DrawCutSample> c = make();
    draw_cut_smooth(c, 2, /*closed*/ true);
    REQUIRE((c.front().pos - raw.front().pos).norm() > 1e-9);

    // Normals stay unit length through every pass.
    for (const DrawCutSample& s : b)
        REQUIRE(std::abs(s.normal.norm() - 1.0) < 1e-9);
}

// ---------------------------------------------------------------------------
// (3) The gates: too short, self-crossing, leaving the mesh.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: a stroke that is too short is refused with a clear error", "[DrawCut]")
{
    // One sample is not a stroke.
    DrawCutStroke one;
    one.append(Vec3d::Zero(), Vec3d::UnitZ());
    REQUIRE(one.finish() == DrawCutError::TooShort);
    REQUIRE(!one.valid());
    REQUIRE(std::string(draw_cut_error_message(DrawCutError::TooShort)).find("longer") != std::string::npos);

    // A 1 mm dab is under MinLength (2 mm), so it is refused even though it has
    // samples to spare.
    DrawCutStroke dab;
    for (int i = 0; i < 10; ++ i)
        dab.append(Vec3d(0.1 * double(i), 0.0, 0.0), Vec3d::UnitZ());
    REQUIRE(dab.finish(0.1, 0.0) == DrawCutError::TooShort);
    REQUIRE(!dab.valid());

    // And the split refuses it rather than producing something.
    const indexed_triangle_set cube = centred_cube();
    DrawCutParams params;
    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(!draw_cut_split(cube, dab, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::TooShort);
}

TEST_CASE("Draw cut: a stroke that leaves the mesh keeps its longest run", "[DrawCut]")
{
    // Capture skips misses, so a stroke that crossed a hole comes back as two
    // runs with a wide gap between them. Bridging it would run a chord through
    // air, so the longest run is kept - here, the 30 mm one.
    DrawCutStroke s;
    for (int i = 0; i < 31; ++ i)
        s.append(Vec3d(double(i), 0.0, 0.0), Vec3d::UnitZ());
    // ... then a 500 mm jump and a short tail.
    for (int i = 0; i < 5; ++ i)
        s.append(Vec3d(500.0 + double(i), 0.0, 0.0), Vec3d::UnitZ());

    REQUIRE(s.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(s.valid());
    // The kept run is the 30 mm one, not the 534 mm span across the gap.
    REQUIRE(s.length() == Approx(30.0).margin(1.0));
    for (const DrawCutSample& smp : s.path())
        REQUIRE(smp.pos.x() < 100.0);

    // And when the longest surviving run is itself too short, the reported reason is
    // LeavesMesh, not TooShort - the user's line WAS long enough, it just was not all
    // on the part, so "draw a longer line" would be the wrong advice.
    DrawCutStroke tiny;
    tiny.append(Vec3d(0.0, 0.0, 0.0), Vec3d::UnitZ());
    tiny.append(Vec3d(0.5, 0.0, 0.0), Vec3d::UnitZ());
    tiny.append(Vec3d(900.0, 0.0, 0.0), Vec3d::UnitZ());
    tiny.append(Vec3d(900.5, 0.0, 0.0), Vec3d::UnitZ());
    REQUIRE(tiny.finish(1.0, 0.0) == DrawCutError::LeavesMesh);
    REQUIRE(std::string(draw_cut_error_message(DrawCutError::LeavesMesh)).find("leaves") != std::string::npos);
}

TEST_CASE("Draw cut: a self-crossing stroke is detected and refused", "[DrawCut]")
{
    // A figure-of-eight in the plane z == 20: a lemniscate crosses itself once at
    // the origin, which is exactly the "no well-defined interior" case phase 1
    // refuses rather than guessing at.
    //
    // THE PHASE OFFSET MATTERS. The plain lemniscate x = sin t, y = sin t cos t
    // STARTS at the crossing point, so the crossing is the stroke's own closure -
    // which the test deliberately does not count, because a polyline's first and
    // last segments always share an endpoint. Starting a quarter turn along puts
    // the crossing in the middle of the stroke, where it is a real crossing of two
    // non-adjacent segments and where a hand-drawn figure-of-eight would put it.
    DrawCutStroke eight;
    const int n = 160;
    for (int i = 0; i <= n; ++ i) {
        const double t = 0.5 * M_PI + 2.0 * M_PI * double(i) / double(n);
        eight.append(Vec3d(12.0 * std::sin(t), 12.0 * std::sin(t) * std::cos(t), 0.5 * CUBE), Vec3d::UnitZ());
    }

    REQUIRE(eight.finish(1.0, 0.0) == DrawCutError::SelfCrossing);
    REQUIRE(!eight.valid());
    REQUIRE(std::string(draw_cut_error_message(DrawCutError::SelfCrossing)).find("crosses itself") != std::string::npos);

    const indexed_triangle_set cube = centred_cube();
    DrawCutParams params;
    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(!draw_cut_split(cube, eight, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::SelfCrossing);

    // A plain circle is NOT self-crossing - the negative control that proves the
    // test is not just always true.
    DrawCutStroke circle = circle_on_top(12.0);
    REQUIRE(circle.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_self_crossing(circle));
}

TEST_CASE("Draw cut: a tight concave corner folds the ruled strip", "[DrawCut]")
{
    // WHICH WAY A CORNER HAS TO TURN TO FOLD. The outward rail is the stroke pushed
    // out by E along the outward binormal, so it folds only where the stroke's turn
    // centre is on the OUTWARD side - a CONCAVE corner. At a convex corner the
    // offset moves away from the centre and the rail merely gets longer, which is
    // why a circle drawn as a loop never folds however tight it is. Both halves of
    // that are checked here, because getting the sign backwards would have made
    // every loop look like a fold.

    // (a) A closed loop with a deep concave notch: a 5-lobed "flower" whose inner
    // radius turns tightly the wrong way.
    DrawCutStroke flower;
    const int n = 240;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        const double r = 14.0 + 5.0 * std::cos(5.0 * a); // 9 .. 19 mm
        flower.append(Vec3d(r * std::cos(a), r * std::sin(a), 0.5 * CUBE), Vec3d::UnitZ());
    }
    flower.append(Vec3d(19.0, 0.0, 0.5 * CUBE), Vec3d::UnitZ());
    REQUIRE(flower.finish(0.5, 0.0) == DrawCutError::None);
    REQUIRE(flower.is_closed());

    double kappa = 0.0;
    // The notches turn with a radius of a couple of mm, so a 5 mm extension folds
    // them and a 0.2 mm one does not.
    REQUIRE(draw_cut_strip_folds(flower, 5.0, &kappa));
    REQUIRE(kappa > 1.0 / 5.0);
    REQUIRE(!draw_cut_strip_folds(flower, 0.2, nullptr));

    // (b) A PLAIN CIRCLE NEVER FOLDS, at any extension - the negative control that
    // proves the sign is the right way round. A 3 mm circle has curvature 1/3 per
    // mm, so a naive "E * kappa > 1" without the sign test would have called this
    // a fold at any E above 3 mm.
    DrawCutStroke tight = circle_on_top(3.0, 48);
    REQUIRE(tight.finish(0.5, 0.0) == DrawCutError::None);
    REQUIRE(tight.is_closed());
    REQUIRE(!draw_cut_strip_folds(tight, 5.0, nullptr));
    REQUIRE(!draw_cut_strip_folds(tight, 50.0, nullptr));

    // (c) An OPEN stroke that WAVES: its binormal is parallel-transported along the
    // stroke rather than set by a winding, so it keeps one side, and a wave turns
    // toward that side on every other crest. A tight wave therefore folds and a
    // gentle one does not - which is the same property as (a) and (b), measured
    // where there is no interior to orient against.
    auto wave = [](double amp, double period) {
        DrawCutStroke w;
        for (int i = 0; i <= 200; ++ i) {
            const double x = -20.0 + 40.0 * double(i) / 200.0;
            w.append(Vec3d(x, amp * std::sin(2.0 * M_PI * x / period), 0.5 * CUBE), Vec3d::UnitZ());
        }
        return w;
    };

    DrawCutStroke tight_wave = wave(3.0, 8.0); // radius of curvature ~ 1 mm at a crest
    REQUIRE(tight_wave.finish(0.5, 0.0) == DrawCutError::None);
    REQUIRE(!tight_wave.is_closed());
    double k2 = 0.0;
    REQUIRE(draw_cut_strip_folds(tight_wave, 5.0, &k2));
    REQUIRE(k2 > 1.0 / 5.0);

    DrawCutStroke gentle_wave = wave(1.0, 60.0); // nearly straight
    REQUIRE(gentle_wave.finish(0.5, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_strip_folds(gentle_wave, 5.0, nullptr));
}

// ---------------------------------------------------------------------------
// (4) The headline case: a closed circle on a cube's top face, Direction =
// Surface normal, through all, gives a cylindrical plug.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: a closed circle on a cube's top face cuts a cylindrical plug", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));

    const double R = 12.0;
    DrawCutStroke stroke = circle_on_top(R, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());
    REQUIRE(stroke.valid());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 5.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);

    // Two parts, both watertight.
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // `upper` is the PLUG - the piece the stroke drew around. On a cube's flat top
    // face the inward normal is -Z everywhere, so the plug is a cylinder of
    // radius R through the full 40 mm height.
    const double expect_plug = M_PI * R * R * CUBE;
    const double plug        = double(its_volume(upper));
    REQUIRE(plug == Approx(expect_plug).epsilon(0.05)); // the spec's +/- 5%

    // Volume is conserved: with no kerf the two halves partition the cube.
    const double rest = double(its_volume(lower));
    REQUIRE(plug + rest == Approx(cube_volume).epsilon(1e-3));

    // The plug's cross-section is a circle of radius R at several heights: the
    // widest |xy| of any vertex is R, and the plug spans the cube's full height.
    BoundingBoxf3 bb;
    for (const Vec3f& v : upper.vertices)
        bb.merge(v.cast<double>());
    REQUIRE(bb.min.z() == Approx(-0.5 * CUBE).margin(1e-3));
    REQUIRE(bb.max.z() == Approx(+0.5 * CUBE).margin(1e-3));
    // A 1 mm resampled circle is a polygon inscribed in the circle, so its
    // circumradius is R and its inradius is R * cos(pi/n) - a hair under. Both
    // stay within a chord of R.
    double max_r = 0.0;
    for (const Vec3f& v : upper.vertices)
        max_r = std::max(max_r, v.cast<double>().head<2>().norm());
    REQUIRE(max_r == Approx(R).margin(0.2));
}

TEST_CASE("Draw cut: the plug does not depend on which way the loop was drawn", "[DrawCut]")
{
    // WINDING DECIDES. A loop drawn clockwise and one drawn counter-clockwise must
    // give the SAME plug - the user should not have to care about drag direction,
    // which is what the binormal sign flip in compute_binormals() is for.
    const indexed_triangle_set cube = centred_cube();

    auto run = [&](bool clockwise) {
        DrawCutStroke s = circle_on_top(10.0, 96, 0.5 * CUBE, clockwise);
        REQUIRE(s.finish(1.0, 0.0) == DrawCutError::None);
        REQUIRE(s.is_closed());
        DrawCutParams params;
        params.extension = 5.0;
        // PHASE 3: on a FLAT face a lip of 0 is a flat shelf lying along the face, so
        // the band never enters the material and there is no plug to compare. The
        // winding question this case asks is about the cut surface, not about the
        // angle, so give it an angle that actually cuts - 90, the straight-walled
        // plug the ruled strip used to make here.
        params.angle_deg = 90.0;
        params.depth     = 6.0;
        indexed_triangle_set up, lo;
        REQUIRE(draw_cut_split(cube, s, params, &up, &lo, nullptr));
        return std::make_pair(up, lo);
    };

    const auto ccw = run(false);
    const auto cw  = run(true);

    // Same plug volume, same plug bounding box - to the accuracy the resampled
    // POLYGON itself has, not bit-for-bit. A 1 mm resample of a 62.8 mm circle does
    // not divide evenly, so traversing it the other way lands the samples at a
    // different phase and the inscribed polygon differs by a fraction of the chord
    // sag. What must not differ is which piece comes back as the plug, or how big
    // it is, which is the actual claim: winding decides, drag direction does not.
    REQUIRE(double(its_volume(ccw.first)) == Approx(double(its_volume(cw.first))).epsilon(1e-3));
    REQUIRE(double(its_volume(ccw.second)) == Approx(double(its_volume(cw.second))).epsilon(1e-3));

    BoundingBoxf3 bb_ccw, bb_cw;
    for (const Vec3f& v : ccw.first.vertices) bb_ccw.merge(v.cast<double>());
    for (const Vec3f& v : cw.first.vertices)  bb_cw.merge(v.cast<double>());
    REQUIRE((bb_ccw.min - bb_cw.min).cwiseAbs().maxCoeff() < 0.05);
    REQUIRE((bb_ccw.max - bb_cw.max).cwiseAbs().maxCoeff() < 0.05);

    // And both really are the plug (much smaller than the cube), not the
    // remainder - i.e. the swap in draw_cut_split() went the right way.
    REQUIRE(double(its_volume(ccw.first)) < 0.5 * double(its_volume(cube)));
}

// ---------------------------------------------------------------------------
// (5) An open stroke across a box splits it in two.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: an open line across a box splits it in two", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));

    // A line along +X across the full 40 mm span, with Extension 5 so the strip
    // reaches past both silhouettes.
    DrawCutStroke stroke = line_on_top(0.5 * CUBE);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!stroke.is_closed());
    REQUIRE(stroke.valid());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 5.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);

    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // The volumes sum to the original - the whole point of a split with no kerf.
    const double vu = double(its_volume(upper));
    const double vl = double(its_volume(lower));
    REQUIRE(vu + vl == Approx(cube_volume).epsilon(1e-3));

    // The line runs down the middle, so both halves are about half the cube.
    REQUIRE(vu == Approx(0.5 * cube_volume).epsilon(0.02));
    REQUIRE(vl == Approx(0.5 * cube_volume).epsilon(0.02));

    // And they lie on opposite sides of y == 0, which is where the stroke was.
    BoundingBoxf3 bu, bl;
    for (const Vec3f& v : upper.vertices) bu.merge(v.cast<double>());
    for (const Vec3f& v : lower.vertices) bl.merge(v.cast<double>());
    REQUIRE(std::abs(bu.center().y() - bl.center().y()) > 5.0);
}

TEST_CASE("Draw cut: a line that does not reach across leaves a side empty", "[DrawCut]")
{
    // THE NEGATIVE CONTROL the spec asks for: the same line, too short, with no
    // Extension. The strip does not reach the silhouettes, so it cannot separate
    // the part and the empty-side test says so BEFORE two booleans run.
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke = line_on_top(8.0); // a 16 mm line across a 40 mm cube
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!stroke.is_closed());

    DrawCutParams params;
    params.extension   = 0.0;
    params.through_all = true;

    bool upper_empty = false, lower_empty = false;
    draw_cut_empty_sides(cube, stroke, params, upper_empty, lower_empty);
    // One side has nothing: the strip is entirely inside the part, so "inside the
    // cutter" holds no vertex of the cube at all.
    REQUIRE((upper_empty || lower_empty));

    // The same line WITH enough extension does separate it - which is what the
    // panel's "increase Extension" advice is for.
    DrawCutStroke across = line_on_top(0.5 * CUBE);
    REQUIRE(across.finish(1.0, 0.0) == DrawCutError::None);
    DrawCutParams ok_params;
    ok_params.extension = 5.0;
    bool ue = true, le = true;
    draw_cut_empty_sides(cube, across, ok_params, ue, le);
    REQUIRE(!ue);
    REQUIRE(!le);
}

// ---------------------------------------------------------------------------
// (6) The kerf.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: a thickness leaves a gap of that width", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));

    // A straight line across the cube, cut with a 1 mm kerf: the band between the
    // two halves belongs to neither, so the two volumes fall short of the cube by
    // the band's own volume - 1 mm x 40 mm x 40 mm.
    DrawCutStroke stroke = line_on_top(0.5 * CUBE);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams p0;
    p0.extension   = 5.0;
    p0.through_all = true;

    indexed_triangle_set u0, l0;
    REQUIRE(draw_cut_split(cube, stroke, p0, &u0, &l0, nullptr));
    const double sum0 = double(its_volume(u0)) + double(its_volume(l0));
    REQUIRE(sum0 == Approx(cube_volume).epsilon(1e-3));

    DrawCutParams p1 = p0;
    p1.thickness = 1.0;

    indexed_triangle_set u1, l1;
    REQUIRE(draw_cut_split(cube, stroke, p1, &u1, &l1, nullptr));
    REQUIRE(watertight(u1));
    REQUIRE(watertight(l1));

    const double sum1 = double(its_volume(u1)) + double(its_volume(l1));
    const double band = 1.0 * CUBE * CUBE; // 1 mm along the cut, across the face
    REQUIRE(sum0 - sum1 == Approx(band).epsilon(0.02));

    // The GAP itself is 1 mm: the two halves' facing surfaces are 1 mm apart, so
    // the y extents leave exactly that much between them.
    BoundingBoxf3 bu, bl;
    for (const Vec3f& v : u1.vertices) bu.merge(v.cast<double>());
    for (const Vec3f& v : l1.vertices) bl.merge(v.cast<double>());
    const double gap = bu.center().y() > bl.center().y() ? bu.min.y() - bl.max.y()
                                                         : bl.min.y() - bu.max.y();
    REQUIRE(gap == Approx(1.0).margin(0.05));
}

// ---------------------------------------------------------------------------
// (7) The constant-direction modes: the "as-is" extrusion.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: Axis Z on a sloped face gives a prism of constant section", "[DrawCut]")
{
    // The point of a constant direction is that the cut is a PRISM: its
    // cross-section is the same at every height, whatever the face the stroke was
    // drawn on was doing. So: a circle whose samples sit on a sloped plane (the
    // normals tilted the way a raycast on a wedge would have returned them), cut
    // along -Z. With Direction = Surface normal the plug would lean; with Axis Z
    // it must not.
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke;
    const double R = 10.0;
    const int    n = 96;
    // A 30-degree slope in x: z = x * tan(30), and the normal tilted to match.
    const double slope = std::tan(M_PI / 6.0);
    const Vec3d  nrm   = Vec3d(-slope, 0.0, 1.0).normalized();
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        const double x = R * std::cos(a), y = R * std::sin(a);
        stroke.append(Vec3d(x, y, 0.5 * CUBE + x * slope), nrm);
    }
    stroke.append(Vec3d(R, 0.0, 0.5 * CUBE + R * slope), nrm);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());

    DrawCutParams params;
    params.direction   = DrawCutDirection::AxisZ;
    params.extension   = 4.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    REQUIRE(double(its_volume(upper)) + double(its_volume(lower)) ==
            Approx(double(its_volume(cube))).epsilon(1e-3));

    // THE PRISM PROPERTY: the plug's section is constant in z.
    //
    // Measured on the vertices rather than by slicing, but NOT by binning them into
    // height bands - a boolean's output has vertices only where geometry actually
    // changes (the two caps and the cut edges), so a band in the middle of a plain
    // cylindrical wall is legitimately empty and a band test there measures nothing.
    //
    // What it measures instead: every vertex of the plug that is not on a cap sits at
    // radius R from the same axis, whatever its height. A LEANING plug - which is
    // what Direction = Surface normal would give on this sloped stroke - has its axis
    // move with z, so its top and bottom vertices would sit at different (x,y)
    // centres. A prism's do not.
    BoundingBoxf3 plug_bb;
    for (const Vec3f& v : upper.vertices)
        plug_bb.merge(v.cast<double>());
    INFO("plug bbox z " << plug_bb.min.z() << " .. " << plug_bb.max.z()
         << ", volume " << its_volume(upper) << " of cube " << its_volume(cube));
    REQUIRE(plug_bb.min.z() == Approx(-0.5 * CUBE).margin(1e-3));
    REQUIRE(plug_bb.max.z() == Approx(+0.5 * CUBE).margin(1e-3));

    // The (x,y) EXTENT of the vertices in the bottom third and in the top third of
    // the plug's height. For a prism the two boxes coincide; for a leaning cut the
    // upper box sits offset from the lower one.
    //
    // The extent, not the MEAN of the vertices: a boolean's output has vertices only
    // where geometry changes, and this plug's top rim - where the sloped stroke meets
    // the cube's top face - carries far more of them than its plain bottom cap does,
    // so the vertex mean is pulled around by tessellation density rather than by any
    // lean. (Measured: 1.5 mm of "lean" on a plug whose volume is pi R^2 h to 0.4%
    // and every one of whose vertices is inside radius R - i.e. an artefact.)
    auto third_box = [&upper, &plug_bb](bool bottom) {
        const double lo = plug_bb.min.z();
        const double hi = plug_bb.max.z();
        const double a  = bottom ? lo : lo + 2.0 * (hi - lo) / 3.0;
        const double b  = bottom ? lo + (hi - lo) / 3.0 : hi;
        BoundingBoxf3 bb;
        for (const Vec3f& v : upper.vertices) {
            const Vec3d q = v.cast<double>();
            if (q.z() >= a - 1e-9 && q.z() <= b + 1e-9)
                bb.merge(Vec3d(q.x(), q.y(), 0.0));
        }
        REQUIRE(bb.defined);
        return bb;
    };
    const BoundingBoxf3 box_lo = third_box(true);
    const BoundingBoxf3 box_hi = third_box(false);
    REQUIRE((box_lo.center().head<2>() - box_hi.center().head<2>()).norm() < 0.2);
    REQUIRE(std::abs(box_lo.size().x() - box_hi.size().x()) < 0.3);
    REQUIRE(std::abs(box_lo.size().y() - box_hi.size().y()) < 0.3);

    // And every vertex is within a chord of radius R of the axis - the section is a
    // circle of radius R at every height, not just at the ends.
    for (const Vec3f& v : upper.vertices)
        REQUIRE(v.cast<double>().head<2>().norm() < R + 0.2);

    // The plug really is a cylinder of radius ~R through the whole height: pi R^2 *
    // 40, with the resampled polygon's inscribed-vs-circumscribed slack.
    REQUIRE(double(its_volume(upper)) == Approx(M_PI * R * R * CUBE).epsilon(0.05));
}

TEST_CASE("Draw cut: a constant direction is the same at every sample", "[DrawCut]")
{
    // The cheap structural check that the constant modes really are constant: the
    // cutter built for Axis Z over a sloped stroke has its inner rail on a single
    // plane, because every ray went the same way and the same distance.
    DrawCutStroke stroke;
    const int n = 40;
    for (int i = 0; i < n; ++ i) {
        const double t = double(i) / double(n - 1);
        // A stroke whose z varies, so "the same depth" is visible.
        stroke.append(Vec3d(-15.0 + 30.0 * t, 0.0, 5.0 * t), Vec3d(0.0, 0.0, 1.0));
    }
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-20, -20, -20));
    bbox.merge(Vec3d(20, 20, 20));

    DrawCutParams params;
    params.direction   = DrawCutDirection::AxisZ;
    params.extension   = 2.0;
    params.through_all = false;
    params.depth       = 10.0;

    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bbox);
    REQUIRE(!cutter.empty());
    REQUIRE(watertight(cutter));

    // THE PROPERTY: with a constant direction, the inward rail is the stroke
    // translated by exactly the same vector at every sample - here (0, 0, -10). So
    // for every sample there is a cutter vertex at exactly p - 10 * Z, and the
    // cutter's z extent is the stroke's own extent shifted down by 10 (plus whatever
    // the OUTWARD rail and the two tangent extensions add above and around it -
    // which is why this measures the inward points rather than the bounding box: an
    // open stroke's end extensions sit past the stroke's own first and last sample
    // and on a rising stroke that puts one of them below the stroke's lowest point).
    for (const DrawCutSample& smp : stroke.path()) {
        const Vec3d want = smp.pos - Vec3d(0.0, 0.0, 10.0);
        double best = 1e9;
        for (const Vec3f& v : cutter.vertices)
            best = std::min(best, (v.cast<double>() - want).norm());
        REQUIRE(best < 1e-6);
    }

    // And the constancy itself: every inward point is at the SAME depth below its
    // own stroke point, which is what would fail if the direction were per-sample.
    double dmin = 1e9, dmax = -1e9;
    for (const DrawCutSample& smp : stroke.path()) {
        double best = 1e9, best_dz = 0.0;
        for (const Vec3f& v : cutter.vertices) {
            const Vec3d p3 = v.cast<double>();
            const double lat = (p3.head<2>() - smp.pos.head<2>()).norm();
            if (lat < best && p3.z() < smp.pos.z() - 1.0) {
                best    = lat;
                best_dz = smp.pos.z() - p3.z();
            }
        }
        if (best < 1e-6) {
            dmin = std::min(dmin, best_dz);
            dmax = std::max(dmax, best_dz);
        }
    }
    REQUIRE(dmax - dmin < 1e-6);
}

TEST_CASE("Draw cut: the cutter solid is closed for both open and closed strokes", "[DrawCut]")
{
    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-20, -20, -20));
    bbox.merge(Vec3d(20, 20, 20));

    DrawCutParams params;
    params.extension   = 5.0;
    params.through_all = true;

    // Closed: a tube-like band capped at both rails.
    DrawCutStroke ring = circle_on_top(10.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(ring.is_closed());
    const indexed_triangle_set tube = draw_cut_cutter_solid(ring, params, bbox);
    REQUIRE(!tube.empty());
    REQUIRE(watertight(tube));
    // A cutter must be OUTWARD wound, or cut_with_solid()'s INTERSECTION would
    // come back as the complement.
    REQUIRE(its_volume(tube) > 0.f);

    // Open: the strip plus two end quads and two caps.
    DrawCutStroke line = line_on_top(0.5 * CUBE);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!line.is_closed());
    const indexed_triangle_set slab = draw_cut_cutter_solid(line, params, bbox);
    REQUIRE(!slab.empty());
    REQUIRE(watertight(slab));
    REQUIRE(its_volume(slab) > 0.f);

    // An invalid stroke gives nothing rather than something broken.
    DrawCutStroke bad;
    bad.append(Vec3d::Zero(), Vec3d::UnitZ());
    bad.finish();
    REQUIRE(draw_cut_cutter_solid(bad, params, bbox).empty());
}

// ---------------------------------------------------------------------------
// (8) The Extension really extends, and Depth really limits.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: Depth stops the cut short of through-all", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke = circle_on_top(10.0, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 4.0;
    params.through_all = false;
    params.depth       = 10.0; // a 10 mm deep pocket in a 40 mm cube
    // PHASE 3: Depth is the band's travel along d(p), and d(p) is straight down only
    // at a 90 degree lip. With that, Depth is the pocket's depth exactly as it was
    // under the ruled strip, which is what this case measures. What Depth does at
    // other angles is covered by the new `Depth sizes the flat core`.
    params.angle_deg   = 90.0;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, nullptr));

    // The plug is a 10 mm tall cylinder, not a 40 mm one.
    const double plug = double(its_volume(upper));
    REQUIRE(plug == Approx(M_PI * 10.0 * 10.0 * 10.0).epsilon(0.05));

    BoundingBoxf3 bb;
    for (const Vec3f& v : upper.vertices)
        bb.merge(v.cast<double>());
    REQUIRE(bb.max.z() == Approx(+0.5 * CUBE).margin(1e-3));
    REQUIRE(bb.min.z() == Approx(+0.5 * CUBE - 10.0).margin(0.2));

    // Volume is still conserved.
    REQUIRE(plug + double(its_volume(lower)) == Approx(double(its_volume(cube))).epsilon(1e-3));
}

// ===========================================================================
// PHASE 2
// ===========================================================================

// ---------------------------------------------------------------------------
// (10) THE DRAFT ANGLE.
//
// The closed form these check against, worked out once here so the cases below
// can quote it.
//
// A circle of radius R is drawn on the cube's top face, so at every sample the
// surface normal is +Z and the outward binormal is the radial direction. The
// ruling is then
//
//   d = cos(theta) * (-Z) + sin(theta) * r_hat
//
// so travelling a distance t along it from a point at (R, z = +20) reaches
// radius R + t sin(theta) at height 20 - t cos(theta). To drop a height h the
// ruling has to run t = h / cos(theta), and the radius there is
//
//   r(h) = R + h * tan(theta)
//
// The plug is therefore a CONE FRUSTUM of height h with radii R and R + h tan
// theta, whose volume is the standard
//
//   V = pi * h / 3 * (r1^2 + r1 r2 + r2^2)
//
// NOTE THE UNITS TRAP, which is the whole reason `depth` is set the way it is
// below: DrawCutParams::depth is measured ALONG THE RULING, not in z. Asking for
// depth = h would cut only h * cos(theta) deep. So the cases ask for
// h / cos(theta) and check the height that comes back.
//
// h = 8 mm on a 40 mm cube keeps both frusta comfortably inside the part (the
// +30 case's widest radius is 16.6 mm against the cube's 20 mm half-width), which
// is what makes the closed form the right answer rather than a clipped version of
// it.
// ---------------------------------------------------------------------------

// The closed-form frustum volume for a draft of `angle_deg` cut `h` deep in z
// from a circle of radius R.
[[maybe_unused]] static double frustum_volume(double R, double h, double angle_deg)
{
    const double r2 = R + h * std::tan(angle_deg * M_PI / 180.0);
    return M_PI * h / 3.0 * (R * R + R * r2 + r2 * r2);
}

// The ruling depth that cuts `h` deep in z at `angle_deg`.
[[maybe_unused]] static double ruling_depth_for(double h, double angle_deg)
{
    return h / std::cos(angle_deg * M_PI / 180.0);
}

// PHASE 3 (2026-09-13). The two cases that used to live here measured the SIGNED
// DRAFT ANGLE against a closed-form frustum, by cutting a CLOSED loop on a cube
// and weighing the plug. Phase 3 replaced the closed-loop surface: a loop is now a
// band plus a flat core, and `angle_deg` is the unsigned LIP ANGLE (0..90) rather
// than a signed draft, so a frustum is no longer the shape that cut makes and
// there is nothing left for those assertions to be true of.
//
// The draft itself is NOT gone - it is still exactly what an OPEN stroke's ruled
// strip does, and draw_cut_inward_dir() is still its implementation (the three
// structural cases below test it directly and are unchanged). So the measurement
// is kept, retargeted at the surface that still has a draft: an open stroke swept
// across the cube. The closed-loop replacements for what these used to cover are
// `the band's inward slope is the angle` and `the angle changes the plug without
// changing the flat core's plane`, in the phase 3 block at the end of this file.

TEST_CASE("Draw cut: a draft angle tilts an open stroke's ruled strip", "[DrawCut]")
{
    // The open stroke keeps the phase 1/2 model, so the draft still means what it
    // meant: the ruling is the inward normal rotated towards the outward binormal,
    // and the cut face leans by exactly that much.
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));

    const double ANGLE = 30.0;

    DrawCutStroke stroke = line_on_top(30.0, 60);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE_FALSE(stroke.is_closed());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 5.0;
    params.through_all = true;
    params.angle_deg   = ANGLE;

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // Material is conserved: a draft changes the SHAPE of the cut, not how much
    // material there is.
    REQUIRE(double(its_volume(upper)) + double(its_volume(lower)) ==
            Approx(cube_volume).epsilon(1e-3));

    // THE RULING LEANS BY THE ANGLE, at every sample. This is the structural fact
    // the frustum volume used to stand in for.
    for (size_t i = 0; i < stroke.path().size(); ++ i) {
        const Vec3d d = draw_cut_inward_dir(stroke, params, i);
        const Vec3d n = stroke.path()[i].normal;
        REQUIRE(std::acos(std::clamp(d.dot(-n), -1.0, 1.0)) * 180.0 / M_PI ==
                Approx(ANGLE).margin(1e-6));
    }
}

TEST_CASE("Draw cut: a negative draft leans an open stroke's strip the other way", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke = line_on_top(30.0, 60);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams pos, neg;
    for (DrawCutParams* p : { &pos, &neg }) {
        p->direction   = DrawCutDirection::SurfaceNormal;
        p->extension   = 5.0;
        p->through_all = true;
    }
    pos.angle_deg = +30.0;
    neg.angle_deg = -30.0;

    // The two drafts are mirror images about the untilted ruling: the same lean,
    // the other way along the binormal. That is the sign convention, and it is what
    // made one plug a flare and the other an undercut under the old closed-loop
    // model.
    for (size_t i = 0; i < stroke.path().size(); ++ i) {
        const Vec3d dp = draw_cut_inward_dir(stroke, pos, i);
        const Vec3d dn = draw_cut_inward_dir(stroke, neg, i);
        const Vec3d b  = stroke.binormal(i);
        REQUIRE(dp.dot(b) == Approx(-dn.dot(b)).margin(1e-9));
        REQUIRE(dp.dot(stroke.path()[i].normal) ==
                Approx(dn.dot(stroke.path()[i].normal)).margin(1e-9));
    }

    indexed_triangle_set u, l;
    REQUIRE(draw_cut_split(cube, stroke, neg, &u, &l, nullptr));
    REQUIRE(watertight(u));
    REQUIRE(watertight(l));
    REQUIRE(double(its_volume(u)) + double(its_volume(l)) ==
            Approx(double(its_volume(cube))).epsilon(1e-3));
}

TEST_CASE("Draw cut: angle 0 is bit-for-bit the phase 1 cut", "[DrawCut]")
{
    // The regression net for the whole of (10): adding the angle must not have
    // moved the zero case even slightly, because every phase 1 test and every
    // existing user's cut IS the zero case.
    DrawCutStroke stroke = circle_on_top(10.0, 64);
    REQUIRE(stroke.finish(1.0, 0.2) == DrawCutError::None);

    DrawCutParams p0;
    p0.direction = DrawCutDirection::SurfaceNormal;
    p0.angle_deg = 0.0;

    for (size_t i = 0; i < stroke.path().size(); ++ i) {
        const Vec3d d = draw_cut_inward_dir(stroke, p0, i);
        // Exactly the inward normal, to the last bit: at theta == 0
        // draw_cut_inward_dir() short-circuits before touching the binormal at all.
        REQUIRE((d + stroke.path()[i].normal).norm() < 1e-15);
    }
}

TEST_CASE("Draw cut: the angle only applies to Surface normal", "[DrawCut]")
{
    // A constant direction is ONE direction at every sample - that is what "as-is
    // extrusion" means - so a per-sample tilt is exactly what it is not. The panel
    // greys the slider out; this is the geometry behind that.
    DrawCutStroke stroke = circle_on_top(10.0, 64);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    for (DrawCutDirection dir : { DrawCutDirection::AxisX, DrawCutDirection::AxisY,
                                  DrawCutDirection::AxisZ, DrawCutDirection::View }) {
        DrawCutParams p;
        p.direction = dir;
        p.view_dir  = Vec3d(0.3, -0.2, -1.0);
        p.angle_deg = 45.0;

        const Vec3d first = draw_cut_inward_dir(stroke, p, 0);
        for (size_t i = 1; i < stroke.path().size(); ++ i)
            REQUIRE((draw_cut_inward_dir(stroke, p, i) - first).norm() < 1e-12);
    }
}

TEST_CASE("Draw cut: the angle rotates the ruling toward the outward binormal", "[DrawCut]")
{
    // The structural check behind the two volume cases: at every sample the ruling
    // is the inward normal rotated by exactly theta, in the plane it spans with the
    // binormal, and it is still a unit vector.
    DrawCutStroke stroke = circle_on_top(10.0, 64);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    for (double theta : { -60.0, -30.0, -5.0, 5.0, 30.0, 60.0 }) {
        DrawCutParams p;
        p.direction = DrawCutDirection::SurfaceNormal;
        p.angle_deg = theta;

        const double rad = theta * M_PI / 180.0;
        for (size_t i = 0; i < stroke.path().size(); ++ i) {
            const Vec3d d = draw_cut_inward_dir(stroke, p, i);
            const Vec3d n = stroke.path()[i].normal;
            const Vec3d b = stroke.binormal(i);

            REQUIRE(d.norm() == Approx(1.0).margin(1e-9));
            // The two components, read straight back off the rotation.
            REQUIRE(d.dot(-n) == Approx(std::cos(rad)).margin(1e-9));
            REQUIRE(d.dot(b)  == Approx(std::sin(rad)).margin(1e-9));
        }
    }
}

TEST_CASE("Draw cut: a concave stroke with a large angle is detected as a fold", "[DrawCut]")
{
    // THE FOLD CASE THE BRIEF ASKS FOR, and the thing phase 1's guard could not
    // see. A stroke with tight CONCAVE corners, with an extension small enough that
    // phase 1's test passes it - and then an angle large enough that the INWARD
    // rail, leaning sideways by depth * sin(theta), folds.
    DrawCutStroke flower;
    const int n = 200;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        const double r = 10.0 + 3.0 * std::cos(6.0 * a);
        flower.append(Vec3d(r * std::cos(a), r * std::sin(a), 0.5 * CUBE), Vec3d::UnitZ(), size_t(i));
    }
    flower.append(Vec3d(13.0, 0.0, 0.5 * CUBE), Vec3d::UnitZ(), 0);
    REQUIRE(flower.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(flower.is_closed());

    double kappa = 0.0;
    // A modest extension and NO angle: whatever phase 1 says here is the baseline.
    const bool folds_flat = draw_cut_strip_folds(flower, 1.0, &kappa, 0.0, 0.0);

    // The same stroke, the same extension, but drafted 45 degrees through 30 mm. The
    // lateral reach is then 30 * sin(45) = 21 mm, against lobes whose turning radius
    // is a couple of millimetres - so it folds, and unmistakably.
    double kappa_angled = 0.0;
    const bool folds_angled = draw_cut_strip_folds(flower, 1.0, &kappa_angled, 45.0, 30.0);
    REQUIRE(folds_angled);
    // The curvature reported is the real thing: a lobe of this flower turns tighter
    // than a 5 mm radius somewhere.
    REQUIRE(kappa_angled > 1.0 / 5.0);

    // And it is the ANGLE that did it, not the depth on its own - the negative
    // control. (If the flat case already folded, the angled one folding would say
    // nothing, so this is conditional on the baseline being clean.)
    if (!folds_flat)
        REQUIRE(draw_cut_strip_folds(flower, 1.0, nullptr, 0.0, 30.0) == false);
}

TEST_CASE("Draw cut: a plain circle never folds until the reach passes its radius", "[DrawCut]")
{
    // The negative control the phase 1 suite has for the extension, extended to the
    // angle. Every point of a circle drawn as a loop is a CONVEX corner as seen from
    // outside, so the outward rail can never fold - and the inward rail, which the
    // angle does move, would have to travel the full radius to reach the centre.
    DrawCutStroke circle = circle_on_top(15.0, 128);
    REQUIRE(circle.finish(1.0, 0.0) == DrawCutError::None);

    // 10 mm of depth at 30 degrees is 5 mm of lateral reach against a 15 mm radius.
    for (double theta : { -30.0, 0.0, 30.0 })
        REQUIRE(draw_cut_strip_folds(circle, 2.0, nullptr, theta, 10.0) == false);

    // Push the reach past the radius and it DOES fold, which is the guard working
    // rather than the test being vacuous: 60 mm at 30 degrees is 30 mm of reach.
    REQUIRE(draw_cut_strip_folds(circle, 2.0, nullptr, 30.0, 60.0));
}

// ---------------------------------------------------------------------------
// (11) LINE EDITING.
//
// The gizmo owns the gestures (hover, drag, right-click, Shift+click) and they
// need a GUI to exercise. What is testable headless is the CONTRACT the gestures
// rely on, and it is the part that would silently break: an edit rewrites the
// point list and hands it back to finish(), so the line has to come out resampled
// and - if it was a loop - still closed.
// ---------------------------------------------------------------------------

// What the gizmo's commit_draw_points() does, in the two lines of it that are not
// wx: rebuild the stroke from the edited points, re-expressing the closing span
// for a loop, and re-finish.
static DrawCutStroke commit_points(const std::vector<DrawCutSample>& pts, bool was_closed,
                                   double spacing = 1.0, double smoothing = 0.0)
{
    DrawCutStroke edited;
    for (const DrawCutSample& s : pts)
        edited.append(s.pos, s.normal, s.facet);
    if (was_closed)
        edited.append(pts.front().pos, pts.front().normal, pts.front().facet);
    edited.finish(spacing, smoothing);
    return edited;
}

// Is the path resampled at `spacing`?
//
// THE MEASURE HAS TO BE THE CHORD, AND THE CHORD IS NOT THE ARC. The resampler
// walks the INPUT polyline's arc length and emits a point every `spacing` along
// it, so two consecutive output points are `spacing` apart ALONG THAT POLYLINE -
// and the straight-line distance between them equals that only where the line is
// locally straight. Across a corner it is shorter, and across a sharp one much
// shorter: an edit that pulls one point 4 mm out of a 12 mm ring leaves an apex
// whose two neighbouring chords measure about 0.71 and 0.85 mm. That is the
// resampler working, not failing, and an earlier version of this helper that
// demanded equal chords was measuring the wrong thing.
//
// So the assertion is the one that actually distinguishes a resampled line from an
// un-resampled one: no span is LONGER than the spacing, and none is degenerate. A
// line that had not been re-finished after an edit would carry one span of several
// millimetres where the point was dragged - which is exactly what this catches -
// while the short chords at a corner are legitimate and are allowed.
static bool evenly_spaced(const DrawCutStroke& s, double spacing, double tol = 0.02)
{
    const std::vector<DrawCutSample>& p = s.path();
    if (p.size() < 3)
        return false;
    // The interior spans only: the last span of an open stroke is whatever is left
    // over, and a closed one's wrap span likewise.
    for (size_t i = 1; i + 2 < p.size(); ++ i) {
        const double d = (p[i + 1].pos - p[i].pos).norm();
        if (d > spacing + tol)   // a span the resampler would have split
            return false;
        if (d < 0.25 * spacing)  // a degenerate span the tangent maths would trip on
            return false;
    }
    return true;
}

TEST_CASE("Draw cut: moving a point keeps the line resampled and closed", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(ring.is_closed());

    std::vector<DrawCutSample> pts = ring.path();
    const size_t n_before = pts.size();
    REQUIRE(n_before > 10);

    // Drag one point 4 mm outward, the way a user pulling a handle would. It stays
    // on the top face (z is untouched), which is what the gizmo's raycast gives.
    const size_t moved = n_before / 3;
    const Vec3d radial = Vec3d(pts[moved].pos.x(), pts[moved].pos.y(), 0.0).normalized();
    pts[moved].pos += 4.0 * radial;

    const DrawCutStroke edited = commit_points(pts, /*was_closed*/ true);

    // STILL A USABLE LOOP. Both halves of that matter: an edit that opened the ring
    // would turn a plug cut into a splitting cut without telling anyone.
    REQUIRE(edited.error() == DrawCutError::None);
    REQUIRE(edited.valid());
    REQUIRE(edited.is_closed());

    // STILL RESAMPLED. This is the reason an edit goes back through finish() at all:
    // moving a point leaves one long span and one short one either side of it, and
    // the ruling density follows the spans.
    REQUIRE(evenly_spaced(edited, 1.0));

    // And the edit actually took: the line is longer than it was, and reaches
    // further out than the original ring.
    REQUIRE(edited.length() > ring.length());
    double max_r = 0.0;
    for (const DrawCutSample& s : edited.path())
        max_r = std::max(max_r, s.pos.head<2>().norm());
    REQUIRE(max_r > 13.0);
}

TEST_CASE("Draw cut: inserting a point keeps the line resampled and closed", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    std::vector<DrawCutSample> pts = ring.path();
    const size_t n_before = pts.size();

    // Shift+click on a segment: a new point on the chord between two neighbours,
    // pulled 3 mm outward (the gizmo re-projects it onto the model, which on a flat
    // face leaves it where the click was).
    const size_t seg = n_before / 2;
    DrawCutSample mid;
    mid.pos    = 0.5 * (pts[seg].pos + pts[seg + 1].pos);
    mid.normal = Vec3d::UnitZ();
    mid.facet  = pts[seg].facet;
    const Vec3d radial = Vec3d(mid.pos.x(), mid.pos.y(), 0.0).normalized();
    mid.pos += 3.0 * radial;
    pts.insert(pts.begin() + int(seg) + 1, mid);

    const DrawCutStroke edited = commit_points(pts, /*was_closed*/ true);

    REQUIRE(edited.error() == DrawCutError::None);
    REQUIRE(edited.is_closed());
    REQUIRE(evenly_spaced(edited, 1.0));
    // The resample is what makes the point COUNT rather than just be there: the new
    // bump lengthens the line, so the resampled path has at least as many points as
    // before even though only one was inserted.
    REQUIRE(edited.path().size() >= n_before);
    REQUIRE(edited.length() > ring.length());
}

TEST_CASE("Draw cut: deleting a point keeps the line resampled and closed", "[DrawCut]")
{
    // A ring with a deliberate spike in it, so deleting the spike's point is a
    // visible edit rather than a no-op the resampler would put straight back.
    DrawCutStroke ring = circle_on_top(12.0, 64);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    std::vector<DrawCutSample> pts = ring.path();
    const size_t spike = pts.size() / 4;
    const Vec3d radial = Vec3d(pts[spike].pos.x(), pts[spike].pos.y(), 0.0).normalized();
    pts[spike].pos += 6.0 * radial;

    const DrawCutStroke with_spike = commit_points(pts, true);
    REQUIRE(with_spike.valid());
    const double spiked_len = with_spike.length();

    // Now delete it - right-click on the handle.
    pts.erase(pts.begin() + int(spike));
    const DrawCutStroke edited = commit_points(pts, true);

    REQUIRE(edited.error() == DrawCutError::None);
    REQUIRE(edited.is_closed());
    REQUIRE(evenly_spaced(edited, 1.0));
    // Taking the spike out shortens the line back toward the plain ring.
    REQUIRE(edited.length() < spiked_len);
    double max_r = 0.0;
    for (const DrawCutSample& s : edited.path())
        max_r = std::max(max_r, s.pos.head<2>().norm());
    REQUIRE(max_r < 13.5);
}

TEST_CASE("Draw cut: editing an open line leaves it open", "[DrawCut]")
{
    // The mirror of the three closed cases: an open line edited is still an open
    // line, and its ENDS DO NOT CREEP - which is what stops a series of edits in the
    // middle of a line from quietly shortening or lengthening the cut's reach.
    DrawCutStroke line = line_on_top(18.0, 40);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!line.is_closed());

    std::vector<DrawCutSample> pts = line.path();
    const Vec3d first_before = pts.front().pos;
    const Vec3d last_before  = pts.back().pos;

    pts[pts.size() / 2].pos += Vec3d(0.0, 5.0, 0.0);

    const DrawCutStroke edited = commit_points(pts, /*was_closed*/ false);
    REQUIRE(edited.error() == DrawCutError::None);
    REQUIRE(!edited.is_closed());
    REQUIRE(evenly_spaced(edited, 1.0));

    // The FIRST point is exact: the resampler always emits the input's first sample.
    REQUIRE((edited.path().front().pos - first_before).norm() < 1e-6);

    // THE LAST POINT IS WITHIN HALF A SPACING, NOT EXACT, and the difference is a
    // deliberate rule rather than slack. draw_cut_resample() appends the final input
    // sample only when it is more than half a spacing from the last one already
    // emitted - otherwise it would leave a span the central-difference tangent reads
    // as a near-zero direction. Lengthening the middle of the line changes where the
    // last multiple of the spacing falls, so the end can be dropped and the path then
    // stops up to half a spacing short.
    //
    // What matters is that the end does not CREEP - the line still reaches the place
    // the user drew to, within that half spacing, and in particular has not grown
    // past it.
    const double end_gap = (edited.path().back().pos - last_before).norm();
    REQUIRE(end_gap < 0.5 * 1.0);
    // And it stops SHORT of the original end rather than overshooting it: the
    // original end is still the furthest point the line was ever asked to reach.
    REQUIRE(edited.path().back().pos.x() <= last_before.x() + 1e-6);
}

TEST_CASE("Draw cut: set_path replaces the path without resampling it", "[DrawCut]")
{
    // The re-projection hook the gizmo uses to put a smoothed line back on the mesh
    // (phase 1's deviation #7). Its contract: same length in, same length out,
    // binormals recomputed, open/closed untouched - and NO resample, because the
    // path already is one and running the resampler over its own output would walk
    // the samples a little further every time a slider moved.
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.3) == DrawCutError::None);
    REQUIRE(ring.is_closed());

    std::vector<DrawCutSample> path = ring.path();
    const size_t n = path.size();
    const Vec3d b_before = ring.binormal(0);

    // Push every sample 2 mm outward, which is what a re-projection onto a slightly
    // larger surface would do - and which must move the binormals' basepoints but
    // not their outwardness.
    for (DrawCutSample& s : path) {
        const Vec3d radial = Vec3d(s.pos.x(), s.pos.y(), 0.0).normalized();
        s.pos += 2.0 * radial;
    }
    ring.set_path(path);

    REQUIRE(ring.path().size() == n);
    REQUIRE(ring.is_closed());
    for (size_t i = 0; i < n; ++ i) {
        REQUIRE((ring.path()[i].pos - path[i].pos).norm() < 1e-12);
        // Still outward: the binormal points away from the centroid.
        const Vec3d radial = Vec3d(ring.path()[i].pos.x(), ring.path()[i].pos.y(), 0.0).normalized();
        REQUIRE(ring.binormal(i).dot(radial) > 0.5);
    }
    REQUIRE(ring.binormal(0).dot(b_before) > 0.9);

    // A path of the wrong length is refused rather than half-applied, which is what
    // keeps the closed decision finish() made meaningful.
    std::vector<DrawCutSample> shorter(path.begin(), path.end() - 3);
    ring.set_path(shorter);
    REQUIRE(ring.path().size() == n);
}

// ---------------------------------------------------------------------------
// (12) THE DRAWN SURFACE AS A SURFACE, and CONNECTORS ON IT.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: the surface point and frame agree with the cutter", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 3.0;
    params.through_all = false;
    params.depth       = 10.0;
    // PHASE 3: a closed loop's surface is the band, and the band's direction is the
    // lip angle applied to the in-plane inward direction. The setting that makes it
    // travel STRAIGHT DOWN from a flat top face - the geometry this case was written
    // against, and still the one it wants - is a 90 degree lip, i.e. a straight-walled
    // plug. At the phase-3 default of 0 the band runs horizontally inward along the
    // face instead, which is a flat shelf: a different, also correct, surface.
    params.angle_deg   = 90.0;

    // w == 0 is ON the stroke, so the surface point at a sample's own arc length is
    // that sample. This is the anchor everything else is measured from.
    const std::vector<DrawCutSample>& p = ring.path();
    double s_walk = 0.0;
    for (size_t i = 0; i < p.size(); ++ i) {
        const Vec3d q = draw_cut_surface_point(ring, params, s_walk, 0.0);
        REQUIRE((q - p[i].pos).norm() < 1e-6);
        s_walk += (p[(i + 1) % p.size()].pos - p[i].pos).norm();
    }

    // w > 0 goes INTO the part: on a cube's top face with no draft, straight down.
    const Vec3d in = draw_cut_surface_point(ring, params, 0.0, 5.0);
    REQUIRE(in.z() == Approx(0.5 * CUBE - 5.0).margin(1e-6));
    // w < 0 comes back OUT of it.
    const Vec3d out = draw_cut_surface_point(ring, params, 0.0, -2.0);
    REQUIRE(out.z() == Approx(0.5 * CUBE + 2.0).margin(1e-6));

    // THE FRAME's Z is the strip's own normal, which on a circular loop cut straight
    // down is the RADIAL direction - the direction the plug and the hole come apart
    // in. That is what makes a connector built on this frame coaxial in the two
    // halves.
    for (double s : { 0.0, 5.0, 20.0, 50.0 }) {
        const Transform3d f = draw_cut_surface_frame(ring, params, s, 3.0);
        const Vec3d z = f.linear().col(2);
        const Vec3d q = draw_cut_surface_point(ring, params, s, 3.0);
        const Vec3d radial = Vec3d(q.x(), q.y(), 0.0).normalized();
        REQUIRE(std::abs(z.dot(radial)) == Approx(1.0).margin(0.05));
        // Orthonormal and right-handed - a connector's own Rotation is applied in
        // this frame, so a skewed one would skew every connector.
        REQUIRE(f.linear().col(0).norm() == Approx(1.0).margin(1e-9));
        REQUIRE(f.linear().col(1).norm() == Approx(1.0).margin(1e-9));
        REQUIRE(f.linear().col(0).dot(f.linear().col(1)) == Approx(0.0).margin(1e-9));
        REQUIRE(f.linear().determinant() == Approx(1.0).margin(1e-9));
    }

    // THE FRAME IS CONTINUOUS round the loop, which a raw t x d would not be: two
    // neighbouring points must not have opposite normals, or two connectors a
    // millimetre apart would point opposite ways.
    Vec3d prev = draw_cut_surface_normal(ring, params, 0.0, 0.0);
    for (double s = 1.0; s < ring.length(); s += 1.0) {
        const Vec3d nrm = draw_cut_surface_normal(ring, params, s, 0.0);
        REQUIRE(nrm.dot(prev) > 0.9);
        prev = nrm;
    }
}

TEST_CASE("Draw cut: the surface projection inverts the surface point", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.through_all = false;
    params.depth       = 12.0;
    params.extension   = 3.0;

    // Round-trip: pick an (s, w), evaluate the surface there, project the result
    // back, and land on the same place. This is what turns a connector's stored
    // position into the parameters its frame is built from, so an error here is a
    // connector whose frame belongs to a different part of the surface.
    for (double s : { 2.0, 11.0, 37.0, 61.0 })
        for (double w : { 0.0, 3.0, 8.0 }) {
            const Vec3d q = draw_cut_surface_point(ring, params, s, w);
            double s2 = 0.0, w2 = 0.0, dist = 0.0;
            REQUIRE(draw_cut_surface_project(ring, params, q, s2, w2, &dist));
            REQUIRE(dist < 0.2);          // it IS on the surface
            REQUIRE(w2 == Approx(w).margin(0.2));
            // The POINT comes back, which is the property that matters - the
            // parameter s can sit a sample either side on a polygonised circle.
            REQUIRE((draw_cut_surface_point(ring, params, s2, w2) - q).norm() < 0.3);
        }
}

TEST_CASE("Draw cut: the surface domain test keeps a connector clear of the rims", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.extension   = 4.0;
    params.through_all = false;
    params.depth       = 10.0;
    const double reach  = 10.0;
    const double radius = 2.0; // the connector's own half-extent

    // Comfortably inside: yes.
    REQUIRE(draw_cut_surface_contains(ring, params, 20.0, 5.0, radius, reach));
    // Past the inward rim: no - its body would hang off the bottom of the cut.
    REQUIRE(!draw_cut_surface_contains(ring, params, 20.0, 9.5, radius, reach));
    // Past the outward rim: no.
    REQUIRE(!draw_cut_surface_contains(ring, params, 20.0, -3.5, radius, reach));
    // A closed stroke WRAPS, so no arc length is out of range.
    REQUIRE(draw_cut_surface_contains(ring, params, 1000.0, 5.0, radius, reach));

    // An OPEN line has ends to fall off, and they are what the test is for.
    DrawCutStroke line = line_on_top(18.0, 40);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!line.is_closed());
    REQUIRE(draw_cut_surface_contains(line, params, line.length() * 0.5, 5.0, radius, reach));
    REQUIRE(!draw_cut_surface_contains(line, params, 0.5, 5.0, radius, reach));
    REQUIRE(!draw_cut_surface_contains(line, params, line.length() - 0.5, 5.0, radius, reach));
}

TEST_CASE("Draw cut: a ruled strip is flat along its rules and curves across them", "[DrawCut]")
{
    // The reason Hinge and Thread get the SAME warning here as on a curved sheet
    // rather than a blanket refusal: a ruled surface is DEVELOPABLE along its rules,
    // so only one of its two directions can curve at all. A connector on a straight
    // stretch of stroke sits on a genuinely flat patch.
    DrawCutParams params;
    params.through_all = false;
    params.depth       = 10.0;
    params.extension   = 2.0;

    // A straight line swept straight down is a PLANE: flat in both directions.
    DrawCutStroke line = line_on_top(18.0, 40);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    for (double w : { 0.0, 5.0 })
        REQUIRE(draw_cut_surface_curvature_radius(line, params, line.length() * 0.5, w) > 1000.0);
    REQUIRE(draw_cut_patch_is_flat_enough(line, params, line.length() * 0.5, 3.0, 5.0));

    // A tight ring curves across the rules by its own radius, so a connector wider
    // than a third of it is warned about - which is the curved cut's own rule
    // (CurvedConnectorFlatPatchFactor == 3) applied to the drawn surface.
    DrawCutStroke tight = circle_on_top(6.0, 64);
    REQUIRE(tight.finish(1.0, 0.0) == DrawCutError::None);
    const double r = draw_cut_surface_curvature_radius(tight, params, 5.0, 0.0);
    REQUIRE(r == Approx(6.0).margin(1.0));
    REQUIRE(draw_cut_patch_is_flat_enough(tight, params, 5.0, 0.0, 1.0));   // small connector: fine
    REQUIRE(!draw_cut_patch_is_flat_enough(tight, params, 5.0, 0.0, 4.0));  // big one: warned
}

TEST_CASE("Draw cut: the surface tilt reads the strip's own normal", "[DrawCut]")
{
    // A loop on a flat top face cut straight down has a VERTICAL wall, so its normal
    // is horizontal - 90 degrees from the plane's +Z, and well past the
    // CurvedConnectorTiltWarnDeg threshold. That is correct, and it is what the panel
    // says about it: a connector in the wall of a plug does print sideways.
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.through_all = false;
    params.depth       = 10.0;
    // PHASE 3: "cut straight down" from a flat top face is the 90 degree lip - the
    // straight-walled plug. At the default lip of 0 the band lies along the face and
    // its normal is vertical, so the tilt is 0: also correct, and a connector there
    // prints flat rather than sideways.
    params.angle_deg   = 90.0;

    REQUIRE(draw_cut_surface_tilt_deg(ring, params, 10.0, 5.0) == Approx(90.0).margin(2.0));
    REQUIRE(draw_cut_surface_tilt_deg(ring, params, 10.0, 5.0) > CurvedConnectorTiltWarnDeg);
}

TEST_CASE("Draw cut: a connector on the swept surface makes a matching hole and plug", "[DrawCut]")
{
    // THE HEADLINE CONNECTOR CASE the brief asks for: a connector placed on the
    // drawn surface has to survive the split, with the hole in one half and the plug
    // in the other, and the two have to MATCH.
    //
    // Headless there is no gizmo, so the connector body is built the way the gizmo
    // builds it - a cylinder on the frame draw_cut_surface_frame() gives at the
    // connector's (s, w) - and the two halves are made by the same boolean the real
    // path uses. What is being checked is the property the real path depends on:
    // that the SAME surface frame, applied to both halves, subtracts and adds the
    // same solid.
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(ring.is_closed());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 3.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, ring, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // The connector: a 3 mm radius, 4 mm long dowel standing on the surface a third
    // of the way round and 8 mm down, on the frame the surface gives there.
    const double s = ring.length() * 0.25;
    const double w = 8.0;
    const double CR = 3.0, CH = 4.0;

    const Vec3d       at    = draw_cut_surface_point(ring, params, s, w);
    const Transform3d frame = draw_cut_surface_frame(ring, params, s, w);

    // A cylinder about the frame's +Z, CENTRED on the surface so half of it is in
    // each side - which is what a plug/hole pair is.
    indexed_triangle_set conn = its_make_cylinder(CR, CH, 2.0 * M_PI / 64.0);
    // its_make_cylinder stands on z == 0 and runs up; centre it.
    for (Vec3f& v : conn.vertices)
        v.z() -= float(0.5 * CH);
    its_transform(conn, Geometry::translation_transform(at) * frame);

    const double conn_volume = double(its_volume(conn));
    REQUIRE(conn_volume == Approx(M_PI * CR * CR * CH).epsilon(0.05));

    // The part of the connector inside each half. cut_with_solid() returns the
    // INTERSECTION as `lower`, so that is the side read here.
    //
    // The property being asserted is that the two are COMPLEMENTARY: the part inside
    // the plug plus the part inside the rest is the whole connector, with nothing
    // double-counted and nothing lost. That only holds if the two halves meet exactly
    // on the surface the connector is standing on - so this is the real test of the
    // frame, and it is what makes the hole and the plug the cut produces match.
    indexed_triangle_set a, b;
    REQUIRE(cut_with_solid(conn, upper, upper, /*kerf*/ false, &a, &b, "draw connector"));
    const double part_upper = double(its_volume(b));
    REQUIRE(cut_with_solid(conn, lower, lower, /*kerf*/ false, &a, &b, "draw connector"));
    const double part_lower = double(its_volume(b));

    REQUIRE(part_upper > 0.1 * conn_volume);
    REQUIRE(part_lower > 0.1 * conn_volume);
    // The two parts add up to the whole connector: the surface really is the
    // boundary between the halves, right where the connector stands.
    REQUIRE(part_upper + part_lower == Approx(conn_volume).epsilon(0.03));

    // And it is a genuinely BALANCED pair: the connector straddles the surface, so
    // neither half gets almost all of it. (A frame built from the wrong normal would
    // lie ALONG the surface instead of across it, and one side would get nearly
    // everything - which is the failure this catches.)
    REQUIRE(part_upper == Approx(part_lower).epsilon(0.25));
}

TEST_CASE("Draw cut: a connector's frame survives the kerf", "[DrawCut]")
{
    // The kerf moves BOTH faces along the same strip normal - the field
    // draw_cut_cutter_solid() derives once for the whole strip - so a connector
    // standing perpendicular to the surface stays coaxial with its own hole: the gap
    // opens along the connector's axis, which is the direction it comes apart in
    // anyway. Concretely that means the frame at an (s, w) does not depend on the
    // thickness, which is what this checks.
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams no_kerf;
    no_kerf.through_all = false;
    no_kerf.depth       = 10.0;

    DrawCutParams kerfed = no_kerf;
    kerfed.thickness = 1.0;

    for (double s : { 3.0, 19.0, 44.0 })
        for (double w : { 0.0, 4.0 }) {
            const Transform3d fa = draw_cut_surface_frame(ring, no_kerf, s, w);
            const Transform3d fb = draw_cut_surface_frame(ring, kerfed,  s, w);
            REQUIRE((fa.linear() - fb.linear()).norm() < 1e-12);
            REQUIRE((draw_cut_surface_point(ring, no_kerf, s, w) -
                     draw_cut_surface_point(ring, kerfed,  s, w)).norm() < 1e-12);
        }
}

TEST_CASE("Draw cut: the holonomy check passes a plain loop", "[DrawCut]")
{
    // A circle on a flat face has a perfectly consistent outward side, so the angle
    // is usable - the check must not fire on the common case.
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_frame_holonomy_flips(ring));

    // Nor on a wobbly one: the field is oriented from the centroid, so it survives
    // anything star-shaped about it.
    DrawCutStroke wobble;
    const int n = 120;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        const double r = 12.0 + 2.0 * std::sin(3.0 * a);
        wobble.append(Vec3d(r * std::cos(a), r * std::sin(a), 0.5 * CUBE), Vec3d::UnitZ(), size_t(i));
    }
    wobble.append(Vec3d(12.0, 0.0, 0.5 * CUBE), Vec3d::UnitZ(), 0);
    REQUIRE(wobble.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_frame_holonomy_flips(wobble));

    // An OPEN stroke has no loop to come back round, so it never flips.
    DrawCutStroke line = line_on_top(18.0, 40);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_frame_holonomy_flips(line));
}

// ---------------------------------------------------------------------------
// (9) Demo exports, matching the curved suite's habit. Behind an env var so a
// normal run writes nothing.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: demo exports", "[DrawCut][.demo]")
{
    if (std::getenv("SLIC3R_DRAW_CUT_DEMO") == nullptr)
        return;

    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.2) == DrawCutError::None);
    DrawCutParams params;
    params.extension   = 5.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, ring, params, &upper, &lower, nullptr));
    TriangleMesh(upper).WriteOBJFile("draw_plug_upper.obj");
    TriangleMesh(lower).WriteOBJFile("draw_plug_lower.obj");
}
// ===========================================================================
// THE CHAIN. 2026-09-12, from owner click-testing: a line round a large object
// has to be drawn in several strokes with the view rotated between them, the
// chain is only lofted once it CLOSES, and only one chain may exist.
// ===========================================================================

// A tall box, 20 x 20 x 80, centred on the origin - the article the owner was
// click-testing on. Four side faces, so a loop round it has to be drawn from at
// least two view directions.
static indexed_triangle_set tall_box(double w = 20.0, double d = 20.0, double h = 80.0)
{
    indexed_triangle_set its = its_make_cube(w, d, h);
    for (Vec3f& v : its.vertices)
        v -= Vec3f(float(0.5 * w), float(0.5 * d), float(0.5 * h));
    return its;
}

// One stroke's worth of raw samples along a straight run on a face of the tall
// box, from `a` to `b` with the face normal `n`, endpoints included.
static std::vector<DrawCutSample> face_run(const Vec3d& a, const Vec3d& b, const Vec3d& n, int n_samples)
{
    std::vector<DrawCutSample> out;
    for (int i = 0; i < n_samples; ++ i) {
        const double t = double(i) / double(n_samples - 1);
        DrawCutSample s;
        s.pos    = a + t * (b - a);
        s.normal = n.normalized();
        s.facet  = size_t(i);
        out.push_back(s);
    }
    return out;
}

TEST_CASE("Draw cut chain: a stroke continues from either endpoint", "[DrawCut]")
{
    const double r = 2.0;
    DrawCutChain chain;

    // First stroke: nothing to continue, so it is taken at the Back.
    std::vector<DrawCutSample> s1 = face_run(Vec3d(0, 0, 0), Vec3d(10, 0, 0), Vec3d::UnitZ(), 11);
    REQUIRE(chain.append(s1, r) == DrawChainEnd::Back);
    REQUIRE(chain.size() == 11);
    REQUIRE_FALSE(chain.is_closed());
    REQUIRE(chain.stroke_count() == 1);
    REQUIRE(chain.front_pos().isApprox(Vec3d(0, 0, 0)));
    REQUIRE(chain.back_pos().isApprox(Vec3d(10, 0, 0)));

    // CONTINUE FROM THE BACK. Starting at (10,0,0), which is the back endpoint.
    std::vector<DrawCutSample> s2 = face_run(Vec3d(10, 0, 0), Vec3d(10, 10, 0), Vec3d::UnitZ(), 11);
    REQUIRE(chain.append(s2, r) == DrawChainEnd::Back);
    REQUIRE(chain.stroke_count() == 2);
    // The duplicated join sample is dropped: 11 + 11 - 1.
    REQUIRE(chain.size() == 21);
    REQUIRE(chain.back_pos().isApprox(Vec3d(10, 10, 0)));
    REQUIRE(chain.front_pos().isApprox(Vec3d(0, 0, 0)));

    // CONTINUE FROM THE FRONT. Starting at (0,0,0), running AWAY from the chain -
    // so the samples must be reversed and prepended, or the sequence doubles back.
    std::vector<DrawCutSample> s3 = face_run(Vec3d(0, 0, 0), Vec3d(0, -10, 0), Vec3d::UnitZ(), 11);
    REQUIRE(chain.append(s3, r) == DrawChainEnd::Front);
    REQUIRE(chain.stroke_count() == 3);
    REQUIRE(chain.size() == 31);
    // The chain's FRONT is now the far end of the third stroke, and the sequence is
    // continuous: the new front is (0,-10,0) and the old front is still in the
    // middle of the list, not duplicated at the join.
    REQUIRE(chain.front_pos().isApprox(Vec3d(0, -10, 0)));
    REQUIRE(chain.back_pos().isApprox(Vec3d(10, 10, 0)));
    // CONTINUITY: no span in the whole chain is longer than the sampling step of
    // 1 mm. A stroke prepended unreversed would leave a 10 mm span at the join.
    const std::vector<DrawCutSample>& all = chain.samples();
    for (size_t i = 1; i < all.size(); ++ i)
        REQUIRE((all[i].pos - all[i - 1].pos).norm() < 1.5);

    // A snap radius of zero, and a start at neither endpoint, is DISJOINT.
    std::vector<DrawCutSample> s4 = face_run(Vec3d(5, 5, 0), Vec3d(6, 6, 0), Vec3d::UnitZ(), 5);
    REQUIRE(chain.append(s4, r) == DrawChainEnd::None);
    REQUIRE(chain.size() == 31); // unchanged
    REQUIRE(chain.stroke_count() == 3);
}

TEST_CASE("Draw cut chain: a second disjoint stroke is rejected", "[DrawCut]")
{
    const double r = 2.0;
    DrawCutChain chain;
    REQUIRE(chain.append(face_run(Vec3d(0, 0, 0), Vec3d(10, 0, 0), Vec3d::UnitZ(), 11), r) == DrawChainEnd::Back);

    // ONLY ONE CHAIN MAY EXIST (owner feedback item 4). A stroke that begins nowhere
    // near either endpoint is refused, and the chain it did not join is untouched -
    // the samples, the stroke count and the closed state all stand.
    const std::vector<DrawCutSample> before = chain.samples();
    REQUIRE(chain.append(face_run(Vec3d(0, 20, 0), Vec3d(10, 20, 0), Vec3d::UnitZ(), 11), r) == DrawChainEnd::None);
    REQUIRE(chain.samples().size() == before.size());
    REQUIRE(chain.stroke_count() == 1);

    // Just OUTSIDE the radius is still refused; just inside is taken. The boundary
    // is the claim, so both sides of it are asserted.
    REQUIRE(chain.append(face_run(Vec3d(10 + 1.05 * r, 0, 0), Vec3d(14, 0, 0), Vec3d::UnitZ(), 5), r) == DrawChainEnd::None);
    REQUIRE(chain.append(face_run(Vec3d(10 + 0.95 * r, 0, 0), Vec3d(14, 0, 0), Vec3d::UnitZ(), 5), r) == DrawChainEnd::Back);
}

TEST_CASE("Draw cut chain: the snap radius closes the chain and only within it", "[DrawCut]")
{
    const double r = 2.0;

    // A square loop on one plane, drawn in four strokes, whose last stroke ends
    // 1 mm from the first stroke's start - INSIDE the radius, so it snaps closed.
    auto square = [&](double final_gap) {
        DrawCutChain c;
        const Vec3d n = Vec3d::UnitZ();
        REQUIRE(c.append(face_run(Vec3d(0, 0, 0), Vec3d(10, 0, 0), n, 11), r) != DrawChainEnd::None);
        REQUIRE(c.append(face_run(Vec3d(10, 0, 0), Vec3d(10, 10, 0), n, 11), r) != DrawChainEnd::None);
        REQUIRE(c.append(face_run(Vec3d(10, 10, 0), Vec3d(0, 10, 0), n, 11), r) != DrawChainEnd::None);
        // The closing run, stopping `final_gap` short of (0,0,0).
        REQUIRE(c.append(face_run(Vec3d(0, 10, 0), Vec3d(0, final_gap, 0), n, 11), r) != DrawChainEnd::None);
        return c;
    };

    // 1 mm short, inside a 2 mm radius: CLOSED.
    DrawCutChain closed = square(1.0);
    REQUIRE(closed.is_closed());

    // 5 mm short, outside it: still OPEN, and therefore still produces no stroke.
    DrawCutChain open = square(5.0);
    REQUIRE_FALSE(open.is_closed());

    DrawCutStroke st;
    REQUIRE(open.finish(st) == DrawCutError::NotClosed);
    REQUIRE(st.path().empty());
    REQUIRE_FALSE(st.valid());

    // The closed one DOES produce a stroke, and it is a closed one.
    DrawCutStroke st2;
    REQUIRE(closed.finish(st2, 1.0, 0.0) == DrawCutError::None);
    REQUIRE(st2.is_closed());
    REQUIRE(st2.valid());

    // The snap radius scales with the part and is clamped at both ends.
    BoundingBoxf3 small(Vec3d(0, 0, 0), Vec3d(10, 10, 10));
    BoundingBoxf3 mid(Vec3d(0, 0, 0), Vec3d(100, 100, 100));
    BoundingBoxf3 huge(Vec3d(0, 0, 0), Vec3d(2000, 2000, 2000));
    REQUIRE(draw_cut_chain_snap_radius(small) == Approx(ChainSnapMinMm));
    REQUIRE(draw_cut_chain_snap_radius(mid) > ChainSnapMinMm);
    REQUIRE(draw_cut_chain_snap_radius(mid) < ChainSnapMaxMm);
    REQUIRE(draw_cut_chain_snap_radius(huge) == Approx(ChainSnapMaxMm));
    REQUIRE(draw_cut_chain_snap_radius(BoundingBoxf3()) == Approx(ChainSnapMinMm));
}

TEST_CASE("Draw cut chain: undo takes back the last stroke, and the closure with it", "[DrawCut]")
{
    const double r = 2.0;
    const Vec3d  n = Vec3d::UnitZ();
    DrawCutChain c;
    REQUIRE(c.append(face_run(Vec3d(0, 0, 0), Vec3d(10, 0, 0), n, 11), r) == DrawChainEnd::Back);
    REQUIRE(c.append(face_run(Vec3d(10, 0, 0), Vec3d(10, 10, 0), n, 11), r) == DrawChainEnd::Back);
    // A FRONT append, so undo has to work on a stroke that sits at index 0.
    REQUIRE(c.append(face_run(Vec3d(0, 0, 0), Vec3d(0, -10, 0), n, 11), r) == DrawChainEnd::Front);
    REQUIRE(c.size() == 31);
    REQUIRE(c.front_pos().isApprox(Vec3d(0, -10, 0)));

    // Take back the FRONT-appended stroke: the front endpoint goes back to (0,0,0)
    // and the back one has not moved.
    REQUIRE(c.undo_last_stroke());
    REQUIRE(c.stroke_count() == 2);
    REQUIRE(c.size() == 21);
    REQUIRE(c.front_pos().isApprox(Vec3d(0, 0, 0)));
    REQUIRE(c.back_pos().isApprox(Vec3d(10, 10, 0)));

    // Now close it, then undo: the closure goes away with the stroke that made it.
    REQUIRE(c.append(face_run(Vec3d(10, 10, 0), Vec3d(0.5, 0, 0), n, 16), r) == DrawChainEnd::Back);
    REQUIRE(c.is_closed());
    REQUIRE(c.undo_last_stroke());
    REQUIRE_FALSE(c.is_closed());
    REQUIRE(c.stroke_count() == 2);

    // Down to nothing, and no further.
    REQUIRE(c.undo_last_stroke());
    REQUIRE(c.undo_last_stroke());
    REQUIRE(c.empty());
    REQUIRE_FALSE(c.undo_last_stroke());
}

TEST_CASE("Draw cut chain: a closed chain refuses a continuation", "[DrawCut]")
{
    const double r = 2.0;
    const Vec3d  n = Vec3d::UnitZ();
    DrawCutChain c;
    REQUIRE(c.append(face_run(Vec3d(0, 0, 0), Vec3d(10, 0, 0), n, 11), r) == DrawChainEnd::Back);
    REQUIRE(c.append(face_run(Vec3d(10, 0, 0), Vec3d(0.5, 0, 0), n, 11), r) == DrawChainEnd::Back);
    REQUIRE(c.is_closed());
    // A closed chain has no free endpoints: end_for_start() says None wherever the
    // new stroke starts, including AT an endpoint. The gizmo's answer to "I want a
    // different line now" is Clear line / Ctrl+Z, which is a deliberate gesture.
    REQUIRE(c.end_for_start(Vec3d(0, 0, 0), r) == DrawChainEnd::None);
    REQUIRE(c.end_for_start(Vec3d(10, 0, 0), r) == DrawChainEnd::None);
    REQUIRE(c.append(face_run(Vec3d(0, 0, 0), Vec3d(0, 10, 0), n, 11), r) == DrawChainEnd::None);
}

TEST_CASE("Draw cut chain: a closed circle on one face cuts a plug and a body", "[DrawCut]")
{
    // OWNER FEEDBACK 4, the last sentence: a closed loop entirely on ONE face is a
    // valid chain and cuts a plug out of that face. Drawn in TWO strokes, so it is
    // the chain doing the closing rather than one gesture.
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));
    const double R = 12.0;
    const double z = 0.5 * CUBE;
    const double r = 2.0;

    auto arc = [&](double a0, double a1, int n) {
        std::vector<DrawCutSample> out;
        for (int i = 0; i < n; ++ i) {
            const double a = a0 + (a1 - a0) * double(i) / double(n - 1);
            DrawCutSample s;
            s.pos    = Vec3d(R * std::cos(a), R * std::sin(a), z);
            s.normal = Vec3d::UnitZ();
            out.push_back(s);
        }
        return out;
    };

    DrawCutChain chain;
    REQUIRE(chain.append(arc(0.0, M_PI, 60), r) == DrawChainEnd::Back);
    REQUIRE_FALSE(chain.is_closed());
    // The second stroke starts at the first's end and comes back round to (R,0,z).
    REQUIRE(chain.append(arc(M_PI, 2.0 * M_PI - 0.02, 60), r) == DrawChainEnd::Back);
    REQUIRE(chain.is_closed());

    DrawCutStroke stroke;
    REQUIRE(chain.finish(stroke, 1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());
    REQUIRE(stroke.valid());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 5.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    // The PLUG is a cylinder of radius R through the cube; the BODY is the rest.
    const double plug = double(its_volume(upper));
    const double body = double(its_volume(lower));
    REQUIRE(plug == Approx(M_PI * R * R * CUBE).epsilon(0.05));
    REQUIRE(plug + body == Approx(cube_volume).epsilon(1e-3));
    REQUIRE(body > plug);
}

TEST_CASE("Draw cut chain: a tall box looped in three strokes cuts two watertight halves", "[DrawCut]")
{
    // THE OWNER'S CASE. A 20 x 20 x 80 box; a horizontal loop round it at z == 0,
    // drawn in THREE strokes from TWO view directions - which is exactly what you
    // cannot do with one gesture, and what the chain exists for.
    //
    // The four side faces at x = +-10 and y = +-10. From the front you can reach the
    // +Y face and part of each side; rotating 180 degrees gets you the -Y face. So:
    //   stroke 1: along +Y face,  (-10, 10) -> (10, 10)
    //   stroke 2: round the +X face, (10, 10) -> (10, -10)
    //   stroke 3: the rest, (10,-10) -> (-10,-10) -> (-10, 10), closing on the start.
    const indexed_triangle_set box = tall_box();
    const double box_volume = double(its_volume(box));
    const double r = draw_cut_chain_snap_radius(BoundingBoxf3(Vec3d(-10, -10, -40), Vec3d(10, 10, 40)));
    REQUIRE(r > 0.9);

    const Vec3d nY(0, 1, 0), nX(1, 0, 0), nYn(0, -1, 0), nXn(-1, 0, 0);

    DrawCutChain chain;
    // Stroke 1, on the +Y face.
    REQUIRE(chain.append(face_run(Vec3d(-10, 10, 0), Vec3d(10, 10, 0), nY, 21), r) == DrawChainEnd::Back);
    // Stroke 2, on the +X face (drawn after rotating the view a quarter turn).
    REQUIRE(chain.append(face_run(Vec3d(10, 10, 0), Vec3d(10, -10, 0), nX, 21), r) == DrawChainEnd::Back);
    REQUIRE_FALSE(chain.is_closed());
    // Stroke 3, from the other side: the -Y face and then the -X face, ending back
    // at the chain's FRONT endpoint (-10, 10, 0) - so the chain snaps closed.
    {
        std::vector<DrawCutSample> s3 = face_run(Vec3d(10, -10, 0), Vec3d(-10, -10, 0), nYn, 21);
        const std::vector<DrawCutSample> tail = face_run(Vec3d(-10, -10, 0), Vec3d(-10, 9.5, 0), nXn, 21);
        s3.insert(s3.end(), tail.begin() + 1, tail.end());
        REQUIRE(chain.append(s3, r) == DrawChainEnd::Back);
    }
    REQUIRE(chain.is_closed());
    REQUIRE(chain.stroke_count() == 3);

    DrawCutStroke stroke;
    REQUIRE(chain.finish(stroke, 1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());
    REQUIRE(stroke.valid());

    // Direction = Surface normal on a loop round a box points INWARD on each face,
    // so the cutter is a horizontal band through the box: the "plug" is the slab
    // between the two rails and the rest is the two ends. That is not the cut the
    // owner wants for a loop round an object - Axis Z is - so this is the AXIS case,
    // which cuts the box into a top half and a bottom half.
    DrawCutParams params;
    params.direction   = DrawCutDirection::AxisZ;
    params.extension   = 5.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(box, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);

    // TWO WATERTIGHT HALVES, and they partition the box.
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    const double a = double(its_volume(upper));
    const double b = double(its_volume(lower));
    REQUIRE(a > 0.0);
    REQUIRE(b > 0.0);
    REQUIRE(a + b == Approx(box_volume).epsilon(1e-3));

    // Axis Z through a loop at z == 0 with 5 mm of extension: the cutter reaches
    // from z == -5 down through the box, so the "plug" is everything below the loop
    // and the other half is everything above it. Each is close to half the box.
    REQUIRE(a == Approx(0.5 * box_volume).epsilon(0.2));
    REQUIRE(b == Approx(0.5 * box_volume).epsilon(0.2));

    // And they really are stacked, not nested: one half's z range is above the
    // other's, which is the geometric claim "the loop cut the box in two".
    BoundingBoxf3 bu, bl;
    for (const Vec3f& v : upper.vertices) bu.merge(v.cast<double>());
    for (const Vec3f& v : lower.vertices) bl.merge(v.cast<double>());
    const bool stacked = bu.min.z() >= bl.max.z() - 1e-3 || bl.min.z() >= bu.max.z() - 1e-3;
    REQUIRE(stacked);
}

// ---------------------------------------------------------------------------
// THE HALVES CLASSIFICATION. Owner feedback item 3: the cyan/magenta colouring,
// the Visible/Ghost/Hidden display and the connectors must follow the DRAWN
// surface, not the flat plane.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: the halves classification follows the drawn surface", "[DrawCut]")
{
    // A closed circle of radius 12 on a 40 mm cube's top face, cut through all with
    // Surface normal. The plug is the cylinder r < 12; the body is the rest. The
    // FLAT PLANE at z == 0 would classify by z, which is the bug - so the test picks
    // points that the plane and the drawn surface disagree about.
    const indexed_triangle_set cube = centred_cube();
    const double R = 12.0;
    DrawCutStroke stroke = circle_on_top(R, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());

    DrawCutParams params;
    params.extension   = 5.0;
    params.through_all = true;

    BoundingBoxf3 bbox;
    for (const Vec3f& v : cube.vertices)
        bbox.merge(v.cast<double>());
    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bbox, 0.0);
    REQUIRE_FALSE(cutter.empty());

    // Inside the plug, ABOVE and BELOW the plane: both are UPPER, because the drawn
    // surface is a cylinder wall and the plug spans the full height. The flat plane
    // would call the lower one "lower", which is the whole complaint.
    REQUIRE(draw_cut_classify_upper(cutter, true, Vec3d(0, 0, +15)));
    REQUIRE(draw_cut_classify_upper(cutter, true, Vec3d(0, 0, -15)));
    REQUIRE(draw_cut_classify_upper(cutter, true, Vec3d(8, 0, -18)));
    // Outside the plug, above and below: both LOWER, for the same reason.
    REQUIRE_FALSE(draw_cut_classify_upper(cutter, true, Vec3d(18, 18, +15)));
    REQUIRE_FALSE(draw_cut_classify_upper(cutter, true, Vec3d(18, 18, -15)));
    REQUIRE_FALSE(draw_cut_classify_upper(cutter, true, Vec3d(0, 17, 0)));

    // AGAINST THE SPLIT ITSELF. Every vertex of the plug half must classify UPPER,
    // and every vertex of the body half LOWER - except the ones ON the cut face,
    // which belong to both and can go either way. Test the vertices pulled slightly
    // toward each half's own centroid, which moves them off the shared face.
    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, nullptr));

    // The nudge is RADIAL, not toward each half's centroid. The body half is a cube
    // with a cylindrical hole down the middle, so its centroid sits INSIDE the hole -
    // i.e. inside the other half - and nudging toward it walks every vertex the wrong
    // way. The cut surface here is the cylinder wall, so the direction that moves a
    // point off it and into its own half is radial: inward for the plug, outward for
    // the body. (The cube's own top and bottom faces are not cut surfaces, so a vertex
    // there needs no nudge at all and the radial one does it no harm.)
    auto radial = [](const Vec3d& p) {
        const Vec2d xy = p.head<2>();
        return xy.norm() > 1e-9 ? Vec3d(xy.x() / xy.norm(), xy.y() / xy.norm(), 0.0) : Vec3d::UnitX();
    };

    int n_up = 0, n_up_ok = 0;
    for (const Vec3f& v : upper.vertices) {
        const Vec3d p = v.cast<double>();
        // Only the vertices ON the cylinder wall need moving; the plug's flat ends are
        // already unambiguous. Nudging them all is simpler and equally valid.
        const Vec3d q = p - 0.5 * radial(p);
        ++ n_up;
        if (draw_cut_classify_upper(cutter, true, q))
            ++ n_up_ok;
    }
    int n_lo = 0, n_lo_ok = 0;
    for (const Vec3f& v : lower.vertices) {
        const Vec3d p = v.cast<double>();
        const Vec3d q = p + 0.5 * radial(p);
        // A vertex on the cube's outer wall nudged outward leaves the part entirely -
        // which is still OUTSIDE the plug, so the classification is unchanged and the
        // assertion holds. Skip nothing.
        ++ n_lo;
        if (!draw_cut_classify_upper(cutter, true, q))
            ++ n_lo_ok;
    }
    REQUIRE(n_up > 20);
    REQUIRE(n_lo > 20);
    // EVERY nudged vertex, not a majority: the radial nudge is exact for this geometry,
    // so anything less would mean the classification really is wrong somewhere.
    REQUIRE(n_up_ok == n_up);
    REQUIRE(n_lo_ok == n_lo);
}

TEST_CASE("Draw cut: the voxel field agrees with the exact classification", "[DrawCut]")
{
    // The field is what the shader samples, so it has to agree with
    // draw_cut_classify_upper() everywhere except within a voxel of the boundary.
    const indexed_triangle_set cube = centred_cube();
    DrawCutStroke stroke = circle_on_top(12.0, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.extension   = 5.0;
    params.through_all = true;

    BoundingBoxf3 bbox;
    for (const Vec3f& v : cube.vertices)
        bbox.merge(v.cast<double>());
    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bbox, 0.0);

    const int N = 32;
    BoundingBoxf3 fb;
    const std::vector<float> field = draw_cut_inside_field(cutter, true, bbox, N, N, N, &fb);
    REQUIRE(field.size() == size_t(N) * N * N);
    REQUIRE(fb.defined);
    // The field's box is the part's box GROWN by a voxel, so a fragment on the
    // part's own surface is inside the field rather than on its clamped border.
    REQUIRE(fb.min.x() < bbox.min.x());
    REQUIRE(fb.max.z() > bbox.max.z());

    const Vec3d span = fb.size();
    const Vec3d step(span.x() / double(N - 1), span.y() / double(N - 1), span.z() / double(N - 1));
    const double voxel = step.maxCoeff();

    int checked = 0, agreed = 0, upper_seen = 0, lower_seen = 0;
    for (int k = 0; k < N; ++ k)
        for (int j = 0; j < N; ++ j)
            for (int i = 0; i < N; ++ i) {
                const Vec3d p(fb.min.x() + i * step.x(), fb.min.y() + j * step.y(), fb.min.z() + k * step.z());
                // Skip the boundary band: within a voxel of the cylinder wall the
                // parity answer legitimately differs between the field's own jittered
                // column and an arbitrary probe ray.
                const double dr = std::abs(p.head<2>().norm() - 12.0);
                if (dr < 1.5 * voxel)
                    continue;
                const bool exact = draw_cut_classify_upper(cutter, true, p);
                const bool from_field = field[(size_t(k) * N + j) * N + i] < 0.f;
                ++ checked;
                if (exact == from_field)
                    ++ agreed;
                if (from_field) ++ upper_seen; else ++ lower_seen;
            }
    REQUIRE(checked > 10000);
    REQUIRE(agreed == checked);
    // Both halves are actually represented, so an all-one-sign field cannot pass.
    REQUIRE(upper_seen > 100);
    REQUIRE(lower_seen > 100);

    // SIGN CONVENTION: negative is UPPER, which is `side < 0` in the shader and so
    // side 1, which apply_color_clip_plane_colors() feeds with the upper colour.
    // Dead centre of the plug:
    const int ci = int(std::lround((0.0 - fb.min.x()) / step.x()));
    const int cj = int(std::lround((0.0 - fb.min.y()) / step.y()));
    const int ck = int(std::lround((0.0 - fb.min.z()) / step.z()));
    REQUIRE(field[(size_t(ck) * N + cj) * N + ci] < 0.f);
}

TEST_CASE("Draw cut: an open stroke's classification is the other way round", "[DrawCut]")
{
    // The convention flips between the two cases, which is the one thing about this
    // that is easy to get backwards: for a CLOSED stroke "inside the cutter" is the
    // plug and therefore UPPER, for an OPEN one the cutter is the swept slab on the
    // LOWER side.
    const indexed_triangle_set cube = centred_cube();
    DrawCutStroke stroke = line_on_top(25.0, 60);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE_FALSE(stroke.is_closed());

    DrawCutParams params;
    params.extension   = 5.0;
    params.through_all = true;

    BoundingBoxf3 bbox;
    for (const Vec3f& v : cube.vertices)
        bbox.merge(v.cast<double>());
    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bbox, 0.0);
    REQUIRE_FALSE(cutter.empty());

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, nullptr));

    // Which side is which comes from the split, not from a guess: take a point well
    // inside each half (their centroids, which for two slabs are inside them) and
    // require the classification to name the same halves the split produced.
    auto centroid = [](const indexed_triangle_set& its) {
        Vec3d c = Vec3d::Zero();
        for (const Vec3f& v : its.vertices)
            c += v.cast<double>();
        return Vec3d(c / double(its.vertices.size()));
    };
    REQUIRE(draw_cut_classify_upper(cutter, false, centroid(upper)));
    REQUIRE_FALSE(draw_cut_classify_upper(cutter, false, centroid(lower)));
}
TEST_CASE("Draw cut chain: an open line is cut with only when the user says so", "[DrawCut]")
{
    // PHASE 1'S OPEN CUT, kept but made explicit. "The line is not finished yet" and "the
    // line is finished and is not a loop" look identical from the samples alone, and
    // lofting every partial line is the wild-shape preview the chain exists to remove -
    // so the chain refuses until the user commits one way or the other.
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));
    const double r = 2.0;

    DrawCutChain chain;
    // A line right across the cube's top face, drawn in TWO strokes.
    REQUIRE(chain.append(face_run(Vec3d(-25, 0, 20), Vec3d(0, 0, 20), Vec3d::UnitZ(), 26), r) == DrawChainEnd::Back);
    REQUIRE(chain.append(face_run(Vec3d(0, 0, 20), Vec3d(25, 0, 20), Vec3d::UnitZ(), 26), r) == DrawChainEnd::Back);
    REQUIRE_FALSE(chain.is_closed());
    REQUIRE_FALSE(chain.is_finished_open());

    // Not finished: NO stroke, so nothing can be lofted or previewed.
    DrawCutStroke st;
    REQUIRE(chain.finish(st, 1.0, 0.0) == DrawCutError::NotClosed);
    REQUIRE_FALSE(st.valid());

    // The user says "use it as it is".
    REQUIRE(chain.finish_open());
    REQUIRE(chain.is_finished_open());
    REQUIRE_FALSE(chain.is_closed());
    REQUIRE(chain.finish(st, 1.0, 0.0) == DrawCutError::None);
    REQUIRE(st.valid());
    REQUIRE_FALSE(st.is_closed());   // still an OPEN stroke, which is the point

    // And it cuts the way phase 1's open line did: two watertight halves partitioning
    // the cube, on opposite sides of the line.
    DrawCutParams params;
    params.extension   = 5.0;
    params.through_all = true;
    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, st, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    REQUIRE(double(its_volume(upper)) + double(its_volume(lower)) == Approx(cube_volume).epsilon(1e-3));

    // CARRYING ON CLEARS THE VERDICT: a chain that has just grown is one the user is
    // still drawing, so the surface goes away again until they commit afresh.
    REQUIRE(chain.append(face_run(Vec3d(25, 0, 20), Vec3d(25, 10, 20), Vec3d::UnitZ(), 11), r) == DrawChainEnd::Back);
    REQUIRE_FALSE(chain.is_finished_open());
    REQUIRE(chain.finish(st, 1.0, 0.0) == DrawCutError::NotClosed);

    // So does an undo, and so does closing the loop.
    REQUIRE(chain.finish_open());
    REQUIRE(chain.undo_last_stroke());
    REQUIRE_FALSE(chain.is_finished_open());
    REQUIRE(chain.finish_open());
    REQUIRE(chain.force_close());
    REQUIRE(chain.is_closed());
    REQUIRE_FALSE(chain.is_finished_open());

    // A CLOSED chain refuses finish_open(): the two are mutually exclusive, and the way
    // back to an open line is Ctrl+Z, not a second verdict on the same chain.
    REQUIRE_FALSE(chain.finish_open());

    // And a chain too short to be a line at all refuses both.
    DrawCutChain tiny;
    REQUIRE(tiny.append(face_run(Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d::UnitZ(), 3), r) == DrawChainEnd::Back);
    REQUIRE_FALSE(tiny.finish_open());
    REQUIRE_FALSE(tiny.force_close());
}

// ===========================================================================
// 2026-09-13, OWNER CLICK-TEST. Three things were broken in practice and none
// of them could have been caught by the phase-1/2/3 tests, because all three
// live where the geometry meets the camera and the phase tests never had one.
//
// There is no GUI harness, so what is pinned here is the pure logic the gizmo
// now calls: the SCREEN-SPACE endpoint pick (item 1) against a projection
// built by hand, and the SURFACE-PATH close (item 2) against a real mesh.
// ===========================================================================

// A pinhole camera, by hand: eye at `eye` looking at the origin, up +Z, a 60 deg
// vertical field of view and a 1000x1000 viewport. Enough to project a point to a
// pixel the way CameraUtils::project does, without any of the GUI.
//
// Returns nullopt for a point BEHIND the eye, which is the case the gizmo's own
// projection has to survive: an endpoint on the far side of a part the camera has
// orbited past.
struct TestCamera
{
    Vec3d  eye;
    double px_per_rad;   // 1000 px over a 60 deg fov
    Vec3d  fwd, right, up;

    explicit TestCamera(const Vec3d& e) : eye(e)
    {
        px_per_rad = 500.0 / std::tan(0.5 * 60.0 * M_PI / 180.0);
        fwd   = (-eye).normalized();
        right = fwd.cross(Vec3d::UnitZ()).normalized();
        up    = right.cross(fwd).normalized();
    }

    std::optional<Vec2d> project(const Vec3d& p) const
    {
        const Vec3d v = p - eye;
        const double z = v.dot(fwd);
        if (z <= 1e-6)
            return std::nullopt;          // behind the eye
        return Vec2d(500.0 + px_per_rad * v.dot(right) / z,
                     500.0 - px_per_rad * v.dot(up)    / z);
    }
};

TEST_CASE("Draw cut chain: the endpoint pick is in SCREEN space, not object mm", "[DrawCut]")
{
    // THE BUG THIS PINS. The press used to ask
    // `end_for_start(raycast_hit, draw_cut_chain_snap_radius(bbox))` - a 3D test in
    // object millimetres. On a large part that radius is a few pixels at a normal
    // zoom, so a click visually dead-centre on the endpoint handle was refused and
    // the chain could never be extended or closed (the owner's item 1).

    // A line up one face of the TALL BOX, so the endpoints are 60 mm apart - far
    // enough that no 3D snap radius could ever cover the gap between them.
    DrawCutChain chain;
    const std::vector<DrawCutSample> s1 =
        face_run(Vec3d(10, 0, -30), Vec3d(10, 0, 30), Vec3d::UnitX(), 31);
    REQUIRE(chain.append(s1, 2.0) == DrawChainEnd::Back);
    REQUIRE(chain.front_pos().isApprox(Vec3d(10, 0, -30)));
    REQUIRE(chain.back_pos().isApprox(Vec3d(10, 0, 30)));

    const TestCamera cam(Vec3d(300, 0, 0));
    auto proj = [&](const Vec3d& p) { return cam.project(p); };

    const std::optional<Vec2d> px_front = cam.project(chain.front_pos());
    const std::optional<Vec2d> px_back  = cam.project(chain.back_pos());
    REQUIRE(px_front.has_value());
    REQUIRE(px_back.has_value());
    // The two handles are far apart ON SCREEN, which is what makes the tie-break
    // below meaningful rather than accidental.
    REQUIRE((*px_front - *px_back).norm() > 100.0);

    const double pick = 16.0;

    // DEAD CENTRE on each handle picks that handle.
    REQUIRE(draw_cut_chain_end_at_pixel(chain, *px_front, proj, pick) == DrawChainEnd::Front);
    REQUIRE(draw_cut_chain_end_at_pixel(chain, *px_back,  proj, pick) == DrawChainEnd::Back);

    // JUST INSIDE the radius still picks; just OUTSIDE does not. Both sides of the
    // boundary, because a pick that is generous everywhere is as bad as one that is
    // generous nowhere - it would swallow clicks meant for the model.
    const Vec2d off(1.0, 0.0);
    REQUIRE(draw_cut_chain_end_at_pixel(chain, *px_back + 0.94 * pick * off, proj, pick) == DrawChainEnd::Back);
    REQUIRE(draw_cut_chain_end_at_pixel(chain, *px_back + 1.06 * pick * off, proj, pick) == DrawChainEnd::None);

    // THE POINT OF THE WHOLE CHANGE: the pick radius is in PIXELS, so it does not
    // move when the object does. The same click, on a chain ten times the size,
    // still lands - where the 3D radius (2% of the diagonal, clamped at 6 mm) would
    // have covered a tenth as much of the screen.
    DrawCutChain big;
    REQUIRE(big.append(face_run(Vec3d(100, 0, -300), Vec3d(100, 0, 300), Vec3d::UnitX(), 31), 6.0)
            == DrawChainEnd::Back);
    const TestCamera cam_far(Vec3d(3000, 0, 0));
    auto proj_far = [&](const Vec3d& p) { return cam_far.project(p); };
    const std::optional<Vec2d> big_back = cam_far.project(big.back_pos());
    REQUIRE(big_back.has_value());
    REQUIRE(draw_cut_chain_end_at_pixel(big, *big_back + 0.94 * pick * off, proj_far, pick) == DrawChainEnd::Back);
    // ... and the 3D test it replaced would have REFUSED that same click: 0.94 * 16 px
    // at this distance is far more than the 6 mm the snap radius clamps to.
    {
        const double px_at_back = cam_far.px_per_rad / (big.back_pos() - cam_far.eye).dot(cam_far.fwd);
        const double mm_per_px  = 1.0 / px_at_back;
        REQUIRE(0.94 * pick * mm_per_px > ChainSnapMaxMm);
    }

    // AN ENDPOINT BEHIND THE CAMERA is not pickable and, crucially, cannot win the
    // tie-break from the other one. Looking from +X at a chain whose front is at
    // x = +10: put the eye between them.
    {
        const TestCamera behind(Vec3d(0, 0, 0));   // fwd is undefined at the origin...
        (void) behind;
        // ... so use a camera whose forward direction puts the FRONT endpoint behind it.
        TestCamera c2(Vec3d(400, 0, 0));
        c2.fwd = Vec3d(1, 0, 0); // looking AWAY from the part
        c2.right = c2.fwd.cross(Vec3d::UnitZ()).normalized();
        c2.up = c2.right.cross(c2.fwd).normalized();
        auto p2 = [&](const Vec3d& p) { return c2.project(p); };
        REQUIRE_FALSE(c2.project(chain.front_pos()).has_value());
        REQUIRE_FALSE(c2.project(chain.back_pos()).has_value());
        // Nothing projects, so nothing is picked - and no NaN sneaks into the compare.
        REQUIRE(draw_cut_chain_end_at_pixel(chain, Vec2d(500, 500), p2, pick) == DrawChainEnd::None);
    }

    // THE NEARER handle wins when both are in range - the short-chain case, where the
    // two ends sit within a pick radius of each other on screen.
    {
        DrawCutChain shortc;
        REQUIRE(shortc.append(face_run(Vec3d(10, 0, -0.4), Vec3d(10, 0, 0.4), Vec3d::UnitX(), 8), 0.01)
                == DrawChainEnd::Back);
        const std::optional<Vec2d> a = cam.project(shortc.front_pos());
        const std::optional<Vec2d> b = cam.project(shortc.back_pos());
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        REQUIRE((*a - *b).norm() < pick);                 // both in range of one click
        REQUIRE(draw_cut_chain_end_at_pixel(shortc, *a, proj, pick) == DrawChainEnd::Front);
        REQUIRE(draw_cut_chain_end_at_pixel(shortc, *b, proj, pick) == DrawChainEnd::Back);
    }

    // AN EMPTY chain has no handles, and a CLOSED one has no FREE handles. Both give
    // None: what a click does in those cases is the caller's rule (an empty chain
    // accepts a stroke anywhere; a closed one accepts none), not a claim about
    // handles that are not drawn.
    REQUIRE(draw_cut_chain_end_at_pixel(DrawCutChain(), Vec2d(500, 500), proj, pick) == DrawChainEnd::None);
    REQUIRE(chain.force_close());
    REQUIRE(draw_cut_chain_end_at_pixel(chain, *px_back, proj, pick) == DrawChainEnd::None);
}

TEST_CASE("Draw cut chain: append_at takes the end as given and still tests closure", "[DrawCut]")
{
    // The gizmo decides WHICH END in screen space and hands it to append_at(), so
    // append_at() must not re-derive it from a 3D radius - that would refuse exactly
    // the strokes the screen pick exists to rescue.
    const double tiny = 1e-6;
    DrawCutChain chain;
    REQUIRE(chain.append_at(face_run(Vec3d(0, 0, 0), Vec3d(10, 0, 0), Vec3d::UnitZ(), 11),
                            DrawChainEnd::Back, tiny) == DrawChainEnd::Back);

    // A stroke whose first sample is 5 mm from the back endpoint - far outside any
    // snap radius this chain would use. append() REFUSES it; append_at() takes it,
    // because the user clicked the handle and the raycast simply landed elsewhere.
    const std::vector<DrawCutSample> off_join =
        face_run(Vec3d(15, 0, 0), Vec3d(25, 0, 0), Vec3d::UnitZ(), 11);
    {
        DrawCutChain copy = chain;
        REQUIRE(copy.append(off_join, tiny) == DrawChainEnd::None);   // the old behaviour
        REQUIRE(copy.size() == 11);                                   // untouched
    }
    REQUIRE(chain.append_at(off_join, DrawChainEnd::Back, tiny) == DrawChainEnd::Back);
    REQUIRE(chain.size() == 22);
    REQUIRE(chain.stroke_count() == 2);

    // A FRONT append is still reversed, so the sequence stays continuous.
    REQUIRE(chain.append_at(face_run(Vec3d(0, 0, 0), Vec3d(-10, 0, 0), Vec3d::UnitZ(), 11),
                            DrawChainEnd::Front, tiny) == DrawChainEnd::Front);
    REQUIRE(chain.front_pos().isApprox(Vec3d(-10, 0, 0)));
    for (size_t i = 1; i < chain.samples().size(); ++ i)
        REQUIRE((chain.samples()[i].pos - chain.samples()[i - 1].pos).norm() < 6.0);

    // THE CLOSURE TEST IS STILL THE RADIUS'S JOB. A stroke ending far from the other
    // endpoint does not close the chain...
    REQUIRE_FALSE(chain.is_closed());
    // ... and one ending on it does.
    REQUIRE(chain.append_at(face_run(Vec3d(25, 0, 0), Vec3d(-10, 0, 0), Vec3d::UnitZ(), 21),
                            DrawChainEnd::Back, 1.0) == DrawChainEnd::Back);
    REQUIRE(chain.is_closed());

    // A CLOSED chain refuses append_at() whatever end is named: the screen pick can
    // choose between two free ends, never conjure one on a finished loop.
    REQUIRE(chain.append_at(face_run(Vec3d(-10, 0, 0), Vec3d(-20, 0, 0), Vec3d::UnitZ(), 11),
                            DrawChainEnd::Front, 1.0) == DrawChainEnd::None);
    // And None is refused outright rather than defaulting to an end.
    DrawCutChain fresh;
    REQUIRE(fresh.append_at(face_run(Vec3d(0, 0, 0), Vec3d(10, 0, 0), Vec3d::UnitZ(), 11),
                            DrawChainEnd::None, tiny) == DrawChainEnd::None);
    REQUIRE(fresh.empty());
}

TEST_CASE("Draw cut chain: Close loop joins along the SURFACE, never through the part", "[DrawCut]")
{
    // THE BUG THIS PINS (owner item 2). force_close() set m_closed and added nothing,
    // so the closing span was the straight CHORD between the two endpoints. On a line
    // drawn round the outside of a part that chord runs through the material and the
    // ruled surface swept along it emerges on the FAR side - which is what "Close loop
    // mirrors the line to the other side of the object" describes. Nothing was ever
    // reflected; a chord through a solid looks the same from outside.

    const indexed_triangle_set box = tall_box(20.0, 20.0, 80.0);   // x,y in [-10,10], z in [-40,40]

    // A chain along the +X face and round onto the +Y face, leaving a gap on the -X
    // face that Close loop has to walk. The endpoints are on ADJACENT faces, which is
    // the brief's case: a straight line between them cuts the corner off through the
    // solid.
    // Up the +X face to the corner, then along the -Y face: every sample really is on
    // the box, which is the baseline the closure has to match.
    DrawCutChain chain;
    REQUIRE(chain.append(face_run(Vec3d(10, -8, 0), Vec3d(10, -10, 0), Vec3d::UnitX(), 9), 1.0)
            == DrawChainEnd::Back);
    REQUIRE(chain.append(face_run(Vec3d(10, -10, 0), Vec3d(-8, -10, 0), -Vec3d::UnitY(), 17), 1.0)
            == DrawChainEnd::Back);
    REQUIRE_FALSE(chain.is_closed());

    const Vec3d a = chain.back_pos();    // (-8, -10, 0), on the -Y face
    const Vec3d b = chain.front_pos();   // (10,  -8, 0), on the +X face

    // THE CHORD between them passes through the MIDDLE of the box - which is exactly
    // why force_close()'s closing span was wrong. Pin that, so the test says what the
    // bug was rather than only what the fix does.
    {
        // Sampled along the chord, the interior points are INSIDE the box - the chord
        // cuts the (10,-10) corner off through solid material. That is exactly the
        // surface force_close() used to sweep the closing ruling along.
        const TriangleMesh tm_chord(box);
        AABBMesh aabb_chord(tm_chord);
        int off_surface = 0;
        for (int i = 1; i < 10; ++ i) {
            const Vec3d p = a + (double(i) / 10.0) * (b - a);
            if (aabb_chord.squared_distance(p) > 0.25)
                ++ off_surface;
        }
        REQUIRE(off_surface > 0);
    }

    // THE SURFACE PATH. The gizmo raycasts these from the camera; here they are built
    // directly - the property under test is that close_along_path() makes the closure
    // out of points ON the mesh, not how they were found.
    //
    // Round the corner at (10, 10): from (-8,10) along +Y face to the corner, then
    // down the +X face to (10,-8).
    // The LONG way round: -Y face to the (-10,-10) corner, up the -X face, across the
    // +Y face and down the +X face back to (10,-8). Every point on a face of the box.
    std::vector<DrawCutSample> path;
    auto add = [&path](const std::vector<DrawCutSample>& run) {
        for (const DrawCutSample& s : run) path.push_back(s);
    };
    add(face_run(Vec3d(-8, -10, 0), Vec3d(-10, -10, 0), -Vec3d::UnitY(),  5));
    add(face_run(Vec3d(-10, -10, 0), Vec3d(-10, 10, 0), -Vec3d::UnitX(), 21));
    add(face_run(Vec3d(-10, 10, 0), Vec3d(10, 10, 0),    Vec3d::UnitY(), 21));
    add(face_run(Vec3d(10, 10, 0), Vec3d(10, -8, 0),     Vec3d::UnitX(), 19));
    // close_along_path() wants only what is BETWEEN the endpoints.
    path.erase(path.begin());
    path.pop_back();

    REQUIRE(chain.close_along_path(path));
    REQUIRE(chain.is_closed());

    // EVERY SAMPLE OF THE CLOSED CHAIN IS ON THE BOX. This is the assertion the brief
    // asks for: "endpoints on a cube's adjacent faces yield a path on the surface with
    // no point off the mesh by more than a tolerance". A chord closure fails it at the
    // first interior point.
    {
        const TriangleMesh tm(box);
        AABBMesh aabb(tm);
        for (const DrawCutSample& s : chain.samples())
            REQUIRE(aabb.squared_distance(s.pos) < 1e-6);   // 1 um of the surface
    }

    // AND IT CUTS. Two watertight halves that partition the box - the point of closing
    // the loop at all.
    DrawCutStroke st;
    REQUIRE(chain.finish(st, 1.0, 0.0) == DrawCutError::None);
    REQUIRE(st.valid());
    REQUIRE(st.is_closed());

    DrawCutParams params;
    params.extension   = 5.0;
    params.through_all = true;
    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(box, st, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    REQUIRE(double(its_volume(upper)) + double(its_volume(lower))
            == Approx(double(its_volume(box))).epsilon(1e-3));

    // UNDO TAKES THE WHOLE CLOSURE BACK IN ONE STEP, and gives back the open line: the
    // closure is one stroke, so Ctrl+Z after Close loop is not a walk back through
    // nineteen invisible points.
    const size_t before = chain.size();
    REQUIRE(chain.undo_last_stroke());
    REQUIRE_FALSE(chain.is_closed());
    REQUIRE(chain.size() < before);
    REQUIRE(chain.back_pos().isApprox(a));
    REQUIRE(chain.front_pos().isApprox(b));
}

TEST_CASE("Draw cut chain: an empty close path is exactly force_close, and a short chain refuses", "[DrawCut]")
{
    // ENDS ALREADY TOUCHING. Within the snap radius there is nothing between the two
    // endpoints to walk, and the closing span is under the resample spacing - so an
    // empty path is the right answer, not a degenerate one. It has to behave exactly
    // as force_close() did, because that case was never the bug.
    DrawCutChain a, b;
    const std::vector<std::vector<DrawCutSample>> strokes = {
        face_run(Vec3d(10, -8, 0), Vec3d(10, 8, 0), Vec3d::UnitX(), 17),
        face_run(Vec3d(10, 8, 0), Vec3d(10, -7.5, 0), Vec3d::UnitX(), 17),
    };
    for (const std::vector<DrawCutSample>& s : strokes) {
        REQUIRE(a.append(s, 0.01) == DrawChainEnd::Back);
        REQUIRE(b.append(s, 0.01) == DrawChainEnd::Back);
    }
    REQUIRE(a.force_close());
    REQUIRE(b.close_along_path({}));
    REQUIRE(a == b);

    // A CLOSED chain refuses to be closed again - there is no second closure to make.
    REQUIRE_FALSE(b.close_along_path({}));

    // TOO SHORT TO BE A LOOP, measured on what the chain will HAVE: a short chain plus
    // a long path is a perfectly good loop, so the floor is on the sum.
    DrawCutChain tiny;
    REQUIRE(tiny.append(face_run(Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d::UnitZ(), 3), 0.01)
            == DrawChainEnd::Back);
    REQUIRE_FALSE(tiny.close_along_path({}));
    REQUIRE_FALSE(tiny.is_closed());
    REQUIRE(tiny.close_along_path(face_run(Vec3d(1, 0, 0), Vec3d(0, 0, 0), Vec3d::UnitZ(), 4)));
    REQUIRE(tiny.is_closed());
}

// ===========================================================================
// PHASE 3 (2026-09-13): THE BAND AND THE FLAT CORE.
//
// The owner's report was that the closed-loop cut was useless: Depth projected
// outwards, Through all drove rulings through the part at whatever angles the
// surface normals had, and there was no flat face for the two halves to meet on.
// The surface is now a BAND (from the skin, in at the lip angle) plus a FLAT
// CORE (the best-fit plane of the loop, inside the band). These cases pin that
// down: the core is planar, the band's slope is the angle, and Depth sizes the
// core rather than reaching through the part.
// ===========================================================================

// A cylinder about the Z axis, radius r, height h, centred on the origin - the
// article the owner drew on. `seg` controls the tessellation.
static indexed_triangle_set centred_cylinder(double r, double h, int seg = 128)
{
    indexed_triangle_set its = its_make_cylinder(r, h, 2.0 * M_PI / double(seg));
    // its_make_cylinder stands on z == 0; centre it.
    for (Vec3f& v : its.vertices)
        v.z() -= float(0.5 * h);
    return its;
}

// A WAVY closed loop round a cylinder at z ~= mid, amplitude `amp` in z. The
// normals are the cylinder's own outward radial ones, which is what a raycast
// onto the barrel would have produced. This is deliberately NOT planar: the
// whole point of the core plane is that a wavy line still yields a flat mating
// face.
static DrawCutStroke wavy_loop_on_cylinder(double r, double z_mid, double amp,
                                           int n = 96, int lobes = 3)
{
    DrawCutStroke stroke;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        const double z = z_mid + amp * std::sin(double(lobes) * a);
        stroke.append(Vec3d(r * std::cos(a), r * std::sin(a), z),
                      Vec3d(std::cos(a), std::sin(a), 0.0), size_t(i));
    }
    // Come back to the start so finish() decides closed from the gap.
    stroke.append(Vec3d(r, 0.0, z_mid), Vec3d(1, 0, 0), 0);
    return stroke;
}

// The signed distance of `p` from the plane (n, c).
static double plane_dist(const Vec3d& p, const Vec3d& n, const Vec3d& c)
{
    return (p - c).dot(n);
}

// The vertices of `its` that lie within `tol` of the plane (n, c), and the worst
// deviation among them. `count` is how many; `worst` the largest |distance|,
// which for a genuinely planar face should be at the tolerance floor rather than
// anywhere near it.
static size_t vertices_on_plane(const indexed_triangle_set& its, const Vec3d& n,
                                const Vec3d& c, double tol, double* worst = nullptr)
{
    size_t count = 0;
    double w = 0.0;
    for (const Vec3f& v : its.vertices) {
        const double d = std::abs(plane_dist(v.cast<double>(), n, c));
        if (d <= tol) {
            ++ count;
            w = std::max(w, d);
        }
    }
    if (worst != nullptr)
        *worst = w;
    return count;
}

// ---------------------------------------------------------------------------
// (A) THE CORE PLANE FIT.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: the core plane fits a wavy loop and is flat", "[DrawCut]")
{
    DrawCutStroke stroke = wavy_loop_on_cylinder(20.0, 0.0, 5.0);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());

    DrawCutParams params;
    Vec3d n, c;
    REQUIRE(draw_cut_core_plane(stroke, params, n, c));

    // A loop that wanders +-5 mm in z round a cylinder still has Z as its
    // area-weighted normal: the wave is symmetric, so it cancels.
    REQUIRE(std::abs(std::abs(n.z()) - 1.0) < 1e-3);
    // The centroid sits on the axis, at the loop's mean height.
    REQUIRE(c.x() == Approx(0.0).margin(0.2));
    REQUIRE(c.y() == Approx(0.0).margin(0.2));
    REQUIRE(c.z() == Approx(0.0).margin(0.2));
}

TEST_CASE("Draw cut: Axis direction forces the core normal to that axis", "[DrawCut]")
{
    // A loop whose own best fit is Z, asked to use X instead.
    DrawCutStroke stroke = circle_on_top(10.0, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.direction = DrawCutDirection::AxisX;
    Vec3d n, c;
    REQUIRE(draw_cut_core_plane(stroke, params, n, c));
    REQUIRE(std::abs(std::abs(n.x()) - 1.0) < 1e-9);
    // The centroid is still the loop's own, so the plane passes through it.
    REQUIRE(c.z() == Approx(0.5 * CUBE).margin(1e-6));
}

TEST_CASE("Draw cut: the band direction is the angle between inward and the normal", "[DrawCut]")
{
    const Vec3d inward = -Vec3d::UnitX();   // towards the axis
    const Vec3d n      =  Vec3d::UnitZ();   // core normal

    // 0: straight in, perpendicular to n - a flat shelf.
    const Vec3d d0 = draw_cut_band_dir(inward, n, 0.0);
    REQUIRE(d0.dot(n) == Approx(0.0).margin(1e-9));
    REQUIRE(d0.dot(inward) == Approx(1.0).margin(1e-9));

    // 90: straight along -n - a straight wall.
    const Vec3d d90 = draw_cut_band_dir(inward, n, 90.0);
    REQUIRE(d90.dot(n) == Approx(-1.0).margin(1e-9));

    // 45: equal parts of each, and still unit.
    const Vec3d d45 = draw_cut_band_dir(inward, n, 45.0);
    REQUIRE(d45.norm() == Approx(1.0).margin(1e-9));
    REQUIRE(d45.dot(inward) == Approx(std::sqrt(0.5)).margin(1e-9));
    REQUIRE(d45.dot(n) == Approx(-std::sqrt(0.5)).margin(1e-9));

    // Out of range is clamped rather than allowed to travel back out of the part.
    REQUIRE(draw_cut_band_dir(inward, n, -30.0).dot(n) == Approx(0.0).margin(1e-9));
    REQUIRE(draw_cut_band_dir(inward, n, 130.0).dot(n) == Approx(-1.0).margin(1e-9));
}

// ---------------------------------------------------------------------------
// (B) THE CUT ITSELF. The spec's headline case.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: a wavy loop round a cylinder gives two watertight halves with a flat core", "[DrawCut]")
{
    const indexed_triangle_set cyl = centred_cylinder(20.0, 60.0);
    REQUIRE(watertight(cyl));

    // z ~= 30 in the spec's frame is the middle in ours (the cylinder is centred),
    // which is the same cut: a loop round the barrel, half way up.
    DrawCutStroke stroke = wavy_loop_on_cylinder(20.0, 0.0, 5.0);
    REQUIRE(stroke.finish(1.0, 0.1) == DrawCutError::None);
    REQUIRE(stroke.is_closed());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 3.0;
    params.through_all = false;
    params.depth       = 3.0;
    params.angle_deg   = 0.0;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cyl, stroke, params, &upper, &lower, nullptr));

    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // Volume is conserved: the two halves are the part, with no kerf.
    const double total = double(its_volume(upper)) + double(its_volume(lower));
    REQUIRE(total == Approx(double(its_volume(cyl))).epsilon(0.01));

    // THE CORE IS FLAT, AND IT IS WHERE draw_cut_core_face() SAYS. Both halves must
    // carry a planar patch on it - that is the face they mate on. Note this is the
    // FACE plane (the fit displaced by the band's travel), not the plane through the
    // loop itself: the band reaches in by Depth before the core starts.
    BoundingBoxf3 cyl_bb;
    for (const Vec3f& v : cyl.vertices)
        cyl_bb.merge(v.cast<double>());
    Vec3d n, c;
    REQUIRE(draw_cut_core_face(stroke, params, cyl_bb, n, c));

    double worst_u = 0.0, worst_l = 0.0;
    const size_t on_u = vertices_on_plane(upper, n, c, 0.05, &worst_u);
    const size_t on_l = vertices_on_plane(lower, n, c, 0.05, &worst_l);

    // A real core, not one or two stray vertices that happen to be near the plane.
    REQUIRE(on_u > 8);
    REQUIRE(on_l > 8);
    // And coplanar to within the spec's 0.05 mm.
    REQUIRE(worst_u <= 0.05);
    REQUIRE(worst_l <= 0.05);
}

TEST_CASE("Draw cut: Depth sizes the flat core", "[DrawCut]")
{
    const indexed_triangle_set cyl = centred_cylinder(20.0, 60.0);

    DrawCutStroke stroke = wavy_loop_on_cylinder(20.0, 0.0, 5.0);
    REQUIRE(stroke.finish(1.0, 0.1) == DrawCutError::None);

    // The core polygon is the loop inset by depth * cos(angle). At angle 0 that is
    // the depth itself, so the core radius goes 20 - 3 = 17 at depth 3 and
    // 20 - 10 = 10 at depth 10. Measure the core's own extent rather than a
    // volume, because the extent is the thing the user sees.
    auto core_radius = [&](double depth) {
        DrawCutParams params;
        params.extension   = 3.0;
        params.through_all = false;
        params.depth       = depth;
        params.angle_deg   = 0.0;

        BoundingBoxf3 bb;
        for (const Vec3f& v : cyl.vertices)
            bb.merge(v.cast<double>());
        Vec3d n, c;
        REQUIRE(draw_cut_core_face(stroke, params, bb, n, c));

        const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bb, 0.0);
        REQUIRE_FALSE(cutter.empty());

        // THE CORE RING, and why "widest on the plane" is the wrong way to find it.
        //
        // At a lip angle of 0 the band travels entirely in-plane, so the core plane
        // sits at the loop's own mean height - and the OUTER ring (pushed out by
        // Extension, radius 23 here) lies on that same plane. Taking the widest
        // radius on the plane therefore measures the outer ring, not the core, and
        // reports 23 where the core is 17.
        //
        // The core is the INNER ring, so take the smallest radius among the vertices
        // on the plane, ignoring the cap centres (which sit at radius ~0 and would
        // win outright). The band's two rings are the only other things there.
        double r = std::numeric_limits<double>::max();
        for (const Vec3f& v : cutter.vertices) {
            const Vec3d q = v.cast<double>();
            if (std::abs(plane_dist(q, n, c)) > 0.05)
                continue;
            const Vec3d rad = (q - c) - plane_dist(q, n, c) * n;
            const double rr = rad.norm();
            if (rr < 1.0)
                continue; // a fan centre, not a ring
            r = std::min(r, rr);
        }
        REQUIRE(r < std::numeric_limits<double>::max());
        return r;
    };

    const double r3  = core_radius(3.0);
    const double r10 = core_radius(10.0);

    REQUIRE(r3  == Approx(17.0).margin(0.6));
    REQUIRE(r10 == Approx(10.0).margin(0.6));
    // Deeper means a SMALLER core, which is the semantic the old Depth got backwards.
    REQUIRE(r10 < r3);
}

TEST_CASE("Draw cut: the band's inward slope is the angle", "[DrawCut]")
{
    // A plain circular loop on a cylinder, so the geometry is exactly analysable:
    // the band runs from radius 20 inward, dropping in z by depth * sin(angle) and
    // in radius by depth * cos(angle). The slope of the band off the core plane is
    // therefore the angle itself.
    const double R = 20.0;

    for (double angle : { 0.0, 30.0, 45.0, 60.0 }) {
        DrawCutStroke stroke = wavy_loop_on_cylinder(R, 0.0, 0.0, 96, 1); // amp 0 => planar
        REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

        DrawCutParams params;
        params.extension   = 2.0;
        params.through_all = false;
        params.depth       = 4.0;
        params.angle_deg   = angle;

        Vec3d n, c;
        REQUIRE(draw_cut_core_plane(stroke, params, n, c));

        // Walk the surface: the point on the stroke (w == 0) and the point at the
        // end of the band's travel (w == depth). The angle between the chord and
        // the core plane is the lip angle.
        const Vec3d p0 = draw_cut_surface_point(stroke, params, 0.0, 0.0);
        const Vec3d p1 = draw_cut_surface_point(stroke, params, 0.0, params.depth);

        const Vec3d chord = p1 - p0;
        REQUIRE(chord.norm() > 1e-6);
        // The component along n against the in-plane component gives the slope.
        const double along_n  = std::abs(chord.dot(n));
        const double in_plane = (chord - chord.dot(n) * n).norm();
        const double slope_deg = std::atan2(along_n, in_plane) * 180.0 / M_PI;

        INFO("angle " << angle << " measured " << slope_deg);
        REQUIRE(slope_deg == Approx(angle).margin(2.0));
    }
}

TEST_CASE("Draw cut: Through all spans the part and leaves no flat core", "[DrawCut]")
{
    const indexed_triangle_set cyl = centred_cylinder(20.0, 60.0);

    DrawCutStroke stroke = wavy_loop_on_cylinder(20.0, 0.0, 5.0);
    REQUIRE(stroke.finish(1.0, 0.1) == DrawCutError::None);

    DrawCutParams params;
    params.extension   = 3.0;
    params.through_all = true;
    params.angle_deg   = 20.0;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cyl, stroke, params, &upper, &lower, nullptr));

    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    REQUIRE(double(its_volume(upper)) + double(its_volume(lower)) ==
            Approx(double(its_volume(cyl))).epsilon(0.01));

    // NO FLAT CORE. With through-all there is no core plane, so the fitted plane
    // should carry no planar patch - a handful of vertices may graze it where the
    // band crosses, but not the dense disc a core produces.
    BoundingBoxf3 tbb;
    for (const Vec3f& v : cyl.vertices)
        tbb.merge(v.cast<double>());
    Vec3d n, c;
    // There is no core FACE with through-all - that is the definition of it.
    REQUIRE_FALSE(draw_cut_core_face(stroke, params, tbb, n, c));
    // The fitted plane still exists (it is the loop's own), and it is what a stray
    // grazing vertex would sit on; assert the dense disc is absent from it.
    REQUIRE(draw_cut_core_plane(stroke, params, n, c));
    const size_t on_plane = vertices_on_plane(upper, n, c, 0.05);
    const size_t total_v  = upper.vertices.size();
    REQUIRE(total_v > 0);
    // Well under the fraction a capped core would put there.
    REQUIRE(double(on_plane) / double(total_v) < 0.15);

    // THE CUT REACHES BOTH ENDS OF THE PART. This loop goes ROUND the barrel, so it
    // is a SEPARATION (see draw_cut_loop_separates()) and the two halves are stacked
    // rather than nested - between them they span the full height, and each one
    // reaches one end of the cylinder. Asserting that ONE half spans the whole height
    // would be the plug reading, which is the wrong shape for a wrap-around loop.
    REQUIRE(draw_cut_loop_separates(cyl, stroke, params));

    BoundingBoxf3 bu, bl;
    for (const Vec3f& v : upper.vertices) bu.merge(v.cast<double>());
    for (const Vec3f& v : lower.vertices) bl.merge(v.cast<double>());
    REQUIRE(std::max(bu.max.z(), bl.max.z()) == Approx(+30.0).margin(0.5));
    REQUIRE(std::min(bu.min.z(), bl.min.z()) == Approx(-30.0).margin(0.5));
    // ONE HALF IS THE SLAB BELOW THE LINE. The other is the remainder, whose bounding
    // box still spans the whole cylinder however it was cut - see the note in
    // "Through all round a cylinder cuts it in two" for why that makes a bbox
    // comparison the wrong instrument here.
    REQUIRE(bu.max.z() < 10.0);          // the slab stops at the wavy line, not the top
    REQUIRE(bu.min.z() == Approx(-30.0).margin(0.5));
    // Each half is a real piece, not a sliver.
    REQUIRE(double(its_volume(upper)) > 0.2 * double(its_volume(cyl)));
    REQUIRE(double(its_volume(lower)) > 0.2 * double(its_volume(cyl)));
}

TEST_CASE("Draw cut: a circle on a cube face cuts a plug with a flat bottom at depth", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke = circle_on_top(10.0, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 3.0;
    params.through_all = false;
    params.depth       = 4.0;
    params.angle_deg   = 90.0;   // a straight-walled plug: the band runs down -n

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, nullptr));

    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // The plug is the piece the loop went round.
    BoundingBoxf3 bb;
    for (const Vec3f& v : upper.vertices)
        bb.merge(v.cast<double>());

    // It reaches the face it was drawn on, and its FLAT BOTTOM sits `depth` below.
    REQUIRE(bb.max.z() == Approx(+0.5 * CUBE).margin(1e-3));
    REQUIRE(bb.min.z() == Approx(+0.5 * CUBE - params.depth).margin(0.2));

    // A straight-walled plug of radius 10 and height 4.
    REQUIRE(double(its_volume(upper)) == Approx(M_PI * 10.0 * 10.0 * 4.0).epsilon(0.06));

    // And the bottom really is FLAT: the core face carries a planar patch.
    BoundingBoxf3 cbb;
    for (const Vec3f& v : cube.vertices)
        cbb.merge(v.cast<double>());
    Vec3d n, c;
    REQUIRE(draw_cut_core_face(stroke, params, cbb, n, c));
    double worst = 0.0;
    REQUIRE(vertices_on_plane(upper, n, c, 0.05, &worst) > 8);
    REQUIRE(worst <= 0.05);

    REQUIRE(double(its_volume(upper)) + double(its_volume(lower)) ==
            Approx(double(its_volume(cube))).epsilon(1e-3));
}

TEST_CASE("Draw cut: the angle keeps the core square to the loop and sets its depth", "[DrawCut]")
{
    // The core's ORIENTATION is the loop's own fit, so the lip angle must not tilt
    // the mating face - a user dialling the key angle must not find the face
    // drifting out of square. Its DEPTH does move with the angle, and must: the band
    // travels depth * sin(angle) along n before it turns, so a 90 degree lip reaches
    // a full Depth down and a 0 degree one stays at the surface. Both halves of that
    // are asserted here, because getting either wrong is a different bug.
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke = circle_on_top(12.0, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    BoundingBoxf3 bb;
    for (const Vec3f& v : cube.vertices)
        bb.merge(v.cast<double>());

    auto core_face = [&](double angle) {
        DrawCutParams params;
        params.extension   = 3.0;
        params.through_all = false;
        params.depth       = 4.0;
        params.angle_deg   = angle;
        Vec3d n, c;
        REQUIRE(draw_cut_core_face(stroke, params, bb, n, c));
        return std::make_pair(n, c);
    };

    // SQUARE at every angle: the normal is the loop's own, which on a top-face circle
    // is Z.
    for (double angle : { 0.0, 30.0, 45.0, 90.0 }) {
        const Vec3d n = core_face(angle).first;
        REQUIRE(std::abs(std::abs(n.z()) - 1.0) < 1e-9);
    }

    // AND ITS DEPTH IS depth * sin(angle) below the drawn line.
    const double top = 0.5 * CUBE;
    REQUIRE(core_face(90.0).second.z() == Approx(top - 4.0).margin(0.05));
    REQUIRE(core_face(30.0).second.z() == Approx(top - 4.0 * 0.5).margin(0.05));
    REQUIRE(core_face(0.0).second.z()  == Approx(top).margin(0.05));

    // AND THE PLUG SHRINKS AS THE LIP FLATTENS. At the same Depth a 90 degree lip
    // reaches a full Depth down with a straight wall, while a 45 degree one reaches
    // only depth * sin(45) down and takes a chamfered bite out of the shoulder on the
    // way - so it removes LESS material, not more. (The chamfer is what the halves
    // key on; it is not there to remove volume.)
    auto plug_volume = [&](double angle) {
        DrawCutParams params;
        params.extension   = 3.0;
        params.through_all = false;
        params.depth       = 4.0;
        params.angle_deg   = angle;
        indexed_triangle_set u, l;
        REQUIRE(draw_cut_split(cube, stroke, params, &u, &l, nullptr));
        return double(its_volume(u));
    };
    const double v90 = plug_volume(90.0);
    const double v45 = plug_volume(45.0);
    REQUIRE(v90 > 0.0);
    REQUIRE(v45 > 0.0);
    REQUIRE(v45 < v90);
    // The straight-walled plug is the cylinder the loop drew, to a few percent.
    REQUIRE(v90 == Approx(M_PI * 12.0 * 12.0 * 4.0).epsilon(0.06));
}

TEST_CASE("Draw cut: the cut surface does not depend on which way the loop was drawn", "[DrawCut]")
{
    // The phase-1 suite asserts this for the ruled strip; the band and core have to
    // keep it, because the core normal now comes from a Newell fit whose sign IS
    // the winding. build_core_band() pins it to the loop's own surface normals,
    // and this is the case that would catch that pinning being dropped.
    const indexed_triangle_set cube = centred_cube();

    DrawCutParams params;
    params.extension   = 3.0;
    params.through_all = false;
    params.depth       = 4.0;
    params.angle_deg   = 30.0;

    auto plug_of = [&](bool cw) {
        DrawCutStroke s = circle_on_top(10.0, 96, 0.5 * CUBE, cw);
        REQUIRE(s.finish(1.0, 0.0) == DrawCutError::None);
        indexed_triangle_set u, l;
        REQUIRE(draw_cut_split(cube, s, params, &u, &l, nullptr));
        return double(its_volume(u));
    };

    REQUIRE(plug_of(false) == Approx(plug_of(true)).epsilon(0.02));
}

TEST_CASE("Draw cut: connectors stand on the band and on the core", "[DrawCut]")
{
    // Phase 4's contract, against the new surface: a point on the band projects
    // back to the (s, w) it was built from, and the frame there is perpendicular to
    // the surface the boolean will actually make.
    DrawCutStroke stroke = wavy_loop_on_cylinder(20.0, 0.0, 3.0);
    REQUIRE(stroke.finish(1.0, 0.1) == DrawCutError::None);

    DrawCutParams params;
    params.extension   = 3.0;
    params.through_all = false;
    params.depth       = 5.0;
    params.angle_deg   = 45.0;

    const double s_mid = 0.25 * stroke.length();

    for (double w : { 0.0, 1.5, 3.0 }) {
        const Vec3d p = draw_cut_surface_point(stroke, params, s_mid, w);

        double s_back = 0.0, w_back = 0.0, dist = 0.0;
        REQUIRE(draw_cut_surface_project(stroke, params, p, s_back, w_back, &dist));
        REQUIRE(dist == Approx(0.0).margin(0.2));
        REQUIRE(w_back == Approx(w).margin(0.2));

        // The frame's Z is the surface normal, and it is a genuine rotation.
        const Transform3d f = draw_cut_surface_frame(stroke, params, s_mid, w);
        const Matrix3d    m = f.linear();
        REQUIRE(std::abs(m.determinant() - 1.0) < 1e-6);
        const Vec3d fz = m.col(2);
        REQUIRE(fz.norm() == Approx(1.0).margin(1e-6));
        // Perpendicular to the direction the band travels, which is what "standing
        // on the cut surface" means.
        const Vec3d p_a = draw_cut_surface_point(stroke, params, s_mid, w);
        const Vec3d p_b = draw_cut_surface_point(stroke, params, s_mid, w + 0.5);
        REQUIRE(std::abs(fz.dot((p_b - p_a).normalized())) < 0.05);
    }
}

TEST_CASE("Draw cut: an open line still uses the ruled strip", "[DrawCut]")
{
    // Phase 3 is a CLOSED-loop model: an open line has no interior, so there is no
    // core plane to put anywhere and the phase-1 strip is still the right surface.
    // This is the case that would catch the band being applied where it cannot be.
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke = line_on_top(30.0, 60);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE_FALSE(stroke.is_closed());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 5.0;
    params.through_all = true;

    Vec3d n, c;
    REQUIRE_FALSE(draw_cut_core_plane(stroke, params, n, c));

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    REQUIRE(double(its_volume(upper)) + double(its_volume(lower)) ==
            Approx(double(its_volume(cube))).epsilon(1e-3));
}

TEST_CASE("Draw cut: a loop round the part separates it, a loop on a face does not", "[DrawCut]")
{
    // THE DISCRIMINATOR behind Through all's two shapes, tested on its own so a
    // failure here is not confused with a failure of the cut that uses it.
    //
    // The bounding box cannot answer this - a cylinder's bbox corners are at
    // r * sqrt(2), further out than any loop drawn on the barrel - which is why the
    // test is the SECTION at the core plane against the loop's projection.

    // A loop ROUND a cylinder's barrel: the section at that height is the full disc,
    // and the loop contains all of it.
    {
        const indexed_triangle_set cyl = centred_cylinder(20.0, 60.0);
        DrawCutStroke round_it = wavy_loop_on_cylinder(20.0, 0.0, 0.0, 96, 1);
        REQUIRE(round_it.finish(1.0, 0.0) == DrawCutError::None);

        DrawCutParams params;
        params.through_all = true;
        REQUIRE(draw_cut_loop_separates(cyl, round_it, params));
    }

    // A small loop drawn ON the cylinder's flat top: the section at the core plane is
    // the whole disc and the loop covers only a fraction of it.
    {
        const indexed_triangle_set cyl = centred_cylinder(20.0, 60.0);
        DrawCutStroke on_top = circle_on_top(6.0, 96, 30.0);
        REQUIRE(on_top.finish(1.0, 0.0) == DrawCutError::None);

        DrawCutParams params;
        params.through_all = true;
        REQUIRE_FALSE(draw_cut_loop_separates(cyl, on_top, params));
    }

    // A loop on a cube's top face - the plug case the phase 1 suite uses.
    {
        const indexed_triangle_set cube = centred_cube();
        DrawCutStroke ring = circle_on_top(10.0, 96);
        REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

        DrawCutParams params;
        params.through_all = true;
        REQUIRE_FALSE(draw_cut_loop_separates(cube, ring, params));
    }

    // An OPEN line is never a separation in this sense - it has no interior at all.
    {
        const indexed_triangle_set cube = centred_cube();
        DrawCutStroke line = line_on_top(30.0, 60);
        REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);

        DrawCutParams params;
        params.through_all = true;
        REQUIRE_FALSE(draw_cut_loop_separates(cube, line, params));
    }
}

TEST_CASE("Draw cut: Through all round a cylinder cuts it in two", "[DrawCut]")
{
    // The wrap-around case, on the article the owner drew on. A loop round the
    // barrel with Through all is a SEPARATION: two stacked halves, not a plug and a
    // shell.
    const indexed_triangle_set cyl = centred_cylinder(20.0, 60.0);
    const double cyl_volume = double(its_volume(cyl));

    DrawCutStroke loop = wavy_loop_on_cylinder(20.0, 0.0, 0.0, 96, 1);
    REQUIRE(loop.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.extension   = 3.0;
    params.through_all = true;
    // THROUGH ALL'S ANGLE ZERO IS THE STRAIGHT WALL. With no core plane there is no
    // shelf for 0 to lie in, so the angle means the wall's TAPER and 0 is the plain
    // straight-through cut - the opposite end from the band, where 0 is the flat
    // shelf and 90 the straight wall. (draw_cut_band_core_solid() says why: reading
    // it the band's way would make 0 an infinite taper.)
    params.angle_deg   = 0.0;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cyl, loop, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    const double a = double(its_volume(upper));
    const double b = double(its_volume(lower));
    REQUIRE(a > 0.0);
    REQUIRE(b > 0.0);
    REQUIRE(a + b == Approx(cyl_volume).epsilon(0.01));
    // The loop is at mid-height, so the two halves are about equal.
    REQUIRE(a == Approx(0.5 * cyl_volume).epsilon(0.2));

    // STACKED, not nested. This loop is PLANAR (amplitude 0), so unlike the wavy case
    // the two halves really do meet on one flat plane and the extremes can be
    // compared directly.
    BoundingBoxf3 bu, bl;
    for (const Vec3f& v : upper.vertices) bu.merge(v.cast<double>());
    for (const Vec3f& v : lower.vertices) bl.merge(v.cast<double>());
    INFO("upper z [" << bu.min.z() << ", " << bu.max.z() << "]  lower z ["
         << bl.min.z() << ", " << bl.max.z() << "]  vol " << a << " / " << b);

    // ONE HALF IS A CLEAN SLAB, and the other's BOUNDING BOX is not the right
    // instrument for the second.
    //
    // The cut is a half-space, so `upper` is the part below the line: a slab from the
    // cylinder's bottom up to the loop, and its bbox says exactly that. `lower` is the
    // rest, and a bounding box of "the rest" still spans the whole cylinder whichever
    // way the part was cut - so comparing the two bboxes cannot distinguish a
    // separation from anything else, and asserting they do not overlap asks the
    // complement to be something it never is.
    //
    // What the separation actually claims is checked instead: one half is the slab
    // below the line, and the two volumes are the two sides of one wall.
    REQUIRE(bu.max.z() == Approx(0.0).margin(1.0));
    REQUIRE(bu.min.z() == Approx(-30.0).margin(0.5));
    // Between them they span the whole cylinder.
    REQUIRE(std::max(bu.max.z(), bl.max.z()) == Approx(+30.0).margin(0.5));
    REQUIRE(std::min(bu.min.z(), bl.min.z()) == Approx(-30.0).margin(0.5));
}

TEST_CASE("Draw cut: the inside field follows the band and core surface", "[DrawCut]")
{
    // The halves colouring, Visible/Ghost/Hidden and the connectors all read this
    // field, so it has to agree with the surface the boolean used - otherwise the
    // preview shows one cut and the result is another, which is exactly the
    // translucent-disc symptom the owner reported.
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke = circle_on_top(10.0, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.extension   = 3.0;
    params.through_all = false;
    params.depth       = 4.0;
    params.angle_deg   = 90.0;

    BoundingBoxf3 bb;
    for (const Vec3f& v : cube.vertices)
        bb.merge(v.cast<double>());

    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bb, 0.0);
    REQUIRE_FALSE(cutter.empty());
    REQUIRE(watertight(cutter));

    // A point well inside the plug, and one well outside it.
    REQUIRE(draw_cut_classify_upper(cutter, true, Vec3d(0.0, 0.0, 0.5 * CUBE - 2.0)));
    REQUIRE_FALSE(draw_cut_classify_upper(cutter, true, Vec3d(0.0, 0.0, 0.5 * CUBE - 10.0)));
    REQUIRE_FALSE(draw_cut_classify_upper(cutter, true, Vec3d(18.0, 0.0, 0.5 * CUBE - 2.0)));
}

// ---------------------------------------------------------------------------
// THE REAL CYLINDER. 2026-09-13, owner click-test of the flat-core cut.
//
// Everything above this line builds its strokes by hand on a mesh built by hand:
// 96 clean samples, exact radii, no jitter, a symmetric wave that cancels, and
// a mesh that already sits in the cut plane's frame. The gizmo produces none of
// those things, which is why the suite could pass 68/68 while the feature was
// unusable on the owner's screen.
//
// This fixture is the gizmo's own article and the gizmo's own pipeline:
//
//   - the mesh comes out of tests/data/cylinder_drawcut.3mf via Model::read_from_file
//     and is transformed the way GLGizmoCut3D::curved_instance_mesh_in_plane() does
//     (instance matrix * volume matrix, then world -> plane). The 3mf instance
//     carries a 2.88 UNIFORM SCALE over a r = 13.5 / h = 27 primitive, so the mesh
//     the gizmo hands to DrawCut is r ~= 38.9 / h ~= 77.8, not the r = 13.5 stored
//     in the file. A fixture built from the raw volume mesh tests a different part;
//   - the stroke is hundreds of samples at a fraction of a millimetre spacing, each
//     PROJECTED ONTO THE MESH (AABBMesh closest point) the way MeshRaycaster does -
//     so a point lands on a FACET, i.e. slightly inside the true radius on a faceted
//     barrel - with the facet own normal and a little jitter, wandering in z and
//     with one inward dent. A hand-drawn line, not a parametric curve.
// ---------------------------------------------------------------------------

// The cylinder from the 3mf, in the CUT PLANE frame, exactly as the gizmo
// composes it: world_to_plane * instance * volume, with the plane at the object
// centre and unrotated (the gizmo default when the user opens the tool).
static indexed_triangle_set cylinder_3mf_in_plane(Vec3d* plane_centre = nullptr)
{
    static indexed_triangle_set cached;
    static Vec3d                cached_centre = Vec3d::Zero();
    if (cached.empty()) {
        Model model;
        const std::string path = std::string(TEST_DATA_DIR) + "/cylinder_drawcut.3mf";
        DynamicPrintConfig cfg;
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::EnableSilent);
        bool ok = false;
        try {
            // LoadModel is not implied by AddDefaultInstances: without it the 3mf
            // importer parses the plate map and drops every object ("can not find
            // object from plate's obj_map"), leaving an empty Model. Silence keeps the
            // importer's own progress chatter out of the test log.
            model = Model::read_from_file(path, &cfg, &ctx,
                                          LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances |
                                          LoadStrategy::Silence);
            ok = !model.objects.empty();
        } catch (const std::exception&) {
            ok = false;
        }
        REQUIRE(ok);

        const ModelObject* mo = model.objects.front();
        REQUIRE(!mo->instances.empty());
        const Transform3d inst = mo->instances.front()->get_transformation().get_matrix();

        indexed_triangle_set merged;
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part() || mv->mesh().empty())
                continue;
            indexed_triangle_set part = mv->mesh().its;
            its_transform(part, inst * mv->get_matrix());
            its_merge(merged, part);
        }
        REQUIRE_FALSE(merged.empty());

        // The plane centre: the object bbox centre, which is where the gizmo drops
        // the plane when the tool opens. The rotation is identity there.
        BoundingBoxf3 bb;
        for (const Vec3f& v : merged.vertices)
            bb.merge(v.cast<double>());
        cached_centre = bb.center();
        for (Vec3f& v : merged.vertices)
            v = (v.cast<double>() - cached_centre).cast<float>();
        cached = std::move(merged);
    }
    if (plane_centre != nullptr)
        *plane_centre = cached_centre;
    return cached;
}

// The mean barrel radius of the 3mf cylinder in the plane frame - taken from the
// mesh rather than hard-coded, so the fixture follows the file.
static double cylinder_3mf_radius(const indexed_triangle_set& cyl)
{
    double sum = 0.0;
    size_t n = 0;
    for (const Vec3f& v : cyl.vertices) {
        const double r = std::hypot(double(v.x()), double(v.y()));
        if (r > 1e-6) { sum += r; ++ n; }
    }
    return n > 0 ? sum / double(n) : 0.0;
}

static double cylinder_3mf_half_height(const indexed_triangle_set& cyl)
{
    double h = 0.0;
    for (const Vec3f& v : cyl.vertices)
        h = std::max(h, std::abs(double(v.z())));
    return h;
}

// A deterministic little PRNG, so the hand-drawn jitter is the same every run.
struct TinyRand
{
    uint32_t s{ 0x13572468u };
    double   next() { s = s * 1664525u + 1013904223u; return double(s >> 8) / double(1 << 24) - 0.5; }
};

// THE OWNER LINE, as the gizmo would have captured it: 400 raw samples round the
// barrel, wandering z0 + 2.5*sin(2 theta), one ~4 mm inward dent over 30 degrees,
// 0.05 mm of jitter, each sample projected onto the mesh with the facet own normal.
static DrawCutStroke owner_loop_on_3mf_cylinder(const indexed_triangle_set& cyl,
                                                double r, double z0,
                                                int n = 400, double jitter = 0.05,
                                                double dent_mm = 4.0)
{
    // AABBMesh keeps a RAW POINTER to the mesh it was built from (m_tm), so it must
    // outlive the AABBMesh: building one from a TriangleMesh temporary dangles and
    // segfaults on the first query. Bind the mesh to a named local.
    const TriangleMesh tm(cyl);
    AABBMesh aabb{ tm };
    TinyRand rnd;

    DrawCutStroke stroke;
    for (int i = 0; i < n; ++ i) {
        const double th = 2.0 * M_PI * double(i) / double(n);
        // The dent: ~4 mm in, over 30 degrees centred on theta = 200 deg.
        const double dent_c = 200.0 * M_PI / 180.0;
        double dd = th - dent_c;
        while (dd >  M_PI) dd -= 2.0 * M_PI;
        while (dd < -M_PI) dd += 2.0 * M_PI;
        const double half = 15.0 * M_PI / 180.0;
        const double dent = std::abs(dd) < half ? dent_mm * 0.5 * (1.0 + std::cos(M_PI * dd / half)) : 0.0;

        const double rr = r - dent + jitter * rnd.next();
        const double z  = z0 + 2.5 * std::sin(2.0 * th) + jitter * rnd.next();
        const Vec3d  q(rr * std::cos(th), rr * std::sin(th), z);

        // Project onto the mesh the way the gizmo raycaster does.
        Vec3d hit = q;
        Vec3d nrm(std::cos(th), std::sin(th), 0.0);
        {
            int fi = -1;
            Vec3d cpt = q;
            aabb.squared_distance(q, fi, cpt);
            if (fi >= 0) {
                hit = cpt;
                const Vec3i32& f = cyl.indices[size_t(fi)];
                const Vec3d a = cyl.vertices[size_t(f(0))].cast<double>();
                const Vec3d b = cyl.vertices[size_t(f(1))].cast<double>();
                const Vec3d c = cyl.vertices[size_t(f(2))].cast<double>();
                const Vec3d fn = (b - a).cross(c - a);
                if (fn.norm() > 1e-12)
                    nrm = fn.normalized();
            }
        }
        stroke.append(hit, nrm, size_t(i));
    }
    // Come back to the start, the way a hand-drawn loop closes.
    stroke.append(stroke.samples().front().pos, stroke.samples().front().normal, 0);
    return stroke;
}

// The stroke the gizmo hands to DrawCut: chain -> finish -> re-project onto the
// mesh (the gizmo reproject_draw_path_onto_mesh, which runs AFTER finish because
// smoothing is what moves the samples off the surface).
static DrawCutStroke finish_like_gizmo(const DrawCutStroke& raw, const indexed_triangle_set& cyl,
                                       double smoothing = 0.2)
{
    DrawCutChain chain;
    chain.set_samples(raw.samples(), /*closed*/ true);

    DrawCutStroke out;
    REQUIRE(chain.finish(out, DrawCutStroke::DefaultSpacing, smoothing) == DrawCutError::None);
    REQUIRE(out.is_closed());

    // The gizmo re-projection, which libslic3r own smoother cannot do.
    // AABBMesh keeps a RAW POINTER to the mesh it was built from (m_tm), so it must
    // outlive the AABBMesh: building one from a TriangleMesh temporary dangles and
    // segfaults on the first query. Bind the mesh to a named local.
    const TriangleMesh tm(cyl);
    AABBMesh aabb{ tm };
    std::vector<DrawCutSample> path = out.path();
    for (DrawCutSample& s : path) {
        int fi = -1;
        Vec3d cpt = s.pos;
        aabb.squared_distance(s.pos, fi, cpt);
        if (fi < 0)
            continue;
        s.pos = cpt;
        const Vec3i32& f = cyl.indices[size_t(fi)];
        const Vec3d a = cyl.vertices[size_t(f(0))].cast<double>();
        const Vec3d b = cyl.vertices[size_t(f(1))].cast<double>();
        const Vec3d c = cyl.vertices[size_t(f(2))].cast<double>();
        const Vec3d fn = (b - a).cross(c - a);
        if (fn.norm() > 1e-12)
            s.normal = fn.normalized();
        s.facet = size_t(fi);
    }
    out.set_path(path);
    return out;
}

// The owner parameters for screenshots 1-4: Angle 30, Depth 3, Extension on,
// Through all OFF.
static DrawCutParams owner_params()
{
    DrawCutParams p;
    p.direction   = DrawCutDirection::SurfaceNormal;
    p.extension   = 5.0;
    p.angle_deg   = 30.0;
    p.depth       = 3.0;
    p.through_all = false;
    return p;
}

TEST_CASE("Draw cut: the 3mf cylinder fixture is the part the gizmo sees", "[DrawCut]")
{
    Vec3d centre;
    const indexed_triangle_set cyl = cylinder_3mf_in_plane(&centre);
    REQUIRE(watertight(cyl));

    // The 3mf instance carries a 2.88 uniform scale over a r = 13.5 / h = 27
    // primitive, so the part the gizmo works on is ~78 mm across and ~78 tall.
    const double r = cylinder_3mf_radius(cyl);
    const double h = cylinder_3mf_half_height(cyl);
    INFO("radius " << r << " half height " << h);
    REQUIRE(r > 30.0);
    REQUIRE(r < 45.0);
    REQUIRE(h == Approx(r).epsilon(0.05));
}

TEST_CASE("Draw cut: a dense hand-drawn loop round the 3mf cylinder does not fold", "[DrawCut]")
{
    const indexed_triangle_set cyl = cylinder_3mf_in_plane();
    const double r = cylinder_3mf_radius(cyl);

    const DrawCutStroke raw = owner_loop_on_3mf_cylinder(cyl, r, 0.0);
    const DrawCutStroke stroke = finish_like_gizmo(raw, cyl);

    // The line is long (a circumference of ~245 mm at 1 mm spacing), which is where
    // the hand-built 96-sample fixtures stop resembling it.
    REQUIRE(stroke.path().size() > 150);
    REQUIRE_FALSE(draw_cut_self_crossing(stroke));

    const DrawCutParams params = owner_params();

    // SYMPTOM 4: the gizmo fold warning. draw_cut_strip_folds() is a RULED STRIP
    // test, and the closed-loop path has not used a ruled strip since phase 3 - so
    // running it on a dense jittery loop scores the jitter own curvature (a fraction
    // of a millimetre between samples is a curvature of several 1/mm) and the warning
    // fires on a perfectly gentle line.
    double kappa = 0.0;
    const bool folds = draw_cut_band_folds(stroke, params);
    INFO("worst kappa " << kappa);
    REQUIRE_FALSE(folds);
}

TEST_CASE("Draw cut: the core of a real loop is one triangulated plate", "[DrawCut]")
{
    // SYMPTOM 1: the preview core had a PIE WEDGE MISSING.
    const indexed_triangle_set cyl = cylinder_3mf_in_plane();
    const double r = cylinder_3mf_radius(cyl);

    const DrawCutStroke stroke = finish_like_gizmo(owner_loop_on_3mf_cylinder(cyl, r, 0.0), cyl);
    const DrawCutParams params = owner_params();

    BoundingBoxf3 bb;
    for (const Vec3f& v : cyl.vertices)
        bb.merge(v.cast<double>());

    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bb, 0.0, &cyl);
    REQUIRE_FALSE(cutter.empty());
    REQUIRE(watertight(cutter));

    Vec3d n, c;
    REQUIRE(draw_cut_core_face(stroke, params, bb, n, c));

    // THE CORE AREA. The core is the loop inset by depth * cos(angle) in the plane,
    // so on a barrel of radius r the core is a disc of radius r - depth*cos(angle),
    // and the triangles that lie ON the core plane must add up to that area. A
    // missing wedge shows here as a shortfall.
    const double inset   = params.depth * std::cos(params.angle_deg * M_PI / 180.0);
    const double expect  = M_PI * (r - inset) * (r - inset);

    double core_area = 0.0;
    for (const Vec3i32& f : cutter.indices) {
        const Vec3d a = cutter.vertices[size_t(f(0))].cast<double>();
        const Vec3d b = cutter.vertices[size_t(f(1))].cast<double>();
        const Vec3d d = cutter.vertices[size_t(f(2))].cast<double>();
        if (std::abs((a - c).dot(n)) > 0.05 || std::abs((b - c).dot(n)) > 0.05 ||
            std::abs((d - c).dot(n)) > 0.05)
            continue;
        core_area += 0.5 * (b - a).cross(d - a).norm();
    }
    INFO("core area " << core_area << " expected " << expect << " (r " << r << " inset " << inset << ")");
    REQUIRE(core_area == Approx(expect).epsilon(0.02));
}

TEST_CASE("Draw cut: the band of a real loop follows the drawn wave", "[DrawCut]")
{
    // SYMPTOM 2: the coloured surface after the cut was a flat horizontal slab with
    // stair-stepped edges, not the wavy line the user drew.
    //
    // THE CONTRACT THIS PINS: the drawn line is where the cut meets the skin, EXACTLY,
    // and that must not depend on Extension. The first fix left the drawn line off the
    // surface - the band was one loft from the skirt tip straight to the flat inner
    // ring - so the wave arrived at the skin interpolated down by E / (E + inset):
    // about a third of it at E 5, and less the more Extension was asked for. Running
    // the whole case at TWO very different Extensions is what makes that a test rather
    // than a number to be re-tuned: a damped surface cannot pass both.
    const indexed_triangle_set cyl = cylinder_3mf_in_plane();
    const double r = cylinder_3mf_radius(cyl);

    const DrawCutStroke stroke = finish_like_gizmo(owner_loop_on_3mf_cylinder(cyl, r, 0.0), cyl);

    BoundingBoxf3 bb;
    for (const Vec3f& v : cyl.vertices)
        bb.merge(v.cast<double>());

    // THE DRAWN LINE ITSELF waves as it was drawn - this is the input, and it is
    // asserted first so a failure below cannot be blamed on the fixture.
    {
        double worst = 0.0;
        for (const DrawCutSample& s : stroke.path()) {
            const double th = std::atan2(s.pos.y(), s.pos.x());
            worst = std::max(worst, std::abs(s.pos.z() - 2.5 * std::sin(2.0 * th)));
        }
        INFO("worst wave deviation on the drawn line " << worst);
        REQUIRE(worst < 0.3);
    }

    for (const double ext : { 5.0, 15.0 }) {
        DrawCutParams params = owner_params();
        params.extension = ext;

        INFO("Extension " << ext << " mm");

        Vec3d n, c;
        REQUIRE(draw_cut_core_face(stroke, params, bb, n, c));
        // The core normal is the barrel axis for a loop round it.
        REQUIRE(std::abs(std::abs(n.z()) - 1.0) < 0.05);

        // THE CORE PLATE SITS AT THE LIP ANGLE'S DROP BELOW THE LINE'S MEAN, and that
        // too is independent of Extension: the band travels Depth from the LINE, not
        // from the skirt tip, so the inner ring is the inset of the drawn line.
        const double drop = params.depth * std::sin(params.angle_deg * M_PI / 180.0);
        double mean_z = 0.0;
        for (const DrawCutSample& s : stroke.path())
            mean_z += s.pos.z();
        mean_z /= double(stroke.path().size());
        INFO("core sits " << (c.z() - mean_z) << " from the line mean, expected " << -drop);
        REQUIRE((c.z() - mean_z) == Approx(-drop).margin(0.25));

        // THE CUT MEETS THE SKIN ON THE DRAWN LINE. Split the part and walk the piece
        // BELOW the line: at every angle round the barrel its highest skin vertex is
        // where the cut surface crossed the skin, and that must be the drawn wave
        // itself - z0 + 2.5*sin(2 theta) - to within 0.3 mm, at BOTH Extensions.
        indexed_triangle_set half_u, half_l;
        REQUIRE(draw_cut_split(cyl, stroke, params, &half_u, &half_l, nullptr));
        REQUIRE(watertight(half_u));
        REQUIRE(watertight(half_l));

        // THE COMPARISON IS PER VERTEX, AT THAT VERTEX'S OWN ANGLE. Binning the skin
        // vertices and comparing each bin's maximum against the wave at the BIN'S
        // CENTRE angle - the obvious way to do this - builds in an error of its own:
        // the maximum within a bin is taken wherever inside it the wave is highest, so
        // on a rising stretch it is at the bin's far edge, and 2.5*sin(2 theta) moves
        // 5 * (pi/36) = 0.44 mm over half a 10-degree bin. That is the whole tolerance,
        // spent on the instrument rather than on the surface.
        //
        // Bins are still used, but only to prove COVERAGE - that the boundary was found
        // all the way round rather than on one arc.
        const int bins = 36;
        std::vector<bool> seen(size_t(bins), false);

        // The boundary at an angle is the TOP of the piece below the line. Bin finely,
        // keep the highest vertex in each bin, and then compare THAT VERTEX against the
        // wave at ITS OWN theta rather than at the bin's centre - so the bin decides
        // only which vertex to look at, and contributes no error of its own to the
        // comparison.
        struct SkinV { double th, z; bool any; };
        const int fine = 180;                                   // 2 degrees per bin
        std::vector<SkinV> best(size_t(fine), SkinV{ 0.0, -1e9, false });
        size_t n_skin = 0;
        for (const Vec3f& v : half_u.vertices) {
            const double rad = std::hypot(double(v.x()), double(v.y()));
            if (rad < r - 1.0)
                continue;                 // not on the barrel skin
            double th = std::atan2(double(v.y()), double(v.x()));
            if (th < 0.0) th += 2.0 * M_PI;
            ++ n_skin;
            const size_t f = std::min(size_t(fine - 1), size_t(th / (2.0 * M_PI) * fine));
            if (double(v.z()) > best[f].z)
                best[f] = SkinV{ th, double(v.z()), true };
        }
        REQUIRE(n_skin > 100);

        double worst = 0.0, tmin = 1e9, tmax = -1e9;
        for (const SkinV& a : best) {
            if (!a.any)
                continue;
            seen[std::min(size_t(bins - 1), size_t(a.th / (2.0 * M_PI) * bins))] = true;
            tmin = std::min(tmin, a.z);
            tmax = std::max(tmax, a.z);
            worst = std::max(worst, std::abs(a.z - 2.5 * std::sin(2.0 * a.th)));
        }
        size_t filled = 0;
        for (bool s2 : seen)
            if (s2) ++ filled;
        INFO("skin boundary over " << filled << " bins: " << tmin << " .. " << tmax
             << ", worst deviation from the drawn wave " << worst);
        REQUIRE(filled > 30);
        // 100% OF THE DRAWN WAVE, at either Extension. A damped surface shows here as a
        // deviation of the damped-away fraction: at E 5 the old code lost about 1.7 mm
        // of the 5 mm, and at E 15 nearly all of it.
        REQUIRE(worst < 0.3);
        // And the full amplitude is present, stated the other way round so a surface
        // that happened to sit low everywhere could not pass on the deviation alone.
        REQUIRE(tmax - tmin == Approx(5.0).margin(0.4));
    }
}

TEST_CASE("Draw cut: a real wrap-around loop separates the 3mf cylinder", "[DrawCut]")
{
    // SYMPTOM 3: "The stroke does not separate the part". A loop that goes all the
    // way round the barrel DOES separate it, and both draw_cut_empty_sides() (the
    // panel cheap pre-check) and draw_cut_split() must say so.
    const indexed_triangle_set cyl = cylinder_3mf_in_plane();
    const double r  = cylinder_3mf_radius(cyl);
    const double hh = cylinder_3mf_half_height(cyl);
    const double cyl_volume = double(its_volume(cyl));

    const DrawCutStroke stroke = finish_like_gizmo(owner_loop_on_3mf_cylinder(cyl, r, 0.0), cyl);
    const DrawCutParams params = owner_params();

    bool up_empty = true, lo_empty = true;
    draw_cut_empty_sides(cyl, stroke, params, up_empty, lo_empty);
    INFO("upper empty " << up_empty << " lower empty " << lo_empty);
    REQUIRE_FALSE(up_empty);
    REQUIRE_FALSE(lo_empty);

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(cyl, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    const double a = double(its_volume(upper));
    const double b = double(its_volume(lower));
    REQUIRE(a > 0.0);
    REQUIRE(b > 0.0);
    REQUIRE(a + b == Approx(cyl_volume).epsilon(0.02));

    // THE SPLIT IS WHERE THE LINE IS. The loop sits at the cylinder mid height, so
    // the two halves are near enough equal - within 55/45.
    const double frac = std::min(a, b) / (a + b);
    INFO("volumes " << a << " / " << b << " (min fraction " << frac << ", half height " << hh << ")");
    REQUIRE(frac > 0.45);
}

TEST_CASE("Draw cut: Through all on a real loop is a straight prism, not an hourglass", "[DrawCut]")
{
    // SYMPTOM 5: the tapered wall converged to a point below the loop and re-expanded
    // into a second cone. Through all now extrudes the loop STRAIGHT along the core
    // normal in both directions - no taper, so no convergence and no hourglass.
    const indexed_triangle_set cyl = cylinder_3mf_in_plane();
    const double r = cylinder_3mf_radius(cyl);
    const double cyl_volume = double(its_volume(cyl));

    const DrawCutStroke stroke = finish_like_gizmo(owner_loop_on_3mf_cylinder(cyl, r, 0.0), cyl);

    DrawCutParams params = owner_params();
    params.through_all = true;   // Angle and Depth are ignored from here on.

    BoundingBoxf3 bb;
    for (const Vec3f& v : cyl.vertices)
        bb.merge(v.cast<double>());

    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bb, 0.0, &cyl);
    REQUIRE_FALSE(cutter.empty());
    REQUIRE(watertight(cutter));

    // NO HOURGLASS: every vertex of the cutter wall is at the radius of the loop
    // point it came from, whatever its height. A taper would show as a radius that
    // shrinks with distance from the loop; an hourglass as one that shrinks past zero
    // and grows again.
    Vec3d n, c0;
    REQUIRE(draw_cut_core_plane(stroke, params, n, c0));
    double r_min = std::numeric_limits<double>::max(), r_max = 0.0;
    for (const Vec3f& v : cutter.vertices) {
        const Vec3d q = v.cast<double>() - c0;
        const double rad = (q - q.dot(n) * n).norm();
        // The two end caps are fanned from their rings' own centroids, which sit ON
        // the axis - so they are near zero radius rather than exactly zero, and a
        // 1e-6 guard lets them through and makes r_min meaningless. The WALL is what
        // is being measured; anything within half the loop's radius of the axis is
        // not on it.
        if (rad < 0.5 * r)
            continue;
        r_min = std::min(r_min, rad);
        r_max = std::max(r_max, rad);
    }
    INFO("cutter wall radius " << r_min << " .. " << r_max << " (loop r " << r << ")");
    REQUIRE(r_min > 0.8 * r);
    REQUIRE(r_max < 1.2 * r);

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cyl, stroke, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    const double a = double(its_volume(upper));
    const double b = double(its_volume(lower));
    REQUIRE(a + b == Approx(cyl_volume).epsilon(0.02));
    const double frac = std::min(a, b) / (a + b);
    INFO("through-all volumes " << a << " / " << b << " frac " << frac);
    REQUIRE(frac > 0.48);
}



// ---------------------------------------------------------------------------
// THE STANFORD BUNNY. 2026-09-14/15 owner click-test, item 3: "a normal draw cut
// cuts beyond the Extension - the band that angles from the drawn line to the flat
// core keeps going and a plate far outside the loop split the whole bunny body in
// two."
//
// Every fixture above this point is CONVEX at the core plane: a cube, a cylinder.
// On a convex part "inward" (towards the loop's own axis) stays inside the loop's
// own footprint all the way to the core, so a band that travels inward can never
// emerge somewhere else on the part. The bunny is not convex, and a loop on its
// HEAD has the neck, the body and the ears in the same core-plane section - which
// is exactly the configuration that broke.
// ---------------------------------------------------------------------------

// The bunny, in the CUT PLANE frame, composed the way cylinder_3mf_in_plane() does:
// instance matrix * volume matrix, then recentred on the object bbox centre (which
// is where the gizmo drops the plane when the tool opens).
static indexed_triangle_set bunny_in_plane(Vec3d* plane_centre = nullptr)
{
    static indexed_triangle_set cached;
    static Vec3d                cached_centre = Vec3d::Zero();
    if (cached.empty()) {
        Model model;
        const std::string path = std::string(TEST_DATA_DIR) +
                                 "/../../resources/handy_models/Stanford_Bunny.3mf";
        DynamicPrintConfig cfg;
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::EnableSilent);
        bool ok = false;
        try {
            model = Model::read_from_file(path, &cfg, &ctx,
                                          LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances |
                                          LoadStrategy::Silence);
            ok = !model.objects.empty();
        } catch (const std::exception&) {
            ok = false;
        }
        REQUIRE(ok);

        const ModelObject* mo = model.objects.front();
        REQUIRE(!mo->instances.empty());
        const Transform3d inst = mo->instances.front()->get_transformation().get_matrix();

        indexed_triangle_set merged;
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part() || mv->mesh().empty())
                continue;
            indexed_triangle_set part = mv->mesh().its;
            its_transform(part, inst * mv->get_matrix());
            its_merge(merged, part);
        }
        REQUIRE_FALSE(merged.empty());

        BoundingBoxf3 bb;
        for (const Vec3f& v : merged.vertices)
            bb.merge(v.cast<double>());
        cached_centre = bb.center();
        for (Vec3f& v : merged.vertices)
            v = (v.cast<double>() - cached_centre).cast<float>();
        cached = std::move(merged);
    }
    if (plane_centre != nullptr)
        *plane_centre = cached_centre;
    return cached;
}

// A LOOP ON A SURFACE, captured EXACTLY the way the gizmo captures one: a ray per
// sample, fired from outside the part along the view direction, keeping the FIRST
// hit with its facet's own normal. That is MeshRaycaster's job, and it is why a
// sample always lands on real front-facing material.
//
// A closest-point projection was tried first and is not the same thing at all: a
// ring of plane points pushed onto a bunny by nearest-surface snaps to whatever is
// nearest, which on a concave stretch is a facet round the far side of an ear - and
// the stroke then "jumps across empty space" and gets truncated by finish(). The
// user's mouse cannot do that, so neither should the fixture.
//
// `centre` is a point on (or just inside) the surface to draw around, `axis` the
// direction the user is looking ALONG (so the eye is at centre - axis * far).
static DrawCutStroke loop_on_surface(const indexed_triangle_set& mesh,
                                     const Vec3d& centre, const Vec3d& axis,
                                     double radius, int n = 160)
{
    const TriangleMesh tm(mesh);
    AABBMesh aabb{ tm };

    BoundingBoxf3 bb;
    for (const Vec3f& v : mesh.vertices)
        bb.merge(v.cast<double>());
    // NOT `far`: that is still a reserved memory-model keyword under MSVC and the
    // declaration silently becomes "const double = ..." with no variable at all.
    const double away = 2.0 * bb.size().norm() + 10.0;

    const Vec3d a  = axis.normalized();
    const Vec3d e1 = (std::abs(a.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitX()).cross(a).normalized();
    const Vec3d e2 = a.cross(e1);

    DrawCutStroke stroke;
    for (int i = 0; i < n; ++ i) {
        const double th = 2.0 * M_PI * double(i) / double(n);
        // The eye ray for this pixel: parallel projection along `a`, offset by the
        // ring's own radius in the view plane.
        const Vec3d src = centre + radius * (std::cos(th) * e1 + std::sin(th) * e2) - away * a;

        const AABBMesh::hit_result hit = aabb.query_ray_hit(src, a);
        if (!(hit.distance() < std::numeric_limits<double>::infinity()) || hit.face() < 0)
            continue;
        const size_t fi = size_t(hit.face());
        const Vec3i32& f = mesh.indices[fi];
        const Vec3d v0 = mesh.vertices[size_t(f(0))].cast<double>();
        const Vec3d v1 = mesh.vertices[size_t(f(1))].cast<double>();
        const Vec3d v2 = mesh.vertices[size_t(f(2))].cast<double>();
        Vec3d nrm = (v1 - v0).cross(v2 - v0);
        nrm = nrm.norm() > 1e-12 ? nrm.normalized() : -a;
        // Front-facing: the gizmo only ever draws on skin that faces the user.
        if (nrm.dot(a) > 0.0)
            nrm = -nrm;
        stroke.append(hit.position(), nrm, fi);
    }
    if (!stroke.samples().empty())
        stroke.append(stroke.samples().front().pos, stroke.samples().front().normal,
                      stroke.samples().front().facet);
    return stroke;
}

// The shortest distance from `p` to the closed stroke polyline - the quantity
// "farther than Extension from the drawn line" is measured with.
static double dist_to_stroke(const DrawCutStroke& stroke, const Vec3d& p)
{
    const std::vector<DrawCutSample>& path = stroke.path();
    double best = std::numeric_limits<double>::max();
    const size_t n = path.size();
    for (size_t i = 0; i < n; ++ i) {
        const Vec3d& a  = path[i].pos;
        const Vec3d& b  = path[(i + 1) % n].pos;
        const Vec3d  ab = b - a;
        const double L2 = ab.squaredNorm();
        const double t  = L2 > 1e-18 ? std::clamp((p - a).dot(ab) / L2, 0.0, 1.0) : 0.0;
        best = std::min(best, (a + t * ab - p).norm());
    }
    return best;
}

// The bunny's skull: the centroid of the vertices in the 55th..72nd percentile of
// height, which is the head proper - below the ear tips, above the shoulders.
// Taken from the mesh rather than hard-coded, so the fixture follows the file.
static Vec3d bunny_head_point(const indexed_triangle_set& bunny)
{
    std::vector<double> zs;
    zs.reserve(bunny.vertices.size());
    for (const Vec3f& v : bunny.vertices)
        zs.push_back(double(v.z()));
    std::sort(zs.begin(), zs.end());
    const double z_lo = zs[size_t(0.55 * double(zs.size()))];
    const double z_hi = zs[size_t(0.72 * double(zs.size()))];

    Vec3d  c = Vec3d::Zero();
    size_t k = 0;
    for (const Vec3f& v : bunny.vertices) {
        const double z = double(v.z());
        if (z >= z_lo && z <= z_hi) { c += v.cast<double>(); ++ k; }
    }
    REQUIRE(k > 0);
    return c / double(k);
}

TEST_CASE("Draw cut: the bunny fixture loads and is the part the gizmo sees", "[DrawCut]")
{
    const indexed_triangle_set bunny = bunny_in_plane();
    REQUIRE_FALSE(bunny.empty());
    REQUIRE(bunny.vertices.size() > 1000);

    BoundingBoxf3 bb;
    for (const Vec3f& v : bunny.vertices)
        bb.merge(v.cast<double>());
    INFO("bunny bbox " << bb.size().x() << " x " << bb.size().y() << " x " << bb.size().z());
    REQUIRE(bb.size().norm() > 10.0);
    REQUIRE(bb.center().norm() < 1e-6);
}

TEST_CASE("Draw cut: a loop on the bunny's head cuts nothing beyond the Extension", "[DrawCut]")
{
    // OWNER ITEM 3. The band angled from the drawn line towards the LOOP'S OWN AXIS,
    // and on the bunny that axis is somewhere inside the skull rather than under the
    // patch - so the band ran on past the core, emerged out of the neck, and the
    // plate it capped split the whole body. Nothing outside (loop + Extension) may
    // be touched.
    const indexed_triangle_set bunny = bunny_in_plane();
    BoundingBoxf3 bb;
    for (const Vec3f& v : bunny.vertices)
        bb.merge(v.cast<double>());

    const Vec3d  head   = bunny_head_point(bunny);
    const double radius = 0.10 * bb.size().norm();
    const DrawCutStroke stroke =
        finish_like_gizmo(loop_on_surface(bunny, head, Vec3d::UnitX(), radius), bunny);
    REQUIRE(stroke.is_closed());
    REQUIRE(stroke.path().size() > 40);

    DrawCutParams params = owner_params();   // Angle 30, Depth 3, Extension 5.

    // THE LOOP IS A PATCH, NOT A BELT: it encloses a piece of the head, it does not
    // go round the bunny. The wrap-around half-space path must not fire.
    REQUIRE_FALSE(draw_cut_loop_separates(bunny, stroke, params));

    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bb, 0.0, &bunny);
    REQUIRE_FALSE(cutter.empty());
    REQUIRE(watertight(cutter));

    // THE CUTTER IS LOCAL. Extension reaches out along the band's own ruling and
    // Depth in along it, so the two together bound the reach from the drawn line;
    // 1 mm of slack absorbs the core plate's own inset.
    const double reach = params.extension + params.depth + 1.0;
    double worst = 0.0;
    for (const Vec3f& v : cutter.vertices)
        worst = std::max(worst, dist_to_stroke(stroke, v.cast<double>()));
    INFO("cutter reaches " << worst << " mm from the line, budget " << reach);
    REQUIRE(worst <= reach);

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(bunny, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // THE PLUG IS SMALL: a patch cut out of the head, not half the bunny. The bug
    // made this ~0.5 - the body cut clean in two.
    const double vol_in = double(its_volume(bunny));
    const double a = double(its_volume(upper));
    const double b = double(its_volume(lower));
    REQUIRE(a > 0.0);
    REQUIRE(b > 0.0);
    REQUIRE(a + b == Approx(vol_in).epsilon(0.02));
    const double small = std::min(a, b) / (a + b);
    INFO("volumes " << a << " / " << b << " (small fraction " << small << ")");
    REQUIRE(small < 0.2);

    // THE FAR GEOMETRY IS UNTOUCHED, triangle for triangle: every input triangle
    // whose centroid is farther than Extension + 1 mm from the line must still be
    // present, unchanged, in one of the two halves. The boolean re-tessellates
    // nothing it does not touch, so a far triangle survives bit for bit.
    auto centroids_far = [&](const indexed_triangle_set& its) {
        std::vector<Vec3d> out;
        for (const Vec3i32& f : its.indices) {
            const Vec3d c = (its.vertices[size_t(f(0))].cast<double>() +
                             its.vertices[size_t(f(1))].cast<double>() +
                             its.vertices[size_t(f(2))].cast<double>()) / 3.0;
            if (dist_to_stroke(stroke, c) > params.extension + 1.0)
                out.push_back(c);
        }
        return out;
    };
    const std::vector<Vec3d> want = centroids_far(bunny);
    REQUIRE(want.size() > 100);

    std::vector<Vec3d>       have    = centroids_far(upper);
    const std::vector<Vec3d> have_lo = centroids_far(lower);
    have.insert(have.end(), have_lo.begin(), have_lo.end());

    auto key = [](const Vec3d& c) {
        return std::make_tuple(int64_t(std::llround(c.x() * 1000.0)),
                               int64_t(std::llround(c.y() * 1000.0)),
                               int64_t(std::llround(c.z() * 1000.0)));
    };
    std::set<std::tuple<int64_t, int64_t, int64_t>> have_set;
    for (const Vec3d& c : have)
        have_set.insert(key(c));

    size_t missing = 0;
    for (const Vec3d& c : want)
        if (have_set.find(key(c)) == have_set.end())
            ++ missing;
    INFO("far triangles: " << want.size() << " in, " << missing << " missing from the halves");
    REQUIRE(double(missing) < 0.01 * double(want.size()));
}

// ---------------------------------------------------------------------------
// OWNER ITEMS 4 AND 5, 2026-09-15: the Extension angle, and the reversible band.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: the Extension angle defaults to continuing the band", "[DrawCut]")
{
    // ITEM 4's contract, and the half of it that matters most: an untouched cut does
    // not move. `extension_angle_deg` unset means "continue the band", which is -d
    // exactly - the skirt the band has always had.
    const Vec3d inward = Vec3d::UnitX();
    const Vec3d n      = Vec3d::UnitZ();

    for (double angle : { -60.0, -30.0, 0.0, 30.0, 60.0 }) {
        DrawCutParams p;
        p.angle_deg = angle;
        REQUIRE_FALSE(p.extension_angle_deg.has_value());
        REQUIRE(draw_cut_extension_angle(p) == Approx(angle));

        const Vec3d band  = draw_cut_band_dir(inward, n, angle);
        const Vec3d skirt = draw_cut_skirt_dir(inward, n, p);
        INFO("angle " << angle);
        // -d, to floating point: one straight line through the drawn point.
        REQUIRE(skirt.x() == Approx(-band.x()).margin(1e-12));
        REQUIRE(skirt.y() == Approx(-band.y()).margin(1e-12));
        REQUIRE(skirt.z() == Approx(-band.z()).margin(1e-12));
    }

    // And a value makes it independent: the skirt leaves at ITS angle, whatever the
    // band's is.
    {
        DrawCutParams p;
        p.angle_deg           = 60.0;
        p.extension_angle_deg = 0.0;   // a flat skirt, in the plane parallel to the core
        REQUIRE(draw_cut_extension_angle(p) == Approx(0.0));
        const Vec3d skirt = draw_cut_skirt_dir(inward, n, p);
        // Flat: no component along n at all, and pointing OUT (away from inward).
        REQUIRE(skirt.dot(n) == Approx(0.0).margin(1e-12));
        REQUIRE(skirt.dot(inward) == Approx(-1.0).margin(1e-12));
    }
    {
        DrawCutParams p;
        p.angle_deg           = 0.0;
        p.extension_angle_deg = 90.0;  // straight out along +n
        const Vec3d skirt = draw_cut_skirt_dir(inward, n, p);
        REQUIRE(skirt.dot(n) == Approx(1.0).margin(1e-12));
    }
}

TEST_CASE("Draw cut: the Extension angle aims the skirt on a real loop", "[DrawCut]")
{
    // The same loop cut three ways: the default skirt, a flat one and one aimed out
    // along +n. The skirt tip ring has to move where the angle says, and the cut has
    // to stay watertight in each case.
    const indexed_triangle_set cyl = cylinder_3mf_in_plane();
    const double r = cylinder_3mf_radius(cyl);
    const DrawCutStroke stroke = finish_like_gizmo(owner_loop_on_3mf_cylinder(cyl, r, 0.0), cyl);

    BoundingBoxf3 bb;
    for (const Vec3f& v : cyl.vertices)
        bb.merge(v.cast<double>());

    Vec3d n, c;
    REQUIRE(draw_cut_core_plane(stroke, owner_params(), n, c));

    // The mean height of the skirt tip above the core plane, which is what the angle
    // moves. The tip ring is the OUTERMOST ring of the cutter, so it is picked out by
    // its in-plane radius rather than by index.
    auto mean_tip_height = [&](const DrawCutParams& p) {
        const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, p, bb, 0.0, &cyl);
        REQUIRE_FALSE(cutter.empty());
        REQUIRE(watertight(cutter));
        double sum = 0.0;
        size_t k = 0;
        for (const Vec3f& v : cutter.vertices) {
            const Vec3d q = v.cast<double>() - c;
            const double rad = (q - q.dot(n) * n).norm();
            if (rad > r) { sum += q.dot(n); ++ k; }   // outside the barrel: the skirt
        }
        REQUIRE(k > 0);
        return sum / double(k);
    };

    DrawCutParams base = owner_params();   // angle 30, so the default skirt leaves at 30
    const double h_default = mean_tip_height(base);

    DrawCutParams up = base;
    up.extension_angle_deg = -80.0;        // aimed steeply along +n
    const double h_up = mean_tip_height(up);

    DrawCutParams down = base;
    down.extension_angle_deg = 80.0;       // aimed steeply along -n
    const double h_down = mean_tip_height(down);

    INFO("skirt tip height: up " << h_up << " default " << h_default << " down " << h_down);
    // Aiming the skirt along +n lifts the tip; aiming it along -n drops it. The
    // default sits between the two, which is what "continues the band" means when the
    // band is leaning at 30 degrees.
    REQUIRE(h_up > h_default);
    REQUIRE(h_down < h_default);
}

TEST_CASE("Draw cut: a negative Angle mirrors the band across the core plane", "[DrawCut]")
{
    // ITEM 5. The same stroke at +A and at -A must give bands that are mirror images
    // in the plane through the drawn line parallel to the core: the inner ring ends
    // up on the OTHER side of the core plane, and the in-plane inset is identical
    // (cos is even), so the two cores are the same size.
    const indexed_triangle_set cyl = cylinder_3mf_in_plane();
    const double r = cylinder_3mf_radius(cyl);
    const DrawCutStroke stroke = finish_like_gizmo(owner_loop_on_3mf_cylinder(cyl, r, 0.0), cyl);

    Vec3d n, c;
    {
        DrawCutParams probe = owner_params();
        REQUIRE(draw_cut_core_plane(stroke, probe, n, c));
    }

    // The band direction, sample by sample, is where the mirroring lives.
    for (size_t i = 0; i < stroke.path().size(); i += 17) {
        DrawCutParams pos = owner_params(); pos.angle_deg =  40.0;
        DrawCutParams neg = owner_params(); neg.angle_deg = -40.0;
        const Vec3d inward = draw_cut_core_inward(stroke, pos, n, c, i);
        const Vec3d dp = draw_cut_band_dir(inward, n, pos.angle_deg);
        const Vec3d dn = draw_cut_band_dir(inward, n, neg.angle_deg);
        INFO("sample " << i);
        // Same in-plane part, opposite along-n part: a reflection in the plane
        // through the point parallel to the core.
        REQUIRE(dp.dot(inward) == Approx(dn.dot(inward)).margin(1e-12));
        REQUIRE(dp.dot(n) == Approx(-dn.dot(n)).margin(1e-12));
        // And the lip really does lean: at 40 degrees it is not a flat shelf.
        REQUIRE(std::abs(dp.dot(n)) > 0.5);
    }

    // The CORE PLANE the band arrives at moves to the other side too, which is the
    // visible half of the change: the flat mating face is above the drawn line
    // instead of below it.
    BoundingBoxf3 bb;
    for (const Vec3f& v : cyl.vertices)
        bb.merge(v.cast<double>());

    auto core_height = [&](double angle) {
        DrawCutParams p = owner_params();
        p.angle_deg = angle;
        Vec3d cn, cp;
        REQUIRE(draw_cut_core_face(stroke, p, bb, cn, cp));
        // Signed height of the core plane above the loop's own centroid, along the
        // core normal - with cn's sign folded in so the two runs are comparable.
        return (cp - c).dot(n) * (cn.dot(n) >= 0.0 ? 1.0 : -1.0);
    };

    const double h_pos = core_height( 40.0);
    const double h_neg = core_height(-40.0);
    INFO("core height at +40 " << h_pos << ", at -40 " << h_neg);
    // Opposite sides, and by the same distance.
    REQUIRE(h_pos * h_neg < 0.0);
    REQUIRE(std::abs(h_pos) == Approx(std::abs(h_neg)).epsilon(0.05));

    // Both directions still cut the part in two, watertight.
    for (double angle : { 40.0, -40.0 }) {
        DrawCutParams p = owner_params();
        p.angle_deg = angle;
        indexed_triangle_set upper, lower;
        DrawCutError err = DrawCutError::None;
        INFO("angle " << angle);
        REQUIRE(draw_cut_split(cyl, stroke, p, &upper, &lower, &err));
        REQUIRE(err == DrawCutError::None);
        REQUIRE(watertight(upper));
        REQUIRE(watertight(lower));
        const double a = double(its_volume(upper));
        const double b = double(its_volume(lower));
        REQUIRE(a > 0.0);
        REQUIRE(b > 0.0);
        REQUIRE(a + b == Approx(double(its_volume(cyl))).epsilon(0.02));
    }
}

TEST_CASE("Draw cut: Through all projects inward from the drawn line", "[DrawCut]")
{
    // ITEM 1. Through all is a prism from the drawn line's own surface INWARD - the
    // side opposite the stroked facets' outward normal - through everything. Nothing
    // on the OUTWARD side of the stroke is touched.
    //
    // On a cube's top face that is easy to state exactly: a loop on the top face has
    // every normal at +Z, so the prism must occupy z <= 20 and leave z > 20 alone.
    const indexed_triangle_set cube = centred_cube();
    DrawCutStroke ring = circle_on_top(10.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    BoundingBoxf3 bb;
    for (const Vec3f& v : cube.vertices)
        bb.merge(v.cast<double>());

    DrawCutParams params;
    params.through_all = true;

    // THE OUTWARD SIDE is +Z here, and the mesh agrees with the samples.
    Vec3d n, c;
    REQUIRE(draw_cut_core_plane(ring, params, n, c));
    const Vec3d outward = draw_cut_outward_side(ring, n, &cube);
    INFO("outward " << outward.x() << " " << outward.y() << " " << outward.z());
    REQUIRE(outward.z() == Approx(1.0).margin(1e-9));

    const indexed_triangle_set cutter = draw_cut_cutter_solid(ring, params, bb, 0.0, &cube);
    REQUIRE_FALSE(cutter.empty());
    REQUIRE(watertight(cutter));

    // NOTHING ABOVE THE DRAWN LINE. The top face is at z == +20 and the loop is on
    // it, so the prism may not reach past it by more than the tie-breaking lift.
    double z_max = -1e9;
    for (const Vec3f& v : cutter.vertices)
        z_max = std::max(z_max, double(v.z()));
    INFO("cutter reaches z " << z_max << " (the face is at 20)");
    REQUIRE(z_max < 20.0 + 0.5);

    // And it goes clean out of the bottom: through ALL.
    double z_min = 1e9;
    for (const Vec3f& v : cutter.vertices)
        z_min = std::min(z_min, double(v.z()));
    REQUIRE(z_min < -20.0);

    // The cut itself: a straight plug, and the rest of the cube intact.
    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(cube, ring, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    const double a = double(its_volume(upper));   // the plug
    const double b = double(its_volume(lower));
    REQUIRE(a + b == Approx(double(its_volume(cube))).epsilon(0.02));
    // A 10 mm-radius plug through a 40 mm cube: pi r^2 h = 3.14 * 100 * 40.
    INFO("plug volume " << a << " expected ~" << (M_PI * 100.0 * 40.0));
    REQUIRE(a == Approx(M_PI * 100.0 * 40.0).epsilon(0.1));
}

TEST_CASE("Draw cut: the outward side comes from the mesh when the samples cancel",
          "[DrawCut]")
{
    // ITEM 1's robustness half. On a loop whose sample normals nearly cancel - a belt
    // round a barrel, or a patch spread over a hemisphere - the averaged skin normal
    // is noise, and that is exactly the case where the old code let Newell's winding
    // decide which way Through all ran. draw_cut_outward_side() falls through to a
    // parity test against the mesh there.
    //
    // A loop on a SPHERE spanning most of a hemisphere is the clean article: its
    // normals fan out over the whole cap, so their mean along n is small, and yet
    // "which way is out" has an obvious right answer.
    indexed_triangle_set sphere = its_make_sphere(20.0, 0.6);
    BoundingBoxf3 bb;
    for (const Vec3f& v : sphere.vertices)
        bb.merge(v.cast<double>());

    // A loop at 60 degrees of latitude: a wide cap, normals spread over 120 degrees.
    DrawCutStroke stroke;
    const double lat = 60.0 * M_PI / 180.0;
    const int n = 96;
    for (int i = 0; i < n; ++ i) {
        const double th = 2.0 * M_PI * double(i) / double(n);
        const Vec3d dir(std::sin(lat) * std::cos(th), std::sin(lat) * std::sin(th), std::cos(lat));
        stroke.append(20.0 * dir, dir, size_t(i));
    }
    stroke.append(stroke.samples().front().pos, stroke.samples().front().normal, 0);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());

    DrawCutParams params;
    params.through_all = true;
    Vec3d nn, cc;
    REQUIRE(draw_cut_core_plane(stroke, params, nn, cc));

    // Whichever way Newell wound it, the outward side is the one that points AWAY
    // from the sphere's centre - here +Z, because the cap is the north one.
    const Vec3d outward = draw_cut_outward_side(stroke, nn, &sphere);
    INFO("core normal z " << nn.z() << ", outward z " << outward.z());
    REQUIRE(outward.z() > 0.0);

    // And the prism it drives goes DOWN, into the sphere.
    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bb, 0.0, &sphere);
    REQUIRE_FALSE(cutter.empty());
    double z_max = -1e9, z_min = 1e9;
    for (const Vec3f& v : cutter.vertices) {
        z_max = std::max(z_max, double(v.z()));
        z_min = std::min(z_min, double(v.z()));
    }
    // The cap is at z == 20 cos 60 == 10; the prism starts there and runs out of the
    // bottom of the sphere.
    INFO("cutter z " << z_min << " .. " << z_max);
    REQUIRE(z_max < 12.0);
    REQUIRE(z_min < -20.0);
}

TEST_CASE("ZZ diag bunny cutter", "[ZZDiag]")
{
    const indexed_triangle_set bunny = bunny_in_plane();
    BoundingBoxf3 bb;
    for (const Vec3f& v : bunny.vertices)
        bb.merge(v.cast<double>());

    const Vec3d  head   = bunny_head_point(bunny);
    const double radius = 0.10 * bb.size().norm();
    const DrawCutStroke raw = loop_on_surface(bunny, head, Vec3d::UnitX(), radius);
    WARN("raw samples " << raw.samples().size());

    const DrawCutStroke stroke = finish_like_gizmo(raw, bunny);
    WARN("path " << stroke.path().size() << " closed " << stroke.is_closed());

    DrawCutParams params = owner_params();
    WARN("separates " << draw_cut_loop_separates(bunny, stroke, params));

    Vec3d n, c;
    REQUIRE(draw_cut_core_plane(stroke, params, n, c));
    WARN("core n " << n.x() << " " << n.y() << " " << n.z()
         << "  centroid " << c.x() << " " << c.y() << " " << c.z());
    const Vec3d ow = draw_cut_outward_side(stroke, n, &bunny);
    WARN("outward " << ow.x() << " " << ow.y() << " " << ow.z());

    // How far the loop samples are from the centroid axis - the in-plane radius
    // spread. A patch whose samples wrap round a lot has a small min.
    double rmin = 1e9, rmax = 0.0;
    for (const DrawCutSample& s : stroke.path()) {
        const Vec3d q = s.pos - c;
        const double rr = (q - q.dot(n) * n).norm();
        rmin = std::min(rmin, rr);
        rmax = std::max(rmax, rr);
    }
    WARN("in-plane radius " << rmin << " .. " << rmax << " (loop drawn at " << radius << ")");

    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bb, 0.0, &bunny);
    WARN("cutter verts " << cutter.vertices.size() << " tris " << cutter.indices.size()
         << " open edges " << (cutter.empty() ? -1 : int(its_num_open_edges(cutter))));

    if (!cutter.empty()) {
        double worst = 0.0;
        for (const Vec3f& v : cutter.vertices)
            worst = std::max(worst, dist_to_stroke(stroke, v.cast<double>()));
        WARN("cutter worst reach from line " << worst
             << " (budget " << (params.extension + params.depth + 1.0) << ")");
    }
}
