// Paint Split, phase 1 (tests/design_paint_split.md section 5.1): the region and its boundary, the Plane
// cutter, the keep-connected rule (both sides, kerf), Cut::perform_with_cutter_solid and the pick helpers.
//
// The synthetic figures are built with a real boolean union (MeshBoolean::mfd::union_all), so their skin
// has no interior faces and the masks, loops and volumes mean what they say.
#include <catch2/catch.hpp>

#include <libslic3r/AABBMesh.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PaintSplit.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const double PS_PI = 3.14159265358979323846;

indexed_triangle_set ps_moved(indexed_triangle_set its, const Vec3d& d)
{
    for (Vec3f& v : its.vertices)
        v += d.cast<float>();
    return its;
}

// A cylinder of radius r from x0 to x1 along +X, its axis through (y, z). A proper rotation, so the
// winding stays outward.
indexed_triangle_set ps_cylinder_x(double r, double x0, double x1, double y, double z)
{
    indexed_triangle_set its = its_make_cylinder(r, x1 - x0, 2. * PS_PI / 48.);
    for (Vec3f& v : its.vertices)
        v = Vec3f(v.z() + float(x0), v.y() + float(y), -v.x() + float(z));
    return its;
}

indexed_triangle_set ps_union(const std::vector<indexed_triangle_set>& parts)
{
    indexed_triangle_set out;
    REQUIRE(MeshBoolean::mfd::union_all(parts, out));
    // The union comes back in floats: vertices Manifold kept apart can land on one float and leave a
    // zero-area sliver, which is not part of the solid. Weld, drop those, and check the skin is closed.
    its_merge_vertices(out);
    const int degenerate = its_remove_degenerate_faces(out);
    its_compactify_vertices(out);
    std::map<uint64_t, int> edge_use;
    for (const Vec3i32& t : out.indices)
        for (int k = 0; k < 3; ++k) {
            int a = t[k], b = t[(k + 1) % 3];
            if (a > b) std::swap(a, b);
            ++edge_use[(uint64_t(uint32_t(a)) << 32) | uint32_t(b)];
        }
    int once = 0, many = 0;
    for (const auto& [e, n] : edge_use) {
        once += n == 1;
        many += n > 2;
    }
    INFO("union: " << out.indices.size() << " faces, " << degenerate << " degenerate removed, " << once
                   << " edges used once, " << many << " edges used more than twice");
    REQUIRE(once == 0);
    REQUIRE(many == 0);
    return out;
}

const Vec3d  PS_HEAD_C(0., 0., 30.);
const double PS_HEAD_R = 12.;

// Torso box, a neck cylinder (r 5), a head sphere (r 12, centre z 30) and two arms (r 3) at x = +-21 whose
// tops (z 25) reach above the neck line. Optionally a "ponytail" (r 1.5) fused to the head only, hanging
// below the neck line behind it.
indexed_triangle_set ps_figure(bool ponytail = false)
{
    std::vector<indexed_triangle_set> parts;
    parts.push_back(ps_moved(its_make_cube(50., 16., 30.), Vec3d(-25., -8., -30.)));
    parts.push_back(ps_moved(its_make_cylinder(5., 24., 2. * PS_PI / 64.), Vec3d(0., 0., -2.)));
    parts.push_back(ps_moved(its_make_sphere(PS_HEAD_R, 2. * PS_PI / 48.), PS_HEAD_C));
    parts.push_back(ps_moved(its_make_cylinder(3., 30., 2. * PS_PI / 48.), Vec3d(21., 0., -5.)));
    parts.push_back(ps_moved(its_make_cylinder(3., 30., 2. * PS_PI / 48.), Vec3d(-21., 0., -5.)));
    if (ponytail)
        parts.push_back(ps_moved(its_make_cylinder(1.5, 16., 2. * PS_PI / 32.), Vec3d(-10.5, 0., 12.)));
    return ps_union(parts);
}

// The facets that belong to the sphere (centre c, radius R): on it, facing away from its centre.
TriangleSelector::TriangleSplittingData ps_mask_sphere(const indexed_triangle_set& its, const Vec3d& c, double R,
                                                       const std::function<bool(const Vec3d&)>& extra = nullptr)
{
    TriangleMesh     tm(its);
    TriangleSelector sel(tm);
    for (int i = 0; i < int(its.indices.size()); ++i) {
        const Vec3i32& t = its.indices[i];
        const Vec3d g = (its.vertices[t[0]] + its.vertices[t[1]] + its.vertices[t[2]]).cast<double>() / 3.;
        const Vec3d n = its_face_normal(its, i).cast<double>();
        const Vec3d d = g - c;
        if (std::abs(d.norm() - R) < 0.6 && n.dot(d.normalized()) > 0.97 && (!extra || extra(g)))
            sel.set_facet(i, EnforcerBlockerType::ENFORCER);
    }
    return sel.serialize();
}

TriangleSelector::TriangleSplittingData ps_mask_where(const indexed_triangle_set& its, const std::function<bool(int, const Vec3d&)>& pick)
{
    TriangleMesh     tm(its);
    TriangleSelector sel(tm);
    for (int i = 0; i < int(its.indices.size()); ++i) {
        const Vec3i32& t = its.indices[i];
        const Vec3d g = (its.vertices[t[0]] + its.vertices[t[1]] + its.vertices[t[2]]).cast<double>() / 3.;
        if (pick(i, g))
            sel.set_facet(i, EnforcerBlockerType::ENFORCER);
    }
    return sel.serialize();
}

double ps_facet_area(const indexed_triangle_set& its, int i)
{
    const Vec3i32& t = its.indices[i];
    const Vec3d a = its.vertices[t[0]].cast<double>(), b = its.vertices[t[1]].cast<double>(), c = its.vertices[t[2]].cast<double>();
    return 0.5 * (b - a).cross(c - a).norm();
}

BoundingBoxf3 ps_bbox(const indexed_triangle_set& its)
{
    BoundingBoxf3 bb;
    for (const Vec3f& v : its.vertices)
        bb.merge(v.cast<double>());
    return bb;
}

// A closed cube [-s/2, s/2]^3 whose every face is an n x n grid of quads.
indexed_triangle_set ps_grid_cube(double s, int n)
{
    indexed_triangle_set its;
    const double h = 0.5 * s;
    auto face = [&](const Vec3d& o, const Vec3d& du, const Vec3d& dv) {
        const int base = int(its.vertices.size());
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
                its.vertices.push_back((o + du * (double(i) / n) + dv * (double(j) / n)).cast<float>());
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const int a = base + j * (n + 1) + i, b = a + 1, c = a + (n + 1), d = c + 1;
                its.indices.emplace_back(a, b, d);
                its.indices.emplace_back(a, d, c);
            }
    };
    // du x dv points outward on every face.
    face(Vec3d(-h, -h, h), Vec3d(s, 0, 0), Vec3d(0, s, 0));   // +Z
    face(Vec3d(-h, h, -h), Vec3d(s, 0, 0), Vec3d(0, -s, 0));  // -Z
    face(Vec3d(h, -h, -h), Vec3d(0, s, 0), Vec3d(0, 0, s));   // +X
    face(Vec3d(-h, h, -h), Vec3d(0, -s, 0), Vec3d(0, 0, s));  // -X
    face(Vec3d(h, h, -h), Vec3d(-s, 0, 0), Vec3d(0, 0, s));   // +Y
    face(Vec3d(-h, -h, -h), Vec3d(s, 0, 0), Vec3d(0, 0, s));  // -Y
    its_merge_vertices(its);
    return its;
}

bool ps_has_warning(const PaintSplitPieces& p, PaintSplitWarning w)
{
    return std::find(p.warnings.begin(), p.warnings.end(), w) != p.warnings.end();
}

void ps_log_rejoin(const char* what, const PaintSplitRejoin& r)
{
    WARN("Rejoin measurement (" << what << "): returned " << r.returned << ", body components " << r.body_components_before
                                << " -> " << r.body_components_after << ", piece cut area " << r.piece_cut_area
                                << ", body cut area after re-union " << r.body_cut_area_reunion << ", membrane "
                                << r.membrane_area << ", volume delta " << r.reunion_volume_delta << ", genus "
                                << r.reunion_genus << ", clean " << (r.clean ? "yes" : "no") << ", fallback "
                                << (r.fallback_used ? "yes" : "no"));
}

} // namespace

// ------------------------------------------------------------------------------------------- 1-4 region

TEST_CASE("Paint split region: a cube's top face is one component with one 4-corner loop", "[PaintSplit]")
{
    const indexed_triangle_set cube = its_make_cube(40., 40., 40.);
    const auto mask = ps_mask_where(cube, [&](int i, const Vec3d&) { return its_face_normal(cube, i).z() > 0.9f; });
    const PaintSplitRegion r = paint_split_region(cube, mask);
    REQUIRE(r.error == PaintSplitError::None);
    REQUIRE(r.num_components() == 1);
    REQUIRE(r.loops[0].size() == 1);
    CHECK(r.loops[0][0].pts.size() == 4);
    CHECK(r.loops[0][0].is_outer);
    CHECK_THAT(r.loops[0][0].length, WithinRel(160., 1e-6));
    CHECK_THAT(r.area[0], WithinRel(1600., 1e-6));
    CHECK(r.component_ok(0));
}

TEST_CASE("Paint split region: a sphere cap painted by a sphere cursor gives one closed loop on the surface", "[PaintSplit]")
{
    const double         R  = 20.;
    indexed_triangle_set sp = its_make_sphere(R, 2. * PS_PI / 40.);
    TriangleMesh         tm(sp);
    TriangleSelector     sel(tm);
    // A sphere cursor at the +Z pole, radius 9: sub-facet paint, flooded from the facet under the cursor.
    const Vec3f centre(0.f, 0.f, float(R));
    int         start = 0;
    float       best  = std::numeric_limits<float>::max();
    for (size_t i = 0; i < sp.indices.size(); ++i) {
        const Vec3i32& t = sp.indices[i];
        const float    d = ((sp.vertices[t[0]] + sp.vertices[t[1]] + sp.vertices[t[2]]) / 3.f - centre).norm();
        if (d < best) { best = d; start = int(i); }
    }
    sel.select_patch(start,
                     std::make_unique<TriangleSelector::Sphere>(centre, Vec3f(0.f, 0.f, float(3. * R)), 9.f, Transform3d::Identity(),
                                                                TriangleSelector::ClippingPlane()),
                     EnforcerBlockerType::ENFORCER, Transform3d::Identity(), true);
    const PaintSplitRegion r = paint_split_region(sp, sel.serialize());
    REQUIRE(r.error == PaintSplitError::None);
    REQUIRE(r.num_components() == 1);
    REQUIRE(r.loops[0].size() == 1);
    const PaintSplitLoop& loop = r.loops[0][0];
    REQUIRE(loop.pts.size() > 8);
    // Closed: the chain came back to its start, so the length is the sum of every segment including the last.
    double len = 0.;
    for (size_t i = 0; i < loop.pts.size(); ++i)
        len += (loop.pts[(i + 1) % loop.pts.size()].pos - loop.pts[i].pos).norm();
    CHECK_THAT(loop.length, WithinRel(len, 1e-6));
    // Every sample lies on the (tessellated) sphere's surface: within its chord sag of radius R.
    const AABBMesh aabb(sp);
    for (const DrawCutSample& s : loop.pts)
        CHECK(aabb.squared_distance(s.pos) < 1e-8);
}

TEST_CASE("Paint split region: bands are cut, small holes are filled at 10 % and kept at 0 %", "[PaintSplit]")
{
    const double               R  = 20.;
    const indexed_triangle_set sp = its_make_sphere(R, 2. * PS_PI / 60.);

    SECTION("a band round the equator") {
        const auto mask = ps_mask_where(sp, [](int, const Vec3d& g) { return std::abs(g.z()) < 4.; });
        const PaintSplitRegion r = paint_split_region(sp, mask);
        REQUIRE(r.error == PaintSplitError::None);
        REQUIRE(r.num_components() == 1);
        CHECK(r.loops[0].size() == 2);
        // Owner 2026-11-25 (D7 reversed): a band is cut as two cuts, each facing into the band.
        CHECK(r.component_ok(0));
        const std::vector<PaintSplitPlane> planes = paint_split_fit_planes(r, 0);
        REQUIRE(planes.size() == 2);
        for (const PaintSplitPlane& pl : planes) {
            REQUIRE(pl.valid);
            CHECK(std::abs(pl.normal.z()) > 0.99);
            // Pointing at the band: from z = +-4 towards z = 0.
            CHECK(pl.normal.z() * pl.point.z() < 0.);
        }
    }
    // Everything above z = -10, except two small unpainted islands round the +Z and +X directions.
    auto island = [&](const Vec3d& g) {
        const Vec3d d = g.normalized();
        return d.dot(Vec3d::UnitZ()) > std::cos(15. * PS_PI / 180.) || d.dot(Vec3d::UnitX()) > std::cos(15. * PS_PI / 180.);
    };
    const auto mask = ps_mask_where(sp, [&](int, const Vec3d& g) { return g.z() > -10. && !island(g); });
    SECTION("two islands, filled at 10 %") {
        const PaintSplitRegion r = paint_split_region(sp, mask);
        REQUIRE(r.error == PaintSplitError::None);
        REQUIRE(r.num_components() == 1);
        CHECK(r.holes_filled[0] == 2);
        CHECK(r.loops[0].size() == 1);
        CHECK(r.component_ok(0));
    }
    SECTION("two islands, kept at 0 %: three loops, cut as three cuts") {
        PaintSplitRegionParams p;
        p.fill_holes_pct = 0.;
        const PaintSplitRegion r = paint_split_region(sp, mask, p);
        REQUIRE(r.error == PaintSplitError::None);
        CHECK(r.holes_filled[0] == 0);
        CHECK(r.loops[0].size() == 3);
        CHECK(r.component_ok(0));
    }
}

TEST_CASE("Paint split region: two cells touching at one vertex are two components", "[PaintSplit]")
{
    const indexed_triangle_set cube = ps_grid_cube(40., 4);
    // On the top face (z = 20) the 10 mm cells [0,10]x[0,10] and [-10,0]x[-10,0] meet at the origin only.
    const auto mask = ps_mask_where(cube, [](int, const Vec3d& g) {
        return g.z() > 19.9 && ((g.x() > 0. && g.x() < 10. && g.y() > 0. && g.y() < 10.) ||
                                (g.x() < 0. && g.x() > -10. && g.y() < 0. && g.y() > -10.));
    });
    const PaintSplitRegion r = paint_split_region(cube, mask);
    REQUIRE(r.error == PaintSplitError::None);
    CHECK(r.num_components() == 2);
    CHECK_THAT(r.area[0], WithinRel(100., 1e-6));
    CHECK_THAT(r.area[1], WithinRel(100., 1e-6));
}

// ------------------------------------------------------------------------------------------- 6 plane fit

TEST_CASE("Paint split plane: a cap tilted by 10 degrees fits within 1 degree", "[PaintSplit]")
{
    const indexed_triangle_set sp = its_make_sphere(20., 2. * PS_PI / 60.);
    const Vec3d nt(std::sin(10. * PS_PI / 180.), 0., std::cos(10. * PS_PI / 180.));
    const auto  mask = ps_mask_where(sp, [&](int, const Vec3d& g) { return g.dot(nt) > 10.; });
    const PaintSplitRegion r = paint_split_region(sp, mask);
    REQUIRE(r.component_ok(0));
    const PaintSplitPlane pl = paint_split_fit_plane(r, 0);
    REQUIRE(pl.valid);
    CHECK(std::acos(std::clamp(pl.normal.dot(nt), -1., 1.)) * 180. / PS_PI < 1.);
    CHECK_THAT(pl.normal.dot(pl.point), WithinAbs(10., 1.));
    CHECK_FALSE(pl.far_from_boundary);
    CHECK_THAT(pl.u.cross(pl.v).dot(pl.normal), WithinAbs(1., 1e-9));

    SECTION("offset, snap and tilt") {
        PaintSplitPlaneParams p;
        p.offset      = 2.;
        p.snap_to_bed = true;
        const PaintSplitPlane s = paint_split_fit_plane(r, 0, p);
        CHECK_THAT(s.normal.z(), WithinAbs(1., 1e-12));
        CHECK_THAT((s.point - pl.point).norm(), WithinAbs(2., 1e-9));
        p.tilt_u_deg = 20.;
        const PaintSplitPlane t = paint_split_fit_plane(r, 0, p);
        CHECK_THAT(std::acos(std::clamp(t.normal.z(), -1., 1.)) * 180. / PS_PI, WithinAbs(20., 1e-6));
    }
}

// ------------------------------------------------------------------------------------------- 7 neckline

TEST_CASE("Paint split: the neckline cut takes the head and leaves the arms on the body", "[PaintSplit]")
{
    const indexed_triangle_set fig  = ps_figure();
    const auto                 mask = ps_mask_sphere(fig, PS_HEAD_C, PS_HEAD_R);
    const PaintSplitRegion     r    = paint_split_region(fig, mask);
    REQUIRE(r.error == PaintSplitError::None);
    REQUIRE(r.num_components() == 1);
    REQUIRE(r.component_ok(0));

    const PaintSplitPlane pl = paint_split_fit_plane(r, 0);
    REQUIRE(pl.valid);
    CHECK(pl.normal.z() > 0.999);
    // The sphere meets the neck at z = 30 - sqrt(144 - 25) = 19.09.
    CHECK_THAT(pl.point.z(), WithinAbs(30. - std::sqrt(119.), 0.3));

    const indexed_triangle_set cutter = paint_split_half_space(pl, ps_bbox(fig));
    const PaintSplitPieces     p      = paint_split_assign(fig, r, 0, cutter, true);
    REQUIRE(p.ok());
    ps_log_rejoin("figure, arms", p.rejoin);

    // The piece is the head: nothing of the arms (|x| >= 18) went with it.
    const BoundingBoxf3 pb = ps_bbox(p.piece);
    CHECK(pb.max.x() < 12.5);
    CHECK(pb.min.x() > -12.5);
    CHECK(pb.min.z() > 18.5);
    CHECK(p.returned_components == 2); // the two arm tops above the neck line came back
    CHECK(p.rejoin.piece_cut_area > 70.);  // the provenance survives: the neck section (pi 25)
    CHECK(its_num_open_edges(p.piece) == 0);
    CHECK(its_num_open_edges(p.body) == 0);
    CHECK(its_number_of_patches(p.piece) == 1);
    CHECK(its_number_of_patches(p.body) == 1);
    CHECK(p.loose.empty());
    // Volume conservation, relative 1e-6.
    CHECK_THAT(p.piece_volume + p.body_volume, WithinRel(p.source_volume, 1e-6));
    CHECK_FALSE(ps_has_warning(p, PaintSplitWarning::VolumeMismatch));
    // The arms are whole: the body's top is the arms' top at z = 25.
    CHECK_THAT(ps_bbox(p.body).max.z(), WithinAbs(25., 1e-3));
}

TEST_CASE("Paint split: ears crossing the plane stay on the body", "[PaintSplit]")
{
    // A head with two ears sticking forward (+X) above its face; the face cap x > 7 is selected. The ears
    // leave the sphere at x ~ 3, so the plane x ~ 7 cuts their tips off as separate components.
    const indexed_triangle_set head = ps_union({ its_make_sphere(10., 2. * PS_PI / 48.), ps_cylinder_x(1.5, -2., 16., 6.5, 7.),
                                                 ps_cylinder_x(1.5, -2., 16., -6.5, 7.) });
    const auto mask = ps_mask_sphere(head, Vec3d::Zero(), 10., [](const Vec3d& g) { return g.x() > 7.; });
    const PaintSplitRegion r = paint_split_region(head, mask);
    REQUIRE(r.component_ok(0));
    const PaintSplitPlane pl = paint_split_fit_plane(r, 0);
    REQUIRE(pl.valid);
    CHECK(pl.normal.x() > 0.99);

    const PaintSplitPieces p = paint_split_assign(head, r, 0, paint_split_half_space(pl, ps_bbox(head)), true);
    REQUIRE(p.ok());
    ps_log_rejoin("head, ears", p.rejoin);
    CHECK(p.returned_components == 2);
    // The face piece has no ear in it: the ears are at |y| = 6.5 +- 1.5, z = 7 +- 1.5.
    const BoundingBoxf3 pb = ps_bbox(p.piece);
    CHECK(pb.max.z() < 7.5);
    CHECK(pb.max.x() < 10.01);
    // The ears are whole on the body, and the body is one piece again.
    CHECK_THAT(ps_bbox(p.body).max.x(), WithinAbs(16., 1e-3));
    CHECK(its_number_of_patches(p.body) == 1);
    CHECK(its_num_open_edges(p.body) == 0);
    CHECK_THAT(p.piece_volume + p.body_volume, WithinRel(p.source_volume, 1e-6));
}

// ------------------------------------------------------------------------------------------- 10, 10b, 10c

TEST_CASE("Paint split keep-connected: a box crossing a separate blob gives the blob back", "[PaintSplit]")
{
    const indexed_triangle_set mesh = ps_union({ its_make_sphere(10., 2. * PS_PI / 48.), ps_moved(its_make_sphere(4., 2. * PS_PI / 32.), Vec3d(16., 0., 0.)) });
    const auto mask = ps_mask_sphere(mesh, Vec3d::Zero(), 10., [](const Vec3d& g) { return g.x() > 6.; });
    const PaintSplitRegion r = paint_split_region(mesh, mask);
    REQUIRE(r.component_ok(0));
    // A hand-built prism along X from x = 5 to x = 16: it takes the face cap and half of the blob.
    const indexed_triangle_set box = ps_moved(its_make_cube(11., 16., 16.), Vec3d(5., -8., -8.));
    const PaintSplitPieces     p   = paint_split_assign(mesh, r, 0, box, true);
    REQUIRE(p.ok());
    ps_log_rejoin("sphere, separate blob", p.rejoin);
    CHECK(p.returned_components == 1);
    CHECK(ps_has_warning(p, PaintSplitWarning::ComponentsReturned));
    CHECK(ps_bbox(p.piece).max.x() < 10.01);
    // The body is the head's rest and the WHOLE blob (x up to 20).
    CHECK(its_number_of_patches(p.body) == 2);
    CHECK_THAT(ps_bbox(p.body).max.x(), WithinAbs(20., 1e-2));
    CHECK_THAT(p.piece_volume + p.body_volume, WithinRel(p.source_volume, 1e-6));

    SECTION("a cutter that misses the region is refused") {
        const indexed_triangle_set far_box = ps_moved(its_make_cube(4., 4., 4.), Vec3d(-30., 0., 0.));
        CHECK(paint_split_assign(mesh, r, 0, far_box, true).error == PaintSplitError::EmptyPiece);
    }
}

TEST_CASE("Paint split body side: a ponytail hanging below the neck line goes with the head", "[PaintSplit]")
{
    const indexed_triangle_set fig  = ps_figure(true);
    const auto                 mask = ps_mask_sphere(fig, PS_HEAD_C, PS_HEAD_R);
    const PaintSplitRegion     r    = paint_split_region(fig, mask);
    REQUIRE(r.error == PaintSplitError::None);
    REQUIRE(r.num_components() == 1);
    CHECK(r.holes_filled[0] == 1); // the ponytail's root
    REQUIRE(r.component_ok(0));
    const PaintSplitPlane      pl     = paint_split_fit_plane(r, 0);
    const indexed_triangle_set cutter = paint_split_half_space(pl, ps_bbox(fig));

    SECTION("D18 attach (default)") {
        const PaintSplitPieces p = paint_split_assign(fig, r, 0, cutter, true);
        REQUIRE(p.ok());
        CHECK(p.attached_components == 1);
        CHECK(ps_has_warning(p, PaintSplitWarning::PiecesAttached));
        CHECK(p.loose.empty());
        CHECK_THAT(ps_bbox(p.piece).min.z(), WithinAbs(12., 1e-3)); // the ponytail's end
        CHECK(its_number_of_patches(p.body) == 1);
        CHECK(its_number_of_patches(p.piece) == 1);
        CHECK_THAT(p.piece_volume + p.body_volume, WithinRel(p.source_volume, 1e-6));
    }
    SECTION("D18 loose") {
        PaintSplitAssignParams ap;
        ap.loose_mode = PaintSplitLooseMode::Loose;
        const PaintSplitPieces p = paint_split_assign(fig, r, 0, cutter, true, ap);
        REQUIRE(p.ok());
        CHECK(p.attached_components == 0);
        REQUIRE(p.loose.size() == 1);
        CHECK(ps_has_warning(p, PaintSplitWarning::LoosePieces));
        CHECK(ps_bbox(p.piece).min.z() > 18.5);
        CHECK(its_number_of_patches(p.body) == 1);
        CHECK_THAT(p.piece_volume + p.body_volume + p.loose_volume, WithinRel(p.source_volume, 1e-6));
    }
}

TEST_CASE("Paint split kerf: the slot is cut at the neck only, the returned arms keep their volume", "[PaintSplit]")
{
    const indexed_triangle_set fig  = ps_figure();
    const PaintSplitRegion     r    = paint_split_region(fig, ps_mask_sphere(fig, PS_HEAD_C, PS_HEAD_R));
    REQUIRE(r.component_ok(0));
    const PaintSplitPlane      pl     = paint_split_fit_plane(r, 0);
    const indexed_triangle_set cutter = paint_split_half_space(pl, ps_bbox(fig));
    const PaintSplitPieces     plain  = paint_split_assign(fig, r, 0, cutter, true);
    REQUIRE(plain.ok());

    const double               kerf = 0.4;
    const indexed_triangle_set slab = paint_split_slab(pl, ps_bbox(fig), kerf);
    PaintSplitAssignParams     ap;
    ap.kerf_solid = &slab;
    ap.kerf       = kerf;
    const PaintSplitPieces k = paint_split_assign(fig, r, 0, cutter, true, ap);
    REQUIRE(k.ok());
    // Each side loses the neck section times half the kerf, and nothing more: an arm slotted as well would
    // take another 2 x pi 9 x 0.2 = 11.3 mm^3 from the body.
    const double neck_loss = plain.rejoin.piece_cut_area * 0.5 * kerf;
    CHECK_THAT(plain.body_volume - k.body_volume, WithinAbs(neck_loss, 0.1 * neck_loss));
    // The head widens just above the neck line (the sphere's section there is wider than the neck's), so its
    // loss is a little more than the neck section's.
    CHECK(plain.piece_volume - k.piece_volume > 0.9 * neck_loss);
    CHECK(plain.piece_volume - k.piece_volume < 1.4 * neck_loss);
    CHECK(its_number_of_patches(k.body) == 1);
    CHECK(its_num_open_edges(k.body) == 0);
    CHECK_THAT(ps_bbox(k.body).max.z(), WithinAbs(25., 1e-3));
}

// ------------------------------------------------------------------------------------------- 14 refusals

TEST_CASE("Paint split refusals: unrepairable mesh, empty mask, bad component; an open mesh is repaired", "[PaintSplit]")
{
    indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const auto           mask = ps_mask_where(cube, [](int i, const Vec3d&) { return i < 2; });

    SECTION("open mesh: repaired, not refused (owner 2026-11-25)") {
        indexed_triangle_set open = cube;
        open.indices.pop_back();
        const PaintSplitRegion r = paint_split_region(open, mask);
        CHECK(r.error == PaintSplitError::None);
        CHECK(r.repaired);
        CHECK(r.repair.holes_filled == 1);
        CHECK(r.repair.faces_added == 1);
    }
    SECTION("an edge used by three faces cannot be repaired") {
        indexed_triangle_set bad = cube;
        bad.indices.push_back(bad.indices.front());
        CHECK(paint_split_region(bad, mask).error == PaintSplitError::RepairFailed);
        CHECK(paint_split_assign(bad, paint_split_region(cube, mask), 0, cube, true).error == PaintSplitError::RepairFailed);
    }
    SECTION("empty mask") {
        CHECK(paint_split_region(cube, TriangleSelector::TriangleSplittingData()).error == PaintSplitError::EmptyMask);
    }
    SECTION("bad component") {
        const PaintSplitRegion r = paint_split_region(cube, mask);
        REQUIRE(r.error == PaintSplitError::None);
        CHECK(paint_split_assign(cube, r, 5, cube, true).error == PaintSplitError::BadComponent);
    }
    SECTION("every message is non-empty") {
        for (PaintSplitError e : { PaintSplitError::OpenMesh, PaintSplitError::NonManifold, PaintSplitError::EmptyMask,
                                   PaintSplitError::NoBoundary, PaintSplitError::Band, PaintSplitError::MultiLoop,
                                   PaintSplitError::CutCrossesRegion,
                                   PaintSplitError::BadComponent, PaintSplitError::BooleanFailed, PaintSplitError::EmptyPiece,
                                   PaintSplitError::EmptyBody, PaintSplitError::RepairFailed })
            CHECK(std::string(paint_split_error_message(e)).size() > 10);
    }
}

// ------------------------------------------------------------------------------------------- 11 picking

TEST_CASE("Paint split picking: pick colour takes every island, pick segment one", "[PaintSplit]")
{
    const indexed_triangle_set sp = its_make_sphere(20., 2. * PS_PI / 48.);
    const TriangleMesh         tm(sp);
    TriangleSelector           paint(tm);
    const double               c20 = std::cos(20. * PS_PI / 180.);
    double                     area_px = 0., area_mx = 0.;
    int                        seed_px = -1;
    for (int i = 0; i < int(sp.indices.size()); ++i) {
        const Vec3i32& t = sp.indices[i];
        const Vec3d    d = ((sp.vertices[t[0]] + sp.vertices[t[1]] + sp.vertices[t[2]]).cast<double>() / 3.).normalized();
        if (d.x() > c20) {
            paint.set_facet(i, EnforcerBlockerType::Extruder2);
            area_px += ps_facet_area(sp, i);
            if (seed_px < 0) seed_px = i;
        } else if (d.x() < -c20) {
            paint.set_facet(i, EnforcerBlockerType::Extruder2);
            area_mx += ps_facet_area(sp, i);
        } else if (d.z() > c20)
            paint.set_facet(i, EnforcerBlockerType::Extruder3);
        else
            paint.set_facet(i, EnforcerBlockerType::Extruder1);
    }
    const auto data = paint.serialize();
    REQUIRE(seed_px >= 0);

    const auto colour = paint_split_mask_from_paint(tm, data, EnforcerBlockerType::Extruder2);
    CHECK_THAT(paint_split_mask_area(tm, colour), WithinRel(area_px + area_mx, 1e-6));
    const PaintSplitRegion rc = paint_split_region(sp, colour);
    REQUIRE(rc.error == PaintSplitError::None);
    CHECK(rc.num_components() == 2); // one piece per island (D8)

    const auto segment = paint_split_mask_from_paint(tm, data, EnforcerBlockerType::Extruder2, seed_px);
    CHECK_THAT(paint_split_mask_area(tm, segment), WithinRel(area_px, 1e-6));
    const PaintSplitRegion rs = paint_split_region(sp, segment);
    REQUIRE(rs.error == PaintSplitError::None);
    CHECK(rs.num_components() == 1);

    // What the gizmo does on a click: the state under the hit, then the segment from the hit itself.
    const Vec3i32& t   = sp.indices[seed_px];
    const Vec3f    hit = (sp.vertices[t[0]] + sp.vertices[t[1]] + sp.vertices[t[2]]) / 3.f;
    CHECK(paint_split_state_at(tm, data, seed_px, hit) == EnforcerBlockerType::Extruder2);
    const auto from_hit = paint_split_mask_from_paint(tm, data, EnforcerBlockerType::Extruder2, seed_px, &hit);
    CHECK_THAT(paint_split_mask_area(tm, from_hit), WithinRel(area_px, 1e-6));
    // The unpainted state is pickable too (state NONE: here nothing is unpainted).
    CHECK(paint_split_mask_area(tm, paint_split_mask_from_paint(tm, data, EnforcerBlockerType::NONE)) < 1e-9);
}

// ------------------------------------------------------------------------------------------- 12 Cut path

namespace {

double ps_painted_area(const ModelObject* o, EnforcerBlockerType state)
{
    double a = 0.;
    for (const ModelVolume* v : o->volumes) {
        if (!v->is_model_part())
            continue;
        const indexed_triangle_set its = v->mmu_segmentation_facets.get_facets(*v, state);
        for (int i = 0; i < int(its.indices.size()); ++i)
            a += ps_facet_area(its, i);
    }
    return a;
}

double ps_volume_of(const ModelObject* o)
{
    double v = 0.;
    for (const ModelVolume* mv : o->volumes)
        if (mv->is_model_part())
            v += double(its_volume(mv->mesh().its)) * std::abs(mv->get_matrix().matrix().block<3, 3>(0, 0).determinant());
    return v;
}

} // namespace

TEST_CASE("Paint split through Cut: pieces, keep as parts and paint on both halves", "[PaintSplit]")
{
    Model        model;
    ModelObject* mo = model.add_object();
    mo->name        = "cube";
    indexed_triangle_set cube = its_make_cube(40., 40., 40.);
    for (Vec3f& v : cube.vertices) v -= Vec3f(20.f, 20.f, 20.f);
    ModelVolume* vol = mo->add_volume(TriangleMesh(cube));
    mo->add_instance();
    // Colour 2 on +X, colour 3 on the top.
    {
        TriangleSelector sel(vol->mesh());
        for (int i = 0; i < int(vol->mesh().its.indices.size()); ++i) {
            const Vec3f n = its_face_normal(vol->mesh().its, i);
            if (n.x() > 0.9f) sel.set_facet(i, EnforcerBlockerType::Extruder2);
            else if (n.z() > 0.9f) sel.set_facet(i, EnforcerBlockerType::Extruder3);
        }
        vol->mmu_segmentation_facets.set_data(sel.serialize());
    }
    const int                      which = GENERATE(0, 1);
    const Geometry::Transformation inst  = which == 0 ? Geometry::Transformation() :
        Geometry::Transformation(Geometry::assemble_transform(Vec3d(5., 7., 20.), Vec3d(0., 0., 30. * PS_PI / 180.), Vec3d(1.5, 1.5, 1.5)));
    CAPTURE(which);
    mo->instances.front()->set_transformation(inst);

    // The mask: the top face. A flat patch: the plane is pushed 10 mm (scaled) into the cube.
    const auto mask = ps_mask_where(vol->mesh().its, [&](int i, const Vec3d&) { return its_face_normal(vol->mesh().its, i).z() > 0.9f; });
    PaintSplitRegion r = paint_split_region(vol->mesh().its, mask);
    REQUIRE(r.component_ok(0));
    const Transform3d to_cut = mo->instances.front()->get_transformation().get_matrix_no_offset() * vol->get_matrix();
    paint_split_transform(r, to_cut);
    const double          scale = inst.get_scaling_factor().x();
    PaintSplitPlaneParams pp;
    pp.offset = -10. * scale;
    const PaintSplitPlane pl = paint_split_fit_plane(r, 0, pp);
    REQUIRE(pl.valid);
    CHECK(pl.normal.z() > 0.999);
    TriangleMesh in_cut(vol->mesh());
    in_cut.transform(to_cut, true);
    const indexed_triangle_set cutter = paint_split_half_space(pl, in_cut.bounding_box());

    PaintSplitKeep keep;
    keep.mask_volume_idx = 0;
    keep.region          = &r;
    keep.component       = 0;
    const ModelObjectCutAttributes base = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                          ModelObjectCutAttribute::KeepPaint;

    SECTION("new object") {
        Cut              cut(mo, 0, Transform3d::Identity(), base);
        PaintSplitReport rep;
        const ModelObjectPtrs& res = cut.perform_with_cutter_solid(cutter, true, keep, &rep);
        REQUIRE(rep.error == PaintSplitError::None);
        REQUIRE(res.size() == 2);
        const double vp = ps_volume_of(res[0]), vb = ps_volume_of(res[1]);
        CHECK_THAT(vp / (vp + vb), WithinRel(0.25, 1e-3));
        // Paint on both halves (spatial remap): the +X face splits 1 : 3, the top stays on the piece.
        const double s2 = scale * scale;
        CHECK_THAT(ps_painted_area(res[0], EnforcerBlockerType::Extruder2), WithinRel(400. * s2, 0.03));
        CHECK_THAT(ps_painted_area(res[1], EnforcerBlockerType::Extruder2), WithinRel(1200. * s2, 0.03));
        CHECK_THAT(ps_painted_area(res[0], EnforcerBlockerType::Extruder3), WithinRel(1600. * s2, 0.03));
        CHECK(ps_painted_area(res[1], EnforcerBlockerType::Extruder3) < 1.);
    }
    SECTION("keep as parts") {
        Cut              cut(mo, 0, Transform3d::Identity(), base | ModelObjectCutAttribute::KeepAsParts);
        PaintSplitReport rep;
        const ModelObjectPtrs& res = cut.perform_with_cutter_solid(cutter, true, keep, &rep);
        REQUIRE(rep.error == PaintSplitError::None);
        REQUIRE(res.size() == 1);
        int parts = 0;
        for (const ModelVolume* v : res[0]->volumes)
            parts += v->is_model_part();
        CHECK(parts == 2);
    }
    SECTION("a refused region returns nothing") {
        PaintSplitRegion bad = r;
        bad.comp_error[0]    = PaintSplitError::Band;
        keep.region          = &bad;
        Cut              cut(mo, 0, Transform3d::Identity(), base);
        PaintSplitReport rep;
        CHECK(cut.perform_with_cutter_solid(cutter, true, keep, &rep).empty());
        CHECK(rep.error == PaintSplitError::Band);
    }
}

// ------------------------------------------------------------------------------------------- repair while splitting
// Owner hand test 2026-11-25: an open part used to be refused with "Repair it first", and Repair dropped the
// painting. The split now closes the part itself and the halves take the painting back.

namespace {

struct PsFigureCut
{
    PaintSplitReport report;
    bool             region_repaired{ false };
    double           piece_volume{ 0. }, body_volume{ 0. };
    double           src_area2{ 0. }, src_area3{ 0. };
    double           piece_area2{ 0. }, piece_area3{ 0. }, body_area2{ 0. }, body_area3{ 0. };
    int              open_edges{ 0 };
    size_t           objects{ 0 };
};

// The figure as one part: colour 2 on the head, colour 3 on the neck; the head masked; cut at the neck
// through Cut::perform_with_cutter_solid with KeepPaint, as the gizmo's Apply does.
PsFigureCut ps_cut_figure(const indexed_triangle_set& mesh)
{
    PsFigureCut  out;
    Model        model;
    ModelObject* mo  = model.add_object();
    mo->name         = "figure";
    ModelVolume* vol = mo->add_volume(TriangleMesh(mesh));
    mo->add_instance();
    const indexed_triangle_set& its = vol->mesh().its;
    const Vec3d                 head = PS_HEAD_C - vol->get_offset();
    {
        TriangleSelector sel(vol->mesh());
        for (int i = 0; i < int(its.indices.size()); ++i) {
            const Vec3i32& t = its.indices[i];
            const Vec3d    g = (its.vertices[t[0]] + its.vertices[t[1]] + its.vertices[t[2]]).cast<double>() / 3. + vol->get_offset();
            const Vec3d    n = its_face_normal(its, i).cast<double>();
            const Vec3d    d = g - PS_HEAD_C;
            const double   radial = Vec2d(g.x(), g.y()).norm();
            if (std::abs(d.norm() - PS_HEAD_R) < 0.6 && n.dot(d.normalized()) > 0.97)
                sel.set_facet(i, EnforcerBlockerType::Extruder2);
            else if (std::abs(radial - 5.) < 0.3 && g.z() > 0.5 && g.z() < 18.)
                sel.set_facet(i, EnforcerBlockerType::Extruder3);
        }
        vol->mmu_segmentation_facets.set_data(sel.serialize());
    }
    out.src_area2 = ps_painted_area(mo, EnforcerBlockerType::Extruder2);
    out.src_area3 = ps_painted_area(mo, EnforcerBlockerType::Extruder3);

    PaintSplitRegion r = paint_split_region(its, ps_mask_sphere(its, head, PS_HEAD_R));
    REQUIRE(r.component_ok(0));
    out.region_repaired = r.repaired;
    paint_split_transform(r, mo->instances.front()->get_transformation().get_matrix_no_offset() * vol->get_matrix());
    const PaintSplitPlane pl = paint_split_fit_plane(r, 0);
    REQUIRE(pl.valid);
    const indexed_triangle_set cutter = paint_split_half_space(pl, mo->instance_bounding_box(0, true));

    PaintSplitKeep keep;
    keep.mask_volume_idx = 0;
    keep.region          = &r;
    Cut cut(mo, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs& res = cut.perform_with_cutter_solid(cutter, true, keep, &out.report);
    out.objects                = res.size();
    if (res.size() != 2)
        return out;
    out.piece_volume = ps_volume_of(res[0]);
    out.body_volume  = ps_volume_of(res[1]);
    out.piece_area2  = ps_painted_area(res[0], EnforcerBlockerType::Extruder2);
    out.piece_area3  = ps_painted_area(res[0], EnforcerBlockerType::Extruder3);
    out.body_area2   = ps_painted_area(res[1], EnforcerBlockerType::Extruder2);
    out.body_area3   = ps_painted_area(res[1], EnforcerBlockerType::Extruder3);
    for (const ModelObject* o : res)
        for (const ModelVolume* v : o->volumes)
            if (v->is_model_part())
                out.open_edges += int(its_num_open_edges(v->mesh().its));
    return out;
}

} // namespace

TEST_CASE("Paint split repair: an open painted figure splits like the closed one and keeps its paint", "[PaintSplit]")
{
    const indexed_triangle_set closed = ps_figure();
    // Holes: one big torso triangle on the +X side, and two small facets at the top of the painted head.
    indexed_triangle_set open = closed;
    {
        std::vector<int> drop;
        int              torso = -1;
        std::vector<std::pair<double, int>> top;
        for (int i = 0; i < int(open.indices.size()); ++i) {
            const Vec3i32& t = open.indices[i];
            const Vec3d    g = (open.vertices[t[0]] + open.vertices[t[1]] + open.vertices[t[2]]).cast<double>() / 3.;
            if (torso < 0 && g.x() > 24.99 && g.z() < -5.)
                torso = i;
            if ((g - PS_HEAD_C).norm() > 11. && g.z() > 40.)
                top.emplace_back(-g.z(), i);
        }
        REQUIRE(torso >= 0);
        REQUIRE(top.size() >= 3);
        std::sort(top.begin(), top.end());
        drop = { torso, top[0].second, top[2].second };
        std::sort(drop.begin(), drop.end());
        for (auto it = drop.rbegin(); it != drop.rend(); ++it)
            open.indices.erase(open.indices.begin() + *it);
    }
    REQUIRE(its_num_open_edges(open) > 0);

    const PsFigureCut c = ps_cut_figure(closed);
    const PsFigureCut o = ps_cut_figure(open);
    REQUIRE(c.report.error == PaintSplitError::None);
    REQUIRE(o.report.error == PaintSplitError::None);
    CHECK_FALSE(c.region_repaired);
    CHECK(o.region_repaired);
    CHECK(std::find(o.report.warnings.begin(), o.report.warnings.end(), PaintSplitWarning::Repaired) != o.report.warnings.end());
    REQUIRE(o.objects == 2);

    // Watertight pieces.
    CHECK(o.open_edges == 0);
    // The same split as the closed part: the holes were patched flat (the torso one exactly), so the volumes
    // agree to within the two small facets at the top of the head.
    CHECK_THAT(o.piece_volume, WithinRel(c.piece_volume, 0.01));
    CHECK_THAT(o.body_volume, WithinRel(c.body_volume, 0.01));

    // The paint survives on the right pieces: the head colour on the head, the neck colour on the body.
    INFO("open: head colour " << o.piece_area2 << " / " << o.src_area2 << " on the piece, " << o.body_area2 << " on the body; neck colour "
                              << o.body_area3 << " / " << o.src_area3 << " on the body, " << o.piece_area3 << " on the piece");
    REQUIRE(o.src_area2 > 100.);
    REQUIRE(o.src_area3 > 100.);
    CHECK(o.piece_area2 > 0.95 * o.src_area2);
    CHECK(o.body_area3 > 0.95 * o.src_area3);
    CHECK(o.body_area2 < 0.05 * o.src_area2);
    CHECK(o.piece_area3 < 0.05 * o.src_area3);
    WARN("Paint kept through repair + split (open figure): head colour " << 100. * o.piece_area2 / o.src_area2 << " %, neck colour "
                                                                         << 100. * o.body_area3 / o.src_area3 << " %");
}

TEST_CASE("Paint split repair: float slivers are welded, not refused", "[PaintSplit]")
{
    // A closed grid cube whose one top vertex is split in two a few float steps apart, as a boolean exported in
    // floats leaves it: the seam's edges no longer pair up.
    indexed_triangle_set cube = ps_grid_cube(40., 4);
    int                  v    = -1;
    for (int i = 0; i < int(cube.vertices.size()); ++i)
        if (std::abs(cube.vertices[i].z() - 20.f) < 1e-4f && std::abs(cube.vertices[i].x()) < 1e-4f && std::abs(cube.vertices[i].y()) < 1e-4f)
            v = i;
    REQUIRE(v >= 0);
    const int v2 = int(cube.vertices.size());
    cube.vertices.push_back(cube.vertices[v] + Vec3f(4e-6f, 0.f, 0.f));
    int moved = 0;
    for (Vec3i32& t : cube.indices) {
        const Vec3f g = (cube.vertices[t[0]] + cube.vertices[t[1]] + cube.vertices[t[2]]) / 3.f;
        for (int k = 0; k < 3; ++k)
            if (t[k] == v && g.x() > 0.f) {
                t[k] = v2;
                ++moved;
            }
    }
    REQUIRE(moved > 0);
    REQUIRE(its_num_open_edges(cube) > 0);

    indexed_triangle_set  fixed;
    PaintSplitRepairStats st;
    REQUIRE(paint_split_repair(cube, fixed, nullptr, st));
    CHECK(st.needed);
    CHECK(st.welded == 1);
    CHECK(st.holes_filled == 0);
    CHECK(its_num_open_edges(fixed) == 0);

    // The whole split goes through: the top face masked, a slab 10 mm deep taken.
    const auto mask = ps_mask_where(cube, [](int, const Vec3d& g) { return g.z() > 19.9; });
    const PaintSplitRegion r = paint_split_region(cube, mask);
    REQUIRE(r.component_ok(0));
    CHECK(r.repaired);
    CHECK_THAT(r.area[0], WithinRel(1600., 1e-6));
    PaintSplitPlaneParams pp;
    pp.offset                = -10.;
    const PaintSplitPlane pl = paint_split_fit_plane(r, 0, pp);
    const PaintSplitPieces p = paint_split_assign(cube, r, 0, paint_split_half_space(pl, ps_bbox(cube)), true);
    REQUIRE(p.ok());
    CHECK(std::find(p.warnings.begin(), p.warnings.end(), PaintSplitWarning::Repaired) != p.warnings.end());
    CHECK_THAT(p.piece_volume, WithinRel(16000., 1e-4));
    CHECK_THAT(p.body_volume, WithinRel(48000., 1e-4));
    CHECK(its_num_open_edges(p.piece) == 0);
    CHECK(its_num_open_edges(p.body) == 0);
}

// ------------------------------------------------------------------------------------------- several loops
// Owner hand test 2026-11-25 (a painted figure: trousers between a belt and the legs, a hat trim with holes):
// D7 reversed. A region with N edges is split off as its own piece with one cut per edge; the rest falls into
// however many connected pieces result.

namespace {

// A closed cylinder of radius r round the Z axis from z0 to z1, with `rings` rings of side facets (so a band
// can be painted on it) and fanned caps. Outward winding.
indexed_triangle_set ps_ring_cylinder(double r, double z0, double z1, int rings, int seg = 48)
{
    indexed_triangle_set its;
    for (int k = 0; k <= rings; ++k) {
        const double z = z0 + (z1 - z0) * double(k) / double(rings);
        for (int i = 0; i < seg; ++i) {
            const double a = 2. * PS_PI * double(i) / double(seg);
            its.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), float(z));
        }
    }
    auto V = [seg](int k, int i) { return k * seg + (i % seg); };
    for (int k = 0; k < rings; ++k)
        for (int i = 0; i < seg; ++i) {
            its.indices.emplace_back(V(k, i), V(k, i + 1), V(k + 1, i + 1));
            its.indices.emplace_back(V(k, i), V(k + 1, i + 1), V(k + 1, i));
        }
    const int bottom = int(its.vertices.size());
    its.vertices.emplace_back(0.f, 0.f, float(z0));
    const int top = int(its.vertices.size());
    its.vertices.emplace_back(0.f, 0.f, float(z1));
    for (int i = 0; i < seg; ++i) {
        its.indices.emplace_back(bottom, V(0, i + 1), V(0, i));
        its.indices.emplace_back(top, V(rings, i), V(rings, i + 1));
    }
    return its;
}

double ps_polygon_disc_area(double r, int seg) { return 0.5 * double(seg) * r * r * std::sin(2. * PS_PI / double(seg)); }

double ps_its_area(const indexed_triangle_set& its)
{
    double a = 0.;
    for (int i = 0; i < int(its.indices.size()); ++i)
        a += ps_facet_area(its, i);
    return a;
}

} // namespace

TEST_CASE("Paint split loops: a band round a cylinder splits into three pieces with two cuts", "[PaintSplit]")
{
    const double R = 10.;
    const int    SEG = 48;
    // z 0..40 in 2 mm rings; the band is z 16..24 (the trousers between the belt and the legs).
    const indexed_triangle_set cyl  = ps_ring_cylinder(R, 0., 40., 20, SEG);
    const auto                 mask = ps_mask_where(cyl, [](int, const Vec3d& g) { return g.z() > 16. && g.z() < 24. && std::hypot(g.x(), g.y()) > 5.; });
    const PaintSplitRegion     r    = paint_split_region(cyl, mask);
    REQUIRE(r.error == PaintSplitError::None);
    REQUIRE(r.num_components() == 1);
    REQUIRE(r.loops[0].size() == 2);
    REQUIRE(r.component_ok(0));

    const std::vector<PaintSplitPlane> planes = paint_split_fit_planes(r, 0);
    REQUIRE(planes.size() == 2);
    for (const PaintSplitPlane& pl : planes) {
        REQUIRE(pl.valid);
        CHECK(std::abs(pl.normal.z()) > 0.999);
        CHECK(pl.normal.z() * (pl.point.z() - 20.) < 0.); // each faces into the band
    }

    SECTION("the cut faces are the cylinder's cross-sections") {
        // 1 mm into the band, so the plane runs between rings of vertices.
        PaintSplitPlaneParams in1;
        in1.offset = 1.;
        for (size_t l = 0; l < planes.size(); ++l) {
            const PaintSplitPlane      pl  = paint_split_fit_loop_plane(r, 0, int(l), in1);
            const indexed_triangle_set cap = paint_split_cap(cyl, pl, r.loops[0][l]);
            REQUIRE(!cap.indices.empty());
            CHECK_THAT(ps_its_area(cap), WithinRel(ps_polygon_disc_area(R, SEG), 1e-3));
            for (int f = 0; f < int(cap.indices.size()); ++f)
                CHECK(its_face_normal(cap, f).cast<double>().dot(pl.normal) > 0.99);
        }
        // A plane moved past the part's end has no cut face.
        PaintSplitPlaneParams pp;
        pp.offset = 30.;
        const PaintSplitPlane far_plane = paint_split_fit_loop_plane(r, 0, 0, pp);
        CHECK(paint_split_cap(cyl, far_plane, r.loops[0][0]).indices.empty());
    }

    SECTION("engine: piece, body and one loose piece; volumes add up") {
        const PaintSplitPieces p = paint_split_assign_loops(cyl, r, 0, planes);
        REQUIRE(p.ok());
        REQUIRE(p.loose.size() == 1); // 3 pieces: the band, and the two ends
        const double disc = ps_polygon_disc_area(R, SEG);
        CHECK_THAT(p.piece_volume, WithinRel(disc * 8., 1e-3));
        CHECK_THAT(p.body_volume, WithinRel(disc * 16., 1e-3));
        CHECK_THAT(p.loose_volume, WithinRel(disc * 16., 1e-3));
        CHECK_THAT(p.piece_volume + p.body_volume + p.loose_volume, WithinRel(p.source_volume, 1e-6));
        const BoundingBoxf3 pb = ps_bbox(p.piece);
        CHECK_THAT(pb.min.z(), WithinAbs(16., 1e-3));
        CHECK_THAT(pb.max.z(), WithinAbs(24., 1e-3));
    }

    SECTION("engine: the body hint picks which end stays the body") {
        PaintSplitAssignParams ap;
        ap.body_hints = { Vec3d(0., 0., 40.) }; // the top cap's centre vertex
        const PaintSplitPieces p = paint_split_assign_loops(cyl, r, 0, planes, ap);
        REQUIRE(p.ok());
        CHECK(ps_bbox(p.body).min.z() > 23.);
        REQUIRE(p.loose.size() == 1);
        CHECK(ps_bbox(p.loose.front()).max.z() < 17.);
    }

    SECTION("a cut that passes through the region is refused, naming the loop") {
        // Offset 9: each cut moves 9 mm into the band, past its other edge, so no cut leaves the band whole.
        PaintSplitPlaneParams pp;
        pp.offset = 9.;
        const PaintSplitPieces p = paint_split_assign_loops(cyl, r, 0, paint_split_fit_planes(r, 0, pp));
        CHECK_FALSE(p.ok());
        CHECK(p.bad_loop >= 0);
    }

    SECTION("through Cut, painted: new objects or parts, colours kept") {
        Model        model;
        ModelObject* mo  = model.add_object();
        mo->name         = "figure";
        ModelVolume* vol = mo->add_volume(TriangleMesh(cyl));
        vol->name        = "figure";
        mo->add_instance();
        // The trousers in colour 2, everything else colour 3.
        {
            TriangleSelector sel(vol->mesh());
            for (int i = 0; i < int(cyl.indices.size()); ++i) {
                const Vec3i32& t = cyl.indices[i];
                const Vec3d    g = (cyl.vertices[t[0]] + cyl.vertices[t[1]] + cyl.vertices[t[2]]).cast<double>() / 3.;
                sel.set_facet(i, g.z() > 16. && g.z() < 24. && std::hypot(g.x(), g.y()) > 5. ? EnforcerBlockerType::Extruder2 :
                                                                                               EnforcerBlockerType::Extruder3);
            }
            vol->mmu_segmentation_facets.set_data(sel.serialize());
        }
        PaintSplitKeep keep;
        keep.mask_volume_idx = 0;
        keep.region          = &r;
        keep.component       = 0;
        keep.planes          = planes;
        const ModelObjectCutAttributes base = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepPaint;
        const double band_area = 2. * R * std::sin(PS_PI / SEG) * SEG * 8.; // the prism's side, 8 mm high

        SECTION("new object") {
            Cut              cut(mo, 0, Transform3d::Identity(), base);
            PaintSplitReport rep;
            const ModelObjectPtrs& res = cut.perform_with_paint_split_planes(keep, &rep);
            REQUIRE(rep.error == PaintSplitError::None);
            REQUIRE(res.size() == 2);
            int parts0 = 0, parts1 = 0;
            for (const ModelVolume* v : res[0]->volumes) parts0 += v->is_model_part();
            for (const ModelVolume* v : res[1]->volumes) parts1 += v->is_model_part();
            CHECK(parts0 == 1);
            CHECK(parts1 == 2); // the body and the "_loose1" rest piece, which the job makes an object
            bool tagged = false;
            for (const ModelVolume* v : res[1]->volumes)
                tagged = tagged || v->name == "figure_loose1";
            CHECK(tagged);
            CHECK_THAT(ps_volume_of(res[0]), WithinRel(ps_polygon_disc_area(R, SEG) * 8., 1e-3));
            // The piece keeps the trousers' colour; the ends keep theirs.
            CHECK_THAT(ps_painted_area(res[0], EnforcerBlockerType::Extruder2), WithinRel(band_area, 0.03));
            CHECK(ps_painted_area(res[1], EnforcerBlockerType::Extruder2) < 0.03 * band_area);
            CHECK(ps_painted_area(res[1], EnforcerBlockerType::Extruder3) > 2. * band_area);
        }
        SECTION("keep as parts") {
            Cut              cut(mo, 0, Transform3d::Identity(), base | ModelObjectCutAttribute::KeepAsParts);
            PaintSplitReport rep;
            const ModelObjectPtrs& res = cut.perform_with_paint_split_planes(keep, &rep);
            REQUIRE(rep.error == PaintSplitError::None);
            REQUIRE(res.size() == 1);
            int parts = 0;
            for (const ModelVolume* v : res[0]->volumes)
                parts += v->is_model_part();
            CHECK(parts == 3);
            CHECK_THAT(ps_volume_of(res[0]), WithinRel(ps_polygon_disc_area(R, SEG) * 40., 1e-3));
        }
    }
}

TEST_CASE("Paint split loops: a cap with two holes left open is cut along its three edges", "[PaintSplit]")
{
    const double               R  = 20.;
    const indexed_triangle_set sp = its_make_sphere(R, 2. * PS_PI / 60.);
    // Everything above z = -10, except two unpainted islands round +Z and +X (the hat trim's holes).
    auto island = [&](const Vec3d& g) {
        const Vec3d d = g.normalized();
        return d.dot(Vec3d::UnitZ()) > std::cos(15. * PS_PI / 180.) || d.dot(Vec3d::UnitX()) > std::cos(15. * PS_PI / 180.);
    };
    const auto             mask = ps_mask_where(sp, [&](int, const Vec3d& g) { return g.z() > -10. && !island(g); });
    PaintSplitRegionParams rp;
    rp.fill_holes_pct = 0.;
    const PaintSplitRegion r = paint_split_region(sp, mask, rp);
    REQUIRE(r.error == PaintSplitError::None);
    REQUIRE(r.num_components() == 1);
    REQUIRE(r.loops[0].size() == 3);
    REQUIRE(r.component_ok(0));

    const std::vector<PaintSplitPlane> planes = paint_split_fit_planes(r, 0);
    REQUIRE(planes.size() == 3);
    // The edge at z = -10 faces up into the region; each hole's cut faces away from its island, towards the
    // sphere's centre. (Which loop comes first is not part of the contract.)
    int rims = 0;
    for (size_t l = 0; l < 3; ++l) {
        if (planes[l].point.z() < -5.) {
            ++rims;
            CHECK(planes[l].normal.z() > 0.99);
        } else
            CHECK(planes[l].normal.dot(planes[l].point.normalized()) < -0.95);
    }
    CHECK(rims == 1);

    const PaintSplitPieces p = paint_split_assign_loops(sp, r, 0, planes);
    REQUIRE(p.ok());
    // The cap, plus the rest: the bottom below z = -10 and one plug per hole.
    CHECK(p.loose.size() == 2);
    CHECK_THAT(p.piece_volume + p.body_volume + p.loose_volume, WithinRel(p.source_volume, 1e-6));
    CHECK(p.body_volume > p.loose_volume);
    // The piece has no material at the poles of the islands.
    const AABBMesh piece(p.piece);
    CHECK(piece.squared_distance(Vec3d(0., 0., R)) > 0.25);
    CHECK(piece.squared_distance(Vec3d(R, 0., 0.)) > 0.25);
    CHECK(piece.squared_distance(Vec3d(0., R, 0.)) < 0.01);
}

TEST_CASE("Paint split pick: a click takes the connected patch, Ctrl+click adds and removes patches", "[PaintSplit]")
{
    // Colour 2 on two islands (+X and -X), colour 3 on the top, colour 1 elsewhere.
    const indexed_triangle_set sp = its_make_sphere(20., 2. * PS_PI / 48.);
    const TriangleMesh         tm(sp);
    TriangleSelector           paint(tm);
    const double               c20 = std::cos(20. * PS_PI / 180.);
    double                     area_px = 0., area_mx = 0., area_top = 0.;
    int                        seed_px = -1, seed_mx = -1, seed_top = -1;
    for (int i = 0; i < int(sp.indices.size()); ++i) {
        const Vec3i32& t = sp.indices[i];
        const Vec3d    d = ((sp.vertices[t[0]] + sp.vertices[t[1]] + sp.vertices[t[2]]).cast<double>() / 3.).normalized();
        if (d.x() > c20) {
            paint.set_facet(i, EnforcerBlockerType::Extruder2);
            area_px += ps_facet_area(sp, i);
            if (seed_px < 0) seed_px = i;
        } else if (d.x() < -c20) {
            paint.set_facet(i, EnforcerBlockerType::Extruder2);
            area_mx += ps_facet_area(sp, i);
            if (seed_mx < 0) seed_mx = i;
        } else if (d.z() > c20) {
            paint.set_facet(i, EnforcerBlockerType::Extruder3);
            area_top += ps_facet_area(sp, i);
            if (seed_top < 0) seed_top = i;
        } else
            paint.set_facet(i, EnforcerBlockerType::Extruder1);
    }
    REQUIRE(seed_px >= 0);
    REQUIRE(seed_mx >= 0);
    REQUIRE(seed_top >= 0);
    const auto data   = paint.serialize();
    auto       centre = [&](int f) {
        const Vec3i32& t = sp.indices[f];
        return Vec3f((sp.vertices[t[0]] + sp.vertices[t[1]] + sp.vertices[t[2]]) / 3.f);
    };

    // A plain click on colour 2 takes only the patch under it, not the other island of that colour.
    EnforcerBlockerType picked = EnforcerBlockerType::NONE;
    auto mask = paint_split_pick(tm, data, TriangleSelector::TriangleSplittingData(), seed_px, centre(seed_px), false, &picked);
    CHECK(picked == EnforcerBlockerType::Extruder2);
    CHECK_THAT(paint_split_mask_area(tm, mask), WithinRel(area_px, 1e-6));
    REQUIRE(paint_split_region(sp, mask).num_components() == 1);

    // Ctrl+click on the other island adds it.
    mask = paint_split_pick(tm, data, mask, seed_mx, centre(seed_mx), true);
    CHECK_THAT(paint_split_mask_area(tm, mask), WithinRel(area_px + area_mx, 1e-4));
    // Ctrl+click on a different colour adds that patch too (a region of several colours).
    mask = paint_split_pick(tm, data, mask, seed_top, centre(seed_top), true, &picked);
    CHECK(picked == EnforcerBlockerType::Extruder3);
    CHECK_THAT(paint_split_mask_area(tm, mask), WithinRel(area_px + area_mx + area_top, 1e-4));
    // Ctrl+click on a selected patch takes it out again.
    mask = paint_split_pick(tm, data, mask, seed_px, centre(seed_px), true);
    CHECK_THAT(paint_split_mask_area(tm, mask), WithinRel(area_mx + area_top, 1e-4));
    // A plain click replaces the selection.
    mask = paint_split_pick(tm, data, mask, seed_top, centre(seed_top), false);
    CHECK_THAT(paint_split_mask_area(tm, mask), WithinRel(area_top, 1e-6));

    // Combining a brushed (sub-facet) mask keeps its partial facets.
    TriangleSelector brush(tm);
    brush.select_patch(seed_px, std::make_unique<TriangleSelector::Sphere>(centre(seed_px), Vec3f(5.f * centre(seed_px)), 2.f,
                                                                            Transform3d::Identity(), TriangleSelector::ClippingPlane()),
                       EnforcerBlockerType::ENFORCER, Transform3d::Identity(), true, 0.f);
    const auto   brushed = brush.serialize();
    const double ab      = paint_split_mask_area(tm, brushed);
    REQUIRE(ab > 0.);
    const auto both = paint_split_mask_combine(tm, brushed, mask, PaintSplitMaskOp::Union);
    CHECK_THAT(paint_split_mask_area(tm, both), WithinRel(ab + area_top, 0.05));
}

// The owner's painted figure (not committed): run by hand with PAINT_SPLIT_GRINCH=<copy of the 3MF>.
// Every connected patch of every colour with two or more edges is split; the report lists what happened.
TEST_CASE("Paint split loops: owner's painted figure", "[.][PaintSplitGrinch]")
{
    const char* path = std::getenv("PAINT_SPLIT_GRINCH");
    if (path == nullptr || *path == 0) {
        WARN("PAINT_SPLIT_GRINCH not set");
        return;
    }
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Enable };
    Model                     model = Model::read_from_file(path, &config, &ctxt, LoadStrategy::LoadModel | LoadStrategy::Silence);
    REQUIRE(!model.objects.empty());
    const ModelVolume* mv = nullptr;
    for (const ModelObject* o : model.objects)
        for (const ModelVolume* v : o->volumes)
            if (v->is_model_part() && !v->mmu_segmentation_facets.empty() && (mv == nullptr || v->mesh().its.indices.size() > mv->mesh().its.indices.size()))
                mv = v;
    REQUIRE(mv != nullptr);
    const TriangleMesh&                           tm    = mv->mesh();
    const TriangleSelector::TriangleSplittingData paint = mv->mmu_segmentation_facets.get_data();
    WARN("facets " << tm.its.indices.size());
    // One seed per connected patch: walk the leaves of each state, skipping those an earlier patch covered.
    TriangleSelector sel(tm);
    sel.deserialize(paint, true);
    int tried = 0;
    for (size_t s = 0; s < paint.used_states.size() && tried < 40; ++s) {
        if (!paint.used_states[s])
            continue;
        std::vector<int>           src;
        const indexed_triangle_set leaves = sel.get_facets_strict(EnforcerBlockerType(s), &src);
        std::vector<char>          covered(tm.its.indices.size(), 0);
        for (size_t i = 0; i < leaves.indices.size() && tried < 40; ++i) {
            if (src[i] < 0 || covered[size_t(src[i])])
                continue;
            const Vec3i32& t   = leaves.indices[i];
            const Vec3f    hit = (leaves.vertices[t[0]] + leaves.vertices[t[1]] + leaves.vertices[t[2]]) / 3.f;
            const auto     mask = paint_split_pick(tm, paint, TriangleSelector::TriangleSplittingData(), src[i], hit, false);
            {
                TriangleSelector m(tm);
                m.deserialize(mask, true);
                std::vector<int> msrc;
                m.get_facets_strict(EnforcerBlockerType::ENFORCER, &msrc);
                for (int f : msrc)
                    if (f >= 0)
                        covered[size_t(f)] = 1;
            }
            const PaintSplitRegion r = paint_split_region(tm.its, mask);
            if (r.error != PaintSplitError::None || r.num_components() != 1)
                continue;
            const size_t edges = r.loops[0].size();
            if (edges < 2 || r.area[0] < 100.)
                continue;
            ++tried;
            // What Apply does: Follow paint, one draw-cut cutter per edge.
            std::vector<indexed_triangle_set> cutters;
            int                               bad_cut = -1;
            for (size_t l = 0; l < r.loops[0].size(); ++l) {
                const PaintSplitFollowCut c = paint_split_follow_cut(tm.its, r.loops[0][l], PaintSplitFollowParams(), tm.bounding_box());
                if (c.error != PaintSplitError::None) {
                    if (bad_cut < 0)
                        bad_cut = int(l);
                    WARN("  edge " << l << " (" << r.loops[0][l].pts.size() << " samples, " << r.loops[0][l].length << " mm) failed at stage " << c.stage);
                }
                cutters.push_back(c.sheet);
                BoundingBoxf3 sb;
                for (const Vec3f& v : c.sheet.vertices)
                    sb.merge(v.cast<double>());
                {
                    // Diagnostics: how much of the cut surface's rim lies inside the part (a rim inside cannot part it).
                    indexed_triangle_set pv = c.preview;
                    its_merge_vertices(pv);
                    std::map<std::pair<int, int>, int> use;
                    for (const Vec3i32& t : pv.indices)
                        for (int k = 0; k < 3; ++k) {
                            int x = t[k], y = t[(k + 1) % 3];
                            if (x > y) std::swap(x, y);
                            ++use[{ x, y }];
                        }
                    std::set<int> rim;
                    int nonmanifold = 0;
                    for (const auto& [e, n] : use) {
                        if (n == 1) { rim.insert(e.first); rim.insert(e.second); }
                        nonmanifold += n > 2;
                    }
                    static AABBMesh* aabb_mesh = nullptr;
                    if (aabb_mesh == nullptr) aabb_mesh = new AABBMesh(tm.its);
                    int inside = 0;
                    double worst = 0.;
                    for (int v : rim) {
                        int fi = -1; Vec3d cp;
                        const Vec3d q = pv.vertices[v].cast<double>();
                        const double d2 = aabb_mesh->squared_distance(q, fi, cp);
                        if (fi >= 0 && (q - cp).dot(its_face_normal(tm.its, fi).cast<double>()) < 0.) {
                            ++inside;
                            worst = std::max(worst, std::sqrt(d2));
                        }
                    }
                    WARN("    rim " << rim.size() << " vertices, " << inside << " inside the part (deepest " << worst << " mm), "
                                    << nonmanifold << " non-manifold edges, preview " << pv.indices.size() << " faces");
                }
                WARN("  edge " << l << ": " << r.loops[0][l].length << " mm, separates " << c.separates << ", ignored " << c.ignored << ", depth "
                               << c.depth << ", sheet " << c.sheet.indices.size() << " faces, size " << (sb.defined ? sb.size().norm() : 0.));
            }
            WARN("  cutters built, first failed edge " << bad_cut);
            const PaintSplitPieces p = paint_split_assign_cutters(tm.its, r, 0, cutters);
            WARN("state " << s << ": " << edges << " edges, " << r.holes_filled[0] << " holes filled, area " << r.area[0] << " mm2 -> "
                          << (p.ok() ? "ok" : paint_split_error_message(p.error)) << " bad_loop " << p.bad_loop << ", pieces "
                          << (p.ok() ? 2 + p.loose.size() : 0) << ", piece vol " << p.piece_volume << ", body vol " << p.body_volume
                          << ", loose vol " << p.loose_volume << ", flipped " << p.flipped_loops);
            CHECK(p.ok());
        }
    }
    CHECK(tried > 0);
}

// ------------------------------------------------------------------------------------------- follow paint
// Owner hand test 2026-11-26: a flat plane per edge sliced far into unpainted areas (the sack under the
// trousers, the whole head under the hat). The cut now follows the paint line: a draw-cut band + flat core.

namespace {

std::vector<indexed_triangle_set> ps_follow_cutters(const indexed_triangle_set& mesh, const PaintSplitRegion& r, int comp,
                                                    const PaintSplitFollowParams& fp = {})
{
    std::vector<indexed_triangle_set> out;
    const BoundingBoxf3               bbox = ps_bbox(mesh);
    for (const PaintSplitLoop& loop : r.loops[size_t(comp)]) {
        const PaintSplitFollowCut cut = paint_split_follow_cut(mesh, loop, fp, bbox);
        REQUIRE(cut.error == PaintSplitError::None);
        REQUIRE(!cut.preview.indices.empty());
        out.push_back(cut.sheet);
    }
    return out;
}

} // namespace

TEST_CASE("Paint split follow paint: a band round a cylinder is cut along its two paint lines", "[PaintSplit]")
{
    const double               R   = 10.;
    const int                  SEG = 48;
    const indexed_triangle_set cyl = ps_ring_cylinder(R, 0., 40., 40, SEG);
    // A wavy lower edge (the trouser waist under a belt): z = 16 + 2 sin(3 theta); a straight upper one at 24.
    auto in_band = [](const Vec3d& g) {
        const double th = std::atan2(g.y(), g.x());
        return std::hypot(g.x(), g.y()) > 5. && g.z() > 16. + 2. * std::sin(3. * th) && g.z() < 24.;
    };
    const auto             mask = ps_mask_where(cyl, [&](int, const Vec3d& g) { return in_band(g); });
    const PaintSplitRegion r    = paint_split_region(cyl, mask);
    REQUIRE(r.error == PaintSplitError::None);
    REQUIRE(r.num_components() == 1);
    REQUIRE(r.loops[0].size() == 2);

    SECTION("each cut surface stays near its own edge") {
        const BoundingBoxf3 bbox = ps_bbox(cyl);
        for (const PaintSplitLoop& loop : r.loops[0]) {
            PaintSplitFollowParams fp;
            const PaintSplitFollowCut cut = paint_split_follow_cut(cyl, loop, fp, bbox);
            REQUIRE(cut.error == PaintSplitError::None);
            CHECK(cut.separates); // the loop goes round the part
            Vec3d lc = Vec3d::Zero();
            for (const DrawCutSample& s : loop.pts)
                lc += s.pos;
            lc /= double(loop.pts.size());
            // Band, skirt and core: within the edge's own radius plus the skirt, nothing model-wide.
            double far_v = 0.;
            for (const Vec3f& v : cut.preview.vertices)
                far_v = std::max(far_v, (v.cast<double>() - lc).norm());
            CHECK(far_v < R + fp.extension + 4.);
        }
    }

    SECTION("the piece is the band: its skin and none of the rest's") {
        const PaintSplitPieces p = paint_split_assign_cutters(cyl, r, 0, ps_follow_cutters(cyl, r, 0));
        REQUIRE(p.ok());
        CHECK(p.loose.size() == 1);
        CHECK_THAT(p.piece_volume + p.body_volume + p.loose_volume, WithinRel(p.source_volume, 1e-6));
        // Side facets on the piece's surface: the selected ones are there, the unselected ones are not.
        const AABBMesh piece(p.piece);
        double sel_on = 0., sel_all = 0., unsel_on = 0.;
        for (int i = 0; i < int(cyl.indices.size()); ++i) {
            const Vec3i32& t = cyl.indices[i];
            const Vec3d    g = (cyl.vertices[t[0]] + cyl.vertices[t[1]] + cyl.vertices[t[2]]).cast<double>() / 3.;
            if (std::hypot(g.x(), g.y()) < 5.)
                continue;
            const bool on = piece.squared_distance(g) < 1e-4;
            if (in_band(g)) {
                sel_all += ps_facet_area(cyl, i);
                sel_on += on ? ps_facet_area(cyl, i) : 0.;
            } else if (on)
                unsel_on += ps_facet_area(cyl, i);
        }
        CHECK(sel_on > 0.95 * sel_all);
        CHECK(unsel_on < 0.05 * sel_all);
    }

    SECTION("through Cut: new objects, the band's colour on the piece") {
        Model        model;
        ModelObject* mo  = model.add_object();
        mo->name         = "figure";
        ModelVolume* vol = mo->add_volume(TriangleMesh(cyl));
        vol->name        = "figure";
        mo->add_instance();
        {
            TriangleSelector sel(vol->mesh());
            for (int i = 0; i < int(cyl.indices.size()); ++i) {
                const Vec3i32& t = cyl.indices[i];
                const Vec3d    g = (cyl.vertices[t[0]] + cyl.vertices[t[1]] + cyl.vertices[t[2]]).cast<double>() / 3.;
                sel.set_facet(i, in_band(g) ? EnforcerBlockerType::Extruder2 : EnforcerBlockerType::Extruder3);
            }
            vol->mmu_segmentation_facets.set_data(sel.serialize());
        }
        PaintSplitKeep keep;
        keep.mask_volume_idx = 0;
        keep.region          = &r;
        keep.component       = 0;
        keep.cutters         = ps_follow_cutters(cyl, r, 0);
        Cut              cut(mo, 0, Transform3d::Identity(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                            ModelObjectCutAttribute::KeepPaint);
        PaintSplitReport rep;
        const ModelObjectPtrs& res = cut.perform_with_paint_split_planes(keep, &rep);
        REQUIRE(rep.error == PaintSplitError::None);
        REQUIRE(res.size() == 2);
        const double a2_piece = ps_painted_area(res[0], EnforcerBlockerType::Extruder2);
        const double a2_rest  = ps_painted_area(res[1], EnforcerBlockerType::Extruder2);
        CHECK(a2_piece > 0.9 * (a2_piece + a2_rest));
        CHECK(ps_painted_area(res[0], EnforcerBlockerType::Extruder3) < 0.05 * a2_piece);
    }
}

TEST_CASE("Paint split follow paint: a patch on one face is a plug, the rest of the part is untouched", "[PaintSplit]")
{
    const indexed_triangle_set grid = ps_grid_cube(30., 30); // -15..15, 1 mm facets
    const auto mask = ps_mask_where(grid, [&](int i, const Vec3d& g) {
        return its_face_normal(grid, i).x() > 0.9f && std::abs(g.y()) < 6. && std::abs(g.z()) < 6.;
    });
    const PaintSplitRegion r = paint_split_region(grid, mask);
    REQUIRE(r.error == PaintSplitError::None);
    REQUIRE(r.num_components() == 1);
    REQUIRE(r.loops[0].size() == 1);
    PaintSplitFollowParams    fp;
    const PaintSplitFollowCut cut = paint_split_follow_cut(grid, r.loops[0][0], fp, ps_bbox(grid));
    REQUIRE(cut.error == PaintSplitError::None);
    CHECK_FALSE(cut.separates);
    const PaintSplitPieces p = paint_split_assign_cutters(grid, r, 0, { cut.sheet });
    INFO("error " << int(p.error) << " bad_loop " << p.bad_loop << " sheet faces " << cut.sheet.indices.size());
    REQUIRE(p.ok());
    // A plug a few mm deep under the patch, nowhere near the rest of the cube.
    const BoundingBoxf3 pb = ps_bbox(p.piece);
    CHECK(pb.min.x() > 15. - 3. * fp.depth);
    CHECK(pb.max.x() < 15. + 1e-3);
    CHECK(pb.size().y() < 16.);
    CHECK(pb.size().z() < 16.);
    CHECK(p.loose.empty());
    CHECK_THAT(p.piece_volume + p.body_volume, WithinRel(p.source_volume, 1e-6));
}
