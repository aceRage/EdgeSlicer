#include "SplitToVolume.hpp"

#include "AABBTreeIndirect.hpp"
#include "AABBTreeLines.hpp"
#include "ClipperUtils.hpp"
#include "Config.hpp"
#include "CutUtils.hpp"
#include "ExtruderAreas.hpp"
#include "Geometry.hpp"
#include "Model.hpp"
#include "PrintConfig.hpp"
#include "PushFit.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace Slic3r {

// File-local helpers live in a named namespace: libslic3r is built as a unity build and short helper
// names collide with neighbours there.
namespace split_to_volume_detail {

static const char AXIS_LETTER[3] = { 'X', 'Y', 'Z' };

// Below this, two positions are the same (mm).
static constexpr double POS_EPS = 1e-6;
// A cell whose material is thinner than this in any direction is a touch, not a part (mm).
static constexpr double MIN_PART_EXTENT = 1e-3;
// Smallest face worth calling a cut face (mm2).
static constexpr double MIN_FACE_AREA = 1e-2;
// D6: a section whose material is less than this share of the area its outlines enclose, with walls
// thinner than HOLLOW_MAX_WALL, makes the model "look hollow".
static constexpr double HOLLOW_MAX_RATIO = 0.35;
static constexpr double HOLLOW_MAX_WALL  = 5.0;

static double polygon_area_mm2(const Pointfs &pts)
{
    double a = 0.;
    for (size_t i = 0, n = pts.size(); i < n; ++i) {
        const Vec2d &p = pts[i];
        const Vec2d &q = pts[(i + 1) % n];
        a += p.x() * q.y() - q.x() * p.y();
    }
    return 0.5 * std::abs(a);
}

static double point_segment_distance(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d  ab = b - a;
    const double l2 = ab.squaredNorm();
    const double t  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
    return (a + t * ab - p).norm();
}

static double rect_area(const BoundingBoxf &r)
{
    return r.defined ? std::max(0., r.max.x() - r.min.x()) * std::max(0., r.max.y() - r.min.y()) : 0.;
}

// The 2D coordinates a plane of `axis` is sliced in: the mesh is turned so the axis becomes Z, by a
// cyclic permutation of the axes (keeps the orientation, so outlines stay counter-clockwise).
//   X plane: (y, z) -> slice (u, v), x -> height
//   Y plane: (z, x) -> slice (u, v), y -> height
//   Z plane: (x, y)
static inline int u_axis(int axis) { return (axis + 1) % 3; }
static inline int v_axis(int axis) { return (axis + 2) % 3; }

static Transform3d axis_to_z(int axis)
{
    Transform3d t = Transform3d::Identity();
    Matrix3d    m = Matrix3d::Zero();
    // Row r of the matrix picks the source coordinate that becomes coordinate r.
    m(0, u_axis(axis)) = 1.;
    m(1, v_axis(axis)) = 1.;
    m(2, axis)         = 1.;
    t.linear()         = m;
    return t;
}

// Clip a convex polygon to x[axis] >= value (keep_above) or x[axis] <= value. Points created on the
// clipping plane get exactly `value`, so a part's box reaches its plane exactly.
static void clip_polygon(std::vector<Vec3d> &poly, int axis, double value, bool keep_above)
{
    if (poly.empty())
        return;
    std::vector<Vec3d> out;
    out.reserve(poly.size() + 2);
    auto inside = [&](const Vec3d &p) { return keep_above ? p[axis] >= value : p[axis] <= value; };
    for (size_t i = 0, n = poly.size(); i < n; ++i) {
        const Vec3d &a  = poly[i];
        const Vec3d &b  = poly[(i + 1) % n];
        const bool   ia = inside(a);
        const bool   ib = inside(b);
        if (ia)
            out.push_back(a);
        if (ia != ib) {
            const double t = (value - a[axis]) / (b[axis] - a[axis]);
            Vec3d        p = a + t * (b - a);
            p[axis]        = value;
            out.push_back(p);
        }
    }
    poly = std::move(out);
}

// Number of planes strictly below `x`.
static int count_below(const std::vector<double> &planes, double x)
{
    return int(std::lower_bound(planes.begin(), planes.end(), x) - planes.begin());
}
// Number of planes at or below `x`.
static int count_at_or_below(const std::vector<double> &planes, double x)
{
    return int(std::upper_bound(planes.begin(), planes.end(), x) - planes.begin());
}

static bool section_contains(const ExPolygons &section, const Vec2d &p)
{
    const Point pt = Point::new_scale(p.x(), p.y());
    for (const ExPolygon &ex : section)
        if (ex.contains(pt))
            return true;
    return false;
}

static Polygon scaled_rect(double u0, double v0, double u1, double v1)
{
    Polygon r;
    r.points = { Point::new_scale(u0, v0), Point::new_scale(u1, v0), Point::new_scale(u1, v1), Point::new_scale(u0, v1) };
    return r;
}

static double expolygons_area_mm2(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &ex : expolys)
        a += ex.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

static Transform3d plane_rotation(int axis)
{
    Transform3d r = Transform3d::Identity();
    if (axis == 0)
        r.linear() = Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitY()).toRotationMatrix();   // +Z -> +X
    else if (axis == 1)
        r.linear() = Eigen::AngleAxisd(-0.5 * PI, Vec3d::UnitX()).toRotationMatrix();  // +Z -> +Y
    return r;
}

// ---- dowels (P2) ----

// The smallest dowel the scale-down goes to (mm).
static constexpr double DOWEL_MIN_DIAMETER = 2.;
// A dowel shorter than this share of its diameter on either side holds nothing: the site is dropped.
static constexpr double DOWEL_MIN_HALF_PER_DIAMETER = 0.75;
// Ray hits closer than this to the plane are the plane's own surface, not the far side (mm).
static constexpr double RAY_EPS = 1e-4;

static CutConnectorShape dowel_shape(SplitDowelShape s)
{
    switch (s) {
    case SplitDowelShape::Square: return CutConnectorShape::Square;
    case SplitDowelShape::Hexagon: return CutConnectorShape::Hexagon;
    case SplitDowelShape::Triangle: return CutConnectorShape::Triangle;
    default: return CutConnectorShape::Circle;
    }
}

static bool dowel_is_round(const SplitDowelOptions &opt)
{
    return opt.type == SplitConnectorType::Pin || opt.shape == SplitDowelShape::Round;
}

// How much deeper than the dowel's own half-length its hole goes, beyond the face (mm).
static double hole_extra_depth(const SplitDowelOptions &opt)
{
    return opt.type == SplitConnectorType::Pin ? double(PushFitParams().depth_clearance) : 0.5 * opt.depth_tolerance;
}

// The connector the Cut tool would make for this dowel: at `pos` (the cut's frame), axis along
// the plane normal (`rotation`), `half` into each side.
static CutConnector site_connector(const SplitDowelOptions &opt, double diameter, double half, const Vec3d &pos,
                                   const Transform3d &rotation)
{
    if (opt.type == SplitConnectorType::Pin) {
        CutConnector c(pos, rotation, float(0.5 * diameter), float(2. * half), 0.f, 0.f, 0.f,
                       CutConnectorAttributes(CutConnectorType::PushFit, CutConnectorStyle::Prism, CutConnectorShape::Circle));
        PushFitParams p = PushFitParams();
        p.diameter        = float(diameter);
        p.length_upper    = float(half);
        p.length_lower    = float(half);
        p.clearance_upper = float(opt.tolerance);
        p.clearance_lower = float(opt.tolerance);
        // The default pin is the 6 mm one; keep its features in proportion on a scaled-down pin.
        p.flat_depth  = float(0.15 * diameter);
        p.slot_width  = float(std::min(1., diameter / 6.));
        p.slot_length = float(std::min(double(p.slot_length), 0.6 * half));
        p.preset      = int(PushFitPreset::Custom);
        c.pushfit     = p;
        return c;
    }
    // A Dowel's height is its whole length; it straddles the plane symmetrically.
    return CutConnector(pos, rotation, float(0.5 * diameter), float(2. * half), float(opt.tolerance), float(opt.depth_tolerance), 0.f,
                        CutConnectorAttributes(CutConnectorType::Dowel, CutConnectorStyle::Prism, dowel_shape(opt.shape)));
}

static double site_footprint_radius(const SplitDowelOptions &opt, double diameter, double half)
{
    return cut_connector_footprint_radius(site_connector(opt, diameter, half, Vec3d::Zero(), Transform3d::Identity()));
}

} // namespace split_to_volume_detail

std::vector<Vec2d> split_pick_sites(const ExPolygon &island, const BoundingBoxf &allowed, double inset, int max_count,
                                    double min_spacing, const std::function<bool(const Vec2d &)> &accept, int max_rejects)
{
    using namespace split_to_volume_detail;
    std::vector<Vec2d> out;
    if (max_count <= 0 || island.contour.points.size() < 3 || !allowed.defined || allowed.max.x() <= allowed.min.x() ||
        allowed.max.y() <= allowed.min.y())
        return out;

    // Where a centre may go.
    ExPolygons area = intersection_ex(offset_ex(island, -float(scale_(std::max(0., inset)))),
                                      Polygons{ scaled_rect(allowed.min.x(), allowed.min.y(), allowed.max.x(), allowed.max.y()) });
    if (area.empty())
        return out;

    // Candidates on a square grid: about a millimetre apart, coarser on a big face.
    const double       area_mm2 = expolygons_area_mm2(area);
    const double       step     = std::max({ 1., 0.25 * inset, std::sqrt(area_mm2 / 4000.) });
    const BoundingBox  bb       = get_extents(area);
    std::vector<Vec2d> cand;
    const Vec2d        lo = unscale(bb.min);
    const Vec2d        hi = unscale(bb.max);
    for (double v = lo.y() + 0.5 * step; v < hi.y(); v += step)
        for (double u = lo.x() + 0.5 * step; u < hi.x(); u += step)
            if (section_contains(area, Vec2d(u, v)))
                cand.emplace_back(u, v);
    // A region smaller than the grid: its middle (or a point of its edge, which is still inset).
    if (cand.empty())
        for (const ExPolygon &ex : area) {
            const Point c = ex.contour.centroid();
            cand.push_back(ex.contains(c) ? unscale(c) : unscale(ex.contour.points.front()));
        }

    // Clearance of every candidate from the face's outline.
    const AABBTreeLines::LinesDistancer<Line> outline(island.lines());
    std::vector<double>                        clr(cand.size());
    for (size_t i = 0; i < cand.size(); ++i)
        clr[i] = outline.distance_from_lines<false>(Point::new_scale(cand[i].x(), cand[i].y())) * SCALING_FACTOR;

    std::vector<bool> alive(cand.size(), true);
    int               rejects = 0;
    while (int(out.size()) < max_count) {
        int    best       = -1;
        double best_score = -1.;
        for (size_t i = 0; i < cand.size(); ++i) {
            if (!alive[i])
                continue;
            double d = std::numeric_limits<double>::max();
            for (const Vec2d &p : out)
                d = std::min(d, (cand[i] - p).norm());
            if (!out.empty() && d < min_spacing - 1e-9)
                continue;
            const double score = out.empty() ? clr[i] : std::min(clr[i], 0.5 * d);
            // Ties go to the earlier candidate, so the same face always gives the same sites.
            if (score > best_score + 1e-9) {
                best_score = score;
                best       = int(i);
            }
        }
        if (best < 0)
            break;
        alive[size_t(best)] = false;
        if (accept && !accept(cand[size_t(best)])) {
            if (++rejects > max_rejects)
                break;
            continue;
        }
        out.push_back(cand[size_t(best)]);
    }
    return out;
}

// ---- names ------------------------------------------------------------------------------------------

std::string SplitPlane::name() const
{
    using namespace split_to_volume_detail;
    return std::string(1, AXIS_LETTER[std::clamp(axis, 0, 2)]) + std::to_string(index + 1);
}

std::string split_cell_name(const SplitCell &cell, const std::array<int, 3> &counts)
{
    using namespace split_to_volume_detail;
    std::string out;
    for (int a = 0; a < 3; ++a) {
        if (counts[a] <= 1)
            continue;
        if (!out.empty())
            out += '-';
        out += AXIS_LETTER[a];
        out += std::to_string(cell[a] + 1);
    }
    return out;
}

size_t SplitPlan::part_count() const
{
    return size_t(std::count_if(parts.begin(), parts.end(), [](const SplitPart &p) { return !p.empty; }));
}

size_t SplitPlan::parts_not_fitting() const
{
    return size_t(std::count_if(parts.begin(), parts.end(), [](const SplitPart &p) { return !p.empty && !p.fits; }));
}

std::vector<double> SplitPlan::plane_positions(int axis) const
{
    std::vector<double> out;
    for (const SplitPlane &p : planes)
        if (p.axis == axis)
            out.push_back(p.pos);
    return out;
}

int SplitPlan::plane_index(int axis, int index) const
{
    for (size_t i = 0; i < planes.size(); ++i)
        if (planes[i].axis == axis && planes[i].index == index)
            return int(i);
    return -1;
}

// ---- the printer's volume ---------------------------------------------------------------------------

SplitPrinterVolume split_printer_volume_from_config(const ConfigBase &config)
{
    SplitPrinterVolume out;
    if (const auto *bed = dynamic_cast<const ConfigOptionPoints *>(config.option("printable_area")))
        out.printable_area = bed->values;
    if (const auto *ex = dynamic_cast<const ConfigOptionPoints *>(config.option("bed_exclude_area")))
        out.exclude_boxes = bed_exclude_area_boxes(ex->values);
    if (const auto *h = dynamic_cast<const ConfigOptionFloat *>(config.option("printable_height")))
        out.printable_height = h->value;
    if (const auto *groups = dynamic_cast<const ConfigOptionPointsGroups *>(config.option("extruder_printable_area"))) {
        std::vector<Pointfs> areas;
        for (const auto &group : groups->values)
            areas.emplace_back(group.begin(), group.end());
        out.extruder_areas = split_merged_extruder_areas(std::move(areas));
    }
    // Declared nullable, so a dynamic config holds the Nullable flavour; both derive from this.
    if (const auto *h = dynamic_cast<const ConfigOptionVector<double> *>(config.option("extruder_printable_height")))
        out.extruder_heights = h->values;
    return out;
}

SplitUsableVolume split_usable_volume(const SplitPrinterVolume &printer, const SplitOptions &options)
{
    using namespace split_to_volume_detail;
    SplitUsableVolume u;
    const Pointfs &pts = printer.printable_area;
    if (pts.size() < 3 || printer.printable_height <= 0.)
        return u;

    const BoundingBoxf bbox(pts);
    u.printer_size = Vec3d(bbox.size().x(), bbox.size().y(), printer.printable_height);

    // 1. The bed shape: a rectangle as it is, a round bed its inscribed square, anything else its
    //    bounding box (said so in the panel).
    BoundingBoxf rect      = bbox;
    const double bbox_area = rect_area(bbox);
    if (bbox_area <= 0.)
        return u;
    if (polygon_area_mm2(pts) < 0.995 * bbox_area) {
        u.bed_not_rectangular = true;
        const Vec2d c    = bbox.center();
        double      rmin = std::numeric_limits<double>::max(), rmax = 0.;
        for (const Vec2d &p : pts) {
            const double r = (p - c).norm();
            rmin           = std::min(rmin, r);
            rmax           = std::max(rmax, r);
        }
        if (pts.size() >= 8 && rmax > 0. && rmax - rmin <= 0.05 * rmax) {
            u.bed_round = true;
            double r    = std::numeric_limits<double>::max();
            for (size_t i = 0; i < pts.size(); ++i)
                r = std::min(r, point_segment_distance(c, pts[i], pts[(i + 1) % pts.size()]));
            const double half = r / std::sqrt(2.);
            rect              = BoundingBoxf(c - Vec2d(half, half), c + Vec2d(half, half));
        }
    }

    // 2. Exclusion areas: each box that reaches into the rectangle takes off the one side that loses
    //    the least area.
    for (const BoundingBoxf &box : printer.exclude_boxes) {
        if (!box.defined)
            continue;
        const bool overlaps = box.min.x() < rect.max.x() - POS_EPS && box.max.x() > rect.min.x() + POS_EPS &&
                              box.min.y() < rect.max.y() - POS_EPS && box.max.y() > rect.min.y() + POS_EPS;
        if (!overlaps)
            continue;
        BoundingBoxf best;
        double       best_area = -1.;
        auto         consider  = [&](BoundingBoxf cand) {
            const double a = rect_area(cand);
            if (cand.max.x() > cand.min.x() && cand.max.y() > cand.min.y() && a > best_area) {
                best_area = a;
                best      = cand;
            }
        };
        BoundingBoxf c = rect; c.min.x() = std::max(rect.min.x(), box.max.x()); consider(c);
        c = rect;              c.max.x() = std::min(rect.max.x(), box.min.x()); consider(c);
        c = rect;              c.min.y() = std::max(rect.min.y(), box.max.y()); consider(c);
        c = rect;              c.max.y() = std::min(rect.max.y(), box.min.y()); consider(c);
        if (best_area <= 0.)
            return u; // the exclusions cover the bed
        rect                   = best;
        u.reduced_by_exclusion = true;
    }

    // 3. Nozzle reach (D14): what every nozzle reaches.
    const ExtruderAreas areas = compute_extruder_areas(pts, printer.extruder_areas, printer.extruder_heights);
    u.has_reach_option        = areas.multi() && areas.has_exclusive_regions();
    if (options.nozzle_reach && u.has_reach_option && !areas.shared.empty()) {
        const BoundingBox  sb = get_extents(areas.shared);
        const BoundingBoxf shared(unscale(sb.min), unscale(sb.max));
        BoundingBoxf       r = rect;
        r.min.x()            = std::max(r.min.x(), shared.min.x());
        r.min.y()            = std::max(r.min.y(), shared.min.y());
        r.max.x()            = std::min(r.max.x(), shared.max.x());
        r.max.y()            = std::min(r.max.y(), shared.max.y());
        if (r.max.x() <= r.min.x() || r.max.y() <= r.min.y())
            return u;
        u.reduced_by_reach = rect_area(r) < rect_area(rect) - POS_EPS;
        rect               = r;
    }

    // 4. Height: the bed's, or the lowest nozzle's own limit.
    double height = printer.printable_height;
    for (double h : printer.extruder_heights)
        if (std::isfinite(h) && h > 0. && h < 1e6)
            height = std::min(height, h);

    u.bed_rect = rect;
    u.size     = Vec3d(rect.size().x() - 2. * options.margin_xy, rect.size().y() - 2. * options.margin_xy, height - options.margin_z);
    u.valid    = u.size.x() > 0. && u.size.y() > 0. && u.size.z() > 0.;
    return u;
}

// ---- the plan ---------------------------------------------------------------------------------------

namespace split_to_volume_detail {

// Design 3.6. For every cut face: its islands (the plane's section clipped to the face's cell),
// largest first; on each, up to "max per face" centres inset from the outline by the wall margin
// and the footprint, kept out of a band along every other plane that bounds the face, and checked
// for depth along both normals; then the face's picks are taken island by island in turn up to the
// cap. A face with no room tries smaller dowels; a face that ends with one round dowel tries two
// smaller ones before it warns.
static void place_dowels(SplitPlan &plan, const indexed_triangle_set &mesh, const std::vector<ExPolygons> &sections,
                         const std::array<std::vector<double>, 3> &positions, const std::function<void()> &cancel_point)
{
    const SplitDowelOptions &opt = plan.dowel_options;
    if (opt.diameter <= 0. || opt.length <= 0. || opt.max_per_face <= 0)
        return;

    const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.vertices, mesh.indices);
    // Material along `dir` from `origin` (mm): the first surface the ray meets. A ray that meets
    // nothing started outside the material.
    std::vector<igl::Hit> hits;
    auto depth_along = [&](const Vec3d &origin, const Vec3d &dir) {
        AABBTreeIndirect::intersect_ray_all_hits(mesh.vertices, mesh.indices, tree, origin, dir, hits);
        for (const igl::Hit &h : hits)
            if (double(h.t) > RAY_EPS)
                return double(h.t);
        return 0.;
    };

    const Vec3d lo        = plan.model_bbox.min;
    const Vec3d hi        = plan.model_bbox.max;
    auto        cell_lo   = [&](int a, int c) { return c == 0 ? lo[a] : positions[a][size_t(c) - 1]; };
    auto        cell_hi   = [&](int a, int c) { return c == plan.counts[a] - 1 ? hi[a] : positions[a][size_t(c)]; };
    const double req_half = 0.5 * opt.length;
    const double extra    = hole_extra_depth(opt);
    const double inf      = std::numeric_limits<double>::max();
    // An island smaller than this could not hold even the smallest dowel: no warning for it.
    const double worth_area = PI * (opt.wall_margin + 0.5 * DOWEL_MIN_DIAMETER) * (opt.wall_margin + 0.5 * DOWEL_MIN_DIAMETER);

    std::vector<double> scales{ 1. };
    if (opt.scale_down)
        for (double s : { 0.75, 0.5 })
            if (opt.diameter * s >= DOWEL_MIN_DIAMETER - 1e-9)
                scales.push_back(s);

    for (SplitFace &face : plan.faces) {
        cancel_point();
        const SplitPlane &pl = plan.planes[size_t(face.plane)];
        const int a = pl.axis, ua = u_axis(a), va = v_axis(a);
        const SplitCell &cell = face.cell_below;
        const double u0 = cell_lo(ua, cell[ua]), u1 = cell_hi(ua, cell[ua]);
        const double w0 = cell_lo(va, cell[va]), w1 = cell_hi(va, cell[va]);

        ExPolygons islands = intersection_ex(sections[size_t(face.plane)], Polygons{ scaled_rect(u0, w0, u1, w1) });
        islands.erase(std::remove_if(islands.begin(), islands.end(),
                                     [](const ExPolygon &ex) { return ex.area() * SCALING_FACTOR * SCALING_FACTOR < MIN_FACE_AREA; }),
                      islands.end());
        std::stable_sort(islands.begin(), islands.end(), [](const ExPolygon &l, const ExPolygon &r) { return l.area() > r.area(); });
        face.islands = int(islands.size());
        if (islands.empty())
            continue;

        // Room along the normal before the next parallel plane (a model end is left to the depth check).
        const double room_below = pl.index > 0 ? pl.pos - positions[a][size_t(pl.index) - 1] : inf;
        const double room_above = size_t(pl.index) + 1 < positions[a].size() ? positions[a][size_t(pl.index) + 1] - pl.pos : inf;

        struct Pick { Vec2d uv; double half; int island; };
        bool had_room = false;
        // The picks for one dowel size, island by island (each list in pick order).
        auto picks_at = [&](double scale) {
            std::vector<std::vector<Pick>> per_island(islands.size());
            const double d  = opt.diameter * scale;
            const double fp = site_footprint_radius(opt, d, req_half);
            // The keep-out bands along the planes bounding this face (not along the model's own ends).
            BoundingBoxf allowed(Vec2d(u0, w0), Vec2d(u1, w1));
            const double band = fp + opt.corner_keepout;
            if (cell[ua] > 0) allowed.min.x() += band;
            if (cell[ua] < plan.counts[ua] - 1) allowed.max.x() -= band;
            if (cell[va] > 0) allowed.min.y() += band;
            if (cell[va] < plan.counts[va] - 1) allowed.max.y() -= band;
            if (allowed.max.x() <= allowed.min.x() || allowed.max.y() <= allowed.min.y())
                return per_island;
            for (size_t ii = 0; ii < islands.size(); ++ii) {
                std::vector<Pick> &picks = per_island[ii];
                double             half  = 0.;
                auto accept = [&](const Vec2d &uv) {
                    had_room = true;
                    // The depth check: the centre and 8 points round the footprint, both ways.
                    double up = inf, down = inf;
                    for (int k = 0; k <= 8; ++k) {
                        Vec3d p = Vec3d::Zero();
                        p[a]    = pl.pos;
                        p[ua]   = uv.x();
                        p[va]   = uv.y();
                        if (k > 0) {
                            const double t = 2. * PI * double(k - 1) / 8.;
                            p[ua] += fp * std::cos(t);
                            p[va] += fp * std::sin(t);
                        }
                        Vec3d n = Vec3d::Zero();
                        n[a]    = 1.;
                        up      = std::min(up, depth_along(p, n));
                        down    = std::min(down, depth_along(p, -n));
                    }
                    half = std::min({ req_half, up - opt.min_material - extra, down - opt.min_material - extra,
                                      room_above - opt.corner_keepout - extra, room_below - opt.corner_keepout - extra });
                    if (half < DOWEL_MIN_HALF_PER_DIAMETER * d)
                        return false;
                    picks.push_back({ uv, half, int(ii) });
                    return true;
                };
                split_pick_sites(islands[ii], allowed, opt.wall_margin + fp, opt.max_per_face, opt.min_spacing, accept);
            }
            return per_island;
        };
        // Island by island in turn: every island gets its first dowel before any gets a second.
        auto merged = [&](const std::vector<std::vector<Pick>> &per_island) {
            std::vector<Pick> out;
            for (size_t round = 0; int(out.size()) < opt.max_per_face; ++round) {
                bool any = false;
                for (const std::vector<Pick> &picks : per_island)
                    if (round < picks.size() && int(out.size()) < opt.max_per_face) {
                        out.push_back(picks[round]);
                        any = true;
                    }
                if (!any)
                    break;
            }
            return out;
        };

        std::vector<Pick> chosen;
        size_t            level = 0;
        for (; level < scales.size(); ++level) {
            chosen = merged(picks_at(scales[level]));
            if (!chosen.empty())
                break;
        }
        // One round dowel lets the parts turn: two smaller ones lock them, if they fit.
        if (chosen.size() == 1 && dowel_is_round(opt) && opt.max_per_face >= 2)
            for (size_t l = level + 1; l < scales.size(); ++l) {
                std::vector<Pick> two = merged(picks_at(scales[l]));
                if (two.size() >= 2) {
                    chosen = std::move(two);
                    level  = l;
                    break;
                }
            }

        if (chosen.empty()) {
            plan.warnings.push_back({ SplitWarning::Kind::NoDowelOnFace, face.plane, -1,
                                      had_room ? SPLIT_NO_DOWEL_TOO_THIN : SPLIT_NO_DOWEL_TOO_SMALL, face.id });
            continue;
        }

        const double d        = opt.diameter * scales[level];
        const double fp       = site_footprint_radius(opt, d, req_half);
        double       shortest = inf;
        std::vector<bool> island_has(islands.size(), false);
        for (const Pick &pk : chosen) {
            SplitSite s;
            s.face             = face.id;
            s.plane            = face.plane;
            s.island           = pk.island;
            s.pos[a]           = pl.pos;
            s.pos[ua]          = pk.uv.x();
            s.pos[va]          = pk.uv.y();
            s.diameter         = d;
            s.half_length      = pk.half;
            s.footprint_radius = fp;
            s.shortened        = pk.half < req_half - 1e-6;
            s.scaled_down      = level > 0;
            s.name             = face.mate_code + "-" + std::to_string(face.sites.size() + 1);
            shortest           = std::min(shortest, pk.half);
            island_has[size_t(pk.island)] = true;
            face.sites.push_back(int(plan.sites.size()));
            plan.sites.push_back(std::move(s));
        }
        int without = 0;
        for (size_t ii = 0; ii < islands.size(); ++ii)
            if (!island_has[ii] && islands[ii].area() * SCALING_FACTOR * SCALING_FACTOR >= worth_area)
                ++without;
        if (without > 0)
            plan.warnings.push_back({ SplitWarning::Kind::IslandsWithoutDowel, face.plane, -1, double(without), face.id });
        if (chosen.size() == 1 && dowel_is_round(opt))
            plan.warnings.push_back({ SplitWarning::Kind::DowelCanTurn, face.plane, -1, 0., face.id });
        if (level > 0)
            plan.warnings.push_back({ SplitWarning::Kind::DowelScaledDown, face.plane, -1, d, face.id });
        if (shortest < req_half - 1e-6)
            plan.warnings.push_back({ SplitWarning::Kind::DowelShortened, face.plane, -1, 2. * shortest, face.id });
    }
}

} // namespace split_to_volume_detail

SplitPlan split_plan_grid(const indexed_triangle_set &mesh, const SplitUsableVolume &usable, const SplitOptions &options,
                          std::function<void()> throw_on_cancel)
{
    using namespace split_to_volume_detail;
    auto cancel_point = [&throw_on_cancel]() {
        if (throw_on_cancel)
            throw_on_cancel();
    };

    SplitPlan plan;
    plan.usable = usable;
    if (mesh.vertices.empty() || mesh.indices.empty())
        return plan;
    for (const Vec3f &v : mesh.vertices)
        plan.model_bbox.merge(v.cast<double>());

    if (!usable.valid) {
        plan.warnings.push_back({ SplitWarning::Kind::NoUsableVolume });
        return plan;
    }
    if (usable.bed_not_rectangular)
        plan.warnings.push_back({ SplitWarning::Kind::BedNotRectangular });
    if (usable.reduced_by_exclusion)
        plan.warnings.push_back({ SplitWarning::Kind::BedExclusion });
    if (usable.reduced_by_reach)
        plan.warnings.push_back({ SplitWarning::Kind::NozzleReach });

    const Vec3d lo = plan.model_bbox.min;
    const Vec3d hi = plan.model_bbox.max;

    // 1. Planes per axis (design 3.2).
    std::array<std::vector<double>, 3> positions;
    std::array<std::vector<double>, 3> nominals;
    std::array<std::vector<bool>, 3>   pinned;
    long long                          cells = 1;
    for (int a = 0; a < 3; ++a) {
        const double S   = hi[a] - lo[a];
        const double U   = usable.size[a];
        plan.needs_cut[a] = S > U + POS_EPS;
        int n            = 1;
        if (plan.needs_cut[a] && options.allow_axis[a])
            n = int(std::ceil((S - POS_EPS) / U));
        n               = std::max(1, n);
        plan.counts[a]  = n;
        cells *= n;
        if (cells > SPLIT_MAX_CELLS) {
            plan.warnings.push_back({ SplitWarning::Kind::TooManyParts, -1, -1, double(cells) });
            return plan;
        }
        std::vector<double> &p = positions[a];
        if (options.layout == SplitLayout::FullSizeFirst) {
            for (int k = 1; k < n; ++k)
                p.push_back(lo[a] + double(k) * U);
            // A remainder thinner than the minimum pulls the last plane in (the part before it stays
            // within U, since the remainder it gives up is smaller than the minimum it takes).
            if (n >= 2) {
                const double remainder = hi[a] - p.back();
                const double min_t     = std::min(options.min_part_thickness, 0.5 * U);
                if (remainder < min_t)
                    p.back() = std::max(hi[a] - min_t, n >= 3 ? p[size_t(n) - 3] + POS_EPS : lo[a] + POS_EPS);
            }
        } else {
            for (int k = 1; k < n; ++k)
                p.push_back(lo[a] + double(k) * S / double(n));
        }
        nominals[a] = p;
        pinned[a].assign(p.size(), false);
    }

    // 2. Planes the user moved stay where they were put (inside the model), then each axis is put back in order.
    for (const SplitPinnedPlane &pp : options.pinned) {
        if (pp.axis < 0 || pp.axis > 2 || pp.index < 0 || size_t(pp.index) >= positions[pp.axis].size() || !std::isfinite(pp.pos))
            continue;
        const double margin                = std::min(0.1, 0.25 * (hi[pp.axis] - lo[pp.axis]));
        positions[pp.axis][size_t(pp.index)] = std::clamp(pp.pos, lo[pp.axis] + margin, hi[pp.axis] - margin);
        pinned[pp.axis][size_t(pp.index)]    = true;
    }
    for (int a = 0; a < 3; ++a) {
        std::vector<size_t> order(positions[a].size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t l, size_t r) { return positions[a][l] < positions[a][r]; });
        for (size_t i = 0; i < order.size(); ++i) {
            SplitPlane pl;
            pl.axis    = a;
            pl.index   = int(i);
            pl.pos     = positions[a][order[i]];
            pl.nominal = nominals[a][order[i]];
            pl.pinned  = pinned[a][order[i]];
            plan.planes.push_back(pl);
        }
        positions[a] = plan.plane_positions(a);
    }

    // 3. Sections of every plane: material area, the hollow check (D6), and what the faces see.
    //    One slice_mesh_ex() call per axis, the mesh turned so the axis is Z.
    std::vector<ExPolygons> sections(plan.planes.size());
    int    hollow_plane = -1;
    double hollow_ratio = 1., hollow_wall = 0.;
    for (int a = 0; a < 3; ++a) {
        if (positions[a].empty())
            continue;
        MeshSlicingParamsEx params;
        params.trafo = axis_to_z(a);
        std::vector<float> zs;
        for (double p : positions[a])
            zs.push_back(float(p));
        std::vector<ExPolygons> slices = slice_mesh_ex(mesh, zs, params, [&cancel_point]() { cancel_point(); });
        for (size_t i = 0; i < slices.size() && i < positions[a].size(); ++i) {
            const int idx = plan.plane_index(a, int(i));
            if (idx < 0)
                continue;
            sections[size_t(idx)] = std::move(slices[i]);
            const ExPolygons &sec = sections[size_t(idx)];
            const double material = expolygons_area_mm2(sec);
            double       enclosed = 0., perimeter = 0.;
            for (const ExPolygon &ex : sec) {
                enclosed += std::abs(ex.contour.area()) * SCALING_FACTOR * SCALING_FACTOR;
                perimeter += ex.contour.length() * SCALING_FACTOR;
                for (const Polygon &h : ex.holes)
                    perimeter += h.length() * SCALING_FACTOR;
            }
            plan.planes[size_t(idx)].section_area = material;
            if (enclosed > 0. && perimeter > 0.) {
                const double ratio = material / enclosed;
                const double wall  = 2. * material / perimeter;
                if (ratio < HOLLOW_MAX_RATIO && wall < HOLLOW_MAX_WALL && ratio < hollow_ratio) {
                    hollow_ratio = ratio;
                    hollow_wall  = wall;
                    hollow_plane = idx;
                }
            }
        }
    }

    // 4. Every cell's material box (design 3.4). The surface clipped to the cell gives it, plus whole
    //    cell faces that lie inside the model without the surface crossing them (found from the sections).
    const int nx = plan.counts[0], ny = plan.counts[1], nz = plan.counts[2];
    plan.parts.resize(size_t(nx) * size_t(ny) * size_t(nz));
    std::vector<BoundingBoxf3> boxes(plan.parts.size());

    auto cell_lo = [&](int a, int c) { return c == 0 ? lo[a] : positions[a][size_t(c) - 1]; };
    auto cell_hi = [&](int a, int c) { return c == plan.counts[a] - 1 ? hi[a] : positions[a][size_t(c)]; };

    std::vector<Vec3d> poly;
    for (size_t f = 0; f < mesh.indices.size(); ++f) {
        if ((f & 0xFFFF) == 0)
            cancel_point();
        const stl_triangle_vertex_indices &t = mesh.indices[f];
        const Vec3d v[3] = { mesh.vertices[t[0]].cast<double>(), mesh.vertices[t[1]].cast<double>(), mesh.vertices[t[2]].cast<double>() };
        std::array<int, 3> c0, c1;
        bool               single = true;
        for (int a = 0; a < 3; ++a) {
            const double mn = std::min({ v[0][a], v[1][a], v[2][a] });
            const double mx = std::max({ v[0][a], v[1][a], v[2][a] });
            c0[a]           = std::min(count_below(positions[a], mn), plan.counts[a] - 1);
            c1[a]           = std::min(count_at_or_below(positions[a], mx), plan.counts[a] - 1);
            single          = single && c0[a] == c1[a];
        }
        if (single) {
            BoundingBoxf3 &bb = boxes[size_t(plan.cell_index({ { c0[0], c0[1], c0[2] } }))];
            for (const Vec3d &p : v)
                bb.merge(p);
            continue;
        }
        for (int k = c0[2]; k <= c1[2]; ++k)
            for (int j = c0[1]; j <= c1[1]; ++j)
                for (int i = c0[0]; i <= c1[0]; ++i) {
                    poly.assign(v, v + 3);
                    const int cc[3] = { i, j, k };
                    for (int a = 0; a < 3 && !poly.empty(); ++a) {
                        if (cc[a] > 0)
                            clip_polygon(poly, a, positions[a][size_t(cc[a]) - 1], true);
                        if (cc[a] < plan.counts[a] - 1)
                            clip_polygon(poly, a, positions[a][size_t(cc[a])], false);
                    }
                    if (poly.empty())
                        continue;
                    BoundingBoxf3 &bb = boxes[size_t(plan.cell_index({ { i, j, k } }))];
                    for (const Vec3d &p : poly)
                        bb.merge(p);
                }
    }

    // Whole faces inside the model: a face the surface does not cross is all inside or all outside, and
    // the surface crossing it would have taken the box to that plane - so a box that falls short of an
    // inner plane only needs the face's centre tested against that plane's section.
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const SplitCell cell{ { i, j, k } };
                BoundingBoxf3  &bb = boxes[size_t(plan.cell_index(cell))];
                for (int a = 0; a < 3; ++a)
                    for (int side = 0; side < 2; ++side) {
                        const int    c     = cell[a];
                        const int    pidx  = side == 0 ? c - 1 : c;  // plane index on this side
                        if (pidx < 0 || pidx >= plan.counts[a] - 1)
                            continue;
                        const double value = positions[a][size_t(pidx)];
                        if (bb.defined && (side == 0 ? bb.min[a] <= value + POS_EPS : bb.max[a] >= value - POS_EPS))
                            continue;
                        const int  ua = u_axis(a), va = v_axis(a);
                        const double u0 = cell_lo(ua, cell[ua]), u1 = cell_hi(ua, cell[ua]);
                        const double w0 = cell_lo(va, cell[va]), w1 = cell_hi(va, cell[va]);
                        const int    plane_idx = plan.plane_index(a, pidx);
                        if (plane_idx < 0 || !section_contains(sections[size_t(plane_idx)], Vec2d(0.5 * (u0 + u1), 0.5 * (w0 + w1))))
                            continue;
                        Vec3d p0, p1;
                        p0[a] = value; p1[a] = value;
                        p0[ua] = u0;   p1[ua] = u1;
                        p0[va] = w0;   p1[va] = w1;
                        bb.merge(p0);
                        bb.merge(p1);
                    }
            }

    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const SplitCell cell{ { i, j, k } };
                const size_t    ci   = size_t(plan.cell_index(cell));
                SplitPart      &part = plan.parts[ci];
                part.cell            = cell;
                part.name            = split_cell_name(cell, plan.counts);
                const BoundingBoxf3 &bb = boxes[ci];
                part.empty = !bb.defined || (bb.size().array() < MIN_PART_EXTENT).any();
                if (part.empty)
                    continue;
                part.bbox = bb;
                const Vec3d s = bb.size();
                part.fits     = s.x() <= usable.size.x() + POS_EPS && s.y() <= usable.size.y() + POS_EPS && s.z() <= usable.size.z() + POS_EPS;
                if (!part.fits)
                    plan.warnings.push_back({ SplitWarning::Kind::PartDoesNotFit, -1, int(ci), 0. });
            }

    // 5. Cut faces: each plane's section clipped to each pair of neighbouring cells.
    for (size_t pi = 0; pi < plan.planes.size(); ++pi) {
        const SplitPlane &pl = plan.planes[pi];
        const int a = pl.axis, ua = u_axis(a), va = v_axis(a);
        int       on_plane = 0;
        // Fixed order (X, Y, Z of the other axes) so ids and codes are stable.
        const int b0 = std::min(ua, va), b1 = std::max(ua, va);
        for (int c1 = 0; c1 < plan.counts[b1]; ++c1)
            for (int c0 = 0; c0 < plan.counts[b0]; ++c0) {
                SplitCell below{ { 0, 0, 0 } };
                below[a]  = pl.index;
                below[b0] = c0;
                below[b1] = c1;
                SplitCell above = below;
                above[a]        = pl.index + 1;
                const int pb    = plan.cell_index(below);
                const int pa    = plan.cell_index(above);
                if (plan.parts[size_t(pb)].empty || plan.parts[size_t(pa)].empty)
                    continue;
                const Polygon rect = scaled_rect(cell_lo(ua, below[ua]), cell_lo(va, below[va]), cell_hi(ua, below[ua]), cell_hi(va, below[va]));
                const double  area = expolygons_area_mm2(intersection_ex(sections[pi], Polygons{ rect }));
                if (area < MIN_FACE_AREA)
                    continue;
                SplitFace face;
                face.id         = int(plan.faces.size());
                face.plane      = int(pi);
                face.cell_below = below;
                face.cell_above = above;
                face.part_below = pb;
                face.part_above = pa;
                face.area       = area;
                face.key        = pl.name();
                std::string others;
                for (int b = 0; b < 3; ++b) {
                    if (b == a || plan.counts[b] <= 1)
                        continue;
                    if (!others.empty())
                        others += '-';
                    others += AXIS_LETTER[b];
                    others += std::to_string(below[b] + 1);
                }
                if (!others.empty())
                    face.key += ":" + others;
                face.mate_code  = pl.name() + "." + std::to_string(++on_plane);
                face.mate_label = plan.parts[size_t(pb)].name + " | " + plan.parts[size_t(pa)].name;
                plan.parts[size_t(pb)].faces.push_back(face.id);
                plan.parts[size_t(pa)].faces.push_back(face.id);
                plan.faces.push_back(std::move(face));
            }
    }

    // 6. Dowel sites (P2, design 3.6), face by face.
    plan.dowel_options = options.dowels;
    if (options.dowels.enabled && !plan.faces.empty())
        place_dowels(plan, mesh, sections, positions, cancel_point);

    // 7. Model-wide warnings.
    if (hollow_plane >= 0)
        plan.warnings.push_back({ SplitWarning::Kind::HollowModel, hollow_plane, -1, hollow_wall });
    if (const size_t open = its_num_open_edges(mesh); open > 0)
        plan.warnings.push_back({ SplitWarning::Kind::ModelNotWatertight, -1, -1, double(open) });

    plan.valid = plan.part_count() > 0;
    return plan;
}

indexed_triangle_set split_object_mesh(const ModelObject &object, int instance)
{
    indexed_triangle_set out;
    if (instance < 0 || size_t(instance) >= object.instances.size())
        return out;
    const Transform3d inst = object.instances[size_t(instance)]->get_transformation().get_matrix();
    for (const ModelVolume *v : object.volumes) {
        if (!v->is_model_part() || v->mesh().empty())
            continue;
        TriangleMesh m = v->mesh();
        m.transform(inst * v->get_matrix(), true);
        its_merge(out, m.its);
    }
    return out;
}

// ---- apply ------------------------------------------------------------------------------------------

SplitApplyResult::SplitApplyResult() : model(std::make_unique<Model>()) {}
SplitApplyResult::~SplitApplyResult() = default;
SplitApplyResult::SplitApplyResult(SplitApplyResult &&) noexcept = default;
SplitApplyResult &SplitApplyResult::operator=(SplitApplyResult &&) noexcept = default;

namespace split_to_volume_detail {

// A copied instance forgets that its assembled pose was set; put it back from the original.
static void copy_assembled_poses(const ModelObject &from, ModelObject &to)
{
    for (size_t i = 0; i < from.instances.size() && i < to.instances.size(); ++i)
        if (from.instances[i]->is_assemble_initialized())
            to.instances[i]->set_assemble_transformation(from.instances[i]->get_assemble_transformation());
}

// cut_mesh() treats a vertex within 1e-3 mm of the plane as lying on it, and a plane through
// existing vertices can leave a few open edges. A grid hits that all the time: an even split of a
// box puts the second plane exactly where the first cut's cap met the box's face diagonals. So the
// plane is moved, by at most a twentieth of a millimetre, to where no vertex of the piece is that
// close. Returns `pos` itself when it is already clear.
static double clear_of_vertices(const ModelObject &piece, int axis, double pos)
{
    std::vector<double> coords;
    const Transform3d   inst = piece.instances.front()->get_transformation().get_matrix();
    for (const ModelVolume *v : piece.volumes) {
        if (!v->is_model_part() || v->mesh().empty())
            continue;
        const Transform3d m = inst * v->get_matrix();
        for (const Vec3f &p : v->mesh().its.vertices)
            coords.push_back((m * p.cast<double>())[axis]);
    }
    std::sort(coords.begin(), coords.end());
    auto clearance = [&coords](double x) {
        auto   it = std::lower_bound(coords.begin(), coords.end(), x);
        double d  = std::numeric_limits<double>::max();
        if (it != coords.end())
            d = std::min(d, *it - x);
        if (it != coords.begin())
            d = std::min(d, x - *(it - 1));
        return d;
    };
    static constexpr double CLEAR = 2.5e-3, STEP = 2.5e-3;
    if (clearance(pos) > CLEAR)
        return pos;
    for (int k = 1; k <= 20; ++k) {
        if (clearance(pos + double(k) * STEP) > CLEAR)
            return pos + double(k) * STEP;
        if (clearance(pos - double(k) * STEP) > CLEAR)
            return pos - double(k) * STEP;
    }
    return pos;
}

static size_t model_part_open_edges(const ModelObject &object)
{
    size_t n = 0;
    for (const ModelVolume *v : object.volumes)
        if (v->is_model_part() && !v->mesh().empty())
            n += its_num_open_edges(v->mesh().its);
    return n;
}

struct SplitApplyRun
{
    const SplitPlan               &plan;
    std::array<std::vector<double>, 3> positions;
    Model                         &scratch;
    std::function<bool()>          canceled;
    std::function<void(int, int)>  progress;
    int                            cuts_done{ 0 };
    int                            cuts_total{ 0 };
    bool                           stopped{ false };
    // (object in scratch, its cell)
    std::vector<std::pair<ModelObject *, SplitCell>> leaves;
    // P2: the dowel / pin objects the cuts made (in scratch).
    std::vector<ModelObject *> dowels;

    // The sites of plane `plane_idx` on faces inside the piece's cells [lo, hi).
    std::vector<const SplitSite *> sites_in(int plane_idx, int axis, const SplitCell &lo, const SplitCell &hi) const
    {
        std::vector<const SplitSite *> out;
        if (plane_idx < 0)
            return out;
        for (const SplitSite &s : plan.sites) {
            if (s.plane != plane_idx || s.face < 0 || size_t(s.face) >= plan.faces.size())
                continue;
            const SplitCell &c      = plan.faces[size_t(s.face)].cell_below;
            bool             inside = true;
            for (int b = 0; b < 3; ++b)
                if (b != axis && (c[b] < lo[b] || c[b] >= hi[b]))
                    inside = false;
            if (inside)
                out.push_back(&s);
        }
        return out;
    }

    // A loose dowel or pin a cut made: one connector solid and nothing else.
    static bool is_dowel_object(const ModelObject &o)
    {
        return o.volumes.size() == 1 && o.volumes.front()->is_model_part() && o.volumes.front()->cut_info.is_connector &&
               is_separate_pin_connector_type(o.volumes.front()->cut_info.connector_type);
    }

    void run(ModelObject *piece, SplitCell lo, SplitCell hi)
    {
        if (stopped)
            return;
        int axis = -1;
        for (int a = 0; a < 3; ++a)
            if (hi[a] - lo[a] > 1) {
                axis = a;
                break;
            }
        if (axis < 0) {
            leaves.emplace_back(piece, lo);
            return;
        }
        if (canceled && canceled()) {
            stopped = true;
            return;
        }
        // The middle plane of this axis inside the piece: between cells m - 1 and m.
        const int    m   = lo[axis] + (hi[axis] - lo[axis]) / 2;
        const double nominal = clear_of_vertices(*piece, axis, positions[axis][size_t(m) - 1]);
        const size_t open_in = model_part_open_edges(*piece);

        // P2: this plane's dowels on this piece, baked as connectors just before the cut (the Cut
        // tool's own path: bake_cut_connectors(), then a cut with CreateDowels). They sit in the
        // frame the cut matrix is in: the world without the instance's offset.
        const std::vector<const SplitSite *> sites = sites_in(plan.plane_index(axis, m - 1), axis, lo, hi);
        const size_t                         baked_from = piece->volumes.size();
        if (!sites.empty()) {
            const Vec3d offset = piece->instances.front()->get_offset();
            for (const SplitSite *s : sites) {
                Vec3d pos = s->pos;
                pos[axis] = nominal;
                piece->cut_connectors.push_back(site_connector(plan.dowel_options, s->diameter, s->half_length, pos - offset, plane_rotation(axis)));
            }
            bake_cut_connectors(piece, "Dowel");
            // The site's own name, so holes and dowels say where they belong ("X1.2-1_A").
            for (size_t i = 0; i < sites.size() && baked_from + i < piece->volumes.size(); ++i)
                piece->volumes[baked_from + i]->name = sites[i]->name;
        }
        double baked_at = nominal;

        std::vector<ModelObject *> below, above;
        // A cut that opens edges the piece did not have is made again a little to the side (at most
        // three times, 0.01 mm apart); the last try is kept whatever it gives (warned after Apply).
        static const double RETRY[] = { 0., 0.01, -0.01, 0.02 };
        for (size_t attempt = 0; attempt < sizeof(RETRY) / sizeof(RETRY[0]); ++attempt) {
            const double pos = nominal + RETRY[attempt];
            // Cut::perform_with_plane() takes the plane in the frame of the instance without its offset
            // (the gizmo's get_cut_matrix() does the same): only the coordinate along the axis matters.
            const Vec3d offset = piece->instances.front()->get_offset();
            Vec3d       point  = Vec3d::Zero();
            point[axis]        = pos;
            const Transform3d cut_matrix = Geometry::translation_transform(point - offset) * plane_rotation(axis);
            // The dowels follow the plane when a retry moves it.
            if (!sites.empty() && pos != baked_at) {
                Vec3d shift = Vec3d::Zero();
                shift[axis] = pos - baked_at;
                for (size_t i = baked_from; i < piece->volumes.size(); ++i)
                    piece->volumes[i]->set_offset(piece->volumes[i]->get_offset() + shift);
                baked_at = pos;
            }

            ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                                  ModelObjectCutAttribute::KeepPaint;
            if (!sites.empty())
                attributes = attributes | ModelObjectCutAttribute::CreateDowels;
            Cut cut(piece, 0, cut_matrix, attributes);
            const ModelObjectPtrs &res = cut.perform_with_plane();
            size_t open_out = 0;
            for (const ModelObject *r : res)
                if (r != nullptr)
                    open_out += model_part_open_edges(*r);
            const bool last = attempt + 1 == sizeof(RETRY) / sizeof(RETRY[0]);
            if (open_out > open_in && !last)
                continue;
            for (const ModelObject *r : res) {
                if (r == nullptr || r->volumes.empty() || r->instances.empty())
                    continue;
                if (!sites.empty() && is_dowel_object(*r)) {
                    ModelObject *copy = scratch.add_object(*r);
                    copy_assembled_poses(*r, *copy);
                    dowels.push_back(copy);
                    continue;
                }
                // Which side: by the box, not by the order (an empty half is dropped by the cut).
                const BoundingBoxf3 bb   = r->instance_bounding_box(0);
                ModelObject        *copy = scratch.add_object(*r);
                copy_assembled_poses(*r, *copy);
                (bb.center()[axis] > pos ? above : below).push_back(copy);
            }
            break;
        }
        scratch.delete_object(piece);
        ++cuts_done;
        if (progress)
            progress(std::min(cuts_done, cuts_total), cuts_total);

        SplitCell lo_hi = hi, hi_lo = lo;
        lo_hi[axis]     = m;
        hi_lo[axis]     = m;
        for (ModelObject *o : below)
            run(o, lo, lo_hi);
        for (ModelObject *o : above)
            run(o, hi_lo, hi);
    }
};

} // namespace split_to_volume_detail

SplitApplyResult split_apply(const ModelObject &object, int instance, const SplitPlan &plan, std::function<bool()> canceled,
                             std::function<void(int, int)> progress)
{
    using namespace split_to_volume_detail;
    SplitApplyResult result;
    if (instance < 0 || size_t(instance) >= object.instances.size() || plan.parts.empty())
        return result;

    Model        scratch;
    ModelObject *root = scratch.add_object(object);
    for (size_t i = root->instances.size(); i-- > 0;)
        if (i != size_t(instance))
            root->delete_instance(i);
    if (object.instances[size_t(instance)]->is_assemble_initialized())
        root->instances.front()->set_assemble_transformation(object.instances[size_t(instance)]->get_assemble_transformation());

    SplitApplyRun run{ plan, {}, scratch, std::move(canceled), std::move(progress) };
    for (int a = 0; a < 3; ++a)
        run.positions[a] = plan.plane_positions(a);
    run.cuts_total = std::max(0, int(plan.cell_count()) - 1);
    run.run(root, { { 0, 0, 0 } }, plan.counts);

    if (run.stopped) {
        result.canceled = true;
        return result;
    }

    // Leaves in plan part order.
    std::stable_sort(run.leaves.begin(), run.leaves.end(), [&plan](const auto &l, const auto &r) {
        return plan.cell_index(l.second) < plan.cell_index(r.second);
    });

    // P2: the dowels, in site order, named after their site (D7: "-Dowel-" as the Cut tool names them).
    std::vector<ModelObject *> dowels;
    {
        const bool        pin  = plan.dowel_options.type == SplitConnectorType::Pin;
        const std::string base = object.name;
        std::vector<std::pair<size_t, ModelObject *>> ordered;
        for (ModelObject *d : run.dowels) {
            const std::string &vname = d->volumes.front()->name;
            size_t             order = plan.sites.size();
            for (size_t i = 0; i < plan.sites.size(); ++i)
                if (plan.sites[i].name == vname) {
                    order = i;
                    break;
                }
            ordered.emplace_back(order, d);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto &l, const auto &r) { return l.first < r.first; });
        for (const auto &od : ordered) {
            const ModelObject *d    = od.second;
            ModelObject       *copy = result.model->add_object(*d);
            copy_assembled_poses(*d, *copy);
            copy->name = base + (pin ? "-Pin-" : "-Dowel-") + d->volumes.front()->name;
            copy->invalidate_bounding_box();
            dowels.push_back(copy);
        }
    }

    // Cut links (D10): linked when dowels exist (or the object already was); the check sum counts the
    // objects of the group, as the Cut tool keeps it.
    CutObjectBase linked;
    const bool    was_linked = !object.cut_id.id().invalid();
    const bool    link       = was_linked || !dowels.empty();
    if (link) {
        if (was_linked)
            linked.copy(object.cut_id);
        else
            linked.init();
        const size_t made = run.leaves.size() + dowels.size();
        if (made > 1)
            linked.increase_check_sum(made - 1);
        linked.increase_connectors_cnt(dowels.size());
    }
    for (ModelObject *d : dowels) {
        if (link)
            d->cut_id.copy(linked);
        else
            d->cut_id.invalidate();
        result.dowels.push_back(d);
    }

    for (const auto &[leaf, cell] : run.leaves) {
        ModelObject *part = result.model->add_object(*leaf);
        copy_assembled_poses(*leaf, *part);
        const std::string suffix = split_cell_name(cell, plan.counts);
        if (!suffix.empty())
            part->name = object.name.empty() ? suffix : object.name + " " + suffix;
        if (link)
            part->cut_id.copy(linked);
        else
            part->cut_id.invalidate();
        part->invalidate_bounding_box();

        const int pi = plan.cell_index(cell);
        result.parts.push_back(part);
        result.part_index.push_back(pi);

        // D15: loose pieces stay in the part, with a warning; open edges are reported too (R2).
        indexed_triangle_set merged;
        size_t               open = 0;
        for (const ModelVolume *v : part->volumes) {
            if (!v->is_model_part() || v->mesh().empty())
                continue;
            open += its_num_open_edges(v->mesh().its);
            TriangleMesh m = v->mesh();
            m.transform(v->get_matrix(), true);
            its_merge(merged, m.its);
        }
        if (const size_t pieces = its_number_of_patches(merged); pieces > 1)
            result.warnings.push_back({ SplitWarning::Kind::LoosePieces, -1, pi, double(pieces) });
        if (open > 0)
            result.warnings.push_back({ SplitWarning::Kind::OpenEdges, -1, pi, double(open) });
    }
    return result;
}

} // namespace Slic3r
