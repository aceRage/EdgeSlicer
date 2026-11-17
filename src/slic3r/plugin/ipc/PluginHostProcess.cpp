#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "PluginHostProcess.hpp"
#include "PluginSandbox.hpp"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwchar>
#include <cwctype>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <cerrno>
#include <poll.h>
#endif

#ifndef _WIN32
#ifndef __APPLE__
// POSIX: environ is not in the C++ standard library headers. Declare it at file
// scope so it is not Slic3r::plugin_ipc::environ. macOS has no such symbol;
// use *_NSGetEnviron() instead.
extern char **environ;
#endif
#endif

namespace Slic3r::plugin_ipc {
namespace {

std::atomic<bool> g_allow_env_overrides{false};

#ifdef _WIN32
// NTSTATUS / Win32 codes seen when JOB_OBJECT_LIMIT_PROCESS_MEMORY kills the child.
constexpr DWORD k_status_no_memory         = 0xC0000017u;
constexpr DWORD k_status_quota_exceeded    = 0xC0000044u;
constexpr DWORD k_status_commit_limit      = 0xC000012Du;
constexpr DWORD k_error_not_enough_quota   = 1816u;
constexpr DWORD k_error_commitment_limit   = 1455u;
// PluginHostMain returns 1000 + GetLastError() when CreateFileW on the IPC pipe fails.
constexpr DWORD k_host_pipe_open_exit_base = 1000u;

std::string win32_error_text(DWORD err)
{
    wchar_t* buf = nullptr;
    const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string text;
    if (n > 0 && buf != nullptr) {
        text = boost::nowide::narrow(buf);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        ::LocalFree(buf);
    }
    return text;
}

std::string host_connect_failure_message(DWORD exit_code)
{
    std::string msg = "edge-plugin-host exited before connecting (exit code " + std::to_string(exit_code) + ")";
    if (exit_code >= k_host_pipe_open_exit_base) {
        const DWORD winerr = exit_code - k_host_pipe_open_exit_base;
        msg += ": pipe open failed";
        if (winerr == ERROR_ACCESS_DENIED)
            msg += " (access denied)";
        const std::string detail = win32_error_text(winerr);
        if (!detail.empty())
            msg += " — " + detail;
    }
    return msg;
}

bool win_env_entry_allowed(const wchar_t* entry)
{
    if (entry == nullptr || entry[0] == L'\0')
        return false;
    auto starts = [](const wchar_t* e, const wchar_t* p) {
        for (; *p != L'\0'; ++p, ++e) {
            if (*e == L'\0')
                return false;
            if (std::towupper(static_cast<wint_t>(*e)) != std::towupper(static_cast<wint_t>(*p)))
                return false;
        }
        return true;
    };
    static const wchar_t* prefixes[] = {L"PATH=",
                                        L"PATHEXT=",
                                        L"SYSTEMROOT=",
                                        L"SYSTEMDRIVE=",
                                        L"WINDIR=",
                                        L"COMSPEC=",
                                        L"TEMP=",
                                        L"TMP=",
                                        L"USERPROFILE=",
                                        L"USERNAME=",
                                        L"USERDOMAIN=",
                                        L"COMPUTERNAME=",
                                        L"HOMEDRIVE=",
                                        L"HOMEPATH=",
                                        L"NUMBER_OF_PROCESSORS=",
                                        L"PROCESSOR_ARCHITECTURE=",
                                        L"PROGRAMFILES=",
                                        L"PROGRAMFILES(X86)=",
                                        L"PROGRAMW6432=",
                                        L"PYTHONNOUSERSITE=",
                                        L"ALLUSERSPROFILE=",
                                        L"PUBLIC=",
                                        L"APPDATA=",
                                        L"LOCALAPPDATA="};
    for (const wchar_t* prefix : prefixes) {
        if (starts(entry, prefix))
            return true;
    }
    return false;
}

std::vector<wchar_t> scrubbed_helper_env_block()
{
    std::vector<std::wstring> kept;
    bool                      have_nousersite = false;
    LPWCH                     strings         = ::GetEnvironmentStringsW();
    if (strings) {
        for (LPWCH p = strings; *p != L'\0'; p += std::wcslen(p) + 1) {
            if (!win_env_entry_allowed(p))
                continue;
            kept.emplace_back(p);
            const wchar_t prefix[] = L"PYTHONNOUSERSITE=";
            bool          match    = true;
            for (size_t i = 0; prefix[i] != L'\0'; ++i) {
                if (p[i] == L'\0' || std::towupper(static_cast<wint_t>(p[i])) != prefix[i]) {
                    match = false;
                    break;
                }
            }
            if (match)
                have_nousersite = true;
        }
        ::FreeEnvironmentStringsW(strings);
    }
    if (!have_nousersite)
        kept.emplace_back(L"PYTHONNOUSERSITE=1");
    std::vector<wchar_t> block;
    for (const std::wstring& s : kept) {
        block.insert(block.end(), s.begin(), s.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

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
// F_DUPFD clears FD_CLOEXEC; F_DUPFD_CLOEXEC keeps it so the exec-error pipe
// closes on execv (ipc/wd then dup2 to 3/4, which clears CLOEXEC on purpose).
int move_fd_ge(int fd, int min_fd)
{
    if (fd < 0)
        return -1;
#ifdef F_DUPFD_CLOEXEC
    const int n = ::fcntl(fd, F_DUPFD_CLOEXEC, min_fd);
#else
    const int n = ::fcntl(fd, F_DUPFD, min_fd);
#endif
    if (n < 0)
        return -1;
    ::close(fd);
#ifndef F_DUPFD_CLOEXEC
    set_cloexec(n);
#endif
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

void set_allow_helper_env_overrides_for_testing(bool allow) { g_allow_env_overrides.store(allow, std::memory_order_release); }

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

bool spawn_host(SpawnedHost& out, std::string& error) { return spawn_host(out, SandboxSpec{}, error); }

bool spawn_host(SpawnedHost& out, const SandboxSpec& spec, std::string& error)
{
    close_host(out);
    const boost::filesystem::path helper = helper_path();
    if (!helper_exists()) {
        error = "edge-plugin-host not found at " + helper.string();
        return false;
    }

    const bool sandbox_on = !sandbox_env_disable_requested() && !sandbox_env_force_unavailable();
    if (sandbox_on && !prepare_parent_sandbox(spec, error))
        return false;

    wchar_t     pipe_name[128];
    static LONG counter = 0;
    const LONG  n       = ::InterlockedIncrement(&counter);
    swprintf(pipe_name, 128, L"\\\\.\\pipe\\edge-plugin-host-%lu-%ld", static_cast<unsigned long>(::GetCurrentProcessId()),
             static_cast<long>(n));

    SECURITY_ATTRIBUTES* pipe_sa = sandbox_on ? static_cast<SECURITY_ATTRIBUTES*>(parent_sandbox_pipe_security_attributes()) : nullptr;
    HANDLE pipe = ::CreateNamedPipeW(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                     1, 64 * 1024, 64 * 1024, 0, pipe_sa);
    if (pipe == INVALID_HANDLE_VALUE) {
        close_parent_sandbox();
        error = "CreateNamedPipe failed";
        return false;
    }

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        ::CloseHandle(pipe);
        close_parent_sandbox();
        error = "CreateJobObject failed";
        return false;
    }
    // DoS cap, not a security boundary, and not plugin-configurable (that would be an escape).
    constexpr SIZE_T                     k_plugin_host_job_process_memory = 1024ull * 1024ull * 1024ull;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    jeli.ProcessMemoryLimit               = k_plugin_host_job_process_memory;
    if (!spec.allow_exec) {
        jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        jeli.BasicLimitInformation.ActiveProcessLimit = 1;
    }
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        ::CloseHandle(job);
        ::CloseHandle(pipe);
        close_parent_sandbox();
        error = "SetInformationJobObject failed";
        return false;
    }

    const std::wstring   cmd = quote_arg(helper.wstring()) + L" --ipc-pipe " + quote_arg(pipe_name);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOEXW      siex{};
    STARTUPINFOW*       si = &siex.StartupInfo;
    PROCESS_INFORMATION pi{};
    DWORD               flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | CREATE_NO_WINDOW;
    LPVOID              attr  = parent_sandbox_security_capabilities();
    if (sandbox_on && attr) {
        si->cb               = sizeof(siex);
        siex.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr);
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    } else {
        si->cb = sizeof(STARTUPINFOW);
    }
    std::vector<wchar_t> env_block = scrubbed_helper_env_block();
    const BOOL ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, flags, env_block.data(), nullptr, si, &pi);
    if (!ok) {
        ::CloseHandle(job);
        ::CloseHandle(pipe);
        close_parent_sandbox();
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
        close_parent_sandbox();
        return false;
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    // Keep AppContainer ACL grants until close_host. Revoking here (r1) dropped
    // plugin_root / write_root from the running child.

    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(job);
        ::CloseHandle(pipe);
        close_parent_sandbox();
        error = "CreateEvent failed";
        return false;
    }

    auto abort_connect = [&](const std::string& msg, bool terminate_child) {
        ::CancelIoEx(pipe, &ov);
        ::CloseHandle(ov.hEvent);
        if (terminate_child)
            ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(job);
        ::CloseHandle(pipe);
        close_parent_sandbox();
        error = msg;
        return false;
    };

    BOOL connected = ::ConnectNamedPipe(pipe, &ov);
    if (!connected) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_IO_PENDING) {
            HANDLE wait[2] = {ov.hEvent, pi.hProcess};
            const DWORD wr = ::WaitForMultipleObjects(2, wait, FALSE, 15000);
            if (wr == WAIT_OBJECT_0 + 1) {
                DWORD exit_code = 1;
                ::GetExitCodeProcess(pi.hProcess, &exit_code);
                return abort_connect(host_connect_failure_message(exit_code), false);
            }
            if (wr != WAIT_OBJECT_0)
                return abort_connect("ConnectNamedPipe timed out", true);
        } else if (err != ERROR_PIPE_CONNECTED) {
            return abort_connect("ConnectNamedPipe failed", true);
        }
    }
    ::CloseHandle(ov.hEvent);

    ULONG client_pid = 0;
    if (!::GetNamedPipeClientProcessId(pipe, &client_pid) || client_pid != pi.dwProcessId) {
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(job);
        ::CloseHandle(pipe);
        close_parent_sandbox();
        error = "named pipe client pid mismatch";
        return false;
    }

    out.channel          = Channel::from_windows_handle(pipe);
    out.pid              = static_cast<std::int64_t>(pi.dwProcessId);
    out.process          = pi.hProcess;
    out.job              = job;
    out.last_wait_status = 0;
    out.wait_reaped      = false;
    return true;
}

void kill_host_handle(void* process)
{
    if (process)
        ::TerminateProcess(static_cast<HANDLE>(process), 1);
}

void kill_host(SpawnedHost& host)
{
    if (host.job)
        ::TerminateJobObject(static_cast<HANDLE>(host.job), 1);
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
        DWORD code = 0;
        if (::GetExitCodeProcess(static_cast<HANDLE>(host.process), &code)) {
            host.last_wait_status = static_cast<int>(code);
            host.wait_reaped      = true;
        }
        ::CloseHandle(static_cast<HANDLE>(host.process));
        host.process = nullptr;
    }
    if (host.job) {
        ::CloseHandle(static_cast<HANDLE>(host.job));
        host.job = nullptr;
    }
    host.pid = 0;
    close_parent_sandbox();
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

bool env_entry_allowed(const char* entry)
{
    if (entry == nullptr || entry[0] == '\0')
        return false;
    static const char* exact[] = {"PATH=",
                                  "HOME=",
                                  "USER=",
                                  "LOGNAME=",
                                  "LANG=",
                                  "TZ=",
                                  "TMPDIR=",
                                  "TMP=",
                                  "TEMP=",
                                  "TERM=",
                                  "CURL_CA_BUNDLE=",
                                  "REQUESTS_CA_BUNDLE=",
                                  "FLATPAK_ID=",
                                  "PYTHONNOUSERSITE=",
                                  "LD_LIBRARY_PATH=",
                                  "APPDIR=",
                                  "APPIMAGE="};
    for (const char* prefix : exact) {
        const size_t n = std::strlen(prefix);
        if (std::strncmp(entry, prefix, n) == 0)
            return true;
    }
    return std::strncmp(entry, "LC_", 3) == 0 || std::strncmp(entry, "SSL_CERT_", 9) == 0;
}

std::vector<std::string> scrubbed_helper_env()
{
    std::vector<std::string> kept;
    bool                     have_nousersite = false;
#ifdef __APPLE__
    char** envp = *_NSGetEnviron();
#else
    char** envp = environ;
#endif
    for (char** e = envp; e && *e; ++e) {
        if (!env_entry_allowed(*e))
            continue;
        kept.emplace_back(*e);
        if (std::strncmp(*e, "PYTHONNOUSERSITE=", 17) == 0)
            have_nousersite = true;
    }
    if (!have_nousersite)
        kept.emplace_back("PYTHONNOUSERSITE=1");
    return kept;
}

bool spawn_host_fork(SpawnedHost&                   out,
                     const boost::filesystem::path& helper,
                     int                            sv[2],
                     int                            watchdog[2],
                     int                            errp[2],
                     int                            stderrp[2],
                     long                           maxfd,
                     std::string&                   error)
{
    // argv and env live in the parent so the child does not allocate after fork.
    const std::string        helper_str = helper.string();
    char                     fd_arg[]   = "3";
    char                     wd_arg[]   = "4";
    char*                    argv[]     = {const_cast<char*>(helper_str.c_str()),
                                           const_cast<char*>("--ipc-fd"),
                                           fd_arg,
                                           const_cast<char*>("--parent-watchdog-fd"),
                                           wd_arg,
                                           nullptr};
    std::vector<std::string> env_keep   = scrubbed_helper_env();
    std::vector<char*>       envp;
    envp.reserve(env_keep.size() + 1);
    for (std::string& s : env_keep)
        envp.push_back(s.data());
    envp.push_back(nullptr);

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
        ::close(stderrp[0]);
        // Move the kept fds above 10 first so dup2(3)/dup2(4) cannot clobber
        // watchdog[0] or errp[1], and so a CLOEXEC fd that already is 3 or 4 still
        // gets a real dup2 (dup2 clears FD_CLOEXEC; skipping it left CLOEXEC set).
        int ipc     = move_fd_ge(sv[1], 11);
        int wd      = move_fd_ge(watchdog[0], 11);
        int err     = move_fd_ge(errp[1], 11);
        int stderrw = move_fd_ge(stderrp[1], 11);
        if (ipc < 0 || wd < 0 || err < 0 || stderrw < 0)
            ::_exit(127);
        if (::dup2(ipc, 3) < 0)
            ::_exit(127);
        if (ipc != 3)
            ::close(ipc);
        if (::dup2(wd, 4) < 0)
            ::_exit(127);
        if (wd != 4)
            ::close(wd);
        if (::dup2(stderrw, STDERR_FILENO) < 0)
            ::_exit(127);
        if (stderrw != STDERR_FILENO)
            ::close(stderrw);
        clear_cloexec(3);
        clear_cloexec(4);
        // Error pipe must stay CLOEXEC so execv closes it and the parent's
        // poll(errp[0]) sees EOF instead of waiting the full 3 s.
        set_cloexec(err);
        errp[1]          = err;
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
        ::execve(helper_str.c_str(), argv, envp.data());
        const int e = errno;
        (void) !::write(errp[1], &e, sizeof(e));
        ::_exit(127);
    }

    // Race-proof the child's setpgid(0,0): the parent also assigns the group.
    ::setpgid(pid, pid);

    close_fd(sv[1]);
    close_fd(errp[1]);
    close_fd(watchdog[0]);
    close_fd(stderrp[1]);
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
    out.last_wait_status  = 0;
    out.wait_reaped       = false;
    out.stderr_read_fd    = stderrp[0];
    if (out.stderr_read_fd >= 0) {
        const int fl = ::fcntl(out.stderr_read_fd, F_GETFL);
        if (fl >= 0)
            ::fcntl(out.stderr_read_fd, F_SETFL, fl | O_NONBLOCK);
    }
    sv[0]       = -1;
    watchdog[1] = -1;
    stderrp[0]  = -1;
    return true;
}

} // namespace

bool spawn_host(SpawnedHost& out, std::string& error) { return spawn_host(out, SandboxSpec{}, error); }

bool spawn_host(SpawnedHost& out, const SandboxSpec& spec, std::string& error)
{
    (void) spec;
    close_host(out);
    const boost::filesystem::path helper = helper_path();
    if (!helper_exists()) {
        error = "edge-plugin-host not found at " + helper.string();
        return false;
    }
#ifdef __linux__
    if (sandbox_env_bwrap_requested() && running_inside_flatpak()) {
        // Nested bwrap is never used; Landlock+seccomp inside Flatpak is the path.
    }
#endif

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

    int stderrp[2] = {-1, -1};
    if (pipe_cloexec(stderrp) != 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        ::close(watchdog[0]);
        ::close(watchdog[1]);
        ::close(errp[0]);
        ::close(errp[1]);
        error = "stderr pipe failed";
        return false;
    }

    long maxfd = ::sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 4096)
        maxfd = 4096;

    if (!spawn_host_fork(out, helper, sv, watchdog, errp, stderrp, maxfd, error)) {
        close_fd(sv[0]);
        close_fd(sv[1]);
        close_fd(watchdog[0]);
        close_fd(watchdog[1]);
        close_fd(errp[0]);
        close_fd(errp[1]);
        close_fd(stderrp[0]);
        close_fd(stderrp[1]);
        return false;
    }
    return true;
}

void kill_host(SpawnedHost& host) { kill_host_pid(host.pid); }

void kill_host_pid(std::int64_t pid)
{
    if (pid <= 0)
        return;
    const pid_t p = static_cast<pid_t>(pid);
    ::kill(-p, SIGKILL);
    ::kill(p, SIGKILL);
}

void close_host(SpawnedHost& host)
{
    host.channel.close();
    close_watchdog(host);
    if (host.pid > 0) {
        int   status = 0;
        pid_t w      = 0;
        for (int i = 0; i < 50; ++i) {
            w = ::waitpid(static_cast<pid_t>(host.pid), &status, WNOHANG);
            if (w != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (w == 0) {
            ::kill(static_cast<pid_t>(host.pid), SIGKILL);
            do {
                w = ::waitpid(static_cast<pid_t>(host.pid), &status, 0);
            } while (w < 0 && errno == EINTR);
        }
        if (w > 0) {
            host.last_wait_status = status;
            host.wait_reaped      = true;
        }
        host.pid = 0;
    }
    close_fd(host.stderr_read_fd);
}

#endif

std::string drain_host_stderr(SpawnedHost& host)
{
#ifdef _WIN32
    (void) host;
    return {};
#else
    if (host.stderr_read_fd < 0)
        return {};
    std::string out;
    char        buf[1024];
    for (;;) {
        const ssize_t n = ::read(host.stderr_read_fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    return out;
#endif
}

std::string host_exit_summary(const SpawnedHost& host)
{
    if (!host.wait_reaped)
        return "not reaped";
#ifdef _WIN32
    return "exit-code " + std::to_string(host.last_wait_status);
#else
    if (WIFEXITED(host.last_wait_status))
        return "exited " + std::to_string(WEXITSTATUS(host.last_wait_status));
    if (WIFSIGNALED(host.last_wait_status))
        return "signal " + std::to_string(WTERMSIG(host.last_wait_status));
    return "wait-status " + std::to_string(host.last_wait_status);
#endif
}

bool host_exceeded_job_memory_limit(const SpawnedHost& host)
{
#ifdef _WIN32
    DWORD code = STILL_ACTIVE;
    if (host.wait_reaped)
        code = static_cast<DWORD>(host.last_wait_status);
    else if (host.process != nullptr) {
        if (!::GetExitCodeProcess(static_cast<HANDLE>(host.process), &code))
            return false;
    } else
        return false;
    if (code == STILL_ACTIVE)
        return false;
    return code == k_status_no_memory || code == k_status_quota_exceeded || code == k_status_commit_limit ||
           code == k_error_not_enough_quota || code == k_error_commitment_limit;
#else
    (void) host;
    return false;
#endif
}

} // namespace Slic3r::plugin_ipc
