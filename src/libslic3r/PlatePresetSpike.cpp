#include "PlatePresetSpike.hpp"

#include "libslic3r/AppConfig.hpp"

#include <cstdlib>
#include <map>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>

namespace Slic3r {

static bool env_flag_on(const char *name)
{
    const char *env = boost::nowide::getenv(name);
    if (env == nullptr || env[0] == '\0')
        return false;
    return env[0] == '1' || boost::iequals(env, "true") || boost::iequals(env, "yes");
}

bool per_plate_presets_spike_env_enabled() { return env_flag_on("EDGE_PLATE_PRESETS_SPIKE"); }

bool per_plate_presets_spike_enabled(const AppConfig *cfg)
{
    if (per_plate_presets_spike_env_enabled())
        return true;
    return cfg != nullptr && cfg->get_bool("per_plate_presets");
}

bool is_bbl_printer_from_config(const DynamicPrintConfig &cfg)
{
    auto starts_bbl = [](const std::string &s) { return s.compare(0, 9, "Bambu Lab") == 0; };
    if (const auto *model = cfg.option<ConfigOptionString>("printer_model")) {
        if (!model->value.empty())
            return starts_bbl(model->value);
    }
    if (const auto *id = cfg.option<ConfigOptionString>("printer_settings_id")) {
        if (!id->value.empty())
            return starts_bbl(id->value);
    }
    return false;
}

std::string spike_plate_config_env_key(int plate_index) { return "EDGE_PLATE_PRESET_CONFIG_" + std::to_string(plate_index); }

bool load_spike_plate_config_file(int plate_index, DynamicPrintConfig &out)
{
    const std::string key = spike_plate_config_env_key(plate_index);
    const char       *path = boost::nowide::getenv(key.c_str());
    if (path == nullptr || path[0] == '\0')
        return false;

    const std::string file(path);
    const std::string ext = boost::algorithm::to_lower_copy(boost::filesystem::path(file).extension().string());
    DynamicPrintConfig file_cfg;
    try {
        if (ext == ".json") {
            std::map<std::string, std::string> key_values;
            std::string                        reason;
            file_cfg.load_from_json(file, ForwardCompatibilitySubstitutionRule::Disable, key_values, reason);
            if (!reason.empty()) {
                BOOST_LOG_TRIVIAL(warning) << "spike: load_from_json " << file << " reason=" << reason;
                return false;
            }
        } else {
            file_cfg.load_from_ini(file, ForwardCompatibilitySubstitutionRule::Disable);
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "spike: failed to load plate " << plate_index << " config from " << file << ": " << e.what();
        return false;
    }
    out = DynamicPrintConfig::full_print_config();
    out.apply(file_cfg);

    BOOST_LOG_TRIVIAL(info) << "spike: loaded plate " << plate_index << " base config from " << file;
    return true;
}

DynamicPrintConfig cli_base_config_for_plate(const DynamicPrintConfig &project_base, int plate_index)
{
    DynamicPrintConfig from_file;
    if (load_spike_plate_config_file(plate_index, from_file))
        return from_file;
    return project_base;
}

} // namespace Slic3r
