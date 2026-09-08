# Mixed nozzle sizes in one slice - Phase 1 (validation scoping) and Phase 2 (U1 proof)

Branch `feat/mixed-nozzle-p1`, from `feat/ultra-preferences` at `67a2d25eb3`.
Research: `mixed_nozzle_sizes_research.md` (2026-09-07).

## What this is about

A "mixed nozzle" job is one plate where different features print through nozzles of different
diameters - the U1's four toolheads, or an H2D/H2C's two. The slicing math for that already existed
in this fork before Phase 1: `PrintRegion::flow()` (`src/libslic3r/PrintRegion.cpp:97-100`) resolves
each `FlowRole`'s line width against **the nozzle of the filament that role is routed to**, and the
support flows in `src/libslic3r/Flow.cpp` do the same. What did not exist was validation that knew
about it: every bound - layer height, "too small line width", "too large line width" - was measured
against the whole print's smallest or largest nozzle, so a mixed job was either rejected for no
reason or accepted when it should not have been.

Phase 1 changes only which nozzle each check measures against. Phase 2 proves an actual U1
0.6 + 0.2 job end to end.

## Phase 1 - what changed

### The filament -> nozzle map (new shared helper)

`physical_extruder_for_filament(const PrintConfig&, unsigned filament_id)`
(`src/libslic3r/PrintConfig.cpp`, declared in `PrintConfig.hpp` next to `has_nozzle_rack`) maps a
1-based **filament** index to the 0-based **physical extruder** it is loaded into, via
`filament_map`. `nozzle_diameter`, `min_layer_height` and `max_layer_height` are per physical
extruder, not per filament: on a machine with more filaments than nozzles (an H2D with an AMS)
`nozzle_diameter.get_at(filament - 1)` silently fell back to `values.front()` for every filament
past the second, i.e. every such filament was sized for the *first* nozzle.

Ported from OrcaSlicer PR #13782 (LixNix, "Printing with different nozzle sizes",
`Flow.cpp::physical_extruder_for_filament`). That PR is **closed, not merged** - the research note
that it landed in 2.2 was wrong, and it was re-checked against the GitHub API for this work. Nothing
else from the PR was taken; in particular this fork already had `outer_wall_filament`, and its
`top_surface_filament` / `bottom_surface_filament` / `extruder_line_width` additions are left for
later (see "What is still missing").

Now used by `PrintRegion::flow()`, `PrintRegion::nozzle_dmr_avg()`, the four support flows in
`Flow.cpp`, `Slicing.cpp`'s layer-height helpers and `Print::validate()`.

### (a) Layer-height limits per object

Two separate defects:

1. `SlicingParameters::create_from_config` (`src/libslic3r/Slicing.cpp`) walks the object's
   `object_extruders`, which are **0-based filament indices**
   (`PrintRegion::collect_object_printing_extruders`), and passed them straight into
   `min_layer_height_from_nozzle` / `max_layer_height_from_nozzle`, which expect the **1-based**
   convention `support_filament` uses (`get_at(idx - 1)`). So filament 2 read filament 1's limits,
   and filament 1 read `get_at(size_t(-1))` -> `values.front()`. On a machine whose extruders all
   carry the same limits this was invisible; on a 0.6 + 0.2 U1 it handed the 0.2 head the 0.6 head's
   0.42 mm ceiling. Fixed by shifting the index and resolving through
   `physical_extruder_for_filament`.

2. `Print::validate()` (`src/libslic3r/Print.cpp`) compared each object's `layer_height` and the
   print's `initial_layer_print_height` against `min_nozzle_diameter` computed over
   `this->extruders()` - every extruder used **anywhere in the print**. An object printed entirely on
   the 0.6 head was rejected because some other object on the plate used the 0.2 head. It now builds
   a per-object filament set from `object->object_extruders()`, plus that object's
   `support_filament` / `support_interface_filament` where it has support (falling back to the whole
   print when either is 0, "use the current filament"), plus every filament of the print when the
   prime tower is on, since the tower shares the object's layer grid.

### (b) Line-width bounds per feature

`validate_extrusion_width` took the print's `min_nozzle_diameter` / `max_nozzle_diameter` and used
the first to resolve `%` widths for the "too small" test and the second for "too large". It now
takes the diameter of the nozzle that actually prints the feature. The caller resolves it with the
same `PrintRegion::extruder(role)` mapping `PrintRegion::flow()` uses, over an explicit
opt_key -> `FlowRole` table:

| option | role | filament key |
| --- | --- | --- |
| `inner_wall_line_width` | `frPerimeter` | `wall_filament` |
| `outer_wall_line_width` | `frExternalPerimeter` | `outer_wall_filament`, else `wall_filament` |
| `sparse_infill_line_width` | `frInfill` | `sparse_infill_filament` |
| `internal_solid_infill_line_width` | `frSolidInfill` | `solid_infill_filament` |
| `top_surface_line_width` | `frTopSolidInfill` | `solid_infill_filament` |
| `skin_infill_line_width`, `skeleton_infill_line_width` | `frInfill` | `sparse_infill_filament` |

`support_line_width` is bounded against the finer of the object's support and support-interface
nozzles. A role width left at 0 falls back to the object's `line_width` before the check, exactly as
`PrintRegion::flow()` does, so what is validated is the width that will really be extruded.

### (c) A feature may not out-width its own nozzle

New: `MAX_FEATURE_WIDTH_TO_NOZZLE_RATIO = 2.0` (`src/libslic3r/libslic3r.h`, beside the existing
whole-print `MAX_LINE_WIDTH_MULTIPLIER = 5`). A role whose resolved width exceeds 2 x the diameter of
its own nozzle is a validation error naming the feature (by its UI label) and the filament:

> Line width of Outer wall (0.620 mm) is more than 2.0 x the diameter of the nozzle that prints it
> (filament 2, nozzle 0.20 mm). Reduce the line width, or print this feature with a coarser nozzle.

Nothing checked this before. The only upper bound was 5 x the **coarsest** nozzle of the print, so on
a 0.6 + 0.2 machine a 0.2 mm head could be asked for a 3.0 mm line without complaint. 2.0 is
deliberately loose - no shipped profile comes near it, and the check is a guard against a
mis-assignment, not a quality opinion.

### (d) The messages that mention different nozzle diameters

Both messages that talked about mixed diameters changed.

- `Print::validate()`'s prime-tower block used to set a **warning**: "Different nozzle diameters and
  different filament diameters may not work well when the prime tower is enabled. It's very
  experimental, so please proceed with caution." It is now split. Differing nozzle diameters are a
  hard error - "The prime tower does not support mixed nozzle diameters: it is generated at a single
  line width for every filament. Turn the prime tower off, or use filaments that all print through
  nozzles of the same diameter." - because `WipeTower::set_extruder()`
  (`src/libslic3r/GCode/WipeTower.cpp:711`) keeps one `m_perimeter_width` scalar and overwrites it
  for every registered filament. Differing *filament* diameters keep the old warning, reworded and
  re-keyed to `filament_diameter`, and the write is now guarded against a null `warning` pointer.
  Measured on the baseline build: a U1 0.6 + 0.2 job with the tower on prints the tower at
  `;WIDTH:0.75` (= 0.6 x `Width_To_Nozzle_Ratio`) for **both** tools, i.e. it asks the 0.2 mm nozzle
  for a 3.75 x line, 501 tower blocks' worth. That is what the refusal prevents.
- The plater's nozzle-sync dialog (`src/slic3r/GUI/Plater.cpp`) said "Note: Inconsistent nozzle
  diameters. Current version does not support mixed diameter printing. Please select one nozzle for
  this print." Mixed diameters are no longer a dead end, so the picker stays but reads as a choice of
  base preset: "This printer reports different nozzle diameters on its tool heads. Pick the base
  nozzle size for the printer preset; you can then give each head its own diameter under Printer
  settings -> Extruder N, and route each feature to a head under Filaments for Features. Note that
  the prime tower cannot be used with mixed nozzle diameters yet."

### One CLI defect fixed along the way

`NamedPresets::find_user` (`src/Snapmaker_Orca.cpp`) read a user preset's parent from
`key_values[BBL_JSON_KEY_INHERITS]`, but the `ConfigBase::load_from_json` overload it calls passes
`load_inherits_to_config = true`, which puts `inherits` in the **config** and never in `key_values`
(`src/libslic3r/Config.cpp:962`). So `--printer-preset` / `--process-preset` / `--filament-presets`
naming a *user* preset flattened only the handful of keys that preset overrode and sliced the rest
from option-registry defaults - a U1 preset came out with `gcode_flavor = marlin`. System presets
were never affected. Found because Phase 2's proof needs a user printer preset.

## Phase 2 - the U1 proof

Gate: `C:/Users/acesa/AppData/Local/Temp/snorca_hubtest/gate_mixnozzle.sh`, with
`mixnozzle_presets.py` (writes the user presets), `mixnozzle_flatten.py` (flattens them so the
baseline build, which lacks the CLI fix above, reads exactly the same config) and
`mixnozzle_measure.py` (per-feature width from `;WIDTH:` and from E per mm). Isolated `--datadir`
copied from `dd_lan`. Baseline = a full build of `67a2d25eb3` in a second worktree,
candidate = this branch; both installed to scratch prefixes.

Machine preset `U1 mixed 0.6+0.2`, inheriting `Snapmaker U1 (0.6 nozzle)`:
`nozzle_diameter = [0.6, 0.2, 0.6, 0.6]`, `min_layer_height = [0.12, 0.04, 0.12, 0.12]`,
`max_layer_height = [0.42, 0.14, 0.42, 0.42]`.

Process preset, layer height 0.12: `wall_filament = 1` and `sparse_infill_filament = 1` on the 0.6,
`outer_wall_filament = 2` and `solid_infill_filament = 2` on the 0.2; `enable_prime_tower = 0`.
The fork's U1 process presets state every line width in **absolute mm** (0.62 for a 0.6 nozzle), not
as a % of the nozzle, so the widths are set per role: 0.62 for the 0.6 head's features, 0.24 for the
0.2 head's.

### Measured, OrcaCube on a 30 mm plate, 250 layers

| feature | filament | nozzle | `;WIDTH:` | E per mm (p90) | width from E |
| --- | --- | --- | --- | --- | --- |
| Inner wall | 1 (T0) | 0.6 | 0.620 | 0.02790 | 0.585 |
| Sparse infill | 1 (T0) | 0.6 | 0.620 | 0.02877 | 0.602 |
| Outer wall | 2 (T1) | 0.2 | 0.240 | 0.01028 | 0.232 |
| Top surface | 2 (T1) | 0.2 | 0.240-0.243 | - | - |

Two individual straight segments, hand-checked against
`mm3_per_mm = h (w - h (1 - pi/4))`, `E = mm3_per_mm / area(1.75) x flow 0.98`:

- outer wall, `G1 X121.42 Y153.847 E.00838` over 0.800 mm -> 0.010475 E/mm; predicted for a
  0.24 x 0.12 line: **0.010475**.
- inner wall, `G1 X130.498 Y124.678 E.01723` over 0.5936 mm -> 0.029027 E/mm; predicted for a
  0.62 x 0.12 line: **0.029054**.

The aggregate p90 runs ~5 % under those because the lower tail of every loop is the seam gap and
ramp-in, and the arc segments are excluded from the statistic; the ratio between the two features
(0.01028 / 0.02790 = 0.368) matches the geometric prediction (0.2142 / 0.5942 = 0.360) to 2 %.

### Gate results (rc = 0)

| # | proof | result |
| --- | --- | --- |
| 1 | U1 mixed 0.6 + 0.2, tower off, slices; each feature sized by its own nozzle | PASS |
| 2 | that same job, baseline vs candidate | byte-identical, 156 197 lines - Phase 1 changes validation, not geometry |
| 3 | prime tower on | baseline slices a 0.75 mm single-width tower; candidate refuses with the mixed-diameter message |
| 4 | outer wall 0.62 mm routed to the 0.2 head | baseline slices it silently; candidate refuses, naming feature and filament |
| 5 | layer height 0.25 with every feature on the 0.2 head | refused ("Layer height cannot exceed nozzle diameter") |
| 6 | layer height 0.25 with every feature on the 0.6 head | accepted |
| 7 | P1S 0.4, single nozzle, baseline vs candidate | byte-identical, 82 519 lines |
| 8 | H2D 0.4 + 0.4, baseline vs candidate | byte-identical, 85 776 lines |

`libslic3r_tests`: 675 cases, 673 passed, 2 failed as expected (the known base failures);
79 149 assertions. `tests/libslic3r/test_mixed_nozzle.cpp` adds 7 cases / 26 assertions covering the
filament -> nozzle map (toolchanger and AMS shapes), (a) as both `SlicingParameters` envelopes and a
two-object `Print::validate()` that the old scoping rejected, (b), (c) in both directions, and (d)
in both directions.

## What is still missing

### Phase 3 - a prime tower with mixed widths

`WipeTower` keeps a single `m_perimeter_width` (`WipeTower.cpp:711`, comment: "all extruders are now
assumed to have the same diameter"), set from whichever filament registered last in
`set_extruder()`. `m_filpar[idx].nozzle_diameter` is already per filament index and its comment
already anticipates this ("to be used in future with (non-single) multiextruder MM"). Phase 3 is:
make `m_perimeter_width` per tool and change every reader (box sizing, purge line counts, ramming
width, brim) to use the active tool's value. That is real geometry work with no reference
implementation to port - upstream BambuStudio has the same single scalar, and OrcaSlicer's open H2D
PR #14125 has the identical bug reported against it (a tower printing 0.22 mm instead of 0.42 mm).
Until it lands, the Phase 1 refusal is the correct behaviour. Acceptance: an H2D or U1 job mixing
diameters with the tower on, where each toolchange block's tower extrusions measure that tool's
`nozzle_diameter x Width_To_Nozzle_Ratio`.

### Phase 4 - H2C rack diameters into Flow

`MultiNozzleUtils::NozzleInfo::diameter` is a real per-rack-slot diameter, but it only feeds G-code
template placeholders (`nozzle_diameter_at_nozzle_id`) and purge/toolchange time estimation.
`Flow` and `PrintRegion::flow()` see only `PrintConfig`, so a filament routed to a 0.2 mm rack slot
on an extruder declared 0.6 is still sliced as 0.6. Phase 4 threads the per-layer
`LayeredNozzleGroupResult` (already computed in `ToolOrdering`/`Print`) down to flow computation, so
`physical_extruder_for_filament` gains a rack-aware sibling that answers "which physical nozzle, with
which diameter, prints this filament on this layer". This is the largest piece: it introduces a
dependency from `PrintRegion`/`Flow` on print-level state they do not have today, and the answer can
change per layer, which the current `Flow` cache assumes it cannot. Also needed there:
`FilamentGroup.cpp`'s auto-grouping has no `nozzle_diameter` reference at all, so nothing stops it
routing a fine-detail filament onto a coarse rack slot. Acceptance: two filaments on the same logical
H2C extruder pinned to rack slots of different declared diameters produce different extrusion widths,
and a region whose width exceeds its resolved slot's bound is refused by the same (c) check.

### Smaller, worth doing before Phase 3

- **`top_surface_filament` / `bottom_surface_filament`** (OrcaSlicer PR #13782). This fork routes the
  top surface through `solid_infill_filament`, so "top surface on the fine head" necessarily drags
  internal solid infill onto it too - which is why the Phase 2 process preset also sets
  `internal_solid_infill_line_width = 0.24`. Splitting the keys makes the intended assignment
  expressible.
- **`extruder_line_width`**, a per-extruder absolute-mm override (same PR), for when a 0.2 mm head's
  right width is not a clean % of a 0.6-based profile.
- **Preset compatibility.** A mixed printer preset matches no shipped filament preset's
  `compatible_printers`, because those are keyed to a single `printer_variant` string. The Phase 2
  proof sidesteps this by loading the 0.6 filament preset for both heads (the diameter comes from the
  printer preset, not the filament). A shipped mixed-nozzle workflow needs either relaxed
  compatibility for the mismatched head or a preset matrix.
- **The plater dialog** now explains that each head can carry its own diameter, but it still does not
  write the device's reported per-head diameters into the printer preset. That is the natural next
  GUI step and was left out of Phase 1 as untestable headlessly.

## Owner's hardware test

U1 with a 0.6 mm head in slot 1 and a 0.2 mm head in slot 2.

1. Printer settings -> Extruder 1: nozzle diameter 0.6, min/max layer height 0.12 / 0.42.
   Extruder 2: nozzle diameter 0.2, min/max layer height 0.04 / 0.14. Save as a user preset.
2. Process: layer height 0.12. Filaments for Features - inner walls and sparse infill on filament 1,
   outer wall and solid infill on filament 2. Line widths: 0.62 for inner wall / sparse infill /
   first layer, 0.24 for outer wall / top surface / internal solid infill.
3. **Turn the prime tower off.** With it on the slice is refused; that is expected until Phase 3.
4. Load the same PLA in both heads (different colours make the assignment visible on the part).
5. Slice a 30 mm calibration cube and print it.

Expect: fine outer walls over a coarse interior, a visibly finer top surface, no width or layer
height errors, and a toolchange on every layer. Watch for the first layer - it uses
`initial_layer_line_width`, which the Phase 2 preset leaves at 0.62 on the 0.6 head, so the first
layer's outer wall is coarse by design. If the print time is much worse than a single-nozzle 0.2
print, the toolchange count is the reason, not the widths.
