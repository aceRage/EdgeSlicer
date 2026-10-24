// ---------------------------------------------------------------------------
// OD11 / OrcaSlicer #13472 "Keep painting after cut" (merge 1c0d0b89cc): the
// cut call sites. The remap core (TriangleSelector::remap_painting,
// ModelVolume::save_painting / restore_painting) came in with texture
// displacement and is covered by [PaintRemap]; this file covers what a CUT does
// with it, on every one of EdgeSlicer's six Cut::perform_* paths (upstream has
// three): plane (with and without a kerf), curved sheet, drawn stroke, flexi
// joint, contour (part selection) and tongue-and-groove.
//
// How "the paint is on the right faces" is checked without trusting the code
// under test: a half's mesh plus its init shift is the cut frame (the source
// instance's linear part times the volume matrix). Every painted vertex is taken
// back through that instance matrix into the ORIGINAL cube's coordinates, where
// the painted face is known (x == +H for the +X face, etc.).
// ---------------------------------------------------------------------------

#include <catch2/catch.hpp>

#include <libslic3r/CutUtils.hpp>
#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/DrawCut.hpp>
#include <libslic3r/FlexiJoint.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>
#include <libslic3r/Utils.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

const double CP_CUBE = 40.0;
const double CP_H    = 0.5 * CP_CUBE; // half the side: the faces sit at +/- 20

indexed_triangle_set cp_centred_cube(double s = CP_CUBE)
{
    indexed_triangle_set its = its_make_cube(s, s, s);
    for (Vec3f &v : its.vertices)
        v -= Vec3f(float(0.5 * s), float(0.5 * s), float(0.5 * s));
    return its;
}

// Paint every facet whose normal is within ~25 degrees of `dir` with `state`.
TriangleSelector::TriangleSplittingData cp_paint_dir(const TriangleMesh &mesh, const Vec3f &dir, EnforcerBlockerType state)
{
    TriangleSelector sel(mesh);
    for (int i = 0; i < int(mesh.its.indices.size()); ++i)
        if (its_face_normal(mesh.its, i).dot(dir) > 0.9f)
            sel.set_facet(i, state);
    return sel.serialize();
}

double cp_area(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const Vec3i32 &f : its.indices)
        a += 0.5 * double((its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).norm());
    return a;
}

using FacetsMember = FacetsAnnotation ModelVolume::*;

struct PaintedOn
{
    double             area{ 0. };
    std::vector<Vec3d> original_pts; // painted vertices, back in the original cube's coordinates
};

// Gathers one channel's painted facets of every model part (connectors skipped) of `objects`.
// `instance_linear` is the SOURCE instance's matrix without offset - see the file header.
PaintedOn cp_painted(const ModelObjectPtrs &objects, FacetsMember member, EnforcerBlockerType state,
                     const Transform3d &instance_linear = Transform3d::Identity())
{
    PaintedOn out;
    const Transform3d back = instance_linear.inverse();
    for (const ModelObject *o : objects)
        for (const ModelVolume *v : o->volumes) {
            if (!v->is_model_part() || v->is_cut_connector())
                continue;
            const indexed_triangle_set its = (v->*member).get_facets(*v, state);
            out.area += cp_area(its);
            const Vec3d shift = v->mesh().get_init_shift();
            for (const Vec3f &p : its.vertices)
                out.original_pts.emplace_back(back * (p.cast<double>() + shift));
        }
    return out;
}

bool cp_all_on(const std::vector<Vec3d> &pts, int axis, double value, double tol = 1e-2)
{
    for (const Vec3d &p : pts)
        if (std::abs(p[axis] - value) > tol)
            return false;
    return true;
}

ModelObject *cp_make_cube(Model &model, const Geometry::Transformation &inst = Geometry::Transformation())
{
    ModelObject *mo = model.add_object();
    mo->name        = "cube";
    ModelVolume *v  = mo->add_volume(TriangleMesh(cp_centred_cube()));
    v->name         = "cube_v";
    ModelInstance *i = mo->add_instance();
    i->set_transformation(inst);
    return mo;
}

// Colour 2 on +X, colour 3 on the top.
void cp_paint_colour(ModelVolume *v)
{
    TriangleSelector sel(v->mesh());
    for (int i = 0; i < int(v->mesh().its.indices.size()); ++i) {
        const Vec3f n = its_face_normal(v->mesh().its, i);
        if (n.x() > 0.9f)
            sel.set_facet(i, EnforcerBlockerType::Extruder2);
        else if (n.z() > 0.9f)
            sel.set_facet(i, EnforcerBlockerType::Extruder3);
    }
    v->mmu_segmentation_facets.set_data(sel.serialize());
}

const ModelObjectCutAttributes CP_HALVES = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;

// The checks every path shares on a cube cut flat at z == 0 (or near it): the +X face
// (40 x 40) is shared between the halves, the top (40 x 40) is on the upper half only,
// nothing else is coloured, and every coloured point is on the face it was painted on.
void cp_check_colour(const ModelObjectPtrs &res, double side_area, const Transform3d &inst = Transform3d::Identity())
{
    const PaintedOn side = cp_painted(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2, inst);
    CHECK(side.area == Approx(side_area).epsilon(0.02));
    CHECK(cp_all_on(side.original_pts, 0, CP_H));

    const PaintedOn top = cp_painted(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3, inst);
    CHECK(top.area == Approx(CP_CUBE * CP_CUBE).epsilon(0.02));
    CHECK(cp_all_on(top.original_pts, 2, CP_H));
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Plane cut, colour.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: a plane cut keeps colour on both halves and leaves the cut faces plain", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    cp_paint_colour(mo->volumes.front());

    Cut cut(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);

    cp_check_colour(res, CP_CUBE * CP_CUBE);

    // Each half carries its own share of the +X face, not both shares.
    for (const ModelObject *half : res) {
        const PaintedOn side = cp_painted({ const_cast<ModelObject *>(half) }, &ModelVolume::mmu_segmentation_facets,
                                          EnforcerBlockerType::Extruder2);
        CHECK(side.area == Approx(0.5 * CP_CUBE * CP_CUBE).epsilon(0.02));
    }
    // Only the upper half has a top.
    int halves_with_top = 0;
    for (const ModelObject *half : res)
        if (cp_painted({ const_cast<ModelObject *>(half) }, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3).area > 1.)
            ++halves_with_top;
    CHECK(halves_with_top == 1);
}

// ---------------------------------------------------------------------------
// 2. Every channel: support (enforcer, blocker, stabilizer point), seam, fuzzy skin.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: support, stabilizer, seam and fuzzy-skin painting all survive", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    ModelVolume *v  = mo->volumes.front();
    {
        TriangleSelector sel(v->mesh());
        for (int i = 0; i < int(v->mesh().its.indices.size()); ++i) {
            const Vec3f n = its_face_normal(v->mesh().its, i);
            if (n.y() < -0.9f)
                sel.set_facet(i, EnforcerBlockerType::ENFORCER);
            else if (n.y() > 0.9f)
                sel.set_facet(i, EnforcerBlockerType::BLOCKER);
            else if (n.z() < -0.9f)
                sel.set_facet(i, EnforcerBlockerType::STABILIZER);
        }
        v->supported_facets.set_data(sel.serialize());
    }
    v->seam_facets.set_data(cp_paint_dir(v->mesh(), Vec3f::UnitX(), EnforcerBlockerType::ENFORCER));
    v->fuzzy_skin_facets.set_data(cp_paint_dir(v->mesh(), -Vec3f::UnitX(), EnforcerBlockerType::FUZZY_SKIN));

    Cut cut(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);

    const double side = CP_CUBE * CP_CUBE;
    const PaintedOn enf = cp_painted(res, &ModelVolume::supported_facets, EnforcerBlockerType::ENFORCER);
    CHECK(enf.area == Approx(side).epsilon(0.02));
    CHECK(cp_all_on(enf.original_pts, 1, -CP_H));

    const PaintedOn blk = cp_painted(res, &ModelVolume::supported_facets, EnforcerBlockerType::BLOCKER);
    CHECK(blk.area == Approx(side).epsilon(0.02));
    CHECK(cp_all_on(blk.original_pts, 1, CP_H));

    const PaintedOn stab = cp_painted(res, &ModelVolume::supported_facets, EnforcerBlockerType::STABILIZER);
    CHECK(stab.area == Approx(side).epsilon(0.02));
    CHECK(cp_all_on(stab.original_pts, 2, -CP_H));

    const PaintedOn seam = cp_painted(res, &ModelVolume::seam_facets, EnforcerBlockerType::ENFORCER);
    CHECK(seam.area == Approx(side).epsilon(0.02));
    CHECK(cp_all_on(seam.original_pts, 0, CP_H));

    const PaintedOn fuzzy = cp_painted(res, &ModelVolume::fuzzy_skin_facets, EnforcerBlockerType::FUZZY_SKIN);
    CHECK(fuzzy.area == Approx(side).epsilon(0.02));
    CHECK(cp_all_on(fuzzy.original_pts, 0, -CP_H));
}

// ---------------------------------------------------------------------------
// 3. A rotated, scaled instance, with Flip and Place on cut.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: a rotated, scaled instance keeps paint on the faces it was painted on", "[CutPaint]")
{
    Geometry::Transformation inst;
    inst.set_rotation(Vec3d(0., 0., Geometry::deg2rad(30.)));
    inst.set_scaling_factor(Vec3d(1.5, 1.5, 1.5));

    Model        model;
    ModelObject *mo = cp_make_cube(model, inst);
    cp_paint_colour(mo->volumes.front());
    const Transform3d linear = mo->instances.front()->get_transformation().get_matrix_no_offset();

    Cut cut(mo, 0, Transform3d::Identity(),
            CP_HALVES | ModelObjectCutAttribute::FlipUpper | ModelObjectCutAttribute::PlaceOnCutLower |
                ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);

    // In mesh units the faces are 1.5 x as wide.
    const double s2 = 1.5 * 1.5;
    const PaintedOn side = cp_painted(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2, linear);
    CHECK(side.area == Approx(s2 * CP_CUBE * CP_CUBE).epsilon(0.02));
    CHECK(cp_all_on(side.original_pts, 0, CP_H));
    const PaintedOn top = cp_painted(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3, linear);
    CHECK(top.area == Approx(s2 * CP_CUBE * CP_CUBE).epsilon(0.02));
    CHECK(cp_all_on(top.original_pts, 2, CP_H));
}

// ---------------------------------------------------------------------------
// 4. A kerf: the removed band has no source faces, so it stays plain.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: with a cut thickness the halves keep paint and the band faces are plain", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    cp_paint_colour(mo->volumes.front());

    Cut cut(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane(2.0);
    REQUIRE(res.size() == 2);

    // The +X face loses the 2 mm band: 40 x 38.
    cp_check_colour(res, CP_CUBE * (CP_CUBE - 2.0));
}

// ---------------------------------------------------------------------------
// 5. The Edge-only surfaces: curved sheet, drawn stroke, flexi joint.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: a curved cut keeps paint on both sides of the sheet", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    cp_paint_colour(mo->volumes.front());

    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    sheet.at(2, 2) = 5.0;
    REQUIRE(!sheet.is_flat());

    Cut cut(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_curved_sheet(sheet);
    REQUIRE(res.size() == 2);

    // The sheet bends the cut line across +X, but the two halves still partition the face.
    cp_check_colour(res, CP_CUBE * CP_CUBE);
}

TEST_CASE("Cut paint: a drawn loop and a drawn open line keep paint", "[CutPaint]")
{
    auto run = [](DrawCutStroke stroke, double expected_side) {
        Model        model;
        ModelObject *mo = cp_make_cube(model);
        cp_paint_colour(mo->volumes.front());

        REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
        REQUIRE(stroke.valid());
        DrawCutParams params;
        params.direction   = DrawCutDirection::SurfaceNormal;
        params.extension   = 5.0;
        params.through_all = true;

        Cut cut(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
        const ModelObjectPtrs &res = cut.perform_with_draw_stroke(stroke, params);
        REQUIRE(res.size() == 2);

        const PaintedOn side = cp_painted(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2);
        CHECK(side.area == Approx(expected_side).epsilon(0.03));
        CHECK(cp_all_on(side.original_pts, 0, CP_H));
        // The top, minus nothing: a loop's plug takes a disc of it, the rest keeps the remainder.
        const PaintedOn top = cp_painted(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3);
        CHECK(top.area == Approx(CP_CUBE * CP_CUBE).epsilon(0.03));
        CHECK(cp_all_on(top.original_pts, 2, CP_H));
    };

    SECTION("closed loop on the top: the plug keeps its disc of the top, the rest keeps the sides") {
        DrawCutStroke loop;
        const int     n = 96;
        for (int i = 0; i < n; ++i) {
            const double a = 2.0 * M_PI * double(i) / double(n);
            loop.append(Vec3d(12.0 * std::cos(a), 12.0 * std::sin(a), CP_H), Vec3d::UnitZ(), size_t(i));
        }
        loop.append(Vec3d(12.0, 0.0, CP_H), Vec3d::UnitZ(), 0);
        run(loop, CP_CUBE * CP_CUBE);
    }
    SECTION("open line across the top") {
        DrawCutStroke line;
        const int     n = 40;
        for (int i = 0; i < n; ++i) {
            const double t = double(i) / double(n - 1);
            line.append(Vec3d(-CP_H + 2.0 * CP_H * t, 0.0, CP_H), Vec3d::UnitZ(), size_t(i));
        }
        run(line, CP_CUBE * CP_CUBE);
    }
}

TEST_CASE("Cut paint: a flexi joint cut keeps paint and leaves the joint bodies plain", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    cp_paint_colour(mo->volumes.front());

    FlexiJointParams p;
    p.kind         = FlexiJointKind::DoubleRing;
    p.clearance    = 0.35f;
    p.outer_radius = 6.0f;
    p.ring_width   = 2.0f;
    p.ring_height  = 1.5f;
    p.tilt         = 0.20f;
    p.neck_ratio   = 0.45f;
    p.hub_radius   = flexi_default_hub_radius(p);

    CutConnector connector;
    connector.pos        = Vec3d::Zero();
    connector.rotation_m = Transform3d::Identity();
    connector.z_angle    = 0.f;
    connector.radius     = flexi_outer_extent(p);
    connector.height     = flexi_protrusion_height(p);
    connector.attribs    = CutConnectorAttributes(CutConnectorType::FlexiJoint, CutConnectorStyle::Prism, CutConnectorShape::Circle);
    connector.flexi      = p;
    add_flexi_joint_volume(mo, connector, "Flexi joint-1");

    Cut cut(mo, 0, Transform3d::Identity(),
            CP_HALVES | ModelObjectCutAttribute::KeepAsParts | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 1);
    REQUIRE(res.front()->volumes.size() >= 2);

    // The joint opens its own gap between the faces; the +X face loses that band.
    const double gap = double(flexi_effective_gap(p));
    cp_check_colour(res, CP_CUBE * (CP_CUBE - gap));
}

// ---------------------------------------------------------------------------
// 6. Connectors: plug and dowel stay plain. Contour (part selection) cut.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: plug and dowel connectors stay unpainted", "[CutPaint]")
{
    using namespace Geometry;
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    // Paint the whole skin so a connector that wrongly took paint would have some to take.
    {
        ModelVolume     *v = mo->volumes.front();
        TriangleSelector sel(v->mesh());
        for (int i = 0; i < int(v->mesh().its.indices.size()); ++i)
            sel.set_facet(i, EnforcerBlockerType::Extruder2);
        v->mmu_segmentation_facets.set_data(sel.serialize());
    }
    auto add_connector = [mo](CutConnectorType type, const Vec3d &pos, const std::string &name) {
        ModelVolume *c = mo->add_volume(TriangleMesh(its_make_cylinder(1.0, 1.0, 2 * PI / 90.)), ModelVolumeType::NEGATIVE_VOLUME);
        c->set_transformation(translation_transform(pos) * scale_transform(Vec3d(4.0, 4.0, 10.0)));
        c->cut_info = ModelVolume::CutInfo(type, 0.f, 0.1f);
        c->name     = name;
    };
    add_connector(CutConnectorType::Plug, Vec3d(-8.0, 0.0, -5.0), "plug");
    add_connector(CutConnectorType::Dowel, Vec3d(8.0, 0.0, -5.0), "dowel");

    Cut cut(mo, 0, Transform3d::Identity(),
            CP_HALVES | ModelObjectCutAttribute::CreateDowels | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 3); // two halves and the dowel

    int connectors = 0;
    for (const ModelObject *o : res)
        for (const ModelVolume *v : o->volumes) {
            if (v->is_cut_connector()) {
                ++connectors;
                CHECK_FALSE(v->is_any_painted());
            } else if (v->is_model_part()) {
                CHECK(v->is_mm_painted());
            }
        }
    CHECK(connectors >= 2);
}

TEST_CASE("Cut paint: a contour (part selection) cut takes its paint from the unsplit object", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    cp_paint_colour(mo->volumes.front());

    // What PartSelection does: a keep-as-parts cut with no paint, giving the pieces to assign.
    Cut pre(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepAsParts);
    const ModelObjectPtrs &pieces = pre.perform_with_plane();
    REQUIRE(pieces.size() == 1);
    ModelObject *pieces_obj = pieces.front();
    REQUIRE(pieces_obj->volumes.size() == 2);
    for (const ModelVolume *v : pieces_obj->volumes)
        REQUIRE_FALSE(v->is_any_painted());

    std::vector<Cut::Part> parts;
    for (const ModelVolume *v : pieces_obj->volumes)
        parts.push_back({ v->is_from_upper(), false });

    Cut cut(pieces_obj, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_by_contour(mo, parts, 0);
    REQUIRE(res.size() == 2);
    cp_check_colour(res, CP_CUBE * CP_CUBE);
}

// ---------------------------------------------------------------------------
// 7. Tongue and groove (single dovetail).
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: a dovetail cut keeps paint on both halves", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    cp_paint_colour(mo->volumes.front());

    Cut::Groove g;
    g.depth       = 5.f;
    g.width       = 10.f;
    g.flaps_angle = float(Geometry::deg2rad(60.));
    g.angle       = 0.f;

    Cut cut(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_groove(g, Transform3d::Identity());
    REQUIRE(res.size() == 2);

    // The dovetail's profile shows on the +X face, but the halves still partition it.
    const PaintedOn side = cp_painted(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2);
    // The tolerance gaps take a sliver of the face.
    CHECK(side.area == Approx(CP_CUBE * CP_CUBE).epsilon(0.05));
    CHECK(cp_all_on(side.original_pts, 0, CP_H));
    const PaintedOn top = cp_painted(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3);
    CHECK(top.area == Approx(CP_CUBE * CP_CUBE).epsilon(0.02));
    CHECK(cp_all_on(top.original_pts, 2, CP_H));
}

// ---------------------------------------------------------------------------
// 8. Without KeepPaint the cut is what it always was.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: without KeepPaint the halves are unpainted and the geometry is unchanged", "[CutPaint]")
{
    auto cut_meshes = [](bool keep_paint, bool &painted) {
        Model        model;
        ModelObject *mo = cp_make_cube(model);
        cp_paint_colour(mo->volumes.front());
        ModelObjectCutAttributes attrs = CP_HALVES;
        if (keep_paint)
            attrs = attrs | ModelObjectCutAttribute::KeepPaint;
        Cut cut(mo, 0, Transform3d::Identity(), attrs);
        const ModelObjectPtrs &res = cut.perform_with_plane();
        std::vector<indexed_triangle_set> out;
        painted = false;
        for (const ModelObject *o : res)
            for (const ModelVolume *v : o->volumes) {
                out.push_back(v->mesh().its);
                painted |= v->is_any_painted();
            }
        return out;
    };
    bool       painted_off = true, painted_on = false;
    const auto off         = cut_meshes(false, painted_off);
    const auto on          = cut_meshes(true, painted_on);
    CHECK_FALSE(painted_off);
    CHECK(painted_on);
    REQUIRE(off.size() == on.size());
    for (size_t i = 0; i < off.size(); ++i) {
        CHECK(off[i].vertices == on[i].vertices);
        CHECK(off[i].indices == on[i].indices);
    }
}

// ---------------------------------------------------------------------------
// 9. MixedFilament: a virtual filament ID beyond the physical ones survives.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: a mixed-filament colour state survives the cut", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    ModelVolume *v  = mo->volumes.front();
    const auto   mixed = EnforcerBlockerType(20);
    v->mmu_segmentation_facets.set_data(cp_paint_dir(v->mesh(), Vec3f::UnitX(), mixed));

    Cut cut(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);
    const PaintedOn side = cp_painted(res, &ModelVolume::mmu_segmentation_facets, mixed);
    CHECK(side.area == Approx(CP_CUBE * CP_CUBE).epsilon(0.02));
    CHECK(cp_all_on(side.original_pts, 0, CP_H));
}

// ---------------------------------------------------------------------------
// 10. 3MF round trip of the painted halves.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: painted halves survive a project 3MF save and reload", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    cp_paint_colour(mo->volumes.front());

    Cut cut(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);

    Model src;
    for (const ModelObject *o : res)
        src.add_object(*o);
    double side_before = 0.;
    for (const ModelObject *o : src.objects)
        for (const ModelVolume *v : o->volumes)
            side_before += cp_area(v->mmu_segmentation_facets.get_facets(*v, EnforcerBlockerType::Extruder2));
    REQUIRE(side_before == Approx(CP_CUBE * CP_CUBE).epsilon(0.02));

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string file = (tmp_root / "cut_paint_round_trip.3mf").string();

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    StoreParams        store_params;
    store_params.path     = file.c_str();
    store_params.model    = &src;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    Model                     dst;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(file.c_str(), &dst_config, &ctxt, &dst, &plate_data, &project_presets, &is_bbl_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    release_PlateData_list(plate_data);

    REQUIRE(dst.objects.size() == 2);
    double side_after = 0., top_after = 0.;
    for (const ModelObject *o : dst.objects)
        for (const ModelVolume *v : o->volumes) {
            side_after += cp_area(v->mmu_segmentation_facets.get_facets(*v, EnforcerBlockerType::Extruder2));
            top_after += cp_area(v->mmu_segmentation_facets.get_facets(*v, EnforcerBlockerType::Extruder3));
        }
    CHECK(side_after == Approx(side_before).epsilon(1e-3));
    CHECK(top_after == Approx(CP_CUBE * CP_CUBE).epsilon(0.02));
    boost::filesystem::remove(file);
}

// ---------------------------------------------------------------------------
// 11. Texture displacement is NOT carried (owner decision D3, 2026-10-24: a
// separate follow-up). Pinned so a change to it is deliberate.
// ---------------------------------------------------------------------------

TEST_CASE("Cut paint: texture-displacement masks are still dropped by a cut", "[CutPaint]")
{
    Model        model;
    ModelObject *mo = cp_make_cube(model);
    ModelVolume *v  = mo->volumes.front();
    cp_paint_colour(v);
    v->texture_displacement_facet(0).set_data(cp_paint_dir(v->mesh(), Vec3f::UnitZ(), EnforcerBlockerType::ENFORCER));
    REQUIRE_FALSE(v->texture_displacement_facet(0).empty());

    Cut cut(mo, 0, Transform3d::Identity(), CP_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);
    for (const ModelObject *o : res)
        for (const ModelVolume *rv : o->volumes) {
            CHECK(rv->texture_displacement_facet(0).empty());
            CHECK(rv->texture_displacement_layers.empty());
        }
    // ... while the colour came through.
    cp_check_colour(res, CP_CUBE * CP_CUBE);
}
