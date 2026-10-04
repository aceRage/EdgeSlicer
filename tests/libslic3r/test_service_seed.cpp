#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <iterator>
#include <string>

using namespace Slic3r;

// Service mode (an unattended hub) seeds a data directory that has never run, so the printer
// wizard and the privacy prompt have nothing left to ask. What matters is that the answers the
// start-up code looks for are all there; GUI_App::config_wizard_startup treats an empty privacy
// flag, a missing first-guide flag or a printer-less config as "run the wizard".

TEST_CASE("service seed: the first-run questions are answered", "[AppConfig][ServiceMode]")
{
    AppConfig config;
    config.seed_service_defaults();

    CHECK(config.get("app", "privacy_policy_isagree") == "false"); // answered, not agreed; empty would ask again
    CHECK_FALSE(config.get("app", "privacy_policy_isagree").empty());
    CHECK(config.get("firstguide", "finish") == "true");
    CHECK(config.get_bool("firstguide", "finish"));
    CHECK(config.get("app", "language") == "en_US");
    CHECK(config.get("app", "check_for_updates_on_startup") == "false");
}

TEST_CASE("service seed: one printer, its filaments and the selection are set", "[AppConfig][ServiceMode]")
{
    AppConfig config;
    config.seed_service_defaults();

    REQUIRE(config.vendors().count("Snapmaker") == 1);
    const auto& models = config.vendors().at("Snapmaker");
    REQUIRE(models.count("Snapmaker U1") == 1);
    CHECK(models.at("Snapmaker U1").count("0.4") == 1);
    CHECK(config.get_variant("Snapmaker", "Snapmaker U1", "0.4"));

    CHECK(config.get("presets", "machine") == "Snapmaker U1 (0.4 nozzle)");
    CHECK(config.get("presets", "print") == "0.20mm Standard @Snapmaker U1 (0.4 nozzle)");
    CHECK(config.get("presets", "filament") == "Generic PLA @U1 0.4 nozzle");
    CHECK(config.has("filaments", "Generic PLA @U1 0.4 nozzle"));
    CHECK(config.has("filaments", "Generic PETG @U1 0.4 nozzle"));
}

TEST_CASE("service seed: the preset names are real profiles in the Snapmaker vendor bundle", "[AppConfig][ServiceMode]")
{
    // The seed names three presets by hand. A rename in resources/profiles/Snapmaker.json would
    // make the seeded selection point at nothing and send the first start to the wizard, so
    // the names are checked against the bundle that ships.
    const boost::filesystem::path bundle = boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles" / "Snapmaker.json";
    if (!boost::filesystem::exists(bundle)) {
        WARN("Snapmaker.json not found at " << bundle.string() << "; skipping the preset-name check");
        return;
    }
    boost::filesystem::ifstream in(bundle);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (const char* name : { "Snapmaker U1 (0.4 nozzle)", "0.20mm Standard @Snapmaker U1 (0.4 nozzle)", "Generic PLA @U1 0.4 nozzle", "Generic PETG @U1 0.4 nozzle" }) {
        INFO(name);
        CHECK(text.find(std::string("\"name\": \"") + name + "\"") != std::string::npos);
    }
}

TEST_CASE("service seed: it only fills in; defaults for everything else still apply", "[AppConfig][ServiceMode]")
{
    AppConfig config;
    config.set_defaults();
    config.seed_service_defaults();
    CHECK_FALSE(config.get("app", "privacy_policy_isagree").empty());
    // a key the seed does not touch keeps whatever set_defaults gave it
    CHECK(config.get("app", "drop_project_action") == "true");
}
