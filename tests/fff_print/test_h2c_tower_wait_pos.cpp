#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

// The Bambu H2C / H2D / H2S change_filament templates read three tool-change values Edge used to hard-code
// (owner report 2026-11-11: an odd detour before the prime tower and ooze on H2C left/right switches):
// - M620.14 X[wipe_tower_center_pos_x] Y[wipe_tower_center_pos_y] (H2C, also in its start G-code): Bambu Studio's
//   "point for nozzle heating", 2 mm beside the prime tower. Without a tower the template's own
//   "M620.14 X95.5 Y336" stays.
// - {if wipe_avoid_perimeter} G387 Y320 J1 / G1 X[wipe_avoid_pos_x] {endif}: with skip points (wall gaps) on, keep
//   clear of the tower wall on the way back, beside the tower.
// - M620.10 R / M983.3 R{new_extruder_retracted_length}: how far the incoming extruder's filament is retracted.

namespace {

PresetBundle &wait_pos_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    if (bundle)
        return *bundle;
    const std::string                    saved_data_dir = data_dir();
    const boost::filesystem::path        scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("h2cwait_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    auto              library  = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

struct Machine
{
    const char *printer;
    const char *process;
    const char *filament;
};

const Machine H2C{ "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C" };
const Machine H2D{ "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D" };

DynamicPrintConfig machine_config(const Machine &m, bool tower, const Vec2d &tower_pos)
{
    PresetBundle &b = wait_pos_bundle();
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament, true));
    b.filament_presets = { m.filament };
    b.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = std::vector<std::string>(2, m.filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", tower ? "1" : "0" },
        { "wipe_tower_x", tower_pos.x() },
        { "wipe_tower_y", tower_pos.y() },
        { "wipe_tower_rotation_angle", 0 },
        { "filament_map_mode", "Manual" },
        // Filament 1 on the left extruder, filament 2 on the right one: every tool change switches extruder.
        { "filament_map", "1,2" },
        { "gcode_comments", 0 },
    });
    return cfg;
}

// Two 20 x 20 x 2 mm cubes, one per filament, in the middle of the bed: every layer switches extruder twice.
std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg)
{
    for (int i = 0; i < 2; ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.1f));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("extruder", i + 1);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(150. + 30. * i, 140., 0.));
        object->ensure_on_bed();
    }
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

std::vector<std::string> lines_of(const std::string &gcode)
{
    std::vector<std::string> out;
    std::istringstream       ss(gcode);
    bool                     in_config = false;
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == "; CONFIG_BLOCK_START")
            in_config = true;
        if (!in_config)
            out.push_back(line);
        if (line == "; CONFIG_BLOCK_END")
            in_config = false;
    }
    return out;
}

// The tower's first-layer box on the plate, as the G-code prints it: the extents of the first wipe tower
// layer's outer wall and brim (Bambu Studio's WipeTower::get_bbx()) moved by the tower position. Edge keeps that
// outline as the tower mesh's bottom, in the frame the G-code prints.
BoundingBoxf tower_box(const Print &print)
{
    const auto &mesh = print.wipe_tower_data().wipe_tower_mesh_data;
    REQUIRE(mesh.has_value());
    REQUIRE_FALSE(mesh->bottom.empty());
    const BoundingBox  local = get_extents(mesh->bottom);
    const Vec2d        pos(print.config().wipe_tower_x.get_at(0), print.config().wipe_tower_y.get_at(0));
    const BoundingBoxf box(unscaled<double>(local.min) + pos, unscaled<double>(local.max) + pos);
    // Sanity: the outline covers the tower's width x depth from its position (give or take a perimeter).
    CHECK(box.min.x() <= pos.x() + 1.);
    CHECK(box.min.y() <= pos.y() + 1.);
    CHECK(box.max.x() >= pos.x() + print.wipe_tower_data().width - 1.);
    CHECK(box.max.y() >= pos.y() + print.wipe_tower_data().depth - 1.);
    return box;
}

struct Point2 { double x, y; };

// Every M620.14 in order: the first is the start G-code's (it comes before any tool change), the rest the tool
// changes'.
std::vector<Point2> m620_14(const std::vector<std::string> &lines)
{
    static const std::regex re(R"(^\s*M620\.14 X([-\d.]+) Y([-\d.]+)\s*$)");
    std::vector<Point2>     out;
    for (const std::string &line : lines) {
        std::smatch m;
        if (std::regex_match(line, m, re))
            out.push_back({ std::stod(m[1].str()), std::stod(m[2].str()) });
    }
    return out;
}

// The X of each "G1 X<x> F30000" right after the template's move to Y320 ({if wipe_avoid_perimeter}).
std::vector<double> avoid_xs(const std::vector<std::string> &lines, const std::string &y320)
{
    static const std::regex re(R"(^G1 X([-\d.]+) F30000$)");
    std::vector<double>     out;
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        std::smatch m;
        if (lines[i] == y320 && std::regex_match(lines[i + 1], m, re))
            out.push_back(std::stod(m[1].str()));
    }
    return out;
}

double expected_avoid_x(const BoundingBoxf &tower)
{
    // Bambu Studio get_wipe_avoid_pos_x(): 3 mm right of the tower if within X 100..250, else 3 mm left of it, else 110.
    const double right = tower.max.x() + 3., left = tower.min.x() - 3.;
    return (right > 100. && right < 250.) ? right : (left > 100. && left < 250.) ? left : 110.;
}

std::vector<double> r_values(const std::vector<std::string> &lines, const std::regex &re)
{
    std::vector<double> out;
    for (const std::string &line : lines) {
        std::smatch m;
        if (std::regex_match(line, m, re))
            out.push_back(std::stod(m[1].str()));
    }
    return out;
}

size_t count(const std::vector<std::string> &lines, const std::string &needle)
{
    size_t n = 0;
    for (const std::string &line : lines)
        if (line == needle)
            ++n;
    return n;
}

const std::regex re_m620_10_r(R"(^M620\.10 R([-\d.]+)$)");
const std::regex re_m983_3_r(R"(^M983\.3 F[\d.]+ A[\d.]+ R([-\d.]+)$)");

// The bed every nozzle reaches (intersection of extruder_printable_area), as a box.
BoundingBoxf shared_bed(const Print &print)
{
    const ExtruderAreas areas = print.get_extruder_areas();
    REQUIRE(areas.multi());
    const BoundingBox b = get_extents(areas.shared);
    return BoundingBoxf(unscaled<double>(b.min), unscaled<double>(b.max));
}

} // namespace

TEST_CASE("H2C: M620.14 waits 2 mm beside the prime tower, at print start and on every tool change", "[PrintGCode][WipeTower][BambuToolChange]")
{
    // Right of the bed middle (bed 350 wide): the start point sits on the tower's left (bed-middle) side.
    const DynamicPrintConfig cfg = machine_config(H2C, true, Vec2d(200., 240.));
    Print                    print;
    Model                    model;
    const std::string        gcode = slice(print, model, cfg);
    REQUIRE(print.has_wipe_tower());
    const std::vector<std::string> lines   = lines_of(gcode);
    const BoundingBoxf             tower   = tower_box(print);
    const BoundingBoxf             bed     = shared_bed(print);
    const size_t                   changes = r_values(lines, re_m620_10_r).size();
    REQUIRE(changes >= 10);
    const std::vector<Point2> all = m620_14(lines);
    REQUIRE(all.size() == changes + 1);

    SECTION("start G-code: 2 mm left of the tower, at its middle Y")
    {
        CHECK(all.front().x == Approx(tower.min.x() - 2.).margin(0.01));
        CHECK(all.front().y == Approx(tower.center().y()).margin(0.01));
        CHECK(count(lines, "    M620.14 X95.5 Y336") == 0);
    }
    SECTION("every tool change: within 3 mm of a vertical tower edge, at the tower's Y, on the shared bed")
    {
        for (size_t i = 1; i < all.size(); ++i) {
            const Point2 &p = all[i];
            INFO("M620.14 X" << p.x << " Y" << p.y << " tower X" << tower.min.x() << ".." << tower.max.x() << " Y" << tower.min.y() << ".."
                             << tower.max.y());
            const bool left_of  = p.x >= tower.min.x() - 3. && p.x <= tower.min.x() + 0.5;
            const bool right_of = p.x >= tower.max.x() - 0.5 && p.x <= tower.max.x() + 3.;
            CHECK((left_of || right_of));
            CHECK(p.y >= tower.min.y() - 0.5);
            CHECK(p.y <= tower.max.y() + 0.5);
            CHECK(p.x >= bed.min.x() - EPSILON);
            CHECK(p.x <= bed.max.x() + EPSILON);
        }
    }
    SECTION("wall gaps on: every tool change keeps clear of the tower, 3 mm beside it")
    {
        // Twice per tool change: the normal and the power-loss-resume branch.
        const std::vector<double> xs = avoid_xs(lines, "G387 Y320 J1 F10000");
        CHECK(count(lines, "G387 Y320 J1 F10000") == 2 * changes);
        CHECK(xs.size() == 2 * changes);
        for (double x : xs)
            CHECK(x == Approx(expected_avoid_x(tower)).margin(0.01));
    }
    SECTION("the incoming extruder's retraction: none on the first visit, what it was left with after that")
    {
        const std::vector<double> r620 = r_values(lines, re_m620_10_r);
        const std::vector<double> r983 = r_values(lines, re_m983_3_r);
        REQUIRE(r983.size() == 2 * r620.size());
        // Each extruder starts unretracted; the first switch onto the other extruder finds it so.
        CHECK(r620[0] == Approx(0.));
        // From the third change on both extruders have been left once, retracted by the tool change retraction.
        for (size_t i = 2; i < r620.size(); ++i) {
            INFO("tool change " << i);
            CHECK(r620[i] > 0.);
            CHECK(r983[2 * i] == Approx(r620[i]));
            CHECK(r983[2 * i + 1] == Approx(r620[i]));
        }
    }
}

TEST_CASE("H2C: a tower left of the bed middle waits on its right side", "[PrintGCode][WipeTower][BambuToolChange]")
{
    const DynamicPrintConfig cfg = machine_config(H2C, true, Vec2d(40., 240.));
    Print                    print;
    Model                    model;
    const std::string        gcode = slice(print, model, cfg);
    REQUIRE(print.has_wipe_tower());
    const BoundingBoxf        tower = tower_box(print);
    const std::vector<Point2> start = m620_14(lines_of(gcode));
    REQUIRE(start.size() > 1);
    CHECK(start[0].x == Approx(tower.max.x() + 2.).margin(0.01));
    CHECK(start[0].y == Approx(tower.center().y()).margin(0.01));
}

TEST_CASE("H2C without a prime tower keeps the template's fixed M620.14", "[PrintGCode][WipeTower][BambuToolChange]")
{
    const DynamicPrintConfig cfg = machine_config(H2C, false, Vec2d(200., 240.));
    Print                    print;
    Model                    model;
    const std::string        gcode = slice(print, model, cfg);
    REQUIRE_FALSE(print.has_wipe_tower());
    const std::vector<std::string> lines = lines_of(gcode);
    // The start G-code's and every tool change's.
    const std::vector<Point2> all = m620_14(lines);
    REQUIRE(all.size() > 1);
    CHECK(all.size() == r_values(lines, re_m620_10_r).size() + 1);
    for (const Point2 &p : all) {
        CHECK(p.x == Approx(95.5));
        CHECK(p.y == Approx(336.));
    }
    // No tower to keep clear of (Bambu Studio's set_extruder path).
    CHECK(count(lines, "G387 Y320 J1 F10000") == 0);
}

TEST_CASE("H2D: tool changes keep clear of the tower and pass the incoming extruder's retraction", "[PrintGCode][WipeTower][BambuToolChange]")
{
    const DynamicPrintConfig cfg = machine_config(H2D, true, Vec2d(200., 240.));
    Print                    print;
    Model                    model;
    const std::string        gcode = slice(print, model, cfg);
    REQUIRE(print.has_wipe_tower());
    const std::vector<std::string> lines = lines_of(gcode);
    const BoundingBoxf             tower = tower_box(print);
    const std::vector<double>      r620  = r_values(lines, re_m620_10_r);
    REQUIRE(r620.size() >= 10);
    // The H2D template has no M620.14.
    CHECK(m620_14(lines).empty());
    // The H2D template moves to Y320 with a plain G1, in the normal and the power-loss-resume branch.
    const std::vector<double> xs = avoid_xs(lines, "G1 Y320 F30000");
    CHECK(xs.size() == 2 * r620.size());
    for (double x : xs)
        CHECK(x == Approx(expected_avoid_x(tower)).margin(0.01));
    CHECK(r620[0] == Approx(0.));
    for (size_t i = 2; i < r620.size(); ++i)
        CHECK(r620[i] > 0.);
}
