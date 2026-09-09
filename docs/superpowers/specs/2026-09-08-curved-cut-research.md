# Curved cut-plane tool — research + design

Status: research only, no code. Branch `research/curved-cut`, based on `origin/feat/ultra-preferences`.

## Recommendation (read this first)

**Phase 1 should be (b), a height-field `z = f(u,v)` over the base cut plane, driven by a coarse
control-point grid (e.g. 8x8, upsampled to a 64x64 sample mesh for cutting/rendering), with
grab-style dragging and a Smooth pass borrowed from `GLGizmoSculpt`'s brush mechanics.** This
validates the task author's hypothesis. Reasoning grounded in what's in this codebase:

- The existing flat cut is not a boolean at all — `Cut::perform_with_plane` calls
  `cut_mesh(mesh.its, 0.0f, &upper_its, &lower_its)` (`TriangleMeshSlicer.cpp:2323`), a direct
  half-space slice of the triangle mesh in cut-plane-local space. A height-field is the smallest
  generalization of "half-space" that keeps a single-valued, self-intersection-free separating
  surface — i.e. it's the one representation that could *also* reuse a slicing-style algorithm
  instead of forcing a full boolean immediately, and it degrades exactly to today's flat cut when
  `f(u,v) = 0`. That degeneracy is a testable invariant (see Test plan).
- A height-field is trivially watertight and self-intersection-free by construction (function of
  two variables), which the general "subdivided mesh dragged like a sculpt brush" (a) is not —
  dragging control points on a free-form quad mesh can fold the sheet over itself, and the
  codebase has no mesh self-intersection repair step in the cut path today (the groove cut avoids
  this entirely by chaining flat cuts; the flexi-joint path uses `MeshBoolean::mfd::make_boolean`
  with `mcut` fallback, at `CutUtils.cpp:337-361`, but callers there hand it two already-clean
  closed solids, not an interactively-edited free sheet).
- (c), surface offset of the model, is real and useful (a "peel a shell of thickness d" mode is a
  legitimate, orthogonal feature) but it is *derived from the model*, not from user intent, so it
  answers a different question than "let me sculpt a mating surface." Recommend it as phase 3, a
  separate mode, not the phase-1 representation.
- (d), fit-through-picked-points, is the most expensive to get robust (needs a smooth
  interpolation/approximation solver, degenerate-input handling, no natural low-effort UI
  precedent in this codebase) and is best layered on top of (b)'s control grid later (a "fit
  control points to these picked points" solve), not built standalone first.

The grid should be **stored as control points + degree/spacing, resampled to a dense mesh only
for preview/cut**, mirroring how `GLGizmoSculpt` treats brush state as ephemeral GUI/session data
layered on top of the persisted mesh.

---

## 1. Representations for a curved cutter

| | Data structure fit | Self-intersection risk | Rendering fit | Editing model |
|---|---|---|---|---|
| (a) subdivided plane mesh + grab/smooth | Needs a generic mesh (`indexed_triangle_set` / `GLModel`), same class of object `GLGizmoSculpt` already edits | High — free vertex drag can fold the sheet; no repair step exists in the cut path | Reuses `GLModel::init_from`/update path used by `m_plane.model` in `GLGizmoCut3D` | Direct vertex drag, exactly like Sculpt's Grab brush |
| (b) height-field `z=f(u,v)` (bicubic/B-spline over an NxN control grid) | Small (`(N+1)^2` doubles), independent of cutter resolution; separate from the base mesh entirely | None by construction (single-valued function) | Sample to a dense quad grid, feed to `GLModel::init_from` exactly like today's flat `m_plane.model` (`GLGizmoCut.cpp:1888-1895`) | Drag a sparse control-point handle, like Sculpt's grab but constrained to move only in local Z and only a control point, not raw mesh verts |
| (c) offset of model's outer surface at distance d | Computed once from the model's mesh (e.g. via `MeshBoolean`/distance field or existing offset utilities), not authored | Can self-intersect at concave regions with small radius of curvature (classic offset-surface problem) | Needs a dense mesh generated from the model, more expensive per-frame than (b) | No manual control; single slider `d` (+ optional Smooth) |
| (d) surface fit through picked points (RBF / least-squares) | Needs a scattered-data fit; no existing utility of this kind found in this codebase (search for "cut_mesh"/"its_split"-style helpers turned up only the flat/groove slicer path, nothing generalized) | Depends entirely on fit quality/regularization; can ring or overshoot away from points | Same sampling path as (b) once fit | Point-and-click on the model surface, more clicks for more control |

Note: I searched for "cut_mesh", "its_split" and similar mesh-splitting helpers as instructed;
the only hits are `TriangleMeshSlicer.cpp`'s `cut_mesh` (flat z-slice) and its callers in
`CutUtils.cpp`. There is no existing generalized "split mesh by arbitrary surface" utility to
reuse — a curved cutter is new machinery either way, which is a point in favor of the simplest
representation, (b).

## 2. Boolean mechanics

Today's flat-plane cut sidesteps booleans entirely: `cut_mesh(mesh.its, 0.0f, &upper, &lower)`
slices at `z=0` in cut-space and caps both pieces (`triangulate_caps` param). A curved cutter
cannot do this in one pass because the separating surface is not planar, so the model's own
triangle stream can't be split by a single scalar comparison per-vertex the way `cut_mesh` does —
we need a genuine closed cutting volume and two booleans, the same pattern `flexi_boolean` already
uses (`CutUtils.cpp:337-361`: Manifold first via `MeshBoolean::mfd::make_boolean`, mcut fallback
via `MeshBoolean::mcut::make_boolean`, "the same chain the Mesh Boolean gizmo uses").

Proposed mechanics:
1. Build the height-field surface as a dense quad grid (e.g. 64x64) in cut-plane local space,
   bounded to (a superset of) the object's footprint under the cut plane.
2. Thicken it into a slab: duplicate the sheet offset by ±`bbox_diag * small_margin` along local Z
   (not the field's own normal, to avoid slab self-intersection on steep slopes), and close the
   sides/ends against the object's bounding box extent — i.e. cap it well outside the object so
   the slab is guaranteed watertight without touching object geometry.
3. Two booleans against the object mesh: `upper = A ∩ (space above slab)`, `lower = A ∩ (space
   below slab)`, or equivalently `upper = A − slab`, `lower = A ∩ slab`, run through the same
   Manifold→mcut chain as `flexi_boolean`.
4. Reuse `MeshBoolean::mfd::make_boolean` conventions (its signature already returns a vector of
   `TriangleMesh`, merged the way `flexi_boolean` merges `dst` back into one mesh).

Concerns to flag explicitly:
- **Watertightness of the slab**: the height field is open (a sheet), so it must be closed off
  with side skirts + a far cap before it's booleanable; a naive "just offset the sheet" will leave
  gaps at the boundary unless the two offset sheets are stitched with a rim.
- **Performance for 64x64**: 4096 quads / ~8k triangles per sheet, ~16k triangles for the closed
  slab — small relative to typical print meshes; the boolean cost is dominated by the object mesh,
  not the cutter, so this should be comparable to today's flexi-joint booleans in cost class.
- **mcut fallback**: same integration point as `flexi_boolean`; no new fallback plumbing needed,
  just call through the same helper (or extract `flexi_boolean` into a shared utility, since two
  call sites will now want "Manifold-then-mcut boolean with logging").
- **Verification**: `upper.volume() + lower.volume() ≈ original.volume()` within tolerance is both
  a correctness signal at runtime (could gate whether to accept a cut) and the primary unit test
  (see section 7).

## 3. Connectors on a curved cut

Today, `CutConnector::pos` + `CutConnector::rotation_m` are defined in the flat cut plane's own
frame, and every consumer assumes one global `m_rotation_m`/`m_cut_normal` for the whole cut
(`GLGizmoCut.cpp:3520`: `translation_transform(cur_pos) * m_rotation_m` used identically for every
connector regardless of its `pos`). For a curved cutter, each connector needs its own **local
frame**: origin = `pos`, normal = `∇f` at `(u,v)` (i.e. the height-field's local surface normal,
not the constant plane normal `m_cut_normal`), with an in-plane basis parallel-transported from
the global cut-plane axes for a stable tangent direction.

By connector kind:
- **Plug / Dowel**: survive as-is. They're solids of revolution around their local Z; swapping the
  flat plane's constant normal for the per-point surface normal in the placement transform is a
  local, mechanical change (same `translation_transform(pos) * rotation_m` shape, just with
  `rotation_m` built from the local normal instead of the shared `m_rotation_m`).
- **Flexi joints**: survive with a local frame. `FlexiJointParams` and the footprint-corner logic
  in `is_outside_of_cut_contour` (`GLGizmoCut.cpp:3449-3474`) already build the joint's footprint
  in its own local 2D frame before transforming by `translation_transform(cur_pos) * m_rotation_m`
  — substituting a per-connector local rotation is the same shape of change as for Plug/Dowel.
- **Snap / Hinge / thread-like connectors**: **flag as risk.** These likely assume a locally flat
  mating patch larger than a single point (a hinge's knuckle run, a thread's pitch line) — if the
  surface curvature is non-negligible over the connector's own footprint, the connector geometry
  itself (generated as if for a flat plane) will not sit flush against the curved cut. Mitigating
  this needs either (a) restricting these connector kinds to control points where local curvature
  is below a threshold, or (b) deforming the connector body to match local curvature — materially
  more work, likely out of scope until a later phase.

**Out-of-contour check**: today's check projects a connector footprint through the *single* clip
plane via `m_c->object_clipper()->is_projection_inside_cut(vertex)` — a 2D contour test in one
plane's frame. For a curved cutter this needs to become "is `(u,v)` inside the base plane's 2D
footprint AND is the connector's local patch not exceeding the sheet's parametrized domain,"
i.e. the contour test moves from 3D-point-vs-clip-plane to a `(u,v)` domain membership test against
the height-field's control grid extent, still followed by the existing bounding-box/conflict
checks in `is_conflict_for_connector`.

## 4. UI

In `GLGizmoCut3D`, add a **Surface: Flat / Curved** mode toggle alongside the existing plane
rotate/translate controls (kept as-is — they position/orient the *base* plane the height field is
defined over). For Curved mode:
- **Subdivision control**: grid resolution slider, 4..64 control points per side (separate from
  the 64x64 dense sample mesh used for cutting/rendering — coarse control grid, dense cut mesh).
- **Draggable control points with falloff**: reuse `GLGizmoSculpt`'s established interaction
  vocabulling — `m_cursor_radius`, `m_falloff` toggle (`GLGizmoSculpt.hpp:120,122`), and its
  drag-plane projection helper (`project_on_drag_plane`) — but constrain drag to local Z of the
  base plane rather than free 3D, since displacement is `f(u,v)` not a free vertex position.
- **Smooth button**: same role as `GLGizmoSculpt`'s `Brush::Smooth`
  (`GLGizmoSculpt.hpp:63`) applied to the control grid.
- **"Fit to surface at offset d" button**: switches to mode (c) — computes control-grid heights
  from the model's own surface at the given offset, still edited through the same control-point
  UI afterward (so (c) becomes "an alternate initializer for (b)'s grid" rather than a fully
  separate code path — recommended way to phase it in given the shared editing/rendering pipeline).
- **Reset**: zero all control points, which per the phase-1 invariant reproduces today's flat cut
  exactly.

**Persistence**: recommend baking the cut and storing nothing extra in the 3MF, matching how flat
plane cuts work today (the cut is destructive — `perform_with_plane` replaces `m_model.objects`
with upper/lower results; there is no "flat cut plane" record kept in the 3MF for later editing,
only connector data persists via `cut_information.xml` per the `Model.hpp` comment on
`CutConnectorType::FlexiJoint`). Persisting the live control grid would be a bigger, separable
feature (re-editable parametric cut) — worth flagging to the task owner as a possible phase 4 but
not assumed here.

## 5. Slicing implications

No change to slicing geometry itself — once the cut is performed, the resulting upper/lower mesh
pieces are ordinary triangle meshes fed into the normal pipeline, same as today's flat or groove
cuts. The one implication is **orientation/support guidance**: a curved mating face is much more
likely to need supports or a specific print orientation than a flat one, since it isn't
guaranteed to lie flush on the print bed either half. I did not find anything in `CutUtils.cpp`
that gives the dovetail/groove cut special support or orientation handling — the groove cut's
angled flap faces (`groove.flaps_angle`, `CutUtils.cpp:792-808`) are handled purely by chaining
additional flat cuts, with no supports-specific logic visible in this file. This suggests parity
is acceptable for phase 1 (no new supports machinery), with a UI hint ("this face may need
supports") as a reasonable low-cost addition once curved cuts exist.

## 6. Phasing

- **Phase 1**: height-field sheet (control grid + dense sample mesh) + control-point drag/Smooth
  (Sculpt-style) + two-boolean cut via the Manifold/mcut chain. No connectors — curved cuts start
  connector-free, matching how the flat cut shipped before connectors existed.
  - Effort: medium. New GLModel generation/update path (parallel to `update_plane_model`), new
    slab-construction + boolean call, new control-grid data structure and drag interaction.
  - Risks: Manifold behavior with a thin slab against thin-walled or non-manifold input models;
    degenerate sheets (all control points collapsed to a line, or extreme local slope causing the
    slab to self-intersect); undo/redo granularity for grid edits (should each drag-tick be one
    undo step, or coalesce a drag gesture into one, matching how `GLGizmoSculpt` presumably
    snapshots per-stroke rather than per-frame — verify against its undo/redo hookup before
    committing to a granularity).
- **Phase 2**: connectors with local frames (Plug/Dowel/Flexi first, per section 3), plus the
  `(u,v)`-domain out-of-contour check.
  - Effort: medium — mostly plumbing a per-connector local frame through existing connector
    placement/rendering code that currently assumes one shared `m_rotation_m`.
  - Risk: Snap/Hinge/thread connectors on curved patches (flagged in section 3) may need to be
    disallowed or restricted rather than fully supported in phase 2.
- **Phase 3**: offset-of-surface fit (mode c) and picked-point fit (mode d), both as alternate
  initializers into the same control-grid editor from phase 1.
  - Effort: offset-of-surface is medium (needs a robust offset/distance computation against the
    model mesh); picked-point fit is higher (needs a real scattered-data solve with
    regularization).
  - Risk: offset surfaces self-intersecting at concave curvature; picked-point fits ringing badly
    with few/clustered points.

## 7. Test plan

**Unit tests** (mirroring the existing Catch2 suites under `tests/libslic3r/`):
- Boolean correctness: `volume(upper) + volume(lower) ≈ volume(original)` within a relative
  tolerance (e.g. 1e-3), for at least a cube and a non-trivial mesh, across several sheet shapes.
- Flat-cut equivalence: a height-field sheet with all control points at zero displacement produces
  triangle-identical (or bit-for-bit within floating point epsilon) output versus today's
  `cut_mesh`-based flat plane cut — this is the phase-1 invariant claimed in the Recommendation.
- Sampling correctness: for a domed sheet with known control points, sample the resulting cutting
  surface at a grid of `(u,v)` and confirm heights match `f(u,v)` within tolerance (validates the
  bicubic/B-spline evaluation independent of the boolean step).

**Demo**: cut a cube with a domed sheet (single raised center control point, e.g. a Gaussian-like
bump), export both halves as STL, visually confirm a smooth dome-shaped mating face on both
pieces and that they nest back together.

**Owner click-test**: load a simple box-like model into the Cut gizmo, switch Surface to Curved,
drag the center control point upward to create a dome, click Smooth once, perform the cut with
both Keep Upper/Keep Lower checked, confirm two solid halves appear with a visibly curved,
matching mating surface, and that dragging one half's curved face against the other by eye shows
them fitting together (no connectors needed for this click-test, per phase 1 scope).

---

## External references

(General knowledge, not fetched live — flag accordingly rather than treating as verified quotes.)

- **Meshmixer** — "Plane Cut" tool (flat, connector-free split) vs. its sculpting "Bend"/"robust
  smooth" brush tools, a similar flat-vs-freeform-deform contrast to what's proposed here.
  https://www.meshmixer.com/
- **Blender** — "Knife Project" (silhouette-projected cutting curve onto a mesh) is a precedent for
  cutting along a non-planar, view/curve-derived boundary rather than a flat plane.
  https://docs.blender.org/manual/en/latest/modeling/meshes/editing/subdividing/knife_project.html
- **PrusaSlicer / OrcaSlicer dovetail-groove cut** — the closest existing precedent for a
  non-planar cut *in this exact family of slicers*; per this codebase's own `CutUtils.cpp`, it is
  implemented as a sequence of five flat half-space cuts at different offsets/angles
  (`perform_with_groove`, `CutUtils.cpp:727-820`) rather than a true curved-surface boolean — a
  useful reminder that "curved-looking" results have historically been achieved here by chaining
  flat operations, and this proposal is the first true curved (non-piecewise-flat) cutting surface
  in this cut-family.

---

## Phase 1 implemented

Branch `feat/curved-cut`, cut from `origin/feat/ultra-preferences` with
`origin/feat/thread-connector` merged in first (that branch adds the Thread and Bayonet Flexi kinds
in `GLGizmoCut.cpp` / `FlexiJoint.*`; building on it avoids a later conflict — the one conflict
was in `render_flexi_joint_inputs`' trailing hint text and was resolved in its favour, keeping
this branch's `ImGui::PopTextWrapPos()`).

### What shipped

**`src/libslic3r/CurvedCut.{hpp,cpp}` — the representation and the cut.**

- `CurvedCutSheet`: a coarse control grid (default 5x5, user 3..9 as the task asked; the research
  text above said 4..64, which was too wide to be useful — nine handles a side is already 81
  handles) over the cut plane's own frame, holding one displacement in mm per control point.
- Interpolation is **Catmull-Rom**, not the bicubic B-spline the research section left open. The
  reason is the phase-1 invariant: Catmull-Rom *interpolates* its control points, so a control
  point's displacement IS the surface height there. That makes "drag this handle to 8 mm and the
  face is 8 mm deep there" literally true, makes the height-sampling proof meaningful, and makes
  a grid whose nodes are a subset of a finer grid's nodes resample exactly. A B-spline only
  approximates its control points and would have failed all three.
- `evaluate(u,v)` short-circuits to exactly `0.0` when every control point is zero, so the flat
  case is bit-exact rather than merely near-zero.
- `grab()` / `smooth()`: the editing operations, using `Sculpt::falloff_weight` from
  `MeshSculpt.hpp` — the actual sculpt-brush falloff, not a reimplementation of it.
- `sample_sheet()`, `curved_cut_lower_slab()`: the dense sheet, and the closed slab built by
  offsetting it down to a floor below the object's bbox and stitching a four-sided rim, so the
  cutter is watertight (the research section flagged this as the thing a naive offset gets wrong).
  Side walls are vertical in local Z, never along the sheet normal, so a steep sheet cannot fold
  the slab into itself.
- `curved_cut_split()`: two booleans, `object ∩ slab` and `object − slab`, through
  `MeshBoolean::mfd::make_boolean` with the `mcut` fallback — the same chain `flexi_boolean` uses
  in `CutUtils.cpp`.

**`Cut::perform_with_curved_sheet()` in `CutUtils.{hpp,cpp}`** — the same shape as
`perform_with_plane()`: same clone/`add_cut_volume`/`post_process`/`reset_instance_transformation`
path, so the result object and part structure, the undo snapshot, and the after-cut options behave
as they do for a plane cut. Its first statement is:

```
if (sheet.is_flat())
    return perform_with_plane();
```

so a zero-displacement curved cut is not "a boolean that happens to agree with the plane cut" — it
is the plane cut, same function, byte for byte.

**`GLGizmoCut3D`** — a `Surface: Flat | Curved` radio pair in the cut-plane window, plus, in
Curved mode: a control-points slider (3..9), a brush-radius slider, a Falloff checkbox, and
Smooth / Reset surface buttons. The deformed sheet renders with the same translucent
`CUT_PLANE_DEF_COLOR` / `CUT_PLANE_ERR_COLOR` material as the flat plane; control points are small
spheres that highlight on hover. The sheet is built in the base plane's frame and drawn through
`translation_transform(m_plane_center) * m_rotation_m`, the same matrix the flat plane uses, so
the existing rotate/translate grabbers move the sheet with the plane, unchanged. A control-point
drag is one undo step per gesture (snapshot on mouse-down), and is *absolute* — each tick
re-applies the whole displacement to the grid as it stood at drag start, so a drag returning to
its origin cancels itself instead of accumulating.

Nothing new is written to the 3MF. The cut is baked, as plane cuts are; the control grid is
session state and `on_set_state()` resets it whenever the gizmo opens or closes.

### Disabled in Curved mode

- **Connectors.** "Add connectors" / "Edit connectors" is greyed with the note *"Connectors are not
  available on a curved cut yet."* — phase 1 scope, matching how the flat cut shipped before
  connectors existed. Phase 2 (per section 3 above) is where per-connector local frames go.
- **The Surface toggle itself** is disabled once connectors exist on the object, so you cannot
  strand placed connectors by switching to Curved.
- **Tongue-and-groove** is a separate `CutMode` and is unaffected; the Surface toggle only appears
  for `cutPlanar`.
- Still **available and unchanged**: Keep upper / Keep lower, Place on cut, Flip, Cut to parts.
  These act on the resulting halves and have nothing to do with how the halves were separated.
  Place-on-cut on a curved half rotates it so the *base plane* is down — the curved face is not
  flat, so it will not sit flush; that is inherent, not a bug, and it is why the flat plane
  remains what the option is defined against.

### Performance: sheet size against boolean time

40 mm cube, 8 mm dome, both booleans (Manifold), Release, measured by the `[.perf]` case in
`test_curved_cut.cpp`:

| samples | slab triangles | both booleans | result triangles |
|---|---|---|---|
| 32x32 | 4 092 | 10 ms | 1 528 |
| 64x64 | 16 380 | 36 ms | 5 112 |
| **128x128** | **65 532** | **131 ms** | **18 424** |
| 192x192 | 147 452 | 300 ms | 39 928 |
| 256x256 | 262 140 | 530 ms | 69 624 |

Cost is essentially linear in slab triangle count here, because the cube is trivial and the slab
dominates; on a real print mesh the object side dominates instead, as the research section
predicted.

**The preview and the cut sample at different rates, deliberately.** The sampled sheet is
piecewise linear, so it sits below the true surface by a chord sag falling as 1/N²: for this
8 mm dome over an 80 mm span that is **0.040 mm at 64x64 but 0.0098 mm at 128x128**. The proof bar
asks the cut face to match `f(u,v)` within 0.02 mm, which 64x64 does *not* meet — so
`CurvedCutSheet::CutSamples` is 128 and `DefaultSamples` (preview only, rebuilt every drag tick)
stays 64. This was found by the test failing at 64x64, not assumed.

### Proofs

`tests/libslic3r/test_curved_cut.cpp`, all passing:

1. **Flat sheet = flat cut.** `evaluate()` returns exactly `0.0` at 441 sample points; and the
   full `Cut` path with a flat sheet produces the same volumes as `perform_with_plane()` — same
   triangle count, same index arrays, vertices identical to 1e-9.
2. **Domed sheet on a 40 mm cube.** `volume(upper) + volume(lower)` matches the cube's volume to
   better than 1e-6 relative (28 245.1 + 35 754.9 = 64 000.0). The cut face, sampled at 25 points
   across the footprint on *both* halves, matches `f(u,v)` within 0.02 mm.
3. **Watertight and disjoint.** `its_num_open_edges` is 0 for both halves; intersecting them gives
   less than 1e-6 of the cube's volume.
4. **Grid resize preserves the surface.** 3 -> 9 -> 3 is an exact refinement (the 3x3 nodes are
   9x9 nodes) and round-trips to 1e-9. 5 -> 7 is a *refit*, not a refinement — the 5x5 interior
   nodes at u = 0.25, 0.75 are not 7x7 nodes — and costs ~0.26 mm on a smooth 8 mm surface, ~3% of
   amplitude; a one-cell spike, the worst case, costs ~0.76 mm. Both are bounded relative to
   amplitude as regression guards. This is a property of resampling between coarse grids of
   different phase, not a defect, and it is written down so a change that makes it worse shows up.

Also covered: Catmull-Rom really interpolates its control points; `grab` gives the centre the full
delta, neighbours a falloff-weighted fraction, and the rim nothing (and no-falloff moves everything
inside the radius the whole way); `smooth` pulls a spike down without flattening it; `reset` is
exactly flat; the slab is closed with positive volume at 8, 32 and 64 samples.

Full `libslic3r_tests`: 767 passed, 2 failed — the two known pre-existing failures in
`test_mixed_filament.cpp`, unrelated to this work.

**Demo**: `curved_cut_upper.stl`, `curved_cut_lower.stl`, `curved_cut_demo.3mf` (both halves as the
two parts of one object), produced by the `[.demo]` case from the same code the tests exercise —
set `EDGESLICER_CURVED_CUT_DEMO_DIR` to regenerate.

### Unverified

**Nobody has clicked this.** Everything above is proved headless, through `libslic3r`. The GUI
compiles and the app launches clean with a scratch data dir, but no human has opened the Cut
gizmo, switched Surface to Curved, dragged a handle and looked at the result. Specifically
unverified by eye:

- Whether the control-point handles are the right size and whether the ~18 px screen-space pick
  radius feels right at typical zoom levels.
- Whether the sheet reads correctly against the model — the preview is a thin two-sided slab, and
  z-fighting against the object's own surface at grazing angles has not been checked.
- Whether a drag feels natural. The drag maps mouse motion onto a camera-facing plane and keeps
  only the component along the cut normal (the Sculpt gizmo's projection), which means a drag is
  very insensitive when looking straight down the normal. Sculpt has the same property; whether it
  is acceptable here has not been judged.
- The interaction between a control-point drag and the plane's own grabbers when a handle sits
  visually on top of a grabber. Handles get first refusal by design, but which one a user *expects*
  to win in that overlap has not been tested.
- `F` / `Shift+F` modal brush sizing was **not** wired up (the task allowed a plain radius slider
  as the cheap alternative, and that is what shipped) — `GLGizmoSculpt`'s version routes through
  `GLGizmosManager::on_char`, which would need a second gizmo hooked into that path.

The owner click-test in section 7 above is still the outstanding gate.

## Bug: flat result

**Reported** on the live build (3269fec61b): Surface = Curved, a strongly S-bent sheet across a
small part on a *vertical* cut plane (rotated 90 deg about X, offset from the object's centre),
"Perform cut" produced a flat cut at the plane and ignored the sheet entirely — "just a flat cut
with the curve as a sort of centre point". The flat preview colouring matched the flat result.

### Cause

`GLGizmoCut3D::perform_cut()` calls `m_parent.reset_all_gizmos()` *before* it builds the `Cut`, and
that closes the Cut gizmo, which runs `on_set_state()`, which deliberately flattens the session-only
surface (`m_curved_surface = false; m_curved_sheet.reset(...)`). By the time the very next block
evaluated `is_curved_surface() && !m_curved_sheet.is_flat()`, both were already false, so the cut
fell through to `perform_with_plane()` — the plain flat plane cut, at exactly the plane the sheet had
been drawn on. Nothing about the geometry was wrong; the surface simply no longer existed when the
cut was asked for. A second, latent defect made the same symptom possible even with the state fixed:
`curved_cut_split()` widened the sheet itself (`s.set_half_size(need)`) to make the cutter reach past
the object, which drags the control points outwards and *stretches* the surface — on a rotated plane,
where the object's footprint in the cut frame is far larger than the sheet, that flattens a real bend
into a shallow ripple.

### Fix

- `perform_cut()` now snapshots the Curved/Flat choice and a copy of the sheet **before**
  `reset_all_gizmos()`, and performs the cut from that snapshot. One `BOOST_LOG_TRIVIAL(warning)` at
  the call site prints resolution, half size, max displacement, `is_flat` and the resulting
  `cut_curved`, so a future report carries its own diagnosis in the log.
- `curved_cut_lower_slab()` takes an explicit `extent` and samples the top surface through
  `evaluate_local(x, y)` rather than by `(u, v)`. `curved_cut_split()` now widens the **slab**, never
  the sheet: over the sheet's own domain the heights are untouched, and outside it the clamped rim
  value is extruded straight outwards.

"Curved but untouched = flat cut" is unchanged — `perform_with_curved_sheet()` still dispatches a
flat sheet into `perform_with_plane()`, and the gizmo still gates `cut_curved` on `!is_flat()`.

### Proofs

Three new cases in `tests/libslic3r/test_curved_cut.cpp`, all green:

- **"a rotated, offset plane cuts curved"** — a 40 mm cube through the whole `Cut` path with a cut
  matrix rotated 90 deg about X and offset 10 mm along its own normal, with a 5 mm dome. Both halves
  have zero open edges and their volumes sum to the cube within 1e-4 relative; the halves are
  unequal, so the offset is real. Mapped back into the *cut plane's own frame* (`cut_matrix.inverse()
  * volume_matrix` — the instance transform is re-seated after the cut and must not be used), the
  upper half spans local z in [0, 10] as the geometry demands, its cut face's height matches
  `f(u,v)` within 0.02 mm at 4604 of its 4608 vertices, and it deviates from flat by more than 1 mm,
  so the face is genuinely not planar. The flat-result guards are the two that would fail loudly on
  the reported bug: no point of the upper half sits *below* the sheet by more than 0.05 mm (a flat
  cut at z == 0 puts the dome's 5 mm crown a full 5 mm underneath the face), and the upper half's
  volume is 13.65 cm3 against the 16.0 cm3 a flat 10x40x40 slice would give — the missing 2.35 cm3
  is the dome's own.
- **"the gizmo's fit sequence keeps the displacement"** — the gizmo's own call order
  `set_half_size(30) → set_resolution(5) → grab(...) → set_half_size(52) → set_resolution(9)`, then
  a real `curved_cut_split`. The displacement survives every step and the split still conserves
  volume.
- **"a wider slab keeps the sheet's own heights"** — a 20 mm sheet with a 5 mm dome, slab built at
  70 mm: the slab really is 70 mm wide, the dome keeps its 5 mm peak at the sheet's own centre, and
  every top vertex beyond the sheet's domain is at zero (extruded, not stretched).

Existing curved-cut cases unchanged and still passing: 13 cases, 11191 assertions. Full
`libslic3r_tests`: 786 cases, 784 passed, 2 failed as expected — the same two known pre-existing
`test_mixed_filament.cpp` failures, unrelated to this work.

Note that the rotated-plane case caught a *second* wrong assumption while it was being written: the
first draft reconstructed the cut frame through the part's instance transform and read the face at
the wrong place. `add_cut_volume()` bakes `cut_matrix` into the stored mesh, `add_volume()` then
re-centres it into the volume matrix, and `reset_instance_transformation()` zeroes the instance
rotation afterwards — so `cut_matrix.inverse() * volume_matrix` on the raw volume mesh is the only
correct way back, and that is worth knowing for any future test on this path.

**Demo**: the rotated-plane cube halves as `rotated_cut_A.stl` / `rotated_cut_B.stl`, written by the
new `[.demo]` case from the same code the tests exercise.

### Unverified

**Nobody has clicked this either.** The fix is proved headless through `libslic3r` and the GUI
compiles, but no human has reopened the Cut gizmo, bent a sheet on a vertical plane and pressed
Perform cut. The specific claim that has only been reasoned about, not observed, is that
`reset_all_gizmos()` was the *only* consumer of gizmo state that ran between the user's click and
the cut: `get_cut_matrix()` is still read after the reset (as it always was, which is why the flat
cut worked), and `on_set_state()` is not the only thing `activate_gizmo(Undefined)` triggers. The
owner click-test remains the gate.
