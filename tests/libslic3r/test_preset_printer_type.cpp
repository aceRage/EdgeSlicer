#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// Port of OrcaSlicer #14437 (faster printer preset switching): Preset::get_printer_type and
// get_current_printer_type walk every vendor's printer models by const reference now, instead of
// copying each VendorProfile and PrinterModel and re-reading "printer_model" per model. They run
// several times per printer switch; the answer must not change.

namespace {

VendorProfile::PrinterModel make_model(const std::string &name, const std::string &model_id)
{
    VendorProfile::PrinterModel model;
    model.id       = name;
    model.name     = name;
    model.model_id = model_id;
    return model;
}

Preset &add_printer(PresetBundle &bundle, const std::string &name, const std::string &printer_model)
{
    DynamicPrintConfig cfg = bundle.printers.default_preset().config;
    cfg.option<ConfigOptionString>("printer_settings_id", true)->value = name;
    cfg.option<ConfigOptionString>("printer_model", true)->value       = printer_model;
    Preset &printer = bundle.printers.load_preset(std::string(), name, std::move(cfg), false);
    printer.is_visible = true;
    return printer;
}

PresetBundle make_bundle()
{
    PresetBundle bundle;
    VendorProfile bbl("BBL");
    bbl.models.push_back(make_model("Bambu Lab X1 Carbon", "BL-P001"));
    bbl.models.push_back(make_model("Bambu Lab H2D", "O1D"));
    bundle.vendors.emplace("BBL", bbl);
    VendorProfile snapmaker("Snapmaker");
    snapmaker.models.push_back(make_model("Snapmaker U1", "SM_U1"));
    bundle.vendors.emplace("Snapmaker", snapmaker);

    add_printer(bundle, "Bambu Lab X1 Carbon 0.4 nozzle", "Bambu Lab X1 Carbon");
    add_printer(bundle, "Bambu Lab H2D 0.4 nozzle", "Bambu Lab H2D");
    add_printer(bundle, "Snapmaker U1 (0.4 nozzle)", "Snapmaker U1");
    add_printer(bundle, "My custom printer", "Not a vendor model");
    return bundle;
}

} // namespace

TEST_CASE("get_printer_type follows the selected printer across switches", "[Preset][PrinterType]")
{
    PresetBundle bundle = make_bundle();

    const std::vector<std::pair<std::string, std::string>> switches = {
        { "Bambu Lab X1 Carbon 0.4 nozzle", "BL-P001" },
        { "Snapmaker U1 (0.4 nozzle)", "SM_U1" },
        { "Bambu Lab H2D 0.4 nozzle", "O1D" },
        { "My custom printer", "" },
        { "Bambu Lab X1 Carbon 0.4 nozzle", "BL-P001" },
    };
    for (const auto &[name, expected] : switches) {
        INFO("selected printer: " << name);
        REQUIRE(bundle.printers.select_preset_by_name(name, true));
        Preset &any = bundle.printers.get_selected_preset();
        // get_printer_type reads the bundle's edited printer, whichever preset it is called on.
        CHECK(any.get_printer_type(&bundle) == expected);
        CHECK(bundle.printers.get_edited_preset().get_printer_type(&bundle) == expected);
    }
}

TEST_CASE("get_current_printer_type reads the preset it is called on", "[Preset][PrinterType]")
{
    PresetBundle bundle = make_bundle();
    REQUIRE(bundle.printers.select_preset_by_name("Bambu Lab X1 Carbon 0.4 nozzle", true));

    Preset *u1 = bundle.printers.find_preset("Snapmaker U1 (0.4 nozzle)", false);
    Preset *h2d = bundle.printers.find_preset("Bambu Lab H2D 0.4 nozzle", false);
    Preset *custom = bundle.printers.find_preset("My custom printer", false);
    REQUIRE(u1 != nullptr);
    REQUIRE(h2d != nullptr);
    REQUIRE(custom != nullptr);

    CHECK(u1->get_current_printer_type(&bundle) == "SM_U1");
    CHECK(h2d->get_current_printer_type(&bundle) == "O1D");
    CHECK(custom->get_current_printer_type(&bundle).empty());
    // No bundle, no lookup.
    CHECK(u1->get_current_printer_type(nullptr).empty());
    CHECK(u1->get_printer_type(nullptr).empty());
}

// select_preset_by_name falls back to another preset when the one asked for is hidden or gone. The presets are
// in name order, so "the first visible" of a printer model is its smallest nozzle; the fallback takes the model's
// default variant (0.4) when that is visible, as PresetBundle::default_printer_variant and
// get_similar_printer_preset do.
namespace {

Preset &add_variant(PresetBundle &bundle, const std::string &name, const std::string &printer_model, const std::string &variant)
{
    Preset &printer = add_printer(bundle, name, printer_model);
    printer.config.option<ConfigOptionString>("printer_variant", true)->value = variant;
    return printer;
}

PresetBundle make_variant_bundle()
{
    PresetBundle bundle;
    for (const char *variant : { "0.2", "0.4", "0.6", "0.8" })
        add_variant(bundle, std::string("Snapmaker U1 (") + variant + " nozzle)", "Snapmaker U1", variant);
    for (const char *variant : { "0.25", "0.6" })
        add_variant(bundle, std::string("Odd Printer ") + variant + " nozzle", "Odd Printer", variant);
    return bundle;
}

void set_visible(PresetBundle &bundle, const std::string &name, bool visible)
{
    // real: the preset of the collection itself, not the edited copy of the selected one.
    Preset *preset = bundle.printers.find_preset(name, false, true);
    REQUIRE(preset != nullptr);
    preset->is_visible = visible;
}

} // namespace

TEST_CASE("select_preset_by_name falls back to the model's default nozzle variant", "[Preset][SelectFallback]")
{
    PresetBundle bundle = make_variant_bundle();
    REQUIRE(bundle.printers.select_preset_by_name("Snapmaker U1 (0.6 nozzle)", true));
    REQUIRE(bundle.printers.get_selected_preset_name() == "Snapmaker U1 (0.6 nozzle)");

    SECTION("a visible preset is selected by its name, whatever its nozzle") {
        bundle.printers.select_preset_by_name("Snapmaker U1 (0.2 nozzle)", true);
        CHECK(bundle.printers.get_selected_preset_name() == "Snapmaker U1 (0.2 nozzle)");
        bundle.printers.select_preset_by_name("Odd Printer 0.6 nozzle", true);
        CHECK(bundle.printers.get_selected_preset_name() == "Odd Printer 0.6 nozzle");
    }
    SECTION("a hidden preset gives way to its model's 0.4, not to the model's first") {
        set_visible(bundle, "Snapmaker U1 (0.6 nozzle)", false);
        bundle.printers.select_preset_by_name("Snapmaker U1 (0.6 nozzle)", true);
        CHECK(bundle.printers.get_selected_preset_name() == "Snapmaker U1 (0.4 nozzle)");
    }
    SECTION("with the 0.4 hidden as well it is the model's first visible") {
        set_visible(bundle, "Snapmaker U1 (0.6 nozzle)", false);
        set_visible(bundle, "Snapmaker U1 (0.4 nozzle)", false);
        bundle.printers.select_preset_by_name("Snapmaker U1 (0.6 nozzle)", true);
        CHECK(bundle.printers.get_selected_preset_name() == "Snapmaker U1 (0.2 nozzle)");
    }
    SECTION("a model without a 0.4 keeps its first visible") {
        set_visible(bundle, "Odd Printer 0.6 nozzle", false);
        bundle.printers.select_preset_by_name("Odd Printer 0.6 nozzle", true);
        CHECK(bundle.printers.get_selected_preset_name() == "Odd Printer 0.25 nozzle");
    }
    SECTION("a name that is not there gives the first visible model, at its 0.4") {
        // "Odd Printer ..." sorts before "Snapmaker ...": hide it, and the first visible preset is the U1's 0.2.
        set_visible(bundle, "Odd Printer 0.25 nozzle", false);
        set_visible(bundle, "Odd Printer 0.6 nozzle", false);
        bundle.printers.select_preset_by_name("No such printer", true);
        CHECK(bundle.printers.get_selected_preset_name() == "Snapmaker U1 (0.4 nozzle)");
    }
    SECTION("and the first visible model's first preset when it has no 0.4") {
        bundle.printers.select_preset_by_name("No such printer", true);
        CHECK(bundle.printers.get_selected_preset_name() == "Odd Printer 0.25 nozzle");
    }
    SECTION("the other preset types keep the first visible") {
        DynamicPrintConfig cfg = bundle.prints.default_preset().config;
        for (const char *name : { "0.08 Fine", "0.20 Standard" })
            bundle.prints.load_preset(std::string(), name, cfg, false).is_visible = true;
        bundle.prints.select_preset_by_name("No such process", true);
        CHECK(bundle.prints.get_selected_preset_name() == "0.08 Fine");
    }
}
