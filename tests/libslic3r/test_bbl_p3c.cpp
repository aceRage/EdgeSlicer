#include <catch2/catch.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <string>

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
