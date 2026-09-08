# H2D/H2C: preload all filaments at print start

Branch `feat/h2-preload-filaments`, from `origin/feat/ultra-preferences` (`bfc949efca`).

## 1. The complaint

On a dual-extruder Bambu machine (H2D, H2D Pro, H2C, X2D) only the *first* filament of the job is
loaded during `machine_start_gcode`. Every other filament the plate uses is loaded at the layer where
it is first needed, through the full `change_filament_gcode` macro: retract/cut, nozzle-cam toggles,
`M620.10`/`M620.11` purge parameters, the `T<f>` change, a flush proportional to `flush_length`,
`SYNC T` temperature waits, and the return travel. That is over a minute on a layer that was
otherwise going to take seconds, and it leaves a visible line where the print stalled.

The owner's proposal, and what this branch implements: *"trick it into auto-loading by including all
colors on the first layer of the prime tower."*

## 2. What was built

### 2.1 The key

`preload_all_filaments` (`coBool`, default **off**), a **process** setting
(`PrintConfig.cpp`, next to `enable_prime_tower`; `PrintConfig.hpp` `PrintConfig`;
`Preset.cpp` `print_options`; `Print.cpp` invalidates `psWipeTower` + `psSkirtBrim`).

> Loads every filament used on the plate during the first layer through the prime tower, so later
> colour changes do not pause mid-layer. Costs one purge per filament at the start of the print.
> Requires the prime tower.

**Visibility.** `Tab.cpp` puts the line in Other > Prime tower.
`ConfigManipulation::toggle_print_fff_options` shows it only when
`DynamicPrintConfig::support_different_extruders()` is true for the edited printer preset - the
"distinct extruder variants" test that already gates the fork's dual-nozzle grouping path, i.e.
H2D / H2D Pro / H2C / X2D and nothing else. The field is additionally greyed out when
`enable_prime_tower` is off.

**Refusal.** `Print::validate()` returns, before the wipe-tower checks:

```
"Preload all filaments" requires the prime tower: every filament is loaded and purged into it on the
first layer. Enable the prime tower, or turn "Preload all filaments" off.
```

with `opt_key = "enable_prime_tower"`, so the existing validation/notification path points the user at
the control to change. `ToolOrdering::apply_preload_all_filaments()` re-checks the same three
conditions (option on, prime tower on, grouping machine) and no-ops otherwise, so a config that
reaches the slicer another way cannot half-apply the feature.

### 2.2 The ToolOrdering change

`ToolOrdering::apply_preload_all_filaments()` (`src/libslic3r/GCode/ToolOrdering.cpp`), called from
the `ToolOrdering(const Print&)` constructor **after** `reorder_extruders()` /
`reorder_extruders_for_minimum_flush_volume()` and **before** `collect_extruder_statistics()`. When
it changes layer 1, the constructor re-runs `fill_wipe_tower_partitions()`.

**The ordering rule (layer 1):**

```
[ preload-only filaments ] ++ [ layer 1's own sequence, unchanged ]
```

* *preload-only* = every filament used anywhere on the plate that layer 1 does not already print.
* They are grouped by the physical extruder the dual-nozzle grouping assigned them to, so each
  extruder's loads are consecutive and an extruder/rack change is never paid twice for the same
  extruder.
* The group that layer 1's **first** filament belongs to is placed **last** among the preload groups,
  so running into the real print costs no extra extruder change.
* Within a group, filaments are ascending by id - deterministic, no dependence on map iteration.
* Layer 1's own order is **never touched**: `first_layer_print_sequence`, the soluble-first
  preference and the min-area adhesion order all survive, and the sequence therefore still **ends on
  the filament layer 1 actually finishes printing with**.

The rule itself is the free function `Slic3r::preload_first_layer_tool_order()` (declared in
`ToolOrdering.hpp`) so it can be tested without building a `Print`.

**Placement matters twice.**

* *After* the reorder passes: the filament->nozzle grouping is computed inside
  `reorder_extruders_for_minimum_flush_volume()` from the layer list. Injecting afterwards means the
  grouping sees the **unmodified** plate, so turning the option on cannot change which nozzle a
  filament lands on - only what layer 1 does with that grouping.
* *Before* `collect_extruder_statistics()`: that derives `m_first_printing_extruder`, which is the
  filament `machine_start_gcode` loads and the wipe tower's starting `current_extruder_id`. With the
  option on it becomes the first pre-change, which is exactly right.

### 2.3 Why the bookkeeping stays consistent

Nothing is special-cased. The extra entries on layer 1 are ordinary members of
`LayerTools::extruders`, so:

* `Print::_make_wipe_tower()` walks `layer_tools.extruders` and calls `WipeTower::plan_toolchange()`
  for every extruder that differs from the current one - **the normal flush volume**,
  `flush_volumes_matrix[old][new] * flush_multiplier`, less whatever
  `mark_wiping_extrusions()` can put into the object's own layer-1 infill. *Decision: use the normal
  flush volume, not a reduced "preload purge volume".* These are real colour changes on a tower that
  is printing colour; a short purge would leave the next real use of that filament contaminated,
  which is the defect the feature exists to remove. No new key was added.
* `WipeTower::plan_tower()` takes `max` over every layer's `toolchanges_depth()`, so the tower's
  depth grows to fit the layer-1 changes automatically. Re-running
  `fill_wipe_tower_partitions()` after the injection updates `wipe_tower_partitions` /
  `has_wipe_tower` for the enlarged first layer.
* `GCode::WipeTowerIntegration::append_tcr()` emits the normal `change_filament_gcode` and then, gated
  only on the tcr G-code really containing the tool-change command, does
  `m_toolchange_count++` and `record_filament_change()`. So `M620 O<n>` (H2C) / `M620 Q<n>` (H2D),
  `Metadata/filament_sequence.json`'s `sequence`/`nozzle_sequence`, and `optimal_assignment` all
  advance for the pre-changes exactly as they do for a mid-print change.
* The H2C in-extruder rack block (`M632`/`M633`, `NOZZLE_CHANGE_START/END`) is spliced in the same
  function, for any tower toolchange whose old and new logical nozzles differ. Layer-1 pre-changes
  across rack slots therefore get the identical block. **The rack part is not gated off** - see §5.4
  for the measurement.

## 3. Proofs

### 3.1 Unit test

`tests/libslic3r/test_h2_preload_filaments.cpp` (new, registered in `tests/libslic3r/CMakeLists.txt`)
pins `preload_first_layer_tool_order()` against a real `LayeredNozzleGroupResult`:

| case | layer 1 | plate uses | result |
| --- | --- | --- | --- |
| 3 filaments, 2 extruders (0 -> E0; 1,2 -> E1), layer 1 prints filament 1 | `[1]` | `{0,1,2}` | `[0, 2, 1]` - all three on layer 1, ends on 1 |
| layer 1 already prints two of three | `[2, 0]` | `{0,1,2}` | `[1, 2, 0]` - layer 1's own order intact |
| layer 1 already touches everything | `[1, 0, 2]` | `{0,1,2}` | unchanged |
| no grouping result | `[1]` | `{0,1,2}` | `[0, 2, 1]` - ascending, layer 1's filament last |
| 5 filaments (0,1 -> E0; 2,3,4 -> E1), layer 1 prints 3 | `[3]` | `{0..4}` | `[0, 1, 2, 4, 3]` - E0's loads together, E1's together, layer 1's last |

`libslic3r_tests`: see §5.

### 3.2 Bar A

Isolated `--datadir` (a copy of `snorca_hubtest/dd_lan`), presets passed on the command line,
baseline = an install of `origin/feat/ultra-preferences` `bfc949efca` from
`C:/Dev/SnapmakerOrcaPhone/build`, candidate = this branch's own build.

| case | project | printer / process | plate | filaments |
| --- | --- | --- | --- | --- |
| `p1s` | `resources/handy_models/3DBenchy.3mf` | Bambu Lab P1S 0.4 / 0.20mm Standard @BBL X1C | 1 | 1 |
| `h2d` | corrected `tests/testload_2.3mf` | Bambu Lab H2D 0.4 / 0.20mm Standard @BBL H2D, `--enable-prime-tower=1` | 1 | 4, all on layer 1 |
| `h2ct` | the same project | Bambu Lab H2C 0.4 / 0.12mm High Quality @BBL H2C, `--enable-prime-tower=1` | 1 | 4, all on layer 1, two right-rack nozzles |
| `h2dm` | the same project | Bambu Lab H2D 0.4 / 0.20mm Standard @BBL H2D, `--enable-prime-tower=1` | 2 | **6, three of them first needed at layers 8 / 48 / 61** |
| `h2cm` | the same project | Bambu Lab H2C 0.4 / 0.20mm Standard @BBL H2C, `--enable-prime-tower=1` | 2 | the same 6, on the rack |

`h2dm` / `h2cm` are the cases the feature is for; `h2d` / `h2ct` (plate 1, every filament already on
layer 1) are the no-op control, and `p1s` the single-nozzle control. The gold files
`tests/h2c_bbs_12mm_slice.gcode.3mf` / `h2c_es_12mm_slice.gcode.3mf` were used only as the reference
for the rack block's shape - they are sliced 3MFs and the loader refuses them ("the supplied file
couldn't be read because it's empty"), confirmed again on this branch.

See §5 for the measured numbers.

## 4. Known limits and open questions

### 4.1 The wasteful case

A filament used once, very late, is still loaded at the start. The total purge volume is unchanged
(one purge per filament either way) but it is all paid before layer 1 rather than spread out, and the
time estimate shows it.

### 4.2 Does the firmware keep a preloaded filament loaded?

**Open, and not answerable from slicer source.** If the firmware unloads a filament it has not used
for hundreds of layers, that filament's real first use pays a second load and the feature buys
nothing for it (it still buys the *earlier* colours). The hardware test in §4.4 is what shows it.

### 4.3 `{if toolchange_count == 2}`

The H2C `change_filament_gcode` template captures travel points on whichever change happens to be the
second one ever. With the option on that is now a pre-change on layer 1 rather than a content change.
The capture is keyed only on the count and the machine is at a known position on the tower, so this
is expected to be harmless - **not verified on hardware**.

### 4.4 The hardware test the owner should run

A three-colour plate where **colour 3 first appears at layer 40** (a small tri-colour object, or the
same object with the third colour painted only on a band near the top), H2D or H2D Pro, prime tower
on.

1. Slice with `preload_all_filaments` **off**, note where the mid-print stall lands (it should be at
   the start of layer 40's colour-3 work) and keep the print as the "before" sample.
2. Slice the same plate with the option **on**. In the preview, the first layer's tower should carry
   three purge blocks.
3. Print it and watch:
   * **All three loads happen during layer 1**, back to back, through the tower.
   * **No pause at layer 40** - colour 3 arrives as an ordinary already-loaded colour change.
   * **No visible line at layer 40** on the finished part; compare against the "before" sample at the
     same height under the same light.
4. The open question in §4.2 shows itself here: if the printer performs a *second*, full load when
   colour 3 is first really used at layer 40 - the stall and the line come back at layer 40 even
   though layer 1 already loaded it - the firmware does not hold a long-idle filament and the feature
   needs a per-filament opt-in (only preload colours used early) rather than a global one. If layer 40
   passes without a stall, the firmware holds it and the global toggle is correct.
5. Secondary check on an H2C with two right-rack nozzles: the rack-change blocks should all appear on
   layer 1 for the pre-changes and the print should not attempt a rack change at a filament's first
   real use.

## 5. Measurements

`libslic3r_tests` on this branch: **657 test cases, 655 passed, 2 failed as expected** (the base's
two known failures; no new failure). The new `[H2PreloadFilaments]` case passes on its own with 40
assertions.

### 5.1 Option off: no change to anything

Baseline `bfc949efca` vs this branch, same command line, same isolated `--datadir`, option not passed.
`; generated by ... on <date>` dropped, `EXECUTABLE_BLOCK` and `CONFIG_BLOCK` scored separately:

```
p1s    EXECUTABLE_BLOCK 107123 lines, byte-identical
       CONFIG_BLOCK     +1 line: "; preload_all_filaments = 0"
h2d    EXECUTABLE_BLOCK  79404 lines, byte-identical      (H2D 0.4, plate 1, 4 filaments)
       CONFIG_BLOCK     +1 line
h2ct   EXECUTABLE_BLOCK 146408 lines, byte-identical      (H2C 0.4 + rack, plate 1, 4 filaments)
       CONFIG_BLOCK     +1 line
h2dm   EXECUTABLE_BLOCK 633234 lines, byte-identical      (H2D 0.4, plate 2, 6 filaments)
       CONFIG_BLOCK     +1 line
h2cm   EXECUTABLE_BLOCK 642974 lines, byte-identical      (H2C 0.4 + rack, plate 2, 6 filaments)
       CONFIG_BLOCK     +1 line
```

The only difference anywhere is the one new config comment.

### 5.2 The plate the feature is for

Plate 2 of the corrected `tests/testload_2.3mf` ("MC Upper Half"): 6 filaments, 340 layers, and with
the option **off** three of the six are first needed well into the print:

```
first-use layer per filament (0-based ids):  {1: 8, 2: 1, 3: 0, 4: 1, 5: 61, 6: 48}
```

i.e. colours 1, 6 and 5 pay their load at layers 8, 48 and 61 - the exact complaint.

With the option **on**:

```
first-use layer per filament:  {1: 0, 2: 1, 3: 1, 4: 1, 5: 1, 6: 1}
filaments whose first T is before layer 2:  6 of 6
```

**Layer 1's tool sequence** (H2D, `filament_map = 2,1,1,2,1,1,2`, so 0-based f1/f2/f4/f5 -> extruder 1
and f0/f3/f6 -> extruder 2):

```
off:  T3   T4  T2                 (the plate's own three layer-1 filaments)
on:   T1   T5  T6   T3  T4  T2
      \_______/     \_________/
      pre-changes    layer 1's own sequence, untouched, still ending on T2
```

which is the ordering rule exactly: preload-only `{1, 5, 6}` -> extruder 1's `{1, 5}` first (ascending),
then extruder 2's `{6}` last because layer 1's first real filament (3) is on extruder 2, then layer 1's
own `3, 4, 2`.

### 5.3 Counter consistency

| | h2dm (H2D) off | on | h2cm (H2C rack) off | on |
| --- | --- | --- | --- | --- |
| `T<n>` lines (executable block) | 546 | **549** | 546 | **549** |
| `T<n>` lines on layer 1 | 5 | **8** | 5 | **8** |
| `M620 Q<n>` / `M620 O<n>` values | 545 | **548** | 545 | **548** |
| contiguous from 1 | 1..545 yes | **1..548 yes** | 1..545 yes | **1..548 yes** |
| `filament_sequence.json` `sequence` length | 545 | **548** | 545 | **548** |
| `nozzle_sequence` length | 545 | 548 | 545 | 548 |
| `optimal_assignment` | `[0,0,0,1,0,0,1]` | same | `[0,0,0,0,1,0,0]` | same |
| `NOZZLE_CHANGE` blocks | 0 (H2D has no rack) | 0 | 543 | **546** |
| ... of which on layer 1 | - | - | 2 | **5** |

**The tool-change arithmetic, stated plainly: +3 = exactly the three filaments layer 1 did not already
print.** 545 -> 548. The later first-use changes do **not** disappear - the printer still has to change
tool at layer 8/48/61 to print with that colour. What preload removes is the *load* at those changes:
the filament is already in the nozzle, so the change is an ordinary already-loaded colour change
instead of a full load-and-purge. (The brief anticipated "the original minus the first-use changes
that disappeared"; measurement says none disappear, the count is simply `original + (filaments not on
layer 1)`.)

`M620 O<n>` / `M620 Q<n>` is contiguous from 1 in every run and its length equals
`filament_sequence.json`'s `sequence` length exactly, on and off - the invariant the nozzle-rack spec
was written to protect. `optimal_assignment` is populated identically on and off.

### 5.4 The rack pre-changes

On the H2C plate the layer-1 pre-changes produce rack interlock blocks of the **same shape** as the
mid-print ones (numbers masked, the two blocks are line-for-line identical):

```
; NOZZLE_CHANGE_START OF1 NF5 ON5 NN2      <- layer 1, a pre-change
M632 S5 M N
M400
M104 T0 S180 N0 ;Wipe tower nozzle change pre cooling
M106 S255
M633
; NOZZLE_CHANGE_END OF1 NF5 ON5 NN2

; NOZZLE_CHANGE_START OF3 NF6 ON3 NN1      <- layer 101, an ordinary change
M632 S6 M N
M400
M104 T0 S180 N0 ;Wipe tower nozzle change pre cooling
M106 S255
M633
; NOZZLE_CHANGE_END OF3 NF6 ON3 NN1
```

**The rack part is not gated off.** It works because `append_tcr` splices the block for any tower
toolchange whose logical nozzles differ, with no layer condition. Three extra blocks on layer 1
(2 -> 5), matching the three pre-changes.

### 5.5 Time

| | off | on | delta |
| --- | --- | --- | --- |
| h2dm total estimated | 13h 13m 23s | 13h 15m 43s | **+2m 20s** (+0.29 %) |
| h2dm first layer | 4m 37s | 5m 27s | **+50s** |
| h2cm total estimated | 9h 47m 30s | 9h 48m 41s | **+1m 11s** (+0.20 %) |

The added time is small because the tower purge for these changes is small on this plate (identical
filament preset on every slot, so `flush_volumes_matrix` is near its minimum). On a plate with real
colour differences the added first-layer time is the sum of three full flushes, and the same amount is
*removed* from layers 8, 48 and 61 - the point of the feature is where the time is spent, not how
much.

### 5.6 The refusal

Option on, prime tower off, H2D, via the CLI:

```
[error] got error when validate: "Preload all filaments" requires the prime tower: every filament is
loaded and purged into it on the first layer. Enable the prime tower, or turn "Preload all filaments"
off.
```

`result.json` -> `"There are some incorrect slicing parameters in the 3mf..."`, exit 127. The existing
validation/notification path, no new mechanism.

### 5.7 What is *not* proven here

* **Hardware.** Nothing here has been on a printer. §4.4 is the test that matters.
* **A plate where layer 1 already uses every filament is a no-op**, correctly: plate 1 of the same
  project (4 filaments, all on layer 1) produces byte-identical G-code with the option on and off,
  and `apply_preload_all_filaments()` returns false. Measured, not assumed.
* **`{if toolchange_count == 2}`** - §4.3.
* **The purge volume decision** (normal flush, no new key) is a correctness argument, not a
  measurement; nobody has printed a preloaded plate to see whether a shorter purge would have been
  enough.
* **Sequential (`ByObject`) prints** are untouched: the injection lives only in the
  `ToolOrdering(const Print&)` constructor, not the per-object one.
