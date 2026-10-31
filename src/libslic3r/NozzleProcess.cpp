// Per-nozzle process P1 - see NozzleProcess.hpp.

#include "NozzleProcess.hpp"

#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <boost/log/trivial.hpp>

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
        { "nozzle_support_line_width", "support_line_width" },
    };
    return keys;
}

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

bool fill_nozzle_line_widths(DynamicPrintConfig &config, const std::vector<const DynamicPrintConfig *> &sources)
{
    const auto  *filaments = dynamic_cast<const ConfigOptionVectorBase *>(config.option("filament_diameter"));
    const size_t count     = filaments != nullptr ? filaments->size() : 0;
    // Per filament: the process of the nozzle it prints on, nullptr for the plate's.
    std::vector<const DynamicPrintConfig *> per_filament(count, nullptr);
    bool any = false;
    for (size_t f = 0; f < count; ++f) {
        const size_t e = nozzle_for_filament(config, unsigned(f + 1));
        if (e < sources.size() && sources[e] != nullptr) {
            per_filament[f] = sources[e];
            any = true;
        }
    }
    for (const auto &[array_key, width_key] : nozzle_line_width_keys()) {
        auto *opt = new ConfigOptionFloatsOrPercents();
        if (any) {
            // A filament on a nozzle without a process of its own keeps the plate's width (0 = line_width, as
            // the plate has it), so a region whose filament a modifier changes is stamped back correctly.
            const auto *plate = config.option<ConfigOptionFloatOrPercent>(width_key);
            const FloatOrPercent plate_width = plate != nullptr ? FloatOrPercent{ plate->value, plate->percent } : FloatOrPercent{ 0., false };
            opt->values.assign(count, plate_width);
            for (size_t f = 0; f < count; ++f) {
                const DynamicPrintConfig *src = per_filament[f];
                if (src == nullptr)
                    continue;
                const auto *width = src->option<ConfigOptionFloatOrPercent>(width_key);
                if (width == nullptr || width->value == 0.)
                    width = src->option<ConfigOptionFloatOrPercent>("line_width"); // a 0 role width means line_width
                if (width != nullptr && width->value > 0.)
                    opt->values[f] = FloatOrPercent{ width->value, width->percent };
            }
        }
        config.set_key_value(array_key, opt);
    }
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
}

void stamp_nozzle_line_widths(PrintObjectConfig &c)
{
    // Support "0 = current filament" has no nozzle of its own: it keeps the object's width.
    stamp_width(c.support_line_width, c.nozzle_support_line_width, c.support_filament.value);
}

bool fill_nozzle_process_slots(DynamicPrintConfig &config, const std::vector<const DynamicPrintConfig *> &sources)
{
    if (!supports_nozzle_processes(config))
        return false;
    const size_t extruders = static_cast<const ConfigOptionVectorBase *>(config.option("nozzle_diameter"))->size();
    bool any_source = false;
    for (size_t e = 0; e < std::min(extruders, sources.size()); ++e)
        any_source |= sources[e] != nullptr;
    if (!any_source)
        return false;

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
    const size_t layout = ids.size();

    std::vector<SlotLayout> source_layouts(sources.size());
    for (size_t e = 0; e < sources.size(); ++e)
        if (sources[e] != nullptr)
            source_layouts[e] = SlotLayout::of(*sources[e]);

    for (const std::string &key : per_nozzle_process_options()) {
        auto *opt = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (opt == nullptr || opt->size() == 0)
            continue;
        const std::unique_ptr<ConfigOption> plate_opt(opt->clone());
        const auto  *plate_vec      = static_cast<const ConfigOptionVectorBase *>(plate_opt.get());
        const bool   plate_in_place = keep && plate_vec->size() == layout;
        opt->resize(layout, plate_opt.get());
        for (size_t s = 0; s < layout; ++s) {
            const size_t             e    = size_t(ids[s] - 1);
            const FilamentVolumeType mode = process_variant_fill_mode(variants[s]);
            const DynamicPrintConfig *src = e < sources.size() ? sources[e] : nullptr;
            if (src != nullptr) {
                const auto *src_vec = dynamic_cast<const ConfigOptionVectorBase *>(src->option(key));
                if (src_vec != nullptr && src_vec->size() > 0 && src_vec->type() == opt->type()) {
                    opt->set_at(src_vec, s, source_layouts[e].index(src_vec->size(), e, mode));
                    continue;
                }
            }
            const size_t plate_idx = !plate_in_place                ? plate.index(plate_vec->size(), e, mode) :
                                     (explicit_values || e == 0) ? s :
                                                                   plate.column(plate_vec->size(), mode);
            opt->set_at(plate_vec, s, plate_idx);
        }
    }

    // Per-nozzle process P2: the line widths of each filament's nozzle process.
    fill_nozzle_line_widths(config, sources);

    if (!keep) {
        config.set_key_value("print_extruder_id", new ConfigOptionInts(ids));
        config.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
        // Per-nozzle process P4: a new layout has no diameter dimension.
        config.set_key_value("print_extruder_nozzle_diameter", new ConfigOptionFloats());
    }
    // The slicer reads the extruder slots only when asked to; a per-nozzle process whose values equal the
    // plate's changes nothing, so it does not ask.
    config.set_key_value("process_extruder_values", new ConfigOptionBool(explicit_values || process_extruder_slots_differ(config)));
    return true;
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
        process.set_key_value("print_extruder_nozzle_diameter", new ConfigOptionFloats());
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

std::string nozzle_process_value(const DynamicPrintConfig &config, const std::string &key, size_t extruder, bool high_flow)
{
    const auto *opt = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
    if (opt == nullptr || opt->size() == 0)
        return std::string();
    const SlotLayout layout = SlotLayout::of(config);
    const size_t     idx    = layout.index(opt->size(), extruder, high_flow ? fvtHighFlow : fvtStandard);
    return opt->vserialize()[idx];
}

} // namespace Slic3r
