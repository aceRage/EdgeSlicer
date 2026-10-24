# Design: "Spread" paint tool (grow-while-held, angle-limited)

Status: design only, no product code. Owner idea 2026-10-24. Branch `design/spread-paint`.
Base: origin/main c253e9f043 (after #684).

## 0. The idea in one paragraph

A new paint tool, **Spread**. You press on the model and a seed lands where you click.
A painted patch starts as a small circle round the seed and keeps growing over the surface
for as long as the button is held. Releasing stops it. Growth respects an angle limit like
Smart fill's: the patch does not cross onto facets whose normal turns away by more than the
set angle. Colour (MMU) painting comes first, then support enforcer/blocker, fuzzy skin and
texture-displacement masks. Seam painting is left out (see D7).

Recommendation in short:
- **Algorithm:** grow a *surface (geodesic) distance* front from the seed, using a
  Dijkstra-ordered front with a fast-marching style "unfolding" update. It is exact on flat
  faces and does not jump across slots or between fingers. Build it incrementally, so each
  frame only processes the newly reached ring.
- **Painting:** cut boundary triangles with a new `GeodesicCursor` that plugs into the
  existing `select_triangle_recursive` machinery. Saved data keeps the same paint format, so
  there is no 3MF change.
- **Angle limit:** measured against the *neighbouring facet* by default, so it matches Smart
  fill and stops only at creases. A "relative to seed" checkbox caps total turning.
- **One undo step per press**, which the existing LeftUp/RightUp snapshot already gives.
- **New pieces:** a wall-clock timer, which painting has never needed because it was purely
  event-driven, and an adaptive tick so 2M-triangle meshes don't stall.

---

## 1. How painting works today

### 1.1 Data: `TriangleSelector` (src/libslic3r/TriangleSelector.{hpp,cpp})

- There is one `TriangleSelector` per model-part volume, built on its `TriangleMesh`.
  `m_neighbors = its_face_neighbors()` and `m_face_normals = its_face_normals()` are computed
  once in the constructor (TriangleSelector.cpp:1329).
- Paint state lives on a forest of triangles. The first `m_orig_size_indices` entries are the
  original facets. Any facet can be split recursively (1, 2 or 3 split sides) into children
  with their own `EnforcerBlockerType state`. `remove_useless_children()` merges children
  back when they agree, and `garbage_collect()` runs when more than half of the triangles are
  invalid.
- `EnforcerBlockerType` is an `int16_t`: NONE, ENFORCER/BLOCKER, or extruder index up to
  `ExtruderMax`. Edge's multi-nibble encoding (`read_leaf_state`) allows states above 16, so
  mixed filaments and big palettes round-trip. See tests/libslic3r/test_triangle_selector.cpp.
- `serialize()` / `deserialize()` produce `TriangleSplittingData` (`triangles_to_split`,
  `bitstream`, `used_states`). It is stored in `ModelVolume::{supported,seam,mmu_segmentation,
  fuzzy_skin}_facets` and in the eight `texture_displacement_facets_N` (Model.hpp:970-1016).
  The texture-displacement masks take their ids from the **secondary id range**
  (`ObjectBase::SecondaryId`, ObjectID.hpp:61, Model.hpp:992) so eight masks per volume don't
  burn primary ids. Spread writes through the same `FacetsAnnotation` objects, so it is
  unaffected.

### 1.2 Tools and the selector entry points

| UI tool (gizmo) | ToolType / CursorType | Selector call |
|---|---|---|
| Circle | BRUSH / CIRCLE | `select_patch(facet, Circle cursor, ...)`. BFS from the hit facet over `m_neighbors`; a neighbour is queued only if `select_triangle()` touched the facet and `Cursor::is_facet_visible` holds (it must face the camera, `n·dir < 0`, TriangleSelector.cpp:1084). |
| Sphere | BRUSH / SPHERE | Same BFS with the `Sphere` cursor (`is_facet_visible` is always true, so it paints back faces inside the ball). |
| Drag strokes | Capsule2D / Capsule3D | `DoublePointCursor::cursor_factory` joins consecutive mouse samples. |
| Triangle | BRUSH / POINTER | `bucket_fill_select_triangles(..., -1, propagate=false)`. Paints one leaf. |
| Height range (BBS) | BRUSH / HEIGHT_RANGE | `select_patch` with a `HeightRange` cursor. Start facets are *every* facet intersecting the slab (TriangleSelector.cpp:313). This is the only existing multi-seed use of `select_patch`. |
| Fill (MMU) | BUCKET_FILL | `bucket_fill_select_triangles(hit, facet, clp, angle, propagate=true)`. Floods leaves of the **same state** as the start leaf; it works on split leaves through `precompute_all_neighbors()`. The angle test is between *adjacent* leaves. "Edge detection" off sets `m_smart_fill_angle = -1` (GLGizmoMmuSegmentation.cpp:823). |
| Smart fill (supports, fuzzy, texture-displacement "area") | SMART_FILL | `seed_fill_select_triangles(hit, facet, trafo, clp, angle, overhang_deg)`. BFS over **original** facets; a neighbour joins if `clamp(n1·n2,0,1) >= cos(angle)` against the *current* facet (TriangleSelector.cpp:394-420). It also honours the clipping plane (`is_facet_clipped`) and the overhang filter (`world_normal_z < -cos(overhang)`). Selection is flagged `select_by_seed_fill()`, previewed on hover, and committed with `seed_fill_apply_on_triangles(state)`. |
| Gap fill (BBS) | GAP_FILL | Works on `TriangleSelectorPatch` patches (`update_triangles_per_patch`, `gap_area`). |

Behaviour worth knowing for Spread:
- **Overwrite semantics.** `select_triangle_recursive` (TriangleSelector.cpp:954) calls
  `undivide_triangle` on a fully covered facet and sets the new state. A brush therefore
  *always overwrites*, wiping any finer paint inside. Nothing today supports "paint only
  unpainted" except Fill, which floods same-state areas.
- **Edge limit.** `select_patch` resets the split edge limit whenever the cursor radius changes
  (`set_edge_limit(min(r/5, 0.05))`, TriangleSelector.cpp:296-306). Boundaries are therefore
  cut down to edges of 0.05 mm or less.
- **Smart fill's angle is neighbour-to-neighbour.** OrcaSlicer issue #15049 (closed as a
  duplicate of #14515) complains about exactly this: it "drifts a long way across continuously
  curved surfaces" and stops at a single steep triangle. This feeds decision D1.

### 1.3 GUI: `GLGizmoPainterBase` (src/slic3r/GUI/Gizmos/GLGizmoPainterBase.{hpp,cpp})

- `PainterGizmoType` covers FDM_SUPPORTS, SEAM, MM_SEGMENTATION, FUZZY_SKIN and
  TEXTURE_DISPLACEMENT (hpp:26). `ToolType {BRUSH, BUCKET_FILL, SMART_FILL, GAP_FILL}` and
  `m_smart_fill_angle = 30` with range 0..90 (hpp:255-293). Each gizmo shows its own subset:
  - MMU: Circle, Sphere, Triangle, Height Range, Fill, Gap Fill.
  - Supports: Circle, Sphere, Fill, Gap Fill.
  - Seam: Circle, Sphere.
  - Fuzzy: Circle, Sphere, Triangle, Fill (SMART_FILL).
  - Texture displacement: brush or area (SMART_FILL), with an erase mode
    (`GLGizmoTextureDisplacement.hpp:207`).
- `gizmo_event()` (cpp:658) drives everything:
  - The mouse wheel changes cursor radius, height or smart-fill angle. A wheel change in
    Smart fill re-runs the hover preview (cpp:679-699).
  - LeftDown/RightDown/Dragging run a raycast per mesh (`get_projected_mouse_positions`). They
    pick the state from `get_left_button_state_type()` / `get_right_button_state_type()`;
    MMU's right button is `-1`, meaning disabled. **Shift means erase**
    (`new_state = NONE` when `shift_down`, cpp:743).
  - They then call the tool's selector function and `request_update_render_data(true)`.
  - LeftUp/RightUp (cpp:968) take **one** `Plater::TakeSnapshot(..., GizmoAction)` and call
    `update_model_object()`. One stroke is one undo step, and the model is written once per
    stroke.
  - `on_mouse` (cpp:983) closes an open stroke if a Moving event arrives with no button down,
    because the release went elsewhere.
- **Painting is purely event-driven.** Nothing happens while the mouse sits still. Nothing in
  the painter owns a timer; the only gizmo `wxTimer` is `GLGizmosManager::m_timer_set_color`.
  `GLCanvas3D::schedule_extra_frame(ms)` / `request_extra_frame()` (GLCanvas3D.hpp:1309-1311)
  exist for animations.
- **Axis locks (Edge, 5B-5 / #679, port of Orca #14262).** `m_vertical_only` /
  `m_horizontal_only` freeze the mouse x or y while *dragging* a BRUSH
  (GLGizmoPainterBase.cpp:815-821). The checkboxes are in the MMU gizmo (cpp:899-907),
  supports (FdmSupports.cpp:359-390) and seam (Vertical only, Seam.cpp:290). They don't apply
  to Spread, which has no drag path; the Spread panel hides them.
- **Rendering.**
  - MMU, supports and others use `TriangleSelectorPatch`. `update_render_data()` (cpp:1584)
    rebuilds every leaf of every state into per-state vertex/index arrays
    (`update_triangles_per_type`, cpp:1386) and re-uploads the VBOs on *every* paint change.
    That is O(all leaf triangles) per update; with the wireframe flag it pushes 6 floats per
    vertex instead of 3.
  - The seed-fill preview is drawn as a contour (`update_paint_contour` /
    `get_seed_fill_contour`).
  - The brush already pays this full rebuild on every drag event, which matters for the
    performance plan (§4).

### 1.4 Edge additions that touch painting

- **Physical palette and mixed filaments.** In the MMU gizmo, `m_display_filament_ids` maps
  palette slots to filament ids, mixed filaments included (GLGizmoMmuSegmentation.cpp:150-175,
  531-582, 1148). The swatches render mixed gradients (`MixedFilamentDisplayContext`). Spread
  needs nothing extra: it paints `get_left_button_state_type()`, like every tool.
- **Image Fill / ImageMap** (`Plater::apply_image_fill`, Plater.cpp:28374; libslic3r/ImageFill,
  ImageRowWalls). This is a separate, dialog-driven writer of `mmu_segmentation_facets` and
  doesn't interact.
- **Texture-displacement masks** (8 slots, SecondaryId). `GLGizmoTextureDisplacement` is a
  `GLGizmoPainterBase`, so it gets Spread for the price of a button (PR 3).
- **Paint CLI** (`slic3r/Utils/PaintCLI.hpp`, `--inspect-paint`). This is useful for scripted
  checks of results; it reads the same data.

---

## 2. Algorithm options for growth over time

The press fixes a seed **s** (hit point, mesh index, original facet `f0`), a target state, an
angle limit θ and an *allowed* predicate. The predicate covers the angle, the clipping plane,
the overhang-only filter and, optionally, "keep existing paint". The patch at time t is
`{ p on the surface : allowed(p) and dist(s, p) <= r(t) }`.

The options differ in what `dist` is.

### Option A: Euclidean sphere clipped to the angle-limited connected region

- Compute the allowed region once (a BFS like `seed_fill_select_triangles`, into a private
  `std::vector<uint8_t>`, not the seed-fill flags). Each tick, call `select_patch` with a
  `Sphere(s, r(t))` and add a mask check in the BFS.
- Incremental version: keep the previous tick's boundary facets as the start set; the
  multi-start path already exists for HeightRange.

Good:
- Almost free to write: the Sphere cursor and splitting exist.
- Perfect circles on flat faces.
- Deterministic.

Bad:
- **Jumps across concave gaps.** The BFS only requires a path *inside the ball*. Take a slot
  2 mm wide whose walls are inside the allowed region (organic models, or a large θ). The far
  wall gets painted as soon as r reaches the slot bottom, even though ink would have to travel
  down and back up. The same goes for the inside of a curled ear or adjacent fingers joined
  through a palm. The result looks like "spray paint", not "spreading ink".
- **Runs ahead around tight curvature.** On a cylinder of radius R, a ball covers
  circumferential arc `2R·asin(r/2R)` measured as chord, so it wraps the whole circumference at
  r = 2R where ink needs πR. On small rounded features the patch looks elongated and blotchy.
- Re-evaluating a growing ball from scratch each tick is O(painted area). It needs the
  front-only trick to be O(new ring).

### Option B: geodesic (surface) distance, grown as a front (recommended)

- Keep a distance per **original mesh vertex** within the allowed region and grow it in
  increasing order with a priority queue.
- The queue persists across ticks: each tick pops until `top > r(t)`. Settled vertices never
  change, so the total cost of a whole press is one Dijkstra over the reached region, spread
  across frames.

Distance quality options, in increasing accuracy:
1. **Edge-graph Dijkstra.** Too crude. On CAD meshes (big flat faces of a few slivers) the
   iso-lines become polygons; on regular grids the metrication error is up to about 8%.
2. **Dijkstra with a fast-marching triangle update** (Kimmel–Sethian 1998; the planar
   "unfolding" update used by Novotni–Klein). Recommended.
   - When vertex w is relaxed through facet (u, x, w) with u and x settled, place the virtual
     source Q in the facet's plane, on the far side of edge ux, with |Q−u| = d_u and
     |Q−x| = d_x.
   - If the segment Q→w crosses ux, `d_w = |w−Q|`. Otherwise fall back to
     `min(d_u+|uw|, d_x+|xw|)`.
   - Initialisation: the three vertices of the seed facet get exact Euclidean distances to s.
   - **Exact on any planar region**, since Q is then the seed itself, so circles on flat faces
     come out as circles.
   - Unfolds correctly over folds and curves.
   - Obtuse slivers fall back to the edge path; a tiny overestimate is acceptable.
   - About 150 lines.
3. **Exact MMP / heat method.** Rejected.
   - MMP windows are heavy (memory and code).
   - The heat method needs a global sparse Cholesky solve (seconds and hundreds of MB at 2M
     triangles) and isn't incremental.

**Sub-facet boundary.** A `GeodesicCursor : Cursor` decides inside/outside for any point,
including split vertices. Its overrides take the `Triangle&`, so `tr.source_triangle` is
known:
- `vertices_inside(tr, vertices)`
- `is_edge_inside_cursor(tr, vertices)`
- `is_pointer_in_triangle`: true only for the seed's leaf.

For a point p in original facet f, evaluate `d(p) = min over the edges e=(u,x) of f with both
endpoints settled of |p − Q_e|`, using the same unfolding as above (vertex fallback when no
edge qualifies).
- On planar regions this is exactly |p − s|.
- For p on a shared edge, |p − Q_e| is the same from both adjacent facets, because rotating Q
  about the hinge preserves distances to points on the hinge line. Split midpoints shared by
  two facets therefore get consistent answers, with no paint slivers along original edges.
- Existing `select_triangle_recursive` then splits exactly as it does for the brush, down to
  the edge limit.

**Allowed region, lazily.** A facet is entered only when the front first relaxes into it, so
no upfront O(N) BFS is needed and the press has no hitch on 2M meshes. The entry test is:
- angle (neighbour or seed mode, D1),
- `is_facet_clipped`,
- the overhang filter,
- optionally "not fully painted another colour" (D6).

A vertex is relaxed only through allowed facets, so a vertex on the boundary between allowed
and blocked facets still gets a distance. Blocked facets are never painted.

**Painting per tick.** Facets are touched in two ways:
- Newly **fully inside** facets (all 3 vertices have d ≤ r, plus a curvature margin check of
  the edge-midpoint d) are set as whole facets: `undivide` and set state, the same as the
  brush's "3 vertices inside" path.
- **Front facets** (some vertices inside) are re-run through `select_triangle` with the
  `GeodesicCursor` at the new radius.

The session keeps the list of front facets. The next tick revisits only those plus the newly
reached ones.

**Comparison (why B):**

| | A: Euclidean ∩ region | B: geodesic front |
|---|---|---|
| Flat faces | exact circle | exact circle (unfolding) |
| Curved and thin parts | runs ahead round tight curves; jumps across slots and between near surfaces | follows the surface; no jumps |
| Cost per tick | O(front) with the incremental trick (otherwise O(area)) | O(new ring · log) + O(front) splitting |
| 500k–2M triangles | fine | fine; Dijkstra over 1M vertices ≈ 50–100 ms *in total*, spread over the press |
| Undo | one snapshot per press | same |
| Determinism | yes | yes (single thread, ties broken by vertex index) |
| Code | ~200 lines | ~500 lines + cursor |

Both share all the infrastructure (session object, mask, front list, timer, GUI). PR 1 builds
B but also carries an `Euclidean` field mode behind the same interface: it is the test oracle
on planar regions and a fallback (D2).

**Edge cases:**
- Non-manifold edges: `its_face_neighbors` picks one neighbour, so the front simply doesn't
  cross elsewhere.
- Open edges (`-1`): the front stops there.
- Degenerate facets (zero normal): treat as allowed only in neighbour mode with the
  neighbour's normal.
- Spread stays on the clicked **part**. Selectors are per volume and multi-part objects have
  no cross-mesh adjacency (D11).
- Scale: distances use world-space positions (`trafo_no_translate` applied to vertex
  positions as they are read), so speed and max radius are in world mm even under
  non-uniform scale. This matches what `Cursor` does when `uniform_scaling` is false.

---

## 3. UX

**Tool button.** Add a "Spread" button (new icon `spread_paint`, registered next to
`FillButtonIcon` / `GapFillIcon` in ImGuiWrapper.cpp:71-74) after Fill in MMU. Panel controls:
- **Spread speed:** slider 0.25×–4×, default 1×.
- **Max radius:** mm, with "∞" meaning until the region runs out; default ∞.
- **Angle limit:** reuses `m_smart_fill_angle` and its slider, 0–90°, default 30°. MMU's
  "Edge detection" off (−1) means no angle limit.
- **"Relative to seed"** checkbox (D1).
- **"Keep existing paint"** checkbox (D6).
- The clipping-plane slider, as other tools have.
- The axis-lock checkboxes are hidden.

**Gestures:**
- Left press: spread the selected colour (or enforcer, fuzzy, mask).
- Right press: spread the right-button state where the gizmo has one (support blocker). MMU's
  right button stays disabled.
- **Shift + press: erase-spread** (state NONE), the same rule as every existing tool. Alt is
  not used; it would collide with nothing, but there is no need.
- **Mouse motion while held is ignored**: the seed is fixed (D5). The press can be "stuck" on
  a still mouse, which is the point.
- Mouse wheel while held changes the max radius live; without a press it does what it does
  today for the angle (re-preview).
- **Esc while held** cancels the press and restores the paint from before it (D8).
  Implementation: `serialize()` at press; on Esc, `deserialize()` and take no snapshot.

**Speed (D3).** The recommendation is screen-relative:
- At press, compute world mm per screen pixel at the seed depth from the camera.
- `v_mm = k · px_per_mm⁻¹ · speed_factor`, with k ≈ 120 px/s.
- Zoomed in, the spread is slow and precise; zoomed out, it covers a big part quickly. It
  behaves the same on a 20 mm figurine and a 250 mm helmet without a model-size heuristic.
- The value is fixed for the press, since you cannot zoom while holding anyway.

**Growth curve (D9).** Choose between:
- *constant radial speed*, `r = r0 + v·t`, or
- *constant area rate*, `r = sqrt(r0² + c·t)`, which feels like ink because it slows as it
  widens.

The recommendation is radial speed with a short ease-in (first 150 ms), because people judge
the edge, not the area. r0 is about 6 px in world mm, so the initial "small circle" is
visible immediately on press.

**Preview:**
- *Hover:* draw the same 2D ring as the Circle brush at r0 so the seed spot is obvious. If
  the region preview is cheap enough (D10), also show the smart-fill-style allowed-region
  contour (`seed_fill_select_triangles` + `update_paint_contour`) so the user sees where the
  spread can go.
- *Held:* a small ImGui label by the cursor ("12.4 mm"). When a max radius is set, a faint
  ring at the max radius, drawn with the existing `render_cursor_sphere`.

**Overwrite vs only unpainted (D6):**
- *Paint over* (default, consistent with brushes): overwrites everything it reaches,
  including finer paint inside fully covered facets.
- *Keep existing paint*:
  - Leaves painted in another non-NONE state are left alone. This needs a `protect` flag in
    `select_triangle_recursive`: for a fully inside but split facet, recurse instead of
    `undivide`; skip leaves whose state is neither NONE nor the target.
  - Original facets that are **entirely** painted another colour act as barriers to the
    front, so you can outline with the brush and spread inside.
  - Limitation: a brush line thinner than one original facet does not block the front (the
    front works on original vertices). The paint under the line is kept, but the spread can
    leak past it through the rest of the facet. Document it in the tooltip; precise outlining
    needs a facet-resolution line or the Fill tool.

**Undo.** One press is one undo step: the LeftUp/RightUp branch (cpp:968) already snapshots
and calls `update_model_object()` once. Ticks call only `request_update_render_data(true)`.
The snapshot name reuses `handle_snapshot_action_name`. An optional wording tweak:
"Spread using: Filament 3".

---

## 4. Performance plan

**Budget.** A tick should cost at most 8 ms on 500k triangles (60 fps with headroom) and at
most 30 ms on 2M. The tick is driven by wall clock, so slower machines get bigger, coarser
steps, not slower spreading.

**Timer.**
- A `wxTimer` owned by `GLGizmoPainterBase`, started on a Spread press and stopped on
  release, cancel or gizmo close (`on_set_state`).
- Each tick:
  1. computes r from `std::chrono::steady_clock`,
  2. `session.grow_to(r)`,
  3. `request_update_render_data(true)`,
  4. `m_parent.set_as_dirty()`, `request_extra_frame()`.
- **Adaptive interval:** `next = clamp(1.5 × last_tick_cost, 16 ms, 100 ms)`.
- `on_mouse`'s "release went elsewhere" path (cpp:987) must stop the timer as well.

**Where the time goes:**
1. *Front advance:* pop and relax with an unfolding update. That is O(Δvertices·log); 10k new
   vertices per tick is under 2 ms.
2. *Painting:*
   - Fully covered facets are O(Δfacets) state sets.
   - Front facets are re-split each tick: about perimeter / edge-limit leaves; for a 300 mm
     front at 0.05 mm, roughly 25k leaves.
   - The edge limit is **fixed per press**: `max(0.05, 1 px at seed depth)`, so it isn't
     reset by radius changes as `select_patch` does. Leaves left by the previous front are
     merged by `undivide` once covered.
   - `garbage_collect()` runs on its normal trigger.
3. *Render-buffer rebuild:* the dominant cost today. `TriangleSelectorPatch::update_render_data`
   rebuilds and uploads all N leaves per update. At 2M triangles that is about 72 MB of floats
   per tick (≈ 50–150 ms). The brush already has this cost on every drag event.
   - **Step 1 (PR 2):** accept it and let the adaptive interval absorb it.
   - **Step 2 (PR 4, perf):** a *press overlay*. During a press, keep the base VBOs from the
     press start and draw only the **touched original facets** of the session in a small
     overlay `GLModel`, with polygon offset to win the depth test. Rebuilding the overlay is
     O(touched) per tick, not O(N). Erase overlays draw the NONE colour (MMU: the volume's
     base extruder colour; supports and fuzzy: the plain mesh colour). On release, do one full
     rebuild.
   - **Step 3 (optional, helps every tool):** a two-pass parallel `update_triangles_per_type`
     (count per state, prefix sum, `tbb::parallel_for` fill).
4. *Contour:* `update_paint_contour` is only needed for the hover preview, so skip it during a
   press.

**Caching.**
- The session keeps:
  - `dist[]`, a float per original vertex, allocated lazily as an unordered map or as a full
    vector when the mesh has 2M facets or fewer. A full vector is 4 MB per 1M vertices, which
    is fine.
  - the heap,
  - `facet_allowed[]`, uint8 per original facet, filled lazily,
  - the front facet list,
  - the touched facet list.
- It is dropped on release.
- There is **no** cross-press cache. The allowed region depends on angle, mode, clipping and
  paint, and it is filled lazily anyway.

**Threading.** `TriangleSelector` mutates shared vertex and triangle arrays and isn't thread
safe; splitting stays on the UI thread. Dijkstra is inherently sequential and cheap. The only
worthwhile parallelism is the buffer build (step 3). A worker-thread session was considered
and rejected: it adds locking against render reads for no gain once the overlay exists.

---

## 5. Upstream check (facts only, 2026-10-24)

- **OrcaSlicer** (`orcaupstream/main` 46b308c832, 2026-10-25): `ToolType` is still {BRUSH,
  BUCKET_FILL, SMART_FILL, GAP_FILL}, and `CursorType` {CIRCLE, SPHERE, POINTER,
  HEIGHT_RANGE, GAP_FILL}. There are no commits touching TriangleSelector or Gizmos matching
  geodesic, spread or grow. Issue/PR searches for "spread paint", "grow selection", "geodesic",
  "paint grow", "flood fill paint" and "airbrush" returned nothing relevant. Related issues:
  - #15049 "Smart Fill: better boundary/shape detection + edge smoothing option": closed as a
    duplicate of #14515. It notes the neighbour-only angle test drifts on curved surfaces.
  - #14515 "Blur or Smoothing Tool": open.
- **BambuStudio** (local clone C:/Dev/BambuStudio at 79df7ae30, 2026-10-13): same `ToolType`
  and `CursorType` sets, and nothing similar in history. Its issues #10959 "Colour Painting"
  and #11006 "Bambu Studio - Painting" are general feature threads with no spread tool.
- **PrusaSlicer** (master via the GitHub API): `ToolType {BRUSH, BUCKET_FILL, SMART_FILL,
  HEIGHT_RANGE}`. Nothing similar; #7111 (graphics-tablet painting) is unrelated.
- **Snapmaker Orca** (`snapmaker-upstream/main` 229efa64b8, 2026-10-23): same enums as Orca;
  no matches.

There is nothing to port: this is an Edge-original tool. The upstream code is GPL/AGPL, but
only enum names were looked at, and the algorithm references are published papers.

---

## 6. PR split, tests, decisions

### PR split
1. **`feat/spread-paint-core`** (libslic3r only, unit-tested, no GUI)
   - New `src/libslic3r/SpreadPaint.{hpp,cpp}`:
     `class SpreadSession { SpreadSession(TriangleSelector&, const SpreadParams&); void grow_to(float r_mm); bool exhausted() const; float max_reached() const; const std::vector<int>& touched_facets() const; }`
   - `SpreadParams{hit, facet, trafo_no_translate, clp, state, angle_deg, AngleMode{Neighbour, Seed}, overhang_deg, keep_existing, DistanceMode{Geodesic, Euclidean}, edge_limit}`.
   - Tiny hooks in `TriangleSelector`:
     - `friend class SpreadSession`,
     - a `protect_painted` flag through `select_triangle` / `select_triangle_recursive`,
     - `select_triangle_with_cursor(facet, cursor, ...)` that doesn't touch `m_old_cursor_radius_sqr` or the edge limit,
     - the `GeodesicCursor` class.
   - No serialization change.
2. **`feat/spread-paint-mmu`** (GUI)
   - `ToolType::SPREAD` in `GLGizmoPainterBase`: timer, press/tick/release/cancel, ring and
     label, speed, max radius, wheel; the "release went elsewhere" path.
   - The MMU panel button and controls, the icon, and the `m_desc` strings, which go through
     gettext; the gettext list update follows the house recipe.
3. **`feat/spread-paint-more-gizmos`**: Spread buttons in Supports (left enforcer, right
   blocker; honours "highlight overhang only"), Fuzzy skin and Texture displacement (area
   tool; erase mode maps to NONE). Seam only if D7 says so.
4. **`perf/paint-press-overlay`** (optional): the overlay during a press plus the parallel
   `update_triangles_per_type`. It helps the brush too.

PRs 1 and 2 can be one build together; PR 1 needs only `libslic3r_tests`. None of these
change G-code by construction, since paint only changes when a user spreads. A G-code gate
is still cheap insurance on PR 1, because it touches `select_triangle_recursive`.

### Test plan
New file `tests/libslic3r/test_spread_paint.cpp`, tags `[TriangleSelector][Spread]`. Meshes
are built in code (subdivided plane, cube, faceted cylinder, U-channel, thin box). The API
takes a radius, not time, so everything is deterministic.

1. **Flat disc:** a 100×100 plane subdivided into 2 triangles, then into 20k. Seed at the
   centre, r = 10.
   - Painted area (from `get_facets(state)`) is within 3% of π·100.
   - Every painted vertex lies within r + edge_limit of the seed.
   - Geodesic and Euclidean agree within 1%.
   - On the 2-triangle plane, the boundary must still be round. This is the unfolding check.
2. **Seed-relative angle:** a cube, seed on the top, θ = 30, r = 1000. Only the top face is
   painted: area equals the face area and no side normals appear.
3. **Neighbour vs seed:** a 36-segment cylinder (10° per step), θ = 15, seed on the mantle.
   - Neighbour mode wraps the whole mantle (it stops at the caps, which are at 90°).
   - Seed mode paints only facets with normal within 15° of the seed normal.
4. **No jumping:** a U-channel with a slot 2 mm wide and 10 mm deep, θ = 90 neighbour mode,
   seed on one wall 1 mm below the rim.
   - With r = 5, Geodesic leaves the opposite wall unpainted and Euclidean paints it.
   - This documents why B is the default.
5. **Thin wall:** 0.8 mm box wall, θ = 30, seed on the outer face, r = 100. The inner face is
   never painted.
6. **Incremental equals one-shot:** `grow_to(3); grow_to(7); grow_to(10)` on one session,
   compared with a fresh session `grow_to(10)`. `serialize()` must be bit-identical.
7. **Monotonic:** the painted set at r1 is contained in the painted set at r2, by sampling
   points or comparing per-facet area.
8. **Keep existing:**
   - Pre-paint a facet-wide stripe with state 2, then spread state 3 with
     `keep_existing = true`. The stripe is still state 2 and blocks the front (the far side
     stays unpainted when the stripe closes a loop).
   - With `false`, the stripe is overwritten.
9. **Erase:** spread NONE over a painted plate; the area within r is unpainted and
   `used_states` is updated.
10. **Filters:** an active clipping plane and `overhang_deg` both exclude facets exactly as
    Smart fill does. Cross-check against `seed_fill_select_triangles` on the same mesh with
    θ, r = ∞: the facet sets must be equal in neighbour mode.
11. **Exhaustion:** r = ∞ on a closed region sets `exhausted()`; further `grow_to` calls are
    no-ops (no change in triangle count).
12. **Round-trip:** the result survives `serialize()`/`deserialize()` with states above 16
    (reuse the multi-nibble fixture).
13. **Benchmark** `[.][Benchmark]` (not gating): a 1M-facet sphere, growing to full in 100
    steps; log ms per step and the total.

**GUI hand test** (owner card, PR 2/3):
- A Benchy hull from a side press: holding grows a round patch that stops at the deck edge
  at 30°.
- Shift-press erases.
- Esc mid-press restores.
- Ctrl+Z after a press undoes the whole spread in one step.
- A mixed filament spreads with its gradient swatch colour.
- A 2M-triangle scan stays responsive (the spread visibly continues even if choppy).
- Support enforcer spread with "overhang only".
- A texture mask slot spread.

### Open decisions for the owner
- **D1 Angle reference.** Neighbour-to-neighbour (recommended default; same as Smart fill;
  stops at creases, wraps smooth curves) or relative to the seed normal (bounded, "stay on
  this face"). Recommendation: neighbour by default, with a "Relative to seed" checkbox.
- **D2 Algorithm.** Geodesic front (recommended) or ship the Euclidean sphere first as an
  MVP. Recommendation: geodesic in PR 1, with Euclidean kept as the test oracle.
- **D3 Speed model.** Screen-relative px/s at seed depth (recommended), fixed mm/s, or scaled
  by model size. Also the default speed.
- **D4 Max radius.** Default ∞ or a number, and whether the wheel adjusts it while holding.
- **D5 Moving the mouse while held.** Seed fixed (recommended) or the seed follows the mouse,
  as a "drip trail" with multiple seeds.
- **D6 Overwrite.** Default "paint over" (recommended), and whether "keep existing paint" also
  makes painted facets barriers (recommended yes).
- **D7 Which gizmos.** MMU first; then supports, fuzzy skin and texture masks. Seam: skip
  (strokes are thin) unless wanted.
- **D8 Esc cancels the press.** Recommended yes.
- **D9 Growth curve.** Constant radial speed with ease-in (recommended) or constant area rate.
- **D10 Hover preview of the reachable region.** Show the Smart-fill-style contour on hover
  (costs a region BFS per hover move, as Smart fill already does) or just the seed ring.
- **D11 Multi-part objects.** Spread stays on the clicked part (recommended) or tries to
  continue onto touching parts (no adjacency exists today; not proposed).
- **D12 Placement.** A separate "Spread" tool button (recommended) or a mode of Fill.
