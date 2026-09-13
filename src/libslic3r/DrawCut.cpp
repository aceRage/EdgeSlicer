#include "DrawCut.hpp"
#include "CurvedCut.hpp"
#include "TriangleMeshSlicer.hpp"
#include "ClipperUtils.hpp"
#include "Polygon.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

const char* draw_cut_error_message(DrawCutError err)
{
    switch (err) {
    case DrawCutError::None:             return "";
    case DrawCutError::TooShort:         return "Draw a longer line on the model";
    case DrawCutError::SelfCrossing:     return "The line crosses itself";
    case DrawCutError::LeavesMesh:       return "The line leaves the model";
    case DrawCutError::CutterDegenerate: return "The line does not make a usable cut surface";
    case DrawCutError::EmptySide:        return "The stroke does not separate the part";
    case DrawCutError::NotClosed:        return "Carry on from an end of the line until it closes on the other";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Resample
// ---------------------------------------------------------------------------

static Vec3d safe_normalize(const Vec3d& v, const Vec3d& fallback)
{
    const double n = v.norm();
    return n > 1e-12 ? Vec3d(v / n) : fallback;
}

std::vector<DrawCutSample> draw_cut_resample(const std::vector<DrawCutSample>& in, double spacing, bool closed)
{
    std::vector<DrawCutSample> out;
    if (in.size() < 2 || spacing <= 0.0)
        return in;

    // Drop zero-length spans first: a captured stroke can hold two identical hits
    // (the mouse did not move between two motion events), and a zero span has no
    // direction to lerp along.
    std::vector<DrawCutSample> src;
    src.reserve(in.size() + 1);
    src.push_back(in.front());
    for (size_t i = 1; i < in.size(); ++ i)
        if ((in[i].pos - src.back().pos).norm() > 1e-12)
            src.push_back(in[i]);
    // A ring walks its closing span too, which is expressed by appending the first
    // sample - the output then drops that duplicate, because the strip builder
    // wraps and a duplicate would hand it a zero-length span.
    if (closed && (src.back().pos - src.front().pos).norm() > 1e-12)
        src.push_back(src.front());
    if (src.size() < 2)
        return in;

    // Cumulative arc length at each source sample, so the walk below is a plain
    // "where along the polyline is distance d" lookup rather than per-span
    // bookkeeping with a carry (which is exactly where an off-by-one hides).
    std::vector<double> acc(src.size(), 0.0);
    for (size_t i = 1; i < src.size(); ++ i)
        acc[i] = acc[i - 1] + (src[i].pos - src[i - 1].pos).norm();
    const double total = acc.back();
    if (total < 1e-12)
        return in;

    auto sample_at = [&src, &acc](double d, size_t& hint) {
        while (hint + 2 < src.size() && acc[hint + 1] < d)
            ++ hint;
        const double span = acc[hint + 1] - acc[hint];
        const double t    = span > 1e-12 ? std::clamp((d - acc[hint]) / span, 0.0, 1.0) : 0.0;
        DrawCutSample smp;
        smp.pos    = src[hint].pos + t * (src[hint + 1].pos - src[hint].pos);
        smp.normal = safe_normalize((1.0 - t) * src[hint].normal + t * src[hint + 1].normal, src[hint].normal);
        // The facet comes from whichever end is nearer, which is what a
        // re-derivation would give anyway at these spacings.
        smp.facet  = t < 0.5 ? src[hint].facet : src[hint + 1].facet;
        return smp;
    };

    size_t hint = 0;
    // Every emitted sample sits at an exact multiple of `spacing`, so the interior
    // spans are `spacing` long to floating-point precision - not to within an
    // accumulated drift.
    const size_t steps = size_t(std::floor(total / spacing));
    out.reserve(steps + 2);
    for (size_t k = 0; k <= steps; ++ k)
        out.push_back(sample_at(double(k) * spacing, hint));

    if (closed) {
        // The ring stays OPEN: drop a trailing sample that has come back onto the
        // first one, so the wrap span is a real span. (total is the full
        // circumference here, so out.back() is at most one spacing short of it.)
        while (out.size() > 3 && (out.back().pos - out.front().pos).norm() < 0.5 * spacing)
            out.pop_back();
    }
    else {
        // equally_spaced_points emits the first point but not reliably the last.
        // Fix that here rather than inheriting the wart: append the final input
        // sample, unless it lands within half a spacing of what is already there,
        // where appending it would leave a short span that the central-difference
        // tangent would then read as a near-zero direction.
        if ((out.back().pos - src.back().pos).norm() > 0.5 * spacing)
            out.push_back(src.back());
    }

    return out;
}

void draw_cut_smooth(std::vector<DrawCutSample>& path, int passes, bool closed, double strength)
{
    if (passes <= 0 || path.size() < 3)
        return;
    const double w = std::clamp(strength, 0.0, 1.0);
    if (w <= 0.0)
        return;

    const size_t n = path.size();
    std::vector<DrawCutSample> next(n);
    for (int pass = 0; pass < passes; ++ pass) {
        for (size_t i = 0; i < n; ++ i) {
            // An open stroke's endpoints are HELD: moving them would shorten the
            // stroke a little on every pass, and the ends are exactly where the
            // user decided the cut should reach.
            if (!closed && (i == 0 || i + 1 == n)) {
                next[i] = path[i];
                continue;
            }
            const DrawCutSample& prev = path[(i + n - 1) % n];
            const DrawCutSample& cur  = path[i];
            const DrawCutSample& nxt  = path[(i + 1) % n];
            next[i].pos    = (1.0 - w) * cur.pos + w * (0.5 * (prev.pos + nxt.pos));
            next[i].normal = safe_normalize((1.0 - w) * cur.normal + w * (0.5 * (prev.normal + nxt.normal)), cur.normal);
            next[i].facet  = cur.facet;
        }
        path.swap(next);
    }
}

int draw_cut_smooth_passes(double smoothing)
{
    const double s = std::clamp(smoothing, 0.0, 1.0);
    return int(std::lround(s * double(DrawCutStroke::MaxSmoothPasses)));
}

double draw_cut_closing_tolerance(double spacing)
{
    return std::max(3.0 * std::max(spacing, 0.0), 2.0);
}

// ---------------------------------------------------------------------------
// DrawCutStroke
// ---------------------------------------------------------------------------

void DrawCutStroke::append(const Vec3d& pos, const Vec3d& normal, size_t facet)
{
    DrawCutSample s;
    s.pos    = pos;
    s.normal = safe_normalize(normal, Vec3d::UnitZ());
    s.facet  = facet;
    m_samples.push_back(s);
}

void DrawCutStroke::clear()
{
    m_samples.clear();
    m_path.clear();
    m_binormal.clear();
    m_closed = false;
    m_error  = DrawCutError::TooShort;
}

DrawCutError DrawCutStroke::finish(double spacing, double smoothing, bool force_closed)
{
    m_path.clear();
    m_binormal.clear();
    m_closed = false;
    m_error  = DrawCutError::None;

    if (m_samples.size() < 2) {
        m_error = DrawCutError::TooShort;
        return m_error;
    }

    const double sp  = spacing > 0.0 ? spacing : DefaultSpacing;
    const double tol = draw_cut_closing_tolerance(sp);

    // THE JUMP TEST, before anything else. A miss during capture is skipped, not
    // fatal (crossing a hole or the silhouette must not end the stroke), but that
    // leaves the raw samples with a gap where the ray found nothing - and a gap
    // wider than a few spacings means the stroke jumped across empty space.
    // Bridging it would run a chord through air, so keep the LONGEST contiguous
    // run instead and let the length gate below decide whether what is left is
    // enough. The threshold is generous (8 spacings) because a fast flick through
    // a thin feature legitimately leaves a wide-ish gap that the interpolation
    // loop could not fill.
    const double jump = 8.0 * sp;
    std::vector<DrawCutSample> run;
    bool discarded_a_run = false;
    {
        size_t best_begin = 0, best_end = 0; // [begin, end)
        size_t begin = 0;
        double best_len = -1.0, cur_len = 0.0;
        for (size_t i = 1; i <= m_samples.size(); ++ i) {
            const bool  broke = i == m_samples.size() ||
                                (m_samples[i].pos - m_samples[i - 1].pos).norm() > jump;
            if (!broke) {
                cur_len += (m_samples[i].pos - m_samples[i - 1].pos).norm();
                continue;
            }
            if (cur_len > best_len) {
                best_len   = cur_len;
                best_begin = begin;
                best_end   = i;
            }
            begin   = i;
            cur_len = 0.0;
        }
        run.assign(m_samples.begin() + int(best_begin), m_samples.begin() + int(best_end));
        discarded_a_run = best_end - best_begin < m_samples.size();
        if (discarded_a_run)
            BOOST_LOG_TRIVIAL(warning) << "Draw cut: the stroke jumped across empty space; keeping the longest run ("
                                       << (best_end - best_begin) << " of " << m_samples.size() << " samples)";
    }
    // WHICH ERROR. When the repair kept a run and that run is usable, there is no
    // error - the whole point of keeping the longest run is that a stroke which
    // crossed a hole still works. But when what is left is too short, the REASON
    // the user needs is "your line left the model", not "draw a longer line":
    // their line was long enough, it just was not all on the part. So the
    // discarded-run flag decides which of the two gates reports.
    const DrawCutError short_error = discarded_a_run ? DrawCutError::LeavesMesh : DrawCutError::TooShort;
    if (run.size() < 2) {
        m_error = short_error;
        return m_error;
    }

    // Open or closed, decided on the RAW run - the resampler needs to know, since
    // a ring resamples its closing span too.
    const double gap = (run.back().pos - run.front().pos).norm();
    m_closed = force_closed || gap <= tol;
    // A "closed" stroke of three raw samples is not a loop, it is a wobble.
    if (m_closed && run.size() < 3)
        m_closed = false;

    m_path = draw_cut_resample(run, sp, m_closed);

    // Smoothing. A closed path wraps, an open one holds its ends.
    draw_cut_smooth(m_path, draw_cut_smooth_passes(smoothing), m_closed);

    if (m_path.size() < size_t(MinSamples) || length() < MinLength) {
        m_error = short_error;
        return m_error;
    }

    compute_binormals();

    if (draw_cut_self_crossing(*this)) {
        m_error = DrawCutError::SelfCrossing;
        return m_error;
    }

    return m_error;
}

double DrawCutStroke::length() const
{
    if (m_path.size() < 2)
        return 0.0;
    double len = 0.0;
    for (size_t i = 1; i < m_path.size(); ++ i)
        len += (m_path[i].pos - m_path[i - 1].pos).norm();
    if (m_closed)
        len += (m_path.front().pos - m_path.back().pos).norm();
    return len;
}

Vec3d DrawCutStroke::centroid() const
{
    if (m_path.empty())
        return Vec3d::Zero();
    Vec3d c = Vec3d::Zero();
    for (const DrawCutSample& s : m_path)
        c += s.pos;
    return c / double(m_path.size());
}

Vec3d DrawCutStroke::tangent(size_t i) const
{
    const size_t n = m_path.size();
    if (n < 2)
        return Vec3d::UnitX();
    if (m_closed) {
        const Vec3d d = m_path[(i + 1) % n].pos - m_path[(i + n - 1) % n].pos;
        return safe_normalize(d, Vec3d::UnitX());
    }
    // Open: one-sided at the ends, central in the middle.
    if (i == 0)
        return safe_normalize(m_path[1].pos - m_path[0].pos, Vec3d::UnitX());
    if (i + 1 >= n)
        return safe_normalize(m_path[n - 1].pos - m_path[n - 2].pos, Vec3d::UnitX());
    return safe_normalize(m_path[i + 1].pos - m_path[i - 1].pos, Vec3d::UnitX());
}

void DrawCutStroke::compute_binormals()
{
    const size_t n = m_path.size();
    m_binormal.assign(n, Vec3d::Zero());
    if (n == 0)
        return;

    const Vec3d c = centroid();

    for (size_t i = 0; i < n; ++ i) {
        const Vec3d t = tangent(i);
        const Vec3d nrm = m_path[i].normal;
        Vec3d b = t.cross(nrm);
        if (b.norm() < 1e-9) {
            // t parallel to n: the stroke runs along its own normal, which cannot
            // happen on a surface but can at a degenerate sample. Fall back to
            // "away from the centroid, in the tangent plane".
            Vec3d radial = m_path[i].pos - c;
            radial -= radial.dot(nrm) * nrm;
            b = radial;
        }
        m_binormal[i] = safe_normalize(b, Vec3d::UnitX());
    }

    if (m_closed) {
        // WINDING DECIDES, not drag direction. The outward binormal must point
        // away from the loop's interior, so flip the WHOLE field (not per sample -
        // that is what tears the strip) when the average radial agreement says it
        // points inward. Summing over every sample rather than testing one makes
        // the decision robust to a single sample near the centroid.
        double agree = 0.0;
        for (size_t i = 0; i < n; ++ i) {
            Vec3d radial = m_path[i].pos - c;
            const Vec3d& nrm = m_path[i].normal;
            radial -= radial.dot(nrm) * nrm; // in the surface's tangent plane
            agree += m_binormal[i].dot(radial);
        }
        if (agree < 0.0)
            for (Vec3d& b : m_binormal)
                b = -b;
    }
    else {
        // Open: no interior, so the sign is PARALLEL-TRANSPORTED from the first
        // sample - pick the branch that keeps b_i . b_{i-1} > 0. A pointwise
        // t x n flips across an inflection and would tear the strip in half.
        for (size_t i = 1; i < n; ++ i)
            if (m_binormal[i].dot(m_binormal[i - 1]) < 0.0)
                m_binormal[i] = -m_binormal[i];
    }
}

void DrawCutStroke::set_path(const std::vector<DrawCutSample>& path)
{
    // Same length or nothing: the open/closed decision and the error state were made
    // for a path of this size, and a caller that wants a different line has to go
    // back through finish().
    if (path.size() != m_path.size())
        return;
    m_path = path;
    for (DrawCutSample& s : m_path)
        s.normal = safe_normalize(s.normal, Vec3d::UnitZ());
    // The binormals are derived from the positions and the normals, so they are
    // stale the moment either changes.
    compute_binormals();
}

Vec3d DrawCutStroke::binormal(size_t i) const
{
    return i < m_binormal.size() ? m_binormal[i] : Vec3d::UnitX();
}

// ---------------------------------------------------------------------------
// Self-crossing
// ---------------------------------------------------------------------------

// Best-fit plane of the path, as (origin, two orthonormal in-plane axes). Newell's
// method for the normal: it is the area-weighted average of the face normals of
// the fan, which is stable for a nearly planar ring and degrades gracefully for a
// wobbly one.
static void stroke_plane(const DrawCutStroke& stroke, Vec3d& origin, Vec3d& ax, Vec3d& ay)
{
    const std::vector<DrawCutSample>& p = stroke.path();
    origin = stroke.centroid();

    Vec3d nrm = Vec3d::Zero();
    const size_t n = p.size();
    for (size_t i = 0; i < n; ++ i) {
        const Vec3d& a = p[i].pos;
        const Vec3d& b = p[(i + 1) % n].pos;
        nrm.x() += (a.y() - b.y()) * (a.z() + b.z());
        nrm.y() += (a.z() - b.z()) * (a.x() + b.x());
        nrm.z() += (a.x() - b.x()) * (a.y() + b.y());
    }
    if (nrm.norm() < 1e-12) {
        // A straight stroke has no enclosed area, so Newell gives nothing. Any
        // plane containing the stroke will do: take the average surface normal.
        nrm = Vec3d::Zero();
        for (const DrawCutSample& s : p)
            nrm += s.normal;
    }
    nrm = safe_normalize(nrm, Vec3d::UnitZ());

    // An in-plane basis. Pick the world axis least parallel to the normal so the
    // cross product is well conditioned.
    Vec3d seed = std::abs(nrm.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY();
    ax = safe_normalize(seed - seed.dot(nrm) * nrm, Vec3d::UnitX());
    ay = nrm.cross(ax);
}

// Do segments (a,b) and (c,d) properly cross, in 2D? Touching at an endpoint does
// not count - adjacent segments of a polyline always do that.
static bool segments_cross(const Vec2d& a, const Vec2d& b, const Vec2d& c, const Vec2d& d)
{
    auto cross = [](const Vec2d& u, const Vec2d& v) { return u.x() * v.y() - u.y() * v.x(); };
    const Vec2d r = b - a;
    const Vec2d s = d - c;
    const double denom = cross(r, s);
    if (std::abs(denom) < 1e-12)
        return false; // parallel or collinear: a collinear overlap is not a crossing
    const double t = cross(c - a, s) / denom;
    const double u = cross(c - a, r) / denom;
    const double eps = 1e-9;
    return t > eps && t < 1.0 - eps && u > eps && u < 1.0 - eps;
}

bool draw_cut_self_crossing(const DrawCutStroke& stroke)
{
    const std::vector<DrawCutSample>& p = stroke.path();
    const size_t n = p.size();
    if (n < 4)
        return false;

    Vec3d origin, ax, ay;
    stroke_plane(stroke, origin, ax, ay);

    std::vector<Vec2d> flat(n);
    for (size_t i = 0; i < n; ++ i) {
        const Vec3d d = p[i].pos - origin;
        flat[i] = Vec2d(d.dot(ax), d.dot(ay));
    }

    const size_t n_seg = stroke.is_closed() ? n : n - 1;
    for (size_t i = 0; i + 1 < n_seg; ++ i) {
        const Vec2d& a = flat[i];
        const Vec2d& b = flat[(i + 1) % n];
        for (size_t j = i + 2; j < n_seg; ++ j) {
            // Adjacent segments share an endpoint by construction; so do the first
            // and the last of a closed ring.
            if (stroke.is_closed() && i == 0 && j + 1 == n_seg)
                continue;
            const Vec2d& c = flat[j];
            const Vec2d& d = flat[(j + 1) % n];
            if (segments_cross(a, b, c, d))
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The cut direction, and the draft angle. PHASE 2.
// ---------------------------------------------------------------------------

Vec3d draw_cut_inward_dir(const DrawCutStroke& stroke, const DrawCutParams& params, size_t i)
{
    switch (params.direction) {
    case DrawCutDirection::View:  return safe_normalize(params.view_dir, -Vec3d::UnitZ());
    case DrawCutDirection::AxisX: return -Vec3d::UnitX();
    case DrawCutDirection::AxisY: return -Vec3d::UnitY();
    case DrawCutDirection::AxisZ: return -Vec3d::UnitZ();
    default: break;
    }

    if (i >= stroke.path().size())
        return -Vec3d::UnitZ();

    const Vec3d inward = -stroke.path()[i].normal;

    // THE DRAFT ANGLE. A rotation of the inward normal TOWARD THE OUTWARD BINORMAL,
    // in the plane those two span:
    //
    //   d = cos(theta) * (-n) + sin(theta) * b
    //
    // b is unit and perpendicular to n by construction (it is t x n with the sign
    // fixed), so d is unit without renormalising and the ruling stays a straight
    // line - which is what keeps the strip a RULED surface and keeps
    // draw_cut_surface_point() a lerp rather than an integration.
    //
    // theta == 0 returns -n bit for bit, which is phase 1's behaviour: the clamp
    // below and the multiply by sin(0) == 0 both vanish, and cos(0) == 1.
    const double theta = std::clamp(params.angle_deg, -DrawCutMaxAngleDeg, DrawCutMaxAngleDeg) * M_PI / 180.0;
    if (std::abs(theta) < 1e-12)
        return inward;

    const Vec3d b = stroke.binormal(i);
    // A degenerate binormal (t parallel to n) leaves nothing to tilt toward. Fall
    // back to the untilted ray rather than producing a direction that is not a
    // rotation of it.
    if (std::abs(b.dot(inward)) > 0.999)
        return inward;

    // POSITIVE theta LEANS THE RULING OUTWARD, away from the loop's interior, so
    // going IN along d the surface moves away from the stroke's outward side - the
    // plug widens with depth and lifts out. Negative leans it inward and the plug
    // narrows with depth, which is the undercut.
    return safe_normalize(std::cos(theta) * inward + std::sin(theta) * b, inward);
}

bool draw_cut_frame_holonomy_flips(const DrawCutStroke& stroke)
{
    if (!stroke.is_closed())
        return false; // an open stroke has no loop to come back round.

    const size_t n = stroke.path().size();
    if (n < 3)
        return false;

    // compute_binormals() orients a closed loop's field from the CENTROID rather
    // than by transport, which is what makes it winding-independent - but it also
    // means the field can be locally inconsistent where the loop is not star-shaped
    // about its centroid, or where it runs over a surface that turns the tangent
    // plane right over (a stroke round the waist of a twisted band). The symptom is
    // adjacent binormals pointing opposite ways, and a draft angle applied across
    // such a seam flares one way on one side of it and the other way on the other.
    //
    // That is the holonomy the spec asks about, measured where it can actually be
    // seen: a sign flip between neighbours, going all the way round including the
    // closing span.
    for (size_t i = 0; i < n; ++ i)
        if (stroke.binormal(i).dot(stroke.binormal((i + 1) % n)) < 0.0)
            return true;
    return false;
}

bool draw_cut_strip_folds(const DrawCutStroke& stroke, double extension, double* worst_kappa,
                          double angle_deg, double depth)
{
    const std::vector<DrawCutSample>& p = stroke.path();
    const size_t n = p.size();
    if (worst_kappa != nullptr)
        *worst_kappa = 0.0;

    // THE REACH THE FOLD TEST HAS TO USE, which is where phase 2 differs.
    //
    // At angle 0 the ruling is the inward NORMAL: pushing out by E along it moves
    // the rail straight out of the surface and not a millimetre sideways, so the
    // only lateral reach is E itself on the outward side, and phase 1's
    // `E * kappa > 1` is the whole story.
    //
    // At angle theta the ruling LEANS SIDEWAYS by sin(theta). The outward rail is
    // then E * |sin theta| to one side and the inward rail D * |sin theta| to the
    // other - and D is the DEPTH, which through-all makes the bounding-box
    // diagonal. That is the reach that folds a concave stroke at a large angle,
    // and it can be an order of magnitude larger than E.
    //
    // Only Surface normal tilts; the constant directions ignore the angle
    // (draw_cut_inward_dir does), so their reach is phase 1's.
    const double theta = std::clamp(angle_deg, -DrawCutMaxAngleDeg, DrawCutMaxAngleDeg) * M_PI / 180.0;
    const double lean  = std::abs(std::sin(theta));
    // E on the outward side (phase 1's reach, which does not depend on the lean -
    // the outward rail is pushed back by E along the ruling whichever way it points,
    // and at theta == 0 that is the ONLY reach there is), against D * sin(theta) on
    // the inward side. E's own lateral component is E * sin(theta) <= E, so it never
    // wins and does not need a term of its own.
    const double reach = std::max(std::max(0.0, extension), std::max(0.0, depth) * lean);

    if (n < 3 || reach <= 0.0)
        return false;

    double worst = 0.0;
    const size_t first = stroke.is_closed() ? 0 : 1;
    const size_t last  = stroke.is_closed() ? n : n - 1;
    for (size_t i = first; i < last; ++ i) {
        const Vec3d& a = p[(i + n - 1) % n].pos;
        const Vec3d& b = p[i % n].pos;
        const Vec3d& c = p[(i + 1) % n].pos;

        // Discrete curvature from the circumradius of the three points:
        // kappa = 4 * area / (|ab| |bc| |ca|).
        const double ab = (b - a).norm();
        const double bc = (c - b).norm();
        const double ca = (a - c).norm();
        if (ab < 1e-12 || bc < 1e-12 || ca < 1e-12)
            continue;
        const double area = 0.5 * (b - a).cross(c - a).norm();
        const double kappa = 4.0 * area / (ab * bc * ca);

        // THE SIGN MATTERS, and it is the opposite of the obvious guess. The
        // outward rail is the stroke pushed out by E along the binormal, so it
        // FOLDS where the stroke's own turn centre is on the OUTWARD side - a
        // CONCAVE corner as seen from outside - because there the outward offset
        // is walking toward a centre only 1/kappa away and overshoots it once
        // E * kappa > 1. Where the turn centre is on the INWARD side (a convex
        // corner, which is every point of a circle drawn as a loop) the outward
        // offset moves AWAY from the centre and the rail simply gets longer: it
        // cannot fold at all, however tight the curve.
        //
        // The turn vector (the change in unit tangent) points toward the turn
        // centre, so "the centre is on the outward side" is turn . b > 0... with
        // b the OUTWARD binormal, which for a closed loop points away from the
        // interior. A circle therefore scores zero here, which is correct.
        //
        // PHASE 2 ADDS THE OTHER SIGN, and it is the angle that puts it there. Once
        // theta is non-zero the INWARD rail leans the opposite way by D * sin(theta),
        // so a corner whose turn centre is on the INWARD side - a CONVEX corner, the
        // one phase 1 correctly ignored because nothing reached that way - now has a
        // rail walking toward a centre 1/kappa away with D * |sin theta| of reach to
        // do it in. A loop drawn round a small boss and drafted 30 degrees through a
        // thick part folds exactly there.
        //
        // So: the outward side is scored whenever there is any reach at all (E
        // always is), and the inward side only once the ruling leans.
        const Vec3d turn = (c - b).normalized() - (b - a).normalized();
        const double toward_out = turn.dot(stroke.binormal(i % n));
        if (toward_out > 0.0)
            worst = std::max(worst, kappa);
        else if (lean > 1e-9 && toward_out < 0.0)
            worst = std::max(worst, kappa);
    }

    if (worst_kappa != nullptr)
        *worst_kappa = worst;
    return reach * worst > 1.0;
}

// ---------------------------------------------------------------------------
// THE CORE PLANE. Phase 3.
// ---------------------------------------------------------------------------

bool draw_cut_core_plane(const DrawCutStroke& stroke,
                         const DrawCutParams& params,
                         Vec3d&               normal,
                         Vec3d&               centroid)
{
    normal   = Vec3d::UnitZ();
    centroid = Vec3d::Zero();

    if (!stroke.valid() || !stroke.is_closed())
        return false;

    const std::vector<DrawCutSample>& p = stroke.path();
    const size_t n = p.size();
    if (n < 3)
        return false;

    for (const DrawCutSample& s : p)
        centroid += s.pos;
    centroid /= double(n);

    // AN AXIS DIRECTION REPLACES THE FIT. The user asking for Axis Z is asking for
    // a mating face square to Z, which is exactly what a fitted normal would NOT
    // give on a hand-drawn wavy loop. The centroid stays the loop's own, so the
    // plane still sits in the middle of what was drawn.
    switch (params.direction) {
    case DrawCutDirection::AxisX: normal = Vec3d::UnitX(); return true;
    case DrawCutDirection::AxisY: normal = Vec3d::UnitY(); return true;
    case DrawCutDirection::AxisZ: normal = Vec3d::UnitZ(); return true;
    default: break;
    }

    // NEWELL, not a covariance eigenvector. Newell's method is the area-weighted
    // normal of the polygon, so a loop that wanders off its own plane (the wavy
    // loop round a cylinder the spec asks about) still yields the normal of the
    // plane that best carries its enclosed area, and a nearly-degenerate loop
    // degrades to a small vector rather than to an arbitrary axis.
    Vec3d nw = Vec3d::Zero();
    for (size_t i = 0; i < n; ++ i) {
        const Vec3d& a = p[i].pos;
        const Vec3d& b = p[(i + 1) % n].pos;
        nw.x() += (a.y() - b.y()) * (a.z() + b.z());
        nw.y() += (a.z() - b.z()) * (a.x() + b.x());
        nw.z() += (a.x() - b.x()) * (a.y() + b.y());
    }

    if (nw.norm() < 1e-9) {
        // A loop with no enclosed area to speak of. Fall back to the average of the
        // samples' own surface normals, which at least points off the skin.
        Vec3d avg = Vec3d::Zero();
        for (const DrawCutSample& s : p)
            avg += s.normal;
        normal = safe_normalize(avg, Vec3d::UnitZ());
        return true;
    }

    normal = nw.normalized();
    return true;
}

Vec3d draw_cut_core_inward(const DrawCutStroke& stroke,
                           const DrawCutParams& params,
                           const Vec3d&         normal,
                           const Vec3d&         centroid,
                           size_t               i)
{
    const std::vector<DrawCutSample>& p = stroke.path();
    if (i >= p.size())
        return -Vec3d::UnitZ();

    if (!stroke.is_closed()) {
        // No loop, no interior: the open stroke keeps the phase-1 ruling.
        return draw_cut_inward_dir(stroke, params, i);
    }

    // TOWARDS THE AXIS, which for a loop on a flat face is towards the centroid and
    // for a loop round a cylinder is the radial direction - the same formula says
    // both, because removing the component along n turns "towards the centre point"
    // into "towards the centre LINE".
    const Vec3d to_c = centroid - p[i].pos;
    Vec3d       in   = to_c - to_c.dot(normal) * normal;

    if (in.norm() < 1e-9) {
        // The sample sits on the axis itself (a loop pinched to a point there).
        // Use the in-plane part of the old inward normal so the band still has a
        // direction rather than collapsing.
        const Vec3d d = draw_cut_inward_dir(stroke, params, i);
        in = d - d.dot(normal) * normal;
        if (in.norm() < 1e-9)
            return -normal;
    }
    return in.normalized();
}

Vec3d draw_cut_band_dir(const Vec3d& inward, const Vec3d& normal, double angle_deg)
{
    const double a = std::clamp(angle_deg, DrawCutMinLipAngleDeg, DrawCutMaxLipAngleDeg) * M_PI / 180.0;
    // cos * inward + sin * (-n): at 0 the band is perpendicular to n (a flat
    // shelf), at 90 it runs straight along -n (a straight wall). inward and n are
    // perpendicular by construction, so the result is unit without renormalising.
    const Vec3d d = std::cos(a) * inward - std::sin(a) * normal;
    return safe_normalize(d, inward);
}

namespace {

// The band geometry of a closed loop, worked out once so the cutter, the surface
// queries and the tests cannot disagree about it.
struct CoreBand
{
    bool               ok{ false };
    Vec3d              normal{ Vec3d::UnitZ() };   // core plane normal n
    Vec3d              centroid{ Vec3d::Zero() };  // the LOOP's centroid
    // A point ON the core plane: the loop centroid moved along n to the depth the
    // band arrives at. This is the plane the core is flattened onto, and it is NOT
    // the plane through `centroid` - see build_core_band().
    Vec3d              plane_pt{ Vec3d::Zero() };
    std::vector<Vec3d> out;    // p + extension * outward   (outside the skin)
    std::vector<Vec3d> inner;  // the band's inner curve, ON the core plane
    std::vector<Vec3d> core;   // the core polygon (inner, but that IS the join)
    double             depth{ 0.0 };
};

// `depth_along` is the band's travel along d(p); `ext` the outward reach.
CoreBand build_core_band(const DrawCutStroke& stroke, const DrawCutParams& params,
                         double ext, double depth_along, double face_offset)
{
    CoreBand cb;
    if (!draw_cut_core_plane(stroke, params, cb.normal, cb.centroid))
        return cb;

    const std::vector<DrawCutSample>& p = stroke.path();
    const size_t n = p.size();
    cb.depth = depth_along;

    // THE NORMAL'S SIGN. Newell gives a normal tied to the winding, which flips
    // when the user drags the other way round. Everything downstream (which half is
    // "upper", which way the kerf moves) has to be winding-independent, so pin n to
    // the loop's own surface normals: the core normal should point the same way the
    // skin does on average, i.e. OUT of the part on the side the loop was drawn.
    // THE AVERAGE SKIN NORMAL ONLY DECIDES ANYTHING WHEN IT POINTS SOMEWHERE.
    //
    // On a loop drawn on a FLAT FACE every sample normal is the same, so the average
    // is that normal and "make n agree with it" is exactly right. On a loop round a
    // CYLINDER the normals are radial and the average cancels to ~zero, so its
    // direction is pure noise - and pinning to noise flips n arbitrarily, which sends
    // the band outward on half the runs (a core radius of 23 on a loop of radius 20,
    // instead of 17).
    //
    // So the pinning is applied only when the average is a meaningful fraction of the
    // samples it came from. When it is not - the barrel case - n's sign does not
    // matter anyway: the band direction at angle 0 is the in-plane inward direction,
    // which is winding-independent by construction, and `draw_cut_band_dir` tilts
    // towards -n symmetrically. The sign is then fixed downstream by the cutter's
    // its_volume() winding check.
    Vec3d avg_skin = Vec3d::Zero();
    for (const DrawCutSample& s : p)
        avg_skin += s.normal;
    if (avg_skin.norm() > 0.25 * double(n) && cb.normal.dot(avg_skin) < 0.0)
        cb.normal = -cb.normal;

    cb.out.reserve(n);
    cb.inner.reserve(n);

    for (size_t i = 0; i < n; ++ i) {
        const Vec3d inward = draw_cut_core_inward(stroke, params, cb.normal, cb.centroid, i);
        const Vec3d d      = draw_cut_band_dir(inward, cb.normal, params.angle_deg);

        // THE KERF. `face_offset` moves the whole surface along its own normal.
        // For the band that normal is d rotated into the plane the band and n span -
        // but the direction the two halves actually separate along is n for the core
        // and the band's own normal for the band. Using n for BOTH keeps the two
        // offset solids parallel where it matters (the mating faces) and is what
        // makes the kerf measure the same width on the core as on the lip.
        const Vec3d shift = face_offset * cb.normal;

        // OUTWARD is along the loop's own surface normal - that is what "reaches out
        // of the skin" means on a curved part. Falling back to -inward (radially
        // out) where the sample normal is useless keeps a band on a flat face sane.
        // OUTWARD, and the guard that has to point the other way to the obvious one.
        //
        // "Out of the skin" is the sample's own surface normal, and it is at its most
        // useful exactly when that normal is PARALLEL TO THE CORE NORMAL - a loop on
        // a flat top face, where out of the skin is straight up. An earlier version
        // treated that parallel case as degenerate and fell back to -inward, which
        // pushed the outer ring radially OUT instead of UP: the cutter stopped being
        // a plug, the boolean had nothing to intersect, and every closed loop on a
        // flat face failed with "the lower boolean gave nothing".
        //
        // The genuinely degenerate case is the opposite one: a normal lying IN the
        // core plane (perpendicular to n), which is what a loop round a cylinder's
        // barrel has. There the normal is the radial direction and is still the right
        // answer, so there is nothing to guard at all beyond a zero-length normal.
        Vec3d outward = p[i].normal;
        if (outward.norm() < 1e-9)
            outward = -inward;
        outward = safe_normalize(outward, -inward);

        cb.out.emplace_back(p[i].pos + ext * outward + shift);

        // The band travels `depth_along` along d, and then the surface TURNS ONTO
        // THE CORE PLANE. The arrival points are collected first; where the plane
        // actually sits is decided below, once they are all known.
        cb.inner.emplace_back(p[i].pos + depth_along * d + shift);
    }

    // WHERE THE CORE PLANE SITS, and the mistake that looks like the obvious answer.
    //
    // P has to pass through the points the band ARRIVES at, not through the loop's
    // own centroid. Projecting the arrivals onto the plane through cb.centroid - the
    // obvious reading of "project onto P" - UNDOES the band's travel along n: on a
    // loop drawn round a cube's top face with a 90 degree lip the band goes down by
    // Depth and is then snapped straight back up to the face, the plug collapses to
    // zero height, and the boolean reports "the lower boolean gave nothing".
    //
    // So the plane's offset is the MEAN arrival height along n, and the arrivals are
    // then flattened onto it. That mean is what makes the core flat for a loop that
    // is not planar: a wavy loop's arrivals sit at a spread of heights, and every one
    // of them is moved onto the single plane at their average - which is precisely
    // the "the band's inner curve lands on P by construction" the model asks for.
    {
        double mean_h = 0.0;
        for (const Vec3d& v : cb.inner)
            mean_h += (v - cb.centroid).dot(cb.normal);
        mean_h /= double(cb.inner.size());

        // The core plane's own point: the loop centroid moved along n to the band's
        // arrival depth. Everything downstream measures coplanarity against this.
        cb.plane_pt = cb.centroid + mean_h * cb.normal;

        for (Vec3d& v : cb.inner)
            v -= (v - cb.plane_pt).dot(cb.normal) * cb.normal;
    }

    // The core polygon IS the band's inner curve - they join along one closed curve
    // by construction, which is what keeps the solid watertight. The "inset by the
    // band's in-plane footprint" the spec describes is exactly what travelling
    // depth * cos(angle) inward already did.
    cb.core = cb.inner;
    cb.ok   = true;
    return cb;
}

} // namespace

// ---------------------------------------------------------------------------
// The band + core cutter solid. Phase 3, closed loops only.
//
// The solid is "everything the plug occupies": the band wall from the outer ring
// down to the inner ring, then either the FLAT CORE capping the inner ring (the
// normal case) or, for Through all, a straight extrusion of the band all the way
// out of the part on both sides.
//
// The outer ring is capped too - the plug has to be a closed solid, and the cap
// sits outside the skin where Extension put it, so it never shows in the result.
// ---------------------------------------------------------------------------
static indexed_triangle_set draw_cut_band_core_solid(const DrawCutStroke& stroke,
                                                     const DrawCutParams& params,
                                                     const BoundingBoxf3& bbox,
                                                     double               face_offset,
                                                     const indexed_triangle_set* mesh)
{
    indexed_triangle_set its;
    if (!stroke.valid() || !stroke.is_closed())
        return its;

    const double ext  = std::max(0.0, params.extension);
    const double diag = bbox.defined ? bbox.size().norm() : 100.0;

    // THROUGH ALL has no core: the band is extruded along d(p) far enough to leave
    // the part on BOTH sides, so the boolean always has a complete cut to work
    // with. The reach is the bbox diagonal with the usual 1.05 slack.
    const double reach = 1.05 * diag + 1.0;
    const double depth_along = params.through_all ? reach : std::max(0.01, params.depth);

    const CoreBand cb = build_core_band(stroke, params, ext, depth_along, face_offset);
    if (!cb.ok || cb.out.size() < 3)
        return its;

    const size_t m = cb.out.size();

    if (params.through_all) {
        // A TAPERED PLUG WITH NO FLAT MIDDLE.
        //
        // THE AXIS OF THE EXTRUSION IS THE CORE NORMAL, NOT d. Sweeping +-reach along
        // d(p) is the obvious reading of "extrude the band along d", and it is wrong:
        // d points mostly INWARD (towards the loop's axis), so at any useful reach
        // both rings converge past the axis and cross over, and the "solid" is a
        // self-intersecting bow tie rather than a plug. That is the same class of
        // mistake as the ruled strip's - a per-sample direction used as if it were a
        // sweep - and it is what made through-all "project through the object at
        // random angles".
        //
        // The extrusion runs along +-n, far enough to leave the part on both sides.
        // The ANGLE still does its job, because it tilts the WALL: at travel t along
        // n the ring is displaced sideways by t * tan(90 - angle) along the in-plane
        // inward direction, so angle 90 is a straight prism and smaller angles taper
        // it. tan is clamped so a near-zero angle cannot blow the radius up.
        // THE ANGLE'S MEANING HERE IS THE TAPER OF THE WALL, and its zero is a
        // STRAIGHT PRISM - not the flat shelf it is for the band.
        //
        // With a core plane, angle 0 means "the band lies in the plane parallel to
        // P", a flat shelf. With NO core plane there is no shelf to lie in: the cut
        // simply goes through, and the useful thing for the angle to say is how much
        // the through-cut tapers. So 0 is a straight prism along n (the plain through
        // cut every earlier version made, and what the phase-1 suite asserts), and
        // larger angles lean the wall in by tan(angle) per unit of travel.
        //
        // Reading it the other way round - cot(angle), to match the band - would make
        // angle 0 an infinite taper, which is how a plain through cut on a cube face
        // turned into a cone.
        const double a = std::clamp(params.angle_deg, DrawCutMinLipAngleDeg, DrawCutMaxLipAngleDeg)
                         * M_PI / 180.0;
        const double tan_a = std::abs(std::tan(a));

        const std::vector<DrawCutSample>& p = stroke.path();

        // THE SIDEWAYS TRAVEL MUST NOT REACH THE AXIS. `reach` is a whole bbox
        // diagonal, so any appreciable taper sends the inner ring through the centre
        // and out the other side, inverting it and making the solid self-intersect -
        // the very failure this branch exists to avoid. Cap the lateral displacement
        // at 90% of the loop's own smallest in-plane radius: as steep as the angle
        // asks for, right up to the point where it would eat itself.
        double min_r = std::numeric_limits<double>::max();
        for (size_t i = 0; i < m; ++ i) {
            const Vec3d to_c = cb.centroid - p[i].pos;
            min_r = std::min(min_r, (to_c - to_c.dot(cb.normal) * cb.normal).norm());
        }
        if (!(min_r < std::numeric_limits<double>::max()))
            min_r = 0.0;
        const double lateral = std::min(reach * tan_a, 0.9 * min_r);

        std::vector<Vec3d> ring_a, ring_b;
        ring_a.reserve(m);
        ring_b.reserve(m);
        // THE WALL MUST NOT SIT EXACTLY ON THE SKIN, and it must not be moved off it
        // by anything the user can see either.
        //
        // A loop drawn ROUND a box lies ON the box's surface, so a wall built straight
        // through those points is COINCIDENT with the box's own faces - the degenerate
        // case Manifold and mcut both give up on ("the upper boolean gave nothing").
        //
        // Extension is the wrong tool for that: it is measured in millimetres the user
        // chose, and any lateral use of it changes the CUT. On a loop round a barrel it
        // would widen the through cut; on Axis Z through a sloped face it turned a
        // radius-10 prism into a radius-13 one. What is needed is not a reach but a
        // NUDGE - enough to break the tie between two coincident planes, small enough
        // that no dimension moves. A thousandth of a millimetre does it, and it is far
        // below the tolerance any of these cuts are measured to. It has to be big
        // enough for the boolean to see two distinct planes rather than one, and
        // Manifold's own epsilon scales with the model, so it is taken as a tiny
        // fraction of the part rather than as an absolute: 1e-4 of the bbox diagonal
        // is 0.008 mm on an 80 mm box, invisible in any dimension and still four
        // orders of magnitude above float noise.
        //
        // (Extension still does its real job on the band path, where it lifts the
        // outer rim clear of the face the stroke was drawn on.)
        const double nudge = std::max(1e-3, 1e-4 * diag);

        // SEPARATION OR PLUG. A loop drawn ROUND the part has no inside-the-loop
        // piece: a prism through it contains the whole object, so the intersection
        // comes back as everything and the complement as nothing. See
        // draw_cut_loop_separates() for the section-versus-loop test that tells the
        // two apart, and for why the bounding box cannot.
        const bool separates = mesh != nullptr &&
                               draw_cut_loop_separates(*mesh, stroke, params);

        for (size_t i = 0; i < m; ++ i) {
            const Vec3d inward = draw_cut_core_inward(stroke, params, cb.normal, cb.centroid, i);
            const Vec3d shift  = face_offset * cb.normal - nudge * inward;
            if (separates) {
                // THE HALF-SPACE. The tapered wall runs from the drawn line along -n
                // ONLY, through the part and out the far side, and the cap beyond the
                // bbox closes it. The solid is therefore "everything below the wall",
                // so the two halves are the two sides of ONE wall rather than a plug
                // and a shell - which is what a line drawn all the way round a part
                // means.
                //
                // Ring A is the loop itself; ring B is the loop carried down by `reach`
                // with the taper applied, so the angle leans the wall exactly as it
                // does for a plug.
                ring_a.emplace_back(p[i].pos + shift);
                ring_b.emplace_back(p[i].pos - reach * cb.normal + lateral * inward + shift);
            }
            else {
                // Going along +n the wall leans OUT of the loop, going along -n it
                // leans IN - so the section tapers the way a plug's does, and each ring
                // stays on its own side of the loop.
                ring_a.emplace_back(p[i].pos + reach * cb.normal - lateral * inward + shift);
                ring_b.emplace_back(p[i].pos - reach * cb.normal + lateral * inward + shift);
            }
        }
        its.vertices.reserve(m * 2 + 2);
        for (const Vec3d& v : ring_a) its.vertices.emplace_back(v.cast<float>());
        for (const Vec3d& v : ring_b) its.vertices.emplace_back(v.cast<float>());

        auto A = [](size_t i) { return int(i); };
        auto B = [m](size_t i) { return int(m + i); };
        for (size_t i = 0; i < m; ++ i) {
            const size_t j = (i + 1) % m;
            its.indices.emplace_back(Vec3i32(A(i), B(i), B(j)));
            its.indices.emplace_back(Vec3i32(A(i), B(j), A(j)));
        }
        Vec3d ca = Vec3d::Zero(), cbc = Vec3d::Zero();
        for (size_t i = 0; i < m; ++ i) { ca += ring_a[i]; cbc += ring_b[i]; }
        ca /= double(m);
        cbc /= double(m);
        const int c_a = int(its.vertices.size());
        its.vertices.emplace_back(ca.cast<float>());
        const int c_b = int(its.vertices.size());
        its.vertices.emplace_back(cbc.cast<float>());
        for (size_t i = 0; i < m; ++ i) {
            const size_t j = (i + 1) % m;
            its.indices.emplace_back(Vec3i32(c_a, A(i), A(j)));
            its.indices.emplace_back(Vec3i32(c_b, B(j), B(i)));
        }
    }
    else {
        // BAND + FLAT CORE. Outer ring, inner ring (already on P), and the core
        // capped from its own centroid ON THE PLANE - so the cap is exactly planar,
        // which is what the halves mate on.
        its.vertices.reserve(m * 2 + 2);
        for (const Vec3d& v : cb.out)   its.vertices.emplace_back(v.cast<float>());
        for (const Vec3d& v : cb.inner) its.vertices.emplace_back(v.cast<float>());

        auto O = [](size_t i) { return int(i); };
        auto I = [m](size_t i) { return int(m + i); };

        // The band wall.
        for (size_t i = 0; i < m; ++ i) {
            const size_t j = (i + 1) % m;
            its.indices.emplace_back(Vec3i32(O(i), I(i), I(j)));
            its.indices.emplace_back(Vec3i32(O(i), I(j), O(j)));
        }

        // THE CORE. Its centroid is the mean of the inner ring, which lies on P
        // because every inner point does - so the fan is planar to floating point,
        // not just nearly so. That is what the coplanarity test measures.
        Vec3d core_c = Vec3d::Zero();
        for (const Vec3d& v : cb.inner)
            core_c += v;
        core_c /= double(m);
        // The fan centre must lie on the plane the INNER RING lies on, which is P
        // displaced by the band's travel - NOT the plane through the loop's own
        // centroid. Projecting onto the latter (an earlier version did) drags the cap
        // back up to the drawn line, so the plug collapses to nothing and the boolean
        // reports "the lower boolean gave nothing".
        //
        // The inner ring is planar by construction, so its mean is already on that
        // plane; re-projecting it onto the ring's own plane is the belt-and-braces
        // that cannot move it anywhere wrong.
        core_c -= (core_c - cb.plane_pt).dot(cb.normal) * cb.normal;

        // The outer cap, from the outer ring's own centroid. It sits outside the
        // skin, so it never survives the boolean - it only makes the solid closed.
        Vec3d out_c = Vec3d::Zero();
        for (const Vec3d& v : cb.out)
            out_c += v;
        out_c /= double(m);

        const int c_out  = int(its.vertices.size());
        its.vertices.emplace_back(out_c.cast<float>());
        const int c_core = int(its.vertices.size());
        its.vertices.emplace_back(core_c.cast<float>());

        for (size_t i = 0; i < m; ++ i) {
            const size_t j = (i + 1) % m;
            its.indices.emplace_back(Vec3i32(c_out,  O(i), O(j)));
            its.indices.emplace_back(Vec3i32(c_core, I(j), I(i)));
        }
    }

    // Same outward-winding guarantee the ruled strip gives cut_with_solid().
    if (its_volume(its) < 0.f)
        for (Vec3i32& t : its.indices)
            std::swap(t(1), t(2));

    return its;
}

bool draw_cut_loop_separates(const indexed_triangle_set& mesh,
                             const DrawCutStroke&        stroke,
                             const DrawCutParams&        params,
                             double                      contain_frac)
{
    if (mesh.empty() || !stroke.valid() || !stroke.is_closed())
        return false;

    Vec3d n, c;
    if (!draw_cut_core_plane(stroke, params, n, c))
        return false;

    // Pin n the way build_core_band() does, so "the core plane" means the same
    // plane here as it does where the cutter is built.
    {
        const std::vector<DrawCutSample>& p = stroke.path();
        Vec3d avg_skin = Vec3d::Zero();
        for (const DrawCutSample& s : p)
            avg_skin += s.normal;
        if (avg_skin.norm() > 0.25 * double(p.size()) && n.dot(avg_skin) < 0.0)
            n = -n;
    }

    // A FRAME WITH n AS +Z. slice_mesh() cuts at a constant z, so the mesh is
    // rotated into the core plane's own frame and sliced at z == 0. Building the
    // rotation from n alone leaves the in-plane axes free, which is fine: the test
    // is an area ratio and does not care how the plane is spun.
    Vec3d ax = std::abs(n.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitX();
    Vec3d ex = ax.cross(n);
    if (ex.norm() < 1e-9)
        return false;
    ex.normalize();
    const Vec3d ey = n.cross(ex);

    Matrix3d R;
    R.row(0) = ex;
    R.row(1) = ey;
    R.row(2) = n;

    indexed_triangle_set flat = mesh;
    for (Vec3f& v : flat.vertices)
        v = (R * (v.cast<double>() - c)).cast<float>();

    // THE SECTION at the core plane. Regular mode keeps holes as reversed contours,
    // so area() - which sums SIGNED areas - gives the net material at that height,
    // which is exactly the quantity the ratio below is a fraction of.
    const Polygons section = slice_mesh(flat, 0.0f, MeshSlicingParams{});
    const double   section_area = std::abs(area(section));
    if (section.empty() || section_area < EPSILON)
        return false; // nothing there: treat it as a plug, the non-destructive answer.

    // THE LOOP, projected onto the same plane and closed into one polygon.
    Polygon loop;
    loop.points.reserve(stroke.path().size());
    for (const DrawCutSample& s : stroke.path()) {
        const Vec3d q = R * (s.pos - c);
        loop.points.emplace_back(Point(coord_t(scale_(q.x())), coord_t(scale_(q.y()))));
    }
    if (loop.points.size() < 3)
        return false;
    // Clipper wants a consistent orientation; the area sign is the winding and the
    // loop's is whatever the user dragged.
    if (loop.area() < 0)
        loop.reverse();

    // HOW MUCH OF THE SECTION IS INSIDE THE LOOP.
    const Polygons inside = intersection(section, Polygons{ loop });
    const double   inside_area = std::abs(area(inside));

    // A wrap-around loop contains (nearly) all of the section; a plug loop contains
    // only the patch it was drawn around, which is a small part of it.
    return inside_area > contain_frac * section_area;
}

bool draw_cut_core_face(const DrawCutStroke& stroke,
                        const DrawCutParams& params,
                        const BoundingBoxf3& bbox,
                        Vec3d&               normal,
                        Vec3d&               point)
{
    normal = Vec3d::UnitZ();
    point  = Vec3d::Zero();

    if (!stroke.valid() || !stroke.is_closed())
        return false;
    // Through-all has no core: the band goes straight out of the part on both sides.
    if (params.through_all)
        return false;

    const double ext  = std::max(0.0, params.extension);
    const double dep  = std::max(0.01, params.depth);
    (void) bbox; // only through-all needs the bbox reach, and that returned above.

    const CoreBand cb = build_core_band(stroke, params, ext, dep, 0.0);
    if (!cb.ok)
        return false;

    normal = cb.normal;
    point  = cb.plane_pt;
    return true;
}

// ---------------------------------------------------------------------------
// The cutter solid
// ---------------------------------------------------------------------------

indexed_triangle_set draw_cut_cutter_solid(const DrawCutStroke& stroke,
                                           const DrawCutParams& params,
                                           const BoundingBoxf3& bbox,
                                           double               face_offset,
                                           const indexed_triangle_set* mesh)
{
    indexed_triangle_set its;
    if (!stroke.valid())
        return its;

    const std::vector<DrawCutSample>& p = stroke.path();
    const bool   closed = stroke.is_closed();
    const double ext    = std::max(0.0, params.extension);

    // Through-all reaches 1.05 * the bounding box diagonal, the same "reach past
    // the object" slack curved_cut_split() uses for its slab floor. Anything less
    // and a cut aimed diagonally through a long part stops inside it.
    const double diag  = bbox.defined ? bbox.size().norm() : 100.0;
    const double depth = params.through_all ? 1.05 * diag + 1.0 : std::max(0.01, params.depth);

    // ----------------------------------------------------------------------
    // PHASE 3: A CLOSED LOOP IS A BAND PLUS A FLAT CORE.
    //
    // See the model at the top of DrawCut.hpp. The ruled strip below is what an
    // OPEN stroke still uses (no loop, so no interior to put a core plane in) and
    // what a closed loop falls back to only if the plane fit fails outright.
    // ----------------------------------------------------------------------
    if (closed) {
        indexed_triangle_set band_its = draw_cut_band_core_solid(stroke, params, bbox, face_offset, mesh);
        if (!band_its.empty())
            return band_its;
        // Fall through to the ruled strip: better a rough cut than no cut.
    }

    // THE RAILS, and the one thing about them that is easy to get wrong.
    //
    // Each sample contributes two points - one outside the part, one inside it -
    // and the strip between them is the cut surface. The ruling is a STRAIGHT LINE
    // ALONG d_i, the cut direction, so both points lie on that line:
    //
    //   out_i = p_i - E * d_i     (back OUT along the cut direction, clear of the
    //                              surface: E doubles as how far above the face the
    //                              surface starts, so a stroke drawn in a concavity
    //                              has its rim in free air rather than buried in
    //                              material where the boolean would leave a skin)
    //   in_i  = p_i + D * d_i     (in by Depth, or through the bbox)
    //
    // What the outward point must NOT do is move sideways - out along the binormal.
    // That was the first version here, reading the spec's "E is how far the surface
    // reaches past the stroke" as a lateral push, and it silently turned every
    // closed cut into a DRAFT: the strip then runs from radius r + E at the top to
    // radius r at the bottom, so a 12 mm circle with 5 mm extension cut a truncated
    // cone of 1.65x the intended volume rather than a cylinder. At angle 0 - all of
    // phase 1 - there is no draft, so the ruling is straight along d and E only
    // ever measures ALONG that line. (Phase 2's draft angle is what tilts d itself,
    // toward the binormal, which is where the binormal earns its keep.)
    //
    // "Reaching past the stroke" for an OPEN stroke is the TANGENT extension at the
    // two ends - and that is what lets the surface get past the silhouette so the
    // boolean actually separates the part.
    struct Rail { Vec3d out, in; };
    std::vector<Rail> rails;
    const size_t n = p.size();
    rails.reserve(n + 2);

    // THE KERF OFFSET AND ITS SIGN.
    //
    // `face_offset` comes from curved_cut_thickness_faces(), which hands back
    // lo <= 0 <= hi along "the cut normal". Here that normal is the STRIP's own -
    // t x d, so the band is measured along the local frame rather than along a
    // global Z, which is the rule phase 4 established for connectors and which
    // matters wherever the strip is not vertical.
    //
    // The SIGN has to be pinned to which side the cutter's interior is on, or the
    // kerf grows the halves instead of shrinking them. t x d has an arbitrary
    // per-sample sign (it flips with the drag direction and across an inflection),
    // so it cannot be used raw:
    //
    //   OPEN stroke: the cutter is the drawn surface swept along +sweep, so its
    //     interior is on the +sweep side and the surface must move by -offset *
    //     sweep - i.e. the INTERSECTION side (lo, negative) shrinks, and so does
    //     the A_NOT_B side (hi, positive).
    //   CLOSED stroke: the cutter is a tube whose interior is the plug, so the wall
    //     must move toward the plug's axis - along -binormal, the binormal being the
    //     direction that points away from the loop's interior by construction.
    //
    // Getting this backwards is invisible without a kerf and shows up with one as
    // "the two halves together are BIGGER than the part", which is what the
    // volume-difference test measures.
    //
    // `sweep` is also what the open case's slab is built along, below, so it is
    // derived here once for both jobs. It is ONE direction for the whole strip, not
    // the per-sample strip normal: a per-sample sweep of a curved stroke folds
    // wherever the stroke turns and the resulting solid self-intersects.
    Vec3d sweep = Vec3d::Zero();
    for (size_t i = 0; i < n; ++ i) {
        const Vec3d d  = draw_cut_inward_dir(stroke, params, i);
        Vec3d       sn = stroke.tangent(i).cross(d);
        if (sn.norm() < 1e-9)
            continue;
        sn.normalize();
        // Keep the sum coherent: flip a sample whose normal opposes the running
        // total, or a stroke that doubles back would cancel itself to nothing.
        if (!sweep.isZero() && sn.dot(sweep) < 0.0)
            sn = -sn;
        sweep += sn;
    }
    sweep = safe_normalize(sweep, Vec3d::UnitY());

    auto push_rail = [&](const Vec3d& pos, const Vec3d& t, const Vec3d& d, const Vec3d& b) {
        // Away from the cutter's interior, so a POSITIVE offset always grows the
        // cutter and a negative one always shrinks it - see above.
        const Vec3d out_of_cutter = closed ? Vec3d(-b) : Vec3d(-sweep);
        const Vec3d shift = face_offset * out_of_cutter;
        Rail r;
        r.out = pos - ext * d + shift;
        r.in  = pos + depth * d + shift;
        rails.push_back(r);
    };

    if (!closed) {
        // Leading end, extended BACKWARDS along the tangent so the surface reaches
        // past the silhouette on that side.
        const Vec3d t0 = stroke.tangent(0);
        push_rail(p.front().pos - ext * t0, t0, draw_cut_inward_dir(stroke, params, 0), stroke.binormal(0));
    }

    for (size_t i = 0; i < n; ++ i)
        push_rail(p[i].pos, stroke.tangent(i), draw_cut_inward_dir(stroke, params, i), stroke.binormal(i));

    if (!closed) {
        const Vec3d tN = stroke.tangent(n - 1);
        push_rail(p.back().pos + ext * tN, tN, draw_cut_inward_dir(stroke, params, n - 1), stroke.binormal(n - 1));
    }

    const size_t m = rails.size();
    if (m < 3)
        return its;

    // ----------------------------------------------------------------------
    // FROM A STRIP TO A SOLID, and the trap that is easy to walk into twice.
    //
    // A boolean needs a solid. The strip between the two rails is a SURFACE, and
    // for a CLOSED stroke that is already enough: the band is a tube, so capping
    // its two boundary rings gives a solid with a real interior - the plug.
    //
    // For an OPEN stroke it is not enough, and "close the one boundary loop with a
    // fan" - the obvious repair - produces a solid of ZERO VOLUME: a flattened
    // bag, watertight by every edge count and empty inside. The INTERSECTION then
    // comes back with nothing and the caller sees one half, which is the "an open
    // line across a box gave back 1.5x the box" symptom.
    //
    // The fix is the one curved_cut_lower_slab() already uses for the sheet: sweep
    // the surface sideways to a FAR BOUNDARY well outside the part, so the solid is
    // "everything on one side of the drawn surface". Here the sweep is along the
    // strip's own normal (t x d), and the reach is the bbox diagonal with the same
    // 1.05 slack. Intersecting with that gives one whole half of the part; A_NOT_B
    // gives the other.
    // ----------------------------------------------------------------------

    if (closed) {
        // Vertices: the outward ring then the inward ring, so index arithmetic is
        // trivial - the same layout curved_cut_lower_slab() uses for its two grids.
        its.vertices.reserve(m * 2 + 2);
        for (const Rail& r : rails)
            its.vertices.emplace_back(r.out.cast<float>());
        for (const Rail& r : rails)
            its.vertices.emplace_back(r.in.cast<float>());

        auto O = [](size_t i) { return int(i); };
        auto I = [m](size_t i) { return int(m + i); };

        // The tube wall: a quad grid between the two rails, two triangles per span,
        // exactly the way curved_cut_lower_slab() stitches its rim.
        for (size_t i = 0; i < m; ++ i) {
            const size_t j = (i + 1) % m;
            its.indices.emplace_back(Vec3i32(O(i), I(i), I(j)));
            its.indices.emplace_back(Vec3i32(O(i), I(j), O(j)));
        }

        // Cap the two boundary rings from their own centroids, which lie inside them
        // for any stroke that is not self-crossing - and a self-crossing one is
        // refused before it gets here. The wall walks the out ring as O(j) -> O(i)
        // and the in ring as I(i) -> I(j), so each cap has to use its ring the other
        // way round or the shared edge ends up traversed twice the same way (which
        // every edge count still calls watertight, and which Manifold rejects).
        Vec3d co = Vec3d::Zero(), ci = Vec3d::Zero();
        for (const Rail& r : rails) { co += r.out; ci += r.in; }
        co /= double(m);
        ci /= double(m);
        const int c_out = int(its.vertices.size());
        its.vertices.emplace_back(co.cast<float>());
        const int c_in = int(its.vertices.size());
        its.vertices.emplace_back(ci.cast<float>());
        for (size_t i = 0; i < m; ++ i) {
            const size_t j = (i + 1) % m;
            its.indices.emplace_back(Vec3i32(c_out, O(i), O(j)));
            its.indices.emplace_back(Vec3i32(c_in,  I(j), I(i)));
        }
    }
    else {
        // The sweep distance: far enough to leave the part on the swept side, so the
        // solid really is a half-space as far as this object is concerned.
        const double reach = 1.05 * diag + 1.0;

        // `sweep` was derived above, where the kerf offset's sign needed it too.
        const Vec3d shift = reach * sweep;

        // Two copies of the strip - the drawn one and the swept one - and the rim
        // between them, which is the same two-grids-plus-rim construction
        // curved_cut_lower_slab() uses. 4m vertices: out, in, out+shift, in+shift.
        its.vertices.reserve(m * 4);
        for (const Rail& r : rails) its.vertices.emplace_back(r.out.cast<float>());
        for (const Rail& r : rails) its.vertices.emplace_back(r.in.cast<float>());
        for (const Rail& r : rails) its.vertices.emplace_back(Vec3f((r.out + shift).cast<float>()));
        for (const Rail& r : rails) its.vertices.emplace_back(Vec3f((r.in  + shift).cast<float>()));

        auto A = [](size_t i) { return int(i); };            // out, drawn face
        auto B = [m](size_t i) { return int(m + i); };       // in,  drawn face
        auto A2 = [m](size_t i) { return int(2 * m + i); };  // out, swept face
        auto B2 = [m](size_t i) { return int(3 * m + i); };  // in,  swept face

        // The drawn face (the cut surface itself) and the swept face, wound
        // oppositely so the solid between them is the interior.
        for (size_t i = 0; i + 1 < m; ++ i) {
            const size_t j = i + 1;
            its.indices.emplace_back(Vec3i32(A(i), B(i), B(j)));
            its.indices.emplace_back(Vec3i32(A(i), B(j), A(j)));
            its.indices.emplace_back(Vec3i32(A2(i), B2(j), B2(i)));
            its.indices.emplace_back(Vec3i32(A2(i), A2(j), B2(j)));
        }
        // The rim, on all four edges of the strip: the out curve, the in curve, and
        // the two end segments.
        for (size_t i = 0; i + 1 < m; ++ i) {
            const size_t j = i + 1;
            // out curve
            its.indices.emplace_back(Vec3i32(A(i), A(j), A2(j)));
            its.indices.emplace_back(Vec3i32(A(i), A2(j), A2(i)));
            // in curve
            its.indices.emplace_back(Vec3i32(B(i), B2(i), B2(j)));
            its.indices.emplace_back(Vec3i32(B(i), B2(j), B(j)));
        }
        // The two ends.
        its.indices.emplace_back(Vec3i32(A(0), A2(0), B2(0)));
        its.indices.emplace_back(Vec3i32(A(0), B2(0), B(0)));
        its.indices.emplace_back(Vec3i32(A(m - 1), B(m - 1), B2(m - 1)));
        its.indices.emplace_back(Vec3i32(A(m - 1), B2(m - 1), A2(m - 1)));
    }

    // WINDING. The solid above is built consistently, but whether it comes out
    // wound OUTWARDS depends on the stroke's own handedness - and cut_with_solid()
    // decides "inside" from face orientation, so a solid wound inwards would have
    // its INTERSECTION come back as the complement. its_volume() is the same test
    // cut_with_solid() applies to the object; apply it to the cutter here so the
    // cutter handed over is always outward-wound, whichever way the user dragged.
    if (its_volume(its) < 0.f)
        for (Vec3i32& t : its.indices)
            std::swap(t(1), t(2));

    return its;
}

// ---------------------------------------------------------------------------
// The drawn surface as a surface: (s, w) -> point, normal, frame. PHASE 2.
//
// What connectors stand on. The curved cut's connector frame comes from the
// sheet's local normal; the drawn cut's comes from the ruled strip's, and the
// only real work is turning an arc length s into "which span, how far along it".
// ---------------------------------------------------------------------------

namespace {

// Cumulative arc length of the finished path, span by span. For a CLOSED stroke
// the closing span (last -> first) is included as the final entry, so the table
// has n + 1 entries and back() is the full circumference; for an open one it has
// n entries and back() is the length.
std::vector<double> path_arc_table(const DrawCutStroke& stroke)
{
    const std::vector<DrawCutSample>& p = stroke.path();
    const size_t n = p.size();
    std::vector<double> acc;
    if (n == 0)
        return acc;
    acc.reserve(n + 1);
    acc.push_back(0.0);
    for (size_t i = 1; i < n; ++ i)
        acc.push_back(acc.back() + (p[i].pos - p[i - 1].pos).norm());
    if (stroke.is_closed())
        acc.push_back(acc.back() + (p[0].pos - p[n - 1].pos).norm());
    return acc;
}

// Locate arc length `s` in the table: the span index and the 0..1 parameter along
// it. A closed stroke WRAPS (s is taken modulo the circumference); an open one
// CLAMPS to its two ends, so a connector dragged past the end of the line stays on
// the last span rather than vanishing.
void locate_arc(const DrawCutStroke& stroke, const std::vector<double>& acc, double s,
                size_t& span, double& t)
{
    const size_t n = stroke.path().size();
    span = 0;
    t    = 0.0;
    if (acc.size() < 2 || n < 2)
        return;

    const double total = acc.back();
    if (total < 1e-12)
        return;

    double d = s;
    if (stroke.is_closed()) {
        d = std::fmod(d, total);
        if (d < 0.0)
            d += total;
    }
    else
        d = std::clamp(d, 0.0, total);

    // The last table entry is the END of the last span, so the search stops one
    // short of it.
    size_t i = 0;
    while (i + 2 < acc.size() && acc[i + 1] <= d)
        ++ i;
    const double span_len = acc[i + 1] - acc[i];
    span = i;
    t    = span_len > 1e-12 ? std::clamp((d - acc[i]) / span_len, 0.0, 1.0) : 0.0;
}

// The stroke point, its ruling direction and its tangent at arc length s, all
// lerped across the span s falls in. The ruling is lerped AS A DIRECTION and
// renormalised, which is what keeps the surface continuous where the underlying
// normals turn.
// THE SURFACE'S TRAVEL DIRECTION AT ARC LENGTH s.
//
// Phase 3: for a CLOSED loop this is the BAND direction d(p) - the in-plane
// inward direction tilted by the lip angle towards the core normal - because that
// is what the cutter sweeps and so what a connector has to stand on. For an OPEN
// stroke it stays the phase-1 inward ruling.
//
// Keeping this in one place is what makes draw_cut_surface_point() agree with
// draw_cut_cutter_solid() sample for sample; the test that walks the surface and
// checks the cutter's own vertices lie on it is the thing that would catch a
// divergence.
void surface_frame_pieces(const DrawCutStroke& stroke, const DrawCutParams& params, double s,
                          Vec3d& pos, Vec3d& dir, Vec3d& tan)
{
    const std::vector<DrawCutSample>& p = stroke.path();
    const size_t n = p.size();
    pos = Vec3d::Zero();
    dir = -Vec3d::UnitZ();
    tan = Vec3d::UnitX();
    if (n == 0)
        return;

    // The band direction at sample index i, closed loops only.
    Vec3d core_n, core_c;
    const bool has_core = stroke.is_closed() &&
                          draw_cut_core_plane(stroke, params, core_n, core_c);
    if (has_core) {
        // Pin the normal the same way build_core_band() does, or the band direction
        // here would lean the opposite way to the one the cutter used.
        // The same threshold build_core_band() uses, and for the same reason: on a
        // barrel loop the average skin normal cancels and its direction is noise.
        Vec3d avg_skin = Vec3d::Zero();
        for (const DrawCutSample& s2 : p)
            avg_skin += s2.normal;
        if (avg_skin.norm() > 0.25 * double(p.size()) && core_n.dot(avg_skin) < 0.0)
            core_n = -core_n;
    }
    auto dir_at = [&](size_t i) -> Vec3d {
        if (!has_core)
            return draw_cut_inward_dir(stroke, params, i);
        const Vec3d inward = draw_cut_core_inward(stroke, params, core_n, core_c, i);
        if (params.through_all) {
            // THROUGH-ALL SWEEPS ALONG -n, NOT ALONG THE BAND DIRECTION - see
            // draw_cut_band_core_solid(), where using d as a sweep sends the rings
            // across the axis. The surface queries must follow the SAME ruling the
            // cutter builds, or a connector placed at w lands where the cut never
            // went: at angle 0 the band direction is horizontal, so w = 8 put the
            // connector 8 mm radially INSIDE the plug instead of 8 mm down its wall,
            // and the hole and the plug stopped matching.
            //
            // The wall leans sideways by tan(angle) per unit travelled down, which is
            // exactly the taper the cutter applies.
            const double a = std::clamp(params.angle_deg, DrawCutMinLipAngleDeg,
                                        DrawCutMaxLipAngleDeg) * M_PI / 180.0;
            return safe_normalize(-core_n + std::tan(a) * inward, -core_n);
        }
        return draw_cut_band_dir(inward, core_n, params.angle_deg);
    };

    if (n == 1) {
        pos = p[0].pos;
        dir = dir_at(0);
        tan = stroke.tangent(0);
        return;
    }

    const std::vector<double> acc = path_arc_table(stroke);
    size_t span = 0;
    double t = 0.0;
    locate_arc(stroke, acc, s, span, t);

    const size_t i = span % n;
    const size_t j = (span + 1) % n;

    pos = (1.0 - t) * p[i].pos + t * p[j].pos;
    dir = safe_normalize((1.0 - t) * dir_at(i) + t * dir_at(j), dir_at(i));
    tan = safe_normalize((1.0 - t) * stroke.tangent(i) + t * stroke.tangent(j), stroke.tangent(i));
}

// THE STRIP'S NORMAL, with its sign pinned the way the cutter's `sweep` pins it.
//
// t x d has an arbitrary per-sample sign - it flips with the drag direction and
// across an inflection - so it cannot be used raw for a frame a connector stands
// on: two connectors a few millimetres apart would point opposite ways. The
// cutter solid already solved this by summing a coherent field over the whole
// stroke and using ONE direction; the same sum is the reference here, so the
// connector frame and the cutter agree on which side is which.
Vec3d strip_reference_normal(const DrawCutStroke& stroke, const DrawCutParams& params)
{
    const size_t n = stroke.path().size();
    Vec3d sum = Vec3d::Zero();
    for (size_t i = 0; i < n; ++ i) {
        Vec3d sn = stroke.tangent(i).cross(draw_cut_inward_dir(stroke, params, i));
        if (sn.norm() < 1e-9)
            continue;
        sn.normalize();
        if (!sum.isZero() && sn.dot(sum) < 0.0)
            sn = -sn;
        sum += sn;
    }
    return safe_normalize(sum, Vec3d::UnitY());
}

} // namespace

Vec3d draw_cut_surface_point(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w)
{
    if (!stroke.valid())
        return Vec3d::Zero();
    Vec3d pos, dir, tan;
    surface_frame_pieces(stroke, params, s, pos, dir, tan);
    // w is measured ALONG the ruling from the stroke, positive into the part -
    // exactly the parameter the cutter's rails use (out at -extension, in at
    // +depth), so a point built here lies on the surface the boolean will use.
    return pos + w * dir;
}

Vec3d draw_cut_surface_normal(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w)
{
    (void) w; // the ruling is straight, so the normal does not vary along it.
    if (!stroke.valid())
        return Vec3d::UnitZ();

    Vec3d pos, dir, tan;
    surface_frame_pieces(stroke, params, s, pos, dir, tan);
    Vec3d nrm = tan.cross(dir);
    if (nrm.norm() < 1e-9)
        return strip_reference_normal(stroke, params);
    nrm.normalize();

    // THE SIGN, AND WHY A SINGLE GLOBAL REFERENCE CANNOT SUPPLY IT.
    //
    // t x d flips with the drag direction and across an inflection, so it cannot be
    // used raw - two connectors a millimetre apart would point opposite ways. The
    // cutter solid pins its `sweep` by summing the field over the whole stroke and
    // taking ONE direction, and the first version here copied that.
    //
    // That is right for the cutter's sweep (one direction for one slab) and WRONG
    // here. On a CLOSED loop the strip's normal genuinely rotates through a full
    // turn - on a circular plug it is the radial direction, pointing a different way
    // at every sample - so the sum cancels to nearly nothing and whatever survives
    // is noise. Pinning to it flips the frame on roughly half the loop, which is
    // precisely the discontinuity the pinning existed to prevent.
    //
    // The reference has to be LOCAL, and there is already a local field with a
    // consistent sign: the OUTWARD BINORMAL. compute_binormals() orients it away
    // from the loop's interior for a closed stroke and transports it for an open
    // one, so it is continuous by construction and winding-independent. Pinning the
    // strip normal to point the same way as the binormal makes the frame's +Z the
    // direction "out of the plug", continuously, all the way round.
    const Vec3d local_ref = [&]() {
        const std::vector<DrawCutSample>& p = stroke.path();
        const size_t n = p.size();
        if (n == 0)
            return Vec3d(Vec3d::UnitY());
        // The binormal at the sample s falls nearest, which is all the precision a
        // sign decision needs.
        const std::vector<double> acc = path_arc_table(stroke);
        size_t span = 0;
        double t = 0.0;
        locate_arc(stroke, acc, s, span, t);
        return stroke.binormal((t < 0.5 ? span : span + 1) % n);
    }();

    if (nrm.dot(local_ref) < 0.0)
        nrm = -nrm;
    return nrm;
}

Transform3d draw_cut_surface_frame(const DrawCutStroke& stroke, const DrawCutParams& params,
                                   double s, double w, double z_angle)
{
    if (!stroke.valid())
        return Transform3d::Identity();

    const Vec3d n = draw_cut_surface_normal(stroke, params, s, w);

    // Local X: the PLANE's own X projected onto the tangent plane - the same
    // construction curved_cut_sheet_frame() uses, including the same degenerate
    // guard. Keeping the construction identical is what makes a connector's
    // Rotation mean the same thing in both modes.
    Vec3d x = Vec3d::UnitX() - Vec3d::UnitX().dot(n) * n;
    if (x.norm() < 1e-6) {
        // The surface normal is (nearly) the plane's X, so X projects to nothing.
        // Use the plane's Y instead, which cannot also be degenerate.
        x = Vec3d::UnitY() - Vec3d::UnitY().dot(n) * n;
    }
    x = safe_normalize(x, Vec3d::UnitX());
    const Vec3d y = n.cross(x);

    Matrix3d m;
    m.col(0) = x;
    m.col(1) = y;
    m.col(2) = n;

    Transform3d frame = Transform3d::Identity();
    frame.linear() = m;
    if (std::abs(z_angle) > 1e-12)
        frame.rotate(Eigen::AngleAxisd(z_angle, Vec3d::UnitZ()));
    return frame;
}

bool draw_cut_surface_project(const DrawCutStroke& stroke, const DrawCutParams& params,
                              const Vec3d& p, double& s, double& w, double* distance)
{
    s = w = 0.0;
    if (distance != nullptr)
        *distance = 0.0;
    if (!stroke.valid())
        return false;

    const std::vector<DrawCutSample>& path = stroke.path();
    const size_t n = path.size();
    const std::vector<double> acc = path_arc_table(stroke);
    if (acc.size() < 2)
        return false;

    // A sweep over the spans, closest-point on each ruling. The strip is a ruled
    // surface, so per span the nearest point is found by projecting onto the span's
    // own plane - but the spans are short (the resample spacing is 1 mm) and the
    // exact per-span optimum buys nothing over "check both ends of the span and
    // interpolate", so this walks the SAMPLES and refines between the best two.
    //
    // For each sample the ruling is a line through p_i along d_i; the nearest point
    // on that line is the plain projection, and the distance to it is what picks
    // the winner.
    // PHASE 3: the ruling this inverts must be the one draw_cut_surface_point()
    // BUILDS, which for a closed loop is the BAND direction, not the old inward
    // normal. Projecting against the wrong family of lines would land a connector a
    // few millimetres off the surface it was clicked on - and worse, the frame
    // built there would be the frame of a surface that does not exist.
    Vec3d core_n, core_c;
    const bool has_core = stroke.is_closed() &&
                          draw_cut_core_plane(stroke, params, core_n, core_c);
    if (has_core) {
        // The same normal pinning build_core_band() applies, for the same reason.
        // Same threshold as build_core_band(): a barrel loop's average skin normal
        // cancels, and pinning to its noise would invert the ruling this inverts.
        Vec3d avg_skin = Vec3d::Zero();
        for (const DrawCutSample& s2 : path)
            avg_skin += s2.normal;
        if (avg_skin.norm() > 0.25 * double(path.size()) && core_n.dot(avg_skin) < 0.0)
            core_n = -core_n;
    }
    auto ruling_at = [&](size_t i) -> Vec3d {
        if (!has_core)
            return draw_cut_inward_dir(stroke, params, i);
        const Vec3d inward = draw_cut_core_inward(stroke, params, core_n, core_c, i);
        if (params.through_all) {
            // The same -n sweep surface_frame_pieces() uses; see the note there.
            const double a = std::clamp(params.angle_deg, DrawCutMinLipAngleDeg,
                                        DrawCutMaxLipAngleDeg) * M_PI / 180.0;
            return safe_normalize(-core_n + std::tan(a) * inward, -core_n);
        }
        return draw_cut_band_dir(inward, core_n, params.angle_deg);
    };

    double best_d2 = std::numeric_limits<double>::max();
    size_t best_i  = 0;
    double best_w  = 0.0;
    for (size_t i = 0; i < n; ++ i) {
        const Vec3d d = ruling_at(i);
        const Vec3d v = p - path[i].pos;
        const double wi = v.dot(d);
        const double d2 = (v - wi * d).squaredNorm();
        if (d2 < best_d2) {
            best_d2 = d2;
            best_i  = i;
            best_w  = wi;
        }
    }

    // Refine along the stroke: try the two neighbouring spans at a few subdivisions
    // and keep the best. Cheap, and it matters because a connector placed by a
    // click has to land where the user saw the surface, not a sample away from it.
    const size_t span_count = stroke.is_closed() ? n : (n > 0 ? n - 1 : 0);
    double best_s = best_i < acc.size() ? acc[best_i] : 0.0;
    if (span_count > 0) {
        // The two spans meeting at the winning SAMPLE. A closed stroke wraps at both
        // ends; an open one CLAMPS - wrapping the last sample round to span 0 would
        // refine at the wrong end of the line entirely, which on a long open stroke
        // puts the connector's frame somewhere it has never been.
        const size_t prev_span = stroke.is_closed() ? (best_i + n - 1) % n
                                                    : (best_i > 0 ? best_i - 1 : 0);
        const size_t next_span = stroke.is_closed() ? (best_i % span_count)
                                                    : std::min(best_i, span_count - 1);
        for (size_t span : { prev_span, next_span }) {
            if (span + 1 >= acc.size())
                continue;
            const double s0 = acc[span], s1 = acc[span + 1];
            const int steps = 8;
            for (int k = 0; k <= steps; ++ k) {
                const double ss = s0 + (s1 - s0) * double(k) / double(steps);
                Vec3d pos, dir, tan;
                surface_frame_pieces(stroke, params, ss, pos, dir, tan);
                const Vec3d v = p - pos;
                const double ww = v.dot(dir);
                const double d2 = (v - ww * dir).squaredNorm();
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_s  = ss;
                    best_w  = ww;
                }
            }
        }
    }

    s = best_s;
    w = best_w;
    if (distance != nullptr)
        *distance = std::sqrt(best_d2);
    return true;
}

bool draw_cut_surface_contains(const DrawCutStroke& stroke, const DrawCutParams& params,
                               double s, double w, double margin, double depth_reach)
{
    if (!stroke.valid())
        return false;

    const double m = std::max(0.0, margin);

    // The ruled span the cutter actually builds: out at -extension, in at +depth.
    // A connector has to sit `margin` clear of both rims, or its body hangs off the
    // surface and the split leaves it half-made.
    const double lo = -std::max(0.0, params.extension) + m;
    const double hi = std::max(0.0, depth_reach) - m;
    if (lo > hi || w < lo || w > hi)
        return false;

    if (stroke.is_closed())
        return true; // s wraps, so there is no end to fall off.

    const std::vector<double> acc = path_arc_table(stroke);
    if (acc.size() < 2)
        return false;
    // The cutter extends an open stroke by Extension along the tangent at each end,
    // so the surface really does reach that far - but a connector on the extension
    // is standing on surface that is outside the part, so the usable domain is the
    // STROKE's own span with the margin taken off each end.
    return s >= m && s <= acc.back() - m;
}

double draw_cut_surface_curvature_radius(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w)
{
    constexpr double Flat = 1e6;
    if (!stroke.valid())
        return Flat;

    // ACROSS THE RULES only. Along a rule the strip is a straight line, so its
    // curvature there is exactly zero and its radius infinite - a ruled surface is
    // developable that way. What can bend is the walk ALONG the stroke, and that is
    // what a hinge's knuckle run or a thread's pitch line has to sit on.
    //
    // Three points at (s - h, w), (s, w), (s + h, w) on the surface itself - not on
    // the stroke - because the draft angle and the depth both change how fast the
    // surface turns as you move out along the ruling: an outward-drafted strip
    // opens out, so its far edge is flatter than the stroke that generated it.
    const std::vector<double> acc = path_arc_table(stroke);
    if (acc.size() < 2 || acc.back() < 1e-9)
        return Flat;

    // THE STEP HAS TO STRADDLE SEVERAL SAMPLES, and this is the trap. The path is a
    // POLYGON - a 1 mm resample of whatever the user drew - so three points taken a
    // fraction of a millimetre apart land on one or two of its straight facets and
    // the circumradius through them measures the FACETING, not the shape. On a 6 mm
    // ring a step of 0.59 mm reads a radius of 3.7 mm, which is wrong by a third and
    // wrong in the direction that matters (it would warn about connectors that are
    // fine).
    //
    // So the step is at least a few resample spacings - 4 mm here, which is four
    // samples at the default - as well as a fraction of the total length. On a
    // curve tight enough for the flat-patch warning to be interesting, that is still
    // a small arc; on a gentle one the answer is "flat" either way.
    const double h = std::max(4.0 * DrawCutStroke::DefaultSpacing, acc.back() / 64.0);

    const Vec3d a = draw_cut_surface_point(stroke, params, s - h, w);
    const Vec3d b = draw_cut_surface_point(stroke, params, s,     w);
    const Vec3d c = draw_cut_surface_point(stroke, params, s + h, w);

    const double ab = (b - a).norm(), bc = (c - b).norm(), ca = (a - c).norm();
    if (ab < 1e-9 || bc < 1e-9 || ca < 1e-9)
        return Flat;
    const double area = 0.5 * (b - a).cross(c - a).norm();
    if (area < 1e-12)
        return Flat;
    // The circumradius through the three points.
    return std::min(Flat, (ab * bc * ca) / (4.0 * area));
}

bool draw_cut_patch_is_flat_enough(const DrawCutStroke& stroke, const DrawCutParams& params,
                                   double s, double w, double extent)
{
    if (extent <= 0.0)
        return true;
    return draw_cut_surface_curvature_radius(stroke, params, s, w) >= CurvedConnectorFlatPatchFactor * extent;
}

double draw_cut_surface_tilt_deg(const DrawCutStroke& stroke, const DrawCutParams& params, double s, double w)
{
    if (!stroke.valid())
        return 0.0;
    const Vec3d n = draw_cut_surface_normal(stroke, params, s, w);
    // The angle between the surface normal and the plane's +Z, taken to the nearer
    // of the two poles: a normal pointing at -Z is the same tilt as one pointing at
    // +Z as far as printing is concerned, and which of the two the sign lands on is
    // an artefact of the drag direction.
    return std::acos(std::clamp(std::abs(n.z()), 0.0, 1.0)) * 180.0 / M_PI;
}

// ---------------------------------------------------------------------------
// Empty sides
// ---------------------------------------------------------------------------

// Is `pt` inside the closed triangle soup `solid`? Parity of the crossings of a ray
// from pt. The cutter is a few thousand faces, so this is cheap, and the answer only
// has to be right for the vertices of a real mesh - a vertex exactly ON the cutter's
// surface can go either way and the caller does not care, since such a vertex is on
// the cut face.
//
// The ray direction is NOT an axis. An axis ray runs exactly along, or exactly
// through the shared edges of, the axis-aligned faces this cutter is full of (a
// stroke drawn on a cube's flat top face gives a cutter with faces parallel to +Z
// everywhere), and every one of those is a parity answer that could go either way -
// which showed up as "an open line right across the cube reports a side empty". A
// direction with no small integer ratios between its components misses every such
// degeneracy for any geometry a user will produce.
static bool point_in_solid(const indexed_triangle_set& solid, const Vec3d& pt)
{
    // An "irrational-ish" direction, normalised. Fixed rather than random so the
    // answer is deterministic, which a cut has to be.
    static const Vec3d dir = Vec3d(0.3131592653, 0.4271828182, 0.8481471805).normalized();

    // A basis with `dir` as its third axis, so the in-triangle test below is the
    // same barycentric solve an axis ray would use, just in the rotated frame.
    const Vec3d ax = (std::abs(dir.x()) < 0.9 ? Vec3d::UnitX() : Vec3d::UnitY());
    const Vec3d e0 = (ax - ax.dot(dir) * dir).normalized();
    const Vec3d e1 = dir.cross(e0);

    int crossings = 0;
    for (const Vec3i32& tri : solid.indices) {
        // Each vertex as (u, v, w): u,v across the ray, w along it.
        Vec3d uvw[3];
        for (int k = 0; k < 3; ++ k) {
            const Vec3d rel = solid.vertices[tri(k)].cast<double>() - pt;
            uvw[k] = Vec3d(rel.dot(e0), rel.dot(e1), rel.dot(dir));
        }

        // Barycentric solve at (u, v) == (0, 0), i.e. on the ray's own axis. A
        // triangle seen edge-on projects to zero area and is skipped: a neighbouring
        // non-degenerate face provides the same crossing.
        const double det = (uvw[1].y() - uvw[2].y()) * (uvw[0].x() - uvw[2].x()) +
                           (uvw[2].x() - uvw[1].x()) * (uvw[0].y() - uvw[2].y());
        if (std::abs(det) < 1e-12)
            continue;
        const double l0 = ((uvw[1].y() - uvw[2].y()) * (-uvw[2].x()) + (uvw[2].x() - uvw[1].x()) * (-uvw[2].y())) / det;
        const double l1 = ((uvw[2].y() - uvw[0].y()) * (-uvw[2].x()) + (uvw[0].x() - uvw[2].x()) * (-uvw[2].y())) / det;
        const double l2 = 1.0 - l0 - l1;
        if (l0 < 0.0 || l1 < 0.0 || l2 < 0.0)
            continue;

        // Only crossings in FRONT of the point count.
        if (l0 * uvw[0].z() + l1 * uvw[1].z() + l2 * uvw[2].z() > 0.0)
            ++ crossings;
    }
    return (crossings & 1) != 0;
}

void draw_cut_empty_sides(const indexed_triangle_set& mesh,
                          const DrawCutStroke&        stroke,
                          const DrawCutParams&        params,
                          bool&                       upper_empty,
                          bool&                       lower_empty)
{
    upper_empty = true;
    lower_empty = true;
    if (mesh.empty() || !stroke.valid())
        return;

    BoundingBoxf3 bbox;
    for (const Vec3f& v : mesh.vertices)
        bbox.merge(v.cast<double>());

    double face_lo = 0.0, face_hi = 0.0;
    curved_cut_thickness_faces(std::max(0.0, params.thickness), params.thickness_offset, face_lo, face_hi);
    const bool kerf = params.thickness > 0.0;

    const indexed_triangle_set cutter_lo = draw_cut_cutter_solid(stroke, params, bbox, face_lo, &mesh);
    if (cutter_lo.empty())
        return;
    const indexed_triangle_set cutter_hi = kerf ? draw_cut_cutter_solid(stroke, params, bbox, face_hi, &mesh)
                                               : indexed_triangle_set();

    // "Inside the cutter" is the INTERSECTION side, which cut_with_solid() calls
    // `lower`. draw_cut_split() then swaps the pair for a closed stroke (the plug
    // is the upper half), so the swap has to happen here too or the panel would
    // name the wrong side.
    //
    // COST. Each test is one parity ray against the cutter's faces, and the loop
    // short-circuits as soon as both sides have a vertex - which on a cut that works
    // is the first handful of vertices. The bad case is the cut that does NOT work:
    // then one side never fills in and every vertex is tested, which on a 500k-vertex
    // model against a few thousand cutter faces is a visible stall - and this runs on
    // every parameter change, from a slider being dragged.
    //
    // So the walk is STRIDED: it visits at most MaxProbes vertices, spread evenly
    // over the mesh rather than taken from the front (the front of an STL's vertex
    // list is one corner of the part, which would answer for that corner only). The
    // test stays conservative in the direction that matters - it can only ever claim
    // a side is NON-empty, which requires actually finding a vertex there - so a
    // stride can produce a false "empty", never a false "has material". A false
    // "empty" on a part whose only material on one side is a feature smaller than one
    // stride is the trade, and the panel's warning is advisory in that direction: it
    // says "this line does not separate the part", which is the safe thing to say
    // about a cut leaving a sliver.
    static const size_t MaxProbes = 20000;
    const size_t n_verts = mesh.vertices.size();
    const size_t stride  = n_verts > MaxProbes ? (n_verts + MaxProbes - 1) / MaxProbes : 1;

    bool inside_empty = true, outside_empty = true;
    for (size_t i = 0; i < n_verts; i += stride) {
        const Vec3d pt = mesh.vertices[i].cast<double>();
        if (inside_empty && point_in_solid(cutter_lo, pt))
            inside_empty = false;
        if (outside_empty && !point_in_solid(kerf ? cutter_hi : cutter_lo, pt))
            outside_empty = false;
        if (!inside_empty && !outside_empty)
            break;
    }

    if (stroke.is_closed()) {
        upper_empty = inside_empty;   // the plug
        lower_empty = outside_empty;  // the remainder
    }
    else {
        lower_empty = inside_empty;
        upper_empty = outside_empty;
    }
}

// ---------------------------------------------------------------------------
// The split
// ---------------------------------------------------------------------------

bool draw_cut_split(const indexed_triangle_set& mesh,
                    const DrawCutStroke&        stroke,
                    const DrawCutParams&        params,
                    indexed_triangle_set*       upper,
                    indexed_triangle_set*       lower,
                    DrawCutError*               err)
{
    auto fail = [err](DrawCutError e) {
        if (err != nullptr)
            *err = e;
        return false;
    };
    if (err != nullptr)
        *err = DrawCutError::None;

    if (mesh.empty())
        return fail(DrawCutError::CutterDegenerate);
    if (!stroke.valid())
        return fail(stroke.error() == DrawCutError::None ? DrawCutError::TooShort : stroke.error());

    BoundingBoxf3 bbox;
    for (const Vec3f& v : mesh.vertices)
        bbox.merge(v.cast<double>());

    const double t = std::max(0.0, params.thickness);
    double face_lo = 0.0, face_hi = 0.0;
    curved_cut_thickness_faces(t, params.thickness_offset, face_lo, face_hi);
    const bool kerf = t > 0.0;

    // THE KERF, the same shape the curved cut uses: with a thickness the two
    // halves are cut by DIFFERENT solids - one displaced by face_lo along the
    // strip's own normal, the other by face_hi - so the band between them is
    // removed from both. At t == 0 both offsets are zero and ONE solid is built,
    // which is why the no-kerf output is not "close to" what it was without the
    // kerf code, it is the same.
    const indexed_triangle_set cutter_lo = draw_cut_cutter_solid(stroke, params, bbox, face_lo, &mesh);
    if (cutter_lo.empty())
        return fail(DrawCutError::CutterDegenerate);
    const indexed_triangle_set cutter_hi = kerf ? draw_cut_cutter_solid(stroke, params, bbox, face_hi, &mesh)
                                               : indexed_triangle_set();
    if (kerf && cutter_hi.empty())
        return fail(DrawCutError::CutterDegenerate);

    // A FOLD is advisory: Manifold tolerates a slightly self-intersecting cutter
    // and mcut is the fallback, so a fold degrades to "boolean failed ->
    // complement recovery" rather than to a wrong cut. Log it so the reason is
    // recoverable from a log when a cut does come out strange.
    //
    // PHASE 3: the fold test is about the RULED STRIP, so it only means anything
    // for an open stroke now. A closed loop's band travels towards the loop's own
    // axis and terminates on the core plane, so it cannot fold against a tight
    // corner the way a ruling could - what it can do is over-inset (see below).
    double kappa = 0.0;
    if (!stroke.is_closed() && draw_cut_strip_folds(stroke, params.extension, &kappa))
        BOOST_LOG_TRIVIAL(warning) << "Draw cut: the ruled strip folds near a tight corner (extension "
                                   << params.extension << " mm, curvature " << kappa
                                   << " 1/mm); the cut may be imprecise there";

    // THE BAND CAN EAT THE CORE. The inner curve travels depth * cos(angle) inward
    // in the plane, so a depth larger than the loop's own in-plane radius insets
    // the core polygon past the centre and the ring self-intersects. The boolean
    // still returns something, but the core is inside out, so say so.
    if (stroke.is_closed() && !params.through_all) {
        Vec3d cn, cc;
        if (draw_cut_core_plane(stroke, params, cn, cc)) {
            double min_r = std::numeric_limits<double>::max();
            for (const DrawCutSample& s : stroke.path()) {
                const Vec3d to_c = cc - s.pos;
                min_r = std::min(min_r, (to_c - to_c.dot(cn) * cn).norm());
            }
            const double inset = std::max(0.01, params.depth) *
                                 std::cos(std::clamp(params.angle_deg, DrawCutMinLipAngleDeg,
                                                     DrawCutMaxLipAngleDeg) * M_PI / 180.0);
            if (min_r < std::numeric_limits<double>::max() && inset >= min_r)
                BOOST_LOG_TRIVIAL(warning)
                    << "Draw cut: Depth " << params.depth << " mm at angle " << params.angle_deg
                    << " deg insets the core by " << inset << " mm, past the loop's own radius "
                    << min_r << " mm; there is no flat core left. Reduce Depth or raise Angle.";
        }
    }

    // WHICH SIDE IS UPPER. cut_with_solid() produces (inside, outside) as
    // (lower, upper):
    //
    //   OPEN stroke: that IS the convention we want - the strip cuts the part in
    //     two with no inside or outside, and the plane's own +Z convention
    //     applies, which is what cut_with_solid()'s A_NOT_B side already means for
    //     the flat and curved cuts.
    //   CLOSED stroke: the cutter encloses a PLUG, and the plug - the INTERSECTION -
    //     is what the stroke drew around, so it is the UPPER half. Swap the pair.
    //
    // Doing the swap here rather than in the caller is what lets the gizmo and the
    // tests treat open and closed strokes identically.
    indexed_triangle_set  inside, outside;
    indexed_triangle_set* p_inside  = nullptr;
    indexed_triangle_set* p_outside = nullptr;
    if (stroke.is_closed()) {
        p_inside  = upper != nullptr ? &inside  : nullptr;
        p_outside = lower != nullptr ? &outside : nullptr;
    }
    else {
        p_inside  = lower != nullptr ? &inside  : nullptr;
        p_outside = upper != nullptr ? &outside : nullptr;
    }

    const bool ok = cut_with_solid(mesh, cutter_lo, kerf ? cutter_hi : cutter_lo, kerf,
                                   p_outside, p_inside, "Draw cut");

    if (stroke.is_closed()) {
        if (upper != nullptr) *upper = std::move(inside);
        if (lower != nullptr) *lower = std::move(outside);
    }
    else {
        if (lower != nullptr) *lower = std::move(inside);
        if (upper != nullptr) *upper = std::move(outside);
    }

    if (!ok)
        return fail(DrawCutError::EmptySide);
    return true;
}

// ---------------------------------------------------------------------------
// THE CHAIN. 2026-09-12, from owner click-testing.
// ---------------------------------------------------------------------------

double draw_cut_chain_snap_radius(const BoundingBoxf3& bbox)
{
    if (!bbox.defined)
        return ChainSnapMinMm;
    return std::clamp(ChainSnapFraction * bbox.size().norm(), ChainSnapMinMm, ChainSnapMaxMm);
}

void DrawCutChain::clear()
{
    m_samples.clear();
    m_bounds.clear();
    m_closed        = false;
    m_finished_open = false;
}

void DrawCutChain::set_samples(const std::vector<DrawCutSample>& samples, bool closed, bool finished_open)
{
    m_samples = samples;
    m_bounds.clear();
    if (!m_samples.empty())
        // ONE stroke, because the strokes a reopened cut was originally drawn in are
        // not what is being edited: the undo step a user expects after "Edit cut" is
        // "take back what I just did", and the recipe's line is not something they
        // just did.
        m_bounds.emplace_back(size_t(0), m_samples.size());
    m_closed = closed && m_samples.size() >= MinChainSamples;
    // Mutually exclusive by construction: a closed chain is a loop, and "finished open"
    // is the answer to a question a loop does not raise.
    m_finished_open = !m_closed && finished_open && m_samples.size() >= MinChainSamples;
}

// 2026-09-13, owner click-test item 1. See the header for why the pick moved from 3D
// millimetres to screen pixels. Deliberately a free function over the chain rather than
// a member: it needs a camera, which libslic3r has no notion of, so the projection comes
// in as a callable - which is also what lets a test build one by hand.
DrawChainEnd draw_cut_chain_end_at_pixel(const DrawCutChain&                                      chain,
                                         const Vec2d&                                             mouse_px,
                                         const std::function<std::optional<Vec2d>(const Vec3d&)>& project,
                                         double                                                   pick_px)
{
    // No free endpoints: an empty chain has none, a closed chain has none. Both are the
    // caller's cases to handle (an empty chain accepts a stroke anywhere; a closed one
    // accepts none), and answering them here would be a claim about handles that are not
    // drawn.
    if (chain.empty() || chain.is_closed() || !project)
        return DrawChainEnd::None;

    const double r2 = std::max(1e-6, pick_px) * std::max(1e-6, pick_px);

    auto d2_of = [&](const Vec3d& p) -> double {
        const std::optional<Vec2d> s = project(p);
        // A point that does not project (behind the eye, or off any sensible plane) is
        // not pickable - infinity rather than a guess, so it can never win the tie-break.
        return s ? (*s - mouse_px).squaredNorm() : std::numeric_limits<double>::infinity();
    };
    const double d2_fr = d2_of(chain.front_pos());
    const double d2_bk = d2_of(chain.back_pos());

    // The NEARER endpoint wins when both are in range - a short chain can have its two
    // ends within a pick radius of each other on screen. Same tie-break, and the same
    // Back-first preference, as end_for_start() makes in 3D.
    if (d2_bk <= r2 && d2_bk <= d2_fr)
        return DrawChainEnd::Back;
    if (d2_fr <= r2)
        return DrawChainEnd::Front;
    return DrawChainEnd::None;
}

DrawChainEnd DrawCutChain::end_for_start(const Vec3d& p, double snap_radius) const
{
    // An empty chain accepts anything: the first stroke has nothing to continue.
    if (m_samples.empty())
        return DrawChainEnd::Back;
    // A closed chain accepts nothing. It is finished; continuing it would have to
    // pick a place to reopen it, and there is no gesture that says which.
    if (m_closed)
        return DrawChainEnd::None;

    const double r    = std::max(1e-6, snap_radius);
    const double d_bk = (p - back_pos()).norm();
    const double d_fr = (p - front_pos()).norm();
    // The NEARER endpoint wins when both are in range, which happens on a short
    // chain whose two ends are within a snap radius of each other.
    if (d_bk <= r && d_bk <= d_fr)
        return DrawChainEnd::Back;
    if (d_fr <= r)
        return DrawChainEnd::Front;
    return DrawChainEnd::None;
}

DrawChainEnd DrawCutChain::append(const std::vector<DrawCutSample>& stroke, double snap_radius)
{
    if (stroke.size() < 2)
        return DrawChainEnd::None;
    // The end is decided here, from the stroke's own first sample; append_at() does the
    // rest. Kept as the entry point every existing caller (and every phase 1/2 test)
    // already uses.
    return append_at(stroke, end_for_start(stroke.front().pos, snap_radius), snap_radius);
}

DrawChainEnd DrawCutChain::append_at(const std::vector<DrawCutSample>& stroke, DrawChainEnd at,
                                     double snap_radius)
{
    if (stroke.size() < 2)
        return DrawChainEnd::None;
    if (at == DrawChainEnd::None)
        return DrawChainEnd::None;
    // A CLOSED chain refuses every continuation however the end was chosen: it is
    // finished, and reopening it would have to pick a place to reopen, which no gesture
    // says. The screen-space pick cannot override that - it only replaces the question
    // "which of the two free ends did the user aim at".
    if (m_closed)
        return DrawChainEnd::None;

    const double r     = std::max(1e-6, snap_radius);
    const bool   first = m_samples.empty();

    // WHICH ENDPOINT THE NEW STROKE'S OWN END MIGHT CLOSE ON.
    //  - appending at the Back: the free end is the chain's FRONT;
    //  - prepending at the Front: the free end is the chain's BACK;
    //  - a FIRST stroke has no other end, so it closes on its own first sample,
    //    which is the phase 1 "a circle drawn in one gesture" case.
    const Vec3d far_end = first ? stroke.front().pos
                        : (at == DrawChainEnd::Back ? front_pos() : back_pos());

    // Drop the stroke's own first sample when it is a duplicate of the endpoint it
    // continues from - it is the SAME point on the model, and a zero-length span at
    // the join is exactly what the tangent maths cannot read. Not done for a first
    // stroke, which has no join.
    size_t skip = 0;
    if (!first) {
        const Vec3d& join = at == DrawChainEnd::Back ? back_pos() : front_pos();
        if ((stroke.front().pos - join).norm() < 1e-9)
            skip = 1;
    }
    if (stroke.size() - skip < 1)
        return DrawChainEnd::None;

    // The closure test uses the stroke's LAST sample, whichever end it is appended
    // at: the last sample is where the cursor was when the user let go, which is the
    // point they aimed at the far endpoint.
    //
    // MinChainSamples is checked on the RESULT, not on the chain so far: a chain that
    // would close with four samples in total is a dab, and calling it a loop would
    // hand the cutter builder a triangle.
    const size_t total_after = m_samples.size() + (stroke.size() - skip);
    const bool   closes      = total_after >= MinChainSamples &&
                               (stroke.back().pos - far_end).norm() <= r;

    if (at == DrawChainEnd::Back) {
        const size_t begin = m_samples.size();
        m_samples.insert(m_samples.end(), stroke.begin() + int(skip), stroke.end());
        m_bounds.emplace_back(begin, m_samples.size());
    }
    else {
        // REVERSED, then prepended. The stroke was drawn AWAY from the chain's front,
        // so as captured it runs the wrong way: prepending it unreversed makes the
        // sequence double back at the join and every tangent there is wrong.
        std::vector<DrawCutSample> rev(stroke.begin() + int(skip), stroke.end());
        std::reverse(rev.begin(), rev.end());
        const size_t added = rev.size();
        m_samples.insert(m_samples.begin(), rev.begin(), rev.end());
        // Every earlier stroke's range shifts by what was prepended, which is why the
        // bounds are stored as ranges and fixed up here rather than inferred.
        for (std::pair<size_t, size_t>& b : m_bounds) {
            b.first  += added;
            b.second += added;
        }
        // APPENDED AT THE END OF m_bounds, not at its start, even though the samples
        // went in at the start of m_samples. m_bounds is in APPEND order - "newest
        // last" - because that is the order undo consumes it in; ordering it by
        // position in m_samples instead would make undo_last_stroke() take back the
        // OLDEST stroke after any front append, which is a bug whose symptom is the
        // line losing its far end when you Ctrl+Z the near one.
        m_bounds.emplace_back(size_t(0), added);
    }

    if (closes)
        m_closed = true;
    // A chain that has just grown is one the user is still drawing, so any earlier
    // "finished, not a loop" verdict no longer applies.
    m_finished_open = false;
    return at;
}

bool DrawCutChain::undo_last_stroke()
{
    if (m_bounds.empty())
        return false;

    // The LAST APPENDED stroke is the last entry of m_bounds (which is in append
    // order), wherever in m_samples it happens to sit - a Front append puts it at
    // index 0.
    const std::pair<size_t, size_t> b = m_bounds.back();
    m_bounds.pop_back();
    m_samples.erase(m_samples.begin() + int(b.first), m_samples.begin() + int(b.second));

    const size_t removed = b.second - b.first;
    for (std::pair<size_t, size_t>& r : m_bounds) {
        // Only the ranges AFTER the removed one move, and a Front append means every
        // remaining range is after it.
        if (r.first >= b.second) {
            r.first  -= removed;
            r.second -= removed;
        }
    }

    // Taking back a stroke takes back the closure it made. A chain cannot stay closed
    // after losing the span that closed it, and the alternative (keeping m_closed and
    // letting the resampler bridge a gap of any size) is the "wild shape" the whole
    // change exists to remove.
    m_closed        = false;
    m_finished_open = false;
    if (m_samples.size() < 2)
        clear();
    return true;
}

bool DrawCutChain::force_close()
{
    if (m_samples.size() < MinChainSamples)
        return false;
    m_closed        = true;
    m_finished_open = false;
    return true;
}

// 2026-09-13, OWNER CLICK-TEST ITEM 2.
//
// The bug force_close() had: it set m_closed and nothing else, so the span that closed
// the loop was the straight CHORD from the last sample to the first, in the cut plane's
// frame. DrawCutStroke::finish() then resampled along that chord and
// draw_cut_cutter_solid() swept the ruling along it - and on a line drawn round the
// outside of a part, the chord is a line through the middle of the part, so the surface
// generated over it emerged on the FAR side. That is the owner's "Close loop mirrors the
// line to the other side of the object": nothing was ever reflected, but a chord through
// a solid and a reflection look the same from outside.
//
// The fix is to close along a path that is ON the surface. The path's points are found
// by the caller (the gizmo, which has the camera and the raycaster this file has
// neither of) and appended here as one stroke, so the closing span becomes a sequence of
// real spans over the model and Ctrl+Z takes the whole closure back in one step.
bool DrawCutChain::close_along_path(const std::vector<DrawCutSample>& path)
{
    if (m_closed)
        return false;
    // The result has to be a loop, not a dab - the same floor force_close() enforces,
    // measured on what the chain will HAVE, since a short chain plus a long path is a
    // perfectly good loop.
    if (m_samples.size() + path.size() < MinChainSamples)
        return false;

    if (!path.empty()) {
        const size_t begin = m_samples.size();
        m_samples.insert(m_samples.end(), path.begin(), path.end());
        // ONE stroke, so undo_last_stroke() takes the closure back whole - and, because
        // it also clears m_closed, gives back exactly the open line the user had.
        m_bounds.emplace_back(begin, m_samples.size());
    }
    m_closed        = true;
    m_finished_open = false;
    return true;
}

bool DrawCutChain::finish_open()
{
    if (m_closed || m_samples.size() < MinChainSamples)
        return false;
    m_finished_open = true;
    return true;
}

DrawCutError DrawCutChain::finish(DrawCutStroke& out, double spacing, double smoothing) const
{
    out.clear();
    // OWNER FEEDBACK 1 AND 2, in one branch: an open chain produces NO stroke, so no
    // caller can build a cutter from it, so nothing is lofted and nothing is
    // previewed. The gizmo draws the polyline from samples() instead, which is what
    // the user needs while they work.
    // An unfinished chain produces NO stroke. "Finished and not a loop" is a different
    // thing from "not finished yet", and only the user can tell those apart from the
    // samples - which is what finish_open() is for.
    if (!m_closed && !m_finished_open)
        return DrawCutError::NotClosed;
    if (m_samples.size() < MinChainSamples)
        return DrawCutError::TooShort;

    for (const DrawCutSample& s : m_samples)
        out.append(s.pos, s.normal, s.facet);
    if (m_finished_open) {
        // PHASE 1'S OPEN CUT. finish() decides open from the gap between the first and
        // the last sample, and here that gap is whatever the user drew - for a line
        // across a part, the whole part - so the decision comes out open on its own.
        const DrawCutError err = out.finish(spacing, smoothing, /*force_closed*/ false);
        if (err == DrawCutError::None && out.is_closed()) {
            // The user said this is NOT a loop, so a stroke that came back closed is not
            // the thing they asked for. Refusing beats cutting a plug out of a part they
            // meant to halve. (The snap radius is wider than finish()'s own closing
            // tolerance, so a chain that reaches here should never be closed - this is
            // the guard for the case where those two constants ever cross.)
            out.clear();
            return DrawCutError::NotClosed;
        }
        return err;
    }
    // force_closed, because the CHAIN decided it is closed - by the snap radius,
    // which is deliberately wider than finish()'s own closing tolerance on a large
    // part. Letting finish() re-decide would silently reopen a chain the user watched
    // snap shut.
    return out.finish(spacing, smoothing, /*force_closed*/ true);
}

bool DrawCutChain::operator==(const DrawCutChain& o) const
{
    if (m_closed != o.m_closed || m_finished_open != o.m_finished_open ||
        m_samples.size() != o.m_samples.size() || m_bounds != o.m_bounds)
        return false;
    for (size_t i = 0; i < m_samples.size(); ++ i)
        if (!m_samples[i].pos.isApprox(o.m_samples[i].pos) ||
            !m_samples[i].normal.isApprox(o.m_samples[i].normal) ||
            m_samples[i].facet != o.m_samples[i].facet)
            return false;
    return true;
}

// ---------------------------------------------------------------------------
// THE HALVES CLASSIFICATION. 2026-09-12, owner feedback item 3.
// ---------------------------------------------------------------------------

bool draw_cut_classify_upper(const indexed_triangle_set& cutter, bool closed, const Vec3d& p)
{
    if (cutter.empty())
        return false;
    const bool inside = point_in_solid(cutter, p);
    // draw_cut_split()'s convention, and the reason this cannot just return `inside`:
    // for a CLOSED stroke the plug (inside the cutter) is the UPPER half, and for an
    // OPEN one the cutter is the swept slab on the lower side, so inside is LOWER.
    return closed ? inside : !inside;
}

std::vector<float> draw_cut_inside_field(const indexed_triangle_set& cutter,
                                         bool                        closed,
                                         const BoundingBoxf3&        bbox,
                                         int                         nx,
                                         int                         ny,
                                         int                         nz,
                                         BoundingBoxf3*              field_bbox)
{
    nx = std::max(2, nx);
    ny = std::max(2, ny);
    nz = std::max(2, nz);
    std::vector<float> field(size_t(nx) * size_t(ny) * size_t(nz), 1.0f);

    BoundingBoxf3 fb = bbox;
    if (!fb.defined || cutter.empty()) {
        if (field_bbox)
            *field_bbox = fb;
        return field;
    }

    // GROW by one voxel on each side, so a fragment exactly on the part's surface -
    // which is every fragment the shader will ever ask about - sits INSIDE the field
    // rather than on its clamped border, where a linear fetch would read a half-value
    // from outside and the boundary would creep by half a voxel.
    const Vec3d raw = fb.size();
    const Vec3d cell(std::max(1e-6, raw.x() / double(nx - 1)),
                     std::max(1e-6, raw.y() / double(ny - 1)),
                     std::max(1e-6, raw.z() / double(nz - 1)));
    fb.min -= cell;
    fb.max += cell;
    const Vec3d span = fb.size();
    const Vec3d step(span.x() / double(nx - 1), span.y() / double(ny - 1), span.z() / double(nz - 1));

    // ONE PARITY RAY PER COLUMN, not per voxel. The column runs along +Z, so for each
    // (i, j) the crossings of that line with the cutter's triangles are collected
    // once, sorted, and the whole column of nz voxels is filled by walking them - the
    // parity between two consecutive crossings is constant, which is what makes a
    // 64^3 field affordable on a mouse-up instead of 262144 full mesh passes.
    //
    // The DEGENERACY the per-point test avoids by using an irrational direction is
    // handled differently here, because the direction is fixed at +Z: a triangle the
    // column's line passes exactly through the edge of would be counted twice or not
    // at all. The column's XY is nudged by a fixed sub-voxel irrational fraction of
    // the cell, which moves every column off the axis-aligned grid the cutter's own
    // faces are built on (a stroke on a cube's top face gives faces parallel to +Z
    // everywhere) without moving any column more than a fraction of a voxel - the
    // same trick, applied to the sample points rather than to the ray.
    const double jx = 0.00031831 * step.x();
    const double jy = 0.00027183 * step.y();

    std::vector<double> zs;
    for (int j = 0; j < ny; ++ j) {
        const double y = fb.min.y() + double(j) * step.y() + jy;
        for (int i = 0; i < nx; ++ i) {
            const double x = fb.min.x() + double(i) * step.x() + jx;

            zs.clear();
            for (const Vec3i32& tri : cutter.indices) {
                const Vec3d a = cutter.vertices[tri(0)].cast<double>();
                const Vec3d b = cutter.vertices[tri(1)].cast<double>();
                const Vec3d c = cutter.vertices[tri(2)].cast<double>();
                // Barycentric solve at (x, y) in the XY plane.
                const double det = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
                if (std::abs(det) < 1e-12)
                    continue; // edge-on to the column; a neighbour carries the crossing
                const double l0 = ((b.y() - c.y()) * (x - c.x()) + (c.x() - b.x()) * (y - c.y())) / det;
                const double l1 = ((c.y() - a.y()) * (x - c.x()) + (a.x() - c.x()) * (y - c.y())) / det;
                const double l2 = 1.0 - l0 - l1;
                if (l0 < 0.0 || l1 < 0.0 || l2 < 0.0)
                    continue;
                zs.push_back(l0 * a.z() + l1 * b.z() + l2 * c.z());
            }
            std::sort(zs.begin(), zs.end());

            // Walk the column. `crossed` counts how many crossings are BELOW the
            // current z, so its parity is "inside".
            size_t crossed = 0;
            for (int k = 0; k < nz; ++ k) {
                const double z = fb.min.z() + double(k) * step.z();
                while (crossed < zs.size() && zs[crossed] < z)
                    ++ crossed;
                const bool inside = (crossed & 1) != 0;
                const bool upper  = closed ? inside : !inside;
                // -1 UPPER, +1 LOWER: the shader's `side < 0` is side 1, which
                // apply_color_clip_plane_colors() feeds with UPPER_PART_COLOR.
                field[(size_t(k) * size_t(ny) + size_t(j)) * size_t(nx) + size_t(i)] = upper ? -1.0f : 1.0f;
            }
        }
    }

    if (field_bbox)
        *field_bbox = fb;
    return field;
}

} // namespace Slic3r
