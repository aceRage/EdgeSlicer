#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// PresetBundle::check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray() is what
// MachineObject::check_ams_filament_valid() calls for a Bambu AMS tray whose tray_info_idx is a
// user filament id ("P" + 7 chars). It used to dereference filaments.get_filament_presets()
// .find(setting_id) without checking for end(), so a tray pointing at a filament id with no
// root preset in the bundle (user preset deleted, logged out, cloud-sync removed or quarantined
// between two status pushes) was undefined behaviour.

namespace {

const char *PRINTER_MODEL = "AMS-TEST-MODEL";
const char *PRINTER_NAME  = "AMS Test Printer 0.4 nozzle";

void add_system_printer(PresetBundle &bundle)
{
    DynamicPrintConfig cfg;
    Preset &printer = bundle.printers.load_preset(std::string(), PRINTER_NAME, cfg, false);
    printer.is_system  = true;
    printer.is_visible = true;
    printer.config.option<ConfigOptionString>("printer_model", true)->value = PRINTER_MODEL;
}

Preset &add_user_root_filament(PresetBundle &bundle, const std::string &name, const std::string &filament_id, int temp_low, int temp_high)
{
    DynamicPrintConfig cfg;
    Preset &filament = bundle.filaments.load_preset(std::string(), name, cfg, false);
    filament.is_system  = false;
    filament.is_visible = true;
    filament.filament_id = filament_id;
    filament.setting_id  = "PFUS" + filament_id.substr(1);
    filament.config.option<ConfigOptionStrings>("compatible_printers", true)->values = { PRINTER_NAME };
    filament.config.option<ConfigOptionInts>("nozzle_temperature_range_low", true)->values  = { temp_low };
    filament.config.option<ConfigOptionInts>("nozzle_temperature_range_high", true)->values = { temp_high };
    return filament;
}

} // namespace

TEST_CASE("AMS tray temp check survives a filament id with no preset", "[AmsLookup]")
{
    PresetBundle bundle;
    add_system_printer(bundle);
    add_user_root_filament(bundle, "My PLA @" + std::string(PRINTER_NAME), "P1234567", 190, 230);
    REQUIRE(bundle.filaments.get_filament_presets().count("P1234567") == 1);

    std::string nozzle     = "0.4";
    std::string tag_uid    = "";
    std::string temp_min   = "200";
    std::string temp_max   = "240";
    std::string preset_setting_id;

    SECTION("unknown filament id is reported unchanged, nothing to reset") {
        std::string setting_id = "P7654321";
        REQUIRE(bundle.filaments.get_filament_presets().count(setting_id) == 0);
        const bool equal = bundle.check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(
            PRINTER_MODEL, nozzle, setting_id, tag_uid, temp_min, temp_max, preset_setting_id);
        CHECK(equal);
        CHECK(preset_setting_id.empty());
        CHECK(temp_min == "200");
        CHECK(temp_max == "240");
    }

    SECTION("empty filament id is reported unchanged") {
        std::string setting_id;
        const bool equal = bundle.check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(
            PRINTER_MODEL, nozzle, setting_id, tag_uid, temp_min, temp_max, preset_setting_id);
        CHECK(equal);
        CHECK(preset_setting_id.empty());
    }

    SECTION("known filament id with different temps still asks for a reset") {
        std::string setting_id = "P1234567";
        const bool equal = bundle.check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(
            PRINTER_MODEL, nozzle, setting_id, tag_uid, temp_min, temp_max, preset_setting_id);
        CHECK_FALSE(equal);
        CHECK(preset_setting_id == "PFUS1234567");
        CHECK(temp_min == "190");
        CHECK(temp_max == "230");
    }

    SECTION("known filament id with matching temps is unchanged") {
        std::string setting_id = "P1234567";
        temp_min = "190";
        temp_max = "230";
        const bool equal = bundle.check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(
            PRINTER_MODEL, nozzle, setting_id, tag_uid, temp_min, temp_max, preset_setting_id);
        CHECK(equal);
        CHECK(preset_setting_id.empty());
    }
}
