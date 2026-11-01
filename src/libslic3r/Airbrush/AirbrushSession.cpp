#include "AirbrushSession.hpp"

#include "../MixedFilament.hpp"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <locale>
#include <map>
#include <sstream>

namespace Slic3r {
namespace Airbrush {

// =============================================================================================
// Brush settings
// =============================================================================================

static float clamp_or(float v, float lo, float hi, float fallback)
{
    if (!(v == v))
        return fallback;
    return std::clamp(v, lo, hi);
}

void BrushSettings::sanitize()
{
    radius_mm = clamp_or(radius_mm, AIRBRUSH_RADIUS_MIN, AIRBRUSH_RADIUS_MAX, AIRBRUSH_RADIUS_DEFAULT);
    hardness  = clamp_or(hardness, 0.f, 1.f, AIRBRUSH_HARDNESS_DEFAULT);
    flow      = clamp_or(flow, AIRBRUSH_FLOW_MIN, 1.f, AIRBRUSH_FLOW_DEFAULT);
    grain_mm  = clamp_or(grain_mm, AIRBRUSH_MIN_GRAIN_WALL_MM, AIRBRUSH_GRAIN_MAX, AIRBRUSH_DEFAULT_GRAIN_MM);
}

std::string BrushSettings::to_string() const
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "radius=" << radius_mm << ";hardness=" << hardness << ";flow=" << flow << ";grain=" << grain_mm
        << ";mix=" << (mix_steps ? 1 : 0) << ";seed=" << seed;
    return out.str();
}

BrushSettings BrushSettings::from_string(const std::string &s)
{
    BrushSettings b;
    std::istringstream in(s);
    in.imbue(std::locale::classic());
    std::string item;
    while (std::getline(in, item, ';')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string  key = item.substr(0, eq);
        std::istringstream val(item.substr(eq + 1));
        val.imbue(std::locale::classic());
        if (key == "seed") {
            unsigned long v = 0;
            if (val >> v)
                b.seed = uint32_t(v);
            continue;
        }
        float v = 0.f;
        if (!(val >> v))
            continue;
        if (key == "radius")
            b.radius_mm = v;
        else if (key == "hardness")
            b.hardness = v;
        else if (key == "flow")
            b.flow = v;
        else if (key == "grain")
            b.grain_mm = v;
        else if (key == "mix")
            b.mix_steps = v != 0.f;
    }
    b.sanitize();
    return b;
}

AirbrushBakeParams bake_params_from(const BrushSettings &brush_in, int base_filament)
{
    BrushSettings brush = brush_in;
    brush.sanitize();
    AirbrushBakeParams p;
    p.grain_mm      = brush.grain_mm;
    p.mix_steps     = brush.mix_steps ? AIRBRUSH_DEFAULT_MIX_STEPS : 0;
    p.seed          = brush.seed;
    p.base_filament = std::max(0, base_filament);
    return p;
}

// =============================================================================================
// Dabs
// =============================================================================================

std::vector<SurfaceHit> DabEmitter::press(const SurfaceHit &hit, double t, float /*radius*/)
{
    m_active     = true;
    m_last       = hit;
    m_cursor     = hit;
    m_carry      = 0.f;
    m_last_dab_t = t;
    m_emitted    = 1;
    return { hit };
}

std::vector<SurfaceHit> DabEmitter::move(const SurfaceHit &hit, double t, float radius)
{
    std::vector<SurfaceHit> out;
    if (!m_active)
        return out;
    const float step = std::max(m_params.spacing * radius, 1e-3f);
    const Vec3f a    = m_cursor.pos;
    const Vec3f d    = hit.pos - a;
    const float len  = d.norm();
    if (len > m_params.max_gap_radii * std::max(radius, 1e-3f)) {
        // The cursor jumped (across a gap or off and back on the model): no paint in between.
        out.push_back(hit);
        m_last       = hit;
        m_carry      = 0.f;
        m_last_dab_t = t;
    } else if (len > 0.f) {
        float at       = step - m_carry;   // distance along this segment of the next dab
        float last_dab = -1.f;
        while (at <= len + 1e-6f) {
            const float f = std::clamp(at / len, 0.f, 1.f);
            SurfaceHit  h;
            h.pos         = a + d * f;
            Vec3f facing  = m_cursor.facing * (1.f - f) + hit.facing * f;
            h.facing      = facing.squaredNorm() > 0.f ? Vec3f(facing.normalized()) : hit.facing;
            out.push_back(h);
            last_dab      = at;
            at += step;
        }
        if (last_dab >= 0.f) {
            m_carry      = len - last_dab;
            m_last       = out.back();
            m_last_dab_t = t;
        } else
            m_carry += len;
    }
    m_cursor = hit;
    m_emitted += out.size();
    return out;
}

std::vector<SurfaceHit> DabEmitter::hold(double t)
{
    std::vector<SurfaceHit> out;
    if (!m_active || !(m_params.rate_hz > 0.f))
        return out;
    const double period = 1. / double(m_params.rate_hz);
    const double due    = std::floor((t - m_last_dab_t) / period + 1e-9);
    if (due < 1.)
        return out;
    const int n = int(std::min<double>(due, double(std::max(1, m_params.max_dwell_burst))));
    out.assign(size_t(n), m_cursor);
    // Keep the phase; a stall's backlog beyond the burst is dropped, not owed.
    m_last_dab_t += due * period;
    m_last  = m_cursor;
    m_carry = 0.f;
    m_emitted += out.size();
    return out;
}

// =============================================================================================
// Overlay
// =============================================================================================

static int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

uint64_t overlay_chunk_of(uint64_t cell_key)
{
    int64_t i, j, k;
    int     cls;
    AirbrushField::split_key(cell_key, i, j, k, cls);
    const int64_t n = int64_t(1) << AIRBRUSH_OVERLAY_CHUNK_SHIFT;
    return AirbrushField::make_key(floor_div(i, n), floor_div(j, n), floor_div(k, n), cls);
}

void overlay_style(const Weights &w, size_t nslots, int &slot, int &level)
{
    nslots         = std::min(nslots, AIRBRUSH_MAX_SLOTS);
    uint32_t total = 0;
    size_t   best  = 0;
    for (size_t s = 0; s < nslots; ++s) {
        total += w[s];
        if (w[s] > w[best])
            best = s;
    }
    if (total == 0) {
        slot  = -1;
        level = 0;
        return;
    }
    slot             = int(best);
    const float cov  = float(std::min<uint32_t>(total, 255u)) / 255.f;
    level            = std::clamp(int(std::ceil(cov * float(AIRBRUSH_OVERLAY_LEVELS) - 1e-4f)), 1, AIRBRUSH_OVERLAY_LEVELS);
}

std::vector<OverlayBucket> build_overlay_chunk(const AirbrushField &field, const std::vector<uint64_t> &cells)
{
    std::map<std::pair<int, int>, std::vector<Vec3f>> buckets;
    const float h    = field.cell_size();
    const float half = 0.5f * h;
    const float lift = 0.15f * h;   // clear of a surface lying exactly on the cell's face
    for (uint64_t key : cells) {
        int slot = -1, level = 0;
        if (const AirbrushField::Cell *c = field.find(key))
            overlay_style(c->w, field.slots().size(), slot, level);
        int64_t i, j, k;
        int     cls;
        AirbrushField::split_key(key, i, j, k, cls);
        if (cls < 0 || cls > 5)
            continue;
        const int   axis = cls / 2;
        const float sign = (cls % 2 == 0) ? 1.f : -1.f;
        Vec3f       c    = field.center_of(key).cast<float>();
        c[axis] += sign * (half + lift);
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        auto corner = [&](float su, float sv) {
            Vec3f p = c;
            p[u] += su * half;
            p[v] += sv * half;
            return p;
        };
        std::vector<Vec3f> &q = buckets[{ slot, level }];
        q.push_back(corner(-1.f, -1.f));
        q.push_back(corner(1.f, -1.f));
        q.push_back(corner(1.f, 1.f));
        q.push_back(corner(-1.f, 1.f));
    }
    std::vector<OverlayBucket> out;
    out.reserve(buckets.size());
    for (auto &[sl, corners] : buckets) {
        OverlayBucket b;
        b.slot    = sl.first;
        b.level   = sl.second;
        b.corners = std::move(corners);
        out.push_back(std::move(b));
    }
    return out;
}

// =============================================================================================
// Mix ladder
// =============================================================================================

namespace {
struct LadderRow
{
    int a = 0, b = 0;      // components
    int share_b = 0;       // percent of b
    int id = 0;            // virtual filament id
};

std::vector<LadderRow> ladder_rows(const MixedFilamentManager &mixed, size_t num_physical)
{
    std::vector<LadderRow> rows;
    const std::vector<MixedFilament> &all = mixed.mixed_filaments();
    for (size_t idx = 0; idx < all.size(); ++idx) {
        const MixedFilament &mf = all[idx];
        if (!mf.enabled || mf.deleted)
            continue;
        if (mf.distribution_mode != int(MixedFilament::LayerCycle) && mf.distribution_mode != int(MixedFilament::Simple))
            continue;
        if (mf.gradient_component_ids.size() > 2)
            continue;   // three or more components: not a two-filament ladder step
        const unsigned int id = mixed.filament_id_from_mixed_index(idx, num_physical);
        if (id == 0 || id > unsigned(EnforcerBlockerType::ExtruderMax))
            continue;
        LadderRow r;
        r.a       = int(mf.component_a);
        r.b       = int(mf.component_b);
        r.share_b = mf.manual_pattern.empty() ? mf.mix_b_percent : MixedFilamentManager::mix_percent_from_manual_pattern(mf.manual_pattern);
        r.id      = int(id);
        rows.push_back(r);
    }
    return rows;
}

int pick_ladder(const std::vector<LadderRow> &rows, int a, int b, int step, int steps)
{
    if (steps < 2 || step <= 0 || step >= steps || a == b)
        return 0;
    const double target = 100. * double(step) / double(steps);
    // Closer than a quarter of a step: a row halfway between two steps belongs to neither.
    const double tol  = 25. / double(steps);
    int          best = 0;
    double       err  = tol;
    for (const LadderRow &r : rows) {
        int share;
        if (r.a == a && r.b == b)
            share = r.share_b;
        else if (r.a == b && r.b == a)
            share = 100 - r.share_b;
        else
            continue;
        const double e = std::abs(double(share) - target);
        if (e < err || (best != 0 && e == err && r.id < best)) {
            best = r.id;
            err  = e;
        }
    }
    return best;
}
} // namespace

int find_ladder_state(const MixedFilamentManager &mixed, size_t num_physical, int a, int b, int step, int steps)
{
    return pick_ladder(ladder_rows(mixed, num_physical), a, b, step, steps);
}

LadderLookup ladder_from_mixed(const MixedFilamentManager &mixed, size_t num_physical)
{
    auto rows = std::make_shared<const std::vector<LadderRow>>(ladder_rows(mixed, num_physical));
    if (rows->empty())
        return {};
    return [rows](int a, int b, int step, int steps) { return pick_ladder(*rows, a, b, step, steps); };
}

// =============================================================================================
// Bake job
// =============================================================================================

BakeJobResult run_bake_job(const BakeJob &job)
{
    BakeJobResult r;
    const auto    t0 = std::chrono::steady_clock::now();
    if (!job.mesh || !job.field) {
        r.error = "nothing to bake";
        return r;
    }
    const indexed_triangle_set &its = job.mesh->its;
    auto                        f   = std::make_shared<AirbrushField>(*job.field);
    const BakeRecord           &rec = f->record();
    if (rec.valid() && rec.facets != uint32_t(its.indices.size())) {
        if (!job.overwrite) {
            r.status = BakeJobResult::Status::StaleRecord;
            return r;
        }
        // The mesh changed under the record: its specks cannot be told from real paint any more.
        f->record().clear();
    } else if (rec.valid() && rec.paint_hash != paint_hash(job.current) && !job.overwrite) {
        r.status = BakeJobResult::Status::ForeignEdit;
        return r;
    }
    AirbrushBakeResult b = airbrush_bake(its, *f, job.current, job.params, job.ladder);
    if (!b.ok) {
        r.error = b.error;
        return r;
    }
    f->record() = std::move(b.record);
    f->set_bake_params(job.params.to_string());
    f->set_anchor(its);
    r.field    = std::move(f);
    r.painting = std::move(b.painting);
    r.stats    = b.stats;
    r.status   = BakeJobResult::Status::Ok;
    r.bake_ms  = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

void commit_bake(ModelVolume &volume, const BakeJobResult &result)
{
    if (result.status != BakeJobResult::Status::Ok || !result.field)
        return;
    // A field erased to nothing that owns no paint any more leaves no sidecar behind.
    if (result.field->empty() && result.field->record().owned.empty())
        volume.airbrush_field.reset();
    else
        volume.airbrush_field = result.field;
    TriangleSelector::TriangleSplittingData paint = result.painting;
    volume.mmu_segmentation_facets.set_data(std::move(paint));
}

// =============================================================================================
// Cost job
// =============================================================================================

CostJobResult run_cost_job(const CostJob &job)
{
    CostJobResult r;
    const auto    t0 = std::chrono::steady_clock::now();
    std::vector<AirbrushCost> parts;
    parts.reserve(job.parts.size());
    for (const CostJob::Part &part : job.parts) {
        if (!part.mesh)
            continue;
        AirbrushCostParams p = job.params;
        p.base_filament      = part.base_filament;
        parts.push_back(airbrush_estimate_cost(part.mesh->its, part.paint, p, part.trafo));
    }
    r.cost    = airbrush_merge_costs(parts, job.params);
    r.warning = airbrush_cost_warning(r.cost);
    r.ms      = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

} // namespace Airbrush
} // namespace Slic3r
