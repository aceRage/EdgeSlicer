# Bambu Studio config compatibility

Cross-compatibility with Bambu Studio presets is an explicit goal of this project. This document
records where the two config schemas disagree, what the loader does about it, and what is
deliberately left alone. Keep it updated as upstream drifts.

Reference clone used for the comparison: `C:/Dev/BambuStudio`.
Generated from `this->add("<key>", <coType>)` declarations in `src/libslic3r/PrintConfig.cpp` on
both sides, plus the loop-generated `machine_max_*` and `add_nullable` families.

## The problem

Bambu Studio stores many print settings as **per-extruder nullable vectors** and writes the
literal string `"nil"` into the slots of extruders the setting does not apply to:

```json
"top_surface_acceleration": ["100", "nil", "100", "nil", "nil"]
```

This fork types a few of the same options as **scalars**, and the rest as **plain,
non-nullable vectors** (since the High-Flow work, c3c82dbf64, that includes most of the speed and
acceleration family). Either way `ConfigOption*::deserialize()` throws

```
Deserializing nil into a non-nullable object
```

(`src/libslic3r/Config.hpp`, the `deserialize()` overloads around lines 857 and 1181), and
`PresetCollection::load_presets` (`src/libslic3r/Preset.cpp` ~1334-1344, with the same pattern in
the catch handlers ~1432-1452) answers a parse failure by **deleting the `.json` and `.info`**.
On 2026-09-21 that destroyed 208 of the owner's presets.

### The cardinal rule

**A `nil` slot means "not applicable to this extruder". It does not mean zero.**
Substituting `0` for a speed or an acceleration is what emits `G1 F0` and stalls a printer - the
same class of bug as the internal_bridge_speed outage this week. No translation path may invent a
zero. (A zero Bambu genuinely wrote is preserved as-is; several options use `0` as a documented
"unset / use default" sentinel.)

## Measured impact on the owner's data

Scan of 275 presets copied from `%APPDATA%/BambuStudio/user/<id>/` (2026-09-21, before the
High-Flow retype; the totals still hold, but most of what was then a scalar collapse is now a
vector backfill - see table A):

| measure | count |
|---|---|
| presets carrying at least one `nil` array | 211 |
| presets that needed translation to load | 207 |
| presets where a **lossy** choice had to be made | 19 |
| total `nil`-bearing arrays | 1400 |
| &nbsp;&nbsp;non-nil slots all identical (clean collapse) | 1346 |
| &nbsp;&nbsp;non-nil slots genuinely differ (lossy) | 50 |
| &nbsp;&nbsp;every slot `nil` | 4 |
| distinct options implicated | 50 |
| of those, options that actually reach `deserialize()` and throw | 35 |
| options dropped earlier as unknown keys (no fix needed) | 15 |

The 15 dropped options (`vertical_shell_speed`, `overhang_totally_speed`,
`filament_retraction_length`, `filament_z_hop`, `filament_wipe`, ...) are cleared by
`PrintConfigDef::handle_legacy`'s ignore set or its `!print_config_def.has(opt_key)` fallback
before any deserialize happens. They are not part of the problem and need no translation.

## Inventory

### A. Fork SCALAR / Bambu VECTOR (6)

These are the per-extruder vectorization gap. Bambu can write a `nil` in any of them.
Translation: strip `nil`, then collapse to a single value.

| option | fork type | Bambu type |
|---|---|---|
| `filament_extruder_id` | `coInt` | `coInts` |
| `flush_multiplier` | `coFloat` | `coFloats` |
| `nozzle_type` | `coEnum` | `coEnums` |
| `nozzle_volume` | `coFloat` | `coFloats` |
| `top_solid_infill_flow_ratio` | `coFloat` | `coFloats` |
| `travel_speed_z` | `coFloat` | `coFloats` |

Until 2026-09-21 this table had 32 rows. The High-Flow work (c3c82dbf64) retyped the other 26 to
per-flow-variant vectors, and 22 of them now have **exactly** Bambu's type, so they left the
mismatch list altogether: `bridge_speed`, `default_acceleration`, `enable_overhang_speed`,
`gap_infill_speed`, `initial_layer_acceleration`, `initial_layer_infill_speed`,
`initial_layer_speed`, `inner_wall_acceleration`, `inner_wall_speed`,
`internal_solid_infill_speed`, `outer_wall_acceleration`, `outer_wall_speed`,
`small_perimeter_speed`, `small_perimeter_threshold`, `sparse_infill_acceleration`,
`sparse_infill_speed`, `support_interface_speed`, `support_speed`, `top_surface_acceleration`,
`top_surface_speed`, `travel_acceleration`, `travel_speed`. The four `overhang_N_4_speed` rows
moved to table C (arity now matches, base type does not). A `nil` in any of the 26 takes the
backfill path - see table D2.

### A2. Fork VECTOR / Bambu SCALAR (12)

| option | fork type | Bambu type | note |
|---|---|---|---|
| `filament_notes` | `coStrings` | `coString` | harmless: a scalar string deserializes into a 1-element vector |
| `scale` | `coStrings` | `coFloat` | artefact of a **duplicate declaration** in this fork, see below |
| `ironing_speed` | `coFloats` | `coFloat` | High-Flow retype; harmless, see below |
| `default_jerk`, `outer_wall_jerk`, `inner_wall_jerk`, `infill_jerk`, `top_surface_jerk`, `initial_layer_jerk`, `travel_jerk` | `coFloats` | `coFloat` | High-Flow retype; harmless |
| `accel_to_decel_enable` | `coBools` | `coBool` | High-Flow retype; harmless |
| `accel_to_decel_factor` | `coPercents` | `coPercent` | High-Flow retype; harmless |

The High-Flow rows are harmless for the same reason as `filament_notes`: Bambu writes a scalar,
which deserializes into a 1-element vector, and `Preset::normalize` grows it to the length of
`process_flow_support`. Bambu never writes `nil` into a scalar, so none of them reaches the
translation.

### B. Nullable-axis mismatches on shared keys: **none**

No key is a plain vector on one side and a `*Nullable` vector on the other. All 15 shared
`add_nullable` `filament_*` options have identical types on both sides. This matters: it means the
nullable axis needs no translation for keys both sides know about, and the only nullable-related
work is the `nil`-in-a-non-nullable-type problem handled here.

Asymmetries land in the missing-key counts instead:
- Bambu-only nullable options the fork does not declare (7): `filament_bridge_speed`,
  `filament_enable_overhang_speed`, `filament_overhang_{1,2,3,4}_4_speed`,
  `filament_overhang_totally_speed`. These are dropped as unknown keys - no crash, but the
  per-filament overhang overrides a Bambu user set are silently lost. Worth adding if
  cross-compatibility is to be complete.
- Fork-only nullable options (3): `filament_retract_length_toolchange`,
  `filament_retract_restart_extra_toolchange`, `filament_retract_lift_enforce`.

### C. Same arity, different base type (16)

None of these can carry a `nil`, so none of them breaks loading today. They are recorded because
they are silent precision/semantics differences.

| option | fork type | Bambu type |
|---|---|---|
| `fan_cooling_layer_time` | `coFloats` | `coInts` |
| `fan_max_speed` | `coFloats` | `coInts` |
| `fan_min_speed` | `coFloats` | `coInts` |
| `slow_down_layer_time` | `coFloats` | `coInts` |
| `initial_layer_line_width` | `coFloatOrPercent` | `coFloat` |
| `inner_wall_line_width` | `coFloatOrPercent` | `coFloat` |
| `internal_solid_infill_line_width` | `coFloatOrPercent` | `coFloat` |
| `line_width` | `coFloatOrPercent` | `coFloat` |
| `outer_wall_line_width` | `coFloatOrPercent` | `coFloat` |
| `skeleton_infill_line_width` | `coFloatOrPercent` | `coFloat` |
| `skin_infill_line_width` | `coFloatOrPercent` | `coFloat` |
| `sparse_infill_line_width` | `coFloatOrPercent` | `coFloat` |
| `support_line_width` | `coFloatOrPercent` | `coFloat` |
| `top_surface_line_width` | `coFloatOrPercent` | `coFloat` |
| `seam_gap` | `coFloatOrPercent` | `coPercent` |
| `wipe_speed` | `coFloatOrPercent` | `coPercent` |
| `overhang_1_4_speed` | `coFloatsOrPercents` | `coFloats` |
| `overhang_2_4_speed` | `coFloatsOrPercents` | `coFloats` |
| `overhang_3_4_speed` | `coFloatsOrPercents` | `coFloats` |
| `overhang_4_4_speed` | `coFloatsOrPercents` | `coFloats` |

`coFloats` reading a Bambu `coInts` value is safe (widening). The `coFloatOrPercent` rows are a
fork superset: the fork accepts everything Bambu writes plus a percent form.

The four `overhang_N_4_speed` rows used to be a double mismatch (scalar-vs-vector as well). Since
the High-Flow retype only the base type differs: a Bambu value of `"50"` means 50 mm/s on both
sides, and a fork-side percent has no Bambu equivalent (a fork-only extension). Unlike the rest of
this table they **can** carry a `nil` - they are vectors - and take the backfill path (table D2).

### D. Non-nullable vectors that can still receive a `nil`

Not a *type* mismatch - both sides call these vectors - but the fork's is not nullable, so a Bambu
`nil` still throws. Observed in the owner's data:

| option | fork type |
|---|---|
| `filament_flow_ratio` | `coFloats` |
| `filament_max_volumetric_speed` | `coFloats` |
| `nozzle_temperature` | `coInts` |
| `nozzle_temperature_initial_layer` | `coInts` |
| `long_retractions_when_ec` | `coBools` |
| `retraction_distances_when_ec` | `coFloats` |
| `machine_max_jerk_x` / `_y` / `_e` | `coFloats` |

These must **not** be collapsed to a scalar - that would destroy per-extruder indexing. They are
backfilled instead, preserving the slot count.

### D2. Per-flow-variant vectors (the High-Flow retype)

The 26 options that left table A are now non-nullable vectors listed in
`process_flow_variant_options()` (`PrintConfig.cpp`), so a Bambu `nil` in them is **backfilled**,
not collapsed: `["200","nil","200","nil","160"]` loads as `200,200,200,200,160`, where it used to
load as the scalar `200`.

This was checked against how the slicer reads them, because the two sides index these vectors
differently:

- **Bambu** writes one slot per *extruder variant* - `print_extruder_variant`, 4 entries in
  `fdm_process_dual_common.json`, 5-7 in the H2D/H2C process presets
  (`Direct Drive Standard`, `Direct Drive High Flow`, ... per extruder).
- **This fork** reads one slot per *flow variant*: `get_config_idx(config, ConfigFlowDomain::Process, ...)`
  looks the filament's flow type up in `process_flow_support` (default `["standard"]`; an unknown
  type resolves to 0). A Bambu preset carries no `process_flow_support`, so every read resolves to
  **slot 0**, for standard and high-flow filaments alike.
- `print_extruder_variant` / `print_extruder_id` are declared here but nothing indexes by them.

Consequences:

1. **The value the slicer uses is Bambu's slot 0** - extruder 1, Direct Drive Standard - which is
   what Bambu Studio itself prints with on that nozzle. Where slot 0 differs from the majority this
   is a *change* from the old collapse, and a more faithful one: `support_interface_speed`
   `30,50,nil,50,nil` (in the 43-preset corpus) now slices at 30 (was 50), and
   `outer_wall_acceleration` `3000,2000,nil,2000,nil` (fixture `0.30mm @H2D - Custom ASA`) at 3000
   (was 2000).
2. **Keeping the full slot count is right.** `Preset::normalize` only ever *grows* a flow-variant
   vector to the length of `process_flow_support`; it never truncates, and the extra slots are
   inert. The fork's own shipped BBL H2D/H2C system profiles already load with the same 5-7 slot
   layout, so a translated Bambu user preset has the same shape as its system parent.
   Collapsing to one slot would make it differ from that parent in every such key.
3. **Slots 0 and 1 line up.** If a high-flow column is ever added to such a preset
   (`process_flow_support = ["standard","high_flow"]`), a high-flow filament reads slot 1, which is
   Bambu's `Direct Drive High Flow` on extruder 1. Slots 2+ (extruder 2, TPU) are never read.
4. A nil in slot 0 is filled from the first real value after it, so the value the slicer reads is
   never invented: it is always a value Bambu wrote for some variant.

`tests/slic3rutils/bambu_config_compat_tests.cpp` pins all of this end-to-end ("a backfilled Bambu
vector is read at the slot the slicer uses").

### Incidental findings

The fork declares some keys **twice**; the later declaration wins at runtime and the earlier one
is dead code. Bambu declares each once.

- `scale` - `coFloat` at `PrintConfig.cpp:10115`, then `coStrings` at `:10601`. This redeclaration
  is the only reason `scale` appears in table A2 above.
- `outer_wall_acceleration` - `coFloats` at `:3037` and again at `:3069` (same type; the second
  one's default of 500 wins over the first one's 10000). Predates the High-Flow retype, which
  converted both.
- `downward_check` (formerly `coStrings`, then `coBool`) is now declared once.

Neither is caused by this change; both are worth cleaning up separately.

## The translation

Lives in `src/libslic3r/BambuConfigCompat.{hpp,cpp}`, called from
`ConfigBase::load_from_json_document` in `src/libslic3r/Config.cpp`.

### Why there

`PrintConfigDef::handle_legacy(opt_key, value)` (`PrintConfig.cpp` ~8857) looks like the natural
hook - it takes both key and value by reference and already rewrites both. It is not usable for
this, because on the **array** path (`Config.cpp` ~598 scalar / ~1105 array) it is
called *before* `value_str` is assembled from the array elements (assembly happens ~30 lines
later). At that point `value` is still empty, so a value-rewriting translation there has nothing
to rewrite.

Moving or duplicating the `handle_legacy` call to after assembly was considered and rejected: by
then the elements have been joined into a single comma-separated string, so the translation would
have to re-split it, and `handle_legacy` has no access to the `ConfigOptionDef` it needs to know
whether the target is a scalar, a vector, or nullable.

The chosen site is the existing collapse block at `Config.cpp` ~1163, where `array_values` is
still a `std::vector<std::string>` and `optdef` is in hand. That block already collapsed
single-element and all-equal arrays for scalar targets; the `nil` translation runs just before it
and sets a flag so the two do not both fire.

### Rules

| input | fork option | outcome |
|---|---|---|
| no `nil` | any | untouched |
| any | nullable | **untouched** - the type handles `nil` natively; rewriting would destroy it |
| all slots `nil` | any | key dropped, option keeps its compiled-in default. No value is invented |
| non-nil slots all equal | scalar | collapse to that value (`Collapsed`) |
| non-nil slots differ | scalar | **lossy**: majority of non-nil slots wins, first non-nil slot breaks a tie (`CollapsedLossy`) |
| non-nil slots all equal | vector, non-nullable | backfill each `nil` from the nearest real neighbour, slot count preserved (`Backfilled`) |
| non-nil slots differ | vector, non-nullable | same backfill, flagged **lossy** (`BackfilledLossy`) |

The scalar tie-break takes the **first** non-nil slot because Bambu writes the primary extruder
first, so slot 0 is the value a single-extruder machine actually prints with. The majority rule
ahead of it handles the common H2D shape `["200","nil","200","nil","160"]`, where two slots agree
and a third (a different nozzle) does not - taking 200 matches what the machine's main nozzle does.

Backfill takes the nearest *preceding* non-nil value, or the first following one when the array
starts with `nil`, so an extruder that had no value of its own inherits its neighbour's rather
than a fabricated constant.

## What the user sees

Never silent. Three layers:

1. **Log, always.** Every translated option logs at `info` (clean) or `warning` (lossy), naming
   the key, the file, the original array and the result. A per-file summary line records how many
   options were translated and how many were lossy.
2. **Substitution report, when lossy.** Lossy and all-nil cases are pushed into the existing
   `ConfigSubstitutionContext`, which `PresetCollection::load_presets` already collects and
   `show_substitutions_info` (`src/slic3r/GUI/GUI.cpp:381`) already renders as a dialog listing
   each preset and each changed value. No new mechanism was invented, and the existing
   `skip_settings_mapping_warnings` opt-out continues to apply.
3. **Clean collapses are deliberately not surfaced in the dialog.** Collapsing
   `["200","nil","200"]` to `200` loses nothing; reporting 1346 such cases would bury the 50 that
   matter. They remain in the log.

## Round-trip safety on save

The mirror from Bambu Studio is **one-way**: this fork only ever reads from Bambu's folder and
writes into its own (`%APPDATA%/EdgeSlicer`). Nothing in this change writes to
`%APPDATA%/BambuStudio`.

When the fork later saves a translated preset **into its own folder**, it serializes from its own
option types, so a collapsed scalar is written as a scalar (`"top_surface_acceleration": "100"`)
and a backfilled vector as a full vector with no `nil`. The original per-extruder structure of the
lossy cases is therefore not recoverable from the fork's copy - which is why those cases are
reported rather than applied silently.

The user's Bambu Studio copy is unaffected in all cases. If a two-way sync is ever added, this
becomes a real concern and the lossy set is exactly the list that must not be written back.

## Should any of these be retyped instead?

Retyping a scalar to a vector to match upstream is the cleaner long-term answer for the speed and
acceleration families - it would make the fork's model match the hardware (H2D/H2C really do have
per-extruder speeds) and remove the lossy class entirely.

**Update (2026-09-22):** the High-Flow work (c3c82dbf64) has since done this for 26 of the 32
options, as per-flow-variant vectors (table D2). The six left in table A are the ones that remain
scalar. The advice below still applies to them.

**It was not done as part of this change, and should not be done casually.** The High-Flow retype of
`internal_bridge_speed` from a scalar to a vector is precisely what produced this week's `G1 F0`
outage: every read site has to be audited, because a `coFloat` read as `opt->value` silently
becomes `opt->values[idx]` with a different default and a different empty-vector behaviour.

If it is attempted later, the cost per option is roughly:
- every `config.opt_float("key")` / `->value` read site retargeted with a correct extruder index
- the GUI field for the option converted to a per-extruder control
- profile migration for every shipped profile that sets the option as a scalar
- a regression test asserting the option is non-zero at the G-code writer

Of the six left, `travel_speed_z` is the best candidate (few read sites) and
`top_solid_infill_flow_ratio` the worst (widely read, rarely differs per extruder). Doing them
one at a time, each with its own gate, is the only safe route.

## Tests

`tests/slic3rutils/bambu_config_compat_tests.cpp`, tag `[BambuCompat]`. Covers each rule in the
table above on both paths - the collapse path on a key picked at run time from table A that must
still be scalar (so a future retype fails loudly instead of silently testing a vector), and the
backfill path on the retyped keys - plus an all-nil array, an untouched nullable option, a
regression asserting that no translation ever yields `0` for a speed or acceleration while a zero
Bambu really wrote is preserved, and an end-to-end check that a translated H2D preset is read at
slot 0 for standard and high-flow filaments. Fixtures in `tests/data/bambu_compat/` and
`tests/data/bambu_compat_43/` are copies of real presets from the owner's Bambu Studio data;
nothing reads the live folder at test time.

### Verified results

Re-verified 2026-09-22 on `fix/bambu-compat-tests` (main 0ebb94d331 plus test changes only):

`[BambuCompat]`: 27 test cases, 962 assertions, all passing.

`slic3rutils_tests` (default run, as ctest runs it): 255 test cases, 3522 assertions, all passing. The three `[Http]`
cases that talk to github.com and jigsaw.w3.org are hidden (`[.network]`): jigsaw.w3.org's
`/HTTP/Basic/` answers 403 to every client now, plain curl included, so it failed on every machine
without saying anything about this code. Run them on purpose with `slic3rutils_tests "[network]"`.

The 43 presets named in `2026-09-21-16-52-41.log.0` as `parse config ... failed`, run through the
fork's own `load_from_json`:

| outcome | count |
|---|---|
| now load successfully | **43 / 43** |
| still fail | 0 |
| load with at least one lossy value | 2 |
| total lossy values | 4 |
| nil arrays translated, all via backfill (no scalar collapse left in this corpus) | 389 |

The four lossy values, all `BackfilledLossy` on an H2D preset where the second nozzle genuinely
differs (before the High-Flow retype they were `CollapsedLossy` to 100, 100, 100 and 50):

```
0.20mm @H2D - ABS.json     inner_wall_speed            100,nil,100,nil,150 -> 100,100,100,100,150
0.20mm @H2D - ABS.json     internal_solid_infill_speed 100,nil,100,nil,180 -> 100,100,100,100,180
0.20mm @H2D - ABS.json     sparse_infill_speed         100,nil,100,nil,180 -> 100,100,100,100,180
0.30mm @H2D - Custom.json  support_interface_speed     30,50,nil,50,nil    -> 30,50,50,50,50
```

No real value is lost any more: only the `nil` slots are guesses. The slicer reads slot 0, so the
last one now slices at 30 (Bambu's extruder-1 standard value) rather than the old majority 50.

## Note for other work in flight

The destructive delete-on-parse-failure in `PresetCollection::load_presets` is **not** addressed
here - this change prevents the parse failure, it does not make failure non-destructive. That is
tracked separately (quarantine instead of erase). This branch does not modify `Preset.cpp` at all,
so the two changes should not conflict.
