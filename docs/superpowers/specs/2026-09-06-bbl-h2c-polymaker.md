# Polymaker filament presets for the Bambu Lab H2C

Date: 2026-09-06
Branch: `feat/bbl-h2c-polymaker` (from `origin/feat/ultra-preferences`)

## Problem

The fork ships 474 Polymaker-vendor filament presets across 77 filaments for
Bambu Lab printers, but **not one of them had an `@BBL H2C` variant**. The H2C
machine itself is fully supported (4 nozzle profiles, 18 process profiles, 211
Bambu/Generic H2C filaments), so an H2C owner saw the printer but no Polymaker
filaments at all.

## The source: Polymaker publish official H2C presets

The premise that no official H2C data exists turned out to be wrong.
Polymaker's preset site <https://presets.polymaker.com> is backed by
**<https://github.com/Polymaker3D/Polymaker-Preset>** (MIT licensed), whose
`preset/<Filament>/BBL/<Printer>/BambuStudio/` tree contains **54 official
`@BBL H2C` presets**. Their BBL coverage:

| Printer | Official BambuStudio presets |
| --- | --- |
| X2D | 66 |
| H2S | 61 |
| P2S | 58 |
| H2D | 57 |
| **H2C** | **54** |
| A2L | 47 |
| X1 | 33 |
| A1 | 31 |

These are real per-printer tuning, not copies of the H2D values: across the 52
filaments that have both, H2C differs from H2D in `filament_flow_ratio` (50/52),
`filament_max_volumetric_speed` (46/52), `nozzle_temperature` (39/52), fan
curves, plate and chamber temperatures, and the whole nozzle-change (`_nc`)
family.

## What was added

**68 new presets**, all `compatible_printers: ["Bambu Lab H2C 0.4 nozzle"]`:

* **54 ported from Polymaker's official H2C presets**
* **14 derived** from the fork's own H2D (else H2S/P2S) sibling, for filaments
  Polymaker ships for other Bambu printers but not yet for the H2C

Each new file is registered in `resources/profiles/BBL.json` → `filament_list`
(1974 → 2042 entries), positioned beside its siblings but always after its
parent — see the validator section for why that ordering matters.

## Design decisions

### Inheritance

New presets inherit the same parent Polymaker's own H2C presets declare — e.g.
`Generic PLA @BBL H2C 0.4 nozzle`, `Generic PA-CF @BBL H2C 0.4 nozzle`,
`Bambu PET-CF @BBL H2C 0.4 nozzle`, `Bambu ABS @BBL H2C`. Every one of those
parents already exists **and is registered** in the fork, and each already
carries the full H2C plumbing (the `_nc` family, `filament_preheat_temperature_delta`,
`first_x_layer_fan_speed`, and `include: fdm_filament_template_direct_dual_e3d`).

Only keys whose ported value **differs from the fully resolved parent** are
emitted, so the diffs stay small and reviewable and the base keeps ownership of
everything the two agree on. This also matches the fork's existing
`Polymaker * @BBL A2L` style, which inherits `Generic * @BBL A2L`.

The `Polymaker/` subdirectory's `<Filament> @base` files were **not** used as
parents: 259 of the 273 files in that directory are not referenced from
`BBL.json` at all (only the 14 Fiberon `@base`/`@BBL H2D` files are), so
inheriting from them would not resolve when the vendor bundle loads.

### Dual-nozzle array arity

`Bambu Lab H2C 0.4 nozzle` declares three extruder variants:

```
"extruder_variant_list": [
    "Direct Drive Standard,Direct Drive High Flow,Direct Drive E3D High Flow",
    "Direct Drive Standard,Direct Drive High Flow"
]
```

All 77 of the fork's own `@BBL H2C 0.4 nozzle` filaments therefore use
`fdm_filament_template_direct_dual_e3d` and 3-element arrays. Polymaker author
only two (`Standard`, `High Flow`) against the 2-variant template, so ported
per-extruder values are expanded `[a, b]` → `[a, b, a]`. That mirrors Bambu's
own convention: slot 2 (E3D High Flow) equals slot 0 (Standard) in 153/155 of
their H2C presets for `filament_max_volumetric_speed`, 137/141 for
`filament_flow_ratio`, 153/155 for `nozzle_temperature`.

`filament_extruder_variant` itself is never copied from upstream — the parent
and its include template own it.

### `nil` slots

Polymaker's README notes that for many materials only `Direct Drive Standard`
is tuned and the other variants are left `nil`. Where a ported per-extruder
array had `nil` in a non-zero slot it was filled with the `Standard` value
rather than left unset, so the High Flow / E3D nozzles get the conservative
tuned value instead of nothing.

### No `_source` key in the JSON

**Checked first, as required.** `ConfigBase::load_from_json()`
(`src/libslic3r/Config.cpp`) routes every non-meta key to `set_deserialize()`,
which reaches `set_deserialize_raw()` and throws `UnknownOptionException` for a
key with no `ConfigOptionDef`. That escapes through the function's
`catch (std::exception&)` and makes `load_from_json` return `-1`, i.e. the whole
preset fails to load. **The loader does not tolerate unknown keys**, so no
`_source` comment key was added — the source URLs live in the table below
instead.

### ID scheme

* **`filament_id` — reused, never minted.** It identifies the filament (AMS
  matching), and the fork's existing `@BBL H2S` / `@BBL P2S` / `@BBL A2L`
  siblings of a given filament already share one. Every new preset repeats its
  filament's existing id (`PMPL24` for Polymaker PLA, `GFB60` for PolyLite ABS,
  …). No new `filament_id` values were created; all 68 are ≤ 8 characters, as
  `check_filament_id` requires.
* **`setting_id` — one new contiguous block, `GFSPM02_00` … `GFSPM02_67`.**
  `GFSPM` is the fork's own namespace for Polymaker presets. `GFSPM00_xx` is 99/100
  used and `GFSPM01_xx` 60/100; the whole `GFSPM02_xx` block was unused, so it was
  taken contiguously to keep this addition auditable. Verified: zero collisions
  with the 2260 existing `setting_id`s.

### Values ported

Per the brief: temperatures (nozzle, initial layer, range, vitrification),
build-plate temperatures (cool / eng / hot / textured / supertack), chamber,
flow ratio, volumetric speed limits, fan and cooling curves, retraction and
wipe, the H2C nozzle-change / prime-tower family, the per-filament flow
calibration coefficients (`counter_*`, `hole_*`), scarf-seam settings, and
filament identity (vendor, type, cost, density, printable, HRC).

Deliberately **not** ported: `filament_start_gcode` / `filament_end_gcode` (the
H2C parent already carries H2C-correct gcode; Polymaker's H2S/P2S variants
carry printer-specific chamber-cooling workarounds), `filament_extruder_variant`,
and export-only bookkeeping keys.

`pressure_advance` is absent from Polymaker's H2C presets (it is present in
their H2D ones), so none was set — the H2C uses its own calibration.

### `filament_cost` taken from the sibling, not the H2C export

45 of the 54 official H2C exports carry `filament_cost: ["20"]`, which is a
placeholder — the same filament's H2D/H2S/P2S exports carry the real price
(`24.99` for Panchroma PLA, `84.99` for Fiberon PA6-CF20, `240` for the PPS
grades). Cost is a property of the filament, not of the printer, and the fork's
existing variants of a filament already share one price. So cost is taken from
the filament's live sibling where that is non-zero, falling back to the upstream
H2C value, falling back to the parent. 27 presets were corrected this way; the
result is zero cost disagreement between a new H2C preset and its siblings.

## Filaments

Source column: `official` links the exact upstream file the values came from.
Deltas are what the new preset overrides on top of its parent; values shown as
`a/b/c` differ per extruder variant.

| Filament (`@BBL H2C`) | Source | Key deltas applied vs parent | `filament_id` | `setting_id` | Inherits |
| --- | --- | --- | --- | --- | --- |
| Fiberon ASA-CF08 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20ASA-CF08/BBL/H2C/BambuStudio/Fiberon%20ASA-CF08%20%40BBL%20H2C.json) | nozzle 300, nozzle1 300, flow 0.89, volspeed 16, chamber 0, fan+ 30, Tg 110.8, density 1.09 _(+23 more)_ | `PMAS01` | `GFSPM02_00` | `Generic ASA @BBL H2C 0.4 nozzle` |
| Fiberon PA12-CF10 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PA12-CF10/BBL/H2C/BambuStudio/Fiberon%20PA12-CF10%20%40BBL%20H2C.json) | flow 0.88, volspeed 12, bed 50, bed(tex) 50, bed(eng) 50, chamber 0, fan- 20, fan+ 40, Tg 55, density 1.06 _(+27 more)_ | `PMPA01` | `GFSPM02_01` | `Generic PA-CF @BBL H2C 0.4 nozzle` |
| Fiberon PA6-CF20 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PA6-CF20/BBL/H2C/BambuStudio/Fiberon%20PA6-CF20%20%40BBL%20H2C.json) | nozzle 300, nozzle1 300, flow 1.02, volspeed 4, bed 50, bed(tex) 50, bed(eng) 50, chamber 0, fan- 0, fan+ 10, Tg 74.2, density 1.17, cost 84.99 _(+27 more)_ | `PMPA02` | `GFSPM02_02` | `Generic PA-CF @BBL H2C 0.4 nozzle` |
| Fiberon PA6-GF25 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PA6-GF25/BBL/H2C/BambuStudio/Fiberon%20PA6-GF25%20%40BBL%20H2C.json) | nozzle 300, nozzle1 300, flow 0.99, volspeed 16, bed 50, bed(tex) 50, bed(eng) 50, chamber 0, fan- 20, Tg 70.4, density 1.2 _(+28 more)_ | `PMPA03` | `GFSPM02_03` | `Generic PA-CF @BBL H2C 0.4 nozzle` |
| Fiberon PA612-CF15 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PA612-CF15/BBL/H2C/BambuStudio/Fiberon%20PA612-CF15%20%40BBL%20H2C.json) | flow 0.969, bed 50, bed(tex) 50, bed(eng) 50, chamber 0, Tg 206.2, density 1.03 _(+27 more)_ | `PMPA04` | `GFSPM02_04` | `Generic PA-CF @BBL H2C 0.4 nozzle` |
| Fiberon PA612-ESD | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PA612-ESD/BBL/H2C/BambuStudio/Fiberon%20PA612-ESD%20%40BBL%20H2C.json) | flow 0.94, volspeed 12, bed 50, bed(tex) 50, bed(eng) 50, chamber 0, fan- 0, fan+ 10, Tg 190, density 1.1 _(+27 more)_ | `PMPA05` | `GFSPM02_05` | `Generic PA-CF @BBL H2C 0.4 nozzle` |
| Fiberon PET-CF17 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PET-CF17/BBL/H2C/BambuStudio/Fiberon%20PET-CF17%20%40BBL%20H2C.json) | flow 0.95, volspeed 8, bed 70, bed(tex) 70, bed(eng) 70, bed(st) 70, chamber 0, fan- 20, fan+ 30, Tg 79.3, density 1.34, cost 81.99 _(+22 more)_ | `PMPE01` | `GFSPM02_06` | `Bambu PET-CF @BBL H2C 0.4 nozzle` |
| Fiberon PET-GF15 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PET-GF15/BBL/H2C/BambuStudio/Fiberon%20PET-GF15%20%40BBL%20H2C.json) | nozzle 310, nozzle1 310, flow 1, volspeed 8, bed 70, bed(tex) 70, bed(eng) 70, bed(st) 70, chamber 0, fan- 0, fan+ 10, Tg 232.6, density 1.43, cost 81.99 _(+23 more)_ | `PMPE08` | `GFSPM02_07` | `Bambu PET-CF @BBL H2C 0.4 nozzle` |
| Fiberon PETG-ESD | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PETG-ESD/BBL/H2C/BambuStudio/Fiberon%20PETG-ESD%20%40BBL%20H2C.json) | nozzle 290, nozzle1 290, flow 0.93, fan- 0, fan+ 20, Tg 77, density 1.24 _(+25 more)_ | `GFL06` | `GFSPM02_08` | `Generic PETG @BBL H2C 0.4 nozzle` |
| Fiberon PETG-rCF08 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PETG-rCF08/BBL/H2C/BambuStudio/Fiberon%20PETG-rCF08%20%40BBL%20H2C.json) | nozzle 270, nozzle1 270, flow 0.99, volspeed 4, fan- 0, fan+ 20, Tg 69.7, density 1.3, cost 19.99 _(+25 more)_ | `PMPE03` | `GFSPM02_09` | `Generic PETG @BBL H2C 0.4 nozzle` |
| Fiberon PPS-CF10 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PPS-CF10/BBL/H2C/BambuStudio/Fiberon%20PPS-CF10%20%40BBL%20H2C.json) | nozzle 350, nozzle1 350, flow 1.02, volspeed 16, fan- 20, fan+ 40, Tg 268, density 1.29 _(+24 more)_ | `PMPP01` | `GFSPM02_10` | `Generic PPS-CF @BBL H2C 0.4 nozzle` |
| Fiberon PPS-GF20 | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Fiberon%20PPS-GF20/BBL/H2C/BambuStudio/Fiberon%20PPS-GF20%20%40BBL%20H2C.json) | nozzle 340, nozzle1 340, flow 1.03, volspeed 8, fan+ 10, Tg 272, density 1.36 _(+24 more)_ | `PMPP02` | `GFSPM02_11` | `Generic PPS-CF @BBL H2C 0.4 nozzle` |
| Panchroma CoPE | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20CoPE/BBL/H2C/BambuStudio/Panchroma%20CoPE%20%40BBL%20H2C.json) | flow 0.98, volspeed 15, Tg 58.2, density 1.29 _(+21 more)_ | `PMCO01` | `GFSPM02_12` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA/BBL/H2C/BambuStudio/Panchroma%20PLA%20%40BBL%20H2C.json) | nozzle 230, nozzle1 230, volspeed 15, Tg 61, density 1.17 _(+20 more)_ | `PMPL01` | `GFSPM02_13` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Celestial | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Celestial/BBL/H2C/BambuStudio/Panchroma%20PLA%20Celestial%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL02` | `GFSPM02_14` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Galaxy | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Galaxy/BBL/H2C/BambuStudio/Panchroma%20PLA%20Galaxy%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL03` | `GFSPM02_15` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Glow | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Glow/BBL/H2C/BambuStudio/Panchroma%20PLA%20Glow%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL04` | `GFSPM02_16` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Luminous | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Luminous/BBL/H2C/BambuStudio/Panchroma%20PLA%20Luminous%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL05` | `GFSPM02_17` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Marble | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Marble/BBL/H2C/BambuStudio/Panchroma%20PLA%20Marble%20%40BBL%20H2C.json) | flow 1.01, Tg 61, density 1.37 _(+20 more)_ | `PMPL06` | `GFSPM02_18` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Matte | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Matte/BBL/H2C/BambuStudio/Panchroma%20PLA%20Matte%20%40BBL%20H2C.json) | flow 1.01, Tg 61, density 1.37 _(+20 more)_ | `PMPL07` | `GFSPM02_19` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Metallic | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Metallic/BBL/H2C/BambuStudio/Panchroma%20PLA%20Metallic%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL08` | `GFSPM02_20` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Neon | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Neon/BBL/H2C/BambuStudio/Panchroma%20PLA%20Neon%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL09` | `GFSPM02_21` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Satin | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Satin/BBL/H2C/BambuStudio/Panchroma%20PLA%20Satin%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 59, cost 24.99 _(+20 more)_ | `PMPL10` | `GFSPM02_22` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Silk | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Silk/BBL/H2C/BambuStudio/Panchroma%20PLA%20Silk%20%40BBL%20H2C.json) | flow 0.94, Tg 58.2, density 1.34 _(+20 more)_ | `PMPL11` | `GFSPM02_23` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Starlight | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Starlight/BBL/H2C/BambuStudio/Panchroma%20PLA%20Starlight%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL12` | `GFSPM02_24` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA Translucent | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20Translucent/BBL/H2C/BambuStudio/Panchroma%20PLA%20Translucent%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL13` | `GFSPM02_25` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Panchroma PLA UV Shift | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Panchroma%20PLA%20UV%20Shift/BBL/H2C/BambuStudio/Panchroma%20PLA%20UV%20Shift%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL14` | `GFSPM02_26` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite CosPLA | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20CosPLA/BBL/H2C/BambuStudio/PolyLite%20CosPLA%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 59, cost 24.99 _(+20 more)_ | `PMCO02` | `GFSPM02_27` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PLA | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20PLA/BBL/H2C/BambuStudio/PolyLite%20PLA%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 25.4 _(+20 more)_ | `GFL00` | `GFSPM02_28` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PLA Galaxy | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20PLA%20Galaxy/BBL/H2C/BambuStudio/PolyLite%20PLA%20Galaxy%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL16` | `GFSPM02_29` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PLA Glow | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20PLA%20Glow/BBL/H2C/BambuStudio/PolyLite%20PLA%20Glow%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL17` | `GFSPM02_30` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PLA Luminous | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20PLA%20Luminous/BBL/H2C/BambuStudio/PolyLite%20PLA%20Luminous%20%40BBL%20H2C.json) | nozzle 230, nozzle1 230, volspeed 15, Tg 61, density 1.17 _(+20 more)_ | `PMPL18` | `GFSPM02_31` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PLA Neon | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20PLA%20Neon/BBL/H2C/BambuStudio/PolyLite%20PLA%20Neon%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL19` | `GFSPM02_32` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PLA Pro | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20PLA%20Pro/BBL/H2C/BambuStudio/PolyLite%20PLA%20Pro%20%40BBL%20H2C.json) | volspeed 15, Tg 62, density 1.22, cost 24.99 _(+20 more)_ | `PMPL20` | `GFSPM02_33` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PLA Pro Metallic | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20PLA%20Pro%20Metallic/BBL/H2C/BambuStudio/PolyLite%20PLA%20Pro%20Metallic%20%40BBL%20H2C.json) | volspeed 15, Tg 62, density 1.22, cost 24.99 _(+20 more)_ | `PMPL21` | `GFSPM02_34` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PLA Starlight | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20PLA%20Starlight/BBL/H2C/BambuStudio/PolyLite%20PLA%20Starlight%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL22` | `GFSPM02_35` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PLA Translucent | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyLite%20PLA%20Translucent/BBL/H2C/BambuStudio/PolyLite%20PLA%20Translucent%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 61, density 1.17, cost 24.99 _(+20 more)_ | `PMPL23` | `GFSPM02_36` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyMax PLA | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyMax%20PLA/BBL/H2C/BambuStudio/PolyMax%20PLA%20%40BBL%20H2C.json) | flow 1.04, Tg 61, density 1.19 _(+20 more)_ | `PMPL32` | `GFSPM02_37` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyTerra PLA | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyTerra%20PLA/BBL/H2C/BambuStudio/PolyTerra%20PLA%20%40BBL%20H2C.json) | flow 1.01, Tg 61, density 1.37, cost 25.4 _(+20 more)_ | `GFL01` | `GFSPM02_38` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyTerra PLA Marble | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyTerra%20PLA%20Marble/BBL/H2C/BambuStudio/PolyTerra%20PLA%20Marble%20%40BBL%20H2C.json) | flow 1.01, Tg 61, density 1.37 _(+20 more)_ | `PMPL28` | `GFSPM02_39` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyTerra PLA+ | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/PolyTerra%20PLA%2B/BBL/H2C/BambuStudio/PolyTerra%20PLA%2B%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 59, cost 24.99 _(+20 more)_ | `PMPL29` | `GFSPM02_40` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Polylite ASA for TYC Americas | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polylite%20ASA%20for%20TYC%20Americas/BBL/H2C/BambuStudio/Polylite%20ASA%20for%20TYC%20Americas%20%40BBL%20H2C.json) | flow 1.01, volspeed 4, fan- 40, fan+ 70, Tg 105, density 1.13 _(+24 more)_ | `PMAS04` | `GFSPM02_41` | `Generic ASA @BBL H2C 0.4 nozzle` |
| Polymaker ABS Max | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20ABS%20Max/BBL/H2C/BambuStudio/Polymaker%20ABS%20Max%20%40BBL%20H2C.json) | nozzle 280, nozzle1 280, volspeed 12, bed 110, bed(tex) 110, bed(eng) 110, fan+ 20, Tg 127.4, retract 0.4/0.6/0.6, density 1.06, cost 20 _(+18 more)_ | `PMAB02` | `GFSPM02_42` | `Bambu ABS @BBL H2C` |
| Polymaker ABS Pro | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20ABS%20Pro/BBL/H2C/BambuStudio/Polymaker%20ABS%20Pro%20%40BBL%20H2C.json) | nozzle1 270, flow 1.04, volspeed 8, bed(tex) 110, fan- 30, fan+ 50, Tg 112 _(+24 more)_ | `PMAB04` | `GFSPM02_43` | `Generic ABS @BBL H2C 0.4 nozzle` |
| Polymaker ABS Pro Galaxy | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20ABS%20Pro%20Galaxy/BBL/H2C/BambuStudio/Polymaker%20ABS%20Pro%20Galaxy%20%40BBL%20H2C.json) | nozzle1 270, flow 1.04, volspeed 8, bed(tex) 110, fan- 30, fan+ 50, Tg 112 _(+24 more)_ | `PMAB03` | `GFSPM02_44` | `Generic ABS @BBL H2C 0.4 nozzle` |
| Polymaker ASA | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20ASA/BBL/H2C/BambuStudio/Polymaker%20ASA%20%40BBL%20H2C.json) | flow 1.01, volspeed 4, fan- 40, fan+ 70, Tg 105, density 1.13 _(+24 more)_ | `PMAS02` | `GFSPM02_45` | `Generic ASA @BBL H2C 0.4 nozzle` |
| Polymaker HT-PLA | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20HT-PLA/BBL/H2C/BambuStudio/Polymaker%20HT-PLA%20%40BBL%20H2C.json) | flow 1.01, volspeed 15, Tg 59.8, density 1.28, cost 24.99 _(+21 more)_ | `PMHT01` | `GFSPM02_46` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Polymaker HT-PLA Pro | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20HT-PLA%20Pro/BBL/H2C/BambuStudio/Polymaker%20HT-PLA%20Pro%20%40BBL%20H2C.json) | flow 1.02, volspeed 16, bed(tex) 60, Tg 60 _(+23 more)_ | `PMPL60` | `GFSPM02_47` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Polymaker HT-PLA-GF | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20HT-PLA-GF/BBL/H2C/BambuStudio/Polymaker%20HT-PLA-GF%20%40BBL%20H2C.json) | flow 0.96, volspeed 15, Tg 59.76, density 1.34 _(+21 more)_ | `PMHT02` | `GFSPM02_48` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Polymaker PETG | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20PETG/BBL/H2C/BambuStudio/Polymaker%20PETG%20%40BBL%20H2C.json) | nozzle 240, nozzle1 240, flow 0.98, volspeed 15, fan- 20, fan+ 60, Tg 71.24, density 1.3 _(+24 more)_ | `PMPE06` | `GFSPM02_49` | `Generic PETG @BBL H2C 0.4 nozzle` |
| Polymaker PETG Galaxy | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20PETG%20Galaxy/BBL/H2C/BambuStudio/Polymaker%20PETG%20Galaxy%20%40BBL%20H2C.json) | nozzle 240, nozzle1 240, flow 0.98, volspeed 15, fan- 20, fan+ 60, Tg 71.24, density 1.3 _(+24 more)_ | `PMPE07` | `GFSPM02_50` | `Generic PETG @BBL H2C 0.4 nozzle` |
| Polymaker PLA | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20PLA/BBL/H2C/BambuStudio/Polymaker%20PLA%20%40BBL%20H2C.json) | flow 0.989, volspeed 15, Tg 59, density 1.23, cost 24.99 _(+20 more)_ | `PMPL24` | `GFSPM02_51` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Polymaker PLA Pro | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20PLA%20Pro/BBL/H2C/BambuStudio/Polymaker%20PLA%20Pro%20%40BBL%20H2C.json) | flow 0.98, volspeed 15, Tg 55, density 1.23 _(+20 more)_ | `PMPL25` | `GFSPM02_52` | `Generic PLA @BBL H2C 0.4 nozzle` |
| Polymaker PLA Pro Metallic | [official](https://github.com/Polymaker3D/Polymaker-Preset/blob/main/preset/Polymaker%20PLA%20Pro%20Metallic/BBL/H2C/BambuStudio/Polymaker%20PLA%20Pro%20Metallic%20%40BBL%20H2C.json) | flow 0.98, volspeed 15, Tg 55, density 1.23 _(+20 more)_ | `PMPL26` | `GFSPM02_53` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyCast | derived from `PolyCast @BBL H2S` | nozzle 210, nozzle1 210, flow 0.96, volspeed 4, bed(tex) 60, Tg 67, density 1.1 _(+9 more)_ | `PMPL34` | `GFSPM02_54` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyFlex TPU95 | derived from `PolyFlex TPU95 @BBL H2S` | flow 1.02, volspeed 5, Tg 0, density 1.2 _(+8 more)_ | `PMTP01` | `GFSPM02_55` | `Generic TPU @BBL H2C 0.4 nozzle` |
| PolyFlex TPU95-HF | derived from `PolyFlex TPU95-HF @BBL H2S` | nozzle 210, nozzle1 210, flow 1.02, volspeed 5, Tg 0, density 1.16 _(+7 more)_ | `PMTP02` | `GFSPM02_56` | `Generic TPU @BBL H2C 0.4 nozzle` |
| PolyLite ABS | derived from `PolyLite ABS @BBL H2D` | flow 0.95, density 1.03 _(+1 more)_ | `GFB60` | `GFSPM02_57` | `Generic ABS @BBL H2C 0.4 nozzle` |
| PolyLite ASA | derived from `PolyLite ASA @BBL H2D` | flow 0.95, volspeed 13, density 1.02, cost 23.6 _(+2 more)_ | `GFB61` | `GFSPM02_67` | `Generic ASA @BBL H2C 0.4 nozzle` |
| PolyLite LW-PLA | derived from `PolyLite LW-PLA @BBL H2S` | nozzle 200, nozzle1 200, flow 1.05, volspeed 4, Tg 60, density 0.9 _(+8 more)_ | `PMPL30` | `GFSPM02_58` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolyLite PC | derived from `PolyLite PC @BBL H2S` | nozzle 270, flow 0.95, volspeed 8, bed(tex) 105, fan+ 30, Tg 113, density 1.19 _(+10 more)_ | `PMPC01` | `GFSPM02_59` | `Generic PC @BBL H2C 0.4 nozzle` |
| PolyLite PETG | derived from `PolyLite PETG @BBL H2D` | flow 0.95, volspeed 11.5, Tg 70 _(+1 more)_ | `GFG60` | `GFSPM02_60` | `Generic PETG @BBL H2C 0.4 nozzle` |
| PolyLite PETG Translucent | derived from `PolyLite PETG Translucent @BBL P2S` | nozzle 250, nozzle1 250, flow 0.96, volspeed 8, fan- 10, fan+ 30, Tg 81, density 1.25 _(+12 more)_ | `PMPE05` | `GFSPM02_61` | `Generic PETG @BBL H2C 0.4 nozzle` |
| PolyLite PLA-CF | derived from `PolyLite PLA-CF @BBL H2S` | nozzle 230, nozzle1 230, flow 1.04, bed(st) 45, Tg 64, density 1.29 _(+12 more)_ | `PMPL31` | `GFSPM02_62` | `Generic PLA-CF @BBL H2C 0.4 nozzle` |
| PolyMax PC | derived from `PolyMax PC @BBL H2S` | nozzle 260, nozzle1 260, flow 0.96, volspeed 8, bed(tex) 105, fan- 50, Tg 117, density 1.19 _(+10 more)_ | `PMPC02` | `GFSPM02_63` | `Generic PC @BBL H2C 0.4 nozzle` |
| PolyMax PETG | derived from `PolyMax PETG @BBL H2S` | nozzle 260, nozzle1 260, flow 0.96, volspeed 8, bed(tex) 105, fan- 50, Tg 117, density 1.19 _(+10 more)_ | `PMPE09` | `GFSPM02_64` | `Generic PC @BBL H2C 0.4 nozzle` |
| PolySupport | derived from `PolySupport @BBL H2S` | nozzle 230, nozzle1 230, bed(tex) 60, Tg 0, density 1.22 _(+12 more)_ | `PMPL33` | `GFSPM02_65` | `Generic PLA @BBL H2C 0.4 nozzle` |
| PolySupport for PA12 | derived from `PolySupport for PA12 @BBL H2S` | flow 0.97, bed 55, bed(tex) 50, bed(eng) 55, bed(st) 45, chamber 0, fan- 80, fan+ 100, Tg 117, density 1.29 _(+23 more)_ | `PMPA06` | `GFSPM02_66` | `Generic PA @BBL H2C 0.4 nozzle` |

## Not covered (9 of the fork's 77 Polymaker filaments)

| Filament | Why |
| --- | --- |
| `Fiberon PA12-CF`, `Fiberon PA6-CF`, `Fiberon PA6-GF`, `Fiberon PA612-CF`, `Fiberon PET-CF`, `Fiberon PETG-rCF` | Legacy short names in `filament/Polymaker/`, registered only for `@base` + `@BBL H2D`. Polymaker's catalogue calls these `Fiberon PA12-CF10`, `PA6-CF20`, `PA6-GF25`, `PA612-CF15`, `PET-CF17`, `PETG-rCF08` — the top-level entries that **did** get official H2C presets above. Adding H2C to both names would give one physical filament two H2C presets. |
| `Panchroma PLA Stain` | Fork-only name; upstream (and the fork's registered top-level entry) is `Panchroma PLA Satin`, which is covered. Files live in the unregistered subdirectory. |
| `Panchroma PLA Temp Shift` | No upstream product directory at all, and no registered H2D/H2S/P2S sibling to derive from. No source of any kind. |
| `PolySmooth` | PVB. Polymaker publish no H2C preset, and the fork has no `Generic PVB @BBL H2C` parent to derive onto. |

## Validation

### `scripts/orca_extra_profile_check.py --vendor BBL`

Baseline **before** any change: **8 errors**, all pre-existing duplicate
`@base` profile names caused by the unregistered `filament/Polymaker/` and
`filament/eSUN/`, `filament/Overture/` subdirectories:

```
Duplicated profile eSUN PLA+ @base
Duplicated profile Overture Matte PLA @base
Duplicated profile Overture PLA @base
Duplicated profile PolyLite ABS @base
Duplicated profile PolyLite ASA @base
Duplicated profile PolyLite PETG @base
Duplicated profile PolyLite PLA @base
Duplicated profile PolyTerra PLA @base
```

**After** the 68 additions: **the same 8 errors, 0 warnings** — no new error and
no new warning. Those 8 are a separate pre-existing problem in this branch and
were not touched here.

### `Snapmaker_Orca_profile_validator`

Not present in `SnapmakerOrcaPhone\build\src\Release`, so it was built in this
worktree (`build-validator/`, `-DORCA_TOOLS=ON`, reusing the existing dependency
tree read-only). Binary:
`build-validator/src/Release/Snapmaker_Orca_profile_validator.exe`.

**This branch's BBL profiles do not validate at baseline**, before any change
here. Two independent pre-existing problems:

1. `resources/profiles/BBL/process/0.20mm Standard @BBL X2D.json` sets the scalar
   option `enable_overhang_speed` to the non-uniform array `1,1,1,1,0,0`, which
   aborts the load outright. Reproduced identically against a pristine
   `git archive HEAD` copy of `resources/profiles`.
2. With X2D excluded, **1141** BBL presets fail with
   `contains incorrect keys: filament_cooling_before_tower, filament_flush_temp,
   filament_flush_volumetric_speed, which were removed` — options this fork has
   dropped but the profiles still carry. All 1141 are `Bambu *` / `Generic *`
   presets.

So the validator cannot pass on this branch either way, and the meaningful
question is whether the 68 additions make it *worse*. Both trees were run
through the same harness (X2D family excluded so the load reaches the filament
presets) and compared:

| | Baseline (`HEAD`) | With the 68 new presets |
| --- | --- | --- |
| error lines | 1141 | 1141 |
| distinct files named | 1141 | 1141 |
| **files that are new `@BBL H2C` presets** | 0 | **0** |
| files erroring only in this tree | — | **0** |

The two error sets are **identical**. All 68 new presets load, and none is named
in any error.

That comparison caught a real bug in the first attempt. `PresetBundle::
load_vendor_configs_from_json` walks `filament_list` **in order** and fails with
`can not find inherits <parent> for <child>` when a child is listed before its
parent. Registering each new entry purely "beside its siblings" put 66 of the 68
ahead of their `Generic * @BBL H2C 0.4 nozzle` parent. Entries are now inserted
after the **later** of the filament's last sibling entry and the parent's entry,
which keeps them beside their siblings where that is already past the parent
(2 of 68) and after the parent otherwise (66 of 68). Verified: zero children
precede their parent.

### CLI proof slice

Sliced with the live read-only `C:\Dev\SnapmakerOrca\build\Snapmaker_Orca\EdgeSlicer.exe`
copied to a scratch install whose `resources` is a junction to this worktree, an
isolated `--datadir` copied from `snorca_hubtest\dd_ctl`, printer
**`Bambu Lab H2C 0.4 nozzle`**, process `0.20mm Standard @BBL H2C`, filament
**`Polymaker PLA @BBL H2C`** (new), model `resources/handy_models/OrcaToleranceTest.stl`:

```
result.json: "error_string": "Success.", "return_code": 0
plate_1.gcode  641097 bytes, 27077 lines, 2.95 g, 876 s
```

G-code header confirms the new preset was the one used, with its ported values:

```
; filament_settings_id = "Polymaker PLA @BBL H2C"
; printer_settings_id  = Bambu Lab H2C 0.4 nozzle
; print_settings_id    = 0.20mm Standard @BBL H2C
; filament_vendor      = Polymaker
; nozzle_temperature = 220        ; filament_flow_ratio = 0.989
; filament_max_volumetric_speed = 15
; temperature_vitrification = 59  ; hot_plate_temp = 55
; filament_cost = 24.99
M104 S220 T1 ; rise temp in advance
```

Two fixups were needed **in the CLI harness only** — neither touches a filament
value and neither indicates a problem with these presets:

* `prime_tower_brim_width` — every BBL process profile in this branch ships
  `"-1"`, but `PrintConfig.cpp` declares `min = 0` (default `3`). The GUI preset
  path tolerates it; the CLI's range check rejects it. Pre-existing, unrelated.
* `layer_change_gcode` — `Print::validate()`'s relative-E check is guarded by
  `!is_BBL_printer()`, which is false only because the CLI is handed a loose
  flattened config instead of the BBL vendor preset bundle. `G92 E0` was
  prepended for the harness run.

(The CLI's `load_config_file()` calls `load_from_json()` directly and does not
walk `inherits`, so machine/process/filament configs were flattened for the run.
Flattening resolves `inherits` + `include` exactly as the loader does.)

## Not verified

* **No hardware.** Nothing here has been printed on a Bambu Lab H2C. The ported
  values are Polymaker's own published H2C tuning; the derived ones are an
  educated re-parenting, not measured data.
* The **14 derived** presets carry no official H2C tuning. They keep their
  filament's material values and take all H2C-specific plumbing from the H2C
  parent. They should load and print sensibly, but flow ratio, volumetric speed
  and fan curves are H2D/H2S numbers, not H2C-calibrated ones. Replace them if
  and when Polymaker publish H2C presets for those materials.
* Only the **0.4 mm** nozzle is covered, because that is the only H2C variant
  Polymaker publish for. The 0.2 / 0.6 / 0.8 mm H2C machines still have no
  Polymaker presets.
* `PolyMax PETG` is typed `PC` and inherits a PC parent. That is **not** a fork
  bug introduced here: Polymaker's own upstream `PolyMax PETG` presets declare
  `"filament_type": ["PC"]` for H2D, H2S and X2D (only their P2S one says
  `PETG`), and the fork's H2S/P2S siblings mirror it. The derived H2C preset was
  kept consistent with its siblings rather than silently diverging.
* The **8 pre-existing duplicate-`@base`** validator errors and the 259
  unregistered files under `resources/profiles/BBL/filament/Polymaker/` are
  untouched.

## Orphaned presets rescued

Date: 2026-09-06
Branch: `fix/bbl-orphaned-presets` (from `fix/bbl-unregistered-filaments` @ 481452001e)

### The orphaning

`resources/profiles/BBL/filament/` held **425 preset files that no longer
appeared in `BBL.json` → `filament_list`**, so the loader never saw them and no
Bambu machine offered them. All 425 were registered in OrcaSlicer's own
`BBL.json`, still present in this repo's history at **e3d55b3c5b** (959 filament
entries). Fork commit **44f198e1ed "Feature transfer lxy (#130)"** replaced
`BBL.json` wholesale with a Bambu-derived 512-entry list and dropped them; the
files themselves were left on disk.

They cover legacy printers only, and every printer they name is in this fork's
`machine_list`, so those machines simply had no Polymaker / Overture / eSUN /
SUNLU / AliZ / FusRock filaments at all.

### What was re-registered

**All 425 files, 0 deleted, 0 left unregistered.** `filament_list` 2049 → 2474.

| Folder | Files | Per printer |
| --- | --- | --- |
| `filament/Polymaker/` | 247 | A1 48, X1C 48, A1M 44, P1P 44, X1 40, X1E 4, `@base` 19 |
| `filament/Overture/` | 93 | X1C 20, A1 18, A1M 18, P1P 18, X1 18, `@base` 1 |
| `filament/SUNLU/` | 62 | A1 13, A1M 13, X1C 13, P1P 10, X1 6, `@base` 7 |
| `filament/eSUN/` | 7 | A1 2, A1M 2, X1C 2, X1 1 |
| `filament/P1P/` | 6 | P1P 6 |
| `filament/AliZ/` | 5 | `@P1-X1` 5 |
| `filament/FusRock/` | 5 | A1 1, P1P 1, X1C 1, H2D 1, `@base` 1 |

397 are instantiable presets; **28** are abstract `@base` parents
(`instantiation: false`).

Totals by printer across all folders: **X1C 84, A1 82, P1P 79, A1M 77, X1 65,
`@base` 28, AliZ `@P1-X1` 5, X1E 4, H2D 1**.

### Ordering

`PresetBundle::load_vendor_configs_from_json` walks `filament_list` **in order**
and aborts the whole vendor bundle with `can not find inherits <parent> for
<child>` when a child is listed before its parent. Entries were therefore placed
by replaying **e3d55b3c5b's own ordering**: each orphan is inserted after the
nearest preceding e3d55b3c5b entry that still exists in the current list, which
keeps the historical relative order and drops each block beside its siblings.

That left 8 new entries (`PolyLite ABS/ASA @BBL A1 / A1 0.2 nozzle / P1P / X1C`)
ahead of their `PolyLite ABS @base` / `PolyLite ASA @base` parents, and their
own children after them; three repair passes moved 16 entries to just after
their parent. Final state: **zero children before their parent**, and the 2049
pre-existing entries — including the 68 H2C Polymaker ones and the 7 Fiberon
X1C ones — are byte-for-byte unchanged and in their original relative order.

(The 7 pre-existing `Fiberon * @BBL H2D` entries still precede their
`Fiberon * @base` parents. They load only because those `@base` names also exist
in `OrcaFilamentLibrary` and the base bundle rescues them. Pre-existing, not
touched here.)

### Name conflicts: none, so no deletions

The premise was that up to 63 of these names are also registered by Bambu Studio
master at flat `filament/<name>.json` paths in the newer dual-nozzle array form,
in which case the flat file should win and the orphan be deleted.

**No such flat file exists in this tree.** A name sweep over all 2478 files under
`resources/profiles/BBL/filament/` found **zero duplicated preset names**, and
none of the 425 orphan names was already in `filament_list`. The flat
`Polymaker` / `Overture` / `eSUN` files this fork does carry are all `@BBL H2C /
H2D / H2DP / H2S` — the newer printers the orphans do not cover. So the rule
"register the orphan when the flat Bambu file is absent" applies to all 425:
**0 deletions, 0 conflicts, no two entries share a name.**

28 of the newly registered `@base` names *do* also exist under
`resources/profiles/OrcaFilamentLibrary/`. That is not a conflict: they are
`instantiation: false`, so they never enter a `PresetCollection` and
`merge_presets` reports no duplicate (0 "duplicated preset" lines in the
validator, before and after). Their BBL copies differ from the library copies in
27 of 28 cases (`filament_id`, `filament_max_volumetric_speed`,
`slow_down_layer_time`, …), and the loader prefers the vendor's own
`config_maps` over the base bundle, so the rescued BBL children get the BBL
values — which is what they were written against. **No pre-existing registered
preset changes which parent it resolves to.**

### Cross-vendor parents resolve

**69** of the rescued files inherit an `@base` that exists only in
`resources/profiles/OrcaFilamentLibrary/`: 64 Overture (`Overture Air PLA
@base`, `Easy PLA`, `Rock PLA`, `Silk PLA`, `Super PLA+`, `TPU` — 10 each — and
`Overture ASA @base` — 4) and 5 AliZ (`AliZ PA-CF / PETG / PETG-CF /
PETG-Metal / PLA @base`).

They resolve through the base bundle: `load_system_presets_from_json` moves
`OrcaFilamentLibrary` to the front of the vendor list and passes the loaded
library as `base_bundle`, and `parse_subfile` falls back to
`base_bundle->m_config_maps` (and `m_filament_id_maps`) when `inherits` is not in
the vendor's own map. Verified, not assumed: **all 69 load, none re-pointed,
none left unregistered**, and `Overture Silk PLA @BBL A1M` — whose parent is
library-only — slices end to end in the CLI proof below.

### Validation

`scripts/orca_extra_profile_check.py --vendor BBL`

| | Files with errors | Files with warnings |
| --- | --- | --- |
| before | 0 | 0 |
| after | 0 | 0 |

`Snapmaker_Orca_profile_validator` — the binary built for the H2C work
(`.claude/worktrees/h2c-polymaker/build-validator/src/Release/`, used read-only),
pointed at scratch copies of `resources/profiles` with the X2D process family
stripped from `process_list` (16 entries) so the load reaches the filaments, as
before. Both runs still end "Validation failed" for the pre-existing reason.

| | before | after |
| --- | --- | --- |
| `[error]` lines | **1349** | **1349** |
| distinct files named in errors | 1349 | 1349 |
| error lines unique to this tree | — | **0** |
| new presets named in any error | — | **0** |
| `include not found` warnings | 7 | 7 |

(1349, not the 1141 recorded for the H2C work above, because only the X2D
**process** family was stripped here — the 208 X2D *filament* errors are left
in. 1349 − 208 = 1141 exactly, so this is the same error population plus the
X2D filaments, a superset of the earlier harness.)

The two error sets are **identical**; every one is the pre-existing
`contains incorrect keys: filament_cooling_before_tower, filament_flush_temp,
filament_flush_volumetric_speed, which were removed`.

Positive proof, `-l 4`: total `got preset` lines **2351 → 2748, exactly +397** —
the 397 instantiable new presets, with the 28 `instantiation: false` parents
correctly silent (that path returns before the log line). Sample, before = 0
occurrences, after = 1 each:

```
got preset PolyTerra PLA @BBL A1,   from <profiles>/BBL/filament/Polymaker/PolyTerra PLA @BBL A1.json
got preset Overture PLA @BBL A1M,   from <profiles>/BBL/filament/Overture/Overture PLA @BBL A1M.json
got preset eSUN PLA+ @BBL A1M,      from <profiles>/BBL/filament/eSUN/eSUN PLA+ @BBL A1M.json
got preset PolyLite PLA @BBL X1C,   from <profiles>/BBL/filament/Polymaker/PolyLite PLA @BBL X1C.json
got preset SUNLU PLA+ @BBL P1P,     from <profiles>/BBL/filament/SUNLU/SUNLU PLA+ @BBL P1P.json
```

### CLI proof slice

Live read-only `C:\Dev\SnapmakerOrca\build\Snapmaker_Orca\EdgeSlicer.exe` copied
to a scratch install whose `resources` is a junction to this worktree, isolated
empty `--datadir`, model `resources/handy_models/OrcaToleranceTest.stl`, printer
**`Bambu Lab A1 mini 0.4 nozzle`**, process `0.20mm Standard @BBL A1M`. Presets
selected by name (`--printer-preset` / `--process-preset` /
`--filament-presets`), so the real vendor bundle and its base-bundle fallback are
exercised rather than a flattened config.

```
--filament-presets "Overture PLA @BBL A1M"
  result.json: "error_string": "Success.", "return_code": 0
  plate_1.gcode  626024 bytes, 27714 lines, 2.905 g, 955.8 s
  ; filament_settings_id = "Overture PLA @BBL A1M"
  ; printer_settings_id  = Bambu Lab A1 mini 0.4 nozzle
  ; print_settings_id    = 0.20mm Standard @BBL A1M
  ; filament_vendor = Overture   ; filament_type = PLA
  ; fan_max_speed = 80   ; fan_min_speed = 60   ; hot_plate_temp = 60
  ; slow_down_layer_time = 8     ; textured_plate_temp = 65
```

Those six values are exactly what `Overture PLA @BBL A1M.json` overrides, so the
rescued file — not its parent — is what the slice used.

```
--filament-presets "Overture Silk PLA @BBL A1M"     (parent is library-only)
  result.json: "error_string": "Success.", "return_code": 0
  ; filament_settings_id = "Overture Silk PLA @BBL A1M"
  2.795 g, 939.3 s
```

Negative control: the same command against `HEAD`'s `BBL.json` (temporarily
restored, then reverted) fails with `preset not found: Overture PLA @BBL A1M`.

One fixup was needed **in the CLI harness only**, unchanged from the H2C run and
unrelated to any filament value: `--layer-change-gcode "G92 E0"`.
`Print::validate()`'s relative-E check is guarded by `!is_BBL_printer()`, and
`Snapmaker_Orca.cpp` sets `is_BBL_printer()` (line ~5382) *after* it calls
`print->validate()` (line ~5292), so the flag is still false at validation time.
Pre-existing CLI bug. `prime_tower_brim_width` needed no fixup this time,
because the preset-by-name path builds the config from the vendor bundle.

### Version bump

`BBL.json` `"version"` **02.00.00.74 → 02.00.00.75**. Existing installs re-sync
their system profiles only when it rises, so without this the 425 presets would
appear for new data directories only.

### Not verified / left over

* **No hardware.** These are OrcaSlicer's own vendor presets, restored verbatim;
  not one value was edited. Nothing was printed.
* Nothing was **deleted** and nothing was **left unregistered**: 425 of 425
  files are now in `filament_list`.
* The only files still unregistered under `BBL/filament/` are the four
  `fdm_filament_template_direct_*.json` **include templates**, which are pulled
  in by `include` rather than listed, and correctly so.
* **Pre-existing, untouched:** the 7 `Fiberon * @BBL H2D` files carry
  `"include": "fdm_filament_template_direct_dual"`, which the loader resolves
  relative to `BBL/filament/Polymaker/` and cannot find — 7 `include not found`
  warnings, identical before and after this change. None of the 425 rescued
  files uses `include`.
* **Pre-existing, untouched:** the 1349 `incorrect keys` validator errors, and
  `process/0.20mm Standard @BBL X2D.json` setting the scalar
  `enable_overhang_speed` to `1,1,1,1,0,0`, which still aborts a validator run
  that includes the X2D process family.
* Whether Bambu Studio master registers 63 of these names at flat paths was
  **not** checked against upstream — only against this tree, where no such file
  exists.
