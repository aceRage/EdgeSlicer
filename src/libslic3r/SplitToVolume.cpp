#include "SplitToVolume.hpp"

#include "AABBTreeIndirect.hpp"
#include "AABBTreeLines.hpp"
#include "ClipperUtils.hpp"
#include "Config.hpp"
#include "CutUtils.hpp"
#include "ExtruderAreas.hpp"
#include "Geometry.hpp"
#include "KDTreeIndirect.hpp"
#include "Model.hpp"
#include "PaintTransfer.hpp"
#include "PrintConfig.hpp"
#include "PushFit.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>

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

    // Near-ties (a long face has a whole ridge of points equally far from its outline) go to the
    // point nearest the middle of the region, so the first dowel sits in the middle, not at one end.
    Vec2d centre = Vec2d::Zero();
    for (const Vec2d &c : cand)
        centre += c;
    centre /= double(cand.size());
    static constexpr double TIE = 1e-3;

    std::vector<bool> alive(cand.size(), true);
    int               rejects = 0;
    while (int(out.size()) < max_count) {
        int    best        = -1;
        double best_score  = -1.;
        double best_centre = 0.;
        for (size_t i = 0; i < cand.size(); ++i) {
            if (!alive[i])
                continue;
            double d = std::numeric_limits<double>::max();
            for (const Vec2d &p : out)
                d = std::min(d, (cand[i] - p).norm());
            if (!out.empty() && d < min_spacing - 1e-9)
                continue;
            const double score    = out.empty() ? clr[i] : std::min(clr[i], 0.5 * d);
            const double to_centre = (cand[i] - centre).norm();
            // Exact ties go to the earlier candidate, so the same face always gives the same sites.
            if (best < 0 || score > best_score + TIE || (score > best_score - TIE && to_centre < best_centre - 1e-9)) {
                best_score  = score;
                best_centre = to_centre;
                best        = int(i);
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

// ---- plane adjustment (P3) ----------------------------------------------------------------------------

SplitSectionCost split_section_cost(const ExPolygons &section, double area_before, double area_after, double shift,
                                    const SplitOptions &options)
{
    using namespace split_to_volume_detail;
    const SplitAdjustConstants &k = options.adjust;
    const SplitDowelOptions    &d = options.dowels;
    SplitSectionCost            c;

    // An island smaller than a dowel with its wall margin all round is a thin piece.
    const double r          = 0.5 * std::max(0., d.diameter);
    const double small_area = PI * (r + d.wall_margin) * (r + d.wall_margin);
    // Where a full-size dowel's centre may go: inset by the wall margin and the hole's footprint.
    const bool   dowels     = d.enabled && d.diameter > 0. && d.length > 0.;
    const float  inset      = dowels ? float(scale_(d.wall_margin + site_footprint_radius(d, d.diameter, 0.5 * d.length))) : 0.f;

    double perimeter = 0., takes_dowel = 0.;
    for (const ExPolygon &ex : section) {
        const double a = ex.area() * SCALING_FACTOR * SCALING_FACTOR;
        if (a < MIN_FACE_AREA)
            continue;
        c.area += a;
        perimeter += ex.contour.length() * SCALING_FACTOR;
        for (const Polygon &h : ex.holes)
            perimeter += h.length() * SCALING_FACTOR;
        if (a < small_area)
            ++c.small_islands;
        if (dowels && !offset_ex(ex, -inset).empty())
            takes_dowel += a;
    }
    if (dowels && c.area > 0.)
        c.nodowel = std::clamp(1. - takes_dowel / c.area, 0., 1.);
    c.busy = std::min(1., std::abs(area_after - area_before) / std::max(c.area, 1.));
    if (c.area > 0.)
        c.ragged = std::clamp(perimeter / (2. * std::sqrt(PI * c.area)) - 1., 0., 1.);
    c.away  = options.adjust_limit > 0. ? std::min(1., std::abs(shift) / options.adjust_limit) : 0.;
    c.total = k.w_small * double(c.small_islands) + k.w_nodowel * c.nodowel + k.w_busy * c.busy + k.w_ragged * c.ragged + k.w_away * c.away;
    return c;
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

size_t SplitPlan::piece_count() const
{
    return size_t(std::count_if(pieces.begin(), pieces.end(), [](const SplitPiece &p) { return !p.discarded; }));
}

size_t SplitPlan::pieces_not_fitting() const
{
    return size_t(std::count_if(pieces.begin(), pieces.end(), [](const SplitPiece &p) { return !p.discarded && !p.fits; }));
}

size_t SplitPlan::discarded_count() const
{
    return size_t(std::count_if(pieces.begin(), pieces.end(), [](const SplitPiece &p) { return p.discarded; }));
}

double SplitPlan::discarded_volume() const
{
    double v = 0.;
    for (const SplitPiece &p : pieces)
        if (p.discarded)
            v += p.volume;
    return v;
}

std::vector<int> SplitPlan::pieces_of(int section) const
{
    std::vector<int> out;
    for (size_t i = 0; i < pieces.size(); ++i)
        if (pieces[i].section == section)
            out.push_back(int(i));
    return out;
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
// Material along `dir` from `origin` (mm): the first surface the ray meets. A ray that meets nothing
// started outside the material.
struct MaterialDepth
{
    const indexed_triangle_set   &mesh;
    const AABBTreeIndirect::Tree3f &tree;
    mutable std::vector<igl::Hit>  hits;

    double operator()(const Vec3d &origin, const Vec3d &dir) const
    {
        AABBTreeIndirect::intersect_ray_all_hits(mesh.vertices, mesh.indices, tree, origin, dir, hits);
        for (const igl::Hit &h : hits)
            if (double(h.t) > RAY_EPS)
                return double(h.t);
        return 0.;
    }
};

// P5: the faces' mating pairs, face by face (indices in plan.mates).
static std::vector<std::vector<int>> mates_by_face(const SplitPlan &plan)
{
    std::vector<std::vector<int>> out(plan.faces.size());
    for (const SplitMate &m : plan.mates)
        if (m.face >= 0 && size_t(m.face) < out.size())
            out[size_t(m.face)].push_back(m.id);
    return out;
}

// P5: the cap ("up to N") is per mating pair, shared round-robin over the pair's patches; a pair that
// gets no dowel is warned about, so a loose piece is never left without one silently.
static void place_dowels(SplitPlan &plan, const MaterialDepth &depth_along, const std::array<std::vector<double>, 3> &positions,
                         const std::function<void()> &cancel_point)
{
    const SplitDowelOptions &opt = plan.dowel_options;
    if (opt.diameter <= 0. || opt.length <= 0. || opt.max_per_face <= 0)
        return;
    const std::vector<std::vector<int>> face_mates = mates_by_face(plan);
    // P4: the user's edits handled on some pair so far.
    std::vector<bool> manual_done(plan.manual_sites.size(), false);

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

        // Room along the normal before the next parallel plane (a model end is left to the depth check).
        const double room_below = pl.index > 0 ? pl.pos - positions[a][size_t(pl.index) - 1] : inf;
        const double room_above = size_t(pl.index) + 1 < positions[a].size() ? positions[a][size_t(pl.index) + 1] - pl.pos : inf;

        face.islands = 0;
        for (const int mate_id : face_mates[size_t(face.id)]) {
        SplitMate &mate = plan.mates[size_t(mate_id)];
        ExPolygons islands = mate.patches;
        islands.erase(std::remove_if(islands.begin(), islands.end(),
                                     [](const ExPolygon &ex) { return ex.area() * SCALING_FACTOR * SCALING_FACTOR < MIN_FACE_AREA; }),
                      islands.end());
        std::stable_sort(islands.begin(), islands.end(), [](const ExPolygon &l, const ExPolygon &r) { return l.area() > r.area(); });
        face.islands += int(islands.size());
        if (islands.empty())
            continue;

        struct Pick { Vec2d uv; double half; int island; bool manual{ false }; };
        bool had_room = false;
        // The depth check: the centre and 8 points round the footprint, both ways; the half-length a
        // dowel there may have.
        auto half_at = [&](const Vec2d &uv, double fp) {
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
            return std::min({ req_half, up - opt.min_material - extra, down - opt.min_material - extra,
                              room_above - opt.corner_keepout - extra, room_below - opt.corner_keepout - extra });
        };
        // Where a dowel centre may be: the face's cell less the keep-out bands along the planes bounding
        // it (not along the model's own ends).
        auto allowed_box = [&](double fp) {
            BoundingBoxf allowed(Vec2d(u0, w0), Vec2d(u1, w1));
            const double band = fp + opt.corner_keepout;
            if (cell[ua] > 0) allowed.min.x() += band;
            if (cell[ua] < plan.counts[ua] - 1) allowed.max.x() -= band;
            if (cell[va] > 0) allowed.min.y() += band;
            if (cell[va] < plan.counts[va] - 1) allowed.max.y() -= band;
            return allowed;
        };
        // The picks for one dowel size, island by island (each list in pick order).
        auto picks_at = [&](double scale) {
            std::vector<std::vector<Pick>> per_island(islands.size());
            const double d  = opt.diameter * scale;
            const double fp = site_footprint_radius(opt, d, req_half);
            const BoundingBoxf allowed = allowed_box(fp);
            if (allowed.max.x() <= allowed.min.x() || allowed.max.y() <= allowed.min.y())
                return per_island;
            for (size_t ii = 0; ii < islands.size(); ++ii) {
                std::vector<Pick> &picks = per_island[ii];
                double             half  = 0.;
                auto accept = [&](const Vec2d &uv) {
                    had_room = true;
                    half     = half_at(uv, fp);
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

        // P5: the warnings name the pair (and so the two pieces) as well as the face.
        auto warn = [&](SplitWarning::Kind kind, double value) {
            SplitWarning w{ kind, face.plane, -1, value, face.id };
            w.mate = mate.id;
            plan.warnings.push_back(w);
        };

        // P4: the user's edits on this pair's patches, while the plane is where it was when they were made.
        // Removals first (the automatic pick nearest the click), then additions, checked by the same
        // rules as the automatic picks and at the size this face uses.
        const bool   auto_none = chosen.empty();
        const double d_face  = opt.diameter * (auto_none ? 1. : scales[level]);
        const double fp_face = site_footprint_radius(opt, d_face, req_half);
        bool         removed_any = false;
        auto on_this_plane = [&](const SplitManualSite &m) {
            return m.axis == a && m.index == pl.index && std::abs(m.plane_pos - pl.pos) <= 1e-6;
        };
        auto island_of = [&](const Vec2d &uv) {
            const Point pt(scaled<coord_t>(uv.x()), scaled<coord_t>(uv.y()));
            for (size_t ii = 0; ii < islands.size(); ++ii)
                if (islands[ii].contains(pt))
                    return int(ii);
            return -1;
        };
        for (size_t mi = 0; mi < plan.manual_sites.size(); ++mi) {
            const SplitManualSite &m = plan.manual_sites[mi];
            if (m.add || manual_done[mi] || !on_this_plane(m))
                continue;
            const Vec2d uv(m.pos[ua], m.pos[va]);
            if (island_of(uv) < 0)
                continue;
            manual_done[mi] = true;
            size_t best = chosen.size();
            double best_d = std::max(fp_face, 1.) + 0.5;
            for (size_t k = 0; k < chosen.size(); ++k)
                if (const double dist = (chosen[k].uv - uv).norm(); dist <= best_d) {
                    best_d = dist;
                    best   = k;
                }
            if (best < chosen.size()) {
                chosen.erase(chosen.begin() + ptrdiff_t(best));
                removed_any = true;
            }
        }
        for (size_t mi = 0; mi < plan.manual_sites.size(); ++mi) {
            const SplitManualSite &m = plan.manual_sites[mi];
            if (!m.add || manual_done[mi] || !on_this_plane(m))
                continue;
            const Vec2d uv(m.pos[ua], m.pos[va]);
            const int   ii = island_of(uv);
            if (ii < 0)
                continue;
            manual_done[mi] = true;
            auto refuse = [&](SplitManualRefusal why) {
                SplitWarning w{ SplitWarning::Kind::ManualSiteRefused, face.plane, -1, double(int(why)), face.id };
                w.mate   = mate.id;
                w.detail = int(mi);
                plan.warnings.push_back(w);
            };
            const BoundingBoxf allowed = allowed_box(fp_face);
            if (uv.x() < allowed.min.x() || uv.x() > allowed.max.x() || uv.y() < allowed.min.y() || uv.y() > allowed.max.y()) {
                refuse(SplitManualRefusal::NearOtherPlane);
                continue;
            }
            double edge = inf;
            auto   edge_of = [&](const Polygon &poly) {
                for (size_t k = 0; k < poly.points.size(); ++k)
                    edge = std::min(edge, point_segment_distance(uv, unscale(poly.points[k]), unscale(poly.points[(k + 1) % poly.points.size()])));
            };
            edge_of(islands[size_t(ii)].contour);
            for (const Polygon &h : islands[size_t(ii)].holes)
                edge_of(h);
            if (edge < opt.wall_margin + fp_face - 1e-6) {
                refuse(SplitManualRefusal::NearEdge);
                continue;
            }
            if (std::any_of(chosen.begin(), chosen.end(), [&](const Pick &p) { return (p.uv - uv).norm() < opt.min_spacing - 1e-6; })) {
                refuse(SplitManualRefusal::NearDowel);
                continue;
            }
            const double half = half_at(uv, fp_face);
            if (half < DOWEL_MIN_HALF_PER_DIAMETER * d_face) {
                refuse(SplitManualRefusal::TooLittleMaterial);
                continue;
            }
            chosen.push_back({ uv, half, ii, true });
        }

        if (chosen.empty()) {
            // A face whose dowels the user took off is not warned about: that was asked for.
            if (!removed_any)
                warn(SplitWarning::Kind::NoDowelOnFace, had_room ? SPLIT_NO_DOWEL_TOO_THIN : SPLIT_NO_DOWEL_TOO_SMALL);
            continue;
        }

        const double d        = d_face;
        const double fp       = fp_face;
        double       shortest = inf;
        std::vector<bool> island_has(islands.size(), false);
        for (const Pick &pk : chosen) {
            SplitSite s;
            s.face             = face.id;
            s.plane            = face.plane;
            s.island           = pk.island;
            s.mate             = mate.id;
            s.pos[a]           = pl.pos;
            s.pos[ua]          = pk.uv.x();
            s.pos[va]          = pk.uv.y();
            s.diameter         = d;
            s.half_length      = pk.half;
            s.footprint_radius = fp;
            s.shortened        = pk.half < req_half - 1e-6;
            s.scaled_down      = !auto_none && level > 0;
            s.manual           = pk.manual;
            s.name             = mate.code + "-" + std::to_string(mate.sites.size() + 1);
            shortest           = std::min(shortest, pk.half);
            island_has[size_t(pk.island)] = true;
            face.sites.push_back(int(plan.sites.size()));
            mate.sites.push_back(int(plan.sites.size()));
            plan.sites.push_back(std::move(s));
        }
        int without = 0;
        for (size_t ii = 0; ii < islands.size(); ++ii)
            if (!island_has[ii] && islands[ii].area() * SCALING_FACTOR * SCALING_FACTOR >= worth_area)
                ++without;
        if (without > 0)
            warn(SplitWarning::Kind::IslandsWithoutDowel, double(without));
        if (chosen.size() == 1 && dowel_is_round(opt))
            warn(SplitWarning::Kind::DowelCanTurn, 0.);
        if (!auto_none && level > 0)
            warn(SplitWarning::Kind::DowelScaledDown, d);
        if (shortest < req_half - 1e-6)
            warn(SplitWarning::Kind::DowelShortened, 2. * shortest);
        } // mates of the face
    }

    // P4: an added site on a plane that is still where it was, but on no pair's material.
    for (size_t mi = 0; mi < plan.manual_sites.size(); ++mi) {
        const SplitManualSite &m = plan.manual_sites[mi];
        if (!m.add || manual_done[mi])
            continue;
        const int pidx = plan.plane_index(m.axis, m.index);
        if (pidx < 0 || std::abs(plan.planes[size_t(pidx)].pos - m.plane_pos) > 1e-6)
            continue;   // the plane moved: the edit no longer applies
        SplitWarning w{ SplitWarning::Kind::ManualSiteRefused, pidx, -1, double(int(SplitManualRefusal::NotOnFace)) };
        w.detail = int(mi);
        plan.warnings.push_back(w);
    }
}

// Sections of `mesh` across `axis` at every position of `zs`, as material areas and outlines.
// `zs` need not be sorted or unique.
struct AxisSections
{
    std::vector<float>      zs;      // sorted, unique
    std::vector<ExPolygons> slices;
    std::vector<double>     areas;   // mm2

    size_t index(double z) const
    {
        const float f  = float(z);
        auto        it = std::lower_bound(zs.begin(), zs.end(), f);
        if (it == zs.end())
            --it;
        return size_t(it - zs.begin());
    }
};

static AxisSections slice_axis(const indexed_triangle_set &mesh, int axis, std::vector<float> zs, const std::function<void()> &cancel_point)
{
    AxisSections out;
    std::sort(zs.begin(), zs.end());
    zs.erase(std::unique(zs.begin(), zs.end()), zs.end());
    out.zs = std::move(zs);
    if (out.zs.empty())
        return out;
    MeshSlicingParamsEx params;
    params.trafo = axis_to_z(axis);
    out.slices   = slice_mesh_ex(mesh, out.zs, params, [&cancel_point]() { cancel_point(); });
    out.slices.resize(out.zs.size());
    out.areas.reserve(out.slices.size());
    for (const ExPolygons &s : out.slices) {
        double a = 0.;
        for (const ExPolygon &ex : s)
            if (const double ea = ex.area() * SCALING_FACTOR * SCALING_FACTOR; ea >= MIN_FACE_AREA)
                a += ea;
        out.areas.push_back(a);
    }
    return out;
}

// Design 3.5: per axis, every free plane tries positions nominal + k * step (|k| * step <= limit);
// each is costed from its section (split_section_cost()) and the hard rules decide which ones may be
// combined: every part between two planes (or a plane and the model's end) no longer than the usable
// size (rule 1) and no thinner than the smallest part (rule 2), and no plane near a jump in the section
// (rule 3). Dynamic programming over the planes in order finds the cheapest combination. Pinned planes
// stay where the user put them. Writes the new positions into plan.planes and the adjustment warnings.
static void adjust_planes(SplitPlan &plan, const indexed_triangle_set &mesh, const SplitOptions &options,
                          const std::function<void()> &cancel_point)
{
    const SplitAdjustConstants &kc    = options.adjust;
    const double                limit = std::clamp(options.adjust_limit, 0., 100.);
    const double                step  = kc.step > 0. ? kc.step : 1.;
    const int                   K     = int(std::floor(limit / step + 1e-9));
    if (K <= 0)
        return;
    const double hs = 0.5 * step;            // sections are taken every half step
    const int    H  = 2 * K + 2;             // half steps each side of the nominal place
    const int    J  = std::clamp(int(std::lround(kc.jump_distance / hs)), 0, 2);
    const double inf = std::numeric_limits<double>::infinity();
    const Vec3d  lo  = plan.model_bbox.min;
    const Vec3d  hi  = plan.model_bbox.max;

    // Candidate order k = 0, -1, +1, -2, +2, ...: an exact tie goes to the smaller move.
    std::vector<int> ks{ 0 };
    for (int k = 1; k <= K; ++k) {
        ks.push_back(-k);
        ks.push_back(k);
    }

    for (int a = 0; a < 3; ++a) {
        std::vector<int> idx;
        for (size_t i = 0; i < plan.planes.size(); ++i)
            if (plan.planes[i].axis == a)
                idx.push_back(int(i));
        if (idx.empty() || int(idx.size()) > kc.max_planes_per_axis)
            continue;
        if (std::all_of(idx.begin(), idx.end(), [&plan](int i) { return plan.planes[size_t(i)].pinned; }))
            continue;

        const double U     = plan.usable.size[a];
        const double min_t = std::min(options.min_part_thickness, 0.5 * U);
        auto         span_ok = [U, min_t](double span) { return span <= U + POS_EPS && span >= min_t - POS_EPS; };
        auto         fits_U  = [U](double span) { return span <= U + POS_EPS; };

        // The planes as laid out must already keep to the rules; a user's plane that made a part too
        // long is reported as a part that does not fit, and this axis is left alone.
        {
            double prev = lo[a];
            bool   ok   = true;
            for (int i : idx) {
                ok   = ok && span_ok(plan.planes[size_t(i)].pos - prev);
                prev = plan.planes[size_t(i)].pos;
            }
            if (!ok || !span_ok(hi[a] - prev))
                continue;
        }

        // Every section the free planes need, in one slicing pass.
        std::vector<float> zs;
        for (int i : idx) {
            const SplitPlane &pl = plan.planes[size_t(i)];
            if (!pl.pinned)
                for (int j = -H; j <= H; ++j)
                    zs.push_back(float(pl.nominal + double(j) * hs));
        }
        const AxisSections sec = slice_axis(mesh, a, std::move(zs), cancel_point);
        cancel_point();

        struct Cand
        {
            double           pos{ 0. };
            int              k{ 0 };
            bool             jump{ false };   // rule 3
            SplitSectionCost cost;
        };
        std::vector<std::vector<Cand>> cands(idx.size());
        for (size_t i = 0; i < idx.size(); ++i) {
            const SplitPlane &pl = plan.planes[size_t(idx[i])];
            if (pl.pinned) {
                Cand c;
                c.pos = pl.pos;
                cands[i].push_back(c);
                continue;
            }
            auto area_at = [&](int j) { return sec.areas[sec.index(pl.nominal + double(j) * hs)]; };
            for (int k : ks) {
                Cand c;
                c.k   = k;
                c.pos = pl.nominal + double(k) * step;
                for (int j = 2 * k - J; j < 2 * k + J; ++j) {
                    const double a0 = area_at(j), a1 = area_at(j + 1);
                    const double big = std::max(a0, a1);
                    if (big >= MIN_FACE_AREA && std::abs(a1 - a0) > kc.jump_ratio * big)
                        c.jump = true;
                }
                c.cost = split_section_cost(sec.slices[sec.index(pl.nominal + double(2 * k) * hs)], area_at(2 * k - 2), area_at(2 * k + 2),
                                            double(k) * step, options);
                cands[i].push_back(c);
            }
        }

        // Dynamic programming over the planes in order.
        std::vector<std::vector<double>> best(idx.size());
        std::vector<std::vector<int>>    from(idx.size());
        for (size_t i = 0; i < idx.size(); ++i) {
            best[i].assign(cands[i].size(), inf);
            from[i].assign(cands[i].size(), -1);
            for (size_t c = 0; c < cands[i].size(); ++c) {
                const Cand &cd = cands[i][c];
                if (cd.jump)
                    continue;
                if (i == 0) {
                    if (span_ok(cd.pos - lo[a]))
                        best[i][c] = cd.cost.total;
                    continue;
                }
                for (size_t p = 0; p < cands[i - 1].size(); ++p)
                    if (best[i - 1][p] < inf && span_ok(cd.pos - cands[i - 1][p].pos) && best[i - 1][p] + cd.cost.total < best[i][c] - 1e-12) {
                        best[i][c] = best[i - 1][p] + cd.cost.total;
                        from[i][c] = int(p);
                    }
            }
        }
        int    last = -1;
        double total = inf;
        for (size_t c = 0; c < cands.back().size(); ++c)
            if (best.back()[c] < inf && span_ok(hi[a] - cands.back()[c].pos) && best.back()[c] < total - 1e-12) {
                total = best.back()[c];
                last  = int(c);
            }
        if (last < 0) {
            // Rule 3 ruled out every combination: the planes stay as laid out.
            plan.warnings.push_back({ SplitWarning::Kind::AdjustImpossible, idx.front(), -1, double(a) });
            continue;
        }
        std::vector<int> chosen(idx.size(), 0);
        for (int i = int(idx.size()) - 1, c = last; i >= 0; --i) {
            chosen[size_t(i)] = c;
            c                 = from[size_t(i)][size_t(c)];
        }

        for (size_t i = 0; i < idx.size(); ++i) {
            SplitPlane &pl = plan.planes[size_t(idx[i])];
            if (pl.pinned)
                continue;
            const Cand &ch  = cands[i][size_t(chosen[i])];
            const Cand &nom = cands[i].front();   // k = 0
            pl.pos          = ch.pos;
            pl.adjusted     = ch.k != 0;
            const double prev = i == 0 ? lo[a] : cands[i - 1][size_t(chosen[i - 1])].pos;
            const double next = i + 1 == idx.size() ? hi[a] : cands[i + 1][size_t(chosen[i + 1])].pos;

            if (pl.adjusted) {
                SplitAdjustReason why = SplitAdjustReason::Better;
                if (nom.jump)
                    why = SplitAdjustReason::FlatFace;
                else if (nom.cost.total <= ch.cost.total + 1e-12)
                    why = SplitAdjustReason::KeepFit;   // moved for a neighbour, not for its own section
                else {
                    // The term that gained the most.
                    const double gains[4] = { kc.w_small * double(nom.cost.small_islands - ch.cost.small_islands), kc.w_nodowel * (nom.cost.nodowel - ch.cost.nodowel),
                                              kc.w_busy * (nom.cost.busy - ch.cost.busy), kc.w_ragged * (nom.cost.ragged - ch.cost.ragged) };
                    static const SplitAdjustReason reasons[4] = { SplitAdjustReason::ThinPieces, SplitAdjustReason::DowelRoom,
                                                                  SplitAdjustReason::FineDetail, SplitAdjustReason::RaggedOutline };
                    double top = 1e-9;
                    for (int t = 0; t < 4; ++t)
                        if (gains[t] > top) {
                            top = gains[t];
                            why = reasons[t];
                        }
                }
                SplitWarning w{ SplitWarning::Kind::PlaneMoved, idx[i], -1, ch.pos - pl.nominal };
                w.detail = int(why);
                plan.warnings.push_back(w);
            }

            // The cheapest place rule 3 allows, if rule 1 alone kept the plane from it.
            const Cand *free_best = nullptr;
            for (const Cand &c : cands[i])
                if (!c.jump && (free_best == nullptr || c.cost.total < free_best->cost.total - 1e-12))
                    free_best = &c;
            if (free_best != nullptr && free_best->cost.total < ch.cost.total - 1e-9 &&
                (!fits_U(free_best->pos - prev) || !fits_U(next - free_best->pos)))
                plan.warnings.push_back({ SplitWarning::Kind::AdjustRefused, idx[i], -1, free_best->pos - pl.nominal });

            // Still through fine detail or thin pieces: nothing better within the limit.
            if (ch.cost.busy >= 0.5 || ch.cost.small_islands > 0)
                plan.warnings.push_back({ SplitWarning::Kind::PlaneInDetail, idx[i], -1, limit });
        }
    }
}

// ---- pieces (P5) --------------------------------------------------------------------------------------
//
// A section's material can fall apart into several pieces (an ear tip and a wedge of the back that only
// met through material now in another section). The pieces are found without cutting the mesh:
//
//  - every triangle, clipped to the cells it crosses, is a FRAGMENT of the surface in each of them;
//  - the plane's section on each face rectangle splits into ISLANDS, each a cap of the cell below and
//    of the cell above;
//  - two fragments in one cell are joined where their triangles share an edge that runs through the
//    cell; a fragment is joined to the cap its triangle's crossing of the plane lies on; two caps of one
//    cell on planes that meet are joined where both run along the line the planes meet in (a cell with
//    no surface at all, deep inside a big model, is one piece of six caps that way);
//  - pieces from different shells of the model whose boxes overlap stay together (a model made of
//    overlapping bodies is not pulled apart); a void (an inward-facing shell) goes with the piece it is in.
//
// One pass over the triangles and one over their edges, with the face neighbours given (the gizmo keeps
// them for the mesh). The planes are first moved off the mesh's vertices as Apply moves them, so no
// vertex lies on a plane.

struct SplitUnionFind
{
    std::vector<int> parent;

    void init(size_t n)
    {
        parent.resize(n);
        for (size_t i = 0; i < n; ++i)
            parent[i] = int(i);
    }
    int add()
    {
        parent.push_back(int(parent.size()));
        return int(parent.size()) - 1;
    }
    int find(int x)
    {
        while (parent[size_t(x)] != x) {
            parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
            x                 = parent[size_t(x)];
        }
        return x;
    }
    void unite(int a, int b)
    {
        if (a < 0 || b < 0)
            return;
        a = find(a);
        b = find(b);
        if (a == b)
            return;
        if (a < b)
            std::swap(a, b);
        parent[size_t(a)] = b;
    }
};

// `pos` moved, by at most a twentieth of a millimetre, to where no coordinate of `sorted` is within
// 2.5e-3 mm: what Apply's clear_of_vertices() does to a plane.
static double nudge_off(const std::vector<double> &sorted, double pos)
{
    auto clearance = [&sorted](double x) {
        auto   it = std::lower_bound(sorted.begin(), sorted.end(), x);
        double d  = std::numeric_limits<double>::max();
        if (it != sorted.end())
            d = std::min(d, *it - x);
        if (it != sorted.begin())
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

// One island of a plane's section on one face rectangle: a cap of the cell below and of the cell above.
struct CapIsland
{
    int       plane{ -1 };
    int       face{ -1 };       // SplitPlan::faces, or -1 (a face too small to list)
    int       cell_below{ -1 };
    int       cell_above{ -1 };
    ExPolygon shape;            // scaled, the plane's (u, v)
    double    area{ 0. };       // mm2
    int       node_below{ -1 };
    int       node_above{ -1 };
};

// The islands of one face rectangle, to find the one a point of the outline belongs to.
struct CapIndex
{
    std::vector<int>                                     islands;
    std::vector<int>                                     line_island;
    std::unique_ptr<AABBTreeLines::LinesDistancer<Line>> lines;
};

struct PieceComp
{
    int              cell{ -1 };
    BoundingBoxf3    bbox;
    double           volume{ 0. };
    std::vector<int> shells;
    bool             surface{ false };   // has some of the model's surface (not only caps)
};

static std::string piece_letter(int i)
{
    std::string s;
    do {
        s.insert(s.begin(), char('a' + i % 26));
        i = i / 26 - 1;
    } while (i >= 0);
    return s;
}

// The stretches of an island's outline that lie on the side of its rectangle at `side` (scaled) on
// coordinate `coord` (0: u, 1: v), as intervals of the other coordinate (mm).
static std::vector<std::pair<double, double>> side_intervals(const ExPolygon &ex, int coord, coord_t side)
{
    std::vector<std::pair<double, double>> out;
    static constexpr coord_t TOL = 10;
    auto scan = [&](const Polygon &p) {
        for (size_t i = 0, n = p.points.size(); i < n; ++i) {
            const Point &a = p.points[i];
            const Point &b = p.points[(i + 1) % n];
            if (std::abs(a[coord] - side) <= TOL && std::abs(b[coord] - side) <= TOL) {
                double x0 = unscale<double>(a[1 - coord]), x1 = unscale<double>(b[1 - coord]);
                if (x0 > x1)
                    std::swap(x0, x1);
                if (x1 - x0 > 1e-6)
                    out.emplace_back(x0, x1);
            }
        }
    };
    scan(ex.contour);
    for (const Polygon &h : ex.holes)
        scan(h);
    return out;
}

static bool intervals_overlap(const std::vector<std::pair<double, double>> &l, const std::vector<std::pair<double, double>> &r)
{
    for (const auto &a : l)
        for (const auto &b : r)
            if (std::min(a.second, b.second) - std::max(a.first, b.first) > 1e-4)
                return true;
    return false;
}

static bool boxes_touch(const BoundingBoxf3 &l, const BoundingBoxf3 &r, double tol)
{
    if (!l.defined || !r.defined)
        return false;
    for (int a = 0; a < 3; ++a)
        if (l.max[a] < r.min[a] - tol || r.max[a] < l.min[a] - tol)
            return false;
    return true;
}

static bool box_contains(const BoundingBoxf3 &box, const Vec3d &p, double tol)
{
    if (!box.defined)
        return false;
    for (int a = 0; a < 3; ++a)
        if (p[a] < box.min[a] - tol || p[a] > box.max[a] + tol)
            return false;
    return true;
}

// The piece of section `cell` a point of it belongs to, by the pieces' boxes: the smallest box that
// holds it, else the nearest box centre. -1 when the section has no pieces.
static int piece_at(const SplitPlan &plan, const std::vector<int> &section_pieces, const Vec3d &p)
{
    int    best = -1;
    double best_v = std::numeric_limits<double>::max();
    for (int i : section_pieces) {
        const BoundingBoxf3 &bb = plan.pieces[size_t(i)].bbox;
        if (!box_contains(bb, p, 1e-3))
            continue;
        const Vec3d  s = bb.size();
        const double v = s.x() * s.y() * s.z();
        if (v < best_v) {
            best_v = v;
            best   = i;
        }
    }
    if (best >= 0)
        return best;
    double best_d = std::numeric_limits<double>::max();
    for (int i : section_pieces) {
        const BoundingBoxf3 &bb = plan.pieces[size_t(i)].bbox;
        if (!bb.defined)
            continue;
        const double d = (bb.center() - p).squaredNorm();
        if (d < best_d) {
            best_d = d;
            best   = i;
        }
    }
    return best;
}

// Fills plan.pieces (every non-empty section, its pieces largest first), plan.mates (the pairs of pieces
// that meet on a face, with their patches) and plan.vertex_piece. `sections` are the planes' sections at
// their positions.
static void find_pieces(SplitPlan &plan, const indexed_triangle_set &mesh, const std::vector<Vec3i32> &neighbors,
                        const std::vector<ExPolygons> &sections, const SplitOptions &options, const std::function<void()> &cancel_point)
{
    const size_t T     = mesh.indices.size();
    const int    ncell = int(plan.parts.size());
    const Vec3d  lo    = plan.model_bbox.min;
    const Vec3d  hi    = plan.model_bbox.max;
    const Vec3d  origin = plan.model_bbox.center();
    plan.pieces.clear();
    plan.mates.clear();
    plan.vertex_piece.assign(mesh.vertices.size(), -1);

    // Each non-empty cell's pieces (indices in `pieces`, after step 8).
    std::vector<std::vector<int>> section_pieces(static_cast<size_t>(ncell));
    auto make_piece = [&](int ci, const BoundingBoxf3 &bbox, double volume) {
        SplitPiece p;
        p.section = ci;
        p.bbox    = bbox;
        p.volume  = volume;
        section_pieces[size_t(ci)].push_back(int(plan.pieces.size()));
        plan.pieces.push_back(std::move(p));
    };

    if (plan.planes.empty() || T == 0 || neighbors.size() != T) {
        // Nothing is cut: every non-empty cell is one piece, as it was.
        for (int ci = 0; ci < ncell; ++ci)
            if (!plan.parts[size_t(ci)].empty)
                make_piece(ci, plan.parts[size_t(ci)].bbox, plan.parts[size_t(ci)].volume);
        return;
    }

    // 1. The planes moved off the vertices, as Apply moves them; their sections again where one moved.
    std::array<std::vector<double>, 3> np;
    std::vector<ExPolygons>            secs = sections;
    for (int a = 0; a < 3; ++a) {
        np[a] = plan.plane_positions(a);
        if (np[a].empty())
            continue;
        std::vector<double> coords;
        coords.reserve(mesh.vertices.size());
        for (const Vec3f &v : mesh.vertices)
            coords.push_back(double(v[a]));
        std::sort(coords.begin(), coords.end());
        std::vector<double> moved = np[a];
        bool                any = false, ordered = true;
        for (size_t i = 0; i < moved.size(); ++i) {
            moved[i] = nudge_off(coords, moved[i]);
            any      = any || moved[i] != np[a][i];
            if (i > 0 && moved[i] <= moved[i - 1])
                ordered = false;
        }
        if (!any || !ordered)
            continue;
        np[a] = std::move(moved);
        MeshSlicingParamsEx params;
        params.trafo = axis_to_z(a);
        std::vector<float> zs;
        for (double p : np[a])
            zs.push_back(float(p));
        std::vector<ExPolygons> slices = slice_mesh_ex(mesh, zs, params, [&cancel_point]() { cancel_point(); });
        for (size_t i = 0; i < slices.size() && i < np[a].size(); ++i)
            if (const int idx = plan.plane_index(a, int(i)); idx >= 0)
                secs[size_t(idx)] = std::move(slices[i]);
    }
    auto cell_lo   = [&](int a, int c) { return c == 0 ? lo[a] : np[a][size_t(c) - 1]; };
    auto cell_hi   = [&](int a, int c) { return c == plan.counts[a] - 1 ? hi[a] : np[a][size_t(c)]; };
    auto axis_cell = [&](int a, double x) { return std::min(count_below(np[a], x), plan.counts[a] - 1); };
    auto cell_of   = [&](const Vec3d &p) { return plan.cell_index({ { axis_cell(0, p.x()), axis_cell(1, p.y()), axis_cell(2, p.z()) } }); };
    auto vertex    = [&mesh](int i) { return mesh.vertices[size_t(i)].cast<double>(); };

    SplitUnionFind uf;
    uf.init(T);

    // 2. Caps: each plane's section on every face rectangle between two non-empty cells.
    std::vector<CapIsland>                 caps;
    std::map<std::array<int, 3>, CapIndex> cap_index;   // (plane, cell along u, cell along v)
    std::map<std::pair<int, int>, int>     face_of;     // (plane, cell below) -> face
    for (const SplitFace &f : plan.faces)
        face_of[{ f.plane, plan.cell_index(f.cell_below) }] = f.id;
    for (size_t pi = 0; pi < plan.planes.size(); ++pi) {
        const SplitPlane &pl = plan.planes[pi];
        const int         a = pl.axis, ua = u_axis(a), va = v_axis(a);
        for (int cv = 0; cv < plan.counts[va]; ++cv)
            for (int cu = 0; cu < plan.counts[ua]; ++cu) {
                SplitCell below{ { 0, 0, 0 } };
                below[a]        = pl.index;
                below[ua]       = cu;
                below[va]       = cv;
                SplitCell above = below;
                above[a]        = pl.index + 1;
                const int cb = plan.cell_index(below), ca = plan.cell_index(above);
                if (plan.parts[size_t(cb)].empty || plan.parts[size_t(ca)].empty)
                    continue;
                ExPolygons ex = intersection_ex(secs[pi], Polygons{ scaled_rect(cell_lo(ua, cu), cell_lo(va, cv), cell_hi(ua, cu), cell_hi(va, cv)) });
                CapIndex         &index = cap_index[{ { int(pi), cu, cv } }];
                std::vector<Line> lines;
                for (ExPolygon &e : ex) {
                    const double area = e.area() * SCALING_FACTOR * SCALING_FACTOR;
                    if (area <= 1e-9)
                        continue;
                    CapIsland c;
                    c.plane      = int(pi);
                    c.cell_below = cb;
                    c.cell_above = ca;
                    c.area       = area;
                    const auto it = face_of.find({ int(pi), cb });
                    c.face        = it == face_of.end() ? -1 : it->second;
                    c.node_below  = uf.add();
                    c.node_above  = uf.add();
                    for (const Line &l : e.lines()) {
                        lines.push_back(l);
                        index.line_island.push_back(int(caps.size()));
                    }
                    index.islands.push_back(int(caps.size()));
                    c.shape = std::move(e);
                    caps.push_back(std::move(c));
                }
                if (!lines.empty())
                    index.lines = std::make_unique<AABBTreeLines::LinesDistancer<Line>>(std::move(lines));
            }
    }
    auto cap_at = [&cap_index](int plane, int cu, int cv, const Vec2d &uv) -> int {
        const auto it = cap_index.find({ { plane, cu, cv } });
        if (it == cap_index.end() || !it->second.lines)
            return -1;
        if (it->second.islands.size() == 1)
            return it->second.islands.front();
        const auto [d, idx, nearest] = it->second.lines->distance_from_lines_extra<false>(Point::new_scale(uv.x(), uv.y()));
        return idx < it->second.line_island.size() ? it->second.line_island[idx] : -1;
    };

    // 3. Two caps of one cell on planes that meet: joined where both run along the line they meet in.
    {
        std::map<std::array<int, 3>, std::vector<std::pair<int, std::vector<std::pair<double, double>>>>> along;
        for (const CapIsland &c : caps) {
            const int a = plan.planes[size_t(c.plane)].axis;
            for (int side = 0; side < 2; ++side) {
                const int        ci   = side == 0 ? c.cell_below : c.cell_above;
                const int        node = side == 0 ? c.node_below : c.node_above;
                const SplitCell &cell = plan.parts[size_t(ci)].cell;
                for (int k = 0; k < 2; ++k) {
                    const int b = k == 0 ? u_axis(a) : v_axis(a);
                    for (int end = 0; end < 2; ++end) {
                        const int m = end == 0 ? cell[b] - 1 : cell[b];
                        if (m < 0 || m >= plan.counts[b] - 1)
                            continue;
                        const int other = plan.plane_index(b, m);
                        if (other < 0)
                            continue;
                        const coord_t s  = Point::new_scale(np[b][size_t(m)], 0.).x();
                        auto          iv = side_intervals(c.shape, k, s);
                        if (!iv.empty())
                            along[{ { ci, c.plane, other } }].emplace_back(node, std::move(iv));
                    }
                }
            }
        }
        for (const auto &[key, mine] : along) {
            if (key[1] > key[2])
                continue;
            const auto it = along.find({ { key[0], key[2], key[1] } });
            if (it == along.end())
                continue;
            for (const auto &m0 : mine)
                for (const auto &m1 : it->second)
                    if (intervals_overlap(m0.second, m1.second))
                        uf.unite(m0.first, m1.first);
        }
    }

    // 4. The triangles: fragments of those that cross planes, and where they cross a plane, the cap.
    std::vector<int>                  home(T, -1);   // the one cell a triangle lies in, or -1
    std::unordered_map<uint64_t, int> frag_node;
    struct Frag
    {
        int                t;
        int                cell;
        std::vector<Vec3d> poly;
        int                node;
    };
    std::vector<Frag> frags;
    auto key_of  = [ncell](size_t t, int cell) { return uint64_t(t) * uint64_t(ncell) + uint64_t(cell); };
    auto node_of = [&](size_t t, int cell) -> int {
        if (cell < 0)
            return -1;
        if (home[t] >= 0)
            return home[t] == cell ? int(t) : -1;
        const auto it = frag_node.find(key_of(t, cell));
        return it == frag_node.end() ? -1 : it->second;
    };
    std::vector<Vec3d>  poly;
    std::vector<double> ts;
    for (size_t t = 0; t < T; ++t) {
        if ((t & 0xFFFF) == 0)
            cancel_point();
        const stl_triangle_vertex_indices &f = mesh.indices[t];
        const Vec3d v[3] = { vertex(f[0]), vertex(f[1]), vertex(f[2]) };
        std::array<int, 3> c0, c1;
        bool               single = true;
        for (int a = 0; a < 3; ++a) {
            c0[a]  = axis_cell(a, std::min({ v[0][a], v[1][a], v[2][a] }));
            c1[a]  = axis_cell(a, std::max({ v[0][a], v[1][a], v[2][a] }));
            single = single && c0[a] == c1[a];
        }
        if (single) {
            home[t] = plan.cell_index({ { c0[0], c0[1], c0[2] } });
            continue;
        }
        for (int k = c0[2]; k <= c1[2]; ++k)
            for (int j = c0[1]; j <= c1[1]; ++j)
                for (int i = c0[0]; i <= c1[0]; ++i) {
                    poly.assign(v, v + 3);
                    const int cc[3] = { i, j, k };
                    for (int a = 0; a < 3 && !poly.empty(); ++a) {
                        if (cc[a] > 0)
                            clip_polygon(poly, a, np[a][size_t(cc[a]) - 1], true);
                        if (cc[a] < plan.counts[a] - 1)
                            clip_polygon(poly, a, np[a][size_t(cc[a])], false);
                    }
                    if (poly.size() < 3)
                        continue;
                    Vec3d n = Vec3d::Zero();
                    for (size_t q = 1; q + 1 < poly.size(); ++q)
                        n += (poly[q] - poly[0]).cross(poly[q + 1] - poly[0]);
                    if (n.norm() < 1e-12)
                        continue;
                    const int ci   = plan.cell_index({ { i, j, k } });
                    const int node = uf.add();
                    frag_node[key_of(t, ci)] = node;
                    frags.push_back({ int(t), ci, poly, node });
                }
        for (int a = 0; a < 3; ++a)
            for (int k = c0[a]; k < c1[a]; ++k) {
                const double P = np[a][size_t(k)];
                Vec3d        pts[2];
                int          n = 0;
                for (int e = 0; e < 3; ++e) {
                    const Vec3d &p = v[e], &q = v[(e + 1) % 3];
                    const double dp = p[a] - P, dq = q[a] - P;
                    if ((dp < 0.) != (dq < 0.) && n < 2)
                        pts[n++] = p + (q - p) * (dp / (dp - dq));
                }
                const int pi = plan.plane_index(a, k);
                if (n != 2 || pi < 0)
                    continue;
                const int    ua = u_axis(a), va = v_axis(a);
                const double len = (pts[1] - pts[0]).norm();
                ts.assign({ 0., 1. });
                for (int b : { ua, va }) {
                    const double x0 = pts[0][b], x1 = pts[1][b];
                    if (x0 == x1)
                        continue;
                    for (double P2 : np[b]) {
                        const double s = (P2 - x0) / (x1 - x0);
                        if (s > 0. && s < 1.)
                            ts.push_back(s);
                    }
                }
                std::sort(ts.begin(), ts.end());
                for (size_t q = 0; q + 1 < ts.size(); ++q) {
                    if ((ts[q + 1] - ts[q]) * len < 1e-7)
                        continue;
                    const Vec3d mid = pts[0] + (pts[1] - pts[0]) * (0.5 * (ts[q] + ts[q + 1]));
                    const int   cu = axis_cell(ua, mid[ua]), cv = axis_cell(va, mid[va]);
                    const int   cap = cap_at(pi, cu, cv, Vec2d(mid[ua], mid[va]));
                    if (cap < 0)
                        continue;
                    SplitCell below{ { 0, 0, 0 } };
                    below[a]        = k;
                    below[ua]       = cu;
                    below[va]       = cv;
                    SplitCell above = below;
                    above[a]        = k + 1;
                    uf.unite(caps[size_t(cap)].node_below, node_of(t, plan.cell_index(below)));
                    uf.unite(caps[size_t(cap)].node_above, node_of(t, plan.cell_index(above)));
                }
            }
    }

    // 5. Fragments of neighbouring triangles in one cell, joined where their shared edge runs through it.
    for (size_t t = 0; t < T; ++t) {
        if ((t & 0xFFFF) == 0)
            cancel_point();
        for (int e = 0; e < 3; ++e) {
            const int n = neighbors[t][e];
            if (n < 0 || size_t(n) == t || size_t(n) >= T)
                continue;
            if (home[t] >= 0 && home[size_t(n)] >= 0) {
                if (home[t] == home[size_t(n)])
                    uf.unite(int(t), n);
                continue;
            }
            const Vec2i32 ev = its_triangle_edge(mesh.indices[t], e);
            const Vec3d   p0 = vertex(ev[0]), p1 = vertex(ev[1]);
            const int     k0 = cell_of(p0), k1 = cell_of(p1);
            if (k0 == k1) {
                uf.unite(node_of(t, k0), node_of(size_t(n), k0));
                continue;
            }
            ts.assign({ 0., 1. });
            for (int a = 0; a < 3; ++a) {
                if (p0[a] == p1[a])
                    continue;
                for (double P : np[a]) {
                    const double s = (P - p0[a]) / (p1[a] - p0[a]);
                    if (s > 0. && s < 1.)
                        ts.push_back(s);
                }
            }
            std::sort(ts.begin(), ts.end());
            for (size_t q = 0; q + 1 < ts.size(); ++q) {
                if (ts[q + 1] - ts[q] < 1e-12)
                    continue;
                const int c = cell_of(p0 + (p1 - p0) * (0.5 * (ts[q] + ts[q + 1])));
                uf.unite(node_of(t, c), node_of(size_t(n), c));
            }
        }
    }

    // 6. The components: box, volume (divergence theorem, as the cells' volumes) and shells.
    std::vector<int> shell(T, -1);
    {
        int              next = 0;
        std::vector<int> stack;
        for (size_t t = 0; t < T; ++t) {
            if (shell[t] >= 0)
                continue;
            shell[t] = next;
            stack.assign(1, int(t));
            while (!stack.empty()) {
                const int x = stack.back();
                stack.pop_back();
                for (int e = 0; e < 3; ++e) {
                    const int n = neighbors[size_t(x)][e];
                    if (n >= 0 && size_t(n) < T && shell[size_t(n)] < 0) {
                        shell[size_t(n)] = next;
                        stack.push_back(n);
                    }
                }
            }
            ++next;
        }
    }
    std::vector<int>       comp_of(uf.parent.size(), -1);
    std::vector<PieceComp> comps;
    auto comp_index = [&](int node) {
        const int r = uf.find(node);
        if (comp_of[size_t(r)] < 0) {
            comp_of[size_t(r)] = int(comps.size());
            comps.emplace_back();
        }
        return comp_of[size_t(r)];
    };
    auto tet = [&origin](const Vec3d &p0, const Vec3d &p1, const Vec3d &p2) { return (p0 - origin).dot((p1 - origin).cross(p2 - origin)) / 6.; };
    auto add_shell = [](PieceComp &c, int s) {
        if (std::find(c.shells.begin(), c.shells.end(), s) == c.shells.end())
            c.shells.push_back(s);
    };
    for (size_t t = 0; t < T; ++t) {
        if (home[t] < 0)
            continue;
        PieceComp &c = comps[size_t(comp_index(int(t)))];
        const stl_triangle_vertex_indices &f = mesh.indices[t];
        const Vec3d v[3] = { vertex(f[0]), vertex(f[1]), vertex(f[2]) };
        c.cell = home[t];
        for (const Vec3d &p : v)
            c.bbox.merge(p);
        c.volume += tet(v[0], v[1], v[2]);
        c.surface = true;
        add_shell(c, shell[t]);
    }
    for (const Frag &fr : frags) {
        PieceComp &c = comps[size_t(comp_index(fr.node))];
        c.cell       = fr.cell;
        for (const Vec3d &p : fr.poly)
            c.bbox.merge(p);
        for (size_t q = 1; q + 1 < fr.poly.size(); ++q)
            c.volume += tet(fr.poly[0], fr.poly[q], fr.poly[q + 1]);
        c.surface = true;
        add_shell(c, shell[size_t(fr.t)]);
    }
    for (const CapIsland &cap : caps) {
        const SplitPlane &pl = plan.planes[size_t(cap.plane)];
        const int         a = pl.axis, ua = u_axis(a), va = v_axis(a);
        const double      P = np[a][size_t(pl.index)];
        const BoundingBox bb = get_extents(cap.shape);
        Vec3d             p0, p1;
        p0[a]  = P;
        p1[a]  = P;
        p0[ua] = unscale<double>(bb.min.x());
        p1[ua] = unscale<double>(bb.max.x());
        p0[va] = unscale<double>(bb.min.y());
        p1[va] = unscale<double>(bb.max.y());
        const double vol = (P - origin[a]) * cap.area / 3.;
        for (int side = 0; side < 2; ++side) {
            PieceComp &c = comps[size_t(comp_index(side == 0 ? cap.node_below : cap.node_above))];
            c.cell       = side == 0 ? cap.cell_below : cap.cell_above;
            c.bbox.merge(p0);
            c.bbox.merge(p1);
            c.volume += side == 0 ? vol : -vol;
        }
    }

    // 7. Joins the topology does not see: other shells overlapping, caps on their own, voids.
    SplitUnionFind cu;
    cu.init(comps.size());
    std::vector<std::vector<int>> by_cell(static_cast<size_t>(ncell));
    for (size_t i = 0; i < comps.size(); ++i)
        if (comps[i].cell >= 0 && comps[i].cell < ncell)
            by_cell[size_t(comps[i].cell)].push_back(int(i));
    for (const std::vector<int> &list : by_cell) {
        if (list.size() < 2)
            continue;
        std::vector<int> surf, bare;
        for (int i : list)
            (comps[size_t(i)].surface ? surf : bare).push_back(i);
        // Pieces of different shells whose boxes overlap: overlapping bodies stay together.
        if (surf.size() <= 2000)
            for (size_t x = 0; x < surf.size(); ++x)
                for (size_t y = x + 1; y < surf.size(); ++y) {
                    const PieceComp &l = comps[size_t(surf[x])], &r = comps[size_t(surf[y])];
                    bool other = false;
                    for (int sl : l.shells)
                        for (int sr : r.shells)
                            other = other || sl != sr;
                    if (other && boxes_touch(l.bbox, r.bbox, 1e-6))
                        cu.unite(surf[x], surf[y]);
                }
        // A cap that joined no surface: with the piece whose box holds it (a deep inside cell, all caps,
        // is one piece).
        for (int b : bare) {
            if (surf.empty()) {
                cu.unite(b, bare.front());
                continue;
            }
            const Vec3d c    = comps[size_t(b)].bbox.center();
            int         best = -1;
            double      best_d = std::numeric_limits<double>::max();
            for (int s : surf) {
                const BoundingBoxf3 &bb = comps[size_t(s)].bbox;
                const double d = box_contains(bb, c, 1e-3) ? -1. : (bb.center() - c).squaredNorm();
                if (d < best_d) {
                    best_d = d;
                    best   = s;
                }
            }
            cu.unite(b, best);
        }
    }
    std::vector<int>       group_of(comps.size(), -1);
    std::vector<PieceComp> groups;
    for (size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].cell < 0)
            continue;
        const int r = cu.find(int(i));
        if (group_of[size_t(r)] < 0) {
            group_of[size_t(r)] = int(groups.size());
            groups.emplace_back();
            groups.back().cell = comps[i].cell;
        }
        group_of[i]  = group_of[size_t(r)];
        PieceComp &g = groups[size_t(group_of[i])];
        if (comps[i].bbox.defined)
            g.bbox.merge(comps[i].bbox);
        g.volume += comps[i].volume;
        g.surface = g.surface || comps[i].surface;
        for (int s : comps[i].shells)
            add_shell(g, s);
    }
    // A void (an inward-facing shell) goes with the piece whose box holds it; too little to be a piece is dropped.
    std::vector<int> final_of(groups.size(), -1);
    for (size_t g = 0; g < groups.size(); ++g)
        final_of[g] = groups[g].volume > 1e-6 ? int(g) : -1;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (groups[g].volume >= -1e-6)
            continue;
        int    best = -1;
        double best_v = std::numeric_limits<double>::max();
        for (size_t h = 0; h < groups.size(); ++h) {
            if (final_of[h] != int(h) || groups[h].cell != groups[g].cell || !boxes_touch(groups[h].bbox, groups[g].bbox, 1e-3))
                continue;
            if (box_contains(groups[h].bbox, groups[g].bbox.min, 1e-3) && box_contains(groups[h].bbox, groups[g].bbox.max, 1e-3) &&
                groups[h].volume < best_v) {
                best_v = groups[h].volume;
                best   = int(h);
            }
        }
        if (best >= 0) {
            groups[size_t(best)].volume += groups[g].volume;
            final_of[g] = best;
        }
    }

    // 8. The pieces of every section, largest first.
    std::vector<int> group_piece(groups.size(), -1);
    for (int ci = 0; ci < ncell; ++ci) {
        if (plan.parts[size_t(ci)].empty)
            continue;
        std::vector<int> mine;
        for (size_t g = 0; g < groups.size(); ++g)
            if (final_of[g] == int(g) && groups[g].cell == ci)
                mine.push_back(int(g));
        std::stable_sort(mine.begin(), mine.end(), [&groups](int l, int r) { return groups[size_t(l)].volume > groups[size_t(r)].volume; });
        if (mine.empty()) {
            // Nothing found (a model that is not closed, say): the section is one piece, as before.
            make_piece(ci, plan.parts[size_t(ci)].bbox, plan.parts[size_t(ci)].volume);
            continue;
        }
        for (int g : mine) {
            group_piece[size_t(g)] = int(plan.pieces.size());
            make_piece(ci, groups[size_t(g)].bbox, groups[size_t(g)].volume);
        }
    }
    auto piece_of_node = [&](int node) -> int {
        if (node < 0)
            return -1;
        const int c = comp_of[size_t(uf.find(node))];
        if (c < 0 || group_of[size_t(c)] < 0)
            return -1;
        const int f = final_of[size_t(group_of[size_t(c)])];
        return f < 0 ? -1 : group_piece[size_t(f)];
    };

    // 9. The piece of every vertex.
    for (size_t t = 0; t < T; ++t) {
        const stl_triangle_vertex_indices &f = mesh.indices[t];
        for (int k = 0; k < 3; ++k) {
            const size_t vi = size_t(f[k]);
            if (plan.vertex_piece[vi] >= 0)
                continue;
            plan.vertex_piece[vi] = piece_of_node(node_of(t, cell_of(vertex(f[k]))));
        }
    }

    // 10. The mating pairs: the caps of every listed face, grouped by the two pieces either side.
    std::vector<int> order(caps.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = int(i);
    std::stable_sort(order.begin(), order.end(), [&caps](int l, int r) {
        const CapIsland &a = caps[size_t(l)], &b = caps[size_t(r)];
        return a.face != b.face ? a.face < b.face : a.area > b.area;
    });
    std::map<std::array<int, 3>, int> mate_of;
    for (int ci : order) {
        const CapIsland &cap = caps[size_t(ci)];
        if (cap.face < 0 || cap.area < MIN_FACE_AREA)
            continue;
        const SplitPlane &pl = plan.planes[size_t(cap.plane)];
        const int         a = pl.axis, ua = u_axis(a), va = v_axis(a);
        int pb = piece_of_node(cap.node_below), pa = piece_of_node(cap.node_above);
        if (pb < 0 || pa < 0) {
            // A cap the topology did not place: by the pieces' boxes.
            const Point c2 = cap.shape.contour.centroid();
            Vec3d       c3;
            c3[a]  = np[a][size_t(pl.index)];
            c3[ua] = unscale<double>(c2.x());
            c3[va] = unscale<double>(c2.y());
            if (pb < 0)
                pb = piece_at(plan, section_pieces[size_t(cap.cell_below)], c3);
            if (pa < 0)
                pa = piece_at(plan, section_pieces[size_t(cap.cell_above)], c3);
        }
        if (pb < 0 || pa < 0)
            continue;
        const std::array<int, 3> key{ { cap.face, pb, pa } };
        auto                     it = mate_of.find(key);
        if (it == mate_of.end()) {
            SplitMate m;
            m.id          = int(plan.mates.size());
            m.face        = cap.face;
            m.piece_below = pb;
            m.piece_above = pa;
            it            = mate_of.emplace(key, m.id).first;
            plan.mates.push_back(std::move(m));
        }
        SplitMate &m = plan.mates[size_t(it->second)];
        m.patches.push_back(cap.shape);
        m.area += cap.area;
    }
    // Codes: the face's own, with a letter when it joins more than one pair.
    {
        std::vector<std::vector<int>> per_face = mates_by_face(plan);
        for (size_t f = 0; f < per_face.size(); ++f)
            for (size_t k = 0; k < per_face[f].size(); ++k) {
                SplitMate &m = plan.mates[size_t(per_face[f][k])];
                m.code       = plan.faces[f].mate_code;
                if (per_face[f].size() > 1)
                    m.code += k < 26 ? std::string(1, char('A' + k)) : std::to_string(k + 1);
            }
    }
    (void) options;
}

// After find_pieces(): names, sizes against the usable volume, the small-piece rule, the discard, the
// contact areas, the pieces' own warnings, and the pairs of discarded pieces taken out.
static void finish_pieces(SplitPlan &plan, const indexed_triangle_set &mesh, const SplitOptions &options,
                          const std::array<std::vector<double>, 3> &positions, const std::function<void()> &cancel_point)
{
    const Vec3d lo = plan.model_bbox.min, hi = plan.model_bbox.max;
    auto cell_lo = [&](int a, int c) { return c == 0 ? lo[a] : positions[a][size_t(c) - 1]; };
    auto cell_hi = [&](int a, int c) { return c == plan.counts[a] - 1 ? hi[a] : positions[a][size_t(c)]; };
    plan.small_piece_volume = options.small_piece_volume;

    std::vector<std::vector<int>> per_section(plan.parts.size());
    for (size_t i = 0; i < plan.pieces.size(); ++i)
        per_section[size_t(plan.pieces[i].section)].push_back(int(i));

    // Contact areas: a section of one piece stands on what the section stands on; pieces of a section of
    // several get the section's footprint split by their boxes.
    std::vector<float> heights;
    auto contact_z = [](const SplitPiece &p) { return p.bbox.min.z() + std::min(SPLIT_CONTACT_HEIGHT, 0.5 * p.size().z()); };
    for (const SplitPiece &p : plan.pieces)
        if (per_section[size_t(p.section)].size() > 1 && p.bbox.defined)
            heights.push_back(float(contact_z(p)));
    AxisSections bottoms;
    if (!heights.empty() && !plan.planes.empty())
        bottoms = slice_axis(mesh, 2, std::move(heights), cancel_point);

    for (size_t s = 0; s < per_section.size(); ++s) {
        const std::vector<int> &list = per_section[s];
        const SplitPart        &part = plan.parts[s];
        for (size_t k = 0; k < list.size(); ++k) {
            SplitPiece &p = plan.pieces[size_t(list[k])];
            p.index       = int(k);
            const std::string letter = piece_letter(int(k));
            p.id          = part.name.empty() ? letter : part.name + "_" + letter;
            p.tiny        = !plan.planes.empty() && p.volume < options.small_piece_volume;
            p.discarded   = p.tiny && options.discard_small;
            p.name        = part.name;
            if (list.size() > 1)
                p.name += (p.name.empty() ? "" : "_") + letter;
            if (p.tiny)
                p.name += "_tiny";
            const Vec3d sz = p.size();
            p.fits = sz.x() <= plan.usable.size.x() + POS_EPS && sz.y() <= plan.usable.size.y() + POS_EPS && sz.z() <= plan.usable.size.z() + POS_EPS;
            if (list.size() == 1 || bottoms.zs.empty()) {
                p.contact_area = part.contact_area;
                continue;
            }
            const SplitCell &cell = part.cell;
            const double     z    = contact_z(p);
            const ExPolygons isl  = intersection_ex(bottoms.slices[bottoms.index(z)],
                                                    Polygons{ scaled_rect(cell_lo(0, cell[0]), cell_lo(1, cell[1]), cell_hi(0, cell[0]), cell_hi(1, cell[1])) });
            for (const ExPolygon &ex : isl) {
                const Point c = ex.contour.centroid();
                if (piece_at(plan, list, Vec3d(unscale<double>(c.x()), unscale<double>(c.y()), z)) == list[k])
                    p.contact_area += ex.area() * SCALING_FACTOR * SCALING_FACTOR;
            }
        }
    }

    // P4: what each piece could stand on. A cut face is flat by construction and lies on the piece's
    // extreme along its axis (the piece is inside its cell), so laid on it the piece stands on all of
    // that face's material: the areas of its pairs on that side of the plane. The pose must fit.
    {
        std::vector<std::array<double, 6>> cut_area(plan.pieces.size(), std::array<double, 6>{ { 0., 0., 0., 0., 0., 0. } });
        for (const SplitMate &m : plan.mates) {
            if (m.face < 0 || size_t(m.face) >= plan.faces.size())
                continue;
            const int a = plan.planes[size_t(plan.faces[size_t(m.face)].plane)].axis;
            if (m.piece_below >= 0)
                cut_area[size_t(m.piece_below)][size_t(2 * a + 1)] += m.area;   // outward +axis
            if (m.piece_above >= 0)
                cut_area[size_t(m.piece_above)][size_t(2 * a)] += m.area;       // outward -axis
        }
        const Vec3d &U = plan.usable.size;
        for (size_t i = 0; i < plan.pieces.size(); ++i) {
            SplitPiece &p       = plan.pieces[i];
            p.best_contact_area = p.contact_area;
            const Vec3d sz      = p.size();
            for (int a = 0; a < 3; ++a) {
                // Laid on a face across `a`: `a` becomes the height, the other two the footprint.
                const double h = sz[a], f0 = sz[u_axis(a)], f1 = sz[v_axis(a)];
                const bool   fits = h <= U.z() + POS_EPS && ((f0 <= U.x() + POS_EPS && f1 <= U.y() + POS_EPS) ||
                                                           (f1 <= U.x() + POS_EPS && f0 <= U.y() + POS_EPS));
                if (!fits)
                    continue;
                p.best_contact_area = std::max({ p.best_contact_area, cut_area[i][size_t(2 * a)], cut_area[i][size_t(2 * a + 1)] });
            }
        }
    }

    // The pieces' warnings (P1's "does not fit", P3's part checks, now per piece).
    size_t discarded = 0;
    double discarded_v = 0.;
    for (size_t i = 0; i < plan.pieces.size(); ++i) {
        const SplitPiece &p = plan.pieces[i];
        auto warn = [&](SplitWarning::Kind kind, double value) {
            SplitWarning w{ kind, -1, p.section, value };
            w.piece = int(i);
            plan.warnings.push_back(w);
        };
        if (p.discarded) {
            ++discarded;
            discarded_v += p.volume;
            continue;
        }
        if (!p.fits)
            warn(SplitWarning::Kind::PartDoesNotFit, 0.);
        if (plan.planes.empty())
            continue;
        if (p.tiny)
            warn(SplitWarning::Kind::SmallPart, p.volume);
        else if (p.best_contact_area < SPLIT_MIN_CONTACT_AREA)
            // P4: only when neither its pose nor any cut face gives it a base (value: the best it has).
            warn(SplitWarning::Kind::NoFlatFace, p.best_contact_area);
    }
    if (discarded > 0) {
        SplitWarning w{ SplitWarning::Kind::DiscardedPieces, -1, -1, double(discarded) };
        w.value2 = discarded_v;
        plan.warnings.push_back(w);
    }

    // A pair with a discarded piece is no pair: no dowels, no labels.
    if (discarded > 0) {
        std::vector<SplitMate> kept;
        for (SplitMate &m : plan.mates)
            if (!plan.pieces[size_t(m.piece_below)].discarded && !plan.pieces[size_t(m.piece_above)].discarded) {
                m.id = int(kept.size());
                kept.push_back(std::move(m));
            }
        plan.mates = std::move(kept);
    }
    for (SplitPiece &p : plan.pieces)
        p.mates.clear();
    for (const SplitMate &m : plan.mates) {
        plan.pieces[size_t(m.piece_below)].mates.push_back(m.id);
        plan.pieces[size_t(m.piece_above)].mates.push_back(m.id);
    }
}

// ---- labels (P5, design 7.5) --------------------------------------------------------------------------

// Does segment (a, b) touch the box [lo, hi]? (Liang-Barsky.)
static bool segment_touches_box(const Vec2d &a, const Vec2d &b, const Vec2d &lo, const Vec2d &hi)
{
    double      t0 = 0., t1 = 1.;
    const Vec2d d  = b - a;
    for (int k = 0; k < 2; ++k) {
        const double p[2] = { -d[k], d[k] };
        const double q[2] = { a[k] - lo[k], hi[k] - a[k] };
        for (int s = 0; s < 2; ++s) {
            if (p[s] == 0.) {
                if (q[s] < 0.)
                    return false;
            } else {
                const double r = q[s] / p[s];
                if (p[s] < 0.)
                    t0 = std::max(t0, r);
                else
                    t1 = std::min(t1, r);
            }
        }
    }
    return t0 <= t1;
}

// Every mating pair gets one label on each of its two pieces, on the pair's largest patch: inset by the
// margin, clear of every dowel hole on the face, with material behind it on both sides. The text is as
// tall as asked if it fits, else smaller down to the smallest height, else the dot pattern, else none
// (each with a note). Both labels sit on the same spot; each is laid out seen from outside its piece.
static void place_labels(SplitPlan &plan, const MaterialDepth *depth_along, const std::function<void()> &cancel_point)
{
    const SplitLabelOptions &opt = plan.label_options;
    if (opt.depth <= 0. || opt.height <= 0.)
        return;
    for (SplitMate &mate : plan.mates) {
        cancel_point();
        if (mate.patches.empty())
            continue;
        const SplitFace  &face = plan.faces[size_t(mate.face)];
        const SplitPlane &pl   = plan.planes[size_t(face.plane)];
        const int         a = pl.axis, ua = u_axis(a), va = v_axis(a);
        auto warn = [&](SplitWarning::Kind kind, double value) {
            SplitWarning w{ kind, face.plane, -1, value, face.id };
            w.mate = mate.id;
            plan.warnings.push_back(w);
        };

        // Where it may go.
        ExPolygons region = offset_ex(mate.patches.front(), -float(scale_(opt.margin)));
        Polygons   discs;
        static constexpr int DISC = 32;
        for (int si : face.sites) {
            const SplitSite &s = plan.sites[size_t(si)];
            // Round the hole with its clearance; the polygon goes round the circle, not inside it.
            const double r = (s.footprint_radius + opt.dowel_clearance) / std::cos(PI / DISC);
            Polygon      c;
            for (int k = 0; k < DISC; ++k) {
                const double t = 2. * PI * double(k) / DISC;
                c.points.push_back(Point::new_scale(s.pos[ua] + r * std::cos(t), s.pos[va] + r * std::sin(t)));
            }
            discs.push_back(std::move(c));
        }
        if (!discs.empty() && !region.empty())
            region = diff_ex(region, discs);
        if (region.empty()) {
            warn(SplitWarning::Kind::NoLabel, SPLIT_NO_LABEL_TOO_SMALL);
            continue;
        }

        // Candidate centres, farthest from the region's outline first.
        std::vector<Line> lines;
        for (const ExPolygon &ex : region)
            append(lines, ex.lines());
        const AABBTreeLines::LinesDistancer<Line> outline(std::move(lines));
        struct Cand { Vec2d uv; double clr; };
        std::vector<Cand> cands;
        {
            const double      area_mm2 = expolygons_area_mm2(region);
            const double      step     = std::max(0.25, std::sqrt(area_mm2 / 3000.));
            const BoundingBox bb       = get_extents(region);
            const Vec2d       l = unscale(bb.min), h = unscale(bb.max);
            for (double v = l.y() + 0.5 * step; v < h.y(); v += step)
                for (double u = l.x() + 0.5 * step; u < h.x(); u += step)
                    if (section_contains(region, Vec2d(u, v)))
                        cands.push_back({ Vec2d(u, v), outline.distance_from_lines<false>(Point::new_scale(u, v)) * SCALING_FACTOR });
            const Vec2d mid = 0.5 * (l + h);
            std::stable_sort(cands.begin(), cands.end(), [&mid](const Cand &x, const Cand &y) {
                if (std::abs(x.clr - y.clr) > 1e-3)
                    return x.clr > y.clr;
                return (x.uv - mid).squaredNorm() < (y.uv - mid).squaredNorm();
            });
        }

        // Text reads along `right`; `up` is +Z on X / Y faces and +Y on Z faces.
        const int  up_axis = a == 2 ? 1 : 2;
        const bool up_is_v = va == up_axis;
        Vec3d      n_axis  = Vec3d::Zero();
        n_axis[a]          = 1.;
        const double need  = opt.depth + 0.4;
        auto at = [&](const Vec2d &uv) {
            Vec3d p = Vec3d::Zero();
            p[a]    = pl.pos;
            p[ua]   = uv.x();
            p[va]   = uv.y();
            return p;
        };
        auto fits_at = [&](const Cand &c, double w, double h) {
            const double eu = up_is_v ? w : h, ev = up_is_v ? h : w;
            const double half_diag = 0.5 * std::hypot(eu, ev);
            if (c.clr < 0.5 * std::min(eu, ev))
                return false;
            const Vec2d blo(c.uv.x() - 0.5 * eu, c.uv.y() - 0.5 * ev), bhi(c.uv.x() + 0.5 * eu, c.uv.y() + 0.5 * ev);
            if (c.clr < half_diag)
                for (size_t li : outline.all_lines_in_radius(Point::new_scale(c.uv.x(), c.uv.y()), scale_(half_diag))) {
                    const Line &l = outline.get_line(li);
                    if (segment_touches_box(unscale(l.a), unscale(l.b), blo, bhi))
                        return false;
                }
            if (depth_along != nullptr) {
                const Vec2d pts[5] = { c.uv, blo, bhi, Vec2d(blo.x(), bhi.y()), Vec2d(bhi.x(), blo.y()) };
                for (const Vec2d &q : pts) {
                    const Vec3d p = at(q);
                    if ((*depth_along)(p, n_axis) < need || (*depth_along)(p, -n_axis) < need)
                        return false;
                }
            }
            return true;
        };
        static constexpr size_t MAX_TRIES = 400;
        auto find_spot = [&](double w, double h, Vec2d &out) {
            size_t tries = 0;
            for (const Cand &c : cands) {
                if (c.clr < 0.5 * std::min(w, h) || ++tries > MAX_TRIES)
                    break;
                if (fits_at(c, w, h)) {
                    out = c.uv;
                    return true;
                }
            }
            return false;
        };

        SplitLabel  lab;
        Vec2d       spot = Vec2d::Zero();
        bool        found = false;
        lab.depth = opt.depth;
        lab.text  = opt.style == SplitLabelStyle::Number ? std::to_string(mate.id + 1) : mate.code;
        if (opt.style != SplitLabelStyle::Pattern)
            for (double h = opt.height; h >= std::min(opt.min_height, opt.height) - 1e-9 && !found; h -= 0.5) {
                const double stroke = std::max(opt.min_stroke, 0.14 * h);
                double       w      = 0.;
                if (split_label_text_shapes(lab.text, h, stroke, &w).empty())
                    break;
                if (find_spot(w, h, spot)) {
                    found      = true;
                    lab.height = h;
                    lab.width  = w;
                    lab.stroke = stroke;
                }
            }
        if (!found) {
            // The dot pattern: as big as the text would be when it is the chosen style, else the smallest.
            std::vector<double> strokes{ opt.min_stroke };
            if (opt.style == SplitLabelStyle::Pattern && opt.height / 5. > opt.min_stroke)
                strokes.insert(strokes.begin(), opt.height / 5.);
            for (double stroke : strokes) {
                double size = 0.;
                split_label_pattern_shapes(mate.id + 1, stroke, &size);
                if (size > 0. && find_spot(size, size, spot)) {
                    found       = true;
                    lab.pattern = true;
                    lab.number  = mate.id + 1;
                    lab.height  = size;
                    lab.width   = size;
                    lab.stroke  = stroke;
                    break;
                }
            }
        }
        if (!found) {
            warn(SplitWarning::Kind::NoLabel, SPLIT_NO_LABEL_TOO_SMALL);
            continue;
        }
        if (!lab.pattern && lab.height < opt.height - 1e-6)
            warn(SplitWarning::Kind::LabelSmaller, lab.height);
        if (lab.pattern && opt.style != SplitLabelStyle::Pattern)
            warn(SplitWarning::Kind::LabelPattern, 0.);

        Vec3d up = Vec3d::Zero();
        up[up_axis] = 1.;
        for (int side = 0; side < 2; ++side) {
            SplitLabel l = lab;
            l.mate       = mate.id;
            l.below      = side == 0;
            l.piece      = side == 0 ? mate.piece_below : mate.piece_above;
            l.center     = at(spot);
            l.normal     = side == 0 ? n_axis : Vec3d(-n_axis);
            l.up         = up;
            l.right      = up.cross(l.normal);
            const int li = int(plan.labels.size());
            mate.labels.push_back(li);
            plan.pieces[size_t(l.piece)].labels.push_back(li);
            plan.labels.push_back(std::move(l));
        }
    }
}

} // namespace split_to_volume_detail

SplitPlan split_plan_grid(const indexed_triangle_set &mesh, const SplitUsableVolume &usable, const SplitOptions &options,
                          std::function<void()> throw_on_cancel, const std::vector<Vec3i32> *face_neighbors)
{
    using namespace split_to_volume_detail;
    auto cancel_point = [&throw_on_cancel]() {
        if (throw_on_cancel)
            throw_on_cancel();
    };

    SplitPlan plan;
    plan.usable           = usable;
    plan.lay_flat         = options.lay_flat;
    plan.instance_dowels  = options.instance_dowels;
    plan.loose_own_plates = options.loose_own_plates;
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

    // 2b. P3: move the free planes to better places to cut (design 3.5). Order along each axis is kept.
    if (options.adjust_planes) {
        adjust_planes(plan, mesh, options, cancel_point);
        for (int a = 0; a < 3; ++a)
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

    // P3: each cell's material volume by the divergence theorem: the signed tetrahedra of the surface
    // clipped to the cell (from the model's centre), plus the caps on the cell's planes (step 5).
    std::vector<double> volumes(plan.parts.size(), 0.);
    const Vec3d         origin = plan.model_bbox.center();
    auto tet = [&origin](const Vec3d &p0, const Vec3d &p1, const Vec3d &p2) {
        return (p0 - origin).dot((p1 - origin).cross(p2 - origin)) / 6.;
    };

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
            const size_t   ci = size_t(plan.cell_index({ { c0[0], c0[1], c0[2] } }));
            BoundingBoxf3 &bb = boxes[ci];
            for (const Vec3d &p : v)
                bb.merge(p);
            volumes[ci] += tet(v[0], v[1], v[2]);
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
                    const size_t   ci = size_t(plan.cell_index({ { i, j, k } }));
                    BoundingBoxf3 &bb = boxes[ci];
                    for (const Vec3d &p : poly)
                        bb.merge(p);
                    for (size_t q = 1; q + 1 < poly.size(); ++q)
                        volumes[ci] += tet(poly[0], poly[q], poly[q + 1]);
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
                // P5: "does not fit" is warned about per piece (finish_pieces()).
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
                if (plan.parts[size_t(pb)].empty && plan.parts[size_t(pa)].empty)
                    continue;
                const Polygon rect = scaled_rect(cell_lo(ua, below[ua]), cell_lo(va, below[va]), cell_hi(ua, below[ua]), cell_hi(va, below[va]));
                const double  area = expolygons_area_mm2(intersection_ex(sections[pi], Polygons{ rect }));
                // P3: the cap closes both cells' volumes (outward normal +axis below the plane, -axis above).
                const double cap = (pl.pos - origin[a]) * area / 3.;
                volumes[size_t(pb)] += cap;
                volumes[size_t(pa)] -= cap;
                if (plan.parts[size_t(pb)].empty || plan.parts[size_t(pa)].empty)
                    continue;
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

    // 5b. P3 part checks (design 3.7): volume, and the area each part stands on when dropped to the bed
    //     as it is (its section a little above its lowest point, inside its cell). Only for a split.
    if (!plan.planes.empty()) {
        std::vector<float> heights;
        auto contact_z = [](const SplitPart &p) { return p.bbox.min.z() + std::min(SPLIT_CONTACT_HEIGHT, 0.5 * p.size().z()); };
        for (SplitPart &p : plan.parts)
            if (!p.empty)
                heights.push_back(float(contact_z(p)));
        const AxisSections bottoms = slice_axis(mesh, 2, std::move(heights), cancel_point);
        for (size_t ci = 0; ci < plan.parts.size(); ++ci) {
            SplitPart &p = plan.parts[ci];
            if (p.empty)
                continue;
            p.volume              = std::max(0., volumes[ci]);
            const SplitCell &cell = p.cell;
            const Polygon    rect = scaled_rect(cell_lo(0, cell[0]), cell_lo(1, cell[1]), cell_hi(0, cell[0]), cell_hi(1, cell[1]));
            if (!bottoms.zs.empty())
                p.contact_area = expolygons_area_mm2(intersection_ex(bottoms.slices[bottoms.index(contact_z(p))], Polygons{ rect }));
            // P5: the small-part and flat-face warnings are given per piece (finish_pieces()).
        }
    }

    // 5c. P5: the pieces of every section, the mating pairs, and the pieces' checks.
    {
        std::vector<Vec3i32> own_neighbors;
        if (face_neighbors == nullptr || face_neighbors->size() != mesh.indices.size()) {
            own_neighbors  = plan.planes.empty() ? std::vector<Vec3i32>() : its_face_neighbors(mesh);
            face_neighbors = &own_neighbors;
        }
        find_pieces(plan, mesh, *face_neighbors, sections, options, cancel_point);
        finish_pieces(plan, mesh, options, positions, cancel_point);
    }

    // 6. Dowel sites (P2, design 3.6), per mating pair of each face (P5).
    plan.dowel_options = options.dowels;
    plan.label_options = options.labels;
    plan.manual_sites  = options.manual_sites;
    const bool want_dowels = options.dowels.enabled && !plan.mates.empty();
    const bool want_labels = options.labels.enabled && !plan.mates.empty();
    std::unique_ptr<AABBTreeIndirect::Tree3f> tree;
    std::unique_ptr<MaterialDepth>            depth;
    if (want_dowels || want_labels) {
        tree  = std::make_unique<AABBTreeIndirect::Tree3f>(AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.vertices, mesh.indices));
        depth = std::make_unique<MaterialDepth>(MaterialDepth{ mesh, *tree, {} });
    }
    if (want_dowels)
        place_dowels(plan, *depth, positions, cancel_point);
    // 6b. P5: matching labels on both faces of every pair, clear of the dowels.
    if (want_labels)
        place_labels(plan, depth.get(), cancel_point);

    // 7. Model-wide warnings.
    if (hollow_plane >= 0)
        plan.warnings.push_back({ SplitWarning::Kind::HollowModel, hollow_plane, -1, hollow_wall });
    if (const size_t open = its_num_open_edges(mesh); open > 0)
        plan.warnings.push_back({ SplitWarning::Kind::ModelNotWatertight, -1, -1, double(open) });

    plan.valid = plan.piece_count() > 0;
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

// ---- P4: lay a part on a cut face ---------------------------------------------------------------------

double split_overhang_area(const indexed_triangle_set &mesh, const Transform3d &rotation, double threshold_deg, double *contact)
{
    if (contact != nullptr)
        *contact = 0.;
    if (mesh.indices.empty())
        return 0.;
    const Matrix3d     R = rotation.linear();
    std::vector<Vec3d> pts;
    pts.reserve(mesh.vertices.size());
    double zmin = std::numeric_limits<double>::max();
    for (const Vec3f &v : mesh.vertices) {
        pts.push_back(R * v.cast<double>());
        zmin = std::min(zmin, pts.back().z());
    }
    // A down-facing facet leaning more than the threshold from vertical: its normal is within
    // (90 - threshold) degrees of straight down, nz < -sin(threshold).
    const double limit    = -std::sin(threshold_deg * PI / 180.);
    static constexpr double ON_BED = 1e-2;
    double overhang = 0., bed = 0.;
    for (const stl_triangle_vertex_indices &t : mesh.indices) {
        const Vec3d &p0 = pts[size_t(t[0])], &p1 = pts[size_t(t[1])], &p2 = pts[size_t(t[2])];
        const Vec3d  n   = (p1 - p0).cross(p2 - p0);
        const double len = n.norm();
        if (len <= 0.)
            continue;
        const double nz = n.z() / len;
        if (nz >= limit - 1e-9)
            continue;
        const double area = 0.5 * len;
        if (std::max({ p0.z(), p1.z(), p2.z() }) <= zmin + ON_BED) {
            if (nz < -0.999)
                bed += area;   // lies on the bed: what it stands on, not an overhang
            continue;
        }
        overhang += area;
    }
    if (contact != nullptr)
        *contact = bed;
    return overhang;
}

SplitLayFlat split_choose_lay_flat(const indexed_triangle_set &mesh, const std::vector<Vec3d> &cut_normals, const Vec3d &usable)
{
    using namespace split_to_volume_detail;
    SplitLayFlat out;
    if (mesh.indices.empty())
        return out;
    struct Cand
    {
        Transform3d R{ Transform3d::Identity() };
        Vec3d       normal{ Vec3d::Zero() };
        bool        current{ false };
        double      overhang{ 0. };
        double      contact{ 0. };
        bool        fits{ false };
    };
    const bool check_fit = usable.x() > 0. && usable.y() > 0. && usable.z() > 0.;
    auto evaluate = [&](Cand &c) {
        c.overhang = split_overhang_area(mesh, c.R, 45., &c.contact);
        BoundingBoxf3 bb;
        for (const Vec3f &v : mesh.vertices)
            bb.merge(c.R * v.cast<double>());
        const Vec3d s = bb.size();
        if (!check_fit) {
            c.fits = true;
            return;
        }
        if (s.z() > usable.z() + POS_EPS)
            return;
        if (s.x() <= usable.x() + POS_EPS && s.y() <= usable.y() + POS_EPS)
            c.fits = true;
        else if (s.y() <= usable.x() + POS_EPS && s.x() <= usable.y() + POS_EPS) {
            // A quarter turn about Z makes it fit.
            c.fits = true;
            c.R    = Transform3d(Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitZ())) * c.R;
        }
    };

    Cand current;
    current.current = true;
    evaluate(current);
    out.overhang = current.overhang;
    out.contact  = current.contact;
    out.fits     = current.fits;

    std::vector<Cand> cands;
    // The pose as it is competes only when it already stands on a flat base.
    if (current.contact >= SPLIT_MIN_CONTACT_AREA)
        cands.push_back(current);
    std::vector<Vec3d> seen;
    for (const Vec3d &n0 : cut_normals) {
        if (n0.norm() < 1e-9)
            continue;
        const Vec3d n = n0.normalized();
        if (std::any_of(seen.begin(), seen.end(), [&n](const Vec3d &s) { return (s - n).norm() < 1e-6; }))
            continue;
        seen.push_back(n);
        Cand c;
        c.normal = n;
        if ((n + Vec3d::UnitZ()).norm() < 1e-9) {
            // Already resting on this face: that is the pose as it is.
            if (current.contact >= SPLIT_MIN_CONTACT_AREA)
                continue;
            c.current = true;
        } else
            c.R = Transform3d(Eigen::Quaterniond().setFromTwoVectors(n, -Vec3d::UnitZ()));
        evaluate(c);
        cands.push_back(c);
    }

    double least = std::numeric_limits<double>::max();
    for (const Cand &c : cands)
        if (c.fits)
            least = std::min(least, c.overhang);
    if (least == std::numeric_limits<double>::max())
        return out;   // nothing fits better: kept as it is
    const double tie = std::max(1., 0.01 * least);
    const Cand  *best = nullptr;
    for (const Cand &c : cands) {
        if (!c.fits || c.overhang > least + tie)
            continue;
        if (best == nullptr || c.contact > best->contact + 1e-6 || (std::abs(c.contact - best->contact) <= 1e-6 && c.current && !best->current))
            best = &c;
    }
    out.overhang = best->overhang;
    out.contact  = best->contact;
    out.fits     = best->fits;
    out.normal   = best->normal;
    out.rotation = best->R;
    out.changed  = !best->R.isApprox(Transform3d::Identity(), 1e-12);
    return out;
}

// ---- P4: click-to-add / remove a dowel site -----------------------------------------------------------

SplitPickTarget split_pick(const SplitPlan &plan, const Vec3d &origin, const Vec3d &dir0)
{
    using namespace split_to_volume_detail;
    SplitPickTarget out;
    if (dir0.norm() < 1e-12)
        return out;
    const Vec3d dir    = dir0.normalized();
    double      best_t = std::numeric_limits<double>::max();
    auto        hit    = [&](int axis, double pos, double &t, Vec3d &p) {
        if (std::abs(dir[axis]) < 1e-9)
            return false;
        t = (pos - origin[axis]) / dir[axis];
        if (t < 0.)
            return false;
        p       = origin + t * dir;
        p[axis] = pos;
        return true;
    };
    for (size_t i = 0; i < plan.sites.size(); ++i) {
        const SplitSite &s = plan.sites[i];
        if (s.plane < 0 || size_t(s.plane) >= plan.planes.size())
            continue;
        const int a = plan.planes[size_t(s.plane)].axis;
        double    t;
        Vec3d     p;
        if (!hit(a, s.pos[a], t, p) || (p - s.pos).norm() > std::max(1., s.footprint_radius) || t >= best_t)
            continue;
        best_t    = t;
        out.kind  = SplitPickTarget::Kind::Site;
        out.site  = int(i);
        out.plane = s.plane;
        out.mate  = s.mate;
        out.pos   = p;
    }
    if (out.kind == SplitPickTarget::Kind::Site)
        return out;
    for (const SplitMate &m : plan.mates) {
        if (m.face < 0 || size_t(m.face) >= plan.faces.size())
            continue;
        const int         pidx = plan.faces[size_t(m.face)].plane;
        const SplitPlane &pl   = plan.planes[size_t(pidx)];
        double            t;
        Vec3d             p;
        if (!hit(pl.axis, pl.pos, t, p) || t >= best_t)
            continue;
        const Point pt(scaled<coord_t>(p[u_axis(pl.axis)]), scaled<coord_t>(p[v_axis(pl.axis)]));
        if (std::none_of(m.patches.begin(), m.patches.end(), [&pt](const ExPolygon &ex) { return ex.contains(pt); }))
            continue;
        best_t    = t;
        out.kind  = SplitPickTarget::Kind::Face;
        out.plane = pidx;
        out.mate  = m.id;
        out.pos   = p;
    }
    return out;
}

// ---- P4: arranging the result in groups -----------------------------------------------------------------

SplitArrangeStep split_arrange_step(const std::vector<SplitArrangeInstance> &instances, int group, const std::vector<bool> &plate_locked)
{
    SplitArrangeStep  out;
    std::vector<bool> holds(plate_locked.size(), false);
    for (size_t i = 0; i < instances.size(); ++i) {
        const SplitArrangeInstance &in = instances[i];
        if (in.group >= group) {
            // This group and the later ones belong to no plate while the step runs: this group is then
            // placed (it is printable), the later ones are skipped (they are held, below).
            if (in.plate >= 0)
                out.unregister.push_back(i);
            if (in.group > group && in.printable)
                out.hold.push_back(i);
        } else if (in.plate >= 0 && size_t(in.plate) < holds.size())
            holds[size_t(in.plate)] = true;   // stays registered: an earlier group, or not the chain's
    }
    if (group > 0)
        for (size_t p = 0; p < plate_locked.size(); ++p)
            if (holds[p] && !plate_locked[p])
                out.lock_plates.push_back(int(p));
    return out;
}

SplitArrangeReport split_arrange_report(const std::vector<SplitArrangeInstance> &instances, const std::vector<int> &placed_plate, int groups)
{
    SplitArrangeReport out;
    const size_t       n = size_t(std::max(0, groups));
    out.plates.assign(n, {});
    out.unplaced.assign(n, {});
    out.shares.assign(n, false);
    for (size_t i = 0; i < instances.size() && i < placed_plate.size(); ++i) {
        const int g = instances[i].group;
        if (g < 0 || size_t(g) >= n)
            continue;
        if (placed_plate[i] < 0)
            out.unplaced[size_t(g)].push_back(i);
        else
            out.plates[size_t(g)].push_back(placed_plate[i]);
    }
    for (std::vector<int> &p : out.plates) {
        std::sort(p.begin(), p.end());
        p.erase(std::unique(p.begin(), p.end()), p.end());
    }
    for (size_t g = 1; g < n; ++g)
        for (size_t e = 0; e < g && !out.shares[g]; ++e)
            for (int p : out.plates[g])
                if (std::binary_search(out.plates[e].begin(), out.plates[e].end(), p)) {
                    out.shares[g] = true;
                    break;
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

// P3: the area a mesh stands on when dropped to the bed as it is: its section SPLIT_CONTACT_HEIGHT
// above its lowest point (mm2).
static double contact_area_of(const indexed_triangle_set &its)
{
    if (its.vertices.empty())
        return 0.;
    float zmin = std::numeric_limits<float>::max(), zmax = std::numeric_limits<float>::lowest();
    for (const Vec3f &v : its.vertices) {
        zmin = std::min(zmin, v.z());
        zmax = std::max(zmax, v.z());
    }
    const float z = zmin + float(std::min(SPLIT_CONTACT_HEIGHT, 0.5 * double(zmax - zmin)));
    const std::vector<float>      zs{ z };
    MeshSlicingParamsEx           params;
    const std::vector<ExPolygons> s = slice_mesh_ex(its, zs, params);
    return s.empty() ? 0. : expolygons_area_mm2(s.front());
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

// ---- P5: a cut part split into its pieces ----

// The planned mesh's vertices, to find which piece a vertex of a cut part came from.
struct PlanVertexCoords
{
    const std::vector<Vec3f> *v{ nullptr };
    float operator()(size_t idx, size_t dim) const { return (*v)[idx][int(dim)]; }
};
using PlanVertexTree = KDTreeIndirect<3, float, PlanVertexCoords>;

// What Apply made of a leaf: an object in the scratch model, its section and its piece (-1: none matched).
struct MadePart
{
    ModelObject *object{ nullptr };
    int          section{ -1 };
    int          piece{ -1 };
};

// Splits `leaf` (a cut part of section `section`, whose plan has several pieces) into one object per
// piece. Each separate shell of each model part is matched to its piece by the plan's vertex_piece (its
// vertices that the cuts did not make are the planned mesh's own), else by the pieces' boxes. A model
// part keeps the shells of its piece (paint carried over); holes, labels and modifiers go with every
// piece they touch. Discarded pieces make nothing.
static void split_leaf(ModelObject &leaf, int section, const SplitPlan &plan, const std::vector<int> &pieces,
                       const std::vector<Vec3f> &plan_vertices, const PlanVertexTree &tree, Model &scratch, std::vector<MadePart> &made)
{
    struct Patch
    {
        size_t           volume{ 0 };
        std::vector<int> faces;
        int              piece{ -1 };
        BoundingBoxf3    bbox;
    };
    const Transform3d  inst = leaf.instances.front()->get_transformation().get_matrix();
    std::vector<Patch> patches;
    for (size_t vi = 0; vi < leaf.volumes.size(); ++vi) {
        const ModelVolume *v = leaf.volumes[vi];
        if (!v->is_model_part() || v->mesh().empty())
            continue;
        const indexed_triangle_set &its = v->mesh().its;
        const std::vector<Vec3i32>  nb  = its_face_neighbors(its);
        const Transform3d           W   = inst * v->get_matrix();
        std::vector<int>            pid(its.indices.size(), -1);
        std::vector<int>            stack;
        for (size_t f = 0; f < its.indices.size(); ++f) {
            if (pid[f] >= 0)
                continue;
            Patch p;
            p.volume = vi;
            pid[f]   = int(patches.size());
            stack.assign(1, int(f));
            while (!stack.empty()) {
                const int x = stack.back();
                stack.pop_back();
                p.faces.push_back(x);
                for (int e = 0; e < 3; ++e) {
                    const int n = nb.empty() ? -1 : nb[size_t(x)][e];
                    if (n >= 0 && pid[size_t(n)] < 0) {
                        pid[size_t(n)] = int(patches.size());
                        stack.push_back(n);
                    }
                }
            }
            std::sort(p.faces.begin(), p.faces.end());
            // Its box, and a vote of up to 64 of its vertices for the piece they came from.
            std::map<int, int> votes;
            const size_t       stride = std::max<size_t>(1, p.faces.size() * 3 / 64);
            for (size_t k = 0; k < p.faces.size() * 3; ++k) {
                const Vec3d w = W * its.vertices[size_t(its.indices[size_t(p.faces[k / 3])][int(k % 3)])].cast<double>();
                p.bbox.merge(w);
                if (k % stride != 0)
                    continue;
                const Vec3f  wf  = w.cast<float>();
                const size_t idx = find_closest_point(tree, wf);
                if (idx >= plan_vertices.size() || (plan_vertices[idx] - wf).squaredNorm() > 1e-6f)
                    continue;
                const int vp = plan.vertex_piece[idx];
                if (vp >= 0 && size_t(vp) < plan.pieces.size() && plan.pieces[size_t(vp)].section == section)
                    ++votes[vp];
            }
            int best = 0;
            for (const auto &[piece, count] : votes)
                if (count > best) {
                    best    = count;
                    p.piece = piece;
                }
            if (p.piece < 0)
                p.piece = piece_at(plan, pieces, p.bbox.center());
            patches.push_back(std::move(p));
        }
    }

    std::map<int, std::vector<size_t>> groups;
    for (size_t i = 0; i < patches.size(); ++i)
        groups[patches[i].piece].push_back(i);
    if (groups.size() <= 1) {
        const int piece = groups.empty() ? -1 : groups.begin()->first;
        if (piece < 0 || !plan.pieces[size_t(piece)].discarded)
            made.push_back({ &leaf, section, piece });
        return;
    }
    for (const auto &[piece, list] : groups) {
        if (piece >= 0 && plan.pieces[size_t(piece)].discarded)
            continue;
        BoundingBoxf3                 pbb;
        std::vector<std::vector<int>> keep(leaf.volumes.size());
        for (size_t pi : list) {
            pbb.merge(patches[pi].bbox);
            append(keep[patches[pi].volume], patches[pi].faces);
        }
        ModelObject *obj = scratch.add_object(leaf);
        copy_assembled_poses(leaf, *obj);
        std::vector<size_t> drop;
        for (size_t vi = 0; vi < leaf.volumes.size(); ++vi) {
            const ModelVolume *src = leaf.volumes[vi];
            ModelVolume       *dst = obj->volumes[vi];
            if (src->is_model_part() && !src->mesh().empty()) {
                std::vector<int> &faces = keep[vi];
                if (faces.empty()) {
                    drop.push_back(vi);
                    continue;
                }
                if (faces.size() == src->mesh().its.indices.size())
                    continue;
                std::sort(faces.begin(), faces.end());
                indexed_triangle_set sub;
                sub.vertices = src->mesh().its.vertices;
                sub.indices.reserve(faces.size());
                for (int f : faces)
                    sub.indices.push_back(src->mesh().its.indices[size_t(f)]);
                its_compactify_vertices(sub);
                // Keep paint v2: each facet of the patch IS facet faces[k] of the source (same corners,
                // same order), so the painting is copied facet by facet. An airbrushed part keeps the
                // remap, which re-bakes its colour field.
                const bool exact = !src->airbrush_field && PaintTransfer::has_painting(*src);
                const std::optional<TriangleSelector::SavedPainting> saved = exact ? std::nullopt : src->save_painting();
                dst->set_mesh(TriangleMesh(std::move(sub)));
                dst->restore_painting(saved, false);
                if (exact)
                    PaintTransfer::copy_painting(*src, *dst, faces);
                dst->calculate_convex_hull();
                dst->invalidate_convex_hull_2d();
            } else if (!boxes_touch(src->mesh().transformed_bounding_box(inst * src->get_matrix()), pbb, 0.5)) {
                // A hole, label or modifier of another piece.
                drop.push_back(vi);
            }
        }
        for (auto it = drop.rbegin(); it != drop.rend(); ++it)
            obj->delete_volume(*it);
        obj->invalidate_bounding_box();
        made.push_back({ obj, section, piece });
    }
}

// ---- P4: identical dowels as instances of one object ----

// Two loose dowels (or pins) are the same shape when their one solid has the same mesh, the same
// rotation and scale and the same connector type; only where they stand differs.
static bool same_dowel(const ModelObject &l, const ModelObject &r)
{
    if (l.volumes.size() != 1 || r.volumes.size() != 1)
        return false;
    const ModelVolume &a = *l.volumes.front(), &b = *r.volumes.front();
    if (a.type() != b.type() || a.cut_info.connector_type != b.cut_info.connector_type || a.cut_info.is_connector != b.cut_info.is_connector)
        return false;
    if (!a.get_matrix().linear().isApprox(b.get_matrix().linear(), 1e-9))
        return false;
    const indexed_triangle_set &x = a.mesh().its, &y = b.mesh().its;
    if (x.vertices.size() != y.vertices.size() || x.indices.size() != y.indices.size())
        return false;
    for (size_t i = 0; i < x.indices.size(); ++i)
        if (x.indices[i] != y.indices[i])
            return false;
    for (size_t i = 0; i < x.vertices.size(); ++i)
        if ((x.vertices[i] - y.vertices[i]).cwiseAbs().maxCoeff() > 1e-5f)
            return false;
    return true;
}

static std::string dowel_size_text(double diameter, double length)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%gx%g", std::round(diameter * 10.) / 10., std::round(length * 10.) / 10.);
    return buf;
}

// Folds every dowel into the first one of the same shape as an instance of it, standing (and
// assembled) exactly where it stood. The leaders are named by their size; the others are deleted from
// `model`. Returns the leaders in the order of the dowels they were first seen as.
static std::vector<ModelObject *> collapse_dowels(Model &model, std::vector<ModelObject *> dowels, const std::string &base, bool pin)
{
    std::vector<ModelObject *> leaders;
    for (ModelObject *d : dowels) {
        auto it = std::find_if(leaders.begin(), leaders.end(), [d](const ModelObject *l) { return same_dowel(*l, *d); });
        if (it == leaders.end()) {
            leaders.push_back(d);
            continue;
        }
        ModelObject *leader = *it;
        // The solid sits at its own offset in each object; that offset goes into the instance.
        const Vec3d       shift = d->volumes.front()->get_offset() - leader->volumes.front()->get_offset();
        const Transform3d local = Geometry::translation_transform(shift);
        for (const ModelInstance *inst : d->instances) {
            ModelInstance *ni = leader->add_instance(*inst);
            ni->set_transformation(Geometry::Transformation(inst->get_matrix() * local));
            if (inst->is_assemble_initialized())
                ni->set_assemble_transformation(Geometry::Transformation(inst->get_assemble_transformation().get_matrix() * local));
        }
        model.delete_object(d);
    }
    for (ModelObject *l : leaders) {
        if (l->instances.size() < 2)
            continue;
        // "<object>-Dowel 6x18" (diameter x length, mm, of the solid as it lies).
        const BoundingBoxf3 bb = l->volumes.front()->mesh().transformed_bounding_box(l->volumes.front()->get_matrix());
        const Vec3d         s  = bb.size();
        l->name = base + (pin ? "-Pin " : "-Dowel ") + dowel_size_text(std::max(s.x(), s.y()), s.z());
        l->invalidate_bounding_box();
    }
    return leaders;
}

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
        // P4: identical dowels as instances of one object.
        if (plan.instance_dowels && dowels.size() > 1)
            dowels = collapse_dowels(*result.model, std::move(dowels), base, pin);
        for (const ModelObject *d : dowels)
            result.dowel_count += d->instances.size();
    }

    // P5: every leaf as its pieces (a section of one piece is the leaf itself).
    std::vector<MadePart>             made;
    std::unique_ptr<PlanVertexTree>   vertex_tree;
    std::vector<Vec3f>                plan_vertices;
    for (const auto &[leaf, cell] : run.leaves) {
        const int              ci     = plan.cell_index(cell);
        const std::vector<int> pieces = plan.pieces_of(ci);
        if (pieces.size() <= 1) {
            const int p = pieces.empty() ? -1 : pieces.front();
            if (p < 0 || !plan.pieces[size_t(p)].discarded)
                made.push_back({ leaf, ci, p });
            continue;
        }
        if (!vertex_tree) {
            plan_vertices = split_object_mesh(object, instance).vertices;
            if (plan_vertices.size() != plan.vertex_piece.size())
                plan_vertices.clear();   // not the mesh the plan was made from: match by the boxes only
            vertex_tree = std::make_unique<PlanVertexTree>(PlanVertexCoords{ &plan_vertices }, plan_vertices.size());
        }
        if (canceled && canceled()) {
            result.canceled = true;
            return result;
        }
        split_leaf(*leaf, ci, plan, pieces, plan_vertices, *vertex_tree, scratch, made);
    }
    result.discarded        = plan.discarded_count();
    result.discarded_volume = plan.discarded_volume();

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
        const size_t n_made = made.size() + dowels.size();
        if (n_made > 1)
            linked.increase_check_sum(n_made - 1);
        linked.increase_connectors_cnt(result.dowel_count);
    }
    for (ModelObject *d : dowels) {
        if (link)
            d->cut_id.copy(linked);
        else
            d->cut_id.invalidate();
        result.dowels.push_back(d);
    }

    std::vector<ModelObject *> piece_object(plan.pieces.size(), nullptr);
    for (const MadePart &mp : made) {
        const ModelObject *leaf = mp.object;
        ModelObject       *part = result.model->add_object(*leaf);
        copy_assembled_poses(*leaf, *part);
        // D7, P5: the section's name, with the piece's letter and "_tiny" where the plan has them.
        const std::string suffix = mp.piece >= 0 ? plan.pieces[size_t(mp.piece)].name : plan.parts[size_t(mp.section)].name;
        if (!suffix.empty())
            part->name = object.name.empty() ? suffix : object.name + " " + suffix;
        if (link)
            part->cut_id.copy(linked);
        else
            part->cut_id.invalidate();

        // P5: this piece's labels, recessed as negative volumes.
        if (mp.piece >= 0 && piece_object[size_t(mp.piece)] == nullptr) {
            piece_object[size_t(mp.piece)] = part;
            const Transform3d to_object = part->instances.front()->get_transformation().get_matrix().inverse();
            for (int li : plan.pieces[size_t(mp.piece)].labels) {
                const SplitLabel    &label = plan.labels[size_t(li)];
                indexed_triangle_set its   = split_label_mesh(label);
                if (its.indices.empty())
                    continue;
                its_transform(its, to_object, true);
                ModelVolume *v = part->add_volume(TriangleMesh(std::move(its)), ModelVolumeType::NEGATIVE_VOLUME);
                v->name        = "Label " + label.text;
                ++result.labels;
            }
        }
        part->invalidate_bounding_box();

        const int pi = mp.section;
        result.parts.push_back(part);
        result.part_index.push_back(pi);
        result.piece_index.push_back(mp.piece);
        result.part_loose.push_back(plan.is_loose_piece(mp.piece));

        // P4: turned onto the cut face with the least overhang (that still fits), or kept as it is.
        bool laid = false;
        if (plan.lay_flat && mp.piece >= 0) {
            const SplitPiece  &pc   = plan.pieces[size_t(mp.piece)];
            const SplitCell   &cell = plan.parts[size_t(mp.section)].cell;
            std::vector<Vec3d> normals;
            for (int a = 0; a < 3; ++a) {
                Vec3d e = Vec3d::Zero();
                e[a]    = 1.;
                if (cell[a] > 0 && pc.bbox.defined && pc.bbox.min[a] <= run.positions[a][size_t(cell[a]) - 1] + 0.5)
                    normals.push_back(-e);
                if (cell[a] < plan.counts[a] - 1 && pc.bbox.defined && pc.bbox.max[a] >= run.positions[a][size_t(cell[a])] - 0.5)
                    normals.push_back(e);
            }
            indexed_triangle_set world;
            const Transform3d    inst = part->instances.front()->get_matrix();
            for (const ModelVolume *v : part->volumes)
                if (v->is_model_part() && !v->mesh().empty()) {
                    TriangleMesh m = v->mesh();
                    m.transform(inst * v->get_matrix(), true);
                    its_merge(world, m.its);
                }
            if (!normals.empty()) {
                const SplitLayFlat lf = split_choose_lay_flat(world, normals, plan.usable.valid ? plan.usable.size : Vec3d::Zero());
                if (lf.changed) {
                    part->instances.front()->set_transformation(Geometry::Transformation(lf.rotation * inst));
                    part->invalidate_bounding_box();
                    laid = true;
                }
            }
        }
        result.part_laid_flat.push_back(laid);
        // Labels on a face the part now stands on (recessed: a dent in its bottom).
        if (mp.piece >= 0) {
            const Matrix3d R = part->instances.front()->get_matrix().linear();
            for (int li : plan.pieces[size_t(mp.piece)].labels)
                if ((R * plan.labels[size_t(li)].normal).normalized().z() < -0.999)
                    ++result.labels_on_bed;
        }

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
        auto warn = [&](SplitWarning::Kind kind, double value) {
            SplitWarning w{ kind, -1, pi, value };
            w.piece = mp.piece;
            result.warnings.push_back(w);
        };
        // D15 now applies only to shells kept together on purpose (overlapping bodies, P5).
        if (const size_t pieces = its_number_of_patches(merged); pieces > 1)
            warn(SplitWarning::Kind::LoosePieces, double(pieces));
        if (open > 0)
            warn(SplitWarning::Kind::OpenEdges, double(open));
        // P3 (design 3.7): a very small part, or one with nothing flat to stand on (D11 keeps its pose).
        const double volume = std::abs(double(its_volume(merged)));
        if (volume < plan.small_piece_volume)
            warn(SplitWarning::Kind::SmallPart, volume);
        else {
            // P4: measured in the pose the part is left in (laid on a cut face or as it was). Left as it
            // was, it is only warned about when no cut face would give it a base either (item 1).
            Transform3d turn = Transform3d::Identity();
            turn.linear()    = part->instances.front()->get_matrix().linear();
            its_transform(merged, turn, true);
            const double contact = contact_area_of(merged);
            const bool   could_lay =
                !laid && mp.piece >= 0 && plan.pieces[size_t(mp.piece)].best_contact_area >= SPLIT_MIN_CONTACT_AREA;
            if (contact < SPLIT_MIN_CONTACT_AREA && !could_lay)
                warn(SplitWarning::Kind::NoFlatFace, contact);
        }
    }
    if (result.discarded > 0) {
        SplitWarning w{ SplitWarning::Kind::DiscardedPieces, -1, -1, double(result.discarded) };
        w.value2 = result.discarded_volume;
        result.warnings.push_back(w);
    }
    return result;
}

} // namespace Slic3r
