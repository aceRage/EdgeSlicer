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
