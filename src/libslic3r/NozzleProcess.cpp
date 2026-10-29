// Per-nozzle process P1 - see NozzleProcess.hpp.

#include "NozzleProcess.hpp"

#include "PrintConfig.hpp"

#include <algorithm>
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

    if (!keep) {
        config.set_key_value("print_extruder_id", new ConfigOptionInts(ids));
        config.set_key_value("print_extruder_variant", new ConfigOptionStrings(variants));
    }
    // The slicer reads the extruder slots only when asked to; a per-nozzle process whose values equal the
    // plate's changes nothing, so it does not ask.
    config.set_key_value("process_extruder_values", new ConfigOptionBool(explicit_values || process_extruder_slots_differ(config)));
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
