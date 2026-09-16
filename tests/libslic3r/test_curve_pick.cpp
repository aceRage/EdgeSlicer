#include <catch2/catch.hpp>

#include <libslic3r/Measure.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <cmath>
#include <numeric>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Measure;

// Ultra: the Assembly gizmo's "Curve and curve" pick. The patch under the cursor is region-grown with a
// per-step crease limit and (optionally) a total spread cap vs the seed facet; the mate then fits the
// patch analytically. These tests pin:
//   * the default 8/20 grow (a partial patch on a sphere) against the uncapped "smooth shell" grow,
//   * that a smooth-shell grow on a half sphere stops at the cut face's rim,
//   * that the sphere fit recovers centre and radius,
//   * that the cylinder fit is unchanged and still wins on a cylinder.

namespace {

// A UV hemisphere (z >= 0) of radius r, closed by a flat disk at z = 0. The rim between shell and disk is
// a genuine 90 deg crease, which is what a smooth-shell grow must stop at.
static indexed_triangle_set make_half_sphere(double r, int n_lat, int n_lon)
{
    indexed_triangle_set its;
    // shell vertices: lat 0 = equator (z = 0) .. lat n_lat = pole
    std::vector<std::vector<int>> ring(n_lat + 1);
    for (int i = 0; i <= n_lat; ++i) {
        const double phi = 0.5 * M_PI * double(i) / double(n_lat); // 0 at equator, pi/2 at pole
        const double z = r * std::sin(phi), rr = r * std::cos(phi);
        if (i == n_lat) {
            ring[i].push_back(int(its.vertices.size()));
            its.vertices.emplace_back(Vec3f(0.f, 0.f, float(z)));
            continue;
        }
        for (int j = 0; j < n_lon; ++j) {
            const double th = 2.0 * M_PI * double(j) / double(n_lon);
            ring[i].push_back(int(its.vertices.size()));
            its.vertices.emplace_back(Vec3f(float(rr * std::cos(th)), float(rr * std::sin(th)), float(z)));
        }
    }
    for (int i = 0; i < n_lat; ++i) {
        for (int j = 0; j < n_lon; ++j) {
            const int j2 = (j + 1) % n_lon;
            const int a = ring[i][j], b = ring[i][j2];
            if (i + 1 == n_lat) {
                its.indices.emplace_back(Vec3i32(a, b, ring[i + 1][0]));
            } else {
                const int c = ring[i + 1][j2], d = ring[i + 1][j];
                its.indices.emplace_back(Vec3i32(a, b, c));
                its.indices.emplace_back(Vec3i32(a, c, d));
            }
        }
    }
    // flat cut face at z = 0, wound the other way so its normal is -Z
    const int centre = int(its.vertices.size());
    its.vertices.emplace_back(Vec3f(0.f, 0.f, 0.f));
    for (int j = 0; j < n_lon; ++j) {
        const int j2 = (j + 1) % n_lon;
        its.indices.emplace_back(Vec3i32(centre, ring[0][j2], ring[0][j]));
    }
    return its;
}

// The facet whose centroid is closest to `target`.
static size_t facet_nearest(const indexed_triangle_set &its, const Vec3d &target)
{
    size_t best = 0;
    double bestd = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const auto &t = its.indices[i];
        const Vec3d c = (its.vertices[t[0]].cast<double>() + its.vertices[t[1]].cast<double>() +
                         its.vertices[t[2]].cast<double>()) / 3.0;
        const double d = (c - target).squaredNorm();
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static Vec3d facet_centroid(const indexed_triangle_set &its, size_t f)
{
    const auto &t = its.indices[f];
    return (its.vertices[t[0]].cast<double>() + its.vertices[t[1]].cast<double>() +
            its.vertices[t[2]].cast<double>()) / 3.0;
}

// All facets of `its` whose own normal has a negative Z component (i.e. the flat cut face of the hemisphere).
static std::set<int> cut_face_facets(const indexed_triangle_set &its)
{
    std::set<int> out;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const auto &t = its.indices[i];
        const Vec3d a = its.vertices[t[0]].cast<double>(), b = its.vertices[t[1]].cast<double>(),
                    c = its.vertices[t[2]].cast<double>();
        const Vec3d n = (b - a).cross(c - a);
        if (n.norm() > 1e-12 && n.normalized().z() < -0.99) out.insert(int(i));
    }
    return out;
}

} // namespace

TEST_CASE("CurvePick: the total-spread cap bounds the grow, smooth shell removes it", "[CurvePick][Measure]")
{
    const double         r   = 10.0;
    indexed_triangle_set its = its_make_sphere(r, 2.0 * M_PI / 60.0); // ~3 deg tessellation
    REQUIRE(its.indices.size() > 500);
    Measuring m(its);

    const size_t seed = facet_nearest(its, Vec3d(0.0, 0.0, r));
    const Vec3d  hit  = facet_centroid(its, seed);

    // Default parameters == the legacy hard-coded 8 deg / 20 deg / 20000 grow.
    auto f_default = m.get_feature(seed, hit, Transform3d::Identity(), false, 0.5, 2);
    REQUIRE(f_default.has_value());
    REQUIRE(f_default->get_type() == SurfaceFeatureType::Curve);
    REQUIRE(f_default->plane_indices != nullptr);
    const size_t n_default = f_default->plane_indices->size();

    // Explicitly passing the defaults must give exactly the same patch (bit-for-bit legacy behaviour).
    auto f_explicit = m.get_feature(seed, hit, Transform3d::Identity(), false, 0.5, 2, CurvePickParams());
    REQUIRE(f_explicit.has_value());
    REQUIRE(f_explicit->plane_indices->size() == n_default);
    CHECK(*f_explicit->plane_indices == *f_default->plane_indices);

    SECTION("a 20 deg cap grows only a partial patch of the sphere") {
        // 20 deg of spread around the pole is a spherical cap -- a small fraction of the whole sphere.
        CHECK(n_default > 1);
        CHECK(n_default < its.indices.size() / 4);
    }

    SECTION("a wider cap grows a strictly larger patch") {
        CurvePickParams wide; wide.cap_deg = 60.0f;
        auto f = m.get_feature(seed, hit, Transform3d::Identity(), false, 0.5, 2, wide);
        REQUIRE(f.has_value());
        CHECK(f->plane_indices->size() > n_default);
        CHECK(f->plane_indices->size() < its.indices.size());
    }

    SECTION("smooth shell swallows the whole sphere -- nothing stops it") {
        auto f = m.get_feature(seed, hit, Transform3d::Identity(), false, 0.5, 2, CurvePickParams::smooth_shell());
        REQUIRE(f.has_value());
        REQUIRE(f->get_type() == SurfaceFeatureType::Curve);
        CHECK(f->plane_indices->size() == its.indices.size());
    }

    SECTION("smooth shell still honours max_facets") {
        auto f = m.get_feature(seed, hit, Transform3d::Identity(), false, 0.5, 2,
                               CurvePickParams::smooth_shell(8.0f, 50));
        REQUIRE(f.has_value());
        CHECK(f->plane_indices->size() == 50);
    }
}

TEST_CASE("CurvePick: a smooth-shell grow on a half sphere stops at the cut face's rim", "[CurvePick][Measure]")
{
    const double         r   = 12.0;
    indexed_triangle_set its = make_half_sphere(r, 40, 80);
    const std::set<int>  cut = cut_face_facets(its);
    REQUIRE(cut.size() == 80);
    Measuring m(its);

    const size_t seed = facet_nearest(its, Vec3d(0.0, 0.0, r)); // the pole of the dome
    const Vec3d  hit  = facet_centroid(its, seed);

    auto f = m.get_feature(seed, hit, Transform3d::Identity(), false, 0.5, 2, CurvePickParams::smooth_shell());
    REQUIRE(f.has_value());
    REQUIRE(f->get_type() == SurfaceFeatureType::Curve);
    REQUIRE(f->plane_indices != nullptr);
    const std::vector<int> &patch = *f->plane_indices;

    // The whole outer shell, and NOT one facet of the flat cut face.
    CHECK(patch.size() == its.indices.size() - cut.size());
    for (int t : patch)
        REQUIRE(cut.count(t) == 0);

    SECTION("picking the cut face itself grows only the cut face") {
        const size_t cseed = *cut.begin();
        auto fc = m.get_feature(cseed, facet_centroid(its, cseed), Transform3d::Identity(), false, 0.5, 2,
                                CurvePickParams::smooth_shell());
        REQUIRE(fc.has_value());
        CHECK(fc->plane_indices->size() == cut.size());
    }
}

TEST_CASE("CurvePick: fit_patch recovers a sphere's centre and radius", "[CurvePick][Measure]")
{
    const double         r      = 8.0;
    const Vec3d          centre(3.0, -4.0, 5.0);
    indexed_triangle_set its    = its_make_sphere(r, 2.0 * M_PI / 90.0);
    for (auto &v : its.vertices) v += centre.cast<float>();

    std::vector<int> all(its.indices.size());
    std::iota(all.begin(), all.end(), 0);

    const PatchFit sph = fit_sphere_to_patch(its, all);
    REQUIRE(sph.ok);
    CHECK(sph.radius == Approx(r).margin(0.05));
    CHECK((sph.centre - centre).norm() < 0.05);

    SECTION("fit_patch prefers the sphere over the cylinder on a full sphere") {
        const PatchFit best = fit_patch(its, all);
        CHECK(best.shape == PatchShape::Sphere);
        CHECK(best.radius == Approx(r).margin(0.05));
        CHECK((best.centre - centre).norm() < 0.05);
        const PatchFit cyl = fit_cylinder_to_patch(its, all);
        REQUIRE(cyl.ok);
        CHECK(cyl.rel_residual > sph.rel_residual);
    }

    SECTION("a hemisphere patch also recovers the full sphere's centre and radius") {
        // Curve-to-curve on two hemispheres has to align centres, so the fit must work on half the shell.
        std::vector<int> upper;
        for (size_t i = 0; i < its.indices.size(); ++i)
            if (facet_centroid(its, i).z() > centre.z()) upper.push_back(int(i));
        REQUIRE(upper.size() > 100);
        const PatchFit half = fit_patch(its, upper);
        REQUIRE(half.shape == PatchShape::Sphere);
        CHECK(half.radius == Approx(r).margin(0.1));
        CHECK((half.centre - centre).norm() < 0.1);
    }
}

TEST_CASE("CurvePick: the cylinder fit is unchanged and wins on a cylinder", "[CurvePick][Measure]")
{
    const double         r   = 6.0, h = 25.0;
    indexed_triangle_set its = its_make_cylinder(r, h, 2.0 * M_PI / 120.0);

    // Only the lateral wall (its facet normals are perpendicular to +Z); the caps would spoil any fit.
    std::vector<int> wall;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const auto &t = its.indices[i];
        const Vec3d a = its.vertices[t[0]].cast<double>(), b = its.vertices[t[1]].cast<double>(),
                    c = its.vertices[t[2]].cast<double>();
        const Vec3d n = (b - a).cross(c - a);
        if (n.norm() > 1e-12 && std::abs(n.normalized().z()) < 0.1) wall.push_back(int(i));
    }
    REQUIRE(wall.size() > 100);

    const PatchFit cyl = fit_cylinder_to_patch(its, wall);
    REQUIRE(cyl.ok);
    CHECK(cyl.radius == Approx(r).margin(0.02));
    CHECK(std::abs(cyl.axis.normalized().z()) == Approx(1.0).margin(1e-3));
    // The returned point sits on the axis (x = y = 0 for this mesh) at the patch's axial midpoint.
    CHECK(std::hypot(cyl.centre.x(), cyl.centre.y()) < 0.02);
    CHECK(cyl.centre.z() == Approx(h / 2.0).margin(0.5));

    const PatchFit best = fit_patch(its, wall);
    CHECK(best.shape == PatchShape::Cylinder);
    CHECK(best.radius == Approx(r).margin(0.02));

    SECTION("a flat patch is described by neither -- fit_patch falls back to Plane") {
        indexed_triangle_set cube = its_make_cube(20.0, 20.0, 20.0);
        std::vector<int> top;
        for (size_t i = 0; i < cube.indices.size(); ++i) {
            const auto &t = cube.indices[i];
            const Vec3d a = cube.vertices[t[0]].cast<double>(), b = cube.vertices[t[1]].cast<double>(),
                        c = cube.vertices[t[2]].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            if (n.norm() > 1e-12 && n.normalized().z() > 0.99) top.push_back(int(i));
        }
        REQUIRE(top.size() == 2);
        CHECK(fit_patch(cube, top).shape == PatchShape::Plane);
    }
}
