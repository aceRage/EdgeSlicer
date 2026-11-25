#include "MixedPatternWalls.hpp"

#include "ClipperUtils.hpp"
#include "I18N.hpp"
#include "format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Slic3r {

namespace {

// Summed over the layers of an object. A 10 mm fin one wall too thin passes it in two or three layers.
constexpr double mixed_pattern_min_thin_area = 20.; // mm2

std::string mm1(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

} // namespace

ExPolygons areas_with_fewer_walls(const ExPolygons &outline, coord_t ext_width, coord_t spacing, int walls, bool arachne)
{
    if (outline.empty() || walls < 2 || ext_width <= 0 || spacing <= 0)
        return {};

    // Depth of the deepest wall's centreline; the wall exists where the outline survives a shrink by it.
    double depth = 0.5 * double(ext_width) + double(walls - 1) * double(spacing);
    if (arachne)
        depth -= 0.5 * double(spacing);
    // Stay a little short, so an outline exactly as wide as the walls is not reported.
    depth *= 0.95;

    const Polygons   opened = opening(outline, float(depth));
    const ExPolygons thin   = diff_ex(outline, opened);
    const double     min_area = 4. * double(ext_width) * double(ext_width);

    ExPolygons out;
    for (const ExPolygon &piece : thin)
        if (piece.area() >= min_area)
            out.push_back(piece);
    return out;
}

bool MixedPatternWallShortfall::thin_areas() const
{
    return thin_layers > 0 && thin_area >= mixed_pattern_min_thin_area;
}

std::string mixed_pattern_walls_notice(const std::vector<MixedPatternWallShortfall> &found, const std::string &object_name)
{
    std::string text;
    for (const MixedPatternWallShortfall &f : found) {
        if (!f.any())
            continue;
        if (!text.empty())
            text += "\n";
        text += object_name + ": ";
        if (f.setting_too_low())
            text += Slic3r::format(_u8L("mixed filament %1% has a pattern for %2% walls, but the part prints %3%. "
                                        "The pattern's deeper walls do not print, so the colour will differ and their "
                                        "filaments are only purged. Set Wall loops to %2% or more."),
                                   f.filament_id, f.walls_needed, f.walls_set);
        else if (f.thin_layers == 1)
            text += Slic3r::format(_u8L("mixed filament %1% has a pattern for %2% walls, but fewer fit in thin areas on "
                                        "1 layer (at %3% mm). The colour will differ there."),
                                   f.filament_id, f.walls_needed, mm1(f.first_thin_z));
        else
            text += Slic3r::format(_u8L("mixed filament %1% has a pattern for %2% walls, but fewer fit in thin areas on "
                                        "%3% layers (from %4% mm up). The colour will differ there."),
                                   f.filament_id, f.walls_needed, f.thin_layers, mm1(f.first_thin_z));
        if (f.setting_too_low() && f.thin_areas())
            text += " " + Slic3r::format(_u8L("Where the setting is enough, fewer walls still fit in thin areas on %1% layers."),
                                         f.thin_layers);
    }
    return text;
}

} // namespace Slic3r
