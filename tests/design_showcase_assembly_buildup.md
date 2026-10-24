# Showcase: assembly build-up (design, 2026-10-24)

The owner's idea (2026-10-24): a new build-up mode for Showcase videos. Parts fly in from outside the
frame and settle into place until the model is fully assembled. It is an alternative to the
layer-by-layer build-up S5 added.

This is a design only. No product code is changed. Phase names follow `tests/design_showcase_tab.md`.
This would be **S6a**.

---

## 0. TL;DR

- **It fits the existing machinery.** A new timeline event, `"type": "assemble"`, sits next to
  `build`. It compiles, GL-free and deterministically, to a per-part schedule: start time, landing
  time, direction and distance. Each frame then evaluates one rigid transform per part at `t`.
- **The renderer change is small.** The Showcase canvas owns its GLVolumes
  (`ShowcaseRenderer::update_visibility`), and every pass reads `GLVolume::world_matrix()`: the
  material draw, the shadow map, the contact shadow, the mirror and the ID pass. Applying a per-part
  transform at the start of `draw_scene` and restoring it afterwards makes all of them agree.
- **There are three real fixes:**
  - the shadow and contact-shadow fit boxes;
  - framing, which must use the at-rest box rather than the in-flight one;
  - callout anchors, which must ride with their part.
- **No shader changes,** so Mac risk is low.
- **What a "part" is.** The default is "Auto":
  - the volumes of a multi-part object explode apart;
  - separate objects on the plate drop in.

  A single-body model splits into connected pieces when it has more than one. Otherwise it drops in
  whole, with a note suggesting the layer build-up. Painted colours are surface shells, so they never
  fly.
- **Where parts end.** By default they land where they sit on the plate. "Assembled" (the Assemble
  view's poses) is optional. It is meaningful only when something wrote those poses: split-to-objects,
  a Bambu 3MF, or our Measure assembly fit. **Cut halves and pins/dowels cannot fly back together yet,**
  because the cut doesn't record where they came from. A small hook in the Pin PR 2 / OD11 cut work
  would fix that (decision D8).
- **Upstream.** Bambu Studio added an "Assembly guide" in 2026-06: step trees, per-volume keyframes,
  auto-explode and MP4/PDF export, stored as `Metadata/assembly_step.json`. It is about 20k lines of GUI
  code. We borrow its auto-explode algorithm (dominant-axis stacking) but don't port it (D9).
- **Plan:** 3 PRs (about 5 to 7 agent-days), plus an optional 4th for reassembling cut parts.
  The owner decisions are in section 9.

---

## 1. What exists today (main at 49a62d89e6)

### 1.1 Showcase build-up and paths

- **Path file** (`docs/showcase_paths.md`, `src/libslic3r/Showcase/ShowcasePath.{hpp,cpp}`).
  - Format `edgeslicer-showcase-path`, version 1: keyframes, moves (compiled to keys) and events.
  - Events: `lighting`, `background`, `floor`, `look`, `callouts`, `watermark`, `build`.
  - `build` needs `from`/`to` and takes `start`, `end`, `ease`, `source` (`auto|toolpaths|clip`),
    `infill`, `supports` and `follow`. An unknown `source` value is an error naming the allowed values,
    and so is an unknown event type.
  - `evaluate_timeline(events, base, t, video_build)` returns a `TimelineState`: the scene,
    the callout visuals, `build`, `build_source`, `build_infill`, `build_supports` and `build_follow`.
    It is a pure function of `t`.
- **Video settings** (`ShowcaseVideo.hpp`).
  - `VideoSettings::build` is a `BuildUp` with `enabled`, `from`, `to`, `easing`, `source`,
    `infill` and `supports`.
  - It serialises as `"build": {...}` inside the scene's `"video"` (`ShowcaseVideo.cpp:715`).
  - An unknown `source` read by `venum` keeps the default, so it degrades to Auto.
- **The layer build-up** (`ShowcaseRenderer::draw_scene`, `set_build`, `build_height_at`).
  - **Toolpaths**: Preview's libvgcode viewer draws the whole plate's slice as one buffer, in plate
    coordinates.
  - **Clip**: a horizontal plane `m_build_clip_z` cuts the meshes. It is passed to the material
    shader, the contact shadow and the shadow pass (`rs.clip_top`).
  - **Timing**: `BuildSchedule` gives every layer the same time.
- **Camera paths.** `VideoPath::{Orbit, Turntable, OrbitBob, Keyframes, Static, StaticZoomOut}`.
  - StaticZoomOut and `build.follow` go through `frame_built_part(pose, box_min, box_max, built_z)`
    (`ShowcaseView::video_pose`). It keeps the angle and zooms out as the build grows.
  - A turntable is an orbit with the lights counter-rotated (`spin_lights`). The model's world
    transform never changes, which matters below.
- **Callouts "As used"** (`ShowcaseView.cpp:2503`). A callout appears once
  `built_z >= first_z[callout]`, fading in over `fade_mm`, where `first_z` is the height at which
  its filament is first printed.
  - Anchors are `CalloutAnchor{object_idx, instance_idx, local}` and are resolved through
    `showcase_anchor_world`, which uses the ModelInstance matrix. **They know the object but not the
    volume.**
- **The renderer's objects.**
  - The Showcase canvas (`CanvasShowcase`) holds the same objects as Prepare, at their plate
    transforms (`GLCanvas3D::reload_scene`; the AssembleView branch is the only one using assemble
    transforms).
  - `update_visibility()` sets `is_active` every frame: only the current plate's instances, plus its
    wipe tower unless hidden.
  - `draw_objects_material` draws per GLVolume. Painted multi-material is drawn per filament sub-model
    (`mmuseg_models`), with `volume_world_matrix = v->world_matrix()`.
  - `render_contact_shadow`, `render_reflection` and `GLCanvas3D::_render_shadows` also iterate
    active GLVolumes and use `world_matrix()` and `transformed_bounding_box()`.
  - **The shadow map is rebuilt every frame** (`m_shadow_map_valid = false` in `draw_scene`). Its
    light frustum is fitted to the union of the active volumes' boxes (`GLCanvas3D.cpp` around
    line 10200).
  - **The contact shadow** fits its rectangle to `objects_box()`. Only geometry within
    `contact_height` (1.5 to 12 mm) of the floor contributes.
- **CLI** (`ShowcaseRenderCli`). `--showcase-render` with `--path`, `--still`, `--dry-run` and
  `--report` (the report has `"build": "clip"|"toolpaths"`).
- **The panel** (`ShowcaseViewVideo.cpp`).
  - A "Layer build-up" checkbox with Infill/Supports, the callout Position/Appearance choices, a
    painted timeline strip (`paint_timeline`) and a Time scrubber (`m_vid_time`).
  - Path Import/Export.

### 1.2 Assemble view and explode (GUI)

- **Two poses per instance.** `ModelInstance` has `m_assemble_transformation` and
  `m_offset_to_assembly`, saved in the 3MF as `assemble_item` (`bbs_3mf.cpp` around line 5610). Its
  `is_assemble_initialized()` flag says whether one was ever set.
- **Explode** (`GLVolume::world_matrix`, `3DScene.cpp:449`).
  - It translates each volume by `(explosion_ratio - 1) * (volume offset in the object +
    the instance's offset_to_assembly)`, applied in the volume's local frame (`m.translate`).
  - **Direction**: each part moves away from its object's origin, the way its offset already points.
  - `explosion_ratio` is a **static global** (1 to 3, the Assemble view's slider or the mouse wheel,
    `GLCanvas3D.cpp:5946/12149`). Showcase must never use it: it would move every canvas.
- **Where the assembled pose comes from today:**
  - **`Model::InitializeAssemblyPositions`** (`Model.cpp:603`). Called on load and import,
    FillBed, Emboss, the bake jobs and so on, for objects that aren't initialized. It **lays new
    objects out in a row** 10 mm apart. That isn't an assembly; it just keeps them apart.
  - **`ModelObject::split`** (`Model.cpp:2435`). Splitting a multi-part object into objects keeps the
    assembled pose: each new instance gets `assemble = old_assemble * ...` and
    `offset_to_assembly = the volume's old offset`. **A "split to objects" model keeps its real
    assembly.**
  - **Edge's Measure assembly fit** (`GLGizmoMeasure::ultra_apply_attachment_print_pose`,
    `GLGizmoMeasure.cpp:3149`). It writes the print pose *and* a mirrored assembled pose, so mated
    parts are assembled in the Assemble view. `ultra_merge_parts()` then merges them into one object
    with two volumes.
  - Moving things in the Assemble view writes the assemble pose (`GLCanvas3D.cpp:7013/7148`,
    `Selection.cpp:1724`).
  - A Bambu 3MF with `assemble_item`s brings its poses.
- **Cut.**
  - **Outputs:**
    - halves become separate objects sharing a `cut_id`;
    - connectors are volumes with `cut_info.is_connector`;
    - dowels become their own objects (`CutUtils.cpp:144`, `CreateDowels`);
    - "keep as parts" keeps one object with both halves as volumes, still assembled.
  - **Nothing in the cut sets an assemble pose** (no setter in `CutUtils.cpp` or the cut gizmos).
    The re-editable cut recipe (`CutRecipe.hpp`) stores the plane in the *original* object frame and
    the pre-cut mesh, but not the transform from each half's new frame back to the original.
  - **The Pin connector (PR #693, PR 1 of 3)** produces `pushfit_pin_mesh` with `connector_to_mesh`:
    the transform from the connector frame to the pin's print pose. PR 2 (model, cut and 3MF wiring)
    waits for OD11 B.

### 1.3 Object list terms

| term | what it is | Showcase today |
|---|---|---|
| object (`ModelObject`) | a row in the object list | `focus_object`, callout anchors |
| instance | a copy of an object on a plate | each active instance drawn |
| volume / part (`ModelVolume`, model part) | a sub-row; modifiers, blockers and enforcers are not parts | drawn per GLVolume |
| painted region | facets painted with a filament, inside one volume | drawn per filament sub-model |
| plate | the current plate | only its instances are active |

---

## 2. What a "part" is

### 2.1 The rule ("Auto", the default)

A **flying part** is one rigid piece moved by one transform. It is collected from the current plate's
active, printable, non-modifier GLVolumes.

| situation | flying parts |
|---|---|
| several objects on the plate | one per **instance** (all its volumes move together) |
| one object with several model parts (volumes) | one per **volume** |
| several objects, some with several parts | one per volume (an object's parts explode, the objects drop in; see 3.1) |
| one object, one volume, a mesh with more than one connected piece | one per **connected piece** (`its_split`), as temporary display meshes |
| one object, one volume, one piece | **the whole object drops in** as one part, and the report and panel say "one part: the layer build-up may suit this model better" (D2) |
| wipe tower | not a part. It fades in over the first part's flight, or stays hidden (it follows `hide_wipe_tower`). |

The panel's "Parts" choice overrides this: Auto, Objects (whole instances only), Parts (volumes) or
Pieces (connected pieces of every volume).

### 2.2 Special cases

- **Multi-colour painted single meshes.** The painted regions are drawn as per-filament surface
  sub-models. A region is a **shell**, not a solid: flying one away shows the hole it leaves and its
  own open back. So painted regions never fly (D10). A painted single body is treated like any single
  body (2.1).
- **Cut objects:**
  - **Halves** are separate objects sharing a `cut_id`, or two volumes of one object with "keep as
    parts".
  - **Dowels** are separate objects. After PR 2, **pins** will be too (lying on a flat in print pose).
  - **Plug/snap connectors** are merged into a half.
  - **Grouping.** In "Auto", members of a `cut_id` group are ordered together.

  **But on the plate the halves sit as printed** (flipped, apart), and nothing records how they fit
  back together. So:
  - **v1, target "plate":** halves and dowels drop onto their plate spots like any objects. This is
    correct, and dull.
  - **"keep as parts" cut:** the halves are volumes of one object in their assembled place, so they
    explode and come back together. Connectors are already merged.
  - **The "pins fly into holes" shot needs an assembled pose** for each half and pin relative to the
    uncut object. The cut knows it at cut time. With the gizmo's plane, rotation and "place on cut",
    each new object's transform back to the original object frame is one matrix. Proposal (D8):
    - the cut writes it into each output instance's `assemble_transformation`, so the Assemble view
      shows the cut model assembled;
    - PR 2 of the Pin connector writes the pin's connector pose there the same way. It already
      computes `connector_to_mesh`.

    This is about 30 lines in `CutUtils`/`Cut::perform_*` plus a unit test, with no G-code impact.
    It belongs to the cut work, not to Showcase. Showcase then needs only target "assembled" (3.5) and
    the connector ordering rule (3.2).
- **Instances of one object** are separate parts with the same geometry. With order "bottom up", ties
  break by object, then instance, then volume index.
- **Text/emboss and SVG parts** are volumes: they fly like any part (a logo dropping onto a lid).
  Negative volumes and modifiers are never drawn and never fly.

### 2.3 Plates

Only the current plate, the same as everything else in Showcase (D17 of the Showcase design). A
"whole project" assembly across plates is out of scope: the Showcase scene is per plate.

---

## 3. Motion design

### 3.1 Start position: where a part comes from (`direction`)

Each part `i` has a rest transform `R_i` (its world pose at the end), a unit direction `d_i`, a
distance `s_i` and an optional tumble. Its motion is a translation of `R_i` (3.4).

| `direction` | `d_i` | notes |
|---|---|---|
| `auto` (default) | `explode` for the volumes and pieces of one object; `above` for whole objects | separate objects side by side "explode" sideways into each other's paths, while dropping in reads well |
| `explode` | **dominant axis**, Bambu's auto-explode: the axis (±X, ±Y, ±Z) along which the part's rest-box centre is furthest from the centre of all parts | clean, axis-aligned; parts on the same axis stack outward (see below) |
| `radial` | normalised (part centre − assembly centre); straight up if they coincide | more organic and diagonal |
| `above` | +Z | "drop in"; the floor shadow grows as it lands |
| `left`, `right`, `front`, `back` | world ±X / ±Y | |
| `screen_left`, `screen_right`, `screen_top` | the camera's right/up vectors at the part's start time, flattened so a part never comes up through the floor | "from outside the frame" whatever the camera does |
| `plate` | none: the part starts at its **plate pose** and flies to its **assembled pose** | only with `target: "assembled"`; the "print, then assemble" story (3.7) |
| per part | `"overrides": [{ "part": ..., "direction": ... }]` | for a part that should come from a particular side |

**Explode stacking** (borrowed from Bambu Studio's `auto_explode_current_keyframe`, facts in 6.1):
- Parts are bucketed by direction and sorted by their distance from the centre.
- Each one is pushed past the previous one's far face plus a gap.
- So parts in the same bucket fly in on separate lanes and don't pass through each other.

We use that order and those lanes. The final distance is then set by `distance`.

**`distance`:**
- **`"auto"` (default).** The smallest distance at which the part's bounding sphere is fully
  **outside the view frustum at its start time**, plus 10%.
  - It is computed analytically against the four side planes of the compiled camera at `t_start`:
    sphere centre `c + s·d`, solve `n·(c + s·d) + o >= r` for the plane that `d` leaves through.
  - If `d` points along the view axis (towards or away from the camera), a frustum plane is never
    crossed. Then use 2.5 × the assembly's radius and clamp, so the part comes from behind the
    camera or recedes. The report warns about it.
  - The camera path is compiled and deterministic, so this is deterministic too.
- **A number:** millimetres.

### 3.2 Order (`order`)

| `order` | rule | tie-break |
|---|---|---|
| `bottom_up` (default) | rest-box min Z, then centre Z: what is lower lands first, so nothing lands on air | object, instance, volume, piece index |
| `inside_out` | the reverse of the explode lanes: the core first, then outward. This is the "assembly order inferred from the explode". | as above |
| `list` | object list order, then volume order | |
| `largest_first` | rest-box volume, descending (base parts first) | as above |
| `custom` | `"sequence": [ ... ]` of part references: an object name or 0-based index, `"obj:vol"`, or a volume name. Unlisted parts follow in `bottom_up` order. | |

**Connectors** (`"connectors": "between" | "with_half" | "last"`, default `between`):
- For a `cut_id` group with pins or dowels, `between` gives: lower half, then the pins/dowels,
  then the upper half. That is the satisfying sequence.
- It only means something once halves have an assembled pose (D8).

**Simultaneous parts.** `"group_by": "none" | "object" | "filament"` lands all members of a group
together (all of one colour at once, for example). It is off by default.

### 3.3 Timing

- The event's `from`..`to` is the whole assembly. With `n` parts, flight time `D` and overlap
  `o ∈ [0, 0.9]` (default 0.35):
  - step `= D·(1 − o)`;
  - total `= D + (n − 1)·step`;
  - so `D = (to − from) / (1 + (n − 1)(1 − o))`.
- Part `k` (in order) flies over `[from + k·step, from + k·step + D]`.
- With `group_by`, `k` counts groups.
- If `"per_part": seconds` is given instead, `to` is derived, and the report warns if it runs past the
  video.
- A part is **not drawn before its start** (`is_active = false`), so an off-frame part never casts a
  shadow into the frame early.
- **Ease** (`ease`, default `ease_out`): fast in, slowing to land. Linear, ease_in_out and ease_in are
  also available, as for `build`.
- **Settle** (`settle`, 0 to 1, default 0.3): a single small overshoot along `−d`, then back. It is a
  "back-out" curve with overshoot `= settle × min(3% of the part's size, 2 mm)`, in the last 20% of
  the flight. One bounce only, because several look cartoonish (D5). Downward (`above`), the
  overshoot is clamped so the part never goes below its rest Z: it settles from above.
- **Tumble** (`spin`, degrees, default 0): the part turns about a fixed axis while flying and is
  upright when it lands, via a slerp from `spin` to 0. The axis is derived from `d` (`d × Z`, or X).
  It is off by default (D11).
- **Reverse** (`"reverse": true`): disassembly, the same schedule played backwards. A video can
  assemble, hold, then explode out, and loop.

### 3.4 The pose at time t (the contract the tests pin)

```
u   = clamp((t - t0_i) / (t1_i - t0_i), 0, 1)
p   = settle_ease(ease(u))            # 0 at start, 1 at rest; may slightly exceed 1 near the end
off = (1 - p) * s_i * d_i             # world mm
rot = slerp(Q(spin_i), I, ease(u))    # about the part's rest-box centre
M_i(t) = T(off) * T(c_i) * rot * T(-c_i) * R_i
visible_i(t) = t >= t0_i              # (reverse: t <= t1_i)
```

`target: "assembled"` with `direction: "plate"` interpolates between the plate pose `P_i` and the
assembled pose `A_i`:
- translation is lerped, rotation slerped, both about the part centre;
- a lift arc `h·sin(πu)` is added, with `h` set so the path clears the assembly's top. This stops
  parts sliding through each other along the floor.

Everything is a function of `(parts, rest poses, params, compiled camera)`, so it is deterministic.
There is no RNG. If jitter is ever added, it comes from a hash of the part key and a `seed`.

### 3.5 Target: where a part ends (`target`)

- **`plate` (default).** As printed: the scene the user set up and the framing they chose.
- **`assembled`.** Each part ends at its Assemble-view pose (`get_assemble_transformation()`,
  `offset_to_assembly`, and the volume transform within). The whole assembly is translated so its box
  is centred where the plate's objects' box was, and dropped to Z = 0.
  - **It is meaningful only when the poses were written** by split-to-objects, a Bambu 3MF, the
    Measure assembly fit, a hand placement in the Assemble view, or (after D8) a cut.
  - **Detection.** If every pair of parts is more than 1 mm apart at the assembled pose and they lie
    in a row (the `InitializeAssemblyPositions` layout), the report and panel warn: "these parts have
    no assembled positions; they will land as laid out in the Assemble view".
  - **Framing then uses the assembled box, not the plate.** The scene's saved camera was framed on
    the plate, so the panel offers "Frame the assembly" (the same as `frame_model()` on the rest box).

### 3.6 Camera

- **Static** (the default for assembly) and the scene's camera both work unchanged. Framing (the
  default view, `frame_model`, `focus_object`) must use the **rest box**, never the current
  in-flight box (5.2).
- **Orbit and turntable** work unchanged: part motion is in world space, and a turntable never moves
  the model.
- **"Follow" for assembly** (later, v2) is the analogue of `frame_built_part`: frame the union of the
  landed parts plus the current part's rest box, zooming out to the full rest box. It risks pumping
  when a big part lands, so it is smoothed with the same `max_zoom` cap. It is not in v1.
- **Keyframes and paths** work as they are. Agents get `--dry-run` landing times to time moves
  against (4.4).

### 3.7 Shadows, floor, callouts and combination with the layer build-up

- **Shadows.** In-flight parts cast real, moving shadows: a dropping part's shadow sharpens as it
  lands. The fit fix is in 5.2.
- **Floor contact.** The contact shadow already counts only geometry within `contact_height` of the
  floor, so it appears as a part touches down.
- **The mirror floor** reflects flying parts for free.
- **Callouts:**
  - **"As used" becomes "as placed".** A filament's callout fades in when the first part using that
    filament lands. It uses the same fade, now over 0.3 s of time instead of `fade_mm`. A part uses
    its volume's filament plus its painted filaments.
  - **The leaders ride with their part.** An anchor's world point is `M_i(t) * rest_world`. Its part
    is found once per video, at rest, through the ID pass. The ID pass already records
    `object_idx`, `volume_idx` and `instance_idx` per pixel. A piece split from a volume needs the
    piece index added to the ID pass entry.
  - **New anchors** could store `volume_idx` as an optional key that older builds drop harmlessly.
  - A callout whose part is in flight is hidden, so there is no leader chasing a moving dot.
- **With the layer build-up:**
  - **v1: one or the other in any instant.** An `assemble` event overlapping a `build` event is a
    compile error naming both. In sequence they work: a `build` (clip) of the plate layout, then an
    `assemble` with `target: "assembled", direction: "plate"` gives "it prints, then the parts lift
    off the plate and assemble". That is the strongest story for multi-part prints, but it needs
    real assembled poses.
  - **Toolpaths can't fly.** libvgcode draws the plate's slice as one buffer in plate coordinates,
    with no per-object transform. During an assemble event the renderer draws meshes. If the scene's
    source is toolpaths, the report and panel say "assembly shows the model, not the toolpaths".
    The material look (layer stripes, finishes) stays, because procedural layer lines are in object
    space and move with the part.

---

## 4. The path file and the UI

### 4.1 A new event type rather than a new build `source` (D6)

- **Reusing `build` with `"source": "assembly"` would mix meanings.** `start` and `end` are fractions
  of a print, and infill, supports and follow mean nothing here. It would also turn an old build's
  scene into a layer build-up silently (`venum` keeps Auto).
- **A separate event type keeps both clean** and lets them be sequenced.

```json
{ "type": "assemble", "from": 0.5, "to": 6.5,
  "parts": "auto",            "_": "auto | objects | parts | pieces",
  "target": "plate",          "_t": "plate | assembled",
  "order": "bottom_up",       "_o": "bottom_up | inside_out | list | largest_first | custom",
  "sequence": ["Base", "1:2", "Lid"],
  "direction": "auto",        "_d": "auto | explode | radial | above | left | right | front | back | screen_left | screen_right | screen_top | plate",
  "distance": "auto",
  "overlap": 0.35,
  "per_part": 1.2,
  "ease": "ease_out",
  "settle": 0.3,
  "spin": 0,
  "connectors": "between",
  "group_by": "none",
  "reverse": false,
  "exclude": ["Stand"],
  "overrides": [ { "part": "Lid", "direction": "above", "spin": 90 } ] }
```

- **The parse rules are the existing reader's:**
  - unknown keys are warnings with the likely meant key;
  - an unknown value is an error listing the allowed ones;
  - `from`/`to` are required.
- **Compile errors name what exists.** A `sequence` or `overrides` part that doesn't exist lists the
  parts, as `focus_object` does.
- **Before `from`**, parts not yet placed are hidden: an empty plate with the floor and backdrop. With
  `reverse`, everything is assembled.
- **After `to`**, everything is at rest, so the scene looks exactly as without the event.
- **`version`** (D6). A version-1 reader errors on `"type": "assemble"` with "unknown type".
  - **Recommendation:** the writer stamps `"version": 2` only when the file uses an event added after
    v1, and the reader accepts 1 and 2.
  - An older EdgeSlicer then gives its existing "needs a newer EdgeSlicer" message instead of a
    confusing type error.
- **Scene JSON.** `VideoSettings` gains `AssemblyUp assembly` (the same fields), written as
  `"assembly": {...}` inside `"video"`.
  - An older build ignores the key, but drops it if it re-saves the scene. That is acceptable: the
    same trade-off S4 and S5 accepted.
  - `BuildUp` and `AssemblyUp` are mutually exclusive in the panel (4.2).
- An example ships: `resources/showcase/paths/assemble_drop.json` (an assemble over 6 s, a slow
  quarter orbit, and callouts as placed).

### 4.2 The Video panel

- **Build-up choice.** The "Layer build-up" checkbox becomes a choice: **None / Layer by layer /
  Assembly**. Layer by layer keeps its Infill/Supports rows. Assembly shows:
  - **Parts**: Auto / Whole objects / Parts / Connected pieces.
  - **Comes from**: Auto / Explode / All directions (radial) / Above / Left / Right / Front / Back /
    Its place on the plate (only with "Assembled").
  - **Order**: Bottom up / Inside out / Object list / Largest first. Custom stays path-file only
    in v1.
  - **Overlap**: a slider ("one at a time" to "all together").
  - **Settle**: a checkbox (on = 0.3).
  - **Assemble into**: Plate layout / Assembled view, with the 3.5 warning beside it when the poses
    look unset.
  - **A one-line summary**, for example "7 parts, 0.9 s each, from 0.5 s to 6.5 s".
- **Timeline strip.** `paint_timeline` gets a landing tick per part (hover shows the part's name), the
  same as the keyframe diamonds.
- **Scrubber and preview.** They already call `evaluate_timeline` at `t`. The assembly state comes
  from the same call, so scrubbing shows poses with no new code path.
- **Path Export** writes the `assemble` event. **Import** sets the panel from it when it is the only
  build-up.
- **Strings** go through `_L` and gettext as usual. The wording is the owner's to approve (D12).

### 4.3 Tooltips (draft)

- **Assembly:** "The parts fly in from outside the frame and settle into place until the model is
  whole. A model with one part drops in whole; the layer build-up may suit it better."
- **Assemble into, Assembled view:** "Where the Assemble view has the parts. Use it for parts that fit
  together, such as a split model or parts mated with the Measure tool's assembly."

### 4.4 CLI

- `--showcase-render` needs no new arguments: the path drives it.
- **The report gains:**

  ```json
  "assembly": { "parts": 7, "target": "plate",
                "order": [ { "part": "Base", "object": 0, "volume": 0, "lands": 1.4, "direction": "above" }, ... ],
                "warnings": [] }
  ```

- **`--dry-run`** prints the landing times, so an agent can time camera moves and callouts to them.
- **`--still <t>`** renders a mid-flight frame.
- `"build"` in the report stays as it is, and is `"none"` during a pure assembly video.

---

## 5. Rendering, performance and determinism

### 5.1 Per-frame per-part transforms

- **Where the data lives.** `ShowcaseRenderer::set_assembly(const AssemblyFrame*)`, where
  `AssemblyFrame` holds per part key (`object_idx`, `instance_idx`, `volume_idx`, `piece`) a
  `Transform3d` and a `visible` flag. `nullptr` means none.
- **Applying it (`draw_scene`, after `update_visibility`):**
  1. For each affected GLVolume, save its instance and volume transformations.
  2. Set `instance = M * instance`. For a volume-level part, set `volume = (inst⁻¹ · M · inst) ·
     volume`, so the world is `M · inst · vol`.
  3. Set `is_active = visible`.
  4. Run every pass: shadows, floor, contact, mirror, material, overlay.
  5. **Restore.** `set_*_transformation` marks the bounding boxes dirty, and they recompute lazily.
- **Why mutate rather than add a uniform.** Every consumer uses `world_matrix()` and
  `transformed_bounding_box()`: `_render_shadows` (in GLCanvas3D, not ours), the contact footprint,
  the reflection, the ID pass and `objects_box`. Mutating and restoring gives one consistent world
  with no edits to GLCanvas3D's shadow pass or the shaders.
- **Why it is safe.** The Showcase canvas has no selection editing and owns its GLVolumes. A
  `reload_scene` in the middle of a frame can't happen: it runs on the GUI thread between frames.
- **The ID pass** runs from the `before_frame` hook, so the poses must be applied **before** the hook
  as well. Restoring runs after `render_frame` and after `render_export`.
- **Connected pieces (`parts: pieces`).**
  - `its_split` once per video (cached by the object's mesh id and volume).
  - Each piece gets a temporary `GLModel` drawn through the material shader with the volume's
    material and layer-stripe info (`layers_for(object_idx)`). It isn't a GLVolume, so the shadow
    pass doesn't see it.
  - **Recommendation (v1):** while a split volume is in flight, swap the volume's GLVolume for its
    pieces in a small Showcase-owned `GLVolumeCollection` that is merged into the iteration, so every
    pass sees them. This is the only part with real plumbing, and it is why pieces sit in PR A2b
    (section 7), which can slip.
- **Painted volumes** move as one: their `mmuseg_models` share the GLVolume's world matrix.

### 5.2 The fixes that matter

1. **Shadow-map fit.** `_render_shadows` fits the light frustum to the union of the active boxes. A
   part 300 mm off-frame would inflate the frustum and blur every shadow, mid-video, frame to frame.
   - **Fix:** a "caster fit box" in `RealisticRenderSettings`, set by Showcase to the rest box
     × 1.25. `_render_shadows` uses it instead of `obj_bb` when present. It is a few lines; the
     `has_fit_box` plumbing for the plate clamp already exists.
   - Parts outside that frustum read the border (= lit), so a far part just casts no shadow until it
     is near. That looks natural.
2. **Contact-shadow rectangle.** It is fitted to `objects_box()`. Use the rest box instead, for the
   same reason.
3. **Framing.**
   - `objects_box()` stays "what is drawn now" for the passes.
   - A new `rest_box()` (computed before the poses are applied) serves `place_default_camera`,
     `frame_model`, `PathContext::plate`/`objects`, `frame_built_part` and the callouts' `fade_mm`.
4. **The clip plane** is off during an assembly (1e30). Compile errors keep the two from overlapping
   (3.7).

### 5.3 Performance

- **Per frame:** n matrix products and n bounding-box recomputes. That is nothing next to the
  supersampled draw.
- **Shadows** are rebuilt every frame already, so there is no new cost.
- **Pieces:** one `its_split` per volume per video (milliseconds to about a second on a large mesh)
  and the GPU memory of a second copy while in use. They are freed when the video ends or the tab is
  left.
- **The schedule** compiles once per path or settings change. Evaluating it per frame is O(n).

### 5.4 Determinism and export

- **The libslic3r side is GL-free.** It holds the part list (keys, rest boxes, rest transforms,
  filaments, `cut_id`, connector flag) → schedule → `pose_at(t)`, so it is unit-testable like
  `BuildSchedule`.
- **Inputs:**
  - the part list, sorted by key before any rule runs (so the GLVolume order doesn't matter);
  - the params;
  - the compiled camera (for `distance: auto` and the screen directions).
- **Floating point.** Use the same operations in the same order and no parallel reductions.
  Bit-identical frames across runs on one machine are the goal, as for the existing export.
- **Video export** renders frame `k` at `frame_time(v, k)`. The assembly state comes from the same
  `t`, so preview, export and the CLI agree.

---

## 6. Upstream and other apps (facts only)

### 6.1 Bambu Studio (reference clone `C:/Dev/BambuStudio`, v02.08.04.57+)

- **Assemble view with explode.** The same as ours:
  - `GLVolume::explosion_ratio`, a static, 1 to 3;
  - the offset is `(ratio − 1) × (volume offset + offset_to_assembly)`;
  - the Assemble view uses instance `assemble_transformation`.
- **"Assembly guide" (2026-06-24, `7d67dd459`)**, in `src/slic3r/GUI/Overview/AssemblySteps*`
  (about 20k lines) and `src/libslic3r/Format/AssemblyStepsJson.hpp`.
  - **Steps:** a step tree of steps, objects and volumes ("sub-assembly", "final assembly").
  - **Per-step keyframes,** each with a camera (view matrix, zoom, margin) and per-object and
    per-volume **assemble transforms**. Volumes are keyed by `(object_idx, volume_idx)` plus a stable
    `part_guid`. Newer Bambu also gives `ModelVolume` its own `assemble_transformation`.
  - **Interpolation** between keyframes: rotation slerped, translation and scale lerped
    (`interpolate_keyframe`).
  - **Annotations:** notes, arrows, SVG icons, part-number labels bound to volumes.
  - **Export:** PDF, Markdown and **MP4** (with an intro cover and step titles, cancellable as of
    2026-09-08).
  - **Storage:** saved in the 3MF as `Metadata/assembly_step.json`.
  - **Auto-explode** (`auto_explode_current_keyframe`):
    - centre = the box of every item;
    - each item's direction is the dominant axis of (item centre − centre), giving 6 buckets;
    - each bucket is sorted by distance;
    - each item is pushed to the bucket's running boundary plus a **5 mm gap**.

    Items already assembled in an earlier step stay pinned. We borrow this rule as
    `direction: "explode"` (3.1).
- **Licence.** Bambu Studio is AGPL-3.0, the same as EdgeSlicer, so porting code would be allowed.
  The recommendation is **not to port** the guide: it is a separate authoring product with a large UI
  surface. EdgeSlicer currently drops `Metadata/assembly_step.json`. A later option is to *read* its
  step order as `order: "steps"` (D9).

### 6.2 Others

- **OrcaSlicer** (upstream main) and **Snapmaker Orca:** the Bambu-derived Assemble view and explode
  slider. No assembly guide or `AssemblySteps` files on `orcaupstream/main` or
  `snapmaker-upstream/main`.
- **PrusaSlicer:** no Assemble view and no explode.
- **Common CAD and rendering tools (general knowledge, not checked here):**
  - **Fusion 360's Animation workspace:** auto-explode by level, a storyboard timeline, per-part
    transform steps.
  - **SolidWorks:** exploded views with explode steps and an "Animate collapse/explode" wizard.
  - **Onshape:** exploded views with steps.
  - **KeyShot / Blender:** per-part keyframed translation and rotation.

  **The ideas worth taking:**
  - explode steps that *are* the assembly order, reversed;
  - a collapse/explode toggle (our `reverse`);
  - staggered overlap on a timeline;
  - part-number labels appearing as parts land (our callouts "as placed").

---

## 7. PR split

| PR | contents | size | depends on |
|---|---|---|---|
| **A1: schedule and path** (libslic3r, GL-free) | `Showcase/ShowcaseAssembly.{hpp,cpp}`:<br>- `AssemblyPart` and `AssemblyParams`;<br>- the order rules and explode lanes;<br>- the directions, `distance: auto` against a frustum, timing, settle and tumble;<br>- `pose_at(t)`;<br>- the `assemble` event (parse, write, compile errors, overlap with `build` an error, path version 2 rule);<br>- `VideoSettings::assembly` JSON and `sanitize`;<br>- `PathContext::parts`;<br>- `docs/showcase_paths.md`;<br>- unit tests (8.1) | M (2 d) | nothing |
| **A2: renderer and CLI** (GUI) | - collect parts from the active GLVolumes;<br>- apply/restore poses in `draw_scene` and before `before_frame`;<br>- `rest_box()` for framing;<br>- shadow caster fit box (`RealisticRenderSettings`, one branch in `_render_shadows`) and contact rectangle;<br>- hidden-before-start;<br>- callouts as placed and anchor-to-part via the ID pass at rest;<br>- target "assembled" with the row-layout warning;<br>- the CLI report `assembly` block;<br>- the example path | M to L (2 to 3 d) | A1 |
| **A2b: connected pieces** | `its_split` cache, piece GLVolumes in a Showcase-owned collection, piece index in the ID pass | S to M (1 d) | A2; can slip |
| **A3: panel** | the build-up choice, the Assembly rows, landing ticks on the timeline, path import/export to the panel, tooltips and strings | S to M (1 to 1.5 d) | A2 |
| *(cut work, not Showcase)* **C1: cut writes assembled poses** | `Cut::perform_*` sets each output instance's `assemble_transformation` (back to the uncut frame); dowels and (Pin PR 2) pins get their connector pose; unit test that halves at their assembled poses rebuild the uncut box | S (0.5 d) | D8; belongs in OD11 B / Pin PR 2 |
| *(later)* **A4** | `connectors: between` sequencing for `cut_id` groups; "print, then assemble" (`direction: plate`); follow camera; `order: "steps"` from a Bambu `assembly_step.json` | M | A2, C1 |

- **Builds.** Test-only for A1 (`libslic3r_tests`, can run on the Mac slot). The app
  (`encoding-check Snapmaker_Orca Snapmaker_Orca_app_gui`) for A2 and A3, with one `--install` test
  copy for the hand test.
- **No G-code impact,** so no gcode-gate. C1 changes the cut's model output but not geometry or
  G-code. The cut tests and one gate run on a cut project are prudent anyway.

---

## 8. Tests

### 8.1 Unit tests (`tests/libslic3r/test_showcase_assembly.cpp`, `[Showcase][Assembly]`)

- **Deterministic placement.**
  - For a fixed part set and params, `pose_at(t)` at t = from, mid-flight and to equals hand-computed
    matrices (1e-9).
  - Two compiles of the same inputs give bit-identical schedules.
  - Shuffling the input part order gives the same schedule.
- **At rest.** At `t >= to` every `M_i = R_i` exactly. Before `from`, nothing is visible; with
  `reverse`, everything is.
- **Order rules.**
  - `bottom_up` with equal min Z falls back to centre Z, then the key.
  - `inside_out` is the reverse of the explode lanes.
  - `largest_first`.
  - `custom` with unlisted parts appended `bottom_up`.
  - `group_by: filament` lands groups together.
  - `connectors: between` gives lower half, connectors, upper half for a synthetic `cut_id` group.
- **Explode lanes** reproduce Bambu's rule on a synthetic 3×1 row: a dominant axis, sorted by
  distance, and boundary plus a 5 mm gap. Parts in one lane never overlap at any sampled `t`
  (box test at 50 samples).
- **Timing maths.** `D`, step and total for n = 1, 2, 7 and o = 0, 0.35 and 0.9; `per_part` derives
  `to`; past the video's end gives a warning.
- **`distance: auto`.**
  - At `t0` the part's sphere is outside every side plane of the given frustum, and the result is
    minimal (95% of it would be inside).
  - A direction along the view axis gives the fallback and a warning.
- **Settle.**
  - The overshoot is at most the cap.
  - For `above`, Z never goes below rest Z.
  - p is monotonic before the last 20%.
- **Tumble.** The rotation is identity at landing and the full angle at start.
- **Path.**
  - The `assemble` event round-trips through `path_to_json` and `parse_path` unchanged.
  - Unknown keys give warnings; unknown enum values give errors listing the options.
  - A missing `from`/`to` is an error.
  - `sequence` or `overrides` naming a missing part gives a compile error listing the parts.
  - Overlap with a `build` event gives a compile error.
  - Version: a file using `assemble` is written as v2, a v1 file still parses, and v3 is refused with
    the "newer EdgeSlicer" message.
- **Scene JSON.** `VideoSettings::assembly` round-trips. `sanitize` clamps overlap, settle and spin.
  A scene without the key loads as None.
- **Target assembled.** The row-layout detector fires on `InitializeAssemblyPositions`-style poses and
  not on a split-to-objects fixture.

### 8.2 GUI, renderer and CLI checks (no GUI driving)

- **CLI `--dry-run --report`** on a 3-object fixture project with an assemble path: the landing times
  and order in the report match the unit-test expectations.
- **CLI `--still`** at the start, mid-flight and the end on the fixture. Check that the PNGs differ
  where expected and that the end still matches a no-assembly still at the same camera (pixel diff
  under 0.5%; any difference comes from shadow-map texel snapping).

### 8.3 Owner hand test (test copy, A2 + A3)

1. **Plate with 4 separate objects** (Bambu calibration cube, Benchy, two small boxes), static camera,
   Assembly with defaults.
   - They drop in bottom up and land with a small settle.
   - The shadows stay sharp throughout and appear as each object lands.
   - Nothing is visible before its turn.
2. **One object with 3 parts** (a box with a lid and a logo text part), "Explode", orbit 90°.
   - The parts come from their own sides on separate lanes and never cross.
   - The logo lands last when "Largest first" is picked.
3. **A split-to-objects model** (split a multi-part object into objects first): "Assemble into:
   Assembled view".
   - The parts land assembled, centred on the plate.
   - "Frame the assembly" frames it.
   - Then try a plain 2-object plate with "Assembled view": the warning shows.
4. **A cut model with dowels,** target plate: halves and dowels drop onto their plate spots. After C1,
   with target assembled: the lower half, then the dowels, then the upper half, and the dowels go
   into the holes.
5. **Single-body Benchy:** the one-part note shows and the Benchy drops in whole. "Connected pieces"
   on a two-body STL splits it (after A2b).
6. **A multi-colour painted model, Callouts on, Appearance "As used":** each filament's callout fades
   in as the first part with that filament lands. Leaders don't chase moving parts.
7. **Scrub the Time slider** back and forth: the poses follow exactly, with no drift. Export MP4 and
   check it matches the preview.
8. **Path round trip:** Export, then Import, then the same video.
9. **The Mac mini:** steps 1 and 7 (Mac slot test copy only if the owner asks; the memory rule).

---

## 9. Owner decisions

| # | decision | options | recommendation |
|---|---|---|---|
| **D1** | Where parts end by default | plate layout / assembled view | **Plate layout.** "Assembled" is an option with a warning when the poses look unset. |
| **D2** | A single-body model | drop in whole / always offer connected pieces / refuse and point to the layer build-up | **Connected pieces when there are 2 or more, otherwise drop in whole** with a one-line note suggesting the layer build-up |
| **D3** | Default direction | explode (axis lanes) / radial / above / auto | **Auto:** an object's parts explode (axis lanes); separate objects drop from above |
| **D4** | Default order | bottom up / inside out / object list / largest first | **Bottom up** (nothing lands on air) |
| **D5** | Landing feel | none / one small overshoot / several bounces | **One small overshoot** (settle 0.3), downward never below rest |
| **D6** | Format | new `assemble` event + path version 2 when used / `build` with `source: "assembly"` | **New event type,** with v2 stamped only when used |
| **D7** | With the layer build-up | sequential only / overlapping (each part builds layer by layer while flying) | **Sequential only in v1** |
| **D8** | Cut halves and pins flying back together | ask the cut work (OD11 B / Pin PR 2) to write assembled poses (C1) / reconstruct in Showcase / skip | **C1 in the cut work.** It also makes the Assemble view show cut models assembled. |
| **D9** | Bambu Studio's Assembly guide | port it / read its `assembly_step.json` order later / ignore | **Don't port.** Maybe read its step order later (A4). Note: we currently drop that file from Bambu 3MFs. |
| **D10** | Painted colour regions as flying parts | no / yes | **No.** They are surface shells and look hollow. |
| **D11** | Tumble in flight | off / on by default | **Off by default;** `spin` available per event or part |
| **D12** | UI wording | "Assembly", "Comes from", "Assemble into", "Connected pieces" | Owner to approve the labels in 4.2 and 4.3 |
| **D13** | Toolpath scenes | the assembly shows the model with a note / block the assembly | **Show the model with a note** (toolpaths can't move per part) |

---

## 10. Risks and unknowns

| # | risk | mitigation |
|---|---|---|
| R1 | Mutating GLVolume transforms leaks if a frame throws before restoring | Use an RAII guard object in `draw_scene` and `render_export`. A `reload_scene` resets transforms anyway. |
| R2 | The shadow fit box hides a far part's shadow, then it "pops in" | It is outside the frustum only while far away. The fit box is the rest box × 1.25, so the shadow appears before landing. Check in hand test 1. |
| R3 | `distance: auto` with a moving camera: a part starts outside the frame but the camera swings onto it | Compute it against the camera at `t0`. It is acceptable for the part to be in frame after its start (that is the flight). |
| R4 | Assembled poses from Bambu 3MFs with per-volume assemble transforms (newer Bambu) aren't read by Edge | Out of scope. Only instance-level poses are used. Note it in the report when a part's volume offset differs. |
| R5 | Connected-pieces memory on very large meshes | Off by default (only in Auto for single bodies); cap at 64 pieces, and above that fall back to whole with a note |
| R6 | Callout anchors made before this feature have no volume | Resolve through the ID pass at rest, once per video; an anchor that misses falls back to the object's first part |
