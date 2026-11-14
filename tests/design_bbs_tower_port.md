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
(key not in our config; the BBS default 0 = the table, which is what we use). BBS's PVA rule (no
cooling stretch for PVA on hotend_as_extruder printers) is ported.

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
2. Tower: planner depth, `NozzleChangeResult` generation (`ramming()`, `heater_off_nozzle_change()`,
   `idle_travel_cooling()`), writer ramming limit, Print's per-change nozzle plan. On its own this stage
   reserves the ramming depth and generates the results, but nothing writes them yet.
3. GCode emission in append_tcr, tests (`[BBSTowerRamming]`: unit + H2D / X1C G-code).

Stages 2 and 3 are built together (the gate of stage 2 alone would show a deeper tower with a gap).

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

## 8. Material blocks (owner question 2026-11-14): a separate stage

**What it is.** In Bambu Studio 2.9.1 every tower layer is split into `WipeTowerBlock`s, one per block key
(`get_block_key()` = `filament_adhesiveness_category` + nozzle diameter + lane, 59 hits in WipeTower.cpp).
`generate_wipe_tower_blocks()` gives each block a fixed Y band for the whole print (its deepest layer),
`tool_change_new()` purges a filament inside its own category's band, `ramming()` rams inside the
**outgoing** filament's band, and `finish_block()` / `finish_block_solid()` fill each band's unused part
with a filament of that same category (Contact / Solid layers under a wider band). So PLA never bonds
onto PETG/TPU/support material inside the tower.

**Why it is not in this port.** It is not an add-on to our planner; it *is* `generate_new()`:
`plan_tower_new()`, `generate_wipe_tower_blocks()`, `finish_layer_new()`, `finish_block(_solid)()`,
`get_block_fill_filament()` / `same_category_filler()`, the Contact / Solid layer types, the per-band
infill gap (`calc_block_infill_gap`), plus the merged-slab / band-ladder / lane code those functions
now carry (`plan_merged_slabs`, `plan_tower_ladder`, `m_lane_assignment`) - roughly 4-5k of the 8.6k
lines. Porting it replaces our `generate()` / `finish_layer()` / Y-stack for **every** Bambu tower:
X1/P1/A1 change too (two categories split into two bands; even one category gets `finish_layer_new()`'s
layout), whereas this port changes only nozzle-change printers. It also needs a key we lack:
`filament_adhesiveness_category` (BBL base profiles carry 100 PLA, 300, 400, 500, 704, 801, 902...;
our loader drops it, and `WipeTowerEstimate` already models "one block per category" with it at 0 -
the plate's tower estimate is the block layout, the generated tower is not).

**Recommendation: its own package after this PR merges** ("BBS tower blocks"), staged:
B1 the `filament_adhesiveness_category` key (CONFIG-ONLY); B2 `generate_wipe_tower_blocks()` +
`tool_change` / `ramming()` writing into bands (`block->cur_depth` replaces `m_depth_traversed`;
the ramming then moves from in front of the new purge to the old filament's band, as in BBS);
B3 `finish_layer_new()` / `finish_block(_solid)()` with Contact / Solid layers and same-category fill;
the merged-slab / lane code only where ALM needs it (we have our own AdaptiveLayerMerge slabs).
Gate: every Bambu multi-material case changes; compare band order and fill filament with BBS files.

**Interaction with #937 (Type1 mixed colour sub-layer / Local-Z replay).** The replay prints a layer's
passes with direct filament changes (no tower result) and tells the tower which filament they leave
loaded (`local_z_start_tool` / `local_z_prev_tool`). With blocks: (1) a pass filament has no planned
tower change on that layer, so its band gets no purge there and must be filled by
`get_block_fill_filament()` - if the filament left loaded is of another category, BBS would have to fill
the band with a different-category filament or leave it floating, so the replay has to hand the block
planner the per-layer set of loaded filaments, not just the last one; (2) a replayed direct change onto
another nozzle gets no ramming (it never reaches the tower) - same as today, `format_nozzle_change_block`
markers only; (3) BBS's own mixed sub-layer goes through `slab_height_for()` per band, which our replay
does not use, so the band heights stay the tower layer height.

**Interaction with #1042 (per-filament prime volume, H3C).** Unchanged by blocks in principle:
`plan_toolchange()` already takes the per-filament `filament_prime_volume` (15 mm3 in saving mode) and
the hotend tracker's flush; blocks only decide *where* that depth goes (the new filament's category
band). Two follow-ups for the block package: BBS uses `filament_prime_volume_nc` for a hotend change
on one extruder (not ported here either), and on H3C heater-off / idle-travel changes the new
filament's band still gets its prime purge (idle travel moves over that band, as here).

**This port and blocks.** Nothing here blocks the block package: `NozzleChangePlan`,
`NozzleChangeResult`, the keys, `ramming()` / `heater_off` / `idle_travel` and the append_tcr emission
are the same objects BBS's block planner uses; only the Y origin of the ramming box changes.

## 9. OrcaSlicer already has the newer planner (coordinator note 2026-11-14)

OrcaSlicer main's Type1 `WipeTower.cpp` (5564 lines) is the Bambu block planner: `generate_new()`,
`plan_tower_new()`, `WipeTowerBlock` / `get_block_by_category()` (keyed by `filament_adhesiveness_category`
only), `tool_change_new()`, `ramming()`, `finish_layer_new()`, `finish_block(_solid)()`, used for **every**
Type1 tower (Print.cpp calls `generate_new()`; `generate()` is WipeTower2 there). Source PRs (gh, file history):

| Orca PR | merged | what it brought to the Type1 tower |
|---|---|---|
| #10780 H2D/H2S support | 2025-10-24 | the BBS 2.x tower: blocks per adhesiveness category, nozzle change / ramming, contact & solid layers, TPU reverse travel, skip points (BBS commits 955bb4dea8 "wipe tower support nozzle change", 257fc86ca9 "filaments that do not adhere", bca7f73f7f ...). Our port plan filed it as a profile resync only, so the planner never came over. |
| #14685 Add Bambu H2C and A2L machine support | 2026-07-13 | multi-nozzle engine: per-layer nozzle grouping into the tower |
| #14800 H2C carousel (NozzleStatusRecorder) | 2026-07-22 | ramming depth, M632 barriers, precool / ramming speed / reverse travel parity, per_cooling_max_speed clamp |
| #15035 rib wall and skip points on all printers | 2026-07-31 | "Sync WipeTower from BambuStudio (through ca1881761)" |
| #15485 Port wipe tower BBS improvements | 2026-09-17 | later BBS fixes (we ported the WipeTower2 side as #51) |
| #15183, #16717 + e1d613096d, #15441, #17199, #17246 | 10-24 .. 11-06 | bounds checks, decimal points in F / flow tags, no sparse layers during a toolchange, filament statistics on non-Bambu printers, Klipper M400 stalls (several already ported to our planner: #15133, #15146, #15841) |

**Orca vs BBS 2.9.1 for the ramming itself:** same lineage. Orca's `ramming()` uses one tower-wide
ramming width (`m_nozzle_change_perimeter_width`), its own `Nozzle_Change_Start_Tag`, `M104` without the
2.9.1 `N0 C` form; it has no `handle_hotend_as_extruder`, `heater_off_nozzle_change()`,
`idle_travel_cooling()`, PVA rule, per-filament ramming width, merged slabs or lanes. So for H3C the
BBS 2.9.1 code stays the source either way.

**Does this change the plan or size?**
- **This PR stays as is** (built and gated as a self-contained step): it delivers the owner's target
  (H3C / H2 change blocks with BBS's ramming + post-extrusion cooling) and changes only nozzle-change
  printers. Its keys, `NozzleChangePlan` / `NozzleChangeResult`, append_tcr emission, H3C modes and tests are
  the same objects Orca's planner uses, so they carry over.
- **The block / category stage becomes "port Orca's Type1 planner"** (edge-port-package, `Port OrcaSlicer #N`
  commits): the tower part of #10780, then #14685 / #14800 / #15035 / #15485 tower hunks and the later
  fixes, then BBS 2.9.1 only for the gaps (H3C modes, per-filament widths, `N0 C`, `filament_prime_volume_nc`).
  Bigger than estimated in section 8 when it was BBS-sourced? No - smaller in adaptation work (Orca's config
  and GCode layer are ours), but it replaces our 2963-line planner with ~5.5k lines and must re-apply Edge's
  own tower work on top: Local-Z / mixed sub-layer start tool (#937), per-nozzle widths (P2), tower interface
  (#119 vs Orca #12266), rib walls (#109 vs Orca #15035), sparse layer combination (#15841, Orca has it),
  `filament_adhesiveness_category` key. Size: XL (several agent days, every Bambu tower changes, owner
  prints on X1C + H2D/H2C + H3C). In that package this PR's `ramming()` graft is replaced by Orca's block
  version (about 300 lines dropped), everything else stays.

