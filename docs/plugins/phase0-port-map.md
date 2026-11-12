# Phase 0 — Python plugins: spike and port map

Revision: Edge `feature/plugins-p0-spike`, 6 November 2026.
Plan: `orca-plugins-plan.md` r4 (6 November 2026, America/Chicago).
This document is the Phase 0 deliverable. It is an adapt map, not a cherry-pick list. Phase 1 has not started.

## Method

Scanned:

- Edge `main` `ccb393e103` (6 November 2026, merge of #905). Confirmed: no `src/slic3r/plugin/`, no `PluginManager` / `PythonPlugin` / `PluginsDialog` / `OrcaCloud` symbols, no `deps/python3`, no `deps_src/pybind11`.
- Upstream [OrcaSlicer/OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) `main` `9612260bb5` (6 November 2026, 20:53 UTC, “Fix multiline infill paths…”). Plugin tree, CMake Python/`uv` wiring, `docs/HLSD/plugin-permissions.md`, `PluginsDialog.hpp`, tests, and `Print.hpp` pipeline seams were read from that tip. The r4 plan cited code-search tip `6b0d50bf`; this map is against a later `main`.
- This Cloud Agent Linux host (kernel `6.12.113+`, x86_64): Landlock, seccomp, bubblewrap, OpenSSL Ed25519, and system Python ABI.

Spike code lives under `src/slic3r/plugin/spike/` and is compiled only when `EDGE_PYTHON_PLUGINS_SPIKE=ON`. Default builds do not link it. Nothing in this phase hooks `Print`, `PostProcessor`, or G-code export.

## 1. Settled decisions this map respects

From plan section 4 (final). Encoded as compile-time policy in the spike tests:

| Decision | Spike symbol | Later phase |
|---|---|---|
| GitHub catalog first; only Edge publishes | `PluginCatalogPolicy` | 11 |
| Per-plugin `printer_send`; Full access does **not** grant it; send only through existing Edge connections | `PluginGrantPolicy` | 4, 10 |
| Sandbox-unavailable defaults to Don't run; remembered per plugin | `PluginSandboxPolicy::DontRun` | 8 |
| Early `slicing-pipeline` steps may stay in-process, labeled, no extra prompt | `PluginWorkload::EarlySlicingPipeline` → `InProcessLabeled` | 6, 8 |
| Home > Plugins **and** a Plugins inspector dialog | see §3 UI | 2, 4 |
| Python packages in `{data_dir}/orca_plugins`, never `{data_dir}/plugins` | `PluginPathPolicy` | 1 |
| `feature/plugins` does not merge to `main` until after sandbox phases 7–8 | branching | — |

## 2. Runtime and port boundaries

### 2.1 What a plugin is (Orca, unchanged)

A plugin is a Python package that runs on an embedded CPython interpreter (pybind11) inside the slicer process in v1. Public module: `orca`. Packaging: a `.py` with PEP 723 metadata, or a `.whl`. Discovery writes `{data_dir}/orca_plugins` (plus Orca Cloud `_subscribed/{user_id}/`, which Edge will **not** port). `.install_state.json` defaults `enabled` to false. Per-plugin storage: `orca_plugins/plugin_data`.

Capability types named: `printer-connection`, `pages`, `analysis`, `importer`, `exporter`, `visualization`, `script`, `slicing-pipeline`, `resource-texture`. Only five have C++ implementations. Reserved names stay unwired.

### 2.2 What Edge will run, and where

```
Edge process                          sandboxed child (phases 7–8)
────────────────                      ────────────────────────────
GUI, Home, inspector                  edge-plugin-host
embedded CPython (v1, all workloads)  bundled CPython + orca.host RPC stubs
early slicing-pipeline (Print*)       script / pages Python / resource-texture
audit hook (v1 and leftover           psGCodePostProcess (file + config reads)
  in-process work)
UltraNet / vendors / FlashForge  <── printer_send proxy only (phase 10)
```

IPC: local socket or pipe. The child never receives raw `Print *`, UltraNet library paths, or secrets. Grants are checked on the Edge side of every `orca.host` call, not only in the child.

### 2.3 Existing Edge spawn code to reuse (not wxExecute)

Confirmed against this tree:

| OS | Do this | Cite | Do not |
|---|---|---|---|
| Windows | `CreateProcess` into a Job object, create suspended, assign, resume. `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, `JOB_OBJECT_LIMIT_ACTIVE_PROCESS=1`, **no** `BREAKAWAY_OK`. Phase 8 adds AppContainer (`CreateAppContainerProfile` / `SECURITY_CAPABILITIES`). | `src/slic3r/GUI/RemoteHub.cpp` (~5148, `CreateJobObjectW`) | `wxExecute` for a long-lived helper |
| macOS / Linux (phase 7) | `HubPlatform::spawn_child_posix`: fork+exec, no shell, own process group, close-on-exec error pipe | `src/slic3r/Utils/HubPlatform.hpp` `spawn_child_posix` | `wxExecute` (Process.cpp: “On Apple the wxExecute fails”) |
| macOS (phase 8) | Signed helper / XPC with `com.apple.security.app-sandbox` | new target next to the app bundle | `sandbox-exec` / `sandbox_init` as the shipping path |
| Linux (phase 8) | Landlock + seccomp in the child. bubblewrap only on native packages. Flatpak: Landlock+seccomp inside the existing sandbox, **never nested bwrap**. | new; Flatpak manifest `scripts/flatpak/io.github.acerage.EdgeSlicer.yml` | silent skip of Landlock on an old kernel |

Helper binary name: `edge-plugin-host` (`k_plugin_host_helper_name`). Not shipped in this phase.

### 2.4 In-process vs out-of-process (confirmed)

`SlicingPipelinePluginCapability.hpp` on Orca `main` hands `Print *` and `const PrintObject *` into the live graph, with the same invalidation rules as `std::vector` iterators. That API is shared-memory. A full out-of-process sandbox that preserves Orca's in-pipeline `Print *` API is not feasible without an upstream redesign. G-code post-process (`psGCodePostProcess`) only needs the working file, `host`, `output_name`, and config reads — that **can** move to the child.

Edge `Print.hpp` has `PrintStep` / `PrintObjectStep` but **does not** have `SlicingPipelineStepPlugin`, `Print::set_slicing_pipeline_hook_fn`, or `run_pipeline_hook`. Those libslic3r seams are part of the port, not present today.

## 3. File-level adapt list

Adapt, do not apply `#14530` as a patch. Orca's plugin surface has grown through dozens of PRs since 17 July 2026.

### 3.1 Port into `src/slic3r/plugin/` (new on Edge)

Upstream listing at `9612260bb5`. Split by phase. Cloud subscribe types are listed so they are **not** copied.

**Phase 1 — runtime and local packages**

| Upstream | Adapt notes |
|---|---|
| `PythonInterpreter.cpp/.hpp` | Bundle CPython 3.12.13 + `python_abi_tag()` `"cp312"`. `bundled_uv_path()`. Shutdown must be reachable from macOS `wxEVT_END_SESSION` (Edge already unloads UltraNet there; add Python). |
| `PluginManager.cpp/.hpp` | Drop `CloudPluginService` / `set_cloud_agent` / `_subscribed/{user_id}` from the v1 path. Keep `install_plugin`, `delete_plugin`, `mutate_plugin_permissions`, `launch_script_capability`. Fix #17298 (delete walks into parent) while adapting. |
| `PluginLoader.cpp/.hpp`, `PluginResolver.cpp/.hpp` | PEP 723 + wheel. Wheel key must **not** include version (#17100). |
| `PluginConfig.cpp/.hpp`, `PluginFsUtils.cpp/.hpp` | Sidecar schema. Storage under `orca_plugins/plugin_data`. |
| `PluginDescriptor.hpp`, `PyPluginPackage.hpp`, `PluginJsonPy.hpp`, `PluginBindingUtils.hpp` | Keep `plugin_key` / capability-name shape so an Orca 3MF can name a plugin. |
| `PythonPluginInterface.hpp`, `PyPluginTrampoline.hpp`, `PythonPluginBridge.cpp/.hpp` | Registration, `orca.request_permissions`, trampoline audit scope. |

**Do not copy in phase 1:** `CloudPluginService.*`. Replace later with an Edge catalog client (phase 11).

**Phase 3 — scripts and v1 host**

| Upstream | Adapt notes |
|---|---|
| `pluginTypes/script/*` | `execute()` sync and `async def` via `ScriptAsyncRuntime`. |
| `host/PluginHost.cpp/.hpp`, `PluginHostBindings.hpp` | `orca.host` domains: `app`, `model` (read), `presets` (read), `geometry`, `mesh`, `slicing`, `plugin`, `ui`. Writes to presets wait for phase 12. Missing symbols must error “not available in this Edge build”. |
| `host/PluginHostApp.cpp`, `PluginHostModel.cpp`, `PluginHostPresets.cpp` (read subset), `PluginHostGeometry.cpp`, `PluginHostMesh.*`, `PluginHostSlicing.cpp`, `PluginHostUi.cpp/.hpp` | `PluginHostUi` talks to wx; keep off the slicing worker. |

**Phase 4 — audit and inspector**

| Upstream | Adapt notes |
|---|---|
| `PluginAuditManager.cpp/.hpp` (~104 KB `.cpp`) | CPython `PySys_AddAuditHook`. Port the grant model. Add Edge `printer_send` **outside** the audit-hook map (it is a host-API gate, not an `open()` event). |
| `GUI/PluginsDialog.cpp/.hpp` (shared helpers only) | Reuse `refresh_plugin_metadata_blocking`, `install_local_plugin_package`. Do **not** ship Orca's standalone manager as Edge's Home. |
| `GUI/PluginsConfigDialog.*`, `GUI/PluginPermissionValidation.*`, `GUI/PluginProgressDialog.*` | Inspector chrome. Permission editor from #17285. Full access switch, warning, default button No. |
| `resources/web/dialog/PluginsDialog/` | Payload/copy reference. Edge inspector page is new; do not clone the catalog grid. |

**Phase 6 — pipeline (G-code)**

| Upstream | Adapt notes |
|---|---|
| `pluginTypes/slicingPipeline/*` | Landed on `feature/plugins-p6-pipeline`. Live `Print *` in-process. Home card: "Early slicing steps run unsandboxed in-process". |
| `PluginHooks.cpp/.hpp` | Landed. Installs `Print::set_slicing_pipeline_hook_fn`, lifecycle hook, capability resolver, G-code post-process hook. |
| `libslic3r/LifecycleEvents.hpp` | Landed in earlier plugin phases. |
| `libslic3r/Print.hpp` `SlicingPipelineStepPlugin`, `set_slicing_pipeline_hook_fn`, `run_pipeline_hook` | Landed. Default hook is empty so goldens stay identical until a plugin is enabled. |
| `libslic3r/GCode/PostProcessor.cpp` | Landed. `psGCodePostProcess` plugin call with a scoped write root on the G-code folder. #17095 retry was already on Edge. |
| PrintConfig `slicing_pipeline_plugin` + dynamic `plugins` manifest | Landed. Survives 3MF round-trips. `ConfigBase::set_resolve_capability_fn` / `collect_plugin_manifest` are GUI-resolver only. |

**Phase 9**

`pluginTypes/pages/*`, `host/PluginPages.*`. Watch open #16397 (WebView session isolation). Edge already isolates WebView2 *copies* (`WebView2UserData.hpp`); that is not plugin-session isolation.

**Phase 10**

`pluginTypes/printerAgent/*`. Layer on UltraNet / vendor send / FlashForge. `printer_send` default deny. Never a back door to `bambu_cdn_download_allowed()`.

**Phase 12**

`host/PluginResourceTextures.*`, `pluginTypes/resourceTexture/*`, mutable presets (#16518). Score still-open Orca host PRs against Edge's paint-depth / MixedFilament / per-nozzle stack before copying.

### 3.2 Orca GUI files that are reference, not a dump

`GUI/PluginPickerDialog.*`, `GUI/PluginSort.hpp`, `GUI/PluginSource.hpp`, `GUI/PluginStatus.hpp`, `resources/web/dialog/PluginsDialog/{index.html,index.js,plugin-search.*,plugin-sort.*,styles.css}`.

Edge Home section is original work in:

- `src/slic3r/Utils/HomeTabLogic.hpp/.cpp` — add `"plugins"` to `valid_section`
- `src/slic3r/GUI/HomePanel.cpp/.hpp` — `ensure_plugins()` next to `ensure_vendors` / `ensure_vaults`
- `src/slic3r/GUI/HomePlugins.hpp/.cpp` — new, same shape as `HomeVendors` (`handle(json)`, `send_state()`)
- `resources/web/home/index.html`, `home.js` (`SECTIONS`)
- `tests/slic3rutils/home_tab_tests.cpp`

Page never receives `entry_path` / `plugin_root`; opaque `plugin_key` only.

### 3.3 Tests to port (under `tests/slic3rutils/`)

`test_plugin_audit`, `test_plugin_capabilities_in_use`, `test_plugin_capability_config`, `test_plugin_cloud_metadata` (adapt to Edge index), `test_plugin_config`, `test_plugin_host_api`, `test_plugin_install`, `test_plugin_json_depth`, `test_plugin_lifecycle`, `test_plugin_permission_validation`, `test_plugin_printer_agent`, `test_plugin_resource_texture`, `test_plugin_sort`, `test_plugin_status`, `test_precise_seam_plugin`, `test_slicing_pipeline_config`, plus `plugin_test_utils.hpp`.

Add Edge-only: `{data_dir}/plugins` is never created or written by Python-plugin code; `printer_send` independent of Full access; Home `valid_section("plugins")`.

Example plugins: Orca `sandboxes/orca_*_plugin_any.py`. Copy into Edge `sandboxes/` as fixtures, not as enabled-by-default packages.

### 3.4 Do not port

- `OrcaCloudServiceAgent`, cloud sign-in, `_subscribed/{user_id}`, subscribe/unsubscribe UI
- Bambu network-plugin download path (already killed; `bambu_cdn_download_allowed()` is always false)
- Open/unmerged Orca PRs as if shipped: #17281 (`uv` wheel install), #17292 (uv 0.13.7), #16397, #16941 (infill plugins, G-code), #16692, #15711, #16758, #16709, #16790, #16858, #17132, #17114, #16891, #16782, #16783, #16884, #16814, #16812
- Treating Orca Cloud as a trust root

## 4. Edge collisions

| Edge today | Collision | Rule |
|---|---|---|
| `{data_dir}/plugins` UltraNet / FlashNetwork | Same English word “plugin” | Python packages → `orca_plugins`. UI: “Plugins” vs “network plug-in” / UltraNet. |
| `PluginGuard.hpp/.cpp`, `PluginLoadDiagnostics`, `PluginInstallStatus`, `WebUpdatePlugin` | Name clash with `src/slic3r/plugin/` | Keep Guard names. New Python types live under `slic3r/plugin/`. Never call `begin_network_plugin_download` from the Python path. |
| Home tab five sections | New sixth rail item | `HomeTab::valid_section` currently `recent\|library\|history\|vendors\|vaults`. |
| `resources/web/homepage/` leftover `NoPluginTip` | Old Orca homepage, not Home | Ignore; do not revive Bambu download copy. |
| macOS `wxEVT_END_SESSION` in `GUI_App.cpp` (~3435) | Unloads UltraNet because `OnExit` never runs | Same hook must shut down `PluginManager` + CPython (#16136). Distinct from the UltraNet unload already ported. |
| `PostProcessor.cpp` #17095 | Retry after script failure | Extend with plugin post-process; default off. |
| `WebView2UserData.hpp` | Per-copy WebView2 folder | Not plugin-page session isolation. Phase 9 must not share cookies with Home > Vendors or printer login. |
| `RemoteHub` Job object / `spawn_child_posix` | Spawn patterns | Reuse for `edge-plugin-host`; do not invent a third spawn stack. |
| `bambu_cdn_download_allowed()` always false | Printer agents | A Python agent must not reintroduce a Bambu CDN fetch. |

## 5. Sandbox feasibility (validated)

Plan section 6 stands. Additional evidence from this tree and this host:

### 5.1 Linux (this VM)

| Probe | Result |
|---|---|
| Kernel | `6.12.113+` x86_64 |
| `linux/landlock.h` | Present. `landlock_create_ruleset` version query returned **ABI 6**, errno 0 |
| `libseccomp2` | Installed (`2.5.5-1ubuntu3.1`) |
| `libseccomp-dev` / `<seccomp.h>` | **Not** installed. `scripts/linux.d/debian` does not list it. Phase 8 must add the -dev package (and the Flatpak SDK equivalent) |
| `bwrap` | Not on PATH. Acceptable: Flatpak must not nest it; native packages can depend on bubblewrap at phase 8 |
| Fail closed | Encoded. Old kernels without Landlock → per-plugin Don't run, not a silent skip |

Landlock is feasible on current Ubuntu 24.04-class kernels. The missing header package is a build-script gap, not a kernel gap.

### 5.2 Windows

AppContainer APIs exist from Windows 8; Edge already targets newer desktop. Job objects are **already in use** (`RemoteHub.cpp`). Cost estimate 5–8 days after the IPC host still holds. ACL work on granted paths is the usual footgun.

### 5.3 macOS

`CMAKE_OSX_DEPLOYMENT_TARGET` is 12.0. App Sandbox belongs on a **helper**, not on Edge.app (the slicer opens arbitrary models). `sandbox-exec` remains a development probe only. Signing/notarisation of the helper is why macOS is the long pole (8–12 days). Edge already re-signs bundled `libpython` ad-hoc in Orca's `python3.cmake` (`codesign --force --sign -`); the plugin helper needs the real identity in CI (`docs/windows-signing.md` is Windows; macOS notarisation is a separate pipeline).

### 5.4 Flatpak

`scripts/flatpak/io.github.acerage.EdgeSlicer.yml` already exists. Nested user namespaces inside Flatpak are restricted. Phase 8 path: Landlock + seccomp inside the Flatpak sandbox. `ORCA_UV_FLATPAK_BINARY` on Orca is `${CMAKE_INSTALL_FULL_DATAROOTDIR}/OrcaSlicer/tools/uv/uv` — Edge will need the equivalent under the Edge Flatpak id.

## 6. Orca `orca` API compatibility assumptions

Goal is full public `orca` parity so a community plugin written against Orca can load. #14530 still calls the surface unstable. Edge must advertise an `orca` version and refuse a plugin that asks for a newer one.

**v1 subset (phases 0–6), confirmed still correct against `9612260bb5`:**

In: `@orca.plugin`, `register_capability()`, `ExecutionResult` / `PluginResult`, `script` + `slicing-pipeline` bases, `orca.request_permissions`, `orca.host.ui`, `orca.host.app` (version, paths, storage, notifications), `orca.host.model` read, `orca.host.presets` read, geometry/mesh as required, `orca.host.slicing`, lifecycle events.

Out of v1: `pages`, `printer-connection`, `resource-texture`, mutable presets (#16518), reserved types.

Assumptions to treat as constraints, not hopes:

1. Capability type strings (`script`, `slicing-pipeline`, …) stay stable; they are already used in presets and 3MFs.
2. `plugin_key` for local packages is the filesystem-safe stem; cloud keys are UUIDs. Edge catalog rows may carry optional `orca_cloud_uuid` later so a 3MF that names an Orca Cloud plugin can be matched without talking to Orca Cloud.
3. Host bindings are split by domain in `host_bindings::register_*`. Phase 3 can omit a domain's `.cpp` and still fail cleanly if `PluginHost::RegisterBindings` only registers what exists.
4. `SlicingPipelineContext.print` is a raw pointer. v1 keeps those steps in-process. IPC stubs for `orca.host.slicing` at `psGCodePostProcess` must not pretend to expose `Print *`.
5. `requires-python` is parsed but not enforced upstream. Edge v1 can keep that gap; document it.
6. Open Orca API PRs are **not** part of the advertised version until scored.

`PluginContext` currently carries `orca_version` only. Edge should add an Edge build identifier in host.app without breaking the Orca field.

## 7. Dependencies, licensing, and build per OS

Edge has **none** of the plugin runtime today. Tests/CMakeLists.txt's `Python3` is the *host* interpreter for profile scripts, not an embedded CPython.

### 7.1 What to add (phase 1)

| Dep | Orca source | License | Notes |
|---|---|---|---|
| CPython 3.12.13 | `deps/python3/python3.cmake` (~400 lines, OS-specific) | PSF License v2 (GPL-compatible with AGPL-3.0) | Shared lib under `${prefix}/libpython`. SHA256 pinned. Windows PCbuild; macOS `ac_cv_func_pipe2=no` (SDK 27 trap); Linux rpath relocated for Flatpak. |
| pybind11 3.0.1 | `deps_src/pybind11` headers; `pybind11::embed` INTERFACE in root CMakeLists | BSD-3-Clause | `PYBIND11_SIMPLE_GIL_MANAGEMENT`. Edge `deps_src/` has no pybind11. Header-only: add as a vendored snapshot with upstream tag recorded. |
| uv 0.11.21 | Root CMakeLists auto-download with per-arch SHA256 | MIT OR Apache-2.0 | `#17281` (install wheels with uv) is still draft; `#17292` would bump to 0.13.7. Pin 0.11.21 until those merge and are scored. Flatpak supplies uv as a file, not a download. |
| OpenSSL 3.x | Already in Edge (`find_package(OpenSSL 3.0...<4.0)` on Flatpak; deps otherwise) | OpenSSL / Apache-2.0 | Ed25519 works (`openssl genpkey -algorithm ED25519` on this host). Phase 5 signing needs **no** new crypto library. Catalog **private** key stays out of the repo. |

Edge `LICENSE.txt` is AGPL-3.0. PSF + BSD-3 + MIT/Apache are all distributable with it. Bundled CPython source must ship with the app (PSF). uv binary: include license files next to the tool.

CPython on Windows pulls its own OpenSSL 3.0.16.2 nupkg for `_ssl`; Unix links Edge's static OpenSSL. Phase 1 must keep that split so `_ssl`/`_hashlib` relink when OpenSSL is rebuilt (Orca already has `relink_ssl_extensions`).

### 7.2 Per-OS build cost inside phase 1

| OS | Extra work vs Orca |
|---|---|
| Windows | PCbuild + 64-bit MSBuild (C1002 if 32-bit host). VS 2026 ARM64 unicodectype.props. Stage into `libpython`. Job object reuse is free. |
| macOS | Deployment target 12.0. `install_name_tool` `@rpath` + ad-hoc codesign of libpython. `#16136` shutdown on `wxEVT_END_SESSION`. Helper notarisation is phase 8, not 1. |
| Linux | `--enable-optimizations` PGO is slow and memory-hungry; Cloud Agent balloon means `-j` must stay low (`nproc` 2, `CMAKE_BUILD_PARALLEL_LEVEL=1` for deps). Relocate rpath for `/opt` vs Flatpak `/app`. |
| Flatpak | `FLATPAK=ON` already. Add CPython/uv to the manifest; do not download uv at build time if the sandbox has no network. |

`scripts/linux.d/debian` needs `libseccomp-dev` for phase 8, not phase 1. Phase 1 does not enable Landlock.

### 7.3 Binary size / packaging

Bundled CPython is a shared library plus stdlib. Expect tens of MB next to the app (Orca layout: `libpython/`). `uv` is a single binary per arch. Neither belongs in `{data_dir}/plugins`.

## 8. Findings

1. **Edge has zero of this system.** A naive cherry-pick of #14530 would be months out of date and would still miss Home, UltraNet isolation, `printer_send`, signing, and the sandbox.
2. **libslic3r seams are the quiet Phase 1/6 cost.** `LifecycleEvents.hpp`, `SlicingPipelineStepPlugin`, `Print::set_slicing_pipeline_hook_fn`, and the preset capability resolver do not exist on Edge. They must be added as empty/default-off hooks so G-code stays identical until a user enables a pipeline plugin.
3. **Path and name collisions are real but mechanical.** `PluginGuard` stays. Python code uses `orca_plugins`. Spike tests pin that split.
4. **Spawn code already exists** for a helper process (hub/go2rtc). Phase 7 should copy that model, not `wxExecute`.
5. **Landlock works on this class of Linux.** ABI 6. The gap is `libseccomp-dev` in the Debian package list and a Flatpak policy, not the kernel.
6. **Audit hook ≠ sandbox.** v1 is in-process with the user's rights. Signing/allow-list (phase 5) reduce *who* runs. OS sandbox (phase 8) reduces *what a running script can do*. Early pipeline stays the labeled exception.
7. **`printer_send` is an Edge host-API grant**, not an Orca audit category. Full access must not set it. Implementation belongs next to send-path code, with the permission editor showing it as a separate switch.
8. **Orca Cloud is not a compatibility requirement.** Package format, `plugin_key`, and `orca` API are. Subscribe/sync is not.
9. **#17095 is already on Edge** (`PostProcessor.cpp`). Phase 6 extends it; it does not re-port the retry wrapper.
10. **Home is original UI.** Orca's manager is `PluginsDialog` (WebViewHostDialog). Edge's manager is a Home rail section plus a dense inspector. Reuse C++ helpers and payloads; do not clone the dialog as a second grid.
11. **Default-off is already Orca's sidecar behaviour** (`PluginDescriptor::enabled` defaults false). CI goldens must assert no pipeline plugin is loaded. That is sufficient for `GATE_RESULT identical=10 diff=0` until phase 6's listed stamp fixture.
12. **Open upstream PRs that would change the port:** #17281 (uv install — phase 1 may need a temporary `pip`/`uv pip` path), #16397 (pages WebView isolation — phase 9), #16941 (infill — G-code, out of v1).

## 9. Blockers

Nothing blocks **this** phase (the map and the OFF spike). These block later phases if unaddressed:

| ID | Blocks | What |
|---|---|---|
| B1 | Phase 1 | No bundled CPython / pybind11 / uv in Edge deps. Must vendor `deps/python3` and `deps_src/pybind11` and wire `pybind11::embed`. |
| B2 | Phase 1 (macOS quit) | Python interpreter teardown on `wxEVT_END_SESSION` is not present; UltraNet-only today. |
| B3 | Phase 6 | Missing libslic3r pipeline/lifecycle/manifest seams. Adding them default-off is required before any pipeline plugin can run, and before goldens can prove they stay identical. |
| B4 | Phase 8 (Linux CI) | `libseccomp-dev` absent from `scripts/linux.d/debian`. Landlock headers exist on this image; seccomp userspace headers do not. |
| B5 | Phase 8 (macOS) | No signed plugin-host helper target, entitlements, or notarisation story. `sandbox-exec` is not an acceptable substitute. |
| B6 | Phase 8 (Flatpak) | Nested bwrap is the wrong tool. Need a Landlock+seccomp path inside the existing Flatpak sandbox before catalog installs can be claimed sandboxed on Flathub. |
| B7 | Phase 1 packaging | `#17281` (uv wheel install) is still draft upstream. Decide: wait, or ship a documented subset (PEP 723 `.py` first, wheels when #17281 is scored). **Recommendation:** phase 1 installs `.py` and unpacks wheels as Orca does *today* (without the draft uv installer); revisit when #17281 merges. |
| B8 | Phase 10 | `printer_send` has no upstream type. Must be designed as an Edge grant on the send path, default deny, existing connections only. Not a blocker for v1 scripts. |
| B9 | Merge to `main` | Working rule: not until phases 7–8, and not without Lance's own words. This draft PR must not be merged to `main`. |

Non-blockers (accepted limitations): early pipeline stays in-process; Orca Cloud subscribe is out of scope; community catalog publishers are out of scope; #16941 infill plugins stay out.

## 10. Risks

Unchanged from plan section 12, with these Edge-specific sharpenings:

- **libslic3r hook drift.** If `Print::process()` gains or reorders steps on Edge `main` while `feature/plugins` lags, pipeline hook placement will be wrong. Merge `main` into `feature/plugins` on the plan's cadence (weekly, and before every phase cut).
- **PluginGuard false friend.** A search-replace of “plugin” will hit UltraNet. Reviewers should treat `{data_dir}/plugins` writes in a Python-plugin PR as a defect.
- **In-process v1 on `feature/plugins`.** Anyone running that branch has unsandboxed Python in-process. Do not enable pipeline plugins in CI. Do not merge to `main` before 7–8.
- **IPC grant checks.** Every `orca.host` method that crosses the process boundary is a new trust boundary. Forgetting an Edge-side check would make the child sandbox cosmetic.
- **macOS WebView + Python teardown.** Two known-sharp edges (#16136, WKWebView). Budget them; do not discover them at RC.
- **CPython PGO / balloon.** `deps/python3` `--enable-optimizations` on a memory-ballooned Linux agent can OOM. Phase 1 Linux builds should keep the Cloud Agent `nproc` 2 / `CMAKE_BUILD_PARALLEL_LEVEL=1` recipe, or disable PGO in agent builds.
- **uv download in CMake.** Orca's root CMakeLists downloads uv at configure time. Flatpak and air-gapped CI need the `ORCA_BUNDLED_UV_EXECUTABLE` / manifest file escape hatch from day one.

## 11. Refined phase estimates

Engineering days, one implementer who knows both trees. Original r4 ranges in parentheses. Changes are from this inventory, not calendar padding.

| Phase | r4 | Refined | Why it moved |
|---|---|---|---|
| 0 Spike and port map | 3–5 | **3–5** (this PR) | Map + OFF spike. |
| 1 Embedded runtime | 12–16 | **14–18** | No python3/pybind11/uv in Edge at all; OS-specific CPython cmake; LifecycleEvents seam; PluginGuard naming; #17281 gap. |
| 2 Home tab section | 5–8 | **6–9** | Original UI (not a PluginsDialog dump); HomeVendors-shaped host; inspector left for phase 4. |
| 3 Scripts + v1 host | 6–9 | **7–10** | LifecycleEvents missing; host.ui vs Edge WebView; clean errors for omitted domains. |
| 4 Permissions and audit | 8–12 | **8–12** | Audit .cpp is large but well documented. Extra: `printer_send` switch in the inspector (even if send is phase 10). |
| 5 Signing / allow-list | 6–9 | **6–9** | OpenSSL 3 Ed25519 already works. |
| 6 Slicing pipeline | 8–12 | **10–14** | Entire Print/Config/PostProcessor seam is missing. #17095 exists. Goldens stay `diff=0` with plugins off. |
| **v1 (0–6)** | **48–71** | **54–81** | |
| 7 IPC host | 15–22 | **16–23** | Reuse Job object / posix spawn (helps). Still a new RPC `orca.host`. |
| 8 OS sandbox | 15–22 | **15–22** | Linux Landlock proven; seccomp-dev and macOS helper still the work. |
| **Sandbox (7–8)** | **32–48** | **31–45** | Slight reuse of spawn code. |
| 9 Pages | 6–10 | **6–10** | #16397 still open; Edge WebView2 copy isolation is not enough. |
| 10 Printer agents | 15–25 | **16–26** | UltraNet / `printer_send` / existing-connections-only is extra vs Orca. |
| 11 Catalog | 10–16 | **10–16** | GitHub-first; client must take a replaceable base URL. |
| 12 Parity | 8–12 | **8–12** | Score merged Orca host PRs against Edge. |
| **Later (9–12)** | **39–63** | **40–64** | |
| **First merge to main (after 7–8)** | **80–119** | **85–126** | Plus U1 prints for phase 6 claimed pipeline behaviour. |

Phase order is unchanged. Do not stack 6 ahead of 5, or 11 ahead of 5 and 7–8.

## 12. Spike (this PR)

CMake option `EDGE_PYTHON_PLUGINS_SPIKE` **OFF by default**. When ON:

- static lib `edge_plugin_spike` (no slicer linkage)
- `edge_plugin_spike_probe` — prints spawn plan, path split, grants, sandbox probe
- `plugin_spike_tests` — Catch2, registered as `ctest -R plugin_spike`

Policies encoded: `orca_plugins` vs `plugins`, `printer_send` independent of Full access, Don't run default, early-pipeline exception, GitHub/Edge-only catalog, spawn plan that cites RemoteHub / HubPlatform and forbids `wxExecute`.

On this Linux host the probe reported Landlock ABI 6, no bwrap, no libseccomp headers.

## 13. Out of scope (not started)

Phase 1 embedded CPython. Home rail item. Audit hook. Pipeline hooks. `edge-plugin-host` binary. Catalog client. Any G-code change.

Default gate for this PR: the option is OFF, so slicer tests and golden G-code are unaffected. The spike tests are the only new automated coverage.
