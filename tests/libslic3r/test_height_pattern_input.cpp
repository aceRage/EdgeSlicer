// Height colour pattern, phase 2 (tests/design_height_colour_pattern.md): the dialog's model, the
// overlap check, the object-list row text and the "what is wrong with this pattern" health check.

#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "libslic3r/HeightColourPattern.hpp"
#include "libslic3r/HeightPatternInput.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

std::vector<double> hp_grid(double height, double first, double step)
{
    std::vector<double> tops;
    tops.push_back(first);
    for (int k = 1; tops.back() < height - 1e-9; ++k)
        tops.push_back(first + k * step);
    tops.back() = height;
    return tops;
}

ModelObject *hp_cube(Model &model, double height)
{
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(20., 20., height));
    object->add_instance();
    object->ensure_on_bed();
    return object;
}

HeightPatternInput hp_example()
{
    HeightPatternInput in;
    in.start    = 2.;
    in.band     = 10.;
    in.gap      = 10.;
    in.repeat   = 3;
    in.sequence = { 1, 2, 3 };
    return in;
}

void hp_add_hand_range(ModelObject &o, double lo, double hi, int extruder, double layer_height = 0.2)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    cfg.set_key_value("extruder", new ConfigOptionInt(extruder));
    o.layer_config_ranges[{ lo, hi }].assign_config(cfg);
}

} // namespace

TEST_CASE("Height pattern input validation", "[HeightPattern]")
{
    HeightPatternInput in = hp_example();
    CHECK(validate_height_pattern_input(in, 4).empty());

    SECTION("empty sequence") { in.sequence.clear(); CHECK_FALSE(validate_height_pattern_input(in, 4).empty()); }
    SECTION("filament above the project") { in.sequence = { 1, 5 }; CHECK_FALSE(validate_height_pattern_input(in, 4).empty()); }
    SECTION("filament above the project is not checked with max 0") { in.sequence = { 1, 5 }; CHECK(validate_height_pattern_input(in, 0).empty()); }
    SECTION("zero filament") { in.sequence = { 0 }; CHECK_FALSE(validate_height_pattern_input(in, 4).empty()); }
    SECTION("band must be positive") { in.band = 0.; CHECK_FALSE(validate_height_pattern_input(in, 4).empty()); }
    SECTION("gap cannot be negative") { in.gap = -1.; CHECK_FALSE(validate_height_pattern_input(in, 4).empty()); }
    SECTION("start cannot be negative") { in.start = -1.; CHECK_FALSE(validate_height_pattern_input(in, 4).empty()); }
    SECTION("repeat needs a count unless to top")
    {
        in.repeat = 0;
        CHECK_FALSE(validate_height_pattern_input(in, 4).empty());
        in.to_top = true;
        CHECK(validate_height_pattern_input(in, 4).empty());
    }
    SECTION("fixed gap filament")
    {
        in.gap_mode = int(HeightPatternGapMode::Fixed);
        CHECK_FALSE(validate_height_pattern_input(in, 4).empty());
        in.gap_filament = 9;
        CHECK_FALSE(validate_height_pattern_input(in, 4).empty());
        in.gap_filament = 4;
        CHECK(validate_height_pattern_input(in, 4).empty());
    }
}

TEST_CASE("Height pattern input converts layers to millimetres", "[HeightPattern]")
{
    const std::vector<double> tops = hp_grid(20., 0.2, 0.2); // 0.2 mm layers all the way
    HeightPatternInput in = hp_example();
    in.unit  = HeightPatternUnit::Layers;
    in.start = 10;   // ten layers below the first band
    in.band  = 25;
    in.gap   = 50;

    const HeightColourPattern p = height_pattern_from_input(in, 0.2, tops);
    CHECK(p.start_z == Approx(2.0).margin(1e-6));
    CHECK(p.band_h == Approx(5.0).margin(1e-6));
    CHECK(p.gap_h == Approx(10.0).margin(1e-6));
    CHECK(p.repeat == 3);

    SECTION("start of zero layers is the bottom")
    {
        in.start = 0;
        CHECK(height_pattern_from_input(in, 0.2, tops).start_z == Approx(0.).margin(1e-9));
    }
    SECTION("a start beyond the object clamps to the top")
    {
        in.start = 100000;
        CHECK(height_pattern_from_input(in, 0.2, tops).start_z == Approx(20.).margin(1e-6));
    }
    SECTION("no layer grid falls back to layers x height")
    {
        CHECK(height_pattern_from_input(in, 0.2, {}).start_z == Approx(2.0).margin(1e-6));
    }
    SECTION("to top gives repeat 0")
    {
        in.to_top = true;
        CHECK(height_pattern_from_input(in, 0.2, tops).repeat == 0);
    }
}

TEST_CASE("Height pattern input round-trips through a record", "[HeightPattern]")
{
    HeightPatternInput in = hp_example();
    in.gap_mode = int(HeightPatternGapMode::Fixed);
    in.gap_filament = 4;
    in.snap_to_layers = false;
    const HeightColourPattern p = height_pattern_from_input(in, 0.2, {});
    const HeightPatternInput back = height_pattern_input_from_pattern(p);
    CHECK(back.unit == HeightPatternUnit::Mm);
    CHECK(back.start == Approx(2.).margin(1e-9));
    CHECK(back.band == Approx(10.).margin(1e-9));
    CHECK(back.gap == Approx(10.).margin(1e-9));
    CHECK(back.repeat == 3);
    CHECK_FALSE(back.to_top);
    CHECK(back.sequence == in.sequence);
    CHECK(back.gap_mode == in.gap_mode);
    CHECK(back.gap_filament == 4);
    CHECK_FALSE(back.snap_to_layers);

    HeightPatternInput to_top = in;
    to_top.to_top = true;
    CHECK(height_pattern_input_from_pattern(height_pattern_from_input(to_top, 0.2, {})).to_top);
}

TEST_CASE("Height pattern preview shows the snapped result", "[HeightPattern]")
{
    const std::vector<double> tops = hp_grid(120., 0.25, 0.2);
    const HeightPatternPreview pv = preview_height_pattern(nullptr, hp_example(), 0, tops, 0.2, 120., 4);
    REQUIRE(pv.ok());
    CHECK(pv.expansion.bands == 3);
    CHECK(pv.summary.find("3 bands") == 0);
    CHECK(pv.summary.find("(last band ends at") != std::string::npos);
    CHECK(pv.clashes.empty());

    SECTION("an invalid input is reported, not expanded")
    {
        HeightPatternInput bad = hp_example();
        bad.sequence = { 7 };
        const HeightPatternPreview b = preview_height_pattern(nullptr, bad, 0, tops, 0.2, 120., 4);
        CHECK_FALSE(b.ok());
        CHECK(b.expansion.ranges.empty());
    }
    SECTION("a start above the object leaves no band")
    {
        HeightPatternInput high = hp_example();
        high.start = 500.;
        CHECK_FALSE(preview_height_pattern(nullptr, high, 0, tops, 0.2, 120., 4).ok());
    }
    SECTION("a clipped last band is said so")
    {
        HeightPatternInput to_top = hp_example();
        to_top.to_top = true;
        to_top.start  = 0.;
        to_top.band   = 55.;
        to_top.gap    = 20.;
        const HeightPatternPreview c = preview_height_pattern(nullptr, to_top, 0, tops, 0.2, 120., 4);
        REQUIRE(c.ok());
        CHECK(c.expansion.clipped_at_top);
        CHECK(c.summary.find("cut short") != std::string::npos);
    }
}

TEST_CASE("Height pattern clash detection", "[HeightPattern]")
{
    Model model;
    ModelObject *obj = hp_cube(model, 120.);
    const std::vector<double> tops = hp_grid(120., 0.2, 0.2);

    hp_add_hand_range(*obj, 5., 8., 2);      // inside the first band (2-12)
    hp_add_hand_range(*obj, 100., 110., 2);  // well clear of the pattern

    const HeightPatternPreview pv = preview_height_pattern(obj, hp_example(), 0, tops, 0.2, 120., 4);
    REQUIRE(pv.ok());
    REQUIRE(pv.clashes.size() == 1);
    CHECK(pv.clashes[0].first == Approx(5.).margin(1e-9));
    CHECK(height_pattern_clash_text(pv.clashes).find("Overlaps 1 existing height range") == 0);
    CHECK(height_pattern_clash_text({}).empty());

    SECTION("touching ranges do not clash")
    {
        hp_add_hand_range(*obj, 12., 22., 2);   // exactly the gap
        const HeightPatternPreview t = preview_height_pattern(obj, hp_example(), 0, tops, 0.2, 120., 4);
        CHECK(t.clashes.size() == 1);
    }

    SECTION("the pattern's own ranges are not clashes when it is edited")
    {
        Model model2;
        ModelObject *o2 = hp_cube(model2, 120.);
        HeightColourPattern p = height_pattern_from_input(hp_example(), 0.2, tops);
        const HeightPatternApplyResult r = apply_height_colour_pattern(*o2, p, tops, 0.2, false, 120., 4);
        REQUIRE(r.ok);
        const HeightPatternPreview edit = preview_height_pattern(o2, hp_example(), r.pattern_id, tops, 0.2, 120., 4);
        CHECK(edit.clashes.empty());
        // ... but a new pattern over the same span does clash with them.
        const HeightPatternPreview fresh = preview_height_pattern(o2, hp_example(), 0, tops, 0.2, 120., 4);
        CHECK(fresh.clashes.size() == 3);
    }
}

TEST_CASE("Height pattern ownership of a range hides only untouched output", "[HeightPattern]")
{
    Model model;
    ModelObject *obj = hp_cube(model, 120.);
    const std::vector<double> tops = hp_grid(120., 0.2, 0.2);
    const HeightColourPattern p = height_pattern_from_input(hp_example(), 0.2, tops);
    const HeightPatternApplyResult r = apply_height_colour_pattern(*obj, p, tops, 0.2, false, 120., 4);
    REQUIRE(r.ok);
    hp_add_hand_range(*obj, 100., 110., 2);

    const auto &g = obj->height_colour_patterns.front().generated;
    REQUIRE(g.size() == 3);
    for (const HeightPatternRange &range : g)
        CHECK(height_pattern_owns_range(*obj, { range.lo, range.hi }));
    CHECK_FALSE(height_pattern_owns_range(*obj, { 100., 110. }));

    // Edit one by hand: it is an ordinary range now and the pattern is modified.
    obj->layer_config_ranges[{ g[1].lo, g[1].hi }].set_key_value("extruder", new ConfigOptionInt(4));
    CHECK_FALSE(height_pattern_owns_range(*obj, { g[1].lo, g[1].hi }));
    CHECK(height_pattern_owns_range(*obj, { g[0].lo, g[0].hi }));
    CHECK(height_colour_pattern_state(*obj, obj->height_colour_patterns.front()).modified);

    // Without any pattern, nothing is owned.
    Model model2;
    ModelObject *plain = hp_cube(model2, 120.);
    hp_add_hand_range(*plain, 2., 12., 2);
    CHECK_FALSE(height_pattern_owns_range(*plain, { 2., 12. }));
}

TEST_CASE("Height pattern row label counts distinct filaments and bands", "[HeightPattern]")
{
    Model model;
    ModelObject *obj = hp_cube(model, 120.);
    const std::vector<double> tops = hp_grid(120., 0.2, 0.2);

    HeightPatternInput in = hp_example();
    in.repeat   = 4;
    in.sequence = { 1, 2, 3, 4 };
    HeightColourPattern p = height_pattern_from_input(in, 0.2, tops);
    REQUIRE(apply_height_colour_pattern(*obj, p, tops, 0.2, false, 120., 4).ok);
    CHECK(height_pattern_row_label(obj->height_colour_patterns.front()) == "Height colour pattern (4 filaments x 4 bands)");

    SECTION("gap ranges are not bands")
    {
        Model m2;
        ModelObject *o2 = hp_cube(m2, 120.);
        HeightPatternInput g = in;
        g.gap_mode = int(HeightPatternGapMode::Fixed);
        g.gap_filament = 1;
        HeightColourPattern pg = height_pattern_from_input(g, 0.2, tops);
        REQUIRE(apply_height_colour_pattern(*o2, pg, tops, 0.2, false, 120., 4).ok);
        CHECK(o2->height_colour_patterns.front().generated.size() == 7);
        CHECK(height_pattern_band_count(o2->height_colour_patterns.front()) == 4);
    }
    SECTION("singular")
    {
        Model m2;
        ModelObject *o2 = hp_cube(m2, 120.);
        HeightPatternInput one = hp_example();
        one.repeat   = 1;
        one.sequence = { 2 };
        HeightColourPattern p1 = height_pattern_from_input(one, 0.2, tops);
        REQUIRE(apply_height_colour_pattern(*o2, p1, tops, 0.2, false, 120., 4).ok);
        CHECK(height_pattern_row_label(o2->height_colour_patterns.front()) == "Height colour pattern (1 filament x 1 band)");
    }
}

TEST_CASE("Height pattern health reports modified, missing filaments and a stale layer height", "[HeightPattern]")
{
    Model model;
    ModelObject *obj = hp_cube(model, 120.);
    const std::vector<double> tops = hp_grid(120., 0.2, 0.2);
    HeightColourPattern p = height_pattern_from_input(hp_example(), 0.2, tops);
    REQUIRE(apply_height_colour_pattern(*obj, p, tops, 0.2, false, 120., 4).ok);
    const HeightColourPattern &rec = obj->height_colour_patterns.front();

    CHECK(height_pattern_health(*obj, rec, 4, 0.2).ok());

    SECTION("fewer filaments than the pattern uses")
    {
        const HeightPatternHealth h = height_pattern_health(*obj, rec, 2, 0.2);
        CHECK_FALSE(h.ok());
        REQUIRE(h.missing_filaments.size() == 1);
        CHECK(h.missing_filaments[0] == 3);
    }
    SECTION("a different layer height")
    {
        const HeightPatternHealth h = height_pattern_health(*obj, rec, 4, 0.16);
        CHECK(h.layer_height_stale);
        CHECK_FALSE(h.ok());
    }
    SECTION("a range edited by hand")
    {
        obj->layer_config_ranges.begin()->second.set_key_value("extruder", new ConfigOptionInt(4));
        const HeightPatternHealth h = height_pattern_health(*obj, rec, 4, 0.2);
        CHECK(h.modified);
        CHECK_FALSE(h.ok());
    }
    SECTION("a filament count of 0 is not checked")
    {
        CHECK(height_pattern_health(*obj, rec, 0, 0.2).ok());
    }
}

TEST_CASE("Height pattern band list and short summary", "[HeightPattern]")
{
    const std::vector<double> tops = hp_grid(120., 0.2, 0.2);

    SECTION("bands only, gap ranges left out")
    {
        HeightPatternInput in = hp_example();
        in.gap_mode     = int(HeightPatternGapMode::Fixed);
        in.gap_filament = 4;
        const HeightPatternPreview pv = preview_height_pattern(nullptr, in, 0, tops, 0.2, 120., 4);
        REQUIRE(pv.ok());
        CHECK(pv.expansion.ranges.size() == 5);
        const std::vector<HeightPatternRange> bands = height_pattern_band_ranges(pv.expansion);
        REQUIRE(bands.size() == 3);
        CHECK(bands[0].extruder == 1);
        CHECK(bands[2].lo == Approx(42.).margin(1e-6));
        CHECK(height_pattern_short_summary(pv.expansion) == "3 bands");
    }
    SECTION("a clipped last band is named, and 50 bands stay one short line")
    {
        HeightPatternInput in = hp_example();
        in.to_top = true;
        in.start  = 0.;
        in.band   = 2.;
        in.gap    = 0.;
        const HeightPatternPreview pv = preview_height_pattern(nullptr, in, 0, tops, 0.2, 120., 4);
        REQUIRE(pv.ok());
        CHECK(height_pattern_band_ranges(pv.expansion).size() == 60);
        CHECK(height_pattern_short_summary(pv.expansion) == "60 bands");

        in.band = 7.;
        const HeightPatternPreview c = preview_height_pattern(nullptr, in, 0, tops, 0.2, 120., 4);
        REQUIRE(c.ok());
        CHECK(c.expansion.clipped_at_top);
        CHECK(height_pattern_short_summary(c.expansion) == "18 bands, last band cut short by the top of the object");
    }
}
