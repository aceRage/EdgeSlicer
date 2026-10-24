# Push-fit connector ("plug pin"): design (2026-10-24)

Status: **design only**. No product code. Source: the owner's idea of 2026-10-24: a new Cut-tool connector
that is a separate printed pin, mostly a cylinder with a slight lip and a small side cutout, pushed into
matching holes in both cut halves. Written against origin/main at c253e9f043. It is meant to land **after
OD11** (PR #687, painting through cuts, and OD11 B, multiple dovetails plus cut recipe v5 in
`C:/Dev/wt_pkg_od11`, which this design does not touch).

Working name in the UI: **Pin** (type) with the tooltip "Push-fit pin: printed separately, lying flat, and
pushed into holes in both halves". "Push-fit" is the internal name (`CutConnectorType::PushFit`).

---

## 0. The idea in one paragraph

Today's **Dowel** already makes a separate pin and a hole in both halves, but the pin is a plain cylinder
that prints **upright** (layer lines across the pin, so it shears at the joint line), its hole is the pin
scaled up by the tolerance, the depth tolerance all lands in one half (see 1.6), and nothing holds the
halves together except friction. The push-fit pin keeps the Dowel's workflow (place, cut, get a third
object) and changes the part. It prints **lying on a flat** (layer lines along the pin, no supports). A
small **lip** on each side grips or clicks into a shallow groove. A **slot** or the flat lets it
compress and lets air out of a blind hole. The holes are **generated** with a lead-in chamfer and a
pointed end instead of a scaled cylinder, with a clearance per side.

---

## 1. How connectors work today

### 1.1 Types, styles and shapes

`src/libslic3r/Model.hpp:256`:

| enum | values | notes |
|---|---|---|
| `CutConnectorType` | `Plug=0, Dowel=1, Snap=2, Undef=3, FlexiJoint=4` | FlexiJoint was added **after** Undef so the old values stay put. The 3MF readers bound the type against `FlexiJoint`, the last value. |
| `CutConnectorStyle` | `Prism, Frustum, Undef` | Dowel is forced to Prism in the panel (`GLGizmoCut.cpp:6652`). Frustum only works for a Plug. |
| `CutConnectorShape` | `Triangle, Square, Hexagon, Circle, Undef` | Snap is forced to Circle. |

`CutConnector` (`Model.hpp:322`) holds `pos, rotation_m, radius, height, radius_tolerance, height_tolerance,
z_angle, attribs, flexi`. Only FlexiJoint has its own parameter block (`FlexiJointParams flexi`).
`ModelVolume::CutInfo` (`Model.hpp:915`) is what the **volume** carries into the cut:
`connector_type, radius_tolerance, height_tolerance, flexi`, plus `is_connector / is_processed`.

### 1.2 Panel

`GLGizmoCut3D::render_connectors_input_window` (`GLGizmoCut.cpp:6607`) shows:

- Type radio buttons: Plug / Dowel / Snap / Flexi. Flexi switches to its own panel
  (`render_flexi_joint_inputs`) and returns early.
- Style combo (Plug only) and Shape combo (not Snap).
- **Depth** with its tolerance, **Size** (diameter) with its tolerance, and **Rotation**. The values are in
  mm, and the panel halves them into `radius` / `radius_tolerance`.
- Snap only: **Bulge** and **Space** (proportions of the radius, `m_snap_bulge_proportion` and
  `m_snap_space_proportion`). These are **gizmo globals**, not per-connector values, and they are not stored
  per connector, in the recipe, or in the 3MF (`CutConnectorParas` exists in Model.hpp but nothing carries it).
- The footer has "Confirm connectors" and "Cancel".

### 1.3 Preview meshes

`GLGizmoCut3D::get_connector_mesh` (`GLGizmoCut.cpp:10060`) builds **unit** meshes that are scaled by
`(radius, radius, height)` when drawn, picked and placed:

- Prism: `its_make_cylinder(1,1)`. Frustum Plug: `its_make_frustum`. Frustum Dowel: `its_make_frustum_dowel`.
- Snap: `its_make_snap(1, 1, space, bulge)` (`TriangleMesh.cpp:1515`). This is PrusaSlicer's snap: two
  half-cylinder prongs separated by a gap of `space x r`, swelling to `(1 + bulge) x r` at mid height and
  narrowing to `0.5 x r` at the tip.
- Flexi: `flexi_preview_body(m_flexi)` in mm, normalised back to unit by its outer extent and protrusion. One
  cached shape is rebuilt on every parameter change (`update_connector_shape`, `:9759`). **The push-fit pin
  copies this pattern.**

### 1.4 From placed connectors to volumes

`apply_connectors_in_model` (`GLGizmoCut.cpp:8478`) bakes each connector before the cut:

- It re-derives `pos` and `rotation_m` from the cut surface (`connector_pos_on_sheet` and
  `connector_rotation_m`), so connectors on curved and drawn cuts stand on the **surface normal**.
- **Dowel:** for the Prism style, `height *= 2` (the Depth field is per side and the pin straddles the
  plane). It also adds `height += kerf`, and `dowels_count++`.
- **Plug / Snap:** `height += kerf`, and the centre moves to `face_lo + height/2` along the connector normal.
- **FlexiJoint:** no shift. The kerf goes into the joint's own gap.

`apply_cut_connectors` (`:10104`) then adds each connector as a **NEGATIVE_VOLUME** built from the unit mesh,
scaled by `(r, r, h)`. Flexi goes through `add_flexi_joint_volume` (CutUtils) instead, which uses mm and does
no scaling.

### 1.5 The cut (`src/libslic3r/CutUtils.cpp`)

`process_connector_cut` (`:108`):

- **Plug / Snap:** the upper half gets the volume as a NEGATIVE_VOLUME (for Snap a plain cylinder replaces the
  snap body, so the hole is round), widened by `apply_tolerance`. The lower half gets the same volume as a
  **MODEL_PART**, so the plug is printed as part of the lower half with its axis along the cut normal.
- **Dowel:**
  1. With `CreateDowels`, it clones the object (`clone_for_cut`) and adds the connector volume as a
     MODEL_PART. It **zeroes the rotation and the Z offset**, so the pin stands upright. The result goes into
     `dowels`.
  2. `apply_tolerance(volume)` scales the volume: `sf.xy += radius_tolerance` and `sf.z += height_tolerance`,
     with the radius tolerance in mm because the unit mesh is scaled by r. It then shifts the volume along the
     normal by `height_tolerance / 2`.
  3. It cuts the widened volume with the plane and adds the two pieces as NEGATIVE_VOLUMEs `_A` and `_B` in the
     halves, each nudged 0.05 mm for the preview.
- Every perform path (plane `:957`, curved `:690`, drawn `:854`) ends by putting the dowel objects through
  `reset_instance_transformation` and appending them to the result as `<name>-Dowel-<connector>`.
- `perform_by_contour` (`:1098`) adds them as objects 2..n.

`Plater::apply_cut_object_to_model` (`Plater.cpp:22834`) loads the new objects and selects them. **Arrange is
commented out**, so a dowel appears where the object stood, inside the halves' footprint, and the user
arranges it by hand.

The gizmo refuses to open on a dowel object (`on_is_activable`, `:5775`). The recipe is not attached to dowel
objects (`:9497`): they are recognised by "one volume, a cut connector of type Dowel".

### 1.6 Known Dowel faults (they matter for the pin)

- **The depth tolerance is one-sided.** `apply_tolerance` adds `height_tolerance` to the whole pin length and
  then shifts the hole volume by `height_tolerance / 2` along the normal for **every** type. For a Dowel,
  which straddles the plane, that gives one hole all of the extra depth and the other hole none. Upstream
  this is OrcaSlicer issue #14299, fixed by **open** PR OrcaSlicer#16020 (shift only Plug/Snap), with
  tests-only PR OrcaSlicer#16514 (`test_cut_connectors.cpp`) on top. Our tree has the same code. BambuStudio
  #11148 (closed) reported the same symptom. **The pin must not inherit this**: it builds its holes per side
  in mm (2.3), so the fault cannot reach it. Porting #16020 for the Dowel is a separate, small job.
- **Blind-hole ceilings sag.** When a half is printed cut face down, the far end of the hole is a bridge.
  OrcaSlicer #11186 (open feature request) asks for domed or pointed hole ends. The pin's holes get a pointed
  end (2.3).
- **The hole can close on layer 1.** OrcaSlicer #14512 (open): a connector hole closes on the first layer when
  the half is laid cut-side down. The 0.05 mm preview nudge is involved. The pin's hole volume therefore
  starts **0.2 mm outside** the cut face (2.3), and #14512 goes into the hand test.

### 1.7 Recipes and 3MF

- `CutRecipeConnector` (`CutRecipe.hpp`) mirrors `CutConnector`, including `flexi`. It is stored in the
  recipe as the **definition** (before `apply_connectors_in_model`), so a re-cut regenerates the volumes.
- 3MF: `Metadata/cut_recipe.xml` writes one `<connector>` per definition (`bbs_3mf.cpp:9629`). Flexi adds
  `flexi_*` attributes. The reader (`:3221`) clamps `type` to `0..FlexiJoint`, and any **unknown value
  becomes 0 = Plug**.
- `Metadata/cut_information.xml` stores the cut_info of each connector **volume** (`:8563`). The reader
  (`:2431`) rejects types above FlexiJoint with `add_error(...)`. That is logged only and is not fatal, and
  the volume keeps no cut_info.
- Version history (`CutRecipe.hpp`): 1 initial; 2 stroke chains; 3 drawn-cut surface model; 4 extension
  angle and signed lip angle (**main**); 5 multiple dovetails (OD11 B, in flight). **The push-fit pin is
  version 6.** See 5.3 for why the bump is needed and not optional.
- Re-edit (`ObjectList::edit_cut`, `GUI_ObjectList.cpp:3537`) removes every object that shares the cut's
  `cut_id`. That includes dowel objects, because they come from `clone_for_cut`, so a re-cut regenerates the
  pins rather than leaving orphans. **Verify this in PR 2** with a test (6.1 T7).

### 1.8 Which cut surfaces take which connectors

- Flat, curved (phase 4) and drawn (phase 2) cuts all take Plug, Dowel and Snap. Connectors stand on the
  surface normal at their point.
- Tongue and groove takes no connectors (`apply_connectors_in_model` returns early).
- Flexi uses a flat cut only.
- **The pin follows the Dowel everywhere.**

### 1.9 Flexi: the template to copy

Flexi is the precedent for "a connector with its own parameter block, generated in mm, real geometry":

- `FlexiJointParams` is in `CutConnector`, `CutInfo`, `CutRecipeConnector`, `cut_information.xml` and
  `cut_recipe.xml`.
- The bodies come from `its_make_revolved(profile_rz, sectors)` (`TriangleMesh.hpp:361`, watertight with
  outward normals) plus a Clipper offset for the clearance.
- The booleans are `flexi_boolean()` (`CutUtils.cpp:356`): Manifold (`MeshBoolean::mfd`) first, mcut as the
  fallback.
- `flexi_validate()` returns a user-facing string. Tests are in `tests/libslic3r/test_flexi_joint.cpp`
  (watertight, 3MF round trip, guards).

The pin takes the same shape: `PushFitParams`, `PushFit.hpp/.cpp`, `pushfit_validate()`, and
`test_pushfit.cpp`.

---

## 2. Geometry

### 2.1 Frame and parameters

Connector frame (the same as Flexi): the cut surface is `z = 0`, the pin axis is +Z (the connector normal),
the **upper** half is `z > 0` and the **lower** half is `z < 0`. `z_angle` (the panel's Rotation) turns the
pin about its axis. For a pin it only decides where the flat faces, which matters for the vent (2.4). All
lengths are in mm.

```
struct PushFitParams {
    // body
    float diameter{ 6.0f };        // D, nominal pin diameter (= Size field)
    float length_upper{ 7.0f };    // Lu, engagement into the upper half (from that half's cut face)
    float length_lower{ 7.0f };    // Ll, engagement into the lower half; "Same both sides" ties it to Lu
    float end_chamfer{ 0.5f };     // lead-in at each pin end, 45 deg, axial = radial
    // lip (one per side, mirrored about the cut plane)
    bool  lip{ true };
    float lip_height{ 0.25f };     // radial, beyond D/2
    float lip_width{ 1.0f };       // axial length of the lip crest + flanks
    float lip_position{ 0.6f };    // centre of the lip as a fraction of the side's length, from the cut face
    // cutout
    enum class Cutout : int { None = 0, Flat = 1, Slot = 2, FlatAndSlot = 3 } cutout{ FlatAndSlot };
    float flat_depth{ 0.9f };      // f, chord depth of the printing flat (default 0.15 D, see 2.4)
    bool  flat_both_sides{ true }; // a second flat opposite (symmetric pin, two vents)
    float slot_width{ 1.0f };      // w, slit width, through the pin
    float slot_length{ 4.0f };     // Ls, slit length from each end
    // holes
    float clearance_upper{ 0.10f };// radial clearance, upper hole
    float clearance_lower{ 0.10f };// radial clearance, lower hole
    float depth_clearance{ 0.4f }; // extra depth per hole beyond the pin end (air + squish)
    bool  hole_groove{ false };    // matching groove for the lip (detent) vs plain hole (interference)
    float mouth_chamfer{ 0.4f };   // 45 deg chamfer at each hole mouth (also eats elephant foot)
    bool  pointed_end{ true };     // hole ends in a 90 deg cone instead of a flat bridge
    int   preset{ 1 };             // 0 S, 1 M, 2 L, 3 Custom: UI only, stored so re-edit shows it
};
```

The connector's own `radius` stores D/2 and `height` stores `Lu + Ll` (the pin's length), so the existing
bbox, picking and overlap code works on the pin unchanged. `radius_tolerance` and `height_tolerance` are
**unused** by a pin: the per-side clearances replace them, and the generic `apply_tolerance()` is never
called on a pin volume.

### 2.2 The pin body

Build it in the **connector frame**, then move it to the print pose (2.5).

1. **Profile and revolve.** Write a closed `(r, z)` profile and pass it to `its_make_revolved(profile, 96)`.
   The profile runs from the axis at `z = -(Ll + kerf_lo)` to the axis at `z = +(Lu + kerf_hi)`:
   - Body radius `D/2`.
   - A 45 degree end chamfer of `end_chamfer` at each end.
   - With `lip`, a trapezoid on each side centred at `z = +/-(face + lip_position x L_side)`. It has 45 degree
     flanks (35 degrees is the option if a click proves too hard to push) and a crest of
     `lip_width - 2 x lip_height`.

   The pin is a solid of revolution, so the profile is the whole body. It is watertight by construction (the
   same generator the Flexi tests already check: `test_flexi_joint.cpp:278`).

   kerf_lo and kerf_hi are the faces from `cut_thickness_faces()`, so the pin spans a kerf the way a Dowel
   does today.
2. **Flats.** Cut along a plane parallel to the axis at distance `D/2 - f` with `cut_mesh()` (the same
   half-space split the kerf uses), and keep the inner side. `cut_mesh` caps the section, so the result stays
   watertight with no boolean. With `flat_both_sides`, do the same on the opposite side. A flat cuts through
   the lip as well, which is what we want: the lip is a ring with two chords missing.
3. **Slot.** This is the only boolean. Subtract a stadium prism: a box `w x (D + 2) x Ls` plus a cylinder of
   diameter `w` at its inner end, so the slit has a **round end** and no crack starter. The prism goes
   through the pin perpendicular to the flats, so the flats stay intact and the two prongs sit side by side
   on the bed (2.5). Do this at each end; with `Ls` below `L_side` the two slits never meet. Use the
   `flexi_boolean()` chain (Manifold, then mcut). Lift that helper into a shared `mesh_boolean_chain()` in
   CutUtils or MeshBoolean so Flexi and Pin share it. Both operands are convex-ish and watertight, which is
   Manifold's easy case.

   *Analytic alternative* (no boolean): split the pin into the middle (solid) segment and the end segments,
   cut the end segments with two half-spaces `|x| > w/2`, and stitch. Stitching coplanar caps by hand is how
   the "watertight but self-intersecting" faults from the bevel work started (memory: edit-bevel-geometry).
   **Recommendation: use the boolean.** It runs once per pin at cut time, not per frame. If the boolean
   fails, fall back to **no slot** with a notification. Do not fail the cut.
4. **Checks.** Run `its_num_open_edges() == 0` (the check the Flexi tests use) and check that the volume is
   positive. On failure, use a plain flatted pin and log it.

Cost: 96 sectors x roughly 20 profile points is about 4k triangles before the boolean, which is trivial.

### 2.3 The holes

Each half gets **its own** NEGATIVE_VOLUME in mm. It is not a scaled copy of the pin and it is not cut with the
plane:

- Profile (upper hole; the lower one is the mirror with the lower values):
  - `r = D/2 + clearance_side` from `z = face - 0.2` (it starts **0.2 mm outside** the cut face, so the hole
    opens on layer 1; see 1.6, #14512) to `z = face + L_side + depth_clearance`.
  - Then either a 90 degree cone to the axis (`pointed_end`; it prints as a 45 degree overhang with no bridge)
    or a flat end.
  - At the mouth, a 45 degree flare of `mouth_chamfer`.
  - With `hole_groove`, a trapezoid groove at the lip's position, with depth `lip_height` and width
    `lip_width + 2 x clearance`. Its flanks are 45 degrees, so the groove ceiling also prints without support
    when the half's hole axis is vertical.
- The hole is **round**, with no flats. The pin's flats leave a D-shaped gap, and that gap is the **air
  vent** along the full length.
- The `_A` and `_B` naming and `cut_info` (type PushFit, processed) are the same as for the Dowel, so the
  object list and `cut_information.xml` treat them as connector volumes.

This replaces the Dowel's "scale by tolerance, then plane-cut" path for pins only. The per-side clearance
exists because the two halves often print in different orientations (one cut face down, the other flipped or
on its side), and holes print undersized in different ways (3.1).

### 2.4 Cutout: what the flat and slot are for

- **Flat (printing flat).** A round pin lying on its side overhangs at its underside. With a flat of chord
  depth f, the steepest overhang is `phi = asin(1 - 2f/D)` from vertical. For 45 degrees or less you need
  `f >= D (1 - sin 45) / 2 = 0.146 D`. **Default f = 0.15 D** (0.9 mm on a 6 mm pin): no supports, and a solid
  bottom face. A second flat on top makes the pin symmetric, so it goes in either way up. It also gives two
  vents and removes the stair-stepped crown of a lying cylinder, which is the least accurate surface.
- **Air relief.** A near-fitting pin in a **blind** hole is a piston. The flats (with `depth_clearance`) let
  the air out, which is why the pin does not spring back out of a tight hole.
- **Slot (split pin).** The slot lets the lip side flex inward by about the slot width at the tip, so the lip
  can be proud of the hole (an interference of about `lip_height - clearance`) without the user forcing it.
  It also keeps a slightly oversize print from jamming. Without a slot the lip has to be very low (0.1 to
  0.15 mm) or the hole has to have the groove. The slot is vertical in the print pose: the prongs stand side
  by side and nothing bridges. A slot parallel to the bed would make the upper prong a bridge, so it is not
  offered.

### 2.5 Print pose and placement on the bed

- **Lying (default).** Rotate the pin so its axis lies along the bed's X and one flat is the bottom face at
  `z = 0`. Make the dowel object's volume mesh **in this pose**, with no instance rotation trick. The
  existing dowel code zeroes the rotation, which for a pin gives "upright", so the pin path sets its own
  volume mesh and leaves the instance unrotated.
- **Upright (option).** The axis is vertical and stands on one end chamfer. Round holes and pins print most
  accurately in XY, but layer lines then cross the pin at the cut plane, where the shear load is. Offer it
  for fits that must be very accurate in a soft material, and warn about strength in the tooltip.
- **Placement.** Today a dowel appears inside the halves' footprint. For pins: lay them in a row along +X,
  `5 mm` beyond the max X of the source object's instance bounding box. The spacing is `pin width + 3 mm`, and
  the row wraps when it reaches the bed edge. Then load them through `load_model_objects`, which already drops
  objects to the bed. This is deterministic and needs no arrange job. Optional: the owner may prefer to run
  the arrange job on just the new objects (decision D6).
- Name: `<object>-Pin-<n>`. One object per pin, the same as a Dowel, so each can be moved, duplicated or
  given a filament.

### 2.6 Validation (`pushfit_validate`, its messages shown in the panel the way Flexi's are)

These block the cut:

- `D >= 3`.
- `L_side >= 2` on both sides.
- `lip_height < D/4`.
- `slot_width >= 0.6`, and every prong at least 2 lines wide: `(D - w)/2 - f >= 0.9` (2 x 0.45 line width).
- `slot_length <= L_side - lip_width/2`, so the slot does not reach the cut plane or the middle segment.
- `flat_depth < D/3`.

These warn:

- The hole's depth plus wall exceeds the half's local thickness along the normal. Check with a raycast from
  `pos` along `+/-n` (the curved-cut connector code already raycasts).
- The clearance is below `flexi_clearance_floor(nozzle)` (0.75 x nozzle).
- Lip without slot and without groove, with an interference above 0.15 mm ("will need force; add a slot or
  groove").

---

## 3. Printability and strength

### 3.1 FDM fit facts (from published guidance, facts only)

- **Clearances.** Maker guides give roughly **0.2-0.3 mm total gap** for an assembly fit and 0.3-0.5 mm for a
  free slide. Interference press fits of plastic or metal pins into printed holes are quoted at about 0.1-0.3 mm
  ([zbotic](https://zbotic.in/3d-printing-tolerances-designing-gaps-for-press-fits-threads-and-snap-fits/)).
- **Holes print small.** Printed holes come out undersized, so the recommended practice is to enlarge the hole
  rather than shrink the pin ([Sovol](https://www.sovol3d.com/blogs/news/fdm-3d-printing-tolerances-clearances-how-to-design-parts-that-fit)).
  A PLA study on a Bambu A1 mini measured holes about 0.1 mm under size and pins about 0.05 mm over, and
  suggests 0.15-0.25 mm of added clearance for a slip fit ([U-M Deep Blue](https://deepblue.lib.umich.edu/data/downloads/8s45qb25h)).
- **Test first.** Several guides recommend a small test coupon, because material, orientation and first-layer
  squish all change the fit ([UAL wiki](https://wiki.cci.arts.ac.uk/books/digital-fabrication-lab/page/design-the-clearance-and-tolerance)).
  That argues for the S/M/L presets plus a printed test (section 6.3).
- **Layer direction.** FDM parts are weakest between layers (well established; no number is claimed here).
  That is why the pin lies down: the load across the joint is shear perpendicular to the pin axis, and lying
  down puts that load across continuous extrusions rather than between layers.
- **Prior art.** The widely shared printable "pin connector" designs (split, flat-sided pins printed lying
  down, with a lip at each end) use the same three features. They were not re-checked in this session, so
  treat this as background, not a citation.

### 3.2 Defaults and presets

All values in mm. Each side length L is engaged into one half.

| Preset | D | L per side | lip h / w | flat f | slot w x Ls | clearance (radial) | depth clr | mouth chamfer |
|---|---|---|---|---|---|---|---|---|
| **S** | 4 | 5 | 0.20 / 0.8 | 0.6 | 0.8 x 3 | 0.10 | 0.4 | 0.3 |
| **M** (default) | 6 | 7 | 0.25 / 1.0 | 0.9 | 1.0 x 4 | 0.10 | 0.4 | 0.4 |
| **L** | 8 | 10 | 0.30 / 1.2 | 1.2 | 1.2 x 6 | 0.12 | 0.5 | 0.5 |

- **Auto size** (the same idea as `flexi_auto_size`): pick the largest preset whose
  `D <= 0.5 x the inscribed radius of the cut section at pos` **and** whose `L + depth_clr + 1.2 mm wall` fits
  the raycast thickness on both sides. If none fits, offer S with a warning.
- **Radial clearance 0.10 mm** (0.2 mm on the diameter) is the tight end of the assembly range. It is right
  here because the slot supplies the compliance and the lip supplies the retention. Without a slot, the M
  preset's suggested clearance becomes 0.15.
- **Material.** PLA works, but its prongs are brittle when overstressed. PETG and ABS/ASA flex better, and the
  tooltip should say so. On a 0.2 mm nozzle the presets still hold; on a 0.6 mm nozzle S fails the
  prong-width rule, and the validation blocks it.

---

## 4. UX in the connector panel

- **Type row:** Plug / Dowel / Snap / **Pin** / Flexi. "Pin" is a new radio button, and its tooltip is in
  section 0. Choosing it hides Style and Shape (a pin is always round) and shows the Pin block. Do it the way
  Flexi does: an early-return `render_pushfit_inputs()`, so the generic Depth/Size tolerance sliders never
  appear for pins. Their meaning would be wrong.
- **Pin block, top to bottom:**
  1. **Preset:** S / M / L / Auto / Custom. A preset fills every field below. Editing any field switches the
     selection to Custom.
  2. **Diameter**, **Length** with "Same both sides" (on by default). With that box off: **Upper length** and
     **Lower length**.
  3. **Fit**: radial **Clearance**, with "Same both sides" and Upper/Lower when off. Also **Groove for lip**
     (a checkbox).
  4. **Lip**: on/off, **Height**, **Width**, **Position** (%).
  5. **Cutout**: None / Flat / Slot / Flat + slot. **Flat depth**, **Both sides**, **Slot width**, **Slot
     length**. Each field shows only when its cutout uses it.
  6. **Print**: Lying (default) / Upright.
  7. "**Advanced**" collapsible: end chamfer, mouth chamfer, depth clearance, pointed hole end, lip flank
     angle.
  8. **Rotation**: the existing field. It turns the flats about the axis.
- **Units.** mm and inches follow the panel's existing inch handling: OD11 just added inches to the dovetail
  panel, so reuse it.
- **Preview.**
  - While placing, the connector draws the **real pin** (the cached shape, rebuilt on change and normalised
    to unit like Flexi), in the Dowel colour, plus a translucent **hole envelope** on each side, so the
    per-side lengths are visible against the part.
  - The picking proxy is the Dowel's: height 0.05 at the plane.
  - A small 2D inset (an ImGui draw list) shows the pin's side profile with lip, flat and slot. It is cheap,
    and it is the clearest way to show "Position" and "Slot length". Owner decision D5.
- **Undo:** each field edit is one snapshot, as the dovetail Count/Gap fields do after OD11.
- **Multiple pins:** you place several connectors as today. Selecting several and editing applies to all
  selected (`apply_selected_connectors`). Each connector keeps its own `PushFitParams`.
- **After the cut:** a notification says "N pins added beside the object (print lying flat)", with a hyperlink
  that selects them.

---

## 5. Integration plan

### 5.1 New and changed code

| Where | Change |
|---|---|
| `libslic3r/PushFit.hpp/.cpp` (new) | `PushFitParams`, presets, `pushfit_auto_size`, `pushfit_validate`, `pushfit_pin_mesh(p, kerf_lo, kerf_hi, Pose)`, `pushfit_hole_mesh(p, side, kerf_face)`, `pushfit_preview_body(p)`. Pure geometry with no Model dependency, so tests drive it directly. |
| `Model.hpp` | `CutConnectorType::PushFit` **appended after FlexiJoint** (= 5). `is_pushfit_connector_type()`. `PushFitParams pushfit` in `CutConnector` and `CutInfo` (cereal, copy constructor, `operator==`). |
| `CutUtils.cpp` | `process_connector_cut`: a PushFit branch (pin object in the print pose, two hole volumes from `pushfit_hole_mesh`, no `apply_tolerance`). The shared `mesh_boolean_chain()` lifted from `flexi_boolean`. The dowel-object loops in the plane, curved and drawn paths and `perform_by_contour` also accept pins (rename `dowels` to `separate_pins` in a comment, not in the code, to keep the diff small). Placement row (2.5). |
| `GLGizmoCut.cpp/.hpp` | Type radio button (`m_connector_types` gains an entry at index 5). `render_pushfit_inputs`, `m_pushfit`, `sync_pushfit_params`, `get_connector_mesh` and `update_connector_shape` branches, `apply_connectors_in_model` (a PushFit counts like a Dowel and needs no centre shift, because the pin is built straddling the faces), colours, `on_is_activable`, and the recipe "is_dowel" filter (`:9497`) extended to pins. |
| `bbs_3mf.cpp` | `cut_information.xml` and `cut_recipe.xml`: write `pushfit_*` attributes only for pin connectors. Raise both type range checks to `PushFit`. Clamp every read value to the validation ranges, because the file is untrusted. |
| `CutRecipe.hpp/.cpp` | `CutRecipeConnector::pushfit`, conversions to and from the model, mirror (`cut_recipe_mirrored`: a pin is round, so no change apart from the flats' `z_angle`, which negates like a shaped connector), `CutRecipeVersion = 6`. |
| `GLGizmoAdvancedCut.cpp` | Untouched (BambuStudio-style gizmo). Check that it never sees a pin. |

### 5.2 Why a new type, not a Dowel style

The cheaper alternative is to keep `type = Dowel` and add `CutConnectorStyle::PushFit`. Upstream OrcaSlicer,
BambuStudio and Prusa would then read our pins as dowels, which is nearly right. The cost is that every
Dowel-specific branch has to grow a style check, and the panel's Style combo, which is greyed out for Dowel,
would have to become the type switch.

A new type keeps the code paths separate (the Flexi precedent) and makes the 3MF unambiguous. The halves still
open everywhere because the holes are ordinary negative volumes and the pin is an ordinary object. **The
recommendation is a new type**, but it is a decision (D1).

### 5.3 Recipe version 6 (after OD11's 5)

- **New fields only, no changed meanings.** A version 5 or older recipe loads with no pins and re-cuts
  identically.
- **The bump is mandatory, not cosmetic.** An older reader clamps an unknown connector type to 0 (Plug)
  (`bbs_3mf.cpp:3252`). A v5 build reading a pin recipe would therefore silently re-cut the pins as **plugs**,
  which is exactly the "Edit cut makes a different cut" failure the version gate exists to prevent. With 6,
  an older build refuses the recipe: the halves load, but "Edit cut" is not offered.
- **Ordering.** If OD11 B has not merged when pin PR 2 is ready, rebase the version history comment onto
  OD11's entry 5 and take 6. Do not take 5. The PR-2 brief must say "check origin/main's CutRecipeVersion
  first".
- **Undo/cereal.** `CutRecipeConnector::serialize` and `CutConnector::serialize` gain `pushfit`. That is safe,
  because cereal here is same-binary only (the comment in `CutRecipeStroke` explains why).

### 5.4 Interop

- **Older EdgeSlicer, Orca, Bambu or Prusa opening a file with pins.** The halves load with their hole
  negative volumes. The pins load as plain objects. `cut_information.xml` type 5 is rejected with a logged,
  non-fatal error (our older builds) or ignored.
- **BambuStudio's numbering.** Current BambuStudio master has `CutConnectorType { Plug, Dowel, Snap, Thread,
  Undef }` (commit 236bfee2e, 2026-03-12, "Localized Cuts and Thread tool"). Its Thread is **3**, which is our
  `Undef`, and its `Undef` is 4, our FlexiJoint. That is an existing interop wrinkle, not something the pin
  creates. Do **not** pick 3 for the pin. Value 5 collides with nothing upstream today.

---

## 6. PR split, tests, hand test

### 6.1 PR split

1. **PR 1, geometry (libslic3r only, no UI).** `PushFit.hpp/.cpp`, the shared boolean helper (moved from
   `flexi_boolean`; Flexi behaviour unchanged), and `tests/libslic3r/test_pushfit.cpp`. Build: test-only
   `libslic3r_tests` (it can take the Mac slot). No G-code change, so no gate.
2. **PR 2, model, cut and 3MF (after OD11 B merges).** Enum, `CutInfo`, `CutUtils` branch, placement row,
   both 3MF files, recipe version 6, and round-trip and re-cut tests. It also includes an optional, separate
   commit that ports OrcaSlicer#16020 (symmetric Dowel depth tolerance) together with its tests from #16514,
   because the pin tests sit next to them. If the lead prefers it as its own port package, split it out
   (decision D7). Build: `libslic3r_tests`. Run a G-code gate only if #16020 is included: it changes the hole
   geometry of existing dowel projects, but the gate's standard set has no connectors, so expect identity.
3. **PR 3, panel and preview.** The Pin type, panel, presets, preview and notification. Build: the full app
   plus a test copy for the owner's hand test.

### 6.2 Unit tests (`[PushFit]`)

- **T1 watertight.** For each preset x cutout (None/Flat/Slot/FlatAndSlot) x lip on/off x groove on/off: the
  pin mesh and both hole meshes have `its_num_open_edges() == 0` and positive volume, and have no
  self-intersections (use the valid status Manifold reports when it imports the mesh).
- **T2 dimensions.**
  - Pin bbox: length `Lu + Ll + kerf`.
  - Diameter across the round: `D + 2 x lip_height`.
  - Across the flats: `D - f` (one flat) or `D - 2f` (two flats).
  - Slice the pin at a lip z: the polygon's max radius is `D/2 + lip_height`.
  - Slice inside a slot: two polygons with a gap of `w`.
  - Hole: radius `D/2 + c_side`, depth `L_side + depth_clearance` (plus the cone), and it starts 0.2 below
    the face.
- **T3 print pose.** The pin object's mesh min z is 0, the bottom face is a planar region of width `>= D - 2f`
  (the flat), the axis is along X, and the max overhang angle of the underside facets is at most 45.5 degrees.
- **T4 fit.** Pin minus the upper hole (a boolean): empty when `clearance > lip_height` and there is no groove
  overlap; non-empty with the volume expected from the lip ring when the lip interferes; empty with
  `hole_groove`. This checks that the groove is where the lip is.
- **T5 cut.** Cut a 30 mm cube with one M pin on a flat cut. The result is 3 objects; each half has exactly
  one PushFit negative volume, and their depths match `Lu` and `Ll` (asymmetric lengths tested). Repeat with a
  0.5 mm kerf (the pin length grows by the kerf, each hole depth stays the same from its own face) and on a
  curved sheet (the pin axis follows the sheet normal).
- **T6 3MF round trip.** `cut_information.xml` and `cut_recipe.xml` with a pin: every `PushFitParams` field
  survives. Values out of range in a crafted file are clamped. A v5 file loads unchanged.
- **T7 re-edit.** Cut with a pin, reopen from the recipe, change D, re-cut. The old pin object is gone, one new
  pin exists, and its D is the new value. "Copy cut to..." onto a second object gives a pin for that object
  too. Mirroring keeps the type and negates `z_angle`.
- **T8 guards.** `pushfit_validate` rejects each rule in 2.6. A boolean failure is simulated by a degenerate
  slot (width 0 is caught by validation; force it through the test hook) and must fall back to no slot.

### 6.3 Hand test and print plan (owner)

1. In a test copy, cut a 40 mm cube with S, M and L pins on one plane. Check the panel, presets, preview, the
   notification and pins beside the object, lying flat. Re-edit, change the preset, re-cut.
2. Slice: the pins need no supports, the hole mouths are open on layer 1 with the halves cut-face down (#14512
   check), and the hole ends are cones with no bridge.
3. **Fit coupon** (print once per material, PLA and PETG): a 20 x 20 x 10 mm two-part block with M pins at
   radial clearance 0.05 / 0.10 / 0.15 / 0.20, with and without a slot, with and without a groove. Record which
   ones push in by hand, which hold when the block is shaken, and which come apart by hand. The outcome sets
   the shipped defaults (D3).
4. Strength sanity check: the same pin printed lying and upright, bent by hand at the joint line. Expect the
   upright one to snap at a layer line.
5. On a curved cut and a drawn cut, place one pin each and check that the axis follows the surface and the
   halves mate.
6. Open the project in the previous release: the halves load, the pins are separate objects, and "Edit cut" is
   not offered.

---

## 7. Upstream check (facts only)

| Project | What exists | Port? |
|---|---|---|
| **OrcaSlicer** | Plug/Dowel/Snap only. Open: #16020 (symmetric dowel depth tolerance, fixes #14299), #16514 (first connector unit tests), feature request #11186 (domed/pointed dowel holes), bug #14512 (hole closed on layer 1 cut-side down). Nothing like a lipped or split separate pin. #5649 (sliding dovetail connector) is closed. | Port #16020 + #16514 (D7). #11186 is answered by the pin's `pointed_end`; we could also offer it for Dowels later. |
| **BambuStudio** | master adds a **Thread** connector type (value 3) with a preview `its_make_thread` (2026-03-12). Open requests: #10803 (magnet/insert cavities in the connector tool), #8110 (connector editing). #11148 (no depth tolerance on dowels) is closed. | Nothing pin-like. The Thread numbering clash is noted in 5.4. Magnet cavities (#10803) is a related idea for later. |
| **PrusaSlicer** | The origin of Plug/Dowel/Snap (`its_make_snap`). #9911 (offset and taper options for connectors) and #13145 (dovetail + dowel) are closed. #15833 (3.0 alpha connector first-layer bug) is open. | Nothing pin-like. |
| **Snapmaker Orca** | A GitHub search of Snapmaker/OrcaSlicer found no connector issues or PRs. It inherits Orca's set. | Nothing. |

**Conclusion:** no upstream push-fit/lipped-pin connector exists to port. The pin is EdgeSlicer's own. The
geometry reuses our Flexi machinery (`its_make_revolved`, the Manifold chain), not upstream code.

---

## 8. Risks

- **The boolean on the slot.** Manifold is robust for these operands, and the fallback is "no slot" plus a
  notification. It never blocks the cut.
- **Prongs of PLA snapping.** The validation enforces a minimum prong width, and the tooltip points brittle
  materials to Flat-only plus a groove.
- **A blind hole too deep for a thin part.** The raycast warning covers it. The Dowel has the same problem
  today with no warning.
- **Merge order with OD11.** Recipe version 6 and the `m_connector_types` index both touch files OD11 B
  changes (`CutRecipe.hpp`, the 3MF recipe writer, the gizmo). PR 2 and PR 3 start from main **after** OD11 B
  merges.
- **Interaction with painting (OD11 A, #687).** The pin object is a fresh volume with no paint, which is
  correct. The halves keep their paint through the cut by #687's path, and the hole negative volumes do not
  affect painting. T5 should assert that the halves' paint survives when #687 is in main.

---

## 9. Owner decisions

- **D1. Type or style.** A new connector type "Pin" (recommended: clean code paths, unambiguous files), or a
  Dowel style "Push-fit" (upstream would see the pins as dowels).
- **D2. Name in the UI.** "Pin" (recommended), "Push-fit pin" or "Plug pin". Note that "Plug" already means
  the integral connector.
- **D3. Default fit.** Lip and slot with a plain hole (recommended: compliant grip, tolerant of printer
  variation), or lip with a matching groove (a definite click, but sensitive to clearance), or flats only
  (a plain friction pin). The shipped clearance is set after the fit coupon (6.3).
- **D4. Lips per side.** One lip per side, mirrored (recommended, as the owner sketched), or a single central
  lip at the joint line (simpler, but it does not hold both halves).
- **D5. Preview extras.** The 2D side-profile inset and the hole envelopes in the 3D preview: both (recommended),
  or the 3D only.
- **D6. Pin placement.** A deterministic row beside the object (recommended), or run Arrange on the new objects.
- **D7. Port OrcaSlicer #16020 + #16514** (symmetric Dowel depth tolerance and connector tests) inside pin PR 2,
  or as its own small port package.
- **D8. Print pose default.** Lying on the flat (recommended), with Upright as an option. Or offer no Upright
  at all.
- **D9. Asymmetric lengths and clearances per side** in the first release (recommended, behind "Same both
  sides"), or symmetric only, with the fields left for later.
