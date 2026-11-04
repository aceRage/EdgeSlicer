// ---------------------------------------------------------------------------
// OD11 / OrcaSlicer #12318 (multiple dovetail cuts, merge 15451c6c50) and #13945
// (merge 3cce9b09ed): Cut::perform_with_groove() with a count and a gap, and
// Cut::calculate_groove_width(), the spacing the gizmo preview and the cut share.
// Edge additions covered here: the count clamp, and painting through a
// multi-dovetail cut (OD11 PR A). The recipe side (v5 count / gap / radius) is
// in test_cut_recipe.cpp.
// ---------------------------------------------------------------------------

#include <catch2/catch.hpp>

#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <cmath>
#include <vector>

using namespace Slic3r;

namespace {

// A 100 x 20 x 20 bar centred on the origin: long along X, the axis the dovetails are laid out on.
indexed_triangle_set cg_bar()
{
    indexed_triangle_set its = its_make_cube(100., 20., 20.);
    for (Vec3f &v : its.vertices)
        v -= Vec3f(50.f, 10.f, 10.f);
    return its;
}

Cut::Groove cg_groove()
{
    Cut::Groove g;
    g.depth       = 5.f;
    g.width       = 10.f;
    g.flaps_angle = float(Geometry::deg2rad(60.));
    g.angle       = 0.f;
    return g;
}

double cg_signed_volume(const indexed_triangle_set &its)
{
    double v = 0.;
    for (const Vec3i32 &f : its.indices) {
        const Vec3d a = its.vertices[f[0]].cast<double>(), b = its.vertices[f[1]].cast<double>(), c = its.vertices[f[2]].cast<double>();
        v += a.dot(b.cross(c));
    }
    return v / 6.;
}

// Every model part of an object, in the object's frame, as one mesh.
indexed_triangle_set cg_object_mesh(const ModelObject *o)
{
    TriangleMesh out;
    for (const ModelVolume *v : o->volumes)
        if (v->is_model_part()) {
            TriangleMesh m = v->mesh();
            m.transform(v->get_matrix());
            out.merge(m);
        }
    return out.its;
}

struct Halves
{
    indexed_triangle_set upper, lower;
};

Halves cg_cut(int count, float gap)
{
    Model        model;
    ModelObject *mo = model.add_object();
    mo->add_volume(TriangleMesh(cg_bar()))->name = "bar";
    mo->add_instance();

    Cut cut(mo, 0, Transform3d::Identity(), ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs &res = cut.perform_with_groove(cg_groove(), Transform3d::Identity(), count, gap, 30.f);
    REQUIRE(res.size() == 2);

    Halves h;
    indexed_triangle_set a = cg_object_mesh(res[0]), b = cg_object_mesh(res[1]);
    const float          a_top = bounding_box(a).max.z(), b_top = bounding_box(b).max.z();
    h.upper = a_top >= b_top ? a : b;
    h.lower = a_top >= b_top ? b : a;
    return h;
}

} // namespace

TEST_CASE("Dovetail: calculate_groove_width matches the hand-worked width", "[CutGroove]")
{
    Cut::Groove g = cg_groove();
    // Flap 60 deg, depth 5: each flap leans out depth / tan(60) = 2.8868, so the mouth is
    // 10 + 2 x 2.8868 = 15.7735 wide. With no groove angle the radius does not matter.
    CHECK(Cut::calculate_groove_width(g, 30.f) == Approx(15.7735).epsilon(1e-4));
    CHECK(Cut::calculate_groove_width(g, 80.f) == Approx(15.7735).epsilon(1e-4));

    // A 10 deg groove angle tapers the slot over the preview plane's half height,
    // 0.5 x 1.5 x 1.5 x radius = 33.75 at radius 30: 2 x 33.75 x tan(10 deg) = 11.902 wider.
    g.angle = float(Geometry::deg2rad(10.));
    CHECK(Cut::calculate_groove_width(g, 30.f) == Approx(15.7735 + 11.9021).epsilon(1e-4));
}

TEST_CASE("Dovetail: one dovetail does not depend on the gap", "[CutGroove]")
{
    const Halves a = cg_cut(1, 5.f);
    const Halves b = cg_cut(1, 50.f);
    CHECK(a.upper.vertices == b.upper.vertices);
    CHECK(a.upper.indices == b.upper.indices);
    CHECK(a.lower.vertices == b.lower.vertices);
    CHECK(a.lower.indices == b.lower.indices);

    // The upper half (the tongue side) is closed. The lower half is assembled from several cut
    // pieces and is not a single closed mesh even for one dovetail - that is the cut as it was
    // before #12318 (count 1 runs the same cuts), so it is not asserted here.
    CHECK(its_num_open_edges(a.upper) == 0);
}

TEST_CASE("Dovetail: every extra dovetail moves the same volume, and the total is kept", "[CutGroove]")
{
    // f(n) = upper half's volume with n dovetails. Each dovetail moves one identical
    // tongue's worth of material from one half to the other (no groove angle, every
    // dovetail sits fully inside the 100 mm bar), so f is linear in n with a non-zero step.
    double f[4] = { 0., 0., 0., 0. };
    for (int n = 1; n <= 3; ++n) {
        const Halves h = cg_cut(n, 10.f);
        INFO("dovetails: " << n);
        CHECK(its_num_open_edges(h.upper) == 0);
        f[n] = cg_signed_volume(h.upper);
        // Only the tolerance clearances (0.1 mm deep and wide) are taken away.
        const double bar = 100. * 20. * 20.;
        CHECK(f[n] + cg_signed_volume(h.lower) == Approx(bar).epsilon(0.01));
    }
    const double step1 = f[2] - f[1];
    const double step2 = f[3] - f[2];
    // A tongue is ~ (10 + 15.8) / 2 x 5 mm in section over the 20 mm bar: ~1300 mm3.
    CHECK(std::abs(step1) > 500.);
    CHECK(step2 == Approx(step1).epsilon(0.02));
}

TEST_CASE("Dovetail: a count below one cuts one dovetail", "[CutGroove]")
{
    const Halves zero = cg_cut(0, 10.f);
    const Halves one  = cg_cut(1, 10.f);
    CHECK(zero.upper.vertices == one.upper.vertices);
    CHECK(zero.lower.indices == one.lower.indices);
}

TEST_CASE("Dovetail: painting survives a three-dovetail cut", "[CutGroove][CutPaint]")
{
    Model        model;
    ModelObject *src = nullptr;
    // Paint the +Y face, where the dovetails' profile shows, before cutting.
    {
        ModelObject *mo = model.add_object();
        mo->add_volume(TriangleMesh(cg_bar()))->name = "bar";
        mo->add_instance();
        src             = mo;
        ModelVolume     *v = mo->volumes.front();
        TriangleSelector sel(v->mesh());
        for (int i = 0; i < int(v->mesh().its.indices.size()); ++i)
            if (its_face_normal(v->mesh().its, i).y() > 0.9f)
                sel.set_facet(i, EnforcerBlockerType::Extruder2);
        v->mmu_segmentation_facets.set_data(sel.serialize());
    }

    Cut cut(src, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_groove(cg_groove(), Transform3d::Identity(), 3, 10.f, 30.f);
    REQUIRE(res.size() == 2);

    double area = 0.;
    for (const ModelObject *o : res)
        for (const ModelVolume *v : o->volumes) {
            if (!v->is_model_part())
                continue;
            const indexed_triangle_set its = v->mmu_segmentation_facets.get_facets(*v, EnforcerBlockerType::Extruder2);
            for (const Vec3i32 &f : its.indices)
                area += 0.5 * double((its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).norm());
        }
    // 100 x 20, less the tolerance slivers of three dovetails.
    CHECK(area == Approx(100. * 20.).epsilon(0.03));
}
