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

// Farthest-point timelapse on the two-nozzle Bambu printers (owner report 2026-11-11, H2C right->left switches:
// "moves to the prime tower, then goes to a location to the right of the print, then slowly back to the tower").
// When a layer's farthest point (from the camera at the bed origin) belongs to the nozzle that is not the photo
// nozzle (the most used one), Bambu Studio takes the photo right after switching from the photo nozzle to the
// other one ("case B", append_tcr): straight after the change, BEFORE the travel to the tower's start point and
// the unretract there. Edge took it after both, so the nozzle was primed at the tower, travelled to the photo
// spot and back.

namespace {

PresetBundle &timelapse_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    if (bundle)
        return *bundle;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("h2ctl_%%%%-%%%%");
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

DynamicPrintConfig machine_config(const Machine &m)
{
    PresetBundle &b = timelapse_bundle();
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament, true));
    b.filament_presets = { m.filament };
    b.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = std::vector<std::string>(2, m.filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "wipe_tower_x", 40. },
        { "wipe_tower_y", 230. },
        { "wipe_tower_rotation_angle", 0 },
        { "timelapse_type", "0" },
        { "farthest_point_timelapse", "1" },
        { "filament_map_mode", "Manual" },
        // Filament 1 (the minority) on the left extruder, filament 2 on the right one.
        { "filament_map", "1,2" },
        { "gcode_comments", 0 },
    });
    return cfg;
}

void add_cube(Model &model, int filament, const Vec3d &offset, double height)
{
    TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
    cube.scale(Vec3f(1.f, 1.f, float(height / 20.)));
    ModelObject *object = model.add_object();
    object->name        = "cube" + std::to_string(filament);
    object->add_volume(cube);
    object->volumes.front()->name = object->name;
    object->config.set("extruder", filament);
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(offset);
    object->ensure_on_bed();
}

// The minority (left) filament prints a low cube far from the camera (bed origin), the right filament a taller
// cube near it: the right extruder prints on more layers (photo nozzle), and on the shared layers the farthest
// point belongs to the left one, so every right->left switch there is a case B photo.
std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg)
{
    add_cube(model, 1, Vec3d(230., 200., 0.), 2.);
    add_cube(model, 2, Vec3d(150., 100., 0.), 4.);
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

struct Scan
{
    size_t                   blocks = 0;       // timelapse blocks after the start G-code
    size_t                   after_change = 0; // taken between a tool change and the first extrusion of the new tool
    size_t                   after_unretract = 0;
    std::map<int, int>       per_layer;
    std::vector<std::string> m9711_after_change;
};

// Walks the body. A tool change window opens at the end of a change_filament_gcode ("; custom gcode end") and
// closes at the next extrusion move; a timelapse block in it is a case B photo, and it must come before the
// unretract ("G1 E<positive>") that primes the nozzle at the tower.
Scan scan(const std::vector<std::string> &lines)
{
    static const std::regex re_extrude(R"(^G[123] [^;]*[XY][-\d.]+[^;]*E\d*\.?\d+)");
    static const std::regex re_unretract(R"(^G1 E\d*\.?\d+(\s|$))");
    Scan                    s;
    int                     layer = 0;
    bool                    started = false, window = false, unretracted = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &l = lines[i];
        if (l.rfind("; CHANGE_LAYER", 0) == 0) {
            ++layer;
            started = true;
        }
        if (l == "; custom gcode end") {
            window      = true;
            unretracted = false;
            continue;
        }
        if (window && std::regex_search(l, re_extrude))
            window = false;
        if (window && std::regex_search(l, re_unretract))
            unretracted = true;
        if (started && l == "; SKIPTYPE: timelapse") {
            ++s.blocks;
            ++s.per_layer[layer];
            if (window) {
                ++s.after_change;
                if (unretracted)
                    ++s.after_unretract;
                for (size_t j = i; j < std::min(lines.size(), i + 40); ++j)
                    if (lines[j].find("M9711") != std::string::npos) {
                        s.m9711_after_change.push_back(lines[j]);
                        break;
                    }
            }
        }
    }
    return s;
}

void check_case_b(const Machine &m)
{
    const DynamicPrintConfig cfg = machine_config(m);
    Print                    print;
    Model                    model;
    const std::string        gcode = slice(print, model, cfg);
    REQUIRE(print.has_wipe_tower());
    const Scan s = scan(lines_of(gcode));
    INFO("timelapse blocks " << s.blocks << ", after a change " << s.after_change << ", after the unretract " << s.after_unretract);
    // The case is exercised: photos right after a switch to the left (non-photo) nozzle.
    REQUIRE(s.after_change > 0);
    // None of them after the nozzle was primed at the tower.
    CHECK(s.after_unretract == 0);
    // One photo per layer.
    for (const auto &[layer, n] : s.per_layer) {
        INFO("layer " << layer);
        CHECK(n == 1);
    }
    // The photo nozzle is the right one (logical 1); physical_extruder_map [1,0] makes it physical 0: E0.
    for (const std::string &line : s.m9711_after_change) {
        INFO(line);
        CHECK(line.find("M9711 M0 E0 ") != std::string::npos);
    }
}

} // namespace

TEST_CASE("H2C farthest-point timelapse: the case B photo comes before the tower travel and unretract", "[PrintGCode][WipeTower][Timelapse]")
{
    check_case_b(H2C);
}

TEST_CASE("H2D farthest-point timelapse: the case B photo comes before the tower travel and unretract", "[PrintGCode][WipeTower][Timelapse]")
{
    check_case_b(H2D);
}
