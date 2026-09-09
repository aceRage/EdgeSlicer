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

## 8. Implemented

Branch `feat/fuzzy-skin-skip-overhangs`, off `origin/feat/ultra-preferences` at 1afca41fab. The
option shipped as researched above: one bool, no new threshold, the support test run inside the
fuzz pass against the polygons the overhang split already builds.

### Hook points

| What | Where |
| --- | --- |
| Option definition | `src/libslic3r/PrintConfig.cpp`, after `fuzzy_skin_persistence` |
| Config member | `src/libslic3r/PrintConfig.hpp`, `PrintRegionConfig` |
| GUI line | `src/slic3r/GUI/Tab.cpp`, Fuzzy Skin optgroup, after `fuzzy_skin_first_layer` |
| GUI dependency | `src/slic3r/GUI/ConfigManipulation.cpp` - shown only when `fuzzy_skin != none` **and** `detect_overhang_wall` is on, since it reuses that detection's polygons |
| Fuzz config | `FuzzySkinConfig::skip_overhangs` (`PerimeterGenerator.hpp`), in `operator==` and the hash, populated in `group_region_by_fuzzify` |
| Support region | `fuzzy_skip_overhangs_wanted()` / `fuzzy_support_region()` in `FuzzySkin.cpp` |
| Classic fuzz | `fuzzy_polyline(..., const Polygons* support)` |
| Arachne fuzz | `fuzzy_extrusion_line(..., const Polygons* support)` |

The support region is what section 2 predicted, per generator:

* **Classic** - `m_lower_polygons_series` / `m_external_lower_polygons_series` index 0: the lower
  slices offset by `-0.5*width + 0.5*(end-start)/(overhang_sampling_number-1)`, about -0.17 mm on a
  0.42 mm outer wall over a 0.4 nozzle. That is the same set `traverse_loops` clips the loop against
  to decide `erOverhangPerimeter`, so the fuzz boundary and the role boundary are one boundary.
* **Arachne** - `process_arachne` never fills the width-indexed series (only `process_classic`
  does), so the Arachne overload falls back to `lower_slices_polygons()`, the `+nozzle/2` growth its
  own `clip_extrusion` overhang split uses. Same principle, one definition of "overhang" per
  generator, shared with the role assignment.

`fuzzy_skip_overhangs_wanted()` is the single gate. It returns false - so the fuzz functions get a
null `support` and take their original code path verbatim - when the option is off for every
fuzzified region, when `lower_slices == nullptr`, when `detect_overhang_wall` is off, or when
`layer_id <= raft_layers`. That last one is the first-layer rule: the first printed layer sits on
the bed and is never an overhang.

### The blend

`support_blend_factors()` turns the per-sample supported/unsupported flags into a factor in [0, 1]
that multiplies the displacement (and, in Extrusion/Combined mode, the width delta):

* 0 at every unsupported sample.
* A linear ramp back to 1 over `kBlendSamples + 1 = 3` samples on either side of an unsupported run,
  computed as a two-sweep distance transform over the sample sequence - O(n), wrapping for a closed
  loop, clamped at the ends for an open segment from the paint splitter.
* A supported run shorter than the ramp never reaches 1, the graceful degradation the risk list
  wanted for short overhang runs and small point distances.

Sampling cadence is untouched: an unsupported sample is emitted at its exact un-jittered position,
never dropped, so segment length statistics, the `while (out.size() < 3)` fallback and the Arachne
closing-segment trim all see the same number of points as before. Seam handling is unchanged - the
endpoint connect and the closing-segment spacing trim still run after the displacement pass, exactly
where they ran before.

In Extrusion and Combined mode an unsupported junction gets neither a position shift nor a width
change: over open air a fatter or thinner bead reaches past the wall as far as a sideways shift
does, so both are held.

### Proofs

**Bar A (option OFF) - passed.** This branch's CLI against a build of its own base commit
(1afca41fab, a second worktree), four projects, each sliced into its own scratch copy of the control
data dir: OrcaToleranceTest plain, the overhang pyramid classic, the same under Arachne, and the
same with `detect_overhang_wall` forced on. All four are **identical apart from the timestamp line
and the single new `; fuzzy_skin_skip_overhangs = 0` line in the config block** - the whole diff, in
every case, is those two lines.

The comparison is against the branch's own base, not against the live install: the install is a
different branch (e72923e917) whose G-code differs from this base for reasons unrelated to this
change.

> **Fuzzy skin is not deterministic on this codebase**, so a Bar A over a project that actually
> fuzzes is impossible in principle. `FuzzySkin.cpp:random_value()` seeds from `std::random_device`
> (falling back to a hash of the thread id), and the same baseline exe run twice on the same
> fuzzy-skin project differs by ~27,600 G-code lines. That was measured, not assumed. The Bar A
> above therefore proves the new code is inert on every path that does not fuzz - which is every
> existing project that does not already use fuzzy skin - and the ON-mode behaviour is proved by the
> unit test and the demo instead.

**Unit test** `tests/fff_print/test_fuzzy_skin_overhangs.cpp`, added to
`tests/fff_print/CMakeLists.txt`. 39 assertions, green on three consecutive runs (worth stating,
given the RNG above). Model: the inverted pyramid this spec suggests, a 10x10 mm foot on the bed
widening to 38x38 mm over 5 mm - about 70 degrees from vertical, so each 0.2 mm layer steps ~0.56 mm
past the one below and its whole wall is unsupported, while the first layer sits on the bed and is
not. Fuzzy skin thickness 0.3, point distance 0.8, `fuzzy_skin_first_layer` on so the first layer is
the control. Six cases: fuzzy skin off entirely (the zero), the option OFF, the option ON for
external walls, ON for all walls, and ON under Arachne for both fuzzy modes. Each measures the
"waviness" of a layer's outer wall - the mean distance of its points from its own bounding
rectangle, which the model's square cross-section makes a direct read of the displacement - plus a
95th-percentile point-spacing bound that excludes a hard step at a blend boundary.

Two things the test taught, both recorded in its comments because they are traps for the next
person:

* **45 degrees is too shallow to test this.** At 45 degrees a 0.2 mm layer steps out 0.2 mm, which
  is *smaller than the support tolerance itself*, so no wall point is ever unambiguously
  unsupported.
* **A large single-step cantilever does not work either.** On the one layer where a wide slab first
  appears over a narrow column, the wall comes out of the generator with the raw slice polygon's ~30
  points rather than a resampled path: that area is handled as a bridge and its wall never reaches
  the fuzz pass, so there is nothing there for the option to act on.

**Demo** - `<scratchpad>/fuzzy_overhang_demo/`, the pyramid sliced with the option off and on by the
same exe, with `NOTES.md` giving the per-layer figures and the `; CHANGE_LAYER` line numbers to
compare. The result is unambiguous: **layer 1 is identical (10 extrusion moves either way)** - the
bed is not an overhang - and from layer 4 up the ON file carries ~60-65% of the OFF file's moves
(5387 vs 3232 overall). Reading the ON file's outer wall on a high layer shows four moves on four
exact coordinates: the wall over air is exactly the rectangle it would have been without fuzzy skin.

**Suites.** `libslic3r_tests`: 783 cases, 781 passed, 2 failed-as-expected. `fff_print_tests`: 9 of
18 cases fail - and the **baseline worktree fails the identical 9 cases and 23 assertions in the
identical files** (`test_data`, `test_flow`, `test_gcodewriter`, `test_model`, `test_print`,
`test_printgcode`, `test_skirt_brim`), so all of them are pre-existing. They are the bare-filename
export bug already recorded in `test_over_support_surfaces.cpp`. Note that `test_skirt_brim`'s
SIGSEGV aborts the default run before it reaches the new case, so the new test is run by tag
(`fff_print_tests.exe "[FuzzySkinOverhangs]"`) - which is how the three green runs above were taken.

### Unverified

* **No physical print.** The owner print test in section 7 has not been run. Nothing here shows the
  curling actually goes away on a real overhang - only that the toolpath over air is the un-fuzzed
  one.
* **Nobody clicked the checkbox.** The Tab.cpp line and the ConfigManipulation dependency compile
  and follow the neighbouring fuzzy options' pattern, but the GUI was never launched and the option
  was never toggled by hand. Every ON-mode result above comes from the CLI or the unit test setting
  the key directly.
* **Perf was not profiled.** The per-sample `contains()` against the lower slices is the cost
  section 6 flagged. It runs only with the option on, and against the cached polygon set, but no
  timing was taken; the bbox pre-filter / AABB tree mitigation remains a follow-up.
* **Extrusion and Combined modes are reasoned, not measured.** The width-hold is implemented and
  compiles, but every assertion above is in Displacement mode; the other two need a width-aware
  yardstick.
* **The unit test's ON-mode tolerance is 0.1 mm, not 1e-3.** The un-fuzzed control does meet 1e-3,
  and the demo G-code shows the wall over air collapsing to exact coordinates, but the measured
  ON-mode residual on an overhang layer is ~0.05 mm. That is understood to be the blend ramp around
  the seam junction - real, wanted displacement on a wall the overhang split has cut down to a dozen
  points - rather than surviving jitter, which would be an order of magnitude larger. It was
  reasoned from the numbers and the demo, not isolated with a dedicated experiment.
