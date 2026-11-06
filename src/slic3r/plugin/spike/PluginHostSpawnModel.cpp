#include "PluginHostSpawnModel.hpp"

namespace Slic3r {
namespace PluginSpike {

PluginHostSpawnPlan spawn_plan_for_current_os()
{
    PluginHostSpawnPlan plan;
#if defined(_WIN32)
    plan.backend              = PluginHostSpawnBackend::WindowsJobObject;
    plan.kill_on_parent_exit  = true;
    plan.allow_breakaway      = false;
    plan.max_active_processes = 1;
    plan.create_suspended     = true;
#elif defined(__APPLE__)
    // v1 (phase 7) can use posix_spawn / HubPlatform::spawn_child_posix. The shipping sandbox is a
    // signed helper with App Sandbox entitlements, because sandbox-exec is deprecated and a child
    // of an unsandboxed Edge.app does not inherit a useful Seatbelt profile.
    plan.backend             = PluginHostSpawnBackend::PosixForkExec;
    plan.create_suspended    = false;
    plan.kill_on_parent_exit = true;
#else
    plan.backend             = PluginHostSpawnBackend::PosixForkExec;
    plan.create_suspended    = false;
    plan.kill_on_parent_exit = true;
#endif
    plan.use_wx_execute = false;
    return plan;
}

std::string spawn_plan_source_citation(const PluginHostSpawnPlan &plan)
{
    switch (plan.backend) {
    case PluginHostSpawnBackend::WindowsJobObject:
    case PluginHostSpawnBackend::WindowsAppContainer:
        return "src/slic3r/GUI/RemoteHub.cpp CreateJobObjectW + JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE";
    case PluginHostSpawnBackend::PosixForkExec:
        return "src/slic3r/Utils/HubPlatform.hpp spawn_child_posix (fork+exec, setpgid)";
    case PluginHostSpawnBackend::MacSignedHelper:
        return "phase 8 signed helper / XPC; do not ship sandbox-exec";
    case PluginHostSpawnBackend::LinuxLandlockSeccomp:
        return "phase 8 Landlock + seccomp in the child; Flatpak does not nest bwrap";
    case PluginHostSpawnBackend::LinuxBubblewrap:
        return "native Linux packages only; scripts/flatpak must not nest bwrap";
    }
    return "unknown";
}

} // namespace PluginSpike
} // namespace Slic3r
