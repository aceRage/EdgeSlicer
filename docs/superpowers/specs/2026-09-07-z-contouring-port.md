# Z contouring (Z anti-aliasing, "ZAA") - the minimal port

2026-09-07, branch `feat/z-contouring` off `feat/ultra-preferences` (`fcaed0bdc0`).

Z contouring gives top surfaces a per-point Z that follows the mesh instead of the layer plane, so
a shallow slope prints as a ramp rather than a staircase. On 5-25 degree slopes CNC Kitchen measured
Ra ~80 um dropping to ~25 um, i.e. a 0.2 mm ZAA layer looking close to a conventional 0.1 mm one.
It needs no hardware change: the nozzle simply moves in Z while extruding, and the flow follows the
local layer height.

## What was ported, and from where

Upstream: **OrcaSlicer PR #12736** ("feat: Add Z Anti-Aliasing (ZAA) contouring support (updated)",
head `adob:zaa`, merged 2026-05-02, merge commit `0f366ddff14a37a9c19fc6f626bd0191799f007f`),
shipped in OrcaSlicer v2.4.0. The reference read for this port was `SoftFever/OrcaSlicer` `main` at
`9b1e141a`, which already contains all six follow-up fixes:

| upstream | what it fixed | in this port |
|---|---|---|
| #13450 | degenerate polylines from ZAA crashing `as_polyline()` / `split_at_index()` | not needed - the polyline stays a 2-D `Polyline` here and the `< 2 points` bail-out below covers the cause |
| #13452 | `slice_z` bounds check rejecting a value that lands on the boundary within FP noise | yes, `is_approx` form in `compute_slice_z()` |
| #13508 | bail out on paths with fewer than two points; do not collapse a point that came from the original path | yes, both |
| #13510 | raycast from `slice_z` (not `print_z`) with a single UP ray; guard the normal with `is_hit()` | yes, both |
| #13766 | layer 0 keeps the plain mid-layer slicing plane | yes, in `compute_slice_z()` |

Cross-checked against `adob/BambuStudio-ZAA` (branch `zaa`, release `zaa-v1.0.3`), the origin fork.
Both upstreams are AGPL-3.0, the same licence as this tree, so the port is licence-clean.

Still open upstream and therefore inherited-or-guarded here:

* **#13552** - "perimeter replaced by artifacts when minimize wall height non-zero". Guarded, see
  *The #13552 guard* below.
* **#13540** - ZAA only affects upward-facing surfaces. Not addressed; downward-facing curves and
  surfaces resting on supports are untouched, exactly as upstream.

### Files

New:

* `src/libslic3r/ContourZ.hpp` / `ContourZ.cpp` - the algorithm, `Layer::make_contour_z()`, the
  `ContourZSamples` lookup, and the two pure helpers the tests drive.
* `tests/libslic3r/test_contour_z.cpp` - the unit tests.

Changed: `ExtrusionEntity.{hpp,cpp}`, `Layer.hpp`, `Print.hpp`, `Print.cpp`, `PrintObject.cpp`,
`PrintObjectSlice.cpp`, `GCode.cpp`, `PrintConfig.{hpp,cpp}`, `Preset.cpp`, `Fill/FillBase.{hpp,cpp}`,
`Fill/Fill.cpp`, `libslic3r/CMakeLists.txt`, `tests/libslic3r/CMakeLists.txt`,
`slic3r/GUI/Tab.cpp`, `slic3r/GUI/ConfigManipulation.cpp`, `slic3r/GUI/GUI_Factories.cpp`.

## The pipeline

1. **`compute_slice_z()`** (`PrintObjectSlice.cpp`). When any region has `zaa_enabled`, the slicing
   plane moves from mid-layer to `lo + zaa_min_z` (layer 0 excepted), so a contoured path can be
   pushed *down* by up to `height - zaa_min_z` and *up* by `zaa_min_z` and still describe the same
   solid. This fork's slicing entry point allowed the change verbatim: `new_layers()` computed
   `0.5 * (lo + hi)` inline, and that one expression became the call.
2. **`posContouring`** - a new `PrintObjectStep` between `posIroning` and `posSupportMaterial`,
   driven by `PrintObject::contour_z()` and wired into `Print::process()` right after the ironing
   loop, in all three of its branches. Invalidation follows `posIroning` everywhere.
3. `contour_z()` builds an `sla::IndexedMesh` from the object's raw mesh, transformed into object
   coordinates (instance transform with the XY offset replaced by `-center_offset`, ground level
   offset by `-min_z`), then runs `Layer::make_contour_z()` over layers `1..n` in parallel.
4. **`Layer::make_contour_z()`** resamples every eligible path at 0.1 mm, raycasts a vertical ray
   upward from the slicing plane at each sample, clamps the result, collapses collinear samples, and
   attaches the result to the path.
5. **`GCode::_extrude()`** emits per-segment Z and per-segment E for contoured paths, and disables
   arc fitting on them.

Eligible roles are exactly upstream's: `erTopSolidInfill`, `erIroning`, `erExternalPerimeter`,
`erPerimeter`. Sparse infill, bridges, supports and everything else are untouched.

## The minimal variant: what it leaves out versus `Polyline3`

Upstream changed a core geometry type - `ExtrusionPath::polyline` became a `Polyline3` (`Points3`,
per-point Z) - and that single change is what makes the merged PR 52 files and +2338/-392. This fork
sits on the pre-ZAA (Orca 2.3.x) layout with a 2-D `Polyline`, and the port keeps it.

Instead, a contoured `ExtrusionPath` carries

```cpp
ContourZSamplesPtr z_contour;   // std::shared_ptr<const ContourZSamples>
```

a **shared, immutable** set of the resampled points with their Z deltas, and the path's polyline is
replaced by exactly those points in 2-D. The emitter looks the Z up **by coordinate**, not by index.

That choice is the whole reason the port is small, and it is what makes it safe. Between
`posContouring` and G-code emission the polyline is still reordered, rotated, split at the seam and
clipped - `ExtrusionLoop::split_at`, `split_at_vertex`, `clip_end`, `ExtrusionPath::reverse`, the
loop rotation in `split_at_vertex` that `std::swap`s point vectors around. A parallel vector indexed
by point would desynchronise in every one of those. A coordinate-keyed lookup cannot: those
operations either preserve point values exactly or introduce a single interpolated point, and
`ContourZSamples::z_at()` resolves an unknown point by projecting it onto the nearest sample segment
and interpolating. **None of those functions needed to be touched** beyond copying the shared
pointer where `z_offset` was already being copied by hand (`ExtrusionLoop::split_at`).

What the minimal variant therefore does **not** get:

* **Arc fitting on contoured paths.** Upstream disables it too (`!enable_arc_fitting || ... ||
  path.z_contoured`), so this is parity, not a regression - but note it is a real cost: a contoured
  top surface is emitted as G1 segments at 0.1 mm resolution, so the file is larger and, on printers
  that benefit from G2/G3, the motion is less smooth. `ExtrusionPath::simplify()` and
  `simplify_by_fitting_arc()` both return early on a contoured path, so `posSimplifyPath` cannot
  undo the contour either.
* **Z inside the geometry.** Anything that reasons about extrusion geometry in 3-D upstream still
  sees 2-D here: `length()`, `polygons_covered_by_width()`, conflict checking, the preview's path
  extraction, `GCodeProcessor`'s own idea of the path. The G-code is correct because the emitter is
  the only consumer that matters for output, but the **preview still draws contoured paths flat**.
  Upstream shows the contour in the preview; this port does not. That is the largest visible gap.
* **`Circle.cpp`, `Polyline3`, `Points3` plumbing, `MultiPoint` 3-D, `AABBTreeLines` 3-D** - none of
  it is pulled in.
* **A degenerate case:** a path that crosses itself at exactly one XY point resolves both visits to
  the first sample's Z. Perimeters and monotonic top infill do not self-cross at a sample point in
  practice, and both visits were raycast against the same mesh at the same layer, so the two values
  agree to within the 0.1 mm resampling resolution.

## Interaction decisions

### (1) `offset_layers` - the one that needed real thought

This fork raises odd walls by `z_offset = 0.5` (a *fraction* of the layer height, `PerimeterGenerator.cpp`
`offset_layers_apply()` at ~165, applied at ~342 classic and ~697 Arachne), and `GCode::_extrude`
reads it as `m_nominal_z + path.z_offset * path.height`. ZAA writes per-point Z on the same paths.
Three separate decisions:

**Base Z.** The contour deltas are relative to the path's **own base**,
`print_z + z_offset * height`, not to `print_z`. `contour_z_sample_delta()` computes the raw delta
`d` from `print_z` exactly as upstream, then returns `d - z_offset*height`, and the emitter adds it
back to the offset base. The two therefore compose by addition, and the *absolute* band is
identical to upstream's: `[lo + zaa_min_z, print_z + zaa_min_z]`, whether the wall is offset or not.

One nuance the tests pinned: the cap "never above `print_z + zaa_min_z`" is a statement about
**contoured** moves. Where the raycast says "this is not a top surface" the delta is zero and the
wall stays exactly where `offset_layers` put it, half a layer up at `print_z + 0.5h`. It must not be
dragged down to `print_z + zaa_min_z` - that would undo `offset_layers` on every interior layer,
which is most of the print. Both `test_contour_z.cpp` and the Bar B analyzer distinguish the two
cases.

**The never-raise clamp.** Upstream forbids raising perimeters (`d > 0 -> d = 0`) so a raised wall
does not look like a seam. Its reference had to move from `print_z` to the offset base. If it had
not, the rule would read `d_off > -0.5h -> d_off = -0.5h` in the offset frame, which would force
*every* offset odd wall down by half a layer regardless of the mesh - silently undoing
`offset_layers` on exactly the walls it exists for. With the reference on the path's own base, ZAA
still never lifts a wall above where `offset_layers` put it, and can still lower it onto the mesh.
`test_contour_z.cpp` pins both halves of this.

**Flow.** `offset_layers` also sets `extrusion_multiplier` (1.5 on layer 1, 0.5 on layer `n-2`) - a
*bonding* ratio, not a height. ZAA contributes a *height* ratio `(height + d) / height`. Composing
them multiplicatively on those two layers would be the double count the brief warns about, because
the 1.5x exists precisely because that wall bonds over a modified height. Rather than reason about
it, **contouring is skipped on any path whose `extrusion_multiplier != 1`**, i.e. on those two
layers only. Everywhere else the flow is computed once, from the single summed local height:
`e_per_mm` (which already carries `extrusion_multiplier == 1`) times
`contour_z_extrusion_ratio(...) = (height + d) / height`. One height term, applied once.

**Arc fitting** is now off on contoured odd walls, which it was not before. **Z travel** is
unaffected: the fork's `slope_need_z_travel` check already fires on any Z change and now uses the
contoured start Z, and `_extrude` explicitly puts Z back for the next path, because a contoured path
ends wherever its last sample was.

### (2) Sloped paths

Upstream `throw`s `RuntimeError` on `ExtrusionPathSloped` and `ExtrusionLoopSloped`, and on any
unrecognised `ExtrusionEntity` subtype. This fork emits sloped loops routinely (scarf-joint seams)
and carries entity subclasses of its own, so **contouring skips them silently** - both the sloped
types and anything unrecognised. A scarf joint already owns the Z of its path; the two must not both
write it.

### (3) Painted zones / the local-Z dithering planner

`LocalZInterval` / `SubLayerPlan` (`Print.hpp` ~63) already subdivide a layer's Z budget into
sublayers in painted zones, for colour dithering. ZAA is a second claim on the same budget. The
policy here is the simple one: **ZAA yields entirely.** `need_z_contouring()` returns false when
`dithering_local_z_mode` is on, or when the object already has a sub-layer plan
(`local_z_intervals()` non-empty). It logs which guard tripped.

A finer policy is possible later - the planner owns the budget and hands ZAA a residual
`[z_lo, z_hi]` band per sublayer instead of the fixed `[-(height - min_z), min_z]` clamp - and
`posContouring` runs late enough to read the plan. Not done here.

### (4) Spiral vase, variable and adaptive layer height

`need_z_contouring()` also refuses when `spiral_mode` is on (the vase already owns Z along the whole
path) and when the object has a non-empty `layer_height_profile` (variable/adaptive layer height -
the same exclusion `offset_layers` documents, and ZAA re-purposes the intra-layer height budget).
It also refuses when the object does not have exactly one instance, which is what the raycast setup
requires.

### (5) Support-interface tops and bridges

Follows upstream: they are simply not eligible roles. Bridges (`erBridgeInfill`,
`erInternalBridgeInfill`, `erOverhangPerimeter`) and this fork's over-support roles
(`erBottomSurfaceOverSupport`, `erOverSupportPerimeter`) are never contoured.

### The #13552 guard

Upstream's slope rule is a **hard step**: the instant a sample's mesh slope exceeds
`zaa_minimize_perimeter_height`, the wall drops by the full `half_width * sin(slope)`. A wall whose
slope varies across the threshold therefore gets a half-line-width Z jump part-way along, and
upstream #13552 ("perimeter replaced by artifacts") is that jump. The ZAA author confirms the
trigger is "varying slopes that cross the angle threshold" and the only mitigation offered upstream
is to set the angle to 0 - i.e. turn the feature off, while the default is 35.

This port keeps upstream's default of 35 and instead **ramps the adjustment in continuously** over
`ZAA_SLOPE_RAMP_DEGREES = 5` degrees above the threshold:

```
ramp = clamp((slope_deg - threshold) / 5, 0, 1);
d   += adjustment * ramp;
```

Below the threshold: identical to upstream (nothing). At `threshold + 5` and above: identical to
upstream (the full drop). In between: a continuous ramp instead of a cliff, so no sample can step by
more than `half_width / 5` per degree of slope change. `test_contour_z.cpp` asserts both the
continuity and the agreement with upstream outside the band. This is a deliberate, documented
deviation from upstream.

Upstream's `throw RuntimeError("ContourZ: got positive adjustment")` is also gone: the value is
clamped to 0 instead. `sin()` of an angle in `[0, pi/2]` cannot be negative, so the branch is
unreachable either way, and a slicing pass should not abort a print over an unreachable assertion.

## The four settings

Upstream's names, defaults, tooltips and placement (process tab, Quality page, a new "Z contouring"
group after Ironing; all `comExpert`; all per-object overridable via `GUI_Factories.cpp`).

| key | type | default | meaning |
|---|---|---|---|
| `zaa_enabled` | bool | `false` | master switch |
| `zaa_min_z` | float mm | `0.05` | minimum local layer height; also sets the slicing-plane offset |
| `zaa_minimize_perimeter_height` | float deg | `35` | drop top-surface walls to the model edge above this slope; 0 = off |
| `zaa_dont_alternate_fill_direction` | bool | `false` | keep the fill direction constant on contoured tops |

`ConfigManipulation` greys the last three out when `zaa_enabled` is off.

## Verification

All three gates were run on 2026-09-07 against a baseline built from `fcaed0bdc0` in its own
worktree (the main tree had already moved past it, so it was not usable as the head).

### Unit tests - PASS

`libslic3r_tests`: **626 cases, 624 passed, 2 failed as expected** (the head's own two). The head
had 621 cases; `test_contour_z.cpp` adds 5. It covers the raycast clamp (both bounds, the "not a top
surface" rejection, the miss case, the wider ironing band), the perimeter never-raised rule, the
slope rule and the continuity of the #13552 ramp, the `offset_layers` composition (base Z, absolute
band, the never-raise reference, the single flow term), and a real 10 degree ramp mesh raycast
through `sla::IndexedMesh` whose samples must rise monotonically and stay inside the clamp.

### Bar A - PASS, all four cases

`scripts/zaa/bar_a_zaa.py`. With `zaa_enabled` off, a CLI slice of `OrcaToleranceTest.stl` with an
isolated `--datadir` copied from `snorca_hubtest/dd_lan`:

| case | result |
|---|---|
| Bambu Lab P1S 0.4 nozzle | identical apart from the timestamp and 4 `zaa_*` lines (24565 lines) |
| P1S + `offset_layers` | identical, 24951 lines |
| Snapmaker U1 (0.4 nozzle) | identical, 25521 lines |
| U1 + `offset_layers` | identical, 26027 lines |

The four added lines are exactly
`; zaa_dont_alternate_fill_direction = 0`, `; zaa_enabled = 0`, `; zaa_min_z = 0.05`,
`; zaa_minimize_perimeter_height = 35`, and the baseline carries none of them.

### Bar B - PASS

`scripts/zaa/bar_b_zaa.py` (analysis in `bar_b_report.py`) against the 40 x 40 mm wedge rising
2 mm -> 12 mm that `scripts/zaa/make_wedge.py` generates - 14.036 degrees, which is what "a 40x40
wedge from 2 to 12 mm" actually is.

ZAA on, 0.2 mm layers, `--export-3mf` (which runs the fork's own `GCodeProcessor`): slice returns 0
and writes the 3MF. 25078 moves, **10360 of them contoured** (carrying their own Z), across 50
layers, and **every one of those 50 layers carries more than one Z**. Contoured features: outer wall
396, top surface 9964.

* Clamp: 10360 of 10360 inside the absolute band `[lo + zaa_min_z, print_z + zaa_min_z]`, none
  outside.
* Flow: `E per mm / ((H + d) / H)` is constant per feature - outer wall `e_per_mm = 0.03076` with
  0.296 % spread, top surface `0.03108` with 0.357 % spread.

With `offset_layers` also on: 10842 contoured, adding 482 inner-wall moves. 10650 land in the
absolute band and **192 rest exactly on their own uncontoured base `print_z + 0.5h`** - the raised
odd walls at points the raycast declared not a top surface, which is the correct behaviour (see the
nuance above). Nothing lands outside both. The flow fit is the decisive one:

```
Inner wall   n=482   base = print_z + 0.50*H   e_per_mm = 0.03319   spread 0.365%   ok
Outer wall   n=396   base = print_z + 0.00*H   e_per_mm = 0.03076   spread 0.296%   ok
Top surface  n=9964  base = print_z + 0.00*H   e_per_mm = 0.03108   spread 0.357%   ok
```

The inner walls only fit with the base at `print_z + 0.5h`: their Z **is** the offset base plus the
contour, and their flow follows the single summed local height. Fitting them against `print_z`
instead gives a 1.8x error.

Spiral vase on top of ZAA: slices clean, rc 0, no error.

Spot-checked by hand against the mesh: at layer `print_z = 2.2` the outer wall emits `Z2.102` where
the ramp surface is at 2.1025 and `Z2.177` where it is at 2.1775 - the contour is landing on the
mesh to within the 0.1 mm resampling, including negative deltas.

### The guards, confirmed live

Slicing the wedge at `--debug 4` shows each guard firing by name:

| run | log |
|---|---|
| `--zaa-enabled=1` | `Contouring in parallel - start` / `- end`, status "Z contouring" |
| `+ --spiral-mode=1` | `ZAA: skipped, spiral vase mode is on` |
| `+ --dithering-local-z-mode=1` | `ZAA: skipped, local-Z dithering owns the layer Z budget` |

The remaining guards - a variable layer-height profile, an object with more than one instance, and
an existing sub-layer plan - are not reachable from the CLI and were verified by inspection only.

## The hardware test the owner should print

Generate the wedge with `python scripts/zaa/make_wedge.py wedge.stl` (40 x 40 mm, 2 mm -> 12 mm,
14.04 degrees) and print it three times at **0.2 mm**
layers on the same filament and printer, changing nothing else:

1. **ZAA off** - the control. The slope should show clear 0.2 mm stair steps.
2. **ZAA on** (`zaa_enabled`, defaults otherwise: `zaa_min_z` 0.05, minimize-wall-height 35).
3. **ZAA on + offset layers on** - the interaction this port had to get right.

Compare the slope by eye under a raking light and by running a fingernail down it across the steps.
Expect (2) to feel and look close to a 0.1 mm print of (1). Things to look for specifically:

* the wall along the top edge of the ramp - #13552's artifact class is missing or shredded
  perimeters where the slope crosses 35 degrees; the ramp is a constant 14 degrees so it should not
  trigger, but the short end faces will cross it;
* nozzle-skirt drag where the head fills from high to low (a known ZAA artifact upstream);
* on (3), that the odd walls still interlock and that nothing sits proud of the top surface -
  the port's claim is that a contoured odd wall lands on the mesh and never above `print_z + 0.05`.

Also worth a look: a 0.3 mm ZAA print, which upstream reports as still close to a conventional
0.1 mm finish and about half the time.
