#include "OutlineObjects.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Emboss.hpp"
#include "MeshBoolean.hpp"
#include "libslic3r.h"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

namespace {

// Outlines are simplified by this tolerance after offsets [in mm], far below what a printer shows
const double OUTLINE_SIMPLIFY = 0.01;

float scaled_f(double mm) { return float(scale_(mm)); }

double area_mm2(const ExPolygons &shape)
{
    double a = 0.;
    for (const ExPolygon &e : shape)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

ExPolygons simplified(ExPolygons shape)
{
    ExPolygons out = expolygons_simplify(shape, scale_(OUTLINE_SIMPLIFY));
    // simplification of a thin shape can make it touch itself
    return union_ex(out);
}

// Mirror by Y (image rows go down, the bed has Y up) and optionally by X
ExPolygons flip(const ExPolygons &shape, bool mirror_x)
{
    Polygons polygons;
    auto     add = [&polygons, mirror_x](const Polygon &src) {
        Polygon p;
        p.points.reserve(src.points.size());
        for (const Point &pt : src.points)
            p.points.emplace_back(mirror_x ? -pt.x() : pt.x(), -pt.y());
        polygons.push_back(std::move(p));
    };
    for (const ExPolygon &e : shape) {
        add(e.contour);
        for (const Polygon &h : e.holes)
            add(h);
    }
    // Contours and holes are found again by the even-odd rule, whatever the orientation is now
    return union_ex(polygons, ClipperLib::pftEvenOdd);
}

ExPolygons without_holes(const ExPolygons &shape)
{
    Polygons contours;
    contours.reserve(shape.size());
    for (const ExPolygon &e : shape)
        contours.push_back(e.contour);
    return union_ex(contours);
}

// Silhouette of the levels scaled by the factor, not centered
Silhouette build_silhouette(const ExPolygons &source, const SilhouetteParams &params, double factor)
{
    Silhouette out;
    ExPolygons shape = source;
    for (ExPolygon &e : shape)
        e.scale(factor);
    shape = union_ex(shape);

    if (params.close_gaps > 0.)
        shape = closing_ex(shape, scaled_f(params.close_gaps / 2.), ClipperLib::jtRound);
    if (params.fill_holes)
        shape = without_holes(shape);
    if (params.min_feature > 0.) {
        // Miter join keeps the corners of the outline sharp
        ExPolygons opened = opening_ex(shape, scaled_f(params.min_feature / 2.), ClipperLib::jtMiter, 3.);
        out.removed       = diff_ex(shape, opened);
        shape             = std::move(opened);
    }
    shape       = simplified(std::move(shape));
    out.islands = shape.size();
    if (params.largest_only && shape.size() > 1) {
        auto largest = std::max_element(shape.begin(), shape.end(),
                                        [](const ExPolygon &a, const ExPolygon &b) { return a.area() < b.area(); });
        ExPolygon keep = std::move(*largest);
        shape.clear();
        shape.push_back(std::move(keep));
    }
    out.shape = std::move(shape);
    return out;
}

double longest_side_mm(const ExPolygons &shape, double *width = nullptr, double *height = nullptr)
{
    if (shape.empty())
        return 0.;
    BoundingBox bb = get_extents(shape);
    double      w  = unscale<double>(bb.size().x());
    double      h  = unscale<double>(bb.size().y());
    if (width != nullptr)
        *width = w;
    if (height != nullptr)
        *height = h;
    return std::max(w, h);
}

// Ring around the shape: the inner side is the shape itself
ExPolygons ring(const ExPolygons &shape, double thickness)
{
    return diff_ex(offset_ex(shape, scaled_f(thickness), ClipperLib::jtRound), shape);
}

ExPolygons brace_bars(const ExPolygons &silhouette, const CookieCutterParams &p)
{
    BoundingBox bb = get_extents(silhouette);
    double      w  = unscale<double>(bb.size().x());
    double      h  = unscale<double>(bb.size().y());
    Polygons    bars;
    auto        add_bars = [&bars, &bb, &p](double length, bool vertical) {
        int count = std::max(1, int(std::floor(length / std::max(10., p.brace_spacing))));
        for (int i = 0; i < count; ++i) {
            coord_t half   = scale_(p.brace_width / 2.);
            coord_t extent = vertical ? bb.size().x() : bb.size().y();
            coord_t c      = (vertical ? bb.min.x() : bb.min.y()) + coord_t(double(extent) * double(i + 1) / double(count + 1));
            Polygon bar;
            if (vertical)
                bar.points = {Point(c - half, bb.min.y()), Point(c + half, bb.min.y()), Point(c + half, bb.max.y()), Point(c - half, bb.max.y())};
            else
                bar.points = {Point(bb.min.x(), c - half), Point(bb.max.x(), c - half), Point(bb.max.x(), c + half), Point(bb.min.x(), c + half)};
            bars.push_back(std::move(bar));
        }
    };
    add_bars(w, true);
    add_bars(h, false);
    // The bars reach half way into the blade, so they are joined with it
    return intersection_ex(union_ex(bars), offset_ex(silhouette, scaled_f(p.blade_thickness / 2.), ClipperLib::jtRound));
}

} // namespace

Silhouette make_silhouette(const std::vector<ExPolygons> &levels, const SilhouetteParams &params)
{
    Silhouette out;
    ExPolygons all;
    for (const ExPolygons &level : levels)
        append(all, level);
    ExPolygons source = flip(union_ex(all), params.mirror);
    double     longest = longest_side_mm(source);
    if (source.empty() || longest <= 0.) {
        out.warnings.emplace_back("The image has no shape.");
        return out;
    }

    double target = std::clamp(params.longest_side, 5., 2000.);
    double factor = target / longest;
    double used   = factor; // the factor the silhouette was built with
    // Removed islands and thin parts change the size, so the scale is refined
    for (int pass = 0; pass < 4; ++pass) {
        out            = build_silhouette(source, params, factor);
        used           = factor;
        double current = longest_side_mm(out.shape);
        if (current <= 0.)
            break;
        if (std::abs(current - target) <= 0.002 * target)
            break;
        factor *= target / current;
    }
    if (out.shape.empty()) {
        out.removed.clear();
        out.warnings.emplace_back("Nothing is left of the shape. Lower \"Remove thin parts\" or make the object larger.");
        return out;
    }

    // Exact size and center
    double current = longest_side_mm(out.shape);
    double fix     = target / current;
    for (ExPolygon &e : out.shape)
        e.scale(fix);
    for (ExPolygon &e : out.removed)
        e.scale(fix);
    Point center = get_extents(out.shape).center();
    for (ExPolygon &e : out.shape)
        e.translate(-center);
    for (ExPolygon &e : out.removed)
        e.translate(-center);
    longest_side_mm(out.shape, &out.width, &out.height);
    out.factor   = used * fix;
    out.center   = center;
    out.mirrored = params.mirror;

    if (params.largest_only && out.islands > 1)
        out.warnings.emplace_back("The image has " + std::to_string(out.islands) +
                                  " separate parts, only the largest one is used. Untick \"Largest part only\" to keep all.");
    if (area_mm2(out.removed) > 0.01 * area_mm2(out.shape))
        out.warnings.emplace_back("Thin parts of the shape were removed (red in the preview).");
    return out;
}

ExPolygons to_silhouette_frame(const ExPolygons &traced, const Silhouette &silhouette)
{
    ExPolygons out = flip(traced, silhouette.mirrored);
    for (ExPolygon &e : out) {
        e.scale(silhouette.factor);
        e.translate(-silhouette.center);
    }
    return out;
}

CookieCutterParams sanitize(const CookieCutterParams &params)
{
    CookieCutterParams p = params;
    p.blade_thickness = std::clamp(p.blade_thickness, 0.3, 5.);
    p.handle_height   = std::clamp(p.handle_height, 0.4, 20.);
    p.handle_width    = std::clamp(p.handle_width, p.blade_thickness, 30.);
    p.blade_height    = std::clamp(p.blade_height, p.handle_height + 1., 100.);
    p.tip_width       = std::clamp(p.tip_width, 0.2, p.blade_thickness);
    p.tip_height      = std::clamp(p.tip_height, 0., p.blade_height - p.handle_height);
    p.taper_step      = std::clamp(p.taper_step, 0.05, 1.);
    p.brace_width     = std::clamp(p.brace_width, 1., 20.);
    p.brace_spacing   = std::clamp(p.brace_spacing, 10., 500.);
    if (p.tip_height <= 0. || p.tip_width >= p.blade_thickness)
        p.sharp_edge = false;
    return p;
}

CookieCutterSections cookie_cutter_sections(const ExPolygons &silhouette, const CookieCutterParams &params)
{
    CookieCutterSections out;
    if (silhouette.empty())
        return out;
    const CookieCutterParams p = sanitize(params);
    // The rings are not simplified: all of them share the points of the silhouette on their
    // inner side, so the walls of bands stacked on each other are exactly aligned
    out.blade  = ring(silhouette, p.blade_thickness);
    out.tip    = p.sharp_edge ? ring(silhouette, p.tip_width) : out.blade;
    out.handle = ring(silhouette, p.handle_width);
    out.braces = p.braces == CutterBraces::Always ||
                 (p.braces == CutterBraces::Auto && longest_side_mm(silhouette) > COOKIE_CUTTER_AUTO_BRACE_SIDE);
    if (out.braces)
        out.handle = union_ex(out.handle, brace_bars(silhouette, p));
    return out;
}

indexed_triangle_set outline_prism(const ExPolygons &shape, double z0, double z1)
{
    indexed_triangle_set its;
    if (shape.empty() || !(z1 > z0))
        return its;
    // Any positive depth: only the side of the vertex (front or back) is read from it
    const double depth = 1000.;
    its = Emboss::polygons2model(shape, Emboss::ProjectZ(depth));
    for (Vec3f &v : its.vertices) {
        // Shared points of prisms stacked on each other get identical coordinates
        v.x() = float(unscale<double>(double(v.x())));
        v.y() = float(unscale<double>(double(v.y())));
        v.z() = float(v.z() > depth / 2. ? z1 : z0);
    }
    if (its_volume(its) < 0.f)
        its_flip_triangles(its);
    return its;
}

indexed_triangle_set join_outline_bands(const std::vector<OutlineBand> &bands, bool *joined)
{
    std::vector<indexed_triangle_set> prisms;
    for (const OutlineBand &band : bands) {
        indexed_triangle_set prism = outline_prism(band.shape, band.z0, band.z1);
        if (!prism.indices.empty())
            prisms.push_back(std::move(prism));
    }
    if (joined != nullptr)
        *joined = false;
    indexed_triangle_set out;
    if (prisms.empty())
        return out;
    if (prisms.size() == 1) {
        if (joined != nullptr)
            *joined = true;
        return std::move(prisms.front());
    }
    if (MeshBoolean::mfd::union_all(prisms, out) && !out.indices.empty()) {
        if (joined != nullptr)
            *joined = true;
        return out;
    }
    BOOST_LOG_TRIVIAL(warning) << "OutlineObjects: union of " << prisms.size() << " bands failed, the bands are only merged";
    out = {};
    for (const indexed_triangle_set &prism : prisms)
        its_merge(out, prism);
    return out;
}

OutlineObjectResult make_cookie_cutter(const ExPolygons &silhouette, const CookieCutterParams &params)
{
    OutlineObjectResult out;
    if (silhouette.empty()) {
        out.error = "There is no shape to make a cookie cutter from.";
        return out;
    }
    const CookieCutterParams   p        = sanitize(params);
    const CookieCutterSections sections = cookie_cutter_sections(silhouette, p);
    if (sections.blade.empty() || sections.handle.empty()) {
        out.error = "The outline of the shape could not be created.";
        return out;
    }

    std::vector<OutlineBand> bands;
    bands.push_back({sections.handle, 0., p.handle_height});
    double blade_top = p.sharp_edge ? p.blade_height - p.tip_height : p.blade_height;
    if (blade_top > p.handle_height + EPSILON)
        bands.push_back({sections.blade, p.handle_height, blade_top});
    else
        blade_top = p.handle_height;
    if (p.sharp_edge) {
        double height = p.blade_height - blade_top;
        int    steps  = std::max(1, int(std::ceil(height / p.taper_step - 1e-6)));
        for (int i = 0; i < steps; ++i) {
            double     t         = (double(i) + 0.5) / double(steps);
            double     thickness = p.blade_thickness + (p.tip_width - p.blade_thickness) * t;
            ExPolygons shape     = i + 1 == steps ? sections.tip : ring(silhouette, thickness);
            bands.push_back({std::move(shape), blade_top + height * double(i) / double(steps),
                             blade_top + height * double(i + 1) / double(steps)});
        }
    }

    out.mesh = join_outline_bands(bands, &out.joined);
    if (out.mesh.indices.empty()) {
        out.error = "The cookie cutter could not be created.";
        return out;
    }
    if (!out.joined)
        out.warnings.emplace_back("The parts of the cutter could not be joined into one mesh; they overlap, which still prints correctly.");
    return out;
}

// ---- box with a lid ----------------------------------------------------------------------------

namespace {

// Slivers of the picture thinner than this are not printable and are dropped [in mm]
const double DECORATION_MIN_WIDTH = 0.4;
// Height of one step of a chamfer [in mm]
const double CHAMFER_STEP = 0.2;

ExPolygons mirror_x(const ExPolygons &shape)
{
    Polygons polygons;
    auto     add = [&polygons](const Polygon &src) {
        Polygon p;
        p.points.reserve(src.points.size());
        for (const Point &pt : src.points)
            p.points.emplace_back(-pt.x(), pt.y());
        polygons.push_back(std::move(p));
    };
    for (const ExPolygon &e : shape) {
        add(e.contour);
        for (const Polygon &h : e.holes)
            add(h);
    }
    return union_ex(polygons, ClipperLib::pftEvenOdd);
}

ExPolygons inset(const ExPolygons &shape, double distance)
{
    if (distance <= 0.)
        return shape;
    // Miter join: the inner corners of the wall follow the outline
    return offset_ex(shape, -scaled_f(distance), ClipperLib::jtMiter, 3.);
}

} // namespace

OutlineBoxParams sanitize(const OutlineBoxParams &params)
{
    OutlineBoxParams p  = params;
    p.wall              = std::clamp(p.wall, 0.4, 10.);
    p.floor             = std::clamp(p.floor, 0.2, 10.);
    p.height            = std::clamp(p.height, p.floor + 2., 300.);
    p.chamfer           = std::clamp(p.chamfer, 0., std::min(p.floor, p.wall));
    p.lid_overhang      = std::clamp(p.lid_overhang, 0., 10.);
    p.lip_height        = std::clamp(p.lip_height, 1., std::min(20., p.height - p.floor));
    p.lip_thickness     = std::clamp(p.lip_thickness, 0.8, 5.);
    p.clearance         = std::clamp(p.clearance, 0., 1.);
    p.lid_thickness     = std::clamp(p.lid_thickness, 0.6, 10.);
    p.decoration_depth  = std::clamp(p.decoration_depth, 0.2, 2.);
    // the picture never goes through the lid
    p.lid_thickness     = std::max(p.lid_thickness, p.decoration_depth + 0.4);
    p.decoration_margin = std::clamp(p.decoration_margin, 0., 10.);
    return p;
}

OutlineBoxSections outline_box_sections(const ExPolygons &silhouette, const std::vector<ExPolygons> &detail, const OutlineBoxParams &params)
{
    OutlineBoxSections out;
    if (silhouette.empty())
        return out;
    const OutlineBoxParams p = sanitize(params);
    out.cavity = inset(silhouette, p.wall);
    out.wall   = diff_ex(silhouette, out.cavity);
    out.lid    = p.lid_overhang > 0. ? offset_ex(silhouette, scaled_f(p.lid_overhang), ClipperLib::jtRound) : silhouette;
    ExPolygons lip_outer = inset(silhouette, p.wall + p.clearance);
    out.lip              = diff_ex(lip_outer, inset(lip_outer, p.lip_thickness));

    if (p.decoration && !detail.empty()) {
        ExPolygons area = inset(silhouette, p.decoration_margin);
        ExPolygons used;
        for (const ExPolygons &level : detail) {
            ExPolygons shape = intersection_ex(level, area);
            // every point of the lid belongs to one level at most
            if (!used.empty())
                shape = diff_ex(shape, used);
            if (!shape.empty())
                shape = opening_ex(shape, scaled_f(DECORATION_MIN_WIDTH / 2.), ClipperLib::jtMiter, 3.);
            if (!shape.empty())
                used = union_ex(used, shape);
            out.decoration.push_back(std::move(shape));
        }
        if (used.empty())
            out.decoration.clear();
    }
    return out;
}

OutlineBoxResult make_outline_box(const ExPolygons &silhouette, const std::vector<ExPolygons> &detail, const OutlineBoxParams &params)
{
    OutlineBoxResult out;
    if (silhouette.empty()) {
        out.error = "There is no shape to make a box from.";
        return out;
    }
    const OutlineBoxParams   p        = sanitize(params);
    const OutlineBoxSections sections = outline_box_sections(silhouette, detail, p);
    bool                     joined   = true;

    // ---- base: floor (with chamfered bottom edge) and wall -----------------------------------
    {
        std::vector<OutlineBand> bands;
        double                   z     = 0.;
        int                      steps = p.chamfer > 0. ? std::max(1, int(std::ceil(p.chamfer / CHAMFER_STEP - 1e-6))) : 0;
        for (int i = 0; i < steps; ++i) {
            double     t     = (double(i) + 0.5) / double(steps);
            ExPolygons shape = inset(silhouette, p.chamfer * (1. - t));
            double     z1    = p.chamfer * double(i + 1) / double(steps);
            if (!shape.empty())
                bands.push_back({std::move(shape), z, z1});
            z = z1;
        }
        if (p.floor > z + EPSILON)
            bands.push_back({silhouette, z, p.floor});
        if (sections.cavity.empty()) {
            out.warnings.emplace_back("The shape is too thin for a cavity, the base is solid. Make the box larger or the wall thinner.");
            bands.push_back({silhouette, p.floor, p.height});
        } else {
            if (area_mm2(sections.cavity) < 0.25 * area_mm2(silhouette))
                out.warnings.emplace_back("Most of the shape is too thin for a cavity; the thin parts of the base are solid.");
            bands.push_back({sections.wall, p.floor, p.height});
        }
        bool         ok = false;
        OutlinePart  part;
        part.name = "Base";
        part.mesh = join_outline_bands(bands, &ok);
        joined    = joined && ok;
        if (part.mesh.indices.empty()) {
            out.error = "The base of the box could not be created.";
            return out;
        }
        out.base.name = "Base";
        out.base.parts.push_back(std::move(part));
    }

    // ---- lid: printed with the top face on the bed, so everything is mirrored ----------------
    {
        std::vector<ExPolygons> pictures;
        ExPolygons              picture_all;
        for (const ExPolygons &level : sections.decoration) {
            pictures.push_back(mirror_x(level));
            append(picture_all, pictures.back());
        }
        picture_all = union_ex(picture_all);
        const ExPolygons plate = mirror_x(sections.lid);
        const ExPolygons lip   = mirror_x(sections.lip);

        std::vector<OutlineBand> bands;
        if (picture_all.empty()) {
            bands.push_back({plate, 0., p.lid_thickness});
        } else {
            bands.push_back({diff_ex(plate, picture_all), 0., p.decoration_depth});
            bands.push_back({plate, p.decoration_depth, p.lid_thickness});
        }
        if (lip.empty())
            out.warnings.emplace_back("The shape is too thin for a lip under the lid, the lid only lies on the base.");
        else
            bands.push_back({lip, p.lid_thickness, p.lid_thickness + p.lip_height});

        bool        ok = false;
        OutlinePart body;
        body.name = "Lid";
        body.mesh = join_outline_bands(bands, &ok);
        joined    = joined && ok;
        if (body.mesh.indices.empty()) {
            out.error = "The lid of the box could not be created.";
            return out;
        }
        out.lid.name = "Lid";
        out.lid.parts.push_back(std::move(body));
        for (size_t i = 0; i < pictures.size(); ++i) {
            OutlinePart part;
            part.mesh = outline_prism(pictures[i], 0., p.decoration_depth);
            if (part.mesh.indices.empty())
                continue;
            part.name  = "Colour " + std::to_string(i + 1);
            part.level = int(i);
            out.lid.parts.push_back(std::move(part));
        }
    }

    if (!joined)
        out.warnings.emplace_back("Some parts of the box could not be joined into one mesh; they overlap, which still prints correctly.");
    return out;
}

// ---- clicker -----------------------------------------------------------------------------------

namespace {

ExPolygon square_at(const Vec2d &center, double side)
{
    double    h = side / 2.;
    ExPolygon e;
    e.contour.points = {Point::new_scale(center.x() - h, center.y() - h), Point::new_scale(center.x() + h, center.y() - h),
                        Point::new_scale(center.x() + h, center.y() + h), Point::new_scale(center.x() - h, center.y() + h)};
    return e;
}

ExPolygon rect_at(const Vec2d &center, double size_x, double size_y)
{
    double    hx = size_x / 2., hy = size_y / 2.;
    ExPolygon e;
    e.contour.points = {Point::new_scale(center.x() - hx, center.y() - hy), Point::new_scale(center.x() + hx, center.y() - hy),
                        Point::new_scale(center.x() + hx, center.y() + hy), Point::new_scale(center.x() - hx, center.y() + hy)};
    return e;
}

ExPolygon circle_at(const Vec2d &center, double radius, int segments = 48)
{
    ExPolygon e;
    e.contour.points.reserve(size_t(segments));
    for (int i = 0; i < segments; ++i) {
        double a = 2. * PI * double(i) / double(segments);
        e.contour.points.push_back(Point::new_scale(center.x() + radius * std::cos(a), center.y() + radius * std::sin(a)));
    }
    return e;
}

// Post under a cap: a cylinder with the cross of the stem cut out
ExPolygons stem_post(const Vec2d &center, double fit)
{
    double     length = MX_CROSS_LENGTH + 2. * fit;
    double     width  = MX_CROSS_WIDTH + 2. * fit;
    ExPolygons cross  = union_ex(ExPolygons{rect_at(center, length, width), rect_at(center, width, length)});
    return diff_ex(ExPolygons{circle_at(center, MX_POST_DIAMETER / 2.)}, cross);
}

// One switch in a body
struct SwitchSocket
{
    Vec2d  position = Vec2d::Zero();
    double fit      = 0.;
};

// Picture on a top face: the levels inside of the outline, made not to overlap
std::vector<ExPolygons> picture_levels(const ExPolygons &silhouette, const std::vector<ExPolygons> &detail, double margin)
{
    std::vector<ExPolygons> out;
    ExPolygons              area = inset(silhouette, margin);
    ExPolygons              used;
    for (const ExPolygons &level : detail) {
        ExPolygons shape = intersection_ex(level, area);
        if (!used.empty())
            shape = diff_ex(shape, used);
        if (!shape.empty())
            shape = opening_ex(shape, scaled_f(DECORATION_MIN_WIDTH / 2.), ClipperLib::jtMiter, 3.);
        if (!shape.empty())
            used = union_ex(used, shape);
        out.push_back(std::move(shape));
    }
    if (used.empty())
        out.clear();
    return out;
}

// Solid body with sockets. footprint .. outline of the body; low .. added to the footprint under
// low_height (key ring tab); hole .. cut through everything (hole of the key ring)
indexed_triangle_set socket_body(const ExPolygons &footprint, const ExPolygons &low, double low_height, const ExPolygons &hole,
                                 const std::vector<SwitchSocket> &sockets, const ClickerParams &p, bool *joined)
{
    ExPolygons pockets, openings;
    for (const SwitchSocket &s : sockets) {
        pockets.push_back(square_at(s.position, MX_POCKET));
        openings.push_back(square_at(s.position, MX_OPENING + 2. * s.fit));
    }
    pockets  = union_ex(pockets);
    openings = union_ex(openings);

    const double lip_z = p.body_height - MX_PLATE;
    // Heights where the cross-section changes
    std::vector<double> levels = {0., p.floor, lip_z, p.body_height};
    int                 steps  = p.chamfer > 0. ? std::max(1, int(std::ceil(p.chamfer / CHAMFER_STEP - 1e-6))) : 0;
    for (int i = 1; i <= steps; ++i)
        levels.push_back(p.chamfer * double(i) / double(steps));
    if (!low.empty() && low_height > 0. && low_height < p.body_height)
        levels.push_back(low_height);
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end(), [](double a, double b) { return std::abs(a - b) < 1e-6; }), levels.end());

    const ExPolygons with_low = low.empty() ? footprint : union_ex(footprint, low);
    std::vector<OutlineBand> bands;
    for (size_t i = 0; i + 1 < levels.size(); ++i) {
        double     z0 = levels[i], z1 = levels[i + 1];
        double     z  = (z0 + z1) / 2.;
        ExPolygons shape = (!low.empty() && z < low_height) ? with_low : footprint;
        if (z < p.chamfer)
            shape = inset(shape, p.chamfer * (1. - z / p.chamfer));
        if (!hole.empty())
            shape = diff_ex(shape, hole);
        if (z > lip_z)
            shape = diff_ex(shape, openings);
        else if (z > p.floor)
            shape = diff_ex(shape, pockets);
        if (!shape.empty())
            bands.push_back({std::move(shape), z0, z1});
    }
    return join_outline_bands(bands, joined);
}

// Cap in print orientation (top face on the bed): plate with the picture inlaid and posts on it.
// All shapes are already mirrored.
bool cap_object(OutlineObject &out, const ExPolygons &plate, const std::vector<ExPolygons> &pictures, const ExPolygons &posts,
                double thickness, double picture_depth, bool *joined)
{
    ExPolygons picture_all;
    for (const ExPolygons &level : pictures)
        append(picture_all, level);
    picture_all = union_ex(picture_all);

    std::vector<OutlineBand> bands;
    if (picture_all.empty()) {
        bands.push_back({plate, 0., thickness});
    } else {
        bands.push_back({diff_ex(plate, picture_all), 0., picture_depth});
        bands.push_back({plate, picture_depth, thickness});
    }
    bands.push_back({posts, thickness, thickness + MX_POST_HEIGHT});

    OutlinePart body;
    body.name = "Cap";
    body.mesh = join_outline_bands(bands, joined);
    if (body.mesh.indices.empty())
        return false;
    out.parts.push_back(std::move(body));
    for (size_t i = 0; i < pictures.size(); ++i) {
        OutlinePart part;
        part.mesh = outline_prism(pictures[i], 0., picture_depth);
        if (part.mesh.indices.empty())
            continue;
        part.name  = "Colour " + std::to_string(i + 1);
        part.level = int(i);
        out.parts.push_back(std::move(part));
    }
    return true;
}

// Tab for a key ring: a ring which overlaps the edge of the shape in the given direction
void keyring_tab(const ExPolygons &silhouette, double angle_deg, ExPolygons &tab, ExPolygons &hole)
{
    BoundingBox bb     = get_extents(silhouette);
    Point       middle = bb.center();
    Vec2d       origin(unscale<double>(middle.x()), unscale<double>(middle.y()));
    double      a      = angle_deg * PI / 180.;
    Vec2d       dir(std::cos(a), std::sin(a));
    double      reach  = unscale<double>((bb.max - bb.min).cast<double>().norm());
    // the last point of the ray which is inside of the shape
    double edge = 0.;
    for (double t = 0.; t <= reach; t += 0.2) {
        Point pt = Point::new_scale(origin.x() + dir.x() * t, origin.y() + dir.y() * t);
        for (const ExPolygon &e : silhouette)
            if (e.contains(pt)) {
                edge = t;
                break;
            }
    }
    // the hole lies just outside of the shape, the ring around it overlaps the shape
    Vec2d center = origin + dir * (edge + KEYRING_HOLE / 2. + 0.5);
    tab  = {circle_at(center, KEYRING_HOLE / 2. + KEYRING_WIDTH)};
    hole = {circle_at(center, KEYRING_HOLE / 2.)};
}

double distance_to_boundary_sq(const ExPolygon &e, const Vec2d &p)
{
    double best = std::numeric_limits<double>::max();
    auto   scan = [&best, &p](const Polygon &polygon) {
        const Points &pts = polygon.points;
        for (size_t i = 0; i < pts.size(); ++i) {
            Vec2d  a  = pts[i].cast<double>();
            Vec2d  b  = pts[(i + 1) % pts.size()].cast<double>();
            Vec2d  ab = b - a;
            double l2 = ab.squaredNorm();
            double t  = l2 > 0. ? std::clamp((p - a).dot(ab) / l2, 0., 1.) : 0.;
            best      = std::min(best, (a + ab * t - p).squaredNorm());
        }
    };
    scan(e.contour);
    for (const Polygon &h : e.holes)
        scan(h);
    return best;
}

} // namespace

Vec2d largest_inscribed_circle(const ExPolygons &shape, double *radius)
{
    if (radius != nullptr)
        *radius = 0.;
    if (shape.empty())
        return Vec2d::Zero();
    // Distance (scaled, squared) from the point to the outline, negative outside of the shape
    auto depth = [&shape](const Vec2d &p) {
        Point pt(coord_t(p.x()), coord_t(p.y()));
        for (const ExPolygon &e : shape)
            if (e.contains(pt))
                return distance_to_boundary_sq(e, p);
        return -1.;
    };
    BoundingBox bb   = get_extents(shape);
    Vec2d       min  = bb.min.cast<double>();
    Vec2d       size = (bb.max - bb.min).cast<double>();
    Vec2d       best = bb.center().cast<double>();
    double      best_depth = depth(best);
    // Coarse grid, then finer grids around the best point
    const int grid = 48;
    Vec2d     step = size / double(grid);
    for (int y = 0; y <= grid; ++y)
        for (int x = 0; x <= grid; ++x) {
            Vec2d  p(min.x() + step.x() * x, min.y() + step.y() * y);
            double d = depth(p);
            if (d > best_depth) {
                best_depth = d;
                best       = p;
            }
        }
    for (int pass = 0; pass < 4; ++pass) {
        Vec2d center = best;
        Vec2d fine   = step / 4.;
        for (int y = -4; y <= 4; ++y)
            for (int x = -4; x <= 4; ++x) {
                Vec2d  p(center.x() + fine.x() * x, center.y() + fine.y() * y);
                double d = depth(p);
                if (d > best_depth) {
                    best_depth = d;
                    best       = p;
                }
            }
        step = fine;
    }
    if (best_depth < 0.)
        return Vec2d::Zero();
    if (radius != nullptr)
        *radius = unscale<double>(std::sqrt(best_depth));
    return Vec2d(unscale<double>(best.x()), unscale<double>(best.y()));
}

ClickerParams sanitize(const ClickerParams &params)
{
    ClickerParams p     = params;
    p.floor             = std::clamp(p.floor, 0.6, 5.);
    p.body_height       = std::clamp(p.body_height, p.floor + MX_DEPTH + MX_PLATE, 60.);
    p.chamfer           = std::clamp(p.chamfer, 0., p.floor);
    p.socket_fit        = std::clamp(p.socket_fit, -0.2, 0.5);
    p.stem_fit          = std::clamp(p.stem_fit, -0.1, 0.3);
    p.switch_offset.x() = std::clamp(p.switch_offset.x(), -200., 200.);
    p.switch_offset.y() = std::clamp(p.switch_offset.y(), -200., 200.);
    p.cap_overhang      = std::clamp(p.cap_overhang, 0., 10.);
    p.decoration_depth  = std::clamp(p.decoration_depth, 0.2, 2.);
    p.cap_thickness     = std::clamp(p.cap_thickness, std::max(0.8, p.decoration_depth + 0.4), 10.);
    p.decoration_margin = std::clamp(p.decoration_margin, 0., 10.);
    return p;
}

ClickerSections clicker_sections(const ExPolygons &silhouette, const std::vector<ExPolygons> &detail, const ClickerParams &params)
{
    ClickerSections out;
    if (silhouette.empty())
        return out;
    const ClickerParams p = sanitize(params);
    out.switch_position   = largest_inscribed_circle(silhouette, &out.inscribed_radius) + p.switch_offset;
    // the corner of the pocket is the farthest point from the middle of the switch
    out.needed_radius = MX_POCKET / 2. * std::sqrt(2.) + CLICKER_MIN_WALL;
    out.pocket        = {square_at(out.switch_position, MX_POCKET)};
    out.opening       = {square_at(out.switch_position, MX_OPENING + 2. * p.socket_fit)};
    out.fits          = diff_ex(offset_ex(out.pocket, scaled_f(CLICKER_MIN_WALL), ClipperLib::jtMiter, 3.), silhouette).empty();

    out.body = silhouette;
    if (p.keyring) {
        ExPolygons tab, hole;
        keyring_tab(silhouette, p.keyring_angle, tab, hole);
        out.body = diff_ex(union_ex(silhouette, tab), hole);
    }
    out.cap  = p.cap_overhang > 0. ? offset_ex(silhouette, scaled_f(p.cap_overhang), ClipperLib::jtRound) : silhouette;
    out.post = stem_post(out.switch_position, p.stem_fit);
    if (p.decoration && !detail.empty())
        out.decoration = picture_levels(silhouette, detail, p.decoration_margin);
    return out;
}

ClickerResult make_clicker(const ExPolygons &silhouette, const std::vector<ExPolygons> &detail, const ClickerParams &params)
{
    ClickerResult out;
    if (silhouette.empty()) {
        out.error = "There is no shape to make a clicker from.";
        return out;
    }
    const ClickerParams   p        = sanitize(params);
    const ClickerSections sections = clicker_sections(silhouette, detail, p);
    if (!sections.fits) {
        out.error = "The shape is too small for the switch at this place. Make the clicker larger or move the switch.";
        return out;
    }

    bool joined = true, ok = false;
    // ---- body --------------------------------------------------------------------------------
    {
        ExPolygons tab, hole;
        if (p.keyring)
            keyring_tab(silhouette, p.keyring_angle, tab, hole);
        OutlinePart part;
        part.name = "Body";
        part.mesh = socket_body(silhouette, tab, KEYRING_HEIGHT, hole, {{sections.switch_position, p.socket_fit}}, p, &ok);
        joined    = joined && ok;
        if (part.mesh.indices.empty()) {
            out.error = "The body of the clicker could not be created.";
            return out;
        }
        out.body.name = "Body";
        out.body.parts.push_back(std::move(part));
    }
    // ---- cap: printed with the top face on the bed, so everything is mirrored ----------------
    {
        std::vector<ExPolygons> pictures;
        for (const ExPolygons &level : sections.decoration)
            pictures.push_back(mirror_x(level));
        out.cap.name = "Cap";
        if (!cap_object(out.cap, mirror_x(sections.cap), pictures, mirror_x(sections.post), p.cap_thickness, p.decoration_depth, &ok)) {
            out.error = "The cap of the clicker could not be created.";
            return out;
        }
        joined = joined && ok;
    }
    if (!joined)
        out.warnings.emplace_back("Some parts of the clicker could not be joined into one mesh; they overlap, which still prints correctly.");
    return out;
}

std::vector<OutlineObject> make_clicker_fit_test(const ClickerParams &params)
{
    ClickerParams p = sanitize(params);
    p.keyring       = false;
    p.decoration    = false;
    const double pitch = 22.;
    // i + 1 notches in the front edge mark the socket / the stub
    auto notches = [](const Vec2d &center, double front_y, int count) {
        ExPolygons out;
        for (int k = 0; k < count; ++k)
            out.push_back(circle_at(Vec2d(center.x() + (double(k) - double(count - 1) / 2.) * 3., front_y), 1., 24));
        return out;
    };

    std::vector<OutlineObject> objects;
    // ---- tile with three sockets -------------------------------------------------------------
    {
        const double depth = MX_POCKET + 2. * (CLICKER_MIN_WALL + 1.8);
        ExPolygons   tile  = {rect_at(Vec2d::Zero(), 3. * pitch, depth)};
        std::vector<SwitchSocket> sockets;
        ExPolygons                marks;
        for (int i = 0; i < 3; ++i) {
            Vec2d pos((i - 1) * pitch, 0.);
            sockets.push_back({pos, CLICKER_FIT_TEST_SOCKET[i]});
            append(marks, notches(pos, -depth / 2., i + 1));
        }
        tile = diff_ex(tile, union_ex(marks));
        OutlineObject object;
        object.name = "Fit test - switch";
        OutlinePart part;
        part.name = object.name;
        part.mesh = socket_body(tile, {}, 0., {}, sockets, p, nullptr);
        if (!part.mesh.indices.empty()) {
            object.parts.push_back(std::move(part));
            objects.push_back(std::move(object));
        }
    }
    // ---- three cap stubs ---------------------------------------------------------------------
    {
        const double side = 14.;
        ExPolygons   plates, posts, marks;
        for (int i = 0; i < 3; ++i) {
            Vec2d pos((i - 1) * pitch, 0.);
            plates.push_back(square_at(pos, side));
            append(posts, stem_post(pos, CLICKER_FIT_TEST_STEM[i]));
            append(marks, notches(pos, -side / 2., i + 1));
        }
        plates = diff_ex(plates, union_ex(marks));
        OutlineObject object;
        object.name = "Fit test - cap";
        OutlinePart part;
        part.name = object.name;
        part.mesh = join_outline_bands({{plates, 0., 1.2}, {posts, 1.2, 1.2 + MX_POST_HEIGHT}}, nullptr);
        if (!part.mesh.indices.empty()) {
            object.parts.push_back(std::move(part));
            objects.push_back(std::move(object));
        }
    }
    return objects;
}

} // namespace Slic3r
