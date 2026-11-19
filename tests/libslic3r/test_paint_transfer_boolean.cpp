// ---------------------------------------------------------------------------
// Keep paint v2 (NeotkoCM keep-painting-sm240, re-ported from PR #605): Mesh
// Boolean (the gizmo and the object list's Boolean) and Split (to objects, to
// parts) keep support, seam, colour and fuzzy-skin painting.
//
// A boolean builds new facets, so its result is painted by resampling the
// inputs (PaintTransfer); the checks are painted areas worked out by hand on
// boxes, for each paint kind and each operation. A split keeps the facets
// (TriangleMesh::split()), so each piece's painting is checked facet by facet,
// bit for bit, against the facet it came from (matched by its corners in the
// world, by this file's own code).
// ---------------------------------------------------------------------------

#include <catch2/catch.hpp>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>
// After Model.hpp, as test_meshboolean.cpp has them: CSGMesh.hpp needs Transform3f declared.
#include <libslic3r/CSGMesh/ModelToCSGMesh.hpp>
#include <libslic3r/CSGMesh/PerformCSGMeshBooleans.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/PaintTransfer.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <chrono>
#include <cmath>
#include <functional>
#include <sstream>
#include <vector>

using namespace Slic3r;

namespace {

using FacetsMember = FacetsAnnotation ModelVolume::*;
using StateAt      = std::function<EnforcerBlockerType(const Vec3d &)>;

const FacetsMember PB_LAYERS[] = { &ModelVolume::mmu_segmentation_facets, &ModelVolume::supported_facets, &ModelVolume::seam_facets,
                                   &ModelVolume::fuzzy_skin_facets };

double pb_area(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const Vec3i32 &f : its.indices)
        a += 0.5 * double((its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).norm());
    return a;
}

double pb_painted(const ModelVolume &v, FacetsMember member, EnforcerBlockerType state)
{
    return pb_area((v.*member).get_facets(v, state));
}

// Paints one layer from a function of the point in the OBJECT's coordinates (the volume's matrix
// applied), with borders that need not follow the facets.
void pb_paint(ModelVolume &v, FacetsMember member, const StateAt &at, float edge_limit = 0.25f)
{
    const Transform3d to_object = v.get_matrix();
    TriangleSelector  sel(v.mesh());
    sel.resample([&](const Vec3f &p) {
        TriangleSelector::SampledState s;
        s.state = at(to_object * p.cast<double>());
        return s;
    }, edge_limit);
    (v.*member).set(sel);
}

// The boxes: A = [0, 20]^3, B = [15, 25] x [5, 15] x [5, 15], both in the object's coordinates.
struct BoxScene
{
    Model        model;
    ModelObject *mo = nullptr;
    ModelVolume *a  = nullptr;
    ModelVolume *b  = nullptr;

    explicit BoxScene(bool paint = true, bool mirror_b = false)
    {
        mo = model.add_object();
        mo->name = "boxes";
        a = mo->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
        a->name = "A";
        indexed_triangle_set bits = its_make_cube(10., 10., 10.);
        its_translate(bits, Vec3f(15.f, 5.f, 5.f));
        b = mo->add_volume(TriangleMesh(bits));
        b->name = "B";
        if (mirror_b)
            // Mirrored in place about its own centre: the same box, its facets turned inside out.
            b->set_transformation(Geometry::Transformation(b->get_matrix() * Geometry::scale_transform(Vec3d(-1., 1., 1.))));
        mo->add_instance();
        if (!paint)
            return;
        // A: colour 2 on the +X face below y = 12.5, support enforcer on the bottom, seam on +Y,
        // fuzzy skin on -X above z = 7.3.
        pb_paint(*a, &ModelVolume::mmu_segmentation_facets, [](const Vec3d &p) {
            return p.x() > 19.99 && p.y() < 12.5 ? EnforcerBlockerType::Extruder2 : EnforcerBlockerType::NONE;
        });
        pb_paint(*a, &ModelVolume::supported_facets, [](const Vec3d &p) {
            return p.z() < 0.01 ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
        });
        pb_paint(*a, &ModelVolume::seam_facets, [](const Vec3d &p) {
            return p.y() > 19.99 ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
        });
        pb_paint(*a, &ModelVolume::fuzzy_skin_facets, [](const Vec3d &p) {
            return p.x() < 0.01 && p.z() > 7.3 ? EnforcerBlockerType::FUZZY_SKIN : EnforcerBlockerType::NONE;
        });
        // B: colour 3 on its +X face, support blocker on its top.
        pb_paint(*b, &ModelVolume::mmu_segmentation_facets, [](const Vec3d &p) {
            return p.x() > 24.99 ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::NONE;
        });
        pb_paint(*b, &ModelVolume::supported_facets, [](const Vec3d &p) {
            return p.z() > 14.99 ? EnforcerBlockerType::BLOCKER : EnforcerBlockerType::NONE;
        });
    }
};

struct Expected
{
    double colour2, colour3, enforcer, blocker, seam, fuzzy;
};

void pb_check(const ModelVolume &r, const Expected &e)
{
    auto close_to = [](double got, double want) { return std::abs(got - want) <= 3.0 + 0.01 * want; };
    const double c2 = pb_painted(r, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2);
    const double c3 = pb_painted(r, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3);
    const double en = pb_painted(r, &ModelVolume::supported_facets, EnforcerBlockerType::ENFORCER);
    const double bl = pb_painted(r, &ModelVolume::supported_facets, EnforcerBlockerType::BLOCKER);
    const double se = pb_painted(r, &ModelVolume::seam_facets, EnforcerBlockerType::ENFORCER);
    const double fz = pb_painted(r, &ModelVolume::fuzzy_skin_facets, EnforcerBlockerType::FUZZY_SKIN);
    INFO("colour 2 " << c2 << ", colour 3 " << c3 << ", enforcer " << en << ", blocker " << bl << ", seam " << se << ", fuzzy " << fz);
    CHECK(close_to(c2, e.colour2));
    CHECK(close_to(c3, e.colour3));
    CHECK(close_to(en, e.enforcer));
    CHECK(close_to(bl, e.blocker));
    CHECK(close_to(se, e.seam));
    CHECK(close_to(fz, e.fuzzy));
}

// The gizmo's apply path without the GUI: the boolean in object coordinates, then
// replace_volume_with_object_mesh() with the inputs as paint sources, then the tool deleted.
ModelVolume *pb_gizmo(BoxScene &s, const std::string &op, bool with_sources = true)
{
    TriangleMesh src = s.a->mesh();
    src.transform(s.a->get_matrix(), true);
    TriangleMesh tool = s.b->mesh();
    tool.transform(s.b->get_matrix(), true);
    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::make_boolean_with_fallback(src, tool, op, out) == MeshBoolean::BooleanOutcome::Done);
    REQUIRE(!out.empty());
    const std::vector<PaintTransfer::Source> sources = PaintTransfer::part_boolean_sources(*s.a, *s.b, op == "A_NOT_B");
    ModelVolume *nv = s.mo->replace_volume_with_object_mesh(0, TriangleMesh(out.front()), "r", with_sources ? &sources : nullptr);
    s.mo->delete_volume(1);
    return nv;
}

// Facet trees: facet -> its bits (empty: no painting).
std::vector<std::vector<bool>> pb_trees(const TriangleSelector::TriangleSplittingData &data, size_t num_facets)
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

// A painted volume as it was before a split changed it: its world facets and its trees.
struct Snapshot
{
    std::vector<std::array<Vec3d, 3>>             corners;
    std::vector<std::vector<std::vector<bool>>>   trees; // [layer][facet]
};

Snapshot pb_snapshot(const ModelVolume &v, const Transform3d &to_world)
{
    Snapshot s;
    const indexed_triangle_set &its = v.mesh().its;
    s.corners.resize(its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i)
        for (int c = 0; c < 3; ++c)
            s.corners[i][c] = to_world * its.vertices[its.indices[i][c]].cast<double>();
    for (const FacetsMember layer : PB_LAYERS)
        s.trees.push_back(pb_trees((v.*layer).get_data(), its.indices.size()));
    return s;
}

// Checks a piece facet by facet against the snapshot; returns how many facets matched.
size_t pb_check_piece(const ModelVolume &piece, const Transform3d &to_world, const Snapshot &src)
{
    const indexed_triangle_set &its     = piece.mesh().its;
    size_t                      matched = 0;
    std::vector<std::vector<std::vector<bool>>> trees;
    for (const FacetsMember layer : PB_LAYERS)
        trees.push_back(pb_trees((piece.*layer).get_data(), its.indices.size()));
    for (size_t i = 0; i < its.indices.size(); ++i) {
        std::array<Vec3d, 3> c;
        for (int k = 0; k < 3; ++k)
            c[k] = to_world * its.vertices[its.indices[i][k]].cast<double>();
        for (size_t j = 0; j < src.corners.size(); ++j)
            if ((c[0] - src.corners[j][0]).norm() < 1e-3 && (c[1] - src.corners[j][1]).norm() < 1e-3 && (c[2] - src.corners[j][2]).norm() < 1e-3) {
                ++matched;
                for (size_t l = 0; l < trees.size(); ++l) {
                    INFO("layer " << l << " piece facet " << i << " = source facet " << j);
                    CHECK(trees[l][i] == src.trees[l][j]);
                }
                break;
            }
    }
    return matched;
}

// Two 10 mm cubes, 20 mm apart, in one mesh; the second optionally inside out.
indexed_triangle_set pb_two_cubes(bool second_inside_out = false)
{
    indexed_triangle_set its = its_make_cube(10., 10., 10.);
    indexed_triangle_set two = its_make_cube(10., 10., 10.);
    its_translate(two, Vec3f(30.f, 0.f, 0.f));
    if (second_inside_out)
        for (Vec3i32 &f : two.indices)
            std::swap(f[1], f[2]);
    its_merge(its, two);
    return its;
}

void pb_paint_all_kinds(ModelVolume &v)
{
    pb_paint(v, &ModelVolume::mmu_segmentation_facets, [](const Vec3d &p) {
        return p.z() > 6.1 ? EnforcerBlockerType::Extruder2 : p.x() > 33.3 ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::NONE;
    });
    pb_paint(v, &ModelVolume::supported_facets, [](const Vec3d &p) {
        return p.y() < 2.2 ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
    });
    pb_paint(v, &ModelVolume::seam_facets, [](const Vec3d &p) {
        return std::abs(p.x() - 4.4) < 1.1 || std::abs(p.x() - 35.5) < 1.1 ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE;
    });
    pb_paint(v, &ModelVolume::fuzzy_skin_facets, [](const Vec3d &p) {
        return p.y() > 7.7 ? EnforcerBlockerType::FUZZY_SKIN : EnforcerBlockerType::NONE;
    });
}

} // namespace

TEST_CASE("Keep paint v2: Mesh Boolean gizmo, each operation and paint kind", "[PaintTransfer][MeshBoolean]")
{
    for (bool mirror_b : { false, true }) {
        DYNAMIC_SECTION("union" << (mirror_b ? ", mirrored tool" : ""))
        {
            BoxScene s(true, mirror_b);
            // A's +X face loses the 7.5 x 10 under B; B keeps what is outside A.
            pb_check(*pb_gizmo(s, "UNION"), { 175., 100., 400., 50., 400., 254. });
        }
        DYNAMIC_SECTION("difference" << (mirror_b ? ", mirrored tool" : ""))
        {
            BoxScene s(true, mirror_b);
            // B's skin left in the result is the wall of the pocket: plain.
            pb_check(*pb_gizmo(s, "A_NOT_B"), { 175., 0., 400., 0., 400., 254. });
        }
        DYNAMIC_SECTION("intersection" << (mirror_b ? ", mirrored tool" : ""))
        {
            BoxScene s(true, mirror_b);
            // [15, 20] x [5, 15] x [5, 15]: A's +X face below y = 12.5, and B's top.
            pb_check(*pb_gizmo(s, "INTERSECTION"), { 75., 0., 0., 50., 0., 0. });
        }
    }
}

TEST_CASE("Keep paint v2: an unpainted boolean is what it was", "[PaintTransfer][MeshBoolean]")
{
    for (const char *op : { "UNION", "A_NOT_B", "INTERSECTION" }) {
        INFO(op);
        BoxScene     with(false), without(false);
        ModelVolume *r1 = pb_gizmo(with, op, true);
        ModelVolume *r2 = pb_gizmo(without, op, false);
        CHECK_FALSE(r1->is_any_painted());
        CHECK(r1->mesh().its.vertices == r2->mesh().its.vertices);
        CHECK(r1->mesh().its.indices == r2->mesh().its.indices);
        CHECK(r1->get_matrix().isApprox(r2->get_matrix(), 0.));
    }
}

TEST_CASE("Keep paint v2: a union keeps the filament of each part on its unpainted skin", "[PaintTransfer][MeshBoolean]")
{
    BoxScene s(false);
    s.b->config.set("extruder", 3);
    ModelVolume *r = pb_gizmo(s, "UNION");
    // B's skin outside A: its +X face, half of each of four sides.
    CHECK(pb_painted(*r, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3) == Approx(300.).margin(3.));
    CHECK(pb_painted(*r, &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2) == Approx(0.).margin(1e-6));
    // A difference's tool has no skin of its own in the result.
    BoxScene d(false);
    d.b->config.set("extruder", 3);
    CHECK(pb_painted(*pb_gizmo(d, "A_NOT_B"), &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3) ==
          Approx(0.).margin(1e-6));
}

TEST_CASE("Keep paint v2: the object list's Boolean, with a negative part and copies", "[PaintTransfer][MeshBoolean][CSGMesh]")
{
    BoxScene s;
    // A 4 x 4 pocket into A's bottom: the enforcer loses 16 mm2, the pocket walls stay plain.
    indexed_triangle_set pocket = its_make_cube(4., 4., 5.);
    its_translate(pocket, Vec3f(2.f, 2.f, -1.f));
    s.mo->add_volume(TriangleMesh(pocket), ModelVolumeType::NEGATIVE_VOLUME);
    // A second copy, turned: the boolean works in the object's own coordinates, copies apart.
    s.mo->add_instance()->set_transformation(Geometry::Transformation(Geometry::translation_transform(Vec3d(60., 0., 0.)) *
                                                                       Geometry::rotation_transform(Vec3d(0., 0., 0.7))));

    // What Plater::combine_mesh_fff(object, -2) does.
    std::vector<csg::CSGPart> parts;
    csg::model_to_csgmesh(*s.mo, Transform3d::Identity(), std::back_inserter(parts), csg::mpartsPositive | csg::mpartsNegative);
    MeshBoolean::mcut::McutMeshPtr mesh_ptr = csg::perform_csgmesh_booleans_mcut(Range{ std::begin(parts), std::end(parts) });
    TriangleMesh mesh = MeshBoolean::mcut::mcut_to_triangle_mesh(*mesh_ptr);
    REQUIRE(!mesh.empty());

    ModelObject *result = s.model.add_object();
    result->config.assign_config(s.mo->config);
    ModelVolume *nv = result->add_volume(mesh);
    for (const ModelInstance *i : s.mo->instances)
        result->add_instance(*i);
    REQUIRE(PaintTransfer::paint_boolean_result(PaintTransfer::object_boolean_sources(*s.mo), *nv));
    pb_check(*nv, { 175., 100., 384., 50., 400., 254. });
}

TEST_CASE("Keep paint v2: the boolean result's painting survives the undo stack's serialization", "[PaintTransfer][MeshBoolean]")
{
    BoxScene     s;
    ModelVolume *r = pb_gizmo(s, "UNION");
    for (const FacetsMember layer : PB_LAYERS) {
        const TriangleSelector::TriangleSplittingData &data = (r->*layer).get_data();
        REQUIRE_FALSE(data.triangles_to_split.empty());
        std::stringstream ss;
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
    }
}

TEST_CASE("Keep paint v2: split to objects copies each piece's painting facet by facet", "[PaintTransfer][Split]")
{
    Model        model;
    ModelObject *mo = model.add_object();
    ModelVolume *v  = mo->add_volume(TriangleMesh(pb_two_cubes()));
    mo->add_instance()->set_transformation(Geometry::Transformation(Geometry::translation_transform(Vec3d(50., 40., 0.)) *
                                                                    Geometry::rotation_transform(Vec3d(0., 0., 0.4))));
    pb_paint_all_kinds(*v);
    const Transform3d src_world = mo->instances.front()->get_matrix() * v->get_matrix();
    const Snapshot    snap      = pb_snapshot(*v, src_world);

    ModelObjectPtrs pieces;
    mo->split(&pieces);
    REQUIRE(pieces.size() == 2);
    size_t matched = 0, facets = 0;
    for (const ModelObject *p : pieces) {
        REQUIRE(p->volumes.size() == 1);
        const ModelVolume *pv = p->volumes.front();
        facets += pv->mesh().its.indices.size();
        matched += pb_check_piece(*pv, p->instances.front()->get_matrix() * pv->get_matrix(), snap);
    }
    CHECK(matched == facets);
    CHECK(facets == v->mesh().its.indices.size());
}

TEST_CASE("Keep paint v2: split to parts copies each piece's painting facet by facet", "[PaintTransfer][Split]")
{
    Model        model;
    ModelObject *mo = model.add_object();
    ModelVolume *v  = mo->add_volume(TriangleMesh(pb_two_cubes()));
    // A second part, so the split part is not the whole object, and a turned, scaled part.
    const ModelVolume *other = mo->add_volume(TriangleMesh(its_make_cube(5., 5., 5.)));
    mo->volumes.back()->set_offset(Vec3d(0., 40., 0.));
    v->set_transformation(Geometry::Transformation(Geometry::translation_transform(Vec3d(3., 4., 5.)) *
                                                   Geometry::rotation_transform(Vec3d(0.3, 0., 0.2)) *
                                                   Geometry::scale_transform(Vec3d(1.2, 1.2, 1.2))));
    mo->add_instance();
    pb_paint_all_kinds(*v);
    const Snapshot snap        = pb_snapshot(*v, v->get_matrix());
    const size_t   src_facets  = v->mesh().its.indices.size();

    REQUIRE(v->split(4) == 2);
    size_t matched = 0, facets = 0;
    for (const ModelVolume *pv : mo->volumes) {
        if (pv == other)
            continue;
        facets += pv->mesh().its.indices.size();
        matched += pb_check_piece(*pv, pv->get_matrix(), snap);
    }
    CHECK(facets == src_facets);
    CHECK(matched == facets);
}

TEST_CASE("Keep paint v2: an object of several parts split to objects keeps every paint kind", "[PaintTransfer][Split]")
{
    Model        model;
    ModelObject *mo = model.add_object();
    ModelVolume *a  = mo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    indexed_triangle_set away = its_make_cube(10., 10., 10.);
    its_translate(away, Vec3f(30.f, 0.f, 0.f));
    ModelVolume *b = mo->add_volume(TriangleMesh(away));
    mo->add_instance();
    pb_paint_all_kinds(*a);
    pb_paint_all_kinds(*b);
    std::vector<std::vector<TriangleSelector::TriangleSplittingData>> before;
    for (const ModelVolume *v : { a, b }) {
        before.emplace_back();
        for (const FacetsMember layer : PB_LAYERS)
            before.back().push_back((v->*layer).get_data());
    }
    ModelObjectPtrs pieces;
    mo->split(&pieces);
    REQUIRE(pieces.size() == 2);
    for (size_t i = 0; i < 2; ++i)
        for (size_t l = 0; l < std::size(PB_LAYERS); ++l) {
            INFO("piece " << i << " layer " << l);
            CHECK((pieces[i]->volumes.front()->*PB_LAYERS[l]).get_data() == before[i][l]);
        }
}

TEST_CASE("Keep paint v2: a shell split turns inside out is resampled, not dropped", "[PaintTransfer][Split]")
{
    Model        model;
    ModelObject *mo = model.add_object();
    ModelVolume *v  = mo->add_volume(TriangleMesh(pb_two_cubes(true)));
    mo->add_instance();
    pb_paint_all_kinds(*v);
    struct Kind { FacetsMember member; EnforcerBlockerType state; };
    const Kind kinds[] = { { &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2 },
                           { &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3 },
                           { &ModelVolume::supported_facets, EnforcerBlockerType::ENFORCER },
                           { &ModelVolume::seam_facets, EnforcerBlockerType::ENFORCER },
                           { &ModelVolume::fuzzy_skin_facets, EnforcerBlockerType::FUZZY_SKIN } };
    std::vector<double> in;
    for (const Kind &k : kinds)
        in.push_back(pb_painted(*v, k.member, k.state));

    REQUIRE(v->split(4) == 2);
    for (size_t i = 0; i < std::size(kinds); ++i) {
        double out = 0.;
        for (const ModelVolume *pv : mo->volumes)
            out += pb_painted(*pv, kinds[i].member, kinds[i].state);
        INFO("kind " << i << ": source " << in[i] << ", pieces " << out);
        CHECK(std::abs(out - in[i]) <= 0.01 * in[i] + 0.5);
    }
}

// ---------------------------------------------------------------------------
// Timing on a ~1M-facet mesh. Hidden: run it by hand (libslic3r_tests
// "[PaintTransferBench]"). The old path (main before keep paint v2) carried no
// painting through a split or a boolean, so it costs what an unpainted run costs.
// ---------------------------------------------------------------------------

TEST_CASE("Keep paint v2: split and boolean timing on a 1M-facet mesh", "[.][PaintTransferBench]")
{
    using clock = std::chrono::steady_clock;
    auto ms     = [](clock::time_point t) { return double(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t).count()) / 1000.; };

    auto make = [](Model &model, bool paint) {
        ModelObject         *mo  = model.add_object();
        indexed_triangle_set its = its_make_sphere(30., 2. * PI / 700.);
        indexed_triangle_set two = its_make_sphere(30., 2. * PI / 700.);
        its_translate(two, Vec3f(80.f, 0.f, 0.f));
        its_merge(its, two);
        ModelVolume *v = mo->add_volume(TriangleMesh(its));
        mo->add_instance();
        if (paint) {
            TriangleSelector sel(v->mesh());
            for (int i = 0; i < int(v->mesh().its.indices.size()); ++i)
                if (v->mesh().its.vertices[v->mesh().its.indices[i][0]].z() > 10.f)
                    sel.set_facet(i, EnforcerBlockerType::Extruder2);
            v->mmu_segmentation_facets.set(sel);
        }
        return mo;
    };
    for (bool paint : { false, true }) {
        {
            Model        model;
            ModelObject *mo = make(model, paint);
            WARN("facets " << mo->volumes.front()->mesh().its.indices.size());
            const auto t = clock::now();
            mo->volumes.front()->split(4);
            WARN((paint ? "painted" : "unpainted") << " split to parts: " << ms(t) << " ms");
        }
        {
            Model           model;
            ModelObject    *mo = make(model, paint);
            const auto      t  = clock::now();
            ModelObjectPtrs pieces;
            mo->split(&pieces);
            WARN((paint ? "painted" : "unpainted") << " split to objects: " << ms(t) << " ms");
        }
        {
            Model        model;
            ModelObject *mo = make(model, paint);
            ModelVolume *v  = mo->volumes.front();
            ModelVolume *tool = mo->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
            tool->set_offset(Vec3d(30., 0., 0.));
            TriangleMesh src = v->mesh();
            src.transform(v->get_matrix(), true);
            TriangleMesh tm = tool->mesh();
            tm.transform(tool->get_matrix(), true);
            std::vector<TriangleMesh> out;
            const auto t0 = clock::now();
            REQUIRE(MeshBoolean::make_boolean_with_fallback(src, tm, "A_NOT_B", out) == MeshBoolean::BooleanOutcome::Done);
            const double boolean_ms = ms(t0);
            const std::vector<PaintTransfer::Source> sources = PaintTransfer::part_boolean_sources(*v, *tool, true);
            const auto t1 = clock::now();
            mo->replace_volume_with_object_mesh(0, TriangleMesh(out.front()), "r", &sources);
            WARN((paint ? "painted" : "unpainted") << " difference: boolean " << boolean_ms << " ms, replace + paint " << ms(t1) << " ms");
        }
    }
}
