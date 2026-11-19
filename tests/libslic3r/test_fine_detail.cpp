// EdgeSlicer, Fine detail (tests/design_arachne_thin_features.md, P2).
//
// What a thin fin becomes through WallToolPaths, the way PerimeterGenerator calls it for the "0.20mm Standard @BBL X1C"
// numbers (0.4 nozzle, 0.2 mm layers, outer 0.42 / inner 0.45 lines, 2 wall loops, precise outer wall on): a rectangle
// fin of width w, read at the middle of its length, for Fine detail Off / Balanced / Fine text.
//
// Today's numbers (Off) are pinned from the closed form of the beading strategies, which the design's G-code
// measurements of main 63bd59427b match (measurements.txt, fins 0.15-0.60):
//   - precise outer wall takes ext_width - ext_spacing = 0.0429 mm off each side, so the fin is t = w - 0.0858 thick;
//   - t < 0.10 (min_feature_size 25 %): no line;
//   - 0.10 <= t < 0.34 (min_bead_width 85 %): one line widened to 0.34 (a 0.383 mm line in the G-code);
//   - one line of width t up to the split, then two lines of t / 2 (well past the split, w 0.85-1.00).
// Close to the 1 -> 2 split (w 0.65-0.80) the bead count also depends on Arachne's transition filtering, so there the
// test checks what must hold (the lines fill the fin; every level splits exactly as Off does) instead of a number.

#include <catch2/catch.hpp>

#include "libslic3r/Arachne/WallToolPaths.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/FineDetail.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kNozzle   = 0.4;
constexpr double kLayer    = 0.2;
constexpr double kOuter    = 0.42;
constexpr double kInner    = 0.45;
constexpr double kFinLen   = 20.;
constexpr int    kLayerId  = 5; // not the first layer: Fine detail does not act there

double spacing(double width, double h) { return width - h * (1. - 0.25 * PI); }

struct Configs
{
    PrintObjectConfig object;
    PrintRegionConfig region;
    PrintConfig       print;
};

Configs make_configs(FineDetailLevel level)
{
    Configs c; // static configs start at the code defaults
    c.print.nozzle_diameter.values = { kNozzle };
    c.region.fine_detail.value     = level;
    return c;
}

// The widths (mm, Arachne spacing) of the lines crossing x = kFinLen / 2, sorted.
std::vector<double> widths_at_middle(const std::vector<Arachne::VariableWidthLines> &toolpaths)
{
    const coord_t       xm = scaled<coord_t>(kFinLen / 2.);
    std::vector<double> out;
    auto cross = [&out, xm](const Arachne::ExtrusionJunction &a, const Arachne::ExtrusionJunction &b) {
        const coord_t lo = std::min(a.p.x(), b.p.x()), hi = std::max(a.p.x(), b.p.x());
        if (lo > xm || hi <= xm || hi == lo)
            return;
        const double f = double(xm - a.p.x()) / double(b.p.x() - a.p.x());
        out.push_back(unscaled<double>(double(a.w) + f * double(b.w - a.w)));
    };
    for (const Arachne::VariableWidthLines &inset : toolpaths)
        for (const Arachne::ExtrusionLine &line : inset) {
            for (size_t i = 0; i + 1 < line.junctions.size(); ++i)
                cross(line.junctions[i], line.junctions[i + 1]);
            if (line.is_closed && line.junctions.size() > 2 && line.junctions.front().p != line.junctions.back().p)
                cross(line.junctions.back(), line.junctions.front());
        }
    std::sort(out.begin(), out.end());
    return out;
}

// One fin of width w through WallToolPaths, as PerimeterGenerator::process_arachne() calls it with precise outer wall.
std::vector<double> fin_beads(double w, const Configs &c, double layer_height = kLayer)
{
    const coord_t ext_width   = scaled<coord_t>(kOuter);
    const coord_t ext_spacing = scaled<coord_t>(spacing(kOuter, layer_height));
    const coord_t spacing_x   = scaled<coord_t>(spacing(kInner, layer_height));

    Polygon fin;
    fin.points = { Point::new_scale(0., -w / 2.), Point::new_scale(kFinLen, -w / 2.), Point::new_scale(kFinLen, w / 2.),
                   Point::new_scale(0., w / 2.) };
    const ExPolygons last = offset_ex(ExPolygon(fin), -float(ext_width - ext_spacing));
    const coord_t wall_0_inset = -coord_t(ext_width / 2 - ext_spacing / 2);

    Arachne::WallToolPathsParams params = Arachne::make_paths_params(kLayerId, c.object, c.print, &c.region);
    // WallToolPaths keeps a reference to the outline: it has to outlive the generator.
    const Polygons               outline = to_polygons(last);
    Arachne::WallToolPaths       wall_tool_paths(outline, ext_spacing, spacing_x, 2, wall_0_inset, layer_height, params);
    return widths_at_middle(wall_tool_paths.getToolPaths());
}

double sum(const std::vector<double> &v) { return std::accumulate(v.begin(), v.end(), 0.); }

// Fin widths 0.15 ... 1.00 in 0.05 steps, as integers in hundredths so the loop has no float drift.
std::vector<int> fin_widths_hundredths()
{
    std::vector<int> out;
    for (int w = 15; w <= 100; w += 5)
        out.push_back(w);
    return out;
}

// Precise outer wall takes ext_width - ext_spacing = h (1 - pi/4) off each side: 0.0858 mm in all.
constexpr double kShrink = 2. * kLayer * (1. - 0.25 * PI);

} // namespace

TEST_CASE("Fine detail: the level's floors and the raw keys", "[FineDetail]")
{
    CHECK(FineDetail::floors(FineDetailLevel::Off, 30., 5.).line_percent == 0.);
    CHECK(FineDetail::floors(FineDetailLevel::Off, 30., 5.).feature_percent == 0.);
    CHECK(FineDetail::floors(FineDetailLevel::Balanced, 30., 5.).line_percent == FineDetail::balanced_line_percent);
    CHECK(FineDetail::floors(FineDetailLevel::Balanced, 30., 5.).feature_percent == FineDetail::balanced_feature_percent);
    CHECK(FineDetail::floors(FineDetailLevel::FineText, 30., 5.).line_percent == FineDetail::fine_text_line_percent);
    CHECK(FineDetail::floors(FineDetailLevel::FineText, 30., 5.).feature_percent == FineDetail::fine_text_feature_percent);
    // Custom is the only level that reads the raw keys.
    CHECK(FineDetail::floors(FineDetailLevel::Custom, 30., 5.).line_percent == 30.);
    CHECK(FineDetail::floors(FineDetailLevel::Custom, 30., 5.).feature_percent == 5.);
    // D2: 60 % / 45 % of the nozzle, a 0.28 / 0.22 mm line on a 0.4 nozzle at 0.2 mm layers.
    CHECK_THAT(FineDetail::line_width(0.6 * kNozzle, kLayer), WithinAbs(0.283, 0.001));
    CHECK_THAT(FineDetail::line_width(0.45 * kNozzle, kLayer), WithinAbs(0.223, 0.001));
}

TEST_CASE("Fine detail: the floor is never above the Thinnest line nor narrower than the layer is tall", "[FineDetail]")
{
    // Off (0) is min_bead_width exactly.
    CHECK(FineDetail::effective_thin_bead_width(0., 0.34, 0.2) == 0.34);
    CHECK(FineDetail::effective_thin_feature_size(0., 0.10) == 0.10);
    // A floor above the Off floor acts as the Off floor.
    CHECK(FineDetail::effective_thin_bead_width(0.5, 0.34, 0.2) == 0.34);
    CHECK(FineDetail::effective_thin_feature_size(0.2, 0.10) == 0.10);
    for (const double h : { 0.08, 0.12, 0.2, 0.28, 0.32 }) {
        DYNAMIC_SECTION("layer " << h) {
            const double s = FineDetail::effective_thin_bead_width(0.18, 0.34, h);
            // The line it prints as is at least as wide as the layer is tall...
            CHECK(FineDetail::line_width(s, h) >= h - 1e-9);
            // ...and the floor is the level's own value whenever the layer allows it.
            if (FineDetail::min_spacing_for_layer(h) <= 0.18)
                CHECK(s == 0.18);
            else
                CHECK_THAT(FineDetail::line_width(s, h), WithinAbs(h, 1e-9));
        }
    }
}

TEST_CASE("Fine detail: choosing a level writes the raw keys, editing one makes it Custom", "[FineDetail]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.opt_enum<FineDetailLevel>("fine_detail") == FineDetailLevel::Off);

    SECTION("Off writes nothing") {
        CHECK(FineDetail::changes_for_edit(config, "fine_detail").empty());
        config.set_key_value("thin_feature_line_width", new ConfigOptionPercent(30));
        CHECK(FineDetail::changes_for_edit(config, "thin_feature_line_width").empty());
    }
    SECTION("Fine text writes its values") {
        config.set_key_value("fine_detail", new ConfigOptionEnum<FineDetailLevel>(FineDetailLevel::FineText));
        const DynamicPrintConfig changes = FineDetail::changes_for_edit(config, "fine_detail");
        REQUIRE(changes.has("thin_feature_line_width"));
        REQUIRE(changes.has("thin_feature_min_size"));
        CHECK(changes.option<ConfigOptionPercent>("thin_feature_line_width")->value == FineDetail::fine_text_line_percent);
        CHECK(changes.option<ConfigOptionPercent>("thin_feature_min_size")->value == FineDetail::fine_text_feature_percent);
        CHECK_FALSE(changes.has("fine_detail"));
        CHECK(changes.keys().size() == 2);

        // Applied, the level and the raw keys agree, and a second pass has nothing left to do.
        config.apply(changes);
        CHECK(FineDetail::changes_for_edit(config, "fine_detail").empty());
        CHECK(FineDetail::changes_for_edit(config, "thin_feature_line_width").empty());

        // Editing a raw key to another value makes the level Custom and leaves the edit alone.
        config.set_key_value("thin_feature_line_width", new ConfigOptionPercent(50));
        const DynamicPrintConfig custom = FineDetail::changes_for_edit(config, "thin_feature_line_width");
        REQUIRE(custom.has("fine_detail"));
        CHECK(custom.opt_enum<FineDetailLevel>("fine_detail") == FineDetailLevel::Custom);
        CHECK(custom.keys().size() == 1);
        config.apply(custom);
        // Custom then means exactly the raw values.
        PrintRegionConfig region;
        region.apply(config, true);
        CHECK(FineDetail::floors(region).line_percent == 50.);
        CHECK(FineDetail::floors(region).feature_percent == FineDetail::fine_text_feature_percent);
        // Custom never rewrites the raw keys.
        CHECK(FineDetail::changes_for_edit(config, "fine_detail").empty());
    }
    SECTION("Balanced writes its values, a key edited to the same value keeps it Balanced") {
        config.set_key_value("fine_detail", new ConfigOptionEnum<FineDetailLevel>(FineDetailLevel::Balanced));
        config.apply(FineDetail::changes_for_edit(config, "fine_detail"));
        CHECK(config.option<ConfigOptionPercent>("thin_feature_line_width")->value == FineDetail::balanced_line_percent);
        CHECK(config.option<ConfigOptionPercent>("thin_feature_min_size")->value == FineDetail::balanced_feature_percent);
        CHECK(FineDetail::changes_for_edit(config, "thin_feature_min_size").empty());
    }
    SECTION("an unrelated key changes nothing") {
        config.set_key_value("fine_detail", new ConfigOptionEnum<FineDetailLevel>(FineDetailLevel::FineText));
        CHECK(FineDetail::changes_for_edit(config, "wall_loops").empty());
    }
}

TEST_CASE("Fine detail: keys, defaults and an old project", "[FineDetail][Config]")
{
    const DynamicPrintConfig defaults = DynamicPrintConfig::full_print_config();
    CHECK(defaults.opt_enum<FineDetailLevel>("fine_detail") == FineDetailLevel::Off);
    CHECK(defaults.option<ConfigOptionPercent>("min_odd_wall_line_width")->value == 0.);
    CHECK(defaults.option<ConfigOptionPercent>("min_even_wall_line_width")->value == 0.);
    CHECK(defaults.option<ConfigOptionPercent>("thin_feature_min_size")->value == 0.);
    // Region keys (a modifier can carry them); the odd / even minimums are object keys like min_bead_width.
    CHECK(PrintRegionConfig().has("fine_detail"));
    CHECK(PrintRegionConfig().has("thin_feature_line_width"));
    CHECK(PrintRegionConfig().has("thin_feature_min_size"));
    CHECK(PrintRegionConfig().has("fine_detail_speed"));
    CHECK(PrintObjectConfig().has("min_odd_wall_line_width"));
    CHECK(PrintObjectConfig().has("min_even_wall_line_width"));
    // In the process preset's keys, so presets and projects save them.
    const std::vector<std::string> &opts = Preset::print_options();
    for (const char *key : { "fine_detail", "thin_feature_line_width", "thin_feature_min_size", "fine_detail_speed",
                             "min_odd_wall_line_width", "min_even_wall_line_width" }) {
        INFO(key);
        CHECK(std::find(opts.begin(), opts.end(), key) != opts.end());
    }
    // The enum round-trips by name.
    DynamicPrintConfig config;
    config.set_deserialize_strict({ { "fine_detail", "fine_text" } });
    CHECK(config.opt_enum<FineDetailLevel>("fine_detail") == FineDetailLevel::FineText);
    CHECK(config.opt_serialize("fine_detail") == "fine_text");
    // An old project has none of the keys: what it loads into is Off, which is the walls as before.
    DynamicPrintConfig old_project = DynamicPrintConfig::full_print_config();
    old_project.erase("fine_detail");
    PrintRegionConfig region;
    region.apply(old_project, true);
    CHECK(region.fine_detail.value == FineDetailLevel::Off);
    CHECK_FALSE(FineDetail::enabled(region));
}

TEST_CASE("Fine detail: Off reaches WallToolPaths as today's parameters", "[FineDetail][Arachne]")
{
    const Configs off = make_configs(FineDetailLevel::Off);
    const Arachne::WallToolPathsParams with_region    = Arachne::make_paths_params(kLayerId, off.object, off.print, &off.region);
    const Arachne::WallToolPathsParams without_region = Arachne::make_paths_params(kLayerId, off.object, off.print);
    // The three-argument call (concentric fills, curved tops) and the region call agree, and the new fields are 0.
    CHECK(with_region.thin_bead_width == 0.f);
    CHECK(with_region.thin_feature_size == 0.f);
    CHECK(with_region.min_odd_wall_width == 0.f);
    CHECK(with_region.min_even_wall_width == 0.f);
    CHECK(without_region.thin_bead_width == 0.f);
    CHECK(with_region.min_bead_width == without_region.min_bead_width);
    CHECK(with_region.min_feature_size == without_region.min_feature_size);
    CHECK_THAT(with_region.min_bead_width, WithinAbs(0.34, 1e-6));
    CHECK_THAT(with_region.min_feature_size, WithinAbs(0.10, 1e-6));

    // First layer: no Fine detail, whatever the level.
    const Configs fine = make_configs(FineDetailLevel::FineText);
    CHECK(Arachne::make_paths_params(0, fine.object, fine.print, &fine.region).thin_bead_width == 0.f);
    CHECK_THAT(Arachne::make_paths_params(kLayerId, fine.object, fine.print, &fine.region).thin_bead_width, WithinAbs(0.18, 1e-6));
    CHECK_THAT(Arachne::make_paths_params(kLayerId, fine.object, fine.print, &fine.region).thin_feature_size, WithinAbs(0.06, 1e-6));
}

TEST_CASE("Fine detail: what a fin of 0.15 - 1.00 mm becomes on a 0.4 nozzle", "[FineDetail][Arachne]")
{
    const Configs off      = make_configs(FineDetailLevel::Off);
    const Configs balanced = make_configs(FineDetailLevel::Balanced);
    const Configs fine     = make_configs(FineDetailLevel::FineText);

    for (const int hundredths : fin_widths_hundredths()) {
        const double w = hundredths / 100.;
        const double t = w - kShrink;
        DYNAMIC_SECTION("fin " << w) {
            const std::vector<double> b_off  = fin_beads(w, off);
            const std::vector<double> b_bal  = fin_beads(w, balanced);
            const std::vector<double> b_fine = fin_beads(w, fine);
            CAPTURE(t, b_off, b_bal, b_fine);

            // ---- Off: today's numbers.
            if (hundredths == 15) {
                CHECK(b_off.empty()); // t = 0.064 < 0.10
                CHECK(b_bal.empty()); // Balanced keeps "skip thinner than" as set
            } else if (hundredths <= 40) {
                // Widened to min_bead_width.
                REQUIRE(b_off.size() == 1);
                CHECK_THAT(b_off.front(), WithinAbs(0.34, 0.002));
            } else if (hundredths <= 60) {
                // One line as wide as the fin.
                REQUIRE(b_off.size() == 1);
                CHECK_THAT(b_off.front(), WithinAbs(t, 0.003));
            } else if (hundredths >= 85) {
                // Two lines sharing the fin.
                REQUIRE(b_off.size() == 2);
                CHECK_THAT(b_off.front(), WithinAbs(t / 2., 0.003));
                CHECK_THAT(b_off.back(), WithinAbs(t / 2., 0.003));
            } else {
                // Around the split (0.65 - 0.80): one or two lines that fill the fin.
                CHECK(b_off.size() >= 1);
                CHECK(b_off.size() <= 2);
                CHECK_THAT(sum(b_off), WithinAbs(t, 0.005));
            }

            // ---- Balanced (60 %) and Fine text (45 %): only features thinner than the Thinnest line change.
            if (hundredths >= 20 && hundredths <= 40) {
                REQUIRE(b_bal.size() == 1);
                REQUIRE(b_fine.size() == 1);
                CHECK_THAT(b_bal.front(), WithinAbs(std::max(t, 0.24), 0.003));
                CHECK_THAT(b_fine.front(), WithinAbs(std::max(t, 0.18), 0.003));
                // Never wider than Off, and the line is not narrower than the layer is tall.
                CHECK(b_fine.front() <= b_off.front() + 1e-6);
                CHECK(FineDetail::line_width(b_fine.front(), kLayer) >= kLayer);
            }
            // The whole point of the split: from 0.45 mm up every level prints exactly the lines Off prints.
            if (hundredths >= 45) {
                REQUIRE(b_bal.size() == b_off.size());
                REQUIRE(b_fine.size() == b_off.size());
                for (size_t i = 0; i < b_off.size(); ++i) {
                    CHECK_THAT(b_bal[i], WithinAbs(b_off[i], 0.001));
                    CHECK_THAT(b_fine[i], WithinAbs(b_off[i], 0.001));
                }
            }
        }
    }
}

TEST_CASE("Fine detail: at thick layers the floor follows the layer height", "[FineDetail][Arachne]")
{
    // 0.28 mm layers: Fine text's 0.18 mm would print a 0.24 mm line, narrower than the layer; it becomes a 0.28 line.
    const Configs fine = make_configs(FineDetailLevel::FineText);
    const double  h    = 0.28;
    const double  w    = 0.25;
    const std::vector<double> beads = fin_beads(w, fine, h);
    REQUIRE(beads.size() == 1);
    CHECK_THAT(FineDetail::line_width(beads.front(), h), WithinAbs(h, 0.003));
}

TEST_CASE("Fine detail: the odd / even minimums move the split, and only they do", "[FineDetail][Arachne]")
{
    // Setting the odd minimum to 45 % alone is the old "min_bead_width 45 %" split: a 0.60 mm fin becomes two lines
    // (measured on main with min_bead_width 45 %: 2 x 0.30 mm lines).
    Configs odd = make_configs(FineDetailLevel::Off);
    odd.object.min_odd_wall_line_width.value = 45.;
    const std::vector<double> split = fin_beads(0.60, odd);
    CHECK(split.size() == 2);
    // ...whereas Fine text, whose floor is the same 45 %, keeps it one line (the walls do not change).
    const std::vector<double> kept = fin_beads(0.60, make_configs(FineDetailLevel::FineText));
    CHECK(kept.size() == 1);
}
