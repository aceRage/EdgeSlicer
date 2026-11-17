#include "PluginHostChild.hpp"
#include "PluginIpcChannel.hpp"
#include "PluginIpcProtocol.hpp"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#else
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace {

int parse_fd_arg(int argc, char** argv, const char* flag)
{
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], flag) == 0)
            return std::atoi(argv[i + 1]);
    }
    return -1;
}

std::string parse_string_arg(int argc, char** argv, const char* flag)
{
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], flag) == 0)
            return argv[i + 1] ? argv[i + 1] : "";
    }
    return {};
}

void on_terminate()
{
    using namespace Slic3r::plugin_ipc;
    child_diag("std::terminate");
    if (auto ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& ex) {
            child_diag(std::string("terminate exception: ") + ex.what());
        } catch (...) {
            child_diag("terminate unknown exception");
        }
    }
    std::_Exit(k_host_exit_terminate);
}

void on_sigabrt(int)
{
    using namespace Slic3r::plugin_ipc;
    child_diag("abort / SIGABRT");
    std::_Exit(k_host_exit_abort);
}

#ifdef _WIN32
void __cdecl on_invalid_parameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned int line, uintptr_t)
{
    using namespace Slic3r::plugin_ipc;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "invalid parameter in %ls %ls:%u expr=%ls", function ? function : L"?", file ? file : L"?", line,
                  expression ? expression : L"?");
    child_diag(buf);
    std::_Exit(k_host_exit_invalid_param);
}
#endif

void install_crash_handlers()
{
    std::set_terminate(on_terminate);
    std::signal(SIGABRT, on_sigabrt);
#ifdef _WIN32
    _set_invalid_parameter_handler(on_invalid_parameter);
    // Do not Watson / fail-fast on abort: write the log and _Exit(14) from SIGABRT.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
#endif
}

#ifdef _WIN32
std::wstring parse_pipe_arg(int argc, char** argv)
{
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--ipc-pipe") == 0) {
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0);
            if (n <= 0)
                return {};
            std::wstring out(static_cast<size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, out.data(), n);
            if (!out.empty() && out.back() == L'\0')
                out.pop_back();
            return out;
        }
    }
    return {};
}
#endif

void install_parent_death()
{
#ifdef __linux__
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() == 1)
        ::_exit(1);
#endif
}

} // namespace

#ifndef _WIN32
void set_cloexec_fd(int fd)
{
    if (fd < 0)
        return;
    const int fl = ::fcntl(fd, F_GETFD);
    if (fl >= 0)
        ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}
#endif

int main(int argc, char** argv)
{
    using namespace Slic3r::plugin_ipc;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::fputs("edge-plugin-host\n", stdout);
            return 0;
        }
    }
    set_in_child_process();
    const std::string host_log = parse_string_arg(argc, argv, "--host-log");
    if (!host_log.empty())
        set_child_diag_log_path(host_log);
    install_crash_handlers();
    child_diag("main: crash handlers installed");
    install_parent_death();

#ifndef _WIN32
    const int watchdog_fd = parse_fd_arg(argc, argv, "--parent-watchdog-fd");
    if (watchdog_fd >= 0) {
        set_cloexec_fd(watchdog_fd);
        std::thread([watchdog_fd]() {
            char buf;
            while (::read(watchdog_fd, &buf, 1) < 0 && errno == EINTR) {}
            ::_exit(1);
        }).detach();
    }
#endif

#ifdef _WIN32
    const std::wstring pipe = parse_pipe_arg(argc, argv);
    if (pipe.empty())
        return 1;
    HANDLE handle = ::CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // Distinct from the generic 1 (missing --ipc-pipe) so the parent can map
        // ERROR_ACCESS_DENIED (1005) vs a missing pipe without waiting 15 s.
        const DWORD err = ::GetLastError();
        return static_cast<int>(1000u + err);
    }
    DWORD mode = PIPE_READMODE_BYTE;
    ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
    Channel channel = Channel::from_windows_handle(handle);
#else
    const int ipc_fd = parse_fd_arg(argc, argv, "--ipc-fd");
    if (ipc_fd < 0)
        return 1;
    set_cloexec_fd(ipc_fd);
    Channel channel = Channel::from_posix_fd(ipc_fd);
#endif
    child_diag("main: ipc connected");
    int rc = 1;
    try {
        rc = run_child(channel);
    } catch (const std::exception& ex) {
        child_diag(std::string("uncaught std::exception: ") + ex.what());
        rc = k_host_exit_uncaught_std;
    } catch (...) {
        child_diag("uncaught unknown exception");
        rc = k_host_exit_uncaught_unknown;
    }
    // Skip C++ static destructors (Boost.Log TLS) — they abort with "Failed to set TLS value"
    // after the parent has already torn the process down.
    std::_Exit(rc);
}
