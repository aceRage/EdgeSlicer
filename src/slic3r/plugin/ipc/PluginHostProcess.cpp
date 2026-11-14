#include "PluginHostProcess.hpp"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>

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
#include <thread>
#endif

namespace Slic3r::plugin_ipc {
namespace {

#ifdef _WIN32
std::wstring widen(const std::string& s) { return boost::nowide::widen(s); }

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
#endif

} // namespace

boost::filesystem::path helper_path()
{
    if (const char* env = boost::nowide::getenv("EDGE_PLUGIN_HOST")) {
        if (env[0] != '\0')
            return boost::filesystem::path(env);
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

    const std::wstring cmd = quote_arg(widen(helper.string())) + L" --ipc-pipe " + quote_arg(pipe_name);
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

bool spawn_host(SpawnedHost& out, std::string& error)
{
    close_host(out);
    const boost::filesystem::path helper = helper_path();
    if (!helper_exists()) {
        error = "edge-plugin-host not found at " + helper.string();
        return false;
    }

    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        error = "socketpair failed";
        return false;
    }

    int errp[2] = {-1, -1};
    if (::pipe(errp) != 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        error = "error pipe failed";
        return false;
    }
    for (int fd : {errp[0], errp[1]}) {
        const int fl = ::fcntl(fd, F_GETFD);
        if (fl >= 0)
            ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }

#ifdef __APPLE__
    int watchdog[2] = {-1, -1};
    if (::pipe(watchdog) != 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        ::close(errp[0]);
        ::close(errp[1]);
        error = "watchdog pipe failed";
        return false;
    }
#endif

    long maxfd = ::sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 4096)
        maxfd = 4096;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        ::close(errp[0]);
        ::close(errp[1]);
#ifdef __APPLE__
        ::close(watchdog[0]);
        ::close(watchdog[1]);
#endif
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
#ifdef __APPLE__
        ::close(watchdog[1]);
#endif
        if (sv[1] != 3) {
            if (::dup2(sv[1], 3) < 0)
                ::_exit(127);
            ::close(sv[1]);
            sv[1] = 3;
        }
        const int ipc_fd = sv[1];
        const int keep[] = {
            ipc_fd, errp[1],
#ifdef __APPLE__
            watchdog[0],
#endif
        };
        for (int fd = 3; fd < maxfd; ++fd) {
            bool keep_fd = false;
            for (int k : keep) {
                if (fd == k)
                    keep_fd = true;
            }
            if (!keep_fd)
                ::close(fd);
        }
        std::string fd_arg = std::to_string(ipc_fd);
        std::vector<char*> argv;
        std::string helper_str = helper.string();
        argv.push_back(const_cast<char*>(helper_str.c_str()));
        argv.push_back(const_cast<char*>("--ipc-fd"));
        argv.push_back(const_cast<char*>(fd_arg.c_str()));
#ifdef __APPLE__
        std::string wd_arg = std::to_string(watchdog[0]);
        argv.push_back(const_cast<char*>("--parent-watchdog-fd"));
        argv.push_back(const_cast<char*>(wd_arg.c_str()));
#endif
        argv.push_back(nullptr);
        ::execv(helper_str.c_str(), argv.data());
        const int e = errno;
        (void) !::write(errp[1], &e, sizeof(e));
        ::_exit(127);
    }

    ::close(sv[1]);
    ::close(errp[1]);
#ifdef __APPLE__
    ::close(watchdog[0]);
#endif
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
    ::close(errp[0]);
    if (n == static_cast<ssize_t>(sizeof(exec_err))) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        ::close(sv[0]);
#ifdef __APPLE__
        ::close(watchdog[1]);
#endif
        error = "edge-plugin-host exec failed";
        return false;
    }

    out.channel = Channel::from_posix_fd(sv[0]);
    out.pid     = static_cast<std::int64_t>(pid);
#ifdef __APPLE__
    out.watchdog_write_fd = watchdog[1];
#endif
    return true;
}

void kill_host(SpawnedHost& host)
{
    if (host.pid <= 0)
        return;
    ::kill(static_cast<pid_t>(host.pid), SIGKILL);
}

void close_host(SpawnedHost& host)
{
    host.channel.close();
#ifdef __APPLE__
    if (host.watchdog_write_fd >= 0) {
        ::close(host.watchdog_write_fd);
        host.watchdog_write_fd = -1;
    }
#endif
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
