// Bambu Studio 2.9.1 process keys ported for the H3C profiles (PR #854 follow-up):
//  * auto circle contour-hole compensation (enable_circle_compensation and the per-filament
//    coefficients; BS LayerRegion.cpp:69, Polygon.cpp:104, PerimeterGenerator, GCode.cpp:6709);
//  * support ironing inset / direction / speed (BS SupportCommon.cpp:2827, GCode.cpp:7750);
//  * the prime tower options the Type 1 (Bambu) tower honours: prime_tower_acceleration,
//    prime_tower_line_width, prime_tower_wipe_speed, prime_tower_wipe_distance.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "test_data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

Polygon circle_polygon(double radius_mm, int points)
{
    Polygon p;
    for (int i = 0; i < points; ++i) {
        const double a = 2. * PI * double(i) / double(points);
        p.points.emplace_back(Point::new_scale(radius_mm * std::cos(a), radius_mm * std::sin(a)));
    }
    return p;
}

// ---------------------------------------------------------------------------------------------
// Circle compensation fixtures: a tube, outer radius 8 mm and a 3 mm radius hole cut by a negative
// volume, 10 mm tall.
// ---------------------------------------------------------------------------------------------

DynamicPrintConfig circle_config(const char *wall_generator)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator", wall_generator },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "skirt_loops", "0" },
        { "brim_type", "no_brim" },
        { "enable_arc_fitting", "0" },
        { "sparse_infill_density", "0%" },
        { "top_shell_layers", "0" },
        { "bottom_shell_layers", "0" },
        // The 2 mm3/s default would cap the wall speed below the compensation speed.
        { "filament_max_volumetric_speed", "50" },
    });
    return config;
}

void add_tube(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "tube";
    object->add_volume(make_cylinder(8., 10., 2. * PI / 360.));
    object->add_volume(make_cylinder(3., 10., 2. * PI / 360.), ModelVolumeType::NEGATIVE_VOLUME);
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
}

struct TubeAreas
{
    double contour = 0.; // area inside the outer contour, mm2
    double hole    = 0.; // area of the hole, mm2
};

TubeAreas tube_areas(const Print &print)
{
    TubeAreas out;
    const PrintObject &object = *print.objects().front();
    const Layer       *layer  = object.layers()[object.layers().size() / 2];
    REQUIRE(layer->lslices.size() == 1);
    const ExPolygon &island = layer->lslices.front();
    REQUIRE(island.holes.size() == 1);
    out.contour = std::abs(island.contour.area()) * SCALING_FACTOR * SCALING_FACTOR;
    out.hole    = std::abs(island.holes.front().area()) * SCALING_FACTOR * SCALING_FACTOR;
    return out;
}

void slice_tube(Print &print, Model &model, const DynamicPrintConfig &config)
{
    add_tube(model);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

void count_flagged(const ExtrusionEntityCollection &coll, size_t &flagged, size_t &loops)
{
    for (const ExtrusionEntity *ee : coll.entities) {
        if (const auto *sub = dynamic_cast<const ExtrusionEntityCollection *>(ee)) {
            count_flagged(*sub, flagged, loops);
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(ee)) {
            ++loops;
            if (loop->circle_compensation)
                ++flagged;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Support ironing fixture: a 20 mm cube floating 10 mm above a 2 mm leg, supported with a solid
// ironed interface.
// ---------------------------------------------------------------------------------------------

DynamicPrintConfig ironing_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", "1" },
        { "support_type", "normal(auto)" },
        { "support_style", "grid" },
        { "support_interface_top_layers", "2" },
        { "support_interface_bottom_layers", "2" },
        { "support_interface_spacing", "0" },
        { "support_on_build_plate_only", "0" },
        { "support_ironing", "1" },
        { "skirt_loops", "0" },
        { "brim_type", "no_brim" },
        { "enable_arc_fitting", "0" },
    });
    return config;
}

void make_floating_cube(Print &print, Model &model, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name        = "floating_cube";
    object->add_volume(make_cube(2., 2., 10.));
    TriangleMesh top = make_cube(20., 20., 10.);
    top.translate(0.f, 0.f, 10.f);
    object->add_volume(top);
    object->add_instance()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();
    print.apply(model, config);
    print.set_status_silent();
}

struct IroningGeometry
{
    double length = 0.; // mm of ironing
    double dx     = 0.; // summed |dx| of the ironing segments, mm
    double dy     = 0.; // summed |dy|
    BoundingBox bbox;
};

void collect_ironing(const ExtrusionEntityCollection &coll, IroningGeometry &g)
{
    for (const ExtrusionEntity *ee : coll.entities) {
        if (const auto *sub = dynamic_cast<const ExtrusionEntityCollection *>(ee)) {
            collect_ironing(*sub, g);
            continue;
        }
        if (ee->role() != erIroning)
            continue;
        const Polyline pl = ee->as_polyline();
        g.length += unscale<double>(pl.length());
        for (size_t i = 1; i < pl.points.size(); ++i) {
            g.dx += std::abs(unscale<double>(pl.points[i].x() - pl.points[i - 1].x()));
            g.dy += std::abs(unscale<double>(pl.points[i].y() - pl.points[i - 1].y()));
        }
        for (const Point &p : pl.points)
            g.bbox.merge(p);
    }
}

IroningGeometry ironing_geometry(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    make_floating_cube(print, model, config);
    print.process();
    IroningGeometry g;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        collect_ironing(layer->support_fills, g);
    return g;
}

// ---------------------------------------------------------------------------------------------
// Prime tower fixture: two cubes on two tools, Type 1 (Bambu) tower.
// ---------------------------------------------------------------------------------------------

DynamicPrintConfig tower_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#0000FF" };
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = { 15. };
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = { 15. };
    config.set_deserialize_strict({
        { "nozzle_diameter", "0.4,0.4" },
        { "single_extruder_multi_material", "0" },
        { "enable_prime_tower", "1" },
        { "wipe_tower_type", "type1" },
        { "prime_tower_width", "35" },
        { "enable_arc_fitting", "0" },
        { "skirt_loops", "0" },
        { "brim_type", "no_brim" },
        { "machine_start_gcode", "" },
        { "change_filament_gcode", "T{next_extruder}" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wipe", "1,1" },
        { "wipe_distance", "3,3" },
    });
    return config;
}

std::string slice_tower(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    for (int tool = 1; tool <= 2; ++tool) {
        ModelObject *object = model.add_object();
        object->name        = "cube-" + std::to_string(tool) + ".stl";
        object->add_volume(Test::mesh(Test::TestMesh::cube_20x20x20));
        object->add_instance()->set_offset(Vec3d(60. + 40. * tool, 60., 0.));
        object->ensure_on_bed();
        object->volumes.front()->config.set("extruder", tool);
    }
    print.apply(model, config);
    print.is_BBL_printer() = true;
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

std::vector<std::string> tower_blocks(const std::string &gcode)
{
    std::vector<std::string> out;
    for (size_t start = gcode.find("; CP TOOLCHANGE START"); start != std::string::npos;
         start = gcode.find("; CP TOOLCHANGE START", start + 1)) {
        const size_t end = gcode.find("; CP TOOLCHANGE END", start);
        out.emplace_back(gcode.substr(start, end == std::string::npos ? std::string::npos : end - start));
    }
    return out;
}

double word(const std::string &line, char letter, double fallback)
{
    const size_t comment = line.find(';');
    for (size_t i = line.find(' '); i != std::string::npos && i < comment; i = line.find(' ', i + 1))
        if (i + 1 < line.size() && line[i + 1] == letter)
            return std::atof(line.c_str() + i + 2);
    return fallback;
}

// XY length of the last wipe (WIPE_START .. WIPE_END) before each tool change line "T<n>".
std::vector<double> toolchange_wipe_lengths(const std::string &gcode)
{
    std::vector<double> out;
    std::istringstream  in(gcode);
    std::string         line;
    double x = 0., y = 0., wipe_len = -1., last_wipe = -1.;
    bool   in_wipe = false;
    while (std::getline(in, line)) {
        if (line.find("WIPE_START") != std::string::npos) {
            in_wipe  = true;
            wipe_len = 0.;
            continue;
        }
        if (line.find("WIPE_END") != std::string::npos) {
            in_wipe   = false;
            last_wipe = wipe_len;
            continue;
        }
        if (line.size() > 1 && line[0] == 'T' && std::isdigit((unsigned char) line[1])) {
            if (last_wipe >= 0.)
                out.push_back(last_wipe);
            last_wipe = -1.;
            continue;
        }
        if (line.rfind("G1 ", 0) == 0 || line.rfind("G0 ", 0) == 0) {
            const double nx = word(line, 'X', x), ny = word(line, 'Y', y);
            if (in_wipe)
                wipe_len += std::hypot(nx - x, ny - y);
            x = nx;
            y = ny;
        }
    }
    return out;
}

} // namespace

// =============================================================================================
// Circle compensation
// =============================================================================================

TEST_CASE("Polygon::is_approx_circle detects circles as Bambu Studio does", "[BS29Keys][CircleCompensation]")
{
    Point  center;
    double diameter = 0.;

    SECTION("a 64-point circle of radius 5 mm")
    {
        REQUIRE(circle_polygon(5., 64).is_approx_circle(scale_(0.5), 5 * scale_(0.01) * scale_(0.01), center, diameter));
        CHECK(unscale<double>(diameter) == Approx(10.).margin(0.05));
        CHECK(std::abs(unscale<double>(center.x())) < 0.01);
        CHECK(std::abs(unscale<double>(center.y())) < 0.01);
    }
    SECTION("fewer than 8 points is never a circle")
    {
        CHECK_FALSE(circle_polygon(5., 7).is_approx_circle(scale_(0.5), 5 * scale_(0.01) * scale_(0.01), center, diameter));
    }
    SECTION("a square is not a circle")
    {
        Polygon square;
        for (const Vec2d &p : { Vec2d(0, 0), Vec2d(5, 0), Vec2d(10, 0), Vec2d(10, 5), Vec2d(10, 10), Vec2d(5, 10), Vec2d(0, 10), Vec2d(0, 5) })
            square.points.emplace_back(Point::new_scale(p.x(), p.y()));
        CHECK_FALSE(square.is_approx_circle(scale_(0.5), 5 * scale_(0.01) * scale_(0.01), center, diameter));
    }
}

TEST_CASE("Auto circle compensation offsets circular contours and holes by the filament coefficients", "[BS29Keys][CircleCompensation]")
{
    const char *generator = GENERATE(as<const char *>{}, "classic", "arachne");
    INFO("wall_generator " << generator);

    Print     print_off, print_on, print_manual;
    Model     model_off, model_on, model_manual;
    DynamicPrintConfig config = circle_config(generator);
    slice_tube(print_off, model_off, config);
    const TubeAreas off = tube_areas(print_off);

    config.set_deserialize_strict({ { "enable_circle_compensation", "1" } });
    slice_tube(print_on, model_on, config);
    const TubeAreas on = tube_areas(print_on);

    // Defaults: contour 0.025 * 16 - 0.11 = 0.29, clamped to the 0.05 mm limit max: the contour grows
    // 0.05 mm. Hole -0.025 * 6 + 0.28 = 0.13 (inside 0.08..0.25): the hole opens up by 0.13 mm.
    const double r_contour = std::sqrt(off.contour / PI);
    const double r_hole    = std::sqrt(off.hole / PI);
    CHECK(std::sqrt(on.contour / PI) - r_contour == Approx(0.05).margin(0.01));
    CHECK(std::sqrt(on.hole / PI) - r_hole == Approx(0.13).margin(0.01));

    // The manual offset takes half its value off each side: contour 0.05 - 0.1, hole 0.13 + 0.1.
    config.set_deserialize_strict({ { "circle_compensation_manual_offset", "0.2" } });
    slice_tube(print_manual, model_manual, config);
    const TubeAreas manual = tube_areas(print_manual);
    CHECK(std::sqrt(manual.contour / PI) - r_contour == Approx(-0.05).margin(0.01));
    CHECK(std::sqrt(manual.hole / PI) - r_hole == Approx(0.23).margin(0.01));

    // Off by default: no compensation unless switched on.
    CHECK_FALSE(DynamicPrintConfig::full_print_config().opt_bool("enable_circle_compensation"));
}

TEST_CASE("Circle compensation coefficients are read per filament", "[BS29Keys][CircleCompensation]")
{
    Print              print_off, print_on;
    Model              model_off, model_on;
    DynamicPrintConfig config = circle_config("classic");
    slice_tube(print_off, model_off, config);
    const TubeAreas off = tube_areas(print_off);

    // A filament whose contour offset is a flat +0.03 mm and hole offset a flat 0.1 mm.
    config.set_deserialize_strict({
        { "enable_circle_compensation", "1" },
        { "counter_coef_2", "0" }, { "counter_coef_3", "0.03" },
        { "hole_coef_2", "0" }, { "hole_coef_3", "0.1" },
    });
    slice_tube(print_on, model_on, config);
    const TubeAreas on = tube_areas(print_on);
    CHECK(std::sqrt(on.contour / PI) - std::sqrt(off.contour / PI) == Approx(0.03).margin(0.01));
    CHECK(std::sqrt(on.hole / PI) - std::sqrt(off.hole / PI) == Approx(0.1).margin(0.01));
}

TEST_CASE("Compensated circle walls print at the compensation speed with a scarf seam", "[BS29Keys][CircleCompensation]")
{
    const char *generator = GENERATE(as<const char *>{}, "classic", "arachne");
    INFO("wall_generator " << generator);

    DynamicPrintConfig config = circle_config(generator);
    config.set_deserialize_strict({ { "circle_compensation_speed", "37" }, { "apply_scarf_seam_on_circles", "0" } });

    SECTION("off: no walls flagged, no slow-down block, no compensation speed")
    {
        Print print;
        Model model;
        slice_tube(print, model, config);
        size_t flagged = 0, loops = 0;
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions())
                count_flagged(region->perimeters, flagged, loops);
        CHECK(loops > 0);
        CHECK(flagged == 0);
        const std::string gcode = Test::gcode(print);
        CHECK(gcode.find("; Slow Down Start") == std::string::npos);
        CHECK(gcode.find("F2220") == std::string::npos);
    }

    SECTION("on: the walls of both circles are flagged and printed at 37 mm/s")
    {
        config.set_deserialize_strict({ { "enable_circle_compensation", "1" } });
        Print print;
        Model model;
        slice_tube(print, model, config);
        size_t flagged = 0, loops = 0;
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions())
                count_flagged(region->perimeters, flagged, loops);
        CHECK(loops > 0);
        CHECK(flagged == loops);
        const std::string gcode = Test::gcode(print);
        CHECK(gcode.find("; Slow Down Start") != std::string::npos);
        CHECK(gcode.find("F2220") != std::string::npos);
    }

    SECTION("on, circles above the diameter limit: still offset, but not flagged")
    {
        config.set_deserialize_strict({ { "enable_circle_compensation", "1" }, { "diameter_limit", "5" } });
        Print print;
        Model model;
        slice_tube(print, model, config);
        size_t flagged = 0, loops = 0;
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions())
                count_flagged(region->perimeters, flagged, loops);
        // The 6 mm hole is above the 5 mm limit as well as the 16 mm contour.
        CHECK(loops > 0);
        CHECK(flagged == 0);
        CHECK(Test::gcode(print).find("; Slow Down Start") == std::string::npos);
    }
}

TEST_CASE("Scarf seam on compensated circles follows apply_scarf_seam_on_circles", "[BS29Keys][CircleCompensation]")
{
    // A scarf seam lifts Z while it extrudes: a G1 with both Z and E inside a slow-down block.
    auto sloped_moves = [](const std::string &gcode) {
        std::istringstream in(gcode);
        std::string        line;
        bool               in_block = false;
        size_t             count    = 0;
        while (std::getline(in, line)) {
            if (line.rfind("; Slow Down Start", 0) == 0)
                in_block = true;
            else if (line.rfind("; Slow Down End", 0) == 0)
                in_block = false;
            else if (in_block && line.rfind("G1 ", 0) == 0 && line.find(" Z") != std::string::npos && line.find(" E") != std::string::npos)
                ++count;
        }
        return count;
    };
    DynamicPrintConfig config = circle_config("classic");
    config.set_deserialize_strict({ { "enable_circle_compensation", "1" }, { "seam_slope_type", "none" } });

    Print print_off, print_on;
    Model model_off, model_on;
    config.set_deserialize_strict({ { "apply_scarf_seam_on_circles", "0" } });
    slice_tube(print_off, model_off, config);
    CHECK(sloped_moves(Test::gcode(print_off)) == 0);

    config.set_deserialize_strict({ { "apply_scarf_seam_on_circles", "1" } });
    slice_tube(print_on, model_on, config);
    CHECK(sloped_moves(Test::gcode(print_on)) > 0);
}

// =============================================================================================
// Support ironing
// =============================================================================================

TEST_CASE("Support ironing inset shrinks the ironed area", "[BS29Keys][SupportIroning]")
{
    DynamicPrintConfig config = ironing_config();
    const IroningGeometry plain = ironing_geometry(config);
    REQUIRE(plain.length > 10.);

    config.set_deserialize_strict({ { "support_ironing_inset", "1.5" } });
    const IroningGeometry inset = ironing_geometry(config);
    REQUIRE(inset.length > 1.);
    CHECK(inset.length < plain.length * 0.9);
    // 1.5 mm off every side of the ironed area.
    CHECK(unscale<double>(plain.bbox.size().x() - inset.bbox.size().x()) == Approx(3.).margin(0.5));
}

TEST_CASE("Support ironing direction turns the ironing lines", "[BS29Keys][SupportIroning]")
{
    DynamicPrintConfig config = ironing_config();
    const IroningGeometry plain = ironing_geometry(config);
    REQUIRE(plain.length > 10.);
    const bool plain_along_x = plain.dx > plain.dy;

    config.set_deserialize_strict({ { "support_ironing_direction", "90" } });
    const IroningGeometry turned = ironing_geometry(config);
    REQUIRE(turned.length > 10.);
    CHECK((turned.dx > turned.dy) != plain_along_x);
}

TEST_CASE("Support ironing speed reaches the G-code, 0 keeps the ironing speed", "[BS29Keys][SupportIroning]")
{
    DynamicPrintConfig config = ironing_config();
    config.set_deserialize_strict({ { "ironing_speed", "29" } });
    {
        Print print;
        Model model;
        make_floating_cube(print, model, config);
        const std::string gcode = Test::gcode(print);
        CHECK(gcode.find("F1740") != std::string::npos); // 29 mm/s: the ironing speed
        CHECK(gcode.find("F1020") == std::string::npos);
    }
    config.set_deserialize_strict({ { "support_ironing_speed", "17" } });
    {
        Print print;
        Model model;
        make_floating_cube(print, model, config);
        const std::string gcode = Test::gcode(print);
        CHECK(gcode.find("F1020") != std::string::npos); // 17 mm/s
    }
}

// =============================================================================================
// Prime tower (Type 1)
// =============================================================================================

TEST_CASE("prime_tower_acceleration sets the Type 1 tower's M204", "[BS29Keys][PrimeTower]")
{
    DynamicPrintConfig config = tower_config();
    config.set_deserialize_strict({ { "travel_acceleration", "9000" } });
    {
        const std::string gcode = slice_tower(config);
        const auto        blocks = tower_blocks(gcode);
        REQUIRE(blocks.size() > 2);
        for (const std::string &b : blocks)
            CHECK(b.find("M204 S7000") == std::string::npos);
    }
    config.set_deserialize_strict({ { "prime_tower_acceleration", "7000" } });
    {
        const std::string gcode = slice_tower(config);
        const auto        blocks = tower_blocks(gcode);
        REQUIRE(blocks.size() > 2);
        size_t with_accel = 0, with_travel = 0;
        for (const std::string &b : blocks) {
            if (b.find("M204 S7000") != std::string::npos)
                ++with_accel;
            if (b.find("M204 S9000") != std::string::npos)
                ++with_travel;
        }
        // Every tower visit above the first layer.
        CHECK(with_accel >= blocks.size() - 2);
        CHECK(with_travel > 0);
    }
}

TEST_CASE("prime_tower_line_width sets the Type 1 tower's line width", "[BS29Keys][PrimeTower]")
{
    static const std::regex width_re(R"(^;\s*(?:LINE_)?WIDTH:\s*([0-9.]+))");
    auto widths = [](const std::string &gcode) {
        std::vector<long> out;
        for (const std::string &b : tower_blocks(gcode)) {
            std::istringstream in(b);
            std::string        line;
            while (std::getline(in, line)) {
                std::smatch m;
                if (std::regex_search(line, m, width_re))
                    out.push_back(std::lround(std::stod(m[1].str()) * 1000.));
            }
        }
        return out;
    };
    DynamicPrintConfig config = tower_config();
    const auto plain = widths(slice_tower(config));
    REQUIRE(! plain.empty());
    CHECK(std::find(plain.begin(), plain.end(), 500L) != plain.end());
    CHECK(std::find(plain.begin(), plain.end(), 800L) == plain.end());

    config.set_deserialize_strict({ { "prime_tower_line_width", "0.8" } });
    const auto wide = widths(slice_tower(config));
    REQUIRE(! wide.empty());
    CHECK(std::find(wide.begin(), wide.end(), 800L) != wide.end());
}

TEST_CASE("prime_tower_wipe_speed and _distance drive the wipe before a tool change", "[BS29Keys][PrimeTower]")
{
    DynamicPrintConfig config = tower_config();
    {
        const std::string gcode = slice_tower(config);
        CHECK(gcode.find("F1380") == std::string::npos);
        const auto lengths = toolchange_wipe_lengths(gcode);
        REQUIRE(! lengths.empty());
        CHECK(*std::max_element(lengths.begin(), lengths.end()) > 1.);
    }
    config.set_deserialize_strict({ { "prime_tower_wipe_speed", "23" }, { "prime_tower_wipe_distance", "0.6" } });
    {
        const std::string gcode = slice_tower(config);
        CHECK(gcode.find("F1380") != std::string::npos); // 23 mm/s
        const auto lengths = toolchange_wipe_lengths(gcode);
        REQUIRE(! lengths.empty());
        for (double l : lengths)
            CHECK(l <= 0.6 + 0.02);
    }
}
