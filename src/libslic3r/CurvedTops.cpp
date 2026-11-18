// EdgeSlicer: curved (non-planar) top surfaces, experimental (2026-11-12). See CurvedTops.hpp for
// the credits (fich-l/OrcaSlicer-Nonplanar v0.1, AGPL-3.0; PancakeSlicer, AGPL-3.0, as reference)
// and tests/design_curved_tops.md for the design note.
//
// How it works, in the frame Z contouring already uses (mesh Z = print Z + imesh.ground_level()):
//
//  1. Surfaces. Every facet that faces up within the maximum angle and has nothing above it is a
//     candidate. Their XY projections are unioned; each connected piece is one surface. A surface
//     is kept when it is not flat (rises at least one layer), not tiny, and no taller than the
//     head's nozzle clearance.
//  2. Clearance. Nothing outside the kept surfaces may stand higher than the lowest curved layer,
//     otherwise the head would have to dip next to it. Such objects are refused (left planar).
//  3. Lowered planar body. Inside a surface, planar material is kept only where its layer's top
//     is at least N layers below the surface: keep  print_z <= S(x,y) - N*h. Everything else of
//     those layers (walls, top skins, ironing) is clipped away.
//  4. Conformal layers. N layers are built over each surface (one wall loop plus a rectilinear
//     solid fill, angle alternating) and every point is placed at S(x,y) - (N-1-k)*h. They are
//     appended, in order, to the object's top layer, so they print after all the planar layers.
//     Their Z rides on ContourZSamples with curved_top set: the emitter lifts to a safe Z before
//     each travel, descends vertically, and uses the stored flow (the first conformal layer fills
//     the stepped gap the planar body left, 1 to 2 layer heights).

#include "CurvedTops.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <boost/log/trivial.hpp>

#include "AABBTreeLines.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "ContourZ.hpp"
#include "ExPolygon.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Fill/FillBase.hpp"
#include "Flow.hpp"
#include "HeadClearance.hpp"
#include "I18N.hpp"
#include "Layer.hpp"
#include "LocalesUtils.hpp"
#include "Print.hpp"
#include "format.hpp"
#include "PrintConfig.hpp"
#include "SLA/IndexedMesh.hpp"
#include "Surface.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"
#include "VariableWidth.hpp"
#include "libslic3r.h"

namespace Slic3r {

namespace {

std::string mm_str(double v) { return float_to_string_decimal_point(v, 2); }
std::string deg_str(double v) { return float_to_string_decimal_point(v, 1); }

// "the Bambu Lab X1 Carbon and the Elegoo Centauri Carbon": every printer model a shipped head file
// records a curved print for. Empty when there is none (no resources, as in some unit tests).
std::string verified_printer_list()
{
    std::vector<std::string> models;
    for (const HeadClearancePtr &h : shipped_head_clearances())
        if (h && h->curved_tops_verified)
            for (const std::string &m : h->curved_tops_verified->printer_models)
                if (h->matches_printer_model(m) && std::find(models.begin(), models.end(), m) == models.end())
                    models.push_back(m);
    std::sort(models.begin(), models.end());
    std::string out;
    for (size_t i = 0; i < models.size(); ++i) {
        if (i > 0)
            out += i + 1 == models.size() ? _u8L(" and ") : ", ";
        out += models[i];
    }
    return out;
}

std::string only_on_verified_printers()
{
    const std::string list = verified_printer_list();
    return list.empty() ? std::string() :
                          " " + Slic3r::format(_u8L("For now curved tops runs only on the printers it has been printed on: %1%."), list);
}

std::atomic<bool> g_curved_tops_developer_mode { false };

} // namespace

void set_curved_tops_developer_mode(bool on) { g_curved_tops_developer_mode.store(on); }
bool curved_tops_developer_mode() { return g_curved_tops_developer_mode.load(); }

CurvedTopsDevOverrides curved_tops_dev_overrides(const PrintObjectConfig &config)
{
    return curved_tops_dev_overrides(config, curved_tops_developer_mode());
}

CurvedTopsDevOverrides curved_tops_dev_overrides(const PrintObjectConfig &cfg, bool developer_mode)
{
    CurvedTopsDevOverrides set; // what the config asks for
    const CurvedTopsDevOverrides builtin;
    std::vector<std::string> &list = set.carried_list;
    // A numeric override counts when it differs from its built-in value (its default). Values outside the
    // documented range (a per-object value, a hand-edited file: the GUI fields and validate() check the
    // range, Print::apply() does not) are clamped to it, a value that is not a number falls back to the
    // built-in one; either is said (clamp_notes, shown with the summary).
    std::vector<std::string> &clamped = set.clamp_notes;
    const auto num_str = [](double value) {
        // Up to three decimals, trailing zeros dropped ("25", "0.15").
        std::string v = float_to_string_decimal_point(value, 3);
        if (v.find('.') != std::string::npos) {
            while (!v.empty() && v.back() == '0')
                v.pop_back();
            if (!v.empty() && v.back() == '.')
                v.pop_back();
        }
        return v;
    };
    const auto num = [&list, &clamped, &num_str](const char *key, const ConfigOptionFloat &opt, double builtin_value, double &dst,
                                                 const char *name, const char *unit) {
        double value = opt.value;
        if (!std::isfinite(value)) {
            clamped.push_back(Slic3r::format(_u8L("Developer override %1% is not a number; the built-in %2%%3% is used."), name,
                                             num_str(builtin_value), unit));
            return;
        }
        if (const ConfigOptionDef *def = print_config_def.get(key); def != nullptr) {
            const double v = std::clamp(value, double(def->min), double(def->max));
            if (v != value) {
                clamped.push_back(Slic3r::format(_u8L("Developer override %1% %2%%3% is outside %4% to %5%; %6%%3% is used."), name,
                                                 num_str(value), unit, num_str(def->min), num_str(def->max), num_str(v)));
                value = v;
            }
        }
        if (std::abs(value - builtin_value) > 1e-9) {
            dst = value;
            list.push_back(std::string(name) + " " + num_str(value) + unit);
        }
    };
    const auto flag = [&list](const ConfigOptionBool &opt, bool &dst, const char *name) {
        if (opt.value) {
            dst = true;
            list.push_back(name);
        }
    };
    num("curved_tops_dev_angle_ceiling", cfg.curved_tops_dev_angle_ceiling, builtin.angle_ceiling_deg, set.angle_ceiling_deg, "angle ceiling", " deg");
    num("curved_tops_dev_head_lowest_part", cfg.curved_tops_dev_head_lowest_part, builtin.head_lowest_part_mm, set.head_lowest_part_mm, "lowest head part", " mm");
    num("curved_tops_dev_clearance_margin", cfg.curved_tops_dev_clearance_margin, builtin.clearance_margin_mm, set.clearance_margin_mm, "clearance margin", " mm");
    num("curved_tops_dev_flank_margin", cfg.curved_tops_dev_flank_margin, builtin.flank_margin_deg, set.flank_margin_deg, "flank margin", " deg");
    num("curved_tops_dev_land_tolerance", cfg.curved_tops_dev_land_tolerance, builtin.land_tolerance_mm, set.land_tolerance_mm, "land tolerance", " mm");
    num("curved_tops_dev_reach_margin", cfg.curved_tops_dev_reach_margin, builtin.reach_margin_mm, set.reach_margin_mm, "reach margin", " mm");
    num("curved_tops_dev_speed_cap", cfg.curved_tops_dev_speed_cap, builtin.speed_cap_mm_s, set.speed_cap_mm_s, "curved speed cap", " mm/s");
    num("curved_tops_dev_wipe_speed_cap", cfg.curved_tops_dev_wipe_speed_cap, builtin.wipe_speed_cap_mm_s, set.wipe_speed_cap_mm_s, "wipe speed cap", " mm/s");
    num("curved_tops_dev_safe_lift", cfg.curved_tops_dev_safe_lift, builtin.safe_lift_mm, set.safe_lift_mm, "travel/exit lift", " mm");
    flag(cfg.curved_tops_dev_contact_warning_only, set.contact_warning_only, "head contact is a warning only");
    flag(cfg.curved_tops_dev_allow_multiple_objects, set.allow_multiple_objects, "allow several objects or copies");
    flag(cfg.curved_tops_dev_allow_by_object, set.allow_by_object, "allow by-object sequence");
    flag(cfg.curved_tops_dev_allow_multi_filament, set.allow_multi_filament, "allow several filaments or a prime tower");
    flag(cfg.curved_tops_dev_allow_supports_in_reach, set.allow_supports_in_reach, "allow supports within reach");
    flag(cfg.curved_tops_dev_allow_material_beside, set.allow_material_beside, "allow material beside the surface (the lowest curved layers pass through the walls beside the surface; expect nozzle contact)");
    flag(cfg.curved_tops_dev_allow_z_contouring, set.allow_z_contouring, "allow Z contouring");
    flag(cfg.curved_tops_dev_allow_raft, set.allow_raft, "allow raft");
    flag(cfg.curved_tops_dev_allow_variable_layer_height, set.allow_variable_layer_height, "allow variable layer height");
    flag(cfg.curved_tops_dev_allow_tall_skirt, set.allow_tall_skirt, "allow draft shield or tall skirt");
    flag(cfg.curved_tops_dev_allow_unverified_head, set.allow_unverified_head, "allow unverified or missing head data");
    flag(cfg.curved_tops_dev_nozzle_tolerance_unrecorded, set.nozzle_tolerance_unrecorded,
         "apply the nozzle contact tolerance without a printed record");

    if (list.empty() && clamped.empty())
        return builtin;
    if (!developer_mode) {
        // Ignored: the built-in values, but say what was carried.
        CurvedTopsDevOverrides out;
        out.carried      = true;
        out.carried_list = std::move(list);
        if (out.carried_list.empty())
            out.carried_list.push_back(_u8L("an override value that is not valid"));
        return out;
    }
    set.carried = true;
    set.active  = true;
    return set;
}

std::string CurvedTopsDevOverrides::record() const
{
    if (!this->carried)
        return std::string();
    std::string out = this->active ? "active:" : "ignored:";
    for (const std::string &s : this->carried_list)
        out += s + "|";
    for (const std::string &s : this->clamp_notes)
        out += s + "|";
    return out;
}

std::string CurvedTopsDevOverrides::active_message() const
{
    if (!this->active)
        return std::string();
    std::string out = _u8L("Curved tops developer overrides active:");
    for (size_t i = 0; i < this->carried_list.size(); ++i)
        out += (i == 0 ? " " : "; ") + this->carried_list[i];
    return out + ". " + _u8L("The safety limits these replace were not verified on a print. Watch the print.");
}

std::string CurvedTopsDevOverrides::ignored_note() const
{
    if (!this->carried || this->active)
        return std::string();
    std::string names;
    for (size_t i = 0; i < this->carried_list.size(); ++i)
        names += (i == 0 ? "" : "; ") + this->carried_list[i];
    return Slic3r::format(_u8L("The developer overrides in this project were ignored (Developer mode is off): %1%."), names);
}

CurvedTopsHeadLimits curved_tops_head_limits(const std::shared_ptr<const HeadClearance> &head, const std::string &printer_model,
                                             const CurvedTopsDevOverrides &dev)
{
    CurvedTopsHeadLimits out;
    out.head                = head;
    out.angle_ceiling_deg   = dev.angle_ceiling_deg;
    out.clearance_margin_mm = dev.clearance_margin_mm;
    if (!head) {
        out.refusal = Slic3r::format(_u8L("EdgeSlicer has no print head clearance data for the printer model \"%1%\"."), printer_model) +
                      only_on_verified_printers();
        // P3: no head data at all, curved on the developer's own numbers. The angle is then limited by
        // the ceiling only (there is no nozzle outline), and nothing can check the export.
        if (dev.active && dev.allow_unverified_head) {
            if (dev.head_lowest_part_mm <= 0.)
                out.refusal += " " + _u8L("The developer override for missing head data needs the lowest head part above the tip.");
            else if (dev.head_lowest_part_mm - dev.clearance_margin_mm <= 0.)
                out.refusal += " " + _u8L("The developer override leaves no room: the lowest head part is not above the clearance margin.");
            else {
                out.refusal.clear();
                out.usable          = true;
                out.allowed_rise_mm = dev.head_lowest_part_mm - dev.clearance_margin_mm;
                out.max_slope_deg   = dev.angle_ceiling_deg;
                out.source          = _u8L("no head data: developer override values, not checked");
            }
        }
        return out;
    }
    out.known  = true;
    out.source = head->name + (head->calibrated ? std::string() : " (" + _u8L("an estimate, not a measurement") + ")");

    // Allowed rise: the lowest head part above the tip, less the margin.
    const double lowest = dev.head_lowest_part_mm > 0. ? dev.head_lowest_part_mm : head->lowest_box_z();
    out.allowed_rise_mm = std::isfinite(lowest) ? lowest - dev.clearance_margin_mm : 0.;
    // Head angle cap: the nozzle flank, less a margin, never above the ceiling.
    out.max_slope_deg = std::clamp(head->nozzle_flank_angle_deg(dev.land_tolerance_mm) - dev.flank_margin_deg, 0.,
                                   dev.angle_ceiling_deg);

    const bool verified = head->curved_tops_verified_for(printer_model);
    if (verified) {
        out.verified_rise_mm   = head->curved_tops_verified->rise_mm;
        out.verified_angle_deg = head->curved_tops_verified->angle_deg;
        out.verified_nozzle_contact_mm = head->curved_tops_verified->nozzle_contact_mm;
        out.verified_on        = head->curved_tops_verified->on;
    }

    if (!std::isfinite(lowest))
        out.refusal = Slic3r::format(_u8L("the print head data for %1% (%2%) has no head parts above the nozzle, so the room for a curved "
                                          "surface is unknown."),
                                     printer_model, head->name);
    else if (out.allowed_rise_mm <= 0.)
        out.refusal = Slic3r::format(_u8L("the print head data for %1% (%2%) leaves no room for a curved surface: its lowest part is "
                                          "%3% mm above the nozzle tip."),
                                     printer_model, head->name, mm_str(lowest));
    else if (out.max_slope_deg <= 0.)
        out.refusal = Slic3r::format(_u8L("the nozzle outline in the print head data for %1% (%2%) clears no slope."), printer_model,
                                     head->name);
    else if (!verified && !(dev.active && dev.allow_unverified_head))
        // P1: an estimated head nobody has printed curved tops with stays refused until the
        // acknowledgement (P2) lets the user take that risk knowingly.
        out.refusal = Slic3r::format(_u8L("the print head data for %1% (%2%) is an estimate that no curved print has checked yet."),
                                     printer_model, head->name) +
                      only_on_verified_printers();
    else {
        out.usable = true;
        if (!verified)
            out.source += ", " + _u8L("no curved print checked it (developer override)");
    }
    return out;
}

CurvedTopsHeadLimits curved_tops_head_limits(const PrintConfig &config, const CurvedTopsDevOverrides &dev)
{
    return curved_tops_head_limits(resolve_head_clearance(config), config.printer_model.value, dev);
}

double curved_tops_effective_max_angle(double user_max_deg, const CurvedTopsHeadLimits &head)
{
    return std::clamp(std::min(user_max_deg, head.max_slope_deg), 0.0, head.angle_ceiling_deg);
}

double curved_tops_head_reach_mm(const HeadClearance &head, double height)
{
    const auto &profile = head.nozzle_profile;
    double      reach   = profile.empty() ? 0. : profile.front().second;
    // Nozzle: the outline is a cone between profile points; its underside is below `height` out to
    // where the cone reaches that height, and nothing of the nozzle is beyond the last radius.
    for (size_t i = 0; i + 1 < profile.size(); ++i) {
        const auto &[h0, r0] = profile[i];
        const auto &[h1, r1] = profile[i + 1];
        if (height >= h1)
            reach = r1;
        else {
            if (height > h0)
                reach = r0 + (r1 - r0) * (height - h0) / (h1 - h0);
            break;
        }
    }
    for (const HeadClearanceBox &box : head.boxes) {
        if (box.min.z() >= height)
            continue;
        if (box.spans_x)
            return std::numeric_limits<double>::infinity();
        for (const double x : { box.min.x(), box.max.x() })
            for (const double y : { box.min.y(), box.max.y() })
                reach = std::max(reach, std::hypot(x, y));
    }
    return reach;
}

double curved_tops_gap_flow(double gap, double height)
{
    if (height <= 1e-9)
        return 1.0;
    return std::clamp(gap / height, 1.0, 2.0);
}

namespace {

// The part of the triangle p[0..2] where the linear function given by d at its corners is <= 0, in order
// (a convex polygon of 3 or 4 points, fewer when nothing is left).
std::vector<Vec3d> clip_triangle(const Vec3d p[3], const double d[3])
{
    std::vector<Vec3d> out;
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        if (d[i] <= 0.)
            out.push_back(p[i]);
        if ((d[i] < 0. && d[j] > 0.) || (d[i] > 0. && d[j] < 0.)) {
            const double t = d[i] / (d[i] - d[j]);
            out.push_back(p[i] + (p[j] - p[i]) * t);
        }
    }
    return out;
}

constexpr double GOLDEN_ANGLE_DEG = 137.50776405;

} // namespace

CurvedTopsCandidates curved_tops_candidate_surfaces(const indexed_triangle_set &its, const sla::IndexedMesh &imesh, double max_angle_deg,
                                                    double ground, bool smooth_edge)
{
    CurvedTopsCandidates out;
    const size_t         nf = its.indices.size();
    std::vector<Vec3d>   normal(nf, Vec3d::Zero());
    std::vector<double>  weight(nf, 0.); // twice the area
    for (size_t i = 0; i < nf; ++i) {
        const stl_triangle_vertex_indices &f = its.indices[i];
        const Vec3d a = its.vertices[f[0]].cast<double>();
        const Vec3d b = its.vertices[f[1]].cast<double>();
        const Vec3d c = its.vertices[f[2]].cast<double>();
        const Vec3d n = (b - a).cross(c - a);
        const double len = n.norm();
        if (len > 1e-12) {
            normal[i] = n / len;
            weight[i] = len;
        }
    }
    // A facet may contribute when it faces up within the limit (P3), or (smooth edge) within the limit plus
    // the edge tolerance; how much of it is then decided by the interpolated slope.
    const double gate_deg   = smooth_edge ? std::min(max_angle_deg + CURVED_TOPS_EDGE_ANGLE_TOLERANCE_DEG, 89.) : max_angle_deg;
    const double cos_gate   = std::cos(gate_deg * M_PI / 180.);
    const double cos_crease = std::cos(CURVED_TOPS_CREASE_DEG * M_PI / 180.);
    std::vector<std::vector<int>> around;
    if (smooth_edge) {
        around.assign(its.vertices.size(), {});
        for (size_t i = 0; i < nf; ++i)
            if (weight[i] > 0.)
                for (int c = 0; c < 3; ++c)
                    around[size_t(its.indices[i][c])].push_back(int(i));
    }

    Polygons tris;
    for (size_t i = 0; i < nf; ++i) {
        const Vec3d &n = normal[i];
        if (weight[i] <= 0. || n.z() < cos_gate)
            continue;
        const stl_triangle_vertex_indices &f = its.indices[i];
        const Vec3d p[3] = { its.vertices[f[0]].cast<double>(), its.vertices[f[1]].cast<double>(), its.vertices[f[2]].cast<double>() };
        std::vector<Vec3d> part;
        if (smooth_edge) {
            double d[3];
            for (int c = 0; c < 3; ++c) {
                // The surface's slope at this corner: the facets round the vertex on this side of a crease.
                Vec3d sum = Vec3d::Zero();
                for (const int g : around[size_t(f[c])])
                    if (normal[size_t(g)].dot(n) >= cos_crease)
                        sum += normal[size_t(g)] * weight[size_t(g)];
                const double len = sum.norm();
                const double nz  = len > 1e-12 ? sum.z() / len : n.z();
                d[c] = std::acos(std::clamp(nz, -1., 1.)) * 180. / M_PI - max_angle_deg;
            }
            part = clip_triangle(p, d);
        } else
            part.assign(p, p + 3);
        if (part.size() < 3)
            continue;
        Vec3d centroid = Vec3d::Zero();
        for (const Vec3d &v : part)
            centroid += v;
        centroid /= double(part.size());
        if (imesh.query_ray_hit(centroid + Vec3d(0., 0., 1e-3), {0.0, 0.0, 1.0}).is_hit())
            continue; // something above it: not a top surface
        CurvedTopsFacet facet;
        facet.z_min = std::numeric_limits<double>::max();
        facet.z_max = -std::numeric_limits<double>::max();
        for (const Vec3d &v : part) {
            const Point q = Point::new_scale(v.x(), v.y());
            if (facet.xy.points.empty() || facet.xy.points.back() != q)
                facet.xy.points.push_back(q);
            facet.z_min = std::min(facet.z_min, v.z() - ground);
            facet.z_max = std::max(facet.z_max, v.z() - ground);
        }
        if (facet.xy.points.size() > 1 && facet.xy.points.front() == facet.xy.points.back())
            facet.xy.points.pop_back();
        if (facet.xy.points.size() < 3 || std::abs(facet.xy.area()) < 1.)
            continue;
        if (!facet.xy.is_counter_clockwise())
            facet.xy.reverse();
        facet.probe     = Point::new_scale(centroid.x(), centroid.y());
        facet.slope_deg = std::acos(std::clamp(n.z(), -1., 1.)) * 180. / M_PI;
        tris.push_back(facet.xy);
        out.facets.push_back(std::move(facet));
    }
    if (tris.empty())
        return out;
    // A tiny closing heals the cracks between the projected facets.
    ExPolygons pieces = offset2_ex(union_ex(tris), scale_(0.01), -scale_(0.01));
    if (smooth_edge) {
        // Opening takes nothing but slivers narrower than a line; the simplification moves the edge by at
        // most CURVED_TOPS_EDGE_SIMPLIFY_MM.
        pieces = opening_ex(pieces, float(scale_(CURVED_TOPS_EDGE_OPENING_MM)));
        pieces = union_ex(expolygons_simplify(pieces, scale_(CURVED_TOPS_EDGE_SIMPLIFY_MM)));
    }
    out.pieces = std::move(pieces);
    return out;
}

size_t curved_tops_seam_index(const Points &loop, const Point & /* centre */, int k, int index)
{
    const size_t n = loop.size();
    if (n < 2)
        return 0;
    // A fixed anchor, so the same loop shape gives the same start whatever its first point.
    size_t anchor = 0;
    for (size_t i = 1; i < n; ++i)
        if (loop[i].x() < loop[anchor].x() || (loop[i].x() == loop[anchor].x() && loop[i].y() < loop[anchor].y()))
            anchor = i;
    std::vector<double> at(n + 1, 0.);
    for (size_t j = 0; j < n; ++j)
        at[j + 1] = at[j] + (loop[(anchor + j + 1) % n] - loop[(anchor + j) % n]).cast<double>().norm();
    const double total = at[n];
    if (total <= 0.)
        return anchor;
    double f = (double(k) * 3. + double(index)) * 0.6180339887498949;
    f -= std::floor(f);
    const double target = f * total;
    size_t best = 0;
    for (size_t j = 1; j < n; ++j)
        if (std::abs(at[j] - target) < std::abs(at[best] - target))
            best = j;
    return (anchor + best) % n;
}

size_t curved_tops_path_starts(const std::vector<CurvedTopsFillPath> &paths)
{
    size_t out = 0;
    for (size_t i = 0; i < paths.size(); ++i)
        if (paths[i].points.empty() || i == 0 || paths[i - 1].points.empty() || paths[i - 1].points.back() != paths[i].points.front())
            ++out;
    return out;
}

// ---- P5: the Spiral pattern ----

namespace {

// The insets are densified to this before a spiral runs over them, so each turn moves inward in small steps.
constexpr double SPIRAL_STEP_MM = 0.25;
// Shortest ring a spiral runs on.
constexpr double SPIRAL_MIN_RING_MM = 1.0;

// A closed loop (one inset), densified, first point not repeated, with its arc length and a nearest-point index.
// Spiral geometry is in scaled units held as doubles.
class SpiralLoop
{
public:
    explicit SpiralLoop(const Polygon &poly)
    {
        const double  step = scale_(SPIRAL_STEP_MM);
        const Points &src  = poly.points;
        for (size_t i = 0; i < src.size(); ++i) {
            const Vec2d a = src[i].cast<double>();
            const Vec2d b = src[(i + 1) % src.size()].cast<double>();
            const int   n = std::max(1, int(std::ceil((b - a).norm() / step)));
            for (int j = 0; j < n; ++j) {
                const Vec2d q = a + (b - a) * (double(j) / double(n));
                const Point pt(coord_t(std::llround(q.x())), coord_t(std::llround(q.y())));
                if (pts.empty() || pt != pts.back())
                    pts.push_back(pt);
            }
        }
        while (pts.size() > 1 && pts.front() == pts.back())
            pts.pop_back();
        cum.assign(pts.size() + 1, 0.);
        Lines lines;
        lines.reserve(pts.size());
        for (size_t i = 0; i < pts.size(); ++i) {
            const Point &b = pts[(i + 1) % pts.size()];
            cum[i + 1] = cum[i] + (b - pts[i]).cast<double>().norm();
            lines.emplace_back(pts[i], b);
        }
        length = cum.back();
        if (pts.size() >= 3)
            dist = std::make_unique<AABBTreeLines::LinesDistancer<Line>>(std::move(lines));
    }
    // Review S1: a ring shorter than SPIRAL_MIN_RING_MM is not run (the spiral ends on the ring before, and the gap fill
    // closes the middle), so the last turn never shrinks to a fraction of a millimetre at the apex.
    bool valid() const { return pts.size() >= 3 && length > scale_(SPIRAL_MIN_RING_MM) && dist != nullptr; }
    // The point of the loop nearest p and its arc position from pts[0].
    void nearest(const Vec2d &p, double &arc, Vec2d &q) const
    {
        const auto [d, idx, np] = dist->distance_from_lines_extra<false>(Point(coord_t(std::llround(p.x())), coord_t(std::llround(p.y()))));
        (void) d;
        q   = np;
        arc = idx < pts.size() ? cum[idx] + (np - pts[idx].cast<double>()).norm() : 0.;
    }
    // The point at arc position `arc` (wrapped round the loop).
    Vec2d at(double arc) const
    {
        arc -= std::floor(arc / length) * length;
        size_t i = size_t(std::upper_bound(cum.begin(), cum.end(), arc) - cum.begin());
        i        = i == 0 ? 0 : std::min(i - 1, pts.size() - 1);
        const double seg = cum[i + 1] - cum[i];
        const Vec2d  a   = pts[i].cast<double>();
        const Vec2d  b   = pts[(i + 1) % pts.size()].cast<double>();
        return seg > 0. ? Vec2d(a + (b - a) * ((arc - cum[i]) / seg)) : a;
    }

    Points                                               pts;
    std::vector<double>                                  cum;
    double                                               length { 0. };
    std::unique_ptr<AABBTreeLines::LinesDistancer<Line>> dist;
};

// A loop as a spiral runs it: from its start round to the start again (front == back), with the turn's progress
// theta at each point (0 at the start, 1 back at it, never decreasing). The progress is carried from ring to ring
// along the nearest points (spiral_step), so the turns on either side of a ring agree on it there and run one
// spacing apart, however much shorter the inner ring is.
struct SpiralRing
{
    std::vector<Vec2d>  pts;
    std::vector<double> theta;
};

// Ring points of `loop` after arc position `start` (exclusive), in order, with their position from it.
std::vector<std::pair<double, size_t>> spiral_vertices_after(const SpiralLoop &loop, double start)
{
    std::vector<std::pair<double, size_t>> out;
    const double L = loop.length;
    for (size_t j = 0; j < loop.pts.size(); ++j) {
        double rel = loop.cum[j] - start;
        rel -= std::floor(rel / L) * L;
        if (rel > 1. && rel < L - 1.)
            out.emplace_back(rel, j);
    }
    std::sort(out.begin(), out.end());
    return out;
}

SpiralRing spiral_ring_from(const SpiralLoop &loop, double start)
{
    SpiralRing out;
    out.pts.push_back(loop.at(start));
    out.theta.push_back(0.);
    for (const auto &[rel, j] : spiral_vertices_after(loop, start)) {
        out.pts.push_back(loop.pts[j].cast<double>());
        out.theta.push_back(rel / loop.length);
    }
    out.pts.push_back(out.pts.front());
    out.theta.push_back(1.);
    return out;
}

// One turn from ring A to loop B: for every point of A the point of B the turn heads for (its nearest point, taken in
// order round B from where A's start lands, so it never runs backward), and B as the next ring, its progress taken over
// from A.
struct SpiralStep
{
    std::vector<Vec2d> target;
    SpiralRing         next;
};

SpiralStep spiral_step(const SpiralRing &A, const SpiralLoop &B)
{
    SpiralStep          out;
    const size_t        n = A.pts.size();
    const double        L = B.length;
    std::vector<double> raw(n, 0.);
    Vec2d               q;
    for (size_t i = 0; i < n; ++i)
        B.nearest(A.pts[i], raw[i], q);
    const double        start = raw[0];
    std::vector<double> s(n, 0.);
    for (size_t i = 1; i < n; ++i) {
        double rel = raw[i] - start;
        rel -= std::floor(rel / L) * L;
        const double c = rel + std::round((s[i - 1] - rel) / L) * L;
        s[i]           = std::clamp(std::max(s[i - 1], c), 0., L);
    }
    s[n - 1] = L;
    out.target.reserve(n);
    for (size_t i = 0; i < n; ++i)
        out.target.push_back(B.at(start + s[i]));
    SpiralRing &next = out.next;
    next.pts.push_back(B.at(start));
    next.theta.push_back(0.);
    size_t i = 0;
    for (const auto &[r, j] : spiral_vertices_after(B, start)) {
        while (i + 1 < n && s[i + 1] <= r)
            ++i;
        double theta = A.theta[i];
        if (i + 1 < n && s[i + 1] > s[i])
            theta += (A.theta[i + 1] - A.theta[i]) * std::clamp((r - s[i]) / (s[i + 1] - s[i]), 0., 1.);
        next.pts.push_back(B.pts[j].cast<double>());
        next.theta.push_back(std::clamp(std::max(theta, next.theta.back()), 0., 1.));
    }
    next.pts.push_back(next.pts.front());
    next.theta.push_back(1.);
    return out;
}

// Distance from ring point j along the unit vector n to the first crossing of the closed ring (front == back) other than
// the two segments at j, at most max_len.
double spiral_ray_hit(const std::vector<Vec2d> &ring, size_t j, const Vec2d &n, double max_len)
{
    const Vec2d &p    = ring[j];
    const size_t segs = ring.size() - 1;
    double       best = max_len;
    for (size_t e = 0; e < segs; ++e) {
        const Vec2d &a = ring[e];
        const Vec2d &b = ring[e + 1];
        if (e == j % segs || (e + 1) % segs == j % segs || (a - p).norm() < 1. || (b - p).norm() < 1.)
            continue;
        const Vec2d  d   = b - a;
        const double den = n.x() * d.y() - n.y() * d.x();
        if (std::abs(den) < 1e-12)
            continue;
        const Vec2d  w = a - p;
        const double t = (w.x() * d.y() - w.y() * d.x()) / den;
        const double u = (w.x() * n.y() - w.y() * n.x()) / den;
        if (t > 0. && t < best && u >= 0. && u <= 1.)
            best = t;
    }
    return best;
}

// A point of a spiral, with the width of the strip the bead ending there newly covers (its spacing, scaled) and
// whether it belongs to the wall lap the spiral leads in from.
struct SpiralVertex
{
    Vec2d  p;
    double strip;
    bool   wall;
};

// One spiral over the insets rings[0..m] of one region, outermost first, one spacing `s` apart (scaled). With `lead`
// (a wall loop lead_distance outside rings[0], its bead lead_spacing wide) the spiral starts with the wall's lap and
// goes on from where the lap ends; without, with a full lap on rings[0]. Then:
//  - the first turn runs from the lap to the next ring, covering what lies between the lap's bead and the next turn's:
//    its flow rises from nothing at the start to a whole bead at its end;
//  - every further turn moves from its ring to the next over its whole length (by the progress theta), one spacing
//    from the turns on either side all round;
//  - the last turn runs on the innermost ring and moves inward by up to half a spacing (never past the ring's other
//    side) while its flow falls to nothing, covering exactly what the turn outside it leaves.
// The start is the layer's seam point on the first loop (curved_tops_seam_index with k and index, so it rotates from
// layer to layer); of up to eight candidates the first whose start lands on every ring within 1.75 spacings of the
// one before is taken (a start at the tip of a narrowing limb would cut across it from ring to ring).
std::vector<SpiralVertex> spiral_unit(const Polygon *lead, double lead_spacing, double lead_distance, const Polygons &rings, double s,
                                      int k, int index)
{
    std::vector<SpiralVertex> out;
    std::vector<SpiralLoop>   loops;
    loops.reserve(rings.size() + 1);
    if (lead != nullptr)
        loops.emplace_back(*lead);
    for (const Polygon &ring : rings) {
        loops.emplace_back(ring);
        if (!loops.back().valid()) {
            loops.pop_back();
            break; // a ring too small to run: the spiral ends on the one before
        }
    }
    if (loops.empty() || !loops.front().valid())
        return out;
    const bool has_lead = lead != nullptr;
    const auto expected = [&](size_t i) { return has_lead && i == 0 ? lead_distance : s; };

    const SpiralLoop &first    = loops.front();
    double            best_arc = 0.;
    size_t            best_bad = std::numeric_limits<size_t>::max();
    for (int attempt = 0; attempt < 8 && best_bad > 0; ++attempt) {
        const size_t idx = curved_tops_seam_index(first.pts, Point(0, 0), k, index + 7 * attempt);
        const double arc = first.cum[idx];
        Vec2d        p   = first.at(arc);
        size_t       bad = 0;
        for (size_t i = 0; i + 1 < loops.size(); ++i) {
            double a;
            Vec2d  q;
            loops[i + 1].nearest(p, a, q);
            bad += (q - p).norm() > 1.75 * expected(i) ? 1 : 0;
            p = q;
        }
        if (bad < best_bad) {
            best_bad = bad;
            best_arc = arc;
        }
    }
    SpiralRing   ring        = spiral_ring_from(first, best_arc);
    const double first_strip = has_lead ? lead_spacing : s;
    for (const Vec2d &p : ring.pts)
        out.push_back({ p, first_strip, has_lead });
    if (loops.size() == 1)
        return out;
    const double h0 = 0.5 * first_strip;
    for (size_t i = 0; i + 1 < loops.size(); ++i) {
        const SpiralStep step = spiral_step(ring, loops[i + 1]);
        // Review M1: where the next ring has no part opposite (it ends short of this ring's tip in a narrowing limb), the
        // nearest point of it lies along the limb, not across. The turn then does not move toward it: its move fades
        // out between 1.5 and 2 expected distances, so the turn stays on its own ring there (the rings have no slivers,
        // spiral_chains(), so its two sides lie at least a spacing apart) and the variable-width gap fill takes the
        // limb's spine. (P5's first version pulled both sides onto the spine, laying beads on top of each other.)
        // There the turn instead steps inward across its own ring (along its inward normal) as far as the turn outside it
        // closes in, its bead no wider than the room left up to the middle between the ring's two sides (`across`, the
        // distance to the other side), so the two sides' beads meet in the middle and never lie on top of each other.
        const double e_i = expected(i);
        const size_t n_i = ring.pts.size();
        for (size_t j = 1; j < n_i; ++j) {
            const Vec2d &a     = ring.pts[j];
            const Vec2d  v     = step.target[j] - a;
            const double d     = v.norm();
            const double theta = ring.theta[j];
            const double w     = std::clamp((2. * e_i - d) / (0.5 * e_i), 0., 1.);
            // The regular move (toward the next ring) and its strip.
            Vec2d  p_reg;
            double strip_reg;
            if (i == 0) {
                // At a corner of the lap the next ring lies farther off; the bead stays at most 1.1 spacings wide there
                // and the gap fill takes the rest of the corner.
                const double lo = h0;
                const double hi = std::clamp(d - 0.5 * s + theta * s, lo, lo + 1.1 * s);
                p_reg           = d > 1. ? Vec2d(a + v * (0.5 * (lo + hi) / d)) : a;
                strip_reg       = hi - lo;
            } else {
                p_reg     = a + v * theta;
                strip_reg = s;
            }
            if (w >= 1.) {
                out.push_back({ p_reg, strip_reg, false });
                continue;
            }
            // The tip zone: inward across the own ring.
            Vec2d        p_tip     = a;
            double       strip_tip = 0.;
            const Vec2d  t         = ring.pts[j + 1 < n_i ? j + 1 : 1] - ring.pts[j - 1];
            if (t.norm() > 1.) {
                const Vec2d  nrm    = Vec2d(-t.y(), t.x()) / t.norm();
                const double across = spiral_ray_hit(ring.pts, j, nrm, 4. * s);
                // Covered so far on this side: the lap's bead (first turn) or the turn outside, which has closed in to
                // theta of a spacing from this ring.
                const double lo  = i == 0 ? h0 : theta * s - 0.5 * s;
                const double hi  = std::min(0.5 * across, lo + (i == 0 ? 1.1 : 1.0) * s);
                strip_tip        = std::max(hi - lo, 0.);
                p_tip            = a + nrm * std::clamp(0.5 * (lo + hi), 0., 0.5 * across);
            }
            out.push_back({ p_reg * w + p_tip * (1. - w), strip_reg * w + strip_tip * (1. - w), false });
        }
        ring = step.next;
    }
    const size_t n = ring.pts.size();
    for (size_t j = 1; j < n; ++j) {
        const double theta = ring.theta[j];
        const Vec2d &a     = ring.pts[j];
        const Vec2d  t     = ring.pts[j + 1 < n ? j + 1 : 1] - ring.pts[j - 1];
        Vec2d        p     = a;
        if (t.norm() > 1.) {
            // Left of the direction of travel is inside: contours run counter-clockwise, holes clockwise.
            const Vec2d  nrm = Vec2d(-t.y(), t.x()) / t.norm();
            const double hit = spiral_ray_hit(ring.pts, j, nrm, s);
            p                = a + nrm * std::min(0.5 * theta * s, 0.45 * hit);
        }
        out.push_back({ p, (1. - theta) * s, false });
    }
    return out;
}

// The spiral's points as paths: consecutive pieces sharing their end points (no travel between them), each with one
// width: the wall lap at the wall's flow, whole turns at the fill's, the rising and falling turns in steps of under a
// tenth of a spacing (flow from the strip each covers, as for any bead: spacing x layer height).
void spiral_append_paths(const std::vector<SpiralVertex> &v, const Flow &wall_flow, const Flow &solid_flow,
                         std::vector<CurvedTopsFillPath> &out)
{
    if (v.size() < 2)
        return;
    const double h   = solid_flow.height();
    const double s   = solid_flow.scaled_spacing();
    const double tol = 0.08 * s;
    size_t       st  = 0;
    while (st + 1 < v.size()) {
        // Segment i runs from v[i] to v[i + 1] and takes v[i + 1]'s strip and wall flag.
        const bool   wall   = v[st + 1].wall;
        const double strip0 = v[st + 1].strip;
        size_t       e      = st + 1;
        while (e + 1 < v.size() && v[e + 1].wall == wall && (wall || std::abs(v[e + 1].strip - strip0) <= tol))
            ++e;
        double len = 0., sum = 0.;
        Points pts;
        pts.reserve(e - st + 1);
        for (size_t i = st; i <= e; ++i) {
            const Point q(coord_t(std::llround(v[i].p.x())), coord_t(std::llround(v[i].p.y())));
            if (pts.empty() || q != pts.back())
                pts.push_back(q);
            if (i > st) {
                const double l = (v[i].p - v[i - 1].p).norm();
                len += l;
                sum += l * v[i].strip;
            }
        }
        st = e;
        if (pts.size() < 2)
            continue;
        CurvedTopsFillPath p;
        p.points = std::move(pts);
        if (wall) {
            p.width      = float(wall_flow.width());
            p.mm3_per_mm = wall_flow.mm3_per_mm();
            p.wall       = true;
        } else {
            const double strip = len > 0. ? sum / len : strip0;
            if (std::abs(strip - s) <= 0.02 * s) {
                p.width      = float(solid_flow.width());
                p.mm3_per_mm = solid_flow.mm3_per_mm();
            } else {
                const double spacing = std::max(unscale<double>(strip), 0.02);
                p.mm3_per_mm         = spacing * h;
                p.width              = float(spacing + h * (1. - 0.25 * PI));
            }
        }
        out.push_back(std::move(p));
    }
}

// The insets of `area`, one spacing `s` apart (the first half a spacing in, so they are the beads' centre lines; parts too
// narrow for a bead on each side left out), as
// the chains of rings a spiral can run through: an inset continues a chain while the inset inside it is a single piece
// with its holes matching one to one. Where it splits (a narrow neck), or a hole's insets meet the outline's, the
// chain ends and each piece starts its own. Each chain is the outline's rings or one hole's rings; `lead` is 0 for the
// outline chain of the area's single first inset, h + 1 for its hole h, -1 otherwise (chains a wall loop can lead into).
struct SpiralChain
{
    Polygons rings;
    int      lead { -1 };
};

std::vector<SpiralChain> spiral_chains(const ExPolygon &area, double s)
{
    struct Node
    {
        ExPolygon        ex;
        std::vector<int> children;
    };
    std::vector<Node> nodes;
    std::vector<int>  roots, prev;
    for (int j = 0; j < 5000; ++j) {
        // Review M1: without its slivers. The inset at (j + 0.5) spacings is taken as the inset at j + 1 spacings grown back
        // by half a spacing, so wherever the area is too narrow for two beads side by side on this ring (a narrowing
        // limb's tip, a neck) the ring is not there: its two sides always lie at least a spacing apart, and what it leaves
        // goes to the variable-width gap fill, as with Concentric. (Mitred, so the corners stay sharp.)
        ExPolygons level = offset_ex(offset_ex(area, -float((1. + double(j)) * s)), float(0.5 * s));
        if (level.empty())
            break;
        std::vector<int> cur;
        for (ExPolygon &ex : level) {
            if (ex.contour.points.size() < 3)
                continue;
            int parent = -1;
            for (const int p : prev)
                if (nodes[size_t(p)].ex.contains(ex.contour.points.front())) {
                    parent = p;
                    break;
                }
            nodes.push_back({ std::move(ex), {} });
            const int id = int(nodes.size()) - 1;
            if (parent >= 0)
                nodes[size_t(parent)].children.push_back(id);
            else
                roots.push_back(id);
            cur.push_back(id);
        }
        prev = std::move(cur);
    }
    // Each hole of the parent matched to the child's hole round it (the child's holes are the larger).
    const auto match_holes = [&nodes](int parent, int child, std::vector<int> &map) {
        const ExPolygon &P = nodes[size_t(parent)].ex;
        const ExPolygon &C = nodes[size_t(child)].ex;
        if (P.holes.size() != C.holes.size())
            return false;
        map.assign(P.holes.size(), -1);
        std::vector<bool> used(C.holes.size(), false);
        for (size_t h = 0; h < P.holes.size(); ++h)
            for (size_t c = 0; c < C.holes.size(); ++c) {
                if (used[c] || P.holes[h].points.empty())
                    continue;
                Polygon ccw = C.holes[c];
                ccw.make_counter_clockwise();
                if (ccw.contains(P.holes[h].points.front())) {
                    map[h]  = int(c);
                    used[c] = true;
                    break;
                }
            }
        return std::find(map.begin(), map.end(), -1) == map.end();
    };
    std::vector<SpiralChain> out;
    std::function<void(int, bool)> run = [&](int first, bool lead) {
        std::vector<int>              seg { first };
        std::vector<std::vector<int>> maps;
        for (;;) {
            const Node &node = nodes[size_t(seg.back())];
            std::vector<int> map;
            if (node.children.size() != 1 || !match_holes(seg.back(), node.children.front(), map))
                break;
            maps.push_back(std::move(map));
            seg.push_back(node.children.front());
        }
        SpiralChain contour;
        contour.lead = lead ? 0 : -1;
        for (const int id : seg)
            contour.rings.push_back(nodes[size_t(id)].ex.contour);
        out.push_back(std::move(contour));
        for (size_t h = 0; h < nodes[size_t(first)].ex.holes.size(); ++h) {
            SpiralChain hole;
            hole.lead  = lead ? int(h) + 1 : -1;
            size_t cur = h;
            for (size_t i = 0; i < seg.size(); ++i) {
                hole.rings.push_back(nodes[size_t(seg[i])].ex.holes[cur]);
                if (i < maps.size())
                    cur = size_t(maps[i][cur]);
            }
            out.push_back(std::move(hole));
        }
        for (const int c : nodes[size_t(seg.back())].children)
            run(c, false);
    };
    for (const int r : roots)
        run(r, roots.size() == 1);
    return out;
}

} // namespace

std::vector<CurvedTopsFillPath> curved_tops_fill(const ExPolygon &area, CurvedTopsPattern pattern, const Flow &solid_flow, int k,
                                                 const PrintConfig &pcfg, const PrintObjectConfig &ocfg, bool *failed)
{
    std::vector<CurvedTopsFillPath> out;
    Surface    srf(stTop, area);
    FillParams params;
    params.density = 1.f;
    bool         threw    = false;
    const double area_mm2 = area.area() * SCALING_FACTOR * SCALING_FACTOR;
    if (pattern == ctpSpiral) {
        // No wall loop to lead in from: each spiral starts with a full turn on its outer inset.
        int index = 0;
        for (const SpiralChain &chain : spiral_chains(area, solid_flow.scaled_spacing()))
            spiral_append_paths(spiral_unit(nullptr, 0., 0., chain.rings, solid_flow.scaled_spacing(), k, index++), solid_flow, solid_flow,
                                out);
    } else if (pattern == ctpConcentric) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipConcentric));
        filler->spacing             = solid_flow.spacing();
        filler->bounding_box        = get_extents(area);
        filler->print_config        = &pcfg;
        filler->print_object_config = &ocfg;
        params.use_arachne          = true;
        params.layer_height         = solid_flow.height();
        ThickPolylines loops;
        try {
            loops = filler->fill_surface_arachne(&srf, params);
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(warning) << "Curved tops: concentric fill failed: " << ex.what();
            threw = true;
        }
        const Point centre = get_extents(area).center();
        int         index  = 0;
        for (ThickPolyline &tp : loops) {
            if (tp.points.size() < 2)
                continue;
            if (tp.points.size() >= 4 && tp.points.front() == tp.points.back() && !tp.width.empty() && tp.width.front() == tp.width.back()) {
                const Points ring(tp.points.begin(), tp.points.end() - 1);
                tp.start_at_index(int(curved_tops_seam_index(ring, centre, k, index + 1)));
            }
            ++index;
            const ExtrusionMultiPath mp = thick_polyline_to_multi_path(tp, erTopSolidInfill, solid_flow, scaled<float>(0.05),
                                                                       float(SCALED_EPSILON));
            for (const ExtrusionPath &p : mp.paths)
                if (p.polyline.points.size() >= 2)
                    out.push_back({ p.polyline.points, p.width, p.mm3_per_mm });
        }
    } else {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipMonotonic));
        filler->angle        = float((k % 2 == 0 ? 45. : 135.) * M_PI / 180.);
        filler->spacing      = solid_flow.spacing();
        filler->bounding_box = get_extents(area);
        Polylines lines;
        try {
            lines = filler->fill_surface(&srf, params);
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(warning) << "Curved tops: fill failed: " << ex.what();
            threw = true;
        }
        for (Polyline &pl : lines)
            if (pl.points.size() >= 2)
                out.push_back({ std::move(pl.points), float(solid_flow.width()), solid_flow.mm3_per_mm() });
    }
    // Review S3: a layer must not print its wall with nothing inside.
    if (failed != nullptr && (threw || (out.empty() && area_mm2 > 1.0)))
        *failed = true;
    return out;
}

namespace {

// The strip a path covers: its centre line grown by half its spacing (from its flow at the layer height).
Polygons path_footprint(const CurvedTopsFillPath &p, double height)
{
    const double spacing = height > 0. ? p.mm3_per_mm / height : double(p.width);
    return offset(Polyline(p.points), float(scale_(0.5 * spacing + 0.005)));
}

// What of `area` the paths leave open, without the slivers narrower than min_width.
ExPolygons uncovered_by(const ExPolygon &area, const std::vector<CurvedTopsFillPath> &paths, double height, double min_width)
{
    Polygons strokes;
    for (const CurvedTopsFillPath &p : paths)
        if (p.points.size() >= 2)
            polygons_append(strokes, path_footprint(p, height));
    return opening_ex(diff_ex(ExPolygons{ area }, union_ex(strokes)), float(scale_(0.5 * min_width)));
}

double bead_length_mm(const std::vector<CurvedTopsFillPath> &bead)
{
    double len = 0.;
    for (const CurvedTopsFillPath &p : bead)
        len += unscale<double>(Polyline(p.points).length());
    return len;
}

double bead_mm3(const std::vector<CurvedTopsFillPath> &bead)
{
    double v = 0.;
    for (const CurvedTopsFillPath &p : bead)
        v += unscale<double>(Polyline(p.points).length()) * p.mm3_per_mm;
    return v;
}

// Owner, after printing P5 (2026-11-17): short gap-fill beads printed on their own were dabs - a tiny extrusion with a
// retract, lift and drop before it and a retract and lift after it, with no room to spread or wipe. So each gap bead is
// placed after the path before it, nearest end first:
//  - an end within CURVED_TOPS_GAP_ATTACH_SPACINGS line spacings of where that path ends is joined to it in one motion:
//    a short connecting piece at CURVED_TOPS_GAP_CONNECTOR_FLOW of the fill's flow (an extrusion, so the move stays at
//    the surface and its contact with the beads beside it is judged as for any bead), then the bead; the bead is then
//    where the next one is looked for from, so beads along a spine chain up;
//  - otherwise it is printed on its own only when it is at least CURVED_TOPS_GAP_MIN_ISOLATED_MM long; a shorter one is
//    left out (a hole under the nozzle's size is better than a blob).
void place_gap_beads(std::vector<CurvedTopsFillPath> &out, std::vector<std::vector<CurvedTopsFillPath>> beads, const Flow &solid_flow,
                     CurvedTopsGapStats *stats)
{
    CurvedTopsGapStats st;
    st.candidates = beads.size();
    for (const std::vector<CurvedTopsFillPath> &b : beads)
        st.candidate_mm3 += bead_mm3(b);
    const double      attach  = scale_(CURVED_TOPS_GAP_ATTACH_SPACINGS * solid_flow.spacing());
    const double      h       = solid_flow.height();
    const double      c_mm3   = CURVED_TOPS_GAP_CONNECTOR_FLOW * solid_flow.mm3_per_mm();
    std::vector<bool> used(beads.size(), false);
    bool              has_cur = !out.empty() && !out.back().points.empty();
    Point             cur     = has_cur ? out.back().points.back() : Point(0, 0);
    for (size_t round = 0; round < beads.size(); ++round) {
        size_t best = beads.size();
        bool   rev  = false;
        double bd   = std::numeric_limits<double>::max();
        for (size_t i = 0; i < beads.size(); ++i) {
            if (used[i])
                continue;
            for (int end = 0; end < 2; ++end) {
                const Point  e = end == 0 ? beads[i].front().points.front() : beads[i].back().points.back();
                const double d = has_cur ? (e - cur).cast<double>().norm() : 0.;
                if (d < bd) {
                    bd   = d;
                    best = i;
                    rev  = end == 1;
                }
            }
        }
        if (best == beads.size())
            break;
        used[best] = true;
        std::vector<CurvedTopsFillPath> &b = beads[best];
        if (rev) {
            std::reverse(b.begin(), b.end());
            for (CurvedTopsFillPath &p : b)
                std::reverse(p.points.begin(), p.points.end());
        }
        const double len = bead_length_mm(b);
        if (has_cur && bd <= attach) {
            if (b.front().points.front() != cur) {
                CurvedTopsFillPath c{ Points{ cur, b.front().points.front() }, float(c_mm3 / h + h * (1. - 0.25 * PI)), c_mm3, false, true };
                st.kept_mm3 += unscale<double>(Polyline(c.points).length()) * c_mm3;
                out.push_back(std::move(c));
            }
            ++st.attached;
        } else if (len >= CURVED_TOPS_GAP_MIN_ISOLATED_MM) {
            ++st.isolated;
            st.shortest_isolated_mm = st.shortest_isolated_mm > 0. ? std::min(st.shortest_isolated_mm, len) : len;
        } else {
            ++st.dropped;
            continue;
        }
        st.kept_mm3 += bead_mm3(b);
        for (CurvedTopsFillPath &p : b)
            out.push_back(std::move(p));
        cur     = out.back().points.back();
        has_cur = true;
    }
    if (stats != nullptr) {
        stats->candidates += st.candidates;
        stats->candidate_mm3 += st.candidate_mm3;
        stats->attached += st.attached;
        stats->isolated += st.isolated;
        stats->dropped += st.dropped;
        stats->kept_mm3 += st.kept_mm3;
        if (st.shortest_isolated_mm > 0.)
            stats->shortest_isolated_mm = stats->shortest_isolated_mm > 0. ? std::min(stats->shortest_isolated_mm, st.shortest_isolated_mm) :
                                                                              st.shortest_isolated_mm;
    }
}

} // namespace

double curved_tops_min_gap_width(const PrintConfig &pcfg)
{
    return 0.25 * min_physical_nozzle_diameter(pcfg);
}

double curved_tops_uncovered_mm2(const ExPolygon &fp, const std::vector<CurvedTopsFillPath> &paths, double height, double min_width)
{
    return area(uncovered_by(fp, paths, height, min_width)) * SCALING_FACTOR * SCALING_FACTOR;
}

std::vector<CurvedTopsFillPath> curved_tops_layer_paths(const ExPolygon &fp, CurvedTopsPattern pattern, const Flow &wall_flow,
                                                        const Flow &solid_flow, int k, const PrintConfig &pcfg,
                                                        const PrintObjectConfig &ocfg, bool *failed, double nozzle_tolerance,
                                                        CurvedTopsGapStats *stats)
{
    std::vector<CurvedTopsFillPath> out;
    // One wall loop along the outline, each loop starting at a seam spread along its length per layer.
    const Point centre  = get_extents(fp).center();
    int         loop_no = 0;
    const auto  wall    = [&](const Polygon &polygon) {
        Points pts = polygon.points;
        if (pts.size() >= 3)
            std::rotate(pts.begin(), pts.begin() + ptrdiff_t(curved_tops_seam_index(pts, centre, k, -1 - loop_no)), pts.end());
        ++loop_no;
        pts.push_back(pts.front());
        CurvedTopsFillPath p{ std::move(pts), float(wall_flow.width()), wall_flow.mm3_per_mm() };
        p.wall = true;
        out.push_back(std::move(p));
    };
    // The fill region's outline lies this far inside the patch edge (its beads' outer edge).
    const double inner_delta = wall_flow.scaled_width() - 0.5 * wall_flow.scaled_spacing() + 0.5 * solid_flow.scaled_spacing();
    if (pattern == ctpSpiral) {
        // P5: each wall loop leads into the spiral inside it, when the region inside it is one piece; a hole's wall loop
        // leads into the spiral round that hole. A wall loop with nothing to lead into prints on its own, first.
        const double s = solid_flow.scaled_spacing();
        // Wall centre line to the first inset's: the wall's half width, the fill's outline, half a spacing.
        const double lead_distance = inner_delta - 0.5 * wall_flow.scaled_width() + 0.5 * s;
        const double area_mm2      = fp.area() * SCALING_FACTOR * SCALING_FACTOR;
        size_t       fill_paths    = 0;
        int          unit_no       = 0;
        for (const ExPolygon &loop : offset_ex(fp, -0.5 * wall_flow.scaled_width())) {
            const ExPolygons         areas = offset_ex(loop, -float(inner_delta - 0.5 * wall_flow.scaled_width()));
            std::vector<SpiralChain> chains;
            for (const ExPolygon &area : areas)
                append(chains, spiral_chains(area, s));
            // Which wall ring leads into which chain: the outline into the outline chain of the single first inset, a
            // hole into the chain of the first inset's hole round it.
            std::vector<const Polygon *> lead_of(chains.size(), nullptr);
            std::vector<bool>            led(loop.holes.size() + 1, false);
            if (areas.size() == 1)
                for (size_t c = 0; c < chains.size(); ++c) {
                    if (chains[c].lead == 0) {
                        lead_of[c] = &loop.contour;
                        led[0]     = true;
                    } else if (chains[c].lead > 0 && !chains[c].rings.empty()) {
                        Polygon ccw = chains[c].rings.front();
                        ccw.make_counter_clockwise();
                        for (size_t h = 0; h < loop.holes.size(); ++h)
                            if (!led[h + 1] && !loop.holes[h].points.empty() && ccw.contains(loop.holes[h].points.front())) {
                                lead_of[c]  = &loop.holes[h];
                                led[h + 1] = true;
                                break;
                            }
                    }
                }
            if (!led[0])
                wall(loop.contour);
            for (size_t h = 0; h < loop.holes.size(); ++h)
                if (!led[h + 1])
                    wall(loop.holes[h]);
            for (size_t c = 0; c < chains.size(); ++c) {
                const size_t before = out.size();
                if (lead_of[c] != nullptr)
                    spiral_append_paths(spiral_unit(lead_of[c], wall_flow.scaled_spacing(), lead_distance, chains[c].rings, s, k, -1 - loop_no++),
                                        wall_flow, solid_flow, out);
                else
                    spiral_append_paths(spiral_unit(nullptr, 0., 0., chains[c].rings, s, k, unit_no++), wall_flow, solid_flow, out);
                for (size_t i = before; i < out.size(); ++i)
                    fill_paths += out[i].wall ? 0 : 1;
            }
        }
        // As for the other patterns: never a wall with nothing inside (review S3).
        if (failed != nullptr && fill_paths == 0 && area_mm2 > 4.0 && !offset_ex(fp, -float(inner_delta)).empty())
            *failed = true;
    } else {
        for (const ExPolygon &loop : offset_ex(fp, -0.5 * wall_flow.scaled_width())) {
            wall(loop.contour);
            for (const Polygon &hole : loop.holes)
                wall(hole);
        }
        // The fill inside it. P5: Monotonic with a nozzle tolerance in force reaches CURVED_TOPS_MONOTONIC_OVERLAP of a
        // spacing further out, over the wall loop's inner edge, so the line ends close the wedges between them and the
        // wall (their beads touch the wall's by about that much, within the tolerance); without one, P4's fill exactly.
        const double overlap = pattern == ctpMonotonic && nozzle_tolerance > 0. ?
                                   CURVED_TOPS_MONOTONIC_OVERLAP * solid_flow.scaled_spacing() : 0.;
        for (const ExPolygon &area : offset_ex(fp, -(inner_delta - overlap)))
            append(out, curved_tops_fill(area, pattern, solid_flow, k, pcfg, ocfg, failed));
    }

    // Gap fill (owner, 2026-11-17): whatever the wall and the fill leave open and a bead can still cover - the
    // spine of a narrowing limb that the concentric loops do not reach, wedges where loops turn sharp corners,
    // the strip between the wall and the fill - gets variable-width beads along its medial axis, at the layer's
    // height (flow from width and height as for any bead). Slivers narrower than a quarter of the nozzle stay.
    // Monotonic (P5) only with a nozzle contact tolerance in force: beads squeezed into the wedges at the line ends
    // touch their neighbours by about 0.1 mm, which the strict head check (tolerance 0) refuses; without a tolerance
    // Monotonic keeps P4's behaviour (no gap fill, the wedges stay open).
    if (pattern == ctpMonotonic && nozzle_tolerance <= 0.)
        return out;
    const double h         = solid_flow.height();
    const double min_width = curved_tops_min_gap_width(pcfg);
    const double max_width = 2. * solid_flow.spacing();
    // Each gap bead as its pieces (one per width), in order.
    std::vector<std::vector<CurvedTopsFillPath>> beads;
    ThickPolylines gaps;
    for (ExPolygon &gap : uncovered_by(fp, out, h, min_width)) {
        const size_t before = gaps.size();
        gap.douglas_peucker(scale_(0.0125));
        gap.medial_axis(scale_(min_width), scale_(max_width), &gaps);
        if (gaps.size() == before) {
            // A compact hole with no axis (a dome's apex inside the innermost ring): one short line through it.
            const double gap_mm2 = gap.area() * SCALING_FACTOR * SCALING_FACTOR;
            if (gap_mm2 < 0.05 || gap_mm2 > 1.0)
                continue;
            if (pattern == ctpSpiral) {
                // Review S1: the spiral ends round the apex; the line through the middle turns from layer to layer (60
                // degrees per curved layer), so its ends do not stack, and its flow puts the hole's own volume in it.
                const double ang  = double(k) * M_PI / 3.;
                const Vec2d  c    = unscale(get_extents(gap).center());
                const Vec2d  dir(std::cos(ang), std::sin(ang));
                const double half = 0.5 * std::max(2. * std::sqrt(gap_mm2 / M_PI) - solid_flow.width(), 0.1);
                const double mm3  = std::clamp(gap_mm2 * h / (2. * half), 0.25 * solid_flow.mm3_per_mm(), 2. * solid_flow.mm3_per_mm());
                const Vec2d  a = c - dir * half, b = c + dir * half;
                beads.push_back({ { Points{ Point::new_scale(a.x(), a.y()), Point::new_scale(b.x(), b.y()) },
                                    float(mm3 / h + h * (1. - 0.25 * PI)), mm3, false, true } });
                continue;
            }
            const BoundingBox bb   = get_extents(gap);
            const Vec2d       size = unscale(bb.size());
            const Vec2d       c    = unscale(bb.center());
            const Vec2d       dir  = size.x() >= size.y() ? Vec2d(1., 0.) : Vec2d(0., 1.);
            const double      half = 0.5 * std::max(std::max(size.x(), size.y()) - solid_flow.width(), 0.1);
            const Vec2d       a = c - dir * half, b = c + dir * half;
            beads.push_back({ { Points{ Point::new_scale(a.x(), a.y()), Point::new_scale(b.x(), b.y()) }, float(solid_flow.width()),
                                solid_flow.mm3_per_mm(), false, true } });
        }
    }
    for (const ThickPolyline &tp : gaps) {
        if (tp.length() < scale_(0.2))
            continue;
        const ExtrusionMultiPath mp = thick_polyline_to_multi_path(tp, erGapFill, solid_flow, scaled<float>(0.05), float(SCALED_EPSILON));
        std::vector<CurvedTopsFillPath> bead;
        for (const ExtrusionPath &p : mp.paths)
            if (p.polyline.points.size() >= 2)
                bead.push_back({ p.polyline.points, p.width, p.mm3_per_mm, false, true });
        if (!bead.empty())
            beads.push_back(std::move(bead));
    }
    place_gap_beads(out, std::move(beads), solid_flow, stats);
    return out;
}

CurvedTopsNozzleTolerance curved_tops_nozzle_tolerance(const PrintObjectConfig &config, const CurvedTopsHeadLimits &head,
                                                       const CurvedTopsDevOverrides &dev)
{
    CurvedTopsNozzleTolerance out;
    const double v = config.curved_tops_nozzle_tolerance.value;
    out.asked      = std::isfinite(v) ? std::clamp(v, 0., 1.) : 0.;
    out.record     = head.verified_nozzle_contact_mm;
    if (out.asked <= 0.)
        return out;
    if (dev.active && dev.nozzle_tolerance_unrecorded) {
        out.used = out.asked;
        if (out.asked > out.record + 1e-9)
            out.note = out.record > 0. ?
                           Slic3r::format(_u8L("Developer override: nozzle contact tolerance %1% mm applied, deeper than the %2% mm "
                                               "printed cleanly with this print head."),
                                          mm_str(out.asked), mm_str(out.record)) :
                           Slic3r::format(_u8L("Developer override: nozzle contact tolerance %1% mm applied although no nozzle contact "
                                               "print is recorded for this print head."),
                                          mm_str(out.asked));
        return out;
    }
    out.used = std::min(out.asked, out.record);
    if (out.record <= 0.)
        out.note = Slic3r::format(_u8L("Nozzle contact tolerance not applied: no contact print recorded for this print head (the setting asks "
                                       "for %1% mm). The nozzle may not pass through printed material."),
                                  mm_str(out.asked));
    else if (out.asked > out.record + 1e-9)
        out.note = Slic3r::format(_u8L("Nozzle contact tolerance capped at %1% mm, the deepest nozzle contact printed cleanly with this print "
                                       "head (%2%); the setting asks for %3% mm."),
                                  mm_str(out.record), head.verified_on, mm_str(out.asked));
    return out;
}

namespace {

constexpr double SAMPLE_STEP_MM   = 0.5;   // XY resampling of conformal paths
constexpr double MIN_AREA_MM2     = 4.0;   // smaller surfaces stay planar
constexpr double OUTSIDE_TOL_MM2  = 1.0;   // tolerated sliver of material outside a surface

struct Surface3
{
    ExPolygon footprint;
    double    z_min = std::numeric_limits<double>::max();
    double    z_max = -std::numeric_limits<double>::max();
    double    slope_max_deg = 0.;
};

// The top surface height (print frame) at (x, y), NaN where the downward ray misses.
struct SurfaceHeight
{
    const sla::IndexedMesh &imesh;
    double                  ray_origin_z;
    double                  ground;

    double operator()(double x, double y) const
    {
        const auto hit = imesh.query_ray_hit({x, y, ray_origin_z}, {0.0, 0.0, -1.0});
        if (!hit.is_hit())
            return std::numeric_limits<double>::quiet_NaN();
        return ray_origin_z - hit.distance() - ground;
    }
};

bool path_is_curved(const ExtrusionEntity *e)
{
    if (auto *p = dynamic_cast<const ExtrusionPath *>(e))
        return p->z_contour != nullptr && p->z_contour->curved_top;
    if (auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e))
        return !c->entities.empty() && path_is_curved(c->entities.front());
    return false;
}

// Remove what an earlier run appended (posContouring can re-run without posInfill).
void remove_previous_conformal(ExtrusionEntityCollection &fills)
{
    ExtrusionEntitiesPtr kept;
    for (ExtrusionEntity *e : fills.entities) {
        if (path_is_curved(e))
            delete e;
        else
            kept.push_back(e);
    }
    fills.entities = std::move(kept);
}

void collect_paths(const ExtrusionEntity *e, std::vector<const ExtrusionPath *> &out)
{
    if (auto *p = dynamic_cast<const ExtrusionPath *>(e))
        out.push_back(p);
    else if (auto *l = dynamic_cast<const ExtrusionLoop *>(e))
        for (const ExtrusionPath &p : l->paths) out.push_back(&p);
    else if (auto *m = dynamic_cast<const ExtrusionMultiPath *>(e))
        for (const ExtrusionPath &p : m->paths) out.push_back(&p);
}

// Every path of an extrusion tree, collections included.
void collect_all_paths(const ExtrusionEntity *e, std::vector<const ExtrusionPath *> &out)
{
    if (auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        for (const ExtrusionEntity *child : c->entities)
            collect_all_paths(child, out);
    } else
        collect_paths(e, out);
}

// What a support layer (supports, interfaces, side stabilizers) puts on the plate: its islands and
// every extrusion of it at its width.
Polygons support_material_area(const SupportLayer &layer)
{
    Polygons out = to_polygons(layer.support_islands);
    std::vector<const ExtrusionPath *> paths;
    collect_all_paths(&layer.support_fills, paths);
    for (const auto &[extruder, coll] : layer.interface_by_extruder)
        collect_all_paths(&coll, paths);
    for (const ExtrusionPath *p : paths)
        if (p->polyline.size() >= 2)
            polygons_append(out, offset(p->polyline, float(scale_(0.5 * std::max(0.1f, p->width)))));
    return out;
}

// Clip every extrusion of `coll` against `drop`. A leaf that does not touch `drop` is kept as is
// (loop, seam and all); one that does is replaced by its surviving pieces as plain paths.
void clip_collection(ExtrusionEntityCollection &coll, const ExPolygons &drop, const BoundingBox &drop_bb)
{
    ExtrusionEntitiesPtr out;
    out.reserve(coll.entities.size());
    for (ExtrusionEntity *e : coll.entities) {
        if (auto *c = dynamic_cast<ExtrusionEntityCollection *>(e)) {
            clip_collection(*c, drop, drop_bb);
            if (c->entities.empty())
                delete e;
            else
                out.push_back(e);
            continue;
        }
        std::vector<const ExtrusionPath *> paths;
        collect_paths(e, paths);
        if (paths.empty() || path_is_curved(e)) {
            out.push_back(e);
            continue;
        }
        bool touches = false;
        for (const ExtrusionPath *p : paths) {
            BoundingBox bb(p->polyline.points);
            if (bb.overlap(drop_bb) && !intersection_pl(Polylines{p->polyline}, drop).empty()) {
                touches = true;
                break;
            }
        }
        if (!touches) {
            out.push_back(e);
            continue;
        }
        // The surviving pieces replace the leaf in place: GCode::extrude() takes leaves only, so
        // they must not be wrapped in a collection of their own.
        for (const ExtrusionPath *p : paths)
            for (Polyline &kept : diff_pl(Polylines{p->polyline}, drop)) {
                if (kept.length() < scale_(0.2))
                    continue;
                ExtrusionPath np(*p);
                np.polyline = std::move(kept);
                np.polyline.fitting_result.clear();
                out.push_back(new ExtrusionPath(std::move(np)));
            }
        delete e;
    }
    coll.entities = std::move(out);
}

// Owner, P5 (rim gap, steeper dome): the planar layers right under the curved block must be solid. With partial curving
// the plateau at the cut lies deep inside the part, where the slicer laid sparse infill, so the lowest curved layer printed
// onto sparse lines (the curved layers above the cut were the part's only top shell); with whole curving the same happens
// where the top shells do not reach below the clipped layers. Here, in each planar layer within n_solid layers under the
// curved block (cover: where it prints), sparse infill is replaced by solid infill (monotonic, 45 / 135 degrees by layer,
// the region's solid infill flow). n_solid: the region's top shell count less the curved layers, at least 2. Where the
// layer is solid already nothing changes. Returns how many layer regions got solid infill.
size_t solid_under_curved_block(PrintObject &object, const indexed_triangle_set &its, double ground, const ExPolygons &cover, bool partial,
                                double cut, int n_layers, double h)
{
    if (cover.empty() || object.layers().empty() || object.layers().back()->regions().empty())
        return 0;
    const PrintRegionConfig &rcfg  = object.layers().back()->regions().front()->region().config();
    const int n_top   = std::max(rcfg.top_shell_layers.value, int(std::ceil(rcfg.top_shell_thickness.value / h - EPSILON)));
    const int n_solid = std::max(2, n_top - n_layers);
    std::vector<size_t> idx;
    std::vector<float>  za, zb;
    for (size_t i = 1; i < object.layers().size(); ++i) {
        const double z = object.layers()[i]->print_z;
        idx.push_back(i);
        za.push_back(float(z + n_layers * h + ground - 0.5 * EPSILON));
        zb.push_back(float(z + (n_layers + n_solid) * h + ground - 0.5 * EPSILON));
    }
    const std::vector<ExPolygons> A = slice_mesh_ex(its, za);
    const std::vector<ExPolygons> B = slice_mesh_ex(its, zb);
    size_t added = 0;
    for (size_t j = 0; j < idx.size(); ++j) {
        Layer       *layer = object.layers()[idx[j]];
        const double z     = layer->print_z;
        ExPolygons   under;
        if (partial && z <= cut + EPSILON) {
            // The plateau's top layers.
            if (z <= cut - n_solid * h + EPSILON)
                continue;
            under = diff_ex(cover, B[j]);
        } else
            under = diff_ex(intersection_ex(cover, A[j]), B[j]);
        if (under.empty())
            continue;
        for (LayerRegion *region : layer->regions()) {
            ExPolygons sparse;
            for (const Surface &s : region->fill_surfaces.surfaces)
                if (s.surface_type == stInternal)
                    sparse.push_back(s.expolygon);
            if (sparse.empty())
                continue;
            const ExPolygons solid = opening_ex(intersection_ex(under, sparse), float(scale_(0.2)));
            if (solid.empty())
                continue;
            clip_collection(region->fills, solid, get_extents(solid));
            const Flow            flow = region->flow(frSolidInfill, layer->height);
            std::unique_ptr<Fill> filler(Fill::new_from_type(ipMonotonic));
            filler->angle        = float((idx[j] % 2 == 0 ? 45. : 135.) * M_PI / 180.);
            filler->spacing      = flow.spacing();
            filler->bounding_box = get_extents(solid);
            FillParams params;
            params.density = 1.f;
            auto coll      = std::make_unique<ExtrusionEntityCollection>();
            for (const ExPolygon &ex : solid) {
                Surface   srf(stInternalSolid, ex);
                Polylines lines;
                try {
                    lines = filler->fill_surface(&srf, params);
                } catch (const std::exception &ex_) {
                    BOOST_LOG_TRIVIAL(warning) << "Curved tops: solid infill under the block failed: " << ex_.what();
                    continue;
                }
                for (Polyline &pl : lines)
                    if (pl.size() >= 2) {
                        ExtrusionPath path(erSolidInfill, flow.mm3_per_mm(), float(flow.width()), float(layer->height));
                        path.polyline = std::move(pl);
                        coll->entities.push_back(new ExtrusionPath(std::move(path)));
                    }
            }
            if (!coll->entities.empty()) {
                region->fills.entities.push_back(coll.release());
                ++added;
            }
        }
    }
    return added;
}

Points resample(const Points &pts)
{
    Points out;
    if (pts.empty())
        return out;
    out.push_back(pts.front());
    for (size_t i = 1; i < pts.size(); ++i) {
        const Vec2d  a   = pts[i - 1].cast<double>();
        const Vec2d  b   = pts[i].cast<double>();
        const double len = unscale<double>((b - a).norm());
        const int    n   = std::max(1, int(std::ceil(len / SAMPLE_STEP_MM)));
        for (int j = 1; j <= n; ++j) {
            const Vec2d p = a + (b - a) * (double(j) / double(n));
            const Point q(coord_t(std::llround(p.x())), coord_t(std::llround(p.y())));
            if (q != out.back())
                out.push_back(q);
        }
    }
    return out;
}

} // namespace

bool has_curved_tops_block(const PrintObject &object)
{
    return !object.layers().empty() && object.layers().back()->curved_tops_block;
}

namespace {

// The settings side of every guard in apply_curved_tops(), each reduced to what decides the verdict
// (review r2): "alone on the plate" rather than the object and copy counts, "more than one filament"
// rather than which ones, "a skirt that blocks" rather than its numbers. A change that cannot change
// any verdict then changes nothing here, so adding the 20th part to a plate does not re-slice the
// other 19. Left out on purpose: what only slicing produces (layer count, the sub-layer plan) and the
// mesh, whose changes re-slice the object anyway.
struct GuardSettings
{
    std::string head;
    bool        head_usable { false };
    bool        not_alone { false }, by_object { false }, zaa { false }, spiral { false }, raft { false }, vlh { false };
    bool        multi_filament { false }, smooth_tower { false }, local_z { false }, nozzles { false }, skirt { false };
    bool        support { false };
    double      printable_height { 0. };
    int         layers { 0 };
    double      angle { 0. }, layer_height { 0. };
    // P4: the nozzle contact tolerance and the fill pattern.
    double      nozzle_tolerance { 0. };
    int         pattern { 0 };
    // P3: the developer overrides in force (or "ignored"), see CurvedTopsDevOverrides::record().
    CurvedTopsDevOverrides dev;

    bool blocked() const
    {
        return !head_usable || (not_alone && !dev.allow_multiple_objects) || (by_object && !dev.allow_by_object) ||
               (zaa && !dev.allow_z_contouring) || spiral || (raft && !dev.allow_raft) || (vlh && !dev.allow_variable_layer_height) ||
               ((multi_filament || smooth_tower) && !dev.allow_multi_filament) || local_z || nozzles || (skirt && !dev.allow_tall_skirt);
    }
};

GuardSettings guard_settings(const PrintObject &object, int multi_filament)
{
    const Print             &print = *object.print();
    const PrintConfig       &pcfg  = print.config();
    const PrintObjectConfig &ocfg  = object.config();
    GuardSettings s;
    s.dev = curved_tops_dev_overrides(ocfg);
    const CurvedTopsHeadLimits head = curved_tops_head_limits(pcfg, s.dev);
    s.head             = (head.head ? head.head->id : std::string("-")) + "|" + pcfg.printer_model.value;
    s.head_usable      = head.usable;
    s.not_alone        = print.objects().size() != 1 || object.instances().size() != 1;
    s.by_object        = pcfg.print_sequence.value == PrintSequence::ByObject;
    s.zaa              = object.need_z_contouring();
    s.spiral           = pcfg.spiral_mode.value;
    s.raft             = ocfg.raft_layers.value > 0;
    s.vlh              = object.model_object() != nullptr && !object.model_object()->layer_height_profile.empty();
    s.multi_filament   = multi_filament >= 0 ? multi_filament > 0 : print.extruders(true).size() > 1;
    s.smooth_tower     = print.has_wipe_tower() && pcfg.timelapse_type.value == TimelapseType::tlSmooth;
    s.local_z          = pcfg.dithering_local_z_mode.value;
    s.nozzles          = print.uses_multiple_nozzle_diameters();
    s.skirt            = pcfg.skirt_loops.value > 0 && (pcfg.draft_shield.value != dsDisabled || pcfg.skirt_height.value > 1);
    // Recorded so that turning supports or stabilizers on or off re-plans the block, but no longer a
    // blocker by itself (P2): the block is checked against the support layers once they exist.
    s.support          = object.has_support_material() || object.has_stabilizers();
    s.printable_height = pcfg.printable_height.value;
    s.layers           = ocfg.curved_tops_layers.value;
    s.angle            = ocfg.curved_tops_max_angle.value;
    s.layer_height     = ocfg.layer_height.value;
    s.nozzle_tolerance = ocfg.curved_tops_nozzle_tolerance.value;
    s.pattern          = int(ocfg.curved_tops_pattern.value);
    return s;
}

} // namespace

std::string curved_tops_guard_inputs(const PrintObject &object, int multi_filament)
{
    const GuardSettings s = guard_settings(object, multi_filament);
    std::ostringstream  out;
    out << "head=" << s.head << ',' << s.head_usable << ";alone=" << !s.not_alone << ";seq=" << s.by_object << ";zaa=" << s.zaa
        << ";spiral=" << s.spiral << ";raft=" << s.raft << ";vlh=" << s.vlh << ";filaments=" << s.multi_filament
        << ";tower=" << s.smooth_tower << ";localz=" << s.local_z << ";nozzles=" << s.nozzles << ";skirt=" << s.skirt
        << ";support=" << s.support << ";height=" << s.printable_height << ";ct=" << s.layers << ',' << s.angle << ',' << s.layer_height
        << ',' << s.nozzle_tolerance << ',' << s.pattern;
    // P3 (last, Print::apply() compares this tail on its own): the overrides in force.
    out << ";dev=" << s.dev.record();
    return out.str();
}

bool curved_tops_blocked_by_settings(const PrintObject &object, int multi_filament)
{
    return guard_settings(object, multi_filament).blocked();
}

CurvedTopsResult apply_curved_tops(PrintObject &object, const TriangleMesh &mesh, const sla::IndexedMesh &imesh)
{
    const PrintObjectConfig &ocfg  = object.config();
    const Print             *print = object.print();
    CurvedTopsResult         result;
    const auto               refuse = [&object, &result](const std::string &why) {
        BOOST_LOG_TRIVIAL(warning) << "Curved tops: object \"" << object.model_object()->name << "\" left planar: " << why;
        result.applied = false;
        result.refusal = why;
        result.summary.clear();
        result.notes.clear();
        return result;
    };

    if (object.layers().size() < 3)
        return refuse(_u8L("it has fewer than three layers."));
    Layer *top_layer = object.layers().back();
    top_layer->curved_tops_block     = false;
    top_layer->curved_tops_slope_deg = 0.f;
    for (LayerRegion *region : top_layer->regions())
        remove_previous_conformal(region->fills);

    // Guards: what this version handles (design tests/design_curved_tops_any_printer.md, 3.2).
    // P3: a developer override (Developer mode only) may switch a refusal that guards against a collision
    // off, or widen a limit. Refusals that guard against output the code cannot get right (spiral vase,
    // sub-layer plans, mixed nozzle diameters, the printable height) have no override.
    const PrintConfig            &pcfg = print->config();
    const CurvedTopsDevOverrides  dev  = curved_tops_dev_overrides(ocfg);
    const CurvedTopsHeadLimits    head = curved_tops_head_limits(pcfg, dev);
    if (!head.usable)
        return refuse(head.refusal);
    if ((print->objects().size() != 1 || object.instances().size() != 1) && !dev.allow_multiple_objects)
        return refuse(_u8L("there is more than one object or copy on the plate (curved tops handles one object for now)."));
    if (pcfg.print_sequence.value == PrintSequence::ByObject && !dev.allow_by_object)
        return refuse(_u8L("the plate prints by object; curved tops needs the by-layer sequence."));
    // Z contouring moves the same top layers up and down; the curved-tops head check is also more
    // lenient on the block's extrusions than on Z contouring moves. One or the other per object.
    if (object.need_z_contouring() && !dev.allow_z_contouring)
        return refuse(_u8L("Z contouring is on for it as well; curved tops and Z contouring do not run on the same object."));
    if (pcfg.spiral_mode.value)
        return refuse(_u8L("spiral vase mode is on."));
    if (ocfg.raft_layers.value > 0 && !dev.allow_raft)
        return refuse(_u8L("it has a raft."));
    if (object.model_object() != nullptr && !object.model_object()->layer_height_profile.empty() && !dev.allow_variable_layer_height)
        return refuse(_u8L("it has a variable layer height."));
    // The tower stands at layer height next to the part; with more than one filament there is one
    // (and tool changes). A smooth timelapse builds one even for a single filament.
    if (print->extruders(true).size() > 1 && !dev.allow_multi_filament)
        return refuse(_u8L("the plate uses more than one filament (curved tops handles one filament and no prime tower for now)."));
    if (print->has_wipe_tower() && pcfg.timelapse_type.value == TimelapseType::tlSmooth && !dev.allow_multi_filament)
        return refuse(_u8L("the smooth timelapse adds a prime tower to the plate."));
    // Same tests as Z contouring's guard (3) in need_z_contouring(): the band's layers must be plain.
    if (pcfg.dithering_local_z_mode.value)
        return refuse(_u8L("local-Z dithering is on."));
    if (!object.local_z_intervals().empty())
        return refuse(_u8L("it has a sub-layer plan (local Z or mixed-colour sub-layers)."));
    if (print->uses_multiple_nozzle_diameters())
        return refuse(_u8L("the plate mixes nozzle diameters (adaptive layer merge)."));
    // A draft shield stands about 2 mm from the part, inside the nozzle's reach; a skirt taller
    // than one layer may reach the curved band.
    if (pcfg.skirt_loops.value > 0 && (pcfg.draft_shield.value != dsDisabled || pcfg.skirt_height.value > 1) && !dev.allow_tall_skirt)
        return refuse(_u8L("it has a draft shield or a skirt taller than one layer."));
    // Supports and side stabilizers (P2): no refusal here. Print::process() runs this step after the
    // support step for an object with curved tops on, and the block is checked against the support
    // layers below, once the band is known.
    if (top_layer->regions().empty())
        return refuse(_u8L("its top layer has nothing to print."));

    const int    n_layers  = std::max(1, ocfg.curved_tops_layers.value);
    const double h         = ocfg.layer_height.value;
    const double max_angle = curved_tops_effective_max_angle(ocfg.curved_tops_max_angle.value, head);
    // P4: how deep the nozzle may pass through flat material beside the patch (the head check's nozzle
    // tolerance). 0 keeps P3's rules exactly: nothing beside above the band bottom. Otherwise material beside
    // may stand up to the lowest curved bead (one layer above the band bottom) plus the tolerance.
    // Review M2: capped by the head's printed record, 0 without one (unless the developer override lifts it).
    const CurvedTopsNozzleTolerance nozzle = curved_tops_nozzle_tolerance(ocfg, head, dev);
    const double nozzle_tol    = nozzle.used;
    const double beside_allow  = nozzle_tol > 0. ? h + nozzle_tol : 0.;
    // Mesh frame to print frame: print Z = mesh Z - ground. A raft (developer override only, refused
    // above otherwise) lifts every object layer by object_print_z_min, which the mesh does not know.
    const double z_shift   = ocfg.raft_layers.value > 0 ? object.slicing_parameters().object_print_z_min : 0.;
    const double ground    = imesh.ground_level() - z_shift;

    // 1. Candidate facets.
    const indexed_triangle_set &its = mesh.its;
    double mesh_z_max = -std::numeric_limits<double>::max();
    for (const stl_vertex &v : its.vertices)
        mesh_z_max = std::max(mesh_z_max, double(v.z()));
    const SurfaceHeight surface{imesh, mesh_z_max + 5.0, ground};

    // P4: the patch edge follows the interpolated iso-slope line across the facets (see
    // curved_tops_candidate_surfaces()), not the facets' sides.
    CurvedTopsCandidates                found  = curved_tops_candidate_surfaces(its, imesh, max_angle, ground);
    const std::vector<CurvedTopsFacet> &facets = found.facets;
    const ExPolygons                   &pieces = found.pieces;
    if (facets.empty() || pieces.empty())
        return refuse(Slic3r::format(_u8L("it has no top surface flatter than %1% degrees."), deg_str(max_angle)));

    // Every surface that is curved (rises at least one layer) and large enough, however tall.
    std::vector<Surface3> candidates;
    for (const ExPolygon &piece : pieces) {
        Surface3 s;
        s.footprint = piece;
        for (const CurvedTopsFacet &f : facets)
            if (piece.contains(f.probe)) {
                s.z_min         = std::min(s.z_min, f.z_min);
                s.z_max         = std::max(s.z_max, f.z_max);
                s.slope_max_deg = std::max(s.slope_max_deg, f.slope_deg);
            }
        const double area_mm2 = piece.area() * SCALING_FACTOR * SCALING_FACTOR;
        const double rise     = s.z_max - s.z_min;
        if (area_mm2 < MIN_AREA_MM2 || rise < h) {
            BOOST_LOG_TRIVIAL(info) << "Curved tops: surface skipped, area " << area_mm2 << " mm2, rise " << rise << " mm";
            continue;
        }
        candidates.push_back(std::move(s));
    }
    if (candidates.empty())
        return refuse(_u8L("it has no top surface that is curved (rises at least one layer) and large enough."));

    // Top of every planar layer, ascending.
    std::vector<double> planar_tops;
    for (const Layer *layer : object.layers())
        planar_tops.push_back(layer->print_z);

    // R1 (rise) over the whole block (review finding e): the curved layers of every surface print in one
    // block, so with the nozzle at the lowest curved point the highest point of the block may be under
    // the head. When every surface fits within the allowed rise of the highest point, each is curved
    // whole, as in P1. Otherwise (P2, partial curving) the block is cut at `cut`, the first planar layer
    // top at most the allowed rise below the highest point: above it the surfaces are curved, below it
    // they print as ordinary flat layers, and the planar body is kept as a flat plateau up to `cut` under
    // the curved part, and every curved bead lies at least one layer above the cut (cut + h).
    //
    // R2 (nothing beside the footprint in the band) decides the same way (P2): when material outside
    // the surfaces stands in the band of whole curving (the steep flanks of a rounded part, say), the
    // cut is raised to the highest layer top such material reaches, so it all prints flat below the
    // plateau. The check itself is unchanged (beside_z), so like P1's it tolerates a sliver of up to
    // OUTSIDE_TOL_MM2 per layer and anything within 0.5 mm of the footprint. On the partial path that
    // tolerance does not reach above the lowest curved bead: flat_above_z() raises the cut over every
    // layer above cut + h that still has a planar extrusion outside the footprint, so nothing printed
    // flat stands higher than the lowest curved bead, by construction. (Whole curving keeps P1's
    // tolerance; there the head check is what stops a sliver that would be met, see the tests.)
    double z_top = -std::numeric_limits<double>::max(), z_bottom = std::numeric_limits<double>::max();
    for (const Surface3 &s : candidates) {
        z_top    = std::max(z_top, s.z_max);
        z_bottom = std::min(z_bottom, s.z_min);
    }
    const double floor_z = z_top - head.allowed_rise_mm;
    // The highest layer above `above` with more than the tolerated sliver of material outside the
    // grown footprint (-inf when none).
    const auto beside_z = [&object](const ExPolygons &fp, double above) {
        const ExPolygons grown = offset_ex(fp, scale_(0.5));
        double           fail  = -std::numeric_limits<double>::max();
        for (const Layer *layer : object.layers())
            if (layer->print_z > above + EPSILON &&
                area(diff_ex(layer->lslices, grown)) * SCALING_FACTOR * SCALING_FACTOR > OUTSIDE_TOL_MM2)
                fail = std::max(fail, layer->print_z);
        return fail;
    };
    // Partial path, strict: the highest layer above `above` with a planar extrusion (wall, fill, thin
    // fill; centre line) outside the footprint grown by 0.05 mm, over 0.1 mm of it (-inf when none). The
    // clip (step 4) removes planar material inside the footprint only, so this is exactly the flat
    // material that would print above `above` beside the curved part.
    const auto flat_above_z = [&object](const ExPolygons &fp, double above) {
        const ExPolygons grown = offset_ex(fp, scale_(0.05));
        double           fail  = -std::numeric_limits<double>::max();
        for (const Layer *layer : object.layers()) {
            if (layer->print_z <= above + EPSILON)
                continue;
            std::vector<const ExtrusionPath *> paths;
            for (const LayerRegion *region : layer->regions()) {
                collect_all_paths(&region->perimeters, paths);
                collect_all_paths(&region->fills, paths);
                collect_all_paths(&region->thin_fills, paths);
            }
            Polylines lines;
            for (const ExtrusionPath *p : paths)
                if (!path_is_curved(p) && p->polyline.size() >= 2)
                    lines.push_back(p->polyline);
            double outside = 0.;
            for (const Polyline &pl : diff_pl(lines, grown))
                outside += unscale<double>(pl.length());
            if (outside > 0.1)
                fail = std::max(fail, layer->print_z);
        }
        return fail;
    };
    bool   partial  = false;
    double cut_min  = -std::numeric_limits<double>::max();
    double beside   = -std::numeric_limits<double>::max(); // highest such layer when it raised the cut
    for (const Surface3 &s : candidates)
        partial |= s.z_min < floor_z - EPSILON;
    const bool by_rise = partial;
    if (partial)
        cut_min = floor_z;
    else if (!dev.allow_material_beside) {
        ExPolygons all;
        for (const Surface3 &s : candidates)
            all.push_back(s.footprint);
        // P4: material beside may stand up to the nozzle tolerance above the lowest curved bead. With a
        // tolerance the strict test of the partial path applies here as well: the planner, not the head
        // check, keeps flat material right beside the surface within the tolerance (with none, P3's rule).
        double fail = beside_z(all, z_bottom - n_layers * h + beside_allow);
        if (fail == -std::numeric_limits<double>::max() && nozzle_tol > 0.)
            fail = flat_above_z(all, z_bottom - n_layers * h + h + nozzle_tol);
        if (fail > -std::numeric_limits<double>::max()) {
            partial = true;
            beside  = fail;
            // Only as far as needed: with a tolerance the material may stand beside_allow above the cut.
            cut_min = fail - beside_allow;
        }
    }
    double                cut = -std::numeric_limits<double>::max();
    std::vector<Surface3> surfaces;
    size_t                left_flat    = 0;  // surfaces wholly below the cut
    size_t                cut_surfaces = 0;  // surfaces curved above the cut only
    double                cut_rise     = 0.; // the tallest of those, whole
    if (!partial)
        surfaces = std::move(candidates);
    else
        for (;;) {
            auto it = std::lower_bound(planar_tops.begin(), planar_tops.end(), cut_min - EPSILON);
            // Every round must raise the cut (P4: cut_min may sit below the material that raised it).
            while (it != planar_tops.end() && cut > -std::numeric_limits<double>::max() && *it <= cut + EPSILON)
                ++it;
            if (it == planar_tops.end() || *it + h > z_top + EPSILON || *it >= top_layer->print_z - EPSILON)
                return refuse(beside > -std::numeric_limits<double>::max() ?
                                  Slic3r::format(_u8L("part of it next to the curved surface rises into the curved layers (at Z %1% mm), "
                                                      "and no layer leaves a curved part above it."),
                                                 mm_str(beside)) :
                                  Slic3r::format(_u8L("its curved top surface rises %1% mm, more than the %2% mm the print head allows "
                                                      "(lowest head part %3% mm above the nozzle tip, less a %4% mm margin), and no layer "
                                                      "leaves a curved part above it."),
                                                 mm_str(z_top - z_bottom), mm_str(head.allowed_rise_mm),
                                                 mm_str(head.allowed_rise_mm + head.clearance_margin_mm),
                                                 mm_str(head.clearance_margin_mm)));
            cut = *it;
            ExPolygons kept;
            for (const Surface3 &s : candidates)
                if (s.z_max >= cut + h - EPSILON)
                    kept.push_back(s.footprint);
            // Material beside what is kept, above the cut: raise the cut over it and try again. Then
            // (strict, see above) flat extrusions beside it above the lowest curved bead (cut + h).
            // P3: allow_material_beside skips both: the cut stays where the rise puts it.
            // P4: with a nozzle tolerance both let material stand up to the tolerance above the lowest
            // curved bead (cut + h), and the cut is raised only as far as that needs.
            double fail = dev.allow_material_beside ? -std::numeric_limits<double>::max() : beside_z(kept, cut + beside_allow);
            if (fail == -std::numeric_limits<double>::max() && !dev.allow_material_beside)
                fail = flat_above_z(kept, cut + h + nozzle_tol);
            if (fail > -std::numeric_limits<double>::max()) {
                beside  = fail;
                cut_min = nozzle_tol > 0. ? fail - h - nozzle_tol : fail;
                continue;
            }
            for (Surface3 &s : candidates) {
                if (s.z_max < cut + h - EPSILON) {
                    ++left_flat;
                    continue;
                }
                if (s.z_min < cut - EPSILON) {
                    ++cut_surfaces;
                    cut_rise = std::max(cut_rise, s.z_max - s.z_min);
                }
                surfaces.push_back(std::move(s));
            }
            break;
        }
    // Why the block was cut, for the summary: the head's rise (floor_z) or material beside it.
    // Beside decided when it raised the cut at all and the rise did not set it (a cut raised by material
    // beside may lie below the rise floor, review d).
    const bool cut_by_beside = partial && beside > -std::numeric_limits<double>::max() && (!by_rise || cut > floor_z + EPSILON);

    // The planar body is cut where the surfaces are (footprint). The block's own layers: each curved
    // layer k covers the footprint (whole surfaces), or, when partial, the part of it where layer k lies
    // at least one layer above the cut (layer_area[k], from the mesh: the surface is at least
    // cut + (n - k) h there).
    ExPolygons footprint;
    double     z_lo = std::numeric_limits<double>::max(), z_hi = -std::numeric_limits<double>::max();
    double     slope_used = 0.;
    for (const Surface3 &s : surfaces) {
        footprint.push_back(s.footprint);
        z_lo = std::min(z_lo, s.z_min);
        z_hi = std::max(z_hi, s.z_max);
        slope_used = std::max(slope_used, s.slope_max_deg);
    }
    std::vector<ExPolygons> layer_area;
    if (partial) {
        z_lo = cut;
        // The slope the block follows is the curved part's only: the head check lets the tip land meet
        // the neighbouring bead up to this slope, so the flat lower part must not widen it.
        slope_used = 0.;
        for (const CurvedTopsFacet &f : facets)
            if (f.z_max > cut + EPSILON)
                for (const Surface3 &s : surfaces)
                    if (s.footprint.contains(f.probe)) {
                        slope_used = std::max(slope_used, f.slope_deg);
                        break;
                    }
        // slice_mesh_ex() takes its heights in ascending order: m = n - k layers above the cut.
        std::vector<float> zs;
        for (int m = 1; m <= n_layers; ++m)
            zs.push_back(float(cut + m * h + ground - 0.5 * EPSILON));
        const std::vector<ExPolygons> above_cut = slice_mesh_ex(its, zs);
        for (int k = 0; k < n_layers; ++k)
            layer_area.push_back(intersection_ex(footprint, above_cut[size_t(n_layers - k - 1)]));
        if (layer_area.back().empty())
            return refuse(Slic3r::format(_u8L("no part of its curved top surface lies a layer above Z %1% mm, the lowest a curved layer may "
                                              "start for the print head."),
                                         mm_str(cut)));
    }
    const ExPolygons &tip_area = partial ? layer_area.back() : footprint; // where the nozzle tip goes
    // What is actually curved: from the cut, or from the lowest curved surface when every one of them
    // lies wholly above it (a cut raised by material beside them can be below their lowest point, and
    // below the head's rise floor; the span reported must be the curved part's, review d).
    double curved_lo = z_lo;
    if (partial) {
        curved_lo = std::numeric_limits<double>::max();
        for (const Surface3 &s : surfaces)
            curved_lo = std::min(curved_lo, s.z_min);
        curved_lo = std::max(curved_lo, cut);
    }
    const double rise_used   = z_hi - curved_lo;
    const double band_bottom = partial ? cut : z_lo - n_layers * h;

    // 2. Clearance: nothing outside the surfaces may reach into the curved band. With partial curving
    //    the lower part of a cut surface is inside the footprint: its layers above the cut are cut away
    //    below (step 4), and the ones up to the cut stand no higher than the cut, under every curved bead.
    // P3 notes: what a developer override let through, said with the summary.
    std::vector<std::string> dev_notes;
    const ExPolygons footprint_grown = offset_ex(footprint, scale_(0.5));
    for (const Layer *layer : object.layers())
        if (layer->print_z > band_bottom + beside_allow + EPSILON) {
            const double outside = area(diff_ex(layer->lslices, footprint_grown)) * SCALING_FACTOR * SCALING_FACTOR;
            if (outside > OUTSIDE_TOL_MM2) {
                const std::string why = Slic3r::format(_u8L("part of it next to the curved surface rises into the curved layers (at Z %1% mm)."),
                                                       mm_str(layer->print_z));
                if (!dev.allow_material_beside)
                    return refuse(why);
                dev_notes.push_back(_u8L("Developer override: curved although ") + why);
                break;
            }
        }

    // 2b. Supports and side stabilizers (P2): generated before this step for an object with curved tops
    //     on (Print::process). Support material above the band bottom, within the head's reach of where
    //     the tip goes, would be met by the nozzle or a head part: the object prints planar.
    for (const SupportLayer *support : object.support_layers()) {
        if (support->print_z <= band_bottom + EPSILON)
            continue;
        const Polygons material = support_material_area(*support);
        if (material.empty())
            continue;
        const double height = support->print_z - band_bottom;
        // No head data (developer override only): the reach is unknown, so any support in the band counts.
        const double reach  = head.head ? curved_tops_head_reach_mm(*head.head, height) + dev.reach_margin_mm :
                                          std::numeric_limits<double>::infinity();
        const bool   hit    = std::isinf(reach) ||
                         area(intersection_ex(material, offset_ex(tip_area, float(scale_(reach))))) * SCALING_FACTOR * SCALING_FACTOR > 0.01;
        if (hit && dev.allow_supports_in_reach) {
            dev_notes.push_back(Slic3r::format(_u8L("Developer override: curved although its supports (or side stabilizers) reach Z %1% mm, "
                                                    "into the curved layers (from Z %2% mm), within the print head's reach."),
                                               mm_str(support->print_z), mm_str(band_bottom)));
            break;
        }
        if (hit)
            return refuse(std::isinf(reach) ?
                              Slic3r::format(_u8L("its supports (or side stabilizers) reach Z %1% mm, into the curved layers (from Z %2% mm), "
                                                  "and the print head's gantry would pass over them."),
                                             mm_str(support->print_z), mm_str(band_bottom)) :
                              Slic3r::format(_u8L("its supports (or side stabilizers) reach Z %1% mm, into the curved layers (from Z %2% mm), "
                                                  "within %3% mm of the curved surface, where the print head passes."),
                                             mm_str(support->print_z), mm_str(band_bottom), mm_str(reach)));
    }

    // R4: the travels in the block run level at the safe Z; it must be within the printer.
    const double base_z     = top_layer->print_z;
    const double safe_z     = std::max(z_hi + dev.safe_lift_mm, base_z + dev.safe_lift_mm);
    const float  safe_delta = float(safe_z - base_z);
    if (safe_z > pcfg.printable_height.value + EPSILON)
        return refuse(Slic3r::format(_u8L("the travels over the curved surface would need Z %1% mm, above the printable height of %2% mm."),
                                     mm_str(safe_z), mm_str(pcfg.printable_height.value)));

    // 3. Conformal layers, built before anything planar is touched, so a refusal leaves the object
    //    as it was. Appended to the top layer at the end.
    LayerRegion *top_region = top_layer->regions().front();
    const Flow   wall_flow  = top_region->flow(frExternalPerimeter, h);
    const Flow   solid_flow = top_region->flow(frTopSolidInfill, h);

    // Top of the planar body at a point whose curved surface is at s: the highest kept layer. With
    // partial curving every layer up to the cut is kept (the plateau).
    const auto planar_top_below = [&planar_tops, n_layers, h, partial, cut](double s) {
        const double limit = s - n_layers * h + EPSILON;
        auto it = std::upper_bound(planar_tops.begin(), planar_tops.end(), limit);
        const double top = it == planar_tops.begin() ? 0.0 : *(it - 1);
        return partial ? std::max(top, cut) : top;
    };

    auto conformal = std::make_unique<ExtrusionEntityCollection>();
    conformal->no_sort = true;
    size_t           n_paths      = 0;
    ContourZSamples *last_samples = nullptr; // the block's last path, for the exit lift (R5)
    // P3 hard limits, checked when developer overrides are in force: every curved Z finite and above
    // the first layer, never overridable.
    bool   bad_sample    = false;
    double lowest_bead_z = std::numeric_limits<double>::max();
    // P3: the comment written before the block's first path when overrides are in force.
    std::string block_comment;
    if (dev.active) {
        block_comment = ";CURVED_TOPS_DEV_OVERRIDES_START\n; " + dev.active_message() + "\n";
        for (const std::string &s : dev.carried_list)
            block_comment += "; - " + s + "\n";
        block_comment += ";CURVED_TOPS_DEV_OVERRIDES_END\n";
    }

    // P4: width and flow per path (the concentric pattern's loops vary in width where they close up).
    const auto add_path = [&](const Points &raw, ExtrusionRole role, float width, double mm3_per_mm, int k) {
        const Points pts = resample(raw);
        if (pts.size() < 2)
            return;
        auto samples = std::make_shared<ContourZSamples>();
        samples->curved_top   = true;
        samples->safe_z_delta = safe_delta;
        if (dev.active) {
            // 0 (the built-in cap) unless overridden.
            if (dev.speed_cap_mm_s != CURVED_TOPS_MAX_SPEED_MM_S)
                samples->speed_cap = float(dev.speed_cap_mm_s);
            if (dev.wipe_speed_cap_mm_s != CURVED_TOPS_MAX_SPEED_MM_S)
                samples->wipe_speed_cap = float(dev.wipe_speed_cap_mm_s);
            if (n_paths == 0)
                samples->block_comment = block_comment;
        }
        samples->points       = pts;
        samples->z.reserve(pts.size());
        samples->flow.reserve(pts.size());
        const double below_surface = (n_layers - 1 - k) * h;
        std::vector<double> heights;
        heights.reserve(pts.size());
        for (const Point &p : pts)
            heights.push_back(surface(unscale<double>(p.x()), unscale<double>(p.y())));
        if (partial) {
            // The ray slipped off the edge: hold the height of the nearest sample along the path that
            // hit (the block may mix surfaces of different heights, so no single rim height fits).
            double last = std::numeric_limits<double>::quiet_NaN();
            for (double &s : heights) {
                if (std::isfinite(s))
                    last = s;
                else
                    s = last;
            }
            last = std::numeric_limits<double>::quiet_NaN();
            for (auto it = heights.rbegin(); it != heights.rend(); ++it) {
                if (std::isfinite(*it))
                    last = *it;
                else
                    *it = last;
            }
        }
        for (size_t i = 0; i < pts.size(); ++i) {
            double s = heights[i];
            if (!std::isfinite(s))
                // The ray slipped off the edge: hold the surface's rim height (partial, no sample of
                // the path hit at all: this layer's own rim, one layer above the cut).
                s = partial ? cut + (n_layers - k) * h : z_lo;
            double z = s - below_surface;
            // Partial curving: a curved bead is never lower than one layer above the cut, the top of
            // everything printed flat around it (layer_area, from the mesh, may differ from the ray by
            // a sliver at its rim).
            if (partial)
                z = std::max(z, cut + h);
            // The lowest curved layer at this point fills the gap down to the planar body: layer 0, or
            // (partial) a layer whose layer below does not reach this point.
            const bool lowest = k == 0 || (partial && s - (n_layers - k) * h < cut + h - EPSILON);
            const float flow  = lowest ? float(curved_tops_gap_flow(z - planar_top_below(s), h)) : 1.f;
            samples->z.push_back(float(z - base_z));
            samples->flow.push_back(flow);
            bad_sample |= !std::isfinite(z) || !std::isfinite(flow);
            lowest_bead_z = std::min(lowest_bead_z, z);
        }
        samples->build_index();
        last_samples = samples.get();
        ExtrusionPath path(role, mm3_per_mm, width, float(h));
        path.polyline.points = pts;
        path.z_contour       = std::move(samples);
        conformal->entities.push_back(new ExtrusionPath(std::move(path)));
        ++n_paths;
    };

    const CurvedTopsPattern pattern = ocfg.curved_tops_pattern.value;
    const double min_gap_width = curved_tops_min_gap_width(pcfg);
    // P5: the head check applies the nozzle tolerance only on a plate without Z contouring (GCode.cpp); with Z contouring
    // on any object (developer overrides only), Monotonic keeps P4's paths, which the strict check passes.
    bool plate_contoured = false;
    for (const PrintObject *other : print->objects())
        plate_contoured |= other->need_z_contouring();
    const double pattern_tol = plate_contoured ? 0. : nozzle_tol;
    for (int k = 0; k < n_layers; ++k) {
        const ExtrusionRole fill_role = k == n_layers - 1 ? erTopSolidInfill : erSolidInfill;
        double layer_open = 0., layer_area_mm2 = 0.;
        size_t layer_starts = 0, layer_gap = 0;
        CurvedTopsGapStats gap_stats;
        for (const ExPolygon &fp : partial ? layer_area[k] : footprint) {
            // One wall loop along the surface's outline and the fill inside it in the chosen pattern, with the
            // gaps between them filled (P4, curved_tops_layer_paths(); P5: Monotonic only with a nozzle tolerance).
            bool layer_failed = false;
            const std::vector<CurvedTopsFillPath> paths = curved_tops_layer_paths(fp, pattern, wall_flow, solid_flow, k, pcfg, ocfg,
                                                                                  &layer_failed, pattern_tol, &gap_stats);
            for (const CurvedTopsFillPath &p : paths) {
                add_path(p.points, p.wall ? erExternalPerimeter : fill_role, p.width, p.mm3_per_mm, k);
                layer_gap += p.gap ? 1 : 0;
            }
            // Review S3: never a curved layer with its wall and no fill.
            if (layer_failed)
                return refuse(Slic3r::format(_u8L("the fill of curved layer %1% could not be built."), k + 1));
            // P5: for the log (the owner's test file tables): what the layer leaves open and how often it travels.
            layer_open += curved_tops_uncovered_mm2(fp, paths, h, min_gap_width);
            layer_area_mm2 += fp.area() * SCALING_FACTOR * SCALING_FACTOR;
            layer_starts += curved_tops_path_starts(paths);
        }
        BOOST_LOG_TRIVIAL(info) << "Curved tops: layer " << k + 1 << " of " << n_layers << ", pattern " << int(pattern) << ": open "
                                << layer_open << " mm2 of " << layer_area_mm2 << " mm2 ("
                                << (layer_area_mm2 > 0. ? 100. * layer_open / layer_area_mm2 : 0.) << " %), " << layer_starts
                                << " path starts (travels), " << layer_gap << " gap-fill paths; gap beads " << gap_stats.candidates
                                << " found (" << gap_stats.candidate_mm3 << " mm3), " << gap_stats.attached << " joined, "
                                << gap_stats.isolated << " on their own, " << gap_stats.dropped << " left out, "
                                << gap_stats.kept_mm3 << " mm3 printed, shortest on its own " << gap_stats.shortest_isolated_mm << " mm";
    }
    if (n_paths == 0 || last_samples == nullptr)
        return refuse(_u8L("no curved path could be built on its top surface."));
    // P3: whatever the overrides, no curved move goes below the first layer (into the bed) or to an
    // invalid height. Not overridable.
    if (dev.active) {
        const double floor_bead = object.layers().front()->print_z;
        if (bad_sample)
            return refuse(_u8L("a curved move would get an invalid height (developer overrides cannot change this)."));
        if (lowest_bead_z < floor_bead - EPSILON)
            return refuse(Slic3r::format(_u8L("a curved move would reach Z %1% mm, below the first layer (Z %2% mm) (developer overrides "
                                              "cannot change this)."),
                                         mm_str(lowest_bead_z), mm_str(floor_bead)));
    }
    // R5: after the block's last path the emitter retracts and lifts to the safe Z before anything
    // else (the end G-code, a following travel) can move the head across the surface.
    last_samples->exit_lift = true;

    // 4. Lower the planar body: inside the surfaces, keep a layer only where it is at least N
    //    layers below the surface, i.e. where the mesh still has material at print_z + N*h.
    std::vector<size_t> band_layers;
    std::vector<float>  probe_z;
    for (size_t i = 1; i < object.layers().size(); ++i) {
        const Layer *layer = object.layers()[i];
        if (layer->print_z > band_bottom + EPSILON) {
            band_layers.push_back(i);
            probe_z.push_back(float(layer->print_z + n_layers * h + ground - 0.5 * EPSILON));
        }
    }
    const std::vector<ExPolygons> above = slice_mesh_ex(its, probe_z);
    const BoundingBox             footprint_bb = get_extents(footprint);
    // Owner, P5 (rim gap): with partial curving the curved block prints only where the surface lies at least a layer above
    // the cut (layer_area, largest for the top curved layer). Elsewhere in the footprint - the ring where the surface lies
    // between the cut and one layer above it - planar material above the cut is kept, as for any flat top: dropping it
    // there left that ring printed by neither, a strip of the plateau showing all round the curved block. It stands no
    // higher than the lowest curved bead (cut + h): the layer above it is sliced at cut + 1.5 h, where the surface is
    // inside layer_area.
    const ExPolygons cover = partial ? layer_area.back() : footprint;
    // From here on the planar walls and fills are cut; only the perimeter step restores them.
    object.set_curved_tops_clipped(true);
    for (size_t j = 0; j < band_layers.size(); ++j) {
        const ExPolygons drop = partial ? intersection_ex(diff_ex(footprint, above[j]), cover) : diff_ex(footprint, above[j]);
        if (drop.empty())
            continue;
        Layer *layer = object.layers()[band_layers[j]];
        for (LayerRegion *region : layer->regions()) {
            clip_collection(region->perimeters, drop, footprint_bb);
            clip_collection(region->fills, drop, footprint_bb);
            clip_collection(region->thin_fills, drop, footprint_bb);
        }
    }
    const size_t solid_layers_added = solid_under_curved_block(object, its, ground, cover, partial, cut, n_layers, h);
    if (solid_layers_added > 0)
        BOOST_LOG_TRIVIAL(info) << "Curved tops: solid infill under the curved block on " << solid_layers_added << " layer region(s)";

    top_region->fills.entities.push_back(conformal.release());
    top_layer->curved_tops_block     = true;
    top_layer->curved_tops_slope_deg = float(slope_used);
    BOOST_LOG_TRIVIAL(info) << "Curved tops: " << surfaces.size() << " surface(s), Z " << z_lo << ".." << z_hi << ", "
                            << n_layers << " layers, " << n_paths << " paths, max angle " << max_angle << " deg ("
                            << head.source << ")";

    result.applied = true;
    result.summary = Slic3r::format(_u8L("Curved tops on %1%: %2% surface(s), %3% curved layers; rise %4% mm of %5% mm allowed, steepest slope "
                                         "%6% of %7% degrees allowed; curved moves at most %8% mm/s. Print head data: %9%."),
                                    object.model_object()->name, surfaces.size(), n_layers, mm_str(rise_used),
                                    mm_str(head.allowed_rise_mm), deg_str(slope_used), deg_str(max_angle),
                                    float_to_string_decimal_point(dev.speed_cap_mm_s, 0), head.source);
    // P3: a developer override in force also gives the lowest curved bead and the travel height.
    if (dev.active)
        result.notes.push_back(Slic3r::format(_u8L("Lowest curved bead at Z %1% mm, highest curved point at Z %2% mm, travels at Z %3% mm."),
                                              mm_str(lowest_bead_z), mm_str(z_hi), mm_str(safe_z)));
    for (std::string &note : dev_notes)
        result.notes.push_back(std::move(note));
    if (!nozzle.note.empty())
        result.notes.push_back(nozzle.note);
    if (partial) {
        // Material beside raised the cut: said even when every curved surface lies wholly above it (the
        // flanks around it are what prints flat).
        if (cut_surfaces > 0 || cut_by_beside)
            result.notes.push_back(
                (cut_by_beside ?
                     Slic3r::format(_u8L("Partly curved: below Z %1% mm, parts of the object next to the curved surface stand where the "
                                         "print head would pass, so the curved layers start above them."),
                                    mm_str(cut)) :
                     Slic3r::format(_u8L("Partly curved: %1% surface(s) rise up to %2% mm, more than the %3% mm the print head allows."),
                                    cut_surfaces, mm_str(cut_rise), mm_str(head.allowed_rise_mm))) +
                " " +
                Slic3r::format(_u8L("The top %1% mm (from Z %2% mm to %3% mm) is curved; the part below Z %4% mm prints as flat layers."),
                               mm_str(rise_used), mm_str(curved_lo), mm_str(z_hi), mm_str(cut)));
        if (left_flat > 0)
            result.notes.push_back(Slic3r::format(_u8L("%1% lower surface(s) stay flat: the curved layers print as one block, which may span "
                                                       "at most %2% mm in height, and only the highest part of the object is curved."),
                                                  left_flat, mm_str(head.allowed_rise_mm)));
    }
    // Angle above what the head (or the absolute ceiling) allows: say which value is used (C).
    if (ocfg.curved_tops_max_angle.value > max_angle + 0.05)
        result.notes.push_back(Slic3r::format(_u8L("Curved tops max angle is set to %1% degrees; %2% degrees is used, the most this print head "
                                                   "allows (curved tops never goes above %3% degrees)."),
                                              deg_str(ocfg.curved_tops_max_angle.value), deg_str(max_angle),
                                              deg_str(head.angle_ceiling_deg)));
    // P4: the smoothed edge may reach onto facets a little steeper than the limit (at most
    // CURVED_TOPS_EDGE_ANGLE_TOLERANCE_DEG); said, so the slope above is not a puzzle.
    if (slope_used > max_angle + 0.05)
        result.notes.push_back(Slic3r::format(_u8L("The edge of the curved surface follows the line where the surface reaches %1% degrees "
                                                   "smoothly across the model's facets; along it the facets slope up to %2% degrees (at most "
                                                   "%3% degrees over the limit)."),
                                              deg_str(max_angle), deg_str(slope_used), deg_str(CURVED_TOPS_EDGE_ANGLE_TOLERANCE_DEG)));
    // D8: beyond what has been printed on this printer is allowed, but said.
    if (head.verified_rise_mm > 0. && (rise_used > head.verified_rise_mm + EPSILON || slope_used > head.verified_angle_deg + 0.05))
        result.notes.push_back(Slic3r::format(_u8L("This curved surface (rise %1% mm, slope %2% degrees) goes beyond what has been printed on "
                                                   "this printer so far (rise %3% mm, slope %4% degrees, %5%). Watch the first print."),
                                              mm_str(rise_used), deg_str(slope_used), mm_str(head.verified_rise_mm),
                                              deg_str(head.verified_angle_deg), head.verified_on));
    bool ironed = false;
    for (const LayerRegion *region : top_layer->regions())
        ironed |= region->region().config().ironing_type.value != IroningType::NoIroning;
    if (ironed)
        result.notes.push_back(_u8L("The curved surface is not ironed."));
    return result;
}

} // namespace Slic3r
