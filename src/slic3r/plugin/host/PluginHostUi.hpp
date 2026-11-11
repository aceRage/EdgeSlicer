#pragma once

#include <pybind11/pybind11.h>

#include <functional>
#include <string>

namespace Slic3r {

// Binds the `orca.host.ui` submodule: native message boxes, progress dialogs,
// HTML windows, and plater notifications. All calls run on the main/UI thread
// (marshaled from the plugin worker thread) and the host owns every window.
//
// create_dock_panel is accepted as an API name but raises a clear "not available
// in this Edge build" error after validating the dock argument (no DockPanel on Edge).
class PluginHostUi
{
public:
    static void RegisterBindings(pybind11::module_& host);

    static void run_on_ui_thread(std::function<void()> fn);

    // Lifecycle hook: close and tear down every UI window owned by a plugin. PluginManager
    // invokes this after plugin teardown and also for bulk unload during application shutdown.
    static void close_windows_for_plugin(const std::string& plugin_key);
};

} // namespace Slic3r
