#pragma once

#include <pybind11/pybind11.h>

namespace Slic3r {
class Model;
class PresetBundle;
}

// Internal to plugin/host/: the per-domain registrars of the `orca.host`
// surface, one per translation unit, called by PluginHost::RegisterBindings.
namespace Slic3r::host_bindings {

void register_geometry(pybind11::module_& host); // PluginHostGeometry.cpp
void register_mesh(pybind11::module_& host);     // PluginHostMesh.cpp
void register_presets(pybind11::module_& host);  // PluginHostPresets.cpp
void register_model(pybind11::module_& host);    // PluginHostModel.cpp
void register_app(pybind11::module_& host);      // PluginHostApp.cpp
void register_slicing(pybind11::module_& host);  // PluginHostSlicing.cpp
void register_plugin(pybind11::module_& host);   // PluginHost.cpp

// Test seam: host.model() / host.preset_bundle() copy this pointer instead of the live Plater.
void set_live_model_for_testing(Model* model);
void set_live_preset_bundle_for_testing(PresetBundle* bundle);

} // namespace Slic3r::host_bindings

namespace Slic3r {
void register_unported_capability_types(pybind11::module_& module);
}
