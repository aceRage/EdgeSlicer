// See PaintTransfer.hpp.
#include "PaintTransfer.hpp"

#include "AABBTreeIndirect.hpp"
#include "Model.hpp"
#include "TriangleSelector.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cmath>
#include <memory>
#include <optional>
#include <functional>

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

namespace Slic3r {
namespace PaintTransfer {

// Only a safety floor (mm, in target mesh coords): the resolution comes from the painting being
// copied (see TriangleSelector::resample), and the brush can paint finer than 0.05 with a high
// precision setting.
static constexpr float EDGE_LIMIT = 0.005f;

int effective_filament(const ModelVolume &volume)
{
    if (volume.config.has("extruder") && volume.config.opt_int("extruder") > 0)
        return volume.config.opt_int("extruder");
    const ModelObject *object = volume.get_object();
    if (object != nullptr && object->config.has("extruder") && object->config.opt_int("extruder") > 0)
        return object->config.opt_int("extruder");
    return 1;
}

bool has_painting(const ModelVolume &volume)
{
    return volume.is_fdm_support_painted() || volume.is_seam_painted() || volume.is_mm_painted() || volume.is_fuzzy_skin_painted();
}

bool worth_transferring(const std::vector<Source> &sources)
{
    for (const Source &src : sources)
        if (has_painting(*src.volume) || (!src.cutter && src.filament != sources.front().filament))
            return true;
    return false;
}

void append_object_sources(const ModelObject &object, bool cutter, std::vector<Source> &out)
{
    if (object.instances.empty())
        return;
    const Transform3d instance = object.instances.front()->get_matrix();
    for (const ModelVolume *volume : object.volumes)
        if (volume->is_model_part() && ! volume->mesh().empty()) {
            Source src;
            src.volume   = volume;
            src.to_world = instance * volume->get_matrix();
            src.filament = effective_filament(*volume);
            src.cutter   = cutter;
            out.push_back(src);
        }
}

namespace {

using Layer = FacetsAnnotation ModelVolume::*;

// The paint layers every Orca build has. The colour layer is also where the filament of each
// input goes. Paint-depth / texture-displacement are not carried.
const Layer COLOUR_LAYER = &ModelVolume::mmu_segmentation_facets;
const Layer LAYERS[]     = { &ModelVolume::supported_facets, &ModelVolume::seam_facets,
                             &ModelVolume::mmu_segmentation_facets, &ModelVolume::fuzzy_skin_facets };
// The LayerMask bit of each entry of LAYERS.
const unsigned LAYER_BITS[] = { LAYER_SUPPORT, LAYER_SEAM, LAYER_COLOUR, LAYER_FUZZY };

struct PreparedSource
{
    const Source                 *src = nullptr;
    indexed_triangle_set          world_its; // same facet order as the volume mesh
    AABBTreeIndirect::Tree3f      tree;
    Transform3f                   to_local = Transform3f::Identity();
    float                         area_to_world = 1.f; // source mesh area -> world area
    Eigen::AlignedBox3f           bbox;                // of world_its, to skip sources that are far away
    std::vector<Vec3i32>          neighbors;           // edge neighbours of each facet, for the hint walk
};

struct Hit
{
    int   source = -1;
    int   facet  = -1;
    Vec3f local  = Vec3f::Zero(); // point on the source, in its volume mesh coords
};

} // namespace

struct Transfer::Impl
{
    std::vector<Source>         sources;
    std::vector<PreparedSource> prepared;
    // The painting of each source, per paint layer, loaded on first use.
    std::vector<std::vector<std::unique_ptr<TriangleSelector>>> selectors; // [layer][source]
    std::vector<bool>                                           loaded;    // [layer]
};

Transfer::Transfer(std::vector<Source> sources) : m(new Impl)
{
    m->sources = std::move(sources);
    m->prepared.resize(m->sources.size());
    for (size_t i = 0; i < m->sources.size(); ++i) {
        PreparedSource &p = m->prepared[i];
        p.src             = &m->sources[i];
        p.world_its       = m->sources[i].volume->mesh().its;
        its_transform(p.world_its, m->sources[i].to_world);
        p.tree          = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(p.world_its.vertices, p.world_its.indices);
        p.neighbors = its_face_neighbors(p.world_its);
        p.bbox.setEmpty();
        for (const Vec3f &v : p.world_its.vertices)
            p.bbox.extend(v);
        p.to_local      = m->sources[i].to_world.inverse().cast<float>();
        p.area_to_world = float(std::pow(std::abs(m->sources[i].to_world.linear().determinant()), 2. / 3.));
    }
    m->selectors.resize(std::size(LAYERS));
    m->loaded.assign(std::size(LAYERS), false);
}

Transfer::~Transfer() = default;

Stats transfer(const std::vector<Source> &sources, ModelVolume &target, const Transform3d &target_to_world, int base_filament,
               unsigned layers)
{
    const auto t0 = std::chrono::steady_clock::now();
    Transfer   transfer(sources);
    const long prepare_ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    Stats      stats      = transfer.to(target, target_to_world, base_filament, nullptr, layers);
    stats.prepare_ms      = prepare_ms;
    return stats;
}

// Where the bits of each facet are in serialized painting: facet -> [begin, end), {-1, -1} for a
// facet with none. `num_facets` is the facet count of the mesh the painting belongs to.
using FacetBits = std::vector<std::pair<int, int>>;
static FacetBits bits_by_facet(const TriangleSelector::TriangleSplittingData &data, size_t num_facets)
{
    std::vector<TriangleSelector::TriangleBitStreamMapping> by_start = data.triangles_to_split;
    std::sort(by_start.begin(), by_start.end(), [](const auto &l, const auto &r) { return l.bitstream_start_idx < r.bitstream_start_idx; });
    FacetBits bits(num_facets, { -1, -1 });
    for (size_t i = 0; i < by_start.size(); ++i)
        if (by_start[i].triangle_idx >= 0 && size_t(by_start[i].triangle_idx) < num_facets)
            bits[by_start[i].triangle_idx] = { by_start[i].bitstream_start_idx,
                                               i + 1 < by_start.size() ? by_start[i + 1].bitstream_start_idx : int(data.bitstream.size()) };
    return bits;
}

// Appends facet `t`'s painting, the bits [range) of `stream`, to `out`.
static void append_facet_bits(TriangleSelector::TriangleSplittingData &out, int t, const std::vector<bool> &stream, const std::pair<int, int> &range)
{
    if (range.first < 0 || range.second <= range.first)
        return;
    out.triangles_to_split.emplace_back(t, int(out.bitstream.size()));
    out.bitstream.insert(out.bitstream.end(), stream.begin() + range.first, stream.begin() + range.second);
}

// Stores painting spliced together from whole facet trees. The trees are copied verbatim, so only
// the used states need working out; a stream that does not parse goes through a selector instead.
static void store_spliced(FacetsAnnotation &to, const ModelVolume &target, TriangleSelector::TriangleSplittingData &&out)
{
    if (out.triangles_to_split.empty()) {
        to.reset();
        return;
    }
    out.reset_used_states();
    if (out.update_used_states(0)) {
        to.set_data(std::move(out));
        return;
    }
    TriangleSelector selector(target.mesh());
    selector.deserialize(out);
    to.set(selector);
}

Stats Transfer::to(ModelVolume &target, const Transform3d &target_to_world, int base_filament, const std::vector<int> *facet_map,
                   unsigned layers)
{
    Stats stats;
    const std::vector<Source>         &sources  = m->sources;
    const std::vector<PreparedSource> &prepared = m->prepared;
    if (sources.empty() || target.mesh().empty())
        return stats;
    // A facet copied as it is keeps the source's painting, so this only holds for one source that
    // prints with the target's filament (else its unpainted skin would need colouring).
    if (facet_map && (sources.size() != 1 || facet_map->size() != target.mesh().its.indices.size() ||
                      sources.front().filament != base_filament))
        facet_map = nullptr;
    std::vector<bool> copied;
    if (facet_map) {
        copied.resize(facet_map->size());
        for (size_t i = 0; i < facet_map->size(); ++i)
            copied[i] = (*facet_map)[i] >= 0 || (*facet_map)[i] == FACET_UNPAINTED;
    }

    // Two skins closer than this count as the same place, and the earlier source wins.
    const BoundingBoxf3 bbox = target.mesh().transformed_bounding_box(target_to_world);
    const float         tie  = std::max(1e-4f, float(bbox.size().norm()) * 1e-6f);
    // The result's skin lies on the skin of the sources (the boolean only nudges vertices a few
    // microns). Anything farther than this has no source and stays unpainted.
    const float         max_dist = std::max(0.05f, float(bbox.size().norm()) * 1e-3f);
    const Transform3f   t2w  = target_to_world.cast<float>();
    const float         world_to_target_area = 1.f / float(std::pow(std::abs(target_to_world.linear().determinant()), 2. / 3.));

    // The sampler runs in parallel (TriangleSelector::resample): everything it touches is read
    // only, except this counter.
    tbb::enumerable_thread_specific<size_t> samples(0), searches(0), tie_searches(0);
    auto ms_since = [](std::chrono::steady_clock::time_point t) {
        return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count());
    };
    // Nearest point on source `i`, searching its whole mesh.
    auto query = [&](size_t i, const Vec3f &world, float &d, int &facet_out, Vec3f &on_mesh) {
        size_t      facet = 0;
        const float d2    = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
            prepared[i].world_its.vertices, prepared[i].world_its.indices, prepared[i].tree, world, facet, on_mesh);
        if (d2 < 0.f)
            return false;
        d         = std::sqrt(d2);
        facet_out = int(facet);
        return true;
    };

    // The boolean / cut only cuts along the line where the inputs meet, so nearly every triangle of
    // the result lies inside ONE source triangle. Each thread remembers the last source triangle it
    // hit and tries it first: a point-in-triangle test in place of a search over the whole mesh.
    struct HintCache { const void *owner = nullptr; uint64_t call = 0; int source = -1; int facet = -1; };
    static std::atomic<uint64_t> calls { 0 };
    const uint64_t               this_call = ++calls;
    thread_local HintCache       hint;
    // Distance of a recent point to the sources listed before the one it hit, for the tie check.
    struct TieCache { const void *owner = nullptr; uint64_t call = 0; int source = -1; Vec3f p = Vec3f::Zero(); float d = 0.f; };
    thread_local TieCache        tie_far;

    // Whether `world` lies on facet `facet` of source `src` (within the tie distance of its plane,
    // inside its edges); if so, the projected point and the distance.
    auto on_facet = [&](int src, int facet, const Vec3f &world, Vec3f &proj, float &dist) {
        const PreparedSource              &p = prepared[src];
        const stl_triangle_vertex_indices &f = p.world_its.indices[facet];
        const Vec3f a = p.world_its.vertices[f[0]], b = p.world_its.vertices[f[1]], c = p.world_its.vertices[f[2]];
        Vec3f       n   = (b - a).cross(c - a);
        const float len = n.norm();
        if (len < 1e-12f)
            return false;
        n /= len;
        dist = (world - a).dot(n);
        if (std::abs(dist) > tie)
            return false;
        proj = world - n * dist;
        const Vec3f v0 = b - a, v1 = c - a, v2 = proj - a;
        const float d00 = v0.dot(v0), d01 = v0.dot(v1), d11 = v1.dot(v1), d20 = v2.dot(v0), d21 = v2.dot(v1);
        const float den = d00 * d11 - d01 * d01;
        if (std::abs(den) < 1e-20f)
            return false;
        const float v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
        return std::min({ 1.f - v - w, v, w }) >= -1e-4f;
    };

    auto on_hint = [&](const Vec3f &world, Hit &hit, float &d) {
        if (hint.owner != &prepared || hint.call != this_call || hint.source < 0)
            return false;
        const PreparedSource &p = prepared[hint.source];
        Vec3f proj;
        float dist  = 0.f;
        int   facet = hint.facet;
        if (! on_facet(hint.source, facet, world, proj, dist)) {
            facet = -1;
            for (int k = 0; k < 3 && facet < 0; ++k) {
                const int nb = p.neighbors[hint.facet](k);
                if (nb >= 0 && on_facet(hint.source, nb, world, proj, dist))
                    facet = nb;
            }
            if (facet < 0)
                return false;
        }
        if (hint.source > 0) {
            const float need = std::abs(dist) + tie;
            if (! (tie_far.owner == &prepared && tie_far.call == this_call && tie_far.source == hint.source &&
                   tie_far.d - (world - tie_far.p).norm() > need)) {
                float nearest = std::numeric_limits<float>::max();
                for (int i = 0; i < hint.source; ++i) {
                    const float box_d = prepared[i].bbox.exteriorDistance(world);
                    if (box_d > need) {
                        nearest = std::min(nearest, box_d);
                        continue;
                    }
                    float e; int ef; Vec3f eon;
                    ++tie_searches.local();
                    if (query(size_t(i), world, e, ef, eon)) {
                        if (e <= need)
                            return false;
                        nearest = std::min(nearest, e);
                    }
                }
                tie_far = TieCache{ &prepared, this_call, hint.source, world, nearest };
            }
        }
        hit.source = hint.source;
        hit.facet  = facet;
        hit.local  = p.to_local * proj;
        d          = std::abs(dist);
        hint.facet = facet;
        return true;
    };

    auto closest = [&](const Vec3f &target_pt) {
        ++samples.local();
        const Vec3f world = t2w * target_pt;
        Hit         best;
        float       best_d = 0.f;
        if (on_hint(world, best, best_d))
            return best;
        ++searches.local();
        best_d = std::numeric_limits<float>::max();
        for (size_t i = 0; i < prepared.size(); ++i) {
            if (best.source >= 0 && prepared[i].bbox.exteriorDistance(world) > best_d)
                continue;
            float d; int facet; Vec3f on_mesh;
            if (! query(i, world, d, facet, on_mesh))
                continue;
            if (best.source < 0 || d < best_d - tie) {
                best_d      = d;
                best.source = int(i);
                best.facet  = facet;
                best.local  = prepared[i].to_local * on_mesh;
            }
        }
        if (best.source >= 0 && best_d > max_dist)
            best.source = -1;
        if (best.source >= 0)
            hint = HintCache{ &prepared, this_call, best.source, best.facet };
        return best;
    };

    for (size_t layer_idx = 0; layer_idx < std::size(LAYERS); ++layer_idx) {
        if ((layers & LAYER_BITS[layer_idx]) == 0)
            continue;
        const Layer layer  = LAYERS[layer_idx];
        const bool  colour = layer == COLOUR_LAYER;

        std::vector<std::unique_ptr<TriangleSelector>> &selectors = m->selectors[layer_idx];
        const auto t_load = std::chrono::steady_clock::now();
        if (! m->loaded[layer_idx]) {
            selectors.resize(sources.size());
            for (size_t i = 0; i < sources.size(); ++i) {
                const FacetsAnnotation &fa = sources[i].volume->*layer;
                if (! fa.empty()) {
                    selectors[i] = std::make_unique<TriangleSelector>(sources[i].volume->mesh());
                    selectors[i]->deserialize(fa.get_data());
                }
            }
            m->loaded[layer_idx] = true;
        }
        stats.load_ms += ms_since(t_load);
        bool anything = false;
        for (size_t i = 0; i < sources.size(); ++i)
            if (selectors[i] || (colour && ! sources[i].cutter && sources[i].filament != base_filament))
                anything = true;
        if (! anything) {
            (target.*layer).reset();
            continue;
        }

        const auto       t_resample = std::chrono::steady_clock::now();
        // With a facet map, only the facets the cut split need sampling.
        const bool       sample = ! facet_map || std::find(copied.begin(), copied.end(), false) != copied.end();
        std::optional<TriangleSelector> selector;
        if (sample) {
            selector.emplace(target.mesh());
            selector->resample([&](const Vec3f &pt) {
                TriangleSelector::SampledState out;
                const Hit hit = closest(pt);
                if (hit.source < 0 || sources[hit.source].cutter)
                    return out;
                if (selectors[hit.source]) {
                    out.state = selectors[hit.source]->leaf_state_at(hit.facet, hit.local, &out.leaf_area);
                    out.leaf_area *= prepared[hit.source].area_to_world * world_to_target_area;
                }
                if (colour && out.state == EnforcerBlockerType::NONE && sources[hit.source].filament != base_filament)
                    out.state = EnforcerBlockerType(sources[hit.source].filament);
                return out;
            }, EDGE_LIMIT, facet_map ? &copied : nullptr);
        }
        stats.resample_ms += ms_since(t_resample);

        const auto t_store = std::chrono::steady_clock::now();
        if (facet_map) {
            // Splice: copied facets take the source's bits, the others the resampled ones.
            const FacetsAnnotation &from       = sources.front().volume->*layer;
            const size_t            src_facets = sources.front().volume->mesh().its.indices.size();
            const TriangleSelector::TriangleSplittingData resampled = sample ? selector->serialize() : TriangleSelector::TriangleSplittingData{};
            const FacetBits src_bits = bits_by_facet(from.get_data(), src_facets);
            const FacetBits new_bits = bits_by_facet(resampled, facet_map->size());
            TriangleSelector::TriangleSplittingData out;
            for (int t = 0; t < int(facet_map->size()); ++t) {
                const int src_facet = (*facet_map)[t];
                if (src_facet >= 0) {
                    if (size_t(src_facet) < src_facets)
                        append_facet_bits(out, t, from.get_data().bitstream, src_bits[src_facet]);
                } else if (src_facet != FACET_UNPAINTED)
                    append_facet_bits(out, t, resampled.bitstream, new_bits[t]);
            }
            store_spliced(target.*layer, target, std::move(out));
        } else
            (target.*layer).set(*selector);
        stats.store_ms += ms_since(t_store);
        ++stats.layers;
    }
    stats.samples      = samples.combine(std::plus<size_t>());
    stats.searches     = searches.combine(std::plus<size_t>());
    stats.tie_searches = tie_searches.combine(std::plus<size_t>());
    return stats;
}


namespace {
// The three corners of a facet, bit for bit.
struct FacetKey
{
    std::array<float, 9> v;
    bool operator==(const FacetKey &o) const { return std::memcmp(v.data(), o.v.data(), sizeof(float) * 9) == 0; }
};
struct FacetKeyHash
{
    size_t operator()(const FacetKey &k) const
    {
        size_t h = 1469598103934665603ull;
        for (float f : k.v) {
            uint32_t u;
            std::memcpy(&u, &f, sizeof(u));
            h = (h ^ u) * 1099511628211ull;
        }
        return h;
    }
};
FacetKey facet_key(const indexed_triangle_set &its, size_t facet)
{
    FacetKey k;
    for (int c = 0; c < 3; ++c)
        for (int d = 0; d < 3; ++d)
            k.v[c * 3 + d] = its.vertices[its.indices[facet][c]](d);
    return k;
}
} // namespace

// The source's facets sorted by the hash of their corners (16 bytes a facet, not a hash map of
// the 36-byte keys): a 1M-facet mesh is matched without a 100 MB table.
struct FacetMatcher::Impl
{
    const indexed_triangle_set           *source = nullptr;
    std::vector<std::pair<size_t, int>>   by_hash;
};

FacetMatcher::FacetMatcher(const indexed_triangle_set &source) : m(new Impl)
{
    m->source = &source;
    m->by_hash.resize(source.indices.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, source.indices.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i)
            m->by_hash[i] = { FacetKeyHash()(facet_key(source, i)), int(i) };
    });
    std::sort(m->by_hash.begin(), m->by_hash.end());
}

FacetMatcher::~FacetMatcher() = default;

std::vector<int> FacetMatcher::match(const indexed_triangle_set &piece) const
{
    std::vector<int> map(piece.indices.size(), -1);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, piece.indices.size()), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const FacetKey key = facet_key(piece, i);
            const size_t   h   = FacetKeyHash()(key);
            auto it = std::lower_bound(m->by_hash.begin(), m->by_hash.end(), std::make_pair(h, std::numeric_limits<int>::min()));
            for (; it != m->by_hash.end() && it->first == h; ++it)
                if (facet_key(*m->source, size_t(it->second)) == key) {
                    map[i] = it->second;
                    break;
                }
        }
    });
    return map;
}

bool copy_painting(const ModelVolume &source, ModelVolume &target, const std::vector<int> &facet_map, unsigned layers)
{
    if (facet_map.size() != target.mesh().its.indices.size() ||
        std::any_of(facet_map.begin(), facet_map.end(), [](int f) { return f < 0; }))
        return false;

    for (size_t layer_idx = 0; layer_idx < std::size(LAYERS); ++layer_idx) {
        if ((layers & LAYER_BITS[layer_idx]) == 0)
            continue;
        const Layer             layer = LAYERS[layer_idx];
        const FacetsAnnotation &from = source.*layer;
        FacetsAnnotation       &to   = target.*layer;
        if (from.empty()) {
            to.reset();
            continue;
        }
        const TriangleSelector::TriangleSplittingData &data       = from.get_data();
        const size_t                                   src_facets = source.mesh().its.indices.size();
        const FacetBits                                bits       = bits_by_facet(data, src_facets);

        TriangleSelector::TriangleSplittingData out;
        for (size_t t = 0; t < facet_map.size(); ++t)
            if (facet_map[t] >= 0 && size_t(facet_map[t]) < src_facets)
                append_facet_bits(out, int(t), data.bitstream, bits[facet_map[t]]);
        store_spliced(to, target, std::move(out));
    }
    return true;
}

std::vector<Source> object_boolean_sources(const ModelObject &object)
{
    std::vector<Source> out;
    for (const ModelVolume *v : object.volumes)
        if ((v->is_model_part() || v->is_negative_volume()) && ! v->mesh().empty()) {
            Source src;
            src.volume   = v;
            src.to_world = v->get_matrix();
            src.filament = effective_filament(*v);
            src.cutter   = v->is_negative_volume();
            out.push_back(src);
        }
    return out;
}

std::vector<Source> part_boolean_sources(const ModelVolume &source, const ModelVolume &tool, bool difference)
{
    std::vector<Source> out(2);
    out[0].volume   = &source;
    out[0].to_world = source.get_matrix();
    out[0].filament = effective_filament(source);
    out[1].volume   = &tool;
    out[1].to_world = tool.get_matrix();
    out[1].filament = effective_filament(tool);
    out[1].cutter   = difference;
    return out;
}

bool paint_boolean_result(const std::vector<Source> &sources, ModelVolume &result)
{
    if (sources.empty() || result.mesh().empty() || ! worth_transferring(sources))
        return false;
    unsigned layers = ALL_LAYERS;
    for (const Source &src : sources)
        if (src.volume != nullptr && src.volume->airbrush_field)
            layers &= ~unsigned(LAYER_COLOUR);
    transfer(sources, result, result.get_matrix(), effective_filament(result), layers);
    return true;
}

struct SplitPainter::Impl
{
    const ModelVolume                *source = nullptr;
    Transform3d                       source_to_world = Transform3d::Identity();
    unsigned                          layers = ALL_LAYERS;
    // The mesh the matcher refers to: the source's own, kept alive here.
    std::shared_ptr<const TriangleMesh> mesh;
    std::unique_ptr<FacetMatcher>     matcher;
    std::unique_ptr<Transfer>         fallback; // built on the first piece that needs it
};

SplitPainter::SplitPainter(const ModelVolume &source, const Transform3d &source_to_world, unsigned layers) : m(new Impl)
{
    m->source          = &source;
    m->source_to_world = source_to_world;
    m->layers          = layers;
    m->mesh            = std::make_shared<const TriangleMesh>(source.mesh());
    m->matcher         = std::make_unique<FacetMatcher>(m->mesh->its);
}

SplitPainter::~SplitPainter() = default;

std::vector<int> SplitPainter::match(const indexed_triangle_set &piece) const { return m->matcher->match(piece); }

bool SplitPainter::paint(ModelVolume &piece, const std::vector<int> &map, const Transform3d &piece_to_world)
{
    if (copy_painting(*m->source, piece, map, m->layers))
        return true;
    if (! m->fallback) {
        Source src;
        src.volume   = m->source;
        src.to_world = m->source_to_world;
        src.filament = effective_filament(*m->source);
        m->fallback  = std::make_unique<Transfer>(std::vector<Source>{ src });
    }
    // The piece prints with the source's filament: its unpainted skin stays unpainted. The facets
    // that did match are still copied; only the others are resampled.
    m->fallback->to(piece, piece_to_world, effective_filament(*m->source), &map, m->layers);
    return false;
}

} // namespace PaintTransfer
} // namespace Slic3r
