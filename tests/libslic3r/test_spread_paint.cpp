// Edge: the Spread paint tool core (src/libslic3r/SpreadPaint.hpp). Test plan: section 6 of
// tests/design_spread_paint_tool.md. The meshes are built in code and the session takes a radius,
// not a time, so every case is deterministic.

#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/SpreadPaint.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

using namespace Slic3r;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
const EnforcerBlockerType kPaint = EnforcerBlockerType(3);

// size x size plane at z = 0, n x n cells, two triangles each, normals +z.
indexed_triangle_set make_plane(float size, int n)
{
    indexed_triangle_set its;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            its.vertices.emplace_back(size * i / n, size * j / n, 0.f);
    auto v = [n](int i, int j) { return j * (n + 1) + i; };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            its.indices.emplace_back(v(i, j), v(i + 1, j), v(i + 1, j + 1));
            its.indices.emplace_back(v(i, j), v(i + 1, j + 1), v(i, j + 1));
        }
    return its;
}

// Adds triangle abc, flipped if needed so its normal points along `outward`.
void add_oriented(indexed_triangle_set &its, int a, int b, int c, const Vec3f &outward)
{
    const Vec3f n = (its.vertices[b] - its.vertices[a]).cross(its.vertices[c] - its.vertices[a]);
    if (n.dot(outward) >= 0.f)
        its.indices.emplace_back(a, b, c);
    else
        its.indices.emplace_back(a, c, b);
}

// A U-shaped channel along y: outer width 6, walls 2 thick, bottom 2 thick, height 12, so the slot
// is 2 wide (x 2..4) and 10 deep (z 2..12). Length 40.
indexed_triangle_set make_u_channel()
{
    const std::vector<Vec2f> p { { 0, 0 }, { 6, 0 }, { 6, 12 }, { 4, 12 }, { 4, 2 }, { 2, 2 }, { 2, 12 }, { 0, 12 } }; // CCW in (x, z)
    const float L = 40.f;
    indexed_triangle_set its;
    for (float y : { 0.f, L })
        for (const Vec2f &q : p)
            its.vertices.emplace_back(q.x(), y, q.y());
    const int n = int(p.size());
    for (int i = 0; i < n; ++i) {
        const int   j = (i + 1) % n;
        const Vec2f d = p[j] - p[i];
        const Vec3f out(d.y(), 0.f, -d.x()); // right of a CCW edge
        add_oriented(its, i, j, n + j, out);
        add_oriented(its, i, n + j, n + i, out);
    }
    const int caps[6][3] = { { 0, 1, 4 }, { 0, 4, 5 }, { 1, 2, 3 }, { 1, 3, 4 }, { 0, 5, 6 }, { 0, 6, 7 } };
    for (const auto &t : caps) {
        add_oriented(its, t[0], t[1], t[2], Vec3f(0.f, -1.f, 0.f));
        add_oriented(its, n + t[0], n + t[1], n + t[2], Vec3f(0.f, 1.f, 0.f));
    }
    return its;
}

float triangle_area(const Vec3f &a, const Vec3f &b, const Vec3f &c) { return 0.5f * (b - a).cross(c - a).norm(); }

double painted_area(const TriangleSelector &sel, EnforcerBlockerType state)
{
    const indexed_triangle_set its = sel.get_facets(state);
    double                     area = 0.;
    for (const auto &t : its.indices)
        area += triangle_area(its.vertices[t[0]], its.vertices[t[1]], its.vertices[t[2]]);
    return area;
}

double facets_area(const indexed_triangle_set &its, const std::function<bool(int)> &pred)
{
    double area = 0.;
    for (int f = 0; f < int(its.indices.size()); ++f)
        if (pred(f)) {
            const auto &t = its.indices[f];
            area += triangle_area(its.vertices[t[0]], its.vertices[t[1]], its.vertices[t[2]]);
        }
    return area;
}

// Original facet containing p (on its plane), or -1.
int facet_at(const indexed_triangle_set &its, const Vec3f &p)
{
    for (int f = 0; f < int(its.indices.size()); ++f) {
        const auto &t = its.indices[f];
        const Vec3f a = its.vertices[t[0]], b = its.vertices[t[1]], c = its.vertices[t[2]];
        const Vec3f n = (b - a).cross(c - a);
        if (n.norm() <= 0.f || std::abs(n.normalized().dot(p - a)) > 1e-4f)
            continue;
        const float area = n.norm();
        const float u    = (c - b).cross(p - b).dot(n) / (area * area);
        const float v    = (a - c).cross(p - c).dot(n) / (area * area);
        const float w    = 1.f - u - v;
        if (u >= -1e-5f && v >= -1e-5f && w >= -1e-5f)
            return f;
    }
    return -1;
}

Vec3f centroid(const indexed_triangle_set &its, int f)
{
    const auto &t = its.indices[f];
    return (its.vertices[t[0]] + its.vertices[t[1]] + its.vertices[t[2]]) / 3.f;
}

// Is p (on the surface) inside a leaf painted `state`?
bool painted_at(const indexed_triangle_set &painted, const Vec3f &p)
{
    for (const auto &t : painted.indices) {
        const Vec3f a = painted.vertices[t[0]], b = painted.vertices[t[1]], c = painted.vertices[t[2]];
        const Vec3f n = (b - a).cross(c - a);
        const float nn = n.squaredNorm();
        if (nn <= 0.f || std::abs(n.dot(p - a)) > 1e-4f * std::sqrt(nn))
            continue;
        const float u = (c - b).cross(p - b).dot(n) / nn;
        const float v = (a - c).cross(p - c).dot(n) / nn;
        if (u >= -1e-6f && v >= -1e-6f && 1.f - u - v >= -1e-6f)
            return true;
    }
    return false;
}

SpreadParams params_at(const indexed_triangle_set &its, const Vec3f &hit, EnforcerBlockerType state = kPaint)
{
    SpreadParams p;
    p.hit   = hit;
    p.facet = facet_at(its, hit);
    p.state = state;
    REQUIRE(p.facet >= 0);
    return p;
}

double max_painted_distance(const TriangleSelector &sel, EnforcerBlockerType state, const Vec3f &seed)
{
    const indexed_triangle_set its = sel.get_facets(state);
    double                     d   = 0.;
    for (const Vec3f &v : its.vertices)
        d = std::max(d, double((v - seed).norm()));
    return d;
}

} // namespace

TEST_CASE("Spread: a flat disc is round, on a fine and on a two-triangle plane", "[TriangleSelector][Spread]")
{
    const float r          = 10.f;
    const float edge_limit = 0.05f;
    const Vec3f seed(50.f, 50.f, 0.f);
    const double disc = M_PI * r * r;

    for (int n : { 1, 100 }) {
        INFO("plane with " << 2 * n * n << " triangles");
        const indexed_triangle_set its = make_plane(100.f, n);
        const TriangleMesh         mesh(its);

        TriangleSelector geo(mesh);
        SpreadParams     p = params_at(its, seed);
        p.edge_limit       = edge_limit;
        SpreadSession(geo, p).grow_to(r);

        TriangleSelector euc(mesh);
        p.distance_mode = SpreadParams::DistanceMode::Euclidean;
        SpreadSession(euc, p).grow_to(r);

        const double a_geo = painted_area(geo, kPaint);
        const double a_euc = painted_area(euc, kPaint);
        CHECK(std::abs(a_geo - disc) / disc < 0.03);
        CHECK(std::abs(a_geo - a_euc) / a_euc < 0.01);
        CHECK(max_painted_distance(geo, kPaint, seed) <= r + edge_limit);

        // Round, not a polygon: points just inside the circle all round are painted, points just
        // outside are not.
        const indexed_triangle_set painted = geo.get_facets(kPaint);
        for (int k = 0; k < 24; ++k) {
            const float a = float(2. * M_PI * k / 24.);
            const Vec3f dir(std::cos(a), std::sin(a), 0.f);
            INFO("direction " << k);
            CHECK(painted_at(painted, seed + (r - 0.3f) * dir));
            CHECK_FALSE(painted_at(painted, seed + (r + 0.3f) * dir));
        }
    }
}

TEST_CASE("Spread: relative to the seed, a cube top stays on the top face", "[TriangleSelector][Spread]")
{
    const indexed_triangle_set its = its_make_cube(20., 20., 20.);
    const TriangleMesh         mesh(its);
    TriangleSelector           sel(mesh);
    SpreadParams               p = params_at(its, Vec3f(7.f, 9.f, 20.f));
    p.angle_deg                  = 30.f;
    p.angle_mode                 = SpreadParams::AngleMode::Seed;
    SpreadSession(sel, p).grow_to(1000.f);

    CHECK(painted_area(sel, kPaint) == Approx(400.).epsilon(1e-4));
    const indexed_triangle_set painted = sel.get_facets(kPaint);
    for (const auto &t : painted.indices) {
        const Vec3f n = (painted.vertices[t[1]] - painted.vertices[t[0]]).cross(painted.vertices[t[2]] - painted.vertices[t[0]]).normalized();
        CHECK(n.z() > 0.999f);
    }
}

TEST_CASE("Spread: neighbour mode wraps a cylinder, seed mode stays within the angle", "[TriangleSelector][Spread]")
{
    // 36 segments, 10 degrees apart.
    const indexed_triangle_set its = its_make_cylinder(10., 20., 2. * M_PI / 36. * 1.0001);
    const TriangleMesh         mesh(its);
    const std::vector<Vec3f>   normals = its_face_normals(its);
    int                        seed_facet = -1;
    for (int f = 0; f < int(its.indices.size()) && seed_facet < 0; ++f)
        if (std::abs(normals[f].z()) < 1e-3f)
            seed_facet = f;
    REQUIRE(seed_facet >= 0);
    const Vec3f seed_n = normals[seed_facet];

    SpreadParams p = params_at(its, centroid(its, seed_facet));
    p.angle_deg    = 15.f;

    {
        TriangleSelector sel(mesh);
        p.angle_mode = SpreadParams::AngleMode::Neighbour;
        SpreadSession(sel, p).grow_to(kInf);
        const double mantle = facets_area(its, [&](int f) { return std::abs(normals[f].z()) < 1e-3f; });
        CHECK(painted_area(sel, kPaint) == Approx(mantle).epsilon(1e-4));
    }
    {
        TriangleSelector sel(mesh);
        p.angle_mode = SpreadParams::AngleMode::Seed;
        SpreadSession(sel, p).grow_to(kInf);
        const double within = facets_area(its, [&](int f) {
            return std::abs(normals[f].z()) < 1e-3f && normals[f].dot(seed_n) >= std::cos(15. * M_PI / 180.) - 1e-4;
        });
        CHECK(within > 0.);
        CHECK(painted_area(sel, kPaint) == Approx(within).epsilon(1e-4));
    }
}

TEST_CASE("Spread: the surface distance does not jump across a slot", "[TriangleSelector][Spread]")
{
    const indexed_triangle_set its = make_u_channel();
    const TriangleMesh         mesh(its);
    REQUIRE(its_num_open_edges(its) == 0);
    const std::vector<Vec3f> normals = its_face_normals(its);

    // Left wall's inner face (x = 2, normal +x), 1 mm below the rim, mid-length.
    SpreadParams p = params_at(its, Vec3f(2.f, 20.f, 11.f));
    REQUIRE(normals[p.facet].x() > 0.999f);
    p.angle_deg  = 90.f;
    p.edge_limit = 0.1f;

    // Paint on the right wall's inner face (x = 4, normal -x).
    auto far_wall_paint = [&](const TriangleSelector &sel) {
        const indexed_triangle_set painted = sel.get_facets(kPaint);
        double                     area    = 0.;
        for (const auto &t : painted.indices) {
            const Vec3f a = painted.vertices[t[0]], b = painted.vertices[t[1]], c = painted.vertices[t[2]];
            const Vec3f n = (b - a).cross(c - a).normalized();
            if (n.x() < -0.999f && std::abs(a.x() - 4.f) < 1e-4f)
                area += triangle_area(a, b, c);
        }
        return area;
    };

    TriangleSelector geo(mesh);
    SpreadSession(geo, p).grow_to(5.f);
    CHECK(painted_area(geo, kPaint) > 10.);
    CHECK(far_wall_paint(geo) == 0.);

    TriangleSelector euc(mesh);
    p.distance_mode = SpreadParams::DistanceMode::Euclidean;
    SpreadSession(euc, p).grow_to(5.f);
    CHECK(far_wall_paint(euc) > 1.);
}

TEST_CASE("Spread: a thin wall's other side is never painted", "[TriangleSelector][Spread]")
{
    const indexed_triangle_set its = its_make_cube(30., 30., 0.8);
    const TriangleMesh         mesh(its);
    TriangleSelector           sel(mesh);
    SpreadParams               p = params_at(its, Vec3f(13.f, 17.f, 0.8f));
    p.angle_deg                  = 30.f;
    SpreadSession(sel, p).grow_to(100.f);

    const indexed_triangle_set painted = sel.get_facets(kPaint);
    REQUIRE(! painted.indices.empty());
    for (const Vec3f &v : painted.vertices)
        CHECK(v.z() == Approx(0.8f));
    CHECK(painted_area(sel, kPaint) == Approx(900.).epsilon(1e-4));
}

TEST_CASE("Spread: growing in steps paints the same as one step", "[TriangleSelector][Spread]")
{
    auto check = [](const indexed_triangle_set &its, const Vec3f &seed) {
        const TriangleMesh mesh(its);
        SpreadParams       p = params_at(its, seed);
        p.angle_deg          = 40.f;
        p.edge_limit         = 0.1f;

        TriangleSelector steps(mesh);
        SpreadSession    s(steps, p);
        s.grow_to(3.f);
        s.grow_to(7.f);
        s.grow_to(10.f);

        TriangleSelector once(mesh);
        SpreadSession(once, p).grow_to(10.f);

        CHECK(painted_area(once, kPaint) > 1.);
        CHECK(steps.serialize() == once.serialize());
    };
    SECTION("two-triangle plane") { check(make_plane(100.f, 1), Vec3f(40.f, 55.f, 0.f)); }
    SECTION("fine plane") { check(make_plane(100.f, 50), Vec3f(40.3f, 55.7f, 0.f)); }
    SECTION("cylinder")
    {
        const indexed_triangle_set its = its_make_cylinder(6., 30., 2. * M_PI / 48.);
        const std::vector<Vec3f>   normals = its_face_normals(its);
        int                        f = 0;
        while (std::abs(normals[f].z()) > 1e-3f)
            ++f;
        check(its, centroid(its, f));
    }
}

TEST_CASE("Spread: the patch only grows", "[TriangleSelector][Spread]")
{
    const indexed_triangle_set its = make_plane(100.f, 50);
    const TriangleMesh         mesh(its);
    const Vec3f                seed(37.f, 41.f, 0.f);
    TriangleSelector           sel(mesh);
    SpreadParams               p = params_at(its, seed);
    p.edge_limit                 = 0.1f;
    SpreadSession s(sel, p);

    s.grow_to(8.f);
    const double               a1 = painted_area(sel, kPaint);
    const indexed_triangle_set p1 = sel.get_facets(kPaint);
    std::vector<Vec3f>         inside;
    for (float x = seed.x() - 10.f; x <= seed.x() + 10.f; x += 0.5f)
        for (float y = seed.y() - 10.f; y <= seed.y() + 10.f; y += 0.5f)
            if (painted_at(p1, Vec3f(x, y, 0.f)))
                inside.emplace_back(x, y, 0.f);
    REQUIRE(inside.size() > 100);

    s.grow_to(15.f);
    CHECK(painted_area(sel, kPaint) > a1);
    const indexed_triangle_set p2 = sel.get_facets(kPaint);
    size_t                     lost = 0;
    for (const Vec3f &q : inside)
        if (! painted_at(p2, q))
            ++lost;
    CHECK(lost == 0);
}

TEST_CASE("Spread: keep existing paint keeps it and stops at it", "[TriangleSelector][Spread]")
{
    // 2 mm cells; a stripe of cells x 60..62 over the whole height is painted state 2.
    const int                  n   = 50;
    const indexed_triangle_set its = make_plane(100.f, n);
    const TriangleMesh         mesh(its);
    const EnforcerBlockerType  stripe_state = EnforcerBlockerType(2);
    auto prepaint = [&](TriangleSelector &sel) {
        for (int j = 0; j < n; ++j) {
            const int base = 2 * (j * n + 30);
            sel.set_facet(base, stripe_state);
            sel.set_facet(base + 1, stripe_state);
        }
    };
    SpreadParams p = params_at(its, Vec3f(30.f, 50.f, 0.f));

    SECTION("keep existing")
    {
        TriangleSelector sel(mesh);
        prepaint(sel);
        p.keep_existing = true;
        SpreadSession s(sel, p);
        s.grow_to(kInf);
        CHECK(s.exhausted());
        CHECK(painted_area(sel, stripe_state) == Approx(200.).epsilon(1e-4));
        CHECK(painted_area(sel, kPaint) == Approx(6000.).epsilon(1e-4)); // the left part only
        CHECK(painted_area(sel, EnforcerBlockerType::NONE) == Approx(3800.).epsilon(1e-4));
    }
    SECTION("keep existing protects finer paint inside a covered facet")
    {
        TriangleSelector sel(mesh);
        // A brush dot inside one facet of the left part, in state 2.
        const Vec3f dot(20.5f, 50.5f, 0.f);
        sel.select_patch(facet_at(its, dot),
                         TriangleSelector::SinglePointCursor::cursor_factory(dot, Vec3f(20.5f, 50.5f, 100.f), 0.3f,
                                                                             TriangleSelector::CursorType::CIRCLE, Transform3d::Identity(),
                                                                             TriangleSelector::ClippingPlane()),
                         stripe_state, Transform3d::Identity(), true);
        const double dot_area = painted_area(sel, stripe_state);
        REQUIRE(dot_area > 0.1);
        p.keep_existing = true;
        SpreadSession(sel, p).grow_to(kInf);
        CHECK(painted_area(sel, stripe_state) == Approx(dot_area).epsilon(1e-5));
        CHECK(painted_area(sel, kPaint) == Approx(10000. - dot_area).epsilon(1e-4));
    }
    SECTION("paint over")
    {
        TriangleSelector sel(mesh);
        prepaint(sel);
        p.keep_existing = false;
        SpreadSession(sel, p).grow_to(kInf);
        CHECK(painted_area(sel, stripe_state) == 0.);
        CHECK(painted_area(sel, kPaint) == Approx(10000.).epsilon(1e-4));
    }
}

TEST_CASE("Spread: erasing spreads NONE", "[TriangleSelector][Spread]")
{
    const int                  n   = 50;
    const indexed_triangle_set its = make_plane(100.f, n);
    const TriangleMesh         mesh(its);
    const EnforcerBlockerType  base = EnforcerBlockerType(1);
    TriangleSelector           sel(mesh);
    for (int f = 0; f < int(its.indices.size()); ++f)
        sel.set_facet(f, base);

    SpreadParams p = params_at(its, Vec3f(50.f, 50.f, 0.f), EnforcerBlockerType::NONE);
    p.edge_limit   = 0.05f;
    SpreadSession s(sel, p);
    s.grow_to(10.f);
    const double disc = M_PI * 100.;
    CHECK(std::abs((10000. - painted_area(sel, base)) - disc) / disc < 0.03);
    CHECK(sel.serialize().used_states[size_t(base)]);

    s.grow_to(kInf);
    CHECK_FALSE(sel.has_facets(base));
    CHECK_FALSE(sel.serialize().used_states[size_t(base)]);
}

TEST_CASE("Spread: clipping and overhang filters match Smart fill", "[TriangleSelector][Spread]")
{
    auto compare = [](const indexed_triangle_set &its, int seed_facet, float angle, float overhang, const TriangleSelector::ClippingPlane &clp) {
        const TriangleMesh mesh(its);
        const Vec3f        hit = centroid(its, seed_facet);

        TriangleSelector smart(mesh);
        smart.seed_fill_select_triangles(hit, seed_facet, Transform3d::Identity(), clp, angle, overhang, true);
        smart.seed_fill_apply_on_triangles(kPaint);

        TriangleSelector spread(mesh);
        SpreadParams     p;
        p.hit          = hit;
        p.facet        = seed_facet;
        p.state        = kPaint;
        p.angle_deg    = angle;
        p.overhang_deg = overhang;
        p.clp          = clp;
        SpreadSession s(spread, p);
        s.grow_to(kInf);
        CHECK(s.exhausted());
        CHECK(painted_area(spread, kPaint) > 0.);
        CHECK(spread.serialize() == smart.serialize());
    };

    SECTION("clipping plane")
    {
        const indexed_triangle_set its = make_plane(100.f, 50);
        TriangleSelector::ClippingPlane clp;
        clp.normal = Vec3f(1.f, 0.f, 0.f);
        clp.offset = 70.f; // x > 70 is clipped
        compare(its, facet_at(its, Vec3f(20.3f, 30.7f, 0.f)), 30.f, 0.f, clp);
    }
    SECTION("overhang only")
    {
        const indexed_triangle_set its     = its_make_sphere(10., 2. * M_PI / 40.);
        const std::vector<Vec3f>   normals = its_face_normals(its);
        int                        bottom  = 0;
        for (int f = 1; f < int(normals.size()); ++f)
            if (normals[f].z() < normals[bottom].z())
                bottom = f;
        compare(its, bottom, 30.f, 45.f, TriangleSelector::ClippingPlane());
    }
    SECTION("angle limit on a sphere")
    {
        const indexed_triangle_set its = its_make_sphere(10., 2. * M_PI / 30.);
        compare(its, 7, 20.f, 0.f, TriangleSelector::ClippingPlane());
    }
}

TEST_CASE("Spread: an exhausted spread stops", "[TriangleSelector][Spread]")
{
    const indexed_triangle_set its = its_make_cube(20., 20., 20.);
    const TriangleMesh         mesh(its);
    TriangleSelector           sel(mesh);
    SpreadParams               p = params_at(its, Vec3f(5.f, 6.f, 20.f));
    SpreadSession              s(sel, p);
    s.grow_to(3.f);
    CHECK_FALSE(s.exhausted());
    s.grow_to(kInf);
    CHECK(s.exhausted());
    CHECK(s.max_reached() > 10.f);
    const auto data = sel.serialize();
    s.grow_to(kInf);
    s.grow_to(1e9f);
    CHECK(sel.serialize() == data);
    CHECK(painted_area(sel, kPaint) == Approx(400.).epsilon(1e-4));
}

TEST_CASE("Spread: the result round-trips with states above sixteen", "[TriangleSelector][Spread]")
{
    const indexed_triangle_set its = make_plane(100.f, 4);
    const TriangleMesh         mesh(its);
    const EnforcerBlockerType  high = EnforcerBlockerType(40);
    TriangleSelector           sel(mesh);
    sel.set_facet(0, EnforcerBlockerType(17));
    SpreadParams p = params_at(its, Vec3f(60.f, 60.f, 0.f), high);
    p.edge_limit   = 0.2f;
    SpreadSession(sel, p).grow_to(12.f);

    const auto data = sel.serialize();
    REQUIRE(data.used_states[size_t(high)]);
    REQUIRE(data.used_states[17]);
    TriangleSelector restored(mesh);
    restored.deserialize(data, true);
    CHECK(restored.serialize() == data);
    CHECK(painted_area(restored, high) == Approx(painted_area(sel, high)).epsilon(1e-6));
}

TEST_CASE("Spread: painted colour survives a 3MF save and load unchanged", "[TriangleSelector][Spread][3mf]")
{
    const TriangleMesh mesh(its_make_cube(20., 20., 20.));
    Model              model;
    ModelObject       *object = model.add_object();
    object->name              = "spread_cube";
    ModelVolume *volume       = object->add_volume(mesh);
    volume->name              = "cube";
    object->add_instance();

    TriangleSelector sel(volume->mesh());
    SpreadParams     p = params_at(volume->mesh().its, Vec3f(6.f, 7.f, 20.f), EnforcerBlockerType(2));
    p.angle_deg        = 95.f; // over the edges onto the sides
    p.edge_limit       = 0.2f;
    SpreadSession(sel, p).grow_to(9.f);
    REQUIRE(volume->mmu_segmentation_facets.set(sel));
    const auto want = volume->mmu_segmentation_facets.get_data();
    REQUIRE(! want.triangles_to_split.empty());

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(2);

    const boost::filesystem::path file = boost::filesystem::path(Slic3r::temporary_dir()) / "spread_paint_roundtrip.3mf";
    const std::string             path = file.string();
    StoreParams                   sp;
    sp.path     = path.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(sp));

    Model                     dst;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt { ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plates;
    std::vector<Preset *>     presets;
    bool                      is_bbl = false;
    Semver                    version;
    const bool                loaded = load_bbs_3mf(path.c_str(), &dst_config, &ctxt, &dst, &plates, &presets, &is_bbl, &version, nullptr,
                                                    LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                                        LoadStrategy::Silence);
    release_PlateData_list(plates);
    boost::filesystem::remove(file);
    REQUIRE(loaded);
    REQUIRE(dst.objects.size() == 1);
    REQUIRE(dst.objects.front()->volumes.size() == 1);
    const ModelVolume *back = dst.objects.front()->volumes.front();
    REQUIRE(back->mesh().its.indices.size() == mesh.its.indices.size());
    CHECK(back->mmu_segmentation_facets.get_data() == want);
}

TEST_CASE("Spread: benchmark on a 1M-facet sphere", "[TriangleSelector][Spread][.][Benchmark]")
{
    const indexed_triangle_set its = its_make_sphere(50., std::sqrt(4. * M_PI * M_PI / 1e6));
    const TriangleMesh         mesh(its);
    TriangleSelector           sel(mesh);
    SpreadParams               p;
    p.facet      = 0;
    p.hit        = centroid(its, 0);
    p.angle_deg  = 30.f;
    p.edge_limit = 0.05f;

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    SpreadSession s(sel, p);
    const float full = float(M_PI * 50.);
    double      worst = 0.;
    for (int i = 1; i <= 100 && ! s.exhausted(); ++i) {
        const auto a = clock::now();
        s.grow_to(full * i / 100.f);
        worst = std::max(worst, std::chrono::duration<double, std::milli>(clock::now() - a).count());
    }
    s.grow_to(kInf);
    const double total = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    WARN("Spread on " << its.indices.size() << " facets: worst step " << worst << " ms, total " << total << " ms");
    CHECK(s.exhausted());
}
