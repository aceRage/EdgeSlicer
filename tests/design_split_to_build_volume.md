# Design spike: split a large model to the printer's build volume

Date: 2026-11-15. Status: owner accepted every recommendation D1-D17 on 2026-11-15 ("go with the recommendations"). P1 built on `feat/split-to-fit-p1` (section 7, "P1 as built"); cut-face labels added as P5 (section 7.5).
Tree read: main at `a21acf2e87`. Every `file:line` below is from that commit.

The owner's ask: a cut-like tool that cuts a very large model into many parts based on a printer's build volume, with configurable margins, automatic dowels (up to X of a chosen shape) where the cuts land, and slight automatic adjustment of the cut planes, with warnings when an adjustment would exceed the build volume. LuBan and similar splitters are inspiration only.

## 0. Short answer

- **Feasible, and mostly a planning and UI job.** The per-plane cut, the connector geometry, paint carry-over, assembled poses and multi-plate arrange all exist. What is missing is: many planes at once, automatic connector placement, and plane adjustment.
- **Shape:** a new small gizmo ("Split to Fit") with a pure planner in libslic3r. Apply runs the existing `Cut::perform_with_plane()` repeatedly (bisection order) inside a background job, then arranges the parts over plates.
- **Size:** about 14 to 17 agent-days over four shippable phases.
- **Biggest risk:** connectors and repeated cuts interacting (a dowel near a corner where two planes meet, and how already-made connector volumes are handed on by the next cut). It is contained by a keep-out rule and by tests, but it is the part that has never been exercised.

## 1. What exists today (verified)

### 1.1 Cut engine (libslic3r)

| Item | Where | Note |
|---|---|---|
| `Cut` class: one object, one instance, one cut matrix | `src/libslic3r/CutUtils.hpp:220-346` | Six entry points; the flat one is `perform_with_plane()` (`:306`). |
| Flat cut body | `src/libslic3r/CutUtils.cpp:2046-2170` | Clones the object into upper/lower (`:2089-2095`), cuts every model part, hands modifiers and connectors on, post-processes, `finalize()`. |
| Mesh cut per volume | `CutUtils.cpp:533-561` | Copies the mesh, transforms it into the plane frame, calls `cut_mesh()`. |
| `cut_mesh()` | `src/libslic3r/TriangleMeshSlicer.cpp:2450` | One plane at height z. Single-threaded loop over all facets (`:2480`). Each half first gets a copy of **all** vertices (`:2460`, `:2466`), caps are triangulated by `triangulate_slice()` (`:2288`), unused vertices dropped at `:2417`. No boolean library involved. |
| The `Cut` object owns a copy of the source object | `CutUtils.cpp:913-919` | `m_model.add_object(*object)`. Results are owned by the `Cut` and freed in its destructor (`CutUtils.hpp:268`), so callers copy them out. |
| Instance transform after a cut | `CutUtils.cpp:872-910` | Rotation and scale are baked into the meshes; only the offset (and Z rotation of other instances) stays on the instance. |
| Empty half is dropped | `CutUtils.cpp:991-1000` | `keep && !object->volumes.empty()`. |
| Paint survives a cut | `CutUtils.cpp:1017-1035`, `:1158-1168` | `KeepPaint` saves colour / support / seam / fuzzy paint and remaps it onto every result. The gizmo always sets it (`GLGizmoCut.cpp:10960-10961`). Airbrush field carried too (`CutUtils.cpp:1170-1192`). |
| Modifiers through a cut | `CutUtils.cpp:822-843`, `:2142-2159` | Not cut; copied to the side(s) their bounding box is on, then dropped if they miss the half. |
| Assembled pose of every result | `CutUtils.cpp:936-947`, `:1147-1152`; `src/slic3r/GUI/Plater.cpp:25633-25645` | Each half, dowel and pin remembers where it sat in the uncut object. The Assemble view has an explosion ratio already (`src/slic3r/GUI/GLCanvas3D.hpp:727`, `GLCanvas3D.cpp:6020-6027`). So "assembly preview / exploded view" needs no new data. |

### 1.2 Connectors

| Item | Where | Note |
|---|---|---|
| Types | `src/libslic3r/Model.hpp:260-281` | Plug, Dowel, Snap, FlexiJoint, PushFit ("Pin"), TacJoint. |
| Styles / shapes | `Model.hpp:292-306` | Prism, Frustum / Triangle, Square, Hexagon, Circle. |
| `CutConnector` (pos, rotation, radius, height, tolerances, z_angle) | `Model.hpp:343-382` | Defaults radius 5, height 10, height tolerance 0.1 (`:360`). |
| Dowel handling in the cut | `CutUtils.cpp:767-819` | A separate dowel **object** per connector (`:768-785`), and the dowel volume halved into a hole for each side, each reaching 0.05 mm through its face (`:807-818`, `CutUtils.hpp:21`). Holes are `NEGATIVE_VOLUME`s resolved at slicing time, so no mesh boolean runs. |
| Plug / Snap handling | `CutUtils.cpp:741-766` | Hole in the upper half, solid part in the lower half. |
| Tolerance | `CutUtils.cpp:455-488` | Hole grows by radius and height tolerance. |
| Push-fit Pin | `src/libslic3r/PushFit.hpp:49-73`, `CutUtils.cpp:573`, `CutUtils.hpp:158` | Separate printed pin, default diameter 6 mm, 7 mm into each side, 0.10 mm radial clearance, optional detent lip. Pins are laid out in a row beside the object (`CutUtils.cpp:949-981`). PRs #693 / #698 / #699. |
| Does a connector fit where it stands? | `CutUtils.hpp:88-114` | `cut_connector_fit()` and `cut_connector_footprint_radius()`: reusable as the footprint check for automatic sites. |
| Baking connector definitions into volumes | `src/slic3r/GUI/Gizmos/GLGizmoCut.cpp:11752-11788`, `:9929-10017` | **GUI-only** for Plug / Dowel / Snap (`apply_cut_connectors`, `get_connector_mesh`, declared at `GLGizmoCut.hpp:1319-1320`). The Flexi / Pin / TAC equivalents are already in libslic3r (`CutUtils.hpp:149`, `:158`, `:162`). The existing headless test builds the Dowel volume by hand (`tests/libslic3r/test_cut_connectors.cpp:214`). |
| Connector mode enum has an `Auto` value | `GLGizmoCut.hpp:959-962`, default Manual at `:968` | What `Auto` does today: **unverified** (only seen as a guard at `GLGizmoCut.cpp:9719`, `:11602`). It is not automatic placement on a cross-section. |
| Gizmo defaults | `GLGizmoCut.hpp:880-884` | Size 2.5, depth ratio 3, depth tolerance 0.1. |

### 1.3 How cut parts stay linked

- `ModelObject::cut_id` (`Model.hpp:496`) is a `CutObjectBase`: id + check sum + connector count (`src/libslic3r/ObjectID.hpp:159-201`).
- A **plain cut with no connectors is not linked at all**: the gizmo passes `InvalidateCutInfo` (`GLGizmoCut.cpp:10958-10959`) and `update_object_cut_id()` returns early (`GLGizmoCut.cpp:10037-10043`).
- With connectors, every result shares the id, the check sum grows by the number of extra objects (`:10046-10055`), and `synchronize_model_after_cut()` copies it to every object with the same id (`:10084-10089`). Cutting an already-cut object again therefore keeps one group.
- Being "cut" has consequences: no size / scale edits (`src/slic3r/GUI/GUI_ObjectList.cpp:3986-3998`), a warning on delete that then unlinks the whole group (`Plater.cpp:16064-16082`).
- Stored in the 3MF as `Metadata/cut_information.xml` (`src/libslic3r/Format/bbs_3mf.cpp:209`, write `:8995-9023`, read `:3056-3092`). This is the upstream schema.

### 1.4 Re-editable cuts (EdgeSlicer only)

- `CutRecipe` (`src/libslic3r/CutRecipe.hpp:330-465`): one recipe = one cut, plus the **whole pre-cut mesh** (`:416`), stored once per recipe in `Metadata/cut_recipe/` (`CutRecipe.hpp:18-28`, `bbs_3mf.cpp:218-221`). Version 9 (`CutRecipe.hpp:144`). Both halves carry the same recipe (`GLGizmoCut.cpp:11073-11086`).
- It describes exactly two halves. A grid of N planes would need a chain of recipes, each with its own intermediate mesh, and "Edit cut" (`GLGizmoCut.cpp:10528`) could only re-open the last cut of a pair that still exists. **Not reusable as-is for a grid** (see 4.4).

### 1.5 Build volume, plates, arrange

| Item | Where |
|---|---|
| `printable_area`, `bed_exclude_area`, `printable_height` | `src/libslic3r/PrintConfig.cpp:919`, `:926`, `:989` |
| `extruder_printable_area`, `extruder_printable_height` (H2D / H2C reach) | `PrintConfig.cpp:4866`, `:4872` |
| `wrapping_exclude_area` | **Not defined in this tree** (no `add("wrapping_exclude_area")` in `PrintConfig.cpp`). Ignored by this design. |
| `BuildVolume` (type, bounding volume, `object_state`) | `src/libslic3r/BuildVolume.hpp:15`, `:34-54`, `:64-83` |
| GUI access | `src/slic3r/GUI/3DBed.hpp:137` (`build_volume()`), `src/slic3r/GUI/PartPlate.hpp:485` (`get_build_volume()`), `:494` / `:863` (exclude areas), `:956` (`get_extruder_areas()`) |
| Shared nozzle-reach area | `src/libslic3r/ExtruderAreas.hpp:26-37` (`shared`), `:70` (`extruder_areas_from_config`); already used to keep the wipe tower where both nozzles reach (`src/slic3r/GUI/Selection.cpp:1241-1250`) |
| Arrange over plates | `Plater::arrange()` (`Plater.cpp:29786`) runs `ArrangeJob` (`src/slic3r/GUI/Jobs/ArrangeJob.hpp:18`, `prepare_selected()` `:45`). Items get a `bed_idx`; items that fit nowhere are reported (`ArrangeJob.cpp:705-719`). Limit 36 plates (`PartPlate.hpp:39`). Rotation is off by default (`src/libslic3r/Arrange.hpp:121`); brim / skirt allowance is a parameter (`Arrange.hpp:130-132`). |
| Arrange after a cut | Deliberately not done today (`Plater.cpp:25659-25661`, commented out). |

### 1.6 Mesh helpers and jobs

- Cross-sections without cutting: `slice_mesh_ex()` (`src/libslic3r/TriangleMeshSlicer.hpp:72`), many heights in one pass, TBB-parallel (`TriangleMeshSlicer.cpp:601`, `:1031`).
- Shell counting and splitting: `its_split`, `its_number_of_patches`, `its_num_open_edges`, `its_volume` (`src/libslic3r/TriangleMesh.hpp:221-232`, `:321`); `ModelObject::split()` (`src/libslic3r/Model.cpp:2481`).
- Booleans exist (CGAL, mcut and others: `src/libslic3r/MeshBoolean.hpp:27`, `:87`, `:120-131`) but **this tool needs none**: planar cuts use `cut_mesh()`, dowel holes are negative volumes.
- Non-manifold results: the gizmo checks open edges after a cut and offers the Windows repair (`GLGizmoCut.cpp:11014-11055`).
- Background work: `Job` (`src/slic3r/GUI/Jobs/Job.hpp:17`, `process` `:54`, `finalize` `:63`), cancel via `ctl.was_canceled()`. Today's cut runs on the UI thread under a busy cursor (`GLGizmoCut.cpp:10946`).
- No largest-inscribed-circle / pole-of-inaccessibility helper exists in libslic3r (searched for `polylabel`, `inscribed`). Placement needs its own small routine.
- Cut speed on multi-million-triangle meshes: **not measured** in this spike.

### 1.7 Reuse versus missing

| Reuse as-is | Missing |
|---|---|
| One flat cut with any number of connectors on it | Planning many planes from a volume |
| Dowel and Pin geometry, tolerances, hole overlap | Automatic sites on a cross-section |
| Paint, modifiers, per-part settings through a cut | Plane adjustment and its warnings |
| Assembled poses, Assemble view explosion | Headless connector baking for Plug / Dowel (move out of the gizmo) |
| cut_id linking, 3MF storage | Naming by grid cell; one undo step for the whole split |
| ArrangeJob over plates | Running arrange on the new parts after the split |

## 2. User flow

Case: a 600 x 200 x 300 mm prop, a 256 x 256 x 256 mm printer.

1. The prop loads red (outside the volume). The user selects it and opens **Split to Fit** (toolbar gizmo; also a "Split to fit the printer..." button in the Cut panel and in the object's right-click menu).
2. The tool reads the current printer's volume, takes off the margins, and shows at once: usable size 246 x 246 x 251, a grid of translucent planes on the model (2 on X, 1 on Z), and "6 parts".
3. Each part is listed with its bounding size. Parts that fit are plain; a part that does not fit is red.
4. The user may: tick or untick axes, change margins, switch even split / full size first, drag a plane, pick the dowel settings, turn automatic adjustment on.
5. Dowel sites show as small discs on the planes; the count per plane and the total are listed.
6. Warnings sit in a list at the bottom of the panel; clicking one zooms to the plane or part.
7. **Apply**: a progress notification with Cancel. Result: N objects named by cell, the dowels as loose objects, everything selected, then arranged over as many plates as needed. One undo step restores the uncut prop.
8. The Assemble view shows the parts back together; its existing explode control spreads them.

### Panel mock

```
+- Split to Fit ---------------------------------------------+
| Printer volume   256 x 256 x 256 mm   [Current printer v]  |
| Margins          X/Y [ 5.0] mm each side   Top Z [ 5.0] mm |
| [ ] Keep to the area both nozzles reach      (H2D / H2C)   |
| Usable           246 x 246 x 251 mm                        |
|-------------------------------------------------------------|
| Cut along        [x] X  (3 parts)   [ ] Y  (1)   [x] Z (2) |
| Layout           (o) Even parts   ( ) Full size first      |
| Grid angle       [ 0 ] deg about Z        [Find best]  (P4) |
|-------------------------------------------------------------|
| [x] Adjust planes automatically    up to [10] mm      (P3) |
| Smallest part thickness            [10] mm                 |
|-------------------------------------------------------------|
| Dowels           [x] Add dowels                        (P2) |
|   Type           [Dowel v]   (Dowel / Pin)                 |
|   Shape          [Round v]   (Round / Square / Hex / Tri)  |
|   Diameter       [ 6.0] mm   [x] Scale down on small faces |
|   Length         [20.0] mm   (10 mm into each part)        |
|   Up to          [ 3 ] per cut face                        |
|   Wall margin    [ 3.0] mm   Min spacing [20.0] mm         |
|   Fit tolerance  [0.10] mm                                 |
|-------------------------------------------------------------|
| Result           6 parts, 14 dowels, about 3 plates        |
|   X1-Z1  200 x 200 x 150   ok                              |
|   X2-Z1  200 x 200 x 150   ok                              |
|   X3-Z2  200 x 180 x 150   ok          ... (scroll)        |
| Warnings (2)                                               |
|   ! Plane X2: one face is too thin for a dowel             |
|   ! Part X3-Z2 has 2 separate pieces                       |
|                          [Reset]   [Cancel]   [ Apply ]    |
+-------------------------------------------------------------+
```

Defaults are the values shown. "Cut along" axes are ticked automatically where the model is larger than the usable size. Every numeric field has a tooltip; nothing is hidden behind an Advanced switch except "Smallest part thickness", "Wall margin" and "Min spacing".

## 3. Planning algorithm

All of this is pure libslic3r, in mm, in a "grid frame": the object's world orientation turned by the grid angle about Z. Input: the merged model-part mesh in that frame, the usable box, options. Output: planes, parts, connector sites, warnings.

### 3.1 Usable volume

```
bed_rect  = largest axis-aligned rectangle of printable_area
            (Rectangle bed: the bed; Circle bed: inscribed square; Custom: bounding box + warning)
bed_rect -= bed_exclude_area      (for each exclusion box, shrink the one side that loses least area)
if "both nozzles" ticked: bed_rect = bed_rect intersected with bounding box of ExtruderAreas::shared
usable.x  = bed_rect.x - 2 * margin_xy
usable.y  = bed_rect.y - 2 * margin_xy
usable.z  = min(printable_height, lowest extruder_printable_height) - margin_z
```

The XY margin is the brim / skirt allowance; the default 5 mm covers a 5 mm brim or a skirt at 2 mm. The panel says so in the tooltip. The margin is also what makes the later arrange succeed: the parts must be smaller than the bed by more than the arranger's own shrink.

### 3.2 Grid

For each ticked axis `a` with model extent `[lo, hi]`, size `S = hi - lo`, usable `U`:

- `n = ceil((S - eps) / U)` parts, `n - 1` planes.
- **Even parts** (default): planes at `lo + k * S / n`. Every part has slack `U - S/n`, which is what adjustment uses.
- **Full size first**: planes at `lo + k * U`; the last part is the remainder. If the remainder is thinner than "smallest part thickness", the last plane moves in until it is not.
- An unticked axis whose size exceeds `U` gives the warning "parts will not fit in Z" and red parts.

Part count is the product of `n` over the axes, less empty cells (found in 3.4).

### 3.3 Grid angle (P4)

Try angles 0, 5, ... 85 degrees about Z. For each, take the bounding box of the convex hull's vertices in the turned frame (cheap), compute the part count as above, keep the smallest count; ties go to the smaller angle, and 0 wins any tie. "Find best" sets the field; it is never applied silently. On Apply the parts are turned back by minus the angle so they sit square on the bed.

### 3.4 Parts and their checks (no cutting yet)

For the preview the mesh is not cut. Per cell:

- Size = bounding box of the mesh vertices clipped to the cell (vertices inside the cell plus the cross-section outlines on its faces). Compared with the usable box: **fits / does not fit**.
- Empty cell: no vertices and no cross-section on any face. Not counted.
- Exact shell count and volume come from the real cut at Apply (3.7 warnings marked "after Apply").

### 3.5 Plane adjustment (P3)

Per axis, independently. For each plane `i` with nominal position `p_i`, candidates are `p_i + k * 1 mm` for `|k| <= limit` (default limit 10 mm, so 21 candidates). All candidates of all planes on one axis are sliced in **one** `slice_mesh_ex()` call.

For each candidate cross-section (a set of islands):

| Term | Definition | Weight |
|---|---|---|
| `small` | number of islands with area below `pi * (dowel radius + wall margin)^2` | 1.0 each |
| `nodowel` | 1 - (area of islands that can take at least one dowel) / (total area); 0 when dowels are off | 2.0 |
| `busy` | `abs(area(p + 1) - area(p - 1)) / max(area(p), 1 mm2)`, capped at 1: the section is changing fast, so the cut runs through detail or along a sloped / flat face | 1.0 |
| `ragged` | outline length / (2 * sqrt(pi * area)), minus 1, capped at 1: a frilly outline is harder to glue and align | 0.5 |
| `away` | `abs(k) / limit`: stay near the nominal position | 0.2 |

`cost = 1.0*small + 2.0*nodowel + 1.0*busy + 0.5*ragged + 0.2*away`. Constants live in one struct so tests can pin them and the owner can ask for a retune.

Hard rules (never traded against cost):

1. Every part on this axis, measured between its two planes (or plane and model end), is no longer than `U`. This is the owner's "would exceed build volume" rule: such a move is **refused**, never made.
2. Every part is at least "smallest part thickness" long (no slivers).
3. A plane is not within 1 mm of a position where the section area jumps by more than 30 % (a cut that just shaves a flat face leaves a paper-thin skin on one side).

Solve: dynamic programming over the planes of the axis in order, state = candidate index, transition allowed when rules 1 and 2 hold for the part between two neighbouring planes. At most about 10 planes x 21 candidates, exact and instant. If no assignment satisfies the rules, all planes of that axis stay nominal and a warning says why.

Warnings from this step:

- "Plane X2 was moved 6 mm to avoid a thin region." (information)
- "Plane X2 could not be moved to a better place: the next part would exceed the usable size." (the plane's best-cost candidate was excluded by rule 1)
- "Plane Z1 cuts through fine detail and could not be improved within 10 mm."

Manual drag: the user may drag any plane. A dragged plane is pinned (adjustment skips it). If the drag makes a part too long, the part turns red and Apply asks for confirmation; it is not blocked, because the user may be about to rotate that part by hand.

Known simplification: the cost looks at the whole section of the model at that position, not at each cell's face separately. Per-cell dowel viability is checked afterwards (3.6) and reported.

### 3.6 Automatic dowels (P2)

Sites are found per **cut face**: one plane's section clipped to one cell of the other axes, then split into islands.

```
for each face island I (ExPolygon):
    r   = dowel radius (footprint radius incl. tolerance, cut_connector_footprint_radius())
    A   = offset(I, -(wall_margin + r))            # where a dowel centre may be
    A  -= bands of width (r + corner_keepout) along every OTHER plane crossing this face
    if A is empty and "scale down" is on: retry with r * 0.75, then r * 0.5 (not below 2 mm diameter)
    if A is still empty: warn "face too small for a dowel", continue
    C   = points of a square grid (step = max(1 mm, r / 2)) inside A
    clr(c) = distance from c to the outline of I
    pick 1 = argmax clr
    pick k = argmax over C of min(clr(c), 0.5 * distance to nearest picked), subject to distance >= min spacing
    stop at "up to X", or when no candidate is left
    for each pick: depth check (below); drop the pick and take the next candidate if it fails
```

- **Depth check:** cast rays along both plane normals from the centre and from 8 points on the footprint rim against the mesh (the AABB tree classes the cut code already uses). Material depth on a side = shortest hit. Dowel half-length on that side = min(requested, depth - 1.5 mm). If that is below 0.75 x diameter, the site is dropped. A shortened dowel is listed as information.
- **Anti-rotation:** two or more dowels on a face lock rotation. If a face gets exactly one **round** dowel, warn "part can turn on its dowel"; with "Scale down" on the tool first tries two smaller ones. A single Square / Hex dowel is keyed and gives no warning.
- **Corner keep-out** (default 2 mm beyond the footprint): no dowel body may reach another cut plane. This is what makes repeated cuts safe (4.3) and is the answer to "connectors colliding near part corners".
- **Type:** Dowel (default) or Pin. Both are symmetric and loose, so no side "owns" a protruding plug, parts can be printed in any pose, and neither half's bounding box grows. Plug and Snap are not offered in v1 (a plug sticks out of one part: it can push a part over the usable size and usually needs support).
- **Printed how:** loose dowel / pin objects, as the cut makes them today (`CutUtils.cpp:768-785`, pins in a row `:949-981`). The panel shows the count. Identical dowels are collapsed to one object with N instances in a clean-up step (P2, see risk R8).
- **Editing afterwards:** before Apply, a click on a site disc removes it and a click on a face adds one (P2 stretch; without it, the user changes the numbers). After Apply the holes are ordinary negative volumes in each part: they can be deleted or moved in the object list like any negative volume. Re-opening them in the Cut gizmo's connector editor is **not** available in v1, because that editor works from a cut recipe and the grid writes none (4.4).

### 3.7 Warnings (full list)

| Warning | When known | Severity |
|---|---|---|
| Part does not fit the usable volume (axis unticked, or manual drag) | preview | red, Apply asks |
| Adjustment refused: neighbouring part would exceed the usable size | preview | amber |
| Plane cuts through fine detail, no better place within the limit | preview | amber |
| Face too small / too thin for a dowel (count of faces) | preview | amber |
| Only one round dowel on a face: part can turn | preview | amber |
| Dowel shortened to fit the material | preview | info |
| Model looks hollow: walls about N mm, dowels need solid material | preview (section area much smaller than its outline's area, or all faces fail the inset) | amber |
| Bed is not rectangular / has exclusion areas: usable area was reduced | preview | info |
| Part has N separate pieces | after Apply | amber |
| Part is very small (below 1 cm3) | after Apply | amber |
| Part has no flat face on the bed (contact area below 1 cm2 at its lowest 0.3 mm) | after Apply | amber |
| Cut left open edges (model was not watertight) | after Apply | amber, offers the existing repair |
| More parts than fit 36 plates / parts left unarranged | after arrange | amber |

## 4. Data model and code structure

### 4.1 New files

| File | Content |
|---|---|
| `src/libslic3r/SplitToVolume.hpp/.cpp` | `SplitOptions`, `SplitPlane {axis, pos, nominal, pinned}`, `SplitPart {cell, bbox, fits}`, `SplitSite {plane, pos, radius, half_len_a, half_len_b}`, `SplitWarning {kind, plane, part, value}`, `SplitPlan`. Functions: `split_usable_volume()`, `split_plan_grid()`, `split_best_angle()`, `split_adjust_planes()`, `split_place_dowels()`, `split_apply()`. No GUI, no globals; every function takes a cancel callback. |
| `src/libslic3r/CutUtils.hpp/.cpp` (small edit) | Move the Plug / Dowel / Snap branch of `apply_cut_connectors()` and `get_connector_mesh()` out of the gizmo into libslic3r next to `add_pushfit_volume()`, so `split_apply()` and tests can bake connectors headless. The gizmo then calls the moved function. No behaviour change. |
| `src/slic3r/GUI/Gizmos/GLGizmoSplitToFit.hpp/.cpp` | Panel, plane and site rendering, plane dragging. Registered in `GLGizmosManager` (`GLGizmosManager.hpp:81` enum region, `GLGizmosManager.cpp:228`). Kept out of `GLGizmoCut.cpp`, which is already 11,794 lines. |
| `src/slic3r/GUI/Jobs/SplitToFitJob.hpp/.cpp` | `process()`: `split_apply()` off the UI thread with progress and cancel. `finalize()`: one snapshot, replace the object, name, select, start the arrange. |
| `tests/libslic3r/test_split_to_volume.cpp` | Tag `[SplitToVolume]`. |

### 4.2 Apply: N cuts, efficiently

`split_apply()` works on a scratch `Model`, not on the user's.

1. Copy the object in. Bake the grid angle into a working instance rotation.
2. **Bisection, not peeling.** Recursively: for the current piece, pick the axis with planes still crossing it (order X, Y, Z), take the **middle** plane of that axis, run `Cut(piece, 0, plane_matrix, KeepUpper | KeepLower | KeepPaint | CreateDowels).perform_with_plane()`, copy both results into the scratch model, recurse into each. Each triangle is then visited about `log2(parts)` times instead of once per plane, and the big mesh is copied only at the top levels.
3. Connectors for a plane are baked into the piece just before that plane's cut: only the sites whose face lies inside the piece.
4. After each cut: shift nothing. `perform_with_plane()` already bakes rotation and scale into the meshes and keeps the offset (`CutUtils.cpp:872-910`), so later plane matrices are "grid frame position minus instance offset", the same formula the gizmo uses (`GLGizmoCut.cpp:10030-10034`).
5. Leaves: name by cell, set one shared `cut_id` (when dowels exist), record shell count, volume and bed contact for the after-Apply warnings.

Parallelism: v1 runs the recursion **serially** inside the job. The two branches are independent meshes, but every clone takes new object ids from a process-wide counter and the thread-safety of that was **not verified**; running branches under TBB is a follow-up once it is. `cut_mesh()` itself is single-threaded (1.1).

Memory: a `Cut` holds one copy of its piece, and each half starts with a full vertex copy before compaction (1.1). Peak is roughly three times the piece being cut, so about 3x the original at the first cut and falling fast. A 5-million-triangle model is in the order of 0.2 GB per copy (estimate, not measured).

Paint: `KeepPaint` remaps at every level. For an unpainted model that is free (`save_paintings` returns nothing to restore, `CutUtils.cpp:1027-1030`). For a painted one it costs once per level; saving once and restoring once on the leaves is a later optimisation that needs a small `Cut` API addition.

### 4.3 Why repeated cuts are safe with connectors

After plane A's cut, its dowel holes are processed negative volumes in both pieces. When plane B later cuts a piece, they go through `process_modifier_cut()` (`CutUtils.cpp:822-843`): copied whole to the side their bounding box is on, or to both sides if B crosses the box. The corner keep-out (3.6) guarantees B never crosses a dowel, so each hole lands in exactly one piece. This path exists but has **not been exercised by a grid before**; P2 has a test for it (7).

### 4.4 Save / load, undo, re-edit

- **Undo / redo:** one `Plater::TakeSnapshot` in the job's `finalize()`, like the cut's (`GLGizmoCut.cpp:10892`). One Ctrl+Z brings the uncut object back.
- **Project save / load:** nothing new. Parts are ordinary objects; holes are ordinary negative volumes with `cut_info`; the group is in `cut_information.xml`; assembled poses are instance data already serialised (`Model.hpp:1728`).
- **Settings:** the panel's values are remembered in app config (per user, not per project).
- **No cut recipes in v1.** A recipe is one cut plus the whole pre-cut mesh (1.4). A chain of them would store an intermediate mesh per level and still could not re-open the grid as a grid.
- **Follow-up "Edit split" (not in the estimate):** one new recipe kind `Grid` on every part: original mesh once (content-addressed, as today), plane list, dowel options and sites. Recipe version 10; older builds refuse it and still load the parts, which is the documented rule (`CutRecipe.hpp:58-61`). Size M, about 4 days.

### 4.5 3MF compatibility with upstream Orca / Bambu Studio

| What | Upstream sees |
|---|---|
| Parts | Plain objects with their meshes. Always loads. |
| Dowel holes | Negative volumes; `cut_information.xml` uses upstream's own schema and Dowel is upstream type 1 (`Model.hpp:260-263`). Expected to load as a linked cut group with connectors. |
| Dowel objects | Plain objects. |
| Pin holes / pins | Geometry loads (they are meshes). The connector **type** 5 is EdgeSlicer-only (`Model.hpp:272-277`); what an upstream reader does with an unknown type is **unverified** (our own older reader clamps to Plug, `CutRecipe.hpp:134-136`). Default type is therefore Dowel. |
| Assembled poses | Stored the way upstream stores them (**unverified** that every upstream version reads them back). |
| Plate layout | Normal plate data. |

No new 3MF parts, no new config keys, no G-code change.

## 5. Out of scope for v1

| Left out | Why | Later? |
|---|---|---|
| Curved / free-form / per-part cut surfaces | The owner asked for a modest tool; it would also need per-face connector frames on curved surfaces | No plan. A single part can still be re-cut by hand with the curved or drawn cut. |
| Automatic hollowing, wall thickening, adding bosses behind dowels in shell models | A separate feature family | No plan; warn instead. |
| Per-part automatic orientation for strength or supports | Can break "fits the volume" unless the box is re-checked; needs its own rules | Medium follow-up: offer "lay on largest cut face" per part with a re-check. `OrientJob` exists (`src/slic3r/GUI/Jobs/OrientJob.hpp:15`). |
| Dovetails / puzzle joints / tongue and groove on grid faces | The groove cut is a different entry point (`CutUtils.hpp:337`) with its own preview; multiplying it over a grid is not cheap | Not cheap. |
| Plug and Snap connectors | A protruding plug changes part size and print pose | Cheap to offer later once per-part size re-check exists. |
| Glue-face texturing | Not asked for | No plan. |
| **Part numbers embossed on the cut faces** | Needs text volumes placed on each face | **Now phase P5** (owner, 2026-11-15): matching labels on both faces of every mating pair; see 7.5. |
| Assembly guide / numbered instructions | Not asked for | The Assemble view with explode already gives the picture. |
| "Edit split" (re-open the grid) | See 4.4 | Medium follow-up. |
| Non-rectangular usable area (L-shaped beds, true circle) | Keeps the planner a box problem | Only if asked. |
| Splitting several objects at once | Selection rules, naming | Cheap follow-up. |

Inspiration, for the record: LuBan is closed-source commercial software (proprietary licence). Nothing from it was downloaded, run or copied; only second-hand descriptions were read: a Fabbaloo article (https://www.fabbaloo.com/2019/05/luban-3d-printing-very-large-objects) and a Prusa forum thread (https://forum.prusa3d.com/forum/prusaslicer/large-model-splitting). They describe multi-plane splitting with dowel or mortise-and-tenon connectors and numbered parts with an assembly sequence. The sources disagree on whether it plans cuts from the build volume by itself. Which tool the owner means by "modelsplitter" was **not identified**. All text, defaults and algorithms in this document are our own.

## 6. Risks and unknowns

| # | Risk | What we do |
|---|---|---|
| R1 | **Repeated cuts with connectors** have never run as a grid: hole hand-over by bounding box (4.3), cut_id check sums across many cuts, duplicate connector names (`check_objects_after_cut`, `GLGizmoCut.cpp:10058-10080`, names must be unique per object). | Corner keep-out; unique names "Dowel-<plane>-<n>"; a P2 test that cuts a 2 x 2 x 2 grid with dowels and checks every hole is in exactly one part and every part is watertight. |
| R2 | **Mesh robustness.** `cut_mesh()` only promises not to add open edges (`TriangleMeshSlicer.cpp:2470-2471`); a non-manifold input gives non-manifold parts, and errors compound over several cuts. | Check open edges on the input and warn before Apply; check each leaf and offer the existing repair; never hide it. |
| R3 | **Performance on huge meshes.** Not measured. Preview slices are parallel and cheap; Apply is serial per cut. | Background job with cancel; bisection; P1 hand test times a 2-million-triangle model and reports. If too slow: TBB over branches after the id question is settled. |
| R4 | **Hollow / shell models.** A cut through a 2 mm shell has no room for a 6 mm dowel. | Detect and warn; "scale down" helps a little; the honest answer in v1 is "glue the faces, or use a solid model". Bosses are out of scope. |
| R5 | **Fit depends on the printer.** One tolerance does not suit every machine. | Tolerance is a visible field (default 0.10 mm radial for Pin as today, `PushFit.hpp:65-66`; dowel tolerance as the Cut gizmo's default, `GLGizmoCut.hpp:884`); the tooltip suggests a single test cut first. |
| R6 | **Corners where two or three planes meet.** | Keep-out bands (3.6). Faces that become too small because of the bands are reported as "too small for a dowel". |
| R7 | **Painted / multi-material models.** Colour, support, seam and fuzzy paint and the airbrush field survive each cut (1.1). Texture-displacement masks are dropped by a cut today (`tests/plan_od11_cut_features.md`, section "Edge features at risk", item 6). Modifiers are kept by bounding box and can end up duplicated in two neighbours when they straddle a plane (`CutUtils.cpp:838-842`), which is correct but may surprise. Cost grows with levels for painted models. | State it in the panel when the object has texture displacement; test colour paint through a 2 x 2 grid. |
| R8 | **Many dowel objects.** One object per dowel can mean 50+ list rows. | Collapse identical dowels into instances after Apply. Interaction with the cut_id check sum is **unverified**; if it conflicts, dowel objects are simply left out of the group. |
| R9 | **Linked-cut restrictions** (no scaling, delete warning) on a 20-part group may annoy. | Owner decision D10. |
| R10 | **Arrange cannot place a part** although the planner said it fits (arranger shrink, brim, wipe tower, exclusion areas). | Margin default 5 mm; report unarranged parts; the "both nozzles" option for H2D / H2C. |
| R11 | **Part falls over / needs supports** after the cut. | Warning "no flat face on the bed"; no automatic re-orientation in v1. |
| R12 | **Multi-instance objects.** A cut works on one instance (`CutUtils.hpp:264`). | v1 requires a single instance, as the Cut gizmo requires a single full instance (`GLGizmoCut.cpp:6306-6308`). |
| R13 | **Sliced beds with odd shapes** (delta, custom polygons). | Inscribed square / bounding box with an information line. |

## 7. Phases

Sizes: S up to 2 agent-days, M 3 to 5, L more. Every phase is shippable and has no G-code impact (model edit only; the standard gate set has no cut objects, so the `gcode-gate` run must be identical).

| Phase | What | Size | Status |
|---|---|---|---|
| P1 | Planner, preview, plain cuts, arrange | M | Built on `feat/split-to-fit-p1` (see "P1 as built" below) |
| P2 | Automatic dowels | M | Next |
| P3 | Plane adjustment, 3D plane handles, more after-Apply checks | M | |
| P4 | Polish (grid angle, other printer / custom size, dowel instances) | S to M | |
| P5 | Cut-face labels: matching embossed / debossed ids on mating faces (owner, 2026-11-15) | S to M, about 3 days | Designed (section 7.5); after P2 |

### P1: planner, preview, plain cuts, arrange. M, 5 days

- Files: `SplitToVolume.hpp/.cpp` (usable volume, grid, part boxes, apply without connectors), `GLGizmoSplitToFit.*`, `SplitToFitJob.*`, `GLGizmosManager.*`, `src/libslic3r/CMakeLists.txt`, `src/slic3r/CMakeLists.txt`, `tests/libslic3r/test_split_to_volume.cpp` and its CMake entry, translations template.
- Panel: volume, margins, axes, layout, result list, the fit warnings. Planes shown; dragging a plane.
- Unit tests `[SplitToVolume]`:
  1. Usable volume from a 256 bed, margins, an exclusion corner, a circular bed, an H2D-style shared area.
  2. Grid: a 600 x 200 x 300 box in 246 x 246 x 251 gives 2 X planes, 0 Y, 1 Z, six parts; even and full-size layouts; remainder thinner than the minimum is pulled in.
  3. Apply on a box: six parts, each watertight (`its_num_open_edges == 0`), volumes add up to the original within 0.1 %, every part's box within the usable box.
  4. Apply on a sphere and on a model with an empty cell (an L shape): empty cells produce no object.
  5. A rotated and scaled instance gives the same parts as the baked mesh.
  6. Assembled poses: every part's assembled pose puts its mesh back inside the original's bounding box.
  7. Colour paint on the top face survives on exactly the top parts.
  8. Cancel callback stops the apply and leaves the input untouched.
- Hand test (owner): load a model about 2.3 times the bed in X; open Split to Fit; check the plane preview and the part list; untick Z and see the red parts; Apply; parts are named, selected, arranged over plates and all white (inside the volume); Assemble view shows the whole model; Ctrl+Z restores it; save, reopen, parts and plates intact. Time a 2-million-triangle model and note it.

### P2: automatic dowels. M, 4 days

- Files: `SplitToVolume.*` (sites, depth check, keep-out), `CutUtils.*` (move the connector baking out of the gizmo), `GLGizmoCut.cpp/.hpp` (call the moved function), `GLGizmoSplitToFit.*` (dowel section, site discs), tests.
- Unit tests:
  1. A 60 x 40 face, 6 mm dowels, up to 3: three sites, all at least wall margin + radius from the edge and at least the min spacing apart.
  2. A 10 x 10 face: one smaller dowel with "scale down", none without; the right warning each time.
  3. A 2 mm-wall tube: no sites, "hollow" warning.
  4. Depth: a 12 mm-thick slab side shortens a 10 mm half-length to fit; a 3 mm side drops the site.
  5. Keep-out: on a 2 x 2 x 2 grid no dowel body is within 2 mm of another plane.
  6. Apply with dowels on a 2 x 2 x 2 cube: every hole is in exactly one part, each part has holes on each of its inner faces, dowel objects count = sites, one shared `cut_id`, no duplicate connector names.
  7. The moved connector baking gives byte-identical volumes to today's gizmo path (golden from `test_cut_connectors.cpp`'s hand-built dowel).
  8. Pin type: pins created, holes in both parts.
  9. 3MF round trip of the split result: groups, holes and poses intact.
- Hand test: split the P1 model with dowels; check discs on every face; Apply; slice one pair of parts and see the holes in the preview; print one joint (two small parts and a dowel) and check the fit; try Pin; try Square with "up to 1".

### P3: plane adjustment and warnings. M, 4 days

- Files: `SplitToVolume.*` (section costs, DP, refusal rules, after-Apply checks), `GLGizmoSplitToFit.*` (adjust controls, amber planes, warning list with zoom-to).
- Unit tests:
  1. A bar with a thin neck exactly at the nominal plane: the plane moves off the neck, within the limit.
  2. Full-size layout, where any move outward would exceed the usable size: the move is refused and the "would exceed" warning is raised; the plane may still move inward.
  3. A plane 0.3 mm from a flat end face is pushed away (area-jump rule).
  4. Minimum part thickness is never violated.
  5. Costs: hand-worked values for a rectangle, a rectangle plus a small island, a ring.
  6. A pinned plane does not move.
  7. After-Apply checks: two-piece part, tiny part, no-flat-face part, open-edge input each raise their warning.
  8. With adjustment off, the plan equals P1's exactly.
- Hand test: a figure with thin limbs near the nominal planes: turn adjustment on and see planes shift and say why; drag a plane too far and see the red part and the Apply confirmation; click a warning and see the view jump.

### P4: polish. S to M, 3 days

- Grid angle with "Find best"; collapse identical dowels to instances; "Split it to fit" link on the outside-the-volume notification; printer-volume source list (D8); remember settings; optional click-to-remove a site.
- Unit tests: best angle for a 45-degree-turned long box is 45 with the smaller count; ties prefer 0; parts are turned back square on Apply; dowel collapsing keeps the count.
- Hand test: a long model lying diagonally; the notification link; a second printer chosen as the volume source.

### 7.5 Cut-face labels (later phase, P5)

Owner requirement (2026-11-15): every cut face gets an embossed or debossed identifier - a pattern, a number, or a letter-number combination - and the two faces that mate carry the same one, so a person can tell which parts go together.

**What P1 already provides.** The planner's result has one `SplitFace` per mating pair (`src/libslic3r/SplitToVolume.hpp`): the plane, the two cells and the two parts either side, the material area, a stable `key` (`X1:Y2-Z1`: plane name, then the cell along the other cut axes), a short `mate_code` both faces share (`X1.2`: plane name and the face's 1-based number on that plane) and a readable `mate_label` (`X1-Y2 | X2-Y2`). The same planes over the same model give the same ids, keys and codes, and every `SplitPart` lists the ids of its faces. P5 needs nothing more from the planner.

**What gets written.** Default: the `mate_code` (`X1.2`). Options: a running number (face id + 1), or a dot / bar pattern for people who prefer no text (the face number in binary as dots on a fixed grid, so small faces still get a code). Both faces of a pair carry the same code.

**How it is made.** Reuse the Emboss text tool: `src/libslic3r/Emboss.hpp` turns text into outlines (`text2shapes`) and outlines into a mesh (`polygons2model`) without the GUI, and a text volume keeps its text configuration, so the user can later open a label in the Text gizmo and edit it. The pattern option goes through the same path as an SVG shape (the SVG tool's code). Each label is a **negative volume** on its part: it is resolved at slicing time, so no mesh boolean runs and the 3MF stays readable upstream (a plain negative volume).

**Deboss, not emboss, by default.** A raised label would stop the faces sitting flush for glue, so both faces are recessed: depth 0.6 mm (three 0.2 mm layers), text height fitted to the space (below), at least 4 mm; smaller faces fall back to the dot pattern, and a face too small for that gets no label and a line in the warning list. An "emboss" option for faces that are not glued flush is off by default.

**Placement.** On each part's own face (the `SplitFace` region clipped to that part's material), at the point of largest clearance from the outline - the same largest-inscribed-circle routine P2 writes for dowel sites (3.6) - after removing a keep-out disc around every dowel site (dowel footprint radius + 2 mm) and the corner bands along other planes (3.6). The text is scaled to fit what is left. Labels are placed after the P2 sites, so P5 comes after P2.

**Mirrored on the mate.** The two faces of a pair look in opposite directions, so each label is laid out in its own face's frame (seen from outside the part, text upright along +Z on X / Y faces and along +Y on Z faces). In world space the two are mirror images of each other, which is what makes both read correctly when each part is held face-on.

**Tests (P5).** Every face of a 2 x 2 x 2 split gets exactly one label on each of its two parts, with the same text; no label reaches into a dowel keep-out; every label lies inside its part's face; a face too small for text gets the pattern; a 3MF round trip keeps the labels as editable text volumes.

### P1 as built (2026-11-15)

- Planner, usable volume, apply: `src/libslic3r/SplitToVolume.hpp/.cpp`; tests `tests/libslic3r/test_split_to_volume.cpp` (`[SplitToVolume]`).
- Gizmo `src/slic3r/GUI/Gizmos/GLGizmoSplitToFit.*` (toolbar icon `toolbar_split_fit*.svg`), job `src/slic3r/GUI/Jobs/SplitToFitJob.*`, a "Split to fit..." button beside "Perform cut" in the Cut panel and "Split to fit the printer..." in the object menu (D1).
- Differences from the plan above: a plane is moved by dragging its position in the panel's "Plane positions" list, not by a handle in the 3D view (P3 adds handles); the nozzle-reach option (D14) is in, shown only on printers with nozzle-only strips; after Apply the parts are moved off the plates and the "place loose objects" arrange runs (it also places anything else already off the plates); the warnings after Apply are "separate pieces" (D15) and "open edges" (text only, no repair button); "very small part" and "no flat face on the bed" stay in P3. The planner refuses more than 1000 cells. Objects with several copies are refused with a message (R12).
- No G-code change: nothing in slicing or G-code was touched.

Total: about 16 agent-days (range 14 to 17). Follow-ups not included: embossed labels (2), "Edit split" (4), per-part lay-flat (3), TBB over branches (1 to 2).

Model choice per the standing rule: Opus for P1 to P3 (new geometry code with tests), Sonnet for P4.

## 8. Owner decisions

Recommendation first in each.

- **D1. Where the tool lives.** A new gizmo with its own toolbar icon, plus a button in the Cut panel and the right-click menu. Reason: it needs a 3D preview with draggable planes, and the Cut gizmo file is already too large to grow. Alternatives: a mode inside Cut; a plain dialog with no preview.
- **D2. Default margins.** 5 mm each side in X/Y, 5 mm at the top in Z. Reason: covers a normal brim or skirt and the arranger's own shrink.
- **D3. Default connector.** Loose round Dowel, 6 mm diameter, 20 mm long, up to 3 per face, sizes scale down on small faces. Reason: symmetric, prints in any pose, loads in upstream slicers. Alternative default: the push-fit Pin.
- **D4. Plane adjustment on by default?** Yes, limit 10 mm, once P3 ships. Reason: it only moves planes within the slack, never past the usable size, and says what it did.
- **D5. Layout default.** Even parts. Reason: leaves slack for adjustment and avoids a thin last piece. Alternative: full size first (fewest glue lines on some models).
- **D6. Hollow / shell models.** Warn and still split, with whatever dowels fit. Reason: the cut itself is fine; only the dowels suffer. Alternative: refuse dowels entirely on such models.
- **D7. Part names.** `<object> X2-Y1-Z1` (axes that are not cut are left out), dowels keep today's `-Dowel-` naming. Reason: tells the user where the part goes without a guide. Alternative: running numbers.
- **D8. Volume source.** Current printer only in P1; in P4 add "another printer preset" and "custom size". Reason: the parts get arranged on the current printer's plates anyway.
- **D9. Z cuts.** Ticked automatically when the model is taller than the usable height, like X and Y. Reason: a novice with a tall model should not get red parts by default. Alternative: Z off unless ticked (Z cuts put a seam across the visible sides).
- **D10. Linking the parts.** Link them as one cut group when dowels exist (today's behaviour for cuts with connectors), leave them unlinked when there are none. Reason: matches what users of Cut already know. Alternative: never link, to avoid the no-scaling and delete-warning rules on a large group.
- **D11. Part pose after the split.** Keep the model's orientation, parts dropped to the bed; warn when a part has no flat face. Reason: any rotation can break "fits the volume". Alternative: lay each part on its largest cut face (follow-up).
- **D12. Arrange after Apply.** Always arrange the new parts (and only them) over plates. Reason: six parts stacked in one place is not a usable result. Alternative: ask first.
- **D13. A part that does not fit.** Apply asks for confirmation but is allowed. Reason: the user may intend to rotate that part by hand. Alternative: block Apply.
- **D14. Nozzle-reach option on H2D / H2C.** Off by default, shown only on printers that have exclusive strips. Reason: most split parts are single-material and one nozzle reaches the whole bed.
- **D15. Loose pieces inside one cell.** Keep them in the part and warn. Reason: deleting or separating automatically can lose small but wanted detail. Alternative: separate them into their own objects.
- **D16. Plug / Snap connectors.** Not in v1. Reason: a plug sticks out of one part and changes its size and print pose.
- **D17. Follow-ups to queue now or later:** embossed part labels (recommended next, 2 days), "Edit split", lay-flat per part.

## 9. Name

1. **Split to Fit** (recommended): says what it does in three words; panel title "Split to Fit", menu "Split to fit the printer...".
2. **Cut to Build Volume**: exact, but long and jargon-heavy.
3. **Fit to Printer**: friendly, but could be read as scaling the model down.

## 10. Not verified in this spike

- Cut time and memory on multi-million-triangle meshes (no build, no run).
- What the Cut gizmo's `Auto` connector mode does.
- Thread safety of object id generation when two `Cut`s run at once.
- How `process_modifier_cut()` behaves over three or more nested cuts (expected fine; P2 test 6).
- Whether collapsing dowels into instances upsets the cut_id check sum.
- What upstream Orca / Bambu Studio do with connector type 5 (Pin) and with stored assembled poses.
- The exact option name for passing a transform to `slice_mesh_ex()`; the fallback is slicing a rotated copy.
- `tests/design_pushfit_connector.md`, named in code comments (`Model.hpp:273`), is not present in `tests/` on this machine; the Pin facts above come from the code.
- LuBan's current feature set (second-hand sources only).
