# Startup costs: instrumented breakdown, ranked fixes, PR split

Branch `perf/startup-costs` (worktree `C:/Dev/wt_startup`), 2026-10-24, from origin/main `0b3af03608`.
Follows the 4B-3 design note (`origin/design/4b3-preset-cache:tests/design_4b3_preset_cache.md`). Owner
decision 2026-10-24: skip upstream's preset cache (#14217/#16175), keep our CBOR preset cache on, and aim
startup work at the bigger costs.

Phase 1 (this PR) is logging only: finer `ORCA_STARTUP_PROFILE` marks. No fixes yet. The fixes below are
ranked from code reading and the owner's existing logs, and wait for one profiled start of the test copy.

## 1. What the owner's logs already show

Source: the 27 GUI starts in `%APPDATA%\EdgeSlicer\log` from 2026-10-22 and 2026-10-23 (main install and
test copies, all 2.4.7.0). Only log timestamps were used, no names, paths or IPs. None of these runs had
`ORCA_STARTUP_PROFILE` set, so phases are bounded by ordinary info lines. Times are seconds from the
first log line (HTTP service start, a few hundred ms after process start).

Starts fall into two groups. 15 are "warm" and 12 are "slow": in the slow ones, copying the filament
library takes more than 3 s.

| Phase (bounded by log lines) | Warm median (min-max) | Slow median (min-max) |
|---|---|---|
| Splash to updater check | 0.39 | 0.35 |
| **OrcaFilamentLibrary reinstall** (updater check to `atomic_replace_directory: replaced`) | **1.08** (0.77-1.30) | **6.67** (4.53-15.24) |
| Plug-in sync + network init | 0.12 | 0.21 |
| User preset folder + **Bambu preset mirror** | 0.47 | **8.47** (0.40-10.22) |
| System presets (`load_presets enter` to `load_user_presets`) | 2.05 (1.63-6.90) | 2.03 |
| User presets + selections, up to "create the main window" | 0.72 | 1.57 |
| MainFrame start to Plater `select_technology` (topbar, Home, Params, Sidebar...) | 2.14 | 2.20 |
| `select_technology` to first `set_current_panel` (3D views, UV editor, menus, Layout...) | 1.57 | 1.75 |
| `set_current_panel` to frame shown (rest of Plater, tabs, initial config, load_current_presets) | 2.07 | 2.17 |
| **Total to main frame shown** | **11.5** (8.4-14.6) | **26.9** (13.1-37.4) |
| Shown to end of post_init (GL init, first render, project restore) | 3.60 | 3.73 |
| post_init end to "startup is over" (first idle; wizard/updater CallAfter) | 3.04 | 2.77 |
| **Total to startup settled** | **19.5** (15.6-24.7) | **33.9** (18.8-44.9) |

(The 4B-1 harness's "~12.8 s to first idle" was measured differently: a hidden instance, no project
argument. The relative sizes match.)

Findings:

1. **The "hot-bed rules + default suppression" step is the PresetUpdater constructor, and nearly all of
   that is reinstalling OrcaFilamentLibrary on every start.** `check_installed_vendor_profiles()` always
   adds the library to the install set (an upstream Orca rule: "always install filament library").
   `install_bundles_rsrc()` then `remove_all`s `system/OrcaFilamentLibrary` and copies all 970 files back
   through `atomic_replace_directory`. That happened on all 27 starts. It costs ~1 s warm and 4.5-15 s on
   the slow starts, which are probably cold file cache plus Defender scanning 970 new files. Two knock-on
   costs follow:
   - Every reinstall changes the stats the CBOR cache key is built from, so the library's cache misses on
     every start. That is the "library cache rebuilt, 1.6 s" start in the 4B-3 note.
   - The fresh files must then be read back from disk.

   The other parts of that step are cheap in the logs: the association self-heal and registration take
   ~2 ms, and the hot-bed rules copy is a single file. Default suppression costs nothing at that point,
   because nothing is loaded yet.
2. **The Bambu preset mirror is the second-largest stall on slow starts (~8.5 s).** At startup it always
   runs in full, comparing ~660 mirrored presets against the Bambu Studio folder. The skip gate
   (`MirrorSkipGate`, fingerprint of that folder) exists, but it is only kept in memory and only used for
   printer switches.
3. **Main frame construction is ~5.8 s, not 2.0 s**, from "create the main window" to "main frame
   firstly shown". The existing `MainFrame::init_tabpanel step=` marks split the tab panel. The two big
   unlogged gaps are:
   - ~2.1 s before Plater reaches `select_technology`. This is MainFrame members, topbar, HomePanel,
     ParamsPanel and the Sidebar constructor, which runs as a Plater member initialiser.
   - ~1.6 s between the 3D view canvases and the first `set_current_panel`. This is the UV editor panel
     (another GL canvas), `menus.init`, canvas binds, `q->Layout()` and `apply_color_mode()`.

   A WebView is also created during construction (0.5-0.8 s), from StreamPanel, PrinterWebView or the
   Home/Params panels.
4. **User presets take ~0.7-1.6 s.** The owner has ~840 user presets: 504 detached parents in
   `filament/base` (from the Bambu mirror), 185 filaments, 141 processes and a few machines. The loader
   is sequential and works per file:
   - `fs::exists` and `load_info` for the `.info` file.
   - A full JSON parse.
   - `find_preset2()`, which builds a `std::regex` on every miss for "Generic" names.
   - A copy of the whole preset into `presets_loaded`.
   - A deferral round that **re-parses** every child whose detached parent has not been loaded yet.
5. **After the window is shown, ~6.6 s pass before startup settles.** That time is:
   - GL init: `canvas3D init` 0.2 s, the imgui first frame ~0.8 s, the first test render ~0.5 s.
   - Project restore.
   - The post_init `CallAfter`: wizard check, `WebPresetDialog` (vendor family scan), updater sync and
     the version check.

## 2. New marks in this PR (all behind `ORCA_STARTUP_PROFILE=1`)

Every line is `[StartupProfile] ...` at warning level in the normal app log. With the variable unset, each
mark costs one cached bool check. The new helpers are `StartupPhase` (stepwise marks), `StartupAccumulator`
(sums per-file intervals) and `startup_profile_uptime_ms()`, all in `src/libslic3r/StartupProfile.hpp`.

| Area | Marks |
|---|---|
| `GUI_App::on_init_inner` | The old "hot-bed rules + default suppression" mark is now: `repair_stale_associations`, `associate_files/associate_url`, `PresetUpdater ctor`, `updater event binds`, `default suppression`. Also: `http extra headers + ultranet plug-in sync`, `refresh_ultranet_plugin_state`, `enable_user_preset_folder`, `bambu preset mirror: change fingerprint` / `run`, `pre-mainframe`, `new MainFrame` (uptime stamp before it), `UpdateVersionDialog` (was "mainframe construction"), `select_tab/obj_list/notifications/printhost queue`. Uptime stamps at "main frame shown" and "startup settled" |
| `PresetUpdater::priv ctor` | `set_download_prefs`, `check_installed_vendor_profiles`, `printer config updates`; the list of bundles it decided to install; `install_bundles_rsrc step=remove_old_dir vendor=...`; one `perform_updates install vendor=... dir=... target=... ms=` per update |
| `PresetBundle::load_user_presets` | process / filament / machine: `load_presets` and `select_preset_by_name` separately |
| `PresetCollection::load_presets` (user folders) | One line per folder: type, folder (`base` or the type folder, never the full path), `json_files`, `parses`, `loaded`, `rounds`, `deferred`, `info_files`, `collection_size`, and `info_ms parse_ms lookup_ms apply_ms store_ms merge_sort_ms ms` |
| `MainFrame ctor` | Body-begin uptime (the member dialogs are before it), `update_fonts`, `topbar`, tab panel + menubar (already split), `event binds + sizer`, `update_layout`, `Fit/SetSize/Layout/update_title`, `update_ui_from_settings`, `update_slice_print_status_impl` |
| `Sidebar ctor` | Printer section, filament section, mixed filaments, search bar, `ObjectList`, `ObjectSettings/SearchObjectDialog`, end |
| `Plater::priv ctor` | `View3D`, `Preview`, `AssembleView`, `ShowcaseView`, `timers + update()`, `UVEditorPanel`, `menus.init`, canvas binds, `q->Layout()`, `apply_color_mode`, `set_current_panel`, `update_sidebar(true)` |
| `GUI_App::load_current_presets` | `check_printer_presets`, `set_printer_technology`, one mark per tab, model tabs, mixed-filament keys, `update_nozzle_settings` |
| `GUI_App::post_init` | `init_opengl`, `canvas3D init`, `imgui new_frame`, `first test render`, project restore, hints/Sentry/HMSQuery, discovery, blacklist, log cleanup. The `CallAfter`: `config_wizard_startup`, `WebPresetDialog`, updater sync/version check, legacy filament check |

Existing marks kept, apart from the two renames noted above: the `phase=...` lines, `PresetBundle::load_presets`,
the per-vendor system preset lines and the `MainFrame::init_tabpanel step=` timers. The 4B-1 harness's
parser, if it keys on the old "hot-bed rules" or "mainframe construction" labels, needs the new names.

Also fixed: `SLIC3R_STARTUP_SCOPE` pasted `__LINE__` literally, so two uses in one scope would collide.
Nothing uses it yet.

## 3. Ranked fixes (gain / risk), to confirm against the owner's profiled start

| # | Fix | Expected gain | Risk | Visible to the owner? |
|---|---|---|---|---|
| **F1** | **Stop reinstalling OrcaFilamentLibrary on every start.** Install it, like other vendors, only when it is missing, the resource version is newer, or a content stamp differs. The stamp would be written after a successful install; candidates are the resource `.json` version plus a size/mtime digest of the resource library directory, or the app build stamp. Keep the "always" behaviour behind an escape hatch (`ORCA_LIBRARY_ALWAYS_INSTALL=1`) | **~1 s warm, 4.5-15 s on slow starts** (44% of the owner's starts), plus the library's CBOR cache hit (0.3-1.6 s) | Low-medium. Upstream added "always" so the library cannot drift when its version isn't bumped; the content stamp covers that. Test copies of different builds sharing `%APPDATA%` still reinstall when the library content differs, as they should | No. Hand edits under `system/OrcaFilamentLibrary` would no longer be undone at the next start, only on the next library change |
| **F2** | **Persist the Bambu preset mirror's skip gate.** Save the last clean fingerprint (stamp file under `user/` or app_config), and at startup skip the mirror run when the Bambu Studio folder hasn't changed. The fingerprint listing still runs | ~0.4 s warm, **up to 8-10 s on slow starts** | Low: the gate and its fingerprint are already used for printer switches; only persistence is new | No, except a Bambu Studio change made while EdgeSlicer was closed, which is still picked up because the fingerprint differs |
| F3 | User preset loader: parse files in parallel (TBB, results kept in sorted file order), then run the inherit/apply/store pass sequentially in that order. Defer children of detached parents without re-parsing. Move rather than copy into `presets_loaded`. Build the "Generic" regex in `find_preset2` once (static) | 0.3-0.8 s (the per-folder `parse_ms`/`lookup_ms` will say) | Medium: needs a determinism test for user folders, like `[PresetLoadDeterminism]`, including detached parents (#653) and quarantine | No |
| F4 | Lazy-create the heavy main-frame children the owner rarely lands on: StreamPanel and PrinterWebView (WebView creation), the AssembleView/ShowcaseView canvases, and the UV editor panel. Use the existing `LazyPanelHolder` pattern (Monitor, Printers, Project and Calibration are already deferred) | 1-2 s (the Plater/MainFrame marks will say which) | Medium: code that assumes the pointers are non-null; the tab index mapping (tpMonitor etc.) | First open of those tabs/views becomes slower by what it saves now |
| F5 | Whatever dominates the Sidebar ctor or the `select_technology`-to-`set_current_panel` gap once measured (likely `q->Layout()` or `apply_color_mode()` over the whole sidebar, or `menus.init`). Usually avoidable repeated layout/refresh work | 0.5-1.5 s | Low-medium | No |
| F6 | Defer the post_init `CallAfter` pieces that need not run before first idle: build `WebPresetDialog` (vendor family scan) on first use, and run updater sync and the version check after settle (timer) | 1-2 s of "settled" (not of "shown") | Low-medium: the wizard check must still run first; the update dialog appears a moment later | Update notice may appear a second or two later |
| F7 | GL first-frame costs: imgui `new_frame` (~0.8 s, likely the font atlas with large glyph ranges) and the first test render. Build glyph ranges lazily / only the UI language's | ~0.5 s | Medium (fonts in all languages, CJK) | No |
| F8 | UltraNet plug-in sync reads both copies of the DLL and BambuSource whole for a byte compare on every start. Compare a stored size+mtime stamp (or hash once per version) | 0.05-0.3 s (more when cold) | Low | No |
| - | File associations "once per version" | ~0 (2 ms in the logs) | - | Not worth doing |
| - | PresetUpdater on a background thread | Unnecessary after F1. Racing `load_presets` against a vendor reinstall is risky | High | - |
| - | Repeated preset re-selection / notifications | No evidence in the logs so far. `load_current_presets` is called once at startup; the new per-tab marks will show if a tab reload is slow | - | - |

All of these are startup-only and change no G-code. F3 touches the preset loader, so it gets a gate run
anyway (no expected diff).

## 4. PR split

1. **This PR (draft): instrumentation only.** Owner runs the test copy once with the variable set.
2. **F1 library reinstall stamp** (`PresetUpdater.cpp`, plus a unit-testable decision function and test).
   Small, biggest win. Can start before the owner's numbers.
3. **F2 persistent mirror gate** (`GUI_App.cpp` / `PresetMirrorCore`, with a test of the gate round trip).
4. **F3 user preset loader** (`Preset.cpp`, `test_preset_load_determinism.cpp` extended to user folders).
5. **F4 + F5 main frame** (lazy Stream/PrinterWebView/Assemble/Showcase/UV editor; Sidebar/Plater
   hot spot), guided by the marks. One PR per panel family if the diffs grow.
6. **F6 + F7 post-show** (deferred CallAfter pieces, glyph ranges), last, after re-measuring.

## 5. Overlaps

- **4B-2 port (`C:/Dev/wt_pkg_4b2`, running in parallel):** it edits `GUI_App.cpp` `on_init_inner`
  (splash/progress around the `load_presets` call, the mainframe creation and show) and
  `PresetBundle.cpp` (`load_system_presets_from_json`, `load_vendor_configs_from_json`, a new block near
  `copy_files`). This PR adds single mark lines next to `new MainFrame()`, after `load_presets`'
  neighbours, and in `load_user_presets`. Expect trivial textual conflicts at most; whichever merges
  second resolves them. F3 and F1 don't touch 4B-2's code paths.
- The CBOR cache stays on by default (owner decision). F1 is what lets the library's cache entry survive
  between starts.

## 6. Owner's profiled start (to fill in)

One cold-ish start of `C:/Dev/EdgeSlicerTest/startup_prof` with `ORCA_STARTUP_PROFILE=1`. Then
`grep "\[StartupProfile\]"` the newest log in `%APPDATA%\EdgeSlicer\log`. Paste the numbers here and
re-rank section 3.
