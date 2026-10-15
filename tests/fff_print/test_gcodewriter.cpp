#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Geometry/ArcWelder.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

SCENARIO("lift() is not ignored after unlift() at normal values of Z", "[GCodeWriter]") {
    GIVEN("A config from a file and a single extruder.") {
        GCodeWriter writer;
        GCodeConfig &config = writer.config;
        // ConfigBase::load() dispatches on the file extension and, since Bambu
        // commented load_from_ini() out of it (Config.cpp:783), rejects .ini with
        // "unsupported format for config file" and returns an EMPTY config - which
        // left z_hop at its 0 default and made every lift() below emit nothing.
        // load_from_ini() itself is still present and works, so call it directly.
        config.load_from_ini(std::string(TEST_DATA_DIR) + "/fff_print_tests/test_gcodewriter/config_lift_unlift.ini", ForwardCompatibilitySubstitutionRule::Disable);

        // FORK BEHAVIOUR (Bambu "lazy lift"): GCodeWriter::lift() no longer emits the Z
        // move itself. Unless spiral_vase is set it only records the pending lift in
        // m_to_lift (GCodeWriter.cpp:866-875) and returns "", leaving travel_to_xy() to
        // fold the hop into the next travel move. The upstream contract this scenario was
        // written against - "lift() returns g-code" - therefore only holds on the
        // spiral_vase path, which is what these calls now exercise. The property under
        // test is unchanged: a lift after an unlift must not be ignored.
        std::vector<unsigned int> extruder_ids {0};
        writer.set_extruders(extruder_ids);
        writer.set_extruder(0);

        WHEN("Z is set to 203") {
            double trouble_Z = 203;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 500003") {
            double trouble_Z = 500003;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                        }
                    }
                }
            }
        }
        WHEN("Z is set to 10.3") {
            double trouble_Z = 10.3;
            writer.travel_to_z(trouble_Z);
            AND_WHEN("GcodeWriter::Lift() is called") {
                REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                AND_WHEN("Z is moved post-lift to the same delta as the config Z lift") {
                    REQUIRE(writer.travel_to_z(trouble_Z + config.z_hop.values[0]).size() == 0);
                    AND_WHEN("GCodeWriter::Unlift() is called") {
                        REQUIRE(writer.unlift().size() == 0); // we're the same height so no additional move happens.
                        THEN("GCodeWriter::Lift() emits gcode.") {
                            REQUIRE(writer.lift(LiftType::NormalLift, true).size() > 0);
                        }
                    }
                }
            }
        }
		// The test above will fail for trouble_Z == 9007199254740992, where trouble_Z + 1.5 will be rounded to trouble_Z + 2.0 due to double mantisa overflow.
    }
}

SCENARIO("set_speed emits values with fixed-point output.", "[GCodeWriter]") {

    GIVEN("GCodeWriter instance") {
        GCodeWriter writer;
        WHEN("set_speed is called to set speed to 99999.123") {
            THEN("Output string is G1 F99999.123") {
                REQUIRE_THAT(writer.set_speed(99999.123), Catch::Equals("G1 F99999.123\n"));
            }
        }
        WHEN("set_speed is called to set speed to 1") {
            THEN("Output string is G1 F1") {
                REQUIRE_THAT(writer.set_speed(1.0), Catch::Equals("G1 F1\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200022") {
            THEN("Output string is G1 F203.2") {
                REQUIRE_THAT(writer.set_speed(203.200022), Catch::Equals("G1 F203.2\n"));
            }
        }
        WHEN("set_speed is called to set speed to 203.200522") {
            THEN("Output string is G1 F203.201") {
                REQUIRE_THAT(writer.set_speed(203.200522), Catch::Equals("G1 F203.201\n"));
            }
        }
    }
}

// Orca #15848: custom G-code e_retracted R/W must use the same retract storage as
// retract()/unretract(). Edge keeps a single static m_share_retracted (not Orca's
// per-physical vector / filament_map / extruder_id()), so these cases cover shared
// vs per-filament and relative vs absolute E against that scalar model.
//
// Deferred from upstream:
// - "follows the physical extruder mapping" — needs filament_map + vector share.
// - "Start G-code retraction is repaid..." — needs multifilament_config + a full
//   slice with placeholder start G-code. Unit-level retract/unretract below is
//   the Edge stand-in.

namespace {

void parse_retract_unretract(const std::string &gcode, const GCodeConfig &config, double &retracted, double &unretracted)
{
    retracted   = 0.;
    unretracted = 0.;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.retracting(self))
            retracted -= line.dist_E(self);
        else if (line.extruding(self))
            unretracted += line.dist_E(self);
    });
}

} // namespace

TEST_CASE("Custom retraction state controls generated retract and unretract moves", "[GCodeWriter][Retraction]")
{
    const bool   shared            = GENERATE(false, true);
    const bool   relative_e        = GENERATE(false, true);
    const double custom_retraction = GENERATE(0., 0.5, 0.8);
    CAPTURE(shared, relative_e, custom_retraction);

    GCodeWriter writer;
    writer.config.single_extruder_multi_material.value = shared;
    writer.config.use_relative_e_distances.value       = relative_e;
    writer.config.retraction_length.values             = {0.6};
    writer.config.retract_restart_extra.values         = {0.};
    writer.set_extruders({0, 1});
    writer.set_extruder(1);
    REQUIRE(writer.extruder() != nullptr);

    writer.extruder()->set_retracted(custom_retraction, 0.);
    std::string    gcode            = writer.retract();
    const double   total_retraction = std::max(custom_retraction, 0.6);
    CHECK_THAT(writer.extruder()->retracted(), WithinAbs(total_retraction, 1e-9));
    gcode += writer.unretract();
    CHECK_THAT(writer.extruder()->retracted(), WithinAbs(0., 1e-9));

    double retracted = 0., unretracted = 0.;
    parse_retract_unretract(gcode, writer.config, retracted, unretracted);
    CHECK_THAT(retracted, WithinAbs(total_retraction - custom_retraction, 1e-6));
    CHECK_THAT(unretracted, WithinAbs(total_retraction, 1e-6));
}

TEST_CASE("Custom retraction state uses the shared SEMM scalar", "[GCodeWriter][Retraction]")
{
    GCodeWriter writer;
    writer.config.single_extruder_multi_material.value = true;
    writer.config.use_relative_e_distances.value       = true;
    writer.config.retraction_length.values             = {0.6};
    writer.config.retract_restart_extra.values         = {0.};
    writer.set_extruders({0, 1});
    writer.set_extruder(1);
    REQUIRE(writer.extruder() != nullptr);

    writer.extruder()->set_retracted(0.5, 0.2);
    // One static m_share_retracted: every SEMM filament sees the same length.
    CHECK_THAT(writer.extruders()[0].retracted(), WithinAbs(0.5, 1e-9));
    CHECK_THAT(writer.extruders()[1].retracted(), WithinAbs(0.5, 1e-9));
    CHECK_THAT(writer.extruder()->unretract(), WithinAbs(0.7, 1e-9));
    CHECK_THAT(writer.extruders()[0].retracted(), WithinAbs(0., 1e-9));
    CHECK_THAT(writer.extruders()[1].retracted(), WithinAbs(0., 1e-9));

    writer.extruder()->set_retracted(0.5, 0.2);
    writer.extruder()->set_retracted(0., 0.2);
    CHECK_THAT(writer.extruder()->retracted(), WithinAbs(0., 1e-9));
    CHECK_THAT(writer.extruder()->restart_extra(), WithinAbs(0., 1e-9));
    CHECK(writer.unretract().empty());
}

TEST_CASE("Custom retraction state stays per-filament when SEMM is off", "[GCodeWriter][Retraction]")
{
    GCodeWriter writer;
    writer.config.single_extruder_multi_material.value = false;
    writer.config.use_relative_e_distances.value       = true;
    writer.config.retraction_length.values             = {0.6};
    writer.set_extruders({0, 1});
    writer.set_extruder(1);
    REQUIRE(writer.extruder() != nullptr);

    writer.extruder()->set_retracted(0.5, 0.2);
    CHECK_THAT(writer.extruders()[0].retracted(), WithinAbs(0., 1e-9));
    CHECK_THAT(writer.extruders()[1].retracted(), WithinAbs(0.5, 1e-9));
    CHECK_THAT(writer.extruder()->restart_extra(), WithinAbs(0.2, 1e-9));
}

TEST_CASE("GCodeWriter::toolchange throws when the extruder is not registered", "[GCodeWriter][GCode]")
{
    GCodeWriter writer;
    writer.set_extruders({0});
    REQUIRE_THROWS_AS(writer.toolchange(2), SlicingError);
    REQUIRE(writer.extruder() == nullptr);
}

TEST_CASE("Acceleration and velocity limit commands print their values in general notation", "[GCodeWriter]")
{
    enum class Command { Print, Travel, KlipperLimits };
    struct Case
    {
        GCodeFlavor              flavor;
        Command                  command;
        unsigned int             acceleration;
        double                   jerk;
        bool                     comments;
        std::vector<std::string> present;
        std::vector<std::string> absent;
    };
    // accel_to_decel_factor is 50%, so ACCEL_TO_DECEL is half the acceleration.
    const Case cases[] = {
        {gcfKlipper, Command::KlipperLimits, 2000000, 25. / 3., false,
         {"SET_VELOCITY_LIMIT ACCEL=2000000 ", "ACCEL_TO_DECEL=1e+06 ", "SQUARE_CORNER_VELOCITY=8.33333\n"}, {}},
        {gcfKlipper, Command::KlipperLimits, 12345, 0., false, {"ACCEL=12345 ", "ACCEL_TO_DECEL=6172.5\n"}, {"SQUARE_CORNER_VELOCITY"}},
        {gcfKlipper, Command::KlipperLimits, 0, 0.25, true, {"SQUARE_CORNER_VELOCITY=0.25 ", "; adjust VELOCITY_LIMIT"}, {"ACCEL"}},
        {gcfKlipper, Command::Print, 3001, 0., true, {"ACCEL=3001 ", "ACCEL_TO_DECEL=1500.5 ", "; adjust ACCEL_TO_DECEL", "; adjust acceleration"}, {}},
        {gcfMarlinFirmware, Command::Print, 2500, 0., false, {"M204 P2500\n"}, {}},
        {gcfMarlinFirmware, Command::Travel, 7000, 0., false, {"M204 T7000\n"}, {}},
        {gcfRepRapFirmware, Command::Travel, 7000, 0., true, {"M204 T7000 ", "; adjust acceleration"}, {}},
        {gcfMarlinLegacy, Command::Print, 2500, 0., false, {"M204 S2500\n"}, {}},
        {gcfRepetier, Command::Print, 2500, 0., false, {"M201 X2500 Y2500\n"}, {}},
        {gcfRepetier, Command::Travel, 7000, 0., false, {"M202 X7000 Y7000\n"}, {}},
    };

    for (const Case &c : cases) {
        DYNAMIC_SECTION("flavor " << int(c.flavor) << " command " << int(c.command) << " accel " << c.acceleration) {
            struct CommentGuard
            {
                bool saved = GCodeWriter::full_gcode_comment;
                ~CommentGuard() { GCodeWriter::full_gcode_comment = saved; }
            } comment_guard;
            GCodeWriter::full_gcode_comment = c.comments;

            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(c.flavor));
            config.option<ConfigOptionBools>("accel_to_decel_enable")->values.assign(1, true);
            config.option<ConfigOptionPercents>("accel_to_decel_factor")->values.assign(1, 50.);
            for (const char *limit : {"machine_max_acceleration_extruding", "machine_max_acceleration_travel", "machine_max_acceleration_x",
                                      "machine_max_acceleration_y", "machine_max_jerk_x", "machine_max_jerk_y"}) {
                std::vector<double> &values = config.option<ConfigOptionFloats>(limit)->values;
                std::fill(values.begin(), values.end(), 0.);
            }
            PrintConfig print_config;
            print_config.apply(config, true);
            GCodeWriter writer;
            writer.apply_print_config(print_config);

            const std::string line = c.command == Command::Print         ? writer.set_print_acceleration(c.acceleration) :
                                     c.command == Command::Travel        ? writer.set_travel_acceleration(c.acceleration) :
                                                                           writer.set_accel_and_jerk(c.acceleration, c.jerk);
            INFO(line);
            for (const std::string &token : c.present)
                CHECK(line.find(token) != std::string::npos);
            for (const std::string &token : c.absent)
                CHECK(line.find(token) == std::string::npos);
        }
    }
}

TEST_CASE("GCodeWriter append overloads emit the same line as the returning overloads", "[GCodeWriter]")
{
    GCodeWriter writer;
    std::string appended;
    writer.set_speed(appended, 1800.);
    CHECK(appended == writer.set_speed(1800.));
}

// Port OrcaSlicer #12824 (#12244): Klipper's SET_VELOCITY_LIMIT ACCEL= limits every kind of motion, so the
// writer must clamp to the smallest of the extruding limit and the X/Y limits, not the extruding limit alone.
namespace {
std::string klipper_accel_line(GCodeFlavor flavor, double extruding, double x, double y, unsigned int requested)
{
    PrintConfig print_config;
    print_config.gcode_flavor.value = flavor;
    print_config.machine_max_acceleration_extruding.values = { extruding, extruding };
    print_config.machine_max_acceleration_x.values         = { x, x };
    print_config.machine_max_acceleration_y.values         = { y, y };
    GCodeWriter writer;
    writer.apply_print_config(print_config);
    return writer.set_print_acceleration(requested);
}
} // namespace

TEST_CASE("Klipper acceleration is capped by the X and Y limits too", "[GCodeWriter][Klipper][U1]")
{
    SECTION("an X limit below the extruding limit wins") {
        const std::string gcode = klipper_accel_line(gcfKlipper, 10000., 8700., 10000., 10000);
        CHECK(gcode.find("SET_VELOCITY_LIMIT ACCEL=8700") != std::string::npos);
    }
    SECTION("a Y limit below the extruding limit wins") {
        const std::string gcode = klipper_accel_line(gcfKlipper, 10000., 10000., 6000., 10000);
        CHECK(gcode.find("SET_VELOCITY_LIMIT ACCEL=6000") != std::string::npos);
    }
    SECTION("the extruding limit still wins when it is the smallest") {
        const std::string gcode = klipper_accel_line(gcfKlipper, 5000., 20000., 20000., 10000);
        CHECK(gcode.find("SET_VELOCITY_LIMIT ACCEL=5000") != std::string::npos);
    }
    SECTION("equal limits (the U1 profile) change nothing") {
        const std::string gcode = klipper_accel_line(gcfKlipper, 20000., 20000., 20000., 10000);
        CHECK(gcode.find("SET_VELOCITY_LIMIT ACCEL=10000") != std::string::npos);
    }
    SECTION("a zero axis limit means unset and is ignored") {
        const std::string gcode = klipper_accel_line(gcfKlipper, 10000., 0., 0., 12000);
        CHECK(gcode.find("SET_VELOCITY_LIMIT ACCEL=10000") != std::string::npos);
    }
    SECTION("other flavours keep using only the extruding limit") {
        const std::string gcode = klipper_accel_line(gcfMarlinFirmware, 10000., 8700., 8700., 12000);
        CHECK(gcode.find("M204 P10000") != std::string::npos);
    }
}

// Batch 2C: spiral lift (Orca #11430, #11468, #11556, #12564, #13634, #15118).
namespace {

struct SpiralLine
{
    std::string            cmd;
    std::map<char, double> v;
    bool has(char c) const { return v.count(c) != 0; }
};

std::vector<SpiralLine> parse_spiral_gcode(const std::string &gcode)
{
    std::vector<SpiralLine> out;
    std::istringstream      in(gcode);
    std::string             line;
    while (std::getline(in, line)) {
        const std::string  body = line.substr(0, line.find(';'));
        std::istringstream words(body);
        SpiralLine         l;
        if (!(words >> l.cmd))
            continue;
        std::string w;
        while (words >> w)
            if (w.size() > 1 && std::isalpha((unsigned char) w[0]))
                l.v[w[0]] = std::stod(w.substr(1));
        out.push_back(std::move(l));
    }
    return out;
}

struct SpiralSetup
{
    bool                arc_fitting = false;
    double              resolution  = 0.012;
    Vec2d               plate_offset{0., 0.};
    Vec2d               start{100., 100.}; // plate coordinates
    Vec2d               target{150., 100.};
    unsigned int        filament = 0;
    Pointfs             bed      = {Vec2d(0, 0), Vec2d(200, 0), Vec2d(200, 200), Vec2d(0, 200)};
    std::vector<Vec2ds> extruder_areas;
    std::vector<int>    filament_map = {1, 1};
};

constexpr double spiral_z_hop   = 0.4;
constexpr double spiral_layer_z = 0.2;

// The writer's spiral radius for a 0.4 mm z-hop at the default 3 degree travel slope.
double spiral_radius() { return spiral_z_hop / (2. * PI * std::atan(3. * PI / 180.)); }

// Lift with LiftType::SpiralLift at `start`, then travel to `target` on the same layer.
std::string spiral_lift_gcode(const SpiralSetup &s)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_arc_fitting", new ConfigOptionBool(s.arc_fitting));
    config.set_key_value("resolution", new ConfigOptionFloat(s.resolution));
    config.set_key_value("z_hop", new ConfigOptionFloats{spiral_z_hop, spiral_z_hop});
    config.set_key_value("travel_slope", new ConfigOptionFloats{3., 3.});
    config.set_key_value("retract_lift_above", new ConfigOptionFloats{0., 0.});
    config.set_key_value("retract_lift_below", new ConfigOptionFloats{0., 0.});
    config.set_key_value("printable_area", new ConfigOptionPoints(s.bed));
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups(s.extruder_areas));
    config.set_key_value("filament_map", new ConfigOptionInts(s.filament_map));
    PrintConfig print_config;
    print_config.apply(config, true);

    GCodeWriter writer;
    writer.apply_print_config(print_config);
    writer.set_extruders({0, 1});
    writer.set_extruder(s.filament);
    writer.set_xy_offset(s.plate_offset.x(), s.plate_offset.y());
    // The writer keeps positions with the plate offset added and emits plate coordinates.
    writer.set_position(Vec3d(s.start.x() + s.plate_offset.x(), s.start.y() + s.plate_offset.y(), spiral_layer_z));
    writer.set_current_position_clear(true);

    std::string gcode = writer.lift(LiftType::SpiralLift);
    gcode += writer.travel_to_xyz(Vec3d(s.target.x() + s.plate_offset.x(), s.target.y() + s.plate_offset.y(), spiral_layer_z));
    return gcode;
}

// The G1 points of an emulated spiral: XYZ moves without a feedrate (the travel after it carries one).
std::vector<SpiralLine> spiral_points(const std::vector<SpiralLine> &lines)
{
    std::vector<SpiralLine> out;
    for (const SpiralLine &l : lines)
        if (l.cmd == "G1" && l.has('X') && l.has('Y') && l.has('Z') && !l.has('F'))
            out.push_back(l);
    return out;
}

size_t count_cmd(const std::vector<SpiralLine> &lines, const std::string &cmd)
{
    return size_t(std::count_if(lines.begin(), lines.end(), [&cmd](const SpiralLine &l) { return l.cmd == cmd; }));
}

size_t expected_spiral_segments(double radius, double resolution)
{
    return resolution > 0. ? std::clamp(Geometry::ArcWelder::arc_discretization_steps(radius, 2. * PI, resolution), size_t(8), size_t(128)) :
                             size_t(128);
}

} // namespace

TEST_CASE("Spiral lift without arc fitting is a circle of G1 moves in plate coordinates", "[GCodeWriter][SpiralLift]")
{
    // #11430: no G2/G3 when arc fitting is off. #11468: the points are in plate coordinates on every plate.
    const Vec2d offsets[] = {Vec2d(0., 0.), Vec2d(-275.5, 12.25)};
    for (const Vec2d &offset : offsets) {
        DYNAMIC_SECTION("plate offset " << offset.x() << "," << offset.y()) {
            SpiralSetup s;
            s.plate_offset          = offset;
            const std::string gcode = spiral_lift_gcode(s);
            INFO(gcode);
            const std::vector<SpiralLine> lines = parse_spiral_gcode(gcode);
            CHECK(count_cmd(lines, "G2") == 0);
            CHECK(count_cmd(lines, "G3") == 0);
            CHECK(count_cmd(lines, "G17") == 0);

            const double                  r      = spiral_radius();
            const std::vector<SpiralLine> points = spiral_points(lines);
            REQUIRE(points.size() == expected_spiral_segments(r, s.resolution));

            // The travel goes along +X, so the centre is one radius to its left (+Y).
            const Vec2d center = s.start + Vec2d(0., r);
            double      last_z = spiral_layer_z;
            for (const SpiralLine &p : points) {
                CHECK_THAT((Vec2d(p.v.at('X'), p.v.at('Y')) - center).norm(), WithinAbs(r, 0.002));
                CHECK(p.v.at('Z') > last_z);
                last_z = p.v.at('Z');
            }
            CHECK_THAT(points.back().v.at('X'), WithinAbs(s.start.x(), 0.0015));
            CHECK_THAT(points.back().v.at('Y'), WithinAbs(s.start.y(), 0.0015));
            CHECK_THAT(points.back().v.at('Z'), WithinAbs(spiral_layer_z + spiral_z_hop, 0.0015));

            // Then the travel itself, at the lifted height.
            const SpiralLine &travel = lines.back();
            CHECK(travel.cmd == "G1");
            CHECK_THAT(travel.v.at('X'), WithinAbs(s.target.x(), 0.0015));
            CHECK_THAT(travel.v.at('Y'), WithinAbs(s.target.y(), 0.0015));
            CHECK_THAT(travel.v.at('Z'), WithinAbs(spiral_layer_z + spiral_z_hop, 0.0015));
        }
    }
}

TEST_CASE("Emulated spiral lift segments follow the slicing resolution", "[GCodeWriter][SpiralLift]")
{
    // #11556 / #12564 / #15118: no chord deviates from the arc by more than the resolution,
    // 8 to 128 segments, and the upper bound for a resolution of 0.
    const double resolutions[] = {0.012, 0.05, 0.001, 0.};
    const double r             = spiral_radius();
    for (const double resolution : resolutions) {
        DYNAMIC_SECTION("resolution " << resolution) {
            SpiralSetup s;
            s.resolution                         = resolution;
            const std::vector<SpiralLine> points = spiral_points(parse_spiral_gcode(spiral_lift_gcode(s)));
            CHECK(points.size() == expected_spiral_segments(r, resolution));
            CHECK(points.size() >= 8);
            CHECK(points.size() <= 128);
        }
    }
    // The profile resolution 0.012 mm gives 23 segments for this radius.
    CHECK(expected_spiral_segments(r, 0.012) == 23);
}

TEST_CASE("Spiral lift with arc fitting is one G3 arc", "[GCodeWriter][SpiralLift]")
{
    SpiralSetup s;
    s.arc_fitting           = true;
    s.plate_offset          = Vec2d(-275.5, 12.25);
    const std::string gcode = spiral_lift_gcode(s);
    INFO(gcode);
    const std::vector<SpiralLine> lines = parse_spiral_gcode(gcode);
    CHECK(count_cmd(lines, "G17") == 1);
    REQUIRE(count_cmd(lines, "G3") == 1);
    CHECK(spiral_points(lines).empty());
    const SpiralLine &arc = *std::find_if(lines.begin(), lines.end(), [](const SpiralLine &l) { return l.cmd == "G3"; });
    CHECK_THAT(arc.v.at('Z'), WithinAbs(spiral_layer_z + spiral_z_hop, 0.0015));
    CHECK_THAT(arc.v.at('I'), WithinAbs(0., 0.0015));
    CHECK_THAT(arc.v.at('J'), WithinAbs(spiral_radius(), 0.0015));
    CHECK(arc.v.at('P') == 1.);
}

TEST_CASE("A spiral lift that would leave the printable area is a normal lift", "[GCodeWriter][SpiralLift]")
{
    // #13634. The circle's centre is one radius to the left of the travel direction.
    const double r = spiral_radius();
    for (const bool arc_fitting : {false, true}) {
        DYNAMIC_SECTION("arc fitting " << arc_fitting) {
            SpiralSetup s;
            s.arc_fitting = arc_fitting;
            s.start       = Vec2d(0.5, 100.);

            SECTION("centre outside the bed: normal lift") {
                s.target                            = Vec2d(0.5, 150.); // +Y: the centre is at x = 0.5 - r < 0
                const std::vector<SpiralLine> lines = parse_spiral_gcode(spiral_lift_gcode(s));
                CHECK(count_cmd(lines, "G3") == 0);
                CHECK(spiral_points(lines).empty());
                REQUIRE(lines.size() == 2);
                CHECK_FALSE(lines.front().has('X'));
                CHECK_THAT(lines.front().v.at('Z'), WithinAbs(spiral_layer_z + spiral_z_hop, 0.0015));
            }
            SECTION("circle inside the bed: spiral") {
                s.target                            = Vec2d(0.5, 50.); // -Y: the centre is at x = 0.5 + r, more than r from every edge
                const std::vector<SpiralLine> lines = parse_spiral_gcode(spiral_lift_gcode(s));
                if (arc_fitting)
                    CHECK(count_cmd(lines, "G3") == 1);
                else
                    CHECK(spiral_points(lines).size() == expected_spiral_segments(r, s.resolution));
            }
            SECTION("circle crossing an edge: normal lift") {
                s.start                             = Vec2d(100., 199.5);
                s.target                            = Vec2d(150., 199.5); // +X: the centre is at y = 199.5 + r, beyond y = 200
                const std::vector<SpiralLine> lines = parse_spiral_gcode(spiral_lift_gcode(s));
                CHECK(count_cmd(lines, "G3") == 0);
                CHECK(spiral_points(lines).empty());
            }
        }
    }
}

TEST_CASE("The spiral lift uses the printable area of the extruder the filament prints from", "[GCodeWriter][SpiralLift]")
{
    // #13634 on a Bambu H2D-like bed: the left extruder reaches x 0..325, the right one 25..350.
    // Our Extruder is a filament slot, so the extruder comes from filament_map (1-based).
    SpiralSetup s;
    s.arc_fitting    = true;
    s.bed            = {Vec2d(0, 0), Vec2d(350, 0), Vec2d(350, 320), Vec2d(0, 320)};
    s.extruder_areas = {Vec2ds{Vec2d(0, 0), Vec2d(325, 0), Vec2d(325, 320), Vec2d(0, 320)},
                        Vec2ds{Vec2d(25, 0), Vec2d(350, 0), Vec2d(350, 320), Vec2d(25, 320)}};
    s.start          = Vec2d(26., 100.);
    s.target         = Vec2d(26., 150.); // +Y: the centre is at x = 26 - r, left of the right extruder's x = 25
    REQUIRE(26. - spiral_radius() < 25.);

    SECTION("filament on the left extruder: spiral") {
        s.filament_map = {1, 2};
        s.filament     = 0;
        CHECK(count_cmd(parse_spiral_gcode(spiral_lift_gcode(s)), "G3") == 1);
    }
    SECTION("filament on the right extruder: normal lift") {
        s.filament_map = {1, 2};
        s.filament     = 1;
        CHECK(count_cmd(parse_spiral_gcode(spiral_lift_gcode(s)), "G3") == 0);
    }
    SECTION("first filament mapped to the right extruder: normal lift") {
        s.filament_map = {2, 1};
        s.filament     = 0;
        CHECK(count_cmd(parse_spiral_gcode(spiral_lift_gcode(s)), "G3") == 0);
    }
    SECTION("no per-extruder areas: the bed decides") {
        s.extruder_areas.clear();
        s.filament_map = {1, 2};
        s.filament     = 1;
        CHECK(count_cmd(parse_spiral_gcode(spiral_lift_gcode(s)), "G3") == 1);
    }
    SECTION("both areas merged into one group of eight points (older projects): split per extruder") {
        s.extruder_areas = {Vec2ds{Vec2d(0, 0), Vec2d(325, 0), Vec2d(325, 320), Vec2d(0, 320),
                                   Vec2d(25, 0), Vec2d(350, 0), Vec2d(350, 320), Vec2d(25, 320)}};
        s.filament_map   = {1, 2};
        s.filament       = 0;
        CHECK(count_cmd(parse_spiral_gcode(spiral_lift_gcode(s)), "G3") == 1);
        s.filament = 1;
        CHECK(count_cmd(parse_spiral_gcode(spiral_lift_gcode(s)), "G3") == 0);
    }
}

// Snapmaker #993: generated Z travel respects machine_max_speed_z (pure Z travel, lift/unlift, the Z component
// of XYZ travel and of the spiral lift). Ported from the upstream tests (open PR, head 06583b935f) to this
// tree's writer, plus the Edge adaptations: the layer-change Z is deferred through defer_z_move(), and the
// spiral lift is slowed only as far as its Z component needs.
namespace {

// GCodeWriter::full_gcode_comment is a process-wide static (true by default, set per export), so
// these exact-string cases switch it off and put it back, whatever ran before them.
struct NoFullComments
{
    bool saved = GCodeWriter::full_gcode_comment;
    NoFullComments() { GCodeWriter::full_gcode_comment = false; }
    ~NoFullComments() { GCodeWriter::full_gcode_comment = saved; }
};

void setup_z_speed_writer(GCodeWriter &writer, double travel_speed, double travel_speed_z, double max_speed_z,
                          bool arc_fitting = false, double z_hop = 1.5)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("travel_speed", new ConfigOptionFloats{travel_speed});
    config.set_key_value("travel_speed_z", new ConfigOptionFloat(travel_speed_z));
    config.set_key_value("machine_max_speed_z", new ConfigOptionFloats{max_speed_z, 5.});
    config.set_key_value("z_hop", new ConfigOptionFloats{z_hop});
    config.set_key_value("travel_slope", new ConfigOptionFloats{3.});
    config.set_key_value("retract_lift_above", new ConfigOptionFloats{0.});
    config.set_key_value("retract_lift_below", new ConfigOptionFloats{0.});
    config.set_key_value("enable_arc_fitting", new ConfigOptionBool(arc_fitting));
    PrintConfig print_config;
    print_config.apply(config, true);
    writer.apply_print_config(print_config);
    writer.set_is_first_layer(false);
    writer.set_extruders({0});
    writer.set_extruder(0);
}

// The highest Z velocity (mm/s) of the G1 moves in `gcode` that change Z, read from `start`.
double max_g1_z_velocity(const std::string &gcode, const Vec3d &start, int *z_moves = nullptr)
{
    std::ostringstream preamble;
    preamble << "G1 X" << start.x() << " Y" << start.y() << " Z" << start.z() << " F6000\n";
    double      worst = 0.;
    int         count = 0;
    bool        first = true;
    GCodeReader reader;
    reader.parse_buffer(preamble.str() + gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (first) {
            first = false;
            return;
        }
        if (line.cmd_is("G1") && line.dist_Z(self) != 0.) {
            ++count;
            const double dz       = std::abs(line.dist_Z(self));
            const double distance = std::hypot(line.dist_XY(self), dz);
            worst                 = std::max(worst, line.new_F(self) / 60. * dz / distance);
        }
    });
    if (z_moves != nullptr)
        *z_moves = count;
    return worst;
}

// Drop the F words (and the lines that only set a feedrate): what is left is the path.
std::string without_feedrates(const std::string &gcode)
{
    std::istringstream in(gcode);
    std::string        line, out;
    while (std::getline(in, line)) {
        std::string body = line.substr(0, line.find(';'));
        const size_t f   = body.find(" F");
        if (f != std::string::npos) {
            const size_t end = body.find(' ', f + 1);
            body.erase(f, end == std::string::npos ? std::string::npos : end - f);
        }
        while (!body.empty() && body.back() == ' ')
            body.pop_back();
        if (body != "G1")
            out += body + "\n";
    }
    return out;
}

} // namespace

TEST_CASE("Z travel respects the machine Z speed limit", "[GCodeWriter][ZSpeed]")
{
    NoFullComments no_comments;
    struct SpeedCase
    {
        double      travel_speed;
        double      travel_speed_z;
        double      max_speed_z;
        std::string feedrate;
        std::string restored; // the feedrate left active after an unknown-position travel, when capped
    };
    const SpeedCase cases[] = {
        {100., 0., 40., " F2400", "G1 F6000\n"}, // travel speed capped
        {30., 0., 40., " F1800", ""},            // below the limit: unchanged
        {100., 70., 40., " F2400", "G1 F4200\n"}, // travel_speed_z capped
        {100., 20., 40., " F1200", ""},          // travel_speed_z below the limit: unchanged
        {100., 0., 0., " F6000", ""},            // no limit: unchanged
    };
    for (const SpeedCase &c : cases) {
        CAPTURE(c.travel_speed, c.travel_speed_z, c.max_speed_z);
        GCodeWriter writer;
        setup_z_speed_writer(writer, c.travel_speed, c.travel_speed_z, c.max_speed_z);

        CHECK(writer.travel_to_z(10.) == "G1 Z10" + c.feedrate + "\n");
        CHECK(writer.travel_to_z(9.) == "G1 Z9" + c.feedrate + "\n");

        writer.set_position(Vec3d(100., 100., 9.));
        writer.set_current_position_clear(true);

        // Normal lift: the lift and the unlift are Z travel; the travel between them is not slowed.
        writer.lift(LiftType::NormalLift);
        const std::string hop = writer.travel_to_xyz(Vec3d(110., 100., 9.));
        CHECK(hop.find("G1 Z10.5" + c.feedrate + "\n") == 0);
        CHECK(hop.find("G1 X110 Y100 Z10.5 F" + std::to_string(int(c.travel_speed * 60.)) + "\n") != std::string::npos);
        CHECK(writer.unlift() == "G1 Z9" + c.feedrate + "\n");

        // Immediate lift (spiral vase).
        CHECK(writer.lift(LiftType::NormalLift, true) == "G1 Z10.5" + c.feedrate + "\n");
        CHECK(writer.unlift() == "G1 Z9" + c.feedrate + "\n");

        // A travel that only changes Z is a Z travel.
        CHECK(writer.travel_to_xyz(Vec3d(110., 100., 8.)) == "G1 X110 Y100 Z8" + c.feedrate + "\n");

        // Unknown position: XY first, then Z.
        writer.set_current_position_clear(false);
        // Edge: the follower (wipe tower or custom G-code without F words) keeps the feedrate it had before.
        CHECK(writer.travel_to_xyz(Vec3d(120., 100., 7.)) ==
              "G1 X120 Y100 F" + std::to_string(int(c.travel_speed * 60.)) + "\nG1 Z7" + c.feedrate + "\n" + c.restored);
    }
}

TEST_CASE("Z travel keeps the first-layer travel speed and follows a changed limit", "[GCodeWriter][ZSpeed]")
{
    NoFullComments no_comments;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("travel_speed", new ConfigOptionFloats{100.});
    config.set_key_value("travel_speed_z", new ConfigOptionFloat(0.));
    config.set_key_value("machine_max_speed_z", new ConfigOptionFloats{40., 40.});
    config.set_key_value("initial_layer_travel_speed", new ConfigOptionFloatsOrPercents({FloatOrPercent{25., true}}));
    PrintConfig print_config;
    print_config.apply(config, true);

    GCodeWriter writer;
    writer.apply_print_config(print_config);
    writer.set_extruders({0});
    writer.set_extruder(0);
    // First layer: 25 % of 100 mm/s, under the limit.
    CHECK(writer.travel_to_z(1.) == "G1 Z1 F1500\n");
    writer.set_is_first_layer(false);
    CHECK(writer.travel_to_z(2.) == "G1 Z2 F2400\n");

    config.set_key_value("machine_max_speed_z", new ConfigOptionFloats{10., 10.});
    print_config.apply(config, true);
    writer.apply_print_config(print_config);
    CHECK(writer.travel_to_z(3.) == "G1 Z3 F600\n");
    // XY travel is not touched.
    CHECK(writer.travel_to_xy(Vec2d(10., 0.)) == "G1 X10 Y0 F6000\n");
}

TEST_CASE("Combined XYZ travel limits its Z velocity without slowing shallow moves", "[GCodeWriter][ZSpeed]")
{
    NoFullComments no_comments;
    GCodeWriter writer;
    setup_z_speed_writer(writer, 100., 0., 40.);
    writer.set_current_position_clear(true);

    CHECK(writer.travel_to_xyz(Vec3d(10., 0., 10.)) == "G1 X10 Y0 Z10 F3394.112\n");
    CHECK(writer.travel_to_xyz(Vec3d(20., 0., 0.)) == "G1 X20 Y0 Z0 F3394.112\n");
    CHECK(writer.travel_to_xyz(Vec3d(120., 0., 1.)) == "G1 X120 Y0 Z1 F6000\n");
    CHECK(writer.travel_to_xyz(Vec3d(220., 0., 1.)) == "G1 X220 Y0 F6000\n");

    setup_z_speed_writer(writer, 100., 0., 0.);
    CHECK(writer.travel_to_xyz(Vec3d(230., 0., 11.)) == "G1 X230 Y0 Z11 F6000\n");

    // Measured between the emitted (rounded) coordinates, and the feedrate rounded down.
    setup_z_speed_writer(writer, 100., 0., 40.);
    writer.set_position(Vec3d(0.00049, 0., 0.00049));
    int z_moves = 0;
    CHECK(max_g1_z_velocity(writer.travel_to_xyz(Vec3d(1.00049, 0., 1.00051)), Vec3d::Zero(), &z_moves) <= 40. + 0.0001);
    CHECK(z_moves == 1);
}

TEST_CASE("Each Z-hop segment respects the Z velocity limit", "[GCodeWriter][ZSpeed]")
{
    NoFullComments no_comments;
    for (LiftType type : {LiftType::NormalLift, LiftType::LazyLift, LiftType::SpiralLift}) {
        for (double distance : {0.5, 10., 120.}) {
            CAPTURE(int(type), distance);
            GCodeWriter writer;
            setup_z_speed_writer(writer, 100., 0., 40.);
            writer.set_current_position_clear(true);
            std::string gcode = writer.travel_to_xyz(Vec3d(50., 50., 10.));
            writer.lift(type);
            gcode += writer.travel_to_xyz(Vec3d(50. + distance, 50. + distance, 10.));
            gcode += writer.unlift();
            INFO(gcode);

            int z_moves = 0;
            CHECK(max_g1_z_velocity(gcode, Vec3d::Zero(), &z_moves) <= 40. + 0.0001);
            CHECK(z_moves >= 3);
            if (type == LiftType::NormalLift || distance > 10.)
                CHECK(gcode.find(" F6000") != std::string::npos);
            if (type == LiftType::SpiralLift) {
                // Edge: the spiral is slowed only as far as its Z component needs (here not at all),
                // not to the Z limit itself.
                CHECK(gcode.find("G1 F6000\n") != std::string::npos);
            }
        }
    }
}

TEST_CASE("A deferred layer change caps its travel from the previous layer's Z and keeps the hop height",
          "[GCodeWriter][ZSpeed]")
{
    NoFullComments no_comments;
    GCodeWriter writer;
    setup_z_speed_writer(writer, 100., 0., 40., false, 0.4);

    SECTION("the travel climbs the layer height: capped") {
        writer.set_position(Vec3d(167.804, 167.985, 0.3));
        writer.set_current_position_clear(true);
        writer.defer_z_move(0.5);
        // Everything that reads the writer's position right after the layer change sees the new layer's Z.
        CHECK(writer.get_position().z() == Approx(0.5));
        CHECK(writer.travel_to_xyz(Vec3d(167.921, 167.804, 0.5), "", true) == "G1 X167.921 Y167.804 Z.5 F3528.285\n");
    }
    SECTION("the head is already at that Z: full travel speed") {
        writer.set_position(Vec3d(167.804, 167.985, 0.5));
        writer.set_current_position_clear(true);
        CHECK(writer.travel_to_xyz(Vec3d(167.921, 167.804, 0.5), "", true) == "G1 X167.921 Y167.804 Z.5 F6000\n");
    }
    SECTION("a lift before the layer change lifts above the new layer") {
        writer.set_position(Vec3d(167.804, 167.985, 0.3));
        writer.set_current_position_clear(true);
        writer.lift();                // GCode::change_layer() retracts and lifts first ...
        writer.defer_z_move(0.5);     // ... then moves the writer to the new layer
        CHECK(writer.travel_to_xyz(Vec3d(167.921, 167.804, 0.5), "", true).find("G1 Z.9 F2400\n") == 0);
        CHECK(writer.get_zhop() == Approx(0.4));
        CHECK(writer.unlift() == "G1 Z.5 F2400\n");
    }
    SECTION("a Z move or a new position ends the deferral") {
        writer.set_position(Vec3d(167.804, 167.985, 0.3));
        writer.set_current_position_clear(true);
        writer.defer_z_move(0.5);
        CHECK(writer.travel_to_z(0.7) == "G1 Z.7 F2400\n");
        CHECK(writer.travel_to_xyz(Vec3d(167.921, 167.804, 0.7), "", true) == "G1 X167.921 Y167.804 Z.7 F6000\n");

        writer.set_position(Vec3d(167.804, 167.985, 0.3));
        writer.defer_z_move(0.5);
        writer.set_position(Vec3d(167.804, 167.985, 0.5));
        CHECK(writer.travel_to_xyz(Vec3d(167.921, 167.804, 0.5), "", true) == "G1 X167.921 Y167.804 Z.5 F6000\n");
    }
}

TEST_CASE("A spiral lift at a layer change keeps its path and caps only its Z component", "[GCodeWriter][ZSpeed][SpiralLift]")
{
    NoFullComments no_comments;
    // U1-like: 500 mm/s travel, 20 mm/s Z limit, 0.4 mm z-hop, 3 degree slope.
    auto spiral_at_layer_change = [](bool arc_fitting, bool deferred) {
        GCodeWriter writer;
        setup_z_speed_writer(writer, 500., 0., 20., arc_fitting, 0.4);
        writer.set_position(Vec3d(100., 100., deferred ? 0.2 : 0.4));
        writer.set_current_position_clear(true);
        writer.lift(LiftType::SpiralLift);
        if (deferred)
            writer.defer_z_move(0.4);
        return writer.travel_to_xyz(Vec3d(150., 100., 0.4), "", true);
    };

    SECTION("emulated with G1 segments (arc fitting off)") {
        const std::string deferred  = spiral_at_layer_change(false, true);
        const std::string reference = spiral_at_layer_change(false, false);
        INFO(deferred);
        // Same path as without the layer change (the #471 geometry), only feedrates differ.
        CHECK(without_feedrates(deferred) == without_feedrates(reference));
        // Every segment, the first one climbing the 0.2 mm layer as well, stays within 20 mm/s in Z.
        CHECK(max_g1_z_velocity(deferred, Vec3d(100., 100., 0.2)) <= 20. * 1.001);
        CHECK(max_g1_z_velocity(reference, Vec3d(100., 100., 0.4)) <= 20. * 1.001);

        // The first segment alone runs slower; the second restores the spiral's feedrate, which is far
        // above the Z limit (a 3 degree spiral is mostly XY motion).
        const std::vector<SpiralLine> lines = parse_spiral_gcode(deferred);
        REQUIRE(lines.size() > 3);
        REQUIRE(lines[0].has('F'));
        REQUIRE_FALSE(lines[1].has('F'));
        REQUIRE(lines[2].has('F'));
        CHECK(lines[0].v.at('F') < lines[2].v.at('F'));
        CHECK(lines[2].v.at('F') > 20. * 60. * 5.);
        CHECK(lines[2].v.at('F') < 500. * 60.);
        // Without a layer change, one feedrate for the whole spiral, as before.
        const std::vector<SpiralLine> ref_lines = parse_spiral_gcode(reference);
        CHECK(std::count_if(ref_lines.begin(), ref_lines.end(), [](const SpiralLine &l) { return l.has('F'); }) == 2);
    }
    SECTION("one G3 arc (arc fitting on)") {
        const std::string deferred = spiral_at_layer_change(true, true);
        INFO(deferred);
        CHECK(without_feedrates(deferred) == without_feedrates(spiral_at_layer_change(true, false)));
        const std::vector<SpiralLine> lines = parse_spiral_gcode(deferred);
        const auto arc = std::find_if(lines.begin(), lines.end(), [](const SpiralLine &l) { return l.cmd == "G3"; });
        REQUIRE(arc != lines.end());
        const double dz         = arc->v.at('Z') - 0.2;
        const double arc_length = std::hypot(2. * PI * std::hypot(arc->v.at('I'), arc->v.at('J')), dz);
        CHECK(arc->v.at('F') / 60. * dz / arc_length <= 20. + 0.0001);
        CHECK(arc->v.at('F') > 20. * 60. * 5.);
        CHECK(arc->v.at('F') < 500. * 60.);
    }
}
