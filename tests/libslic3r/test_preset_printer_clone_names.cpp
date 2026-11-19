// Create Printer copies the filament and process presets ticked for the new printer with
// PresetCollection::clone_presets_for_printer(), each named PresetCollection::name_for_printer_clone():
// the template's name up to " @" ("/" written "-"), then " @" and the printer's name.
//
// CreatePrinterPresetDialog checks whether the printer's presets are there already (so that, when a
// printer is made again over itself, no filament or process need be ticked) by looking each template's
// copy up by name. It used to build that name as alias + " @ " + printer (filament) and alias + " @" +
// printer (process): a space too many for every filament, and an alias that is not the template's name
// up to the "@" for a user preset (empty, unless it is a stand-alone filament) or a name with "/" in it.
// So the check missed presets that were there. These tests make the copies and look them up both ways.
#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>

#include <string>
#include <vector>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

const std::string MY_PRINTER = "My Printer 0.4 nozzle";

struct ScopedTemporaryDir
{
    fs::path root;
    ScopedTemporaryDir()
    {
        root = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("clone_names_%%%%%%%%");
        fs::create_directories(root);
    }
    ~ScopedTemporaryDir()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
};

// A collection whose user presets are written under dir, as the app's are.
void point_at(PresetCollection &presets, const fs::path &dir, const std::string &subdir)
{
    PresetsConfigSubstitutions substitutions;
    presets.load_presets(dir.string(), subdir, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    fs::create_directories(dir / subdir / "base");
}

Preset &add(PresetCollection &presets, const std::string &name, bool system, const std::string &alias, const std::string &inherits = std::string())
{
    DynamicPrintConfig cfg = presets.default_preset().config;
    cfg.option<ConfigOptionStrings>("compatible_printers", true)->values = {"Vendor Printer 0.4 nozzle"};
    cfg.option<ConfigOptionString>("inherits", true)->value = inherits;
    Preset &preset    = presets.load_preset(std::string(), name, std::move(cfg), false);
    preset.is_system  = system;
    preset.is_visible = true;
    preset.alias      = alias;
    return preset;
}

// The names the dialog looked up before.
std::string old_filament_check(const Preset &tmpl) { return tmpl.alias + " @ " + MY_PRINTER; }
std::string old_process_check(const Preset &tmpl) { return tmpl.alias + " @" + MY_PRINTER; }

} // namespace

TEST_CASE("The name a preset copied for a printer gets", "[Preset][CreatePrinter]")
{
    CHECK(PresetCollection::name_for_printer_clone("Generic PLA @Vendor Printer 0.4 nozzle", MY_PRINTER) == "Generic PLA @" + MY_PRINTER);
    CHECK(PresetCollection::name_for_printer_clone("0.20mm Standard @Vendor", MY_PRINTER) == "0.20mm Standard @" + MY_PRINTER);
    // No " @" in it: the whole name.
    CHECK(PresetCollection::name_for_printer_clone("My PLA", MY_PRINTER) == "My PLA @" + MY_PRINTER);
    // "/" would make a folder of the file name.
    CHECK(PresetCollection::name_for_printer_clone("PLA/PHA @Vendor", MY_PRINTER) == "PLA-PHA @" + MY_PRINTER);
    // Cut at the first " @" only.
    CHECK(PresetCollection::name_for_printer_clone("A @B @C", MY_PRINTER) == "A @" + MY_PRINTER);
}

TEST_CASE("The copies clone_presets_for_printer makes are found under that name", "[Preset][CreatePrinter]")
{
    ScopedTemporaryDir temp;
    PresetCollection   filaments(Preset::TYPE_FILAMENT, Preset::filament_options(),
                                 static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Filament");
    PresetCollection   prints(Preset::TYPE_PRINT, Preset::print_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()),
                              "Default Setting");
    point_at(filaments, temp.root, PRESET_FILAMENT_NAME);
    point_at(prints, temp.root, PRESET_PRINT_NAME);

    // Templates as the dialog lists them: the vendor's (alias: the name up to "@"), a user preset that
    // inherits one (no alias), one with "/" in its name.
    add(filaments, "Generic PLA @Vendor Printer 0.4 nozzle", true, "Generic PLA");
    add(filaments, "My PLA @Vendor Printer 0.4 nozzle", false, "", "Generic PLA @Vendor Printer 0.4 nozzle");
    add(prints, "0.20mm Standard @Vendor", true, "0.20mm Standard");
    add(prints, "0.20mm Fine/Strong @Vendor", true, "0.20mm Fine/Strong");
    add(prints, "My Process @Vendor", false, "", "0.20mm Standard @Vendor");

    const std::vector<Preset const *> filament_templates = {filaments.find_preset("Generic PLA @Vendor Printer 0.4 nozzle"),
                                                            filaments.find_preset("My PLA @Vendor Printer 0.4 nozzle")};
    const std::vector<Preset const *> process_templates  = {prints.find_preset("0.20mm Standard @Vendor"),
                                                            prints.find_preset("0.20mm Fine/Strong @Vendor"),
                                                            prints.find_preset("My Process @Vendor")};
    for (const Preset *tmpl : filament_templates) REQUIRE(tmpl != nullptr);
    for (const Preset *tmpl : process_templates) REQUIRE(tmpl != nullptr);
    // Copies of what find_preset() hands out, as the dialog keeps them.
    std::vector<Preset> filament_copies, process_copies;
    for (const Preset *tmpl : filament_templates) filament_copies.push_back(*tmpl);
    for (const Preset *tmpl : process_templates) process_copies.push_back(*tmpl);

    std::vector<std::string> failures;
    auto filament_id = [](std::string) { return std::string("P0000001"); };
    REQUIRE(filaments.clone_presets_for_printer(filament_templates, failures, MY_PRINTER, filament_id));
    REQUIRE(prints.clone_presets_for_printer(process_templates, failures, MY_PRINTER, filament_id));
    REQUIRE(failures.empty());

    for (const Preset &tmpl : filament_copies) {
        INFO("filament template " << tmpl.name);
        const std::string made = PresetCollection::name_for_printer_clone(tmpl.name, MY_PRINTER);
        CHECK(filaments.find_preset(made) != nullptr);
        // The old check never finds a filament: " @ " is not " @".
        CHECK(filaments.find_preset(old_filament_check(tmpl)) == nullptr);
    }
    for (const Preset &tmpl : process_copies) {
        INFO("process template " << tmpl.name);
        const std::string made = PresetCollection::name_for_printer_clone(tmpl.name, MY_PRINTER);
        CHECK(prints.find_preset(made) != nullptr);
    }
    // The old process check finds the vendor's plain one only: not the user preset (no alias), not the
    // one with "/" in its name.
    CHECK(prints.find_preset(old_process_check(process_copies[0])) != nullptr);
    CHECK(prints.find_preset(old_process_check(process_copies[1])) == nullptr);
    CHECK(prints.find_preset(old_process_check(process_copies[2])) == nullptr);

    // Made again without rewriting: every copy is reported as there already, under the same name.
    // (Found again: the copies were inserted into the collection, which moves its presets.)
    failures.clear();
    const std::vector<Preset const *> again = {filaments.find_preset("Generic PLA @Vendor Printer 0.4 nozzle"),
                                               filaments.find_preset("My PLA @Vendor Printer 0.4 nozzle")};
    CHECK_FALSE(filaments.clone_presets_for_printer(again, failures, MY_PRINTER, filament_id));
    CHECK(failures == std::vector<std::string>{"Generic PLA @" + MY_PRINTER, "My PLA @" + MY_PRINTER});
}
