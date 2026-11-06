// Per-nozzle process P1 - see NozzleProcess.hpp.

#include "NozzleProcess.hpp"
#include "NozzleOverride.hpp"

#include "I18N.hpp"
#include "LocalesUtils.hpp"
#include "PrintConfig.hpp"
#include "Preset.hpp"
#include "ToolSlots.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <tuple>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#ifndef L
#define L(s) Slic3r::I18N::translate(s)
#endif

namespace Slic3r {

std::vector<std::string> nozzle_process_preset_names(const ConfigBase &config)
{
    std::vector<std::string> out;
    const auto *names = dynamic_cast<const ConfigOptionStrings *>(config.option("nozzle_process_presets"));
    if (names != nullptr)
        for (const std::string &name : names->values)
            if (!name.empty())
                out.push_back(name);
    return out;
}

bool has_nozzle_process_presets(const ConfigBase &config) { return !nozzle_process_preset_names(config).empty(); }

bool supports_nozzle_processes(const ConfigBase &printer_config)
{
    const auto *nozzles = dynamic_cast<const ConfigOptionVectorBase *>(printer_config.option("nozzle_diameter"));
    if (nozzles == nullptr || nozzles->size() < 2)
        return false;
    const auto *semm = dynamic_cast<const ConfigOptionBool *>(printer_config.option("single_extruder_multi_material"));
    return semm == nullptr || !semm->value;
}

bool supports_filament_nozzle_assignment(const DynamicPrintConfig &printer_config)
{
    if (!supports_nozzle_processes(printer_config))
        return false;
    int extruder_count = 0;
    return !printer_config.support_different_extruders(extruder_count);
}

std::vector<int> filament_nozzle_assignment_stored(std::vector<int> assignment)
{
    for (int &tool : assignment)
        if (tool < 0)
            tool = 0;
    if (std::all_of(assignment.begin(), assignment.end(), [](int tool) { return tool == 0; }))
        assignment.clear();
    return assignment;
}

namespace {

// The layout half of a config, copied before the config is rewritten.
struct SlotLayout
{
    std::vector<int>         ids;
    std::vector<std::string> variants;
    std::vector<std::string> flow_support;

    static SlotLayout of(const ConfigBase &config)
    {
        SlotLayout out;
        if (const auto *opt = dynamic_cast<const ConfigOptionInts *>(config.option("print_extruder_id")))
            out.ids = opt->values;
        if (const auto *opt = dynamic_cast<const ConfigOptionStrings *>(config.option("print_extruder_variant")))
            out.variants = opt->values;
        if (const auto *opt = dynamic_cast<const ConfigOptionStrings *>(config.option(flow_support_key(ConfigFlowDomain::Process))))
            out.flow_support = opt->values;
        return out;
    }

    // Number of extruders of a valid per-extruder layout (ids 1, 1, .., 2, .., n with n >= 2), else 0.
    size_t extruder_count() const
    {
        if (ids.empty() || ids.size() != variants.size())
            return 0;
        size_t n = 0;
        for (int id : ids) {
            if (id == int(n) + 1)
                ++n;
            else if (n == 0 || id != int(n))
                return 0;
        }
        return n >= 2 ? n : 0;
    }

    // The index of a vector of `size` values holding extruder `extruder`'s value for flow mode `mode`.
    size_t index(size_t size, size_t extruder, FilamentVolumeType mode) const
    {
        if (size == 0)
            return 0;
        if (size == ids.size() && extruder < extruder_count()) {
            int first = -1;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (size_t(ids[i]) != extruder + 1)
                    continue;
                if (first < 0)
                    first = int(i);
                if (process_variant_flow_mode(variants[i]) == int(mode))
                    return i;
            }
            if (first >= 0)
                return size_t(first);
        }
        return column(size, mode);
    }

    // The flow-mode column of `mode` (process_flow_support), Standard when not declared: what
    // get_config_idx(Process) reads, so what every extruder printed before per-extruder values.
    size_t column(size_t size, FilamentVolumeType mode) const
    {
        const size_t idx = flow_variant_index(flow_support, to_string(mode));
        return idx < size ? idx : 0;
    }
};

std::string flow_mode_variant_name(const std::string &mode)
{
    if (mode == FLOW_MODE_STANDARD)
        return "Direct Drive Standard";
    if (mode == FLOW_MODE_HIGH_FLOW)
        return "Direct Drive High Flow";
    std::string label = mode;
    std::replace(label.begin(), label.end(), '_', ' ');
    return "Direct Drive " + label;
}

} // namespace

const std::vector<std::pair<std::string, std::string>> &nozzle_line_width_keys()
{
    // (per-filament array, the role width it stands for). Technique from FOrca 0b1f80cd22 (AGPL-3.0,
    // fos_stamp_per_nozzle_region / _object): resolve per nozzle, then write into the scalar the engine reads.
    static const std::vector<std::pair<std::string, std::string>> keys {
        { "nozzle_outer_wall_line_width", "outer_wall_line_width" },
        { "nozzle_inner_wall_line_width", "inner_wall_line_width" },
        { "nozzle_sparse_infill_line_width", "sparse_infill_line_width" },
        { "nozzle_internal_solid_infill_line_width", "internal_solid_infill_line_width" },
        { "nozzle_top_surface_line_width", "top_surface_line_width" },
        // P5: the locked-zag infill's skin and skeleton widths.
        { "nozzle_skin_infill_line_width", "skin_infill_line_width" },
        { "nozzle_skeleton_infill_line_width", "skeleton_infill_line_width" },
        // P6: the bridge width (0 = auto keeps its meaning: no line_width fallback).
        { "nozzle_bridge_line_width", "bridge_line_width" },
        { "nozzle_support_line_width", "support_line_width" },
    };
    return keys;
}

namespace {
// A 0 of these widths means "line_width"; a 0 bridge width means auto and a 0 first-layer width "the role widths".
bool zero_width_is_line_width(const std::string &width_key)
{
    return width_key != "bridge_line_width" && width_key != "initial_layer_line_width" && width_key != "line_width";
}
} // namespace

size_t nozzle_for_filament(const ConfigBase &config, unsigned int filament_id)
{
    const auto *nozzles   = dynamic_cast<const ConfigOptionVectorBase *>(config.option("nozzle_diameter"));
    const auto *filaments = dynamic_cast<const ConfigOptionVectorBase *>(config.option("filament_diameter"));
    const auto *fmap      = dynamic_cast<const ConfigOptionInts *>(config.option("filament_map"));
    const auto *assign    = dynamic_cast<const ConfigOptionInts *>(config.option("filament_nozzle_assignment"));
    static const std::vector<int> none;
    const size_t nozzle_count = nozzles != nullptr ? nozzles->size() : 0;
    return physical_extruder_for_filament(nozzle_count, filaments != nullptr ? filaments->size() : nozzle_count,
                                          fmap != nullptr ? fmap->values : none, filament_id, assign != nullptr ? assign->values : none);
}

bool fill_nozzle_line_widths(DynamicPrintConfig &config, const std::vector<const DynamicPrintConfig *> &sources,
                             const std::vector<const DynamicPrintConfig *> *filament_sources)
{
    const auto  *filaments = dynamic_cast<const ConfigOptionVectorBase *>(config.option("filament_diameter"));
    const size_t count     = filaments != nullptr ? filaments->size() : 0;
    if (filament_sources != nullptr && filament_sources->size() != count)
        filament_sources = nullptr;
    // Per filament: the process of the nozzle it prints on (P9: of its rack slot), nullptr for the plate's.
    std::vector<const DynamicPrintConfig *> per_filament(count, nullptr);
    std::vector<int>                        tool_of(count, 0);
    std::vector<double>                     diameter_of(count, 0.);
    bool any = false;
    for (size_t f = 0; f < count; ++f) {
        const size_t e = nozzle_for_filament(config, unsigned(f + 1));
        tool_of[f]     = int(e) + 1;
        diameter_of[f] = nozzle_diameter_for_filament(config, unsigned(f + 1));
        const DynamicPrintConfig *src = filament_sources != nullptr ? (*filament_sources)[f] : e < sources.size() ? sources[e] : nullptr;
        if (src != nullptr) {
            per_filament[f] = src;
            any = true;
        }
    }
    // A source process's value of `width_key` for filament f: its own table entry for the nozzle (P6), else its value.
    auto source_width = [&](const DynamicPrintConfig &src, const std::string &width_key, size_t f, FloatOrPercent &out) {
        if (process_nozzle_width_override(src, width_key, tool_of[f], diameter_of[f], out))
            return true;
        if (const auto *width = src.option<ConfigOptionFloatOrPercent>(width_key)) {
            out = FloatOrPercent{ width->value, width->percent };
            return true;
        }
        return false;
    };
    auto fill_array = [&](const std::string &array_key, const std::string &width_key) {
        auto *opt = new ConfigOptionFloatsOrPercents();
        if (any) {
            // A filament on a nozzle without a process of its own keeps the plate's width (0 = line_width, as
            // the plate has it), so a region whose filament a modifier changes is stamped back correctly.
            const auto *plate = config.option<ConfigOptionFloatOrPercent>(width_key);
            const FloatOrPercent plate_width = plate != nullptr ? FloatOrPercent{ plate->value, plate->percent } : FloatOrPercent{ 0., false };
            opt->values.assign(count, plate_width);
            for (size_t f = 0; f < count; ++f) {
                const DynamicPrintConfig *src = per_filament[f];
                if (src == nullptr) {
                    // P6: rules 2-4 (the plate's table, scaling) for a nozzle without a process of its own.
                    opt->values[f] = nozzle_width_for(config, width_key, tool_of[f], diameter_of[f]).value;
                    continue;
                }
                FloatOrPercent width{ 0., false };
                const bool     has = source_width(*src, width_key, f, width);
                if (!zero_width_is_line_width(width_key)) {
                    // P5: the first-layer width keeps 0 ("the role widths"); P6: so does the bridge width (auto) and
                    // line_width itself.
                    if (has)
                        opt->values[f] = width;
                    continue;
                }
                if (!has || width.value == 0.)
                    if (!source_width(*src, "line_width", f, width)) // a 0 role width means line_width
                        continue;
                if (width.value > 0.)
                    opt->values[f] = width;
            }
        }
        config.set_key_value(array_key, opt);
    };
    for (const auto &[array_key, width_key] : nozzle_line_width_keys())
        fill_array(array_key, width_key);
    fill_array("nozzle_initial_layer_line_width", "initial_layer_line_width");
    fill_array("nozzle_line_width", "line_width"); // P6: the skirt's fallback width
    return any;
}

namespace {
// The width `array` gives 1-based `filament`, or nullptr when it gives none (no per-nozzle process in the job,
// support on "the current filament", a mixed-filament id past the physical filaments).
const FloatOrPercent *nozzle_width_at(const ConfigOptionFloatsOrPercents &array, int filament)
{
    if (filament < 1 || size_t(filament) > array.values.size())
        return nullptr;
    return &array.values[size_t(filament - 1)];
}
void stamp_width(ConfigOptionFloatOrPercent &dst, const ConfigOptionFloatsOrPercents &array, int filament)
{
    if (const FloatOrPercent *w = nozzle_width_at(array, filament)) {
        dst.value   = w->value;
        dst.percent = w->percent;
    }
}
} // namespace

void layer_height_limits_for_nozzles(const ConfigBase &printer_config, const std::vector<size_t> &used_nozzles, double &min_layer_height,
                                     double &max_layer_height)
{
    min_layer_height = max_layer_height = 0.;
    const auto *mins    = dynamic_cast<const ConfigOptionFloats *>(printer_config.option("min_layer_height"));
    const auto *maxs    = dynamic_cast<const ConfigOptionFloats *>(printer_config.option("max_layer_height"));
    const auto *nozzles = dynamic_cast<const ConfigOptionFloats *>(printer_config.option("nozzle_diameter"));
    if (mins == nullptr || maxs == nullptr || mins->values.empty() || maxs->values.empty())
        return;
    min_layer_height = *std::min_element(mins->values.begin(), mins->values.end());
    max_layer_height = *std::max_element(maxs->values.begin(), maxs->values.end());
    if (nozzles == nullptr || !has_mixed_nozzle_diameters(nozzles->values))
        return;
    int smallest = -1;
    for (size_t e : used_nozzles)
        if (e < nozzles->values.size() && (smallest < 0 || nozzles->values[e] < nozzles->values[size_t(smallest)]))
            smallest = int(e);
    if (smallest < 0)
        return;
    min_layer_height = mins->get_at(size_t(smallest));
    max_layer_height = maxs->get_at(size_t(smallest));
}

void stamp_nozzle_line_widths(PrintRegionConfig &c)
{
    // The filament of each role, as PrintRegion::extruder() resolves it.
    const int wall   = c.wall_filament.value;
    const int outer  = c.outer_wall_filament.value > 0 ? c.outer_wall_filament.value : wall;
    const int sparse = c.sparse_infill_filament.value;
    const int solid  = std::abs(c.sparse_infill_density.value - 100.) < EPSILON ? sparse : c.solid_infill_filament.value;
    stamp_width(c.outer_wall_line_width, c.nozzle_outer_wall_line_width, outer);
    stamp_width(c.inner_wall_line_width, c.nozzle_inner_wall_line_width, wall);
    stamp_width(c.sparse_infill_line_width, c.nozzle_sparse_infill_line_width, sparse);
    stamp_width(c.internal_solid_infill_line_width, c.nozzle_internal_solid_infill_line_width, solid);
    stamp_width(c.top_surface_line_width, c.nozzle_top_surface_line_width, c.solid_infill_filament.value);
    // P5: the locked-zag skin and skeleton are sparse infill.
    stamp_width(c.skin_infill_line_width, c.nozzle_skin_infill_line_width, sparse);
    stamp_width(c.skeleton_infill_line_width, c.nozzle_skeleton_infill_line_width, sparse);
    // P6: bridges are solid infill (LayerRegion::bridging_flow takes the solid infill filament's nozzle).
    stamp_width(c.bridge_line_width, c.nozzle_bridge_line_width, solid);
}

void stamp_nozzle_line_widths(PrintObjectConfig &c)
{
    // Support "0 = current filament" has no nozzle of its own: it keeps the object's width.
    stamp_width(c.support_line_width, c.nozzle_support_line_width, c.support_filament.value);
}

bool fill_nozzle_process_slots(DynamicPrintConfig &config, const std::vector<const DynamicPrintConfig *> &sources,
                               const std::vector<const DynamicPrintConfig *> *filament_sources)
{
    // P9: the per-filament internal key is rewritten on every fill (a stale one would point into an old layout).
    config.set_key_value("nozzle_filament_process_slot", new ConfigOptionInts());
    // Chip-scope fix: so are the tool record and, when nothing fills them below, the per-filament widths and region values: a
    // project file carries the arrays of the fill it was saved with, which no longer apply when no tool has a process of its
    // own now (the CLI reads them straight from the 3MF). Empty arrays are what a job without per-nozzle processes has.
    config.set_key_value("nozzle_filament_tool", new ConfigOptionStrings());
    auto clear_filament_arrays = [&config]() {
        for (const auto &entry : nozzle_line_width_keys())
            config.set_key_value(entry.first, new ConfigOptionFloatsOrPercents());
        config.set_key_value("nozzle_initial_layer_line_width", new ConfigOptionFloatsOrPercents());
        config.set_key_value("nozzle_line_width", new ConfigOptionFloatsOrPercents());
        config.set_key_value("nozzle_region_values", new ConfigOptionStrings());
    };
    if (!supports_nozzle_processes(config)) {
        clear_filament_arrays();
        return false;
    }
    const size_t extruders = static_cast<const ConfigOptionVectorBase *>(config.option("nozzle_diameter"))->size();
    const auto  *filament_opt = dynamic_cast<const ConfigOptionVectorBase *>(config.option("filament_diameter"));
    const size_t filaments    = filament_opt != nullptr ? filament_opt->size() : 0;
    if (filament_sources != nullptr && filament_sources->size() != filaments)
        filament_sources = nullptr;
    bool any_source = false;
    for (size_t e = 0; e < std::min(extruders, sources.size()); ++e)
        any_source |= sources[e] != nullptr;
    if (filament_sources != nullptr)
        for (const DynamicPrintConfig *src : *filament_sources)
            any_source |= src != nullptr;
    if (!any_source) {
        clear_filament_arrays();
        return false;
    }

    const SlotLayout plate = SlotLayout::of(config);
    const bool       keep  = plate.extruder_count() == extruders;
    // A process the user split per nozzle (left / right edits) keeps every extruder's slots. Any other
    // process - every system profile, every preset saved before per-nozzle processes - printed all
    // extruders from the flow-mode column, whatever its other slots hold (the X2D's Bowden slots differ,
    // an older user preset edited only the first column): a nozzle without a process of its own keeps
    // printing exactly that.
    const auto *explicit_opt = dynamic_cast<const ConfigOptionBool *>(config.option("process_extruder_values"));
    const bool  explicit_values = explicit_opt != nullptr && explicit_opt->value;
    std::vector<int>         ids      = plate.ids;
    std::vector<std::string> variants = plate.variants;
    if (!keep) {
        // One slot per extruder and declared flow mode, in the order of process_flow_support (the U1:
        // standard, high_flow), so get_config_idx(Process) still lands on extruder 1's slots.
        std::vector<std::string> modes = plate.flow_support;
        if (modes.empty())
            modes.emplace_back(FLOW_MODE_STANDARD);
        ids.clear();
        variants.clear();
        for (size_t e = 0; e < extruders; ++e)
            for (const std::string &mode : modes) {
                ids.push_back(int(e) + 1);
                variants.push_back(flow_mode_variant_name(mode));
            }
    }
    const auto *head_sizes = config.option<ConfigOptionFloats>("nozzle_diameter");

    // The slots of the layout to fill: the extruders' (in place, `orig` = their index in the plate's layout), and per P9 rack
    // slot group (filaments whose rack slot has another process than their extruder) one slot per flow mode of its extruder,
    // right after that extruder's slots so the ids keep running 1, 1, .., 2, .., n.
    struct SlotDesc
    {
        size_t                    e{ 0 };
        std::string               variant;
        int                       orig{ -1 };
        const DynamicPrintConfig *src{ nullptr };
        double                    diameter{ 0. };
    };
    std::vector<SlotDesc> slots;
    for (size_t s = 0; s < ids.size(); ++s) {
        const size_t e = size_t(ids[s] - 1);
        slots.push_back({ e, variants[s], int(s), e < sources.size() ? sources[e] : nullptr, head_sizes != nullptr ? head_sizes->get_at(e) : 0. });
    }
    // Filament -> its group's first slot; groups by (extruder, source).
    std::vector<int> filament_group(filaments, -1);
    struct Group { size_t e; const DynamicPrintConfig *src; double diameter; std::vector<size_t> members; };
    std::vector<Group> groups;
    if (filament_sources != nullptr)
        for (size_t f = 0; f < filaments; ++f) {
            const size_t e = nozzle_for_filament(config, unsigned(f + 1));
            if (e >= extruders)
                continue;
            const DynamicPrintConfig *own = (*filament_sources)[f];
            if (own == (e < sources.size() ? sources[e] : nullptr))
                continue;
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group &g) { return g.e == e && g.src == own; });
            if (it == groups.end()) {
                groups.push_back({ e, own, nozzle_diameter_for_filament(config, unsigned(f + 1)), {} });
                it = groups.end() - 1;
            }
            it->members.push_back(f);
            filament_group[f] = int(it - groups.begin());
        }
    std::vector<std::vector<size_t>> group_slots(groups.size());
    for (size_t g = 0; g < groups.size(); ++g) {
        std::vector<std::string> modes;
        size_t                   insert_at = 0;
        for (size_t s = 0; s < slots.size(); ++s)
            if (slots[s].e == groups[g].e) {
                if (slots[s].orig >= 0 && std::find(modes.begin(), modes.end(), slots[s].variant) == modes.end())
                    modes.push_back(slots[s].variant);
                insert_at = s + 1;
            }
        if (modes.empty())
            continue;
        for (const std::string &variant : modes)
            slots.insert(slots.begin() + insert_at++, SlotDesc{ groups[g].e, variant, -1, groups[g].src, groups[g].diameter });
    }
    // The slots of each group, after every insertion.
    for (size_t s = 0, g = 0; s < slots.size(); ++s)
        if (slots[s].orig < 0)
            for (g = 0; g < groups.size(); ++g)
                if (groups[g].e == slots[s].e && groups[g].src == slots[s].src) {
                    group_slots[g].push_back(s);
                    break;
                }
    const bool   extra  = slots.size() != ids.size();
    const size_t layout = slots.size();
    const size_t plate_layout_size = ids.size();

    // P6: a plate process split by nozzle size (P5 diameter slots) keeps giving a nozzle without a process of its own
    // its size's values; the new per-extruder layout below has no diameter dimension, so they are copied per extruder.
    // Before, such a nozzle fell back to the flow-mode column (the first size's values).
    std::unique_ptr<DynamicPrintConfig> by_size;
    if (!keep && process_has_diameter_slots(config))
        by_size = std::make_unique<DynamicPrintConfig>(config);

    std::map<const DynamicPrintConfig *, SlotLayout> source_layouts;
    for (const SlotDesc &slot : slots)
        if (slot.src != nullptr && source_layouts.count(slot.src) == 0)
            source_layouts.emplace(slot.src, SlotLayout::of(*slot.src));

    for (const std::string &key : per_nozzle_process_options()) {
        auto *opt = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (opt == nullptr || opt->size() == 0)
            continue;
        const std::unique_ptr<ConfigOption> plate_opt(opt->clone());
        const auto  *plate_vec      = static_cast<const ConfigOptionVectorBase *>(plate_opt.get());
        const bool   plate_in_place = keep && plate_vec->size() == plate_layout_size;
        opt->resize(layout, plate_opt.get());
        for (size_t s = 0; s < layout; ++s) {
            const size_t              e    = slots[s].e;
            const FilamentVolumeType  mode = process_variant_fill_mode(slots[s].variant);
            const DynamicPrintConfig *src  = slots[s].src;
            if (src != nullptr) {
                const auto *src_vec = dynamic_cast<const ConfigOptionVectorBase *>(src->option(key));
                if (src_vec != nullptr && src_vec->size() > 0 && src_vec->type() == opt->type()) {
                    opt->set_at(src_vec, s, source_layouts.at(src).index(src_vec->size(), e, mode));
                    continue;
                }
            }
            if (by_size) {
                const int size_slot = process_diameter_slot(*by_size, key, slots[s].diameter, mode);
                if (size_slot >= 0 && size_t(size_slot) < plate_vec->size()) {
                    opt->set_at(plate_vec, s, size_t(size_slot));
                    continue;
                }
            }
            const int    orig      = slots[s].orig;
            const size_t plate_idx = !plate_in_place                ? plate.index(plate_vec->size(), e, mode) :
                                     orig < 0                       ? (explicit_values ? plate.index(plate_vec->size(), e, mode) :
                                                                                         plate.column(plate_vec->size(), mode)) :
                                     (explicit_values || e == 0) ? size_t(orig) :
                                                                   plate.column(plate_vec->size(), mode);
            opt->set_at(plate_vec, s, plate_idx);
        }
    }

    // P9: the per-filament sources the region keys and widths are read from (each filament's rack slot, else its extruder).
    std::vector<const DynamicPrintConfig *> per_filament;
    if (filament_sources != nullptr)
        per_filament = *filament_sources;
    // Per-nozzle process P2: the line widths of each filament's nozzle process.
    fill_nozzle_line_widths(config, sources, filament_sources != nullptr ? &per_filament : nullptr);
    // Per-nozzle process P8: and every other region setting of it (walls, infill, top / bottom, ironing...).
    fill_nozzle_region_values(config, sources, filament_sources != nullptr ? &per_filament : nullptr);

    if (!keep || extra) {
        std::vector<int>         new_ids;
        std::vector<std::string> new_variants;
        for (const SlotDesc &slot : slots) {
            new_ids.push_back(int(slot.e) + 1);
            new_variants.push_back(slot.variant);
        }
        // Per-nozzle process P4: a new layout has no diameter dimension; the plate's own one keeps its entries.
        std::vector<double> diameters { 0. };
        if (keep)
            if (const auto *d = config.option<ConfigOptionFloats>("print_extruder_nozzle_diameter"); d != nullptr && d->values.size() == plate_layout_size) {
                diameters.clear();
                for (const SlotDesc &slot : slots)
                    diameters.push_back(slot.orig >= 0 ? d->values[size_t(slot.orig)] : 0.);
            }
        config.set_key_value("print_extruder_id", new ConfigOptionInts(new_ids));
        config.set_key_value("print_extruder_variant", new ConfigOptionStrings(new_variants));
        config.set_key_value("print_extruder_nozzle_diameter", new ConfigOptionFloats(diameters));
    }
    // P9: each filament of a group reads its group's slot of its flow mode.
    if (extra) {
        std::vector<int> own(filaments, -1);
        for (size_t f = 0; f < filaments; ++f) {
            const int g = filament_group[f];
            if (g < 0 || group_slots[size_t(g)].empty())
                continue;
            const int mode = filament_flow_mode(config, unsigned(f));
            own[f]         = int(group_slots[size_t(g)].front());
            for (size_t s : group_slots[size_t(g)])
                if (process_variant_flow_mode(slots[s].variant) == mode) {
                    own[f] = int(s);
                    break;
                }
        }
        config.set_key_value("nozzle_filament_process_slot", new ConfigOptionInts(own));
    }
    // The slicer reads the extruder slots only when asked to; a per-nozzle process whose values equal the
    // plate's changes nothing, so it does not ask.
    config.set_key_value("process_extruder_values", new ConfigOptionBool(explicit_values || process_extruder_slots_differ(config)));
    // Chip-scope fix: the tool each filament's entries above were worked out for, so Print::apply can tell when the plate's own
    // filament map puts a filament on another nozzle (realign_nozzle_filament_values).
    {
        std::vector<std::string> tools(filaments);
        for (size_t f = 0; f < filaments; ++f)
            tools[f] = nozzle_filament_tool_id(config, unsigned(f + 1));
        config.set_key_value("nozzle_filament_tool", new ConfigOptionStrings(std::move(tools)));
    }
    return true;
}

bool realign_nozzle_filament_values(DynamicPrintConfig &config)
{
    auto        *recorded  = config.option<ConfigOptionStrings>("nozzle_filament_tool");
    const auto  *filaments = dynamic_cast<const ConfigOptionVectorBase *>(config.option("filament_diameter"));
    const size_t count     = filaments != nullptr ? filaments->size() : 0;
    if (recorded == nullptr || count == 0 || recorded->values.size() != count)
        return false;
    const std::vector<std::string> was = recorded->values;
    std::vector<std::string>       now(count);
    bool                           moved = false;
    for (size_t f = 0; f < count; ++f) {
        now[f] = nozzle_filament_tool_id(config, unsigned(f + 1));
        moved |= now[f] != was[f];
    }
    if (!moved)
        return false;
    // Per moved filament: another filament whose entries were worked out for the tool it prints on now (-1 = none).
    std::vector<int> donor(count, -1);
    for (size_t f = 0; f < count; ++f) {
        if (now[f] == was[f])
            continue;
        for (size_t g = 0; g < count; ++g)
            if (was[g] == now[f]) {
                donor[f] = int(g);
                break;
            }
    }
    bool changed = false;
    // Widths: the donor's, else the plate's width for the nozzle it prints on now (rules 2-4 of nozzle_width_for, as
    // fill_nozzle_line_widths gives a filament on a nozzle without a process of its own).
    std::vector<std::pair<std::string, std::string>> arrays = nozzle_line_width_keys();
    arrays.emplace_back("nozzle_initial_layer_line_width", "initial_layer_line_width");
    arrays.emplace_back("nozzle_line_width", "line_width");
    for (const auto &[array_key, width_key] : arrays) {
        auto *opt = config.option<ConfigOptionFloatsOrPercents>(array_key);
        if (opt == nullptr || opt->values.size() != count)
            continue;
        const std::vector<FloatOrPercent> orig = opt->values;
        for (size_t f = 0; f < count; ++f) {
            if (now[f] == was[f])
                continue;
            const FloatOrPercent value = donor[f] >= 0 ? orig[size_t(donor[f])] :
                                                         nozzle_width_for(config, width_key, int(nozzle_for_filament(config, unsigned(f + 1))) + 1,
                                                                          nozzle_diameter_for_filament(config, unsigned(f + 1)))
                                                             .value;
            if (!(value == opt->values[f])) {
                opt->values[f] = value;
                changed        = true;
            }
        }
    }
    // Region values: the donor's, else the plate's values of the same keys.
    if (auto *regions = config.option<ConfigOptionStrings>("nozzle_region_values"); regions != nullptr && regions->values.size() == count) {
        const std::vector<std::string> orig = regions->values;
        std::vector<std::string>       keys;
        for (const std::string &entry : orig) {
            if (entry.empty())
                continue;
            size_t start = 0;
            while (start < entry.size()) {
                size_t end = entry.find('\n', start);
                if (end == std::string::npos)
                    end = entry.size();
                const std::string line = entry.substr(start, end - start);
                if (const size_t eq = line.find(" = "); eq != std::string::npos)
                    keys.push_back(line.substr(0, eq));
                start = end + 1;
            }
            break;
        }
        std::string plate_text;
        for (const std::string &key : keys)
            if (const ConfigOption *value = config.option(key)) {
                if (!plate_text.empty())
                    plate_text += '\n';
                plate_text += key + " = " + value->serialize();
            }
        for (size_t f = 0; f < count; ++f) {
            if (now[f] == was[f])
                continue;
            std::string text = donor[f] >= 0 ? orig[size_t(donor[f])] : plate_text;
            if (text != regions->values[f]) {
                regions->values[f] = std::move(text);
                changed            = true;
            }
        }
    }
    // A rack size group's own layout slot: the donor's (same extruder, same process), else the extruder's slots (-1).
    if (auto *slots = config.option<ConfigOptionInts>("nozzle_filament_process_slot"); slots != nullptr && slots->values.size() == count) {
        const std::vector<int> orig = slots->values;
        for (size_t f = 0; f < count; ++f) {
            if (now[f] == was[f])
                continue;
            const int slot = donor[f] >= 0 ? orig[size_t(donor[f])] : -1;
            if (slot != slots->values[f]) {
                slots->values[f] = slot;
                changed          = true;
            }
        }
    }
    std::string log;
    for (size_t f = 0; f < count; ++f)
        if (now[f] != was[f])
            log += " filament " + std::to_string(f + 1) + ": " + was[f] + " -> " + now[f] + (donor[f] >= 0 ? "" : " (plate widths)") + ";";
    BOOST_LOG_TRIVIAL(warning) << "per-nozzle process: the plate's filament map moved filaments to other nozzles, their per-nozzle values follow:" << log;
    recorded->values = now;
    return changed;
}

bool process_nozzles_splittable(const ConfigBase &process, size_t extruders)
{
    // P2 (owner bug report 2026-10-30): any process on a printer with two or more nozzles can be split. A process
    // whose layout does not cover exactly these extruders (a U1 process, a user preset that saved its own
    // print_extruder_variant list, a process carried over from another printer) gets a new layout when it is
    // split (split_process_per_nozzle).
    (void) process;
    return extruders >= 2;
}

NozzleSplitControl nozzle_split_control(size_t extruders, bool nozzle_has_own_process, bool split)
{
    if (extruders < 2)
        return NozzleSplitControl::Hidden;
    if (split)
        return NozzleSplitControl::Link; // Linking acts on the plate's process, whatever the shown nozzle uses.
    if (nozzle_has_own_process)
        return NozzleSplitControl::OwnProcess;
    return NozzleSplitControl::Split;
}

bool process_nozzles_split(const ConfigBase &process)
{
    const auto *flag = dynamic_cast<const ConfigOptionBool *>(process.option("process_extruder_values"));
    return flag != nullptr && flag->value;
}

int process_nozzle_slot(const ConfigBase &process, const std::string &key, size_t extruder, FilamentVolumeType mode)
{
    if (!process_nozzles_split(process) || !is_per_nozzle_process_option(key))
        return -1;
    const SlotLayout layout = SlotLayout::of(process);
    const auto      *opt    = dynamic_cast<const ConfigOptionVectorBase *>(process.option(key));
    if (layout.extruder_count() == 0 || extruder >= layout.extruder_count() || opt == nullptr || opt->size() != layout.ids.size())
        return -1;
    int first = -1;
    for (size_t s = 0; s < layout.ids.size(); ++s) {
        if (size_t(layout.ids[s]) != extruder + 1)
            continue;
        if (first < 0)
            first = int(s);
        if (process_variant_flow_mode(layout.variants[s]) == int(mode))
            return int(s);
    }
    return first;
}

bool split_process_per_nozzle(DynamicPrintConfig &process, size_t extruders)
{
    if (!process_nozzles_splittable(process, extruders))
        return false;
    const SlotLayout layout = SlotLayout::of(process);
    const bool       fits   = layout.extruder_count() == extruders;
    std::vector<int>         ids      = layout.ids;
    std::vector<std::string> variants = layout.variants;
    if (!fits) {
        // A new layout, as fill_nozzle_process_slots() makes for the U1: one slot per extruder and declared flow
        // mode, in process_flow_support order, so extruder 1's slots stay the columns get_config_idx() reads.
        std::vector<std::string> modes = layout.flow_support;
        if (modes.empty())
            modes.emplace_back(FLOW_MODE_STANDARD);
        ids.clear();
        variants.clear();
        for (size_t e = 0; e < extruders; ++e)
            for (const std::string &mode : modes) {
                ids.push_back(int(e) + 1);
                variants.push_back(flow_mode_variant_name(mode));
            }
    }
    const size_t size = ids.size();
    for (const std::string &key : per_nozzle_process_options()) {
        auto *opt = dynamic_cast<ConfigOptionVectorBase *>(process.option(key));
        if (opt == nullptr || opt->size() == 0)
            continue;
        const std::unique_ptr<ConfigOption> before(opt->clone());
        const auto  *src     = static_cast<const ConfigOptionVectorBase *>(before.get());
        const bool   in_place = fits && src->size() == size;
        opt->resize(size, before.get());
        for (size_t s = 0; s < size; ++s) {
            const size_t e = size_t(ids[s] - 1);
            // Extruder 1 keeps its slots; every other nozzle starts from the column it prints with now.
            const size_t idx = in_place && (e == 0 || process_variant_flow_mode(variants[s]) < 0) ? s :
                                                                       layout.column(src->size(), process_variant_fill_mode(variants[s]));
            opt->set_at(src, s, idx);
        }
    }
    if (!fits) {
        process.set_key_value("print_extruder_id", new ConfigOptionInts(ids));
        process.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
        process.set_key_value("print_extruder_nozzle_diameter", new ConfigOptionFloats({ 0. }));
    }
    process.set_key_value("process_extruder_values", new ConfigOptionBool(true));
    return true;
}

bool copy_nozzle_slots_to_all(DynamicPrintConfig &process, size_t from)
{
    const SlotLayout layout = SlotLayout::of(process);
    if (layout.extruder_count() == 0 || from >= layout.extruder_count())
        return false;
    bool changed = false;
    for (const std::string &key : per_nozzle_process_options()) {
        auto *opt = dynamic_cast<ConfigOptionVectorBase *>(process.option(key));
        if (opt == nullptr || opt->size() != layout.ids.size())
            continue;
        const std::unique_ptr<ConfigOption> before(opt->clone());
        const std::vector<std::string>      values = opt->vserialize();
        for (size_t dst = 0; dst < layout.ids.size(); ++dst) {
            const int mode = process_variant_flow_mode(layout.variants[dst]);
            if (size_t(layout.ids[dst]) == from + 1 || mode < 0)
                continue;
            const int src = process_nozzle_slot(process, key, from, FilamentVolumeType(mode));
            if (src >= 0 && values[size_t(src)] != values[dst]) {
                opt->set_at(before.get(), dst, size_t(src));
                changed = true;
            }
        }
    }
    return changed;
}

bool link_process_nozzles(DynamicPrintConfig &process, size_t from, const ConfigBase *reference)
{
    if (!process_nozzles_split(process))
        return false;
    copy_nozzle_slots_to_all(process, from);
    if (reference != nullptr) {
        // A split that made a new layout (split_process_per_nozzle) gives the reference's layout back: every key
        // takes the shape it has in the reference, each slot the linked value of that slot's flow mode.
        const SlotLayout layout = SlotLayout::of(process);
        const SlotLayout ref_layout = SlotLayout::of(*reference);
        if (ref_layout.ids != layout.ids || ref_layout.variants != layout.variants) {
            const bool ref_valid = !ref_layout.ids.empty() && ref_layout.ids.size() == ref_layout.variants.size();
            for (const std::string &key : per_nozzle_process_options()) {
                auto       *opt = dynamic_cast<ConfigOptionVectorBase *>(process.option(key));
                const auto *ref = dynamic_cast<const ConfigOptionVectorBase *>(reference->option(key));
                if (opt == nullptr || ref == nullptr || ref->size() == 0 || opt->size() != layout.ids.size())
                    continue;
                const std::unique_ptr<ConfigOption> linked(opt->clone());
                opt->resize(ref->size(), linked.get());
                for (size_t i = 0; i < ref->size(); ++i) {
                    FilamentVolumeType mode = fvtStandard;
                    if (ref_valid && ref->size() == ref_layout.ids.size())
                        mode = process_variant_fill_mode(ref_layout.variants[i]);
                    else if (i < ref_layout.flow_support.size() && ref_layout.flow_support[i] == FLOW_MODE_HIGH_FLOW)
                        mode = fvtHighFlow;
                    opt->set_at(linked.get(), i, layout.index(layout.ids.size(), from, mode));
                }
            }
            for (const char *key : { "print_extruder_id", "print_extruder_variant" }) {
                if (const ConfigOption *opt = reference->option(key))
                    process.set_key_value(key, opt->clone());
                else
                    process.erase(key);
            }
            process.set_key_value("process_extruder_values", new ConfigOptionBool(false));
            return true;
        }
        for (const std::string &key : per_nozzle_process_options()) {
            auto       *opt = dynamic_cast<ConfigOptionVectorBase *>(process.option(key));
            const auto *ref = dynamic_cast<const ConfigOptionVectorBase *>(reference->option(key));
            if (opt == nullptr || ref == nullptr || ref->size() == 0 || ref->size() >= opt->size() || opt->size() != layout.ids.size())
                continue;
            // The reference's shorter vector is a flow-mode vector: every slot must hold its mode's column.
            const std::vector<std::string> values = opt->vserialize();
            bool fits = true;
            for (size_t s = 0; s < values.size() && fits; ++s)
                fits = values[s] == values[layout.column(ref->size(), process_variant_fill_mode(layout.variants[s]))];
            if (fits)
                opt->resize(ref->size());
        }
    }
    process.set_key_value("process_extruder_values", new ConfigOptionBool(false));
    return true;
}

// ---- Per-nozzle process P5: one process with separate values per nozzle size ----------------------------------

std::vector<double> printer_nozzle_sizes(const ConfigBase &printer_config)
{
    std::vector<double> out;
    auto add = [&out](double d) {
        if (d > 0. && std::none_of(out.begin(), out.end(), [d](double s) { return same_nozzle_diameter(s, d); }))
            out.push_back(d);
    };
    // Per-nozzle process P7: the sizes the extruders carry in the project.
    for (double d : effective_nozzle_diameters(printer_config))
        add(d);
    for (const std::vector<RackNozzle> &rack : extruder_rack_nozzles(printer_config))
        for (const RackNozzle &nozzle : rack)
            add(nozzle.diameter);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<double> process_slot_diameters(const ConfigBase &process)
{
    std::vector<double> out;
    if (!process_has_diameter_slots(process))
        return out;
    for (const ToolSlot &slot : process_tool_slots(process))
        if (slot.diameter > 0. && std::none_of(out.begin(), out.end(), [&slot](double d) { return same_nozzle_diameter(d, slot.diameter); }))
            out.push_back(slot.diameter);
    std::sort(out.begin(), out.end());
    return out;
}

int process_diameter_slot(const ConfigBase &process, const std::string &key, double diameter, FilamentVolumeType mode)
{
    if (!(diameter > 0.) || !is_per_nozzle_process_option(key) || !process_has_diameter_slots(process))
        return -1;
    const std::vector<ToolSlot> slots = process_tool_slots(process);
    const auto                 *opt   = dynamic_cast<const ConfigOptionVectorBase *>(process.option(key));
    if (opt == nullptr || opt->size() != slots.size())
        return -1;
    int first = -1;
    for (size_t s = 0; s < slots.size(); ++s) {
        if (slots[s].tool != 0 || !(slots[s].diameter > 0.) || !same_nozzle_diameter(slots[s].diameter, diameter))
            continue;
        if (first < 0)
            first = int(s);
        if (slots[s].flow_mode == int(mode))
            return int(s);
    }
    return first;
}

bool split_process_per_diameter(DynamicPrintConfig &process, std::vector<double> diameters)
{
    std::sort(diameters.begin(), diameters.end());
    diameters.erase(std::unique(diameters.begin(), diameters.end(), [](double a, double b) { return same_nozzle_diameter(a, b); }),
                    diameters.end());
    diameters.erase(std::remove_if(diameters.begin(), diameters.end(), [](double d) { return !(d > 0.); }), diameters.end());
    if (diameters.size() < 2 || process_nozzles_split(process) || process_has_diameter_slots(process))
        return false;
    const SlotLayout         layout = SlotLayout::of(process);
    std::vector<std::string> modes  = layout.flow_support;
    if (modes.empty())
        modes.emplace_back(FLOW_MODE_STANDARD);
    std::vector<int>         ids;
    std::vector<std::string> variants;
    std::vector<double>      sizes;
    for (double d : diameters)
        for (const std::string &mode : modes) {
            ids.push_back(0); // any tool
            variants.push_back(flow_mode_variant_name(mode));
            sizes.push_back(d);
        }
    for (const std::string &key : per_nozzle_process_options()) {
        auto *opt = dynamic_cast<ConfigOptionVectorBase *>(process.option(key));
        if (opt == nullptr || opt->size() == 0)
            continue;
        // Every size starts from what it prints with now: the flow-mode column of the slot's mode.
        const std::unique_ptr<ConfigOption> before(opt->clone());
        const auto *src = static_cast<const ConfigOptionVectorBase *>(before.get());
        opt->resize(ids.size(), before.get());
        for (size_t s = 0; s < ids.size(); ++s)
            opt->set_at(src, s, layout.column(src->size(), process_variant_fill_mode(variants[s])));
    }
    process.set_key_value("print_extruder_id", new ConfigOptionInts(ids));
    process.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
    process.set_key_value("print_extruder_nozzle_diameter", new ConfigOptionFloats(sizes));
    return true;
}

bool link_process_diameters(DynamicPrintConfig &process, double from, const ConfigBase *reference)
{
    if (!process_has_diameter_slots(process))
        return false;
    const std::vector<ToolSlot> slots = process_tool_slots(process);
    // A reference that is split by size itself gives no shape to go back to.
    if (reference != nullptr && process_has_diameter_slots(*reference))
        reference = nullptr;
    const SlotLayout ref_layout = reference != nullptr ? SlotLayout::of(*reference) : SlotLayout::of(process);
    const bool       ref_valid  = !ref_layout.ids.empty() && ref_layout.ids.size() == ref_layout.variants.size();
    const std::vector<std::string> &flow_support = ref_layout.flow_support;
    for (const std::string &key : per_nozzle_process_options()) {
        auto *opt = dynamic_cast<ConfigOptionVectorBase *>(process.option(key));
        if (opt == nullptr || opt->size() != slots.size())
            continue;
        const auto  *ref = reference != nullptr ? dynamic_cast<const ConfigOptionVectorBase *>(reference->option(key)) : nullptr;
        const size_t n   = ref != nullptr && ref->size() > 0 ? ref->size() : std::max<size_t>(1, flow_support.size());
        const std::unique_ptr<ConfigOption> linked(opt->clone());
        opt->resize(n, linked.get());
        for (size_t i = 0; i < n; ++i) {
            FilamentVolumeType mode = fvtStandard;
            if (reference != nullptr && ref_valid && n == ref_layout.ids.size())
                mode = process_variant_fill_mode(ref_layout.variants[i]);
            else if (i < flow_support.size() && flow_support[i] == FLOW_MODE_HIGH_FLOW)
                mode = fvtHighFlow;
            ToolSlotKey key_of_size;
            key_of_size.tool      = 0;
            key_of_size.flow_mode = int(mode);
            key_of_size.diameter  = from;
            const int src         = match_tool_slot(slots, key_of_size);
            opt->set_at(linked.get(), i, size_t(std::max(0, src)));
        }
    }
    for (const char *key : { "print_extruder_id", "print_extruder_variant" }) {
        if (const ConfigOption *opt = reference != nullptr ? reference->option(key) : nullptr)
            process.set_key_value(key, opt->clone());
        else
            process.erase(key);
    }
    process.set_key_value("print_extruder_nozzle_diameter", new ConfigOptionFloats({ 0. }));
    return true;
}

std::string nozzle_process_value(const DynamicPrintConfig &config, const std::string &key, size_t extruder, bool high_flow)
{
    const auto *opt = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
    if (opt == nullptr || opt->size() == 0)
        return std::string();
    const SlotLayout layout = SlotLayout::of(config);
    const size_t     idx    = layout.index(opt->size(), extruder, high_flow ? fvtHighFlow : fvtStandard);
    return opt->vserialize()[idx];
}

// ---- Per-nozzle process P6 ----------------------------------------------------------------------------------------

const std::vector<std::string> &nozzle_width_option_keys()
{
    static const std::vector<std::string> keys { "line_width",
                                                 "initial_layer_line_width",
                                                 "outer_wall_line_width",
                                                 "inner_wall_line_width",
                                                 "top_surface_line_width",
                                                 "sparse_infill_line_width",
                                                 "internal_solid_infill_line_width",
                                                 "support_line_width",
                                                 "bridge_line_width",
                                                 "skin_infill_line_width",
                                                 "skeleton_infill_line_width" };
    return keys;
}

namespace {

bool is_nozzle_width_key(const std::string &key)
{
    const auto &keys = nozzle_width_option_keys();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

bool parse_width(const std::string &text, FloatOrPercent &out)
{
    std::string s = text;
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    if (s.empty())
        return false;
    bool percent = false;
    if (s.back() == '%') {
        percent = true;
        s.pop_back();
    }
    try {
        size_t       used  = 0;
        const double value = std::stod(s, &used);
        if (used != s.size() || !std::isfinite(value) || value < 0.)
            return false;
        out = FloatOrPercent{ value, percent };
        return true;
    } catch (...) {
        return false;
    }
}

std::string process_nozzle_width_text(const ConfigBase &config)
{
    if (const auto *opt = dynamic_cast<const ConfigOptionString *>(config.option("process_nozzle_line_widths")))
        return opt->value;
    return std::string();
}

bool same_width(const FloatOrPercent &a, const FloatOrPercent &b) { return a.percent == b.percent && std::abs(a.value - b.value) < 1e-9; }

} // namespace

double line_width_reference_diameter(const ConfigBase &config)
{
    if (const auto *opt = dynamic_cast<const ConfigOptionFloat *>(config.option("line_width_nozzle_diameter")); opt != nullptr && opt->value > 0.)
        return opt->value;
    if (const auto *variant = dynamic_cast<const ConfigOptionString *>(config.option("printer_variant")); variant != nullptr) {
        // "0.4", "0.4+0.6": the first number.
        const std::string &v     = variant->value;
        const size_t       start = v.find_first_of("0123456789.");
        if (start != std::string::npos) {
            const size_t end = v.find_first_not_of("0123456789.", start);
            try {
                const double d = string_to_double_decimal_point(v.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (d > 0.05 && d < 3.)
                    return d;
            } catch (...) {
            }
        }
    }
    if (const auto *nozzles = dynamic_cast<const ConfigOptionFloats *>(config.option("nozzle_diameter")); nozzles != nullptr && !nozzles->values.empty())
        return nozzles->values.front();
    return 0.;
}

bool process_nozzle_width_override(const ConfigBase &process, const std::string &key, int tool, double diameter, FloatOrPercent &out)
{
    const std::string text = process_nozzle_width_text(process);
    if (text.empty())
        return false;
    int best = -1;
    for (const FilamentNozzleVariant &v : parse_filament_nozzle_variants(text)) {
        if (v.flow_mode >= 0)
            continue; // widths do not vary per flow type (validate_process_nozzle_line_widths names it)
        if (v.tool > 0 && v.tool != tool)
            continue;
        if (v.diameter > 0. && !(diameter > 0. && same_nozzle_diameter(v.diameter, diameter)))
            continue;
        if (v.tool == 0 && !(v.diameter > 0.))
            continue;
        const int score = (v.tool > 0 ? 2 : 0) + (v.diameter > 0. ? 1 : 0);
        if (score <= best)
            continue;
        for (const auto &[k, value] : v.values) {
            FloatOrPercent w;
            if (k == key && parse_width(value, w)) {
                out  = w;
                best = score;
            }
        }
    }
    return best >= 0;
}

std::string set_process_nozzle_width(const std::string &text, int tool, double diameter, const std::string &key, const std::string &value)
{
    return set_filament_nozzle_value(text, tool, diameter, -1, key, value);
}

std::string validate_nozzle_width_value(const std::string &value)
{
    FloatOrPercent w;
    if (!parse_width(value, w))
        return "\"" + value + "\" is not a line width: a width in mm (0.62) or a percentage of the nozzle diameter (105%).";
    if (!w.percent && w.value > 5.)
        return "\"" + value + "\" mm is wider than any nozzle can print.";
    if (w.percent && w.value > 1000.)
        return "\"" + value + "\" is wider than any nozzle can print.";
    return std::string();
}

std::vector<std::string> validate_process_nozzle_line_widths(const std::string &text)
{
    std::vector<std::string> errors;
    for (const FilamentNozzleVariant &v : parse_filament_nozzle_variants(text, &errors)) {
        if (v.flow_mode >= 0)
            errors.emplace_back("Line widths do not differ per flow type: " + format_filament_nozzle_selector(v.tool, v.diameter, v.flow_mode));
        if (v.tool == 0 && !(v.diameter > 0.))
            errors.emplace_back("An entry needs a tool or a nozzle size.");
        for (const auto &[key, value] : v.values) {
            if (!is_nozzle_width_key(key))
                errors.emplace_back("\"" + key + "\" is not a line width setting.");
            else if (const std::string problem = validate_nozzle_width_value(value); !problem.empty())
                errors.emplace_back(key + ": " + problem);
        }
    }
    return errors;
}

NozzleWidth nozzle_width_for(const ConfigBase &config, const std::string &key, int tool, double diameter)
{
    NozzleWidth out;
    const auto *plate = dynamic_cast<const ConfigOptionFloatOrPercent *>(config.option(key));
    if (plate != nullptr)
        out.value = FloatOrPercent{ plate->value, plate->percent };
    // 2. The table.
    if (FloatOrPercent w; process_nozzle_width_override(config, key, tool, diameter, w)) {
        out.value  = w;
        out.source = NozzleWidthSource::Override;
        return out;
    }
    // A 0 role width stands for line_width: it follows line_width's value for the nozzle when that changes.
    if (out.value.value == 0. && zero_width_is_line_width(key)) {
        const NozzleWidth lw = nozzle_width_for(config, "line_width", tool, diameter);
        if (lw.source != NozzleWidthSource::Plate)
            return lw;
        return out;
    }
    // 3. A percentage or auto resolves against the nozzle as it is.
    if (out.value.percent || !(out.value.value > 0.) || !(diameter > 0.))
        return out;
    // 4. An absolute width on a printer with several nozzle sizes, for a nozzle of another size than the process's.
    //    P7: also on a printer with one size when this project overrides a nozzle size (the process is for the preset's).
    if (printer_nozzle_sizes(config).size() < 2 && !has_nozzle_diameter_override(config))
        return out;
    const double reference = line_width_reference_diameter(config);
    if (!(reference > 0.) || same_nozzle_diameter(reference, diameter))
        return out;
    out.value.value = std::round(out.value.value * diameter / reference * 10000.) / 10000.;
    out.source      = NozzleWidthSource::Scaled;
    return out;
}

bool resolve_nozzle_line_widths(DynamicPrintConfig &config)
{
    const auto  *filaments = dynamic_cast<const ConfigOptionVectorBase *>(config.option("filament_diameter"));
    const size_t count     = filaments != nullptr ? filaments->size() : 0;
    if (count == 0)
        return false;
    // Nothing to resolve on one nozzle size without table entries: the job slices as it always did.
    if (printer_nozzle_sizes(config).size() < 2 && process_nozzle_width_text(config).empty() && !has_nozzle_diameter_override(config))
        return false;

    // Rule 1: a job with per-nozzle processes had its arrays written by fill_nozzle_line_widths, which applies rules
    // 2-4 to the filaments without a process of their own already.
    if (const auto *filled = dynamic_cast<const ConfigOptionFloatsOrPercents *>(config.option("nozzle_outer_wall_line_width"));
        filled != nullptr && filled->values.size() == count)
        return false;
    std::vector<int>    tool_of(count, 0);
    std::vector<double> diameter_of(count, 0.);
    for (size_t f = 0; f < count; ++f) {
        tool_of[f]     = int(nozzle_for_filament(config, unsigned(f + 1))) + 1;
        diameter_of[f] = nozzle_diameter_for_filament(config, unsigned(f + 1));
    }

    std::vector<std::pair<std::string, std::string>> arrays = nozzle_line_width_keys();
    arrays.emplace_back("nozzle_initial_layer_line_width", "initial_layer_line_width");
    arrays.emplace_back("nozzle_line_width", "line_width");

    bool                                             changed = false;
    std::vector<std::vector<FloatOrPercent>>         values(arrays.size());
    for (size_t a = 0; a < arrays.size(); ++a) {
        const auto &[array_key, width_key] = arrays[a];
        const auto *plate                  = config.option<ConfigOptionFloatOrPercent>(width_key);
        const FloatOrPercent plate_width   = plate != nullptr ? FloatOrPercent{ plate->value, plate->percent } : FloatOrPercent{ 0., false };
        values[a].assign(count, plate_width);
        for (size_t f = 0; f < count; ++f) {
            const NozzleWidth w = nozzle_width_for(config, width_key, tool_of[f], diameter_of[f]);
            values[a][f]        = w.value;
            changed |= !same_width(w.value, plate_width);
        }
    }
    if (!changed)
        return false;
    for (size_t a = 0; a < arrays.size(); ++a) {
        auto *opt   = new ConfigOptionFloatsOrPercents();
        opt->values = std::move(values[a]);
        config.set_key_value(arrays[a].first, opt);
    }
    return true;
}

ConfigOptionFloatOrPercent initial_layer_line_width_for_filament(const PrintConfig &config, unsigned int filament)
{
    if (const FloatOrPercent *w = nozzle_width_at(config.nozzle_initial_layer_line_width, int(filament)))
        return ConfigOptionFloatOrPercent(w->value, w->percent);
    return config.initial_layer_line_width;
}

ConfigOptionFloatOrPercent line_width_for_filament(const PrintConfig &config, const ConfigOptionFloatOrPercent &object_width, unsigned int filament)
{
    if (const FloatOrPercent *w = nozzle_width_at(config.nozzle_line_width, int(filament)))
        return ConfigOptionFloatOrPercent(w->value, w->percent);
    return object_width;
}

// ---- Per-nozzle process P8: a whole process profile per tool -------------------------------------------------------

std::string nozzle_tool_id(size_t extruder) { return "T" + std::to_string(extruder + 1); }

int nozzle_tool_id_extruder(const std::string &id)
{
    if (id.size() < 2 || id[0] != 'T')
        return -1;
    for (size_t i = 1; i < id.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(id[i])))
            return -1;
    const int n = std::atoi(id.c_str() + 1);
    return n >= 1 ? n - 1 : -1;
}

std::string nozzle_tool_name(const ConfigBase &config, size_t extruder)
{
    const auto  *nozzles = dynamic_cast<const ConfigOptionVectorBase *>(config.option("nozzle_diameter"));
    const size_t count   = nozzles != nullptr ? nozzles->size() : 0;
    if (count == 2 && is_bambu_grouping_machine(config, 2))
        return std::string(extruder == 0 ? "Left" : "Right") + " (" + nozzle_tool_id(extruder) + ")";
    return nozzle_tool_id(extruder);
}

// ---- P9: rack nozzle sizes as tools -------------------------------------------------------------------------------
//
// Owner hand test round 1 (2026-11-02): the selector groups an extruder's rack by nozzle size. The extruder itself stands for its
// mounted size (id "T2", P8's own id, so a P8 choice keeps working); each other size in its rack is a tool "T2:0.6".

std::string nozzle_size_group_id(size_t extruder, double diameter)
{
    return nozzle_tool_id(extruder) + ":" + format_nozzle_diameter_word(diameter);
}

bool parse_nozzle_size_group_id(const std::string &id, size_t &extruder, double &diameter)
{
    const size_t colon = id.find(':');
    if (colon == std::string::npos || colon + 1 >= id.size())
        return false;
    const int e = nozzle_tool_id_extruder(id.substr(0, colon));
    if (e < 0)
        return false;
    const std::string word = id.substr(colon + 1);
    size_t            used = 0;
    const double      d    = string_to_double_decimal_point(word, &used); // "0.4" in any UI locale
    if (used == 0 || used != word.size() || !(d > 0.) || d > 3.)
        return false;
    extruder = size_t(e);
    diameter = d;
    return true;
}

std::vector<NozzleToolEntry> nozzle_tool_entries(const ConfigBase &printer_config)
{
    std::vector<NozzleToolEntry> out;
    const std::vector<double>    nozzles = effective_nozzle_diameters(printer_config);
    const auto                   racks   = extruder_rack_nozzles(printer_config);
    for (size_t e = 0; e < nozzles.size(); ++e) {
        out.push_back({ e, 0, nozzles[e] });
        if (rack_slot_count(printer_config, e) == 0)
            continue;
        // The rack's other sizes, in rack order, each once.
        std::vector<double> sizes;
        for (const RackNozzle &nozzle : racks[e])
            if (!same_nozzle_diameter(nozzle.diameter, nozzles[e]) &&
                std::none_of(sizes.begin(), sizes.end(), [&nozzle](double d) { return same_nozzle_diameter(d, nozzle.diameter); }))
                sizes.push_back(nozzle.diameter);
        for (size_t k = 0; k < sizes.size(); ++k)
            out.push_back({ e, int(k) + 1, sizes[k] });
    }
    return out;
}

std::vector<NozzleToolEntry> nozzle_slot_tools(const ConfigBase &printer_config)
{
    std::vector<NozzleToolEntry> out;
    for (const NozzleToolEntry &entry : nozzle_tool_entries(printer_config))
        if (entry.slot > 0)
            out.push_back(entry);
    return out;
}

std::string nozzle_tool_entry_id(const NozzleToolEntry &entry)
{
    return entry.slot > 0 ? nozzle_size_group_id(entry.extruder, entry.diameter) : nozzle_tool_id(entry.extruder);
}

std::string nozzle_tool_entry_name(const ConfigBase &config, const NozzleToolEntry &entry)
{
    return entry.slot > 0 ? rack_size_name(config, entry.extruder, entry.diameter) : nozzle_tool_name(config, entry.extruder);
}

std::string nozzle_filament_group_id(const ConfigBase &config, unsigned int filament_id)
{
    const unsigned int f = std::max(filament_id, 1u);
    const size_t       e = nozzle_for_filament(config, f);
    if (rack_slot_count(config, e) == 0)
        return std::string();
    const std::vector<double> nozzles = effective_nozzle_diameters(config);
    const double              d       = nozzle_diameter_for_filament(config, f);
    if (e >= nozzles.size() || same_nozzle_diameter(d, nozzles[e]))
        return std::string();
    return nozzle_size_group_id(e, d);
}

std::string nozzle_filament_tool_name(const ConfigBase &config, unsigned int filament_id)
{
    const size_t e = nozzle_for_filament(config, std::max(filament_id, 1u));
    if (!nozzle_filament_group_id(config, filament_id).empty())
        return rack_size_name(config, e, nozzle_diameter_for_filament(config, std::max(filament_id, 1u)));
    return nozzle_tool_name(config, e);
}

std::string nozzle_filament_tool_id(const ConfigBase &config, unsigned int filament_id)
{
    if (std::string id = nozzle_filament_group_id(config, filament_id); !id.empty())
        return id;
    return nozzle_tool_id(nozzle_for_filament(config, std::max(filament_id, 1u)));
}

std::map<std::string, std::string> nozzle_slot_process_names(const ConfigBase &config)
{
    std::map<std::string, std::string> out;
    if (const auto *opt = dynamic_cast<const ConfigOptionStrings *>(config.option("nozzle_slot_process_presets")))
        for (const std::string &entry : opt->values) {
            const size_t nl = entry.find('\n');
            if (nl == std::string::npos)
                continue;
            std::string id   = entry.substr(0, nl);
            std::string name = entry.substr(nl + 1);
            boost::algorithm::trim(id);
            boost::algorithm::trim_right(name);
            size_t e = 0;
            double d = 0.;
            if (!name.empty() && parse_nozzle_size_group_id(id, e, d))
                out[id] = name;
        }
    return out;
}

std::vector<std::string> serialize_nozzle_slot_process_names(const std::map<std::string, std::string> &names)
{
    std::vector<std::string> out;
    for (const auto &[id, name] : names)
        if (!name.empty())
            out.push_back(id + "\n" + name);
    return out;
}

std::string nozzle_size_group_missing_error(const ConfigBase &config, size_t extruder, double diameter)
{
    char n[32];
    snprintf(n, sizeof(n), "%g", std::round(diameter * 1000.) / 1000.);
    const std::string tool = rack_size_name(config, extruder, diameter);
    return (boost::format(L("%1% (the rack's %2% mm nozzles): neither the plate's process nor any process of this printer is made for "
                            "%2% mm nozzles. Choose a process for %1% on the Process tab, or change the rack's nozzles.")) %
            tool % n)
        .str();
}

namespace {

// The keys that route a region's roles to filaments: plate-wide, they decide which tool prints what.
bool is_region_routing_key(const std::string &key)
{
    return key == "wall_filament" || key == "outer_wall_filament" || key == "sparse_infill_filament" || key == "solid_infill_filament";
}

// Process keys that describe a profile rather than set a value: neither shown as Plate nor taken per tool.
bool is_process_meta_key(const std::string &key)
{
    static const std::set<std::string> keys { "inherits", "compatible_printers", "compatible_printers_condition", "print_settings_id",
                                              "print_extruder_id", "print_extruder_variant", "print_extruder_nozzle_diameter",
                                              "process_extruder_values", "process_flow_support", "version", "setting_id", "from",
                                              "name", "print_compatible_printers" };
    return keys.count(key) > 0;
}

enum class RegionRole { Wall, Sparse, Solid };

// The role whose filament a region key is taken from (stamp_nozzle_region_values): top / bottom / solid / bridge /
// ironing settings from the solid infill filament, other infill settings from the sparse infill filament, the rest
// (walls, seams, fuzzy skin, overhangs, flow) from the wall filament.
RegionRole region_key_role(const std::string &key)
{
    if (key == "only_one_wall_top" || key == "min_width_top_surface" || key == "top_bottom_infill_wall_overlap")
        return RegionRole::Wall;
    if (boost::starts_with(key, "top_") || boost::starts_with(key, "bottom_") || key.find("solid") != std::string::npos ||
        key.find("bridge") != std::string::npos || boost::starts_with(key, "ironing") || key == "infill_complete_top")
        return RegionRole::Solid;
    if (key.find("infill") != std::string::npos || boost::starts_with(key, "lightning_") || boost::starts_with(key, "lateral_lattice") ||
        boost::starts_with(key, "gyroid") || boost::starts_with(key, "fill_") || boost::starts_with(key, "skin_") ||
        boost::starts_with(key, "skeleton_") || boost::starts_with(key, "locked_") || boost::starts_with(key, "adaptive_infill"))
        return RegionRole::Sparse;
    return RegionRole::Wall;
}

std::string serialize_option_lines(const ConfigBase &config, const std::vector<std::string> &keys)
{
    std::string out;
    for (const std::string &key : keys)
        if (const ConfigOption *opt = config.option(key)) {
            if (!out.empty())
                out += '\n';
            out += key + " = " + opt->serialize();
        }
    return out;
}

// "key = value" lines into `out` (unknown keys and values that do not parse are skipped).
void parse_option_lines(const std::string &text, ConfigBase &out, const std::function<bool(const std::string &)> &accept = nullptr)
{
    std::istringstream in(text);
    std::string        line;
    ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::EnableSilent);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t eq = line.find(" = ");
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        boost::algorithm::trim(key);
        if (key.empty() || print_config_def.get(key) == nullptr || (accept && !accept(key)))
            continue;
        try {
            out.set_deserialize_nothrow(key, line.substr(eq + 3), ctxt);
        } catch (const std::exception &) {
        }
    }
}

} // namespace

const std::vector<std::string> &nozzle_region_option_keys()
{
    static const std::vector<std::string> keys = [] {
        const std::vector<std::string> &print_options = Preset::print_options();
        const std::set<std::string>     process(print_options.begin(), print_options.end());
        std::vector<std::string>        out;
        for (const std::string &key : PrintRegionConfig().keys()) {
            if (process.count(key) == 0 || is_region_routing_key(key) || boost::starts_with(key, "nozzle_") ||
                is_per_nozzle_process_option(key) || is_nozzle_width_key(key) || is_process_meta_key(key))
                continue;
            out.push_back(key);
        }
        std::sort(out.begin(), out.end());
        return out;
    }();
    return keys;
}

bool is_nozzle_region_option(const std::string &key)
{
    const auto &keys = nozzle_region_option_keys();
    return std::binary_search(keys.begin(), keys.end(), key);
}

bool is_per_tool_process_option(const std::string &key)
{
    return is_per_nozzle_process_option(key) || is_nozzle_width_key(key) || is_nozzle_region_option(key) ||
           key == "line_width_nozzle_diameter" || key == "process_nozzle_line_widths";
}

bool is_plate_process_option(const std::string &key)
{
    if (is_per_tool_process_option(key) || is_process_meta_key(key))
        return false;
    static const std::set<std::string> process(Preset::print_options().begin(), Preset::print_options().end());
    return process.count(key) > 0;
}

std::vector<std::string> plate_process_option_keys()
{
    std::vector<std::string> out;
    for (const std::string &key : Preset::print_options())
        if (is_plate_process_option(key))
            out.push_back(key);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

NozzleProcessEdits parse_nozzle_process_edits(const std::vector<std::string> &entries)
{
    NozzleProcessEdits out;
    for (const std::string &entry : entries) {
        const size_t nl = entry.find('\n');
        std::string  id = entry.substr(0, nl);
        boost::algorithm::trim(id);
        if (id.empty())
            continue;
        DynamicPrintConfig values;
        if (nl != std::string::npos)
            parse_option_lines(entry.substr(nl + 1), values, [](const std::string &key) { return is_per_tool_process_option(key); });
        if (!values.empty())
            out[id] = std::move(values);
    }
    return out;
}

NozzleProcessEdits nozzle_process_edits(const ConfigBase &config)
{
    const auto *opt = dynamic_cast<const ConfigOptionStrings *>(config.option("nozzle_process_edits"));
    return opt != nullptr ? parse_nozzle_process_edits(opt->values) : NozzleProcessEdits();
}

std::vector<std::string> serialize_nozzle_process_edits(const NozzleProcessEdits &edits)
{
    std::vector<std::string> out;
    for (const auto &[id, values] : edits) {
        if (values.empty())
            continue;
        std::vector<std::string> keys = values.keys();
        std::sort(keys.begin(), keys.end());
        out.push_back(id + "\n" + serialize_option_lines(values, keys));
    }
    return out;
}

DynamicPrintConfig nozzle_process_diff(const DynamicPrintConfig &edited, const DynamicPrintConfig &base)
{
    DynamicPrintConfig out;
    for (const std::string &key : edited.keys()) {
        if (!is_per_tool_process_option(key))
            continue;
        const ConfigOption *a = edited.option(key);
        const ConfigOption *b = base.option(key);
        if (a != nullptr && (b == nullptr || !(*a == *b)))
            out.set_key_value(key, a->clone());
    }
    return out;
}

DynamicPrintConfig nozzle_tool_process(const DynamicPrintConfig &plate, const DynamicPrintConfig &base, const DynamicPrintConfig *edits)
{
    DynamicPrintConfig out = base;
    if (edits != nullptr)
        for (const std::string &key : edits->keys())
            if (is_per_tool_process_option(key))
                out.set_key_value(key, edits->option(key)->clone());
    // The plate-wide keys are the plate's, whatever the tool's profile holds.
    for (const std::string &key : plate.keys())
        if (is_plate_process_option(key))
            out.set_key_value(key, plate.option(key)->clone());
    return out;
}

namespace {
// The quality word of a process name ("0.20mm Standard @Snapmaker U1 (0.4 nozzle)" -> "Standard").
std::string process_quality_word(const std::string &name)
{
    const size_t mm = name.find("mm ");
    const size_t at = name.find(" @");
    if (mm == std::string::npos || at == std::string::npos || at <= mm + 3)
        return std::string();
    return name.substr(mm + 3, at - mm - 3);
}
} // namespace

std::string pick_nozzle_process(std::vector<NozzleProcessCandidate> candidates, double plate_layer_height, const std::string &plate_name)
{
    if (candidates.empty())
        return std::string();
    const std::string word = process_quality_word(plate_name);
    auto rank = [&](const NozzleProcessCandidate &c) {
        // The layer height is the Plate's anyway: the kind of profile ("Standard", "Strength"...) matters more.
        return std::make_tuple(c.system ? 0 : 1, (!word.empty() && process_quality_word(c.name) == word) ? 0 : 1,
                               int(std::lround(std::abs(c.layer_height - plate_layer_height) * 1000.)),
                               process_quality_word(c.name) == "Standard" ? 0 : 1, c.name);
    };
    return std::min_element(candidates.begin(), candidates.end(),
                            [&](const NozzleProcessCandidate &a, const NozzleProcessCandidate &b) { return rank(a) < rank(b); })
        ->name;
}

bool fill_nozzle_region_values(DynamicPrintConfig &config, const std::vector<const DynamicPrintConfig *> &sources,
                               const std::vector<const DynamicPrintConfig *> *filament_sources)
{
    const auto  *filaments = dynamic_cast<const ConfigOptionVectorBase *>(config.option("filament_diameter"));
    const size_t count     = filaments != nullptr ? filaments->size() : 0;
    if (filament_sources != nullptr && filament_sources->size() != count)
        filament_sources = nullptr;
    // P9: with per-filament sources (rack slots), those are the processes that print.
    const std::vector<const DynamicPrintConfig *> &candidates = filament_sources != nullptr ? *filament_sources : sources;
    // The keys some tool's process sets to another value than the plate: every filament's entry carries all of
    // them, so a region a modifier moves to another filament is stamped completely.
    std::vector<std::string> keys;
    for (const std::string &key : nozzle_region_option_keys()) {
        const ConfigOption *plate = config.option(key);
        if (plate == nullptr)
            continue;
        for (const DynamicPrintConfig *src : candidates)
            if (src != nullptr)
                if (const ConfigOption *own = src->option(key); own != nullptr && own->type() == plate->type() && !(*own == *plate)) {
                    keys.push_back(key);
                    break;
                }
    }
    auto *opt = new ConfigOptionStrings();
    if (!keys.empty()) {
        opt->values.assign(count, std::string());
        for (size_t f = 0; f < count; ++f) {
            const size_t              e   = nozzle_for_filament(config, unsigned(f + 1));
            const DynamicPrintConfig *src = filament_sources != nullptr ? (*filament_sources)[f] : e < sources.size() ? sources[e] : nullptr;
            std::string               text;
            for (const std::string &key : keys) {
                const ConfigOption *value = src != nullptr ? src->option(key) : nullptr;
                if (value == nullptr || value->type() != config.option(key)->type())
                    value = config.option(key);
                if (!text.empty())
                    text += '\n';
                text += key + " = " + value->serialize();
            }
            opt->values[f] = std::move(text);
        }
    }
    config.set_key_value("nozzle_region_values", opt);
    return !keys.empty();
}

void stamp_nozzle_region_values(PrintRegionConfig &c)
{
    const std::vector<std::string> &entries = c.nozzle_region_values.values;
    if (entries.empty())
        return;
    // The filament of each role, as PrintRegion::extruder() resolves it (stamp_nozzle_line_widths).
    const int wall   = c.wall_filament.value;
    const int sparse = c.sparse_infill_filament.value;
    const int solid  = std::abs(c.sparse_infill_density.value - 100.) < EPSILON ? sparse : c.solid_infill_filament.value;
    std::map<int, DynamicPrintConfig> parsed;
    auto values_of = [&](int filament) -> const DynamicPrintConfig * {
        if (filament < 1 || size_t(filament) > entries.size())
            return nullptr;
        auto it = parsed.find(filament);
        if (it == parsed.end()) {
            DynamicPrintConfig values;
            parse_option_lines(entries[size_t(filament - 1)], values, [](const std::string &key) { return is_nozzle_region_option(key); });
            it = parsed.emplace(filament, std::move(values)).first;
        }
        return &it->second;
    };
    std::set<std::string> keys;
    for (int f : { wall, sparse, solid })
        if (const DynamicPrintConfig *values = values_of(f))
            for (const std::string &key : values->keys())
                keys.insert(key);
    for (const std::string &key : keys) {
        const RegionRole          role   = region_key_role(key);
        const DynamicPrintConfig *values = values_of(role == RegionRole::Wall ? wall : role == RegionRole::Sparse ? sparse : solid);
        ConfigOption             *dst    = c.option(key);
        if (values == nullptr || dst == nullptr)
            continue;
        if (const ConfigOption *src = values->option(key); src != nullptr && src->type() == dst->type())
            dst->set(src);
    }
}

std::string nozzle_layer_height_error(const ConfigBase &config, unsigned int filament_id, bool first_layer, double layer_height, double nozzle)
{
    const auto *nozzles   = dynamic_cast<const ConfigOptionVectorBase *>(config.option("nozzle_diameter"));
    const bool  several   = nozzles != nullptr && nozzles->size() >= 2;
    const std::string what = first_layer ? L("first layer height") : L("layer height");
    char h[32], n[32];
    snprintf(h, sizeof(h), "%g", std::round(layer_height * 1000.) / 1000.);
    snprintf(n, sizeof(n), "%g", std::round(nozzle * 1000.) / 1000.);
    if (!several)
        return (boost::format(L("The nozzle is %1% mm; %2% %3% mm is too high (max %1% mm). Lower the %2%, or use a larger nozzle.")) % n % what % h)
            .str();
    // P9: a filament on a rack slot names the slot ("Right slot 3").
    const std::string tool = nozzle_filament_tool_name(config, filament_id);
    return (boost::format(L("%1% has a %2% mm nozzle; %3% %4% mm is too high (max %2% mm). Lower it on Plate, or change %1%'s nozzle.")) % tool % n %
            what % h)
        .str();
}

std::string nozzle_process_missing_error(const ConfigBase &config, size_t extruder)
{
    const auto *nozzles = dynamic_cast<const ConfigOptionFloats *>(config.option("nozzle_diameter"));
    char        n[32];
    snprintf(n, sizeof(n), "%g", nozzles != nullptr ? std::round(nozzles->get_at(extruder) * 1000.) / 1000. : 0.);
    const std::string tool = nozzle_tool_name(config, extruder);
    return (boost::format(L("%1% has a %2% mm nozzle, and neither the plate's process nor any process of this printer is made for "
                            "%2% mm nozzles. Choose a process for %1% on the Process tab, or change %1%'s nozzle.")) %
            tool % n)
        .str();
}

} // namespace Slic3r
