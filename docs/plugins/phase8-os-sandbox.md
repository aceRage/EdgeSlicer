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
| Linux | Landlock (filesystem) + seccomp-bpf (ptrace/mount/unshare/modules/bpf/execve/inet sockets) | Child, after CPython starts, before `load` / `execute`. Inherited by grandchildren. |
| Windows | AppContainer (no capabilities unless network was granted) + the Phase 7 Job object (`KILL_ON_JOB_CLOSE`, `ACTIVE_PROCESS=1`, no `BREAKAWAY_OK`) | Parent: `CreateAppContainerProfile`, ACLs on the plugin dir / bundled Python / G-code folder, `SECURITY_CAPABILITIES` on `STARTUPINFOEX`. Create suspended, assign job, resume. |
| macOS | Signed helper with `com.apple.security.app-sandbox` (`scripts/edge-plugin-host.entitlements`) | Code signature. Ad-hoc and Developer ID signing hooks live in `src/CMakeLists.txt`, `scripts/macos_sign_app.sh`, `scripts/sign_and_package.sh`, and `build_release_macos.sh`. |

bubblewrap is optional for **native** Linux packages only and is off unless
`EDGE_PLUGIN_SANDBOX_BWRAP=1` is set through the Phase 7 test env-override gate.
**Flatpak must never nest bwrap** (`running_inside_flatpak()` / `/.flatpak-info` / `FLATPAK_ID`).
Inside Flatpak the child still uses Landlock+seccomp.

`sandbox-exec` / Seatbelt (`sandbox_init` with `kSBXProfileNoInternet`) is a **dev/test probe
only**, labelled `seatbelt-probe` and enabled with `EDGE_PLUGIN_SANDBOX_SEATBELT_PROBE=1` through
the same test-only gate. It is not the shipping macOS design.

## Handshake

`hello` may carry a `sandbox` object (plugin dir, Python home, extra grant paths, network/exec).
`hello_ok` always carries `sandbox: {applied, kind, detail, landlock_abi, network, exec}`.

`kind` is one of `landlock+seccomp`, `appcontainer`, `app-sandbox`, `seatbelt-probe`, `none`.

Edge refuses to run the workload when the child reports `applied=false`, unless the user has a
stored override (see below) or a test-only env override is set.

The helper is respawned when the sandbox spec changes (different plugin or a different G-code
write root), so Landlock rules never have to be loosened.

The child does not re-hash the plugin tree on `load`. Edge already evaluated Phase 5 trust
in-process before sending `load`, and host RPC still rechecks p5 on the Edge side. Re-hashing
under Landlock missed `trust.json` or picked up execute-time files the pin did not include.

## Fail-closed policy

If the OS sandbox cannot be applied (Landlock ABI 0 / kernel older than 5.13, AppContainer APIs
missing, unsigned macOS helper, and so on) Edge **does not** silently fall back.

- Prompt per plugin. Default button is **Don't run**. **Run anyway** is the other button.
- Choosing Run anyway records the plugin_key on `trust.json` as `unsandboxed` next to the pin
  allow-list, and the Home card shows **Running unsandboxed**.
- Early slicing-pipeline steps (`posSlice` … `psSkirtBrim`) stay in-process with a live `Print *`.
  They do **not** get this prompt. The Home card already says "Early slicing steps run
  unsandboxed in-process".

Test-only env overrides, ignored in production (same gate as Phase 7's `EDGE_PLUGIN_HOST`):

| Env | Effect |
|---|---|
| `EDGE_PLUGIN_SANDBOX=0` | Do not apply the OS sandbox; report `kind=none`. |
| `EDGE_PLUGIN_SANDBOX_FORCE_UNAVAILABLE=1` | Pretend the OS cannot sandbox, so the prompt path runs. |
| `EDGE_PLUGIN_SANDBOX_SEATBELT_PROBE=1` | macOS: `sandbox_init` probe instead of requiring a signed container. |
| `EDGE_PLUGIN_SANDBOX_BWRAP=1` | Native Linux only; ignored inside Flatpak. Shipping path remains Landlock+seccomp. |

## Grants mapped into the sandbox

Always allowed inside the child:

- bundled Python home (read)
- the plugin's own directory (read/write, including `__pycache__`)
- a private `TMPDIR` (`/tmp/edge-plugin-XXXXXX`)
- helper directory / shared `python/packages` / `resources_dir` (read)
- `trust.json` (the sidecar file only, not all of `data_dir`; Landlock file
  rules drop directory-only bits so `add_rule` is not EINVAL)
- the plugin's `plugin_data` storage directory (read/write)
- on Linux: `/usr`, `/lib`, `/lib64`, `/etc`, `/dev`, `/proc`, `/sys` (read) so CPython and
  libpython keep working

From the plugin's Phase 4 sidecar:

- `fs_read` / `fs_readwrite` paths (tree `/*` markers stripped)
- network (`network_http` / `network_socket`) → Windows `internetClient` capability; Linux
  seccomp allows `AF_INET`/`AF_INET6`. When network is not granted, Landlock ABI ≥ 4 also
  handles TCP bind/connect with no rules added (so those are denied) and seccomp still
  denies inet sockets.
- process grants → `allow_exec` (otherwise Linux seccomp denies `execve`/`execveat`; Windows
  `ACTIVE_PROCESS=1` already blocks children)

A `full_access` Python audit grant does **not** widen the OS sandbox.

## Grandchildren

- Linux: `PR_SET_CHILD_SUBREAPER` on Edge, `setpgid` in parent and child, `kill(-pid, SIGKILL)`
  on host kill.
- Windows: the Job object kills every assigned process; `ACTIVE_PROCESS=1` stops them spawning.
- macOS / POSIX: process group kill (`kill(-pid)`).

## What can only be verified on a signed/notarised bundle

- macOS App Sandbox actually restricting the helper (`APP_SANDBOX_CONTAINER_ID`). Ad-hoc
  `codesign --sign - --entitlements scripts/edge-plugin-host.entitlements` is hooked up but
  does not go through notarisation.
- Runtime exceptions for the plugin dir and the G-code folder on a sandboxed macOS helper
  (those paths are not known at sign time).
- Windows AppContainer + named-pipe DACL + Job object on a real Windows box.
- Flatpak: Landlock+seccomp inside the existing bwrap; no nested bwrap.
- AppImage: helper next to the binary, same Landlock+seccomp path as a native prefix.

## Smoke tests owed at merge

Linux `[PluginSandbox]` (probe, handshake, fs/net/exec isolation, fail-closed prompt +
`trust.json` override). Existing `[PluginIpc]`, `[PluginTrust]`, Home plugin-filter tests.
Spawn+handshake timing next to Phase 7 r5 (~0.275 s on Linux). U1 prints are still required
for pipeline plugins.
