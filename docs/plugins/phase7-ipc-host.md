# Phase 7 — Out-of-process plugin host

Revision: Edge `feature/plugins-p7-ipc-host`.
Plan: plugins plan section 6 (sandbox workloads, Out-of-process host and IPC) and Phase 7 in
section 10. Catalog publishing is Phase 11. Printer-agent hooks stay unwired. There is no OS
sandbox in this phase (Phase 8).

All of this is compiled only when `EDGE_PYTHON_PLUGINS` is ON (default OFF). `libslic3r` is
not given that define. With it OFF, or ON with no pipeline plugin enabled, G-code is
byte-identical to today and the empty plugin config keys are still skipped in the G-code dump
(the Phase 6 B1 fix). Startup never spawns `edge-plugin-host` in golden or CI runs: the helper
is launched on the first script or `psGCodePostProcess` execute only.

## What was built

`edge-plugin-host` is a bundled helper next to EdgeSlicer. It embeds the same CPython runtime
and the same `orca` module. Edge talks to it over a local socket (POSIX `socketpair`) or a
Windows named pipe.

| Workload | Where it runs |
|---|---|
| Script | **Out of process** via the helper |
| `psGCodePostProcess` | **Out of process** via the helper |
| Early slicing steps (`posSlice` … `psSkirtBrim`) | **In-process**, unsandboxed, live `Print` / `PrintObject`. The Home card still says so. |

## Protocol

Versioned, length-prefixed JSON:

- 4-byte little-endian payload length, then UTF-8 JSON object with a `type` field
- protocol version **1**
- max payload 16 MiB
- handshake `hello` / `hello_ok`
- `load` / `load_ok`, `execute` / `result` / `error`, `shutdown` / `shutdown_ok`
- nested `host_call` / `host_result` and `audit_check` / `audit_result` during execute

Every `orca.host.*` call in the child is an RPC back into Edge. Edge applies Phase 4
permission grants and Phase 5 trust on the Edge side before performing it. The child never
receives raw `Print*` / `PrintObject*` pointers, UltraNet `{data_dir}/plugins` paths, or
secrets. Filesystem events in the child (`open` and the other FS audit categories) are also
decided on the Edge side so the Phase 6 write root (the exported G-code's folder only) cannot
be skipped by a modified child.

## Crash isolation

A child abort, segfault, hang, or kill does not take Edge down. Edge reports a clear failure
for that run, plugin state stays recoverable, and the next run respawns the helper. Execute
timeouts kill a hung child.

The child dies with Edge:

- Linux: `prctl(PR_SET_PDEATHSIG, SIGKILL)`
- Windows: Job object with `KILL_ON_JOB_CLOSE` and `ACTIVE_PROCESS=1`, no breakaway; create
  suspended, assign, resume
- macOS: extra parent-pipe EOF watchdog (plus the inherited IPC fd closing)

There is no AppContainer, Seatbelt, Landlock, or seccomp in this phase.

## Permission prompts

Phase 4's worker-thread deny-first behaviour is unchanged. Host RPC and audit RPC run on the
execute worker, so they cannot pop a permission dialog off the UI thread.

## Known limitations

- No OS sandbox until Phase 8. A malicious child that skips its own Python audit hook still
  has the process's ambient filesystem until then; Edge-side host RPC and FS `audit_check`
  cover the `orca.host` surface and audited `open()` events.
- Early slicing-pipeline steps remain in-process and unsandboxed.
- `host.preset_bundle` / `host.plater` are not snapshotted over IPC in this phase (they throw
  in the child the same way they throw without a GUI). Script and post-process fixtures do not
  need them.
- Printer-agent hooks stay unwired. No catalog and no pages.

## Smoke tests owed at merge

Mac GUI (script runs via the host, kill the host mid-run), Windows, Linux pkg, Flatpak. U1
prints are still required for pipeline plugins.
