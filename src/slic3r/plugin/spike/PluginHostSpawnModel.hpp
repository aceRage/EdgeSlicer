#ifndef slic3r_plugin_spike_PluginHostSpawnModel_hpp_
#define slic3r_plugin_spike_PluginHostSpawnModel_hpp_

#include <string>

namespace Slic3r {
namespace PluginSpike {

// How Edge would spawn edge-plugin-host. This is a process-model map against existing spawn
// code in the tree, not a shipping helper. Phase 7 implements it; phase 8 wraps the child in
// the OS sandbox.
inline constexpr const char *k_plugin_host_helper_name = "edge-plugin-host";

enum class PluginHostSpawnBackend {
    WindowsJobObject,       // RemoteHub.cpp: CreateProcess + CreateJobObject
    WindowsAppContainer,    // phase 8: AppContainer around the job
    PosixForkExec,          // HubPlatform::spawn_child_posix (fork + exec, own process group)
    MacSignedHelper,        // phase 8: signed App Sandbox helper / XPC, not sandbox-exec
    LinuxLandlockSeccomp,   // phase 8: Landlock + seccomp in the child
    LinuxBubblewrap,        // native packages only; never nested inside Flatpak
};

enum class PluginHostIpc {
    LocalSocketOrPipe, // child never sees raw Print*, UltraNet paths, or secrets
};

struct PluginHostSpawnPlan {
    const char *           helper_name          = k_plugin_host_helper_name;
    PluginHostSpawnBackend backend              = PluginHostSpawnBackend::PosixForkExec;
    PluginHostIpc          ipc                  = PluginHostIpc::LocalSocketOrPipe;
    bool                   kill_on_parent_exit  = true;
    bool                   allow_breakaway      = false; // JOB_OBJECT_LIMIT_BREAKAWAY_OK must stay off
    int                    max_active_processes = 1;     // JOB_OBJECT_LIMIT_ACTIVE_PROCESS
    bool                   create_suspended     = true;  // assign to job, then resume (Windows)
    bool                   use_wx_execute       = false; // wxExecute is unreliable on macOS
};

PluginHostSpawnPlan spawn_plan_for_current_os();

// Human-readable existing-code citation for the plan's backend.
std::string spawn_plan_source_citation(const PluginHostSpawnPlan &plan);

} // namespace PluginSpike
} // namespace Slic3r

#endif
