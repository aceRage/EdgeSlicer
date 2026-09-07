# H2C nozzle rack: what Bambu Studio's plate 3MF carries that this fork's does not

Date: 2026-09-07
Branch: `fix/h2c-nozzle-rack` (from `origin/feat/ultra-preferences`, `499440252d`)
Upstream reference: `C:\Dev\BambuStudio` at `66e405477` (2026-08-31). All upstream line numbers are
from that commit.
Predecessors: `2026-09-06-h2c-3mf-schema.md` (the 3MF writer port), `2026-09-07-h2-send-dual-nozzle.md`
(the send payload and the nozzle-offset-calibration flag).

## The evidence

Two plate-5 exports of the **same project, same plate, same 12 mm test object**, made by the user on
2026-09-07 on a Bambu H2C (`printer_model_id` `O1C2`; left extruder = 1 nozzle, right extruder =
a rack of up to 6, `extruder_max_nozzle_count = 1,6`):

* `tests/h2c_bbs_12mm_slice.gcode.3mf` - Bambu Studio 02.08.02.61 (**gold**)
* `tests/h2c_es_12mm_slice.gcode.3mf` - EdgeSlicer 2.3.6.5, install of `499440252d`

Hardware, same day, with the nozzle-offset-calibration flag now being sent:

| machine | result |
| --- | --- |
| H2D | both nozzles at the correct Z; the stored job re-prints and offers both AMS units |
| **H2C** | the **first** right-rack nozzle prints at the correct height, the **second** right-rack nozzle is lifted; selecting the stored job on the printer's screen **freezes the UI** |

The two files use a different filament->nozzle assignment (BS put filament 3 on the left extruder,
EdgeSlicer put filament 2 there) and therefore different toolpaths and a different tool order. That
is legitimate - the grouping engine is free to choose. Everything below is about the *shape* of the
metadata and the command stream, not about which filament went where.

---

## 1. Ranked findings

Ranked by how likely each is to cause **(a)** the wrong Z on the second right-rack nozzle and
**(b)** the stored-job freeze. "Fixed" means fixed on this branch.

### Rank 1 - (a) and (b) - the toolchange counter and the filament-change sequence are polluted by sparse wipe-tower layers. **FIXED**

Hard evidence, straight out of the two G-code files:

```
BBS   M620 O1, O2, O3, ... O211          211 values, no gaps
ES    M620 O1, O2, O52, O53, ... O262    213 values, one gap: 2 -> 52
```

```
filament_sequence.json / plate_5
BBS   len(sequence) = 211,  0 consecutive duplicates
ES    len(sequence) = 262,  the first 51 entries are all filament 3 (50 phantom entries)
```

`M620 O<n>` is `M620 O{toolchange_count + 1}` from the H2C `change_filament_gcode`: the firmware's
own toolchange index. The fork skipped 49 of them, then carried a 50-off index for the rest of the
job.

Cause: `WipeTowerIntegration::append_tcr` (`src/libslic3r/GCode.cpp:502`) did

```cpp
    // BBS: increase toolchange count
    gcodegen.m_toolchange_count++;
    gcodegen.record_filament_change(new_extruder_id);
```

unconditionally, at the top of the function. But `append_tcr` also runs for a *sparse* wipe-tower
layer - the `; CP EMPTY GRID` block printed on the 50 single-filament layers at the start of this
plate. That tcr's `tcr_rotated_gcode` carries no `[change_filament_gcode]` placeholder, so the
processed `change_filament_gcode` string is built and then thrown away: no `T<n>`, no `M620 O<n>`,
no toolchange - but the counter and the sequence both moved.

Upstream does not do this. `GCode.cpp:831` keeps the same increment **commented out**, passes
`m_toolchange_count + 1` to the template (`:970`), and counts *after* the tcr G-code exists, gated
on whether it really changed tool (`:1253-1257`):

```cpp
        if (custom_gcode_changes_tool(tcr_gcode, gcodegen.writer().toolchange_prefix(), new_filament_id)) {
            gcodegen.m_toolchange_count++;
            gcodegen.m_filament_change_sequence.emplace_back(new_filament_id);
            gcodegen.m_nozzle_change_sequence.emplace_back(new_nozzle_info->group_id);
        }
```

Three separate things went wrong downstream of the one bug:

1. `M620 O<n>` desynchronised from the printer's own toolchange bookkeeping.
2. `{if toolchange_count == 2}` in the H2C `change_filament_gcode` - the block that captures the
   three travel points for the filament-change path - fired on a **phantom** change during layer 2
   instead of on the second real toolchange.
3. `Metadata/filament_sequence.json` gained 50 phantom `(filament 3, nozzle 0)` entries. That file
   is the per-plate schedule the machine reads when it re-opens a stored job; the fork told it the
   job starts with 51 consecutive uses of the same filament.

### Rank 2 - (b) - `filament_sequence.json` `optimal_assignment` is empty. **FIXED**

```json
BBS   "optimal_assignment": [0, 0, 1, 0, 1]
ES    "optimal_assignment": []
```

This is the per-filament *physical group* assignment: for each of the 5 project filaments, which
extruder group it should be loaded into. Upstream computes it in `GCode.cpp:3482-3517` with
`MultiNozzleUtils::find_optimal_physical_assignment` and Bambu Studio's own UI indexes straight
into it to build the per-nozzle filament groups (`SelectMachine.cpp:6844-6847`):

```cpp
    for (int fila_idx = 0; fila_idx < optimal_assignment.size(); fila_idx++)
        pos2group[optimal_assignment.at(fila_idx)].insert(fila_idx);
```

An empty array on a machine that has to decide, per nozzle, which AMS to offer is the exact shape
of "the stored job only offers the left nozzle's AMS" and of a screen that hangs building that list.
The fork's writer had the field and a comment saying it "has no source here".

### Rank 3 - (b) - `filament_sequence.json` contains only the sliced plate. **FIXED**

BBS writes `plate_1` .. `plate_5` (plates 1-4 with three empty arrays). The fork wrote `plate_5`
only, because its port added `|| !plate_data->is_sliced_valid` to upstream's `if (!plate_data)`.
`Metadata/model_settings.config` still lists all five plates, so a consumer that looks up
`plate_<n>` for each plate it sees finds nothing for four of them.

### Rank 4 - (b) - the G-code `HEADER_BLOCK` is missing the used-filament list. **FIXED**

```
BBS   ; total filament length [mm] : 1066.84,12371.49,1187.59
      ; total filament volume [cm^3] : 2566.05,29756.92,2856.50
      ; total filament weight [g] : 3.39,39.28,3.77
      ; filament: 2,3,5
      ; support_material_on_wipe_tower: 0
ES    (none of these five lines)
```

For a **stored** job the printer re-reads the G-code header - `; filament: 2,3,5` is the only place
in the whole G-code stream that names the filament slots the job needs. Fixed for `; filament:` and
`; support_material_on_wipe_tower:` (ported from upstream `GCode.cpp:2461-2476`). The three
`; total filament ...` lines are written by upstream's `GCodeProcessor` statistics pass
(`GCodeProcessor.cpp:760`), which this fork does not have - **not fixed**, see §4.

The fork also writes three header lines Bambu Studio does not (`; generated by Snapmaker Orca ...`,
`; estimated first layer printing time ...`, `; model label id: 813`). Those are Orca lines and are
harmless - the H2D accepts them.

### Rank 5 - (b) - `Metadata/plate_5.json` has no `first_layer_time`. **FIXED**

BBS `"first_layer_time": 221.00680541992188`; the fork's `PlateBBoxData` had no such member.
The value is already on the fork's slice result (`GCodeProcessorResult::initial_layer_time`, added
by the 2026-09-06 port) - it just never reached the plate JSON.

### Rank 6 - (a) - `M104` is emitted with no target extruder, and with no `M632`/`M633` interlock. **NOT FIXED**

```
BBS   M104 T0 S112 N0 ;Multi extruder pre cooling            (49)
      M104 T0 S200 N0 ;Multi extruder pre heating            (49)
      M104 T0 S... N0 ;Wipe tower nozzle change pre cooling  (112)
      M104 T0 S... N0 ;Wipe tower reheat before wipe         (161)
      M104 T1 ... same four kinds                            (148 total)
                                                    -> 525 M104 lines, every one with T<n> and N0
ES    M104 S<t>                                              (bare)
      M104 S<t> T<n> ; preheat T<n> time: <n>s               (Orca's own line, 211)
                                                    -> 217 M104 lines
```

`T` is the **physical extruder** (`physical_extruder_map[extruder]`, upstream
`GCodeProcessor.cpp:6697`); `N0` means "this line was generated by the slicer". Around every
*skippable* one BBS wraps an interlock:

```
M632 S1 W                          left extruder  (extruder_max_nozzle_count == 1)
M104 T1 S200 N0 ;Multi extruder pre heating
M633
```
```
M632 S2 N R W                      right extruder (extruder_max_nozzle_count == 6 -> "N R")
M104 T0 S... N0 ;...
M633
```

`M632`/`M633` counts: **BBS 322 each, EdgeSlicer 0**. The `N R` suffix is emitted exactly when
`extruder_max_nozzle_count[target_extruder] > 1` - i.e. it is the rack-specific form, and `M632` is
the only command in the whole stream that is conditioned on the rack.

Note what is *not* the difference: `T` is the extruder, and both rack nozzles sit on the same
extruder, so `M104 T0` cannot distinguish them and neither can EdgeSlicer's `M104 S<t> T<n>`. What
`M632 S<filament> ... W` adds is the *filament* the machine is about to need and the permission to
re-select the rack (`N R`) for it - that is the notice EdgeSlicer never sends, and it is sent
immediately before every temperature change that straddles a nozzle change.

Upstream generates all of this in `GCodeProcessor::PreCoolingInjector` (`GCodeProcessor.cpp:6669-6767`),
machinery this fork does not have at all (`PreCoolingInjector`, `ExtruderPreHeating`,
`FilamentUsageBlock`, `inserted_operation_lines`, `physical_extruder_map`,
`extruder_max_nozzle_count`, `nozzle_group_result`: **0 hits** in the fork's `GCodeProcessor.*`).

### Rank 7 - (a) - the nozzle-change block inside one extruder is absent entirely. **NOT FIXED**

```
BBS   ; NOZZLE_CHANGE_START OF2 NF1 ON2 NN0   (49)   right rack -> left
      ; NOZZLE_CHANGE_START OF1 NF2 ON0 NN2   (49)   left -> right rack
      ; NOZZLE_CHANGE_START OF2 NF4 ON2 NN1   (56)   right rack slot 2 -> slot 1
      ; NOZZLE_CHANGE_START OF4 NF2 ON1 NN2   (56)   right rack slot 1 -> slot 2
      + the matching _END lines                     -> 420 lines
      ; CP_TOOLCHANGE_WIPE CT0 FL0                  (210)
      M220 B / M220 R  (feedrate override save/restore around each change)  (210 each)
ES    none of the above; the wipe marker is Orca's older "; CP TOOLCHANGE WIPE" (211), M220 S only
```

Here is one whole rack-to-rack change out of the gold file (toolchange #21, right-rack nozzle 2 ->
nozzle 1, i.e. exactly the transition the user's second rack nozzle prints after):

```gcode
; CP TOOLCHANGE START
; toolchange #21
; material : PLA -> PLA
;--------------------
M220 B                                      <- save the feedrate override
M220 S100
; WIPE_TOWER_START
...
; NOZZLE_CHANGE_START OF2 NF4 ON2 NN1
M632 S4 M N                                 <- "prepare filament 4, nozzle re-select" (skippable block)
M400
M104 T0 S180 N0 ;Wipe tower nozzle change pre cooling
M106 S255
M633                                        <- end skippable block
... the nozzle-change ramming lines ...
M632 S4 M N                                 <- again, for the travel phase
... travel ...
M633
; NOZZLE_CHANGE_END OF2 NF4 ON2 NN1
```

EdgeSlicer emits, for the same transition, only the wipe-tower extrusions and then the
`change_filament_gcode` template - no `M220 B`, no `M632`/`M633`, no `M104 T0 ... N0`.

`OF/NF` are old/new filament, `ON/NN` are old/new **logical nozzle**. The 112 `ON2 NN1` / `ON1 NN2`
lines are precisely the rack changes - a swap of the physical nozzle on the right extruder without
an extruder change. Upstream emits them from `WipeTower::ramming(..., extruder_change = false)`
(`WipeTower.cpp:3388-3530`), which also emits the `M632`/`M633` pair of Rank 6 twice per change
(before the ramming lines and after them).

The `NOZZLE_CHANGE_*` and `CP_TOOLCHANGE_WIPE` tags themselves are slicer-internal (they are in
`GCodeProcessor::Reserved_Tags`, consumed by the preview), so they are not what the firmware reads -
but the `M632 ... M N` / `M633` pair inside that block is, and it only exists because that block
exists.

Porting it is a rewrite of the prime tower: the fork's `WipeTower.cpp` is Orca's (1768 lines,
no `WipeTowerBlock`, no `nozzle_change_length`/`nozzle_change_depth`, no `m_multi_nozzle_group_result`,
no `get_nozzle_id`/`get_extruder_id`, no `precool_target_temp`), upstream's is 5301 lines. See §4.

### Rank 8 - (a) - the per-nozzle placeholders are still shimmed to 0. **NOT FIXED**

`src/libslic3r/GCode.cpp:2862-2874` hard-codes `current_nozzle_id`, `next_nozzle_id`,
`initial_nozzle_id`, `curr_physical_extruder_id`, `most_used_physical_extruder_id` to `0` and the
`*_hotend` variables to `-1`.

The `-1` is **correct** and matches Bambu Studio: upstream's `NOZZLE_ID_FOR_GCODE`
(`GCode.cpp:117`) returns `-1` whenever `is_support_dynamic_nozzle_map()` is false, which it is for
a static grouping - and the gold file indeed contains `;VT2 H-1`, `T2 H-1`, `M620 S2A H-1`,
`M620.6 I2 H-1 W1`. **The tool-change command shape is already right.**

The `0` is not correct. In the H2C `change_filament_gcode` it feeds
`nozzle_diameter_at_nozzle_id[current_nozzle_id]`, `nozzle_volume_types[current_nozzle_id]`, the
`M620.10` purge parameters and the flush temperatures. On this plate every nozzle is 0.4 Standard,
so slot 0 gives the right answer by accident; on a mixed-diameter or mixed-flow rack it would not.
Also `long_retraction_when_ec` is shimmed to false, so the fork writes `M620.11 K0 I<f> B-1 R0`
where BS writes `M620.11 K1 I<f> B-1 R10 F<feed>` (212 lines).

### Rank 9 - (a)/(b) - the end sequence retracts the wrong filament. **FIXED**

```
BBS   M620.11 P1 I2 B-1 E-14 F623.623      I = the filament actually loaded at the end
      M620.11 K1 I2 B-1 R10 F623.623
ES    M620.11 P1 I0 B-1 E-14 F623.623      I = 0, the global shim
      M620.11 K0 I0 B-1 R0
```

`I[current_filament_id]` in `machine_end_gcode`. Upstream sets `current_filament_id`,
`current_extruder_id` and `current_nozzle_id` in the end-G-code config (`GCode.cpp:3374-3387`);
the fork's end block (`GCode.cpp:3312-3333`) set only `filament_extruder_id`, so
`current_filament_id` fell through to the global shim `0`. The job therefore told the AMS to pull
back slot 1 instead of the slot that is loaded.

### Rank 10 - (b) - `slice_info.config` misses `limit_filament_maps` and `<layer_filament_lists>`. **NOT FIXED**

```xml
BBS   <metadata key="limit_filament_maps" value="0 0 0 0 0"/>
      <layer_filament_lists>
        <layer_filament_list filament_list="2"     layer_ranges="0 48,180 247" />
        <layer_filament_list filament_list="2 4"   layer_ranges="146 179" />
        <layer_filament_list filament_list="1 2"   layer_ranges="49 67" />
        <layer_filament_list filament_list="1 2 4" layer_ranges="68 145" />
      </layer_filament_lists>
ES    neither
```

The fork's writer already has the `limit_filament_maps` conditional (ported 2026-09-06) but nothing
populates `PlateData::limit_filament_maps`; `layer_filaments` has no fork equivalent at all
(upstream fills it from the layered grouping result). `<layer_filament_lists>` says which filaments
are live over which layer ranges - on a rack machine that is what lets the firmware plan which
nozzles it needs mounted when. Both are candidates for the freeze; neither is ported here.

Everything else in `slice_info.config` matches attribute for attribute, including the whole
2026-09-06 port: `extruder_type`, `nozzle_volume_type`, `printer_model_id`, `nozzle_diameters`,
`pause_count`, `first_layer_time`, `support_material_on_wipe_tower`, `enable_filament_dynamic_map`,
`has_filament_switcher`, `filament_maps`, the per-`<filament>` `group_id` / `nozzle_diameter` /
`volume_type` / `used_for_object` / `used_for_support`, and the identical three-row `<nozzle>` table
(`id=0 extruder_id=1`, `id=1 extruder_id=2`, `id=2 extruder_id=2`).

Two informational gaps remain: `total_load_time`/`total_unload_time` are `0.00` in the fork (they
feed only the time estimate; upstream's AMS load/unload tables are not ported) and the fork emits a
`<warning msg="bed_temperature_too_high_than_filament" .../>` element that BS does not.

### Rank 11 - object labelling differs, but it is a project setting, not a defect

BBS `exclude_object = 1`, `label_object_enabled = false`, and writes `; OBJECT_ID: 959` (457 lines).
EdgeSlicer `exclude_object = 0`, `label_object_enabled = true`, and writes Orca's
`; start printing object, unique label id: 813` + `M624 AQAAAAAAAAA=` / `M625` (457 pairs). The
base64 payload is the same on all 457 lines and decodes to `1`, while `slice_info.config` calls the
object `identify_id="813"`. Worth a look on its own, but this is the same code path the H2D uses
successfully, and the two files disagree because the two slicers defaulted `exclude_object`
differently - not because of the rack.

### Rank 12 - the per-layer timelapse block is never emitted

BBS writes, once per layer (248x), the printer preset's `time_lapse_gcode`:
`; SKIPPABLE_START / ; SKIPTYPE: timelapse / M1002 judge_flag timelapse_record_flag / M622 J1 /
M9711 ... / M1004 S5 ... / M971 S11 P5 / M623 / ; SKIPPABLE_END`. Command counts: `M1004` 248 vs 0,
`M971` 248 vs 0, `M622`/`M623` 471 vs 225. Both files carry the identical `time_lapse_gcode` in the
CONFIG_BLOCK and both have `timelapse_type = 0`; the fork simply never expands it. The block is
`M622 J1`-guarded on the printer's own `timelapse_record_flag`, so with timelapse off the machine
skips it either way - low risk, but it is a real omission.

### Rank 13 - (b) - `Metadata/plate_5.json` object bbox is in the wrong coordinate frame. **FIXED**

```
BBS   bbox_all      [129.21, 127.57, 205.84, 289.45]
      Sub-merged body [129.21, 127.57, 200.79, 191.05]     id 990
      wipe_tower      [163.16, 248.16, 205.84, 289.45]     id 1004
ES    bbox_all      [-273.99, 250.00, 225.00, 635.05]
      Sub-merged body [-273.99, 571.57, -202.41, 635.05]   id 921   <- off the bed
      wipe_tower      [165.00, 250.00, 225.00, 270.50]     id 1004  <- plausible
```

The offset is exact and constant: EdgeSlicer minus Bambu Studio is `(-403.2, +444.0, -403.2,
+444.0)` on all four object coordinates - one plate-5 origin. `PrintObject::get_first_layer_bbox`
already returns plate-relative coordinates, but `Plater::priv::generate_first_layer_bbox` subtracted
the plate origin from it anyway; only `first_layer_wipe_tower_corners()` is in global coordinates
and needs the subtraction, which is why the wipe tower came out right and the object did not.
Upstream (`Plater.cpp:16174-16182`) does not subtract there, and this fork's own CLI path
(`Snapmaker_Orca.cpp:6360-6370`) never did either - so the GUI, which is what produces the file that
actually gets sent, was the only path with the bug. The two lines are removed; the object bbox now
matches Bambu Studio's exactly.

`slice_info.config` still said `outside="false"`, so the print was never refused - but the printer's
plate preview and any bbox-driven layout on the stored-job screen were being handed an object placed
403 mm off the bed.

### Rank 14 - project_settings.config vectorisation (581 keys BBS / 632 ES; 114 shared keys differ)

The two config schemas diverge structurally: Bambu Studio vectorises per *extruder variant*
(5 slots = 3 variants on extruder 1 + 2 on extruder 2, and 15 = 5 filaments x 3 variants for
filament options) where the fork uses a scalar or a per-filament vector. The rack-relevant entries:

| key | BBS | EdgeSlicer |
| --- | --- | --- |
| `extruder_max_nozzle_count` | `["1","6"]` | `["1","6"]` (same) |
| `printer_extruder_id` / `print_extruder_id` | `["1","1","1","2","2"]` | same |
| `physical_extruder_map` | `["1","0"]` | same |
| `master_extruder_id` | `"2"` | same |
| `filament_nozzle_map` | `["0","0","0","0","0"]` | same |
| `filament_volume_map` | `["0","0","0","0","0"]` | same |
| `filament_map_mode` | `"Auto For Flush"` | same (neither run used `fmmNozzleManual`) |
| `prime_volume_mode` | `"Default"` | same (neither used `pvmSaving`) |
| `nozzle_volume_type` / `default_nozzle_volume_type` | `["Standard","Standard"]` | same |
| **`extruder_type`** | `["Direct Drive","Direct Drive"]` | **`["Direct Drive"]`** - second extruder lost |
| **`extruder_printable_area`** | two polygons | **one 8-point string** - both extruders merged |
| **`extruder_nozzle_stats`** | `["Standard#1","Standard#6"]` | **`[]`** |
| `extruder_nozzle_stats_new` | `["Standard#1","Standard#6"]` | absent |
| **`extruder_ams_count`** | `["1#1|4#0","1#0|4#2"]` | **`[]`** |
| **`nozzle_volume`** | `["130","133","133","145","148"]` | **`"130"`** - the rack's per-slot volumes collapsed |
| `flush_volumes_matrix` | 36 entries (6x6) | 25 entries (5x5) |
| `single_extruder_multi_material` | `"1"` | `"0"` |

So the *topology* is right in both files - the fork knows the right extruder has a 6-nozzle rack -
but the per-slot data (`extruder_nozzle_stats`, `nozzle_volume`, `extruder_ams_count`) is empty or
collapsed. This is the fork's config schema, not the 3MF writer, and is **out of scope** here; it is
the same schema the H2D uses successfully. The `extruder_printable_area` flattening is the one entry
that looks like an outright bug rather than a schema choice.

### Rank 15 - remaining metadata differences, all benign or cosmetic

* `Metadata/cut_information.xml` - BS writes it unconditionally (12 objects, all in the default
  no-cut state, so informationally empty); the fork writes no such part.
* `Metadata/plate_2.json` - present in the BS file only, because plate 2 had also been sliced there.
* `Metadata/model_settings.config` - BS writes `filament_maps` only on the plates that have one
  (plate 2 `"2 1 2 1 2"`, plate 5 `"1 1 2 1 2"`) plus `filament_volume_maps="0 0 0 0 0"`; the fork
  writes the *same* global `filament_maps="2 2 1 2 2"` on all five plates and no
  `filament_volume_maps`. Project-reload fidelity, not the printer.
* `[Content_Types].xml`, `_rels/.rels`, `Metadata/_rels/model_settings.config.rels` - **byte
  identical** on both sides (md5 `dde62d91...`, `d07359db...`, `0b701d1f...`). Both point at
  `plate_5.png` / `plate_5_small.png` / `Metadata/plate_5.gcode`.
* `3D/3dmodel.model` - both are geometry-free stubs (0 objects, 0 vertices, 0 triangles, 0 build
  items) with the same 17 metadata elements and the same namespaces/attributes. Only values differ:
  the fork leaves `CreationDate` / `ModificationDate` / `DesignerUserId` empty and single-escapes
  `ProfileTitle` / `ProfileDescription` where BS double-escapes them (`Title` and `Description` are
  double-escaped on both sides). Both still point `Thumbnail_Middle`/`Thumbnail_Small` at
  `plate_1.png` on a plate-5 export.
* CONFIG_BLOCK - 169 keys only in the BS file, 220 only in the fork's; all of the fork-only ones are
  Orca settings and all of the BS-only ones are settings this fork has no implementation for. No
  H2C key present in the preset is missing from the CONFIG_BLOCK on either side.
* EXECUTABLE_BLOCK start sequence - the H2C `machine_start_gcode` expands **identically** in both
  files, line for line, including `M620 M`/`M620 N` (remap enable), the `;M1002 set_flag
  auto_cali_toolhead_offset_flag=1` line the nozzle-offset calibration un-comments, and `;VT2 H-1`.
* End sequence - identical apart from Rank 9.
* Toolpath-level differences (arc fitting: `G2`/`G3`/`G17` counts, `; COOLING_NODE`,
  `; Slow Down Start/End`, `; FEATURE: Floating vertical shell`, `; OBJECT_ID`) are expected and not
  firmware-relevant.

---

## 2. Root cause

Upstream BambuStudio models the H2C rack in four layers, and this fork carries the first two:

| layer | upstream | this fork |
| --- | --- | --- |
| the grouping engine (`MultiNozzleUtils`, `LayeredNozzleGroupResult`, `filament_nozzle_map`, `filament_volume_map`, `nozzle_change_sequence`) | yes | **yes** - `MultiNozzleUtils.hpp` is byte-identical to upstream's, `.cpp` differs only in one GUI-only time-recalc helper |
| the 3MF metadata (`slice_info.config` `<nozzle>` table + per-filament `group_id`, `filament_sequence.json`, `;VT<f> H<n>`) | yes | **yes** - ported 2026-09-06 |
| the entry order fed to that metadata (`m_toolchange_count`, `m_filament_change_sequence`, `m_nozzle_change_sequence`, `optimal_assignment`) | counted only on a real tool change | **was wrong** - counted on every wipe-tower block; `optimal_assignment` never computed |
| the G-code the firmware executes for a nozzle change within one extruder (`WipeTower::ramming`, `M632`/`M633`, `M104 T<e> S<t> N0`, `NOZZLE_CHANGE_*`, `CP_TOOLCHANGE_WIPE CT<n> FL<n>`, `M220 B`/`M220 R`) | yes | **no** - the fork's WipeTower and GCodeProcessor are Orca's |

**What the firmware needs in order to know "filament X prints from right-rack nozzle N", and to
switch nozzles within the rack:**

1. **The nozzle table and the per-filament group.** `Metadata/slice_info.config`:
   `<nozzle id="N" extruder_id="E" nozzle_diameter=".." volume_type=".."/>` for every logical nozzle,
   and `group_id="N"` on every `<filament>`. **The fork writes both, correctly** (verified attribute
   by attribute against the gold file).
2. **The schedule.** `Metadata/filament_sequence.json`: `sequence[i]` (1-based filament) paired with
   `nozzle_sequence[i]` (logical nozzle), one entry per real tool change, plus `optimal_assignment`
   (per-filament physical group). **The fork's pairing was right; its length, its content and
   `optimal_assignment` were wrong.** Fixed.
3. **The toolchange index in the stream.** `M620 O<n>`, monotonic from 1. **Was broken.** Fixed.
4. **The per-extruder temperature commands.** `M104 T<physical extruder> S<temp> N0`, wrapped in
   `M632 S<filament> [H<nozzle>] [N R] W` / `M633` when the target extruder has a rack. **Absent.**
   Not fixed - this is the `PreCoolingInjector` port.
5. **The nozzle-change block itself.** `WipeTower::ramming(extruder_change = false)`:
   `; NOZZLE_CHANGE_START`, `M632 ... M N`, the ramming lines, `M632 ... M N` again,
   `; NOZZLE_CHANGE_END`. **Absent.** Not fixed - this is the WipeTower port.
6. **`T<f> H<n>` on the tool change.** Already correct: `H` is `-1` in both files because neither
   plate uses a dynamic nozzle map, and the H2C `change_filament_gcode` template supplies the rest.

So: the metadata that names the rack was already right; **what the fork got wrong was the bookkeeping
that feeds it (Rank 1-5), and what it never had is the in-extruder nozzle-change G-code (Rank 6-7).**

For symptom (a) specifically - the second rack nozzle at the wrong Z - the ranked candidates are
Rank 6/7 (the machine is never told a nozzle change is happening, so it never re-applies that
nozzle's Z trim) with Rank 1 as a contributing factor (the desynchronised `M620 O<n>` index). This
is **not proven on hardware**; see §4.

For symptom (b) - the stored-job freeze - Rank 1-5 are all in the data the machine parses when it
opens a stored job, and Rank 2 (`optimal_assignment: []`) is the single most suspicious value in the
whole file.

---

## 3. What was ported

All five changes are ports of code that exists in `C:\Dev\BambuStudio` at `66e405477`.

| file | change | upstream |
| --- | --- | --- |
| `src/libslic3r/GCode.cpp` | `WipeTowerIntegration::append_tcr`: drop the unconditional `m_toolchange_count++` / `record_filament_change`; pass `m_toolchange_count + 1` to the template; count + record after the tcr G-code exists, gated on `custom_gcode_changes_tool` | `GCode.cpp:831`, `:970`, `:1253-1257` |
| `src/libslic3r/GCode.cpp` | `HEADER_BLOCK`: `; filament: <1-based used filaments>` and `; support_material_on_wipe_tower: <0\|1>`, BBL printers only | `GCode.cpp:2461-2476` |
| `src/libslic3r/GCode.cpp` | compute `optimal_assignment` from the plate's grouping result with `MultiNozzleUtils::find_optimal_physical_assignment` | `GCode.cpp:3482-3517` |
| `src/libslic3r/GCode/GCodeProcessor.hpp` | `GCodeProcessorResult::optimal_assignment` | `GCodeProcessor.hpp:308` |
| `src/libslic3r/Format/bbs_3mf.cpp` | `PlateData::parse_filament_info` copies `optimal_assignment`; `_add_filament_sequence_file_to_archive` no longer skips unsliced plates | `bbs_3mf.cpp:9070-9096` |
| `src/libslic3r/GCode/ThumbnailData.hpp` | `PlateBBoxData::first_layer_time` + `j["first_layer_time"]` | `ThumbnailData.hpp:83, 97` |
| `src/libslic3r/GCode.cpp` | end-G-code block: set `current_filament_id` per filament, and to the loaded filament before `machine_end_gcode` | `GCode.cpp:3370-3387` |
| `src/slic3r/GUI/Plater.cpp` | stop subtracting the plate origin from the object bbox in `generate_first_layer_bbox` | `Plater.cpp:16174-16182` (which does not subtract) |
| `src/slic3r/GUI/Plater.cpp` | fill `bboxdata.first_layer_time` from the slice result | `Plater.cpp:16157` |

Adaptations, and why:

* Upstream's `get_slice_used_filaments(false)` does not exist in this fork. Upstream fills it from
  `ToolOrdering::all_extruders()` (`Print.cpp:2298,2312`); this fork keeps its tool ordering on the
  wipe-tower data, so `print.get_tool_ordering().all_extruders()` is used when it is populated (any
  multi-filament plate) and `Print::extruders(true)` - object + support + custom-G-code +
  wipe-tower filaments, sorted unique - is the fallback for a single-filament plate, where the two
  sets coincide. On this plate it gives `2,3,5`, matching the gold file exactly.
* Upstream records the sequence entry with `new_nozzle_info->group_id` taken from the toolchange it
  just planned; the fork's `record_filament_change` looks the nozzle up from the plate's grouping
  result by filament id. For a static grouping (everything this fork produces) those are the same
  value, and the gold file confirms the pairing is a strict 1:1 function of filament on both sides.
* Upstream computes `optimal_assignment` inside the block that also handles the PA-line calibration
  special case; the fork has no `Calib_PA_Line` filament-sequence path, so only the main branch is
  ported.
* Upstream's end-G-code `current_filament_id` is left at whatever the last loop iteration set, which
  is right for it because Bambu Studio's H2C project has `single_extruder_multi_material = 1` and so
  takes the single-filament branch. This fork's H2C project has it `0` and takes the loop, so
  `current_filament_id` is re-set to the loaded filament immediately before `machine_end_gcode` -
  the same value upstream ends up with, reached explicitly.
* The `; filament:` / `; support_material_on_wipe_tower:` lines are gated on `is_bbl_printers`, so
  non-Bambu output is byte-identical to before.
* `optimal_assignment` is only computed when the plate has a grouping result, i.e. on H2C / H2D /
  X2D. Single-nozzle machines get an empty vector, exactly as before.

### What is deliberately unchanged

* **P1S and every other single-nozzle BBL machine.** `optimal_assignment` stays empty (no grouping
  result), `; filament:` and `; support_material_on_wipe_tower:` are new header lines that upstream
  writes for those machines too, and `first_layer_time` is a new plate-JSON key that upstream writes
  too. The `append_tcr` counting change only bites when a wipe tower emits a block that changes no
  tool - which on a single-filament print never happens, because there is no wipe tower.
* **H2D single-rack output.** Same code path; the counting fix removes phantom entries there too
  (upstream does the same), and the H2D's `extruder_max_nozzle_count` is 1 per extruder so none of
  the rack-specific commands would appear anyway.
* Multi-material P1S / A1 prints **do** change: `M620 O<n>` becomes contiguous and
  `filament_sequence.json` loses its phantom entries. That is upstream's behaviour, and it is the
  same defect being fixed.

---

## 4. Not fixed, and why

| gap | why not |
| --- | --- |
| `M632`/`M633` + `M104 T<e> S<t> N0` (Rank 6) | needs `GCodeProcessor::PreCoolingInjector` / `ExtruderPreHeating` / `FilamentUsageBlock` / `inserted_operation_lines` / `physical_extruder_map` / `extruder_max_nozzle_count`, none of which exist in this fork's `GCodeProcessor`. It is a self-contained upstream subsystem (~600 lines) that also rewrites the time estimate; it deserves its own branch. |
| `WipeTower::ramming` nozzle-change block, `NOZZLE_CHANGE_*`, `CP_TOOLCHANGE_WIPE CT/FL`, `M220 B`/`M220 R` (Rank 7) | the fork's `WipeTower.cpp` is Orca's (1768 lines) and upstream's is BambuStudio's (5301). There is no `WipeTowerBlock`, no per-block `nozzle_change_length`/`nozzle_change_depth`, no `m_multi_nozzle_group_result`. Porting it means replacing the prime-tower engine. |
| real `current_nozzle_id` / `next_nozzle_id` / `curr_physical_extruder_id` (Rank 8) | the "single-mapped shim" is deliberate and predates this branch; wiring it needs the grouping result threaded through every placeholder site (upstream touches ~20 call sites in `GCode.cpp`). Harmless on a uniform 0.4 Standard rack; wrong on a mixed one. |
| `; total filament length/volume/weight` header lines (Rank 4) | written by upstream's `GCodeProcessor` statistics pass (`GCodeProcessor.cpp:760`), which this fork does not have. |
| `limit_filament_maps`, `<layer_filament_lists>` (Rank 10) | `PlateData::limit_filament_maps` has no producer in this fork and `layer_filaments` has no equivalent member; both come out of upstream's layered grouping result. The writer side is already in place from the 2026-09-06 port, so this is a data-source job. |
| project_settings vectorisation, `extruder_printable_area` flattening (Rank 14) | the fork's config schema. Out of scope. |
| `cut_information.xml`, `filament_volume_maps`, per-plate `filament_maps`, `total_load_time` (Rank 15) | project-reload fidelity and time estimation, not the printer. |

**Unverified:** nothing in this document has been confirmed on the H2C. The ranking of (a) against
Rank 6/7 is inference from the command diff plus the fact that those are the only rack-conditional
commands in the stream; the ranking of (b) against Rank 2 is inference from what upstream's own UI
does with `optimal_assignment`.

---

## 5. Proof

Built from this branch, VS2022 x64 Release, deps
`C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local`, `BUILD_TESTS=ON`, in the branch's own
`build/` tree. Baseline = the same tree with the five source files stashed back to
`origin/feat/ultra-preferences` (`499440252d`), rebuilt and installed to a second scratch prefix, so
baseline and candidate differ only by this change.

### 5.1 `libslic3r_tests`

```
test cases:   621 |   619 passed | 2 failed as expected
assertions: 54559 | 54557 passed | 2 failed as expected
```

Identical to head (621 cases, 2 expected failures).

### 5.2 Baseline vs candidate G-code, same input, same isolated `--datadir`

`--datadir` is a copy of `snorca_hubtest/dd_lan`, presets given as `--printer-preset` /
`--process-preset` / `--filament-presets` from the installed profile set. Four cases:

| case | project | printer | plate | notes |
| --- | --- | --- | --- | --- |
| `h2c` | `tests/testload_2.3mf` | Bambu Lab H2C 0.4 nozzle | 2 | 7 filaments, 6 used, 903 tool changes |
| `h2d` | the same project | Bambu Lab H2D 0.4 nozzle | 2 | 545 tool changes |
| `p1s` | `resources/handy_models/3DBenchy.3mf` | Bambu Lab P1S 0.4 nozzle | 1 | single filament, no wipe tower |
| `tower1` | the same project with `--enable-prime-tower=1` | Bambu Lab H2C 0.4 nozzle | 1 | 4 objects on 4 filaments, prime tower on, 239 tool changes, 1 sparse `CP EMPTY GRID` layer |

(The project is `tests/testload_2.3mf` with five out-of-range legacy values corrected -
`raft_first_layer_expansion -1 -> 2`, `solid_infill_filament`/`sparse_infill_filament`/
`wall_filament 0 -> 1`, `tree_support_wall_count -1 -> 0` - so the CLI's config validation accepts
it. The user's own gold files could not be re-sliced: a plate `*.gcode.3mf` carries a geometry-free
`3D/3dmodel.model` on both sides, so neither Bambu Studio's nor EdgeSlicer's file is a loadable
project.)

Whole-G-code comparison, dropping only the `; generated by ... on <date>` line:

```
h2c     base 1065218 lines, cand 1065220;  5 line-instances only in cand, 3 only in base
   + ; filament: 2,3,4,5,6,7
   + ; support_material_on_wipe_tower: 0
   + M620.11 P1 I4 B-1 E-14 F623.623      - M620.11 P1 I0 B-1 E-14 F623.623
   + M620.11 K0 I4 B-1 R0                 - M620.11 K0 I0 B-1 R0
   + M620.11 P1 I4 B-1 E-14               - M620.11 P1 I0 B-1 E-14

h2d     base 600519 lines, cand 600521;  2 only in cand, 0 only in base
   + ; filament: 2,3,4,5,6,7
   + ; support_material_on_wipe_tower: 0

p1s     base 107772 lines, cand 107774;  2 only in cand, 0 only in base
   + ; filament: 1
   + ; support_material_on_wipe_tower: 0

tower1  base 145787 lines, cand 145789;  5 only in cand, 3 only in base
   + ; filament: 1,2,3,4
   + ; support_material_on_wipe_tower: 0
   + M620.11 P1 I1 ... (x3)               - M620.11 P1 I0 ... (x3)
```

So **every toolpath line, every tool-change block and the entire start sequence are byte-identical**
on all four machines. The only G-code changes are the two new HEADER_BLOCK lines (which upstream
writes for these machines too) and the end-sequence `I[current_filament_id]`, which now names the
loaded filament instead of slot 1. The H2D and P1S `machine_end_gcode` do not use that placeholder,
so their end sequences are untouched.

Metadata, base vs candidate:

```
slice_info.config      identical on all four cases
model_settings.config  identical on all four cases
plate_N.json           differs by exactly one added key: "first_layer_time": 0.0
filament_sequence.json h2c    + "plate_1" entry (empty arrays), optimal_assignment [] -> [0,0,0,0,1,0,0]
                       h2d    + "plate_1" entry,                optimal_assignment [] -> [0,0,0,1,0,0,1]
                       tower1                                   optimal_assignment [] -> [0,0,1,0,0,0,0]
                       p1s    identical (no grouping result -> still empty, as before)
M620 O<n>              h2c 903 values 1..903 no gaps, base and candidate identical
                       tower1 239 values 1..239 no gaps, base and candidate identical
                       h2d / p1s: those templates emit no M620 O
sequence length        identical base vs candidate in every case (903 / 545 / 0 / 239)
```

`plate_N.json`'s `first_layer_time` comes out `0.0` on the CLI path because only the GUI's
`generate_first_layer_bbox` fills it - and that is exactly what upstream does too (`BambuStudio.cpp`
never sets it either). The GUI path, which is what produces the file that gets sent to the printer,
fills it from the slice result.

### 5.3 What is *not* proven here

The Rank 1 counting fix could not be reproduced end-to-end on a CLI-sliceable project. It only bites
when a plate has a wipe-tower layer with **no** tool change at all (the sparse `CP EMPTY GRID`
block), and none of the four cases above has a long enough single-filament stretch for that: even
`tower1`, with the prime tower forced on and one sparse layer, produced a contiguous
`M620 O 1..239` at baseline. The evidence for the defect is the user's own gold file - `M620 O`
`1, 2, 52, 53 ... 262` for 213 emitted `change_filament_gcode` blocks and 262 `filament_sequence`
entries with 51 consecutive uses of filament 3 at the head - and the evidence that the fix cannot
lose a real tool change is that the sequence length and the `M620 O` series are identical between
baseline and candidate on all four cases. The gate is literally the string that carries the tool
change: `custom_gcode_changes_tool(tcr_gcode, ...)`.

Also unverified: everything in this document about the *printer's* behaviour. See §4.

## 6. The hardware test

Same project, same plate 5, both right-rack nozzles in use (i.e. two filaments assigned to the right
extruder, which is what the grouping engine already does for this plate).

1. Slice plate 5 with the branch build and send it to the H2C over LAN with **Nozzle Offset
   Calibration** ticked (the default since `fix/h2-send-dual-nozzle`).
2. Watch the print through the first change onto the **second** right-rack nozzle - the one that
   printed lifted before. Expect its first layer to be at the same height as the first right-rack
   nozzle's, with no visible gap or squash.
3. Let the job finish (or stop it after the second rack nozzle has printed a few layers), then on
   the printer's own screen open the stored-job list and **select this job**. Expect: the screen
   opens without freezing, and the filament/AMS mapping page offers **both** AMS units, with each
   filament shown against its own nozzle.
4. Re-print the stored job from the screen and confirm both nozzles are used and both are at the
   right Z.

If step 2 still shows a lifted second nozzle, the remaining cause is Rank 6/7 (no `M632`/`M633` and
no nozzle-change block) and that is the next port. If step 3 still freezes, the remaining candidates
are Rank 10 (`limit_filament_maps`, `<layer_filament_lists>`) and Rank 4's three `; total filament`
header lines.

Worth capturing while testing: keep the plate's sliced 3MF. If the freeze is gone, the cause was in
Rank 1-5 and which one can be narrowed by removing them one at a time; if it is not, the file is the
input for the `<layer_filament_lists>` work.


---

## Appendix A - `HEADER_BLOCK`, line by line

`=` identical, `~` same key different value, `B` Bambu Studio only, `E` EdgeSlicer only.

```
 [B] ; BambuStudio 02.08.02.61
 [E] ; generated by Snapmaker Orca 2.3.6.5 on 2026-09-07 at 12:34:50
 [~] ; model printing time: 4h 12m 35s; ...   |  5h 7m 18s; ...
 [E] ; estimated first layer printing time (normal mode) = 4m 41s
 [=] ; total layer number: 248
 [E] ; model label id: 813
 [B] ; total filament length [mm] : 1066.84,12371.49,1187.59
 [B] ; total filament volume [cm^3] : 2566.05,29756.92,2856.50
 [B] ; total filament weight [g] : 3.39,39.28,3.77
 [=] ; filament_density: 1.32,1.32,1.32,1.32,1.32
 [=] ; filament_diameter: 1.75,1.75,1.75,1.75,1.75
 [=] ; max_z_height: 29.84
 [B] ; filament: 2,3,5                        <- FIXED on this branch
 [B] ; support_material_on_wipe_tower: 0      <- FIXED on this branch
```

The three `; total filament ...` lines remain missing (upstream's `GCodeProcessor` statistics pass).
The `; BambuStudio <version>` / `; generated by ...` line is the slicer's own identity and is
deliberately different; the H2D accepts the fork's form.

## Appendix B - command inventory of the EXECUTABLE_BLOCK

Counts of every G/M command whose totals differ between the two files. Toolpath commands
(`G1`/`G2`/`G3`/`G17`, `M204`, `M106`) differ because the toolpaths differ and are not listed as
findings.

```
cmd            BBS       ES     note
M632           322        0     nozzle-change / skippable interlock      <- rack-specific
M633           322        0
M1004          248        0     timelapse block (per layer)
M971           248        0
M104           525      217     BBS always "M104 T<e> S<t> N0"
M220           632      213     BBS adds "M220 B" / "M220 R" per toolchange
M622/M623      471      225     the extra pairs are the timelapse block
M624             0      457     Orca object labels (project setting differs)
M625             0      457
T1/T2/T4    49/107/56  62/82/70 different tool order - legitimate
M620 O         211      213     BBS 1..211 contiguous; ES 1,2,52..262     <- FIXED
M620.11       1056     1066     "K1 ... F<feed>" vs "K0 ... R0" (long_retraction_when_ec shim)
```

Identical in shape on both sides: `;VT2 H-1`, `T<f> H-1`, `M620 S<f>A H-1`, `M621 S<f>A`,
`M620.6 I<f> H-1 W1`, `M620.10`, `M620.14`, `M620.15`, `M628`/`M629`, `M983.3`, `M993`, `M1015.3`,
`M1015.4`, `G387`, `G392`, the whole machine start sequence and the whole end sequence apart from
Rank 9.
