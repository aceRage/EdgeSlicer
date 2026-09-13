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

Final state: **`[DrawCut]` 64 / 66**, **`[CutRecipe]` 14 / 14**, **`[CurvedCut]` 50 / 50**.
The 66 is 53 existing plus 13 new; the 2 failures are the documented wrap-around gap at the
end of this section. `[CurvedCut]` is untouched by this work and stays fully green, which is
the check that the shared boolean path was not disturbed.

13 new `[DrawCut]` cases:

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

### Two cases that do NOT pass, and why they are a known gap rather than a regression

`Draw cut chain: a tall box looped in three strokes cuts two watertight halves` and
`Draw cut chain: Close loop joins along the SURFACE, never through the part` both draw a
loop **all the way round** a tall box and cut it with `Axis Z` + `Through all`, expecting the
box to come apart into a top half and a bottom half of roughly equal volume.

That is a **separation**, not a plug, and phase 3's through-all cannot express it. Its cutter
is a prism along `n` through the loop; when the loop encloses the whole part, that prism
contains the entire object, so the intersection is everything and the complement is nothing —
`draw_cut_split()` reports "the upper boolean gave nothing". The ruled strip could do this by
accident, because its rulings pointed inward from the loop and swept a horizontal slab.

The honest fix is a **half-space cutter for a wrap-around loop**: when the part reaches
outside the drawn line, the cut is "everything below the line" rather than "everything inside
it". A bbox-corner test for that condition was tried and rejected — a cylinder's bbox corners
are further out than its own radius, so the heuristic fired on ordinary plug loops and broke
four cases that had been passing. Distinguishing the two properly wants the mesh's own extent
in the core plane, not the bbox's, and that is a piece of work in its own right.

Until then: a wrap-around loop wants `Through all` **off**, where the band-and-core path
handles it correctly (`a wavy loop round a cylinder gives two watertight halves with a flat
core` is exactly that case, and it passes). The two cases above are left failing rather than
deleted or weakened, because they describe a cut the feature should support and the next
piece of work on this is to make them pass.

Everything else in the 53 stays green: the resampler, smoothing, the chain, the editing,
the errors, the open-stroke cuts and the connector contract are all unchanged by this.
