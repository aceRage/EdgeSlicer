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
// Flatten
// ----------------------------------------------------------------------------

// A flat grid with a bump raised in the middle of the brush: exactly the patch a
// Flatten stroke is for.
static indexed_triangle_set make_bumped_grid(int n, float h, const Vec3f &centre, float bump_radius, float bump_height)
{
    indexed_triangle_set its = make_grid(n, h);
    for (Vec3f &v : its.vertices) {
        const float d = (Vec3f(v.x(), v.y(), 0.f) - Vec3f(centre.x(), centre.y(), 0.f)).norm();
        // A smooth bump so the surface stays well behaved, plus a saw-tooth so
        // the patch is genuinely rough rather than a single clean dome.
        v.z() += bump_height * falloff_weight(d, bump_radius);
        v.z() += 0.05f * bump_height * std::sin(7.f * v.x()) * std::cos(7.f * v.y());
    }
    return its;
}

TEST_CASE("Flatten reduces the plane-distance variance of a bumped patch", "[Sculpt][SculptFlatten]")
{
    const Vec3f centre(10.f, 10.f, 0.f);
    const indexed_triangle_set bumped = make_bumped_grid(41, 0.5f, centre, 3.f, 1.5f);

    SculptSession session(bumped);

    BrushParams p;
    p.type     = BrushType::Flatten;
    p.center   = Vec3f(centre.x(), centre.y(), session.mesh().vertices[size_t(20 * 41 + 20)].z());
    p.radius   = 3.f;
    p.strength = 1.f;
    p.falloff  = true;

    // The plane the brush fits, and the variance about it, measured before the
    // stroke on exactly the vertices the stroke will touch.
    std::vector<uint32_t> verts = session.vertices_in_radius(p.center, p.radius);
    REQUIRE(verts.size() > 20);
    Vec3f origin = Vec3f::Zero(), normal = Vec3f::Zero();
    REQUIRE(fit_plane(session.mesh(), session.vertex_normals(), verts, p.center, p.radius, p.falloff, origin, normal));
    // The grid is z = f(x, y) with a modest bump, so the fitted plane still
    // faces roughly up.
    CHECK(std::abs(normal.z()) > 0.9f);

    const float variance_before = plane_distance_variance(session.mesh(), verts, origin, normal);
    REQUIRE(variance_before > 0.f);

    const StrokeStep step = session.apply(p);
    REQUIRE_FALSE(step.empty());

    const float variance_after = plane_distance_variance(session.mesh(), verts, origin, normal);
    INFO("variance before " << variance_before << " after " << variance_after);
    CHECK(variance_after < variance_before);

    // The vertex at the very centre of the brush has weight 1: it lands exactly
    // ON the plane in a single tick.
    const uint32_t centre_v = uint32_t(20 * 41 + 20);
    CHECK(std::abs((session.mesh().vertices[centre_v] - origin).dot(normal)) < 1e-4f);

    // One tick with the falloff on cannot flatten the whole patch: weight 1 only
    // at the centre, tailing to 0 at the rim, so the rim barely moves. What must
    // hold is that the brush CONVERGES - hold the plane and keep stroking, and
    // the patch flattens against it. Ten more ticks take better than 80 % out.
    for (int i = 0; i < 10; ++i)
        session.apply(p);
    const float variance_converged = plane_distance_variance(session.mesh(), verts, origin, normal);
    INFO("variance converged " << variance_converged);
    CHECK(variance_converged < variance_after);

    // Convergence is real but gradual, and deliberately so. The plane's offset
    // is refitted each tick as the weighted centroid of the very vertices being
    // moved, so the plane follows the surface down and each tick takes out only
    // its share; and a rim vertex has weight ~0 and is MEANT to stay put, so the
    // brush blends into the surrounding surface instead of stamping a disc into
    // it. Eleven ticks take roughly a third out over the whole brush, and the
    // sequence is monotone - which is the property that matters. A brush that
    // overshot or oscillated would fail this even though a single tick passed.
    CHECK(variance_converged < 0.75f * variance_before);
    float prev = variance_converged;
    for (int i = 0; i < 10; ++i) {
        session.apply(p);
        const float now = plane_distance_variance(session.mesh(), verts, origin, normal);
        INFO("tick " << i << " variance " << now);
        CHECK(now <= prev + 1e-6f);
        prev = now;
    }
    CHECK(prev < variance_converged);

    // Nothing outside the brush moved.
    for (size_t i = 0; i < bumped.vertices.size(); ++i)
        if ((bumped.vertices[i] - p.center).norm() >= p.radius)
            CHECK(session.mesh().vertices[i] == bumped.vertices[i]);

    // And the topology is untouched, like every other brush.
    REQUIRE(session.mesh().indices.size() == bumped.indices.size());
    for (size_t i = 0; i < bumped.indices.size(); ++i)
        REQUIRE(session.mesh().indices[i] == bumped.indices[i]);
}

TEST_CASE("Flatten's inverse fills only what is below the plane", "[Sculpt][SculptFlatten]")
{
    // A grid with a dent AND a bump inside one brush. Symmetric Flatten levels
    // both; the "Fill" variant lifts the dent and leaves the bump standing.
    const Vec3f centre(10.f, 10.f, 0.f);
    indexed_triangle_set its = make_grid(41, 0.5f);
    for (Vec3f &v : its.vertices) {
        const float dl = (Vec3f(v.x(), v.y(), 0.f) - Vec3f(8.5f, 10.f, 0.f)).norm();
        const float dr = (Vec3f(v.x(), v.y(), 0.f) - Vec3f(11.5f, 10.f, 0.f)).norm();
        v.z() -= 1.0f * falloff_weight(dl, 1.5f); // dent, left
        v.z() += 1.0f * falloff_weight(dr, 1.5f); // bump, right
    }

    BrushParams p;
    p.type     = BrushType::Flatten;
    p.center   = Vec3f(centre.x(), centre.y(), 0.f);
    p.radius   = 4.f;
    p.strength = 1.f;
    p.falloff  = true;

    SculptSession symmetric(its);
    const std::vector<uint32_t> verts = symmetric.vertices_in_radius(p.center, p.radius);
    Vec3f origin = Vec3f::Zero(), normal = Vec3f::Zero();
    REQUIRE(fit_plane(symmetric.mesh(), symmetric.vertex_normals(), verts, p.center, p.radius, p.falloff, origin, normal));
    p.plane_normal = normal;

    symmetric.apply(p);

    SculptSession fill(its);
    p.fill_only = true;
    fill.apply(p);

    // Count what each variant moved, split by which side of the plane the vertex
    // started on.
    size_t sym_moved_below = 0, sym_moved_above = 0;
    size_t fill_moved_below = 0, fill_moved_above = 0;
    for (uint32_t v : verts) {
        const float d0 = (its.vertices[v] - origin).dot(normal);
        const bool  sym_moved  = (symmetric.mesh().vertices[v] - its.vertices[v]).norm() > 1e-5f;
        const bool  fill_moved = (fill.mesh().vertices[v] - its.vertices[v]).norm() > 1e-5f;
        if (d0 < -1e-4f) {
            sym_moved_below  += sym_moved ? 1 : 0;
            fill_moved_below += fill_moved ? 1 : 0;
        } else if (d0 > 1e-4f) {
            sym_moved_above  += sym_moved ? 1 : 0;
            fill_moved_above += fill_moved ? 1 : 0;
        }
    }

    INFO("symmetric below/above " << sym_moved_below << "/" << sym_moved_above
         << "  fill below/above " << fill_moved_below << "/" << fill_moved_above);
    // Both variants lift the dent ...
    CHECK(sym_moved_below > 5);
    CHECK(fill_moved_below == sym_moved_below);
    // ... only the symmetric one flattens the bump.
    CHECK(sym_moved_above > 5);
    CHECK(fill_moved_above == 0);
}

TEST_CASE("Flatten respects strength and the falloff toggle", "[Sculpt][SculptFlatten]")
{
    const Vec3f centre(10.f, 10.f, 0.f);
    const indexed_triangle_set bumped = make_bumped_grid(41, 0.5f, centre, 3.f, 1.5f);

    BrushParams p;
    p.type     = BrushType::Flatten;
    p.center   = Vec3f(centre.x(), centre.y(), 0.f);
    p.radius   = 3.f;
    p.falloff  = true;

    SculptSession full(bumped);
    p.strength = 1.f;
    // Pin the same plane for both runs, so the only difference is the strength.
    const std::vector<uint32_t> verts = full.vertices_in_radius(p.center, p.radius);
    Vec3f origin = Vec3f::Zero(), normal = Vec3f::Zero();
    REQUIRE(fit_plane(full.mesh(), full.vertex_normals(), verts, p.center, p.radius, p.falloff, origin, normal));
    p.plane_normal = normal;
    full.apply(p);

    SculptSession half(bumped);
    p.strength = 0.5f;
    half.apply(p);

    // Half the strength is exactly half the move, vertex by vertex.
    size_t checked = 0;
    for (uint32_t v : verts) {
        const Vec3f df = full.mesh().vertices[v] - bumped.vertices[v];
        const Vec3f dh = half.mesh().vertices[v] - bumped.vertices[v];
        if (df.norm() < 1e-5f)
            continue;
        CHECK((dh - 0.5f * df).norm() == Approx(0.f).margin(1e-5));
        ++checked;
    }
    CHECK(checked > 20);

    // With the falloff off, every vertex in the brush lands on the plane in one
    // tick at strength 1 - a hard-edged flatten. The plane to measure against is
    // the one THIS stroke fits: the brush refits the plane's origin from the
    // weights in force, and unweighted vertices give a different (higher)
    // centroid than falloff-weighted ones. Same normal, different offset.
    SculptSession hard(bumped);
    p.strength = 1.f;
    p.falloff  = false;
    Vec3f hard_origin = Vec3f::Zero(), hard_normal = Vec3f::Zero();
    REQUIRE(fit_plane(hard.mesh(), hard.vertex_normals(), verts, p.center, p.radius, /* falloff */ false,
                      hard_origin, hard_normal));
    // The pinned direction is what the brush uses, so measure with that.
    hard.apply(p);
    for (uint32_t v : verts)
        CHECK(std::abs((hard.mesh().vertices[v] - hard_origin).dot(normal)) < 1e-3f);
}

// ----------------------------------------------------------------------------
// Crease
// ----------------------------------------------------------------------------

TEST_CASE("Crease pulls a ring of vertices toward the centre line and lowers them", "[Sculpt][SculptCrease]")
{
    // A flat grid: the brush's normal is +z, so "toward the centre line" is
    // radially inward in xy and "lower" is -z, both easy to state exactly.
    const int   n = 41;
    const float h = 0.5f;
    const indexed_triangle_set grid = make_grid(n, h);

    SculptSession session(grid);

    BrushParams p;
    p.type     = BrushType::Crease;
    p.center   = Vec3f(10.f, 10.f, 0.f);
    p.radius   = 3.f;
    p.strength = 0.5f;
    p.falloff  = true;
    p.ridge    = false;
    p.crease_normal_ratio = 1.f;

    const std::vector<uint32_t> verts = session.vertices_in_radius(p.center, p.radius);
    REQUIRE(verts.size() > 20);

    const StrokeStep step = session.apply(p);
    REQUIRE_FALSE(step.empty());

    size_t pulled_in = 0, lowered = 0, checked = 0;
    for (uint32_t v : verts) {
        const Vec3f before = grid.vertices[v];
        const Vec3f after  = session.mesh().vertices[v];
        const float d      = (before - p.center).norm();
        const float w      = falloff_weight(d, p.radius) * p.strength;
        if (w < 1e-3f)
            continue;
        ++checked;

        // Pinch: the horizontal distance to the centre line shrank ...
        const float r_before = (Vec3f(before.x(), before.y(), 0.f) - Vec3f(p.center.x(), p.center.y(), 0.f)).norm();
        const float r_after  = (Vec3f(after.x(), after.y(), 0.f) - Vec3f(p.center.x(), p.center.y(), 0.f)).norm();
        if (r_before > 1e-4f) {
            CHECK(r_after < r_before);
            // ... by exactly the weight, since the pinch is a straight lerp
            // toward the axis.
            CHECK(r_after == Approx((1.f - w) * r_before).margin(1e-4));
            ++pulled_in;
        }

        // Push: and the vertex went down, by weight * ratio * 0.25 * radius.
        CHECK(after.z() < before.z());
        CHECK(after.z() == Approx(before.z() - w * 0.25f * p.radius).margin(1e-4));
        ++lowered;
    }
    INFO("checked " << checked << " pulled_in " << pulled_in << " lowered " << lowered);
    CHECK(checked > 20);
    CHECK(pulled_in > 20);
    CHECK(lowered == checked);

    // The vertex exactly on the axis has nothing to pinch, but it still drops.
    const uint32_t centre_v = uint32_t((n / 2) * n + (n / 2));
    CHECK(session.mesh().vertices[centre_v].z() == Approx(-0.5f * 0.25f * p.radius).margin(1e-4));

    // Nothing outside the brush moved, and the indices are untouched.
    for (size_t i = 0; i < grid.vertices.size(); ++i)
        if ((grid.vertices[i] - p.center).norm() >= p.radius)
            CHECK(session.mesh().vertices[i] == grid.vertices[i]);
    REQUIRE(session.mesh().indices.size() == grid.indices.size());
    for (size_t i = 0; i < grid.indices.size(); ++i)
        REQUIRE(session.mesh().indices[i] == grid.indices[i]);
}

TEST_CASE("An inverted Crease raises a ridge instead of cutting a valley", "[Sculpt][SculptCrease]")
{
    const indexed_triangle_set grid = make_grid(41, 0.5f);

    BrushParams p;
    p.type     = BrushType::Crease;
    p.center   = Vec3f(10.f, 10.f, 0.f);
    p.radius   = 3.f;
    p.strength = 0.5f;
    p.falloff  = true;

    SculptSession valley(grid);
    p.ridge = false;
    valley.apply(p);

    SculptSession ridge(grid);
    p.ridge = true;
    ridge.apply(p);

    const std::vector<uint32_t> verts = SculptSession(grid).vertices_in_radius(p.center, p.radius);
    size_t checked = 0;
    for (uint32_t v : verts) {
        const float z0 = grid.vertices[v].z();
        const float zv = valley.mesh().vertices[v].z();
        const float zr = ridge.mesh().vertices[v].z();
        if (std::abs(zv - z0) < 1e-5f)
            continue;
        // Opposite sides, same size: the ridge is the valley mirrored in z.
        CHECK(zv < z0);
        CHECK(zr > z0);
        CHECK((zr - z0) == Approx(-(zv - z0)).margin(1e-5));
        ++checked;
    }
    CHECK(checked > 20);

    // The pinch is the same either way - only the normal push flips.
    for (uint32_t v : verts) {
        const Vec3f a = valley.mesh().vertices[v];
        const Vec3f b = ridge.mesh().vertices[v];
        CHECK(a.x() == Approx(b.x()).margin(1e-5));
        CHECK(a.y() == Approx(b.y()).margin(1e-5));
    }
}

TEST_CASE("Crease on a sphere pinches toward the brush axis, not toward the origin", "[Sculpt][SculptCrease]")
{
    // A curved surface, where "the tangent plane at the hit" is not the z = 0
    // plane and the pinch direction has to be derived from the fitted normal.
    const indexed_triangle_set sphere = its_make_sphere(10.0, 2 * PI / 90);
    SculptSession session(sphere);

    BrushParams p;
    p.type     = BrushType::Crease;
    p.center   = Vec3f(0.f, 0.f, 10.f); // the north pole
    p.radius   = 3.f;
    p.strength = 0.6f;
    p.falloff  = true;

    const std::vector<uint32_t> verts = session.vertices_in_radius(p.center, p.radius);
    REQUIRE(verts.size() > 20);
    // At the pole the fitted normal is +z, so the axis is the z axis and the
    // pinch is radial in xy.
    Vec3f origin = Vec3f::Zero(), normal = Vec3f::Zero();
    REQUIRE(fit_plane(session.mesh(), session.vertex_normals(), verts, p.center, p.radius, p.falloff, origin, normal));
    CHECK(normal.z() > 0.9f);

    session.apply(p);

    size_t pulled = 0;
    for (uint32_t v : verts) {
        const Vec3f before = sphere.vertices[v];
        const Vec3f after  = session.mesh().vertices[v];
        const float r_before = std::hypot(before.x(), before.y());
        const float r_after  = std::hypot(after.x(), after.y());
        if (r_before < 1e-3f)
            continue;
        CHECK(r_after <= r_before + 1e-5f);
        if (r_after < r_before - 1e-5f)
            ++pulled;
        // And the surface went in, not out.
        CHECK(after.z() <= before.z() + 1e-5f);
    }
    CHECK(pulled > 20);

    // A valley cut into a sphere takes volume out of it.
    CHECK(its_volume(session.mesh()) < its_volume(sphere));
    // Vertex-only edits keep it closed.
    CHECK(its_num_open_edges(session.mesh()) == 0);
}

// ----------------------------------------------------------------------------
// Plane fitting
// ----------------------------------------------------------------------------

TEST_CASE("fit_plane returns the patch's centroid and average normal", "[Sculpt][SculptFlatten]")
{
    // A grid tilted so the answer is not axis-aligned: z = 0.5 x, whose upward
    // normal is (-0.5, 0, 1) normalised.
    indexed_triangle_set its = make_grid(41, 0.5f);
    for (Vec3f &v : its.vertices)
        v.z() = 0.5f * v.x();

    SculptSession session(its);
    const Vec3f centre(10.f, 10.f, 5.f);
    const std::vector<uint32_t> verts = session.vertices_in_radius(centre, 3.f);
    REQUIRE_FALSE(verts.empty());

    Vec3f origin = Vec3f::Zero(), normal = Vec3f::Zero();
    REQUIRE(fit_plane(session.mesh(), session.vertex_normals(), verts, centre, 3.f, /* falloff */ true, origin, normal));

    const Vec3f expected = Vec3f(-0.5f, 0.f, 1.f).normalized();
    CHECK(normal.x() == Approx(expected.x()).margin(1e-3));
    CHECK(normal.y() == Approx(expected.y()).margin(1e-3));
    CHECK(normal.z() == Approx(expected.z()).margin(1e-3));
    // The centroid lies on the plane the grid describes.
    CHECK(origin.z() == Approx(0.5f * origin.x()).margin(1e-3));
    // A perfectly flat patch has no variance about its own plane.
    CHECK(plane_distance_variance(session.mesh(), verts, origin, normal) == Approx(0.f).margin(1e-6));

    // Nothing to fit means no plane.
    std::vector<uint32_t> none;
    Vec3f o2 = Vec3f::Zero(), n2 = Vec3f::Zero();
    CHECK_FALSE(fit_plane(session.mesh(), session.vertex_normals(), none, centre, 3.f, true, o2, n2));
}

// ----------------------------------------------------------------------------
// Modal brush adjust (F / Shift+F)
// ----------------------------------------------------------------------------

TEST_CASE("The F modal scales the radius with horizontal mouse travel", "[Sculpt][SculptAdjust]")
{
    const float r_min = 0.4f, r_max = 20.f;
    AdjustState st = adjust_begin(AdjustTarget::Radius, 2.f, 500.);
    REQUIRE(st.active());
    CHECK(st.value == Approx(2.f));
    CHECK(st.start_value == Approx(2.f));

    // Right is bigger: a full-scale drag doubles it.
    AdjustState right = adjust_move(st, 500. + AdjustFullScalePx, r_min, r_max);
    CHECK(right.value == Approx(4.f).margin(1e-4));
    // Left is smaller: the same travel the other way halves it.
    AdjustState left = adjust_move(st, 500. - AdjustFullScalePx, r_min, r_max);
    CHECK(left.value == Approx(1.f).margin(1e-4));
    // Half a full scale is a factor of sqrt(2), i.e. the mapping is smooth and
    // multiplicative rather than linear.
    AdjustState half = adjust_move(st, 500. + 0.5 * AdjustFullScalePx, r_min, r_max);
    CHECK(half.value == Approx(2.f * std::sqrt(2.f)).margin(1e-3));

    // The value is recomputed from the TOTAL travel, so coming back to the start
    // is exactly the starting value - no accumulated drift.
    AdjustState wandered = adjust_move(adjust_move(adjust_move(st, 900., r_min, r_max), 100., r_min, r_max), 500., r_min, r_max);
    CHECK(wandered.value == Approx(2.f).margin(1e-5));

    // The range is honoured at both ends.
    CHECK(adjust_move(st, 500. + 40. * AdjustFullScalePx, r_min, r_max).value == Approx(r_max));
    CHECK(adjust_move(st, 500. - 40. * AdjustFullScalePx, r_min, r_max).value == Approx(r_min));

    // Confirm keeps the live value, cancel puts back what it was.
    CHECK(adjust_confirm(right) == Approx(4.f).margin(1e-4));
    CHECK(adjust_cancel(right) == Approx(2.f));
    // Cancelling after wandering all over also lands back on the start.
    CHECK(adjust_cancel(adjust_move(st, 12345., r_min, r_max)) == Approx(2.f));
}

TEST_CASE("The Shift+F modal moves the strength additively over its range", "[Sculpt][SculptAdjust]")
{
    const float s_min = 0.05f, s_max = 1.f;
    AdjustState st = adjust_begin(AdjustTarget::Strength, 0.5f, 200.);
    REQUIRE(st.active());

    // A full-scale drag right covers the whole 0.05..1 span, which from the
    // middle means it clamps at the top.
    CHECK(adjust_move(st, 200. + AdjustFullScalePx, s_min, s_max).value == Approx(s_max));
    // Half a full scale adds half the span.
    CHECK(adjust_move(st, 200. + 0.5 * AdjustFullScalePx, s_min, s_max).value
          == Approx(0.5f + 0.5f * (s_max - s_min)).margin(1e-4));
    // Left decreases, and clamps at the floor.
    CHECK(adjust_move(st, 200. - AdjustFullScalePx, s_min, s_max).value == Approx(s_min));
    // Reversible, like the radius.
    CHECK(adjust_move(adjust_move(st, 800., s_min, s_max), 200., s_min, s_max).value == Approx(0.5f).margin(1e-5));

    // A fresh state is inactive and a move on it is a no-op.
    AdjustState idle;
    CHECK_FALSE(idle.active());
    CHECK_FALSE(adjust_move(idle, 999., s_min, s_max).active());
}

TEST_CASE("Ctrl inverts the brushes that have an opposite, and only those", "[Sculpt][SculptAdjust]")
{
    // Inflate <-> Deflate, Flatten <-> Fill, Crease <-> Ridge all have a
    // meaningful inverse; dragging a patch and smoothing it do not.
    CHECK(brush_inverts_with_ctrl(BrushType::Inflate));
    CHECK(brush_inverts_with_ctrl(BrushType::Flatten));
    CHECK(brush_inverts_with_ctrl(BrushType::Crease));
    CHECK_FALSE(brush_inverts_with_ctrl(BrushType::Grab));
    CHECK_FALSE(brush_inverts_with_ctrl(BrushType::Smooth));
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

    p.type = BrushType::Flatten;
    p.plane_normal = Vec3f::Zero();
    session.apply(p);

    p.type  = BrushType::Crease;
    p.ridge = false;
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
    size_t touched = 0, dirty = 0;
    for (int i = 0; i < ticks; ++i) {
        const StrokeStep s = session.apply(p);
        touched = s.moved_vertices.size();
        dirty   = s.dirty_triangles.size();
    }
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
              << " dirty_triangles=" << dirty
              << " subdata_bytes=" << (dirty * 72)
              << " tree_rebuild_ms=" << retree_ms
              << std::endl;

    // A tick costs what is under the brush, not what is in the mesh. On a mesh
    // this fine the brush still covers tens of thousands of vertices, so the
    // bound here is generous - the number printed above is the interesting part.
    CHECK(tick_ms < 60.0);
}

// ----------------------------------------------------------------------------
// v3, phase 3a: the new brushes
// ----------------------------------------------------------------------------

TEST_CASE("Pinch pulls in toward the brush axis and leaves the plane distance alone", "[Sculpt][SculptPinch]")
{
    // A flat grid, so the fitted axis is +z, "toward the axis" is radially
    // inward in xy, and "no normal component" is "z did not change" - the
    // simplest possible statement of Pinch's defining property.
    const indexed_triangle_set grid = make_grid(41, 0.5f);
    SculptSession session(grid);

    BrushParams p;
    p.type     = BrushType::Pinch;
    p.center   = Vec3f(10.f, 10.f, 0.f);
    p.radius   = 3.f;
    p.strength = 0.5f;
    p.falloff  = true;

    const std::vector<uint32_t> verts = session.vertices_in_radius(p.center, p.radius);
    REQUIRE(verts.size() > 20);

    const StrokeStep step = session.apply(p);
    REQUIRE_FALSE(step.empty());

    size_t pulled = 0, checked = 0;
    for (uint32_t v : verts) {
        const Vec3f before = grid.vertices[v];
        const Vec3f after  = session.mesh().vertices[v];
        const float d      = (before - p.center).norm();
        const float w      = falloff_weight(d, p.radius) * p.strength;
        if (w < 1e-3f)
            continue;
        ++checked;

        // The distance to the fitted plane is untouched: that IS "the move has
        // no normal component", stated without reimplementing the projection.
        CHECK(after.z() == Approx(before.z()).margin(1e-6));

        const float r_before = (Vec3f(before.x(), before.y(), 0.f) - Vec3f(p.center.x(), p.center.y(), 0.f)).norm();
        const float r_after  = (Vec3f(after.x(), after.y(), 0.f) - Vec3f(p.center.x(), p.center.y(), 0.f)).norm();
        if (r_before > 1e-4f) {
            CHECK(r_after < r_before);
            // A straight lerp toward the axis, exactly by the weight.
            CHECK(r_after == Approx((1.f - w) * r_before).margin(1e-4));
            ++pulled;
        }
    }
    CHECK(checked > 20);
    CHECK(pulled > 20);

    // Nothing outside the brush moved.
    for (size_t i = 0; i < grid.vertices.size(); ++i)
        if ((grid.vertices[i] - p.center).norm() >= p.radius)
            CHECK(session.mesh().vertices[i] == grid.vertices[i]);
}

TEST_CASE("Magnify is Pinch with the sign flipped, vertex for vertex", "[Sculpt][SculptPinch]")
{
    const indexed_triangle_set grid = make_grid(41, 0.5f);

    BrushParams p;
    p.type     = BrushType::Pinch;
    p.center   = Vec3f(10.f, 10.f, 0.f);
    p.radius   = 3.f;
    p.strength = 0.4f;
    p.falloff  = true;

    SculptSession pinch(grid);
    p.magnify = false;
    pinch.apply(p);

    SculptSession magnify(grid);
    p.magnify = true;
    magnify.apply(p);

    const std::vector<uint32_t> verts = SculptSession(grid).vertices_in_radius(p.center, p.radius);
    size_t checked = 0;
    for (uint32_t v : verts) {
        const Vec3f v0 = grid.vertices[v];
        const Vec3f a  = pinch.mesh().vertices[v];
        const Vec3f b  = magnify.mesh().vertices[v];
        if ((a - v0).norm() < 1e-6f)
            continue;
        ++checked;
        // Equal and opposite displacements: the one bool is the whole difference.
        CHECK((b - v0).x() == Approx(-(a - v0).x()).margin(1e-6));
        CHECK((b - v0).y() == Approx(-(a - v0).y()).margin(1e-6));
        CHECK((b - v0).z() == Approx(-(a - v0).z()).margin(1e-6));
        // And Magnify really does push out.
        CHECK((Vec3f(b.x(), b.y(), 0.f) - Vec3f(p.center.x(), p.center.y(), 0.f)).norm() >
              (Vec3f(v0.x(), v0.y(), 0.f) - Vec3f(p.center.x(), p.center.y(), 0.f)).norm());
    }
    CHECK(checked > 20);
}

TEST_CASE("Nudge moves every vertex orthogonally to its own normal", "[Sculpt][SculptNudge]")
{
    // A sphere, so the vertex normals point every which way and "orthogonal to
    // its OWN normal" is a real constraint rather than a restatement of the
    // drag direction.
    const indexed_triangle_set sphere = its_make_sphere(10.0, 2 * PI / 60);
    SculptSession session(sphere);

    BrushParams p;
    p.type         = BrushType::Nudge;
    p.center       = Vec3f(0.f, 0.f, 10.f);
    p.radius       = 4.f;
    p.strength     = 1.f;
    p.falloff      = true;
    // Deliberately not tangential to the surface anywhere: the brush has to do
    // the projection itself.
    p.displacement = Vec3f(0.3f, 0.1f, 0.4f);

    const std::vector<uint32_t> verts = session.vertices_in_radius(p.center, p.radius);
    REQUIRE(verts.size() > 20);
    const std::vector<Vec3f> normals = session.vertex_normals();

    const StrokeStep step = session.apply(p);
    REQUIRE_FALSE(step.empty());

    size_t checked = 0;
    for (uint32_t v : verts) {
        const Vec3f move = session.mesh().vertices[v] - sphere.vertices[v];
        if (move.norm() < 1e-5f)
            continue;
        ++checked;
        // The move lies in the vertex's tangent plane: that is Nudge's whole
        // difference from Grab, which would carry the full 3D displacement.
        CHECK(move.dot(normals[v]) == Approx(0.f).margin(1e-5));
        // And it points the way the drag did, in that plane.
        const Vec3f tan = p.displacement - p.displacement.dot(normals[v]) * normals[v];
        CHECK(move.normalized().dot(tan.normalized()) == Approx(1.f).margin(1e-4));
    }
    CHECK(checked > 20);

    // Grab, by contrast, carries the normal component too.
    SculptSession grabbed(sphere);
    p.type = BrushType::Grab;
    grabbed.apply(p);
    bool differs = false;
    for (uint32_t v : verts)
        if ((grabbed.mesh().vertices[v] - session.mesh().vertices[v]).norm() > 1e-4f)
            differs = true;
    CHECK(differs);
}

TEST_CASE("Snake Hook accumulates like a Grab whose anchor rides the cursor", "[Sculpt][SculptSnakeHook]")
{
    const indexed_triangle_set sphere = its_make_sphere(10.0, 2 * PI / 60);

    BrushParams p;
    p.type         = BrushType::SnakeHook;
    p.radius       = 3.f;
    p.strength     = 1.f;
    p.falloff      = true;
    p.displacement = Vec3f(0.f, 0.f, 0.6f);

    // Ten ticks of a drag straight up the +z axis, with the centre following the
    // accumulated displacement - which is exactly what the gizmo feeds it.
    SculptSession hook(sphere);
    Vec3f centre = Vec3f(0.f, 0.f, 10.f);
    for (int i = 0; i < 10; ++i) {
        centre += p.displacement;
        p.center = centre;
        hook.apply(p);
    }

    // A fixed-anchor Grab over the same total displacement, for contrast.
    SculptSession grab(sphere);
    BrushParams g = p;
    g.type   = BrushType::Grab;
    g.center = Vec3f(0.f, 0.f, 10.f);
    for (int i = 0; i < 10; ++i)
        grab.apply(g);

    // Snake Hook pulls a horn: the apex ends up well above the original surface,
    // and the brush keeps finding fresh geometry as it travels.
    float hook_max = -1e9f, grab_max = -1e9f;
    for (const Vec3f &v : hook.mesh().vertices)
        hook_max = std::max(hook_max, v.z());
    for (const Vec3f &v : grab.mesh().vertices)
        grab_max = std::max(grab_max, v.z());
    INFO("hook apex " << hook_max << " grab apex " << grab_max);
    CHECK(hook_max > 10.f);

    // Vertex-only: the topology is untouched and the mesh stays closed.
    CHECK(hook.mesh().indices.size() == sphere.indices.size());
    CHECK(its_num_open_edges(hook.mesh()) == 0);
}

TEST_CASE("Clay Strips builds up to the target offset and never retracts", "[Sculpt][SculptClay]")
{
    // A flat grid: the fitted plane is z = 0 with normal +z, so the target is
    // z = clay_offset and every assertion is about z directly.
    const indexed_triangle_set grid = make_grid(41, 0.5f);
    SculptSession session(grid);

    BrushParams p;
    p.type        = BrushType::ClayStrips;
    p.center      = Vec3f(10.f, 10.f, 0.f);
    p.radius      = 3.f;
    p.strength    = 1.f;
    // No falloff, so every vertex in the brush gets weight 1 and should land
    // exactly on the target - the hardest, most exact case.
    p.falloff     = false;
    p.clay_offset = 0.4f;
    // The plane is pinned for the stroke, both direction AND offset, exactly as
    // the gizmo pins it at start_stroke(). Refitting the offset every tick would
    // measure the target from the material the previous tick just laid down and
    // a held stroke would climb by clay_offset per tick - which is precisely the
    // divergence this pin exists to prevent, and what the second-tick assertion
    // below is really testing.
    p.plane_normal = Vec3f::UnitZ();
    p.plane_origin = Vec3f(10.f, 10.f, 0.f);

    const std::vector<uint32_t> verts = session.vertices_in_radius(p.center, p.radius);
    REQUIRE(verts.size() > 20);

    session.apply(p);
    for (uint32_t v : verts)
        CHECK(session.mesh().vertices[v].z() == Approx(p.clay_offset).margin(1e-5));

    // A second tick changes nothing: everything is already at the target, and
    // the one-sided clamp refuses to retract it. That non-accumulation is what
    // distinguishes Clay Strips from Inflate.
    const std::vector<Vec3f> after_one = session.mesh().vertices;
    session.apply(p);
    for (uint32_t v : verts)
        CHECK(session.mesh().vertices[v].z() == Approx(after_one[v].z()).margin(1e-6));

    // A vertex already above the target is left strictly alone.
    indexed_triangle_set raised = make_grid(41, 0.5f);
    const uint32_t high = uint32_t(20 * 41 + 20);
    raised.vertices[high].z() = 2.f;
    SculptSession s2(raised);
    BrushParams q = p;
    q.clay_offset = 0.4f;
    s2.apply(q);
    CHECK(s2.mesh().vertices[high].z() == raised.vertices[high].z());

    // Ctrl carves: a negative offset cuts a trench and, symmetrically, leaves a
    // vertex already below the target alone.
    SculptSession carve(grid);
    q = p;
    q.clay_offset = -0.4f;
    carve.apply(q);
    for (uint32_t v : verts)
        CHECK(carve.mesh().vertices[v].z() == Approx(-0.4f).margin(1e-5));
}

TEST_CASE("Ctrl inverts the v3 brushes that have an opposite, and only those", "[Sculpt][SculptAdjust]")
{
    CHECK(brush_inverts_with_ctrl(BrushType::Pinch));       // Magnify
    CHECK(brush_inverts_with_ctrl(BrushType::ClayStrips));  // carve
    CHECK(brush_inverts_with_ctrl(BrushType::Mask));        // erase
    // A drag has no opposite, exactly as for Grab.
    CHECK_FALSE(brush_inverts_with_ctrl(BrushType::Nudge));
    CHECK_FALSE(brush_inverts_with_ctrl(BrushType::SnakeHook));
}

// ----------------------------------------------------------------------------
// v3, phase 3b: the mask
// ----------------------------------------------------------------------------

// A box with two separate feet: two disjoint bottom islands at the same z_min,
// which is the case the research spec flags as easy to get wrong by baking in a
// single-loop assumption.
static indexed_triangle_set make_two_footed_part()
{
    indexed_triangle_set a = its_make_cube(10., 10., 10.);
    indexed_triangle_set b = its_make_cube(10., 10., 10.);
    for (Vec3f &v : b.vertices)
        v.x() += 30.f;
    const uint32_t off = uint32_t(a.vertices.size());
    a.vertices.insert(a.vertices.end(), b.vertices.begin(), b.vertices.end());
    for (const Vec3i32 &f : b.indices)
        a.indices.emplace_back(Vec3i32(f(0) + int(off), f(1) + int(off), f(2) + int(off)));
    return a;
}

TEST_CASE("Bed-contact detection finds the flat bottom, and every island of it", "[Sculpt][SculptMask]")
{
    const indexed_triangle_set cube = its_subdivide_midpoint(its_make_cube(20., 20., 20.));
    SculptSession session(cube);

    REQUIRE(session.detect_bed_contact() > 0);
    CHECK(session.bed_z_min() == Approx(0.f).margin(1e-6));
    // Every detected vertex really is on the bottom face.
    for (uint32_t v : session.bed_contact_vertices())
        CHECK(session.mesh().vertices[v].z() == Approx(0.f).margin(1e-4));
    // And every vertex on the bottom face was detected.
    size_t on_bottom = 0;
    for (const Vec3f &v : cube.vertices)
        if (std::abs(v.z()) < 1e-4f)
            ++on_bottom;
    CHECK(session.bed_contact_vertices().size() == on_bottom);

    // Two feet: the predicate is per-facet, so both islands join the set with no
    // flood fill and no single-island assumption anywhere.
    const indexed_triangle_set two = make_two_footed_part();
    SculptSession twofoot(two);
    REQUIRE(twofoot.detect_bed_contact() > 0);
    bool left = false, right = false;
    for (uint32_t v : twofoot.bed_contact_vertices()) {
        if (twofoot.mesh().vertices[v].x() < 15.f)
            left = true;
        else
            right = true;
    }
    CHECK(left);
    CHECK(right);
    // And the footprint comes back as two loops, not one.
    const auto loops = bed_footprint_loops(twofoot.mesh(), twofoot.bed_contact_vertices());
    CHECK(loops.size() == 2);

    // A sphere touches the bed at a point, not a face: nothing to protect, and
    // the detection says so rather than doing something surprising.
    SculptSession ball(its_make_sphere(10.0, 2 * PI / 30));
    CHECK(ball.detect_bed_contact() == 0);
    CHECK(ball.bed_contact_vertices().empty());
}

TEST_CASE("A masked vertex is untouched by every brush, bit for bit", "[Sculpt][SculptMask]")
{
    // The hardest case the plan asks for: strength 1, falloff off, and the brush
    // centred on and covering the whole masked bottom face, so every masked
    // vertex would otherwise get full weight.
    const indexed_triangle_set cube = its_subdivide_midpoint(its_subdivide_midpoint(its_make_cube(20., 20., 20.)));

    const std::vector<BrushType> all = {
        BrushType::Grab, BrushType::Inflate, BrushType::Smooth, BrushType::Flatten,
        BrushType::Crease, BrushType::Pinch, BrushType::Nudge, BrushType::SnakeHook,
        BrushType::ClayStrips};

    for (BrushType type : all) {
        SculptSession session(cube);
        REQUIRE(session.detect_bed_contact() > 0);
        session.set_bed_contact_enabled(true);
        const std::vector<uint32_t> masked = session.bed_contact_vertices();
        REQUIRE(!masked.empty());

        BrushParams p;
        p.type         = type;
        p.center       = Vec3f(10.f, 10.f, 0.f);
        p.radius       = 40.f;   // the whole part
        p.strength     = 1.f;
        p.falloff      = false;  // no weight decay anywhere
        p.displacement = Vec3f(1.f, 1.f, 1.f);
        p.amount       = 1.f;
        p.clay_offset  = 2.f;
        p.iterations   = 3;

        const StrokeStep step = session.apply(p);
        INFO("brush " << int(type));

        // Not merely "moved by zero": a masked vertex is dropped from the
        // touched set outright, so it never appears in moved_vertices ...
        for (uint32_t m : masked)
            CHECK(std::find(step.moved_vertices.begin(), step.moved_vertices.end(), m) == step.moved_vertices.end());
        // ... and its position is bit-identical, with no tolerance at all.
        for (uint32_t m : masked)
            CHECK(session.mesh().vertices[m] == cube.vertices[m]);

        // Something unmasked did move, otherwise the assertion is vacuous.
        bool moved = false;
        for (size_t i = 0; i < cube.vertices.size() && !moved; ++i)
            moved = session.mesh().vertices[i] != cube.vertices[i];
        CHECK(moved);
    }
}

TEST_CASE("The bed face survives Subdivide plus a Smooth stroke unchanged", "[Sculpt][SculptMask]")
{
    const indexed_triangle_set cube = its_subdivide_midpoint(its_make_cube(20., 20., 20.));

    SculptSession before(cube);
    REQUIRE(before.detect_bed_contact() > 0);
    const float z_min = before.bed_z_min();
    const std::vector<uint32_t> bed_before = before.bed_contact_vertices();

    // Footprint of the original, for the area/perimeter comparison below.
    const auto loops_before = bed_footprint_loops(cube, bed_before);
    REQUIRE(loops_before.size() == 1);
    const double area_before = loop_area_xy(cube, loops_before.front());
    const double peri_before = loop_perimeter_xy(cube, loops_before.front());
    CHECK(area_before == Approx(400.).epsilon(1e-9));

    // Subdivide with the bed pinned, then re-detect (subdivision renumbers, so
    // the mask is recomputed from the new mesh - exactly what the gizmo does).
    const indexed_triangle_set sub = its_subdivide_midpoint(cube, bed_before, z_min);
    SculptSession session(sub);
    REQUIRE(session.detect_bed_contact() > 0);
    session.set_bed_contact_enabled(true);
    const std::vector<uint32_t> bed_after_sub = session.bed_contact_vertices();

    // Every bed vertex, old or newly introduced, is at exactly z_min - bitwise,
    // because the pin snaps rather than trusting the float midpoint.
    for (uint32_t v : bed_after_sub)
        CHECK(sub.vertices[v].z() == z_min);

    // Now a Smooth stroke that covers the whole part, bottom face included.
    BrushParams p;
    p.type       = BrushType::Smooth;
    p.center     = Vec3f(10.f, 10.f, 0.f);
    p.radius     = 40.f;
    p.strength   = 1.f;
    p.falloff    = false;
    p.iterations = 5;
    session.apply(p);

    // Bit-identical in z, not merely close: the masked vertices were never in
    // the touched set at all.
    for (uint32_t v : bed_after_sub)
        CHECK(session.mesh().vertices[v].z() == z_min);

    // And the footprint outline itself - area and perimeter - is unchanged. The
    // subdivided outline has twice as many vertices, but they are the midpoints
    // of the old edges, so the polygon is geometrically the same.
    const auto loops_after = bed_footprint_loops(session.mesh(), bed_after_sub);
    REQUIRE(loops_after.size() == 1);
    const double area_after = loop_area_xy(session.mesh(), loops_after.front());
    const double peri_after = loop_perimeter_xy(session.mesh(), loops_after.front());
    CHECK(std::abs(area_after - area_before) < 1e-6);
    CHECK(std::abs(peri_after - peri_before) < 1e-6);
}

TEST_CASE("Sharp-edge protection pins the cube's edges and the threshold moves it", "[Sculpt][SculptMask]")
{
    const indexed_triangle_set cube = its_subdivide_midpoint(its_make_cube(20., 20., 20.));
    SculptSession session(cube);

    // A cube's edges are 90 degrees, so a 60-degree threshold catches every
    // vertex on an edge or corner and nothing in the middle of a face.
    const size_t sharp = session.detect_sharp_edges(60.f);
    REQUIRE(sharp > 0);
    CHECK(sharp < cube.vertices.size());

    // Raise the threshold past 90 and the cube has no sharp edges left at all.
    SculptSession loose(cube);
    CHECK(loose.detect_sharp_edges(100.f) == 0);

    // With it on, a Smooth stroke over the whole cube leaves the edges alone -
    // which is the point: Smooth would otherwise round the cube's corners off.
    SculptSession guarded(cube);
    REQUIRE(guarded.detect_sharp_edges(60.f) > 0);
    guarded.set_sharp_edge_enabled(true);
    BrushParams p;
    p.type       = BrushType::Smooth;
    p.center     = Vec3f(10.f, 10.f, 10.f);
    p.radius     = 40.f;
    p.strength   = 1.f;
    p.falloff    = false;
    p.iterations = 3;
    const StrokeStep step = guarded.apply(p);
    // A corner vertex - the extreme case - is bit-identical.
    for (size_t i = 0; i < cube.vertices.size(); ++i) {
        const Vec3f &v = cube.vertices[i];
        const bool corner = (std::abs(v.x()) < 1e-4f || std::abs(v.x() - 20.f) < 1e-4f) &&
                            (std::abs(v.y()) < 1e-4f || std::abs(v.y() - 20.f) < 1e-4f) &&
                            (std::abs(v.z()) < 1e-4f || std::abs(v.z() - 20.f) < 1e-4f);
        if (corner)
            CHECK(guarded.mesh().vertices[i] == v);
    }
    CHECK_FALSE(step.empty());
}

TEST_CASE("The Mask brush paints protection on and Ctrl paints it off", "[Sculpt][SculptMask]")
{
    const indexed_triangle_set grid = make_grid(41, 0.5f);
    SculptSession session(grid);
    CHECK_FALSE(session.has_painted_mask());

    BrushParams paint;
    paint.type        = BrushType::Mask;
    paint.center      = Vec3f(10.f, 10.f, 0.f);
    paint.radius      = 3.f;
    paint.strength    = 1.f;
    paint.falloff     = false;
    paint.mask_amount = 1.f;

    const std::vector<uint32_t> under = session.vertices_in_radius(paint.center, paint.radius);
    REQUIRE(under.size() > 20);

    session.apply(paint);
    CHECK(session.has_painted_mask());
    // The Mask brush moves nothing at all - that is what makes it a mask brush
    // and not a sculpt brush that happens to write a side table.
    for (size_t i = 0; i < grid.vertices.size(); ++i)
        CHECK(session.mesh().vertices[i] == grid.vertices[i]);
    for (uint32_t v : under)
        CHECK(session.effective_mask(v) == Approx(0.f).margin(1e-6));

    // And a sculpt brush now refuses to touch the painted patch.
    BrushParams p;
    p.type     = BrushType::Inflate;
    p.center   = paint.center;
    p.radius   = 4.f;
    p.strength = 1.f;
    p.falloff  = false;
    p.amount   = 1.f;
    session.apply(p);
    for (uint32_t v : under)
        CHECK(session.mesh().vertices[v] == grid.vertices[v]);

    // Ctrl erases it again ...
    BrushParams erase = paint;
    erase.mask_amount = -1.f;
    session.apply(erase);
    for (uint32_t v : under)
        CHECK(session.effective_mask(v) == Approx(1.f).margin(1e-6));
    // ... and now the same sculpt brush does move them.
    session.apply(p);
    size_t moved = 0;
    for (uint32_t v : under)
        if (session.mesh().vertices[v] != grid.vertices[v])
            ++moved;
    CHECK(moved > 20);

    // Clear puts it back to nothing.
    session.clear_painted_mask();
    CHECK_FALSE(session.has_painted_mask());
    CHECK_FALSE(session.any_mask());
}

TEST_CASE("The mask damps a partially painted vertex rather than pinning it", "[Sculpt][SculptMask]")
{
    // The mask is a float, not a bool, so half a stroke's worth of paint should
    // halve the brush rather than stop it - that is what lets the boundary of a
    // painted region be soft.
    const indexed_triangle_set grid = make_grid(41, 0.5f);
    SculptSession session(grid);

    BrushParams paint;
    paint.type        = BrushType::Mask;
    paint.center      = Vec3f(10.f, 10.f, 0.f);
    paint.radius      = 3.f;
    paint.strength    = 0.5f;   // half the mask
    paint.falloff     = false;
    paint.mask_amount = 1.f;
    session.apply(paint);

    const std::vector<uint32_t> under = session.vertices_in_radius(paint.center, 2.f);
    REQUIRE(under.size() > 10);
    for (uint32_t v : under)
        CHECK(session.effective_mask(v) == Approx(0.5f).margin(1e-5));

    BrushParams p;
    p.type         = BrushType::Grab;
    p.center       = paint.center;
    p.radius       = 3.f;
    p.strength     = 1.f;
    p.falloff      = false;
    p.displacement = Vec3f(0.f, 0.f, 1.f);
    session.apply(p);
    for (uint32_t v : under)
        // Half the mask, half the move.
        CHECK(session.mesh().vertices[v].z() == Approx(0.5f).margin(1e-5));
}

TEST_CASE("Subdivide without a bed set behaves exactly as it did before v3", "[Sculpt][SculptMask]")
{
    // The pin is opt-in: the one-argument overload must be byte-for-byte the old
    // behaviour, so nothing that already called it changes.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const indexed_triangle_set a = its_subdivide_midpoint(cube);
    const indexed_triangle_set b = its_subdivide_midpoint(cube, {}, 0.f);
    REQUIRE(a.vertices.size() == b.vertices.size());
    for (size_t i = 0; i < a.vertices.size(); ++i)
        CHECK(a.vertices[i] == b.vertices[i]);
    REQUIRE(a.indices.size() == b.indices.size());
    for (size_t i = 0; i < a.indices.size(); ++i)
        CHECK(a.indices[i] == b.indices[i]);
}

// ----------------------------------------------------------------------------
// cursor tracking
// ----------------------------------------------------------------------------
//
// The v1 bug the owner hit: "the sphere does not follow the mouse once the
// click+drag begins, it just sits at the initial click point." Nobody drives the
// gizmo with a mouse in this session, so what is tested here is the pure helper
// the gizmo now delegates the decision to (Sculpt::next_cursor_state), fed the
// frame-by-frame inputs a real drag would produce. The gizmo side - calling it
// from wxMouseEvent::Dragging() as well as Moving() - is a one-line wiring the
// test cannot reach without a GL context.

// Replay a whole gesture through the tracker and return the cursor position at
// each frame, which is exactly the sequence the sphere would be drawn at.
static std::vector<CursorState> replay(const std::vector<CursorInput> &frames)
{
    std::vector<CursorState> out;
    CursorState state;
    for (const CursorInput &in : frames) {
        state = next_cursor_state(state, in);
        out.push_back(state);
    }
    return out;
}

TEST_CASE("The cursor follows the mouse through a drag, not just on hover", "[Sculpt][SculptCursor]")
{
    // A straight drag across the surface: the raycast hit walks with the mouse.
    // This is the Inflate/Smooth case - the fresh hit is the cursor.
    std::vector<CursorInput> frames;
    for (int i = 0; i < 6; ++i) {
        CursorInput in;
        // Frame 0 is the hover before the press; the rest are drag ticks.
        in.mode      = (i == 0) ? CursorMode::Hover : CursorMode::StrokeHit;
        in.hit_valid = true;
        in.hit       = Vec3f(float(i), 0.f, 0.f);
        frames.push_back(in);
    }

    const std::vector<CursorState> seen = replay(frames);
    REQUIRE(seen.size() == frames.size());

    // Every frame is visible and sits exactly on that frame's hit: the cursor
    // moved six times, rather than sticking at the press point.
    for (size_t i = 0; i < seen.size(); ++i) {
        INFO("frame " << i);
        CHECK(seen[i].visible);
        CHECK(seen[i].position.x() == Approx(float(i)));
    }

    // The regression itself, stated directly: the cursor is not still at the
    // point the drag started from.
    CHECK(seen.back().position.x() != Approx(seen.front().position.x()));
    CHECK(seen.back().position.x() == Approx(5.f));
}

TEST_CASE("A Grab cursor rides the dragged surface, not the stale raycast", "[Sculpt][SculptCursor]")
{
    // Grab drags the very surface the cursor sits on, and the AABB tree is left
    // stale for the duration of a stroke - so the fresh hit lags behind the
    // patch being pulled. The cursor must follow the anchor instead.
    const Vec3f anchor0(10.f, 0.f, 0.f);

    std::vector<CursorInput> frames;
    for (int i = 0; i < 5; ++i) {
        CursorInput in;
        in.mode        = CursorMode::StrokeGrab;
        in.hit_valid   = true;
        // The stale surface barely moves: this is what a raycast would return.
        in.hit         = anchor0;
        // ...while the grabbed patch has been pulled i units along +y.
        in.grab_anchor = anchor0 + Vec3f(0.f, float(i), 0.f);
        frames.push_back(in);
    }

    const std::vector<CursorState> seen = replay(frames);
    for (size_t i = 0; i < seen.size(); ++i) {
        INFO("frame " << i);
        CHECK(seen[i].visible);
        // The cursor tracks the moved anchor...
        CHECK(seen[i].position.y() == Approx(float(i)));
        // ...and specifically NOT the stale hit, which never left y = 0.
        if (i > 0)
            CHECK(seen[i].position.y() != Approx(frames[i].hit.y()));
    }
}

TEST_CASE("A stroke keeps its cursor when the ray runs off the part", "[Sculpt][SculptCursor]")
{
    // Dragging past the silhouette must not make the sphere blink out: the user
    // is still sculpting. The cursor holds its last position instead.
    std::vector<CursorInput> frames;

    CursorInput on;
    on.mode      = CursorMode::StrokeHit;
    on.hit_valid = true;
    on.hit       = Vec3f(3.f, 4.f, 5.f);
    frames.push_back(on);

    CursorInput off;               // the ray misses this frame
    off.mode      = CursorMode::StrokeHit;
    off.hit_valid = false;
    frames.push_back(off);
    frames.push_back(off);

    const std::vector<CursorState> seen = replay(frames);
    CHECK(seen[0].visible);
    for (size_t i = 1; i < seen.size(); ++i) {
        INFO("frame " << i);
        CHECK(seen[i].visible);
        CHECK(seen[i].position.x() == Approx(3.f));
        CHECK(seen[i].position.y() == Approx(4.f));
        CHECK(seen[i].position.z() == Approx(5.f));
    }
}

TEST_CASE("Hovering off the part hides the cursor", "[Sculpt][SculptCursor]")
{
    // The hover case is the opposite of the stroke case: no button is held, so
    // a miss means there is nothing to point at and the sphere is hidden.
    CursorInput on;
    on.mode      = CursorMode::Hover;
    on.hit_valid = true;
    on.hit       = Vec3f(1.f, 2.f, 3.f);

    CursorInput off;
    off.mode      = CursorMode::Hover;
    off.hit_valid = false;

    const std::vector<CursorState> seen = replay({on, off, on});
    CHECK(seen[0].visible);
    CHECK_FALSE(seen[1].visible);
    CHECK(seen[2].visible);
    CHECK(seen[2].position.y() == Approx(2.f));
}

TEST_CASE("Sculpt stroke latency on large meshes", "[Sculpt][SculptBench]")
{
    bench_stroke(200000, "200k");
    bench_stroke(1000000, "1M");
}
