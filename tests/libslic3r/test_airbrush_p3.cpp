// Airbrush P3 (tests/design_airbrush_paint.md, section 5 row P3, plus the P2 follow-ups): the
// filament remap of a field (A1), committing a pending bake before an undo (A3), carrying the field
// through mesh edits and scaling (A4), nozzle-aware grain floors (B), and the Gradient tool and
// procedural patterns (C).
#include <catch2/catch.hpp>

#include "libslic3r/Airbrush/AirbrushBake.hpp"
#include "libslic3r/Airbrush/AirbrushField.hpp"
#include "libslic3r/Airbrush/AirbrushProcedural.hpp"
#include "libslic3r/Airbrush/AirbrushSession.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Airbrush;
using Catch::Matchers::WithinAbs;

namespace {

// A closed box whose faces are regular grids of `step` (shared vertices), from `o`.
indexed_triangle_set p3_box(double sx, double sy, double sz, double step, const Vec3d &o = Vec3d::Zero())
{
    indexed_triangle_set                    its;
    std::map<std::array<long long, 3>, int> vmap;
    auto vert = [&](const Vec3d &p) {
        const std::array<long long, 3> k = { std::llround(p.x() * 1e4), std::llround(p.y() * 1e4), std::llround(p.z() * 1e4) };
        auto it = vmap.find(k);
        if (it != vmap.end())
            return it->second;
        its.vertices.emplace_back(p.cast<float>());
        const int idx = int(its.vertices.size()) - 1;
        vmap.emplace(k, idx);
        return idx;
    };
    auto tri = [&](int a, int b, int c, const Vec3d &out) {
        const Vec3d pa = its.vertices[a].cast<double>(), pb = its.vertices[b].cast<double>(), pc = its.vertices[c].cast<double>();
        if ((pb - pa).cross(pc - pa).dot(out) < 0.)
            std::swap(b, c);
        its.indices.emplace_back(a, b, c);
    };
    auto grid = [&](const Vec3d &O, const Vec3d &U, const Vec3d &V, int nu, int nv, const Vec3d &out) {
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i) {
                const int a = vert(O + U * (double(i) / nu) + V * (double(j) / nv));
                const int b = vert(O + U * (double(i + 1) / nu) + V * (double(j) / nv));
                const int c = vert(O + U * (double(i + 1) / nu) + V * (double(j + 1) / nv));
                const int d = vert(O + U * (double(i) / nu) + V * (double(j + 1) / nv));
                tri(a, b, c, out);
                tri(a, c, d, out);
            }
    };
    const int nx = std::max(1, int(std::lround(sx / step))), ny = std::max(1, int(std::lround(sy / step))),
              nz = std::max(1, int(std::lround(sz / step)));
    grid(o + Vec3d(0, 0, sz), Vec3d(sx, 0, 0), Vec3d(0, sy, 0), nx, ny, Vec3d(0, 0, 1));
    grid(o, Vec3d(sx, 0, 0), Vec3d(0, sy, 0), nx, ny, Vec3d(0, 0, -1));
    grid(o, Vec3d(sx, 0, 0), Vec3d(0, 0, sz), nx, nz, Vec3d(0, -1, 0));
    grid(o + Vec3d(0, sy, 0), Vec3d(sx, 0, 0), Vec3d(0, 0, sz), nx, nz, Vec3d(0, 1, 0));
    grid(o, Vec3d(0, sy, 0), Vec3d(0, 0, sz), ny, nz, Vec3d(-1, 0, 0));
    grid(o + Vec3d(sx, 0, 0), Vec3d(0, sy, 0), Vec3d(0, 0, sz), ny, nz, Vec3d(1, 0, 0));
    return its;
}

// One 20 x 20 x 10 box part on filament 1.
struct Part
{
    Model        model;
    ModelObject *obj = nullptr;
    ModelVolume *vol = nullptr;
    explicit Part(double step = 1.)
    {
        obj = model.add_object();
        obj->config.set("extruder", 1);
        // Not centred, so the box is [0, 20] x [0, 20] x [0, 10] in mesh space.
        vol = obj->add_volume(TriangleMesh(p3_box(20, 20, 10, step)), ModelVolumeType::MODEL_PART, false);
        obj->add_instance();
    }
    const indexed_triangle_set &its() const { return vol->mesh().its; }
};

// A spray of `filament` over the top centre, baked into the volume.
void spray_and_bake(ModelVolume &vol, int filament, const AirbrushBakeParams &bp, float radius = 4.f)
{
    auto            field = std::make_shared<AirbrushField>(vol.airbrush_field ? *vol.airbrush_field : AirbrushField());
    AirbrushSurface surface(vol.mesh().its);
    StrokeParams    sp;
    sp.slot = field->add_slot(filament);
    REQUIRE(sp.slot >= 0);
    field->begin_stroke(sp);
    Dab d;
    d.center   = Vec3f(10.f, 10.f, 10.f);
    d.radius   = radius;
    d.hardness = 0.4f;
    d.flow     = 0.6f;
    d.normal   = Vec3f::UnitZ();
    for (int i = 0; i < 3; ++i)
        field->dab(surface, d);
    field->end_stroke();
    BakeJob job;
    job.mesh    = vol.mesh_ptr();
    job.field   = field;
    job.current = vol.mmu_segmentation_facets.get_data();
    job.params  = bp;
    const BakeJobResult r = run_bake_job(job);
    REQUIRE(r.status == BakeJobResult::Status::Ok);
    commit_bake(vol, r);
}

TriangleSelector::TriangleSplittingData normalized(const TriangleMesh &mesh, const TriangleSelector::TriangleSplittingData &d)
{
    TriangleSelector sel(mesh);
    sel.deserialize(d, true);
    return sel.serialize();
}

EnforcerBlockerStateMap identity_map()
{
    EnforcerBlockerStateMap m;
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = EnforcerBlockerType(i);
    return m;
}

int facet_near(const indexed_triangle_set &its, const Vec3f &p)
{
    int   best = -1;
    float bd   = std::numeric_limits<float>::max();
    for (size_t f = 0; f < its.indices.size(); ++f) {
        const Vec3f c = (its.vertices[its.indices[f][0]] + its.vertices[its.indices[f][1]] + its.vertices[its.indices[f][2]]) / 3.f;
        if ((c - p).squaredNorm() < bd) {
            bd   = (c - p).squaredNorm();
            best = int(f);
        }
    }
    return best;
}

Weights weights_at(const AirbrushField &f, const Vec3f &p, int cls)
{
    Weights w {};
    f.sample(p.cast<double>(), cls, w);
    return w;
}

AirbrushBakeParams plain_params(uint32_t seed = 5)
{
    AirbrushBakeParams bp;
    bp.base_filament = 1;
    bp.mix_steps     = 0;
    bp.seed          = seed;
    return bp;
}

} // namespace

// =============================================================================================
// A1: remap filaments
// =============================================================================================

TEST_CASE("Airbrush P3: remapping filaments remaps the field's slots, under-paint and base", "[Airbrush][AirbrushP3]")
{
    Part part;
    ModelVolume &vol = *part.vol;
    // Paint under the spray: a top facet on filament 4, before the airbrush.
    const int under_facet = facet_near(part.its(), Vec3f(10.2f, 10.2f, 10.f));
    {
        TriangleSelector sel(vol.mesh());
        sel.set_facet(under_facet, EnforcerBlockerType::Extruder4);
        vol.mmu_segmentation_facets.set(sel);
    }
    spray_and_bake(vol, 2, plain_params());
    REQUIRE(vol.airbrush_field);
    REQUIRE(airbrush_paint_in_sync(vol));
    REQUIRE(vol.airbrush_field->slots() == std::vector<int>{ 2 });
    REQUIRE(std::find(vol.airbrush_field->record().owned.begin(), vol.airbrush_field->record().owned.end(), under_facet) !=
            vol.airbrush_field->record().owned.end());

    // The gizmo's remap: 2 <-> 3, 4 -> 5, 1 -> 6 (the part's own filament).
    EnforcerBlockerStateMap map = identity_map();
    map[2] = EnforcerBlockerType::Extruder3;
    map[3] = EnforcerBlockerType::Extruder2;
    map[4] = EnforcerBlockerType::Extruder5;
    map[1] = EnforcerBlockerType::Extruder6;
    TriangleSelector sel(vol.mesh());
    sel.deserialize(vol.mmu_segmentation_facets.get_data(), true);
    sel.remap_triangle_state(map);
    vol.mmu_segmentation_facets.set(sel);
    const auto remapped_paint = vol.mmu_segmentation_facets.get_data();
    // Without the field remap the paint reads as a foreign edit.
    CHECK_FALSE(airbrush_paint_in_sync(vol));

    auto f = airbrush_remap_field(*vol.airbrush_field, vol.mesh(), map, remapped_paint);
    REQUIRE(f);
    CHECK(f->slots() == std::vector<int>{ 3 });
    AirbrushBakeParams bp;
    REQUIRE(AirbrushBakeParams::from_string(f->bake_params(), bp));
    CHECK(bp.base_filament == 6);
    // The under-paint went through the same map: facet `under_facet` was 4, is 5.
    {
        TriangleSelector u(vol.mesh());
        u.deserialize(f->record().under, true);
        const auto used = u.serialize().used_states;
        CHECK(used[size_t(EnforcerBlockerType::Extruder5)]);
        CHECK_FALSE(used[size_t(EnforcerBlockerType::Extruder4)]);
    }
    vol.airbrush_field = f;
    CHECK(airbrush_paint_in_sync(vol));

    SECTION("the next bake is not a foreign edit and gives the remapped paint again")
    {
        BakeJob job;
        job.mesh    = vol.mesh_ptr();
        job.field   = vol.airbrush_field;
        job.current = vol.mmu_segmentation_facets.get_data();
        job.params  = bp;
        const BakeJobResult r = run_bake_job(job);
        REQUIRE(r.status == BakeJobResult::Status::Ok);
        CHECK(normalized(vol.mesh(), r.painting) == normalized(vol.mesh(), remapped_paint));
    }
    SECTION("an identity map changes nothing")
    {
        CHECK_FALSE(airbrush_remap_field(*vol.airbrush_field, vol.mesh(), identity_map(), vol.mmu_segmentation_facets.get_data()));
    }
}

TEST_CASE("Airbrush P3: slots that remap onto one filament are merged", "[Airbrush][AirbrushP3]")
{
    AirbrushField f;
    REQUIRE(f.add_slot(2) == 0);
    REQUIRE(f.add_slot(3) == 1);
    const uint64_t k = AirbrushField::make_key(1, 2, 3, 4);
    Weights        w {};
    w[0] = 100;
    w[1] = 50;
    f.set_cell(k, w);
    CHECK(f.remap_slots([](int id) { return id == 3 ? 2 : id; }));
    CHECK(f.slots() == std::vector<int>{ 2 });
    REQUIRE(f.find(k));
    CHECK(f.find(k)->w[0] == 150);
    CHECK(f.find(k)->w[1] == 0);
    CHECK_FALSE(f.remap_slots([](int id) { return id; }));
    // An id outside the filament range keeps its slot.
    CHECK_FALSE(f.remap_slots([](int) { return 0; }));
}

// =============================================================================================
// A3: an undo while a bake is pending
// =============================================================================================

TEST_CASE("Airbrush P3: committing a pending bake before an undo keeps the step before it", "[Airbrush][AirbrushP3]")
{
    // The undo stack as the Plater keeps it: one saved state per committed step.
    struct State
    {
        TriangleSelector::TriangleSplittingData paint;
        std::string                             blob;
    };
    Part part;
    ModelVolume &vol = *part.vol;
    auto save = [&vol]() {
        return State{ vol.mmu_segmentation_facets.get_data(), vol.airbrush_field ? vol.airbrush_field->to_blob() : std::string() };
    };
    auto load = [&vol](const State &s) {
        vol.mmu_segmentation_facets.set_data(TriangleSelector::TriangleSplittingData(s.paint));
        vol.airbrush_field = s.blob.empty() ? nullptr : AirbrushField::from_blob(s.blob);
    };
    std::vector<State> stack;
    stack.push_back(save());                        // before anything
    spray_and_bake(vol, 2, plain_params(), 3.f);    // stroke 1, committed
    stack.push_back(save());
    const State after_stroke_1 = stack.back();

    // Stroke 2 is laid and its bake job captured, but not committed yet.
    auto            working = std::make_shared<AirbrushField>(*vol.airbrush_field);
    AirbrushSurface surface(vol.mesh().its);
    StrokeParams    sp;
    sp.slot = working->add_slot(3);
    working->begin_stroke(sp);
    Dab d;
    d.center = Vec3f(4.f, 4.f, 10.f);
    d.radius = 3.f;
    d.flow   = 0.8f;
    d.normal = Vec3f::UnitZ();
    working->dab(surface, d);
    working->end_stroke();
    BakeJob job;
    job.mesh    = vol.mesh_ptr();
    job.field   = working;
    job.current = vol.mmu_segmentation_facets.get_data();
    job.params  = plain_params();

    SECTION("P3: the pending bake is committed first, so the undo removes stroke 2 only")
    {
        const BakeJobResult r = run_bake_job(job);
        REQUIRE(r.status == BakeJobResult::Status::Ok);
        commit_bake(vol, r);
        stack.push_back(save());
        // Undo: back one step.
        load(stack[stack.size() - 2]);
        CHECK(vol.mmu_segmentation_facets.get_data() == after_stroke_1.paint);
        REQUIRE(vol.airbrush_field);
        CHECK(vol.airbrush_field->to_blob() == after_stroke_1.blob);
        CHECK(airbrush_paint_in_sync(vol));
        // Redo brings stroke 2 back.
        load(stack.back());
        CHECK(vol.mmu_segmentation_facets.get_data() == r.painting);
    }
    SECTION("P2's behaviour, for contrast: undo first drops stroke 1 AND the pending stroke 2")
    {
        load(stack[stack.size() - 2]);
        CHECK(vol.mmu_segmentation_facets.empty());
        CHECK(vol.airbrush_field == nullptr);
    }
}

// =============================================================================================
// A4: mesh edits and scale
// =============================================================================================

TEST_CASE("Airbrush P3: the field follows mesh edits and scaling", "[Airbrush][AirbrushP3]")
{
    Part part;
    ModelVolume &vol = *part.vol;
    spray_and_bake(vol, 2, plain_params());
    REQUIRE(airbrush_paint_in_sync(vol));
    const size_t cells = vol.airbrush_field->size();

    SECTION("save / set_mesh (a retriangulated mesh) / restore re-bakes on the new mesh")
    {
        const auto saved = vol.save_painting();
        REQUIRE(saved);
        REQUIRE(saved->airbrush_field);
        vol.set_mesh(TriangleMesh(p3_box(20, 20, 10, 0.5)));
        vol.restore_painting(saved);
        REQUIRE(vol.airbrush_field);
        CHECK(vol.airbrush_field->record().facets == uint32_t(vol.mesh().its.indices.size()));
        CHECK(airbrush_paint_in_sync(vol));
        CHECK_FALSE(vol.mmu_segmentation_facets.empty());
        CHECK(vol.airbrush_field->size() == cells);
    }
    SECTION("a repair that keeps positions re-bakes in place")
    {
        const auto old_mesh  = vol.mesh_ptr();
        const auto old_paint = vol.mmu_segmentation_facets.get_data();
        vol.set_mesh(TriangleMesh(p3_box(20, 20, 10, 2.)));
        REQUIRE(airbrush_after_mesh_replaced(vol, old_mesh->its, old_paint));
        CHECK(airbrush_paint_in_sync(vol));
        CHECK_FALSE(vol.mmu_segmentation_facets.empty());
    }
    SECTION("scaling the mesh scales the field and re-bakes it")
    {
        vol.scale_geometry_after_creation(Vec3f(2.f, 2.f, 2.f));
        REQUIRE(vol.airbrush_field);
        CHECK(airbrush_paint_in_sync(vol));
        // An enlarged field is splatted without holes: more cells than before.
        CHECK(vol.airbrush_field->size() > cells);
        // The cells sit on the scaled top (z = 20).
        bool on_top = false;
        for (const auto &kv : vol.airbrush_field->cells())
            on_top |= std::abs(vol.airbrush_field->center_of(kv.first).z() - 20.) < 0.5;
        CHECK(on_top);
    }
    SECTION("a mesh the field does not reach drops the field")
    {
        const auto old_mesh  = vol.mesh_ptr();
        const auto old_paint = vol.mmu_segmentation_facets.get_data();
        const std::shared_ptr<const AirbrushField> field = vol.airbrush_field;
        vol.set_mesh(TriangleMesh(p3_box(5, 5, 5, 1., Vec3d(500, 500, 0))));
        CHECK_FALSE(airbrush_carry_field(vol, *field, old_mesh->its, old_paint, Transform3d::Identity()));
        CHECK(vol.airbrush_field == nullptr);
    }
    SECTION("a mesh boolean result keeps the spray (the source is replaced, centred)")
    {
        ModelVolume *nv = part.obj->replace_volume_with_object_mesh(0, TriangleMesh(p3_box(20, 20, 10, 1.)), "union");
        REQUIRE(nv);
        REQUIRE(nv->airbrush_field);
        CHECK(airbrush_paint_in_sync(*nv));
        CHECK_FALSE(nv->mmu_segmentation_facets.empty());
    }
    SECTION("reset_extra_facets drops the field with the paint")
    {
        vol.reset_extra_facets();
        CHECK(vol.airbrush_field == nullptr);
    }
}

TEST_CASE("Airbrush P3: grain is a printed size; scaling a part re-bakes it", "[Airbrush][AirbrushP3]")
{
    Part part;
    ModelVolume &vol = *part.vol;
    spray_and_bake(vol, 2, plain_params());
    CHECK(airbrush_print_scale(vol) == 1.f);
    CHECK_FALSE(airbrush_rebake_after_scale(vol));

    AirbrushBakeParams bp;
    REQUIRE(AirbrushBakeParams::from_string(vol.airbrush_field->bake_params(), bp));
    CHECK(bp.to_string().find("pscale") == std::string::npos);

    // Half size: the same printed grain is twice as wide in mesh mm, so a quarter of the cells.
    AirbrushBakeStats one;
    REQUIRE(airbrush_bake_volume(vol, bp, {}, &one));
    part.obj->instances.front()->set_scaling_factor(Vec3d(0.5, 0.5, 0.5));
    CHECK_THAT(double(airbrush_print_scale(vol)), WithinAbs(0.5, 1e-6));
    REQUIRE(airbrush_rebake_after_scale(vol));
    AirbrushBakeParams after;
    REQUIRE(AirbrushBakeParams::from_string(vol.airbrush_field->bake_params(), after));
    CHECK_THAT(double(after.print_scale), WithinAbs(0.5, 1e-6));
    CHECK(airbrush_paint_in_sync(vol));
    AirbrushBakeStats half;
    REQUIRE(airbrush_bake_volume(vol, after, {}, &half));
    CHECK(2 * half.grain_cells < one.grain_cells);
    // Reported in printed mm.
    CHECK_THAT(double(half.grain_wall_mm), WithinAbs(double(one.grain_wall_mm), 1e-4));
    CHECK_FALSE(airbrush_rebake_after_scale(vol));
}

// =============================================================================================
// B: nozzle-aware grain floors
// =============================================================================================

TEST_CASE("Airbrush P3: grain floors per nozzle", "[Airbrush][AirbrushP3]")
{
    SECTION("a 0.4 mm nozzle (and unknown) is exactly the P0 floors")
    {
        for (float d : { 0.4f, 0.f, -1.f }) {
            const AirbrushFloors f = airbrush_floors_for_nozzle(d);
            CHECK(f.wall_mm == AIRBRUSH_MIN_GRAIN_WALL_MM);
            CHECK(f.top_mm == AIRBRUSH_MIN_GRAIN_TOP_MM);
        }
        CHECK(AIRBRUSH_MIN_GRAIN_WALL_MM == 0.6f);
        CHECK(AIRBRUSH_MIN_GRAIN_TOP_MM == 0.8f);
    }
    SECTION("the floors scale with the nozzle")
    {
        CHECK_THAT(double(airbrush_floors_for_nozzle(0.2f).wall_mm), WithinAbs(0.3, 1e-6));
        CHECK_THAT(double(airbrush_floors_for_nozzle(0.2f).top_mm), WithinAbs(0.4, 1e-6));
        CHECK_THAT(double(airbrush_floors_for_nozzle(0.25f).wall_mm), WithinAbs(0.375, 1e-6));
        CHECK_THAT(double(airbrush_floors_for_nozzle(0.6f).wall_mm), WithinAbs(0.9, 1e-6));
        CHECK_THAT(double(airbrush_floors_for_nozzle(0.8f).top_mm), WithinAbs(1.6, 1e-6));
        // Never below the slider's minimum.
        CHECK(airbrush_floors_for_nozzle(0.05f).wall_mm == AIRBRUSH_MIN_GRAIN_MM);
    }
    SECTION("the hint fires below the nozzle's wall floor only")
    {
        CHECK(airbrush_grain_below_floor(0.1f, 0.2f));
        CHECK_FALSE(airbrush_grain_below_floor(0.3f, 0.2f));
        CHECK(airbrush_grain_below_floor(0.5f, 0.4f));
        CHECK_FALSE(airbrush_grain_below_floor(0.6f, 0.4f));
        CHECK_FALSE(airbrush_grain_below_floor(1.0f, 0.4f));
    }
    SECTION("brush: the slider reaches 0.1 mm; the nozzle override round-trips")
    {
        BrushSettings b;
        b.grain_mm = 0.1f;
        b.sanitize();
        CHECK(b.grain_mm == 0.1f);
        b.grain_mm = 0.02f;
        b.sanitize();
        CHECK(b.grain_mm == AIRBRUSH_MIN_GRAIN_MM);
        CHECK(b.to_string().find("nozzle") == std::string::npos);   // Auto writes nothing
        b.nozzle_mm = 0.25f;
        const BrushSettings back = BrushSettings::from_string(b.to_string());
        CHECK(back.nozzle_mm == 0.25f);
        b.nozzle_mm = 9.f;
        b.sanitize();
        CHECK(b.nozzle_mm == AIRBRUSH_NOZZLE_MAX);
    }
    SECTION("bake parameters: a 0.4 nozzle keeps the P1 top floor; a 0.2 nozzle lowers it; the override wins")
    {
        BrushSettings b;
        b.grain_mm = 0.1f;
        AirbrushBakeParams p4 = bake_params_from(b, 1, 0.4f);
        CHECK(p4.min_grain_top_mm == AIRBRUSH_MIN_GRAIN_TOP_MM);
        CHECK(p4.min_grain_wall_mm == AIRBRUSH_MIN_GRAIN_MM);
        CHECK(p4.to_string().find("nozzle") == std::string::npos);
        AirbrushBakeParams p2 = bake_params_from(b, 1, 0.2f);
        CHECK_THAT(double(p2.min_grain_top_mm), WithinAbs(0.4, 1e-6));
        // The wall grain is honoured; a top below its floor falls back to tone at the floor.
        CHECK(effective_grain(p2, false).grain_mm == 0.1f);
        CHECK_FALSE(effective_grain(p2, false).tone_fallback);
        CHECK_THAT(double(effective_grain(p2, true).grain_mm), WithinAbs(0.4, 1e-6));
        CHECK(effective_grain(p2, true).tone_fallback);
        // "Printer Nozzle Diameter" = 0.25 on a 0.4 printer.
        b.nozzle_mm           = 0.25f;
        AirbrushBakeParams po = bake_params_from(b, 1, 0.4f);
        CHECK(po.design_nozzle_mm == 0.25f);
        CHECK_THAT(double(po.min_grain_top_mm), WithinAbs(0.5, 1e-6));
        AirbrushBakeParams back;
        REQUIRE(AirbrushBakeParams::from_string(po.to_string(), back));
        CHECK(back == po);
        CHECK(back.design_nozzle_mm == 0.25f);
        CHECK(airbrush_design_nozzle(back, 0.4f) == 0.25f);
        CHECK(airbrush_design_nozzle(p4, 0.6f) == 0.6f);
    }
    SECTION("a P1/P2 parameter string is unchanged")
    {
        const AirbrushBakeParams d;
        CHECK(d.to_string().find("nozzle") == std::string::npos);
        CHECK(d.to_string().find("pscale") == std::string::npos);
    }
    SECTION("segmentation's run filters: identical at 0.4 mm and above, finer below")
    {
        CHECK(mm_segmentation_run_filter_scale(0.4) == 1.f);
        CHECK(mm_segmentation_run_filter_scale(0.6) == 1.f);
        CHECK(mm_segmentation_run_filter_scale(0.8) == 1.f);
        CHECK(mm_segmentation_run_filter_scale(0.) == 1.f);
        CHECK(mm_segmentation_run_filter_scale(-1.) == 1.f);
        CHECK_THAT(double(mm_segmentation_run_filter_scale(0.2)), WithinAbs(0.5, 1e-6));
        CHECK_THAT(double(mm_segmentation_run_filter_scale(0.25)), WithinAbs(0.625, 1e-6));
        CHECK(mm_segmentation_run_filter_scale(0.05) == 0.25f);
    }
    SECTION("a 0.1 mm grain bakes finer specks than a 0.6 mm one")
    {
        Part a, b;
        AirbrushBakeParams coarse = plain_params();
        coarse.grain_mm           = 0.6f;
        AirbrushBakeParams fine   = plain_params();
        fine.grain_mm             = 0.1f;
        fine.min_grain_wall_mm    = AIRBRUSH_MIN_GRAIN_MM;
        fine.min_grain_top_mm     = airbrush_floors_for_nozzle(0.1f).top_mm;
        spray_and_bake(*a.vol, 2, coarse);
        AirbrushBakeStats sc, sf;
        REQUIRE(airbrush_bake_volume(*a.vol, coarse, {}, &sc));
        spray_and_bake(*b.vol, 2, fine);
        REQUIRE(airbrush_bake_volume(*b.vol, fine, {}, &sf));
        CHECK(sf.grain_top_mm < sc.grain_top_mm);
        CHECK(sf.grain_cells > 4 * sc.grain_cells);
    }
}

// =============================================================================================
// C: gradients and patterns
// =============================================================================================

TEST_CASE("Airbrush P3: fill recipes round-trip and sanitize", "[Airbrush][AirbrushP3]")
{
    AirbrushFillRecipe r;
    r.kind     = FillKind::Marble;
    r.stops    = { { 3, 1.f }, { 0, 0.f }, { 2, 0.4f } };
    r.start    = Vec3f(1.5f, -2.f, 3.f);
    r.end      = Vec3f(4.f, 5.f, 6.f);
    r.up       = Vec3f(0.f, 0.6f, 0.8f);
    r.scale_mm = 12.5f;
    r.contrast = 1.5f;
    r.octaves  = 5;
    r.amount   = 0.2f;
    r.seed     = 777;
    r.opacity  = 0.75f;
    REQUIRE(r.sanitize());
    // Sorted by position.
    CHECK(r.stops[0].filament == 0);
    CHECK(r.stops[1].filament == 2);
    CHECK(r.stops[2].filament == 3);
    AirbrushFillRecipe back;
    REQUIRE(AirbrushFillRecipe::from_string(r.to_string(), back));
    CHECK(back == r);
    CHECK(back.kind == FillKind::Marble);
    CHECK(back.seed == 777);

    AirbrushFillRecipe one;
    one.stops = { { 2, 0.f } };
    CHECK_FALSE(one.sanitize());
    CHECK_FALSE(AirbrushFillRecipe::from_string("kind=bogus;stops=1@0,2@1", back));
    CHECK_FALSE(AirbrushFillRecipe::from_string("kind=noise;stops=1@0", back));
    CHECK(AirbrushFillRecipe::from_string("kind=noise;stops=1@0,2@1", back));
    // More stops than allowed are cut.
    AirbrushFillRecipe many;
    for (int i = 0; i < 10; ++i)
        many.stops.push_back({ 1 + i, float(i) / 9.f });
    REQUIRE(many.sanitize());
    CHECK(many.stops.size() == AIRBRUSH_MAX_STOPS);
}

TEST_CASE("Airbrush P3: ramps and noise functions", "[Airbrush][AirbrushP3]")
{
    const auto stops = std::vector<GradientStop>{ { 2, 0.f }, { 3, 0.5f }, { 4, 1.f } };
    auto w = ramp_weights(stops, 0.25f, false);
    CHECK_THAT(double(w[0]), WithinAbs(0.5, 1e-6));
    CHECK_THAT(double(w[1]), WithinAbs(0.5, 1e-6));
    CHECK(w[2] == 0.f);
    w = ramp_weights(stops, -1.f, false);
    CHECK(w[0] == 1.f);
    w = ramp_weights(stops, 2.f, false);
    CHECK(w[2] == 1.f);
    // Hard bands: one stop each third.
    CHECK(ramp_weights(stops, 0.1f, true)[0] == 1.f);
    CHECK(ramp_weights(stops, 0.5f, true)[1] == 1.f);
    CHECK(ramp_weights(stops, 0.99f, true)[2] == 1.f);
    CHECK(ramp_weights(stops, 1.f, true)[2] == 1.f);

    // Noise: in range, deterministic, seed-dependent, continuous.
    double mn = 1., mx = 0., diff = 0.;
    for (int i = 0; i < 2000; ++i) {
        const Vec3f p(float(i) * 0.137f, float(i % 37) * 0.291f, float(i % 11) * 0.73f);
        const float a = fbm_noise(p, 4, 1);
        CHECK(a == fbm_noise(p, 4, 1));
        mn = std::min(mn, double(a));
        mx = std::max(mx, double(a));
        diff += std::abs(a - fbm_noise(p, 4, 2));
        CHECK(std::abs(value_noise(p, 9) - value_noise(p + Vec3f(1e-3f, 0.f, 0.f), 9)) < 0.01f);
    }
    CHECK(mn >= 0.);
    CHECK(mx <= 1.);
    CHECK(mx - mn > 0.3);
    CHECK(diff / 2000. > 0.05);
    // Worley: the distance to the nearest feature point is at most sqrt(3) cells.
    for (int i = 0; i < 200; ++i)
        CHECK(worley_f1(Vec3f(float(i) * 0.31f, 1.7f, -2.2f), 3) <= 1.7321f);

    SECTION("speckle covers about the asked share of a surface")
    {
        AirbrushFillRecipe r;
        r.kind     = FillKind::Speckle;
        r.stops    = { { 0, 0.f }, { 2, 1.f } };
        r.scale_mm = 1.f;
        r.amount   = 0.3f;
        double sum = 0.;
        int    n   = 0;
        for (int i = 0; i < 100; ++i)
            for (int j = 0; j < 100; ++j, ++n)
                sum += pattern_value(r, Vec3f(float(i) * 0.173f, float(j) * 0.173f, 0.5f));
        const double cover = sum / double(n);
        CHECK(cover > 0.15);
        CHECK(cover < 0.42);
    }
    SECTION("marble and camo stay in range")
    {
        AirbrushFillRecipe r;
        r.stops = { { 2, 0.f }, { 3, 1.f } };
        for (FillKind k : { FillKind::Marble, FillKind::Camo, FillKind::Noise })
            for (int i = 0; i < 300; ++i) {
                r.kind        = k;
                const float t = pattern_value(r, Vec3f(float(i) * 0.7f, float(i) * 0.3f, float(i % 13)));
                CHECK(t >= 0.f);
                CHECK(t <= 1.f);
            }
    }
}

TEST_CASE("Airbrush P3: surface distance for the radial gradient", "[Airbrush][AirbrushP3]")
{
    const indexed_triangle_set its = p3_box(20, 20, 10, 1.);
    const Vec3f                seed(10.f, 10.f, 10.f);
    std::vector<char>          patch;
    const std::vector<float>   dist = geodesic_distances(its, seed, &patch);
    // The whole top is the flat patch: straight-line distance, exact.
    const int top_f = facet_near(its, Vec3f(10.3f, 15.3f, 10.f));
    CHECK(patch[size_t(top_f)]);
    CHECK_THAT(double(geodesic_at(its, dist, patch, seed, size_t(top_f), Vec3f(10.f, 15.f, 10.f))), WithinAbs(5., 1e-4));
    // Down the +Y side: 10 to the edge, then 5 down - not the 11.2 straight through the air.
    const int side_f = facet_near(its, Vec3f(10.3f, 20.f, 5.3f));
    CHECK_FALSE(patch[size_t(side_f)]);
    CHECK_THAT(double(geodesic_at(its, dist, patch, seed, size_t(side_f), Vec3f(10.f, 20.f, 5.f))), WithinAbs(15., 0.6));
}

TEST_CASE("Airbrush P3: gradients and patterns fill the field deterministically", "[Airbrush][AirbrushP3]")
{
    const indexed_triangle_set its = p3_box(20, 20, 10, 1.);
    AirbrushSurface            surface(its);
    auto run = [&](const AirbrushFillRecipe &r, AirbrushField &f) {
        std::string error;
        const long  n = apply_fill(f, surface, r, nullptr, &error);
        INFO(error);
        REQUIRE(n >= 0);
        return n;
    };

    SECTION("linear: the stops at the ends of the drag, a blend between")
    {
        AirbrushFillRecipe r;
        r.kind  = FillKind::Linear;
        r.stops = two_stops(2, 3);
        r.start = Vec3f(2.f, 10.f, 10.f);
        r.end   = Vec3f(18.f, 10.f, 10.f);
        AirbrushField f;
        CHECK(run(r, f) == long(f.size()));
        CHECK(f.slots() == std::vector<int>{ 2, 3 });
        const Weights a = weights_at(f, Vec3f(1.f, 10.f, 10.f), 4);
        CHECK(a[0] == 255);
        CHECK(a[1] == 0);
        const Weights m = weights_at(f, Vec3f(10.f, 10.f, 10.f), 4);
        CHECK(std::abs(int(m[0]) - 128) <= 6);
        CHECK(std::abs(int(m[1]) - 128) <= 6);
        CHECK(int(m[0]) + int(m[1]) <= 255);
        const Weights b = weights_at(f, Vec3f(19.f, 10.f, 10.f), 4);
        CHECK(b[1] == 255);
        // The recipe travels with the field.
        AirbrushFillRecipe stored;
        REQUIRE(AirbrushFillRecipe::from_string(f.fill_recipe(), stored));
        CHECK(stored.kind == FillKind::Linear);
        auto back = AirbrushField::from_blob(f.to_blob());
        REQUIRE(back);
        CHECK(back->fill_recipe() == f.fill_recipe());
        CHECK(*back == f);
        // A degenerate drag is refused and changes nothing.
        AirbrushFillRecipe dot = r;
        dot.end                = dot.start;
        AirbrushField empty;
        std::string   error;
        CHECK(apply_fill(empty, surface, dot, nullptr, &error) == -1);
        CHECK_FALSE(error.empty());
        CHECK(empty.empty());
    }
    SECTION("along Z: the part colour at the bottom, a filament at the top")
    {
        AirbrushFillRecipe r;
        r.kind  = FillKind::AlongZ;
        r.stops = two_stops(0, 2);
        r.start = Vec3f(10.f, 0.f, 0.f);
        r.end   = Vec3f(10.f, 0.f, 10.f);
        AirbrushField f;
        run(r, f);
        CHECK(f.slots() == std::vector<int>{ 2 });
        CHECK(weights_at(f, Vec3f(10.f, 0.f, 1.f), 3)[0] < 40);
        CHECK(weights_at(f, Vec3f(10.f, 0.f, 9.f), 3)[0] > 215);
        // A click without a drag grades over the part's whole height.
        AirbrushFillRecipe click = r;
        click.end                = click.start;
        AirbrushField f2;
        run(click, f2);
        REQUIRE(f2.size() == f.size());
        for (const auto &kv : f.cells()) {
            const AirbrushField::Cell *c = f2.find(kv.first);
            REQUIRE(c);
            CHECK(c->w == kv.second.w);
        }
    }
    SECTION("radial over the surface reaches round the edge")
    {
        AirbrushFillRecipe r;
        r.kind  = FillKind::Radial;
        r.stops = two_stops(2, 0);
        r.start = Vec3f(10.f, 10.f, 10.f);
        r.end   = Vec3f(10.f, 22.f, 10.f);   // radius 12
        AirbrushField f;
        run(r, f);
        CHECK(weights_at(f, Vec3f(10.f, 10.f, 10.f), 4)[0] > 245);
        // 10 on the top + 1 down the side = 11 of 12 over the surface (straight: 10.05).
        const int over_edge = weights_at(f, Vec3f(10.f, 20.f, 9.f), 2)[0];
        CHECK(over_edge > 10);
        CHECK(over_edge < 40);
        CHECK(weights_at(f, Vec3f(10.f, 20.f, 5.f), 2)[0] == 0);
    }
    SECTION("camo: hard bands, one filament per cell")
    {
        AirbrushFillRecipe r;
        r.kind     = FillKind::Camo;
        r.stops    = even_stops({ 2, 3, 4 });
        r.scale_mm = 6.f;
        r.seed     = 42;
        AirbrushField f;
        run(r, f);
        CHECK(f.slots().size() == 3);
        std::array<size_t, 3> per {};
        for (const auto &kv : f.cells()) {
            int nonzero = 0;
            for (size_t s = 0; s < 3; ++s)
                if (kv.second.w[s] != 0) {
                    ++nonzero;
                    CHECK(kv.second.w[s] == 255);
                    ++per[s];
                }
            CHECK(nonzero == 1);
        }
        // More than one band shows on a 20 x 20 x 10 box at a 6 mm scale.
        CHECK(int(per[0] > 0) + int(per[1] > 0) + int(per[2] > 0) >= 2);
    }
    SECTION("patterns are deterministic and seeded")
    {
        for (FillKind k : { FillKind::Noise, FillKind::Speckle, FillKind::Camo, FillKind::Marble }) {
            AirbrushFillRecipe r;
            r.kind     = k;
            r.stops    = two_stops(2, 3);
            r.scale_mm = 4.f;
            r.seed     = 9;
            AirbrushField a, b, c;
            run(r, a);
            run(r, b);
            CHECK(a.to_blob() == b.to_blob());
            r.seed = 10;
            run(r, c);
            CHECK_FALSE(c == a);
        }
    }
    SECTION("a fill at partial strength glazes over what is there")
    {
        AirbrushFillRecipe base;
        base.kind  = FillKind::Linear;
        base.stops = two_stops(2, 2);
        base.start = Vec3f(0.f, 0.f, 0.f);
        base.end   = Vec3f(20.f, 0.f, 0.f);
        AirbrushField f;
        run(base, f);
        AirbrushFillRecipe glaze = base;
        glaze.stops              = two_stops(3, 3);
        glaze.opacity            = 0.5f;
        run(glaze, f);
        const Weights w = weights_at(f, Vec3f(10.f, 10.f, 10.f), 4);
        CHECK(std::abs(int(w[0]) - 128) <= 2);
        CHECK(std::abs(int(w[1]) - 128) <= 2);
        CHECK(int(w[0]) + int(w[1]) <= 255);
    }
    SECTION("a gradient that needs a ninth filament is refused")
    {
        AirbrushField f;
        for (int id = 1; id <= int(AIRBRUSH_MAX_SLOTS); ++id)
            REQUIRE(f.add_slot(id) >= 0);
        AirbrushFillRecipe r;
        r.kind  = FillKind::Noise;
        r.stops = two_stops(1, 12);
        std::string error;
        CHECK(apply_fill(f, surface, r, nullptr, &error) == -1);
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("Airbrush P3: a gradient bakes like a stroke, one commit, with a cost", "[Airbrush][AirbrushP3]")
{
    Part part;
    ModelVolume       &vol = *part.vol;
    AirbrushSurface    surface(vol.mesh().its);
    auto               field = std::make_shared<AirbrushField>();
    AirbrushFillRecipe r;
    r.kind  = FillKind::AlongZ;
    r.stops = two_stops(0, 2);
    r.start = Vec3f(0.f, 0.f, 2.f);
    r.end   = Vec3f(0.f, 0.f, 8.f);
    REQUIRE(apply_fill(*field, surface, r) > 0);
    BakeJob job;
    job.mesh    = vol.mesh_ptr();
    job.field   = field;
    job.current = vol.mmu_segmentation_facets.get_data();
    job.params  = plain_params();
    const BakeJobResult res = run_bake_job(job);
    REQUIRE(res.status == BakeJobResult::Status::Ok);
    commit_bake(vol, res);
    CHECK(airbrush_paint_in_sync(vol));
    CHECK(res.stats.leaves_airbrushed > 0);
    CHECK_FALSE(vol.airbrush_field->fill_recipe().empty());
    AirbrushCostParams cp;
    cp.layer_height       = 0.2;
    cp.first_layer_height = 0.2;
    cp.base_filament      = 1;
    const AirbrushCost cost = airbrush_estimate_cost(vol.mesh().its, vol.mmu_segmentation_facets.get_data(), cp);
    CHECK(cost.swaps > 0);
    // Deterministic end to end.
    const BakeJobResult again = run_bake_job(job);
    CHECK(again.painting == res.painting);
}

// =============================================================================================
// Hand test 2026-10-31: the radial gradient, the brush area
// =============================================================================================

namespace {

// The same triangles with every vertex repeated per facet (an unwelded mesh: seams everywhere).
indexed_triangle_set unweld(const indexed_triangle_set &its)
{
    indexed_triangle_set out;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const int b = int(out.vertices.size());
        for (int k = 0; k < 3; ++k)
            out.vertices.push_back(its.vertices[size_t(f[k])]);
        out.indices.emplace_back(b, b + 1, b + 2);
    }
    return out;
}

// The weight of `slot` at a point of the sphere of radius `R` at polar angle `theta` (degrees),
// in the plane y = 0.
int sphere_weight(const AirbrushField &f, float R, float theta_deg, size_t slot)
{
    const float th = theta_deg * float(PI) / 180.f;
    const Vec3f p(R * std::sin(th), 0.f, R * std::cos(th));
    Weights     w {};
    f.sample(p.cast<double>(), surface_class(Vec3f(p.normalized())), w);
    return w[slot];
}

} // namespace

TEST_CASE("Airbrush P3: a radial gradient grades monotonically with surface distance", "[Airbrush][AirbrushP3]")
{
    const float                R      = 10.f;
    const indexed_triangle_set sphere = its_make_sphere(R, PI / 60.);
    // The owner's stops: filament 2 up to 39%, filament 3 from 59%; the drag ends on the equator.
    AirbrushFillRecipe r;
    r.kind  = FillKind::Radial;
    r.stops = { { 2, 0.39f }, { 3, 0.59f } };
    r.start = Vec3f(0.f, 0.f, R);
    r.end   = Vec3f(R, 0.f, 0.f);

    AirbrushField welded;
    {
        AirbrushSurface surface(sphere);
        // The radius is the surface distance of the drag's end (a quarter circle), not the chord.
        AirbrushField::FillFunction fn;
        AirbrushField               tmp;
        float                       radius = 0.f;
        REQUIRE(make_fill_function(tmp, sphere, r, fn, nullptr, &radius));
        CHECK_THAT(double(radius), WithinAbs(double(R) * PI / 2., 0.08 * double(R) * PI / 2.));
        REQUIRE(apply_fill(welded, surface, r) > 0);
    }
    REQUIRE(welded.slots() == std::vector<int>{ 2, 3 });
    int prev3 = -1;
    for (float th = 2.f; th <= 170.f; th += 2.f) {
        const int w2 = sphere_weight(welded, R, th, 0);
        const int w3 = sphere_weight(welded, R, th, 1);
        INFO("theta " << th << " w2 " << w2 << " w3 " << w3);
        // Filament 3 only grows outwards, filament 2 only shrinks, and together they cover the cell.
        CHECK(w3 + 3 >= prev3);
        prev3 = std::max(prev3, w3);
        CHECK(w2 + w3 >= 250);
        // Solid filament 2 well inside the first stop, solid filament 3 well past the last one.
        if (th <= 0.39f * 90.f - 6.f)
            CHECK(w3 == 0);
        if (th >= 0.59f * 90.f + 6.f)
            CHECK(w3 == 255);
    }
    // A blend between the stops, not a hard step.
    const int mid = sphere_weight(welded, R, 0.49f * 90.f, 1);
    CHECK(mid > 60);
    CHECK(mid < 195);

    SECTION("an unwelded mesh (seams everywhere) gives the same field")
    {
        const indexed_triangle_set loose = unweld(sphere);
        AirbrushSurface            surface(loose);
        AirbrushField              f;
        REQUIRE(apply_fill(f, surface, r) > 0);
        REQUIRE(f.size() == welded.size());
        // Equal up to rounding (ties in the distance search may be walked in another order).
        size_t differ = 0;
        for (const auto &kv : welded.cells()) {
            const AirbrushField::Cell *c = f.find(kv.first);
            bool                       d = c == nullptr;
            for (size_t k = 0; !d && k < AIRBRUSH_MAX_SLOTS; ++k)
                d = std::abs(int(c->w[k]) - int(kv.second.w[k])) > 2;
            differ += d;
        }
        CHECK(differ == 0);
    }
    SECTION("the bake keeps the order: filament 3's share of the paint grows with the distance")
    {
        Model        model;
        ModelObject *obj = model.add_object();
        obj->config.set("extruder", 1);
        ModelVolume *vol = obj->add_volume(TriangleMesh(sphere), ModelVolumeType::MODEL_PART, false);
        obj->add_instance();
        vol->airbrush_field = std::make_shared<AirbrushField>(welded);
        AirbrushBakeParams bp;
        bp.base_filament = 1;
        bp.mix_steps     = 0;
        bp.seed          = 2;
        bp.grain_mm      = 0.6f;
        REQUIRE(airbrush_bake_volume(*vol, bp));
        TriangleSelector sel(vol->mesh());
        sel.deserialize(vol->mmu_segmentation_facets.get_data(), true);
        // Painted area per 10 degree ring, per filament.
        std::array<double, 18> a2 {}, a3 {};
        auto add = [&](EnforcerBlockerType st, std::array<double, 18> &acc) {
            const indexed_triangle_set t = sel.get_facets(st);
            for (const stl_triangle_vertex_indices &f : t.indices) {
                const Vec3f a = t.vertices[size_t(f[0])], b = t.vertices[size_t(f[1])], c = t.vertices[size_t(f[2])];
                const Vec3f m = (a + b + c) / 3.f;
                const float th = std::acos(std::clamp(m.normalized().z(), -1.f, 1.f)) * 180.f / float(PI);
                acc[std::min<size_t>(17, size_t(th / 10.f))] += 0.5 * double((b - a).cross(c - a).norm());
            }
        };
        add(EnforcerBlockerType::Extruder2, a2);
        add(EnforcerBlockerType::Extruder3, a3);
        double prev = -1.;
        for (size_t ring = 0; ring < 18; ++ring) {
            const double tot = a2[ring] + a3[ring];
            if (tot <= 0.)
                continue;
            const double share = a3[ring] / tot;
            INFO("ring " << ring << " share of filament 3 " << share);
            CHECK(share + 0.08 >= prev);
            prev = std::max(prev, share);
        }
        CHECK(a3[1] < 0.05 * (a2[1] + a3[1]));   // 10-20 degrees: filament 2
        CHECK(a2[8] < 0.05 * (a2[8] + a3[8]));   // 80-90 degrees: filament 3
    }
}

TEST_CASE("Airbrush P3: the brush area limits a gradient or pattern", "[Airbrush][AirbrushP3]")
{
    const indexed_triangle_set its = p3_box(20, 20, 10, 1.);
    AirbrushSurface            surface(its);

    SECTION("a pattern clicked on the top stays inside the brush radius, on the side facing the viewer")
    {
        AirbrushFillRecipe r;
        r.kind         = FillKind::Noise;
        r.stops        = two_stops(2, 3);
        r.area         = AirbrushFillRecipe::Area::Brush;
        r.start = r.end = Vec3f(10.f, 10.f, 10.f);
        r.brush_radius = 4.f;
        r.hardness     = 1.f;
        r.facing       = Vec3f::UnitZ();
        AirbrushField f;
        REQUIRE(apply_fill(f, surface, r) > 0);
        for (const auto &kv : f.cells()) {
            const Vec3d c = f.center_of(kv.first);
            CHECK((c - Vec3d(10., 10., 10.)).norm() <= 4.3);
            CHECK(c.z() > 9.5);   // only the top faces the viewer
        }
        // A full-strength cell in the middle.
        const Weights w = weights_at(f, Vec3f(10.f, 10.f, 10.f), 4);
        CHECK(int(w[0]) + int(w[1]) >= 250);
        // The recipe keeps the area.
        AirbrushFillRecipe back;
        REQUIRE(AirbrushFillRecipe::from_string(f.fill_recipe(), back));
        CHECK(back.area == AirbrushFillRecipe::Area::Brush);
        CHECK(back.brush_radius == 4.f);
        CHECK(back == r);
    }
    SECTION("a soft brush fades the rim")
    {
        AirbrushFillRecipe r;
        r.kind         = FillKind::Linear;
        r.stops        = two_stops(2, 2);
        r.area         = AirbrushFillRecipe::Area::Brush;
        r.start        = Vec3f(4.f, 10.f, 10.f);
        r.end          = Vec3f(16.f, 10.f, 10.f);
        r.brush_radius = 4.f;
        r.hardness     = 0.f;
        r.facing       = Vec3f::UnitZ();
        AirbrushField f;
        REQUIRE(apply_fill(f, surface, r) > 0);
        const int on_path = weights_at(f, Vec3f(10.f, 10.f, 10.f), 4)[0];
        const int half    = weights_at(f, Vec3f(10.f, 12.f, 10.f), 4)[0];
        const int outside = weights_at(f, Vec3f(10.f, 15.f, 10.f), 4)[0];
        CHECK(on_path > 240);
        CHECK(half > 60);
        CHECK(half < 200);
        CHECK(outside == 0);
        // Beyond the ends of the drag only the round caps get it.
        CHECK(weights_at(f, Vec3f(0.5f, 10.f, 10.f), 4)[0] < 40);
    }
    SECTION("a radial gradient in the brush area stays inside its circle")
    {
        AirbrushFillRecipe r;
        r.kind     = FillKind::Radial;
        r.stops    = two_stops(2, 3);
        r.area     = AirbrushFillRecipe::Area::Brush;
        r.start    = Vec3f(10.f, 10.f, 10.f);
        r.end      = Vec3f(14.f, 10.f, 10.f);
        r.hardness = 1.f;
        AirbrushField f;
        REQUIRE(apply_fill(f, surface, r) > 0);
        for (const auto &kv : f.cells())
            CHECK((f.center_of(kv.first) - Vec3d(10., 10., 10.)).norm() <= 4.3);
        CHECK(weights_at(f, Vec3f(10.f, 10.f, 10.f), 4)[0] > 240);
        CHECK(weights_at(f, Vec3f(13.8f, 10.f, 10.f), 4)[1] > 200);
    }
    SECTION("the whole part is still the whole part")
    {
        AirbrushFillRecipe r;
        r.kind     = FillKind::Noise;
        r.stops    = two_stops(2, 3);
        r.area     = AirbrushFillRecipe::Area::WholePart;
        r.start    = Vec3f(10.f, 10.f, 10.f);
        AirbrushField brush, whole;
        REQUIRE(apply_fill(whole, surface, r) > 0);
        r.area         = AirbrushFillRecipe::Area::Brush;
        r.end          = r.start;
        r.brush_radius = 4.f;
        REQUIRE(apply_fill(brush, surface, r) > 0);
        CHECK(whole.size() > 10 * brush.size());
    }
}

// =============================================================================================
// The owner's hand-test projects (hidden): AIRBRUSH_P3_OUT=<directory>
// =============================================================================================

namespace {

struct SampleObject
{
    std::string                  name;
    double                       size = 30.;
    int                          base = 1;
    AirbrushFillRecipe           recipe;   // in print mm; start / end relative to the box's min corner
    AirbrushBakeParams           params;
};

void write_sample(const std::string &out_path, const std::string &printer, const std::string &process, const std::string &filament,
                  const std::vector<std::string> &colours, float nozzle, const std::vector<SampleObject> &objects)
{
    namespace fs = boost::filesystem;
    const std::string saved_data_dir = data_dir();
    const fs::path    scratch        = fs::temp_directory_path() / ("airbrush_p3_" + std::to_string(get_current_pid()));
    fs::create_directories(scratch / "datadir");
    set_data_dir((scratch / "datadir").string());
    set_temporary_dir(scratch.string());

    const std::string profiles = (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    PresetBundle      library;
    library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles, "Elegoo", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
    REQUIRE(bundle.filaments.select_preset_by_name(filament, true));
    bundle.filament_presets = { filament };
    bundle.set_num_filaments(unsigned(colours.size()), colours);
    bundle.filament_presets = std::vector<std::string>(colours.size(), filament);
    DynamicPrintConfig cfg = bundle.full_config_secure();
    cfg.option<ConfigOptionStrings>("filament_colour", true)->values = colours;

    Model                model;
    const BoundingBoxf   bed(cfg.option<ConfigOptionPoints>("printable_area")->values);
    double               x = bed.center().x() - 0.5 * (objects.size() - 1) * 45.;
    for (const SampleObject &so : objects) {
        ModelObject *obj = model.add_object();
        obj->name        = so.name;
        obj->config.set("extruder", so.base);
        ModelVolume *vol = obj->add_volume(TriangleMesh(p3_box(so.size, so.size, so.size, 1.)), ModelVolumeType::MODEL_PART, false);
        vol->name        = so.name;
        obj->add_instance();
        auto            field = std::make_shared<AirbrushField>();
        AirbrushSurface surface(vol->mesh().its);
        std::string     error;
        INFO(so.name << ": " << error);
        REQUIRE(apply_fill(*field, surface, so.recipe, nullptr, &error) > 0);
        vol->airbrush_field     = field;
        AirbrushBakeParams bp   = so.params;
        bp.base_filament        = so.base;
        airbrush_apply_nozzle_floors(bp, airbrush_design_nozzle(bp, nozzle));
        AirbrushBakeStats st;
        REQUIRE(airbrush_bake_volume(*vol, bp, {}, &st));
        CHECK(airbrush_paint_in_sync(*vol));
        WARN(so.name << ": " << st.leaves_speck << " speck leaves, grain walls " << st.grain_wall_mm << " mm, tops " << st.grain_top_mm
                     << " mm" << (st.top_tone_fallback ? " (tone)" : ""));
        obj->instances.front()->set_offset(Vec3d(x, bed.center().y(), 0.));
        obj->ensure_on_bed();
        x += 45.;
    }

    PlateDataPtrs plates;
    PlateData    *pd = new PlateData();
    pd->plate_index  = 0;
    pd->plate_name   = "airbrush P3";
    for (int i = 0; i < int(objects.size()); ++i)
        pd->objects_and_instances.emplace_back(i, 0);
    plates.push_back(pd);
    StoreParams sp;
    sp.path            = out_path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = plates;
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored  = store_bbs_3mf(sp);
    release_PlateData_list(plates);
    REQUIRE(stored);
    WARN("wrote " << out_path);
    set_data_dir(saved_data_dir);
    boost::system::error_code ec;
    fs::remove_all(scratch, ec);
}

} // namespace

TEST_CASE("Airbrush P3: write the hand-test projects", "[.][AirbrushP3Samples]")
{
    const char *out_env = std::getenv("AIRBRUSH_P3_OUT");
    REQUIRE(out_env != nullptr);
    const std::string dir = out_env;

    // 1. Gradient + camo on the owner's CC1 (0.4 nozzle): white, blue, olive, brown.
    {
        SampleObject grad;
        grad.name         = "gradient_along_z";
        grad.base         = 1;
        grad.recipe.kind  = FillKind::AlongZ;
        grad.recipe.stops = { { 0, 0.f }, { 2, 0.5f }, { 3, 1.f } };
        grad.recipe.start = Vec3f(0.f, 0.f, 4.f);
        grad.recipe.end   = Vec3f(0.f, 0.f, 26.f);
        grad.params.seed  = 3;
        SampleObject camo;
        camo.name            = "camo";
        camo.base            = 3;
        camo.recipe.kind     = FillKind::Camo;
        camo.recipe.stops    = even_stops({ 3, 4, 1 });
        camo.recipe.scale_mm = 9.f;
        camo.recipe.seed     = 21;
        camo.params.seed     = 4;
        write_sample(dir + "/airbrush_p3_gradient_camo.3mf", "Elegoo Centauri Carbon 0.4 nozzle", "0.12mm Fine @Elegoo CC 0.4 nozzle",
                     "Elegoo PLA @ECC", { "#FFFFFF", "#2F6FD0", "#5B6B2E", "#6B4A2B" }, 0.4f, { grad, camo });
    }
    // 2. Fine grain for a 0.2 mm nozzle: noise at 0.30 mm (the 0.2 nozzle's wall floor) and at 0.15 mm
    //    (below it: the panel and the slicer warn).
    {
        SampleObject fine;
        fine.name                    = "fine_0.30mm";
        fine.size                    = 20.;
        fine.base                    = 1;
        fine.recipe.kind             = FillKind::Noise;
        fine.recipe.stops            = { { 0, 0.35f }, { 2, 0.65f } };
        fine.recipe.scale_mm         = 2.f;
        fine.recipe.contrast         = 1.5f;
        fine.recipe.seed             = 8;
        fine.params.grain_mm         = 0.3f;
        fine.params.design_nozzle_mm = 0.2f;
        fine.params.seed             = 5;
        SampleObject finer           = fine;
        finer.name                   = "fine_0.15mm";
        finer.params.grain_mm        = 0.15f;
        write_sample(dir + "/airbrush_p3_fine_0p2_nozzle.3mf", "Elegoo Centauri Carbon 0.2 nozzle", "0.10mm Standard @Elegoo CC 0.2 nozzle",
                     "Elegoo PLA @ECC", { "#FFFFFF", "#202020" }, 0.2f, { fine, finer });
    }
}
