#include "PluginHost.hpp"

#include <pybind11/pybind11.h>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginManager.hpp>

#include <stdexcept>
#include <string>

namespace Slic3r {

void PluginHost::RegisterBindings(pybind11::module_& module)
{
    auto host        = module.def_submodule("host", "Host application API");
    auto plugin_host = host.def_submodule("plugin", "Plugin host API");

    plugin_host.def(
        "storage",
        []() -> std::string {
            const std::string plugin_key = PluginAuditManager::instance().current_plugin();
            if (plugin_key.empty())
                throw std::runtime_error("plugin.storage() must be called from a plugin callback");

            return PluginManager::instance().get_storage_dir(plugin_key);
        },
        "Return the installed folder of the current plugin.");
}

} // namespace Slic3r
