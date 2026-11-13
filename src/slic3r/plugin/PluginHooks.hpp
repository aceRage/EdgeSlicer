#pragma once

// The plugin layer's installers for the hooks libslic3r exposes. Phase 6 installs the
// lifecycle-event hook, the capability-name resolver, the slicing-pipeline hook, and the
// G-code post-process plugin seam.

namespace Slic3r::plugin_hooks {

// Install every hook. Called once from PluginManager::initialize().
void install();

// Reset every hook to null so none can enter Python after the interpreter
// finalizes. The lifecycle-event hook drains callbacks already in progress
// before returning. Other hooks retain their existing caller-side shutdown
// requirements.
void uninstall();

} // namespace Slic3r::plugin_hooks
