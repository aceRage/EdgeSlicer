#pragma once

// The platform-dependent edges of driving the user's own Tailscale install from the hub
// (RemoteHub.cpp): where the CLI lives, which download page to send people to, the words used for
// "this computer", and a POSIX way to run it and capture what it prints.
//
// Everything here is header-only and takes the platform (or an "exists" predicate) as an argument
// so tests/slic3rutils/tailscale_cli_tests.cpp can drive every platform's answer on any machine.
// Only run_capture_posix() touches the OS, and it does not exist on Windows (the hub has its own
// CreateProcess version of that).

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <cerrno>
#  include <chrono>
#  include <csignal>
#  include <cstdlib>
#  include <cstring>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <thread>
#  include <unistd.h>
#endif

namespace Slic3r {
namespace TailscaleCli {

enum class Platform { Windows, MacOS, Linux };

// BackendState stand-in for "the CLI ran too long and was killed" (never a value tailscaled sends).
inline constexpr const char* BACKEND_NO_ANSWER = "NoAnswer";

constexpr Platform current_platform()
{
#if defined(_WIN32)
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::MacOS;
#else
    return Platform::Linux;
#endif
}

// Where the card sends people who have no Tailscale yet.
inline const char* download_url(Platform p)
{
    switch (p) {
    case Platform::MacOS: return "https://tailscale.com/download/mac";
    case Platform::Linux: return "https://tailscale.com/download/linux";
    default:              return "https://tailscale.com/download/windows";
    }
}

// "this PC" / "this Mac" / "this computer", for the sentences the card shows.
inline const char* this_computer(Platform p)
{
    switch (p) {
    case Platform::MacOS: return "this Mac";
    case Platform::Linux: return "this computer";
    default:              return "this PC";
    }
}

// "your PC" / "your Mac" / "your computer".
inline const char* your_computer(Platform p)
{
    switch (p) {
    case Platform::MacOS: return "your Mac";
    case Platform::Linux: return "your computer";
    default:              return "your PC";
    }
}

// Where Tailscale's own sign-in lives once it is installed.
inline const char* sign_in_place(Platform p)
{
    return p == Platform::MacOS ? "the menu bar" : "the system tray";
}

// The places the CLI is looked for, best first. A GUI app on macOS (and a launcher-started one on
// Linux) is handed a minimal PATH that has none of these directories, so a bare "tailscale" would
// find nothing even with Tailscale installed and running; each is named explicitly instead.
//   macOS:  the app bundle's own binary (standalone and App Store builds both ship it and it acts
//           as the CLI when given arguments), the shim the app's "Install CLI" writes, Homebrew.
//   Linux:  the package-manager and snap locations.
//   Windows: <Program Files>\Tailscale\tailscale.exe.
// `program_files` is only read for Windows.
inline std::vector<std::string> exe_candidates(Platform p, const std::string& program_files = std::string())
{
    switch (p) {
    case Platform::MacOS:
        return { "/Applications/Tailscale.app/Contents/MacOS/Tailscale", "/usr/local/bin/tailscale", "/opt/homebrew/bin/tailscale" };
    case Platform::Linux:
        return { "/usr/bin/tailscale", "/usr/local/bin/tailscale", "/usr/sbin/tailscale", "/snap/bin/tailscale" };
    default:
        return { (program_files.empty() ? std::string("C:\\Program Files") : program_files) + "\\Tailscale\\tailscale.exe" };
    }
}

// The first candidate that exists, else the bare name for a PATH lookup.
inline std::string pick_exe(Platform p, const std::string& program_files, const std::function<bool(const std::string&)>& exists)
{
    for (const std::string& c : exe_candidates(p, program_files))
        if (exists && exists(c)) return c;
    return "tailscale";
}

#ifndef _WIN32

// The path to execute for `name`: a name with a slash is used as given; a bare name is searched
// for in `path_env` (a ':'-separated list, as PATH) for a regular file we may execute. Empty when
// there is no such file. No shell is involved, so nothing in `name` is ever interpreted.
inline std::string resolve_executable(const std::string& name, const std::string& path_env)
{
    if (name.empty()) return std::string();
    if (name.find('/') != std::string::npos) return name;
    const std::string path = path_env.empty() ? std::string("/usr/bin:/bin:/usr/sbin:/sbin") : path_env;
    size_t            pos  = 0;
    while (pos <= path.size()) {
        size_t colon = path.find(':', pos);
        if (colon == std::string::npos) colon = path.size();
        const std::string dir = path.substr(pos, colon - pos);
        pos                   = colon + 1;
        if (dir.empty()) continue;
        const std::string cand = dir + "/" + name;
        struct stat       st;
        if (::stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(cand.c_str(), X_OK) == 0) return cand;
    }
    return std::string();
}

namespace detail {

inline void set_cloexec(int fd)
{
    const int fl = ::fcntl(fd, F_GETFD);
    if (fl >= 0) ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

// Keep a descriptor out of 0..2, so the dup2()s in the child onto stdin/stdout/stderr can never
// overwrite one of our own ends (an app that closed its standard streams hands out low numbers).
inline void move_above_stdio(int& fd)
{
    if (fd >= 3) return;
    const int n = ::fcntl(fd, F_DUPFD, 3);
    if (n >= 0) { ::close(fd); fd = n; }
}

inline void close_fd(int& fd)
{
    if (fd >= 0) { ::close(fd); fd = -1; }
}

} // namespace detail

// Run a command to completion and capture what it prints (stdout and stderr merged, like the
// Windows version). Returns true when the program ran and finished within `timeout_ms`; false when
// it could not be started (not found, not executable, fork failed) or had to be killed for running
// too long. `exit_code` is the exit status, 128 + signal if a signal ended it, -1 if never run.
//
// - No shell: `args` is the argv, args[0] is resolved on PATH when it has no slash.
// - The child gets /dev/null on stdin and every descriptor above stderr closed, SIGPIPE back to
//   its default and an empty signal mask (the hub ignores SIGPIPE, and an exec inherits that).
// - Reads are non-blocking and poll()ed against the deadline, so a child that prints a lot, or
//   nothing, cannot wedge the caller; output past `max_out` bytes is read and dropped.
// - On timeout the child is SIGKILLed; in every case it is waitpid()ed, so there is no zombie.
// - A failed exec is reported through a close-on-exec pipe (the child writes its errno), so a
//   program that exists but cannot run is "could not be started" rather than exit status 127.
//
// `timed_out` (optional) says which kind of "false" it was. `stop_early` (optional) sees all the
// output so far after every read; when it returns true the child is killed at once and the call
// returns false with timed_out unset - for a command that prints what it is waiting for (`tailscale
// serve` printing the admin-console link) and would otherwise sit until the timeout.
inline bool run_capture_posix(const std::vector<std::string>& args, std::string& out, int& exit_code, int timeout_ms,
                              size_t max_out = 1024 * 1024, bool* timed_out_flag = nullptr,
                              const std::function<bool(const std::string&)>& stop_early = nullptr)
{
    if (timed_out_flag) *timed_out_flag = false;
    out.clear();
    exit_code = -1;
    if (args.empty()) return false;
    const char*       path_env = std::getenv("PATH");
    const std::string exe      = resolve_executable(args.front(), path_env ? path_env : "");
    if (exe.empty()) return false;

    // Everything the child needs is built before fork(): between fork() and exec() only
    // async-signal-safe calls are allowed in a multi-threaded process, which rules out allocating.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int outp[2] = { -1, -1 }; // child's stdout+stderr -> us
    int errp[2] = { -1, -1 }; // exec failure report (errno) -> us
    int devnull = -1;
    if (::pipe(outp) != 0) return false;
    if (::pipe(errp) != 0) { ::close(outp[0]); ::close(outp[1]); return false; }
    for (int* fd : { &outp[0], &outp[1], &errp[0], &errp[1] }) {
        detail::move_above_stdio(*fd);
        detail::set_cloexec(*fd);
    }
    devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) { detail::move_above_stdio(devnull); detail::set_cloexec(devnull); }
    long maxfd = ::sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 4096) maxfd = 4096;

    const pid_t pid = ::fork();
    if (pid < 0) {
        detail::close_fd(outp[0]); detail::close_fd(outp[1]);
        detail::close_fd(errp[0]); detail::close_fd(errp[1]);
        detail::close_fd(devnull);
        return false;
    }
    if (pid == 0) {
        // Child: async-signal-safe calls only from here to execv().
        ::signal(SIGPIPE, SIG_DFL);
        sigset_t none;
        sigemptyset(&none); // no ::, macOS defines sigemptyset as a macro
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        if (devnull >= 0) ::dup2(devnull, 0);
        ::dup2(outp[1], 1);
        ::dup2(outp[1], 2);
        for (int fd = 3; fd < maxfd; ++fd)
            if (fd != errp[1]) ::close(fd);
        ::execv(exe.c_str(), argv.data());
        const int e = errno;
        (void) !::write(errp[1], &e, sizeof(e));
        ::_exit(127);
    }

    detail::close_fd(outp[1]);
    detail::close_fd(errp[1]);
    detail::close_fd(devnull);
    const int rd = outp[0];
    const int er = errp[0];
    ::fcntl(rd, F_SETFL, ::fcntl(rd, F_GETFL) | O_NONBLOCK);

    using clock               = std::chrono::steady_clock;
    const auto deadline       = clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    const auto remaining_ms   = [&]() -> long long {
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
    };
    auto reap = [&](int& status) {
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    };

    // Did exec work? The write end closes on a successful exec (EOF here), or carries the errno.
    {
        pollfd p { er, POLLIN, 0 };
        int    r;
        do { r = ::poll(&p, 1, (int) std::min<long long>(std::max<long long>(remaining_ms(), 0), 5000)); } while (r < 0 && errno == EINTR);
        if (r > 0) {
            int     e = 0;
            ssize_t n;
            do { n = ::read(er, &e, sizeof(e)); } while (n < 0 && errno == EINTR);
            if (n == (ssize_t) sizeof(e)) {
                int status = 0;
                reap(status);
                ::close(rd);
                ::close(er);
                return false;
            }
        }
    }
    ::close(er);

    bool timed_out = false;
    bool stopped   = false;
    bool eof       = false;
    char buf[4096];
    while (!eof) {
        const long long left = remaining_ms();
        if (left <= 0) { timed_out = true; break; }
        pollfd p { rd, POLLIN, 0 };
        const int r = ::poll(&p, 1, (int) std::min<long long>(left, 1000));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;
        // One read per wake-up: a child that never stops talking still lets the deadline be checked.
        const ssize_t n = ::read(rd, buf, sizeof(buf));
        if (n > 0) {
            if (out.size() < max_out) out.append(buf, std::min<size_t>((size_t) n, max_out - out.size()));
            if (stop_early && stop_early(out)) { stopped = true; break; }
        } else if (n == 0) {
            eof = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            eof = true;
        }
    }
    ::close(rd);

    // stdout closed (or we ran out of time): now wait for the process itself, against the same deadline.
    int status = 0;
    if (!timed_out && !stopped) {
        for (;;) {
            const pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
                return true;
            }
            if (w < 0 && errno != EINTR) {
                // Someone else reaped it (SIGCHLD ignored): it did finish, its status is just gone.
                exit_code = 0;
                return true;
            }
            if (remaining_ms() <= 0) { timed_out = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ::kill(pid, SIGKILL);
    reap(status);
    exit_code = 128 + SIGKILL;
    if (timed_out_flag) *timed_out_flag = timed_out && !stopped;
    return false;
}

#endif // !_WIN32

} // namespace TailscaleCli
} // namespace Slic3r
