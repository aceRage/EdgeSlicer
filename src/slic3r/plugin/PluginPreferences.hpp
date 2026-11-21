#pragma once

// Runtime switches for the Python plugin runtime in an EDGE_PYTHON_PLUGINS build. All default
// to off: a key missing from the app config reads as false, and nothing writes them until the
// user changes a preference, so an ON build behaves like an OFF build until the user opts in.
//
//   enable_python_plugins                D10. Off: the plugin manager is never initialised, no
//                                        plugin folder is scanned, nothing auto-loads, Python is
//                                        not started and the Home rail stays hidden.
//   python_plugins_in_process_steps      H7 / D4. Off: slicing-pipeline plugins run only at the
//                                        G-code post-process step, in the sandboxed helper. On:
//                                        the early slicing steps (Slice ... Skirt/brim) also run,
//                                        inside EdgeSlicer, unsandboxed, with a live Print, for
//                                        plugins signed by a shipped publisher key only (D2):
//                                        those are then imported in-process. Every other plugin
//                                        is imported and run in the helper (D3).
//   python_plugins_allow_unsigned        H7 / D2. Only consulted in a signed release build: off
//                                        means a plugin approved only by a local pin (not signed
//                                        by a shipped publisher key) does not run there.
//                                        Unsigned developer builds always allow local pins.

#include <string>

namespace Slic3r::plugin_prefs {

inline constexpr const char* k_enable_python_plugins   = "enable_python_plugins";
inline constexpr const char* k_in_process_steps        = "python_plugins_in_process_steps";
inline constexpr const char* k_allow_unsigned_plugins  = "python_plugins_allow_unsigned";

// Reads one of the keys above from the running app's config; false without an app or a config.
bool enabled(const char* key);

// Test seam: when set (>= 0) overrides every key's value for this process. -1 clears it.
void set_override_for_testing(const char* key, int value);

} // namespace Slic3r::plugin_prefs
