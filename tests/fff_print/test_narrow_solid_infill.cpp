// Narrow internal solid areas (detect_narrow_internal_solid_infill, on by default): the parts of the solid infill
// inside a part that straight lines would cross in strokes too short to be worth laying are told from the rest and
// filled with loops (Fill.cpp split_solid_surface and filter_vibrating_extrusions, FillConcentricInternal.cpp). And
// small area flow compensation (GCode::_needSAFC), which thins short lines of solid infill. Errors the Rust port of
// this code turned up:
//  * a short line was asked where its run began by reading the lines before it, not itself: a lone short line beside
//    a long one was removed and laid by itself as a narrow area, and of short lines running out two stayed;
//  * a short line reached two ways kept the first comer's count of short lines, the other's went into its skips;
//  * small area flow compensation thinned the loops of a narrow area as if each of their moves were a short line;
//  * it took whatever is on the first layer for a bottom surface, and never compensated one above the first layer;
//  * every loop of a narrow area started at its point nearest the object's origin;
//  * a line of a narrow area that is no loop lost the seam gap from its end.

#include <catch2/catch.hpp>

#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Fill/Fill.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// ---- the filter by itself -------------------------------------------------------------------------------------------

// One line of a section: the sections are 0.45 mm apart, the line runs from `low` to `high` mm.
Line stroke(int section, double low, double high)
{
    const coord_t x = scaled<coord_t>(0.45 * section);
    return Line(Point(x, scaled<coord_t>(low)), Point(x, scaled<coord_t>(high)));
}

// How many lines of each section the filter leaves.
std::vector<size_t> kept(const std::vector<Lines>& sections)
{
    std::vector<size_t> out;
    for (const Lines& lines : filter_vibrating_extrusions(sections))
        out.push_back(lines.size());
    return out;
}

// ---- parts ----------------------------------------------------------------------------------------------------------

// One mesh of several boxes, each given by two opposite corners in mm. Boxes that are to be one body overlap.
TriangleMesh boxes(std::initializer_list<std::array<double, 6>> corners)
{
    indexed_triangle_set all;
    for (const std::array<double, 6>& c : corners) {
        indexed_triangle_set box = its_make_cube(c[3] - c[0], c[4] - c[1], c[5] - c[2]);
        for (stl_vertex& v : box.vertices)
            v += Vec3f(float(c[0]), float(c[1]), float(c[2]));
        its_merge(all, box);
    }
    return TriangleMesh(std::move(all));
}

// A round tube standing on the plate, its axis through the origin: outer and inner radius and height in mm.
TriangleMesh tube(double outer, double inner, double height, int sides = 180)
{
    indexed_triangle_set its;
    for (int i = 0; i < sides; ++i) {
        const double a = 2. * M_PI * double(i) / double(sides);
        const float  c = float(std::cos(a)), s = float(std::sin(a));
        its.vertices.emplace_back(float(outer) * c, float(outer) * s, 0.f);
        its.vertices.emplace_back(float(outer) * c, float(outer) * s, float(height));
        its.vertices.emplace_back(float(inner) * c, float(inner) * s, 0.f);
        its.vertices.emplace_back(float(inner) * c, float(inner) * s, float(height));
    }
    for (int i = 0; i < sides; ++i) {
        const int j  = (i + 1) % sides;
        const int ob = 4 * i, ot = 4 * i + 1, ib = 4 * i + 2, it = 4 * i + 3;
        const int pb = 4 * j, pt = 4 * j + 1, qb = 4 * j + 2, qt = 4 * j + 3;
        its.indices.emplace_back(ob, pb, pt); // outside
        its.indices.emplace_back(ob, pt, ot);
        its.indices.emplace_back(ib, qt, qb); // inside
        its.indices.emplace_back(ib, it, qt);
        its.indices.emplace_back(ot, pt, qt); // top
        its.indices.emplace_back(ot, qt, it);
        its.indices.emplace_back(ob, qb, pb); // bottom
        its.indices.emplace_back(ob, ib, qb);
    }
    return TriangleMesh(std::move(its));
}

// ---- settings -------------------------------------------------------------------------------------------------------

// A 0.4 mm nozzle, 0.2 mm layers, every line 0.45 mm wide, two classic walls, three solid layers at the bottom and at
// the top and sparse infill between. Ten layers in a part 2 mm high: a bottom surface, two layers of solid infill,
// sparse infill, and under the top surface a layer bridged over the sparse infill and one of solid infill.
DynamicPrintConfig narrow_config(const char* solid_pattern = "monotonic")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.4"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"line_width", "0.45"},
        {"outer_wall_line_width", "0.45"},
        {"inner_wall_line_width", "0.45"},
        {"internal_solid_infill_line_width", "0.45"},
        {"sparse_infill_line_width", "0.45"},
        {"top_surface_line_width", "0.45"},
        {"initial_layer_line_width", "0.45"},
        {"wall_loops", "2"},
        {"wall_generator", "classic"},
        {"top_shell_layers", "3"},
        {"bottom_shell_layers", "3"},
        {"top_shell_thickness", "0"},
        {"bottom_shell_thickness", "0"},
        {"sparse_infill_density", "15%"},
        {"internal_solid_infill_pattern", solid_pattern},
        {"detect_narrow_internal_solid_infill", "1"},
        {"skirt_loops", "0"},
        {"brim_type", "no_brim"},
        {"enable_arc_fitting", "0"},
        {"use_relative_e_distances", "1"},
        {"gcode_comments", "1"},
        {"machine_start_gcode", ""},
        {"machine_end_gcode", ""},
    });
    return config;
}

// ---- reading a sliced print ------------------------------------------------------------------------------------------

void flatten(const ExtrusionEntity* entity, std::vector<const ExtrusionPath*>& out)
{
    if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
        for (const ExtrusionEntity* e : collection->entities)
            flatten(e, out);
    } else if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(entity)) {
        for (const ExtrusionPath& path : loop->paths)
            out.push_back(&path);
    } else if (const auto* multi = dynamic_cast<const ExtrusionMultiPath*>(entity)) {
        for (const ExtrusionPath& path : multi->paths)
            out.push_back(&path);
    } else if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
        out.push_back(path);
}

// One area of solid infill inside the part, as a layer has it: the paths it is filled with, in the order they are
// laid, and whether they are the loops of a narrow area or the lines of the pattern.
//
// Told apart by the pattern: with internal_solid_infill_pattern "rectilinear" the lines are handed over to be put in
// order as they are printed (ExtrusionEntityCollection::no_sort false) and the loops of a narrow area are not.
struct SolidArea
{
    size_t                            layer  = 0;
    bool                              narrow = false;
    std::vector<const ExtrusionPath*> paths;

    double length() const
    {
        double out = 0.;
        for (const ExtrusionPath* path : paths)
            out += unscaled<double>(path->polyline.length());
        return out;
    }
    // How far the nozzle moves without laying anything between one path of the area and the next, in mm.
    double travel() const
    {
        double out = 0.;
        for (size_t i = 1; i < paths.size(); ++i)
            out += unscaled<double>((paths[i]->first_point() - paths[i - 1]->last_point()).cast<double>().norm());
        return out;
    }
};

std::vector<SolidArea> solid_areas(const Print& print)
{
    std::vector<SolidArea> out;
    for (const Layer* layer : print.objects().front()->layers())
        for (const LayerRegion* layerm : layer->regions())
            for (const ExtrusionEntity* entity : layerm->fills.entities) {
                const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(entity);
                REQUIRE(collection != nullptr);
                SolidArea area;
                flatten(entity, area.paths);
                if (area.paths.empty() || area.paths.front()->role() != erSolidInfill)
                    continue;
                area.layer  = layer->id();
                area.narrow = collection->no_sort;
                out.push_back(std::move(area));
            }
    return out;
}

// The straight moves of an area's paths that run at 45 degrees to the axes, as the lines of the pattern do over a
// part square to them: their lengths in mm, in the order they are laid.
std::vector<double> diagonal_strokes(const SolidArea& area)
{
    std::vector<double> out;
    for (const ExtrusionPath* path : area.paths)
        for (size_t i = 1; i < path->polyline.points.size(); ++i) {
            const Vec2d  d     = unscaled<double>(Point(path->polyline.points[i] - path->polyline.points[i - 1]));
            const double angle = std::fmod(std::atan2(d.y(), d.x()) * 180. / M_PI + 360., 90.);
            if (std::abs(angle - 45.) < 1. && d.norm() > 0.3)
                out.push_back(d.norm());
        }
    return out;
}

// What a file lays of one kind of extrusion on each layer: mm of moves and mm of filament.
struct Laid
{
    double length   = 0.;
    double filament = 0.;
};

// By layer (0 the first) and by the ";TYPE:" it is laid under. For a file written with relative E and no arcs.
std::vector<std::map<std::string, Laid>> laid_by_layer(const std::string& gcode)
{
    std::vector<std::map<std::string, Laid>> out;
    std::istringstream                       in(gcode);
    std::string                              line, type;
    Vec2d                                    pos{0., 0.};
    while (std::getline(in, line)) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0) {
            out.emplace_back();
            continue;
        }
        if (line.rfind(";TYPE:", 0) == 0) {
            type = line.substr(6);
            while (!type.empty() && (type.back() == '\r' || type.back() == ' '))
                type.pop_back();
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        std::istringstream words(line.substr(0, line.find(';')));
        std::string        word;
        Vec2d              to     = pos;
        bool               has_xy = false;
        double             e      = 0.;
        while (words >> word) {
            if (word.size() < 2)
                continue;
            const double value = std::atof(word.c_str() + 1);
            switch (word.front()) {
            case 'X':
                to.x() = value;
                has_xy = true;
                break;
            case 'Y':
                to.y() = value;
                has_xy = true;
                break;
            case 'E': e = value; break;
            default: break;
            }
        }
        if (has_xy && e > 0. && !out.empty()) {
            Laid& laid = out.back()[type];
            laid.length += (to - pos).norm();
            laid.filament += e;
        }
        pos = to;
    }
    return out;
}

// All of one kind over the layers from `first_layer` on.
Laid laid(const std::vector<std::map<std::string, Laid>>& by_layer, const std::string& type, size_t first_layer = 0)
{
    Laid out;
    for (size_t layer = first_layer; layer < by_layer.size(); ++layer) {
        const auto it = by_layer[layer].find(type);
        if (it != by_layer[layer].end()) {
            out.length += it->second.length;
            out.filament += it->second.filament;
        }
    }
    return out;
}

std::string slice_with(const TriangleMesh& mesh, DynamicPrintConfig config, bool small_area_compensation)
{
    config.set_deserialize_strict({{"small_area_infill_flow_compensation", small_area_compensation ? "1" : "0"}});
    return Slic3r::Test::slice({mesh}, config);
}

const char* const SOLID_INFILL   = "Internal solid infill";
const char* const BOTTOM_SURFACE = "Bottom surface";

} // namespace

// ---- which lines are taken out --------------------------------------------------------------------------------------

// A short line with nothing after it goes unless the run of short lines it ends began beside a long line. Where a run
// began is noted on the line the run begins with and handed on along the run, but it was read off the lines of the
// section before the line asked about. The line a run begins with was so never asked what was noted of it: the lines
// before it are the long ones, of which nothing is noted.
TEST_CASE("A short line that ends the lines beside a long one stays a line", "[NarrowSolidInfill]")
{
    // Two long lines and then one of 3 mm: a line that is the last against a round wall.
    CHECK(kept({{stroke(0, 0., 10.)}, {stroke(1, 0., 10.)}, {stroke(2, 3., 6.)}}) == std::vector<size_t>{1, 1, 1});
    // The same the other way round: the short line first. This was kept before too.
    CHECK(kept({{stroke(0, 3., 6.)}, {stroke(1, 0., 10.)}, {stroke(2, 0., 10.)}}) == std::vector<size_t>{1, 1, 1});
    // And between two long ones.
    CHECK(kept({{stroke(0, 0., 10.)}, {stroke(1, 3., 6.)}, {stroke(2, 0., 10.)}}) == std::vector<size_t>{1, 1, 1});
}

TEST_CASE("Of short lines running out after a long one only the one beside it stays", "[NarrowSolidInfill]")
{
    // The lines of a corner: 10 mm, then 3.5, 2.5, 1.5 and 0.5 mm. The second short line stayed as well, on what the
    // first had been told when a run began with it.
    CHECK(kept({{stroke(0, 0., 10.)}, {stroke(1, 3., 6.5)}, {stroke(2, 3.5, 6.)}, {stroke(3, 4., 5.5)}, {stroke(4, 4.5, 5.)}}) ==
          std::vector<size_t>{1, 1, 0, 0, 0});
    // Short lines with no long line anywhere all go, as before.
    CHECK(kept({{stroke(0, 3., 6.)}, {stroke(1, 3., 6.)}, {stroke(2, 3., 6.)}}) == std::vector<size_t>{0, 0, 0});
    // And long lines all stay.
    CHECK(kept({{stroke(0, 0., 10.)}, {stroke(1, 0., 10.)}}) == std::vector<size_t>{1, 1});
}

// ---- sliced parts ---------------------------------------------------------------------------------------------------

// The lines across a round disc end against its wall in one line shorter than the rest. Where that line came out
// under 4 mm it was taken from the lines and laid by itself as a narrow area, a pass of 3 to 4 mm against the wall.
TEST_CASE("A round disc has no narrow area in its solid infill", "[NarrowSolidInfill]")
{
    const DynamicPrintConfig config             = narrow_config("rectilinear");
    size_t                   layers_with_narrow = 0, layers = 0;
    for (double radius = 8.; radius < 14.01; radius += 0.5) {
        Print print;
        Slic3r::Test::init_and_process_print({make_cylinder(radius, 2.)}, print, config);
        std::map<size_t, size_t> lines, narrow;
        for (const SolidArea& area : solid_areas(print))
            if (area.layer == 1 || area.layer == 2 || area.layer == 8)
                ++(area.narrow ? narrow : lines)[area.layer];
        INFO("radius " << radius << " mm");
        // All three layers have lines.
        CHECK(lines.size() == 3);
        CHECK(narrow.empty());
        layers += lines.size();
        layers_with_narrow += narrow.size();
    }
    INFO(layers_with_narrow << " of " << layers << " solid layers have a narrow area");
    CHECK(layers_with_narrow == 0);
}

// Over a cube the lines run out into two corners. In the corner the filter comes to last the short lines after the
// last long one are a run that began beside it: the first of them stays a line and the rest is the narrow area.
TEST_CASE("The short lines after the first in the corner a cube's lines end in are a narrow area", "[NarrowSolidInfill]")
{
    Print print;
    Slic3r::Test::init_and_process_print({make_cube(20., 20., 2.)}, print, narrow_config("rectilinear"));
    size_t layers = 0;
    for (size_t layer : {size_t(1), size_t(2), size_t(8)}) {
        std::vector<const SolidArea*> lines, narrow;
        const std::vector<SolidArea>  areas = solid_areas(print);
        for (const SolidArea& area : areas)
            if (area.layer == layer)
                (area.narrow ? narrow : lines).push_back(&area);
        INFO("layer " << layer);
        REQUIRE(lines.size() == 1);
        REQUIRE(narrow.size() == 1);
        // The lines as they are laid, from one corner to the other: the short ones before the first long one and
        // after the last.
        const std::vector<double> strokes = diagonal_strokes(*lines.front());
        REQUIRE(strokes.size() > 20);
        size_t at_start = 0, at_end = 0;
        while (at_start < strokes.size() && strokes[at_start] < 4.)
            ++at_start;
        while (at_end < strokes.size() && strokes[strokes.size() - 1 - at_end] < 4.)
            ++at_end;
        INFO("short lines at one end " << at_start << ", at the other " << at_end << "; narrow area " << narrow.front()->length()
                                       << " mm in " << narrow.front()->paths.size() << " paths");
        // One corner keeps its short lines, which lead into the long ones. In the other one of the filter's lines
        // is left, which the pattern, laying its own lines half a spacing from where the filter counted them, fills
        // with two (three were left where the filter kept two).
        CHECK(std::min(at_start, at_end) == 2);
        CHECK(std::max(at_start, at_end) >= 3);
        ++layers;
    }
    CHECK(layers == 3);
}

