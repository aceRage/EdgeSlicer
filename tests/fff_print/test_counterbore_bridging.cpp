// Counterbore hole bridging (counterbore_hole_bridging, PerimeterGenerator::process_no_bridge): a hole that narrows on
// the way up has, on the first layer of its narrow part, the wide hole under it. What of that layer has nothing under
// it is taken out of what gets walls and laid as a bridge across the wide hole ("partiallybridge": only what can be
// spanned; "sacrificiallayer": all of it, the narrow hole covered as well). Two errors the Rust port of this code
// turned up:
//  * the option asked only whether the part has a hole somewhere on the layer, not whether what has nothing under it
//    has a hole on its edge, so any hole anywhere made a bridged deck or a table top be treated as a counterbore;
//  * the sacrificial layer is made only over an outline that is convex, and a corner counted as concave however
//    little it turned the wrong way: a point left a nanometre inside a straight side was enough.

#include <catch2/catch.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A slab from x0, y0 to x1, y1 between two heights, with a square hole of side `hole` about cx, cy: four boxes.
void slab_with_hole(std::vector<std::array<double, 6>>& out,
                    double                              x0,
                    double                              y0,
                    double                              x1,
                    double                              y1,
                    double                              z0,
                    double                              z1,
                    double                              cx,
                    double                              cy,
                    double                              hole)
{
    const double h = hole / 2.;
    out.push_back({x0, y0, z0, cx - h, y1, z1});
    out.push_back({cx + h, y0, z0, x1, y1, z1});
    out.push_back({cx - h, y0, z0, cx + h, cy - h, z1});
    out.push_back({cx - h, cy + h, z0, cx + h, y1, z1});
}

// One mesh of several boxes, each given by two opposite corners in mm.
TriangleMesh boxes(const std::vector<std::array<double, 6>>& corners)
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

// Two posts 10 mm square and 2 mm high, 30 mm apart, and a deck 1 mm thick across them, with a hole 3 mm square
// through the deck over the middle of the first post. Nothing narrows: the hole has the post under it.
TriangleMesh deck_with_hole_over_post()
{
    std::vector<std::array<double, 6>> parts{{0., 0., 0., 10., 10., 2.}, {40., 0., 0., 50., 10., 2.}};
    slab_with_hole(parts, 0., 0., 50., 10., 2., 3., 5., 5., 3.);
    return boxes(parts);
}

// The same deck with no hole.
TriangleMesh deck() { return boxes({{0., 0., 0., 10., 10., 2.}, {40., 0., 0., 50., 10., 2.}, {0., 0., 2., 50., 10., 3.}}); }

// A table: four legs 8 mm square and 2 mm high under the corners of a top 40 mm square and 1 mm thick, with a hole
// 3 mm square through the top over the middle of one leg.
TriangleMesh table_with_hole_over_leg()
{
    std::vector<std::array<double, 6>> parts{{0., 0., 0., 8., 8., 2.},
                                             {32., 0., 0., 40., 8., 2.},
                                             {0., 32., 0., 8., 40., 2.},
                                             {32., 32., 0., 40., 40., 2.}};
    slab_with_hole(parts, 0., 0., 40., 40., 2., 3., 4., 4., 3.);
    return boxes(parts);
}

// A block 20 mm square with a hole up through its middle that is 10 mm square for the first 2 mm and 4 mm square for
// the 1 mm above: a counterbore printed wide end down.
TriangleMesh counterbore()
{
    std::vector<std::array<double, 6>> parts;
    slab_with_hole(parts, 0., 0., 20., 20., 0., 2., 10., 10., 10.);
    slab_with_hole(parts, 0., 0., 20., 20., 2., 3., 10., 10., 4.);
    return boxes(parts);
}

// A round tube standing on the plate from `z0` to `z1`, its axis through the origin: outer and inner radius in mm.
indexed_triangle_set tube(double outer, double inner, double z0, double z1, int sides = 90)
{
    indexed_triangle_set its;
    for (int i = 0; i < sides; ++i) {
        const double a = 2. * M_PI * double(i) / double(sides);
        const float  c = float(std::cos(a)), s = float(std::sin(a));
        its.vertices.emplace_back(float(outer) * c, float(outer) * s, float(z0));
        its.vertices.emplace_back(float(outer) * c, float(outer) * s, float(z1));
        its.vertices.emplace_back(float(inner) * c, float(inner) * s, float(z0));
        its.vertices.emplace_back(float(inner) * c, float(inner) * s, float(z1));
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
    return its;
}

// The same in the round: a boss 24 mm across with a hole 12 mm across for the first 2 mm and 5 mm across above.
TriangleMesh round_counterbore()
{
    indexed_triangle_set all = tube(12., 6., 0., 2.);
    its_merge(all, tube(12., 2.5, 2., 3.));
    return TriangleMesh(std::move(all));
}

// A 0.4 mm nozzle, 0.2 mm layers, every line 0.45 mm wide, two walls, three solid layers at the bottom and the top.
DynamicPrintConfig counterbore_config(const char* mode, const char* wall_generator = "classic")
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
        {"wall_generator", wall_generator},
        {"top_shell_layers", "3"},
        {"bottom_shell_layers", "3"},
        {"top_shell_thickness", "0"},
        {"bottom_shell_thickness", "0"},
        {"sparse_infill_density", "15%"},
        {"counterbore_hole_bridging", mode},
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

// What a file lays on each layer (0 the first), in mm of moves, by the ";TYPE:" it is laid under. For a file written
// with relative E and no arcs.
using Layers = std::vector<std::map<std::string, double>>;

Layers laid_by_layer(const std::string& gcode)
{
    Layers             out;
    std::istringstream in(gcode);
    std::string        line, type;
    Vec2d              pos{0., 0.};
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
        if (has_xy && e > 0. && !out.empty())
            out.back()[type] += (to - pos).norm();
        pos = to;
    }
    return out;
}

double of(const std::map<std::string, double>& layer, const std::string& type)
{
    const auto it = layer.find(type);
    return it == layer.end() ? 0. : it->second;
}

// The walls of a layer, whatever they are laid as.
double walls(const std::map<std::string, double>& layer)
{
    return of(layer, "Outer wall") + of(layer, "Inner wall") + of(layer, "Overhang wall");
}

// Everything a layer lays.
double all(const std::map<std::string, double>& layer)
{
    double out = 0.;
    for (const auto& [type, length] : layer)
        out += length;
    return out;
}

Layers slice_layers(const TriangleMesh& mesh, const char* mode, const char* wall_generator = "classic")
{
    return laid_by_layer(Slic3r::Test::slice({mesh}, counterbore_config(mode, wall_generator)));
}

// How many layers of two files do not lay the same length of every kind, to a hundredth of a millimetre.
size_t layers_that_differ(const Layers& a, const Layers& b)
{
    size_t out = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
    for (size_t layer = 0; layer < std::min(a.size(), b.size()); ++layer) {
        bool same = a[layer].size() == b[layer].size();
        for (const auto& [type, length] : a[layer])
            same = same && std::abs(of(b[layer], type) - length) < 0.01;
        out += same ? 0 : 1;
    }
    return out;
}

// The first layer of what stands on posts 2 mm high.
constexpr size_t DECK_LAYER = 10;

// A square of side 10 mm with one more point in the middle of its first side, `off` units (nanometres) inside the
// side: the point its outline starts with.
Polygon square_starting_mid_side(coord_t off)
{
    const coord_t side = scaled<coord_t>(10.);
    return Polygon({{side / 2, off}, {side, 0}, {side, side}, {0, side}, {0, 0}});
}

// A hexagon 10 mm across its flats, flat side down, starting the same way.
Polygon hexagon_starting_mid_side(coord_t off)
{
    Points       points;
    const double r = 5. / std::cos(M_PI / 6.);
    points.emplace_back(0, scaled<coord_t>(-5.) + off);
    for (int i = 0; i < 6; ++i) {
        const double a = -M_PI / 3. + i * M_PI / 3.;
        points.emplace_back(scaled<coord_t>(r * std::cos(a)), scaled<coord_t>(r * std::sin(a)));
    }
    return Polygon(points);
}

} // namespace

// "keep out if cannot be bridged or no holes to bridge": the second half of that was asked of the whole region,
// `!surface->expolygon.holes.empty()`. A deck across two posts is a bridge and no counterbore; with a hole drilled
// through it over one post it was taken for one. Its first layer then lost its walls, and with "partiallybridge"
// whatever of it the bridge detector's lines do not span was not laid at all.
TEST_CASE("A hole over a post does not make a counterbore of the deck across two posts", "[CounterboreBridging]")
{
    const TriangleMesh part = deck_with_hole_over_post();
    for (const char* wall_generator : {"classic", "arachne"}) {
        const Layers off = slice_layers(part, "none", wall_generator);
        REQUIRE(off.size() == 15);
        // With the option off the first layer of the deck is walls round the deck and the hole and a bridge.
        REQUIRE(walls(off[DECK_LAYER]) > 200.);
        REQUIRE(of(off[DECK_LAYER], "Bridge") > 500.);
        for (const char* mode : {"partiallybridge", "sacrificiallayer"}) {
            const Layers on = slice_layers(part, mode, wall_generator);
            INFO(wall_generator << " walls, " << mode << ": the deck's first layer has " << walls(on[DECK_LAYER]) << " mm of walls and "
                                << of(on[DECK_LAYER], "Bridge") << " mm of bridge; with the option off " << walls(off[DECK_LAYER])
                                << " mm and " << of(off[DECK_LAYER], "Bridge") << " mm");
            REQUIRE(on.size() == off.size());
            CHECK_THAT(walls(on[DECK_LAYER]), WithinAbs(walls(off[DECK_LAYER]), 0.01));
            CHECK_THAT(of(on[DECK_LAYER], "Bridge"), WithinAbs(of(off[DECK_LAYER], "Bridge"), 0.01));
            // And nothing else in the file is touched.
            CHECK(layers_that_differ(on, off) == 0);
        }
    }
}

TEST_CASE("A hole over a leg does not make a counterbore of a table top", "[CounterboreBridging]")
{
    const TriangleMesh part = table_with_hole_over_leg();
    for (const char* wall_generator : {"classic", "arachne"}) {
        const Layers off = slice_layers(part, "none", wall_generator);
        REQUIRE(off.size() == 15);
        REQUIRE(of(off[DECK_LAYER], "Bridge") > 2000.);
        for (const char* mode : {"partiallybridge", "sacrificiallayer"}) {
            const Layers on = slice_layers(part, mode, wall_generator);
            INFO(wall_generator << " walls, " << mode << ": the top's first layer has " << walls(on[DECK_LAYER]) << " mm of walls and "
                                << of(on[DECK_LAYER], "Bridge") << " mm of bridge, " << all(on[DECK_LAYER])
                                << " mm in all; with the option off " << walls(off[DECK_LAYER]) << " mm, " << of(off[DECK_LAYER], "Bridge")
                                << " mm and " << all(off[DECK_LAYER]) << " mm");
            REQUIRE(on.size() == off.size());
            CHECK_THAT(walls(on[DECK_LAYER]), WithinAbs(walls(off[DECK_LAYER]), 0.01));
            CHECK_THAT(of(on[DECK_LAYER], "Bridge"), WithinAbs(of(off[DECK_LAYER], "Bridge"), 0.01));
            CHECK(layers_that_differ(on, off) == 0);
        }
    }
}

// With no hole in it the deck was left alone before as well.
TEST_CASE("A deck with no hole is laid the same with counterbore bridging on", "[CounterboreBridging]")
{
    const TriangleMesh part = deck();
    const Layers       off  = slice_layers(part, "none");
    REQUIRE(off.size() == 15);
    for (const char* mode : {"partiallybridge", "sacrificiallayer"})
        CHECK(layers_that_differ(slice_layers(part, mode), off) == 0);
}

// The option still does what it is for.
TEST_CASE("A hole that narrows from the layer below is still bridged", "[CounterboreBridging]")
{
    struct Part
    {
        const char*  name;
        TriangleMesh mesh;
        // How much longer the walls of the layer are round the wide hole than round the narrow one, at the least.
        double more_wall;
    };
    // Square: 16 mm round the narrow hole, 40 mm round the wide one, two walls. Round: 15.7 mm and 37.7 mm.
    const Part parts[] = {{"square in square", counterbore(), 30.}, {"round in round", round_counterbore(), 30.}};
    for (const Part& part : parts)
        for (const char* wall_generator : {"classic", "arachne"}) {
            const Layers off = slice_layers(part.mesh, "none", wall_generator);
            REQUIRE(off.size() == 15);
            const Layers partly      = slice_layers(part.mesh, "partiallybridge", wall_generator);
            const Layers sacrificial = slice_layers(part.mesh, "sacrificiallayer", wall_generator);
            REQUIRE(partly.size() == 15);
            REQUIRE(sacrificial.size() == 15);
            INFO(part.name << ", " << wall_generator << " walls, first layer of the narrow hole: walls " << walls(off[DECK_LAYER]) << " / "
                           << walls(partly[DECK_LAYER]) << " / " << walls(sacrificial[DECK_LAYER]) << " mm, of them over air "
                           << of(off[DECK_LAYER], "Overhang wall") << " / " << of(partly[DECK_LAYER], "Overhang wall") << " / "
                           << of(sacrificial[DECK_LAYER], "Overhang wall") << " mm, bridge " << of(off[DECK_LAYER], "Bridge") << " / "
                           << of(partly[DECK_LAYER], "Bridge") << " / " << of(sacrificial[DECK_LAYER], "Bridge")
                           << " mm (off / partiallybridge / sacrificiallayer)");
            // Off: walls round the narrow hole, laid over the wide one.
            CHECK(of(off[DECK_LAYER], "Overhang wall") > 25.);
            // On: the walls go round the wide hole and none is laid over air.
            CHECK(walls(partly[DECK_LAYER]) > walls(off[DECK_LAYER]) + part.more_wall);
            CHECK(walls(sacrificial[DECK_LAYER]) > walls(off[DECK_LAYER]) + part.more_wall);
            CHECK_THAT(of(partly[DECK_LAYER], "Overhang wall"), WithinAbs(0., 0.01));
            CHECK_THAT(of(sacrificial[DECK_LAYER], "Overhang wall"), WithinAbs(0., 0.01));
            // The sacrificial layer covers the wide hole, the narrow one with it; partly bridged a slot is left open.
            CHECK(of(partly[DECK_LAYER], "Bridge") > 50.);
            CHECK(of(sacrificial[DECK_LAYER], "Bridge") > of(partly[DECK_LAYER], "Bridge") + 30.);
        }
}

// The sacrificial layer is laid only over an outline that is convex ("check convex, has some bridge, not overhang"),
// asked of the outline once simplified. Simplifying keeps the outline's first point whatever it is, and a first point
// in the middle of a straight side, a nanometre to the inside of it, is a corner that turns the wrong way.
TEST_CASE("A point a nanometre inside a straight side does not make an outline concave", "[CounterboreBridging]")
{
    const double spacing = scaled<double>(0.45);
    for (coord_t off : {coord_t(-1), coord_t(0), coord_t(1), coord_t(50)}) {
        INFO("the first point " << off << " nm inside its side");
        for (const Polygon& outline : {square_starting_mid_side(off), hexagon_starting_mid_side(off)}) {
            REQUIRE(outline.is_counter_clockwise());
            const Polygons simplified = outline.simplify(spacing);
            REQUIRE(simplified.size() == 1);
            // What made the old test fail: the point is still there, and it is a concave one.
            if (off > 0) {
                CHECK(simplified.front().points.size() == outline.points.size());
                CHECK(simplified.front().concave_points().size() == 1);
            }
            CHECK(counterbore_outline_is_convex(simplified.front()));
        }
    }
    // A corner that turns the wrong way by more than a tenth of a micron is one.
    CHECK_FALSE(counterbore_outline_is_convex(square_starting_mid_side(scaled<coord_t>(0.001))));
    // An L is not convex.
    const coord_t u = scaled<coord_t>(5.);
    CHECK_FALSE(counterbore_outline_is_convex(Polygon({{0, 0}, {2 * u, 0}, {2 * u, u}, {u, u}, {u, 2 * u}, {0, 2 * u}})));
    // A square and a hexagon are.
    CHECK(counterbore_outline_is_convex(Polygon({{0, 0}, {u, 0}, {u, u}, {0, u}})));
    CHECK(counterbore_outline_is_convex(hexagon_starting_mid_side(0)));
}
