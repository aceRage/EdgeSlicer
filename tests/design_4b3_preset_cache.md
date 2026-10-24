# 4B-3 System preset cache: design note (no code)

Package 4B-3 of `tests/orca_port_plan_remaining.md` (the "4B split" table, step 3). Upstream: OrcaSlicer
#14217 "Cache system presets to eliminate startup and wizard load times" (`5ed56eb876`, +4584/-569,
35 files) and its follow-up #16175 "Create Printer finds no system vendors in release builds"
(`c33515914d`, +190/-81, 6 files). Written 2026-10-24 against origin/main `0b3af03608`.

## Summary for the owner

- **Our own cache isn't the one the plan names.** #255 only made the cache's writes atomic. The CBOR
  cache came in with perf/startup (`bc3b7ddfdb`, merged in `59c9b76c21` on 2026-09-13). Its key moved
  from a content hash to file stats in `12c96c77a3`. That same merge already loads vendors in parallel
  with a pumped splash, which is most of what 4B-2 (#13667) ports.
- **On today's code our CBOR cache makes loading slower, not faster.** Measured without the GUI
  (section 3): with the cache on, loading your 7 installed vendors costs 2.39 s of summed per-vendor
  time; with `ORCA_PRESET_CACHE=0` it costs 1.82 s. BBL, the slowest vendor and so the end of the
  parallel stage, takes 1.01 s with the cache and 0.76 s without. The stat walk that builds the key
  (0.40 s), the CBOR decode (0.19 s) and copying the documents out of the decoded tree cost more than
  the JSON parse they skip.
- **What is left to win is small.** The system-preset load takes about 1.9 s of wall time in your own
  GUI logs from 2026-10-23, out of roughly 12.8 s to first idle. Turning our cache off should save about
  0.3 s of that. An upstream-style binary cache could save at most about 0.4 s more, because it still
  resolves inheritance, normalizes and stores every preset at load, as we do.
- **Recommendation: skip #14217 and #16175.** Their design ships each vendor as a `.opc` file *instead
  of* its JSON. That breaks our validators, the gates, the CLI's presets-by-name, the wizard's raw
  JSON scan, the Polymaker H2C install check and hand copies into `system/`. It also keys staleness on
  the vendor version, which our dev and test-copy workflow does not always bump. Instead, do a small
  4B-3a: confirm in the GUI harness that the cache is a loss, then turn it off by default (S, one
  line plus a test change). Spend the 4B effort on the unattributed ~3.5 s startup step and on user
  preset loading (section 5).

**Decision needed:** (a) skip the upstream port, (b) approve 4B-3a (measure in the hidden-instance
harness, then default `ORCA_PRESET_CACHE` off, or delete the cache code if you'd rather), and
(c) whether to re-point 4B's remaining startup effort at the "hot-bed rules + default suppression"
step and user-preset loading. One measurement only you can make: the cold-boot case (section 3.4).

---

## 1. What upstream's cache does (#14217 + #16175)

### Design in one paragraph

Each vendor's system presets are serialized into one binary `<vendor>.opc` file. On a shipped build
the `.opc` **replaces** the vendor's JSON tree: CI generates the caches after the build, then deletes
every preset JSON, the vendor profile included. On a cache hit the file walk and JSON parse are
skipped, but every entry is still "installed" through the JSON path's code: inheritance against the
loaded presets and the filament library, `include` layering, flattening onto collection defaults,
normalize, validation, registration, and alias and lookup maps. Entries hold presets in *source form*
(their own diff plus `inherits`/`include` names), never resolved values.

### Format

- Fixed header (magic, `CACHE_VERSION`, payload size, CRC32), then one binary stream. Stamps come
  first (format version, vendor name, vendor version) so the updater can read a vendor's version from
  the first kilobyte.
- A per-file dictionary of the distinct `opt_key`s, the `ConfigOptionType` each was written as, and
  the distinct enum value names. Each option is a `uint16` index plus a typed value, so options are
  keyed by name and not by `serialization_key_ordinal`. An option this build doesn't know is dropped,
  the way a JSON load drops it.
- The `VendorProfile` (cereal `serialize()` added to `VendorProfile`, `PrinterModel`, `Semver` and
  `Preset`), then three entry lists (process, filament, machine) and the parse error count.

### Invalidation

Gates, all of which must pass or the vendor is parsed instead:

1. Integrity: magic, size, CRC.
2. `CACHE_VERSION`, bumped by hand only when the layout changes. Config-schema drift is deliberately
   **not** a gate.
3. Vendor name, and a cache version >= the profile version on disk. The check is skipped when there is
   no profile beside the cache (cache-only install).
4. Every entry installs. One failure rejects the whole vendor.

There is no content or stat check. Changing a preset without bumping the vendor version leaves the
old cache in use. A filament-library update changes what a cache *produces* (inheritance resolves by
name at load), not whether the cache is valid.

The wizard has a second cache, `<data_dir>/cache/wizard_profile_data.json`: the derived
model/machine/filament/process catalog, stamped by each vendor's name and version. Upstream measured
the wizard at ~0.2 s with it against ~2 s without, and ~29 s on their old raw scan.

### Where it plugs in, file by file

| Upstream file | Change |
|---|---|
| `src/libslic3r/PresetCacheFormat.{hpp,cpp}` (new, ~780 lines) | The bytes on disk: dictionary, config wire format, header and stamps, `CachedPreset` entries, `VendorCacheFile` save/load/peek, bounded stamp reads |
| `src/libslic3r/PresetBundle.{hpp,cpp}` (869 + 73 changed lines) | `load_vendors` (library first, other vendors read in parallel, installed in parallel into their own bundles, merged in stable order); serve-or-parse per vendor; `install_vendor` split into `resolve_vendor_preset` (parallel, read-only) and `commit_vendor_preset` (ordered); write-back after a parse; `clear_printer_hold_aliases`; `set_generate_vendor_caches` |
| `src/libslic3r/Preset.{hpp,cpp}`, `Semver.hpp`, `Config.hpp`, `PrintConfig.hpp` | cereal `serialize()` for `Preset`/`VendorProfile`/`PrinterModel`/`Semver`; an assert becomes a "cache is stale" throw |
| `src/libslic3r/Utils.hpp`, `utils.cpp` | Vendor discovery reads `.json` **or** `.opc`; `installed_vendor_version`; install copies whichever form is newer and deletes the other |
| `src/slic3r/Utils/PresetUpdater.cpp` | Update and reinstall decisions use the installed version from either form; a cache this build can't read counts as "not installed" |
| `src/slic3r/Config/Snapshot.cpp` | Snapshot version comes from `installed_vendor_version` rather than a full vendor load |
| `src/slic3r/GUI/WebGuideDialog.{hpp,cpp}` (+456) | Wizard builds its bundle through `load_vendors` (installed dir + resources) and caches the derived catalog JSON |
| `src/slic3r/GUI/ConfigWizard*.{cpp,hpp}`, `CreatePresetsDialog.cpp` | Read vendors through the cache; #16175 makes Create Printer find vendors when only `.opc` ships |
| `src/slic3r/GUI/GUI_App.cpp` (+6) | Wiring |
| `src/dev-utils/generate_system_cache.cpp`, `src/dev-utils/CMakeLists.txt` | Generator tool |
| `scripts/build_preset_cache.{sh,bat}`, `.github/workflows/build_{all,orca}.yml`, `build_linux.sh`, flatpak manifest, `.gitignore`, `.gitattributes` | Generate caches after the build, copy them into the package, **prune the preset JSONs** (`--prune-source` for the checkout) |
| `docs/HLSD/preset-cache.md` | 400-line design doc |
| `tests/libslic3r/test_vendor_cache.cpp` (1620 lines), `test_preset_bundle_loading.cpp` | Cache round-trips, gates, listing-order runs, corrupt-file handling |

## 2. What we already have

### Our CBOR cache (perf/startup, not #255)

- **Origin:** `bc3b7ddfdb` "Preset load: parse a vendor's files in parallel, and cache the parsed
  documents as CBOR", merged with perf/lazy-panels in `59c9b76c21` (2026-09-13). `12c96c77a3`
  changed the key from a content hash to `(relative path, size, std::filesystem mtime)` for every
  `*.json` under the vendor directory, plus `Snapmaker_VERSION` and a format version. #255
  (`336058b43d`) made the cache writes, and config and preset writes, atomic.
- **Format:** `<data_dir>/cache/presets/<vendor>.cbor`. Each section's post-include JSON documents
  are stored in file order beside their relative path. On a hit, the documents go into the same
  sequential `parse_subfile` loop a fresh parse feeds. The JSON tree stays the source of truth.
- **Invalidation:** any stat change under the vendor directory, a different app version, or a section
  whose path list no longer lines up. A decode failure deletes the file and falls back to parsing.
  Escape hatches: `ORCA_PRESET_CACHE=0` and `ORCA_PRESET_LOAD_SEQUENTIAL=1`.
- **Where it plugs in:** inside `PresetBundle::load_vendor_configs_from_json`, about 350 lines in one
  function. Nothing else knows the cache exists.
- **Also in that merge:** parallel vendor loading. The library loads first, the other vendors load
  into their own bundles under `tbb::parallel_for` from a helper thread while the splash is pumped,
  and the results merge in vendor order. Each vendor's files are also parsed in parallel. The test
  `[PresetLoadDeterminism]` covers both.

### 4B-1 (#525)

4B-1 doesn't touch the load path. It ported #15830 (skip the second tab reload in the startup
`new_project`, -0.31 s) and the `Preset.cpp` part of #14437. Its timings put
`preset_bundle->load_presets` at 3.2-3.8 s out of ~12.8 s to first idle, behind the "hot-bed rules +
default suppression" step at ~3.5 s.

### Overlap and gap

| | Upstream #14217 | Ours today | Gap |
|---|---|---|---|
| Skip file walk + JSON parse on a hit | yes | yes, but the key still stats every file | same idea, but our key isn't cheap |
| Skip JSON-to-config deserialization | yes (typed binary) | **no**: cached JSON documents still go through `load_from_json_document` | ~0.25 s on BBL |
| Inherit, normalize, validate, store, aliases at load | still done | still done | none |
| Parallel vendors, ordered merge | yes (`load_vendors`) | yes (perf/startup) | none; we also pump the splash |
| Parallel inherit/resolve within a vendor | yes (resolve/commit runs) | no (sequential loop) | moderate, complex |
| Staleness | vendor version | file stats + app version | ours is safer for dev/test edits |
| JSON removed from the package | **yes** | no | we don't want this (section 4) |
| Wizard catalog cache | yes | no; our wizard scans raw JSON with TBB (`2d10f4eb96`, July) | wizard time not measured (GUI) |
| Installer/updater understands the cache | yes | not needed | none |

## 3. Measurements

### 3.1 Method

- **No GUI.** The CLI's presets-by-name path (`NamedPresets` in `src/Snapmaker_Orca.cpp`) calls
  the same `PresetBundle::load_vendor_configs_from_json`, with the CBOR cache and the
  `ORCA_STARTUP_PROFILE=1` breakdown (deserialize / inherit / normalize+validate / store, cache key,
  CBOR decode). I ran the shared `_baseline` install (main `1009229746`) as
  `EdgeSlicer.exe --slice 0 --datadir <scratch> --filament-presets "<one instantiated filament per vendor>;__stop__"`.
  The final unknown name makes it exit once every listed vendor has loaded, before any slicing.
  Scratch data dirs only; %APPDATA% wasn't used. The scripts stay in the session scratchpad and
  aren't committed.
- **Limitation:** the CLI loads vendors **one after another**, so its totals are summed per-vendor
  times, not GUI wall time. The GUI loads the library first and then all other vendors in parallel, so
  its wall time is about library + slowest vendor (BBL) + merge. The validator exe that would run the
  GUI's parallel path headless is stale (2026-09-20 build; it rejects current BBL X2D files), and a
  fresh build wasn't worth a build slot for a design note.
- **Load:** the local build slot was compiling another agent's job throughout. Cached and uncached
  runs were interleaved, so both saw the same load.
- **Sets:**
  - "Owner set": the 7 vendors installed in your data dir (OrcaFilamentLibrary, BBL, Custom, Elegoo,
    Flashforge, Qidi, Snapmaker), 7,808 JSON files.
  - "Full set": every vendor in `resources/profiles` that has filaments (45 + the library), about
    13,000 of the 13,484 JSON files. The 13 vendors without a filament list aren't reachable by
    filament name.

### 3.2 Owner set, 4 interleaved runs each (summed per-vendor ms, median)

| Bucket | Cache on (hit) | Cache off | Notes |
|---|---|---|---|
| Cache key (stat every file) | 400 | - | |
| CBOR decode | 195 | - | |
| File walk + parallel JSON parse, or cache read + copy out | ~470 | ~490 | cache read and copy cost as much as the parse it replaces |
| Deserialize (`load_from_json_document`) | 495 | 500 | runs either way |
| Inherit (parent copy + apply) | 320 | 323 | |
| Normalize + `remove_invalid_keys` + model/variant checks | 288 | 287 | |
| Store (`load_preset`, aliases, `config_maps` copy) | 222 | 217 | |
| **Total** | **2,392** (2,342-2,474) | **1,820** (1,768-1,859) | **cache costs +0.57 s** |
| CLI process wall | 3.26-3.61 s | 2.64-2.97 s | |

Per vendor (total ms, 4 runs):

| Vendor | Cache on | Cache off |
|---|---|---|
| BBL (2,868 files) | 1040, 1016, 1003, 1001 | 766, 772, 744, 760 |
| OrcaFilamentLibrary (970) | 273, 258, 246, 250 | 187, 187, 186, 206 |
| Qidi (1,816) | 437, 426, 430, 399 | 327, 316, 305, 327 |

### 3.3 Full set (summed per-vendor ms)

| Mode | Runs | Total |
|---|---|---|
| Cache hit | 3 | 4,489 / 4,460 / 4,323 |
| Cache off | 3 | 3,428 / 3,334 / 3,354 |
| Cache miss (parse + write) | 3 | 7,114* / 4,087 / 4,225 |

\* First run, the first time these files were read since the OS file cache last held them: about
+3 s. It's the only cold-file-cache data point (section 3.4).

Breakdown with the cache off (one run): deserialize 966, inherit 613, normalize/validate 520, store 394.
That leaves ~0.84 s for the walk and parse.

### 3.4 What the GUI sees

From your own logs on 2026-10-23 (log timestamps, numbers only): `load_presets` enter to
`load_user_presets` took **1.88 s and 1.90 s** on two starts. The library took 0.28-0.61 s and the six
parallel vendors 1.29-1.59 s. On a third start the library cache was rebuilt, and it took **3.16 s**
(library 1.6 s). User-preset loading follows and took about 0.85-1.45 s.

Realistic gains, wall time, warm OS file cache:

| Option | System-preset load | Saved vs today |
|---|---|---|
| Today (CBOR cache, hit) | ~1.9 s | - |
| Cache off | ~1.6 s (library -65 ms, BBL critical path -250 ms) | **~0.3 s** |
| Upstream-style typed binary cache (no parse, little deserialize) | ~1.2-1.3 s (BBL ~0.41 s, library ~0.11 s) | ~0.6-0.7 s, of which ~0.3 s is "cache off" |

So on top of simply switching ours off, the 35-file port would buy about 0.3-0.4 s, roughly 3% of
startup. These are estimates from per-vendor CLI numbers. Contention among the 6 parallel vendors in
the GUI could shift them by ±0.1 s.

**Unmeasured: cold boot.** After a reboot, 7,800 small files come off disk rather than out of the OS
cache, and the one cold data point above added ~3 s on the full set. A single-file cache could help
there. Ours still stats every file for its key, though, which pays part of the cold metadata cost.
Only a GUI start right after a reboot measures it (yours to run if you want: one start with the cache,
one with `ORCA_PRESET_CACHE=0`, each after a fresh reboot, with `ORCA_STARTUP_PROFILE=1`).

## 4. Edge-specific risks of taking upstream's design

| Area | Risk with #14217 as designed | With ours / with "cache off" |
|---|---|---|
| Profile tooling | Shipped builds have no preset JSON. `check_profiles.sh` and the three validators, the G-code gates (`gate.sh` resolves presets by name from `resources/profiles` JSON), the CLI's `NamedPresets` (reads vendor manifests), the wizard's raw `LoadProfileFamily` scan, vendor Download & Open (#446) and the Vendors CSV tooling all assume JSON | unaffected |
| Flow variants (`process_flow_support`, `filament_flow_support`, per-extruder variant arrays, H2D/H2C/U1) | Values are nullable vector options composed at use time (`PresetFlowVariant`). A typed cache must round-trip nil slots, `ConfigOptionEnumsGeneric` (`filament_volume_type`) and our extra enum names exactly. That's new code with no gate | Ours stores the JSON itself, so it's transparent |
| Edge load-time transforms | `SystemPresetTowerKeysScope` (Bambu tower-interface keys mean something else for system presets), `Preset::ForeignKeyReportScope`, legacy-key handling and the "collapse redundant array for scalar" fix-ups all run during deserialization. Upstream's cache is meant to **survive app upgrades** (no schema fingerprint), so values transformed by an older build would be served by a newer one whose transforms differ | Ours re-runs deserialization every time and keys on app version |
| U1 HF presets, U1 PA off | Same as flow variants; any wrong slot changes G-code silently | transparent |
| Project presets (#357/#384), detached user presets (#653) | Not cached in either design; they inherit from system presets after the load. Upstream's `clear_printer_hold_aliases` and the reset-before-fallback path would need checking against our `reset_project_embedded_presets` | unaffected |
| Vendor updates / version bumps | Staleness keyed on vendor version only. A preset edited without a bump keeps loading the old cache. Our `profile-change` rule bumps on merge, but dev trees, test copies, ported upstream profiles and hand fixes often don't | stat key catches any edit |
| Polymaker H2C install check | The check (and the owner's OK to copy presets into `system\BBL`) assumes JSON files there. With a `.opc` at an unchanged version, hand-copied JSON beside it loses to the cache (gate 3), and the copy has no effect | stat key sees the copied files |
| Several installs sharing %APPDATA% (main install + test copies) | `.opc` files live in `<data_dir>/system/`, shared by every install. A test copy built from a branch with a different `CACHE_VERSION`, or a cache-only vendor it can't read, triggers "not installed" and a reinstall from *that copy's* resources, and test copies can then rewrite the main install's vendors | Ours: the key includes `Snapmaker_VERSION`, so two installs of **different** versions alternate misses and rewrite the cache (~11 MB) on every switch. Test copies of the same version share it harmlessly. Your logs show 2.4.7.0 test copies only, so no thrash seen |
| Mac/Linux | Generation must run inside the Mac DMG and AppImage packaging, before signing on Mac. The flatpak-style build step and the read-only `.app`/AppImage resources need the "cache is the installation" path to work, and our Mac session builds DMGs by hand | Ours uses `std::filesystem` mtime (ns on APFS/ext4); no packaging step |
| Size / merge cost | 35 files, +4.6k lines, and `PresetBundle.cpp`'s load path is where our locale fix #222, quarantine, OrcaFilamentLibrary manifest scan, renamed-parent lookups (#494) and flow-variant code live | - |

The upstream wizard catalog cache is the one piece that would fit us without pruning: it caches
derived data, is stamped by vendor versions, and lives under `cache/`. We have no measurement that our
TBB-parallel wizard scan is slow, though.

## 5. Recommendation

**Skip #14217 and #16175.** The saving left after 4B-1 is ~0.3-0.4 s on top of what switching our
cache off gives, and taking it means dropping JSON from the package, which our whole profile workflow
depends on. #16175 only fixes a bug that pruning creates.

**Adapt ours instead, smallest first:**

| PR | Scope | Size | Expected gain |
|---|---|---|---|
| **4B-3a** (recommended) | Measure in the 4B-1 hidden-instance harness: startup with `ORCA_PRESET_CACHE` unset vs `=0`, 5 alternating runs on a scratch copy of your 7 vendors. If it confirms, default the cache **off** (keep `ORCA_PRESET_CACHE=1` to opt in), adjust `test_preset_load_determinism.cpp`'s cache assertions to the new default, and leave the existing `cache/presets/*.cbor` files alone (11 MB; or remove them once). No G-code impact; gate identical | S | ~0.3 s per start; also removes the 3 s rebuild starts |
| 4B-3b (optional, only if the cold-boot number says so) | Keep a cache, but make it pay: key on the vendor manifest's version plus the vendor directory's newest mtime from one directory walk, and store typed configs rather than JSON documents (upstream's name-keyed dictionary idea, stored under `cache/`, **never** replacing JSON, rejected on app version). Same determinism test, plus a round-trip test over every Edge flow-variant and nullable option | M-L | ≤0.3-0.4 s warm; cold boot unknown |
| 4B-3c (optional, separate) | Wizard catalog cache as upstream did it, if a timed wizard open shows it's slow | M | wizard only |

**Better targets for the 4B startup budget** (from 4B-1's breakdown and your logs):

- The "hot-bed rules + default suppression" step, ~3.5 s. Despite its name, that mark also covers
  `PresetUpdater` construction and Windows file associations; what inside it costs the time
  isn't known. It needs a sub-split before anyone optimizes it.
- User-preset loading, ~0.9-1.4 s. It is sequential, with a `load_info` per preset, and no system
  cache touches it.
- Main frame construction, ~2.0 s.

**Note for the lead on 4B-2:** perf/startup (`59c9b76c21`) already loads vendors in parallel and
pumps the splash from the calling thread. #13667 / #14155 may come down to the progress-bar UI.

## Decision the owner needs to make

1. Skip the upstream port of #14217 + #16175: yes / no.
2. Approve 4B-3a (GUI-harness confirmation, then the CBOR cache off by default): yes / no. Or
   remove the cache code outright.
3. Optional: run the cold-boot comparison (section 3.4) before deciding on 4B-3b.
4. Re-point the rest of 4B startup work at the ~3.5 s "hot-bed rules" step and at user-preset loading:
   yes / no.
