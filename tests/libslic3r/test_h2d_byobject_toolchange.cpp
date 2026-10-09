#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

// A print-by-object plate on a Bambu H2D froze the printer (2026-09-24, tests/h2d_stuckprint.gcode.3mf,
// plate 4). Every command the fork sent that names a filament, a hotend or a tray was compared with
// Bambu Studio 2.8.2's output for the same plate; these cases pin each place where they differed:
//
// 1. "M104 S<t> T<filament index>" at each object transition ("M104 S220 T6"). On a Bambu two-extruder
//    printer M104 T is a PHYSICAL hotend (0/1): the start G-code's "T{filament_map[..] % 2}", the end
//    G-code's T0/T1. Every M104/M109 T must be 0 or 1.
// 2. A change back into a nozzle that still holds another filament ("left -> right" after the right
//    nozzle printed T6, now loading T4) told the firmware to flush nothing ("M620.10 ... L0"). Bambu
//    Studio flushes from the filament parked in that nozzle (switch_to_nozzle, GCode.cpp:8180).
// 3. change_filament_gcode ran once more for the filament the start G-code had just loaded, telling the
//    firmware the hotend held filament 0 ("M620.11 ... I0", old temperature "M620.10 A0 ... P0").
//    Bambu Studio adopts the start G-code's filament instead.
// 4. Without a grouping result (print by object, several objects) the start G-code's
//    "M104 T{filament_map[..] % 2}" / "G151 P{..} M" named the LEFT hotend for a filament loaded into
//    the right one, and "M620.17 T<hotend> L<filament>" named the first filament for both hotends.

namespace {

PresetBundle &bbl_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    static std::unique_ptr<PresetBundle> library;
    if (bundle)
        return *bundle;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("h2d_byobj_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    library = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

constexpr int NUM_FILAMENTS = 3;

// Filament 1 on the right hotend, 2 on the left, 3 on the right again (filament_map is 1-based:
// 1 = left, 2 = right; the H2D's physical_extruder_map [1,0] makes the right hotend physical 0).
DynamicPrintConfig h2d_config(const std::string &sequence, bool tower)
{
    PresetBundle &b = bbl_bundle();
    const std::string filament = "Bambu PLA Basic @BBL H2D";
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H2D 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2D", true));
    REQUIRE(b.filaments.select_preset_by_name(filament, true));
    b.filament_presets = { filament };
    b.set_num_filaments(NUM_FILAMENTS, std::vector<std::string>{ "#E01919", "#1943E0", "#19E043" });
    b.filament_presets = std::vector<std::string>(NUM_FILAMENTS, filament);
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "print_sequence", sequence },
        { "enable_prime_tower", tower ? "1" : "0" },
        { "wipe_tower_x", 40. },
        { "wipe_tower_y", 250. },
        { "wipe_tower_rotation_angle", 0 },
        { "filament_map_mode", "Manual" },
        { "filament_map", "2,1,2" },
        { "gcode_comments", 0 },
        // Distinct flush volumes so every pair is recognisable in M620.10 ... L<length>.
        { "flush_volumes_matrix", "0,101,102,110,0,112,120,121,0" },
        { "flush_multiplier", 1 },
    });
    return cfg;
}

// Three 10 x 10 x 3 mm cubes far apart (print by object needs the extruder clearance between them),
// printed with filaments 1, 2, 3 in that order.
void add_objects(Model &model, const std::vector<int> &filaments)
{
    const double xs[] = { 60., 175., 290. };
    for (size_t i = 0; i < filaments.size(); ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(0.5f, 0.5f, 0.15f));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("extruder", filaments[i]);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(xs[i], 160., 0.));
        object->ensure_on_bed();
    }
}

std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg)
{
    print.is_BBL_printer() = true;
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

// One change_filament_gcode block (from "M620 S<f>A" to the template's "M620.15 C", which the start
// G-code does not contain), reduced to what the firmware is told.
struct Change
{
    int    loaded_before = -1; // filament in the active hotend before the block (from the G-code)
    int    next          = -1; // M620 S<next>A
    int    current_i     = -1; // M620.11 P.. I<current>
    int    old_temp      = -1; // M620.10 A0 ... P<old temperature>
    double flush         = -1; // M620.10 A1 ... L<flush length>
};

std::vector<Change> collect_changes(const std::vector<std::string> &lines)
{
    static const std::regex re_select(R"(^M620 S(\d+)A)");
    static const std::regex re_t(R"(^T(\d+)( |$))");
    static const std::regex re_a0(R"(^M620\.10 A0 .*P(\d+))");
    static const std::regex re_a1_len(R"(^M620\.10 A1 .*L([0-9.]+))");
    static const std::regex re_i(R"(^M620\.11 P\d I(\d+))");
    std::vector<Change> out;
    int                 loaded = -1;
    Change              pending;
    bool                in_block = false;
    for (const std::string &line : lines) {
        std::smatch m;
        if (std::regex_search(line, m, re_select)) {
            pending          = Change();
            pending.next     = std::stoi(m[1].str());
            pending.loaded_before = loaded;
            in_block         = true;
        } else if (in_block && std::regex_search(line, m, re_a0)) {
            pending.old_temp = std::stoi(m[1].str());
        } else if (in_block && std::regex_search(line, m, re_a1_len)) {
            pending.flush = std::stod(m[1].str());
        } else if (in_block && std::regex_search(line, m, re_i)) {
            pending.current_i = std::stoi(m[1].str());
        } else if (in_block && line.rfind("M620.15 C", 0) == 0) {
            out.push_back(pending);
            in_block = false;
        }
        if (std::regex_search(line, m, re_t)) {
            const int t = std::stoi(m[1].str());
            if (t < 255)
                loaded = t;
        }
    }
    return out;
}

// Physical hotends named by M104/M109 T (the end G-code's "; turn off hotend" lines included).
std::vector<int> temperature_tools(const std::vector<std::string> &lines)
{
    static const std::regex re(R"(^M10[49] [^;]*T(\d+))");
    std::vector<int> out;
    for (const std::string &line : lines) {
        std::smatch m;
        if (std::regex_search(line, m, re))
            out.push_back(std::stoi(m[1].str()));
    }
    return out;
}

// "; key = value" from the CONFIG_BLOCK.
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

double flush_length(const DynamicPrintConfig &cfg, int from, int to)
{
    const auto  *matrix = cfg.option<ConfigOptionFloats>("flush_volumes_matrix");
    const double d      = cfg.option<ConfigOptionFloats>("filament_diameter")->get_at(to);
    return matrix->get_at(from * NUM_FILAMENTS + to) / (M_PI / 4. * d * d);
}

// 1-based filament_map "2,1,2" -> the nozzle (1 = left, 2 = right) each filament prints from.
const int NOZZLE_OF[NUM_FILAMENTS] = { 2, 1, 2 };

void check_flushes_follow_the_nozzle_contents(const std::vector<Change> &changes, const DynamicPrintConfig &cfg, int start_filament)
{
    std::map<int, int> in_nozzle{ { NOZZLE_OF[start_filament], start_filament } };
    int                active = start_filament;
    size_t             returns_with_other_colour = 0;
    for (const Change &c : changes) {
        CAPTURE(active, c.next, c.flush, c.current_i, c.old_temp);
        REQUIRE(c.next >= 0);
        REQUIRE(c.next < NUM_FILAMENTS);
        // The firmware is told the filament that is really in the hotend, and its real temperature.
        CHECK(c.current_i == active);
        CHECK(c.old_temp > 0);
        CHECK(c.next != active);
        const int  nozzle = NOZZLE_OF[c.next];
        const auto it     = in_nozzle.find(nozzle);
        if (it == in_nozzle.end() || it->second == c.next) {
            CHECK(c.flush == Approx(0.).margin(1e-3));
        } else {
            ++returns_with_other_colour;
            CHECK(c.flush == Approx(flush_length(cfg, it->second, c.next)).epsilon(1e-3));
            CHECK(c.flush > 0.);
        }
        in_nozzle[nozzle] = c.next;
        active            = c.next;
    }
    CHECK(returns_with_other_colour >= 1);
}

} // namespace

SCENARIO("H2D print by object: tool changes and temperatures name the real hotend and nozzle contents", "[H2DByObject]")
{
    GIVEN("three objects: filament 1 (right), filament 2 (left), filament 3 (right), printed one after another")
    {
        const DynamicPrintConfig cfg = h2d_config("by object", false);
        Print print;
        Model model;
        add_objects(model, { 1, 2, 3 });
        const std::string              gcode = slice(print, model, cfg);
        const std::vector<std::string> lines = body_lines(gcode);

        THEN("every M104/M109 T is a physical hotend (0 or 1), never a filament slot")
        {
            const std::vector<int> tools = temperature_tools(lines);
            REQUIRE_FALSE(tools.empty());
            for (int t : tools) {
                CAPTURE(t);
                CHECK((t == 0 || t == 1));
            }
            // The object-transition lines are there, now per hotend.
            CHECK(gcode.find("M104 S220 T2") == std::string::npos);
        }
        THEN("the right -> left -> right changes flush what the target nozzle still holds, and name the loaded filament")
        {
            const std::vector<Change> changes = collect_changes(lines);
            // filament 1 -> 2 (to the empty left nozzle), 2 -> 3 (back to the right nozzle, which holds 1).
            REQUIRE(changes.size() == 2);
            CHECK(changes[0].next == 1);
            CHECK(changes[1].next == 2);
            check_flushes_follow_the_nozzle_contents(changes, cfg, 0);
            CHECK(changes[1].flush == Approx(flush_length(cfg, 0, 2)).epsilon(1e-3));
        }
        THEN("the start G-code heats and selects the right hotend, and hands each hotend its own first filament")
        {
            // Filament 1 is on the right hotend = physical 0.
            CHECK(gcode.find("M104 S220 T0 ; rise temp in advance") != std::string::npos);
            CHECK(gcode.find("G151 P0 M") != std::string::npos);
            CHECK(gcode.find("G151 P1 M") == std::string::npos);
            // Toolhead offset calibration: right (T0) -> filament index 0, left (T1) -> filament index 1.
            CHECK(gcode.find("M620.17 T0 S220 L0") != std::string::npos);
            CHECK(gcode.find("M620.17 T1 S220 L1") != std::string::npos);
        }
        THEN("the job describes the same nozzles: header nozzle map and grouping follow filament_map")
        {
            // Nozzle 0 = the left extruder's, nozzle 1 = the right one's (the ids slice_info's
            // <filament group_id> and <nozzle> use). The project's stale value must not leak out.
            CHECK(config_value(gcode, "filament_map") == "2,1,2");
            CHECK(config_value(gcode, "filament_nozzle_map") == "1,0,1");
            auto group = print.get_layered_nozzle_group_result();
            REQUIRE(group);
            CHECK(group->get_extruder_map(false) == std::vector<int>{ 2, 1, 2 });
        }
    }

    GIVEN("the same plate printed by layer without a prime tower")
    {
        const DynamicPrintConfig cfg = h2d_config("by layer", false);
        Print print;
        Model model;
        add_objects(model, { 1, 2, 3 });
        const std::string              gcode = slice(print, model, cfg);
        const std::vector<std::string> lines = body_lines(gcode);
        for (int t : temperature_tools(lines)) {
            CAPTURE(t);
            CHECK((t == 0 || t == 1));
        }
        const std::vector<Change> changes = collect_changes(lines);
        REQUIRE(changes.size() >= 2);
        // The start G-code loads the first filament of the first layer; find it from the first change.
        check_flushes_follow_the_nozzle_contents(changes, cfg, changes.front().current_i);
    }
}

SCENARIO("H2D: no second change_filament block for the filament the start G-code loaded", "[H2DByObject]")
{
    GIVEN("a single object printed with filament 3 (right hotend), print by object")
    {
        const DynamicPrintConfig cfg = h2d_config("by object", false);
        Print print;
        Model model;
        add_objects(model, { 3 });
        const std::string              gcode = slice(print, model, cfg);
        const std::vector<std::string> lines = body_lines(gcode);
        // The start G-code loads filament index 2 itself ("M620 S2A ... T2 H-1 ... M621 S2A"); nothing
        // after it may reload it or claim the hotend holds filament 0.
        CHECK(collect_changes(lines).empty());
        CHECK(gcode.find("M620.11 P0 I0") == std::string::npos);
        CHECK(gcode.find("M104 S220 T0 ; rise temp in advance") != std::string::npos);
        // The left hotend prints nothing: it must not be handed the right hotend's filament.
        CHECK(gcode.find("M620.17 T1 S220 L2") == std::string::npos);
        CHECK(gcode.find("M620.17 T0 S220 L2") != std::string::npos);
        // One object, no prime tower: Print::process builds no print-wide tool ordering, so this plate
        // used to go out ungrouped too.
        CHECK(config_value(gcode, "filament_nozzle_map") == "1,0,1");
        REQUIRE(print.get_layered_nozzle_group_result());
        for (int t : temperature_tools(lines)) {
            CAPTURE(t);
            CHECK((t == 0 || t == 1));
        }
    }
}

// Owner's H2C, 2026-09-24: on a print-by-object plate the idle hotend was cooled at each filament change
// and then heated back to print temperature for the whole next object. The object-start temperature
// lines (_print_first_layer_extruder_temperatures) set BOTH hotends, and the 2nd-layer transition set
// every hotend that holds a filament. Only the hotend that prints may be set; the idle one is heated by
// its own tool change (M620.10 A1 ... P<temp>) or by the idle-nozzle pre-heat ("... pre heating").
SCENARIO("H2D print by object: the idle hotend is never set to print temperature", "[H2DByObject]")
{
    for (const bool tower : { false, true }) {
        GIVEN(std::string("filaments right, left, right; first layer 230, other layers 220; prime tower ") + (tower ? "on" : "off"))
        {
            DynamicPrintConfig cfg = h2d_config("by object", tower);
            cfg.set_deserialize_strict({
                { "nozzle_temperature_initial_layer", "230,230,230" },
                { "nozzle_temperature", "220,220,220" },
            });
            Print print;
            Model model;
            add_objects(model, { 1, 2, 3 });
            const std::string              gcode = slice(print, model, cfg);
            const std::vector<std::string> lines = body_lines(gcode);

            // filament_map 2,1,2 with physical_extruder_map [1,0]: filaments 0 and 2 print on hotend 0.
            auto hotend_of = [](int filament) { return filament == 1 ? 1 : 0; };
            static const std::regex re_tool(R"(^T(\d+)( |$))");
            static const std::regex re_temp(R"(^M10[49] [^;]*S(\d+)[^;]*T(\d+))");
            static const std::regex re_temp2(R"(^M10[49] [^;]*T(\d+)[^;]*S(\d+))");
            int    loaded = -1;
            bool   printing = false;
            size_t object_starts = 0, active_lines = 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                const std::string &line = lines[i];
                std::smatch        m;
                if (line.rfind("; start printing object", 0) == 0) {
                    printing = true;
                    ++object_starts;
                    continue;
                }
                if (std::regex_search(line, m, re_tool)) {
                    const int t = std::stoi(m[1].str());
                    if (t < 255)
                        loaded = t;
                    continue;
                }
                if (!printing || loaded < 0 || line.find("pre heating") != std::string::npos)
                    continue;
                int temp = -1, tool = -1;
                if (std::regex_search(line, m, re_temp)) {
                    temp = std::stoi(m[1].str());
                    tool = std::stoi(m[2].str());
                } else if (std::regex_search(line, m, re_temp2)) {
                    tool = std::stoi(m[1].str());
                    temp = std::stoi(m[2].str());
                } else
                    continue;
                if (temp == 0)
                    continue; // the end G-code switching the hotends off
                CAPTURE(i, line, loaded);
                if (tool == hotend_of(loaded))
                    ++active_lines;
                else
                    CHECK(temp < 200); // a pre-cool at most, never print temperature
            }
            CHECK(object_starts >= 3);
            // The printing hotend still gets its object-start and 2nd-layer temperatures.
            CHECK(active_lines >= 4);
        }
    }
}

// Bambu Studio's per-filament flush settings and pre-tower cooling:
//   filament_flush_temp / filament_flush_temp_fast   -> flush_temperatures      (M620.10 ... T<t>)
//   filament_flush_volumetric_speed                  -> flush_volumetric_speeds (M620.10 / M620.11 ... F<f>)
//   filament_cooling_before_tower                    -> M620.15 C{new_filament_temp - filament_cooling_before_tower[next]}
// The BBL profiles carry them, but filament presets used to drop them, so the G-code always ran on
// the defaults (M620.15 C245 / C220 for PETG HF 245 + PLA 220 on an H2D, where Bambu Studio writes
// C235 / C210). Filament presets hold them now, per flow variant, and GCode.cpp publishes them the
// way Bambu Studio does (no pre-tower cooling on the first layer).
//
// Named namespace so these helpers do not collide with h2d_config / bbl_bundle above.
namespace bambu_flush_keys {

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

// Two H2D filaments with a prime tower, one 20 x 20 x 2 mm block each; every layer changes filament twice.
DynamicPrintConfig h2d_flush_config(const std::vector<std::string> &filaments, const std::string &nozzles = "Standard,Standard",
                                    const std::string &map = "1,2")
{
    PresetBundle &b = vendor_bundle("BBL");
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H2D 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2D", true));
    REQUIRE(b.filaments.select_preset_by_name(filaments.front(), true));
    b.filament_presets = { filaments.front() };
    b.set_num_filaments(unsigned(filaments.size()), std::vector<std::string>{ "#E01919", "#1943E0" });
    b.filament_presets = filaments;
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "nozzle_volume_type", nozzles },
        { "filament_map_mode", "Manual" },
        { "filament_map", map },
        { "enable_prime_tower", "1" },
        { "wipe_tower_x", 40. },
        { "wipe_tower_y", 250. },
        { "wipe_tower_rotation_angle", 0 },
        { "gcode_comments", 0 },
    });
    return cfg;
}

std::string slice_h2d(Print &print, const DynamicPrintConfig &cfg)
{
    Model model;
    for (int i = 0; i < 2; ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.1f));
        ModelObject *object = model.add_object();
        object->name        = "block" + std::to_string(i + 1);
        object->add_volume(cube);
        object->config.set("extruder", i + 1);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(150. + 40. * i, 160., 0.));
        object->ensure_on_bed();
    }
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

// The G-code lines matching `re` (first capture group), with the layer they are on
// ("; layer num/total_layer_count: N/M"; 0 before the first layer).
std::vector<std::pair<int, std::string>> lines_by_layer(const std::string &gcode, const std::regex &re)
{
    static const std::regex                  re_layer(R"(^; layer num/total_layer_count: (\d+)/)");
    std::vector<std::pair<int, std::string>> out;
    std::istringstream                       ss(gcode);
    int                                      layer = 0;
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
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

} // namespace bambu_flush_keys

TEST_CASE("BBL filament presets keep the Bambu flush keys", "[BambuFlushKeys][BBLProfiles]")
{
    using namespace bambu_flush_keys;
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
    SECTION("X1C PLA Basic carries none of the keys: the defaults")
    {
        const DynamicPrintConfig &x1c = filament_preset("BBL", "Bambu PLA Basic @BBL X1C");
        const std::vector<double> cooling = floats_of(x1c, "filament_cooling_before_tower");
        REQUIRE_FALSE(cooling.empty());
        for (double c : cooling)
            CHECK(c == 10.);
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
    using namespace bambu_flush_keys;
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

TEST_CASE("H2D PLA + PETG HF: M620.15 cools 10 degrees before the tower, not on the first layer", "[BambuFlushKeys][BBLProfiles]")
{
    using namespace bambu_flush_keys;
    Print             print;
    const std::string gcode = slice_h2d(print, h2d_flush_config({ H2D_PLA, H2D_PETG_HF }));
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

    // The CONFIG_BLOCK records the presets' values, not dropped defaults.
    CHECK(gcode.find("\n; filament_cooling_before_tower = 10,10,10,10\n") != std::string::npos);
}

TEST_CASE("Nil flush keys read as their defaults in the G-code", "[BambuFlushKeys][BBLProfiles]")
{
    using namespace bambu_flush_keys;
    DynamicPrintConfig cfg   = h2d_flush_config({ H2D_PLA, H2D_PETG_HF });
    const size_t       slots = cfg.option<ConfigOptionFloatsNullable>("filament_cooling_before_tower")->values.size();
    REQUIRE(slots >= 2);
    auto nil_ints   = new ConfigOptionIntsNullable(std::vector<int>(slots, ConfigOptionIntsNullable::nil_value()));
    auto nil_floats = new ConfigOptionFloatsNullable(std::vector<double>(slots, ConfigOptionFloatsNullable::nil_value()));
    cfg.set_key_value("filament_flush_temp", nil_ints);
    cfg.set_key_value("filament_flush_volumetric_speed", nil_floats);
    cfg.set_key_value("filament_cooling_before_tower", nil_floats->clone());
    Print             print;
    const std::string gcode = slice_h2d(print, cfg);

    // filament_cooling_before_tower nil -> its default 10.
    CHECK(values_on(lines_by_layer(gcode, re_m620_15), false) == std::set<std::string>{ "235", "210" });
    // filament_flush_temp nil -> 0 -> the top of the recommended range (PLA 240, PETG HF 270).
    const std::set<std::string> flush_temps = values_on(lines_by_layer(gcode, std::regex(R"(^M620\.10 A1 .* T(\d+) P\d+ S1$)")), false);
    CHECK(flush_temps == std::set<std::string>{ "240", "270" });
    CHECK(gcode.find("Tnan") == std::string::npos);
    CHECK(gcode.find("Cnan") == std::string::npos);
    CHECK(gcode.find("Fnan") == std::string::npos);
}

TEST_CASE("The flush keys are read from each filament's flow-variant column", "[BambuFlushKeys][BBLProfiles]")
{
    using namespace bambu_flush_keys;
    // Left nozzle Standard, right nozzle High Flow: filament 0 slices its Standard column, filament 1
    // its High Flow column. The composed config packs [F0 Standard, F0 High Flow, F1 Standard, F1 High Flow].
    DynamicPrintConfig cfg = h2d_flush_config({ H2D_PLA, H2D_PLA }, "Standard,High Flow", "1,2");
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
}
