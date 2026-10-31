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
    in.repeat   = 1;   // the whole 3-filament sequence once: 3 bands
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
    CHECK(p.repeat == 1);

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
    CHECK(back.repeat == 1);
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
    CHECK(pv.summary.find("1 repeat of 3 filaments") != std::string::npos);
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
    in.repeat   = 1;
    in.sequence = { 1, 2, 3, 4 };
    HeightColourPattern p = height_pattern_from_input(in, 0.2, tops);
    REQUIRE(apply_height_colour_pattern(*obj, p, tops, 0.2, false, 120., 4).ok);
    CHECK(height_pattern_row_label(obj->height_colour_patterns.front()) == "Color Pattern (4 filaments x 4 bands)");

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
        CHECK(height_pattern_row_label(o2->height_colour_patterns.front()) == "Color Pattern (1 filament x 1 band)");
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
        CHECK(height_pattern_short_summary(pv.expansion) == "3 bands (1 repeat of 3 filaments)");
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

TEST_CASE("Color pattern axis is reserved and does not change the Phase 1 sidecar", "[HeightPattern]")
{
    Model model;
    ModelObject *obj = hp_cube(model, 120.);
    const std::vector<double> tops = hp_grid(120., 0.2, 0.2);
    HeightColourPattern p = height_pattern_from_input(hp_example(), 0.2, tops);
    REQUIRE(apply_height_colour_pattern(*obj, p, tops, 0.2, false, 120., 4).ok);
    CHECK(obj->height_colour_patterns.front().axis == 0);

    const std::vector<const std::vector<HeightColourPattern> *> objs = { &obj->height_colour_patterns };
    const std::string json = height_patterns_to_json(objs);
    CHECK(json.find("axis") == std::string::npos);   // nothing new is written for a height pattern
    REQUIRE(height_patterns_from_json(json).size() == 1);

    SECTION("a pattern for another direction is written, and dropped by a build that cannot show it")
    {
        obj->height_colour_patterns.front().axis = 1;
        const std::string with_axis = height_patterns_to_json(objs);
        CHECK(with_axis.find("axis") != std::string::npos);
        CHECK(height_patterns_from_json(with_axis).empty());
    }
}

namespace {

HeightPatternChangeInput hp_change_input(int base = 1)
{
    HeightPatternChangeInput in;
    in.base_filament   = base;
    in.filament_count  = 3;
    // from -> to, mm^3: 1->2 100, 1->3 200, 2->1 110, 2->3 300, 3->1 210, 3->2 310 (diagonal unused)
    in.flush_matrix    = { 0, 100, 200,  110, 0, 300,  210, 310, 0 };
    in.flush_multiplier = 1.;
    in.density         = { 1.0, 2.0, 4.0 };
    return in;
}

HeightPatternRange hp_range(double lo, double hi, int extruder)
{
    HeightPatternRange r;
    r.lo = lo; r.hi = hi; r.extruder = extruder; r.layer_height = 0.2;
    return r;
}

} // namespace

TEST_CASE("Color pattern change count", "[HeightPattern]")
{
    SECTION("bands with gaps on the object's own filament: two changes per band")
    {
        // object on 1; bands 2-12 on 2, 22-32 on 3, 42-52 on 2, object top 60
        const std::vector<HeightPatternRange> r = { hp_range(2, 12, 2), hp_range(22, 32, 3), hp_range(42, 52, 2) };
        const HeightPatternChangeEstimate e = estimate_height_pattern_changes(r, 60., hp_change_input());
        // 1->2, 2->1, 1->3, 3->1, 1->2, 2->1
        CHECK(e.changes == 6);
        CHECK(e.purge_known);
        CHECK(e.purge_mm3 == Approx(100. + 110. + 200. + 210. + 100. + 110.).margin(1e-6));
        // grams use the density of the filament changed TO: 2.0, 1.0, 4.0, 1.0, 2.0, 1.0 g/cm3
        CHECK(e.purge_g == Approx((100 * 2. + 110 * 1. + 200 * 4. + 210 * 1. + 100 * 2. + 110 * 1.) / 1000.).margin(1e-9));
    }
    SECTION("a band at the bottom is the starting filament, a band at the top needs no change back")
    {
        const std::vector<HeightPatternRange> r = { hp_range(0, 10, 2), hp_range(50, 60, 3) };
        const HeightPatternChangeEstimate e = estimate_height_pattern_changes(r, 60., hp_change_input());
        // starts on 2; 2->1 (gap), 1->3 (band); ends at the top on 3
        CHECK(e.changes == 2);
        CHECK(e.purge_mm3 == Approx(110. + 200.).margin(1e-6));
    }
    SECTION("touching bands on different filaments change once, on the same filament not at all")
    {
        const std::vector<HeightPatternRange> r = { hp_range(0, 10, 2), hp_range(10, 20, 3), hp_range(20, 30, 3) };
        const HeightPatternChangeEstimate e = estimate_height_pattern_changes(r, 30., hp_change_input());
        CHECK(e.changes == 1);
    }
    SECTION("a gap filament counts as a band")
    {
        // object on 1; 2-12 on 2, gap 12-22 on 3, 22-32 on 2, top 32: 1->2, 2->3, 3->2
        const std::vector<HeightPatternRange> r = { hp_range(2, 12, 2), hp_range(12, 22, 3), hp_range(22, 32, 2) };
        const HeightPatternChangeEstimate e = estimate_height_pattern_changes(r, 32., hp_change_input());
        CHECK(e.changes == 3);
    }
    SECTION("a flush matrix that does not cover the filaments leaves the purge unknown, not zero")
    {
        HeightPatternChangeInput in = hp_change_input();
        in.flush_matrix.clear();
        const HeightPatternChangeEstimate e = estimate_height_pattern_changes({ hp_range(2, 12, 2) }, 60., in);
        CHECK(e.changes == 2);
        CHECK_FALSE(e.purge_known);
    }
    SECTION("no ranges, no changes")
    {
        const HeightPatternChangeEstimate e = estimate_height_pattern_changes({}, 60., hp_change_input());
        CHECK(e.changes == 0);
        CHECK(e.purge_known);
    }
}

TEST_CASE("Color pattern hand-edited bands are found and reset one by one", "[HeightPattern]")
{
    Model model;
    ModelObject *obj = hp_cube(model, 120.);
    const std::vector<double> tops = hp_grid(120., 0.2, 0.2);
    HeightPatternInput in = hp_example();
    in.gap_mode     = int(HeightPatternGapMode::Fixed);
    in.gap_filament = 4;
    HeightColourPattern p = height_pattern_from_input(in, 0.2, tops);
    const HeightPatternApplyResult applied = apply_height_colour_pattern(*obj, p, tops, 0.2, false, 120., 4);
    REQUIRE(applied.ok);
    const int id = applied.pattern_id;
    const HeightColourPattern &rec = *find_height_colour_pattern(*obj, id);
    REQUIRE(rec.generated.size() == 5);   // band, gap, band, gap, band

    CHECK(height_pattern_modified_ranges(*obj, rec).empty());
    CHECK(reset_height_pattern_modified_ranges(*obj, id) == 0);

    // Change band 2's filament (range 3 of 5) and delete band 3 (range 5), and make an unrelated edit to a gap.
    obj->layer_config_ranges[{ rec.generated[2].lo, rec.generated[2].hi }].set_key_value("extruder", new ConfigOptionInt(4));
    obj->layer_config_ranges.erase(t_layer_height_range(rec.generated[4].lo, rec.generated[4].hi));
    obj->layer_config_ranges[{ rec.generated[1].lo, rec.generated[1].hi }].set_key_value("layer_height", new ConfigOptionFloat(0.1));

    const std::vector<HeightPatternBandDiff> diff = height_pattern_modified_ranges(*obj, rec);
    REQUIRE(diff.size() == 3);
    CHECK(diff[0].band_number == 0);   // the gap
    CHECK(diff[0].status == HeightPatternBandStatus::LayerHeightChanged);
    CHECK(diff[1].band_number == 2);
    CHECK(diff[1].status == HeightPatternBandStatus::FilamentChanged);
    CHECK(diff[1].expected_extruder == 2);
    CHECK(diff[1].actual_extruder == 4);
    CHECK(diff[2].band_number == 3);
    CHECK(diff[2].status == HeightPatternBandStatus::Removed);

    // A hand-made range elsewhere and a hand-edit of an untouched band's neighbour must survive a reset.
    hp_add_hand_range(*obj, 100., 110., 3);
    // A hand range that replaced band 3 with another extent: it overlaps the removed band and goes.
    hp_add_hand_range(*obj, 43., 50., 1);

    CHECK(reset_height_pattern_modified_ranges(*obj, id) == 3);
    CHECK(height_pattern_modified_ranges(*obj, rec).empty());
    CHECK(obj->layer_config_ranges.count(t_layer_height_range(100., 110.)) == 1);          // untouched
    CHECK(obj->layer_config_ranges.count(t_layer_height_range(43., 50.)) == 0);            // replaced by the restored band
    CHECK(reset_height_pattern_modified_ranges(*obj, 99) == -1);
}
