#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentCliGates.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <vector>

using namespace Slic3r;

// Tests for the pure CLI mixed-filament gate logic extracted from Snapmaker_Orca.cpp
// (src/libslic3r/MixedFilamentCliGates.hpp/.cpp) into PR #29's port of OrcaSlicer #15636's
// wipe/flush/type-compatibility gates. Snapmaker_Orca.cpp is the application's main and is not
// itself unit-testable, so these tests exercise the extracted decision functions directly with
// hand-checkable inputs; the CLI call sites forward to these functions unchanged.

namespace {

// 4 physical filaments, colours are arbitrary/unused by the gates themselves.
DynamicPrintConfig four_physical_filament_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(4);
    config.set_num_filaments(4);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};
    return config;
}

const std::vector<std::string> four_colors = {"#FF0000", "#00FF00", "#0000FF", "#FFFF00"};

} // namespace

// ============================================================================
// Gate: cli_check_mixed_filament_slots_have_filament
// ============================================================================

TEST_CASE("CLI mixed filament slot gate passes when the feature is off (no definitions, no enabled rows)", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr; // never auto_generate()'d or loaded: enabled_count() == 0
    REQUIRE(mgr.enabled_count() == 0);

    const std::vector<Model> models; // no loaded objects
    DynamicPrintConfig       print_config = four_physical_filament_config();

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_slots_have_filament(mgr, /*mixed_defs=*/"", /*num_physical=*/4, models, print_config,
                                                       /*filament_count=*/4);
    CHECK(verdict.ok);
    CHECK(verdict.message.empty());
}

TEST_CASE("CLI mixed filament slot gate passes for a valid combination: 4 physical + 1 enabled mixed row, all ids in range", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    REQUIRE(mgr.enabled_count() == 1);
    // With 4 physical filaments the one enabled mixed row is virtual id 4+1 = 5.
    REQUIRE(mgr.filament_id_from_mixed_index(0, 4) == 5);

    const std::string mixed_defs = mgr.serialize_custom_entries();

    // Model uses filament id 5 (the mixed slot) on its wall_filament option: a legitimate use of
    // a mixed slot, not a "slot without filament" violation.
    Model model;
    ModelObject *object = model.add_object();
    object->config.set("wall_filament", 5);

    std::vector<Model> models;
    models.push_back(std::move(model));

    DynamicPrintConfig print_config = four_physical_filament_config();

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_slots_have_filament(mgr, mixed_defs, /*num_physical=*/4, models, print_config,
                                                       /*filament_count=*/4);
    CHECK(verdict.ok);
}

TEST_CASE("CLI mixed filament slot gate refuses a serialized definition naming a slot beyond num_physical", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.enabled_count() == 1);

    // Hand-construct a serialized row that references physical filament 9, but only 4 physical
    // filaments (ids 1-4) are loaded: "9,2,1" = component_a=9, component_b=2, enabled=1.
    // mixed_definitions_have_slot_without_filament flags this because 9 > num_physical(4).
    const std::string bad_defs = "9,2,1;";

    const std::vector<Model> models;
    DynamicPrintConfig       print_config = four_physical_filament_config();

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_slots_have_filament(mgr, bad_defs, /*num_physical=*/4, models, print_config,
                                                       /*filament_count=*/4);
    CHECK_FALSE(verdict.ok);
    CHECK(verdict.message.find("mixed filament slot has no filament of its own") != std::string::npos);
    // The message reports the loaded filament_count we passed in (4).
    CHECK(verdict.message.find("only 4 filaments are loaded") != std::string::npos);
}

TEST_CASE("CLI mixed filament slot gate refuses a model reference to a filament id beyond num_physical that is not a mixed slot", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.enabled_count() == 1);
    // Only virtual id 5 (4 physical + 1) is a real mixed slot.
    REQUIRE(mgr.is_mixed(5, 4));
    REQUIRE_FALSE(mgr.is_mixed(6, 4));

    const std::string mixed_defs = mgr.serialize_custom_entries();

    // Model references filament id 6: beyond the 4 physical filaments, and not a mixed slot
    // either (only 5 is). This must be refused.
    Model model;
    ModelObject *object = model.add_object();
    object->config.set("solid_infill_filament", 6);

    std::vector<Model> models;
    models.push_back(std::move(model));

    DynamicPrintConfig print_config = four_physical_filament_config();

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_slots_have_filament(mgr, mixed_defs, /*num_physical=*/4, models, print_config,
                                                       /*filament_count=*/4);
    CHECK_FALSE(verdict.ok);
    CHECK(verdict.message.find("mixed filament slot 6 has no filament of its own") != std::string::npos);
    CHECK(verdict.message.find("only 4 filaments are loaded") != std::string::npos);
}

// ============================================================================
// Gate: cli_check_mixed_filament_type_compatibility
// ============================================================================

TEST_CASE("CLI mixed filament type gate passes when the components share the same filament_type", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.filament_id_from_mixed_index(0, 4) == 5);

    DynamicPrintConfig cfg = four_physical_filament_config();
    // filaments 1 and 2 (0-based indices 0 and 1) are both "PLA".
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PLA", "PETG", "PETG"};

    const std::vector<int> plate_slots = {1, 2, 5}; // plate uses physical 1, 2, and the mixed slot 5.

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_type_compatibility(mgr, plate_slots, /*num_physical=*/4, cfg, /*plate_index_1based=*/1);
    CHECK(verdict.ok);
}

TEST_CASE("CLI mixed filament type gate refuses a mixed slot whose components differ in filament_type", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    // Mixed slot combines physical filament 1 (PLA) and physical filament 3 (PETG): a genuine
    // type mismatch.
    mgr.add_custom_filament(1, 3, 50, four_colors);
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.filament_id_from_mixed_index(0, 4) == 5);

    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PLA", "PETG", "PETG"};

    const std::vector<int> plate_slots = {5}; // plate uses only the mixed slot.

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_type_compatibility(mgr, plate_slots, /*num_physical=*/4, cfg, /*plate_index_1based=*/3);
    CHECK_FALSE(verdict.ok);
    CHECK(verdict.message.find("plate 3: mixed filament 5 mixes components of different filament types") != std::string::npos);
}

TEST_CASE("CLI mixed filament type gate is skipped entirely when no mixed rows are enabled", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr; // enabled_count() == 0: the feature-off case.
    REQUIRE(mgr.enabled_count() == 0);

    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PETG", "ABS", "TPU"};

    // Even a slot id that would be an out-of-range mixed id if the feature were on must not be
    // inspected: is_mixed() returns false unconditionally when nothing is enabled, so the loop
    // in the gate never calls mixed_filament_from_id/get_filament_type at all.
    const std::vector<int> plate_slots = {1, 2, 3, 4, 5};

    const CliMixedFilamentVerdict verdict =
        cli_check_mixed_filament_type_compatibility(mgr, plate_slots, /*num_physical=*/4, cfg, /*plate_index_1based=*/1);
    CHECK(verdict.ok);
}

// ============================================================================
// Supporting pure helpers used by the gates above
// ============================================================================

TEST_CASE("zero_mixed_flush_rows_and_cols zeros the row and column of a mixed slot only", "[MixedFilamentCli]")
{
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, four_colors);
    REQUIRE(mgr.filament_id_from_mixed_index(0, 4) == 5);

    // 5x5 matrix (4 physical + 1 mixed), flattened row-major, all entries start at a distinct
    // nonzero value: matrix[from][to] = from*5 + to + 1 (so no accidental zero already present).
    const size_t         n = 5;
    std::vector<double>  matrix(n * n);
    for (size_t from = 0; from < n; ++from)
        for (size_t to = 0; to < n; ++to)
            matrix[from * n + to] = double(from * n + to + 1);

    zero_mixed_flush_rows_and_cols(matrix, n, mgr, /*num_physical=*/4);

    for (size_t from = 0; from < n; ++from) {
        for (size_t to = 0; to < n; ++to) {
            const double value = matrix[from * n + to];
            // 0-based index 4 is filament id 5 (the mixed slot): its row and column are zeroed,
            // as is every from==to diagonal cell.
            if (from == to || from == 4 || to == 4)
                CHECK(value == 0.0);
            else
                CHECK(value == double(from * n + to + 1));
        }
    }
}

TEST_CASE("mixed_definitions_have_slot_without_filament flags an out-of-range component and accepts an in-range one", "[MixedFilamentCli]")
{
    // "1,2,1" = component_a=1, component_b=2, enabled=1: both within num_physical=4.
    CHECK_FALSE(mixed_definitions_have_slot_without_filament("1,2,1;", 4));
    // "1,9,1": component_b=9 is beyond num_physical=4.
    CHECK(mixed_definitions_have_slot_without_filament("1,9,1;", 4));
    // A disabled row (third field 0) referencing an out-of-range id is not flagged.
    CHECK_FALSE(mixed_definitions_have_slot_without_filament("1,9,0;", 4));
    // A deleted row ("d1" token present) referencing an out-of-range id is not flagged.
    CHECK_FALSE(mixed_definitions_have_slot_without_filament("1,9,1,d1;", 4));
    // Empty serialization: nothing to flag.
    CHECK_FALSE(mixed_definitions_have_slot_without_filament("", 4));
}

TEST_CASE("mixed_physical_component_ids resolves component_a/component_b for a row with no manual pattern", "[MixedFilamentCli]")
{
    MixedFilament mf;
    mf.component_a = 2;
    mf.component_b = 3;
    mf.manual_pattern.clear();

    const std::vector<unsigned int> ids = mixed_physical_component_ids(mf, /*num_physical=*/4);
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == 2);
    CHECK(ids[1] == 3);
}

TEST_CASE("mixed_components_differ_in_filament_type reports no difference for a single-component resolution", "[MixedFilamentCli]")
{
    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 1; // degenerate: resolves to the single id {1} after dedup.

    DynamicPrintConfig cfg = four_physical_filament_config();
    cfg.option<ConfigOptionStrings>("filament_type")->values = {"PLA", "PETG", "ABS", "TPU"};

    CHECK_FALSE(mixed_components_differ_in_filament_type(mf, cfg, 4));
}
