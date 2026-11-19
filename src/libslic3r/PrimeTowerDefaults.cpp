#include "PrimeTowerDefaults.hpp"

namespace Slic3r {
namespace PrimeTowerDefaults {

const std::vector<std::string>& bambu_default_tower_keys()
{
    // Rows 2-4, 7-46 and 48 of the table in design_prime_tower_menu_rework.md 2.1, in Tab.cpp's row order.
    static const std::vector<std::string> keys {
        "prime_tower_width",
        "prime_volume",
        "prime_tower_brim_width",
        "wipe_tower_rotation_angle",
        "wipe_tower_bridging",
        "wipe_tower_extra_spacing",
        "wipe_tower_extra_flow",
        "wipe_tower_max_purge_speed",
        "prime_tower_acceleration",
        "prime_tower_line_width",
        "prime_tower_infill_gap",
        "prime_tower_enable_framework",
        "prime_tower_flat_ironing",
        "prime_tower_line_ironing",
        "prime_tower_ironing_speed",
        "prime_tower_feedrate_factor",
        "prime_tower_tail_speed_factor",
        "prime_tower_tail_slowdown_volume",
        "prime_tower_side_scrape",
        "prime_tower_side_scrape_face",
        "prime_tower_side_scrape_length",
        "prime_tower_side_scrape_height",
        "prime_tower_side_scrape_speed",
        "prime_tower_side_scrape_wall_gap",
        "prime_tower_side_scrape_point_spacing",
        "prime_tower_side_scrape_along_length",
        "prime_tower_wipe_speed",
        "prime_tower_wipe_distance",
        "wipe_tower_wall_type",
        "wipe_tower_cone_angle",
        "wipe_tower_extra_rib_length",
        "wipe_tower_rib_width",
        "wipe_tower_fillet_wall",
        "wipe_tower_no_sparse_layers",
        "wipe_tower_sparse_layers_combination",
        "wipe_tower_wall_gap",
        "wipe_tower_stagger_toolchange_start",
        "enable_tower_interface_features",
        "enable_tower_interface_cooldown_during_tower",
        "wipe_tower_interface_temp",
        "wipe_tower_interface_run_in",
        "wipe_tower_interface_extra_prime",
        "wipe_tower_interface_trigger",
        "single_extruder_multi_material_priming",
        "preload_all_filaments",
    };
    return keys;
}

const std::vector<std::string>& excluded_tower_keys()
{
    static const std::vector<std::string> keys {
        "enable_prime_tower",
        "prime_tower_brim_chamfer",
        "prime_tower_brim_chamfer_max_width",
        "wipe_tower_filament",
        "local_z_wipe_tower_purge_lines",
    };
    return keys;
}

bool offered(bool is_bbl_printer, bool prime_tower_enabled)
{
    return is_bbl_printer && prime_tower_enabled;
}

DynamicPrintConfig bambu_tower_defaults(const DynamicPrintConfig& system_process)
{
    DynamicPrintConfig out;
    for (const std::string& key : bambu_default_tower_keys()) {
        if (const ConfigOption* opt = system_process.option(key); opt != nullptr)
            out.set_key_value(key, opt->clone());
        else if (const ConfigOptionDef* def = print_config_def.get(key); def != nullptr && def->default_value)
            out.set_key_value(key, def->default_value->clone());
    }
    // Bambu Studio: prime_tower_rib_wall defaults to true (BS src/libslic3r/PrintConfig.cpp:7316-7321) and no Bambu
    // process preset sets it, so Bambu Studio prints a rib-wall tower on every Bambu printer. Edge's code default and
    // its BBL system presets give a rectangle (design D7: override to Rib).
    out.set_key_value("wipe_tower_wall_type", new ConfigOptionEnum<WipeTowerWallType>(wtwRib));
    return out;
}

std::vector<std::string> changed_keys(const DynamicPrintConfig& current, const DynamicPrintConfig& defaults)
{
    std::vector<std::string> out;
    for (const std::string& key : bambu_default_tower_keys()) {
        const ConfigOption* now  = current.option(key);
        const ConfigOption* want = defaults.option(key);
        if (now == nullptr || want == nullptr)
            continue;
        if (now->type() != want->type() || *now != *want)
            out.push_back(key);
    }
    return out;
}

DynamicPrintConfig changes_to_apply(const DynamicPrintConfig& current, const DynamicPrintConfig& defaults)
{
    DynamicPrintConfig out;
    for (const std::string& key : changed_keys(current, defaults))
        out.set_key_value(key, defaults.option(key)->clone());
    return out;
}

} // namespace PrimeTowerDefaults
} // namespace Slic3r
