# H2C rack nozzle change: the in-extruder nozzle-change G-code

Date: 2026-09-07
Branch: `feat/h2c-rack-nozzle-change` (from `origin/feat/ultra-preferences`, `fcaed0bdc0`, which
already contains `fix/h2c-nozzle-rack`)
Upstream reference: `C:\Dev\BambuStudio` at `66e405477` (2026-08-31). All upstream line numbers are
from that commit.
Predecessor: `2026-09-07-h2c-nozzle-rack-diff.md` - this note picks up its **Rank 6, 7 and 8**, the
three findings it ranked as the cause of symptom (a) and explicitly left unported.

Gold files (copied, never modified):
`C:\Dev\SnapmakerOrca\tests\h2c_bbs_12mm_slice.gcode.3mf` (Bambu Studio 02.08.02.61) vs
`h2c_es_12mm_slice.gcode.3mf` (EdgeSlicer 2.3.6.5, pre-fix). Same project, same plate 5, same 12 mm
object, real H2C (`printer_model_id` `O1C2`, `extruder_max_nozzle_count = 1,6`).

---

## 1. The problem, restated in one line

On an H2C the **right extruder carries a rack of up to six nozzles**. The fork's grouping engine
already puts different filaments on different rack nozzles - the pre-fix gold file's own
`slice_info.config` proves it:

```xml
<filament id="3" ... group_id="0"/>     <- logical nozzle 0, extruder 1 (left)
<filament id="5" ... group_id="1"/>     <- logical nozzle 1, extruder 2 (right rack, slot 1)
<filament id="2" ... group_id="2"/>     <- logical nozzle 2, extruder 2 (right rack, slot 2)
<nozzle id="0" extruder_id="1" .../>
<nozzle id="1" extruder_id="2" .../>
<nozzle id="2" extruder_id="2" .../>
```

...but the **G-code stream never says a nozzle change is happening**. A change from filament 5 to
filament 2 is a swap of the physical nozzle *inside one extruder*; nothing in the fork's output
distinguishes it from an ordinary colour change. On hardware the first rack nozzle prints at the
right height and every later one prints lifted, because the machine never re-selects the rack and
never re-applies that nozzle's Z trim.

Upstream marks the transition with **`M632 ... M N` / `M633`**, the only rack-conditional commands
in the whole stream. Command inventory of the two gold files: `M632` **322 vs 0**, `M633` **322 vs
0**.

---

## 2. Exactly which G-code a rack change consists of upstream

### 2.1 The annotated block (gold file, toolchange #21, right-rack slot 2 -> slot 1)

Verbatim from `Metadata/plate_5.gcode` of `h2c_bbs_12mm_slice.gcode.3mf`, lines 81205-81264:

```gcode
; LAYER_HEIGHT: 0.120000
; FEATURE: Prime tower
; LINE_WIDTH: 0.500000
;--------------------
; CP TOOLCHANGE START
; toolchange #21
; material : PLA -> PLA
;--------------------
M220 B                                    (1) save the firmware feedrate override
M220 S100
; WIPE_TOWER_START
; WIPE_START
M204 S4000
G1 X157.065 Y188.045 E-.38
; WIPE_END
G1 E-.02 F1800
M204 S10000
G17
G3 Z8.76 I1.217 J0 P1  F30000             (2) travel onto the tower
G1 X167.752 Y269.306
G1 Z8.36
G1 E.4 F1800
; LAYER_HEIGHT: 0.120000
; FEATURE: Prime tower
; LINE_WIDTH: 1.000000
; NOZZLE_CHANGE_START OF2 NF4 ON2 NN1     (3) OF/NF = old/new filament, ON/NN = old/new LOGICAL NOZZLE
M632 S4 M N                               (4) skippable block: "prepare filament 4, nozzle re-select"
M400                                      (5) \
M104 T0 S180 N0 ;Wipe tower nozzle change pre cooling   (6)  > the pre-cool, inside the block
M106 S255                                 (7) /
M633                                      (8) end skippable block
; LAYER_HEIGHT: 0.200000                  (9) \
M204 S4000                                     |
G1  X201.252 Y269.306  E2.6660 F7836           | the nozzle-change ramming/purge area
...                                            | (a dedicated sub-block of upstream's wipe tower)
; LAYER_HEIGHT: 0.120000                  /
M632 S4 M N                               (10) second skippable block, wrapping the travel
M204 S10000
G1  Y273.056
G1  X168.252  F5850
...
M633                                      (11)
; NOZZLE_CHANGE_END OF2 NF4 ON2 NN1       (12)

G1 E-2 F1800                              (13) the toolchange retract, deferred to after the block
G17
G3 Z8.76 I1.217 J0 P1  F5400
; filament end gcode
;======== H2C filament_change ========    (14) the change_filament_gcode template, unchanged shape
...
M620 S4A H-1
...
T4 H-1
...
M620.6 I4 H-1 W1
M620 O22                                  (15) the firmware toolchange index
; filament start gcode
G4 S0
; CP_TOOLCHANGE_WIPE CT0 FL0
...
M104 T0 S220 N0 ;Wipe tower reheat before wipe
...
M220 R                                    (16) restore the feedrate override
```

### 2.2 Where each piece is produced upstream

| piece | upstream source |
| --- | --- |
| (1) `M220 B`, (16) `M220 R` | `WipeTowerWriter::speed_override_backup/restore`, `WipeTower.cpp:1179-1194`, gated on `gcfMarlinLegacy \|\| gcfMarlinFirmware` |
| (3)/(12) `NOZZLE_CHANGE_START/END` | `WipeTower::ramming`, `WipeTower.cpp:3411-3419` + `:3564` (tags `GCodeProcessor::ETags::NozzleChangeStart/End`, `GCodeProcessor.cpp:70-71`) |
| (4)(8)(10)(11) `M632`/`M633` | `WipeTower::ramming` lambdas `format_line_M632`/`format_line_M633`, `WipeTower.cpp:3392-3399`, emitted at `:3449`, `:3454`, `:3521`, `:3560` - **only when `extruder_change == false`** |
| (5)(6)(7) `M400` / `M104 T<pe> S<t> N0` / `M106 S255` | `WipeTowerWriter::format_line_M104`, `WipeTower.cpp:1351-1366`, called at `:3451` when `precool_target_temp.second != 0` |
| (9) the ramming area | `WipeTower::ramming` body, `WipeTower.cpp:3459-3546`, sized by `WipeTowerBlock::nozzle_change_length/nozzle_change_depth` |
| (13) the deferred retract | `GCode.cpp:841` - `retract(tcr.is_tool_change && !is_nozzle_change, ...)`, re-issued at `:898` at the end of `nozzle_change_gcode_trans` |
| the whole block's placement | `GCode.cpp:855-899`: `end_filament_gcode_str = nozzle_change_gcode_trans + end_filament_gcode_str;` - i.e. it goes into the `[filament_end_gcode]` slot of `tcr_rotated_gcode`, **before** `filament_end_gcode` and `change_filament_gcode` |
| (14) the template | `change_filament_gcode` of the H2C machine preset, expanded in `WipeTowerIntegration::append_tcr` |
| `M104 ... ;Multi extruder pre cooling/pre heating` | `GCodeProcessor::PreCoolingInjector`, `GCodeProcessor.cpp:6669-6767` - a post-processing pass over the finished file, driven by the time estimate |
| `M104 ... ;Wipe tower reheat before wipe` | `WipeTower.cpp:4074`, inside `toolchange_wipe` |
| `; CP_TOOLCHANGE_WIPE CT<n> FL<n>` | `WipeTower.cpp:4002` |

### 2.3 The M632 grammar, measured on the gold file

Every `M632` in the gold file, classified by what sits between it and its `M633`:

| form | count | contents | producer |
| --- | --- | --- | --- |
| `M632 S<f> M N` | 112 | `M400` + `M104 T<pe> S<precool> N0` + `M106 S255` | `ramming()` preamble |
| `M632 S<f> M N` | 112 | the post-ramming travel moves | `ramming()` post-ramming |
| `M632 S<f> W` | 49 | one `M104 ... ;Multi extruder pre heating` | `PreCoolingInjector`, left extruder (`extruder_max_nozzle_count == 1`) |
| `M632 S<f> N R W` | 49 | one `M104 ... ;Multi extruder pre heating` | `PreCoolingInjector`, right extruder (`extruder_max_nozzle_count == 6`) |
| | **322** | | |

and the 210 `; NOZZLE_CHANGE_START` blocks split as

```
OF2 NF4 ON2 NN1   56  right rack slot 2 -> slot 1   <- rack change,     M632/M633 present
OF4 NF2 ON1 NN2   56  right rack slot 1 -> slot 2   <- rack change,     M632/M633 present
OF2 NF1 ON2 NN0   49  right rack -> left extruder   <- extruder change, NO M632/M633
OF1 NF2 ON0 NN2   49  left extruder -> right rack   <- extruder change, NO M632/M633
```

**So the rack-conditional part of the stream is exactly: `M632 S<new filament> M N` ... `M633`,
twice, inside a `; NOZZLE_CHANGE_START/END` pair, on the 112 same-extruder / different-nozzle
transitions.** `H<nozzle>` is appended to `M632` only when
`is_support_dynamic_nozzle_map()` (a selector machine); this plate is a static map, hence no `H`,
matching the `H-1` that already appears on `T4`, `M620 S4A` and `M620.6 I4`.

The `N R W` suffix on the `PreCoolingInjector`'s `M632` is the other rack-conditional form: `N R` is
emitted exactly when `extruder_max_nozzle_count[target_extruder] > 1` (`GCodeProcessor.cpp:6680-6695`).

---

## 3. Which placeholders in the H2C templates carry it

`resources/profiles/BBL/machine/Bambu Lab H2C 0.4 nozzle template change_filament_gcode.json` is
**byte-identical to upstream's**. It never emits `M632` itself - the interlock is emitted around the
template, not by it. What the template *does* need, and what the fork currently feeds it:

| placeholder | used at (template line) | upstream source | fork today |
| --- | --- | --- | --- |
| `current_nozzle_id` | 35, 36, 38, 78, 84 | `group_result->get_nozzle_id(old_filament_id, layer)` (`GCode.cpp:962`) | **hard-coded 0** (`GCode.cpp:2950`) |
| `next_nozzle_id` | 41, 42, 44, 210 | `group_result->get_nozzle_id(new_filament_id, layer)` (`GCode.cpp:963`) | **hard-coded 0** |
| `nozzle_diameter_at_nozzle_id[...]` | 35, 36, 38, 41, 42, 44, 210 | `get_nozzle_diameters_by_nozzle_id(group_result)` - one entry **per logical nozzle** (`GCode.cpp:119-131`) | `nozzle_diameter.values` - one entry **per extruder**. On H2C that is 2 entries for 3 logical nozzles, so `[2]` is out of range |
| `nozzle_volume_types[...]` | 78, 84 | `get_nozzle_volume_types_by_nozzle_id(group_result)` -> **`ConfigOptionStrings`** ("Standard", "TPU High Flow", ...) (`GCode.cpp:134-155`) | `ConfigOptionInts` per extruder - the template compares it against the *string* `"TPU High Flow"` |
| `current_hotend` / `next_hotend` | 12, 48-73, 92, 215 | `NOZZLE_ID_FOR_GCODE(group_result, id)` = the nozzle id, or `-1` for a static map (`GCode.cpp:117`) | `-1` - **already correct**, and confirmed by the gold file's `T4 H-1` / `M620 S4A H-1` |
| `current_filament_id` / `next_filament_id` | throughout | the real filament ids | already correct in `append_tcr` |
| `long_retraction_when_ec` / `retraction_distance_when_ec` | 58-61 | per-filament config | shimmed to `false` / `0` (out of scope here, see §6) |

On this plate every rack nozzle is 0.4 / Standard, so the wrong index gives the right answer by
accident. On a mixed-diameter or mixed-flow rack - the entire point of a six-nozzle rack - it does
not, and `nozzle_diameter_at_nozzle_id[2]` is an out-of-range read.

---

## 4. Where the nozzle ids come from

The fork already carries the whole grouping layer: `src/libslic3r/MultiNozzleUtils.hpp` is
byte-identical to upstream's. Per the predecessor spec, the fork's `MultiNozzleUtils.cpp` differs
from upstream's only in one GUI-only time-recalc helper.

* `Print::get_layered_nozzle_group_result()` returns the plate's
  `MultiNozzleUtils::LayeredNozzleGroupResult` (null on classic single-nozzle machines).
* `get_nozzle_id(filament_id, layer_id)` -> the **logical nozzle** (`NozzleInfo::group_id`).
* `get_extruder_id(filament_id, layer_id)` -> the **logical extruder, 0-based**
  (`NozzleInfo::extruder_id`; `get_extruder_map(false)` is the +1'd, 1-based view, which is what
  `filament_map` and the 3MF `<nozzle extruder_id=...>` table use).
* `get_nozzle_from_id(id)` -> `NozzleInfo{diameter (string), volume_type, extruder_id, group_id}`,
  which is what the two per-nozzle-id arrays are built from.
* `is_support_dynamic_nozzle_map()` -> false for every plate this fork produces, so `H<n>` is
  omitted from `M632` and every `*_hotend` stays `-1`.
* The **physical** extruder (the `T` of `M104 T<e>`) is `physical_extruder_map[logical extruder]`,
  a `ConfigOptionInts` the fork already carries and never reads. On H2C it is `[1, 0]`: logical
  extruder 0 (left) -> physical 1, logical extruder 1 (right rack) -> physical 0. That is why every
  right-extruder `M104` in the gold file is `M104 T0`.
* `nozzle_change_sequence` / `filament_change_sequence` (fed by `GCode::record_filament_change`,
  fixed on the predecessor branch) is the same information in schedule form, and is what
  `Metadata/filament_sequence.json` carries.

So **nothing new has to be computed**. Every value the block needs is already sitting on
`Print::get_layered_nozzle_group_result()` at the moment `WipeTowerIntegration::append_tcr` runs.

---

## 5. The smallest faithful port

Five changes, all in code that exists upstream, all gated on the printer actually having a rack.

### 5.1 The gate

```cpp
// true iff some extruder carries more than one nozzle (H2C: extruder_max_nozzle_count = 1,6)
bool has_nozzle_rack(const PrintConfig&);   // any_of(extruder_max_nozzle_count > 1)
```

`extruder_max_nozzle_count` is already read this way in `ToolOrdering.cpp:1308` and `:1448`, so the
predicate is the fork's own existing idiom. **Every** behaviour change below is behind it. P1S
(`extruder_max_nozzle_count` absent/`[1]`) and H2D (`[1,1]`) therefore take byte-identical code
paths to the head.

This gate is *narrower than upstream*, deliberately. Upstream emits `; NOZZLE_CHANGE_*` and `M220
B`/`M220 R` on the H2D too. Matching that would change H2D output, and the H2D is the one dual-nozzle
machine this fork is known-good on. Rack machines are the defect; rack machines get the change.

### 5.2 Real per-nozzle context (Rank 8)

Port upstream's two helpers verbatim (`GCode.cpp:119-155`):

```cpp
static std::vector<double>      get_nozzle_diameters_by_nozzle_id(const NozzleGroupResultBase*);
static std::vector<std::string> get_nozzle_volume_types_by_nozzle_id(const NozzleGroupResultBase*);
```

and feed them, plus the real ids, wherever the shim currently writes `0`:

* global scope (`GCode.cpp:2937-2955`): `nozzle_diameter_at_nozzle_id`, `nozzle_volume_types`
  (now `ConfigOptionStrings`), `initial_nozzle_id`, `current_nozzle_id`,
  `curr_physical_extruder_id`, `most_used_physical_extruder_id`;
* per tool change in `WipeTowerIntegration::append_tcr` (upstream `GCode.cpp:960-966`):
  `current_nozzle_id` = old filament's nozzle, `next_nozzle_id` = new filament's nozzle, plus the
  two per-nozzle-id arrays in the same `DynamicConfig`;
* per tool change on the no-wipe-tower path `GCode::set_extruder` (upstream `GCode.cpp:8227-8228`);
* in the `filament_end_gcode` and `machine_end_gcode` scopes (upstream `GCode.cpp:821`, `:3375`).

Without a grouping result every one of these falls back to exactly the value the shim writes today.

### 5.3 The nozzle-change block (Rank 7)

In `WipeTowerIntegration::append_tcr`, at upstream's own insertion point
(`end_filament_gcode_str = nozzle_change_gcode + end_filament_gcode_str`), when the printer has a
rack and the tool change moves between two different logical nozzles:

```
; NOZZLE_CHANGE_START OF<old fil> NF<new fil> ON<old nozzle> NN<new nozzle>
                                              <- if !extruder_change:
M632 S<new fil>[ H<new nozzle> if dynamic map] M N
M400                                          <- only when a pre-cool temperature is configured
M104 T<physical extruder> S<precool> N0 ;Wipe tower nozzle change pre cooling
M106 S255
M633
; NOZZLE_CHANGE_END OF<old fil> NF<new fil> ON<old nozzle> NN<new nozzle>
```

**This is upstream's `ramming()` with `nozzle_change_line_count == 0`, byte for byte.** That is not a
coincidence and it is not a shortcut: `nozzle_change_line_count` is
`ceil(WipeTowerBlock::nozzle_change_length / box_width)`, and the fork's Orca-derived wipe tower has
no `WipeTowerBlock` and no nozzle-change purge area at all, so its nozzle-change length is
structurally zero. Read upstream's `ramming()` with that value substituted and everything between
`M633` and `; NOZZLE_CHANGE_END` - the ramming lines, the second `M632`/`M633` pair, the reverse
travel, the wipe path - is inside `if (nozzle_change_line_count > 0)` and disappears. What remains is
the preamble and the two markers, which is exactly the emission above.

Consequences of taking upstream's zero-length branch, stated plainly:

* the machine gets the **full nozzle-change notification** (`M632 ... M N` / `M633` and the
  pre-cool) at the right point in the stream, which is the firmware-relevant content;
* it does **not** get a dedicated nozzle-change purge stripe. The fork's wipe tower still purges at
  every tool change through its own `change_filament_gcode` flush, so the new nozzle is still
  primed; what is missing is upstream's separate low-flow ramming area, which is a purge-quality
  and ooze feature, not a correctness one.

The second `M632`/`M633` pair is deliberately **not** synthesised around the fork's existing
toolchange travel. Its job upstream is to mark *upstream's own* post-ramming travel skippable; there
is no such travel here, and wrapping a different set of moves would be invention, not a port.

### 5.4 `M220 B` / `M220 R` (Rank 7)

The fork's `WipeTowerWriter::speed_override_backup/restore` (`WipeTower.cpp:422-440`) are upstream's
functions with the bodies `#if 0`'d out under an Orca-era comment ("BBL machine don't support speed
backup"). The gold file proves BBL machines do support them. Re-enable the upstream bodies behind
the rack gate, threaded in as a `WipeTower` member set from `extruder_max_nozzle_count` in the
constructor.

### 5.5 The pre-cool temperature (Rank 6, the part that is portable)

`M104 T<pe> S<t> N0` needs a target. Upstream reads
`m_filpar[tool].precool_target_temp.second` <- `config.filament_pre_cooling_temperature_nc`
(`WipeTower.cpp:1948`). **The fork's own BBL H2C filament profiles already carry that key** -
`resources/profiles/BBL/filament/Bambu PLA Basic @BBL H2C.json` has
`"filament_pre_cooling_temperature_nc": ["180","180","180"]`, and `180` is exactly the `S180` in the
gold block above. The fork simply has no `PrintConfigDef` entry for it, so the loader drops it.

Port the key (`coInts`, nullable, default `0`, per-filament - the fork's filament-option schema),
exactly as upstream defines it at `PrintConfig.cpp:3041`.

**Cost, stated up front:** adding a `PrintConfigDef` key adds one line to the `CONFIG_BLOCK` of
*every* G-code the fork writes, P1S and H2D included
(`; filament_pre_cooling_temperature_nc = 0`). That is the one place where "byte-identical to the
head" cannot hold. The `EXECUTABLE_BLOCK` - every machine instruction - stays byte-identical, and
§7 measures both separately so the claim is checkable rather than asserted.

The alternative was to skip the key and emit `M632 S<f> M N` / `M633` with nothing between. Upstream
does emit exactly that when the pre-cool temperature is 0, so it would have been faithful too - but
it would ship an empty skippable block to the machine on the very transition being fixed, and it
would throw away data the fork's own profiles already contain. One config-block comment line is the
better trade.

### 5.6 What is deliberately NOT ported

| not ported | why |
| --- | --- |
| **The upstream WipeTower** (5301 lines vs the fork's Orca-derived 1768): `WipeTowerBlock`, `nozzle_change_length`/`nozzle_change_depth`, `m_multi_nozzle_group_result`, `get_nozzle_id`/`get_extruder_id`, the nozzle-change purge geometry | It is a replacement of the prime-tower engine, not a port into it. Every downstream consumer of the fork's tower (depth planning, sparse layers, rib walls, the Orca `WipeTower2` sibling) would have to move with it. The block's *firmware* content survives without it, per §5.3 |
| **`GCodeProcessor::PreCoolingInjector`** (`GCodeProcessor.cpp:6669-6767`) with `ExtruderPreHeating`, `FilamentUsageBlock`, `inserted_operation_lines`: the 99 `;Multi extruder pre cooling` + 98 `;Multi extruder pre heating` `M104` lines and their 98 `M632 S<f> [N R] W` / `M633` wrappers | It is a second pass over the finished file that places each `M104` by *elapsed time* - it needs the full BBS time estimator, which this fork does not have. It is thermal optimisation: it warms the idle extruder before it is needed and cools it after. Getting it wrong wastes time and oozes; not having it means the `change_filament_gcode` template's own `M620.15`/`M104` sequence does the heating at the change, which is what the fork does today on the H2D successfully |
| **`M104 ... ;Wipe tower reheat before wipe`** (210 lines, `WipeTower.cpp:4074`) | Same family: a mid-wipe reheat inside upstream's tower wipe. No `M632` wrapper, no rack conditionality |
| **`; CP_TOOLCHANGE_WIPE CT<n> FL<n>`** (the fork writes Orca's `; CP TOOLCHANGE WIPE`) | A slicer-internal preview tag (`GCodeProcessor::ETags`), read by the previewer, not by the firmware |
| **`GCodeProcessor::ETags::NozzleChangeStart/End` enum members** | The block is emitted as the literal comment text upstream's tags expand to. Adding enum members to the fork's `Reserved_Tags` would change a table the fork indexes elsewhere, for a preview feature that has no fork consumer |
| **`long_retraction_when_ec` / `retraction_distance_when_ec`** (Rank 8's tail: `M620.11 K0 ... R0` vs upstream's `K1 ... R10 F<feed>`) | Per-filament extruder-change retraction config the fork does not carry; a separate key port with the same `CONFIG_BLOCK` cost, and unrelated to nozzle selection |
| Ranks 10, 14, 15 of the predecessor spec | Unchanged scope decisions; see that document |

### 5.7 Why the smaller port still produces firmware-correct output

The machine learns "this transition is a rack change" from three places, and after this port the
fork writes all three:

1. **`Metadata/slice_info.config`** - the `<nozzle>` table plus `group_id` on every `<filament>`.
   Already correct (verified attribute-by-attribute against the gold file by the predecessor spec).
2. **`Metadata/filament_sequence.json`** - `sequence[i]` paired with `nozzle_sequence[i]`, plus
   `optimal_assignment`. Fixed on the predecessor branch.
3. **The stream itself** - `M632 S<f> M N` / `M633` around the transition, and `M620 O<n>`
   monotonic so the two agree. `M620 O<n>` was fixed on the predecessor branch; `M632`/`M633` is
   this one.

What the *unported* pieces contribute is thermal scheduling and purge geometry. Neither can move a
nozzle in Z. The rack-conditional commands - the ones whose count is 322 upstream and 0 here - are
`M632`/`M633`, and after this port the fork emits the 224 of them that come from the nozzle-change
path, in upstream's exact form, at upstream's exact insertion point.

That last paragraph is a **causal inference from the command diff**, not a hardware result. §8 is
the test that settles it.

---

## 6. Files touched

| file | change |
| --- | --- |
| `src/libslic3r/PrintConfig.{hpp,cpp}` | new `filament_pre_cooling_temperature_nc` (`coInts`, nullable, default 0), ported from upstream `PrintConfig.cpp:3041` |
| `src/libslic3r/GCode.cpp` | the two per-nozzle-id helpers; `has_nozzle_rack`; real nozzle-id / physical-extruder context in the global scope, `append_tcr`, `set_extruder` and the end-G-code scope; the nozzle-change block emission in `append_tcr` |
| `src/libslic3r/GCode.hpp` | declaration of the nozzle-change helper |
| `src/libslic3r/GCode/WipeTower.{hpp,cpp}` | rack flag; `speed_override_backup/restore` bodies restored behind it |
| `tests/libslic3r/test_h2c_rack_nozzle_change.cpp` | new: nozzle-id context and change-block emission |

---

## 7. Proof

Built from this branch, VS2022 x64 Release, deps
`C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local`, `BUILD_TESTS=ON`, in the branch's own
`build/` tree. Baseline = an install of the same tree at `origin/feat/ultra-preferences`
(`fcaed0bdc0`), candidate = an install of the same tree with this change, so the two differ only by
this commit.

### 7.1 `libslic3r_tests`

```
test cases:   624 |   622 passed | 2 failed as expected
assertions: 54588 | 54586 passed | 2 failed as expected
```

Head is 621 cases / 2 expected failures; the three added cases are
`tests/libslic3r/test_h2c_rack_nozzle_change.cpp` - the block's exact command shape against the
gold file (rack change, extruder change, pre-cool disabled, dynamic nozzle map, no physical
extruder), the per-logical-nozzle arrays on a three-nozzle H2C grouping, and the rack gate
(no `extruder_max_nozzle_count` / H2D `1,1` -> false, H2C `1,6` -> true).

### 7.2 Baseline vs candidate, same input, same isolated `--datadir`

`--datadir` is a copy of `snorca_hubtest/dd_lan`; presets are passed as `--printer-preset` /
`--process-preset` / `--filament-presets` from the installed profile set.

| case | project | printer | plate | notes |
| --- | --- | --- | --- | --- |
| `p1s` | `resources/handy_models/3DBenchy.3mf` | Bambu Lab P1S 0.4 nozzle | 1 | single filament, no wipe tower |
| `h2d` | the corrected `tests/testload_2.3mf` | Bambu Lab H2D 0.4 nozzle | 2 | 7 filaments, 545 tool changes, no rack |
| `h2c` | the same project | Bambu Lab H2C 0.4 nozzle | 2 | 6 logical nozzles (5 of them on the right rack), 545 tool changes, **prime tower off** |
| `h2ct` | the same project, `--enable-prime-tower=1` | Bambu Lab H2C 0.4 nozzle | 1 | 4 logical nozzles (3 on the rack), 147 tool changes, 145 nozzle changes |

Whole-G-code comparison, `EXECUTABLE_BLOCK` and `CONFIG_BLOCK` scored separately, dropping only the
`; generated by ... on <date>` line:

```
p1s    EXECUTABLE_BLOCK 107152 lines, byte-identical
       CONFIG_BLOCK     +1 line: "; filament_pre_cooling_temperature_nc = 0"

h2d    EXECUTABLE_BLOCK 599897 lines, byte-identical
       CONFIG_BLOCK     +1 line: "; filament_pre_cooling_temperature_nc = 180,180,..."

h2c    EXECUTABLE_BLOCK 605687 lines, byte-identical      <- prime tower off, see 7.5
       CONFIG_BLOCK     +1 line

h2ct   EXECUTABLE_BLOCK 81175 -> 81970 lines, +795 line-instances, -0
       CONFIG_BLOCK     +1 line
```

So the only change to any non-rack machine's machine instructions is **none**, and the only change
to any machine's file at all is the one new `CONFIG_BLOCK` comment line predicted in §5.5.
`Metadata/project_settings.config` gains the same key and nothing else.

The 795 added lines on `h2ct` are exactly:

```
 +145  M220 B                                                one per tool change
 +145  M220 R
 +145  ; NOZZLE_CHANGE_START OF<of> NF<nf> ON<on> NN<nn>      one per nozzle change
 +145  ; NOZZLE_CHANGE_END   OF<of> NF<nf> ON<on> NN<nn>
  +43  M632 S<f> M N
  +43  M400                                                  the 43 of those 145
  +43  M104 T0 S180 N0 ;Wipe tower nozzle change pre cooling  that are RACK changes
  +43  M106 S255
  +43  M633
```

### 7.3 Block-by-block against the gold file

`h2ct`'s nozzle table is `id0->ex1  id1->ex2  id2->ex2  id3->ex2` - one left nozzle and a
three-slot right rack. Classifying its 145 blocks by the transition in their own header:

```
OF2 NF0 ON0 NN3   39   left -> rack             extruder change   markers only
OF1 NF2 ON2 NN0   36   rack -> left             extruder change   markers only
OF3 NF2 ON1 NN0   14   rack -> left             extruder change   markers only
OF2 NF1 ON0 NN2   11   left -> rack             extruder change   markers only
OF0 NF2 ON3 NN0    1   rack -> left             extruder change   markers only
OF2 NF3 ON0 NN1    1   left -> rack             extruder change   markers only
                 ---   102 markers-only blocks
OF0 NF1 ON3 NN2   26   rack slot -> rack slot   RACK change       interlock
OF0 NF3 ON3 NN1   14   rack slot -> rack slot   RACK change       interlock
OF3 NF1 ON1 NN2    1   rack slot -> rack slot   RACK change       interlock
OF1 NF0 ON2 NN3    1   rack slot -> rack slot   RACK change       interlock
OF3 NF0 ON1 NN3    1   rack slot -> rack slot   RACK change       interlock
                 ---    43 interlocked blocks
```

102 + 43 = 145, and `M632` = `M633` = **43**, one pair per rack change and none anywhere else -
the same split Bambu Studio makes on the gold plate (112 rack changes interlocked, 98 extruder
changes not).

The shape of all 43 interlocked blocks, reduced (temperatures and coordinates elided):

```
M632 S<f> M N
M400
M104 T0 S<t> N0 ;Wipe tower nozzle change pre cooling
M106 S255
M633
```

which is the gold file's block with its ramming lines and its post-ramming travel removed - the
`nozzle_change_line_count == 0` branch of §5.3. `T0` is the right extruder
(`physical_extruder_map = [1, 0]`), matching every right-extruder `M104` in the gold file, and
`S180` is `filament_pre_cooling_temperature_nc` for `Bambu PLA Basic @BBL H2C`, matching the gold
file's `M104 T0 S180 N0` exactly.

One block in its emitted context (candidate `h2ct`, toolchange #6):

```gcode
; CP TOOLCHANGE START
; toolchange #6
; material : PLA -> PLA
;--------------------
M220 B
M220 S100
; WIPE_TOWER_START
; NOZZLE_CHANGE_START OF0 NF1 ON3 NN2
M632 S1 M N
M400
M104 T0 S180 N0 ;Wipe tower nozzle change pre cooling
M106 S255
M633
; NOZZLE_CHANGE_END OF0 NF1 ON3 NN2
; filament end gcode
G1 E-1.96732 F1800
; WIPE_START
...
```

**Ordering deviation from the gold, stated plainly.** Bambu Studio's order inside the toolchange is
`M220 B` / wipe / travel onto the tower / **block** / toolchange retract / `filament_end_gcode` /
`change_filament_gcode`; ours is `M220 B` / **block** / `filament_end_gcode` / toolchange retract +
wipe / `change_filament_gcode`. The block keeps its position *relative to `filament_end_gcode` and
`change_filament_gcode`* - which is where upstream splices `nozzle_change_gcode_trans` - but the
fork carries its toolchange retract in the `change_filament` slot rather than the
`filament_end_gcode` one, so the retract and wipe land after the block instead of before it. The
consequence is that the pre-cool `M104` starts slightly earlier in the toolchange than upstream's.
Moving it would mean moving the fork's retract, which is a wipe-tower structural change, not a port.

### 7.4 Counter consistency

```
h2ct   M620 O:   147 values, 1..147, no gaps
       filament_sequence.json plate_1: sequence length 147, optimal_assignment [0,0,1,0,0,0,0]
       nozzle changes in nozzle_sequence: 145  ==  145 NOZZLE_CHANGE blocks emitted
```

147 sequence entries against 145 nozzle changes is the correct difference: the first entry is the
initial filament (no change), and one of the 146 transitions is between two filaments that share a
nozzle - a plain colour change, which correctly gets no block.

### 7.5 What is *not* proven here

* **The prime tower must be on.** The `h2c` case (prime tower off) came out byte-identical: with no
  wipe tower every tool change goes through `GCode::set_extruder`, and upstream emits its
  nozzle-change block only from the wipe tower, so neither does this port. That machine still gets
  the right `current_nozzle_id` / `next_nozzle_id` in its `change_filament_gcode`, but no
  `M632`/`M633`. On an H2C the prime tower is on by default; a user who turns it off gets no rack
  notice. Not fixed - probably belongs in the UI as a constraint rather than in the G-code writer.
* **The layer argument.** `append_tcr` passes `gcodegen.m_layer_index` where upstream passes its own
  `m_layer_idx`. For a static nozzle map - everything this fork produces - `get_nozzle_id(f, layer)`
  ignores the layer and returns the default map, so the two are the same value. On a selector
  machine they might not be.
* **`Metadata/model_settings.config` object ordering is non-deterministic** in this fork, before and
  after this change: two runs of the *baseline* build on the same input produce files differing by a
  permutation of the `<object>` elements. Every base-vs-candidate `model_settings.config` difference
  measured above is that same permutation and nothing else. Pre-existing; worth its own fix.
* **Everything about the printer's behaviour.** See §8.

## 8. The hardware test for the user

Same 12 mm project, same plate, printer **Bambu Lab H2C 0.4 nozzle**, sliced with this branch's
build, sent over LAN with **Nozzle Offset Calibration** ticked.

1. **Both right-rack nozzles.** Arrange the plate so two filaments land on the right extruder on
   *different* rack nozzles (the grouping engine already does this for the 12 mm project - check
   before sending: the sliced 3MF's `Metadata/slice_info.config` must show two `<filament>` elements
   whose `group_id` differ and whose `<nozzle>` rows both say `extruder_id="2"`).
   Watch the first change onto the **second** rack nozzle. Expect its first layer at the same height
   as the first rack nozzle's - no gap, no squash. This is the symptom that was reported lifted.
2. **The A1/A3 case.** Load AMS slot **A1** and slot **A3**, one filament each, and a two-colour
   object that alternates between them, with the filament map putting **both** on the right
   extruder. A3 is a non-first rack nozzle; previously A1 printed correctly and A3 printed lifted.
   Expect both at the same height now.
3. **Left/right control.** Repeat 2 with A1 on the left extruder and A3 on the right - the
   extruder-change case, which was already working. Expect no regression.
4. **Stored job.** Let a job finish, then re-select it from the printer's own screen. Expect no
   freeze and both AMS units offered (that is the predecessor branch's fix; re-checking it here
   guards against a regression from the new stream content).

If step 1 or 2 still lifts, the remaining untested candidates are, in order: the missing
nozzle-change **purge stripe** (the machine may expect extrusion between `M633` and the filament
change), the missing **`PreCoolingInjector`** `M632 S<f> N R W` blocks, and
`long_retraction_when_ec`. Keep the sliced 3MF either way - it is the input for whichever of those
comes next.
