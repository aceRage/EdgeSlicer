# Locked Zag: separate skin/skeleton infill patterns — Research Spec

Date: 2026-09-10 · Branch: feat/ultra-preferences (research only, no code changes)
Trees: this fork `C:\Dev\SnapmakerOrcaPhone` vs upstream reference `C:\Dev\BambuStudio`.
Both AGPL-3.0 forks of the same lineage.

## 1. Bambu Studio: what it has

Locked Zag splits the sparse-infill surface into a skin band (near the outer contour) and
a skeleton interior, each with its **own fill pattern**, density, depth, and line width.

Config, `PrintConfig.hpp`: `InfillPattern` enum incl. `ipLockedZag` hpp:90. `PrintRegionConfig`
block: `symmetric_infill_y_axis` hpp:1082, `skeleton_infill_density` hpp:1087,
`skin_infill_density` hpp:1088, `infill_lock_depth` hpp:1091, `skin_infill_depth` hpp:1092,
**`locked_skin_infill_pattern`** hpp:1094, **`locked_skeleton_infill_pattern`** hpp:1095,
`skin_infill_line_width` hpp:1108, `skeleton_infill_line_width` hpp:1109 (coFloat, not FloatOrPercent).

Definitions, `PrintConfig.cpp`: `"lockedzag"` enum value cpp:3127, key-map cpp:242.
**`locked_skin_infill_pattern`** (coEnum, 17 sub-patterns, default `ipCrossZag`) cpp:3153-3192.
**`locked_skeleton_infill_pattern`** (same set, default `ipZigZag`) cpp:3194-3233.
`skeleton_infill_density`/`skin_infill_density` (coPercent, default 15%) cpp:3789-3811.
`skin_infill_depth` (coFloat mm, default 2.0) cpp:3813-3821. `infill_lock_depth` (coFloat mm,
default 1.0, "overlap depth between interior and skin") cpp:3823-3831. `skin_infill_line_width`/
`skeleton_infill_line_width` (coFloat mm, default 0.4) cpp:3833-3849. `symmetric_infill_y_axis`
(coBool) cpp:3851-3857. Line-width sanity list includes both cpp:9618-9619.

UI, `Tab.cpp` Strength→"Sparse infill" optgroup cpp:3306-3324: after `sparse_infill_pattern`,
appends `locked_skin_infill_pattern` (3310), `skin_infill_density` (3311),
`locked_skeleton_infill_pattern` (3312), `skeleton_infill_density` (3313), `infill_lock_depth`
(3314), `skin_infill_depth` (3315), `skin_infill_line_width` (3316), `skeleton_infill_line_width`
(3317) — all `is_line_indented=true`. Then `symmetric_infill_y_axis` (3319).

Toggles, `ConfigManipulation.cpp`: `is_locked_zig` CM.cpp:892; gates
`infill_instead_top_bottom_surfaces`, both densities, both depths, both widths, **both pattern
options** CM.cpp:894-895. `symmetric_infill_y_axis` shown for zigzag/crosszag/lockedzag CM.cpp:901.
Validation: `infill_lock_depth` clamped ≤ `skin_infill_depth/2` CM.cpp:745-755.

Slicing — inline in `Fill/FillRectilinear.hpp:164-187`: `FillLockedZag` carries
`LockRegionParam lock_param` and **`InfillPattern skin_pattern`, `skeleton_pattern` members**
(hpp:170-171) with a real `set_skin_and_skeleton_pattern()` override (hpp:183-186).
`LockRegionParam` (`Fill/FillBase.hpp:38-49`): `skin_density_params`, `skin_depths_params`,
`locked_depths_params`, `outlook`, `skeleton_density_params`, `skin_flow_params`,
`skeleton_flow_params`. `SurfaceFillParams` (Fill.cpp) carries `skin_pattern`/`skeleton_pattern`
cpp:30-31, compared cpp:115-116,145-146; populated from `region_config.locked_skin_infill_pattern`/
`locked_skeleton_infill_pattern` when `pattern==ipLockedZag` cpp:214-217; flows/densities/depths
cpp:307-328. `Layer::set_outlook_range()` builds `outlook` from non-`stInternal` surfaces when
`infill_instead_top_bottom_surfaces` is set, Fill.cpp:581-596 (skin hugs the outer contour, not
just an offset band). Dispatch in `make_fills()`: `f->set_lock_region_param(lock_param)` **and**
`f->set_skin_and_skeleton_pattern(...)` Fill.cpp:724-727. Core algorithm
`FillRectilinear.cpp:3506-3541`: `get_skin_and_skeleton_area()` (3414-3430) splits by depth/outlook;
`generate_skeleton_pattern()` (3432-3476) **instantiates `Fill::new_from_type(this->skeleton_pattern)`**
(3435) and calls its `fill_surface()` per density band; `generate_skin_pattern()` (3478-3504)
**instantiates `Fill::new_from_type(this->skin_pattern)`** (3481) likewise; results re-split by
per-band `Flow` (3521-3540). Factory `FillBase.cpp:63`.

## 2. This tree: what it has

Depth-based skin/skeleton split and dual line-width machinery are ported, but **both bands
are hardcoded to the same fill algorithm** — no per-band pattern choice exists.

Config, `PrintConfig.hpp`: `ipLockedZag` hpp:70. `PrintRegionConfig`: `symmetric_infill_y_axis`
hpp:1151, `skeleton_infill_density` hpp:1177, `skin_infill_density` hpp:1178,
`infill_lock_depth` hpp:1179, `skin_infill_depth` hpp:1180, `skin_infill_line_width` hpp:1181
(**ConfigOptionFloatOrPercent**, not coFloat), `skeleton_infill_line_width` hpp:1182 (same).
**Missing**: no `locked_skin_infill_pattern`/`locked_skeleton_infill_pattern` anywhere (zero
hits in PrintConfig.*, Tab.cpp, ConfigManipulation.cpp, Fill/*).

Definitions, `PrintConfig.cpp`: `skeleton_infill_density`/`skin_infill_density` (coPercent,
default **25%**, differs from Bambu's 15%) cpp:3912-3936. `skin_infill_depth`/`infill_lock_depth`
(coFloat mm, 2.0/1.0) cpp:3938-3956 — matches Bambu. `skin_infill_line_width`/
`skeleton_infill_line_width` (**coFloatOrPercent**, `ratio_over=nozzle_diameter`, default 100%)
cpp:3958-3976 — Orca-wide convention diverges from Bambu's absolute coFloat.
`symmetric_infill_y_axis` cpp:3978-3984. Sanity list cpp:9006-9007.

UI, `Tab.cpp` Strength→"Infill" optgroup (named "Infill" not "Sparse infill", Tab.cpp:2421):
cpp:2427-2433 appends `skin_infill_density`, `skeleton_infill_density`, `infill_lock_depth`,
`skin_infill_depth`, `skin_infill_line_width`, `skeleton_infill_line_width`,
`symmetric_infill_y_axis` — **no pattern-choice rows**; this tree's
`append_single_option_line` doesn't use an indentation arg. `is_locked_zig` check exists
Tab.cpp:1761.

Toggles, `ConfigManipulation.cpp`: `is_locked_zig` CM.cpp:620; gates both densities, both
depths, both widths CM.cpp:624-625 (no pattern rows to gate). Same lock-depth clamp CM.cpp:478-485.
`symmetric_infill_y_axis` for zigzag/crosszag/lockedzag CM.cpp:629.

Slicing — `ipLockedZag` **does slice**, mechanism ported but pattern-blind:
`FillParams` (`Fill/FillBase.hpp:99-105`) carries `locked_zag`, `infill_lock_depth`,
`skin_infill_depth` as scalar floats directly (architecturally different from Bambu, which
routes depths only through `LockRegionParam`). `FillLockedZag`
(`Fill/FillRectilinear.hpp:209-223`) has **no `skin_pattern`/`skeleton_pattern` members and no
override** of the base no-op `set_skin_and_skeleton_pattern(...){}` (`FillBase.hpp:174`,
declared but never overridden or called). `LockRegionParam` keeps `skin_density_params`,
`skeleton_density_params`, `skin_flow_params`, `skeleton_flow_params` but **no `outlook`, no
`skin_depths_params`, no `locked_depths_params`** — Bambu's contour-hugging skin
(`infill_instead_top_bottom_surfaces` + `set_outlook_range`) was never ported; no
`set_outlook_range`/`outlook` symbol exists anywhere in this tree. `group_fills()`
(Fill.cpp:1234-1393) sets `params.infill_lock_depth`/`skin_infill_depth` (1251-1254) and the
flow/density maps (1375-1393) but reads no pattern option, because none exists.
`FillLockedZag::fill_surface_locked_zag` (`Fill/FillRectilinear.cpp:3491-3559`): splits by
fixed offset only (`offset_ex(surface->expolygon, -offset_threshold)`, cpp:3505 — no outlook).
Skeleton band: `this->fill_surface(&zig_surface, zig_params)` cpp:3532 — **calls its own
base-class fill_surface**, i.e. whatever `FillLockedZag`/`FillRectilinear::fill_surface`
produces. Skin band: `this->fill_surface(&cross_surface, cross_params)` cpp:3552 — **same
call, same effective pattern as skeleton**. This is the entire gap: only density/flow/width
differ between bands today. Flow re-split by `skin_flow_params`/`skeleton_flow_params`
(cpp:3539,3558) *is* ported faithfully. Factory `FillBase.cpp:74` matches Bambu. Anchoring
switch `case ipLockedZag: break;` Fill.cpp:1880 matches.

**Locked Zag slicing exists and functions (density, depth, lock overlap, independent line
widths all work) but cannot vary fill pattern per band; the outlook refinement is also absent.**

## 3. Gap analysis and port plan

New options (reuse Bambu naming, no Orca conflict):

| Key | Type | Default |
|---|---|---|
| `locked_skin_infill_pattern` | `coEnum<InfillPattern>` | `ipCrossZag` |
| `locked_skeleton_infill_pattern` | `coEnum<InfillPattern>` | `ipZigZag` |

Add to `PrintRegionConfig` block near hpp:1177-1178; define in PrintConfig.cpp near the
`"lockedzag"` registration (~cpp:2859), mirroring Bambu cpp:3153-3233 in spirit — **do not
copy `enum_values.push_back` calls verbatim**; verify each string exists in this tree's own
`s_keys_map_InfillPattern` first (naming for lattice/hatch patterns already diverges between
trees). Est. **+45 lines** PrintConfig.cpp, **+2** PrintConfig.hpp.

Tab.cpp: insert two option lines into the "Infill" optgroup (Tab.cpp:2421 block) right after
`sparse_infill_pattern` (2424) and before `skin_infill_density` (2427), so order reads
pattern → skin pattern → skin density → skeleton pattern → skeleton density → depths →
widths → symmetric axis. **+2 lines**.

ConfigManipulation.cpp: extend the `is_locked_zig`-gated list at CM.cpp:624 with the two new
keys. **+1 line changed**.

Slicing changes (the real work, phase 2):
1. Add `skin_pattern`/`skeleton_pattern` members + real `set_skin_and_skeleton_pattern()`
   override to `FillLockedZag` (`FillRectilinear.hpp:209-223`), mirroring Bambu hpp:170-171,183-186. **+8 lines**.
2. Add matching fields to this tree's `SurfaceFillParams` equivalent in Fill.cpp (near
   Fill.cpp:1246), populate from `region_config.locked_skin_infill_pattern`/
   `locked_skeleton_infill_pattern` alongside the existing depth reads (Fill.cpp:1251-1254). **+6 lines**.
3. In `make_fills()`'s `ipLockedZag` branch (Fill.cpp:1718-1722), add
   `f->set_skin_and_skeleton_pattern(...)` alongside the existing `set_lock_region_param`
   call, mirroring Bambu Fill.cpp:727. **+1 line**.
4. Rewrite `fill_surface_locked_zag` (`FillRectilinear.cpp:3491-3559`) so skeleton
   instantiates `Fill::new_from_type(this->skeleton_pattern)` and skin instantiates
   `Fill::new_from_type(this->skin_pattern)` (mirror Bambu's split, its
   `FillRectilinear.cpp:3432-3504`), using `copy_fill_data()` (present on `Fill` base in both
   trees) instead of two `this->fill_surface()` calls. Outlook/contour-hugging deferred to 2b.
   **~70-90 lines changed**.
5. Phase 2b (optional): port `outlook`, `skin_depths_params`, `locked_depths_params` into
   `LockRegionParam` and a `Layer::set_outlook_range()` mirroring Bambu Fill.cpp:581-596,
   called from `make_fills()` (Bambu Fill.cpp:607). **+25 lines**.

**Total**: phase 1 (options+UI, inert) ≈ **50 lines**, 4 files — compiles but has no slicing
effect until phase 2a lands; recommend shipping 1+2a together, not 1 alone. Phase 2a ≈ **+85
lines**, 3 files (`FillRectilinear.hpp/.cpp`, `Fill.cpp`). Phase 2b (stretch) ≈ **+25 lines**,
2 files. Grand total 1+2a ≈ **135 lines**, 6 files.

**3MF/preset compatibility**: both keys are additive coEnum with concrete defaults; missing
keys on load fall back to `set_default_value` per normal `ConfigBase` behavior — no migration
needed (matches how `skin_infill_density` etc. were added previously with no `handle_legacy`
entry). Non-Locked-Zag profiles are provably unaffected: `group_fills()` only reads the new
fields inside the existing `if (params.pattern == ipLockedZag)` guard (Fill.cpp:1251), so
G-code identity holds structurally. **Bambu import**: string enum keys (`"crosszag"`,
`"zigzag"`, etc.) already match between trees' `s_keys_map_InfillPattern`, so a Bambu 3MF/preset
carrying these keys imports correctly once the option definitions exist here — no shim needed.

**Licence/attribution**: both AGPL-3.0, so verbatim copying is legally permitted, but this
tree's convention (e.g. the existing `coFloatOrPercent` vs Bambu's `coFloat` line-width
divergence) is to re-implement from understanding, not copy-paste — a literal copy of
`FillRectilinear.cpp:3432-3504` wouldn't compile against this tree's diverged
`LockRegionParam`/`FillParams` shapes anyway. Use fresh tooltip wording, keep Bambu-compatible
enum string keys for profile portability, cite this spec + Bambu file:line refs in the commit.

## 4. Test plan

1. **libslic3r unit test**: slice a 20mm cube, `sparse_infill_pattern=lockedzag`, `density=15%`,
   once with skin=grid/skeleton=zigzag, once with both equal. Assert (a) skin-band polyline
   geometry differs measurably between runs (angle/direction signature), (b) skin vs skeleton
   extrusions keep distinct flow/line-width regardless of pattern. Exercises the 2a rewrite.
2. **G-code identity check**: slice a profile at a non-Locked-Zag pattern before/after —
   assert byte-identical G-code (holds structurally, guarded by `ipLockedZag` check above).
   For Locked-Zag at new-option defaults, geometry may change from pre-patch output (today
   both bands resolve via `this->fill_surface()`); document as expected, don't assert identity.
3. **Owner click-tests**: pattern rows shown only when sparse pattern = Locked Zag; skin=Grid/
   skeleton=Gyroid slices with visually distinct bands; pre-change profile/.3mf loads with no
   crash; Bambu .3mf with these keys imports correctly.

## 5. Phasing

- **Phase 1**: add the two enum options + Tab.cpp/ConfigManipulation.cpp wiring. Ship only
  bundled with 2a — shipping alone exposes UI that does nothing.
- **Phase 2a** (required): give `FillLockedZag` real pattern members, thread them from
  `SurfaceFillParams` through `make_fills()`, rewrite `fill_surface_locked_zag` to
  instantiate two independent `Fill` sub-fillers instead of calling `this->fill_surface()`
  twice. This is the actual gap.
- **Phase 2b** (optional/stretch): port `outlook`/`set_outlook_range` contour-hugging skin
  (tied to `infill_instead_top_bottom_surfaces`) if the owner wants skin to follow the
  model's outer surface rather than a uniform depth-offset band. Not required for basic
  per-band pattern control.
