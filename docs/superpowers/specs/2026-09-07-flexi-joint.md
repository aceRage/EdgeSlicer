# Flexi — a print-in-place articulated joint in the Cut gizmo

Phase 1: branch `feat/flexi-joint-p1`, from `feat/ultra-preferences` at `67a2d25eb3`.
Phase 2: branch `feat/flexi-p2`, from `feat/ultra-preferences` at `32ddb61631`.

**The connector family is called "Flexi" in the UI.** It read "Flexi joint" in phase 1; the
owner asked for the shorter name, so the radio button, the panel and this document say Flexi.
The code identifiers keep the longer spelling — `CutConnectorType::FlexiJoint`,
`FlexiJointKind`, `FlexiJointParams`, `FlexiJoint.{hpp,cpp}` — because renaming them is
churn with no user-visible effect.

Phase 2 adds a third joint kind (**Chain link**), a **Gap** parameter for every kind, and fixes
the bug the owner hit in phase 1: after one Flexi cut the connector option stayed greyed out
until the slicer was restarted (§10).

## The short version

The Cut gizmo can already insert a parametric body that straddles the cut plane and comes out solid
on one half and a clearance-inflated void on the other — that is what Plug and Snap connectors are.
A flexi joint is the same idea with two changes that matter: the male body has an **undercut**, so
the two halves cannot pull apart, and the clearance has to be **exactly the same on every face**, so
the halves print in place without fusing.

The connector family **Flexi** now has three joint types:

* **Chain link** (phase 2, the one the owner actually wanted): two interlocking closed loops, one
  lying in the cut plane and one standing along the cut normal, so the two segments hinge *and*
  swivel the way two links of a chain do. §1.6.
* **Double ring** (the primary of phase 1, the one on articulated dragons and rexes): an annular
  dovetail lip on one segment nesting in an annular groove on the other, plus an optional solid
  hub.
* **Ball & socket**: a ball on a neck captured by a socket whose mouth is narrower than the ball.

All three produce **one object with two watertight model parts**. Nothing is left as a live
negative volume — see §3 for why.

Every kind also takes a **Gap** (§1.7): the thickness of the cut itself, i.e. how far apart the
two segments' flat faces end up. Without it a joint whose faces are only a clearance apart barely
moves, which is what the owner reported after printing phase 1.

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

| Parameter | Double ring | Ball & socket | Chain link | Note |
|---|---|---|---|---|
| Outer radius / ball radius | `0.4 × inscribed radius` (4.0 mm on a 20 mm cylinder) | same | — | auto, manual override |
| Ring width `W` | 2.0 mm | — | — | radial thickness of the lip head |
| Ring height `H` | 1.5 mm | — | — | protrusion above the plane |
| Neck ratio | 0.45 | — | — | stem width as a fraction of `W`; this is the undercut |
| Opening half angle `θ` | — | 40° | — | bounds the range of motion |
| Link length | — | — | 7.0 mm | overall length of each loop's centreline |
| Link width | — | — | 5.5 mm | overall width; both must be ≥ `4·wire + 2·C` = 4.7 mm |
| Wire thickness | — | — | 1.0 mm | tube radius of the loop wire |
| Loop tilt | — | — | 7° | how far the horizontal loop tilts up out of the cut plane |
| Stem depth | — | — | 1.5 mm | how deep each loop's far end sits in its own segment |
| Clearance `C` | 0.35 mm | 0.35 mm | 0.35 mm | floor = `0.75 × nozzle diameter` (see below) |
| **Gap** | **0.6 mm** | **0.6 mm** | **1.5 mm** | thickness of the cut; floor = `C` (§1.7) |
| Tilt allowance | 0.20 mm | 0.20 mm | — | extra headroom in the cavity ceiling |
| Hub radius | `Ro − W − C − W`, clamped ≥ 0 (so **0**, i.e. no hub, at the 20 mm-cylinder defaults) | — | — | 0 disables |

At the chain-link defaults the assembly's outer extent is **5.86 mm**, so it fits a 20 mm cylinder
with 4 mm of wall to spare, and the two loops clear each other by **0.72 mm** against a declared
0.35 mm — the rest position is not the tight one, which is what lets the joint swing.

**Clearance floor — a deliberate deviation from the brief.** The brief said "floor = nozzle
diameter". Taken literally that makes the stated 0.35 mm default unreachable on the standard 0.4 mm
nozzle: every joint would silently become 0.4 mm. The floor implemented is `0.75 × nozzle diameter`
(0.30 mm on a 0.4 nozzle, 0.45 mm on a 0.6), which is the upper end of the 0.5–0.75× range the
research settled on and keeps the documented default usable. If the owner wants the literal
nozzle-diameter floor, it is one constant in `flexi_clearance_floor()`.

### 1.6 Chain link (phase 2)

Two interlocking closed loops. Each loop's centreline is a **stadium** — two parallel straights
joined by semicircular caps, overall length `link_length` and width `link_width` — swept with a
tube of radius `wire`. A stadium has no corners for the swept tube to pinch at, and the two caps
are what the other loop threads.

```
                                     the horizontal loop, tilted up by tilt_angle,
                                     its far (+x) end embedded in the UPPER segment
              .-------------------------------.
   z=+gap/2  =============================================  upper segment face
             |    ,--.                        |
             |   /    \   <- the vertical loop, standing in the x-z plane,
             |  |      |     threaded through the horizontal one
             |   \    /
             |    `--'
   z=-gap/2  =============================================  lower segment face
                  |  |   <- its far (bottom) end embedded `stem` deep in the LOWER segment
```

* The **vertical loop belongs to the lower segment**. It stands in the x–z plane (normal +Y),
  long axis +Z, centre at `z = -gap/2 - stem + link_length/2`, so its bottom centreline extremity
  sits `stem` below the lower face and is unioned into the lower body. Its upper half is free.
* The **horizontal loop belongs to the upper segment**. Long axis +X, tilted up by `tilt_angle`
  about +Y so its far (+x) end rises into the upper body while its near half stays free and
  threads the vertical loop. Its plane sits at `z = +gap/2 + wire`, i.e. immediately on top of the
  upper face.
* The whole assembly is then shifted in x so it is **centred on the joint axis**, which keeps the
  joint from being lopsided and makes the gizmo's "fits inside the cross-section" test mean what
  it says.

**Why those roles and not the reverse.** Printing runs bottom-up. A loop belonging to the upper
segment that hung *down* into the gap would have its lowest material printed over thin air. So the
vertical loop has to grow *out of* the lower segment (its top span is then a short, self-supporting
bridge — the owner's words), and the horizontal loop has to lie flat *on* the upper segment's first
layers, where each of its layers is a closed ring resting on the one below. That is exactly the
"far end embedded in the segment, the near half free" the owner described, for both loops.

**The two sizing constraints.** A loop's hole is a stadium of length `link_length - 2*wire` and
width `link_width - 2*wire`. The other loop's wire (radius `wire`) has to pass through it with the
clearance `C` to spare on both sides, in both directions:

> `link_length >= 4*wire + 2*C` and `link_width >= 4*wire + 2*C`

`flexi_min_link_size()` returns that bound and `flexi_validate()` refuses anything under it. At the
defaults it is `4*1.0 + 2*0.35 = 4.7 mm`, comfortably under the 7.0 / 5.5 the loops actually get.

**The clearance is exact, without any Clipper work.** A uniform offset of a tube *is* a fatter
tube, so the relief carved out of each segment for the other's loop is simply the same centreline
swept with radius `wire + C`. Both loops and both reliefs use the same wire-sector and cap-step
counts, so a loop and the relief made for it stay facet-parallel and the measured clearance is the
true `C` rather than `C ± the faceting error` — the same discipline §1.2 applies to the revolved
kinds, by a different route.

**The construction, in one line:**

> Lower half = (object ∩ {z ≤ -gap/2}) ∪ V, minus dilate(H, C).
> Upper half = (object ∩ {z ≥ +gap/2}) ∪ H, minus dilate(V, C).

where `V` is the vertical loop and `H` the horizontal one. Both segments get a body *and* a relief,
which is the structural difference from the revolved kinds (a protrusion on one side, a cavity on
the other). `flexi_lower_bodies()` / `flexi_upper_bodies()` / `flexi_lower_reliefs()` /
`flexi_upper_reliefs()` express the general case; for Double ring and Ball & socket the upper-body
and lower-relief lists are empty and the whole thing collapses back to phase 1's behaviour.

**Non-separable, and why the pull test is bounded.** The loops are *linked*: each threads the
other's hole, so no rigid motion frees them. The Catch2 pull test translates the lower part along
the cut normal and requires an intersection all the way out to the point where the vertical loop's
hole top would have dropped past the horizontal loop's wire (3.0 mm at the defaults, against the
owner's `gap + clearance` = 1.85 mm test). Past that bound a *pure translation* of one mesh through
another stops reporting overlap — the centrelines have passed straight through each other, which no
rigid body can do — so the test stops there rather than asserting something a mesh-intersection
check cannot see. Sideways pulls are checked too.

### 1.7 Gap (phase 2, all kinds)

The owner's report: *"if the gap between faces is only very small it isn't very flexible in
practice."* In phase 1 the two flat faces ended up exactly one clearance (0.35 mm) apart, because
the female half simply started one clearance higher. That is enough to keep the halves from fusing
and nowhere near enough to let the segment visibly bend.

**Gap is now the thickness of the cut itself**: each face is set back from the cut plane by
`gap/2`, so the lower segment ends at `z = -gap/2`, the upper starts at `z = +gap/2`, and the joint
bodies bridge the distance.

* **Floor = the clearance.** Below that the slicer's gap closing would weld the faces and there
  would be no gap at all. `flexi_effective_gap()` enforces it and `flexi_validate()` refuses it.
* **Default 1.5 mm for Chain link.** The loops have to swing *through* each other, so the segments
  need real room between them; 1.5 mm is about one wire diameter and reads as an obvious hinge.
* **Default 0.6 mm for Double ring and Ball & socket.** Those only rotate and rock in place, so the
  gap just has to stop the two flat faces rubbing. 0.6 mm is three 0.2 mm layers — wide enough to
  survive slicing with margin, small enough that the joint does not look like a mistake. (0.35, the
  phase 1 value, is the floor, not a good default; anything past ~1 mm on a ring starts to look
  like a gap rather than a joint.)

**Auto-sizing spans the gap.** For the revolved kinds the whole `(r, z)` profile is lifted by
`gap/2` (`flexi_revolved_z_shift()`), so the lip's straight stem starts at the *female face* rather
than at the cut plane, and the anchor is deepened by the gap so the body still reaches down into
the male segment. The undercut geometry — which is what makes the joint captive — is untouched by
the gap; the gap only lengthens the stem that crosses it. For the chain link the gap enters the
frame directly (§1.6).

The rock allowance changes with it: on a full-diameter cut the rim at radius `R` has to climb the
gap, so the tilt the flat faces permit is `asin(gap / R)`, not `asin(C / R)`.

### 1.8 Chain link, corrected: the two ring planes (phase 3)

**The owner's phase 2 report.** *"In the 3D view the chain link shows one ring lying flat in the
cut plane and the other looking like a small horizontal torus — it's orienting the vertical ring to
print horizontally."* And, separately: *"there is no way to rotate the connector."*

**The cause.** §1.6 gave the two loops different roles on purpose — one standing, one lying flat —
and that was the bug. `chain_horizontal_path()` mapped the upper loop's stadium long axis to `+X`
and its width to `+Y`, so the loop's plane was spanned by `(d, n × d)`: it **was the cut plane**.
Only the lower loop actually crossed the plane. Two consequences followed:

* the joint hinged about **one** axis (`+Y`) rather than two — a hinge, not a chain link;
* the flat loop had to print as a horizontal torus sitting on the upper face, which is what the
  owner saw in the viewport.

**The rule.** For a chain link at a plane with normal `n` and a unit reference direction `d` lying
in that plane:

> **Both** ring planes contain `n`, and the two planes are **perpendicular to each other**.
> Ring A lies in span(`n`, `d`) and is offset along `-n`; ring B lies in span(`n`, `n × d`) and is
> offset along `+n`. Each ring's free half crosses the cut plane and threads the other's hole.

That is what a chain link is: two rings whose planes are at right angles, interlocked. Neither ring
lies *in* the cut plane; both pass *through* it.

```
        looking along +Y                        looking along +X
   (ring B is edge-on: a line)              (ring A is edge-on: a line)

          ,--.  ring A, in span(n,d)                ,--.  ring B, in span(n, n x d)
         /    \  centred at z = -off                /    \  centred at z = +off
   =====|======|=====  z = 0                  =====|======|=====  z = 0
         \    /                                     \    /
          `--'  its bottom is embedded               `--'  its top is embedded
                in the LOWER half                          in the UPPER half
```

**The offset.** Each ring is centred `off` from the cut plane on **its own** side — ring A at
`z = -off`, ring B at `z = +off`. That single number is what puts the two centroids on opposite
sides while both rings still cross the plane, and it has to satisfy three constraints at once
(`chain_frame()` in `FlexiJoint.cpp`):

| # | constraint | why |
|---|------------|-----|
| 1 | `off >= gap/2 + wire` | the ring's centre clears its own segment's face, so the ring is centred in its own half rather than in the gap |
| 2 | `off >= stem + gap/2 - link_length/2` | the ring's far end is embedded at least `stem` deep in its own segment, which is what attaches it |
| 3 | `off <= (link_length - wire)/2 - C` | the rings still **thread**: ring B's free end reaches below the top of ring A's hole with the clearance to spare |

The preferred value is `link_length/4` — a quarter length puts the centroids half a length apart
and leaves each ring's free half well inside the other — clamped into `[max(1,2), 3]`. At the
defaults (`L = 7`, `wire = 1`, `gap = 1.5`, `stem = 1.5`, `C = 0.35`) that is `off = 1.75 mm`, with
bounds `[1.75, 2.65]` and an embed depth of `4.5 mm`. When constraint 3 falls below 1 and 2 the
parameters cannot make an interlocked joint at all, and `flexi_validate()` refuses them with a
message naming what to change.

`tilt_angle` keeps its job but changes meaning: it now tips ring B about `n × d` — **inside its own
plane's family** — so ring B's free end leans clear of ring A instead of sitting dead concentric
with it. `0` is legal and puts both rings on the same axis.

**What this replaces.** §1.6's "vertical loop / horizontal loop" split, its `x_shift` centring and
its `hcx = L/2` nesting offset are all gone; §1.6 stands as the record of what phase 2 built and
why it was wrong. The construction line is otherwise unchanged — lower half = (object ∩ {z ≤
−gap/2}) ∪ A minus dilate(B, C), upper half = (object ∩ {z ≥ +gap/2}) ∪ B minus dilate(A, C) —
with `A`/`B` now naming the two upright rings. `flexi_min_link_size()` and the two sizing
constraints of §1.6 are unchanged and still enforced.

### 1.9 Rotation (phase 3)

`FlexiJointParams::rotation` — **degrees, 0–180, default 0**, about the cut normal `n`. The
reference direction is `d = rotate(d0, n, rotation)` with `d0` the cut plane's own `+X` axis, so
`rotation = 0` reproduces exactly the orientation every joint had before the field existed.

* **Chain link honours it.** Both rings turn *together*, which is why their planes stay
  perpendicular to each other; the parameter only chooses where in the cut plane the pair sits.
  A rotation of 90° swaps the two ring planes, which is what the unit test asserts.
* **Double ring and Ball & socket ignore it**, and are documented as doing so. They are solids of
  revolution about `n`, so rotating them produces the identical mesh — the test checks that the
  body volumes are unchanged at 0° and 90°.
* It is applied **last**, as a rotation about `+Z` in the cut frame, to the ring centrelines
  (`chain_apply_rotation()`), so every downstream measure — the swept bodies, the reliefs, the
  preview — follows for free. `flexi_outer_extent()` is measured about the axis and is therefore
  rotation-invariant, which is what keeps the gizmo's "fits inside the cross-section" check from
  moving as the user drags the slider.

**In the UI.** A *Rotation* row in the Cut gizmo's connector panel, shown only when a Flexi
connector is selected and only for the Chain link kind (the other two would do nothing). It is a
slider plus a numeric field, laid out by `render_flexi_rotation_input()` to match the plain
connector's own *Rotation* row, and it applies to the selected connectors exactly the way
Depth / Size / Gap already do — `sync_flexi_params()` copies `m_flexi` onto every selected flexi
connector and then calls `update_connector_shape()`, so the preview body in the 3D view is rebuilt
and the rings visibly turn as the slider moves.

> **Units caution.** `FlexiJointParams::rotation` is in **degrees**. `CutConnector::z_angle`, which
> the plain connector's identically-named row drives, is in **radians**. They are different fields
> with different units; flexi connectors leave `z_angle` at 0 and carry their rotation in the
> geometry, so there is no double rotation.

**Persistence.** The joint's parameters were not written to 3MF at all before phase 3 — a project
saved with a flexi connector still placed, then reopened, came back with default parameters and a
silently different joint. `Metadata/cut_information.xml` now carries the whole `FlexiJointParams`
as attributes on the `<connector>` element, written only for the flexi connector type so nothing
changes in a file that has no flexi joint:

`flexi_kind`, `flexi_outer_radius`, `flexi_ring_width`, `flexi_ring_height`, `flexi_clearance`,
`flexi_gap`, `flexi_hub_radius`, `flexi_tilt`, `flexi_neck_ratio`, `flexi_open_angle`,
`flexi_link_length`, `flexi_link_width`, `flexi_wire`, `flexi_tilt_angle`, `flexi_stem`,
**`flexi_rotation`**.

The reader treats each as optional with the `FlexiJointParams` default as the fallback, and gates
the whole block on `flexi_kind` being present, so an older 3MF loads exactly as it used to — with
`rotation = 0`, the orientation it was always built with. `flexi_kind` is range-checked before the
enum cast, following the same untrusted-input discipline the surrounding `volume_id` / `type`
checks already use. The cereal archive (`FlexiJointParams::serialize`, used by undo/redo
snapshots) gains `rotation` at the end of its field list.

### 1.10 Printability of the two rings (phase 3)

Both rings now stand vertical, so each prints the way the phase 2 *standing* loop already did:
growing up out of its own segment, with a short self-supporting bridge at the top of its arc. That
is a real change of character for ring B, which used to lie flat and print as stacked closed rings
with no bridging at all.

The trade is deliberate and the interlock had to win: a ring lying in the cut plane cannot be part
of a chain link, whatever it costs to print. What it costs is one overhanging arc per ring. A
stadium cap of radius `link_width/2` swept with a `wire`-radius tube presents its steepest wall
near the cap's equator and flattens to horizontal only at the very top, where the span is short
(about twice the wire radius at the apex) and bridges on a typical 0.2 mm layer at the default
`wire = 1.0 mm`. Thinner wire makes the bridge shorter still.

Two things follow that are **not** automated:

* **Bed orientation matters and nothing enforces it.** The rings' planes contain the cut normal, so
  if the cut plane is horizontal (the common case) both rings stand upright and print well. A cut
  at a steep angle tips the rings with it, and at some angle a ring's arc becomes a genuine
  overhang. `rotation` turns the pair *within* the cut plane and cannot fix that; only reorienting
  the object on the bed can.
* **No support interaction is computed for the rings.** §6 covers the groove ceiling for the
  revolved kinds; the chain link's arcs are assumed self-supporting and nothing checks that against
  the actual layer height or overhang-angle settings.

## 2. Where it lives

| File | What changed |
|---|---|
| `src/libslic3r/FlexiJoint.{hpp,cpp}` | **new** — `FlexiJointKind`, `FlexiJointParams`, the profiles, the Clipper dilation, auto-sizing, the guards |
| `src/libslic3r/TriangleMesh.{hpp,cpp}` | **new** `its_make_revolved()` — the generic ring/lathe generator, next to `its_make_torus`; **phase 2** adds `its_make_swept_loop()` — a closed tube swept along a closed 3D polyline with a rotation-minimising frame whose residual twist is spread over the loop so the seam closes watertight |
| `src/libslic3r/Model.hpp` | `CutConnectorType::FlexiJoint` (appended **after** `Undef`); `FlexiJointParams` on `CutConnector` and on `ModelVolume::CutInfo`, both cereal-serialized |
| `src/libslic3r/CutUtils.{hpp,cpp}` | `add_flexi_joint_volume()`, `has_flexi_joint()`, `Cut::perform_with_flexi_joints()`; `perform_with_plane()` dispatches to it |
| `src/slic3r/GUI/Gizmos/GLGizmoCut.{hpp,cpp}` | the "Flexi joint" radio entry, the joint-kind combo, the parameter panel, auto-size, the preview shape, forced keep-as-parts |
| `tests/libslic3r/test_flexi_joint.cpp` | **new** — 8 Catch2 cases in phase 1, **16** after phase 2 |

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
| **Multiple flexi joints on one cut** | Applies the **first** and logs a warning for the rest. |
| **Which segment carries what** | Double ring / Ball & socket put the solid body on the lower half and the cavity on the upper. **Chain link puts a body on both** and relieves each with the other's clearance-inflated loop; `flexi_is_two_sided()` says which. |
| **Multi-volume objects** | All solid parts are merged into one mesh per side; the result is two parts, not (parts × 2). Per-volume config follows the first solid volume. Modifiers are carried over to the resulting object. |

## 5. The guards

* **Gap closing radius (BambuStudio #182).** The slicer's `slice_closing_radius` fills cracks
  smaller than twice its value, which welds a print-in-place clearance shut layer by layer.
  `flexi_gap_closing_conflict(p, r)` is true when `r ≥ C/2`; the gizmo shows an orange warning
  naming the current radius and the largest safe one (`C/2`). At the shipped default
  (`slice_closing_radius = 0.049 mm`) and `C = 0.35 mm` there is a 3.5× margin, so the warning does
  **not** fire on a stock profile — it fires when someone raises the radius, or drops the clearance
  below ~0.1 mm.
  Note the Gap makes this less catastrophic than it was in phase 1: with a gap wider than the
  clearance, crossing the threshold welds the *groove* but leaves the two segments separated by the
  gap, so the joint comes out stiff rather than solid (measured in §7).
* **Clearance floor from the nozzle** — see §1.5.
* **Gap floor = the clearance** — see §1.7. `flexi_effective_gap()` clamps it and
  `flexi_validate()` refuses an explicitly smaller one.
* **`flexi_validate()`** refuses zero clearance, a gap below the clearance, a ring narrower than the
  clearance, a hub with no room beside it, an out-of-range opening angle, and — the important one —
  any parameter set where the mouth is not narrower than the lip, i.e. a joint that would pull
  apart. For the Chain link it additionally refuses a non-positive wire or stem, a tilt outside
  0–30°, a length shorter than the width, and loops smaller than `4·wire + 2·C`, which is the
  sizing rule that makes the interlock possible (§1.6).
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
* The **Chain link** has no enclosed cavity at all, so there is nothing for support to get trapped
  inside. Its two overhangs are both deliberate and both short: the vertical loop's **top span**,
  a bridge of `link_width` (5.5 mm at the defaults) closing over the loop's own two sides, and the
  horizontal loop's free half, which is a flat cantilever anchored at its far end and closing back
  on itself. Both are inside normal bridging range at 0.2 mm layers. Supports off is still the
  right setting: support under the horizontal loop would fuse it to the segment below.
* The joint bodies are consumed by the cut, so there is no negative volume left for the support
  generator to see; the cavity is simply absent material inside a normal model part.

## 7. What is proven, and what is not

### Phase 3

`libslic3r_tests` as a whole: **721 cases, 719 passed, 2 failed as expected** (the same two
pre-existing expected failures; phase 2's baseline was 706 + the same 2). `[FlexiJoint]` alone:
**21 cases, 366 assertions, all green** — 16 from phase 2, 5 new, and three of the phase 2 cases
updated for the corrected geometry.

The ring-plane assertions use a principal-component fit of each ring mesh's vertices: a ring is a
flat loop swept with a tube, so its vertex cloud is thinnest along its plane's normal, and the
covariance eigenvector for the smallest eigenvalue *is* that normal. `fit_plane()` in
`test_flexi_joint.cpp` returns it together with the residual, which is checked to be well under the
in-plane spread first — otherwise "the normal" would be meaningless.

New in phase 3:

* **Both ring planes contain `n`.** `|normal · n| = 0` to within `1e-3` for *both* rings, and the
  two ring normals are perpendicular to each other to the same tolerance. This is the assertion
  phase 2 failed: its upper ring's normal *was* `n`, so the dot product was 1.
* **Opposite sides.** The two ring centroids sit on opposite sides of the cut plane (`z < 0` and
  `z > 0`), and with the default parameters they are exact mirror images (`±off`, `off = 1.75 mm`),
  centred on the joint axis in x and y.
* **They do not touch**, at gaps of 0.2, 0.35, 1.5 and 2.5 mm: Manifold intersection volume 0.
  The phase 2 clearance case (`min_surface_distance ≥ C`) also passes again — the corrected tilt
  axis is what restored it, see below.
* **Rotation swaps the planes.** At `rotation = 90°` ring A's plane is ring B's at 0° and vice
  versa. Across 0 / 17 / 45 / 90 / 133 / 180° every invariant holds — watertight, both planes
  contain `n`, planes perpendicular, opposite sides, zero intersection — each ring's normal is
  parallel to the direction the rotation predicts to within `2e-3`, and `flexi_outer_extent()` is
  unchanged, so the "fits inside the cross-section" check does not move as the slider does.
* **The revolved kinds ignore it**: Double ring's and Ball & socket's body volumes are identical at
  0° and 90°.
* **A rotated joint still cuts.** At 55° the cut gives one object, two watertight parts, zero
  intersection, minimum clearance 0.35 ± 0.05, the faces exactly `gap` apart, and it is still
  non-separable under a straight pull.
* **3MF round trip.** A project saved with the joint placed but *not yet cut*, through
  `store_bbs_3mf` / `load_bbs_3mf`, comes back with the whole `FlexiJointParams` intact
  (`operator==` on the struct, so a field added later without a 3MF attribute fails here) and still
  **unprocessed**, so the cut still sees a joint to apply. The rings rebuilt from the loaded
  parameters have the same planes as the ones that went in.
* **Old files.** `FlexiJointParams`'s default `rotation` is 0, the reader gates the whole flexi
  block on `flexi_kind` being present, and a chain link at `rotation = 0` is asserted to be the
  reference orientation — ring A in the x–z plane, ring B in the y–z plane.
* **Both hinge axes.** The swing sweep now runs about `+X` *and* `+Y` (phase 2 could only do `+Y`),
  ±4° each way, intersection-free throughout.

Two phase 2 cases had to be corrected rather than merely re-run:

* the non-separable pull test's `free_travel` bound is derived from the ring offset now, not from
  the old `vcz` formula;
* the swing test's title and axes: "swings about the horizontal loop's axis" became "swings about
  both in-plane axes", which is the property the corrected geometry actually has.

**Two bugs the proof bar caught on the way**, both fixed and both now covered:

* **The tilt axis.** Tipping the upper ring about `+Y` swung its free tip along x, straight towards
  the lower ring's plane, and ate the clearance: the two wires' closest approach fell to 0.32 mm
  against a 0.35 mm clearance at only 7° of tilt. Tipping about the ring's own plane normal instead
  keeps every point in its plane, so the lateral margin the `link_width ≥ 4·wire + 2·C` rule buys is
  untouched at any tilt (0.75 mm at the defaults).
* **`CutConnectorType::FlexiJoint` was unreachable through 3MF.** It is enumerated *after* `Undef`,
  and the reader's range check was `type > int(Undef)` — written when no flexi connector could ever
  reach a file. Since a flexi joint now does, the bound is the last enumerator. The writer had two
  matching gates (`ModelObject::is_cut()` and `ModelVolume::is_cut_connector()`, the latter of which
  demands `is_processed`) that both dropped an uncut joint; both now admit one.

Checked by hand from the exported demo STLs (§8), independently of the Catch2 fit: for `rotation`
0 / 45 / 90 the two ring normals come out `(0,1,0)`/`(1,0,0)`, `(-0.707,0.707,0)`/`(0.707,0.707,0)`
and `(1,0,0)`/`(0,1,0)`, every one with `|n · z| = 0.0000`, centroids at `z = -1.750` and `+1.750`.

**Not proven in phase 3:**

* **The gizmo panel itself.** `render_flexi_rotation_input()` needs a GL canvas and wxWidgets, so
  nothing automated exercises the slider, the "only for Chain link" gating, or that the preview
  body visibly turns. That is the owner's click test.
* **Print behaviour.** §1.10's claim that a standing ring's top arc bridges cleanly is reasoning
  about the geometry plus the phase 2 experience with the one standing ring, not a sliced or
  printed result. Nothing checks the arcs against the actual layer height or overhang settings.
* **Rotation on a non-horizontal cut.** Every test cuts a horizontal plane. The rotation is defined
  in the cut frame and should follow a tilted plane by construction, but no test tilts one.
* **`rotation` is not auto-chosen.** Nothing aligns the rings with the cross-section's major axis
  or with the print direction; the value is whatever the user sets.

### Phase 2

`libslic3r_tests` as a whole: **706 cases, 704 passed, 2 failed as expected** (the two pre-existing
expected failures; phase 1's baseline was 676 + the same 2). `[FlexiJoint]` alone: **16 cases, 204
assertions, all green** — 8 from phase 1, 8 new.

New in phase 2:

* `its_make_swept_loop` is watertight, matches the torus volume `2π²Rr²` to 2%, closes on a
  non-planar path, and refuses degenerate input.
* The two chain-link loops are watertight, positive-volume, **do not intersect** and clear each
  other by ≥ `C`; the sizing rule `4·wire + 2·C` is enforced and an undersized loop is refused.
* Cutting a 20 mm cylinder with a Chain link gives **one object, two watertight parts**, zero
  intersection, **minimum clearance 0.35 mm ± 0.03**, and the two flat faces **exactly `gap`
  apart**.
* **Non-separable**: pulling one part along the cut normal by `gap + clearance` — and on out to the
  3.0 mm bound where the loops could first begin to unthread — always produces an intersection, as
  does a sideways pull. See §1.6 for why the sweep is bounded.
* **Swing sweep**: rotating one segment about the horizontal loop's axis (+Y) by ±4°, which is
  inside the `asin(gap / 2R)` the flat faces allow on a 20 mm cylinder, stays intersection-free.
* **Gap**: the kind defaults are 1.5 / 0.6 / 0.6, an unset gap falls back to the kind default, the
  floor is the clearance and a gap below it is refused, and a hand-set 1.6 mm gap really does put
  the faces 1.6 mm apart in the cut result while the joint still bridges it.
* **Chain-link auto sizing** keeps the assembly inside the cross-section at inscribed radii of 5,
  10 and 20 mm, and keeps the loops big enough to thread each other at every scale.
* **The connector regression** (§9): a Flexi cut, then a plain Plug cut on a new object yielding
  two objects, then a Flexi cut again.

Phase 1's cases still pass with the gap in place; two of them were updated for it — the face-to-face
distance is now the gap rather than the clearance, and the rock allowance is `asin(gap / R)`.

### Phase 1

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

Proven by the CLI in phase 2, same printer (**Bambu Lab P1S 0.4 nozzle / 0.20mm Standard @BBL X1C
/ Bambu PLA Basic**), supports off, isolated `--datadir` copied from `dd_ctl`. Gate:
`snorca_hubtest/gate_flexip2.sh`, `GATE_RC=0`.

* **All three jointed cylinders slice**, `exit=0`, `enable_support = 0` in every header:
  Chain link 487,150 bytes / 7,403 extrusion moves, Double ring 421,653 / 6,696, Ball & socket
  525,303 / 8,795.
* **The Gap survives slicing, for every kind.** Measured from the G-code: over the layers that lie
  strictly inside the gap, the extruded radius never reaches the wall.

  | kind | gap | faces at Z | max extruded radius in the gap band | wall |
  |---|---|---|---|---|
  | Chain link | 1.50 | 9.25 / 10.75 | **4.81 mm** (the two loops) | 9.80 mm |
  | Double ring | 0.60 | 9.70 / 10.30 | **3.24 mm** (the ring lip at its 3.0 mm mean radius) | 9.79 mm |
  | Ball & socket | 0.60 | 9.70 / 10.30 | **2.23 mm** (the ball) | 9.78 mm |

  Nothing at all is laid across the rest of the 20 mm face in those layers; the full disc returns
  on the far side. For the chain link that is six consecutive layers carrying 3–7 moves each,
  against 50–100 in the solid layers on either side.
* **The gap-closing guard still bites.** Same ring model, only `slice_closing_radius` changed from
  the shipped 0.049 to 0.25 (past the guard's `C/2 = 0.175` threshold): the moves inside the gap
  band collapse from **8 to 2** and the filament goes from **3.41 g to 3.46 g** — the groove's own
  0.35 mm clearance has been welded shut, exactly as BambuStudio #182 describes and as phase 1
  measured. What is new is that the **face gap of 0.6 mm survives it**: the segments still separate,
  the joint is merely stiff. That robustness is a side effect of the Gap parameter and is the
  reason a gap wider than the clearance is worth having even when the clearance alone would do.
* **No effect on unrelated models.** `OrcaCube_v2.3mf` sliced on the same preset by this branch and
  by a head-of-fork baseline install: **2 differing lines in 2,141,791 bytes, both the timestamp
  comment**. Every extrusion move is byte-identical.
* **A hidden scratch instance starts.** The scratch install with a data dir copied from `dd_lan`:
  alive after 45 s, `Responding = True`, no window, and it shuts down on `CloseMainWindow()`. The
  gate stops only processes whose `Path` starts with its own scratch install prefix.

Proven by the CLI in phase 1, on **Bambu Lab P1S 0.4 nozzle / 0.20mm Standard @BBL X1C / Bambu
PLA Basic**, supports off (`enable_support = 0` in every G-code header):

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

**Nobody clicked the gizmo**, in either phase — see §8. The UI code compiles, the app builds and a
hidden instance starts, but no human or agent has opened the Cut gizmo, picked Flexi, chosen a
joint kind, dragged it or pressed Apply. In particular **the connector-bug fix is verified at the
model level only** (§9): that the button is no longer greyed out is §8 step 10, for the owner.

Also unverified: nothing has been printed. The clearance defaults come from the research's survey of
maker sources, not from a print on this owner's machine.

## 8. The owner's click test

**Nobody has clicked the gizmo** — not in phase 1, not in phase 2. The UI code compiles, the app
builds and a hidden instance starts, but no human or agent has opened the Cut gizmo, picked Flexi,
chosen a joint kind, dragged it or pressed Apply. Everything below is for the owner to do.

**Chain link first — it is the one phase 2 is about.**

1. Load `resources/handy_models/` → any 20 mm calibration cylinder, or drop in a 20 mm ⌀ × 20 mm
   cylinder. Printer preset **Bambu Lab P1S 0.4 nozzle**, 0.2 mm layer, PLA.
2. Open the **Cut** gizmo, leave the plane at mid height (Z = 10 mm).
3. Click **Add connectors**.
4. In **Type**, pick **Flexi**. In **Joint**, pick **Chain link**. Leave **Auto size from the cut
   cross-section** ticked. At the defaults the panel should read: Link length 7.00, Link width
   5.50, Wire thickness 1.00, Stem depth 1.50, Loop tilt 7.00, Clearance 0.35, **Gap 1.50**.
5. Click on the cut plane at its centre to place the joint. The preview is the two interlocked
   loops at their clearance-inflated size; it turns red if you drag it past the edge of the
   cross-section.
6. **Confirm connectors**, then **Perform cut**.
7. Expected: **one object** with **two parts** — not two objects. "Cut to parts" shows ticked and
   greyed out.
8. Slice with **supports off**, print it.
9. **Off the bed the two halves should hinge and swivel like two links of a chain**, and must not
   come apart. No knife, no picking. The gap between the two flat faces should be visibly open
   (1.5 mm) — that is the point of the Gap parameter, and the thing that was missing in phase 1.
10. **Then the bug check, which is the other half of phase 2.** With the cut object still on the
    plate, select it (or any other object) and open the **Cut** gizmo again. **"Add connectors"
    must be clickable**, and picking a Plug must work exactly as it did before any Flexi cut. In
    phase 1 this button was greyed out from here on and only came back after restarting the
    slicer.
11. Repeat 2–9 with **Joint = Double ring** (Outer radius 4.00, Ring width 2.00, Ring height 1.50,
    Clearance 0.35, Tilt allowance 0.20, Hub radius 0.00, **Gap 0.60**). It should rotate freely
    about the joint axis and rock a degree or two, and not pull apart. Note the gap is now 0.6 mm
    rather than phase 1's 0.35 — tell me if that reads as too much or too little on the print.
12. Repeat with **Joint = Ball & socket** (Ball radius 4.00, Opening angle 40°, Clearance 0.35,
    Gap 0.60). It should swivel in every direction up to roughly the opening angle and stay
    captive.

If a joint prints fused: raise the clearance to 0.40–0.45 mm and check that
Quality → *Slice gap closing radius* is at or below 0.05 mm. If a joint is too floppy or too
stiff, the **Gap** is the knob for it — that is what it is for.

## 9. The connector bug from phase 1, and its fix (phase 2)

**The report.** *"It breaks the 'connector' option after being used once. Future cuts have a
greyed-out connector option; the entire slicer has to be restarted to restore it."*

**The cause.** `GLGizmoCut3D::m_keep_as_parts` is a plain `bool` member of the gizmo, and the Cut
gizmo is a **singleton owned by the 3D canvas** — it is not rebuilt per cut, per object or per
project. The phase 1 flexi branch of `render_cut_plane_input_window()` wrote that member directly:

```cpp
if (flexi_placed) {
    m_keep_as_parts = true;                 // <- the bug
    m_imgui->text(_L("A Flexi joint always keeps both halves ..."));
}
if (m_keep_as_parts) {
    m_keep_upper = m_keep_lower = true;     // <- and these
    ...
}
```

and the button that opens connector editing is disabled by

```cpp
m_imgui->disabled_begin(!m_keep_upper || !m_keep_lower || m_keep_as_parts || ...);
    if (m_imgui->button(has_connectors ? _L("Edit connectors") : _L("Add connectors")))
```

Nothing anywhere clears `m_keep_as_parts`: not `on_set_state()`, not `update_bb()`, not
`reset_cut_plane()`, not `reset_connectors()`. So the first Flexi cut latched it true and
**every later cut in that session opened with Add connectors greyed out**, on any object, until
the process was restarted. The same latch also disabled the Keep / Place on cut / Flip
checkboxes, which is the rest of what the owner saw.

It was never a `CutConnectorType` left at `FlexiJoint`, an app-config key, or the connectors-mode
enable condition rejecting the last-used type — those were the other candidates, and all three are
innocent: the type is re-picked from the radio buttons each time, nothing flexi is persisted to
`app_config`, and `CutConnectorMode` is dead code in this fork.

**The fix, in two halves.**

1. *Stop writing the user's setting.* The flexi branch now computes a **local** forced value for
   display and leaves `m_keep_as_parts` alone:

   ```cpp
   bool shown_keep_as_parts = flexi_placed ? true : m_keep_as_parts;
   m_imgui->disabled_begin(flexi_placed);
   if (m_imgui->bbl_checkbox(_L("Cut to parts"), shown_keep_as_parts) && !flexi_placed)
       m_keep_as_parts = shown_keep_as_parts;
   m_imgui->disabled_end();
   ```

   The force still reaches the cut, because `perform_cut()` has always applied it independently
   (`only_if(has_flexi ? true : ..., ModelObjectCutAttribute::KeepAsParts)`) and
   `Cut::perform_with_flexi_joints()` ignores the attribute bits entirely.

2. *Clear the rest on the way in.* `perform_cut()` records `m_flexi_forced_after_cut` when the cut
   used a flexi connector, and `on_set_state()` puts the after-cut flags (`m_keep_as_parts`, both
   Keep flags, Place on cut, Flip) and the connector type back to their defaults the next time the
   gizmo opens — once per cut, which is the natural place for it.

**The regression test** is `A flexi cut does not disable connectors on the next cut`
(`tests/libslic3r/test_flexi_joint.cpp`). The gizmo needs a GL canvas and wxWidgets and cannot be
instantiated in `libslic3r_tests`, so the test pins the model-level invariant the fix has to
preserve: a Flexi cut (one object, two parts), then a **plain Plug connector cut on a new object**
which must take the ordinary path and yield **two objects**, then a Flexi cut again. Before the
fix the middle step was unreachable through the UI at all. The parts of the fix that live purely
in ImGui state — that the button is no longer greyed — are covered by the owner's click test
(§8 step 10), not by an automated check.

## 10. Phase 4 — what is still open

Chain-link specific:

* ~~**The hinge axis is fixed at +X.**~~ **Done in phase 3.** The rings' orientation is now
  the `rotation` field (§1.9), exposed as a slider in the panel and persisted in the 3MF. What is
  still missing is the *auto*-choice: nothing aligns the rings with the cross-section's major axis
  or with the print direction, and there is no draggable handle on the plane — only the numeric
  slider.
* **The rest position is the extended one.** The two rings hang concentric-ish about the cut plane,
  which is where a chain sits and is what makes the interlock roomiest, but it also means the joint
  has a few tenths of a millimetre of free slop before it takes up. A "preload" that starts the
  rings part-way through their travel would feel tighter in the hand.
* **The swing range is not derived, only measured.** The Catch2 sweep checks ±4° about +X and +Y
  because that is inside `asin(gap / 2R)` for a 20 mm cylinder; the actual range of a chain link is
  bounded by the rings, not by the faces, and nothing computes it or shows it to the user.
* **Printability is reasoned about, not measured** (§1.10): both rings now stand vertical and their
  top arcs are assumed to bridge, but nothing checks that against the layer height or the
  overhang-angle settings, and nothing warns when a steeply tilted cut plane turns a ring's arc
  into a real overhang.
* **Two loops, not N.** A real chain has many links; one interlocked pair per cut is all this is.

General:

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
