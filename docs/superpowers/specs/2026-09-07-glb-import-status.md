# glTF 2.0 / GLB import — integration status

Date: 2026-09-07 · Branch: `feat/glb-import` · Integrating: `feat/glb-import-stage1..4`
Base: `feat/ultra-preferences` @ `526ed0afdc` (the v2.3.6.5-edge release head)

This is the assessment the integration asked for: what the four stage branches actually contain,
what each claimed versus delivered, how they are tested, and what they collide with on the current
head. The implementation plan and every stage's own status section live in
`docs/superpowers/plans/2026-09-02-glb-import.md`; the research that preceded them is
`docs/superpowers/specs/2026-09-02-glb-import-research.md`. This file does not repeat them — it
says what a reviewer needs to decide whether the stack merges.

## 1. The stack

The four branches are strictly stacked, confirmed with `git merge-base --is-ancestor`:

```
feat/glb-import-stage1  5 commits   base d6e020124a  (an ancestor of 526ed0afdc)
feat/glb-import-stage2  8           = stage1 + 3
feat/glb-import-stage3 13           = stage2 + 5
feat/glb-import-stage4 16           = stage3 + 3
```

So the stage-4 tip is the whole feature; nothing was left behind on an earlier branch. All 16
commits are dated 2026-09-03 and none of them has been touched since.

`feat/glb-import` is those 16 commits rebased onto `526ed0afdc`, in order, with no conflicts and no
edits — `git rebase --onto` replayed all 16 clean.

## 2. Conflicts with the integration head

`git merge-tree --write-tree 526ed0afdc <stage>` for each stage:

| Stage | Result |
|---|---|
| stage1 | clean — tree `d1a5f30e38` |
| stage2 | clean — tree `28b0bbc27a` |
| stage3 | clean — tree `af01695355` |
| stage4 | clean — tree `fcb16c6a79` |

**No conflicts at all**, at any stage. The reason is that the feature is almost entirely additive:
it touches 68 files and 51 of them are new. The 17 pre-existing files it edits are three
`CMakeLists.txt`, the plan document, `Model.{cpp,hpp}`,
`slic3r/GUI/{GUI_App,MainFrame,Plater,ObjColorDialog.{cpp,hpp},RemoteAccess.{cpp,hpp},RemoteHub}`,
`resources/web/orca/stream_center.html` and `src/dev-utils/platform/osx/Info.plist.in` — and every
edit is a small insertion into a list or a dispatch chain. The one substantial edit — moving two
loop bodies out of `Model::obj_import_*` into file-static helpers — is in a region nothing on
`feat/ultra-preferences` has touched since.

Since the rebase the branch has been rebuilt and re-tested from scratch (section 5), so "no textual
conflict" is backed by a build, not only by `merge-tree`.

## 3. What the stages implement

### Format support

| Capability | State |
|---|---|
| `.glb` (binary, GLB container) | yes |
| `.gltf` (JSON) with an external `.bin` | yes, with a buffer-URI containment check (`escaping_buffer.gltf`) |
| `.gltf` with embedded `data:` buffers | yes (cgltf handles base64 URIs) |
| Meshes: TRIANGLES, TRIANGLE_STRIP, TRIANGLE_FAN | yes — strips and fans are de-indexed, with the odd-triangle winding swap |
| Indexed and non-indexed primitives | yes |
| Sparse accessors | yes, and refused when the sparse views have no buffer (a real crash, found in stage 3) |
| POINTS / LINES primitives | dropped, and refused by name if that is all there is |
| Node hierarchy and TRS transforms | yes — `cgltf_node_transform_world` composes the chain, and the matrix is **baked into the mesh**; the volume keeps identity rotation and unit scale |
| Multiple nodes referencing one mesh | yes — each reference becomes its own volume |
| Up axis | glTF Y-up → Slic3r Z-up, `(x,y,z) → (x,-z,y)` |
| Units | 1 glTF unit = 1 mm, plus the existing `looks_like_saved_in_meters()` rescue prompt |
| Multi-object | **one file = one `ModelObject`**, one `ModelVolume` per drawn primitive. This is a deliberate plan decision, not a limitation that was missed |
| Names | object from the glTF scene name (else the file stem); volumes from the node/mesh name, suffixed `_2`, `_3`… for extra primitives |
| Materials: `baseColorFactor` | yes → per-part filament assignment, sRGB-encoded |
| `COLOR_0` vertex colours | yes → MMU painting |
| `baseColorTexture` | yes (stage 3d) → per-face colours sampled at the triangle centroid, then MMU painting |
| `KHR_materials_*`, PBR beyond base colour | not read — out of scope by design |
| `KHR_mesh_quantization` | yes (no code needed; covered by a fixture) |
| `EXT_meshopt_compression` | yes — vendored decoder-only meshoptimizer v1.2 |
| `KHR_draco_mesh_compression` | **refused by name**, deliberately. See §4 |
| `KHR_texture_basisu` / KTX2 | refused by name when required; dropped with a warning when merely used |
| Animation, skinning, cameras, lights | ignored — out of scope |
| Writing glTF | not in scope |

The library is **cgltf 1.15** (MIT), vendored into `deps_src/cgltf/`, plus decoder-only
**meshoptimizer 1.2** in `deps_src/meshoptimizer/`. Neither touches the prebuilt `deps/` tree, so
nobody has to rebuild dependencies — that was the point of choosing cgltf over Assimp.

### Registration points

Everything a user can reach the format through:

* File ▸ Import — `MainFrame.cpp:2695,2701`, both `#ifdef` branches, label
  `Import 3MF/STL/STEP/SVG/OBJ/AMF/GLB`.
* The import dialog's filter and title — `GUI_App.cpp:680,684` (`FT_MODEL`, both lists) and
  `:4397,4399` ("Choose one or more files (3mf/step/stl/svg/obj/amf/glb…)").
* Drag and drop — `Plater.cpp` `pattern_drop`.
* The `.obj`-only colour-import guards, widened to `.obj|.glb|.gltf` — `Plater.cpp:12321` (import)
  and `:14826` (reload from disk).
* The assembly-position guard at `Plater.cpp:11730` already listed `.glb`/`.gltf` and simply becomes
  live.
* Phone upload — `RemoteHub.cpp:1091` (`spool_upload` allow-list) and the API manifest at `:2555`,
  with the client-side picker in `resources/web/orca/stream_center.html:1513`.
* macOS document type — `src/dev-utils/platform/osx/Info.plist.in`.
* CLI — **no change was needed and none was made**: `Snapmaker_Orca.cpp:1747` hands every
  non-3MF file to `Model::read_from_file`, which now has a `.glb`/`.gltf` branch
  (`Model.cpp:326`). Verified by running it (§5).

**Windows file associations: not registered, and that is consistent.** `GUI_App.cpp` only
associates `3mf`, `stl`, `step`/`stp` and `gcode`; `.obj`, `.amf` and `.svg` are *not* associated
either, even though they are fully supported import formats. Adding `glb` would mean adding a new
`associate_glb` preference and a Preferences checkbox for it — new scope, and it would leave `.obj`
as the odd one out. Recommendation: leave it, and if associations are ever widened, widen them for
`obj`, `amf` and `glb` together. This is the one deliverable in the brief that is deliberately not
done; it is a product decision, not an omission.

### Colour import

* `Model.cpp:326-373` dispatches, in priority order: `COLOR_0` vertex colours → per-face texture
  colours → per-material colours. Each hands its colour list to the existing `ObjImportColorFn`
  callback (the `ObjColorDialog` machinery the OBJ importer already used), and applies the answer
  through one of three appliers in `Model` (`import_multi_volume_vertex_color_deal`,
  `import_multi_volume_face_color_deal`, `import_volume_color_deal`).
* A single-material glTF opens **no** dialog — stricter than OBJ, deliberately (nothing to choose).
* A file whose texture could not be sampled opens no dialog and returns a warning through the new
  `Model::read_from_file(..., std::string *import_warning)` out-parameter — added because
  `read_from_file` previously read `message` only on failure, so a success-with-a-warning had
  nowhere to go.
* Stage 4 adds the headless case: `src/libslic3r/ObjColorMatch.{hpp,cpp}` (pure, wx-free:
  k-means clustering + a slot-mapping policy) and `obj_color_auto_match_headless()` beside the
  panel. When `RemoteAccess::dialog_mode() != Interactive` — i.e. a phone import into a hidden
  instance — the colours are matched automatically instead of a dialog being auto-answered into
  nothing. Interactive mode is byte-for-byte the old path.
* The policy: reuse a loaded spool within CIE76 ΔE ≤ 20; else add a slot if fewer than 16 exist;
  else merge into the nearest. Busiest clusters get first claim. The result is reported as `colors`
  in the phone API response via `RemoteAccess::note_color_import` / `take_color_import`.
* The OBJ path gets the same headless treatment as a side effect, because OBJ and glTF arrive
  through the same lambda. The OBJ *interactive* path is unchanged, and two `[golden]` tests pin its
  exact 3MF/AMF serialisation so that stays true.

## 4. Claimed vs. delivered

Every stage wrote its own status section into the plan, and those sections are unusually honest —
each lists its deviations rather than claiming the plan was followed. Cross-checking them against
the code on this branch:

**Stage 1 (geometry) — claimed complete, is complete.** All 15 plan items are present at the
file:line the status names. Ten deviations are declared; the load-bearing ones are (a) refusing
`cgltf_result_data_too_short` from `cgltf_validate` rather than only logging it, because that is the
only bounds check on sparse accessor indices in a parser reachable from the LAN upload endpoint —
this is a security fix disguised as a deviation, and it is the right call; and (b) an extra
`its_compactify_vertices` hygiene step so unreferenced vertices cannot inflate the bounding box the
unit/up-axis assertion measures.

**Stage 2 (colours → filaments) — claimed complete, is complete.** Plan item 2.1 turned out to be
already done in stage 1. The one interface change (`import_warning`) is defaulted, so no existing
call site moved. The claim that the OBJ path is unchanged is backed by a scripted line-by-line
comparison (105 of 105 body lines identical) *and* by golden tests from here on — that is stronger
evidence than most refactors get.

**Stage 3 (compressed/textured) — claimed 3a, 3c, 3d done and 3b deliberately not; that is what is
there.** Draco (3b) is **not implemented** and is refused by name. The reasoning in the status is
sound: Draco is the one piece that would force every developer and CI runner to rebuild `deps/`, and
`box_draco.glb` is a genuinely compressed fixture sitting ready for whoever wants to invert the
assertion. This is the only planned capability that did not ship, it is documented, and the failure
mode is a specific message rather than a generic loader error.

**Stage 4 (headless colour) — claimed complete, is complete.** It is scope the original three-stage
plan did not contain; it exists because stage 3's finding 3 showed that a hidden instance threw the
colours away. It is a coherent completion of stage 2/3 rather than an expansion.

### Known gaps, all documented by the stages themselves

1. **Draco is not decoded** (stage 3b, deliberate).
2. **Paletted, greyscale and 16-bit PNG textures are not decoded** — the in-tree
   `png::decode_colored_png` only handles 8-bit RGB/RGBA, so such a texture falls back to the
   "texture dropped" warning. Fixing it means widening `PNGReadWrite.cpp`, which the SLA and
   thumbnail paths share; correctly left out of scope.
3. **KTX2 / Basis textures** are refused or dropped, never decoded.
4. **Texture sampling is one colour per triangle** (centroid sample). A detailed texture on a
   low-poly mesh is quantised hard. Inherent to the approach and worth a release-note line: texture
   import approximates, it does not reproduce.
5. **`Color.hpp` calls `assert()` without including `<cassert>`** — pre-existing, worked around
   locally in the fuzz target, flagged for a separate one-line fix.
6. **A hidden instance answers the "saved in metres?" prompt with yes**, so a 1-unit cube such as
   `BoxVertexColors.glb` is scaled ×1000 on a phone import. Pre-existing hidden-mode behaviour, not
   caused by this feature, but it will be the first thing anyone notices.
7. **Cancelling the colour dialog surfaces as an error dialog** ("Import cancelled."), because
   `read_from_file` turns a `false` into a `RuntimeError`. Honest and non-hanging, but arguably a
   user-initiated cancel should not raise an error. Stage 1 flagged it as a reviewer decision and
   nobody has taken it.

Read independently of the stage notes, the refactor holds up: `paint_volume_from_vertex_colors` and
`paint_volume_from_face_colors` are verbatim moves of the loop bodies, with `volume` and the array
length turned into parameters and nothing else altered. They faithfully carry across the upstream
oddities too — `std::cout << "error"`, a `std::cout << ""` no-op, and the `filament_id2 <= 2`
comparison where its two siblings say `<= 1` (an upstream typo, most likely). Leaving those alone is
the correct choice for a move, and the `[golden]` tests now pin the behaviour including the typo; if
anyone ever fixes it, those tests are what will tell them what changed.

## 5. Test coverage

### Automated

| Suite | Where |
|---|---|
| 10 `[gltf]` scenarios (2 also `[golden]`) | `tests/libslic3r/test_gltf.cpp`, 840 lines |
| 2 `[objcolor]` scenarios | `tests/libslic3r/test_obj_color_match.cpp`, 185 lines |
| 30 fixtures | `tests/data/test_gltf/`, with `make_fixtures.py` (736 lines) regenerating the self-authored ones and `SOURCES.md` recording provenance and licences |
| a fuzz target | `tests/fuzz_gltf/` — driver, `mutate.py`, `EXCLUDE_FROM_ALL` |

The fixtures cover: the up-axis and unit rule in one assertion, external `.bin`, non-ASCII paths,
the metres rescue, two primitives with two materials, nested TRS, strips and fans, sparse accessors,
points-only, real Draco, an unknown required extension, a buffer URI trying to escape its directory,
a truncated file, quantized positions, meshopt compression, three textured cases, vertex colours,
and 24 materials against 16 slots. Licences were checked; one Khronos asset the plan listed as CC0
turned out to be CC-BY-4.0 and was replaced with a self-authored equivalent rather than taking on an
attribution obligation.

### What the stages verified but this integration did not repeat

* The **fuzz runs** (102,920 and 90,920 mutated cases, 0 findings). Re-running is cheap but slow;
  the corpus and driver are on the branch for anyone who wants to.
* The **phone-upload route end to end** into a hidden instance, and the stage-4 live colour table
  (three_materials / textured_two_regions / BoxVertexColors / many_materials with the reuse/add/merge
  counts). Those need a hub on a port the user's live hub wants.
* **Interactive GUI checks** — the manual checklist at the end of the stage-4 status is still
  unticked. See §7.

## 6. Proof taken for this integration

Everything below was produced on this branch, in its own build tree
(`.claude/worktrees/glb/build`, VS 2022 x64 Release, `BUILD_TESTS=ON`, deps from
`C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local`), and on an isolated `--datadir` copied
from `snorca_hubtest\dd_lan`. The user's live install and data directory were never touched.

### 6.1 `libslic3r_tests`

Built from scratch and run in full:

```
test cases:   621 |   619 passed | 2 failed as expected
assertions: 54559 | 54557 passed | 2 failed as expected
```

The two expected failures are the pre-existing pair in `tests/libslic3r/test_mixed_filament.cpp`
(lines 3483 and 4429) — nothing to do with glTF, and the same two the release head has.

The arithmetic ties out exactly. The stages contribute **12** test cases (`--list-tests
"[gltf],[objcolor]"` lists exactly 12: 10 `[gltf]`, of which 2 are also `[golden]`, plus 2
`[objcolor]`), so the base this branch sits on is 609 cases. And 609 is right: the stages branched
from `d6e020124a`, which the stage statuses recorded at 584 cases, and
`git diff d6e020124a 526ed0afdc -- tests/libslic3r` adds 25 `TEST_CASE`/`SCENARIO` macros and
removes none. 584 + 25 = 609, + 12 = **621**. (The brief quoted 605 for the release head; that
figure is four cases stale — five commits merged into `feat/ultra-preferences` after it added
tests.)

By tag:

```
libslic3r_tests.exe "[gltf]"      All tests passed (451 assertions in 10 test cases)
libslic3r_tests.exe "[objcolor]"  All tests passed (218 assertions in  2 test cases)
```

### 6.2 CLI import and slice

`cmake --install`ed to a scratch prefix, then every sample imported by the CLI and sliced to
Success on the isolated data dir, exporting a 3MF that was read back and measured
(`scripts/orca_cli.py`, printer `Snapmaker U1 (0.4 nozzle)`, process
`0.20 Standard @Snapmaker U1 (0.4 nozzle)`, filament `Generic PLA`):

| Sample | What it exercises | Slice | Objects / volumes | Object name · part names | Size read back from the 3MF |
|---|---|---|---|---|---|
| `box_10_20_30.glb` | plain binary GLB | Success, 890 s / 2.86 g | 1 / 1 | `box scene` · `box` | **(10, 30, 20)** |
| `box_10_20_30.gltf` + `.bin` | JSON glTF, external buffer | Success, 890 s / 2.86 g | 1 / 1 | `box scene` · `box` | **(10, 30, 20)** — identical to the GLB |
| `nested_trs.glb` | nested TRS, up-axis | Success, 66 s / 0.12 g | 1 / 1 | `nested_trs` · `child` | **(8, 2, 4)** |
| `two_parts_two_materials.glb` | two primitives → two volumes | Success, 463 s / 1.31 g | 1 / **2** | `two_parts_two_materials` · `blocks_1`, `blocks_2` | **(22, 10, 10)** |
| `agent_box.glb` | plain binary GLB, never seen before | Success, 373 s / 0.72 g | 1 / 1 | `agent box scene` · `block node` | **(7, 13, 11)** |
| `agent_ext.gltf` + `.bin` | external buffer, never seen before | Success, 373 s / 0.72 g | 1 / 1 | `agent box scene` · `block node` | **(7, 13, 11)** — identical to the GLB |
| `agent_hierarchy.glb` | `T·Ry(90°)` over `S(3,1,2)`, never seen before | Success, 146 s / 0.29 g | 1 / 1 | `agent hierarchy scene` · `child` | **(12, 6, 4)** |

Every size is the geometry the source file describes, after the documented `1 unit = 1 mm` and
Y-up→Z-up rules; every object carries a name taken from the glTF (scene name, or the file stem when
the scene is unnamed) and every volume a name taken from its node; every object sits at `z = 0` on
the plate. The two pairs that should be byte-equivalent — the same box as GLB and as glTF+bin —
produce identical bounding boxes and identical slice results (890.2 s / 2.86 g and 373.2 s / 0.72 g
respectively), which is the external-buffer path proving itself against the embedded one.

Three of the samples are **not** repo fixtures. `agent_box.glb`, `agent_ext.gltf` + `.bin` and
`agent_hierarchy.glb` were written for this integration by a standalone script that shares no code
with the branch's `make_fixtures.py` — a plain GLB, a JSON glTF with an external buffer, and a
two-level node hierarchy (parent `T(10,0,0)·Ry(90°)`, child `S(3,1,2)`, a 2×4×6 box). They matter
because the repo fixtures were authored by the same sessions that wrote the reader; passing on
files the feature has never seen is the stronger claim. The hierarchy sample is the sharpest of the
three: getting `(12, 6, 4)` out of it requires the TRS composition, the rotation and the Y-up→Z-up
swap all to be right, and any one of them being wrong gives a different set of three numbers.

### 6.3 Hidden GUI instance

Two imports into a **hidden** scratch instance (its own install, its own `--datadir`, its own hub
token; the hub picked 13641 and then 13642, so the user's live hub kept 13640 throughout — checked
before and after). Read back through the instance's own loopback API:

* **`three_materials.glb`** → one object `traffic light`, size `[34, 10, 10]`, on the plate at
  `(135.5, 136.0, 5.0)`. The plate thumbnail shows **three cubes in red, green and blue** — so
  Stage 4's headless colour matching ran with nobody at the PC, and no dialog was answered. The
  filament list afterwards had grown from the five it started with
  (`#FF8800 #FFFFFF #000000 #D50000 #FEC600`) to seven, the new two being `#03FF07` and `#0102FF`:
  the red **reused** the already-loaded `#D50000` and green and blue were **added**. That is exactly
  the "input 3, clusters 3, reused 1, added 2" row of the Stage 4 status table, reproduced on a
  different machine state four days later.
* **`agent_hierarchy.glb`** (the sample this feature has never seen) → one object
  `agent hierarchy scene`, plate footprint `[129.5, 133.0] .. [141.5, 139.0]`, i.e. 12 × 6 mm, and
  the thumbnail is a 12 × 6 × 4 slab. Matching what the CLI reported, and what the transform
  composes to on paper.

Both instances were quit through their own API, and only processes started here were stopped
(filtered on `ExecutablePath` under this session's scratch install). The live install was never
touched, before or after.

## 7. What remains unverified

Listed plainly, because a reviewer should know what this integration did *not* stand behind:

1. **Every interactive GUI path.** File ▸ Import from a real window, drag-and-drop, the colour
   dialog appearing with three swatches and OK assigning three filaments, and the reload-from-disk
   guard at `Plater.cpp:14826`. The registration points were read and are correct, and the CLI and
   the hidden-instance API exercise the same `Model::read_from_file` and the same
   `ObjImportColorFn`, but nobody clicked anything. The Stage 4 manual checklist is still the right
   list to work through, and it is unticked.
2. **The OBJ interactive path after the refactor.** Guarded by two `[golden]` tests and by a proven
   pure move, not by an actual OBJ import in a window.
3. **The fuzz corpus was not re-run** on this build. Stage 1 and Stage 3 ran 102,920 and 90,920
   mutated cases with zero findings; the target and `mutate.py` are on the branch, and
   `EXCLUDE_FROM_ALL` means it needs the per-generator build command in the header of
   `fuzz_gltf.cpp`. Worth one 10-minute run before release, since the reader sits behind the LAN
   upload endpoint.
4. **The phone upload route** (`POST /r/<token>/i/<pid>/open?mode=import` and
   `POST /api/instances/open`) was not re-exercised here — the hidden instances were started
   directly with a file on the command line. The allow-list edit is three characters wide and the
   stages verified the route, but that specific HTTP path was not re-driven on this build.
5. **macOS.** The `Info.plist.in` document type has never been built or opened on a Mac, and
   `make_temp_stl_with_modelio` sits in the same dispatch chain untouched.
6. **Large and real-world assets.** Everything imported here is under 30 KB. No file from a real
   exporter (Blender, Sketchfab, Substance) has been through this reader, and no file anywhere near
   the 512 MB / 20 M-triangle caps.
7. **Draco, KTX2, and paletted/greyscale PNG textures** — refused or dropped by design, so "not
   verified" here means "not implemented", not "untested".

## 8. Verdict

**The branch is ready to merge into `feat/ultra-preferences`**, with one product decision to take
first and one manual pass to schedule.

What backs that: the four stages are a complete and coherent feature rather than a half-finished
stack; they rebase onto the release head with zero conflicts; the build is clean; the whole
`libslic3r` suite passes with exactly the pre-existing two expected failures and exactly the twelve
new cases the stages wrote; and seven files — four repo fixtures and three written for this
integration, which the reader has never seen — import through the real CLI and slice to Success with
the right object count, the right names and bounding boxes correct to the millimetre. The one
capability the plan named and did not deliver, Draco, fails with a message that says "Draco" rather
than a generic loader error, and the fixture for turning it on is already committed.

Before merging:

* **Take the cancel decision** (§4 gap 7): a user who cancels the colour dialog currently gets an
  error dialog. It is one branch in `Model.cpp` either way, but it should be somebody's decision
  rather than an accident of `read_from_file`'s contract.

Before releasing:

* **Work the Stage 4 manual checklist** — it is seven interactive checks and none of them are hard.
* **Re-run the fuzz target** for ten minutes, because this parser is reachable from the network.
* **Two release-note lines**: texture import approximates (one colour per triangle), and a phone
  import of a model authored in metres is scaled ×1000 because the hidden instance answers the
  metres prompt with yes.

Nothing on that list is a reason to hold the merge; they are a reason not to call the feature
finished on the day it lands.
