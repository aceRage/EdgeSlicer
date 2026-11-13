#include "PluginHostBindings.hpp"
#include "slic3r/plugin/OrcaApiVersion.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <string>

namespace py = pybind11;

namespace Slic3r {
namespace {

[[noreturn]] void raise_unavailable(const std::string& name)
{
    throw edge_plugin_api_unavailable(name);
}

void bind_unavailable_submodule(py::module_& parent, const char* name, const char* prefix)
{
    auto mod = parent.def_submodule(name, "Not available in this Edge build");
    mod.def(
        "__getattr__",
        [prefix](const std::string& attr) {
            raise_unavailable(std::string(prefix) + attr);
            return py::none();
        });
}

} // namespace

void register_unported_capability_types(py::module_& module)
{
    bind_unavailable_submodule(module, "pages", "orca.pages.");
    bind_unavailable_submodule(module, "printer_connection", "orca.printer_connection.");
    bind_unavailable_submodule(module, "resource_texture", "orca.resource_texture.");
}

} // namespace Slic3r
