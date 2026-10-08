// Snapmaker U1 profile guards, run against the shipped resources/profiles tree.
//
//  * A new install must land on the 0.4 mm U1 with 0.4 on all four heads. The Setup Wizard and the Add
//    Printer dialog enable every nozzle variant of the model and then activate "the first variant of the
//    sorted set" - which is "0.2" - unless PresetBundle::default_printer_variant picks the default.
//  * The mixed-nozzle machine "Snapmaker U1 (0.4+0.6 nozzle)" (OrcaSlicer #12824) is a real, selectable
//    variant of the model, with 0.4 on heads 1-2 and 0.6 on heads 3-4, and filaments for both sizes.
//  * Pressure advance stays OFF in the Snapmaker-brand U1 filaments added with it. A filament with its own
//    pressure advance stops the U1's dynamic flow calibration from taking effect (owner rule).

#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

constexpr const char *U1_MODEL = "Snapmaker U1";

// The Snapmaker vendor (with OrcaFilamentLibrary behind it) loaded from this tree's resources/profiles.
// Built once: it is read-only for every case below.
struct SnapmakerTree
{
    std::string                  saved_data_dir;
    fs::path                     scratch;
    std::unique_ptr<PresetBundle> library;
    std::unique_ptr<PresetBundle> bundle;

    SnapmakerTree()
    {
        saved_data_dir = data_dir();
        // Keep anything the loader writes (preset caches) out of the user's data directory.
        scratch = fs::temp_directory_path() / "snorca_tests" / fs::unique_path("u1_profiles_%%%%%%%%");
        fs::create_directories(scratch);
        set_data_dir(scratch.string());
        const std::string profiles = (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
        library = std::make_unique<PresetBundle>();
        library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                               ForwardCompatibilitySubstitutionRule::EnableSilent);
        bundle = std::make_unique<PresetBundle>();
        bundle->load_vendor_configs_from_json(profiles, PresetBundle::SM_BUNDLE, PresetBundle::LoadSystem,
                                              ForwardCompatibilitySubstitutionRule::EnableSilent, library.get());
    }
    ~SnapmakerTree()
    {
        set_data_dir(saved_data_dir);
        boost::system::error_code ec;
        fs::remove_all(scratch, ec);
    }

    std::set<std::string> u1_variants() const
    {
        std::set<std::string> out;
        const auto            vit = bundle->vendors.find(PresetBundle::SM_BUNDLE);
        REQUIRE(vit != bundle->vendors.end());
        for (const VendorProfile::PrinterModel &model : vit->second.models)
            if (model.name == U1_MODEL)
                for (const VendorProfile::PrinterVariant &variant : model.variants)
                    out.insert(variant.name);
        return out;
    }

    // What the wizard writes to app_config for a model it enables: every nozzle variant.
    AppConfig wizard_enabled_u1() const
    {
        AppConfig config;
        for (const std::string &variant : u1_variants())
            config.set_variant(PresetBundle::SM_BUNDLE, U1_MODEL, variant, true);
        return config;
    }
};

SnapmakerTree &tree()
{
    static SnapmakerTree instance;
    return instance;
}

std::vector<double> nozzle_diameters(const Preset &printer)
{
    const auto *opt = printer.config.option<ConfigOptionFloats>("nozzle_diameter");
    REQUIRE(opt != nullptr);
    return opt->values;
}

} // namespace

TEST_CASE("default_printer_variant prefers the 0.4 nozzle", "[Preset][U1]")
{
    CHECK(PresetBundle::default_printer_variant({}) == "");
    CHECK(PresetBundle::default_printer_variant({ "0.2", "0.4", "0.6", "0.8" }) == "0.4");
    CHECK(PresetBundle::default_printer_variant({ "0.2", "0.4", "0.4+0.6", "0.6", "0.8" }) == "0.4");
    // No 0.4 in the set: fall back to the first one, as the pickers always did.
    CHECK(PresetBundle::default_printer_variant({ "0.2", "0.6" }) == "0.2");
    CHECK(PresetBundle::default_printer_variant({ "0.6" }) == "0.6");
    // "0.4HF" is not the plain 0.4 default.
    CHECK(PresetBundle::default_printer_variant({ "0.4HF", "0.6" }) == "0.4HF");
}

TEST_CASE("The Snapmaker U1 model offers every nozzle variant, 0.4 default among them", "[Preset][U1]")
{
    const std::set<std::string> variants = tree().u1_variants();
    for (const char *wanted : { "0.2", "0.4", "0.6", "0.8", "0.4+0.6" })
        CHECK(variants.count(wanted) == 1);
    // The sort order is what made "0.2" the activated variant.
    CHECK(*variants.begin() == "0.2");
    CHECK(PresetBundle::default_printer_variant(variants) == PresetBundle::SM_DEFAULT_PRINTER_VARIANT);
}

TEST_CASE("A new install that enables the U1 activates the 0.4 machine with four 0.4 heads", "[Preset][U1]")
{
    SnapmakerTree &t = tree();
    AppConfig      config = t.wizard_enabled_u1();

    // The wizard's choice (WebGuideDialog / WebPresetDialog / ConfigWizard::apply_config).
    const std::string variant = PresetBundle::default_printer_variant(t.u1_variants());
    REQUIRE(variant == "0.4");

    PresetBundle::PresetPreferences preferred;
    preferred.printer_model_id = U1_MODEL;
    preferred.printer_variant  = variant;
    t.bundle->load_selections(config, preferred);

    const Preset &selected = t.bundle->printers.get_selected_preset();
    CHECK(selected.name == "Snapmaker U1 (0.4 nozzle)");
    CHECK(selected.config.opt_string("printer_variant") == "0.4");
    CHECK(nozzle_diameters(selected) == std::vector<double>{ 0.4, 0.4, 0.4, 0.4 });
    CHECK(nozzle_diameters(t.bundle->printers.get_edited_preset()) == std::vector<double>{ 0.4, 0.4, 0.4, 0.4 });
}

TEST_CASE("The 0.2, 0.6, 0.8 and mixed U1 machines stay selectable", "[Preset][U1]")
{
    SnapmakerTree &t = tree();
    AppConfig      config = t.wizard_enabled_u1();

    struct Expect { const char *variant; const char *name; std::vector<double> nozzles; };
    const Expect expected[] = {
        { "0.2",     "Snapmaker U1 (0.2 nozzle)",     { 0.2, 0.2, 0.2, 0.2 } },
        { "0.4",     "Snapmaker U1 (0.4 nozzle)",     { 0.4, 0.4, 0.4, 0.4 } },
        { "0.6",     "Snapmaker U1 (0.6 nozzle)",     { 0.6, 0.6, 0.6, 0.6 } },
        { "0.8",     "Snapmaker U1 (0.8 nozzle)",     { 0.8, 0.8, 0.8, 0.8 } },
        { "0.4+0.6", "Snapmaker U1 (0.4+0.6 nozzle)", { 0.4, 0.4, 0.6, 0.6 } },
    };
    for (const Expect &e : expected) {
        INFO(e.variant);
        PresetBundle::PresetPreferences preferred;
        preferred.printer_model_id = U1_MODEL;
        preferred.printer_variant  = e.variant;
        t.bundle->load_selections(config, preferred);
        const Preset &selected = t.bundle->printers.get_selected_preset();
        CHECK(selected.name == e.name);
        CHECK(nozzle_diameters(selected) == e.nozzles);
        CHECK(selected.is_visible);
    }
}

TEST_CASE("The mixed U1 machine carries per-head layer limits and no High Flow", "[Preset][U1]")
{
    SnapmakerTree &t     = tree();
    const Preset  *mixed = t.bundle->printers.find_preset("Snapmaker U1 (0.4+0.6 nozzle)", false);
    REQUIRE(mixed != nullptr);
    CHECK(mixed->is_system);
    CHECK(mixed->config.opt_string("printer_model") == U1_MODEL);
    CHECK(mixed->config.opt_string("printer_variant") == "0.4+0.6");
    CHECK(supports_mixed_nozzle_diameters(mixed->config));
    CHECK(has_mixed_nozzle_diameters(mixed->config));
    const auto *min_h = mixed->config.option<ConfigOptionFloats>("min_layer_height");
    const auto *max_h = mixed->config.option<ConfigOptionFloats>("max_layer_height");
    REQUIRE(min_h != nullptr);
    REQUIRE(max_h != nullptr);
    CHECK(min_h->values == std::vector<double>{ 0.08, 0.08, 0.12, 0.12 });
    CHECK(max_h->values == std::vector<double>{ 0.32, 0.32, 0.42, 0.42 });
    // A 0.6 head cannot be High Flow here, so the machine does not advertise it.
    const auto *flow = mixed->config.option<ConfigOptionStrings>("printer_flow_support");
    CHECK((flow == nullptr || flow->values.empty() ||
           std::find(flow->values.begin(), flow->values.end(), "high_flow") == flow->values.end()));
}

namespace {

const std::vector<std::string> NEW_SNAPMAKER_U1_FILAMENTS = {
    "Snapmaker PET @U1",       "Snapmaker PETG @U1",       "Snapmaker PLA @U1",
    "Snapmaker PLA Eco @U1",   "Snapmaker PLA Lite @U1",   "Snapmaker PLA Metal @U1",
    "Snapmaker TPE @U1",       "Snapmaker TPU 95A @U1",    "Snapmaker TPU High-Flow @U1",
};

const Preset &filament_preset(const char *name)
{
    const Preset *p = tree().bundle->filaments.find_preset(name, false);
    INFO(name);
    REQUIRE(p != nullptr);
    return *p;
}

} // namespace

TEST_CASE("The Snapmaker-brand U1 filaments are present, compatible with the U1 and keep pressure advance off", "[Preset][U1]")
{
    for (const std::string &name : NEW_SNAPMAKER_U1_FILAMENTS) {
        INFO(name);
        const Preset &preset = filament_preset(name.c_str());
        CHECK(preset.is_system);
        const auto *compatible = preset.config.option<ConfigOptionStrings>("compatible_printers");
        REQUIRE(compatible != nullptr);
        auto lists = [&](const char *printer) {
            return std::find(compatible->values.begin(), compatible->values.end(), printer) != compatible->values.end();
        };
        CHECK(lists("Snapmaker U1 (0.4 nozzle)"));
        CHECK(lists("Snapmaker U1 (0.4+0.6 nozzle)"));

        // Pressure advance stays OFF in every slot (standard and High Flow): a filament that carries its
        // own pressure advance stops the U1's dynamic flow calibration from taking effect.
        const auto *pa = preset.config.option<ConfigOptionBools>("enable_pressure_advance");
        REQUIRE(pa != nullptr);
        REQUIRE(!pa->values.empty());
        for (unsigned char on : pa->values)
            CHECK(on == 0);
        // ... and no pressure-advance command hides in the filament G-code.
        for (const char *key : { "filament_start_gcode", "filament_end_gcode" }) {
            const auto *gcode = preset.config.option<ConfigOptionStrings>(key);
            if (gcode == nullptr)
                continue;
            for (const std::string &g : gcode->values) {
                CHECK(g.find("PRESSURE_ADVANCE") == std::string::npos);
                CHECK(g.find("M900") == std::string::npos);
            }
        }
        const auto *adaptive = preset.config.option<ConfigOptionBools>("adaptive_pressure_advance");
        if (adaptive != nullptr)
            for (unsigned char on : adaptive->values)
                CHECK(on == 0);

        // High-Flow aware like the other 0.4 U1 filaments.
        const auto *flow = preset.config.option<ConfigOptionStrings>("filament_flow_support");
        REQUIRE(flow != nullptr);
        CHECK(flow->values == std::vector<std::string>{ "standard", "high_flow" });
    }
}

TEST_CASE("The new Snapmaker-brand U1 filaments resolve to real material settings", "[Preset][U1]")
{
    // Temperatures are integer vectors; a null option here would be a crash, so fail loudly instead.
    auto first = [](const Preset &p, const char *key) {
        const auto *opt = p.config.option<ConfigOptionInts>(key);
        REQUIRE(opt != nullptr);
        REQUIRE(!opt->values.empty());
        return double(opt->values.front());
    };
    auto type = [](const Preset &p) {
        const auto *opt = p.config.option<ConfigOptionStrings>("filament_type");
        REQUIRE(opt != nullptr);
        REQUIRE(!opt->values.empty());
        return opt->values.front();
    };

    // A thin parent chain would leave these at PrintConfig defaults (the mistake this guards against).
    CHECK(type(filament_preset("Snapmaker PLA @U1")) == "PLA");
    CHECK(type(filament_preset("Snapmaker PLA Metal @U1")) == "PLA");
    CHECK(type(filament_preset("Snapmaker PLA Lite @U1")) == "PLA");
    CHECK(type(filament_preset("Snapmaker PLA Eco @U1")) == "PLA");
    CHECK(type(filament_preset("Snapmaker PETG @U1")) == "PETG");
    CHECK(type(filament_preset("Snapmaker PET @U1")) == "PET");
    CHECK(type(filament_preset("Snapmaker TPE @U1")) == "TPU");
    CHECK(type(filament_preset("Snapmaker TPU 95A @U1")) == "TPU");
    CHECK(type(filament_preset("Snapmaker TPU High-Flow @U1")) == "TPU");
    // Upstream's Snapmaker TPU base prints at 225 C (the library's generic TPU would be 240).
    CHECK(first(filament_preset("Snapmaker TPE @U1"), "nozzle_temperature") == Approx(225.));
    CHECK(first(filament_preset("Snapmaker TPU High-Flow @U1"), "nozzle_temperature") == Approx(225.));
    CHECK(first(filament_preset("Snapmaker PET @U1"), "nozzle_temperature") == Approx(278.));
    CHECK(first(filament_preset("Snapmaker PLA @U1"), "nozzle_temperature") == Approx(220.));
    CHECK(first(filament_preset("Snapmaker PETG @U1"), "nozzle_temperature") == Approx(245.));
    CHECK(first(filament_preset("Snapmaker PETG @U1"), "nozzle_temperature_initial_layer") >= 250.);
    // TPU High-Flow is the one that also fits the 0.6 and 0.8 machines.
    const auto *compatible = filament_preset("Snapmaker TPU High-Flow @U1").config.option<ConfigOptionStrings>("compatible_printers");
    CHECK(std::find(compatible->values.begin(), compatible->values.end(), "Snapmaker U1 (0.8 nozzle)") != compatible->values.end());
}

TEST_CASE("On the mixed U1 each head only offers filaments cut for its own nozzle", "[Preset][U1][MixedNozzle]")
{
    SnapmakerTree &t = tree();
    AppConfig      config = t.wizard_enabled_u1();
    PresetBundle::PresetPreferences preferred;
    preferred.printer_model_id = U1_MODEL;
    preferred.printer_variant  = "0.4+0.6";
    t.bundle->load_selections(config, preferred);
    REQUIRE(t.bundle->printers.get_selected_preset().name == "Snapmaker U1 (0.4+0.6 nozzle)");

    const Preset &for_06 = filament_preset("Snapmaker PLA SnapSpeed @U1 0.6 nozzle");
    const Preset &for_04 = filament_preset("Snapmaker PLA @U1");
    CHECK(filament_preset_nozzle_diameter(for_04, t.bundle->printers) == Approx(0.4));
    CHECK(filament_preset_nozzle_diameter(for_06, t.bundle->printers) == Approx(0.6));
    // Filament slots are 1-based; heads 1-2 are 0.4, heads 3-4 are 0.6.
    for (unsigned slot : { 1u, 2u }) {
        CHECK(filament_preset_fits_slot(for_04, t.bundle->printers, slot));
        CHECK_FALSE(filament_preset_fits_slot(for_06, t.bundle->printers, slot));
    }
    for (unsigned slot : { 3u, 4u }) {
        CHECK_FALSE(filament_preset_fits_slot(for_04, t.bundle->printers, slot));
        CHECK(filament_preset_fits_slot(for_06, t.bundle->printers, slot));
    }
}

TEST_CASE("The mixed U1 process is compatible with the mixed machine only and leaves the prime tower off", "[Preset][U1][MixedNozzle]")
{
    const Preset *process = tree().bundle->prints.find_preset("0.20mm Standard @Snapmaker U1 (0.4+0.6 nozzle)", false);
    REQUIRE(process != nullptr);
    const auto *compatible = process->config.option<ConfigOptionStrings>("compatible_printers");
    REQUIRE(compatible != nullptr);
    CHECK(compatible->values == std::vector<std::string>{ "Snapmaker U1 (0.4+0.6 nozzle)" });
    // Print::validate refuses a prime tower across different nozzle diameters, so the process must not
    // turn it on (a two-colour job on the mixed machine would otherwise refuse to slice).
    CHECK_FALSE(process->config.opt_bool("enable_prime_tower"));
    // Percent line widths follow each tool's own nozzle.
    CHECK(process->config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent);
    // The 0.4 U1 processes do not list the mixed machine.
    const Preset *p04 = tree().bundle->prints.find_preset("0.20mm Standard @Snapmaker U1 (0.4 nozzle)", false);
    REQUIRE(p04 != nullptr);
    const auto *c04 = p04->config.option<ConfigOptionStrings>("compatible_printers");
    CHECK(std::find(c04->values.begin(), c04->values.end(), "Snapmaker U1 (0.4+0.6 nozzle)") == c04->values.end());
}

TEST_CASE("Every U1 filament that fits the 0.4 or 0.6 machine also fits the mixed machine", "[Preset][U1][MixedNozzle]")
{
    SnapmakerTree &t = tree();
    size_t         checked = 0;
    for (const Preset &preset : t.bundle->filaments) {
        if (!preset.is_system || preset.name.find("@U1") == std::string::npos)
            continue;
        const auto *compatible = preset.config.option<ConfigOptionStrings>("compatible_printers");
        if (compatible == nullptr || compatible->values.empty())
            continue;
        const auto &v = compatible->values;
        const bool  fits_04_or_06 = std::find(v.begin(), v.end(), "Snapmaker U1 (0.4 nozzle)") != v.end() ||
                                    std::find(v.begin(), v.end(), "Snapmaker U1 (0.6 nozzle)") != v.end();
        if (!fits_04_or_06)
            continue;
        ++checked;
        INFO(preset.name);
        CHECK(std::find(v.begin(), v.end(), "Snapmaker U1 (0.4+0.6 nozzle)") != v.end());
    }
    CHECK(checked > 100);
}
