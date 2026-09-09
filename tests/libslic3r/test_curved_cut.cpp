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
