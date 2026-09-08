# Sculpt mode v1 - brush sculpting that keeps the paint

2026-09-07/08, branch `feat/sculpt-v1` off `feat/ultra-preferences` (`67a2d25eb3`).

A "Sculpt" gizmo with three brushes for quick print-prep touch-ups on a loaded part: pull a lump
out, push a dent in, take the lumps off something. Not a sculpting app - there is no dynamic
topology, no symmetry, no falloff curves and no tablet pressure, and the framing should stay
"touch up a model you already have", not "model in the slicer".

The one thing v1 does that no other mesh-replacing feature in this tree does: **it keeps the
painted annotations**. Painted supports, seams, MMU colours and fuzzy skin all survive a sculpt
stroke.

## Why the paint survives

`ModelVolume` carries four `FacetsAnnotation` stores (`supported_facets`, `seam_facets`,
`mmu_segmentation_facets`, `fuzzy_skin_facets`), all indexed by facet id and all independent of the
mesh object. `ModelVolume::set_mesh()` only swaps the mesh `shared_ptr`; it never touches them. The
thing that wipes them is `Plater::clear_before_change_mesh()`, which every existing mesh-replacing
feature calls - `GLGizmoSimplify::apply_simplify()`, `ObjectList::fix_through_netfabb()`,
`ObjectList::repair_by_remesh()` - precisely because QEM and voxel remeshing renumber the facets.

A sculpt stroke never renumbers anything. It rewrites `indexed_triangle_set::vertices` and leaves
`::indices` byte-identical, so facet *n* after the stroke is the same facet *n* it was before, and
the annotations remain meaningful. The commit therefore goes:

```
Plater::TakeSnapshot(..., UndoRedo::SnapshotType::GizmoAction)   // one per stroke
Sculpt::commit_sculpted_mesh(volume, std::move(its))             // set_mesh + hull + bbox + on-bed
Plater::changed_mesh(object_idx)                                 // re-slice, object list, scene
```

with **no** `clear_before_change_mesh()`. `commit_sculpted_mesh()` checks the indices itself and
refuses (returns false, changes nothing) if they differ, so a future caller cannot silently
misalign existing paint by handing it a retopologised mesh.

The test `Painted facets survive a sculpt commit` paints all four kinds on a subdivided cube,
inflates a bump, commits, and compares each store's `TriangleSplittingData` before and after -
byte-for-byte equal, while the vertices moved and the indices did not.

## What is in the branch

New:

* `src/libslic3r/MeshSculpt.hpp` / `.cpp` - all of the maths, no GUI, unit-testable.
  * `falloff_weight(d, r)` - the smooth quartic bump `(1 - (d/r)^2)^2`, 0 outside the brush.
  * `SculptSession` - per-part cache built once when the gizmo attaches to a volume: the working
    `indexed_triangle_set`, an `AABBTreeIndirect` tree over its triangles, the vertex -> face CSR
    adjacency, and the vertex normals. `apply(BrushParams, StrokeStep&)` is one brush tick.
  * `its_subdivide_midpoint()` - uniform 1:4 midpoint subdivision with shared edge midpoints.
  * `its_vertex_normals()`, `its_laplacian_energy()`.
  * `indices_match()`, `commit_sculpted_mesh()` - the annotation-preserving commit.
* `src/slic3r/GUI/Gizmos/GLGizmoSculpt.hpp` / `.cpp` - the gizmo.
* `tests/libslic3r/test_mesh_sculpt.cpp` - 14 test cases.

Changed:

* `GLModel` gains `can_update_triangles_in_place()` / `update_triangles_in_place()` (see the GPU
  spike below).
* `GLCanvas3D` gains a non-const `get_volumes()` so the gizmo can patch a volume's buffer.
* `GLGizmosManager` - a `Sculpt` `EType` at the end of the enum, the registration, the icon case,
  the `gizmo_event` dispatch and the Ctrl+wheel list.

## The brushes

All three take a radius (world mm, drawn with the paint gizmos' translucent cursor sphere), a
strength (0.05-1), and a falloff toggle. Only vertices **strictly inside** the brush sphere move -
a vertex exactly on the rim has weight 0 either way, so it is left out of the touched set. With the
falloff off every vertex inside gets weight 1 (a hard-edged brush).

**Grab.** On drag, the mouse is projected onto the plane through the stroke's anchor point facing
the camera; the world-space delta between ticks is taken back into volume space through
`trafo.linear().inverse()` and applied as `w * displacement`. The brush centre follows the drag, so
a long pull keeps hold of the same patch rather than sliding off it.

**Inflate / Deflate.** `w * amount * vertex_normal`, where the normals are the ones cached at the
end of the previous tick (so the direction is the pre-stroke surface, not the half-inflated one).
`amount` is `0.06 * radius` per tick, which makes the brush feel the same at any size. Shift
inverts it.

**Smooth.** Uniform-weight (graph) Laplacian, `v += w * lambda * (mean(one-ring) - v)`, as a Jacobi
update - the whole pass reads the old positions. The optional **Taubin** checkbox alternates a
`+lambda` pass with a `-mu` pass (`lambda = 0.6307`, `mu = -0.6732`, Taubin's own values), which is
what stops the mesh shrinking. Measured on a noisy sphere over ten passes: Taubin holds the volume
inside 0.5 %, plain Laplacian loses several percent.

Undo is one step per stroke. The snapshot is taken at mouse-up, immediately before the volume is
touched - the `ModelVolume` is untouched for the whole drag (only the session's working copy and
the GPU buffer move), so the snapshot records the pre-stroke mesh. This is exactly the discipline
`GLGizmoPainterBase` uses.

## Subdivide-on-demand

A stroke on a low-poly part has nothing to move. When the mean edge length under the cursor exceeds
`radius / 4` the panel says so and offers a **Subdivide** button: one uniform 1:4 midpoint
subdivision of the whole part, capped at 2,000,000 triangles.

This one *does* renumber the facets, so it is handled the way Simplify handles it: its own undo
step, `Plater::clear_before_change_mesh()` first, and the panel states plainly that painted
supports, seams, colours and fuzzy skin on that part will be cleared. Subdividing only the region
under the brush is a v2 item.

Midpoint subdivision keeps a watertight mesh watertight (the two triangles of an edge share the
midpoint) and does not move the surface - `its_volume` is unchanged on a cube to 1e-4.

## The GPU spike - result: yes, in place

`GLVolume::model` is a `GLModel`. `GLModel::init_from(const indexed_triangle_set&)` **explodes** the
mesh into three unshared vertices per triangle with a flat face normal, layout `P3N3`, in triangle
order, and `send_to_gpu()` uploads it with `GL_STATIC_DRAW` and then **frees the CPU-side vertex
array**. `ENABLE_SMOOTH_NORMALS` is off in this tree, so that is the path every model part takes.

That layout turns out to be ideal for partial updates:

* triangle *t* owns bytes `[t * 72, t * 72 + 72)` of the VBO - three vertices of six floats,
* the normals are per-triangle, so a moved vertex needs no cross-triangle bookkeeping,
* a run of consecutive dirty triangle ids is one contiguous range.

`GLModel::update_triangles_in_place(its, sorted_triangle_ids)` rebuilds the payload for the dirty
triangles, coalesces consecutive ids into runs, and issues one `glBufferSubData` per run. It
refuses (returns false) unless the buffer is uploaded, the format is `Triangles`/`P3N3`, and the
vertex count is exactly `3 * its.indices.size()` - so it can never scribble over a buffer with a
different layout.

The gizmo calls it every tick with the tick's `dirty_triangles`. If it ever returns false the
gizmo falls back to `model.reset(); model.init_from(its)` throttled to one rebuild per 30 ms; the
fallback is written and wired but on this tree's render path it is not the one that runs.

**Not verified:** the GL upload cost itself. Nobody drives the gizmo with a mouse in this session,
and a headless test has no GL context, so the `glBufferSubData` calls have never executed. What is
measured is the CPU work that produces them, and the payload size, which is the honest proxy:

| mesh | dirty triangles per tick | bytes uploaded per tick | bytes for a full re-upload |
|---|---|---|---|
| 222,720 triangles | 7,680 | 553 KB | 16.0 MB |
| 890,880 triangles | 28,800 | 2.07 MB | 64.1 MB |

i.e. a 3 mm Inflate brush moves 3.2 % of the buffer on the big mesh, in a handful of coalesced
ranges, instead of all of it.

## Measured stroke latency

Release build, the 20-core box, `libslic3r_tests.exe "[SculptBench]"`. Sphere subdivided to the
sizes below; Inflate brush, 3 mm radius; 50 ticks timed after a warm-up tick.

| mesh | vertices | session setup (once) | brush tick | vertices touched | tree rebuild (once per stroke) |
|---|---|---|---|---|---|
| 222,720 triangles | 111,362 | 53 ms | **0.53 ms** | 3,601 | 48 ms |
| 890,880 triangles | 445,442 | 237 ms | **2.4 ms** | 13,921 | 213 ms |

A tick costs what is under the brush, not what is in the mesh - the tick time tracks the touched
vertex count (0.15 us and 0.18 us per touched vertex), not the triangle count. Both are comfortably
inside a 16 ms frame.

The two "once" columns are the price of picking up a part and of putting it down: the AABB tree is
built when the gizmo attaches to a volume, left deliberately stale for the duration of a stroke
(the brush centre comes from the shared `MeshRaycaster`, which is also stale and equally
acceptable), and rebuilt once at mouse-up. A fifth of a second to start sculpting a 900k-triangle
part is fine; if it ever is not, the tree build is the thing to parallelise.

The first bench figure was 40 ms per tick before the hot path was cleaned up: the query used to
build an intermediate triangle-index vector and every tick allocated its weights, dirty list and
normal-gather buffers fresh. Stamping the vertices inside the tree traversal in one pass and
reusing the scratch buffers took it to 2.5 ms.

## What was proved

* `libslic3r_tests` after the change: **682 test cases, 680 passed, 2 failed as expected**
  (91,456 assertions, 91,454 passed). The two expected failures are the tree's baseline.
* 14 new `[Sculpt]` cases, all passing: falloff weights; Grab moving exactly the vertices inside
  the brush by exactly `strength * falloff * displacement` and leaving everything outside
  bit-identical; the falloff-off variant; strength scaling linearly; Inflate parallel to the
  pre-stroke vertex normal with magnitude `w * amount`, growing the volume (and shrinking it when
  deflating); Smooth halving the Laplacian energy of a noisy sphere; Taubin inside 0.5 % on volume
  where plain Laplacian loses more; indices and open-edge count unchanged across a Grab + Inflate +
  Smooth sequence; midpoint subdivision quadrupling the faces, adding exactly `E` vertices and
  staying watertight; `local_edge_length`; the annotation survival test; a commit with mismatched
  indices being refused.
* Build clean: full Release build of `Snapmaker_Orca`, `Snapmaker_Orca_app_gui` and
  `libslic3r_tests`, zero `error C` / `error LNK`.
* A hidden scratch instance (`snorca_hubtest/sculpt_cand`, datadir copied from `dd_lan`) starts,
  stays up for 40 s and writes a normal startup log. Its 713 `[error] can not find parent for
  config` lines are the datadir's pre-existing orphaned user presets, not this change.
* CLI slice of a sculpted mesh: `sculpted_bump.stl` (a 20 mm cube subdivided 4x, an Inflate bump,
  then a Taubin smooth, written by the export test through the model-level API) slices with
  `--slice 0` on a P1S preset, exit 0, 120 layers, `max_z_height: 24.00` - the cube's 20 mm plus
  the 4 mm bump, i.e. the sculpt is in the G-code.
* Untouched P1S slice, head (`67a2d25eb3`) vs candidate, same build tree and flags: both
  **2,141,791 bytes**, differing only in the `; generated by ... on <timestamp>` header line;
  0 differing lines once that line is filtered.

Gate script: `snorca_hubtest/gate_sculpt_cli.sh`. Bench: `libslic3r_tests.exe "[SculptBench]"`.

## What nobody checked

**Nobody drove the gizmo with a mouse.** Everything above is unit tests, a CLI slice and a headless
start. The whole interactive half - the cursor sphere rendering, the drag-plane projection for
Grab, the panel layout, the live vertex-buffer patching, whether a stroke *feels* right - has never
been exercised. The owner's test:

1. Load the calibration cube (`resources/handy_models/OrcaCube_v2.3mf`). Select it.
2. Paint something on it first - a couple of support enforcers, or an MMU colour patch on one face
   - so there is paint to lose.
3. Open the Sculpt gizmo (the toolbar icon is the modifier-sphere placeholder, last in the bar).
   The panel should offer Subdivide, because a calibration cube is far too coarse for any brush.
   Press it. Confirm the paint is gone and the panel said it would be. Press it two or three more
   times.
4. Re-paint. Now sculpt: Inflate a bump on the top face, Grab it sideways, Smooth the shoulder.
   Check the surface follows the cursor live and that the brush size feels like the sphere drawn.
5. Ctrl+Z once per stroke should walk it back one stroke at a time, not one frame at a time.
6. Confirm the paint from step 4 is still on the part after the strokes.
7. Slice. The bump should be in the preview at the layers you expect, with no new "not manifold"
   warning that was not there before.
8. Print it. The bump should be there, roughly the size of the on-screen brush.

Also unverified: the brush on a non-uniformly scaled part (the radius is converted with the mean of
the three scaling factors, so a 2:1 scale makes the brush elliptical in mesh space); the gizmo on
an object with several parts (v1 binds to a single selected part and the toolbar entry is inactive
otherwise); anything in the assemble view (explicitly disabled).

## What v2 and v3 need

**v2** - the interaction polish, none of which changes the topology story:

* Falloff curves (linear / smooth / sphere / root / sharp, SculptGL-style) instead of the one fixed
  quartic. `BrushParams` already isolates the weight, so this is a function pointer and a combo.
* Symmetry: mirror the touched vertex set across an X/Y/Z plane through the bounding-box centre and
  apply the mirrored displacement. Needs a plane indicator in the cursor rendering.
* Flatten: project the vertices under the brush onto the plane fitted at stroke start.
* **Subdivide under the brush only.** This is the real work in v2. `its_subdivide_midpoint()`
  subdivides the whole part, which is wasteful and hits the triangle cap fast. Subdividing a patch
  needs a crack-free boundary: the triangles on the ring outside the patch have to be split to
  match the new midpoints on the shared edges (the standard "red-green" refinement), or the mesh
  opens up. `TriangleSelector::split_triangle()` / `perform_split()` already does neighbour-
  consistent bisection for paint state and is the closest existing thing to lift from, but it is
  bespoke to the paint boolean tree, not a general mesh operator. Budget it as the phase's long
  pole, not as glue.
* A per-stroke tree refresh cheaper than a full rebuild (refit the touched nodes' bounding boxes
  bottom-up rather than rebuilding), if the 217 ms at 900k triangles ever bites.
* Non-uniform scale: build the brush as an ellipsoid in mesh space instead of a sphere.

**v3** - dynamic topology. The backend is already vendored: `remesh_by_voxels()` in
`OpenVDBUtils.cpp` (`meshToVolume` -> `levelSetRebuild` -> `volumeToMesh`) is a working, tested
whole-mesh voxel remesh that "Repair by remeshing" ships today. v3 scopes it to a padded box around
the brush, edits the level set directly with the brush's implicit displacement, re-extracts, and
splices the patch back into the untouched mesh (Manifold, also vendored, is the right tool for the
splice). Three things it needs that v1 does not have:

1. A worker thread. A voxel-grid job cannot finish in a frame. `GLGizmoSimplify::process()` is the
   template - a mutex-guarded idle/running/cancelling state machine with a `throw_on_cancel` token.
2. An annotation story. Voxel remeshing renumbers everything in the patch, so v3 must either fall
   back to clear-and-notify for the whole volume (reuse `clear_before_change_mesh()` and the
   existing `CustomSupportsAndSeamRemovedAfterRepair` notification verbatim) or invent an
   old-facet -> new-facet correspondence. The first is honest and shippable; the second is a
   research project.
3. A trigger model. Blender fires dyntopo on stretch/curvature thresholds, not on every stroke; the
   notification must fire exactly when topology actually changed, or users will learn to ignore it.

Self-intersection stays out of scope at every level. A hard Grab stroke can push one part of the
surface through another; the slicer tolerates a degree of that (which is why the repair actions are
opt-in after the fact, not automatic), and preventing it per-tick is far too expensive. The right
move is to point at "Repair by remeshing" after a sculpt session, not to invent new intersection
logic.

## Notes for whoever picks this up

* The gizmo has **no art**. It borrows `toolbar_modifier_sphere.svg` /
  `toolbar_modifier_sphere_dark.svg` as a placeholder, registered in `GLGizmosManager::init()` and
  in `switch_gizmos_icon_filename()`. Both need swapping when an icon exists.
* `Sculpt` was added at the **end** of `GLGizmosManager::EType`, before `Undefined`, and the gizmo
  is pushed last in `init()` - the enum and the vector must stay in step.
* The gizmo has no keyboard shortcut (`m_shortcut_key = 0`); every letter worth having is taken.
* Reference for the brush maths: **SculptGL** (stephomi/sculptgl, MIT). Nothing was copied; the
  falloff and the grab/inflate/smooth kernels are the standard formulations it also uses.
* `libigl` is **not** a dependency of this tree despite the comments in `AABBTreeIndirect.hpp`.
  ARAP or biharmonic deformation for a future handle-based mode would be a new dependency
  decision (MPL2), not a reuse.
