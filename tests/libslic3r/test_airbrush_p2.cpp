// Airbrush P2 (tests/design_airbrush_paint.md, section 5 row P2): the non-GUI half of the Airbrush tool
// in the colour gizmo - brush settings, dabs from a held button, the overlay geometry, the mix ladder
// from existing mixed filaments, the background bake job and its single-step commit (undo/redo), and
// the cost meter.
#include <catch2/catch.hpp>

#include "libslic3r/Airbrush/AirbrushBake.hpp"
#include "libslic3r/Airbrush/AirbrushField.hpp"
#include "libslic3r/Airbrush/AirbrushSession.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Airbrush;
using Catch::Matchers::WithinAbs;

namespace {

// A closed box whose faces are regular grids of `step` (shared vertices).
indexed_triangle_set p2_box(double sx, double sy, double sz, double step)
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
    grid(Vec3d(0, 0, sz), Vec3d(sx, 0, 0), Vec3d(0, sy, 0), nx, ny, Vec3d(0, 0, 1));
    grid(Vec3d(0, 0, 0), Vec3d(sx, 0, 0), Vec3d(0, sy, 0), nx, ny, Vec3d(0, 0, -1));
    grid(Vec3d(0, 0, 0), Vec3d(sx, 0, 0), Vec3d(0, 0, sz), nx, nz, Vec3d(0, -1, 0));
    grid(Vec3d(0, sy, 0), Vec3d(sx, 0, 0), Vec3d(0, 0, sz), nx, nz, Vec3d(0, 1, 0));
    grid(Vec3d(0, 0, 0), Vec3d(0, sy, 0), Vec3d(0, 0, sz), ny, nz, Vec3d(-1, 0, 0));
    grid(Vec3d(sx, 0, 0), Vec3d(0, sy, 0), Vec3d(0, 0, sz), ny, nz, Vec3d(1, 0, 0));
    return its;
}

SurfaceHit hit_at(float x, float y, float z) { return SurfaceHit{ Vec3f(x, y, z), Vec3f(0.f, 0.f, 1.f) }; }

// The undo stack's view of an airbrushed volume: what ModelVolume::save() writes for the paint and
// the field (the paint through cereal, the field as its blob), and the way load() reads it back.
struct VolumeArchive
{
    std::string bytes;
};
VolumeArchive save_volume(const ModelVolume &vol)
{
    std::stringstream ss;
    {
        cereal::BinaryOutputArchive oa(ss);
        TriangleSelector::TriangleSplittingData paint = vol.mmu_segmentation_facets.get_data();
        oa(paint);
        const bool has_field = vol.airbrush_field != nullptr;
        oa(has_field);
        if (has_field) {
            const std::string blob = vol.airbrush_field->to_blob();
            oa(blob);
        }
    }
    return { ss.str() };
}
void load_volume(ModelVolume &vol, const VolumeArchive &a)
{
    std::stringstream ss(a.bytes);
    cereal::BinaryInputArchive ia(ss);
    TriangleSelector::TriangleSplittingData paint;
    ia(paint);
    vol.mmu_segmentation_facets.set_data(std::move(paint));
    bool has_field = false;
    ia(has_field);
    if (has_field) {
        std::string blob;
        ia(blob);
        vol.airbrush_field = AirbrushField::from_blob(blob);
    } else
        vol.airbrush_field.reset();
}

// A hand-held stroke: a press at `from`, a drag to `to` in small moves, and `hold_s` seconds held at
// the end, at 60 timer ticks a second.
void stroke(AirbrushField &field, const AirbrushSurface &surface, int slot, const Vec3f &from, const Vec3f &to, float radius, float flow,
            double hold_s, std::vector<uint64_t> *changed = nullptr)
{
    StrokeParams sp;
    sp.slot = slot;
    field.begin_stroke(sp);
    DabEmitter emitter;
    auto lay = [&](const std::vector<SurfaceHit> &hits) {
        for (const SurfaceHit &h : hits) {
            Dab d;
            d.center   = h.pos;
            d.radius   = radius;
            d.hardness = 0.5f;
            d.flow     = flow;
            d.normal   = h.facing;
            field.dab(surface, d, changed);
        }
    };
    double t = 0.;
    lay(emitter.press(SurfaceHit{ from, Vec3f(0.f, 0.f, 1.f) }, t, radius));
    const int moves = 20;
    for (int i = 1; i <= moves; ++i) {
        t += 1. / 60.;
        lay(emitter.move(SurfaceHit{ from + (to - from) * (float(i) / float(moves)), Vec3f(0.f, 0.f, 1.f) }, t, radius));
    }
    for (double h = 0.; h < hold_s; h += 1. / 60.) {
        t += 1. / 60.;
        lay(emitter.hold(t));
    }
    emitter.release();
    field.end_stroke();
}

// A Model with one 20 x 20 x 10 box part on filament 1, and its mesh in mesh space.
struct OnePart
{
    Model        model;
    ModelObject *obj = nullptr;
    ModelVolume *vol = nullptr;
    OnePart(double step = 1.)
    {
        obj = model.add_object();
        obj->config.set("extruder", 1);
        vol = obj->add_volume(TriangleMesh(p2_box(20, 20, 10, step)));
        obj->add_instance();
    }
    const indexed_triangle_set &its() const { return vol->mesh().its; }
    Vec3f top_center() const
    {
        const BoundingBoxf3 bb = bounding_box(its());
        return Vec3f(float(bb.center().x()), float(bb.center().y()), float(bb.max.z()));
    }
};

BakeJob job_for(const ModelVolume &vol, const AirbrushField &field, const AirbrushBakeParams &params)
{
    BakeJob job;
    job.mesh    = vol.mesh_ptr();
    job.field   = std::make_shared<const AirbrushField>(field);
    job.current = vol.mmu_segmentation_facets.get_data();
    job.params  = params;
    return job;
}

uint32_t coverage_at(const AirbrushField &f, const Vec3f &p, int cls, size_t slot)
{
    Weights w;
    f.sample(p.cast<double>(), cls, w);
    return w[slot];
}

} // namespace

// =============================================================================================

TEST_CASE("Airbrush P2: brush settings round trip and clamp", "[Airbrush][AirbrushP2]")
{
    BrushSettings b;
    b.radius_mm = 7.5f;
    b.hardness  = 0.8f;
    b.flow      = 0.4f;
    b.grain_mm  = 1.25f;
    b.mix_steps = false;
    b.seed      = 4242;
    const BrushSettings back = BrushSettings::from_string(b.to_string());
    CHECK(back == b);
    CHECK(back.seed == 4242);
    CHECK_FALSE(back.mix_steps);

    SECTION("out of range and garbage values are clamped or ignored")
    {
        const BrushSettings c = BrushSettings::from_string("radius=100;hardness=-3;flow=0;grain=0.2;mix=1;seed=7;bogus=9;flow");
        CHECK(c.radius_mm == AIRBRUSH_RADIUS_MAX);
        CHECK(c.hardness == 0.f);
        CHECK(c.flow == AIRBRUSH_FLOW_MIN);
        // The floor is the wall floor (0.6 mm, P0); tops are the bake's business.
        CHECK(c.grain_mm == AIRBRUSH_MIN_GRAIN_WALL_MM);
        CHECK(c.mix_steps);
        CHECK(c.seed == 7);
        BrushSettings n;
        n.radius_mm = std::numeric_limits<float>::quiet_NaN();
        n.sanitize();
        CHECK(n.radius_mm == AIRBRUSH_RADIUS_DEFAULT);
        CHECK(BrushSettings::from_string("") == BrushSettings());
    }
    SECTION("defaults are the P0 ones")
    {
        const BrushSettings d;
        CHECK(d.grain_mm == AIRBRUSH_DEFAULT_GRAIN_MM);
        CHECK(d.grain_mm == 1.0f);
        CHECK(AIRBRUSH_MIN_GRAIN_WALL_MM == 0.6f);
        CHECK(AIRBRUSH_MIN_GRAIN_TOP_MM == 0.8f);
        CHECK(AIRBRUSH_TONE_COVERAGE == 0.35f);
    }
    SECTION("bake parameters")
    {
        AirbrushBakeParams p = bake_params_from(b, 3);
        CHECK(p.grain_mm == 1.25f);
        CHECK(p.mix_steps == 0);
        CHECK(p.seed == 4242);
        CHECK(p.base_filament == 3);
        CHECK(p.min_grain_top_mm == AIRBRUSH_MIN_GRAIN_TOP_MM);
        b.mix_steps = true;
        CHECK(bake_params_from(b, 1).mix_steps == AIRBRUSH_DEFAULT_MIX_STEPS);
    }
}

TEST_CASE("Airbrush P2: dabs from a held button", "[Airbrush][AirbrushP2]")
{
    const float radius = 4.f;   // drag dabs every 1 mm
    DabEmitter  e;
    SECTION("nothing before the press")
    {
        CHECK(e.move(hit_at(5, 0, 0), 0.1, radius).empty());
        CHECK(e.hold(10.).empty());
    }
    SECTION("the press lays one dab where it landed")
    {
        const auto d = e.press(hit_at(1, 2, 3), 0., radius);
        REQUIRE(d.size() == 1);
        CHECK(d[0].pos == Vec3f(1, 2, 3));
        CHECK(e.active());
    }
    SECTION("a drag lays a dab every spacing, however the moves are cut")
    {
        e.press(hit_at(0, 0, 0), 0., radius);
        const auto one = e.move(hit_at(10, 0, 0), 0.01, radius);
        REQUIRE(one.size() == 10);
        for (size_t i = 0; i < one.size(); ++i)
            CHECK_THAT(one[i].pos.x(), WithinAbs(double(i + 1), 1e-4));

        DabEmitter            f;
        std::vector<SurfaceHit> all;
        f.press(hit_at(0, 0, 0), 0., radius);
        for (int i = 1; i <= 33; ++i) {
            const auto d = f.move(hit_at(0.3f * float(i), 0, 0), 0.001 * i, radius);
            all.insert(all.end(), d.begin(), d.end());
        }
        // 9.9 mm in 0.3 mm moves: dabs at 1..9 mm.
        REQUIRE(all.size() == 9);
        for (size_t i = 0; i < all.size(); ++i)
            CHECK_THAT(all[i].pos.x(), WithinAbs(double(i + 1), 1e-4));
        CHECK(f.emitted() == 10);
    }
    SECTION("holding still sprays at the dab rate")
    {
        e.press(hit_at(0, 0, 0), 0., radius);
        size_t n = 0;
        for (int tick = 1; tick <= 60; ++tick)
            n += e.hold(double(tick) / 60.).size();
        // One second at 15 Hz.
        CHECK(n >= 14);
        CHECK(n <= 15);
        CHECK(e.hold(1.0).empty());
    }
    SECTION("a stalled UI gets a short burst, not the backlog")
    {
        e.press(hit_at(0, 0, 0), 0., radius);
        CHECK(e.hold(10.).size() == 4);
        CHECK(e.hold(10.01).empty());
        CHECK(e.hold(10. + 1. / 15. + 1e-3).size() == 1);
    }
    SECTION("moving resets the dwell clock")
    {
        e.press(hit_at(0, 0, 0), 0., radius);
        CHECK(e.move(hit_at(1, 0, 0), 0.5, radius).size() == 1);
        CHECK(e.hold(0.55).empty());
        CHECK(e.hold(0.5 + 1. / 15. + 1e-3).size() == 1);
    }
    SECTION("a jump across a gap is not filled in")
    {
        e.press(hit_at(0, 0, 0), 0., radius);
        const auto d = e.move(hit_at(100, 0, 0), 0.1, radius);
        REQUIRE(d.size() == 1);
        CHECK(d[0].pos == Vec3f(100, 0, 0));
        CHECK(e.move(hit_at(100.5f, 0, 0), 0.2, radius).empty());
    }
    SECTION("facing is interpolated along the drag and stays unit length")
    {
        e.press(SurfaceHit{ Vec3f(0, 0, 0), Vec3f(0, 0, 1) }, 0., radius);
        const auto d = e.move(SurfaceHit{ Vec3f(2, 0, 0), Vec3f(1, 0, 0) }, 0.1, radius);
        REQUIRE(d.size() == 2);
        CHECK_THAT(double(d[0].facing.norm()), WithinAbs(1., 1e-5));
        CHECK(d[0].facing.x() > 0.f);
        CHECK(d[0].facing.z() > 0.f);
        CHECK(d[1].facing.isApprox(Vec3f(1, 0, 0)));
    }
}

TEST_CASE("Airbrush P2: held strokes build up in the field", "[Airbrush][AirbrushP2]")
{
    const indexed_triangle_set its = p2_box(20, 20, 10, 1.);
    AirbrushSurface            surface(its);
    const Vec3f                c(10.f, 10.f, 10.f);   // the middle of the top
    const int                  top = 4;

    AirbrushField a;
    a.add_slot(2);
    stroke(a, surface, 0, c, c, 3.f, 0.2f, 0.);
    const uint32_t tap = coverage_at(a, c, top, 0);
    AirbrushField b;
    b.add_slot(2);
    stroke(b, surface, 0, c, c, 3.f, 0.2f, 1.);
    const uint32_t held = coverage_at(b, c, top, 0);
    AirbrushField d;
    d.add_slot(2);
    stroke(d, surface, 0, c, c, 3.f, 0.2f, 3.);
    const uint32_t long_held = coverage_at(d, c, top, 0);
    CHECK(tap > 0);
    CHECK(held > tap);
    CHECK(long_held >= held);
    CHECK(long_held >= 250);   // about 45 dabs of 20%: practically full

    SECTION("the changed keys are what the dab changed, in order")
    {
        AirbrushField f;
        f.add_slot(2);
        StrokeParams sp;
        f.begin_stroke(sp);
        Dab dab;
        dab.center = c;
        dab.radius = 2.f;
        dab.flow   = 0.5f;
        std::vector<uint64_t> keys;
        const size_t          n = f.dab(surface, dab, &keys);
        CHECK(n == keys.size());
        CHECK(n > 0);
        CHECK(std::is_sorted(keys.begin(), keys.end()));
        CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
        for (uint64_t k : keys)
            CHECK(f.find(k) != nullptr);
        // The same dab again builds the same cells up; rim cells that rounded to nothing the first time
        // may appear now. Every reported key is in the field, and the first dab's cells all grew.
        std::vector<uint64_t> again;
        f.dab(surface, dab, &again);
        CHECK(again.size() >= keys.size() / 2);
        for (uint64_t k : again)
            CHECK(f.find(k) != nullptr);
        size_t grown = 0;
        for (uint64_t k : keys)
            grown += std::binary_search(again.begin(), again.end(), k) ? 1 : 0;
        CHECK(grown * 10 >= keys.size() * 9);
        f.end_stroke();
    }
    SECTION("a drag stroke covers its path and nothing far from it")
    {
        AirbrushField f;
        f.add_slot(2);
        stroke(f, surface, 0, Vec3f(4, 10, 10), Vec3f(16, 10, 10), 2.f, 0.3f, 0.);
        for (float x = 4.f; x <= 16.f; x += 1.f)
            CHECK(coverage_at(f, Vec3f(x, 10, 10), top, 0) > 0);
        CHECK(coverage_at(f, Vec3f(10, 16, 10), top, 0) == 0);
        CHECK(coverage_at(f, Vec3f(10, 4, 10), top, 0) == 0);
    }
}

TEST_CASE("Airbrush P2: overlay geometry", "[Airbrush][AirbrushP2]")
{
    SECTION("chunks")
    {
        const int n = 1 << AIRBRUSH_OVERLAY_CHUNK_SHIFT;
        CHECK(overlay_chunk_of(AirbrushField::make_key(0, 0, 0, 4)) == overlay_chunk_of(AirbrushField::make_key(n - 1, n - 1, n - 1, 4)));
        CHECK(overlay_chunk_of(AirbrushField::make_key(0, 0, 0, 4)) != overlay_chunk_of(AirbrushField::make_key(n, 0, 0, 4)));
        CHECK(overlay_chunk_of(AirbrushField::make_key(-1, 0, 0, 4)) != overlay_chunk_of(AirbrushField::make_key(0, 0, 0, 4)));
        CHECK(overlay_chunk_of(AirbrushField::make_key(-1, 0, 0, 4)) == overlay_chunk_of(AirbrushField::make_key(-n, 0, 0, 4)));
        CHECK(overlay_chunk_of(AirbrushField::make_key(-n - 1, 0, 0, 4)) != overlay_chunk_of(AirbrushField::make_key(-n, 0, 0, 4)));
        // Classes never share a chunk.
        CHECK(overlay_chunk_of(AirbrushField::make_key(0, 0, 0, 4)) != overlay_chunk_of(AirbrushField::make_key(0, 0, 0, 5)));
    }
    SECTION("style: the dominant slot and the coverage level")
    {
        int    slot = 9, level = 9;
        Weights w {};
        overlay_style(w, 2, slot, level);
        CHECK(slot == -1);
        CHECK(level == 0);
        w[1] = 63;
        overlay_style(w, 2, slot, level);
        CHECK(slot == 1);
        CHECK(level == 1);
        w[0] = 100;
        w[1] = 155;
        overlay_style(w, 2, slot, level);
        CHECK(slot == 1);
        CHECK(level == AIRBRUSH_OVERLAY_LEVELS);
        // Weights beyond the field's slots are not counted.
        Weights x {};
        x[3] = 255;
        overlay_style(x, 2, slot, level);
        CHECK(slot == -1);
    }
    SECTION("quads sit on the outer face of their cells")
    {
        AirbrushField f;
        f.add_slot(2);
        f.add_slot(3);
        Weights full {};
        full[0] = 255;
        Weights half {};
        half[1] = 128;
        const uint64_t k_top  = AirbrushField::make_key(3, 4, 5, 4);   // +Z
        const uint64_t k_side = AirbrushField::make_key(-2, 1, 0, 1);  // -X
        const uint64_t k_gone = AirbrushField::make_key(7, 7, 7, 2);   // erased: background
        f.set_cell(k_top, full);
        f.set_cell(k_side, half);
        const auto buckets = build_overlay_chunk(f, { k_side, k_gone, k_top });
        REQUIRE(buckets.size() == 3);
        // Sorted by (slot, level).
        CHECK(buckets[0].slot == -1);
        CHECK(buckets[0].level == 0);
        CHECK(buckets[1].slot == 0);
        CHECK(buckets[1].level == AIRBRUSH_OVERLAY_LEVELS);
        CHECK(buckets[2].slot == 1);
        CHECK(buckets[2].level == 3);
        const float h = f.cell_size();
        for (const auto &b : buckets)
            REQUIRE(b.corners.size() == 4);
        const Vec3f ct = f.center_of(k_top).cast<float>();
        for (const Vec3f &p : buckets[1].corners) {
            CHECK_THAT(double(p.z()), WithinAbs(double(ct.z() + 0.65f * h), 1e-5));
            CHECK_THAT(double(std::abs(p.x() - ct.x())), WithinAbs(0.5 * h, 1e-5));
            CHECK_THAT(double(std::abs(p.y() - ct.y())), WithinAbs(0.5 * h, 1e-5));
        }
        const Vec3f cs = f.center_of(k_side).cast<float>();
        for (const Vec3f &p : buckets[2].corners)
            CHECK_THAT(double(p.x()), WithinAbs(double(cs.x() - 0.65f * h), 1e-5));
    }
}

TEST_CASE("Airbrush P2: the mix ladder from existing mixed filaments", "[Airbrush][AirbrushP2]")
{
    const std::vector<std::string> colours = { "#FFFFFF", "#3A7BD5" };
    const bool                     saved   = MixedFilamentManager::auto_generate_enabled();
    MixedFilamentManager::set_auto_generate_enabled(true);
    MixedFilamentManager mgr;
    mgr.auto_generate(colours);
    for (MixedFilament &mf : mgr.mixed_filaments()) {
        mf.deleted = true;
        mf.enabled = false;
    }
    // The P1 hand-test project's ladder: 2/8, 4/8, 6/8 of filament 2 over filament 1.
    for (int k : { 2, 4, 6 }) {
        std::string pattern;
        for (int i = 0; i < 8; ++i)
            pattern += ((i + 1) * k / 8 > i * k / 8) ? '2' : '1';
        mgr.add_custom_filament(1, 2, (k * 100 + 4) / 8, colours);
        MixedFilament &mf    = mgr.mixed_filaments().back();
        mf.manual_pattern    = MixedFilamentManager::normalize_manual_pattern(pattern);
        mf.distribution_mode = int(MixedFilament::LayerCycle);
    }
    MixedFilamentManager::set_auto_generate_enabled(saved);

    CHECK(find_ladder_state(mgr, 2, 1, 2, 1, 4) == 3);
    CHECK(find_ladder_state(mgr, 2, 1, 2, 2, 4) == 4);
    CHECK(find_ladder_state(mgr, 2, 1, 2, 3, 4) == 5);
    // The other way round: 25% of 1 over 2 is the 75%-of-2 row.
    CHECK(find_ladder_state(mgr, 2, 2, 1, 1, 4) == 5);
    // No step outside (0, steps), no row for another pair, no 3-step ladder at 33/67%.
    CHECK(find_ladder_state(mgr, 2, 1, 2, 0, 4) == 0);
    CHECK(find_ladder_state(mgr, 2, 1, 2, 4, 4) == 0);
    CHECK(find_ladder_state(mgr, 2, 1, 3, 2, 4) == 0);
    CHECK(find_ladder_state(mgr, 2, 1, 2, 1, 3) == 0);

    const LadderLookup ladder = ladder_from_mixed(mgr, 2);
    REQUIRE(bool(ladder));
    CHECK(ladder(1, 2, 2, 4) == 4);
    // The lookup is a copy: later edits of the project do not reach a running bake.
    mgr.mixed_filaments().clear();
    CHECK(ladder(1, 2, 2, 4) == 4);
    CHECK_FALSE(bool(ladder_from_mixed(mgr, 2)));
}

TEST_CASE("Airbrush P2: bake job, single-step commit and undo/redo", "[Airbrush][AirbrushP2]")
{
    OnePart part;
    ModelVolume &vol = *part.vol;
    AirbrushSurface surface(part.its());
    const Vec3f     c = part.top_center();

    auto working = std::make_shared<AirbrushField>();
    working->add_slot(2);
    stroke(*working, surface, 0, c - Vec3f(4, 0, 0), c + Vec3f(4, 0, 0), 3.f, 0.5f, 0.5);
    AirbrushBakeParams bp;
    bp.base_filament = 1;
    bp.seed          = 11;

    const VolumeArchive before = save_volume(vol);
    REQUIRE(vol.airbrush_field == nullptr);
    REQUIRE(vol.mmu_segmentation_facets.empty());

    BakeJob job = job_for(vol, *working, bp);
    // Strokes after the release do not reach a job already captured.
    stroke(*working, surface, 0, c + Vec3f(0, 5, 0), c + Vec3f(0, 6, 0), 2.f, 0.5f, 0.);
    const BakeJobResult r = run_bake_job(job);
    REQUIRE(r.status == BakeJobResult::Status::Ok);
    REQUIRE(r.field);
    CHECK_FALSE(*r.field == *working);
    CHECK(r.field->size() == job.field->size());
    CHECK(r.field->record().valid());
    CHECK(r.field->record().facets == uint32_t(part.its().indices.size()));
    CHECK(r.field->bake_params() == bp.to_string());
    CHECK_FALSE(r.painting.triangles_to_split.empty());
    CHECK(r.stats.leaves_airbrushed > 0);
    // Nothing reaches the volume until the commit.
    CHECK(vol.airbrush_field == nullptr);
    CHECK(vol.mmu_segmentation_facets.empty());

    // The commit: field and paint as one change.
    commit_bake(vol, r);
    REQUIRE(vol.airbrush_field);
    CHECK(airbrush_paint_in_sync(vol));
    CHECK(vol.mmu_segmentation_facets.get_data() == r.painting);
    const VolumeArchive after = save_volume(vol);

    SECTION("undo restores the volume before the stroke, redo the baked one")
    {
        load_volume(vol, before);
        CHECK(vol.airbrush_field == nullptr);
        CHECK(vol.mmu_segmentation_facets.empty());
        load_volume(vol, after);
        REQUIRE(vol.airbrush_field);
        CHECK(*vol.airbrush_field == *r.field);
        CHECK(vol.mmu_segmentation_facets.get_data() == r.painting);
        CHECK(airbrush_paint_in_sync(vol));
        // A second round trip is byte-identical.
        CHECK(save_volume(vol).bytes == after.bytes);
    }
    SECTION("the same job again bakes the same paint (deterministic, idempotent)")
    {
        const BakeJobResult again = run_bake_job(job_for(vol, *r.field, bp));
        REQUIRE(again.status == BakeJobResult::Status::Ok);
        CHECK(again.painting == r.painting);
        CHECK(again.field->record().paint_hash == r.field->record().paint_hash);
    }
    SECTION("the next stroke bakes on top of the committed record")
    {
        working->record() = r.field->record();
        const BakeJobResult next = run_bake_job(job_for(vol, *working, bp));
        REQUIRE(next.status == BakeJobResult::Status::Ok);
        CHECK(next.painting != r.painting);
        commit_bake(vol, next);
        CHECK(airbrush_paint_in_sync(vol));
    }
    SECTION("paint edited elsewhere stops the job until the user says re-bake")
    {
        TriangleSelector sel(vol.mesh());
        sel.deserialize(vol.mmu_segmentation_facets.get_data(), true);
        sel.set_facet(0, EnforcerBlockerType::Extruder3);
        vol.mmu_segmentation_facets.set(sel);
        REQUIRE_FALSE(airbrush_paint_in_sync(vol));
        const auto edited = vol.mmu_segmentation_facets.get_data();
        BakeJob j = job_for(vol, *vol.airbrush_field, bp);
        const BakeJobResult refused = run_bake_job(j);
        CHECK(refused.status == BakeJobResult::Status::ForeignEdit);
        commit_bake(vol, refused);   // a refused result commits nothing
        CHECK(vol.mmu_segmentation_facets.get_data() == edited);
        j.overwrite = true;
        const BakeJobResult over = run_bake_job(j);
        REQUIRE(over.status == BakeJobResult::Status::Ok);
        commit_bake(vol, over);
        CHECK(airbrush_paint_in_sync(vol));
    }
    SECTION("a record from another mesh is refused, then cleared on request")
    {
        const TriangleMesh other(p2_box(20, 20, 10, 2.));
        BakeJob j = job_for(vol, *vol.airbrush_field, bp);
        j.mesh    = std::make_shared<const TriangleMesh>(other);
        j.current = {};
        CHECK(run_bake_job(j).status == BakeJobResult::Status::StaleRecord);
        j.overwrite = true;
        const BakeJobResult over = run_bake_job(j);
        REQUIRE(over.status == BakeJobResult::Status::Ok);
        CHECK(over.field->record().facets == uint32_t(other.its.indices.size()));
    }
    SECTION("erasing the whole spray restores the paint and drops the field")
    {
        auto erased = std::make_shared<AirbrushField>(*vol.airbrush_field);
        StrokeParams sp;
        sp.slot = -1;
        erased->begin_stroke(sp);
        Dab d;
        d.center   = c;
        d.radius   = 100.f;
        d.hardness = 1.f;
        d.flow     = 1.f;
        erased->dab(surface, d);
        erased->end_stroke();
        REQUIRE(erased->empty());
        const BakeJobResult e = run_bake_job(job_for(vol, *erased, bp));
        REQUIRE(e.status == BakeJobResult::Status::Ok);
        commit_bake(vol, e);
        CHECK(vol.airbrush_field == nullptr);
        CHECK(vol.mmu_segmentation_facets.empty());
    }
    SECTION("a job with nothing to bake fails cleanly")
    {
        BakeJob empty;
        const BakeJobResult f = run_bake_job(empty);
        CHECK(f.status == BakeJobResult::Status::Failed);
        CHECK_FALSE(f.error.empty());
    }
}

TEST_CASE("Airbrush P2: cost meter numbers", "[Airbrush][AirbrushP2]")
{
    AirbrushCostParams cp;
    cp.flush_mm3        = 280.;
    cp.flush_multiplier = 0.3;
    cp.density_g_cm3    = 1.24;
    cp.swap_seconds     = 57.;

    SECTION("swaps from per-layer filaments: each layer starts on what is loaded")
    {
        const AirbrushCost c = airbrush_cost_from_layers({ { 1 }, { 2, 1 }, { 1, 2, 2 }, { 1 } }, cp);
        CHECK(c.layers == 4);
        CHECK(c.multi_tool_layers == 2);
        // 1 | 1 -> 2 | 2 -> 1 | 1
        CHECK(c.swaps == 2);
        CHECK_THAT(c.purge_mm3, WithinAbs(2 * 84., 1e-9));
        CHECK_THAT(c.purge_g, WithinAbs(2 * 84. / 1000. * 1.24, 1e-9));
        CHECK_THAT(c.swap_seconds, WithinAbs(114., 1e-9));
        REQUIRE(c.filaments_per_layer.size() == 4);
        CHECK(c.filaments_per_layer[2] == std::vector<int>{ 1, 2 });
        CHECK(airbrush_cost_from_layers({}, cp).swaps == 0);
        CHECK(airbrush_cost_from_layers({ {}, { 3 }, {} }, cp).swaps == 0);
    }
    SECTION("parts of one object merge per layer")
    {
        AirbrushCost a = airbrush_cost_from_layers({ { 1 }, { 1 }, { 1 } }, cp);
        AirbrushCost b = airbrush_cost_from_layers({ { 2 }, { 2 }, { 2 }, { 2 } }, cp);
        CHECK(a.swaps == 0);
        CHECK(b.swaps == 0);
        const AirbrushCost m = airbrush_merge_costs({ a, b }, cp);
        CHECK(m.layers == 4);
        // 1 -> 2 | 2 -> 1 | 1 -> 2 | 2
        CHECK(m.swaps == 3);
        CHECK(m.multi_tool_layers == 3);
    }
    SECTION("D8 warnings at 50 g and 2 h, never a cap")
    {
        AirbrushCost c;
        c.purge_g      = 49.;
        c.swap_seconds = 7000.;
        CHECK_FALSE(airbrush_cost_warning(c).any());
        c.purge_g = 51.;
        CHECK(airbrush_cost_warning(c).purge);
        CHECK_FALSE(airbrush_cost_warning(c).time);
        c.purge_g      = 1.;
        c.swap_seconds = 7300.;
        CHECK(airbrush_cost_warning(c).time);
    }
    SECTION("the cost job over two parts side by side")
    {
        cp.layer_height       = 0.2;
        cp.first_layer_height = 0.2;
        CostJob job;
        job.params = cp;
        for (int base : { 1, 2 }) {
            CostJob::Part part;
            part.mesh          = std::make_shared<const TriangleMesh>(p2_box(10, 10, 2, 1.));
            part.base_filament = base;
            part.trafo         = Transform3d(Eigen::Translation3d(base == 1 ? 0. : 20., 0., 0.));
            job.parts.push_back(part);
        }
        const CostJobResult r = run_cost_job(job);
        CHECK(r.cost.layers == 10);
        // Both filaments on every layer: one swap per layer after the first filament.
        CHECK(r.cost.swaps == 10);
        CHECK(r.cost.multi_tool_layers == 10);
        CHECK_THAT(r.cost.purge_g, WithinAbs(10 * 84. / 1000. * 1.24, 1e-9));
        CHECK_FALSE(r.warning.any());
        // One part alone: no swaps; the estimate agrees with the per-layer model.
        job.parts.pop_back();
        const CostJobResult one = run_cost_job(job);
        CHECK(one.cost.swaps == 0);
        const AirbrushCost direct = airbrush_estimate_cost(job.parts[0].mesh->its, {}, cp, job.parts[0].trafo);
        CHECK(direct.swaps == one.cost.swaps);
        CHECK(direct.filaments_per_layer == one.cost.filaments_per_layer);
    }
    SECTION("a painted part: the cost job counts the airbrush's layers")
    {
        OnePart part;
        AirbrushSurface surface(part.its());
        AirbrushField   f;
        f.add_slot(2);
        // Spray across the middle of the front wall (y = min), 4 mm tall.
        const BoundingBoxf3 bb = bounding_box(part.its());
        const Vec3f         a(float(bb.min.x() + 4), float(bb.min.y()), float(bb.center().z()));
        const Vec3f         b(float(bb.max.x() - 4), float(bb.min.y()), float(bb.center().z()));
        StrokeParams sp;
        f.begin_stroke(sp);
        for (float x = a.x(); x <= b.x(); x += 0.5f) {
            Dab d;
            d.center   = Vec3f(x, a.y(), a.z());
            d.radius   = 2.f;
            d.hardness = 1.f;
            d.flow     = 1.f;
            d.normal   = Vec3f(0, -1, 0);
            f.dab(surface, d);
        }
        f.end_stroke();
        AirbrushBakeParams bp;
        bp.base_filament = 1;
        const BakeJobResult r = run_bake_job(job_for(*part.vol, f, bp));
        REQUIRE(r.status == BakeJobResult::Status::Ok);
        commit_bake(*part.vol, r);

        cp.layer_height       = 0.2;
        cp.first_layer_height = 0.2;
        CostJob job;
        job.params = cp;
        CostJob::Part p;
        p.mesh          = part.vol->mesh_ptr();
        p.paint         = part.vol->mmu_segmentation_facets.get_data();
        p.base_filament = 1;
        // On the bed: the mesh is centred by add_volume().
        p.trafo = Transform3d(Eigen::Translation3d(0., 0., -bb.min.z()));
        job.parts.push_back(p);
        const CostJobResult cr = run_cost_job(job);
        // The 4 mm band (grown by the grain cells and the dithered rim) is 20-36 layers of two filaments; each costs one swap, plus one back to the
        // base filament after the band when the band ends on the sprayed one.
        CHECK(cr.cost.multi_tool_layers >= 16);
        CHECK(cr.cost.multi_tool_layers <= 36);
        CHECK(cr.cost.swaps >= cr.cost.multi_tool_layers);
        CHECK(cr.cost.swaps <= cr.cost.multi_tool_layers + 1);
    }
}

// Hidden benchmark: the P2 numbers on a ~1M-facet mesh. Run it by name:
//   libslic3r_tests "[AirbrushP2Bench]"
TEST_CASE("Airbrush P2: 1M-facet timings", "[AirbrushP2Bench][.]")
{
    using clk = std::chrono::steady_clock;
    auto ms   = [](clk::time_point t0) { return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); };
    // A 40 mm sphere of about a million facets (edges about 0.18 mm).
    const double fa = 2. * PI / std::sqrt(1.0e6);
    auto         t0 = clk::now();
    auto         mesh = std::make_shared<const TriangleMesh>(its_make_sphere(40., fa));
    const double t_mesh = ms(t0);
    const size_t nf     = mesh->its.indices.size();
    std::cout << "[AirbrushP2Bench] facets " << nf << " (mesh " << t_mesh << " ms)\n";
    REQUIRE(nf > 800000);

    t0 = clk::now();
    AirbrushSurface surface(mesh->its);
    std::cout << "[AirbrushP2Bench] AirbrushSurface build " << ms(t0) << " ms\n";

    // A 60 mm stroke at radius 4 mm round the equator, flow 25%, then a 1 s hold: dabs as the tool lays them.
    AirbrushField field;
    field.add_slot(2);
    StrokeParams sp;
    field.begin_stroke(sp);
    DabEmitter            e;
    std::vector<double>   dab_ms;
    std::vector<uint64_t> changed;
    size_t                changed_total = 0;
    auto lay = [&](const std::vector<SurfaceHit> &hits) {
        for (const SurfaceHit &h : hits) {
            Dab d;
            d.center   = h.pos;
            d.radius   = 4.f;
            d.hardness = 0.3f;
            d.flow     = 0.25f;
            d.normal   = h.facing;
            changed.clear();
            const auto t = clk::now();
            field.dab(surface, d, &changed);
            dab_ms.push_back(ms(t));
            changed_total += changed.size();
        }
    };
    auto on_sphere = [](double ang) {
        const Vec3f p(float(40. * std::cos(ang)), float(40. * std::sin(ang)), 0.f);
        return SurfaceHit{ p, Vec3f(p.normalized()) };
    };
    double t = 0.;
    lay(e.press(on_sphere(0.), t, 4.f));
    for (int i = 1; i <= 120; ++i) {
        t += 1. / 60.;
        lay(e.move(on_sphere(1.5 * double(i) / 120.), t, 4.f));
    }
    for (int i = 0; i < 60; ++i) {
        t += 1. / 60.;
        lay(e.hold(t));
    }
    field.end_stroke();
    std::sort(dab_ms.begin(), dab_ms.end());
    double sum = 0.;
    for (double v : dab_ms)
        sum += v;
    std::cout << "[AirbrushP2Bench] dabs " << dab_ms.size() << ": avg " << sum / double(dab_ms.size()) << " ms, median "
              << dab_ms[dab_ms.size() / 2] << " ms, max " << dab_ms.back() << " ms; cells changed " << changed_total
              << ", field cells " << field.size() << "\n";

    // The overlay: every chunk of the stroke rebuilt once (the CPU side of the GL upload).
    std::map<uint64_t, std::vector<uint64_t>> chunks;
    for (const auto &kv : field.cells())
        chunks[overlay_chunk_of(kv.first)].push_back(kv.first);
    t0                 = clk::now();
    size_t quads       = 0;
    double chunk_max   = 0.;
    for (const auto &kv : chunks) {
        const auto tc = clk::now();
        for (const auto &b : build_overlay_chunk(field, kv.second))
            quads += b.corners.size() / 4;
        chunk_max = std::max(chunk_max, ms(tc));
    }
    std::cout << "[AirbrushP2Bench] overlay: " << chunks.size() << " chunks, " << quads << " quads, all " << ms(t0) << " ms, worst chunk "
              << chunk_max << " ms\n";

    // Release: the field copy (UI thread), the bake and the selector (worker), the cost (worker).
    t0 = clk::now();
    BakeJob job;
    job.field          = std::make_shared<const AirbrushField>(field);
    const double t_copy = ms(t0);
    job.mesh           = mesh;
    job.params.base_filament = 1;
    const BakeJobResult r = run_bake_job(job);
    REQUIRE(r.status == BakeJobResult::Status::Ok);
    std::cout << "[AirbrushP2Bench] field copy " << t_copy << " ms; bake " << r.bake_ms << " ms (" << r.stats.leaves_evaluated
              << " leaves evaluated, " << r.stats.leaves_airbrushed << " airbrushed, " << r.stats.triangles_owned << " facets owned)\n";

    t0 = clk::now();
    TriangleSelector sel(*mesh, 0.2f);
    const double t_sel = ms(t0);
    t0 = clk::now();
    sel.deserialize(r.painting, false);
    std::cout << "[AirbrushP2Bench] selector construct " << t_sel << " ms, deserialize " << ms(t0) << " ms\n";

    // A second stroke: the re-bake with a record.
    AirbrushField again = *r.field;
    again.begin_stroke(sp);
    Dab d;
    d.center = on_sphere(2.).pos;
    d.radius = 4.f;
    d.flow   = 0.5f;
    d.normal = on_sphere(2.).facing;
    again.dab(surface, d);
    again.end_stroke();
    BakeJob job2;
    job2.field   = std::make_shared<const AirbrushField>(again);
    job2.mesh    = mesh;
    job2.current = r.painting;
    job2.params.base_filament = 1;
    const BakeJobResult r2 = run_bake_job(job2);
    REQUIRE(r2.status == BakeJobResult::Status::Ok);
    std::cout << "[AirbrushP2Bench] second bake (with record) " << r2.bake_ms << " ms\n";

    CostJob cj;
    cj.params.layer_height       = 0.2;
    cj.params.first_layer_height = 0.2;
    CostJob::Part p;
    p.mesh          = mesh;
    p.paint         = r2.painting;
    p.base_filament = 1;
    p.trafo         = Transform3d(Eigen::Translation3d(0., 0., 40.));
    cj.parts.push_back(p);
    const CostJobResult cr = run_cost_job(cj);
    std::cout << "[AirbrushP2Bench] cost job " << cr.ms << " ms (" << cr.cost.swaps << " swaps over " << cr.cost.layers << " layers)\n";

    // The worst case: the whole sphere sprayed at 30% and baked at the 0.6 mm wall floor, so the paint
    // reaches about a million leaves (the P0 benchmark's size).
    AirbrushField dense;
    dense.add_slot(2);
    dense.begin_stroke(sp);
    Dab all;
    all.radius   = 1000.f;
    all.hardness = 1.f;
    all.flow     = 0.3f;
    t0           = clk::now();
    dense.dab(surface, all);
    const double t_all = ms(t0);
    dense.end_stroke();
    t0 = clk::now();
    BakeJob job3;
    job3.field                 = std::make_shared<const AirbrushField>(dense);
    const double t_copy3       = ms(t0);
    job3.mesh                  = mesh;
    job3.params.base_filament  = 1;
    job3.params.grain_mm       = 0.6f;
    const BakeJobResult r3 = run_bake_job(job3);
    REQUIRE(r3.status == BakeJobResult::Status::Ok);
    t0 = clk::now();
    TriangleSelector sel3(*mesh, 0.2f);
    sel3.deserialize(r3.painting, false);
    const double t_sel3 = ms(t0);
    t0 = clk::now();
    size_t painted_tris = 0;
    for (int st = 1; st <= 2; ++st)
        painted_tris += sel3.get_facets_strict(EnforcerBlockerType(st)).indices.size();
    const double t_facets3 = ms(t0);
    p.paint = r3.painting;
    cj.parts.assign(1, p);
    const CostJobResult cr3 = run_cost_job(cj);
    std::cout << "[AirbrushP2Bench] dense: whole-sphere dab " << t_all << " ms, field " << dense.size() << " cells, copy " << t_copy3
              << " ms; bake " << r3.bake_ms << " ms (" << r3.stats.leaves_evaluated << " leaves evaluated, " << r3.stats.leaves_airbrushed
              << " airbrushed); selector construct+deserialize " << t_sel3 << " ms; facets per state (GLVolume CPU side) " << t_facets3
              << " ms, " << painted_tris << " triangles; cost job " << cr3.ms << " ms\n";
}
