#include <catch2/catch.hpp>

#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Format/3mf.hpp>
#include <libslic3r/Format/STL.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <set>

using namespace Slic3r;

// The test article: a 40 mm cube centred on the origin, so the cut plane frame
// (z == 0) runs through its middle.
static const double CUBE = 40.0;

static indexed_triangle_set centred_cube(double s = CUBE)
{
    indexed_triangle_set its = its_make_cube(s, s, s);
    for (Vec3f& v : its.vertices)
        v -= Vec3f(float(0.5 * s), float(0.5 * s), float(0.5 * s));
    return its;
}

// A dome: the centre control point raised, the rim left at zero.
static CurvedCutSheet dome_sheet(double height, int resolution = 5, double half_size = 40.0)
{
    CurvedCutSheet sheet(resolution);
    sheet.set_half_size(half_size);
    const int c = resolution / 2;
    sheet.at(c, c) = height;
    return sheet;
}

// ---------------------------------------------------------------------------
// (1) Zero displacement reproduces the flat cut EXACTLY.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a flat sheet is the flat cut", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();

    // The flat reference: the same cut_mesh() call perform_with_plane() makes.
    indexed_triangle_set ref_upper, ref_lower;
    cut_mesh(cube, 0.0f, &ref_upper, &ref_lower);

    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    REQUIRE(sheet.is_flat());

    // Everything on the sheet must read as exactly zero, in double precision,
    // not merely near it - that is what lets the cut dispatch to the flat path.
    for (int j = 0; j <= 20; ++ j)
        for (int i = 0; i <= 20; ++ i)
            REQUIRE(sheet.evaluate(double(i) / 20.0, double(j) / 20.0) == 0.0);

    // And the whole Cut path: a flat sheet must produce the same objects as
    // perform_with_plane(), because it calls exactly that.
    Model model;
    ModelObject* mo = model.add_object();
    mo->add_volume(TriangleMesh(cube));
    mo->add_instance();
    mo->ensure_on_bed();

    // Cut clones the object it is handed, so the same mo can drive both runs.
    const Transform3d cut_matrix = Geometry::translation_transform(Vec3d(0., 0., 0.5 * CUBE));
    auto cut_with = [&](bool curved) {
        Cut cut(mo, 0, cut_matrix, ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
        const ModelObjectPtrs& res = curved ? cut.perform_with_curved_sheet(sheet) : cut.perform_with_plane();
        std::vector<indexed_triangle_set> out;
        for (const ModelObject* obj : res)
            for (const ModelVolume* v : obj->volumes)
                out.push_back(v->mesh().its);
        return out;
    };

    const std::vector<indexed_triangle_set> flat   = cut_with(false);
    const std::vector<indexed_triangle_set> curved = cut_with(true);

    REQUIRE(flat.size() == curved.size());
    REQUIRE(!flat.empty());
    for (size_t k = 0; k < flat.size(); ++ k) {
        // Same triangle count and the same vertex set, to 1e-9.
        REQUIRE(flat[k].indices.size() == curved[k].indices.size());
        REQUIRE(flat[k].vertices.size() == curved[k].vertices.size());
        for (size_t i = 0; i < flat[k].vertices.size(); ++ i)
            REQUIRE((flat[k].vertices[i] - curved[k].vertices[i]).cwiseAbs().maxCoeff() < 1e-9f);
        for (size_t i = 0; i < flat[k].indices.size(); ++ i)
            REQUIRE(flat[k].indices[i] == curved[k].indices[i]);
    }
}

// ---------------------------------------------------------------------------
// (2) A domed sheet on a 40 mm cube: volume conservation and face heights.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a domed sheet splits a cube by volume and by height", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const float cube_volume = its_volume(cube);
    REQUIRE(cube_volume == Approx(CUBE * CUBE * CUBE).epsilon(1e-4));

    const CurvedCutSheet sheet = dome_sheet(8.0);

    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());

    const double vu = double(its_volume(upper));
    const double vl = double(its_volume(lower));
    // Volume conservation within 1e-6 relative.
    REQUIRE(std::abs(vu + vl - double(cube_volume)) / double(cube_volume) < 1e-6);

    // The cut face: sample f(u,v) at 25 points inside the cube's footprint and
    // check the lower half's top surface really sits at that height. Shoot a
    // ray straight down from above and take the FIRST hit on the lower half;
    // that hit is the mating face.
    auto surface_height = [](const indexed_triangle_set& its, double x, double y, bool topmost) {
        double best = topmost ? -1e30 : 1e30;
        bool   found = false;
        for (const Vec3i32& f : its.indices) {
            const Vec3d a = its.vertices[f(0)].cast<double>();
            const Vec3d b = its.vertices[f(1)].cast<double>();
            const Vec3d c = its.vertices[f(2)].cast<double>();
            // Barycentric containment in XY.
            const double d = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
            if (std::abs(d) < 1e-12)
                continue;
            const double l1 = ((b.y() - c.y()) * (x - c.x()) + (c.x() - b.x()) * (y - c.y())) / d;
            const double l2 = ((c.y() - a.y()) * (x - c.x()) + (a.x() - c.x()) * (y - c.y())) / d;
            const double l3 = 1.0 - l1 - l2;
            if (l1 < -1e-9 || l2 < -1e-9 || l3 < -1e-9)
                continue;
            const double z = l1 * a.z() + l2 * b.z() + l3 * c.z();
            found = true;
            best = topmost ? std::max(best, z) : std::min(best, z);
        }
        REQUIRE(found);
        return best;
    };

    const double half = 0.5 * CUBE;
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i) {
            // Stay off the cube's own edges, where the mating face meets the wall.
            const double x = -half + (double(i) + 0.5) * (CUBE / 5.0);
            const double y = -half + (double(j) + 0.5) * (CUBE / 5.0);
            const double expected = sheet.evaluate_local(x, y);
            const double got_lower = surface_height(lower, x, y, /*topmost*/ true);
            const double got_upper = surface_height(upper, x, y, /*topmost*/ false);
            INFO("x=" << x << " y=" << y << " expected=" << expected);
            REQUIRE(std::abs(got_lower - expected) < 0.02);
            REQUIRE(std::abs(got_upper - expected) < 0.02);
        }
}

// ---------------------------------------------------------------------------
// (3) Both halves are watertight and they do not overlap.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: halves are closed and do not intersect", "[CurvedCut]")
{
    const indexed_triangle_set cube  = centred_cube();
    const CurvedCutSheet       sheet = dome_sheet(8.0);

    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));

    REQUIRE(its_num_open_edges(upper) == 0);
    REQUIRE(its_num_open_edges(lower) == 0);

    // Non-overlap: intersecting the two halves must give (essentially) nothing.
    TriangleMesh              mu(upper), ml(lower);
    std::vector<TriangleMesh> dst;
    const bool                ok = MeshBoolean::mfd::make_boolean(mu, ml, dst, "INTERSECTION");
    REQUIRE(ok);
    double overlap = 0.0;
    for (const TriangleMesh& m : dst)
        overlap += std::abs(double(its_volume(m.its)));
    // Any leftover is boolean noise at the shared face, not real interpenetration.
    REQUIRE(overlap / (CUBE * CUBE * CUBE) < 1e-6);
}

// ---------------------------------------------------------------------------
// (4) Grid resize preserves the surface.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a grid resize keeps the surface", "[CurvedCut]")
{
    // A SMOOTH surface, not a single-cell spike: resampling a 5x5 grid onto a
    // 7x7 one is a refit, not a refinement (the 5x5 nodes at u = 0.25 and 0.75
    // are not 7x7 nodes), so how well the shape survives depends on how much of
    // it lives between the old nodes. A spike concentrated in one cell is the
    // worst case and loses ~0.76 mm out of 8; a surface the control grid can
    // actually resolve - which is what dragging with a falloff brush produces -
    // survives far better. Both are measured below.
    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i) {
            const double x = double(i) / 4.0 * 2.0 - 1.0;
            const double y = double(j) / 4.0 * 2.0 - 1.0;
            sheet.at(i, j) = 8.0 * std::cos(0.5 * PI * x) * std::cos(0.5 * PI * y) - 1.5 * x * y;
        }

    // Reference: the surface as it stands, on a dense sample grid.
    const int    N = 33;
    std::vector<double> ref(size_t(N) * N);
    for (int j = 0; j < N; ++ j)
        for (int i = 0; i < N; ++ i)
            ref[size_t(j) * N + i] = sheet.evaluate(double(i) / (N - 1), double(j) / (N - 1));

    CurvedCutSheet round_trip = sheet;
    round_trip.set_resolution(7);
    REQUIRE(round_trip.resolution() == 7);
    // The 5x5 grid's own nodes are not all nodes of a 7x7 grid, so 7x7 is a
    // re-fit, not a refinement: check the surface stays close, then that coming
    // back to 5x5 lands on the same control values.
    double max_err = 0.0;
    for (int j = 0; j < N; ++ j)
        for (int i = 0; i < N; ++ i)
            max_err = std::max(max_err, std::abs(round_trip.evaluate(double(i) / (N - 1), double(j) / (N - 1)) -
                                                 ref[size_t(j) * N + i]));
    // 5 -> 7 is a REFIT, not a refinement: the 5x5 interior nodes (u = 0.25,
    // 0.75) are not 7x7 nodes, so the shape is re-fitted through a differently
    // phased grid and some detail between the old nodes is lost. On this smooth
    // 8 mm surface that costs ~0.26 mm, about 3% of amplitude. The bound is
    // stated relative to amplitude so it means the same thing if the fixture
    // changes, and it is a REGRESSION guard, not an accuracy claim.
    INFO("5 -> 7 max surface error " << max_err << " mm");
    REQUIRE(max_err < 0.05 * 8.0);

    round_trip.set_resolution(5);
    REQUIRE(round_trip.resolution() == 5);
    double max_ctl_err = 0.0;
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i)
            max_ctl_err = std::max(max_ctl_err, std::abs(round_trip.at(i, j) - sheet.at(i, j)));
    INFO("5 -> 7 -> 5 max control point error " << max_ctl_err << " mm");
    REQUIRE(max_ctl_err < 0.05 * 8.0);

    // The worst case for the record: a spike in a single cell cannot survive a
    // refit onto nodes that miss it. This is a property of the representation,
    // not a defect - it is documented so a later change that makes it WORSE is
    // visible.
    CurvedCutSheet spike = dome_sheet(8.0, 5);
    double spike_err = 0.0;
    {
        std::vector<double> before_spike(size_t(N) * N);
        for (int j = 0; j < N; ++ j)
            for (int i = 0; i < N; ++ i)
                before_spike[size_t(j) * N + i] = spike.evaluate(double(i) / (N - 1), double(j) / (N - 1));
        spike.set_resolution(7);
        for (int j = 0; j < N; ++ j)
            for (int i = 0; i < N; ++ i)
                spike_err = std::max(spike_err, std::abs(spike.evaluate(double(i) / (N - 1), double(j) / (N - 1)) -
                                                         before_spike[size_t(j) * N + i]));
    }
    INFO("5 -> 7 max surface error for a one-cell spike " << spike_err << " mm");
    REQUIRE(spike_err < 1.0);

    // 3 -> 9 -> 3 IS a refinement in both directions (the 3x3 nodes u = 0,
    // 0.5, 1 are all 9x9 nodes), so it must round-trip essentially exactly.
    CurvedCutSheet coarse(3);
    coarse.set_half_size(40.0);
    coarse.at(1, 1) = 6.0;
    coarse.at(0, 2) = -2.0;
    const std::vector<double> before = coarse.values();
    coarse.set_resolution(9);
    coarse.set_resolution(3);
    for (size_t k = 0; k < before.size(); ++ k)
        REQUIRE(coarse.values()[k] == Approx(before[k]).margin(1e-9));
}

// ---------------------------------------------------------------------------
// The building blocks the interactive editor rests on.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: the sheet interpolates its control points", "[CurvedCut]")
{
    CurvedCutSheet sheet(5);
    sheet.set_half_size(20.0);
    sheet.at(2, 2) = 5.0;
    sheet.at(0, 4) = -1.5;

    // Catmull-Rom is interpolating: at a control point the surface IS its value.
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i)
            REQUIRE(sheet.evaluate(sheet.control_u(i), sheet.control_u(j)) == Approx(sheet.at(i, j)).margin(1e-9));

    // control_xy and evaluate_local agree with (u,v).
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i) {
            const Vec2d xy = sheet.control_xy(i, j);
            REQUIRE(sheet.evaluate_local(xy.x(), xy.y()) == Approx(sheet.at(i, j)).margin(1e-9));
        }
}

TEST_CASE("Curved cut: grab and smooth behave like the sculpt brush", "[CurvedCut]")
{
    CurvedCutSheet sheet(5);
    sheet.set_half_size(20.0);

    // A grab centred on the middle control point with a radius that reaches its
    // neighbours: the centre gets the full delta, the neighbours less, the rim
    // nothing.
    sheet.grab(Vec2d(0.0, 0.0), 12.0, 4.0, /*falloff*/ true);
    REQUIRE(sheet.at(2, 2) == Approx(4.0).margin(1e-9));
    REQUIRE(sheet.at(1, 2) > 0.0);
    REQUIRE(sheet.at(1, 2) < 4.0);
    REQUIRE(sheet.at(0, 2) == Approx(0.0).margin(1e-9));
    REQUIRE_FALSE(sheet.is_flat());

    // No falloff: everything inside the radius moves the whole way.
    CurvedCutSheet hard(5);
    hard.set_half_size(20.0);
    hard.grab(Vec2d(0.0, 0.0), 12.0, 4.0, /*falloff*/ false);
    REQUIRE(hard.at(2, 2) == Approx(4.0).margin(1e-9));
    REQUIRE(hard.at(1, 2) == Approx(4.0).margin(1e-9));

    // Smooth pulls the spike down towards its neighbours without moving the
    // surface's mean far.
    const double before = sheet.at(2, 2);
    sheet.smooth(0.5);
    REQUIRE(sheet.at(2, 2) < before);
    REQUIRE(sheet.at(2, 2) > 0.0);

    // Reset really is flat, and a flat sheet is what the flat-cut path keys on.
    sheet.reset();
    REQUIRE(sheet.is_flat());
    REQUIRE(sheet.max_displacement() == 0.0);
}

// Sheet size against boolean time, for the record in the spec. Not an
// assertion about wall-clock (CI machines vary) - it prints, and only fails if
// a size becomes outright unusable.
TEST_CASE("Curved cut: sheet size against boolean cost", "[CurvedCut][.perf]")
{
    const indexed_triangle_set cube  = centred_cube();
    const CurvedCutSheet       sheet = dome_sheet(8.0);

    for (int samples : {32, 64, 128, 192, 256}) {
        indexed_triangle_set upper, lower;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = curved_cut_split(cube, sheet, &upper, &lower, samples);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        REQUIRE(ok);
        BoundingBoxf3 dummy;
        dummy.merge(Vec3d(-20, -20, -20));
        dummy.merge(Vec3d(20, 20, 20));
        const size_t slab_tris = curved_cut_lower_slab(sheet, dummy, samples).indices.size();
        WARN("samples " << samples << "x" << samples
             << "  slab tris " << slab_tris
             << "  both booleans " << ms << " ms"
             << "  result tris " << (upper.indices.size() + lower.indices.size()));
    }
}

// ---------------------------------------------------------------------------
// (2b) The cut MATRIX, not just the sheet: a vertical cut plane, off-centre.
//
// The gizmo lets the base plane be rotated and translated, and the sheet rides
// on that plane's frame. So the interesting failure is not the sheet maths - it
// is whether Cut::perform_with_curved_sheet carries the SAME frame the flat cut
// uses. If it did not, a bent sheet under a rotated plane would land somewhere
// else entirely, or the two halves would not add up to the cube.
//
// Here the plane is rotated 90 degrees about X (so its normal is world -Y: a
// vertical cut plane) and translated 7 mm off centre along that normal. The
// proof is threefold: the halves' volumes sum to the cube, both are closed, and
// the mating face sits at f(u,v) in the PLANE'S OWN frame - which is only true
// if the frame survived the round trip.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a rotated, off-centre cut plane keeps its frame", "[CurvedCut]")
{
    const indexed_triangle_set cube        = centred_cube();
    const double               cube_volume = double(its_volume(cube));

    // A dome, tall enough that a frame mistake could not hide inside tolerance.
    const double         dome   = 8.0;
    const CurvedCutSheet sheet  = dome_sheet(dome);
    const double         offset = 7.0;

    // The gizmo builds the cut matrix as translation * rotation, and Cut splits
    // it back into get_rotation_matrix() and get_offset(); build it the same way.
    // 90 degrees about X sends local +Z to world -Y, so the plane is vertical.
    const Transform3d rotation = Geometry::rotation_transform(Vec3d(0.5 * PI, 0., 0.));
    // Off centre ALONG THE NORMAL, so the cut is not through the middle: the
    // plane's own origin ends up at world (0, -7, 0).
    const Vec3d       plane_origin = rotation * Vec3d(0., 0., offset);
    const Transform3d cut_matrix   = Geometry::translation_transform(plane_origin) * rotation;

    // Sanity: the frame really is rotated and off centre.
    REQUIRE((rotation * Vec3d::UnitZ() - Vec3d(0., -1., 0.)).norm() < 1e-12);
    REQUIRE((plane_origin - Vec3d(0., -offset, 0.)).norm() < 1e-12);

    Model model;
    ModelObject* mo = model.add_object();
    mo->add_volume(TriangleMesh(cube));
    mo->add_instance();
    // NOTE: no ensure_on_bed(). The cube stays centred on the origin so the
    // plane frame above is the frame the assertions below use; dropping it onto
    // the bed would add an instance offset the test would have to undo again.

    Cut cut(mo, 0, cut_matrix, ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& res = cut.perform_with_curved_sheet(sheet);
    REQUIRE(res.size() == 2);

    // Cut re-frames each half onto the cut plane (reset_instance_transformation),
    // so pull the meshes back into world coordinates the way the viewer does.
    auto world_its = [](const ModelObject* obj) {
        indexed_triangle_set out;
        const Transform3d inst = obj->instances.front()->get_transformation().get_matrix();
        for (const ModelVolume* v : obj->volumes) {
            if (!v->is_model_part())
                continue;
            TriangleMesh m(v->mesh());
            m.transform(inst * v->get_matrix(), true);
            its_merge(out, m.its);
        }
        return out;
    };

    const indexed_triangle_set a = world_its(res[0]);
    const indexed_triangle_set b = world_its(res[1]);
    REQUIRE(!a.empty());
    REQUIRE(!b.empty());

    // (i) The halves add up to the cube.
    const double va = double(its_volume(a));
    const double vb = double(its_volume(b));
    REQUIRE(std::abs(va + vb - cube_volume) / cube_volume < 1e-5);
    // Both halves are real: the off-centre plane cannot have thrown one away.
    REQUIRE(va > 0.05 * cube_volume);
    REQUIRE(vb > 0.05 * cube_volume);

    // (ii) Both halves are closed.
    REQUIRE(its_num_open_edges(a) == 0);
    REQUIRE(its_num_open_edges(b) == 0);

    // (iii) The cut face is f(u,v) in the PLANE'S frame. Take every vertex of
    // both halves into that frame; a vertex whose (x,y) is inside the cube's
    // footprint under the plane and whose local z is anywhere near the sheet
    // must be ON the sheet. A frame error - the rotation applied the wrong way
    // round, the offset taken along the wrong axis - moves the face bodily and
    // shows up here immediately.
    const Transform3d world_to_plane = cut_matrix.inverse();

    size_t sampled = 0;
    double worst   = 0.;
    for (const indexed_triangle_set* half : { &a, &b })
        for (const Vec3f& vf : half->vertices) {
            const Vec3d p = world_to_plane * vf.cast<double>();
            // Well inside the cube's cross-section, so this is face, not rim:
            // the cube spans +-20 in x and (after the rotation) +-20 in the
            // plane's y as well.
            if (std::abs(p.x()) > 16.0 || std::abs(p.y()) > 16.0)
                continue;
            const double f = sheet.evaluate_local(p.x(), p.y());
            // The sampled sheet sits a chord sag below the true surface, and the
            // boolean adds vertices along the object's own faces too, so only
            // judge the vertices that are meant to be on the face at all.
            if (std::abs(p.z() - f) > 0.5)
                continue;
            worst = std::max(worst, std::abs(p.z() - f));
            ++ sampled;
        }

    // The face has to have actually been found - a frame error that moved it
    // out of the window above would leave this at zero.
    REQUIRE(sampled > 100);
    INFO("worst deviation from f(u,v) in the plane frame: " << worst << " mm over " << sampled << " vertices");
    REQUIRE(worst < 0.02);

    // And the dome really did bend the cut: the face is not the flat plane.
    // A dome of `dome` mm over the cube's footprint must show up as a spread of
    // local z on the face far larger than the 0.02 mm tolerance above.
    double zmin = 1e30, zmax = -1e30;
    for (const Vec3f& vf : a.vertices) {
        const Vec3d p = world_to_plane * vf.cast<double>();
        if (std::abs(p.x()) > 16.0 || std::abs(p.y()) > 16.0)
            continue;
        if (std::abs(p.z() - sheet.evaluate_local(p.x(), p.y())) > 0.5)
            continue;
        zmin = std::min(zmin, p.z());
        zmax = std::max(zmax, p.z());
    }
    REQUIRE(zmax - zmin > 0.5 * dome);
}

TEST_CASE("Curved cut: the slab is a watertight solid", "[CurvedCut]")
{
    const CurvedCutSheet sheet = dome_sheet(8.0);
    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-20, -20, -20));
    bbox.merge(Vec3d(20, 20, 20));

    for (int samples : {8, 32, 64}) {
        const indexed_triangle_set slab = curved_cut_lower_slab(sheet, bbox, samples);
        INFO("samples = " << samples);
        REQUIRE(its_num_open_edges(slab) == 0);
        // Positive volume with the winding as generated: outward normals point out.
        REQUIRE(its_volume(slab) > 0.f);
    }
}

// ---------------------------------------------------------------------------
// Demo export. Skipped unless EDGESLICER_CURVED_CUT_DEMO_DIR names a directory:
// this is the "cut a cube with a domed sheet, look at both halves" artefact the
// research spec's test plan asks for, produced from the very code under test.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: demo export", "[CurvedCut][.demo]")
{
    const char* dir = std::getenv("EDGESLICER_CURVED_CUT_DEMO_DIR");
    if (dir == nullptr || *dir == '\0')
        return;

    const boost::filesystem::path out(dir);
    boost::filesystem::create_directories(out);

    const indexed_triangle_set cube  = centred_cube();
    const CurvedCutSheet       sheet = dome_sheet(8.0);

    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));

    TriangleMesh mu(upper), ml(lower);
    REQUIRE(store_stl((out / "curved_cut_upper.stl").string().c_str(), &mu, true));
    REQUIRE(store_stl((out / "curved_cut_lower.stl").string().c_str(), &ml, true));

    // Both halves in one 3MF, as the two parts a keep-as-parts cut produces.
    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "curved_cut_demo";
    mo->add_volume(TriangleMesh(upper))->name = "curved_cut_demo_A";
    mo->add_volume(TriangleMesh(lower))->name = "curved_cut_demo_B";
    mo->add_instance();
    mo->ensure_on_bed();
    REQUIRE(store_3mf((out / "curved_cut_demo.3mf").string().c_str(), &model, nullptr, false));

    // A few numbers to read back off the console alongside the files.
    WARN("demo: cube volume " << its_volume(cube)
         << " upper " << its_volume(upper) << " lower " << its_volume(lower)
         << " open edges upper " << its_num_open_edges(upper)
         << " lower " << its_num_open_edges(lower));
}

// ---------------------------------------------------------------------------
// (10) The gizmo's own call sequence must not lose the displacement.
//
// The gizmo re-fits the sheet to the object every time it rebuilds the preview
// model (update_curved_sheet_model -> set_half_size(curved_sheet_half_size())),
// and the control-point slider re-samples the grid. Neither may zero the
// surface, and set_half_size must not move the surface over the object either:
// it changes the sheet's DOMAIN, and the cut has to keep evaluating the same
// f(u,v) over the object's footprint.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: the gizmo's fit sequence keeps the displacement", "[CurvedCut]")
{
    CurvedCutSheet sheet(5);
    // The sequence a drag actually produces: fit, set the grid, drag a handle,
    // then fit again with a different extent (a new bounding box, a moved plane).
    sheet.set_half_size(30.0);
    sheet.set_resolution(5);
    const Vec2d handle = sheet.control_xy(2, 2);
    sheet.grab(handle, 20.0, 6.0, /*falloff*/ true);
    REQUIRE(!sheet.is_flat());
    const double peak = sheet.max_displacement();
    REQUIRE(peak == Approx(6.0));

    sheet.set_half_size(52.0);
    REQUIRE(!sheet.is_flat());
    REQUIRE(sheet.max_displacement() == Approx(peak));

    // A grid resize in between must not flatten it either.
    sheet.set_resolution(9);
    REQUIRE(!sheet.is_flat());
    REQUIRE(sheet.max_displacement() > 0.5 * peak);

    // And the cut must still see a curve: the slab widening curved_cut_split()
    // does may not rescale the sheet, so the height over the object's footprint
    // is the height the gizmo drew.
    const indexed_triangle_set cube = centred_cube();
    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());
    REQUIRE(std::abs(double(its_volume(upper)) + double(its_volume(lower)) - CUBE * CUBE * CUBE) / (CUBE * CUBE * CUBE) < 1e-6);
}

// ---------------------------------------------------------------------------
// (11) Widening the slab past the sheet must not move the surface.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a wider slab keeps the sheet's own heights", "[CurvedCut]")
{
    const CurvedCutSheet sheet = dome_sheet(5.0, 5, 20.0);

    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-60, -60, -20));
    bbox.merge(Vec3d(60, 60, 20));

    // The object reaches far past the sheet, so curved_cut_lower_slab has to be
    // built at 70 mm. The dome's own peak stays where the sheet puts it.
    const indexed_triangle_set slab = curved_cut_lower_slab(sheet, bbox, 128, 70.0);
    REQUIRE(its_num_open_edges(slab) == 0);

    double peak = -1e30, peak_x = 0.0, peak_y = 0.0;
    double half_extent = 0.0;
    for (const Vec3f& v : slab.vertices) {
        half_extent = std::max(half_extent, double(std::abs(v.x())));
        if (double(v.z()) > peak) { peak = double(v.z()); peak_x = double(v.x()); peak_y = double(v.y()); }
    }
    REQUIRE(half_extent >= 69.0);              // the slab really was widened
    // ... and the dome kept its height. The 128-sample grid now spans 140 mm, so its
    // 1.1 mm spacing straddles rather than hits the peak: the sampled maximum sits a
    // chord sag below 5 mm. Nowhere near stretched flat, which is what the old
    // set_half_size() rescale would have produced here (~0.9 mm).
    REQUIRE(peak == Approx(5.0).margin(0.15));
    REQUIRE(std::abs(peak_x) < 2.0);           // ... at the sheet's own centre
    REQUIRE(std::abs(peak_y) < 2.0);
    // Outside the sheet's domain the rim value (zero) is extruded, not stretched.
    // (Floor vertices sit far below; only look at the top sheet.)
    for (const Vec3f& v : slab.vertices)
        if (std::abs(double(v.x())) > 25.0 && double(v.z()) > -10.0)
            REQUIRE(std::abs(double(v.z())) < 0.01);
}

// ---------------------------------------------------------------------------
// (12) A ROTATED, OFFSET cut plane through Cut::perform_with_curved_sheet.
//
// The bug the owner hit: a vertical plane (rotated 90 deg about X) offset from
// the object's centre came out FLAT. This drives the whole Cut path, not
// curved_cut_split() on an axis-aligned mesh, and checks the cut face in the
// CUT PLANE'S OWN frame - the frame the sheet lives in.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a rotated, offset plane cuts curved", "[CurvedCut]")
{
    // A 40 mm cube with its centre at the origin, one instance.
    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
    mo->add_instance();

    // The cut plane: rotated 90 degrees about X (a VERTICAL plane whose normal
    // is world -Y), then offset 10 mm from the object's centre along its normal.
    const Transform3d rotation   = Transform3d(Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitX()));
    const Vec3d       offset     = rotation * Vec3d(0.0, 0.0, 10.0);
    const Transform3d cut_matrix = Geometry::translation_transform(offset) * rotation;

    // A 5 mm dome, the shape the gizmo's centre handle makes.
    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    sheet.at(2, 2) = 5.0;
    REQUIRE(!sheet.is_flat());

    ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;
    Cut cut(mo, 0, cut_matrix, attributes);
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(sheet);
    REQUIRE(parts.size() == 2);

    // Volumes sum to the cube, and both halves are closed.
    double total = 0.0;
    std::vector<double> vols;
    for (const ModelObject* part : parts) {
        REQUIRE(part->volumes.size() == 1);
        const indexed_triangle_set& its = part->volumes.front()->mesh().its;
        REQUIRE(!its.empty());
        REQUIRE(its_num_open_edges(its) == 0);
        const double v = std::abs(double(its_volume(its)));
        vols.push_back(v);
        total += v;
    }
    REQUIRE(std::abs(total - CUBE * CUBE * CUBE) / (CUBE * CUBE * CUBE) < 1e-4);

    // The 10 mm offset means the plane sits 10 mm off centre, so the halves are
    // NOT equal: 30x40x40 and 10x40x40, plus/minus the dome's own volume.
    const size_t small_idx = vols[0] < vols[1] ? 0 : 1;
    REQUIRE(vols[small_idx] < 0.5 * CUBE * CUBE * CUBE);
    REQUIRE(vols[1 - small_idx] > 0.5 * CUBE * CUBE * CUBE);

    // The cut face, back in the CUT PLANE'S frame - the frame the sheet lives in.
    // add_cut_volume() bakes cut_matrix into the mesh it stores, and add_volume()
    // then re-centres that mesh and puts the shift in the volume matrix. So the way
    // back is cut_matrix.inverse() * volume_matrix. The INSTANCE transform must NOT
    // come in: post_process() re-seats it after the cut.
    const ModelObject* small_part = parts[small_idx];
    const Transform3d  m          = cut_matrix.inverse() * small_part->volumes.front()->get_matrix();

    std::vector<Vec3d> pts;
    pts.reserve(small_part->volumes.front()->mesh().its.vertices.size());
    for (const Vec3f& v : small_part->volumes.front()->mesh().its.vertices)
        pts.emplace_back(m * v.cast<double>());

    // The frame mapping itself: the cube spans local z in [-30, +10] (the plane sits
    // 10 mm off centre along its own normal), so the small half is the UPPER slice,
    // between the sheet and local z == +10, and its rim reaches down to z == 0 where
    // the sheet is undisplaced.
    double zmin = 1e30, zmax = -1e30;
    for (const Vec3d& p : pts) { zmin = std::min(zmin, p.z()); zmax = std::max(zmax, p.z()); }
    INFO("small half local z range: [" << zmin << ", " << zmax << "]");
    REQUIRE(zmax == Approx(10.0).margin(0.05));
    REQUIRE(zmin == Approx(0.0).margin(0.05));

    // THE FLAT-RESULT GUARD, in volume. A flat cut at this plane gives a plain
    // 10 x 40 x 40 = 16000 mm3 box. The 5 mm dome lifts the cut face into the upper
    // half and takes a real bite out of it, so a curved cut is measurably smaller -
    // and the missing volume is the dome's own, ~2.3 cm3 for this sheet.
    const double flat_upper = 10.0 * CUBE * CUBE;
    INFO("upper half volume " << vols[small_idx] << " vs flat " << flat_upper);
    REQUIRE(vols[small_idx] < flat_upper - 1000.0);
    REQUIRE(vols[small_idx] > flat_upper - 4000.0);

    // Every point on the cut face has local z == f(x,y) within 0.02 mm, and the
    // face is genuinely NOT planar: for a 5 mm dome its deviation from the best
    // fit plane (z == const, by the dome's symmetry) exceeds 1 mm.
    int    on_face           = 0;
    double max_dev_from_flat = 0.0;
    for (const Vec3d& p : pts) {
        const double f = sheet.evaluate_local(p.x(), p.y());
        if (std::abs(p.z() - f) < 0.02) {
            ++ on_face;
            max_dev_from_flat = std::max(max_dev_from_flat, std::abs(f));
        }
    }
    INFO("points on the cut face: " << on_face << " of " << pts.size());
    REQUIRE(on_face > 20);
    REQUIRE(max_dev_from_flat > 1.0);

    // ... and the half really lies on ONE side of the sheet: being the upper one, no
    // point of it may sit BELOW the sheet beyond boolean noise. This is the check
    // that fails outright if the cut ignored the sheet and went flat at z == 0 - the
    // dome's 5 mm crown would then be 5 mm underneath the flat face.
    double below = 0.0;
    for (const Vec3d& p : pts)
        below = std::max(below, sheet.evaluate_local(p.x(), p.y()) - p.z());
    INFO("deepest point below the sheet: " << below);
    REQUIRE(below < 0.05);
}

// ---------------------------------------------------------------------------
// (13) Rotated-plane demo export, alongside the axis-aligned one.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: rotated demo export", "[CurvedCut][.demo]")
{
    const char* dir = std::getenv("EDGESLICER_CURVED_CUT_DEMO_DIR");
    if (dir == nullptr || dir[0] == 0)
        return;

    const boost::filesystem::path out = boost::filesystem::path(dir) / "rotated";
    boost::filesystem::create_directories(out);

    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
    mo->add_instance();

    const Transform3d rotation   = Transform3d(Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitX()));
    const Vec3d       offset     = rotation * Vec3d(0.0, 0.0, 10.0);
    const Transform3d cut_matrix = Geometry::translation_transform(offset) * rotation;

    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    sheet.at(2, 2) = 5.0;

    Cut cut(mo, 0, cut_matrix, ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(sheet);
    REQUIRE(parts.size() == 2);

    int idx = 0;
    for (const ModelObject* part : parts) {
        TriangleMesh     m(part->volumes.front()->mesh());
        const std::string name = (idx == 0 ? "rotated_cut_A.stl" : "rotated_cut_B.stl");
        REQUIRE(store_stl((out / name).string().c_str(), &m, true));
        ++ idx;
    }
    WARN("rotated demo written to " << out.string());
}

// ===========================================================================
// PHASE 2
//
// (a) A rectangular sheet, and what an extent change does to the surface.
// (b) The cross-section fit.
// (c) The snap helper.
// ===========================================================================

// ---------------------------------------------------------------------------
// (P2-1) A rectangular domain evaluates and samples over the rectangle, and an
// extent change RE-SAMPLES the surface so it stays put in the plane rather than
// stretching with the new rectangle - the same contract set_resolution() has.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a rectangular sheet re-samples on an extent change", "[CurvedCut]")
{
    CurvedCutSheet sheet(7);
    sheet.set_half_size(30.0, 12.0);
    REQUIRE(sheet.half_size_u() == Approx(30.0));
    REQUIRE(sheet.half_size_v() == Approx(12.0));
    // half_size() is the square API: the larger of the two, so a caller that
    // only wants "how big is this sheet" still gets a covering answer.
    REQUIRE(sheet.half_size() == Approx(30.0));

    // The control grid really is laid over the rectangle.
    REQUIRE(sheet.control_xy(0, 0).x() == Approx(-30.0));
    REQUIRE(sheet.control_xy(0, 0).y() == Approx(-12.0));
    REQUIRE(sheet.control_xy(6, 6).x() == Approx(30.0));
    REQUIRE(sheet.control_xy(6, 6).y() == Approx(12.0));

    // ... and so is the dense sample grid.
    {
        const indexed_triangle_set its = sheet.sample_sheet(9);
        double mx = 0.0, my = 0.0;
        for (const Vec3f& v : its.vertices) {
            mx = std::max(mx, double(std::abs(v.x())));
            my = std::max(my, double(std::abs(v.y())));
        }
        REQUIRE(mx == Approx(30.0));
        REQUIRE(my == Approx(12.0));
    }

    // A smooth bump the control grid can actually resolve - the same shape a
    // falloff drag produces, and the case the re-fit has to survive.
    for (int j = 0; j < 7; ++ j)
        for (int i = 0; i < 7; ++ i) {
            const double x = sheet.control_xy(i, j).x();
            const double y = sheet.control_xy(i, j).y();
            sheet.at(i, j) = 6.0 * std::exp(-(x * x) / (2.0 * 14.0 * 14.0) - (y * y) / (2.0 * 6.0 * 6.0));
        }

    // Reference: the surface in LOCAL MILLIMETRES over the region that is inside
    // both the old and the new rectangle. That is the region the contract is
    // about - outside it there is nothing to preserve.
    const double keep_u = 20.0, keep_v = 9.0;
    const int    N = 21;
    std::vector<double> ref(size_t(N) * N);
    for (int j = 0; j < N; ++ j)
        for (int i = 0; i < N; ++ i) {
            const double x = (2.0 * double(i) / (N - 1) - 1.0) * keep_u;
            const double y = (2.0 * double(j) / (N - 1) - 1.0) * keep_v;
            ref[size_t(j) * N + i] = sheet.evaluate_local(x, y);
        }
    const double amp = sheet.max_displacement();
    REQUIRE(amp == Approx(6.0).margin(1e-9));

    // Grow the rectangle, re-sampling. The bump must stay where it was, at the
    // size it was: this is the thing the owner would notice if it broke, because
    // the plane moving would drag his bend around with it.
    CurvedCutSheet grown = sheet;
    grown.set_half_size(45.0, 20.0, /*resample*/ true);
    REQUIRE(grown.half_size_u() == Approx(45.0));
    REQUIRE(grown.half_size_v() == Approx(20.0));
    REQUIRE(!grown.is_flat());

    double max_err = 0.0;
    for (int j = 0; j < N; ++ j)
        for (int i = 0; i < N; ++ i) {
            const double x = (2.0 * double(i) / (N - 1) - 1.0) * keep_u;
            const double y = (2.0 * double(j) / (N - 1) - 1.0) * keep_v;
            max_err = std::max(max_err, std::abs(grown.evaluate_local(x, y) - ref[size_t(j) * N + i]));
        }
    INFO("rectangular re-sample max error " << max_err << " mm of " << amp << " mm amplitude");
    // A re-fit onto a differently phased grid, exactly like the 5 -> 7 grid
    // resize test: some detail between the old nodes is lost. 10% of amplitude
    // is the regression guard, not an accuracy claim.
    REQUIRE(max_err < 0.10 * amp);

    // The peak keeps its height and stays over the centre.
    REQUIRE(grown.evaluate_local(0.0, 0.0) == Approx(6.0).margin(0.10 * amp));

    // WITHOUT resample the old behaviour stands: the control values are kept, so
    // the surface is STRETCHED onto the new rectangle. Documented, not a bug -
    // some callers (and the phase 1 tests) want exactly that.
    CurvedCutSheet stretched = sheet;
    stretched.set_half_size(45.0, 20.0, /*resample*/ false);
    REQUIRE(stretched.values() == sheet.values());
    // The whole bump is now spread over a 45 mm half extent instead of 30, so
    // at a fixed 30 mm out the stretched sheet reads what the original read at
    // 20 mm - i.e. much HIGHER, because the bump has been pulled outwards with
    // the rectangle. That is exactly the drift the `resample` flag exists to
    // prevent, and it is what the fitted sheet must never do.
    REQUIRE(stretched.evaluate_local(30.0, 0.0) > sheet.evaluate_local(30.0, 0.0));
    REQUIRE(stretched.evaluate_local(30.0, 0.0) == Approx(sheet.evaluate_local(20.0, 0.0)).margin(1e-9));
    // ... whereas the RE-SAMPLED sheet still reads what the original read there.
    REQUIRE(grown.evaluate_local(30.0, 0.0) == Approx(sheet.evaluate_local(30.0, 0.0)).margin(0.10 * amp));

    // A flat sheet re-sampled is still exactly flat: the phase 1 invariant.
    CurvedCutSheet flat(5);
    flat.set_half_size(10.0, 3.0);
    flat.set_half_size(40.0, 25.0, /*resample*/ true);
    REQUIRE(flat.is_flat());
}

// ---------------------------------------------------------------------------
// (P2-2) The cross-section fit. A 40 x 20 x 10 box cut by a VERTICAL plane: the
// outline is 40 x 10 (or 20 x 10, depending which way the plane faces), and the
// sheet has to be that plus the margin - not the bbox diagonal phase 1 used.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: the sheet fits the cut's cross-section", "[CurvedCut]")
{
    // A 40 x 20 x 10 box centred on the origin, already in the plane frame.
    auto box = [](double sx, double sy, double sz) {
        indexed_triangle_set its = its_make_cube(sx, sy, sz);
        for (Vec3f& v : its.vertices)
            v -= Vec3f(float(0.5 * sx), float(0.5 * sy), float(0.5 * sz));
        return its;
    };

    const double margin_rel = 0.15, margin_abs = 5.0;

    // A HORIZONTAL cut (the plane frame is the world frame): the section is the
    // 40 x 20 footprint.
    {
        const indexed_triangle_set b = box(40.0, 20.0, 10.0);
        double hu = 0.0, hv = 0.0;
        REQUIRE(curved_cut_fit_extent(b, hu, hv, margin_rel, margin_abs));
        // half extents of the outline: 20 and 10.
        REQUIRE(hu == Approx(20.0 + std::max(0.15 * 20.0, 5.0)));   // 20 + 5   = 25
        REQUIRE(hv == Approx(10.0 + std::max(0.15 * 10.0, 5.0)));   // 10 + 5   = 15
        // And it is nothing like the bbox-diagonal extent phase 1 would use:
        // that is 0.5 * sqrt(40^2 + 20^2 + 10^2) ~ 22.9, scaled up again by the
        // plane's radius koef, and SQUARE - so on the short axis the handles
        // sat well off the part.
        REQUIRE(hv < 0.5 * Vec3d(40, 20, 10).norm());
    }

    // A VERTICAL cut: rotate the box 90 degrees about X, so the plane z == 0 now
    // slices it lengthways and the section is 40 x 10.
    {
        indexed_triangle_set b = box(40.0, 20.0, 10.0);
        its_transform(b, Geometry::rotation_transform(0.5 * PI * Vec3d::UnitX()));
        double hu = 0.0, hv = 0.0;
        REQUIRE(curved_cut_fit_extent(b, hu, hv, margin_rel, margin_abs));
        REQUIRE(hu == Approx(20.0 + 5.0));   // the 40 mm axis is untouched by an X rotation
        REQUIRE(hv == Approx(5.0 + 5.0));    // the 10 mm axis has rotated into the plane
    }

    // A LARGE section takes the relative margin, not the absolute one.
    {
        const indexed_triangle_set b = box(200.0, 100.0, 10.0);
        double hu = 0.0, hv = 0.0;
        REQUIRE(curved_cut_fit_extent(b, hu, hv, margin_rel, margin_abs));
        REQUIRE(hu == Approx(100.0 * 1.15));
        REQUIRE(hv == Approx(50.0 * 1.15));
    }

    // A plane that MISSES the object reports failure, so the caller keeps the
    // extent it has instead of collapsing the sheet to nothing.
    {
        indexed_triangle_set b = box(40.0, 20.0, 10.0);
        its_translate(b, Vec3f(0.f, 0.f, 100.f));
        double hu = 1.0, hv = 1.0;
        REQUIRE(!curved_cut_fit_extent(b, hu, hv, margin_rel, margin_abs));
        REQUIRE(hu == Approx(1.0));   // untouched
        REQUIRE(hv == Approx(1.0));
    }

    // An empty mesh is a failure too, not a crash.
    {
        const indexed_triangle_set empty;
        double hu = 3.0, hv = 4.0;
        REQUIRE(!curved_cut_fit_extent(empty, hu, hv));
        REQUIRE(hu == Approx(3.0));
        REQUIRE(hv == Approx(4.0));
    }
}

// ---------------------------------------------------------------------------
// (P2-3) The snap helper: given a mesh and a handle position in the plane frame,
// the SIGNED local-Z distance to the nearest surface, in either direction.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: the snap helper finds the nearest surface", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();   // 40 mm, faces at +-20

    SECTION("a cube") {
        double d = 0.0;

        // Above the top face: the nearest surface is the top, 5 mm DOWN.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, 25.0), d));
        REQUIRE(d == Approx(-5.0));

        // Below the bottom face: the nearest is the bottom, 5 mm UP.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, -25.0), d));
        REQUIRE(d == Approx(5.0));

        // INSIDE, nearer the top: it snaps out to the top, not through to the
        // bottom. "Nearest hit in either direction" is the whole rule.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, 12.0), d));
        REQUIRE(d == Approx(8.0));

        // Inside, nearer the bottom.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, -12.0), d));
        REQUIRE(d == Approx(-8.0));

        // Exactly on the top face: zero, not a jump to the far one.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, 20.0), d));
        REQUIRE(d == Approx(0.0).margin(1e-9));

        // Off to the side of the cube entirely: no hit, and the caller must
        // leave its control point alone.
        REQUIRE(!curved_cut_snap_distance(cube, Vec3d(100.0, 0.0, 0.0), d));

        // A handle over a corner region but still off the footprint.
        REQUIRE(!curved_cut_snap_distance(cube, Vec3d(21.0, 21.0, 0.0), d));
    }

    SECTION("a domed mesh") {
        // A spherical cap: z = sqrt(R^2 - r^2) - sqrt(R^2 - r_max^2) over a disc
        // of radius r_max, so the apex is at the centre and the rim comes down to
        // z == 0, closed there with a flat base disc. That is what a real STL of
        // a domed top looks like to the helper - a curved shell over a flat face,
        // with the shell nearer over the middle and the base nearer near the rim.
        const double R = 30.0, r_max = 20.0;
        const int    RINGS = 32, SEGS = 64;
        const double z_off = std::sqrt(R * R - r_max * r_max);
        auto dome_z = [R, z_off](double r) { return std::sqrt(std::max(0.0, R * R - r * r)) - z_off; };
        const double apex = dome_z(0.0);   // ~7.64

        indexed_triangle_set dome;
        // Vertex 0 is the apex; then RINGS rings out to the rim at r_max.
        dome.vertices.emplace_back(Vec3f(0.f, 0.f, float(apex)));
        for (int k = 1; k <= RINGS; ++ k) {
            const double r = r_max * double(k) / double(RINGS);
            for (int s = 0; s < SEGS; ++ s) {
                const double a = 2.0 * PI * double(s) / double(SEGS);
                dome.vertices.emplace_back(Vec3f(float(r * std::cos(a)), float(r * std::sin(a)), float(dome_z(r))));
            }
        }
        auto vid = [SEGS](int ring, int seg) { return 1 + (ring - 1) * SEGS + (seg % SEGS); };
        for (int s = 0; s < SEGS; ++ s)
            dome.indices.emplace_back(Vec3i32(0, vid(1, s), vid(1, s + 1)));
        for (int k = 1; k < RINGS; ++ k)
            for (int s = 0; s < SEGS; ++ s) {
                dome.indices.emplace_back(Vec3i32(vid(k, s), vid(k + 1, s), vid(k + 1, s + 1)));
                dome.indices.emplace_back(Vec3i32(vid(k, s), vid(k + 1, s + 1), vid(k, s + 1)));
            }
        // The flat base at z == 0: a fan from the centre out to the rim ring,
        // which really is at z == 0 now, so this is a disc and not a cone.
        const int base_c = int(dome.vertices.size());
        dome.vertices.emplace_back(Vec3f(0.f, 0.f, 0.f));
        for (int s = 0; s < SEGS; ++ s)
            dome.indices.emplace_back(Vec3i32(base_c, vid(RINGS, s + 1), vid(RINGS, s)));

        double d = 0.0;
        // Directly over the apex, 4 mm above it: the shell is 4 mm down, the
        // base further, so it snaps onto the shell.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(0.0, 0.0, apex + 4.0), d));
        REQUIRE(d == Approx(-4.0).margin(1e-6));

        // Over the flank at r = 15, 2 mm above the shell. The MESH is not the
        // analytic surface - a query point between two rings hits a facet that
        // is a chord below the sphere - so the hit is checked against the shell's
        // neighbourhood rather than an exact figure. What matters is that it went
        // DOWN, onto the shell, and not through to the base.
        const double zr   = dome_z(15.0);   // ~5.99
        const double from = zr + 2.0;
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(15.0, 0.0, from), d));
        REQUIRE(d < 0.0);
        const double hit_z = from + d;
        INFO("flank hit at z = " << hit_z << " (analytic shell " << zr << ", apex " << apex << ")");
        REQUIRE(hit_z == Approx(zr).margin(0.15));   // on the shell, within a facet
        REQUIRE(hit_z > 1.0);                        // ... nowhere near the base

        // Near the rim at r = 19.5 the shell is almost down at the base, and a
        // handle just under the base plane snaps UP onto the base.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(19.5, 0.0, -0.3), d));
        REQUIRE(d == Approx(0.3).margin(1e-6));

        // Below the base under the middle: it snaps UP onto the base, not all
        // the way through to the shell above it.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(5.0, 0.0, -6.0), d));
        REQUIRE(d == Approx(6.0).margin(1e-6));

        // Inside the dome, nearer the base than the shell: it snaps DOWN.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(0.0, 0.0, 1.0), d));
        REQUIRE(d == Approx(-1.0).margin(1e-6));

        // Inside, nearer the shell: it snaps UP.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(0.0, 0.0, apex - 1.0), d));
        REQUIRE(d == Approx(1.0).margin(1e-6));

        // Off the disc: no hit.
        REQUIRE(!curved_cut_snap_distance(dome, Vec3d(25.0, 0.0, 5.0), d));
    }
}

// ---------------------------------------------------------------------------
// (P2-4) The end-to-end gesture the gizmo performs: fit the sheet to a section,
// snap a handle onto the surface, and cut. The point is that the pieces compose
// - the fit does not flatten the sheet and the snap produces a real bend.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: fit then snap produces a real curved cut", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();   // 40 mm, faces at +-20

    double hu = 0.0, hv = 0.0;
    REQUIRE(curved_cut_fit_extent(cube, hu, hv, 0.15, 5.0));
    REQUIRE(hu == Approx(25.0));
    REQUIRE(hv == Approx(25.0));

    CurvedCutSheet sheet(5);
    sheet.set_half_size(hu, hv, /*resample*/ true);
    REQUIRE(sheet.is_flat());

    // Snap the centre handle onto the cube's TOP face. A handle sitting exactly
    // at z = 0 is 20 mm from both faces - a genuine tie, and which of the two a
    // tie resolves to is not a contract worth pinning - so lift it 1 mm first,
    // the way a user who wants the top would. The top is then 19 mm away and the
    // bottom 21, so the helper must report +19.
    // (The rim handles are at +-25, off the cube's 20 mm footprint, so they
    // report no hit - which is what leaves them where they are.)
    const Vec2d c = sheet.control_xy(2, 2);
    sheet.at(2, 2) = 1.0;
    double d = 0.0;
    REQUIRE(curved_cut_snap_distance(cube, Vec3d(c.x(), c.y(), sheet.at(2, 2)), d));
    REQUIRE(d == Approx(19.0));
    // A tie really is a tie: from dead centre it lands on one face or the other,
    // 20 mm away, and either is a correct answer.
    {
        double tie = 0.0;
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(c.x(), c.y(), 0.0), tie));
        REQUIRE(std::abs(tie) == Approx(20.0));
    }
    sheet.at(2, 2) = 0.0;
    REQUIRE(sheet.is_flat());

    double corner = 0.0;
    const Vec2d rim = sheet.control_xy(0, 0);
    REQUIRE(!curved_cut_snap_distance(cube, Vec3d(rim.x(), rim.y(), 0.0), corner));

    // Apply it the way the gesture does, with the falloff pulling the
    // neighbours: a dome, not a spike. The sheet starts flat again (the 1 mm
    // lift above was undone), so the peak is the snap distance itself.
    sheet.grab(c, 1.5 * (2.0 * hu / 4.0), d, /*falloff*/ true);
    REQUIRE(!sheet.is_flat());
    REQUIRE(sheet.max_displacement() == Approx(19.0));

    // ... and it still cuts into two closed halves whose volumes add up. The
    // dome reaches to within 1 mm of the top face over the centre, so this is
    // also a near-tangent boolean - the case most likely to produce a sliver.
    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());
    REQUIRE(its_num_open_edges(upper) == 0);
    REQUIRE(its_num_open_edges(lower) == 0);
    // 1e-4 relative, looser than the 1e-6 the phase 1 cube tests hold to,
    // because this is deliberately the near-tangent case: the dome comes to
    // within 1 mm of the top face, so the boolean's seam runs almost along that
    // face and the sliver it leaves is a facet of the 128-sample cutter thick.
    // Both halves are closed (checked above), so nothing leaked - the cutter is
    // discretised, and this is how much that costs.
    const double total = double(its_volume(upper)) + double(its_volume(lower));
    REQUIRE(std::abs(total - CUBE * CUBE * CUBE) / (CUBE * CUBE * CUBE) < 1e-4);
    // The sheet reaches almost to the top face over the middle, so the lower
    // half takes clearly more than half the cube - about 61% here. The bend
    // radius is 1.5 control spacings, so the dome covers the middle and the
    // sheet is still flat at the rim; it is not a raised lid over the whole
    // footprint, and the fraction says so.
    const double lower_frac = double(its_volume(lower)) / (CUBE * CUBE * CUBE);
    INFO("lower half is " << 100.0 * lower_frac << "% of the cube");
    REQUIRE(lower_frac > 0.55);
    REQUIRE(lower_frac < 0.75);
}

// ---------------------------------------------------------------------------
// (P2-5) The slab still covers a part LARGER than the sheet, now that the sheet
// is fitted to a cross-section and so is routinely smaller than the object.
// This is the phase 1 guarantee restated for a RECTANGULAR sheet: the widening
// is per axis and it must not move the surface.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a rectangular sheet's slab still covers a larger part", "[CurvedCut]")
{
    // A deliberately lopsided sheet: 18 mm along u, 7 mm along v, with a dome.
    CurvedCutSheet sheet(5);
    sheet.set_half_size(18.0, 7.0);
    sheet.at(2, 2) = 5.0;

    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-60, -35, -20));
    bbox.merge(Vec3d(60, 35, 20));

    // Widened per axis, as curved_cut_split() does.
    const indexed_triangle_set slab = curved_cut_lower_slab(sheet, bbox, 128, 65.0, 40.0);
    REQUIRE(its_num_open_edges(slab) == 0);

    double ext_u = 0.0, ext_v = 0.0, peak = -1e30, peak_x = 0.0, peak_y = 0.0;
    for (const Vec3f& v : slab.vertices) {
        ext_u = std::max(ext_u, double(std::abs(v.x())));
        ext_v = std::max(ext_v, double(std::abs(v.y())));
        if (double(v.z()) > peak) { peak = double(v.z()); peak_x = double(v.x()); peak_y = double(v.y()); }
    }
    REQUIRE(ext_u >= 64.0);        // really widened past the object on u ...
    REQUIRE(ext_v >= 39.0);        // ... and on v, independently
    // ... and the dome kept its height at the sheet's own centre. (The sample
    // grid now straddles the peak, so a chord sag below 5 mm is expected.)
    REQUIRE(peak == Approx(5.0).margin(0.25));
    REQUIRE(std::abs(peak_x) < 3.0);
    REQUIRE(std::abs(peak_y) < 3.0);
    // Outside the sheet's own domain the border height (zero) is extruded, not
    // stretched: look only at the top surface, the floor sits far below.
    for (const Vec3f& v : slab.vertices)
        if (double(v.z()) > -10.0 && (double(std::abs(v.x())) > 22.0 || double(std::abs(v.y())) > 11.0))
            REQUIRE(std::abs(double(v.z())) < 0.01);

    // And the whole thing still cuts a part far larger than the sheet in two.
    indexed_triangle_set big = its_make_cube(100.0, 60.0, 30.0);
    its_translate(big, Vec3f(-50.f, -30.f, -15.f));
    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(big, sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());
    REQUIRE(its_num_open_edges(upper) == 0);
    REQUIRE(its_num_open_edges(lower) == 0);
    // 1e-4 relative, not the 1e-6 the 40 mm cube tests use: the slab here is
    // sampled at 128 across 130 mm, so its facets are ~1 mm and the boolean's
    // seam follows them. That is a discretisation of the CUTTER, not a leak -
    // both halves are closed (checked above) and nothing is lost between them
    // beyond a facet's worth.
    const double total = double(its_volume(upper)) + double(its_volume(lower));
    REQUIRE(std::abs(total - 100.0 * 60.0 * 30.0) / (100.0 * 60.0 * 30.0) < 1e-4);
}

// ---------------------------------------------------------------------------
// (P2-6) The resolution ceiling moved to 15, and a 15 x 15 grid still behaves:
// it re-samples, it evaluates, and it cuts.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a 15 x 15 control grid", "[CurvedCut]")
{
    REQUIRE(CurvedCutSheet::MaxResolution == 15);

    CurvedCutSheet sheet(5);
    sheet.set_half_size(20.0, 20.0);
    sheet.at(2, 2) = 6.0;
    const double before = sheet.evaluate(0.5, 0.5);
    REQUIRE(before == Approx(6.0));

    sheet.set_resolution(15);
    REQUIRE(sheet.resolution() == 15);
    REQUIRE(sheet.values().size() == 15u * 15u);
    // 5 -> 15 IS a refinement (u = 0, .25, .5, .75, 1 are all 15-grid nodes), so
    // the surface must come through essentially exactly.
    REQUIRE(sheet.evaluate(0.5, 0.5) == Approx(before).margin(1e-9));
    REQUIRE(sheet.max_displacement() == Approx(6.0).margin(1e-9));

    // Clamping still holds at the top of the range.
    CurvedCutSheet over(5);
    over.set_resolution(99);
    REQUIRE(over.resolution() == 15);

    // And a 15 x 15 sheet cuts.
    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(centred_cube(), sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());
}
