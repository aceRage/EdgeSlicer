// The project's nozzles remembered per printer between sessions (NozzleRackMemory.hpp, owner bugs 2026-11-06: an H2C opened with
// its whole rack on the variant's size and an H3C with every slot 0.4 Standard on every start). The text form, the fit check
// against a printer model (rack slots, sizes and nozzle types from extruder_variant_list / default_nozzle_volume_type), and the
// round trip through the app config on this tree's BBL system profiles, as a printer switch or a restart does it.

#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BambuDualNozzleSync.hpp"
#include "libslic3r/NozzleOverride.hpp"
#include "libslic3r/NozzleRackMemory.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"
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
        const std::string ten = "{\"diameters\":[0.4],\"racks\":[\"0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4,0.4\"]}";
        SECTION("only the variant's own entry is bad: the model's entry is used")
        {
            config.set("nozzle_rack", "Bambu Lab H3C 0.4 nozzle", ten);
            select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
            CHECK(select_printer(b, config, "Bambu Lab H3C 0.4 nozzle"));
            CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
        }
        SECTION("only the model's entry is bad: the variant's own entry is used")
        {
            config.set("nozzle_rack_model", "Bambu Lab H3C", ten);
            select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
            CHECK(select_printer(b, config, "Bambu Lab H3C 0.4 nozzle"));
            CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
        }
        SECTION("both are bad")
        {
            config.set("nozzle_rack", "Bambu Lab H3C 0.4 nozzle", ten);
            config.set("nozzle_rack_model", "Bambu Lab H3C", ten);
            select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
            CHECK_FALSE(select_printer(b, config, "Bambu Lab H3C 0.4 nozzle"));
            CHECK(project_racks(b).empty());
        }
    }

    SECTION("the same model's variant switch keeps the rack and is remembered under the new name")
    {
        select_printer(b, config, "Bambu Lab H3C 0.4 nozzle");
        CHECK_FALSE(select_printer(b, config, "Bambu Lab H3C 0.2 nozzle"));
        CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
    }
}

// ---- Owner bugs 2026-11-07 (H3C): cross-printer rack bleed and "Sync from printer" on a single-extruder rack -----------------

TEST_CASE("Which remembered entry a printer preset tries first", "[NozzleRackMemory]")
{
    // A system variant is one machine with its model's other variants: the model's last rack wins over the variant's older entry.
    CHECK(nozzle_rack_memory_candidates(true, "name", "model") == std::vector<std::string>{ "model", "name" });
    // A user preset may be another machine of the model: its own entry first.
    CHECK(nozzle_rack_memory_candidates(false, "name", "model") == std::vector<std::string>{ "name", "model" });
    CHECK(nozzle_rack_memory_candidates(true, "", "model") == std::vector<std::string>{ "model" });
    CHECK(nozzle_rack_memory_candidates(false, "same", "same") == std::vector<std::string>{ "same" });
    CHECK(nozzle_rack_memory_candidates(true, "", "").empty());
}

TEST_CASE("H2S 0.8 then H3C: the H3C keeps its own rack", "[NozzleRackMemory][PresetBundle][BblH3C]")
{
    PresetBundle     &b = bbl_bundle();
    AppConfig         config;
    const std::string h3c_rack = "0.4,0.6,0.4,0.4,0.4,0.4,0.4,0.4:tpu285,0.4";

    // The owner's H3C: a rack set on the 0.4 variant, remembered.
    select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
    select_printer(b, config, "Bambu Lab H3C 0.4 nozzle");
    REQUIRE(b.set_rack_nozzle_override(0, h3c_rack));
    b.remember_nozzle_rack(config);
    b.remember_nozzle_diameter_override(config);
    const std::string h3c_model_entry = config.get("nozzle_rack_model", "Bambu Lab H3C");
    REQUIRE_FALSE(h3c_model_entry.empty());
    // An older entry under the 0.8 variant's own name: every slot 0.8, from a time the H3C sat on that variant.
    config.set("nozzle_rack", "Bambu Lab H3C 0.8 nozzle", "{\"diameters\":[0.8],\"types\":[0],\"racks\":[]}");

    // The H2S: 0.8 in the main nozzle-size selector (its 0.8 variant), remembered as the H2S's.
    select_printer(b, config, "Bambu Lab H2S 0.4 nozzle");
    CHECK(project_racks(b).empty()); // the H3C's rack does not come along
    select_printer(b, config, "Bambu Lab H2S 0.8 nozzle");
    CHECK(b.effective_nozzle_diameters() == std::vector<double>{ 0.8 });
    b.remember_nozzle_rack(config);
    b.remember_nozzle_diameter_override(config);
    // The H2S's 0.8 never changes what the H3C remembers.
    CHECK(config.get("nozzle_rack_model", "Bambu Lab H3C") == h3c_model_entry);
    CHECK(config.has("nozzle_rack_model", "Bambu Lab H2S"));

    SECTION("the printer combo opens the H3C on the variant of its own nozzle, not the H2S's 0.8")
    {
        // get_similar_printer_preset gives the H2S's variant; the remembered nozzles pick the H3C's own.
        CHECK(b.printer_preset_for_remembered_nozzles(config, "Bambu Lab H3C 0.8 nozzle") == "Bambu Lab H3C 0.4 nozzle");
        CHECK(select_printer(b, config, "Bambu Lab H3C 0.4 nozzle"));
        CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
        CHECK(b.effective_nozzle_diameters() == std::vector<double>{ 0.4 });
        CHECK(b.printer_variant_for_nozzle_sizes().empty()); // already on the 0.4 variant
    }
    SECTION("landing on the 0.8 variant anyway: the model's rack, not the variant's stale all-0.8 entry; the variant follows")
    {
        CHECK(select_printer(b, config, "Bambu Lab H3C 0.8 nozzle"));
        CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
        CHECK(b.effective_nozzle_diameters() == std::vector<double>{ 0.4 });
        CHECK(b.printer_variant_for_nozzle_sizes() == "Bambu Lab H3C 0.4 nozzle");
    }
    SECTION("back to the H2S, 0.6, back to the H3C: still its own rack")
    {
        select_printer(b, config, "Bambu Lab H3C 0.4 nozzle");
        select_printer(b, config, "Bambu Lab H2S 0.6 nozzle");
        b.remember_nozzle_rack(config);
        CHECK(b.printer_preset_for_remembered_nozzles(config, "Bambu Lab H3C 0.6 nozzle") == "Bambu Lab H3C 0.4 nozzle");
        CHECK(select_printer(b, config, "Bambu Lab H3C 0.6 nozzle"));
        CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
        CHECK(b.effective_nozzle_diameters() == std::vector<double>{ 0.4 });
    }
    SECTION("a nozzle change handled before the Printer tab caught up stores nothing under the new model")
    {
        // The H2S project (0.8 variant, a 0.6 override) is still in place when the H3C preset is already selected.
        REQUIRE(b.set_nozzle_diameter_override({ 0.6 }));
        b.remember_nozzle_diameter_override(config);
        const std::string h2s_sizes = config.get("nozzle_diameter_override", "Bambu Lab H2S");
        const bool        had_h3c   = config.has("nozzle_diameter_override", "Bambu Lab H3C");
        const std::string h3c_sizes = had_h3c ? config.get("nozzle_diameter_override", "Bambu Lab H3C") : std::string();
        REQUIRE(b.printers.select_preset_by_name("Bambu Lab H3C 0.4 nozzle", true));
        b.remember_nozzle_diameter_override(config);
        b.remember_nozzle_rack(config);
        CHECK(config.has("nozzle_diameter_override", "Bambu Lab H3C") == had_h3c);
        if (had_h3c)
            CHECK(config.get("nozzle_diameter_override", "Bambu Lab H3C") == h3c_sizes);
        CHECK(config.get("nozzle_rack_model", "Bambu Lab H3C") == h3c_model_entry);
        CHECK(config.get("nozzle_diameter_override", "Bambu Lab H2S") == h2s_sizes);
        // The Printer tab's update then brings the H3C's own nozzles.
        CHECK(b.update_nozzle_diameter_override_for_printer(config));
        CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
    }
    SECTION("a restart after the switches: the H3C's rack")
    {
        AppConfig restarted;
        for (const char *section : { "nozzle_rack", "nozzle_rack_model", "nozzle_diameter_override" })
            for (const auto &kv : config.get_section(section))
                restarted.set(section, kv.first, kv.second);
        b.project_config.option<ConfigOptionStrings>("extruder_rack_nozzle_override", true)->values.clear();
        select_printer(b, restarted, "Bambu Lab X1 Carbon 0.4 nozzle");
        CHECK(select_printer(b, restarted, "Bambu Lab H3C 0.4 nozzle"));
        CHECK(project_racks(b) == std::vector<std::string>{ h3c_rack });
    }
}

TEST_CASE("H3C Sync from printer: all nine slots with their sizes and types", "[NozzleRackMemory][Sync][BblH3C]")
{
    // What an H3C reports: its mounted nozzle (pos 0) and rack positions 0x10 + n, in any order.
    DualNozzleSync::PrinterState state;
    state.physical_extruder_map = { 0 };
    auto add = [&state](int pos, const char *d, NozzleVolumeType t, bool normal = true) {
        DualNozzleSync::PrinterNozzle n;
        n.pos      = pos;
        n.diameter = d;
        n.volume   = t;
        n.normal   = normal;
        state.nozzles.push_back(n);
    };
    add(0, "0.4", nvtStandard);
    add(0x13, "0.2", nvtStandard);
    add(0x10, "0.6", nvtHighFlow);
    add(0x11, "0.4", nvtTPU285);
    add(0x12, "0.8", nvtStandard, false); // not usable
    add(0x14, "0.4", nvtStandard);
    add(0x15, "0.4", nvtStandard);
    add(0x16, "0.4", nvtHighFlow);
    add(0x17, "0.4", nvtStandard);
    add(0x18, "0.6", nvtStandard);

    const std::vector<std::vector<RackNozzle>> layout = reported_rack_nozzles(state, 1);
    REQUIRE(layout.size() == 1);
    REQUIRE(layout[0].size() == 9);
    CHECK(reported_rack_layout(state, 1) == std::vector<std::vector<double>>{ { 0.4, 0.6, 0.4, 0.2, 0.4, 0.4, 0.4, 0.4, 0.6 } });

    const std::string synced = "0.4,0.6:high_flow,0.4:tpu285,0.2,0.4,0.4,0.4:high_flow,0.4,0.6";
    CHECK(rack_override_from_reported(layout, { "" }, { int(nvtStandard) }, { 9 }) == std::vector<std::string>{ synced });
    // On a High Flow extruder an unannotated slot is High Flow: Standard slots name their type.
    CHECK(rack_override_from_reported(layout, { "" }, { int(nvtHighFlow) }, { 9 }) ==
          std::vector<std::string>{ "0.4:standard,0.6,0.4:tpu285,0.2:standard,0.4:standard,0.4:standard,0.4,0.4:standard,0.6:standard" });
    // No more slots than the profile has; an extruder without a rack keeps its text.
    CHECK(rack_override_from_reported(layout, { "" }, { int(nvtStandard) }, { 3 }) == std::vector<std::string>{ "0.4,0.6:high_flow,0.4:tpu285" });
    CHECK(rack_override_from_reported(layout, { "old" }, { int(nvtStandard) }, { 1 }) == std::vector<std::string>{ "old" });
    // A report without rack nozzles changes nothing.
    DualNozzleSync::PrinterState mounted_only;
    mounted_only.physical_extruder_map = { 0 };
    mounted_only.nozzles.push_back(state.nozzles.front());
    CHECK(rack_override_from_reported(reported_rack_nozzles(mounted_only, 1), { "" }, { int(nvtStandard) }, { 9 }).empty());

    // The synced rack fits the H3C, is remembered and comes back after a printer switch.
    PresetBundle &b = bbl_bundle();
    AppConfig     config;
    select_printer(b, config, "Bambu Lab X1 Carbon 0.4 nozzle");
    select_printer(b, config, "Bambu Lab H3C 0.4 nozzle");
    REQUIRE(b.nozzle_rack_limits().capacity == std::vector<int>{ 9 });
    REQUIRE(b.set_rack_nozzle_override(rack_override_from_reported(layout, { b.rack_nozzle_override(0) }, { int(nvtStandard) },
                                                                   b.nozzle_rack_limits().capacity)));
    CHECK(validate_nozzle_rack_memory(b.current_nozzle_rack_memory(), b.nozzle_rack_limits()).empty());
    b.remember_nozzle_rack(config);
    select_printer(b, config, "Bambu Lab H2S 0.8 nozzle");
    CHECK(select_printer(b, config, "Bambu Lab H3C 0.4 nozzle"));
    CHECK(project_racks(b) == std::vector<std::string>{ synced });
}
