#include "AirbrushBake.hpp"
#include "BlueNoise.hpp"

#include "../ImageFill.hpp"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace Slic3r {
namespace Airbrush {

// =============================================================================================
// Parameters
// =============================================================================================

std::string AirbrushBakeParams::to_string() const
{
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << "grain=" << grain_mm << ";wall_min=" << min_grain_wall_mm << ";top_min=" << min_grain_top_mm << ";flat_cos=" << flat_cos
       << ";steps=" << mix_steps << ";low=" << low_pass_mm << ";min_cov=" << min_coverage << ";seed=" << seed << ";base=" << base_filament;
    if (design_nozzle_mm > 0.f)
        ss << ";nozzle=" << design_nozzle_mm;
    if (print_scale != 1.f)
        ss << ";pscale=" << print_scale;
    return ss.str();
}

bool AirbrushBakeParams::from_string(const std::string &s, AirbrushBakeParams &out)
{
    AirbrushBakeParams p;
    std::istringstream ss(s);
    ss.imbue(std::locale::classic());
    std::string item;
    bool        any = false;
    while (std::getline(ss, item, ';')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
        std::istringstream vs(v);
        vs.imbue(std::locale::classic());
        if (k == "grain") vs >> p.grain_mm;
        else if (k == "wall_min") vs >> p.min_grain_wall_mm;
        else if (k == "top_min") vs >> p.min_grain_top_mm;
        else if (k == "flat_cos") vs >> p.flat_cos;
        else if (k == "steps") vs >> p.mix_steps;
        else if (k == "low") vs >> p.low_pass_mm;
        else if (k == "min_cov") vs >> p.min_coverage;
        else if (k == "seed") vs >> p.seed;
        else if (k == "base") vs >> p.base_filament;
        else if (k == "nozzle") vs >> p.design_nozzle_mm;
        else if (k == "pscale") vs >> p.print_scale;
        else continue;
        if (vs.fail())
            return false;
        any = true;
    }
    // Sanity: anything outside these is a damaged project, not a setting.
    if (!(p.grain_mm > 0.05f && p.grain_mm < 50.f) || !(p.min_grain_wall_mm >= 0.f) || !(p.min_grain_top_mm >= 0.f) ||
        !(p.flat_cos > 0.f && p.flat_cos <= 1.f) || p.mix_steps < 0 || p.mix_steps > 64 || !(p.low_pass_mm >= 0.f) ||
        p.base_filament < 0 || p.base_filament > int(EnforcerBlockerType::ExtruderMax) ||
        !(p.design_nozzle_mm >= 0.f && p.design_nozzle_mm <= 5.f) || !(p.print_scale > 1e-4f && p.print_scale < 1e4f))
        return false;
    if (any)
        out = p;
    return any;
}

EffectiveGrain effective_grain(const AirbrushBakeParams &params, bool flat)
{
    EffectiveGrain g;
    const float req = std::max(params.grain_mm, 0.05f);
    if (flat) {
        if (req < params.min_grain_top_mm) {
            g.grain_mm      = params.min_grain_top_mm;
            g.tone_fallback = true;
        } else
            g.grain_mm = req;
    } else
        g.grain_mm = std::max(req, params.min_grain_wall_mm);
    return g;
}

AirbrushFloors airbrush_floors_for_nozzle(float nozzle_mm)
{
    AirbrushFloors f;
    if (!(nozzle_mm > 0.f) || std::abs(nozzle_mm - AIRBRUSH_REFERENCE_NOZZLE_MM) < 1e-4f)
        return f;   // exactly the P0 constants
    const float k = nozzle_mm / AIRBRUSH_REFERENCE_NOZZLE_MM;
    f.nozzle_mm   = nozzle_mm;
    f.wall_mm     = std::max(AIRBRUSH_MIN_GRAIN_MM, AIRBRUSH_MIN_GRAIN_WALL_MM * k);
    f.top_mm      = std::max(AIRBRUSH_MIN_GRAIN_MM, AIRBRUSH_MIN_GRAIN_TOP_MM * k);
    return f;
}

float airbrush_design_nozzle(const AirbrushBakeParams &params, float printer_nozzle_mm)
{
    if (params.design_nozzle_mm > 0.f)
        return params.design_nozzle_mm;
    return printer_nozzle_mm > 0.f ? printer_nozzle_mm : AIRBRUSH_REFERENCE_NOZZLE_MM;
}

void airbrush_apply_nozzle_floors(AirbrushBakeParams &params, float nozzle_mm)
{
    const AirbrushFloors f   = airbrush_floors_for_nozzle(nozzle_mm);
    params.min_grain_wall_mm = AIRBRUSH_MIN_GRAIN_MM;
    params.min_grain_top_mm  = f.top_mm;
}

bool airbrush_grain_below_floor(float grain_mm, float nozzle_mm)
{
    return grain_mm < airbrush_floors_for_nozzle(nozzle_mm).wall_mm - 1e-4f;
}

// =============================================================================================
// The bitstream: a uniform-depth leaf array, encoded with uniform subtrees collapsed to one leaf.
// =============================================================================================

namespace {

void push_leaf(std::vector<bool> &bits, int n)
{
    bits.push_back(false);   // number_of_split_sides & 0b01
    bits.push_back(false);   // number_of_split_sides & 0b10
    if (n >= 3) {
        bits.push_back(true);
        bits.push_back(true);
        int m = n - 3;
        while (m >= 15) {
            for (int i = 0; i < 4; ++i)
                bits.push_back(true);
            m -= 15;
        }
        for (int i = 0; i < 4; ++i)
            bits.push_back((m & (1 << i)) != 0);
    } else {
        bits.push_back((n & 0b01) != 0);
        bits.push_back((n & 0b10) != 0);
    }
}

// Mirrors TriangleSelector::serialize() (via image_fill_encode's encoder) - three split sides,
// special side 0, children in reverse - except that a subtree whose leaves all share one state is
// written as that one leaf, which is what serialize() itself does after a merge.
void encode_collapsed(std::vector<bool> &bits, int depth, const int *states, size_t stride)
{
    bool uniform = true;
    for (size_t i = 1; i < stride && uniform; ++i)
        uniform = states[i] == states[0];
    if (depth == 0 || uniform) {
        push_leaf(bits, states[0]);
        return;
    }
    bits.push_back(true);    // 3 & 0b01
    bits.push_back(true);    // 3 & 0b10
    bits.push_back(false);   // special_side 0
    bits.push_back(false);
    const size_t child = stride / 4;
    for (int i = 3; i >= 0; --i)
        encode_collapsed(bits, depth - 1, states + size_t(i) * child, child);
}

// Where each original triangle's subtree lives in a painting: [begin, end) in its bitstream.
std::map<int, std::pair<size_t, size_t>> subtree_ranges(const TriangleSelector::TriangleSplittingData &data)
{
    std::map<int, std::pair<size_t, size_t>> out;
    const auto                              &tts = data.triangles_to_split;
    for (size_t k = 0; k < tts.size(); ++k) {
        const size_t begin = size_t(tts[k].bitstream_start_idx);
        const size_t end   = (k + 1 < tts.size()) ? size_t(tts[k + 1].bitstream_start_idx) : data.bitstream.size();
        if (begin <= end && end <= data.bitstream.size())
            out.emplace(tts[k].triangle_idx, std::make_pair(begin, end));
    }
    return out;
}

void append_subtree(TriangleSelector::TriangleSplittingData &dst, int tri, const TriangleSelector::TriangleSplittingData &src,
                    const std::pair<size_t, size_t> &range)
{
    dst.triangles_to_split.emplace_back(tri, int(dst.bitstream.size()));
    dst.bitstream.insert(dst.bitstream.end(), src.bitstream.begin() + range.first, src.bitstream.begin() + range.second);
}

void finish_used_states(TriangleSelector::TriangleSplittingData &data)
{
    data.reset_used_states();
    if (!data.triangles_to_split.empty())
        (void) data.update_used_states(0);
}

// The painted states a painting really uses, read from its bits (a loaded painting's own
// used_states need not be filled in).
std::vector<EnforcerBlockerType> painted_states(const TriangleSelector::TriangleSplittingData &data)
{
    TriangleSelector::TriangleSplittingData copy;
    copy.triangles_to_split = data.triangles_to_split;
    copy.bitstream          = data.bitstream;
    finish_used_states(copy);
    std::vector<EnforcerBlockerType> out;
    for (EnforcerBlockerType st : TriangleSelector::extract_used_facet_states(copy))
        if (st != EnforcerBlockerType::NONE)
            out.push_back(st);
    return out;
}

// `current` with every triangle in `record.owned` replaced by its `record.under` subtree (or by
// nothing, when it carried no paint before the airbrush).
TriangleSelector::TriangleSplittingData restore_under(const TriangleSelector::TriangleSplittingData &current, const BakeRecord &record)
{
    const std::set<int> owned(record.owned.begin(), record.owned.end());
    const auto          cur   = subtree_ranges(current);
    const auto          under = subtree_ranges(record.under);
    TriangleSelector::TriangleSplittingData out;
    std::set<int> tris;
    for (const auto &kv : cur)
        tris.insert(kv.first);
    for (const auto &kv : under)
        if (owned.count(kv.first))
            tris.insert(kv.first);
    for (int t : tris) {
        if (owned.count(t)) {
            if (auto it = under.find(t); it != under.end())
                append_subtree(out, t, record.under, it->second);
        } else if (auto it = cur.find(t); it != cur.end())
            append_subtree(out, t, current, it->second);
    }
    finish_used_states(out);
    return out;
}

// Point location in an existing painting: per original triangle, its painted sub-triangles.
struct SubTri
{
    int   state;
    Vec3f a, b, c;
};

class ExistingPaint
{
public:
    ExistingPaint(const indexed_triangle_set &its, const TriangleSelector::TriangleSplittingData &data) : m_by_tri(its.indices.size())
    {
        if (data.triangles_to_split.empty())
            return;
        m_any = true;
        TriangleMesh     mesh(its);
        TriangleSelector sel(mesh);
        sel.deserialize(data, true);
        for (EnforcerBlockerType st : painted_states(data)) {
            std::vector<int>           src;
            const indexed_triangle_set f = sel.get_facets_strict(st, &src);
            for (size_t k = 0; k < f.indices.size() && k < src.size(); ++k) {
                const int t = src[k];
                if (t < 0 || size_t(t) >= m_by_tri.size())
                    continue;
                m_by_tri[size_t(t)].push_back(SubTri{ int(st), f.vertices[size_t(f.indices[k][0])], f.vertices[size_t(f.indices[k][1])],
                                                      f.vertices[size_t(f.indices[k][2])] });
            }
        }
    }
    bool any() const { return m_any; }
    // The state at point p of original triangle t; 0 when unpainted there.
    int at(size_t t, const Vec3f &p) const
    {
        if (!m_any || t >= m_by_tri.size())
            return 0;
        int   best      = 0;
        float best_miss = std::numeric_limits<float>::max();
        for (const SubTri &s : m_by_tri[t]) {
            // Barycentric coordinates in the sub-triangle's own plane.
            const Vec3f v0 = s.b - s.a, v1 = s.c - s.a, v2 = p - s.a;
            const float d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
            const float den = d00 * d11 - d01 * d01;
            if (!(std::abs(den) > 0.f))
                continue;
            const float v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den, u = 1.f - v - w;
            const float miss = std::max({ -u, -v, -w, 0.f });
            if (miss <= 1e-4f)
                return s.state;
            if (miss < best_miss && miss < 1e-3f) {
                best_miss = miss;
                best      = s.state;
            }
        }
        return best;
    }

private:
    bool                             m_any = false;
    std::vector<std::vector<SubTri>> m_by_tri;
};

// The grain cell a leaf falls in: its facet's class, whether that facet is flat, and (i, j) in the
// plane perpendicular to the class axis.
struct GrainKey
{
    int     cls;
    int     flat;
    int64_t i, j;
    bool    operator==(const GrainKey &o) const { return cls == o.cls && flat == o.flat && i == o.i && j == o.j; }
    bool    operator<(const GrainKey &o) const
    {
        return std::tie(cls, flat, i, j) < std::tie(o.cls, o.flat, o.i, o.j);
    }
};
struct GrainKeyHash
{
    size_t operator()(const GrainKey &k) const
    {
        return size_t(airbrush_hash(uint32_t(k.cls * 2 + k.flat), uint32_t(k.i), uint32_t(k.j)));
    }
};

Vec2d project_for_class(const Vec3d &p, int cls)
{
    switch (cls / 2) {
    case 0: return Vec2d(p.y(), p.z());   // X walls: across = y, up = z
    case 1: return Vec2d(p.x(), p.z());   // Y walls: across = x, up = z
    default: return Vec2d(p.x(), p.y());  // tops and bottoms
    }
}

struct LeafEval
{
    Vec3f    centroid;
    Weights  w {};
    int      existing = 0;
    GrainKey cell {};
};

struct TriEval
{
    int                   depth   = 0;
    bool                  touched = false;
    std::vector<LeafEval> leaves;
};

struct CellAcc
{
    std::array<uint32_t, AIRBRUSH_MAX_SLOTS> sum {};
    uint32_t                                 n            = 0;
    bool                                     has_existing = false;
    float                                    grain        = 1.f;
    bool                                     tone         = false;
    // The decision: a state id, or -1 for "background" (keep what was there).
    int                                      outcome      = -1;
    bool                                     is_mix       = false;
};

} // namespace

// =============================================================================================
// The bake
// =============================================================================================

AirbrushBakeResult airbrush_bake(const indexed_triangle_set &its, const AirbrushField &field, const TriangleSelector::TriangleSplittingData &current,
                                 const AirbrushBakeParams &params_in, const LadderLookup &ladder)
{
    // Grain, floors and the low band are print sizes; the bake works in mesh mm.
    AirbrushBakeParams params = params_in;
    if (params.print_scale != 1.f && params.print_scale > 0.f) {
        const float inv          = 1.f / params.print_scale;
        params.grain_mm          *= inv;
        params.min_grain_wall_mm *= inv;
        params.min_grain_top_mm  *= inv;
        params.low_pass_mm       *= inv;
    }
    AirbrushBakeResult res;
    const size_t       n = its.indices.size();
    if (n == 0) {
        res.error = "empty mesh";
        return res;
    }
    // D16: start from the paint as it was before the last airbrush bake.
    if (field.record().valid() && field.record().facets != uint32_t(n)) {
        res.error = "the airbrush record belongs to another mesh; re-bake through airbrush_rebake_after_mesh_edit()";
        return res;
    }
    const bool record_ok = field.record().valid();
    const TriangleSelector::TriangleSplittingData base = record_ok ? restore_under(current, field.record()) : current;

    const EffectiveGrain gwall = effective_grain(params, false);
    const EffectiveGrain gtop  = effective_grain(params, true);
    res.stats.grain_wall_mm    = gwall.grain_mm * params_in.print_scale;
    res.stats.grain_top_mm     = gtop.grain_mm * params_in.print_scale;
    res.stats.top_tone_fallback = gtop.tone_fallback;

    // Per facet: class, flatness, depth for a leaf a quarter of its grain across.
    std::vector<int>  cls(n);
    std::vector<char> flat(n);
    std::vector<int>  depth(n);
    auto depth_for = [&](size_t f, float leaf_mm) {
        const Vec3f a = its.vertices[size_t(its.indices[f][0])], b = its.vertices[size_t(its.indices[f][1])], c = its.vertices[size_t(its.indices[f][2])];
        const float e = std::max({ (b - a).norm(), (c - b).norm(), (a - c).norm() });
        if (!(e > leaf_mm))
            return 0;
        return std::min(AIRBRUSH_MAX_DEPTH, int(std::ceil(std::log2(double(e) / double(leaf_mm)))));
    };
    float  leaf_scale = 0.25f;
    for (int attempt = 0; attempt < 8; ++attempt) {
        size_t total = 0;
        for (size_t f = 0; f < n; ++f) {
            const Vec3f nrm = its_face_normal(its, int(f));
            cls[f]          = surface_class(nrm);
            flat[f]         = std::abs(nrm.z()) >= params.flat_cos ? 1 : 0;
            depth[f]        = depth_for(f, (flat[f] ? gtop.grain_mm : gwall.grain_mm) * leaf_scale);
            total += size_t(1) << (2 * depth[f]);
        }
        if (total <= AIRBRUSH_MAX_LEAVES)
            break;
        leaf_scale *= 1.5f;
    }

    const ExistingPaint existing(its, base);
    const uint32_t      seed = params.seed;

    // Pass A (parallel, one slot per facet): sample the field at every leaf.
    std::vector<TriEval> tri(n);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        std::vector<ImageFillLeaf> leaves;
        for (size_t f = range.begin(); f < range.end(); ++f) {
            TriEval &te = tri[f];
            te.depth    = depth[f];
            if (cls[f] < 0)
                continue;
            leaves.clear();
            image_fill_subdivide(its.vertices[size_t(its.indices[f][0])], its.vertices[size_t(its.indices[f][1])],
                                 its.vertices[size_t(its.indices[f][2])], te.depth, leaves);
            const float g = flat[f] ? gtop.grain_mm : gwall.grain_mm;
            te.leaves.resize(leaves.size());
            for (size_t l = 0; l < leaves.size(); ++l) {
                LeafEval &le = te.leaves[l];
                le.centroid  = leaves[l].centroid();
                if (field.sample(le.centroid.cast<double>(), cls[f], le.w))
                    for (uint8_t v : le.w)
                        te.touched |= v > 0;
                const Vec2d q = project_for_class(le.centroid.cast<double>() - field.origin(), cls[f]);
                le.cell       = GrainKey{ cls[f], int(flat[f]), int64_t(std::floor(q.x() / g)), int64_t(std::floor(q.y() / g)) };
            }
            if (!te.touched) {
                te.leaves.clear();
                te.leaves.shrink_to_fit();
                continue;
            }
            for (LeafEval &le : te.leaves)
                le.existing = existing.at(f, le.centroid);
        }
    });

    // Pass B (sequential): average each grain cell over its leaves.
    std::unordered_map<GrainKey, CellAcc, GrainKeyHash> cells;
    for (size_t f = 0; f < n; ++f) {
        const TriEval &te = tri[f];
        if (!te.touched)
            continue;
        res.stats.leaves_evaluated += te.leaves.size();
        for (const LeafEval &le : te.leaves) {
            CellAcc &c = cells[le.cell];
            for (size_t s = 0; s < AIRBRUSH_MAX_SLOTS; ++s)
                c.sum[s] += le.w[s];
            ++c.n;
            c.has_existing |= le.existing != 0;
            c.grain = flat[f] ? gtop.grain_mm : gwall.grain_mm;
            c.tone  = flat[f] && gtop.tone_fallback;
        }
    }
    res.stats.grain_cells = cells.size();

    // Pass C: decide every cell. Visit them in key order so nothing depends on hash iteration.
    std::vector<GrainKey> keys;
    keys.reserve(cells.size());
    for (const auto &kv : cells)
        keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    const std::vector<int> &slots  = field.slots();
    const size_t            nslots = slots.size();
    for (const GrainKey &key : keys) {
        CellAcc &c = cells.at(key);
        std::array<float, AIRBRUSH_MAX_SLOTS> cov {};
        float                                 total = 0.f;
        for (size_t s = 0; s < nslots; ++s) {
            cov[s] = float(c.sum[s]) / (255.f * float(c.n));
            total += cov[s];
        }
        total = std::min(total, 1.f);
        if (coverage_reads_as_tone(total))
            ++res.stats.grain_cells_tone;
        if (total < params.min_coverage)
            continue;   // background
        if (c.tone) {
            // Tops asked for a grain finer than they can print: majority, no dithering.
            size_t best   = 0;
            float  bestw  = -1.f;
            for (size_t s = 0; s < nslots; ++s)
                if (cov[s] > bestw) {
                    bestw = cov[s];
                    best  = s;
                }
            if (bestw > 1.f - total)
                c.outcome = slots[best];
            continue;
        }
        // The low band: the field smoothed over low_pass_mm, from neighbouring cells of the same
        // class and flatness.
        std::array<float, AIRBRUSH_MAX_SLOTS> low {};
        if (!key.flat && params.mix_steps >= 2 && ladder && params.base_filament > 0 && !c.has_existing) {
            const int k   = std::max(1, int(std::lround(params.low_pass_mm / c.grain)));
            float     cnt = 0.f;
            for (int64_t dj = -k; dj <= k; ++dj)
                for (int64_t di = -k; di <= k; ++di) {
                    auto it = cells.find(GrainKey{ key.cls, key.flat, key.i + di, key.j + dj });
                    if (it == cells.end())
                        continue;
                    for (size_t s = 0; s < nslots; ++s)
                        low[s] += float(it->second.sum[s]) / (255.f * float(it->second.n));
                    cnt += 1.f;
                }
            for (size_t s = 0; s < nslots; ++s)
                low[s] /= std::max(cnt, 1.f);
        }
        size_t dom = 0;
        for (size_t s = 1; s < nslots; ++s)
            if (low[s] > low[dom])
                dom = s;
        float m           = 0.f;
        int   ladder_state = 0;
        if (low[dom] > 0.f) {
            const int k = std::clamp(int(std::lround(low[dom] * float(params.mix_steps))), 0, params.mix_steps);
            if (k > 0 && k < params.mix_steps) {
                const int st = ladder(params.base_filament, slots[dom], k, params.mix_steps);
                if (st > 0 && st <= int(EnforcerBlockerType::ExtruderMax)) {
                    m            = float(k) / float(params.mix_steps);
                    ladder_state = st;
                }
            }
        }
        // Masses: pure specks of every slot, ladder cells, background - in that order against the
        // blue-noise threshold, so the rarest outcome gets the best spread.
        std::array<float, AIRBRUSH_MAX_SLOTS> q {};
        float                                 q_ladder = 0.f;
        float                                 others   = 0.f;
        for (size_t s = 0; s < nslots; ++s)
            if (s != dom)
                others += cov[s];
        if (ladder_state > 0) {
            q_ladder = std::min({ cov[dom] / m, (1.f - total) / (1.f - m), std::max(0.f, 1.f - others) });
            q_ladder = std::max(q_ladder, 0.f);
        }
        for (size_t s = 0; s < nslots; ++s)
            q[s] = s == dom ? std::max(0.f, cov[s] - q_ladder * m) : cov[s];
        const float t   = blue_noise_threshold(key.i, key.j, airbrush_hash(seed, uint32_t(key.cls), uint32_t(key.flat)));
        float       cum = 0.f;
        for (size_t s = 0; s < nslots && c.outcome < 0; ++s) {
            cum += q[s];
            if (t < cum)
                c.outcome = slots[s];
        }
        if (c.outcome < 0 && ladder_state > 0) {
            cum += q_ladder;
            if (t < cum) {
                c.outcome = ladder_state;
                c.is_mix  = true;
            }
        }
    }

    // Pass C2: a facet the field does not reach can still share a grain cell with one it does. It
    // takes that cell's decision too, so a speck is never cut short at a facet edge.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n), [&](const tbb::blocked_range<size_t> &range) {
        std::vector<ImageFillLeaf> leaves;
        for (size_t f = range.begin(); f < range.end(); ++f) {
            TriEval &te = tri[f];
            if (te.touched || cls[f] < 0)
                continue;
            const float g  = flat[f] ? gtop.grain_mm : gwall.grain_mm;
            Vec2d       lo = Vec2d::Constant(std::numeric_limits<double>::max()), hi = -lo;
            for (int k = 0; k < 3; ++k) {
                const Vec2d q = project_for_class(its.vertices[size_t(its.indices[f][k])].cast<double>() - field.origin(), cls[f]);
                lo            = lo.cwiseMin(q);
                hi            = hi.cwiseMax(q);
            }
            const int64_t i0 = int64_t(std::floor(lo.x() / g)), i1 = int64_t(std::floor(hi.x() / g));
            const int64_t j0 = int64_t(std::floor(lo.y() / g)), j1 = int64_t(std::floor(hi.y() / g));
            bool          hit = false;
            for (int64_t j = j0; j <= j1 && !hit; ++j)
                for (int64_t i = i0; i <= i1 && !hit; ++i) {
                    auto it = cells.find(GrainKey{ cls[f], int(flat[f]), i, j });
                    hit     = it != cells.end() && it->second.outcome > 0;
                }
            if (!hit)
                continue;
            leaves.clear();
            image_fill_subdivide(its.vertices[size_t(its.indices[f][0])], its.vertices[size_t(its.indices[f][1])],
                                 its.vertices[size_t(its.indices[f][2])], te.depth, leaves);
            te.leaves.resize(leaves.size());
            for (size_t l = 0; l < leaves.size(); ++l) {
                LeafEval &le  = te.leaves[l];
                le.centroid   = leaves[l].centroid();
                const Vec2d q = project_for_class(le.centroid.cast<double>() - field.origin(), cls[f]);
                le.cell       = GrainKey{ cls[f], int(flat[f]), int64_t(std::floor(q.x() / g)), int64_t(std::floor(q.y() / g)) };
                le.existing   = existing.at(f, le.centroid);
            }
            te.touched = true;
        }
    });

    // Pass D: per facet, the leaf states and the bitstream.
    const auto base_ranges = subtree_ranges(base);
    TriangleSelector::TriangleSplittingData out;
    BakeRecord                              record;
    record.facets = uint32_t(n);
    std::vector<int> states;
    for (size_t f = 0; f < n; ++f) {
        TriEval &te    = tri[f];
        bool     owned = false;
        if (te.touched) {
            states.assign(te.leaves.size(), 0);
            for (size_t l = 0; l < te.leaves.size(); ++l) {
                const LeafEval &le = te.leaves[l];
                auto            ci = cells.find(le.cell);
                if (ci != cells.end() && ci->second.outcome > 0) {
                    const CellAcc &c = ci->second;
                    states[l] = c.outcome;
                    owned     = true;
                    ++res.stats.leaves_airbrushed;
                    if (c.is_mix) {
                        ++res.stats.leaves_mix;
                        if (flat[f])
                            ++res.stats.mix_on_flat_leaves;
                    } else
                        ++res.stats.leaves_speck;
                } else
                    states[l] = le.existing;
            }
        }
        auto it = base_ranges.find(int(f));
        if (owned) {
            record.owned.push_back(int(f));
            if (it != base_ranges.end())
                append_subtree(record.under, int(f), base, it->second);
            out.triangles_to_split.emplace_back(int(f), int(out.bitstream.size()));
            encode_collapsed(out.bitstream, te.depth, states.data(), states.size());
        } else if (it != base_ranges.end())
            append_subtree(out, int(f), base, it->second);
        te.leaves.clear();
        te.leaves.shrink_to_fit();
    }
    // A facet whose whole leaf array collapsed to state 0 is unpainted: serialize() would omit it.
    {
        TriangleSelector::TriangleSplittingData pruned;
        const auto                              ranges = subtree_ranges(out);
        for (const auto &[t, r] : ranges) {
            const bool single_zero_leaf = r.second - r.first == 4 && !out.bitstream[r.first] && !out.bitstream[r.first + 1] &&
                                          !out.bitstream[r.first + 2] && !out.bitstream[r.first + 3];
            if (!single_zero_leaf)
                append_subtree(pruned, t, out, r);
        }
        out = std::move(pruned);
    }
    finish_used_states(out);
    finish_used_states(record.under);
    record.paint_hash         = paint_hash(out);
    res.stats.triangles_owned = record.owned.size();
    res.painting              = std::move(out);
    res.record                = std::move(record);
    res.ok                    = true;
    return res;
}

bool airbrush_bake_volume(ModelVolume &volume, const AirbrushBakeParams &params, const LadderLookup &ladder, AirbrushBakeStats *stats,
                          OnForeignPaintEdit on_edit)
{
    if (!volume.airbrush_field)
        return false;
    const BakeRecord &rec = volume.airbrush_field->record();
    if (on_edit == OnForeignPaintEdit::Refuse && rec.valid() && rec.paint_hash != paint_hash(volume.mmu_segmentation_facets.get_data()))
        return false;
    AirbrushBakeResult r = airbrush_bake(volume.mesh().its, *volume.airbrush_field, volume.mmu_segmentation_facets.get_data(), params, ladder);
    if (!r.ok)
        return false;
    auto f = std::make_shared<AirbrushField>(*volume.airbrush_field);
    f->record() = std::move(r.record);
    f->set_bake_params(params.to_string());
    f->set_anchor(volume.mesh().its);
    volume.mmu_segmentation_facets.set_data(std::move(r.painting));
    volume.airbrush_field = std::move(f);
    if (stats)
        *stats = r.stats;
    return true;
}

bool airbrush_paint_in_sync(const ModelVolume &volume)
{
    if (!volume.airbrush_field || !volume.airbrush_field->record().valid())
        return false;
    return volume.airbrush_field->record().facets == uint32_t(volume.mesh().its.indices.size()) &&
           volume.airbrush_field->record().paint_hash == paint_hash(volume.mmu_segmentation_facets.get_data());
}

bool airbrush_rebake_after_mesh_edit(ModelVolume &volume, const indexed_triangle_set &old_its,
                                     const TriangleSelector::TriangleSplittingData &old_paint, const Transform3d &old_to_new,
                                     const AirbrushBakeParams &params, const LadderLookup &ladder, AirbrushBakeStats *stats)
{
    if (!volume.airbrush_field)
        return false;
    const AirbrushField &old_field = *volume.airbrush_field;
    // The paint as it was before the airbrush, on the old mesh.
    const bool old_record_ok = old_field.record().valid() && old_field.record().facets == uint32_t(old_its.indices.size());
    const TriangleSelector::TriangleSplittingData old_base = old_record_ok ? restore_under(old_paint, old_field.record()) : old_paint;
    // ... carried onto the new mesh.
    TriangleSelector::TriangleSplittingData new_base;
    if (!old_base.triangles_to_split.empty())
        new_base = TriangleSelector::remap_painting(old_its, old_base, volume.mesh().its, old_to_new.inverse(), std::nullopt);
    auto f = std::make_shared<AirbrushField>(old_field);
    if (!old_to_new.matrix().isIdentity(1e-12))
        f->transform(old_to_new);
    f->record().clear();
    AirbrushBakeResult r = airbrush_bake(volume.mesh().its, *f, new_base, params, ladder);
    if (!r.ok)
        return false;
    f->record() = std::move(r.record);
    f->set_bake_params(params.to_string());
    f->set_anchor(volume.mesh().its);
    volume.mmu_segmentation_facets.set_data(std::move(r.painting));
    volume.airbrush_field = std::move(f);
    if (stats)
        *stats = r.stats;
    return true;
}

// =============================================================================================
// P3: mesh edits outside the gizmo (A4) and filament remap (A1)
// =============================================================================================

namespace {
std::mutex                    g_ladder_mutex;
std::function<LadderLookup()> g_ladder_provider;
} // namespace

void set_ladder_provider(std::function<LadderLookup()> provider)
{
    std::lock_guard<std::mutex> lock(g_ladder_mutex);
    g_ladder_provider = std::move(provider);
}

LadderLookup default_ladder()
{
    std::function<LadderLookup()> provider;
    {
        std::lock_guard<std::mutex> lock(g_ladder_mutex);
        provider = g_ladder_provider;
    }
    return provider ? provider() : LadderLookup();
}

TriangleSelector::TriangleSplittingData airbrush_paint_under(const AirbrushField &field, size_t facets,
                                                            const TriangleSelector::TriangleSplittingData &paint)
{
    if (field.record().valid() && field.record().facets == uint32_t(facets))
        return restore_under(paint, field.record());
    return paint;
}

bool airbrush_carry_field(ModelVolume &volume, const AirbrushField &field, const indexed_triangle_set &old_its,
                          const TriangleSelector::TriangleSplittingData &old_paint, const Transform3d &old_to_new,
                          const TriangleSelector::TriangleSplittingData *base)
{
    const indexed_triangle_set &its = volume.mesh().its;
    if (its.indices.empty() || field.empty()) {
        volume.airbrush_field.reset();
        return false;
    }
    auto f = std::make_shared<AirbrushField>(field);
    if (!old_to_new.matrix().isIdentity(1e-12))
        f->transform(old_to_new);
    f->record().clear();
    // Only a field that reaches the new mesh comes along (a cut half takes the cells on its side).
    {
        BoundingBoxf3 bb;
        for (const stl_vertex &v : its.vertices)
            bb.merge(Vec3d(v.cast<double>()));
        bb.offset(2. * f->cell_size());
        bool any = false;
        for (const auto &kv : f->cells())
            if (bb.contains(f->center_of(kv.first))) {
                any = true;
                break;
            }
        if (!any) {
            volume.airbrush_field.reset();
            return false;
        }
    }
    TriangleSelector::TriangleSplittingData new_base;
    if (base != nullptr)
        new_base = *base;
    else {
        const TriangleSelector::TriangleSplittingData old_under = airbrush_paint_under(field, old_its.indices.size(), old_paint);
        if (!old_under.triangles_to_split.empty())
            new_base = TriangleSelector::remap_painting(old_its, old_under, its, old_to_new.inverse(), std::nullopt);
    }
    AirbrushBakeParams params;
    if (!AirbrushBakeParams::from_string(field.bake_params(), params))
        params = AirbrushBakeParams();
    AirbrushBakeResult r = airbrush_bake(its, *f, new_base, params, default_ladder());
    if (!r.ok) {
        volume.airbrush_field.reset();
        return false;
    }
    f->record() = std::move(r.record);
    f->set_bake_params(params.to_string());
    f->set_anchor(its);
    volume.mmu_segmentation_facets.set_data(std::move(r.painting));
    volume.airbrush_field = std::move(f);
    return true;
}

float airbrush_print_scale(const ModelVolume &volume)
{
    Transform3d t = volume.get_matrix();
    if (const ModelObject *mo = volume.get_object(); mo != nullptr && !mo->instances.empty())
        t = mo->instances.front()->get_matrix() * t;
    const double det = std::abs(t.linear().determinant());
    return det > 0. ? float(std::cbrt(det)) : 1.f;
}

bool airbrush_rebake_after_scale(ModelVolume &volume)
{
    if (!volume.airbrush_field || !volume.is_model_part())
        return false;
    AirbrushBakeParams params;
    if (!AirbrushBakeParams::from_string(volume.airbrush_field->bake_params(), params))
        return false;
    const float s = airbrush_print_scale(volume);
    if (std::abs(s - params.print_scale) <= 0.005f * std::max(s, params.print_scale))
        return false;
    // Exactly 1 stays 1, so an unscaled part writes the same parameter string as before.
    params.print_scale = std::abs(s - 1.f) < 1e-5f ? 1.f : s;
    return airbrush_bake_volume(volume, params, default_ladder());
}

bool airbrush_after_mesh_replaced(ModelVolume &volume, const indexed_triangle_set &old_its,
                                  const TriangleSelector::TriangleSplittingData &old_paint)
{
    if (!volume.airbrush_field)
        return false;
    const std::shared_ptr<const AirbrushField> field = volume.airbrush_field;
    return airbrush_carry_field(volume, *field, old_its, old_paint, Transform3d::Identity());
}

std::shared_ptr<AirbrushField> airbrush_remap_field(const AirbrushField &field, const TriangleMesh &mesh,
                                                    const EnforcerBlockerStateMap &state_map,
                                                    const TriangleSelector::TriangleSplittingData &new_paint)
{
    auto map_id = [&state_map](int id) {
        return id >= 0 && size_t(id) < state_map.size() ? int(state_map[size_t(id)]) : id;
    };
    auto f       = std::make_shared<AirbrushField>(field);
    bool changed = f->remap_slots(map_id);
    // The base filament (the "A" end of the ladder) follows the part's filament.
    AirbrushBakeParams params;
    if (AirbrushBakeParams::from_string(f->bake_params(), params) && params.base_filament > 0) {
        const int nb = map_id(params.base_filament);
        if (nb != params.base_filament && nb > 0) {
            params.base_filament = nb;
            f->set_bake_params(params.to_string());
            changed = true;
        }
    }
    // The under-paint went through the same remap as the paint above it.
    BakeRecord &rec = f->record();
    if (rec.valid() && rec.facets == uint32_t(mesh.its.indices.size()) && !rec.under.triangles_to_split.empty()) {
        TriangleSelector sel(mesh);
        sel.deserialize(rec.under, false);
        sel.remap_triangle_state(state_map);
        TriangleSelector::TriangleSplittingData under = sel.serialize();
        if (!(under == rec.under)) {
            rec.under = std::move(under);
            changed   = true;
        }
    }
    if (rec.valid()) {
        const uint64_t h = paint_hash(new_paint);
        if (h != rec.paint_hash) {
            rec.paint_hash = h;
            changed        = true;
        }
    }
    return changed ? f : nullptr;
}

// =============================================================================================
// Cost (D8)
// =============================================================================================

AirbrushCost airbrush_estimate_cost(const indexed_triangle_set &its, const TriangleSelector::TriangleSplittingData &painting,
                                    const AirbrushCostParams &params, const Transform3d &trafo)
{
    AirbrushCost cost;
    if (its.indices.empty() || !(params.layer_height > 0.) || !(params.first_layer_height > 0.))
        return cost;
    double zmax = -std::numeric_limits<double>::max();
    double zmin = std::numeric_limits<double>::max();
    for (const stl_vertex &v : its.vertices) {
        const double z = (trafo * v.cast<double>()).z();
        zmin           = std::min(zmin, z);
        zmax           = std::max(zmax, z);
    }
    zmin = std::max(zmin, 0.);
    if (!(zmax > zmin))
        return cost;
    // Layer i spans (top(i-1), top(i)], top(0) = first layer height.
    auto layer_top = [&](int i) { return params.first_layer_height + double(i) * params.layer_height; };
    const int nlayers = std::max(1, int(std::ceil((zmax - params.first_layer_height) / params.layer_height - 1e-9)) + 1);
    cost.layers       = nlayers;
    std::vector<std::set<int>> per_layer(static_cast<size_t>(nlayers));
    auto resolve = [&](int state, int layer) { return params.resolve ? params.resolve(state, layer) : state; };
    // The part's own filament prints on every layer it spans (unpainted area, infill).
    for (int i = 0; i < nlayers; ++i)
        if (layer_top(i) > zmin)
            per_layer[size_t(i)].insert(resolve(params.base_filament, i));

    if (!painting.triangles_to_split.empty()) {
        TriangleMesh     mesh(its);
        TriangleSelector sel(mesh);
        sel.deserialize(painting, true);
        for (EnforcerBlockerType st : painted_states(painting)) {
            const indexed_triangle_set f = sel.get_facets_strict(st);
            for (const stl_triangle_vertex_indices &tri : f.indices) {
                double lo = std::numeric_limits<double>::max(), hi = -std::numeric_limits<double>::max();
                for (int k = 0; k < 3; ++k) {
                    const double z = (trafo * f.vertices[size_t(tri[k])].cast<double>()).z();
                    lo             = std::min(lo, z);
                    hi             = std::max(hi, z);
                }
                // The layers whose mid-plane the sub-triangle crosses; a sliver between two
                // mid-planes still prints on the layer containing it.
                auto layer_of = [&](double z) {
                    if (z <= params.first_layer_height)
                        return 0;
                    return std::min(nlayers - 1, int(std::ceil((z - params.first_layer_height) / params.layer_height - 1e-9)));
                };
                const int l0 = layer_of(std::max(lo, 0.)), l1 = layer_of(std::max(hi, 0.));
                for (int l = l0; l <= l1; ++l) {
                    const double top = layer_top(l), mid = top - 0.5 * (l == 0 ? params.first_layer_height : params.layer_height);
                    if ((mid >= lo && mid <= hi) || l0 == l1)
                        per_layer[size_t(l)].insert(resolve(int(st), l));
                }
            }
        }
    }

    std::vector<std::vector<int>> layers(static_cast<size_t>(nlayers));
    for (int l = 0; l < nlayers; ++l)
        layers[size_t(l)].assign(per_layer[size_t(l)].begin(), per_layer[size_t(l)].end());
    return airbrush_cost_from_layers(std::move(layers), params);
}

AirbrushCost airbrush_cost_from_layers(std::vector<std::vector<int>> per_layer, const AirbrushCostParams &params)
{
    AirbrushCost cost;
    cost.layers = int(per_layer.size());
    int loaded  = -1;
    for (std::vector<int> &need : per_layer) {
        std::sort(need.begin(), need.end());
        need.erase(std::unique(need.begin(), need.end()), need.end());
        if (need.size() > 1)
            ++cost.multi_tool_layers;
        if (need.empty())
            continue;
        // Start with what is loaded when this layer needs it, then the rest in id order; end on
        // the last one, as the tool ordering does.
        std::vector<int> order;
        order.reserve(need.size());
        if (std::binary_search(need.begin(), need.end(), loaded))
            order.push_back(loaded);
        for (int id : need)
            if (id != loaded)
                order.push_back(id);
        for (int id : order) {
            if (loaded >= 0 && id != loaded)
                ++cost.swaps;
            loaded = id;
        }
    }
    cost.filaments_per_layer = std::move(per_layer);
    cost.purge_mm3           = double(cost.swaps) * params.flush_mm3 * params.flush_multiplier;
    cost.purge_g             = cost.purge_mm3 / 1000. * params.density_g_cm3;
    cost.swap_seconds        = double(cost.swaps) * params.swap_seconds;
    return cost;
}

AirbrushCost airbrush_merge_costs(const std::vector<AirbrushCost> &parts, const AirbrushCostParams &params)
{
    size_t n = 0;
    for (const AirbrushCost &c : parts)
        n = std::max(n, c.filaments_per_layer.size());
    std::vector<std::vector<int>> layers(n);
    for (const AirbrushCost &c : parts)
        for (size_t l = 0; l < c.filaments_per_layer.size(); ++l)
            layers[l].insert(layers[l].end(), c.filaments_per_layer[l].begin(), c.filaments_per_layer[l].end());
    return airbrush_cost_from_layers(std::move(layers), params);
}

AirbrushCostWarning airbrush_cost_warning(const AirbrushCost &cost, double purge_g_limit, double swap_seconds_limit)
{
    AirbrushCostWarning w;
    w.purge = cost.purge_g > purge_g_limit;
    w.time  = cost.swap_seconds > swap_seconds_limit;
    return w;
}

} // namespace Airbrush
} // namespace Slic3r
