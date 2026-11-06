// Mixed nozzle diameters, AdaptiveLayerMerge step 4 (supports).
// Ported from BambuStudio v02.09.01.70 src/libslic3r/SupportMergePlan.cpp (AGPL-3.0).

#include "SupportMergePlan.hpp"

#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Layer.hpp"
#include "../NozzleProcess.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../libslic3r.h"
#include "../ClipperUtils.hpp"
#include "SupportParameters.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace Slic3r {

static std::vector<SupportMergePlan::Candidate> candidates_in_span(
    const SupportMergePlan                                       &plan,
    const std::vector<std::vector<SupportMergePlan::Candidate>> &candidates_per_layer,
    double                                                        bottom_z,
    double                                                        top_z)
{
    std::vector<SupportMergePlan::Candidate> out;
    if (! plan.enabled)
        return out;

    // An object layer overlaps the span when it sticks out above bottom_z and starts below top_z.
    const auto first = std::lower_bound(plan.layer_print_z.begin(), plan.layer_print_z.end(), bottom_z + EPSILON);
    for (size_t idx = size_t(first - plan.layer_print_z.begin()); idx < plan.layer_print_z.size(); ++ idx) {
        if (plan.layer_print_z[idx] - plan.layer_heights[idx] > top_z - EPSILON)
            break;
        for (const SupportMergePlan::Candidate &candidate : candidates_per_layer[idx]) {
            auto same = std::find_if(out.begin(), out.end(),
                [&candidate](const SupportMergePlan::Candidate &other) { return other.filament_id == candidate.filament_id; });
            if (same == out.end())
                out.emplace_back(candidate);
        }
    }
    return out;
}

std::vector<SupportMergePlan::Candidate> SupportMergePlan::candidates_in_span(double bottom_z, double top_z) const
{
    return Slic3r::candidates_in_span(*this, candidates_per_layer, bottom_z, top_z);
}

std::vector<SupportMergePlan::Candidate> SupportMergePlan::candidates_for_slab(double top_z, double height) const
{
    std::vector<Candidate> candidates = candidates_in_span(top_z - height, top_z);
    if (candidates.empty()) {
        // A support layer sits in the gap between two object layers, so a thin span may not overlap any of
        // them at all. Widen it by one object layer rather than give up.
        const double slack = base_height_at(top_z) + EPSILON;
        candidates = candidates_in_span(top_z - height - slack, top_z + slack);
    }
    return candidates;
}

SupportMergePlan::Candidate SupportMergePlan::coarsest_for_slab(double top_z, double height) const
{
    Candidate best;
    for (const Candidate &candidate : candidates_for_slab(top_z, height))
        if (std::abs(fixed_band_height_at(candidate, top_z) - height) < EPSILON)
            if (! best.valid() || candidate.nozzle_diameter > best.nozzle_diameter)
                best = candidate;
    return best;
}

static std::vector<SupportMergePlan::Candidate> interface_candidates_for_slab(const SupportMergePlan &plan, double top_z, double height)
{
    std::vector<SupportMergePlan::Candidate> candidates =
        Slic3r::candidates_in_span(plan, plan.interface_candidates_per_layer, top_z - height, top_z);
    if (candidates.empty()) {
        const double slack = plan.base_height_at(top_z) + EPSILON;
        candidates = Slic3r::candidates_in_span(plan, plan.interface_candidates_per_layer, top_z - height - slack, top_z + slack);
    }
    return candidates;
}

SupportMergePlan::Candidate SupportMergePlan::finest_interface_for_slab(double top_z, double height) const
{
    Candidate best;
    for (const Candidate &candidate : interface_candidates_for_slab(*this, top_z, height))
        if (std::abs(fixed_band_height_at(candidate, top_z) - height) < EPSILON &&
            (! best.valid() || candidate.nozzle_diameter < best.nozzle_diameter))
            best = candidate;
    return best;
}

double SupportMergePlan::max_interface_height(double top_z, double height) const
{
    double max_height = 0.;
    for (const Candidate &candidate : interface_candidates_for_slab(*this, top_z, height))
        max_height = std::max(max_height, fixed_band_height_at(candidate, top_z));
    return max_height;
}

double SupportMergePlan::max_height_for_slab(double top_z, double height) const
{
    double max_height = 0.;
    for (const Candidate &candidate : candidates_for_slab(top_z, height))
        max_height = std::max(max_height, fixed_band_height_at(candidate, top_z));
    return max_height;
}

double SupportMergePlan::base_height_at(double top_z) const
{
    if (! enabled || layer_print_z.empty())
        return 0.;
    // The object layer ending at or just below top_z sets the granularity there.
    auto it = std::upper_bound(layer_print_z.begin(), layer_print_z.end(), top_z + EPSILON);
    if (it == layer_print_z.begin())
        return layer_heights.front();
    return layer_heights[size_t(it - layer_print_z.begin()) - 1];
}

double SupportMergePlan::fixed_band_height_at(const Candidate &candidate, double top_z) const
{
    if (! enabled || layer_print_z.empty())
        return 0.;

    auto layer_it = std::lower_bound(layer_print_z.begin(), layer_print_z.end(), top_z - EPSILON);
    if (layer_it == layer_print_z.end() || *layer_it > top_z + EPSILON)
        return 0.;
    const size_t layer_idx = size_t(layer_it - layer_print_z.begin());
    if (candidate.band_size < 2)
        return layer_heights[layer_idx];

    auto bands_it = fixed_bands_per_filament.find(candidate.filament_id);
    if (bands_it == fixed_bands_per_filament.end())
        return 0.;
    auto band_it = std::lower_bound(bands_it->second.begin(), bands_it->second.end(), layer_idx,
        [](const AdaptiveLayerBand &band, size_t idx) { return band.top_idx < idx; });
    return band_it != bands_it->second.end() && band_it->top_idx == layer_idx ? band_it->merged_height : 0.;
}

const SupportMergePlan::Slab *SupportMergePlan::slab_for_layer(size_t layer_idx) const
{
    if (layer_idx >= slab_index_per_layer.size())
        return nullptr;
    const int slab_idx = slab_index_per_layer[layer_idx];
    return slab_idx < 0 ? nullptr : &slabs[size_t(slab_idx)];
}

// Cut the object layers into the slab partition the classic tree folds onto. Every nozzle anchors its ladder on
// layer 1, so walking up and always taking the band that starts exactly here and reaches highest follows the
// coarsest ladder without ever stepping off it.
static void expand_slabs(SupportMergePlan &plan, const std::vector<std::vector<int>> *extruded_per_layer)
{
    const size_t num_layers = plan.layer_print_z.size();
    plan.slab_index_per_layer.assign(num_layers, -1);
    if (plan.fixed_bands_per_filament.empty())
        return;

    size_t next_bottom = 0;
    while (next_bottom < num_layers) {
        const SupportMergePlan::Candidate *best      = nullptr;
        const AdaptiveLayerBand           *best_band = nullptr;
        for (const auto &entry : plan.fixed_bands_per_filament) {
            const std::vector<AdaptiveLayerBand> &bands = entry.second;
            auto band_it = std::lower_bound(bands.begin(), bands.end(), next_bottom,
                [](const AdaptiveLayerBand &band, size_t idx) { return band.bottom_idx < idx; });
            if (band_it == bands.end() || band_it->bottom_idx != next_bottom || band_it->top_idx >= num_layers)
                continue;
            // The nozzle has to be one the object actually prints with where the band ends, or the support
            // would borrow a tool from a height nothing else uses there.
            const std::vector<SupportMergePlan::Candidate> &at_top = plan.candidates_per_layer[band_it->top_idx];
            auto cand_it = std::find_if(at_top.begin(), at_top.end(),
                [&entry](const SupportMergePlan::Candidate &candidate) { return candidate.filament_id == entry.first; });
            if (cand_it == at_top.end())
                continue;
            // A region keeping slices on a layer does not mean its filament is extruded there: its band may be
            // emitted at another layer. A support filament the user picked is exempt (nullptr).
            if (extruded_per_layer != nullptr) {
                const std::vector<int> &extruded = (*extruded_per_layer)[band_it->top_idx];
                if (std::find(extruded.begin(), extruded.end(), entry.first) == extruded.end())
                    continue;
            }
            // Reaching higher wins, and on a tie the coarser nozzle does.
            if (best_band == nullptr || band_it->top_idx > best_band->top_idx ||
                (band_it->top_idx == best_band->top_idx && cand_it->nozzle_diameter > best->nozzle_diameter)) {
                best      = &*cand_it;
                best_band = &*band_it;
            }
        }
        if (best_band == nullptr) {
            // Nothing starts here, so this layer stays on the fine grid and the cursor moves up by one.
            ++ next_bottom;
            continue;
        }

        SupportMergePlan::Slab slab;
        slab.bottom_idx      = next_bottom;
        slab.top_idx         = best_band->top_idx;
        slab.top_z           = plan.layer_print_z[slab.top_idx];
        slab.height          = best_band->merged_height;
        slab.filament_id     = best->filament_id;
        slab.nozzle_diameter = best->nozzle_diameter;

        const int slab_id = int(plan.slabs.size());
        for (size_t idx = slab.bottom_idx; idx <= slab.top_idx; ++ idx)
            plan.slab_index_per_layer[idx] = slab_id;
        plan.slabs.emplace_back(slab);
        next_bottom = slab.top_idx + 1;
    }
}

// Edge differences from Bambu's make_support_merge_plan(), all so the support ladders are exactly the ladders
// compute_adaptive_merge_groups() stacks the walls on:
//   * the band reference is Print::adaptive_merge_reference_nozzle() (the plate's smallest wall nozzle), not
//     Bambu's layer_grid_nozzle_diameter();
//   * each candidate's window is the per-nozzle clamp adaptive_layer_height_range_for_nozzle(), and its band size
//     walks into that window (the four-argument adaptive_layer_band_size());
//   * the ladders come from build_adaptive_layer_ladder(), so layer 0 is never part of a slab;
//   * the "whatever the object prints with" fallback collects the object's region filaments, not its model
//     volumes' extruders (Edge has no GlobalFeatureFilaments), and the per-layer extruded filaments use the raw
//     region ids (ToolOrdering's mixed-filament resolution is not applied, as Bambu does not apply its override).
SupportMergePlan make_support_merge_plan(const PrintObject &object)
{
    SupportMergePlan plan;

    const Print             *print         = object.print();
    const PrintObjectConfig &object_config = object.config();
    const PrintConfig       &print_config  = print->config();
    if (! print->uses_multiple_nozzle_diameters())
        return plan;
    if (! object.slicing_parameters().valid)
        return plan;

    const ConstLayerPtrsAdaptor layers = object.layers();
    if (layers.size() < 2)
        return plan;

    const double reference_nozzle = print->adaptive_merge_reference_nozzle();
    if (reference_nozzle <= EPSILON)
        return plan;
    const double base_height = object.slicing_parameters().layer_height;

    plan.layer_print_z.reserve(layers.size());
    plan.layer_heights.reserve(layers.size());
    for (const Layer *layer : layers) {
        plan.layer_print_z.emplace_back(layer->print_z);
        plan.layer_heights.emplace_back(layer->height);
    }

    std::vector<unsigned int> used_filaments;
    print->used_printing_filaments(used_filaments);

    auto is_soluble = [&print_config](int filament_id) {
        return filament_id > 0 && size_t(filament_id - 1) < print_config.filament_soluble.values.size() &&
               print_config.filament_soluble.get_at(size_t(filament_id - 1));
    };
    auto make_candidate = [&](int filament_id) {
        SupportMergePlan::Candidate candidate;
        candidate.filament_id     = filament_id;
        candidate.nozzle_diameter = nozzle_diameter_for_filament(print_config, (unsigned int) filament_id);
        candidate.window          = adaptive_layer_height_range_for_nozzle(print_config, (unsigned int) filament_id, used_filaments);
        candidate.band_size       = candidate.nozzle_diameter > EPSILON ?
            adaptive_layer_band_size(candidate.nozzle_diameter, reference_nozzle, base_height, candidate.window) : 1;
        // H3C fix: a process chosen for a coarser nozzle sets its slab height, as for the walls (compute_adaptive_merge_groups).
        if (candidate.nozzle_diameter > reference_nozzle + EPSILON)
            candidate.band_size = nozzle_process_band_size(nozzle_layer_height_for_filament(print_config, (unsigned int) filament_id),
                                                           candidate.band_size, base_height, candidate.window.min_height,
                                                           candidate.window.max_height);
        return candidate;
    };
    auto ensure_fixed_grid = [&plan](const SupportMergePlan::Candidate &candidate) {
        if (candidate.band_size > 1 && plan.fixed_bands_per_filament.find(candidate.filament_id) == plan.fixed_bands_per_filament.end())
            plan.fixed_bands_per_filament.emplace(candidate.filament_id,
                build_adaptive_layer_ladder(plan.layer_heights, candidate.band_size, candidate.window));
    };
    auto add_candidate = [&](std::vector<SupportMergePlan::Candidate> &candidates, int filament_id, bool allow_soluble) {
        if (filament_id <= 0 || (! allow_soluble && is_soluble(filament_id)))
            return;
        auto same = std::find_if(candidates.begin(), candidates.end(),
            [filament_id](const SupportMergePlan::Candidate &other) { return other.filament_id == filament_id; });
        if (same != candidates.end())
            return;
        const SupportMergePlan::Candidate candidate = make_candidate(filament_id);
        if (candidate.nozzle_diameter > EPSILON) {
            candidates.emplace_back(candidate);
            ensure_fixed_grid(candidate);
        }
    };

    plan.candidates_per_layer.resize(layers.size());
    plan.interface_candidates_per_layer.resize(layers.size());

    // A filament value of 0 means "whatever the object prints with". Resolve those choices per object layer,
    // because a fine nozzle available elsewhere in the object must not keep a thin support contact at this Z.
    std::vector<SupportMergePlan::Candidate> object_filaments;
    for (size_t region_id = 0; region_id < object.num_printing_regions(); ++ region_id) {
        const PrintRegionConfig &region_config = object.printing_region(region_id).config();
        add_candidate(object_filaments, region_config.wall_filament.value, false);
        add_candidate(object_filaments, region_config.sparse_infill_filament.value, false);
        add_candidate(object_filaments, region_config.solid_infill_filament.value, false);
    }

    std::vector<std::vector<SupportMergePlan::Candidate>> object_candidates(layers.size());
    // Filaments the object actually extrudes per layer, collected like ToolOrdering::collect_extruders(): walls
    // only where perimeters exist, one slot per fill collection. Listing both infill filaments whenever any fill
    // exists would pin the slab to a filament this height never extrudes.
    std::vector<std::vector<int>> extruded_per_layer(layers.size());
    for (size_t layer_idx = 0; layer_idx < layers.size(); ++ layer_idx) {
        std::vector<SupportMergePlan::Candidate> &candidates = object_candidates[layer_idx];
        for (const LayerRegion *layerm : layers[layer_idx]->regions()) {
            if (layerm->slices.empty())
                continue;
            const PrintRegionConfig &region_config = layerm->region().config();
            std::vector<int> &extruded = extruded_per_layer[layer_idx];
            if (! layerm->perimeters.empty())
                extruded.emplace_back(region_config.wall_filament.value);
            for (const ExtrusionEntity *ee : layerm->fills.entities) {
                const auto *fill = dynamic_cast<const ExtrusionEntityCollection *>(ee);
                if (fill == nullptr || fill->empty())
                    continue;
                const ExtrusionRole role = fill->entities.empty() ? erNone : fill->entities.front()->role();
                // LayerTools::extruder(): 100 % sparse infill prints its internal solid with the sparse filament.
                const bool sparse_for_solid = role == erSolidInfill &&
                    std::abs(region_config.sparse_infill_density.value - 100.) < EPSILON;
                extruded.emplace_back(is_solid_infill(role) && ! sparse_for_solid ?
                    region_config.solid_infill_filament.value : region_config.sparse_infill_filament.value);
            }
            add_candidate(candidates, region_config.wall_filament.value, false);
            add_candidate(candidates, region_config.sparse_infill_filament.value, false);
            add_candidate(candidates, region_config.solid_infill_filament.value, false);
        }
        if (candidates.empty() && ! layers[layer_idx]->empty())
            candidates = object_filaments;
    }

    const int support_filament = object_config.support_filament.value;
    if (support_filament > 0) {
        if (! is_soluble(support_filament)) {
            const SupportMergePlan::Candidate candidate = make_candidate(support_filament);
            if (candidate.nozzle_diameter > EPSILON) {
                ensure_fixed_grid(candidate);
                for (std::vector<SupportMergePlan::Candidate> &candidates : plan.candidates_per_layer)
                    candidates.emplace_back(candidate);
            }
        }
    } else {
        plan.candidates_per_layer = object_candidates;
    }

    const int interface_filament = object_config.support_interface_filament.value;
    if (interface_filament > 0) {
        const SupportMergePlan::Candidate candidate = make_candidate(interface_filament);
        if (candidate.nozzle_diameter > EPSILON) {
            ensure_fixed_grid(candidate);
            for (std::vector<SupportMergePlan::Candidate> &candidates : plan.interface_candidates_per_layer)
                candidates.emplace_back(candidate);
        }
    } else {
        plan.interface_candidates_per_layer = std::move(object_candidates);
    }

    expand_slabs(plan, support_filament > 0 ? nullptr : &extruded_per_layer);

    // The roof is printed with the pinned interface filament, so it may only be folded onto a slab that filament
    // can actually extrude.
    if (interface_filament > 0) {
        const double interface_nozzle = nozzle_diameter_for_filament(print_config, (unsigned int) interface_filament);
        plan.interface_fold_enabled = ! plan.slabs.empty() && interface_nozzle > EPSILON;
        for (const SupportMergePlan::Slab &slab : plan.slabs)
            if (std::abs(slab.nozzle_diameter - interface_nozzle) > EPSILON) {
                plan.interface_fold_enabled = false;
                break;
            }
    }

    plan.enabled = true;
    return plan;
}

// ---- Merging the generated support areas (BambuStudio v02.09.01.70 SupportCommon.cpp:1495-2176) ----------------

// The mergeable layer arrays, in the order merge_support_layers_multi_diameter() indexes them.
enum SupportMergeArray { smaBottomContact = 0, smaTopContact, smaIntermediate, smaInterface, smaBaseInterface, smaCount };
// Everything but the sparse base faces the object and follows the interface rule.
static constexpr const bool support_merge_array_is_interface[smaCount] { true, true, false, true, true };

namespace {

// One print_z carrying support areas, together with the layer of every array that shares that print_z.
struct SupportMergeLevel
{
    coordf_t print_z { 0. };
    // Below the raft contact / the first object layer, or holding geometry Edge's own support features own
    // (interlock weave): never touched.
    bool     frozen { false };
    bool     has_raft { false };
    // Index into the corresponding array, or -1.
    int      layer_idx[smaCount] { -1, -1, -1, -1, -1 };
};

} // namespace

Polygons merged_band_footprint(const Layer &layer, size_t layer_idx)
{
    Polygons band;
    for (const LayerRegion *layerm : layer.regions())
        for (const AdaptiveMergePatch &patch : layerm->adaptive_merge_patches)
            if (patch.bottom_idx <= layer_idx && layer_idx <= patch.top_idx)
                polygons_append(band, to_polygons(patch.area()));
    return band;
}

void merge_support_layers_multi_diameter(
    const PrintObject               &object,
    const SupportParameters         &support_params,
    const SlicingParameters         &slicing_params,
    const SupportGeneratorLayersPtr &raft_layers,
    SupportGeneratorLayersPtr       &bottom_contacts,
    SupportGeneratorLayersPtr       &top_contacts,
    SupportGeneratorLayersPtr       &intermediate_layers,
    SupportGeneratorLayersPtr       &interface_layers,
    SupportGeneratorLayersPtr       &base_interface_layers,
    SupportMergeResult              &result,
    coordf_t                         top_gap,
    bool                             support_groups_active)
{
    // Edge: a disabled plan (every single-diameter plate) returns before anything is touched, so such a plate's
    // support arrays reach generate_support_layers() exactly as they always did.
    if (support_groups_active)
        return;
    const SupportMergePlan plan = make_support_merge_plan(object);
    if (! plan.enabled)
        return;
    if (top_gap < 0.)
        top_gap = slicing_params.gap_support_object;

    std::map<coordf_t, SupportMergePlan::Candidate> &planned_filaments           = result.planned_filaments;
    std::map<coordf_t, SupportMergePlan::Candidate> &planned_interface_filaments = result.planned_interface_filaments;
    std::map<coordf_t, coordf_t>                     &planned_base_heights        = result.planned_base_heights;
    std::map<coordf_t, coordf_t>                     &planned_interface_heights   = result.planned_interface_heights;

    SupportGeneratorLayersPtr *arrays[smaCount] {
        &bottom_contacts, &top_contacts, &intermediate_layers, &interface_layers, &base_interface_layers };

    // Folding areas together leaves holes in the arrays, and generate_support_layers() cannot cope with those.
    auto compact_arrays = [&arrays]() {
        for (int array_id = 0; array_id < smaCount; ++ array_id) {
            SupportGeneratorLayersPtr &layers = *arrays[array_id];
            layers.erase(std::remove(layers.begin(), layers.end(), nullptr), layers.end());
        }
    };

    // Keep raft and bridging-contact geometry fixed. Edge never merges object layer 0, so everything up to the
    // first object layer stays as generated too.
    const coordf_t z_frozen_max = std::max(slicing_params.raft_contact_top_z, object.layers().front()->print_z) + EPSILON;

    std::vector<SupportMergeLevel> levels;
    std::multimap<coordf_t, size_t> level_index;
    auto level_at = [&levels, &level_index](coordf_t print_z) -> SupportMergeLevel & {
        size_t found = std::numeric_limits<size_t>::max();
        for (auto it = level_index.upper_bound(print_z - EPSILON); it != level_index.end() && it->first < print_z + EPSILON; ++ it)
            if (std::abs(it->first - print_z) < EPSILON)
                found = std::min(found, it->second);
        if (found == std::numeric_limits<size_t>::max()) {
            found = levels.size();
            levels.emplace_back();
            levels.back().print_z = print_z;
            level_index.emplace(print_z, found);
        }
        return levels[found];
    };

    for (const SupportGeneratorLayer *layer : raft_layers)
        if (layer != nullptr && ! layer->polygons.empty()) {
            SupportMergeLevel &level = level_at(layer->print_z);
            level.frozen   = true;
            level.has_raft = true;
        }

    for (int array_id = 0; array_id < smaCount; ++ array_id)
        for (int i = 0; i < int(arrays[array_id]->size()); ++ i) {
            SupportGeneratorLayer *layer = (*arrays[array_id])[i];
            if (layer == nullptr || layer->polygons.empty())
                continue;
            SupportMergeLevel &level = level_at(layer->print_z);
            // A bridging layer hangs at a fixed gap above the object, so print_z - height is where its
            // extrusion starts rather than where the layer starts. Growing it would close that gap.
            // Edge: the support interface interlock weaves stripes through the base; such a level keeps its layers.
            level.frozen |= layer->print_z < z_frozen_max || layer->bridging || ! layer->interlock_parts.empty();
            if (level.layer_idx[array_id] >= 0) {
                SupportGeneratorLayer *kept = (*arrays[array_id])[level.layer_idx[array_id]];
                // The organic generator may list one layer object twice; merging a layer into itself would wipe it.
                if (kept == layer) {
                    (*arrays[array_id])[i] = nullptr;
                    continue;
                }
                // Downstream the arrays are looked up by print_z, so two areas of the same kind at the same
                // height cannot be told apart. Fold them together right away.
                kept->merge(std::move(*layer));
                (*arrays[array_id])[i] = nullptr;
                continue;
            }
            level.layer_idx[array_id] = i;
        }

    std::sort(levels.begin(), levels.end(),
        [](const SupportMergeLevel &lhs, const SupportMergeLevel &rhs) { return lhs.print_z < rhs.print_z; });

    const coord_t gap_xy_scaled = scaled<coord_t>(support_params.gap_xy);

    auto layer_of = [&arrays](const SupportMergeLevel &level, int array_id) -> SupportGeneratorLayer * {
        const int idx = level.layer_idx[array_id];
        return idx < 0 ? nullptr : (*arrays[array_id])[idx];
    };

    // The object slices grown by the XY gap, per object layer. Every slab asks for the same layers again.
    const ConstLayerPtrsAdaptor object_layers = object.layers();
    std::vector<std::optional<Polygons>> object_keep_out_cache(object_layers.size());
    auto object_keep_out = [&object_layers, &object_keep_out_cache, gap_xy_scaled](size_t object_layer_idx) -> const Polygons & {
        std::optional<Polygons> &keep_out = object_keep_out_cache[object_layer_idx];
        if (! keep_out)
            keep_out = offset(object_layers[object_layer_idx]->lslices, float(gap_xy_scaled));
        return *keep_out;
    };
    std::vector<std::optional<Polygons>> band_keep_out_cache(object_layers.size());
    auto band_keep_out = [&object_layers, &band_keep_out_cache, gap_xy_scaled](size_t object_layer_idx) -> const Polygons & {
        std::optional<Polygons> &keep_out = band_keep_out_cache[object_layer_idx];
        if (! keep_out) {
            const Polygons band = merged_band_footprint(*object_layers[object_layer_idx], object_layer_idx);
            keep_out = band.empty() ? Polygons() : offset(union_(band), float(gap_xy_scaled));
        }
        return *keep_out;
    };
    auto first_object_layer_above = [&object_layers](coordf_t z) {
        return size_t(std::lower_bound(object_layers.begin(), object_layers.end(), z + EPSILON,
            [](const Layer *layer, coordf_t print_z) { return layer->print_z < print_z; }) - object_layers.begin());
    };

    // A support area is only kept clear of the object at its own level. A slab is a single extrusion over its
    // whole Z span, so it covers the XY the object occupies further down as well and has to be cut back there.
    auto object_clearance = [&object_layers, &object_keep_out, &first_object_layer_above](coordf_t bottom_z, coordf_t top_z) {
        Polygons trimming;
        for (size_t idx = first_object_layer_above(bottom_z); idx < object_layers.size(); ++ idx) {
            const Layer *object_layer = object_layers[idx];
            if (object_layer->print_z - object_layer->height > top_z - EPSILON)
                break;
            polygons_append(trimming, object_keep_out(idx));
        }
        return trimming.empty() ? Polygons() : union_(trimming);
    };
    // The XY of the object starting within the top gap above top_z. An area lifted to top_z from below was placed
    // against an object surface higher up; where that surface starts in here, lifting it eats into the gap.
    auto top_gap_zone = [&object_layers, &object_keep_out, &band_keep_out, &first_object_layer_above, top_gap](coordf_t top_z) {
        Polygons zone;
        if (top_gap <= EPSILON)
            return zone;
        for (size_t idx = first_object_layer_above(top_z); idx < object_layers.size(); ++ idx) {
            const Layer   *object_layer   = object_layers[idx];
            const coordf_t layer_bottom_z = object_layer->print_z - object_layer->height;
            if (layer_bottom_z > top_z + top_gap - EPSILON)
                break;
            if (layer_bottom_z < top_z - EPSILON)
                continue;
            polygons_append(zone, object_keep_out(idx));
            polygons_append(zone, band_keep_out(idx));
        }
        return zone.empty() ? Polygons() : union_(zone);
    };

    // Resolve the top contact stack from the model downwards. A thin level stays untouched when that does not
    // strand an unprintable contact directly below it. Otherwise adjacent contacts and, if necessary, sparse base
    // are folded into a continuous slab printable by the largest available interface nozzle. Base-interface is
    // printed with the support-body extruder, so it stays out of contact slabs.
    static constexpr int top_interface_arrays[] { smaTopContact, smaInterface };
    auto first_top_interface = [&layer_of](const SupportMergeLevel &level) -> SupportGeneratorLayer * {
        for (int array_id : top_interface_arrays)
            if (SupportGeneratorLayer *layer = layer_of(level, array_id); layer != nullptr && ! layer->polygons.empty())
                return layer;
        return nullptr;
    };
    auto interface_height_range = [&layer_of](const SupportMergeLevel &level) {
        std::pair<coordf_t, coordf_t> range { std::numeric_limits<coordf_t>::max(), 0. };
        for (int array_id : top_interface_arrays)
            if (const SupportGeneratorLayer *layer = layer_of(level, array_id); layer != nullptr && ! layer->polygons.empty()) {
                range.first  = std::min(range.first, layer->height);
                range.second = std::max(range.second, layer->height);
            }
        return range;
    };
    auto candidate_fits_bottom_contact = [&layer_of](const SupportMergeLevel &level, const SupportMergePlan::Candidate &candidate) {
        const SupportGeneratorLayer *bottom_contact = layer_of(level, smaBottomContact);
        return bottom_contact == nullptr || bottom_contact->polygons.empty() || candidate.window.contains(bottom_contact->height);
    };
    auto level_bottom_z = [&layer_of](const SupportMergeLevel &level) {
        coordf_t bottom_z = level.print_z;
        bool     found    = false;
        for (int array_id : top_interface_arrays)
            if (const SupportGeneratorLayer *layer = layer_of(level, array_id); layer != nullptr && ! layer->polygons.empty()) {
                bottom_z = std::min(bottom_z, layer->print_z - layer->height);
                found    = true;
            }
        if (const SupportGeneratorLayer *base = layer_of(level, smaIntermediate); base != nullptr && ! base->polygons.empty()) {
            bottom_z = std::min(bottom_z, base->print_z - base->height);
            found    = true;
        }
        return found ? bottom_z : level.print_z;
    };
    auto interface_candidate = [&plan, &candidate_fits_bottom_contact](const SupportMergeLevel &level, coordf_t top_z, coordf_t height) {
        SupportMergePlan::Candidate candidate = plan.finest_interface_for_slab(top_z, height);
        if (candidate.valid() && ! candidate_fits_bottom_contact(level, candidate))
            candidate = {};
        return candidate;
    };
    std::vector<Polygons> interface_borrowed_areas(levels.size());

    // Returns false when the slab was left alone, in which case the areas keep their own levels.
    auto merge_interface_slab = [&](int owner_idx, int bottom_idx, const SupportMergePlan::Candidate &candidate) {
        SupportGeneratorLayer *interface_top = first_top_interface(levels[owner_idx]);
        if (interface_top == nullptr)
            return false;

        const coordf_t top_z       = levels[owner_idx].print_z;
        const coordf_t bottom_z    = level_bottom_z(levels[bottom_idx]);
        const coordf_t slab_height = top_z - bottom_z;
        // What a lower level holds under an object surface starting within the gap above top_z stays at its own
        // level: lifted, it would print into that gap.
        const Polygons gap_zone  = top_gap_zone(top_z);
        // Refuse a slab the object leaves no room for at all rather than merge the areas away.
        const Polygons clearance = object_clearance(bottom_z, top_z);
        if (! clearance.empty()) {
            Polygons slab;
            for (int i = owner_idx; i >= bottom_idx; -- i)
                for (int array_id : top_interface_arrays)
                    if (const SupportGeneratorLayer *source = layer_of(levels[i], array_id); source != nullptr)
                        polygons_append(slab, i < owner_idx && ! gap_zone.empty() ? diff(source->polygons, gap_zone) : source->polygons);
            if (diff(union_(slab), clearance).empty())
                return false;
        }

        std::vector<std::pair<int, int>> kept_sources;
        for (int i = owner_idx; i >= bottom_idx; -- i)
            for (int array_id : top_interface_arrays) {
                SupportGeneratorLayer *source = layer_of(levels[i], array_id);
                if (source == nullptr || source == interface_top)
                    continue;
                if (i < owner_idx && ! gap_zone.empty()) {
                    if (Polygons kept = intersection(source->polygons, gap_zone); ! kept.empty()) {
                        interface_top->polygons = union_(interface_top->polygons, diff(source->polygons, gap_zone));
                        source->polygons        = std::move(kept);
                        kept_sources.emplace_back(i, array_id);
                        continue;
                    }
                }
                interface_top->merge(std::move(*source));
                (*arrays[array_id])[levels[i].layer_idx[array_id]] = nullptr;
                levels[i].layer_idx[array_id] = -1;
            }

        interface_top->print_z  = top_z;
        interface_top->height   = slab_height;
        interface_top->bottom_z = bottom_z;
        if (! clearance.empty())
            interface_top->polygons = diff(interface_top->polygons, clearance);

        const Polygons borrowed_area = union_(interface_top->polygons);
        // The slab now spans the levels its kept areas stay on, so they must not overlap it.
        for (const auto &[i, array_id] : kept_sources) {
            SupportGeneratorLayer *source = layer_of(levels[i], array_id);
            source->polygons = diff(source->polygons, borrowed_area);
            if (source->polygons.empty()) {
                (*arrays[array_id])[levels[i].layer_idx[array_id]] = nullptr;
                levels[i].layer_idx[array_id] = -1;
            }
        }
        for (int i = bottom_idx; i <= owner_idx; ++ i) {
            polygons_append(interface_borrowed_areas[size_t(i)], borrowed_area);
            if (SupportGeneratorLayer *base = layer_of(levels[i], smaIntermediate); base != nullptr && ! base->polygons.empty())
                base->polygons = diff(base->polygons, borrowed_area);
            auto filament_it = planned_interface_filaments.lower_bound(levels[i].print_z - EPSILON);
            if (filament_it != planned_interface_filaments.end() && filament_it->first <= levels[i].print_z + EPSILON)
                planned_interface_filaments.erase(filament_it);
            auto height_it = planned_interface_heights.lower_bound(levels[i].print_z - EPSILON);
            if (height_it != planned_interface_heights.end() && height_it->first <= levels[i].print_z + EPSILON)
                planned_interface_heights.erase(height_it);
        }

        planned_interface_filaments[top_z] = candidate;
        planned_interface_heights[top_z]   = slab_height;
        return true;
    };

    for (int top = int(levels.size()) - 1; top >= 0; -- top) {
        SupportGeneratorLayer *interface_top = first_top_interface(levels[top]);
        if (interface_top == nullptr || levels[top].frozen)
            continue;

        const coordf_t                      top_z        = levels[top].print_z;
        const std::pair<coordf_t, coordf_t> height_range = interface_height_range(levels[top]);
        SupportMergePlan::Candidate candidate = interface_candidate(levels[top], top_z, height_range.first);
        if (candidate.valid() && ! candidate.window.contains(height_range.second))
            candidate = {};
        if (candidate.valid()) {
            planned_interface_filaments[top_z] = candidate;
            continue;
        }

        int      bottom      = top;
        coordf_t bottom_z    = level_bottom_z(levels[top]);
        coordf_t slab_height = top_z - bottom_z;
        candidate = interface_candidate(levels[top], top_z, slab_height);
        while (! candidate.valid() && bottom > 0) {
            const SupportMergeLevel &below = levels[bottom - 1];
            if (below.frozen || below.print_z < bottom_z - EPSILON)
                break;
            const bool has_interface = first_top_interface(below) != nullptr;
            const SupportGeneratorLayer *base = layer_of(below, smaIntermediate);
            if (! has_interface && (base == nullptr || base->polygons.empty()))
                break;
            // Stop once the slab outgrows the tallest band any interface nozzle ends here with.
            const coordf_t grown_height = top_z - level_bottom_z(levels[bottom - 1]);
            if (grown_height > plan.max_interface_height(top_z, grown_height) + EPSILON)
                break;
            -- bottom;
            bottom_z    = level_bottom_z(levels[bottom]);
            slab_height = top_z - bottom_z;
            candidate   = interface_candidate(levels[top], top_z, slab_height);
        }
        if (candidate.valid() && merge_interface_slab(top, bottom, candidate)) {
            // Resume at the highest level still holding an area kept out of the top gap.
            int resume = bottom;
            for (int i = top - 1; i >= bottom; -- i)
                if (first_top_interface(levels[i]) != nullptr) {
                    resume = i + 1;
                    break;
                }
            top = resume;
            continue;
        }

        // A printable upper contact may still absorb this thin level, e.g. a printable 0.2 mm layer above an
        // unprintable 0.1 mm layer become one continuous 0.3 mm slab.
        int upper = top + 1;
        while (upper < int(levels.size()) && first_top_interface(levels[upper]) == nullptr)
            ++ upper;
        if (upper < int(levels.size()) && ! levels[upper].frozen) {
            const SupportGeneratorLayer *upper_interface = first_top_interface(levels[upper]);
            const coordf_t               upper_bottom_z  = upper_interface->print_z - upper_interface->height;
            if (upper_bottom_z <= top_z + EPSILON) {
                const coordf_t combined_height = levels[upper].print_z - level_bottom_z(levels[top]);
                candidate = interface_candidate(levels[upper], levels[upper].print_z, combined_height);
                if (candidate.valid())
                    merge_interface_slab(upper, top, candidate);
            }
        }
    }

    // Only the sparse base is merged here. An interface area is extruded straight against the object, so it keeps
    // its own print_z and height and is cut out of the slab growing past it.
    for (int top = int(levels.size()) - 1; top >= 0; -- top) {
        SupportGeneratorLayer *base_top = layer_of(levels[top], smaIntermediate);
        if (base_top == nullptr || base_top->polygons.empty() || levels[top].frozen)
            continue;

        const coordf_t              slab_top_z   = levels[top].print_z;
        int                         bottom       = top;
        coordf_t                    slab_height  = base_top->height;
        SupportMergePlan::Candidate pinned       = plan.coarsest_for_slab(slab_top_z, slab_height);
        int                         probe        = top;
        coordf_t                    probe_height = slab_height;

        // Probe down to the next fixed-grid boundary. Intermediate heights may be unprintable by every candidate
        // but still lead to a printable band.
        while (probe > 0 && ! levels[probe - 1].frozen) {
            const SupportGeneratorLayer *base_below = layer_of(levels[probe - 1], smaIntermediate);
            if (base_below == nullptr || base_below->polygons.empty())
                break;
            // levels only holds the heights that carry support areas, so the level below may sit under a Z gap.
            // The slab is a single extrusion and would hang over that void, so it stops here instead.
            if (base_below->print_z < slab_top_z - probe_height - EPSILON)
                break;
            const coordf_t height = slab_top_z - (base_below->print_z - base_below->height);
            if (height > plan.max_height_for_slab(slab_top_z, height) + EPSILON)
                break;
            const SupportMergePlan::Candidate candidate = plan.coarsest_for_slab(slab_top_z, height);
            -- probe;
            probe_height = height;
            if (candidate.valid()) {
                bottom      = probe;
                slab_height = height;
                pinned      = candidate;
            }
        }

        if (bottom < top) {
            // A merged extrusion cannot follow every fine-layer contour through Z. Keep the band's top face, the
            // one the model ladder uses (Edge never anchors a band on the bed). Unioning all contours would create
            // a swept wedge where a column moves laterally between layers.
            Polygons representative_polygons = base_top->polygons;

            // Interface extrusions may be thinner than this sparse-base slab while sharing its print_z. Keep the
            // original fine base layers under that footprint and merge only the remainder.
            Polygons interface_columns;
            for (int i = bottom; i <= top; ++ i)
                for (int array_id = 0; array_id < smaCount; ++ array_id)
                    if (support_merge_array_is_interface[array_id])
                        if (const SupportGeneratorLayer *layer = layer_of(levels[i], array_id); layer != nullptr && ! layer->polygons.empty())
                            polygons_append(interface_columns, layer->polygons);
            interface_columns = union_(interface_columns);
            if (! interface_columns.empty())
                representative_polygons = diff(representative_polygons, interface_columns);

            for (int i = top - 1; i >= bottom; -- i) {
                const int idx = levels[i].layer_idx[smaIntermediate];
                SupportGeneratorLayer *base = (*arrays[smaIntermediate])[idx];
                Polygons landing_columns;
                for (int j = bottom; j <= top; ++ j)
                    for (int array_id = 0; array_id < smaCount; ++ array_id)
                        if (support_merge_array_is_interface[array_id])
                            if (const SupportGeneratorLayer *layer = layer_of(levels[j], array_id);
                                layer != nullptr && ! layer->polygons.empty() && layer->print_z - layer->height >= base->print_z - EPSILON)
                                polygons_append(landing_columns, layer->polygons);
                Polygons preserved = landing_columns.empty() ? Polygons() : intersection(base->polygons, union_(landing_columns));
                if (preserved.empty()) {
                    (*arrays[smaIntermediate])[idx]      = nullptr;
                    levels[i].layer_idx[smaIntermediate] = -1;
                } else {
                    base->polygons = std::move(preserved);
                }
            }
            base_top->polygons = std::move(representative_polygons);
            assert(! base_top->bridging);
            base_top->print_z  = slab_top_z;
            base_top->height   = slab_height;
            base_top->bottom_z = slab_top_z - slab_height;

            // The merged part of the base fills the whole slab, so the object has to be kept clear over the
            // complete slab height.
            Polygons trimming = object_clearance(base_top->bottom_z, slab_top_z);
            if (! trimming.empty() && ! base_top->polygons.empty())
                base_top->polygons = diff(base_top->polygons, union_(trimming));
            if (! base_top->polygons.empty())
                planned_base_heights[slab_top_z] = slab_height;
        }

        // Pin the slab to its filament only once it got thicker than the object layers: a slab of base height can
        // be extruded by any nozzle, and leaving it alone lets the exporter minimize tool changes.
        if (pinned.valid() && slab_height > plan.base_height_at(slab_top_z) + EPSILON)
            planned_filaments[slab_top_z] = pinned;

        // The for loop's decrement then moves below the slab.
        top = bottom;
    }

    // A merged base slab is one extrusion, so cutting an interface column out of it drops the support under that
    // column over the slab's whole Z span. Grow the interface down to where support actually resumes.
    static constexpr int rescued_arrays[] { smaTopContact, smaInterface, smaBaseInterface };
    for (int i = int(levels.size()) - 1; i >= 0; -- i) {
        if (levels[i].frozen)
            continue;
        for (int rescued_id : rescued_arrays) {
            SupportGeneratorLayer *interface_top = layer_of(levels[i], rescued_id);
            if (interface_top == nullptr || interface_top->polygons.empty())
                continue;

            // A support transition area is extruded with the support body filament, so its reach and nozzle come
            // from the base plan.
            const bool     with_base_filament = rescued_id == smaBaseInterface;
            const coordf_t bottom_z           = interface_top->print_z - interface_top->height;
            const coordf_t max_reach          = with_base_filament ?
                plan.max_height_for_slab(interface_top->print_z, interface_top->height) :
                plan.max_interface_height(interface_top->print_z, interface_top->height);
            if (max_reach <= interface_top->height + EPSILON)
                continue;

            // Land on the highest support surface the column reaches.
            coordf_t landing_z = 0.;
            bool     landed    = false;
            bool     blocked   = false;
            for (int j = i - 1; j >= 0 && ! blocked; -- j) {
                if (interface_top->print_z - levels[j].print_z > max_reach + EPSILON)
                    break;
                for (int array_id = 0; array_id < smaCount; ++ array_id) {
                    const SupportGeneratorLayer *below = layer_of(levels[j], array_id);
                    if (below == nullptr || below->polygons.empty())
                        continue;
                    if (intersection(below->polygons, interface_top->polygons).empty())
                        continue;
                    if (below->print_z > bottom_z + EPSILON) {
                        // Support already reaches into the range the slab would grow through.
                        blocked = true;
                        break;
                    }
                    landing_z = std::max(landing_z, below->print_z);
                    landed    = true;
                }
            }
            if (blocked || ! landed || landing_z > bottom_z - EPSILON)
                continue;

            // The base slab merge keeps its distance to the object, and so must an interface growing down.
            bool hits_object = false;
            for (size_t idx = first_object_layer_above(landing_z); idx < object_layers.size() && ! hits_object; ++ idx) {
                const Layer *object_layer = object_layers[idx];
                if (object_layer->print_z - object_layer->height > bottom_z - EPSILON)
                    break;
                hits_object = ! intersection(object_keep_out(idx), interface_top->polygons).empty();
            }
            if (hits_object)
                continue;

            const coordf_t              height = interface_top->print_z - landing_z;
            SupportMergePlan::Candidate candidate;
            if (with_base_filament) {
                auto pin = planned_filaments.lower_bound(interface_top->print_z - EPSILON);
                if (pin != planned_filaments.end() && pin->first <= interface_top->print_z + EPSILON) {
                    if (pin->second.window.contains(height))
                        candidate = pin->second;
                } else {
                    candidate = plan.coarsest_for_slab(interface_top->print_z, height);
                    if (candidate.valid())
                        if (const SupportGeneratorLayer *base = layer_of(levels[i], smaIntermediate);
                            base != nullptr && ! base->polygons.empty() && ! candidate.window.contains(base->height))
                            candidate = {};
                }
            } else {
                candidate = plan.finest_interface_for_slab(interface_top->print_z, height);
                if (candidate.valid() && ! candidate_fits_bottom_contact(levels[i], candidate))
                    candidate = {};
            }
            if (! candidate.valid()) {
                BOOST_LOG_TRIVIAL(warning) << "Support interface at z=" << interface_top->print_z << " stays "
                                           << (bottom_z - landing_z) << "mm above the support below it: no filament can extrude a "
                                           << height << "mm slab there";
                continue;
            }
            interface_top->height   = height;
            interface_top->bottom_z = landing_z;
            if (with_base_filament) {
                planned_filaments[interface_top->print_z] = candidate;
                continue;
            }
            planned_interface_filaments[interface_top->print_z] = candidate;
            coordf_t &planned_height = planned_interface_heights[interface_top->print_z];
            planned_height = std::max(planned_height, height);
        }
    }

    compact_arrays();
}

void apply_support_merge_result(PrintObject &object, const SupportMergeResult &result)
{
    if (result.empty())
        return;
    auto find_at = [](const auto &map, coordf_t print_z) {
        auto it = map.lower_bound(print_z - EPSILON);
        return it != map.end() && it->first <= print_z + EPSILON ? it : map.end();
    };
    for (SupportLayer *support_layer : object.support_layers()) {
        if (auto it = find_at(result.planned_filaments, support_layer->print_z); it != result.planned_filaments.end()) {
            support_layer->planned_filament_id     = it->second.filament_id;
            support_layer->planned_nozzle_diameter = it->second.nozzle_diameter;
        }
        if (auto it = find_at(result.planned_interface_filaments, support_layer->print_z); it != result.planned_interface_filaments.end()) {
            support_layer->planned_interface_filament_id     = it->second.filament_id;
            support_layer->planned_interface_nozzle_diameter = it->second.nozzle_diameter;
        }
        if (auto it = find_at(result.planned_base_heights, support_layer->print_z); it != result.planned_base_heights.end())
            support_layer->base_slab_height = it->second;
        if (auto it = find_at(result.planned_interface_heights, support_layer->print_z); it != result.planned_interface_heights.end())
            support_layer->interface_slab_height = it->second;
    }
}

} // namespace Slic3r
