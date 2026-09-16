# Assembly gizmo: "Face and face" detected no flat face

Branch `fix/assembly-face-face`, off `feat/ultra-preferences`.

## Symptom

In the Assembly gizmo, mode **Face and face assembly** (`AssemblyMode::FACE_FACE`) highlighted
nothing when hovering a plain flat face of a part, and nothing could be selected. **Curve and
curve** and **Triangle and triangle** both worked, and so did the Measure gizmo, so the raycast
and the pick plumbing were fine -- only the plane path was dead.

## Cause

`MeasuringImpl::get_feature` (`src/libslic3r/Measure.cpp`) picks, in order: a direct vertex snap
from the hit facet's corners, then the plane's extracted features (border edges, circles), and
only if nothing is close enough does it fall through to "return the plane as a whole".

In the BBS port (3d45414b71) those first two steps were gated by the fixed constant
`feature_hover_limit = 0.5` mm, so an ordinary hover in the middle of a face was never near
anything and always fell through to the Plane.

The Ultra commits (2ceac46ff7, bba3aef9fc) replaced that constant with the caller's
view-scaled `snap_radius`, and added the direct vertex pick gated by the same value.
`GLGizmoMeasure::on_render` computes it as

    const double snap_radius = 8.0 * double(inv_zoom) / mesh_scale;

i.e. **~8 pixels converted into mesh units**. That is the right idea for keeping vertices and
edges pickable when zoomed in, but it was left unbounded, so it grows without limit as you zoom
out. When the part is small on screen -- the normal state while assembling two parts, where
`inv_zoom` is on the order of 0.5-2 mm/px -- `snap_radius` reaches 4-16 mm, comparable to or
larger than the face being hovered. On a 20 mm cube face:

* the hover is inside the vertex radius of the hit facet's own corners, so `get_feature` returns
  a **Point**; failing that,
* it is inside `hover_limit` of one of the plane's **border edges**, so it returns an **Edge**.

Either way it never returns the Plane. The Face/Face hover filter in `GLGizmoMeasure::on_render`
accepts only `Plane` and `Circle`, so it called `curr_feature.reset()` on every hover and nothing
highlighted; `is_pick_meet_assembly_mode` would have rejected the click for the same reason.

Triangle/Curve were unaffected because `pick_kind` 1 and 2 return from `get_feature` before this
code is reached -- which is exactly why "Curve and curve" appeared to detect flat faces fine and
made the failure look plane-specific.

## Fix

Clamp the view-scaled snap radius against the geometry it is picking on, in `Measure.cpp`:

* new `MeasuringImpl::facet_snap_extent(face_idx)` returns `snap_facet_fraction` (0.25) times the
  hit facet's **shortest edge**;
* `eff_snap = min(snap_radius, facet_snap_extent(face_idx))` now drives the vertex pick radius,
  the feature `hover_limit`, and the edge-endpoint snap limit. `snap_radius < 0` still selects the
  legacy fixed-radius behaviour.

Zoomed in, `snap_radius` is far below the cap and the cap never binds, so Ultra's vertex and edge
picking is unchanged. Zoomed out, the cap guarantees that a vertex or edge snap can claim at most
a quarter of the facet, so the centre of a face always belongs to the face and Face/Face gets its
Plane back.

No GUI change was needed: the Face/Face filter and `is_pick_meet_assembly_mode` already accepted
`Plane` and `Circle`, and were correct.

## Proofs

`tests/libslic3r/test_assembly_face_pick.cpp` (new, in `libslic3r_tests`) builds a 20 mm cube,
constructs `Measuring` on its `indexed_triangle_set`, and calls `get_feature` on a facet of the
+Z face:

* centre of the top face, legacy `snap_radius = -1` -> `Plane`, normal +Z;
* centre of the top face at `snap_radius` 0.2, 0.5, 1.0, 5.0, 12.0 and 30.0 mm (a tight zoom-in
  through a wide zoom-out) -> `Plane`, normal +Z. **Before the fix this failed from 5.0 mm up**,
  returning `Point` / `Edge` -- the bug, reproduced headlessly;
* a hover placed exactly on a corner still returns `Point`;
* a hover 0.1 mm inside a border edge, away from both corners, still returns `Edge`;
* `pick_kind` 1 returns a `Triangle` with a one-facet `plane_indices`, and `pick_kind` 2 a
  `Curve` with a multi-facet `plane_indices` -- Triangle and Curve behaviour locked in unchanged.

## Owner click test

1. Load two parts, open the Assembly gizmo, choose **Face and face assembly**.
2. Hover a plain flat face of a part at a normal working zoom (whole part visible). The face
   highlights, and clicking selects it. Do the same on the second part and mate -- this is the
   case that previously did nothing at all.
3. Zoom out further and repeat: still highlights.
4. Circle rims (a peg or a hole) still highlight and still select in this mode.
5. Switch to **Curve and curve** and **Triangle and triangle**: unchanged from before.
6. Zoom well in on a corner or an edge in the Measure gizmo: vertices and edges still snap, as
   the Ultra change intended.
