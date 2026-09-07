#include "ContourZ.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <limits>

#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Line.hpp"
#include "Point.hpp"
#include "Print.hpp"
#include "SLA/IndexedMesh.hpp"
#include "libslic3r.h"

namespace Slic3r {

// ---------------------------------------------------------------------------------------------
// ContourZSamples
// ---------------------------------------------------------------------------------------------

static inline std::pair<int64_t, int64_t> contour_key(const Point &p)
{
    return { int64_t(p.x()), int64_t(p.y()) };
}

void ContourZSamples::build_index()
{
    this->m_index.clear();
    this->m_index.reserve(this->points.size() * 2);
    for (size_t i = 0; i < this->points.size(); ++i)
        // A self-crossing path can visit the same XY twice; the first sample wins. Both samples
        // were raycast against the same mesh at the same layer, so they agree to within the
        // resampling resolution.
        this->m_index.emplace(contour_key(this->points[i]), this->z[i]);
}

float ContourZSamples::z_at(const Point &p) const
{
    if (this->points.empty())
        return 0.f;

    auto it = this->m_index.find(contour_key(p));
    if (it != this->m_index.end())
        return it->second;

    // Not one of the original samples: a seam split or a clip_end introduced it somewhere along a
    // sample segment. Interpolate along the nearest segment. This is rare (at most a couple of
    // points per path), so a linear scan is fine.
    const Vec2d q = p.cast<double>();
    double      best_d2 = std::numeric_limits<double>::max();
    float       best_z = this->z.front();
    for (size_t i = 0; i + 1 < this->points.size(); ++i) {
        const Vec2d a = this->points[i].cast<double>();
        const Vec2d b = this->points[i + 1].cast<double>();
        const Vec2d ab = b - a;
        const double len2 = ab.squaredNorm();
        double t = 0.0;
        if (len2 > 0.0)
            t = std::clamp((q - a).dot(ab) / len2, 0.0, 1.0);
        const Vec2d  foot = a + ab * t;
        const double d2 = (q - foot).squaredNorm();
        if (d2 < best_d2) {
            best_d2 = d2;
            best_z = float(this->z[i] + t * (double(this->z[i + 1]) - double(this->z[i])));
        }
    }
    return best_z;
}

// ---------------------------------------------------------------------------------------------
// The contouring pass
// ---------------------------------------------------------------------------------------------

static void contour_extrusion_entity(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntity *extr);

static double follow_slope_down(double angle_rad, double dist)
{
    return -dist * std::sin(angle_rad);
}

static double slope_from_normal(const Eigen::Vector3d &normal)
{
    // Ensure the normal is normalized.
    Eigen::Vector3d n = normal.normalized();
    // Angle between the normal and the vertical.
    return std::acos(std::abs(n.z()));
}

// EdgeSlicer guard for the still-open upstream bug OrcaSlicer#13552 ("perimeter replaced by
// artifacts when minimize wall height non-zero"). Upstream applies the slope drop as a HARD STEP
// at zaa_minimize_perimeter_height: a wall whose slope crosses the threshold part-way along gets a
// half-line-width Z jump mid-path, which is what shreds the perimeter. The upstream advice is to
// set the angle to 0, i.e. to turn the feature off. Instead we ramp the adjustment in continuously
// over this band above the threshold, so no sample can ever step by more than
// (half_width / band) * one_sample. Above threshold + band the result is identical to upstream.
static constexpr double ZAA_SLOPE_RAMP_DEGREES = 5.0;

// The whole per-sample decision, as a pure function of numbers. Unit tested directly by
// tests/libslic3r/test_contour_z.cpp.
double contour_z_sample_delta(const ContourZSampleInput &in)
{
    // Delta from the layer's nominal top (print_z) to the mesh surface the ray hit.
    double d = in.hit_distance - (in.print_z - in.slice_z);

    double       max_up     = in.min_z;
    double       min_down   = -(in.height - in.min_z);
    const double half_width = in.half_width;
    if (in.is_ironing) {
        max_up   = in.height;
        min_down = -(in.height + 0.1);
    }

    // Upstream #13510: normalizing a zero vector is UB, so the slope rule needs the hit flag.
    if (in.is_perimeter && in.hit) {
        const double slope_rad     = slope_from_normal(in.hit_normal);
        const double slope_degrees = slope_rad * 180.0 / M_PI;

        if (d > min_down && in.minimize_perimeter_height_deg > 0 && in.minimize_perimeter_height_deg < slope_degrees) {
            double adjustment = follow_slope_down(slope_rad, half_width);
            if (adjustment > 0)
                // Cannot happen: sin() of an angle in [0, pi/2] is non-negative, so
                // follow_slope_down() is non-positive. Belt and braces, and never a throw.
                adjustment = 0;
            // EdgeSlicer guard for OrcaSlicer#13552, see ZAA_SLOPE_RAMP_DEGREES above.
            const double ramp = std::clamp((slope_degrees - in.minimize_perimeter_height_deg) / ZAA_SLOPE_RAMP_DEGREES,
                                           0.0, 1.0);
            d += adjustment * ramp;
            if (d < min_down)
                d = min_down;
        }
    }

    if (!in.hit || d < -in.height || d > max_up + 0.03)
        // This point is too far from the mesh edge, probably because this is not a top surface.
        // Do not contour it: leave the path at its own base, which means a delta of zero in the
        // path's own frame.
        return 0.0;

    // Shift into the path's own frame (guard 1a, offset_layers). For a flat path z_offset_mm is 0
    // and this is a no-op; for an offset_layers odd wall the whole band moves down by exactly the
    // amount the wall was raised by, so the absolute band stays [lo + min_z, print_z + min_z].
    double       d_off        = d - in.z_offset_mm;
    const double max_up_off   = max_up - in.z_offset_mm;
    const double min_down_off = min_down - in.z_offset_mm;

    if (d_off < min_down_off)
        d_off = min_down_off;
    else if (d_off > max_up_off)
        d_off = max_up_off;

    if (in.is_perimeter && d_off > 0)
        // Do not increase the height of perimeters as this may create the appearance of a seam.
        // NOTE the reference is the path's OWN base, not print_z: an offset_layers odd wall is
        // already half a layer up and must not be dragged back down to print_z, it just must not
        // go any higher than where it already is.
        d_off = 0;

    return d_off;
}

static bool contour_extrusion_path(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionPath &path)
{
    if (path.role() != erTopSolidInfill && path.role() != erIroning && path.role() != erExternalPerimeter &&
        path.role() != erPerimeter)
        return false;

    // Guard (1b), offset_layers: the two bonding layers (layer 1 over-extrudes by 1.5x, layer n-2
    // under-extrudes by 0.5x) have a flow that is deliberately NOT the geometric one for their
    // height. Contouring them would mean composing offset_layers' bonding factor with ZAA's height
    // ratio, i.e. exactly the double count the port must avoid, so those paths are left alone.
    if (path.extrusion_multiplier != 1.f)
        return false;

    // Guard (2), sloped entities: scarf-joint seams already own the Z of their path. Upstream
    // throws here; we skip, because this fork emits sloped loops routinely.
    if (dynamic_cast<const ExtrusionPathSloped *>(&path) != nullptr)
        return false;

    Layer         *layer = region->layer();
    const coordf_t mesh_slice_z = layer->slice_z + mesh.ground_level();
    const coordf_t min_z = region->region().config().zaa_min_z;
    const coordf_t height = layer->height;

    const double minimize_perimeter_height_angle = region->region().config().zaa_minimize_perimeter_height;

    // Guard (1a), offset_layers: an odd wall's base Z is print_z + z_offset * height, not print_z.
    // Everything below is expressed as a delta from THAT base, so the emitted Z is
    //     base + d      with     base = print_z + z_offset * height
    // and the absolute band is the same [lo + min_z, print_z + min_z] upstream uses.
    const double z_offset_mm = double(path.z_offset) * double(height);

    const Points &points = path.polyline.points;
    const double  resolution_mm = 0.1;

    Pointf3s contoured_points;
    bool     was_contoured = false;

    if (points.size() < 2)
        // Safety check (upstream #13508). The loop below does not handle paths with fewer than two
        // points correctly.
        return false;

    for (Points::const_iterator it = points.begin(); it != points.end() - 1; ++it) {
        const Vec2d p1d(unscale_(it->x()), unscale_(it->y()));
        const Vec2d p2d(unscale_((it + 1)->x()), unscale_((it + 1)->y()));
        const Vec2d delta = p2d - p1d;

        const double length_mm = delta.norm();
        const int    num_segments = int(std::ceil(length_mm / resolution_mm));
        if (num_segments == 0)
            continue;

        for (int i = 0; i < num_segments + 1; i++) {
            const Vec2d p = p1d + delta * i / num_segments;

            const coordf_t x = p.x();
            const coordf_t y = p.y();

            // Upstream #13510: a single ray UP from the slicing plane. Casting both ways and
            // taking the nearer hit could latch onto the bottom surface and wrongly lower the
            // layer.
            const sla::IndexedMesh::hit_result hit_up = mesh.query_ray_hit({x, y, mesh_slice_z}, {0.0, 0.0, 1.0});

            ContourZSampleInput in;
            in.is_perimeter                   = is_perimeter(path.role());
            in.is_ironing                     = path.role() == erIroning;
            in.hit                            = hit_up.is_hit();
            in.hit_distance                   = hit_up.distance();
            in.hit_normal                     = in.hit ? hit_up.normal() : Vec3d(0.0, 0.0, 1.0);
            in.print_z                        = layer->print_z;
            in.slice_z                        = layer->slice_z;
            in.height                         = height;
            in.min_z                          = min_z;
            in.minimize_perimeter_height_deg  = minimize_perimeter_height_angle;
            in.half_width                     = path.width / 2.0;
            in.z_offset_mm                    = z_offset_mm;

            const double d_off = contour_z_sample_delta(in);

            if (std::abs(d_off) > EPSILON)
                was_contoured = true;

            const Vec3d new_point = {p.x(), p.y(), d_off};

            if (contoured_points.size() >= 2 && i != 0) {
                // Normally, if the new point is collinear with the last two points, we do not add
                // it to the list of contoured points; we move the last point instead, to avoid a
                // large number of very short segments. But if the new point corresponds to a point
                // of the original path (i == 0) we add it anyway, so that a three-point polyline
                // cannot collapse into a degenerate two-point one (upstream #13508).
                const double dist = line_alg::distance_to_infinite_squared(
                    Linef3{contoured_points[contoured_points.size() - 2], contoured_points[contoured_points.size() - 1]},
                    new_point);
                if (dist < EPSILON * EPSILON) {
                    contoured_points[contoured_points.size() - 1] = new_point;
                    continue;
                }
            }

            contoured_points.push_back(new_point);
        }
    }

    if (!was_contoured)
        return false;

    if (contoured_points.size() < 2)
        return false;

    auto samples = std::make_shared<ContourZSamples>();
    samples->points.reserve(contoured_points.size());
    samples->z.reserve(contoured_points.size());

    Polyline polyline;
    polyline.points.reserve(contoured_points.size());
    for (const Vec3d &point : contoured_points) {
        const Point pt = Point::new_scale(point.x(), point.y());
        polyline.points.push_back(pt);
        samples->points.push_back(pt);
        samples->z.push_back(float(point.z()));
    }
    samples->build_index();

    path.polyline = std::move(polyline);
    // The resampled polyline invalidates any arc fitting the path may already carry, and contoured
    // paths must not be arc fitted at all - see ExtrusionPath::simplify_by_fitting_arc() and the
    // emitter in GCode.cpp.
    path.polyline.fitting_result.clear();
    path.z_contour = std::move(samples);
    return true;
}

static void contour_extrusion_multipath(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionMultiPath &multipath)
{
    for (ExtrusionPath &path : multipath.paths)
        contour_extrusion_path(region, mesh, path);
}

static void contour_extrusion_loop(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionLoop &loop)
{
    for (ExtrusionPath &path : loop.paths)
        contour_extrusion_path(region, mesh, path);
}

static void contour_extrusion_entitiy_collection(LayerRegion            *region,
                                                 const sla::IndexedMesh &mesh,
                                                 ExtrusionEntityCollection &collection)
{
    for (ExtrusionEntity *entity : collection.entities)
        contour_extrusion_entity(region, mesh, entity);
}

static void contour_extrusion_entity(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntity *extr)
{
    // Guard (2). Upstream throws RuntimeError on both sloped types and on anything it does not
    // recognise. This fork routinely produces sloped loops (scarf joints) and carries extrusion
    // entity subclasses of its own, so an unrecognised entity is simply left uncontoured.
    if (dynamic_cast<const ExtrusionPathSloped *>(extr) != nullptr)
        return;
    if (dynamic_cast<const ExtrusionLoopSloped *>(extr) != nullptr)
        return;

    if (ExtrusionMultiPath *multipath = dynamic_cast<ExtrusionMultiPath *>(extr); multipath != nullptr) {
        contour_extrusion_multipath(region, mesh, *multipath);
        return;
    }
    if (ExtrusionPath *path = dynamic_cast<ExtrusionPath *>(extr); path != nullptr) {
        contour_extrusion_path(region, mesh, *path);
        return;
    }
    if (ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop *>(extr); loop != nullptr) {
        contour_extrusion_loop(region, mesh, *loop);
        return;
    }
    if (ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection *>(extr); collection != nullptr) {
        contour_extrusion_entitiy_collection(region, mesh, *collection);
        return;
    }
}

static void handle_extrusion_collection(LayerRegion                        *region,
                                        const sla::IndexedMesh             &mesh,
                                        ExtrusionEntityCollection          &collection,
                                        std::initializer_list<ExtrusionRole> roles)
{
    for (ExtrusionEntity *extr : collection.entities) {
        if (!contains(roles, extr->role()))
            continue;
        contour_extrusion_entity(region, mesh, extr);
    }
}

void Layer::make_contour_z(const sla::IndexedMesh &mesh)
{
    for (LayerRegion *region : this->regions()) {
        if (!region->region().config().zaa_enabled)
            continue;

        handle_extrusion_collection(region, mesh, region->fills,
                                    {erTopSolidInfill, erIroning, erPerimeter, erExternalPerimeter, erMixed});
        handle_extrusion_collection(region, mesh, region->perimeters, {erPerimeter, erExternalPerimeter, erMixed});
    }
}

} // namespace Slic3r
