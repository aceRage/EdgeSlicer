# Spike: porting OrcaSlicer "Texture displacement" (#14662 + #16148)

Date: 2026-10-06. Budget 1 day; used about 4.5 h (most of it build-queue wait; three build rounds). Measurement only, never merge.
Branch `spike/texture-displacement` (worktree `C:\Dev\wt_texdisp_spike`, from origin/main 2e42dc001e).
Context: `tests/research_png_emboss_and_relief.md` option 2B (which guessed 15-25 d).

## 0. Result in one paragraph

The port is **far cheaper than the 15-25 d guess**. Cherry-picking the merged PR onto our main gave
**6 conflicted files / 10 hunks, all trivial** (about 30 min). The open 3MF follow-up #16148 added
**7 small hunks in `bbs_3mf.cpp`** (about 30 min). A full Ninja build with `-k 0` then showed
**about 12 distinct compile root causes, only one of them a real prerequisite** (the core of upstream
#13472 "Keep painting after cut": `TriangleSelector::remap_painting` / `SavedPainting`,
`ModelVolume::save_painting/restore_painting`, `FacetsAnnotation::set_data`). The rest were small API
shims. After round 2 the only failing object was a typo of mine; **round 3 built the whole app (EdgeSlicer.exe) and both test suites, and every test passed**: libslic3r_tests 1,836 cases (1,833 passed, 3 pre-existing `[!shouldfail]`), including all 46 upstream `[TextureDisplacement]` cases (4,279 assertions, incl. the #16148 3MF round trips); fff_print_tests 300 cases / 44,862 assertions all passed (incl. the 2 upstream bake-then-slice cases). GUI not run (no desktop control; owner hand-test).
Nothing in the slicing pipeline, `PrintObject`, `GLGizmoPainterBase.cpp`, ObjectList, our Image Fill,
EmbossShape/SVG or curved-text code is touched by the PR. **Realistic estimate: 5-7 agent-days (est.)
to a shippable, hand-tested drop including 3MF persistence**, plus about 1 d per quarter to track
upstream follow-ups. Recommended: **one port PR** (feature is self-contained), gated behind the gizmo,
with #16148 folded in so we never ship an unsaveable state.

## 1. Upstream commits

| Item | Commit | Notes |
|---|---|---|
| PR #14662 "Texture displacement" (merged 2026-10-01) | merge `ba361d98823f8991675daf8488937e42578f1db3` (parents: main `865e9963c3`, PR head `bd4306e8f8`) | 80 commits on `feature/texture_displacement`, PR merge-base `da0611a301` (2026-09-30). Applied as `cherry-pick -m 1` of the merge (squashed diff). +30,013 / -48, 159 files. Author ExPikaPaka, merged by SoftFever. |
| PR #16148 "save layers, textures and paint in the .3mf" (OPEN) | `fcb1f62bd9` (single commit on top of newer main) | +546/-1, 4 files: `bbs_3mf.cpp` +151, `TextureDisplacement.{hpp,cpp}` +259 (JSON round trip), tests +136. |
| Prerequisite: PR #13472 "Keep painting after cut" (merged 2026-05-17) | merge `1c0d0b89cc` | +665/-92, 18 files. Spike ports only `TriangleSelector.{hpp,cpp}` and `ModelVolume::save/restore_painting` + `FacetsAnnotation::set_data` (~420 lines). Its cut/split/ObjectList/MeshBoolean/Simplify parts are **not** needed for the feature. |
| Merged follow-ups since (main 78f74a6276, 2026-10-06) | #16076 (checkbox contrast + panel styling, +43/-43), #16071 (missing includes), #16142 (translations) | Drift on the feature's own files since the merge: about 250 lines (171 in the gizmo), mostly includes. Cheap to replay. |
| Open follow-ups | #16147 (Checker/Distortion view fix + UV editor tooltips, +142), #16149 (net layout 3x faster, +425), #16069 (video/wiki links) | Upstream is still churning; take them when merged. |

Spike branch commits (each ends with `Deft`):
1. `56c6379b73` cherry-pick of #14662 with the 6 conflicted files resolved.
2. `e75bd75d8e` mechanical fixups (upstream-only includes, Catch2 v2 test headers).
3. `5f1e1c812d` apply #16148.
4. `5c38fb54b6` shims + minimal #13472 core.
5. `2e495c3ca9` close `restore_painting()` (my own paste error from round 2).

## 2. Files the PR touches

**Pure additions (142 files, no conflict possible):**
- libslic3r: `TextureDisplacement.{hpp,cpp}` (1,139 + 5,044 lines), `TextureBake/*` (Debug, Decimate, Displace, Flip, Index, Mesh, Pipeline, Regularize, Relocate, Repair, Subdivide; 22 files, ~3,700 lines).
- GUI: `Gizmos/GLGizmoTextureDisplacement.{hpp,cpp}` (946 + 6,942), `UVEditorCanvas.{hpp,cpp}` (441 + 2,150), `TextureLibrary.{hpp,cpp}`, `TextureProjectorFrame.{hpp,cpp}`, `Jobs/TextureDisplacement{Bake,Prepare,Preview,Debug}Job.{hpp,cpp}`.
- Resources: 41 SVG icons (`texture_displacement_*.svg`, `toolbar_texture_displacement.svg`, `toolbar_big_brush.svg`, `toolbar_face.svg`), 4 shaders x 2 GLSL versions (`resources/shaders/{110,140}/texture_displacement_{shaded,uvcheck}.{vs,fs}`), 44 procedural 512x512 PNG height maps in `resources/textures/displacement/`, `scripts/generate_displacement_textures.py` (generator for those PNGs).
- Docs `TEXTURE_DISPLACEMENT.md`, `TEXTURE_DISPLACEMENT_GUIDE.md` (repo root; we would move them under `docs/`).
- Tests `tests/libslic3r/test_texture_displacement.cpp` (2,114 lines), `tests/fff_print/test_texture_displacement.cpp` (149).

**Modified existing files (17):**
`src/libslic3r/CMakeLists.txt`, `MeshBoolean.{hpp,cpp}` (+175: CGAL LSCM `parameterize_lscm`, `remesh_isotropic`), `Model.{hpp,cpp}` (8 per-layer `FacetsAnnotation` members on `ModelVolume`, layer/option structs, copy ctor, ids, `reset_extra_facets`), `PNGReadWrite.cpp` (setjmp-guarded decode; behaviour-neutral hardening), `TriangleSelector.{hpp,cpp}` (`get_facets_strict(..., out_source)`), `src/slic3r/CMakeLists.txt`, `GLShadersManager.cpp` (2 shaders), `GLTexture.{hpp,cpp}` (+44), `GLGizmoPainterBase.hpp` (new `PainterGizmoType`), `GLGizmosManager.{hpp,cpp}` (EType + registration + icon), `ImGuiWrapper.cpp` (light-theme check-mark colour, global), `Plater.{hpp,cpp}` (+87: UV editor AUI pane), `tests/{libslic3r,fff_print}/CMakeLists.txt`.

No `deps/` change: CGAL `Surface_mesh_parameterization` and `Polygon_mesh_processing/remesh` headers are already in our CGAL and compiled clean. No change to `PrintObject`, `Print`, slicing, G-code, `bbs_3mf.cpp` (in #14662), `GUI_ObjectList`, `GLGizmoPainterBase.cpp`, `GLCanvas3D`.

## 3. Conflict and compile table (measured)

| Area | What happened | Effort to do properly (est.) |
|---|---|---|
| `Model.hpp` (4 hunks) | Our includes (ImageFill, FlexiJoint, CutRecipe, CadBody) vs theirs; copy-ctor init list needs our `cad_body` too; two pure-add hunks. | 0.1 d |
| `MeshBoolean.hpp` (1) | Context only (we already had `repair()` elsewhere); our Manifold/mcut backends untouched. | done |
| `slic3r/CMakeLists.txt` (2) | Context around upstream-only `GLGizmoUtils` and our `ThumbnailView`. | done |
| `GLGizmosManager.cpp` (1) | Our Sculpt/Edit/CadFillet names vs theirs; enum order and `m_gizmos` order line up (TextureDisplacement after MmSegmentation). Toolbar placement, dark icon, shortcut not reviewed in GUI. | 0.25 d incl. toolbar/dark icon |
| `Plater.cpp` (1) | Window-layout restore: upstream also had Wayland/fallback logic we lack; kept ours + the "hide uv_editor pane" bit. | done |
| `tests/fff_print/CMakeLists.txt` (1) | Our test list vs theirs. | done |
| `bbs_3mf.cpp` (#16148, 7 hunks) | Our CAD-body files, fuzzy-skin attribute alias, inline-SVG `to_xml` signature vs theirs; upstream's `get_dealed_platform_path` context dropped. All "keep both". | 0.25 d + round-trip tests 0.5 d |
| Prerequisite #13472 core | `TriangleSelector::SavedPainting`, `remap_painting` (+ `TriangleCursor`, `select_partially`), `ModelVolume::save/restore_painting`, `FacetsAnnotation::set_data`. Applied cleanly except one hunk (dropped upstream-only `extract_used_facet_states`). Used by the gizmo and Prepare job to keep support/seam/MMU/fuzzy paint when subdividing/remeshing before a bake. | 0.5 d (review + its own tests; it is also a nice standalone win for our Simplify/Boolean) |
| GUI API drift (shims) | `OpenGLManager::GLInfo::is_core_profile()` (we are compatibility-profile only -> `false`), `GLModel::render(range, shader)` (ours uses the bound shader), `Plater::get_extruders_colors()` (rewritten over our `get_extruder_colors_from_plater_config(nullptr, false)`), `ImGuiWrapper::COL_ORCA_HOVER`, static `ScalableBitmap::GetBmpSize(bmp)`, `SETTING_OPENGL_AA_SAMPLES`, includes `GuiColor.hpp` / `GLGizmoUtils.hpp` (unused) and `ColorSpaceConvert.hpp` (for `DeltaE00`). | 0.5 d to turn the shims into proper small backports |
| MSVC C++17 | Two `constexpr` locals read inside lambdas without capture (C3493) - upstream builds as C++20/clang-friendly; `static constexpr` fixes. | done |
| Tests | Catch2 v3 (`catch_all.hpp`) -> our Catch2 v2; `test_helpers.hpp` -> our `test_data.hpp`, `Test::cube(20.)` -> `mesh(TestMesh::cube_20x20x20)`; #16148 test used `TextureTileMethod::Mirror` (renamed upstream later) -> `MirroredRepeat`. | done |
| Shaders / resources | Added as-is; GLSL 110/140 like ours, `glad/gl.h` like ours (#351). Runtime compile not verified (needs GUI). | hand-test |
| Translations | ~350 `_L`/`_u8L` strings, English only until a gettext pass. | 0.25 d (pot update) |

Build rounds (standard `agent_build.sh`, Ninja + unity + PCH, `-k 0`):
- Round 1 (after conflicts): 246 failed objects, but almost all from one header error (`SavedPainting` in `Model.hpp`); about 12 distinct root causes listed above.
- Round 2 (prereq + shims): **1 failed object** (`Model.cpp`, a missing brace from my own paste). Every new libslic3r, TextureBake, GUI, job and test object compiled.
- Round 3: **BUILD_EXIT=0** (app + libslic3r_tests + fff_print_tests linked). Tests: libslic3r 1,836 cases, 0 unexpected failures; `[TextureDisplacement]` 46/46 (4,279 assertions); fff_print 300/300 (44,862 assertions). 29 min wall clock incl. lock wait. The GUI (gizmo, shaders, UV editor pane) was **not** exercised.

## 4. Prerequisites and risks

- **Prerequisites:** only the #13472 core above. No new dependency, no CMake option, no deps rebuild.
- **Model/undo:** eight new `FacetsAnnotation` members on `ModelVolume` (ObjectBase ids) plus `texture_displacement_layers/options`. That is exactly the kind of change the Image Fill spec avoided (`ImageFill.hpp`: "adds no ObjectBase member anywhere"). Undo/redo (cereal) and the "; model label id" G-code gate need checking: the members are not in `ModelVolume::serialize` in #14662 itself, so a paint stroke's undo relies on `FacetsAnnotation` snapshots like the other channels. Must be covered by a test before shipping.
- **Mesh replacement drops our extras:** a bake calls `set_mesh()`; our `cad_body` (CAD fillet) is then dropped by its fingerprint check (handled, but silent), and a baked **emboss/SVG/text volume** keeps its `emboss_shape`/`text_configuration`, so the next re-edit regenerates the mesh and silently throws the relief away. Needs a guard (disallow, or convert to a plain part on bake).
- **Colour mode overlaps Image Fill.** Upstream layers can also colour the painted area from an RGB texture, writing `mmu_segmentation_facets` (merged over existing paint) with its own palette matching (CIEDE2000) and filament mixing (Z bands / XY dither). Our Image Fill writes the same channel from per-part config + `ImageAssetStore`. Last writer wins; neither knows the other. Our mixed-filament feature (#375) adds virtual filaments that upstream's palette code does not know about (the spike passes `include_mixed = false`).
- **Large-depth self-intersection:** upstream says so; no check. Same class of fault as our bevel cone.
- **Upstream churn:** three open TD PRs today; #16148 not merged, so its 3MF format could still change before upstream merges it. Porting it now risks a format fork; mitigation in section 7.
- **Global side effect:** `ImGuiWrapper::push_toolbar_style` check-mark colour changes for every light-theme gizmo (it is a fix, but visible).

## 5. Licence

Upstream OrcaSlicer is AGPL-3.0, same as EdgeSlicer, so the code can be ported as-is. Keep authorship: port with the upstream commit/PR reference in the commit message (`Ported from OrcaSlicer #14662 by ExPikaPaka, merge ba361d9882`; `#16148`; `#13472`), keep `TEXTURE_DISPLACEMENT*.md` attribution, and list it in our release notes/credits. The 44 shipped textures are procedural from `scripts/generate_displacement_textures.py` (no third-party art). The source files carry no per-file licence header (Orca removed those), so nothing to preserve there beyond git history.

## 6. Lithophanes: does upstream already cover them?

**Partly - it covers the geometry, not the lithophane workflow.** What it does today:
- Any PNG/JPG/BMP imported as an 8-bit grey height map, aspect-correct, with depth (mm), **invert** (dark = thick), midlevel 0 (only outward), smoothing, edge feathering.
- **Decal mode** (`tile_enabled = false`, clamps outside [0,1)) and **From view** projection with a resizable projection frame, so "one image, once, on this face" works.
- **Cylindrical** and spherical projections, plus LSCM unwrap - so a curved lithophane on a cylinder primitive is possible.
- Adaptive subdivision with a triangle budget (default 1 M) so 0.1-0.2 mm detail on a 100 x 150 mm face is reachable.

So a user can make a flat or curved lithophane by hand: add a box (or cylinder) primitive, paint the front face, load the photo with Invert, depth = max - min thickness, decal/from-view placement, bake.

What a lithophane flow would still need (none of this is upstream):
- **One-click plate generator:** size the plate from the image aspect and a chosen width, min/max thickness (e.g. 0.8 / 3.0 mm) rather than "box thickness + depth".
- **Transmission/tone curve:** upstream maps grey linearly to height. Lithophanes want a Beer-Lambert (exponential) mapping plus gamma/contrast so mid-tones read correctly; one LUT applied before sampling.
- **Frames/borders, stand/foot, hanging hole, rounded corners**, and curved presets (arc angle, radius) for a curved plate rather than a full cylinder.
- **Print profile hints:** 100% infill, outward walls, vertical orientation, layer height - our existing preset machinery, not geometry.
- **Back-side flatness:** displacement moves vertices along normals; with a decal on one face the back stays flat, which is what we want, but edge feathering must be off at the frame boundary.

Conclusion: do **not** build a separate height-field mesh generator (research 2A, 9-12 d) if this port lands. A lithophane mode becomes a **thin wizard over texture displacement**: create the primitive at the right size, add a layer with Invert + decal + tone LUT, add a frame (a second primitive or a boolean union), bake. Estimate **2-4 d (est.)** on top of the port, against 9-12 d for a standalone generator.

## 7. Interactions

- **Image Fill (colour):** complementary for geometry (Image Fill never moves vertices), overlapping for colour (both write MMU paint, section 4). Recommendation: port with upstream's colour mode **hidden or off by default** at first and route TD colour through Image Fill's palette/mixed-filament mapping in a second step; at minimum warn when a volume already has an Image Fill annotation. A bake that subdivides keeps Image Fill's MMU paint via `restore_painting()` (the #13472 remap), but the Image Fill annotation itself is per-part config and can simply be re-run.
- **PNG trace -> SVG emboss (planned, research option 1):** different job, no conflict. Trace gives crisp N-level relief as **separate emboss parts** (own filament, re-editable, no mesh mutation, safe in 3MF everywhere). TD gives **continuous** grey relief baked into the part's own mesh. Keep both; the trace tool's "stepped relief" bridge (+2 d in the research doc) becomes less important once TD exists, so it can be dropped from option 1's MVP. Guard: TD must not bake onto an emboss volume (section 4).
- **3MF compatibility:**
  - **Baked** displacement is just a denser mesh: opens everywhere (Bambu Studio, older EdgeSlicer, Prusa), no format change. This is all #14662 alone produces.
  - **Unbaked** layers/masks need #16148: per-triangle attributes `paint_texture_0..7`, a `texture_displacement` volume metadata key pointing to `Metadata/texture_displacement/<id>.json`, and the images under the same folder. Older EdgeSlicer: unknown triangle attributes are ignored; the unknown metadata key goes to `volume->config.set_deserialize()`, whose `handle_legacy()` blanks keys it does not know (`print_config_def.has()` check), so it is silently dropped; the extra archive files are ignored. Result: the old build opens the file with the unbaked surface and no texture, no error. Bambu Studio is expected to behave the same (same importer lineage) - **not verified** in this spike.
  - Our **"Export Bambu 3MF"** path and printer-send 3MFs need a check that the `Metadata/texture_displacement/` payload is not shipped to printers (size) - sliced `.gcode.3mf` should only carry the mesh.
  - Format risk: #16148 is still open. Port it but keep its reader tolerant, and re-sync when upstream merges.

## 8. Recommended port strategy and estimate

**One port PR, staged in commits** (not several PRs): the feature is self-contained (one gizmo, its jobs, a libslic3r module) and a partial drop (e.g. libslic3r without GUI) has no user value.

1. #13472 core (TriangleSelector remap + save/restore painting) with its own tests - 0.5 d.
2. #14662 cherry-pick + conflict resolution as in the spike, shims replaced by real small backports - 0.75 d.
3. Merged upstream follow-ups (#16076, #16071 bits) - 0.25 d.
4. #16148 3MF persistence + round-trip tests (EdgeSlicer save/load, old-build load, Bambu export strip) - 1 d.
5. Our guards: no bake on emboss/text/SVG volumes, cad_body notice, Image Fill / mixed-filament colour interplay (colour mode off by default), undo/redo tests, "; model label id" gate - 1-1.5 d.
6. Toolbar placement/dark icon, gettext pot, docs move - 0.5 d.
7. Owner hand-test round (GUI, shaders on Windows + Mac, large meshes) and fixes - 1-2 d.

**Total 5-7 agent-days (est.)**, versus the research doc's 15-25 d guess. Lithophane wizard on top: 2-4 d (est.). Ongoing: replay upstream TD PRs (three open now) about 1 d per quarter while it churns.

## 9. Housekeeping

- Worktree `C:\Dev\wt_texdisp_spike` kept (useful starting point for the real port: conflicts already resolved, shims marked `EdgeSlicer spike:`). Build dir deleted at the end.
- Scratch: `%TEMP%\texdisp_scratch` (resolver scripts, build outputs).
- Remote-tracking ref added: `orcaupstream/pr16148` (fetch only). Note: the local clone is **shallow** for upstream history, so `git merge-base` with Orca main is unreliable; use `gh` for PR provenance.
