#include <catch2/catch.hpp>

#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

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
// tool change on which that filament is loaded. Every change purges 600 mm3, no nozzle change.
struct DirectTower
{
    PrintConfig                                           config;
    std::unique_ptr<WipeTower>                            tower;
    std::vector<std::vector<WipeTower::ToolChangeResult>> result;
};

std::unique_ptr<DirectTower> direct_tower(const DynamicPrintConfig &cfg, const std::vector<std::pair<int, int>> &layers,
                                          float first_layer_flow_ratio = 1.f)
{
    auto t = std::make_unique<DirectTower>();
    t->config.apply(cfg, true);
    t->tower = std::make_unique<WipeTower>(t->config, 0, Vec3d::Zero(), 15.f, size_t(layers.front().first), 0.2f * layers.size());
    t->tower->set_first_layer_flow_ratio(first_layer_flow_ratio);
    t->tower->set_extruder(0, t->config);
    t->tower->set_extruder(1, t->config);
    for (size_t layer = 0; layer < layers.size(); ++layer) {
        const float z = 0.2f * float(layer + 1);
        const auto [from, to] = layers[layer];
        t->tower->plan_toolchange(z, 0.2f, from, from);
        if (from != to)
            t->tower->plan_toolchange(z, 0.2f, from, to, 600.f, 0.f, WipeTower::NozzleChangePlan());
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
