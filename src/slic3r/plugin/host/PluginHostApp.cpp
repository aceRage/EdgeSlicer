#include "PluginHostBindings.hpp"

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

#include <stdexcept>
#include <string>
#include <wx/app.h>
#include <wx/string.h>
#include <wx/thread.h>

namespace py = pybind11;

namespace Slic3r {
namespace {

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

GUI::GUI_App& current_app()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    return GUI::wxGetApp();
}

void require_main_thread(const char* function_name)
{
    if (!wxIsMainThread())
        throw std::runtime_error(std::string(function_name) + "() must be called from the UI thread");
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
    GUI::GUI_App& app = current_app();
    require_main_thread("app_info");
    py::dict out;
    out["version"]  = Snapmaker_VERSION;
    out["build"]    = std::string();
    out["mode"]     = app.is_editor() ? "editor" : "gcode viewer";
    out["language"] = GUI::into_u8(app.current_language_code_safe());
    // App-config version is not exposed on Edge's GUI_App; leave empty rather than adding a
    // getter that would change the EDGE_PYTHON_PLUGINS=OFF binary.
    out["app_config_version"] = std::string();
    out["stealth_mode"]       = app.app_config != nullptr && app.app_config->get_stealth_mode();
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
    current_app();
    require_main_thread("selected_printer");
    // Device identity (name, serial, address, access code) is withheld. A richer printer-connection
    // surface is Phase 10; this accessor exists so scripts can tell whether a machine is selected.
    return py::none();
}

void export_3mf_copy(GUI::Plater&, const std::string&)
{
    throw edge_plugin_api_unavailable("orca.host.Plater.export_3mf_copy");
}

} // namespace

void host_bindings::register_app(py::module_& host)
{
    py::class_<GUI::Plater, std::unique_ptr<GUI::Plater, py::nodelete>>(host, "Plater")
        .def("model", static_cast<Model& (GUI::Plater::*)()>(&GUI::Plater::model), py::return_value_policy::reference_internal)
        .def("is_project_dirty", &GUI::Plater::is_project_dirty)
        .def("is_presets_dirty", &GUI::Plater::is_presets_dirty)
        .def("inside_snapshot_capture", &GUI::Plater::inside_snapshot_capture)
        .def("project_path", [](GUI::Plater& plater) {
            require_main_thread("project_path");
            return GUI::into_u8(plater.get_project_filename(".3mf"));
        })
        .def("export_3mf_copy", &export_3mf_copy, py::arg("path"),
             "Not available in this Edge build (needs the 3D view thumbnail path).");

    host.def("plater", &current_plater, py::return_value_policy::reference);
    host.def("model", []() -> Model& {
        return current_plater()->model();
    }, py::return_value_policy::reference);
    host.def("preset_bundle", &current_preset_bundle, py::return_value_policy::reference);
    host.def("app_info", &app_info,
             "The running app: version, build, mode, language, stealth_mode, orca_api_version. "
             "Cloud sign-in and network-plugin fields are empty on Edge.");
    host.def("gl_info", &gl_info,
             "OpenGL vendor, renderer, version, glsl_version and core_profile of the 3D view; None before it has been created.");
    host.def("selected_printer", &selected_printer,
             "Printer selected in the device list. Always None in this Edge build (Phase 10).");
}

} // namespace Slic3r
