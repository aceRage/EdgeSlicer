#include <catch2/catch.hpp>

#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTowerInterface.hpp"
#include "libslic3r/GCodeWriter.hpp"
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
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// Bambu Studio 2.9.1 tower leftovers on the Type 1 block planner (PR-B of tests/survey_bbs_tower_gap_2026-11-14.md):
// the first-layer purge flow (initial_layer_flow_ratio), the purge start row, the contact-layer tower interface features
// (enable_tower_interface_features: run-in from outside, wall gaps, interface temperature, M620.13 placeholders), Orca's
// cool-down during the tower, and the adhesiveness category fallback.

namespace {

PresetBundle &leftover_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    if (bundle)
        return *bundle;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bbsleft_%%%%-%%%%");
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

const Machine X1C_PLA_PETG{ "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C", "Bambu PETG HF @BBL X1C" };
const Machine H2C_PLA_PETG{ "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C", "Bambu PETG HF @BBL H2C" };
// Bambu Studio offers the tower interface features on multi-extruder and hotend-as-extruder printers only.
const Machine H2D_PLA_PETG{ "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D", "Bambu PETG HF @BBL H2D 0.4 nozzle" };
const Machine H3C_PLA_PETG{ "Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C", "Bambu PETG HF @BBL H3C" };

DynamicPrintConfig machine_config(const Machine &m)
{
    PresetBundle &b = leftover_bundle();
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
        // The H3C presets scrape the tower side after a tool change; these cases look at the purge itself.
        { "prime_tower_side_scrape", 0 },
    });
    if (cfg.option("filament_map") != nullptr)
        cfg.set_deserialize_strict({ { "filament_map_mode", "Manual" }, { "filament_map", "1,2" } });
    return cfg;
}

// A Type 1 tower planned directly from (old, new) filament pairs, one per 0.2 mm layer; old == new is a layer without a
// tool change on which that filament is loaded. Every change purges 100 mm3 (the tower stays on the bed), no nozzle change.
struct DirectTower
{
    PrintConfig                                           config;
    std::unique_ptr<WipeTower>                            tower;
    std::vector<std::vector<WipeTower::ToolChangeResult>> result;
};

std::unique_ptr<DirectTower> direct_tower(const DynamicPrintConfig &cfg, const std::vector<std::pair<int, int>> &layers,
                                          float first_layer_flow_ratio = 1.f, bool bbl_gcode = true,
                                          const WipeTower::NozzleChangePlan &plan = WipeTower::NozzleChangePlan())
{
    auto t = std::make_unique<DirectTower>();
    t->config.apply(cfg, true);
    t->tower = std::make_unique<WipeTower>(t->config, 0, Vec3d::Zero(), 15.f, size_t(layers.front().first), 0.2f * layers.size());
    t->tower->set_first_layer_flow_ratio(first_layer_flow_ratio);
    t->tower->set_bbl_gcode_tags(bbl_gcode);
    t->tower->set_extruder(0, t->config);
    t->tower->set_extruder(1, t->config);
    for (size_t layer = 0; layer < layers.size(); ++layer) {
        const float z = 0.2f * float(layer + 1);
        const auto [from, to] = layers[layer];
        t->tower->plan_toolchange(z, 0.2f, from, from);
        if (from != to)
            t->tower->plan_toolchange(z, 0.2f, from, to, 100.f, 0.f, plan);
    }
    t->tower->generate(t->result);
    return t;
}

std::vector<std::pair<int, int>> alternating(int layers)
{
    std::vector<std::pair<int, int>> out;
    for (int layer = 0; layer < layers; ++layer)
        out.emplace_back(layer % 2, 1 - layer % 2);
    return out;
}

const WipeTower::ToolChangeResult *change_on_layer(const DirectTower &t, size_t layer)
{
    if (t.result.size() <= layer)
        return nullptr;
    for (const WipeTower::ToolChangeResult &tcr : t.result[layer])
        if (tcr.initial_tool != tcr.new_tool)
            return &tcr;
    return nullptr;
}

std::string layer_gcode(const DirectTower &t, size_t layer)
{
    std::string out;
    if (layer < t.result.size())
        for (const WipeTower::ToolChangeResult &tcr : t.result[layer])
            out += tcr.gcode;
    return out;
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
    for (std::string tok; ss >> tok;) {
        if (tok[0] == ';')
            break;
        if (tok.size() > 1 && tok[0] == key) {
            if (value != nullptr)
                *value = std::stod(tok.substr(1));
            return true;
        }
    }
    return false;
}

double total_extrusion(const std::string &gcode)
{
    double sum = 0.;
    for (const std::string &line : split_lines(gcode)) {
        double e = 0.;
        if (line.rfind("G1 ", 0) == 0 && has_token(line, 'E', &e) && e > 0.)
            sum += e;
    }
    return sum;
}

bool ends_with(const std::string &s, const std::string &suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The first line starting with `prefix` and ending with `suffix` (the temperature in between follows the flow variant).
std::vector<std::string>::const_iterator find_line(const std::vector<std::string> &lines, const std::string &prefix, const std::string &suffix)
{
    return std::find_if(lines.begin(), lines.end(),
                        [&](const std::string &l) { return l.rfind(prefix, 0) == 0 && ends_with(l, suffix); });
}

size_t count_matching_suffix(const std::string &gcode, const std::string &prefix, const std::string &suffix)
{
    size_t n = 0;
    for (const std::string &line : split_lines(gcode))
        if (line.rfind(prefix, 0) == 0 && ends_with(line, suffix))
            ++n;
    return n;
}

size_t count_prefix(const std::string &gcode, const std::string &prefix)
{
    size_t n = 0;
    for (const std::string &line : split_lines(gcode))
        if (line.rfind(prefix, 0) == 0)
            ++n;
    return n;
}

} // namespace

TEST_CASE("The first tower layer purges at the process' initial layer flow ratio", "[BBSTowerLeftovers]")
{
    // Bambu Studio 2.9.1 hands initial_layer_flow_ratio (bottom_solid_infill_flow_ratio here) to the tower's first layer.
    const DynamicPrintConfig cfg   = machine_config(X1C_PLA_PETG);
    const auto               plain = direct_tower(cfg, alternating(2), 1.f);
    const auto               thin  = direct_tower(cfg, alternating(2), 0.8f);
    const auto              *p0 = change_on_layer(*plain, 0), *t0 = change_on_layer(*thin, 0);
    const auto              *p1 = change_on_layer(*plain, 1), *t1 = change_on_layer(*thin, 1);
    REQUIRE(p0 != nullptr);
    REQUIRE(t0 != nullptr);
    REQUIRE(p1 != nullptr);
    REQUIRE(t1 != nullptr);
    const double e_plain = total_extrusion(p0->gcode), e_thin = total_extrusion(t0->gcode);
    REQUIRE(e_plain > 1.);
    CHECK(e_thin < e_plain * 0.95);
    // Only the first layer.
    CHECK_THAT(total_extrusion(t1->gcode), WithinRel(total_extrusion(p1->gcode), 1e-6));
}

namespace {

// PLA (filament 0, category 100) and PETG (filament 1, category 300): layer 0 changes PLA -> PETG, layers 1 and 2 print
// PETG only, layer 3 changes back to PLA. The PLA block is a contact block on layer 1 (PLA stops) and on layer 3 (PLA
// starts again, a contact tool change); layer 3 is odd, so its purge starts on the left wall.
const std::vector<std::pair<int, int>> contact_layers{ { 0, 1 }, { 1, 1 }, { 1, 1 }, { 1, 0 }, { 0, 1 } };

DynamicPrintConfig interface_config(const Machine &m, bool features)
{
    DynamicPrintConfig cfg = machine_config(m);
    cfg.set_deserialize_strict({ { "enable_tower_interface_features", features ? "1" : "0" },
                                 { "filament_adhesiveness_category", "100,300" },
                                 { "nozzle_temperature", "220,240" },
                                 { "nozzle_temperature_initial_layer", "220,240" },
                                 { "filament_tower_interface_print_temp", "235,260" },
                                 { "filament_tower_interface_pre_extrusion_dist", "10,10" },
                                 { "filament_tower_interface_pre_extrusion_length", "0,0" },
                                 { "wipe_tower_wall_gap", "1" },
                                 { "wipe_tower_wall_type", "rectangle" } });
    return cfg;
}

std::string all_layers(const DirectTower &t)
{
    std::string out;
    for (size_t layer = 0; layer < t.result.size(); ++layer)
        out += layer_gcode(t, layer);
    return out;
}

} // namespace

TEST_CASE("Tower interface features: a contact tool change starts its purge outside the tower", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    const auto  on    = direct_tower(interface_config(H2D_PLA_PETG, true), contact_layers);
    const auto  off   = direct_tower(interface_config(H2D_PLA_PETG, false), contact_layers);
    const auto *c_on  = change_on_layer(*on, 3);
    const auto *c_off = change_on_layer(*off, 3);
    REQUIRE(c_on != nullptr);
    REQUIRE(c_off != nullptr);
    CHECK(c_on->is_contact);
    CHECK(c_off->is_contact);
    const float width = on->tower->width();
    // tool_change_start_pos: where GCode moves for the purge (a block filled before it may own start_pos).
    const float x_on = c_on->tool_change_start_pos.x(), x_off = c_off->tool_change_start_pos.x();
    INFO("tower width " << width << ", start " << x_on << " (off: " << x_off << "), tower at " << on->tower->position().x() << ","
                         << on->tower->position().y());
    // Without the features the purge starts on the wall; with them 10 mm out (layer 3 starts on the left).
    CHECK(x_off > -0.01f);
    CHECK(x_off < width + 0.01f);
    CHECK(x_on < x_off - 9.9f);
    CHECK(x_on > x_off - 10.1f);
    CHECK_THAT(c_on->tool_change_start_pos.y(), WithinAbs(c_off->tool_change_start_pos.y(), 1e-3));
    // A change that is not on a contact layer stays on the wall.
    const auto *normal = change_on_layer(*on, 4);
    REQUIRE(normal != nullptr);
    CHECK_FALSE(normal->is_contact);
    CHECK(normal->tool_change_start_pos.x() > -0.01f);
    CHECK(normal->tool_change_start_pos.x() < width + 0.01f);
}

TEST_CASE("Tower interface features: an unfilled contact block is filled hot, from outside the tower", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    // Layer 1: the PLA block's contact layer (PLA stops), filled solid by the PETG left loaded.
    const auto        on   = direct_tower(interface_config(H2D_PLA_PETG, true), contact_layers);
    const auto        off  = direct_tower(interface_config(H2D_PLA_PETG, false), contact_layers);
    const std::string g_on = layer_gcode(*on, 1), g_off = layer_gcode(*off, 1);
    // Bambu Studio's forms: M109 T<physical extruder> S<t> N0, a 2 mm prime, the rows, then M400 + M104 ... C.
    CHECK(count_prefix(g_on, "M109 T0 S260 N0 ;Wipe tower interface layer enter") == 1);
    CHECK(count_matching_suffix(g_on, "M104 T0 S", " N0 C ;Wipe tower interface layer leave restore") == 1);
    CHECK(count_prefix(g_on, "G1 E2.0000 F100") == 1);
    CHECK(g_off.find("Wipe tower interface layer") == std::string::npos);
    CHECK(count_prefix(g_off, "G1 E2.0000 F100") == 0);
    const std::vector<std::string> lines = split_lines(g_on);
    const auto enter = std::find(lines.begin(), lines.end(), std::string("M109 T0 S260 N0 ;Wipe tower interface layer enter"));
    const auto leave = find_line(lines, "M104 T0 S", " N0 C ;Wipe tower interface layer leave restore");
    REQUIRE(enter != lines.end());
    REQUIRE(leave != lines.end());
    CHECK(enter < leave);
    CHECK(*(leave - 1) == "M400");
}

TEST_CASE("Tower interface features: H3C names the hotend by filament", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    const auto        on = direct_tower(interface_config(H3C_PLA_PETG, true), contact_layers);
    const std::string g  = layer_gcode(*on, 1);
    CHECK(count_prefix(g, "M109 I1 S260 N0 ;Wipe tower interface layer enter") == 1);
    INFO(g);
    CHECK(count_prefix(g, "M104 I1 S") == 1);
    CHECK(g.find(" N0 C ;Wipe tower interface layer leave restore") != std::string::npos);
}

TEST_CASE("Tower interface features turn the Edge interface options off on the Bambu tower", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    DynamicPrintConfig cfg = interface_config(H2D_PLA_PETG, true);
    cfg.set_deserialize_strict({ { "wipe_tower_interface_temp", "1" },
                                 { "wipe_tower_interface_extra_prime", "1" },
                                 { "wipe_tower_interface_trigger", "every_toolchange" } });
    CHECK(all_layers(*direct_tower(cfg, contact_layers)).find("; tower interface\n") == std::string::npos);
    cfg.set_deserialize_strict({ { "enable_tower_interface_features", "0" } });
    CHECK(all_layers(*direct_tower(cfg, contact_layers)).find("; tower interface\n") != std::string::npos);
}

TEST_CASE("The unretract can push extra filament, only when something was retracted", "[BBSTowerLeftovers][GCodeWriter]")
{
    GCodeWriter writer;
    writer.config.use_relative_e_distances.value = true;
    writer.config.retraction_length.values       = { 0.8 };
    writer.config.retract_restart_extra.values   = { 0. };
    writer.set_extruders({ 0 });
    writer.set_extruder(0);
    REQUIRE(writer.extruder() != nullptr);
    writer.retract();
    CHECK_THAT(writer.extruder()->unretract(1.5), WithinAbs(2.3, 1e-9));
    // Nothing retracted: nothing pushed (Bambu Studio's unretract(extra_retract) writes no line then).
    CHECK_THAT(writer.extruder()->unretract(1.5), WithinAbs(0., 1e-9));
    writer.retract();
    const std::string line = writer.unretract(1.5);
    double            e    = 0.;
    REQUIRE(has_token(line, 'E', &e));
    CHECK_THAT(e, WithinAbs(2.3, 1e-5));
}

namespace {

// Two 4 mm cubes: the first PLA throughout, the second PLA below 2 mm and PETG above, so the PETG band starts on the
// tower at 2 mm with a contact tool change.
std::string slice_contact_plate(Print &print, const DynamicPrintConfig &cfg)
{
    Model model;
    for (int i = 0; i < 2; ++i) {
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(TriangleMesh(its_make_cube(20., 20., 4.)));
        object->volumes.front()->name = object->name;
        object->config.set("extruder", 1);
        object->add_instance()->set_offset(Vec3d(60. + 40. * i, 60., 0.));
        object->ensure_on_bed();
        if (i == 1) {
            DynamicPrintConfig range_config;
            range_config.set_key_value("extruder", new ConfigOptionInt(2));
            range_config.set_key_value("layer_height", new ConfigOptionFloat(cfg.opt_float("layer_height")));
            object->layer_config_ranges[t_layer_height_range{ 2., 4. }].assign_config(std::move(range_config));
        }
    }
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    const StringObjectException err = print.validate();
    INFO(err.string);
    REQUIRE(err.string.empty());
    print.set_status_silent();
    return Test::gcode(print);
}

} // namespace

TEST_CASE("H2C G-code: a contact tool change runs the firmware interface purge", "[BBSTowerLeftovers][TowerInterfaceBBS][PrintGCode]")
{
    auto slice_with = [](bool features) {
        DynamicPrintConfig cfg = machine_config(H2C_PLA_PETG);
        cfg.set_deserialize_strict({ { "enable_tower_interface_features", features ? "1" : "0" },
                                     { "curr_bed_type", "Textured PEI Plate" },
                                     { "filament_tower_interface_print_temp", "-1,262" },
                                     { "filament_tower_interface_purge_volume", "20,25" } });
        Print print;
        return slice_contact_plate(print, cfg);
    };
    const std::string on = slice_with(true), off = slice_with(false);
    REQUIRE_FALSE(on.empty());
    REQUIRE_FALSE(off.empty());
    // The PETG band starts on the tower at 2 mm: a contact change, the only interface of the print.
    const size_t purges = count_prefix(on, "M620.13 W0 L25 T262 R0.0");
    CHECK(purges >= 1);
    CHECK(count_prefix(on, "M620.13") == purges);
    // G150.1 (the wipe before it) once per purge; the start G-code has its own.
    CHECK(count_prefix(on, "G150.1") == count_prefix(off, "G150.1") + purges);
    CHECK(count_prefix(off, "M620.13") == 0);
}

TEST_CASE("Cool down during the tower: the print temperature goes back on before the contact fill", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    DynamicPrintConfig cfg = interface_config(H2D_PLA_PETG, true);
    cfg.set_deserialize_strict({ { "enable_tower_interface_cooldown_during_tower", "1" } });
    const auto                     on    = direct_tower(cfg, contact_layers);
    const std::vector<std::string> lines = split_lines(layer_gcode(*on, 1));
    const auto enter = std::find(lines.begin(), lines.end(), std::string("M109 T0 S260 N0 ;Wipe tower interface layer enter"));
    const auto cool  = find_line(lines, "M104 T0 S", " N0 C ;Wipe tower interface cool down during tower");
    REQUIRE(enter != lines.end());
    REQUIRE(cool != lines.end());
    CHECK(enter < cool);
    // Before the first row of the fill: no extrusion with X or Y between the heat-up and the cool-down.
    for (auto it = enter; it != cool; ++it)
        CHECK_FALSE((it->rfind("G1 X", 0) == 0 || it->rfind("G1 Y", 0) == 0));
    CHECK(find_line(lines, "M104", "Wipe tower interface layer leave restore") == lines.end());
}

TEST_CASE("Cool down during the tower with Edge's interface heat-up", "[BBSTowerLeftovers][TowerInterface]")
{
    DynamicPrintConfig cfg = interface_config(X1C_PLA_PETG, false);
    cfg.set_deserialize_strict({ { "wipe_tower_interface_temp", "1" }, { "wipe_tower_interface_trigger", "every_toolchange" } });
    const std::string plain = all_layers(*direct_tower(cfg, contact_layers));
    cfg.set_deserialize_strict({ { "enable_tower_interface_cooldown_during_tower", "1" } });
    const std::string cooled = all_layers(*direct_tower(cfg, contact_layers));
    const size_t      heats  = plain.find("tower interface temperature") == std::string::npos ? 0 : 1;
    REQUIRE(heats == 1);
    CHECK(plain.find("tower interface cool down during tower") == std::string::npos);
    CHECK(plain.find("tower interface done, normal temperature") != std::string::npos);
    CHECK(cooled.find("tower interface cool down during tower") != std::string::npos);
    CHECK(cooled.find("tower interface done, normal temperature") == std::string::npos);
    // The cool-down follows its heat-up directly.
    const std::vector<std::string> lines = split_lines(cooled);
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find("tower interface temperature") != std::string::npos) {
            REQUIRE(i + 1 < lines.size());
            CHECK(lines[i + 1].find("tower interface cool down during tower") != std::string::npos);
        }
}

TEST_CASE("A filament without an adhesiveness category gets Bambu's for its type", "[BBSTowerLeftovers][TowerBlocks]")
{
    // A category that is set always wins.
    CHECK(WipeTower::adhesiveness_category(300, "PLA", false) == 300);
    CHECK(WipeTower::adhesiveness_category(0, "PLA", false) == 100);
    CHECK(WipeTower::adhesiveness_category(0, "pla-cf ", false) == 100);
    CHECK(WipeTower::adhesiveness_category(0, "ABS", false) == 200);
    CHECK(WipeTower::adhesiveness_category(0, "ASA-CF", false) == 200);
    CHECK(WipeTower::adhesiveness_category(0, "PETG", false) == 300);
    CHECK(WipeTower::adhesiveness_category(0, "PETG-CF", false) == 300);
    CHECK(WipeTower::adhesiveness_category(0, "PCTG", false) == 300);
    CHECK(WipeTower::adhesiveness_category(0, "PET-CF", false) == 800);
    CHECK(WipeTower::adhesiveness_category(0, "PA6-CF", false) == 400);
    CHECK(WipeTower::adhesiveness_category(0, "PAHT-CF", false) == 400);
    CHECK(WipeTower::adhesiveness_category(0, "PPA-CF", false) == 400);
    CHECK(WipeTower::adhesiveness_category(0, "PC", false) == 500);
    CHECK(WipeTower::adhesiveness_category(0, "PCTG-CF", false) == 300);
    CHECK(WipeTower::adhesiveness_category(0, "TPU-AMS", false) == 600);
    CHECK(WipeTower::adhesiveness_category(0, "PVA", true) == 704);
    CHECK(WipeTower::adhesiveness_category(0, "BVOH", false) == 797);
    CHECK(WipeTower::adhesiveness_category(0, "HIPS", false) == 798);
    CHECK(WipeTower::adhesiveness_category(0, "PPS-CF", false) == 801);
    CHECK(WipeTower::adhesiveness_category(0, "PE-CF", false) == 901);
    CHECK(WipeTower::adhesiveness_category(0, "PP-GF", false) == 902);
    // Support filaments get Bambu's support bands, not their base material's.
    CHECK(WipeTower::adhesiveness_category(0, "PLA", true) == 702);
    CHECK(WipeTower::adhesiveness_category(0, "PA", true) == 703);
    CHECK(WipeTower::adhesiveness_category(0, "ABS", true) == 706);
    // No Bambu category for these: they stay uncategorised.
    for (const char *type : { "EVA", "PHA", "SBS", "CoPE", "PEEK", "" })
        CHECK(WipeTower::adhesiveness_category(0, type, false) == 0);
}

TEST_CASE("An uncategorised PLA and PETG purge into separate blocks", "[BBSTowerLeftovers][TowerBlocks]")
{
    DynamicPrintConfig cfg = machine_config(X1C_PLA_PETG);
    cfg.set_deserialize_strict({ { "filament_adhesiveness_category", "0,0" } });
    REQUIRE(cfg.option<ConfigOptionStrings>("filament_type")->get_at(1) == "PETG");
    const auto t = direct_tower(cfg, alternating(4));
    REQUIRE(t->tower->blocks().size() == 2);
    std::vector<int> categories;
    for (const auto &b : t->tower->blocks())
        categories.push_back(b.filament_adhesiveness_category);
    std::sort(categories.begin(), categories.end());
    CHECK(categories == std::vector<int>{ 100, 300 });
}


// Review #1062 (d): Bambu Studio offers the switch on multi-extruder and hotend-as-extruder printers only; elsewhere it
// does nothing.
TEST_CASE("Tower interface features do nothing on a single-extruder printer", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    const DynamicPrintConfig on_cfg = interface_config(X1C_PLA_PETG, true);
    REQUIRE_FALSE(TowerInterface::bambu_features_supported(on_cfg));
    REQUIRE(TowerInterface::bambu_features_supported(interface_config(H2D_PLA_PETG, true)));
    REQUIRE(TowerInterface::bambu_features_supported(interface_config(H3C_PLA_PETG, true)));
    const auto  on  = direct_tower(on_cfg, contact_layers);
    const auto  off = direct_tower(interface_config(X1C_PLA_PETG, false), contact_layers);
    const auto *c_on = change_on_layer(*on, 3), *c_off = change_on_layer(*off, 3);
    REQUIRE(c_on != nullptr);
    REQUIRE(c_off != nullptr);
    CHECK_THAT(c_on->tool_change_start_pos.x(), WithinAbs(c_off->tool_change_start_pos.x(), 1e-4));
    CHECK(all_layers(*on) == all_layers(*off));
}

// Review #1062 (d): a non-Bambu printer on this tower gets its flavour's temperature lines, never Bambu's N0 / C / W.
TEST_CASE("Tower interface features: no Bambu temperature forms outside Bambu G-code", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    const auto                     plain = direct_tower(interface_config(H2D_PLA_PETG, true), contact_layers, 1.f, false);
    const std::vector<std::string> lines = split_lines(layer_gcode(*plain, 1));
    const auto enter = find_line(lines, "M109 S260", "Wipe tower interface layer enter");
    const auto leave = find_line(lines, "M104 S", "Wipe tower interface layer leave restore");
    REQUIRE(enter != lines.end());
    REQUIRE(leave != lines.end());
    for (const std::string &l : lines)
        if (l.find("Wipe tower interface layer") != std::string::npos) {
            INFO(l);
            CHECK(l.find(" N0") == std::string::npos);
        }
}

// Review #1062 (i): the run-in start stays on the bed: shortened at the bed edge, also through the tower's rotation.
TEST_CASE("Tower interface features: the run-in start is clamped to the bed", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    SECTION("tower beside the left bed edge")
    {
        DynamicPrintConfig cfg = interface_config(H2D_PLA_PETG, true);
        const double       bed_min_x = TowerInterface::shared_printable_box(cfg).min.x();
        cfg.set_deserialize_strict({ { "wipe_tower_x", bed_min_x + 4. } });
        const auto  t   = direct_tower(cfg, contact_layers);
        const auto  off = direct_tower(interface_config(H2D_PLA_PETG, false), contact_layers);
        const auto *c   = change_on_layer(*t, 3);
        const auto *c0  = change_on_layer(*off, 3);
        REQUIRE(c != nullptr);
        REQUIRE(c0 != nullptr);
        // On the bed's left edge (tower-local x = bed min - tower x), 4 mm plus the wall offset instead of 10 mm out.
        CHECK_THAT(c->tool_change_start_pos.x(), WithinAbs(-4.f, 1e-3));
        CHECK(c0->tool_change_start_pos.x() + 4.f < 10.f);
    }
    SECTION("tower turned 90 degrees, its left wall facing the front bed edge")
    {
        DynamicPrintConfig cfg = interface_config(H2D_PLA_PETG, true);
        const double       bed_min_y = TowerInterface::shared_printable_box(cfg).min.y();
        cfg.set_deserialize_strict({ { "wipe_tower_rotation_angle", 90 }, { "wipe_tower_x", 200. }, { "wipe_tower_y", bed_min_y + 3. } });
        const auto  t = direct_tower(cfg, contact_layers);
        const auto *c = change_on_layer(*t, 3);
        REQUIRE(c != nullptr);
        // Tower-local -X is the bed's -Y: the start is moved only as far as the bed allows, 3 mm minus the wall offset.
        CHECK(c->tool_change_start_pos.x() < 0.f);
        CHECK(c->tool_change_start_pos.x() > -3.01f);
    }
}

// Review #1062 (i): the wall gaps for a contact run-in are cut on the contact layer's three layers below.
TEST_CASE("Tower interface features: wall gaps on the three layers below a contact change", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    // PLA stops after layer 0 and starts again on layer 5: the run-in of layer 5 needs gaps on layers 2, 3 and 4.
    const std::vector<std::pair<int, int>> layers{ { 0, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 0 }, { 0, 1 } };
    const auto on  = direct_tower(interface_config(H2D_PLA_PETG, true), layers);
    const auto off = direct_tower(interface_config(H2D_PLA_PETG, false), layers);
    // Only the outer walls differ there (cut open at the run-in's place, printed in pieces); nothing else on those layers
    // depends on the switch.
    for (size_t layer : { size_t(2), size_t(3), size_t(4) }) {
        INFO("layer " << layer);
        CHECK(layer_gcode(*on, layer) != layer_gcode(*off, layer));
    }
    // Layer 6 (above the contact) is not cut for it.
    CHECK(layer_gcode(*on, 6) == layer_gcode(*off, 6));
}

// Review #1062 (i): a contact change onto another nozzle rams the outgoing filament and still runs in from outside.
TEST_CASE("Tower interface features: run-in together with ramming", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    WipeTower::NozzleChangePlan plan;
    plan.need_ramming    = true;
    plan.extruder_change = true;
    const auto  on  = direct_tower(interface_config(H2D_PLA_PETG, true), contact_layers, 1.f, true, plan);
    const auto  off = direct_tower(interface_config(H2D_PLA_PETG, false), contact_layers, 1.f, true, plan);
    const auto *c   = change_on_layer(*on, 3);
    const auto *c0  = change_on_layer(*off, 3);
    REQUIRE(c != nullptr);
    REQUIRE(c0 != nullptr);
    CHECK(c->is_contact);
    CHECK(c->nozzle_change_result.has_action());
    CHECK(c0->nozzle_change_result.has_action());
    CHECK_THAT(c->tool_change_start_pos.x(), WithinAbs(c0->tool_change_start_pos.x() - 10.f, 1e-3));
}

// Review #1062 (a): Bambu reserves no room for the run-in; Print::validate() warns when a part is within it.
TEST_CASE("A part within the contact run-in beside the tower is warned about", "[BBSTowerLeftovers][TowerInterfaceBBS]")
{
    auto warns = [](bool features, double part_max_x) {
        DynamicPrintConfig cfg = machine_config(H2C_PLA_PETG);
        cfg.set_deserialize_strict({ { "enable_tower_interface_features", features ? "1" : "0" },
                                     { "curr_bed_type", "Textured PEI Plate" },
                                     { "prime_tower_brim_width", "0" },
                                     { "wipe_tower_wall_type", "rectangle" } });
        Model model;
        for (int i = 0; i < 2; ++i) {
            ModelObject *object = model.add_object();
            object->name        = "cube" + std::to_string(i);
            object->add_volume(TriangleMesh(its_make_cube(10., 10., 4.)));
            object->config.set("extruder", i + 1);
            // The tower is at X160 Y180: the first cube ends part_max_x left of its wall, the second is far away.
            object->add_instance()->set_offset(i == 0 ? Vec3d(160. - part_max_x - 10., 182., 0.) : Vec3d(60., 60., 0.));
            object->ensure_on_bed();
        }
        Print print;
        print.is_BBL_printer() = true;
        print.apply(model, cfg);
        std::vector<StringObjectException> warnings;
        print.validate(&warnings);
        return std::any_of(warnings.begin(), warnings.end(),
                           [](const StringObjectException &w) { return w.string.find("tower interface features") != std::string::npos; });
    };
    CHECK(warns(true, 5.));
    CHECK_FALSE(warns(false, 5.));
    CHECK_FALSE(warns(true, 15.));
}
