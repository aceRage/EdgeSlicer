#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/calib.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

DynamicPrintConfig wipe_config(const char *wall_generator, bool wipe_inward,
                               const char *wipe_inward_distance = "50%",
                               const char *seam_gap = "10%", bool wipe_on_loops = false,
                               const char *wall_loops = "2",
                               const char *wall_sequence = "inner wall/outer wall",
                               bool alternate_extra_wall = false,
                               const char *sparse_infill_density = "0%",
                               const char *seam_position = "aligned")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "nozzle_diameter",             "0.4" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "line_width",                  "0.45" },
        { "outer_wall_line_width",       "0" }, // Orca: Auto must use the actual path width.
        { "wall_loops",                  wall_loops },
        { "wall_generator",              wall_generator },
        { "wall_sequence",               wall_sequence },
        { "top_shell_layers",            "0" },
        { "bottom_shell_layers",         "0" },
        { "sparse_infill_density",       sparse_infill_density },
        { "seam_position",               seam_position },
        { "seam_gap",                    seam_gap },
        { "wipe",                        "1" },
        { "wipe_distance",               "2" },
        { "retraction_length",           "0.8" },
        { "retract_when_changing_layer", "1" },
        { "wipe_inward",                 wipe_inward ? "1" : "0" },
        { "wipe_inward_distance",        wipe_inward_distance },
        { "wipe_on_loops",               wipe_on_loops ? "1" : "0" },
        { "alternate_extra_wall",        alternate_extra_wall ? "1" : "0" },
        { "gcode_comments",              "1" },
        { "machine_start_gcode",         "" },
        { "machine_end_gcode",           "" },
    });
    return config;
}

struct WipeTrajectory {
    Vec2d start;
    double z;
    std::vector<Vec2d> destinations;
};

std::vector<WipeTrajectory> wipe_trajectories(const std::string &gcode)
{
    const std::string &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
    const std::string &end_tag   = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End);
    std::vector<WipeTrajectory> trajectories;
    bool in_wipe = false;

    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.find(start_tag) != std::string_view::npos) {
            in_wipe = true;
            trajectories.push_back({Vec2d(self.x(), self.y()), self.z(), {}});
            return;
        }
        if (comment.find(end_tag) != std::string_view::npos) {
            in_wipe = false;
            return;
        }
        if (in_wipe && line.dist_XY(self) > EPSILON)
            trajectories.back().destinations.emplace_back(line.new_X(self), line.new_Y(self));
    });
    return trajectories;
}

std::vector<Vec2d> wipe_destinations(const std::string &gcode)
{
    std::vector<Vec2d> destinations;
    for (const WipeTrajectory &trajectory : wipe_trajectories(gcode))
        destinations.insert(destinations.end(), trajectory.destinations.begin(), trajectory.destinations.end());
    return destinations;
}

bool trajectories_differ(const std::vector<Vec2d> &lhs, const std::vector<Vec2d> &rhs)
{
    if (lhs.size() != rhs.size())
        return true;
    for (size_t i = 0; i < lhs.size(); ++i)
        if ((lhs[i] - rhs[i]).norm() > 0.01)
            return true;
    return false;
}

double trajectory_length(const WipeTrajectory &trajectory)
{
    double length = 0.;
    Vec2d previous = trajectory.start;
    for (const Vec2d &destination : trajectory.destinations) {
        length += (destination - previous).norm();
        previous = destination;
    }
    return length;
}

} // namespace

TEST_CASE("Wipe retraction preserves fractional speed with inward wipe disabled", "[Wipe][Regression]")
{
    const char *retraction_speed = GENERATE("25.25", "25.5", "25.75");
    const char *relative_e = GENERATE("0", "1");
    INFO("retraction speed: " << retraction_speed);
    INFO("relative E: " << relative_e);
    DynamicPrintConfig config = wipe_config("classic", false);
    config.set_deserialize_strict({
        {"gcode_flavor", "marlin2"},
        {"use_relative_e_distances", relative_e},
        {"retraction_speed", retraction_speed},
        {"retraction_length", "0.8"},
        {"retract_before_wipe", "0%"},
        {"retract_after_wipe", "0%"},
        {"role_based_wipe_speed", "0"},
        {"wipe_speed", "100"},
        {"wipe_distance", "2"},
    });
    const std::string output = slice({make_cube(10., 10., 1.)}, config);
    const auto &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
    const auto &end_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End);
    double before_wipe = 0.;
    double during_wipe = 0.;
    bool in_wipe = false;
    bool complete = false;
    GCodeReader parser;
    parser.apply_config(config);
    parser.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (complete)
            return;
        if (line.comment().find(start_tag) != std::string_view::npos) {
            in_wipe = true;
        } else if (in_wipe && line.comment().find(end_tag) != std::string_view::npos) {
            complete = true;
        } else if (line.retracting(self)) {
            (in_wipe ? during_wipe : before_wipe) -= line.dist_E(self);
        } else if (line.extruding(self)) {
            before_wipe = 0.;
        }
    });

    REQUIRE(complete);
    // At 100 mm/s, the 2 mm wipe lasts 0.02 seconds. The remaining part of
    // the configured 0.8 mm retraction must be emitted before that wipe.
    const double expected_during = std::stod(retraction_speed) * 2. / 100.;
    CHECK_THAT(during_wipe, Catch::Matchers::WithinAbs(expected_during, 0.00005));
    CHECK_THAT(before_wipe, Catch::Matchers::WithinAbs(0.8 - expected_during, 0.00005));
}

TEST_CASE("Inward wipe respects the minimum travel for retraction and Z hop", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *relative_e = GENERATE("0", "1");
    const char *reduce_crossing_wall = GENERATE("0", "1");
    const char *minimum_travel = GENERATE("5", "0");
    CAPTURE(wall_generator, relative_e, reduce_crossing_wall, minimum_travel);
    DynamicPrintConfig config = wipe_config(
        wall_generator, true, "50%", "10%", false, "3", "inner-outer-inner wall");
    config.set_deserialize_strict({
        {"gcode_flavor", "marlin2"},
        {"use_relative_e_distances", relative_e},
        {"reduce_crossing_wall", reduce_crossing_wall},
        {"retraction_minimum_travel", minimum_travel},
        {"retract_when_changing_layer", "0"},
        {"use_firmware_retraction", "0"},
        {"retract_before_wipe", "0%"},
        {"retract_after_wipe", "0%"},
        {"retraction_speed", "25.5"},
        {"role_based_wipe_speed", "0"},
        {"wipe_speed", "100"},
        {"z_hop", "0.4"},
        {"retract_lift_above", "0"},
        {"retract_lift_below", "0"},
    });
    config.set_key_value("z_hop_types", new ConfigOptionEnumsGeneric{zhtNormal});
    config.set_key_value("retract_lift_enforce", new ConfigOptionEnumsGeneric{rletAllSurfaces});
    const std::string output = slice({make_cube(10., 10., 1.)}, config);
    const auto &role_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    const auto &start_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
    const auto &end_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_End);
    ExtrusionRole role = erNone;
    bool after_outer_wall = false;
    bool in_wipe = false;
    size_t transitions = 0;
    size_t same_layer_transitions = 0;
    size_t inward_wipes = 0;
    double retraction = 0.;
    double lift = 0.;
    double outer_z = 0.;
    GCodeReader parser;
    parser.apply_config(config);
    parser.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.comment().find(role_tag) == 0)
            role = ExtrusionEntity::string_to_role(line.comment().substr(role_tag.size()));
        if (line.comment().find(start_tag) == 0) {
            in_wipe = true;
            if (after_outer_wall)
                ++inward_wipes;
        } else if (line.comment().find(end_tag) == 0) {
            in_wipe = false;
        }
        if (line.extruding(self) && line.dist_XY(self) > EPSILON) {
            if (role == erExternalPerimeter) {
                after_outer_wall = true;
                retraction = lift = 0.;
                outer_z = line.new_Z(self);
            } else if (after_outer_wall) {
                REQUIRE(role == erPerimeter);
                ++transitions;
                const double layer_rise = std::max(0., double(self.z()) - outer_z);
                if (layer_rise < EPSILON)
                    ++same_layer_transitions;
                // A 5 mm threshold suppresses retraction across a few wall widths.
                // A zero threshold still permits the ordinary retract and lift.
                const bool retract = std::stod(minimum_travel) == 0.;
                CHECK_THAT(retraction, Catch::Matchers::WithinAbs(retract ? 0.8 : 0., 0.00005));
                // Exclude an ordinary layer change from the accumulated upward motion.
                CHECK_THAT(lift - layer_rise, Catch::Matchers::WithinAbs(retract ? 0.4 : 0., 0.001));
                after_outer_wall = false;
            }
        } else if (after_outer_wall) {
            if (line.retracting(self))
                retraction -= line.dist_E(self);
            lift += std::max(0., double(line.dist_Z(self)));
            if (in_wipe)
                CHECK_THAT(line.dist_E(self), Catch::Matchers::WithinAbs(0., 0.00005));
        }
    });
    // The 1 mm cube has five 0.2 mm layers: every outer wall must still wipe.
    REQUIRE(transitions == 5);
    REQUIRE(same_layer_transitions >= 4);
    REQUIRE(inward_wipes == transitions);
}

TEST_CASE("Changing inward wipe settings preserves the sliced geometry", "[Wipe][Regression]")
{
    const char *key = GENERATE("wipe_inward", "wipe_inward_distance");
    DynamicPrintConfig config = wipe_config("classic", false);
    Print print;
    Model model;
    init_print({make_cube(10., 10., 1.)}, print, model, config);
    gcode(print);
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.is_step_done(posPerimeters));
    REQUIRE(object.is_step_done(posInfill));
    REQUIRE(print.is_step_done(psWipeTower));
    REQUIRE(print.is_step_done(psGCodeExport));

    DynamicPrintConfig changed = config;
    changed.set_deserialize_strict({{key, std::string(key) == "wipe_inward" ? "1" : "75%"}});
    print.apply(model, changed);

    CHECK(print.objects().front()->is_step_done(posPerimeters));
    CHECK(print.objects().front()->is_step_done(posInfill));
    CHECK(print.is_step_done(psWipeTower));
    CHECK_FALSE(print.is_step_done(psGCodeExport));
}

TEST_CASE("Retraction and pressure advance calibration suppress inward wipe overrides", "[Wipe][Regression]")
{
    // Upstream tests Calib_Auto_PA_Line here (upstream value 4), which draws PA lines
    // on top of the printed object. Edge has no Auto_PA_Line: its legacy Calib_PA_Line
    // calibration REPLACES the print with the pure test pattern (writer-direct output
    // in calib.cpp, no object extrusions, no wipe moves), so there is nothing to
    // suppress and REQUIRE_FALSE(empty) cannot hold. Calib_PA_Tower and
    // Calib_Retraction_tower do print the object and cover the suppression path.
    const auto mode = GENERATE(CalibMode::Calib_None, CalibMode::Calib_PA_Tower,
                              CalibMode::Calib_Retraction_tower,
                              CalibMode::Calib_Flow_Rate);
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool per_object = GENERATE(false, true);
    INFO("calibration mode: " << int(mode) << ", wall generator: " << wall_generator
         << ", per-object override: " << per_object);

    const auto trajectories = [&](bool inward) {
        DynamicPrintConfig config = wipe_config(wall_generator, inward && !per_object);
        Print print;
        Model model;
        init_print({make_cube(10., 10., 1.)}, print, model, config);
        if (per_object) {
            model.objects.front()->config.set_key_value("wipe_inward", new ConfigOptionBool(inward));
            print.apply(model, config);
        }
        Calib_Params params;
        params.mode = mode;
        params.start = 0.2;
        params.end = 0.4;
        params.step = 0.1;
        print.set_calib_params(params);
        return wipe_destinations(gcode(print));
    };

    const auto regular = trajectories(false);
    const auto inward = trajectories(true);
    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(inward.empty());
    // Other calibration modes and ordinary prints must still honor the option.
    const bool should_differ = mode == CalibMode::Calib_None || mode == CalibMode::Calib_Flow_Rate;
    CHECK(trajectories_differ(regular, inward) == should_differ);
}

TEST_CASE("Inactive inward wipe settings preserve the exported trajectory", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool disable_wiping = GENERATE(false, true);
    DynamicPrintConfig regular = wipe_config(wall_generator, false);
    DynamicPrintConfig inward = wipe_config(wall_generator, true, disable_wiping ? "50%" : "0");
    if (disable_wiping) {
        regular.set_deserialize_strict({{"wipe", "0"}});
        inward.set_deserialize_strict({{"wipe", "0"}});
    }
    const auto regular_paths = wipe_destinations(slice({make_cube(10., 10., 1.)}, regular));
    const auto inward_paths = wipe_destinations(slice({make_cube(10., 10., 1.)}, inward));
    if (!disable_wiping)
        REQUIRE_FALSE(regular_paths.empty());
    CHECK_FALSE(trajectories_differ(regular_paths, inward_paths));
}

TEST_CASE("Inward wipe changes the exported trajectory when outer wall width is Auto", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, false)));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true)));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(inward.empty());
    REQUIRE(trajectories_differ(regular, inward));
}

TEST_CASE("Inward wipe recognizes an external wall starting on an overhang", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const bool inward = GENERATE(false, true);
    CAPTURE(wall_generator, inward);
    const auto config = wipe_config(wall_generator, inward, "50%", "0%", false,
                                    "3", "inner-outer-inner wall", false, "0%", "back");
    Print print;
    Model model;
    init_print({make_cube(10., 10., 1.)}, print, model, config);
    print.process();
    size_t mixed_loops = 0;
    const auto mark_overhangs = [&](auto &&self, ExtrusionEntity *entity) -> void {
        if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
            for (ExtrusionEntity *child : collection->entities)
                self(self, child);
        } else if (auto *loop = dynamic_cast<ExtrusionLoop *>(entity); loop && is_external_perimeter(loop->role())) {
            // Keep the printed geometry intact and give the back seam overhang
            // roles. The front edge remains an ordinary external-wall segment.
            ExtrusionPaths paths;
            bool has_overhang = false;
            bool has_external = false;
            for (const ExtrusionPath &source : loop->paths) {
                for (size_t i = 1; i < source.polyline.points.size(); ++i) {
                    ExtrusionPath path = source;
                    path.polyline.points = {source.polyline.points[i - 1], source.polyline.points[i]};
                    const bool overhang = path.polyline.points.front().y() > 0 || path.polyline.points.back().y() > 0;
                    path.set_extrusion_role(overhang ? erOverhangPerimeter : erExternalPerimeter);
                    has_overhang |= overhang;
                    has_external |= !overhang;
                    paths.push_back(std::move(path));
                }
            }
            REQUIRE(has_overhang);
            REQUIRE(has_external);
            loop->paths = std::move(paths);
            ++mixed_loops;
        }
    };
    for (const PrintObject *object : print.objects())
        for (Layer *layer : object->layers())
            for (LayerRegion *region : layer->regions())
                mark_overhangs(mark_overhangs, &region->perimeters);
    REQUIRE(mixed_loops > 0);

    bool has_inward_wipe = false;
    for (const WipeTrajectory &trajectory : wipe_trajectories(gcode(print))) {
        if (trajectory.destinations.empty())
            continue;
        const Vec2d move = trajectory.destinations.front() - trajectory.start;
        if (trajectory.start.x() > 4. && trajectory.start.y() > 4. && move.x() < -0.05 && move.y() < -0.05)
            has_inward_wipe = true;
    }
    CHECK(has_inward_wipe == inward);
}

TEST_CASE("Inward wipe keeps its offset when seam gap is zero", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, false, "50%", "0%")));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true, "50%", "0%")));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(inward.empty());
    REQUIRE(trajectories_differ(regular, inward));
}

TEST_CASE("Inward wipe is retained across layers with a back seam", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const DynamicPrintConfig inward_config = wipe_config(
        wall_generator, true, "50%", "0%", false, "3", "inner-outer-inner wall", false, "0%", "back");
    const std::vector<WipeTrajectory> inward = wipe_trajectories(slice({make_cube(27., 27., 1.)}, inward_config));

    REQUIRE_FALSE(inward.empty());
    std::map<double, bool> inward_wipe_by_layer;
    for (const WipeTrajectory &trajectory : inward) {
        bool &has_inward_wipe = inward_wipe_by_layer[trajectory.z];
        if (trajectory.destinations.empty())
            continue;
        const Vec2d first_move = trajectory.destinations.front() - trajectory.start;
        // Orca: a back seam lands on the cube's positive-X/positive-Y corner.
        // Its inward wipe must move diagonally away from both external faces.
        has_inward_wipe = has_inward_wipe ||
            (trajectory.start.x() > 13. && trajectory.start.y() > 13. &&
             first_move.x() < -0.05 && first_move.y() < -0.05);
    }
    REQUIRE(inward_wipe_by_layer.size() == 5);
    for (const auto &[z, has_inward_wipe] : inward_wipe_by_layer) {
        INFO("layer Z: " << z);
        REQUIRE(has_inward_wipe);
    }
}

TEST_CASE("Literal inward wipe distance is clamped to the outer wall width", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, false)));
    const std::vector<Vec2d> full_width = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true, "100%")));
    const std::vector<Vec2d> oversized = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true, "2")));

    REQUIRE_FALSE(full_width.empty());
    REQUIRE(trajectories_differ(regular, full_width));
    REQUIRE(oversized.size() == full_width.size());
    for (size_t i = 0; i < full_width.size(); ++i)
        REQUIRE_THAT((oversized[i] - full_width[i]).norm(), Catch::Matchers::WithinAbs(0., 0.01));
}

TEST_CASE("Inward wipe is not applied without an adjacent wall", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, false, "50%", "10%", false, "1")));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(wall_generator, true, "50%", "10%", false, "1")));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(trajectories_differ(regular, inward));
}

TEST_CASE("Inward wipe uses an alternate extra wall when the configured wall count is one", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const DynamicPrintConfig regular_config = wipe_config(
        wall_generator, false, "50%", "10%", false, "1", "inner wall/outer wall", true, "15%");
    const DynamicPrintConfig inward_config = wipe_config(
        wall_generator, true, "50%", "10%", false, "1", "inner wall/outer wall", true, "15%");
    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, regular_config));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, inward_config));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(inward.empty());
    REQUIRE(trajectories_differ(regular, inward));
}

TEST_CASE("Inward wipe is not applied before the adjacent wall is printed", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<Vec2d> regular = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(
            wall_generator, false, "50%", "10%", false, "2", "outer wall/inner wall")));
    const std::vector<Vec2d> inward = wipe_destinations(
        slice({make_cube(10., 10., 1.)}, wipe_config(
            wall_generator, true, "50%", "10%", false, "2", "outer wall/inner wall")));

    REQUIRE_FALSE(regular.empty());
    REQUIRE_FALSE(trajectories_differ(regular, inward));
}

TEST_CASE("Wipe on loops preserves the corner move with inward wipe disabled", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *nozzle_diameter = GENERATE("0.4", "0.8");
    const char *comments = GENERATE("0", "1");
    CAPTURE(comments);
    INFO("wall generator: " << wall_generator << ", nozzle diameter: " << nozzle_diameter);
    // A closed square gives a 90-degree material-side corner at the seam.
    DynamicPrintConfig config = wipe_config(wall_generator, false, "50%", "0", true);
    config.set_deserialize_strict({{"nozzle_diameter", nozzle_diameter}, {"seam_position", "nearest"},
                                   {"gcode_comments", comments}});
    const std::string output = slice({make_cube(10., 10., 1.)}, config);
    const auto &role_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    const auto &wipe_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
    ExtrusionRole role = erNone;
    std::vector<Vec2d> loop;
    bool after_extrusion = false;
    size_t moves = 0;
    GCodeReader parser;
    parser.apply_config(config);
    parser.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.comment().find(role_tag) == 0) {
            role = ExtrusionEntity::string_to_role(line.comment().substr(role_tag.size()));
            loop.clear();
            after_extrusion = false;
        }
        if (line.comment().find(wipe_tag) == 0)
            after_extrusion = false;
        if (role != erExternalPerimeter)
            return;
        if (line.extruding(self) && line.dist_XY(self) > EPSILON) {
            if (loop.empty())
                loop.emplace_back(self.x(), self.y());
            loop.emplace_back(line.new_X(self), line.new_Y(self));
            after_extrusion = true;
            return;
        }
        // The loop move is the first non-extruding XY move after the external
        // wall and before the reserved wipe marker, regardless of comment text.
        if (!after_extrusion || line.dist_XY(self) <= EPSILON)
            return;
        after_extrusion = false;

        ++moves;
        INFO("layer Z: " << self.z());
        REQUIRE(loop.size() >= 4);
        const Vec2d seam = loop.front();
        REQUIRE_THAT((loop.back() - seam).norm(), Catch::Matchers::WithinAbs(0., 0.003));
        const Vec2d outgoing = (loop[1] - seam).normalized();
        const Vec2d into_corner = (loop[loop.size() - 2] - seam).normalized();
        REQUIRE_THAT(outgoing.dot(into_corner), Catch::Matchers::WithinAbs(0., 0.01));
        const Vec2d move = Vec2d(line.new_X(self), line.new_Y(self)) - seam;
        // The legacy corner move is 20% of the nozzle diameter, turned 30 degrees
        // from the outgoing edge into the square. Check both components independently.
        const double distance = 0.2 * std::stod(nozzle_diameter);
        CHECK_THAT(move.dot(outgoing), Catch::Matchers::WithinAbs(distance * std::sqrt(3.) / 2., 0.003));
        CHECK_THAT(move.dot(into_corner), Catch::Matchers::WithinAbs(distance / 2., 0.003));
    });
    REQUIRE(moves == 5);
}

TEST_CASE("Inward wipe remains valid after wipe on loops moves the nozzle", "[Wipe][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *comments = GENERATE("0", "1");
    CAPTURE(comments);
    INFO("wall generator: " << wall_generator);

    DynamicPrintConfig config = wipe_config(wall_generator, false, "50%", "10%", true);
    config.set_deserialize_strict({{"gcode_comments", comments}});
    const std::string loop_move = slice({make_cube(10., 10., 1.)}, config);
    config.set_deserialize_strict({{"wipe_inward", "1"}});
    const std::string combined = slice({make_cube(10., 10., 1.)}, config);
    config.set_deserialize_strict({{"wipe_on_loops", "0"}});
    const std::string inward_only = slice({make_cube(10., 10., 1.)}, config);

    for (const std::string *output : {&loop_move, &combined}) {
        INFO("wipe_inward: " << (output == &combined));
        std::map<double, std::vector<Vec2d>> loop_moves_by_layer;
        const auto &role_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
        const auto &wipe_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Wipe_Start);
        ExtrusionRole role = erNone;
        bool after_extrusion = false;
        GCodeReader parser;
        parser.apply_config(config);
        parser.parse_buffer(*output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            if (line.comment().find(role_tag) == 0) {
                role = ExtrusionEntity::string_to_role(line.comment().substr(role_tag.size()));
                after_extrusion = false;
            }
            if (line.comment().find(wipe_tag) == 0)
                after_extrusion = false;
            if (role != erExternalPerimeter || line.dist_XY(self) <= EPSILON)
                return;
            if (line.extruding(self)) {
                after_extrusion = true;
            } else if (after_extrusion) {
                loop_moves_by_layer[line.new_Z(self)].emplace_back(line.new_X(self), line.new_Y(self));
                after_extrusion = false;
            }
        });

        // The 1 mm cube at 0.2 mm layer height has one external loop on each of five layers.
        const auto trajectories = wipe_trajectories(*output);
        REQUIRE(loop_moves_by_layer.size() == 5);
        for (size_t layer = 1; layer <= 5; ++layer) {
            const double z = layer * 0.2;
            const auto moves = std::find_if(loop_moves_by_layer.begin(), loop_moves_by_layer.end(),
                [z](const auto &entry) { return std::abs(entry.first - z) < 0.001; });
            REQUIRE(moves != loop_moves_by_layer.end());
            REQUIRE(moves->second.size() == 1);
            const auto wipe = std::find_if(trajectories.begin(), trajectories.end(), [&](const WipeTrajectory &trajectory) {
                return std::abs(trajectory.z - z) < 0.001 &&
                       (trajectory.start - moves->second.front()).norm() < 0.001;
            });
            REQUIRE(wipe != trajectories.end());
            // The configured 2 mm wipe must be measured from the inward move's
            // endpoint, including when wipe_inward is off (set_last_pos regression).
            CHECK_THAT(trajectory_length(*wipe), Catch::Matchers::WithinAbs(2., 0.003));
        }
    }

    const std::vector<WipeTrajectory> combined_trajectories = wipe_trajectories(combined);
    const std::vector<WipeTrajectory> inward_trajectories = wipe_trajectories(inward_only);
    REQUIRE_FALSE(combined_trajectories.empty());
    REQUIRE(combined_trajectories.size() == inward_trajectories.size());
    REQUIRE(trajectories_differ(wipe_destinations(combined), wipe_destinations(loop_move)));

    bool start_changed = false;
    for (size_t i = 0; i < combined_trajectories.size(); ++i) {
        start_changed = start_changed ||
            (combined_trajectories[i].start - inward_trajectories[i].start).norm() > 0.01;
        REQUIRE_THAT(trajectory_length(combined_trajectories[i]),
                     Catch::Matchers::WithinAbs(trajectory_length(inward_trajectories[i]), 0.01));
    }
    REQUIRE(start_changed);
}

namespace {

// Non-Bambu (WipeTower2) two-filament plate. Cone angle stays in the config for every
// wall type, but only a cone wall prints or reserves the cone.
DynamicPrintConfig cone_reservation_config(const char *wall_type, const char *cone_angle)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values           = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values            = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values             = {0.4, 0.4};
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = true;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.set_key_value("printer_model", new ConfigOptionString("Snapmaker U1"));
    config.set_deserialize_strict({
        {"wipe_tower_wall_type",       wall_type},
        {"wipe_tower_cone_angle",      cone_angle},
        {"wipe_tower_x",               "30"},
        {"wipe_tower_y",               "80"},
        {"prime_tower_width",          "35"},
        {"prime_tower_brim_width",     "3"},
        {"layer_height",               "0.3"},
        {"initial_layer_print_height", "0.3"},
        {"skirt_loops",                "1"},
        {"brim_type",                  "no_brim"},
        {"enable_support",             "0"},
        {"gcode_comments",             "1"},
        {"machine_end_gcode",          ""},
        {"machine_start_gcode",
         "MIN={first_layer_print_min[0]},{first_layer_print_min[1]}\n"
         "MAX={first_layer_print_max[0]},{first_layer_print_max[1]}\n"},
    });
    return config;
}

// A 120 mm object makes the tower tall enough that a 30 degree cone reaches the
// thin plate in front of it. Positions are instance offsets so arrange_objects
// is not used (Edge's init_print would move them).
void load_tall_and_plate(Print &print, Model &model, const DynamicPrintConfig &config)
{
    ModelObject *tall = model.add_object();
    tall->name        = "tall.stl";
    tall->add_volume(make_cube(20., 20., 120.));
    tall->add_instance()->set_offset(Vec3d(150., 150., 0.));
    tall->ensure_on_bed();

    ModelObject *plate = model.add_object();
    plate->name        = "plate.stl";
    plate->add_volume(make_cube(30., 32., 2.));
    plate->add_instance()->set_offset(Vec3d(40., 30., 0.));
    plate->ensure_on_bed();
    plate->volumes.front()->config.set("extruder", 2);

    // The wipe tower only rises as far as the last layer with a tool change, so a tall
    // second-extruder pillar keeps changing tools up to the top of the tall object.
    ModelObject *pillar = model.add_object();
    pillar->name        = "pillar.stl";
    pillar->add_volume(make_cube(6., 6., 120.));
    pillar->add_instance()->set_offset(Vec3d(150., 185., 0.));
    pillar->ensure_on_bed();
    pillar->volumes.front()->config.set("extruder", 2);

    print.apply(model, config);
    print.set_status_silent();
}

std::string gcode_line_starting(const std::string &gcode, const char *prefix)
{
    const size_t pos = gcode.find(prefix);
    REQUIRE(pos != std::string::npos);
    const size_t end = gcode.find('\n', pos);
    return gcode.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

} // namespace

TEST_CASE("Only a cone wall gives the wipe tower a stabilization cone", "[Wipe][WipeTower]")
{
    const char *wall_type = GENERATE("rectangle", "rib", "cone");
    INFO("wall type " << wall_type);

    DynamicPrintConfig config = cone_reservation_config(wall_type, "30");
    Print              print;
    Model              model;
    load_tall_and_plate(print, model, config);
    print.process();
    REQUIRE(print.has_wipe_tower());
    REQUIRE_FALSE(print.is_BBL_printer());

    const BoundingBox footprint(print.first_layer_wipe_tower_corners());
    REQUIRE(footprint.defined);
    const double plate_max_y = 30. + 32.;
    INFO("tower depth " << print.wipe_tower_data().depth << ", footprint min y " << unscaled(footprint.min.y())
                        << ", max y " << unscaled(footprint.max.y()));
    if (std::string(wall_type) == "cone") {
        CHECK(unscaled(footprint.min.y()) < plate_max_y);
        CHECK(print.get_conflict_result().has_value());
    } else {
        CHECK(unscaled(footprint.min.y()) > plate_max_y);
        CHECK_FALSE(print.get_conflict_result().has_value());
    }
}

TEST_CASE("A non-cone wall ignores wipe_tower_cone_angle in the first-layer footprint and G-code", "[Wipe][WipeTower]")
{
    const char *wall_type = GENERATE("rectangle", "rib");
    INFO("wall type " << wall_type);

    auto slice_case = [](const char *wall, const char *cone_angle) {
        DynamicPrintConfig config = cone_reservation_config(wall, cone_angle);
        Print              print;
        Model              model;
        load_tall_and_plate(print, model, config);
        const std::string gcode = Test::gcode(print);
        REQUIRE(print.has_wipe_tower());
        return std::make_tuple(BoundingBox(print.first_layer_wipe_tower_corners()),
                               gcode_line_starting(gcode, "MIN="),
                               gcode_line_starting(gcode, "MAX="),
                               print.get_conflict_result().has_value());
    };

    const auto [box_30, min_30, max_30, conflict_30] = slice_case(wall_type, "30");
    const auto [box_0, min_0, max_0, conflict_0]     = slice_case(wall_type, "0");
    REQUIRE(box_30.defined);
    REQUIRE(box_0.defined);
    CHECK(box_30.min == box_0.min);
    CHECK(box_30.max == box_0.max);
    CHECK(min_30 == min_0);
    CHECK(max_30 == max_0);
    CHECK_FALSE(conflict_30);
    CHECK_FALSE(conflict_0);
}

TEST_CASE("A cone wall's first-layer footprint grows with the cone angle", "[Wipe][WipeTower]")
{
    auto slice_case = [](const char *cone_angle) {
        DynamicPrintConfig config = cone_reservation_config("cone", cone_angle);
        Print              print;
        Model              model;
        load_tall_and_plate(print, model, config);
        const std::string gcode = Test::gcode(print);
        REQUIRE(print.has_wipe_tower());
        return std::make_tuple(BoundingBox(print.first_layer_wipe_tower_corners()),
                               gcode_line_starting(gcode, "MIN="),
                               gcode_line_starting(gcode, "MAX="));
    };

    const auto [box_30, min_30, max_30] = slice_case("30");
    const auto [box_0, min_0, max_0]    = slice_case("0");
    REQUIRE(box_30.defined);
    REQUIRE(box_0.defined);
    CHECK(unscaled(box_30.min.y()) < unscaled(box_0.min.y()));
    // The plate and the tower's own corners still set the -X / -Y corner of the print hull, and the objects at
    // Y 150-190 set +Y / +X, so the cone (which grows toward the plate inside that hull) leaves both G-code
    // lines unchanged here; the footprint above is what the cone changes.
    CHECK(min_30 == min_0);
    CHECK(max_30 == max_0);
}
