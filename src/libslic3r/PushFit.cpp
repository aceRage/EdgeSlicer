#include "PushFit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <boost/log/trivial.hpp>

#include "MeshBoolean.hpp"
#include "TriangleMeshSlicer.hpp"

namespace Slic3r {

// ------------------------------------------------------------------ presets and sizing

namespace {

struct PresetRow
{
    float d, len, lip_h, lip_w, flat, slot_w, slot_l, clr, depth_clr, mouth;
};

// Design 3.2. All mm; L is per side.
const PresetRow PRESET_ROWS[3] = {
    { 4.f,  5.f, 0.20f, 0.8f, 0.6f, 0.8f, 3.f, 0.10f, 0.4f, 0.3f }, // S
    { 6.f,  7.f, 0.25f, 1.0f, 0.9f, 1.0f, 4.f, 0.10f, 0.4f, 0.4f }, // M
    { 8.f, 10.f, 0.30f, 1.2f, 1.2f, 1.2f, 6.f, 0.12f, 0.5f, 0.5f }, // L
};

bool finite_all(const PushFitParams &p)
{
    const float v[] = { p.diameter, p.length_upper, p.length_lower, p.end_chamfer, p.lip_height, p.lip_width,
                        p.lip_position, p.flat_depth, p.slot_width, p.slot_length, p.clearance_upper,
                        p.clearance_lower, p.depth_clearance, p.mouth_chamfer };
    for (float x : v)
        if (!std::isfinite(x))
            return false;
    return true;
}

// Total hole depth from the cut face, cone included.
double hole_depth(const PushFitParams &p, PushFitSide s)
{
    const double rh = 0.5 * double(p.diameter) + double(p.clearance(s));
    return double(p.length(s)) + double(p.depth_clearance) + (p.pointed_end ? rh : 0.);
}

// Drop consecutive (and wrap-around) points closer than eps: its_make_revolved would turn them
// into zero-area facets.
std::vector<Vec2d> dedupe(std::vector<Vec2d> pts, double eps = 1e-6)
{
    std::vector<Vec2d> out;
    out.reserve(pts.size());
    for (const Vec2d &q : pts)
        if (out.empty() || (q - out.back()).norm() > eps)
            out.push_back(q);
    while (out.size() > 1 && (out.front() - out.back()).norm() <= eps)
        out.pop_back();
    return out;
}

// The connector frame -> the work frame W used to build the pin: the pin axis (C z) goes to W x,
// the flats' normal (C y) to W z and the slit's normal (C x) to W y. A cyclic permutation, so a
// proper rotation: no winding flip.
inline Vec3f c_to_w(const Vec3f &c) { return Vec3f(c.z(), c.x(), c.y()); }
inline Vec3f w_to_c(const Vec3f &w) { return Vec3f(w.y(), w.z(), w.x()); }

void map_vertices(indexed_triangle_set &its, Vec3f (*f)(const Vec3f &))
{
    for (Vec3f &v : its.vertices)
        v = f(v);
}

// A convex polygon (CCW in the XY plane) extruded from z0 to z1: a closed, outward-facing prism.
indexed_triangle_set extrude_convex(const std::vector<Vec2d> &poly, double z0, double z1)
{
    indexed_triangle_set its;
    const int n = int(poly.size());
    if (n < 3)
        return its;
    for (const Vec2d &q : poly)
        its.vertices.emplace_back(float(q.x()), float(q.y()), float(z0));
    for (const Vec2d &q : poly)
        its.vertices.emplace_back(float(q.x()), float(q.y()), float(z1));
    for (int i = 0; i < n; ++ i) {
        const int i2 = (i + 1) % n;
        its.indices.emplace_back(i, i2, n + i2);
        its.indices.emplace_back(i, n + i2, n + i);
    }
    for (int i = 1; i + 1 < n; ++ i) {
        its.indices.emplace_back(0, i + 1, i);          // bottom, facing -z
        its.indices.emplace_back(n, n + i, n + i + 1);  // top, facing +z
    }
    return its;
}

// One slit in the work frame: a stadium of width w in XY whose round end is centred at x = cx and
// whose open end runs out to x = xe (past the pin tip), extruded through the whole pin height.
indexed_triangle_set slit_prism(double cx, double xe, double w, double half_height)
{
    const int    n_arc = 16;
    const double hw    = 0.5 * w;
    std::vector<Vec2d> poly;
    if (xe > cx) {
        // Round end on the left (-x).
        poly.emplace_back(xe, -hw);
        poly.emplace_back(xe, hw);
        for (int k = 0; k <= n_arc; ++ k) {
            const double a = 0.5 * M_PI + M_PI * double(k) / double(n_arc);
            poly.emplace_back(cx + hw * std::cos(a), hw * std::sin(a));
        }
    } else {
        // Round end on the right (+x).
        poly.emplace_back(xe, hw);
        poly.emplace_back(xe, -hw);
        for (int k = 0; k <= n_arc; ++ k) {
            const double a = -0.5 * M_PI + M_PI * double(k) / double(n_arc);
            poly.emplace_back(cx + hw * std::cos(a), hw * std::sin(a));
        }
    }
    return extrude_convex(dedupe(poly, 1e-9), -half_height, half_height);
}

bool mesh_is_sound(const indexed_triangle_set &its)
{
    return !its.indices.empty() && its_num_open_edges(its) == 0 && its_volume(its) > 0.f;
}

} // namespace

PushFitParams pushfit_apply_preset(PushFitParams p, PushFitPreset preset)
{
    const int i = int(preset);
    if (i < 0 || i > 2)
        return p;
    const PresetRow &r = PRESET_ROWS[i];
    p.diameter        = r.d;
    p.length_upper    = r.len;
    p.length_lower    = r.len;
    p.lip_height      = r.lip_h;
    p.lip_width       = r.lip_w;
    p.flat_depth      = r.flat;
    p.slot_width      = r.slot_w;
    p.slot_length     = r.slot_l;
    p.clearance_upper = r.clr;
    p.clearance_lower = r.clr;
    p.depth_clearance = r.depth_clr;
    p.mouth_chamfer   = r.mouth;
    p.preset          = i;
    return p;
}

PushFitAutoSize pushfit_auto_size(const PushFitParams &p, double inscribed_radius, double thickness_upper,
                                  double thickness_lower)
{
    for (int i = 2; i >= 0; -- i) {
        const PushFitParams q = pushfit_apply_preset(p, PushFitPreset(i));
        if (inscribed_radius > 0. && double(q.diameter) > 0.5 * inscribed_radius)
            continue;
        if (thickness_upper > 0. && hole_depth(q, PushFitSide::Upper) + PUSHFIT_MIN_WALL > thickness_upper)
            continue;
        if (thickness_lower > 0. && hole_depth(q, PushFitSide::Lower) + PUSHFIT_MIN_WALL > thickness_lower)
            continue;
        return { q, true };
    }
    return { pushfit_apply_preset(p, PushFitPreset::S), false };
}

// ------------------------------------------------------------------ print pose

double pushfit_lying_overhang_deg(const PushFitParams &p)
{
    if (!pushfit_cutout_has_flat(p.cutout) || p.diameter <= 0.f)
        return 90.;
    const double d = double(p.diameter);
    const double f = std::clamp(double(p.flat_depth), 0., 0.5 * d);
    return std::asin(std::clamp(1. - 2. * f / d, -1., 1.)) * 180. / M_PI;
}

double pushfit_flat_width(const PushFitParams &p)
{
    if (!pushfit_cutout_has_flat(p.cutout) || p.diameter <= 0.f)
        return 0.;
    const double d = double(p.diameter);
    const double f = std::clamp(double(p.flat_depth), 0., 0.5 * d);
    return 2. * std::sqrt(f * (d - f));
}

bool pushfit_can_lie(const PushFitParams &p, std::string *why)
{
    auto no = [why](const char *msg) {
        if (why)
            *why = msg;
        return false;
    };
    if (!pushfit_cutout_has_flat(p.cutout))
        return no("The pin has no flat to lie on, so it prints upright.");
    // 45 degrees plus a hair, so the exact 0.146 D threshold counts as flat enough.
    if (pushfit_lying_overhang_deg(p) > 45. + 1e-3)
        return no("The flat is too shallow to lie on without supports (it needs about 0.15 x the diameter), "
                  "so the pin prints upright.");
    const double w = pushfit_flat_width(p);
    if (w < std::max(0.5 * double(p.diameter), 1.5))
        return no("The flat is too narrow to be a stable footprint, so the pin prints upright.");
    if (why)
        why->clear();
    return true;
}

PushFitPrintPose pushfit_resolve_pose(const PushFitParams &p)
{
    if (p.pose == PushFitPose::Upright)
        return PushFitPrintPose::Upright;
    return pushfit_can_lie(p) ? PushFitPrintPose::Lying : PushFitPrintPose::Upright;
}

// ------------------------------------------------------------------ profiles

std::vector<Vec2d> pushfit_pin_profile(const PushFitParams &p, double kerf_lo, double kerf_hi)
{
    const double R  = 0.5 * double(p.diameter);
    const double ec = std::clamp(double(p.end_chamfer), 0., 0.5 * R);
    const double zb = -(double(p.length_lower) + kerf_lo);
    const double zt = double(p.length_upper) + kerf_hi;

    std::vector<Vec2d> pts;
    pts.emplace_back(0., zb);
    if (ec > 0.) {
        pts.emplace_back(R - ec, zb);
        pts.emplace_back(R, zb + ec);
    } else
        pts.emplace_back(R, zb);

    auto add_lip = [&](double zc) {
        const double h  = double(p.lip_height);
        const double hw = 0.5 * double(p.lip_width);
        pts.emplace_back(R, zc - hw);
        pts.emplace_back(R + h, zc - hw + h);
        pts.emplace_back(R + h, zc + hw - h);
        pts.emplace_back(R, zc + hw);
    };
    if (p.lip && p.lip_height > 0.f) {
        add_lip(-(kerf_lo + double(p.lip_position) * double(p.length_lower)));
        add_lip(kerf_hi + double(p.lip_position) * double(p.length_upper));
    }

    if (ec > 0.) {
        pts.emplace_back(R, zt - ec);
        pts.emplace_back(R - ec, zt);
    } else
        pts.emplace_back(R, zt);
    pts.emplace_back(0., zt);
    return dedupe(std::move(pts));
}

std::vector<Vec2d> pushfit_hole_profile(const PushFitParams &p, PushFitSide side, double face)
{
    const double R    = 0.5 * double(p.diameter);
    const double c    = std::max(0., double(p.clearance(side)));
    const double rh   = R + c;
    const double L    = double(p.length(side));
    const double mc   = std::max(0., double(p.mouth_chamfer));
    const double lead = PUSHFIT_HOLE_MOUTH_LEAD;
    const double z0   = face - lead;
    const double zend = face + L + std::max(0., double(p.depth_clearance));

    std::vector<Vec2d> pts;
    pts.emplace_back(0., z0);
    if (mc > 0.) {
        // The 45 degree flare carries on through the 0.2 mm lead below the face.
        pts.emplace_back(rh + mc + lead, z0);
        pts.emplace_back(rh, face + mc);
    } else
        pts.emplace_back(rh, z0);
    if (p.lip && p.hole_groove && p.lip_height > 0.f) {
        const double h  = double(p.lip_height);
        const double zl = face + double(p.lip_position) * L;
        const double hw = 0.5 * double(p.lip_width) + c;   // groove base = lip width + 2 x clearance
        pts.emplace_back(rh, zl - hw);
        pts.emplace_back(rh + h, zl - hw + h);
        pts.emplace_back(rh + h, zl + hw - h);
        pts.emplace_back(rh, zl + hw);
    }
    pts.emplace_back(rh, zend);
    pts.emplace_back(0., p.pointed_end ? zend + rh : zend);

    if (side == PushFitSide::Lower)
        for (Vec2d &q : pts)
            q.y() = -q.y();
    return dedupe(std::move(pts));
}

// ------------------------------------------------------------------ meshes

indexed_triangle_set pushfit_hole_mesh(const PushFitParams &p, PushFitSide side, double face, int sectors)
{
    return its_make_revolved(pushfit_hole_profile(p, side, face), sectors);
}

PushFitPinResult pushfit_pin_mesh(const PushFitParams &p, const PushFitPinOptions &opt)
{
    PushFitPinResult res;
    res.pose           = pushfit_resolve_pose(p);
    res.slot_requested = pushfit_cutout_has_slot(p.cutout);

    const double R = 0.5 * double(p.diameter);
    const double h = p.lip ? std::max(0., double(p.lip_height)) : 0.;

    // 1. The solid of revolution, in the connector frame, then into the work frame.
    indexed_triangle_set body = its_make_revolved(pushfit_pin_profile(p, opt.kerf_lo, opt.kerf_hi), opt.sectors);
    map_vertices(body, c_to_w);
    indexed_triangle_set mesh = body;

    // 2. The flats: half-space cuts parallel to the axis. cut_mesh() caps the section, so the
    //    result stays closed with no boolean, and the flat runs through the lips as well.
    if (pushfit_cutout_has_flat(p.cutout) && p.flat_depth > 0.f && double(p.flat_depth) < R) {
        const float a = float(R - double(p.flat_depth));
        indexed_triangle_set up;
        cut_mesh(mesh, -a, &up, nullptr);
        its_compactify_vertices(up);
        mesh = std::move(up);
        if (p.flat_both_sides) {
            indexed_triangle_set lo;
            cut_mesh(mesh, a, nullptr, &lo);
            its_compactify_vertices(lo);
            mesh = std::move(lo);
        }
        if (!mesh_is_sound(mesh)) {
            BOOST_LOG_TRIVIAL(error) << "Push-fit pin: the flatted body is not closed, using the round pin";
            mesh              = body;
            res.plain_fallback = true;
        }
    }

    // 3. The slit: the only boolean. One stadium prism per end, perpendicular to the flats, round
    //    at its inner end so it has no crack starter.
    if (res.slot_requested) {
        const double w      = double(p.slot_width);
        const double ls     = double(p.slot_length);
        const double tip_hi = double(p.length_upper) + opt.kerf_hi;
        const double tip_lo = -(double(p.length_lower) + opt.kerf_lo);
        bool ok = !opt.simulate_slot_boolean_failure && w > 1e-3 && ls > w;
        if (ok) {
            const double half_h = R + h + 1.;
            indexed_triangle_set cutter = slit_prism(tip_hi - ls + 0.5 * w, tip_hi + 1., w, half_h);
            its_merge(cutter, slit_prism(tip_lo + ls - 0.5 * w, tip_lo - 1., w, half_h));
            TriangleMesh out;
            ok = MeshBoolean::make_boolean_chain(TriangleMesh(mesh), TriangleMesh(cutter), "A_NOT_B", out,
                                                 "Push-fit pin");
            if (ok) {
                indexed_triangle_set slit = out.its;
                its_compactify_vertices(slit);
                ok = mesh_is_sound(slit) && its_volume(slit) < its_volume(mesh) - 1e-3f;
                if (ok)
                    mesh = std::move(slit);
                else
                    BOOST_LOG_TRIVIAL(error) << "Push-fit pin: the slit boolean gave a bad mesh, no slit";
            }
        }
        if (!ok)
            BOOST_LOG_TRIVIAL(warning) << "Push-fit pin: slit not made, the pin is built without one";
        res.slot_made = ok;
    }

    // 4. The frame.
    Transform3d c_to_mesh = Transform3d::Identity();
    if (opt.frame == PushFitMeshFrame::Connector || res.pose == PushFitPrintPose::Upright)
        map_vertices(mesh, w_to_c);
    else {
        Matrix3d m;
        m << 0., 0., 1.,
             1., 0., 0.,
             0., 1., 0.;
        c_to_mesh.linear() = m;
    }
    if (opt.frame == PushFitMeshFrame::PrintPose && !mesh.vertices.empty()) {
        BoundingBoxf3 bb;
        for (const Vec3f &v : mesh.vertices)
            bb.merge(v.cast<double>());
        const Vec3d shift(-0.5 * (bb.min.x() + bb.max.x()), -0.5 * (bb.min.y() + bb.max.y()), -bb.min.z());
        const Vec3f sf = shift.cast<float>();
        for (Vec3f &v : mesh.vertices)
            v += sf;
        c_to_mesh.pretranslate(shift);
    }
    res.connector_to_mesh = c_to_mesh;
    res.mesh              = std::move(mesh);
    return res;
}

indexed_triangle_set pushfit_preview_body(const PushFitParams &p)
{
    PushFitPinOptions opt;
    opt.frame = PushFitMeshFrame::Connector;
    return pushfit_pin_mesh(p, opt).mesh;
}

// ------------------------------------------------------------------ validation

std::string pushfit_validate(const PushFitParams &p, double nozzle_diameter)
{
    if (!finite_all(p))
        return "A pin value is not a number.";
    const double D  = double(p.diameter);
    const double R  = 0.5 * D;
    if (D < 3.)
        return "Pin diameter must be at least 3 mm.";
    if (p.length_upper < 2.f || p.length_lower < 2.f)
        return "Pin length must be at least 2 mm on each side.";
    if (p.end_chamfer < 0.f || double(p.end_chamfer) > 0.25 * D)
        return "End chamfer must be between 0 and a quarter of the diameter.";
    if (p.clearance_upper < 0.f || p.clearance_lower < 0.f || p.clearance_upper > 1.f || p.clearance_lower > 1.f)
        return "Clearance must be between 0 and 1 mm.";
    if (p.depth_clearance < 0.f || p.depth_clearance > 5.f)
        return "Depth clearance must be between 0 and 5 mm.";
    if (p.mouth_chamfer < 0.f || double(p.mouth_chamfer) > 0.25 * D)
        return "Mouth chamfer must be between 0 and a quarter of the diameter.";

    const bool  has_flat = pushfit_cutout_has_flat(p.cutout);
    const bool  has_slot = pushfit_cutout_has_slot(p.cutout);
    const float min_len  = std::min(p.length_upper, p.length_lower);

    if (p.lip) {
        if (p.lip_height <= 0.f || double(p.lip_height) >= 0.25 * D)
            return "Lip height must be above 0 and below a quarter of the diameter.";
        if (p.lip_width <= 2.f * p.lip_height + 0.05f)
            return "Lip is too narrow for its height: make it wider than twice its height.";
        if (p.lip_position <= 0.f || p.lip_position >= 1.f)
            return "Lip position must be between 0 and 100% of the length.";
        for (PushFitSide s : { PushFitSide::Upper, PushFitSide::Lower }) {
            const double L  = double(p.length(s));
            const double zc = double(p.lip_position) * L;
            const double hw = 0.5 * double(p.lip_width);
            if (zc + hw > L - double(p.end_chamfer) - 1e-3)
                return "The lip runs into the pin's end chamfer: move it towards the cut or shorten it.";
            if (zc - hw < 1e-3)
                return "The lip reaches the cut plane: move it away from the cut.";
            if (p.hole_groove && zc - hw - double(p.clearance(s)) < double(p.mouth_chamfer) + 1e-3)
                return "The lip groove runs into the hole's mouth chamfer: move the lip away from the cut.";
        }
    }
    if (has_flat) {
        if (p.flat_depth <= 0.f || double(p.flat_depth) >= D / 3.)
            return "Flat depth must be above 0 and below a third of the diameter.";
    }
    if (has_slot) {
        if (p.slot_width < 0.6f)
            return "Slot width must be at least 0.6 mm.";
        const double line  = 1.125 * (nozzle_diameter > 0. ? nozzle_diameter : 0.4);
        const double prong = 0.5 * (D - double(p.slot_width)) - (has_flat ? double(p.flat_depth) : 0.);
        if (prong < 2. * line - 1e-6)
            return "The slot leaves the prongs too thin to print (each needs two lines): make the slot narrower, "
                   "the flat shallower or the pin larger.";
        if (p.slot_length < p.slot_width)
            return "Slot length must be at least the slot width.";
        const float max_len = min_len - (p.lip ? 0.5f * p.lip_width : 0.5f);
        if (p.slot_length > max_len + 1e-4f)
            return "The slot is too long: it must stop short of the cut plane.";
    }
    (void)R;
    return {};
}

std::vector<std::string> pushfit_warnings(const PushFitParams &p, double nozzle_diameter, double thickness_upper,
                                          double thickness_lower)
{
    std::vector<std::string> out;
    const double nozzle = nozzle_diameter > 0. ? nozzle_diameter : 0.4;
    if (thickness_upper > 0. && hole_depth(p, PushFitSide::Upper) + PUSHFIT_MIN_WALL > thickness_upper)
        out.emplace_back("The upper hole is deeper than the upper half is thick here: shorten the pin's upper length.");
    if (thickness_lower > 0. && hole_depth(p, PushFitSide::Lower) + PUSHFIT_MIN_WALL > thickness_lower)
        out.emplace_back("The lower hole is deeper than the lower half is thick here: shorten the pin's lower length.");
    // A separate pin is not print-in-place, so Flexi's 0.75 x nozzle floor does not apply; this is
    // the "it will need force" floor instead.
    const double floor = 0.125 * nozzle;
    if (double(std::min(p.clearance_upper, p.clearance_lower)) < floor - 1e-6)
        out.emplace_back("The clearance is very small for this nozzle: the pin will need force to go in.");
    if (p.lip && !pushfit_cutout_has_slot(p.cutout) && !p.hole_groove) {
        const float interference = p.lip_height - std::min(p.clearance_upper, p.clearance_lower);
        if (interference > 0.15f + 1e-5f)
            out.emplace_back("The lip will need force without a slot or a groove: add a slot or a groove, or lower the lip.");
    }
    return out;
}

} // namespace Slic3r
