# Plan: layer-based support highlighting

Date: 2026-10-12. Base: origin/main 6b79e2225a (2.4.5.5). Branch: `research/support-highlight-layer`
(worktree `C:\Dev\wt_supporthl`). Status: research. Nothing merged, no PR.

Goal (owner): show, layer by layer, where supports will be generated and what they hold up (the overhang
areas that need support on each layer), in the 3D view before slicing and in the preview after slicing, so
the user can see the support decision and change it (paint, settings) before printing.

## TL;DR

* Today there are two unrelated views of support. **Before slicing**, the paint-on-supports gizmo and the
  3D view's "Show overhang" tint faces red by a single facet-normal angle on the GPU. **After slicing**,
  the preview shows the support extrusions (Support, Support interface, Support transition roles) layer by
  layer with the layer slider, but not what each layer of support is holding up. Neither view shows the
  generator's per-layer decision.
* The angle tint is wrong in common cases, because the generators decide per layer (layer i needs support
  where it sticks out past layer i-1 grown by an offset):
  * **Overlap mode (threshold angle 0, normal supports):** the effective angle depends on the layer height
    (24 deg at 0.1 mm, 41.6 deg at 0.2 mm, 53.1 deg at 0.3 mm for a 0.45 mm outer wall at 50 %). The gizmo
    starts at 0 deg and highlights nothing.
  * **Tree supports (every style, organic too) ignore the overlap** and use a fixed 30 deg when the angle
    is 0.
  * **Normal supports cap steep thresholds** with a sliver filter: at 0.2 mm layers and a 0.45 mm wall,
    nothing steeper than 65.8 deg is supported even with a threshold of 80.
  * The generators also skip or add areas a normal test cannot see: bridges, sharp tails, small-overhang
    removal, build plate only, critical regions only, parts resting on parts, thin features, convex corners.
* **Recommendation:**
  1. **Phase 1, option (b), S (1-2 days):** "Like auto supports" in the gizmo and the 3D view's "Show
     overhang": the generator's rule turned into the right angle for the current layer height and support
     type. Free at runtime, fixes the three rule mismatches.
  2. **Phase 2, option (c), M (4-6 days):** after slicing, the preview draws each layer's **supported
     overhang areas** (exactly what the generator decided to hold up, bridges and sharp tails included) as a
     translucent overlay that follows the layer slider, plus the overhangs it left **unsupported** in amber.
     Exact by construction, no G-code change, reuses the existing slider.
  3. **Phase 3, option (a), M-L (6-9 days):** before slicing, a background quick slice runs the generator's
     detection and paints the result onto the mesh in the 3D view and the gizmo, with a layer stepper on the
     section view. Optional, after phase 2 shows how much users want it pre-slice.
  4. **Phase 4, after `port/orca-batch-3c` merges, M (3-5 days):** one shared detector called by both
     generators and the highlight, so (a) cannot drift.
* A working prototype of (b) and of the (a) core exists (not committed here, see "Prototype"): 8 Catch2
  tests, 88 assertions pass; slice + detect + map of a 1M-facet sphere takes 0.42-0.65 s.

## 1. What exists today

### 1.1 Paint-on supports gizmo

Files: `src/slic3r/GUI/Gizmos/GLGizmoFdmSupports.cpp/.hpp`, `GLGizmoPainterBase.cpp/.hpp`,
`src/libslic3r/TriangleSelector.cpp`, `resources/shaders/{110,140}/mm_gouraud.{vs,fs}`.

* Tools: brush (circle / sphere), smart fill, gap fill, enforce / block / stabilizer points (our
  stabilizer state, `EnforcerBlockerType::STABILIZER`), erase. Painted state is a `TriangleSelector` per
  model volume, stored in the 3MF and applied by both generators as enforcer / blocker slabs per layer.
* Row "Highlight overhang areas": a slider 0-90 deg (`m_highlight_by_angle_threshold_deg`) and a drag box.
  Moving it calls `m_parent.set_slope_normal_angle(90 - angle)` and turns the canvas slope mode on.
* Start value: when the gizmo opens, `on_set_state()` sets `m_support_threshold_angle = -1`, and the next
  frame resets the slider to `get_selection_support_threshold_angle()`: the object's (else the print
  preset's) `support_threshold_angle` when supports are on and the type is auto, **else 0**. So it is 0
  (nothing highlighted) in overlap mode, with supports off and with a manual type. The tooltip shows "Auto
  support threshold angle: N" or "No auto support"; overlap, layer height and the tree fallback never
  appear.
* "On overhangs only" (`m_paint_on_overhangs_only`): brush, smart fill and bucket fill pass the angle into
  `TriangleSelector::select_patch()` / `bucket_fill_select_triangles()`, which skip facets with
  `world_normal_z >= -cos(angle)` (CPU, source facet normal).
* "Section view" (Alt + wheel): the object clipper cuts the mesh with a plane; this is the only "layer-like"
  control in the 3D view today.
* Dead code: `select_facets_by_angle()` (no button calls it) and the BBS "support preview"
  (`update_support_volumes()`, `run_thread()`, `PrintObject::generate_support_preview()`, a green
  `m_support_volume` drawn in `render_painter_gizmo()`), which sliced and generated the real support in a
  thread and drew it in the gizmo. Nothing calls `update_support_volumes()`, here or in BambuStudio.

### 1.2 "Highlight overhang" (gizmo and 3D view)

* `GLGizmoPainterBase::render_triangles()` binds `mm_gouraud` per part and sets `slope.actived`,
  `slope.volume_world_normal_matrix` and `slope.normal_z = -cos(threshold)`.
* `mm_gouraud.fs` (GLSL 110 and 140) computes the face normal per fragment from `dFdx/dFdy(model_pos)` and
  tints the fragment 50 % towards red when `world normal.z < slope.normal_z`. Fragments with
  `|world z| < 0.1` turn light blue (bed contact). Per pixel, every frame, nothing stored, no slicing, no
  layer height, no neighbourhood, no part interaction.
* The 3D view's "Show overhang" toggle uses the same shader logic through `GLVolumeCollection::render()` in
  `3DScene.cpp` (around line 1153) with the **global** `support_threshold_angle`
  (`get_selection_support_threshold_angle()`, line 962): no object override, and 0 in overlap mode.
  Upstream #12650 (in our unmerged `port/orca-batch-2e`, `67b11a88b5`) fixes the fallbacks there only.

### 1.3 Preview: support roles and the layer slider

* Extrusion roles (`ExtrusionEntity.hpp`): `erSupportMaterial` ("Support"), `erSupportMaterialInterface`
  ("Support interface"), `erSupportTransition` ("Support transition"). Ours also has
  `erBottomSurfaceOverSupport` ("Bottom surface over support", azure) and "Wall over support"
  (`d14e6ebf21`, the over-support surfaces feature, option `over_support_surfaces`). These come from the
  G-code `;TYPE:` tokens through `GCodeProcessor` into the libvgcode viewer (main has libvgcode since
  `48f3d69d40`).
* "Bottom surface over support" is the closest thing today to "what is supported", but it is a
  **prediction made before support generation** (`PrintObject.cpp` around line 1880, in prepare infill):
  bottom-bridge surfaces are retyped when auto supports are on, minus painted blockers, minus small
  bridgeable surfaces. It is only on when `over_support_surfaces` is set and the top Z distance is > 0, it
  shows only bottom solid fill (not the walls' share), and it does not know about the sliver filter,
  small-overhang removal, critical-only or build-plate-only.
* Layer slider: `IMSlider` (ImGui, `src/slic3r/GUI/IMSlider.cpp`) in the preview, with layer values,
  one-layer mode and "go to layer". `GCodeViewer::set_layers_z_range()` / `get_layers_z_range()` filter the
  toolpaths. `GCodeViewer` also draws non-toolpath overlays itself (`m_shells` via `load_shells()` /
  `render_shells()`, `m_conflict_result`), which is the hook an overlay would use.
* The 3D (Prepare) view has no layer slider. Upstream #15879 (`a1ad2b4425`, 2026-10-02) adds a
  canvas-wide section view (replaces the per-gizmo clippers, also clips the libvgcode preview). It is on
  the port list (`tests/orca_port_plan.md`, "Section view", L, High) and was blocked on libvgcode, which
  main now has.

### 1.4 Support generation: how overhangs are decided

Both generators work on `Layer::lslices` (union of region slices after XY compensation and the closing
radius) and per-region `LayerRegion::slices`. `posSupportMaterial` runs after perimeters and prepare infill,
because the bridge logic reads `layerm->perimeters`, `fill_surfaces` (stBottomBridge) and
`unsupported_bridge_edges`.

**Normal supports** (`Support/SupportMaterial.cpp`, static `detect_overhangs()` from
`top_contact_layers()`), per layer i >= 1, per region:

1. `thresh_angle = support_threshold_angle > 0 ? angle + 1 : 0`, clamped to 89 (+1: inclusive).
2. `fw = layerm->flow(frExternalPerimeter).scaled_width()` (`outer_wall_line_width`, else `line_width`,
   else 1.125 x nozzle of the wall filament).
3. `lower_layer_offset`: 0 for the first `enforce_support_layers` layers; angle mode
   `lower_layer.height / tan(thresh_angle)`; overlap mode `fw - support_threshold_overlap(fw)` (default
   50 %, so fw/2).
4. Offset 0: `diff(region slices, lower lslices)`. Otherwise, for normal(auto) only:
   `diff(region slices, expand(lower, offset))`, then grown back to the full overhang
   (`diff(intersection(expand(d, offset), region slices), lower)`). Normal(manual) gets only enforcers.
5. Build plate only: subtract the prefix union of every lower layer.
6. Sharp tails (always on): islands with nothing below and area < 36 mm2, extended upwards while small.
7. Blockers subtracted. 8. `bridge_no_support`: `remove_bridges_from_contacts()`.
9. **Sliver filter:** skip the layer if `offset(diff, -0.1 fw)` is empty.
10. `support_expansion`. 11. Cantilevers recorded; `support_remove_small_overhang` (default on) removes
    small clusters. 12. Enforcers in `detect_contacts()`.
* The result lives in the generator's transient `SupportGeneratorLayer::overhang_polygons` /
  `contact_polygons` / `enforcer_polygons` (`Support/SupportLayer.hpp` line 118) and is freed after
  generation.

**Tree supports** (`TreeSupport::detect_overhangs()`, used by every tree style; organic calls it from
`generate_support_areas()` in `TreeSupport3D.cpp`):

1. `lslices_extrudable = intersection(lslices, offset2(lslices, -w/2, +w))`, `w = line_width`.
2. `thresh_angle = angle > 0 ? angle + 1 : 30`, clamped to 89. **No overlap mode.**
3. Offset: `-0.15 w` for the first `enforce_support_layers`, else `lower->height / tan(thresh)`.
4. `overhang = diff_ex(extrudable[i], offset_ex(extrudable[i-1], offset))`. No grow-back, no sliver
   filter. 5. Sharp tails and cantilevers (skipped after 30 s or > 100 islands per layer).
6. Small-overhang clusters removed. 7. Blockers. 8. Critical regions only: keep cantilevers and sharp tails.
9. `max_bridge_length > 0` (default 10 mm): `remove_bridges_from_contacts(..., break_bridge = false)`.
10. Enforcers grown by 0.8 mm.
* Result: `Layer::loverhangs` (`TreeSupport.cpp` lines 1045-1107). **`PrintObject::detect_overhangs_for_lift()`
  then overwrites `loverhangs`** (lift overhangs, 30 % line width), so the support's overhangs are lost
  after a slice. `generate_contact_points()` samples them into tips.
* `PrintObject::is_support_necessary()` runs the tree detection with `stTreeAuto` even with supports off,
  for the "object needs support" warning.

| Key | Normal | Tree (all styles) |
|---|---|---|
| `support_threshold_angle` | +1, h/tan; 0 = overlap mode | +1, h/tan; 0 = 30 deg |
| `support_threshold_overlap` | offset = fw - overlap (angle 0 only) | ignored |
| layer height | in the offset | in the offset |
| line widths | outer wall width: offset, sliver filter, sharp tails | `line_width`: extrudable filter, tails, clusters |
| `support_on_build_plate_only` | subtract lower layers | branches dropped later |
| `support_critical_regions_only` | n/a (hidden) | cantilevers and sharp tails only |
| `support_remove_small_overhang` | cluster removal | cluster removal |
| bridges | `bridge_no_support` | `max_bridge_length` |
| first layers | `enforce_support_layers`: offset 0 | `enforce_support_layers`: offset -0.15 w |
| painted enforcers / blockers | yes | yes |

**Our ports and pending batches.** 3A/3B (#379, #383, #395) did not change detection; they made sure that
once an area is detected support reliably reaches it (tips along every contour edge, organic plate-only roof
recovery, exact interface counts, no dropped layers from #13310). `port/orca-batch-3c` (#11295, `ce4c297c67`)
removes `SupportCommon::remove_bridges_from_contacts()` and compiles out organic's own
`generate_overhangs()`, so trees use `max_bridge_length` everywhere; it touches `SupportMaterial.cpp`
and `PrintObject.cpp`. `port/orca-batch-2e` carries #12650 (Show overhang fallbacks) and #15694
(`printable_slices()` for wall overhang classification, not support detection). Phases 2 and 4 touch the
generators and must be rebased on 3C.

## 2. Prior art

* **OrcaSlicer upstream** (`orcaupstream/main` at `b920116fbd`, 2026-10-12): no layer-based support
  highlight. #12650 (`b0334325f8`, 2026-05-26, fixes #12634 "Overhangs aren't highlighted when calculating
  supports by overlap percentage") is option (b) for the 3D view's "Show overhang" only: `angle + 1`, else
  30 for trees, else `atan(layer_height / (outer wall width - overlap))`. No gizmo, no sliver cap, no
  object overrides. #12367 hides threshold overlap for tree supports. #12218 / #12239 persist the Show
  overhang toggle. #13340 restructured the painting UI. #15879 (`a1ad2b4425`) adds the canvas section view
  (section 1.3). On the belt-printer branch, gizmo work uses a shared build-plate-tilt "up" helper
  (`a8a45439db`): any highlight we write should take "up" from one helper, not a hard-coded +Z.
  #13310 (`0c7e16aa36`) fixed missing support layers in the preview.
* **BambuStudio** (`C:\Dev\BambuStudio`, 2026-10-08): the same angle highlight and the same dead green
  "support preview" (`generate_support_preview()`). No overlap option. Tree detection identical (30 deg).
* **PrusaSlicer** (master, `src/slic3r-shared/src/Slic3r/App/Plater/PaintOnSupportsGizmo.cpp`): the
  highlight is still angle-only, plus "Enforce overhangs" (paint every facet below the angle). It also has
  **Automatic painting** (since 2.6): slice up to `posSupportSpotsSearch` (`SupportSpotsGenerator`, a
  physics-style stability check, not the support generator's rule), then for each support point cast a ray
  up from the bed and paint an enforcer sphere at the first downward hit. The closest prior art to (a):
  background slice, per-layer result, mapped back to facets. Its preview shows support and support
  interface roles, no "what is supported" overlay.
* **Cura** (`resources/shaders/overhang.shader`, `plugins/SolidView/SolidView.py`): per-fragment
  face-normal test (`-normal.y > u_overhangAngle`), red, only above the build plate; the angle comes from
  the support extruder's `support_angle`, independent of whether support is enabled; geometry below
  `layer_height_0` is drawn inverted. No layer logic. Users are told to slice and check the Preview (layer
  view) and to place support blocker / custom support meshes. Same gap as ours.
* Summary: nobody shows the generator's per-layer decision before slicing, and nobody overlays "what this
  support layer is holding up" in the preview. Options (c) and (a) would be new.

## 3. Design options

### (b) Rule-based angle (pre-slice, free)

`SupportOverhangRule` (prototype) turns the generator's rule into the angle a planar slope needs at a given
layer height: angle mode `min(angle + 1, 89)`, capped for normal supports at `atan(h / (0.2 fw))`; overlap
mode `atan(h / max(fw - overlap, 0.2 fw))`; tree with angle 0: 30.

* **Data flow:** full config + object overrides (+ the wall filament's physical extruder for the width) ->
  rule -> one angle -> existing shader uniform and "on overhangs only".
* **Cost:** a config read on config change. No slice. No threading.
* **UI:** gizmo switch **Angle | Like auto supports**; in Like-auto mode the slider is read-only and the
  tooltip explains the source ("41.6 deg at 0.20 mm layers, normal, overlap 0.225 mm of a 0.45 mm outer
  wall"; "tree: 30 deg fallback, overlap does not apply"; "automatic supports are off: this shows what they
  would support"). Angle mode starts at the effective angle instead of 0. 3D view "Show overhang" uses the
  same rule.
* **Fidelity:** exact for planar slopes away from the first layers (tested). Misses bridges (every flat
  underside is red), sharp tails, small-overhang removal, build plate only, parts on parts, thin features,
  convex corners (a 50 deg frustum's corner edges slope at 40.1 deg and get support; no facet does),
  critical-only. Variable layer height in overlap mode: one angle only (tooltip shows the range).
* **Not layer by layer:** it is a better angle, not a per-layer view.
* **Risk:** low. Touches the gizmo, 3DScene and a new libslic3r helper; no G-code change.

### (c) Post-slice: the generator's own result in the preview, per layer

* **Data flow:**
  1. Both generators copy their final per-layer overhang areas (after blockers, enforcers, bridges, sharp
     tails, small-overhang removal, critical-only, build plate only) into a new
     `Layer::support_overhangs` (ExPolygons), before anything frees or overwrites them. Normal: the
     contact layers' `overhang_polygons` (+ `enforcer_polygons`) mapped back to object layers by
     `print_z`. Tree: `loverhangs` at the end of `TreeSupport::detect_overhangs()`, before
     `detect_overhangs_for_lift()` overwrites it. Cleared with the support step (`posSupportMaterial`
     invalidation).
  2. Unsupported overhangs for free: `diff(lift-detection overhangs or a 1-line-width diff of lslices,
     support_overhangs)` = what hangs in the air on that layer and gets no support (bridges, skipped small
     details, critical-only exclusions).
  3. `GCodeViewer` builds two overlays per object instance, like `load_shells()`: triangulated
     `support_overhangs` (translucent red) and unsupported overhangs (amber) at each layer's
     `print_z` (with the instance shift and the raft offset). Rendered after the toolpaths, filtered by
     `get_layers_z_range()`: in normal mode the top visible layer's areas are bright and lower ones dimmed
     or hidden; in one-layer mode only that layer.
  4. Optional: project the same per-layer areas back onto the mesh (the (a) mapping) to colour the model in
     the 3D view and gizmo while the slice is still valid ("as sliced" highlight).
* **Cost:** needs a slice (support step). Extra work: copying polygons (negligible) and triangulating them
  once per preview load (tens of ms; the shells already do similar work). Memory: about 1-2 MB per 100
  layers for typical models.
* **UI:** in the preview legend, under the Support roles, two toggles: **Supported overhangs** and
  **Unsupported overhangs** (both off by default the first time, remembered). The layer slider drives
  them. Hover tooltip on the legend explains "areas this layer's support holds up". From the preview, the
  existing "paint supports" path (click the support paint icon, upstream #9417 crash fix) opens the gizmo
  to fix what the user sees.
* **Fidelity:** exact, by construction: it is the generator's decision, bridges included. It is stale as
  soon as the object or support config changes (same as the toolpaths).
* **Threading:** written by the background process inside the support step; read by the GUI only after the
  step is done (as the preview already does for layers).
* **Risk:** medium-low. Touches the generators (copy only) and must not change the G-code: guard with the
  identity gate. Must rebase on 3C. Overlay rendering must work on the macOS core profile.

### (a) Pre-slice: quick layer detection projected onto the mesh

1. Layer heights from `PrintObject::slicing_parameters()` and `generate_object_layers()` (variable layer
   height and layer ranges).
2. Slice the model parts (world transform, minus negative volumes, `slice_closing_radius`) at those planes
   (`slice_mesh_ex`, TBB). Skip modifiers. Fast path: if the plate is sliced and valid, copy
   `layer->lslices` / `height` / `slice_z` from the gizmo's `PrintObject` on the UI thread while the
   background process is idle, and skip this step.
3. Run the generator's rule (`support_overhangs_from_slices()`), then the slice-only extras: build plate
   only, sharp tails, cantilevers, small-overhang clusters, critical-only, enforcer / blocker slabs.
   Bridges need perimeters, so either a slice-only proxy (component anchored on two opposite sides with a
   span below `max_bridge_length`, shown amber as "probably bridged") or the (c) cache when available.
4. Map back (`overhang_facets_from_layers()`): each downward facet is clipped to the slab between layers
   i-1 and i, projected to XY and flagged when at least 50 % of it lies in layer i's overhang (grown by the
   lower-layer offset for tree bands).

* **Display:** do not use painting states (the highlight must not be undoable paint).
  `TriangleSelectorPatch` already writes unshared vertices per triangle; add a per-vertex `v_highlight`
  float (split triangles inherit their source facet's flag) and in `mm_gouraud` (110 and 140) tint from it
  when `slope.mode == 1`. Sub-facet precision later via a `TriangleSelector::Cursor` that asks "is (x, y) in
  layer(z)'s overhang".
* **Layer by layer in 3D:** with the canvas section view (upstream #15879, or our gizmo clipper), a "Layer"
  stepper sets the plane to a layer's `print_z` and draws that layer's overhang polygons on the cut (red
  supported, amber skipped). This is the pre-slice equivalent of (c)'s one-layer mode.
* **Cost:** benchmark below: 50-170 ms up to 130k facets, 0.24-0.40 s at 500k, 0.42-0.65 s at 1M. Runs on a
  `Worker` job (as arrange / fill bed), debounced about 300 ms, cancellable, last result kept on screen.
* **Updates:** mesh, volume or instance transform (except XY moves and Z rotation), layer height profile or
  ranges, any key in the table in 1.4.
* **Fidelity:** matches the rule part exactly (within 1 % per layer on frustums, 15 % on a sphere, the gap
  being sharp tails at the foot); handles corners, parts on parts, thin features and variable layer height.
  Bridges approximate. Drifts when upstream changes detection, until phase 4.
* **Risk:** medium-high. New background job, shader change in both GLSL versions, a second copy of the
  detection logic.

### Comparison

| | (b) rule angle | (c) post-slice overlay | (a) pre-slice layers |
|---|---|---|---|
| Slice needed | no | yes (support step) | no (own quick slice) |
| Layer by layer | no | yes, preview slider | yes, section stepper |
| Matches generator | planar slopes only | exactly | rule exact, bridges approx. |
| Shows bridges / skipped areas | no | yes (amber) | approx. |
| Runtime cost | none | negligible after slice | 0.05-0.65 s background |
| Size | S | M | M-L |
| Main risk | none | must keep G-code identical | drift, shader, threading |

## 4. Recommendation and phases

Do (b) now because it is cheap and fixes visible bugs, then (c) because it answers the owner's question
("layer by layer, what is being supported") exactly and reuses the preview and slider, then decide on (a)
with real use. The previous draft put (a) second; (c) is now first because it cannot disagree with the
generator, needs no shader or job work, and its cache also gives (a) exact bridges later.

### Phase 1: (b) in the gizmo and the 3D view (S, 1-2 days)

* `libslic3r/Support/SupportOverhangHighlight.{hpp,cpp}`: `SupportOverhangRule` /
  `support_overhang_rule()` from the prototype, with a comment in both generators pointing to it. Map the
  wall filament to its physical extruder (`physical_extruder_for_filament`) for H2D/H2C widths; the
  prototype uses the filament index.
* Gizmo: Angle | Like auto supports switch (translated), effective start angle, tooltip, object overrides,
  read-only slider in Like-auto mode, update on config change events instead of the prototype's 0.5 s poll.
* 3D view "Show overhang": after 2E merges, make #12650's `get_selection_support_normal_z()` call the same
  rule, so both views agree.
* Tests: the prototype's `[SupportHighlight]` rule cases.

### Phase 2: (c) supported / unsupported overhangs in the preview (M, 4-6 days, after 3C)

* `Layer::support_overhangs` written by both generators; cleared on support invalidation.
* Unsupported overhangs per layer.
* `GCodeViewer` overlay (load per instance like shells, render filtered by layer range), legend toggles,
  persisted.
* Optional at the end: the "as sliced" mesh highlight in the gizmo from the cache (needs the (a) mapping
  from the prototype, without the quick slice).
* Gate: G-code identity against main on the gate2b case set plus the support cases (normal, tree, organic,
  build plate only, critical only), as PR #451 / #390 ran it.

### Phase 3: (a) pre-slice layer highlight (M-L, 6-9 days, optional)

* Worker job: quick slice (or the valid-slice fast path), rule, slice-only extras, facet mapping.
* Per-vertex highlight in `TriangleSelectorPatch` and `mm_gouraud` (110, 140), amber class.
* Mask-based "on overhangs only"; bring back `select_facets_by_angle()` as **Enforce highlighted** /
  **Block highlighted** (one snapshot each) using the mask.
* Layer stepper on the section view (port #15879 first if it has landed by then; otherwise the gizmo
  clipper).
* Fallback to (b) above a facet count, with a note.

### Phase 4: one detector (M, 3-5 days, after 3C)

* Extract both generators' overhang detection into a shared slice-only function (bridge removal as an
  optional step that needs perimeters). (a) calls the same code. Identity gate on the support models.

### Risks

* Generator changes (3C, future Orca ports) move the truth: (c) follows automatically; (a)/(b) need the
  comparison tests below to fail loudly, until phase 4.
* G-code identity in phase 2: the cache must be a copy only. Gate it.
* Big meshes in (a): debounce, cancel, keep the last result, fall back to (b).
* Variable layer height / layer ranges / per-volume width overrides: (a) and (c) handle the first two; (b)
  shows one angle; per-volume wall width is approximated with the object's in (a)/(b).
* Multi-nozzle (H2D/H2C): physical extruder mapping for widths.
* Threading: never read `PrintObject` layers from a worker while the background process runs.
* Shader / overlay rendering on the macOS core profile (phases 2 and 3): test on the Mac.
* Belt printers (upstream branch): "up" must come from a shared helper if that lands.

## 5. Test ideas

Automated (Catch2, `fff_print_tests`):

* `[SupportHighlight]` rule (in the prototype): inverted square frustums at 0.1 / 0.2 / 0.3 mm, normal(auto)
  overlap mode, tree(auto) angle 0, angle 40 and 80 on both: the rule's prediction equals "the generator
  made support" in every case; the old gizmo answer is in the failure message.
* Convex corners: a 50 deg frustum gets support at its corners; the rule says no facet; the layer core
  finds > 10 overhang layers (documents the blind spot of any per-facet angle).
* (a) core vs `TreeSupport::detect_overhangs()` on the object's own slices: per-layer area within 1 % on
  frustums, 15 % on a sphere.
* Facet mapping: frustum side facets flagged exactly when below the rule; sphere: (a) and (b) disagree on
  < 10 % of flagged facets.
* Phase 2 (new): after `print.process()`, `support_overhangs` is non-empty exactly on the layers below which
  support top contacts exist; every support overhang lies within one line width of the next support layer's
  extrusion footprint; a T-shape with a 20 mm bridge under `max_bridge_length` has the bridge in the
  unsupported set and its ends in the supported set; with `support_on_build_plate_only` an overhang above
  the object has no supported area; the cache is empty with supports off and cleared when the support step
  is invalidated.
* Phase 2 identity: G-code byte-identical to main (strict compare) on the gate case set.
* Benchmark (hidden, `[SupportHighlightBench]`): spheres 32k-1M facets, 0.2 and 0.1 mm.

Manual (owner, test copy), each with normal(auto), tree slim and organic; compare the highlight / overlay with
the support in the sliced preview:

| Model | Checks | (b) expected | (c) / (a) expected |
|---|---|---|---|
| Sphere r 20 | slope band, foot sharp tail | band moves with layer height in overlap mode | plus the foot |
| 3DBenchy | small details, bow, cabin roof bridge | details red, some unsupported | small details and roof bridge amber |
| T-shape, 20 mm bridge | `max_bridge_length` / `bridge_no_support` | whole underside red | bridge amber, ends red |
| 45 deg chamfer at 0.08-0.28 mm | overlap rule vs layer height | red from about 0.2 mm (41.6 deg) | same, per layer |
| Same, variable layer height | per-layer rule | one angle + range in tooltip | per layer |
| Two parts, one resting on the other | part interaction | contact faces red (wrong) | not highlighted |
| Downward cone tip | sharp tail | not highlighted (wrong) | highlighted |
| Threshold 80, normal | sliver cap | red only below 65.8 deg at 0.2 mm | same |
| Build plate only, overhang above the part | plate only | red (wrong) | amber |

Pass: no red without support under it and no supported area without red, except the documented classes for
(b).

## Prototype (not committed on this branch)

The previous agent built a prototype in this worktree. It is product code, so per the brief it is **not**
committed here. It stays uncommitted in `C:\Dev\wt_supporthl` and is saved as a patch at
`C:\Dev\SnapmakerOrca\tests\spike_support_highlight_prototype.patch` (untracked, 1044 lines).

* `src/libslic3r/Support/SupportOverhangHighlight.{hpp,cpp}` (new, 515 lines): `SupportOverhangRule` /
  `support_overhang_rule()` (b); `support_overhangs_from_slices()`, `overhang_facets_from_layers()`,
  `layer_overhang_facets()` (the (a) core: slice, detect, map); `angle_overhang_facets()` (b per facet).
* `GLGizmoFdmSupports.{cpp,hpp}`: a dev-only "Like auto supports (dev)" checkbox, shown only with
  `EDGESLICER_DEV_SUPPORT_HL=1`, untranslated, polls the config twice a second.
* `tests/fff_print/test_support_overhang_highlight.cpp` (309 lines) + CMake entries.
* Built with `agent_build.sh` (tag `supporthl2`, exit 0) and installed to the test copy
  `C:\Dev\EdgeSlicerTest\supporthl`.

Results (re-run 2026-10-12 on that build):

* `fff_print_tests "[SupportHighlight]" --order rand`: **8 test cases, 88 assertions, all pass** (rule:
  overlap vs layer height, tree 30 deg fallback, +1 inclusive, sliver cap; convex-corner blind spot; core vs
  `TreeSupport::detect_overhangs`; facet mapping; layer mapping vs angle on a sphere).
* `[SupportHighlightBench]` (sphere r 40 on the bed, slice + detect + map, Windows release build):

| Facets | Type | 0.2 mm total (slice / detect / map) | 0.1 mm total (slice / detect / map) |
|---|---|---|---|
| 32k | normal | 63 ms (36 / 13 / 14) | 87 ms (46 / 27 / 13) |
| 32k | tree | 51 ms (28 / 17 / 6) | 69 ms (37 / 22 / 10) |
| 129k | normal | 91 ms (56 / 23 / 12) | 159 ms (102 / 37 / 20) |
| 129k | tree | 108 ms (71 / 24 / 13) | 171 ms (96 / 54 / 21) |
| 517k | normal | 243 ms (172 / 47 / 24) | 391 ms (272 / 84 / 34) |
| 517k | tree | 270 ms (197 / 49 / 24) | 401 ms (252 / 94 / 55) |
| 998k | normal | 417 ms (308 / 70 / 38) | 541 ms (364 / 118 / 57) |
| 998k | tree | 477 ms (326 / 102 / 48) | 649 ms (404 / 175 / 68) |

  Slicing dominates; the facet mapping (with bbox culling) is under 70 ms even at 1M facets. The earlier
  targets (300 ms at 200k, 1.5 s at 1M) are met without the raster / centroid optimisations. The (c)
  fast path (reuse a valid slice) would remove the slice column.
* Not yet done: the owner's hand test of the dev toggle on the test copy (set `EDGESLICER_DEV_SUPPORT_HL=1`,
  open the support painting gizmo, tick "Like auto supports (dev)", compare with the sliced preview using the
  manual table above).
