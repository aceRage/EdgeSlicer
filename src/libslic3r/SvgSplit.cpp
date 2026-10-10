#include "SvgSplit.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Emboss.hpp" // get_extents(ExPolygonsWithIds)
#include "NSVGUtils.hpp"
#include "UntrustedInput.hpp"
#include "FlushVolPredictor.hpp" // CIEDE2000
#include "nanosvg/nanosvg.h"

namespace Slic3r {
namespace {

long long svgsplit_coord_to_um(double c) { return std::llround(c * SCALING_FACTOR * 1000.); }

std::array<uint8_t, 3> svgsplit_paint_color(const NSVGpaint &paint)
{
    unsigned int c = 0;
    if (paint.type == NSVG_PAINT_COLOR)
        c = paint.color;
    else if ((paint.type == NSVG_PAINT_LINEAR_GRADIENT || paint.type == NSVG_PAINT_RADIAL_GRADIENT) && paint.gradient != nullptr &&
             paint.gradient->nstops > 0)
        c = paint.gradient->stops[0].color;
    // NanoSVG stores colour as 0xAABBGGRR
    return {uint8_t(c & 0xff), uint8_t((c >> 8) & 0xff), uint8_t((c >> 16) & 0xff)};
}

void svgsplit_remove_small(ExPolygons &shape, double min_area_scaled)
{
    if (min_area_scaled <= 0.)
        return;
    shape.erase(std::remove_if(shape.begin(), shape.end(),
                               [min_area_scaled](const ExPolygon &e) { return std::abs(e.contour.area()) < min_area_scaled; }),
                shape.end());
    for (ExPolygon &e : shape)
        e.holes.erase(std::remove_if(e.holes.begin(), e.holes.end(),
                                     [min_area_scaled](const Polygon &h) { return std::abs(h.area()) < min_area_scaled; }),
                      e.holes.end());
}

// Align points to whole micrometers, as they are written into the SVG of the part
void svgsplit_snap_to_um(ExPolygons &shape)
{
    auto snap = [](Polygon &polygon) {
        for (Point &p : polygon.points) {
            p.x() = coord_t(svgsplit_coord_to_um(double(p.x())) * 1000);
            p.y() = coord_t(svgsplit_coord_to_um(double(p.y())) * 1000);
        }
        polygon.points.erase(std::unique(polygon.points.begin(), polygon.points.end()), polygon.points.end());
        if (polygon.points.size() > 1 && polygon.points.front() == polygon.points.back())
            polygon.points.pop_back();
    };
    for (ExPolygon &e : shape) {
        snap(e.contour);
        for (Polygon &h : e.holes)
            snap(h);
        e.holes.erase(std::remove_if(e.holes.begin(), e.holes.end(), [](const Polygon &h) { return h.points.size() < 3; }), e.holes.end());
    }
    shape.erase(std::remove_if(shape.begin(), shape.end(), [](const ExPolygon &e) { return e.contour.points.size() < 3; }), shape.end());
}

std::string svgsplit_um_to_mm(long long um)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s%lld.%03lld", um < 0 ? "-" : "", std::llabs(um) / 1000, std::llabs(um) % 1000);
    return buffer;
}

} // namespace

std::string svg_split_color_to_hex(const std::array<uint8_t, 3> &color)
{
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", unsigned(color[0]), unsigned(color[1]), unsigned(color[2]));
    return buffer;
}

std::string svg_split_part_svg(const ExPolygons &shape, const BoundingBox &drawing, const std::array<uint8_t, 3> &color)
{
    // SVG is Y down: origin in the top left corner of the drawing
    const long long left = svgsplit_coord_to_um(double(drawing.min.x()));
    const long long top  = svgsplit_coord_to_um(double(drawing.max.y()));
    const long long w    = std::max(1LL, svgsplit_coord_to_um(double(drawing.max.x())) - left);
    const long long h    = std::max(1LL, top - svgsplit_coord_to_um(double(drawing.min.y())));
    std::stringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n";
    ss << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << svgsplit_um_to_mm(w) << "mm\" height=\"" << svgsplit_um_to_mm(h)
       << "mm\" viewBox=\"0 0 " << w << " " << h << "\">\n";
    ss << "<path fill=\"" << svg_split_color_to_hex(color) << "\" fill-rule=\"evenodd\" d=\"";
    auto write_polygon = [&](const Polygon &polygon) {
        if (polygon.points.size() < 3)
            return;
        for (size_t i = 0; i < polygon.points.size(); ++i) {
            const Point &p = polygon.points[i];
            ss << (i == 0 ? "M" : (i == 1 ? "L" : " ")) << (svgsplit_coord_to_um(double(p.x())) - left) << " "
               << (top - svgsplit_coord_to_um(double(p.y())));
        }
        ss << "Z";
    };
    for (const ExPolygon &expoly : shape) {
        write_polygon(expoly.contour);
        for (const Polygon &hole : expoly.holes)
            write_polygon(hole);
    }
    ss << "\"/>\n</svg>\n";
    return ss.str();
}

std::string svg_split_path_in_3mf(const std::string &group_id, size_t index)
{
    return "3D/svgsplit_" + group_id + "_" + std::to_string(index + 1) + ".svg";
}

double svg_split_min_area(const BoundingBox &drawing, double floor)
{
    if (!drawing.defined)
        return floor;
    double w = unscale<double>(drawing.size().x());
    double h = unscale<double>(drawing.size().y());
    return std::max(floor, SVG_SPLIT_SPECK_AREA_FRACTION * w * h);
}

double svg_split_min_width(const BoundingBox &drawing, double floor)
{
    if (!drawing.defined)
        return floor;
    double w = unscale<double>(drawing.size().x());
    double h = unscale<double>(drawing.size().y());
    return std::max(floor, SVG_SPLIT_SLIVER_DIAGONAL_FRACTION * std::sqrt(w * w + h * h));
}

SvgSplitResult split_svg_by_shapes(const NSVGimage &image, const NSVGLineParams &params, double min_area_floor, bool write_svg)
{
    SvgSplitResult result;

    // Same conversion as the whole SVG volume, but keep the image coordinates (Y up, not centered)
    bool              too_complex = false;
    ExPolygonsWithIds shapes      = create_shape_with_ids(image, params, &too_complex, false);
    if (too_complex) {
        result.too_complex = true;
        result.error       = "The SVG is too complex, it has too many shapes or points.";
        return result;
    }

    // Paint of every shape in the order of the file
    std::vector<const NSVGshape *> nsvg_shapes;
    for (const NSVGshape *s = image.shapes; s != nullptr; s = s->next)
        nsvg_shapes.push_back(s);

    shapes.erase(std::remove_if(shapes.begin(), shapes.end(), [](const ExPolygonsWithId &s) { return s.expoly.empty(); }),
                 shapes.end());
    result.painted = shapes.size();
    if (shapes.empty()) {
        result.error = "The SVG does not contain a single shape.";
        return result;
    }

    const BoundingBox drawing = get_extents(shapes);
    result.width              = unscale<double>(drawing.size().x());
    result.height             = unscale<double>(drawing.size().y());
    // Specks scale with the drawing: an SVG is usually scaled down to the print, so a speck of a big
    // drawing is a speck of the print too (see svg_split_min_area / svg_split_min_width)
    result.min_area              = svg_split_min_area(drawing, min_area_floor);
    result.min_width             = svg_split_min_width(drawing);
    const double min_area_scaled = result.min_area / (SCALING_FACTOR * SCALING_FACTOR);
    // Opening by half of the width removes slivers thinner than the width
    const float sliver = float(scale_(result.min_width / 2.));

    // From the top of the painting down: everything painted later covers the shape
    std::vector<SvgSplitPart> parts;
    ExPolygons                covered;
    for (size_t i = shapes.size(); i-- > 0;) {
        const ExPolygonsWithId &s = shapes[i];
        ExPolygons visible = covered.empty() ? s.expoly : diff_ex(s.expoly, covered);
        if (!visible.empty() && sliver > 0.f)
            visible = opening_ex(visible, sliver);
        svgsplit_remove_small(visible, min_area_scaled);
        svgsplit_snap_to_um(visible);
        if (i > 0 && !visible.empty()) {
            // Only what is kept covers the shapes below: a dropped speck or sliver stays with the
            // shape under it instead of leaving a hole there
            ExPolygons all = std::move(covered);
            append(all, visible);
            covered = union_ex(all);
        }
        if (visible.empty()) {
            ++result.covered;
            continue;
        }
        SvgSplitPart part;
        part.shape       = std::move(visible);
        part.shape_index = s.id / 2;
        part.is_stroke   = (s.id % 2) == 1;
        if (part.shape_index < nsvg_shapes.size()) {
            const NSVGshape &ns = *nsvg_shapes[part.shape_index];
            part.color          = svgsplit_paint_color(part.is_stroke ? ns.stroke : ns.fill);
        }
        Point c     = get_extents(part.shape).center();
        Point d     = drawing.center();
        part.offset = Vec2d(unscale<double>(c.x() - d.x()), unscale<double>(c.y() - d.y()));
        parts.push_back(std::move(part));
    }
    std::reverse(parts.begin(), parts.end());
    result.parts = std::move(parts);

    if (!write_svg)
        return result;

    for (SvgSplitPart &part : result.parts) {
        part.svg = svg_split_part_svg(part.shape, drawing, part.color);
        // The same limits as for SVG from 3MF, so the project can be loaded again
        SvgRefusal    refusal = SvgRefusal::None;
        std::string   why;
        NSVGimage_ptr check = nsvgParse_checked(part.svg, refusal, &why);
        if (check == nullptr) {
            result.too_complex = refusal == SvgRefusal::TooComplex || refusal == SvgRefusal::TooLarge;
            result.error       = "A part of the SVG is too complex (" + why + ").";
            result.parts.clear();
            return result;
        }
    }
    return result;
}

std::vector<SvgSplitColor> svg_split_colors(const SvgSplitResult &result)
{
    std::vector<SvgSplitColor> colors;
    for (size_t i = 0; i < result.parts.size(); ++i) {
        const SvgSplitPart &part = result.parts[i];
        auto it = std::find_if(colors.begin(), colors.end(), [&part](const SvgSplitColor &c) { return c.color == part.color; });
        if (it == colors.end()) {
            colors.push_back({part.color, {}, 0.});
            it = colors.end() - 1;
        }
        it->parts.push_back(i);
        for (const ExPolygon &e : part.shape)
            it->area += e.area() * SCALING_FACTOR * SCALING_FACTOR;
    }
    return colors;
}

bool svg_split_parse_hex(const std::string &hex, std::array<uint8_t, 3> &out)
{
    // "#rrggbb" or "#rrggbbaa" (alpha ignored)
    if (hex.size() < 7 || hex[0] != '#')
        return false;
    for (size_t i = 1; i < 7; ++i)
        if (!std::isxdigit(static_cast<unsigned char>(hex[i])))
            return false;
    for (int c = 0; c < 3; ++c)
        out[size_t(c)] = uint8_t(std::stoi(hex.substr(size_t(1 + 2 * c), 2), nullptr, 16));
    return true;
}

double svg_split_color_distance(const std::array<uint8_t, 3> &a, const std::array<uint8_t, 3> &b)
{
    return FlushPredict::calc_color_distance(FlushPredict::RGBColor(a[0], a[1], a[2]), FlushPredict::RGBColor(b[0], b[1], b[2]));
}

int svg_split_nearest_filament(const std::array<uint8_t, 3> &color, const std::vector<std::string> &filament_colors, double *distance)
{
    int    best   = 0;
    double best_d = std::numeric_limits<double>::max();
    for (size_t i = 0; i < filament_colors.size(); ++i) {
        std::array<uint8_t, 3> f;
        if (!svg_split_parse_hex(filament_colors[i], f))
            continue;
        double d = svg_split_color_distance(color, f);
        // strictly smaller: a tie keeps the lowest filament
        if (d < best_d) {
            best_d = d;
            best   = int(i) + 1;
        }
    }
    if (distance != nullptr)
        *distance = best == 0 ? std::numeric_limits<double>::max() : best_d;
    return best;
}

} // namespace Slic3r
