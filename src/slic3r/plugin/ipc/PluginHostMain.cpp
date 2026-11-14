#include "PluginHostChild.hpp"
#include "PluginIpcChannel.hpp"
#include "PluginIpcProtocol.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#ifndef _WIN32
#include <cerrno>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
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

#ifdef _WIN32
std::wstring parse_pipe_arg(int argc, char** argv)
{
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--ipc-pipe") == 0) {
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0);
            std::wstring out(n > 0 ? n - 1 : 0, L'\0');
            if (n > 1)
                ::MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, out.data(), n);
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

int main(int argc, char** argv)
{
    using namespace Slic3r::plugin_ipc;
    set_in_child_process();
    install_parent_death();

#ifndef _WIN32
    const int watchdog_fd = parse_fd_arg(argc, argv, "--parent-watchdog-fd");
    if (watchdog_fd >= 0) {
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
    HANDLE handle = ::CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return 1;
    DWORD mode = PIPE_READMODE_BYTE;
    ::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
    Channel channel = Channel::from_windows_handle(handle);
#else
    const int ipc_fd = parse_fd_arg(argc, argv, "--ipc-fd");
    if (ipc_fd < 0)
        return 1;
    Channel channel = Channel::from_posix_fd(ipc_fd);
#endif
    return run_child(channel);
}
