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
    // Parent-chosen path passed as --host-log. Survives close_host so the caller can
    // still attach the tail after a crash.
    std::string host_log_path;

    bool valid() const { return channel.valid() && pid > 0; }
};

boost::filesystem::path helper_path();
bool                    helper_exists();

// EDGE_PLUGIN_HOST / EDGE_PLUGIN_HOST_IN_PROCESS are ignored unless this is true.
// Production never enables it; slic3rutils tests may. Hidden so a release DSO does not
// export a switch for the env gate (Russ accepted the runtime-only gate).
#if defined(__GNUC__) || defined(__clang__)
#define EDGE_PLUGIN_TEST_HOOK __attribute__((visibility("hidden")))
#else
#define EDGE_PLUGIN_TEST_HOOK
#endif
EDGE_PLUGIN_TEST_HOOK void set_allow_helper_env_overrides_for_testing(bool allow);
bool                       helper_env_overrides_allowed();

// Spawn edge-plugin-host. POSIX: socketpair + fork/exec. POSIX_SPAWN_CLOEXEC_DEFAULT
// is Apple-only (glibc has no such flag), so Linux never takes a posix_spawn path.
// Linux child installs PR_SET_PDEATHSIG. Every POSIX spawn also keeps a parent-pipe
// EOF watchdog so the helper dies if the spawning thread exits while Edge lives.
// The child is placed in its own process group; kill_host signals the group so
// grandchildren die too. setsid and setpgid are denied in the Linux seccomp filter
// after the sandbox applies (the pre-exec setpgid(0,0) still runs). Linux does not
// set PR_SET_CHILD_SUBREAPER (no reap path, and orphans would become Edge zombies).
// Windows: named pipe + Job object (KILL_ON_JOB_CLOSE, ACTIVE_PROCESS=1, no breakaway),
// create suspended, assign, resume. AppContainer ACL grants live until close_host.
// The pipe client pid must match the child. The helper env is an allow-list, not
// CreateProcessW lpEnvironment=nullptr. Exceeding the 1 GiB job memory cap is reported
// as a distinct error (not a generic closed pipe).
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
// True when the child was killed for exceeding JOB_OBJECT_LIMIT_PROCESS_MEMORY (1 GiB).
bool host_exceeded_job_memory_limit(const SpawnedHost& host);
// On closed-pipe / early-exit, wait briefly, record GetExitCodeProcess / waitpid, and
// append the exit code plus the last ~40 lines of host_log_path (and POSIX stderr).
void enrich_host_error(SpawnedHost& host, std::string& error);

} // namespace Slic3r::plugin_ipc
