# Over-support surfaces — spec

Date: 2026-09-05 · Branch: `feat/over-support-surfaces` (from `feat/ultra-preferences` @ `e513088d61`)
Status: implemented, gated

## 1. What

Three new process settings, per object:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `over_support_surfaces` | bool | `0` (off) | Print the object's bottom faces that rest on support material like a bottom shell instead of like a bridge. |
| `over_support_flow` | float (flow ratio) | `1.0` | Flow ratio for those faces. Replaces **Bridge flow ratio** for them. |
| `over_support_speed` | float, mm/s | `0` | Speed for those faces. `0` = follow the layer's **outer wall speed**, so the face matches the perimeters framing it. Any other value is used as it is. Replaces **Bridge speed** for them. |

They live in the `Support` category and appear on the process tab's Support page, in their own
**Over-support surfaces** group between *Support ironing* and *Advanced*.

The affected faces get their own surface type (`stBottomOverSupport`), their own extrusion role
(`erBottomSurfaceOverSupport`) and their own preview feature type, **"Bottom surface over support"**,
drawn in turquoise `#29C2B8` (`{0.16, 0.76, 0.72}`) next to the existing indigo "Bottom surface".

## 2. Why

Today a bottom face lying on support with `support_top_z_distance > 0` is classified `stBottomBridge`
— the same class as a genuinely unsupported span over air. It therefore gets **Bridge flow ratio**,
**Bridge speed**, bridge density and **Thick bridges**. Those faces are usually among the most visible
surfaces of a print, and they are not bridges: they are lying on the support interface. The bridge
settings are tuned for spanning air, they are convoluted, and nothing in the UI tells the user that
changing "Bridge speed" will change how the underside of their part looks. The result is a face that
does not match the walls around it, and no obvious way to fix it without wrecking real bridges.

With the switch on, those faces are printed as what they are: a solid bottom shell, with the object's
normal bottom surface pattern and density, no thick bridges, no bridge angle detection, and a flow and
speed of their own. True bridges over air are untouched.

## 3. The classification rule, as built

`PrintObject::detect_surfaces_type` (`src/libslic3r/PrintObject.cpp`) decides, once per object:

```
over-support is possible  ⟺  over_support_surfaces
                          &&  has_support()                        (enable_support || enforce_support_layers > 0)
                          &&  support_top_z_distance > 0
```

The Z-gap condition is not a nicety: at a zero gap the fork already classifies these faces `stBottom`
(the soluble-interface rule), so there is nothing left to reclassify and this feature stands down.

Then, per support type — **the same predicate the fork already uses to decide "this bottom is fully
supported" in the soluble case, only at a non-zero gap**:

* `normal(auto)` — the generator supports every overhang it detects. With `bridge_no_support` on,
  `SupportMaterial.cpp` calls a `remove_bridges_from_contacts` that takes **no length** and drops every
  bridge it can detect from its contacts, so the feature stands down for the whole object.
* `tree(auto)`, **organic** (the default tree style; `SupportParameters.hpp` resolves `default`, `grid`
  and `snug` to organic for a tree type) — supported when `support_interface_top_layers > 0` and
  `support_critical_regions_only` is off. `TreeSupport3D.cpp` runs the same length-less removal under
  the same `bridge_no_support` gate, so the feature stands down there too. `TreeSupport::generate()`
  hands organic trees to `TreeSupport3D` before `detect_overhangs()` runs, so the length rule below
  never applies to them.
* `tree(auto)`, **classic** (`tree_slim`, `tree_strong`, `tree_hybrid`) — same two refusals, and
  `TreeSupport.cpp` drops the *bridgeable* faces whenever `max_bridge_length > 0` (the default, 10 mm),
  not gated on `bridge_no_support`: a bottom bridge whose fill surface is shorter than
  `max_bridge_length` in both X and Y gets no support and stays a bridge; a longer one is supported.
  The classifier reproduces that per-face test on the slice surface minus the wall band. (The first
  version demanded `max_bridge_length == 0` here and therefore switched itself off on every default
  tree profile - 2026-09-05 hardware finding; ef20d80316 then applied the per-face test to BOTH
  support types, which reclassified faces under `bridge_no_support` that nothing would be under -
  caught by the test suite once fff_print_tests was relinked, and corrected to the per-generator
  rule above.)
* `normal(manual)` / `tree(manual)` — support exists only where the user asked for it, so only the
  part of the face covered by a **support enforcer** (enforcer volume or painted enforcer facets,
  projected onto that layer) counts as over support.

Finally, in every case, **support blockers are subtracted**: enforcer/blocker volumes and painted
facets are projected onto the object's layers by `PrintObject::slice_support_annotations`, which uses
exactly the recipe `SupportAnnotations` uses inside the support generator (same slices, same painted
facets, same blocker expansion). What is left over after the blockers stays `stBottomBridge` and keeps
the bridge settings.

The first layer never participates: it has no lower layer, is already `stBottom`, and is on the plate.

### 3.1 Why this is a reconstruction and not the generator's own contacts

The task asked for "the support generator's contact/roof areas for that layer". Those do not exist
when the decision has to be made. `Print::process()` runs `make_perimeters → infill → ironing →
generate_support_material`: supports are generated **after** slicing and after the fills exist, so
`detect_surfaces_type` (inside `posSlice`) cannot ask the generator anything. Classifying later would
mean regenerating the fills for the affected layers, which is a different and much larger change.

So the rule is a slice-time reconstruction. What it gets exactly right: whether support exists at all,
whether the support type will place support under detected overhangs, the `bridge_no_support` /
`max_bridge_length` / `support_critical_regions_only` refusals, and the user's enforcers and blockers.

What it cannot see, and where it can therefore over-claim:

* `support_on_build_plate_only` — a face above a *top* surface of the same object gets no support, but
  the classifier still calls it over-support. Computing `buildplate_covered` needs a downward
  propagation pass the classifier does not run.
* `support_remove_small_overhang`, the sharp-tail and cantilever heuristics, and the XY trimming that
  can leave a support column narrower than the face above it. A narrow fringe of a wide face can end
  up over air while being printed as over-support.
* The support generator's own minimum-area filters.

All of these are margins of a face that genuinely is on support, and the failure mode is cosmetic
(a fringe printed as a bottom shell rather than as a bridge), not structural. If a user hits one of
them, the escape hatch is a support blocker over the fringe, which the classifier does honour.

## 4. What stays a bridge

* Every bottom face over air on an object with no support.
* Every bottom face when `over_support_surfaces` is off — **byte-identical to today**, by
  construction: the classifier is gated on the key, `expand_merge_surfaces` for the new type returns
  before it touches anything when there are no such surfaces, and `discover_horizontal_shells` still
  runs exactly three surface-type iterations.
* `stInternalBridge` / `stSecondInternalBridge` — internal bridges over sparse infill. Untouched.
* Anything under `bridge_no_support` (normal auto), under a tree configuration that will not carry the
  face, or under a support blocker.
* Anything at `support_top_z_distance == 0`, which is already `stBottom` and already correct.

## 5. Where it acts

| Stage | File | What changed |
|---|---|---|
| Classification | `src/libslic3r/PrintObject.cpp` | `detect_surfaces_type` retypes the over-support part of each bottom bridge; `slice_support_annotations` projects enforcers/blockers; `discover_vertical_shells` / `discover_horizontal_shells` carry the new type; invalidation entries |
| Surface model | `src/libslic3r/Surface.hpp/.cpp` | `stBottomOverSupport` appended to `SurfaceType` (appended, so no existing value moves), `is_bottom()` and `is_bottom_over_support()`, debug SVG colour |
| External surfaces | `src/libslic3r/LayerRegion.cpp` | its own `expand_merge_surfaces` pass with the bottom-shell expansion parameters |
| Fill | `src/libslic3r/Fill/Fill.cpp` | role `erBottomSurfaceOverSupport`; pattern/density/flow already come from the bottom-surface branch because `is_bottom()`, `is_external()` and `is_solid()` all hold and `is_bridge()` does not; ironing "iron everything" includes it |
| Role | `src/libslic3r/ExtrusionEntity.hpp/.cpp` | `erBottomSurfaceOverSupport` inserted after `erBottomSurface`; name `"Bottom surface over support"` both ways (this is the `;TYPE:` token in the G-code and the preview legend label) |
| G-code | `src/libslic3r/GCode.cpp` | flow `*= over_support_flow`; speed from `over_support_speed`, falling back to `outer_wall_speed`; `extrusion_role_to_string_for_parser` |
| Preview | `src/slic3r/GUI/GCodeViewer.cpp` | colour table entry and calibration-thumbnail visibility |
| UI | `src/slic3r/GUI/Tab.cpp`, `ConfigManipulation.cpp` | the Support-page group; the flow and speed fields grey out unless the switch is on and the switch greys out unless supports are on with a non-zero Z gap |
| Keys | `src/libslic3r/PrintConfig.*`, `Preset.cpp` | the three defs, `part_support_keys()`, the process preset's known-key list |
| Per part (Stage 5) | `src/libslic3r/PrintConfig.hpp`, `PrintObject.cpp`, `Print.hpp`, `PerimeterGenerator.hpp`, `Layer.cpp` | the three keys are `PrintRegionConfig` members, so a **part** carries them; the bottom-face classifier reads the switch per region and the wall classifier off its own region config (`PerimeterGenerator::over_support_active()`); `PrintObject::any_region_over_support()` answers the object-wide question the object config can no longer answer; `is_perimeter_compatible` names them |

`PrintConfig.cpp`'s `handle_legacy` is untouched: these are new keys, and a project that does not
carry them simply gets the defaults (switch off = today's behaviour).

`Slicing.cpp`'s `soluble_interface` path is untouched too, and deliberately: `soluble_interface` is
`support_top_z_distance == 0`, which is precisely the case this feature refuses to enter.

## 6. Support sets and groups

The three keys are in the `Support` category, so they join `support_set_keys()` automatically (that
list is derived, not hand-written — see `src/libslic3r/SupportSet.cpp`), and they were added by hand
to the curated `part_support_keys()`. A saved support set carries them, a part may carry them in its
own config, and they survive the 3MF round trip and the group resolver.

**Stage 5 (2026-09-05) made them act per part**, by moving all three from `PrintObjectConfig` to
`PrintRegionConfig`. That is the fork's own per-part mechanism for a setting that describes the
OBJECT's own extrusions rather than the support: `apply_to_print_region_config`
(`src/libslic3r/PrintObject.cpp`) copies every `PrintRegionConfig` key a `ModelVolume` carries onto
that volume's own `PrintRegion`, and `GCode::extrude_infill` applies the region's config before it
extrudes the region's infill. So a part that asks for over-support surfaces gets them with its own
flow and speed while its neighbour keeps its bridges, and a part that carries none of the three
keeps the object's values — which is why off mode cannot move. `detect_surfaces_type` reads the
switch per region; the three object-wide conditions (support exists, the Z gap is non-zero, the
generator will carry the face) stay object-wide because they are.

**The walls follow the part too** (section 8, merged after Stage 5 was cut). The reconstruction of
"there is support under here" is object-wide work and is built as soon as **any** part asks for the
feature - `PrintObject::over_support_settings()` gates on `any_region_over_support()` rather than on
an object-config key that no longer exists. The switch itself is then read per part at both consumers:
`detect_surfaces_type` ANDs it with the region it is classifying, and
`PerimeterGenerator::over_support_active()` ANDs it with `config->over_support_surfaces`, its own
region's. A part that did not ask therefore takes both generators' original code path verbatim -
the same stand-down every build with the feature off takes, which is what keeps off mode
byte-identical. The object-wide half of the predicate - support exists, the top Z gap is non-zero,
the generator will carry the face, the enforcers, the blockers - stays object-wide, because it is.

Two consequences worth naming:

- `support_set_keys()`'s derived rule grew a second class. It used to be "a `PrintObjectConfig`
  member whose category is `Support`"; it is now "a `PrintObjectConfig` **or** `PrintRegionConfig`
  member whose category is `Support`". Where a key lives decides how it ACTS, not whether it is a
  support setting.
- `Layer::is_perimeter_compatible()` names the three. Two regions differing only in G-code-level
  values are otherwise merged for perimeter generation and the merged group's extrusions are
  assigned to the first region — which is exactly why `outer_wall_speed` and
  `scarf_joint_flow_ratio` were already on that list.

**The limit, measured.** When the OBJECT-wide switch is on and two parts therefore both produce
over-support surfaces, both come out with the object's flow ratio and feedrate, even though each
part's `PrintRegion` really does carry its own values (checked directly) and the two regions are not
merged. A part that turns the feature on for itself — which is how a support group uses these keys —
does get its own flow and speed. See 2e of the support-sets plan.

## 7. Gate

* `tests/fff_print/test_over_support_surfaces.cpp` (`[OverSupport]`): off-mode role identity (no new
  role, no new surface type, bridges still bridges), on-mode role presence, feedrate == the outer wall
  speed at `over_support_speed = 0` and == the set value otherwise, extrusion-per-millimetre scaling
  with `over_support_flow`, and the four stand-down cases (`bridge_no_support`, supports off, zero Z
  gap, defaults).
* `scripts/support_group_identity.py` with the `over_support_off` corpus case: a support configuration
  that reads exactly the inputs this feature reads (`support_top_z_distance = 0.2`, interface layers),
  with the switch left at its default, compared against a baseline build that does not have the
  feature at all. It must stay within the tolerance gate along with every other off-mode case.

### 7.0 What the gate showed

`tests/data/support_corpus/onepart_ledge.3mf`, real presets (Snapmaker U1 0.4 nozzle, 0.20 Standard,
Generic PLA), normal(auto) supports at a 0.2 mm top Z distance, sliced by the shipped CLI. Per feature
type: extruding segments, extruded path length, extrusion per millimetre and the feedrates seen.

Switch **off**:

```
Bottom surface               segments=23    mm=79.1     E/mm=0.04855  F=[6300]
Bridge                       segments=26    mm=618.3    E/mm=0.02419  F=[3000]
Internal Bridge              segments=68    mm=697.6    E/mm=0.03935  F=[4500]
```

Switch **on**, `over_support_speed = 25`, `over_support_flow = 0.9`:

```
Bottom surface               segments=23    mm=79.1     E/mm=0.04855  F=[6300]
Bottom surface over support  segments=62    mm=629.3    E/mm=0.02697  F=[1500]
Internal Bridge              segments=68    mm=697.6    E/mm=0.03935  F=[4500]
```

The face moved out of `Bridge` and into the new role: `F=1500` is 25 mm/s, exactly
`over_support_speed`, where the bridge was printed at the profile's 50 mm/s bridge speed. The segment
count rises because the surface is now filled with the bottom-surface pattern at solid density rather
than at bridge density, and `E/mm` changes with it because the line is a normal solid line of the
layer height rather than a round bridging line. Every other feature type - the first-layer bottom
surface, the walls, the internal bridges over sparse infill, the support itself - is untouched, to the
segment.

Tolerance gate, candidate against a `feat/ultra-preferences` baseline that does not have the feature:
**within tolerance on all 9 off-mode cases** (including the new `over_support_off`), and the Stage 3/4
`--gate groups` cases still act, with `config_rows=0`.

### 7.1 One deviation the gate needed

The support-sets plan §3.7 could say "the only new config key never reaches `full_print_config`",
because `support_group` is a `ModelVolume` key. These three are `PrintObjectConfig` members, so they
**do** reach `full_print_config` and therefore the G-code `CONFIG_BLOCK`. `diff_configs`
(`src/libslic3r/SliceCompare/Diff.cpp`) counts a key present on one side only as a changed row, so a
baseline that has never heard of `over_support_flow` would fail the "zero changed config rows"
criterion on **every** case, for the mere existence of a new setting.

`scripts/support_group_identity.py` therefore gained `effective_config_rows()` and a small
`NEW_CANDIDATE_KEYS` list: a row is forgiven only when the key is on that list **and the baseline
value is missing entirely**. A new key whose value differs between two builds that both have it is
still a changed row, and any other one-sided key is still a changed row. This is the same kind of
narrow, named normalisation the script already applies to the `; generated by` header and the
`id:<N>` token, and it is the only thing that had to move for this feature.

## 8. Walls

Date: 2026-09-05 (second pass) — Branch `feat/over-support-walls` from `feat/ultra-preferences` @ `ef20d80316`.

### 8.1 The complaint

Hardware finding: with tree(auto) supports the preview labels the faces over support
**"Overhang wall"**, and "nothing looks uniform with the outer walls". Correct — the bottom *faces*
had been fixed by section 3, but the *walls* around them had not. A wall segment that hangs over the
lower layer is `erOverhangPerimeter` whatever is underneath it: bridging flow, `bridge_speed`, the
overhang fan, the overhang degree slowdown. Next to an outer wall printed at `outer_wall_speed` with
the normal flow it reads as a different feature — because it is one.

The three settings already existed and already meant the right thing. They now also act on walls.

### 8.2 What is a "wall over support"

A perimeter segment is retyped `erOverSupportPerimeter` ("Wall over support") when **all** of:

1. It is in the overhang half of the split the wall generator already performs — outside the grown
   lower slices. Everything inside keeps `erPerimeter` / `erExternalPerimeter`, untouched.
2. Its own region - i.e. its **part** - has `over_support_surfaces` on (Stage 5 made the switch a
   `PrintRegionConfig` key), and it lies inside the layer's **over-support region** — the same
   slice-time reconstruction the bottom-face classifier uses, factored into one place,
   `PrintObject::over_support_settings()`: some part asks for the feature, `has_support()`,
   `support_top_z_distance > 0`; an auto type that will carry its
   overhangs (tree(auto) also needs `support_interface_top_layers > 0` and
   `support_critical_regions_only` off); enforcers only, for the manual types; minus the blockers,
   projected exactly the way `SupportAnnotations` projects them.
3. It is **not** a segment the support generators refuse to carry. This is the second half of
   `PrintObject::remove_bridges_from_contacts`, which the face classifier did not need: a *straight*
   overhang segment, extended by `fw` at both ends, anchored inside the lower layer at both of those
   ends, and no longer than `max_bridge_length + 10`, is offset into a thick line and cut out of the
   generator's contacts. Nothing is printed on top of it, so it stays an overhang wall. It applies
   for the classic tree whenever `max_bridge_length > 0` — the same `bridgeable` length section 3
   already computes; normal(auto) and the organic tree never reach it (under `bridge_no_support` the
   auto pass stands down, section 3). Both call sites pass
   `break_bridge = false`, so a segment *longer* than `max_bridge_length` is **not** cut out, does
   get support, and is claimable.

The first layer never participates: it has no lower layer and sits on the plate.

### 8.3 Where the data comes from

`PrintObject::build_over_support_below()` runs once at the top of `PrintObject::make_perimeters()`
and produces one `Polygons` per layer — the region of the plane that will have support under it —
which `LayerRegion::make_perimeters()` hands to `PerimeterGenerator` beside `lower_slices`. It is
dropped again when the perimeter pass ends. For an auto support type the region is "everywhere",
represented as the layer's own bounding box grown by 10 mm (minus the blockers), so the generator
has one code path and no special case for "the whole layer"; for a manual type it is the enforcers
minus the blockers.

When no part has `over_support_surfaces` on the vector stays empty and `over_support_below()` returns
`nullptr`; and for a part that did not ask for the feature `over_support_active()` is false on its
region's config alone. Either way **both** generators take their original
code path verbatim — the same `extrusion_paths_append` calls, in the same order, with the same
arguments. That is what makes the off-mode G-code byte-identical rather than merely equivalent.

### 8.4 Both wall generators

* **Classic** (`traverse_loops`): after the existing `inside_polines` / `remain_polines` split,
  `remain_polines` is intersected with the region and the intersection is emitted with the loop's
  own `extrusion_mm3_per_mm`, `extrusion_width` and the layer height — external or internal,
  whichever this loop is — under the new role. The rest stays `erOverhangPerimeter` at
  `mm3_per_mm_overhang()` / `overhang_flow`.
* **Arachne** (`traverse_extrusions`): the region is turned into a `ClipperLib_Z` clip set and the
  loop is clipped three ways instead of two — intersection with the lower slices (normal role),
  intersection with the over-support region (new role, `ext_perimeter_flow` or `perimeter_flow`),
  difference with the union of the two (`erOverhangPerimeter`, `overhang_flow`). The two clip sets
  are disjoint by construction, so concatenating their paths is their union under the non-zero rule.

### 8.5 Flow and speed

In `GCode::_extrude`, `erOverSupportPerimeter` joins `erBottomSurfaceOverSupport`:

* flow: the path's own `mm3_per_mm` — already the normal perimeter flow of that loop — `*=
  over_support_flow`. No bridging flow anywhere in the chain: the path carries the loop's width and
  the layer height, not the bridge flow's round section.
* speed: `over_support_speed` when it is `> 0`, otherwise `outer_wall_speed`. Deliberately the outer
  wall speed for internal walls too: the point of `0` is "match the wall the user sees".

### 8.6 Not an overhang, anywhere downstream

`erOverSupportPerimeter` is **in** `is_perimeter()` and **not** in `is_bridge()`. That one choice
settles most of the list, because everything overhang-shaped keys off one or the other:

| Behaviour | Why it does not fire |
|---|---|
| bridge flow (`polygons_covered_by_spacing`, `VariableWidth`) | `is_bridge()` false |
| `" (bridge)"` description, `bridge_acceleration` | `is_bridge()` false |
| overhang / bridge fan (`check_overhang_fan`, `_OVERHANG` markers) | the markers test `erBridgeInfill` / `erOverhangPerimeter` by name; `Overhang_threshold_none` tests `is_external_perimeter()` |
| `overhang_reverse` | both generators set `steep_overhang_*` from paths whose role is `erOverhangPerimeter`; the over-support paths are split out before that test and never carry it |
| `slowdown_for_curled_perimeters`, `overhang_1_4..4_4_speed` | live inside the `enable_overhang_speed` block — see below |
| Adaptive PA `BR:` / `OV:` flags | tested by role name |
| `SupportMaterialInternal::has_bridging_perimeters` / `collect_bridging_perimeter_areas` | tested by role name, so a wall over support no longer argues for removing the support under it — which is exactly right, it was classified as over support in the first place |
| seam "prefer a non-overhang start point" | tested by role name; a wall over support is a fine place to start |

The one exception is `GCode::_extrude`'s **overhang degree classification**: it is gated on
`is_bridge() || is_perimeter()`, so a plain `erPerimeter` reaches it too. Left alone it would measure
the segment's overlap with the lower layer, find zero, grade it 4/4 and hand it `bridge_speed` —
undoing the feature. It is therefore excluded by name (`path.role() != erOverSupportPerimeter`)
rather than by dropping the role out of `is_perimeter()`, so seams, travel/retraction and
small-perimeter handling still see the wall as the wall it is.

`detect_overhang_wall` off means no overhang split at all, in either generator, so no wall over
support either — the same stand-down the overhang role has. `make_overhang_printable` reshapes the
*slices* before perimeters exist and is untouched; whatever it leaves overhanging is classified here
as usual.

### 8.7 Role plumbing

`erOverSupportPerimeter` is inserted after `erOverhangPerimeter` — an insertion, like
`erBottomSurfaceOverSupport` before it: nothing persists the numeric value, the G-code carries the
`;TYPE:` string. Added to `role_to_string` / `string_to_role` ("Wall over support"),
`GCode::extrusion_role_to_string_for_parser` ("OverSupportPerimeter"),
`GCodeViewer::Extrusion_Role_Colors` (a lighter turquoise `{0.49, 0.90, 0.86}` beside the darker
`{0.16, 0.76, 0.72}` of "Bottom surface over support") and the calibration-thumbnail visibility
flags. The only two switches over roles that have a `default`/assert are `role_to_string` and
`extrusion_role_to_string_for_parser`, and both list it.

### 8.8 Invalidation

Every key the wall classifier reads (`over_support_surfaces`, `support_type`,
`support_top_z_distance`, `bridge_no_support`, `max_bridge_length`, `support_interface_top_layers`,
`support_critical_regions_only`) already invalidates `posSlice`, which is before `posPerimeters`, so
the wall pass reruns for all of them. `enable_support` only invalidated `posSupportMaterial` (plus
`posSlice` at a zero Z gap); it now also invalidates `posSlice` when `over_support_surfaces` is on,
because `has_support()` is part of the predicate.

### 8.9 Evidence

`tests/supportgroup_test.3mf` — tree(auto), `max_bridge_length = 10`, `support_top_z_distance = 0.2`,
`support_interface_top_layers = 2`, Arachne walls, `enable_overhang_speed = 1`,
`overhang_reverse = 1`, `outer_wall_speed = 60` — sliced by the shipped CLI pinned to one CPU.
The file carries **per-object** `over_support_*` overrides, so every case below sets the key in
`Metadata/model_settings.config` as well as in `Metadata/project_settings.config`.

**Off mode is byte-identical.** Candidate against the `feat/ultra-preferences` build that does not
have the walls pass: both G-code files are 30,844,142 bytes and 1,211,808 lines, and exactly **one**
line differs — the `; generated by … on <timestamp>` header. Normalised SHA-256 is the same on both
sides (`7783e41a…4b83dda1e6`).

**On mode**, `over_support_speed = 0`, `over_support_flow = 1`:

```
                                off                        on
Overhang wall            blocks=312 moves=2292      blocks=180 moves=1410
Wall over support               -                   blocks=132 moves=882    F=3600
Bridge                   blocks=60  moves=1476      blocks=57  moves=1377
Bottom surface over sup.        -                   blocks=3   moves=144    F=3600
```

The overhang wall loses exactly what the new role gains — 132 blocks and 882 extruding moves — and
the extruded length is conserved to the hundredth of a millimetre: 999.33 mm of overhang wall off,
571.07 mm of overhang wall + 428.26 mm of wall over support on. `F=3600` is 60 mm/s, the profile's
`outer_wall_speed`, on every one of the 882 moves; the outer wall beside it runs at `F=3600` too.
The flow changes with the role, from the bridging flow to the loop's own: `E/mm` 0.046310 for the
overhang wall off, 0.027096 for the same geometry as a wall over support.

Other cases, all with the same 3MF:

* `over_support_speed = 25` → every wall-over-support move at `F=1500` (25 mm/s), and the bottom
  surface over support with it.
* `over_support_flow = 0.9` → wall-over-support `E/mm` 0.027096 → 0.024387, a ratio of 0.90001;
  the outer wall's `E/mm` is unchanged to the last digit (0.039345 both ways).
* `wall_generator = classic` → the classic path does it too: 120 blocks / 1509 moves at `F=3600`.
* `support_type = normal(auto)` → 144 blocks / 894 moves at `F=3600`.
* `support_type = tree(manual)` and `normal(manual)`, no enforcers → **zero** over-support roles of
  either kind, and `Overhang wall` back at its full 312 blocks / 2292 moves.

## 9. Hardware pass

2026-09-05 (evening), live install b0a44585c2 and later: the user confirms the over-support settings
apply as intended on the test objects (bottom faces and walls over support take the outer-wall flow
and speed and show as their own feature types in the preview). Root cause of the earlier "no
effect" report was the tree(auto) gate demanding `max_bridge_length == 0` (section 3, fixed in
ef20d80316); the walls pass (section 8) landed in the same day's build.
