// Deleting a stand-alone user printer (one made with Create Printer / Create Nozzle, which inherits no
// system printer) also deletes the filament and process presets "attached" to it: Tab::delete_preset
// counts them for its question and Tab::select_preset deletes them. Which presets those are is
// PresetCollection::get_user_presets_for_printer(): the user presets whose compatible_printers name that
// printer and no other, as clone_presets_for_printer() makes them.
//
// Before, the Tab took every preset that was is_compatible && !is_default with the printer selected.
// is_compatible_with_printer() is true for a preset naming no printer and having no condition, and for
// one whose condition the printer meets, so the vendor's generic presets and the person's own presets
// made for every printer, or shared with another printer, were counted and deleted too. The first test
// keeps that rule here to show what it picked.
#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

const std::string SYSTEM_PRINTER = "Vendor Printer 0.4 nozzle";
const std::string MY_PRINTER     = "My Printer 0.4 nozzle";
const std::string OTHER_PRINTER  = "Other Printer 0.4 nozzle";

Preset &add_printer(PresetBundle &bundle, const std::string &name, bool system)
{
    DynamicPrintConfig cfg = bundle.printers.default_preset().config;
    cfg.option<ConfigOptionString>("printer_settings_id", true)->value = name;
    cfg.option<ConfigOptionString>("printer_model", true)->value       = system ? "Vendor Printer" : "";
    cfg.option<ConfigOptionString>("inherits", true)->value            = "";
    Preset &printer    = bundle.printers.load_preset(std::string(), name, std::move(cfg), false);
    printer.is_system  = system;
    printer.is_visible = true;
    return printer;
}

// A filament or process preset; compatible_printers and compatible_printers_condition as given.
Preset &add(PresetCollection &presets, const std::string &name, bool system, std::vector<std::string> compatible_printers,
            const std::string &condition = std::string(), const std::string &inherits = std::string())
{
    DynamicPrintConfig cfg = presets.default_preset().config;
    cfg.option<ConfigOptionStrings>("compatible_printers", true)->values = std::move(compatible_printers);
    cfg.option<ConfigOptionString>("compatible_printers_condition", true)->value = condition;
    cfg.option<ConfigOptionString>("inherits", true)->value                     = inherits;
    Preset &preset    = presets.load_preset(std::string(), name, std::move(cfg), false);
    preset.is_system  = system;
    preset.is_visible = true;
    return preset;
}

std::vector<std::string> names_of(const std::vector<Preset const *> &presets)
{
    std::vector<std::string> names;
    for (const Preset *preset : presets)
        names.push_back(preset->name);
    std::sort(names.begin(), names.end());
    return names;
}

// The rule Tab::delete_preset and Tab::select_preset used before: every compatible, non-default preset.
std::vector<std::string> old_rule(const PresetCollection &presets)
{
    std::vector<std::string> names;
    for (const Preset &preset : presets.get_presets())
        if (preset.is_compatible && !preset.is_default)
            names.push_back(preset.name);
    std::sort(names.begin(), names.end());
    return names;
}

PresetBundle make_bundle()
{
    PresetBundle bundle;
    add_printer(bundle, SYSTEM_PRINTER, true);
    add_printer(bundle, MY_PRINTER, false);
    add_printer(bundle, OTHER_PRINTER, false);

    // The vendor's: a generic filament for every printer (as the Orca filament library writes them), one
    // for its own printer, one for any 0.4 nozzle by condition; the same for process presets.
    add(bundle.filaments, "Generic PLA @System", true, {});
    add(bundle.filaments, "Vendor PLA @Vendor Printer 0.4 nozzle", true, {SYSTEM_PRINTER});
    add(bundle.filaments, "Generic PETG @0.4 nozzle", true, {}, "nozzle_diameter[0]==0.4");
    add(bundle.prints, "0.20mm Standard @System", true, {});
    add(bundle.prints, "0.20mm Standard @Vendor Printer", true, {SYSTEM_PRINTER});

    // The person's: made for My Printer alone (what Create Printer makes), shared with another printer,
    // made for every printer, made for the other printer.
    add(bundle.filaments, "Generic PLA @" + MY_PRINTER, false, {MY_PRINTER});
    add(bundle.filaments, "Shared PLA", false, {MY_PRINTER, OTHER_PRINTER});
    add(bundle.filaments, "Every Printer PLA", false, {});
    add(bundle.filaments, "Other PLA", false, {OTHER_PRINTER});
    add(bundle.prints, "0.20mm Standard @" + MY_PRINTER, false, {MY_PRINTER});
    add(bundle.prints, "Every Printer Process", false, {});
    add(bundle.prints, "Shared Process", false, {MY_PRINTER, OTHER_PRINTER});

    bundle.printers.select_preset_by_name(MY_PRINTER, true);
    bundle.update_compatible(PresetSelectCompatibleType::Always);
    return bundle;
}

} // namespace

TEST_CASE("A stand-alone user printer takes only the user presets made for it alone", "[Preset][Bundle][DeletePrinter]")
{
    PresetBundle bundle = make_bundle();
    REQUIRE(bundle.printers.get_selected_preset().name == MY_PRINTER);
    REQUIRE(!bundle.printers.get_selected_preset().is_system);
    REQUIRE(bundle.printers.get_selected_preset().inherits().empty());

    SECTION("The rule before picked the vendor's presets and the person's shared ones as well") {
        const std::vector<std::string> filaments = old_rule(bundle.filaments);
        const std::vector<std::string> prints    = old_rule(bundle.prints);
        INFO("old filament pick: " << Catch::Detail::stringify(filaments));
        INFO("old process pick: " << Catch::Detail::stringify(prints));
        auto has = [](const std::vector<std::string> &v, const std::string &s) { return std::find(v.begin(), v.end(), s) != v.end(); };
        // The vendor's generic presets: not files of the person's, but erased from the session.
        CHECK(has(filaments, "Generic PLA @System"));
        CHECK(has(filaments, "Generic PETG @0.4 nozzle"));
        CHECK(has(prints, "0.20mm Standard @System"));
        // The person's own presets that other printers use: their files were removed.
        CHECK(has(filaments, "Every Printer PLA"));
        CHECK(has(filaments, "Shared PLA"));
        CHECK(has(prints, "Every Printer Process"));
        CHECK(has(prints, "Shared Process"));
        // The dialog said "5 Filament Preset and 4 Process Preset", where 1 and 1 are the printer's.
        CHECK(filaments.size() == 5);
        CHECK(prints.size() == 4);
    }

    SECTION("get_user_presets_for_printer picks the printer's own") {
        CHECK(names_of(bundle.filaments.get_user_presets_for_printer(MY_PRINTER)) == std::vector<std::string>{"Generic PLA @" + MY_PRINTER});
        CHECK(names_of(bundle.prints.get_user_presets_for_printer(MY_PRINTER)) == std::vector<std::string>{"0.20mm Standard @" + MY_PRINTER});
        CHECK(names_of(bundle.filaments.get_user_presets_for_printer(OTHER_PRINTER)) == std::vector<std::string>{"Other PLA"});
        // A system printer's presets are system presets: nothing of the person's is its alone here.
        CHECK(bundle.filaments.get_user_presets_for_printer(SYSTEM_PRINTER).empty());
        CHECK(bundle.filaments.get_user_presets_for_printer("No Such Printer").empty());
        CHECK(bundle.filaments.get_user_presets_for_printer("").empty());
    }
}

TEST_CASE("A printer's own preset stays while a kept preset inherits from it", "[Preset][DeletePrinter]")
{
    PresetBundle bundle;
    // Root <- Middle <- Shared: Shared also lists another printer, so it is kept, and so are its parents.
    add(bundle.filaments, "Root", false, {MY_PRINTER});
    add(bundle.filaments, "Middle", false, {MY_PRINTER}, std::string(), "Root");
    add(bundle.filaments, "Shared", false, {MY_PRINTER, OTHER_PRINTER}, std::string(), "Middle");
    // Alone <- Child: both made for the printer alone, so both go with it.
    add(bundle.filaments, "Alone", false, {MY_PRINTER});
    add(bundle.filaments, "Child", false, {MY_PRINTER}, std::string(), "Alone");

    CHECK(names_of(bundle.filaments.get_user_presets_for_printer(MY_PRINTER)) == std::vector<std::string>{"Alone", "Child"});
}
