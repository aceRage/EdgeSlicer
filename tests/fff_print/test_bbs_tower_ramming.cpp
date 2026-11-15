#include <catch2/catch.hpp>

#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Bambu Studio 2.9.1's nozzle-change ramming on the Type1 prime tower (tests/design_bbs_tower_port.md): on a change
// onto another nozzle the outgoing filament rams filament_change_length(_nc) on the tower before the swap
// (WipeTower::ramming), between "; NOZZLE_CHANGE_START" and "; NOZZLE_CHANGE_END". A handle_hotend_as_extruder
// printer (H3C) with no ramming length turns the heater off or cools on an idle travel instead.

namespace {

PresetBundle &ramming_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    if (bundle)
        return *bundle;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bbsram_%%%%-%%%%");
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
    const char *filament_1;
    const char *filament_2;
};

const Machine H2D{ "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D", "Bambu PLA Basic @BBL H2D" };
const Machine H2C{ "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C", "Bambu PLA Basic @BBL H2C" };
const Machine H3C{ "Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C", "Bambu PETG Basic @BBL H3C" };
const Machine X1C{ "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C", "Bambu PLA Basic @BBL X1C" };

DynamicPrintConfig machine_config(const Machine &m, const char *filament_map = "1,2")
{
    PresetBundle &b = ramming_bundle();
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament_1, true));
    b.filament_presets = { m.filament_1 };
    b.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = { m.filament_1, m.filament_2 };
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        // Inside every bed of these printers (the X1C's is 256 x 256).
        { "wipe_tower_x", 160. },
        { "wipe_tower_y", 180. },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments", 0 },
    });
    if (cfg.option("filament_map") != nullptr && filament_map != nullptr)
        cfg.set_deserialize_strict({ { "filament_map_mode", "Manual" }, { "filament_map", filament_map } });
    return cfg;
}

// Two 20 x 20 x 2 mm cubes, one per filament: every layer changes filament twice.
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

// One NOZZLE_CHANGE_START .. END window: its lines, the filament extruded in it and the line widths it announces.
struct Window
{
    std::vector<std::string> lines;
    double                   extruded = 0.;
    std::vector<double>      widths;
    size_t                   start_line = 0;
};

std::vector<Window> nozzle_change_windows(const std::vector<std::string> &lines)
{
    static const std::regex e_re(R"(^G1 .*E([-\d.]+))");
    static const std::regex w_re(R"(^; LINE_WIDTH: ([\d.]+))");
    std::vector<Window>     out;
    Window                 *open = nullptr;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (line.rfind("; NOZZLE_CHANGE_START", 0) == 0) {
            out.emplace_back();
            open             = &out.back();
            open->start_line = i;
            // The ramming writer announces its width just before the START marker, as Bambu Studio's does.
            for (size_t j = i; j > 0 && j + 4 > i; --j) {
                std::smatch m;
                if (std::regex_search(lines[j - 1], m, w_re))
                    open->widths.push_back(std::stod(m[1].str()));
            }
            continue;
        }
        if (open == nullptr)
            continue;
        if (line.rfind("; NOZZLE_CHANGE_END", 0) == 0) {
            open = nullptr;
            continue;
        }
        open->lines.push_back(line);
        std::smatch m;
        if (std::regex_search(line, m, e_re)) {
            const double e = std::stod(m[1].str());
            if (e > 0.)
                open->extruded += e;
        }
        if (std::regex_search(line, m, w_re))
            open->widths.push_back(std::stod(m[1].str()));
    }
    return out;
}

bool contains(const std::vector<std::string> &lines, const std::string &needle)
{
    return std::find(lines.begin(), lines.end(), needle) != lines.end();
}

// Index of the first line starting with prefix at or after from, or npos.
size_t find_from(const std::vector<std::string> &lines, const std::string &prefix, size_t from)
{
    for (size_t i = from; i < lines.size(); ++i)
        if (lines[i].rfind(prefix, 0) == 0)
            return i;
    return std::string::npos;
}

// A Type1 tower planned directly: layer 0 (the first) and layer 1, each changing from filament 0 to 1 with the
// nozzle change plan given.
struct DirectTower
{
    PrintConfig                                           config;
    std::unique_ptr<WipeTower>                            tower;
    std::vector<std::vector<WipeTower::ToolChangeResult>> result;
};

std::unique_ptr<DirectTower> direct_tower(const DynamicPrintConfig &cfg, const WipeTower::NozzleChangePlan &plan, int layers = 2)
{
    auto t = std::make_unique<DirectTower>();
    t->config.apply(cfg, true);
    t->tower = std::make_unique<WipeTower>(t->config, 0, Vec3d::Zero(), 15.f, 0, 0.2f * layers);
    t->tower->set_extruder(0, t->config);
    t->tower->set_extruder(1, t->config);
    for (int layer = 0; layer < layers; ++layer) {
        const float z = 0.2f * (layer + 1);
        t->tower->plan_toolchange(z, 0.2f, layer % 2, layer % 2);
        // 600 mm3: deeper than the tower's 20 mm stability minimum, which would hide the ramming depth.
        t->tower->plan_toolchange(z, 0.2f, layer % 2, 1 - layer % 2, 600.f, 0.f, plan);
    }
    t->tower->generate(t->result);
    return t;
}

const WipeTower::ToolChangeResult &change_on_layer(const DirectTower &t, size_t layer)
{
    REQUIRE(t.result.size() > layer);
    for (const WipeTower::ToolChangeResult &tcr : t.result[layer])
        if (tcr.initial_tool != tcr.new_tool)
            return tcr;
    FAIL("no tool change on layer " << layer);
    return t.result[layer].front();
}

std::vector<std::string> split_lines(const std::string &gcode)
{
    std::vector<std::string> out;
    std::istringstream       ss(gcode);
    for (std::string line; std::getline(ss, line);)
        out.push_back(line);
    return out;
}

} // namespace

TEST_CASE("The ramming length is filament_change_length over a ramming line's extrusion, in whole lines", "[BBSTowerRamming]")
{
    DynamicPrintConfig cfg = machine_config(H2D);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_change_length", "4,4" }, { "filament_change_length_nc", "10,10" } });
    PrintConfig config;
    config.apply(cfg, true);
    WipeTower tower(config, 0, Vec3d::Zero(), 15.f, 0, 1.f);
    tower.set_extruder(0, config);
    tower.set_extruder(1, config);

    // 0.4 nozzle: Bambu Studio's ramming line is 1.0 mm wide.
    CHECK_THAT(tower.nozzle_change_width(0), WithinAbs(1.0, 1e-6));
    // Bambu Studio set_toolchange(): mm of filament per mm of line = h * (w - h * (1 - pi/4)) / filament area.
    const double area   = M_PI * 1.75 * 1.75 / 4.;
    const double e_flow = 0.2 * (1.0 - 0.2 * (1. - M_PI / 4.)) / area;
    CHECK_THAT(tower.nozzle_change_length(0, true, 0.2f), WithinRel(4. / e_flow, 1e-4));
    CHECK_THAT(tower.nozzle_change_length(0, false, 0.2f), WithinRel(10. / e_flow, 1e-4));
    // Whole lines across the tower between the outgoing filament's purge lines: 35 - (1.0 + 0.5) per line.
    const float length = tower.nozzle_change_length(0, true, 0.2f);
    const float line   = 35.f - (1.0f + tower.tool_line_width(0));
    CHECK_THAT(tower.nozzle_change_depth(0, length), WithinAbs(std::ceil(length / line) * 1.0, 1e-5));
    CHECK(tower.nozzle_change_depth(0, 0.f) == 0.f);
}

TEST_CASE("A nozzle change makes the tower deeper by its ramming lines", "[BBSTowerRamming]")
{
    DynamicPrintConfig cfg = machine_config(H2D);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_change_length", "4,4" } });
    WipeTower::NozzleChangePlan ram;
    ram.need_ramming    = true;
    ram.extruder_change = true;
    ram.old_nozzle      = 0;
    ram.new_nozzle      = 1;
    ram.old_extruder    = 0;
    const auto plain  = direct_tower(cfg, WipeTower::NozzleChangePlan());
    const auto rammed = direct_tower(cfg, ram);
    // Whole ramming lines (nozzle_change_depth() at the 1.0 mm ramming width), laid in the block at the ramming
    // gap: the line width plus prime_tower_infill_gap's extra (150 %: 0.25 mm on 0.5 mm lines), Orca #10780
    // calc_block_infill_gap().
    const float lines = rammed->tower->nozzle_change_depth(0, rammed->tower->nozzle_change_length(0, true, 0.2f)) / 1.0f;
    REQUIRE(lines > 0.f);
    CHECK_THAT(rammed->tower->get_depth(), WithinAbs(plain->tower->get_depth() + lines * 1.25f, 0.01));
    // Without a plan the tower writes no nozzle change at all.
    CHECK_FALSE(change_on_layer(*plain, 1).nozzle_change_result.has_action());
}

TEST_CASE("An extruder change rams the outgoing filament and wipes back along the last line", "[BBSTowerRamming]")
{
    DynamicPrintConfig cfg = machine_config(H2D);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_change_length", "4,4" }, { "filament_ramming_travel_time", "0,0" } });
    WipeTower::NozzleChangePlan ram;
    ram.need_ramming          = true;
    ram.extruder_change       = true;
    ram.old_nozzle            = 0;
    ram.new_nozzle            = 1;
    ram.old_extruder          = 0;
    ram.old_physical_extruder = 1;
    const auto t = direct_tower(cfg, ram);
    const WipeTower::NozzleChangeResult &nc = change_on_layer(*t, 1).nozzle_change_result;
    REQUIRE(nc.has_action());
    CHECK(nc.is_extruder_change);
    CHECK_FALSE(nc.heater_off_only);
    CHECK_FALSE(nc.zero_flush_idle_cooling);
    const std::vector<std::string> lines = split_lines(nc.gcode);
    // Markers name the filaments and nozzles; an extruder change has no M632 / M633 interlock.
    CHECK(contains(lines, "; NOZZLE_CHANGE_START OF1 NF0 ON0 NN1"));
    CHECK(contains(lines, "; NOZZLE_CHANGE_END OF1 NF0 ON0 NN1"));
    CHECK(nc.gcode.find("M632") == std::string::npos);
    CHECK(nc.gcode.find("M633") == std::string::npos);
    // The ramming lines are 1.0 mm wide and extrude at least filament_change_length, less than one line more.
    double extruded = 0.;
    static const std::regex e_re(R"(^G1 .*E([-\d.]+))");
    for (const std::string &line : lines) {
        std::smatch m;
        if (std::regex_search(line, m, e_re))
            extruded += std::stod(m[1].str());
    }
    CHECK(nc.gcode.find("; LINE_WIDTH: 1.000000") != std::string::npos);
    CHECK(extruded >= 4. - 0.01);
    const double area   = M_PI * 1.75 * 1.75 / 4.;
    const double e_line = (35. - 1.5) * 0.2 * (1.0 - 0.2 * (1. - M_PI / 4.)) / area;
    CHECK(extruded < 4. + e_line + 0.5);
    // No reverse travel: the wipe goes back along the last ramming line, to the tower side.
    REQUIRE(nc.wipe_path.size() == 2);
    CHECK(std::abs(nc.wipe_path[1].y() - nc.wipe_path[0].y()) < 1e-3f);
}

TEST_CASE("A hotend change on one extruder rams inside the M632 / M633 interlock with pre-cooling and a reverse travel", "[BBSTowerRamming]")
{
    DynamicPrintConfig cfg = machine_config(H2C, "2,2");
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" },
                                 { "enable_pre_heating", "1" },
                                 { "filament_change_length_nc", "4,4" },
                                 { "filament_pre_cooling_temperature_nc", "180,180" },
                                 { "filament_ramming_travel_time_nc", "1,1" } });
    WipeTower::NozzleChangePlan ram;
    ram.need_ramming          = true;
    ram.extruder_change       = false;
    ram.old_nozzle            = 1;
    ram.new_nozzle            = 2;
    ram.old_extruder          = 1;
    ram.old_physical_extruder = 0;
    const auto t = direct_tower(cfg, ram);
    const WipeTower::NozzleChangeResult &nc = change_on_layer(*t, 1).nozzle_change_result;
    REQUIRE(nc.has_action());
    CHECK_FALSE(nc.is_extruder_change);
    const std::vector<std::string> lines = split_lines(nc.gcode);
    // Bambu Studio ramming(): START, M632, pre-cool M104 (N0 C) + fan, M633, the ramming lines, M632 again, the
    // reverse travel, M633, END.
    const size_t start = find_from(lines, "; NOZZLE_CHANGE_START OF1 NF0 ON1 NN2", 0);
    REQUIRE(start != std::string::npos);
    const size_t m632  = find_from(lines, "M632 S0 M N", start);
    const size_t m104  = find_from(lines, "M104 T0 S180 N0 C ;Wipe tower nozzle change pre cooling", start);
    const size_t m106  = find_from(lines, "M106 S255", start);
    const size_t m633  = find_from(lines, "M633", start);
    REQUIRE(m632 != std::string::npos);
    REQUIRE(m104 != std::string::npos);
    REQUIRE(m106 != std::string::npos);
    REQUIRE(m633 != std::string::npos);
    CHECK(lines[m104 - 1] == "M400");
    CHECK(m632 < m104);
    CHECK(m104 < m106);
    CHECK(m106 < m633);
    static const std::regex extrude_re(R"(^G1 .*E[\d.]+)");
    size_t first_ram = std::string::npos;
    for (size_t i = m633; i < lines.size(); ++i)
        if (std::regex_search(lines[i], extrude_re)) {
            first_ram = i;
            break;
        }
    REQUIRE(first_ram != std::string::npos);
    const size_t m632_again = find_from(lines, "M632 S0 M N", first_ram);
    REQUIRE(m632_again != std::string::npos);
    const size_t m633_again = find_from(lines, "M633", m632_again);
    const size_t end        = find_from(lines, "; NOZZLE_CHANGE_END OF1 NF0 ON1 NN2", m632_again);
    REQUIRE(m633_again != std::string::npos);
    REQUIRE(end != std::string::npos);
    CHECK(m633_again < end);
    // The reverse travel: moves without extrusion between the second M632 and M633.
    size_t travels = 0;
    for (size_t i = m632_again + 1; i < m633_again; ++i)
        if (lines[i].rfind("G1 ", 0) == 0 && lines[i].find('E') == std::string::npos)
            ++travels;
    CHECK(travels >= 2);
    // A reverse travel takes the place of the wipe.
    CHECK(nc.wipe_path.empty());
}

TEST_CASE("An extruder change with a pre-cool target stretches the ramming over the cooling time", "[BBSTowerRamming]")
{
    DynamicPrintConfig cfg = machine_config(H2D);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_change_length", "4,4" }, { "enable_pre_heating", "1" },
                                 { "filament_pre_cooling_temperature", "0,0" } });
    WipeTower::NozzleChangePlan ram;
    ram.need_ramming    = true;
    ram.extruder_change = true;
    ram.old_nozzle      = 0;
    ram.new_nozzle      = 1;
    ram.old_extruder    = 0;
    auto feedrates = [](const std::string &gcode) {
        std::vector<int>        out;
        static const std::regex f_re(R"(^G1 .*E[\d.]+.* F(\d+))");
        for (const std::string &line : split_lines(gcode)) {
            std::smatch m;
            if (std::regex_search(line, m, f_re))
                out.push_back(std::stoi(m[1].str()));
        }
        return out;
    };
    const auto fast = direct_tower(cfg, ram);
    const std::vector<int> fast_f = feedrates(change_on_layer(*fast, 1).nozzle_change_result.gcode);
    REQUIRE_FALSE(fast_f.empty());

    // 40 degrees to lose at hotend_cooling_rate: the ramming takes that long, so it runs slower.
    PrintConfig probe;
    probe.apply(cfg, true);
    const int temp = probe.nozzle_temperature.get_at(1);
    cfg.set_deserialize_strict({ { "filament_pre_cooling_temperature", std::to_string(temp - 40) + "," + std::to_string(temp - 40) } });
    const auto slow = direct_tower(cfg, ram);
    const std::vector<int> slow_f = feedrates(change_on_layer(*slow, 1).nozzle_change_result.gcode);
    REQUIRE_FALSE(slow_f.empty());
    CHECK(slow_f.front() < fast_f.front());
}

TEST_CASE("H3C: a filament without a ramming length turns its hotend off, or cools on an idle travel", "[BBSTowerRamming]")
{
    DynamicPrintConfig cfg = machine_config(H3C, nullptr);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_change_length", "0,0" }, { "filament_idle_travel_cooling_enabled", "0,0" },
                                 { "filament_pre_cooling_temperature", "0,0" } });
    WipeTower::NozzleChangePlan change;
    change.need_ramming    = true;
    change.extruder_change = true; // every H3C hotend counts as an extruder
    change.old_nozzle      = 0;
    change.new_nozzle      = 3;
    change.old_extruder    = 0;
    {
        const auto t = direct_tower(cfg, change);
        // First layer: Bambu Studio rams, with zero lines - the markers alone.
        const WipeTower::NozzleChangeResult &first = change_on_layer(*t, 0).nozzle_change_result;
        REQUIRE(first.has_action());
        CHECK_FALSE(first.heater_off_only);
        CHECK(first.gcode.find("; NOZZLE_CHANGE_START OF0 NF1 ON0 NN3") != std::string::npos);
        CHECK(first.gcode.find(" E") == std::string::npos);
        // Above it: heater off, markers only.
        const WipeTower::NozzleChangeResult &later = change_on_layer(*t, 1).nozzle_change_result;
        CHECK(later.heater_off_only);
        CHECK(later.gcode == "; NOZZLE_CHANGE_START OF1 NF0 ON0 NN3\n; NOZZLE_CHANGE_END OF1 NF0 ON0 NN3\n");
    }
    cfg.set_deserialize_strict({ { "filament_idle_travel_cooling_enabled", "1,1" } });
    {
        const auto t = direct_tower(cfg, change);
        const WipeTower::NozzleChangeResult &idle = change_on_layer(*t, 1).nozzle_change_result;
        REQUIRE(idle.zero_flush_idle_cooling);
        const std::vector<std::string> lines = split_lines(idle.gcode);
        // The heater goes off (no pre-cool target) by filament (I), the firmware is told (M632 D), the head only
        // travels, then M633.
        CHECK(contains(lines, "M104 I1 S0 N0 C ;Wipe tower idle travel heater off"));
        CHECK(contains(lines, "M632 S0 D"));
        CHECK(contains(lines, "M633"));
        CHECK(idle.gcode.find(" E") == std::string::npos);
        // The idle travel takes no tower depth.
        const auto plain = direct_tower(cfg, WipeTower::NozzleChangePlan());
        CHECK_THAT(t->tower->get_depth(), WithinAbs(plain->tower->get_depth(), 0.01));
    }
}

TEST_CASE("H3C: a filament with a ramming length rams on the tower", "[BBSTowerRamming]")
{
    DynamicPrintConfig cfg = machine_config(H3C, nullptr);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_change_length", "5,5" } });
    WipeTower::NozzleChangePlan change;
    change.need_ramming    = true;
    change.extruder_change = true;
    change.old_nozzle      = 0;
    change.new_nozzle      = 3;
    change.old_extruder    = 0;
    const auto t = direct_tower(cfg, change);
    const WipeTower::NozzleChangeResult &nc = change_on_layer(*t, 1).nozzle_change_result;
    REQUIRE(nc.has_action());
    CHECK_FALSE(nc.heater_off_only);
    CHECK_FALSE(nc.zero_flush_idle_cooling);
    CHECK(nc.gcode.find(" E") != std::string::npos);
    CHECK(nc.gcode.find("M632") == std::string::npos);
}

TEST_CASE("H2D G-code: every extruder change rams on the tower before the change template", "[BBSTowerRamming][PrintGCode]")
{
    const DynamicPrintConfig cfg = machine_config(H2D);
    Print                    print;
    Model                    model;
    const std::string        gcode = slice(print, model, cfg);
    REQUIRE(print.has_wipe_tower());
    const std::vector<std::string> lines   = lines_of(gcode);
    const std::vector<Window>      windows = nozzle_change_windows(lines);
    REQUIRE(windows.size() >= 10);
    PrintConfig config;
    config.apply(cfg, true);
    const double change_length = config.filament_change_length.get_at(0);
    REQUIRE(change_length > 0.);
    for (const Window &w : windows) {
        INFO("window at line " << w.start_line);
        // The outgoing filament's ramming, at the ramming width, before the M620 of the change template.
        CHECK(w.extruded >= change_length - 0.01);
        CHECK(std::find(w.widths.begin(), w.widths.end(), 1.0) != w.widths.end());
        CHECK_FALSE(contains(w.lines, "M632 S0 M N"));
        const size_t template_start = find_from(lines, "M620 S", w.start_line);
        const size_t end            = find_from(lines, "; NOZZLE_CHANGE_END", w.start_line);
        REQUIRE(end != std::string::npos);
        CHECK(end < template_start);
    }
}

TEST_CASE("X1C G-code: a single-nozzle printer has no nozzle change", "[BBSTowerRamming][PrintGCode]")
{
    const DynamicPrintConfig cfg = machine_config(X1C, nullptr);
    Print                    print;
    Model                    model;
    const std::string        gcode = slice(print, model, cfg);
    REQUIRE(print.has_wipe_tower());
    CHECK(gcode.find("; NOZZLE_CHANGE_START") == std::string::npos);
    CHECK(gcode.find("CP TOOLCHANGE START") != std::string::npos);
}

// Orca #10780 (Bambu Studio): the outgoing filament rams in its own category's block, at the block's next free row,
// ahead of what purges there next. (This replaces #1049's "ramming band behind the purges", review S1: the block
// gaps keep the rows of a block on one grid instead.)
TEST_CASE("The ramming rows go into the outgoing filament's block, ahead of the purge", "[BBSTowerRamming][TowerBlocks]")
{
    DynamicPrintConfig cfg = machine_config(H2D);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_change_length", "4,4" } });
    WipeTower::NozzleChangePlan ram;
    ram.need_ramming    = true;
    ram.extruder_change = true;
    ram.old_nozzle      = 0;
    ram.new_nozzle      = 1;
    ram.old_extruder    = 0;
    static const std::regex y_re(R"(^G1 .*Y([-\d.]+))");
    static const std::regex e_re(R"( E[0-9.]+)");
    auto y_range = [](const std::string &gcode, bool wipe_only) {
        float lo = 1e9f, hi = -1e9f;
        bool  in_wipe = !wipe_only;
        for (const std::string &line : split_lines(gcode)) {
            if (wipe_only && line.find("; CP TOOLCHANGE WIPE") != std::string::npos)
                in_wipe = true;
            else if (wipe_only && line.find("; CP TOOLCHANGE END") != std::string::npos)
                in_wipe = false;
            std::smatch m;
            // Extrusions only: the flat ironing at the purge start travels around it.
            if (in_wipe && std::regex_search(line, m, y_re) && std::regex_search(line, e_re)) {
                lo = std::min(lo, std::stof(m[1].str()));
                hi = std::max(hi, std::stof(m[1].str()));
            }
        }
        return std::make_pair(lo, hi);
    };

    SECTION("one category: ramming, then the purge, in the same block")
    {
        const auto rammed = direct_tower(cfg, ram);
        REQUIRE(rammed->tower->blocks().size() == 1);
        for (size_t layer = 0; layer < 2; ++layer) {
            INFO("layer " << layer);
            const WipeTower::ToolChangeResult &tc = change_on_layer(*rammed, layer);
            REQUIRE(tc.nozzle_change_result.has_action());
            const auto ramming = y_range(tc.nozzle_change_result.gcode, false);
            const auto purge   = y_range(tc.gcode, true);
            REQUIRE(purge.first < 1e9f);
            // The block starts one 0.5 mm line in; the 1.0 mm ramming lines are centred half a line further.
            CHECK_THAT(tc.nozzle_change_result.start_pos.y(), WithinAbs(0.5 + 0.25, 1e-3));
            CHECK(purge.first > ramming.second);
        }
    }
    SECTION("two categories: each in its own block")
    {
        cfg.set_deserialize_strict({ { "filament_adhesiveness_category", "100,300" } });
        const auto t = direct_tower(cfg, ram);
        REQUIRE(t->tower->blocks().size() == 2);
        auto band_of = [&t](int category) {
            for (const WipeTower::WipeTowerBlock &b : t->tower->blocks())
                if (b.filament_adhesiveness_category == category)
                    return std::make_pair(b.start_depth, b.start_depth + b.depth);
            FAIL("no block of category " << category);
            return std::make_pair(0.f, 0.f);
        };
        for (size_t layer = 0; layer < 2; ++layer) {
            INFO("layer " << layer);
            const WipeTower::ToolChangeResult &tc       = change_on_layer(*t, layer);
            const int                          old_cat  = tc.initial_tool == 0 ? 100 : 300;
            const int                          new_cat  = tc.new_tool == 0 ? 100 : 300;
            const auto                         ramming  = y_range(tc.nozzle_change_result.gcode, false);
            const auto                         purge    = y_range(tc.gcode, true);
            const auto                         old_band = band_of(old_cat);
            const auto                         new_band = band_of(new_cat);
            CHECK(ramming.first >= old_band.first - 1e-3f);
            CHECK(ramming.second <= old_band.second + 1e-3f);
            CHECK(purge.first >= new_band.first - 1e-3f);
            CHECK(purge.second <= new_band.second + 1e-3f);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Orca #10780 / #15035: the material-category block planner (filament_adhesiveness_category).
// ---------------------------------------------------------------------------------------------------------------------

namespace {

// An X1C tower planned directly: every layer changes 0 -> 1 -> 0. `start_tool[layer]` >= 0 makes that layer start
// with the filament Local-Z passes left loaded (#937) and plan no tower change of its own.
std::unique_ptr<DirectTower> block_tower(const DynamicPrintConfig &cfg, int layers, const std::vector<int> &start_tool = {})
{
    auto t = std::make_unique<DirectTower>();
    t->config.apply(cfg, true);
    t->tower = std::make_unique<WipeTower>(t->config, 0, Vec3d::Zero(), 15.f, 0, 0.2f * layers);
    t->tower->set_extruder(0, t->config);
    t->tower->set_extruder(1, t->config);
    unsigned int loaded = 0;
    for (int layer = 0; layer < layers; ++layer) {
        const float z = 0.2f * (layer + 1);
        t->tower->plan_toolchange(z, 0.2f, loaded, loaded);
        if (layer < int(start_tool.size()) && start_tool[layer] >= 0) {
            t->tower->set_last_layer_start_tool(loaded, unsigned(start_tool[layer]));
            loaded = unsigned(start_tool[layer]);
            continue;
        }
        t->tower->plan_toolchange(z, 0.2f, loaded, 1 - loaded, 300.f, 0.f);
        t->tower->plan_toolchange(z, 0.2f, 1 - loaded, loaded, 300.f, 0.f);
    }
    t->tower->generate(t->result);
    return t;
}

std::pair<float, float> band_of(const WipeTower &tower, int category)
{
    for (const WipeTower::WipeTowerBlock &b : tower.blocks())
        if (b.filament_adhesiveness_category == category)
            return { b.start_depth, b.start_depth + b.depth };
    FAIL("no block of category " << category);
    return { 0.f, 0.f };
}

// The extrusions inside the tower's wall, as (tool, y low, y high).
std::vector<std::tuple<unsigned, float, float>> inner_extrusions(const WipeTower &tower, const std::vector<WipeTower::ToolChangeResult> &layer)
{
    const float w = tower.width(), d = tower.get_depth(), m = 0.3f;
    std::vector<std::tuple<unsigned, float, float>> out;
    for (const WipeTower::ToolChangeResult &tcr : layer)
        for (size_t i = 1; i < tcr.extrusions.size(); ++i) {
            const WipeTower::Extrusion &e = tcr.extrusions[i];
            if (e.width <= 0.f)
                continue;
            const Vec2f a = tcr.extrusions[i - 1].pos, b = e.pos;
            auto on = [m](float u, float v, float line) { return std::abs(u - line) < m && std::abs(v - line) < m; };
            const bool outside = std::min(a.x(), b.x()) < -m || std::max(a.x(), b.x()) > w + m || std::min(a.y(), b.y()) < -m ||
                                 std::max(a.y(), b.y()) > d + m;
            if (outside || on(a.x(), b.x(), 0.f) || on(a.x(), b.x(), w) || on(a.y(), b.y(), 0.f) || on(a.y(), b.y(), d))
                continue; // the wall and the brim
            out.emplace_back(e.tool, std::min(a.y(), b.y()), std::max(a.y(), b.y()));
        }
    return out;
}

} // namespace

TEST_CASE("Filaments of different adhesiveness categories purge into separate blocks", "[TowerBlocks]")
{
    DynamicPrintConfig cfg = machine_config(X1C, nullptr);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_adhesiveness_category", "100,300" } });
    const auto t = block_tower(cfg, 4);
    REQUIRE(t->tower->blocks().size() == 2);
    const auto pla  = band_of(*t->tower, 100);
    const auto petg = band_of(*t->tower, 300);
    CHECK((pla.second <= petg.first + 1e-3f || petg.second <= pla.first + 1e-3f));
    // Every purge and every block fill stays in the band of its own filament's category.
    const float pw = 0.5f;
    for (size_t layer = 0; layer < t->result.size(); ++layer)
        for (const auto &[tool, lo, hi] : inner_extrusions(*t->tower, t->result[layer])) {
            INFO("layer " << layer << " tool " << tool << " y " << lo << " .. " << hi);
            const auto band = tool == 0 ? pla : petg;
            CHECK(lo >= band.first - pw);
            CHECK(hi <= band.second + pw);
        }
}

TEST_CASE("Filaments of one adhesiveness category share one block", "[TowerBlocks]")
{
    DynamicPrintConfig cfg = machine_config(X1C, nullptr);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_adhesiveness_category", "100,100" } });
    const auto t = block_tower(cfg, 4);
    REQUIRE(t->tower->blocks().size() == 1);
    CHECK(t->tower->blocks().front().filament_adhesiveness_category == 100);
    CHECK_THAT(t->tower->blocks().front().start_depth, WithinAbs(0.5, 1e-5));
}

TEST_CASE("The block tower stays stacked: no layer prints beyond the layer below it", "[TowerBlocks]")
{
    DynamicPrintConfig cfg = machine_config(X1C, nullptr);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_adhesiveness_category", "100,300" } });
    // Less to purge towards the top: the upper layers are shallower than the first.
    auto t = std::make_unique<DirectTower>();
    t->config.apply(cfg, true);
    t->tower = std::make_unique<WipeTower>(t->config, 0, Vec3d::Zero(), 15.f, 0, 1.2f);
    t->tower->set_extruder(0, t->config);
    t->tower->set_extruder(1, t->config);
    for (int layer = 0; layer < 6; ++layer) {
        const float z = 0.2f * (layer + 1);
        const float v = layer < 3 ? 400.f : 150.f;
        t->tower->plan_toolchange(z, 0.2f, 0, 0);
        t->tower->plan_toolchange(z, 0.2f, 0, 1, v, 0.f);
        t->tower->plan_toolchange(z, 0.2f, 1, 0, v, 0.f);
    }
    t->tower->generate(t->result);
    for (const WipeTower::WipeTowerBlock &b : t->tower->blocks())
        for (size_t l = 1; l < b.layer_depths.size(); ++l)
            CHECK(b.layer_depths[l] <= b.layer_depths[l - 1] + 1e-4f);
    // Per category, the highest row of a layer is supported by the layer below.
    auto top_of = [&t](size_t layer, unsigned tool) {
        float top = -1.f;
        for (const auto &[x_tool, lo, hi] : inner_extrusions(*t->tower, t->result[layer]))
            if (x_tool == tool)
                top = std::max(top, hi);
        return top;
    };
    for (size_t layer = 1; layer < t->result.size(); ++layer)
        for (unsigned tool : { 0u, 1u }) {
            INFO("layer " << layer << " tool " << tool);
            CHECK(top_of(layer, tool) <= top_of(layer - 1, tool) + 1e-3f);
        }
}

TEST_CASE("#937: a layer its Local-Z passes start with another filament prints the tower with that one", "[TowerBlocks]")
{
    DynamicPrintConfig cfg = machine_config(X1C, nullptr);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_adhesiveness_category", "100,300" } });
    // Layer 1: the passes leave filament 1 loaded and the layer plans no tower change; layer 2 changes from it.
    const auto t = block_tower(cfg, 3, { -1, 1, -1 });
    REQUIRE(t->result.size() == 3);
    for (const WipeTower::ToolChangeResult &tcr : t->result[1]) {
        CHECK(tcr.initial_tool == 1);
        CHECK(tcr.new_tool == 1);
    }
    REQUIRE_FALSE(t->result[2].empty());
    bool found_change = false;
    for (const WipeTower::ToolChangeResult &tcr : t->result[2])
        if (tcr.is_tool_change) {
            CHECK(tcr.initial_tool == 1);
            found_change = true;
            break;
        }
    CHECK(found_change);
}

TEST_CASE("X1C G-code: a PLA and PETG plate slices through the block planner", "[TowerBlocks][PrintGCode]")
{
    Machine pla_petg = X1C;
    pla_petg.filament_2 = "Bambu PETG Basic @BBL X1C";
    const DynamicPrintConfig cfg = machine_config(pla_petg, nullptr);
    REQUIRE(cfg.option<ConfigOptionInts>("filament_adhesiveness_category")->get_at(0) == 100);
    REQUIRE(cfg.option<ConfigOptionInts>("filament_adhesiveness_category")->get_at(1) == 300);
    Print             print;
    Model             model;
    const std::string gcode = slice(print, model, cfg);
    REQUIRE(print.has_wipe_tower());
    // One tower visit per change, each carrying its change template.
    size_t changes = 0, templates = 0;
    for (const std::string &line : lines_of(gcode)) {
        if (line == "; CP TOOLCHANGE START")
            ++changes;
        if (line.rfind("M620 S", 0) == 0)
            ++templates;
    }
    CHECK(changes >= 10);
    // The start and end G-code have their own too.
    CHECK(templates >= changes);
}

// Review S4 (#1052): the H2C rack hotend change no gate plate reaches (the grouping puts two filaments of one
// extruder on one nozzle): the prime volume rule, and the ramming inside the M632 / M633 interlock of a change planned
// with it.
TEST_CASE("H2C rack hotend change: filament_prime_volume_nc primes it, the ramming sits inside M632 / M633", "[TowerBlocks][BBSTowerRamming]")
{
    DynamicPrintConfig cfg = machine_config(H2C, "2,2");
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" },
                                 { "filament_prime_volume", "45,300" },
                                 { "filament_prime_volume_nc", "60,600" },
                                 { "filament_change_length_nc", "4,4" },
                                 { "prime_volume_mode", "Default" } });
    PrintConfig config;
    config.apply(cfg, true);
    CHECK_THAT(WipeTower::bambu_prime_volume(config, 1, false), WithinAbs(300., 1e-6));
    CHECK_THAT(WipeTower::bambu_prime_volume(config, 1, true), WithinAbs(600., 1e-6));
    {
        PrintConfig saving = config;
        saving.prime_volume_mode.value = PrimeVolumeMode::pvmSaving;
        CHECK_THAT(WipeTower::bambu_prime_volume(saving, 1, true), WithinAbs(15., 1e-6));
    }

    WipeTower::NozzleChangePlan hotend;
    hotend.need_ramming          = true;
    hotend.extruder_change       = false;
    hotend.old_nozzle            = 1;
    hotend.new_nozzle            = 2;
    hotend.old_extruder          = 1;
    hotend.old_physical_extruder = 0;
    // Two layers 0 -> 1, each primed with what Print hands the tower for a hotend change or for an extruder change.
    auto tower_with = [&config, &hotend](bool use_nc) {
        auto t = std::make_unique<DirectTower>();
        t->config = config;
        t->tower  = std::make_unique<WipeTower>(t->config, 0, Vec3d::Zero(), 15.f, 0, 0.4f);
        t->tower->set_extruder(0, t->config);
        t->tower->set_extruder(1, t->config);
        for (int layer = 0; layer < 2; ++layer) {
            const float z = 0.2f * (layer + 1);
            t->tower->plan_toolchange(z, 0.2f, 0, 0);
            t->tower->plan_toolchange(z, 0.2f, 0, 1, WipeTower::bambu_prime_volume(t->config, 1, use_nc), 0.f, hotend);
        }
        t->tower->generate(t->result);
        return t;
    };
    const auto nc = tower_with(true);
    const auto ec = tower_with(false);
    // Twice the prime: a deeper tower.
    CHECK(nc->tower->get_depth() > ec->tower->get_depth() + 5.f);

    const WipeTower::NozzleChangeResult &ncr = change_on_layer(*nc, 1).nozzle_change_result;
    REQUIRE(ncr.has_action());
    CHECK_FALSE(ncr.is_extruder_change);
    const std::vector<std::string> lines = split_lines(ncr.gcode);
    static const std::regex        extrude_re(R"(^G1 .*E[\d.]+)");
    size_t first_m632 = std::string::npos, first_ram = std::string::npos, last_ram = 0, last_m633 = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("M632 S1", 0) == 0 && first_m632 == std::string::npos)
            first_m632 = i;
        if (std::regex_search(lines[i], extrude_re)) {
            if (first_ram == std::string::npos)
                first_ram = i;
            last_ram = i;
        }
        if (lines[i] == "M633")
            last_m633 = i;
    }
    REQUIRE(first_m632 != std::string::npos);
    REQUIRE(first_ram != std::string::npos);
    CHECK(first_m632 < first_ram);
    CHECK(last_ram < last_m633);
}

// Review S1 (#1052): a Polymaker PLA preset of a P2S sits in Bambu PLA's band.
TEST_CASE("A Polymaker PLA and a Bambu PLA share the PLA adhesiveness category", "[TowerBlocks][BBLProfiles]")
{
    PresetBundle &b = ramming_bundle();
    for (const char *name : { "Bambu PLA Basic @BBL P2S", "Panchroma PLA @BBL P2S", "Polymaker PLA @BBL H2S" }) {
        INFO(name);
        const Preset *preset = b.filaments.find_preset(name, false);
        REQUIRE(preset != nullptr);
        const auto *category = preset->config.option<ConfigOptionInts>("filament_adhesiveness_category");
        REQUIRE(category != nullptr);
        CHECK(category->get_at(0) == 100);
    }
}

// #1052 review S3: on a two-category tower each band is purged on one layer and block-filled on the next; the fill
// under the next layer's purge is solid (Edge), so the purge rows are not bridges over a sparse grid. A single-block
// tower keeps Orca's sparse fill.
namespace {

struct Segment2
{
    Vec2f a, b;
};

// The extrusions inside the tower wall of every layer (wall and brim left out, as inner_extrusions() does).
std::vector<std::vector<Segment2>> inner_segments(const DirectTower &t)
{
    const float w = t.tower->width(), d = t.tower->get_depth(), m = 0.3f;
    std::vector<std::vector<Segment2>> out;
    for (const std::vector<WipeTower::ToolChangeResult> &layer : t.result) {
        std::vector<Segment2> segs;
        for (const WipeTower::ToolChangeResult &tcr : layer)
            for (size_t i = 1; i < tcr.extrusions.size(); ++i) {
                if (tcr.extrusions[i].width <= 0.f)
                    continue;
                const Vec2f a = tcr.extrusions[i - 1].pos, b = tcr.extrusions[i].pos;
                auto on = [m](float u, float v, float line) { return std::abs(u - line) < m && std::abs(v - line) < m; };
                const bool outside = std::min(a.x(), b.x()) < -m || std::max(a.x(), b.x()) > w + m || std::min(a.y(), b.y()) < -m ||
                                     std::max(a.y(), b.y()) > d + m;
                if (!outside && !on(a.x(), b.x(), 0.f) && !on(a.x(), b.x(), w) && !on(a.y(), b.y(), 0.f) && !on(a.y(), b.y(), d))
                    segs.push_back({ a, b });
            }
        out.push_back(std::move(segs));
    }
    return out;
}

float distance_to_segment(const Vec2f &p, const Segment2 &s)
{
    const Vec2f ab = s.b - s.a;
    const float l2 = ab.squaredNorm();
    const float t  = l2 > 0.f ? std::clamp((p - s.a).dot(ab) / l2, 0.f, 1.f) : 0.f;
    return (p - (s.a + t * ab)).norm();
}

// Share of the horizontal rows of layers 2.. with no extrusion of the layer below within 0.5 mm (the review's measure),
// sampled every 0.25 mm. The tower wall counts as support (it is under every row end).
float unsupported_share(const DirectTower &t)
{
    const std::vector<std::vector<Segment2>> layers = inner_segments(t);
    const float w = t.tower->width();
    float total = 0.f, unsupported = 0.f;
    for (size_t l = 2; l < layers.size(); ++l)
        for (const Segment2 &s : layers[l]) {
            if (std::abs(s.a.y() - s.b.y()) > 1e-3f)
                continue;
            const float len = (s.b - s.a).norm();
            for (float u = 0.f; u <= len; u += 0.25f) {
                const Vec2f p = s.a + (len > 0.f ? u / len : 0.f) * (s.b - s.a);
                total += 0.25f;
                if (p.x() < 0.5f || p.x() > w - 0.5f)
                    continue;
                bool supported = false;
                for (const Segment2 &below : layers[l - 1])
                    if (distance_to_segment(p, below) <= 0.5f) {
                        supported = true;
                        break;
                    }
                if (!supported)
                    unsupported += 0.25f;
            }
        }
    return total > 0.f ? unsupported / total : 0.f;
}

// layers layers, one change per layer between filaments 0 and 1, alternating, with the given prime volumes.
std::unique_ptr<DirectTower> alternating_tower(const DynamicPrintConfig &cfg, int layers, float even_volume, float odd_volume)
{
    auto t = std::make_unique<DirectTower>();
    t->config.apply(cfg, true);
    t->tower = std::make_unique<WipeTower>(t->config, 0, Vec3d::Zero(), 15.f, 0, 0.2f * layers);
    t->tower->set_extruder(0, t->config);
    t->tower->set_extruder(1, t->config);
    for (int layer = 0; layer < layers; ++layer) {
        const float        z    = 0.2f * (layer + 1);
        const unsigned int from = unsigned(layer % 2);
        t->tower->plan_toolchange(z, 0.2f, from, from);
        t->tower->plan_toolchange(z, 0.2f, from, 1 - from, layer % 2 ? odd_volume : even_volume, 0.f);
    }
    t->tower->generate(t->result);
    return t;
}

} // namespace

TEST_CASE("Two-category tower: a band the next layer purges into is filled solid under it", "[TowerBlocks]")
{
    DynamicPrintConfig cfg = machine_config(X1C, nullptr);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_adhesiveness_category", "100,300" } });
    const auto t = alternating_tower(cfg, 8, 300.f, 300.f);
    REQUIRE(t->tower->blocks().size() == 2);
    const float share = unsupported_share(*t);
    INFO("unsupported share of the row length " << share);
    // Before: about 0.4 (rows over fill lines 8 mm apart). Left: the row ends past the solid fill's last line.
    CHECK(share < 0.15f);
}

TEST_CASE("Single-category tower: the block fill under a deeper purge stays sparse, as upstream", "[TowerBlocks]")
{
    DynamicPrintConfig cfg = machine_config(X1C, nullptr);
    cfg.set_deserialize_strict({ { "prime_tower_width", "35" }, { "filament_adhesiveness_category", "100,100" } });
    // Even layers purge little, odd layers much: the odd layers' upper rows lie over the even layers' sparse fill.
    const auto t = alternating_tower(cfg, 8, 100.f, 600.f);
    REQUIRE(t->tower->blocks().size() == 1);
    const float share = unsupported_share(*t);
    INFO("unsupported share of the row length " << share);
    CHECK(share > 0.15f);
}
