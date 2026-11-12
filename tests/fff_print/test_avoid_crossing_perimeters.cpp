// "Avoid crossing walls" (reduce_crossing_wall): a travel that would cross a wall goes round the inside of the
// walls instead. Five errors the Rust port of this code turned up:
//  * the shorter way round a ring was chosen from lengths that had parts of the two edges met taken off them,
//    so some detours went the longer way;
//  * the boundary for moves inside a part was made on a layer's first such move and kept, and where that move
//    was one between supports the part's own moves crossed its top surfaces;
//  * a move that is to go straight (from the first layer's skirt, or from a brim) was routed after all when
//    the retract before it wiped;
//  * a point met exactly at the start of an edge was left on the ring instead of stepped off it;
//  * the width of a part across from the edge that closes its outline was judged from the wrong place.

#include <catch2/catch.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/EdgeGrid.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/GCode/AvoidCrossingPerimeters.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace Slic3r {
// Defined in GCode/AvoidCrossingPerimeters.cpp.
void               init_boundary(AvoidCrossingPerimeters::Boundary *boundary, Polygons &&boundary_polygons);
size_t             avoid_perimeters(const AvoidCrossingPerimeters::Boundary &boundary,
                                    const Point                             &start,
                                    const Point                             &end,
                                    const Layer                             &layer,
                                    Polyline                                &result_out);
Point              get_middle_point_offset(const Polygon &polygon,
                                           const size_t   left_idx,
                                           const size_t   right_idx,
                                           const Point   &middle,
                                           const coord_t  offset);
void               precompute_polygon_distances(const Polygon &polygon, std::vector<float> &polygon_distances_out);
std::vector<float> contour_distance(const EdgeGrid::Grid     &grid,
                                    const std::vector<float> &poly_distances,
                                    const size_t              contour_idx,
                                    const Polygon            &polygon,
                                    double                    compensation,
                                    double                    search_radius);
} // namespace Slic3r

namespace {

Point mm(double x, double y) { return Point(scaled<coord_t>(x), scaled<coord_t>(y)); }

// One mesh of several boxes, each given by two opposite corners in mm. Boxes that are to be one body overlap.
TriangleMesh boxes(std::initializer_list<std::array<double, 6>> corners)
{
    indexed_triangle_set all;
    for (const std::array<double, 6> &c : corners) {
        indexed_triangle_set box = its_make_cube(c[3] - c[0], c[4] - c[1], c[5] - c[2]);
        for (stl_vertex &v : box.vertices)
            v += Vec3f(float(c[0]), float(c[1]), float(c[2]));
        its_merge(all, box);
    }
    return TriangleMesh(std::move(all));
}

// A move of the file in the plane.
struct Move
{
    enum Kind { Travel, Extrude, Wipe };
    Kind        kind = Travel;
    double      z    = 0.;
    Vec2d       from{0., 0.};
    Vec2d       to{0., 0.};
    std::string type; // the ";TYPE:" it was made under
};

// Reads the moves of a file written with relative E.
std::vector<Move> moves_of(const std::string &gcode)
{
    std::vector<Move>  out;
    std::istringstream in(gcode);
    std::string        line, type;
    Vec2d              pos{0., 0.};
    double             z       = 0.;
    bool               in_wipe = false;
    while (std::getline(in, line)) {
        if (line.rfind(";WIPE_START", 0) == 0 || line.rfind(";WIPE_END", 0) == 0) {
            in_wipe = line.rfind(";WIPE_START", 0) == 0;
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
        const std::string  code = line.substr(0, line.find(';'));
        std::istringstream words(code);
        std::string        word;
        Vec2d              to     = pos;
        bool               has_xy = false, has_e = false;
        double             e      = 0.;
        while (words >> word) {
            if (word.size() < 2)
                continue;
            const double value = std::atof(word.c_str() + 1);
            switch (word.front()) {
            case 'X': to.x() = value; has_xy = true; break;
            case 'Y': to.y() = value; has_xy = true; break;
            case 'Z': z = value; break;
            case 'E': e = value; has_e = true; break;
            default: break;
            }
        }
        if (!has_xy)
            continue;
        Move move;
        move.kind = has_e && e > 0. ? Move::Extrude : in_wipe ? Move::Wipe : Move::Travel;
        move.z    = z;
        move.from = pos;
        move.to   = to;
        move.type = type;
        out.emplace_back(std::move(move));
        pos = to;
    }
    return out;
}

// The way the nozzle takes from the end of one extrusion to the start of the next.
struct Hop
{
    double             z = 0.;          // of the extrusion it leads to
    std::string        from_type;       // what was laid before it
    std::string        to_type;         // what is laid after it
    size_t             wipes = 0;       // moves of the wipe that comes before the travel
    std::vector<Vec2d> points;          // the travel: where it starts (after the wipe), each point on the way, where it ends
};

std::vector<Hop> hops_of(const std::vector<Move> &moves)
{
    std::vector<Hop> out;
    bool             open = false, seen_extrusion = false;
    std::string      last_type;
    for (const Move &move : moves) {
        if (move.kind == Move::Extrude) {
            if (open) {
                out.back().z       = move.z;
                out.back().to_type = move.type;
                open               = false;
            }
            seen_extrusion = true;
            last_type      = move.type;
            continue;
        }
        if (!seen_extrusion)
            continue;
        if (!open) {
            out.emplace_back();
            out.back().from_type = last_type;
            open                 = true;
        }
        Hop &hop = out.back();
        if (move.kind == Move::Wipe) {
            ++hop.wipes;
            hop.points.clear();
        } else {
            if (hop.points.empty())
                hop.points.emplace_back(move.from);
            hop.points.emplace_back(move.to);
        }
    }
    if (open)
        out.pop_back();
    return out;
}

// The hop's way, or the straight line between its ends, in the object's own scaled coordinates.
Polyline in_object(const Hop &hop, const Point &shift, bool straight = false)
{
    Polyline out;
    for (const Vec2d &p : hop.points)
        out.points.emplace_back(mm(p.x(), p.y()) - shift);
    if (straight && out.points.size() > 2)
        out.points = {out.points.front(), out.points.back()};
    return out;
}

bool crosses(const Polyline &way, const ExPolygons &area) { return !intersection_pl(Polylines{way}, area).empty(); }

bool holds(const ExPolygons &area, const Point &point)
{
    for (const ExPolygon &ex : area)
        if (ex.contains(point))
            return true;
    return false;
}

double length_mm(const Polyline &way) { return unscale<double>(way.length()); }

// A layer to ask the spacing of the walls of: the first of a sliced 20 mm cube.
struct SlicedCube
{
    Print print;
    SlicedCube()
    {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"nozzle_diameter", "0.4"}, {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}});
        Test::init_and_process_print({Test::TestMesh::cube_20x20x20}, print, config);
    }
    const Layer &layer() const { return *print.objects().front()->layers().front(); }
};

DynamicPrintConfig routed_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"nozzle_diameter", "0.4"},
        {"wall_generator", "classic"},
        {"wall_loops", "2"},
        {"enable_arc_fitting", "0"},
        {"use_relative_e_distances", "1"},
        {"reduce_crossing_wall", "1"},
        {"max_travel_detour_distance", "0"},
        {"z_hop", "0"},
        {"retraction_length", "0.8"},
        {"brim_type", "no_brim"},
    });
    return config;
}

} // namespace

TEST_CASE("A detour takes the shorter way round a ring", "[AvoidCrossingPerimeters]")
{
    // A plate with a hole 20 mm by 10 mm in it, as a boundary holds them: the hole clockwise, the area travel keeps
    // to on its left. The hole's last edge is the long one along the bottom, from (20, 0) back to (0, 0).
    AvoidCrossingPerimeters::Boundary boundary;
    init_boundary(&boundary, Polygons{Polygon{mm(-10, -10), mm(30, -10), mm(30, 20), mm(-10, 20)},
                                      Polygon{mm(0, 0), mm(0, 10), mm(20, 10), mm(20, 0)}});
    const SlicedCube cube;

    // A move that meets the bottom edge 2 mm from its left end and the right edge half way up. Round by the
    // bottom right corner it is 18 + 5 = 23 mm along the ring, round by the two left corners 2 + 10 + 20 + 5 = 37.
    // The lengths were worked out right and then had taken off them what lies between each meeting and the ends
    // of its edge, 18 + 5 from the long way and 2 + 5 from the short one, which made the long way the shorter.
    const Point below = mm(2. - 7.2, -2.), right = mm(20. + 7.2, 7.);
    const double short_way = (Vec2d(20., 0.) - Vec2d(-5.2, -2.)).norm() + (Vec2d(27.2, 7.) - Vec2d(20., 0.)).norm();

    SECTION("from below to the right")
    {
        Polyline way;
        REQUIRE(avoid_perimeters(boundary, below, right, cube.layer(), way) == 2);
        // Straightened, the short way is two lines by the bottom right corner.
        CHECK_THAT(length_mm(way), WithinAbs(short_way, 0.01));
        for (const Point &p : way.points)
            CHECK(unscale<double>(p.y()) < 7.01);
    }
    SECTION("and back")
    {
        Polyline way;
        REQUIRE(avoid_perimeters(boundary, right, below, cube.layer(), way) == 2);
        CHECK_THAT(length_mm(way), WithinAbs(short_way, 0.01));
        for (const Point &p : way.points)
            CHECK(unscale<double>(p.y()) < 7.01);
    }
    SECTION("a move across the left end still goes round by the left")
    {
        // Meets the bottom edge 2 mm from its left end and the left edge 2 mm up: 4 mm round by the corner.
        Polyline way;
        REQUIRE(avoid_perimeters(boundary, mm(2. + 3., -3.), mm(-3., 2. + 3.), cube.layer(), way) == 2);
        CHECK_THAT(length_mm(way), WithinAbs(2. * (Vec2d(5., -3.) - Vec2d(0., 0.)).norm(), 0.01));
    }
}

TEST_CASE("A point met at the start of an edge is stepped off the ring", "[AvoidCrossingPerimeters]")
{
    // Hardening: nothing in a print shows this. A point of a detour is put a tenth of a micrometre to the
    // ring's left, by the two edges that meet at it. For a point met exactly at a corner as the start of the
    // edge leaving it, the search back for the edge before took its first step forward and found the far end
    // of that same edge on both sides; the two normals cancelled and the point stayed on the ring.
    const Polygon square{mm(0, 0), mm(10, 0), mm(10, 10), mm(0, 10)};
    const coord_t step = coord_t(SCALED_EPSILON);
    for (size_t corner = 0; corner < 4; ++corner) {
        const size_t before = (corner + 3) % 4, after = (corner + 1) % 4;
        const Point &at = square.points[corner];
        // Met as the end of the edge that arrives at the corner, and as the start of the one that leaves it.
        const Point as_end   = get_middle_point_offset(square, before, corner, at, step);
        const Point as_start = get_middle_point_offset(square, corner, after, at, step);
        INFO("corner " << corner);
        CHECK(as_end != at);
        CHECK(as_start == as_end);
        CHECK(square.contains(as_start));
    }
}

TEST_CASE("Width across from the edge that closes an outline", "[AvoidCrossingPerimeters]")
{
    // Hardening: nothing in a print shows this. Before an outline is drawn in, how wide the part is at each of
    // its points is measured to the nearest edge across from it. An edge of the point's own outline counts only
    // if it is far enough round the outline, and how far round was worked out without the start of the edge that
    // closes the outline (from its last point back to its first), as if that edge began where the outline does.
    //
    // A strip 10 mm long and 0.5 mm wide. The point half way along its bottom edge has the top edge across from
    // it, 0.5 mm off and 10.5 mm round the outline either way.
    const double compensation = scaled<double>(0.6), search_radius = scaled<double>(2.4);
    auto width_at_bottom_middle = [&](const Polygon &strip, size_t idx) {
        ExPolygon   ex(strip);
        BoundingBox bbox = get_extents(ex);
        bbox.offset(SCALED_EPSILON);
        EdgeGrid::Grid grid;
        grid.set_bbox(bbox);
        grid.create(ex, coord_t(0.7 * search_radius));
        std::vector<float> along;
        precompute_polygon_distances(strip, along);
        const std::vector<float> widths = contour_distance(grid, along, 0, strip, compensation, search_radius);
        REQUIRE(widths.size() == strip.size());
        REQUIRE(strip.points[idx] == mm(5, 0));
        return unscale<double>(widths[idx]);
    };

    SECTION("the top edge is an edge like any other")
    {
        CHECK_THAT(width_at_bottom_middle(Polygon{mm(0, 0), mm(5, 0), mm(10, 0), mm(10, 0.5), mm(0, 0.5)}, 1), WithinAbs(0.5, 1e-4));
    }
    SECTION("the top edge closes the outline")
    {
        // The same strip begun at its top left corner. The top edge was taken to be 0.5 mm round the outline
        // from the point, too near to count, and the strip to be as wide there as the search reaches.
        CHECK_THAT(width_at_bottom_middle(Polygon{mm(0, 0.5), mm(0, 0), mm(5, 0), mm(10, 0), mm(10, 0.5)}, 2), WithinAbs(0.5, 1e-4));
    }
}

TEST_CASE("A move from the first layer's skirt or a brim goes straight when the retract wipes", "[AvoidCrossingPerimeters]")
{
    // The move from the first layer's skirt, or from a brim, to the first point of the part is to go straight
    // (AvoidCrossingPerimeters::disable_once). It was planned again, after a retract whose wipe had moved the
    // nozzle, without looking at that.
    //
    // A frame 40 mm square with a hole 20 mm square. The skirt ends in front of it and the walls start at its
    // back, so the straight line from one to the other crosses the hole, which a routed move goes round.
    const TriangleMesh frame = boxes({{0, 0, 0, 10, 40, 1}, {30, 0, 0, 40, 40, 1}, {0, 0, 0, 40, 10, 1}, {0, 30, 0, 40, 40, 1}});

    auto first_hop = [&frame](bool wipe, ExPolygons &holes, const char *from = "Skirt") {
        DynamicPrintConfig config = routed_config();
        config.set_deserialize_strict({
            {"skirt_loops", "1"},
            {"skirt_distance", "3"},
            {"skirt_start_angle", "-90"},
            {"seam_position", "back"},
            {"wipe", wipe ? "1" : "0"},
            {"wipe_distance", "2"},
        });
        if (std::string(from) == "Brim")
            config.set_deserialize_strict({{"skirt_loops", "0"}, {"brim_type", "outer_only"}, {"brim_width", "3"}});
        Print print;
        Model model;
        Test::init_print({frame}, print, model, config);
        const std::string gcode = Test::gcode(print);
        const PrintObject &object = *print.objects().front();
        const Point        shift  = object.instances().front().shift;
        holes.clear();
        for (const ExPolygon &island : object.layers().front()->lslices)
            for (const Polygon &hole : island.holes) {
                Polygon filled = hole;
                filled.make_counter_clockwise();
                holes.emplace_back(filled);
            }
        Hop found;
        for (const Hop &hop : hops_of(moves_of(gcode)))
            if (hop.from_type == from && hop.to_type != from) {
                found = hop;
                break;
            }
        REQUIRE(found.points.size() >= 2);
        REQUIRE_THAT(found.z, WithinAbs(0.2, 1e-6));
        // Into the object's own coordinates, to lay against its hole.
        for (Vec2d &p : found.points)
            p -= unscale(shift);
        return found;
    };

    auto straight_crosses = [](const Hop &hop, const ExPolygons &holes) {
        Polyline line{mm(hop.points.front().x(), hop.points.front().y()), mm(hop.points.back().x(), hop.points.back().y())};
        return crosses(line, offset_ex(holes, -float(scaled<double>(2.))));
    };

    SECTION("without a wipe")
    {
        ExPolygons holes;
        const Hop  hop = first_hop(false, holes);
        REQUIRE(holes.size() == 1);
        CHECK(hop.wipes == 0);
        CHECK(straight_crosses(hop, holes));
        CHECK(hop.points.size() == 2);
    }
    SECTION("with a wipe")
    {
        ExPolygons holes;
        const Hop  hop = first_hop(true, holes);
        REQUIRE(holes.size() == 1);
        CHECK(hop.wipes > 0);
        CHECK(straight_crosses(hop, holes));
        CHECK(hop.points.size() == 2);
    }
    SECTION("from a brim, with a wipe")
    {
        // No skirt and a brim 3 mm wide round the outside of the frame; the same holds for the move away from it.
        ExPolygons holes;
        const Hop  hop = first_hop(true, holes, "Brim");
        REQUIRE(holes.size() == 1);
        CHECK(hop.wipes > 0);
        CHECK(straight_crosses(hop, holes));
        CHECK(hop.points.size() == 2);
    }
}

TEST_CASE("Moves in a part go round its top surfaces on a layer with supports", "[AvoidCrossingPerimeters]")
{
    // The boundary for moves inside a part has the layer's top surfaces left out of it, so that a move that has to
    // be routed goes round them as well. It was made on the layer's first move of that kind and kept for the layer.
    // Supports are printed before the part and their moves are of that kind too, so on a layer with supports the
    // boundary was the one made for the support layer, which has no top surfaces, and the part's own moves crossed
    // the top surfaces they go round on a layer without supports.
    //
    // Two blocks 6 mm high joined by a floor 3 mm high and 20 mm long, with a slot across the floor that leaves a
    // neck 3 mm wide. On the floor's last layer a move from one block to the other meets the slot, so it is routed,
    // and the floor between them is a top surface. A ledge high on one block stands on supports that pass this layer.
    const TriangleMesh part = boxes({
        {0, 0, 0, 10.5, 20, 6},
        {10, 0, 0, 19, 20, 3},
        {18.5, 0, 0, 21.5, 3, 3},
        {21, 0, 0, 30, 20, 3},
        {29.5, 0, 0, 60, 20, 6},
        {59.5, 0, 4.6, 66, 20, 6},
    });
    const double floor_top = 3.0;

    // The moves on the floor's last layer from one extrusion of the part to the next whose straight line would cross
    // the top surface and that neither start nor end across it: how many there are, and how many of them cross it.
    // A move that starts or ends at the edge of the top surface counts: which moves the layer has depends on the
    // order the part is printed in, which is not the same on every compiler (MSVC prints the infill of the two
    // blocks one after the other along the neck's edge, so no move between the blocks meets the slot there, and
    // the move that is routed is the one from the floor's outer wall to its top surface).
    auto moves_over_the_floor = [&part, floor_top](bool supports) {
        DynamicPrintConfig config = routed_config();
        config.set_deserialize_strict({
            {"skirt_loops", "0"},
            {"wipe", "0"},
            {"sparse_infill_density", "20%"},
            {"top_shell_layers", "3"},
            {"bottom_shell_layers", "3"},
            {"enable_support", supports ? "1" : "0"},
            {"support_type", "normal(auto)"},
            {"independent_support_layer_height", "0"},
        });
        Print print;
        Model model;
        Test::init_print({part}, print, model, config);
        const std::string  gcode  = Test::gcode(print);
        const PrintObject &object = *print.objects().front();
        const Point        shift  = object.instances().front().shift;

        const Layer *layer = nullptr;
        for (const Layer *l : object.layers())
            if (std::abs(l->print_z - floor_top) < 1e-4)
                layer = l;
        REQUIRE(layer != nullptr);
        ExPolygons top;
        for (const LayerRegion *region : layer->regions())
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.is_top())
                    top.emplace_back(surface.expolygon);
        top = union_ex(top);
        REQUIRE(!top.empty());
        size_t support_layers = 0;
        for (const SupportLayer *l : object.support_layers())
            if (std::abs(l->print_z - floor_top) < 1e-4 && !l->support_fills.empty())
                ++support_layers;
        REQUIRE(support_layers == (supports ? 1 : 0));

        // A routed move may run along the edge of a top surface, a quarter of a millimetre inside it. Half a
        // millimetre inside it is across it.
        const ExPolygons across = offset_ex(top, -float(scaled<double>(0.5)));
        std::pair<size_t, size_t> counts{0, 0};
        for (const Hop &hop : hops_of(moves_of(gcode))) {
            if (std::abs(hop.z - floor_top) > 1e-4 || hop.points.empty() || hop.from_type.rfind("Support", 0) == 0 ||
                hop.to_type.rfind("Support", 0) == 0)
                continue;
            // From one line of the top surface to the next is a move inside the top surface, not round it.
            if (hop.from_type == "Top surface" && hop.to_type == "Top surface")
                continue;
            const Polyline way = in_object(hop, shift);
            if (holds(across, way.points.front()) || holds(across, way.points.back()) || !crosses(in_object(hop, shift, true), across))
                continue;
            ++counts.first;
            if (crosses(way, across))
                ++counts.second;
        }
        return counts;
    };

    SECTION("without supports")
    {
        const std::pair<size_t, size_t> counts = moves_over_the_floor(false);
        CHECK(counts.first > 0);
        CHECK(counts.second == 0);
    }
    SECTION("with supports")
    {
        const std::pair<size_t, size_t> counts = moves_over_the_floor(true);
        CHECK(counts.first > 0);
        CHECK(counts.second == 0);
    }
}
