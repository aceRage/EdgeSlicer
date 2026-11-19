// Scarf seams (seam_slope_type): the loop is laid whole, its first stretch climbing from the layer beneath and
// its last going on over that stretch, level. Six errors the Rust port of this code turned up:
//  * a scarf of no length (seam_slope_min_length 0) closed the loop with neither a slope nor the seam gap;
//  * an inner wall's slope lost its stagger when seam_gap was 0;
//  * scarf_joint_speed was not looked at on a small loop;
//  * a scarf_joint_speed of 0 laid the joint at the filament's volumetric maximum;
//  * the conditional scarf's overhang threshold was a share of the configured width, 0 when that is automatic;
//  * a slope whose step came to less than one unit split its lines in two without end.

#include <catch2/catch.hpp>

#include <cmath>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A stretch of the file laid without a break: every extruding move from one travel to the next.
struct Run
{
    size_t           layer = 0;      // 0 for the first layer
    std::string      type;           // the ";TYPE:" it was laid under
    Vec2d            first{0., 0.};  // where it starts
    Vec2d            last{0., 0.};   // where it ends
    double           length  = 0.;   // mm, in the plane
    size_t           moves   = 0;
    size_t           rising  = 0;    // moves that say a Z as they lay: the two halves of a scarf joint
    std::set<int>    rising_feedrates; // mm/min
    std::set<int>    level_feedrates;  // mm/min, of the moves that say no Z

    bool   is_wall() const { return type == "Outer wall" || type == "Inner wall"; }
    double gap() const { return (last - first).norm(); }
};

// Reads the extruding runs of a file written with relative E.
std::vector<Run> runs_of(const std::string &gcode)
{
    std::vector<Run>   out;
    std::istringstream in(gcode);
    std::string        line, type;
    Vec2d              pos{0., 0.};
    double             feedrate = 0.;
    size_t             layers   = 0;
    bool               open     = false;
    while (std::getline(in, line)) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0) {
            ++layers;
            open = false;
            continue;
        }
        if (line.rfind(";TYPE:", 0) == 0) {
            type = line.substr(6);
            while (!type.empty() && (type.back() == '\r' || type.back() == ' '))
                type.pop_back();
            open = false;
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        const std::string  code = line.substr(0, line.find(';'));
        std::istringstream words(code);
        std::string        word;
        Vec2d              to    = pos;
        bool               has_xy = false, has_z = false, has_e = false;
        double             e = 0.;
        while (words >> word) {
            if (word.size() < 2)
                continue;
            const double value = std::atof(word.c_str() + 1);
            switch (word.front()) {
            case 'X': to.x() = value; has_xy = true; break;
            case 'Y': to.y() = value; has_xy = true; break;
            case 'Z': has_z = true; break;
            case 'E': e = value; has_e = true; break;
            case 'F': feedrate = value; break;
            default: break;
            }
        }
        if (!has_xy)
            continue;
        if (has_e && e > 0. && layers > 0) {
            if (!open) {
                out.emplace_back();
                out.back().layer = layers - 1;
                out.back().type  = type;
                out.back().first = pos;
                open             = true;
            }
            Run &run = out.back();
            run.length += (to - pos).norm();
            run.last = to;
            ++run.moves;
            if (has_z) {
                ++run.rising;
                run.rising_feedrates.insert(int(std::lround(feedrate)));
            } else
                run.level_feedrates.insert(int(std::lround(feedrate)));
        } else
            open = false;
        pos = to;
    }
    return out;
}

size_t rising_moves(const std::vector<Run> &runs)
{
    size_t n = 0;
    for (const Run &run : runs)
        n += run.rising;
    return n;
}

// How many of the runs are loops joined with a scarf.
size_t scarfed(const std::vector<Run> &runs)
{
    size_t n = 0;
    for (const Run &run : runs)
        n += run.rising > 0 ? 1 : 0;
    return n;
}

// One wall and nothing else, so that a layer is a loop: no skirt, no fill, no top or bottom.
DynamicPrintConfig walls_only_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "nozzle_diameter", "0.4" },
        { "wall_generator", "classic" },
        { "wall_loops", "1" },
        { "skirt_loops", "0" },
        { "brim_type", "no_brim" },
        { "enable_arc_fitting", "0" },
        { "sparse_infill_density", "0%" },
        { "top_shell_layers", "0" },
        { "bottom_shell_layers", "0" },
        { "use_relative_e_distances", "1" },
        { "seam_gap", "0.2" },
        // Nothing but the settings under test is to set a wall's speed: no cooling slowdown, and a hot end that
        // can melt whatever is asked of it (the 2 mm3/s default would cap every wall).
        { "slow_down_for_layer_cooling", "0" },
        { "filament_max_volumetric_speed", "50" },
        { "outer_wall_speed", "100" },
        { "inner_wall_speed", "100" },
    });
    return config;
}

std::vector<Run> wall_runs(TriangleMesh mesh, const DynamicPrintConfig &config, size_t from_layer = 1)
{
    Print print;
    Model model;
    Test::init_print({ std::move(mesh) }, print, model, config);
    std::vector<Run> walls;
    for (Run &run : runs_of(Test::gcode(print)))
        if (run.is_wall() && run.layer >= from_layer)
            walls.emplace_back(std::move(run));
    return walls;
}

// A round post that widens as it rises, `sides` flat sides round.
TriangleMesh widening_post(double r_bottom, double r_top, double height, int sides)
{
    indexed_triangle_set its;
    its.vertices.emplace_back(0.f, 0.f, 0.f);
    its.vertices.emplace_back(0.f, 0.f, float(height));
    for (int i = 0; i < sides; ++i) {
        const double a = 2. * PI * double(i) / double(sides);
        its.vertices.emplace_back(float(r_bottom * std::cos(a)), float(r_bottom * std::sin(a)), 0.f);
        its.vertices.emplace_back(float(r_top * std::cos(a)), float(r_top * std::sin(a)), float(height));
    }
    for (int i = 0; i < sides; ++i) {
        const int j  = (i + 1) % sides;
        const int bi = 2 + 2 * i, ti = 3 + 2 * i, bj = 2 + 2 * j, tj = 3 + 2 * j;
        its.indices.emplace_back(0, bj, bi);
        its.indices.emplace_back(1, ti, tj);
        its.indices.emplace_back(bi, bj, tj);
        its.indices.emplace_back(bi, tj, ti);
    }
    return TriangleMesh(std::move(its));
}

ExtrusionPaths straight_wall(ExtrusionRole role, double length_mm)
{
    ExtrusionPath path{role, 1.0, 0.45f, 0.2f};
    for (double x = 0.; x < length_mm + 0.5; x += 5.)
        path.polyline.append(Point::new_scale(x, 0.));
    return {path};
}

double length_mm(const std::vector<ExtrusionPathSloped> &paths)
{
    double length = 0.;
    for (const ExtrusionPathSloped &path : paths)
        length += unscale_(path.length());
    return length;
}

} // namespace

TEST_CASE("A scarf of no length is a plain seam with its gap", "[ScarfSeam]")
{
    // seam_slope_min_length 0 "effectively disables the scarf" (its tooltip). The loop was taken for one that
    // gets a scarf all the same, so it was not clipped by the seam gap, and then given a slope of no length: laid
    // whole, its end on its start.
    SECTION("seam_slope_type external")
    {
        DynamicPrintConfig config = walls_only_config();
        config.set_deserialize_strict({ { "seam_slope_type", "none" } });
        const std::vector<Run> plain = wall_runs(Test::mesh(Test::TestMesh::cube_20x20x20), config);

        config.set_deserialize_strict({ { "seam_slope_type", "external" }, { "seam_slope_min_length", "0" } });
        const std::vector<Run> none_long = wall_runs(Test::mesh(Test::TestMesh::cube_20x20x20), config);

        // 99 layers above the first, one loop of 4 * (20 - 0.45) mm less the gap on each.
        REQUIRE(plain.size() == 99);
        REQUIRE(none_long.size() == plain.size());
        for (size_t i = 0; i < plain.size(); ++i) {
            CAPTURE(i);
            CHECK_THAT(plain[i].gap(), WithinAbs(0.2, 0.005));
            CHECK_THAT(none_long[i].gap(), WithinAbs(0.2, 0.005));
            CHECK_THAT(none_long[i].length, WithinAbs(plain[i].length, 0.005));
            CHECK(none_long[i].rising == 0);
        }

        // With the whole loop a slope the length is not asked, and the scarf stays.
        config.set_deserialize_strict({ { "seam_slope_entire_loop", "1" } });
        CHECK(rising_moves(wall_runs(Test::mesh(Test::TestMesh::cube_20x20x20), config)) > 0);
    }

    SECTION("the scarf on a compensated circle")
    {
        // A tube, 8 mm outside and 3 mm inside: with the auto circle compensation every wall of it gets a scarf
        // whatever seam_slope_type says (apply_scarf_seam_on_circles).
        const auto tube_runs = [](const DynamicPrintConfig &config) {
            Print        print;
            Model        model;
            ModelObject *object = model.add_object();
            object->name        = "tube";
            object->add_volume(make_cylinder(8., 4., 2. * PI / 360.));
            object->add_volume(make_cylinder(3., 4., 2. * PI / 360.), ModelVolumeType::NEGATIVE_VOLUME);
            object->add_instance()->set_offset(Vec3d(100., 100., 0.));
            object->ensure_on_bed();
            print.apply(model, config);
            print.set_status_silent();
            std::vector<Run> walls;
            for (Run &run : runs_of(Test::gcode(print)))
                if (run.is_wall() && run.layer >= 1)
                    walls.emplace_back(std::move(run));
            return walls;
        };
        DynamicPrintConfig config = walls_only_config();
        config.set_deserialize_strict({
            { "wall_loops", "2" },
            { "seam_slope_type", "none" },
            { "enable_circle_compensation", "1" },
            { "apply_scarf_seam_on_circles", "1" },
        });
        // The scarf is there at its usual length ...
        CHECK(rising_moves(tube_runs(config)) > 0);

        // ... and at none every loop is a plain one: 19 layers of two walls round the tube and two round the hole.
        config.set_deserialize_strict({ { "seam_slope_min_length", "0" } });
        const std::vector<Run> none_long = tube_runs(config);
        REQUIRE(none_long.size() == 19 * 4);
        for (size_t i = 0; i < none_long.size(); ++i) {
            CAPTURE(i);
            CHECK_THAT(none_long[i].gap(), WithinAbs(0.2, 0.005));
            CHECK(none_long[i].rising == 0);
        }
    }
}

TEST_CASE("An inner wall's scarf starts two fifths on whatever the seam gap", "[ScarfSeam]")
{
    // clip_front asked "while (distance > 0 ...)", distance being twice the seam gap, where the length left to
    // clip was meant: with seam_gap 0 nothing was clipped and the inner wall's way up lay beside the outer one's.
    const double slope_length = 20.;
    const double max_segment  = scale_(slope_length / 10);

    SECTION("an inner wall, seam gap 0")
    {
        ExtrusionPaths      paths = straight_wall(erPerimeter, 30.);
        ExtrusionLoopSloped loop(paths, 0., slope_length, max_segment, 0.);
        REQUIRE(loop.role() == erPerimeter);
        CHECK_THAT(length_mm(loop.starts), WithinAbs(20., 1e-3));
        loop.clip_slope(0.);
        REQUIRE_FALSE(loop.starts.empty());
        CHECK_THAT(length_mm(loop.starts), WithinAbs(12., 1e-3));
        CHECK_THAT(unscale_(loop.starts.front().polyline.first_point().x()), WithinAbs(8., 1e-3));
        CHECK_THAT(unscale_(loop.starts.back().polyline.last_point().x()), WithinAbs(20., 1e-3));
        // The way down over it is not touched.
        CHECK_THAT(length_mm(loop.ends), WithinAbs(20., 1e-3));
    }

    SECTION("an inner wall, seam gap 0.06 mm: the same two fifths")
    {
        ExtrusionPaths      paths = straight_wall(erPerimeter, 30.);
        ExtrusionLoopSloped loop(paths, 0., slope_length, max_segment, 0.);
        loop.clip_slope(scale_(0.06));
        CHECK_THAT(length_mm(loop.starts), WithinAbs(12., 1e-3));
        CHECK_THAT(length_mm(loop.ends), WithinAbs(20. - 0.06, 1e-3));
    }

    SECTION("an outer wall is clipped by two seam gaps, and by nothing at 0")
    {
        ExtrusionPaths      paths = straight_wall(erExternalPerimeter, 30.);
        ExtrusionLoopSloped at_zero(paths, 0., slope_length, max_segment, 0.);
        at_zero.clip_slope(0.);
        CHECK_THAT(length_mm(at_zero.starts), WithinAbs(20., 1e-3));

        ExtrusionPaths      again = straight_wall(erExternalPerimeter, 30.);
        ExtrusionLoopSloped with_gap(again, 0., slope_length, max_segment, 0.);
        with_gap.clip_slope(scale_(0.06));
        CHECK_THAT(length_mm(with_gap.starts), WithinAbs(20. - 0.12, 1e-3));
    }
}

TEST_CASE("scarf_joint_speed holds the joint of a small loop", "[ScarfSeam]")
{
    // A post 6 mm across: its wall is 17.4 mm round, a small perimeter under a threshold of 5 mm (31.4 mm round).
    // extrude_loop hands such a loop its small perimeter speed, and _extrude only looked at scarf_joint_speed
    // where no speed was handed down.
    DynamicPrintConfig config = walls_only_config();
    config.set_deserialize_strict({
        { "seam_slope_type", "external" },
        { "seam_slope_min_length", "5" },
        { "small_perimeter_threshold", "5" },
        { "small_perimeter_speed", "60" },
    });
    const auto joint_and_wall = [&config](const char *scarf_joint_speed) {
        config.set_deserialize_strict({ { "scarf_joint_speed", scarf_joint_speed } });
        // From the eleventh layer, clear of the first layers' own speeds.
        const std::vector<Run> walls = wall_runs(make_cylinder(3., 4., 2. * PI / 180.), config, 10);
        std::set<int>          joint, wall;
        for (const Run &run : walls) {
            joint.insert(run.rising_feedrates.begin(), run.rising_feedrates.end());
            wall.insert(run.level_feedrates.begin(), run.level_feedrates.end());
        }
        REQUIRE(walls.size() == 10);
        REQUIRE(rising_moves(walls) > 0);
        return std::make_pair(joint, wall);
    };

    SECTION("slower than the small perimeter speed: the joint is at 20 mm/s, the rest of the wall at 60")
    {
        const auto [joint, wall] = joint_and_wall("20");
        CHECK(joint == std::set<int>{ 1200 });
        CHECK(wall == std::set<int>{ 3600 });
    }
    SECTION("faster than the small perimeter speed: the joint is no faster than its wall")
    {
        const auto [joint, wall] = joint_and_wall("80");
        CHECK(joint == std::set<int>{ 3600 });
        CHECK(wall == std::set<int>{ 3600 });
    }
    SECTION("a percentage is of the outer wall's speed, as on a loop that is not small")
    {
        // 50% of 100 mm/s is under the 60 of a small perimeter, 80% is not.
        const auto [joint, wall] = joint_and_wall("50%");
        CHECK(joint == std::set<int>{ 3000 });
        CHECK(wall == std::set<int>{ 3600 });
        CHECK(joint_and_wall("80%").first == std::set<int>{ 3600 });
    }
}

TEST_CASE("A scarf_joint_speed of 0 is the wall's own speed", "[ScarfSeam]")
{
    // The option's lower limit is 1, which only the settings page holds a value to; a project file can say 0.
    // 0 was then "slower than the wall", the joint's speed became 0, and a speed of 0 is taken for one not set
    // and replaced by the fastest the filament can be melted: here 50 mm3/s, some 580 mm/s, on a 100 mm/s wall.
    DynamicPrintConfig config = walls_only_config();
    config.set_deserialize_strict({ { "seam_slope_type", "external" }, { "seam_slope_min_length", "5" } });
    const auto joint_and_wall = [&config](TriangleMesh mesh, const char *scarf_joint_speed) {
        config.set_deserialize_strict({ { "scarf_joint_speed", scarf_joint_speed } });
        const std::vector<Run> walls = wall_runs(std::move(mesh), config, 10);
        std::set<int>          joint, wall;
        for (const Run &run : walls) {
            joint.insert(run.rising_feedrates.begin(), run.rising_feedrates.end());
            wall.insert(run.level_feedrates.begin(), run.level_feedrates.end());
        }
        REQUIRE(rising_moves(walls) > 0);
        return std::make_pair(joint, wall);
    };

    SECTION("0")
    {
        const auto [joint, wall] = joint_and_wall(Test::mesh(Test::TestMesh::cube_20x20x20), "0");
        CHECK(joint == std::set<int>{ 6000 });
        CHECK(wall == std::set<int>{ 6000 });
    }
    SECTION("0%")
    {
        const auto [joint, wall] = joint_and_wall(Test::mesh(Test::TestMesh::cube_20x20x20), "0%");
        CHECK(joint == std::set<int>{ 6000 });
        CHECK(wall == std::set<int>{ 6000 });
    }
    SECTION("0 on a small loop: its small perimeter speed")
    {
        config.set_deserialize_strict({ { "small_perimeter_threshold", "5" }, { "small_perimeter_speed", "60" } });
        const auto [joint, wall] = joint_and_wall(make_cylinder(3., 4., 2. * PI / 180.), "0");
        CHECK(joint == std::set<int>{ 3600 });
        CHECK(wall == std::set<int>{ 3600 });
    }
    SECTION("a speed that is set still holds")
    {
        const auto [joint, wall] = joint_and_wall(Test::mesh(Test::TestMesh::cube_20x20x20), "30");
        CHECK(joint == std::set<int>{ 1800 });
        CHECK(wall == std::set<int>{ 6000 });
    }
}

TEST_CASE("The conditional scarf's overhang threshold is a share of the wall's own width", "[ScarfSeam]")
{
    // A round post that widens by 0.12 mm a layer. A conditional scarf is left out where the seam is short of
    // the layer beneath by scarf_overhang_threshold of the wall's width or more; the seam placer counts from a
    // tenth of the width inside the outline beneath, so on a 0.45 mm wall the post gets its scarf at the default
    // 40% (0.5 * 0.45 = 0.225 mm of lean allowed) and none at 10% (0.09 mm). The width was read from
    // outer_wall_line_width, which is 0 when the width is left automatic: the threshold came to 0 mm and the
    // post got no scarf at any percentage.
    DynamicPrintConfig config = walls_only_config();
    config.set_deserialize_strict({
        { "seam_slope_type", "external" },
        { "seam_slope_conditional", "1" },
        { "seam_slope_min_length", "10" },
    });
    // How many of the 39 loops above the first layer get a scarf.
    const auto scarfs = [&config](const char *width, const char *threshold) {
        config.set_deserialize_strict({
            { "outer_wall_line_width", width },
            { "inner_wall_line_width", width },
            { "scarf_overhang_threshold", threshold },
        });
        // 4.8 mm of radius at the bottom and 9.6 at the top, 8 mm up: 0.6 mm out for each mm up.
        const std::vector<Run> walls = wall_runs(widening_post(4.8, 9.6, 8., 180), config);
        REQUIRE(walls.size() == 39);
        return scarfed(walls);
    };

    // The automatic width of a 0.4 mm nozzle's wall.
    {
        Print print;
        Model model;
        config.set_deserialize_strict({ { "outer_wall_line_width", "0" }, { "inner_wall_line_width", "0" } });
        Test::init_print({ widening_post(4.8, 9.6, 8., 180) }, print, model, config);
        print.process();
        const ExtrusionEntityCollection &walls = print.objects().front()->layers()[5]->regions().front()->perimeters;
        REQUIRE_FALSE(walls.entities.empty());
        const ExtrusionEntity *first = walls.entities.front();
        while (first->is_collection()) {
            REQUIRE_FALSE(static_cast<const ExtrusionEntityCollection *>(first)->entities.empty());
            first = static_cast<const ExtrusionEntityCollection *>(first)->entities.front();
        }
        const ExtrusionLoop *loop = dynamic_cast<const ExtrusionLoop *>(first);
        REQUIRE(loop != nullptr);
        REQUIRE_THAT(double(loop->paths.front().width), WithinAbs(0.45, 1e-4));
    }

    // With the width set the threshold always counted.
    CHECK(scarfs("0.45", "40%") == 39);
    CHECK(scarfs("0.45", "10%") == 0);
    // With the width automatic it counts the same.
    CHECK(scarfs("0", "40%") == 39);
    CHECK(scarfs("0", "10%") == 0);
    // A threshold of 0 is no threshold, as before.
    CHECK(scarfs("0", "0%") == 39);
}

TEST_CASE("A scarf whose step is shorter than one unit is still split", "[ScarfSeam]")
{
    // The slope is cut into steps by halving its lines until none is longer than a step. A line one unit long
    // has its midpoint on its own start, so with a step under one unit its second half was the line itself:
    // the halving never ended and the stack ran out. The step is seam_slope_min_length / seam_slope_steps: under
    // one unit (a millionth of a mm) with, say, 0.001 mm in 2000 steps, and under nothing at all with a
    // negative number of steps, which a project file can hold.
    SECTION("a slope of 200 units in steps of half a unit")
    {
        ExtrusionPath path{erExternalPerimeter, 1.0, 0.45f, 0.2f};
        path.polyline.append(Point(0, 0));
        path.polyline.append(Point::new_scale(30., 0.));
        ExtrusionPaths      paths{path};
        ExtrusionLoopSloped loop(paths, 0., unscale_(200.), 0.5, 0.);
        REQUIRE(loop.starts.size() == 1);
        const Points &points = loop.starts.front().polyline.points;
        // Cut as fine as the units go: 0, 1, 2 ... 200.
        REQUIRE(points.size() == 201);
        size_t in_place = 0;
        for (size_t i = 0; i < points.size(); ++i)
            in_place += points[i] == Point(coord_t(i), 0) ? 1 : 0;
        CHECK(in_place == 201);
        REQUIRE(loop.ends.size() == 1);
        CHECK(loop.ends.front().polyline.points == points);
        REQUIRE(loop.paths.size() == 1);
        CHECK(loop.paths.front().polyline.first_point() == Point(200, 0));
    }

    SECTION("a step of less than nothing")
    {
        ExtrusionPath path{erExternalPerimeter, 1.0, 0.45f, 0.2f};
        path.polyline.append(Point(0, 0));
        path.polyline.append(Point::new_scale(30., 0.));
        ExtrusionPaths      paths{path};
        ExtrusionLoopSloped loop(paths, 0., unscale_(200.), -1., 0.);
        REQUIRE(loop.starts.size() == 1);
        CHECK(loop.starts.front().polyline.points.size() == 201);
    }

    SECTION("a line one unit along and one across, which has its midpoint on its start as well")
    {
        ExtrusionPath path{erExternalPerimeter, 1.0, 0.45f, 0.2f};
        path.polyline.append(Point(0, 0));
        path.polyline.append(Point(100000, 0));
        path.polyline.append(Point(100001, 1));
        path.polyline.append(Point::new_scale(30., 0.));
        ExtrusionPaths      paths{path};
        ExtrusionLoopSloped loop(paths, 0., 0.2, 0.5, 0.);
        REQUIRE(loop.starts.size() == 1);
        const Points &points = loop.starts.front().polyline.points;
        REQUIRE(points.size() > 100001);
        CHECK(points[100000] == Point(100000, 0));
        CHECK(points[100001] == Point(100001, 1));
    }

    SECTION("sliced: 0.001 mm in 2000 steps")
    {
        DynamicPrintConfig config = walls_only_config();
        config.set_deserialize_strict({
            { "seam_slope_type", "external" },
            { "seam_slope_min_length", "0.001" },
            { "seam_slope_steps", "2000" },
        });
        CHECK(wall_runs(Test::mesh(Test::TestMesh::cube_20x20x20), config).size() == 99);
    }

    SECTION("sliced: a negative number of steps is one step")
    {
        DynamicPrintConfig config = walls_only_config();
        config.set_deserialize_strict({ { "seam_slope_type", "external" }, { "seam_slope_steps", "-1" } });
        const std::vector<Run> walls = wall_runs(Test::mesh(Test::TestMesh::cube_20x20x20), config);
        REQUIRE(walls.size() == 99);
        CHECK(scarfed(walls) == 99);

        config.set_deserialize_strict({ { "seam_slope_steps", "1" } });
        const std::vector<Run> one_step = wall_runs(Test::mesh(Test::TestMesh::cube_20x20x20), config);
        REQUIRE(one_step.size() == 99);
        CHECK(rising_moves(walls) == rising_moves(one_step));
    }
}
