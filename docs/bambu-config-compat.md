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

This fork types many of the same options as **scalars**, and others as **plain, non-nullable
vectors**. Either way `ConfigOption*::deserialize()` throws

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

Scan of 275 presets copied from `%APPDATA%/BambuStudio/user/<id>/`:

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

### A. Fork SCALAR / Bambu VECTOR (32)

These are the per-extruder vectorization gap. Bambu can write a `nil` in any of them.
Translation: strip `nil`, then collapse to a single value.

| option | fork type | Bambu type |
|---|---|---|
| `bridge_speed` | `coFloat` | `coFloats` |
| `default_acceleration` | `coFloat` | `coFloats` |
| `enable_overhang_speed` | `coBool` | `coBools` |
| `filament_extruder_id` | `coInt` | `coInts` |
| `flush_multiplier` | `coFloat` | `coFloats` |
| `gap_infill_speed` | `coFloat` | `coFloats` |
| `initial_layer_acceleration` | `coFloat` | `coFloats` |
| `initial_layer_infill_speed` | `coFloat` | `coFloats` |
| `initial_layer_speed` | `coFloat` | `coFloats` |
| `inner_wall_acceleration` | `coFloat` | `coFloats` |
| `inner_wall_speed` | `coFloat` | `coFloats` |
| `internal_solid_infill_speed` | `coFloat` | `coFloats` |
| `nozzle_type` | `coEnum` | `coEnums` |
| `nozzle_volume` | `coFloat` | `coFloats` |
| `outer_wall_acceleration` | `coFloat` | `coFloats` |
| `outer_wall_speed` | `coFloat` | `coFloats` |
| `overhang_1_4_speed` | `coFloatOrPercent` | `coFloats` |
| `overhang_2_4_speed` | `coFloatOrPercent` | `coFloats` |
| `overhang_3_4_speed` | `coFloatOrPercent` | `coFloats` |
| `overhang_4_4_speed` | `coFloatOrPercent` | `coFloats` |
| `small_perimeter_speed` | `coFloatOrPercent` | `coFloatsOrPercents` |
| `small_perimeter_threshold` | `coFloat` | `coFloats` |
| `sparse_infill_acceleration` | `coFloatOrPercent` | `coFloatsOrPercents` |
| `sparse_infill_speed` | `coFloat` | `coFloats` |
| `support_interface_speed` | `coFloat` | `coFloats` |
| `support_speed` | `coFloat` | `coFloats` |
| `top_solid_infill_flow_ratio` | `coFloat` | `coFloats` |
| `top_surface_acceleration` | `coFloat` | `coFloats` |
| `top_surface_speed` | `coFloat` | `coFloats` |
| `travel_acceleration` | `coFloat` | `coFloats` |
| `travel_speed` | `coFloat` | `coFloats` |
| `travel_speed_z` | `coFloat` | `coFloats` |

The four `overhang_N_4_speed` rows are a **double** mismatch: scalar-vs-vector *and*
`FloatOrPercent`-vs-plain-`Float`. A Bambu value of `"50"` means 50 mm/s in both, so the arity fix
is enough in practice; a fork-side percent has no Bambu equivalent and is a fork-only extension.

### A2. Fork VECTOR / Bambu SCALAR (2)

| option | fork type | Bambu type | note |
|---|---|---|---|
| `filament_notes` | `coStrings` | `coString` | harmless: a scalar string deserializes into a 1-element vector |
| `scale` | `coStrings` | `coFloat` | artefact of a **duplicate declaration** in this fork, see below |

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

`coFloats` reading a Bambu `coInts` value is safe (widening). The `coFloatOrPercent` rows are a
fork superset: the fork accepts everything Bambu writes plus a percent form.

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

### Incidental findings

The fork declares two keys **twice, with different types**; the later declaration wins at runtime
and the earlier one is dead code. Bambu declares each once.

- `downward_check` - `coStrings` at `PrintConfig.cpp:9533`, then `coBool` at `:9850`.
- `scale` - `coFloat` at `:9772`, then `coStrings` at `:10258`. This redeclaration is the only
  reason `scale` appears in table A2 above.

Neither is caused by this change; both are worth cleaning up separately.

## The translation

Lives in `src/libslic3r/BambuConfigCompat.{hpp,cpp}`, called from
`ConfigBase::load_from_json_document` in `src/libslic3r/Config.cpp`.

### Why there

`PrintConfigDef::handle_legacy(opt_key, value)` (`PrintConfig.cpp:8508`) looks like the natural
hook - it takes both key and value by reference and already rewrites both. It is not usable for
this, because on the **array** path (`Config.cpp:576` scalar / `Config.cpp:1045` array) it is
called *before* `value_str` is assembled from the array elements (assembly happens ~30 lines
later). At that point `value` is still empty, so a value-rewriting translation there has nothing
to rewrite.

Moving or duplicating the `handle_legacy` call to after assembly was considered and rejected: by
then the elements have been joined into a single comma-separated string, so the translation would
have to re-split it, and `handle_legacy` has no access to the `ConfigOptionDef` it needs to know
whether the target is a scalar, a vector, or nullable.

The chosen site is the existing collapse block at `Config.cpp` ~1096, where `array_values` is
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

**It is not done here, and should not be done casually.** The High-Flow retype of
`internal_bridge_speed` from a scalar to a vector is precisely what produced this week's `G1 F0`
outage: every read site has to be audited, because a `coFloat` read as `opt->value` silently
becomes `opt->values[idx]` with a different default and a different empty-vector behaviour.

If it is attempted later, the cost per option is roughly:
- every `config.opt_float("key")` / `->value` read site retargeted with a correct extruder index
- the GUI field for the option converted to a per-extruder control
- profile migration for every shipped profile that sets the option as a scalar
- a regression test asserting the option is non-zero at the G-code writer

`travel_speed`, `default_acceleration` and the `overhang_N_4_speed` family are the best candidates
(few read sites, genuinely per-extruder in hardware). `small_perimeter_threshold` and
`top_solid_infill_flow_ratio` are the worst (widely read, rarely differ per extruder). Doing them
one at a time, each with its own gate, is the only safe route.

## Tests

`tests/slic3rutils/bambu_config_compat_tests.cpp`, tag `[BambuCompat]`. Covers each rule in the
table above, an all-nil array, an untouched nullable option, and a regression asserting that no
translation ever yields `0` for a speed or acceleration while a zero Bambu really wrote is
preserved. Fixtures in `tests/data/bambu_compat/` and `tests/data/bambu_compat_43/` are copies of
real presets from the owner's Bambu Studio data; nothing reads the live folder at test time.

### Verified results

`[BambuCompat]`: 18 test cases, 296 assertions, all passing.

Full suites after the change:

| suite | result |
|---|---|
| `slic3rutils_tests` | 231 cases, 230 passed, 1 failed - the pre-existing `[NotWorking]` Http basic-auth test (`slic3rutils_tests_main.cpp:58`, external service returns 403) |
| `libslic3r_tests` | 1184 cases, 1181 passed, 3 failed **as expected** (pre-existing `[!shouldfail]`) |

The 43 presets named in `2026-09-21-16-52-41.log.0` as `parse config ... failed`, run through the
fork's own `load_from_json`:

| outcome | count |
|---|---|
| now load successfully | **43 / 43** |
| still fail | 0 |
| load with at least one lossy value | 2 |
| total lossy values | 4 |

The four lossy values, all of them `CollapsedLossy` on an H2D preset where the second nozzle
genuinely differs:

```
0.20mm @H2D - ABS.json     inner_wall_speed            100,nil,100,nil,150 -> 100
0.20mm @H2D - ABS.json     internal_solid_infill_speed 100,nil,100,nil,180 -> 100
0.20mm @H2D - ABS.json     sparse_infill_speed         100,nil,100,nil,180 -> 100
0.30mm @H2D - Custom.json  support_interface_speed     30,50,nil,50,nil    -> 50
```

The last one shows the majority rule working as intended: `50` occupies two slots and wins over
the first slot's `30`.

## Note for other work in flight

The destructive delete-on-parse-failure in `PresetCollection::load_presets` is **not** addressed
here - this change prevents the parse failure, it does not make failure non-destructive. That is
tracked separately (quarantine instead of erase). This branch does not modify `Preset.cpp` at all,
so the two changes should not conflict.
