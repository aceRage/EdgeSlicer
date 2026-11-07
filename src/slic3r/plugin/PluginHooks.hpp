#pragma once

// The plugin layer's installers for the hooks libslic3r exposes. Phase 1
// installs only the lifecycle-event hook. Capability resolver and slicing
// pipeline seams stay unwired so Print.hpp / ConfigBase G-code paths are
// unchanged (Phase 0 blocker B3).

namespace Slic3r::plugin_hooks {

// Install every hook. Called once from PluginManager::initialize().
void install();

// Reset every hook to null so none can enter Python after the interpreter
// finalizes. The lifecycle-event hook drains callbacks already in progress
// before returning. Other hooks retain their existing caller-side shutdown
// requirements.
void uninstall();

} // namespace Slic3r::plugin_hooks
