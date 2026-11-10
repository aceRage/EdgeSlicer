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

namespace host_bindings {

void register_slicing(py::module_& host)
{
    auto slicing = host.def_submodule(
        "slicing",
        "Slicing print-graph data model. Not available in this Edge build (Phase 6).");

    auto stub = [](const char* name) {
        return [name](py::args, py::kwargs) {
            raise_unavailable(name);
            return py::none();
        };
    };

    slicing.def("Print", stub("orca.host.slicing.Print"));
    slicing.def("PrintObject", stub("orca.host.slicing.PrintObject"));
    slicing.def("PrintRegion", stub("orca.host.slicing.PrintRegion"));
    slicing.def("Layer", stub("orca.host.slicing.Layer"));
    slicing.def("LayerRegion", stub("orca.host.slicing.LayerRegion"));
    slicing.def("SupportLayer", stub("orca.host.slicing.SupportLayer"));
    slicing.def("Surface", stub("orca.host.slicing.Surface"));
    slicing.def("ExtrusionPath", stub("orca.host.slicing.ExtrusionPath"));
}

} // namespace host_bindings

void register_unported_capability_types(py::module_& module)
{
    bind_unavailable_submodule(module, "pages", "orca.pages.");
    bind_unavailable_submodule(module, "printer_connection", "orca.printer_connection.");
    bind_unavailable_submodule(module, "resource_texture", "orca.resource_texture.");
}

} // namespace Slic3r
