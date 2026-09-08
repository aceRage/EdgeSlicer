# Hinge — a print-in-place pin hinge for the Cut gizmo

Research + design spec. No code in this branch. Builds on `docs/superpowers/specs/2026-09-07-flexi-joint.md`
(the Flexi connector family: `CutConnectorType::FlexiJoint`, `src/libslic3r/FlexiJoint.{hpp,cpp}`,
`its_make_revolved` / `its_make_swept_loop`, Gap, real Manifold booleans with an mcut fallback) and
assumes the concurrent fix that gives every connector a `z_angle` "Rotation" about the cut normal
(the field already exists on `CutConnector` at `src/libslic3r/Model.hpp:322`; the chain link's
`tilt_angle` is the joint-local analogue, not the connector-level one — Phase 3 of the flexi spec
flags "the hinge axis is fixed" as exactly this gap for Chain link).

## Recommendation

Add **Hinge** as a fourth `FlexiJointKind` (or, more precisely, a sibling family under
`CutConnectorType::FlexiJoint` — the male/female-per-half plumbing, the Gap parameter, the
gap-closing guard, and the "forced Cut to parts" behavior all apply unchanged). Phase 1: a
**fixed odd knuckle count** (default 3, so the barrel is self-centring), **alternating barrels
with an integral pin belonging to the lower half**, **auto-placed flush at the edge of the cut
contour** in the direction the Rotation angle's in-plane axis points away from, **horizontal
axis only** (round barrels, no teardrop needed — see §1's correction of the brief's framing),
sized from Gap/nozzle-diameter defaults. This is the smallest change that produces a hinge people
can actually print: it reuses `its_make_revolved` for each barrel/pin pair exactly as Double Ring
reuses it for the lip, reuses the Gap/clearance guard machinery verbatim, and needs exactly one
new UI concept (edge placement) beyond what Flexi already has. Vertical-axis teardrop profiles,
fold stops, and N-along-a-wide-cut are real but strictly harder and are Phase 2/3.

---

## 1. What print-in-place hinges actually need on FDM

**Correction to the brief's framing.** The brief poses the horizontal-axis / vertical-axis
choice as "round barrels are fine horizontal, vertical needs teardrop to avoid sag." It is the
other way around. A pin whose axis is **vertical** (parallel to the printer's Z) is sliced as a
stack of plain circles — every layer is a closed ring resting on the one below, nothing bridges,
and a plain round hole is fine:

> "For hinges oriented along the vertical Z-axis... Because the circular cross-section is sliced
> in the horizontal X/Y plane... no horizontal bridging is required across the moving gap."
> — [UAVMODEL, *Print-in-Place Mechanism Design*](https://blog.uavmodel.com/3d-printer-print-in-place-mechanism-design-clearance-tolerances-hinge-geometry-and-joint-guidelines-2026/)

A pin whose axis is **horizontal** (parallel to the bed) is the hard case: the barrel's bore is a
horizontal circular tunnel, and the top half of that circle is an unsupported arch the printer
has to bridge over the clearance gap without drooping onto the pin:

> "Standard circular pins create unsupported horizontal arches that sag into the internal
> clearance space. Replacing circular holes with a 45° teardrop profile allows the top of the
> socket to bridge cleanly."
> — [3dprinterstuff.com, *How to Design Print-in-Place Hinges*](https://www.3dprinterstuff.com/workshop/design-print-in-place-hinges-that-work)

> "A teardrop hinge profile... creates a single linear bridging gap with no curve or overhang."
> — [UAVMODEL, *Print-in-Place Designs*](https://blog.uavmodel.com/3d-printer-print-in-place-designs-clearance-tolerances-material-choice-and-articulation-tips-2026-guide/)

This matters directly for §4: **the Cut tool's cut plane, not the hinge, decides which case
applies.** A hinge's pin axis is the in-plane direction `d`; `d` is horizontal (good, round pins)
whenever the cut plane's normal `n` is vertical — an ordinary horizontal slice through an upright
part. `d` can only be vertical if the cut plane itself is chosen vertical *and* Rotation is set to
put `d` along Z, which is an unusual, deliberate choice. So the common case (cut a part
horizontally, hinge it like a lid) is the easy one; teardrop support is Phase 2 for the uncommon
vertical-cut case, not a default requirement.

**Clearance.** Independent sources converge tighter than Flexi's own 0.5–0.75× nozzle-diameter
rule:

> "Leaving a 0.2mm to 0.3mm gap between the pin and the barrel is the sweet spot... 0.15mm
> possible on well calibrated machines."
> — [UAVMODEL, *Print-in-Place Mechanism Design*](https://blog.uavmodel.com/3d-printer-print-in-place-mechanism-design-clearance-tolerances-hinge-geometry-and-joint-guidelines-2026/)

> "PLA: 0.20mm for a slide fit, 0.30mm for free rotation, 0.40mm for loose tolerance... add
> 0.05–0.10mm extra per side for PETG."
> — [same source]

This is materially **tighter** than Flexi's 0.35 mm default (which was tuned for an interlocking
ring/ball, not a plain rotating pin). Recommendation: reuse the existing `flexi_clearance_floor()`
(0.75× nozzle) as the hinge's floor too, but default the hinge's own Gap-equivalent clearance to
**0.3 mm** on a 0.4 mm nozzle (the "free rotation" value above), not 0.35.

**Alternating knuckles.** This is the standard print-in-place hinge topology (door-hinge knuckles,
piano hinge), and is what lets a boolean union print without the pin itself needing to be a
separate part:

> "The classic print-in-place design alternates barrel segments between the two halves along a
> single pin, exactly like a real butt hinge." — [Sovol3D, *Print-in-Place 3D Printing*](https://www.sovol3d.com/blogs/news/print-in-place-3d-printing-how-to-design-hinges-joints-and-moving-parts-that-actually-work)

**Chamfered barrel edges and reduced flow** reduce fusing risk at the moving interfaces:

> "Recalibrate your flow rate by lowering it 2% to 5% to free up mechanical parts... pairing a
> 0.20–0.30mm radial clearance with bottom-edge chamfers, proper Z-axis pin orientation, and tuned
> bridge settings." — [UAVMODEL, *Print-in-Place Mechanism Design*](https://blog.uavmodel.com/3d-printer-print-in-place-mechanism-design-clearance-tolerances-hinge-geometry-and-joint-guidelines-2026/)

Flow-rate tuning is a slicing/profile concern, out of scope for the connector geometry itself, but
the chamfer is cheap to add to the revolved profile (§2) and is recommended for Phase 1.

**A well-known reference implementation** worth studying for parametrization (not for code, it's
OpenSCAD/BOSL2, but its parameter set validates what Phase 1 below proposes — knuckle count, pin
diameter, clearance, knuckle length, offset from the hinge line):
[BOSL2 `hinges.scad`](https://github.com/BelfrySCAD/BOSL2/wiki/hinges.scad).

Also consulted: [Snapmaker, *3D Printed Hinges: Design Rules*](https://www.snapmaker.com/blog/3d-printed-hinges/);
[Snapmaker, *3D Printing Tolerances*](https://www.snapmaker.com/blog/3d-printing-tolerances/);
[Creative3DP, *Press-Fit Tolerances*](https://tools.creative3dp.com/blog/press-fit-tolerances-3d-printing/) —
all corroborate the 0.2–0.4 mm clearance band and the alternating-knuckle topology; no source
disagreed on either point.

---

## 2. Geometry

Cut frame as in Flexi: cut plane `z == 0`, normal `n = +Z`. New for Hinge: an **in-plane axis**
`d`, a unit vector in the `z == 0` plane, taken from the connector's existing `z_angle` — `d =
Rz(z_angle) · (1, 0, 0)` in the cut frame, the same rotation Flexi's concurrent Rotation-angle work
adds for Chain link's `tilt_angle`... except here `z_angle` **is** the hinge axis directly, not a
secondary tilt. `e = n × d` is the in-plane direction perpendicular to the hinge (the "how far the
barrel sticks out from the cut line" direction).

### 2.1 Parametric definition

At connector position `p`, axis `d`, normal `n = +Z`:

* `N` knuckles, indexed `0..N-1`, centred on the hinge line, spaced with a Gap-derived axial gap
  `g` between consecutive knuckle end faces. Knuckle `i` occupies
  `[t_i, t_i + K)` along `d`, where `K` is knuckle length and `t_i` is laid out so the whole run
  of `N` knuckles is centred at `p` along `d`.
* Knuckles with even index belong to the **lower** half (male, `z ≤ -gap/2` per Flexi's existing
  convention — reuse it verbatim, do not invent a second gap concept); odd index to the **upper**
  half. (Reversed if the connector's fold-side flag, §2.3, is flipped.)
* Each knuckle is a **cylinder along `d`** of outer radius `R_o`, length `K`, i.e. exactly
  `its_make_cylinder(R_o, K)` reoriented so its axis is `d` instead of `+Z` — no new mesh
  primitive needed (`its_make_cylinder` already exists at `TriangleMesh.hpp:338`; a revolved
  profile via `its_make_revolved` is only needed if the chamfer from §1 is added, since
  `its_make_revolved` handles a `(r, z)` profile with corners for free while a raw cylinder does
  not — Phase 1 can ship with a plain cylinder and add the chamfer profile in the same pass as
  Phase 2's teardrop work, both being "swap the revolved profile" changes).
* A **pin** of radius `R_p` runs the full length of the knuckle run along `d`, continuous through
  every knuckle. Phase 1 makes the pin **integral to the lower half** (male): every lower-half
  knuckle is solid with the pin fused in (their union, one mesh); every upper-half knuckle has a
  bore of radius `R_p + Gap` drilled through it (their difference against a full-length cylinder
  of radius `R_p + Gap`). This is the "pin belongs to one half" option from the brief, not
  "integral pin per knuckle" — picked because a pin split at every knuckle boundary would need
  `N-1` extra clearance faces along the axis with nothing holding alignment between segments, and
  because one continuous pin is what makes the assembly a true captive hinge rather than `N`
  independent finger joints that could shear apart under axial load. Per-knuckle integral pins
  are worth flagging as a Phase 2/3 option for a "smoother swing, more forgiving of print
  imperfection" trade discussed in an issue but not implemented here.

### 2.2 Folding without collision — edge offset

If the barrel sits centred on the cut plane's contour interior, the two halves collide the moment
they rotate: material on the "inside" of the fold on each half sweeps through where the other
half's material is. The barrel has to sit at (or beyond) the **boundary of the cut face**, offset
in the `-e` direction (`e = n × d`, per the brief), so that when the part closes there is no solid
material behind the hinge line to hit.

Two placement modes, both worth having:

* **Automatic (Phase 1 default).** Given the cut contour (already computed by the gizmo's
  `object_clipper()` / `is_projection_inside_cut` machinery — see §3), project the contour onto
  the `d` axis and find where the perpendicular line through `p` in direction `e` last intersects
  the contour boundary walking in `-e`. Place the barrel's `e`-centerline at that boundary point,
  offset outward by `R_o` so the barrel's outer surface is tangent to (or just outside) the cut
  face edge. This mirrors a real box hinge: the knuckle sits on the outside edge of the lid, not
  in the middle of the panel.
* **Explicit "Edge offset" parameter (Phase 1, exposed, auto by default).** A signed distance
  along `e` from the auto-detected boundary point — positive pushes the barrel further outward
  (protruding hinge, like a piano hinge barrel sitting proud of the surface), negative pulls it
  inward (flush/recessed hinge, cut into the material, needs the part to have wall thickness to
  spare). Auto-placement (above) is Edge offset `= 0`.

Either way, this is a **placement** decision, not a topology change — it only affects where the
revolved/cylinder axis sits relative to `p`, exactly the way Flexi's existing auto-size already
repositions the ring based on the cross-section's inscribed radius (`flexi_auto_size()`).

### 2.3 Fold direction flag

A `bool fold_forward` (name TBD) picks which half is the even-indexed (pin-bearing) one when the
two halves are not geometrically symmetric — i.e., swaps "lower/male carries the pin" for
"upper/female carries the pin." Necessary because the cut's male/female assignment is a Z-order
convention (`z ≤ 0` vs `z > 0` in the cut frame) that has nothing to do with which side of the
physical part the user wants the barrel to protrude toward; without the flag every hinge's pin
would always end up on the "lower" printed half regardless of which way the fold should open.
Cheap to implement: it only swaps which parity of knuckle index is solid-with-pin vs. bored.

### 2.4 Fold stops (Phase 2, noted here for the boolean design)

A hard stop at 0° (fully open, flat) and/or 180° (fully closed) can be built as a flat chamfer on
one knuckle's mating face that meets a matching flat on the neighbor, so rotation past the flat is
blocked by solid-on-solid contact — the same idea as a D-shaped shaft flat. This is a profile
change to the revolved knuckle cross-section (an extra straight segment in the `(r, z)`-equivalent
profile), not a new boolean step, so it composes cleanly with §2.1's construction once knuckles
move from plain cylinders to `its_make_revolved` profiles (which Phase 1's chamfer already
motivates — see §2.1). Not implemented in Phase 1.

### 2.5 The boolean construction

Following Flexi's Chain Link precedent exactly (§1.6 of the flexi spec — "both segments get a body
*and* a relief", `flexi_lower_bodies/upper_bodies/lower_reliefs/upper_reliefs`), Hinge is
**two-sided** (`flexi_is_two_sided`-equivalent returns true for Hinge, same as Chain Link):

> Lower half = (object ∩ {z ≤ -gap/2}) ∪ LowerKnuckles ∪ Pin, minus dilate(UpperKnuckles, Gap).
> Upper half = (object ∩ {z ≥ +gap/2}) ∪ UpperBoredKnuckles, minus dilate(LowerKnuckles ∪ Pin, Gap).

Where `UpperBoredKnuckles` are the odd-index cylinders pre-bored to `R_p + Gap` (so the *relief*
subtraction that follows only needs to remove the small **inter-knuckle** clearance, not redo the
bore — this keeps the two operations independent and each individually testable, matching how
Flexi keeps "body" and "relief" as separate lists per §1.6 of the flexi spec).

Watertightness and the **inter-knuckle Gap** (the brief's point (d), "alternating knuckles have
Gap between their end faces too"): each knuckle cylinder is generated `Gap/2` short of its nominal
boundary on each end (i.e., length `K - Gap`, centred in its `K`-wide slot), the same "shrink by
gap/2 at each face" trick Flexi's `flexi_revolved_z_shift()` uses for the ring's stem — except
here it applies along `d` at every knuckle-to-knuckle boundary, not just once at the cut plane.
This guarantees every pair of facing knuckle end faces (both the cut-plane pair and every
knuckle-to-knuckle pair) is exactly `Gap` apart without a second Clipper offset pass: it is pure
placement arithmetic on cylinder length and position, because unlike Flexi's dovetail lip there is
no undercut profile to preserve here — a cylinder shortened uniformly is still a cylinder.

**Reuse summary against the brief's ask "show how the existing ChainLink code path generalizes":**
`flexi_lower_bodies()` returns `LowerKnuckles ∪ Pin` as one merged ITS (mirroring how Chain Link
merges the vertical loop into one body); `flexi_upper_bodies()` returns the bored upper knuckles;
`flexi_lower_reliefs()` returns `dilate(UpperKnuckles, Gap)`; `flexi_upper_reliefs()` returns
`dilate(LowerKnuckles ∪ Pin, Gap)`. The dispatch in `Cut::perform_with_flexi_joints()` and the
Manifold-boolean-with-mcut-fallback plumbing (flexi spec §3) need **zero changes** — they already
iterate these four generic lists. `its_make_revolved` is reused if/when the chamfer (§2.1) or
stop-flat (§2.4) profile ships; Phase 1's plain-cylinder knuckles can use `its_make_cylinder`
directly and are the one place Hinge's mesh generation is simpler than Chain Link's, not harder.

---

## 3. UI

New fields in the connector panel when **Type = Flexi, Joint = Hinge** (naming follows the existing
panel's pattern of one row per parameter, per `GLGizmoCut.cpp`'s Flexi block):

| Field | Default | Notes |
|---|---|---|
| **Knuckle count** | 3 | int, odd preferred (self-centring on `p`); even allowed but the panel should note it shifts the hinge line off `p` by half a knuckle |
| **Pin diameter** | 2·`R_p` = 2.0 mm | |
| **Barrel diameter** | 2·`R_o` = 4.0 mm | must be > pin diameter + 2·Gap with margin for wall thickness |
| **Knuckle length** | auto from cut extent along `d` / N, or explicit | "Auto size" checkbox reused from Flexi's existing pattern |
| **Gap** | 0.6 mm (reuse Flexi's Gap parameter and its floor-at-clearance rule verbatim) | |
| **Clearance** (pin-to-bore) | 0.3 mm, floor `flexi_clearance_floor()` | new default per §1, not Flexi's 0.35 |
| **Rotation** | 0° | existing `z_angle` field — this *is* the hinge axis `d`, no separate control needed |
| **Edge offset** | 0 (auto placement) | signed, see §2.2 |
| **Fold side** | Lower carries pin | the §2.3 flag |

**Auto-derived, not shown:** knuckle axial gap (from Gap), the boundary point for auto edge
placement (from the cut contour), whether the axis is horizontal or vertical (derived from `n` and
`d`, used only for the warning in §4).

**Validation — the out-of-contour check must change.** `GLGizmoCut3D::is_outside_of_cut_contour()`
at `src/slic3r/GUI/Gizmos/GLGizmoCut.cpp:3271` builds its test footprint from
`cur_connector.radius` alone: it samples `sectorCount` points on a **circle** of that radius
(lines 3282–3296, the `shape == Circle` branch effectively, since Flexi connectors set
`connector.radius = flexi_outer_extent()` per the flexi spec §5) and checks each sampled point
against `object_clipper()->is_projection_inside_cut()`. A hinge's real footprint is not a disc
around `p` — it is a **rectangle** (or an offset, off-centre rectangle once Edge offset is
nonzero) of length `N·K + (N-1)·gap` along `d` and width `2·R_o` along `e`, positioned by the edge
offset rather than centred on `p`. A circular test either (a) rejects perfectly valid long, thin
hinges that fit the actual contour but not a circumscribing circle, or (b) if under-sized to the
short axis, silently misses a hinge whose knuckle run actually crosses the contour boundary along
`d`.

Concretely, this needs a small new branch in `is_outside_of_cut_contour()`: when
`cur_connector.attribs.type == CutConnectorType::FlexiJoint && cur_connector.flexi.kind ==
FlexiJointKind::Hinge`, build `mesh.vertices` from the hinge's actual oriented rectangle corners
(four points, or sample the barrel-run's convex hull if the chamfer/stop-flat profile from §2.1/
§2.4 makes it not perfectly rectangular) transformed by `translation_transform(cur_pos) *
m_rotation_m` exactly as the existing circle branch does at line 3297 — i.e. the transform
plumbing is unchanged, only the `vertices` construction (lines 3290–3296) needs a hinge-specific
branch alongside the existing `Triangle/Square/Circle/Hexagon` `sectorCount` switch. This is the
same kind of shape-aware footprint the function already has for `CutConnectorShape::Square` vs
`Circle`; Hinge just needs to be a fifth shape-like case keyed off `flexi.kind` rather than
`attribs.shape` (Flexi connectors don't use `attribs.shape` today — worth confirming, but the flexi
spec's own use of `flexi_outer_extent()` as a plain `connector.radius` suggests they don't
differentiate there either).

`is_conflict_for_connector()` (line 3318) also does an overlap test against other connectors using
`(connector.pos - cur_connector.pos).norm() < radius + radius` (line 3341) — a circle-circle test.
For Phase 1 (single hinge per cut, per the flexi spec's existing "applies the first, warns on the
rest" rule for multiple Flexi connectors) this does not need to change; it only matters once Phase
3's "multiple hinges along a cut" ships, at which point it needs the same rectangle-aware treatment
as the contour check.

---

## 4. Slicing implications

**Clearances vs. `slice_closing_radius`.** Same guard as Flexi (`flexi_gap_closing_conflict()`,
flexi spec §5): the hinge's pin-to-bore clearance must stay above `2 × slice_closing_radius`. At
the stock default (`slice_closing_radius = 0.049 mm`) and the recommended 0.3 mm clearance (§1)
there is a 3× margin — comfortably clear, though tighter than Flexi's ring at 0.35 mm/3.5×. Reuse
`flexi_gap_closing_conflict(p, r)` verbatim by treating the hinge's pin clearance as `p.clearance`
in that call.

**`xy_hole_compensation`.** This is the one guard Flexi's own spec does not mention, and Hinge
needs it: `xy_hole_compensation` (PrintConfig.cpp:7163) grows internal holes to counter elephant-
foot-style inward bowing, which **directly enlarges the bore** the pin sits in. A positive
`xy_hole_compensation` is *helpful* here (it fights the same over-extrusion-narrows-the-gap failure
mode the UAVMODEL source calls out in §1), but a large one could open the clearance up beyond
intent. Recommend the gizmo's Hinge panel show the *effective* clearance as `nominal clearance +
xy_hole_compensation` (read-only, informational) rather than trying to compensate for it
automatically — the two knobs live in different places (per-connector vs. per-print-profile) and
conflating them risks a clearance that silently changes when the user switches print profiles.

**Recommended per-nozzle clearance defaults** (extending Flexi's `flexi_clearance_floor()` table,
using the tighter free-rotation numbers from §1 rather than Flexi's ring/socket numbers, since a
plain pin has less surface area to catch on than a dovetail lip):

| Nozzle | Pin clearance default | Floor (0.75× nozzle, same rule as Flexi) |
|---|---|---|
| 0.2 mm | 0.25 mm | 0.15 mm |
| 0.4 mm | 0.30 mm | 0.30 mm |
| 0.6 mm | 0.40 mm | 0.45 mm |

Note the 0.6 mm row: the 0.75×-nozzle floor (0.45) exceeds the "free rotation" default (0.40) from
the maker sources, exactly the situation the flexi spec's §1.5 "clearance floor" note already
flagged for Double Ring/Ball & Socket — on a 0.6 mm nozzle the floor wins and the joint gets
looser than the source-recommended default, same tradeoff, same justification (a floor that isn't
reachable on the stock nozzle is worse than a slightly-loose joint on a fat nozzle).

**No auto-added bridge or support blocker recommended for Phase 1.** Unlike Flexi's Double Ring
(whose groove ceiling is an internal cavity nothing can reach, flexi spec §6), the hinge's
"ceiling" — the top of each bore, the arch a horizontal pin axis has to bridge — sits at the
part's actual outer surface (the knuckle is on the edge of the cut face, §2.2) so it is visible to
the normal overhang/bridging heuristics and to the user in preview, unlike the Double Ring's
groove which is fully enclosed. Recommend the same guidance as Flexi §6 ("print with supports off
where the model allows, else build-plate-only") rather than a code-level auto-blocker; a real
per-object support exclusion zone generated from the hinge geometry is a reasonable Phase 2/3 item
but adds real complexity (support blockers are a separate object-level feature, `Print::` /
`PrintObject::` support-material machinery, not connector geometry) for a case the existing
guidance already covers.

**Horizontal-axis warning.** Per §1's corrected framing: **`d` horizontal is the good case** (round
pin, self-supporting via the ordinary overhang/bridge slicer settings — see §1's sources), **`d`
vertical is the case needing extra care** (Phase 2 teardrop). The gizmo should warn — not block —
when `d`'s Z component exceeds some small threshold (e.g. `|d · Z| > 0.1`, roughly >6° from
horizontal), worded as: *"This hinge's pin axis is not horizontal. A vertical or steeply tilted
pin axis needs support inside the bore on this nozzle; rotate the part or change Rotation so the
axis lies flat, or expect to support the bore manually."* This is a straightforward extension of
the same `m_rotation_m` / cut-frame transform the gizmo already has on hand (it already computes
`n` from the cut plane and `d` from `z_angle`; the warning is one dot product and one imgui text
line, following the same pattern as the existing gap-closing-radius warning in the flexi spec §5).

---

## 5. Phasing

**Phase 1** (this spec's recommendation): fixed odd `N` (default 3), alternating knuckles,
integral pin on one half (§2.1), horizontal-axis only (warn, don't block, on non-horizontal),
parameters: Knuckle count, Pin diameter, Barrel diameter, Knuckle length (+ auto), Gap, Clearance,
Rotation (existing `z_angle`), Edge offset (+ auto), Fold side. Auto edge placement from the cut
contour (§2.2). Rectangle-footprint out-of-contour check (§3). No stops, no chamfer, no teardrop.
*Effort:* similar order to Flexi phase 1 (new `FlexiJointKind::Hinge`, ~150–250 lines of geometry
in `FlexiJoint.cpp` reusing `its_make_cylinder`/dilation patterns already in the file, panel
additions in `GLGizmoCut.cpp` following the existing Flexi block, the contour-check branch from
§3). *Risks:* (a) **boolean robustness with N barrels** — each knuckle-to-pin union and
knuckle-to-bore difference is its own Manifold op; at `N = 3` that is on the order of 6–8 boolean
calls per cut (comparable to Flexi's "four booleans at worst" per flexi spec §3), but a larger `N`
(Phase 3) scales roughly linearly and each op is small/simple (cylinder vs. cylinder), so it should
stay well inside interactive-cut budgets — worth a timing test once `N` becomes user-facing beyond
a small fixed default; (b) **edge-placement correctness on concave/multi-lobe contours** — the
"walk the contour boundary in `-e` from `p`" procedure in §2.2 needs a real definition for contours
that are not convex (which barrel-run direction "outward" if the cut face has a notch at that
point); Phase 1 should fall back to the object bounding-box edge (the same approximation Flexi
phase 1 already accepts for auto-sizing, per the flexi spec's own Phase 3 "Contour-exact auto
sizing" backlog item) rather than blocking on a robust contour-offset algorithm.

**Phase 2:** fold stops (§2.4, flat-faced knuckle ends), protrusion vs. flush vs. recessed as a
first-class option beyond the raw Edge-offset number (i.e., a labelled dropdown that sets sensible
Edge-offset presets), teardrop bore profile for the vertical-axis case (§1, §4) — this is the
"needs extra care" case from §4's warning becoming actually supported rather than just flagged,
using the chamfer/profile machinery already motivated in §2.1.

**Phase 3:** multiple hinges placed along a long cut (mirrors the flexi spec's own Phase 3 backlog
item "multiple joints along a wide cut" — same `is_conflict_for_connector()` extension needed, per
§3), and a living-hinge alternative (a thin flexible web instead of a pin — a fundamentally
different, material-dependent mechanism, likely its own connector kind rather than a Hinge
variant, since it does not use the pin/knuckle boolean machinery at all and instead needs a
thin-wall/rib generator).

---

## 6. Test plan

**Unit tests** (`tests/libslic3r/test_flexi_joint.cpp`, extending the existing `[FlexiJoint]` tag
with a Hinge sub-case, following the file's existing pattern for Chain Link):

* **Non-intersection with Gap.** Lower-half body (knuckles + pin) and upper-half body (bored
  knuckles) do not intersect at rest; minimum measured distance between them is within tolerance
  of the declared Gap and Clearance (same measurement style as the flexi spec §7's "min distance
  0.34988mm against a declared 0.35mm").
* **N knuckles alternate correctly.** For `N = 3, 4, 5`, confirm knuckle `i`'s parity determines
  which half's boolean-union list it lands in, and that the fold-side flag (§2.3) swaps it.
* **Pin continuity.** The pin is a single connected component spanning the full knuckle run
  (watertight, one shell) regardless of `N`; the bore in every odd knuckle is concentric with it
  within the declared clearance.
* **Captivity / pull test**, mirroring the flexi spec's non-separable check (§7): translating the
  upper half along `e` (perpendicular to the pin) by more than `R_o - R_p` must intersect the pin
  — the hinge cannot be pulled apart sideways. (Axial pull along `d` is *not* expected to be
  captive by geometry alone in Phase 1 — nothing stops the pin sliding out its own axis unless the
  knuckle run is deliberately made longer than the pin, or end caps are added; note this as a known
  Phase 1 limitation to record in the same section, not silently pass a test that shouldn't pass.)
* **Rotation sweep.** No intersection through at least ±90° of swing about `d` (a folding lid needs
  up to 180°; test the practically-useful range and flag full 180° as dependent on Edge offset
  clearing the part body, which is a placement concern, not a joint-geometry one).
* **Rectangle footprint vs. circle footprint**, at the GLGizmoCut level if feasible without a GL
  canvas (the flexi spec notes the gizmo itself can't be instantiated in `libslic3r_tests` — same
  constraint applies here, so this may need to be a `flexi_outer_extent`/footprint-corners unit
  test on the geometry function proposed for §3 rather than a true UI test, plus a manual click
  test item below).

**Demo export.** A 30×30×15mm test cube (matching the flexi spec's 20mm-cylinder precedent scale)
cut at mid-height with a Hinge connector (N=3, default sizes), exported as two-part STL/3MF via the
same CLI-driven flow the flexi spec's phase 2 CLI gate used (`snorca_hubtest`-style), confirming
`exit=0`, one object, two watertight parts, and (per §4) that the clearance survives slicing at
the stock `slice_closing_radius`.

**Physical print checklist** (mirrors the flexi spec §8 "owner's click test" structure — nobody
has clicked this gizmo either, since it does not exist yet):

1. Load a lid-and-box test shape, or a plain cube. Bambu Lab P1S / 0.4mm nozzle / 0.2mm layer /
   PLA, matching the flexi spec's existing print rig for a same-machine comparison.
2. Cut gizmo → Type = Flexi → Joint = Hinge, defaults (N=3, 2mm pin, 4mm barrel, Gap 0.6mm,
   Clearance 0.30mm, auto edge placement).
3. Confirm the panel warns (not blocks) if the cut orientation makes the axis non-horizontal; note
   whether the warning appears/disappears correctly as the model is rotated before cutting.
4. Slice with supports off (per §4), check the preview at the knuckle layers for the expected short
   bridge over the pin clearance and nothing else unsupported.
5. Print, remove from bed without tools, and confirm: the hinge swings through its intended range
   without cracking, the pin does not pull free sideways, and the two halves do not fuse. If fused:
   raise Clearance one step per §4's per-nozzle table and re-check `slice_closing_radius`, exactly
   as the flexi spec's own troubleshooting note (§8) recommends.
6. Fold the part fully closed; confirm Edge offset = 0 (auto) placed the barrel far enough outward
   that the two halves do not collide before reaching the intended closed angle — this is the one
   check that only a physical print (or an in-slicer motion preview, not yet built) can really
   validate, since §2.2's auto placement is a heuristic, not a proof.

---

## 7. Phase 1 implemented

Branch `feat/hinge-connector`, merged onto `feat/ultra-preferences` after the concurrent
chain-link orientation work landed there. This section records what actually shipped against
the plan above, what the proofs were, and what is explicitly still unverified.

Two things came from that merge rather than from this plan. The hinge **reuses**
`FlexiJointParams::rotation` instead of the connector-level `z_angle` this spec assumed, and
the hinge's own six fields are written into `Metadata/cut_information.xml` alongside the rest
of the Flexi parameters, using the per-attribute 3MF serialization that branch introduced.

### 7.1 What was built

`FlexiJointKind::Hinge` is a fourth kind under `CutConnectorType::FlexiJoint`. As §2.5
predicted, `Cut::perform_with_flexi_joints()` needed **no structural change at all**: the hinge
fills the same four generic lists the chain link does (`flexi_lower_bodies` /
`flexi_upper_bodies` / `flexi_lower_reliefs` / `flexi_upper_reliefs`), and the existing
Manifold-with-mcut-fallback plumbing runs them unchanged.

Geometry (`src/libslic3r/FlexiJoint.cpp`), in the cut frame with the hinge axis along **+X**:

* `N` knuckle **slots** of length `S = hinge_length / N`, the run centred on the joint origin.
  Knuckle `i` is a cylinder of length `S - gap` centred in slot `i`, so **every** pair of
  facing knuckle end faces is exactly `gap` apart. That is pure placement arithmetic - no
  Clipper pass - because a cylinder shortened uniformly along its own axis is still a cylinder,
  exactly as §2.5 argued.
* Even-indexed knuckles belong to the lower half, odd to the upper; `hinge_fold_upper` swaps
  both parities at once (§2.3).
* The **pin** is one continuous cylinder spanning the whole run, returned as its own component
  in the pin-bearing half's body list. The cut pipeline unions the list entry by entry, so the
  union is what fuses pin to knuckles - the same way the double ring's lip and hub are separate
  components.
* The **bore** is not cut in the body pass. It falls out of the relief pass: the bore-side
  half's reliefs are the other half's knuckles inflated by `C` plus **the pin inflated by `C`**,
  and subtracting that inflated pin from the half drills every one of its knuckles at exactly
  `R_p + C`. Body and clearance stay two independent, separately testable steps.
* **Rotation** reuses `FlexiJointParams::rotation` - the field the concurrent chain-link
  orientation work added, in degrees, 0-180 - rather than adding a hinge-specific one. For the
  chain link that angle turns the ring pair within the cut plane; for the hinge it **is** the
  pin axis `d`, so it is the joint's main control rather than a secondary one. The geometry
  applies it the same way `chain_apply_rotation()` does: the run is built along the cut plane's
  +X and swung to `d = rotate(+X, n, rotation)`. `hinge_footprint_corners()` turns with it, so
  the contour test needs no extra transform.
* **Edge placement** is `hinge_edge_offset`, a distance along `-e` (i.e. `-Y` in the joint
  frame). See §7.3 for the correction auto-placement needed.

### 7.2 Parameters and defaults

| Field | Default | Range | Notes |
|---|---|---|---|
| `hinge_knuckles` | 3 | 1..9 | odd is self-centring; the panel slider enforces the range |
| `hinge_pin_dia` | 2.0 mm | 0.4..20 | |
| `hinge_barrel_dia` | 4.0 mm | 1..40 | must exceed pin + 2xClearance + 0.4 wall, enforced in `flexi_validate()` |
| `hinge_length` | 12.0 mm | 2..200 | the whole run, end face to end face |
| `hinge_edge_offset` | 0 (auto) | 0..200 | auto by default; the checkbox releases it |
| `hinge_fold_upper` | false | | false = the lower half carries the pin |
| Gap | 0.6 mm | >= clearance | shared with the ring and the ball, per `flexi_default_gap()` |
| Clearance | `flexi_clearance_floor(nozzle)` = 0.30 on a 0.4 nozzle | | shared, unchanged |
| Rotation | 0 deg | 0..180 | `FlexiJointParams::rotation`, shared with the chain link; **this is** the hinge axis |

Auto sizing (`flexi_auto_size()`) derives the run length from the cut cross-section
(`1.6 x` the inscribed radius, capped at 60 mm), the barrel from `0.35 x` it (2..10 mm), the pin
from `0.45 x` the barrel, and then widens the barrel if needed so it still clears the pin,
twice the clearance and a 0.8 mm wall on each side.

Note on the clearance default: §1/§4 recommended a hinge-specific 0.30 mm pin clearance rather
than Flexi's 0.35. That value **is** what a 0.4 mm nozzle produces today, because
`flexi_clearance_floor(0.4) == 0.30` and the gizmo floors the clearance at it - so the
recommended number arrives without a separate per-kind default. A distinct `hinge_clearance`
field was deliberately not added: one clearance knob per joint is easier to reason about, and
the per-nozzle table in §4 is already what the floor implements.

### 7.3 The auto edge placement correction

§2.2 says to park the barrel so its outer surface is tangent to the cut face's edge. Built that
way and tested against the round cut face of the 20 mm cylinder, the **middle** of the barrel
is tangent but the run's two far **corners** hang outside the contour - a 12 mm run at the
widest point of a 20 mm circle overhangs by 1.7 mm at each end.

Auto placement therefore parks the barrel at the **chord at half the run's length**:
`reach = sqrt(r_in^2 - (hinge_length/2)^2)`, then out by the barrel radius. On a square or
rectangular cut face this costs essentially nothing; on a round one it is exactly the correction
needed, and it puts the run's far corners *on* the rim - as far out as the hinge can go with its
whole footprint still inside the face. This is still a bounding-box/inscribed-circle
approximation, the same one §5's risk (b) says Phase 1 should accept.

**And a second correction, found the hard way.** A barrel parked so its outer wall lands
*exactly* on the part's own side face - which is what "tangent to the edge" literally means on
a flat-sided part - gives the Manifold union two **coplanar** surfaces to work across. On the
40 mm demo cube that made the boolean fail outright, and a failed boolean does not degrade
gracefully: `perform_with_flexi_joints()` logs and falls back to two plain halves, so the whole
joint silently disappears. Auto placement therefore backs off a further **0.1 mm**, biting that
much into the wall so every face stays transverse. It is invisible on the part and it is the
difference between a hinge and no hinge. Worth remembering for any future "flush with the
surface" placement option (§6's Phase 2 item): flush must mean *nearly* flush.

### 7.4 The out-of-contour fix

`GLGizmoCut3D::is_outside_of_cut_contour()` now branches on
`cur_connector.attribs.type == CutConnectorType::FlexiJoint` and samples
`flexi_footprint_corners()` - the joint's **real** outline - instead of a circle of radius
`flexi_outer_extent()`:

* **Hinge**: the knuckle run's rectangle, `hinge_length` by `hinge_barrel_dia`, offset by the
  edge offset.
* **Chain link**: the bounding rectangle of both loops' centrelines projected onto the cut
  plane, grown by `wire + clearance` - the slot the interlocked pair actually sweeps through
  the plane.
* **Double ring / ball & socket**: a 60-gon on the disc of radius `flexi_outer_extent()`, i.e.
  byte-for-byte the circle the function used to build by hand.

Each edge is sampled at 8 points as well as its corners, so a notch in the contour cannot be
stepped over. The joint's own Rotation is already baked into the corners the helper returns - it turns them
exactly as the bodies are turned - so the gizmo only applies
`translation_transform(pos) * m_rotation_m`, and a rotated hinge tests as a rotated rectangle
rather than a bigger one.

**This is what fixes the owner's spurious "1 connector is out of cut contour" on chain links.**
The old test asked "does a disc big enough to swallow the joint fit?", which for a 7 mm long
chain link means a disc of radius ~4.9 mm in *every* direction, when what the joint needs is
that reach in *one* direction and about half of it across. `flexi_outer_extent()` is still what
sizes the connector's picking radius and preview - only the contour test changed.

### 7.5 Printability warning

`hinge_axis_needs_care(axis_world)` returns true when `|d.z| / |d| > 0.1` (about 6 degrees off
horizontal), and the panel then shows an orange warning - never a block. Per §1's correction of
the brief, **horizontal is the easy case** (short bridge over the pin, ordinary overhang
settings carry it) and vertical is the one needing care (full unsupported circle over the bore,
teardrop profile deferred to Phase 2). The gizmo derives `d` in world coordinates as
`m_rotation_m.linear() * (cos(rotation), sin(rotation), 0)`.

### 7.6 Proofs

Build: `BUILD_EXIT=0` in a dedicated worktree build tree configured like `build` with
`-DBUILD_TESTS=ON`. `libslic3r_tests "[FlexiJoint]"` - all cases pass, including every
pre-existing Flexi and Chain link case, unchanged.

New cases in `tests/libslic3r/test_flexi_joint.cpp`:

* **Knuckles alternate.** For `N = 1, 2, 3, 4, 5, 9`: consecutive knuckles belong to opposite
  halves; the fold-side flag inverts every parity; the two halves' body lists partition the run
  (`ceil(N/2) + 1` bodies on the pin side counting the pin, `floor(N/2)` on the other); the run
  is centred on the joint origin.
* **Pin continuity.** The pin is watertight, **one** connected component (union-find over its
  triangles), spans exactly from the first knuckle's outer face to the last one's, has the
  declared radius, and its volume matches a solid cylinder to 2% - it is not a tube. Every
  bore-side knuckle's slot lies strictly inside the pin's span, so the pin really does pass
  through them.
* **Non-intersection with Gap.** The finished cut's two halves have **zero** boolean
  intersection volume and a minimum surface distance of **0.34988 mm** against a declared
  clearance of 0.35 - and `>= 0.2` as the proof bar asks.
* **Knuckle end-face gaps.** At gaps of 0.4, 0.6 and 1.2 mm: slot length minus knuckle length
  equals the gap exactly, and every facing pair of end faces is that far apart, `>= gap`.
* **Barrel at the contour edge.** Every corner of the footprint is inside the cut contour, the
  run's far corners sit *on* the rim (as far out as it can go), and pushing the offset 0.5 mm
  further puts corners outside - which is what the fixed contour check now catches.
* **Rotation.** The footprint's long and short axes swap under a 90 degree rotation (same
  rectangle, turned); and a full cut with `z_angle = 90 deg` produces material above the cut
  plane spread along Y where the unrotated cut spreads it along X, the same size either way.
* **Pin-axis warning.** Fires for vertical and steeply tilted axes, stays quiet for horizontal
  ones and for a few degrees off flat, and does not nag on a degenerate axis.
* **Guards.** A barrel too small for its bore plus a wall, too many knuckles for the length, and
  out-of-range knuckle counts are all refused; the hinge is captive (bore < barrel); auto sizing
  produces a valid hinge at inscribed radii of 5, 10 and 20 mm.
* **Footprint vs. the old circle** (the gizmo needs a GL canvas, so this tests the helper the
  fixed function calls, as §6 anticipated): the chain link fits a contour the old circle test
  rejected, and still fails one it genuinely does not fit; a long thin hinge fits a contour far
  smaller than its own length; the revolved kinds still produce exactly the 60-point circle.
* **Serialization round trip.** All six new fields survive a cereal binary round trip (the
  undo/redo path); the defaults are what old files get and they make a **valid** hinge as they
  stand; `operator==` and `!=` see the new fields, so undo/redo notices a change to them.
* **3MF parameter round trip.** All six fields plus the shared Rotation survive a real
  `store_bbs_3mf` / `load_bbs_3mf` project round trip as `Metadata/cut_information.xml`
  attributes, checked field by field and then with `q == p` over the whole struct - so a field
  added later without a 3MF attribute fails this test. **The kind above all**: the reader
  clamps the enum against untrusted file content, and that clamp had to be widened to `Hinge`
  or every saved hinge would have silently reloaded as a double ring. The knuckle count is
  clamped to 1..9 on read for the same reason - it drives a loop.
* **3MF geometry round trip.** A hinged cut object stores and loads as one object with two
  watertight parts.

**Demo.** `libslic3r_tests "Export the hinge demo"` (hidden `[.][HingeDemo]` tag, output
directory in `SNORCA_HINGE_OUT`) cuts a 40 mm cube at mid height with one 3-knuckle hinge
(3 mm pin, 6 mm barrel, 24 mm run, Gap 0.6, Clearance 0.30) and writes
`hinge_demo_upper.stl`, `hinge_demo_lower.stl` and `hinge_demo.3mf`.

### 7.7 Unverified

* **Nobody has clicked it.** The gizmo cannot be instantiated in `libslic3r_tests` (no GL
  canvas), so the panel rows, the Rotation control inside the Flexi block, the auto-edge
  checkbox, the knuckle slider and the warning text have been compiled but never rendered. The
  same caveat the Flexi spec's §8 carries.
* **Nothing has been printed.** Every clearance claim here is measured on the mesh, not on
  plastic. The 0.30 mm clearance and the bridge over the bore are the two things only a print
  can settle.
* **The contour check is proven at the helper, not through the gizmo.** `flexi_footprint_corners()`
  is tested directly; that `is_outside_of_cut_contour()` calls it correctly is code review plus a
  clean build, not a test.
* **Concave and multi-lobe cut faces.** Auto placement uses the bounding box and the inscribed
  circle, per §5's risk (b). A cut face with a notch where the hinge lands will place badly - the
  contour check will *catch* it, but the auto offset will not have avoided it.
* **Even knuckle counts** are allowed and tested for topology, but an even run puts a
  knuckle-to-knuckle boundary on the joint origin rather than a knuckle, so the hinge is not
  self-centring there. That is by design (§3) and is not warned about in the panel.
* **Axial captivity.** As §6 flags: nothing stops the pin sliding out along its own axis. The
  hinge is captive against sideways pull (the bore wraps the pin) but not against axial pull.
  End caps are a Phase 2 item.

### 7.8 Physical print checklist

1. Open `hinge_demo.3mf` from the demo export (or cut your own: any part, Cut gizmo,
   Type = Flexi, Joint = Hinge, defaults).
2. 0.4 mm nozzle, 0.2 mm layer, PLA. Same rig as the Flexi spec's prints, for comparison.
3. Confirm `slice_closing_radius` is at its stock 0.049 mm. The panel warns if it is not; at
   0.30 mm clearance the safe ceiling is 0.15 mm.
4. Check the pin-axis warning behaves: rotate the part before cutting so the cut plane tilts,
   and confirm the orange line appears as the axis leaves horizontal and goes away again.
5. Orient so the pin axis lies **flat on the bed**. Slice with supports off (or build-plate
   only). In preview, step through the knuckle layers: expect a short bridge over the pin
   clearance at the top of each bore and nothing else unsupported.
6. Print. Remove from the bed without tools.
7. Confirm: the hinge swings without cracking; the two halves are not fused; the halves cannot
   be pulled apart sideways. If fused, raise Clearance one step (§4's per-nozzle table) and
   re-check `slice_closing_radius`; consider dropping flow 2-5% per §1.
8. Fold the part fully closed and confirm the two halves do not collide before the intended
   angle. This is the one check only a print settles - §2.2/§7.3's placement is a heuristic,
   not a proof.
9. Note whether the pin can be pushed out along its axis (§7.7). Expected: yes, in Phase 1.
