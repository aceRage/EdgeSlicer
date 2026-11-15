#pragma once

#include "PluginIpcChannel.hpp"
#include "PluginSandbox.hpp"

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <string>

namespace Slic3r::plugin_ipc {

struct SpawnedHost
{
    Channel      channel;
    std::int64_t pid = 0;
#ifdef _WIN32
    void* process = nullptr; // HANDLE, owned
    void* job     = nullptr; // HANDLE, owned
#endif
#ifndef _WIN32
    int watchdog_write_fd = -1;
    int stderr_read_fd    = -1;
#endif
    int  last_wait_status = 0;
    bool wait_reaped      = false;

    bool valid() const { return channel.valid() && pid > 0; }
};

boost::filesystem::path helper_path();
bool                    helper_exists();

// EDGE_PLUGIN_HOST / EDGE_PLUGIN_HOST_IN_PROCESS are ignored unless this is true.
// Production never enables it; slic3rutils tests may.
void set_allow_helper_env_overrides_for_testing(bool allow);
bool helper_env_overrides_allowed();

// Spawn edge-plugin-host. POSIX: socketpair + fork/exec. POSIX_SPAWN_CLOEXEC_DEFAULT
// is Apple-only (glibc has no such flag), so Linux never takes a posix_spawn path.
// Linux child installs PR_SET_PDEATHSIG. Every POSIX spawn also keeps a parent-pipe
// EOF watchdog so the helper dies if the spawning thread exits while Edge lives.
// The child is placed in its own process group; kill_host signals the group so
// grandchildren die too. Linux also sets PR_SET_CHILD_SUBREAPER on Edge.
// Windows: named pipe + Job object (KILL_ON_JOB_CLOSE, ACTIVE_PROCESS=1, no breakaway),
// create suspended, assign, resume. Phase 8 wraps the job in an AppContainer when
// `spec` is supplied. The pipe client pid must match the child.
// Nested bubblewrap is never used (Flatpak already is bwrap).
bool spawn_host(SpawnedHost& out, std::string& error);
bool spawn_host(SpawnedHost& out, const SandboxSpec& spec, std::string& error);

void kill_host(SpawnedHost& host);
void kill_host_pid(std::int64_t pid);
#ifdef _WIN32
void kill_host_handle(void* process);
#endif
void close_host(SpawnedHost& host);

// Drain the child's captured stderr (POSIX spawn redirects fd 2 to a pipe). Empty on Windows.
std::string drain_host_stderr(SpawnedHost& host);
// Human-readable waitpid / GetExitCodeProcess result after close_host.
std::string host_exit_summary(const SpawnedHost& host);

} // namespace Slic3r::plugin_ipc
