// ---------------------------------------------------------------------------
// Repair / remesh keeps paint (RepairPaint.hpp): Fix model, Repair/Remesh, Round all edges,
// Quad remesh, Simplify and Sculpt's Subdivide replace a part's mesh with a rebuilt one, and
// the support, seam, colour and fuzzy-skin painting is carried onto the new facets instead of
// being cleared.
//
// The painting is laid down from analytic functions of the position (half-spaces), so every
// check compares the carried painting with that function - not with the old mesh's painting
// looked up by nearest point, which would only confirm the carry against itself.
// ---------------------------------------------------------------------------

#include <catch2/catch.hpp>

#include <libslic3r/Model.hpp>
#include <libslic3r/QuadricEdgeCollapse.hpp>
#include <libslic3r/RepairPaint.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

using namespace Slic3r;

namespace {

using FacetsMember = FacetsAnnotation ModelVolume::*;
using StateAt      = std::function<EnforcerBlockerType(const Vec3f &)>;
// Distance from a point to the nearest border of the painting (where StateAt changes).
using BorderDist   = std::function<float(const Vec3f &)>;

double rp_area(const indexed_triangle_set &its)
{
    double a = 0.;
    for (const Vec3i32 &f : its.indices)
        a += 0.5 * double((its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]).norm());
    return a;
}

float rp_max_edge(const indexed_triangle_set &its)
{
    float e = 0.f;
    for (const Vec3i32 &f : its.indices)
        for (int i = 0; i < 3; ++i)
            e = std::max(e, (its.vertices[f[(i + 1) % 3]] - its.vertices[f[i]]).norm());
    return e;
}

// Paints one layer of `v` from `at` (mesh coords), with borders that cut through facets.
void rp_paint(ModelVolume &v, FacetsMember member, const StateAt &at, float edge_limit)
{
    TriangleSelector sel(v.mesh());
    sel.resample([&](const Vec3f &p) {
        TriangleSelector::SampledState s;
        s.state = at(p);
        return s;
    }, edge_limit);
    (v.*member).set(sel);
}

struct LayerCheck
{
    double old_area   = 0.; // painted with the state on the old mesh
    double new_area   = 0.; // painted with the state on the new mesh
    double wrong_area = 0.; // of new_area, where the function says another state, farther than a facet from a border
};

// Compares the painting of `state` on `v` (new mesh) with the function, after it covered
// `old_area` on the old mesh. `facet` is how far a carried border may stray.
LayerCheck rp_check(const ModelVolume &v, FacetsMember member, EnforcerBlockerType state, const StateAt &at, const BorderDist &border,
                    float facet, double old_area)
{
    LayerCheck c;
    c.old_area                     = old_area;
    const indexed_triangle_set its = (v.*member).get_facets(v, state);
    for (const Vec3i32 &f : its.indices) {
        const Vec3f  a = its.vertices[f[0]], b = its.vertices[f[1]], cc = its.vertices[f[2]];
        const double area = 0.5 * double((b - a).cross(cc - a).norm());
        c.new_area += area;
        const Vec3f centre = (a + b + cc) / 3.f;
        if (at(centre) != state && border(centre) > facet)
            c.wrong_area += area;
    }
    return c;
}

ModelVolume *rp_add(Model &model, indexed_triangle_set its)
{
    ModelObject *mo = model.add_object();
    mo->name        = "painted";
    ModelVolume *v  = mo->add_volume(TriangleMesh(std::move(its)), ModelVolumeType::MODEL_PART, /*modify_to_center_geometry*/ false);
    mo->add_instance();
    return v;
}

// The painting the sphere tests use (sphere of radius 10 about the origin).
struct SpherePaint
{
    static EnforcerBlockerType colour(const Vec3f &p)
    {
        if (p.z() < -5.f) return EnforcerBlockerType::Extruder3;
        if (p.x() > 3.f) return EnforcerBlockerType::Extruder2;
        return EnforcerBlockerType::NONE;
    }
    static float colour_border(const Vec3f &p) { return std::min(std::abs(p.z() + 5.f), std::abs(p.x() - 3.f)); }
    static EnforcerBlockerType support(const Vec3f &p) { return p.z() < -7.f ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE; }
    static float support_border(const Vec3f &p) { return std::abs(p.z() + 7.f); }
    static EnforcerBlockerType seam(const Vec3f &p) { return p.y() > 8.f ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::NONE; }
    static float seam_border(const Vec3f &p) { return std::abs(p.y() - 8.f); }
    static EnforcerBlockerType fuzzy(const Vec3f &p) { return p.x() < -6.f ? EnforcerBlockerType::FUZZY_SKIN : EnforcerBlockerType::NONE; }
    static float fuzzy_border(const Vec3f &p) { return std::abs(p.x() + 6.f); }

    static void paint(ModelVolume &v, float edge_limit)
    {
        rp_paint(v, &ModelVolume::mmu_segmentation_facets, colour, edge_limit);
        rp_paint(v, &ModelVolume::supported_facets, support, edge_limit);
        rp_paint(v, &ModelVolume::seam_facets, seam, edge_limit);
        rp_paint(v, &ModelVolume::fuzzy_skin_facets, fuzzy, edge_limit);
    }
};

struct Expect
{
    FacetsMember        member;
    EnforcerBlockerType state;
    StateAt             at;
    BorderDist          border;
};

const std::vector<Expect> &sphere_expect()
{
    static const std::vector<Expect> e = {
        { &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder2, SpherePaint::colour, SpherePaint::colour_border },
        { &ModelVolume::mmu_segmentation_facets, EnforcerBlockerType::Extruder3, SpherePaint::colour, SpherePaint::colour_border },
        { &ModelVolume::supported_facets, EnforcerBlockerType::ENFORCER, SpherePaint::support, SpherePaint::support_border },
        { &ModelVolume::seam_facets, EnforcerBlockerType::ENFORCER, SpherePaint::seam, SpherePaint::seam_border },
        { &ModelVolume::fuzzy_skin_facets, EnforcerBlockerType::FUZZY_SKIN, SpherePaint::fuzzy, SpherePaint::fuzzy_border },
    };
    return e;
}

std::vector<double> rp_old_areas(const ModelVolume &v)
{
    std::vector<double> out;
    for (const Expect &e : sphere_expect())
        out.push_back(rp_area((v.*e.member).get_facets(v, e.state)));
    return out;
}

// Replaces `v`'s mesh with `new_its` the way the repair paths do, and carries the painting.
RepairPaint::Result rp_replace(ModelVolume &v, indexed_triangle_set new_its, float tolerance = 0.f)
{
    const RepairPaint::Saved saved(v);
    v.set_mesh(TriangleMesh(std::move(new_its)));
    v.set_new_unique_id();
    return RepairPaint::restore(saved, v, tolerance);
}

void rp_require_kept(const ModelVolume &v, const std::vector<double> &old_areas, float facet)
{
    const std::vector<Expect> &ex = sphere_expect();
    for (size_t i = 0; i < ex.size(); ++i) {
        const LayerCheck c = rp_check(v, ex[i].member, ex[i].state, ex[i].at, ex[i].border, facet, old_areas[i]);
        INFO("layer " << i << ": old " << c.old_area << " new " << c.new_area << " wrong " << c.wrong_area);
        REQUIRE(c.old_area > 1.);
        // More than 95 % of the painted area survives ...
        CHECK(c.new_area >= 0.95 * c.old_area);
        CHECK(c.new_area <= 1.08 * c.old_area);
        // ... with the right state: nothing painted farther than a facet from the border.
        CHECK(c.wrong_area <= 0.01 * c.new_area);
    }
}

} // namespace

TEST_CASE("RepairPaint: a simplified (remeshed) sphere keeps every paint layer", "[RepairPaint]")
{
    Model        model;
    ModelVolume *v = rp_add(model, its_make_sphere(10., 2. * PI / 120.)); // ~28.8k facets
    SpherePaint::paint(*v, 0.05f);
    const std::vector<double> old_areas = rp_old_areas(*v);
    const float               old_edge  = rp_max_edge(v->mesh().its);

    // Simplify to a quarter of the facets: a new triangulation, slightly off the old surface.
    indexed_triangle_set simplified = v->mesh().its;
    its_quadric_edge_collapse(simplified, uint32_t(simplified.indices.size() / 4));
    const float new_edge = rp_max_edge(simplified);
    REQUIRE(simplified.indices.size() < v->mesh().its.indices.size());

    const RepairPaint::Result r = rp_replace(*v, std::move(simplified));
    REQUIRE(r.carried);
    rp_require_kept(*v, old_areas, std::max(old_edge, new_edge));
}

TEST_CASE("RepairPaint: a painted mesh with holes keeps its paint when repaired closed", "[RepairPaint]")
{
    // Old: a sphere with holes (every 23rd facet missing), as a repair would find it.
    indexed_triangle_set holed = its_make_sphere(10., 2. * PI / 100.);
    {
        indexed_triangle_set kept;
        kept.vertices = holed.vertices;
        for (size_t i = 0; i < holed.indices.size(); ++i)
            if (i % 23 != 7)
                kept.indices.push_back(holed.indices[i]);
        holed = std::move(kept);
    }
    REQUIRE(its_num_open_edges(holed) > 0);
    Model        model;
    ModelVolume *v = rp_add(model, std::move(holed));
    SpherePaint::paint(*v, 0.05f);
    const std::vector<double> old_areas = rp_old_areas(*v);

    // New: closed, with its own (different) triangulation - holes filled, facets renumbered.
    indexed_triangle_set repaired = its_make_sphere(10., 2. * PI / 90.);
    const float          facet    = std::max(rp_max_edge(v->mesh().its), rp_max_edge(repaired));
    const RepairPaint::Result r   = rp_replace(*v, std::move(repaired));
    REQUIRE(r.carried);
    // The holes were about 4 % of the old surface; the filled patches take the paint around
    // them, so the new painted area is a little MORE than the old.
    rp_require_kept(*v, old_areas, facet);
}

TEST_CASE("RepairPaint: borders stay sharp across a coarse remesh", "[RepairPaint]")
{
    // Old: fine sphere. New: much coarser, so most new facets straddle the colour border and
    // have to be split along it rather than take one colour whole.
    Model        model;
    ModelVolume *v = rp_add(model, its_make_sphere(10., 2. * PI / 160.));
    SpherePaint::paint(*v, 0.05f);
    const std::vector<double> old_areas = rp_old_areas(*v);
    indexed_triangle_set coarse = its_make_sphere(10., 2. * PI / 24.);
    const RepairPaint::Result r = rp_replace(*v, std::move(coarse));
    REQUIRE(r.carried);
    // The border is followed inside the coarse facets: within an OLD facet of the function,
    // not within a coarse one. The coarse sphere's flat facets sit inside the old surface, so
    // the painted area shrinks with it (about 1.5 %); the band test of rp_require_kept still holds.
    rp_require_kept(*v, old_areas, rp_max_edge(its_make_sphere(10., 2. * PI / 160.)) + 0.1f);
}

TEST_CASE("RepairPaint: a thin wall does not take the paint of its other side", "[RepairPaint]")
{
    // Old: a 20 x 20 x 0.6 slab, its top painted. New: the slab rebuilt thinner and lower
    // (z 0.1 .. 0.2), so its top is NEARER the old bottom than the old top. Normal agreement
    // must still take the top's colour.
    indexed_triangle_set slab = its_make_cube(20., 20., 0.6);
    Model        model;
    ModelVolume *v = rp_add(model, std::move(slab));
    auto top = [](const Vec3f &p) { return p.z() > 0.59f ? EnforcerBlockerType::Extruder2 : EnforcerBlockerType::NONE; };
    rp_paint(*v, &ModelVolume::mmu_segmentation_facets, top, 0.05f);

    indexed_triangle_set thin = its_make_cube(20., 20., 0.1);
    its_translate(thin, Vec3f(0.f, 0.f, 0.1f));
    const RepairPaint::Result r = rp_replace(*v, std::move(thin));
    REQUIRE(r.carried);

    const indexed_triangle_set painted = v->mmu_segmentation_facets.get_facets(*v, EnforcerBlockerType::Extruder2);
    // The whole new top (400 mm^2) and nothing of the bottom.
    double top_area = 0., other = 0.;
    for (const Vec3i32 &f : painted.indices) {
        const Vec3f  a = painted.vertices[f[0]], b = painted.vertices[f[1]], c = painted.vertices[f[2]];
        const double area = 0.5 * double((b - a).cross(c - a).norm());
        if ((a.z() + b.z() + c.z()) / 3.f > 0.199f)
            top_area += area;
        else
            other += area;
    }
    CHECK(top_area == Approx(400.).epsilon(0.01));
    CHECK(other < 0.5);
}

TEST_CASE("RepairPaint: an unpainted part is left exactly as it is", "[RepairPaint]")
{
    Model        model;
    ModelVolume *v = rp_add(model, its_make_sphere(10., 2. * PI / 60.));
    indexed_triangle_set simplified = v->mesh().its;
    its_quadric_edge_collapse(simplified, uint32_t(simplified.indices.size() / 2));
    const indexed_triangle_set expected = simplified;

    const RepairPaint::Saved saved(*v);
    CHECK_FALSE(saved.has_painting());
    CHECK(saved.volume() == nullptr);
    v->set_mesh(TriangleMesh(std::move(simplified)));
    const RepairPaint::Result r = RepairPaint::restore(saved, *v);
    CHECK_FALSE(r.carried);
    CHECK(v->supported_facets.empty());
    CHECK(v->seam_facets.empty());
    CHECK(v->mmu_segmentation_facets.empty());
    CHECK(v->fuzzy_skin_facets.empty());
    CHECK(v->mesh().its.vertices == expected.vertices);
    CHECK(v->mesh().its.indices == expected.indices);
}

TEST_CASE("RepairPaint: a layer that was not painted stays empty, the others are carried", "[RepairPaint]")
{
    Model        model;
    ModelVolume *v = rp_add(model, its_make_sphere(10., 2. * PI / 60.));
    rp_paint(*v, &ModelVolume::mmu_segmentation_facets, SpherePaint::colour, 0.05f);
    REQUIRE(v->supported_facets.empty());
    const RepairPaint::Result r = rp_replace(*v, its_make_sphere(10., 2. * PI / 50.));
    REQUIRE(r.carried);
    CHECK_FALSE(v->mmu_segmentation_facets.empty());
    CHECK(v->supported_facets.empty());
    CHECK(v->seam_facets.empty());
    CHECK(v->fuzzy_skin_facets.empty());
}

TEST_CASE("RepairPaint: a 1M-facet remesh is carried in reasonable time", "[RepairPaint][slow]")
{
    // Old: ~1M facets, painted with borders that split facets (stroke-like leaves), so the carry
    // walks split painting, not whole facets. New: ~960k facets of another triangulation.
    Model        model;
    ModelVolume *v = rp_add(model, its_make_sphere(10., 2. * PI / 1000.));
    REQUIRE(v->mesh().its.indices.size() > 950000);
    // Wavy colour bands so the borders run across facets at every angle.
    auto bands = [](const Vec3f &p) {
        const float s = std::sin(1.3f * p.x()) + std::cos(0.9f * p.y()) + 0.4f * p.z();
        return s > 0.5f ? EnforcerBlockerType::Extruder2 : (s < -1.f ? EnforcerBlockerType::Extruder3 : EnforcerBlockerType::NONE);
    };
    rp_paint(*v, &ModelVolume::mmu_segmentation_facets, bands, 0.002f);
    rp_paint(*v, &ModelVolume::supported_facets, SpherePaint::support, 0.002f);
    const double old2 = rp_area(v->mmu_segmentation_facets.get_facets(*v, EnforcerBlockerType::Extruder2));
    const double old3 = rp_area(v->mmu_segmentation_facets.get_facets(*v, EnforcerBlockerType::Extruder3));
    const double olds = rp_area(v->supported_facets.get_facets(*v, EnforcerBlockerType::ENFORCER));

    indexed_triangle_set remeshed = its_make_sphere(10., 2. * PI / 980.);
    const size_t         new_facets = remeshed.indices.size();
    const auto           t0 = std::chrono::steady_clock::now();
    const RepairPaint::Result r = rp_replace(*v, std::move(remeshed));
    const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    REQUIRE(r.carried);

    const double new2 = rp_area(v->mmu_segmentation_facets.get_facets(*v, EnforcerBlockerType::Extruder2));
    const double new3 = rp_area(v->mmu_segmentation_facets.get_facets(*v, EnforcerBlockerType::Extruder3));
    const double news = rp_area(v->supported_facets.get_facets(*v, EnforcerBlockerType::ENFORCER));
    WARN("RepairPaint 1M: " << new_facets << " new facets, " << ms << " ms total (carry " << r.ms << " ms, "
                             << r.stats.samples << " samples); kept colour 2 " << 100. * new2 / old2 << " %, colour 3 "
                             << 100. * new3 / old3 << " %, supports " << 100. * news / olds << " %");
    CHECK(new2 >= 0.95 * old2);
    CHECK(new3 >= 0.95 * old3);
    CHECK(news >= 0.95 * olds);
    CHECK(new2 <= 1.05 * old2);
    CHECK(new3 <= 1.05 * old3);
    // Generous: a regression to a per-sample brute-force search would take minutes.
    CHECK(ms < 60000);
}
