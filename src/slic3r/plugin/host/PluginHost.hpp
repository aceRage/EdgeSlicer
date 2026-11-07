#pragma once

#include <pybind11/pybind11.h>

namespace Slic3r {

// Entry point of the `orca.host` Python API surface. Phase 1 registers only
// `orca.host.plugin.storage`; geometry/mesh/presets/model/app/slicing/UI
// registrars land in later phases.
class PluginHost
{
public:
    static void RegisterBindings(pybind11::module_& module);
};

} // namespace Slic3r
