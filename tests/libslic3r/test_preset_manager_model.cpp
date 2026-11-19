// The Filament manager / Process manager rows (libslic3r/PresetManagerModel) and the process
// "Enabled" storage (AppConfig::SECTION_HIDDEN_PROCESSES, Preset::set_visible_from_appconfig).
#include <catch2/catch.hpp>

#include <cmath>
#include <memory>
#include <set>

#include <boost/filesystem.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetManagerModel.hpp"

using namespace Slic3r;

namespace {

PresetManagerItem filament(const std::string &name, const std::string &alias, const std::string &vendor_id, bool visible,
                           std::vector<std::string> printers, double nozzle_t = 220.)
{
    PresetManagerItem it;
    it.name            = name;
    it.alias           = alias;
    it.vendor_id       = vendor_id;
    it.is_system       = !vendor_id.empty();
    it.visible         = visible;
    it.favorite_key    = preset_manager_favorite_key(name, alias, it.is_system);
    it.filament_vendor = vendor_id.empty() ? "Me" : vendor_id;
    it.type            = "PLA";
    it.nozzle_t        = nozzle_t;
    it.printers        = std::move(printers);
    return it;
}

PresetManagerItem process(const std::string &name, const std::string &alias, bool system, const std::string &inherits = "")
{
    PresetManagerItem it;
    it.name         = name;
    it.alias        = alias;
    it.is_system    = system;
    it.vendor_id    = system ? "BBL" : "";
    it.favorite_key = preset_manager_favorite_key(name, alias, system);
    it.inherits     = inherits;
    it.layer_height = 0.2;
    it.printers     = {"Bambu Lab X1 Carbon 0.4 nozzle"};
    it.nozzles      = {"0.4"};
    return it;
}

const PresetManagerRow &row(const PresetManagerModel &m, const std::string &key)
{
    const PresetManagerRow *r = m.find(key);
    INFO("row " << key);
    REQUIRE(r != nullptr);
    return *r;
}

std::string profiles_dir()
{
    return (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
}

} // namespace

TEST_CASE("Process quality tier from the alias", "[PresetManager]")
{
    CHECK(process_quality_tier("0.20mm Standard") == "Standard");
    CHECK(process_quality_tier("0.08mm Extra Fine") == "Extra Fine");
    CHECK(process_quality_tier("0.28mm Extra Draft") == "Extra Draft");
    CHECK(process_quality_tier("0.20mm Strength") == "Strength");
    CHECK(process_quality_tier("0.12mm Fine @BBL X1C") == "Fine");
    CHECK(process_quality_tier("0.2mm Balanced Quality") == "Balanced Quality");
    CHECK(process_quality_tier("My process") == "");
    CHECK(process_quality_tier("0.20mm") == "");
    CHECK(process_quality_tier("") == "");
}

TEST_CASE("Favourite key follows the favourite filaments rule", "[PresetManager]")
{
    CHECK(preset_manager_favorite_key("Bambu PLA Basic @BBL X1C", "Bambu PLA Basic", true) == "Bambu PLA Basic");
    CHECK(preset_manager_favorite_key("Generic PLA @System", "", true) == "Generic PLA");
    CHECK(preset_manager_favorite_key("My PLA @BBL X1C", "My PLA", false) == "My PLA @BBL X1C");
}

TEST_CASE("Filament rows: one per vendor bundle and alias, user rows one each", "[PresetManager]")
{
    std::vector<PresetManagerItem> items;
    items.push_back(filament("Generic PLA @BBL X1C", "Generic PLA", "BBL", false, {"X1C"}, 220.));
    items.push_back(filament("Generic PLA @BBL P1S", "Generic PLA", "BBL", true, {"P1S"}, std::nan("")));
    items.push_back(filament("Generic PLA @System", "Generic PLA", "OrcaFilamentLibrary", false, {"U1"}));
    items.back().renamed_from = {"Generic PLA Old @System"};
    items.push_back(filament("My PLA @BBL X1C", "My PLA", "", true, {"X1C"}));
    items.push_back(filament("My PLA @BBL P1S", "My PLA", "", true, {"P1S"}));
    items[1].in_use = true;

    const PresetManagerModel m = build_preset_manager_model(PresetManagerKind::Filament, items);
    REQUIRE(m.rows.size() == 4);

    const PresetManagerRow &bbl = row(m, "s|BBL|Generic PLA");
    CHECK(bbl.name == "Generic PLA");
    CHECK(bbl.members == std::vector<std::string>{"Generic PLA @BBL P1S", "Generic PLA @BBL X1C"});
    CHECK(bbl.enabled);              // any member visible
    CHECK(bbl.in_use);
    CHECK(bbl.lock == "in_use");     // shown and in use: cannot be unticked
    CHECK(bbl.printers == std::vector<std::string>{"P1S", "X1C"});
    CHECK(bbl.nozzle_t == 220.);     // first known value of the members
    CHECK(bbl.favorite_key == "Generic PLA");

    const PresetManagerRow &lib = row(m, "s|OrcaFilamentLibrary|Generic PLA");
    CHECK_FALSE(lib.enabled);
    CHECK(lib.lock.empty());
    CHECK(lib.old_names == std::vector<std::string>{"Generic PLA Old @System"});
    CHECK(lib.favorite_key == bbl.favorite_key); // one star for both vendors' "Generic PLA"

    const PresetManagerRow &u1 = row(m, "u|My PLA @BBL X1C");
    CHECK(u1.name == "My PLA @BBL X1C");
    CHECK(u1.enabled);
    CHECK(u1.lock == "user");
    CHECK(row(m, "u|My PLA @BBL P1S").members.size() == 1);

    // system rows first, then user rows
    CHECK(m.rows[0].is_system);
    CHECK(m.rows[1].is_system);
    CHECK_FALSE(m.rows[2].is_system);
    CHECK(m.printers == std::vector<std::string>{"P1S", "U1", "X1C"});
}

TEST_CASE("Process rows: one per preset, tier, parent, Detached", "[PresetManager]")
{
    std::vector<PresetManagerItem> items;
    items.push_back(process("0.20mm Standard @BBL X1C", "0.20mm Standard", true));
    items.push_back(process("0.08mm Extra Fine @BBL X1C", "0.08mm Extra Fine", true));
    items.push_back(process("Mine", "", false, "0.20mm Standard @BBL X1C"));
    items.push_back(process("Loose", "", false, ""));
    items[0].in_use  = true;
    items[1].visible = false;

    const PresetManagerModel m = build_preset_manager_model(PresetManagerKind::Process, items);
    REQUIRE(m.rows.size() == 4);
    const PresetManagerRow &std_row = row(m, "s|0.20mm Standard @BBL X1C");
    CHECK(std_row.name == "0.20mm Standard");
    CHECK(std_row.tier == "Standard");
    CHECK(std_row.lock == "in_use");
    const PresetManagerRow &fine = row(m, "s|0.08mm Extra Fine @BBL X1C");
    CHECK_FALSE(fine.enabled);
    CHECK(fine.tier == "Extra Fine");
    CHECK(row(m, "u|Mine").tier == "0.20mm Standard");
    CHECK(row(m, "u|Loose").tier == "Detached");
    CHECK(row(m, "u|Loose").lock == "user");
}

TEST_CASE("Rows JSON: shape, nulls, stars", "[PresetManager]")
{
    std::vector<PresetManagerItem> items;
    items.push_back(filament("Generic PLA @BBL X1C", "Generic PLA", "BBL", true, {"X1C"}, std::nan("")));
    items.push_back(filament("My PLA", "", "", true, {}));
    items.back().mtime = 1700000000;
    const PresetManagerModel m = build_preset_manager_model(PresetManagerKind::Filament, items);
    const nlohmann::json     j = preset_manager_rows_json(m, {"Generic PLA"}, "X1C");
    CHECK(j["kind"] == "filament");
    CHECK(j["current_printer"] == "X1C");
    REQUIRE(j["rows"].size() == 2);
    const nlohmann::json &sys = j["rows"][0];
    for (const char *k : {"key", "fav", "name", "members", "vendor", "type", "source", "printers", "all", "nozzle_t", "nozzle_lo",
                          "nozzle_hi", "bed_t", "lh", "tier", "mtime", "enabled", "lock", "in_use", "star", "can_rename", "can_delete"}) {
        INFO(k);
        CHECK(sys.contains(k));
    }
    CHECK(sys["nozzle_t"].is_null());
    CHECK(sys["mtime"].is_null());
    CHECK(sys["star"] == true);
    CHECK(sys["source"] == "System");
    CHECK(sys["can_rename"] == false);
    const nlohmann::json &user = j["rows"][1];
    CHECK(user["star"] == false);
    CHECK(user["mtime"] == 1700000000);
    CHECK(user["source"] == "User");
}

TEST_CASE("Enabled storage edits", "[PresetManager]")
{
    PresetManagerRow group;
    group.is_system = true;
    group.members   = {"A @X", "A @Y"};
    group.old_names = {"A old @X"};

    std::map<std::string, std::string> fil = {{"A old @X", "true"}, {"B", "true"}};
    auto on = filament_section_with(fil, group, true);
    CHECK(on.count("A @X") == 1);
    CHECK(on.count("A @Y") == 1);
    CHECK(on.count("B") == 1);
    auto off = filament_section_with(on, group, false);
    CHECK(off.count("A @X") == 0);
    CHECK(off.count("A old @X") == 0); // a renamed_from name would keep it visible
    CHECK(off.count("B") == 1);

    std::map<std::string, std::string> hidden;
    auto h = hidden_process_section_with(hidden, group, false);
    CHECK(h.size() == 2);
    auto s = hidden_process_section_with(h, group, true);
    CHECK(s.empty());

    PresetManagerRow user;
    user.is_system = false;
    user.members   = {"Mine"};
    CHECK(filament_section_with(fil, user, false) == fil);
    CHECK(hidden_process_section_with(hidden, user, false).empty());
}

TEST_CASE("Hidden system process presets are not visible", "[PresetManager][AppConfig]")
{
    VendorProfile vendor("BBL");
    Preset sys(Preset::TYPE_PRINT, "0.20mm Standard @BBL X1C");
    sys.vendor = &vendor;
    Preset renamed(Preset::TYPE_PRINT, "0.20mm Strength @BBL X1C");
    renamed.vendor       = &vendor;
    renamed.renamed_from = {"0.20mm Strong @BBL X1C"};
    Preset user(Preset::TYPE_PRINT, "Mine");

    AppConfig config;
    sys.set_visible_from_appconfig(config);
    CHECK(sys.is_visible); // no section: every system process shown

    config.set(AppConfig::SECTION_HIDDEN_PROCESSES, "0.20mm Standard @BBL X1C", "true");
    config.set(AppConfig::SECTION_HIDDEN_PROCESSES, "0.20mm Strong @BBL X1C", "true");
    config.set(AppConfig::SECTION_HIDDEN_PROCESSES, "Mine", "true");
    sys.set_visible_from_appconfig(config);
    renamed.set_visible_from_appconfig(config);
    user.set_visible_from_appconfig(config);
    CHECK_FALSE(sys.is_visible);
    CHECK_FALSE(renamed.is_visible); // hidden under the name it was renamed from
    CHECK(user.is_visible);          // user presets are always shown

    config.erase(AppConfig::SECTION_HIDDEN_PROCESSES, "0.20mm Standard @BBL X1C");
    sys.set_visible_from_appconfig(config);
    CHECK(sys.is_visible);           // shown again
}

TEST_CASE("Preset manager rows from the shipped Snapmaker profiles", "[PresetManager]")
{
    PresetBundle library;
    library.load_vendor_configs_from_json(profiles_dir(), PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles_dir(), "Snapmaker", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          &library);

    SECTION("process: hidden set honoured, rows listed under printers")
    {
        const Preset *first = nullptr;
        for (const Preset &p : bundle->prints)
            if (p.is_system) { first = &p; break; }
        REQUIRE(first != nullptr);
        AppConfig config;
        config.set(AppConfig::SECTION_HIDDEN_PROCESSES, first->name, "true");
        bundle->load_installed_prints(config);
        size_t hidden = 0, shown = 0;
        for (const Preset &p : bundle->prints)
            if (p.is_system) (p.is_visible ? shown : hidden)++;
        CHECK(hidden == 1);
        CHECK(shown > 0);

        const auto items = collect_preset_manager_items(*bundle, PresetManagerKind::Process, "");
        const PresetManagerModel m = build_preset_manager_model(PresetManagerKind::Process, items);
        CHECK(m.rows.size() == items.size());
        const PresetManagerRow &r = row(m, "s|" + first->name);
        CHECK_FALSE(r.enabled);
        size_t with_printer = 0;
        for (const PresetManagerRow &x : m.rows)
            if (!x.printers.empty() || x.all_printers) ++with_printer;
        CHECK(with_printer > 0);
    }

    SECTION("filament: system presets grouped by vendor + alias")
    {
        const auto items = collect_preset_manager_items(*bundle, PresetManagerKind::Filament, "hot_plate_temp");
        const PresetManagerModel m = build_preset_manager_model(PresetManagerKind::Filament, items);
        REQUIRE_FALSE(items.empty());
        CHECK(m.rows.size() < items.size());
        std::set<std::string> keys;
        size_t members = 0;
        for (const PresetManagerRow &x : m.rows) {
            keys.insert(x.key);
            members += x.members.size();
        }
        CHECK(keys.size() == m.rows.size()); // keys are unique
        CHECK(members == items.size());     // every preset in exactly one row
    }
}
