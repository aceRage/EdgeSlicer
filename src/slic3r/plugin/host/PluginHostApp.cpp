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
#include <mutex>
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

struct GlInfoSnapshot
{
    bool        available = false;
    std::string vendor;
    std::string renderer;
    std::string version;
    std::string glsl_version;
    bool        core_profile = false;
};

py::object gl_info()
{
    // Do not call OpenGLManager::GLInfo getters off the UI thread, and do not call them at all
    // when there is no plater: they lazily detect() and would cache "N/A" for the session.
    if (wxTheApp == nullptr)
        return py::none();
    const GlInfoSnapshot snap = plugin_host::run_on_ui_blocking([] {
        GlInfoSnapshot out;
        if (GUI::wxGetApp().plater() == nullptr)
            return out;
        const GUI::OpenGLManager::GLInfo& info = GUI::OpenGLManager::get_gl_info();
        if (info.get_version().empty() || info.get_version() == "N/A")
            return out;
        out.available    = true;
        out.vendor       = info.get_vendor();
        out.renderer     = info.get_renderer();
        out.version      = info.get_version();
        out.glsl_version = info.get_glsl_version();
        out.core_profile = info.is_core_profile();
        return out;
    });
    if (!snap.available)
        return py::none();
    py::dict out;
    out["vendor"]       = snap.vendor;
    out["renderer"]     = snap.renderer;
    out["version"]      = snap.version;
    out["glsl_version"] = snap.glsl_version;
    out["core_profile"] = snap.core_profile;
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

std::string preset_bundle_fingerprint(const PresetBundle& bundle)
{
    std::string fp;
    fp += bundle.prints.get_selected_preset_name();
    fp += '\0';
    fp += bundle.printers.get_selected_preset_name();
    fp += '\0';
    for (const std::string& name : bundle.filament_presets) {
        fp += name;
        fp += '\0';
    }
    fp += bundle.prints.get_edited_preset().is_dirty ? 'D' : 'c';
    fp += bundle.printers.get_edited_preset().is_dirty ? 'D' : 'c';
    fp += bundle.filaments.get_edited_preset().is_dirty ? 'D' : 'c';
    return fp;
}

struct PresetBundleCache
{
    std::mutex                    mutex;
    std::shared_ptr<PresetBundle> bundle;
    std::string                   fingerprint;
};

PresetBundleCache g_preset_bundle_cache;

std::shared_ptr<PresetBundle> snapshot_preset_bundle()
{
    if (g_live_bundle_for_testing)
        return std::make_shared<PresetBundle>(*g_live_bundle_for_testing);
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");
    return plugin_host::run_on_ui_blocking([] {
        PresetBundle* live = current_preset_bundle();
        const std::string fp = preset_bundle_fingerprint(*live);
        std::lock_guard<std::mutex> lock(g_preset_bundle_cache.mutex);
        if (!g_preset_bundle_cache.bundle || g_preset_bundle_cache.fingerprint != fp) {
            g_preset_bundle_cache.bundle      = std::make_shared<PresetBundle>(*live);
            g_preset_bundle_cache.fingerprint = fp;
        }
        return g_preset_bundle_cache.bundle;
    });
}

// Token for orca.host.plater(): methods marshal to the UI thread and never store a live Plater*.
struct PlaterHandle
{};

void export_3mf_copy(PlaterHandle&, const std::string&)
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
    py::class_<PlaterHandle>(host, "Plater")
        .def("model", [](const PlaterHandle&) { return snapshot_model(); })
        .def("is_project_dirty", [](const PlaterHandle&) {
            return plugin_host::run_on_ui_blocking([] { return current_plater()->is_project_dirty(); });
        })
        .def("is_presets_dirty", [](const PlaterHandle&) {
            return plugin_host::run_on_ui_blocking([] { return current_plater()->is_presets_dirty(); });
        })
        .def("inside_snapshot_capture", [](const PlaterHandle&) {
            return plugin_host::run_on_ui_blocking([] { return current_plater()->inside_snapshot_capture(); });
        })
        .def("project_path", [](const PlaterHandle&) {
            return plugin_host::run_on_ui_blocking([] {
                return GUI::into_u8(current_plater()->get_project_filename(".3mf"));
            });
        })
        .def("export_3mf_copy", &export_3mf_copy, py::arg("path"),
             "Not available in this Edge build (needs the 3D view thumbnail path).");

    host.def("plater", [] {
        if (wxTheApp == nullptr)
            throw std::runtime_error("OrcaSlicer application is not initialized");
        plugin_host::run_on_ui_blocking([] { (void) current_plater(); });
        return PlaterHandle{};
    });
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
