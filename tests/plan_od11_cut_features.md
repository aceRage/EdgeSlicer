# OD11 Cut features: port plan (phase 1)

Owner approved OD11 on 2026-10-24 ("be careful on this one"). This is the phase 1 plan. No code is ported yet.

Branch `port/orca-od11`, worktree `C:/Dev/wt_pkg_od11`, based on origin/main `0b3af03608`.

## Upstream PRs (pinned)

| PR | Merge commit | Files | Size |
|---|---|---|---|
| #13472 Keep painting after cut | `1c0d0b89cc` | 18 files: CutUtils, Model, TriangleSelector, ObjectList, MeshBoolean, Simplify, Plater, Preferences, FixModelByCgal, CLI | +665 / -92 |
| #12318 Multiple dovetail cuts | `15451c6c50` | CutUtils.cpp/.hpp, GLGizmoCut.cpp/.hpp | +538 / -237 |
| #13945 Check the x offset of every dovetail | `3cce9b09ed` | GLGizmoCut.cpp | +10 / -11 |

- #12318 was written on top of #13472. Its hunks assume `perform_by_contour(src_object, ...)` and the `KeepPaint` attribute.
- Upstream has changed nothing in these files since then except include clean-ups (#16220, #16221).

## What Edge already has

- **The #13472 core is already in**, from `7165e11e43` (texture displacement):
  - `TriangleSelector::remap_painting` and `SavedPainting`, byte-identical to upstream.
  - `ModelVolume::save_painting` / `restore_painting`, `FacetsAnnotation::set_data`, `is_any_painted`.
  - Tests in `tests/libslic3r/test_paint_remap.cpp` ([PaintRemap]).
  - Today the only callers are the texture-displacement gizmo and its jobs.
- **Today a cut drops all painting.**
  - Every `Cut::perform_*` calls `volume->reset_extra_facets()`.
  - `add_cut_volume()` copies neither facets nor `texture_displacement_layers`.
  - So colour, support, seam and fuzzy-skin paint, the 8 texture-displacement masks and the model UV channel are all lost on every cut.
- **Our cut code has six entry points; upstream has three:**
  - `perform_with_plane`: ours adds the kerf ("thickness") and a dispatch to flexi joints.
  - `perform_with_curved_sheet`: Edge only.
  - `perform_with_draw_stroke`: Edge only.
  - `perform_with_flexi_joints`: Edge only.
  - `perform_by_contour`: identical to upstream before #13472.
  - `perform_with_groove`: identical to upstream before #13472.
  - All six build their halves through the same `add_cut_volume()`. So the half's mesh plus `get_init_shift()` sits in the frame `restore_painting()` expects (the instance's linear part times the volume matrix).
- **The gizmo functions #12318 touches are identical to upstream's pre-#12318 text:** `its_make_groove_plane`, `render_slider_double_input`, `render_groove_float_input`, `process_contours`, and the groove block of `render_cut_plane_input_window`.
- Only these need adapting:
  - the `perform_cut` call site;
  - `on_save`/`on_load`;
  - the re-editable cut recipe;
  - the label map.

## Hunk map

Key: **clean** = applies as is. **adapt** = needs Edge changes. **conflict** = clashes with an Edge feature (named). **present** = already in. **skip** = not taken, with the reason.

### #13472 Keep painting after cut (`1c0d0b89cc`)

| File / hunk | Verdict | Note |
|---|---|---|
| TriangleSelector.cpp/.hpp: `remap_painting`, `SavedPainting`, cursor helpers | present | From `7165e11e43`, byte-identical. |
| Model.cpp/.hpp: `save_painting`, `restore_painting`, `set_data`, `is_any_painted` | present | From `7165e11e43`. |
| Model.hpp: remove `exterior_facets` | skip | We still reset it in `ModelVolume::split`. It is harmless. |
| Model.cpp: `ModelObject::split(.., remap_paint)` and `ModelVolume::split(.., remap_paint)`, plus COPY_FACETS for multi-volume split | conflict: open PR #605 | #605 makes split always keep painting with PaintTransfer (exact facet copy, no flag). OD11 leaves split to #605 and does not change the `split()` signature. |
| CutUtils.hpp: `KeepPaint` attribute, `finalize(objects, saved)`, `perform_by_contour(src_object, ...)` | adapt | Same API. `perform_with_curved_sheet`, `perform_with_draw_stroke` and `perform_with_flexi_joints` also save before `reset_extra_facets()` and pass the saved painting to `finalize`. |
| CutUtils.cpp `perform_with_plane`: save before reset, `finalize(.., saved)` | adapt | Our function has the kerf and the flexi dispatch at the top. Saving goes after the dispatch, so the flexi path saves its own (see next row). |
| CutUtils.cpp `perform_by_contour`, `perform_with_groove`: save from the source object | clean | The functions are identical to upstream. |
| CutUtils.cpp curved / draw / flexi | adapt (Edge only) | Same three lines (save, transform to cut space, finalize). Each needs its own test. |
| CutUtils.cpp `process_volume_cut` / `process_solid_part_cut` const-ness | clean | Cosmetic. Take it so the flexi and #569 signatures line up. |
| GLGizmoCut.cpp: `PartSelection` `split(1, false)` | adapt | We keep `split(1)` (signature unchanged, see #605). `perform_by_contour` remaps from the original object `mo`, so split painting is not needed. |
| GLGizmoCut.cpp: `perform_cut` adds `KeepPaint` and passes `mo` to `perform_by_contour` | adapt | Our attribute list also has the flexi `KeepAsParts` rule. Where `KeepPaint` comes from depends on decision D1. |
| GLGizmoCut.cpp: fix-after-cut lambda passes `keep_painting` to `fix_model_with_cgal_gui` | skip | We have no FixModelByCgal. Our block is `HAS_WIN10SDK` / `fix_model_by_win10_sdk_gui`. PR C handles repair. |
| ObjectList `split()`, `boolean()` | conflict: #605 | #605 covers both with PaintTransfer (and Union keeps each part's filament). Not taken here. |
| ObjectList `merge()`: drop the `mmu_segmentation_facets.assign` | skip | Not cut related. Merge keeps today's behaviour. |
| ObjectList `fix_through_cgal`, `smooth_mesh` | adapt (PR C) | Our repair is the remesh/manifold path (`clear_before_change_mesh` at GUI_ObjectList.cpp ~6458, 6611, 7085). We have no smooth_mesh. |
| GLGizmoMeshBoolean: `VolumeInfo::save_painting` | conflict: #605 | Not taken. |
| GLGizmoSimplify `apply_simplify` | adapt (PR C) | Simple: save, set mesh, restore, instead of `clear_before_change_mesh`. |
| Plater `split_object` | conflict: #605 | |
| Plater `replace_volume_with_stl`, `reload_from_disk` | adapt (PR C) | Simple. |
| Plater `combine_mesh_fff` std::move | skip | Only needed by the boolean hunk. |
| Preferences: "(Experimental) Keep painted feature after mesh change", key `keep_painting`, default off | adapt per D1 | |
| FixModelByCgal.cpp/.hpp | skip | File does not exist here (OD17 territory). |
| Geometry.hpp: `deg2rad` static_assert | skip | Unrelated. No integer call sites found, but not needed. |
| TriangleMeshSlicer.cpp: comment | skip | Comment only. |
| OrcaSlicer.cpp CLI `split(&new_objects, false)` | skip | Signature unchanged. Same file as `src/Snapmaker_Orca.cpp:4559`. |

### #12318 Multiple dovetail cuts (`15451c6c50`)

| File / hunk | Verdict | Note |
|---|---|---|
| CutUtils.cpp `perform_with_groove(groove, rot, count, gap, radius, keep_as_parts)`: per-groove isolation cuts plus offset angle and tolerance cuts | clean, plus 2 fixes | 1. Upstream leaks one cloned `groove_object` per groove (`clone_for_cut` returns a new ModelObject that is never freed). Free it. 2. With count = 1 it must give the same mesh as today; covered by a test. |
| CutUtils `calculate_groove_width(groove, radius)` | clean | Depends on the gizmo radius. See D5 and the recipe row. |
| CutUtils.hpp signature | clean | Default arguments keep existing callers (tests) compiling. |
| GLGizmoCut.hpp: `m_groove_count`/`_init`, `m_groove_gap`/`_init`, renamed or new render helpers, `offset_indices` | clean | |
| GLGizmoCut.cpp label map: Multiple, Count, Gap, Spacing | clean | New translatable strings. Run gettext. |
| `render_slider_double_input` renamed `render_slider_two_input`, plus new `render_slider_input` | clean | Same 3 callers as upstream. |
| `its_make_groove_plane` rewrite | clean | Our function is identical to upstream's old one. Upstream calls `Max(...)`, defined in neither tree. Use `std::max` (it would break clang on the Mac slot). |
| `render_groove_two_float_input` / `render_groove_float_input` / `render_groove_int_input` | clean | Upstream's `render_slider_input` shadows `m_imperial_units` with a local `false`. Use the gizmo member, as the other sliders do. |
| Panel block: Multiple: Count, Gap, Spacing | clean | Check it in the pinned / collapsible panel (df20fd89e1) and in Simple mode (bd48332044). |
| `process_contours` and `perform_cut` call sites pass count, gap, radius | adapt | Our `perform_cut` dispatch has five branches. Only the groove branch changes. |
| (not upstream) `on_save`/`on_load` | adapt (Edge fix) | Upstream does not add count/gap to the gizmo's undo archive, so Ctrl+Z on Count or Gap does not restore them. Add both. |
| (not upstream) cut recipe | conflict: re-editable cuts | `CutRecipeGroove` has no count, gap or radius. A multi-dovetail cut would re-edit as a single dovetail. See "Re-editable cuts" below. |

### #13945 (`3cce9b09ed`)

| Hunk | Verdict | Note |
|---|---|---|
| `m_groove_vertices` cleared once, then 8 vertices per groove with `+ offset_x` | clean | Folds into the #12318 commit's groove-plane text. Kept as its own commit, as the skill asks. With this, `has_valid_groove()` checks every dovetail, not just the last one. |

## Edge features at risk, and how each is protected

1. **Re-editable cuts (merge `311e7624b4`, CutRecipe v4, Edit cut / Copy cut / Mirror).**
   - Multiple dovetails need **recipe version 5** with `groove.count` (int, default 1), `groove.gap` (float, default 10) and `groove.plane_radius` (the gizmo `m_radius` the spacing was computed with).
   - Each is a new XML attribute in `bbs_3mf.cpp` (~3201 read, ~9613 write), added to the cereal `serialize`, `operator==`, `build_recipe_from_gizmo` and `apply_recipe_to_gizmo`.
   - **A v4 file loads as count 1** and re-cuts to the same halves: the defaults are today's single dovetail.
   - A **v5 file opened in an older Edge**: the reader refuses unknown versions. The halves still load; only "Edit cut" is not offered. This is the documented rule, not a regression.
   - **Mirror:** `cut_recipe_mirrored` leaves the groove alone. Dovetails are placed symmetrically about the plane centre, so X/Y/Z mirrors still need nothing. The [CutMirror] tests will cover count 3.
   - **Copy cut to...:** the spacing (`calculate_groove_width`) depends on the gizmo radius, which comes from the target's bounding box. Storing `plane_radius` keeps the source's spacing in mm. See D5.
   - **Edit cut and painting:** the stand-in object is rebuilt from `recipe.mesh` only. So even after PR A, re-editing a painted cut gives unpainted halves. See D2 and PR D.
2. **Draw cut (merges `7614df6381`, `c458c2eac3` fix/draw-cut-bunny; parked bunny list).**
   - OD11 does not touch the stroke, band, core or extension geometry.
   - Its only change in `perform_with_draw_stroke` is saving painting before `reset_extra_facets()` and remapping it in `finalize`.
   - Regression tests reuse the draw-cut fixtures in `tests/libslic3r/test_draw_cut.cpp` (loop and open line).
   - The parked bunny bugs are unaffected and stay parked.
3. **Curved cut, flexi joints, kerf.**
   - Same three-line save/finalize change. Each path gets its own paint test.
   - Kerf: the removed band has no source triangles, so the cut faces stay unpainted. Asserted.
4. **Connectors (plug, dowel, snap, flexi; dovetail is the groove mode).**
   - `finalize` skips `is_cut_connector()` volumes, as upstream does, so connector solids and dowel objects stay unpainted.
   - Dovetail mode and connectors: OD11 changes nothing there. The hand test checks that dovetail mode still offers no connectors, as today.
5. **Colour paint and the physical-filament / MixedFilament palette.**
   - `remap_painting` copies `EnforcerBlockerType` states verbatim, so virtual (mixed) filament IDs survive like physical ones.
   - Test: a state above the physical count, e.g. 20.
   - The volume's own `extruder` config is copied by `add_cut_volume` as today.
6. **Texture displacement (SecondaryId masks, `texture_displacement_layers`, `texture_uv`).**
   - Not carried by upstream's `SavedPainting`, and not copied by `add_cut_volume` today. After OD11 they are still dropped on a cut, exactly as today.
   - `restore_painting()` only fills the four standard channels. It does not touch the SecondaryId annotations' IDs, so undo/redo keying is unchanged.
   - Carrying them is decision D3.
7. **ImageMap (open stack #567-#569, #572, #574).**
   - The image itself is config (`image_fill_params`) and goes through `config.assign_config`.
   - #569 (5c) adds a per-volume `pre_transform` to `add_cut_volume()` in the plane, curved and draw paths. PR A touches the `perform_*` bodies and `finalize`, not `add_cut_volume`, so the conflict is small. Whichever lands second merges.
   - After both land, a cut keeps the ImageMap frame (#569) and the facet paint ImageMap applied (PR A). A test asserts both together once #569 is in.
8. **Open PR #605 (keep painting through Mesh Boolean and Split, Neotko PaintTransfer).**
   - Split and boolean stay with #605. OD11 PR A does not touch `Model.cpp` split, ObjectList, MeshBoolean or `TriangleSelector`, so the two PRs share no hunks.
   - Two remap engines then exist side by side: upstream `remap_painting` (cuts, simplify, reload) and PaintTransfer (boolean, split). Unifying them is a follow-up, not OD11.
9. **G-code.**
   - Cutting is a model edit, so G-code changes only where a cut half now carries paint it lost before.
   - The standard gate set has no cut objects. It must stay **identical** (gcode-gate skill, shared `_baseline`).

## Regression tests to add

New `tests/libslic3r/test_cut_paint.cpp` ([CutPaint]), registered in `tests/libslic3r/CMakeLists.txt`, Catch2 v2. Also new cases in `test_cut_recipe.cpp` and a new `test_cut_groove.cpp` ([CutGroove]).

PR A (paint through cuts):
1. Plane cut of a 20 mm cube with the top painted colour 2: the upper half's top is still 2, the lower half has no colour, and the cut faces are unpainted.
2. The same for support enforcer and blocker, seam enforcer and fuzzy skin (one section per channel).
3. Instance rotated 30 degrees about Z and scaled 1.5, cut with FlipUpper and PlaceOnCutLower: the painted faces are still the ones on the original top. Checked in world space by face centroid and normal.
4. Kerf of 2 mm: halves keep paint, and the band faces are unpainted.
5. Curved sheet (non-flat), draw stroke (loop and open line, from the test_draw_cut fixtures), flexi joint: each keeps the paint on its side.
6. Contour (part selection) cut with a plug and a dowel: parts keep paint, and connector volumes and the dowel object are unpainted.
7. Groove cut (single dovetail): both halves keep paint.
8. With `KeepPaint` off: the halves are byte-identical to today's (vertex and facet hash), and nothing is painted.
9. A MixedFilament state (20) survives the cut.
10. 3MF round trip: save the cut halves with `store_bbs_3mf` and reload. Painting strings and the recipe are intact.
11. Texture-displacement masks after a cut: still empty (pins down D3's current behaviour so a change is deliberate).

PR B (multiple dovetails):
1. Count 1, gap 10: the halves are byte-identical to the pre-port `perform_with_groove` (golden hash taken before the port).
2. Count 3 on an 80 x 20 x 20 box:
   - both halves are watertight (`its_num_open_edges == 0`);
   - upper volume + lower volume = original minus the tolerance band, to 1 %;
   - the tongue count, from connected components of the section at the groove's mid depth, is 3.
3. `calculate_groove_width` against hand-worked values for flaps angle 60/90 and groove angle 0/10.
4. Recipe v5 groove count/gap/radius: XML round trip, cereal round trip, `operator==`.
5. A v4 recipe loads as count 1, and a re-cut from it equals today's halves.
6. A count-3 recipe re-cut from the loaded 3MF reproduces the same halves (extends the existing "re-performed from a loaded recipe" case).
7. [CutMirror]: a count-3 groove mirrored on X is its own inverse.
8. No ModelObject leak per groove (count of live objects before and after, or ASan on the Mac slot).

PR D (if D2 = a or b): a re-edit stand-in rebuilt from a painted cut carries the paint onto the recipe mesh (libslic3r helper, headless).

Undo/redo and the gizmo panel are GUI-only, so they go in the hand test.

## Hand-test list (owner)

Test copy from the PR's `--install`. Projects to use: a painted Benchy (colour + support enforcers + seam), a ring for flexi joints, and the bunny.

PR A:
1. Paint the Benchy's hull in 2 colours, add support enforcers under the bow and a seam line.
   - Cut flat at mid height with Place on cut.
   - Both halves keep colour, enforcers and seam. The cut faces are plain.
   - Slice: colours print where painted.
2. Same with a 1 mm cut thickness, a curved cut, a drawn loop round the funnel, and an open drawn line.
3. Plug connectors and a dowel: the plug and dowel pieces are unpainted.
4. Ctrl+Z after the cut returns the painted whole. Ctrl+Y brings the painted halves back.
5. Save, close, reopen: the paint is still on both halves.
6. Edit cut on a painted cut: behaviour per D2 (paint kept, or a notification saying it is dropped).
7. Cut a model with texture displacement: the texture is dropped as today (per D3).
8. Time a cut of a large painted model (more than 500k triangles) and report it.

PR B:
1. Dovetail mode: Count 3, Gap 10. The preview shows 3 dovetails and Spacing reads gap + width.
   - The cut gives 3 tongues that fit.
   - Gap is greyed out at Count 1.
2. Move one dovetail off the part (large gap): the plane turns red and the cut is refused.
3. Ctrl+Z on Count and Gap changes restores them.
4. Cut, save, reopen, Edit cut: Count and Gap come back and Perform cut gives the same halves.
5. Copy cut to a smaller object: spacing per D5.
6. Mirror X with Count 2.
7. Count 20 on a long part: report the preview lag.
8. Painted part with Count 3: paint kept (PR A in).

## Recommended PR split and order

| PR | Content | Files | Build / gate |
|---|---|---|---|
| **A** `port/orca-od11-paint` | #13472 cut call sites only: `KeepPaint`, finalize remap in all six `perform_*`, gizmo attribute and `perform_by_contour(mo, ...)`, plus the D1 switch. Tests 1-11. | CutUtils.cpp/.hpp, GLGizmoCut.cpp, (Preferences.cpp), tests | libslic3r_tests; app copy for the hand test; G-code gate (expected identical) |
| **B** `port/orca-od11-dovetail` (after A) | #12318, then #13945 (two commits), then Edge commits: recipe v5, undo save/load, leak fix, `std::max`, label strings. Tests B1-B8. | CutUtils, GLGizmoCut.cpp/.hpp, CutRecipe.hpp/.cpp, bbs_3mf.cpp, tests | same |
| **C** (optional) | #13472's other mesh-change sites: Simplify, replace with STL, reload from disk, our repair/remesh (`clear_before_change_mesh` callers). | GLGizmoSimplify, Plater, GUI_ObjectList | libslic3r_tests + hand test |
| **D** (per D2) | Edit cut keeps painting. | GLGizmoCut.cpp `begin_reedit`, CutRecipe (if b), tests | |

- A goes first because B's upstream text assumes it. A is also the cheapest part and the one with user value now.
- A and B do not depend on #605. A shares no hunks with it.
- A against the ImageMap stack (#569): small textual overlap in CutUtils.cpp. Whichever merges second runs `git merge origin/main` and re-runs [CutPaint] and #569's [ImageFill] cases.
- The working branch `port/orca-od11` holds this plan. A and B are cut from it as `port/orca-od11-paint` and `port/orca-od11-dovetail`, or one branch with two PRs stacked. The lead decides.
- Model: Opus for A and B (adapted, risky). A Fable reviewer pass is not needed; it is not a G-code-heavy package.

## Decisions for the owner

- **D1. Keep paint through a cut: always, or behind a preference?**
  - Upstream adds "(Experimental) Keep painted feature after mesh change", default **off**, labelled slow and artifact-prone.
  - #605 keeps paint through boolean and split **always**, with no switch.
  - Recommend **always on for cuts**: consistent with #605, and a cut that drops paint is the bug. Add a busy notice on large meshes.
  - Alternative: the upstream preference, default on.
- **D2. Edit cut on a painted cut.** The stand-in is rebuilt from the recipe's bare mesh, so its paint is lost. Options:
  - (a) Carry the halves' current paint back onto the stand-in at re-edit time. Recipe v5 stores the pre-cut instance's linear transform so the frames line up. This keeps paint added after the cut too. Moderate work.
  - (b) Store the pre-cut paint in the recipe, on the recipe mesh, at cut time. Exact, but larger 3MFs, and paint added to the halves after the cut is lost on re-edit.
  - (c) Keep dropping it, with a notification "Edit cut drops painting".
  - Recommend **(a)**, with (c) as the fallback if the frame spike fails.
- **D3. Texture displacement through a cut.**
  - Masks, layers and UV are dropped today and stay dropped after OD11.
  - Carrying them needs the 8 SecondaryId masks in the saved painting, a copy of `texture_displacement_layers`, and placement rebasing like ImageMap 5c.
  - Recommend a **separate follow-up**, not OD11.
- **D4. Dovetail count cap.**
  - Upstream allows 1-100.
  - Every Count/Gap change re-runs the whole groove cut for the preview (`process_contours`), 7 plane cuts per dovetail.
  - Recommend a **cap of 20**.
- **D5. Copy cut to... with several dovetails.**
  - Recommend keeping the source's spacing in mm (store `plane_radius` in the recipe).
  - The alternative recomputes the spacing from the target's size, so a copied cut changes shape.
  - Re-edit needs the stored radius either way to be identical.
- **D6 (lead).** OD11 PR C (simplify, reload, repair) is optional. Take it now, or after #605 is decided?

## Found while reading (not OD11 scope)

- "Edit cut" builds the stand-in with no object config, no per-part settings and no modifiers (`begin_reedit`: name, recipe mesh, instance transform, connectors only). A re-edited cut loses the object's filament and per-object settings, as well as paint. Worth its own issue.
- `perform_with_groove` upstream leaks a ModelObject per dovetail. Fixed in PR B, and worth reporting upstream.
