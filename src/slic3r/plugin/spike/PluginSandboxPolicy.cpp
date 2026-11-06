#include "PluginSandboxPolicy.hpp"

namespace Slic3r {
namespace PluginSpike {

SandboxPlacement sandbox_placement(PluginWorkload workload)
{
    switch (workload) {
    case PluginWorkload::Script:
    case PluginWorkload::PagesPython:
    case PluginWorkload::ResourceTexture:
    case PluginWorkload::GCodePostProcess: return SandboxPlacement::OutOfProcessSandboxed;
    case PluginWorkload::EarlySlicingPipeline: return SandboxPlacement::InProcessLabeled;
    case PluginWorkload::PrinterConnection: return SandboxPlacement::Deferred;
    }
    return SandboxPlacement::InProcessLabeled;
}

SandboxUnavailableAction default_sandbox_unavailable_action() { return SandboxUnavailableAction::DontRun; }

bool early_pipeline_requires_sandbox_unavailable_prompt() { return false; }

bool workload_must_prompt_when_sandbox_unavailable(PluginWorkload workload)
{
    return sandbox_placement(workload) == SandboxPlacement::OutOfProcessSandboxed;
}

} // namespace PluginSpike
} // namespace Slic3r
