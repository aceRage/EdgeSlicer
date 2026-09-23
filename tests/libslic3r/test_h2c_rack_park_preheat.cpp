#include <catch2/catch.hpp>

#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cstdlib>

#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

// Three fixes around a Bambu dual-nozzle tool change, sliced on this tree's own system profiles.
//
// 1. Parking a rack nozzle. The H2C change_filament_gcode (identical to Bambu Studio's) carries
//        M620.15 P[filament_pre_cooling_temperature_nc[next_filament_id]]
//        M620.11 O1 T[filament_retract_length_nc]
//    which tell the firmware how far to cool the outgoing hotend and how much filament to pull back
//    inside it before the hotend change. A racked nozzle has no electrical contact, so this must happen
//    while it is still on the toolhead. The fork used to shim both to 0 on every change (M620.15 P0 /
//    M620.11 O1 T0); Bambu Studio emits the incoming filament's pre-cool target and the OUTGOING
//    filament's retract length (BambuStudio GCode.cpp:934 / :8170), e.g. P180 / T18 for Bambu PLA
//    Basic @BBL H2C.
// 2. Orca's tool-changer preheat ("M104 S<t> T<filament> ; preheat Tn") must not be written for a
//    Bambu printer: Bambu firmware reads M104 T as a physical extruder. A non-Bambu tool changer
//    (Snapmaker U1) keeps it.
// 3. The CONFIG_BLOCK's filament_nozzle_map / filament_volume_map come from the plate's grouping
//    result (the same ids slice_info writes as <filament group_id>), not the stale project value.

namespace {

// Vendor bundles are big (BBL holds thousands of presets): load each once.
PresetBundle &vendor_bundle(const std::string &vendor)
{
    static std::map<std::string, std::unique_ptr<PresetBundle>> bundles;
    static PresetBundle                                        *library = nullptr;
    auto it = bundles.find(vendor);
    if (it != bundles.end())
        return *it->second;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("h2c_park_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    if (library == nullptr) {
        library = new PresetBundle();
        library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    auto b = std::make_unique<PresetBundle>();
    b->load_vendor_configs_from_json(profiles, vendor, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, library);
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

const Machine H2C{ "BBL", "Bambu Lab H2C 0.4 nozzle", "0.20mm Standard @BBL H2C", "Bambu PLA Basic @BBL H2C", { 165., 160. }, { 40., 250. }, true };
const Machine H2D{ "BBL", "Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D", { 165., 160. }, { 40., 250. }, true };
const Machine X1C{ "BBL", "Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C", { 128., 128. }, { 30., 200. }, true };
const Machine U1{ "Snapmaker", "Snapmaker U1 (0.4 nozzle)", "0.20mm Standard @Snapmaker U1 (0.4 nozzle)", "Generic PLA @U1 0.4 nozzle", { 135., 135. }, { 30., 210. }, false };

constexpr int NUM_FILAMENTS = 3;

DynamicPrintConfig machine_config(const Machine &m, std::initializer_list<ConfigBase::SetDeserializeItem> overrides = {})
{
    PresetBundle &b = vendor_bundle(m.vendor);
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
        // The tool-changer preheat only runs with a preheat time; make sure it is asked for everywhere.
        { "preheat_time", 30 },
    });
    cfg.set_deserialize_strict(overrides);
    return cfg;
}

// Three 20 x 20 x 6 mm cubes side by side, one per filament: every layer changes tool three times.
void add_plate(Model &model, const Machine &m)
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
        object->instances.front()->set_offset(Vec3d(m.bed_center.x() + 25. * (i - 1), m.bed_center.y(), 0.));
        object->ensure_on_bed();
    }
}

std::string slice(Print &print, Model &model, const DynamicPrintConfig &cfg, const Machine &m)
{
    add_plate(model, m);
    print.is_BBL_printer() = m.bbl;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

// The G-code without its "; key = value" config dump (which echoes change_filament_gcode verbatim).
std::string body_of(const std::string &gcode)
{
    static const std::string begin_tag = "; CONFIG_BLOCK_START";
    static const std::string end_tag   = "; CONFIG_BLOCK_END";
    std::string body = gcode;
    for (size_t b; (b = body.find(begin_tag)) != std::string::npos;) {
        const size_t e = body.find(end_tag, b);
        body.erase(b, e == std::string::npos ? std::string::npos : e + end_tag.size() - b);
    }
    return body;
}

std::vector<std::string> lines_of(const std::string &text)
{
    std::vector<std::string> out;
    std::istringstream       ss(text);
    for (std::string line; std::getline(ss, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out.push_back(line);
    }
    return out;
}

// "; key = value" from the config dump.
std::string config_value(const std::string &gcode, const std::string &key)
{
    const std::string tag = "\n; " + key + " = ";
    const size_t      pos = gcode.find(tag);
    if (pos == std::string::npos)
        return "<missing>";
    const size_t start = pos + tag.size();
    return gcode.substr(start, gcode.find('\n', start) - start);
}

std::string ints_to_string(const std::vector<int> &v) { return ConfigOptionInts(v).serialize(); }

size_t count_preheat_lines(const std::string &gcode)
{
    const std::regex re(R"(^M104(\.1)? .*T\d+.*; preheat T\d+)");
    size_t           n = 0;
    for (const std::string &line : lines_of(body_of(gcode)))
        if (std::regex_search(line, re))
            ++n;
    return n;
}

// One tool change of the H2C template, reduced to what the firmware is told about the parked hotend.
struct ParkCommands
{
    int    outgoing = -1;  // filament id before the change (-1: not known, the first change)
    int    incoming = -1;  // filament id after the change (the template's "M620 S<f>A")
    int    precool  = -1;  // M620.15 P<t>
    double retract  = -1.; // M620.11 O1 T<len>
};

std::vector<ParkCommands> collect_park_commands(const std::string &gcode)
{
    static const std::regex re_change(R"(^M620 S(\d+)A)");
    static const std::regex re_precool(R"(^M620\.15 P(\d+))");
    static const std::regex re_retract(R"(^M620\.11 O1 T([0-9.]+))");
    std::vector<ParkCommands> out;
    // The outgoing filament is the one the previous change block loaded; the start G-code's own
    // "M620 S0A" does not count (no filament is loaded yet when the first block runs).
    int                       pending = -1, loaded = -1;
    for (const std::string &line : lines_of(body_of(gcode))) {
        std::smatch m;
        if (std::regex_search(line, m, re_change)) {
            pending = std::stoi(m[1].str());
        } else if (std::regex_search(line, m, re_precool)) {
            out.push_back({ loaded, pending, std::stoi(m[1].str()), -1. });
            loaded = pending;
        } else if (std::regex_search(line, m, re_retract)) {
            if (!out.empty() && out.back().retract < 0.)
                out.back().retract = std::stod(m[1].str());
        }
    }
    return out;
}

} // namespace

TEST_CASE("H2C filament profiles keep filament_retract_length_nc on load", "[H2CRackPark]")
{
    const DynamicPrintConfig cfg = machine_config(H2C);
    const auto *retract = cfg.option<ConfigOptionFloatsNullable>("filament_retract_length_nc");
    REQUIRE(retract != nullptr);
    // Bambu PLA Basic @BBL H2C.json: "filament_retract_length_nc": ["18", "18", "18"].
    CHECK(retract->values == std::vector<double>(NUM_FILAMENTS, 18.));
    const auto *precool = cfg.option<ConfigOptionInts>("filament_pre_cooling_temperature_nc");
    REQUIRE(precool != nullptr);
    CHECK(precool->values == std::vector<int>(NUM_FILAMENTS, 180));

    // A filament that does not carry the key reads Bambu Studio's default (10 mm).
    CHECK(print_config_def.get("filament_retract_length_nc")->get_default_value<ConfigOptionFloatsNullable>()->values == std::vector<double>{ 10. });
}

SCENARIO("H2C tool changes tell the firmware how to park the outgoing hotend", "[H2CRackPark]")
{
    GIVEN("three filaments with distinct hotend-change pre-cool targets and retract lengths")
    {
        const DynamicPrintConfig cfg = machine_config(H2C, {
            { "filament_retract_length_nc", "11,14,17" },
            { "filament_pre_cooling_temperature_nc", "170,185,200" },
        });
        const std::vector<double> retract{ 11., 14., 17. };
        const std::vector<int>    precool{ 170, 185, 200 };
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, H2C);

        WHEN("the plate is sliced")
        {
            const auto changes = collect_park_commands(gcode);
            THEN("every change carries the incoming pre-cool and the outgoing filament's retract, never the old zero shim")
            {
                REQUIRE(changes.size() >= size_t(3));
                size_t with_outgoing = 0;
                for (const ParkCommands &c : changes) {
                    CAPTURE(c.outgoing, c.incoming, c.precool, c.retract);
                    REQUIRE(c.incoming >= 0);
                    REQUIRE(c.incoming < NUM_FILAMENTS);
                    CHECK(c.precool == precool[c.incoming]);
                    CHECK(c.precool != 0);
                    REQUIRE(c.retract >= 0.);
                    if (c.outgoing >= 0) {
                        ++with_outgoing;
                        CHECK(c.retract == retract[c.outgoing]);
                    } else {
                        // The first load after the start G-code: nothing is in a hotend yet, and Bambu
                        // Studio publishes 0 there too (GCode.cpp:8208).
                        CHECK(c.retract == 0.);
                    }
                }
                CHECK(with_outgoing == changes.size() - 1);
            }
            THEN("no Orca tool-changer preheat is written")
            {
                CHECK(count_preheat_lines(gcode) == 0);
            }
        }
    }

    GIVEN("the stock profiles (Bambu PLA Basic @BBL H2C on all three slots)")
    {
        const DynamicPrintConfig cfg = machine_config(H2C);
        Print print;
        Model model;
        const std::string gcode   = slice(print, model, cfg, H2C);
        const auto        changes = collect_park_commands(gcode);
        REQUIRE_FALSE(changes.empty());
        for (const ParkCommands &c : changes) {
            CHECK(c.precool == 180);
            if (c.outgoing >= 0)
                CHECK(c.retract == 18.);
        }
    }
}

SCENARIO("Header nozzle and volume maps follow the plate's grouping", "[H2CRackPark]")
{
    auto check_header = [](const Print &print, const std::string &gcode) {
        auto group = print.get_layered_nozzle_group_result();
        REQUIRE(group);
        const std::vector<int> nozzle_map = group->get_nozzle_map();
        CHECK(config_value(gcode, "filament_map") == ints_to_string(group->get_extruder_map(false)));
        CHECK(config_value(gcode, "filament_nozzle_map") == ints_to_string(nozzle_map));
        const std::vector<int> volumes = group->get_volume_map();
        const std::string      header_volumes = config_value(gcode, "filament_volume_map");
        ConfigOptionInts       parsed;
        parsed.deserialize(header_volumes);
        REQUIRE(parsed.values.size() == size_t(NUM_FILAMENTS));
        // Real NozzleVolumeType values only (a short nozzle_volume_type used to leak garbage here).
        for (int v : parsed.values) {
            CHECK(v >= 0);
            CHECK(v <= int(NozzleVolumeType::nvtMaxNozzleVolumeType));
        }
        for (unsigned int f : group->get_used_filaments())
            CHECK(parsed.values[f] == volumes[f]);
        // The header's nozzle ids are the same ids slice_info writes as <filament group_id>: each one
        // sits on the extruder filament_map names.
        const std::vector<int> extruders = group->get_extruder_map(true);
        for (unsigned int f : group->get_used_filaments()) {
            auto nozzle = group->get_first_nozzle_for_filament(int(f));
            REQUIRE(nozzle);
            CHECK(nozzle->group_id == nozzle_map[f]);
            CHECK(nozzle->extruder_id == extruders[f]);
        }
        return nozzle_map;
    };

    GIVEN("an H2C plate grouped automatically")
    {
        // A stale project value that must not reach the header.
        const DynamicPrintConfig cfg = machine_config(H2C, { { "filament_nozzle_map", "5,5,5" } });
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, H2C);
        check_header(print, gcode);
        CHECK(config_value(gcode, "filament_nozzle_map") != "5,5,5");
    }

    GIVEN("an H2C plate grouped by hand: filament 1 left, filaments 2 and 3 right (#114's manual mode)")
    {
        const DynamicPrintConfig cfg = machine_config(H2C, {
            { "filament_map_mode", "Manual" },
            { "filament_map", "1,2,2" },
            { "filament_nozzle_map", "5,5,5" },
        });
        Print print;
        Model model;
        const std::string gcode      = slice(print, model, cfg, H2C);
        const auto        nozzle_map = check_header(print, gcode);
        CHECK(config_value(gcode, "filament_map") == "1,2,2");
        // Filament 1 is on the left extruder's only nozzle, 2 and 3 on the right.
        CHECK(nozzle_map[0] != nozzle_map[1]);
        CHECK(nozzle_map[0] != nozzle_map[2]);
        CHECK(count_preheat_lines(gcode) == 0);
    }

    GIVEN("an H2D plate")
    {
        const DynamicPrintConfig cfg = machine_config(H2D, { { "filament_nozzle_map", "5,5,5" } });
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, H2D);
        check_header(print, gcode);
        CHECK(count_preheat_lines(gcode) == 0);
    }
}

TEST_CASE("Single-nozzle and non-Bambu machines keep their header and preheat behaviour", "[H2CRackPark]")
{
    SECTION("X1C: project nozzle map untouched, never a tool-changer preheat")
    {
        const DynamicPrintConfig cfg = machine_config(X1C, { { "filament_nozzle_map", "5,5,5" } });
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, X1C);
        CHECK(config_value(gcode, "filament_nozzle_map") == "5,5,5");
        CHECK(count_preheat_lines(gcode) == 0);
        // The parking commands belong to the H2C template only.
        CHECK(collect_park_commands(gcode).empty());
    }

    SECTION("Snapmaker U1 (a non-Bambu tool changer): the preheat stays")
    {
        const DynamicPrintConfig cfg = machine_config(U1, { { "filament_nozzle_map", "5,5,5" } });
        Print print;
        Model model;
        const std::string gcode = slice(print, model, cfg, U1);
        CHECK(config_value(gcode, "filament_nozzle_map") == "5,5,5");
        CHECK(count_preheat_lines(gcode) > 0);
    }
}

// Hidden, for the before/after and Bambu Studio comparisons. For each machine writes
//   $H2C_PARK_OUT/<machine>.gcode        this build's G-code of the three-cube plate
//   $H2C_PARK_OUT/<machine>.3mf          the same plate as a project of this fork (slice it with an
//                                        older EdgeSlicer CLI for a byte comparison)
//   $H2C_PARK_OUT/<machine>_bambu.3mf    Bambu machines: the plate through Export Bambu 3MF (slice it
//                                        with bambu-studio.exe --slice 0 and compare the tool changes)
TEST_CASE("Write rack parking plates for a before/after and Bambu Studio comparison", "[.H2CRackParkLive]")
{
    const char *out_dir = std::getenv("H2C_PARK_OUT");
    REQUIRE(out_dir != nullptr);
    const std::vector<std::pair<const Machine *, std::string>> machines{ { &H2C, "h2c" }, { &H2D, "h2d" }, { &X1C, "x1c" }, { &U1, "u1" } };
    for (const auto &[m, name] : machines) {
        DynamicPrintConfig cfg = machine_config(*m);
        Model              model;
        add_plate(model, *m);
        {
            Print print;
            print.is_BBL_printer() = m->bbl;
            print.apply(model, cfg);
            print.validate();
            print.set_status_silent();
            const std::string gcode = Test::gcode(print);
            boost::nowide::ofstream f((boost::filesystem::path(out_dir) / (name + ".gcode")).string(), std::ios::binary);
            f << gcode;
        }
        for (bool bambu : { false, true }) {
            if (bambu && !m->bbl)
                continue;
            PlateData *plate   = new PlateData();
            plate->plate_index = 0;
            for (int i = 0; i < NUM_FILAMENTS; ++i)
                plate->objects_and_instances.emplace_back(i, 0);
            StoreParams         sp;
            const std::string   path = (boost::filesystem::path(out_dir) / (name + (bambu ? "_bambu.3mf" : ".3mf"))).string();
            BambuExport::Report report;
            sp.path            = path.c_str();
            sp.model           = &model;
            sp.config          = &cfg;
            sp.plate_data_list = { plate };
            sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
            sp.bambu_compat    = bambu;
            sp.bambu_report    = bambu ? &report : nullptr;
            CHECK(store_bbs_3mf(sp));
            release_PlateData_list(sp.plate_data_list);
        }
    }
}
