#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "PluginHostProcess.hpp"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/operations.hpp>
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
#include <sys/prctl.h>
#endif
#include <cerrno>
#include <poll.h>
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

void clear_cloexec(int fd)
{
    if (fd < 0)
        return;
    const int fl = ::fcntl(fd, F_GETFD);
    if (fl >= 0)
        ::fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC);
}

// Move fd to a number >= min_fd so later dup2(3)/dup2(4) cannot clobber it.
int move_fd_ge(int fd, int min_fd)
{
    if (fd < 0)
        return -1;
    const int n = ::fcntl(fd, F_DUPFD, min_fd);
    if (n < 0)
        return -1;
    ::close(fd);
    return n;
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

void kill_host_handle(void* process)
{
    if (process)
        ::TerminateProcess(static_cast<HANDLE>(process), 1);
}

void kill_host(SpawnedHost& host)
{
    kill_host_handle(host.process);
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
        // Move the kept fds above 10 first so dup2(3)/dup2(4) cannot clobber
        // watchdog[0] or errp[1], and so a CLOEXEC fd that already is 3 or 4 still
        // gets a real dup2 (dup2 clears FD_CLOEXEC; skipping it left CLOEXEC set).
        int ipc = move_fd_ge(sv[1], 11);
        int wd  = move_fd_ge(watchdog[0], 11);
        int err = move_fd_ge(errp[1], 11);
        if (ipc < 0 || wd < 0 || err < 0)
            ::_exit(127);
        if (::dup2(ipc, 3) < 0)
            ::_exit(127);
        if (ipc != 3)
            ::close(ipc);
        if (::dup2(wd, 4) < 0)
            ::_exit(127);
        if (wd != 4)
            ::close(wd);
        clear_cloexec(3);
        clear_cloexec(4);
        errp[1] = err;
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
