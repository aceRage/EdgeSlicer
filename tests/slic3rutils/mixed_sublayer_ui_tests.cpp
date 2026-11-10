#include <catch2/catch.hpp>

#include "slic3r/GUI/MixedSublayerUi.hpp"

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI::MixedSublayerUi;
using M = MixedSublayerMethod;

// Mixed color sublayer P2 (GUI). The widgets are hand-tested; these are the decisions and the arithmetic behind
// them: method / badge, the dialog's cost line, the broken (mixed diameter) row, which rows an object uses, the
// preview legend's percents and the Bambu Studio import notice.

static MixedFilament make_row(unsigned a, unsigned b, int pct_b, int method = 0)
{
    MixedFilament mf;
    mf.component_a     = a;
    mf.component_b     = b;
    mf.mix_b_percent   = pct_b;
    mf.sublayer_method = method;
    mf.custom          = true;
    mf.enabled         = true;
    return mf;
}

TEST_CASE("Mixed sublayer UI: method and sidebar badge", "[MixedSublayerUi]")
{
    const ProcessDefaults off {};
    const ProcessDefaults ratio_on { true, false };
    const ProcessDefaults subdivide_on { false, true };
    const ProcessDefaults both_on { true, true };

    SECTION("an Auto row follows the process keys; Subdivide wins over Ratio")
    {
        const MixedFilament row = make_row(1, 2, 30);
        CHECK(effective_method(row, off) == M::Cadence);
        CHECK(effective_method(row, ratio_on) == M::Ratio);
        CHECK(effective_method(row, subdivide_on) == M::Subdivide);
        CHECK(effective_method(row, both_on) == M::Subdivide);
        CHECK(badge_for_row(row, off) == Badge::Hidden);
        CHECK(badge_for_row(row, ratio_on) == Badge::Ratio);
        CHECK(badge_for_row(row, subdivide_on) == Badge::Subdivide);
    }
    SECTION("a row's own method wins over the process keys")
    {
        CHECK(badge_for_row(make_row(1, 2, 30, int(M::Ratio)), off) == Badge::Ratio);
        CHECK(badge_for_row(make_row(1, 2, 30, int(M::Subdivide)), ratio_on) == Badge::Subdivide);
        // Whole layers chosen on purpose: shown, so it is not mistaken for a row the toggle forgot.
        CHECK(badge_for_row(make_row(1, 2, 30, int(M::Cadence)), ratio_on) == Badge::Cadence);
        CHECK(badge_for_row(make_row(1, 2, 30, int(M::Cadence)), off) == Badge::Cadence);
    }
    SECTION("a Z gradient row sub-layers by itself, no process toggle needed")
    {
        MixedFilament grad = make_row(1, 2, 50);
        grad.gradient_enabled = true;
        CHECK(row_is_gradient(grad));
        CHECK(effective_method(grad, off) == M::Subdivide);
        CHECK(effective_method(grad, ratio_on) == M::Ratio);
        grad.component_b = 1; // same filament twice is no gradient
        CHECK_FALSE(row_is_gradient(grad));
    }
    SECTION("badge texts and the dialog's choice index")
    {
        CHECK(std::string(badge_text(Badge::Ratio)) == "Ratio");
        CHECK(std::string(badge_text(Badge::Subdivide)) == "Subdivide");
        CHECK(std::string(badge_text(Badge::Cadence)) == "Layers");
        CHECK(std::string(badge_text(Badge::Hidden)).empty());
        for (int m = 0; m <= 3; ++m) {
            CHECK(method_to_choice(m) == m);
            CHECK(choice_to_method(m) == m);
        }
        CHECK(method_to_choice(7) == 3);
        CHECK(choice_to_method(-1) == 0);
    }
}

static CostLineTemplates english()
{
    return { "About %1% tool changes, %2% g flush", "About %1% tool changes", "About %1% tool changes and %2% g flush per layer",
             "About %1% tool changes per layer" };
}

TEST_CASE("Mixed sublayer UI: the dialog's cost line", "[MixedSublayerUi]")
{
    CostInput in;
    in.components       = { 1, 2 };
    in.num_physical     = 2;
    in.flush_matrix     = { 0., 200., 300., 0. }; // 1->2 200 mm3, 2->1 300 mm3
    in.flush_multiplier = 1.;
    in.density          = { 1.24, 1.24 };
    in.single_nozzle    = true;

    SECTION("single nozzle, no tower, 100 layers: one change per layer, mean flush 250 mm3")
    {
        in.layers = 100;
        const CostEstimate c = estimate_cost(in);
        REQUIRE(c.valid);
        CHECK(c.per_layer == 1);
        CHECK(c.changes == 100);
        CHECK(c.flush_mm3 == Approx(25000.));
        CHECK(c.flush_g == Approx(31.));
        CHECK(format_cost_line(c, english()) == "About 100 tool changes, 31 g flush");
    }
    SECTION("a prime tower doubles the changes (the G-code goes back to the tower plan's filament)")
    {
        in.layers     = 100;
        in.wipe_tower = true;
        const CostEstimate c = estimate_cost(in);
        CHECK(c.per_layer == 2);
        CHECK(c.changes == 200);
        CHECK(format_cost_line(c, english()) == "About 200 tool changes, 62 g flush");
    }
    SECTION("Bambu order changes once per component")
    {
        in.layers   = 10;
        in.bs_order = true;
        CHECK(estimate_cost(in).per_layer == 2);
    }
    SECTION("the flush multiplier and the density scale the grams")
    {
        in.layers           = 10;
        in.flush_multiplier = 2.;
        in.density          = { 1.0, 2.0 };
        const CostEstimate c = estimate_cost(in);
        CHECK(c.flush_mm3 == Approx(5000.));
        CHECK(c.flush_g == Approx(7.5));
        CHECK(format_cost_line(c, english()) == "About 10 tool changes, 7.5 g flush");
    }
    SECTION("no object yet: the line is per layer")
    {
        in.layers = 0;
        const CostEstimate c = estimate_cost(in);
        CHECK(c.changes == 0);
        CHECK(c.flush_mm3 == Approx(250.));
        CHECK(format_cost_line(c, english()) == "About 1 tool changes and 0.3 g flush per layer");
    }
    SECTION("a toolchanger counts changes and no flush")
    {
        in.single_nozzle = false;
        in.layers        = 50;
        const CostEstimate c = estimate_cost(in);
        CHECK(c.changes == 50);
        CHECK(c.flush_mm3 == 0.);
        CHECK(format_cost_line(c, english()) == "About 50 tool changes");
        in.layers = 0;
        CHECK(format_cost_line(estimate_cost(in), english()) == "About 1 tool changes per layer");
    }
    SECTION("fewer than two usable components: no line")
    {
        in.components = { 1, 1 };
        CHECK_FALSE(estimate_cost(in).valid);
        in.components = { 1, 9 };
        CHECK_FALSE(estimate_cost(in).valid);
        CHECK(format_cost_line(estimate_cost(in), english()).empty());
    }
    SECTION("a missing density falls back to PLA")
    {
        in.layers  = 100;
        in.density = {};
        CHECK(estimate_cost(in).flush_g == Approx(31.));
    }
    SECTION("grams text")
    {
        CHECK(format_grams(0.04) == "0.0");
        CHECK(format_grams(9.94) == "9.9");
        CHECK(format_grams(12.4) == "12");
        CHECK(format_grams(1234.4) == "1234");
    }
}

TEST_CASE("Mixed sublayer UI: broken row on mismatched nozzle diameters", "[MixedSublayerUi]")
{
    const std::vector<double> u1_mixed = { 0.4, 0.6, 0.4, 0.4 };
    const ProcessDefaults     ratio_on { true, false };
    const ProcessDefaults     off {};

    const MixedDiameterMismatch broken = broken_ratio_row(make_row(1, 2, 30), ratio_on, 4, u1_mixed);
    REQUIRE(broken.mismatch());
    CHECK(broken.filament_a == 1);
    CHECK(broken.filament_b == 2);
    CHECK(broken.diameter_a == Approx(0.4));
    CHECK(broken.diameter_b == Approx(0.6));

    CHECK_FALSE(broken_ratio_row(make_row(1, 3, 30), ratio_on, 4, u1_mixed).mismatch());
    // Only Ratio rows have the rule: the same mix printed as whole layers or Subdivide is fine.
    CHECK_FALSE(broken_ratio_row(make_row(1, 2, 30), off, 4, u1_mixed).mismatch());
    CHECK_FALSE(broken_ratio_row(make_row(1, 2, 30, int(M::Subdivide)), ratio_on, 4, u1_mixed).mismatch());
    CHECK(broken_ratio_row(make_row(1, 2, 30, int(M::Ratio)), off, 4, u1_mixed).mismatch());
}

TEST_CASE("Mixed sublayer UI: rows an object uses, plate tab and legend", "[MixedSublayerUi]")
{
    const std::vector<std::string> colours = { "#FF0000", "#0000FF", "#00FF00" };
    MixedFilamentManager           mgr;
    mgr.add_custom_filament(1, 2, 30, colours); // virtual 4
    mgr.add_custom_filament(2, 3, 50, colours); // virtual 5
    REQUIRE(mgr.mixed_filaments().size() >= 2);
    const size_t n = 3;
    const unsigned int v_a = mgr.filament_id_from_mixed_index(0, n);
    const unsigned int v_b = mgr.filament_id_from_mixed_index(1, n);
    REQUIRE(v_a == 4);
    REQUIRE(v_b == 5);

    Model        model;
    ModelObject *object = model.add_object();
    ModelVolume *part   = object->add_volume(make_cube(10., 10., 10.));
    object->add_instance();
    object->config.set("extruder", 1);

    const ProcessDefaults ratio_on { true, false };
    const ProcessDefaults off {};

    SECTION("plain physical filaments use no mixed row")
    {
        CHECK(mixed_ids_used_by_object(*object, mgr, n).empty());
        CHECK_FALSE(object_uses_ratio_rows(*object, mgr, n, ratio_on));
    }
    SECTION("a part on a mixed filament, and a mixed wall filament on the object")
    {
        part->config.set("extruder", int(v_b));
        object->config.set("wall_filament", int(v_a));
        const std::vector<unsigned int> used = mixed_ids_used_by_object(*object, mgr, n);
        REQUIRE(used.size() == 2);
        CHECK(used[0] == v_a);
        CHECK(used[1] == v_b);
        CHECK(object_uses_ratio_rows(*object, mgr, n, ratio_on));
        CHECK_FALSE(object_uses_ratio_rows(*object, mgr, n, off));
    }
    SECTION("a height range on a mixed filament")
    {
        object->layer_config_ranges[{ 2., 4. }].set("extruder", int(v_a));
        const std::vector<unsigned int> used = mixed_ids_used_by_object(*object, mgr, n);
        REQUIRE(used.size() == 1);
        CHECK(used[0] == v_a);
    }
    SECTION("the plate tab locks its sequences while any row prints as Ratio")
    {
        CHECK(any_ratio_row(mgr, ratio_on));
        CHECK_FALSE(any_ratio_row(mgr, off));
        mgr.mixed_filaments()[1].sublayer_method = int(M::Ratio);
        CHECK(any_ratio_row(mgr, off));
        mgr.mixed_filaments()[1].deleted = true;
        CHECK_FALSE(any_ratio_row(mgr, off));
    }
    SECTION("legend rows: components with whole percents that add up to 100")
    {
        const std::vector<LegendRow> rows = legend_rows(mgr, { v_b, v_a, v_a, 2, 99 }, n, ratio_on);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].virtual_id == v_a);
        CHECK(rows[0].method == M::Ratio);
        REQUIRE(rows[0].components.size() == 2);
        CHECK(rows[0].components[0].filament == 1);
        CHECK(rows[0].components[0].percent == 70);
        CHECK(rows[0].components[1].filament == 2);
        CHECK(rows[0].components[1].percent == 30);
        CHECK(rows[1].virtual_id == v_b);
        CHECK(rows[1].components[0].percent + rows[1].components[1].percent == 100);
    }
    SECTION("three equal components round to 100 too")
    {
        MixedFilament &three            = mgr.mixed_filaments()[0];
        three.gradient_component_ids    = MixedFilamentManager::encode_gradient_component_ids({ 1, 2, 3 });
        three.gradient_component_weights = "1/1/1";
        const std::vector<LegendRow> rows = legend_rows(mgr, { v_a }, n, off);
        REQUIRE(rows.size() == 1);
        REQUIRE(rows[0].components.size() == 3);
        int sum = 0;
        for (const LegendComponent &c : rows[0].components) {
            CHECK((c.percent == 33 || c.percent == 34));
            sum += c.percent;
        }
        CHECK(sum == 100);
    }
}

TEST_CASE("Mixed sublayer UI: rows in a mixed_filament_definitions value", "[MixedSublayerUi]")
{
    CHECK(count_mixed_definition_rows("") == 0);
    CHECK(count_mixed_definition_rows("  ") == 0);
    CHECK(count_mixed_definition_rows("1,2,1,1,30") == 1);
    CHECK(count_mixed_definition_rows("1,2,1,1,30;2,3,1,1,50") == 2);
    CHECK(count_mixed_definition_rows("1,2,1,1,30;;2,3,1,1,50;") == 2);

    const std::vector<std::string> colours = { "#FF0000", "#0000FF", "#00FF00" };
    MixedFilamentManager           mgr;
    mgr.add_custom_filament(1, 2, 30, colours);
    mgr.add_custom_filament(1, 3, 40, colours);
    CHECK(count_mixed_definition_rows(mgr.serialize_custom_entries()) == mgr.mixed_filaments().size());
}

TEST_CASE("Mixed sublayer UI: rows the planner never splits, and the layer count", "[MixedSublayerUi]")
{
    const ProcessDefaults ratio_on { true, false };

    MixedFilament pattern = make_row(1, 2, 50, int(M::Ratio));
    pattern.manual_pattern = "112";
    CHECK_FALSE(row_can_sublayer(pattern));
    CHECK(effective_method(pattern, ratio_on) == M::Cadence);

    MixedFilament dots = make_row(1, 2, 50);
    dots.distribution_mode = int(MixedFilament::SameLayerPointillisme);
    CHECK(effective_method(dots, ratio_on) == M::Cadence);

    // 0 % of B leaves one filament: whole layers, as the planner does.
    CHECK(effective_method(make_row(1, 2, 0), ratio_on) == M::Cadence);
    CHECK(effective_method(make_row(1, 2, 1), ratio_on) == M::Ratio);

    CHECK(estimate_layers(20., 0.2) == 100);
    CHECK(estimate_layers(20.1, 0.2) == 101);
    CHECK(estimate_layers(0., 0.2) == 0);
    CHECK(estimate_layers(20., 0.) == 0);
}
