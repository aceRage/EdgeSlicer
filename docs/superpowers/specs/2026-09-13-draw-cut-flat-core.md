# Draw cut — the flat core, and the lip that reaches it

Branch `feat/draw-cut-flat-core`, off `feat/ultra-preferences`. Replaces the closed-loop
cut surface: what was a ruled strip swept along the drawn line is now a **band** plus a
**flat core plane**. Phases 1 and 2 stay the truth for an **open** line, which has no
interior and so no core to put anywhere.

## What was wrong

Owner click-test, 2026-09-13, a closed loop drawn round a cylinder. The preview showed a
translucent disc and a few cyan slivers of band, and the cut it produced was unusable:

> "Depth and Through all are mostly pointless and broken. The inner part of the cut should
> connect at a flat projected plane in the middle. Currently Depth projects outwards
> (wrong) and Through all projects through the object at random angles, producing a useless
> cut, not a clean projected normal to separate two parts. Angle should affect the angle at
> which the outer shell projects towards the inner flat projection. Extension mostly works."

The cause is the phase-1 model itself, not a bug inside it. Every loop point got a straight
ruling along **its own inward surface normal**. Round a cylinder those normals are radial,
so:

- the rulings all point at the axis, and `Depth` moved the rail along the ruling — on the
  outward side that reads as "projects outwards";
- `Through all` drove each ruling clean through the part at whatever angle its own normal
  had, so the far side of the cut was a scribble rather than a face;
- and **nowhere in the surface was there a flat region at all**, so the two halves had
  nothing to mate on. That is the thing that made the cut useless rather than merely ugly.

Split3r and the other model-splitting tools mean something quite different by this kind of
cut, and the owner named it as the reference: a drawn line on the skin says *where the cut
meets the surface*, the middle of the cut is a *clean flat plane*, and the skin-to-core
transition can be *angled* so the halves key together.

## The surface model

For a closed loop `L` on the mesh, in the cut plane's own frame.

### 1. The core plane `P`

Best-fit plane of `L`: **Newell** normal `n` over the loop, centroid `c` the mean of the
path points. Newell rather than a covariance eigenvector because it is the area-weighted
normal — a loop that wanders off its own plane still yields the normal of the plane that
best carries its enclosed area, and a nearly-degenerate loop degrades to a small vector
instead of an arbitrary axis.

With `Direction = Axis X/Y/Z` the chosen axis **replaces** the fitted normal (still through
`c`): the user is saying "make the mating face square to this axis", which is exactly what
a fit would not give on a hand-drawn wavy loop.

`n`'s sign is pinned to the loop's own surface normals, not to its winding. Newell's sign
*is* the winding, so without that pinning a loop dragged the other way round would flip
which half is "upper". `[DrawCut] the cut surface does not depend on which way the loop was
drawn` is the case that catches it.

The core is the region of `P` inside the loop's projection.

### 2. The skin band

For each loop point `p`:

```
p_out = p + extension * outward(p)            outside the skin
p_in  = project_onto_P( p + depth * d(p) )    the band's inner curve, ON P
```

- `outward(p)` is the sample's own surface normal — that is what "reaches out of the skin"
  means on a curved part — falling back to `-inward(p)` where that normal is useless.
- `inward(p)` is the in-plane inward direction: the component of `c - p` perpendicular to
  `n`. For a loop on a flat face that is "towards the centroid"; for a loop round a cylinder
  it is the radial direction. The same formula says both, because removing the component
  along `n` turns "towards the centre point" into "towards the centre **line**".
- `d(p) = cos(angle) * inward(p) - sin(angle) * n` — the band's travel direction.

### 3. `Angle` — the lip angle, 0..90, unsigned

The angle at which the outer shell projects in towards the flat core:

| Angle | Band | Result |
|---|---|---|
| `0` | perpendicular to `n`, in the plane through `p` parallel to `P` | a flat shelf around the core |
| `45` | half and half | a chamfered lip the halves key into |
| `90` | straight along `-n` | a straight-walled plug, no lip |

This is **not** the phase-2 draft angle. That one was signed (`-60..+60`) and rotated the
ruling towards the outward binormal; this one is unsigned and measures against the core
normal. They are different quantities, which is why the recipe version had to move (below).

For a closed loop the lip angle applies whatever the `Direction` is, because the band's
direction now comes from the core plane rather than from the surface normal. An open line
keeps the phase-2 rule (Surface normal only).

### 4. `Depth` — the band's inward travel

How far the band travels along `d(p)` before the surface **turns onto the core plane**.
The core polygon is the loop projected onto `P` and inset by the band's in-plane footprint,
`depth * cos(angle)` — so band and core join along one closed curve and the solid is
watertight by construction.

**Deeper means a smaller core.** That is the semantic the old `Depth` had backwards. At
`angle = 0` on a loop of radius 20, `depth = 3` gives a core radius of 17 and `depth = 10`
gives 10; `[DrawCut] Depth sizes the flat core` measures exactly that.

A depth past the loop's own in-plane radius insets the core past the centre and there is no
flat left. `draw_cut_split()` logs a warning naming the inset and the radius rather than
silently producing an inside-out core.

Default **3 mm** (was 10).

### 5. Why the core is flat even when the loop is not — and where it sits

Each `p_in` is **flattened onto `P` along `n`** at the end of the depth travel. However far
off-plane `p` was, `p_in` lands exactly on `P`. So a wavy hand-drawn line still yields a
flat mating face — which is the whole point, and what
`[DrawCut] a wavy loop round a cylinder gives two watertight halves with a flat core`
asserts to 0.05 mm.

**Which plane, though.** `P`'s orientation is the loop's fit, but its *position* is **not**
the plane through the loop's centroid — the band travels first. The core sits at the mean
arrival height, `centroid + mean((p_in - centroid)·n) * n`, i.e. `depth * sin(angle)` below
the drawn line.

Projecting the arrivals onto the plane through the loop centroid instead — the obvious
reading of "project onto `P`" — *undoes the band's travel*: on a loop round a cube's top
face with a 90° lip the band goes down by `Depth` and is then snapped straight back up to
the face, the plug collapses to zero height, and the boolean reports "the lower boolean gave
nothing". That was a real bug during this work, and it is why the position is derived from
the arrivals rather than assumed.

`draw_cut_core_face()` is the public accessor for the plane the mating face is actually on
(orientation plus position); `draw_cut_core_plane()` gives the fit and the loop's own
centroid. Tests that ask "is this vertex on the core?" must use the former. Through-all
returns false from `draw_cut_core_face()`, because there is no core at all.

Consequences worth stating, because each was a wrong expectation first:

- the lip angle never **tilts** the mating face — the normal is the loop's fit at every
  angle — but it does set the face's **depth**;
- at the same `Depth`, a 45° lip removes **less** than a 90° one (it reaches
  `depth * sin 45` down, not a full `Depth`); the chamfer is there to key the halves, not
  to remove volume;
- `angle = 0` on a **flat face** is a flat shelf at the skin, so the plug has no height.
  Useful cuts on a flat face want a positive angle; `90` reproduces the old straight-walled
  plug exactly.

### 6. `Through all`

No core plane at all: the cut is extruded all the way through the part, giving a tapered
plug/socket with no flat middle. The extrusion reaches `1.05 * bbox diagonal` **on both
sides**, so the cut is always complete rather than stopping inside the part.

Two details that are not the obvious reading, both of which were bugs first:

- **The extrusion axis is `n`, not `d(p)`.** Sweeping `±reach` along `d(p)` — the literal
  reading of "extrude the band along `d`" — is wrong, because `d` points mostly *inward*: at
  any useful reach both rings converge past the axis and cross over, and the "solid" is a
  self-intersecting bow tie. That is the same class of mistake as the ruled strip's (a
  per-sample direction used as a sweep) and it is what made through-all "project through the
  object at random angles". The rings travel `±reach * n`, and the angle tilts the **wall**.
- **Here the angle's zero is a straight prism**, not the flat shelf it is for the band: the
  lateral travel is `reach * tan(angle)`, so `0` is the plain through cut every earlier
  version made (and what the phase-1 suite asserts), and larger angles taper it. Reading it
  as `cot(angle)` to match the band would make `0` an infinite taper, which turned a plain
  through cut on a cube face into a cone.

The lateral travel is capped at 90% of the loop's smallest in-plane radius, so a steep taper
stops just short of inverting the ring rather than self-intersecting.

Default **off** (was on). On is the case that produced the owner's "projects through the
object at random angles"; off is the case that gives two halves that sit flat against each
other.

### 7. `Extension`

Unchanged in meaning, and the one parameter the owner said already worked: how far the band
reaches **outside** the skin. It only ever moves `p_out`.

### 8. One surface everywhere

The cutter solid, the inside field (halves colouring, Visible/Ghost/Hidden) and the
connector frames all read the same band-and-core surface, via `surface_frame_pieces()`. A
connector dropped on the band or on the core therefore stands on the face that will
actually exist. A divergence between the preview and the boolean is precisely the
translucent-disc symptom, so keeping these in one place is load-bearing rather than tidy.

**The invariant, stated plainly:** whatever ruling the cutter sweeps at a sample, the
surface queries must sweep the same one at that sample — `surface_frame_pieces()` (which
gives `draw_cut_surface_point/normal/frame`) and `draw_cut_surface_project()`'s `ruling_at`
both mirror `draw_cut_band_core_solid()`. In particular **through-all rules along `-n`,
not along `d(p)`**, so both query paths special-case it the same way the builder does.
Getting this wrong is not subtle in its consequences but is invisible in the geometry: a
connector placed at `w = 8` on a through-all cut at angle 0 landed 8 mm *radially inside
the plug* instead of 8 mm down its wall, and the hole and the plug stopped matching.

`draw_cut_surface_project()` is the inverse of `draw_cut_surface_point()`; if they sweep
different families of lines, a connector placed by a click lands off the surface it was
clicked on and the frame built there belongs to a surface that does not exist.

`Smoothing` smooths the loop and nothing else; `Edit points` is untouched.

## Code

| Thing | Where |
|---|---|
| `draw_cut_core_plane()` | `src/libslic3r/DrawCut.cpp` — Newell fit, axis override |
| `draw_cut_core_inward()` | in-plane inward direction at a sample |
| `draw_cut_band_dir()` | the lip angle, applied in one place |
| `build_core_band()` (file-static) | the band's two rings, kerf and normal pinning |
| `draw_cut_band_core_solid()` (file-static) | the cutter solid: band wall + core cap, or the through-all extrusion |
| `draw_cut_cutter_solid()` | dispatches closed → band+core, open → the phase-1 ruled strip |
| `surface_frame_pieces()` | the band direction for the surface/connector queries |

`DrawCutMinLipAngleDeg` / `DrawCutMaxLipAngleDeg` (0 / 90) replace `DrawCutMaxAngleDeg`
for the closed-loop path; the open-stroke ruled strip still uses the old `±60` constant,
because that path still has the old meaning.

## The recipe: version 3

Two stored numbers changed meaning, so this is a real version bump and not just new fields:

- `draw_angle_deg` — was a signed draft `-60..+60`; now the unsigned lip angle `0..90`.
- `draw_depth` — was a reach through the part (tens of mm, and usually ignored because
  `through_all` defaulted on); now the band's travel, a few mm.

A version 2 recipe **still loads**, but it cannot re-cut to the same halves, because the
surface it described no longer exists. `CutRecipe::migrate_draw_v2()` maps it onto the
nearest phase-3 cut, once, at the point of load in `bbs_3mf.cpp`, so nothing downstream has
to know two meanings:

- `angle := clamp(90 - |old draft|, 0, 90)`. `|draft|` is how far the old ruling leaned off
  the surface normal, and the phase-3 surface closest to that is a lip of `90 - |draft|`.
  An old angle of 0 — the ruling straight down the normal — becomes a 90° straight wall,
  which is exactly what that cut looked like on a flat face.
- `depth := 3` (the phase-3 default). An old depth is meaningless as a band travel and
  would inset the core past the loop's radius.
- `through_all := false`. Carrying the old default forward would faithfully reproduce the
  broken cut.

The halves themselves are untouched until the user re-cuts, because those come from the
stored mesh blob.

The 3MF reader defaults for `draw_through_all` / `draw_depth` are the **old** file's
meanings when `version <= 2`, so a missing attribute and a present one both go through the
migration exactly once.

## Tests

Final state: **`[DrawCut]` 68 / 68**, **`[CutRecipe]` 14 / 14**, **`[CurvedCut]` 50 / 50`**.
The 68 is 53 existing plus 15 new. `[CurvedCut]` is untouched by this work and stays fully
green, which is the check that the shared boolean path was not disturbed.

15 new `[DrawCut]` cases - the 13 below, plus the two separation cases described at the end:

| Case | What it pins |
|---|---|
| the core plane fits a wavy loop and is flat | Newell fit survives a ±5 mm wave |
| Axis direction forces the core normal to that axis | the axis override, centroid unchanged |
| the band direction is the angle between inward and the normal | `draw_cut_band_dir` at 0/45/90, and its clamp |
| a wavy loop round a cylinder gives two watertight halves with a flat core | **the headline case**, coplanar to 0.05 mm |
| Depth sizes the flat core | core radius 17 at depth 3, 10 at depth 10 |
| the band's inward slope is the angle | slope within 2° at 0/30/45/60 |
| Through all spans the part and leaves no flat core | no core face, band spans full height |
| a circle on a cube face cuts a plug with a flat bottom at depth | the plug case, flat bottom at `depth` |
| the angle keeps the core square to the loop and sets its depth | normal fixed, depth `= depth·sin(angle)` |
| the cut surface does not depend on which way the loop was drawn | winding independence of the new surface |
| connectors stand on the band and on the core | project inverts point; frame ⊥ band |
| an open line still uses the ruled strip | no core plane for an open stroke |
| the inside field follows the band and core surface | preview agrees with the boolean |

4 new `[CutRecipe]` cases cover the v2 → v3 migration: the mapping itself, the angle mapping
across several v2 values, that a flat/curved recipe is left alone, and that migrating a
current recipe is a no-op.

The existing cases that asserted the old ruled-strip **closed loop** surface were changed as
follows, and why is recorded in the commit:

- `Draw cut: a positive angle flares the plug into a frustum` and `a negative angle
  undercuts the plug` measured the **signed draft** on a closed loop against a closed-form
  frustum. The lip angle is a different quantity on a different surface, so a frustum is no
  longer the shape that cut makes. The draft itself is not gone — it is still exactly what
  an open stroke's ruled strip does — so the measurement is **retargeted** at that surface
  as `a draft angle tilts an open stroke's ruled strip` and `a negative draft leans an open
  stroke's strip the other way`. The closed-loop replacements are `the band's inward slope
  is the angle` and `the angle keeps the core square to the loop and sets its depth`.
- Four cases assert a closed loop on a cube's **flat top face** cutting *straight down* —
  the ruled strip's behaviour there. Under phase 3 that is the `angle = 90` case exactly, so
  each keeps every assertion and simply sets `params.angle_deg = 90.0` with a comment saying
  why: `Depth stops the cut short of through-all`, `the surface point and frame agree with
  the cutter`, `the surface tilt reads the strip's own normal`, and `the plug does not depend
  on which way the loop was drawn`. At the phase-3 default of `0` the band runs horizontally
  along the face instead — a flat shelf, which is a different and also correct surface, but
  one that removes no material on a flat face and so has no plug to measure.

Three structural cases that test `draw_cut_inward_dir()` directly — `angle 0 is bit-for-bit
the phase 1 cut`, `the angle only applies to Surface normal`, `the angle rotates the ruling
toward the outward binormal` — are **unchanged and still green**: that function is still the
open stroke's ruling and still has the old meaning.

### Separation versus plug — how a wrap-around loop is told apart

A loop drawn **all the way round** the part is not a plug: a prism through it contains the
whole object, so the intersection is everything and the complement nothing ("the upper
boolean gave nothing"). What the user drew is a **separation**, and the halves are the two
sides of one tapered wall.

`draw_cut_loop_separates()` decides which, **from the section, not the bounding box**:

1. slice the mesh with the core plane `P` (the mesh is rotated so `n` is `+Z` and sliced at
   `z == 0`);
2. project the loop onto `P`;
3. intersect, and compare areas: if **more than 90% of the section area lies inside the
   loop**, the loop contains the part and therefore wraps it; otherwise the loop lies within
   the section and it is a plug on the skin.

A bounding-box test was tried first and **rejected**: a cylinder's bbox corners are at
`r·√2`, further out than any loop drawn on the barrel, so "the part reaches outside the
loop" fired on ordinary plug loops and broke four passing cases. The section is the actual
material at that height, so a plug loop projects strictly *inside* it and a wrap-around loop
strictly *contains* it — there is no case in between to tune, which is what makes this
robust where the corner test was not.

Under `Through all` the two then build different solids:

- **wrap** → the tapered wall runs from the drawn line along `-n` only, through the part and
  out the far side, capped beyond the bbox: the solid is "everything below the wall", so the
  halves are the two sides of one wall;
- **plug** → the prism through both sides, as before.

`draw_cut_cutter_solid()` takes an optional `mesh` for this; `draw_cut_split()`,
`draw_cut_empty_sides()` and the gizmo's preview all pass it, so the preview and the cut
agree. Without a mesh the plug reading is kept, which is the harmless default.

**Note the angle's zero differs between the two paths**, and deliberately: with a core plane
`0` is the flat shelf and `90` the straight wall; with `Through all` there is no shelf to lie
in, so `0` is the straight prism and larger angles taper it. Reading the through-all angle
the band's way would make `0` an infinite taper, which is how a plain through cut on a cube
face once turned into a cone.

Tests: `a loop round the part separates it, a loop on a face does not` pins the predicate on
four cases (barrel loop, small loop on the cylinder's top, cube-face loop, open line), and
`Through all round a cylinder cuts it in two` pins the cut it produces - a loop round the
middle of a 20x60 cylinder splits it 37680 / 37688 mm3, which is 50/50 to four significant
figures.

**A bounding box cannot check a separation, and this cost a round of wrong assertions.** The
cutter is a half-space, so one half is a clean slab (`z` from the cylinder's bottom up to the
line) but the *other* is the remainder, and the remainder's bounding box spans the whole part
however it was cut. Asserting the two bboxes do not overlap asks the complement to be
something it never is. What the tests check instead is the slab's own extent plus the two
volumes. The same applies doubly to a wavy loop, where the wall is wavy and the halves
interleave by the amplitude at their boundary by construction.

---

# Phase 3b — what the real cylinder showed

Branch `fix/draw-cut-real-cylinder`, off `feat/ultra-preferences` at 04571b3426. Second
owner click-test, 2026-09-13, on `tests/data/cylinder_drawcut.3mf`: a wavy loop all the way
round a ~78 mm barrel, Depth 3, Angle ~30, Extension on, Through all off (and once on).

The `[DrawCut]` suite was 68/68 green throughout. **It was green because none of its
fixtures resembled what the gizmo makes.** Every closed-loop fixture was 96 clean samples at
an exact radius, with a symmetric wave that cancels, on a mesh built in the cut plane's own
frame. The gizmo hands DrawCut a few hundred samples at a millimetre spacing, each landing
on a facet with raycast noise on it, on a mesh that has been through an instance transform
(the 3mf instance carries a 2.88 uniform scale, so the part is r 38.9 / h 77.8, not the
r 13.5 stored in the file), with a dent in the line and a wave that does not cancel.

Five symptoms, five separate causes.

## 1. The core plate with a pie wedge missing

The core was a TRIANGLE FAN from the mean of the inner ring. A fan is a valid triangulation
only of a polygon that is **star-shaped about the fan centre**. Travelling `depth*cos(angle)`
inward from a loop with a 4 mm dent pulls the dent past its neighbours and the inner ring
locally self-touches; the fan triangles over that stretch come out zero-area or wound
backwards, and the winding repair at the end of the builder then reads that as material
missing. On the idealised fixtures the inset ring is always a convex near-circle, so a fan is
always correct there and the bug cannot appear.

`build_core_plate()` replaces it: project the inner ring onto the core plane, make it simple
with a **non-zero union** (Clipper resolves the self-touch rather than folding over it), keep
the **largest contour**, and triangulate the interior with the codebase own tesselator. The
plate hands back both its triangles and the ring they are bounded by, and the band is stitched
to *that* ring - two closed curves of different lengths walked together by in-plane angle - so
the join is one shared curve by construction.

Two traps inside that, both of which cost a build:

- `triangulate_expolygon_2d()` returns **unscaled millimetres**, unlike the `Polygon` it is
  given. Unscaling again put the whole plate within a micron of the origin, and the vertex
  dedup then collapsed every triangle: 0 plate triangles, 63 open edges;
- the plate boundary vertices must be the **same indices** the band was stitched to, and its
  winding must be the **reverse** of the stitch traversal of that ring. The stitch may have
  reversed the ring to agree with the outer one, so which way the plate faces follows that
  reversal rather than being fixed.

Measured: core area 4156 mm2 against 4143 expected (0.3%), for a loop of r 38.9 inset 2.60.

## 2 and 3. The thin cyan ring, and "the stroke does not separate the part"

One cause. A band-and-core surface is an open dish, and it was **always** closed into a plug -
cap the outer ring over the top. For a loop drawn all the way ROUND the part that dish spans
the whole section at that height, so capping it makes a **plate lying across the part**, not a
plug in it. The intersection of a plate with a cylinder is a thin slab (the cyan ring, its
jagged edge being the band own facets), and the complement of a slab out of the middle of a
cylinder is ONE connected piece - so nothing was separated and the panel said exactly that.

The band-and-core path now asks `draw_cut_loop_separates()` the same question Through all was
already asking, and closes the dish **downwards** for a wrap: a skirt from the outer ring along
`-n` past the part, capped. The solid is then "everything below the drawn surface", the
intersection is the lower half bounded above by the band and the core, and the complement is
the upper half.

Measured on the fixture: 178136 / 192067 mm3, i.e. 48.1 / 51.9 for a loop at mid height.

Watertightness of a two-ring stitch is entirely a question of edge DIRECTION, not edge count.
The band stitch walks the outer ring as `O(i+1) -> O(i)`, so the plug cap and the wrap skirt
must both walk it as `O(i) -> O(i+1)`; getting the skirt backwards left 492 open edges on a
246-sample loop while `its_num_open_edges()` counted the ring twice and every boolean refused
the solid.

## 4. "The line turns tighter than the cut surface reaches sideways"

`draw_cut_strip_folds()` is a test about the **ruled strip**, which a closed loop has not used
since phase 3. `draw_cut_split()` already skipped it for closed strokes; the **gizmo ran it on
every stroke**, so the preview warned about a fold the cut did not have.

And on a dense stroke it does not measure the line shape at all. Discrete curvature from three
consecutive samples is `4*area/(|ab||bc||ca|)`, which on samples a millimetre apart with a few
hundredths of a millimetre of raycast noise reads several 1/mm - the **jitter** curvature.
Times any Extension over 1 mm and the warning fires on any hand-drawn line whatever its shape.

`draw_cut_band_folds()` asks each kind of stroke the question its own surface can fail. A
closed loop can only over-inset: the band travels `depth*cos(angle)` toward the loop axis, so
it folds when that reaches the loop own in-plane radius - measured at the **5th percentile** of
the sample radii, not the minimum, so one stray sample or the bottom of a deliberate dent does
not condemn a loop that is 38 mm everywhere else. An open stroke still gets the strip test, on
a **smoothed** copy of the path for the same jitter reason.

The panel wording splits with it. Telling someone whose core has been eaten to "reduce the
Angle" sends them the wrong way: the inset is `depth*cos(angle)`, so a **larger** angle insets
less.

## 5. The hourglass — and the decision about Through all

The through-all wall leaned inward by `reach*tan(angle)` per side with `reach` a whole bbox
diagonal, so at any angle past a couple of degrees the lateral travel was tens of millimetres
on a part tens of millimetres across. The inset went through zero, the ring inverted, and the
result was two cones meeting at a point. The 90%-of-min-radius cap was meant to prevent it,
but a cap taken from ONE number cannot serve a loop whose radius varies: with a dent the min_r
is nothing like the radius elsewhere, so the cap either failed to bite or crushed the ring.

**DECISION, made for the owner.** Through all means the loop extruded **straight along the
core-plane normal** through the whole part in both directions. No taper, so no convergence and
no hourglass. **Angle and Depth are ignored** and the gizmo greys both out while it is ticked -
greyed, not hidden, so the panel does not jump a row and the user can still see the values they
get back on unticking.

What the decision does *not* remove is the wrap/plug question, and that is the easy thing to
get wrong here. The taper is why the old wrap solid **leaned**; it is not why a wrap needed a
different solid. A prism through a loop that goes right round the part **contains the whole
part**, tapered or not, so its intersection is everything and its complement nothing. A wrap
still gets a half-space: the same straight wall run one way only, capped beyond the part. A
plug gets the prism, both ways. `draw_cut_loop_separates()` therefore stays, and is now read by
both surfaces rather than only by Through all.

Measured: wall radius 38.87 .. 38.98 for a loop of r 38.92 (a prism, to a tenth of a
millimetre), and a split of 185112 / 185092 mm3 - 50.0 / 50.0.

## What the tests are now

Six cases built from the 3mf, running the functions the gizmo runs in the order it runs them:
`Model::read_from_file` (with `LoadStrategy::LoadModel`, without which the importer drops every
object and hands back an empty Model), the instance transform, a 400-sample loop with a 4 mm
dent and 0.05 mm of jitter projected onto the mesh, `DrawCutChain::finish()`, and the gizmo
re-projection afterwards.

A trap in the fixture itself, worth recording because it is a segfault rather than a wrong
number: **`AABBMesh` keeps a raw pointer to the mesh it was built from**, so
`AABBMesh aabb{ TriangleMesh(cyl) }` binds to a temporary that dies immediately, and every
query after it reads freed memory.

One assertion measured less than it should have, and was wrong to: the wave arriving at the
SKIN came out damped by `E / (E + inset)`, and the first version of this section wrote that up
as geometry to be asserted rather than as a bug. It is a bug. See **Correction - the drawn
line has to BE on the surface** below, which is where that is fixed and where the test now
asserts the full drawn wave at two very different Extensions.

## Correction — the drawn line has to BE on the surface

The first pass at symptom 2 left a damping artefact and the write-up above accepted it as
geometry. It is not; it is the same bug one layer down, and the owner was right to send it
back.

**The claim that was wrong:** "the wave that survives to the skin is `(1 - E/(E+inset))` of
the drawn amplitude, because the band interpolates from the outer ring to the flat inner
one." That is an accurate description of what the code did and a wrong description of what
the code should do. The drawn line is *where the cut meets the skin* - that is the entire
contract of a draw cut - so the fraction is not a number to be predicted and asserted, it is
1, and any dependence on Extension at all is the bug.

**The cause.** The band was ONE loft, from the skirt tip `p + E*outward` straight to the
inner ring `project(p + depth*d)`. The drawn line `p` was not a vertex of that surface at
all - only a point the loft passed near - so the skin crossing landed part-way along it and
carried a correspondingly part-way height. More Extension, more damping; at E 15 almost the
whole wave was gone.

**The fix: three rings, not two.**

```
out  -> line     the SKIRT, outside the skin
line -> inner    the BAND, skin down to the core at the lip angle
inner            the CORE PLATE
```

`line` is the drawn samples themselves (kerf-shifted), so the surface passes through them
exactly. `out` and `line` share a count and an order, so the skirt is a plain 1:1 quad loft;
`line` and the plate ring do not, so that join keeps the angle walk. The inner ring is the
inset of the **line**, never of the extended ring, so Depth means the same thing at every
Extension.

**Which way the skirt goes**, since the brief allowed either: it CONTINUES THE BAND'S OWN
SLOPE, i.e. `-d`, not the skin normal and not flat in the core plane. Two reasons pointing
the same way:

- *No crease.* `-d` is the band ruling run backwards, so skirt and band are one straight line
  through `p` and the surface is C1 across the drawn line. A skirt meeting the band at an
  angle puts a crease exactly on the skin, which is the worst place for a boolean to find two
  nearly tangent faces.
- *The smaller lateral reach*, which is what folds an outward offset on a concave stretch such
  as the owner's dent. Out along `-d` moves the rail sideways by `E*cos(angle)`; out flat in
  the core plane moves it by the whole `E`. `cos(angle) <= 1` always, so this is the strictly
  safer of the two at every angle and identical to it at 0. (At the dent - turn radius ~25 mm -
  even E 15 gives `13 * 0.04 = 0.52`, comfortably under the fold threshold of 1.)

**A latent bug this closed on the way past.** `surface_frame_pieces()` and
`draw_cut_surface_project()` already ruled along `d` from the drawn line with
`w` in `[-extension, +depth]` - i.e. they already described the three-ring surface. It was the
*builder* that disagreed with them, so a connector placed at `w = -2` stood on a surface the
cutter did not build. The two now agree by construction. Both query paths also had Through
all still leaning by `tan(angle)`, left over from the tapered wall that the hourglass fix
removed; they now sweep straight `-n` the way the builder does.

**What the test asserts now.** The same case at Extension 5 **and** Extension 15: split the
part, take the highest skin vertex of the piece below the line in each of 180 fine bins, and
require it to be within 0.3 mm of `z0 + 2.5*sin(2*theta)` - the drawn wave itself - with a
peak-to-peak of 5.0 mm and coverage over at least 30 of 36 coarse bins. Running both
Extensions is what makes this a test rather than a tuned number: a damped surface cannot pass
both, and the old code passed neither.

**The instrument needs as much care as the surface, and got it wrong first.** Comparing each
bin's maximum against the wave at the BIN'S CENTRE angle - the obvious way - builds in an
error of its own: the maximum inside a bin sits wherever the wave is highest within it, so on
a rising stretch it is at the bin's far edge, and `2.5*sin(2*theta)` moves `5 * (pi/36)` =
0.44 mm across half a 10-degree bin. That is the entire tolerance, spent on the measurement.
The comparison is therefore made at each kept vertex's OWN theta, and the bins decide only
which vertex to look at and whether the boundary was found all the way round. (A first repair
- scoring only vertices that are the highest within a few degrees - failed the other way: on a
wave that steep only the four crests qualify, so coverage collapsed to 3 bins of 36.)

Measured after the fix: worst deviation **0.021 mm** at both Extensions, which is the same
0.021 mm the DRAWN LINE itself deviates from the ideal wave. The surface contributes
essentially nothing; the cut meets the skin on the drawn line.
