#include <catch2/catch.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>

#include "libslic3r/MeshSculpt.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;
using namespace Slic3r::Sculpt;

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

// A flat n x n grid of quads in the z = 0 plane, spacing h, corner at the origin.
static indexed_triangle_set make_grid(int n, float h)
{
    indexed_triangle_set its;
    its.vertices.reserve(size_t(n) * size_t(n));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            its.vertices.emplace_back(Vec3f(float(x) * h, float(y) * h, 0.f));
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            const int a = y * n + x, b = a + 1, c = a + n, d = c + 1;
            its.indices.emplace_back(stl_triangle_vertex_indices(a, b, d));
            its.indices.emplace_back(stl_triangle_vertex_indices(a, d, c));
        }
    return its;
}

static indexed_triangle_set make_noisy_sphere(double r, double fa, float amplitude)
{
    indexed_triangle_set its = its_make_sphere(r, fa);
    std::mt19937 rng(20260907u);
    std::uniform_real_distribution<float> dist(-amplitude, amplitude);
    for (Vec3f &v : its.vertices) {
        const float len = v.norm();
        if (len > 1e-6f)
            v += (v / len) * dist(rng);
    }
    return its;
}

static indexed_triangle_set subdivide_to(indexed_triangle_set its, size_t target_triangles)
{
    // Stop before overshooting the target by more than half.
    while (its.indices.size() * 4 <= target_triangles + target_triangles / 2)
        its = its_subdivide_midpoint(its);
    return its;
}

// ----------------------------------------------------------------------------
// falloff
// ----------------------------------------------------------------------------

TEST_CASE("Sculpt brush falloff is the smooth quartic bump", "[Sculpt]")
{
    const float r = 4.f;

    CHECK(falloff_weight(0.f, r) == Approx(1.f));
    CHECK(falloff_weight(r, r) == Approx(0.f));
    CHECK(falloff_weight(r * 1.5f, r) == Approx(0.f));
    CHECK(falloff_weight(-1.f, r) == Approx(1.f));

    // (1 - (d/r)^2)^2
    CHECK(falloff_weight(0.5f * r, r) == Approx(0.5625f));
    CHECK(falloff_weight(0.25f * r, r) == Approx(0.87890625f));

    // A degenerate brush touches nothing.
    CHECK(falloff_weight(0.f, 0.f) == Approx(0.f));
    CHECK(falloff_weight(0.f, -1.f) == Approx(0.f));

    // Monotonically decreasing over the brush.
    float prev = falloff_weight(0.f, r);
    for (int i = 1; i <= 40; ++i) {
        const float w = falloff_weight(float(i) * r / 40.f, r);
        CHECK(w <= prev);
        prev = w;
    }
}

// ----------------------------------------------------------------------------
// Grab
// ----------------------------------------------------------------------------

TEST_CASE("Grab moves exactly the vertices under the brush, by the falloff weight", "[Sculpt]")
{
    const float h = 0.5f;
    const int   n = 41;
    const indexed_triangle_set original = make_grid(n, h);

    SculptSession session(original);

    BrushParams p;
    p.type         = BrushType::Grab;
    p.center       = Vec3f(10.f, 10.f, 0.f); // the middle vertex of the grid
    p.radius       = 3.f;
    p.strength     = 1.f;
    p.falloff      = true;
    p.displacement = Vec3f(0.f, 0.f, 2.f);

    const StrokeStep step = session.apply(p);
    REQUIRE_FALSE(step.empty());

    const indexed_triangle_set &after = session.mesh();
    REQUIRE(after.vertices.size() == original.vertices.size());

    size_t moved = 0, untouched = 0;
    for (size_t i = 0; i < original.vertices.size(); ++i) {
        const Vec3f &o = original.vertices[i];
        const Vec3f &a = after.vertices[i];
        const float  d = (o - p.center).norm();
        if (d >= p.radius) {
            // Outside the brush: bit-identical, not merely close.
            CHECK(a.x() == o.x());
            CHECK(a.y() == o.y());
            CHECK(a.z() == o.z());
            ++untouched;
        } else {
            const Vec3f expected = o + falloff_weight(d, p.radius) * p.displacement;
            CHECK(a.x() == Approx(expected.x()));
            CHECK(a.y() == Approx(expected.y()));
            CHECK(a.z() == Approx(expected.z()).margin(1e-5));
            ++moved;
        }
    }
    CHECK(moved > 50);
    CHECK(untouched > 100);

    // The centre vertex gets the whole displacement.
    const size_t centre = size_t((n / 2) * n + (n / 2));
    CHECK(after.vertices[centre].z() == Approx(2.f));

    // The stroke reported every vertex it moved, and every triangle that needs
    // a refresh is incident to one of them.
    CHECK(step.moved_vertices.size() == moved);
    CHECK_FALSE(step.dirty_triangles.empty());
    CHECK(std::is_sorted(step.moved_vertices.begin(), step.moved_vertices.end()));
    CHECK(std::is_sorted(step.dirty_triangles.begin(), step.dirty_triangles.end()));
}

TEST_CASE("Grab with the falloff off moves the whole brush disc uniformly", "[Sculpt]")
{
    const indexed_triangle_set original = make_grid(31, 0.5f);
    SculptSession session(original);

    BrushParams p;
    p.type         = BrushType::Grab;
    p.center       = Vec3f(7.5f, 7.5f, 0.f);
    p.radius       = 2.f;
    p.strength     = 1.f;
    p.falloff      = false;
    p.displacement = Vec3f(1.f, 0.f, 0.f);

    session.apply(p);
    const indexed_triangle_set &after = session.mesh();

    for (size_t i = 0; i < original.vertices.size(); ++i) {
        const float d = (original.vertices[i] - p.center).norm();
        const Vec3f expected = d < p.radius ? Vec3f(original.vertices[i] + p.displacement)
                                             : original.vertices[i];
        CHECK((after.vertices[i] - expected).norm() == Approx(0.f).margin(1e-5));
    }
}

TEST_CASE("Strength scales a Grab stroke linearly", "[Sculpt]")
{
    const indexed_triangle_set original = make_grid(21, 0.5f);

    BrushParams p;
    p.type         = BrushType::Grab;
    p.center       = Vec3f(5.f, 5.f, 0.f);
    p.radius       = 2.f;
    p.falloff      = true;
    p.displacement = Vec3f(0.f, 0.f, 1.f);

    p.strength = 1.f;
    SculptSession full(original);
    full.apply(p);

    p.strength = 0.25f;
    SculptSession quarter(original);
    quarter.apply(p);

    for (size_t i = 0; i < original.vertices.size(); ++i) {
        const float df = full.mesh().vertices[i].z() - original.vertices[i].z();
        const float dq = quarter.mesh().vertices[i].z() - original.vertices[i].z();
        CHECK(dq == Approx(0.25f * df).margin(1e-6));
    }
}

// ----------------------------------------------------------------------------
// Inflate
// ----------------------------------------------------------------------------

TEST_CASE("Inflate moves vertices along their pre-stroke normals", "[Sculpt]")
{
    const indexed_triangle_set original = its_make_sphere(10.0, 2 * PI / 60);
    SculptSession session(original);
    const std::vector<Vec3f> normals_before = session.vertex_normals();

    BrushParams p;
    p.type     = BrushType::Inflate;
    p.center   = Vec3f(0.f, 0.f, 10.f); // the north pole
    p.radius   = 4.f;
    p.strength = 1.f;
    p.falloff  = true;
    p.amount   = 1.5f;

    const StrokeStep step = session.apply(p);
    REQUIRE_FALSE(step.empty());

    size_t checked = 0;
    for (uint32_t v : step.moved_vertices) {
        const Vec3f delta = session.mesh().vertices[v] - original.vertices[v];
        const float d     = (original.vertices[v] - p.center).norm();
        const float w     = falloff_weight(d, p.radius);
        if (w < 1e-4f)
            continue;
        // Parallel to the pre-stroke vertex normal ...
        const Vec3f n = normals_before[v];
        CHECK(delta.cross(n).norm() == Approx(0.f).margin(1e-4));
        CHECK(delta.dot(n) > 0.f);
        // ... with the expected magnitude.
        CHECK(delta.norm() == Approx(w * p.amount).margin(1e-4));
        ++checked;
    }
    CHECK(checked > 10);

    // Everything outside the brush is bit-identical.
    for (size_t i = 0; i < original.vertices.size(); ++i)
        if ((original.vertices[i] - p.center).norm() >= p.radius)
            CHECK(session.mesh().vertices[i] == original.vertices[i]);

    // Inflating grows the mesh; deflating with the same settings shrinks it.
    SculptSession deflated(original);
    p.deflate = true;
    deflated.apply(p);
    CHECK(its_volume(session.mesh()) > its_volume(original));
    CHECK(its_volume(deflated.mesh()) < its_volume(original));
}

// ----------------------------------------------------------------------------
// Smooth
// ----------------------------------------------------------------------------

TEST_CASE("Smooth reduces the roughness of a noisy sphere", "[Sculpt]")
{
    const indexed_triangle_set noisy = make_noisy_sphere(10.0, 2 * PI / 90, 0.25f);
    const float energy_before = its_laplacian_energy(noisy);

    SculptSession session(noisy);
    BrushParams p;
    p.type       = BrushType::Smooth;
    p.center     = Vec3f::Zero();
    p.radius     = 100.f; // the whole mesh
    p.strength   = 1.f;
    p.falloff    = false;
    p.iterations = 5;
    p.taubin     = false;
    p.lambda     = 0.6307f;

    session.apply(p);
    const float energy_after = its_laplacian_energy(session.mesh());

    INFO("energy before " << energy_before << " after " << energy_after);
    CHECK(energy_after < energy_before);
    CHECK(energy_after < 0.5f * energy_before);
}

TEST_CASE("Taubin smoothing preserves the volume, plain Laplacian does not", "[Sculpt]")
{
    const indexed_triangle_set noisy = make_noisy_sphere(10.0, 2 * PI / 90, 0.2f);
    const float volume_before = its_volume(noisy);
    REQUIRE(volume_before > 0.f);

    BrushParams p;
    p.center   = Vec3f::Zero();
    p.radius   = 100.f;
    p.strength = 1.f;
    p.falloff  = false;
    p.type     = BrushType::Smooth;
    p.lambda   = 0.6307f;
    p.mu       = -0.6732f;

    // Taubin: 5 iterations = 5 lambda passes + 5 mu passes.
    SculptSession taubin(noisy);
    p.taubin     = true;
    p.iterations = 5;
    taubin.apply(p);
    const float volume_taubin = its_volume(taubin.mesh());

    // Plain Laplacian, the same number of passes.
    SculptSession plain(noisy);
    p.taubin     = false;
    p.iterations = 10;
    plain.apply(p);
    const float volume_plain = its_volume(plain.mesh());

    const float err_taubin = std::abs(volume_taubin / volume_before - 1.f);
    const float err_plain  = std::abs(volume_plain / volume_before - 1.f);

    INFO("volume before " << volume_before << " taubin " << volume_taubin << " plain " << volume_plain);
    CHECK(err_taubin < 0.005f);      // within 0.5 %
    CHECK(volume_plain < volume_before); // plain Laplacian shrinks
    CHECK(err_plain > err_taubin);
    CHECK(err_plain > 0.01f);

    // Taubin still smooths.
    CHECK(its_laplacian_energy(taubin.mesh()) < its_laplacian_energy(noisy));
}

// ----------------------------------------------------------------------------
// Topology invariants
// ----------------------------------------------------------------------------

TEST_CASE("A sculpt stroke never changes the triangle indices", "[Sculpt]")
{
    const indexed_triangle_set original = its_make_sphere(8.0, 2 * PI / 60);
    SculptSession session(original);

    BrushParams p;
    p.center   = Vec3f(0.f, 0.f, 8.f);
    p.radius   = 3.f;
    p.strength = 1.f;

    p.type = BrushType::Inflate;
    p.amount = 0.5f;
    session.apply(p);

    p.type         = BrushType::Grab;
    p.displacement = Vec3f(0.5f, 0.2f, 0.f);
    session.apply(p);

    p.type       = BrushType::Smooth;
    p.iterations = 3;
    session.apply(p);

    REQUIRE(session.mesh().indices.size() == original.indices.size());
    for (size_t i = 0; i < original.indices.size(); ++i)
        REQUIRE(session.mesh().indices[i] == original.indices[i]);
    REQUIRE(session.mesh().vertices.size() == original.vertices.size());

    // Something did move, otherwise the invariant is vacuous.
    bool changed = false;
    for (size_t i = 0; i < original.vertices.size() && !changed; ++i)
        changed = session.mesh().vertices[i] != original.vertices[i];
    CHECK(changed);

    // Vertex-only edits cannot open the mesh up.
    CHECK(its_num_open_edges(session.mesh()) == its_num_open_edges(original));
    CHECK(its_num_open_edges(session.mesh()) == 0);
}

TEST_CASE("Midpoint subdivision quadruples the faces and stays watertight", "[Sculpt]")
{
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    REQUIRE(its_num_open_edges(cube) == 0);

    const indexed_triangle_set sub = its_subdivide_midpoint(cube);
    CHECK(sub.indices.size() == 4 * cube.indices.size());
    // V' = V + E, and for a closed triangle mesh E = 3F/2.
    CHECK(sub.vertices.size() == cube.vertices.size() + (3 * cube.indices.size()) / 2);
    CHECK(its_num_open_edges(sub) == 0);
    // A midpoint subdivision of a polyhedron does not move the surface.
    CHECK(its_volume(sub) == Approx(its_volume(cube)).epsilon(1e-4));

    // The edges really did halve.
    CHECK(its_average_edge_length(sub) < its_average_edge_length(cube));

    // A sphere too, where the shared-midpoint bookkeeping matters more.
    const indexed_triangle_set sphere = its_make_sphere(10.0, 2 * PI / 30);
    const indexed_triangle_set sphere_sub = its_subdivide_midpoint(sphere);
    CHECK(its_num_open_edges(sphere_sub) == 0);
    CHECK(sphere_sub.indices.size() == 4 * sphere.indices.size());
}

TEST_CASE("The session reports the local edge length under the brush", "[Sculpt]")
{
    const indexed_triangle_set grid = make_grid(21, 0.5f);
    SculptSession session(grid);

    const float len = session.local_edge_length(Vec3f(5.f, 5.f, 0.f), 2.f);
    // A right-angled grid triangle has two 0.5 legs and one 0.5*sqrt(2) hypotenuse.
    CHECK(len == Approx((0.5f + 0.5f + 0.5f * std::sqrt(2.f)) / 3.f).epsilon(0.05));

    // Away from the mesh there is nothing to measure.
    CHECK(session.local_edge_length(Vec3f(100.f, 100.f, 100.f), 1.f) == Approx(-1.f));
}

// ----------------------------------------------------------------------------
// The commit path: annotations survive
// ----------------------------------------------------------------------------

TEST_CASE("Painted facets survive a sculpt commit", "[Sculpt]")
{
    indexed_triangle_set its = its_subdivide_midpoint(its_subdivide_midpoint(its_make_cube(20., 20., 20.)));

    Model        model;
    ModelObject *object = model.add_object();
    REQUIRE(object != nullptr);
    ModelVolume *mv = object->add_volume(TriangleMesh(its), /* modify_to_center_geometry */ false);
    REQUIRE(mv != nullptr);
    object->add_instance();

    // Paint a few facets of each kind.
    {
        TriangleMesh     mesh(its);
        TriangleSelector selector(mesh);
        selector.set_facet(0, EnforcerBlockerType::ENFORCER);
        selector.set_facet(7, EnforcerBlockerType::BLOCKER);
        mv->supported_facets.set(selector);
    }
    {
        TriangleMesh     mesh(its);
        TriangleSelector selector(mesh);
        selector.set_facet(3, EnforcerBlockerType::ENFORCER);
        mv->seam_facets.set(selector);
    }
    {
        TriangleMesh     mesh(its);
        TriangleSelector selector(mesh);
        selector.set_facet(11, static_cast<EnforcerBlockerType>(3));
        mv->mmu_segmentation_facets.set(selector);
    }
    {
        TriangleMesh     mesh(its);
        TriangleSelector selector(mesh);
        selector.set_facet(5, EnforcerBlockerType::ENFORCER);
        mv->fuzzy_skin_facets.set(selector);
    }

    REQUIRE_FALSE(mv->supported_facets.empty());
    REQUIRE_FALSE(mv->seam_facets.empty());
    REQUIRE_FALSE(mv->mmu_segmentation_facets.empty());
    REQUIRE_FALSE(mv->fuzzy_skin_facets.empty());

    const TriangleSelector::TriangleSplittingData supports_before = mv->supported_facets.get_data();
    const TriangleSelector::TriangleSplittingData seams_before    = mv->seam_facets.get_data();
    const TriangleSelector::TriangleSplittingData mmu_before      = mv->mmu_segmentation_facets.get_data();
    const TriangleSelector::TriangleSplittingData fuzzy_before    = mv->fuzzy_skin_facets.get_data();

    // Sculpt a bump on the top face.
    SculptSession session(mv->mesh().its);
    BrushParams p;
    p.type     = BrushType::Inflate;
    p.center   = Vec3f(10.f, 10.f, 20.f);
    p.radius   = 6.f;
    p.strength = 1.f;
    p.amount   = 2.5f;
    REQUIRE_FALSE(session.apply(p).empty());

    indexed_triangle_set sculpted = session.mesh();
    REQUIRE(indices_match(*mv, sculpted));
    REQUIRE(commit_sculpted_mesh(*mv, std::move(sculpted), /* ensure_on_bed */ false));

    // The mesh really changed ...
    bool changed = false;
    for (size_t i = 0; i < its.vertices.size() && !changed; ++i)
        changed = mv->mesh().its.vertices[i] != its.vertices[i];
    CHECK(changed);
    // ... the indices did not ...
    REQUIRE(mv->mesh().its.indices.size() == its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i)
        REQUIRE(mv->mesh().its.indices[i] == its.indices[i]);
    // ... and every painted store is byte-for-byte what it was.
    CHECK(mv->supported_facets.get_data() == supports_before);
    CHECK(mv->seam_facets.get_data() == seams_before);
    CHECK(mv->mmu_segmentation_facets.get_data() == mmu_before);
    CHECK(mv->fuzzy_skin_facets.get_data() == fuzzy_before);
    CHECK_FALSE(mv->supported_facets.empty());
    CHECK_FALSE(mv->mmu_segmentation_facets.empty());

    // The convex hull was refreshed along with the mesh.
    CHECK(mv->get_convex_hull().its.vertices.size() > 0);
}

TEST_CASE("A commit with different indices is refused", "[Sculpt]")
{
    const indexed_triangle_set its = its_make_cube(10., 10., 10.);

    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *mv     = object->add_volume(TriangleMesh(its), /* modify_to_center_geometry */ false);
    object->add_instance();

    indexed_triangle_set subdivided = its_subdivide_midpoint(its);
    CHECK_FALSE(indices_match(*mv, subdivided));
    CHECK_FALSE(commit_sculpted_mesh(*mv, std::move(subdivided)));
    // Nothing happened.
    CHECK(mv->mesh().its.indices.size() == its.indices.size());
}

// ----------------------------------------------------------------------------
// An STL a CLI slice can chew on
// ----------------------------------------------------------------------------

TEST_CASE("Export a sculpted part for a CLI slice", "[Sculpt][SculptExport]")
{
    indexed_triangle_set its = its_make_cube(20., 20., 20.);
    for (int i = 0; i < 4; ++i)
        its = its_subdivide_midpoint(its);
    REQUIRE(its.indices.size() == 12 * 256);

    SculptSession session(its);
    BrushParams p;
    p.type     = BrushType::Inflate;
    p.center   = Vec3f(10.f, 10.f, 20.f);
    p.radius   = 6.f;
    p.strength = 1.f;
    p.amount   = 4.f;
    REQUIRE_FALSE(session.apply(p).empty());

    // Relax the shoulder of the bump so the slicer sees a clean dome.
    p.type       = BrushType::Smooth;
    p.radius     = 8.f;
    p.iterations = 3;
    p.taubin     = true;
    session.apply(p);

    const indexed_triangle_set &out = session.mesh();
    CHECK(its_num_open_edges(out) == 0);
    CHECK(its_volume(out) > its_volume(its));

    const char *path = "sculpted_bump.stl";
    REQUIRE(its_write_stl_binary(path, "sculpted_bump", out));
    std::cout << "[sculpt] exported " << path << " (" << out.indices.size() << " triangles)" << std::endl;
}

// ----------------------------------------------------------------------------
// Latency
// ----------------------------------------------------------------------------

static void bench_stroke(size_t target_triangles, const char *label)
{
    using clock = std::chrono::steady_clock;

    indexed_triangle_set its = subdivide_to(its_make_sphere(20.0, 2 * PI / 60), target_triangles);

    const auto t0 = clock::now();
    SculptSession session(its);
    const auto t1 = clock::now();

    BrushParams p;
    p.type     = BrushType::Inflate;
    p.center   = Vec3f(0.f, 0.f, 20.f);
    p.radius   = 3.f;
    p.strength = 1.f;
    p.amount   = 0.01f;

    // Warm up, then time 50 ticks the way a drag would issue them.
    session.apply(p);
    const auto t2 = clock::now();
    const int ticks = 50;
    size_t touched = 0;
    for (int i = 0; i < ticks; ++i)
        touched = session.apply(p).moved_vertices.size();
    const auto t3 = clock::now();

    const auto t4a = clock::now();
    session.rebuild_tree();
    const auto t4b = clock::now();

    const double setup_ms  = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double tick_ms   = std::chrono::duration<double, std::milli>(t3 - t2).count() / double(ticks);
    const double retree_ms = std::chrono::duration<double, std::milli>(t4b - t4a).count();

    std::cout << "[sculpt-bench] " << label
              << " triangles=" << its.indices.size()
              << " vertices=" << its.vertices.size()
              << " session_setup_ms=" << setup_ms
              << " brush_tick_ms=" << tick_ms
              << " vertices_touched=" << touched
              << " tree_rebuild_ms=" << retree_ms
              << std::endl;

    // A tick costs what is under the brush, not what is in the mesh. On a mesh
    // this fine the brush still covers tens of thousands of vertices, so the
    // bound here is generous - the number printed above is the interesting part.
    CHECK(tick_ms < 60.0);
}

TEST_CASE("Sculpt stroke latency on large meshes", "[Sculpt][SculptBench]")
{
    bench_stroke(200000, "200k");
    bench_stroke(1000000, "1M");
}
