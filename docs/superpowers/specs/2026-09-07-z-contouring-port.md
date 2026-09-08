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

## The review (2026-09-07) - "it looks like fuzzy skin"

The owner printed the wedge on the live install at `bfc949efca` with the defaults and reported that
the effect was **lackluster**, and that "the way the outer nozzle movement looks, it almost ends up
looking like fuzzy skin because of the uneven movements it's doing."

That is a fair description of what the G-code actually contained. Everything below was measured on
real slices (P1S 0.4 nozzle, `0.20mm Standard @BBL X1C`, Generic PLA, 0.2 mm layers, an isolated
`--datadir` copied from `snorca_hubtest/dd_lan`) of four models: the 40 x 40 wedge
(`scripts/zaa/make_wedge.py`, a constant 14.04 degrees), a 30 mm hemisphere on a 2 mm plinth
(`scripts/zaa/make_dome.py`, every slope from 0 to 90 degrees), the same hemisphere at a coarse
tessellation (40 x 20 facets), and an ellipsoid (the same dome scaled 0.5 in Y), which is the only
one of the four whose surface slope varies *around* a single outer-wall loop.

Two new analyzers do the measuring, and both are committed:

* `scripts/zaa/zaa_jitter.py` - **within** a layer: the distribution of the Z step between
  neighbouring extrusion moves along one contoured path, its second difference (how fast the Z
  *slope* changes), and the number of sign reversals. A true ramp has a large first difference and
  a near-zero second one; fuzz is the opposite.
* `scripts/zaa/zaa_wall_profile.py` - **across** layers: the outer wall's Z offset from `print_z`
  layer by layer, and the effective bead height that results. A wall printing 190, 232, 193,
  230 um reads as banding however smooth each individual layer is.

### What was NOT wrong

* **The raycast follows the mesh exactly.** On the wedge the surface height is known analytically
  (`z = 2 + 0.25*(x-108)` in plate coordinates), and every in-band contoured move was compared
  against it: top surface 4935 moves, error **0.00 um**; outer wall 300 moves, error mean 0.31 um,
  max 0.50 um (the G-code's own 1 um Z quantisation). Facet crossings, the AABB tree and the 0.1 mm
  resampling are not the noise source.
* **Z is emitted in the same `G1` as X/Y/E**, never as a separate move. The wedge's 25032-move file
  contains 115 bare `G1 Z...` lines and every one of them is an ordinary layer change or Z hop.
  There is no per-segment Z stutter for the firmware's look-ahead to trip over.
* **The `offset_layers` composition is not engaged when `offset_layers` is off** - `z_offset` is 0,
  `z_offset_mm` is 0 and both the clamp shift and `zaa_base_z` reduce to identities. Bar A's
  byte-identity on the two `_ofs` cases is the same statement from the other side.
* **This is not a porting divergence.** Upstream contours `erExternalPerimeter` and `erPerimeter`
  exactly as this port does: same four eligible roles, same 0.1 mm resampling, same
  `-half_width*sin(slope)` rule above `zaa_minimize_perimeter_height`, same never-raise clamp, same
  `z_contoured` early-out in `simplify()` / `simplify_by_fitting_arc()`, same per-point Z in the
  extrusion move. Upstream does **not** leave outer walls straight. So the fuzz is upstream's, and
  #13552 is the same bug seen along the path rather than across the layers.

### What was wrong - three findings, with numbers

**1. The slope rule is the fuzz.** `-half_width * sin(slope)` is a function of the local surface
*normal*, not of the local surface *height*, so it injects up to `half_width` (0.21 mm at a 0.42 mm
external width, a full layer) of Z that has nothing to do with flattening the staircase. Wherever
the surface slope varies quickly - around a loop on a non-axisymmetric model, or from layer to
layer on any curve - the wall's Z varies with it.

| measurement | slope rule on (default 35) | slope rule off (`=0`) |
|---|---|---|
| ellipsoid, within one outer-wall loop, \|dZ\| max | **28 um** | 5 um |
| ellipsoid, same, \|d2Z\| max (change of slope between neighbours) | **32 um** | 5 um |
| ellipsoid, same, sign reversals | **505 of 8561 steps (5.90 %)** | 22 of 2654 (0.83 %) |
| hemisphere, outer-wall bead height across layers | **185..232 um, sd 11.4** | 185..194 um, sd 2.5 |
| hemisphere, biggest layer-to-layer change of bead height | **39 um** | 4 um |

The 39 um jump sits exactly where the hemisphere's surface slope crosses 35 degrees: the 5 degree
ramp this port added for #13552 is crossed in about four layers there, so the whole
`0.21*sin(35) = 0.12 mm` arrives over four layers. On the coarsely tessellated hemisphere, where the
facet normals quantise the slope, the same measurement was **174..271 um with an 80 um jump**.

**2. The top-of-band tolerance is a 50 um cliff.** Upstream pins every sample whose mesh is between
`max_up` and `max_up + 0.03` to exactly `max_up`, then drops the next one - a micron further up - to
zero. Two neighbouring beads therefore differ by a full `zaa_min_z`, 50 um by default.

On the wedge this was not a detail, it was **the entire top-surface contouring**: the emitted delta
took only the values 0 and +50 um (measured range `[0, +50]`, `|dZ|` mean 25 um, and every second
difference exactly 50 um), a ridge one bead wide repeated every 0.8 mm across the whole ramp. The
hemisphere and the ellipsoid showed the same 50 um step on their top surfaces.

**3. The flow used the wrong height.** The emitter (like upstream) scaled `E` by
`(height + z_at(end)) / height`, i.e. by the local height at the segment's **end point**, while the
height varies linearly along the segment. Against the trapezoid - the exact volume for a linear
ramp - the error on Z-transition segments measured **mean 12.2 %, max 18.8 %** on the wedge's outer
wall and **mean 12.5 %** on its top surface, over 3535 segments, alternating in sign. An extrusion
whose width alternates by a tenth at every Z step is a fuzzy-looking extrusion.

### One more thing the numbers said: on a shallow ramp there is not much for ZAA to contour

At 0.2 mm layers on a 14 degree ramp the band of a layer whose surface is within one layer height is
`0.2 / tan(14) = 0.8 mm` wide, and two walls (0.42 + 0.45 mm) consume all of it. Everything the
slicer then labels "Top surface" is the external-infill margin, which lies *under* the next layer,
0.05 to 0.40 mm below its own surface. So on a wedge the only real ZAA effect is the outer wall
riding down onto the mesh at the leading edge (98 um at the last measured layer, exactly the mesh),
and the useful part of the top-surface contouring never gets a chance. That is the "lackluster"
half of the report, and it is a property of the technique at that slope and layer height, not of
the port: a dome, whose exposed top is wide, is contoured over `[-107, 0]` um on its cap.

### The fix

All six changes are in `ContourZ.{hpp,cpp}` and the two emitter sites in `GCode.cpp`. No new config
key: every constant is documented in `ContourZ.hpp` next to the measurement that set it.

| # | change | why |
|---|---|---|
| 1 | `slope_ramp()`: **smoothstep over `ZAA_SLOPE_RAMP_DEGREES = 20`** (was linear over 5) | finding 1. Smoothstep makes the derivative continuous too, so the *bead height* cannot step either; 20 degrees spreads the 0.12 mm over enough layers that the crossing disappears into the layer height |
| 2 | the top tolerance **fades** from `max_up` to 0 across `ZAA_TOP_TOLERANCE_MM` instead of being pinned then dropped | finding 2. Identical to upstream at both ends of the band, continuous in between |
| 3 | **`contour_z_smooth_profile()`**, a symmetric moving average of radius `ZAA_SMOOTH_RADIUS_SAMPLES = 3` (a 0.6 mm window) over each path's delta profile | findings 1 and 2, and mesh facet noise generally. A straight ramp is a fixed point of a moving average, so mesh following is untouched; every output is a convex combination of inputs, so the clamps and the never-raise rule survive it |
| 4 | a per-path deadband, `ZAA_MIN_PATH_DELTA_MM = 0.010`: a path whose whole profile is under 10 um is left uncontoured | flat-ish walls stay straight instead of gaining a Z word per move and losing arc fitting for a contour the machine cannot express. Per *path*, so it can never introduce a step |
| 5 | the collinear collapse gets a real tolerance, `ZAA_COLLAPSE_TOLERANCE_MM = 0.001` (was `EPSILON`, 0.1 um) | a smoothed profile is a curve, and at 0.1 um almost no sample collapses; this holds the emitted profile within a micron of the smoothed one at a third of the point count |
| 6 | the flow uses the **trapezoid**: `0.5 * (z_at(a) + z_at(b))` | finding 3 |

The pass is now: resample and raycast the whole path -> smooth the profile -> deadband -> collapse,
rather than upstream's interleaved sample-and-collapse. Nothing else about the pipeline moved.

`contour_z_smooth_profile()` is exported so the tests can drive it directly, and
`ZAA_SLOPE_RAMP_DEGREES` moved from `ContourZ.cpp` to the header for the same reason.

### The result, on the same four models

Outer wall, **within** a layer (`scripts/zaa/zaa_jitter.py`, microns):

| model | \|dZ\| max before -> after | \|d2Z\| max before -> after | reversals before -> after |
|---|---|---|---|
| wedge | 75 -> 56 (the true 14 degree ramp) | 75 -> 46 | 0 -> 0 |
| hemisphere | 2 -> 1 | 4 -> 1 | 11 -> 0 |
| ellipsoid | 28 -> **8** | 32 -> **8** | 505 (5.90 %) -> **186 (2.10 %)** |

Top surface, within a layer:

| model | \|dZ\| max | \|d2Z\| max | emitted delta range |
|---|---|---|---|
| wedge | 50 -> **7** | 50 -> **5** | `[0, +50]` -> `[0, +16]` |
| hemisphere | 50 -> 23 | 50 -> 12 | `[-107, +50]` -> `[-101, +35]` |
| coarse hemisphere | 59 -> 28 | 51 -> 26 | `[-112, +50]` -> `[-105, +37]` |
| ellipsoid | 50 -> 15 | 50 -> 7 | `[-105, +50]` -> `[-93, +36]` |

The residual 23-28 um on the curved tops is the mesh: 23 um over a 0.1 mm sample is a 13 degree
surface, which is what a hemisphere's cap edge is. The **second** difference is the fuzz measure,
and it is down to 5-12 um everywhere except the coarse mesh's facet boundaries.

Outer wall, **across** layers (`scripts/zaa/zaa_wall_profile.py`, bead height in microns):

| model | before | after |
|---|---|---|
| hemisphere | 185..232, sd 11.4, max jump **39** | 185..211, sd 7.3, max jump **21** |
| coarse hemisphere | 174..271, sd 11.9, max jump **80** | 168..226, sd 7.5, max jump **39** |
| ellipsoid | 172..224, sd 9.2, max jump **28.5** | 176..204, sd 7.0, max jump **10** |
| wedge | 137.5..200, sd 8.2, max jump 62 | unchanged (a 14 degree ramp never reaches the 35 degree threshold, so there was no slope term to smooth; the profile is pure mesh) |

Flow: the Bar B fit of `E per mm / ((H + d)/H)` with `d` the trapezoid height is now **0.278 %**
spread on the outer wall, **0.305 %** on the top surface and **0.238 %** on the offset-layers inner
wall - tighter than the port's original end-point fit (0.296 / 0.357 / 0.365 %), which is the
independent confirmation that the trapezoid is the right height term.

The cost is G-code size, because a smoothed profile collapses less: the wedge grows from 865 KB /
25032 `G1` to 1128 KB / 32237 (+30 %), the hemisphere 536 KB -> 610 KB (+14 %), the ellipsoid
773 KB -> 778 KB (+0.7 %).

### Proofs re-run after the change

* **`libslic3r_tests`: 657 cases, 655 passed, 2 failed as expected** (the head's own two). The
  `[ContourZ]` tag alone is 6 cases / 1278 assertions, all passing: the existing five updated for
  the faded tolerance and the smoothstep ramp (which now also asserts that the *step between steps*
  never jumps, i.e. the derivative is continuous), plus a new case for
  `contour_z_smooth_profile()` - a ramp is a fixed point, alternating 40 um noise comes out under
  10 um, every output stays inside the input's range, and a short profile or a zero radius is
  returned untouched.
* **Bar A: PASS, all four cases.** `scripts/zaa/bar_a_zaa.py`, baseline = a build of the head
  `bfc949efca` (`snorca_hubtest/inst_u1base`), candidate = this branch: with `zaa_enabled` off the
  P1S and U1 slices of `OrcaToleranceTest.stl` are identical apart from the timestamp, with and
  without `offset_layers` (24568 / 24954 / 25524 / 26030 lines). The script itself needed one
  change: its baseline used to be a *pre*-ZAA build and it asserted the baseline carried no `zaa_*`
  config lines; now that the port is merged the head carries them too, so it asserts they match the
  candidate's instead.
* **Bar B: PASS.** `scripts/zaa/bar_b_zaa.py` on the wedge: 17444 contoured moves over 50 layers,
  every layer carrying more than one Z, none outside `[lo + min_z, print_z + min_z]`; with
  `offset_layers` also on, 18118 contoured moves, 240 of them at their own raised base and none
  above it; `--export-3mf` (which runs the fork's own `GCodeProcessor`) writes the file in both
  cases; spiral vase on top of ZAA still slices clean. Two assertions in `bar_b_report.py` were
  updated for the reviewed behaviour and the reasons are in the file: the flow check now uses the
  trapezoid, and the clamp check accepts the *interval* between `lo + min_z` and a wall's own base
  rather than only its two endpoints - the smoother's whole job is to interpolate between an
  uncontoured stretch of a wall and a contoured one instead of stepping between them.
* `--export-3mf` also accepted the hemisphere, and `--ironing-type=top` with ZAA on slices clean
  with ironing present in the output.

### What is still not addressed

* **The slope rule remains a bias term driven by the surface normal.** The geometrically exact
  version of "lower the wall until its outer edge meets the model" is a second raycast at the
  point `half_width` outboard along the path normal, which needs the path's orientation and is a
  bigger change than a review should make. `sin(slope)` is also only an approximation of the
  `tan(slope)` the geometry asks for, and it diverges badly above 45 degrees - upstream's, kept.
* **The lower rejection is still a cliff.** `d < -height` returns 0 while `d` just above it clamps
  to `-(height - min_z)`, a 150 um step in principle. It did not fire on any of the four models
  (the deepest delta measured was -125 um), so it was left as upstream rather than faded like the
  upper one.
* **Upstream #13540** - downward-facing surfaces - still unaddressed, as before.
* **The preview still draws contoured paths flat**, as the minimal variant always did.
* **No print was made of the fixed G-code.** Everything above is measured on the emitted file.


## The hardware test the owner should print

Generate both models:

```
python scripts/zaa/make_wedge.py wedge.stl     # 40 x 40 mm, 2 -> 12 mm, 14.04 degrees
python scripts/zaa/make_dome.py  dome.stl      # 30 mm hemisphere on a 2 mm plinth
```

The wedge is the constant-slope case and the dome is the varying-slope case - the dome is the one
that showed the artifact this review fixed, because its surface slope sweeps the whole range and
crosses `zaa_minimize_perimeter_height` part-way up. Print at **0.2 mm** layers, same filament and
printer, changing nothing else.

**The before/after test.** The interesting comparison now is not only ZAA on versus off, it is this
branch versus what the owner already printed on `bfc949efca`. Print, in this order:

1. **wedge, ZAA off** - the control. The slope should show clear 0.2 mm stair steps.
2. **wedge, ZAA on** (`zaa_enabled`, defaults otherwise: `zaa_min_z` 0.05,
   minimize-wall-height 35).
3. **dome, ZAA off** - the control for the curved case.
4. **dome, ZAA on** - the one to compare against the 2026-09-07 print.
5. **wedge, ZAA on + offset layers on** - the interaction this port had to get right.

Compare by eye under a raking light and by running a fingernail across the steps. What to look for,
now that the numbers say what changed:

* **On the dome, the band where the surface passes 35 degrees** - roughly 11 to 14 mm up on a
  30 mm hemisphere. That is where the outer wall's bead height used to swing from 193 to 232 um
  over four layers; it should no longer read as a band.
* **On the wedge's ramp, the regular ridges every 0.8 mm** - one bead wide, 50 um proud, the
  top-of-band cliff. They should be gone; the top surface's emitted range is now `[0, +16]` um
  instead of `[0, +50]`.
* **Bead width consistency wherever the nozzle changes Z** - the trapezoid flow removed an
  alternating 12 % width error at every Z transition.
* the wall along the top edge of the ramp - upstream #13552's artifact class is missing or
  shredded perimeters where the slope crosses 35 degrees; the wedge's short end faces cross it;
* nozzle-skirt drag where the head fills from high to low (a known ZAA artifact upstream, and one
  this review does not touch);
* on (5), that the odd walls still interlock and that nothing sits proud of the top surface - the
  port's claim is that a contoured odd wall lands on the mesh and never above its own base.

And the honest expectation for the wedge specifically: at 0.2 mm and 14 degrees the exposed band is
0.8 mm and the two walls fill it, so ZAA's whole effect there is the outer wall riding onto the
mesh. If the owner wants the effect ZAA is famous for, the dome - or a shallower slope at a
*larger* layer height, where the exposed band is wider than the walls - is the model that shows it.
A 0.3 mm ZAA print of the dome is worth a look for the same reason: upstream reports it as still
close to a conventional 0.1 mm finish at about half the time.

## Constant-flow speed scaling (2026-09-08, branch `feat/z-contouring-speed`)

The owner printed the reviewed version on 2026-09-08 and reported it "looks much better", with
one remaining complaint: **at 0.2 mm layers the contoured surface is a little rougher than at
thinner layers**. The diagnosis in the brief: the ZAA band is a full layer high, so a contoured
segment swings from a 0.25 mm bead down to the 0.05 mm `zaa_min_z` minimum *at the role's
unchanged speed*. The thin end is starved of material and the Z axis is doing its fastest work
exactly where the bead is thinnest.

### The rule

For every contoured extrusion segment, with `H` the nominal layer height and `h_seg` the
segment's local height (`H` plus the trapezoid mean of its two endpoint contour deltas - exactly
the height the segment's `E` was already scaled by):

```
F_seg = clamp( F_role * h_seg / H , floor , F_role )   then capped by max_volumetric_speed
```

* `F_role` is the feed rate the segment was about to run at, so the scaling **composes with**
  rather than overrides every dynamic slowdown the fork already applies: overhang grading and
  the curled-perimeter estimator (which sets its own per-point F, scaled here in turn),
  small-perimeter / resonance avoidance, the initial-layer ramp, and the
  `filament_max_volumetric_speed` cap that `speed` has already been clamped to.
* **Never above `F_role`.** A contoured bead can sit up to `zaa_min_z` proud (the top half of the
  band, on top surfaces) and the ratio would ask for a speed-up; the role speed is a deliberate
  ceiling, so contoured moves are only ever slowed.
* **Floor:** a fixed `ZAA_MIN_SPEED_MM_S = 10 mm/s`, not a config key. It is a lower bound on sane
  behaviour rather than a tuning knob - below it a print move becomes a dwell that oozes - and it
  never raises F above a role that is already slower than it.
* **Hysteresis:** `ZAA_SPEED_HYSTERESIS = 0.05`. Consecutive segments whose local height is within
  5 % of the height that set the F in force keep that F. This bounds the flow error the hysteresis
  itself introduces at exactly 5 %, and it is what keeps the F changes down to 2-3 % of contoured
  segments on a dome (41-45 % on the wedge, whose contour is one continuous ramp).

Both `contour_z_segment_feedrate()` and `contour_z_speed_within_hysteresis()` are pure functions
in `ContourZ.{hpp,cpp}`, driven directly by the tests.

**A note on the brief's formula.** The brief wrote the rule as `F_seg = F_role * (h_nominal /
h_seg)` while also requiring that "a thin segment slows down", that the 10 mm/s floor be
reachable, and that "the time estimate reflect the slower thin segments". Those cannot both hold:
with `h_nominal / h_seg` a thin segment asks for a *higher* F, the never-exceed clamp then pins it
at `F_role`, and the rule becomes inert on exactly the walls the owner complained about. Measured
on the dome, all 6266 contoured outer-wall segments are thinner than nominal, so that reading
changes nothing on them and the floor is unreachable (the slowest it can go is `H / (H + min_z)` =
0.8 x `F_role`). The transposed form above satisfies every clause of the brief, so that is what is
implemented; the two orderings are otherwise identical.

### The key

| key | type | default | meaning |
|---|---|---|---|
| `zaa_speed_scaling` | bool | `true` | keep volumetric flow constant on contoured moves by slowing thin segments |

Placed next to the other four `zaa_*` keys in `PrintConfig.cpp`, greyed out with them by
`ConfigManipulation` when `zaa_enabled` is off, per-object overridable via `GUI_Factories.cpp`,
and on the Quality page under Z contouring. The tooltip carries the `zaa_min_z` guidance the
brief asked for: at 0.2 mm layers a minimum Z height of 0.08 keeps the slowdown to 2.5x, where
the 0.05 default asks for 4x on the thinnest segments.

### THE BLOCKER: CoolingBuffer owns one speed per extrusion

**This feature does not currently reach the G-code when `slow_down_for_layer_cooling` is on,**
which is the default on every profile tested. The scaling is correct and provably applied when
that option is off; with it on, the emitted file is byte-identical to the unscaled one apart from
the timestamp, the config line and a 12 um retract difference.

The cause is architectural, in `GCode/CoolingBuffer.cpp`:

* the buffer models **one adjustable speed per extrusion path**. The first `;_EXTRUDE_SET_SPEED`
  line of a path becomes the block's speed modifier (`active_speed_modifier`), and every movement
  line after it until `;_EXTRUDE_END` is folded into that modifier and then discarded outright
  (`line.type = 0; // Don't store this line`, ~line 490);
* it also asserts that no `G1` inside such a block carries its own `F` (~line 476).

Three emission strategies were tried, each defeated by a different half of that:

| strategy | result |
|---|---|
| `F` word inside the movement `G1` (what the brief asked for) | violates the assert; in Release the line silently **escapes the layer-time slowdown**. Measured on the dome: contoured outer walls kept running at 9124 mm/min where cooling had slowed the rest of the layer to 2841 - a 3.2x over-speed, strictly worse than doing nothing |
| a separate `G1 F...` carrying `;_EXTRUDE_SET_SPEED` | folded into the path's first modifier and dropped; 0 of 7004 contoured segments kept an F |
| `;_EXTRUDE_END` then a new `;_EXTRUDE_SET_SPEED` block per speed change | same; the collapse is not avoided this way |

The matrix, dome at 0.2 mm, model printing time:

| | scaling off | scaling on |
|---|---|---|
| `slow_down_for_layer_cooling=1` (default) | 12m 43s | **12m 43s** (no effect) |
| `slow_down_for_layer_cooling=0` | 6m 21s | 6m 19s |

With cooling off the scaling demonstrably works, measured on the contoured moves alone:
**contoured extrusion time 9.1 s -> 11.0 s (+21 %), non-contoured 151.5 s -> 151.5 s** (identical
to the microsecond). That is the feature doing exactly what it should, on exactly the moves it
should, and nothing else.

Note also that the *whole-print* estimate is not a valid measure of this change even when it
works: slowing the contoured moves lengthens each layer, which makes the layer-time cooling
slowdown relax the speed it was imposing on the **whole** layer, so the total can legitimately
come out lower. `bar_b_speed.py` therefore asserts on the contoured extrusion time computed from
the file, not on the header estimate.

**Resolving this needs a decision that is bigger than this change:** either teach CoolingBuffer to
carry a per-segment speed profile through a path (it currently reduces a path to one `feedrate`,
one `time` and one `time_max`), or let a contoured path opt out of the layer-time slowdown and
accept that ZAA tops are not cooled like the rest of the layer. Neither should be chosen without
the owner. Until then the key is best treated as effective only with layer-time cooling off.

### Proofs

* **`libslic3r_tests`: 700 cases, 698 passed, 2 failed as expected** (the head's own two). The
  `[ContourZ]` tag is 8 cases / 1473 assertions, all passing: the review's six plus two new ones
  for the scaling rule (constant flow on a thin segment, the never-speed-up clamp swept across
  the whole height range, the floor and its interaction with an already-slow role, the volumetric
  cap winning over both, degenerate inputs, and monotonicity in the local height) and for the
  hysteresis (the band's two sides, no-F-yet, the bounded flow error, and that a slowly drifting
  profile emits exactly one F while a real ramp emits more than one but far fewer than one per
  segment).
* **Bar B (`scripts/zaa/bar_b_speed.py` + `zaa_speed_report.py`)**: on the wedge and the dome at
  0.2 and 0.12 mm, with `slow_down_for_layer_cooling` off, every contoured path satisfies the
  within-path invariant `F_seg / h_seg = F_role / H` to **0.000 % mean / 0.575 % max** on the
  outer wall and the inner wall, with the residual on top surfaces bounded by the 5 % hysteresis
  band and **nothing outside it**; no segment below the floor; `--export-3mf` accepted every
  case; and the **Z profile is identical to the unscaled slice in every run** - 7029 vs 7029
  values on the dome, 17444 vs 17444 on the wedge - so every jitter and bead-height number the
  2026-09-07 review measured is untouched by construction.
* The analyzer measures a **within-path** ratio rather than an absolute flow precisely so it needs
  no role-speed reference. Two earlier versions that inferred `F_role` (from the maximum F in the
  scaled file, then from a reference slice) both reported phantom 11 % and 87 % errors: a path
  whose contour is flat-but-not-nominal is scaled uniformly, so the largest F in the scaled file
  is already scaled, and the largest F in a reference slice is a travel speed. It also skips the
  `G1` after any `G2`/`G3`, whose segment length is not measurable from the previous `G1`.
* **Bar A**: `scripts/zaa/bar_a_zaa.py`, updated for the fifth `zaa_*` config line.

### At 0.12 mm the scaling engages less, as expected

On the dome the contoured outer wall spans `h_seg` 0.079..0.189 mm at 0.2 mm layers (F 3825..9218,
a 2.4x spread) but only 0.063..0.105 mm at 0.12 mm layers (F 6252..10537, a 1.7x spread), and the
share of contoured segments needing an F change falls from 2.9 % to 2.1 %. The wedge shows the
same: 0.102..0.200 mm at 0.2 mm against 0.102..0.120 mm at 0.12 mm. Thinner layers give the band
less room to vary, which is the same reason the owner saw less roughness there.

### The print test the owner should run

The same two models as the review - `scripts/zaa/make_wedge.py` and `scripts/zaa/make_dome.py` -
at **0.2 mm**, ZAA on, changing only `zaa_speed_scaling`:

1. **dome, scaling off** - the 2026-09-08 print, the control.
2. **dome, scaling on**.
3. **wedge, scaling off**.
4. **wedge, scaling on**.

**Set `slow_down_for_layer_cooling` off for all four**, or 2 and 4 will be byte-identical to 1 and
3 - see the blocker above. What to look for: on the dome's cap, where the bead is thinnest and the
Z axis moves most, whether the surface is smoother and the extrusion width more even; and whether
the thin end of any contoured run still shows the starved, under-filled look that motivated this.
A `zaa_min_z` of 0.08 is worth a fifth print at 0.2 mm: it halves the worst-case slowdown from 4x
to 2.5x and may be the better trade at this layer height.
