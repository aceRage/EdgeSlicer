#include <catch2/catch.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
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
//     M104 T<hotend> S<t> N0 ;Multi extruder pre cooling      right after its last use
//     M632 S<filament> [N R] W
//     M104 T<hotend> S<t> N0 ;Multi extruder pre heating      timed to finish before its next use
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

const std::regex re_precool(R"(^M104 T(\d+) S(\d+) N0 ;Multi extruder pre cooling$)");
const std::regex re_preheat(R"(^M104 T(\d+) S(\d+) N0 ;Multi extruder pre heating$)");
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
// - every pre-cool is "M400" + "M104 T<hotend> S<t> N0", every pre-heat is "M632 S<f> [N R] W" +
//   "M104 T<hotend> S<t> N0" + "M633", with T a physical hotend (0/1);
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

SCENARIO("H2D: the idle hotend is pre-cooled and pre-heated in time", "[BambuPreCool]")
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
        CHECK(count_lines(gcode, std::regex("^; NOZZLE_CHANGE_(START|END)")) == 0);
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
