# ImageMap Phase 5: finishing the port

Date: 2026-10-16
Base: origin/main @ c2d859aa3f
Earlier phases: `2026-09-07-imagemap-phase1-solver.md`, `-phase2-imagefill.md`, `-phase3-imagerow.md`,
`2026-09-12-imagemap-phase4-walls.md`.
Reference for the feature list: the closed ImageMap upstream-fork stack, PRs #10-#14
(`refs/pull/14/head`, OrcaSlicer-ImageMap v1.0.44 lineage), its `docs/imagemap-full-*.md`, and
`tests/research_orca_16551_chromastack.md`. Facts were read from those; no code was copied.

Phases 1-4 built a fork-native ImageMap: a colour solver, image-to-painting, image-weighted rows
on top solid infill, and the same rows on outer walls at G-code time. This audit asks a narrower
question than "what did ImageMap have": **what does a user still hit when they try to use it?**
The answer is mostly wiring. Phase 4's wall dither has no control in the GUI, and when it is
reached it costs a real tool change for every candidate filament on every layer. The image is
always stretched over the part's bounding box. A cut re-stretches it. Textured OBJ files come in
with no colour. Those are fixed here. One real feature is also added: ImageMap's own side-wall
method (one filament per layer, outer wall width modulated by the image), as an opt-in second
wall method.

---

## 1. Gap table

Legend: **OK** = works on main today; **GAP** = missing or broken, fixed in the phase named;
**OUT** = deliberately not done (section 3 says why).

| # | ImageMap capability (PRs #10-#14) | On main (c2d859aa3f) | Verdict | Phase |
| --- | --- | --- | --- | --- |
| 1 | Colour solver (mix weights per target colour) | Phase 1, MIT mixer, no LUT | OK | - |
| 2 | Image to model colour (projections: flat, wrap, box, mesh UV) | Phase 2 facet painting, 4 projections, faces rule | OK | - |
| 3 | Top-surface image at nozzle resolution | Phase 3 image row on top solid infill, ironing, Arachne/Concentric tops (phase 4) | OK | - |
| 4 | Side-wall image | Phase 4 split of the outer wall into per-filament runs at G-code time | **GAP**: no GUI control; only reachable through the model API | 5a |
| 5 | Side-wall image with facet painting | The dialog always paints facets; painted regions overwrite `wall_filament` with the painted id (PrintApply.cpp), so a wall row would be lost exactly as the top row was before `fix/imagefill-dither-effect` | **GAP** | 5a |
| 6 | Tool changes and purge for wall images | ToolOrdering registers the row's whole candidate set on every perimeter layer. An extruder in `layer_tools.extruders` always gets a tool change and a tower purge, even with nothing to print (Print.cpp:6539/6701, GCode.cpp:8524-8572); the ToolOrdering comment saying otherwise is wrong. AMS mapping then asks for colours the image never uses. | **GAP** | 5a |
| 7 | Where the image does not land | Wall and top samplers ignore the faces rule: a flat Z picture bound to walls is smeared down every side. Unsplit wall/top pieces print the row's per-layer cycle colour (a different filament each layer) instead of the part's own filament. | **GAP** | 5a |
| 8 | Top-visible perimeters (ImageMap "recolor top visible perimeter sections") | A top image stops at the perimeters: a 1-2 mm ring of the base colour frames the picture | **GAP** | 5a |
| 9 | Plate filament counts / wipe tower size | Expanded through `expand_virtual_extruder_ids` in the GUI; the CLI estimate (`PartPlate::estimate_wipe_tower_footprint`) never expands mixed ids, so the tower is sized for 1 filament. Re-applying an image to an existing row leaves `component_a/b` stale. | **GAP** | 5a |
| 10 | H2D/H2C filament grouping, U1 tool mapping | Grouping reads `lt.extruders`, which holds the run filaments; U1 keeps filament n on tool n | OK (cost of #6 aside) | - |
| 11 | 3MF persistence of zones/rows and assets | Volume keys and row assets are saved (bbs_3mf.cpp:9052, :9966); no test reloads a row-only asset or a wall binding | **GAP** (test) | 5a |
| 12 | ImageMap's side-wall method: one filament per layer in a fixed order (C, M, Y, K, ...), outer wall width modulated by the image ("LineWidth" modulation, one tool change per layer) | Not present. Rows already fall back to a per-layer cycle through their filaments; nothing modulates the wall | **GAP** | 5b |
| 13 | Image placement (offset, scale, rotation, tiling, aspect ratio) - ImageMap's Image Projection gizmo | None: `image_fill_project` stretches the image over the whole mesh box | **GAP** | 5c |
| 14 | Remap: image survives simplify/cut | Cut, split to parts and merge re-centre or rebuild the mesh; the box is taken from the new mesh, so the image re-stretches over each half | **GAP** | 5c |
| 15 | ImportedTexture: OBJ + PNG/JPG (`map_Kd`) | The parser reads `map_Kd`, but the colour branch in Model.cpp is commented out: a textured OBJ comes in with no colour. GLB/glTF works. | **GAP** | 5d |
| 16 | 2D gradient / linear gradient sources | 2- and 3-stop gradients along u, v or radial | OK (subset) | - |
| 17 | Dithering methods (Floyd-Steinberg, Bayer, halftone) | 1-D error diffusion on rows; nearest per leaf on facets | OK (subset) | - |
| 18 | Contoning (ordered translucent stacks on top surfaces, TD models) | Dropped in Phase 1 | OUT | - |
| 19 | TrueColor RGB painting gizmo | None | OUT | - |
| 20 | Interactive projection gizmo with 3D preview | Dialog with a 2D quantised preview | OUT (5c adds numeric placement instead) | - |
| 21 | Seam hiding hint from the texture | None | OUT | - |
| 22 | Prime-tower images, calibration Electron app, pigment-painter LUT | None | OUT (locked decisions in #10-#14) | - |
| 23 | Paint-depth + image on one object ("C3") | Works through the same region code | OK, best effort (no-crash test in 5a) | 5a |

## 2. What "works fully" means: acceptance criteria

Each criterion is testable, and the phase that proves it is named.

**A. Top surfaces (already true, kept true).** A plaque with a ramp image and "Top surfaces" ticked
prints the top layer in three filaments in Y order (`test_image_row_dialog.cpp`).

**B. Side walls from the dialog (5a).** Applying an image with "Side walls" ticked and a box or wrap
projection makes the mid-height outer wall of a cube or cylinder multi-coloured in the G-code, with
facet painting on (the dialog path), not only through the model API.

**C. Tool changes track the image, not the candidate list (5a).** On a layer whose outer wall the
image puts only one colour on, the G-code has no tool change to the other candidates, with the
prime tower ON. Per-layer tool changes stay at or below `2 * (filaments used on that layer - 1)`.

**D. The image lands only where it points (5a).** A flat Z image bound to the walls leaves the side
walls in the part's own filament (no streaks); unsplit wall and top pieces print the part's own
filament, never the row's per-layer cycle.

**E. No base-colour frame (5a).** With "Top surfaces" ticked, the top layer's outer wall takes the
image's colours where it is seen from above.

**F. Counts (5a).** The CLI wipe-tower estimate counts an image row's filaments; re-applying with a
different pair updates `component_a/b`.

**G. Persistence (5a).** Save and reload a project whose picture is referenced only by a row bound to
the walls: the asset, the row and the binding come back and the reloaded slice matches.

**H. Layer stripes (5b).** With wall method "Layer stripes": every wall layer uses exactly one
filament, the filaments follow the row's order layer by layer, the outer wall's extrusion per mm
varies along the loop with the image (wider where that layer's filament is wanted), at most one
tool change per layer is added by the walls, and a layer whose image is one colour is wide only
on that colour's layers.

**I. Placement (5c).** Offset, scale, rotation, tiling and "keep aspect ratio" move the picture by
the expected amount in `image_fill_project` (unit tests with known points); defaults reproduce
today's mapping bit for bit.

**J. Cut keeps the picture in place (5c).** After a planar cut of an image-filled box, a point on
either half samples the same pixel it sampled before the cut.

**K. Textured OBJ (5d).** Importing an OBJ with a `map_Kd` PNG paints the part from the texture
through the same path GLB uses.

**L. Nothing else moves.** For every phase, the standard G-code gate against `_baseline` is
IDENTICAL on every case. Image-row test cases may differ from main and each difference is
explained in the PR.

**M. Speed.** Slicing a Benchy with a box-projected image on top and walls stays within 1.5x of the
same Benchy without the image (measured with the CLI and reported in 5a and 5b).

## 3. What stays OUT, and why

| Item | Why |
| --- | --- |
| Contoning | It needs a per-filament transmission distance that no profile carries (a new filament key, calibration data, and a profile pass), and ImageMap's implementation is about 13k lines of fill geometry. The flat-face form of the idea is Orca #16551 (ChromaStack), still a draft upstream; the research note recommends porting that when it leaves draft rather than reviving Contoning. Decision for the owner if wanted sooner. |
| TrueColor RGB painting gizmo | A second painting gizmo (ImageMap copied a 1 MB MMU gizmo for it). Image Fill plus the existing colour painting and texture displacement colours already cover "put a picture on it"; the cost is high and the overlap large. |
| Interactive projection gizmo | Phase 5c gives the same placement as numbers in the dialog. A canvas gizmo can reuse texture displacement's adjust handles later if the owner wants it. |
| Seam hiding from the texture | Small visual gain, touches SeamPlacer for every print path. |
| Prime-tower images, calibration Electron app, pigment-painter LUT (GPL, 36 MiB LFS) | Locked OUT in #10-#14; nothing has changed. |
| A pre-slice 3D preview of nozzle-resolution colour | The slice preview already shows it per run (phase 4 test); the 3D view keeps showing the facet painting. |

## 4. Package split

Each phase is one draft PR. 5a goes first; 5b and 5c build on 5a's params and dialog changes, so
their branches start from 5a's branch (stacked) and are rebased onto main once 5a merges. 5d is
independent of the others and starts from origin/main.

### 5a - `feat/imagemap-p5a-walls-wiring`: side walls from the dialog, and correct

- Dialog: "Nozzle-resolution colour" with two boxes, **Top surfaces** and **Side walls**.
- `ImageFillParams::surfaces` (key `rs`, bit 1 top, bit 2 walls; absent = legacy).
- Plater: binds `solid_infill_filament` for top and `wall_filament` for walls or top (the top ring
  needs the wall split); unbinds what is unticked; keeps `component_a/b` in step on update.
- PrintApply: painted regions keep an image row on `wall_filament` (both region paths).
- Walls: faces rule per sample; top-visible samples (not covered by the layer above) use the up
  normal when the top bit is set; samples the image does not reach print the part's own filament
  (the owning volume's extruder, or the painted region's id).
- Tops: when the projection does not land on an upward face, the whole surface prints the part's
  own filament instead of the cycle colour.
- ToolOrdering: registers the filaments the wall's runs actually use (computed from the same
  sampler on the layer's loops) instead of the candidate list; GCode maps any run whose filament
  is not in the layer's list to the nearest registered colour instead of `extruders.back()`.
- CLI wipe-tower estimate expands mixed ids from the project config.
- Tests: dialog-path walls, faces rule, base filament, top ring, tool changes with the tower on,
  3MF round trip of a wall-bound row, C3 no-crash.

### 5b - `feat/imagemap-p5b-layer-stripes`: ImageMap's side-wall method

- Row option **Wall method**: *Colour runs* (phase 4, default) or *Layer stripes*.
- Layer stripes: the walls print one filament per layer, cycling through the row's filaments in
  their listed order; the outer loop is cut at sample spacing and each piece's width (and
  extrusion) is set from the image's demand for that layer's filament, between a minimum and a
  maximum width (row keys `wn`, `wx`, percent of the outer wall width). The inner edge of the bead
  stays put; only the outside moves.
- Tests: one filament per wall layer, order follows the row, E per mm tracks the image, tool
  changes per layer at most one more than without the image.

### 5c - `feat/imagemap-p5c-placement`: placement and staying in place

- Params: offset (u, v), scale, rotation, tile, keep aspect ratio; dialog controls.
- A per-volume projection frame recorded at apply time, carried through cut, split to parts and
  merge so a half keeps sampling the picture where it was.

### 5d - `feat/imagemap-p5d-obj-texture`: textured OBJ import

- Decode the `map_Kd` image and sample it per sub-facet in UV space, then
  `image_fill_from_face_colors`, the path GLB already uses.

## 5. Owner decisions (none blocking)

1. Contoning / ChromaStack: stays OUT here; port ChromaStack when it leaves draft (research note),
   or ask for a fork-native flat-face version sooner.
2. Layer stripes is opt-in and marked experimental until the owner's print shows whether width
   modulation reads well on his filaments; its defaults may change after that print.
