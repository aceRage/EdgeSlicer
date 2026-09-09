# Sculpt brushes research: more brush types, and auto-masking the bed face

2026-09-08, research on branch `research/sculpt-brushes` off `feat/ultra-preferences` (`5bf84e1f1d`).
Read-only against `src/libslic3r/MeshSculpt.{hpp,cpp}`, `src/slic3r/GUI/Gizmos/GLGizmoSculpt.{hpp,cpp}`
and `docs/superpowers/specs/2026-09-07-sculpt-mode.md` (v1/v1.1/v2). No code changed. Answers the
owner's two questions on what phase-3 sculpting could look like: more brush types, and an
auto-protected bed-contact face.

## Recommendation, up front

**Phase 3a - four brushes, in this order: Pinch, Layer, Clay Strips, Snake Hook.** All four are
vertex-only displacement, all four reuse `weights[i] = strength * falloff_weight(d, r)` verbatim,
and none needs a new field on `SculptSession` - only new cases in the `apply()` switch and (for two
of them) one or two new `BrushParams` fields, the same shape as `fill_only`/`ridge` today. Magnify
rides in for free as Pinch's sign flip (like Deflate rides on Inflate's `deflate` bool) so it is not
counted as a fifth brush. Relax is a strong fifth candidate - it is Smooth's tangential twin and
almost as cheap - but is deferred to 3a+ only because "smooth without shrinking the silhouette" is a
harder story to test than the other four's closed-form displacements (see its row below).

**Do not attempt Rotate/Twist, Elastic (Kelvinlets), Mask-as-a-brush, or any remesh/Dyntopo brush in
phase 3.** Twist and Kelvinlets both need a rotation/shear applied to a whole neighbourhood rather
than a per-vertex scalar-times-vector displacement - they do not fit `apply()`'s current shape
without a new displacement-field abstraction (a "get displacement for vertex v" functor rather than
one formula evaluated the same way for every vertex). Mask-as-a-brush is exactly Question 2's
problem and is scoped there, as phase 3b. Local remesh brushes (Scrape done properly, Simplify/Dyntopo)
change topology and inherit the whole `clear_before_change_mesh()` / annotation story the spec's own
"v3" section already flags as a research project - nothing here changes that assessment.

**Phase 3b - auto-mask the bed-contact face**, built as a general per-vertex mask (a `std::vector<bool>`
or bitset alongside the existing per-vertex arrays) that every brush's weight computation ANDs against,
plus one non-brush pass that detects and pins the bed-contact vertices when the gizmo attaches to a
volume and after every commit. This is the harder of the two questions - not because pinning vertices
is hard, but because "keep the footprint polygon exactly the same shape" is a stronger guarantee than
"don't move these vertices," and the two are only equivalent when the bed face is a single flat
island with no brush work reaching its boundary from outside. See the risks section.

Phasing: **3a** (four brushes) and **3b** (mask + bed protection) are independent and can ship in
either order or in parallel branches; 3a touches only `MeshSculpt.{hpp,cpp}` + the gizmo's brush enum
and radio row, 3b touches `SculptSession` construction, `apply()`'s weight loop, and
`commit_sculpted_mesh()` / `its_subdivide_midpoint()`. Recommend 3a first - it is lower risk and
delivers visible value immediately - then 3b, since 3b's "Mask brush" toggle is much more useful once
there are more brushes worth protecting a region from.

## How a brush fits into this architecture (recap, for grounding the estimates below)

Every brush lives as one `case` in `SculptSession::apply(const BrushParams&, StrokeStep&)`
(`MeshSculpt.cpp:486-593`). The shared preamble already does the expensive part: it collects the
vertices strictly inside the brush sphere (`collect_vertices_in_radius`, backed by the AABB tree) and
computes a per-vertex scalar `weights[i] = strength * falloff_weight(d, r)` (or the hard-edged 0/1 form
when falloff is off). Every existing brush is then `vertices[v] += weights[i] * (some Vec3f)` - Grab
uses a caller-supplied displacement, Inflate uses `amount * vertex_normal`, Flatten/Crease use a fitted
plane (`fit_plane()`, area-weighted centroid + averaged vertex normal), Smooth reads the one-ring mean.
**Any brush whose per-vertex move is "weight times some vector computed from that vertex's neighbourhood
or a brush-wide fitted quantity" drops into this switch for free.** That is the dividing line this
report uses for "easy": does the brush's math fit `move = weight * f(v, neighbourhood, brush-wide fit)`,
evaluated independently per vertex, or does it need information that only exists at the whole-mesh or
whole-stroke level (rotation composition, volume-preserving global correction, topology change)?

Two structural facts constrain every proposal:

* **Vertex-only moves preserve the annotations; anything that changes `its.indices` does not.**
  `commit_sculpted_mesh()` refuses unless `indices_match()` holds. A brush that only moves vertices is
  therefore "free" on the paint story; a brush that needs new vertices or a different triangulation
  (any local remesh) is not, and inherits exactly the `clear_before_change_mesh()` path Subdivide
  already uses.
* **The falloff, Ctrl-invert and undo machinery are all brush-agnostic today** except for the one
  per-brush question `brush_inverts_with_ctrl()` answers. A new brush gets `weights[]`, one stroke =
  one undo step, and F/Shift+F adjustment for free; it only needs to opt in to Ctrl-invert if it has a
  sensible opposite (the same judgement call already made for Grab/Smooth: "no sensible opposite,
  Ctrl keeps its canvas meaning").

## Brush-by-brush evaluation

Each row: what it needs beyond the shared preamble, the one-line displacement, falloff/Ctrl/undo fit,
effort, printability notes.

### Clay / Clay Strips - build up toward an offset plane

**Needs:** `fit_plane()` (already have it), one new scalar field (`clay_offset`, mm, the target
standoff above the fitted plane - Blender's brush "Plane Offset").
**Displacement (one line):** `v += weight * max(0, (offset_plane_signed_distance(v) ... )) * normal` -
concretely, fit the plane as Flatten does, then push each vertex toward `origin + offset*normal` along
`normal` only if it is currently below that target, i.e. `v += weight * max(0, target_d - d) * normal`
where `d = (v - origin).dot(normal)` and `target_d = clay_offset`. This is Flatten with a target
*above* the surface instead of *on* it, and a one-sided (never-retract) move instead of Flatten's
two-sided pull - so implementation-wise it is closer to "Flatten with `fill_only` inverted and an
offset" than a new formula.
**Composes with:** falloff yes, Ctrl yes (Ctrl = negative offset, i.e. carve instead of build, which is
Blender's own Clay Strips Ctrl behavior), undo yes (vertex-only).
**Effort: S.** It is structurally Flatten with a nonzero, signed target distance and a one-sided
clamp; the plane-pin-for-the-stroke logic Flatten already has (fit direction once at `start_stroke()`,
refit offset every tick) carries over unchanged.
**Printability:** building material outward can create overhangs where none existed - the panel should
carry the same "you can create unsupported geometry" caution any inflate-like brush deserves, but
there is nothing here MeshSculpt itself needs to check (this tree does not overhang-check strokes
today; Inflate has the identical exposure).

### Draw Sharp

**Needs:** nothing new - it is Flatten/Clay's plane-fit machinery with a *sharper* falloff (Blender's
Draw Sharp is literally Clay Strips with a smaller, harder falloff curve and no averaging across the
whole brush footprint) rather than a distinct kernel.
**Displacement:** identical to Clay Strips; the "sharp" character comes from a falloff curve, which
this tree does not have yet (v2's backlog item is "falloff curves ... instead of the one fixed
quartic").
**Composes with:** everything Clay Strips does.
**Effort: S, but blocked on falloff curves being pluggable first** (that is on the existing v2
backlog, not something this report needs to re-litigate). Until then "Draw Sharp" is just "Clay Strips
with a smaller radius," which is not worth a separate brush entry. **Recommendation: fold into Clay
Strips, do not ship as a separate brush until falloff curves land.**

### Pinch - in-plane pull toward centre, no normal component

**Needs:** nothing new beyond what Crease already computes.
**Displacement:** `v += weight * (origin - v_tangential_component)`, i.e. exactly Crease's tangential
term (`rel - rel.dot(normal)*normal`, then move toward the centre line) **with the normal push term
removed**. In fact this is Crease with `crease_normal_ratio = 0`.
**Composes with:** falloff yes, Ctrl yes (Ctrl = push outward = Magnify, see next row), undo yes.
**Effort: S.** This is close enough to "Crease with the push disabled" that it could plausibly be a
`BrushParams` flag on Crease rather than a new `BrushType` - but the owner asked for it as a distinct,
discoverable brush in the radio row, and Blender ships it separately too, so treat it as its own
`BrushType::Pinch` sharing Crease's tangential code path (extract that one loop into a shared helper
used by both `case`s, avoiding duplicated math).
**Printability:** pinch pulls surface area toward a line, which is exactly how Crease already creates
thin, near-coincident geometry at high strength - same self-intersection/near-zero-thickness risk
Crease has today, nothing new.

### Magnify - inverse of Pinch

**Needs:** nothing - `brush_inverts_with_ctrl(BrushType::Pinch) == true`, and the existing Ctrl-invert
plumbing (`ctrl_down` threaded into `make_brush()`) flips the sign of the tangential move, exactly the
way Inflate/Deflate already share one brush with a sign flag.
**Displacement:** Pinch's formula, sign flipped (`v -= weight * tangential_offset` instead of `+=`).
**Composes with:** everything Pinch does; this *is* the Ctrl variant, not a new radio entry.
**Effort: XS** (it is Pinch's `deflate`-style bool, not new code).
**Printability:** pushing outward radially can thin a wall from the inside if the brush straddles two
sides of a thin feature - the same caution Inflate already carries, no new failure mode.

### Scrape / Fill as standalone brushes

**Needs:** `fit_plane()`, reused. This is exactly Flatten today, split into its two existing halves as
separate radio entries instead of one Ctrl pair: "Scrape" = symmetric or above-only removal (Flatten
without `fill_only`, or a one-sided "shave only what's above" variant - Blender's Scrape is one-sided
downward), "Fill" = `fill_only=true` (already implemented, currently reached only via Ctrl+Flatten).
**Displacement:** identical to Flatten's existing two branches.
**Composes with:** everything Flatten already does - this is a UI/discoverability change, not a math
change.
**Effort: XS-S.** The math exists; the only work is deciding whether to (a) leave Flatten/Ctrl=Fill as
is and call it done (already shipped, v2), or (b) additionally expose Fill as its own radio button for
users who never discover Ctrl. **Recommendation: skip as a phase-3 item** - v2 already ships this
functionality; a separate radio entry is a pure discoverability tweak that can piggyback on any future
panel pass rather than consuming a brush slot.

### Nudge - move along stroke direction

**Needs:** the stroke's current drag direction, which the gizmo already computes for Grab
(`GLGizmoSculpt` tracks the mouse delta each tick to build Grab's `displacement`). `BrushParams`
already carries `displacement`; Nudge would reuse that field with a different weighting.
**Displacement:** `v += weight * displacement` where `displacement` is the *tangential* projection of
the drag onto each vertex's own normal plane (Blender's Nudge moves the surface along the brush's
drag vector but flattened into the local tangent, so it does not lift the surface the way Grab does).
Concretely: `d_tan = displacement - displacement.dot(vertex_normal[v]) * vertex_normal[v]; v += weight
* d_tan`.
**Composes with:** falloff yes, Ctrl: no sensible opposite (same call as Grab), undo yes.
**Effort: S.** Same shape as Grab's case, with one extra per-vertex projection using the already-cached
`m_vertex_normals[v]`. No new session state.
**Printability:** shifting surface tangentially at high strength near a silhouette edge can fold
triangles over each other (self-intersection) exactly as a hard Grab already can; not a new risk
class.

### Thumb

**Needs:** nothing new - Thumb is Blender's "rigid Grab with a wide, flat, low falloff" - i.e. Grab
with the falloff forced to a much flatter curve and typically a larger radius. Like Draw Sharp, this is
a *falloff-curve* variant of an existing brush, not a new kernel.
**Recommendation: not a separate brush** until falloff curves are pluggable; fold into Grab's future
falloff-curve selector as a preset rather than a `BrushType`.

### Snake Hook - Grab whose anchor follows the cursor as it drags

**Needs:** this is architecturally different from every brush above: Grab's anchor is fixed for the
whole stroke and every tick's displacement is measured from that one anchor, but Snake Hook's anchor
*moves with the drag*, so the touched vertex set itself grows/changes as the cursor travels - it drags
a "worm" of geometry out behind the cursor rather than translating one patch. The kernel per tick is
still `v += weight * displacement_this_tick` (identical to Grab's case), but the **gizmo-side** stroke
logic must re-run `collect_vertices_in_radius` around the *current* cursor position every tick (instead
of the fixed anchor) and accumulate, which `SculptSession::apply()` already supports by construction
(each `apply()` call takes whatever `params.center` the caller passes this tick) - so no
`MeshSculpt.cpp` change at all, only a `GLGizmoSculpt::continue_stroke()` change to feed a moving
center instead of pinning it. This also means the `CursorMode::StrokeGrab` cursor-tracking rule in
`next_cursor_state()` already does the right thing for Snake Hook (follow the dragged patch) with no
change.
**Displacement:** identical to Grab's, `v += weight * displacement`, called with the *live* cursor
position each tick rather than a stroke-start anchor.
**Composes with:** falloff yes, Ctrl: no sensible opposite (same as Grab), undo yes (still one stroke =
one undo step, `StrokeStep`s accumulate the same way a long Grab drag already does).
**Effort: S.** No `MeshSculpt.cpp` change; the only work is in `GLGizmoSculpt`'s stroke-continuation
function, and it is a strict subset of what it already does for Grab (drop the "hold the anchor fixed"
behavior).
**Printability:** intentionally creates thin, elongated protrusions - self-intersection and
sub-nozzle-width thickness are the expected failure mode of this brush by design (it is a "pull a
horn/spike out" tool), same caution as Grab at extreme strength, slightly more likely to be hit because
that is the point of the brush.

### Rotate/Twist

**Needs:** a rotation of each vertex's tangential offset around the stroke axis, i.e. `v' = origin +
R(angle * weight) * (v - origin)` where `R` is a rotation about the brush normal. This is the first
brush in this list whose displacement is **not** `weight * (a fixed vector)` - it is
`weight`-*parameterized rotation angle* applied to the vertex's own offset, so the "vector" being
scaled by weight is different for every vertex and depends on that vertex's own position relative to
the centre, not just a shared per-brush direction. It fits `apply()`'s per-vertex loop shape fine
(no new session-level state needed, `fit_plane()`-style axis fit reused), but it is the first entry
where "the formula" is genuinely a small new derivation rather than a recombination of existing terms.
**Displacement (one line):** `v = origin + w_partial_rotation(rel, normal, weight*max_angle)` via
Rodrigues' rotation formula on `rel = v - origin` about `normal`.
**Composes with:** falloff yes, Ctrl yes (Ctrl flips rotation direction), undo yes.
**Effort: M.** Not hard math (Rodrigues' formula is a few lines), but it is new math, needs its own
angle-scale parameter (analogous to Inflate's `amount` or Crease's `crease_normal_ratio`), and -
importantly - a large-angle Twist at full strength on a wide brush is the first brush in this set
genuinely likely to produce severe local self-intersection (adjacent vertices rotated by very different
amounts as they cross the falloff boundary), so it deserves more manual test-driving before shipping
than the S-effort brushes above. Recommend for 3a+ (a follow-up phase), not the initial four.
**Printability:** high self-intersection risk at the falloff boundary (rotation is continuous in weight
but the *mesh* between a weight-1 and weight-0 vertex can still fold if the angle is large relative to
local edge length) - this is a genuinely new risk class, not a variant of an existing one.

### Layer - raised plateau to a fixed height above the original surface

**Needs:** the **original, pre-stroke** vertex position, which today's brushes do not need to retain
(Inflate reads the pre-stroke *normal* but moves from the *current* position; Layer needs the
pre-stroke *position* itself as the reference plane so repeated ticks converge on a flat plateau
instead of drifting upward the way repeated Inflate ticks would). Concretely this needs one snapshot
taken once per stroke (at `start_stroke()`, the same moment Flatten pins its plane direction) of the
touched vertices' starting positions and normals, keyed by vertex id.
**Displacement:** `v = v0 + clamp(height, 0, current_progress) * normal0` where `v0`/`normal0` are the
per-vertex stroke-start snapshot, i.e. move each vertex toward `v0 + height*normal0` and stop - a
per-vertex target clamp rather than an incremental push. This is Blender's Layer brush exactly ("raises
the surface to a fixed layer/plateau height and does not exceed it even under repeated strokes").
**Composes with:** falloff yes (weight scales how close to the target height a given tick gets, same
"lands on it at weight 1" pattern Flatten already uses), Ctrl yes (negative height = trench instead of
plateau), undo yes (still vertex-only).
**Effort: S-M.** The displacement itself is trivial once the snapshot exists; the only real cost is
plumbing a per-vertex "stroke-start snapshot" cache into `SculptSession`, which does not exist today
(every current brush is happy reading `m_its.vertices`/`m_vertex_normals` live). This is one new
`std::unordered_map<uint32_t, std::pair<Vec3f,Vec3f>>` or a parallel sparse array populated in
`start_stroke()`/cleared in `end_stroke()` - contained, but it is new session state, which is why this
is M rather than S like Pinch/Nudge/Clay.
**Printability:** a plateau at a large height relative to the local mesh creates a vertical or
overhanging step at the falloff ring - same overhang caution as Inflate/Clay, plus the plateau's flat
top can create a thin shell if `height` is large on a thin part (e.g. a boss on a wall), worth a
tooltip.

### Blob

**Needs:** nothing beyond Inflate - Blender's Blob is Inflate/Grab with a falloff that pulls the
*centre* out further than the edges relative to the standard quartic (a "spherical" bump rather than a
smooth dome), i.e. it is a falloff-curve variant again, not a new kernel.
**Recommendation: not a separate brush** for the same reason as Draw Sharp/Thumb - defer to the
falloff-curve backlog item; Inflate + a "spherical" falloff curve *is* Blob.

### Elastic deform (Kelvinlets)

**Needs:** a fundamentally different formulation from everything above. Kelvinlets (Pixar, de Goes &
James, *"Regularized Kelvinlets: Sculpting Brushes based on Fundamental Solutions of Elasticity"*,
SIGGRAPH 2017, https://www.disneyanimation.com/publications/regularized-kelvinlets-sculpting-brushes-based-on-fundamental-solutions-of-elasticity/)
compute displacement from a closed-form elastostatic Green's function evaluated at every point in
space, parameterized by force/twist/scale applied at the brush centre and a regularization radius -
the displacement of a given vertex depends on its 3D distance from the brush centre through a rational
function of elastic parameters (Poisson ratio, shear modulus), not on the mesh's own connectivity, and
volume preservation falls out of the elasticity math rather than needing a separate correction pass
(unlike Taubin, which needs the explicit +lambda/-mu two-pass trick because plain Laplacian shrinks
volume). It genuinely fits the "one formula, evaluated per vertex, no cross-vertex iteration" shape
this codebase likes - closer to Inflate's shape than Twist's - but the formula itself is materially
more involved (it is a 3x3 tensor contraction with several elastic constants, not a dot product and a
scale).
**Displacement (one line, grab case only - the paper has three: grab, twist, scale):** `v += weight *
K_grab(v - center; force, radius, nu)` where `K_grab` is the regularized Kelvinlet displacement field
from the paper's closed-form formula (not reproducible faithfully in one line; see citation above).
**Composes with:** falloff - **partially**: Kelvinlets have their own built-in radius-based
regularization that already shapes the falloff, so combining it with this codebase's separate
`falloff_weight()` multiplier is either redundant or needs the two to be reconciled (probably: use
Kelvinlets' own regularization as "the falloff" and skip `falloff_weight()` for this brush, which makes
it not compose cleanly with the shared preamble the way every other brush here does). Ctrl: yes (force
direction flips). Undo: yes, vertex-only.
**Effort: L.** New math from a paper (correctly transcribing the regularized kernel is the actual
work, not the profile-line summary above), does not cleanly reuse the shared `weights[]` preamble
(see above), and needs its own elastic-parameter exposure in the panel that the other brushes have no
equivalent for (shear modulus / Poisson ratio, or a simplified "softness" knob hiding them). Genuinely
valuable (it is the one brush here that produces the smoothest, most "physical" large-scale
deformations with zero extra volume-preservation work) but not phase-3 scope.
**Printability:** volume-preserving by construction, which is a *printability positive* relative to
Grab (a hard Grab pull thins the surface it drags from; a Kelvinlet grab conserves the swept volume) -
worth flagging as the actual motivation for eventually doing this, not just a "nice to have."

### Relax - tangential smoothing that keeps volume/shape, evens triangle sizes

**Needs:** almost nothing new - it is Smooth's one-ring-mean computation with the move projected into
the **tangent plane only**, i.e. drop the component of `(mean - v)` along `vertex_normal[v]` before
applying it. This is precisely analogous to how Pinch is Crease with the normal term dropped.
**Displacement:** `v += weight * lambda * tangential_component(mean(one_ring(v)) - v, normal[v])` -
literally Smooth's existing Jacobi update (`MeshSculpt.cpp:563-593`) with one extra projection line per
vertex.
**Composes with:** falloff yes, Ctrl: no sensible opposite (same call as Smooth itself), undo yes,
iterations yes (reuses the existing Jacobi loop and even the Taubin two-pass option verbatim - a
"Taubin Relax" is a coherent thing and falls out for free).
**Effort: S.** This is the cheapest new brush in the whole list computationally - it is almost
literally the existing Smooth case with one line changed - held out of the "top 4" only because its
*value proposition* ("evens triangle sizes before a remesh, without shrinking the silhouette") is best
demonstrated once there is a remesh step downstream to benefit from it (Subdivide today does not need
its input pre-relaxed - midpoint subdivision does not care about triangle quality - so shipping Relax
before there is a quality-sensitive remesh consumer undersells it). **Recommend as the fifth brush if
the set is widened to five, or as the first pick if 3b's masked local-remesh work ever lands and needs
a pre-conditioning pass.**
**Printability:** strictly improves mesh quality (more uniform triangle sizes) without moving the
surface much - the safest brush on this list from a printability standpoint, since (unlike Smooth) it
does not round off sharp features by pulling them toward the volumetric average, only re-distributes
vertices along the existing surface.

### Mask brush (paint a protected region)

Covered under Question 2 below - it is the same mechanism as bed-contact protection (a per-vertex
mask consulted by `apply()`'s weight loop), just painted by the user instead of computed from geometry.
**Effort:** see Question 2 - S once the mask infrastructure exists, because "painting" it is exactly
`GLGizmoPainterBase`'s existing brush-stamp-into-a-per-vertex-store pattern (the same one
`supported_facets`/`seam_facets` already use, just per-vertex instead of per-facet).

### Simplify/Dyntopo-style local remesh brush

**Needs:** everything the spec's own "v3" section already lists: a worker thread
(`GLGizmoSimplify::process()`'s mutex-guarded idle/running/cancelling pattern), an annotation story
(local remesh renumbers facets in the touched patch, so either whole-volume `clear_before_change_mesh()`
+ the existing `CustomSupportsAndSeamRemovedAfterRepair` notification, or a genuinely new old-facet ->
new-facet correspondence), and a trigger model (fire on stretch/curvature thresholds, not every tick).
Nothing in this research changes that assessment - it remains a multi-week research project, not a
phase-3 brush.
**Effort: L**, already scoped as "v3" work in the existing spec; not re-estimated here.

### Summary table

| Brush | Kind of change | Composes w/ falloff/Ctrl/undo | Effort | Notes |
|---|---|---|---|---|
| Pinch | vertex-only, reuses Crease's tangential term | yes / yes (Ctrl=Magnify) / yes | S | share code with Crease |
| Magnify | Pinch, sign flipped | yes / is-the-Ctrl-variant / yes | XS | not a separate radio entry |
| Nudge | vertex-only, tangential drag projection | yes / no opposite / yes | S | reuses Grab's `displacement` field |
| Snake Hook | vertex-only, Grab w/ moving anchor | yes / no opposite / yes | S | gizmo-side change only, no MeshSculpt.cpp change |
| Clay Strips | vertex-only, offset-plane Flatten | yes / yes (Ctrl=carve) / yes | S | fold Draw Sharp into this |
| Layer | vertex-only, needs stroke-start snapshot | yes / yes (Ctrl=trench) / yes | S-M | first brush needing new session state |
| Relax | vertex-only, Smooth w/ normal dropped | yes / no opposite / yes | S | best pre-remesh conditioner; 5th pick |
| Draw Sharp | falloff-curve variant of Clay Strips | blocked on falloff curves | - | not a separate brush |
| Thumb | falloff-curve variant of Grab | blocked on falloff curves | - | not a separate brush |
| Blob | falloff-curve variant of Inflate | blocked on falloff curves | - | not a separate brush |
| Scrape/Fill standalone | UI split of existing Flatten/Ctrl | n/a | XS | already shipped via Ctrl, skip |
| Rotate/Twist | vertex-only but new math (Rodrigues) | yes / yes / yes | M | real self-intersection risk, needs testing |
| Elastic (Kelvinlets) | new closed-form field, doesn't share `weights[]` cleanly | partial / yes / yes | L | best long-term printability story (volume-preserving) |
| Mask brush | infra shared with Q2 | n/a | S (after Q2 infra) | see Question 2 |
| Local remesh/Dyntopo | topology change | breaks annotation-preserving path | L | already scoped as v3 in existing spec |

## Question 2: auto-masking the bed-contact face

### Design

**Detection** (once, when the gizmo attaches to a volume, and again after any commit that could have
moved the bottom - i.e. re-run whenever `SculptSession` is (re)constructed):

1. Compute `z_min` of the mesh in the same volume-local coordinates `SculptSession` already works in.
2. A vertex is a *bed-contact candidate* if `z <= z_min + eps` (eps a small absolute tolerance, e.g.
   `1e-4` * the part's bounding-box diagonal, not a fixed mm value, so it scales with part size the
   way `commit_sculpted_mesh`'s `ensure_on_bed` already must reason about scale).
3. A triangle is a *bed-contact facet* if all three of its vertices are candidates **and** its face
   normal is within a small angular tolerance of `-Z` (protects against a candidate vertex that
   happens to sit at `z_min` but belongs to a near-vertical wall grazing the plane, e.g. a thin fin
   touching the bed at one edge).
4. The bed-contact vertex set is the union of vertices belonging to at least one bed-contact facet
   (tighter than "all candidate-z vertices," so an isolated near-bed vertex on a sloped wall that
   happens to graze `z_min` without being part of a flat facet is not pinned).
5. The **footprint contour** is the boundary loop of that facet set (the outer edge(s) where a
   bed-contact facet borders a non-bed-contact one) - this is what "the outline does not change" means
   operationally: the 2D polygon obtained by projecting that boundary loop to `z = z_min`.

**Pinning.** Store the result as a `std::vector<bool> m_masked` (or a bitset) sized to
`vertices_count()`, parallel to `m_vertex_normals`, populated by the detection pass. In
`SculptSession::apply()`, the shared weight loop gains one line: `weights[i] *= m_masked[verts[i]] ? 0.f
: 1.f` (or skip masked vertices from the touched set entirely, which is cheaper and also keeps them out
of `StrokeStep::moved_vertices`/`dirty_triangles` - preferable, since it means a masked vertex is
provably untouched rather than touched-with-zero-weight, which matters for the unit test in the plan
below). This is a one-line change to the *existing* shared preamble every brush already goes through,
so it applies uniformly to all current and proposed brushes with no per-brush code.

**Remeshing (Subdivide today, any future local remesh).** `its_subdivide_midpoint()` currently
subdivides uniformly and unconditionally. Two changes needed:
* New vertices introduced as edge midpoints of an all-bed-contact edge must be re-snapped to `z =
  z_min` exactly (floating-point midpoint of two z_min-valued points should already land on z_min to
  within epsilon, but re-snapping removes any doubt and is O(1) per new vertex).
* New vertices introduced as edge midpoints of an edge with **one** bed-contact endpoint and one
  non-bed-contact endpoint (i.e. midpoints that sit ON the footprint boundary) must be re-projected
  onto the **original footprint contour**, not left at the raw 3D midpoint - otherwise the boundary
  polygon gains a new vertex that is not exactly on the old edge (it is on the old edge by construction
  for a straight boundary segment, so this is actually automatic for a polygonal footprint; it only
  needs explicit handling if the boundary is later allowed to be curved/re-fit, which it is not in this
  design). In practice: because `its_subdivide_midpoint()` only ever adds a vertex at the exact
  geometric midpoint of an existing edge, and the footprint contour today is just "the polyline through
  the existing boundary vertices," a boundary edge's midpoint is automatically on the segment between
  its two boundary endpoints - so **no correction is actually needed for uniform midpoint subdivision
  specifically**; this bookkeeping only becomes necessary once a *non-midpoint* local remesh (e.g. a
  future Dyntopo-style brush) can introduce boundary vertices that are not literal edge midpoints. Call
  this out explicitly in the implementation so nobody re-derives it three times.
* The pinned vertices (interior bed-contact vertices, not just the boundary) must not be touched by
  whatever selects which triangles to subdivide, if/when subdivision becomes brush-local rather than
  whole-mesh (today's whole-mesh Subdivide already subdivides everything including the bed face, which
  is fine under the "re-snap to z_min" rule above but wasteful - subdividing a perfectly flat bed face
  produces triangles with nothing to sculpt, since they are masked. A future optimization, not a
  correctness requirement, is to skip subdividing purely-interior bed-contact triangles at all).

**General "Mask" generalization.** Represent `m_masked` as float weights in `[0,1]` rather than a bool,
so it composes with:
* **Auto bed-contact** mask: 0 for pinned vertices, 1 elsewhere, computed as above.
* **"Keep sharp edges" by dihedral angle**: for each vertex, look at the dihedral angle between each
  pair of incident facets (already have `m_vertex_faces` CSR adjacency to walk this); if the vertex
  touches an edge whose dihedral angle exceeds a threshold (e.g. 60 degrees from planar), give it a
  reduced or zero mask weight. This reuses adjacency `SculptSession` already builds
  (`build_adjacency()`), no new topology queries needed - just a new one-time pass over
  `m_vertex_faces`.
* **Paint-a-mask brush**: a new "Mask" brush entry that, instead of moving vertices, writes into
  `m_masked` directly - `mask[v] -= weight` (paint to protect) with a modifier to erase (`mask[v] +=
  weight`, clamped to `[0,1]`), exactly the increment/decrement gesture `GLGizmoPainterBase`'s
  supports/seam painting already uses, just targeting a per-vertex float array instead of a per-facet
  `TriangleSelector` tree. This is the "Mask brush" from Question 1, unified here rather than
  double-counted.
* Every other brush's weight becomes `weights[i] = strength * falloff_weight(d,r) * mask[v]`  - one
  more multiply in the existing preamble, applied uniformly.

**Showing the mask.** The paint gizmos already have a tinted-overlay rendering path
(`GLGizmoPainterBase` colors facets by their `TriangleSelector` state, e.g. supported/blocked). The
mask is per-vertex rather than per-facet, so the natural analog is a vertex-color overlay (interpolate
mask value across each triangle) rather than a flat per-facet tint - visually this reads as a soft edge
at the mask boundary instead of a hard facet-aligned edge, which is arguably the more honest picture
since the mask itself is a continuous weight. This needs a new render path in `GLGizmoSculpt` (there is
no existing per-vertex-color overlay to borrow directly - the closest analog, MMU segmentation, is also
per-facet) - budget this as the single most GUI-code-heavy piece of 3b.

**Panel toggles.** Three checkboxes, matching the panel style already established in v1.1/v2's rewrite
(`text_wrapped`, fixed-width panel):
* "Protect bed contact" - on by default when the gizmo attaches to a part actually resting flat on the
  bed at z_min (i.e. when the detection pass in step 1-4 above finds a non-empty bed-contact facet
  set); off/disabled (greyed, with an explanatory wrapped note) for a part with no flat bottom.
* "Protect sharp edges" - off by default (it is a stronger, more surprising restriction than bed
  protection - a user sculpting near a deliberate hard edge like a chamfer may well want to soften it).
* "Mask brush" - selects the paint-a-mask brush as the active tool; not a toggle on other brushes, a
  distinct entry in the brush radio row (or a separate mode button next to it, since painting the mask
  is conceptually a different action from sculpting with it active).

**Persistence.** Recommend **session-only** for phase 3b: the mask (whichever of the three sources
populated it) lives in `SculptSession`, rebuilt each time the gizmo attaches to the volume (auto
bed-contact and sharp-edge detection are pure functions of the current mesh, so recomputing them on
attach is both correct and cheap - no staleness risk) and **discarded** when the gizmo closes, the same
lifetime as `SculptSession` itself today. A user-painted mask is the one piece that is lossy under
this choice (re-opening the gizmo loses hand-painted protection), which is the honest trade-off to
flag: making the painted mask persistent would mean a fifth `FacetsAnnotation`-style store on
`ModelVolume` (a `PaintedMask` alongside `supported_facets` et al.), a 3MF schema change, and a
migration story - real scope, not a phase-3b afterthought. **Recommend: ship session-only first; revisit
persistence only if hands-on testing shows users repeatedly repainting the same mask across gizmo
sessions.** Auto-detected masks (bed contact, sharp edges) need no persistence question at all since
they are cheap to recompute from the current mesh every time.

**Interaction with the paint-preserving commit path.** No change needed to
`commit_sculpted_mesh()`/`indices_match()` at all, as long as masking is enforced purely by
**preventing vertices from moving** rather than by any post-hoc correction - the commit path already
only cares that `its.indices` is unchanged, and a masked vertex that never moved trivially satisfies
that. The one thing to verify explicitly (see test plan) is that Subdivide's re-snap/re-projection step
for bed-contact vertices happens *before* whatever the subdivide path does today, so that a masked
vertex's position is bit-identical to its pre-subdivide value when subdivision does not need to move it
at all (interior bed vertices are literally unchanged by `its_subdivide_midpoint()` - it only adds new
vertices at edge midpoints, never moves an existing one - so today's function already satisfies this
for pre-existing vertices; the re-snap discussion above only concerns *new* vertices).

### Effort and risks

Effort for the whole of 3b: **M-L**. Detection pass and the `weights[i] *= mask[v]` change are S each;
the per-vertex mask overlay render path and the paint-a-mask brush's `TriangleSelector`-style
increment/decrement gesture (adapted to a per-vertex float array rather than the existing per-facet
tree) are the bulk of the work, each roughly comparable to one existing paint gizmo feature.

Risks, as asked:

* **A fillet that meets the bed at an angle.** The dihedral-angle-based facet normal check (step 3
  above, `-Z` within tolerance) correctly *excludes* a filleted/chamfered edge from the pinned set,
  since its facet normals are not `-Z` - but this means the footprint contour for such a part is the
  inner edge of the flat pad, not the true outermost silhouette at z_min (a fillet's lowest ring of
  triangles, being sloped, is unmasked and sculptable, which could in principle let a stroke nibble the
  visual footprint even though the literal flat pad is protected). This is the correct, conservative
  choice (protect only what is unambiguously flat and bed-normal) but the panel copy should say
  "protects the flat area touching the bed," not "protects the footprint," since for a filleted part
  those are different things.
* **A non-planar bottom** (no z_min-normal facets at all, e.g. a sphere or a part printed on a raft with
  a single point of contact). Detection naturally produces an empty bed-contact set in this case; "Protect
  bed contact" should simply report nothing to protect and grey itself out rather than doing anything
  surprising. No special-case code needed beyond the toggle-disable UX already described.
* **Multiple bottom islands** (e.g. a part with two separate feet, or a multi-part object where each
  part's own z_min differs). The detection as specified operates per-volume in volume-local
  coordinates, so a single volume with two disjoint flat regions at the same z_min both correctly
  join the bed-contact set (nothing in the algorithm assumes one connected island - "facet normal
  near -Z and z near z_min" is a per-facet predicate, not a flood fill from one seed). The footprint
  *contour* extraction, however, must handle multiple boundary loops (one per island) rather than
  assuming a single polygon - this is a data-structure detail (a `vector<vector<uint32_t>>` of loops
  instead of one loop) rather than an algorithmic risk, but it is exactly the kind of thing that is
  easy to gloss over and get a single-loop assumption baked in by accident; flag it for the
  implementer. A multi-part **object** (several `ModelVolume`s) is out of scope by construction, since
  the gizmo already binds to a single selected part (noted as unverified-but-existing behavior in the
  v1/v1.1 spec).

## Test plan

**Unit (extends `tests/libslic3r/test_mesh_sculpt.cpp`, in the existing `[Sculpt]`-tag style):**

* *Bed-contact vertices unchanged in z after Subdivide + Smooth with mask on.* Build a mesh with a flat
  bottom (e.g. the existing test cube), run the bed-contact detection, subdivide, then run a Smooth
  stroke whose brush sphere covers the whole part including the bottom face; assert every originally-
  pinned vertex (matched by pre-subdivide position, since ids are not preserved across subdivision) is
  at exactly the original `z_min` (bit-for-bit, since nothing should have touched it) and every
  *newly-added* bed-face vertex is also at exactly `z_min` (within the re-snap tolerance).
* *Footprint polygon area/perimeter unchanged within 1e-6.* Extract the boundary loop before and after
  the Subdivide + Smooth sequence above, compute polygon area (shoelace) and perimeter, assert both
  within `1e-6` relative tolerance (absolute for a part near the origin, relative for one that is not,
  matching the existing test suite's general float-comparison style).
* *A brush with mask leaves masked vertices at identity.* Run every brush type (Grab, Inflate, Smooth,
  Flatten, Crease, and whichever of the phase-3a four have landed) with the brush sphere centered
  directly on and covering the masked bed face, strength 1, falloff off (hardest case - every vertex
  in radius would otherwise get full weight); assert `moved_vertices` contains none of the masked ids
  and every masked vertex's position is bit-identical (`==`, not epsilon-close) to before the call -
  this is the strongest and simplest test in the plan, since "did not touch" needs no tolerance at all
  given masked vertices are skipped from the touched set rather than moved-with-zero-weight.
* *New brush kernels, phase 3a* (once implemented): the standard pattern already used for Flatten/Crease
  in the existing suite - Pinch reduces distance-to-centre-line monotonically and has zero normal
  component (dot with `vertex_normal` is not asserted zero since the *move* has zero normal component
  by construction, but distance to the fitted plane along `normal` should be unchanged, which is the
  simplest way to assert "no normal component" without reimplementing the projection in the test);
  Magnify is Pinch with the sign flipped, vertex-for-vertex; Nudge's move is orthogonal to
  `vertex_normal[v]` for every touched vertex; Snake Hook accumulates like a multi-tick Grab (reuse the
  existing Grab test's tick-accumulation pattern with a moving center); Clay Strips never moves a vertex
  that already exceeds the target offset (one-sided clamp) and moves a vertex exactly to the target at
  weight 1; Layer converges to the snapshot-plus-height plane under repeated ticks and does not
  overshoot it (the defining Layer behavior, worth its own explicit assertion since it is what
  distinguishes Layer from Inflate).

**Manual / CLI (extends the existing gate-script pattern, `snorca_hubtest/gate_sculpt_cli.sh`):** slice
a part with a flat bottom before and after a masked Subdivide + Smooth pass and diff the g-code's first
layer perimeter coordinates - this is the sanity check that "footprint unchanged" survives all the way
through slicing, not just in the mesh, mirroring the existing "untouched P1S slice... 0 differing
lines" determinism check the v1 spec already runs for the unmasked case.

## Phasing

* **Phase 3a** - Pinch, Magnify (free with Pinch), Nudge, Snake Hook, Clay Strips (folding in Draw
  Sharp as a future falloff-curve variant rather than a separate brush). All vertex-only, all reuse the
  shared `weights[]` preamble, no new `SculptSession` state except Clay Strips' one new scalar
  parameter. Layer and Relax are documented above as strong follow-ups (Layer needs new per-stroke
  snapshot state, Relax's value proposition sharpens once a remesh consumer exists) - fold whichever of
  the two the owner prefers into 3a if a fifth or sixth slot is wanted; both are the same S-M effort
  class as the core four.
* **Phase 3b** - mask infrastructure (per-vertex float weights consulted by the shared preamble),
  auto-detected bed-contact protection, auto-detected sharp-edge protection, the paint-a-mask brush, the
  mask overlay render path, and the three panel toggles. Ships independently of 3a; recommended to
  follow it so the "Mask brush" toggle has more brushes worth protecting a region from when it lands.
* **Deferred past phase 3** (this report's judgment, not asked for but worth being explicit about so it
  is not silently expected): Rotate/Twist (real self-intersection risk, needs hands-on tuning before
  shipping), Elastic/Kelvinlets (new math from a paper, does not cleanly share the `weights[]`
  preamble, best long-term printability story but genuinely more work), painted-mask persistence across
  gizmo sessions (needs a new `FacetsAnnotation`-equivalent store and a 3MF schema change), and any
  local-remesh/Dyntopo brush (already correctly scoped as "v3" in the existing spec - a worker thread,
  an annotation story, and a trigger model, none of which this report found reason to re-open).

## References

* Blender Manual, Sculpting brushes - the standard vocabulary and default-tool descriptions used
  throughout this report (Grab, Clay Strips, Layer, Pinch/Magnify, Snake Hook, Scrape/Fill, Draw
  Sharp, Blob, Thumb, Nudge, Rotate, Mask):
  https://docs.blender.org/manual/en/latest/sculpt_paint/sculpting/tool_settings/brushes/brushes.html
* Blender Manual, Mask - the paint-a-protected-region UX this report's Mask brush follows:
  https://docs.blender.org/manual/en/latest/sculpt_paint/sculpting/tools/mask.html
* de Goes & James, "Regularized Kelvinlets: Sculpting Brushes based on Fundamental Solutions of
  Elasticity," ACM SIGGRAPH 2017 - "brushes ... based on ... elasticity" grounded in a closed-form,
  volume-preserving displacement field, cited above for the Elastic deform brush:
  https://www.disneyanimation.com/publications/regularized-kelvinlets-sculpting-brushes-based-on-fundamental-solutions-of-elasticity/
* Meshmixer (Autodesk), sculpting brush reference (Drag/Draw/Inflate/Pinch/Flatten/Robust Smooth
  family and its printability-oriented framing of sculpting on top of scanned/print-bound meshes,
  the closest existing tool to this project's own "touch up a model you already have" framing):
  https://www.meshmixer.com/
