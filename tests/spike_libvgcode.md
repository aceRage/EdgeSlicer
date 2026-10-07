# Spike: porting libvgcode (OrcaSlicer #10735) into EdgeSlicer

Date: 2026-10-07. Branch `spike/libvgcode` (worktree `C:\Dev\wt_vgcode_spike`), from `origin/main` 4d3d94d746 (after #358). SPIKE ONLY - never merge.
Context: `tests/orca_port_plan.md` 6C row "libvgcode / new G-code viewer" and section 11 "libvgcode-dependent viewer work"; `tests/orca_pr_audit_2025Q4_2026Q1.md` row 10735.

## 1. TL;DR

* #10735 is not "a viewer". It is four things in one squash: (a) the libvgcode library, (b) a **GCodeProcessor rework** (planner fixes, per-move times, actual-speed vertices, PrusaSlicer G2/G3 discretisation replacing BBS arc interpolation, removal of the role/move/layer time tables), (c) an **OpenGL core-profile migration** (VAOs in GLModel/ImGui/painter gizmos, core context selection, new geometry-shader dashed lines), and (d) a rewrite of GCodeViewer / GLCanvas3D preview code on top of (a).
* libvgcode needs **OpenGL 3.2 core (GLSL 150, texture buffer objects, instancing) or GLES 3.0**. It has **no fallback**: with an older context the preview is empty plus an error dialog. Our app asks for a *compatibility* profile everywhere (since #351); on **macOS that is a GL 2.1 context**, so libvgcode cannot run on our Mac builds until the app moves to a core profile there. Windows/Linux compatibility contexts are normally 4.x and would work, except low-end Mesa/VM drivers that cap compatibility at 3.0.
* A 3-way apply of the whole PR onto our tree: **101 files apply cleanly, 16 files conflict (75 hunks, ~5,000 conflict lines)**. The conflicts concentrate in GCodeViewer (28 hunks, ~2,700 lines), GCodeProcessor (20 hunks, ~1,250 lines) and GLCanvas3D (10 hunks, ~720 lines). Most conflicts are with **upstream changes we never took** (BBS H2D filament groups, `register_commands` dispatch, etc.), not with Edge features.
* Our own viewer customisation is smaller than feared: our GCodeViewer.cpp differs from upstream's pre-#10735 file by ~350 added / ~530 removed lines. The Edge features that must be re-ported onto the new viewer are listed in section 7 (cost breakdown, over-support roles, phone preview capture, OOM guard, layer times, H2C first-layer time, effective-filament preview, colour fixes).
* The spike got to a **compiling, tested state** for the cheap half: libvgcode (vendored from upstream main, with the shared GLAD) builds and links into `libslic3r_gui`, an adapter feeds it from **our** `GCodeProcessorResult`, and our processor got two additive per-move fields (tag-based `layer_id`, per-mode `times`). Unit tests prove roles, layers, arcs and times come through (section 6).
* Recommended: a **staged port** (processor first, then core profile, then viewer swap), about **17-25 agent-days** plus owner hand-testing on Windows and the Mac. It should sit **right after Batch 4A and before 2F** (2F's upstream diffs are written against the post-#10735 time machine). Details in sections 9-11.

## 2. Upstream commit range and files

| Item | Value |
|---|---|
| PR | OrcaSlicer #10735 "Port libvgcode/improved G-code viewer from PrusaSlicer 2.8.0" (as-com), merged 2026-01-06 |
| Merge commit | `ba5f0e707d355c1d34a3f1f7a3945ea1a9fb660e` (already in our object DB, so no extra fetch was needed) |
| First parent (upstream before) | `eb72dce9aac676331a2d9377171b4f21dbac1d78` |
| PR head | `3b4600d65ba65bad8fae2821a97b4f7d18c98328` (38 commits incl. 9 merges of main; the PrusaSlicer port itself is `c92328c9cc`) |
| Size | 117 files, +19,383 / -6,223 |
| Patch used | `git diff --binary --full-index eb72dce9 ba5f0e707d` (29,450 lines); 3-way applied with `git apply --3way` because the preimage blobs are in our object DB, so every file got a true 3-way merge (base = upstream pre-file, ours = Edge, theirs = upstream post-file) |

By area (lines from the PR diffstat):

| Area | Files | + / - | Notes |
|---|---|---|---|
| `src/libvgcode` (library) | 41 | +6,524 | self-contained; AGPL-3.0-or-later (Prusa Research) |
| `src/libvgcode/glad` (GL + GLES loaders) | 5 | +8,255 | **drop the desktop GL part**: upstream #13197 later made libvgcode use the shared `src/glad`, which our #351 already vendored; a second `gl.c` would clash at link time |
| `GUI/LibVGCode/LibVGCodeWrapper` | 2 | +897 | result -> libvgcode conversion, and Print -> libvgcode for the sliced-not-exported preview |
| GCodeViewer | 2 | +1,592 / -4,206 | rewrite onto libvgcode |
| GLCanvas3D, GUI_Preview, IMSlider, Plater | 7 | +164 / -954 | old `_load_print_toolpaths` / legend texture / calibration thumbnail removed |
| libslic3r: GCodeProcessor, GCodeReader, Geometry/ArcWelder (new), WipeTower, GCode, BuildVolume, Technologies, utils | 14 | +854 / -938 | the processor rework |
| GL core profile + misc GUI (GLModel VAOs, ImGuiWrapper, painter/move/scale/rotate/measure/cut gizmos, GLSelectionRectangle, PartPlate, Selection, 3DScene, OpenGLManager, GUI_Init, shaders 140, new `hotbed` and `dashed_thick_lines` (geometry shader)) | 42 | +1,086 / -124 | the core-profile migration |

Prerequisites: none in the PR sense (it was merged straight onto main), but it assumes the upstream tree of 2026-01-06. Things that tree had and we do not, which show up as conflicts or compile errors: BBS H2D filament-group UI in the viewer (`FilamentGroupPopup`, left/right extruder filament lists, `render_legend_color_arr_recommen`), `extruder_areas` / `wrapping_exclude_area` in `set_bed_shape`, the `register_commands()` handler table in GCodeProcessor, per-filament `m_remaining_volume` vectors, `GCodeCheckResult` / `filament_printable_reuslt`, the "Summary" view type, and the Dec-2025 legacy-viewer fixes #11397 / #11734 (default view type). We do not need to port these first: taking the upstream viewer file brings the viewer-side ones along, and the processor-side ones are resolved by hand.

## 3. Dependent upstream PRs (the chain), in merge order

Libvgcode-dependent work merged after #10735 (from the GitHub commit history of `src/libvgcode`, `GUI/LibVGCode`, `GCodeViewer.*`):

| Date | PR | What | Touches |
|---|---|---|---|
| 01-08 | 11848 | speed / flow interpolation fix | viewer |
| 01-09 | 11881 | G-code marker QoL | processor + libvgcode + viewer |
| 01-09 | 11871 | G-code window padding | viewer |
| 01-16 | 11912 | black colour preview | wrapper |
| 01-16 | 11968 | legend layer/time for pause/custom entries | viewer |
| 01-20 | 12014 | title vs body data | viewer |
| 02-04 | 12068 | tool position window rework | viewer, imconfig |
| 02-09 | 12195 | tool position window glitch (Linux) | viewer |
| 02-21 | **11673** | Pressure Advance visualisation | processor + libvgcode + viewer + wrapper |
| 02-23..03-02 | 12403, 12499, 12567 | unit strings, capitalisation, degree sign | viewer |
| 03-10 | 12364, 12711 | hidden line type (Linux); actual-speed plot duplicates | viewer |
| 04-13 | 13197 (part) | Wayland: libvgcode on the shared GLAD | libvgcode CMake / OpenGLUtils |
| 04-19 | 13233 | seams glitch (Linux) | libvgcode |
| 04-24 | **13169** | kinematics (jerk / accel) view | processor + libvgcode + viewer (needs 2F per the plan) |
| 04-24 | 12474 | tool-change count after slicing | viewer |
| 05-05..05-14 | 13365, 12614, 12707, 11899 | widget sizes, tool position, speed profile window, legend hit area | viewer |
| 05-21 | 13681 | line-type distances / amounts | GCode.cpp + processor + viewer |
| 06-05 | 14042 (part) | divided filament (6C; overlaps our MixedFilament) | wrapper |
| 06-08 | 14103 (part) | crash on printer switch | viewer |
| 06-16 | 13625 | persist view mode | viewer |
| 07-20, 07-29 | **14705, 15001** | dim lower layers (+ improve) | libvgcode + viewer + prefs |
| 07-22 | 14613 (part) | accel/jerk state after custom G-code | wrapper |
| 08-23, 09-06 | 15326, 15448 | move length; comment text / non-ASCII lines | viewer |
| 09-18 | 15674 | GPU load of mouse moves (Batch 4B) | 30 files |
| 09-22 | 15809, 15769 | preview colours (+ SSAO tie-in); default view type | libvgcode + viewer + GLCanvas3D |
| 09-23, 09-28 | **15833, 15883** | cached sums + index buffer; top-down draw order (perf) | libvgcode + viewer |
| 09-28 | 15360 | clipped tall models in G-code viewer mode | viewer |
| 10-02 | 15879 | section view (6B, needs painter gizmos) | 31 files incl. libvgcode |
| 10-02 | 15884 | faster preview view (our #254/#260 took Stages A/B only) | 37 files incl. libvgcode |
| 10-05 | 15074 and include clean-ups 16048/16099/16106 | UI fixes | viewer |

Already merged upstream before #10735 and folded into its base: #11397 and #11734 (legacy-viewer default view type). They come along with the upstream viewer file.

Upstream time-estimate PRs in Batch 2F that are written against the **post-#10735** time machine (they call `calculate_time(result, mode, ...)`, use `block.move_id`, `first_layer_time`, `actual_speed_moves`): **#14573** (14 such lines), #15304, #15308. #12417 / #12440 (junction deviation) and Batch 4A's #14166 (static regex) do not depend on it.

## 4. What the 3-way apply measured

`git apply --3way` of the full PR onto `origin/main`:

| File | Conflict hunks | Conflict lines | Nature |
|---|---|---|---|
| GUI/GCodeViewer.cpp | 19 | 2,352 | rewrite vs our edits; treat as "take theirs, re-port ours" |
| GUI/GCodeViewer.hpp | 9 | 385 | same |
| GCode/GCodeProcessor.cpp | 16 | 1,143 | see section 5 |
| GCode/GCodeProcessor.hpp | 4 | 113 | MoveVertex fields; Edge's H2C `initial_layer_time` + layer order; `GCodeCheckResult` |
| GUI/GLCanvas3D.cpp | 7 | 666 | PR deletes `_load_print_toolpaths` / `_load_print_object_toolpaths` / `_load_wipe_tower_toolpaths` (573 lines, which carry Edge's effective-filament fix), the legend texture and the drag struct |
| GUI/GLCanvas3D.hpp | 3 | 51 | Edge sculpt API next to `reset_volumes()`; `load_gcode_preview` signature |
| GUI/OpenGLManager.cpp | 2 | 104 | ours = GLAD + explicit compatibility profile; theirs = GLEW + core-profile search |
| GUI/Plater.cpp | 5 | 85 | `reload_print` signature, load-files flow |
| GCode.cpp / GCode.hpp | 1 / 1 | 20 / 10 | `init_gcode_processor` |
| WipeTower.cpp, GCodeReader.hpp | 1 / 1 | 13 / 5 | trivial |
| slic3r/CMakeLists.txt | 1 | 14 | trivial |
| 3DScene.cpp, GLShadersManager.cpp, GUI_Preview.cpp | 1 / 2 / 2 | 11 / 23 / 13 | trivial to small |
| **All others (101 files incl. all gizmo / GLModel / ImGui / PartPlate core-profile fixes)** | 0 | 0 | applied cleanly |

Why the viewer conflict is less scary than its line count: diffing our GCodeViewer.cpp against upstream's **pre**-#10735 file (CR-insensitive) gives only +347 / -532 lines in 56 hunks; about half of that is upstream work we never took (H2D filament groups, Summary view, bed areas), the rest is the Edge feature set in section 7. So the honest procedure is: take upstream's post file (or a later snapshot, section 9), then re-apply the Edge delta.

## 5. How our GCodeProcessor result feeds libvgcode

libvgcode's per-vertex input (`PathVertex`) needs: position, height, width, feedrate, **actual_feedrate**, mm3_per_mm, fan speed, temperature, role, type, gcode_id, **layer_id**, extruder_id, colour id, **times[Normal, Stealth]** (this move's own duration), and since #11673 / #13169 also pressure advance, acceleration, jerk.

| Need | Ours today | Gap and fix |
|---|---|---|
| layer_id per move | `MoveVertex::layer_duration` holds the layer id until `finalize()` overwrites it with the layer duration | **Spike: added `MoveVertex::layer_id`** (0-based, tag-based, as upstream stores it). Tag-based matters for ContourZ: moves whose Z rides the contour stay in their layer (the legacy viewer appends a new "layer" whenever an extrusion's Z changes, so with ContourZ on it most likely over-counts layers; not verified in the app) |
| per-move durations | `MoveVertex::time` is a placeholder (the move index); only elapsed times per G1 line exist (`g1_times_cache`) | **Spike: added `TimeBlock::move_id` + `MoveVertex::times`**, filled in `finalize()` from the processed blocks. Additive: no total, M73 line or G-code byte changes |
| actual (planner) speed | none | upstream computes it in `TimeMachine::calculate_time` and inserts extra vertices at accel/decel points (`actual_speed_moves`). Spike uses the commanded speed (Actual-speed views would equal Speed views). Real port: take upstream's code |
| arcs | BBS keeps G2/G3 as one move with `interpolation_points` | upstream replaced it with PrusaSlicer's discretisation into internal G1 vertices (new `Geometry/ArcWelder`). Spike expands the interpolation points in the adapter and splits the move's time by segment length |
| spiral vase | `spiral_vase_layers` (BBS) | libvgcode only needs a flag; spike derives it |
| roles | Edge adds `erOverSupportPerimeter` and `erBottomSurfaceOverSupport` mid-enum | appended two roles to libvgcode's enum (it asks for additions at the end), with the legacy viewer's colours |
| first move | upstream's processor stores a dummy move 0 (`initialize_result_moves`) and its wrapper starts at 1 | ours stores real moves from 0; the adapter treats move 0 as the start point only |
| role / move / layer time tables | `roles_times`, `moves_times`, `layers_times` used by our legend (time per feature), IMSlider (`SetLayersTimes`) and **Edge's H2C slice_info `initial_layer_time`** (first layer time minus the `erCustom` time) | upstream drops them (libvgcode computes them from per-move times) and stores `first_layer_time`. Keep them during the transition; the H2C value must keep Bambu's semantics (HMS 05004046 lesson) |

Edge-local processor features that the processor rework must not break (all in conflicting regions or the same functions): machine prepare time in estimates (#5796 port), booked waits / Bambu tool-change times and idle-nozzle pre-cooling (post-process builder over `g1_times_cache`), Klipper accel early-out and AdaptivePA skip (#16031 port), flow-variant reads (U1 High-Flow), H2C toolchange counting and 3MF schema times, cost breakdown inputs (`time_cost`, filament costs; #358), gcode-import filament view (#66), reserved-keyword check by printer kind (#14908 port).

Behaviour changes the processor part of #10735 brings, which an identity gate would show: planner passes reordered to Marlin's (reverse then forward, new kernels, SPE-2397), first block keeps its entry speed after a queue flush (SPE-2441), total time accumulated in double, PrusaSlicer arc timing. Expect **G-code identical apart from M73 / estimated-time lines**; record the time deltas on the X1C and U1 cubes.

## 6. What the spike branch contains and what it proved

Commits on `spike/libvgcode` (see `git log origin/main..spike/libvgcode`):

* `src/libvgcode/` vendored from **upstream main** (`orcaupstream/main` 78f74a6276), i.e. #10735 plus every later libvgcode change (11673, 11881, 13169, 13197 shared GLAD, 13233, 14705, 15001, 15809, 15833, 15879, 15883, 15884). Only the GLES2 loader remains under `glad/` (ES builds only); desktop GL links our `src/glad`. Two Edge roles appended (`Types.hpp`, `Settings.hpp`, `Viewer.hpp`, `ViewerImpl.hpp/.cpp`, marked `// EDGE`). File headers (Prusa Research, AGPL-3.0-or-later) kept.
* `src/CMakeLists.txt`: `add_subdirectory(libvgcode)` inside `SLIC3R_GUI`; `src/slic3r/CMakeLists.txt`: `libslic3r_gui` links `libvgcode`.
* `GCodeProcessor.hpp/.cpp`: additive `MoveVertex::layer_id`, `MoveVertex::times`, `TimeBlock::move_id`, `TimeMachine::move_durations`; durations distributed in `finalize()` (a duration taken before a seam vertex was stored moves on to the real move).
* `src/slic3r/GUI/LibVGCode/EdgeVGCodeInput.{hpp,cpp}`: our result -> `libvgcode::GCodeInputData` (roles incl. Edge's, move types, phantom path-start vertices as upstream emits them, arc expansion, times, palettes with upstream's dark-colour floor).
* `tests/slic3rutils/libvgcode_input_tests.cpp`: processes a two-layer G-code (Bambu-style tags, an arc, Edge roles, moves riding above the layer like ContourZ) with our real `GCodeProcessor`, converts it, and checks roles, layer ids, monotonic layers, arc expansion and that the per-move times add up to the processor's estimate. A hidden `[libvgcode_bench]` case converts a 2-million-move synthetic result.
* The legacy GCodeViewer still renders; nothing user-visible changes.

Build and test results (standard agent build, Ninja + unity + PCH, MSVC, Windows):

* Clean build of the app, `libslic3r_tests`, `slic3rutils_tests`: **BUILD_EXIT=0** (libvgcode compiles warning-free in the unity build; no GLAD symbol clash). `slic3rutils_tests`: all 782 test cases / 9,686 assertions pass, including the new one (80 assertions).
* The two-layer G-code: 12 processor moves become 69 libvgcode vertices (9 path-start vertices, 49 from the arc's interpolation points); extrusions span exactly layer ids {0, 1} although their Z takes four values; both Edge roles arrive as their own libvgcode roles; **sum of per-move durations = 14.6907 s = the processor's own estimate** (the additive `times` field is exact, so libvgcode's layer and cumulative times would match our estimate).
* Benchmark (hidden case, Release): 2,000,000 moves -> 2,439,998 vertices in **109-136 ms**. `sizeof(MoveVertex)` = 120 B (ours, with the BBS arc vector), `sizeof(PathVertex)` = 80 B, so the CPU copy is ~195 MB for a 2M-move file. GPU side, libvgcode keeps ~40 B per vertex (vec4 position + vec4 height/width/angle + 4 B colour + 4 B index, from the texture-buffer uploads in `ViewerImpl.cpp`) = **~98 MB**, against the legacy viewer's up to 8 vertices x 24 B + 30 indices x 4 B = **~312 B per extrusion segment (~620 MB)** plus its CPU-side path tables. That is the memory headroom behind the "much faster preview" claim and why our OOM guard (#642) could be relaxed. Frame-time gains could not be measured without a GL context in tests and without desktop control; upstream's PR and #15833 / #15883 / #15884 report them.
* After the processor change (incremental rebuild incl. `fff_print_tests`): `libslic3r_tests` 1,804 cases, all pass (3 expected failures), `fff_print_tests` 303 cases / 44,808 assertions all pass, `slic3rutils_tests` all pass. The additive fields change no existing result.

## 7. EdgeSlicer viewer features to re-port onto the new viewer

| Feature | Where today | On libvgcode | Effort |
|---|---|---|---|
| Cost breakdown (#358): `render_cost_section`, per-plate and all-plates cost, "machine time only" | GCodeViewer legend + `render_all_plates_stats`; `compute_cost` in libslic3r | legend stays ImGui in the new viewer; move the section and its three call sites | M (1 d) |
| Over-support roles (colours, visibility flags) | `Extrusion_Role_Colors`, role flags | **done in spike** (libvgcode enum + colours); legend rows need the names | S |
| Phone / remote preview (RemoteAccess -> `GLCanvas3D::render_gcode_preview_image`, `GCodeViewer::render_toolpaths_for_capture`, `loaded_result_id`, `get_layers_zs`, `set_layers_z_range`, ortho side views, layer slider) | Edge-only API on the legacy viewer | re-implement capture with `libvgcode::Viewer::render(view, projection)` into our FBO; layer API maps to libvgcode `get_layers_view_range` / `set_layers_view_range` | M (1-1.5 d) + phone check |
| OOM guard `skip_toolpaths` (#642) + `extract_layer_metadata` | legacy-only (GPU buffers ~300 B per segment) | libvgcode needs ~40 B per vertex on the GPU; keep a guard with a much higher threshold or drop it | S-M |
| Layer times on the layer slider and in the LayerTime views | `layers_times` / `layer_duration` | from libvgcode's per-layer times (upstream wiring) | S (comes with upstream files) |
| H2C slice_info `initial_layer_time` | `layers_times[0] - erCustom time` | keep a processor-side value with the same meaning; add a regression test against a Bambu Studio reference | S, high importance |
| Sliced-not-exported preview with effective filaments (Edge fix in `_load_print_object_toolpaths`) | GLCanvas3D | moves into `LibVGCodeWrapper::convert(Print, ...)` (`convert_object_to_vertices`) | S |
| G-code-only filament colours (Snapmaker #875 port), import filament view (#66), MixedFilament / Image Row colours | GUI_Preview colour lists, viewer tool colours | palettes passed to `convert()`; check virtual (mixed) filament ids map to a colour | S-M |
| Tool / colour visibility toggles in the legend | legacy viewer | **not supported by libvgcode** (upstream lists it as removed functionality) | accept or patch libvgcode (M) |
| Calibration thumbnail (`render_calibration_thumbnail`) | GCodeViewer + CLI | dead in both our GUI and CLI (commented out); upstream deletes it | none |
| Render timings / depth sort (#254, #260) | GLCanvas3D / 3DScene | unaffected (model view); FrameProfiler can wrap the libvgcode pass | S |

## 8. OpenGL and macOS implications

* libvgcode: `OpenGLWrapper::load_opengl()` accepts GL >= 3.2 (desktop) or GLES >= 3.0, shaders are `#version 150` with `samplerBuffer`/`usamplerBuffer` + `texelFetch` and instanced draws. No legacy path. Upstream shows an error dialog and an empty preview otherwise.
* Our context: since #351 `OpenGLManager::init_glcontext` requests `CompatibilityProfile()` on every platform, then the default context. On **macOS that is GL 2.1** (Apple only offers 3.2+ as forward-compatible core), so we run the 110 shaders there (#336 fixed their sampler units). **libvgcode cannot run on our Mac builds with today's context.**
* Upstream's answer (inside #10735): search core profiles from 4.6 down to 3.2 (forward-compatible), fall back to compatibility only on request (`--opengl-compatibility`) or to the default context. That makes macOS 4.1 core.
* Moving to core means the rest of the renderer must be core-clean. #10735's own fixes for that applied cleanly here (VAOs in GLModel, ImGuiWrapper and painter gizmos; dashed lines via a geometry shader instead of `glLineStipple`; PartPlate legacy draws). Our remaining direct `glDraw*` sites are in the same files as upstream's (3DBed, GLModel, painter/MMU gizmos, ImGuiWrapper, PartPlate) plus the legacy GCodeViewer, which goes away. Edge-only code uses GLModel; Edge's draw-cut uses GL_LUMINANCE only on its 2.1 fallback path. `glLineWidth > 1` (24 sites, upstream has 23) silently clamps to 1 px on macOS core: thinner gizmo lines on Mac, as upstream Orca shows.
* Windows / Linux: a core context is what upstream ships. Risk is old Intel iGPUs and VMs: compatibility-profile Mesa often stops at 3.0, which would also fail libvgcode, while core 3.3+ is available on the same drivers, so core is the better default there too. Keep a compatibility escape hatch (CLI flag / env) and our GLAD loader (#351 already uses `gladLoaderLoadGL`, which works with core).
* Caveat found: libvgcode's `shutdown()` calls `gladLoaderUnloadGL()`, which with the shared GLAD unloads the app's GL pointers too. Harmless at exit (upstream lives with it) but the real port should make libvgcode's unload a no-op when it shares the loader.
* Option that de-risks staging: bind one global VAO right after context creation so legacy VBO draws (the old viewer) stay valid in a core context. That lets the core-profile switch ship before the viewer swap.

## 9. Recommended strategy (staged)

Snapshot rather than replay: vendor `src/libvgcode` from upstream main (self-contained, every later fix included, as the spike does), and take the viewer host files (GCodeViewer, LibVGCodeWrapper, GUI_Preview / IMSlider bits) from an upstream commit **before #15674 / #15809** (for example `c833ccdf6f`, 2026-09-17), because 15674 (Batch 4B), 15809 (SSAO tie-in) and 15879 / 15884 (section view, realistic-view shadows) pull 6B/6C GLCanvas3D features into the host. Their libvgcode halves are already in the vendored library; the host halves can follow later as their own small PRs.

| Stage | Content | Gate | Effort |
|---|---|---|---|
| 0 (this spike) | libvgcode builds and links; adapter + additive processor fields + tests | builds, tests | done |
| 1 | **GCodeProcessor part of #10735**: Marlin planner kernels, first-block entry fix, double accumulation, per-move times, actual-speed vertices, PrusaSlicer arc discretisation (+ `Geometry/ArcWelder`), `first_layer_time`, dummy move 0, spiral flag. Keep `roles_times` / `moves_times` / `layers_times` and BBS arc interpolation data until stage 3 so the legacy viewer keeps working. Port every Edge processor feature listed in section 5 | identity gate: G-code equal except M73 / estimate lines; record time deltas; H2C slice_info `initial_layer_time` unchanged on an H2C file; fff_print + libslic3r tests | 4-5 d |
| 2 | **Core profile**: upstream's context search + compatibility escape hatch, the PR's core fixes (GLModel VAOs, ImGui, gizmos, PartPlate, shader tweaks, `hotbed`, `dashed_thick_lines`), a global VAO so the legacy viewer survives, audit Edge render code (sculpt, draw-cut, bevel, selection outline/composite, bed texture logo, live theme) | Windows NVIDIA/AMD/Intel, a Linux Mesa box or VM, **owner Mac session** (fold into Batch 5D) | 3-4 d + Mac time |
| 3 | **Viewer swap**: GCodeViewer / GLCanvas3D preview / GUI_Preview / IMSlider / Plater from the chosen snapshot, wrapper on our result (now stage-1 shaped), re-port section 7, drop legacy tables and BBS arc data | owner hand-test of preview on X1C / H2C / U1 / multi-colour / vase / arc-fitted / ContourZ files; phone preview; G-code viewer mode | 7-10 d |
| 4 | Remaining host halves: 11673 PA view (needs PA tags in the processor), 13681, 14705 + 15001 dim layers, 15769, 15833 / 15883 host bits, Linux fixes; 13169 after 2F; 15879 / 15884 / 15809 with 6B/6C | per PR | 3-5 d |

Realistic estimate: **17-25 agent-days** for stages 1-4 (the plan's 25-40 d assumed replaying the PR; the snapshot approach and the small Edge viewer delta cut it), plus 2-3 owner hand-test sessions (Windows preview, Mac, phone). Risk stays High because stage 1 changes every time estimate and stage 2 changes every frame's GL state.

## 10. Where it should sit in `tests/orca_port_plan.md`

* **Stage 1 right after Batch 4A** (step 5 of section 12; needs #351 merged) and **before 2F**: #14573 / #15304 / #15308 are written against the post-#10735 `calculate_time(result, mode, ...)` with `move_id`; porting 2F first means adapting them to the old time machine and then redoing the merge. 4A (#14166 static regex, #16028 remainder) does not overlap and is smaller, so it goes first. Re-baseline 2F's time estimates after stage 1, not before.
* **Stage 2 alongside Batch 1C / 5D** (the Mac pack): one Mac hand-test session covers both.
* **Stage 3 before** the 6B "Preview extras" row (13681, 13919, 15769, 14705 + 15001) and before X-Ray #15770 and Section view #15879, which all assume the new viewer. Batch 1B's GCodeViewer part of #14103 should wait for stage 3.
* Move "libvgcode-dependent viewer work" out of section 11 ("Not applicable") into a stage-4 list.

## 11. Licence and attribution

libvgcode is "released under the terms of the AGPLv3 or higher" (Prusa Research, Enrico Turri, Pavel Mikuš, Vojtěch Bubník); LibVGCodeWrapper carries the PrusaSlicer AGPLv3 header. EdgeSlicer is AGPL-3.0 (`LICENSE.txt`), so it is compatible. Keep every file header unchanged, mark Edge edits inside libvgcode with `// EDGE`, and in the real port add libvgcode (PrusaSlicer 2.8, via OrcaSlicer #10735) to the README credits / third-party list. No new external dependency: the shared GLAD is MIT-licensed generated code already in `src/glad`.

## 12. Housekeeping

Worktree `C:\Dev\wt_vgcode_spike` kept (useful for stage 1); its `build_ninja` deleted after the run. Scratch files (patch, 3-way log, extracted upstream files) in `%TEMP%\vgcode_scratch`. Nothing under `%APPDATA%` touched, no printers contacted, EdgeSlicer.exe not stopped.
