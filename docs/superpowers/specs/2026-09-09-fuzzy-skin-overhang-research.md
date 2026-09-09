# Skip fuzzy skin on overhang walls: feasibility research

Research only, branch `research/fuzzy-skin-overhang` off `origin/feat/ultra-preferences`.

## Recommendation

Buildable, effort **M**. Add a bool `fuzzy_skin_skip_overhangs` (default off) in the Fuzzy Skin
group. Implement it as a per-point support test run *inside* the existing fuzz functions
(`fuzzy_polyline` / `fuzzy_extrusion_line` in `src/libslic3r/Feature/FuzzySkin/FuzzySkin.cpp`),
reusing the grown lower-layer polygons the perimeter generator already builds for overhang
detection (`m_lower_polygons_series` / `lower_slices_polygons()`), rather than trying to skip
fuzzing on paths already labeled `erOverhangPerimeter` — fuzz runs *before* that split, so the role
doesn't exist yet at the point we'd need it. The main risk is not correctness but perf (per-point
point-in-polygon tests against the lower slices for every jittered sample) and the Arachne
variable-width path, which needs the same test wired through junction width, not point-in-polygon on
`Point`.

## 1. Where fuzzy skin is applied

Config options `fuzzy_skin`, `fuzzy_skin_thickness`, `fuzzy_skin_point_distance`,
`fuzzy_skin_first_layer`, `fuzzy_skin_noise_type`, `fuzzy_skin_scale`, `fuzzy_skin_octaves`,
`fuzzy_skin_persistence`, `fuzzy_skin_mode` are all defined together in
`src/libslic3r/PrintConfig.cpp:3257-3373`, all `category = "Others"`, GUI group "Fuzzy Skin" in
`src/slic3r/GUI/Tab.cpp:2729-2738` (`others_settings_fuzzy_skin` help anchors). There's also a
"fuzzy skin paint" mechanism: `PerimeterGenerator::regions_by_fuzzify`
(`FuzzySkin.cpp:group_region_by_fuzzify`) buckets the compatible regions of a layer by their fuzzy
config and, when more than one config is present, splits each loop against the per-region
`ExPolygons` via `Algorithm::split_line` so different parts of one loop can get different fuzz
settings (or none). This is a directly reusable pattern for a per-segment "is this part
overhanging" split.

Application sites, `src/libslic3r/PerimeterGenerator.cpp`:
- Classic (round) generator: `apply_fuzzy_skin(loop.polygon, ...)` at **line 235**, producing a
  `Polygon` used for everything downstream.
- Arachne generator: `apply_fuzzy_skin(extrusion, ...)` at **line 498**, mutating
  `Arachne::ExtrusionLine::junctions` in place.

Both calls run inside `should_fuzzify()` gating (loop index / contour-vs-hole / first-layer check,
`FuzzySkin.cpp:227-243`), then delegate to `fuzzy_polyline` (classic, operates on `Points`) or
`fuzzy_extrusion_line` (Arachne, operates on `Arachne::ExtrusionJunctions`, carries per-junction
width `w` so `Extrusion`/`Combined` modes can jitter width instead of position).

**Stage: fuzz runs before overhang detection/splitting in both generators.** Classic:
`apply_fuzzy_skin` at line 235, overhang split (`intersection_pl`/`diff_pl` against
`lower_polygons_series`) starts at line 238. Arachne: `apply_fuzzy_skin` at line 498, overhang clip
(`clip_extrusion` against `lower_slices_paths`) starts at line 502. So the fuzzed geometry is what
gets tested for support, and the overhang/non-overhang boundary is computed on top of already-jittered
points — consistent with the reported curling behavior.

## 2. Overhang classification: what data exists where

No `overhang_degree` field lives on `ExtrusionPath` in this fork; the "overhang buckets"
(`overhang_1_4_speed`..`overhang_4_4_speed`, `PrintConfig.cpp`, consumed in
`GCode.cpp:8415-8448`) are a downstream, per-path-width speed heuristic computed at G-code
emission time from the ratio of the `erOverhangPerimeter` path's extrusion width to
`overhang_flow.width()` — they classify *how much* of a path overhangs, not *where*. The
binary overhang/non-overhang wall split that actually matters for this feature happens earlier,
purely geometrically, in `PerimeterGenerator::process_classic` / `traverse_extrusions`:

- `PerimeterGenerator::lower_slices` (`const ExPolygons*`, header line 69) — this layer's lower
  layer, may be `nullptr` (first non-raft layer, or lower_slices otherwise unavailable).
- `m_lower_slices_polygons` / `lower_slices_polygons()` — `offset(*lower_slices, nozzle_diameter/2)`,
  computed once per generator instance (`PerimeterGenerator.cpp:1408, 2370`). Used directly by the
  Arachne path's `clip_extrusion`.
- `m_lower_polygons_series` / `m_external_lower_polygons_series` /
  `m_smaller_external_lower_polygons_series` — built by `generate_lower_polygons_series(width)`
  (`PerimeterGenerator.cpp:2812-2835`): two offsets of `lower_slices`, `start_offset = -0.5*width`
  and `end_offset = +0.5*nozzle_diameter`, i.e. **exactly** the "-0.5\*ext_perimeter_width" tolerance
  named in the brief is already the offset this fork uses for the "is this point actually
  supported" test (index 0; `.back()` uses the more generous nozzle-radius offset, and classic uses
  `.back()` at line 264 for the non-overhang `intersection_pl`).
- `erOverhangPerimeter` role and `ExtrusionPath` produced from `diff_pl(polygon, lower_polygons)`
  (classic) / `diff` against `lower_slices_paths` (Arachne) — assigned strictly after the fuzz call,
  so nothing upstream of fuzzing currently carries an overhang flag.

Because fuzzing precedes this split, **the option must run its own support test inside the fuzz
pass** (per the brief's option B), not filter already-tagged `erOverhangPerimeter` paths. The good
news: the exact offset polygons needed (`m_lower_polygons_series` family, or a fresh
`offset(*lower_slices, -0.5*width)` for symmetry with classic's own tolerance) are already computed
per `PerimeterGenerator` instance and available at the call sites (lines 235 and 498), just not
threaded into `apply_fuzzy_skin`'s signature yet.

## 3. Design

New option, Fuzzy Skin group, `src/libslic3r/PrintConfig.cpp` near line 3373 (after
`fuzzy_skin_persistence`) and `src/slic3r/GUI/Tab.cpp:2738`:

```cpp
def = this->add("fuzzy_skin_skip_overhangs", coBool);
def->label = L("Skip fuzzy skin on overhangs");
def->category = L("Others");
def->tooltip = L("Do not apply fuzzy skin to the parts of a wall that are unsupported by the "
                  "layer below (overhang walls and bridging perimeters). Keeps overhangs clean "
                  "since fuzzy skin's sideways jitter otherwise extrudes into open air there.");
def->mode = comAdvanced;
def->set_default_value(new ConfigOptionBool(false));
```

No new threshold option — reuse the wall's own support test (same offset already used to decide
`erOverhangPerimeter` vs normal, `-0.5 * extrusion_width`) rather than inventing a second knob tied
to the unrelated `overhang_1_4_speed..4_4_speed` fan/speed buckets, which classify degree-of-overhang
for cooling/speed, not "is this point sitting on anything." Keeping one definition of "overhang"
avoids the two mechanisms disagreeing at their boundary.

**Hook.** Extend `FuzzySkinConfig` (`PerimeterGenerator.hpp`, alongside `fuzzy_first_layer` etc.,
populated in `group_region_by_fuzzify`) with `bool skip_overhangs` and a `const Polygons*
lower_slices_offset` (or capture it by reference from the `PerimeterGenerator`, matching how
`slice_z` is already threaded through). Both `apply_fuzzy_skin` overloads already receive the full
`PerimeterGenerator&`, so the lower-slices data doesn't need new plumbing at the call site — only
inside `FuzzySkin.cpp`.

**Algorithm**, applied inside `fuzzy_polyline` / `fuzzy_extrusion_line` at the point each new sample
`pa` is generated (line 94 classic, line 141 Arachne):
1. If `!skip_overhangs` or `lower_slices` is null (no lower layer / first printed layer — first
   layer is bed-supported and never an overhang; `should_fuzzify` already gates first-layer fuzz
   via `fuzzy_first_layer`), behave exactly as today.
2. Test `pa` against the offset lower-slices polygon (`ExPolygon::contains(Point)` or
   `Polygons`-based point-in-polygon on the `-0.5*width` growth already computed): supported → jitter
   as today; unsupported → keep `pa` at its un-displaced position (`r = 0`, or for Arachne, `w`
   unchanged) instead of discarding the point — the sampling cadence must stay the same so segment
   length statistics don't change.
3. **Blend, don't hard-cut.** Track the support state of the previous sample; on a
   supported→unsupported (or reverse) transition, ramp the displacement `r` (classic) or the width
   delta (Arachne) linearly to zero over the next 1-2 samples (`~1-2 * point_distance`) rather than
   snapping, so there's no visible step/gap at the boundary. This mirrors the segment-splitting
   pattern `apply_fuzzy_skin` already uses for multi-region paint (`fuzzy_current_segment` /
   `split_line`), just at finer (per-sample) granularity instead of per-segment, because a full
   segment split would itself reintroduce a hard seam unless additionally feathered.
4. **Bridging perimeters**: `erOverhangPerimeter` spanning paths are a strict subset of "unsupported
   by lower layer" — the same point-in-polygon test against `lower_slices` already answers this, no
   separate bridge check needed. A bridging perimeter is unsupported along its whole length, so it
   fuzzes to nothing (all points pass through undisplaced) automatically.
5. **Inner walls**: correctly never flagged as overhangs by this test unless the lower layer is
   genuinely missing/absent under them (matches the brief) — the test is purely geometric containment,
   not wall-index-based, so it's correct for `fuzzy_skin = allwalls` too.
6. **Generators**: implement once in `FuzzySkin.cpp`, called from both `fuzzy_polyline` (classic
   `Points`) and `fuzzy_extrusion_line` (Arachne `ExtrusionJunctions`) — the two already share
   the same noise-sampling structure, so the support test slots into both loops identically. Arachne's
   variable width per junction (`ExtrusionJunction::w`) means "unsupported" must zero the
   *position* delta even in `Extrusion`/`Combined` mode (which jitters width, not position) — an
   unsupported point should not balloon or starve extrusion width either, since that changes the
   nozzle's effective reach over open air just as much as lateral displacement does.

## 4. Preview / G-code / seams

`erOverhangPerimeter` role assignment and speed happen after fuzzing regardless of this option — the
new option changes *only* the (x,y) (and, in Extrusion/Combined mode, width) of the jittered points
that later fall inside the overhang diff; it doesn't touch role assignment, flow, or speed
computation at all, so overhang speed buckets and preview coloring by role are unaffected. Nothing
else keys off "was this point fuzzed" — fuzzing is destructive (positions/widths are overwritten in
place before role/flow assignment), so there's no separate flag to update elsewhere. Seams: seam
placement (`SeamPlacer`) picks a start point on the loop before/independent of `fuzzy_polyline`
displacement (no `fuzzy` references found in `SeamPlacer.cpp`); since unsupported points are now
*closer* to their un-fuzzed position, if the seam happens to land in a skipped region the seam point
itself is unaffected either way (fuzzing already preserves it via the closed-loop endpoint handling
in `fuzzy_polyline`/`fuzzy_extrusion_line`).

## 5. Upstream comparison

No released OrcaSlicer or BambuStudio version has this option as of 2026-09-09. Two open,
unresolved requests confirm it's a known gap, not a shipped feature:
- [OrcaSlicer#9137 "Avoid Fuzzy skin on bridges"](https://github.com/SoftFever/OrcaSlicer/issues/9137)
  — open issue asking to skip fuzzy skin specifically on bridging perimeters.
- [OrcaSlicer#10615 "Fuzzy Skin Over-extrusion at certain overhang angles"](https://github.com/OrcaSlicer/OrcaSlicer/issues/10615)
  — reports "over-extrusion... between 25-0 degrees" of overhang and requests "enable fuzzy skins
  only on certain levels of overhang."
- [OrcaSlicer#4751 (BambuStudio) "Fuzzy Skin does not work with reduced speed on overhangs"](https://github.com/bambulab/BambuStudio/issues/4751)
  — separate but related: overhang speed reduction gets overridden when fuzzy skin is on, same root
  cause (fuzz runs before/independent of the overhang-aware path split).

No PR closes any of the three. This fork would be first to ship it.

## 6. Effort and risks

**Effort: M.** Config option + tooltip + GUI line: trivial. Core logic: moderate — touches the
shared noise-sampling loops in both `fuzzy_polyline` and `fuzzy_extrusion_line`, needs the blend
logic to avoid visible seams, and needs `lower_slices`/offset polygons threaded into
`FuzzySkinConfig` or the call sites. Not L because no new data structures are needed (the offset
polygons already exist) and no G-code/role-assignment code changes.

Risks:
- **Arachne variable width**: displacement and width-jitter are coupled in `Extrusion`/`Combined`
  mode (`FuzzySkin.cpp:143-154`); zeroing only position but not width for unsupported points in
  those modes needs care so extrusion width doesn't drift where position is being held down to zero.
- **Perf**: point-in-polygon per sample, per loop, is O(loop points × lower-slice edges) if done
  naively. Mitigate by reusing the already-offset `m_lower_polygons_series`/`m_lower_slices_polygons`
  cache (`ExPolygon`/`Polygons` built once per `PerimeterGenerator`, not per point) and, if profiling
  shows a hotspot, adding an `AABBTreeIndirect` or per-layer coarse bounding-box pre-filter before
  the exact contains test (the same pattern `ClipperUtils::clip_clipper_polygons_with_subject_bbox`
  already uses to bound the lower-slices set to the loop's bbox before the exact boolean op).
- **Tiny islands / degenerate loops**: `fuzzy_polyline`/`fuzzy_extrusion_line` already have a
  `while (out.size() < 3)` fallback for over-aggressive resampling; the same guard covers the case
  where "mostly unsupported" collapses a loop to too few points.
- **Blend window vs. point_distance**: at very small `fuzzy_skin_point_distance` or very short
  overhang runs, a 1-2 sample blend could span the whole unsupported segment, which is fine
  (degrades gracefully to no visible ramp) but should be covered by the unit test's short-overhang
  case.

## 7. Test plan

**Unit test**, `tests/fff_print/test_fuzzy_skin_overhangs.cpp` (new; follow the shape of
`tests/fff_print/test_over_support_surfaces.cpp`, same author area / similar geometric-overhang
setup), added to `tests/fff_print/CMakeLists.txt`'s file list:
- Model: a 45°+ cantilever or inverted-pyramid test solid (small, hand-built via existing
  `test_data.hpp` mesh helpers or an STL under `tests/fff_print/` — check for an existing overhang
  fixture before adding a new mesh file).
- Case A (option on): slice with `fuzzy_skin = allwalls`, `fuzzy_skin_skip_overhangs = true`.
  Extract the generated `ExtrusionPath`/`ExtrusionLoop` points for a layer with a known overhang
  region; assert points whose un-fuzzed position falls outside `lower_slices` (grown by the same
  tolerance) are unchanged (or within the blend-ramp band) versus a `fuzzy_skin = none` slice of the
  same layer, and points inside the supported region are displaced (differ from the `none` slice by
  something on the order of `fuzzy_skin_thickness`).
- Case B (option off, default): slice the same model with `fuzzy_skin_skip_overhangs` at its default
  (false) both before and after this change lands; assert the resulting G-code is byte-identical —
  this is the regression guard that the new code path is fully inert when the option is off. (Cheap
  to do exactly like the "determinism" fixtures already in `tests/*.gcode` at the repo root per
  `git status`.)
- Cover both classic and Arachne generators (toggle whichever config selects Arachne vs classic
  perimeter generation in this fork), and both `external` and `allwalls` fuzzy modes, since inner
  walls should only get skipped when genuinely unsupported.

**Owner print test** (manual, hardware): slice a small single-wall overhang tower (e.g. 45-60°
single-wall cone or the classic overhang-angle test) once with `fuzzy_skin_skip_overhangs = false`
and once `= true`, same fuzzy settings otherwise; compare the printed overhang region for curling/
stringing with the option on vs. off, and confirm the fuzzed (supported) region still looks
textured on both.
