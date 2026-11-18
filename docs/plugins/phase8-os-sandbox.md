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
- **Import is not sandboxed.** `load_enabled_plugins` (startup auto-load) and Home > Plugins
  Enable import a *trusted* plugin inside Edge with the user's rights. The OS sandbox covers
  helper **execute**, not import. Trust is the import-time control. See
  `docs/HLSD/plugin-permissions.md`.

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

- `fs_read` / `fs_readwrite` tree paths (`<dir>/*`, marker stripped) and exact files. A
  bare directory is an exact grant at both the audit layer and the OS layer; it does not
  cover children. Audit is the source of truth for that split.
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
  walk). Every **read-only** grant (helper dir, `resources_dir`, `python_home`, packages,
  `plugin_root`, extras) first checks whether the package half of the AppContainer access
  check already allows `FILE_GENERIC_READ|FILE_GENERIC_EXECUTE`: the DACL is walked for
  ALLOW ACEs on this package SID or ALL APPLICATION PACKAGES (`S-1-15-2-1`), which every
  AppContainer token includes as a group. If ALL APPLICATION PACKAGES already has RX,
  the grant is skipped with a warning and is **not** journaled (a Program Files
  install has no `WRITE_DAC`; we must not revoke `S-1-15-2-1`). If **this package
  SID** already has an ACE (a leftover from a crash), the grant is skipped but
  **journaled** so close/sweep can still remove it. Otherwise the ACE is applied
  and spawn fail-closes if that fails. There is no path-substring exemption. A
  missing `data_dir` skips the traverse ACE with a warning (the helper
  `set_data_dir` treats not-found as create). Each grant is journalled **before**
  it is applied, in a **parent-only** file at
  `{spec.data_dir or data_dir()}/plugin_host_acl/acl_journal.json`. The path is
  resolved once at grant time and stored on the session; close/revoke never
  re-reads the global (a test or caller that restores `data_dir()` before
  close would otherwise leave a journal with 0 ACEs). That directory is a
  sibling of the helper-writable `plugin_host` log dir and is never granted
  to the package SID or ALL APPLICATION PACKAGES; an explicit protected DACL
  allows only the creating user and SYSTEM. A missing `plugin_host_acl`
  folder is created; spawn fails only when neither `spec.data_dir` nor
  `data_dir()` is set. The helper must not read or write the journal
  (`PluginManager::initialize` skips the sweep in-process; journal functions
  no-op inside an AppContainer / `in_child_process`). There is no
  `%LOCALAPPDATA%` / `Packages/*/AC` copy — `SHGetKnownFolderPath` inside the
  helper would resolve to the container folder. Sweep accepts only SIDs that
  match a derived `EdgeSlicer.ph.*` / test `EdgeSlicer.t.*` profile (never
  `S-1-15-2-1`). ACE revoke and Low-label restore on a recorded grant path
  are allowed for that session's `python_home`, helper dir, `resources_dir`,
  and `data_dir` even when they sit outside this process's current roots
  (update, portable copy, test binary vs GUI). Copy-replace of Low-IL files
  from a journal session stays inside that session's write grants under this
  process's known user-rw roots (current `data_dir`, remembered spec data
  dirs, plugin dirs, `python_home`, helper, `resources_dir`, temp). A session
  whose grants were skipped or failed is rewritten with the remainder and
  kept; the journal is deleted only after every grant is removed. A leftover
  helper-writable `{data_dir}/plugin_host/acl_journal.json` is deleted unread.
  Permanent
  uninstall of that exact plugin key calls `DeleteAppContainerProfile`
  (`close_host` and test teardown never delete a pre-existing profile).
  Test-only plugin keys (`test.…`) use the `EdgeSlicer.t.*` moniker only when
  the test harness enables `set_test_only_plugin_keys_for_testing`; production
  hashes `test.*` to `EdgeSlicer.ph.*`. Real plugin ids starting with `test.`
  (case-insensitive) are rejected by `is_valid_plugin_id` / install.
  `ERROR_FILE_NOT_FOUND` / `ERROR_PATH_NOT_FOUND` on revoke is treated as
  already removed so a vanished temp dir cannot pin the journal forever. The
  journal is a list of sessions (package SID + profile name + session roots +
  grant time + owner pid/start + grants) and is deleted only after every ACE
  in every session is removed. Each session records the Edge process that
  granted it; sweep skips a session whose owner pid is still alive with the
  same process start time, so two instances sharing a `data_dir` do not
  revoke each other. Journal load-modify-write is serialized with a named
  mutex (`Local\EdgeSlicer.acl_journal.<hash>`). Parent
  `PluginManager::initialize` and CLI after `set_data_dir` (gcode-viewer /
  headless share that CLI sweep) revoke anything the journal still lists
  except the live session and sessions whose owner process is still alive.
  A same-user rewrite of the journal is not an escalation: the file is already
  parent-only to the interactive user, and forging grants can only name
  paths that user can already ACL. Sweep still requires a derived
  `EdgeSlicer.ph.*` / `EdgeSlicer.t.*` profile name (not a bare package SID).
  Each sweep revokes at most 64 leftover sessions / 512 grants and logs if
  it truncated. The next `prepare` continues the remainder. `close_win_appcontainer` revokes, restores the
  original SACL mandatory label on Edge-created work dirs **and** on user
  `fs_readwrite` roots that this process labelled, walks write-granted
  trees (bounded) to strip Low labels from files the Low-IL helper created
  (`edge-plugin-host.log`, `post_process.gcode`, plugin output in user
  `fs_readwrite`), and clears the journal. Hosted writes into `fs_readwrite`
  need package-SID modify plus a Low integrity label (AppContainer is Low IL
  and cannot write up). The parent adds that label only when the root is not
  already Low, journals the ACE and the prior label SDDL, and on close/sweep
  revokes the ACE and restores **only** a label it added — a folder that was
  already Low stays Low. Close/sweep never strip the grant root unless this
  process added the label, and they apply the grant-time filter before any
  label removal (not only copy-replace) so a pre-existing Low file or dir
  inside the tree keeps its label. The grant-time is the earliest stamp for
  that package SID / session and is not restamped on a later prepare.
  Copy-replace is never used to apply or restore a
  user folder's label. If `WRITE_OWNER` is missing, spawn fail-closes for
  that **user** `fs_readwrite` root with a clear error rather than leaving it
  read-only. Edge-owned work dirs (`plugin_host`, `plugin_data`, `write_root`)
  log a warning and continue so a Modify-only tree (no `WRITE_OWNER`) can still
  run hosted plugins. Changing a
  mandatory label on helper-created files still needs `WRITE_OWNER`; when
  that is missing the strip **logs** the `SetNamedSecurityInfo` error and
  copy-replaces only files created at/after the session grant time that are
  Low-labelled, have a single hard link, and have no alternate data streams
  (MOTW / Zone.Identifier). Pre-grant, hard-linked, or ADS files are left
  labelled. Copy-replace creates a Medium-IL temp with `CREATE_NEW` in the
  same directory, copies bytes (preserving timestamps and attributes), then
  `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)`. It never deletes the
  original first; if the move fails the original is kept and only the temp
  is removed. `ReplaceFile` is not used because it copies the Low label.
  A successful copy-replace is logged at warning. `FindFirstStreamW` /
  link-count `ERROR_ACCESS_DENIED` is logged as access denied, not as extra
  ADS: an empty DACL (only the inherited package-SID ACE, then revoked)
  makes those queries fail even when there are no streams. Write-granted
  directories also get an inheritable user/SYSTEM ACE, journaled and restored
  on close, so helper-created files under `plugin_host` and user
  `fs_readwrite` keep a user DACL after the package ACE is removed. Hosted
  G-code that Edge writes out of `plugin_data` is stripped of a Low label
  after the write.
  `apply_dir_acl`
  appends a package-SID ACE onto the existing DACL, which already allows the creating
  user. Subfolder grants do not make `data_dir` itself `GetFileAttributes`-able, so the
  parent also sets a non-inherited `FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE`
  ACE on `data_dir` (no list/read). `set_data_dir` uses `error_code` overloads so a
  denied `status()` cannot abort the helper, and `create_directories` so missing
  parents are still created (a missing path sets ENOENT on `exists(path, ec)`, which
  is the create case, not a denial). The IPC named-pipe DACL is
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
  `CreateProcessW` uses the same allow-list (not `lpEnvironment=nullptr`). Windows omits
  `APPDATA` / `USERPROFILE` / `HOMEDRIVE` / `HOMEPATH` so CPython cannot probe the real
  user profile, but keeps `LOCALAPPDATA`: CreateProcess with AppContainer
  `SECURITY_CAPABILITIES` fails with `ERROR_ENVVAR_NOT_FOUND` (203) without it. Only
  `SHGetKnownFolderPath(FOLDERID_LocalAppData)` is redirected inside the container; the
  env var still carries the real user path unless the parent rewrites it to
  `GetAppContainerFolderPath` (the Packages\<family>\AC folder) when that API succeeds.
  The helper opens
  `--host-log <write_root|data_dir/plugin_host>/edge-plugin-host.log` (stderr, faulthandler,
  `Py_FatalError`). Uncaught exceptions, `std::terminate`, abort, and the MSVC invalid-
  parameter handler exit 10–14 instead of a silent `0xc0000409`. The parent attaches
  `GetExitCodeProcess` plus the last ~40 helper-log lines (control chars stripped, length
  capped, each line prefixed `helper log:`) to closed-pipe handshake errors.
- Post-process G-code is read by Edge and sent as JSON `gcode` on the existing
  length-prefixed IPC (POSIX socketpair / Windows named pipe). The helper stages a scratch
  copy under `plugin_data` / TMPDIR, Python still sees `ctx.gcode_path`, and Edge writes
  the reply back. The hosted write root is that working-copy folder, not the export
  directory — a write outside `plugin_data` raises `PermissionError` naming
  `ctx.gcode_path`. Sibling files next to the original G-code are not visible to the child.
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
  live until host close (not revoked at `ResumeThread`). Every read-only grant is skipped
  when the package SID or ALL APPLICATION PACKAGES already has RX (Program Files installs);
  otherwise the grant is fail-closed. The helper uses `PyConfig_InitIsolatedConfig` with explicit
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
