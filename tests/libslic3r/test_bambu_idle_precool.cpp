#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/PreCoolingInjector.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

// Bambu Studio lets the idle nozzle of a two-extruder printer (H2D, H2D Pro, H2C, X2D) cool down
// between uses and heats it back just in time (GCodeProcessor::PreCoolingInjector):
//     M400
//     M104 T<hotend> S<t> N0 C ;Multi extruder pre cooling    right after its last use
//     M632 S<filament> [N R] W
//     M104 T<hotend> S<t> N0 W ;Multi extruder pre heating    timed to finish before its next use
//     M633
// T is the PHYSICAL hotend (physical_extruder_map; right = 0 on H2D/H2C). Only printers whose profile
// sets enable_pre_heating get these lines; every other printer's G-code must stay as it was.

namespace {

PresetBundle &precool_bundle(const std::string &vendor)
{
    static std::map<std::string, std::unique_ptr<PresetBundle>> bundles;
    static std::unique_ptr<PresetBundle>                        library;
    auto it = bundles.find(vendor);
    if (it != bundles.end())
        return *it->second;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("precool_%%%%-%%%%");
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

struct Machine
{
    const char *vendor;
    const char *printer;
    const char *process;
    const char *filament;
    Vec2d       bed_center;
    Vec2d       tower;
    bool        bbl;
};

const Machine H2D{ "BBL", "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D", { 165., 160. }, { 40., 250. }, true };
const Machine H2C{ "BBL", "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C", { 165., 160. }, { 40., 250. }, true };
const Machine X1C{ "BBL", "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C", { 128., 128. }, { 30., 200. }, true };
const Machine U1{ "Snapmaker", "Snapmaker U1 (0.4 nozzle)", "0.20mm Standard @Snapmaker U1 (0.4 nozzle)", "Generic PLA @U1 0.4 nozzle", { 135., 135. }, { 30., 210. }, false };

constexpr int NUM_FILAMENTS = 3;

DynamicPrintConfig machine_config(const Machine &m, std::initializer_list<ConfigBase::SetDeserializeItem> overrides = {})
{
    PresetBundle &b = precool_bundle(m.vendor);
    REQUIRE(b.printers.select_preset_by_name(m.printer, true));
    REQUIRE(b.prints.select_preset_by_name(m.process, true));
    REQUIRE(b.filaments.select_preset_by_name(m.filament, true));
    b.filament_presets = { m.filament };
    b.set_num_filaments(NUM_FILAMENTS, std::vector<std::string>{ "#E01919", "#1943E0", "#19E043" });
    b.filament_presets = std::vector<std::string>(NUM_FILAMENTS, m.filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "enable_prime_tower", "1" },
        { "wipe_tower_x", m.tower.x() },
        { "wipe_tower_y", m.tower.y() },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments", 0 },
    });
    cfg.set_deserialize_strict(overrides);
    return cfg;
}

// Three 20 x 20 x 6 mm cubes side by side, one per filament: every layer uses all three filaments.
void add_plate(Model &model, const Machine &m, double spacing = 25.)
{
    for (int i = 0; i < NUM_FILAMENTS; ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.3f));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("extruder", i + 1);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(m.bed_center.x() + spacing * (i - 1), m.bed_center.y(), 0.));
        object->ensure_on_bed();
    }
}

std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg, const Machine &m, double spacing = 25.)
{
    add_plate(model, m, spacing);
    print.is_BBL_printer() = m.bbl;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

std::vector<std::string> body_lines(const std::string &gcode)
{
    static const std::string begin_tag = "; CONFIG_BLOCK_START";
    static const std::string end_tag   = "; CONFIG_BLOCK_END";
    std::string body = gcode;
    for (size_t b; (b = body.find(begin_tag)) != std::string::npos;) {
        const size_t e = body.find(end_tag, b);
        body.erase(b, e == std::string::npos ? std::string::npos : e + end_tag.size() - b);
    }
    std::vector<std::string> out;
    std::istringstream       ss(body);
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(line);
    }
    return out;
}

std::string config_value(const std::string &gcode, const std::string &key)
{
    const std::string tag = "\n; " + key + " = ";
    const size_t      pos = gcode.find(tag);
    if (pos == std::string::npos)
        return "<missing>";
    const size_t start = pos + tag.size();
    std::string  value = gcode.substr(start, gcode.find('\n', start) - start);
    if (!value.empty() && value.back() == '\r')
        value.pop_back();
    return value;
}

std::vector<int> config_ints(const std::string &gcode, const std::string &key)
{
    ConfigOptionInts opt;
    opt.deserialize(config_value(gcode, key));
    return opt.values;
}

size_t count_lines(const std::string &gcode, const std::regex &re)
{
    size_t n = 0;
    for (const std::string &line : body_lines(gcode))
        if (std::regex_search(line, re))
            ++n;
    return n;
}

// Bambu Studio ef8222a6c: C marks a cooling line, W a heating one.
const std::regex re_precool(R"(^M104 T(\d+) S(\d+) N0 C ;Multi extruder pre cooling$)");
const std::regex re_preheat(R"(^M104 T(\d+) S(\d+) N0 W ;Multi extruder pre heating$)");
const std::regex re_any_injected(R"(;Multi extruder pre (cooling|heating))");
const std::regex re_m632(R"(^M632 S(\d+)( N R)? W$)");
const std::regex re_tool(R"(^T(\d+)( |$))");
const std::regex re_extrude(R"(^G[123] .*[XY][-\d.]+.* E(\d*\.?\d+))");
// Any other nozzle temperature command naming a hotend: "M104 S<t> T<h>" or "M104 T<h> S<t>".
const std::regex re_set_temp_st(R"(^M10[49] [^;]*S(\d+)[^;]*T(\d+))");
const std::regex re_set_temp_ts(R"(^M10[49] [^;]*T(\d+)[^;]*S(\d+))");

struct Walk
{
    size_t precools = 0, preheats = 0;
    std::vector<std::string> errors;
    std::map<int, std::string> m632_flags; // physical hotend -> " N R" or ""
};

// Replays the G-code body and checks what the injected lines promise:
// - every pre-cool is "M400" + "M104 T<hotend> S<t> N0 C", every pre-heat is "M632 S<f> [N R] W" +
//   "M104 T<hotend> S<t> N0 W" + "M633", with T a physical hotend (0/1);
// - no extrusion is printed from a hotend between its pre-cool and its pre-heat;
// - a pre-heat names the filament that hotend loads next, on that filament's hotend, at that
//   filament's nozzle temperature (minus filament_preheat_temperature_delta at most).
Walk walk(const std::string &gcode)
{
    Walk                   w;
    const std::vector<int> fmap  = config_ints(gcode, "filament_map");
    const std::vector<int> pem   = config_ints(gcode, "physical_extruder_map");
    const std::vector<int> temps = config_ints(gcode, "nozzle_temperature");
    auto hotend_of = [&](int filament) -> int {
        if (filament < 0 || size_t(filament) >= fmap.size())
            return -1;
        const int logical = fmap[size_t(filament)] - 1;
        return logical >= 0 && size_t(logical) < pem.size() ? pem[size_t(logical)] : logical;
    };
    const std::vector<std::string> lines = body_lines(gcode);
    bool               started = false;
    int                loaded  = -1;
    std::map<int, bool> cooled;
    std::map<int, int>  heated_for; // hotend -> filament its pre-heat was for
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        std::smatch        m;
        if (line == "; MACHINE_START_GCODE_END")
            started = true;
        if (line == "; MACHINE_END_GCODE_START")
            break;
        if (std::regex_search(line, m, re_precool)) {
            ++w.precools;
            const int h = std::stoi(m[1].str());
            if (h != 0 && h != 1)
                w.errors.push_back("pre-cool names hotend " + m[1].str() + ": " + line);
            if (i == 0 || lines[i - 1] != "M400")
                w.errors.push_back("pre-cool without M400 before it, line " + std::to_string(i));
            if (std::stoi(m[2].str()) < 25)
                w.errors.push_back("pre-cool below room temperature: " + line);
            cooled[h] = true;
            heated_for.erase(h);
            continue;
        }
        if (std::regex_search(line, m, re_preheat)) {
            ++w.preheats;
            const int h = std::stoi(m[1].str());
            std::smatch m2;
            if (i == 0 || !std::regex_search(lines[i - 1], m2, re_m632)) {
                w.errors.push_back("pre-heat without M632 before it, line " + std::to_string(i));
                continue;
            }
            if (i + 1 >= lines.size() || lines[i + 1] != "M633")
                w.errors.push_back("pre-heat without M633 after it, line " + std::to_string(i));
            const int f = std::stoi(m2[1].str());
            w.m632_flags[h] = m2[2].str();
            if (hotend_of(f) != h)
                w.errors.push_back("pre-heat of hotend " + std::to_string(h) + " for filament " + std::to_string(f) + " which prints on hotend " +
                                   std::to_string(hotend_of(f)));
            const int t = std::stoi(m[2].str());
            if (f >= 0 && size_t(f) < temps.size() && (t > temps[size_t(f)] || t < temps[size_t(f)] - 30))
                w.errors.push_back("pre-heat to " + std::to_string(t) + " for a filament printed at " + std::to_string(temps[size_t(f)]));
            cooled[h]     = false;
            heated_for[h] = f;
            continue;
        }
        if (std::regex_search(line, m, re_tool)) {
            const int t = std::stoi(m[1].str());
            if (t >= 255)
                continue;
            loaded = t;
            if (started) {
                const int h = hotend_of(t);
                if (cooled[h])
                    w.errors.push_back("filament " + std::to_string(t) + " loaded into hotend " + std::to_string(h) + " while it is pre-cooled, line " +
                                       std::to_string(i));
                auto it = heated_for.find(h);
                if (it != heated_for.end()) {
                    if (it->second != t)
                        w.errors.push_back("hotend " + std::to_string(h) + " pre-heated for filament " + std::to_string(it->second) + " but loads " +
                                           std::to_string(t));
                    heated_for.erase(it);
                }
            }
            continue;
        }
        if (started) {
            // A pre-cooled hotend is heated only by its own pre-heat: an object-start or 2nd-layer
            // "M104 S<print temperature> T<hotend>" on it undoes the pre-cool (owner's H2C, 2026-09-24).
            int temp = -1, tool = -1;
            if (std::regex_search(line, m, re_set_temp_st)) {
                temp = std::stoi(m[1].str());
                tool = std::stoi(m[2].str());
            } else if (std::regex_search(line, m, re_set_temp_ts)) {
                tool = std::stoi(m[1].str());
                temp = std::stoi(m[2].str());
            }
            if (tool >= 0 && temp > 0 && cooled[tool]) {
                w.errors.push_back("pre-cooled hotend " + std::to_string(tool) + " set to " + std::to_string(temp) + " before its pre-heat, line " +
                                   std::to_string(i) + ": " + line);
                continue;
            }
        }
        if (started && loaded >= 0 && std::regex_search(line, m, re_extrude) && std::stod(m[1].str()) > 0.) {
            const int h = hotend_of(loaded);
            if (cooled[h])
                w.errors.push_back("extrusion from pre-cooled hotend " + std::to_string(h) + ", line " + std::to_string(i) + ": " + line);
        }
    }
    return w;
}

void check_walk(const Walk &w)
{
    for (const std::string &e : w.errors) {
        CAPTURE(e);
        CHECK(false);
    }
    CHECK(w.errors.empty());
}

} // namespace

SCENARIO("H2D: the idle hotend is pre-cooled and pre-heated in time", "[BambuPreCool][slow]")
{
    for (const bool tower : { true, false }) {
        GIVEN(std::string("filament 1 on the left, 2 and 3 on the right, by layer, prime tower ") + (tower ? "on" : "off"))
        {
            const DynamicPrintConfig cfg = machine_config(H2D, {
                { "filament_map_mode", "Manual" },
                { "filament_map", "1,2,2" },
                { "enable_prime_tower", tower ? "1" : "0" },
            });
            Print print;
            Model model;
            const std::string gcode = slice(print, model, cfg, H2D);
            REQUIRE(PreCooling::pre_cooling_active(print));

            THEN("the markers, the pre-cool and pre-heat lines are written and describe the plate")
            {
                CHECK(count_lines(gcode, std::regex("^; MACHINE_START_GCODE_END$")) == 1);
                CHECK(count_lines(gcode, std::regex("^; MACHINE_END_GCODE_START$")) == 1);
                const Walk w = walk(gcode);
                check_walk(w);
                CHECK(w.precools > 0);
                CHECK(w.preheats > 0);
                // The H2D has one nozzle per extruder: no "N R" (nozzle re-select) on the M632.
                for (const auto &[hotend, flags] : w.m632_flags)
                    CHECK(flags.empty());
            }
            THEN("with a tower, each extruder change is marked for the processor, as Bambu Studio's tower does")
            {
                const size_t markers = count_lines(gcode, std::regex(R"(^; NOZZLE_CHANGE_END OF\d+ NF\d+ ON\d+ NN\d+$)"));
                if (tower)
                    CHECK(markers > 0);
                else
                    CHECK(markers == 0);
            }
            THEN("the config block carries the pre-heating keys, as Bambu Studio's does")
            {
                CHECK(config_value(gcode, "enable_pre_heating") == "1");
                CHECK(config_value(gcode, "filament_pre_cooling_temperature") != "<missing>");
                CHECK(config_value(gcode, "filament_preheat_temperature_delta") != "<missing>");
            }
        }
    }
}

SCENARIO("H2C: the rack extruder's pre-heat re-selects its nozzle", "[BambuPreCool]")
{
    GIVEN("filament 1 on the left extruder, 2 and 3 on the right (rack) extruder")
    {
        const DynamicPrintConfig cfg = machine_config(H2C, {
            { "filament_map_mode", "Manual" },
            { "filament_map", "1,2,2" },
        });
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, H2C);
        REQUIRE(PreCooling::pre_cooling_active(print));
        const Walk w = walk(gcode);
        check_walk(w);
        CHECK(w.precools > 0);
        CHECK(w.preheats > 0);
        // Right extruder = logical 1 = physical 0 carries the rack: "M632 S<f> N R W". The left one does not.
        const std::vector<int> pem = config_ints(gcode, "physical_extruder_map");
        REQUIRE(pem.size() == 2);
        for (const auto &[hotend, flags] : w.m632_flags)
            CHECK(flags == (hotend == pem[1] ? " N R" : ""));
        // #120's hotend-change parking values are still written by the change_filament template.
        CHECK(count_lines(gcode, std::regex(R"(^M620\.15 P\d+)")) > 0);
    }
}

// PR 1165: the Bambu firmware flush (";VG1" moves and "SYNC T" waits between "; VFLUSH_START" and "; VFLUSH_END") counts in
// the estimate. Booked as planner waits it moved the pre-cooling and pre-heating lines on every plate with a flush (the
// Windows lead's check: 8 lines on an H2D, 339 M104 on an H3C); the owner asked for the G-code to stay as it was. Slicing the
// same plate with and without that accounting must give the same G-code apart from the estimate lines.
TEST_CASE("The firmware flush time counts in the estimate and moves no G-code line", "[BambuPreCool][VFlush]")
{
    const std::regex re_estimate(R"(^(M73 |; model printing time|; estimated first layer|; filament used \[|; total filament|; filament cost|; generated by))");
    // Object ids (label ids, M624 skip masks) count up from one Print to the next: blank them out.
    const std::regex re_ids(R"((label id: |^M62[45] )[^ ;]+)");
    auto without_estimates = [&](const std::string &gcode) {
        std::vector<std::string> out;
        for (const std::string &line : body_lines(gcode))
            if (!std::regex_search(line, re_estimate))
                out.push_back(std::regex_replace(line, re_ids, "$1#"));
        return out;
    };
    auto header_time = [](const std::string &gcode) {
        for (const std::string &line : body_lines(gcode))
            if (line.rfind("; model printing time", 0) == 0)
                return line;
        return std::string("<missing>");
    };
    // Filaments 2 and 3 share the right nozzle: changes between them are flushed by the firmware.
    for (const char *map : { "1,2,2", "2,2,2" }) {
        DYNAMIC_SECTION("H2D, filament_map " << map)
        {
            const DynamicPrintConfig cfg = machine_config(H2D, { { "filament_map_mode", "Manual" }, { "filament_map", map } });
            const bool was = GCodeProcessor::s_EstimateVirtualFlush;
            std::string with, without;
            {
                Print print;
                Model model;
                GCodeProcessor::s_EstimateVirtualFlush = true;
                with = slice(print, model, cfg, H2D);
            }
            {
                Print print;
                Model model;
                GCodeProcessor::s_EstimateVirtualFlush = false;
                without = slice(print, model, cfg, H2D);
            }
            GCodeProcessor::s_EstimateVirtualFlush = was;
            // The plate has a firmware flush and, on the 1,2,2 map, pre-cooling lines.
            CHECK(count_lines(with, std::regex(R"(^;VG1 E[1-9])")) > 0);
            if (std::string(map) == "1,2,2")
                CHECK(count_lines(with, re_any_injected) > 0);
            // The estimate has the flush ...
            CHECK(header_time(with) != header_time(without));
            // ... and nothing else differs.
            const std::vector<std::string> a = without_estimates(with);
            const std::vector<std::string> b = without_estimates(without);
            CHECK(a.size() == b.size());
            size_t differing = 0;
            for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
                if (a[i] != b[i]) {
                    if (differing++ < 5)
                        UNSCOPED_INFO("line " << i << ": " << b[i] << " -> " << a[i]);
                }
            CHECK(differing == 0);
        }
    }
}

TEST_CASE("Printers without pre-heating keep their G-code", "[BambuPreCool]")
{
    const std::regex re_marker("^; MACHINE_(START_GCODE_END|END_GCODE_START)$");
    auto check_untouched = [&](const std::string &gcode) {
        CHECK(count_lines(gcode, re_any_injected) == 0);
        CHECK(count_lines(gcode, re_marker) == 0);
        CHECK(config_value(gcode, "enable_pre_heating") == "<missing>");
        CHECK(config_value(gcode, "filament_pre_cooling_temperature") == "<missing>");
        CHECK(config_value(gcode, "filament_preheat_temperature_delta") == "<missing>");
    };

    SECTION("H2D with enable_pre_heating off")
    {
        const DynamicPrintConfig cfg = machine_config(H2D, { { "filament_map_mode", "Manual" }, { "filament_map", "1,2,2" }, { "enable_pre_heating", "0" } });
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, H2D);
        CHECK_FALSE(PreCooling::pre_cooling_active(print));
        check_untouched(gcode);
        // Bambu Studio 2.9.1 rams the outgoing filament on the tower at every extruder change whether or not the
        // printer pre-heats (WipeTower::ramming, tests/design_bbs_tower_port.md), so the markers stay; only the
        // pre-cooling lines inside them are gone.
        CHECK(count_lines(gcode, std::regex("^; NOZZLE_CHANGE_START")) > 0);
        CHECK(count_lines(gcode, std::regex("^; NOZZLE_CHANGE_START")) == count_lines(gcode, std::regex("^; NOZZLE_CHANGE_END")));
    }
    SECTION("H2D with every filament on one extruder: nothing is idle")
    {
        const DynamicPrintConfig cfg = machine_config(H2D, { { "filament_map_mode", "Manual" }, { "filament_map", "2,2,2" } });
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, H2D);
        CHECK(count_lines(gcode, re_any_injected) == 0);
    }
    SECTION("X1C, a single-extruder Bambu printer with three filaments")
    {
        const DynamicPrintConfig cfg = machine_config(X1C);
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, X1C);
        CHECK_FALSE(PreCooling::pre_cooling_active(print));
        check_untouched(gcode);
    }
    SECTION("Snapmaker U1, a non-Bambu tool changer whose filament profiles carry filament_pre_cooling_temperature")
    {
        const DynamicPrintConfig cfg = machine_config(U1);
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, U1);
        CHECK_FALSE(PreCooling::pre_cooling_active(print));
        check_untouched(gcode);
    }
}

TEST_CASE("Print by object: the idle hotend stays cool while the other one prints", "[BambuPreCool]")
{
    // A by-object plate is grouped only once PR #138 (ToolOrdering::group_by_plate_map) is in; before
    // that pre_cooling_active() is false for it and nothing is injected. Either way the invariants hold:
    // no pre-cooled hotend prints, loads, or is set to a temperature by anything but its own pre-heat
    // (the object-start and 2nd-layer temperature lines used to heat both hotends: owner's H2C,
    // 2026-09-24). Filaments right, left, right, so every object changes hotend; a first layer hotter
    // than the rest so the 2nd-layer lines are written too.
    for (const Machine *m : { &H2D, &H2C }) {
        for (const bool tower : { false, true }) {
            CAPTURE(m->printer, tower);
            const DynamicPrintConfig cfg = machine_config(*m, {
                { "print_sequence", "by object" },
                { "filament_map_mode", "Manual" },
                { "filament_map", "2,1,2" },
                { "enable_prime_tower", tower ? "1" : "0" },
                { "nozzle_temperature_initial_layer", "230,230,230" },
                { "nozzle_temperature", "220,220,220" },
            });
            Print print;
            Model model;
            const std::string gcode = slice(print, model, cfg, *m, 115.);
            REQUIRE(count_lines(gcode, std::regex(R"(^T[012]( |$))")) >= 3);
            const Walk w = walk(gcode);
            check_walk(w);
            if (PreCooling::pre_cooling_active(print)) {
                // Two hotend changes: each outgoing hotend is cooled once it is done.
                CHECK(w.precools >= 2);
            } else {
                CHECK(count_lines(gcode, re_any_injected) == 0);
            }
        }
    }
}

TEST_CASE("An H2D project saved before enable_pre_heating existed pre-heats once opened", "[BambuPreCool]")
{
    // Opening a project splits its config into presets (PresetBundle::load_config_model, as
    // Plater::load_files does); a printer key the project does not mention takes the system preset's
    // value, so projects saved by an older EdgeSlicer get the H2D's enable_pre_heating = 1.
    DynamicPrintConfig cfg = machine_config(H2D);
    REQUIRE(cfg.opt_bool("enable_pre_heating"));
    cfg.erase("enable_pre_heating");
    cfg.erase("filament_pre_cooling_temperature");
    cfg.erase("filament_preheat_temperature_delta");

    Model model;
    add_plate(model, H2D);
    const boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("precool_old_%%%%-%%%%.3mf");
    {
        PlateData *plate   = new PlateData();
        plate->plate_index = 0;
        for (int i = 0; i < NUM_FILAMENTS; ++i)
            plate->objects_and_instances.emplace_back(i, 0);
        StoreParams       sp;
        const std::string path_str = path.string();
        sp.path            = path_str.c_str();
        sp.model           = &model;
        sp.config          = &cfg;
        sp.plate_data_list = { plate };
        sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
        REQUIRE(store_bbs_3mf(sp));
        release_PlateData_list(sp.plate_data_list);
    }

    DynamicPrintConfig        loaded;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    En3mfType                 type = En3mfType::From_BBS;
    PlateDataPtrs             plates;
    std::vector<Preset *>     project_presets;
    Semver                    version;
    Model                     reloaded = Model::read_from_archive(path.string(), &loaded, &substitutions, type,
                                                                  LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence, &plates,
                                                                  &project_presets, &version);
    release_PlateData_list(plates);
    boost::filesystem::remove(path);
    REQUIRE(loaded.option("printer_settings_id") != nullptr);
    CHECK(loaded.option("enable_pre_heating") == nullptr);

    // A bundle of its own: loading a project adds presets to it.
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("precool_load_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    PresetBundle library;
    library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
    set_data_dir(saved_data_dir);

    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config += std::move(loaded);
    bundle.load_config_model(path.string(), std::move(config), version);
    CHECK(bundle.printers.get_edited_preset().config.opt_bool("enable_pre_heating"));
    CHECK(bundle.full_config_secure().opt_bool("enable_pre_heating"));
}

// The H2C keeps hotend_cooling_rate / hotend_heating_rate per extruder variant (1.6,1.6,1.6,3.4,3.4 / 3.5,3.5,3.5,13.3,13.3).
// The change_filament_gcode's "compensate for heating and cooling" divides one temperature difference by
// hotend_cooling_rate[filament_map[next]-1] and then by hotend_heating_rate[...]: the two SYNC T values of a change are in the
// ratio heating / cooling rate of the extruder the next filament is on. Before, a right-extruder change read index 1 of the
// variant list (the left extruder's High Flow entry): 3.5 / 1.6 instead of 13.3 / 3.4.
TEST_CASE("H2C: the change template's temperature compensation uses the rates of the extruder it changes on", "[BambuPreCool][HotendRates]")
{
    const std::regex re_sync(R"(^SYNC T(-?[\d.]+)$)");
    auto ratios = [&](const std::string &gcode) {
        std::vector<double> out;
        const std::vector<std::string> lines = body_lines(gcode);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i] != "; compensate for heating and cooling")
                continue;
            // The next two SYNC lines (blank lines between them), when the flush was not 0.
            std::vector<std::string> next;
            for (size_t j = i + 1; j < lines.size() && j < i + 6 && next.size() < 2; ++j)
                if (lines[j].rfind("SYNC ", 0) == 0)
                    next.push_back(lines[j]);
            std::smatch a, b;
            if (next.size() == 2 && std::regex_match(next[0], a, re_sync) && std::regex_match(next[1], b, re_sync)) {
                const double t1 = std::stod(a[1].str()), t2 = std::stod(b[1].str());
                if (std::abs(t2) > 1e-6 && std::abs(t1) > 1e-6)
                    out.push_back(t1 / t2);
            }
        }
        return out;
    };
    for (const auto &[map, ratio] : std::vector<std::pair<const char *, double>>{ { "2,2,2", 13.3 / 3.4 }, { "1,1,1", 3.5 / 1.6 } }) {
        DYNAMIC_SECTION("filament_map " << map)
        {
            const DynamicPrintConfig cfg = machine_config(H2C, { { "filament_map_mode", "Manual" }, { "filament_map", map } });
            REQUIRE(cfg.option<ConfigOptionFloats>("hotend_cooling_rate")->values.size() == 5);
            Print print;
            Model model;
            const std::string gcode = slice(print, model, cfg, H2C);
            const std::vector<double> r = ratios(gcode);
            REQUIRE_FALSE(r.empty());
            for (double v : r)
                CHECK_THAT(v, Catch::Matchers::WithinRel(ratio, 1e-3));
            // The config block keeps the variant list.
            CHECK(config_value(gcode, "hotend_cooling_rate") == "1.6,1.6,1.6,3.4,3.4");
        }
    }
}

// The prime tower's ramming on an extruder change waits for the outgoing nozzle to reach filament_pre_cooling_temperature,
// timed by hotend_cooling_rate (WipeTower). The H2C keeps that rate per extruder variant (1.6,1.6,1.6,3.4,3.4); the tower
// read it by extruder, so the right extruder (index 1) got the left extruder's High Flow slot. Only each extruder's current
// variant may matter: changing the slots no extruder uses must leave the G-code as it is.
TEST_CASE("H2C: the tower's pre-cool ramming reads the cooling rate of the outgoing extruder's variant", "[BambuPreCool][HotendRates][Tower]")
{
    auto sliced = [](const char *cooling) {
        const DynamicPrintConfig cfg = machine_config(H2C, { { "filament_map_mode", "Manual" },
                                                             { "filament_map", "2,1,2" },
                                                             { "filament_pre_cooling_temperature", "200,200,200" },
                                                             { "hotend_cooling_rate", cooling } });
        Print print;
        Model model;
        return slice(print, model, cfg, H2C);
    };
    const std::string stock  = sliced("1.6,1.6,1.6,3.4,3.4");
    // The left High Flow / E3D High Flow slots: no extruder of this plate prints with them (both are Standard).
    const std::string unused = sliced("1.6,9.9,9.9,3.4,3.4");
    // The plate has extruder changes with a ramming on the tower.
    REQUIRE(count_lines(stock, std::regex("^; NOZZLE_CHANGE_START")) > 0);
    const std::vector<std::string> a = body_lines(stock);
    const std::vector<std::string> b = body_lines(unused);
    CHECK(a.size() == b.size());
    size_t differing = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        if (a[i] != b[i] && a[i].find("label id") == std::string::npos && a[i].rfind("M62", 0) != 0) {
            if (differing++ < 5)
                UNSCOPED_INFO("line " << i << ": " << a[i] << " -> " << b[i]);
        }
    CHECK(differing == 0);
    // And the right extruder's own slot does matter.
    const std::string slower = sliced("1.6,1.6,1.6,1.0,1.0");
    CHECK(body_lines(slower) != a);
}
