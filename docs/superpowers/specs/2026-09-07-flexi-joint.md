# Flexi joint — a print-in-place articulated joint in the Cut gizmo (phase 1)

Branch `feat/flexi-joint-p1`, from `feat/ultra-preferences` at `67a2d25eb3`.

## The short version

The Cut gizmo can already insert a parametric body that straddles the cut plane and comes out solid
on one half and a clearance-inflated void on the other — that is what Plug and Snap connectors are.
A flexi joint is the same idea with two changes that matter: the male body has an **undercut**, so
the two halves cannot pull apart, and the clearance has to be **exactly the same on every face**, so
the halves print in place without fusing.

This branch adds a new connector family, **Flexi joint**, with two joint types:

* **Double ring** (the primary, the one on articulated dragons and rexes): an annular dovetail lip
  on one segment nesting in an annular groove on the other, plus an optional solid hub.
* **Ball & socket** (secondary): a ball on a neck captured by a socket whose mouth is narrower than
  the ball.

Both produce **one object with two watertight model parts**. Nothing is left as a live negative
volume — see §3 for why.

## 1. The geometry

Everything is defined in the **cut frame**: the cut plane is `z == 0`, the joint axis is `+Z`, the
**male** half is the material below the plane and the **female** half the material above it.

### 1.1 The construction, in one line

> Male half = object ∩ ({z ≤ 0} ∪ L). Female half = object ∖ dilate({z ≤ 0} ∪ L, C).

`L` is the male protrusion and `C` the clearance. Dilation by a ball of radius `C` distributes over
union, so `dilate({z ≤ 0} ∪ L, C) = {z ≤ C} ∪ dilate(L, C)`. Two consequences fall straight out:

* The two **flat mating faces** end up `C` apart — the female half starts at `z = C`, not at `z = 0`.
  That is why the cut runs the plane twice: at `z = 0` for the male half, at `z = C` for the female.
* The **cavity is the male body dilated by C**, so the distance from every male face to the matching
  female face is exactly `C`. Nothing is scaled; `apply_tolerance()`'s "add the tolerance to the
  XY/Z scale factor" trick cannot express this for a ring (scaling a torus moves its mean radius as
  well as its tube radius), which is the reason the flexi path does not reuse it.

### 1.2 Why revolved profiles

Both joint bodies are solids of revolution. For a solid of revolution, the distance from any point
to the solid equals the distance measured in that point's own meridional half-plane — the `Δφ = 0`
term always wins in `|P−Q|² = ρ_P² + ρ_Q² − 2ρ_Pρ_Q cos Δφ + (z_P−z_Q)²`. So the 3D dilation by `C`
**is** the revolution of the 2D dilation of the meridional section by `C`, and a plain Clipper
`offset(polygon, C, jtMiter)` in the `(r, z)` half-plane gives the cavity exactly. Convex corners
get mitered (clearance there is larger than `C`, never smaller); concave corners get exactly `C`.

`its_make_revolved(profile_rz, sectors)` (new, in `TriangleMesh.cpp` next to the existing
`its_make_torus`) revolves a closed `(r, z)` loop. Points at `r == 0` collapse onto the axis, so the
same generator produces both ring topology (Double ring, torus-like) and sphere topology
(Ball & socket) and comes out watertight with outward normals either way.

Male and female use the **same sector count and the same angular phase** (120 sectors), so their
facets stay parallel and the measured clearance is the true `C` rather than `C ± the faceting error`.

### 1.3 Double ring

Meridional profile of the lip, at mean radius `Rm = outer_radius − ring_width/2`:

```
        <-------- W -------->            z = H            (flat top)
         \                  /
          \                /             z = H − Hd       (Hd = 0.6·H, the flare)
           |   Wn = W·nr   |
           |               |
   --------|---------------|--------     z = 0            (the cut plane)
           |               |
           +---------------+             z = −A           (anchor, inside the male half)
```

The flare is the undercut: the mouth of the groove is `Wn/2 + C` wide, the head of the lip is `W/2`
wide, so `Wn/2 + C < W/2` makes the joint captive. With the defaults that is `0.80 < 1.00 mm`.
The flare's underside sits at ~59° from horizontal, so it prints without support; the groove's
ceiling is a flat annular bridge about `W + 2C ≈ 2.7 mm` wide — a short bridge (see §6).

An optional **hub** — a plain cylinder of radius `hub_radius` from `z = −A` to `z = H` — is a second,
disjoint revolved solid, with its own dilated bore. It keeps the joint from collapsing inwards on
large cuts.

### 1.4 Ball & socket

Profile: a neck cylinder of radius `Rn` from `z = −A` up to the sphere, then the sphere of radius
`Rb` centred at `zb`. Both derived from the **opening half angle** `θ`, which is the parameter the
user actually turns:

```
Rn = (Rb + C)·sin θ − C          clamped to [0.25·Rb, 0.85·Rb]
zb = C + (Rb + C)·cos θ + 0.2
```

Picking `zb` that way puts the neck/sphere junction of the *dilated* body at `z = C + 0.2`, i.e.
just above the female's face, so the socket mouth is the neck channel of radius `Rn + C`, which is
smaller than `Rb` — the ball cannot come out. The socket roof is a dome, so it prints unsupported.

### 1.5 Defaults

| Parameter | Double ring | Ball & socket | Note |
|---|---|---|---|
| Outer radius / ball radius | `0.4 × inscribed radius` (4.0 mm on a 20 mm cylinder) | same | auto, manual override |
| Ring width `W` | 2.0 mm | — | radial thickness of the lip head |
| Ring height `H` | 1.5 mm | — | protrusion above the plane |
| Neck ratio | 0.45 | — | stem width as a fraction of `W`; this is the undercut |
| Opening half angle `θ` | — | 40° | bounds the range of motion |
| Clearance `C` | 0.35 mm | 0.35 mm | floor = `0.75 × nozzle diameter` (see below) |
| Tilt allowance | 0.20 mm | 0.20 mm | extra headroom in the cavity ceiling |
| Hub radius | `Ro − W − C − W`, clamped ≥ 0 (so **0**, i.e. no hub, at the 20 mm-cylinder defaults) | — | 0 disables |

**Clearance floor — a deliberate deviation from the brief.** The brief said "floor = nozzle
diameter". Taken literally that makes the stated 0.35 mm default unreachable on the standard 0.4 mm
nozzle: every joint would silently become 0.4 mm. The floor implemented is `0.75 × nozzle diameter`
(0.30 mm on a 0.4 nozzle, 0.45 mm on a 0.6), which is the upper end of the 0.5–0.75× range the
research settled on and keeps the documented default usable. If the owner wants the literal
nozzle-diameter floor, it is one constant in `flexi_clearance_floor()`.

## 2. Where it lives

| File | What changed |
|---|---|
| `src/libslic3r/FlexiJoint.{hpp,cpp}` | **new** — `FlexiJointKind`, `FlexiJointParams`, the profiles, the Clipper dilation, auto-sizing, the guards |
| `src/libslic3r/TriangleMesh.{hpp,cpp}` | **new** `its_make_revolved()` — the generic ring/lathe generator, next to `its_make_torus` |
| `src/libslic3r/Model.hpp` | `CutConnectorType::FlexiJoint` (appended **after** `Undef`); `FlexiJointParams` on `CutConnector` and on `ModelVolume::CutInfo`, both cereal-serialized |
| `src/libslic3r/CutUtils.{hpp,cpp}` | `add_flexi_joint_volume()`, `has_flexi_joint()`, `Cut::perform_with_flexi_joints()`; `perform_with_plane()` dispatches to it |
| `src/slic3r/GUI/Gizmos/GLGizmoCut.{hpp,cpp}` | the "Flexi joint" radio entry, the joint-kind combo, the parameter panel, auto-size, the preview shape, forced keep-as-parts |
| `tests/libslic3r/test_flexi_joint.cpp` | **new** — 8 Catch2 cases |

`CutConnectorType::FlexiJoint` is appended after `Undef` on purpose: `bbs_3mf.cpp` validates loaded
connector types against `Undef`, and inserting before it would renumber `Undef` in every existing
3MF. No flexi connector ever reaches a 3MF anyway — the flexi cut consumes the connector volume and
writes out two plain `MODEL_PART`s.

## 3. Negative volume, or a real boolean? — a real boolean

**Decision: an eager Manifold boolean (`MeshBoolean::mfd::`, falling back to `mcut::`), producing
two watertight `MODEL_PART` volumes.** Three reasons:

1. **The clearance cannot be expressed by the existing mechanism.** `apply_tolerance()` inflates a
   connector by *adding* the tolerance to the volume's XY/Z scale factors. For a cylinder that
   happens to equal a uniform offset; for a ring it does not (it moves the mean radius too), and for
   anything with an undercut it certainly does not. The cavity has to be a separately generated
   body, which means the female side needs a *different mesh* from the male side — which the live
   negative-volume path cannot carry (it copies one volume to both halves).
2. **It has to be verifiable at cut time.** Every acceptance test the owner asked for — watertight,
   min distance == C, intersection volume == 0, rotation sweep, tilt — needs two actual meshes.
   Left as live negative volumes, the CSG only resolves at slice time, hours of debugging away from
   the thing being wrong.
3. **The fallback chain already exists and is already trusted.** The Mesh Boolean gizmo runs
   `mfd::` first and `mcut::` on failure; the flexi cut uses the same helper.

Cost: the cut is slower (four booleans at worst) and the halves are baked, so changing the clearance
means undo + re-cut. Acceptable for phase 1.

## 4. How it interacts with the other Cut options

| Option | Behaviour with a Flexi joint |
|---|---|
| **Cut to parts** | Forced **on** and disabled in the UI. A flexi joint that lands as two free-floating objects is not a joint — auto-arrange scatters the halves (PrusaSlicer #10656). `Cut::perform_with_flexi_joints()` ignores the attribute bits entirely and always builds one object with two volumes. |
| **Keep upper / Keep lower** | Both forced on, same reason. |
| **Place on cut / Flip upper / lower** | Not applied. The two halves keep one shared instance transform. |
| **Dowels** | Not applicable; a flexi joint is never a loose part. |
| **Mixing with Plug/Dowel/Snap connectors on the same cut** | The flexi path takes over the whole cut and ignores the other connector volumes. Phase 1 does not support mixing. |
| **Tongue and groove (Dovetail) mode** | Unrelated code path; the flexi family is planar-cut only. |
| **Cut by contour / part selection** | Not supported in phase 1 — `perform_by_contour()` is untouched, so a flexi joint placed while a part selection is active takes the plain-plane path. |
| **Multiple flexi joints on one cut** | Phase 1 applies the **first** and logs a warning for the rest. |
| **Multi-volume objects** | All solid parts are merged into one mesh per side; the result is two parts, not (parts × 2). Per-volume config follows the first solid volume. Modifiers are carried over to the resulting object. |

## 5. The guards

* **Gap closing radius (BambuStudio #182).** The slicer's `slice_closing_radius` fills cracks
  smaller than twice its value, which welds a print-in-place clearance shut layer by layer.
  `flexi_gap_closing_conflict(p, r)` is true when `r ≥ C/2`; the gizmo shows an orange warning
  naming the current radius and the largest safe one (`C/2`). At the shipped default
  (`slice_closing_radius = 0.049 mm`) and `C = 0.35 mm` there is a 3.5× margin, so the warning does
  **not** fire on a stock profile — it fires when someone raises the radius, or drops the clearance
  below ~0.1 mm.
* **Clearance floor from the nozzle** — see §1.5.
* **`flexi_validate()`** refuses zero clearance, a ring narrower than the clearance, a hub with no
  room beside it, an out-of-range opening angle, and — the important one — any parameter set where
  the mouth is not narrower than the lip, i.e. a joint that would pull apart.
* **Fits inside the cross-section**: the joint sets `connector.radius = flexi_outer_extent()` and
  `connector.height = flexi_protrusion_height()`, so the gizmo's existing
  `is_outside_of_cut_contour()` / `is_conflict_for_connector()` checks flag an oversized joint the
  same way they flag an oversized plug.

## 6. Support settings for the groove's ceiling

The female cavity is an internal overhang that no tool can reach once the joint is closed, so it
must **never** get support:

* **Print with supports off** for a flexi-jointed model whenever the rest of the model allows it.
  This is what the CLI proofs in this branch use.
* If the model needs support elsewhere, use **"Support on build plate only"** so nothing is
  generated inside the object, and check the preview at the joint layer.
* The Double ring's groove ceiling is a flat annular bridge of about `ring_width + 2·clearance`
  (2.7 mm at the defaults) — well inside normal bridging range at 0.2 mm layers.
* The Ball & socket's roof is a dome and self-supports.
* The joint bodies are consumed by the cut, so there is no negative volume left for the support
  generator to see; the cavity is simply absent material inside a normal model part.

## 7. What is proven, and what is not

Proven by `libslic3r_tests [FlexiJoint]` (8 cases, 117 assertions, all green):

* `its_make_revolved` is watertight in both topologies and matches Pappus / the sphere volume.
* Both joint types' male bodies and female cavities are watertight, positive-volume and captive.
* Cutting a 20 mm × 20 mm cylinder at mid height with either joint yields **one object, two
  watertight parts**.
* Measured min distance between the two parts: **0.34988 mm** against a declared 0.35 mm.
* Boolean **intersection volume == 0** at rest, and at every 15° of a full rotation sweep.
* Rotation sweep: min distance stays ≥ `C − 0.02` through 360°.
* Tilt: no intersection at 0.7 × the flat-face allowance; 8.8 mm³ of intersection at 2 × it, so the
  allowance really does bound the motion.
* 3MF round trip: store + load keeps one object with two watertight model parts.
* The guards behave as documented.

Proven by the CLI, on **Bambu Lab P1S 0.4 nozzle / 0.20mm Standard @BBL X1C / Bambu PLA Basic**,
supports off (`enable_support = 0` in every G-code header):

* Both jointed cylinders slice with `return_code 0`. Each raises the expected
  *"object has floating regions"* warning — that is what a print-in-place joint is: the female half
  bridges over the 0.35 mm gap.
* **The gap survives slicing.** In the Double ring G-code the two layers that span the face gap
  (Z = 10.20 and Z = 10.40, between the male's face at 10.00 and the female's at 10.35) contain
  extrusion only between radius **2.76 and 3.24 mm** — the ring lip at its 3.0 mm mean radius.
  Nothing at all is laid down across the rest of the 20 mm face. The full disc returns at Z = 10.60,
  where the female half begins.
* **The gap-closing guard's threshold is exactly right.** Same model, same printer, only
  `slice_closing_radius` changed:

  | `slice_closing_radius` | guard verdict (`r ≥ C/2 = 0.175`) | extrusion moves in the groove band (r 1.8–4.4 mm) at Z = 11.0 / 11.6 | filament |
  |---|---|---|---|
  | 0.049 (shipped default) | quiet | **118 / 150** — the groove is there | 3.41 g |
  | 0.25 | fires | **0 / 0** — the groove is gone, the female half slices as a solid disc | 3.64 g |
  | 1.0 | fires | **0 / 0** | 3.64 g |

  So the warning is not decorative: crossing `C/2` really does weld the joint shut, exactly as
  BambuStudio #182 describes, and the stock P1S profile sits 3.5x below the line.
* **No effect on unrelated models.** 3DBenchy sliced on the same preset by this branch and by a
  head-of-fork baseline install differs only in the timestamp comment and two config-comment lines
  (`preload_all_filaments`, `unload_filaments_at_end`) that the baseline binary predates. All
  107,781 remaining lines — every extrusion move — are byte-identical.
* **A hidden scratch instance starts.** `run_control_app.py` against the scratch install and a data
  dir copied from `dd_lan`, loading `flexi_ring.3mf`: PID alive, `Responding = True`, no window, log
  reaching `post_init: hidden instance warm-up = full`. The only errors in the log are the usual
  "can not find parent for config" noise from user presets in the copied data dir.

**Nobody clicked the gizmo.** The UI code compiles and the app builds and starts, but no human or
agent has opened the Cut gizmo, picked "Flexi joint", dragged it, or pressed Apply. The owner's
click test is §8.

Also unverified: nothing has been printed. The clearance defaults come from the research's survey of
maker sources, not from a print on this owner's machine.

## 8. The owner's click test

1. Load `resources/handy_models/` → any 20 mm calibration cylinder, or drop in a 20 mm ⌀ × 20 mm
   cylinder. Printer preset **Bambu Lab P1S 0.4 nozzle**, 0.2 mm layer, PLA.
2. Open the **Cut** gizmo, leave the plane at mid height (Z = 10 mm).
3. Click **Connectors** (the pencil / "Add connectors" step).
4. In **Type**, pick **Flexi joint**. **Joint** should read *Double ring*; leave **Auto size from
   the cut cross-section** ticked. Confirm the panel shows: Outer radius 4.00, Ring width 2.00,
   Ring height 1.50, Clearance 0.35, Tilt allowance 0.20, Hub radius 0.00.
5. Click on the cut plane at its centre to place the joint. The preview should be a ring at
   ~4 mm radius; it turns red if you drag it past the edge of the cross-section.
6. **Confirm connectors**, then **Perform cut**.
7. Expected: **one object** in the object list with **two parts** (`cyl_A`, `cyl_B`) — not two
   objects. "Cut to parts" is ticked and greyed out.
8. Slice with **supports off**. In preview, step to the layer at Z ≈ 10.4 mm and confirm the groove
   reads as a real gap, not a fused line.
9. Print. Off the bed the two halves should **rotate freely** about the joint axis and **rock a
   degree or two**, and must **not** pull apart. No knife, no picking.
10. Repeat 2–9 with **Joint = Ball & socket** (Ball radius 4.00, Opening angle 40°, Clearance 0.35).
    The joint should swivel in every direction up to roughly the opening angle and stay captive.

If a joint prints fused: raise the clearance to 0.40–0.45 mm and check that
Quality → *Slice gap closing radius* is at or below 0.05 mm.

## 9. Phase 2

* **Hinge**: a pin plus alternating knuckles, sweeping about the pin axis only. The generator
  (`its_make_revolved` plus a boolean) already covers the shapes; it needs a knuckle count and an
  axial gap, and a hinge axis that lies *in* the cut plane rather than along its normal.
* **Multiple joints along a wide cut**: place N joints evenly along the cross-section's major axis,
  one per lobe on multi-lobe sections, validated against the existing
  `is_conflict_for_connector()` / `is_outside_of_cut_contour()` checks. `CutConnectorMode::Auto` is
  dead code in this fork, so this has to be written, not re-enabled.
* **Contour-exact auto sizing**: phase 1 approximates the cross-section's inscribed radius by half
  the smaller side of the object's bounding box in the cut frame. Use the object clipper's actual
  contour and the largest inscribed circle at the centroid instead, and refuse the default
  placement (rather than shrinking) when the section is too thin.
* **Dragging and sizing UI polish**: a live radius handle on the plane, a numeric readout of the
  resulting mouth/lip margin, and a warning when the joint's outer extent leaves less than one
  perimeter of wall.
* **Mixing** flexi joints with Plug/Dowel/Snap on one cut, and flexi joints under *cut by contour*.
* **Per-volume fidelity**: keep multi-part objects as parts × 2 instead of merging.
