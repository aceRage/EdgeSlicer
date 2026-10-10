// Where the hub looks for the Tailscale CLI, which download page it offers and how it runs the
// CLI off Windows (slic3r/Utils/TailscaleCli.hpp). The per-platform answers are plain functions of a
// Platform value, so every platform's table is checked from any machine; the process runner only
// exists on POSIX and is driven here with ordinary system programs (no shell involved).

#include <catch2/catch.hpp>

#include "slic3r/Utils/TailscaleCli.hpp"

#include <set>
#include <string>
#include <vector>

using namespace Slic3r::TailscaleCli;

TEST_CASE("the download page follows the platform", "[TailscaleCli]")
{
    REQUIRE(std::string(download_url(Platform::Windows)) == "https://tailscale.com/download/windows");
    REQUIRE(std::string(download_url(Platform::MacOS))   == "https://tailscale.com/download/mac");
    REQUIRE(std::string(download_url(Platform::Linux))   == "https://tailscale.com/download/linux");
}

TEST_CASE("the words for this computer follow the platform", "[TailscaleCli]")
{
    REQUIRE(std::string(this_computer(Platform::Windows)) == "this PC");
    REQUIRE(std::string(this_computer(Platform::MacOS))   == "this Mac");
    REQUIRE(std::string(this_computer(Platform::Linux))   == "this computer");
    REQUIRE(std::string(your_computer(Platform::MacOS))   == "your Mac");
    REQUIRE(std::string(sign_in_place(Platform::MacOS))   == "the menu bar");
    REQUIRE(std::string(sign_in_place(Platform::Windows)) == "the system tray");
}

TEST_CASE("candidate lists, in the order they are tried", "[TailscaleCli]")
{
    REQUIRE(exe_candidates(Platform::MacOS) == std::vector<std::string>{
        "/Applications/Tailscale.app/Contents/MacOS/Tailscale", "/usr/local/bin/tailscale", "/opt/homebrew/bin/tailscale" });
    REQUIRE(exe_candidates(Platform::Linux) == std::vector<std::string>{
        "/usr/bin/tailscale", "/usr/local/bin/tailscale", "/usr/sbin/tailscale", "/snap/bin/tailscale" });
    // Windows is exactly what the hub always did: <Program Files>\Tailscale\tailscale.exe.
    REQUIRE(exe_candidates(Platform::Windows, "D:\\Programs") == std::vector<std::string>{ "D:\\Programs\\Tailscale\\tailscale.exe" });
    REQUIRE(exe_candidates(Platform::Windows) == std::vector<std::string>{ "C:\\Program Files\\Tailscale\\tailscale.exe" });
}

TEST_CASE("pick_exe takes the first candidate that exists, else the bare name for PATH", "[TailscaleCli]")
{
    const auto only = [](std::set<std::string> have) {
        return [have](const std::string& p) { return have.count(p) > 0; };
    };
    // Everything present: the app bundle wins over the shim and Homebrew.
    REQUIRE(pick_exe(Platform::MacOS, "", only({ "/opt/homebrew/bin/tailscale", "/usr/local/bin/tailscale",
                                                  "/Applications/Tailscale.app/Contents/MacOS/Tailscale" })) ==
            "/Applications/Tailscale.app/Contents/MacOS/Tailscale");
    // No app bundle: the "Install CLI" shim, then Homebrew.
    REQUIRE(pick_exe(Platform::MacOS, "", only({ "/opt/homebrew/bin/tailscale", "/usr/local/bin/tailscale" })) == "/usr/local/bin/tailscale");
    REQUIRE(pick_exe(Platform::MacOS, "", only({ "/opt/homebrew/bin/tailscale" })) == "/opt/homebrew/bin/tailscale");
    // Linux: package manager before snap.
    REQUIRE(pick_exe(Platform::Linux, "", only({ "/snap/bin/tailscale", "/usr/sbin/tailscale" })) == "/usr/sbin/tailscale");
    REQUIRE(pick_exe(Platform::Linux, "", only({ "/snap/bin/tailscale" })) == "/snap/bin/tailscale");
    // A candidate from another platform never counts.
    REQUIRE(pick_exe(Platform::MacOS, "", only({ "/usr/bin/tailscale" })) == "tailscale");
    // Nothing installed anywhere we know: leave it to PATH.
    REQUIRE(pick_exe(Platform::Linux, "", only({})) == "tailscale");
    REQUIRE(pick_exe(Platform::Windows, "C:\\Program Files", only({})) == "tailscale");
    REQUIRE(pick_exe(Platform::Windows, "C:\\Program Files", only({ "C:\\Program Files\\Tailscale\\tailscale.exe" })) ==
            "C:\\Program Files\\Tailscale\\tailscale.exe");
}

#ifndef _WIN32

TEST_CASE("resolve_executable searches PATH without a shell", "[TailscaleCli]")
{
    // A name with a slash is used as given, found or not.
    REQUIRE(resolve_executable("/no/such/dir/tool", "/usr/bin") == "/no/such/dir/tool");
    REQUIRE(resolve_executable("", "/usr/bin").empty());
    // A bare name is looked up in the supplied PATH only; a directory or a missing file is not a hit.
    REQUIRE(resolve_executable("echo", "/no/such/dir:/bin:/usr/bin").find("/echo") != std::string::npos);
    REQUIRE(resolve_executable("echo", "/no/such/dir").empty());
    REQUIRE(resolve_executable("definitely-not-a-program-xyz", "/bin:/usr/bin").empty());
    REQUIRE(resolve_executable("bin", "/").empty()); // /bin is a directory (or symlink to one), not a program
    // Shell syntax in a name is just a name that does not exist.
    REQUIRE(resolve_executable("echo;true", "/bin:/usr/bin").empty());
}

TEST_CASE("run_capture_posix captures stdout and the exit status", "[TailscaleCli]")
{
    std::string out;
    int         code = -1;
    REQUIRE(run_capture_posix({ "/bin/echo", "hello", "world" }, out, code, 10000));
    REQUIRE(code == 0);
    REQUIRE(out == "hello world\n");
}

TEST_CASE("run_capture_posix does not interpret its arguments", "[TailscaleCli]")
{
    std::string out;
    int         code = -1;
    REQUIRE(run_capture_posix({ "/bin/echo", "$HOME", "a;b", "`id`", "*" }, out, code, 10000));
    REQUIRE(code == 0);
    REQUIRE(out == "$HOME a;b `id` *\n");
}

TEST_CASE("run_capture_posix merges stderr and reports a non-zero exit", "[TailscaleCli]")
{
    std::string out;
    int         code = 0;
    REQUIRE(run_capture_posix({ "/bin/ls", "/definitely/not/a/real/path" }, out, code, 10000));
    REQUIRE(code != 0);
    REQUIRE(out.find("/definitely/not/a/real/path") != std::string::npos); // ls names it on stderr
}

TEST_CASE("run_capture_posix finds a bare program name on PATH", "[TailscaleCli]")
{
    std::string out;
    int         code = -1;
    REQUIRE(run_capture_posix({ "echo", "via-path" }, out, code, 10000));
    REQUIRE(code == 0);
    REQUIRE(out == "via-path\n");
}

TEST_CASE("run_capture_posix says false for a program that is not there", "[TailscaleCli]")
{
    std::string out = "stale";
    int         code = 7;
    REQUIRE_FALSE(run_capture_posix({ "/definitely/not/installed/tailscale", "status" }, out, code, 10000));
    REQUIRE(out.empty());
    REQUIRE(code == -1);
    REQUIRE_FALSE(run_capture_posix({ "definitely-not-installed-tailscale-xyz", "status" }, out, code, 10000));
    REQUIRE_FALSE(run_capture_posix({}, out, code, 10000));
}

TEST_CASE("run_capture_posix says false for a file that cannot be executed", "[TailscaleCli]")
{
    // /etc/hosts exists but is not executable: exec fails in the child and is reported, not run as exit 127.
    std::string out;
    int         code = 0;
    REQUIRE_FALSE(run_capture_posix({ "/etc/hosts" }, out, code, 10000));
}

TEST_CASE("run_capture_posix kills a child that outlives the timeout and reaps it", "[TailscaleCli]")
{
    std::string out;
    int         code = 0;
    const auto  t0   = std::chrono::steady_clock::now();
    REQUIRE_FALSE(run_capture_posix({ "/bin/sleep", "30" }, out, code, 300));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(ms < 5000);
    // Reaped: no child of ours is left to wait for.
    int status = 0;
    REQUIRE(::waitpid(-1, &status, WNOHANG) == -1);
    REQUIRE(errno == ECHILD);
}

TEST_CASE("run_capture_posix cannot be wedged by a child that never stops talking", "[TailscaleCli]")
{
    std::string out;
    int         code = 0;
    const auto  t0   = std::chrono::steady_clock::now();
    REQUIRE_FALSE(run_capture_posix({ "/usr/bin/yes" }, out, code, 400, /*max_out*/ 64 * 1024));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(ms < 5000);
    REQUIRE(out.size() == 64 * 1024); // read and kept up to the cap, the rest drained and dropped
}

TEST_CASE("run_capture_posix gives the child a closed stdin and no leaked descriptors", "[TailscaleCli]")
{
    // /bin/ls /proc/self/fd (Linux) lists what the child itself holds open: 0, 1, 2 and ls's own
    // directory handle, never a descriptor of ours. macOS has /dev/fd, which shows the same.
    const char* dir = ::access("/proc/self/fd", R_OK) == 0 ? "/proc/self/fd" : "/dev/fd";
    const int   base = ::open("/dev/null", O_RDONLY);
    const int   leak = ::fcntl(base, F_DUPFD, 100); // deliberately without close-on-exec, and not a number ls itself would pick
    REQUIRE(leak >= 100);
    std::string out;
    int         code = -1;
    REQUIRE(run_capture_posix({ "/bin/ls", dir }, out, code, 10000));
    REQUIRE(code == 0);
    REQUIRE(out.find("\n" + std::to_string(leak) + "\n") == std::string::npos);
    // No byte read from stdin: a program that reads it sees end of file straight away.
    std::string cat_out;
    REQUIRE(run_capture_posix({ "/bin/cat" }, cat_out, code, 10000));
    REQUIRE(code == 0);
    REQUIRE(cat_out.empty());
    ::close(leak);
    ::close(base);
}

#endif // !_WIN32
