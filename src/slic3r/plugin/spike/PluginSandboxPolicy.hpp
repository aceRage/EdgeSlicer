#ifndef slic3r_plugin_spike_PluginSandboxPolicy_hpp_
#define slic3r_plugin_spike_PluginSandboxPolicy_hpp_

namespace Slic3r {
namespace PluginSpike {

// What the OS sandbox helper would apply to the edge-plugin-host child. Encoded here so phase 7–8
// implement against a frozen policy rather than rediscovering it.
enum class PluginWorkload {
    Script,
    PagesPython,
    ResourceTexture,
    GCodePostProcess,   // slicing-pipeline at psGCodePostProcess
    EarlySlicingPipeline, // live Print* / PrintObject* steps
    PrinterConnection,
};

enum class SandboxPlacement {
    OutOfProcessSandboxed, // scripts, pages Python, textures, G-code post-process
    InProcessLabeled,      // early pipeline: accepted exception, card must say so
    Deferred,              // printer-connection: phase 10, not the first sandbox cut
};

enum class SandboxUnavailableAction {
    DontRun,     // default button after phase 8
    RunInProcess // only if the user explicitly chooses it; remembered on that plugin
};

SandboxPlacement sandbox_placement(PluginWorkload workload);

// If the OS sandbox cannot be applied to a workload that should be sandboxed, do not silently
// fall back. Prompt per plugin; default is Don't run.
SandboxUnavailableAction default_sandbox_unavailable_action();

// Early slicing-pipeline steps may run unsandboxed in-process without that extra prompt.
bool early_pipeline_requires_sandbox_unavailable_prompt();

bool workload_must_prompt_when_sandbox_unavailable(PluginWorkload workload);

} // namespace PluginSpike
} // namespace Slic3r

#endif
