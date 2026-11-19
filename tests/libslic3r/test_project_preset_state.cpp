// Project strip P4 (tests/design_multi_project_sidebar.md): one live Plater per open project, one app-global PresetBundle.
// Each project's presets are captured when another project is shown (PresetBundle::capture_project_presets) and put back
// when it is shown again (restore_project_presets): the selected printer / process / filaments, the edits made to them
// (they come back as that project's unsaved changes), the project config and the project's own embedded presets.

#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

const std::string PRINTER_A = "Strip Printer A 0.4 nozzle";
const std::string PRINTER_B = "Strip Printer B 0.4 nozzle";
const std::string PROCESS_A = "0.20mm Strip A";
const std::string PROCESS_B = "0.16mm Strip B";

void add_printer(PresetBundle &bundle, const std::string &name)
{
    DynamicPrintConfig cfg = bundle.printers.default_preset().config;
    cfg.option<ConfigOptionString>("printer_settings_id", true)->value      = name;
    cfg.option<ConfigOptionString>("printer_model", true)->value            = name;
    cfg.option<ConfigOptionString>("printer_variant", true)->value          = "0.4";
    cfg.option<ConfigOptionString>("default_print_profile", true)->value    = "";
    cfg.option<ConfigOptionStrings>("default_filament_profile", true)->values = {};
    Preset &printer   = bundle.printers.load_preset(std::string(), name, std::move(cfg), false);
    printer.is_system  = true;
    printer.is_visible = true;
}

void add_process(PresetBundle &bundle, const std::string &name, double layer_height)
{
    DynamicPrintConfig cfg = bundle.prints.default_preset().config;
    cfg.option<ConfigOptionString>("print_settings_id", true)->value = name;
    cfg.option<ConfigOptionFloat>("layer_height", true)->value       = layer_height;
    Preset &process   = bundle.prints.load_preset(std::string(), name, std::move(cfg), false);
    process.is_system  = true;
    process.is_visible = true;
}

PresetBundle make_bundle()
{
    PresetBundle bundle;
    add_printer(bundle, PRINTER_A);
    add_printer(bundle, PRINTER_B);
    add_process(bundle, PROCESS_A, 0.2);
    add_process(bundle, PROCESS_B, 0.16);
    return bundle;
}

// What the Printer and Process tabs do when the user picks presets: select, then the edited copy follows.
void select(PresetBundle &bundle, const std::string &printer, const std::string &process)
{
    REQUIRE(bundle.printers.select_preset_by_name(printer, true));
    REQUIRE(bundle.prints.select_preset_by_name(process, true));
}

double edited_infill(const PresetBundle &bundle) { return bundle.prints.get_edited_preset().config.opt_float("sparse_infill_density"); }

void set_project_colour(PresetBundle &bundle, const std::string &colour)
{
    bundle.project_config.option<ConfigOptionStrings>("filament_colour", true)->values = { colour };
}

std::string project_colour(const PresetBundle &bundle)
{
    const auto *opt = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    return opt != nullptr && !opt->values.empty() ? opt->values.front() : std::string();
}

} // namespace

TEST_CASE("Each project's presets come back when it is shown again", "[ProjectStrip][ProjectPresetState]")
{
    PresetBundle bundle = make_bundle();

    // Project A: printer A, process A with an unsaved edit, its own filament colour.
    select(bundle, PRINTER_A, PROCESS_A);
    bundle.prints.get_edited_preset().config.option<ConfigOptionPercent>("sparse_infill_density", true)->value = 33;
    bundle.prints.update_dirty();
    set_project_colour(bundle, "#FF0000");
    REQUIRE(bundle.prints.get_edited_preset().is_dirty);
    const PresetBundle::ProjectPresetState a = bundle.capture_project_presets();
    REQUIRE(a.valid);
    CHECK(bundle.project_presets_current(a));

    // Project B is shown: printer B, process B, nothing edited.
    select(bundle, PRINTER_B, PROCESS_B);
    set_project_colour(bundle, "#0000FF");
    REQUIRE_FALSE(bundle.prints.get_edited_preset().is_dirty);
    const PresetBundle::ProjectPresetState b = bundle.capture_project_presets();
    CHECK_FALSE(bundle.project_presets_current(a));
    CHECK(bundle.project_presets_current(b));

    SECTION("switching back to A puts its printer, process, edit and project config back")
    {
        std::vector<std::string> missing;
        bundle.restore_project_presets(a, &missing);
        CHECK(missing.empty());
        CHECK(bundle.printers.get_selected_preset_name() == PRINTER_A);
        CHECK(bundle.prints.get_selected_preset_name() == PROCESS_A);
        CHECK(edited_infill(bundle) == Approx(33));
        // The edit is A's unsaved change again, not a saved value.
        CHECK(bundle.prints.get_edited_preset().is_dirty);
        CHECK(project_colour(bundle) == "#FF0000");
        CHECK(bundle.project_presets_current(a));

        // And back to B: B's own, clean.
        bundle.restore_project_presets(b);
        CHECK(bundle.printers.get_selected_preset_name() == PRINTER_B);
        CHECK(bundle.prints.get_selected_preset_name() == PROCESS_B);
        CHECK_FALSE(bundle.prints.get_edited_preset().is_dirty);
        CHECK(project_colour(bundle) == "#0000FF");
        CHECK(bundle.project_presets_current(b));
    }

    SECTION("a preset saved meanwhile is the new baseline: A's values show as A's changes")
    {
        // While B is shown the user saves process A with a new infill (the preset on disk changes).
        Preset *saved = bundle.prints.find_preset(PROCESS_A, false, true);
        REQUIRE(saved != nullptr);
        saved->config.option<ConfigOptionPercent>("sparse_infill_density", true)->value = 50;
        bundle.restore_project_presets(a);
        CHECK(bundle.prints.get_selected_preset_name() == PROCESS_A);
        // A keeps the value it had and shows it as modified against the new save.
        CHECK(edited_infill(bundle) == Approx(33));
        CHECK(bundle.prints.get_edited_preset().is_dirty);
    }

    SECTION("a preset deleted meanwhile is reported and its values are kept")
    {
        PresetBundle::ProjectPresetState gone = a;
        gone.prints.selected = "0.20mm Deleted Meanwhile";
        std::vector<std::string> missing;
        bundle.restore_project_presets(gone, &missing);
        REQUIRE(missing.size() == 1);
        CHECK(missing.front() == "0.20mm Deleted Meanwhile");
        CHECK(edited_infill(bundle) == Approx(33));
    }
}

TEST_CASE("A project's own embedded presets follow it across a switch", "[ProjectStrip][ProjectPresetState]")
{
    PresetBundle bundle = make_bundle();
    select(bundle, PRINTER_A, PROCESS_A);

    // Project A was opened from a 3MF holding its own process, derived from process A.
    Preset own                = bundle.prints.get_selected_preset();
    own.name                  = "Project process (a.3mf)";
    own.is_system             = false;
    own.is_project_embedded   = true;
    own.config.option<ConfigOptionString>("inherits", true)->value     = PROCESS_A;
    own.config.option<ConfigOptionFloat>("layer_height", true)->value = 0.12;
    std::vector<Preset *> project_presets{ &own };
    bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(bundle.prints.find_preset("Project process (a.3mf)", false) != nullptr);
    REQUIRE(bundle.prints.select_preset_by_name("Project process (a.3mf)", true));
    const PresetBundle::ProjectPresetState a = bundle.capture_project_presets();
    REQUIRE(a.embedded.size() == 1);

    // Project B (a new project) has none: they go when it is shown.
    bundle.reset_project_embedded_presets();
    select(bundle, PRINTER_B, PROCESS_B);
    const PresetBundle::ProjectPresetState b = bundle.capture_project_presets();
    CHECK(b.embedded.empty());

    bundle.restore_project_presets(a);
    const Preset *back = bundle.prints.find_preset("Project process (a.3mf)", false);
    REQUIRE(back != nullptr);
    CHECK(back->is_project_embedded);
    CHECK(bundle.prints.get_selected_preset_name() == "Project process (a.3mf)");
    CHECK(bundle.prints.get_edited_preset().config.opt_float("layer_height") == Approx(0.12));
    CHECK(bundle.project_presets_current(a));

    bundle.restore_project_presets(b);
    CHECK(bundle.prints.find_preset("Project process (a.3mf)", false) == nullptr);
    CHECK(bundle.prints.get_selected_preset_name() == PROCESS_B);
    CHECK(bundle.project_presets_current(b));
}
