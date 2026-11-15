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
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Bambu Studio 2.9.1's H3C tower behaviour on the Type 1 block planner (PR-A of tests/survey_bbs_tower_gap_2026-11-14.md):
// the tower speed model (calc_tower_feedrate, prime_tower_feedrate_factor, filament_prime_tower_speed_factor_curve), the
// end-of-visit slowdown, the purge-start line ironing switch and speed, the side scrape after a tool change, the
// nozzle-addressed M104 / M109 forms, and the mixed nozzle diameter warning.

namespace {

PresetBundle &parity_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    if (bundle)
        return *bundle;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bbsh3c_%%%%-%%%%");
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

const Machine H3C{ "Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C", "Bambu PLA Basic @BBL H3C" };
const Machine H2C{ "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C", "Bambu PLA Basic @BBL H2C" };
const Machine X1C{ "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C", "Bambu PLA Basic @BBL X1C" };

DynamicPrintConfig machine_config(const Machine &m)
{
    PresetBundle &b = parity_bundle();
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament_1, true));
    b.filament_presets = { m.filament_1 };
    b.set_num_filaments(2, std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = { m.filament_1, m.filament_2 };
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "wipe_tower_x", 160. },
        { "wipe_tower_y", 180. },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments", 0 },
    });
    if (cfg.option("filament_map") != nullptr)
        cfg.set_deserialize_strict({ { "filament_map_mode", "Manual" }, { "filament_map", "1,2" } });
    return cfg;
}

void set_cooling_before_tower(DynamicPrintConfig &cfg, double value)
{
    auto *opt = cfg.option<ConfigOptionFloatsNullable>("filament_cooling_before_tower", true);
    if (opt->values.empty())
        opt->values.resize(2);
    std::fill(opt->values.begin(), opt->values.end(), value);
}

void set_speed_curve(DynamicPrintConfig &cfg, const std::string &curve)
{
    cfg.option<ConfigOptionStrings>("filament_prime_tower_speed_factor_curve", true)->values = { curve, curve };
}

// A Type 1 tower planned directly: `layers` layers of 0.2 mm, each changing filament (0 -> 1 on even layers, 1 -> 0 on odd
// ones) with a 600 mm3 purge, no nozzle change.
struct DirectTower
{
    PrintConfig                                           config;
    std::unique_ptr<WipeTower>                            tower;
    std::vector<std::vector<WipeTower::ToolChangeResult>> result;
};

std::unique_ptr<DirectTower> direct_tower(const DynamicPrintConfig &cfg, int layers = 2)
{
    auto t = std::make_unique<DirectTower>();
    t->config.apply(cfg, true);
    t->tower = std::make_unique<WipeTower>(t->config, 0, Vec3d::Zero(), 15.f, 0, 0.2f * layers);
    t->tower->set_extruder(0, t->config);
    t->tower->set_extruder(1, t->config);
    for (int layer = 0; layer < layers; ++layer) {
        const float z = 0.2f * (layer + 1);
        t->tower->plan_toolchange(z, 0.2f, layer % 2, layer % 2);
        t->tower->plan_toolchange(z, 0.2f, layer % 2, 1 - layer % 2, 600.f, 0.f, WipeTower::NozzleChangePlan());
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
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(line);
    }
    return out;
}

bool has_token(const std::string &line, char key, double *value = nullptr)
{
    std::istringstream ss(line);
    for (std::string tok; ss >> tok;)
        if (tok.size() > 1 && tok[0] == key) {
            if (value != nullptr)
                *value = std::stod(tok.substr(1));
            return true;
        }
    return false;
}

// The feedrates of the purge's extrusions (with X or Y, positive E, an F of their own), in order, from the
// "; CP TOOLCHANGE WIPE" of the tool change to its end.
std::vector<double> purge_feedrates(const std::string &gcode)
{
    std::vector<double> out;
    bool                in_wipe = false;
    for (const std::string &line : split_lines(gcode)) {
        if (line.rfind("; CP TOOLCHANGE WIPE", 0) == 0)
            in_wipe = true;
        else if (line.rfind("; CP TOOLCHANGE END", 0) == 0)
            in_wipe = false;
        if (!in_wipe || line.rfind("G1 ", 0) != 0)
            continue;
        double e = 0., f = 0.;
        if ((has_token(line, 'X') || has_token(line, 'Y')) && has_token(line, 'E', &e) && e > 0. && has_token(line, 'F', &f))
            out.push_back(f);
    }
    return out;
}

double total_extrusion(const std::string &gcode)
{
    double sum = 0.;
    for (const std::string &line : split_lines(gcode)) {
        double e = 0.;
        if (line.rfind("G1 ", 0) == 0 && has_token(line, 'E', &e))
            sum += e;
    }
    return sum;
}

size_t line_index(const std::vector<std::string> &lines, const std::string &prefix, size_t from = 0)
{
    for (size_t i = from; i < lines.size(); ++i)
        if (lines[i].rfind(prefix, 0) == 0)
            return i;
    return std::string::npos;
}

size_t count_matching(const std::string &gcode, const std::regex &re)
{
    size_t n = 0;
    for (const std::string &line : split_lines(gcode))
        if (std::regex_search(line, re))
            ++n;
    return n;
}

// Two 20 x 20 x 2 mm cubes, one per filament: every layer changes filament.
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

} // namespace

TEST_CASE("The purge rows follow the filament's speed factor curve", "[BBSTowerH3C]")
{
    DynamicPrintConfig cfg = machine_config(X1C);
    // The tower speed is prime_tower_max_speed here: PLA's volumetric limit is far above it.
    const double max_speed = cfg.opt_float("wipe_tower_max_purge_speed") * 60.;
    SECTION("no curve: Bambu's built-in ramp, 0.33 then 0.375 of the tower speed")
    {
        set_speed_curve(cfg, "");
        const auto                t = direct_tower(cfg);
        const std::vector<double> f = purge_feedrates(change_on_layer(*t, 1).gcode);
        REQUIRE(f.size() >= 2);
        CHECK_THAT(f[1] / f[0], WithinRel(0.375 / 0.33, 0.02));
        CHECK_THAT(f[0], WithinRel(0.33 * max_speed, 0.02));
    }
    SECTION("a curve: the first row at half the tower speed, then the full speed")
    {
        set_speed_curve(cfg, "0.5,1");
        const auto                t = direct_tower(cfg);
        const std::vector<double> f = purge_feedrates(change_on_layer(*t, 1).gcode);
        REQUIRE(f.size() >= 2);
        CHECK_THAT(f[0], WithinRel(0.5 * max_speed, 0.02));
        CHECK_THAT(f[1], WithinRel(max_speed, 0.02));
        // A short curve repeats its last value: no row after the second changes the speed again.
        CHECK(std::all_of(f.begin() + 1, f.end(), [&f](double v) { return std::abs(v - f[1]) < 1.; }));
    }
    SECTION("a curve that does not parse is the built-in ramp")
    {
        set_speed_curve(cfg, "fast");
        const auto                t = direct_tower(cfg);
        const std::vector<double> f = purge_feedrates(change_on_layer(*t, 1).gcode);
        REQUIRE(f.size() >= 2);
        CHECK_THAT(f[1] / f[0], WithinRel(0.375 / 0.33, 0.02));
    }
}

TEST_CASE("H3C: the tower speed is the volumetric speed times the feedrate factor, above the 90 mm/s cap", "[BBSTowerH3C]")
{
    DynamicPrintConfig cfg = machine_config(H3C);
    set_speed_curve(cfg, "1");
    cfg.set_deserialize_strict({ { "prime_tower_feedrate_factor", "1" } });
    const std::vector<double> full = purge_feedrates(change_on_layer(*direct_tower(cfg), 1).gcode);
    cfg.set_deserialize_strict({ { "prime_tower_feedrate_factor", "0.5" } });
    const std::vector<double> half = purge_feedrates(change_on_layer(*direct_tower(cfg), 1).gcode);
    REQUIRE(! full.empty());
    REQUIRE(! half.empty());
    const double full_max = *std::max_element(full.begin(), full.end());
    const double half_max = *std::max_element(half.begin(), half.end());
    // Bambu Studio: no prime_tower_max_speed cap on a hotend_as_extruder printer.
    CHECK(full_max > cfg.opt_float("wipe_tower_max_purge_speed") * 60. + 1.);
    CHECK(full_max < 1000. * 60.);
    CHECK_THAT(half_max / full_max, WithinRel(0.5, 0.02));

    // The X1C keeps the cap.
    DynamicPrintConfig x1c = machine_config(X1C);
    set_speed_curve(x1c, "1");
    const std::vector<double> capped = purge_feedrates(change_on_layer(*direct_tower(x1c), 1).gcode);
    REQUIRE(! capped.empty());
    CHECK(*std::max_element(capped.begin(), capped.end()) <= x1c.opt_float("wipe_tower_max_purge_speed") * 60. + 1.);
}

TEST_CASE("Tail slowdown: the last volume of a visit at the tail speed factor, the move it starts in split", "[BBSTowerH3C]")
{
    const Vec2f origin(0.f, 0.f);
    SECTION("half of the last move")
    {
        std::string gcode = "G1 F6000\nG1 X10 Y0 E1\nG1 X20 Y0 E1\n";
        wipe_tower_tail_slowdown(gcode, 0.5f, 0.5f, 1.f, &origin, nullptr);
        const std::vector<std::string> lines = split_lines(gcode);
        REQUIRE(lines.size() == 4);
        CHECK(lines[0] == "G1 F6000");
        CHECK(lines[1] == "G1 X10 Y0 E1");
        double x = 0., e = 0., f = 0.;
        REQUIRE(has_token(lines[2], 'X', &x));
        CHECK_THAT(x, WithinAbs(15., 1e-6));
        REQUIRE(has_token(lines[2], 'E', &e));
        CHECK_THAT(e, WithinAbs(0.5, 1e-6));
        CHECK_FALSE(has_token(lines[2], 'F'));
        REQUIRE(has_token(lines[3], 'F', &f));
        CHECK_THAT(f, WithinAbs(3000., 0.5));
        REQUIRE(has_token(lines[3], 'E', &e));
        CHECK_THAT(e, WithinAbs(0.5, 1e-6));
    }
    SECTION("a volume of 0 slows the whole visit")
    {
        std::string gcode = "G1 F6000\nG1 X10 Y0 E1\nG1 X20 Y0 E1\n";
        wipe_tower_tail_slowdown(gcode, 0.f, 0.5f, 1.f, &origin, nullptr);
        const std::vector<std::string> lines = split_lines(gcode);
        REQUIRE(lines.size() == 3);
        CHECK(lines[1] == "G1 X10 Y0 E1 F3000");
        CHECK(lines[2] == "G1 X20 Y0 E1 F3000");
    }
    SECTION("a factor of 100 % changes nothing")
    {
        std::string       gcode = "G1 F6000\nG1 X10 Y0 E1\nG1 X20 Y0 E1\n";
        const std::string before = gcode;
        wipe_tower_tail_slowdown(gcode, 3.f, 1.f, 1.f, &origin, nullptr);
        CHECK(gcode == before);
    }
}

TEST_CASE("H3C: the end of every tower visit slows down, not on the first layer", "[BBSTowerH3C]")
{
    DynamicPrintConfig cfg = machine_config(H3C);
    cfg.set_deserialize_strict({ { "prime_tower_tail_speed_factor", "100%" } });
    const auto plain = direct_tower(cfg);
    cfg.set_deserialize_strict({ { "prime_tower_tail_speed_factor", "50%" }, { "prime_tower_tail_slowdown_volume", "3" } });
    const auto slowed = direct_tower(cfg);

    // First layer: unchanged.
    CHECK(change_on_layer(*plain, 0).gcode == change_on_layer(*slowed, 0).gcode);
    // Layer 2: the visit's end is slower, the same filament is laid.
    const std::string &a = change_on_layer(*plain, 1).gcode;
    const std::string &b = change_on_layer(*slowed, 1).gcode;
    CHECK(a != b);
    CHECK_THAT(total_extrusion(b), WithinAbs(total_extrusion(a), 1e-3));
    double last_f_a = 0., last_f_b = 0.;
    for (const std::string &line : split_lines(a)) {
        double f = 0.;
        if (line.rfind("G1 ", 0) == 0 && has_token(line, 'F', &f) && has_token(line, 'E'))
            last_f_a = f;
    }
    for (const std::string &line : split_lines(b)) {
        double f = 0.;
        if (line.rfind("G1 ", 0) == 0 && has_token(line, 'F', &f) && has_token(line, 'E'))
            last_f_b = f;
    }
    CHECK(last_f_b < last_f_a);

    // Not on a printer that keeps its hotends apart (X1C): the key is not even read.
    DynamicPrintConfig x1c = machine_config(X1C);
    x1c.set_deserialize_strict({ { "prime_tower_tail_speed_factor", "100%" } });
    const auto x1c_plain = direct_tower(x1c);
    x1c.set_deserialize_strict({ { "prime_tower_tail_speed_factor", "50%" } });
    const auto x1c_slowed = direct_tower(x1c);
    CHECK(change_on_layer(*x1c_plain, 1).gcode == change_on_layer(*x1c_slowed, 1).gcode);
}

TEST_CASE("Line ironing can be turned off; the ironing speed is prime_tower_ironing_speed", "[BBSTowerH3C]")
{
    DynamicPrintConfig cfg = machine_config(X1C);
    cfg.set_deserialize_strict({ { "wipe_tower_wall_gap", "1" }, { "prime_tower_flat_ironing", "0" } });
    const std::regex iron_travel(R"(^G1 X[-\d.]+ F600$)");
    SECTION("on (the default): the purge start is ironed into the wall gap")
    {
        const std::string &gcode = change_on_layer(*direct_tower(cfg), 1).gcode;
        CHECK(count_matching(gcode, iron_travel) >= 1);
    }
    SECTION("off (the H3C presets): no ironing")
    {
        cfg.set_deserialize_strict({ { "prime_tower_line_ironing", "0" } });
        const std::string &gcode = change_on_layer(*direct_tower(cfg), 1).gcode;
        CHECK(count_matching(gcode, iron_travel) == 0);
        CHECK(count_matching(gcode, std::regex(R"( F240$)")) == 0);
    }
    SECTION("flat ironing spirals at the ironing speed")
    {
        cfg.set_deserialize_strict({ { "prime_tower_flat_ironing", "1" }, { "prime_tower_ironing_speed", "20" } });
        cfg.option<ConfigOptionFloats>("filament_tower_ironing_area", true)->values = { 4., 4. };
        const std::string &gcode = change_on_layer(*direct_tower(cfg), 1).gcode;
        CHECK(count_matching(gcode, std::regex(R"(^G1 [XY][-\d.]+ F1200$)")) >= 1);
    }
}

TEST_CASE("The reheat before the purge names the nozzle as Bambu Studio does, with W", "[BBSTowerH3C]")
{
    SECTION("H3C: the filament (I)")
    {
        DynamicPrintConfig cfg = machine_config(H3C);
        set_cooling_before_tower(cfg, 10.);
        const auto t = direct_tower(cfg);
        // Layer 2 changes onto filament 0.
        CHECK(count_matching(change_on_layer(*t, 1).gcode, std::regex(R"(^M104 I0 S\d+ N0 W ;Wipe tower reheat before wipe$)")) == 1);
        CHECK(count_matching(change_on_layer(*t, 1).gcode, std::regex(R"(^M104 T)")) == 0);
        // Never on the first layer.
        CHECK(count_matching(change_on_layer(*t, 0).gcode, std::regex("reheat before wipe")) == 0);
    }
    SECTION("H2C: the physical extruder (T)")
    {
        DynamicPrintConfig cfg = machine_config(H2C);
        set_cooling_before_tower(cfg, 10.);
        const auto t = direct_tower(cfg);
        CHECK(count_matching(change_on_layer(*t, 1).gcode, std::regex(R"(^M104 T\d S\d+ N0 W ;Wipe tower reheat before wipe$)")) == 1);
    }
    SECTION("X1C: extruder 0")
    {
        DynamicPrintConfig cfg = machine_config(X1C);
        set_cooling_before_tower(cfg, 10.);
        const auto t = direct_tower(cfg);
        CHECK(count_matching(change_on_layer(*t, 1).gcode, std::regex(R"(^M104 T0 S\d+ N0 W ;Wipe tower reheat before wipe$)")) == 1);
    }
}

TEST_CASE("H3C: the tower interface temperature lines name the filament (I)", "[BBSTowerH3C]")
{
    auto interface_config = [](const Machine &m) {
        DynamicPrintConfig cfg = machine_config(m);
        cfg.set_deserialize_strict({ { "wipe_tower_interface_temp", "1" },
                                     { "wipe_tower_interface_trigger", "every_toolchange" },
                                     { "filament_tower_interface_print_temp", "250,250" } });
        return cfg;
    };
    SECTION("H3C")
    {
        const std::string &gcode = change_on_layer(*direct_tower(interface_config(H3C)), 1).gcode;
        CHECK(count_matching(gcode, std::regex(R"(^M109 I0 S250 N0 ;tower interface temperature$)")) == 1);
        // Back down to the print temperature: wait for the moves, then cool (C).
        CHECK(count_matching(gcode, std::regex(R"(^M104 I0 S\d+ N0 C ;tower interface done, normal temperature$)")) == 1);
        CHECK(count_matching(gcode, std::regex(R"(^M10[49] S)")) == 0);
    }
    SECTION("H2C keeps its extruder form")
    {
        const std::string &gcode = change_on_layer(*direct_tower(interface_config(H2C)), 1).gcode;
        CHECK(count_matching(gcode, std::regex(R"(^M109 S250 T\d ; tower interface temperature$)")) == 1);
        CHECK(count_matching(gcode, std::regex(R"(^M10[49] I)")) == 0);
    }
}

TEST_CASE("Side scrape: travel only, beside the tower, before the purge, only where the preset turns it on", "[BBSTowerH3C]")
{
    DynamicPrintConfig cfg = machine_config(H3C);
    // No brim: the scrape floor is the tower's first layer + 0.4 mm.
    cfg.set_deserialize_strict({ { "prime_tower_brim_width", "0" } });
    REQUIRE(cfg.opt_bool("prime_tower_side_scrape"));
    {
        PrintConfig pc;
        pc.apply(cfg, true);
        CHECK_THAT(prime_tower_side_scrape_margin(pc), WithinAbs(0.2 + 2., 1e-6));
    }
    const auto t = direct_tower(cfg, 8);

    // Not on the first layer, nor while the tower is too low to descend.
    CHECK_FALSE(change_on_layer(*t, 0).side_scrape);
    CHECK(change_on_layer(*t, 0).gcode.find("SIDE SCRAPE") == std::string::npos);

    const WipeTower::ToolChangeResult &tcr = change_on_layer(*t, 7);
    REQUIRE(tcr.side_scrape);
    // 1.5 mm below the top (z 1.6) would be 0.1: held at the floor, 0.2 + 0.4.
    CHECK_THAT(tcr.side_scrape_z_low, WithinAbs(0.6, 1e-4));
    CHECK_THAT(tcr.side_scrape_depth, WithinAbs(1.0, 1e-4));
    CHECK_THAT(tcr.side_scrape_min_height_above_base, WithinAbs(0.4, 1e-4));

    // The tool change lands on the scrape start, outside the front or back wall by the wall gap + the scrape length.
    CHECK(tcr.start_pos == tcr.tool_change_start_pos);
    const float depth = t->tower->get_depth();
    const bool  front = tcr.start_pos.y() < -2.f;
    const bool  back  = tcr.start_pos.y() > depth + 1.9f;
    CHECK((front || back));
    CHECK(tcr.start_pos.x() > 0.f);
    CHECK(tcr.start_pos.x() < t->tower->width());

    const std::vector<std::string> lines   = split_lines(tcr.gcode);
    const size_t                   scrape  = line_index(lines, "; CP TOOLCHANGE SIDE SCRAPE");
    const size_t                   unretract = line_index(lines, "[wipe_tower_deretract_gcode]");
    const size_t                   wipe    = line_index(lines, "; CP TOOLCHANGE WIPE");
    REQUIRE(scrape != std::string::npos);
    REQUIRE(unretract != std::string::npos);
    REQUIRE(wipe != std::string::npos);
    CHECK(scrape < unretract);
    CHECK(unretract < wipe);
    // After the tool change, before anything is extruded.
    CHECK(line_index(lines, "[filament_start_gcode]") < scrape);
    size_t z_low = 0, z_top = 0;
    for (size_t i = scrape; i < unretract; ++i) {
        CHECK_FALSE(has_token(lines[i], 'E'));
        z_low += lines[i].find(" Z[wipe_tower_scrape_z_low]") != std::string::npos;
        z_top += lines[i].find(" Z[wipe_tower_scrape_z_top]") != std::string::npos;
    }
    CHECK(z_low == 1);
    CHECK(z_top == 1);
    // The scrape runs at prime_tower_side_scrape_speed, 1400 mm/s.
    CHECK(tcr.gcode.find(" F84000") != std::string::npos);

    // Every layer keeps the full tower depth.
    for (const std::vector<WipeTower::ToolChangeResult> &layer : t->result)
        for (const WipeTower::ToolChangeResult &r : layer)
            for (const WipeTower::Extrusion &e : r.extrusions)
                CHECK(e.pos.y() < depth + 1e-3f);

    SECTION("the preset's switch off: no scrape")
    {
        cfg.set_deserialize_strict({ { "prime_tower_side_scrape", "0" } });
        const auto off = direct_tower(cfg, 8);
        for (const std::vector<WipeTower::ToolChangeResult> &layer : off->result)
            for (const WipeTower::ToolChangeResult &r : layer) {
                CHECK_FALSE(r.side_scrape);
                CHECK(r.gcode.find("wipe_tower_scrape") == std::string::npos);
                CHECK(r.gcode.find("wipe_tower_deretract_gcode") == std::string::npos);
            }
        PrintConfig pc;
        pc.apply(cfg, true);
        CHECK(prime_tower_side_scrape_margin(pc) == 0.);
    }
    SECTION("X1C: the presets do not turn it on")
    {
        DynamicPrintConfig x1c = machine_config(X1C);
        CHECK_FALSE(x1c.opt_bool("prime_tower_side_scrape"));
        const auto x = direct_tower(x1c, 8);
        for (const std::vector<WipeTower::ToolChangeResult> &layer : x->result)
            for (const WipeTower::ToolChangeResult &r : layer)
                CHECK_FALSE(r.side_scrape);
    }
}

TEST_CASE("H3C G-code: the side scrape descends beside the tower, the unretract follows it", "[BBSTowerH3C][PrintGCode]")
{
    DynamicPrintConfig cfg = machine_config(H3C);
    cfg.set_deserialize_strict({ { "prime_tower_brim_width", "0" } });
    Model             model;
    Print             print;
    const std::string gcode = slice(print, model, cfg);
    REQUIRE(! gcode.empty());
    // Every placeholder is filled in.
    CHECK(gcode.find("[wipe_tower_") == std::string::npos);

    const std::vector<std::string> lines = split_lines(gcode);
    static const std::regex        z_re(R"(^G1 [^;]*Z([\d.]+))");
    size_t                         scrapes = 0;
    for (size_t i = line_index(lines, "; CP TOOLCHANGE SIDE SCRAPE"); i != std::string::npos;
         i = line_index(lines, "; CP TOOLCHANGE SIDE SCRAPE", i + 1)) {
        ++scrapes;
        const size_t wipe = line_index(lines, "; CP TOOLCHANGE WIPE", i);
        REQUIRE(wipe != std::string::npos);
        std::vector<double> zs;
        size_t              unretracts = 0;
        size_t              last_z     = 0;
        for (size_t j = i; j < wipe; ++j) {
            std::smatch m;
            if (std::regex_search(lines[j], m, z_re) && ! has_token(lines[j], 'E')) {
                zs.push_back(std::stod(m[1].str()));
                last_z = j;
            }
            double e = 0.;
            if (lines[j].rfind("G1 ", 0) == 0 && has_token(lines[j], 'E', &e)) {
                // Only the unretract, after the diagonal, with no move of its own.
                CHECK(e > 0.);
                CHECK_FALSE(has_token(lines[j], 'X'));
                CHECK_FALSE(has_token(lines[j], 'Y'));
                CHECK(j > last_z);
                ++unretracts;
            }
        }
        REQUIRE(zs.size() == 2);
        CHECK(zs[0] < zs[1]);
        CHECK(zs[0] >= 0.6 - 1e-6);
        CHECK(unretracts <= 1);
    }
    CHECK(scrapes > 0);

    // The X1C presets have no scrape.
    Model             x1c_model;
    Print             x1c_print;
    const std::string x1c = slice(x1c_print, x1c_model, machine_config(X1C));
    CHECK(x1c.find("SIDE SCRAPE") == std::string::npos);
}

namespace {

// Two tools of the given diameters, one filament and one cube each, with a prime tower.
DynamicPrintConfig two_nozzle_tower(const char *diameters, const char *layer, const char *min_heights, const char *max_heights)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "nozzle_diameter", diameters },
        { "filament_diameter", "1.75,1.75" },
        { "filament_map", "1,2" },
        { "single_extruder_multi_material", "0" },
        { "enable_prime_tower", "1" },
        { "use_relative_e_distances", "1" },
        { "layer_height", layer },
        { "initial_layer_print_height", layer },
        { "min_layer_height", min_heights },
        { "max_layer_height", max_heights },
    });
    return config;
}

std::vector<std::string> tower_layer_warnings(const DynamicPrintConfig &config)
{
    Model model;
    for (int filament = 1; filament <= 2; ++filament) {
        ModelObject *object = model.add_object();
        object->name        = "cube-" + std::to_string(filament);
        object->add_volume(TriangleMesh(its_make_cube(10., 10., 2.)));
        object->add_instance()->set_offset(Vec3d(40. + 40. * filament, 60., 0.));
        object->ensure_on_bed();
        object->volumes.front()->config.set("extruder", filament);
    }
    Print print;
    print.apply(model, config);
    std::vector<StringObjectException> warnings;
    print.validate(&warnings);
    std::vector<std::string> out;
    for (const StringObjectException &w : warnings)
        if (w.string.find("The prime tower prints every filament at the layer height") != std::string::npos)
            out.push_back(w.string);
    return out;
}

} // namespace

TEST_CASE("Mixed nozzle diameters: a warning when the tower's layer height is outside a nozzle's range", "[BBSTowerH3C]")
{
    SECTION("0.4 + 0.8 at 0.12 mm: below the 0.8 nozzle's 0.16 mm minimum")
    {
        const std::vector<std::string> w = tower_layer_warnings(two_nozzle_tower("0.4,0.8", "0.12", "0.08,0.16", "0.28,0.56"));
        REQUIRE(w.size() == 1);
        CHECK(w.front().find("0.8 mm nozzle") != std::string::npos);
        CHECK(w.front().find("Filament 2") != std::string::npos);
    }
    SECTION("0.4 + 0.8 at 0.2 mm: inside both ranges")
    {
        CHECK(tower_layer_warnings(two_nozzle_tower("0.4,0.8", "0.2", "0.08,0.16", "0.28,0.56")).empty());
    }
    SECTION("one diameter: never")
    {
        CHECK(tower_layer_warnings(two_nozzle_tower("0.4,0.4", "0.3", "0.08,0.08", "0.28,0.28")).empty());
    }
    SECTION("no prime tower: never")
    {
        DynamicPrintConfig config = two_nozzle_tower("0.4,0.8", "0.12", "0.08,0.16", "0.28,0.56");
        config.set_deserialize_strict({ { "enable_prime_tower", "0" } });
        CHECK(tower_layer_warnings(config).empty());
    }
}
