#include <catch2/catch.hpp>

#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/PushFit.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace Slic3r;

// Push-fit "Pin" connector geometry (PR 1 of tests/design_pushfit_connector.md). The cut, the
// 3MF and re-edit tests (design T5-T7) come with the model integration.

namespace {

const PushFitPreset ALL_PRESETS[] = { PushFitPreset::S, PushFitPreset::M, PushFitPreset::L };
const PushFitCutout ALL_CUTOUTS[] = { PushFitCutout::None, PushFitCutout::Flat, PushFitCutout::Slot,
                                      PushFitCutout::FlatAndSlot };

PushFitParams preset(PushFitPreset pr) { return pushfit_apply_preset(PushFitParams{}, pr); }

BoundingBoxf3 bbox(const indexed_triangle_set &its)
{
    BoundingBoxf3 bb;
    for (const Vec3f &v : its.vertices)
        bb.merge(v.cast<double>());
    return bb;
}

double volume(const indexed_triangle_set &its) { return double(its_volume(its)); }

void require_sound(const indexed_triangle_set &its)
{
    REQUIRE(!its.indices.empty());
    REQUIRE(its_num_open_edges(its) == 0);
    REQUIRE(volume(its) > 0.);
    REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(TriangleMesh(its)));
}

indexed_triangle_set pin_in(const PushFitParams &p, PushFitMeshFrame frame, double kerf_lo = 0., double kerf_hi = 0.)
{
    PushFitPinOptions opt;
    opt.frame   = frame;
    opt.kerf_lo = kerf_lo;
    opt.kerf_hi = kerf_hi;
    return pushfit_pin_mesh(p, opt).mesh;
}

// The polygons of a horizontal slice, in mm.
std::vector<std::vector<Vec2d>> slice_at(const indexed_triangle_set &its, double z)
{
    std::vector<std::vector<Vec2d>> out;
    for (const Polygon &poly : slice_mesh(its, float(z), MeshSlicingParams{})) {
        std::vector<Vec2d> pts;
        for (const Point &pt : poly.points)
            pts.emplace_back(unscale<double>(pt.x()), unscale<double>(pt.y()));
        out.push_back(std::move(pts));
    }
    return out;
}

double max_radius(const std::vector<std::vector<Vec2d>> &polys)
{
    double r = 0.;
    for (const auto &poly : polys)
        for (const Vec2d &q : poly)
            r = std::max(r, q.norm());
    return r;
}

// Steepest downward-facing facet, in degrees from vertical, ignoring the bed face itself (all
// three corners at z ~ 0) and slivers whose normals are noise.
double max_overhang_deg(const indexed_triangle_set &its)
{
    double worst = 0.;
    for (const Vec3i32 &f : its.indices) {
        const Vec3d a = its.vertices[f[0]].cast<double>();
        const Vec3d b = its.vertices[f[1]].cast<double>();
        const Vec3d c = its.vertices[f[2]].cast<double>();
        if (a.z() < 1e-4 && b.z() < 1e-4 && c.z() < 1e-4)
            continue;
        const Vec3d  n    = (b - a).cross(c - a);
        const double area = 0.5 * n.norm();
        if (area < 1e-4)
            continue;
        const double nz = n.z() / n.norm();
        if (nz < 0.)
            worst = std::max(worst, std::asin(std::min(1., -nz)) * 180. / M_PI);
    }
    return worst;
}

double intersection_volume(const indexed_triangle_set &a, const indexed_triangle_set &b)
{
    std::vector<TriangleMesh> dst;
    if (!MeshBoolean::mfd::make_boolean(TriangleMesh(a), TriangleMesh(b), dst, "INTERSECTION")) {
        dst.clear();
        MeshBoolean::mcut::make_boolean(TriangleMesh(a), TriangleMesh(b), dst, "INTERSECTION");
    }
    double vol = 0.;
    for (const TriangleMesh &m : dst)
        vol += std::abs(double(its_volume(m.its)));
    return vol;
}

} // namespace

// ------------------------------------------------------------------------ T1 watertight

TEST_CASE("Push-fit pins are watertight for every preset, cutout and lip", "[PushFit]")
{
    for (PushFitPreset pr : ALL_PRESETS)
        for (PushFitCutout cut : ALL_CUTOUTS)
            for (bool lip : { false, true }) {
                PushFitParams p = preset(pr);
                p.cutout        = cut;
                p.lip           = lip;
                INFO("preset " << int(pr) << " cutout " << int(cut) << " lip " << lip);
                REQUIRE(pushfit_validate(p).empty());
                const PushFitPinResult r = pushfit_pin_mesh(p);
                require_sound(r.mesh);
                REQUIRE(r.slot_requested == pushfit_cutout_has_slot(cut));
                REQUIRE(r.slot_made == r.slot_requested);
                REQUIRE_FALSE(r.plain_fallback);
                // A sanity bound on the volume: less than the lipped cylinder, more than half of it.
                const double R    = 0.5 * double(p.diameter) + (lip ? double(p.lip_height) : 0.);
                const double full = M_PI * R * R * double(p.length_upper + p.length_lower);
                REQUIRE(volume(r.mesh) < full);
                REQUIRE(volume(r.mesh) > 0.4 * full);
            }
}

TEST_CASE("Push-fit holes are watertight for every preset, lip and groove", "[PushFit]")
{
    for (PushFitPreset pr : ALL_PRESETS)
        for (bool lip : { false, true })
            for (bool groove : { false, true })
                for (bool pointed : { false, true })
                    for (PushFitSide side : { PushFitSide::Upper, PushFitSide::Lower }) {
                        PushFitParams p = preset(pr);
                        p.lip           = lip;
                        p.hole_groove   = groove;
                        p.pointed_end   = pointed;
                        INFO("preset " << int(pr) << " lip " << lip << " groove " << groove << " pointed " << pointed
                                       << " side " << int(side));
                        REQUIRE(pushfit_validate(p).empty());
                        require_sound(pushfit_hole_mesh(p, side, 0.));
                        require_sound(pushfit_hole_mesh(p, side, 0.25));
                    }
}

// ------------------------------------------------------------------------ T2 dimensions

TEST_CASE("Push-fit pin dimensions", "[PushFit]")
{
    const PushFitParams m = preset(PushFitPreset::M);
    const double R = 0.5 * double(m.diameter);
    const double h = double(m.lip_height);
    const double f = double(m.flat_depth);

    SECTION("length, with and without a kerf, asymmetric sides") {
        PushFitParams p = m;
        p.length_upper  = 7.f;
        p.length_lower  = 5.f;
        BoundingBoxf3 bb = bbox(pin_in(p, PushFitMeshFrame::Connector));
        REQUIRE(bb.min.z() == Approx(-5.).margin(1e-4));
        REQUIRE(bb.max.z() == Approx(7.).margin(1e-4));
        bb = bbox(pin_in(p, PushFitMeshFrame::Connector, 0.2, 0.3));
        REQUIRE(bb.min.z() == Approx(-5.2).margin(1e-4));
        REQUIRE(bb.max.z() == Approx(7.3).margin(1e-4));
        // Lying, the length runs along X.
        bb = bbox(pin_in(p, PushFitMeshFrame::PrintPose, 0.2, 0.3));
        REQUIRE(bb.size().x() == Approx(12.5).margin(1e-4));
    }
    SECTION("across the round, the lip makes it D + 2h") {
        PushFitParams p = m;
        p.cutout        = PushFitCutout::None;
        const BoundingBoxf3 bb = bbox(pin_in(p, PushFitMeshFrame::Connector));
        REQUIRE(bb.size().x() == Approx(2. * (R + h)).margin(1e-4));
        REQUIRE(bb.size().y() == Approx(2. * (R + h)).margin(1e-4));
        p.lip = false;
        REQUIRE(bbox(pin_in(p, PushFitMeshFrame::Connector)).size().x() == Approx(2. * R).margin(1e-4));
    }
    SECTION("across the flats") {
        PushFitParams p   = m;
        p.cutout          = PushFitCutout::Flat;
        p.flat_both_sides = false;
        p.lip             = false;
        REQUIRE(bbox(pin_in(p, PushFitMeshFrame::Connector)).size().y() == Approx(2. * R - f).margin(1e-4));
        p.lip = true;   // one flat: the lip still crowns the opposite side
        REQUIRE(bbox(pin_in(p, PushFitMeshFrame::Connector)).size().y() == Approx(2. * R - f + h).margin(1e-4));
        p.flat_both_sides = true;   // two flats cut the lip too
        REQUIRE(bbox(pin_in(p, PushFitMeshFrame::Connector)).size().y() == Approx(2. * R - 2. * f).margin(1e-4));
        // ... and the round direction keeps the lip.
        REQUIRE(bbox(pin_in(p, PushFitMeshFrame::Connector)).size().x() == Approx(2. * (R + h)).margin(1e-4));
    }
    SECTION("a slice through the lip reaches D/2 + h, one beside it D/2") {
        const indexed_triangle_set pin = pin_in(m, PushFitMeshFrame::Connector);
        const double zl = double(m.lip_position) * double(m.length_upper);
        REQUIRE(max_radius(slice_at(pin, zl)) == Approx(R + h).margin(0.01));
        REQUIRE(max_radius(slice_at(pin, -zl)) == Approx(R + h).margin(0.01));
        REQUIRE(max_radius(slice_at(pin, 1.0)) == Approx(R).margin(0.01));
    }
    SECTION("a slice inside the slit is two prongs a slot width apart") {
        const indexed_triangle_set pin = pin_in(m, PushFitMeshFrame::Connector);
        for (double z : { double(m.length_upper) - 0.5 * double(m.slot_length),
                          -(double(m.length_lower) - 0.5 * double(m.slot_length)) }) {
            const auto polys = slice_at(pin, z);
            REQUIRE(polys.size() == 2);
            std::vector<std::pair<double, double>> xs;
            for (const auto &poly : polys) {
                double lo = std::numeric_limits<double>::max(), hi = -lo;
                for (const Vec2d &q : poly) {
                    lo = std::min(lo, q.x());
                    hi = std::max(hi, q.x());
                }
                xs.emplace_back(lo, hi);
            }
            std::sort(xs.begin(), xs.end());
            REQUIRE(xs[1].first - xs[0].second == Approx(double(m.slot_width)).margin(0.01));
        }
        // The middle of the pin is one piece.
        REQUIRE(slice_at(pin, 0.).size() == 1);
    }
    SECTION("the holes") {
        PushFitParams p    = m;
        p.length_lower     = 5.f;
        p.clearance_lower  = 0.2f;
        const double rh_u = R + double(p.clearance_upper);
        const double rh_l = R + double(p.clearance_lower);

        indexed_triangle_set up = pushfit_hole_mesh(p, PushFitSide::Upper, 0.);
        BoundingBoxf3 bb = bbox(up);
        REQUIRE(bb.min.z() == Approx(-PUSHFIT_HOLE_MOUTH_LEAD).margin(1e-4));       // opens 0.2 outside the face
        REQUIRE(bb.max.z() == Approx(7. + double(p.depth_clearance) + rh_u).margin(1e-4)); // depth + 90 deg cone
        REQUIRE(bb.size().x() == Approx(2. * (rh_u + double(p.mouth_chamfer) + PUSHFIT_HOLE_MOUTH_LEAD)).margin(1e-4));
        REQUIRE(max_radius(slice_at(up, 3.)) == Approx(rh_u).margin(0.01));

        indexed_triangle_set lo = pushfit_hole_mesh(p, PushFitSide::Lower, 0.);
        bb = bbox(lo);
        REQUIRE(bb.max.z() == Approx(PUSHFIT_HOLE_MOUTH_LEAD).margin(1e-4));
        REQUIRE(bb.min.z() == Approx(-(5. + double(p.depth_clearance) + rh_l)).margin(1e-4));
        REQUIRE(max_radius(slice_at(lo, -2.5)) == Approx(rh_l).margin(0.01));

        // Each hole is measured from its own face: a kerf moves it, it does not deepen it.
        bb = bbox(pushfit_hole_mesh(p, PushFitSide::Upper, 0.25));
        REQUIRE(bb.min.z() == Approx(0.25 - PUSHFIT_HOLE_MOUTH_LEAD).margin(1e-4));
        REQUIRE(bb.max.z() == Approx(0.25 + 7. + double(p.depth_clearance) + rh_u).margin(1e-4));

        // A flat end stops at the depth.
        p.pointed_end = false;
        REQUIRE(bbox(pushfit_hole_mesh(p, PushFitSide::Upper, 0.)).max.z() ==
                Approx(7. + double(p.depth_clearance)).margin(1e-4));

        // The groove sits at the lip and is lip_height deep.
        p.hole_groove = true;
        const double zl = double(p.lip_position) * 7.;
        REQUIRE(max_radius(slice_at(pushfit_hole_mesh(p, PushFitSide::Upper, 0.), zl)) ==
                Approx(rh_u + double(p.lip_height)).margin(0.01));
    }
}

// ------------------------------------------------------------------------ T3 print pose

TEST_CASE("Push-fit print pose rule: lie on a flat that is flat enough, otherwise upright", "[PushFit]")
{
    const PushFitParams m = preset(PushFitPreset::M);

    SECTION("the presets' 0.15 D flats lie down") {
        for (PushFitPreset pr : ALL_PRESETS) {
            const PushFitParams p = preset(pr);
            std::string why = "unset";
            REQUIRE(pushfit_can_lie(p, &why));
            REQUIRE(why.empty());
            REQUIRE(pushfit_resolve_pose(p) == PushFitPrintPose::Lying);
            REQUIRE(pushfit_lying_overhang_deg(p) <= 45.);
        }
    }
    SECTION("the threshold is the 45 degree flat, f = D (1 - sin 45) / 2") {
        PushFitParams p = m;
        const double f45 = 0.5 * double(p.diameter) * (1. - std::sin(M_PI / 4.));
        p.flat_depth = float(f45 + 1e-4);
        REQUIRE(pushfit_resolve_pose(p) == PushFitPrintPose::Lying);
        p.flat_depth = float(f45 - 0.02);
        REQUIRE(pushfit_resolve_pose(p) == PushFitPrintPose::Upright);
    }
    SECTION("too shallow a flat, or none, prints upright") {
        PushFitParams p = m;
        p.flat_depth    = 0.1f * p.diameter;   // 53 degree overhang
        std::string why;
        REQUIRE_FALSE(pushfit_can_lie(p, &why));
        REQUIRE_FALSE(why.empty());
        REQUIRE(pushfit_resolve_pose(p) == PushFitPrintPose::Upright);
        for (PushFitCutout c : { PushFitCutout::None, PushFitCutout::Slot }) {
            p        = m;
            p.cutout = c;
            REQUIRE_FALSE(pushfit_can_lie(p));
            REQUIRE(pushfit_resolve_pose(p) == PushFitPrintPose::Upright);
        }
    }
    SECTION("Upright overrides a good flat") {
        PushFitParams p = m;
        p.pose          = PushFitPose::Upright;
        REQUIRE(pushfit_can_lie(p));
        REQUIRE(pushfit_resolve_pose(p) == PushFitPrintPose::Upright);
    }
}

TEST_CASE("Push-fit pins lying on the flat need no supports", "[PushFit]")
{
    for (PushFitPreset pr : ALL_PRESETS)
        for (PushFitCutout cut : { PushFitCutout::Flat, PushFitCutout::FlatAndSlot })
            for (bool both : { false, true }) {
                PushFitParams p   = preset(pr);
                p.cutout          = cut;
                p.flat_both_sides = both;
                INFO("preset " << int(pr) << " cutout " << int(cut) << " both " << both);
                const PushFitPinResult r = pushfit_pin_mesh(p);
                REQUIRE(r.pose == PushFitPrintPose::Lying);
                const BoundingBoxf3 bb = bbox(r.mesh);
                const double D = double(p.diameter), f = double(p.flat_depth);
                // On the bed, centred, axis along X.
                REQUIRE(bb.min.z() == Approx(0.).margin(1e-4));
                REQUIRE(bb.center().x() == Approx(0.).margin(1e-4));
                REQUIRE(bb.center().y() == Approx(0.).margin(1e-4));
                REQUIRE(bb.size().x() == Approx(double(p.length_upper + p.length_lower)).margin(1e-4));
                REQUIRE(bb.size().x() > bb.size().y());
                // The bottom face is the flat: a planar region at least D - 2f wide.
                double ylo = std::numeric_limits<double>::max(), yhi = -ylo;
                for (const Vec3f &v : r.mesh.vertices)
                    if (v.z() < 1e-4) {
                        ylo = std::min(ylo, double(v.y()));
                        yhi = std::max(yhi, double(v.y()));
                    }
                REQUIRE(yhi - ylo >= D - 2. * f);
                REQUIRE(max_overhang_deg(r.mesh) <= 45.5);
                // The slit is vertical: the prongs stand side by side, nothing bridges.
                // connector_to_mesh puts the upper tip at +X.
                const Vec3d tip = r.connector_to_mesh * Vec3d(0., 0., double(p.length_upper));
                REQUIRE(tip.x() == Approx(bb.max.x()).margin(1e-4));
            }
}

TEST_CASE("Push-fit pins that cannot lie stand upright on an end", "[PushFit]")
{
    PushFitParams p = preset(PushFitPreset::M);
    p.length_lower  = 5.f;
    for (PushFitCutout cut : { PushFitCutout::None, PushFitCutout::Slot }) {
        p.cutout = cut;
        const PushFitPinResult r = pushfit_pin_mesh(p);
        REQUIRE(r.pose == PushFitPrintPose::Upright);
        require_sound(r.mesh);
        const BoundingBoxf3 bb = bbox(r.mesh);
        REQUIRE(bb.min.z() == Approx(0.).margin(1e-4));
        REQUIRE(bb.size().z() == Approx(12.).margin(1e-4));
        // The end chamfer and the lip flanks are the only overhangs. (Upright, the lower slit's
        // round end is a 1 mm arch, which bridges trivially, so it is left out of this check.)
        if (cut == PushFitCutout::None)
            REQUIRE(max_overhang_deg(r.mesh) <= 45.5);
        // The lower tip is the foot.
        REQUIRE((r.connector_to_mesh * Vec3d(0., 0., -5.)).z() == Approx(0.).margin(1e-4));
    }
    // The override.
    p.cutout = PushFitCutout::FlatAndSlot;
    p.pose   = PushFitPose::Upright;
    const PushFitPinResult r = pushfit_pin_mesh(p);
    REQUIRE(r.pose == PushFitPrintPose::Upright);
    REQUIRE(bbox(r.mesh).size().z() == Approx(12.).margin(1e-4));
}

// ------------------------------------------------------------------------ T4 fit

TEST_CASE("Push-fit pin fits its holes", "[PushFit]")
{
    PushFitParams p = preset(PushFitPreset::M);
    p.cutout        = PushFitCutout::None;   // a plain round pin, so the expected overlap is analytic
    const double R  = 0.5 * double(p.diameter);
    const double h  = double(p.lip_height);
    const double lw = double(p.lip_width);

    // The upper half of the pin: what sits in the upper hole.
    auto upper_part = [](const PushFitParams &q) {
        const indexed_triangle_set pin = pin_in(q, PushFitMeshFrame::Connector);
        indexed_triangle_set up;
        cut_mesh(pin, 0.f, &up, nullptr);
        its_compactify_vertices(up);
        return up;
    };
    auto interference = [&](const PushFitParams &q) {
        const indexed_triangle_set up   = upper_part(q);
        const indexed_triangle_set hole = pushfit_hole_mesh(q, PushFitSide::Upper, 0.);
        return volume(up) - intersection_volume(up, hole);
    };

    SECTION("clearance above the lip height: no overlap") {
        p.clearance_upper = 0.3f;
        REQUIRE(interference(p) == Approx(0.).margin(0.01));
    }
    SECTION("a lip prouder than the clearance overlaps by the lip ring beyond the hole wall") {
        p.clearance_upper = 0.10f;
        const double rh = R + double(p.clearance_upper);
        // Pappus by integration: the lip's axial width at radius r is lw - 2 (r - R).
        double expect = 0.;
        const int N = 2000;
        for (int i = 0; i < N; ++ i) {
            const double r = rh + (R + h - rh) * (double(i) + 0.5) / double(N);
            expect += 2. * M_PI * r * (lw - 2. * (r - R)) * (R + h - rh) / double(N);
        }
        REQUIRE(expect > 0.1);
        REQUIRE(interference(p) == Approx(expect).epsilon(0.1));
    }
    SECTION("the groove takes the lip: no overlap") {
        p.clearance_upper = 0.10f;
        p.hole_groove     = true;
        REQUIRE(interference(p) == Approx(0.).margin(0.01));
    }
    SECTION("the pin end stops short of the hole end by the depth clearance") {
        const BoundingBoxf3 pin  = bbox(pin_in(p, PushFitMeshFrame::Connector));
        p.pointed_end            = false;
        const BoundingBoxf3 hole = bbox(pushfit_hole_mesh(p, PushFitSide::Upper, 0.));
        REQUIRE(hole.max.z() - pin.max.z() == Approx(double(p.depth_clearance)).margin(1e-4));
    }
}

// ------------------------------------------------------------------------ T8 guards

TEST_CASE("Push-fit validation", "[PushFit]")
{
    for (PushFitPreset pr : ALL_PRESETS)
        REQUIRE(pushfit_validate(preset(pr)).empty());

    const PushFitParams m = preset(PushFitPreset::M);
    auto rejects = [](PushFitParams p, double nozzle = 0.4) { return !pushfit_validate(p, nozzle).empty(); };

    PushFitParams p = m;
    p.diameter = 2.9f;                          REQUIRE(rejects(p));
    p = m; p.length_upper = 1.9f;               REQUIRE(rejects(p));
    p = m; p.length_lower = 1.9f;               REQUIRE(rejects(p));
    p = m; p.lip_height = 0.25f * p.diameter;   REQUIRE(rejects(p));
    p = m; p.slot_width = 0.5f;                 REQUIRE(rejects(p));
    p = m; p.slot_length = p.length_upper - 0.5f * p.lip_width + 0.1f; REQUIRE(rejects(p));
    p = m; p.length_lower = 4.f;                REQUIRE(rejects(p));   // the slot reaches the lower cut face
    p = m; p.flat_depth = p.diameter / 3.f;     REQUIRE(rejects(p));
    p = m; p.lip_width = 2.f * p.lip_height;    REQUIRE(rejects(p));
    p = m; p.lip_position = 0.95f;              REQUIRE(rejects(p));   // into the end chamfer
    p = m; p.diameter = std::numeric_limits<float>::quiet_NaN(); REQUIRE(rejects(p));
    // Prongs need two lines each: S passes on a 0.4 nozzle and fails on a 0.6 (design 3.2).
    REQUIRE_FALSE(rejects(preset(PushFitPreset::S), 0.4));
    REQUIRE(rejects(preset(PushFitPreset::S), 0.6));
    REQUIRE_FALSE(rejects(m, 0.6));
    // Rules that only apply to the feature that uses them.
    p = m; p.cutout = PushFitCutout::Flat; p.slot_width = 0.1f;  REQUIRE_FALSE(rejects(p));
    p = m; p.lip = false; p.lip_height = 5.f;                    REQUIRE_FALSE(rejects(p));
}

TEST_CASE("Push-fit slit boolean failure falls back to no slit", "[PushFit]")
{
    const PushFitParams m = preset(PushFitPreset::M);
    const PushFitPinResult slit = pushfit_pin_mesh(m);
    REQUIRE(slit.slot_made);

    PushFitPinOptions opt;
    opt.simulate_slot_boolean_failure = true;
    const PushFitPinResult failed = pushfit_pin_mesh(m, opt);
    REQUIRE(failed.slot_requested);
    REQUIRE_FALSE(failed.slot_made);
    require_sound(failed.mesh);
    REQUIRE(volume(failed.mesh) > volume(slit.mesh));
    REQUIRE(failed.pose == PushFitPrintPose::Lying);

    // A degenerate slit forced past validation is the same: no slit, a sound pin.
    PushFitParams p = m;
    p.slot_width    = 0.f;
    REQUIRE_FALSE(pushfit_validate(p).empty());
    const PushFitPinResult degenerate = pushfit_pin_mesh(p);
    REQUIRE_FALSE(degenerate.slot_made);
    require_sound(degenerate.mesh);
}

TEST_CASE("Push-fit warnings", "[PushFit]")
{
    const PushFitParams m = preset(PushFitPreset::M);
    REQUIRE(pushfit_warnings(m).empty());

    PushFitParams p     = m;
    p.cutout            = PushFitCutout::Flat;
    p.clearance_upper   = p.clearance_lower = 0.05f;   // lip 0.25 proud of a 0.05 clearance, no slot
    REQUIRE(pushfit_warnings(p).size() == 1);
    p.hole_groove = true;
    REQUIRE(pushfit_warnings(p).empty());

    // Too thin a half: M needs 7 + 0.4 + 3.1 (cone) + 1.2 wall = 11.7 mm.
    REQUIRE(pushfit_warnings(m, 0.4, 11.0, 0.).size() == 1);
    REQUIRE(pushfit_warnings(m, 0.4, 11.0, 11.0).size() == 2);
    REQUIRE(pushfit_warnings(m, 0.4, 12.0, 12.0).empty());

    p                 = m;
    p.clearance_upper = 0.f;
    REQUIRE(pushfit_warnings(p).size() == 1);
}

// ------------------------------------------------------------------------ presets and Auto

TEST_CASE("Push-fit presets and Auto sizing", "[PushFit]")
{
    // The struct's defaults are the M preset.
    REQUIRE(pushfit_apply_preset(PushFitParams{}, PushFitPreset::M) == PushFitParams{});

    const PushFitParams s = preset(PushFitPreset::S);
    REQUIRE(s.diameter == 4.f);
    REQUIRE(s.length_upper == 5.f);
    REQUIRE(s.preset == int(PushFitPreset::S));
    const PushFitParams l = preset(PushFitPreset::L);
    REQUIRE(l.diameter == 8.f);
    REQUIRE(l.clearance_lower == 0.12f);

    // A preset keeps the on/off choices.
    PushFitParams custom = PushFitParams{};
    custom.cutout        = PushFitCutout::Flat;
    custom.hole_groove   = true;
    custom.pose          = PushFitPose::Upright;
    const PushFitParams applied = pushfit_apply_preset(custom, PushFitPreset::L);
    REQUIRE(applied.cutout == PushFitCutout::Flat);
    REQUIRE(applied.hole_groove);
    REQUIRE(applied.pose == PushFitPose::Upright);
    REQUIRE(pushfit_apply_preset(custom, PushFitPreset::Custom) == custom);

    // D <= 0.5 x the inscribed radius.
    REQUIRE(pushfit_auto_size(PushFitParams{}, 20., 0., 0.).params.diameter == 8.f);
    REQUIRE(pushfit_auto_size(PushFitParams{}, 14., 0., 0.).params.diameter == 6.f);
    REQUIRE(pushfit_auto_size(PushFitParams{}, 10., 0., 0.).params.diameter == 4.f);
    const PushFitAutoSize none = pushfit_auto_size(PushFitParams{}, 6., 0., 0.);
    REQUIRE_FALSE(none.fits);
    REQUIRE(none.params.diameter == 4.f);
    // The thinner half decides: L needs 15.8 mm, M 11.7 mm.
    REQUIRE(pushfit_auto_size(PushFitParams{}, 100., 50., 12.).params.diameter == 6.f);
    REQUIRE(pushfit_auto_size(PushFitParams{}, 100., 16., 16.).params.diameter == 8.f);
}

TEST_CASE("Push-fit preview body is the connector-frame pin", "[PushFit]")
{
    const PushFitParams m = preset(PushFitPreset::M);
    const indexed_triangle_set prev = pushfit_preview_body(m);
    require_sound(prev);
    const BoundingBoxf3 bb = bbox(prev);
    REQUIRE(bb.min.z() == Approx(-double(m.length_lower)).margin(1e-4));
    REQUIRE(bb.max.z() == Approx(double(m.length_upper)).margin(1e-4));
}
