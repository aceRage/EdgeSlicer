# Phase 8 — OS sandbox profiles

Revision: Edge `feature/plugins-p8-os-sandbox`.
Plan: plugins plan section 6 (OS sandbox) and Phase 8 in section 10. Builds on Phase 7
(`docs/plugins/phase7-ipc-host.md`). Catalog publishing is Phase 11. Printer-agent hooks stay
unwired.

All of this is compiled only when `EDGE_PYTHON_PLUGINS` is ON (default OFF). `libslic3r` is
not given that define. With it OFF, G-code is byte-identical to today.

## What was built

`edge-plugin-host` still runs `script` and `psGCodePostProcess` over the v1 length-prefixed JSON
channel. Phase 8 wraps that child in an OS sandbox that matches the plugin's Phase 4 grants:

| Platform | Shipping sandbox | Applied where |
|---|---|---|
| Linux | Landlock (filesystem + ABI4 TCP net + ABI6 abstract-unix/signal scopes) + seccomp-bpf (ptrace/mount/unshare/modules/bpf/io_uring/setsid/execve; `socket`/`connect`/`bind`/`listen`/`accept` when network is off; `AF_UNIX` `socket()` always) | Child, after CPython starts, before `load` / `execute`. Inherited by grandchildren. |
| Windows | AppContainer (no capabilities unless network was granted) + the Phase 7 Job object (`KILL_ON_JOB_CLOSE`, `ACTIVE_PROCESS=1`, no `BREAKAWAY_OK`) | Parent: `CreateAppContainerProfile`, ACLs on the plugin dir / bundled Python / G-code folder, `SECURITY_CAPABILITIES` on `STARTUPINFOEX`. Create suspended, assign job, resume. |
| macOS | Signed helper with `com.apple.security.app-sandbox` (`scripts/edge-plugin-host.entitlements`) | Code signature. Ad-hoc and Developer ID signing hooks live in `src/CMakeLists.txt`, `scripts/macos_sign_app.sh`, `scripts/sign_and_package.sh`, and `build_release_macos.sh`. |

bubblewrap is optional for **native** Linux packages only and is off unless
`EDGE_PLUGIN_SANDBOX_BWRAP=1` is set through the Phase 7 test env-override gate.
**Flatpak must never nest bwrap** (`running_inside_flatpak()` / `/.flatpak-info` / `FLATPAK_ID`).
Inside Flatpak the child still uses Landlock+seccomp.

`sandbox-exec` / Seatbelt is a **dev/test probe only**, labelled `seatbelt-probe`. The parent
honours `EDGE_PLUGIN_SANDBOX_SEATBELT_PROBE=1` through the Phase 7 test-only env gate and copies
`dev_seatbelt_probe` into the hello spec. The child applies the probe from that field, not from
raw environment (so a release helper cannot be switched by exporting the variable). It is not
the shipping macOS design and is **not** auto-enabled for unsigned dev builds: named profiles
`"no-internet"` / `"no-write"` are not grant-aware, so they cannot stand in for App Sandbox.
Unsigned macOS execute tests store a per-plugin `trust.json` `unsandboxed` override instead
(the same Run anyway record the prompt writes). Isolation tests skip when the shipping sandbox
is not applied.

The probe `dlsym`s `sandbox_init` and applies an IPC-safe SBPL (`(allow default)` plus a
deny of `/var/empty`). It does **not** apply named `"no-internet"` / `"no-write"`: those
deny AF_UNIX or writes on the handshake socket, so `hello_ok` never arrived. It does not
use `kSBXProfileNoInternet` / `kSBXProfileNoWrite` (removed in current MacOSX.sdk). A
missing symbol or failed `sandbox_init` is fail-closed (`kind=none` in `hello_ok`; the child
does not abort the handshake). The Catch2 probe case captures child stderr/exit and SKIPs
when the probe cannot be confirmed, instead of REQUIREing a reply that never comes.
`scripts/check_macos_sandbox_sdk.sh` syntax-checks this against a stub SDK (bash 3.2-safe)
so Linux CI can catch the Xcode 27.2 break.

## Handshake

`hello` may carry a `sandbox` object (plugin dir, Python home, extra grant paths, network/exec).
`hello_ok` always carries `sandbox: {applied, kind, detail, landlock_abi, network, exec, degraded}`.

`kind` is one of `landlock+seccomp`, `appcontainer`, `app-sandbox`, `seatbelt-probe`, `none`.
Only the first three are shipping kinds. `seatbelt-probe` with `applied=true` still fail-closes
(the Don't-run prompt) so a labelled probe cannot skip isolation.

`degraded` is true on Linux Landlock ABI < 3 (no `truncate`). The sandbox still applies.

Edge refuses to run the workload when the child reports `applied=false` or a non-shipping
`kind`, unless the user has a stored override (see below) or a test-only env override is set.

On macOS the parent does **not** prompt from `probe_os_sandbox()` before spawn: Edge itself is
not in the helper's App Sandbox (`sandbox_check` in the parent is always false, and
`APP_SANDBOX_CONTAINER_ID` is spoofable). The child reports `kind=app-sandbox` via
`sandbox_check(getpid(), NULL, 0)` after exec of a signed helper.

The helper is respawned when the sandbox spec changes (different plugin or a different G-code
write root), so Landlock rules never have to be loosened.

The child re-hashes the plugin tree on `load`. `plugin_root` is read-only in the OS sandbox
(writable scratch is `plugin_data` plus a private `TMPDIR` / `PYTHONPYCACHEPREFIX`), and
`trust.json` is granted as a file, so the pin matches the Edge-side check. Host RPC still
rechecks p5 on the Edge side.
A sandbox-unavailable execute is reported as `OS sandbox unavailable; plugin did not run`, not
as untrusted. The Trust_Echo "untrusted" log on unsigned macOS was the existing unpin-and-refuse
path after Catch2 `CHECK` continued past a refused execute.

## Fail-closed policy

If the OS sandbox cannot be applied (Landlock ABI 0 / kernel older than 5.13, AppContainer APIs
missing, unsigned macOS helper, and so on) Edge **does not** silently fall back.

- Prompt per plugin. Default button is **Don't run**. **Run anyway** is the other button.
- Choosing Run anyway records `{plugin_key, sha256}` on `trust.json` as `unsandboxed` next to
  the pin allow-list (a legacy string-only key is ignored). An update of the plugin tree drops
  the override. The Home card shows **Running unsandboxed** and a **Revoke unsandboxed run**
  menu item (`plugin_clear_unsandboxed`).
- The prompt is not held across `io_mutex`. Shutdown aborts a pending `CallAfter` wait with
  Don't run (`broken_promise` included) so quitting with the dialog up does not hang.
- Early slicing-pipeline steps (`posSlice` … `psSkirtBrim`) stay in-process with a live `Print *`.
  They do **not** get this prompt. The Home card already says "Early slicing steps run
  unsandboxed in-process".

Test-only env overrides, ignored in production (same gate as Phase 7's `EDGE_PLUGIN_HOST`):

| Env | Effect |
|---|---|
| `EDGE_PLUGIN_SANDBOX=0` | Do not apply the OS sandbox; report `kind=none`. |
| `EDGE_PLUGIN_SANDBOX_FORCE_UNAVAILABLE=1` | Pretend the OS cannot sandbox, so the prompt path runs. |
| `EDGE_PLUGIN_SANDBOX_SEATBELT_PROBE=1` | macOS: labelled Seatbelt probe (`sandbox_init` via `dlsym`) instead of requiring a signed container. |
| `EDGE_PLUGIN_SANDBOX_BWRAP=1` | Native Linux only; ignored inside Flatpak. Shipping path remains Landlock+seccomp. |

## Grants mapped into the sandbox

Always allowed inside the child:

- bundled Python home (read)
- the plugin's own directory (**read-only**; `__pycache__` goes to the private TMPDIR;
  wheel extract goes to `plugin_data/<plugin_key>/__whl_extracted__`)
- a private `TMPDIR` (`/tmp/edge-plugin-XXXXXX`)
- helper directory / shared `python/packages` / `resources_dir` (read)
- `trust.json` (the sidecar file only, not all of `data_dir`; Landlock file
  rules drop directory-only bits so `add_rule` is not EINVAL)
- the plugin's `plugin_data` storage directory (read/write)
- on Linux: `/usr`, `/lib`, `/lib64`, `/etc`, `/dev`, `/sys`, `/proc/self` plus a few
  `/proc` files (`cpuinfo`, `meminfo`, `stat`, `filesystems`, `version`). Not all of
  `/proc` or `/run` (session bus sockets).

`socketpair(AF_UNIX)` stays allowed (asyncio; already-connected pairs). `socket(AF_UNIX)`
is denied even when network is granted, so a plugin cannot open the session D-Bus, X11
abstract sockets, or the Flatpak portal. Landlock ABI ≥ 6 also sets
`LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL`. `io_uring_*` is always
denied (`IORING_OP_SOCKET` would skip the network deny). x32 syscalls (`nr & 0x40000000`)
are killed. `setsid` is denied so `kill(-pgid)` cannot be escaped.

From the plugin's Phase 4 sidecar:

- `fs_read` / `fs_readwrite` paths (tree `/*` markers stripped)
- network (`network_http` / `network_socket`) → Windows `internetClient` capability; Linux
  seccomp allows non-AF_UNIX `socket()` (inet). When network is not granted, Landlock ABI ≥ 4
  also handles TCP bind/connect with no rules added (so those are denied) and seccomp denies
  `socket`/`connect`/`bind`/`listen`/`accept` (UDP included, because no `socket()`).
- process grants → `allow_exec` (otherwise Linux seccomp denies `execve`/`execveat`; Windows
  `ACTIVE_PROCESS=1` already blocks children)

A `full_access` Python audit grant does **not** widen the OS sandbox.

## Grandchildren

- Linux: `setpgid` in parent and child, `kill(-pid, SIGKILL)` on host kill, `setsid` denied.
  Edge is **not** a subreaper (no reap path; orphans would become Edge zombies). Init reaps.
- Windows: the Job object kills every assigned process; `ACTIVE_PROCESS=1` stops them spawning.
  AppContainer profile names are a fixed-length FNV hash of `plugin_key` (no truncation
  collisions). Directory ACLs are granted at the folder with inheritance (not a recursive
  walk), checked fail-closed, and revoked on `close_win_appcontainer`. `grant_dir_acl`
  appends a package-SID ACE onto the existing DACL, which already allows the creating
  user. The IPC named-pipe DACL is
  `D:(A;;GA;;;<package SID>)(A;;GA;;;<TokenUser>)(A;;GA;;;SY)S:(ML;;NW;;;LW)` — not BA,
  because a non-elevated token has Administrators as deny-only and the AppContainer
  access check requires both the user/groups SID and the package SID (helper exit 1005
  otherwise). Job objects and OVERLAPPED events are unnamed (parent-only / assigned by
  handle). The 1 GiB job process memory cap is a named constant, not plugin-configurable.
- macOS / POSIX: process group kill (`kill(-pid)`). `setsid` / `setpgid` / `setpgrp` are
  denied after the Linux sandbox applies so grandchildren cannot leave the group. The helper
  exec env is an allow-list (no `DISPLAY` / `DBUS_*` / `XDG_RUNTIME_DIR` /
  `APP_SANDBOX_CONTAINER_ID` / `PYTHONPATH`). `LD_LIBRARY_PATH`, `APPDIR`, and `APPIMAGE`
  are passed through so an AppImage-started helper still finds bundled libs. Windows
  `CreateProcessW` uses the same allow-list (not `lpEnvironment=nullptr`). Windows also
  omits `APPDATA` / `LOCALAPPDATA` / `USERPROFILE` / `HOMEDRIVE` / `HOMEPATH` so CPython
  inside AppContainer cannot probe the real user profile. The helper opens
  `--host-log <write_root|data_dir/plugin_host>/edge-plugin-host.log` (stderr, faulthandler,
  `Py_FatalError`). Uncaught exceptions, `std::terminate`, abort, and the MSVC invalid-
  parameter handler exit 10–14 instead of a silent `0xc0000409`. The parent attaches
  `GetExitCodeProcess` plus the last ~40 log lines to closed-pipe handshake errors.
- Post-process G-code is read by Edge and sent as JSON `gcode` on the existing
  length-prefixed IPC (POSIX socketpair / Windows named pipe). The helper stages a scratch
  copy under `plugin_data` / TMPDIR, Python still sees `ctx.gcode_path`, and Edge writes
  the reply back. The hosted write root is that working-copy folder, not the export
  directory — sibling files next to the original G-code are not visible to the child.
  Security-scoped bookmarks are not used: Edge is not sandboxed and has a
  different identity from the helper, so a bookmark would not resolve. File-descriptor
  passing (`SCM_RIGHTS` / `DuplicateHandle`) is not on the Channel. Payloads over 16 MiB
  fail with a clear error rather than a silent truncate.
- Linux Landlock grants `/run/systemd/resolve` read-only so systemd-resolved DNS works
  when the plugin has network; `/run` as a whole stays denied. `socketpair(AF_UNIX)` is
  restricted to `SOCK_STREAM` so a `SOCK_DGRAM` pair cannot `sendto()` journald. `AF_UNIX`
  `socket()` stays denied. Landlock ABI < 6 reports `degraded` (missing abstract-unix and
  signal scopes).
- `.whl` plugin entries extract under `plugin_data/<key>/__whl_extracted__`, not
  read-only `plugin_root`.

## macOS entitlements

`scripts/edge-plugin-host.entitlements` enables App Sandbox, disables library validation
(bundled CPython), and adds:

- absolute-path read of `/usr/lib`, `/usr/libexec`, `/System`, `/Library/Frameworks`, `/dev`
- home-relative read of `Library/Application Support/EdgeSlicer/orca_plugins/`
- home-relative read/write of `Library/Application Support/EdgeSlicer/plugin_data/`

The helper's own bundle (bundled Python at `@executable_path/python`) is readable without
an extra exception. User G-code is not a helper filesystem grant. None of this is verified
without a signed helper showing `kind=app-sandbox`.

## What can only be verified on a signed/notarised bundle

- macOS App Sandbox actually restricting the helper (`sandbox_check`, `kind=app-sandbox`).
  Ad-hoc `codesign --sign - --entitlements scripts/edge-plugin-host.entitlements` is hooked
  up but does not go through notarisation. `APP_SANDBOX_CONTAINER_ID` is not trusted.
- Windows AppContainer + named-pipe DACL + Job object on a real Windows box. ACL grants
  live until host close (not revoked at `ResumeThread`). `python_home` under Program Files
  is a best-effort grant: a standard user has no `WRITE_DAC`, and ALL APPLICATION PACKAGES
  can already read it. A user-owned `python_home` (dev tree next to the helper) is
  fail-closed. The helper uses `PyConfig_InitIsolatedConfig` with explicit
  `module_search_paths` (no registry / user-site). Exceeding the 1 GiB job process-memory
  cap is a distinct error.
- A Linux **desktop session** D-Bus / Flatpak portal / X11 abstract-socket escape (the unit
  test uses a forked fixture plus `dbus-daemon --session` when that binary exists).
- The Don't-run / Run anyway GUI prompt (tests use the prompt seam).
- Flatpak: Landlock+seccomp inside the existing bwrap; no nested bwrap.
- AppImage: helper next to the binary, same Landlock+seccomp path as a native prefix.

## Smoke tests owed at merge

Linux `[PluginSandbox]` (probe, handshake, fs/net/exec isolation, fail-closed prompt +
`trust.json` override, Seatbelt IPC-safe SBPL / fail-closed helpers). Existing `[PluginIpc]`,
`[PluginTrust]`, Home plugin-filter tests. `scripts/check_macos_sandbox_sdk.sh` (stub
`<sandbox.h>`, `clang -fsyntax-only -Werror=deprecated-declarations`). Spawn+handshake timing
next to Phase 7 r5 (~0.275 s on Linux). U1 prints are still required for pipeline plugins.
A real Xcode 27.2 link is still owed; this VM cannot build macOS.
