// Showcase tab, phase S3: filament callouts (src/libslic3r/Showcase/ShowcaseCallouts.{hpp,cpp}) - which
// callouts a plate gets, where their anchors go, colour names (D8), the label text (D7), the column
// layout with leaders that never cross and the legend it falls back to, and the callouts' place in a
// scene's JSON and the project 3MF (S1/S2 scenes included).

#include <catch2/catch.hpp>

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Showcase/ShowcaseCallouts.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <random>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Showcase;
using Catch::Matchers::WithinAbs;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

ModelObject *add_cube(Model &model, double size, double x, int extruder = 0)
{
    ModelObject *o = model.add_object();
    o->name        = "cube";
    o->add_volume(make_cube(size, size, size));
    o->add_instance();
    o->instances.front()->set_offset(Vec3d(x, 0., 0.));
    o->ensure_on_bed();
    if (extruder > 0)
        o->config.set("extruder", extruder);
    return o;
}

const CalloutTarget *find_target(const std::vector<CalloutTarget> &targets, const std::string &key)
{
    for (const CalloutTarget &t : targets)
        if (t.key == key)
            return &t;
    return nullptr;
}

// Every label inside the frame, column labels not overlapping each other, and no two leaders meeting.
void check_layout(const std::vector<LayoutLabel> &in, const LayoutParams &p, const CalloutLayoutResult &r)
{
    REQUIRE(r.labels.size() == in.size());
    for (size_t i = 0; i < r.labels.size(); ++i) {
        const RectD &b = r.labels[i].box;
        INFO("label " << i);
        CHECK(b.x >= -1e-6);
        CHECK(b.y >= -1e-6);
        CHECK(b.right() <= p.frame_w + 1e-6);
        CHECK(b.bottom() <= p.frame_h + 1e-6);
    }
    for (size_t i = 0; i < r.labels.size(); ++i)
        for (size_t j = i + 1; j < r.labels.size(); ++j) {
            INFO("labels " << i << " and " << j);
            if (r.labels[i].side != 0 && r.labels[i].side == r.labels[j].side) {
                const RectD &a = r.labels[i].box, &b = r.labels[j].box;
                const bool   apart = a.bottom() <= b.y + 1e-6 || b.bottom() <= a.y + 1e-6;
                CHECK(apart);
            }
            if (!in[i].pinned && !in[j].pinned)
                CHECK_FALSE(polylines_cross(r.labels[i].leader, r.labels[j].leader));
        }
}

fs::path scratch_dir(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("snorca_callouts_" + name + "_%%%%-%%%%");
    fs::create_directories(dir);
    return dir;
}

std::string zip_entry(const std::string &zip_path, const std::string &entry_name)
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

DynamicPrintConfig project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(1);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values  = { 0.4 };
    cfg.option<ConfigOptionStrings>("filament_colour")->values = { "#FF0000" };
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    return cfg;
}

Callouts sample_callouts()
{
    Callouts c;
    c.enabled        = true;
    c.layout         = CalloutLayout::Columns;
    c.max_per_column = 5;
    c.fields.weight  = true;
    c.fields.hex     = false;
    apply_callout_style_preset(c.style, CalloutStylePreset::CardDark);
    c.style.font_face = "Arial";
    c.style.size_pct  = 3.5f;

    Callout a;
    a.key      = make_callout_key(2);
    a.filament = 2;
    a.source   = CalloutSource::Part;
    a.anchors  = { { 0, 0, { { 1.5, -2.25, 10. } } }, { 1, 0, { { 0., 0., 3. } } } };
    a.pinned   = true;
    a.label_pos = { { 0.25, 0.75 } };
    a.title    = "Hero part";
    a.note     = "Printed upside down";
    c.items.push_back(a);

    Callout b;
    b.filament   = 1;
    b.source     = CalloutSource::ColorChange;
    b.z_from     = 12.4;
    b.color      = "#00FF00";
    b.key        = make_callout_key(b.filament, b.z_from, b.color);
    b.anchors    = { { 0, 0, { { 0., 0., 15. } } } };
    b.user_anchor = true;
    b.color_name = "Lime";
    b.hidden     = true;
    c.items.push_back(b);
    return c;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Layout
// ------------------------------------------------------------------------------------------------

TEST_CASE("Callout layout: leaders never cross", "[Showcase][Callouts]")
{
    std::mt19937 rng(20261020);
    std::uniform_real_distribution<double> ux(300., 1300.), uy(80., 820.), uw(120., 260.);
    for (int round = 0; round < 300; ++round) {
        const int n = 1 + round % 12;
        std::vector<LayoutLabel> labels(static_cast<size_t>(n));
        for (LayoutLabel &l : labels) {
            l.anchor = { ux(rng), uy(rng) };
            l.w      = uw(rng);
            l.h      = (round % 3 == 0) ? 40. : 30. + 10. * double(rng() % 3);   // equal and mixed heights
        }
        LayoutParams p;
        p.frame_w        = 1600.;
        p.frame_h        = 900.;
        p.margin         = 24.;
        p.gap            = 10.;
        p.landing        = 16.;
        p.leader         = round % 2 == 0 ? LeaderStyle::Straight : LeaderStyle::Elbow;
        p.mode           = CalloutLayout::Columns;
        p.max_per_column = 50;
        DYNAMIC_SECTION("round " << round << ", " << n << " labels") {
            const CalloutLayoutResult r = layout_callouts(labels, p);
            CHECK_FALSE(r.legend);
            check_layout(labels, p, r);
            for (const PlacedLabel &pl : r.labels) {
                REQUIRE(pl.leader.size() >= 2);
                // A leader starts at its anchor and ends on its label's inner edge.
                const PointD end = pl.leader.back();
                CHECK_THAT(end.y, WithinAbs(pl.box.y + 0.5 * pl.box.h, 1e-6));
                CHECK_THAT(end.x, WithinAbs(pl.side < 0 ? pl.box.right() : pl.box.x, 1e-6));
            }
        }
    }
}

TEST_CASE("Callout layout: the column split", "[Showcase][Callouts]")
{
    LayoutParams p;
    p.frame_w = 1000.;
    p.frame_h = 600.;
    p.margin  = 20.;
    p.gap     = 8.;
    p.landing = 12.;
    p.model_center_x = 500.;

    SECTION("anchors left of the model's middle go left, the rest right") {
        std::vector<LayoutLabel> labels(4);
        labels[0].anchor = { 420., 100. };
        labels[1].anchor = { 580., 120. };
        labels[2].anchor = { 450., 400. };
        labels[3].anchor = { 600., 450. };
        for (LayoutLabel &l : labels) {
            l.w = 150.;
            l.h = 40.;
        }
        const CalloutLayoutResult r = layout_callouts(labels, p);
        CHECK(r.labels[0].side == -1);
        CHECK(r.labels[2].side == -1);
        CHECK(r.labels[1].side == 1);
        CHECK(r.labels[3].side == 1);
        // Left labels hug the column's inner edge, right labels start at it.
        CHECK_THAT(r.labels[0].box.right(), WithinAbs(r.labels[2].box.right(), 1e-9));
        CHECK_THAT(r.labels[1].box.x, WithinAbs(r.labels[3].box.x, 1e-9));
        // Each label sits level with its anchor when nothing is in the way.
        CHECK_THAT(r.labels[0].box.y + 20., WithinAbs(100., 1e-6));
        CHECK_THAT(r.labels[3].box.y + 20., WithinAbs(450., 1e-6));
        check_layout(labels, p, r);
    }

    SECTION("a full column hands its innermost label to the other side") {
        std::vector<LayoutLabel> labels(5);
        for (size_t i = 0; i < labels.size(); ++i) {
            labels[i].anchor = { 300. + 30. * double(i), 100. + 90. * double(i) };   // all left of the middle
            labels[i].w      = 120.;
            labels[i].h      = 40.;
        }
        p.max_per_column = 3;
        const CalloutLayoutResult r = layout_callouts(labels, p);
        CHECK_FALSE(r.legend);
        int left = 0, right = 0;
        for (const PlacedLabel &pl : r.labels)
            (pl.side < 0 ? left : right)++;
        CHECK(left == 3);
        CHECK(right == 2);
        // The two moved across are the ones nearest the middle.
        CHECK(r.labels[3].side == 1);
        CHECK(r.labels[4].side == 1);
        check_layout(labels, p, r);
    }

    SECTION("labels stay clear of the watermark") {
        std::vector<LayoutLabel> labels(3);
        for (size_t i = 0; i < labels.size(); ++i) {
            labels[i].anchor = { 700., 480. + 30. * double(i) };
            labels[i].w      = 150.;
            labels[i].h      = 40.;
        }
        p.reserved = { { 800., 500., 180., 80. } };   // bottom right corner
        const CalloutLayoutResult r = layout_callouts(labels, p);
        for (const PlacedLabel &pl : r.labels) {
            CHECK(pl.side == 1);
            CHECK(pl.box.bottom() <= 500. - p.gap + 1e-6);
        }
        check_layout(labels, p, r);
    }

    SECTION("pinned labels stay put, inside the frame") {
        std::vector<LayoutLabel> labels(2);
        labels[0].anchor     = { 400., 300. };
        labels[0].w          = 100.;
        labels[0].h          = 30.;
        labels[0].pinned     = true;
        labels[0].pinned_pos = { 50., 60. };
        labels[1]            = labels[0];
        labels[1].pinned_pos = { 990., 590. };   // dragged past the corner
        const CalloutLayoutResult r = layout_callouts(labels, p);
        CHECK(r.labels[0].side == 0);
        CHECK_THAT(r.labels[0].box.x, WithinAbs(50., 1e-9));
        CHECK_THAT(r.labels[0].box.y, WithinAbs(60., 1e-9));
        CHECK_THAT(r.labels[1].box.right(), WithinAbs(1000., 1e-9));
        CHECK_THAT(r.labels[1].box.bottom(), WithinAbs(600., 1e-9));
        REQUIRE(r.labels[0].leader.size() == 2);
        CHECK_THAT(r.labels[0].leader.back().x, WithinAbs(150., 1e-9));   // the side facing the anchor
    }
}

TEST_CASE("Callout layout: the legend when crowded", "[Showcase][Callouts]")
{
    LayoutParams p;
    p.frame_w        = 1000.;
    p.frame_h        = 600.;
    p.margin         = 20.;
    p.gap            = 8.;
    p.legend_badge   = 30.;
    p.max_per_column = 8;
    auto many = [](int n, double h) {
        std::vector<LayoutLabel> labels(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            labels[size_t(i)].anchor = { 200. + 10. * i, 50. + 25. * i };
            labels[size_t(i)].w      = 160.;
            labels[size_t(i)].h      = h;
        }
        return labels;
    };

    SECTION("more than a column's share in each column") {
        const std::vector<LayoutLabel> labels = many(17, 30.);
        const CalloutLayoutResult      r      = layout_callouts(labels, p);
        REQUIRE(r.legend);
        std::set<int> numbers;
        for (const PlacedLabel &pl : r.labels) {
            CHECK(pl.leader.empty());
            CHECK(pl.side == 0);
            numbers.insert(pl.number);
            CHECK(pl.box.x >= r.legend_box.x - 1e-6);
            CHECK(pl.box.right() <= r.legend_box.right() + 1e-6);
            CHECK(pl.box.bottom() <= r.legend_box.bottom() + 1e-6);
        }
        CHECK(numbers.size() == 17);
        CHECK(*numbers.begin() == 1);
        CHECK(*numbers.rbegin() == 17);
        // The key is on the side away from the model (anchors are left of the middle).
        CHECK(r.legend_box.x > 500.);
    }

    SECTION("labels too tall to stack beside the model") {
        const CalloutLayoutResult r = layout_callouts(many(8, 150.), p);
        CHECK(r.legend);
    }

    SECTION("Columns keeps the columns however crowded") {
        p.mode = CalloutLayout::Columns;
        const CalloutLayoutResult r = layout_callouts(many(17, 30.), p);
        CHECK_FALSE(r.legend);
    }

    SECTION("Legend asked for") {
        p.mode = CalloutLayout::Legend;
        const CalloutLayoutResult r = layout_callouts(many(2, 30.), p);
        CHECK(r.legend);
        CHECK(r.labels[0].number == 1);
        CHECK(r.labels[1].number == 2);
    }

    SECTION("a few labels get columns") {
        const CalloutLayoutResult r = layout_callouts(many(4, 30.), p);
        CHECK_FALSE(r.legend);
    }
}

TEST_CASE("Callout layout: segment crossing test", "[Showcase][Callouts]")
{
    CHECK(segments_cross({ 0, 0 }, { 10, 10 }, { 0, 10 }, { 10, 0 }));
    CHECK_FALSE(segments_cross({ 0, 0 }, { 10, 0 }, { 0, 1 }, { 10, 1 }));
    CHECK(segments_cross({ 0, 0 }, { 10, 0 }, { 5, 0 }, { 15, 0 }));   // collinear overlap
    CHECK(segments_cross({ 0, 0 }, { 10, 0 }, { 10, 0 }, { 10, 5 }));  // touching
    CHECK_FALSE(segments_cross({ 0, 0 }, { 10, 0 }, { 11, 0 }, { 20, 0 }));
    CHECK(polylines_cross({ { 0, 0 }, { 5, 5 }, { 10, 0 } }, { { 0, 3 }, { 10, 3 } }));
}

// ------------------------------------------------------------------------------------------------
// Colour names (D8)
// ------------------------------------------------------------------------------------------------

TEST_CASE("Callout colour names", "[Showcase][Callouts]")
{
    SECTION("the built-in list is sound") {
        std::set<std::string> names;
        for (const NamedColor &c : named_colors()) {
            std::array<float, 4> rgba;
            INFO(c.name);
            CHECK(parse_hex_color(c.hex, rgba));
            CHECK(names.insert(c.name).second);
        }
        CHECK(named_colors().size() >= 140);
    }
    SECTION("CIEDE2000") {
        CHECK_THAT(delta_e2000({ 10, 20, 30 }, { 10, 20, 30 }), WithinAbs(0., 1e-9));
        CHECK_THAT(delta_e2000({ 255, 0, 0 }, { 0, 0, 255 }), WithinAbs(delta_e2000({ 0, 0, 255 }, { 255, 0, 0 }), 1e-9));
        CHECK_THAT(delta_e2000({ 0, 0, 0 }, { 255, 255, 255 }), WithinAbs(100., 0.5));
        CHECK(delta_e2000({ 200, 30, 30 }, { 201, 30, 30 }) < 1.);
    }
    SECTION("nearest name") {
        ColorNameMatch m = nearest_color_name("#FFFFFF");
        CHECK(m.name == "White");
        CHECK(m.exact);
        m = nearest_color_name("#fe0101");
        CHECK(m.name == "Red");
        CHECK_FALSE(m.exact);
        CHECK(nearest_color_name("#1A1A1A").name == "Jet Black");
        CHECK(nearest_color_name("#4169E0").name == "Royal Blue");
        CHECK(nearest_color_name("not a colour").name.empty());
    }
    SECTION("library, then the user's text, then the nearest name") {
        CHECK(resolve_color_name("Galaxy Black", "My black", "#1A1A1A") == "Galaxy Black");
        CHECK(resolve_color_name("", "My black", "#1A1A1A") == "My black");
        CHECK(resolve_color_name("", "  ", "#1A1A1A") == "\xE2\x89\x88 Jet Black");
        CHECK(resolve_color_name("", "", "#FFFFFF") == "White");   // exactly a named colour: no "≈"
        CHECK(resolve_color_name("", "", "#FFFFFF", [](const char *s) { return std::string("[") + s + "]"; }) == "[White]");
        CHECK(resolve_color_name("", "", "bad").empty());
    }
    SECTION("the Snapmaker colour library") {
        FilamentColorItem white;
        white.colorData.colors = { "#FFFFFF" };
        white.colorNames       = { { "en", "Snow White" }, { "de_DE", "Schneeweiss" } };
        FilamentColorItem duo;
        duo.colorData.colors = { "#FF0000", "#0000ff" };
        duo.colorNames       = { { "en", "Red Blue" } };
        FilamentColorInfo pla;
        pla.filamentName = "Snapmaker PLA @U1";
        pla.colors       = { white, duo };
        FilamentColorItem other;
        other.colorData.colors = { "#123456" };
        other.colorNames       = { { "zh_CN", "Deep" } };
        FilamentColorInfo petg;
        petg.filamentName = "Snapmaker PETG @U1";
        petg.colors       = { other };
        const std::vector<FilamentColorInfo> lib{ pla, petg };

        CHECK(library_color_name(&pla, lib, { "#ffffff" }, "en") == "Snow White");
        CHECK(library_color_name(&pla, lib, { "#FFFFFF" }, "de_DE") == "Schneeweiss");
        CHECK(library_color_name(&pla, lib, { "#FFFFFF" }, "de") == "Schneeweiss");   // short code
        CHECK(library_color_name(&pla, lib, { "#FFFFFF" }, "fr_FR") == "Snow White");   // English fallback
        CHECK(library_color_name(&pla, lib, { "#FF0000", "#0000FF" }, "en") == "Red Blue");
        CHECK(library_color_name(&pla, lib, { "#FF0000" }, "en").empty());   // half of a two-colour SKU
        // Not in the filament's own family: any library filament of that exact colour.
        CHECK(library_color_name(&pla, lib, { "#123456" }, "en") == "Deep");
        CHECK(library_color_name(nullptr, lib, { "#FFFFFF" }, "en") == "Snow White");
        CHECK(library_color_name(nullptr, lib, { "#FFFFFE" }, "en").empty());
    }
}

// ------------------------------------------------------------------------------------------------
// Label text (D7)
// ------------------------------------------------------------------------------------------------

TEST_CASE("Callout label text", "[Showcase][Callouts]")
{
    SECTION("product names") {
        CHECK(product_name("Polymaker PolyTerra PLA @BBL X1C 0.4 nozzle", "Polymaker") == "PolyTerra PLA");
        CHECK(product_name("Bambu PLA Basic @BBL A1", "Bambu Lab") == "PLA Basic");
        CHECK(product_name("Generic PETG", "Generic") == "PETG");
        CHECK(product_name("My PLA", "") == "My PLA");
        CHECK(product_name("Polymaker", "Polymaker") == "Polymaker");
        CHECK(product_name("eSUN PLA+ @U1", "Polymaker") == "eSUN PLA+");
    }
    SECTION("numbers") {
        CHECK(format_weight(38.4) == "38 g");
        CHECK(format_weight(4.24) == "4.2 g");   // one decimal below 10 g
        CHECK(format_price(0.954, "$") == "$0.95");
        CHECK(format_price(12., "EUR ") == "EUR 12.00");
    }

    LabelFilament f;
    f.vendor          = "Polymaker";
    f.preset_name     = "Polymaker PLA Pro @U1";
    f.type            = "PLA";
    f.finish          = "Silk";
    f.standard_finish = false;
    f.colors          = { "#1a1a1a" };
    f.color_name      = "Galaxy Black";
    f.weight_g        = 38.;
    f.price           = 0.95;
    Callout c;
    c.key = make_callout_key(1);
    CalloutFields fields;
    LabelWords    words;

    SECTION("the default fields (D7)") {
        const CalloutLabel l = build_callout_label(c, f, fields, words);
        CHECK(l.title == "Polymaker PLA Pro");
        CHECK(l.color_name == "Galaxy Black");
        CHECK(l.hex == "#1A1A1A");
        CHECK(l.swatches.size() == 1);
        CHECK(l.details == "PLA \xC2\xB7 Silk");
        CHECK(callout_first_line(l) == "Polymaker PLA Pro \xE2\x80\x93 Galaxy Black");
    }
    SECTION("every field") {
        fields.weight = fields.price = true;
        const CalloutLabel l = build_callout_label(c, f, fields, words);
        CHECK(l.details == "PLA \xC2\xB7 Silk \xC2\xB7 38 g \xC2\xB7 $0.95");
    }
    SECTION("weight and price are left out until the plate is sliced") {
        fields.weight = fields.price = true;
        f.weight_g = f.price = -1.;
        CHECK(build_callout_label(c, f, fields, words).details == "PLA \xC2\xB7 Silk");
    }
    SECTION("fields off") {
        fields.brand = fields.hex = fields.type = false;
        const CalloutLabel l = build_callout_label(c, f, fields, words);
        CHECK(l.title == "PLA Pro");
        CHECK(l.hex.empty());
        CHECK(l.swatches.empty());
        CHECK(l.details.empty());
        fields.product = fields.color_name = false;
        CHECK(build_callout_label(c, f, fields, words).empty());
    }
    SECTION("a standard finish, or one the type already says, is not repeated") {
        f.standard_finish = true;
        CHECK(build_callout_label(c, f, fields, words).details == "PLA");
        f.standard_finish = false;
        f.type            = "PLA Silk";
        CHECK(build_callout_label(c, f, fields, words).details == "PLA Silk");
    }
    SECTION("the user's first line and note") {
        c.title = "  Body  ";
        c.note  = "Glow in the dark";
        const CalloutLabel l = build_callout_label(c, f, fields, words);
        CHECK(l.title == "Body");
        CHECK(l.note == "Glow in the dark");
        CHECK(callout_first_line(l) == "Body \xE2\x80\x93 Galaxy Black");
    }
    SECTION("a colour change says from where") {
        c.source = CalloutSource::ColorChange;
        c.z_from = 12.43;
        words.from_height = "ab %1% mm";
        CHECK(build_callout_label(c, f, fields, words).details == "PLA \xC2\xB7 Silk \xC2\xB7 ab 12.4 mm");
    }
    SECTION("a mixed filament lists its parts") {
        f.mixed       = true;
        f.mixed_parts = "PLA Basic 60% + PLA Matte 40%";
        f.type.clear();
        f.standard_finish = true;
        const CalloutLabel l = build_callout_label(c, f, fields, words);
        CHECK(l.title == "Mixed filament");
        CHECK(l.details == "PLA Basic 60% + PLA Matte 40%");
    }
    SECTION("ink against the backdrop") {
        CHECK(auto_ink(1.) == "#1E1E1E");
        CHECK(auto_ink(0.1) == "#F5F5F5");
        CHECK_THAT(hex_luminance("#FFFFFF"), WithinAbs(1., 1e-9));
        CHECK_THAT(hex_luminance("#000000"), WithinAbs(0., 1e-9));
        Background bg;
        bg.type   = BackgroundType::VerticalGradient;
        bg.color1 = "#FFFFFF";
        bg.color2 = "#000000";
        CHECK(backdrop_luminance(bg, 0.5, 0.1) > 0.8);
        CHECK(backdrop_luminance(bg, 0.5, 0.9) < 0.2);
        bg.type = BackgroundType::RadialGradient;
        CHECK(backdrop_luminance(bg, 0.5, 0.5) > backdrop_luminance(bg, 0.0, 0.0));
        bg.type = BackgroundType::Transparent;
        CHECK_THAT(backdrop_luminance(bg, 0.2, 0.2), WithinAbs(1., 1e-9));
    }
}

// ------------------------------------------------------------------------------------------------
// Which callouts a plate gets
// ------------------------------------------------------------------------------------------------

TEST_CASE("Callout targets from filament assignments", "[Showcase][Callouts]")
{
    Model model;
    ModelObject *a = add_cube(model, 20., 0., 2);     // filament 2
    ModelObject *b = add_cube(model, 20., 50., 2);    // also filament 2
    ModelObject *c = add_cube(model, 30., 100.);      // the default filament (1)
    (void) b;

    SECTION("one callout per filament across objects") {
        const std::vector<CalloutTarget> t = plan_callout_targets(model, { 0, 1, 2 }, nullptr);
        REQUIRE(t.size() == 2);
        CHECK(t[0].key == "f1");
        CHECK(t[0].objects == std::vector<int>{ 2 });
        CHECK(t[1].key == "f2");
        CHECK(t[1].objects == std::vector<int>({ 0, 1 }));
        CHECK(t[1].source == CalloutSource::Object);
        // Only the plate's objects count.
        CHECK(plan_callout_targets(model, { 2 }, nullptr).size() == 1);
    }

    SECTION("a part with its own filament, a modifier draws nothing") {
        ModelVolume *part = a->add_volume(make_cube(5., 5., 5.));
        part->config.set("extruder", 3);
        ModelVolume *mod = a->add_volume(make_cube(5., 5., 5.), ModelVolumeType::PARAMETER_MODIFIER);
        mod->config.set("extruder", 4);
        const std::vector<CalloutTarget> t = plan_callout_targets(model, { 0 }, nullptr);
        REQUIRE(t.size() == 2);
        CHECK(t[0].key == "f2");
        CHECK(t[1].key == "f3");
        CHECK(t[1].source == CalloutSource::Part);
    }

    SECTION("painted regions") {
        TriangleSelector selector(c->volumes.front()->mesh());
        selector.set_facet(0, EnforcerBlockerType(3));
        selector.set_facet(1, EnforcerBlockerType(3));
        REQUIRE(c->volumes.front()->mmu_segmentation_facets.set(selector));
        const std::vector<CalloutTarget> t = plan_callout_targets(model, { 2 }, nullptr);
        REQUIRE(t.size() == 2);
        CHECK(t[0].key == "f1");   // the unpainted rest
        CHECK(t[1].key == "f3");
        CHECK(t[1].source == CalloutSource::Paint);

        // Everything painted: no callout for the volume's own filament.
        TriangleSelector all(c->volumes.front()->mesh());
        for (int i = 0; i < int(c->volumes.front()->mesh().its.indices.size()); ++i)
            all.set_facet(i, EnforcerBlockerType(4));
        REQUIRE(c->volumes.front()->mmu_segmentation_facets.set(all));
        const std::vector<CalloutTarget> t2 = plan_callout_targets(model, { 2 }, nullptr);
        REQUIRE(t2.size() == 1);
        CHECK(t2[0].key == "f4");
    }

    SECTION("height range modifiers") {
        c->layer_config_ranges[{ 10., 20. }].set("extruder", 5);
        c->layer_config_ranges[{ 20., 25. }].set("layer_height", 0.1);   // no filament: nothing to say
        const CalloutRegions regions(model, { 2 }, nullptr);
        const std::vector<CalloutTarget> t = regions.targets();
        REQUIRE(t.size() == 2);
        CHECK(t[0].key == "f1");
        CHECK(t[1].key == "f5");
        CHECK(t[1].source == CalloutSource::HeightRange);
        const BaseRegion base = regions.base_regions(2).front();
        CHECK(regions.classify(2, base, 5.).key == "f1");
        CHECK(regions.classify(2, base, 15.).key == "f5");
        CHECK(regions.classify(2, base, 22.).key == "f1");
        // A part with its own filament keeps it through the range.
        CHECK(regions.classify(2, BaseRegion{ 3, false, CalloutSource::Part }, 15.).key == "f3");
    }

    SECTION("colour and tool changes") {
        CustomGCode::Info info;
        CustomGCode::Item cc;
        cc.print_z  = 12.4;
        cc.type     = CustomGCode::ColorChange;
        cc.extruder = 1;
        cc.color    = "#00ff00";
        CustomGCode::Item tc;
        tc.print_z  = 25.;
        tc.type     = CustomGCode::ToolChange;
        tc.extruder = 3;
        CustomGCode::Item pause;
        pause.print_z = 5.;
        pause.type    = CustomGCode::PausePrint;
        info.gcodes   = { tc, cc, pause };
        const CalloutRegions regions(model, { 0, 2 }, &info);
        const std::vector<CalloutTarget> t = regions.targets();
        // Object 2 (filament 1, 30 mm): green from 12.4 mm, filament 3 from 25 mm. Object 0
        // (filament 2, 20 mm): unchanged, the colour change is filament 1's.
        REQUIRE(t.size() == 4);
        CHECK(t[0].key == "f1");
        CHECK(t[1].key == "f1@12.40#00FF00");
        CHECK(t[1].source == CalloutSource::ColorChange);
        CHECK_THAT(t[1].z_from, WithinAbs(12.4, 1e-9));
        CHECK(t[1].color == "#00FF00");
        CHECK(t[2].key == "f2");
        CHECK(t[3].key == "f3@25.00");
        CHECK(t[3].source == CalloutSource::ToolChange);
        CHECK(t[3].objects == std::vector<int>{ 2 });
        const BaseRegion base = regions.base_regions(2).front();
        CHECK(regions.classify(2, base, 1.).key == "f1");
        CHECK(regions.classify(2, base, 20.).key == "f1@12.40#00FF00");
        CHECK(regions.classify(2, base, 28.).key == "f3@25.00");
    }

    SECTION("objects that are not there are skipped") {
        CHECK(plan_callout_targets(model, { -1, 7 }, nullptr).empty());
    }
}

TEST_CASE("Callout keys and pruning", "[Showcase][Callouts]")
{
    CHECK(make_callout_key(3) == "f3");
    CHECK(make_callout_key(0) == "f1");
    CHECK(make_callout_key(2, 12.345) == "f2@12.35");   // %.2f
    CHECK(make_callout_key(2, 1., "#ff00aa") == "f2@1.00#FF00AA");

    Callouts c = sample_callouts();
    Callout gone;
    gone.key      = "f9";
    gone.filament = 9;
    c.items.push_back(gone);
    // Two objects left, four filaments: object 1's anchor and filament 9's callout go.
    const int n = prune_callouts(c, 1, 4);
    CHECK(n == 2);
    REQUIRE(c.items.size() == 2);
    CHECK(c.items[0].anchors.size() == 1);
    CHECK(c.items[0].anchors[0].object_idx == 0);
    CHECK(c.find("f9") == nullptr);
    // A callout left with no anchor forgets it was placed by hand.
    prune_callouts(c, 0, 4);
    CHECK(c.items[1].anchors.empty());
    CHECK_FALSE(c.items[1].user_anchor);
}

// ------------------------------------------------------------------------------------------------
// Anchors
// ------------------------------------------------------------------------------------------------

TEST_CASE("Callout anchors sit deepest inside their region", "[Showcase][Callouts]")
{
    const int w = 60, h = 40;
    std::vector<int> map(size_t(w * h), -1);
    auto fill = [&](int x0, int y0, int x1, int y1, int cls) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                map[size_t(y * w + x)] = cls;
    };

    SECTION("a rectangle: its middle") {
        fill(10, 10, 30, 20, 0);
        const auto a = find_anchor_pixels(map, w, h, 1);
        REQUIRE(a[0].size() == 1);
        CHECK(a[0][0].x == 20);
        CHECK(a[0][0].y == 15);
        CHECK(a[0][0].area == 21 * 11);
        CHECK_THAT(a[0][0].clearance, WithinAbs(6., 1e-9));
    }
    SECTION("an L shape: inside its thick part, never in the notch") {
        fill(0, 0, 39, 39, 0);
        fill(10, 0, 39, 29, -1);   // what is left is an L: a 10 px bar down the left, a 10 px foot
        const auto a = find_anchor_pixels(map, w, h, 1);
        REQUIRE(a[0].size() == 1);
        CHECK(map[size_t(a[0][0].y * w + a[0][0].x)] == 0);
    }
    SECTION("separate regions, largest first, at most three, specks dropped") {
        fill(0, 0, 9, 9, 0);      // 100
        fill(20, 0, 39, 9, 0);    // 200
        fill(0, 20, 4, 24, 0);    // 25
        fill(50, 30, 51, 31, 0);  // 4: a speck
        fill(30, 20, 35, 25, 1);  // another class
        auto a = find_anchor_pixels(map, w, h, 2, 3, 10);
        REQUIRE(a[0].size() == 3);
        CHECK(a[0][0].area == 200);
        CHECK(a[0][1].area == 100);
        CHECK(a[0][2].area == 25);
        REQUIRE(a[1].size() == 1);
        a = find_anchor_pixels(map, w, h, 2, 2, 10);
        CHECK(a[0].size() == 2);
        // Only specks: the biggest one still gets the anchor.
        std::vector<int> specks(size_t(w * h), -1);
        specks[5] = 0;
        a = find_anchor_pixels(specks, w, h, 1, 3, 10);
        CHECK(a[0].size() == 1);
    }
    SECTION("a region cut by the frame keeps away from the frame's edge") {
        fill(0, 0, 59, 9, 0);
        const auto a = find_anchor_pixels(map, w, h, 1);
        REQUIRE(a[0].size() == 1);
        CHECK(a[0][0].y >= 4);
        CHECK(a[0][0].y <= 5);
    }
    SECTION("nothing to anchor") {
        const auto a = find_anchor_pixels(map, w, h, 2);
        CHECK(a.size() == 2);
        CHECK(a[0].empty());
        CHECK(find_anchor_pixels({}, 0, 0, 1)[0].empty());
    }
}

// ------------------------------------------------------------------------------------------------
// Saving
// ------------------------------------------------------------------------------------------------

TEST_CASE("Callouts survive the scene's JSON", "[Showcase][Callouts]")
{
    Project p;
    Scene  &s = p.ensure_scene_for_plate(0);
    s.callouts = sample_callouts();
    Project back;
    REQUIRE(scenes_from_json(scenes_to_json(p), back));
    REQUIRE(back.scenes.size() == 1);
    CHECK(back.scenes[0].callouts == s.callouts);
    CHECK(back.scenes[0] == s);
    CHECK(scenes_to_json(back) == scenes_to_json(p));

    SECTION("an S2 scene (no callouts) reads with callouts off") {
        const std::string s2 = R"({"schema": 2, "active": "plate1", "scenes": [{"id": "plate1", "plate_index": 0,
            "background": {"type": "solid", "color1": "#101010"}, "materials": {"layer_lines": "real", "filaments": []}}]})";
        Project old;
        REQUIRE(scenes_from_json(s2, old));
        REQUIRE(old.scenes.size() == 1);
        CHECK(old.scenes[0].callouts == Callouts());
        CHECK_FALSE(old.scenes[0].callouts.enabled);
        CHECK(old.scenes[0].background.color1 == "#101010");
        CHECK_FALSE(old.read_only());
        // Written back it reads the same.
        Project again;
        REQUIRE(scenes_from_json(scenes_to_json(old), again));
        CHECK(again.scenes[0] == old.scenes[0]);
    }
    SECTION("an S1 scene (schema 1) too") {
        Project old;
        REQUIRE(scenes_from_json(R"({"schema": 1, "scenes": [{"id": "plate1", "plate_index": 0}]})", old));
        CHECK(old.scenes[0].callouts == Callouts());
    }
    SECTION("keys this build does not know are kept") {
        json j = json::parse(scenes_to_json(p));
        j["scenes"][0]["callouts"]["future"] = 7;
        j["scenes"][0]["callouts"]["style"]["shadow"] = "soft";
        j["scenes"][0]["callouts"]["fields"]["sku"] = true;
        Project q;
        REQUIRE(scenes_from_json(j.dump(), q));
        q.scenes[0].callouts.enabled = false;
        const json out = json::parse(scenes_to_json(q));
        CHECK(out["scenes"][0]["callouts"]["future"] == 7);
        CHECK(out["scenes"][0]["callouts"]["style"]["shadow"] == "soft");
        CHECK(out["scenes"][0]["callouts"]["fields"]["sku"] == true);
        CHECK(out["scenes"][0]["callouts"]["enabled"] == false);
    }
    SECTION("bad entries are dropped or clamped, never fatal") {
        const std::string text = R"({"schema": 2, "scenes": [{"id": "x", "plate_index": 0, "callouts": {
            "enabled": "yes", "layout": "spiral", "max_per_column": 900, "style": {"size_pct": 99, "shape": "star"},
            "items": [ {"filament": 0}, {"filament": 2, "key": "f2", "pinned": true},
                       {"filament": 2, "key": "f2", "title": "duplicate"}, "nonsense",
                       {"filament": 3, "anchors": [{"object": -4, "at": [1,2,3]}, {"object": 1, "at": [1,2]},
                                                   {"object": 2, "at": [4,5,6]}], "user_anchor": true} ] }}]})";
        Project q;
        REQUIRE(scenes_from_json(text, q));
        const Callouts &c = q.scenes[0].callouts;
        CHECK_FALSE(c.enabled);
        CHECK(c.layout == CalloutLayout::Auto);
        CHECK(c.max_per_column == 50);
        CHECK(c.style.size_pct == 10.f);
        CHECK(c.style.shape == CalloutShape::Card);
        REQUIRE(c.items.size() == 2);
        CHECK(c.items[0].key == "f2");
        CHECK_FALSE(c.items[0].pinned);   // pinned without a position
        CHECK(c.items[0].title.empty());
        CHECK(c.items[1].key == "f3");   // made from the filament
        // The second anchor's "at" has two numbers: it reads as the origin.
        REQUIRE(c.items[1].anchors.size() == 2);
        CHECK(c.items[1].anchors[0].object_idx == 1);
        CHECK(c.items[1].anchors[1].local == std::array<double, 3>{ { 4., 5., 6. } });
        CHECK(c.items[1].user_anchor);
    }
}

TEST_CASE("Callouts survive the project 3MF and stay out of a Bambu export", "[Showcase][Callouts][3MF]")
{
    const fs::path dir = scratch_dir("3mf");
    Model          model;
    add_cube(model, 20., 0.);
    model.showcase.ensure_scene_for_plate(0).callouts = sample_callouts();
    DynamicPrintConfig cfg = project_config();

    const SaveStrategy strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const std::string  project  = (dir / "callouts.3mf").string();
    {
        StoreParams sp;
        sp.path     = project.c_str();
        sp.model    = &model;
        sp.config   = &cfg;
        sp.strategy = strategy;
        REQUIRE(store_bbs_3mf(sp));
    }
    {
        Model                     loaded;
        DynamicPrintConfig        config;
        ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
        PlateDataPtrs             plates;
        std::vector<Preset *>     presets;
        bool                      is_bbl = false;
        Semver                    version;
        REQUIRE(load_bbs_3mf(project.c_str(), &config, &ctxt, &loaded, &plates, &presets, &is_bbl, &version, nullptr,
                             LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
        release_PlateData_list(plates);
        for (Preset *preset : presets)
            delete preset;
        REQUIRE(loaded.showcase.scenes.size() == 1);
        CHECK(loaded.showcase.scenes[0].callouts == model.showcase.scenes[0].callouts);
    }
    {
        const std::string    bambu = (dir / "bambu.3mf").string();
        BambuExport::Report  report;
        StoreParams          sp;
        sp.path         = bambu.c_str();
        sp.model        = &model;
        sp.config       = &cfg;
        sp.strategy     = strategy;
        sp.bambu_compat = true;
        sp.bambu_report = &report;
        REQUIRE(store_bbs_3mf(sp));
        CHECK(zip_entry(bambu, ARCHIVE_SCENES).empty());
    }
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
}
