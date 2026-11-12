#include "CliPresetSlots.hpp"

#include "PresetFlowVariant.hpp"
#include "PrintConfig.hpp"

#include <algorithm>
#include <memory>

namespace Slic3r {

int cli_filament_slot_count(int project_filament_count, int loaded_filament_count)
{
    return std::max(project_filament_count, loaded_filament_count);
}

void resize_cli_preset_slots(std::vector<std::string> &slots, int filament_count)
{
    const size_t wanted = size_t(std::max(filament_count, 0)) + 2;
    if (slots.size() >= 2 && slots.size() < wanted) {
        std::string printer = std::move(slots.back());
        slots.back().clear();
        slots.resize(wanted);
        slots.back() = std::move(printer);
    } else
        slots.resize(wanted);
}

static bool is_filament_flow_variant_list(const std::string &key)
{
    return key == "filament_flow_support" || is_filament_flow_variant_option(key);
}

void cli_flatten_filament_flow_variants(DynamicPrintConfig &config, size_t filament_count)
{
    if (filament_count == 0)
        return;
    // Resolved against the project's layout as it stands, so every list is read before any is changed.
    std::vector<size_t> read_at(filament_count);
    for (size_t f = 0; f < filament_count; ++f)
        read_at[f] = get_config_idx(config, ConfigFlowDomain::Filament, unsigned(f));
    for (const std::string &key : config.keys()) {
        if (!is_filament_flow_variant_list(key))
            continue;
        auto *opt = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (opt == nullptr || opt->empty())
            continue;
        std::unique_ptr<ConfigOptionVectorBase> packed(static_cast<ConfigOptionVectorBase *>(opt->clone()));
        opt->resize(filament_count);
        for (size_t f = 0; f < filament_count; ++f)
            opt->set_at(packed.get(), f, read_at[f]);
    }
    if (auto *steps = config.option<ConfigOptionInts>("filament_flow_step_size"))
        steps->values.assign(filament_count, 1);
}

void cli_set_filament_preset_value(DynamicPrintConfig &config, const std::string &key, const DynamicPrintConfig &filament,
                                   size_t filament_idx, size_t filament_count)
{
    auto       *dst = dynamic_cast<ConfigOptionVectorBase *>(config.option(key, true));
    const auto *src = dynamic_cast<const ConfigOptionVectorBase *>(filament.option(key));
    if (dst == nullptr || src == nullptr || src->empty())
        return;
    size_t from = 0;
    if (is_filament_flow_variant_list(key)) {
        from = get_preset_flow_variant_idx(filament, ConfigFlowDomain::Filament, filament_volume_type_at(config, unsigned(filament_idx)));
        // Past the end of the list, or no value of its own over a set first one: the first, as the application
        // composes it (compose_filament_flow_variant_segment).
        if (from >= src->size() || (from > 0 && src->nullable() && src->is_nil(from) && !src->is_nil(0)))
            from = 0;
    }
    if (dst->empty())
        dst->resize(1, src);
    dst->set_at(src, filament_idx, from);
    if (filament_count > filament_idx && dst->size() > filament_count)
        dst->resize(filament_count);
}

} // namespace Slic3r
