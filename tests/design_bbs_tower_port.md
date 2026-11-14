# Bambu Studio 2.9.1 tower nozzle-change ramming port (Type1 tower)

Owner go 2026-11-14. Branch `port/bbs-tower-ramming`, worktree `C:/Dev/wt_bbstower` (from origin/main f34257f97f).
Reference: `C:/Dev/BambuStudio` tag `v02.09.01.70` (BBS 2.9.1; that clone's master is 2.8.4).
Follow-up to draft PR #1046 (section 2 of its body: "Tower post-extrusion pre-cooling"); independent of its branch.

## 1. What Bambu Studio 2.9.1 does

- `Print::_make_wipe_tower()` always calls `WipeTower::generate_new()`: the block planner
  (`WipeTowerBlock` per adhesiveness category, `plan_tower_new`, `finish_layer_new`, merged slabs,
  band ladders, side scrape, tower ironing ...). That is the 8579-line file.
- The nozzle-change part of it is small and self-contained:
  - **planning** (`set_toolchange()` / `plan_toolchange()`, WipeTower.cpp 3085-3180): when the two
    filaments sit on different nozzles (`is_need_ramming()` = `!are_filaments_same_nozzle`), the
    change reserves `nozzle_change_depth` = ceil(ramming path length / ramming line length) x ramming
    width, on top of the purge depth. The path length is `filament_change_length` (extruder change) or
    `filament_change_length_nc` (hotend change on one extruder) of the **outgoing** filament, divided by
    the extrusion per mm of a ramming line (`prime_tower_ramming_line_width`, 0 = table 0.2->0.5,
    0.4->1.0, 0.6->1.2, 0.8->1.4).
  - **generation** (`tool_change_new()` 3542-3600 picks one of three, `ramming()` 3727-3920,
    `heater_off_nozzle_change()` 3922-3942, `idle_travel_cooling()` 3944-4054): a
    `NozzleChangeResult` (its own G-code, start/end point, wipe path, flags) stored in the
    `ToolChangeResult` of the change.
    - `ramming()`: `; NOZZLE_CHANGE_START OF NF ON NN`, for a hotend change on one extruder
      `M632 S<f> [H<n>] M N` + optional `M400`/`M104 ... N0 C ;Wipe tower nozzle change pre cooling` +
      `M106 S255` + `M633`; then the ramming lines of the outgoing filament (speed = ramming volumetric
      speed, stretched so the ramming lasts the pre-cool time `precool_t` = (print temp - pre-cooling
      temp) / `hotend_cooling_rate` on an extruder change), bridge flow over floating areas; then (hotend
      change) a second `M632`, a reverse travel of `filament_ramming_travel_time(_nc)` seconds over the
      ramming lines when that is > 0, else a wipe path back along the last line; `M633`; END marker.
    - `heater_off_nozzle_change()` (handle_hotend_as_extruder, change length 0, not the first layer):
      START/END markers only, flagged `heater_off_only`.
    - `idle_travel_cooling()` (the same, with `filament_idle_travel_cooling_enabled`): START, `M104`
      to the pre-cool target (or 0), `M632 S<f> D`, travel-only zig-zag over the new filament's purge
      area timed by `hotend_cooling_rate`, `M633`, END; flagged `zero_flush_idle_cooling`.
- **emission** (`GCode.cpp` append_tcr 851-944): the result goes into the `[filament_end_gcode]` slot,
  before `filament_end_gcode` and `change_filament_gcode`:
  - ramming: short retract at the model -> travel to the ramming start -> unretract -> ramming G-code ->
    wipe along the ramming line + the long toolchange retract (the "phase 2" retract);
  - heater off / idle travel: `M106 S255` first and the long toolchange retract at the model; idle travel
    travels to its start without unretracting and adds no retract after.
- With extrusion between START and END, the pre-cooling injector (already ported:
  `PreCoolingInjector.cpp`, `ext_cooling_rate_post_extrusion`) writes
  `M104 ... ;Multi extruder pre cooling in post extrusion` inside the window. That is the ~1398 lines of
  the owner's H3C file (`h3ctime_bbs.gcode.3mf`: every change away from the PETG, the one filament with
  `filament_change_length` 5).

## 2. What EdgeSlicer has

- Our Type1 tower (`WipeTower.cpp`, 2963 lines) is the BBS 2.0-era planner: changes stacked in Y per layer
  (`m_depth_traversed`), the wall printed by the first toolchange, no blocks, no nozzle-change depth.
- `GCode.cpp format_nozzle_change_block()` writes exactly `ramming()` with zero lines (markers, `M632`,
  pre-cool, `M633`) and `nozzle_change_gcode()` adds `M106 S255` on hotend_as_extruder printers.
- The five keys (seven with the `_nc` twins) are not in `PrintConfig`, so the loader drops them although
  869 BBL filament profiles carry them.

## 3. Scope: port the nozzle-change machinery into our planner, not `generate_new()`

Porting `generate_new()` and the block planner would rewrite every Bambu tower (X1/P1/A1 included:
BBS 2.9.1 uses it for all printers) and collide with Edge's own tower work (Local-Z / mixed colour
sub-layer replay #937, AdaptiveLayerMerge slabs, rib walls, tower interface, sparse layer
combination). It is not needed for the ramming + cooling structure. This port brings in:

| BBS 2.9.1 | Edge |
|---|---|
| `WipeTowerInfo::ToolChange::nozzle_change_depth/_length`, `set_toolchange()` ramming depth | `plan_toolchange()` takes a `NozzleChangePlan` (Print fills it from the nozzle grouping); depth added to `required_depth`, kept through `plan_tower()` extra spacing, the first-layer solid fill and `make_rib_tower_square()` |
| `FilamentParameters::max_e_ramming_speed`, `precool_t`, `precool_t_first_layer`, `precool_target_temp`, `ramming_travel_time`, `idle_travel_cooling_enabled`, `m_filaments_change_length` | same fields, filled in `set_extruder()` |
| `NozzleChangeResult` (+ `ToolChangeResult::nozzle_change_result`, `merge_tcr` carry) | same |
| `ramming()` | ported; box = our Y stack (`m_perimeter_width + m_depth_traversed`), `solid_infill` always false (our tower has no Contact block layers); bridge flow through our `need_thick_bridge_flow()` |
| `heater_off_nozzle_change()`, `idle_travel_cooling()` | ported (H3C) |
| `tool_change_new()` mode choice | at the top of our `tool_change()` |
| `LimitFlow::LimitRammingFlow(NC)` | writer `set_e_speed_limit()` around the ramming lines |
| `WipeTowerWriter::format_line_M104` (`N0 C`, `T<phys>` or `I<filament>`) | tower writes the same line |
| append_tcr nozzle-change emission | ported into our append_tcr; `format_nozzle_change_block()` stays the fallback when the tower made no result |

**Not ported (documented, not needed for the structure):** `generate_new()` / blocks / categories,
`get_block_gap_width()` (wipe_tower infill gap: we use the ramming width), `is_valid_last_layer()`
(per-extruder printable height, skips ramming on an extruder's last layer above its height),
`filament_prime_volume_nc` and grab-length purge reduction, side scrape, `prime_tower_ramming_line_width`
(key not in our config; the BBS default 0 = the table, which is what we use), BBS's PVA command-only rule
(kept: one `if`, cheap; see code).

## 4. The keys

All filament keys, BBS 2.9.1 definitions and defaults (PrintConfig.cpp 3094-3112, 3424-3444, 3640-3658):

| key | type | default | variant (flow slot) |
|---|---|---|---|
| `filament_change_length` | floats | 10 | no |
| `filament_change_length_nc` | floats | 10 | no |
| `filament_ramming_travel_time` | floats nullable | 0 | yes |
| `filament_ramming_travel_time_nc` | floats nullable | 0 | yes |
| `filament_ramming_volumetric_speed` | floats nullable | -1 (= max volumetric speed) | yes |
| `filament_ramming_volumetric_speed_nc` | floats nullable | -1 | yes |
| `filament_idle_travel_cooling_enabled` | bools | true | no |

- Added to `s_Preset_filament_options`, `filament_flow_variant_options()` (the variant ones), the
  psWipeTower invalidation (with `enable_pre_heating`, `filament_pre_cooling_temperature(_nc)`,
  `hotend_cooling_rate`, which the tower now reads), and the filament Tab next to "Wipe tower cooling"
  (BBS shows them on its filament page: ramming length, travel time after ramming, pre-cooling target,
  idle travel cooling, ramming volumetric speed).
- **Profile impact:** no profile edits. The BBL profiles already carry the values (H2D/H2C PLA Basic 4 mm,
  H2C `_nc` 4 mm + 1 s reverse travel, H3C PETG Basic 5 mm, most H3C filaments 0); they now load.
  Filaments without them (OrcaFilamentLibrary, user presets) get BBS's defaults, like in Bambu Studio.
  `WipeTowerEstimate` already reads `filament_change_length` (default 0 until now), so the plate's tower
  footprint estimate on two-nozzle printers grows with the real tower.

## 5. Which printers change

- Ramming is planned only when **Print::is_BBL_printer()** and the nozzle grouping puts the two filaments
  on different nozzles. Non-Bambu printers (U1, Voron, Type1 overrides) and single-nozzle Bambu printers
  (X1/P1/A1/H2S/P2S/A2L) keep identical G-code (CONFIG_BLOCK gains the keys).
- **H2D / H2D Pro / X2D:** every extruder change rams `filament_change_length` of the outgoing filament
  on the tower before the swap; tower deeper by the ramming lines.
- **H2C:** extruder changes as above; rack hotend changes on the right extruder ram `_nc` (with `M632` /
  pre-cool / `M633` around them, 1 s reverse travel on Bambu PLA Basic).
- **H3C** (handle_hotend_as_extruder): filaments with a change length (PETG Basic 5 mm) ram; the rest keep
  the heater-off markers (`M106 S255` + START/END), or idle-travel cooling when the filament enables it.
  First layer: BBS rams zero lines (markers, travel + unretract/retract) instead of heater-off.
- Every Bambu printer with a ramming change: the pre-cooling injector now writes its
  "post extrusion" cool-downs inside the window, as in BBS.

## 6. What stays (Edge changes)

- H2C `M620.14` wait point (#967) and wipe-avoid placeholders: untouched (change_filament_gcode side).
- Timelapse case-B placement (#986): untouched; the photo still follows the template, before the tower
  travel. The ramming happens before the template, like BBS.
- Mixed colour sub-layer / Local-Z Type1 replay (#937): Local-Z changes are direct (no tower result), so
  they keep `format_nozzle_change_block()`; the planner only adds depth to planned tower changes.
- handle_hotend_as_extruder (#1042): flush tracking, prime volumes and the `FilamentChangeGCode` markers
  untouched; the heater-off path is now produced by the tower (same G-code on layers > 1).
- AdaptiveLayerMerge / ContourZ / ZAA: no change; ramming uses the tower layer height like the purge.
- Edge per-nozzle widths (`tool_line_width`), rib wall, tower interface, sparse layer combination,
  `M73 E` (#1046, separate PR).

## 7. Stages (one commit each, one build)

1. Keys + GUI + invalidation. Gate: CONFIG-ONLY everywhere.
2. Planner depth + `NozzleChangeResult` generation (`ramming()`), writer ramming limit. Unit tests.
3. GCode emission in append_tcr. Unit + G-code tests (H2D/H2C/H3C).
4. H3C heater-off / idle-travel cooling. Unit tests.

Tests: `[BBSTowerRamming]` in fff_print_tests (depth formula, line count, speed stretch, M632/M633 order,
reverse travel, heater-off / idle-travel output, X1C two-filament unchanged, non-BBL unchanged).

Gate: standard set + `h3c_benchy_2f` (PLA/PLA), `h3c_petg_2f` (PLA/PETG), `x1c_benchy_2f`, plus the
H2C BBS 2.8.2 project (`tests/h2c_wrongextruder_edge1.3mf` plate 4) for the BBS structure comparison.
Expected diffs: h2d_benchy_2n, h2c_benchy_2n, h3c_petg_2f; everything else identical or CONFIG-ONLY.

BBS comparison: Bambu Studio 2.9.1's CLI (`C:\Program Files\Bambu Studio`, scratch `--datadir`,
flattened system presets) slices a single-filament OBJ but segfaults on any two-filament H2D plate and on
3MF input, so the comparison uses Bambu's own files: the owner's H3C 2.9.1 project
(`h3ctime_bbs.gcode.3mf`) and the 2.8.2 H2C file (`tests/h2c_wrongextruder_bambu_manual.gcode.3mf`;
ramming() is unchanged in structure from 2.8.2). Compared: block order, line count, widths, F,
M632/M633 placement, retract/wipe around the block.
