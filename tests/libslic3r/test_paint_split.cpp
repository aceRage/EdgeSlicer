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
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
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

TEST_CASE("Paint split region: bands are refused, small holes are filled at 10 % and kept at 0 %", "[PaintSplit]")
{
    const double               R  = 20.;
    const indexed_triangle_set sp = its_make_sphere(R, 2. * PS_PI / 60.);

    SECTION("a band round the equator") {
        const auto mask = ps_mask_where(sp, [](int, const Vec3d& g) { return std::abs(g.z()) < 4.; });
        const PaintSplitRegion r = paint_split_region(sp, mask);
        REQUIRE(r.error == PaintSplitError::None);
        REQUIRE(r.num_components() == 1);
        CHECK(r.loops[0].size() == 2);
        CHECK(r.comp_error[0] == PaintSplitError::Band);
        CHECK_FALSE(r.component_ok(0));
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
    SECTION("two islands, kept at 0 %: three loops, refused") {
        PaintSplitRegionParams p;
        p.fill_holes_pct = 0.;
        const PaintSplitRegion r = paint_split_region(sp, mask, p);
        REQUIRE(r.error == PaintSplitError::None);
        CHECK(r.holes_filled[0] == 0);
        CHECK(r.loops[0].size() == 3);
        CHECK(r.comp_error[0] == PaintSplitError::MultiLoop);
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

TEST_CASE("Paint split refusals: open mesh, empty mask, bad component", "[PaintSplit]")
{
    indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const auto           mask = ps_mask_where(cube, [](int i, const Vec3d&) { return i < 2; });

    SECTION("open mesh") {
        indexed_triangle_set open = cube;
        open.indices.pop_back();
        CHECK(paint_split_region(open, mask).error == PaintSplitError::OpenMesh);
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
                                   PaintSplitError::BadComponent, PaintSplitError::BooleanFailed, PaintSplitError::EmptyPiece,
                                   PaintSplitError::EmptyBody })
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
