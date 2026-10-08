// System printer presets in Bambu's per-extruder-variant layout keep every variant on load.
//
// printer_extruder_id / printer_extruder_variant list one slot per (extruder, variant) pair and the
// retraction settings carry one value per slot: upstream Orca's Custom MyToolChanger has 5 extruders x
// 3 variants = 15 values. Preset::normalize used to call set_num_extruders(5), which cut every one of
// those vectors to its first 5 values - extruder 1's three variants and two of extruder 2's - and lost
// the rest. The slicer now resolves the slots to one value per tool when a print is applied
// (resolve_printer_extruder_variants).

#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = boost::filesystem;
using namespace Slic3r;

namespace {

const std::vector<std::string> VARIANTS3 = { "Direct Drive Standard", "Direct Drive High Flow", "Direct Drive Extra High Flow" };

// 5 extruders x 3 variants, every slot a different value: extruder e (1-based), variant v (0-based)
// retracts e + v / 10 mm.
std::vector<double> distinct_retractions(size_t extruders, size_t variants)
{
    std::vector<double> out;
    for (size_t e = 1; e <= extruders; ++e)
        for (size_t v = 0; v < variants; ++v)
            out.push_back(double(e) + double(v) / 10.);
    return out;
}

// A printer preset's keys at their defaults.
DynamicPrintConfig printer_defaults()
{
    std::unique_ptr<DynamicPrintConfig> config(DynamicPrintConfig::new_from_defaults_keys(Preset::printer_options()));
    return *config;
}

DynamicPrintConfig toolchanger_config(size_t extruders, const std::vector<std::string> &variants)
{
    DynamicPrintConfig config = printer_defaults();
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(extruders, 0.4)));
    std::vector<int>         ids;
    std::vector<std::string> names;
    for (size_t e = 0; e < extruders; ++e)
        for (const std::string &v : variants) {
            ids.push_back(int(e + 1));
            names.push_back(v);
        }
    std::string list;
    for (const std::string &v : variants)
        list += (list.empty() ? "" : ",") + v;
    config.set_key_value("printer_extruder_id", new ConfigOptionInts(ids));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings(names));
    config.set_key_value("extruder_variant_list", new ConfigOptionStrings(std::vector<std::string>(extruders, list)));
    config.set_key_value("retraction_length", new ConfigOptionFloats(distinct_retractions(extruders, variants.size())));
    std::vector<double> z_hop = distinct_retractions(extruders, variants.size());
    for (double &z : z_hop)
        z /= 10.;
    config.set_key_value("z_hop", new ConfigOptionFloats(z_hop));
    return config;
}

std::vector<double> floats(const DynamicPrintConfig &config, const char *key) { return config.option<ConfigOptionFloats>(key)->values; }
std::vector<int>    ints(const DynamicPrintConfig &config, const char *key) { return config.option<ConfigOptionInts>(key)->values; }

// A scratch vendor bundle that removes itself. Never points at a user's data directory.
struct ScratchVendor
{
    fs::path    root;
    std::string saved_data_dir;

    ScratchVendor()
    {
        root = fs::temp_directory_path() / fs::unique_path("orca_variant_vendor_%%%%%%%%");
        fs::create_directories(root / "data");
        saved_data_dir = data_dir();
        set_data_dir((root / "data").string());
    }
    ~ScratchVendor()
    {
        set_data_dir(saved_data_dir);
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const std::string &rel, const std::string &content) const
    {
        const fs::path p = root / "profiles" / rel;
        fs::create_directories(p.parent_path());
        std::ofstream ofs(p.string(), std::ios::binary);
        ofs << content;
    }
};

std::string json_list(const std::vector<std::string> &values)
{
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i)
        out += (i ? ", \"" : "\"") + values[i] + "\"";
    return out + "]";
}

template<class T> std::string json_numbers(const std::vector<T> &values)
{
    std::vector<std::string> s;
    for (const T &v : values)
        s.push_back(float_to_string_decimal_point(double(v)));
    return json_list(s);
}

// The vendor "VariantTest": a 5 x 3 per-variant toolchanger and a plain 5-extruder toolchanger
// (no variant layout) with a stale 7-value retraction vector and a single z_hop.
void write_variant_vendor(const ScratchVendor &vendor)
{
    vendor.write("VariantTest.json", R"({
    "name": "VariantTest",
    "version": "01.00.00.00",
    "force_update": "0",
    "description": "per-variant layout test vendor",
    "machine_model_list": [ { "name": "VT Toolchanger", "sub_path": "machine/VT Toolchanger.json" } ],
    "process_list": [],
    "filament_list": [],
    "machine_list": [
        { "name": "fdm_vt_common", "sub_path": "machine/fdm_vt_common.json" },
        { "name": "VT Toolchanger 0.4 nozzle", "sub_path": "machine/VT Toolchanger 0.4 nozzle.json" },
        { "name": "VT Plain 0.4 nozzle", "sub_path": "machine/VT Plain 0.4 nozzle.json" }
    ]
})");
    vendor.write("VariantTest/machine/VT Toolchanger.json", R"({
    "type": "machine_model",
    "name": "VT Toolchanger",
    "model_id": "vt_toolchanger",
    "nozzle_diameter": "0.4",
    "machine_tech": "FFF",
    "family": "VariantTest",
    "bed_model": "",
    "bed_texture": "",
    "hotend_model": "",
    "default_materials": ""
})");
    vendor.write("VariantTest/machine/fdm_vt_common.json", R"({
    "type": "machine",
    "name": "fdm_vt_common",
    "from": "system",
    "instantiation": "false",
    "single_extruder_multi_material": "0",
    "nozzle_diameter": ["0.4", "0.4", "0.4", "0.4", "0.4"]
})");

    std::vector<std::string> ids, names;
    for (int e = 1; e <= 5; ++e)
        for (const std::string &v : VARIANTS3) {
            ids.push_back(std::to_string(e));
            names.push_back(v);
        }
    std::string list = VARIANTS3[0] + "," + VARIANTS3[1] + "," + VARIANTS3[2];
    vendor.write("VariantTest/machine/VT Toolchanger 0.4 nozzle.json", std::string(R"({
    "type": "machine",
    "name": "VT Toolchanger 0.4 nozzle",
    "inherits": "fdm_vt_common",
    "from": "system",
    "setting_id": "VT_001",
    "instantiation": "true",
    "printer_model": "VT Toolchanger",
    "printer_variant": "0.4",
    "printer_extruder_id": )") + json_list(ids) + R"(,
    "printer_extruder_variant": )" + json_list(names) + R"(,
    "extruder_variant_list": )" + json_list(std::vector<std::string>(5, list)) + R"(,
    "retraction_length": )" + json_numbers(distinct_retractions(5, 3)) + R"(
})");
    vendor.write("VariantTest/machine/VT Plain 0.4 nozzle.json", R"({
    "type": "machine",
    "name": "VT Plain 0.4 nozzle",
    "inherits": "fdm_vt_common",
    "from": "system",
    "setting_id": "VT_002",
    "instantiation": "true",
    "printer_model": "VT Toolchanger",
    "printer_variant": "0.4",
    "printer_extruder_id": ["1", "2", "3", "4", "5"],
    "printer_extruder_variant": ["Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard"],
    "retraction_length": ["1", "2", "3", "4", "5", "6", "7"],
    "z_hop": ["0.4"]
})");
}

} // namespace

TEST_CASE("printer_extruder_variant_slots reads the per-variant layout", "[ExtruderVariant][VariantNormalize]")
{
    DynamicPrintConfig config = toolchanger_config(5, VARIANTS3);
    const auto         slots  = printer_extruder_variant_slots(config);
    REQUIRE(slots.size() == 5);
    CHECK(slots[0] == std::vector<size_t>{ 0, 1, 2 });
    CHECK(slots[4] == std::vector<size_t>{ 12, 13, 14 });

    SECTION("one variant per extruder is not a per-variant layout") {
        DynamicPrintConfig plain = toolchanger_config(5, { "Direct Drive Standard" });
        CHECK(printer_extruder_variant_slots(plain).empty());
    }
    SECTION("ids out of order are not a layout") {
        config.option<ConfigOptionInts>("printer_extruder_id")->values[3] = 3;
        CHECK(printer_extruder_variant_slots(config).empty());
    }
    SECTION("ids and variants of different sizes are not a layout") {
        config.option<ConfigOptionStrings>("printer_extruder_variant")->values.pop_back();
        CHECK(printer_extruder_variant_slots(config).empty());
    }
}

TEST_CASE("Preset::normalize keeps every variant of a per-variant toolchanger", "[ExtruderVariant][VariantNormalize]")
{
    DynamicPrintConfig config = toolchanger_config(5, VARIANTS3);
    // A per-extruder key the profile does not set still grows to the extruder count.
    config.set_key_value("retract_restart_extra", new ConfigOptionFloats({ 0.25 }));
    Preset::normalize(config);

    CHECK(floats(config, "retraction_length") == distinct_retractions(5, 3));
    CHECK(floats(config, "z_hop").size() == 15);
    CHECK_THAT(floats(config, "z_hop")[14], Catch::Matchers::WithinAbs(0.52, 1e-9));
    CHECK(ints(config, "printer_extruder_id").size() == 15);
    CHECK(config.option<ConfigOptionStrings>("printer_extruder_variant")->values.size() == 15);
    CHECK(floats(config, "nozzle_diameter").size() == 5);
    CHECK(floats(config, "retract_restart_extra") == std::vector<double>(5, 0.25));
}

TEST_CASE("Preset::normalize still sizes a single-variant printer to its extruder count", "[ExtruderVariant][VariantNormalize]")
{
    DynamicPrintConfig config = toolchanger_config(5, { "Direct Drive Standard" });
    config.set_key_value("retraction_length", new ConfigOptionFloats({ 1., 2., 3., 4., 5., 6., 7. }));
    config.set_key_value("z_hop", new ConfigOptionFloats({ 0.4 }));
    Preset::normalize(config);

    CHECK(floats(config, "retraction_length") == std::vector<double>{ 1., 2., 3., 4., 5. });
    CHECK(floats(config, "z_hop") == std::vector<double>(5, 0.4));
    CHECK(ints(config, "printer_extruder_id") == std::vector<int>{ 1, 2, 3, 4, 5 });

    SECTION("and a printer without any variant lists") {
        DynamicPrintConfig bare = printer_defaults();
        bare.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
        bare.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6 }));
        bare.set_key_value("retraction_length", new ConfigOptionFloats({ 0.8, 1.2, 2.0 }));
        Preset::normalize(bare);
        CHECK(floats(bare, "retraction_length") == std::vector<double>{ 0.8, 1.2 });
    }
}

TEST_CASE("set_num_extruders resizes per-variant vectors one extruder at a time", "[ExtruderVariant][VariantNormalize]")
{
    DynamicPrintConfig config = toolchanger_config(5, VARIANTS3);

    SECTION("fewer extruders drop whole extruders") {
        config.set_num_extruders(2);
        CHECK(floats(config, "retraction_length") == std::vector<double>{ 1.0, 1.1, 1.2, 2.0, 2.1, 2.2 });
        CHECK(ints(config, "printer_extruder_id") == std::vector<int>{ 1, 1, 1, 2, 2, 2 });
        CHECK(config.option<ConfigOptionStrings>("printer_extruder_variant")->values.size() == 6);
        CHECK(config.option<ConfigOptionStrings>("extruder_variant_list")->values.size() == 2);
        CHECK(floats(config, "nozzle_diameter").size() == 2);
    }
    SECTION("an added extruder copies extruder 1's variants") {
        config.set_num_extruders(6);
        std::vector<double> expected = distinct_retractions(5, 3);
        expected.insert(expected.end(), { 1.0, 1.1, 1.2 });
        CHECK(floats(config, "retraction_length") == expected);
        const std::vector<int> ids = ints(config, "printer_extruder_id");
        REQUIRE(ids.size() == 18);
        CHECK(std::vector<int>(ids.end() - 3, ids.end()) == std::vector<int>{ 6, 6, 6 });
        CHECK(config.option<ConfigOptionStrings>("printer_extruder_variant")->values[17] == VARIANTS3[2]);
        CHECK(printer_extruder_variant_slots(config).size() == 6);
    }
    SECTION("the same count keeps every slot") {
        config.set_num_extruders(5);
        CHECK(floats(config, "retraction_length") == distinct_retractions(5, 3));
        CHECK(ints(config, "printer_extruder_id").size() == 15);
        // ...and is idempotent.
        const DynamicPrintConfig once = config;
        config.set_num_extruders(5);
        CHECK(config.equals(once));
    }
}

TEST_CASE("resolve_printer_extruder_variants gives each tool its own variant", "[ExtruderVariant][VariantNormalize]")
{
    DynamicPrintConfig config = toolchanger_config(5, VARIANTS3);

    SECTION("Standard nozzles read each extruder's Standard slot") {
        resolve_printer_extruder_variants(config);
        CHECK(floats(config, "retraction_length") == std::vector<double>{ 1.0, 2.0, 3.0, 4.0, 5.0 });
        CHECK_THAT(floats(config, "z_hop")[4], Catch::Matchers::WithinAbs(0.5, 1e-9));
        // The layout itself stays as the preset wrote it.
        CHECK(ints(config, "printer_extruder_id").size() == 15);
        // Applying it again changes nothing.
        const DynamicPrintConfig once = config;
        resolve_printer_extruder_variants(config);
        CHECK(config.equals(once));
    }
    SECTION("a High Flow nozzle reads that extruder's High Flow slot") {
        config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values = { nvtStandard, nvtHighFlow, nvtStandard, nvtStandard, nvtStandard };
        resolve_printer_extruder_variants(config);
        CHECK(floats(config, "retraction_length") == std::vector<double>{ 1.0, 2.1, 3.0, 4.0, 5.0 });
    }
    SECTION("a single-extruder-multi-material printer is left alone") {
        config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
        resolve_printer_extruder_variants(config);
        CHECK(floats(config, "retraction_length").size() == 15);
    }
    SECTION("a layout for another extruder count is left alone") {
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>(4, 0.4)));
        resolve_printer_extruder_variants(config);
        CHECK(floats(config, "retraction_length").size() == 15);
    }
    SECTION("a Bambu grouping machine keeps the first value per extruder index") {
        // H2D/X2D-like: two extruders, each offering Standard and High Flow. Those printers read these
        // vectors by filament, and before this change the preset load had already cut them to these.
        DynamicPrintConfig dual = toolchanger_config(2, { "Direct Drive Standard", "Direct Drive High Flow" });
        int                count = 0;
        REQUIRE(dual.support_different_extruders(count));
        resolve_printer_extruder_variants(dual);
        CHECK(floats(dual, "retraction_length") == std::vector<double>{ 1.0, 1.1 });
    }
}

TEST_CASE("A system printer in the per-variant layout keeps all its variants through PresetBundle", "[ExtruderVariant][VariantNormalize]")
{
    ScratchVendor vendor;
    write_variant_vendor(vendor);
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json((vendor.root / "profiles").string(), "VariantTest", PresetBundle::LoadSystem,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent);

    const Preset *toolchanger = bundle.printers.find_preset("VT Toolchanger 0.4 nozzle", false);
    REQUIRE(toolchanger != nullptr);
    CHECK(toolchanger->is_system);
    CHECK(floats(toolchanger->config, "retraction_length") == distinct_retractions(5, 3));
    CHECK(ints(toolchanger->config, "printer_extruder_id").size() == 15);
    CHECK(floats(toolchanger->config, "nozzle_diameter").size() == 5);

    // Selected and edited, it is still the system preset, unmodified.
    bundle.printers.select_preset_by_name("VT Toolchanger 0.4 nozzle", true);
    CHECK(floats(bundle.printers.get_edited_preset().config, "retraction_length").size() == 15);
    CHECK_FALSE(bundle.printers.get_edited_preset().is_dirty);

    // The plain toolchanger is sized to its 5 extruders, as before.
    const Preset *plain = bundle.printers.find_preset("VT Plain 0.4 nozzle", false);
    REQUIRE(plain != nullptr);
    CHECK(floats(plain->config, "retraction_length") == std::vector<double>{ 1., 2., 3., 4., 5. });
    CHECK(floats(plain->config, "z_hop") == std::vector<double>(5, 0.4));
}
