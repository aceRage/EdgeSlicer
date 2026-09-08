// ZAA (Z contouring) unit tests - see docs/superpowers/specs/2026-09-07-z-contouring-port.md.
//
// The per-sample decision (contour_z_sample_delta) and the flow correction
// (contour_z_extrusion_ratio) are pure functions of numbers, so they are tested directly rather
// than through a whole Print. The last case builds a real 10 degree ramp mesh and raycasts it with
// the same sla::IndexedMesh the slicing pass uses.

#include <catch2/catch.hpp>
#include <test_utils.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <libslic3r/ContourZ.hpp>
#include <libslic3r/SLA/IndexedMesh.hpp>
#include <libslic3r/TriangleMesh.hpp>

using namespace Slic3r;

namespace {

// A flat top surface exactly at the layer's nominal top: the ray from the slicing plane travels
// (print_z - slice_z) to reach it, so the raw delta is zero.
ContourZSampleInput base_input(double print_z = 1.0, double height = 0.2, double min_z = 0.05)
{
    ContourZSampleInput in;
    in.height  = height;
    in.print_z = print_z;
    in.slice_z = print_z - height + min_z; // the ZAA slicing plane: lo + zaa_min_z
    in.min_z   = min_z;
    in.hit     = true;
    in.hit_distance = print_z - in.slice_z; // surface at print_z
    in.hit_normal   = Vec3d(0., 0., 1.);
    in.half_width   = 0.2;
    return in;
}

// The raw delta that puts the mesh surface at print_z + d.
void set_surface_at(ContourZSampleInput &in, double d)
{
    in.hit_distance = (in.print_z - in.slice_z) + d;
}

} // namespace

TEST_CASE("ZAA: the raycast clamp", "[ContourZ]")
{
    const double height = 0.2;
    const double min_z  = 0.05;

    SECTION("a flat surface at print_z gets no contour")
    {
        ContourZSampleInput in = base_input(1.0, height, min_z);
        REQUIRE(contour_z_sample_delta(in) == Approx(0.0).margin(1e-9));
    }

    SECTION("top solid infill is clamped up to +zaa_min_z")
    {
        ContourZSampleInput in = base_input(1.0, height, min_z);
        set_surface_at(in, 0.02);
        REQUIRE(contour_z_sample_delta(in) == Approx(0.02));

        // Above min_z the delta fades to zero across ZAA_TOP_TOLERANCE_MM instead of being pinned
        // at min_z and then dropped to 0 in one step (upstream's behaviour, a 50 um ridge one bead
        // wide along every top-surface band boundary - see the review section of the port doc).
        set_surface_at(in, min_z);
        REQUIRE(contour_z_sample_delta(in) == Approx(min_z));

        set_surface_at(in, min_z + ZAA_TOP_TOLERANCE_MM / 3.0);
        REQUIRE(contour_z_sample_delta(in) == Approx(min_z * 2.0 / 3.0));

        set_surface_at(in, min_z + ZAA_TOP_TOLERANCE_MM);
        REQUIRE(contour_z_sample_delta(in) == Approx(0.0).margin(1e-9));

        set_surface_at(in, min_z + 0.05); // outside the tolerance -> no contouring at all
        REQUIRE(contour_z_sample_delta(in) == Approx(0.0).margin(1e-9));

        // and the fade is continuous: no step anywhere across the tolerance band.
        double prev = min_z;
        for (double over = 0.0; over <= ZAA_TOP_TOLERANCE_MM + 0.01; over += 0.0005) {
            set_surface_at(in, min_z + over);
            const double d = contour_z_sample_delta(in);
            REQUIRE(std::abs(d - prev) < 0.002);
            prev = d;
        }
    }

    SECTION("top solid infill is clamped down to -(height - zaa_min_z)")
    {
        ContourZSampleInput in = base_input(1.0, height, min_z);
        set_surface_at(in, -0.10);
        REQUIRE(contour_z_sample_delta(in) == Approx(-0.10));

        // -(0.2 - 0.05) = -0.15 is the floor.
        set_surface_at(in, -0.149);
        REQUIRE(contour_z_sample_delta(in) == Approx(-0.149));
        set_surface_at(in, -0.16);
        REQUIRE(contour_z_sample_delta(in) == Approx(-(height - min_z)));

        // Below -height the sample is "not a top surface".
        set_surface_at(in, -0.25);
        REQUIRE(contour_z_sample_delta(in) == Approx(0.0).margin(1e-9));
    }

    SECTION("the absolute Z never goes below the layer's lo + zaa_min_z")
    {
        ContourZSampleInput in = base_input(1.0, height, min_z);
        const double lo = in.print_z - height;
        for (double d = -0.30; d <= 0.30; d += 0.005) {
            set_surface_at(in, d);
            const double z = in.print_z + contour_z_sample_delta(in);
            REQUIRE(z >= lo + min_z - 1e-9);
            REQUIRE(z <= in.print_z + min_z + 1e-9);
        }
    }

    SECTION("a ray that misses the mesh never contours")
    {
        ContourZSampleInput in = base_input(1.0, height, min_z);
        in.hit = false;
        in.hit_distance = std::numeric_limits<double>::infinity();
        REQUIRE(contour_z_sample_delta(in) == Approx(0.0).margin(1e-9));
    }

    SECTION("ironing gets the wider band")
    {
        ContourZSampleInput in = base_input(1.0, height, min_z);
        in.is_ironing = true;
        set_surface_at(in, 0.15);
        // max_up is `height` for ironing, so 0.15 survives where top infill would have refused it.
        REQUIRE(contour_z_sample_delta(in) == Approx(0.15));
        set_surface_at(in, -0.19);
        REQUIRE(contour_z_sample_delta(in) == Approx(-0.19));
    }
}

TEST_CASE("ZAA: perimeters are never raised", "[ContourZ]")
{
    const double height = 0.2, min_z = 0.05;

    ContourZSampleInput in = base_input(1.0, height, min_z);
    in.is_perimeter = true;
    in.minimize_perimeter_height_deg = 0.0; // slope rule off, so only the never-raise rule applies

    SECTION("a surface above print_z does not lift the wall")
    {
        for (double d : {0.005, 0.02, 0.049}) {
            set_surface_at(in, d);
            REQUIRE(contour_z_sample_delta(in) == Approx(0.0).margin(1e-9));
        }
    }

    SECTION("but a surface below print_z still lowers it")
    {
        set_surface_at(in, -0.08);
        REQUIRE(contour_z_sample_delta(in) == Approx(-0.08));
    }

    SECTION("top solid infill in the same place IS raised")
    {
        ContourZSampleInput fill = base_input(1.0, height, min_z);
        set_surface_at(fill, 0.02);
        REQUIRE(contour_z_sample_delta(fill) == Approx(0.02));
    }
}

TEST_CASE("ZAA: the slope rule", "[ContourZ]")
{
    const double height = 0.2, min_z = 0.05;
    const double half_width = 0.2;

    // A mesh normal tilted by `slope_deg` away from vertical.
    auto normal_for_slope = [](double slope_deg) {
        const double r = slope_deg * M_PI / 180.0;
        return Vec3d(std::sin(r), 0., std::cos(r));
    };

    ContourZSampleInput in = base_input(1.0, height, min_z);
    in.is_perimeter = true;
    in.half_width   = half_width;
    in.minimize_perimeter_height_deg = 35.0;
    set_surface_at(in, 0.0);

    SECTION("below the threshold nothing is dropped")
    {
        in.hit_normal = normal_for_slope(20.0);
        REQUIRE(contour_z_sample_delta(in) == Approx(0.0).margin(1e-9));
        in.hit_normal = normal_for_slope(34.9);
        REQUIRE(contour_z_sample_delta(in) == Approx(0.0).margin(1e-9));
    }

    SECTION("well above the threshold the wall drops by half_width * sin(slope), as upstream")
    {
        const double slope = 60.0;
        in.hit_normal = normal_for_slope(slope);
        const double expected = -half_width * std::sin(slope * M_PI / 180.0);
        // Clamped by the floor -(height - min_z) = -0.15; -0.2*sin(60) = -0.173 -> floor.
        REQUIRE(contour_z_sample_delta(in) == Approx(-(height - min_z)));

        // A shallower case that stays inside the band.
        ContourZSampleInput in2 = in;
        in2.half_width = 0.1;
        const double expected2 = -0.1 * std::sin(slope * M_PI / 180.0);
        REQUIRE(expected2 > -(height - min_z));
        REQUIRE(contour_z_sample_delta(in2) == Approx(expected2));
        (void) expected;
    }

    SECTION("EdgeSlicer guard for OrcaSlicer#13552: the adjustment ramps in, it does not step")
    {
        // Upstream applies the full drop the instant the slope crosses the threshold, so a wall
        // whose slope varies across the threshold gets a half-line-width Z step mid-path. Here the
        // adjustment fades in over ZAA_SLOPE_RAMP_DEGREES as a SMOOTHSTEP, so the derivative is
        // continuous too: neither the value nor the rate of change can jump.
        in.half_width = 0.1;
        double prev = 0.0, prev_step = 0.0;
        for (double slope = 30.0; slope <= 35.0 + ZAA_SLOPE_RAMP_DEGREES + 5.0; slope += 0.25) {
            in.hit_normal = normal_for_slope(slope);
            const double d = contour_z_sample_delta(in);
            REQUIRE(d <= prev + 1e-9);             // monotonically non-increasing
            REQUIRE(std::abs(d - prev) < 0.004);   // and never a step
            const double step = d - prev;
            REQUIRE(std::abs(step - prev_step) < 0.002); // nor a corner
            prev_step = step;
            prev      = d;
        }
        // At threshold + ramp and beyond, identical to upstream's hard rule.
        for (double slope : {35.0 + ZAA_SLOPE_RAMP_DEGREES, 60.0, 70.0}) {
            in.hit_normal = normal_for_slope(slope);
            const double upstream = -0.1 * std::sin(slope * M_PI / 180.0);
            REQUIRE(contour_z_sample_delta(in) == Approx(std::max(upstream, -(height - min_z))));
        }
    }
}

TEST_CASE("ZAA: the along-path profile smoother", "[ContourZ]")
{
    SECTION("a straight ramp is a fixed point - the mesh following is not blurred")
    {
        std::vector<double> d;
        for (int i = 0; i < 40; ++i)
            d.push_back(-0.0025 * i); // 2.5 um per 0.1 mm sample, i.e. a 1.4 degree ramp
        std::vector<double> ref = d;
        contour_z_smooth_profile(d, ZAA_SMOOTH_RADIUS_SAMPLES);
        // Everything but the two end windows is untouched; a moving average reproduces a line.
        for (size_t i = ZAA_SMOOTH_RADIUS_SAMPLES; i + ZAA_SMOOTH_RADIUS_SAMPLES < d.size(); ++i)
            REQUIRE(d[i] == Approx(ref[i]).margin(1e-12));
    }

    SECTION("alternating noise is attenuated")
    {
        std::vector<double> d;
        for (int i = 0; i < 40; ++i)
            d.push_back(i % 2 ? -0.05 : -0.09); // 40 um of sample-to-sample fuzz
        contour_z_smooth_profile(d, ZAA_SMOOTH_RADIUS_SAMPLES);
        double worst = 0.0;
        for (size_t i = ZAA_SMOOTH_RADIUS_SAMPLES; i + ZAA_SMOOTH_RADIUS_SAMPLES + 1 < d.size(); ++i)
            worst = std::max(worst, std::abs(d[i + 1] - d[i]));
        REQUIRE(worst < 0.010); // 40 um of fuzz down to under 10
    }

    SECTION("every output stays inside the input's range, so the clamps survive")
    {
        std::vector<double> d{-0.15, 0.0, -0.15, -0.02, 0.0, -0.15, -0.15, 0.0, -0.07};
        contour_z_smooth_profile(d, ZAA_SMOOTH_RADIUS_SAMPLES);
        for (double v : d) {
            REQUIRE(v <= 0.0 + 1e-12);
            REQUIRE(v >= -0.15 - 1e-12);
        }
    }

    SECTION("a radius of zero, or a profile too short to smooth, is left alone")
    {
        std::vector<double> d{-0.1, 0.0, -0.1};
        std::vector<double> ref = d;
        contour_z_smooth_profile(d, 0);
        REQUIRE(d == ref);
        std::vector<double> two{-0.1, 0.0};
        std::vector<double> two_ref = two;
        contour_z_smooth_profile(two, ZAA_SMOOTH_RADIUS_SAMPLES);
        REQUIRE(two == two_ref);
    }
}

TEST_CASE("ZAA composes with offset_layers", "[ContourZ]")
{
    const double height = 0.2, min_z = 0.05;
    const double z_offset_mm = 0.5 * height; // offset_layers raises odd walls by half a layer

    SECTION("the delta is relative to the offset base, and the absolute band is unchanged")
    {
        ContourZSampleInput flat = base_input(1.0, height, min_z);
        ContourZSampleInput odd  = flat;
        odd.z_offset_mm = z_offset_mm;

        const double lo = flat.print_z - height;

        for (double d = -0.30; d <= 0.30; d += 0.005) {
            set_surface_at(flat, d);
            set_surface_at(odd, d);

            const double d_flat = contour_z_sample_delta(flat);
            const double d_odd  = contour_z_sample_delta(odd);

            // The emitted Z of the odd wall is base + delta, base = print_z + 0.5 * height.
            const double z_odd = odd.print_z + z_offset_mm + d_odd;

            REQUIRE(z_odd >= lo + min_z - 1e-9);            // never below lo + zaa_min_z

            if (d_odd == 0.0) {
                // Not a top surface here: ZAA leaves the wall exactly where offset_layers put it,
                // half a layer up. It must NOT be dragged down to print_z + zaa_min_z - that would
                // undo offset_layers on every interior layer. So the cap below applies to
                // CONTOURED moves only.
                REQUIRE(z_odd == Approx(odd.print_z + z_offset_mm));
            } else {
                REQUIRE(z_odd <= odd.print_z + min_z + 1e-9);   // never above print_z + zaa_min_z
            }

            // Where the surface is inside the band, both frames land on the same absolute Z.
            if (d_flat != 0.0)
                REQUIRE(z_odd == Approx(flat.print_z + d_flat));
        }
    }

    SECTION("an odd wall's base Z is print_z + 0.5h and the contour rides on top of it")
    {
        ContourZSampleInput odd = base_input(1.0, height, min_z);
        odd.is_perimeter = true;
        odd.z_offset_mm  = z_offset_mm;
        odd.minimize_perimeter_height_deg = 0.0;

        // Mesh surface 0.06 mm below the nominal layer top.
        set_surface_at(odd, -0.06);
        const double d = contour_z_sample_delta(odd);
        const double base_z = odd.print_z + z_offset_mm;

        REQUIRE(base_z == Approx(1.1));
        // The wall was half a layer up; the contour has to bring it all the way down to the mesh.
        REQUIRE(d == Approx(-0.06 - z_offset_mm));
        REQUIRE(base_z + d == Approx(odd.print_z - 0.06));
    }

    SECTION("the never-raise rule references the wall's own base, not print_z")
    {
        ContourZSampleInput odd = base_input(1.0, height, min_z);
        odd.is_perimeter = true;
        odd.z_offset_mm  = z_offset_mm;
        odd.minimize_perimeter_height_deg = 0.0;

        // If the reference were print_z, this would force the wall down by the whole 0.1 mm
        // offset regardless of the mesh - silently undoing offset_layers on every top wall.
        set_surface_at(odd, 0.03);
        const double d = contour_z_sample_delta(odd);
        REQUIRE(d < 0.0);                                   // it is lowered towards the mesh
        REQUIRE(d == Approx(0.03 - z_offset_mm));           // exactly onto the mesh, not further
        REQUIRE(d > -z_offset_mm - 1e-9);                   // never below print_z here
    }

    SECTION("the flow follows the single summed local height")
    {
        // offset_layers' bonding multiplier and ZAA's height ratio are different quantities. The
        // emitter applies exactly one height correction, from the final local height, and the two
        // bonding layers are not contoured at all (see ContourZ.cpp).
        const double d = -0.06;
        REQUIRE(contour_z_extrusion_ratio(false, height, d) == Approx((height + d) / height));
        REQUIRE(contour_z_extrusion_ratio(false, height, 0.0) == Approx(1.0));
        // Ironing keeps its configured flow.
        REQUIRE(contour_z_extrusion_ratio(true, height, d) == Approx(1.0));
    }
}

TEST_CASE("ZAA: a 10 degree ramp gets monotonically increasing Z deltas", "[ContourZ]")
{
    // A 20 x 10 mm wedge rising 10 degrees along +X, from z = 2 mm at x = 0.
    const double len = 20.0, wid = 10.0, z0 = 2.0;
    const double slope_deg = 10.0;
    const double rise = len * std::tan(slope_deg * M_PI / 180.0);

    indexed_triangle_set its;
    auto V = [&](double x, double y, double z) {
        its.vertices.emplace_back(stl_vertex(float(x), float(y), float(z)));
        return int(its.vertices.size()) - 1;
    };
    // Bottom rectangle at z = 0, top ramp from z0 to z0 + rise.
    const int b00 = V(0, 0, 0), b10 = V(len, 0, 0), b11 = V(len, wid, 0), b01 = V(0, wid, 0);
    const int t00 = V(0, 0, z0), t10 = V(len, 0, z0 + rise), t11 = V(len, wid, z0 + rise), t01 = V(0, wid, z0);
    auto F = [&](int a, int b, int c) { its.indices.emplace_back(stl_triangle_vertex_indices(a, b, c)); };
    F(b00, b11, b10); F(b00, b01, b11);            // bottom
    F(t00, t10, t11); F(t00, t11, t01);            // ramp top
    F(b00, b10, t10); F(b00, t10, t00);            // y = 0
    F(b01, t01, t11); F(b01, t11, b11);            // y = wid
    F(b00, t00, t01); F(b00, t01, b01);            // x = 0
    F(b10, b11, t11); F(b10, t11, t10);            // x = len

    TriangleMesh mesh(its);
    sla::IndexedMesh imesh(mesh);

    const double height = 0.2;
    const double min_z  = 0.05;

    // Pick a layer whose nominal top cuts the ramp somewhere in the middle.
    const double print_z = 3.0;
    const double lo      = print_z - height;
    const double slice_z = lo + min_z;

    // Where does the ramp top sit at a given x?  z = z0 + x * tan(slope)
    const double tan_s = std::tan(slope_deg * M_PI / 180.0);
    auto ramp_z = [&](double x) { return z0 + x * tan_s; };

    // Sample across the x range where the ramp lies between the slicing plane and print_z, walking
    // uphill. Below the slicing plane the upward ray leaves the solid and legitimately misses, so
    // the range starts where the ramp crosses slice_z.
    const double x_lo = (slice_z - z0) / tan_s;
    const double x_hi = (print_z - z0) / tan_s;
    REQUIRE(x_lo > 0.0);
    REQUIRE(x_hi < len);
    REQUIRE(x_hi - x_lo > 0.5);

    double prev  = -1e30;
    int    moved = 0;
    for (double x = x_lo + 0.02; x < x_hi - 0.02; x += 0.02) {
        const sla::IndexedMesh::hit_result hit = imesh.query_ray_hit({x, wid * 0.5, slice_z}, {0., 0., 1.});
        REQUIRE(hit.is_hit());

        ContourZSampleInput in;
        in.height       = height;
        in.print_z      = print_z;
        in.slice_z      = slice_z;
        in.min_z        = min_z;
        in.hit          = true;
        in.hit_distance = hit.distance();
        in.hit_normal   = hit.normal();
        in.half_width   = 0.2;

        const double d = contour_z_sample_delta(in);

        // The raycast really did find the ramp surface at this x.
        REQUIRE(slice_z + hit.distance() == Approx(ramp_z(x)).margin(1e-4));

        // Walking uphill, the contour rises monotonically...
        REQUIRE(d >= prev - 1e-9);
        if (d > prev + 1e-9)
            ++moved;
        prev = d;

        // ...and stays inside the clamp at every point.
        REQUIRE(print_z + d >= lo + min_z - 1e-9);
        REQUIRE(print_z + d <= print_z + min_z + 1e-9);
    }

    // It is a real contour, not a flat run of zeros.
    REQUIRE(moved > 10);
}


TEST_CASE("ZAA: constant-volumetric-flow speed scaling", "[ContourZ]")
{
    // The rule: F_seg = F_role * h_nominal / h_seg, clamped to [floor, F_role], then to the
    // filament's volumetric cap. See docs/superpowers/specs/2026-09-07-z-contouring-port.md,
    // "Constant-flow speed scaling".
    const double h_nominal = 0.20;
    const double F_role    = 6000.;   // 100 mm/s, in mm/min
    const double no_cap    = 0.;
    const double no_mm3    = 0.;

    SECTION("a thin segment is slowed in proportion to its height")
    {
        // Half the nominal height -> half the feed rate. The segment's E was already scaled by
        // the same ratio, so the extruder's melt rate and the time the Z axis gets per step both
        // return to what the role was tuned for at the nominal layer height.
        const double h_seg = 0.10;
        const double f     = contour_z_segment_feedrate(F_role, h_nominal, h_seg, no_cap, no_mm3);
        REQUIRE(f == Approx(3000.).margin(1e-6));
        REQUIRE(f / h_seg == Approx(F_role / h_nominal).margin(1e-6));

        // The thinnest bead ZAA can produce at the 0.05 default: a quarter of the role's speed.
        REQUIRE(contour_z_segment_feedrate(F_role, h_nominal, 0.05, no_cap, no_mm3) ==
                Approx(1500.).margin(1e-6));
    }

    SECTION("a full-height segment is untouched")
    {
        REQUIRE(contour_z_segment_feedrate(F_role, h_nominal, h_nominal, no_cap, no_mm3) ==
                Approx(F_role).margin(1e-9));
    }

    SECTION("a thick segment never speeds up past the role's F")
    {
        // A contoured top surface can sit up to +zaa_min_z proud, i.e. a 0.25 mm bead on a 0.20
        // mm layer, and the ratio would ask for F * 1.25. The role speed is a deliberate ceiling,
        // so contoured moves are only ever slowed.
        REQUIRE(contour_z_segment_feedrate(F_role, h_nominal, 0.25, no_cap, no_mm3) ==
                Approx(F_role).margin(1e-9));
        for (double h_seg = 0.02; h_seg <= 0.40; h_seg += 0.005)
            REQUIRE(contour_z_segment_feedrate(F_role, h_nominal, h_seg, no_cap, no_mm3) <= F_role + 1e-9);
    }

    SECTION("the floor stops a very thin segment becoming a dwell")
    {
        // At 100 mm/s the 0.05 mm minimum bead asks for 25 mm/s, comfortably above the floor.
        REQUIRE(contour_z_segment_feedrate(F_role, h_nominal, 0.05, no_cap, no_mm3) ==
                Approx(1500.).margin(1e-6));
        // But a role already slowed to 30 mm/s by an overhang would ask for 7.5 mm/s there, and
        // the floor holds it at 10.
        const double f = contour_z_segment_feedrate(30. * 60., h_nominal, 0.05, no_cap, no_mm3);
        REQUIRE(f == Approx(ZAA_MIN_SPEED_MM_S * 60.).margin(1e-6));
        // And nothing, at any height, is ever emitted below the floor.
        for (double h_seg = 0.005; h_seg <= 0.30; h_seg += 0.005)
            REQUIRE(contour_z_segment_feedrate(F_role, h_nominal, h_seg, no_cap, no_mm3) >=
                    ZAA_MIN_SPEED_MM_S * 60. - 1e-9);
    }

    SECTION("the floor never raises F above a role that is already slower than the floor")
    {
        // Ironing at 5 mm/s: the floor must not speed it up to 10.
        const double slow = 5. * 60.;
        REQUIRE(contour_z_segment_feedrate(slow, h_nominal, 0.05, no_cap, no_mm3) == Approx(slow).margin(1e-9));
    }

    SECTION("the volumetric cap wins over both the scaling and the floor")
    {
        // A 0.5 mm3/s filament with a 0.05 mm3/mm segment can only run at 10 mm/s = 600 mm/min.
        const double max_vol        = 0.5;   // mm3/s
        const double mm3_per_mm_seg = 0.05;  // mm3/mm
        const double f = contour_z_segment_feedrate(F_role, h_nominal, h_nominal, max_vol, mm3_per_mm_seg);
        REQUIRE(f == Approx(600.).margin(1e-6));
        REQUIRE(f * mm3_per_mm_seg / 60. <= max_vol + 1e-9);
        // And it still wins where the floor would otherwise have raised F.
        const double tiny_cap = 0.05;
        const double f2 = contour_z_segment_feedrate(F_role, h_nominal, 0.02, tiny_cap, mm3_per_mm_seg);
        REQUIRE(f2 * mm3_per_mm_seg / 60. <= tiny_cap + 1e-9);
        REQUIRE(f2 < ZAA_MIN_SPEED_MM_S * 60.);
    }

    SECTION("degenerate inputs are pass-throughs, never zero or negative")
    {
        REQUIRE(contour_z_segment_feedrate(F_role, 0., 0.1, no_cap, no_mm3) == Approx(F_role));
        REQUIRE(contour_z_segment_feedrate(F_role, h_nominal, 0., no_cap, no_mm3) == Approx(F_role));
        REQUIRE(contour_z_segment_feedrate(F_role, h_nominal, -0.1, no_cap, no_mm3) == Approx(F_role));
        REQUIRE(contour_z_segment_feedrate(0., h_nominal, 0.1, no_cap, no_mm3) == Approx(0.));
    }

    SECTION("the rule is monotone non-decreasing in the local height")
    {
        // A thinner bead is never run faster than a thicker one, so the emitted F profile
        // follows the contour rather than oscillating against it.
        double prev = -1.;
        for (double h_seg = 0.01; h_seg <= 0.40 + 1e-9; h_seg += 0.005) {
            const double f = contour_z_segment_feedrate(F_role, h_nominal, h_seg, no_cap, no_mm3);
            REQUIRE(f >= prev - 1e-9);
            prev = f;
        }
    }
}

TEST_CASE("ZAA: the speed hysteresis rule", "[ContourZ]")
{
    // Consecutive contoured segments whose local height is within 5 % of the height that set the
    // F in force keep that F, so a smoothed profile that changes by a micron per 0.1 mm sample
    // does not emit an F word on every move.
    const double h_ref = 0.20;

    SECTION("inside the band keeps the F, outside re-emits")
    {
        REQUIRE(contour_z_speed_within_hysteresis(h_ref, 0.200));
        REQUIRE(contour_z_speed_within_hysteresis(h_ref, 0.205));   // +2.5 %
        REQUIRE(contour_z_speed_within_hysteresis(h_ref, 0.195));   // -2.5 %
        // The boundary itself (+-exactly 5 %) is not pinned: 0.190 and 0.210 sit on it and
        // binary floating point can land either side. Just inside and just outside are.
        REQUIRE(contour_z_speed_within_hysteresis(h_ref, 0.2099));
        REQUIRE(contour_z_speed_within_hysteresis(h_ref, 0.1901));
        REQUIRE_FALSE(contour_z_speed_within_hysteresis(h_ref, 0.211));
        REQUIRE_FALSE(contour_z_speed_within_hysteresis(h_ref, 0.189));
        REQUIRE_FALSE(contour_z_speed_within_hysteresis(h_ref, 0.100));
    }

    SECTION("no F in force yet always re-emits")
    {
        REQUIRE_FALSE(contour_z_speed_within_hysteresis(0., 0.20));
        REQUIRE_FALSE(contour_z_speed_within_hysteresis(-1., 0.20));
        REQUIRE_FALSE(contour_z_speed_within_hysteresis(h_ref, 0.));
    }

    SECTION("the flow error the hysteresis can introduce is bounded by the band")
    {
        // Whenever a segment is allowed to keep the previous F, the flow it actually runs at
        // differs from nominal by exactly h_seg / h_ref, which the band bounds at 5 %.
        const double F_role = 6000.;
        const double h_nom  = 0.20;
        for (double h_seg = 0.05; h_seg <= 0.30; h_seg += 0.0011) {
            if (!contour_z_speed_within_hysteresis(h_ref, h_seg))
                continue;
            // The F in force is the one h_ref asked for.
            const double f_in_force = contour_z_segment_feedrate(F_role, h_nom, h_ref, 0., 0.);
            const double flow_kept  = f_in_force * h_seg;
            const double flow_exact = contour_z_segment_feedrate(F_role, h_nom, h_seg, 0., 0.) * h_seg;
            REQUIRE(std::abs(flow_kept - flow_exact) <= 0.05 * flow_exact + 1e-6);
        }
    }

    SECTION("a monotone profile inside one band emits exactly one F")
    {
        // Walk a slowly drifting profile the way the emitter does and count the emissions.
        double h_in_force = 0.;
        int    emissions  = 0;
        for (int i = 0; i < 40; ++i) {
            const double h_seg = 0.200 - 0.0001 * i;   // 200 um down to 196.1 um, all within 5 %
            if (!contour_z_speed_within_hysteresis(h_in_force, h_seg)) {
                ++emissions;
                h_in_force = h_seg;
            }
        }
        REQUIRE(emissions == 1);

        // A real ramp that leaves the band does re-emit, and only when it has to.
        h_in_force = 0.;
        emissions  = 0;
        for (int i = 0; i < 40; ++i) {
            const double h_seg = 0.200 - 0.004 * i;    // 200 um down to 44 um
            if (h_seg <= 0.)
                break;
            if (!contour_z_speed_within_hysteresis(h_in_force, h_seg)) {
                ++emissions;
                h_in_force = h_seg;
            }
        }
        REQUIRE(emissions > 1);
        REQUIRE(emissions < 40);
    }
}
