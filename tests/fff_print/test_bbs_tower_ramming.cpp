#include <catch2/catch.hpp>

#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

#include <cmath>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

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
        { "wipe_tower_x", 200. },
        { "wipe_tower_y", 240. },
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
        t->tower->plan_toolchange(z, 0.2f, layer % 2, 1 - layer % 2, 30.f, 0.f, plan);
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
    CHECK(tower.nozzle_change_width(0) == Approx(1.0f));
    // Bambu Studio set_toolchange(): mm of filament per mm of line = h * (w - h * (1 - pi/4)) / filament area.
    const double area   = M_PI * 1.75 * 1.75 / 4.;
    const double e_flow = 0.2 * (1.0 - 0.2 * (1. - M_PI / 4.)) / area;
    CHECK(tower.nozzle_change_length(0, true, 0.2f) == Approx(4. / e_flow).epsilon(1e-4));
    CHECK(tower.nozzle_change_length(0, false, 0.2f) == Approx(10. / e_flow).epsilon(1e-4));
    // Whole lines across the tower between the outgoing filament's purge lines: 35 - (1.0 + 0.5) per line.
    const float length = tower.nozzle_change_length(0, true, 0.2f);
    const float line   = 35.f - (1.0f + tower.tool_line_width(0));
    CHECK(tower.nozzle_change_depth(0, length) == Approx(std::ceil(length / line) * 1.0f));
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
    const float depth = rammed->tower->nozzle_change_depth(0, rammed->tower->nozzle_change_length(0, true, 0.2f));
    REQUIRE(depth > 0.f);
    CHECK(rammed->tower->get_depth() == Approx(plain->tower->get_depth() + depth).margin(0.01));
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
        CHECK(t->tower->get_depth() == Approx(plain->tower->get_depth()).margin(0.01));
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
