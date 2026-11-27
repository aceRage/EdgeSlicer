// Settings views (Basic / Advanced / Bambu): the per-line visibility rule and the "changed" rule that
// keeps an option outside Bambu Studio's list visible. See src/libslic3r/SettingsView.hpp and
// tests/design_view_modes.md.
#include <catch2/catch.hpp>

#include "libslic3r/Format/BambuUiKeys.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SettingsView.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <set>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

ConfigOptionMode mode_of(const std::string &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    REQUIRE(def != nullptr);
    return def->mode;
}

LineShow show(const ViewFilter &f, const std::string &key) { return line_show(f, { key }, mode_of(key)); }

ViewFilter level(ConfigOptionMode m)
{
    ViewFilter f;
    f.level = m;
    return f;
}

ViewFilter bambu(const std::set<std::string> *changed = nullptr, const std::set<std::string> *revealed = nullptr)
{
    ViewFilter f;
    f.level    = comAdvanced;
    f.view     = SettingsView::Bambu;
    f.changed  = changed;
    f.revealed = revealed;
    return f;
}

// Keys used below, checked against the generated list so a refresh that moves them fails loudly.
const char *on_list_simple   = "wall_loops";            // Bambu Simple, ours Simple
const char *on_list_advanced = "wall_sequence";         // Bambu Advanced, ours Advanced
const char *off_list         = "staggered_inner_seams"; // EdgeSlicer-only, Advanced
const char *renamed          = "only_one_wall_top";     // Bambu: top_one_wall_type
const char *inferred_pair    = "undertop_surface_pattern"; // Bambu: sub_top_surface_pattern (inferred)

struct TestFilaments : public PresetCollection
{
    TestFilaments()
        : PresetCollection(Preset::TYPE_FILAMENT, Preset::filament_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Filament")
    {}

    // A preset built on the default one, with `inherits` and the given flow ratio / temperature.
    void add(const std::string &name, const std::string &inherits, double flow, int temp)
    {
        DynamicPrintConfig cfg = this->default_preset().config;
        cfg.set_key_value("inherits", new ConfigOptionString(inherits));
        cfg.option<ConfigOptionFloats>("filament_flow_ratio", true)->values = { flow };
        cfg.option<ConfigOptionInts>("nozzle_temperature", true)->values    = { temp };
        this->load_preset("", name, cfg, false);
    }
};

double default_flow() { return print_config_def.get("filament_flow_ratio")->get_default_value<ConfigOptionFloats>()->values.front(); }
int    default_temp() { return print_config_def.get("nozzle_temperature")->get_default_value<ConfigOptionInts>()->values.front(); }

} // namespace

TEST_CASE("Test keys are where the tests expect them", "[SettingsView]")
{
    CHECK(SettingsViewKeys::on_bambu_list(on_list_simple));
    CHECK(SettingsViewKeys::on_bambu_list(on_list_advanced));
    CHECK_FALSE(SettingsViewKeys::on_bambu_list(off_list));
    CHECK(mode_of(on_list_simple) == comSimple);
    CHECK(mode_of(on_list_advanced) == comAdvanced);
    CHECK(mode_of(off_list) == comAdvanced);
    // Our list is the 431 Simple + Advanced Bambu lines minus duplicates across tabs and keys we lack.
    CHECK(SettingsViewKeys::bambu_list_size() > 350);
}

TEST_CASE("Basic and Advanced views are today's mode comparison", "[SettingsView]")
{
    // Every option definition: line_show must equal `mode <= level` for each level when the view is
    // Default, whatever the changed / revealed sets hold.
    const std::set<std::string> everything_changed = { off_list, on_list_simple, on_list_advanced };
    for (ConfigOptionMode lvl : { comSimple, comAdvanced, comDevelop }) {
        ViewFilter f = level(lvl);
        f.changed    = &everything_changed;
        f.revealed   = &everything_changed;
        f.bbl_printer = true;
        for (const auto &[key, def] : print_config_def.options) {
            INFO(key << " at level " << int(lvl));
            const bool expected = def.mode <= lvl;
            CHECK(shown(line_show(f, { key }, def.mode)) == expected);
            CHECK(line_show(f, { key }, def.mode) != LineShow::ShownNotInBambu);
        }
        // Widget-only lines count as Simple, as today.
        CHECK(shown(widget_line_show(f, false)));
    }
}

TEST_CASE("Bambu view shows Bambu's list and hides the rest", "[SettingsView]")
{
    const ViewFilter f = bambu();
    CHECK(show(f, on_list_simple) == LineShow::Shown);
    CHECK(show(f, on_list_advanced) == LineShow::Shown);
    CHECK(show(f, off_list) == LineShow::Hidden);
    // Renamed and inferred-equivalent keys are found under our name.
    CHECK(show(f, renamed) == LineShow::Shown);
    CHECK(show(f, inferred_pair) == LineShow::Shown);
    // An indexed option id ("key#1", per-extruder / per-nozzle columns) is its base key.
    CHECK(line_show(f, { std::string(on_list_advanced) + "#1" }, comAdvanced) == LineShow::Shown);
    // A multi-option line shows when any of its options is listed.
    CHECK(line_show(f, { off_list, on_list_simple }, comAdvanced) == LineShow::Shown);
}

TEST_CASE("Bambu view never shows our Developer lines without Developer mode", "[SettingsView]")
{
    const std::set<std::string> changed = { "support_air_filtration", off_list };
    // support_air_filtration is Advanced in Bambu Studio but Developer here: kept out (design 3.3 rule 2).
    REQUIRE(mode_of("support_air_filtration") == comDevelop);
    REQUIRE(SettingsViewKeys::on_bambu_list("support_air_filtration"));
    CHECK(show(bambu(), "support_air_filtration") == LineShow::Hidden);
    CHECK(show(bambu(&changed), "support_air_filtration") == LineShow::Hidden); // even changed
}

TEST_CASE("Developer mode on: the view still applies", "[SettingsView][Developer]")
{
    // Developer mode raises the level to comDevelop in Advanced and Bambu; Basic stays Simple.
    const char *develop_key = "support_air_filtration"; // our Developer line, on Bambu's list
    REQUIRE(mode_of(develop_key) == comDevelop);

    SECTION("Basic + Developer mode: Simple lines only, as today")
    {
        const ViewFilter f = level(comSimple);
        CHECK(show(f, on_list_simple) == LineShow::Shown);
        CHECK(show(f, on_list_advanced) == LineShow::Hidden);
        CHECK(show(f, develop_key) == LineShow::Hidden);
    }
    SECTION("Advanced + Developer mode: adds our Developer lines")
    {
        const ViewFilter f = level(comDevelop);
        CHECK(show(f, on_list_advanced) == LineShow::Shown);
        CHECK(show(f, off_list) == LineShow::Shown);
        CHECK(show(f, develop_key) == LineShow::Shown);
        CHECK(show(f, "curved_tops_dev_angle_ceiling") == LineShow::Shown);
    }
    SECTION("Bambu + Developer mode: the Bambu filter applies; Developer lines only as exceptions")
    {
        ViewFilter f = bambu();
        f.level      = comDevelop;
        REQUIRE(f.bambu());
        CHECK(show(f, on_list_simple) == LineShow::Shown);
        CHECK(show(f, on_list_advanced) == LineShow::Shown);
        CHECK(show(f, off_list) == LineShow::Hidden);
        CHECK(show(f, develop_key) == LineShow::Hidden); // listed by Bambu, but a Developer line here
        CHECK(show(f, "curved_tops_dev_angle_ceiling") == LineShow::Hidden);

        const std::set<std::string> changed  = { develop_key, off_list };
        const std::set<std::string> revealed = { "curved_tops_dev_angle_ceiling" };
        ViewFilter                  g        = bambu(&changed, &revealed);
        g.level                              = comDevelop;
        CHECK(show(g, develop_key) == LineShow::ShownNotInBambu);
        CHECK(show(g, off_list) == LineShow::ShownNotInBambu);
        CHECK(show(g, "curved_tops_dev_angle_ceiling") == LineShow::ShownNotInBambu);
        // Widget-only lines still follow their group.
        CHECK(widget_line_show(g, false) == LineShow::Hidden);
        CHECK(widget_line_show(g, true) == LineShow::Shown);
    }
}

TEST_CASE("Bambu view shows changed and revealed options outside the list, marked", "[SettingsView]")
{
    const std::set<std::string> changed  = { off_list };
    const std::set<std::string> revealed = { "bridge_line_width" };
    const ViewFilter            f        = bambu(&changed, &revealed);
    CHECK(show(f, off_list) == LineShow::ShownNotInBambu);
    CHECK(show(f, "bridge_line_width") == LineShow::ShownNotInBambu);
    // A listed option that is also changed is an ordinary Bambu line: no chip.
    const std::set<std::string> listed_changed = { on_list_simple };
    CHECK(show(bambu(&listed_changed), on_list_simple) == LineShow::Shown);
    // Indexed ids look up the changed set by base key.
    CHECK(line_show(f, { std::string(off_list) + "#0" }, comAdvanced) == LineShow::ShownNotInBambu);
}

TEST_CASE("Widget-only lines follow their group in the Bambu view", "[SettingsView]")
{
    CHECK(widget_line_show(bambu(), true) == LineShow::Shown);
    CHECK(widget_line_show(bambu(), false) == LineShow::Hidden);
}

TEST_CASE("Printer-conditional rows: Bambu's Bambu Lab hides apply on Bambu Lab printers only", "[SettingsView]")
{
    for (const char *k : BambuUiKeys::bbl_printer_hidden()) {
        INFO(k);
        CHECK(SettingsViewKeys::hidden_by_bambu_on_bbl(k));
    }
    REQUIRE(SettingsViewKeys::hidden_by_bambu_on_bbl("default_jerk"));
    ViewFilter other = bambu();
    ViewFilter bbl   = bambu();
    bbl.bbl_printer  = true;
    CHECK(show(other, "default_jerk") == LineShow::Shown);
    CHECK(show(bbl, "default_jerk") == LineShow::Hidden);
    // Changed on a Bambu Lab printer: still shown, with the chip, so a value in force is never invisible.
    const std::set<std::string> changed = { "default_jerk" };
    ViewFilter bbl_changed  = bambu(&changed);
    bbl_changed.bbl_printer = true;
    CHECK(show(bbl_changed, "default_jerk") == LineShow::ShownNotInBambu);
    // Other listed lines are not affected by the printer.
    CHECK(show(bbl, on_list_simple) == LineShow::Shown);
    // Basic / Advanced never apply the Bambu hides.
    ViewFilter adv  = level(comAdvanced);
    adv.bbl_printer = true;
    CHECK(show(adv, "default_jerk") == LineShow::Shown);
}

TEST_CASE("settings_view string mapping", "[SettingsView]")
{
    CHECK(settings_view_from_string("bambu") == SettingsView::Bambu);
    CHECK(settings_view_from_string("") == SettingsView::Default);
    CHECK(settings_view_from_string("Bambu") == SettingsView::Default);
    CHECK(settings_view_from_string("something newer") == SettingsView::Default);
    CHECK(settings_view_to_string(SettingsView::Bambu) == "bambu");
    CHECK(settings_view_to_string(SettingsView::Default).empty());
}

TEST_CASE("Changed rule A: compared with the system preset the edited one inherits", "[SettingsView][Preset]")
{
    TestFilaments filaments;
    const double  sys_flow = default_flow() - 0.05; // the system preset itself differs from the defaults
    const int     sys_temp = default_temp() + 10;
    filaments.add("Sys PLA", "", sys_flow, sys_temp);
    filaments.add("My PLA", "Sys PLA", sys_flow - 0.03, sys_temp); // user changed the flow only
    filaments.find_preset("Sys PLA", false)->is_system = true;

    SECTION("user preset inheriting a system preset")
    {
        REQUIRE(filaments.select_preset_by_name("My PLA", true));
        REQUIRE(settings_view_reference(filaments) == filaments.find_preset("Sys PLA", false));
        std::set<std::string> changed = settings_view_changed_options(filaments);
        CHECK(changed.count("filament_flow_ratio") == 1);
        // Same as the system preset (though not the hard default): not changed.
        CHECK(changed.count("nozzle_temperature") == 0);

        // An unsaved edit counts too.
        filaments.get_edited_preset().config.option<ConfigOptionInts>("nozzle_temperature")->values = { sys_temp + 5 };
        changed = settings_view_changed_options(filaments);
        CHECK(changed.count("nozzle_temperature") == 1);
    }
    SECTION("the system preset itself")
    {
        REQUIRE(filaments.select_preset_by_name("Sys PLA", true));
        // find_preset() answers the edited copy for the selected name; the reference is the stored preset.
        REQUIRE(settings_view_reference(filaments) == &filaments.get_selected_preset());
        REQUIRE(filaments.get_selected_preset().is_system);
        CHECK(settings_view_changed_options(filaments).count("nozzle_temperature") == 0);
        filaments.get_edited_preset().config.option<ConfigOptionFloats>("filament_flow_ratio")->values = { 0.5 };
        CHECK(settings_view_changed_options(filaments).count("filament_flow_ratio") == 1);
    }
    SECTION("the default preset")
    {
        REQUIRE(filaments.select_preset_by_name("Default Filament", true));
        const Preset *ref = settings_view_reference(filaments);
        REQUIRE(ref != nullptr);
        CHECK(ref->is_default);
        CHECK(settings_view_changed_options(filaments).empty());
    }
}

TEST_CASE("Changed rule B: no system preset falls back to the option defaults", "[SettingsView][Preset]")
{
    TestFilaments filaments;
    filaments.add("Root PLA", "", default_flow(), default_temp() + 15); // no inherits: a root user preset

    SECTION("root user preset")
    {
        REQUIRE(filaments.select_preset_by_name("Root PLA", true));
        CHECK(settings_view_reference(filaments) == nullptr);
        const std::set<std::string> changed = settings_view_changed_options(filaments);
        CHECK(changed.count("nozzle_temperature") == 1);
        CHECK(changed.count("filament_flow_ratio") == 0);
    }
    SECTION("external preset with no inherits")
    {
        filaments.find_preset("Root PLA", false)->is_external = true;
        REQUIRE(filaments.select_preset_by_name("Root PLA", true));
        CHECK(settings_view_reference(filaments) == nullptr);
        CHECK(settings_view_changed_options(filaments).count("nozzle_temperature") == 1);
    }
}

TEST_CASE("Hard-default comparison stretches vector defaults to the edited length", "[SettingsView]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("nozzle_temperature", new ConfigOptionInts({ default_temp(), default_temp(), default_temp() }));
    cfg.set_key_value("filament_flow_ratio", new ConfigOptionFloats({ default_flow(), default_flow() + 0.1 }));
    const std::set<std::string> changed = options_differing_from_defaults(cfg);
    CHECK(changed.count("nozzle_temperature") == 0); // three extruders, all at the default
    CHECK(changed.count("filament_flow_ratio") == 1); // second slot changed
}

TEST_CASE("Object and part overrides are always shown on that object", "[SettingsView][Model]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->config.set("staggered_inner_seams", true);
    object->config.set("wall_loops", 4);
    ModelVolume *part = object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    part->config.set("seam_prefer_part_joints", true);

    std::set<std::string> object_keys;
    add_override_keys(object->config.get(), object_keys);
    CHECK(object_keys == std::set<std::string>{ "staggered_inner_seams", "wall_loops" });
    const ViewFilter on_object = bambu(&object_keys);
    CHECK(show(on_object, off_list) == LineShow::ShownNotInBambu);
    CHECK(show(on_object, on_list_simple) == LineShow::Shown);
    CHECK(show(on_object, "seam_prefer_part_joints") == LineShow::Hidden); // the part's override, not the object's

    std::set<std::string> part_keys;
    add_override_keys(part->config.get(), part_keys);
    CHECK(show(bambu(&part_keys), "seam_prefer_part_joints") == LineShow::ShownNotInBambu);
}

// ---------------------------------------------------------------------------------------------
// Wiring proof (view modes PR 3): with AppConfig "settings_view" unset, the GUI's visibility path
// (OG_CustomCtrl lines, ConfigOptionsGroup::is_visible / update_visibility, Page, Tab) asks only
// line_show / widget_line_show / group_visible; these reproduce main's decisions exactly.
// ---------------------------------------------------------------------------------------------
namespace {

// main's ConfigOptionsGroup::is_visible(ConfigOptionMode), verbatim in logic.
bool main_group_is_visible(const std::vector<ConfigOptionMode> &modes, ConfigOptionMode mode)
{
    if (modes.empty())
        return true;
    if (modes.size() == 1)
        return modes[0] <= mode;
    size_t hidden_row_cnt = 0;
    for (auto opt_mode : modes)
        if (opt_mode > mode)
            hidden_row_cnt++;
    return hidden_row_cnt != modes.size();
}

ViewFilter unset_view_filter(ConfigOptionMode level)
{
    ViewFilter f;
    f.level = level;
    f.view  = settings_view_from_string(""); // AppConfig without "settings_view"
    return f;
}

} // namespace

TEST_CASE("Unset settings_view: every option line shows as on main", "[SettingsView][Wiring]")
{
    REQUIRE(settings_view_from_string("") == SettingsView::Default);
    for (ConfigOptionMode lvl : { comSimple, comAdvanced, comDevelop }) {
        const ViewFilter f = unset_view_filter(lvl);
        for (const auto &[key, def] : print_config_def.options) {
            INFO(key << " at level " << int(lvl));
            // main: OG_CustomCtrl::CtrlLine::update_visibility, `line_mode(og_line) <= mode`
            CHECK(line_show(f, { key }, def.mode) == (def.mode <= lvl ? LineShow::Shown : LineShow::Hidden));
            CHECK(line_show(f, { key + "#0", key + "#1" }, def.mode) == (def.mode <= lvl ? LineShow::Shown : LineShow::Hidden));
        }
        // main: a widget-only row counts as Simple, whatever the rest of its group does.
        CHECK(widget_line_show(f, false) == LineShow::Shown);
        CHECK(widget_line_show(f, true) == LineShow::Shown);
    }
}

TEST_CASE("Unset settings_view: every group decision is main's is_visible", "[SettingsView][Wiring]")
{
    // Every sequence of up to four line modes, at every level.
    const ConfigOptionMode all[] = { comSimple, comAdvanced, comDevelop };
    std::vector<std::vector<ConfigOptionMode>> sequences = { {} };
    for (size_t len = 1; len <= 4; ++len) {
        std::vector<std::vector<ConfigOptionMode>> next;
        for (const auto &s : sequences)
            if (s.size() == len - 1)
                for (ConfigOptionMode m : all) {
                    auto t = s;
                    t.push_back(m);
                    next.push_back(t);
                }
        sequences.insert(sequences.end(), next.begin(), next.end());
    }
    REQUIRE(sequences.size() == 1 + 3 + 9 + 27 + 81);
    for (ConfigOptionMode lvl : all)
        for (const auto &modes : sequences) {
            INFO("level " << int(lvl) << ", " << modes.size() << " lines");
            // The GUI passes no ids outside the Bambu view (ConfigOptionsGroup::is_visible).
            CHECK(group_visible(unset_view_filter(lvl), modes, {}) == main_group_is_visible(modes, lvl));
            // With ids it decides the same: ids only matter in the Bambu view.
            std::vector<std::vector<std::string>> ids(modes.size(), std::vector<std::string>{ off_list });
            CHECK(group_visible(unset_view_filter(lvl), modes, ids) == main_group_is_visible(modes, lvl));
        }
}

TEST_CASE("Selector choice <-> AppConfig user_mode + settings_view", "[SettingsView][AppConfig]")
{
    // Reading: what the selector shows.
    CHECK(view_choice("", "") == ViewChoice::Basic); // new install before MainFrame writes "simple"
    CHECK(view_choice("simple", "") == ViewChoice::Basic);
    CHECK(view_choice("advanced", "") == ViewChoice::Advanced);
    CHECK(view_choice("advanced", "bambu") == ViewChoice::Bambu);
    CHECK(view_choice("simple", "bambu") == ViewChoice::Bambu);
    CHECK(view_choice("develop", "") == ViewChoice::Advanced);    // legacy Developer mode: Advanced
    CHECK(view_choice("develop", "bambu") == ViewChoice::Bambu);  // the view is kept
    CHECK(view_choice("something", "") == ViewChoice::Basic);
    CHECK(view_choice("advanced", "newer-view") == ViewChoice::Advanced); // unknown view from a newer build

    // Writing: what each segment stores.
    const ViewChoiceConfig basic = view_choice_config(ViewChoice::Basic);
    CHECK(basic.user_mode == "simple");
    CHECK(basic.settings_view.empty());
    const ViewChoiceConfig advanced = view_choice_config(ViewChoice::Advanced);
    CHECK(advanced.user_mode == "advanced");
    CHECK(advanced.settings_view.empty());
    const ViewChoiceConfig bambu_cfg = view_choice_config(ViewChoice::Bambu);
    CHECK(bambu_cfg.user_mode == "advanced");
    CHECK(bambu_cfg.settings_view == "bambu");

    // Round trip: every selectable choice reads back as itself (restart keeps the choice).
    for (ViewChoice c : { ViewChoice::Basic, ViewChoice::Advanced, ViewChoice::Bambu }) {
        const ViewChoiceConfig cfg = view_choice_config(c);
        CHECK(view_choice(cfg.user_mode, cfg.settings_view) == c);
    }
}

TEST_CASE("Developer mode is a flag on top of the view", "[SettingsView][AppConfig][Developer]")
{
    // Developer mode off: today's levels.
    CHECK(effective_level(ViewChoice::Basic, false) == comSimple);
    CHECK(effective_level(ViewChoice::Advanced, false) == comAdvanced);
    CHECK(effective_level(ViewChoice::Bambu, false) == comAdvanced);
    // Developer mode on: Basic stays Simple; Advanced and Bambu reach comDevelop (Bambu still filtered).
    CHECK(effective_level(ViewChoice::Basic, true) == comSimple);
    CHECK(effective_level(ViewChoice::Advanced, true) == comDevelop);
    CHECK(effective_level(ViewChoice::Bambu, true) == comDevelop);
    // Toggling Developer mode writes only developer_mode: the chosen view reads back unchanged.
    for (ViewChoice c : { ViewChoice::Basic, ViewChoice::Advanced, ViewChoice::Bambu }) {
        const ViewChoiceConfig cfg = view_choice_config(c);
        CHECK(view_choice(cfg.user_mode, cfg.settings_view) == c);
        CHECK(effective_level(view_choice(cfg.user_mode, cfg.settings_view), true) == (c == ViewChoice::Basic ? comSimple : comDevelop));
    }
}

TEST_CASE("Migration: user_mode develop loads as Advanced with Developer mode on", "[SettingsView][AppConfig][Developer]")
{
    std::string user_mode = "develop";
    bool        developer = false;
    CHECK(migrate_legacy_developer_user_mode(user_mode, developer));
    CHECK(user_mode == "advanced");
    CHECK(developer);
    CHECK(view_choice(user_mode, "") == ViewChoice::Advanced);
    CHECK(effective_level(view_choice(user_mode, ""), developer) == comDevelop); // same lines as before
    // A Bambu view written next to it survives.
    std::string um2 = "develop";
    bool        dev2 = true;
    CHECK(migrate_legacy_developer_user_mode(um2, dev2));
    CHECK(view_choice(um2, "bambu") == ViewChoice::Bambu);
    // Nothing else is touched.
    for (const char *m : { "simple", "advanced", "", "something" }) {
        std::string u = m;
        bool        d = false;
        CHECK_FALSE(migrate_legacy_developer_user_mode(u, d));
        CHECK(u == m);
        CHECK_FALSE(d);
    }
}

TEST_CASE("Basic hides Z-buckling bias, the Thin features group and the Bambu defaults row", "[SettingsView][Basic]")
{
    // Owner 2026-11-25. The Thin features group's lines (Tab.cpp TabPrint::build, Quality page): all
    // Advanced, so the group (and its full-width Fine detail note) is hidden in Basic.
    const char *thin_features[] = { "fine_detail", "thin_feature_line_width", "thin_feature_min_size", "fine_detail_speed",
                                    "min_feature_size", "min_bead_width", "initial_layer_min_bead_width", "min_length_factor" };
    std::vector<ConfigOptionMode> thin_modes;
    for (const char *k : thin_features) {
        INFO(k);
        CHECK(mode_of(k) >= comAdvanced);
        thin_modes.push_back(mode_of(k));
    }
    CHECK_FALSE(group_visible(level(comSimple), thin_modes, {}));
    CHECK(group_visible(level(comAdvanced), thin_modes, {}));

    CHECK(mode_of("gyroid_optimized") == comAdvanced);
    CHECK(show(level(comSimple), "gyroid_optimized") == LineShow::Hidden);
    CHECK(show(level(comAdvanced), "gyroid_optimized") == LineShow::Shown);

    // "Use Bambu defaults" is a widget-only row with widget_mode comAdvanced: hidden in Basic, shown in
    // Advanced; in the Bambu view it still follows its group, as before.
    CHECK(widget_line_show(level(comSimple), true, comAdvanced) == LineShow::Hidden);
    CHECK(widget_line_show(level(comAdvanced), true, comAdvanced) == LineShow::Shown);
    CHECK(widget_line_show(level(comDevelop), true, comAdvanced) == LineShow::Shown);
    CHECK(widget_line_show(bambu(), true, comAdvanced) == LineShow::Shown);
    CHECK(widget_line_show(bambu(), false, comAdvanced) == LineShow::Hidden);
    // Other widget-only rows keep showing at every level.
    CHECK(widget_line_show(level(comSimple), false) == LineShow::Shown);
}

TEST_CASE("Add-settings menus follow the view and keep existing overrides", "[SettingsView][Menu]")
{
    const std::string develop_key = "support_air_filtration"; // our Developer key, on Bambu's list
    REQUIRE(mode_of(develop_key) == comDevelop);
    REQUIRE(SettingsViewKeys::on_bambu_list(on_list_simple));
    REQUIRE_FALSE(SettingsViewKeys::on_bambu_list(off_list));

    const std::vector<std::string> keys = { on_list_simple, off_list, develop_key };

    // Basic and Advanced: today's unfiltered menu, whatever the overrides.
    CHECK(filter_add_settings_keys(SettingsView::Default, false, keys, nullptr) == keys);
    CHECK(filter_add_settings_keys(SettingsView::Default, true, keys, nullptr) == keys);

    // Bambu: only Bambu's list; our off-list and Developer keys drop out (the Developer flag changes nothing).
    CHECK(filter_add_settings_keys(SettingsView::Bambu, false, keys, nullptr) == std::vector<std::string>{ on_list_simple });
    const std::set<std::string> none;
    CHECK(filter_add_settings_keys(SettingsView::Bambu, false, keys, &none) == std::vector<std::string>{ on_list_simple });

    // Existing overrides stay listed, in the original order, off-list and Developer ones too.
    const std::set<std::string> own = { off_list, develop_key };
    CHECK(filter_add_settings_keys(SettingsView::Bambu, false, keys, &own) == keys);
    CHECK(add_settings_key_offered(SettingsView::Bambu, false, off_list, &own));
    CHECK_FALSE(add_settings_key_offered(SettingsView::Bambu, false, off_list, nullptr));
    // An "#index" id is matched by its base key.
    CHECK(add_settings_key_offered(SettingsView::Bambu, false, off_list + std::string("#1"), &own));

    // Bambu's BBL-only hides apply on a Bambu Lab printer, unless the key is already overridden.
    REQUIRE(SettingsViewKeys::hidden_by_bambu_on_bbl("default_jerk"));
    CHECK(add_settings_key_offered(SettingsView::Bambu, false, "default_jerk", nullptr) ==
          SettingsViewKeys::on_bambu_list("default_jerk"));
    CHECK_FALSE(add_settings_key_offered(SettingsView::Bambu, true, "default_jerk", nullptr));
    const std::set<std::string> jerk = { "default_jerk" };
    CHECK(add_settings_key_offered(SettingsView::Bambu, true, "default_jerk", &jerk));

    // Every key on the Bambu list that is not a Developer key is offered in the Bambu menu.
    CHECK(add_settings_key_offered(SettingsView::Bambu, false, on_list_simple, nullptr));
}
