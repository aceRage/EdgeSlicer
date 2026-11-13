// Mixed color sublayer, phase 1 (tests/design_mixed_color_sublayer.md): the Ratio pass policy, the per-row
// method, the same-diameter rule, the tower purge and the Bambu Studio 3MF import of mixed filament slots.

#include <catch2/catch.hpp>

#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedSublayer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ToolSlots.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// MixedFilamentManager's auto-generated pairs are a process-wide switch: off for a test, restored after it.
struct MixsubAutoGenerateOff
{
    bool saved { MixedFilamentManager::auto_generate_enabled() };
    MixsubAutoGenerateOff() { MixedFilamentManager::set_auto_generate_enabled(false); }
    ~MixsubAutoGenerateOff() { MixedFilamentManager::set_auto_generate_enabled(saved); }
};

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
    const MixsubAutoGenerateOff auto_generate_off;
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
    const MixsubAutoGenerateOff auto_generate_off;
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
    const MixsubAutoGenerateOff auto_generate_off;
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

TEST_CASE("Mixed sublayer: a Ratio row starts with the filament a Subdivide row on its layer starts with", "[MixedSublayer][P3]")
{
    MixedRatioRowState state;
    std::vector<MixedRatioPass> passes { { 1, 0.14 }, { 2, 0.06 } };
    SECTION("rotated to the shared filament, heights kept")
    {
        CHECK(reorder_mixed_ratio_passes_to_start(passes, 2, state));
        REQUIRE(passes.size() == 2);
        CHECK(passes[0].extruder == 2);
        CHECK_THAT(passes[0].height, WithinAbs(0.06, 1e-12));
        CHECK(passes[1].extruder == 1);
        CHECK_THAT(passes[1].height, WithinAbs(0.14, 1e-12));
        CHECK(state.last_extruder == 1);
    }
    SECTION("already first, or not a component: unchanged")
    {
        state.last_extruder = 2;
        CHECK_FALSE(reorder_mixed_ratio_passes_to_start(passes, 1, state));
        CHECK_FALSE(reorder_mixed_ratio_passes_to_start(passes, 3, state));
        CHECK(passes[0].extruder == 1);
        CHECK(state.last_extruder == 2);
    }
    SECTION("three components keep their cyclic order")
    {
        std::vector<MixedRatioPass> three { { 1, 0.1 }, { 2, 0.05 }, { 3, 0.05 } };
        CHECK(reorder_mixed_ratio_passes_to_start(three, 2, state));
        CHECK(three[0].extruder == 2);
        CHECK(three[1].extruder == 3);
        CHECK(three[2].extruder == 1);
        CHECK(state.last_extruder == 1);
    }
}

// ---- Phase 3: Bambu Studio export (D12) -------------------------------------------------------------------

namespace {

void check_same_mixed_keys(const BambuMixedExport &a, const BambuMixedExport &b)
{
    CHECK(a.slots.is_mixed == b.slots.is_mixed);
    CHECK(a.slots.components == b.slots.components);
    CHECK(a.slots.ratios == b.slots.ratios);
    CHECK(a.slots.gradient == b.slots.gradient);
    CHECK(a.slots.gradient_range == b.slots.gradient_range);
    CHECK(a.slots.gradient_curve == b.slots.gradient_curve);
    CHECK(a.slots.gradient_per_part == b.slots.gradient_per_part);
    CHECK(a.sublayer == b.sublayer);
}

} // namespace

TEST_CASE("Mixed sublayer: Bambu Studio export round trip (D12)", "[MixedSublayer][BambuExport][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    // A Bambu Studio project [P1, P2, M(1,2), P3] as Edge has it after the import: physical 1-3 plus virtual 4.
    DynamicPrintConfig first = bambu_mixed_project_config();
    REQUIRE(convert_bambu_mixed_slots(first, bambu_slots()).converted);

    DynamicPrintConfig     exported = first;
    const BambuMixedExport e1       = expand_bambu_mixed_slots(exported);
    REQUIRE(e1.any);
    CHECK(e1.physical_count == 3);
    CHECK(e1.mixed_count == 1);
    // The row is slot 4: its Edge virtual id, so painted states and filament settings need no remap.
    CHECK((e1.slots.is_mixed == std::vector<bool>{ false, false, false, true }));
    CHECK(e1.slots.components[3] == "1,2");
    CHECK(e1.slots.ratios[3] == "0.7,0.3");
    CHECK_FALSE(e1.slots.gradient[3]);
    // The slot copies its first component's entries; the flush matrix gains a row and a column.
    const auto &colours = exported.option<ConfigOptionStrings>("filament_colour")->values;
    REQUIRE(colours.size() == 4);
    CHECK((exported.option<ConfigOptionStrings>("filament_settings_id")->values ==
           std::vector<std::string>{ "PLA red", "PLA blue", "PLA green", "PLA red" }));
    CHECK((exported.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{ 210, 220, 230, 210 }));
    const std::vector<double> flush = exported.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    REQUIRE(flush.size() == 16);
    CHECK(flush[1 * 4 + 3] == 21.); // 2 -> mixed = 2 -> 1
    CHECK(flush[3 * 4 + 1] == 12.); // mixed -> 2 = 1 -> 2
    CHECK(flush[3 * 4 + 3] == 0.);
    CHECK(exported.opt_string("mixed_filament_definitions").empty());
    CHECK(exported.opt_int("wall_filament") == 3);

    // Importing the export gives the same EdgeSlicer project, and exporting that again the same Bambu keys.
    DynamicPrintConfig second = exported;
    const auto         r2     = convert_bambu_mixed_slots(second, e1.slots);
    REQUIRE(r2.converted);
    CHECK(bambu_mixed_remap_id(r2, 4) == 4);
    CHECK(second.opt_string("mixed_filament_definitions") == first.opt_string("mixed_filament_definitions"));
    CHECK(second.option<ConfigOptionStrings>("filament_colour")->values == first.option<ConfigOptionStrings>("filament_colour")->values);
    CHECK(second.option<ConfigOptionStrings>("filament_settings_id")->values ==
          first.option<ConfigOptionStrings>("filament_settings_id")->values);
    CHECK(second.option<ConfigOptionInts>("nozzle_temperature")->values == first.option<ConfigOptionInts>("nozzle_temperature")->values);
    CHECK(second.option<ConfigOptionFloats>("flush_volumes_matrix")->values == first.option<ConfigOptionFloats>("flush_volumes_matrix")->values);
    CHECK(second.opt_int("wall_filament") == 3);
    DynamicPrintConfig     again = second;
    const BambuMixedExport e2    = expand_bambu_mixed_slots(again);
    check_same_mixed_keys(e1, e2);

    SECTION("rows above the highest used id are not written")
    {
        DynamicPrintConfig cfg = first;
        CHECK_FALSE(expand_bambu_mixed_slots(cfg, 3).any);
        CHECK(cfg.option<ConfigOptionStrings>("filament_colour")->values.size() == 3);
    }
}

TEST_CASE("Mixed sublayer: Bambu Studio export of gradient, three-colour and Subdivide rows", "[MixedSublayer][BambuExport][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(3);
    const std::vector<std::string> colours { "#FF0000", "#0000FF", "#00FF00" };
    config.option<ConfigOptionStrings>("filament_colour", true)->values = colours;
    config.set_key_value("enable_mixed_color_sublayer", new ConfigOptionBool(true));
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 30, colours); // Z gradient 80 % -> 20 % of filament 1
    mgr.add_custom_filament(1, 2, 50, colours); // three colours 50 / 25 / 25
    mgr.add_custom_filament(2, 3, 40, colours); // Subdivide
    REQUIRE(mgr.mixed_filaments().size() == 3);
    MixedFilament &gradient = mgr.mixed_filaments()[0];
    gradient.gradient_enabled = true;
    gradient.gradient_start   = 0.8f;
    gradient.gradient_end     = 0.2f;
    MixedFilament &three             = mgr.mixed_filaments()[1];
    three.gradient_component_ids     = MixedFilamentManager::encode_gradient_component_ids({ 1, 2, 3 });
    three.gradient_component_weights = "50/25/25";
    mgr.mixed_filaments()[2].sublayer_method = int(MixedSublayerMethod::Subdivide);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    DynamicPrintConfig     exported = config;
    const BambuMixedExport e1       = expand_bambu_mixed_slots(exported);
    REQUIRE(e1.any);
    REQUIRE(e1.mixed_count == 3);
    CHECK(e1.slots.components[3] == "1,2");
    CHECK(e1.slots.ratios[3] == "0.7,0.3");
    CHECK(e1.slots.gradient[3]);
    CHECK(e1.slots.gradient_range[3] == "0.8,0.2");
    CHECK(e1.slots.components[4] == "1,2,3");
    CHECK(e1.slots.ratios[4] == "0.5,0.25,0.25");
    CHECK_FALSE(e1.slots.gradient[4]);
    CHECK(e1.slots.components[5] == "2,3");
    CHECK(e1.slots.ratios[5] == "0.6,0.4");
    CHECK(e1.sublayer);
    // Subdivide has no Bambu Studio equivalent; Bambu Studio splits all mixed slots or none.
    CHECK(std::any_of(e1.notes.begin(), e1.notes.end(), [](const std::string &n) { return n.find("Subdivide") != std::string::npos; }));
    CHECK(exported.option<ConfigOptionStrings>("filament_colour")->values.size() == 6);

    // Re-import and re-export: the same keys.
    DynamicPrintConfig back = exported;
    REQUIRE(convert_bambu_mixed_slots(back, e1.slots).converted);
    MixedFilamentManager back_mgr;
    back_mgr.load_custom_entries(back.opt_string("mixed_filament_definitions"), back.option<ConfigOptionStrings>("filament_colour")->values);
    REQUIRE(back_mgr.mixed_filaments().size() == 3);
    CHECK(back_mgr.mixed_filaments()[0].gradient_enabled);
    CHECK_THAT(double(back_mgr.mixed_filaments()[0].gradient_start), WithinAbs(0.8, 1e-4));
    CHECK(back_mgr.mixed_filaments()[1].gradient_component_weights == "50/25/25");
    DynamicPrintConfig     again = back;
    const BambuMixedExport e2    = expand_bambu_mixed_slots(again);
    check_same_mixed_keys(e1, e2);
}

namespace {

std::string mixsub_zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t      size = 0;
    void       *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

std::string mixsub_temp_file(const std::string &name)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / ("snorca_tests_mixsub_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    return (tmp_root / name).string();
}

DynamicPrintConfig mixsub_project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(2);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values  = { 0.4 };
    cfg.option<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#0000FF" };
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    cfg.set_key_value("enable_mixed_color_sublayer", new ConfigOptionBool(true));
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 30, { "#FF0000", "#0000FF" });
    cfg.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
    return cfg;
}

// project_settings.config's mixed keys as text, for a byte comparison.
std::map<std::string, std::string> mixsub_bambu_keys(const std::string &path)
{
    const nlohmann::json project = nlohmann::json::parse(mixsub_zip_entry(path, "Metadata/project_settings.config"));
    std::map<std::string, std::string> out;
    for (const char *key : { "filament_is_mixed", "filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_gradient",
                             "filament_mixed_gradient_range", "enable_mixed_color_sublayer", "filament_colour" })
        out[key] = project.contains(key) ? project[key].dump() : std::string("<missing>");
    return out;
}

bool mixsub_store(const std::string &path, Model &model, DynamicPrintConfig &cfg, BambuExport::Report &report)
{
    PlateData *plate   = new PlateData();
    plate->plate_index = 0;
    plate->objects_and_instances.emplace_back(0, 0);
    StoreParams sp;
    sp.path            = path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = { plate };
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.bambu_compat    = true;
    sp.bambu_report    = &report;
    const bool ok      = store_bbs_3mf(sp);
    PlateDataPtrs plates { plate };
    release_PlateData_list(plates);
    return ok;
}

} // namespace

TEST_CASE("Mixed sublayer: a mixed row survives Export Bambu 3MF and its re-import", "[MixedSublayer][BambuExport][3mf][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "mixed_cube";
    TriangleMesh cube   = make_cube(20., 20., 20.);
    ModelVolume *part   = object->add_volume(cube);
    part->config.set("extruder", 1);
    object->add_instance();
    object->ensure_on_bed();
    TriangleSelector selector(cube);
    for (int facet = 0; facet < 4; ++facet)
        selector.set_facet(facet, EnforcerBlockerType(3)); // the mixed row, virtual filament 3
    REQUIRE(part->mmu_segmentation_facets.set(selector));
    DynamicPrintConfig cfg = mixsub_project_config();

    const std::string   path = mixsub_temp_file("mixsub_bambu.3mf");
    BambuExport::Report report;
    REQUIRE(mixsub_store(path, model, cfg, report));
    const std::map<std::string, std::string> keys = mixsub_bambu_keys(path);
    INFO("filament_is_mixed " << keys.at("filament_is_mixed"));
    CHECK(keys.at("filament_mixed_components") == R"(["","","1,2"])");
    CHECK(keys.at("filament_mixed_sublayer_ratios") == R"(["","","0.7,0.3"])");
    CHECK(keys.at("filament_is_mixed").find("1") != std::string::npos);
    CHECK(keys.at("enable_mixed_color_sublayer").find("1") != std::string::npos);

    // Re-import: two physical filaments plus the row as virtual filament 3; the paint still points at it.
    DynamicPrintConfig        back_cfg;
    Model                     back_model;
    PlateDataPtrs             back_plates;
    std::vector<Preset *>     back_presets;
    bool                      is_bbl = false;
    Semver                    version;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    REQUIRE(load_bbs_3mf(path.c_str(), &back_cfg, &ctxt, &back_model, &back_plates, &back_presets, &is_bbl, &version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence | LoadStrategy::LoadConfig));
    CHECK(back_cfg.option<ConfigOptionStrings>("filament_colour")->values.size() == 2);
    MixedFilamentManager back_mgr;
    back_mgr.load_custom_entries(back_cfg.opt_string("mixed_filament_definitions"), back_cfg.option<ConfigOptionStrings>("filament_colour")->values);
    REQUIRE(back_mgr.mixed_filaments().size() == 1);
    CHECK(back_mgr.mixed_filaments().front().component_a == 1);
    CHECK(back_mgr.mixed_filaments().front().component_b == 2);
    CHECK(back_mgr.mixed_filaments().front().mix_b_percent == 30);
    CHECK(back_cfg.opt_bool("enable_mixed_color_sublayer"));
    REQUIRE(back_model.objects.size() == 1);
    const std::vector<int> painted = back_model.objects.front()->volumes.front()->get_extruders();
    CHECK(std::find(painted.begin(), painted.end(), 3) != painted.end());

    // Export again: byte-identical mixed keys.
    const std::string   path2 = mixsub_temp_file("mixsub_bambu_again.3mf");
    BambuExport::Report report2;
    REQUIRE(mixsub_store(path2, back_model, back_cfg, report2));
    CHECK(mixsub_bambu_keys(path2) == keys);

    release_PlateData_list(back_plates);
    for (Preset *p : back_presets)
        delete p;
    boost::filesystem::remove(path);
    boost::filesystem::remove(path2);
}

TEST_CASE("Mixed sublayer: Bambu Studio import renumbers filament_self_index", "[MixedSublayer][BambuImport][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    SECTION("one entry per filament")
    {
        DynamicPrintConfig config = bambu_mixed_project_config();
        config.option<ConfigOptionInts>("filament_self_index", true)->values = { 1, 2, 3, 4 };
        REQUIRE(convert_bambu_mixed_slots(config, bambu_slots()).converted);
        CHECK((config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{ 1, 2, 3 }));
    }
    SECTION("one entry per filament and extruder variant")
    {
        DynamicPrintConfig config = bambu_mixed_project_config();
        config.option<ConfigOptionInts>("filament_self_index", true)->values = { 1, 1, 2, 2, 3, 3, 4, 4 };
        config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "L1", "R1", "L2", "R2", "LM", "RM", "L4", "R4" };
        REQUIRE(convert_bambu_mixed_slots(config, bambu_slots()).converted);
        CHECK((config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{ 1, 1, 2, 2, 3, 3 }));
        CHECK((config.option<ConfigOptionStrings>("filament_extruder_variant")->values ==
               std::vector<std::string>{ "L1", "R1", "L2", "R2", "L4", "R4" }));
    }
}

TEST_CASE("Mixed sublayer: Bambu import leaves variant arrays alone without a filament_self_index", "[MixedSublayer][BambuImport][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    DynamicPrintConfig config = bambu_mixed_project_config();
    config.option<ConfigOptionInts>("filament_self_index", true)->values = { 1, 2, 3, 4 };
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values = { "a", "b", "c", "d", "e", "f", "g", "h" };
    REQUIRE(convert_bambu_mixed_slots(config, bambu_slots()).converted);
    // Eight entries but no per-entry filament names: nothing says which entries belong to the mixed slot.
    CHECK(config.option<ConfigOptionStrings>("filament_extruder_variant")->values.size() == 8);
    CHECK((config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{ 1, 2, 3 }));
}

TEST_CASE("Mixed sublayer: Bambu export writes a three-colour row as the planner sees it", "[MixedSublayer][BambuExport][P3]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(3);
    const std::vector<std::string> colours { "#FF0000", "#0000FF", "#00FF00" };
    config.option<ConfigOptionStrings>("filament_colour", true)->values = colours;
    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, colours);
    REQUIRE(mgr.mixed_filaments().size() == 1);
    // Ids 1, 2, 1 at 50 / 30 / 20 and a zero-weight 3: the planner prints 1 at 70 % and 2 at 30 %.
    MixedFilament &row             = mgr.mixed_filaments()[0];
    row.gradient_component_ids     = MixedFilamentManager::encode_gradient_component_ids({ 1, 2, 1, 3 });
    row.gradient_component_weights = "50/30/20/0";
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    DynamicPrintConfig     exported = config;
    const BambuMixedExport e        = expand_bambu_mixed_slots(exported);
    REQUIRE(e.any);
    REQUIRE(e.mixed_count == 1);
    MixedFilamentManager check;
    check.load_custom_entries(config.opt_string("mixed_filament_definitions"), colours);
    REQUIRE(check.mixed_filaments().size() == 1);
    const std::vector<MixedRatioComponent> comps = mixed_ratio_components(check.mixed_filaments()[0], 3);
    std::string components;
    for (size_t i = 0; i < comps.size(); ++i)
        components += (i ? "," : "") + std::to_string(comps[i].extruder);
    CHECK(e.slots.components[3] == components);
    // The shares, written with four decimals.
    std::vector<double> written;
    {
        std::stringstream ss(e.slots.ratios[3]);
        std::string       tok;
        while (std::getline(ss, tok, ','))
            written.push_back(std::atof(tok.c_str()));
    }
    REQUIRE(written.size() == comps.size());
    for (size_t i = 0; i < comps.size(); ++i)
        CHECK_THAT(written[i], WithinAbs(comps[i].ratio, 1e-4));
    // No component is written twice.
    CHECK(e.slots.components[3].find("1,2,1") == std::string::npos);
}

// ---- A mixed filament's nozzle (owner hand test on #937) -------------------------------------------------

namespace {

// An H3C-like single extruder with a rack 0.4 / 0.8 / 0.8 / 0.4, four filaments, filament 2 pinned to slot 2 and
// filament 3 to slot 3 (both 0.8), and the mixed filament 2 + 3 at 30 % as filament 5.
DynamicPrintConfig rack_mixed_config(const std::vector<int> &slot_pins)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_filaments(4);
    const std::vector<std::string> colours { "#FF0000", "#00FF00", "#0000FF", "#FFFF00" };
    config.option<ConfigOptionStrings>("filament_colour", true)->values                = colours;
    config.option<ConfigOptionFloats>("nozzle_diameter", true)->values                  = { 0.4 };
    config.option<ConfigOptionStrings>("extruder_rack_nozzle_diameter", true)->values = { "0.4,0.8,0.8,0.4" };
    config.option<ConfigOptionInts>("filament_rack_nozzle_slot", true)->values          = slot_pins;
    MixedFilamentManager mgr;
    mgr.add_custom_filament(2, 3, 30, colours);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
    return config;
}

} // namespace

TEST_CASE("Mixed sublayer: a mixed filament prints through its components' nozzle", "[MixedSublayer][Nozzle]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    SECTION("components pinned to the 0.8 slots")
    {
        const DynamicPrintConfig config = rack_mixed_config({ 0, 2, 3, 0 });
        REQUIRE_THAT(nozzle_diameter_for_filament(config, 2), WithinAbs(0.8, 1e-9));
        CHECK(nozzle_filament_of(config, 5) == 2);
        CHECK_THAT(nozzle_diameter_for_filament(config, 5), WithinAbs(0.8, 1e-9));
        CHECK(filament_rack_slot(config, 5) == filament_rack_slot(config, 2));
        CHECK(nozzle_filament_tool_id(config, 5) == nozzle_filament_tool_id(config, 2));
        CHECK(nozzle_for_filament(config, 5) == nozzle_for_filament(config, 2));
        // Physical filaments are untouched.
        for (unsigned int f = 1; f <= 4; ++f)
            CHECK(nozzle_filament_of(config, f) == f);
    }
    SECTION("components on the 0.4 slots")
    {
        const DynamicPrintConfig config = rack_mixed_config({ 0, 1, 4, 0 });
        CHECK_THAT(nozzle_diameter_for_filament(config, 5), WithinAbs(0.4, 1e-9));
    }
    SECTION("components on different diameters: the finest")
    {
        const DynamicPrintConfig config = rack_mixed_config({ 0, 2, 4, 0 });
        CHECK(nozzle_filament_of(config, 5) == 3);
        CHECK_THAT(nozzle_diameter_for_filament(config, 5), WithinAbs(0.4, 1e-9));
    }
    SECTION("no mixed filament: an id past the physical ones is returned as given")
    {
        DynamicPrintConfig config = rack_mixed_config({ 0, 2, 3, 0 });
        config.set_key_value("mixed_filament_definitions", new ConfigOptionString(""));
        CHECK(nozzle_filament_of(config, 5) == 5);
    }
}

TEST_CASE("Mixed sublayer: per-nozzle filament arrays get an entry per mixed filament", "[MixedSublayer][Nozzle]")
{
    const MixsubAutoGenerateOff auto_generate_off;
    DynamicPrintConfig config = rack_mixed_config({ 0, 2, 3, 0 });
    config.option<ConfigOptionFloatsOrPercents>("nozzle_inner_wall_line_width", true)->values = {
        FloatOrPercent{ 0.45, false }, FloatOrPercent{ 0.82, false }, FloatOrPercent{ 0.83, false }, FloatOrPercent{ 0.46, false } };
    config.option<ConfigOptionStrings>("nozzle_region_values", true)->values = { "a", "b", "c", "d" };
    REQUIRE(extend_nozzle_filament_values_to_mixed(config));
    const auto &widths = config.option<ConfigOptionFloatsOrPercents>("nozzle_inner_wall_line_width")->values;
    REQUIRE(widths.size() == 5);
    CHECK_THAT(widths[4].value, WithinAbs(0.82, 1e-9)); // filament 2's: the 0.8 slot's process
    CHECK(config.option<ConfigOptionStrings>("nozzle_region_values")->values == std::vector<std::string>{ "a", "b", "c", "d", "b" });
    // A second call leaves the extended arrays alone.
    CHECK_FALSE(extend_nozzle_filament_values_to_mixed(config));
    CHECK(config.option<ConfigOptionFloatsOrPercents>("nozzle_inner_wall_line_width")->values.size() == 5);
}
