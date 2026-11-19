// H7 / D-items: the restrictive defaults chosen for the landing (see the PR body).
//  * The runtime itself is off until the user enables it (D10).
//  * The early slicing steps, which run plugin code inside EdgeSlicer, are off by default (D4):
//    without the preference no slicing-pipeline hook is installed in Print at all.

#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <libslic3r/Print.hpp>
#include <slic3r/plugin/PluginHooks.hpp>
#include <slic3r/plugin/PluginPreferences.hpp>

using namespace Slic3r;

TEST_CASE("D10: Python plugins are off unless the user enables them", "[PluginLanding][H7][D10]")
{
    // No app, no config: every switch reads off. (The test run turns the early steps on for the
    // other plugin tests through the override; clear it for this check.)
    plugin_prefs::set_override_for_testing(plugin_prefs::k_in_process_steps, -1);
    CHECK_FALSE(plugin_prefs::enabled(plugin_prefs::k_enable_python_plugins));
    CHECK_FALSE(plugin_prefs::enabled(plugin_prefs::k_in_process_steps));
    CHECK_FALSE(plugin_prefs::enabled(plugin_prefs::k_allow_unsigned_plugins));
    plugin_prefs::set_override_for_testing(plugin_prefs::k_in_process_steps, 1);
    CHECK(plugin_prefs::enabled(plugin_prefs::k_in_process_steps));
}

TEST_CASE("H7: without the preference the early slicing-step hook is not installed", "[PluginLanding][H7][slicing_pipeline]")
{
    // Leave the hooks as they were: installed only if the plugin manager had installed them.
    struct Restore
    {
        bool were_installed = static_cast<bool>(Print::slicing_pipeline_hook_fn());
        ~Restore()
        {
            plugin_prefs::set_override_for_testing(plugin_prefs::k_in_process_steps, 1);
            plugin_hooks::uninstall();
            if (were_installed)
                plugin_hooks::install();
        }
    } restore;

    plugin_prefs::set_override_for_testing(plugin_prefs::k_in_process_steps, 0);
    plugin_hooks::uninstall();
    plugin_hooks::install();
    CHECK_FALSE(static_cast<bool>(Print::slicing_pipeline_hook_fn()));

    plugin_prefs::set_override_for_testing(plugin_prefs::k_in_process_steps, 1);
    plugin_hooks::uninstall();
    plugin_hooks::install();
    CHECK(static_cast<bool>(Print::slicing_pipeline_hook_fn()));
}
