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
3. Open the Sculpt gizmo (last in the bar). Its icon - a ball with a bump and a brush ring -
   should sit at the same size as the icons either side of it; v1's placeholder was visibly
   larger, which is what v1.1 fixed. The panel should offer Subdivide, because a calibration
   cube is far too coarse for any brush. Press it. Confirm the paint is gone and the panel said
   it would be. Press it two or three more times.
4. Re-paint. Now sculpt: Inflate a bump on the top face, Grab it sideways, Smooth the shoulder.
   Check the surface follows the cursor live and that the brush size feels like the sphere drawn.
   **Watch the cursor sphere through each drag**: it must travel with the mouse from press to
   release, not sit at the point you clicked (the v1 bug). On a Grab it should stay stuck to the
   patch you are pulling; on Inflate and Smooth it should ride the surface under the pointer.
   Drag off the edge of the part mid-stroke - the sphere should hold its last position rather
   than vanish - then release and hover off the part, where it *should* disappear.
4a. In the panel, type an exact figure into the box at the end of each slider (mm for brush
   size, a percentage for strength) and confirm the slider and the on-screen sphere follow.
   Ctrl+wheel over the model should move the brush-size slider and its box live.
5. Ctrl+Z once per stroke should walk it back one stroke at a time, not one frame at a time.
6. Confirm the paint from step 4 is still on the part after the strokes.
7. Slice. The bump should be in the preview at the layers you expect, with no new "not manifold"
   warning that was not there before.
8. Print it. The bump should be there, roughly the size of the on-screen brush.

Also unverified: the brush on a non-uniformly scaled part (the radius is converted with the mean of
the three scaling factors, so a 2:1 scale makes the brush elliptical in mesh space); the gizmo on
an object with several parts (v1 binds to a single selected part and the toolbar entry is inactive
otherwise); anything in the assemble view (explicitly disabled).

## v1.1 - the polish pass (branch `fix/sculpt-v1-polish`)

The owner's first hands-on test of v1 found the interactive half that
"[What nobody checked](#what-nobody-checked)" predicted would need it.

**The cursor did not follow the mouse during a drag.** Reported as "the sphere does not
follow the mouse once the click+drag begins, it just sits at the initial click point",
while the mesh still deformed correctly. The cause was in `GLGizmoSculpt::on_mouse()`: the
cursor position `m_hit` was refreshed only under `if (mouse_event.Moving())`, and wx fires
`Moving()` only while **no** button is held. Once the button went down, every subsequent
event arrived as `Dragging()`, which advanced the stroke but never touched `m_hit`, so
`render_cursor_sphere()` kept drawing at the press point for the whole stroke. The paint
gizmos never had this bug because `GLGizmoPainterBase::render_cursor()` re-runs
`update_raycast_cache()` from the live mouse position every frame, independently of the
button state.

The fix routes every path - hover, press, each drag tick and release - through one
`GLGizmoSculpt::update_cursor()`, which raycasts and then delegates *where the sphere goes*
to a new pure helper, `Sculpt::next_cursor_state()`. Grab needs its own rule: a Grab stroke
drags the surface the cursor sits on, and the AABB tree is deliberately stale during a
stroke, so a fresh raycast mid-Grab returns the pre-stroke surface and the cursor would lag
the patch being pulled. So Grab rides the stroke anchor as the drag moves it, while
Inflate/Deflate/Smooth follow the fresh hit. A stroke that runs off the silhouette holds its
last cursor position instead of blinking out; a *hover* that misses hides it.

**Panel.** Both sliders now carry the paint gizmos' layout: a label column sized to the wider
of the two labels plus `scaled(1.5f)` of gap (the v1 panel had the labels butting against the
slider track), the slider itself, and a `BBLDragFloat` box at the end of the row for typing an
exact value - the same `sliders_left` / `drag_left` / `slider_icon_width` arithmetic
`GLGizmoFdmSupports` uses. Brush size prints its unit (`%.2f mm`); strength is shown as a
percentage (`%.0f%%`, 5-100) rather than the raw 0.05-1 multiplier, and both typed values are
clamped back into range because `BBLDragFloat` does not clamp. Ctrl+wheel needed no new
plumbing to show up in the panel - the slider reads `m_cursor_radius` out of the member every
frame, so the existing `set_as_dirty()` is what makes the wheel, the slider, the input box and
the sphere all agree.

**Icon.** v1 borrowed `toolbar_modifier_sphere.svg`, which rendered visibly larger than its
neighbours. The cause is the art, not the toolbar: every real gizmo icon in this tree is a
**40x40 viewBox with its art inset to roughly 4..36**, while `toolbar_modifier_sphere.svg` is a
**16x16 viewBox with an r=7.5 circle at (8,8)** - art running edge to edge, so scaling it into
the same slot makes it about 20 % larger optically. `toolbar_sculpt.svg` /
`toolbar_sculpt_dark.svg` are new, original art drawn to the house convention: 40x40, art
within 9.5..31.5 x 2.4..35, 1-unit round-capped strokes, outline `#2b3436` light / `#b6b6b6`
dark, accent `#009688` shared - a ball with a bump pushed out of its shoulder, the brush ring
on the bump, and an arrow for the push. Registered in `GLGizmosManager::init()` and
`switch_gizmos_icon_filename()`; the placeholder is no longer referenced by the Sculpt gizmo.

**Cursor colour** now matches the paint gizmos exactly: black at 0.25 alpha on hover
(`get_cursor_hover_color()`), blue at 0.25 alpha while stroking
(`get_cursor_sphere_left_button_color()`), replacing v1's bespoke teal.

### What v1.1 proved, and what it did not

Nobody drove the gizmo with a mouse in this pass either, so the cursor fix is proved the only
way it can be without a hand on the mouse: `Sculpt::next_cursor_state()` was factored out
precisely so a test can replay a gesture through it. Four new `[SculptCursor]` cases feed it
the frame-by-frame inputs a real drag produces and assert on the resulting cursor sequence -
that a six-frame drag yields six distinct positions ending at the last one rather than
sticking at the first (the regression, stated directly); that a Grab cursor tracks the moved
anchor and specifically *not* the stale hit; that a stroke running off the part keeps its
cursor; that a hover off the part hides it. What no test covers is the wiring itself - that
`on_mouse()`'s `Dragging()` branch calls `update_cursor()` - which needs a GL context and a
hand on the mouse.

* `libslic3r_tests`: **702 cases, 700 passed, 2 failed as expected** (92,019 assertions), the
  tree's usual two baseline failures. Note the spec's older "682" figure is stale: the
  mixed-nozzle and flexi-joint branches merged after v1 brought the base to 698, and the four
  `[SculptCursor]` cases take it to 702.
* Build clean: Release `Snapmaker_Orca`, `Snapmaker_Orca_app_gui` and `libslic3r_tests`, zero
  `error C` / `error LNK`.
* A hidden scratch instance (`snorca_hubtest/inst_sculptpolish`, datadir copied from `dd_lan`)
  starts and runs a normal startup, with no SVG-load failure for the new icon.
* The icons were checked by rendering them in a browser beside `toolbar_move`, `toolbar_flatten`,
  `toolbar_meshboolean` and `toolbar_brimears` in both themes, against the old placeholder: the
  new icon matches its neighbours' optical size, the placeholder plainly did not. They have
  **not** been seen through the app's own nanosvg rasteriser in the toolbar.

Still unverified from v1 and untouched here: the brush on a non-uniformly scaled part, the
gizmo on a multi-part object, and everything in the assemble view.

## v2 - two more brushes, Blender's modal keys, and a panel that stops moving (branch `feat/sculpt-v2`)

The owner's second hands-on test, on v1.1. Three things came back: the Subdivide button could
not be clicked, the panel hung off the end of the toolbar, and the brush set was too thin for
print-prep touch-ups. Branch `feat/sculpt-v2` off `feat/ultra-preferences` (`6f9360cecb`).

### The Subdivide button could not be reached

Reported as: the coarse-mesh note and the Subdivide button "appear only while the cursor
hovers the part, and vanish the instant the cursor leaves it, resizing the whole panel - so
the button can never be reached."

The cause is `GLGizmoSculpt::needs_subdivision()`, which in v1 opened with

```cpp
if (!m_session || !m_hit_valid)
    return false;
```

`m_hit_valid` is the last raycast result, false whenever the cursor is off the part. So the
whole `if (needs_subdivision())` block - two lines of text and the button - existed only while
the pointer was over the mesh. The panel carried `ImGuiWindowFlags_AlwaysAutoResize`, so
losing those rows shrank the window; and the pointer necessarily leaves the part on its way to
the button, which is exactly when the button disappeared out from under it. The button was
unreachable by construction, not by a race.

Three changes, and the row is now unconditional:

* **`needs_subdivision()` no longer depends on the hover.** When the cursor is over the part
  it still asks the local question (mean edge length of the triangles under the brush, via
  `SculptSession::local_edge_length()`); when it is not, it asks the same question of the
  whole mesh (`its_average_edge_length()`). The answer may change as the cursor moves - that
  is fine and wanted - but the row's *presence* never does.
* **The Subdivide row is always drawn.** Above it sits one wrapped note, which is the coarse
  warning when subdividing is called for ("The mesh here is too coarse for this brush size.
  Subdividing changes the triangles, so painted supports, seams, colours and fuzzy skin on
  this part are cleared."), the cap message when a subdivision would blow past
  `MaxTrianglesAfterSubdivision`, and otherwise the neutral one-liner "Mesh is fine enough for
  this brush." The button below is enabled whenever a subdivision is actually possible, which
  is the honest condition - a mesh that is already fine enough can still be subdivided, and a
  user may want that. Only the 2M-triangle cap disables it (`ImGuiWrapper::disabled_begin()`).
* **The panel has a fixed width and every long string wraps to it.**
  `ImGuiWindowFlags_AlwaysAutoResize` is gone, replaced by
  `ImGui::SetNextWindowSize({m_imgui->scaled(16.f), 0})` plus `ImGuiWindowFlags_NoResize`, so
  only the height is left to the contents. Every multi-word note goes through
  `ImGuiWrapper::text_wrapped(..., ImGui::GetContentRegionAvail().x)` rather than `text()`.
  The triangle count stays on one line, as asked.

Two knock-on layout changes fall out of the fixed width. The slider width was a hardcoded
`scaled(7.0f)`; it is now whatever is left after the label column and the numeric input
(`wrap_width - sliders_left - 1.5f * slider_icon_width - space_size`, floored at
`scaled(3.0f)`), so a longer translation cannot push the row past the window edge. And six
brush radios no longer fit on one line, so the radio row wraps: each radio measures itself
(`calc_text_size` + frame height + inner spacing) and starts a new line when the current one
is full, instead of an unconditional `SameLine()`.

### The panel opened off the end of the toolbar

`GLGizmosManager::do_render_overlay()` hands each gizmo the x of **its own toolbar icon**, and
Sculpt was added at the end of `EType`, so its icon is the last one on the bar and a panel
drawn rightward from there runs off the canvas. The paint gizmos anchor at their icon in
exactly the same way - `GLGizmoFdmSupports::on_render_input_window()` is the same
`GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f)` call - they simply sit
further left on the bar and never notice.

`GLGizmoBase::GizmoImguiSetNextWIndowPos()` already contains the clamp that fixes this:

```cpp
if (x + w > canvas_width) x = canvas_width - w;
```

but the one-argument overload passes `last_input_window_width`, i.e. **the width the window
had on the previous frame**. Under `AlwaysAutoResize` that was whatever the previous frame's
contents happened to need - and, on the frame the hover state changed, exactly the wrong
number. Now that the width is pinned, v2 calls the four-argument overload with the real width
directly, so the clamp is exact: the panel opens toward the centre of the bar with its right
edge flush against the canvas edge, and the position no longer depends on what was drawn last
frame. No new positioning scheme was invented - this is the existing clamp, fed a number it
can trust.

### Two new brushes

Both live in `Sculpt::SculptSession::apply()` beside the others, both are vertex-only, and
both therefore go through the same annotation-preserving commit
(`Sculpt::commit_sculpted_mesh()`, no `clear_before_change_mesh()`). Both honour
`BrushParams::strength` and the Smooth-falloff toggle the same way every v1 brush does, via
the shared `weights[i] = strength * falloff_weight(d, r)` computed once at the top of
`apply()`.

**Flatten.** Fits a plane to the patch under the brush and slides each vertex toward it by
`weight`. The fit is `Sculpt::fit_plane()`: the origin is the falloff-weighted centroid of the
vertices in the brush, the normal the falloff-weighted average of their vertex normals -
"centroid + area-weighted normal", the first of the two options the brief offered. It
accumulates in `double`, because a `float` accumulator loses the plane offset on a part
sitting far from the origin. The move is `v -= weight * signed_distance * normal`, so weight 1
lands a vertex exactly **on** the plane and can never overshoot past it; that is what makes a
full-strength stroke read as "flatten" and what keeps repeated ticks stable.

The plane's *direction* is fitted once, at `start_stroke()`, and pinned for the whole stroke
(`m_stroke_plane_normal`, passed as `BrushParams::plane_normal`); the offset is refitted every
tick so the plane stays on the surface. Refitting the direction every tick makes the brush
chase the surface it has just levelled and never converge.

**Ctrl-variant: Blender's "Fill"** (`BrushParams::fill_only`). The default is symmetric - a
bump is pushed down and a dent pulled up. Inverted, only the vertices **below** the plane (on
the -normal side) move, so dents are filled and bumps are left standing. That is the pairing
the brief allowed, and it is the one documented here.

**Crease.** Blender's crease is pinch + inward push, and that is what this is. The pinch is
movement toward the brush's centre line - the line through the brush centre along the fitted
surface normal - computed by projecting the vertex's offset from the centre onto the tangent
plane (`rel - rel.dot(n) * n`) and moving `weight` of the way in. The push is
`weight * ratio * 0.25 * radius` along `-normal`, scaled to the brush so a crease is as deep
as the brush is wide whatever the brush size - the same trick Inflate plays with its `amount`.
Inverted (Ctrl) the normal push flips sign and a ridge is raised; the pinch is unchanged,
which is why the ridge is exactly the valley mirrored in the normal direction.

The brush radio row is now, in order: **Grab, Inflate, Deflate, Smooth, Flatten, Crease.**

### Blender-style hotkeys

| Key | While | Does |
| --- | --- | --- |
| `F`, then move the mouse | gizmo open, mouse over the 3D scene | Adjusts **Brush size**. Rightward motion grows the radius, leftward shrinks it. The on-screen circle resizes live and the panel's numeric input shows the value as it changes. |
| `Shift+F`, then move the mouse | as above | Adjusts **Strength**, over its 5-100 % range. |
| Left click, or `Enter` | a size/strength modal is running | Confirms - the new value stands. |
| `Esc`, or right click | a size/strength modal is running | Cancels - the value goes back to what it was when the modal began. |
| `Ctrl` held during a stroke | Inflate/Deflate, Flatten, Crease | Inverts the brush: Inflate acts as Deflate and back, Flatten becomes Fill, Crease becomes Ridge. The cursor circle turns **red** while inverted. |
| `Ctrl` held during a stroke | Grab, Smooth | Nothing - no inverse exists. Ctrl+click keeps its canvas meaning for these two brushes. |
| `Shift` held during a stroke | as v1 | Inverts, as before. Holding Shift *and* Ctrl is a double negative and cancels. |
| `Ctrl` + mouse wheel | gizmo open | Brush size, as v1. Unchanged. |
| `Esc` | a stroke is running | Cancels the stroke, as v1. Unchanged. |

The mapping is deliberately different for the two values. Brush size is **multiplicative** - a
drag of `Sculpt::AdjustFullScalePx` (240 px) to the right doubles the radius and the same
travel left halves it - so the gesture feels the same at 0.5 mm and at 15 mm. Strength is
**additive**: 240 px spans the whole 0.05-1 range. Both recompute the value from the *total*
travel since the modal began rather than accumulating per-frame deltas, so the gesture is
exactly reversible: bringing the mouse back to where it started returns the starting value,
with no drift.

While a modal runs the cursor circle turns **amber**, and it stays on screen even when the
mouse is off the part - the whole point of the gesture is watching the circle resize. The
panel prints "Brush size: move the mouse, click to keep it, Esc to cancel" in place of its
usual note.

#### Conflicts found, and how each was resolved

**`F` is already "Gizmo place face on bed".** `KBShortcutsDialog` lists it, and it is
dispatched from `GLGizmosManager::handle_shortcut()`, which `on_char()` reaches as a
fallthrough after its own switch. Resolution: Sculpt's key handler is called at the **top** of
`GLGizmosManager::on_char()`, gated on `m_current == Sculpt`, and returns true only for the
keys it actually wants (`F`, and `Enter`/`Esc` while a modal runs). Bare `F` therefore sizes
the brush while the Sculpt gizmo is open and opens the flatten gizmo everywhere else, which is
the least surprising reading: a modal key belongs to the tool that is open. The gizmo returns
false for everything else, so nothing else is stolen - `Shift+A` still arranges, `1`-`9` still
set the filament, `Esc` still closes the gizmo when no stroke or modal is running.

**`Ctrl` is the canvas's additive-selection modifier, and v1's own "end the stroke" gesture.**
`GLGizmoSculpt::on_mouse()` returned false on a Ctrl+LeftDown so the canvas could handle it,
and ended any stroke on a Ctrl+Drag, "matching the paint gizmos". Both are now conditional on
`Sculpt::brush_inverts_with_ctrl()`: for Inflate/Deflate, Flatten and Crease, Ctrl is the
invert modifier and the gizmo keeps the event; for Grab and Smooth, where Ctrl means nothing
to the brush, the v1 behaviour is untouched and the canvas still gets the click. This is the
narrowest resolution available - the Ctrl gesture is only taken away on the brushes that have
something to do with it.

**ImGui focus.** `GLCanvas3D::on_char()` gives ImGui first refusal
(`if (imgui->update_key_data(evt)) { render(); return; }`) and returns before
`m_gizmos.on_char()` is reached, so a character typed into the panel's Brush size or Strength
input box never reaches the gizmo's key handler. `F` typed into a numeric input is just an
`F`. No extra guard was needed; the ordering already provides it.

**`Enter`.** Unbound on the canvas except in a `Shift+Ctrl`/`Shift+Alt` combination, so
consuming a bare `Enter` while a modal runs takes nothing away.

### What changed, file by file

* `src/libslic3r/MeshSculpt.{hpp,cpp}` - `BrushType::Flatten` and `BrushType::Crease` plus
  their `BrushParams` fields (`plane_normal`, `fill_only`, `ridge`, `crease_normal_ratio`);
  `fit_plane()`; `plane_distance_variance()`; the modal state machine
  (`AdjustTarget`, `AdjustState`, `adjust_begin/move/confirm/cancel`, `AdjustFullScalePx`);
  `brush_inverts_with_ctrl()`.
* `src/slic3r/GUI/Gizmos/GLGizmoSculpt.{hpp,cpp}` - the two new brushes in the enum and in
  `make_brush()`; the plane pinned at `start_stroke()`; `ctrl_down` threaded through
  `start_stroke`/`continue_stroke`/`make_brush`; `begin_adjust`/`update_adjust`/`end_adjust`
  and `on_sculpt_char()`; the modal's ownership of the mouse in `on_mouse()`; the amber and
  red cursor colours; `needs_subdivision()` no longer hover-gated; the whole panel rewrite.
* `src/slic3r/GUI/Gizmos/GLGizmosManager.cpp` - Sculpt's key handler called first in
  `on_char()`.
* `src/slic3r/GUI/KBShortcutsDialog.cpp` - a "Sculpt Gizmo" section.
* `tests/libslic3r/test_mesh_sculpt.cpp` - nine new cases.

### What v2 proved

* **Build**: `BUILD_EXIT=0`. Release `Snapmaker_Orca`, `Snapmaker_Orca_app_gui` and
  `libslic3r_tests` in the branch's own worktree build tree, zero `error C` / `error LNK`.
* **Tests**: `libslic3r_tests` - **726 cases, 724 passed, 2 failed as expected** (103,997
  assertions), the tree's two usual baseline failures and nothing else. The nine new cases all
  pass. (v1.1's 702 is the stale figure: the base grew to 717 between the branches, and the
  nine new cases take it to 726.)
  * `[SculptFlatten]` - Flatten cuts the plane-distance variance of a bumped, saw-toothed
    patch by more than half and lands the full-weight centre vertex exactly on the plane;
    the Fill variant lifts every vertex below the plane and moves not one above it, while the
    symmetric variant levels both sides; half the strength is exactly half the move,
    vertex by vertex, and with the falloff off every vertex in the brush lands on the plane;
    `fit_plane()` recovers the known normal and centroid of a tilted grid and refuses an
    empty patch.
  * `[SculptCrease]` - on a flat grid, every vertex in the brush ends closer to the centre
    line (by exactly `(1-w)` of its original radius) and lower (by exactly
    `w * 0.25 * radius`), with the on-axis vertex dropping without pinching; the inverted
    brush is the valley mirrored in z with an identical pinch; on a sphere the pinch is
    toward the *brush axis* rather than the mesh origin, the volume goes down, and the mesh
    stays closed.
  * `[SculptAdjust]` - the radius modal doubles on a full-scale rightward drag and halves on
    the leftward one, is smooth (a half-scale drag is a factor of √2), is exactly reversible
    after wandering, and clamps at both ends; the strength modal is additive over its range
    and likewise reversible; confirm keeps the live value and cancel restores the start;
    `brush_inverts_with_ctrl()` is true for exactly Inflate/Flatten/Crease.
  * The existing "a sculpt stroke never changes the triangle indices" case now runs Flatten
    and Crease as well, so both are covered by the annotation-preserving invariant.
* **Launch**: a hidden scratch instance (`snorca_hubtest/inst_sculpt2`, datadir copied from
  `dd_ctl`) starts and runs a normal startup, then was stopped by PID.

### What nobody checked

Nobody drove this with a mouse or a keyboard. Everything in the interaction half is argued
from the code and proved only where a pure helper could be factored out - which is why
`fit_plane()`, the brush kernels and the whole modal state machine live in `MeshSculpt` rather
than in the gizmo. Specifically **unverified**:

* That the panel is actually the right width on screen, that the wrapped notes look right, and
  that the Subdivide button can now be clicked. The reasoning is above; nobody clicked it.
* That the panel now opens toward the centre of the toolbar. The clamp is the tree's own and
  is now fed a correct width, but the result has not been seen.
* That `F` and `Shift+F` fire, that the circle resizes live, and that click/Enter/Esc/right
  click end the modal as described. The arithmetic is tested; the wx routing is not.
* That the Ctrl-invert cursor colour reads as intended, and that taking Ctrl+click away from
  the canvas on three of the six brushes is not itself surprising in practice.
* That Flatten and Crease *feel* right under a real stroke - the tests prove what they do to
  the vertices, not that the resulting brush is pleasant to use.
* Everything still unverified from v1 and v1.1: the brush on a non-uniformly scaled part, the
  gizmo on a multi-part object, and the assemble view.

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

* The gizmo's art is `toolbar_sculpt.svg` / `toolbar_sculpt_dark.svg` (added in v1.1, replacing
  the borrowed `toolbar_modifier_sphere` placeholder), registered in `GLGizmosManager::init()` and
  in `switch_gizmos_icon_filename()`. They are generated rather than hand-drawn - the geometry is
  computed so the bump joins the ball tangentially - and the light/dark pair differ only in the
  outline colour. Keep any replacement to the house convention: 40x40 viewBox, art inset to
  roughly 4..36, 1-unit round-capped strokes, `#2b3436` / `#b6b6b6` outline, `#009688` accent.
* `Sculpt` was added at the **end** of `GLGizmosManager::EType`, before `Undefined`, and the gizmo
  is pushed last in `init()` - the enum and the vector must stay in step.
* The gizmo has no keyboard shortcut (`m_shortcut_key = 0`); every letter worth having is taken.
* Reference for the brush maths: **SculptGL** (stephomi/sculptgl, MIT). Nothing was copied; the
  falloff and the grab/inflate/smooth kernels are the standard formulations it also uses.
* `libigl` is **not** a dependency of this tree despite the comments in `AABBTreeIndirect.hpp`.
  ARAP or biharmonic deformation for a future handle-based mode would be a new dependency
  decision (MPL2), not a reuse.
