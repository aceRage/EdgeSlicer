#include <catch2/catch.hpp>

#include <algorithm>
#include <map>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PlateBedTypes.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {

Preset make_printer_preset(const std::string &default_bed_type)
{
    Preset printer(Preset::TYPE_PRINTER, "Test Printer");
    printer.config.set_key_value("default_bed_type", new ConfigOptionString(default_bed_type));
    return printer;
}

Preset &add_inmemory_printer(PresetBundle &bundle, const std::string &name)
{
    DynamicPrintConfig cfg;
    Preset &printer = bundle.printers.load_preset(std::string(), name, cfg, false);
    printer.is_system = true;
    printer.is_visible = true;
    return printer;
}

} // namespace

TEST_CASE("get_default_bed_type parses symbolic and legacy values", "[Preset][BedType]")
{
    SECTION("Symbolic Textured PEI Plate is btPTE, not atoi 0") {
        Preset printer = make_printer_preset("Textured PEI Plate");
        REQUIRE(printer.get_default_bed_type(nullptr) == btPTE);
    }

    SECTION("Symbolic Engineering Plate is btEP") {
        Preset printer = make_printer_preset("Engineering Plate");
        REQUIRE(printer.get_default_bed_type(nullptr) == btEP);
    }

    SECTION("Legacy numeric default_bed_type stays valid") {
        Preset printer = make_printer_preset("4");
        REQUIRE(printer.get_default_bed_type(nullptr) == btPTE);
    }

    SECTION("Invalid default_bed_type falls back to PEI") {
        Preset printer = make_printer_preset("not-a-bed");
        REQUIRE(printer.get_default_bed_type(nullptr) == btPEI);
    }

    SECTION("Empty default_bed_type uses the generic PEI fallback") {
        Preset printer = make_printer_preset("");
        REQUIRE(printer.get_default_bed_type(nullptr) == btPEI);
    }
}

TEST_CASE("Selected printer uses its default or saved bed type", "[Preset][Bundle]")
{
    PresetBundle bundle;
    Preset &printer = add_inmemory_printer(bundle, "Test Printer");
    printer.config.option<ConfigOptionString>("printer_model", true)->value = "TEST-MODEL";
    printer.config.option<ConfigOptionString>("printer_variant", true)->value = "0.4";
    printer.config.option<ConfigOptionString>("default_bed_type", true)->value = "Engineering Plate";

    AppConfig app_config;
    app_config.set("curr_bed_type", std::to_string(static_cast<int>(btPTE)));
    PresetBundle::PresetPreferences preferred_selection;
    BedType expected_bed_type;

    SECTION("New printer uses its symbolic default") {
        expected_bed_type = btEP;
        preferred_selection = {"TEST-MODEL", "0.4"};
    }
    SECTION("Re-enabled printer uses its saved selection") {
        expected_bed_type = btPC;
        preferred_selection = {"TEST-MODEL", "0.4"};
        app_config.set_printer_setting("Test Printer", "curr_bed_type",
                                       std::to_string(static_cast<int>(expected_bed_type)));
    }
    SECTION("Existing printer keeps its saved selection after presets reload") {
        expected_bed_type = btPCT;
        app_config.set("presets", PRESET_PRINTER_NAME, "Test Printer");
        app_config.set_printer_setting("Test Printer", "curr_bed_type",
                                       std::to_string(static_cast<int>(expected_bed_type)));
    }

    bundle.load_selections(app_config, preferred_selection);
    bundle.export_selections(app_config);

    CHECK(bundle.project_config.opt_enum<BedType>("curr_bed_type") == expected_bed_type);
    CHECK(app_config.get_printer_setting("Test Printer", "curr_bed_type") == std::to_string(static_cast<int>(expected_bed_type)));
}

// Bambu Studio keeps a machine's default plate in the machine model file; get_default_bed_type reads it for
// Bambu machines, after the preset's own default_bed_type and before the plates fixed by model id.
namespace {

VendorProfile::PrinterModel make_model(const std::string &name, const std::string &model_id, const std::string &default_bed_type)
{
    VendorProfile::PrinterModel model;
    model.id               = name;
    model.name             = name;
    model.model_id         = model_id;
    model.default_bed_type = default_bed_type;
    return model;
}

// A bundle with one printer of `printer_model` selected, in a vendor `vendor_id` whose model file says `model_bed_type`.
PresetBundle bundle_with_model(const std::string &vendor_id, const std::string &printer_model, const std::string &model_id,
                               const std::string &model_bed_type, const std::string &preset_bed_type = std::string())
{
    PresetBundle  bundle;
    VendorProfile vendor(vendor_id);
    vendor.models.push_back(make_model(printer_model, model_id, model_bed_type));
    bundle.vendors.emplace(vendor_id, vendor);
    Preset &printer = add_inmemory_printer(bundle, printer_model + " 0.4 nozzle");
    printer.config.option<ConfigOptionString>("printer_model", true)->value    = printer_model;
    printer.config.option<ConfigOptionString>("printer_variant", true)->value  = "0.4";
    printer.config.option<ConfigOptionString>("default_bed_type", true)->value = preset_bed_type;
    bundle.printers.select_preset_by_name(printer_model + " 0.4 nozzle", true);
    return bundle;
}

BedType default_bed_type_of(PresetBundle &bundle) { return bundle.printers.get_edited_preset().get_default_bed_type(&bundle); }

} // namespace

TEST_CASE("get_default_bed_type reads a Bambu machine model's default plate", "[Preset][BedType]")
{
    SECTION("a Bambu machine starts on the plate of its model file") {
        PresetBundle bundle = bundle_with_model("BBL", "Bambu Lab A1", "N2S", "Textured PEI Plate");
        CHECK(default_bed_type_of(bundle) == btPTE);
    }
    SECTION("the model file comes before the plate fixed by model id") {
        // BL-P001 (X1 Carbon) is Cool Plate by model id.
        PresetBundle bundle = bundle_with_model("BBL", "Bambu Lab X1 Carbon", "BL-P001", "Textured PEI Plate");
        CHECK(default_bed_type_of(bundle) == btPTE);
    }
    SECTION("without the key the model id decides, as before") {
        PresetBundle x1c = bundle_with_model("BBL", "Bambu Lab X1 Carbon", "BL-P001", "");
        CHECK(default_bed_type_of(x1c) == btPC);
        PresetBundle a1 = bundle_with_model("BBL", "Bambu Lab A1", "N2S", "");
        CHECK(default_bed_type_of(a1) == btPEI);
    }
    SECTION("a value that names no plate is passed over") {
        PresetBundle named_wrong = bundle_with_model("BBL", "Bambu Lab X1 Carbon", "BL-P001", "No Such Plate");
        CHECK(default_bed_type_of(named_wrong) == btPC);
        PresetBundle default_plate = bundle_with_model("BBL", "Bambu Lab A1", "N2S", "Default Plate");
        CHECK(default_bed_type_of(default_plate) == btPEI);
    }
    SECTION("the preset's own default_bed_type comes first") {
        PresetBundle bundle = bundle_with_model("BBL", "Bambu Lab A1", "N2S", "Textured PEI Plate", "Engineering Plate");
        CHECK(default_bed_type_of(bundle) == btEP);
    }
    SECTION("another vendor's model file is not read") {
        PresetBundle bundle = bundle_with_model("Creality", "Creality K1 SE", "", "Textured PEI Plate");
        CHECK(default_bed_type_of(bundle) == btPEI);
    }
    SECTION("the U1 keeps its Textured PEI Plate") {
        PresetBundle bundle = bundle_with_model("Snapmaker", "Snapmaker U1", "SM_U1", "");
        CHECK(default_bed_type_of(bundle) == btPTE);
    }
    SECTION("a printer that is no model of the bundle is the High Temp Plate, with or without a bundle") {
        PresetBundle bundle = bundle_with_model("BBL", "Bambu Lab A1", "N2S", "Textured PEI Plate");
        Preset       other(Preset::TYPE_PRINTER, "Other");
        other.config.set_key_value("printer_model", new ConfigOptionString("Not a Bambu model"));
        CHECK(other.get_default_bed_type(&bundle) == btPEI);
        CHECK(other.get_default_bed_type(nullptr) == btPEI);
        Preset no_model(Preset::TYPE_PRINTER, "No model");
        CHECK(no_model.get_default_bed_type(&bundle) == btPEI);
        CHECK(no_model.get_default_bed_type(nullptr) == btPEI);
    }
}

TEST_CASE("The vendor loader reads default_bed_type from a machine model file", "[Preset][BedType][Bundle]")
{
    namespace fs = boost::filesystem;
    const fs::path profiles = fs::temp_directory_path() / fs::unique_path("default_bed_type_%%%%-%%%%");
    fs::create_directories(profiles / "BBL" / "machine");
    {
        boost::nowide::ofstream vendor((profiles / "BBL.json").string());
        vendor << R"({ "name": "BBL", "version": "01.00.00.00", "force_update": "0", "description": "test",
                       "machine_model_list": [ { "name": "Test A1", "sub_path": "machine/Test A1.json" },
                                               { "name": "Test Plain", "sub_path": "machine/Test Plain.json" } ],
                       "process_list": [], "filament_list": [], "machine_list": [] })";
        boost::nowide::ofstream with_key((profiles / "BBL" / "machine" / "Test A1.json").string());
        with_key << R"({ "type": "machine_model", "name": "Test A1", "nozzle_diameter": "0.4", "bed_model": "", "bed_texture": "",
                         "default_bed_type": "Textured PEI Plate", "family": "BBL-3DP", "machine_tech": "FFF", "model_id": "N2S",
                         "default_materials": "" })";
        boost::nowide::ofstream without_key((profiles / "BBL" / "machine" / "Test Plain.json").string());
        without_key << R"({ "type": "machine_model", "name": "Test Plain", "nozzle_diameter": "0.4", "bed_model": "", "bed_texture": "",
                            "family": "BBL-3DP", "machine_tech": "FFF", "model_id": "X", "default_materials": "" })";
    }

    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles.string(), "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    fs::remove_all(profiles);

    auto vendor = bundle.vendors.find("BBL");
    REQUIRE(vendor != bundle.vendors.end());
    REQUIRE(vendor->second.models.size() == 2);
    std::map<std::string, std::string> by_name;
    for (const VendorProfile::PrinterModel &model : vendor->second.models)
        by_name[model.name] = model.default_bed_type;
    CHECK(by_name.at("Test A1") == "Textured PEI Plate");
    CHECK(by_name.at("Test Plain").empty());
}

TEST_CASE("The Bambu machines of the shipped profiles start on the plate their model files name", "[Preset][BedType][Bundle]")
{
    namespace fs = boost::filesystem;
    // The real BBL vendor, read into a bundle of its own with an empty data directory.
    const std::string saved_data_dir = data_dir();
    const fs::path    scratch        = fs::temp_directory_path() / fs::unique_path("default_bed_type_bbl_%%%%-%%%%");
    fs::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    PresetBundle      library;
    library.load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    PresetBundle bundle;
    bundle.load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
    set_data_dir(saved_data_dir);
    fs::remove_all(scratch);

    auto vendor = bundle.vendors.find("BBL");
    REQUIRE(vendor != bundle.vendors.end());
    size_t checked = 0;
    for (const VendorProfile::PrinterModel &model : vendor->second.models) {
        if (model.default_bed_type.empty())
            continue;
        BedType expected;
        REQUIRE(ConfigOptionEnum<BedType>::from_string(model.default_bed_type, expected));
        // Any system preset of the model: the plate is the model's, whatever the nozzle.
        std::string preset_name;
        for (const Preset &preset : bundle.printers.get_presets())
            if (preset.is_system && preset.config.opt_string("printer_model") == model.name) {
                preset_name = preset.name;
                break;
            }
        CAPTURE(model.name, model.default_bed_type, preset_name);
        REQUIRE(!preset_name.empty());
        Preset *preset = bundle.printers.find_preset(preset_name, false, true);
        REQUIRE(preset != nullptr);
        preset->is_visible = true;
        REQUIRE(bundle.printers.select_preset_by_name(preset_name, true));
        CHECK(bundle.printers.get_edited_preset().get_default_bed_type(&bundle) == expected);
        ++checked;
    }
    // The X1 Carbon and the A1 are among them, and name the Textured PEI Plate.
    CHECK(checked >= 2);
    for (const char *name : { "Bambu Lab X1 Carbon", "Bambu Lab A1" }) {
        CAPTURE(name);
        auto it = std::find_if(vendor->second.models.begin(), vendor->second.models.end(),
                               [name](const VendorProfile::PrinterModel &model) { return model.name == name; });
        REQUIRE(it != vendor->second.models.end());
        CHECK(it->default_bed_type == "Textured PEI Plate");
    }
}

TEST_CASE("A plate the printer's selector does not list becomes the printer's default plate", "[PlateBedTypes][BedType]")
{
    using PlateBedTypes::listed_bed_type;
    const ConfigOptionDef *def = print_config_def.get("curr_bed_type");
    REQUIRE(def != nullptr);
    const std::vector<std::string> &generic = def->enum_values;    // every printer but the U1
    const std::vector<std::string> &u1      = def->enum_values_u1; // the U1's four plates
    const std::vector<std::string> &u1_all  = def->enum_values_ex; // the U1 with support_multi_bed_types

    SECTION("a listed plate stays, whatever the default") {
        for (BedType plate : { btPC, btEP, btPEI, btPTE, btPCT, btSuperTack }) {
            CAPTURE(int(plate));
            CHECK(listed_bed_type(plate, generic, btPEI) == plate);
        }
        for (BedType plate : { btPTE, btPEI, btGESP, btPC }) {
            CAPTURE(int(plate));
            CHECK(listed_bed_type(plate, u1, btPTE) == plate);
        }
        for (int plate = int(btDefault) + 1; plate < int(btCount); ++plate) {
            CAPTURE(plate);
            CHECK(listed_bed_type(BedType(plate), u1_all, btPTE) == BedType(plate));
        }
    }
    SECTION("Graphic Effect Plate on a printer that is not a U1 is that printer's default plate") {
        CHECK(listed_bed_type(btGESP, generic, btPEI) == btPEI);
        CHECK(listed_bed_type(btGESP, generic, btPTE) == btPTE);
        CHECK(listed_bed_type(btGESP, generic, btPC) == btPC);
    }
    SECTION("so is a value that is no plate") {
        CHECK(listed_bed_type(btDefault, generic, btPTE) == btPTE);
        CHECK(listed_bed_type(btCount, generic, btPTE) == btPTE);
        CHECK(listed_bed_type(BedType(42), u1_all, btPEI) == btPEI);
    }
    SECTION("a default that is not listed either gives the first plate of the list") {
        CHECK(listed_bed_type(btGESP, generic, btGESP) == btPC);
        CHECK(listed_bed_type(btEP, u1, btPCT) == btPTE);
    }
    SECTION("a list without a plate changes nothing") {
        CHECK(listed_bed_type(btGESP, {}, btPEI) == btGESP);
        CHECK(listed_bed_type(btGESP, { "not a plate" }, btPEI) == btGESP);
    }
}
