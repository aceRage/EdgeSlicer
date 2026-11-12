#include "CliNamedPresets.hpp"

namespace Slic3r {

CliNamedPresetOrigin cli_named_preset_origin(bool is_printer, bool is_user, const std::string &system_parent)
{
    if (is_printer && is_user && !system_parent.empty())
        return {"User", system_parent};
    return {"system", ""};
}

std::string cli_config_system_name(const std::string &from, const std::string &name, const std::string &inherits)
{
    return from == "system" ? name : inherits;
}

} // namespace Slic3r
