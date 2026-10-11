# Bambu flush keys: filament_flush_temp, filament_flush_volumetric_speed, filament_cooling_before_tower

Researched 2026-10-08 on origin/main 2df3263b01 (EdgeSlicer), against the Bambu Studio reference clone
at C:\Dev\BambuStudio (HEAD 47a13eeb1) and upstream OrcaSlicer main. Nothing here touched a printer.

## Verdict

| Key | Matters? | Printers | Is our G-code different from Bambu Studio today? |
| --- | --- | --- | --- |
| `filament_cooling_before_tower` | Yes, a little | H2D, H2D Pro, H2C, H2S, X2D (M620.15 and the SYNC compensation), P2S (M620.15), A2L (SYNC only, 6 of 178 profiles carry a value). Not X1/P1/A1: no profile carries it and their templates do not read it. | **Yes.** The change_filament_gcode line `M620.15 C{new_filament_temp - filament_cooling_before_tower[next_filament_id]}` is 10 degrees hotter than Bambu Studio's for every profile that says 10 (H2C 279/279 presets, H2D 204/249, H2D Pro 180/183, H2S 206/206, P2S 205/209, X2D 208/208). Measured below: we emit `C245`/`C220` where Bambu emits `C235`/`C210`. |
| `filament_flush_temp` | Marginal | Only the PLA Silk / Silk+ presets (flush 200 degrees; H2S 4, P2S 8, X2D 5 presets) and the P2S PETG HF family (240,0,240). Everything else says 0 = "top of the nozzle range", which is what we use. | **Yes, for those 17 presets**: `M620.10 ... T<flush temp>` and `M620.10 R...` in machine start / change_filament use the top of the range (for example 240) instead of 200. |
| `filament_flush_volumetric_speed` | Marginal | H2S 37, P2S 34, X2D 31, H2D 3, H2D Pro 4 presets, all 0.2 mm nozzle variants (3 mm3/s). 0.4/0.6/0.8 nozzle presets say 0 = "use max volumetric speed", which is what we use. | **Yes, 0.2 mm nozzle variants only**: flush feed rate and the `max(flush/2.4053*60, 200)` retract rate in the M620.11 lines come from `filament_max_volumetric_speed` instead of 3 mm3/s. For PLA, PLA-CF and PETG on a 0.2 nozzle the H2D templates hard-code F74.8347 (= 3 mm3/s) so those are already right. |

So for the owner's usual 0.4 mm H2D and H2C slices only one thing differs, the 10 degree
`M620.15 C` pre-tower temperature target. Whether Bambu's firmware does anything visible with
that number is hardware behaviour we cannot judge from here (we have no hardware verification).
Nothing is missing that would make a print fail: all three keys are defined in our PrintConfigDef,
the placeholders resolve, the slice succeeds. We just always run on the defaults.

The three keys are **not** unknown to the config definition. They are defined (PrintConfig.cpp ~2527 and
2538, "Ultra" shims, PrintConfig.hpp ~1727) and read by GCode.cpp (lines 949, 3470, 11419). What is
missing is their presence in `s_Preset_filament_options` (Preset.cpp ~1120). A preset's valid key set is
exactly that list, so `Preset::remove_invalid_keys()` drops them from every filament preset that carries
them and logs an error per file, and the G-code only ever sees the defaults (flush temp 0, flush speed 0,
cooling 0). Compare the sibling keys `filament_pre_cooling_temperature_nc` and `filament_retract_length_nc`,
which were ported correctly: defined AND listed in `s_Preset_filament_options`.

## Bambu Studio definitions

All in `src/libslic3r/PrintConfig.cpp` of the reference clone.

| Key | Line | Type | Default | Nullable | Per variant | Mode | Label | Tooltip | UI |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `filament_flush_temp` | 2547 | coInts, 0..max_temp, "degC" | {0} | yes | yes | comAdvanced | Flush temperature | temperature when flushing filament. 0 indicates the upper bound of the recommended nozzle temperature range | Tab.cpp 5004, Filament > "Multi Filament" page, group "Multi Filament" (hidden when prime_volume_mode is Fast, 5177-5190) |
| `filament_flush_temp_fast` (sibling, see below) | 2557 | coInts | {0} | yes | yes | comAdvanced | Flush temperature | Flush temperature used in fast purge mode. | Tab.cpp 5005, shown only in Fast mode |
| `filament_flush_volumetric_speed` | 2567 | coFloats, 0..200, "mm3/s" | {0} | yes | yes | comAdvanced | Flush volumetric speed | Volumetric speed when flushing filament. 0 indicates the max volumetric speed | Tab.cpp 5006, same group |
| `filament_cooling_before_tower` | 3034 | coFloats, "degC" | **{10}** | yes | yes | comDevelop | Wipe tower cooling | Temperature drop before entering filament tower | Tab.cpp 4736, Filament > Basic information group |

The per-variant list is `PrintConfig.cpp` 7693-7707 (`filament_options_with_variant`: one value per
extruder/nozzle-volume variant). All three are also in `Preset.cpp` `s_Preset_filament_options`
(1129, 1140) and `cooling_before_tower` invalidates the wipe tower step in `Print.cpp` 328. The Plater and
CalibUtils (Plater.cpp 21430/21572, CalibUtils.cpp 1467/1555) fall back from a 0 flush speed to
`filament_max_volumetric_speed` for calibration prints.

## Where Bambu Studio uses them

* **GCode.cpp, wipe-tower toolchange** (1005-1026): per filament it builds `flush_volumetric_speeds`
  (flush speed, 0 -> `filament_max_volumetric_speed`), `flush_temperatures` (flush temp, or the _fast key
  when `prime_volume_mode == Fast`, 0 -> `nozzle_temperature_range_high`) and the
  `filament_cooling_before_tower` placeholder vector (the key plus a 'filament switcher' extra, forced to 0
  on tower contact layers and the first layer). Same three vectors again for the non-tower toolchange
  (8290-8340, cooling forced to 0) and for machine_start_gcode (8048-8075, `set_placeholder_parser...`).
* **WipeTower.cpp** 1980 copies the cooling value into `m_filpar`; 4071-4096 emit
  `M104 S<print temp> ... "Wipe tower reheat before wipe"` when the value is above 0, on every printer
  that uses the BBL tower and not on the first layer.
* **Machine templates** (the {...} placeholders in `change_filament_gcode`, `machine_start_gcode`,
  `machine_end_gcode`): `flush_temperatures[...]` in `M620.10 ... T`, `flush_volumetric_speeds[...]` in
  `M620.10 F`, `M620.11 ... F`, and `filament_cooling_before_tower[next_filament_id]` in
  `M620.15 C{new_filament_temp - ...}` and in the "compensate for heating and cooling" `SYNC T` formulas
  (cooling/heating time to reach the flush temperature).

## Our bundled machine templates that read them

`resources/profiles/BBL/machine/`: the `... 0.4 nozzle template change_filament_gcode.json` and
`... template machine_start_gcode.json` files for **A2L, H2C, H2D, H2D Pro, H2S, P2S, X2D**, and the H2C
`machine_end_gcode`. `filament_cooling_before_tower` is in the change_filament templates of A2L, H2C,
H2D, H2D Pro, H2S, X2D and P2S (M620.15 C only in H2C, H2D, H2D Pro, H2S, P2S, X2D).
The Prusa Core One INDX machine (`Prusa/machine/fdm_machine_common_coreone_indx.json`) reads
`filament_flush_volumetric_speed[tool]` directly; no Prusa filament carries the key, so it always sees 0.
No Snapmaker, Creality, Anycubic or Qidi machine reads any of the three.

**What the export does with a placeholder that does not exist**: `PlaceholderParser` throws "Variable
does not exist" (PlaceholderParser.cpp 927/962/971) and the G-code export fails with that error. That
is not the situation here: the three keys exist in the config definition, `flush_temperatures` and
`flush_volumetric_speeds` are published explicitly by GCode.cpp (the machine_start and toolchange
paths), and `filament_cooling_before_tower` falls back to its defined default 0. The CLI slice of an
H2D with both filaments succeeded.

## Profiles that carry them

Counted in `resources/profiles` (this tree):

| Key | BBL | OrcaFilamentLibrary | Anycubic | Qidi | Snapmaker |
| --- | --- | --- | --- | --- | --- |
| `filament_cooling_before_tower` | 1,308 | 3 | 0 | 2 | 27 |
| `filament_flush_temp` | 238 | 132 | 106 (`["nil"]`) | 3 | 27 |
| `filament_flush_volumetric_speed` | 284 | 132 | 106 (`["nil"]`) | 3 | 27 |

Values: cooling is `10` in about 1,250 of the BBL presets and `0` in about 75 (H2C, H2D, H2S, P2S, X2D,
A2L families); flush temp is 0 except for the 17 presets above; flush speed is 0 except the 0.2 mm nozzle
variants (3). The Anycubic and Snapmaker U1 values are `nil`/`0`, i.e. inert. Note the Anycubic `nil`
spelling needs a **nullable** option type if the keys are ever loaded (test_bambu_nil_override.cpp covers
the existing nullable support).

A fourth Bambu key rides along in newer profiles: `filament_flush_temp_fast` (BBL 129 files, mostly A2L
and X2D, plus 2 in OrcaFilamentLibrary). It is not defined in our PrintConfigDef at all, so
`handle_legacy` discards it while the file is parsed (`print_config_def.has()` is false) and it never
produced a log line. It matters only for A2L/X2D with `prime_volume_mode = Fast` and is part of the same
port if one is done.

## Upstream OrcaSlicer

Upstream defines all four keys and lists them in its `s_Preset_filament_options` (Preset.cpp ~1529:
`"filament_change_length","filament_flush_volumetric_speed","filament_flush_temp","filament_flush_temp_fast",
"filament_cooling_before_tower"`). Bisecting upstream PrintConfig.cpp by date:

* `filament_flush_temp`, `filament_flush_volumetric_speed`: first present at commit cdf66984dd
  "ENH: add flush params for multi filament" (2025-09-23), part of **OrcaSlicer PR #10780 "H2D/H2S
  support"** (merged 2025-10-24). Follow-up 5e8272b0cb "support flush params in machine start GCode",
  same PR.
* `filament_cooling_before_tower` and (most likely) `filament_flush_temp_fast`: present by the **X2D
  Support PR #13388** (merged 2026-05-09).

They are not in tests/orca_pr_audit_*.md or tests/orca_port_plan.md by key name. #10780 is listed in
orca_pr_audit_2025Q4_2026Q1.md as "not applicable as-is" because EdgeSlicer carries Bambu Studio's own BBL
profile set; X2D #13388 is not listed, and the X2D machine PR #14778 is noted only as an X2D profile
gap. So nobody decided "these keys are not needed"; they were skipped as a side effect of the Bambu
profile import.

## Impact on our output, measured

Method: current main install (C:\Dev\EdgeSlicerBuilds\current, build 36d363ba55), CLI, scratch
`--datadir`, H2D 0.4 nozzle, `0.20mm Standard @BBL H2D`, filaments `Bambu PLA Basic @BBL H2D` (220 C) and
`Bambu PETG HF @BBL H2D 0.4 nozzle` (245 C), two 20 mm cubes with `--load-filament-ids 1,2`, Textured PEI.
The filament presets resolved by `--filament-presets` are written flat; the three keys are gone from them
(dropped by the preset load), which is the app's behaviour.

| | as shipped (keys dropped) | same flat presets with `filament_cooling_before_tower=10`, `filament_flush_volumetric_speed=3`, `filament_flush_temp=260` added by hand |
| --- | --- | --- |
| `; filament_cooling_before_tower` | `0` | `10,10` |
| `M620.15 C` (PETG, PLA) | `C245`, `C220` | `C235`, `C210` |
| `M620.10 A1 F... T... P...` (machine start) | `F498.898 T270 P245` (25 mm3/s, top of range) | `F59.8678 T260` (3 mm3/s, 260) |

The second column shows the G-code path already honours the keys when they reach the config; it is only
the preset loader that withholds them. The other lines Bambu varies on these keys, the
`SYNC T...` heating/cooling compensation, only run when `flush_length > 0`; in this slice the fork's tower
purges in the tower (`flush_length` is 0, `SYNC T0`), so they show no difference here. (A project with
`purge_in_prime_tower` handled by the firmware flush would.)

## Log noise (the PR that goes with this note)

Owner's start log of 2026-10-08 (2,210 `[error]` lines): 1,508 `load_vendor_configs_from_json ... contains
incorrect keys` (system profiles) + 425 `Error in a preset file` (user presets: 422 flush family, 3
`mixed_filament_definitions`) = 1,933 key-related lines, i.e. 87.5 percent. The remaining 277 are not key
problems: 256 `WebPresetDialog::LoadProfileFamily ... GetFilamentInfo Failed`, 16 `PartPlate::calc_exclude_triangles:
Unable to create exclude triangles`, 3 `Health check is not running`, 1 `file path is null for ... icon-192.png`,
1 `can not find parent for config ... (a user process preset whose parent is gone)`.

After fix/preset-key-log-noise (scratch datadir, the repo's profile tree loaded through
`PresetBundle::load_presets`, hidden `[KeyNoiseProbe]` test): **0 error lines** from the system tree, and one info
line: `Ignored 4,294 occurrences of unsupported Bambu Studio keys (1,508 presets): filament_cooling_before_tower
(1,337), filament_flush_temp (1,499), filament_flush_volumetric_speed (1,458)`. CLI slice of an X1C: 1,481 error
lines before, 0 after (log 685 KB to 47 KB); U1: 159 before, 0 after. G-code of a U1 cube, an X1C cube and a
two-filament H2D plate is identical to a build of origin/main (only the generation timestamp differs).

Note on "unknown" keys: a key that the config *definition* does not know at all (for example
`filament_flush_temp_fast`, or a typo) is already discarded silently while the file is parsed
(`PrintConfigDef::handle_legacy`: `print_config_def.has()` false). The error that remains is for keys that are
defined but sit in the wrong preset type (a process option in a filament preset, for example `layer_height`), which is
the situation `remove_invalid_keys` exists to report.

## mixed_filament_definitions

* Origin: the **Snapmaker Orca mixed filament feature**, merged into EdgeSlicer as `ac3dafe08a "Feat:mix
  filament (#375)"` (ZhangZheng, 2026-05-26), later hardened by `3f7f3069cb` (adapting upstream Orca
  #15728). It is a serialized string of custom mixed-filament rows. Upstream OrcaSlicer has no such key
  (code search: 0 hits).
* It is a **project** option on purpose: it sits in `PresetBundle::s_project_options` next to the other
  `mixed_filament_*` keys, is excluded from dirty checks (`skipped_in_dirty`, Preset.cpp), and is read from
  `project_config`.
* How it gets into process presets: `PresetBundle.cpp` ~4591-4597, `set_mixed_string()`, writes it with
  `print_cfg.set_key_value("mixed_filament_definitions", ...)` into the *edited process preset* whenever
  the filament list changes, and saving that preset serializes every key it holds. So the user process
  presets that carry it (3 in the owner's log) are our own saves, not another fork's. On the next load the
  preset's key list excludes it and it is dropped (and, until now, reported as an error).
* Should we accept it into presets? **No.** It would put project state into a preset, make presets differ
  by project, and the dirty check already ignores it. Treat it as a known, expected extra key.
  A cleaner follow-up (not done here, it changes saved files): stop writing it into the print preset in
  `set_mixed_string()`.

## Port proposal (as researched; done in feat/bambu-flush-keys, see below)

Goal: make these profile values take effect so H2D/H2C/H2S/P2S/X2D/A2L output matches Bambu Studio.
Following the precedent of `filament_pre_cooling_temperature_nc` / `filament_retract_length_nc` (commit
history shows the same fix: define + list in filament options):

1. `Preset.cpp` `s_Preset_filament_options`: add the three keys and `filament_flush_temp_fast`. (~4 lines)
2. `PrintConfig.cpp` / `.hpp`: make the defs nullable (`ConfigOptionIntsNullable`, `FloatsNullable`; the
   Anycubic `nil` values require it), take Bambu's tooltips/limits, default cooling **10**, add
   `filament_flush_temp_fast`, and add all four to the per-variant key list next to
   `filament_retract_length_nc` (PrintConfig.cpp ~8508). (~50 lines)
3. `Print.cpp`: invalidate the wipe-tower step on `filament_cooling_before_tower` (1 line).
4. `GCode.cpp`: replace the three `option<ConfigOptionInts>("...")` reads (949, 3470, 11419) with variant
   aware reads, apply the Fast-mode flush temp, publish `filament_cooling_before_tower` as a placeholder
   vector with the first-layer zeroing, in all three places. (~60 lines)
5. `Tab.cpp`: Filament > Multi Filament page entries, Fast-mode toggle, Basic information entry. (~20 lines)
6. Tests: preset load keeps the values, `M620.15 C` and flush `T/F` for an H2D slice, nullable "nil", 0.2
   nozzle. (~150 lines)

About 300 changed lines plus tests, one focused PR. It is **behaviour-changing**: about 1,900 profiles
start carrying real values, so every H2D/H2C/H2S/P2S/X2D slice changes `M620.15 C`, and 17+ presets change
flush temperature, 0.2 nozzle presets change flush speed. It needs the owner's decision and a hardware
check, and it should be listed as an accepted behaviour change. Two things to decide with it:

* Bambu's tower also emits `M104 ... "Wipe tower reheat before wipe"` when the cooling value is above 0;
  our Orca-based tower has no equivalent (see h2c-rack-nozzle-change notes). The minimal port above does
  not add it. Whether the firmware (`M620.15` + the `;VM109 S[new_filament_temp]` virtual M109 that follows
  the flush) reheats on its own, or the tower reheat is needed, has to be confirmed on a printer.
* The default flips 0 -> 10 for any preset that lacks the key (user presets copied before the port). Bambu's
  is 10; keep that, since every BBL profile states it explicitly anyway.

Not needed for the log noise, which is fixed independently (fix/preset-key-log-noise).

## The port (feat/bambu-flush-keys)

Done as proposed, with these differences:

* **Per-variant list.** Our per-variant list is `filament_flow_variant_options()` (PrintConfig.cpp), the
  Standard / High Flow columns that `BambuFlowSupport` maps Bambu's extruder-variant slots onto. All four keys
  are there, so a composed config packs them per filament and flow column like `nozzle_temperature`. They are
  **not** in `m_filament_option_keys` (~8508): that list is resized to the filament count by
  `set_num_filaments()`, which would cut a packed vector.
* **Reads.** `GCode.cpp` builds `flush_volumetric_speeds`, `flush_temperatures` and
  `filament_cooling_before_tower` in one helper (`bambu_flush_placeholders`), reading each filament's slot with
  `get_config_idx(..., ConfigFlowDomain::Filament, id)`. A nil flush slot reads as the option default, a nil
  cooling slot as 0. Fast `prime_volume_mode` reads `filament_flush_temp_fast`. The cooling comes from
  `filament_cooling_before_tower_at()` (PrintConfig), which GCode and the tower share; on a tower toolchange it
  adds Bambu's extra 10 degrees for a filament switcher feeding extruders of different types. The start G-code
  also gets the unpacked per-filament values of the four keys themselves (Prusa CORE One INDX reads
  `filament_flush_volumetric_speed[next_extruder]`).
* **Defaults.** The filament default preset nulls its nullable options (nil = use the printer value, for the
  retract overrides). These four have no printer value, so the default preset holds what Bambu Studio's base
  filament profile `BBL/filament/fdm_filament_common.json` (which every Bambu filament inherits; ours lacks the
  keys) says: flush 0, flush speed 0, **cooling 0**. The option default of `filament_cooling_before_tower` stays
  Bambu's 10; the BBL presets that cool say so explicitly. A first version held 10 here and made A2L PLA / PETG
  HF slices cool where Bambu does not.
* **Invalidation.** The flush keys only invalidate the G-code export (`steps_gcode`);
  `filament_cooling_before_tower` invalidates `psWipeTower`, as in Bambu (Print.cpp 328), because the tower writes
  the reheat from it.
* **UI.** Filament > Basic information: "Wipe tower cooling" (Develop mode, as in Bambu). Filament >
  Multimaterial > "Tool change parameters with multi extruder MM printers": the flush temperature, Fast flush
  temperature and flush volumetric speed, before the extruder-change retraction (Bambu's "Multi Filament"
  page order). Only one flush temperature line shows: the Fast one when the project's `prime_volume_mode` is
  Fast (we have no purge-mode switch; a Bambu project can set Fast).

### "Wipe tower reheat before wipe" (ported after the H2D hand-test)

The first version of the port left the reheat out. The owner's H2D test showed what that does: after
`M620.15 C210` the nozzle never came back to 220 and the rest of the print ran 10 degrees cold.

Bambu Studio: the tower copies the value into `m_filpar[idx].filament_cooling_before_tower` (WipeTower.cpp 1980).
`toolchange_wipe_new` (4071-4096, every BBL tower toolchange) sets
`should_heating = cooling > EPSILON && !solid_tool_toolchange && !is_first_layer()` and, before the first wipe
extrusion (after the line / flat ironing nub when the gap wall is on), writes

    M104 T<physical extruder> S<nozzle_temperature> N0 ;Wipe tower reheat before wipe

through `format_line_M104(target, extruder, is_heating=true, ...)`: no M400 for heating. GCode.cpp 1001-1026
publishes the cooling to change_filament_gcode only on such toolchanges (zero on the first layer and on a contact
toolchange); a toolchange that does not go through the tower publishes zeros (8318-8337).

Ours (`WipeTower::tool_change`): the same line, with the same conditions (the tower's first layer, an interface
toolchange = Bambu's contact), right after the load, i.e. after the `[change_filament_gcode]` block that carries
`M620.15 C` and before the new filament's first extrusion on the tower. Bambu's first extrusion after the load is
the wipe; ours is the tower wall, which our tower prints inside the toolchange, so the M104 goes before the wall.
The tower reports the decision in `ToolChangeResult::reheats_after_cooling`, and GCode publishes the cooling only
when it is set (and not on layer 0), so a cool-down without its reheat cannot happen.

Paths:
* **Tower toolchange** (any BBL printer with the prime tower, by layer, or by object with the tower: the by-object
  export only takes its own path when there is no tower): cool-down and reheat as above.
* **No tower / WipeTower2 / by object without the tower**: the toolchange goes through `GCode::set_extruder`, which
  publishes zeros, so `M620.15 C` is the print temperature and no reheat is due (Bambu: the same).
* **First layer, interface toolchange**: no cool-down, no reheat.
* **H2C rack**: the rack nozzle change sits in the same tower toolchange (end-filament slot); the reheat names the
  physical extruder, which holds the new nozzle.
* **A2L**: its template has no `M620.15`; the cooling only enters the `SYNC T` heat-up compensation. With the
  0 default only the A2L presets that set 10 (PETG Matte, TPU 85A 0.6/0.8) cool, and the tower reheats after them,
  as Bambu's does.

`tests/libslic3r/test_h2d_byobject_toolchange.cpp` (`bambu_flush_keys::check_reheats()`) is the rule: every `M620.15 C` below the print
temperature of the filament being loaded is followed by the reheat to that temperature before the toolchange ends
and before anything prints, and every reheat answers such a cool-down.
