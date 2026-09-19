# Process settings (Edge overview)

## What it is

EdgeSlicer keeps the usual Orca / Snapmaker Orca **process** (print) settings and adds or extends several options — especially under quality, support, and multimaterial.

This page is the **Edge-focused map**: what we changed or added, where it lives, and when to use it. For stock settings that match upstream, use the [OrcaSlicer Wiki](https://www.orcaslicer.com/wiki/) (Quality, Strength, Speed, Support, Multimaterial, Others).

Dedicated Edge pages (when they exist) go deeper than the short notes below.

## Where to find it

- Process / print preset tabs in the sidebar (Quality, Strength, Speed, Support, Multimaterial, Others) — same layout as Orca, with Edge rows mixed in
- Some multimaterial filament-for-feature options (outer wall filament, Support Filament Matching, brim nearest wall)
- Screenshot for several quality extras: `docs/images/print-quality.png`

## How to use it (Edge highlights)

### Quality

| Setting | What it does | Notes |
|---|---|---|
| **[Offset layers](Offset-layers)** (experimental) | Odd-numbered walls shift by half a layer height so layers interlock (classic and Arachne). | Needs first layer height = layer height, matching top-surface and outer-wall line widths, spiral off. Slicer can offer to fix those when you enable it. |
| **[Z contouring](Z-contouring)** | Smooths staircasing on shallow tops by shaping each layer to the model. | Speed scaling keeps flow steady; works with layer cooling. Off by default; tune angle and minimum height per profile. |
| **[Locked Zag infill](Locked-Zag-infill)** | Skin and skeleton can each take their own infill pattern (as in Bambu Studio); *skin follows the surface* hugs sloped tops. | Existing profiles unchanged until you opt in; Bambu profiles that set these keys import as-is. |
| **[Fuzzy skin](Fuzzy-skin)** (Edge notes) | Overhanging wall segments can stay unfuzzed; a width floor tied to layer height avoids aborting Extrusion / Combined modes. | Prefer when fuzzy was pushing extrusion into open air or failing the slice. |
| **Seam position** | Adds **Left** and **Right** as well as Back. | Same seam UI; more compass options. |

### Strength / walls / surfaces

| Setting | What it does | Notes |
|---|---|---|
| **Print unsupported walls last** | Prints unsupported walls after supported ones. | Ported from Orca; useful when unsupported walls benefit from waiting. |
| **Undertop surface pattern** | Pattern control for the surface under the top shell. | Ported from Orca. |

### Support (Edge)

| Setting | What it does | Notes |
|---|---|---|
| **[Support sets and groups](Support-sets-and-groups)** | Named support recipes; per-part interface groups. | Edge-only. |
| **[Over-support surfaces](Over-support-surfaces)** | Bottoms/walls over support use wall-like flow/speed instead of bridge. | Off by default; per part. |
| **Z overrides X/Y** | Support option ported from Bambu Studio. | See Support advanced options. |
| **[Support Filament Matching](Support-Filament-Matching)** | Supports/interfaces/ironing/brims follow the colour they touch. | Opt-in; disabled for an object when a support group sets its own interface filament. |

### Multimaterial / filaments

| Setting | What it does | Notes |
|---|---|---|
| **Outer wall filament** | Separate filament for outer walls vs inner walls. | See [Outer wall filament](Outer-wall-filament). |
| **Brim filament → Nearest wall** | Each brim uses the filament of the wall it touches. | Covered under Support Filament Matching page. |
| **Paint depth** | Bound painted multi-material claims by wall count or distance. | See [Paint depth](Paint-depth). |
| **Per-filament Z offset** | Z offset per filament. | Ported from Orca. |
| **Create filament** dialog | Sorted and easier to scan. | UX only. |

### Speed / estimates / determinism

| Setting | What it does | Notes |
|---|---|---|
| **Machine prepare time** | Included in time estimates. | Ported from Orca. |
| **Deterministic slicing** | Slice lines sorted after the parallel pass — same project → same G-code at any thread count. | Internal bridges over Hilbert / Octagram; wipe tower footprint/placement/preview match what is printed (CLI too). |

### Workflow tools that compare process changes

| Tool | What it does | Notes |
|---|---|---|
| **[Compare Slices](Compare-Slices)** | Diff settings, time, filament, and per-layer toolpaths between two slices. | View menu. |

## Limits / notes

| Situation | What happens |
|---|---|
| Stock Orca setting, unchanged | Documented on the [OrcaSlicer Wiki](https://www.orcaslicer.com/wiki/) — this page does not duplicate it. |
| Offset layers prerequisites | Feature asks you to fix first-layer / line-width / spiral conflicts when enabled. |
| Locked Zag | Opt-in; old profiles keep prior behaviour until you change them. |
| Hardware validation | Some experimental quality features (for example Offset layers) should be checked on a first print. |
| Support base vs groups | Support *base* geometry is never per group — see Support sets and groups. |

When in doubt: change one Edge toggle at a time and use **Compare Slices** before committing a profile.

## Related

- [Home](Home) — full Edge wiki index  
- [Over-support surfaces](Over-support-surfaces)  
- [Support sets and groups](Support-sets-and-groups)  
- [Support Filament Matching](Support-Filament-Matching)  
- [Compare Slices](Compare-Slices)  
- [Preferences → Extras](Preferences-Extras)  
- [OrcaSlicer Wiki](https://www.orcaslicer.com/wiki/) — upstream process settings  
