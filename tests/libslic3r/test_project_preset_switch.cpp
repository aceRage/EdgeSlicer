#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

// A project 3MF carries its own "Project-inside presets". PresetBundle::load_config_file_config
// splits the project's cummulative config into one filament preset per slot, and the printer
// binding is dropped on the way: clear_compatible_printers() wipes "compatible_printers" up front
// and the scatter loop skips that key (it is a list of printer names, not a per-filament vector).
// is_compatible_with_printer() reads an empty binding as "fits every printer", so such a preset
// stayed selected after switching to a machine it was never made for.
//
// load_external_preset repairs this by accident whenever the preset's "inherits" names an
// installed profile -- it copies every key the project did not explicitly change back from the
// parent, "compatible_printers" included. What is left unrepaired, and is what the bug report
// shows, is a project naming a profile this install does not have and with no inherits to fall
// back on: the preset then claims compatibility with everything.

namespace {

const std::string P1S_PRINTER = "Bambu Lab P1S 0.4 nozzle";
const std::string H2C_PRINTER = "Bambu Lab H2C 0.4 nozzle";
const std::string P1S_MATTE   = "Bambu PLA Matte @BBL X1C";   // the profile that covers the P1S
const std::string H2C_MATTE   = "Bambu PLA Matte @BBL H2C";
const std::string H2C_BASIC   = "Bambu PLA Basic @BBL H2C";

Preset &add_printer(PresetBundle &bundle, const std::string &name, const std::string &default_filament)
{
    DynamicPrintConfig cfg = bundle.printers.default_preset().config;
    cfg.option<ConfigOptionString>("printer_settings_id", true)->value = name;
    cfg.option<ConfigOptionString>("printer_model", true)->value = name;
    cfg.option<ConfigOptionString>("printer_variant", true)->value = "0.4";
    cfg.option<ConfigOptionString>("default_print_profile", true)->value = "";
    cfg.option<ConfigOptionStrings>("default_filament_profile", true)->values = { default_filament };
    Preset &printer = bundle.printers.load_preset(std::string(), name, std::move(cfg), false);
    printer.is_system = true;
    printer.is_visible = true;
    return printer;
}

// A vendor filament preset bound to one printer through compatible_printers, the way the BBL
// profiles under resources/profiles/BBL/filament are written -- they use the explicit list, not
// compatible_printers_condition. The alias is the name stem before '@', as the bundle loader
// derives it.
Preset &add_system_filament(PresetBundle &bundle, const std::string &name, const std::string &compatible_printer,
                            const std::string &alias)
{
    DynamicPrintConfig cfg = bundle.filaments.default_preset().config;
    cfg.option<ConfigOptionStrings>("filament_settings_id", true)->values = { name };
    cfg.option<ConfigOptionStrings>("compatible_printers", true)->values = { compatible_printer };
    cfg.option<ConfigOptionStrings>("filament_type", true)->values = { "PLA" };
    Preset &filament = bundle.filaments.load_preset(std::string(), name, std::move(cfg), false);
    filament.is_system = true;
    filament.is_visible = true;
    filament.alias = alias;
    return filament;
}

PresetBundle make_bundle()
{
    PresetBundle bundle;
    add_printer(bundle, P1S_PRINTER, P1S_MATTE);
    add_printer(bundle, H2C_PRINTER, H2C_BASIC);
    add_system_filament(bundle, P1S_MATTE, P1S_PRINTER, "Bambu PLA Matte");
    add_system_filament(bundle, H2C_MATTE, H2C_PRINTER, "Bambu PLA Matte");
    add_system_filament(bundle, H2C_BASIC, H2C_PRINTER, "Bambu PLA Basic");
    return bundle;
}

// Build what a project 3MF's Metadata/project_settings.config looks like for a P1S project with
// `num_filaments` slots, and load it the way Plater does.
void load_project(PresetBundle &bundle, const std::string &filament_name, const std::string &inherits,
                  size_t num_filaments)
{
    DynamicPrintConfig project;
    project.apply(FullPrintConfig::defaults());
    // Every per-filament vector carries one entry per slot, the way a real 3MF does.
    for (const std::string &key : bundle.filaments.default_preset().config.keys()) {
        ConfigOption *opt = project.option(key, false);
        if (opt != nullptr && !opt->is_scalar())
            static_cast<ConfigOptionVectorBase *>(opt)->resize(num_filaments);
    }
    project.option<ConfigOptionString>("printer_settings_id", true)->value = P1S_PRINTER;
    project.option<ConfigOptionString>("print_settings_id", true)->value = "";

    std::vector<std::string> filament_ids(num_filaments, filament_name);
    project.option<ConfigOptionStrings>("filament_settings_id", true)->values = filament_ids;
    project.option<ConfigOptionStrings>("filament_colour", true)->values.assign(num_filaments, "#FF8800");
    project.option<ConfigOptionStrings>("filament_type", true)->values.assign(num_filaments, "PLA");

    // [print, filament 0 .. n-1, printer]
    std::vector<std::string> inherits_group;
    inherits_group.push_back("");
    inherits_group.insert(inherits_group.end(), num_filaments, inherits);
    inherits_group.push_back(P1S_PRINTER);
    project.option<ConfigOptionStrings>("inherits_group", true)->values = inherits_group;

    // The project changed a filament value. load_external_preset resets every key NOT declared
    // here back to the parent's, so declaring it is what keeps the preset distinct.
    project.option<ConfigOptionInts>("nozzle_temperature", true)->values.assign(num_filaments, 231);
    std::vector<std::string> different;
    different.push_back("");
    different.insert(different.end(), num_filaments, "nozzle_temperature");
    different.push_back("");
    project.option<ConfigOptionStrings>("different_settings_to_system", true)->values = different;

    // A real project config states this, and the loader clears it before splitting the filaments.
    project.option<ConfigOptionStrings>("compatible_printers", true)->values = { P1S_PRINTER };

    bundle.load_config_model("project.3mf", std::move(project));
}

const Preset *find_project_filament(PresetBundle &bundle, std::string &name_out)
{
    for (const std::string &name : bundle.filament_presets) {
        const Preset *preset = bundle.filaments.find_preset(name, false);
        if (preset != nullptr && preset->is_project_embedded) {
            name_out = name;
            return preset;
        }
    }
    return nullptr;
}

} // namespace

// The reported case: the project names "Bambu PLA Matte @BBL P1S 0.4 nozzle" profiles, which this
// install does not have (it ships the "@BBL X1C" ones), and they were system presets in the
// install that saved the project, so there is no "inherits" either.
TEST_CASE("A project-inside filament with no binding of its own is bound to the project's printer",
          "[Preset][Bundle][ProjectPreset]")
{
    PresetBundle bundle = make_bundle();
    load_project(bundle, "Bambu PLA Matte @BBL P1S 0.4 nozzle", std::string(), 4);

    REQUIRE(bundle.filament_presets.size() == 4);
    std::string slot_name;
    const Preset *embedded = find_project_filament(bundle, slot_name);
    REQUIRE(embedded != nullptr);
    INFO("project-inside preset = " << slot_name);

    // Nothing in the project stated a binding, so the loader falls back to the project's printer.
    const auto *compatible = embedded->config.option<ConfigOptionStrings>("compatible_printers");
    REQUIRE(compatible != nullptr);
    CHECK(compatible->values == std::vector<std::string>{ P1S_PRINTER });

    SECTION("On the project's own printer it stays selected") {
        CHECK(bundle.filaments.find_preset(slot_name, false)->is_compatible);
        for (const std::string &name : bundle.filament_presets)
            CHECK(name == slot_name);
    }

    SECTION("Switching to a printer it was never made for deselects it in every slot") {
        bundle.printers.select_preset_by_name(H2C_PRINTER, true);
        bundle.update_compatible(PresetSelectCompatibleType::Always);

        CHECK_FALSE(bundle.filaments.find_preset(slot_name, false)->is_compatible);
        for (size_t idx = 0; idx < bundle.filament_presets.size(); ++idx) {
            INFO("filament slot " << idx);
            CHECK(bundle.filament_presets[idx] != slot_name);
            const Preset *chosen = bundle.filaments.find_preset(bundle.filament_presets[idx], false);
            REQUIRE(chosen != nullptr);
            CHECK(chosen->is_compatible);
        }
        // Deselected, never deleted: the project keeps its presets.
        CHECK(bundle.filaments.find_preset(slot_name, false) != nullptr);
    }

    SECTION("Switching back makes it usable again") {
        bundle.printers.select_preset_by_name(H2C_PRINTER, true);
        bundle.update_compatible(PresetSelectCompatibleType::Always);
        bundle.printers.select_preset_by_name(P1S_PRINTER, true);
        bundle.update_compatible(PresetSelectCompatibleType::Always);

        CHECK(bundle.filaments.find_preset(slot_name, false)->is_compatible);
    }
}

TEST_CASE("A single-slot project keeps its filament bound to the project's printer",
          "[Preset][Bundle][ProjectPreset]")
{
    PresetBundle bundle = make_bundle();
    load_project(bundle, "Bambu PLA Matte @BBL P1S 0.4 nozzle", std::string(), 1);

    REQUIRE(!bundle.filament_presets.empty());
    const std::string loaded_name = bundle.filament_presets.front();
    const Preset *loaded = bundle.filaments.find_preset(loaded_name, false);
    REQUIRE(loaded != nullptr);
    INFO("single-slot filament = " << loaded_name);

    // Bound to the printer the project was saved with, so it is usable there ...
    const auto *compatible = loaded->config.option<ConfigOptionStrings>("compatible_printers");
    REQUIRE(compatible != nullptr);
    CHECK(compatible->values == std::vector<std::string>{ P1S_PRINTER });
    CHECK(loaded->is_compatible);

    bundle.printers.select_preset_by_name(H2C_PRINTER, true);
    bundle.update_compatible(PresetSelectCompatibleType::Always);

    // ... and not on the H2C. Without the binding it would claim to fit this printer too and stay put.
    CHECK_FALSE(bundle.filaments.find_preset(loaded_name, false)->is_compatible);
    CHECK(bundle.filament_presets.front() != loaded_name);
    const Preset *chosen = bundle.filaments.find_preset(bundle.filament_presets.front(), false);
    REQUIRE(chosen != nullptr);
    CHECK(chosen->is_compatible);
}

// When the project does name a parent this install has, load_external_preset already copies the
// parent's compatible_printers back, so the binding was never lost. The replacement should then
// land on the same filament family rather than on the printer's generic default, which is what
// the alias lookup through "inherits" in PresetBundle::update_compatible is for.
TEST_CASE("A project-inside filament that inherits an installed profile follows that profile",
          "[Preset][Bundle][ProjectPreset]")
{
    PresetBundle bundle = make_bundle();
    load_project(bundle, "Bambu PLA Matte @BBL P1S 0.4 nozzle", P1S_MATTE, 2);

    std::string slot_name;
    const Preset *embedded = find_project_filament(bundle, slot_name);
    REQUIRE(embedded != nullptr);
    // Inherited from the parent, not from the project-printer fallback.
    CHECK(embedded->config.option<ConfigOptionStrings>("compatible_printers")->values
          == std::vector<std::string>{ P1S_PRINTER });

    bundle.printers.select_preset_by_name(H2C_PRINTER, true);
    bundle.update_compatible(PresetSelectCompatibleType::Always);

    CHECK_FALSE(bundle.filaments.find_preset(slot_name, false)->is_compatible);
    for (size_t idx = 0; idx < bundle.filament_presets.size(); ++idx) {
        INFO("filament slot " << idx);
        CHECK(bundle.filament_presets[idx] != slot_name);
        // The parent's alias ("Bambu PLA Matte") wins over the H2C's default filament.
        CHECK(bundle.filament_presets[idx] == H2C_MATTE);
    }
}

TEST_CASE("A project filament that states its own printers keeps deciding for itself",
          "[Preset][Bundle][ProjectPreset]")
{
    PresetBundle bundle = make_bundle();
    load_project(bundle, "Bambu PLA Matte @BBL P1S 0.4 nozzle", std::string(), 2);

    std::string slot_name;
    REQUIRE(find_project_filament(bundle, slot_name) != nullptr);

    // Re-point it at the H2C by hand: the fallback must not override an explicit binding.
    Preset *embedded = bundle.filaments.find_preset(slot_name, false);
    embedded->config.option<ConfigOptionStrings>("compatible_printers", true)->values = { H2C_PRINTER };

    bundle.printers.select_preset_by_name(H2C_PRINTER, true);
    bundle.update_compatible(PresetSelectCompatibleType::Always);

    CHECK(bundle.filaments.find_preset(slot_name, false)->is_compatible);
}
