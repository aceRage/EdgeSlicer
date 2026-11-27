#include <catch2/catch.hpp>

#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/PreCoolingInjector.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

// Bambu Lab P3C (N11, one extruder with a seven-nozzle Swaptek rack), imported from Bambu Studio
// 2.9.2 (v02.09.02.62, commit 4ac7753b). The AMS 3P has seven slots; the bed is 256 x 256 x 260.

namespace {

PresetBundle &p3c_bundle()
{
    static PresetBundle *bundle = [] {
        const std::string saved_data_dir = data_dir();
        const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bbl_p3c_%%%%-%%%%");
        boost::filesystem::create_directories(scratch);
        set_data_dir(scratch.string());
        const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
        static PresetBundle library;
        library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
        auto *b = new PresetBundle();
        b->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
        set_data_dir(saved_data_dir);
        return b;
    }();
    return *bundle;
}

DynamicPrintConfig selected_config(const char *printer, const char *process, const char *filament, unsigned n = 1)
{
    PresetBundle &b = p3c_bundle();
    REQUIRE(b.printers.select_preset_by_name(printer, true));
    REQUIRE(b.prints.select_preset_by_name(process, true));
    REQUIRE(b.filaments.select_preset_by_name(filament, true));
    b.filament_presets = {filament};
    std::vector<std::string> colors(n, "#E01919");
    b.set_num_filaments(n, colors);
    b.filament_presets = std::vector<std::string>(n, filament);
    return b.full_config_secure();
}

std::string config_block_dump(const DynamicPrintConfig &cfg)
{
    Print print;
    Model model;
    Test::init_print({Test::TestMesh::cube_20x20x20}, print, model, cfg);
    std::string dump;
    GCode::append_full_config(print, dump);
    return dump;
}

bool config_block_has(const std::string &dump, const std::string &key)
{
    return dump.find("; " + key + " = ") != std::string::npos;
}

std::string config_block_line(const std::string &dump, const std::string &key)
{
    const std::string tag = "; " + key + " = ";
    const size_t      pos = dump.find(tag);
    if (pos == std::string::npos)
        return {};
    const size_t end = dump.find('\n', pos);
    std::string  line = dump.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

// Three 20 x 20 x 6 mm cubes, one per filament, on the P3C bed (256 x 256, centre 128, 128).
std::string slice_p3c_three_cubes(Print &print, Model &model, const DynamicPrintConfig &cfg)
{
    constexpr double spacing = 25.;
    const Vec2d      centre(128., 128.);
    for (int i = 0; i < 3; ++i) {
        TriangleMesh cube = Test::mesh(Test::TestMesh::cube_20x20x20);
        cube.scale(Vec3f(1.f, 1.f, 0.3f));
        ModelObject *object = model.add_object();
        object->name        = "cube" + std::to_string(i);
        object->add_volume(cube);
        object->volumes.front()->name = object->name;
        object->config.set("extruder", i + 1);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(centre.x() + spacing * (i - 1), centre.y(), 0.));
        object->ensure_on_bed();
    }
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

} // namespace

TEST_CASE("P3C machine presets resolve to a 256x256x260 bed and seven tools", "[BblP3C]")
{
    PresetBundle &b = p3c_bundle();
    for (const char *printer : {
             "Bambu Lab P3C 0.2 nozzle",
             "Bambu Lab P3C 0.4 nozzle",
             "Bambu Lab P3C 0.6 nozzle",
             "Bambu Lab P3C 0.8 nozzle",
         }) {
        DYNAMIC_SECTION(printer)
        {
            const Preset *p = b.printers.find_preset(printer, false);
            REQUIRE(p != nullptr);
            CHECK(p->is_system);
            const auto *area = p->config.option<ConfigOptionPoints>("printable_area");
            REQUIRE(area != nullptr);
            REQUIRE(area->values.size() == 4);
            CHECK(area->values[0].x() == 0.);
            CHECK(area->values[0].y() == 0.);
            CHECK(area->values[1].x() == 256.);
            CHECK(area->values[1].y() == 0.);
            CHECK(area->values[2].x() == 256.);
            CHECK(area->values[2].y() == 256.);
            CHECK(area->values[3].x() == 0.);
            CHECK(area->values[3].y() == 256.);
            CHECK(p->config.opt_float("printable_height") == 260.);
            const auto *tools = p->config.option<ConfigOptionInts>("extruder_max_nozzle_count");
            REQUIRE(tools != nullptr);
            REQUIRE_FALSE(tools->values.empty());
            CHECK(tools->values[0] == 7);
            CHECK(p->config.opt_bool("handle_hotend_as_extruder"));
            CHECK(p->config.opt_bool("enable_pre_heating"));
        }
    }
}

TEST_CASE("P3C 0.4 defaults to a seven-slot rack", "[BblP3C][NozzleRackSlots]")
{
    PresetBundle &b = p3c_bundle();
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab P3C 0.4 nozzle", true));
    b.update_compatible(PresetSelectCompatibleType::Never);
    b.set_rack_nozzle_override({});
    const DynamicPrintConfig cfg = b.printer_config_with_project_sizes();
    CHECK(rack_slot_count(cfg, 0) == 7);
    const auto racks = extruder_rack_nozzles(cfg);
    REQUIRE(racks.size() == 1);
    REQUIRE(racks[0].size() == 7);
}

TEST_CASE("A P3C filament and process preset are compatible with the 0.4 machine", "[BblP3C]")
{
    PresetBundle &b = p3c_bundle();
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab P3C 0.4 nozzle", true));
    b.update_compatible(PresetSelectCompatibleType::Always);

    const Preset *filament = b.filaments.find_preset("Bambu PLA Basic @BBL P3C", false);
    REQUIRE(filament != nullptr);
    CHECK(filament->is_compatible);

    const Preset *process = b.prints.find_preset("0.20mm Standard @BBL P3C", false);
    REQUIRE(process != nullptr);
    CHECK(process->is_compatible);

    const auto *idle = filament->config.option<ConfigOptionFloats>("min_temperature_when_ec");
    REQUIRE(idle != nullptr);
    REQUIRE_FALSE(idle->values.empty());
    CHECK(idle->values[0] == 80.);
}

TEST_CASE("P3C TPU 2.85 mm presets keep the Direct Drive TPU285 path", "[BblP3C][NozzleVolumeType]")
{
    PresetBundle &b = p3c_bundle();
    const Preset *printer = b.printers.find_preset("Bambu Lab P3C 0.4 nozzle", false);
    REQUIRE(printer != nullptr);
    const auto *variants = printer->config.option<ConfigOptionStrings>("extruder_variant_list");
    REQUIRE(variants != nullptr);
    REQUIRE_FALSE(variants->values.empty());
    CHECK(variants->values[0].find("Direct Drive TPU285") != std::string::npos);

    const Preset *tpu = b.filaments.find_preset("Bambu TPU 75A(2.85mm) @BBL P3C", false);
    REQUIRE(tpu != nullptr);
}

// The P3C presets inherit the H3C scoped bases plus P3C-scoped copies of Bambu 2.9.2 bases
// ("@base P3C", fdm_filament_*_p3c), so they resolve to Bambu Studio 2.9.2's values and not
// to Edge's older shared bases. Expected values are Bambu v02.09.02.62 resolved.
TEST_CASE("P3C presets resolve to Bambu Studio 2.9.2 values", "[BblP3C]")
{
    PresetBundle &b = p3c_bundle();

    SECTION("process: tree supports, grid infill, arc fitting off")
    {
        const Preset *process = b.prints.find_preset("0.20mm Standard @BBL P3C", false);
        REQUIRE(process != nullptr);
        CHECK(process->config.opt_serialize("support_type") == "tree(auto)");
        CHECK(process->config.opt_serialize("sparse_infill_pattern") == "grid");
        CHECK(process->config.opt_serialize("enable_arc_fitting") == "0");
    }

    SECTION("filament: fins extrude safe temperature (M650.11 E1 in the templates)")
    {
        const std::vector<std::pair<const char *, const char *>> expected {
            {"Bambu PLA Basic @BBL P3C", "57"},
            {"Bambu ABS @BBL P3C", "87"},
            {"Bambu PC @BBL P3C", "117"},
            {"Generic PLA @BBL P3C", "57"},
        };
        for (const auto &[name, temp] : expected) {
            INFO(name);
            const Preset *f = b.filaments.find_preset(name, false);
            REQUIRE(f != nullptr);
            CHECK(f->config.opt_serialize("fins_extrude_safe_temp") == temp);
        }
    }

    SECTION("filament: Engineering plate 55 C, PC density, no Edge-only end G-code")
    {
        for (const char *name : {"Bambu PLA Basic @BBL P3C", "Generic PLA @BBL P3C"}) {
            INFO(name);
            const Preset *f = b.filaments.find_preset(name, false);
            REQUIRE(f != nullptr);
            CHECK(f->config.opt_serialize("eng_plate_temp") == "55");
            CHECK(f->config.opt_serialize("eng_plate_temp_initial_layer") == "55");
        }
        const Preset *pc = b.filaments.find_preset("Bambu PC @BBL P3C", false);
        REQUIRE(pc != nullptr);
        CHECK(pc->config.opt_serialize("filament_density") == "1.185");

        const Preset *abs = b.filaments.find_preset("Generic ABS @BBL P3C 0.4 nozzle", false);
        REQUIRE(abs != nullptr);
        CHECK(abs->config.opt_serialize("filament_end_gcode").find("M106 P3 S0") == std::string::npos);
        CHECK(abs->config.opt_serialize("temperature_vitrification") == "100");
    }

    SECTION("the P3C-scoped copy of the instantiated Bambu \"Generic TPU\" is a base only")
    {
        CHECK(b.filaments.find_preset("Generic TPU P3C", false) == nullptr);
        CHECK(b.filaments.find_preset("Generic TPU @BBL P3C", false) != nullptr);
    }
}

TEST_CASE("min_temperature_when_ec is dumped only when a used filament sets a real idle floor", "[BblP3C][GCode]")
{
    SECTION("H2D and H3C floors are <= 0, so the key stays out of the config block")
    {
        const DynamicPrintConfig h2d = selected_config("Bambu Lab H2D 0.4 nozzle", "0.20mm Standard @BBL H2D", "Bambu PLA Basic @BBL H2D");
        CHECK_FALSE(config_block_has(config_block_dump(h2d), "min_temperature_when_ec"));
        const DynamicPrintConfig h3c = selected_config("Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C");
        CHECK_FALSE(config_block_has(config_block_dump(h3c), "min_temperature_when_ec"));
        DynamicPrintConfig zero = h2d;
        zero.set_deserialize_strict({{"min_temperature_when_ec", "0"}});
        CHECK_FALSE(config_block_has(config_block_dump(zero), "min_temperature_when_ec"));
        DynamicPrintConfig neg = h2d;
        neg.set_deserialize_strict({{"min_temperature_when_ec", "-1"}});
        CHECK_FALSE(config_block_has(config_block_dump(neg), "min_temperature_when_ec"));
    }
    SECTION("P3C's floor of 80 is written")
    {
        const DynamicPrintConfig p3c = selected_config("Bambu Lab P3C 0.4 nozzle", "0.20mm Standard @BBL P3C", "Bambu PLA Basic @BBL P3C");
        CHECK(config_block_line(config_block_dump(p3c), "min_temperature_when_ec") == "; min_temperature_when_ec = 80");
    }
}

TEST_CASE("min_temperature_when_ec is one value per filament not a packed flow-variant slot", "[BblP3C][GCode]")
{
    CHECK_FALSE(is_filament_flow_variant_option("min_temperature_when_ec"));

    // P3C Bambu filaments declare Std+HF (step_size 2). Packed get_config_idx(filament 1) is 2,
    // which is past a two-value vector and used to fall back to filament 0's 80.
    DynamicPrintConfig packed;
    packed.set_key_value("min_temperature_when_ec", new ConfigOptionFloats{80., -1.});
    packed.set_key_value("filament_flow_step_size", new ConfigOptionInts{2, 2});
    CHECK(get_config_idx(packed, ConfigFlowDomain::Filament, 1) == 2);
    CHECK(min_temperature_when_ec_at(packed, 0) == 80.);
    CHECK(min_temperature_when_ec_at(packed, 1) == 0.);

    DynamicPrintConfig three;
    three.set_key_value("min_temperature_when_ec", new ConfigOptionFloats{80., -1., 0.});
    three.set_key_value("filament_flow_step_size", new ConfigOptionInts{2, 2, 2});
    CHECK(get_config_idx(three, ConfigFlowDomain::Filament, 2) == 4);
    CHECK(min_temperature_when_ec_at(three, 0) == 80.);
    CHECK(min_temperature_when_ec_at(three, 1) == 0.);
    CHECK(min_temperature_when_ec_at(three, 2) == 0.);

    // Past the end is no floor (0), not get_at's fallback to filament 0's 80.
    DynamicPrintConfig one;
    one.set_key_value("min_temperature_when_ec", new ConfigOptionFloats{80.});
    CHECK(min_temperature_when_ec_at(one, 0) == 80.);
    CHECK(min_temperature_when_ec_at(one, 1) == 0.);
}

TEST_CASE("A P3C job with floors 80 -1 0 keeps each filament idle floor", "[BblP3C][GCode]")
{
    DynamicPrintConfig cfg = selected_config("Bambu Lab P3C 0.4 nozzle", "0.20mm Standard @BBL P3C", "Bambu PLA Basic @BBL P3C", 3);
    const auto *steps = cfg.option<ConfigOptionInts>("filament_flow_step_size");
    REQUIRE(steps != nullptr);
    REQUIRE(steps->values.size() >= 3);
    // Std+HF: packed indexing would skip slots; filament 2's packed idx is past the three-value vector.
    CHECK(steps->values[0] >= 2);
    CHECK(get_config_idx(cfg, ConfigFlowDomain::Filament, 1) != 1);

    cfg.set_deserialize_strict({
        {"min_temperature_when_ec", "80,-1,0"},
        {"enable_prime_tower", "1"},
        {"wipe_tower_x", 40.},
        {"wipe_tower_y", 200.},
        {"wipe_tower_rotation_angle", 0},
        {"gcode_comments", 0},
    });
    CHECK(min_temperature_when_ec_at(cfg, 0) == 80.);
    CHECK(min_temperature_when_ec_at(cfg, 1) == 0.);
    CHECK(min_temperature_when_ec_at(cfg, 2) == 0.);

    SECTION("the config block dumps the three stored values because filament 0 sets a real floor")
    {
        CHECK(config_block_line(config_block_dump(cfg), "min_temperature_when_ec") == "; min_temperature_when_ec = 80,-1,0");
    }

    SECTION("each tool idle M104 floor matches that filament")
    {
        Print             print;
        Model             model;
        const std::string gcode = slice_p3c_three_cubes(print, model, cfg);
        REQUIRE(PreCooling::pre_cooling_active(print));
        PreCooling::Plan plan;
        REQUIRE(PreCooling::make_plan(print, plan));
        REQUIRE(plan.params.filament_min_temperature_when_ec.size() >= 3);
        CHECK(plan.params.filament_min_temperature_when_ec[0] == 80.f);
        CHECK(plan.params.filament_min_temperature_when_ec[1] == 0.f);
        CHECK(plan.params.filament_min_temperature_when_ec[2] == 0.f);

        CHECK(config_block_line(gcode, "min_temperature_when_ec") == "; min_temperature_when_ec = 80,-1,0");

        // M104 I is the G-code tool. Map it to a filament; do not treat I as a filament index.
        const std::vector<int> filament_tools = filament_output_tools(print.config(), print.extruders());
        const std::vector<int> tool_filaments = output_tool_filaments(filament_tools);
        auto filament_of_tool = [&](int tool) -> int {
            if (tool < 0)
                return -1;
            if (!tool_filaments.empty() && size_t(tool) < tool_filaments.size() && tool_filaments[size_t(tool)] >= 0)
                return tool_filaments[size_t(tool)];
            return tool;
        };

        const std::regex re_idle(R"(^M104 I(\d+) S(\d+) N0 C ;Multi extruder pre cooling)");
        const double     floor[3] = {80., 0., 0.};
        std::map<int, int> min_s;
        std::istringstream ss(gcode);
        for (std::string line; std::getline(ss, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::smatch m;
            if (!std::regex_search(line, m, re_idle))
                continue;
            const int f = filament_of_tool(std::stoi(m[1].str()));
            const int s = std::stoi(m[2].str());
            if (f < 0 || f >= 3)
                continue;
            if (!min_s.count(f) || s < min_s[f])
                min_s[f] = s;
        }
        REQUIRE(min_s.count(0));
        REQUIRE(min_s.count(1));
        REQUIRE(min_s.count(2));
        CHECK(min_s[0] >= int(floor[0]));
        // The old packed-index bug only raised floors (filament 1/2 read 80). S >= floor would still pass.
        CHECK(min_s[1] < int(floor[0]));
        CHECK(min_s[2] < int(floor[0]));
        CHECK(min_s[1] >= int(floor[1]));
        CHECK(min_s[2] >= int(floor[2]));
    }
}
