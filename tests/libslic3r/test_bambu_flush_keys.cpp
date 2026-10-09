#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

// Bambu Studio's per-filament flush settings and pre-tower cooling:
//   filament_flush_temp / filament_flush_temp_fast   -> flush_temperatures      (M620.10 ... T<t>)
//   filament_flush_volumetric_speed                  -> flush_volumetric_speeds (M620.10 / M620.11 ... F<f>)
//   filament_cooling_before_tower                    -> M620.15 C{new_filament_temp - filament_cooling_before_tower[next]}
// The BBL profiles carry them, but filament presets used to drop them, so the G-code always ran on
// the defaults (M620.15 C245 / C220 for PETG HF 245 + PLA 220 on an H2D, where Bambu Studio writes
// C235 / C210). Filament presets hold them now, per flow variant, and GCode.cpp publishes them the
// way Bambu Studio does (no pre-tower cooling on the first layer).
//
// A toolchange that cools before the tower must heat back up before the new filament prints: Bambu
// Studio's tower writes "M104 T<extruder> S<print temperature> N0 ;Wipe tower reheat before wipe"
// (WipeTower.cpp toolchange_wipe_new). Without it an H2D printed the rest of the plate 10 degrees cold
// (owner's hardware test of #392). check_reheats() below is the G-code-level rule.

namespace {

PresetBundle &vendor_bundle(const std::string &vendor)
{
    static std::map<std::string, std::unique_ptr<PresetBundle>> bundles;
    static std::unique_ptr<PresetBundle>                        library;
    auto it = bundles.find(vendor);
    if (it != bundles.end())
        return *it->second;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("flushkeys_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    if (!library) {
        library = std::make_unique<PresetBundle>();
        library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    auto b = std::make_unique<PresetBundle>();
    b->load_vendor_configs_from_json(profiles, vendor, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, library.get());
    set_data_dir(saved_data_dir);
    return *(bundles[vendor] = std::move(b));
}

const DynamicPrintConfig &filament_preset(const std::string &vendor, const std::string &name)
{
    const Preset *preset = vendor_bundle(vendor).filaments.find_preset(name, false);
    INFO("preset: " << name);
    REQUIRE(preset != nullptr);
    return preset->config;
}

std::vector<int> ints_of(const DynamicPrintConfig &cfg, const char *key)
{
    const auto *opt = cfg.option<ConfigOptionIntsNullable>(key);
    INFO("key: " << key);
    REQUIRE(opt != nullptr);
    return opt->values;
}

std::vector<double> floats_of(const DynamicPrintConfig &cfg, const char *key)
{
    const auto *opt = cfg.option<ConfigOptionFloatsNullable>(key);
    INFO("key: " << key);
    REQUIRE(opt != nullptr);
    return opt->values;
}

const std::string H2D_PLA     = "Bambu PLA Basic @BBL H2D";           // 220 C, range high 240
const std::string H2D_PETG_HF = "Bambu PETG HF @BBL H2D 0.4 nozzle";  // 245 C, range high 270

struct Machine
{
    std::string printer;
    std::string process;
};
const Machine H2D{ "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D" };
const Machine H2C{ "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C" };

// BBL filaments with a prime tower; nozzles / map empty = the profile's own.
DynamicPrintConfig bbl_config(const Machine &m, const std::vector<std::string> &filaments, const std::string &nozzles = "Standard,Standard",
                              const std::string &map = "1,2")
{
    PresetBundle &b = vendor_bundle("BBL");
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(filaments.front(), true));
    b.filament_presets = { filaments.front() };
    std::vector<std::string> colours{ "#E01919", "#1943E0", "#19E043", "#E0E019" };
    colours.resize(filaments.size(), "#FFFFFF");
    b.set_num_filaments(unsigned(filaments.size()), colours);
    b.filament_presets = filaments;
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "wipe_tower_x", 40. },
        { "wipe_tower_y", 250. },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments", 0 },
    });
    if (!nozzles.empty())
        cfg.set_deserialize_strict({ { "nozzle_volume_type", nozzles } });
    if (!map.empty())
        cfg.set_deserialize_strict({ { "filament_map_mode", "Manual" }, { "filament_map", map } });
    return cfg;
}

DynamicPrintConfig h2d_config(const std::vector<std::string> &filaments, const std::string &nozzles = "Standard,Standard",
                              const std::string &map = "1,2")
{
    return bbl_config(H2D, filaments, nozzles, map);
}

// One 20 x 20 x 2 mm block per filament; every layer changes filament on each block.
std::string slice_bbl(Print &print, const DynamicPrintConfig &cfg, size_t filaments = 2)
{
    Model model;
    for (size_t i = 0; i < filaments; ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.1f));
        ModelObject *object = model.add_object();
        object->name        = "block" + std::to_string(i + 1);
        object->add_volume(cube);
        object->config.set("extruder", int(i + 1));
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(130. + 40. * double(i), 160., 0.));
        object->ensure_on_bed();
    }
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

std::string slice_h2d(Print &print, const DynamicPrintConfig &cfg) { return slice_bbl(print, cfg, 2); }

std::vector<std::string> gcode_lines(const std::string &gcode)
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

// The G-code lines matching `re` (first capture group), with the layer they are on
// ("; layer num/total_layer_count: N/M"; 0 before the first layer).
std::vector<std::pair<int, std::string>> lines_by_layer(const std::string &gcode, const std::regex &re)
{
    static const std::regex                  re_layer(R"(^; layer num/total_layer_count: (\d+)/)");
    std::vector<std::pair<int, std::string>> out;
    int                                      layer = 0;
    for (const std::string &line : gcode_lines(gcode)) {
        std::smatch m;
        if (std::regex_search(line, m, re_layer))
            layer = std::stoi(m[1].str());
        else if (std::regex_search(line, m, re))
            out.emplace_back(layer, m[1].str());
    }
    return out;
}

std::set<std::string> values_on(const std::vector<std::pair<int, std::string>> &lines, bool first_layer)
{
    std::set<std::string> out;
    for (const auto &[layer, value] : lines)
        if ((layer == 1) == first_layer && layer >= 1)
            out.insert(value);
    return out;
}

const std::regex re_m620_15(R"(^M620\.15 C(-?\d+(\.\d+)?)$)");
const std::regex re_reheat(R"(^M104 T(\d+) S(\d+) N0 ;Wipe tower reheat before wipe$)");

// The rule the H2D hardware test asked for. For every M620.15 C<c> below the print temperature P of the
// filament being loaded (the template's "M620.10 A1 ... P[new_filament_temp] S1" just before it), the
// tower's "M104 T<e> S<P> N0 ;Wipe tower reheat before wipe" follows, before the toolchange ends
// ("; CP TOOLCHANGE END") and before any printing move (X/Y with positive E). Every reheat answers such
// a cool-down (Bambu Studio writes it only after one).
struct ReheatCheck
{
    size_t                   cooled  = 0;
    size_t                   reheats = 0;
    std::vector<std::string> reheat_lines;
    std::vector<std::string> errors;
};

ReheatCheck check_reheats(const std::string &gcode)
{
    static const std::regex re_new_temp(R"(^M620\.10 A1 .* P(\d+) S1$)");
    static const std::regex re_print_move(R"(^G[123] [^;]*[XY]-?[\d.]+[^;]*E(\d*\.?\d+))");
    ReheatCheck out;
    int         new_temp = -1;
    int         pending  = -1; // print temperature still owed after a cool-down
    size_t      n        = 0;
    for (const std::string &line : gcode_lines(gcode)) {
        ++n;
        std::smatch m;
        if (std::regex_search(line, m, re_new_temp)) {
            new_temp = std::stoi(m[1].str());
        } else if (std::regex_search(line, m, re_m620_15)) {
            if (pending >= 0)
                out.errors.push_back("line " + std::to_string(n) + ": a second cool-down before the reheat");
            if (new_temp >= 0 && std::stod(m[1].str()) < double(new_temp)) {
                pending = new_temp;
                ++out.cooled;
            }
        } else if (std::regex_search(line, m, re_reheat)) {
            ++out.reheats;
            out.reheat_lines.push_back(line);
            if (pending < 0)
                out.errors.push_back("line " + std::to_string(n) + ": reheat without a cool-down: " + line);
            else if (std::stoi(m[2].str()) != pending)
                out.errors.push_back("line " + std::to_string(n) + ": reheat to " + m[2].str() + ", print temperature " + std::to_string(pending));
            pending = -1;
        } else if (pending >= 0 && line.rfind("; CP TOOLCHANGE END", 0) == 0) {
            out.errors.push_back("line " + std::to_string(n) + ": toolchange ended still cooled to below " + std::to_string(pending));
            pending = -1;
        } else if (pending >= 0 && std::regex_search(line, m, re_print_move) && std::stod(m[1].str()) > 0.) {
            out.errors.push_back("line " + std::to_string(n) + ": printing while cooled below " + std::to_string(pending) + ": " + line);
            pending = -1;
        }
    }
    if (pending >= 0)
        out.errors.push_back("end of file still cooled below " + std::to_string(pending));
    return out;
}

void require_reheats(const ReheatCheck &check)
{
    std::string errors;
    for (const std::string &e : check.errors)
        errors += e + "\n";
    INFO(errors);
    CHECK(check.errors.empty());
    CHECK(check.reheats == check.cooled);
}

} // namespace

TEST_CASE("BBL filament presets keep the Bambu flush keys", "[BambuFlushKeys][BBLProfiles]")
{
    SECTION("H2D PLA Basic: 10 degrees of pre-tower cooling in each extruder variant")
    {
        CHECK(floats_of(filament_preset("BBL", H2D_PLA), "filament_cooling_before_tower") == std::vector<double>{ 10., 10., 10. });
        CHECK(floats_of(filament_preset("BBL", H2D_PETG_HF), "filament_cooling_before_tower") == std::vector<double>{ 10., 10., 10. });
    }
    SECTION("H2D PETG Basic 0.2 nozzle: 3 mm3/s flush speed")
    {
        CHECK(floats_of(filament_preset("BBL", "Bambu PETG Basic @BBL H2D 0.2 nozzle"), "filament_flush_volumetric_speed") ==
              std::vector<double>{ 3., 3. });
    }
    SECTION("P2S PETG HF: a flush temperature per variant, 0 in the High Flow slot")
    {
        CHECK(ints_of(filament_preset("BBL", "Bambu PETG HF @BBL P2S 0.4 nozzle"), "filament_flush_temp") == std::vector<int>{ 240, 0, 240 });
    }
    SECTION("A2L PLA Basic: the Fast purge mode flush temperature")
    {
        CHECK(ints_of(filament_preset("BBL", "Bambu PLA Basic @BBL A2L 0.4 nozzle"), "filament_flush_temp_fast") == std::vector<int>{ 220 });
    }
    SECTION("X1C and A2L PLA Basic carry no cooling key: 0, as Bambu Studio's fdm_filament_common.json says")
    {
        for (const char *name : { "Bambu PLA Basic @BBL X1C", "Bambu PLA Basic @BBL A2L 0.4 nozzle" }) {
            INFO("preset: " << name);
            const DynamicPrintConfig &cfg     = filament_preset("BBL", name);
            const std::vector<double> cooling = floats_of(cfg, "filament_cooling_before_tower");
            REQUIRE_FALSE(cooling.empty());
            for (double c : cooling)
                CHECK(c == 0.);
        }
        const DynamicPrintConfig &x1c = filament_preset("BBL", "Bambu PLA Basic @BBL X1C");
        for (int t : ints_of(x1c, "filament_flush_temp"))
            CHECK(t == 0);
        for (int t : ints_of(x1c, "filament_flush_temp_fast"))
            CHECK(t == 0);
        for (double v : floats_of(x1c, "filament_flush_volumetric_speed"))
            CHECK(v == 0.);
    }
}

TEST_CASE("Anycubic filament presets load their nil flush keys", "[BambuFlushKeys]")
{
    const DynamicPrintConfig &abs = filament_preset("Anycubic", "Anycubic ABS @Anycubic Kobra 4 0.4 nozzle");
    const auto *temp  = abs.option<ConfigOptionIntsNullable>("filament_flush_temp");
    const auto *speed = abs.option<ConfigOptionFloatsNullable>("filament_flush_volumetric_speed");
    REQUIRE(temp != nullptr);
    REQUIRE(speed != nullptr);
    REQUIRE_FALSE(temp->values.empty());
    REQUIRE_FALSE(speed->values.empty());
    CHECK(temp->is_nil(0));
    CHECK(speed->is_nil(0));
}

TEST_CASE("H2D PLA + PETG HF: M620.15 cools 10 degrees before the tower, the tower heats back", "[BambuFlushKeys][BBLProfiles]")
{
    Print             print;
    const std::string gcode   = slice_h2d(print, h2d_config({ H2D_PLA, H2D_PETG_HF }));
    const auto        m620_15 = lines_by_layer(gcode, re_m620_15);
    REQUIRE(m620_15.size() >= 4);

    // First layer: Bambu Studio zeroes the cooling, so the target is the print temperature.
    const std::set<std::string> first = values_on(m620_15, true);
    CHECK_FALSE(first.empty());
    for (const std::string &c : first) {
        INFO("first-layer M620.15 C" << c);
        CHECK((c == "245" || c == "220"));
    }
    // Every later layer: 10 degrees below, C235 for the PETG HF and C210 for the PLA, as Bambu Studio writes.
    CHECK(values_on(m620_15, false) == std::set<std::string>{ "235", "210" });

    // Each of those cool-downs is answered by the tower's reheat to the print temperature, on the
    // physical extruder of the filament (H2D: left = 1, right = 0; filament 1 left, filament 2 right).
    const ReheatCheck check = check_reheats(gcode);
    require_reheats(check);
    CHECK(check.cooled >= 2);
    CHECK(std::set<std::string>(check.reheat_lines.begin(), check.reheat_lines.end()) ==
          std::set<std::string>{ "M104 T1 S220 N0 ;Wipe tower reheat before wipe", "M104 T0 S245 N0 ;Wipe tower reheat before wipe" });
    // None on the first layer.
    CHECK(values_on(lines_by_layer(gcode, re_reheat), true).empty());

    // The CONFIG_BLOCK records the presets' values, not dropped defaults.
    CHECK(gcode.find("\n; filament_cooling_before_tower = 10,10,10,10\n") != std::string::npos);
}

TEST_CASE("Without the tower, or by object without it, nothing cools before the tower", "[BambuFlushKeys][BBLProfiles]")
{
    // Bambu Studio publishes filament_cooling_before_tower as zeros for a toolchange that does not go
    // through its tower (GCode.cpp set_extruder), so M620.15 C is the print temperature and no reheat is due.
    for (const char *sequence : { "by layer", "by object" }) {
        INFO("print_sequence " << sequence);
        DynamicPrintConfig cfg = h2d_config({ H2D_PLA, H2D_PETG_HF });
        cfg.set_deserialize_strict({ { "enable_prime_tower", "0" }, { "print_sequence", sequence } });
        Print             print;
        const std::string gcode = slice_h2d(print, cfg);
        CHECK_FALSE(lines_by_layer(gcode, re_m620_15).empty());
        // cooled == 0: every M620.15 C is the print temperature of the filament being loaded.
        const ReheatCheck check = check_reheats(gcode);
        require_reheats(check);
        CHECK(check.cooled == 0);
        CHECK(check.reheats == 0);
    }
}

TEST_CASE("By object with the tower: every cool-down is reheated", "[BambuFlushKeys][BBLProfiles]")
{
    DynamicPrintConfig cfg = h2d_config({ H2D_PLA, H2D_PETG_HF });
    cfg.set_deserialize_strict({ { "print_sequence", "by object" } });
    Print             print;
    const std::string gcode = slice_h2d(print, cfg);
    require_reheats(check_reheats(gcode));
}

TEST_CASE("H2C, three filaments on two extruders: every cool-down is reheated", "[BambuFlushKeys][BBLProfiles]")
{
    DynamicPrintConfig cfg = bbl_config(H2C, { "Bambu PLA Basic @BBL H2C", "Bambu PETG HF @BBL H2C", "Bambu PLA Basic @BBL H2C" }, "", "1,2,2");
    Print             print;
    const std::string gcode = slice_bbl(print, cfg, 3);
    const ReheatCheck check = check_reheats(gcode);
    require_reheats(check);
    CHECK(check.cooled >= 2);
}

TEST_CASE("Nil flush keys read as their defaults in the G-code", "[BambuFlushKeys][BBLProfiles]")
{
    DynamicPrintConfig cfg   = h2d_config({ H2D_PLA, H2D_PETG_HF });
    const size_t       slots = cfg.option<ConfigOptionFloatsNullable>("filament_cooling_before_tower")->values.size();
    REQUIRE(slots >= 2);
    auto nil_ints   = new ConfigOptionIntsNullable(std::vector<int>(slots, ConfigOptionIntsNullable::nil_value()));
    auto nil_floats = new ConfigOptionFloatsNullable(std::vector<double>(slots, ConfigOptionFloatsNullable::nil_value()));
    cfg.set_key_value("filament_flush_temp", nil_ints);
    cfg.set_key_value("filament_flush_volumetric_speed", nil_floats);
    cfg.set_key_value("filament_cooling_before_tower", nil_floats->clone());
    Print             print;
    const std::string gcode = slice_h2d(print, cfg);

    // filament_cooling_before_tower nil -> 0: no cool-down, no reheat.
    CHECK(values_on(lines_by_layer(gcode, re_m620_15), false) == std::set<std::string>{ "245", "220" });
    CHECK(check_reheats(gcode).reheats == 0);
    // filament_flush_temp nil -> 0 -> the top of the recommended range (PLA 240, PETG HF 270).
    const std::set<std::string> flush_temps = values_on(lines_by_layer(gcode, std::regex(R"(^M620\.10 A1 .* T(\d+) P\d+ S1$)")), false);
    CHECK(flush_temps == std::set<std::string>{ "240", "270" });
    CHECK(gcode.find("Tnan") == std::string::npos);
    CHECK(gcode.find("Cnan") == std::string::npos);
    CHECK(gcode.find("Fnan") == std::string::npos);
}

TEST_CASE("The flush keys are read from each filament's flow-variant column", "[BambuFlushKeys][BBLProfiles]")
{
    // Left nozzle Standard, right nozzle High Flow: filament 0 slices its Standard column, filament 1
    // its High Flow column. The composed config packs [F0 Standard, F0 High Flow, F1 Standard, F1 High Flow].
    DynamicPrintConfig cfg = h2d_config({ H2D_PLA, H2D_PLA }, "Standard,High Flow", "1,2");
    REQUIRE(cfg.option<ConfigOptionInts>("filament_flow_step_size")->values == std::vector<int>{ 2, 2 });
    cfg.set_key_value("filament_cooling_before_tower", new ConfigOptionFloatsNullable({ 4., 6., 8., 12. }));
    cfg.set_key_value("filament_flush_temp", new ConfigOptionIntsNullable({ 0, 211, 222, 231 }));
    Print             print;
    const std::string gcode = slice_h2d(print, cfg);
    REQUIRE(print.config().filament_volume_type.values == std::vector<int>{ fvtStandard, fvtHighFlow });

    // To filament 0 (Standard column, 4): C216. To filament 1 (High Flow column, 12): C208.
    CHECK(values_on(lines_by_layer(gcode, re_m620_15), false) == std::set<std::string>{ "216", "208" });
    // Flush temperature of the filament loaded next: filament 0's Standard slot says 0 (-> 240),
    // filament 1's High Flow slot says 231.
    const std::set<std::string> flush_temps = values_on(lines_by_layer(gcode, std::regex(R"(^M620\.10 A1 .* T(\d+) P\d+ S1$)")), false);
    CHECK(flush_temps == std::set<std::string>{ "240", "231" });
    // The tower reheats from the same columns.
    require_reheats(check_reheats(gcode));
}
