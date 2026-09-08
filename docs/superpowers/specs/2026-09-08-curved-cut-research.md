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
