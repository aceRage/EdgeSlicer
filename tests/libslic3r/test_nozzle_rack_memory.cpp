// The project's nozzles remembered per printer between sessions (NozzleRackMemory.hpp, owner bugs 2026-11-06: an H2C opened with
// its whole rack on the variant's size and an H3C with every slot 0.4 Standard on every start). The text form, the fit check
// against a printer model (rack slots, sizes and nozzle types from extruder_variant_list / default_nozzle_volume_type), and the
// round trip through the app config on this tree's BBL system profiles, as a printer switch or a restart does it.

#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/NozzleRackMemory.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectSwapGuard.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

bool has_type(const std::vector<NozzleVolumeType> &types, NozzleVolumeType t) { return std::find(types.begin(), types.end(), t) != types.end(); }

DynamicPrintConfig printer(const std::vector<double> &nozzles, const std::vector<int> &capacity, const std::string &variant,
                           const std::vector<std::string> &variant_list, const std::vector<int> &default_types)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.option<ConfigOptionFloats>("nozzle_diameter", true)->values = nozzles;
    cfg.set_key_value("extruder_max_nozzle_count", new ConfigOptionIntsNullable(capacity));
    cfg.option<ConfigOptionString>("printer_variant", true)->value          = variant;
    cfg.option<ConfigOptionStrings>("extruder_variant_list", true)->values  = variant_list;
    cfg.option<ConfigOptionEnumsGeneric>("default_nozzle_volume_type", true)->values = default_types;
    cfg.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values         = std::vector<int>(nozzles.size(), int(nvtStandard));
    return cfg;
}

// Bambu Lab H3C as its profiles declare it: one extruder, a nine-nozzle rack, slot 8 TPU285 on the 0.4 variant only.
DynamicPrintConfig h3c(double size)
{
    if (size == 0.2)
        return printer({ 0.2 }, { 9 }, "0.2", { "Direct Drive Standard,Direct Drive High Flow" }, { int(nvtStandard) });
    std::vector<int> slots(9, int(nvtStandard));
    slots[7] = int(nvtTPU285);
    return printer({ size }, { 9 }, "0.4", { "Direct Drive Standard,Direct Drive High Flow,Direct Drive TPU285" }, slots);
}

// The shipped BBL profiles, loaded once (a scratch data dir, never the user's).
PresetBundle &bbl_bundle()
{
    static PresetBundle *bundle = [] {
        const std::string             saved_data_dir = data_dir();
        const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("nozzle_rack_memory_%%%%-%%%%");
        boost::filesystem::create_directories(scratch);
        set_data_dir(scratch.string());
        const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
        static PresetBundle library;
        library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                              ForwardCompatibilitySubstitutionRule::EnableSilent);
        auto *b = new PresetBundle();
        b->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
        set_data_dir(saved_data_dir);
        return b;
    }();
    return *bundle;
}

// Select a printer as the Printer tab does it (startup or a switch): the preset, then the per-printer nozzle update.
bool select_printer(PresetBundle &b, AppConfig &config, const std::string &name)
{
    REQUIRE(b.printers.select_preset_by_name(name, true));
    return b.update_nozzle_diameter_override_for_printer(config);
}

std::vector<int> project_types(const PresetBundle &b)
{
    const auto *opt = b.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    return opt != nullptr ? opt->values : std::vector<int>();
}

std::vector<std::string> project_racks(const PresetBundle &b)
{
    const auto *opt = b.project_config.option<ConfigOptionStrings>("extruder_rack_nozzle_override");
    return opt != nullptr ? opt->values : std::vector<std::string>();
}

} // namespace

TEST_CASE("Rack memory text round-trips", "[NozzleRackMemory]")
{
    NozzleRackMemory m;
    m.diameters    = { 0.6, 0.4 };
    m.volume_types = { int(nvtStandard), int(nvtHighFlow) };
    m.rack_texts   = { "", "0.4:high_flow,0.4,0.6,0.2" };
    const std::string text = serialize_nozzle_rack_memory(m);
    REQUIRE_FALSE(text.empty());
    NozzleRackMemory back;
    REQUIRE(parse_nozzle_rack_memory(text, back));
    CHECK(back == m);

    SECTION("an empty memory is not written") { CHECK(serialize_nozzle_rack_memory(NozzleRackMemory()).empty()); }
    SECTION("text that is not a memory is refused")
    {
        for (const char *bad : { "", "0.4,0.6", "{}", "{\"diameters\":[]}", "{\"diameters\":[0.4,-1]}", "{\"diameters\":[0.4],\"types\":[17]}",
                                 "{\"diameters\":[0.4],\"racks\":[3]}", "not json at all {" }) {
            CAPTURE(bad);
            CHECK_FALSE(parse_nozzle_rack_memory(bad, back));
            CHECK(back.empty());
        }
    }
}

TEST_CASE("extruder_variant_list entries name their nozzle type", "[NozzleRackMemory]")
{
    NozzleVolumeType t = nvtStandard;
    REQUIRE(nozzle_type_of_extruder_variant("Direct Drive TPU285", t));
    CHECK(t == nvtTPU285);
    REQUIRE(nozzle_type_of_extruder_variant("Direct Drive High Flow", t));
    CHECK(t == nvtHighFlow);
    REQUIRE(nozzle_type_of_extruder_variant("Direct Drive TPU High Flow", t));
    CHECK(t == nvtTPUHighFlow);
    REQUIRE(nozzle_type_of_extruder_variant("Direct Drive E3D High Flow", t));
    CHECK(t == nvtE3DHighFlow);
    REQUIRE(nozzle_type_of_extruder_variant("Direct Drive Standard", t));
    CHECK(t == nvtStandard);
    CHECK_FALSE(nozzle_type_of_extruder_variant("Direct Drive", t));
    CHECK_FALSE(nozzle_type_of_extruder_variant("", t));
}

TEST_CASE("H3C: every variant of the model offers TPU285, the 0.2 variant alone does not", "[NozzleRackMemory][BblH3C]")
{
    const NozzleRackLimits only_02 = nozzle_rack_limits(h3c(0.2));
    CHECK_FALSE(has_type(nozzle_rack_types(only_02, 0), nvtTPU285));
    CHECK(has_type(nozzle_rack_types(only_02, 0), nvtHighFlow));

    NozzleRackLimits model = nozzle_rack_limits(h3c(0.2));
    merge_nozzle_rack_limits(model, h3c(0.4));
    const std::vector<NozzleVolumeType> types = nozzle_rack_types(model, 0);
    REQUIRE(types.size() == 3);
    CHECK(types[0] == nvtStandard);
    CHECK(types[1] == nvtHighFlow);
    CHECK(types[2] == nvtTPU285);
    CHECK(model.capacity == std::vector<int>{ 9 });

    SECTION("a variant with another extruder count is another layout and adds nothing")
    {
        NozzleRackLimits l = nozzle_rack_limits(h3c(0.2));
        merge_nozzle_rack_limits(l, printer({ 0.4, 0.4 }, { 1, 6 }, "0.4", { "Direct Drive TPU285", "Direct Drive TPU285" }, { 0, 0 }));
        CHECK_FALSE(has_type(nozzle_rack_types(l, 0), nvtTPU285));
    }
}

TEST_CASE("A remembered rack is put back only when it still fits the printer model", "[NozzleRackMemory]")
{
    NozzleRackLimits model = nozzle_rack_limits(h3c(0.2));
    merge_nozzle_rack_limits(model, h3c(0.4));

    NozzleRackMemory m;
    m.diameters    = { 0.4 };
    m.volume_types = { int(nvtStandard) };
    m.rack_texts   = { "0.6,0.4,0.4,0.4,0.4,0.4,0.4,0.4:tpu285,0.4" };
    CHECK(validate_nozzle_rack_memory(m, model).empty());

    SECTION("another extruder count") {
        m.diameters = { 0.4, 0.4 };
        CHECK_FALSE(validate_nozzle_rack_memory(m, model).empty());
    }
    SECTION("more nozzles than rack slots") {
        m.rack_texts = { "0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4" };
        CHECK_FALSE(validate_nozzle_rack_memory(m, model).empty());
    }
    SECTION("a type no variant of the model names") {
        m.rack_texts = { "0.4,0.4:e3d_high_flow" };
        CHECK_FALSE(validate_nozzle_rack_memory(m, model).empty());
        m.rack_texts   = { "0.4" };
        m.volume_types = { int(nvtE3DHighFlow) };
        CHECK_FALSE(validate_nozzle_rack_memory(m, model).empty());
    }
    SECTION("a size the model has no nozzle of") {
        m.rack_texts = { "0.4,0.35" };
        CHECK_FALSE(validate_nozzle_rack_memory(m, model).empty());
    }
    SECTION("the extruder's own nozzle is not in its rack") {
        m.rack_texts = { "0.6,0.6" };
        CHECK_FALSE(validate_nozzle_rack_memory(m, model).empty());
    }
    SECTION("a rack on an extruder without one") {
        NozzleRackLimits x1 = nozzle_rack_limits(printer({ 0.4 }, { 1 }, "0.4", { "Direct Drive Standard" }, { 0 }));
        CHECK_FALSE(validate_nozzle_rack_memory(m, x1).empty());
        m.rack_texts.clear();
        CHECK(validate_nozzle_rack_memory(m, x1).empty());
    }
}

TEST_CASE("H3C and H2C racks come back after a printer switch or a restart", "[NozzleRackMemory][PresetBundle][BblH3C]")
{
    PresetBundle &b = bbl_bundle();
    AppConfig     config;
    const std::string h3c_rack = "0.6,0.4,0.4,0.4,0.4,0.4,0.4,0.4:tpu285,0.4";

    // A start on another model (the bundle is shared by the sections): nothing of the H3C's in the project.
    select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
    // H3C: slot 1 = 0.6, slot 8 = TPU285, set in Extruder settings, remembered.
    CHECK_FALSE(select_printer(b, config, "Bambu Lab H3C 0.4 nozzle")); // nothing remembered yet
    REQUIRE(project_racks(b).empty());
    REQUIRE(b.set_rack_nozzle_override(0, h3c_rack));
    b.remember_nozzle_rack(config);
    CHECK(config.has("nozzle_rack", "Bambu Lab H3C 0.4 nozzle"));
    CHECK(config.has("nozzle_rack_model", "Bambu Lab H3C"));

    // H2C: left 0.6, right 0.4 High Flow with its rack.
    const std::string h2c_rack = "0.4:high_flow,0.4,0.6,0.4,0.4,0.4";
    select_printer(b, config, "Bambu Lab H2C 0.4 nozzle");
    CHECK(project_racks(b).empty()); // another model: the H3C's rack does not come along
    REQUIRE(b.set_nozzle_diameter_override({ 0.6, 0. }));
    b.project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { int(nvtStandard), int(nvtHighFlow) };
    REQUIRE(b.set_rack_nozzle_override(1, h2c_rack));
    b.remember_nozzle_rack(config);

    // Back to the H3C: its rack, not the H2C's.
    CHECK(select_printer(b, config, "Bambu Lab H3C 0.4 nozzle"));
    CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
    CHECK(b.effective_nozzle_diameters() == std::vector<double>{ 0.4 });

    // Back to the H2C.
    CHECK(select_printer(b, config, "Bambu Lab H2C 0.4 nozzle"));
    CHECK(b.effective_nozzle_diameters() == std::vector<double>{ 0.6, 0.4 });
    CHECK(project_types(b) == std::vector<int>{ int(nvtStandard), int(nvtHighFlow) });
    CHECK(project_racks(b) == std::vector<std::string>{ "", h2c_rack });

    SECTION("a restart: a fresh project config and an app config read back from its text")
    {
        AppConfig restarted;
        for (const char *section : { "nozzle_rack", "nozzle_rack_model" })
            for (const auto &kv : config.get_section(section))
                restarted.set(section, kv.first, kv.second);
        PresetBundle &fresh = b;
        fresh.project_config.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values.clear();
        fresh.set_nozzle_diameter_override({ 0. });
        // Startup selects the last printer with no model restored yet: an X1C first stands in for "nothing restored".
        select_printer(fresh, restarted, "Bambu Lab X1 Carbon 0.4 nozzle");
        CHECK(select_printer(fresh, restarted, "Bambu Lab H3C 0.4 nozzle"));
        CHECK(project_racks(fresh) == std::vector<std::string>{ h3c_rack });
    }

    SECTION("another variant of the model with no memory of its own takes the model's last rack")
    {
        select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
        CHECK(select_printer(b, config, "Bambu Lab H3C 0.6 nozzle"));
        // The machine's nozzles, not the variant's: the extruder still carries the 0.4 it had.
        CHECK(b.effective_nozzle_diameters() == std::vector<double>{ 0.4 });
        CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
    }

    SECTION("a memory that no longer fits is dropped: the printer's own defaults")
    {
        config.set("nozzle_rack", "Bambu Lab H3C 0.4 nozzle", "{\"diameters\":[0.4],\"racks\":[\"0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4\"]}");
        select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
        CHECK_FALSE(select_printer(b, config, "Bambu Lab H3C 0.4 nozzle"));
        CHECK(project_racks(b).empty());
    }

    SECTION("the same model's variant switch keeps the rack and is remembered under the new name")
    {
        select_printer(b, config, "Bambu Lab H3C 0.4 nozzle");
        CHECK_FALSE(select_printer(b, config, "Bambu Lab H3C 0.2 nozzle"));
        CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
    }
}

// Project strip (tests/design_multi_project_sidebar.md risk 1): switching to another open project loads its nozzles as opening
// its 3MF does. That is not an edit of the printer's nozzles: while the switch runs, neither the rack memory nor the size memory
// is written, and no remembered rack is put over the project's own.
TEST_CASE("A project switch neither writes nor restores the nozzle memory", "[NozzleRackMemory][PresetBundle][ProjectSwapGuard]")
{
    PresetBundle     &b = bbl_bundle();
    AppConfig         config;
    const std::string h2c_rack = "0.4:high_flow,0.4,0.6,0.4,0.4,0.4";
    REQUIRE_FALSE(ProjectSwapGuard::active());

    select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
    select_printer(b, config, "Bambu Lab H2C 0.4 nozzle");
    REQUIRE(b.set_rack_nozzle_override(1, h2c_rack));
    {
        ProjectSwapGuard swap;
        b.remember_nozzle_rack(config);
        b.remember_nozzle_diameter_override(config);
        CHECK_FALSE(config.has("nozzle_rack", "Bambu Lab H2C 0.4 nozzle"));
        CHECK_FALSE(config.has("nozzle_rack_model", "Bambu Lab H2C"));
        CHECK_FALSE(config.has("nozzle_diameter_override", "Bambu Lab H2C"));
    }
    // A user edit after the switch is remembered as before.
    b.remember_nozzle_rack(config);
    CHECK(config.has("nozzle_rack", "Bambu Lab H2C 0.4 nozzle"));
    const std::string remembered = config.get("nozzle_rack", "Bambu Lab H2C 0.4 nozzle");

    // Another model, then back to the H2C during a switch: the memory is not put over the project.
    select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
    {
        ProjectSwapGuard swap;
        CHECK_FALSE(select_printer(b, config, "Bambu Lab H2C 0.4 nozzle"));
        CHECK(project_racks(b).empty());
    }
    CHECK(config.get("nozzle_rack", "Bambu Lab H2C 0.4 nozzle") == remembered);

    // Outside a switch the memory works as before.
    select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
    CHECK(select_printer(b, config, "Bambu Lab H2C 0.4 nozzle"));
    CHECK(project_racks(b) == std::vector<std::string>{ "", h2c_rack });
}
