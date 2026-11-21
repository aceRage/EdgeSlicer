// ---------------------------------------------------------------------------
// Keep paint v2 (NeotkoCM 8f5eae374, re-ported from PR #776): a plane cut paints
// each piece of a model part by exact facet provenance. A facet the plane does
// not cross keeps its painting bit for bit, the cap stays plain, and only the
// facets the plane split are resampled from the source (PaintTransfer).
//
// How "bit for bit" is checked without trusting the code under test: a piece's
// mesh plus its init shift is the halves' frame (the cut instance without its
// offset, times the volume matrix). Each piece facet is matched to a source
// facet by its three corners, in order, in that frame (brute force, its own
// code), and the painting tree of the two facets is compared bit for bit.
//
// The spatial remap (test_cut_paint.cpp, [CutPaint]) still covers what stays on
// it: curved, drawn, flexi, contour and dovetail cuts, an airbrushed or
// mirrored part, applied TAC joints, and Edit cut's carry back.
// ---------------------------------------------------------------------------

#include <catch2/catch.hpp>

#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PaintTransfer.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <sstream>
#include <vector>

using namespace Slic3r;

namespace {

using FacetsMember = FacetsAnnotation ModelVolume::*;
using StateAt      = std::function<EnforcerBlockerType(const Vec3f &)>;

const ModelObjectCutAttributes PT_HALVES = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;

struct PaintKind
{
    const char         *name;
    FacetsMember        member;
    EnforcerBlockerType state;
};

// Every paint kind and state the fixture paints (see pt_paint_all()).
const PaintKind PT_KINDS[] = {
    { "colour 2", &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2 },
    { "colour 3", &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3 },
    { "support enforcer", &ModelVolume::supported_facets, EnforcerBlockerType::ENFORCER },
    { "support blocker", &ModelVolume::supported_facets, EnforcerBlockerType::BLOCKER },
    { "seam", &ModelVolume::seam_facets, EnforcerBlockerType::ENFORCER },
    { "fuzzy skin", &ModelVolume::fuzzy_skin_facets, EnforcerBlockerType::FUZZY_SKIN },
};
const FacetsMember PT_LAYERS[] = { &ModelVolume::mmu_segmentation_facets, &ModelVolume::supported_facets, &ModelVolume::seam_facets,
                                   &ModelVolume::fuzzy_skin_facets };

double pt_area(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const Vec3i32 &f : its.indices)
        a += 0.5 * double((its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).norm());
    return a;
}

// Paints one layer from a function of the mesh point: strokes that run across facets, so the
// fixture has split trees and not only whole facets.
void pt_paint(ModelVolume &v, FacetsMember member, const StateAt &at, float edge_limit = 0.25f)
{
    TriangleSelector sel(v.mesh());
    sel.resample([&](const Vec3f &p) {
        TriangleSelector::SampledState s;
        s.state = at(p);
        return s;
    }, edge_limit);
    (v.*member).set(sel);
}

// All four paint kinds, with borders that do not follow the facets.
void pt_paint_all(ModelVolume &v)
{
    pt_paint(v, &ModelVolume::mmu_segmentation_facets, [](const Vec3f &p) {
        return p.x() > 4.3f ? EnforcerBlockerType::Extruder2 : p.z() > 9.7f ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::NONE;
    });
    pt_paint(v, &ModelVolume::supported_facets, [](const Vec3f &p) {
        return p.y() < -6.1f ? EnforcerBlockerType::ENFORCER : p.y() > 13.3f ? EnforcerBlockerType::BLOCKER : EnforcerBlockerType::NONE;
    });
    pt_paint(v, &ModelVolume::seam_facets, [](const Vec3f &p) {
        return std::abs(p.x() + 2.2f) < 1.5f ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
    });
    pt_paint(v, &ModelVolume::fuzzy_skin_facets, [](const Vec3f &p) {
        return p.x() < -11.1f ? EnforcerBlockerType::FUZZY_SKIN : EnforcerBlockerType::NONE;
    });
}

ModelObject *pt_make_object(Model &model, const indexed_triangle_set &its, const Geometry::Transformation &inst = Geometry::Transformation())
{
    ModelObject *mo = model.add_object();
    mo->name        = "painted";
    ModelVolume *v  = mo->add_volume(TriangleMesh(its));
    v->name         = "painted_v";
    mo->add_instance()->set_transformation(inst);
    return mo;
}

indexed_triangle_set pt_sphere() { return its_make_sphere(20., 2. * PI / 40.); }

// The painting tree of every facet: facet -> its bits (empty: no painting).
std::vector<std::vector<bool>> pt_trees(const TriangleSelector::TriangleSplittingData &data, size_t num_facets)
{
    std::vector<TriangleSelector::TriangleBitStreamMapping> by_start = data.triangles_to_split;
    std::sort(by_start.begin(), by_start.end(), [](const auto &l, const auto &r) { return l.bitstream_start_idx < r.bitstream_start_idx; });
    std::vector<std::vector<bool>> out(num_facets);
    for (size_t i = 0; i < by_start.size(); ++i) {
        const int b = by_start[i].bitstream_start_idx;
        const int e = i + 1 < by_start.size() ? by_start[i + 1].bitstream_start_idx : int(data.bitstream.size());
        REQUIRE(size_t(by_start[i].triangle_idx) < num_facets);
        out[size_t(by_start[i].triangle_idx)].assign(data.bitstream.begin() + b, data.bitstream.begin() + e);
    }
    return out;
}

// A facet's corners in the halves' frame.
using Corners = std::array<Vec3d, 3>;
std::vector<Corners> pt_corners(const indexed_triangle_set &its, const Transform3d &t)
{
    std::vector<Corners> out(its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i)
        for (int c = 0; c < 3; ++c)
            out[i][c] = t * its.vertices[its.indices[i][c]].cast<double>();
    return out;
}

// For each piece facet, the source facet with the same three corners in the same order, else -1.
std::vector<int> pt_match(const std::vector<Corners> &piece, const std::vector<Corners> &source, double tol = 1e-4)
{
    std::vector<int> map(piece.size(), -1);
    for (size_t i = 0; i < piece.size(); ++i)
        for (size_t j = 0; j < source.size(); ++j)
            if ((piece[i][0] - source[j][0]).norm() < tol && (piece[i][1] - source[j][1]).norm() < tol &&
                (piece[i][2] - source[j][2]).norm() < tol) {
                map[i] = int(j);
                break;
            }
    return map;
}

double pt_painted_area(const ModelVolume &v, const PaintKind &k) { return pt_area((v.*k.member).get_facets(v, k.state)); }

double pt_painted_area(const ModelObjectPtrs &objects, const PaintKind &k)
{
    double a = 0.;
    for (const ModelObject *o : objects)
        for (const ModelVolume *v : o->volumes)
            if (v->is_model_part() && !v->is_cut_connector())
                a += pt_painted_area(*v, k);
    return a;
}

struct ExactCheck
{
    size_t matched = 0, cap = 0, split = 0;
};

// The core check: every piece facet matched to a source facet carries the same painting tree, in
// every layer; every cap facet (all corners on the plane z == cap_z, in the halves' frame) is plain.
// A facet with a corner on the plane is left out of the strict check: cut_mesh() may snap that
// corner onto the plane by a hair (#1001), so the code rightly resamples it while this file's
// tolerant match would still pair it.
ExactCheck pt_check_exact(const ModelObjectPtrs &res, const ModelVolume &src, const Transform3d &src_to_halves,
                          const std::vector<double> &cap_z)
{
    auto on_plane = [&cap_z](const Vec3d &p) {
        return std::any_of(cap_z.begin(), cap_z.end(), [&p](double z) { return std::abs(p.z() - z) < 1e-3; });
    };
    auto on_cap = [&cap_z](const Corners &c) {
        return std::any_of(cap_z.begin(), cap_z.end(), [&c](double z) {
            return std::abs(c[0].z() - z) < 1e-3 && std::abs(c[1].z() - z) < 1e-3 && std::abs(c[2].z() - z) < 1e-3;
        });
    };
    ExactCheck                 out;
    const std::vector<Corners> src_corners = pt_corners(src.mesh().its, src_to_halves);
    for (const ModelObject *o : res)
        for (const ModelVolume *v : o->volumes) {
            if (!v->is_model_part() || v->is_cut_connector())
                continue;
            const Transform3d          to_halves = Geometry::translation_transform(v->mesh().get_init_shift());
            const std::vector<Corners> corners   = pt_corners(v->mesh().its, to_halves);
            std::vector<int>           map       = pt_match(corners, src_corners);
            for (size_t i = 0; i < map.size(); ++i)
                if (map[i] >= 0 && (on_plane(corners[i][0]) || on_plane(corners[i][1]) || on_plane(corners[i][2])))
                    map[i] = -1;
            for (const FacetsMember layer : PT_LAYERS) {
                const auto src_trees   = pt_trees((src.*layer).get_data(), src.mesh().its.indices.size());
                const auto piece_trees = pt_trees((v->*layer).get_data(), v->mesh().its.indices.size());
                for (size_t i = 0; i < map.size(); ++i) {
                    if (map[i] >= 0) {
                        INFO("piece facet " << i << " = source facet " << map[i]);
                        CHECK(piece_trees[i] == src_trees[size_t(map[i])]);
                    } else if (on_cap(corners[i])) {
                        INFO("cap facet " << i);
                        CHECK(piece_trees[i].empty());
                    }
                }
            }
            for (size_t i = 0; i < map.size(); ++i) {
                if (map[i] >= 0)
                    ++out.matched;
                else if (on_cap(corners[i]))
                    ++out.cap;
                else
                    ++out.split;
            }
        }
    return out;
}

// Painted area of every kind: the halves together against the source (both in the same units).
void pt_check_areas(const ModelObjectPtrs &res, const ModelVolume &src, double area_scale, double rel_tol)
{
    for (const PaintKind &k : PT_KINDS) {
        const double in  = area_scale * pt_painted_area(src, k);
        const double out = pt_painted_area(res, k);
        INFO(k.name << ": source " << in << " mm2, pieces " << out << " mm2");
        REQUIRE(in > 1.);
        CHECK(std::abs(out - in) <= rel_tol * in);
    }
}

} // namespace

TEST_CASE("FacetMatcher finds the facets a piece kept, in order, and nothing else", "[PaintTransfer][CutPaint]")
{
    const indexed_triangle_set src = its_make_cube(10., 10., 10.);
    indexed_triangle_set       piece;
    piece.vertices = src.vertices;
    // Facets 3 and 7 kept as they are, facet 5 with its corners turned (a different facet for the
    // painting, whose tree is relative to the first corner), and one new facet.
    piece.indices = { src.indices[3], src.indices[7], Vec3i32(src.indices[5][1], src.indices[5][2], src.indices[5][0]), Vec3i32(0, 1, 2) };
    piece.vertices.emplace_back(100.f, 100.f, 100.f);
    piece.indices.back() = Vec3i32(int(piece.vertices.size()) - 1, 0, 1);

    const PaintTransfer::FacetMatcher matcher(src);
    const std::vector<int>            map = matcher.match(piece);
    REQUIRE(map == std::vector<int>{ 3, 7, -1, -1 });
}

TEST_CASE("Keep paint v2: a plane cut copies every uncut facet's painting bit for bit, for each paint kind",
          "[PaintTransfer][CutPaint]")
{
    // A turned instance, so the plane crosses the sphere's facets at odd angles.
    Geometry::Transformation inst;
    inst.set_rotation(Vec3d(Geometry::deg2rad(20.), 0., Geometry::deg2rad(35.)));
    Model        model;
    ModelObject *mo = pt_make_object(model, pt_sphere(), inst);
    ModelVolume *v  = mo->volumes.front();
    pt_paint_all(*v);
    const Transform3d src_to_halves = mo->instances.front()->get_transformation().get_matrix_no_offset() * v->get_matrix();
    const auto        source_before = v->mmu_segmentation_facets.get_data();

    // Cut 3 mm above the centre, in the instance's frame.
    const Transform3d cut_matrix = Geometry::translation_transform(Vec3d(0., 0., 3.));
    Cut               cut(mo, 0, cut_matrix, PT_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);

    const ExactCheck c = pt_check_exact(res, *v, src_to_halves, { 3. });
    // Almost all the facets are copies; the plane splits a ring of them.
    CHECK(c.matched > 3 * c.split);
    CHECK(c.cap > 0);
    pt_check_areas(res, *v, 1., 0.01);

    // The cut works on its own copy: the source keeps its painting (what Undo goes back to).
    CHECK(v->mmu_segmentation_facets.get_data() == source_before);
}

TEST_CASE("Keep paint v2: a kerf, a scaled instance and keep as parts", "[PaintTransfer][CutPaint]")
{
    Geometry::Transformation inst;
    inst.set_rotation(Vec3d(0., Geometry::deg2rad(15.), Geometry::deg2rad(-40.)));
    inst.set_scaling_factor(Vec3d(1.5, 1.5, 1.5));

    SECTION("a 2 mm kerf, both halves")
    {
        Model        model;
        ModelObject *mo = pt_make_object(model, pt_sphere(), inst);
        ModelVolume *v  = mo->volumes.front();
        pt_paint_all(*v);
        const Transform3d src_to_halves = mo->instances.front()->get_transformation().get_matrix_no_offset() * v->get_matrix();
        Cut               cut(mo, 0, Transform3d::Identity(), PT_HALVES | ModelObjectCutAttribute::KeepPaint);
        const ModelObjectPtrs &res = cut.perform_with_plane(2.0);
        REQUIRE(res.size() == 2);
        const ExactCheck c = pt_check_exact(res, *v, src_to_halves, { -1., 1. });
        CHECK(c.matched > 3 * c.split);
        CHECK(c.cap > 0);
    }
    SECTION("keep as parts")
    {
        Model        model;
        ModelObject *mo = pt_make_object(model, pt_sphere(), inst);
        ModelVolume *v  = mo->volumes.front();
        pt_paint_all(*v);
        const Transform3d src_to_halves = mo->instances.front()->get_transformation().get_matrix_no_offset() * v->get_matrix();
        Cut               cut(mo, 0, Transform3d::Identity(),
                              PT_HALVES | ModelObjectCutAttribute::KeepAsParts | ModelObjectCutAttribute::KeepPaint);
        const ModelObjectPtrs &res = cut.perform_with_plane();
        REQUIRE(res.size() == 1);
        REQUIRE(res.front()->volumes.size() == 2);
        const ExactCheck c = pt_check_exact(res, *v, src_to_halves, { 0. });
        CHECK(c.matched > 3 * c.split);
        pt_check_areas(res, *v, 1.5 * 1.5, 0.01);
    }
}

TEST_CASE("Keep paint v2: a plane through vertices and along edges", "[PaintTransfer][CutPaint]")
{
    // A cube turned 45 degrees about X: four of its corners and four of its edges lie on z == 0,
    // where the halves are closed next to vertices (#1001).
    const double             s = 40.;
    indexed_triangle_set     its = its_make_cube(s, s, s);
    for (Vec3f &p : its.vertices)
        p -= Vec3f(float(0.5 * s), float(0.5 * s), float(0.5 * s));
    Geometry::Transformation inst;
    inst.set_rotation(Vec3d(0.25 * PI, 0., 0.));
    Model        model;
    ModelObject *mo = pt_make_object(model, its, inst);
    ModelVolume *v  = mo->volumes.front();
    pt_paint(*v, &ModelVolume::mmu_segmentation_facets, [](const Vec3f &p) {
        return p.x() > 19.99f ? EnforcerBlockerType::Extruder2 : p.x() < -19.99f && p.y() > 3.f ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::NONE;
    });
    pt_paint(*v, &ModelVolume::supported_facets, [](const Vec3f &p) {
        return p.z() < -19.99f ? EnforcerBlockerType::ENFORCER : p.z() > 19.99f && p.x() < 0.f ? EnforcerBlockerType::BLOCKER : EnforcerBlockerType::NONE;
    });
    pt_paint(*v, &ModelVolume::seam_facets, [](const Vec3f &p) {
        return p.y() > 19.99f ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
    });
    pt_paint(*v, &ModelVolume::fuzzy_skin_facets, [](const Vec3f &p) {
        return p.y() < -19.99f && p.x() > -5.f ? EnforcerBlockerType::FUZZY_SKIN : EnforcerBlockerType::NONE;
    });
    const Transform3d src_to_halves = mo->instances.front()->get_transformation().get_matrix_no_offset() * v->get_matrix();

    Cut                    cut(mo, 0, Transform3d::Identity(), PT_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);
    pt_check_exact(res, *v, src_to_halves, { 0. });
    // The painted faces are planar and the plane crosses them along straight lines, so every
    // painted area comes through to within a percent.
    pt_check_areas(res, *v, 1., 0.01);
}

TEST_CASE("Keep paint v2: connectors, dowels and copies", "[PaintTransfer][CutPaint]")
{
    using namespace Geometry;
    Model        model;
    ModelObject *mo = pt_make_object(model, pt_sphere());
    ModelVolume *v  = mo->volumes.front();
    pt_paint_all(*v);
    // A second copy, turned and moved: copies share the parts, so they share the painting.
    ModelInstance *copy = mo->add_instance();
    copy->set_offset(Vec3d(80., 0., 0.));
    copy->set_rotation(Vec3d(0., 0., deg2rad(60.)));
    auto add_connector = [mo](CutConnectorType type, const Vec3d &pos, const std::string &name) {
        ModelVolume *c = mo->add_volume(TriangleMesh(its_make_cylinder(1.0, 1.0, 2 * PI / 90.)), ModelVolumeType::NEGATIVE_VOLUME);
        c->set_transformation(translation_transform(pos) * scale_transform(Vec3d(3.0, 3.0, 8.0)));
        c->cut_info = ModelVolume::CutInfo(type, 0.f, 0.1f);
        c->name     = name;
    };
    add_connector(CutConnectorType::Plug, Vec3d(-6.0, 0.0, -4.0), "plug");
    add_connector(CutConnectorType::Dowel, Vec3d(6.0, 0.0, -4.0), "dowel");
    const Transform3d src_to_halves = mo->instances.front()->get_transformation().get_matrix_no_offset() * v->get_matrix();

    Cut cut(mo, 0, Transform3d::Identity(), PT_HALVES | ModelObjectCutAttribute::CreateDowels | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 3); // two halves and the dowel

    ModelObjectPtrs halves;
    int             connectors = 0;
    for (ModelObject *o : res) {
        bool has_part = false;
        for (const ModelVolume *rv : o->volumes) {
            if (rv->is_cut_connector()) {
                ++connectors;
                CHECK_FALSE(rv->is_any_painted());
            } else if (rv->is_model_part())
                has_part = true;
        }
        if (has_part && o->volumes.size() > 1)
            halves.push_back(o);
    }
    CHECK(connectors >= 2);
    REQUIRE(halves.size() == 2);
    for (const ModelObject *h : halves)
        CHECK(h->instances.size() == 2); // the copy is still a copy
    const ExactCheck c = pt_check_exact(halves, *v, src_to_halves, { 0. });
    CHECK(c.matched > 3 * c.split);
    pt_check_areas(halves, *v, 1., 0.01);
}

TEST_CASE("Keep paint v2: a mirrored part stays on the spatial remap and keeps its paint", "[PaintTransfer][CutPaint]")
{
    Geometry::Transformation inst;
    inst.set_scaling_factor(Vec3d(-1., 1., 1.));
    Model        model;
    ModelObject *mo = pt_make_object(model, pt_sphere(), inst);
    ModelVolume *v  = mo->volumes.front();
    pt_paint_all(*v);
    Cut                    cut(mo, 0, Transform3d::Identity(), PT_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);
    // The remap is not exact facet by facet; it keeps the area to a few percent.
    pt_check_areas(res, *v, 1., 0.05);
}

TEST_CASE("Keep paint v2: an unpainted part is cut exactly as before", "[PaintTransfer][CutPaint]")
{
    struct Out
    {
        std::vector<indexed_triangle_set> meshes;
        std::vector<Transform3d>          matrices;
        bool                              painted = false;
    };
    auto run = [](bool paint, bool keep_paint, double kerf) {
        Geometry::Transformation inst;
        inst.set_rotation(Vec3d(Geometry::deg2rad(20.), 0., Geometry::deg2rad(35.)));
        Model        model;
        ModelObject *mo = pt_make_object(model, pt_sphere(), inst);
        if (paint)
            pt_paint_all(*mo->volumes.front());
        ModelObjectCutAttributes attrs = PT_HALVES;
        if (keep_paint)
            attrs = attrs | ModelObjectCutAttribute::KeepPaint;
        Cut                    cut(mo, 0, Geometry::translation_transform(Vec3d(0., 0., 3.)), attrs);
        const ModelObjectPtrs &res = cut.perform_with_plane(kerf);
        Out                    out;
        for (const ModelObject *o : res)
            for (const ModelVolume *v : o->volumes) {
                out.meshes.push_back(v->mesh().its);
                out.matrices.push_back(v->get_matrix());
                out.painted = out.painted || v->is_any_painted();
            }
        return out;
    };
    for (double kerf : { 0., 2. }) {
        INFO("kerf " << kerf);
        const Out plain      = run(false, false, kerf);
        const Out plain_keep = run(false, true, kerf);
        const Out painted    = run(true, true, kerf);
        CHECK_FALSE(plain.painted);
        CHECK_FALSE(plain_keep.painted);
        CHECK(painted.painted);
        REQUIRE(plain.meshes.size() == plain_keep.meshes.size());
        REQUIRE(plain.meshes.size() == painted.meshes.size());
        for (size_t i = 0; i < plain.meshes.size(); ++i) {
            // Byte for byte: the vertices and the facets, in order.
            CHECK(plain.meshes[i].vertices == plain_keep.meshes[i].vertices);
            CHECK(plain.meshes[i].indices == plain_keep.meshes[i].indices);
            CHECK(plain.meshes[i].vertices == painted.meshes[i].vertices);
            CHECK(plain.meshes[i].indices == painted.meshes[i].indices);
            CHECK(plain.matrices[i].isApprox(plain_keep.matrices[i], 0.));
            CHECK(plain.matrices[i].isApprox(painted.matrices[i], 0.));
        }
    }
}

TEST_CASE("Keep paint v2: the halves' painting survives the undo stack's serialization", "[PaintTransfer][CutPaint]")
{
    // Undo goes back to the source object, which the cut never touches (checked above). Redo loads
    // the halves as the undo stack stored them: each paint layer through cereal.
    Model        model;
    ModelObject *mo = pt_make_object(model, pt_sphere());
    pt_paint_all(*mo->volumes.front());
    Cut                    cut(mo, 0, Transform3d::Identity(), PT_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);
    for (const ModelObject *o : res)
        for (const ModelVolume *v : o->volumes)
            for (const FacetsMember layer : PT_LAYERS) {
                const TriangleSelector::TriangleSplittingData &data = (v->*layer).get_data();
                std::stringstream                              ss;
                {
                    cereal::BinaryOutputArchive oa(ss);
                    oa(data);
                }
                TriangleSelector::TriangleSplittingData back;
                {
                    cereal::BinaryInputArchive ia(ss);
                    ia(back);
                }
                CHECK(back == data);
                // And the stored stream is a valid one: it loads into a selector and back unchanged.
                TriangleSelector sel(v->mesh());
                sel.deserialize(back);
                CHECK(sel.serialize() == data);
            }
}

TEST_CASE("Keep paint v2: cut, carry back for Edit cut, cut again", "[PaintTransfer][CutPaint][CutReeditPaint]")
{
    // Edit cut carries the halves' painting back onto the uncut part by the spatial remap
    // (carry_cut_painting_to_uncut()), then the new cut paints the new halves exactly.
    Model        model;
    ModelObject *mo = pt_make_object(model, pt_sphere());
    ModelVolume *v  = mo->volumes.front();
    pt_paint_all(*v);
    Cut                    cut(mo, 0, Transform3d::Identity(), PT_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res = cut.perform_with_plane();
    REQUIRE(res.size() == 2);

    Model        uncut_model;
    ModelObject *uncut = pt_make_object(uncut_model, pt_sphere());
    const CutPaintCarry carry = carry_cut_painting_to_uncut({ res[0], res[1] }, Transform3d::Identity(), *uncut->volumes.front());
    REQUIRE(carry.carried);

    Cut                    again(uncut, 0, Geometry::translation_transform(Vec3d(0., 0., -5.)), PT_HALVES | ModelObjectCutAttribute::KeepPaint);
    const ModelObjectPtrs &res2 = again.perform_with_plane();
    REQUIRE(res2.size() == 2);
    pt_check_exact(res2, *uncut->volumes.front(),
                   uncut->instances.front()->get_transformation().get_matrix_no_offset() * uncut->volumes.front()->get_matrix(), { -5. });
    // Against the first source: the remap back is not exact, so a few percent.
    pt_check_areas(res2, *v, 1., 0.05);
}

// ---------------------------------------------------------------------------
// Timing, old path against new, on a ~1M-facet mesh. Hidden: run it by hand
// (libslic3r_tests "[PaintTransferBench]"). The old path is what main's
// Cut::finalize() did for a plane cut: a cut without KeepPaint, then the saved
// painting remapped onto every half (ModelVolume::restore_painting()).
// ---------------------------------------------------------------------------

TEST_CASE("Keep paint v2: timing on a 1M-facet mesh", "[.][PaintTransferBench]")
{
    using clock = std::chrono::steady_clock;
    auto ms     = [](clock::time_point t) { return double(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t).count()) / 1000.; };

    Model        model;
    ModelObject *mo = pt_make_object(model, its_make_sphere(50., 2. * PI / 1000.));
    ModelVolume *v  = mo->volumes.front();
    const size_t facets = v->mesh().its.indices.size();
    WARN("facets " << facets);

    const Transform3d cut_matrix = Geometry::translation_transform(Vec3d(0., 0., 7.));
    auto cut_new = [&](ModelObject *src) {
        const auto             t = clock::now();
        Cut                    cut(src, 0, cut_matrix, PT_HALVES | ModelObjectCutAttribute::KeepPaint);
        const ModelObjectPtrs &res = cut.perform_with_plane();
        const double           took = ms(t);
        REQUIRE(res.size() == 2);
        return took;
    };
    auto cut_old = [&](ModelObject *src, double *area2) {
        const auto t = clock::now();
        std::optional<TriangleSelector::SavedPainting> sp = src->volumes.front()->save_painting();
        if (sp)
            sp->mesh.transform(src->instances.front()->get_transformation().get_matrix_no_offset() * src->volumes.front()->get_matrix(), true);
        Cut                    cut(src, 0, cut_matrix, PT_HALVES);
        const ModelObjectPtrs &res = cut.perform_with_plane();
        for (ModelObject *o : res)
            for (ModelVolume *rv : o->volumes)
                if (rv->is_model_part() && sp)
                    rv->restore_painting(sp, true);
        const double took = ms(t);
        if (area2)
            *area2 = pt_painted_area(res, PT_KINDS[0]);
        return took;
    };

    // Unpainted.
    const double plain_keep = cut_new(mo);
    const double plain_old  = cut_old(mo, nullptr);
    {
        const auto t = clock::now();
        Cut        cut(mo, 0, cut_matrix, PT_HALVES);
        cut.perform_with_plane();
        WARN("unpainted: cut without KeepPaint " << ms(t) << " ms, with KeepPaint (new code) " << plain_keep << " ms, old path " << plain_old << " ms");
    }

    // Painted: facet-level colour on a third of the skin, plus a stroke across facets on a band.
    {
        TriangleSelector sel(v->mesh());
        for (int i = 0; i < int(facets); ++i) {
            const Vec3f c = (v->mesh().its.vertices[v->mesh().its.indices[i][0]] + v->mesh().its.vertices[v->mesh().its.indices[i][1]] +
                             v->mesh().its.vertices[v->mesh().its.indices[i][2]]) / 3.f;
            if (c.x() > 15.f)
                sel.set_facet(i, EnforcerBlockerType::Extruder2);
        }
        v->mmu_segmentation_facets.set(sel);
    }
    pt_paint(*v, &ModelVolume::seam_facets, [](const Vec3f &p) {
        return std::abs(p.y() - 3.3f) < 0.7f ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
    }, 0.5f);
    const double in_area = pt_painted_area(*v, PT_KINDS[0]);
    double       old_area = 0.;
    const double painted_new = cut_new(mo);
    const double painted_old = cut_old(mo, &old_area);
    double       new_area    = 0.;
    {
        Cut                    cut(mo, 0, cut_matrix, PT_HALVES | ModelObjectCutAttribute::KeepPaint);
        const ModelObjectPtrs &res = cut.perform_with_plane();
        new_area                   = pt_painted_area(res, PT_KINDS[0]);
    }
    WARN("painted: new path " << painted_new << " ms, old path " << painted_old << " ms; colour area source " << in_area
                              << ", new " << new_area << ", old " << old_area);
}
