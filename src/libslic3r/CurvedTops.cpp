// EdgeSlicer SPIKE (2026-11-12): curved (non-planar) top surfaces. See CurvedTops.hpp for the
// credits (fich-l/OrcaSlicer-Nonplanar v0.1, AGPL-3.0; PancakeSlicer, AGPL-3.0, as reference) and
// tests/spike_curved_tops.md for the findings.
//
// How it works, in the frame Z contouring already uses (mesh Z = print Z + imesh.ground_level()):
//
//  1. Surfaces. Every facet that faces up within the maximum angle and has nothing above it is a
//     candidate. Their XY projections are unioned; each connected piece is one surface. A surface
//     is kept when it is not flat (rises at least one layer), not tiny, and no taller than the
//     head's nozzle clearance.
//  2. Clearance. Nothing outside the kept surfaces may stand higher than the lowest curved layer,
//     otherwise the head would have to dip next to it. The spike simply refuses such objects.
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
#include "Layer.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "SLA/IndexedMesh.hpp"
#include "Surface.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"
#include "libslic3r.h"

namespace Slic3r {

CurvedTopsHeadClearance curved_tops_head_clearance(const PrintConfig &config)
{
    CurvedTopsHeadClearance head;
    // TODO(head-clearance): use the head-clearance model from feat/head-clearance-zaa when its
    // loader commit lands; these numbers are a deliberately conservative placeholder, NOT measured.
    // Elegoo Centauri Carbon: the nozzle tip is taken to stand only 2 mm proud of the lowest part
    // of the heater block / silicone sock / part-cooling duct, and the nozzle cone to clear 20
    // degrees. Both should be measured on the owner's CC1 before the limit is relaxed.
    if (config.printer_model.value.rfind("Elegoo Centauri Carbon", 0) == 0 &&
        config.printer_model.value.find("Carbon 2") == std::string::npos) {
        head.nozzle_height_mm = 2.0;
        head.max_slope_deg    = 20.0;
        head.source           = "Elegoo Centauri Carbon (hard-coded, conservative, unmeasured)";
    }
    return head;
}

double curved_tops_effective_max_angle(double user_max_deg, const CurvedTopsHeadClearance &head)
{
    return std::clamp(std::min(user_max_deg, head.max_slope_deg), 0.0, 89.0);
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

bool apply_curved_tops(PrintObject &object, const TriangleMesh &mesh, const sla::IndexedMesh &imesh)
{
    const PrintObjectConfig &ocfg  = object.config();
    const Print             *print = object.print();
    const auto               refuse = [&object](const std::string &why) {
        BOOST_LOG_TRIVIAL(warning) << "Curved tops: object \"" << object.model_object()->name << "\" left planar: " << why;
        return false;
    };

    if (object.layers().size() < 3)
        return refuse("too few layers");
    Layer *top_layer = object.layers().back();
    top_layer->curved_tops_block = false;
    for (LayerRegion *region : top_layer->regions())
        remove_previous_conformal(region->fills);

    // Spike guards.
    if (print->objects().size() != 1 || object.instances().size() != 1)
        return refuse("more than one object or instance on the plate");
    if (print->config().spiral_mode.value)
        return refuse("spiral vase mode");
    if (ocfg.raft_layers.value > 0)
        return refuse("raft");
    if (object.model_object() != nullptr && !object.model_object()->layer_height_profile.empty())
        return refuse("variable layer height");
    if (top_layer->regions().empty())
        return refuse("no region on the top layer");

    const int    n_layers = std::max(1, ocfg.curved_tops_layers.value);
    const double h        = ocfg.layer_height.value;
    const CurvedTopsHeadClearance head      = curved_tops_head_clearance(print->config());
    const double                  max_angle = curved_tops_effective_max_angle(ocfg.curved_tops_max_angle.value, head);
    const double                  cos_max   = std::cos(max_angle * M_PI / 180.0);
    const double                  ground    = imesh.ground_level();

    // 1. Candidate facets.
    const indexed_triangle_set &its = mesh.its;
    double mesh_z_max = -std::numeric_limits<double>::max();
    for (const stl_vertex &v : its.vertices)
        mesh_z_max = std::max(mesh_z_max, double(v.z()));
    const SurfaceHeight surface{imesh, mesh_z_max + 5.0, ground};

    struct Facet { Polygon xy; double z_min, z_max; Point centroid; };
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
                          Point::new_scale(centroid.x(), centroid.y())});
    }
    if (facets.empty())
        return refuse("no top surface under the maximum angle");

    Polygons tris;
    tris.reserve(facets.size());
    for (const Facet &f : facets)
        tris.push_back(f.xy);
    // A tiny closing heals the cracks between the projected triangles.
    const ExPolygons pieces = offset2_ex(union_ex(tris), scale_(0.01), -scale_(0.01));

    std::vector<Surface3> surfaces;
    for (const ExPolygon &piece : pieces) {
        Surface3 s;
        s.footprint = piece;
        for (const Facet &f : facets)
            if (piece.contains(f.centroid)) {
                s.z_min = std::min(s.z_min, f.z_min);
                s.z_max = std::max(s.z_max, f.z_max);
            }
        const double area_mm2 = piece.area() * SCALING_FACTOR * SCALING_FACTOR;
        const double rise     = s.z_max - s.z_min;
        if (area_mm2 < MIN_AREA_MM2 || rise < h || rise > head.nozzle_height_mm) {
            BOOST_LOG_TRIVIAL(info) << "Curved tops: surface skipped, area " << area_mm2 << " mm2, rise " << rise
                                    << " mm (head allows " << head.nozzle_height_mm << " mm)";
            continue;
        }
        surfaces.push_back(std::move(s));
    }
    if (surfaces.empty())
        return refuse("no surface that is both curved and within the head clearance");

    ExPolygons footprint;
    double     z_lo = std::numeric_limits<double>::max(), z_hi = -std::numeric_limits<double>::max();
    for (const Surface3 &s : surfaces) {
        footprint.push_back(s.footprint);
        z_lo = std::min(z_lo, s.z_min);
        z_hi = std::max(z_hi, s.z_max);
    }
    const double band_bottom = z_lo - n_layers * h;

    // 2. Clearance: nothing outside the surfaces may reach into the curved band.
    const ExPolygons footprint_grown = offset_ex(footprint, scale_(0.5));
    for (const Layer *layer : object.layers())
        if (layer->print_z > band_bottom + EPSILON) {
            const double outside = area(diff_ex(layer->lslices, footprint_grown)) * SCALING_FACTOR * SCALING_FACTOR;
            if (outside > OUTSIDE_TOL_MM2)
                return refuse("material outside the curved surface rises into the curved band at Z " +
                              std::to_string(layer->print_z));
        }

    // 3. Lower the planar body: inside the surfaces, keep a layer only where it is at least N
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

    // 4. Conformal layers, appended to the top layer.
    LayerRegion *top_region = top_layer->regions().front();
    const Flow   wall_flow  = top_region->flow(frExternalPerimeter, h);
    const Flow   solid_flow = top_region->flow(frTopSolidInfill, h);
    const double base_z     = top_layer->print_z;
    const float  safe_delta = float(std::max(z_hi + SAFE_LIFT_MM, base_z + SAFE_LIFT_MM) - base_z);

    std::vector<double> planar_tops;
    for (const Layer *layer : object.layers())
        planar_tops.push_back(layer->print_z);
    // Top of the planar body at a point whose curved surface is at s: the highest kept layer.
    const auto planar_top_below = [&planar_tops, n_layers, h](double s) {
        const double limit = s - n_layers * h + EPSILON;
        auto it = std::upper_bound(planar_tops.begin(), planar_tops.end(), limit);
        return it == planar_tops.begin() ? 0.0 : *(it - 1);
    };

    auto conformal = std::make_unique<ExtrusionEntityCollection>();
    conformal->no_sort = true;
    size_t n_paths = 0;

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
        for (const Point &p : pts) {
            double s = surface(unscale<double>(p.x()), unscale<double>(p.y()));
            if (!std::isfinite(s))
                s = z_lo; // the ray slipped off the edge: hold the surface's rim height
            const double z = s - below_surface;
            samples->z.push_back(float(z - base_z));
            samples->flow.push_back(k == 0 ? float(curved_tops_gap_flow(z - planar_top_below(s), h)) : 1.f);
        }
        samples->build_index();
        ExtrusionPath path(role, flow.mm3_per_mm(), float(flow.width()), float(h));
        path.polyline.points = pts;
        path.z_contour       = std::move(samples);
        conformal->entities.push_back(new ExtrusionPath(std::move(path)));
        ++n_paths;
    };

    for (int k = 0; k < n_layers; ++k) {
        const ExtrusionRole fill_role = k == n_layers - 1 ? erTopSolidInfill : erSolidInfill;
        for (const ExPolygon &fp : footprint) {
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
    if (n_paths == 0)
        return refuse("no conformal path could be built");

    top_region->fills.entities.push_back(conformal.release());
    top_layer->curved_tops_block = true;
    BOOST_LOG_TRIVIAL(info) << "Curved tops: " << surfaces.size() << " surface(s), Z " << z_lo << ".." << z_hi << ", "
                            << n_layers << " layers, " << n_paths << " paths, max angle " << max_angle << " deg ("
                            << head.source << ")";
    return true;
}

} // namespace Slic3r
