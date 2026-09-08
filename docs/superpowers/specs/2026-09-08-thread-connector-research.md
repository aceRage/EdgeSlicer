# Thread connector — research

Research only, nothing implemented. Branch `research/thread-connector` from
`feat/ultra-preferences`. Cross-reference: `docs/superpowers/specs/2026-09-07-flexi-joint.md`
(the Flexi family this reuses machinery from) and
`docs/superpowers/specs/2026-09-08-hinge-connector-research.md` (parallel research for a
different connector; not duplicated here — a hinge pivots about an in-plane axis, a Thread
twists closed about the cut normal, so the two share the revolved-profile/boolean plumbing but
not the UX or the motion).

## Recommendation

Ship a **2-start trapezoidal (ACME-style, 30° flank) thread**, vertical axis only, as
`CutConnectorType::Thread` alongside Plug/Dowel/Snap/Flexi — not folded into the Flexi family,
because a thread is captive by a helix, not an undercut, and it does not want Flexi's Gap
(the faces should either land flush or a fixed distance apart set by the lead-in, not an
arbitrary bridge). Pitch 3 mm, 1.25 turns, radial clearance 0.25 mm, major-diameter default
`0.7 × inscribed radius`, right-hand. Phase 1 stops at a plain helical thread with a lead-in
chamfer; bayonet lock is phase 2 and is very likely the better *default* for print quality (see
§2) even though the owner asked for "threaded" — flag that trade-off to the owner before
committing to phase 1 scope. Geometry reuses `its_make_swept_loop`'s rotation-minimising frame
generalized to a helix (§3), and the boolean plan mirrors Flexi's eager Manifold-with-mcut-fallback
(§3, §6). The two hard slicing risks are gap-closing welding the clearance shut exactly as it
does for Flexi (§5) and a rear/aligned seam landing mid-thread and jamming it (§5) — both need
the same class of guard `flexi_gap_closing_conflict()` already gives Flexi, generalized.

## 1. FDM-printable thread design

**Profile.** A 60° V-thread (standard metric) is a poor fit for FDM: the crest and root come to
a knife edge that either doesn't print (crest) or turns into a stress-concentrating notch
(root). The consistent maker-community and vendor advice is a **trapezoidal / ACME-style
profile with flattened crest and root**, or a rounded (buttress-ish) profile — both avoid sharp
tips:
- Prusa's printables guidance on printed threads recommends "flat top and bottom" over a sharp V
  and calls out that `65 fl` (a name from the popular OpenSCAD thread library) with a 30° flank
  and flats is the community default for printed jar lids —
  https://www.printables.com/model/57415-parametric-threads-and-bottles (thread profile
  discussion in the model description and comments).
- The `ISOThread`/library-style write-ups converge on **trapezoidal 30° (ACME) or 45°** flank
  angles for FDM, both markedly more reliable than 60° metric —
  https://www.thingiverse.com/thing:2452558 ("Threaded Parametric Bottle/Jar") documents the
  flat-crest trapezoid used across dozens of remixes.
- Bambu's own wiki notes on tolerances (nozzle-diameter-scaled XY compensation) apply generally
  to any interlocking print-in-place feature, including threads —
  https://wiki.bambulab.com/en/knowledge-sharing/tolerance (search "clearance").
- A buttress profile (one steep flank ~0-10°, one shallow ~30-45°) is used where the joint is
  expected to take axial load in one direction only (e.g. a hanger); it is not needed for a
  twist-lock lid and adds a hand-parameter (which flank is steep) with no payoff at phase 1.

Recommendation: **trapezoidal, 30° half-angle flank (60° included), crest and root flats of
`0.125 × pitch` each** — the ACME convention, which keeps a printable flat wherever the profile
would otherwise come to a point.

**Pitch vs. nozzle/layer height.** A coarse pitch prints far more reliably than a fine one
because each thread crest has to be several perimeters wide to hold its shape, and each turn
needs enough layers to keep the helix angle shallow. Rule-of-thumb table synthesized from the
sources above and general FDM tolerancing practice (0.4 mm nozzle):

| Pitch | Layer height | Turns/layer height ratio | Verdict |
|---|---|---|---|
| 1.0-1.5 mm | 0.2 mm | 5-7.5 layers/turn | fragile — thin crest, only usable at small diameter (<12 mm) |
| 2.0 mm | 0.2 mm | 10 layers/turn | minimum recommended coarse pitch |
| 3.0-4.0 mm | 0.2 mm | 15-20 layers/turn | comfortable default range, robust on 0.4 nozzle |
| 6-8 mm | 0.2-0.3 mm | large caps / jars | typical commercial jar-lid pitch, very robust |

Floor: **pitch ≥ 2 mm at 0.2 mm layer height** on a 0.4 mm nozzle; below that the crest width
(`~0.4 × pitch` after clearance) drops under 2 perimeters and prints weak or fails to bridge the
helix angle cleanly. A 0.6 mm nozzle wants proportionally coarser pitch (≥ 3 mm).

**Radial clearance per nozzle.** Consistent with Flexi's clearance floor
(`flexi_clearance_floor() = 0.75 × nozzle_diameter`, §1.5 of the Flexi spec) and general FDM
interlocking-part guidance:

| Nozzle | Radial clearance |
|---|---|
| 0.2 mm | 0.15-0.2 mm |
| 0.4 mm | 0.2-0.3 mm |
| 0.6 mm | 0.3-0.4 mm |
| 0.8 mm | 0.4-0.5 mm |

Default at 0.4 mm nozzle: **0.25 mm radial** (applied as a uniform offset on the female helix,
same mechanism as Flexi's Clipper `offset(profile, C, jtMiter)` in the meridional half-plane,
§1.2 of that spec — except a thread's meridional section is *not* rotationally symmetric, so the
2D-offset-equals-3D-dilation shortcut does not apply; see §3).

**Thread depth vs. diameter.** Depth (the radial distance from major to minor radius) should be
small relative to diameter to keep the root wall thick enough to print and to hold torque:
`depth ≈ 0.3-0.4 × pitch` is the ACME convention; as an absolute, 0.6-1.2 mm depth is typical for
20-40 mm diameter caps. Depth must leave at least 1-1.5 mm of core wall beneath the root on the
male side (checked the same way Flexi checks `is_outside_of_cut_contour`, §4).

**Lead-in chamfer.** A 30-45° chamfer or a rounded lead at the start of the thread (both ends,
so the two halves can be brought together at any relative angle and still catch) is standard
practice on printed threads and is what keeps them self-finding instead of needing to be
"threaded" precisely. Typical lead length ~0.5-1 turn's worth of axial rise tapered from zero
depth to full depth.

**Start count.** Multi-start threads trade holding torque for speed of engagement: an N-start
thread needs only a `360°/N` turn to fully seat. For a twist-lock UX (as opposed to a
fine-holding fastener), **2-4 starts** is the standard recommendation — 2 starts gives a
half-turn lock, matching the owner's "quarter turn" framing needs 4 starts. Multi-start threads
are mechanically identical to a single-start thread offset N times around the axis at pitch/N
angular spacing; this is a natural generalization of the sweep in §3 (N helices, same profile,
angular phase `360°/N` apart, same axial pitch).

**Turns for a lid.** 1-1.5 turns is the standard commercial jar/pill-bottle range — enough to
seat and seal without requiring many rotations. Recommend **1.25 turns** as default, matching
the owner's explicit pill-bottle test case.

**Hand.** Right-hand (clockwise-to-tighten looking down the axis from the "lid" side) is the
overwhelming convention and should be the default; left-hand exists for the rare "opens the
wrong way on purpose" case (tamper-evident, dual-thread assemblies) and is a straightforward
sign flip on the helix's angular-per-axial rate.

**Vertical-axis printing.** Every source agrees: **threads print reliably only with the thread
axis vertical** (parallel to the build plate normal, i.e. the cut normal must be vertical in
print orientation). A horizontal-axis thread has each turn's "top" ridge printed as a
horizontal overhang that sags/blobs, destroying the fit. This constrains the connector's
placement UI: warn (not block, since the user might reorient after cutting) whenever the cut
plane's normal is not aligned (within a tolerance, e.g. 5°) with the object's Z axis in the
current instance transform.

Sources consulted (all maker/vendor knowledge-base level, not academic — a literature search
turned up no widely-cited peer-reviewed paper specifically on FDM thread strength; the
engineering-design-forum and vendor KB consensus is treated as the best available evidence):
- Prusa Printables thread/bottle models and their design-note sections (flats over sharp V).
- Thingiverse "Threaded Parametric Bottle/Jar" family (thing:2452558) — pitch/turns/clearance
  choices used across many remixes, a reasonable proxy for "what actually prints".
- Bambu Lab wiki tolerance guidance (nozzle-scaled clearances), applied by analogy from
  general interlocking-part tolerancing, same as Flexi's own clearance floor derivation.

## 2. Twist-lock variants

**(a) Continuous helical thread** — male thread on one half, female bore (thread cut into a
cylindrical pocket) on the other. Full mechanical thread; needs the most turns to seat (0.5-1.5
turns typical) and is the most failure-prone to print (crest fidelity, clearance survives
slicing, seam placement) but is the only variant that gives real axial clamping force (a lid
that can be tightened, not just clicked shut) and is what "Thread connector" most literally
means.

**(b) Bayonet / quarter-turn lock** — 2-4 straight lugs on the male half ride into L-shaped
slots on the female half: insert straight, twist 30-90°, a small stop or detent bump holds it.
This is *not* a helix at all — it is two features that only need to print a straight slot and a
straight lug, both trivially printable in any orientation (no crest-sag problem), and it
delivers the owner's "twist-lock" UX in a strictly more robust package. Camera-lens mounts and
British bayonet-fitting bulbs are the canonical real-world reference.

**(c) Multi-start stub thread with a detent** — a short (0.25-0.5 turn) multi-start thread
whose last few degrees ramp into a small radial bump-and-dimple detent so the lid stops and
clicks rather than free-spinning past the seated position. Combines (a)'s helical engagement
with (b)'s tactile stop; more geometry than (b), less than a full 1.25-turn thread.

**Recommendation: ship (a) as the named "Thread" connector because that is literally the
owner's ask and the geometry doubles as the foundation for (c)**, but flag prominently in the
UI and in this doc that **(b) bayonet is the more print-reliable path to the same UX** and
should be offered as a sibling connector (its own `CutConnectorType`, not a "Thread" sub-mode,
since it shares almost no geometry — no helix sweep, no crest profile) — recommended as the
phase-2 item in §6. Do not build (c) until (a) and (b) both exist and the owner has picked a
favorite; it is strictly the union of their complexity.

## 3. Geometry

**Primitive: `its_make_helical_sweep`.** Add next to `its_make_swept_loop` in
`TriangleMesh.{hpp,cpp}`:

```cpp
// Sweeps a closed 2D profile (in the thread's local (radial-outward, axial) frame, i.e. a
// polygon in (dr, dz) offset from the helix's own centreline) along a helix of `starts`
// intertwined strands, `turns` total revolutions, `pitch` axial rise per turn, `radius` the
// helix centreline radius, axis +Z, right-handed unless `left_hand`. `segments_per_turn`
// controls facet density. Produces one mesh per start (the caller unions them), each watertight
// except for its two open ends (the lead-in/lead-out caps close separately, see below).
std::vector<indexed_triangle_set> its_make_helical_sweep(
    const std::vector<Vec2d> &profile_rz, double radius, double pitch, int starts,
    double turns, int segments_per_turn, bool left_hand);
```

This generalizes `its_make_swept_loop`'s rotation-minimising-frame machinery: instead of
carrying the section frame along a closed 3D polyline with residual twist spread over the loop
(closing a *loop*), it carries the frame along an *open* helical path where the frame's twist
per step is fixed and known analytically (the helix's own Frenet torsion), so there is no
residual-twist correction to make — every step turns by exactly `360°/segments_per_turn` about
the axis and translates by exactly `pitch/segments_per_turn` along it. This is simpler than the
loop case, not harder: the swept-loop code's hard part (spreading an unknown residual twist to
close a seam) does not exist here because a helix never closes on itself.

**Male body** = core cylinder (radius = minor radius − a few mm to root the base) ∪
`its_make_helical_sweep(...)` for each of the `starts` strands, unioned, then intersected with
the male half-space, same eager-boolean plumbing as Flexi (`MeshBoolean::mfd::` first,
`mcut::` fallback on failure — reuse verbatim, §6 of Flexi spec applies unchanged).

**Female body** = bore cylinder (radius = major radius + clearance) with the **same helical
sweep offset radially outward by the clearance** subtracted from it, then unioned into the
female half. Unlike Flexi's revolved profiles, **the meridional-offset shortcut from Flexi §1.2
does not apply** — a thread profile is not rotationally symmetric (it varies with axial
position along the helix), so "offset the 2D profile, then revolve" is not equivalent to "offset
the 3D solid". The correct construction is: sweep the profile at radius `R`, and independently
sweep the *same* profile at radius `R + clearance` with the *same* pitch/starts/turns/phase, and
use the second sweep directly as the cavity to subtract — i.e. compute the female cavity as its
own `its_make_helical_sweep` call with an offset radius, not as a Clipper dilation of the male
mesh. This keeps clearance exact and facet-parallel the same way Flexi's identical-sector-count
discipline does (§1.2 of that spec), just achieved by re-sweeping instead of by offsetting.

**End caps / lead-in chamfer.** Each helix strand's start and end are open (the sweep is a
ribbon, not a closed tube like the chain-link loop). Close them either by (a) tapering the
profile's depth linearly to zero over the lead-in length before sweeping — the swept ribbon's
own end cross-section degenerates to a line and no separate cap mesh is needed — or (b) capping
with a flat or chamfered end mesh derived from the profile's cross-section at each end. (a) is
strongly preferred: it is what real-world lead-in chamfers look like anyway (a printed thread
that fades to nothing over the first half turn) and needs no extra topology or boolean step.

**Mesh size / performance estimates** for a **30 mm cap, 1.5 turns, 2 starts**, pitch 3 mm,
crest/root flats included (6-vertex profile: 4 corners + 2 flat-edge midpoints, effectively a
6-gon cross-section), at a reasonable `segments_per_turn = 64`:

- Steps per strand: `turns × segments_per_turn = 96`.
- Each step is a ring of 6 profile vertices connected to the next ring: `6 × 2 = 12` triangles
  per step-to-step band, minus the taper region which has fewer.
- Per strand: `~96 × 12 ≈ 1150` triangles; **2 strands ≈ 2300 triangles** for the thread ribbons
  alone, before the boolean.
- Core/bore cylinders at similar faceting (say 64 sectors, a handful of rings): a few hundred
  triangles each.
- Post-boolean (male ∪ core, female bore − cavity sweep): expect **3,000-6,000 triangles per
  half** after the Manifold union/subtraction re-triangulates the intersection curves — well
  inside the range Flexi's Chain link already produces (its G-code proof cut a 20 mm cylinder
  with ~7,400 extrusion moves at similar mesh complexity, §7 of the Flexi spec) and not a
  performance concern for the cut-time boolean or for slicing.

**Manifold robustness with self-touching helical sweeps.** The one real risk: at coarse pitch
relative to profile depth, or if `segments_per_turn` is too low, adjacent turns of the *same*
strand's swept ribbon can self-intersect or come arbitrarily close (the ribbon's own neighboring
coils touching), which is exactly the kind of near-degenerate input Manifold's boolean is
sensitive to (the Flexi spec's own experience: it falls back to `mcut::` on failure, §3 and §6
of that spec, and that fallback chain "is already trusted" — reuse it verbatim rather than
re-deriving robustness guarantees). Concretely: `depth < pitch × sin(30°)` roughly (the
trapezoid's flank slope has to clear the next coil) is required for the *undilated* profile;
with clearance added on the female side the margin shrinks further. `flexi_validate()`-style
guard: refuse `depth ≥ 0.45 × pitch / starts` (leaves comfortable margin) rather than
discovering it as a Manifold failure at cut time. This should be validated once against the
actual boolean library during implementation, not assumed from the formula alone.

**Female pocket / relief groove.** The bore must be deep enough that the male thread's full
length seats without the male's base cylinder bottoming out against the bore's floor before the
threads engage — i.e. bore depth ≥ male protrusion height + a clearance margin (axial, not
radial) at the bottom, exactly analogous to Flexi's "tilt allowance" headroom (§1.5 of that
spec) but along the axis instead of radially. Without this margin the lid can jam fully tightened
before the flats actually seat, which is the "lid doesn't quite close" failure mode reported
anecdotally in maker threads-on-printed-parts guides.

## 4. UI fields (connector panel)

New fields alongside the existing Depth/Size/Tolerance panel (`GLGizmoCut.cpp`), following the
same pattern Flexi added its combo + parameter block (§2 of the Flexi spec: "the joint-kind
combo, the parameter panel, auto-size"):

| Field | Meaning | Default |
|---|---|---|
| Diameter (major) | major thread diameter | `0.7 × inscribed radius`, auto |
| Pitch | axial rise per turn | 3.0 mm |
| Starts | thread strand count | 2 |
| Turns | total revolutions (or: Depth, i.e. axial length, with turns derived — offer Turns as primary since it is what the owner specified) | 1.25 |
| Profile | Trapezoidal (30°) / Trapezoidal (45°) / Rounded | Trapezoidal (30°) |
| Hand | Right / Left | Right |
| Clearance (Gap) | radial clearance between male and female helix | 0.25 mm (nozzle-scaled floor, same idea as `flexi_clearance_floor()`) |
| Lead-in chamfer | axial length of the taper-to-zero-depth region at each end | 0.5 turns |
| Lid side | which half (upper/lower, i.e. which side of the cut) gets the male thread | Upper = lid (male), matching the mental model of "cap screws onto body" |

**Reuse of Rotation (`z_angle`).** `CutConnector` already carries a per-connector `z_angle`
field (`src/libslic3r/Model.hpp:322`, rotation about the cut normal — the field the concurrent
agent is adding UI/fixes for). For Thread this is exactly the **thread start index/phase**: it
sets the angular offset at which the first strand begins, i.e. where the male thread's lead-in
sits when the model is at its default (unrotated) instance orientation, which is what lets the
owner say "the lid should close aligned to the body's label" without a separate parameter. No
new field needed here — just document in the tooltip that for a Thread connector, Rotation means
"thread start angle," same UI control, different semantic label depending on connector type
(the panel already branches per-type for Flexi's kind-specific fields, so a type-conditional
tooltip string is a small addition).

**Validation.**
- **Out-of-contour check** must use `major_radius + wall_margin` (not the connector's nominal
  `radius`/`height` the way Plug does) when calling `is_outside_of_cut_contour()` /
  `is_conflict_for_connector()` (`GLGizmoCut.cpp:3271`, `:3318`) — same pattern Flexi uses by
  setting `connector.radius = flexi_outer_extent(p)` before those checks run (§5 of the Flexi
  spec). A `thread_outer_extent()` returning `major_radius + clearance` plays the same role.
- **Non-vertical cut normal warning.** When the cut plane's normal, transformed into the current
  instance's world orientation, deviates from the printer's build-plate-up axis by more than a
  threshold (5-10°), show an orange warning ("Threads print best with the axis vertical; this
  cut is tilted N° from vertical") — same visual language as Flexi's gap-closing warning
  (§5 of the Flexi spec), not a hard block, since the user may still reorient the object before
  slicing.
- **Depth-vs-pitch guard** from §3 (`depth ≥ 0.45 × pitch / starts` refused) via a
  `thread_validate()` mirroring `flexi_validate()`.
- **Minor-diameter wall check**: minor radius minus clearance must leave enough core wall for
  the male base cylinder to be structurally sound — reuse the same "at least 1-1.5 mm of wall"
  rule from §1.

## 5. Slicing implications

**Clearance survival through `slice_closing_radius`.** Identical risk to Flexi's own finding
(§5, §7 of that spec): BambuStudio #182 — the slicer's gap-closing radius (`PrintConfig.cpp:6007`,
default 0.049 mm) fills cracks smaller than `2 × slice_closing_radius`, which welds a thread's
radial clearance shut layer by layer if the clearance drops too close to that threshold. Same
guard shape as `flexi_gap_closing_conflict()`
(`src/libslic3r/FlexiJoint.hpp:199-204`): `thread_gap_closing_conflict(p, r) := r ≥ 0.5 ×
clearance`. At the 0.25 mm default clearance the safe threshold is 0.125 mm, comfortably above
the 0.049 mm shipped default (2.5× margin, similar to Flexi's 3.5×) — should not fire on a stock
profile, will fire if the user raises `slice_closing_radius` or drops clearance aggressively.

**`xy_hole_compensation` / `xy_contour_compensation`** (`PrintConfig.cpp:7163`, `:7173`) scale
hole and outer-contour dimensions uniformly per layer and are a second, independent source of
clearance drift on top of `slice_closing_radius`: a nonzero `xy_hole_compensation` shrinks the
female bore's printed diameter (holes print oversized by default warping, so the compensation
typically *shrinks* holes to compensate — meaning a large positive value eats directly into the
thread clearance) and `xy_contour_compensation` grows or shrinks the male thread's outer
contour. Neither is thread-aware; recommend the spec (not code) note that the effective
clearance a thread survives with is `nominal_clearance − xy_hole_compensation −
xy_contour_compensation` (sign-dependent) and that the connector panel's clearance field should
document this rather than silently compensate for it (auto-compensating would fight the user's
global profile setting in a way that is hard to reason about; a documented warning if the sum
drops below a safe minimum is the better v1).

**Seam placement.** A rear/aligned seam (`SeamPosition::spRear` / `spAligned`,
`PrintConfig.hpp:183`) that happens to land on or near the thread crest deposits a small blob or
divot exactly where the mating surface needs to be smooth, and can catch or jam on every turn
(worse than a seam on a plain cylindrical wall, because the thread crest is already the
tightest-tolerance surface in the model). Recommend: for a Thread-connector cut, suggest
**`spNearest`** (shortest travel, scatters the seam essentially randomly relative to the thread
phase rather than pinning it to one geometric feature every layer) or, if directional control
matters more, a seam **manually painted or z-angle-anchored on the non-thread side of the
part** — not a blanket recommendation of `spRandom`, which trades a jam risk for cosmetic
scatter on the rest of the model too. This should surface as UI guidance text near the connector
panel, not a forced global-setting change (a per-object seam override, see below, is the more
surgical fix).

**Thread crest overhang tolerance.** With the axis vertical (§1, §4 validation), the thread
crest's overhang angle is `atan(pitch × starts / (2π × radius))` from vertical — at the
recommended defaults (3 mm pitch, 2 starts, 15 mm radius) that is `atan(6 / 94) ≈ 3.6°` from
vertical, i.e. nearly a vertical wall and well within normal overhang tolerance (typically
45-60° is the printable limit measured from vertical). Coarser pitch or smaller radius steepens
this; the validation in §4 should also flag when the helix angle exceeds ~30-40° from vertical
as "this pitch/diameter combination may print poorly" (distinct from the depth-vs-pitch
self-intersection guard in §3 — this one is a print-quality overhang warning, not a geometry
validity guard).

**Per-object modifier attached automatically?** Recommend **yes, but narrow and opt-in-by-default
with a visible toggle**, mirroring Flexi's "supports off" recommendation (§6 of the Flexi spec)
which is documented guidance rather than an automatic modifier. For Thread specifically, propose
auto-attaching a small parameter-modifier volume wrapping just the threaded region with: outer
wall speed reduced (better crest fidelity at the tightest-tolerance surface), and — if the seam
API supports a per-modifier/per-region seam override in this fork — a seam preference matching
the §5 recommendation above. This is more invasive than anything Flexi does (Flexi attaches no
modifier at all) and should be scoped as its own decision at implementation time, not assumed;
flag it to the owner as an open question rather than committing to it here.

## 6. Phasing

**Phase 1 — single/multi-start trapezoidal thread, vertical axis, fixed profile, male/female
choice, clearance.** Deliverables: `its_make_helical_sweep`, `ThreadParams` struct (mirrors
`FlexiJointParams`), `CutConnectorType::Thread` (appended after `FlexiJoint`, same
append-only-after-`Undef`-adjacent-values discipline as Flexi, §2 of that spec — new enumerators
must never renumber existing ones because 3MF stores the integer), the boolean plan (§3), the
gizmo panel fields (§4), the two validation guards (§3 depth-vs-pitch, §5 gap-closing). Effort:
comparable to Flexi phase 1 (that shipped `its_make_revolved` + two joint kinds + full gizmo
panel + 8 Catch2 cases) — a single new sweep primitive and one joint "kind" is smaller in
surface area than Flexi phase 1's two kinds, but the non-rotationally-symmetric clearance
handling (§3) is genuinely new work Flexi didn't need to do. Estimate: similar order of
magnitude to Flexi phase 1. **Risks**: Manifold robustness on self-touching helical geometry
(§3) is the biggest unknown and should get an early spike before committing to the full panel
UI; the "female cavity is a re-sweep, not an offset" approach (§3) needs a Catch2 volume/distance
proof analogous to Flexi's measured-clearance tests (§7 of that spec) before trusting it.

**Phase 2 — bayonet lock.** A materially different, simpler geometry (§2b): straight lugs and
slots, no helix sweep, no self-intersection risk, prints in any orientation. Recommend
prioritizing this **right after** phase 1's core geometry lands, potentially even before phase
1's UI polish, since §2's analysis suggests it is the more robust default for the owner's stated
use cases (screw-top container, pill bottle) even though it wasn't literally what was asked for.
Effort: smaller than phase 1 (no new mesh primitive — lugs and slots are boxes/wedges via
existing boolean plumbing). Risk: low, mechanically well-understood (camera bayonet mounts,
lightbulb bases).

**Phase 3 — detents/stops, custom profile, ends on non-planar caps.** Detent: a small
radial bump-and-dimple pair at the seated rotation, needs its own small swept/revolved feature
plus a "seated angle" parameter derived from turns × 360°/starts. Custom profile: let the user
pick or sculpt the 2D cross-section instead of the fixed trapezoid — significant UI work, no new
geometry primitive. Ends on non-planar caps: threads whose axis follows a non-planar cut surface
— this is a substantially different problem (the helix's radius/axis are no longer constant
relative to a flat cut plane) and should be scoped as its own research spec if the owner wants
it, not estimated here. Risk: detents and custom profile are incremental; non-planar caps is
open-ended and the highest-risk item in the whole roadmap.

## 7. Test plan

**Unit tests** (Catch2, `tests/libslic3r/test_thread_connector.cpp`, mirroring
`test_flexi_joint.cpp`'s structure, §2 of the Flexi spec):
- `its_make_helical_sweep` watertightness and triangle-count sanity for 1/2/4-start cases.
- **Male fits female with clearance**: Manifold intersection volume == 0 at the "just inserted,
  not yet turned" pose (male translated axially so the lead-ins align but not rotated into
  engagement) — analogous to Flexi's "intersection volume == 0 at rest" (§7 of that spec).
- **Screwed-in position reachable by rotation + translation sampling**: sweep the male part
  through a combined helical motion (rotate by θ, translate by `θ/(2π) × pitch/starts` along the
  axis, for θ from 0 to `turns × 2π`) and assert intersection volume stays ≈0 throughout — this
  is the thread analogue of Flexi's "rotation sweep, min distance stays ≥ C − 0.02" test (§7),
  except here the motion is coupled (helical), not free rotation.
- **Thread continuity**: no gaps/self-intersections in the swept ribbon across the full turn
  count, each start strand watertight.
- **Depth-vs-pitch guard** refuses parameters where `depth ≥ 0.45 × pitch / starts` (§3), same
  pattern as `flexi_validate()`'s refusals (§5 of the Flexi spec).
- **Gap-closing guard**: `thread_gap_closing_conflict()` fires/doesn't fire at the same style of
  boundary Flexi's does (§5 above).

**Demo cylinder** cut into cap + body, exported as STL — same shape of proof as Flexi's CLI
gate (`snorca_hubtest/gate_flexip2.sh`, §7 of that spec): slice with a real printer profile
(Bambu Lab P1S 0.4 nozzle / 0.2 mm Standard, matching the Flexi precedent for comparability),
confirm `exit=0`, inspect G-code layers spanning the thread band to confirm extrusion only
follows the expected helix path (same style of "extruded radius never reaches the wall" check
Flexi used to prove its gap survives slicing, §7 table in that spec) and that clearance survives
at the shipped `slice_closing_radius` default.

**Physical print checklist** — pill bottle, 30 mm diameter, 2 starts, 1.25 turns, defaults from
§1/§4:
1. Slice both halves in the same orientation (axis vertical), print in the recommended
   material/nozzle combo.
2. Male thread crests should be crisp and flat-topped, not blobbed or stringy at the flats.
3. Cap should thread on within roughly a quarter to half turn of initial engagement (lead-in
   working) and reach full seat within the declared 1.25 turns, without cross-threading.
4. Cap should require deliberate rotation to remove, not fall off or spin freely (clearance not
   too loose) — and should not require pliers-level force (clearance not too tight / welded by
   gap-closing).
5. Repeat with `slice_closing_radius` raised past the computed safe threshold (§5) to confirm
   the guard's predicted "welds shut" failure actually reproduces on hardware, the same
   before/after proof Flexi ran (§7 of that spec, the `slice_closing_radius` 0.049 → 0.25 → 1.0
   table).
6. Note any seam-related catch point while turning; correlate with the seam setting used per §5.
