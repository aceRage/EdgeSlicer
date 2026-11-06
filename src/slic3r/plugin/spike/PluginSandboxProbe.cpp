#include "PluginSandboxProbe.hpp"

#include <sstream>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__has_include)
#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define EDGE_SPIKE_HAS_LANDLOCK_HEADERS 1
#endif
#if __has_include(<seccomp.h>)
#define EDGE_SPIKE_HAS_SECCOMP_HEADERS 1
#endif
#endif
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif
#ifndef SYS_landlock_create_ruleset
#if defined(__x86_64__)
#define SYS_landlock_create_ruleset 444
#elif defined(__aarch64__)
#define SYS_landlock_create_ruleset 444
#endif
#endif
#endif

#if defined(_WIN32)
#define EDGE_SPIKE_HAS_JOB_OBJECT 1
#endif

namespace Slic3r {
namespace PluginSpike {

namespace {

#if defined(__linux__) && defined(SYS_landlock_create_ruleset)
int probe_landlock_abi()
{
    const long abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0)
        return 0;
    return static_cast<int>(abi);
}
#endif

} // namespace

PluginSandboxProbe probe_sandbox_apis()
{
    PluginSandboxProbe p;
    p.fail_closed_when_unavailable = true;

#if defined(_WIN32)
    p.os_name              = "windows";
    p.kernel_or_sdk        = "Win32 desktop (AppContainer from Windows 8)";
    p.job_object_headers   = true; // RemoteHub already creates a job
    p.app_container_headers = true; // userenv.h; not linked in this spike
#elif defined(__APPLE__)
    p.os_name                    = "macos";
    p.kernel_or_sdk              = "App Sandbox on a helper; sandbox-exec deprecated";
    p.sandbox_exec_deprecated    = true;
    p.app_sandbox_helper         = false;
#else
    p.os_name = "linux";
#if defined(__linux__)
    p.kernel_or_sdk = "Linux Landlock + seccomp; bwrap outside Flatpak only";
#ifdef EDGE_SPIKE_HAS_LANDLOCK_HEADERS
    p.landlock_headers = true;
#endif
#ifdef EDGE_SPIKE_HAS_SECCOMP_HEADERS
    p.seccomp_headers = true;
#endif
#ifdef SYS_landlock_create_ruleset
    p.landlock_abi     = probe_landlock_abi();
    p.landlock_runtime = p.landlock_abi > 0;
#endif
    p.bubblewrap_on_path = ::access("/usr/bin/bwrap", X_OK) == 0;
#endif
#endif
    return p;
}

std::string format_sandbox_probe(const PluginSandboxProbe &probe)
{
    std::ostringstream os;
    os << "os=" << probe.os_name << " note=" << probe.kernel_or_sdk << '\n'
       << "landlock_headers=" << (probe.landlock_headers ? "yes" : "no")
       << " landlock_abi=" << probe.landlock_abi
       << " landlock_runtime=" << (probe.landlock_runtime ? "yes" : "no") << '\n'
       << "seccomp_headers=" << (probe.seccomp_headers ? "yes" : "no")
       << " bwrap=" << (probe.bubblewrap_on_path ? "yes" : "no") << '\n'
       << "appcontainer=" << (probe.app_container_headers ? "yes" : "no")
       << " job_object=" << (probe.job_object_headers ? "yes" : "no") << '\n'
       << "sandbox_exec_deprecated=" << (probe.sandbox_exec_deprecated ? "yes" : "no")
       << " fail_closed=" << (probe.fail_closed_when_unavailable ? "yes" : "no") << '\n';
    return os.str();
}

} // namespace PluginSpike
} // namespace Slic3r
