#pragma once

#include "PluginIpcChannel.hpp"

#include <boost/filesystem/path.hpp>

#include <cstdint>
#include <string>

namespace Slic3r::plugin_ipc {

struct SpawnedHost
{
    Channel     channel;
    std::int64_t pid = 0;
#ifdef _WIN32
    void* process = nullptr; // HANDLE, owned
    void* job     = nullptr; // HANDLE, owned
#endif
#ifndef _WIN32
#ifdef __APPLE__
    int watchdog_write_fd = -1;
#endif
#endif

    bool valid() const { return channel.valid() && pid > 0; }
};

boost::filesystem::path helper_path();
bool                    helper_exists();

// Spawn edge-plugin-host. POSIX: socketpair + fork/exec, keep the IPC fd, own process group.
// Linux child installs PR_SET_PDEATHSIG. macOS keeps an extra parent-pipe for EOF watchdog.
// Windows: named pipe + Job object (KILL_ON_JOB_CLOSE, ACTIVE_PROCESS=1, no breakaway),
// create suspended, assign, resume.
bool spawn_host(SpawnedHost& out, std::string& error);

void kill_host(SpawnedHost& host);
void close_host(SpawnedHost& host);

} // namespace Slic3r::plugin_ipc
