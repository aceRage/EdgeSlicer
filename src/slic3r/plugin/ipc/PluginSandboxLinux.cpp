#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "PluginSandboxPriv.hpp"

#include <slic3r/plugin/PluginTrust.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>

#ifndef __linux__

namespace Slic3r::plugin_ipc {

bool apply_linux_sandbox(const SandboxSpec&, SandboxReport& report, std::string& error)
{
    error         = "Landlock+seccomp is Linux-only";
    report.kind   = k_sandbox_kind_none;
    report.detail = error;
    return false;
}

bool probe_linux_sandbox(SandboxProbe& probe)
{
    probe.available = false;
    probe.reason    = "not Linux";
    return false;
}

} // namespace Slic3r::plugin_ipc

#else

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/types.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(__has_include)
#if __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#define EDGE_HAS_LANDLOCK_UAPI 1
#endif
#endif

#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif
#ifndef LANDLOCK_RULE_PATH_BENEATH
#define LANDLOCK_RULE_PATH_BENEATH 1
#endif

#ifndef LANDLOCK_ACCESS_FS_EXECUTE
#define LANDLOCK_ACCESS_FS_EXECUTE (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR (1ULL << 3)
#define LANDLOCK_ACCESS_FS_REMOVE_DIR (1ULL << 4)
#define LANDLOCK_ACCESS_FS_REMOVE_FILE (1ULL << 5)
#define LANDLOCK_ACCESS_FS_MAKE_CHAR (1ULL << 6)
#define LANDLOCK_ACCESS_FS_MAKE_DIR (1ULL << 7)
#define LANDLOCK_ACCESS_FS_MAKE_REG (1ULL << 8)
#define LANDLOCK_ACCESS_FS_MAKE_SOCK (1ULL << 9)
#define LANDLOCK_ACCESS_FS_MAKE_FIFO (1ULL << 10)
#define LANDLOCK_ACCESS_FS_MAKE_BLOCK (1ULL << 11)
#define LANDLOCK_ACCESS_FS_MAKE_SYM (1ULL << 12)
#endif
#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_ACCESS_FS_IOCTL_DEV
#define LANDLOCK_ACCESS_FS_IOCTL_DEV (1ULL << 15)
#endif
#ifndef LANDLOCK_ACCESS_NET_BIND_TCP
#define LANDLOCK_ACCESS_NET_BIND_TCP (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP (1ULL << 1)
#endif
#ifndef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
#define LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET (1ULL << 0)
#define LANDLOCK_SCOPE_SIGNAL (1ULL << 1)
#endif
#ifndef __X32_SYSCALL_BIT
#define __X32_SYSCALL_BIT 0x40000000u
#endif
#ifndef __NR_io_uring_setup
#if defined(__x86_64__) || defined(__aarch64__)
#define __NR_io_uring_setup 425
#define __NR_io_uring_enter 426
#define __NR_io_uring_register 427
#endif
#endif

#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#define SYS_landlock_add_rule 445
#define SYS_landlock_restrict_self 446
#endif

#ifndef SECCOMP_SET_MODE_FILTER
#define SECCOMP_SET_MODE_FILTER 1
#endif
#ifndef SECCOMP_MODE_FILTER
#define SECCOMP_MODE_FILTER 2
#endif
#ifndef SECCOMP_RET_ALLOW
#define SECCOMP_RET_KILL_THREAD 0x00000000U
#define SECCOMP_RET_ERRNO 0x00050000U
#define SECCOMP_RET_ALLOW 0x7fff0000U
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif

struct landlock_ruleset_attr_min
{
    __u64 handled_access_fs;
    __u64 handled_access_net;
    __u64 scoped;
};

struct landlock_path_beneath_attr_min
{
    __u64 allowed_access;
    __s32 parent_fd;
};

namespace Slic3r::plugin_ipc {
namespace {

int landlock_abi()
{
    const long abi = ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0)
        return 0;
    return static_cast<int>(abi);
}

__u64 fs_handled_for_abi(int abi)
{
    __u64 fs = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
               LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
               LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
               LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2)
        fs |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3)
        fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi >= 5)
        fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    return fs;
}

__u64 net_handled_for_abi(int abi)
{
    if (abi >= 4)
        return LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
    return 0;
}

__u64 scoped_for_abi(int abi)
{
    if (abi >= 6)
        return LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
    return 0;
}

size_t ruleset_attr_size(const landlock_ruleset_attr_min& attr)
{
    if (attr.scoped)
        return sizeof(attr);
    if (attr.handled_access_net)
        return offsetof(landlock_ruleset_attr_min, scoped);
    return sizeof(attr.handled_access_fs);
}

__u64 fs_read_access(int abi)
{
    __u64 a = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE;
    if (abi >= 2)
        a |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 5)
        a |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    return a & fs_handled_for_abi(abi);
}

__u64 fs_write_access(int abi)
{
    __u64 a = fs_read_access(abi) | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
              LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_MAKE_SOCK |
              LANDLOCK_ACCESS_FS_MAKE_FIFO;
    if (abi >= 3)
        a |= LANDLOCK_ACCESS_FS_TRUNCATE;
    return a & fs_handled_for_abi(abi);
}

bool add_path_rule(int ruleset, const std::string& path, __u64 access, bool required, std::string& error)
{
    if (path.empty())
        return !required;
    const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) {
        if (!required) {
            BOOST_LOG_TRIVIAL(warning) << "Landlock: skipped unreadable path " << path << ": " << std::strerror(errno);
            return true;
        }
        error = "Landlock: cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat st
    {};
    if (::fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) {
        // Directory-only bits (READ_DIR, MAKE_*, REMOVE_*, REFER) EINVAL on a file.
        access &= LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_TRUNCATE |
                  LANDLOCK_ACCESS_FS_IOCTL_DEV;
    }
    landlock_path_beneath_attr_min beneath{};
    beneath.allowed_access = access;
    beneath.parent_fd      = fd;
    const long rc          = ::syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &beneath, 0);
    const int  saved       = errno;
    ::close(fd);
    if (rc < 0) {
        if (!required) {
            BOOST_LOG_TRIVIAL(warning) << "Landlock: skipped add_rule(" << path << "): " << std::strerror(saved);
            return true;
        }
        error = "Landlock: add_rule(" + path + ") failed: " + std::strerror(saved);
        return false;
    }
    return true;
}

std::string make_private_tmp()
{
    char tmpl[] = "/tmp/edge-plugin-XXXXXX";
    if (::mkdtemp(tmpl) == nullptr)
        return {};
    ::setenv("TMPDIR", tmpl, 1);
    ::setenv("TEMP", tmpl, 1);
    ::setenv("TMP", tmpl, 1);
    ::setenv("PYTHONPYCACHEPREFIX", tmpl, 1);
    return tmpl;
}

#if defined(__x86_64__)
#define EDGE_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define EDGE_AUDIT_ARCH AUDIT_ARCH_AARCH64
#else
#define EDGE_AUDIT_ARCH 0
#endif

// Deny-list seccomp-bpf. Python keeps its usual syscalls; the listed ones return EPERM.
bool install_seccomp(bool allow_network, bool allow_exec, std::string& error)
{
#if EDGE_AUDIT_ARCH == 0
    error = "seccomp: unsupported architecture";
    return false;
#else
    const uint32_t           deny = SECCOMP_RET_ERRNO | (EPERM & 0xffff);
    std::vector<sock_filter> f;
    auto                     emit = [&f](uint16_t code, uint8_t jt, uint8_t jf, uint32_t k) {
        sock_filter insn{};
        insn.code = code;
        insn.jt   = jt;
        insn.jf   = jf;
        insn.k    = k;
        f.push_back(insn);
    };

    // ld arch; jeq native, 1, 0; ret KILL; ld nr
    emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, arch));
    emit(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, EDGE_AUDIT_ARCH);
    emit(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS);
    emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, nr));
#ifdef __x86_64__
    // x32 syscalls set __X32_SYSCALL_BIT; the native table does not apply. Kill them.
    emit(BPF_JMP | BPF_JGE | BPF_K, 0, 1, __X32_SYSCALL_BIT);
    emit(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS);
#endif

    auto deny_nr = [&](int nr) {
        emit(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<uint32_t>(nr));
        emit(BPF_RET | BPF_K, 0, 0, deny);
    };

#ifdef __NR_ptrace
    deny_nr(__NR_ptrace);
#endif
#ifdef __NR_process_vm_readv
    deny_nr(__NR_process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
    deny_nr(__NR_process_vm_writev);
#endif
#ifdef __NR_mount
    deny_nr(__NR_mount);
#endif
#ifdef __NR_umount2
    deny_nr(__NR_umount2);
#endif
#ifdef __NR_pivot_root
    deny_nr(__NR_pivot_root);
#endif
#ifdef __NR_fsopen
    deny_nr(__NR_fsopen);
#endif
#ifdef __NR_fsconfig
    deny_nr(__NR_fsconfig);
#endif
#ifdef __NR_fsmount
    deny_nr(__NR_fsmount);
#endif
#ifdef __NR_move_mount
    deny_nr(__NR_move_mount);
#endif
#ifdef __NR_open_tree
    deny_nr(__NR_open_tree);
#endif
#ifdef __NR_unshare
    deny_nr(__NR_unshare);
#endif
#ifdef __NR_setns
    deny_nr(__NR_setns);
#endif
#ifdef __NR_init_module
    deny_nr(__NR_init_module);
#endif
#ifdef __NR_finit_module
    deny_nr(__NR_finit_module);
#endif
#ifdef __NR_delete_module
    deny_nr(__NR_delete_module);
#endif
#ifdef __NR_kexec_load
    deny_nr(__NR_kexec_load);
#endif
#ifdef __NR_kexec_file_load
    deny_nr(__NR_kexec_file_load);
#endif
#ifdef __NR_bpf
    deny_nr(__NR_bpf);
#endif
#ifdef __NR_userfaultfd
    deny_nr(__NR_userfaultfd);
#endif
#ifdef __NR_perf_event_open
    deny_nr(__NR_perf_event_open);
#endif
#ifdef __NR_io_uring_setup
    deny_nr(__NR_io_uring_setup);
#endif
#ifdef __NR_io_uring_enter
    deny_nr(__NR_io_uring_enter);
#endif
#ifdef __NR_io_uring_register
    deny_nr(__NR_io_uring_register);
#endif
#ifdef __NR_setsid
    deny_nr(__NR_setsid);
#endif
#ifdef __NR_setpgid
    deny_nr(__NR_setpgid);
#endif
#ifdef __NR_setpgrp
    deny_nr(__NR_setpgrp);
#endif

    if (!allow_exec) {
#ifdef __NR_execve
        deny_nr(__NR_execve);
#endif
#ifdef __NR_execveat
        deny_nr(__NR_execveat);
#endif
    }

#if defined(__NR_ioctl) && defined(TIOCSTI) && defined(TIOCLINUX)
    // M1: no typing into a terminal (TIOCSTI, on kernels that still allow it) and no console
    // control (TIOCLINUX), whatever fd the child got hold of.
    emit(BPF_JMP | BPF_JEQ | BPF_K, 0, 6, static_cast<uint32_t>(__NR_ioctl));
    emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[1]));
    emit(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<uint32_t>(TIOCSTI));
    emit(BPF_RET | BPF_K, 0, 0, deny);
    emit(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<uint32_t>(TIOCLINUX));
    emit(BPF_RET | BPF_K, 0, 0, deny);
    emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, nr));
#endif

#ifdef __NR_socketpair
    // socketpair(AF_UNIX, SOCK_STREAM) stays: asyncio and the inherited IPC fd. SOCK_DGRAM
    // pairs can sendto() journald; restrict the type. AF_UNIX socket() is still denied.
    emit(BPF_JMP | BPF_JEQ | BPF_K, 0, 8, static_cast<uint32_t>(__NR_socketpair));
    emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[0]));
    emit(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, static_cast<uint32_t>(AF_UNIX));
    emit(BPF_RET | BPF_K, 0, 0, deny);
    emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[1]));
    emit(BPF_ALU | BPF_AND | BPF_K, 0, 0, 0xfu);
    emit(BPF_JMP | BPF_JEQ | BPF_K, 1, 0, static_cast<uint32_t>(SOCK_STREAM));
    emit(BPF_RET | BPF_K, 0, 0, deny);
    emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, nr));
#endif

    if (!allow_network) {
#ifdef __NR_socket
        deny_nr(__NR_socket);
#endif
#ifdef __NR_connect
        deny_nr(__NR_connect);
#endif
#ifdef __NR_bind
        deny_nr(__NR_bind);
#endif
#ifdef __NR_listen
        deny_nr(__NR_listen);
#endif
#ifdef __NR_accept
        deny_nr(__NR_accept);
#endif
#ifdef __NR_accept4
        deny_nr(__NR_accept4);
#endif
    } else {
#ifdef __NR_socket
        // Network grant is inet. AF_UNIX would still reach the session bus / X11 / portals.
        emit(BPF_JMP | BPF_JEQ | BPF_K, 0, 4, static_cast<uint32_t>(__NR_socket));
        emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, args[0]));
        emit(BPF_JMP | BPF_JEQ | BPF_K, 0, 1, static_cast<uint32_t>(AF_UNIX));
        emit(BPF_RET | BPF_K, 0, 0, deny);
        emit(BPF_LD | BPF_W | BPF_ABS, 0, 0, offsetof(struct seccomp_data, nr));
#endif
    }

    emit(BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW);

    sock_fprog prog{};
    prog.len    = static_cast<unsigned short>(f.size());
    prog.filter = f.data();
    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
        error = std::string("seccomp filter failed: ") + std::strerror(errno);
        return false;
    }
    return true;
#endif
}

} // namespace

bool probe_linux_sandbox(SandboxProbe& probe)
{
    probe.landlock_abi  = landlock_abi();
    probe.landlock      = probe.landlock_abi > 0;
    probe.seccomp       = true;
    probe.bwrap_on_path = ::access("/usr/bin/bwrap", X_OK) == 0;
    if (!probe.landlock) {
        probe.available = false;
        probe.reason    = "Landlock is unavailable (kernel older than 5.13, or the syscall is disabled)";
        return false;
    }
    probe.available = true;
    probe.reason    = "Landlock ABI " + std::to_string(probe.landlock_abi) + " + seccomp-bpf";
    if (probe.inside_flatpak)
        probe.reason += "; inside Flatpak (no nested bwrap)";
    return true;
}

bool apply_linux_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error)
{
    report              = {};
    const int abi       = landlock_abi();
    report.landlock_abi = abi;
    if (abi <= 0) {
        error         = "Landlock is unavailable; refusing to start the plugin host unsandboxed";
        report.kind   = k_sandbox_kind_none;
        report.detail = error;
        return false;
    }

    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        error         = std::string("PR_SET_NO_NEW_PRIVS failed: ") + std::strerror(errno);
        report.detail = error;
        return false;
    }

    landlock_ruleset_attr_min attr{};
    attr.handled_access_fs  = fs_handled_for_abi(abi);
    attr.handled_access_net = spec.allow_network ? 0 : net_handled_for_abi(abi);
    attr.scoped             = scoped_for_abi(abi);
    const size_t attr_size  = ruleset_attr_size(attr);
    const int    ruleset    = static_cast<int>(::syscall(SYS_landlock_create_ruleset, &attr, attr_size, 0));
    if (ruleset < 0) {
        error         = std::string("landlock_create_ruleset failed: ") + std::strerror(errno);
        report.detail = error;
        return false;
    }

    const __u64 read_acc  = fs_read_access(abi);
    const __u64 write_acc = fs_write_access(abi);

    auto add_req = [&](const std::string& path, bool write) {
        return add_path_rule(ruleset, path, write ? write_acc : read_acc, /*required=*/true, error);
    };
    auto add_opt = [&](const std::string& path, bool write) {
        return add_path_rule(ruleset, path, write ? write_acc : read_acc, /*required=*/false, error);
    };

    const std::string tmp = make_private_tmp();
    if (tmp.empty() || !add_req(tmp, true)) {
        ::close(ruleset);
        if (error.empty())
            error = "failed to create a private TMPDIR for the plugin host";
        report.detail = error;
        return false;
    }

    // System trees CPython and libpython need. Read+execute only. Do not grant all of
    // /proc (other pids' environ) or /run (session bus sockets).
    for (const char* p : {"/usr", "/lib", "/lib64", "/lib32", "/opt", "/etc", "/sys"})
        add_opt(p, false);
    // M1: only the harmless devices, never all of /dev (terminals, /dev/input, /dev/video*,
    // /dev/hidraw* for users in those groups). /dev/null is also writable.
    for (const char* p : {"/dev/zero", "/dev/random", "/dev/urandom"})
        add_opt(p, false);
    add_opt("/dev/null", true);
    for (const char* p : {"/proc/self", "/proc/cpuinfo", "/proc/meminfo", "/proc/stat", "/proc/filesystems", "/proc/version"})
        add_opt(p, false);
    // systemd-resolved: /etc/resolv.conf is often a symlink into this directory.
    add_opt("/run/systemd/resolve", false);

    auto add_req_if_exists = [&](const std::string& path, bool write) {
        if (path.empty() || ::access(path.c_str(), F_OK) != 0)
            return true;
        return add_req(path, write);
    };

    // H5: trust.json is not granted; the child checks the plugin tree against the hash Edge
    // sent with the load (plugin_trust::set_parent_verified_hash).
    (void) add_req_if_exists;

    if (!spec.python_home.empty() && !add_req(spec.python_home, false)) {
        ::close(ruleset);
        report.detail = error;
        return false;
    }
    // Plugin dir is read-only: a writable root plus a skipped child rehash would let the
    // plugin rewrite itself for the rest of this host. Scratch lives in plugin_data / TMPDIR.
    if (!spec.plugin_root.empty() && !add_opt(spec.plugin_root, false)) {
        ::close(ruleset);
        report.detail = error;
        return false;
    }
    if (!spec.write_root.empty() && !add_req(spec.write_root, true)) {
        ::close(ruleset);
        report.detail = error;
        return false;
    }
    for (const SandboxPath& extra : spec.extra_paths)
        add_opt(extra.path, extra.write);

    if (::syscall(SYS_landlock_restrict_self, ruleset, 0) != 0) {
        error = std::string("landlock_restrict_self failed: ") + std::strerror(errno);
        ::close(ruleset);
        report.detail = error;
        return false;
    }
    ::close(ruleset);

    if (!install_seccomp(spec.allow_network, spec.allow_exec, error)) {
        report.detail = error;
        return false;
    }

    report.applied  = true;
    report.kind     = k_sandbox_kind_landlock_seccomp;
    report.network  = spec.allow_network;
    report.exec     = spec.allow_exec;
    report.degraded = abi < 6;
    report.detail   = "Landlock ABI " + std::to_string(abi) + (spec.allow_network ? " network=on" : " network=off");
    if (abi < 3)
        report.detail += "; truncated files unenforced (ABI<3)";
    if (abi < 6)
        report.detail += "; no abstract-unix/signal scopes (ABI<6)";
    return true;
}

} // namespace Slic3r::plugin_ipc

#endif
