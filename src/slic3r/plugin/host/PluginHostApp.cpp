#include "PluginHostBindings.hpp"
#include "PluginHostUiMarshal.hpp"

#include <pybind11/cast.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/AppConfig.hpp>
#include "common_func/common_func.hpp"
#include <slic3r/plugin/OrcaApiVersion.hpp>
#include <slic3r/GUI/GUI.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/OpenGLManager.hpp>
#include <slic3r/GUI/Plater.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <wx/app.h>
#include <wx/string.h>
#include <wx/thread.h>

namespace py = pybind11;

namespace Slic3r {
namespace {

Model*        g_live_model_for_testing  = nullptr;
PresetBundle* g_live_bundle_for_testing = nullptr;

GUI::Plater* current_plater()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    GUI::Plater* plater = GUI::wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("Plater is not available");

    return plater;
}

PresetBundle* current_preset_bundle()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    PresetBundle* preset_bundle = GUI::wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        throw std::runtime_error("Preset bundle is not available");

    return preset_bundle;
}

struct AppInfoSnapshot
{
    std::string version;
    std::string mode;
    std::string language;
    bool        stealth_mode = false;
};

AppInfoSnapshot snapshot_app_info()
{
    return plugin_host::run_on_ui_blocking([] {
        GUI::GUI_App&   app = GUI::wxGetApp();
        AppInfoSnapshot out;
        out.version  = Snapmaker_VERSION;
        out.mode     = app.is_editor() ? "editor" : "gcode viewer";
        out.language = GUI::into_u8(app.current_language_code_safe());
        out.stealth_mode = app.app_config != nullptr && app.app_config->get_stealth_mode();
        return out;
    });
}

py::object gl_info()
{
    // Do not call OpenGLManager::GLInfo getters here: they lazily detect() and would cache "N/A"
    // for the session if no context exists. Without a live plater the 3D view is not up.
    if (wxTheApp == nullptr)
        return py::none();
    GUI::Plater* plater = GUI::wxGetApp().plater();
    if (plater == nullptr)
        return py::none();

    const GUI::OpenGLManager::GLInfo& info = GUI::OpenGLManager::get_gl_info();
    if (info.get_version().empty() || info.get_version() == "N/A")
        return py::none();
    py::dict out;
    out["vendor"]       = info.get_vendor();
    out["renderer"]     = info.get_renderer();
    out["version"]      = info.get_version();
    out["glsl_version"] = info.get_glsl_version();
    out["core_profile"] = info.is_core_profile();
    return std::move(out);
}

py::dict app_info()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    const AppInfoSnapshot snap = snapshot_app_info();
    py::dict              out;
    out["version"]  = snap.version;
    out["build"]    = std::string();
    out["mode"]     = snap.mode;
    out["language"] = snap.language;
    // App-config version is not exposed on Edge's GUI_App; leave empty rather than adding a
    // getter that would change the EDGE_PYTHON_PLUGINS=OFF binary.
    out["app_config_version"] = std::string();
    out["stealth_mode"]       = snap.stealth_mode;
    // Orca Cloud / Bambu account and the Bambu network plug-in are not part of this Edge build.
    out["is_signed_in"]           = false;
    out["is_bambu_signed_in"]     = false;
    out["network_plugin_version"] = std::string();
    out["network_plugin_error"]   = std::string();
    out["orca_api_version"]       = k_orca_api_version;
    return out;
}

py::object selected_printer()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    plugin_host::run_on_ui_blocking([] {
        (void) GUI::wxGetApp();
    });
    // Device identity (name, serial, address, access code) is withheld. A richer printer-connection
    // surface is Phase 10; this accessor exists so scripts can tell whether a machine is selected.
    return py::none();
}

std::unique_ptr<Model> snapshot_model()
{
    if (g_live_model_for_testing)
        return std::make_unique<Model>(*g_live_model_for_testing);
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    return plugin_host::run_on_ui_blocking([] {
        return std::make_unique<Model>(current_plater()->model());
    });
}

std::unique_ptr<PresetBundle> snapshot_preset_bundle()
{
    if (g_live_bundle_for_testing)
        return std::make_unique<PresetBundle>(*g_live_bundle_for_testing);
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    return plugin_host::run_on_ui_blocking([] {
        return std::make_unique<PresetBundle>(*current_preset_bundle());
    });
}

void export_3mf_copy(GUI::Plater&, const std::string&)
{
    throw edge_plugin_api_unavailable("orca.host.Plater.export_3mf_copy");
}

} // namespace

void host_bindings::set_live_model_for_testing(Model* model) { g_live_model_for_testing = model; }

void host_bindings::set_live_preset_bundle_for_testing(PresetBundle* bundle)
{
    g_live_bundle_for_testing = bundle;
}

void host_bindings::register_app(py::module_& host)
{
    py::class_<GUI::Plater, std::unique_ptr<GUI::Plater, py::nodelete>>(host, "Plater")
        .def("model", [](GUI::Plater& plater) {
            return plugin_host::run_on_ui_blocking([&] {
                return std::make_unique<Model>(plater.model());
            });
        })
        .def("is_project_dirty", &GUI::Plater::is_project_dirty)
        .def("is_presets_dirty", &GUI::Plater::is_presets_dirty)
        .def("inside_snapshot_capture", &GUI::Plater::inside_snapshot_capture)
        .def("project_path", [](GUI::Plater& plater) {
            return plugin_host::run_on_ui_blocking([&] {
                return GUI::into_u8(plater.get_project_filename(".3mf"));
            });
        })
        .def("export_3mf_copy", &export_3mf_copy, py::arg("path"),
             "Not available in this Edge build (needs the 3D view thumbnail path).");

    host.def("plater", &current_plater, py::return_value_policy::reference);
    host.def("model", &snapshot_model);
    host.def("preset_bundle", &snapshot_preset_bundle);
    host.def("app_info", &app_info,
             "The running app: version, build, mode, language, stealth_mode, orca_api_version. "
             "Cloud sign-in and network-plugin fields are empty on Edge.");
    host.def("gl_info", &gl_info,
             "OpenGL vendor, renderer, version, glsl_version and core_profile of the 3D view; None before it has been created.");
    host.def("selected_printer", &selected_printer,
             "Printer selected in the device list. Always None in this Edge build (Phase 10).");
}

} // namespace Slic3r
