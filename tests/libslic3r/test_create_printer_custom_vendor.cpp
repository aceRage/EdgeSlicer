// Create Printer's vendor choices (CreatePresetsDialog.cpp, get_exist_vendor_choices).
//
// The person's own stand-alone printer models (PresetBundle::get_custom_vendor_models) are
// listed under the vendor key "Custom" (PRESET_CUSTOM_VENDOR). The profiles have their own
// vendor under that key, resources/profiles/Custom.json ("Custom Printer", the generic Klipper,
// Marlin, RRF, Repetier and ToolChanger printers): the vendors are keyed by file name. The
// dialog used to assign the person's models over it (vendors["Custom"] = users_models), so as
// soon as one printer of their own stood alone, "Custom" listed only their models and the
// generic printers could no longer be a template. PresetBundle::add_custom_vendor_models adds
// them instead.

#include <catch2/catch.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

VendorProfile::PrinterModel model_of(const std::string &name, std::vector<std::string> variants)
{
    VendorProfile::PrinterModel model;
    model.id   = name;
    model.name = name;
    for (const std::string &v : variants)
        model.variants.emplace_back(v);
    return model;
}

VendorProfile users_models(std::vector<VendorProfile::PrinterModel> models)
{
    // As get_custom_vendor_models makes it.
    VendorProfile vendor;
    vendor.name   = PRESET_CUSTOM_VENDOR;
    vendor.id     = PRESET_CUSTOM_VENDOR;
    vendor.models = std::move(models);
    return vendor;
}

const VendorProfile::PrinterModel *find_model(const VendorProfile &vendor, const std::string &name)
{
    auto it = std::find_if(vendor.models.begin(), vendor.models.end(),
                           [&name](const VendorProfile::PrinterModel &m) { return m.name == name; });
    return it == vendor.models.end() ? nullptr : &*it;
}

std::vector<std::string> variant_names(const VendorProfile::PrinterModel &model)
{
    std::vector<std::string> out;
    for (const auto &v : model.variants)
        out.push_back(v.name);
    return out;
}

} // namespace

TEST_CASE("Create Printer: the profiles' Custom vendor has the person's vendor key", "[CreatePrinter][Preset]")
{
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    PresetBundle      bundle;
    bundle.load_vendor_configs_from_json(profiles, PRESET_CUSTOM_VENDOR, PresetBundle::LoadVendorOnly,
                                         ForwardCompatibilitySubstitutionRule::EnableSilent);

    auto it = bundle.vendors.find(PRESET_CUSTOM_VENDOR);
    REQUIRE(it != bundle.vendors.end());
    REQUIRE(it->second.id == PRESET_CUSTOM_VENDOR);
    REQUIRE(find_model(it->second, "Generic Klipper Printer") != nullptr);
    const size_t generic_count = it->second.models.size();

    VendorMap vendors = bundle.vendors;
    PresetBundle::add_custom_vendor_models(vendors, users_models({model_of("My Printer", {"0.4"})}));

    const VendorProfile &custom = vendors.at(PRESET_CUSTOM_VENDOR);
    CHECK(custom.models.size() == generic_count + 1);
    CHECK(find_model(custom, "Generic Klipper Printer") != nullptr);
    CHECK(find_model(custom, "My Printer") != nullptr);
    // Still the custom vendor, so the dialog takes the template from the loaded presets.
    CHECK(custom.id == PRESET_CUSTOM_VENDOR);
}

TEST_CASE("Create Printer: the person's models are added to the Custom vendor", "[CreatePrinter][Preset]")
{
    VendorProfile generic;
    generic.name   = "Custom Printer";
    generic.id     = PRESET_CUSTOM_VENDOR;
    generic.models = {model_of("Generic Klipper Printer", {"0.4", "0.6"}), model_of("Generic Marlin Printer", {"0.4"})};

    VendorProfile other;
    other.name   = "Bambu Lab";
    other.id     = "BBL";
    other.models = {model_of("Bambu Lab X1 Carbon", {"0.4"})};

    VendorMap vendors;
    vendors[PRESET_CUSTOM_VENDOR] = generic;
    vendors["BBL"]                = other;

    SECTION("a model of their own is added, the generic ones stay")
    {
        PresetBundle::add_custom_vendor_models(vendors, users_models({model_of("My Printer", {"0.4"})}));
        const VendorProfile &custom = vendors.at(PRESET_CUSTOM_VENDOR);
        REQUIRE(custom.models.size() == 3);
        CHECK(find_model(custom, "Generic Klipper Printer") != nullptr);
        CHECK(find_model(custom, "Generic Marlin Printer") != nullptr);
        CHECK(find_model(custom, "My Printer") != nullptr);
        CHECK(custom.id == PRESET_CUSTOM_VENDOR);
    }

    SECTION("a model of the same name gets the missing nozzles, once each")
    {
        PresetBundle::add_custom_vendor_models(vendors, users_models({model_of("Generic Klipper Printer", {"0.4", "0.8"})}));
        const VendorProfile &custom = vendors.at(PRESET_CUSTOM_VENDOR);
        REQUIRE(custom.models.size() == 2);
        CHECK(variant_names(*find_model(custom, "Generic Klipper Printer")) == std::vector<std::string>{"0.4", "0.6", "0.8"});
    }

    SECTION("another vendor's model of the same name is left alone")
    {
        PresetBundle::add_custom_vendor_models(vendors, users_models({model_of("Bambu Lab X1 Carbon", {"0.4"})}));
        CHECK(vendors.at("BBL").models.size() == 1);
        CHECK(vendors.at(PRESET_CUSTOM_VENDOR).models.size() == 3);
    }

    SECTION("no models of their own: nothing changes")
    {
        PresetBundle::add_custom_vendor_models(vendors, users_models({}));
        CHECK(vendors.at(PRESET_CUSTOM_VENDOR).models.size() == 2);
        CHECK(vendors.size() == 2);
    }
}

TEST_CASE("Create Printer: without a Custom vendor in the profiles the person's models make one", "[CreatePrinter][Preset]")
{
    VendorMap vendors;
    PresetBundle::add_custom_vendor_models(vendors, users_models({model_of("My Printer", {"0.4"})}));
    REQUIRE(vendors.count(PRESET_CUSTOM_VENDOR) == 1);
    CHECK(vendors.at(PRESET_CUSTOM_VENDOR).models.size() == 1);
}
