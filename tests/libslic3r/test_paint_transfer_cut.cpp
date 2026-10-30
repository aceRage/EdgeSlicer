// PaintTransfer through a plane cut (NeotkoCM 8f5eae374): colour, support, seam
// and fuzzy-skin facets survive on every piece. Uncut facets are copied as they
// are (FacetMatcher); the cap of the cut stays unpainted.
//
// Curved / drawn / flexi / groove still use the spatial remap in Cut::finalize
// (see test_cut_paint.cpp). This file covers the FacetMatcher path.

#include <catch2/catch.hpp>

#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PaintTransfer.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <cmath>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

const double PTC_CUBE = 40.0;
const double PTC_H    = 0.5 * PTC_CUBE;

indexed_triangle_set ptc_centred_cube(double s = PTC_CUBE)
{
    indexed_triangle_set its = its_make_cube(s, s, s);
    for (Vec3f &v : its.vertices)
        v -= Vec3f(float(0.5 * s), float(0.5 * s), float(0.5 * s));
    return its;
}

double ptc_area(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const Vec3i32 &f : its.indices)
        a += 0.5 * double((its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).norm());
    return a;
}

using FacetsMember = FacetsAnnotation ModelVolume::*;

double ptc_painted_area(const ModelObjectPtrs &objects, FacetsMember member, EnforcerBlockerType state)
{
    double area = 0.;
    for (const ModelObject *o : objects)
        for (const ModelVolume *v : o->volumes) {
            if (!v->is_model_part() || v->is_cut_connector())
                continue;
            area += ptc_area((v->*member).get_facets(*v, state));
        }
    return area;
}

ModelObject *ptc_make_cube(Model &model)
{
    ModelObject *mo = model.add_object();
    mo->name        = "cube";
    ModelVolume *v  = mo->add_volume(TriangleMesh(ptc_centred_cube()));
    v->name         = "cube_v";
    mo->add_instance();
    return mo;
}

void ptc_paint_all_channels(ModelVolume *v)
{
    const TriangleMesh &mesh = v->mesh();
    {
        TriangleSelector sel(mesh);
        for (int i = 0; i < int(mesh.its.indices.size()); ++i) {
            const Vec3f n = its_face_normal(mesh.its, i);
            if (n.x() > 0.9f)
                sel.set_facet(i, EnforcerBlockerType::Extruder2);
            else if (n.z() > 0.9f)
                sel.set_facet(i, EnforcerBlockerType::Extruder3);
        }
        v->mmu_segmentation_facets.set_data(sel.serialize());
    }
    {
        TriangleSelector sel(mesh);
        for (int i = 0; i < int(mesh.its.indices.size()); ++i)
            if (its_face_normal(mesh.its, i).y() < -0.9f)
                sel.set_facet(i, EnforcerBlockerType::ENFORCER);
        v->supported_facets.set_data(sel.serialize());
    }
    {
        TriangleSelector sel(mesh);
        for (int i = 0; i < int(mesh.its.indices.size()); ++i)
            if (its_face_normal(mesh.its, i).x() > 0.9f)
                sel.set_facet(i, EnforcerBlockerType::ENFORCER);
        v->seam_facets.set_data(sel.serialize());
    }
    {
        TriangleSelector sel(mesh);
        for (int i = 0; i < int(mesh.its.indices.size()); ++i)
            if (its_face_normal(mesh.its, i).x() < -0.9f)
                sel.set_facet(i, EnforcerBlockerType::FUZZY_SKIN);
        v->fuzzy_skin_facets.set_data(sel.serialize());
    }
}

const ModelObjectCutAttributes PTC_HALVES = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;

} // namespace

TEST_CASE("FacetMatcher matches uncut facets by the same corners", "[PaintTransfer][CutPaint]")
{
    indexed_triangle_set src = its_make_cube(10., 10., 10.);
    indexed_triangle_set piece = src;
    // Drop the last triangle: the rest stay bit-identical.
    REQUIRE(piece.indices.size() > 2);
    piece.indices.pop_back();

    const PaintTransfer::FacetMatcher matcher(src);
    const std::vector<int>            map = matcher.match(piece);
    REQUIRE(map.size() == piece.indices.size());
    size_t matched = 0;
    for (int src_facet : map)
        if (src_facet >= 0)
            ++matched;
    CHECK(matched == piece.indices.size());
}

TEST_CASE("A plane cut keeps colour, support, seam and fuzzy-skin facets on every piece", "[PaintTransfer][CutPaint]")
{
    Model        model;
    ModelObject *mo = ptc_make_cube(model);
    ptc_paint_all_channels(mo->volumes.front());
    REQUIRE(PaintTransfer::has_painting(*mo->volumes.front()));

    Cut cut(mo, 0, Transform3d::Identity(), PTC_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);

    const double face = PTC_CUBE * PTC_CUBE;
    REQUIRE_THAT(ptc_painted_area(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2),
                 WithinAbs(face, 32.0));
    REQUIRE_THAT(ptc_painted_area(res, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3),
                 WithinAbs(face, 32.0));
    REQUIRE_THAT(ptc_painted_area(res, &ModelVolume::supported_facets, EnforcerBlockerType::ENFORCER),
                 WithinAbs(face, 32.0));
    REQUIRE_THAT(ptc_painted_area(res, &ModelVolume::seam_facets, EnforcerBlockerType::ENFORCER),
                 WithinAbs(face, 32.0));
    REQUIRE_THAT(ptc_painted_area(res, &ModelVolume::fuzzy_skin_facets, EnforcerBlockerType::FUZZY_SKIN),
                 WithinAbs(face, 32.0));

    // Each half keeps its share of the painted +X face; the cap of the cut is not coloured.
    int halves_with_colour = 0;
    for (const ModelObject *half : res) {
        const double side = ptc_painted_area({ const_cast<ModelObject *>(half) }, &ModelVolume::mmu_segmentation_facets,
                                             EnforcerBlockerType::Extruder2);
        if (side > 1.)
            ++halves_with_colour;
        REQUIRE_THAT(side, WithinAbs(0.5 * face, 40.0));
    }
    CHECK(halves_with_colour == 2);
}

TEST_CASE("Without KeepPaint a painted cut drops every FacetsAnnotation layer", "[PaintTransfer][CutPaint]")
{
    Model        model;
    ModelObject *mo = ptc_make_cube(model);
    ptc_paint_all_channels(mo->volumes.front());

    Cut cut(mo, 0, Transform3d::Identity(), PTC_HALVES);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);
    for (const ModelObject *half : res)
        for (const ModelVolume *v : half->volumes)
            CHECK_FALSE(PaintTransfer::has_painting(*v));
}

TEST_CASE("A rotated plane cut with a kerf paints each half by PaintTransfer alone", "[PaintTransfer][CutPaint][CutPaintKerf]")
{
    // perform_with_plane() no longer saves paintings for the remap in finalize(), so every
    // painted facet below must come from PaintTransfer. A no-op transfer leaves both halves bare.
    Model        model;
    ModelObject *mo = ptc_make_cube(model);
    ModelVolume *v  = mo->volumes.front();
    {
        const TriangleMesh &mesh = v->mesh();
        TriangleSelector    sel(mesh);
        for (int i = 0; i < int(mesh.its.indices.size()); ++i) {
            const Vec3f n = its_face_normal(mesh.its, i);
            if (n.x() > 0.9f)
                sel.set_facet(i, EnforcerBlockerType::Extruder2); // stays vertical: split by the cut
            else if (n.y() > 0.9f)
                sel.set_facet(i, EnforcerBlockerType::Extruder3); // becomes the top after the X turn
        }
        v->mmu_segmentation_facets.set_data(sel.serialize());
    }
    // Local +Y turns to world +Z (X by 90 deg), then a 30 deg Z turn.
    mo->instances.front()->set_rotation(Vec3d(0.5 * PI, 0., PI / 6.));

    const double kerf = 2.0;
    Cut cut(mo, 0, Transform3d::Identity(), PTC_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane(kerf);
    REQUIRE(res.size() == 2);

    // Each half keeps (H - kerf/2) x CUBE of the +X face; the kerf band and the caps are bare.
    const double side = (PTC_H - 0.5 * kerf) * PTC_CUBE;
    double top_total = 0.;
    for (const ModelObject *half : res) {
        const ModelObjectPtrs one{ const_cast<ModelObject *>(half) };
        CHECK_THAT(ptc_painted_area(one, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2),
                   WithinAbs(side, 2.0));
        top_total += ptc_painted_area(one, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3);
    }
    // The painted top lies whole on one half.
    CHECK_THAT(top_total, WithinAbs(PTC_CUBE * PTC_CUBE, 2.0));
    const double top0 = ptc_painted_area({ res[0] }, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3);
    CHECK((top0 < 1.0 || top0 > PTC_CUBE * PTC_CUBE - 2.0));
}