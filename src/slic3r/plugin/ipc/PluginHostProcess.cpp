#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "PluginHostProcess.hpp"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <spawn.h>
#include <sys/prctl.h>
#endif
#include <cerrno>
#include <poll.h>
#endif

#ifndef _WIN32
extern char** environ;
#endif

namespace Slic3r::plugin_ipc {
namespace {

std::atomic<bool> g_allow_env_overrides{false};

#ifdef _WIN32
std::wstring quote_arg(const std::wstring& arg)
{
    if (arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"')
            out += L"\\\"";
        else
            out += c;
    }
    out += L'"';
    return out;
}
#else

void set_cloexec(int fd)
{
    if (fd < 0)
        return;
    const int fl = ::fcntl(fd, F_GETFD);
    if (fl >= 0)
        ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

void set_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void) fd;
#endif
}

int socketpair_cloexec(int sv[2])
{
#ifdef SOCK_CLOEXEC
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0)
        return 0;
#endif
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -1;
    set_cloexec(sv[0]);
    set_cloexec(sv[1]);
    return 0;
}

int pipe_cloexec(int p[2])
{
#ifdef __linux__
    if (::pipe2(p, O_CLOEXEC) == 0)
        return 0;
#endif
    if (::pipe(p) != 0)
        return -1;
    set_cloexec(p[0]);
    set_cloexec(p[1]);
    return 0;
}

#endif

} // namespace

void set_allow_helper_env_overrides_for_testing(bool allow)
{
    g_allow_env_overrides.store(allow, std::memory_order_release);
}

bool helper_env_overrides_allowed() { return g_allow_env_overrides.load(std::memory_order_acquire); }

boost::filesystem::path helper_path()
{
    if (g_allow_env_overrides.load(std::memory_order_acquire)) {
        if (const char* env = boost::nowide::getenv("EDGE_PLUGIN_HOST")) {
            if (env[0] != '\0')
                return boost::filesystem::path(env);
        }
    }
    boost::filesystem::path dir = boost::dll::program_location().parent_path();
#ifdef _WIN32
    return dir / (std::string(k_helper_name) + ".exe");
#else
    return dir / k_helper_name;
#endif
}

bool helper_exists()
{
    boost::system::error_code ec;
    const auto                path = helper_path();
    return boost::filesystem::exists(path, ec) && boost::filesystem::is_regular_file(path, ec);
}

#ifdef _WIN32

bool spawn_host(SpawnedHost& out, std::string& error)
{
    close_host(out);
    const boost::filesystem::path helper = helper_path();
    if (!helper_exists()) {
        error = "edge-plugin-host not found at " + helper.string();
        return false;
    }

    wchar_t pipe_name[128];
    static LONG counter = 0;
    const LONG  n       = ::InterlockedIncrement(&counter);
    swprintf(pipe_name, 128, L"\\\\.\\pipe\\edge-plugin-host-%lu-%ld",
             static_cast<unsigned long>(::GetCurrentProcessId()), static_cast<long>(n));

    HANDLE pipe = ::CreateNamedPipeW(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 64 * 1024,
                                     64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        error = "CreateNamedPipe failed";
        return false;
    }

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        ::CloseHandle(pipe);
        error = "CreateJobObject failed";
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    jeli.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        ::CloseHandle(job);
        ::CloseHandle(pipe);
        error = "SetInformationJobObject failed";
        return false;
    }

    const std::wstring cmd = quote_arg(helper.wstring()) + L" --ipc-pipe " + quote_arg(pipe_name);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NO_WINDOW;
    const BOOL  ok    = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
    if (!ok) {
        ::CloseHandle(job);
        ::CloseHandle(pipe);
        error = "CreateProcess failed for edge-plugin-host";
        return false;
    }
    if (!::AssignProcessToJobObject(job, pi.hProcess)) {
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(job);
        ::CloseHandle(pipe);
        error = "AssignProcessToJobObject failed";
        return false;
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL connected = ::ConnectNamedPipe(pipe, &ov);
    if (!connected) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_IO_PENDING) {
            if (::WaitForSingleObject(ov.hEvent, 15000) != WAIT_OBJECT_0) {
                ::CancelIoEx(pipe, &ov);
                ::CloseHandle(ov.hEvent);
                ::TerminateProcess(pi.hProcess, 1);
                ::CloseHandle(pi.hProcess);
                ::CloseHandle(job);
                ::CloseHandle(pipe);
                error = "ConnectNamedPipe timed out";
                return false;
            }
        } else if (err != ERROR_PIPE_CONNECTED) {
            ::CloseHandle(ov.hEvent);
            ::TerminateProcess(pi.hProcess, 1);
            ::CloseHandle(pi.hProcess);
            ::CloseHandle(job);
            ::CloseHandle(pipe);
            error = "ConnectNamedPipe failed";
            return false;
        }
    }
    if (ov.hEvent)
        ::CloseHandle(ov.hEvent);

    ULONG client_pid = 0;
    if (!::GetNamedPipeClientProcessId(pipe, &client_pid) || client_pid != pi.dwProcessId) {
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(job);
        ::CloseHandle(pipe);
        error = "named pipe client pid mismatch";
        return false;
    }

    out.channel = Channel::from_windows_handle(pipe);
    out.pid     = static_cast<std::int64_t>(pi.dwProcessId);
    out.process = pi.hProcess;
    out.job     = job;
    return true;
}

void kill_host(SpawnedHost& host)
{
    if (host.process)
        ::TerminateProcess(static_cast<HANDLE>(host.process), 1);
}

void kill_host_pid(std::int64_t pid)
{
    if (pid <= 0)
        return;
    HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (h) {
        ::TerminateProcess(h, 1);
        ::CloseHandle(h);
    }
}

void close_host(SpawnedHost& host)
{
    host.channel.close();
    if (host.process) {
        ::WaitForSingleObject(static_cast<HANDLE>(host.process), 2000);
        ::CloseHandle(static_cast<HANDLE>(host.process));
        host.process = nullptr;
    }
    if (host.job) {
        ::CloseHandle(static_cast<HANDLE>(host.job));
        host.job = nullptr;
    }
    host.pid = 0;
}

#else

namespace {

void close_watchdog(SpawnedHost& host)
{
    if (host.watchdog_write_fd >= 0) {
        ::close(host.watchdog_write_fd);
        host.watchdog_write_fd = -1;
    }
}

void close_fd(int& fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool spawn_host_fork(SpawnedHost& out,
                     const boost::filesystem::path& helper,
                     int sv[2],
                     int watchdog[2],
                     int errp[2],
                     long maxfd,
                     std::string& error)
{
    // argv lives in the parent so the child does not allocate after fork.
    const std::string helper_str = helper.string();
    char              fd_arg[]   = "3";
    char              wd_arg[]   = "4";
    char*             argv[]     = {const_cast<char*>(helper_str.c_str()),
                                    const_cast<char*>("--ipc-fd"),
                                    fd_arg,
                                    const_cast<char*>("--parent-watchdog-fd"),
                                    wd_arg,
                                    nullptr};

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
#ifdef __linux__
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() == 1)
            ::_exit(1);
#endif
        ::close(sv[0]);
        ::close(errp[0]);
        ::close(watchdog[1]);
        if (sv[1] != 3) {
            if (::dup2(sv[1], 3) < 0)
                ::_exit(127);
            ::close(sv[1]);
            sv[1] = 3;
        }
        if (watchdog[0] != 4) {
            if (::dup2(watchdog[0], 4) < 0)
                ::_exit(127);
            if (watchdog[0] != 3)
                ::close(watchdog[0]);
            watchdog[0] = 4;
        }
        const int ipc_fd = 3;
        const int wd_fd  = 4;
        const int keep[] = {ipc_fd, wd_fd, errp[1]};
        for (int fd = 3; fd < maxfd; ++fd) {
            bool keep_fd = false;
            for (int k : keep) {
                if (fd == k)
                    keep_fd = true;
            }
            if (!keep_fd)
                ::close(fd);
        }
        ::execv(helper_str.c_str(), argv);
        const int e = errno;
        (void) !::write(errp[1], &e, sizeof(e));
        ::_exit(127);
    }

    close_fd(sv[1]);
    close_fd(errp[1]);
    close_fd(watchdog[0]);
    int    exec_err = 0;
    pollfd p{errp[0], POLLIN, 0};
    int    r;
    do {
        r = ::poll(&p, 1, 3000);
    } while (r < 0 && errno == EINTR);
    ssize_t n = 0;
    if (r > 0) {
        do {
            n = ::read(errp[0], &exec_err, sizeof(exec_err));
        } while (n < 0 && errno == EINTR);
    }
    close_fd(errp[0]);
    if (n == static_cast<ssize_t>(sizeof(exec_err))) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        error = "edge-plugin-host exec failed";
        return false;
    }

    set_nosigpipe(sv[0]);
    out.channel           = Channel::from_posix_fd(sv[0]);
    out.pid               = static_cast<std::int64_t>(pid);
    out.watchdog_write_fd = watchdog[1];
    sv[0]                 = -1;
    watchdog[1]           = -1;
    return true;
}

#if defined(__linux__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
bool spawn_host_posix_spawn(SpawnedHost& out,
                            const boost::filesystem::path& helper,
                            int sv[2],
                            int watchdog[2],
                            std::string& error)
{
    const std::string helper_str = helper.string();
    char              fd_arg[]   = "3";
    char              wd_arg[]   = "4";
    char*             argv[]     = {const_cast<char*>(helper_str.c_str()),
                                    const_cast<char*>("--ipc-fd"),
                                    fd_arg,
                                    const_cast<char*>("--parent-watchdog-fd"),
                                    wd_arg,
                                    nullptr};

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t          attr;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        error = "posix_spawn_file_actions_init failed";
        return false;
    }
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        error = "posix_spawnattr_init failed";
        return false;
    }

    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_CLOEXEC_DEFAULT;
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);

    auto add_dup2 = [&](int from, int to) {
        return posix_spawn_file_actions_adddup2(&fa, from, to) == 0;
    };
    auto add_close = [&](int fd) {
        if (fd == 3 || fd == 4)
            return true;
        return posix_spawn_file_actions_addclose(&fa, fd) == 0;
    };

    bool ok = add_dup2(sv[1], 3) && add_dup2(watchdog[0], 4) && add_close(sv[0]) && add_close(sv[1]) &&
              add_close(watchdog[0]) && add_close(watchdog[1]);
    pid_t pid = 0;
    int   rc  = ok ? posix_spawn(&pid, helper_str.c_str(), &fa, &attr, argv, environ) : EINVAL;
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        error = "posix_spawn failed";
        return false;
    }

    close_fd(sv[1]);
    close_fd(watchdog[0]);
    set_nosigpipe(sv[0]);
    out.channel           = Channel::from_posix_fd(sv[0]);
    out.pid               = static_cast<std::int64_t>(pid);
    out.watchdog_write_fd = watchdog[1];
    sv[0]                 = -1;
    watchdog[1]           = -1;
    return true;
}
#endif

} // namespace

bool spawn_host(SpawnedHost& out, std::string& error)
{
    close_host(out);
    const boost::filesystem::path helper = helper_path();
    if (!helper_exists()) {
        error = "edge-plugin-host not found at " + helper.string();
        return false;
    }

    int sv[2] = {-1, -1};
    if (socketpair_cloexec(sv) != 0) {
        error = "socketpair failed";
        return false;
    }
    set_nosigpipe(sv[0]);
    set_nosigpipe(sv[1]);

    int watchdog[2] = {-1, -1};
    if (pipe_cloexec(watchdog) != 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        error = "watchdog pipe failed";
        return false;
    }

#if defined(__linux__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    if (spawn_host_posix_spawn(out, helper, sv, watchdog, error))
        return true;
    BOOST_LOG_TRIVIAL(warning) << "posix_spawn of edge-plugin-host failed (" << error
                               << "); falling back to fork";
    error.clear();
#endif

    int errp[2] = {-1, -1};
    if (pipe_cloexec(errp) != 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        ::close(watchdog[0]);
        ::close(watchdog[1]);
        error = "error pipe failed";
        return false;
    }

    long maxfd = ::sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 4096)
        maxfd = 4096;

    if (!spawn_host_fork(out, helper, sv, watchdog, errp, maxfd, error)) {
        close_fd(sv[0]);
        close_fd(sv[1]);
        close_fd(watchdog[0]);
        close_fd(watchdog[1]);
        close_fd(errp[0]);
        close_fd(errp[1]);
        return false;
    }
    return true;
}

void kill_host(SpawnedHost& host)
{
    kill_host_pid(host.pid);
}

void kill_host_pid(std::int64_t pid)
{
    if (pid <= 0)
        return;
    ::kill(static_cast<pid_t>(pid), SIGKILL);
}

void close_host(SpawnedHost& host)
{
    host.channel.close();
    close_watchdog(host);
    if (host.pid > 0) {
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            const pid_t w = ::waitpid(static_cast<pid_t>(host.pid), &status, WNOHANG);
            if (w != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (::waitpid(static_cast<pid_t>(host.pid), &status, WNOHANG) == 0) {
            ::kill(static_cast<pid_t>(host.pid), SIGKILL);
            while (::waitpid(static_cast<pid_t>(host.pid), &status, 0) < 0 && errno == EINTR) {}
        }
        host.pid = 0;
    }
}

#endif

} // namespace Slic3r::plugin_ipc
