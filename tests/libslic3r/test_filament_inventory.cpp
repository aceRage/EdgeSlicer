// Filament grouping against the filaments loaded in the printer (H2D / H2C), ported from upstream
// OrcaSlicer #17074 (grouping follows the loaded filaments) and #17195 (a changed AMS inventory
// invalidates the grouping). Catch2 v2.
#include <catch2/catch.hpp>

#include "libslic3r/Print.hpp"
#include "libslic3r/FilamentGroup.hpp"
#include "libslic3r/FilamentGroupUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// One AMS slot in the per-tray shape Plater hands to Print::set_extruder_filament_info.
DynamicPrintConfig make_tray(const std::string &colour, const std::string &type, const std::string &tray_name,
                             bool is_support = false, const std::string &filament_id = "")
{
    DynamicPrintConfig tray;
    tray.set_key_value("filament_colour", new ConfigOptionStrings{colour});
    tray.set_key_value("filament_type", new ConfigOptionStrings{type});
    tray.set_key_value("tray_name", new ConfigOptionStrings{tray_name});
    tray.set_key_value("filament_is_support", new ConfigOptionBools{is_support});
    tray.set_key_value("filament_id", new ConfigOptionStrings{filament_id});
    return tray;
}

// The same slot plus keys the grouping does not consume.
DynamicPrintConfig with_volatile_keys(DynamicPrintConfig tray, double remaining, int temperature, bool changed)
{
    tray.set_key_value("remaining", new ConfigOptionFloats{remaining});
    tray.set_key_value("nozzle_temperature", new ConfigOptionInts{temperature});
    tray.set_key_value("filament_changed", new ConfigOptionBools{changed});
    tray.set_key_value("tag_uid", new ConfigOptionStrings{"0123456789ABCDEF"});
    tray.set_key_value("filament_exist", new ConfigOptionBools{changed});
    return tray;
}

// Outer index = extruder, inner = that extruder's slots in slot order.
using AmsPayload = std::vector<std::vector<DynamicPrintConfig>>;

bool filaments_changed(const AmsPayload &old_payload, const AmsPayload &new_payload)
{
    return FilamentGroupUtils::machine_filaments_changed(old_payload, new_payload);
}

// A two-extruder Bambu printer whose master is the second extruder (H2D: one nozzle on each extruder;
// H2C: six on the second), printing PLA in the given colours. A change between two of them flushes
// extruder_flush[e] on extruder e.
DynamicPrintConfig master_second_extruder_config(int second_extruder_nozzles, const std::vector<std::string> &colours,
                                                 const std::vector<double> &extruder_flush = {100., 100.})
{
    const size_t       n      = colours.size();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values           = {0.4, 0.4};
    config.option<ConfigOptionInts>("extruder_max_nozzle_count", true)->values   = {1, second_extruder_nozzles};
    config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values    = {"Standard#1", "Standard#" + std::to_string(second_extruder_nozzles)};
    config.option<ConfigOptionEnumsGeneric>("extruder_type", true)->values       = {etDirectDrive, etDirectDrive};
    config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type", true)->values  = {nvtStandard, nvtStandard};
    config.option<ConfigOptionInt>("master_extruder_id", true)->value            = 2;
    config.option<ConfigOptionFloat>("machine_load_filament_time", true)->value  = 29.;
    config.option<ConfigOptionFloat>("machine_unload_filament_time", true)->value = 29.;

    config.option<ConfigOptionStrings>("filament_colour", true)->values   = colours;
    config.option<ConfigOptionStrings>("filament_type", true)->values     = std::vector<std::string>(n, "PLA");
    config.option<ConfigOptionBools>("filament_is_support", true)->values = std::vector<unsigned char>(n, 0);
    config.option<ConfigOptionFloats>("filament_diameter", true)->values  = std::vector<double>(n, 1.75);
    config.option<ConfigOptionInts>("filament_map", true)->values         = std::vector<int>(n, 2);
    // One n x n matrix per extruder.
    std::vector<double> flush;
    for (int extruder = 0; extruder < 2; ++extruder)
        for (size_t from = 0; from < n; ++from)
            for (size_t to = 0; to < n; ++to)
                flush.push_back(from == to ? 0. : extruder_flush[extruder]);
    config.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values = flush;
    config.option<ConfigOptionFloat>("flush_multiplier", true)->value       = 1.;
    return config;
}

void add_cube(Model &model)
{
    model.add_object("cube", "", make_cube(20., 20., 20.))->add_instance();
}

} // namespace

// ---- #17195: comparing AMS inventories ---------------------------------------------------------

TEST_CASE("An unchanged AMS inventory reports no filament change", "[FilamentGroup][AmsInventory]")
{
    const AmsPayload payload = {
        {make_tray("#161616", "PLA", "A1", false, "GFL99"), make_tray("#FFFFFF", "PETG", "A2", false, "GFG99")},
        {make_tray("#F95D73", "PLA", "Ext", false, "GFL99")},
    };
    CHECK_FALSE(filaments_changed(payload, payload));
}

TEST_CASE("A tray colour change reports a filament change", "[FilamentGroup][AmsInventory]")
{
    const AmsPayload before = {{make_tray("#161616", "PLA", "A1", false, "GFL99")}, {make_tray("#F95D73", "PLA", "Ext", false, "GFL99")}};
    AmsPayload       after  = before;
    after[0][0]             = make_tray("#00FFFF", "PLA", "A1", false, "GFL99");
    CHECK(filaments_changed(before, after));
}

TEST_CASE("Changing only volatile AMS slot keys reports no filament change", "[FilamentGroup][AmsInventory]")
{
    const AmsPayload before = {{make_tray("#161616", "PLA", "A1", false, "GFL99"), make_tray("#FFFFFF", "PETG", "A2", false, "GFG99")}};
    AmsPayload       after  = before;
    after[0][0]             = with_volatile_keys(before[0][0], 92., 220, true);
    after[0][1]             = with_volatile_keys(before[0][1], 3., 250, true);
    REQUIRE(after[0][0].has("remaining"));
    REQUIRE(after[0][0].has("tag_uid"));
    CHECK_FALSE(filaments_changed(before, after));
}

TEST_CASE("A filament sub-type, tray place or support flag change reports a filament change", "[FilamentGroup][AmsInventory]")
{
    // Same colour and type, a different preset id: PLA Translucent instead of Generic PLA.
    CHECK(filaments_changed(AmsPayload{{make_tray("#161616", "PLA", "A1", false, "GFL99")}},
                            AmsPayload{{make_tray("#161616", "PLA", "A1", false, "GFA17")}}));
    // An AMS slot ("A1") and the external spool ("Ext") group differently.
    const AmsPayload in_ams = {{make_tray("#F95D73", "PLA", "A1", false, "GFL99")}};
    const AmsPayload on_ext = {{make_tray("#F95D73", "PLA", "Ext", false, "GFL99")}};
    CHECK(filaments_changed(in_ams, on_ext));
    CHECK(filaments_changed(on_ext, in_ams));
    CHECK(filaments_changed(AmsPayload{{make_tray("#FFFFFF", "PLA", "A1", true, "GFL99")}},
                            AmsPayload{{make_tray("#FFFFFF", "PLA", "A1", false, "GFL99")}}));
}

TEST_CASE("Swapping or adding AMS trays reports a filament change", "[FilamentGroup][AmsInventory]")
{
    const AmsPayload two = {{make_tray("#161616", "PLA", "A1", false, "GFL99"), make_tray("#FFFFFF", "PLA", "A2", false, "GFL99")}};
    AmsPayload       swapped = two;
    swapped[0][0]            = two[0][1];
    swapped[0][1]            = two[0][0];
    CHECK(filaments_changed(two, swapped));
    const AmsPayload one = {{make_tray("#161616", "PLA", "A1", false, "GFL99")}};
    CHECK(filaments_changed(one, two));
    CHECK(filaments_changed(two, one));
}

TEST_CASE("An empty AMS inventory is unknown, not a change", "[FilamentGroup][AmsInventory]")
{
    const AmsPayload filled = {{make_tray("#161616", "PLA", "A1", false, "GFL99")}};
    CHECK_FALSE(filaments_changed(AmsPayload{}, filled));
    CHECK_FALSE(filaments_changed(filled, AmsPayload{}));
    // A reported slot with no colour is still a reported tray: losing a filament there is a change.
    CHECK(filaments_changed(AmsPayload{{make_tray("", "PLA", "A1", false, "GFL99")}}, filled));
}

TEST_CASE("An AMS slot missing a projected key is ignored by the inventory comparison", "[FilamentGroup][AmsInventory]")
{
    const DynamicPrintConfig keeper = make_tray("#FFFFFF", "PLA", "A2", false, "GFL99");
    SECTION("no tray name")
    {
        DynamicPrintConfig recoloured = make_tray("#00FFFF", "PLA", "A1", false, "GFL99");
        REQUIRE(recoloured.erase("tray_name"));
        DynamicPrintConfig unnamed = make_tray("#161616", "PLA", "A1", false, "GFL99");
        REQUIRE(unnamed.erase("tray_name"));
        CHECK_FALSE(filaments_changed(AmsPayload{{unnamed, keeper}}, AmsPayload{{recoloured, keeper}}));
    }
    SECTION("no filament type")
    {
        DynamicPrintConfig retyped = make_tray("#00FFFF", "PLA", "A1", false, "GFL99");
        REQUIRE(retyped.erase("filament_type"));
        DynamicPrintConfig untyped = make_tray("#161616", "PLA", "A1", false, "GFL99");
        REQUIRE(untyped.erase("filament_type"));
        CHECK_FALSE(filaments_changed(AmsPayload{{untyped, keeper}}, AmsPayload{{retyped, keeper}}));
    }
    SECTION("no colour")
    {
        CHECK_FALSE(filaments_changed(AmsPayload{{make_tray("", "PLA", "A1", false, "GFL99"), keeper}},
                                      AmsPayload{{make_tray("", "PETG", "A1", false, "GFL99"), keeper}}));
    }
    SECTION("an empty option vector is read as missing, not past its end")
    {
        DynamicPrintConfig hollow = make_tray("#161616", "PLA", "A1", false, "GFL99");
        hollow.option<ConfigOptionStrings>("tray_name")->values.clear();
        hollow.option<ConfigOptionBools>("filament_is_support")->values.clear();
        CHECK_FALSE(filaments_changed(AmsPayload{{hollow, keeper}}, AmsPayload{{hollow, keeper}}));
    }
}

TEST_CASE("Different extruder counts are compared bucket by bucket", "[FilamentGroup][AmsInventory]")
{
    const DynamicPrintConfig tray = make_tray("#161616", "PLA", "A1", false, "GFL99");
    SECTION("a further extruder holding a tray is a change")
    {
        const AmsPayload two   = {{tray}, {tray}};
        AmsPayload       three = two;
        three.push_back({make_tray("#FFFFFF", "PETG", "A2", false, "GFG99")});
        CHECK(filaments_changed(two, three));
        CHECK(filaments_changed(three, two));
    }
    SECTION("a second extruder that gains a tray is a change")
    {
        CHECK(filaments_changed(AmsPayload{{tray}}, AmsPayload{{tray}, {tray}}));
    }
    SECTION("an extruder that holds no tray is the same as a missing one")
    {
        CHECK_FALSE(filaments_changed(AmsPayload{{tray}}, AmsPayload{{tray}, {}}));
    }
}

TEST_CASE("The machine filament projection has a bucket for every extruder of the payload", "[FilamentGroup][AmsInventory]")
{
    // It used to be sized 2 and indexed by the payload's extruder: a third extruder wrote past the end.
    const DynamicPrintConfig tray       = make_tray("#161616", "PLA", "A1", false, "GFL99");
    const auto               projection = FilamentGroupUtils::build_full_machine_filaments(AmsPayload{{tray}, {tray}, {tray}});
    REQUIRE(projection.size() == 3);
    CHECK(projection[2].size() == 1);
    CHECK(FilamentGroupUtils::build_full_machine_filaments(AmsPayload{}).size() == 2);
}

// ---- #17195: the Print installs the inventory and invalidates the grouping --------------------

TEST_CASE("A changed AMS inventory takes back the grouping steps", "[FilamentGroup][AmsInventory][Print]")
{
    Model model;
    add_cube(model);
    const DynamicPrintConfig config = master_second_extruder_config(1, {"#FF0000", "#00FF00"});
    Print                    print;
    print.is_BBL_printer() = true;
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    REQUIRE(print.is_step_done(psWipeTower));
    REQUIRE(print.is_step_done(psSkirtBrim));

    const AmsPayload red_green = {{make_tray("#FF0000", "PLA", "A1")}, {make_tray("#00FF00", "PLA", "Ext")}};

    SECTION("through apply()")
    {
        // The setter only stashes the payload; apply() installs it.
        print.set_extruder_filament_info(red_green);
        CHECK(print.get_extruder_filament_info().empty());
        // No inventory before this pass is "unknown", not a change.
        CHECK(print.apply(model, config) != PrintBase::APPLY_STATUS_INVALIDATED);
        REQUIRE(print.get_extruder_filament_info().size() == 2);
        REQUIRE(print.is_step_done(psWipeTower));

        // The same inventory again does not invalidate.
        print.set_extruder_filament_info(red_green);
        CHECK(print.apply(model, config) != PrintBase::APPLY_STATUS_INVALIDATED);
        REQUIRE(print.is_step_done(psWipeTower));

        AmsPayload cyan_green = red_green;
        cyan_green[0]         = {make_tray("#00FFFF", "PLA", "A1")};
        print.set_extruder_filament_info(cyan_green);
        CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_INVALIDATED);
        CHECK_FALSE(print.is_step_done(psWipeTower));
        CHECK_FALSE(print.is_step_done(psSkirtBrim));

        // An empty payload (AMS disconnected) is installed and never invalidates.
        print.process();
        REQUIRE(print.is_step_done(psWipeTower));
        print.set_extruder_filament_info({});
        CHECK(print.apply(model, config) != PrintBase::APPLY_STATUS_INVALIDATED);
        CHECK(print.get_extruder_filament_info().empty());
        CHECK(print.is_step_done(psWipeTower));
    }

    SECTION("through install_pending_extruder_filament_info(), Plater's path after apply()")
    {
        // Nothing pending: nothing happens.
        CHECK_FALSE(print.install_pending_extruder_filament_info());
        print.set_extruder_filament_info(red_green);
        // The first inventory: installed at once, the slice is kept.
        CHECK_FALSE(print.install_pending_extruder_filament_info());
        CHECK(print.get_extruder_filament_info().size() == 2);
        CHECK(print.is_step_done(psWipeTower));
        // Volatile keys only: kept.
        AmsPayload retagged = red_green;
        retagged[0][0]      = with_volatile_keys(red_green[0][0], 50., 220, true);
        print.set_extruder_filament_info(retagged);
        CHECK_FALSE(print.install_pending_extruder_filament_info());
        CHECK(print.is_step_done(psWipeTower));
        // A spool swapped on the printer: the grouping steps are taken back, so a Slice groups again.
        AmsPayload cyan_green = red_green;
        cyan_green[0]         = {make_tray("#00FFFF", "PLA", "A1")};
        print.set_extruder_filament_info(cyan_green);
        CHECK(print.install_pending_extruder_filament_info());
        CHECK_FALSE(print.is_step_done(psWipeTower));
        CHECK_FALSE(print.is_step_done(psSkirtBrim));
        // Already taken back: a second change reports nothing more to take back.
        print.set_extruder_filament_info(red_green);
        CHECK_FALSE(print.install_pending_extruder_filament_info());
        REQUIRE(print.get_extruder_filament_info().size() == 2);
        CHECK(print.get_extruder_filament_info()[0][0].opt_serialize("filament_colour") == "#FF0000");
    }
}

// ---- #17074: grouping follows the printer's loaded filaments -----------------------------------
// Edge already runs BambuStudio's grouping engine (calc_match_mode_cost, calc_one_change_budget, the
// no-machine-filament early return in select_best_group_for_ams); these pin that its AMS match honours the
// loaded filaments. Upstream's master-extruder-preference wrapper does not exist in Edge, so its cases are
// not here; ToolOrdering::get_recommended_filament_maps is private here, so the engine is called directly.

namespace {
MultiNozzleUtils::NozzleInfo nozzle_on(int extruder_id)
{
    MultiNozzleUtils::NozzleInfo n;
    n.diameter    = "0.4";
    n.volume_type = NozzleVolumeType::nvtStandard;
    n.extruder_id = extruder_id;
    n.group_id    = extruder_id;
    return n;
}

FilamentGroupUtils::FilamentInfo pla(const std::string &colour)
{
    FilamentGroupUtils::FilamentInfo f;
    f.color      = FilamentGroupUtils::Color(colour);
    f.type       = "PLA";
    f.is_support = false;
    f.usage_type = FilamentUsageType::ModelOnly;
    return f;
}

FilamentGroupUtils::MachineFilamentInfo loaded(const std::string &colour, const std::string &type, int extruder_id)
{
    FilamentGroupUtils::MachineFilamentInfo m;
    m.color       = FilamentGroupUtils::Color(colour);
    m.type        = type;
    m.is_support  = false;
    m.usage_type  = FilamentUsageType::ModelOnly;
    m.extruder_id = extruder_id;
    m.is_extended = false;
    return m;
}
} // namespace

TEST_CASE("The AMS match picks the grouping that puts a filament where the printer has it loaded", "[FilamentGroup][AmsInventory]")
{
    const std::vector<MultiNozzleUtils::NozzleInfo> nozzles = {nozzle_on(0), nozzle_on(1)};
    const std::vector<unsigned int>                 used    = {0};
    const std::vector<FilamentGroupUtils::FilamentInfo> info = {pla("#F98C36")};
    // Candidate 0 puts the filament on the second extruder, candidate 1 on the first.
    const std::vector<std::vector<int>> candidates = {{1}, {0}};
    // Only the first extruder holds the orange PLA.
    const std::vector<std::vector<FilamentGroupUtils::MachineFilamentInfo>> machine = {
        {loaded("#F98C36", "PLA", 0), loaded("#BCBCBC", "PETG", 0)}, {loaded("#F95D73", "PETG", 1)}};
    CHECK(select_best_group_for_ams(candidates, nozzles, used, info, machine) == std::vector<int>{0});
}

TEST_CASE("With no filament reported by the printer the AMS match keeps the first candidate", "[FilamentGroup][AmsInventory]")
{
    const std::vector<MultiNozzleUtils::NozzleInfo> nozzles = {nozzle_on(0), nozzle_on(1)};
    const std::vector<unsigned int>                 used    = {0, 1};
    const std::vector<FilamentGroupUtils::FilamentInfo> info = {pla("#FFFFFF"), pla("#000000")};
    const std::vector<std::vector<int>> candidates = {{1, 1}, {0, 1}, {0, 0}};
    CHECK(select_best_group_for_ams(candidates, nozzles, used, info, {}) == std::vector<int>{1, 1});
    CHECK(select_best_group_for_ams(candidates, nozzles, used, info, {{}, {}}) == std::vector<int>{1, 1});
}
