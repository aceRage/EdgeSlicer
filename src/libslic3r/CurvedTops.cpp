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
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <boost/log/trivial.hpp>

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

} // namespace

CurvedTopsHeadLimits curved_tops_head_limits(const std::shared_ptr<const HeadClearance> &head, const std::string &printer_model)
{
    CurvedTopsHeadLimits out;
    out.head = head;
    if (!head) {
        out.refusal = Slic3r::format(_u8L("EdgeSlicer has no print head clearance data for the printer model \"%1%\"."), printer_model) +
                      only_on_verified_printers();
        return out;
    }
    out.known  = true;
    out.source = head->name + (head->calibrated ? std::string() : " (" + _u8L("an estimate, not a measurement") + ")");

    // Allowed rise: the lowest head part above the tip, less the margin.
    const double lowest = head->lowest_box_z();
    out.allowed_rise_mm = std::isfinite(lowest) ? lowest - CURVED_TOPS_CLEARANCE_MARGIN_MM : 0.;
    // Head angle cap: the nozzle flank, less a margin, never above the ceiling.
    out.max_slope_deg = std::clamp(head->nozzle_flank_angle_deg(CURVED_TOPS_LAND_TOLERANCE_MM) - CURVED_TOPS_FLANK_MARGIN_DEG, 0.,
                                   CURVED_TOPS_MAX_ANGLE_CEILING);

    const bool verified = head->curved_tops_verified_for(printer_model);
    if (verified) {
        out.verified_rise_mm   = head->curved_tops_verified->rise_mm;
        out.verified_angle_deg = head->curved_tops_verified->angle_deg;
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
    else if (!verified)
        // P1: an estimated head nobody has printed curved tops with stays refused until the
        // acknowledgement (P2) lets the user take that risk knowingly.
        out.refusal = Slic3r::format(_u8L("the print head data for %1% (%2%) is an estimate that no curved print has checked yet."),
                                     printer_model, head->name) +
                      only_on_verified_printers();
    else
        out.usable = true;
    return out;
}

CurvedTopsHeadLimits curved_tops_head_limits(const PrintConfig &config)
{
    return curved_tops_head_limits(resolve_head_clearance(config), config.printer_model.value);
}

double curved_tops_effective_max_angle(double user_max_deg, const CurvedTopsHeadLimits &head)
{
    return std::clamp(std::min(user_max_deg, head.max_slope_deg), 0.0, CURVED_TOPS_MAX_ANGLE_CEILING);
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

constexpr double SAMPLE_STEP_MM   = 0.5;   // XY resampling of conformal paths
constexpr double MIN_AREA_MM2     = 4.0;   // smaller surfaces stay planar
constexpr double SAFE_LIFT_MM     = 1.0;   // travel this far above the highest curved point
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

    bool blocked() const
    {
        return !head_usable || not_alone || by_object || zaa || spiral || raft || vlh || multi_filament || smooth_tower || local_z ||
               nozzles || skirt;
    }
};

GuardSettings guard_settings(const PrintObject &object, int multi_filament)
{
    const Print             &print = *object.print();
    const PrintConfig       &pcfg  = print.config();
    const PrintObjectConfig &ocfg  = object.config();
    const CurvedTopsHeadLimits head = curved_tops_head_limits(pcfg);
    GuardSettings s;
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
        << ";support=" << s.support << ";height=" << s.printable_height << ";ct=" << s.layers << ',' << s.angle << ',' << s.layer_height;
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
    const PrintConfig         &pcfg = print->config();
    const CurvedTopsHeadLimits head = curved_tops_head_limits(pcfg);
    if (!head.usable)
        return refuse(head.refusal);
    if (print->objects().size() != 1 || object.instances().size() != 1)
        return refuse(_u8L("there is more than one object or copy on the plate (curved tops handles one object for now)."));
    if (pcfg.print_sequence.value == PrintSequence::ByObject)
        return refuse(_u8L("the plate prints by object; curved tops needs the by-layer sequence."));
    // Z contouring moves the same top layers up and down; the curved-tops head check is also more
    // lenient on the block's extrusions than on Z contouring moves. One or the other per object.
    if (object.need_z_contouring())
        return refuse(_u8L("Z contouring is on for it as well; curved tops and Z contouring do not run on the same object."));
    if (pcfg.spiral_mode.value)
        return refuse(_u8L("spiral vase mode is on."));
    if (ocfg.raft_layers.value > 0)
        return refuse(_u8L("it has a raft."));
    if (object.model_object() != nullptr && !object.model_object()->layer_height_profile.empty())
        return refuse(_u8L("it has a variable layer height."));
    // The tower stands at layer height next to the part; with more than one filament there is one
    // (and tool changes). A smooth timelapse builds one even for a single filament.
    if (print->extruders(true).size() > 1)
        return refuse(_u8L("the plate uses more than one filament (curved tops handles one filament and no prime tower for now)."));
    if (print->has_wipe_tower() && pcfg.timelapse_type.value == TimelapseType::tlSmooth)
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
    if (pcfg.skirt_loops.value > 0 && (pcfg.draft_shield.value != dsDisabled || pcfg.skirt_height.value > 1))
        return refuse(_u8L("it has a draft shield or a skirt taller than one layer."));
    // Supports and side stabilizers (P2): no refusal here. Print::process() runs this step after the
    // support step for an object with curved tops on, and the block is checked against the support
    // layers below, once the band is known.
    if (top_layer->regions().empty())
        return refuse(_u8L("its top layer has nothing to print."));

    const int    n_layers  = std::max(1, ocfg.curved_tops_layers.value);
    const double h         = ocfg.layer_height.value;
    const double max_angle = curved_tops_effective_max_angle(ocfg.curved_tops_max_angle.value, head);
    const double cos_max   = std::cos(max_angle * M_PI / 180.0);
    const double ground    = imesh.ground_level();

    // 1. Candidate facets.
    const indexed_triangle_set &its = mesh.its;
    double mesh_z_max = -std::numeric_limits<double>::max();
    for (const stl_vertex &v : its.vertices)
        mesh_z_max = std::max(mesh_z_max, double(v.z()));
    const SurfaceHeight surface{imesh, mesh_z_max + 5.0, ground};

    struct Facet { Polygon xy; double z_min, z_max; Point centroid; double slope_deg; };
    std::vector<Facet> facets;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3d a = its.vertices[f[0]].cast<double>();
        const Vec3d b = its.vertices[f[1]].cast<double>();
        const Vec3d c = its.vertices[f[2]].cast<double>();
        const Vec3d n = (b - a).cross(c - a);
        const double len = n.norm();
        if (len < 1e-12 || n.z() / len < cos_max)
            continue;
        const Vec3d centroid = (a + b + c) / 3.0;
        if (imesh.query_ray_hit(centroid + Vec3d(0., 0., 1e-3), {0.0, 0.0, 1.0}).is_hit())
            continue; // something above it: not a top surface
        Polygon poly({Point::new_scale(a.x(), a.y()), Point::new_scale(b.x(), b.y()), Point::new_scale(c.x(), c.y())});
        if (std::abs(poly.area()) < 1.)
            continue;
        if (!poly.is_counter_clockwise())
            poly.reverse();
        facets.push_back({std::move(poly), std::min({a.z(), b.z(), c.z()}) - ground, std::max({a.z(), b.z(), c.z()}) - ground,
                          Point::new_scale(centroid.x(), centroid.y()),
                          std::acos(std::clamp(n.z() / len, -1., 1.)) * 180. / M_PI});
    }
    if (facets.empty())
        return refuse(Slic3r::format(_u8L("it has no top surface flatter than %1% degrees."), deg_str(max_angle)));

    Polygons tris;
    tris.reserve(facets.size());
    for (const Facet &f : facets)
        tris.push_back(f.xy);
    // A tiny closing heals the cracks between the projected triangles.
    const ExPolygons pieces = offset2_ex(union_ex(tris), scale_(0.01), -scale_(0.01));

    // Every surface that is curved (rises at least one layer) and large enough, however tall.
    std::vector<Surface3> candidates;
    for (const ExPolygon &piece : pieces) {
        Surface3 s;
        s.footprint = piece;
        for (const Facet &f : facets)
            if (piece.contains(f.centroid)) {
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
    // the curved part, so that nothing printed flat stands above the cut and every curved bead lies at
    // least one layer above it.
    //
    // R2 (nothing beside the footprint in the band) decides the same way (P2): when material outside
    // the surfaces stands in the band of whole curving (the steep flanks of a rounded part, say), the
    // cut is raised to the highest layer top such material reaches, so it all prints flat below the
    // plateau and every curved bead lies at least one layer above it. The check itself is unchanged.
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
    bool   partial  = false;
    double cut_min  = -std::numeric_limits<double>::max();
    double beside   = -std::numeric_limits<double>::max(); // highest such layer when it raised the cut
    for (const Surface3 &s : candidates)
        partial |= s.z_min < floor_z - EPSILON;
    if (partial)
        cut_min = floor_z;
    else {
        ExPolygons all;
        for (const Surface3 &s : candidates)
            all.push_back(s.footprint);
        const double fail = beside_z(all, z_bottom - n_layers * h);
        if (fail > -std::numeric_limits<double>::max()) {
            partial = true;
            cut_min = beside = fail;
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
            if (it == planar_tops.end() || *it + h > z_top + EPSILON || *it >= top_layer->print_z - EPSILON)
                return refuse(beside > -std::numeric_limits<double>::max() ?
                                  Slic3r::format(_u8L("part of it next to the curved surface rises into the curved layers (at Z %1% mm), "
                                                      "and no layer leaves a curved part above it."),
                                                 mm_str(beside)) :
                                  Slic3r::format(_u8L("its curved top surface rises %1% mm, more than the %2% mm the print head allows "
                                                      "(lowest head part %3% mm above the nozzle tip, less a %4% mm margin), and no layer "
                                                      "leaves a curved part above it."),
                                                 mm_str(z_top - z_bottom), mm_str(head.allowed_rise_mm),
                                                 mm_str(head.allowed_rise_mm + CURVED_TOPS_CLEARANCE_MARGIN_MM),
                                                 mm_str(CURVED_TOPS_CLEARANCE_MARGIN_MM)));
            cut = *it;
            ExPolygons kept;
            for (const Surface3 &s : candidates)
                if (s.z_max >= cut + h - EPSILON)
                    kept.push_back(s.footprint);
            // Material beside what is kept, above the cut: raise the cut over it and try again.
            const double fail = beside_z(kept, cut);
            if (fail > -std::numeric_limits<double>::max()) {
                cut_min = beside = fail;
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
    const bool cut_by_beside = partial && beside > -std::numeric_limits<double>::max() && cut > floor_z + EPSILON;

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
        for (const Facet &f : facets)
            if (f.z_max > cut + EPSILON)
                for (const Surface3 &s : surfaces)
                    if (s.footprint.contains(f.centroid)) {
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
    const double rise_used   = z_hi - z_lo;
    const double band_bottom = partial ? cut : z_lo - n_layers * h;

    // 2. Clearance: nothing outside the surfaces may reach into the curved band. With partial curving
    //    the lower part of a cut surface is inside the footprint: its layers above the cut are cut away
    //    below (step 4), and the ones up to the cut stand no higher than the cut, under every curved bead.
    const ExPolygons footprint_grown = offset_ex(footprint, scale_(0.5));
    for (const Layer *layer : object.layers())
        if (layer->print_z > band_bottom + EPSILON) {
            const double outside = area(diff_ex(layer->lslices, footprint_grown)) * SCALING_FACTOR * SCALING_FACTOR;
            if (outside > OUTSIDE_TOL_MM2)
                return refuse(Slic3r::format(_u8L("part of it next to the curved surface rises into the curved layers (at Z %1% mm)."),
                                             mm_str(layer->print_z)));
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
        const double reach  = curved_tops_head_reach_mm(*head.head, height) + CURVED_TOPS_REACH_MARGIN_MM;
        const bool   hit    = std::isinf(reach) ||
                         area(intersection_ex(material, offset_ex(tip_area, float(scale_(reach))))) * SCALING_FACTOR * SCALING_FACTOR > 0.01;
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
    const double safe_z     = std::max(z_hi + SAFE_LIFT_MM, base_z + SAFE_LIFT_MM);
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

    const auto add_path = [&](const Points &raw, ExtrusionRole role, const Flow &flow, int k) {
        const Points pts = resample(raw);
        if (pts.size() < 2)
            return;
        auto samples = std::make_shared<ContourZSamples>();
        samples->curved_top   = true;
        samples->safe_z_delta = safe_delta;
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
            samples->z.push_back(float(z - base_z));
            samples->flow.push_back(lowest ? float(curved_tops_gap_flow(z - planar_top_below(s), h)) : 1.f);
        }
        samples->build_index();
        last_samples = samples.get();
        ExtrusionPath path(role, flow.mm3_per_mm(), float(flow.width()), float(h));
        path.polyline.points = pts;
        path.z_contour       = std::move(samples);
        conformal->entities.push_back(new ExtrusionPath(std::move(path)));
        ++n_paths;
    };

    for (int k = 0; k < n_layers; ++k) {
        const ExtrusionRole fill_role = k == n_layers - 1 ? erTopSolidInfill : erSolidInfill;
        for (const ExPolygon &fp : partial ? layer_area[k] : footprint) {
            // One wall loop along the surface's outline.
            for (const ExPolygon &loop : offset_ex(fp, -0.5 * wall_flow.scaled_width())) {
                Points contour = loop.contour.points;
                contour.push_back(contour.front());
                add_path(contour, erExternalPerimeter, wall_flow, k);
                for (const Polygon &hole : loop.holes) {
                    Points pts = hole.points;
                    pts.push_back(pts.front());
                    add_path(pts, erExternalPerimeter, wall_flow, k);
                }
            }
            // Solid fill inside it.
            const ExPolygons inner = offset_ex(fp, -(wall_flow.scaled_width() - 0.5 * wall_flow.scaled_spacing() +
                                                     0.5 * solid_flow.scaled_spacing()));
            for (const ExPolygon &area : inner) {
                std::unique_ptr<Fill> filler(Fill::new_from_type(ipRectilinear));
                filler->angle        = float((k % 2 == 0 ? 45. : 135.) * M_PI / 180.);
                filler->spacing      = solid_flow.spacing();
                filler->bounding_box = get_extents(area);
                FillParams params;
                params.density = 1.f;
                Surface srf(stTop, area);
                Polylines lines;
                try {
                    lines = filler->fill_surface(&srf, params);
                } catch (const std::exception &ex) {
                    BOOST_LOG_TRIVIAL(warning) << "Curved tops: fill failed: " << ex.what();
                }
                for (const Polyline &pl : lines)
                    add_path(pl.points, fill_role, solid_flow, k);
            }
        }
    }
    if (n_paths == 0 || last_samples == nullptr)
        return refuse(_u8L("no curved path could be built on its top surface."));
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
    // From here on the planar walls and fills are cut; only the perimeter step restores them.
    object.set_curved_tops_clipped(true);
    for (size_t j = 0; j < band_layers.size(); ++j) {
        const ExPolygons drop = diff_ex(footprint, above[j]);
        if (drop.empty())
            continue;
        Layer *layer = object.layers()[band_layers[j]];
        for (LayerRegion *region : layer->regions()) {
            clip_collection(region->perimeters, drop, footprint_bb);
            clip_collection(region->fills, drop, footprint_bb);
            clip_collection(region->thin_fills, drop, footprint_bb);
        }
    }

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
                                    float_to_string_decimal_point(CURVED_TOPS_MAX_SPEED_MM_S, 0), head.source);
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
                Slic3r::format(_u8L("The top %1% mm (from Z %2% mm to %3% mm) is curved; the part below Z %2% mm prints as flat layers."),
                               mm_str(z_hi - cut), mm_str(cut), mm_str(z_hi)));
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
                                              deg_str(CURVED_TOPS_MAX_ANGLE_CEILING)));
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
