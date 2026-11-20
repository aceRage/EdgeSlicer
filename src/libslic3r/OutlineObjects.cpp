#include "OutlineObjects.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Emboss.hpp"
#include "MeshBoolean.hpp"
#include "libslic3r.h"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>

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
    // Removed islands and thin parts change the size, so the scale is refined
    for (int pass = 0; pass < 4; ++pass) {
        out            = build_silhouette(source, params, factor);
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

    if (params.largest_only && out.islands > 1)
        out.warnings.emplace_back("The image has " + std::to_string(out.islands) +
                                  " separate parts, only the largest one is used. Untick \"Largest part only\" to keep all.");
    if (area_mm2(out.removed) > 0.01 * area_mm2(out.shape))
        out.warnings.emplace_back("Thin parts of the shape were removed (red in the preview).");
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

} // namespace Slic3r
