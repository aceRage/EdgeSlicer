#ifndef slic3r_plugin_spike_PluginSandboxProbe_hpp_
#define slic3r_plugin_spike_PluginSandboxProbe_hpp_

#include <string>

namespace Slic3r {
namespace PluginSpike {

// Compile-time and (on Linux) runtime view of OS sandbox APIs. Phase 0 records what this host
// can do; phase 8 is where a missing API becomes Don't run rather than a silent in-process fallback.
struct PluginSandboxProbe {
    std::string os_name;
    std::string kernel_or_sdk;

    bool landlock_headers  = false;
    int  landlock_abi      = 0; // 0 = unavailable
    bool landlock_runtime  = false;

    bool seccomp_headers   = false;
    bool bubblewrap_on_path = false;

    bool app_container_headers = false; // userenv.h CreateAppContainerProfile
    bool job_object_headers    = false; // already used by RemoteHub

    bool sandbox_exec_deprecated = true; // macOS: do not ship on this
    bool app_sandbox_helper      = false; // phase 8 helper entitlements, not probed here

    bool fail_closed_when_unavailable = true;
};

PluginSandboxProbe probe_sandbox_apis();
std::string        format_sandbox_probe(const PluginSandboxProbe &probe);

} // namespace PluginSpike
} // namespace Slic3r

#endif
