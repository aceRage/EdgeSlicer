// Mixed color sublayer, phase 1 (tests/design_mixed_color_sublayer.md): the Ratio pass policy, the per-row
// method, the same-diameter rule, the tower purge and the Bambu Studio 3MF import of mixed filament slots.

#include <catch2/catch.hpp>

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedSublayer.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

double pass_sum(const std::vector<MixedRatioPass> &passes)
{
    return std::accumulate(passes.begin(), passes.end(), 0., [](double s, const MixedRatioPass &p) { return s + p.height; });
}

double height_of(const std::vector<MixedRatioPass> &passes, unsigned int extruder)
{
    double h = 0.;
    for (const MixedRatioPass &p : passes)
        if (p.extruder == extruder)
            h += p.height;
    return h;
}

std::vector<MixedRatioComponent> two_components(double ratio_a, double floor_mm)
{
    return { { 1, ratio_a, floor_mm }, { 2, 1. - ratio_a, floor_mm } };
}

} // namespace

TEST_CASE("Mixed sublayer: per-row method resolution", "[MixedSublayer]")
{
    using M = MixedSublayerMethod;
    // Auto takes the process keys; Subdivide wins when both are on.
    CHECK(mixed_sublayer_effective_method(0, false, false, false) == M::Cadence);
    CHECK(mixed_sublayer_effective_method(0, true, false, false) == M::Ratio);
    CHECK(mixed_sublayer_effective_method(0, false, true, false) == M::Subdivide);
    CHECK(mixed_sublayer_effective_method(0, true, true, false) == M::Subdivide);
    // Gradient rows keep Local-Z when nothing else is on (existing Edge behaviour).
    CHECK(mixed_sublayer_effective_method(0, false, false, true) == M::Subdivide);
    CHECK(mixed_sublayer_effective_method(0, true, false, true) == M::Ratio);
    // A row's own method wins over the process keys.
    CHECK(mixed_sublayer_effective_method(int(M::Cadence), true, true, true) == M::Cadence);
    CHECK(mixed_sublayer_effective_method(int(M::Ratio), false, true, false) == M::Ratio);
    CHECK(mixed_sublayer_effective_method(int(M::Subdivide), false, false, false) == M::Subdivide);

    // Legacy mode (the identity path): key off and every row Auto.
    std::vector<MixedFilament> rows(2);
    CHECK(mixed_sublayer_legacy_mode(rows, false));
    CHECK_FALSE(mixed_sublayer_legacy_mode(rows, true));
    rows[1].sublayer_method = int(M::Ratio);
    CHECK_FALSE(mixed_sublayer_legacy_mode(rows, false));
}

TEST_CASE("Mixed sublayer: row method survives serialization, old rows stay byte-identical", "[MixedSublayer]")
{
    const std::vector<std::string> colours = { "#FF0000", "#0000FF", "#00FF00" };
    MixedFilamentManager           mgr;
    mgr.add_custom_filament(1, 2, 30, colours);
    const std::string plain = mgr.serialize_custom_entries();
    CHECK(plain.find(",sm") == std::string::npos);
    CHECK(plain.find(",so") == std::string::npos);

    mgr.mixed_filaments().front().sublayer_method   = int(MixedSublayerMethod::Ratio);
    mgr.mixed_filaments().front().sublayer_bs_order = true;
    const std::string with_method = mgr.serialize_custom_entries();
    CHECK(with_method.find(",sm3") != std::string::npos);
    CHECK(with_method.find(",so1") != std::string::npos);

    MixedFilamentManager loaded;
    loaded.load_custom_entries(with_method, colours);
    REQUIRE(loaded.mixed_filaments().size() == 1);
    const MixedFilament &row = loaded.mixed_filaments().front();
    CHECK(row.sublayer_method == int(MixedSublayerMethod::Ratio));
    CHECK(row.sublayer_bs_order);
    CHECK(row.manual_pattern.empty()); // the tokens are not taken for a pattern
    CHECK(row.mix_b_percent == 30);

    MixedFilamentManager old_rows;
    old_rows.load_custom_entries(plain, colours);
    REQUIRE(old_rows.mixed_filaments().size() == 1);
    CHECK(old_rows.mixed_filaments().front().sublayer_method == int(MixedSublayerMethod::Auto));
    CHECK(old_rows.serialize_custom_entries() == plain);
}

TEST_CASE("Mixed sublayer: Ratio pass heights", "[MixedSublayer]")
{
    SECTION("70/30 at 0.2 mm gives 0.14 + 0.06")
    {
        MixedRatioRowState state;
        const auto passes = plan_mixed_ratio_layer(two_components(0.7, 0.04), 0.2, true, state);
        REQUIRE(passes.size() == 2);
        CHECK(passes[0].extruder == 1);
        CHECK(passes[1].extruder == 2);
        CHECK_THAT(passes[0].height, WithinAbs(0.14, 1e-9));
        CHECK_THAT(passes[1].height, WithinAbs(0.06, 1e-9));
        CHECK_THAT(pass_sum(passes), WithinAbs(0.2, 1e-12));
    }
    SECTION("three components")
    {
        MixedRatioRowState               state;
        std::vector<MixedRatioComponent> comps = { { 1, 0.5, 0.04 }, { 2, 0.3, 0.04 }, { 3, 0.2, 0.04 } };
        const auto                       passes = plan_mixed_ratio_layer(comps, 0.24, true, state);
        REQUIRE(passes.size() == 3);
        CHECK_THAT(height_of(passes, 1), WithinAbs(0.12, 1e-9));
        CHECK_THAT(height_of(passes, 2), WithinAbs(0.072, 1e-9));
        CHECK_THAT(height_of(passes, 3), WithinAbs(0.048, 1e-9));
        CHECK_THAT(pass_sum(passes), WithinAbs(0.24, 1e-12));
    }
    SECTION("heights always sum to the layer height, any layer height")
    {
        MixedRatioRowState state;
        for (double lh : { 0.08, 0.12, 0.16, 0.2, 0.28 }) {
            const auto passes = plan_mixed_ratio_layer(two_components(0.83, 0.05), lh, false, state);
            CHECK_THAT(pass_sum(passes), WithinAbs(lh, 1e-12));
        }
    }
}

TEST_CASE("Mixed sublayer: thin-pass floor and carry-over", "[MixedSublayer]")
{
    // 90/10 at 0.2 mm with a 0.04 mm floor: the 0.02 mm pass is skipped and carried; over 10 layers the minor
    // component still prints its 10 % to within one pass.
    MixedRatioRowState state;
    const auto         comps     = two_components(0.9, 0.04);
    double             minor_sum = 0.;
    double             total     = 0.;
    size_t             skipped   = 0;
    for (int layer = 0; layer < 10; ++layer) {
        const auto passes = plan_mixed_ratio_layer(comps, 0.2, true, state);
        CHECK_THAT(pass_sum(passes), WithinAbs(0.2, 1e-12));
        for (const MixedRatioPass &p : passes)
            CHECK(p.height >= 0.04 - 1e-9);
        const double minor = height_of(passes, 2);
        if (minor == 0.)
            ++skipped;
        minor_sum += minor;
        total += 0.2;
    }
    CHECK(skipped >= 4);
    CHECK(std::abs(minor_sum - 0.1 * total) <= 0.04 + 1e-9);

    SECTION("every component below its floor reverts the layer to cadence")
    {
        MixedRatioRowState small;
        const auto         passes = plan_mixed_ratio_layer(two_components(0.5, 0.06), 0.08, true, small);
        REQUIRE(passes.size() == 1);
        CHECK_THAT(passes[0].height, WithinAbs(0.08, 1e-12));
        // The other component is owed its half and gets the next layer whole.
        const auto next = plan_mixed_ratio_layer(two_components(0.5, 0.06), 0.08, true, small);
        REQUIRE(next.size() == 1);
        CHECK(next[0].extruder != passes[0].extruder);
    }
}

TEST_CASE("Mixed sublayer: pass order", "[MixedSublayer]")
{
    SECTION("alternating: each layer starts with the component that ended the last one")
    {
        MixedRatioRowState state;
        const auto         comps = two_components(0.6, 0.04);
        unsigned int       last  = 0;
        for (int layer = 0; layer < 6; ++layer) {
            const auto passes = plan_mixed_ratio_layer(comps, 0.2, false, state);
            REQUIRE(passes.size() == 2);
            if (last != 0)
                CHECK(passes.front().extruder == last);
            last = passes.back().extruder;
        }
    }
    SECTION("BS order: ascending physical id, the higher id on top")
    {
        MixedRatioRowState state;
        const auto         comps = two_components(0.6, 0.04);
        for (int layer = 0; layer < 4; ++layer) {
            const auto passes = plan_mixed_ratio_layer(comps, 0.2, true, state);
            REQUIRE(passes.size() == 2);
            CHECK(passes.front().extruder == 1);
            CHECK(passes.back().extruder == 2);
        }
    }
    SECTION("alternating with three components rotates")
    {
        MixedRatioRowState               state;
        std::vector<MixedRatioComponent> comps = { { 2, 0.4, 0.04 }, { 5, 0.3, 0.04 }, { 7, 0.3, 0.04 } };
        auto                             l1    = plan_mixed_ratio_layer(comps, 0.3, false, state);
        auto                             l2    = plan_mixed_ratio_layer(comps, 0.3, false, state);
        REQUIRE(l1.size() == 3);
        REQUIRE(l2.size() == 3);
        CHECK(l1[0].extruder == 2);
        CHECK(l1[2].extruder == 7);
        CHECK(l2[0].extruder == 7);
        CHECK(l2[1].extruder == 2);
        CHECK(l2[2].extruder == 5);
    }
}

TEST_CASE("Mixed sublayer: components of a row", "[MixedSublayer]")
{
    MixedFilament row;
    row.component_a   = 1;
    row.component_b   = 3;
    row.mix_b_percent = 30;
    auto comps        = mixed_ratio_components(row, 4);
    REQUIRE(comps.size() == 2);
    CHECK(comps[0].extruder == 1);
    CHECK_THAT(comps[0].ratio, WithinAbs(0.7, 1e-12));
    CHECK(comps[1].extruder == 3);
    CHECK_THAT(comps[1].ratio, WithinAbs(0.3, 1e-12));

    // A Z gradient overrides A's share on this layer.
    comps = mixed_ratio_components(row, 4, 0.25);
    CHECK_THAT(comps[0].ratio, WithinAbs(0.25, 1e-12));

    // 3+ gradient ids use their weights.
    row.gradient_component_ids     = "124";
    row.gradient_component_weights = "50/25/25";
    comps                          = mixed_ratio_components(row, 4);
    REQUIRE(comps.size() == 3);
    CHECK(comps[2].extruder == 4);
    CHECK_THAT(comps[0].ratio, WithinAbs(0.5, 1e-12));

    // A component past the physical count is dropped; one left is not a mix.
    MixedFilament bad;
    bad.component_a = 1;
    bad.component_b = 9;
    CHECK(mixed_ratio_components(bad, 4).empty());
}

TEST_CASE("Mixed sublayer: components must share a nozzle diameter (D8)", "[MixedSublayer]")
{
    // U1 0.4 + 0.6, H3C 0.4 + 0.8.
    const std::vector<double> u1  = { 0.4, 0.6, 0.4, 0.4 };
    const std::vector<double> h3c = { 0.4, 0.4, 0.8, 0.8 };
    CHECK_FALSE(mixed_ratio_diameter_mismatch({ 1, 3 }, u1).mismatch());
    const MixedDiameterMismatch m = mixed_ratio_diameter_mismatch({ 1, 2 }, u1);
    REQUIRE(m.mismatch());
    CHECK(m.filament_a == 1);
    CHECK(m.filament_b == 2);
    CHECK_THAT(m.diameter_b, WithinAbs(0.6, 1e-12));
    CHECK_FALSE(mixed_ratio_diameter_mismatch({ 3, 4 }, h3c).mismatch());
    CHECK(mixed_ratio_diameter_mismatch({ 2, 3 }, h3c).mismatch());
    CHECK(mixed_ratio_diameter_mismatch({ 1, 2, 4 }, h3c).mismatch());
}

TEST_CASE("Mixed sublayer: tool changes and tower purge (D7, D10)", "[MixedSublayer]")
{
    CHECK(mixed_ratio_toolchanges_per_layer(2, false) == 1);
    CHECK(mixed_ratio_toolchanges_per_layer(2, true) == 2);
    CHECK(mixed_ratio_toolchanges_per_layer(3, false) == 2);
    CHECK(mixed_ratio_toolchanges_per_layer(1, false) == 0);
    // With a prime tower: the switch back to the tower plan's filament and the planned change follow the passes.
    CHECK(mixed_ratio_toolchanges_per_layer(2, false, true) == 2);
    CHECK(mixed_ratio_toolchanges_per_layer(3, false, true) == 4);
    CHECK(mixed_ratio_toolchanges_per_layer(1, false, true) == 0);

    // Same nozzle: full flush from the matrix. Across heads: prime only.
    CHECK_THAT(double(mixed_ratio_toolchange_purge_volume(true, 300.f, 1.f, 45.f)), WithinAbs(300., 1e-6));
    CHECK_THAT(double(mixed_ratio_toolchange_purge_volume(true, 200.f, 0.5f, 45.f)), WithinAbs(100., 1e-6));
    CHECK_THAT(double(mixed_ratio_toolchange_purge_volume(true, 20.f, 1.f, 45.f)), WithinAbs(45., 1e-6));
    CHECK_THAT(double(mixed_ratio_toolchange_purge_volume(false, 300.f, 1.f, 45.f)), WithinAbs(45., 1e-6));
}

namespace {

// A Bambu Studio project with slots [P1 red, P2 blue, M(1,2) 70/30, P3 green].
DynamicPrintConfig bambu_mixed_project_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(4);
    config.option<ConfigOptionStrings>("filament_colour", true)->values        = { "#FF0000", "#0000FF", "#808080", "#00FF00" };
    config.option<ConfigOptionStrings>("filament_settings_id", true)->values   = { "PLA red", "PLA blue", "Mixed", "PLA green" };
    config.option<ConfigOptionFloats>("filament_diameter", true)->values       = { 1.75, 1.75, 1.75, 1.75 };
    config.option<ConfigOptionInts>("nozzle_temperature", true)->values        = { 210, 220, 0, 230 };
    std::vector<double> flush(16, 0.);
    for (size_t r = 0; r < 4; ++r)
        for (size_t c = 0; c < 4; ++c)
            flush[r * 4 + c] = r == c ? 0. : double(10 * (r + 1) + (c + 1)); // "rc"
    config.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values = flush;
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(""));
    config.set_key_value("wall_filament", new ConfigOptionInt(4));
    return config;
}

BambuMixedSlots bambu_slots()
{
    BambuMixedSlots slots;
    slots.is_mixed          = { false, false, true, false };
    slots.components        = { "", "", "1,2", "" };
    slots.ratios            = { "", "", "0.7,0.3", "" };
    slots.gradient          = { false, false, false, false };
    slots.gradient_range    = { "", "", "", "" };
    slots.gradient_curve    = { "", "", "", "" };
    slots.gradient_per_part = { false, false, false, false };
    return slots;
}

} // namespace

TEST_CASE("Mixed sublayer: Bambu Studio mixed slots import", "[MixedSublayer][BambuImport]")
{
    MixedFilamentManager::set_auto_generate_enabled(false);
    DynamicPrintConfig config = bambu_mixed_project_config();
    const auto         result = convert_bambu_mixed_slots(config, bambu_slots());
    REQUIRE(result.converted);
    CHECK(result.old_slot_count == 4);
    CHECK(result.new_physical_count == 3);
    REQUIRE((result.removed_slots == std::vector<size_t>{ 2 }));

    // Physical [1, 2, 3] plus the virtual row: state 3 becomes the row (4), state 4 becomes 3.
    CHECK(bambu_mixed_remap_id(result, 1) == 1);
    CHECK(bambu_mixed_remap_id(result, 2) == 2);
    CHECK(bambu_mixed_remap_id(result, 3) == 4);
    CHECK(bambu_mixed_remap_id(result, 4) == 3);
    CHECK(bambu_mixed_remap_id(result, 0) == 0);
    CHECK(bambu_mixed_remap_id(result, 5) == 0);

    // The mixed slot is gone from every per-filament array.
    CHECK((config.option<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#FF0000", "#0000FF", "#00FF00" }));
    CHECK((config.option<ConfigOptionStrings>("filament_settings_id")->values ==
           std::vector<std::string>{ "PLA red", "PLA blue", "PLA green" }));
    CHECK((config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{ 210, 220, 230 }));
    const std::vector<double> flush = config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    REQUIRE(flush.size() == 9);
    CHECK(flush[0 * 3 + 1] == 12.); // 1 -> 2
    CHECK(flush[0 * 3 + 2] == 14.); // 1 -> old 4
    CHECK(flush[2 * 3 + 0] == 41.); // old 4 -> 1
    CHECK(flush[2 * 3 + 1] == 42.);

    // A process feature filament follows the remap.
    CHECK(config.opt_int("wall_filament") == 3);

    // The Edge row: components 1 + 2, 30 % of the second, method Auto.
    MixedFilamentManager mgr;
    const std::vector<std::string> colours = config.option<ConfigOptionStrings>("filament_colour")->values;
    mgr.load_custom_entries(config.opt_string("mixed_filament_definitions"), colours);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    const MixedFilament &row = mgr.mixed_filaments().front();
    CHECK(row.component_a == 1);
    CHECK(row.component_b == 2);
    CHECK(row.mix_b_percent == 30);
    CHECK(row.sublayer_method == int(MixedSublayerMethod::Auto));
    CHECK_FALSE(row.gradient_enabled);
    CHECK(mgr.filament_id_from_mixed_index(0, 3) == 4);
    CHECK(result.notices.empty());
}

TEST_CASE("Mixed sublayer: Bambu Studio import keeps the linear gradient, drops the curve", "[MixedSublayer][BambuImport]")
{
    MixedFilamentManager::set_auto_generate_enabled(false);
    const std::string json = R"({
        "name": "project_settings",
        "filament_is_mixed": ["0", "0", "1", "0"],
        "filament_mixed_components": ["", "", "2,1", ""],
        "filament_mixed_sublayer_ratios": ["", "", "0.5,0.5", ""],
        "filament_mixed_gradient": ["0", "0", "1", "0"],
        "filament_mixed_gradient_range": ["", "", "0.10,0.90", ""],
        "filament_mixed_gradient_curve": ["", "", "0,0.15|0.5,0.50|1,0.85", ""],
        "filament_mixed_gradient_per_part": ["0", "0", "1", "0"]
    })";
    BambuMixedSlots slots;
    REQUIRE(parse_bambu_mixed_slots_json(json, slots));
    REQUIRE(slots.any_mixed());
    REQUIRE(slots.components.size() == 4);
    CHECK(slots.components[2] == "2,1");
    CHECK(slots.gradient[2]);
    CHECK(slots.gradient_per_part[2]);

    DynamicPrintConfig config = bambu_mixed_project_config();
    const auto         result = convert_bambu_mixed_slots(config, slots);
    REQUIRE(result.converted);
    REQUIRE(result.notices.size() == 1);
    CHECK(result.notices.front().find("custom gradient curve") != std::string::npos);
    CHECK(result.notices.front().find("per-part gradient") != std::string::npos);

    MixedFilamentManager mgr;
    mgr.load_custom_entries(config.opt_string("mixed_filament_definitions"), config.option<ConfigOptionStrings>("filament_colour")->values);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    const MixedFilament &row = mgr.mixed_filaments().front();
    // Bambu Studio's first component is A, the gradient range is A's share from bottom to top.
    CHECK(row.component_a == 2);
    CHECK(row.component_b == 1);
    CHECK(row.gradient_enabled);
    CHECK_THAT(double(row.gradient_start), WithinAbs(0.10, 1e-4));
    CHECK_THAT(double(row.gradient_end), WithinAbs(0.90, 1e-4));
}

TEST_CASE("Mixed sublayer: Bambu Studio import edge cases", "[MixedSublayer][BambuImport]")
{
    MixedFilamentManager::set_auto_generate_enabled(false);
    SECTION("no mixed slot leaves the config alone")
    {
        DynamicPrintConfig config = bambu_mixed_project_config();
        BambuMixedSlots    slots  = bambu_slots();
        slots.is_mixed            = { false, false, false, false };
        CHECK_FALSE(convert_bambu_mixed_slots(config, slots).converted);
        CHECK(config.option<ConfigOptionStrings>("filament_colour")->values.size() == 4);
    }
    SECTION("slot count mismatch is refused")
    {
        DynamicPrintConfig config = bambu_mixed_project_config();
        BambuMixedSlots    slots  = bambu_slots();
        slots.is_mixed.push_back(false);
        CHECK_FALSE(convert_bambu_mixed_slots(config, slots).converted);
    }
    SECTION("a mix of a mixed slot is dropped, the slot still leaves the arrays")
    {
        DynamicPrintConfig config = bambu_mixed_project_config();
        BambuMixedSlots    slots  = bambu_slots();
        slots.components[2]       = "1,3";
        const auto result         = convert_bambu_mixed_slots(config, slots);
        REQUIRE(result.converted);
        CHECK(bambu_mixed_remap_id(result, 3) == 0);
        CHECK(bambu_mixed_remap_id(result, 4) == 3);
        CHECK(config.option<ConfigOptionStrings>("filament_colour")->values.size() == 3);
        CHECK(config.opt_string("mixed_filament_definitions").empty());
    }
    SECTION("three components become a gradient-id row with weights")
    {
        DynamicPrintConfig config = bambu_mixed_project_config();
        BambuMixedSlots    slots  = bambu_slots();
        slots.components[2]       = "1,2,4";
        slots.ratios[2]           = "0.5,0.25,0.25";
        const auto result         = convert_bambu_mixed_slots(config, slots);
        REQUIRE(result.converted);
        MixedFilamentManager mgr;
        mgr.load_custom_entries(config.opt_string("mixed_filament_definitions"), config.option<ConfigOptionStrings>("filament_colour")->values);
        REQUIRE(mgr.mixed_filaments().size() == 1);
        const auto comps = mixed_ratio_components(mgr.mixed_filaments().front(), 3);
        REQUIRE(comps.size() == 3);
        CHECK(comps[2].extruder == 3); // old slot 4
        CHECK_THAT(comps[0].ratio, WithinAbs(0.5, 1e-9));
    }
}

// ---- Phase 3 ----------------------------------------------------------------------------------------------

TEST_CASE("Mixed sublayer: AdaptiveLayerMerge band split (D9)", "[MixedSublayer][P3]")
{
    SECTION("a 0.4 band of two 0.2 layers splits 70/30, the first pass reaching above the swallowed layer")
    {
        MixedRatioRowState state;
        const MixedRatioBandPlan plan = plan_mixed_ratio_band(two_components(0.7, 0.04), 0.4, 0.2, false, state);
        REQUIRE(plan.split);
        REQUIRE(plan.passes.size() == 2);
        CHECK_THAT(pass_sum(plan.passes), WithinAbs(0.4, 1e-9));
        CHECK_THAT(height_of(plan.passes, 1), WithinAbs(0.28, 1e-9));
        CHECK_THAT(height_of(plan.passes, 2), WithinAbs(0.12, 1e-9));
        CHECK(plan.passes.front().height >= 0.2 - 1e-9);
        CHECK(state.last_extruder == plan.passes.back().extruder);
    }
    SECTION("the preferred start is too thin: the order rotates to the first tall enough pass")
    {
        MixedRatioRowState state;
        state.last_extruder = 2; // alternating order would start with filament 2 (0.12 of 0.4)
        const MixedRatioBandPlan plan = plan_mixed_ratio_band(two_components(0.7, 0.04), 0.4, 0.2, false, state);
        REQUIRE(plan.split);
        REQUIRE(plan.passes.size() == 2);
        CHECK(plan.passes.front().extruder == 1);
        CHECK(plan.passes.back().extruder == 2);
        CHECK(state.last_extruder == 2);
    }
    SECTION("ascending order keeps the higher id on top when the lower one is tall enough")
    {
        MixedRatioRowState state;
        const MixedRatioBandPlan plan = plan_mixed_ratio_band(two_components(0.7, 0.04), 0.4, 0.2, true, state);
        REQUIRE(plan.passes.size() == 2);
        CHECK(plan.passes.front().extruder == 1);
        CHECK(plan.passes.back().extruder == 2);
    }
    SECTION("no pass is tall enough: the band prints whole by the component owed the most")
    {
        // Three even components on a 0.6 band of three 0.2 layers: 0.2 each, the swallowed part is 0.4.
        const std::vector<MixedRatioComponent> comps { { 1, 1. / 3., 0.04 }, { 2, 1. / 3., 0.04 }, { 3, 1. / 3., 0.04 } };
        MixedRatioRowState state;
        const MixedRatioBandPlan plan = plan_mixed_ratio_band(comps, 0.6, 0.4, false, state);
        CHECK_FALSE(plan.split);
        REQUIRE(plan.passes.size() == 1);
        CHECK_THAT(plan.passes.front().height, WithinAbs(0.6, 1e-9));
        // The carry still balances: the printed one owes the others 0.2 each.
        REQUIRE(state.carry.size() == 3);
        CHECK_THAT(std::accumulate(state.carry.begin(), state.carry.end(), 0.), WithinAbs(0., 1e-9));
        // Over three bands every component prints one whole band.
        std::vector<unsigned int> printed { plan.passes.front().extruder };
        for (int band = 0; band < 2; ++band)
            printed.push_back(plan_mixed_ratio_band(comps, 0.6, 0.4, false, state).passes.front().extruder);
        std::sort(printed.begin(), printed.end());
        CHECK(printed == std::vector<unsigned int>{ 1, 2, 3 });
    }
    SECTION("an unmerged layer (no swallowed height) plans exactly as plan_mixed_ratio_layer")
    {
        MixedRatioRowState a, b;
        a.last_extruder = b.last_extruder = 2;
        const MixedRatioBandPlan          band  = plan_mixed_ratio_band(two_components(0.7, 0.04), 0.2, 0., false, a);
        const std::vector<MixedRatioPass> layer = plan_mixed_ratio_layer(two_components(0.7, 0.04), 0.2, false, b);
        REQUIRE(band.passes.size() == layer.size());
        for (size_t i = 0; i < layer.size(); ++i) {
            CHECK(band.passes[i].extruder == layer[i].extruder);
            CHECK_THAT(band.passes[i].height, WithinAbs(layer[i].height, 1e-12));
        }
        CHECK(a.last_extruder == b.last_extruder);
    }
}
