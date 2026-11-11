#include "PluginHooks.hpp"

#include "PluginManager.hpp"

#include "libslic3r/LifecycleEvents.hpp"

namespace Slic3r::plugin_hooks {
namespace {

// Global libslic3r-side seam (Slic3r::fire_lifecycle_event): broadcasts to every loaded,
// enabled capability. Phase 1 installs only this hook. Capability resolver and slicing
// pipeline hooks stay unwired so Print.hpp / ConfigBase G-code paths are unchanged.
void install_lifecycle_event_hook()
{
    set_lifecycle_hook_fn([](LifecycleEvent event, const LifecycleEventContext& ctx) {
        PluginManager::instance().dispatch_lifecycle_event(event, ctx);
    });
}

} // namespace

void install()
{
    install_lifecycle_event_hook();
}

void uninstall()
{
    set_lifecycle_hook_fn(nullptr);
}

} // namespace Slic3r::plugin_hooks
