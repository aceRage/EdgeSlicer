#ifndef slic3r_DrawCut_hpp_
#define slic3r_DrawCut_hpp_

#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "BoundingBox.hpp"
#include "CurvedCut.hpp"

#include <vector>
#include <utility>
#include <cstdint>
#include <functional>
#include <optional>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Draw cut, phase 3 (2026-09-13, owner feedback): the cut surface of a CLOSED
// loop is a BAND plus a FLAT CORE, not a ruled strip.
//
// Phases 1 and 2 lofted a ruled strip: every loop point got a straight ruling
// along its own inward surface normal, and the "cut" was the tube those rulings
// swept. On a cylinder that tube is a ring of rays pointing at the axis from all
// round, so Depth projected OUTWARDS and Through all drove the rulings through
// the part at whatever angles the normals happened to have - the translucent
// disc and cyan slivers the owner screenshotted. There was no flat face anywhere,
// so the two halves had nothing to key against.
//
// The model now matches what a model-splitting tool (Split3r and its kin) means
// by this kind of cut: a drawn line on the skin says WHERE the cut meets the
// surface, the middle of the cut is a CLEAN FLAT PLANE, and the transition from
// skin to plane can be angled so the halves key together.
//
//   1. CORE PLANE P. The best-fit plane of the loop - Newell/PCA normal n through
//      the centroid c. With Direction = Axis the chosen axis is n instead (still
//      through c), so the user can force the core square to an axis. The core is
//      the region of P inside the loop's projection.
//
//   2. SKIN BAND, and THE DRAWN LINE IS ON IT. Three rings, not two - for each loop
//      point p the surface runs
//         p_out = p - extension * d(p)            the SKIRT, clear of the skin
//         p                                       the DRAWN LINE, exactly
//         p_in  = p + depth * d(p)                (then projected onto P along n)
//      where d(p) is the in-plane inward direction (towards the axis through c,
//      perpendicular to n) tilted by `angle` towards n. See DrawCutParams::angle_deg.
//      `depth` is how far the band travels before the surface turns onto the core,
//      and it is measured FROM THE DRAWN LINE, never from the skirt tip.
//
//      p being its own ring is the whole point, not a detail of the triangulation:
//      the drawn line is WHERE THE CUT MEETS THE SKIN, so it has to be on the surface
//      exactly. Lofting straight from p_out to p_in - which is what this did first -
//      leaves p merely near the surface, and the skin crossing then lands part-way
//      along that loft carrying a part-way height: a hand-drawn wave arrived at the
//      skin damped by extension / (extension + depth * cos(angle)), so the more
//      Extension the user asked for the flatter their cut came out.
//
//      The skirt runs along -d, i.e. it CONTINUES THE BAND'S OWN SLOPE outward rather
//      than leaving along the skin normal. That keeps the surface C1 across the drawn
//      line (no crease exactly where the boolean meets the skin) and gives the smaller
//      lateral reach of the two candidates - E * cos(angle) against a flat skirt's
//      full E - so it is the harder one to fold on a concave stretch.
//
//   3. CORE POLYGON. The loop projected onto P and INSET by the band's in-plane
//      footprint (depth * cos(angle)), so band and core join along one closed
//      curve and the result is watertight. Because every p_in is put onto P by
//      construction, the core is flat even when the drawn loop is not planar -
//      which is the whole point: a wavy hand-drawn line still yields a flat
//      mating face.
//
//   4. THROUGH ALL. No core plane at all: the band is extruded along d(p) all the
//      way through the part (reach from the bbox, both sides), giving a tapered
//      plug/socket with no flat middle.
//
// The SAME surface feeds the boolean, the inside field (halves colouring,
// Visible/Ghost/Hidden) and the connectors, so a connector dropped on the band or
// on the core stands on the face that will actually exist. Extension only ever
// changes how far the band reaches OUTSIDE the skin.
//
// An OPEN stroke keeps the phase-1/2 ruled strip: there is no loop, so there is
// no interior to put a core plane in. Everything below that talks about a core is
// closed-loop only.
//
// ---------------------------------------------------------------------------
// Phase 1 (historical): the cut surface is a RULED STRIP swept along a stroke the
// user draws on the model's surface.
//
// This is a PEER of the curved sheet, not a variant of it. The sheet is a height
// field z = f(u,v) over the whole cut plane - single-valued by construction,
// which is what makes it self-intersection-free and what makes a zero sheet the
// flat cut exactly. A drawn stroke is a CURVE, and the surface it generates is
// swept along that curve: it is not single-valued over the plane, so it cannot
// be a CurvedCutSheet. Rasterising a stroke into a height field would lose the
// undercuts and draft angles the feature exists for.
//
// What IS shared is everything downstream: draw_cut_split() builds a closed
// cutter solid and hands it to cut_with_solid() (CurvedCut.hpp), the same
// Manifold->mcut chain with the same winding flip, the same
// both-sides-with-complement-recovery and the same two-solid kerf that the
// curved cut uses. Nothing about the boolean is written twice.
//
// Coordinates: the stroke lives in the CUT PLANE's own frame, the same frame
// cut_mesh() slices at z == 0 and the same frame CurvedCutSheet lives in. That
// is what lets the stroke survive a plane nudge the way the sheet does.
//
// PHASE 1 SCOPE: capture, the as-is directions (Surface normal with no angle,
// View, Axis X/Y/Z), Extension, Depth (through-all by default), the split, and
// the gates. No draft angle, no connectors on the drawn surface, no line editing
// past clear/undo. Phase 2 adds the angle (the binormal rotation with parallel
// transport), the fold clamp, connectors and control points.
// ---------------------------------------------------------------------------

// One captured point of a stroke: where the ray hit the model, the surface
// normal there, and which facet it was. All in the cut plane's frame; the normal
// is a unit vector in that frame too.
struct DrawCutSample
{
    Vec3d  pos{ Vec3d::Zero() };
    Vec3d  normal{ Vec3d::UnitZ() };
    size_t facet{ 0 };
};

// Which way the cut surface reaches INTO the part.
enum class DrawCutDirection {
    // Along the inward surface normal at each sample: the cut goes straight in,
    // perpendicular to the face the stroke was drawn on, so it follows the shape
    // the user drew on. Per-sample, so the surface is genuinely swept.
    SurfaceNormal,
    // Along a single constant direction, the same at every sample - an "as-is"
    // extrusion, i.e. a prism through the stroke. View takes the camera's own
    // forward direction (the gizmo supplies it, already in the plane frame);
    // AxisX/Y/Z take a plane-frame axis.
    View,
    AxisX,
    AxisY,
    AxisZ
};

struct DrawCutParams
{
    DrawCutDirection direction{ DrawCutDirection::SurfaceNormal };
    // The constant direction for DrawCutDirection::View, in the CUT PLANE's
    // frame, pointing INTO the part (i.e. away from the camera). Ignored for
    // every other direction. Need not be normalised.
    Vec3d            view_dir{ -Vec3d::UnitZ() };
    // How far the surface reaches PAST the stroke, in mm - measured ALONG the cut
    // direction, not sideways (see draw_cut_cutter_solid for why sideways would be
    // a draft rather than an extension). It does two jobs: it lifts the surface's
    // rim clear of the face the stroke was drawn on, so a stroke in a concavity is
    // not left with its rim buried in material, and for an OPEN stroke it is also
    // how far past each END the surface reaches along the tangent - which is what
    // lets the cut get past the silhouette so the boolean separates the part.
    double           extension{ 5.0 };
    // THE LIP ANGLE, in DEGREES, 0..90 (see the band/core model above). It is the
    // tilt of the band's travel direction d(p) TOWARDS the core normal n:
    //
    //   d(p) = cos(angle) * inward(p) + sin(angle) * (-n)
    //
    // where inward(p) is the in-plane inward direction at p (towards the axis /
    // centroid, perpendicular to n). So:
    //
    //   0   - the band runs straight IN, perpendicular to n: it lies in the plane
    //         through p parallel to the core plane. A flat shelf around the core.
    //   45  - a 45-degree chamfer lip, the classic keyed joint.
    //   90  - straight along -n: a straight-walled plug/socket, no lip at all.
    //
    // Unlike the old phase-2 draft angle this is NOT signed and NOT a rotation
    // towards the binormal; it is the angle at which the outer shell projects
    // towards the inner flat projection, which is what the owner asked for and
    // what Split3r-style keyed halves need.
    // THE SIGN, 2026-09-15 owner item 5. The angle is now -90..90, and the SIGN is
    // which side of the core plane the band leans towards:
    //
    //   +  the band leans along -n as it travels in: the lip goes DOWN, away from
    //      the side the loop was drawn on. This is what 0..90 always did, so every
    //      stored recipe and every existing cut keeps its meaning exactly.
    //   -  the band leans along +n: the lip goes UP, back towards the viewer. On a
    //      cylinder that is the mirrored band the owner asked for - the inner ring
    //      on the other side of the core plane from where the old one put it.
    //
    // A signed angle rather than a separate "Flip" toggle: the two controls would
    // have to be read together to know what the cut does (a flipped 30 and a plain
    // -30 are the same surface), and a slider that runs through zero shows the whole
    // family in one gesture. Zero is still the flat shelf, and it is still the
    // boundary between the two directions rather than a third case.
    double           angle_deg{ 0.0 };
    // THE EXTENSION ANGLE, in DEGREES. 2026-09-15, owner item 4. The direction the
    // outward SKIRT leaves the drawn line along, as a tilt about the same in-plane
    // inward direction the band uses:
    //
    //   skirt_dir(p) = -draw_cut_band_dir(inward(p), n, extension_angle_deg)
    //
    // i.e. the skirt runs OUT along the reverse of a band ruling built at THIS angle
    // instead of at `angle_deg`.
    //
    // THE DEFAULT IS "CONTINUE THE BAND", which is what the skirt has always done -
    // it leaves along -d, the band's own ruling run backwards, so the surface is C1
    // across the drawn line. That is extension_angle_deg == angle_deg, and because
    // the useful default has to track a parameter rather than be a constant, it is
    // spelt as an EMPTY OPTIONAL rather than as a number: `std::nullopt` means "the
    // same as Angle", which stays true when the user then moves Angle.
    //
    // A value makes the skirt independent: 0 lays it flat in the plane through p
    // parallel to the core, +90 sends it straight out along +n, -45 tucks it under.
    //
    // THE SIGN READS THE SAME WAY ROUND AS THE BAND'S, which is what makes the
    // default the identity rather than a flip: the skirt is the REVERSE of a ruling,
    // so it carries the opposite component along n to a band at the same angle, and
    // a band at +A (leaning along -n) is continued by a skirt leaving along +n.
    // Larger lifts the tip; negative tucks it under. The C1-ness across the drawn
    // line is then the user's to spend.
    std::optional<double> extension_angle_deg{};
    // How far the band travels INWARD along d(p), in mm, before the surface turns
    // onto the core plane. This is the width of the lip measured along its own
    // travel, and it is what sets how far the core polygon is inset from the loop.
    // Larger depth = smaller flat core.
    //
    // `through_all` drops the core plane entirely: the band is extruded all the
    // way through the part along d(p) with the same angle semantics, giving a
    // tapered plug/socket cut with no flat middle. The extrusion reaches beyond
    // the mesh on both sides (from the bbox) so the cut is always complete.
    bool             through_all{ false };
    double           depth{ 3.0 };
    // Kerf, in mm, and where the removed band sits. Shared with the flat and the
    // curved cut (CurvedCut.hpp).
    double           thickness{ 0.0 };
    CutThicknessOffset thickness_offset{ CutThicknessOffset::Centred };
};

// Why a stroke cannot be cut with. Kept as an enum rather than a bool so the
// panel can say WHICH thing is wrong - the spec asks for a clear error, and
// "the line crosses itself" and "draw a longer line" want different words.
enum class DrawCutError {
    None = 0,
    // Fewer than MinSamples after resampling, or an arc length under MinLength.
    TooShort,
    // A closed stroke that crosses itself has no well-defined interior, and an
    // open one that does has no well-defined upper side either. Phase 2 could
    // split it into sub-loops; phase 1 refuses.
    SelfCrossing,
    // The stroke jumped across empty space: the gap between consecutive HITS was
    // wider than a few sample spacings, so bridging it would have run a chord
    // through air. finish() repairs that by keeping the LONGEST contiguous run,
    // and when that run is usable there is no error at all - a line that crossed a
    // hole is meant to work. This is reported when the kept run is itself too
    // short, because then the reason the user needs is "your line left the model"
    // rather than "draw a longer line": their line WAS long enough.
    LeavesMesh,
    // The generated cutter, or one of the booleans, produced nothing usable.
    CutterDegenerate,
    // The cut ran but one side came back with no material.
    EmptySide,
    // 2026-09-12, owner feedback items 1 and 2: the CHAIN is not closed yet, so
    // there is no cut surface to make. Distinct from TooShort because the line may
    // be perfectly long and perfectly good - it simply is not a loop yet, and the
    // thing the user has to do about it (carry on from an endpoint until it snaps)
    // is different from "draw a longer line".
    NotClosed
};

// Human-readable, untranslated. The gizmo wraps these in _L(); the tests read
// the enum.
const char* draw_cut_error_message(DrawCutError err);

// ---------------------------------------------------------------------------
// The stroke.
// ---------------------------------------------------------------------------

class DrawCutStroke
{
public:
    // Default resample spacing, in mm of arc length.
    static constexpr double DefaultSpacing = 1.0;
    // A stroke needs at least this many samples and this much arc length to
    // generate a surface at all. Four samples is three spans - the minimum that
    // can have a tangent at an interior point.
    static constexpr int    MinSamples = 4;
    static constexpr double MinLength  = 2.0;
    // Smoothing: the panel's 0..1 maps onto 0..MaxSmoothPasses windowed-average
    // passes over the positions and the normals. A raw stroke picks up every
    // triangle normal it crossed, and an unsmoothed normal field visibly kinks
    // the ruled surface.
    static constexpr int    MaxSmoothPasses = 10;

    DrawCutStroke() = default;

    // --- capture ----------------------------------------------------------
    // Append one raw sample. Capture appends in chronological order and does no
    // filtering: finish() is where resampling, smoothing and the open/closed
    // decision happen.
    void append(const Vec3d& pos, const Vec3d& normal, size_t facet = 0);
    void clear();
    bool empty() const { return m_samples.empty(); }
    size_t size() const { return m_samples.size(); }
    const std::vector<DrawCutSample>& samples() const { return m_samples; }

    // Resample to `spacing` arc length, smooth by `smoothing` in [0,1], then
    // decide open vs closed. Returns the error state, which is also what
    // error() reports afterwards. `force_closed` snaps the ends together even
    // when they did not meet within the tolerance.
    //
    // Idempotent in the sense that matters: calling it twice with the same
    // arguments on the same raw samples gives the same result, because it always
    // starts from the RAW samples rather than from the last finished ones.
    DrawCutError finish(double spacing = DefaultSpacing, double smoothing = 0.2, bool force_closed = false);

    // --- the finished stroke ----------------------------------------------
    // The resampled, smoothed samples finish() produced. Empty until finish()
    // has run and succeeded.
    const std::vector<DrawCutSample>& path() const { return m_path; }
    // PHASE 2. Replace the finished path IN PLACE, keeping the open/closed
    // decision, and recompute the binormals from it.
    //
    // This exists for exactly one caller: the gizmo's re-projection of the smoothed
    // path back onto the mesh (phase 1 deviation #7). That has to happen AFTER
    // finish() - smoothing is what moves the samples off the surface - and it must
    // not re-resample, because the path already is resampled and running the
    // resampler over its own output would walk the samples a little further every
    // time a slider moved. The binormals DO have to be recomputed, because they are
    // built from the positions and the normals this replaces.
    //
    // Refuses a path of a different length, which would invalidate the closed
    // decision finish() made.
    void set_path(const std::vector<DrawCutSample>& path);
    bool         is_closed() const { return m_closed; }
    DrawCutError error() const { return m_error; }
    bool         valid() const { return m_error == DrawCutError::None && m_path.size() >= size_t(MinSamples); }
    // Arc length of the finished path, in mm. For a closed stroke the closing
    // span counts.
    double       length() const;
    // Centroid of the finished path's positions.
    Vec3d        centroid() const;

    // Unit tangent at path sample i, central-differenced (and wrapped for a
    // closed stroke). Zero-length only for a degenerate path.
    Vec3d        tangent(size_t i) const;
    // The OUTWARD binormal at path sample i: normalize(t x n), with its sign
    // fixed so it points AWAY from the loop's interior for a closed stroke
    // (b . (p - centroid) > 0). Winding therefore decides, so a loop drawn
    // clockwise and one drawn counter-clockwise give the same plug and the user
    // does not have to care which way they dragged.
    //
    // For an open stroke there is no interior, so the sign is carried along the
    // stroke from the first sample (the branch that keeps b_i . b_{i-1} > 0) -
    // a pointwise t x n flips across an inflection and would tear the strip.
    Vec3d        binormal(size_t i) const;

private:
    void compute_binormals();

    std::vector<DrawCutSample> m_samples;  // raw, as captured
    std::vector<DrawCutSample> m_path;     // resampled + smoothed
    std::vector<Vec3d>         m_binormal; // per path sample, sign-continuous
    bool                       m_closed{ false };
    DrawCutError               m_error{ DrawCutError::TooShort };
};

// ---------------------------------------------------------------------------
// The three gaps the research spec names. Only the resampler is needed in phase
// 1 (a dragged stroke, not click-by-click, and no Catmull-Rom over a space
// curve until phase 2's control points).
// ---------------------------------------------------------------------------

// Resample a 3D polyline with per-sample normals to a fixed arc length.
//
// There is NO 3D resampler in this codebase to reuse: Polyline's
// equally_spaced_points / simplify are 2D integer Points, and Polyline3 is a stub
// declaring only lines(). So: walk the arc length, lerp position and normal,
// renormalise the normal.
//
// It does NOT inherit equally_spaced_points' wart of emitting the first point but
// not reliably the last: the final input sample is always appended (unless it
// lands within half a spacing of the previous output, where appending it would
// produce a short span the tangent maths would then trip on).
//
// `closed` resamples the closing span too and leaves the ring OPEN in the output
// (the last sample is not a duplicate of the first) - which is the representation
// the strip builder wants, since it wraps.
std::vector<DrawCutSample> draw_cut_resample(const std::vector<DrawCutSample>& in, double spacing, bool closed = false);

// N windowed-average passes over positions and normals, `strength` in [0,1]
// blending each sample toward the average of its two neighbours. Endpoints are
// held for an open stroke and wrapped for a closed one. Normals are renormalised
// after every pass.
//
// Nothing here re-projects onto the mesh: that needs a raycaster, which lives in
// the GUI. The gizmo re-projects after smoothing (MeshRaycaster::get_closest_point);
// headless, the samples stay where the average puts them, which for the small
// displacements a couple of passes produce is within the chord sag of the surface
// anyway.
void draw_cut_smooth(std::vector<DrawCutSample>& path, int passes, bool closed, double strength = 0.5);

// The passes a panel `smoothing` in [0,1] asks for.
int draw_cut_smooth_passes(double smoothing);

// Closing tolerance: a stroke is CLOSED when its last sample is within this of
// its first. max(3 * spacing, 2 mm) - three spacings so a hand-drawn loop that
// nearly met still counts, and a 2 mm floor so a finely resampled stroke does
// not need pixel accuracy.
double draw_cut_closing_tolerance(double spacing);

// ---------------------------------------------------------------------------
// The cut direction, and the draft angle that tilts it. PHASE 2.
// ---------------------------------------------------------------------------

// The panel's limit on the draft angle, in degrees, and the limit the geometry
// itself imposes. 60 degrees is already a very deep undercut / flare; past about
// 80 the ruling is so close to tangent to the surface that the strip grazes the
// face it was drawn on for its whole length and the boolean has nothing clean to
// work with.
static constexpr double DrawCutMaxAngleDeg = 60.0;

// The unit ray at path sample `i`, pointing INTO the part - the ruling direction
// of the swept strip, and the one place the draft angle is applied.
//
// For the constant directions it is that direction, at every sample. For Surface
// normal it is the inward normal rotated toward the OUTWARD binormal by
// params.angle_deg:
//
//   d_i = cos(theta) * (-n_i) + sin(theta) * b_i
//
// which is a rotation in the plane the two span, so d stays unit and the ruling
// stays a straight line. theta == 0 gives -n_i exactly, which is phase 1's
// behaviour bit for bit.
//
// Exposed (it was file-static in phase 1) because the fold guard, the surface
// frame and the tests all have to ask the SAME question the cutter builder asks.
Vec3d draw_cut_inward_dir(const DrawCutStroke& stroke, const DrawCutParams& params, size_t i);

// True when a CLOSED stroke's binormal field does not close up on itself - the
// frame comes back flipped after going round the loop, so there is no consistent
// "outward" side and a draft angle would flare one way on one part of the loop
// and the other way on the rest (a Moebius path on the surface).
//
// compute_binormals() already orients a closed loop's field from the centroid
// rather than by transport, so the field itself never tears; what this detects is
// the case where that orientation is fighting the surface - adjacent binormals
// that disagree in sign. The caller falls back to angle 0 and warns.
bool draw_cut_frame_holonomy_flips(const DrawCutStroke& stroke);

// ---------------------------------------------------------------------------
// THE CORE PLANE. Phase 3.
// ---------------------------------------------------------------------------

// The best-fit plane of a CLOSED stroke's path: `normal` by Newell's method over
// the loop (which is the area-weighted normal, so it degrades gracefully on a
// wavy loop instead of picking a random eigenvector), `centroid` the mean of the
// path points.
//
// With DrawCutDirection::Axis{X,Y,Z} the axis REPLACES the fitted normal - the
// user is saying "make the mating face square to this axis" - but the centroid is
// still the loop's own, so the plane still passes through the middle of the loop.
// DrawCutDirection::SurfaceNormal and View both fit.
//
// The normal's sign is pinned so it points along the loop's own winding (right
// hand rule); draw_cut_core_plane() then flips it if needed so it agrees with the
// direction the band travels, which is what keeps "upper" stable.
//
// Returns false for a stroke that is not valid() or not closed.
bool draw_cut_core_plane(const DrawCutStroke& stroke,
                         const DrawCutParams& params,
                         Vec3d&               normal,
                         Vec3d&               centroid);

// WHERE THE CORE ACTUALLY SITS: the plane the flat middle is built on, which is
// the fitted plane DISPLACED along `normal` to the depth the band arrives at.
//
// draw_cut_core_plane() gives the plane's ORIENTATION and the loop's own centroid;
// that is the right plane to talk about the loop, but it is not where the mating
// face ends up - the band travels `depth` first. This is the plane to measure the
// cut against, and the one a "is this vertex on the core?" test has to use.
//
// `point` receives a point on it; `normal` the same normal draw_cut_core_plane()
// returns, already sign-pinned. Returns false for a stroke that is not a valid
// closed loop, and for through-all (where there is no core at all).
bool draw_cut_core_face(const DrawCutStroke& stroke,
                        const DrawCutParams& params,
                        const BoundingBoxf3& bbox,
                        Vec3d&               normal,
                        Vec3d&               point);

// The in-plane INWARD direction at path sample `i`: the component of (c - p_i)
// perpendicular to the core normal, normalised. This is "towards the axis" for a
// loop round a cylinder and "towards the centroid" for a loop on a flat face,
// which are the same thing expressed once.
//
// Falls back to the old inward surface normal for an open stroke (no core).
Vec3d draw_cut_core_inward(const DrawCutStroke& stroke,
                           const DrawCutParams& params,
                           const Vec3d&         normal,
                           const Vec3d&         centroid,
                           size_t               i);

// The band's travel direction at sample `i`: `inward` tilted by params.angle_deg
// towards -normal. Angle 0 gives `inward` exactly (the band lies in the plane
// through p parallel to P); angle 90 gives -normal (a straight-walled plug).
//
// This is the one place the lip angle is applied, so the cutter, the surface
// queries, the inside field and the tests all bend the same way.
Vec3d draw_cut_band_dir(const Vec3d& inward, const Vec3d& normal, double angle_deg);

// ---------------------------------------------------------------------------
// THE STROKE'S OUTWARD SIDE. 2026-09-15, owner click-test item 1.
// ---------------------------------------------------------------------------

// The direction that points OUT OF THE MESH on the side the loop was drawn on:
// the average of the stroked facets' own outward normals, taken along the core
// normal `n` and returned as +n or -n.
//
// WHY THIS IS NOT draw_cut_core_plane()'s SIGN. That sign is pinned only when the
// average skin normal is a meaningful fraction of the samples it came from
// (norm > 0.25 n), because on a loop round a cylinder's barrel the radial normals
// cancel and the average is pure noise. That guard is right for the BAND, whose
// travel direction is winding-independent anyway - but Through all has nothing
// else to go on: it is a prism, and which way it runs is entirely this sign. A
// loop on the bunny's head has normals spread over most of a hemisphere, so the
// average is weak, the guard declines to pin, and the prism ran whichever way
// Newell's winding happened to fall - the ears came off as separate slabs and the
// face as a jagged fragment.
//
// So this asks the MESH rather than only the samples. The average skin normal
// gives the candidate; a parity test just inside the candidate's opposite
// direction confirms it (a point a short way along -outward from the drawn line
// must be INSIDE the part, and one along +outward must not be). Where the samples
// agree strongly the parity test merely confirms them; where they cancel, the mesh
// decides, which is the whole point.
//
// `mesh` may be null, in which case only the samples are used and the answer
// degrades to the old guard's - still deterministic, just unconfirmed.
//
// Returns a unit vector parallel to `n`. `n` must be the sign-pinned core normal.
Vec3d draw_cut_outward_side(const DrawCutStroke&        stroke,
                            const Vec3d&                n,
                            const indexed_triangle_set* mesh);

// The panel's limit on the lip angle, in degrees. 0 is a flat shelf, +90 a
// straight wall down, -90 a straight wall up; past either the band would travel
// back out of the part. Signed since 2026-09-15 (owner item 5) - see
// DrawCutParams::angle_deg for what the sign means and why it is not a toggle.
static constexpr double DrawCutMinLipAngleDeg = -90.0;
static constexpr double DrawCutMaxLipAngleDeg = 90.0;

// The skirt's own angle has the same range: it is the same tilt about the same
// axis, only applied to the outward piece.
static constexpr double DrawCutMinExtAngleDeg = -90.0;
static constexpr double DrawCutMaxExtAngleDeg = 90.0;

// The skirt's travel direction at a sample: the reverse of a band ruling built at
// the EXTENSION angle. With `params.extension_angle_deg` unset this is exactly
// -draw_cut_band_dir(inward, n, params.angle_deg), i.e. the band's own ruling run
// backwards, which is what the skirt has always been.
Vec3d draw_cut_skirt_dir(const Vec3d& inward, const Vec3d& normal, const DrawCutParams& params);

// The angle the skirt actually leaves at, resolving the "same as Angle" default.
double draw_cut_extension_angle(const DrawCutParams& params);

// ---------------------------------------------------------------------------
// SEPARATION OR PLUG: does the loop go ROUND the part, or sit ON it?
// ---------------------------------------------------------------------------

// True when the loop WRAPS the part - it encircles the whole object at the core
// plane rather than enclosing a patch of one face.
//
// THE TWO NEED DIFFERENT SOLIDS, and 2026-09-13's owner click-test is what made
// that true of the BAND-AND-CORE path as well as of Through all.
//
//   PLUG (loop on the skin)     the cutter is the plug itself - band wall, flat
//                               core cap, and the outer ring carried OUT of the
//                               part and capped beyond it (2026-09-16; a lid across
//                               the ring ran through any skin that bulged inside the
//                               loop). Intersect for the plug, complement for the
//                               rest. Two pieces, both real.
//   WRAP (loop round the part)  the band and core together span the WHOLE SECTION
//                               of the part at that height, so the same solid is a
//                               PLATE lying across the part rather than a plug in
//                               it. Its intersection with the mesh is a thin slab
//                               (the owner's "thin horizontal cyan ring with
//                               stair-stepped edges"), and its complement is one
//                               connected piece - so the part is NOT separated, and
//                               the panel said exactly that: "The stroke does not
//                               separate the part". What a line drawn all the way
//                               round means is a SEPARATION, so the surface has to
//                               be closed into a HALF-SPACE: band, core, and then
//                               straight on out of the part on the core's far side.
//
// THE SKIN DECIDES FIRST (2026-09-16): a loop that goes round a part is drawn on
// skin facing every way, so its sample normals CANCEL; a loop on one side of a part
// has normals that agree, and is a PLUG whatever the section says (a 19 mm loop on the
// side of the bunny's 20 mm head covers most of the head's own slice, and is still a
// patch). Only a loop whose normals cancel - mean under a quarter of a unit, the same
// guard build_core_band() pins the core's sign with - goes on to the section.
//
// THE TEST IS THEN THE SECTION, NOT THE BOUNDING BOX. Slice the mesh with the loop's
// plane, project the loop onto that plane, and ask, for each connected SLICE of the
// section, how much of it lies inside the loop:
//
//   some slice (at least a quarter of the loop's own area) has more than
//   `contain_frac` of its area inside the loop -> the loop goes round that slice of
//       the part, so it wraps it.
//   otherwise -> the loop lies within a slice, so it is a plug on the skin.
//
// PER SLICE and HALF, since 2026-09-16 (owner report: a belt round the bunny's neck
// was cut as a plug). The whole section is the wrong denominator - the loop's plane
// also slices whatever else it passes through, and none of that is inside a belt
// round the neck - and 90% is the ratio for a barrel, whose skin is square to the
// plane; a neck flares and a sphere widens, so a loop drawn on that skin projects
// INSIDE its slice (the neck belt covered 87% of its own slice).
//
// A bounding-box test cannot do this job, and was tried first: a cylinder's bbox
// corners stick out past its own radius, so "the part reaches outside the loop"
// fires on an ordinary plug loop drawn on the barrel and breaks it. The section is
// the actual material at that height, so a plug loop projects strictly inside it and
// a wrap-around loop strictly contains it - there is no case in between to tune,
// which is what makes this robust where the corner test was not.
//
// Returns false for anything that is not a valid closed loop, and for a mesh with no
// section at the core plane (a loop floating clear of the part), where "plug" is the
// safer answer because it is the non-destructive one.
bool draw_cut_loop_separates(const indexed_triangle_set& mesh,
                             const DrawCutStroke&        stroke,
                             const DrawCutParams&        params,
                             double                      contain_frac = 0.5);

// ---------------------------------------------------------------------------
// The cutter solid.
// ---------------------------------------------------------------------------

// Build the closed cutter solid for `stroke` under `params`, in the cut plane's
// frame. `bbox` is the object's bounding box in that frame, used for the
// through-all reach. `face_offset`, in mm, displaces the whole surface along its
// own local normal (t x d, the strip's own normal) - which is how the kerf's two
// solids are built: offsetting along a global Z would measure the band wrong
// wherever the strip is not vertical.
//
// The strip is triangulated as a quad grid between two rails, both of which lie
// on the ruling line through p_i along the cut direction d_i - the ruling is
// STRAIGHT at phase 1's angle of zero, so nothing here moves sideways:
//
//   out_i = p_i - E * d_i    (back OUT along the cut direction, so the surface
//                             starts clear of the face the stroke was drawn on -
//                             otherwise a stroke in a concavity has its rim
//                             buried in material and the boolean leaves a skin)
//   in_i  = p_i + D * d_i    (in by Depth, or through the bbox)
//
// E must NOT push the outward rail sideways along the binormal, tempting as the
// phrase "how far the surface reaches past the stroke" makes it: that turns every
// closed cut into a DRAFT (the strip then runs from radius r + E down to radius r,
// so a 12 mm circle at E = 5 cuts a truncated cone of 1.65x the intended volume
// instead of a cylinder). Phase 2's draft angle tilts d ITSELF toward the
// binormal, which is where the binormal earns its keep. For an OPEN stroke,
// "reaching past" is the TANGENT extension at the two ends, below.
//
// and then CLOSED, because a boolean needs a solid and a strip is an open
// surface:
//
//   closed stroke: the band is a tube. Cap the inner ring and the outer ring, so
//                  the solid's interior is "everything the plug occupies".
//   open stroke:   extend both ends by Extension along the stroke tangent, then
//                  close the two end quads and both caps. The extended ends are
//                  what let the surface reach past the silhouette.
//
// Returns an empty set when the stroke is not valid(), and also when a closed loop's
// Depth insets the core past its own centre - there is no flat left to build then,
// and returning nothing is what makes draw_cut_split() say so rather than silently
// emitting an inside-out core.
//
// `mesh`, when given, lets the builder ask draw_cut_loop_separates() whether the loop
// goes ROUND the part or sits ON it. For the BAND-AND-CORE surface the two need
// different solids - a plug versus a half-space - and getting it wrong is what made
// a wrap-around loop cut a thin slab out of the barrel instead of two halves (owner
// click-test, symptoms 2 and 3). THROUGH ALL no longer asks: a straight prism along
// the core normal already is the half-space for a wrap and the plug for a face loop.
//
// Passing nullptr keeps the PLUG reading, which is the right default for a loop on a
// face and the non-destructive one when the answer is not available.
indexed_triangle_set draw_cut_cutter_solid(const DrawCutStroke& stroke,
                                           const DrawCutParams& params,
                                           const BoundingBoxf3& bbox,
                                           double               face_offset = 0.0,
                                           const indexed_triangle_set* mesh = nullptr);

// ---------------------------------------------------------------------------
// THE PREVIEW SURFACE: what the GIZMO SHOWS, as opposed to what the boolean
// needs. 2026-09-16, owner report with a screenshot: after drawing a belt round
// the bunny the cut itself came out "exactly as expected", but the preview showed
// "a giant translucent curved wall/cylinder sweeping far outside the part" - the
// FLANGE-AND-WALL closure draw_cut_cutter_solid() adds to a wrap so the shape is a
// watertight half-space (see the "HALF-SPACE" comment above build_core_band's
// caller), rendered at 25% alpha over a part it dwarfs. The same is true of a
// plug's lid where it strays outside the loop, and would be true of an open
// stroke's sweep-to-a-far-boundary rim if that surface were ever shown edge-on.
//
// None of that closure is part of the surface the user asked to see: they drew a
// band round (or across, or over) the part and expect the preview to show exactly
// that band, plus the Extension/skirt that lifts its rim clear of the skin -
// nothing that only exists to give a boolean a watertight solid to intersect.
//
// This returns THAT surface - band + skirt (+ core plate for a closed loop,
// which is itself part of what the user drew, not a closure) - and nothing else:
// no plug lid past the loop's own footprint beyond what the band already covers,
// no wrap flange, no wrap wall, no bottom cap, and no open-stroke sweep rim. It is
// built by the SAME rails/rings draw_cut_cutter_solid() uses (so it is never a
// second, independently-tuned surface that could drift from the real cut), just
// stopped before the closure geometry is appended. Same arguments and the same
// empty-set contract as draw_cut_cutter_solid().
indexed_triangle_set draw_cut_preview_surface(const DrawCutStroke& stroke,
                                              const DrawCutParams& params,
                                              const BoundingBoxf3& bbox,
                                              double               face_offset = 0.0,
                                              const indexed_triangle_set* mesh = nullptr);

// ---------------------------------------------------------------------------
// THE DRAWN SURFACE AS A SURFACE. PHASE 2, and what connectors stand on.
//
// The curved cut's connectors ride on curved_cut_sheet_frame(): a rotation built
// from the sheet's own local normal, composed after the plane's m_rotation_m, so
// nothing in the connector path itself has to know about the sheet. The drawn
// surface needs the same three functions against the RULED STRIP instead of the
// height field.
//
// The strip's parameters are (s, w):
//   s - arc length along the stroke, in mm from the first path sample. For a
//       closed stroke it wraps at length().
//   w - the ruled parameter, in mm along the ruling from the stroke itself:
//       w == 0 is on the stroke, w > 0 is INTO the part, w < 0 is out of it. The
//       strip the cutter builds spans w in [-extension, +depth].
// ---------------------------------------------------------------------------

// The point of the ruled surface at (s, w), in the cut plane's frame.
Vec3d draw_cut_surface_point(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w);

// The strip's OWN unit normal at (s, w): normalize(t x d), the direction the two
// halves separate along. Sign-fixed the same way the cutter's `sweep` is, so it
// is continuous along the stroke and points consistently to one side.
//
// It is w-independent for a straight ruling, which is what the ruling is here -
// the parameter is taken so callers do not have to know that, and so a future
// twisted ruling would not change the signature.
Vec3d draw_cut_surface_normal(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w);

// A right-handed frame ON the drawn surface at (s, w), expressed in the cut
// plane's frame: local Z is the surface normal above, local X is the plane's own
// X projected onto the tangent plane (falling back to the plane's Y where that
// degenerates - the same construction and the same guard curved_cut_sheet_frame()
// uses), and `z_angle` spins the frame about its own Z the way a connector's
// Rotation does.
//
// A connector placed with this frame stands PERPENDICULAR TO THE CUT SURFACE, so
// its hole in one half and its plug in the other are coaxial by construction and
// survive the split - and, because both halves are cut by the same strip, they
// survive the kerf too (the kerf moves both faces along this same normal).
Transform3d draw_cut_surface_frame(const DrawCutStroke& stroke, const DrawCutParams& params,
                                   double s, double w, double z_angle = 0.0);

// The (s, w) of the point on the drawn surface CLOSEST to `p` (in the plane's
// frame), and the distance to it. This is the inverse of draw_cut_surface_point()
// and it is what turns a raycast hit on the cutter shell into surface parameters
// the frame can be built from.
//
// Returns false only for a stroke that is not valid().
bool draw_cut_surface_project(const DrawCutStroke& stroke, const DrawCutParams& params,
                              const Vec3d& p, double& s, double& w, double* distance = nullptr);

// True when (s, w) lies within the strip's own domain - the Draw analogue of the
// curved cut's (u,v)-in-contour test. `w` must be inside [-extension, +depth] with
// `margin` mm of room on each side (the connector's own radius, so a connector is
// not hung off the rim), and for an OPEN stroke `s` must likewise be `margin`
// clear of both ends. A closed stroke wraps, so `s` is never out of range there.
bool draw_cut_surface_contains(const DrawCutStroke& stroke, const DrawCutParams& params,
                               double s, double w, double margin, double depth_reach);

// The curvature radius of the strip ACROSS the rules at (s, w), in mm - i.e. how
// sharply the surface bends as you walk ALONG the stroke. Along the rules the
// strip is developable (a straight ruling has zero curvature that way), which is
// exactly why a straight-featured connector sits flatter here than on a dome:
// only one of the two directions can be curved at all.
//
// Infinity (a huge number) where the strip is locally flat.
double draw_cut_surface_curvature_radius(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w);

// The analogue of curved_cut_patch_is_flat_enough(): the patch under a connector
// of half-extent `extent` is flat enough when the cross-rule curvature radius is
// at least CurvedConnectorFlatPatchFactor times that extent. Same constant, same
// meaning, so the panel's wording does not have to change between the two modes.
bool draw_cut_patch_is_flat_enough(const DrawCutStroke& stroke, const DrawCutParams& params,
                                   double s, double w, double extent);

// The tilt of the drawn surface's normal away from the cut plane's own +Z, in
// degrees - the analogue of curved_cut_sheet_tilt_deg(). A connector standing on
// a steeply tilted patch prints at that angle, which is what
// CurvedConnectorTiltWarnDeg warns about.
double draw_cut_surface_tilt_deg(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w);

// True when the stroke's projection onto its own best-fit plane crosses itself.
// A self-crossing closed loop has no well-defined interior, so phase 1 refuses
// the cut rather than guessing which sub-loop the user meant.
//
// The test is a segment-intersection pass over the projected polyline, skipping
// adjacent segments (which share an endpoint by construction) and, for a closed
// stroke, the first-against-last pair for the same reason.
bool draw_cut_self_crossing(const DrawCutStroke& stroke);

// True when the ruled strip FOLDS: the rails cross because the stroke turns with
// a radius tighter than the ruling reaches sideways.
//
// TWO reaches matter, and phase 1 only knew about the first:
//
//  1. The OUTWARD rail, pushed back out of the face by Extension E along the
//     ruling. At angle 0 the ruling is the inward normal, so that push is
//     straight out of the surface and moves the rail NOWHERE sideways - which is
//     why phase 1's test is `E * kappa > 1` only where the turn centre is on the
//     outward side (a concave corner). At angle theta the ruling leans sideways by
//     sin(theta), so the outward rail's lateral offset is E * sin(theta) and the
//     inward rail's is D * sin(theta) the other way - and the INWARD one is the
//     dangerous one, because D is the depth and can be tens of millimetres.
//
//  2. So the reach to test is max(E * |sin theta|, D * |sin theta|) on the side
//     the lean goes, plus phase 1's E on the outward side. A concave stroke with
//     a large angle folds where the spec says it does.
//
// `depth` is the reach the cut will actually use, in mm (through-all's derived
// reach, or the Depth slider). `angle_deg` is the draft angle. A fold is a
// WARNING, not a refusal: Manifold tolerates a slightly self-intersecting cutter
// and mcut is the fallback, so a missed fold degrades to "boolean failed ->
// complement recovery" rather than to a wrong cut. `worst_kappa`, when non-null,
// receives the largest curvature found so the panel can say how tight.
bool draw_cut_strip_folds(const DrawCutStroke& stroke,
                          double               extension,
                          double*              worst_kappa = nullptr,
                          double               angle_deg = 0.0,
                          double               depth = 0.0);

// WHETHER THE CUT SURFACE FOLDS, asked of whichever surface the stroke actually
// gets. 2026-09-13, owner click-test symptom 4: on a gentle wavy loop round a
// cylinder with Depth 3, the panel said
//
//   "The line turns tighter than the cut surface reaches sideways, so the surface
//    folds there. Reduce the Angle, the Extension or the Depth."
//
// and it was wrong twice over.
//
//   - draw_cut_strip_folds() is a test about the RULED STRIP, which a closed loop
//     has not used since phase 3. A closed loop's band travels toward the loop's
//     own axis and stops on the core plane; it cannot fold against a tight corner
//     the way a ruling leaning along a binormal could. The gizmo nevertheless ran
//     the strip test on every stroke, closed ones included - draw_cut_split()
//     already skipped it for them, so the preview and the cut disagreed;
//   - and on a DENSE stroke the strip test does not measure the line's shape at
//     all. Discrete curvature from three consecutive samples is 4*area/(|ab||bc||ca|),
//     which on samples a fraction of a millimetre apart with a few hundredths of a
//     millimetre of raycast noise on them reads several 1/mm - the JITTER's
//     curvature, not the line's. Times a 5 mm extension that is comfortably over 1
//     and the warning fires on any hand-drawn line whatever its shape.
//
// So a closed loop is asked the question its own surface can actually fail: does
// the band's inward travel INVERT the core? The band moves every point
// `depth * cos(angle)` toward the loop's axis, so it folds when that inset reaches
// the loop's own in-plane radius - measured against a ROBUST radius (a low
// percentile rather than the single smallest sample, so one noisy point or the
// bottom of a 4 mm dent does not condemn a loop whose radius is 38 mm everywhere
// else). Through all has no inward travel at all and never folds.
//
// An OPEN stroke still gets draw_cut_strip_folds(), because it still is a ruled
// strip - but smoothed over a window, for the same jitter reason.
//
// `worst_inset_frac`, when non-null, receives how close the inset came as a
// fraction of that radius, so the panel can say how tight.
bool draw_cut_band_folds(const DrawCutStroke& stroke,
                         const DrawCutParams& params,
                         double*              worst_inset_frac = nullptr);

// ---------------------------------------------------------------------------
// The split.
// ---------------------------------------------------------------------------

// Which side of a drawn cut is "upper":
//
//   CLOSED stroke: the cutter encloses a PLUG, and `upper` is that plug (the
//     INTERSECTION) - "upper" is the piece the stroke drew around. The binormal
//     sign (see DrawCutStroke::binormal) makes this independent of drag
//     direction.
//   OPEN stroke: the strip cuts the part in two with no inside or outside, so
//     the plane's own convention applies - the half on the +Z side of the cut
//     plane frame is `upper`, matching flat and curved. The gizmo's Swap-sides /
//     right-click flip gesture covers the rest.
//
// cut_with_solid() already produces (inside, outside) as (lower, upper), so the
// CLOSED case is exactly the flip of that pair. draw_cut_split() does the flip,
// so its callers never have to know.
bool draw_cut_split(const indexed_triangle_set& mesh,
                    const DrawCutStroke&        stroke,
                    const DrawCutParams&        params,
                    indexed_triangle_set*       upper,
                    indexed_triangle_set*       lower,
                    DrawCutError*               err = nullptr);

// Which side a drawn cut would leave empty, cheaply, before running two
// booleans. The analogue of curved_cut_empty_sides(): the stroke's cutter is a
// solid, so the test is "is any vertex of the mesh inside it, and is any outside
// it" - done with a winding-number-free parity ray cast along +Z against the
// cutter's own triangles, which is cheap because the cutter is a few thousand
// faces and the answer short-circuits as soon as both sides have a vertex.
//
// `thickness` rides along exactly as it does for the curved test: with a kerf the
// sides are tested against the OFFSET cutters, so a side counts as empty when the
// kerf has eaten everything that was on it.
void draw_cut_empty_sides(const indexed_triangle_set& mesh,
                          const DrawCutStroke&        stroke,
                          const DrawCutParams&        params,
                          bool&                       upper_empty,
                          bool&                       lower_empty);

// ---------------------------------------------------------------------------
// THE CHAIN. 2026-09-12, from owner click-testing.
//
// Phase 1 and 2 modelled the line as ONE stroke: a press, a drag, a release, and
// whatever came out of that one gesture was the whole cut line. On a tall box you
// cannot draw all the way round without rotating the view, so the line could never
// be a loop round a large part - and because a partial stroke was lofted anyway,
// what the user got was a preview of a meaningless half-surface.
//
// A DrawCutChain is the line as a CHAIN OF STROKES instead. One chain exists at a
// time. Its samples are one ordered sequence with two ENDPOINTS; a new stroke is
// accepted only when it starts at one of them, and is appended at that end (in the
// order that keeps the chain's sequence continuous). The chain is CLOSED when a
// stroke's last sample comes within the snap radius of the FAR endpoint. Until it
// is closed there is no cut surface at all: draw_cut_split() is never called, the
// cutter shell is not built, and the only thing drawn is the polyline with its two
// endpoints marked.
//
// WHY A CHAIN AND NOT "JUST KEEP APPENDING". Two reasons the append-at-either-end
// rule earns:
//  - the user rotates the view between strokes, so the stroke that continues the
//    line can start at either end, and which end it is is not knowable in advance;
//  - a stroke whose first sample is at the FRONT endpoint runs AWAY from the chain,
//    so its samples have to be reversed before they are prepended, or the sequence
//    doubles back on itself and every tangent at the join is wrong.
//
// UNDO IS PER STROKE, which is why the chain remembers its stroke BOUNDARIES rather
// than only the flat sample list: Ctrl+Z takes back the last appended stroke, whole,
// including the closure it may have made.
// ---------------------------------------------------------------------------

// The snap radius, in mm: how near the far endpoint the cursor has to come for the
// chain to close. Scaled by the object's size, because a 2 mm radius that is
// comfortable on a 200 mm print is most of a 10 mm trinket - and clamped, because a
// radius bigger than the line is long would close every chain the moment it started.
//
//   r = clamp(ChainSnapFraction * bbox diagonal, ChainSnapMinMm, ChainSnapMaxMm)
//
// `bbox` is the object's bounding box in the cut plane's frame. An undefined bbox
// gives ChainSnapMinMm, which is the safe end: too small a radius means the user has
// to be accurate, too large means a chain closes behind their back.
static constexpr double ChainSnapFraction = 0.02;
static constexpr double ChainSnapMinMm    = 1.0;
static constexpr double ChainSnapMaxMm    = 6.0;
double draw_cut_chain_snap_radius(const BoundingBoxf3& bbox);

// Which end of the chain a new stroke starting at `p` continues, if either.
enum class DrawChainEnd {
    // Not within the snap radius of either endpoint: the stroke is DISJOINT and is
    // refused. (Owner feedback item 4: only one chain may exist, and a stroke that
    // does not continue it is rejected rather than silently replacing it.)
    None = 0,
    // At the chain's FIRST sample. The stroke runs away from the chain, so its
    // samples are reversed and prepended.
    Front,
    // At the chain's LAST sample. Appended as captured.
    Back
};

class DrawCutChain
{
public:
    DrawCutChain() = default;

    // --- state ------------------------------------------------------------
    bool   empty() const { return m_samples.empty(); }
    size_t size() const { return m_samples.size(); }
    // The raw samples of the WHOLE chain, in order. This is what the recipe stores
    // and what the stroke is finished from.
    const std::vector<DrawCutSample>& samples() const { return m_samples; }
    bool   is_closed() const { return m_closed; }
    // How many strokes have been appended. Each is one undo step.
    size_t stroke_count() const { return m_bounds.size(); }
    // [begin, end) of each appended stroke in samples(), newest last. The recipe
    // stores these so a reopened cut's Ctrl+Z takes back one stroke.
    const std::vector<std::pair<size_t, size_t>>& stroke_bounds() const { return m_bounds; }

    const Vec3d& front_pos() const { return m_samples.front().pos; }
    const Vec3d& back_pos() const { return m_samples.back().pos; }

    void clear();

    // --- building ---------------------------------------------------------
    // Which end `p` continues, given `snap_radius`. An EMPTY chain accepts anything
    // (returns Back), because the first stroke has nothing to continue. A CLOSED
    // chain accepts nothing: it is finished, and the caller either replaces it
    // wholesale or refuses.
    DrawChainEnd end_for_start(const Vec3d& p, double snap_radius) const;

    // Append `stroke` (raw captured samples, chronological) to the chain.
    //
    // `snap_radius` decides two things: whether the stroke's FIRST sample continues
    // the chain at all, and whether its LAST sample closes the chain on the far
    // endpoint. Returns the end it was appended at, or DrawChainEnd::None when the
    // stroke was REFUSED - which happens when the chain is closed, when the stroke
    // is disjoint from both endpoints, or when the stroke has fewer than two
    // samples.
    //
    // On a Front append the samples are REVERSED before prepending, so the chain's
    // sequence stays continuous.
    //
    // CLOSURE. The chain closes when the appended stroke's far-running end lands
    // within `snap_radius` of the chain's OTHER endpoint. The sample that closed it
    // is kept (it is on the model, it was drawn, and dropping it would leave a
    // visible notch); what is NOT done is snapping it exactly onto the other
    // endpoint, because the closing span is a real span the resampler walks - within
    // one snap radius of zero length, which on any real part is under the resample
    // spacing.
    //
    // A SINGLE stroke can close the chain on its own - a circle drawn in one gesture
    // is the phase 1 case and still works exactly as it did, which is why the closure
    // test runs for the first stroke too (against the chain's own first sample).
    DrawChainEnd append(const std::vector<DrawCutSample>& stroke, double snap_radius);

    // 2026-09-13, owner click-test item 1. APPEND AT A GIVEN END, with the end decided
    // by the CALLER rather than re-derived from `snap_radius`.
    //
    // append() does two jobs with one radius: it decides which end the stroke joins,
    // and it decides whether the stroke CLOSES the chain. The gizmo now decides the
    // first in SCREEN space on the press (a pixel radius around the projected endpoint
    // marker, which is what the user is actually aiming at - see
    // GLGizmoCut3D::draw_chain_end_at), so re-deciding it here from a 3D radius that
    // scales with the object would refuse exactly the strokes the screen pick exists to
    // rescue: a click dead-centre on the marker whose ray hit the mesh 4 mm away.
    //
    // The CLOSURE test still uses `snap_radius`, unchanged - that one is a question
    // about two points on the model, not about what the user can see.
    //
    // `at` must be Front or Back. On an EMPTY chain either is accepted and behaves as
    // the first stroke; None is refused. Everything else - the Front reversal, the
    // duplicate-join skip, the bounds bookkeeping, the closure - is append()'s, which
    // now forwards to this.
    DrawChainEnd append_at(const std::vector<DrawCutSample>& stroke, DrawChainEnd at, double snap_radius);

    // 2026-09-13, OWNER CLICK-TEST ITEM 2: CLOSE THE LOOP ALONG A GIVEN PATH.
    //
    // force_close() just sets m_closed, which leaves the closing span as the straight
    // CHORD between the two endpoints in the cut plane's frame. On a line drawn round
    // the outside of a part that chord runs straight THROUGH the material, and the
    // ruled surface swept along it comes out on the far side - which is what the owner
    // saw as "Close loop mirrors the line to the other side of the object".
    //
    // This takes the intermediate points of a path that lies ON the surface (the gizmo
    // raycasts them; libslic3r has no camera) and appends them to the BACK before
    // closing, so the closing span is a real path over the model rather than a chord
    // through it. `path` runs from the chain's BACK endpoint toward its FRONT and must
    // NOT repeat either endpoint - only what is between them. An empty path is exactly
    // force_close(), which is the right answer when the two ends are already within a
    // snap radius of each other: there is nothing between them to walk.
    //
    // The whole closure is ONE undo step, so the appended points are recorded as one
    // stroke and Ctrl+Z takes the chain back to the open line it was.
    bool close_along_path(const std::vector<DrawCutSample>& path);

    // Take back the last appended stroke, whole, including any closure it made.
    // Returns false when there is nothing to take back. A chain that ends up empty
    // is empty, not "one sample long".
    bool undo_last_stroke();

    // Force the chain closed even though its endpoints did not meet - the panel's
    // explicit "Close loop". Refused for a chain with fewer than MinChainSamples
    // samples, which is not a loop.
    bool force_close();

    // THE OPEN CUT, kept from phase 1 but now DELIBERATE.
    //
    // Phase 1 supported an open line right across a part: the strip cuts it in two with
    // no inside or outside, which is a perfectly good cut. The chain cannot loft that
    // automatically any more, because "the line is not finished yet" and "the line is
    // finished and is not a loop" look identical from the samples alone - and lofting
    // every partial line is exactly the wild-shape preview this change removes.
    //
    // So the USER says which: the panel's "Cut along the line" sets this, and a chain
    // with it set produces an OPEN stroke from finish() the way phase 1 did. Any append
    // clears it, because a chain that has just grown is one the user is still drawing.
    bool is_finished_open() const { return m_finished_open; }
    bool finish_open();

    // --- the stroke it makes ---------------------------------------------
    // Finish the chain's samples into a DrawCutStroke. This is the ONLY way a cut
    // surface is ever produced from a chain, and it produces NOTHING for an open
    // chain: `out` is left cleared with DrawCutError::NotClosed.
    //
    // That is owner feedback items 1 and 2 in one line - "until the chain is closed,
    // no cut surface is generated or previewed". The polyline is drawn from
    // samples() regardless, which is what the user needs to see while they work.
    DrawCutError finish(DrawCutStroke& out, double spacing = DrawCutStroke::DefaultSpacing,
                        double smoothing = 0.2) const;

    // A chain needs at least this many samples before it can be closed at all. Below
    // it a "closure" is a dab, not a loop.
    static constexpr size_t MinChainSamples = 6;

    // Cereal, for the recipe: the flat samples, the closed flag and the stroke
    // boundaries (so undo survives a round trip through the undo/redo stack).
    template<class Archive> void save(Archive& ar) const {
        ar(m_finished_open);
        ar(m_closed);
        ar(uint64_t(m_samples.size()));
        for (const DrawCutSample& s : m_samples)
            ar(s.pos, s.normal, uint64_t(s.facet));
        ar(uint64_t(m_bounds.size()));
        for (const std::pair<size_t, size_t>& b : m_bounds)
            ar(uint64_t(b.first), uint64_t(b.second));
    }
    template<class Archive> void load(Archive& ar) {
        uint64_t n = 0, nb = 0;
        ar(m_finished_open);
        ar(m_closed);
        ar(n);
        m_samples.clear();
        m_samples.resize(size_t(n));
        for (DrawCutSample& s : m_samples) {
            uint64_t facet = 0;
            ar(s.pos, s.normal, facet);
            s.facet = size_t(facet);
        }
        ar(nb);
        m_bounds.clear();
        m_bounds.reserve(size_t(nb));
        for (uint64_t i = 0; i < nb; ++ i) {
            uint64_t a = 0, b = 0;
            ar(a, b);
            m_bounds.emplace_back(size_t(a), size_t(b));
        }
    }

    bool operator==(const DrawCutChain& o) const;
    bool operator!=(const DrawCutChain& o) const { return !(*this == o); }

    // Rebuild a chain from stored samples (the recipe's loader). The whole thing
    // counts as ONE stroke for undo purposes, because the strokes it was originally
    // drawn in are not what a reopened cut is editing.
    void set_samples(const std::vector<DrawCutSample>& samples, bool closed, bool finished_open = false);

private:
    std::vector<DrawCutSample> m_samples;
    // [begin, end) of each appended stroke in m_samples, newest last. A FRONT append
    // shifts every earlier stroke's indices, which is why undo_last_stroke() works
    // off the recorded range rather than off a count.
    std::vector<std::pair<size_t, size_t>> m_bounds;
    bool m_closed{ false };
    // The user said "this line is finished and is not a loop". See finish_open().
    bool m_finished_open{ false };
};

// ---------------------------------------------------------------------------
// 2026-09-13, OWNER CLICK-TEST ITEM 1: THE ENDPOINT PICK IS IN SCREEN SPACE.
//
// The chain's endpoints are marked with handles the user clicks to carry the line on.
// Deciding which one a click grabbed used to be a 3D test - is the point the ray hit on
// the model within draw_cut_chain_snap_radius() mm of an endpoint - and that was wrong
// twice over:
//
//   - the radius is in OBJECT mm (a fraction of the bounding-box diagonal), so how
//     accurately the user has to click depends on the part's size and the camera's
//     distance. On a 200 mm part at a normal zoom the 4 mm radius is a handful of
//     pixels, so a click visually dead-centre on the handle misses and the stroke is
//     refused. That is the owner's "clicking on or near them does not continue the
//     chain";
//   - it needs a HIT on the mesh at all, so an endpoint on the FAR side of the part -
//     which the gizmo deliberately draws visible, through the model, because that is
//     where the next stroke has to start - could never be picked: the ray stops at the
//     near wall.
//
// A pixel radius around the PROJECTED handle fixes both: what the user can see, the
// user can hit. This is the pure geometry of that test, so it can be pinned by a unit
// test against a camera projection built by hand; the gizmo supplies the projection.
//
// `project` takes a point in the CHAIN's frame (the cut plane's frame) and returns its
// pixel position, or std::nullopt when the point does not project at all.
// `pick_px` is the radius in pixels.
//
// Returns Front or Back for the NEARER endpoint within the radius (both can be in range
// on a chain whose ends are close together on screen - the same tie-break
// DrawCutChain::end_for_start makes in 3D), and None when neither is.
//
// An EMPTY chain has no endpoints and gives None: "the first stroke may start anywhere"
// is the caller's rule, not this one's, because it is not a question about handles. A
// CLOSED chain has no free ends and also gives None.
DrawChainEnd draw_cut_chain_end_at_pixel(const DrawCutChain&                          chain,
                                         const Vec2d&                                       mouse_px,
                                         const std::function<std::optional<Vec2d>(const Vec3d&)>& project,
                                         double                                             pick_px);

// The distance from `p` to the polyline `pts`, in the points' own units - to its
// SEGMENTS, not only its vertices, and to the closing segment too when `closed`. A
// single point measures to that point; no points at all is infinity.
//
// 2026-09-16, owner: in Draw mode the plane grab that moves the whole drawn line
// "extends into infinity ... and makes it hard to rotate around the part". The gizmo
// now grabs only within a few pixels of the line, and this is the pure half of that
// test - the gizmo projects the chain's samples to pixels and asks this - so it can be
// pinned by a unit test without a camera.
double draw_cut_distance_to_polyline(const std::vector<Vec2d>& pts, bool closed, const Vec2d& p);

// ---------------------------------------------------------------------------
// THE HALVES CLASSIFICATION. 2026-09-12, owner feedback item 3.
//
// The cyan/magenta preview, the Visible/Ghost/Hidden side display and the
// connectors' notion of "which half am I in" all asked the FLAT PLANE which side a
// point was on, even in Draw mode - so the colours ran straight through the drawn
// surface and meant nothing.
//
// The curved cut answers this with a height field in a 2D texture, which a ruled
// strip cannot be (it is not single-valued over the plane - that is the whole point
// of the mode). So Draw answers it with the question the split itself asks: IS THE
// POINT INSIDE THE CUTTER SOLID. That is exact by construction - it is the same
// solid draw_cut_split() hands to the boolean - and it needs no assumption about the
// surface being a graph over anything.
//
// For the preview the answer has to be available PER FRAGMENT in a shader, so it is
// baked into a VOXEL FIELD over the object's bounding box: draw_cut_inside_field()
// below. sampler3D is core GL 1.2 / GLSL 110, so this works on the 2.1 fallback path
// too, unlike anything needing a compute pass.
// ---------------------------------------------------------------------------

// Which half of a drawn cut the point `p` (in the cut plane's frame) falls in.
// `true` means the UPPER half, using draw_cut_split()'s own convention: for a
// closed stroke the upper half is the PLUG (inside the cutter), for an open one it
// is the side the cutter does not contain.
//
// Exact, and the reference the voxel field below is tested against. One parity ray
// per call against the cutter's faces, so it is for connectors and for tests, not
// for a per-pixel loop.
bool draw_cut_classify_upper(const indexed_triangle_set& cutter,
                             bool                        closed,
                             const Vec3d&                p);

// The same question over a whole grid, as a scalar field a shader can sample.
//
// The field is +1 on the LOWER side and -1 on the UPPER side, matching the sign
// convention the colour-clip shader already uses (`side < 0` is side 1, the upper
// half - see GLVolumeCollection::set_color_clip_plane, which stores -normal). A
// caller that wants a smooth boundary can ask for `signed_distance`, which stores
// the distance to the cutter's surface with that sign instead - the sign is what
// the shader tests, so the two are interchangeable there, and the distance form is
// what makes the boundary land in the right place between two voxels rather than on
// a voxel face.
//
// `nx, ny, nz` are the grid dimensions; the grid spans `bbox` GROWN by one voxel on
// each side, so a fragment exactly on the part's surface is inside the field rather
// than on its clamped border. The layout is x-major within a row, rows within a
// slice: index = (k * ny + j) * nx + i.
//
// COST: one parity ray per COLUMN, not per voxel. For each (i, j) the ray along +Z
// is cast once, its crossings sorted, and the whole column filled from the
// crossing list - which is what makes a 64^3 field affordable on a mouse-up.
std::vector<float> draw_cut_inside_field(const indexed_triangle_set& cutter,
                                         bool                        closed,
                                         const BoundingBoxf3&        bbox,
                                         int                         nx,
                                         int                         ny,
                                         int                         nz,
                                         BoundingBoxf3*              field_bbox = nullptr);


} // namespace Slic3r

#endif /* slic3r_DrawCut_hpp_ */
