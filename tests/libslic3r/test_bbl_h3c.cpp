#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <string>

using namespace Slic3r;

// Bambu Lab H3C (N10, one extruder with a nine-nozzle rack), imported from Bambu Studio 2.9.1.
// Its start / filament-change templates read placeholders Bambu Studio 2.9.1 added:
// initial_no_support_nozzle_id, fins_extrude_safe_temp, initial_layer_travel_acceleration; and
// its 0.4 machine declares a TPU285 rack slot. These cases slice a cube on this tree's own BBL
// system profiles with the FULL start G-code, so any unresolved placeholder fails the slice.

namespace {

PresetBundle &h3c_bundle()
{
    static PresetBundle *bundle = [] {
        const std::string saved_data_dir = data_dir();
        const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bbl_h3c_%%%%-%%%%");
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

DynamicPrintConfig h3c_config(const char *printer, const char *process, const char *filament)
{
    PresetBundle &b = h3c_bundle();
    REQUIRE(b.printers.select_preset_by_name(printer, true));
    REQUIRE(b.prints.select_preset_by_name(process, true));
    REQUIRE(b.filaments.select_preset_by_name(filament, true));
    b.filament_presets = { filament };
    b.set_num_filaments(1, std::vector<std::string>{ "#E01919" });
    b.filament_presets = { filament };
    return b.full_config_secure();
}

std::string slice_cube(const DynamicPrintConfig &cfg)
{
    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(Test::mesh(Test::TestMesh::cube_20x20x20));
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(165., 160., 0.));
    object->ensure_on_bed();
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    return Test::gcode(print);
}

} // namespace

TEST_CASE("NozzleVolumeType TPU285 keeps Bambu's value and round-trips", "[BblH3C][NozzleVolumeType]")
{
    // Values saved in 3MFs, presets and slice_info: nothing existing may shift.
    STATIC_REQUIRE(int(nvtStandard) == 0);
    STATIC_REQUIRE(int(nvtHighFlow) == 1);
    STATIC_REQUIRE(int(nvtHybrid) == 2);
    STATIC_REQUIRE(int(nvtTPUHighFlow) == 3);
    STATIC_REQUIRE(int(nvtTPU285) == 4);
    STATIC_REQUIRE(int(nvtE3DHighFlow) == 5);

    CHECK(get_nozzle_volume_type_string(nvtTPU285) == "TPU285");
    CHECK(get_nozzle_volume_type_string(nvtE3DHighFlow) == "E3D High Flow");

    for (const char *key : { "nozzle_volume_type", "default_nozzle_volume_type", "extruder_nozzle_volume_type" }) {
        DYNAMIC_SECTION(key) {
            DynamicPrintConfig cfg;
            cfg.set_deserialize_strict({ { key, "Standard,TPU285,E3D High Flow,TPU High Flow" } });
            const auto *opt = cfg.option<ConfigOptionEnumsGeneric>(key);
            REQUIRE(opt != nullptr);
            CHECK(opt->values == std::vector<int>{ 0, 4, 5, 3 });
            CHECK(opt->serialize() == "Standard,TPU285,E3D High Flow,TPU High Flow");

            DynamicPrintConfig back;
            back.set_deserialize_strict({ { key, opt->serialize() } });
            CHECK(back.option<ConfigOptionEnumsGeneric>(key)->values == opt->values);
        }
    }
}

TEST_CASE("H3C machine preset declares the TPU285 rack slot", "[BblH3C]")
{
    PresetBundle &b = h3c_bundle();
    const Preset *printer = b.printers.find_preset("Bambu Lab H3C 0.4 nozzle", false);
    REQUIRE(printer != nullptr);
    const auto *dnvt = printer->config.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type");
    REQUIRE(dnvt != nullptr);
    REQUIRE(dnvt->values.size() == 9);
    for (size_t i = 0; i < dnvt->values.size(); ++i)
        CHECK(dnvt->values[i] == (i == 7 ? int(nvtTPU285) : int(nvtStandard)));
}

TEST_CASE("H3C cube slices with the full Bambu start G-code", "[BblH3C][PrintGCode]")
{
    struct Combo { const char *printer, *process, *filament; double nozzle; };
    const Combo combos[] = {
        { "Bambu Lab H3C 0.4 nozzle", "0.20mm Standard @BBL H3C", "Bambu PLA Basic @BBL H3C", 0.4 },
        { "Bambu Lab H3C 0.2 nozzle", "0.10mm Standard @BBL H3C 0.2 nozzle", "Bambu PLA Basic @BBL H3C 0.2 nozzle", 0.2 },
        { "Bambu Lab H3C 0.6 nozzle", "0.30mm Standard @BBL H3C 0.6 nozzle", "Bambu PLA Basic @BBL H3C 0.6 nozzle", 0.6 },
    };
    for (const Combo &c : combos) {
        DYNAMIC_SECTION(c.printer) {
            DynamicPrintConfig cfg = h3c_config(c.printer, c.process, c.filament);
            REQUIRE(cfg.option<ConfigOptionInts>("fins_extrude_safe_temp") != nullptr);
            CHECK(cfg.option<ConfigOptionInts>("fins_extrude_safe_temp")->get_at(0) == 57); // Bambu PLA Basic @base H3C

            std::string gcode;
            REQUIRE_NOTHROW(gcode = slice_cube(cfg));
            REQUIRE(gcode.find(";===== machine: H3C start") != std::string::npos);
            // The whole template ran (the M620.10 nozzle flush lines sit deep in it) and the cube printed.
            CHECK(gcode.find("\nM620.10 ") != std::string::npos);
            // ...and the end template ran after the layers. The config block (which quotes the template text)
            // sits in the header on BBL output, so look for the emitted line, not the quoted one.
            const size_t end_block = gcode.rfind("\n;===== H3C endgcode");
            REQUIRE(end_block != std::string::npos);
            CHECK(end_block > gcode.find("\nM620.10 "));
            // nozzle_diameter_at_nozzle_id[initial_no_support_nozzle_id] resolved to this machine's nozzle:
            // the M620.10 flush line carries it as H<diameter>.
            CHECK(gcode.find("Not a variable name") == std::string::npos);
            const size_t m620 = gcode.find("\nM620.10 A1 ");
            if (m620 != std::string::npos) {
                const std::string line = gcode.substr(m620 + 1, gcode.find('\n', m620 + 1) - m620 - 1);
                INFO(line);
                char buf[16];
                snprintf(buf, sizeof(buf), " H%g ", c.nozzle);
                CHECK(line.find(buf) != std::string::npos);
            }
        }
    }
}

// Owner hand test 2026-11-03: a new H3C project showed two rack nozzles ("0.4, 0.4"). With no rack text in the project
// or the printer preset, a rack machine has its full rack: the H3C nine slots (slot 8 TPU285 on the 0.4, as Bambu Studio
// 2.9.1 shows it), the H2C right extruder six. A saved project rack, a preset rack and Sync still win.
TEST_CASE("Rack machines default to their full rack", "[BblH3C][NozzleRackSlots]")
{
    PresetBundle &b = h3c_bundle();
    auto select = [&b](const char *printer) {
        REQUIRE(b.printers.select_preset_by_name(printer, true));
        b.update_compatible(PresetSelectCompatibleType::Never);
        b.set_rack_nozzle_override({});
        return b.printer_config_with_project_sizes();
    };

    SECTION("H3C 0.4: nine 0.4 slots, slot 8 TPU285")
    {
        const DynamicPrintConfig cfg = select("Bambu Lab H3C 0.4 nozzle");
        CHECK(default_rack_nozzle_text(cfg, 0) == "0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4:tpu285,0.4");
        const auto racks = extruder_rack_nozzles(cfg);
        REQUIRE(racks.size() == 1);
        REQUIRE(racks[0].size() == 9);
        for (size_t i = 0; i < 9; ++i) {
            INFO("slot " << i + 1);
            CHECK(racks[0][i].diameter == Approx(0.4));
            CHECK(racks[0][i].volume_type == (i == 7 ? nvtTPU285 : nvtStandard));
        }
        CHECK(rack_slot_count(cfg, 0) == 9);
        CHECK(rack_summary(racks[0]) == "9 x 0.4");
    }
    SECTION("H3C 0.2 / 0.6: nine slots of the machine's size; the 0.2 lists one Standard type for the extruder")
    {
        const DynamicPrintConfig c02 = select("Bambu Lab H3C 0.2 nozzle");
        CHECK(default_rack_nozzle_text(c02, 0) == "0.2,0.2,0.2,0.2,0.2,0.2,0.2,0.2,0.2");
        const DynamicPrintConfig c06 = select("Bambu Lab H3C 0.6 nozzle");
        CHECK(default_rack_nozzle_text(c06, 0) == "0.6,0.6,0.6,0.6,0.6,0.6,0.6,0.6:tpu285,0.6");
        CHECK(rack_slot_count(c06, 0) == 9);
    }
    SECTION("H2C: no rack on the left, six of its size on the right")
    {
        const DynamicPrintConfig cfg = select("Bambu Lab H2C 0.4 nozzle");
        CHECK(default_rack_nozzle_text(cfg, 0).empty());
        CHECK(default_rack_nozzle_text(cfg, 1) == "0.4,0.4,0.4,0.4,0.4,0.4");
        const auto racks = extruder_rack_nozzles(cfg);
        REQUIRE(racks.size() == 2);
        CHECK(racks[0].empty());
        CHECK(racks[1].size() == 6);
        CHECK(rack_slot_count(cfg, 1) == 6);
    }
    SECTION("X1C / H2D: no rack")
    {
        for (const char *printer : { "Bambu Lab X1 Carbon 0.4 nozzle", "Bambu Lab H2D 0.4 nozzle" }) {
            INFO(printer);
            const DynamicPrintConfig cfg = select(printer);
            for (const auto &rack : extruder_rack_nozzles(cfg))
                CHECK(rack.empty());
        }
    }
    SECTION("a saved project rack keeps its value")
    {
        select("Bambu Lab H3C 0.4 nozzle");
        REQUIRE(b.set_rack_nozzle_override(0, "0.4, 0.6, 0.2"));
        const auto racks = extruder_rack_nozzles(b.printer_config_with_project_sizes());
        REQUIRE(racks[0].size() == 3);
        CHECK(racks[0][1].diameter == Approx(0.6));
        CHECK(b.rack_nozzle_override(0) == "0.4, 0.6, 0.2");
        // The H2C keeps a saved right rack too.
        select("Bambu Lab H2C 0.4 nozzle");
        REQUIRE(b.set_rack_nozzle_override(1, "0.4,0.8"));
        CHECK(extruder_rack_nozzles(b.printer_config_with_project_sizes())[1].size() == 2);
        b.set_rack_nozzle_override({});
    }
    SECTION("a printer preset's rack text wins over the default")
    {
        DynamicPrintConfig cfg = select("Bambu Lab H3C 0.4 nozzle");
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "0.4,0.2" };
        CHECK(extruder_rack_nozzles(cfg)[0].size() == 2);
    }
    SECTION("Sync replaces the default with the reported nozzles")
    {
        const std::vector<std::string> synced = rack_override_from_reported({ { 0.4, 0.4, 0.6, 0.2 } }, {});
        REQUIRE(synced.size() == 1);
        DynamicPrintConfig cfg = select("Bambu Lab H3C 0.4 nozzle");
        cfg.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values = synced;
        const auto racks = extruder_rack_nozzles(cfg);
        REQUIRE(racks[0].size() == 4);
        CHECK(racks[0][2].diameter == Approx(0.6));
    }
}
