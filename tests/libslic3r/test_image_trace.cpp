#include <catch2/catch.hpp>

#include <libslic3r/ImageTrace.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/NSVGUtils.hpp>
#include <libslic3r/UntrustedInput.hpp>


#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <memory>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Color = std::array<uint8_t, 4>;

// Image filled by color(x, y), 4x4 super sampled (anti-aliased like a real exported logo)
TraceImage make_image(int w, int h, const std::function<Color(double, double)> &color)
{
    TraceImage img;
    img.width  = w;
    img.height = h;
    img.rgba.resize(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double sum[4] = {0., 0., 0., 0.};
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    Color c = color(x + (sx + 0.5) / 4., y + (sy + 0.5) / 4.);
                    for (int k = 0; k < 4; ++k)
                        sum[k] += c[k];
                }
            for (int k = 0; k < 4; ++k)
                img.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(k)] = uint8_t(std::lround(sum[k] / 16.));
        }
    return img;
}

const Color WHITE{255, 255, 255, 255};
const Color BLACK{0, 0, 0, 255};
const Color CLEAR{0, 0, 0, 0};

bool in_disc(double x, double y, double cx, double cy, double r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

double area_mm2(const ExPolygons &shape)
{
    double a = 0.;
    for (const ExPolygon &e : shape)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

// shape loaded the same way as the SVG gizmo does (centered by its bounding box)
ExPolygons load(const std::string &svg)
{
    NSVGimage_ptr image = nsvgParse(svg);
    REQUIRE(image != nullptr);
    ExPolygons result;
    for (ExPolygonsWithId &s : create_shape_with_ids(*image, NSVGLineParams{1e6}))
        expolygons_append(result, std::move(s.expoly));
    return result;
}

ImageTraceParams base_params()
{
    ImageTraceParams p;
    p.width       = 40.; // 200 px -> 0.2 mm per pixel
    p.max_side    = 1024;
    p.keep_source = false;
    p.group_id    = "0123456789abcdef";
    return p;
}

} // namespace

TEST_CASE("Trace a ring: one shape with one hole and the right area", "[ImageTrace]")
{
    // black ring on white, R = 60 px, r = 30 px
    TraceImage img = make_image(200, 200, [](double x, double y) {
        return in_disc(x, y, 100, 100, 60) && !in_disc(x, y, 100, 100, 30) ? BLACK : WHITE;
    });
    ImageTraceParams p = base_params();
    p.colors           = 1;
    ImageTraceResult r = trace_image(img, p);
    REQUIRE(r.is_valid());
    CHECK_FALSE(r.from_alpha);
    CHECK(r.threshold > 0);
    CHECK(r.threshold < 255);
    REQUIRE(r.layers.size() == 1);
    const ImageTraceLayer &layer = r.layers.front();
    REQUIRE(layer.shape.size() == 1);
    CHECK(layer.shape.front().holes.size() == 1);
    double expected = PI * (12. * 12. - 6. * 6.); // 0.2 mm per pixel
    CHECK_THAT(area_mm2(layer.shape), WithinRel(expected, 0.015));
    // centered in the image
    CHECK_THAT(layer.offset.x(), WithinAbs(0., 0.05));
    CHECK_THAT(layer.offset.y(), WithinAbs(0., 0.05));
    CHECK_THAT(r.height, WithinAbs(40., 1e-9));

    SECTION("SVG round trip keeps the shape and the metadata")
    {
        ExPolygons loaded = load(layer.svg);
        CHECK_THAT(area_mm2(loaded), WithinRel(area_mm2(layer.shape), 0.001));
        std::optional<ImageTraceMeta> meta = read_image_trace_meta(layer.svg);
        REQUIRE(meta.has_value());
        CHECK(meta->params == p);
        CHECK(meta->layer == 0);
        CHECK(meta->layers == 1);
        CHECK(meta->source_png.empty());
        CHECK_THAT(meta->width, WithinAbs(40., 1e-6));
    }

    SECTION("Invert traces the white part")
    {
        p.invert             = true;
        ImageTraceResult inv = trace_image(img, p);
        REQUIRE(inv.is_valid());
        // outer rectangle with the ring as hole + the inner disc
        CHECK(inv.layers.front().shape.size() == 2);
        CHECK_THAT(area_mm2(inv.layers.front().shape), WithinRel(40. * 40. - expected, 0.015));
    }
}

TEST_CASE("Two islands, alpha cut-out and offsets", "[ImageTrace]")
{
    // two opaque discs on transparent background, left one bigger
    TraceImage img = make_image(200, 100, [](double x, double y) {
        if (in_disc(x, y, 50, 50, 30))
            return Color{200, 30, 30, 255};
        if (in_disc(x, y, 160, 50, 20))
            return Color{200, 30, 30, 255};
        return CLEAR;
    });
    REQUIRE(img.has_alpha());
    ImageTraceParams p = base_params();
    p.colors           = 1;
    ImageTraceResult r = trace_image(img, p);
    REQUIRE(r.is_valid());
    CHECK(r.from_alpha);
    REQUIRE(r.layers.size() == 1);
    CHECK(r.layers.front().shape.size() == 2);
    CHECK_THAT(r.height, WithinAbs(20., 1e-9));
    double expected = PI * (6. * 6. + 4. * 4.);
    CHECK_THAT(area_mm2(r.layers.front().shape), WithinRel(expected, 0.015));
    // ink spans x 20..180 px -> centre 100 px = image centre; y centred too
    CHECK_THAT(r.layers.front().offset.x(), WithinAbs(0., 0.05));
    CHECK_THAT(r.layers.front().offset.y(), WithinAbs(0., 0.05));
    // average color of the shape
    CHECK(int(r.layers.front().color[0]) > 150);
    CHECK(int(r.layers.front().color[1]) < 80);
}

TEST_CASE("Posterise to colour levels, deterministic and aligned", "[ImageTrace]")
{
    // red square top left, blue disc bottom right, dark green bar, on white background
    auto color = [](double x, double y) -> Color {
        if (x >= 20 && x < 80 && y >= 20 && y < 80)
            return {220, 20, 20, 255};
        if (in_disc(x, y, 140, 140, 40))
            return {20, 40, 220, 255};
        if (x >= 20 && x < 180 && y >= 185 && y < 195)
            return {10, 90, 10, 255};
        return WHITE;
    };
    TraceImage       img = make_image(200, 200, color);
    ImageTraceParams p   = base_params();
    p.colors             = 4;
    p.remove_background  = true;

    ImageTraceResult r = trace_image(img, p);
    REQUIRE(r.is_valid());
    REQUIRE(r.layers.size() == 3);
    CHECK(r.colors_found == 3);
    // sorted from dark to light, every layer has a distinct dominant channel
    std::vector<int> dominant;
    for (const ImageTraceLayer &l : r.layers)
        dominant.push_back(int(std::max_element(l.color.begin(), l.color.end()) - l.color.begin()));
    std::sort(dominant.begin(), dominant.end());
    CHECK(dominant == std::vector<int>{0, 1, 2});

    for (const ImageTraceLayer &l : r.layers) {
        int ch = int(std::max_element(l.color.begin(), l.color.end()) - l.color.begin());
        if (ch == 0) {
            // red square: centre (50,50) px -> (10,10) mm from top left, image centre (20,20)
            CHECK_THAT(l.offset.x(), WithinAbs(-10., 0.1));
            CHECK_THAT(l.offset.y(), WithinAbs(10., 0.1)); // Y up
            CHECK_THAT(area_mm2(l.shape), WithinRel(12. * 12., 0.03));
        } else if (ch == 2) {
            CHECK_THAT(l.offset.x(), WithinAbs(8., 0.1));
            CHECK_THAT(l.offset.y(), WithinAbs(-8., 0.1));
        }
        // the layer loaded as SVG volume (centered) moved by the offset lands where it was in the image
        ExPolygons  loaded = load(l.svg);
        BoundingBox bb     = get_extents(loaded);
        CHECK(std::abs(bb.center().x()) <= 2);
        CHECK(std::abs(bb.center().y()) <= 2);
    }

    SECTION("Same input, same output")
    {
        ImageTraceResult again = trace_image(img, p);
        REQUIRE(again.layers.size() == r.layers.size());
        for (size_t i = 0; i < r.layers.size(); ++i) {
            CHECK(again.layers[i].svg == r.layers[i].svg);
            CHECK(again.layers[i].color == r.layers[i].color);
        }
    }

    SECTION("Palette does not depend on the working resolution")
    {
        p.max_side           = 100;
        ImageTraceResult low = trace_image(img, p, {}, false);
        REQUIRE(low.layers.size() == r.layers.size());
        for (size_t i = 0; i < r.layers.size(); ++i)
            CHECK(low.layers[i].color == r.layers[i].color);
    }

    SECTION("Fewer colours than wanted")
    {
        p.colors = 2;
        ImageTraceResult two = trace_image(img, p);
        REQUIRE(two.is_valid());
        CHECK(two.layers.size() == 2);
    }

    SECTION("Background kept")
    {
        p.remove_background = false;
        ImageTraceResult all = trace_image(img, p);
        REQUIRE(all.is_valid());
        CHECK(all.layers.size() == 4);
        // flat levels overlap a little but cover the whole image together
        double sum = 0.;
        for (const ImageTraceLayer &l : all.layers)
            sum += area_mm2(l.shape);
        CHECK(sum >= 40. * 40. * 0.999);
        CHECK(sum <= 40. * 40. * 1.05);
    }

    SECTION("Stacked levels are nested and deeper")
    {
        p.layout     = TraceLayout::Stacked;
        p.depth      = 0.6;
        p.depth_step = 0.4;
        ImageTraceResult st = trace_image(img, p);
        REQUIRE(st.layers.size() == 3);
        for (size_t i = 1; i < st.layers.size(); ++i) {
            CHECK(area_mm2(st.layers[i].shape) < area_mm2(st.layers[i - 1].shape));
            CHECK_THAT(st.layers[i].depth, WithinAbs(0.6 + 0.4 * i, 1e-9));
        }
    }
}

TEST_CASE("Source image is embedded only into the first level", "[ImageTrace]")
{
    TraceImage img = make_image(120, 60, [](double x, double y) {
        if (x >= 10 && x < 50 && y >= 10 && y < 50)
            return Color{250, 200, 0, 255};
        return in_disc(x, y, 90, 30, 20) ? Color{0, 0, 120, 255} : WHITE;
    });
    std::string png = encode_trace_source(img, 64);
    REQUIRE_FALSE(png.empty());
    TraceImage decoded;
    REQUIRE(decode_trace_image(png, decoded));
    CHECK(decoded.width == 64);
    CHECK(decoded.height == 32);

    ImageTraceParams p = base_params();
    p.colors           = 3;
    p.keep_source      = true;
    ImageTraceResult r = trace_image(img, p, png);
    REQUIRE(r.layers.size() == 2);
    std::optional<ImageTraceMeta> first  = read_image_trace_meta(r.layers[0].svg, true);
    std::optional<ImageTraceMeta> second = read_image_trace_meta(r.layers[1].svg, true);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->source_png == png);
    CHECK(second->source_png.empty());
    CHECK(first->layers == 2);
    CHECK(second->layer == 1);
    CHECK(first->params.group_id == second->params.group_id);
    // without asking for the source it is not decoded
    CHECK(read_image_trace_meta(r.layers[0].svg, false)->source_png.empty());

    p.keep_source = false;
    ImageTraceResult no = trace_image(img, p, png);
    REQUIRE(no.is_valid());
    CHECK(read_image_trace_meta(no.layers[0].svg, true)->source_png.empty());
    CHECK(no.layers[0].svg.size() < r.layers[0].svg.size());
}

TEST_CASE("Image headers are read before decoding", "[ImageTrace]")
{
    SECTION("PNG, gray, RGB and RGBA")
    {
        TraceImage gray = make_image(30, 20, [](double x, double) { return x < 15 ? BLACK : WHITE; });
        TraceImage rgba = make_image(30, 20, [](double x, double) { return x < 15 ? Color{200, 10, 10, 255} : CLEAR; });
        for (const TraceImage *img : {&gray, &rgba}) {
            std::string png = encode_trace_source(*img, 64);
            REQUIRE_FALSE(png.empty());
            int w = 0, h = 0;
            REQUIRE(trace_image_dimensions(png, w, h));
            CHECK(w == 30);
            CHECK(h == 20);
            TraceImage decoded;
            REQUIRE(decode_trace_image(png, decoded));
            CHECK(decoded.width == 30);
            CHECK(decoded.height == 20);
            CHECK(decoded.has_alpha() == img->has_alpha());
            CHECK(decoded.rgba == img->rgba); // PNG is lossless
        }
    }

    SECTION("JPG")
    {
        std::unique_ptr<std::string> data = read_from_disk(std::string(TEST_DATA_DIR) + "/image_trace/sign_on_white.jpg");
        REQUIRE(data != nullptr);
        int w = 0, h = 0;
        REQUIRE(trace_image_dimensions(*data, w, h));
        CHECK(w == 320);
        CHECK(h == 200);
        TraceImage decoded;
        REQUIRE(decode_trace_image(*data, decoded));
        CHECK(decoded.width == 320);
        CHECK(decoded.height == 200);
        CHECK_FALSE(decoded.has_alpha());

        // black frame, red heart, green triangle; the white background is left out
        ImageTraceParams p = base_params();
        p.colors           = 4;
        ImageTraceResult r = trace_image(decoded, p);
        REQUIRE(r.is_valid());
        CHECK(r.layers.size() == 3);
    }

    SECTION("Huge image is refused from its header")
    {
        TraceImage  img = make_image(30, 20, [](double x, double) { return x < 15 ? BLACK : WHITE; });
        std::string data = encode_trace_source(img, 64);
        REQUIRE(data.size() > 24);
        // patch IHDR to 30000 x 30000 px
        for (size_t i : {size_t(16), size_t(20)}) {
            data[i]     = 0;
            data[i + 1] = 0;
            data[i + 2] = char(0x75);
            data[i + 3] = char(0x30);
        }
        TraceImage  decoded;
        std::string error;
        CHECK_FALSE(decode_trace_image(data, decoded, &error));
        CHECK(error.find("too large") != std::string::npos);
        CHECK(decoded.empty());
    }

    SECTION("Unknown or broken data")
    {
        TraceImage decoded;
        CHECK_FALSE(decode_trace_image("GIF89a not supported", decoded));
        CHECK_FALSE(decode_trace_image(std::string(), decoded));
        TraceImage  img  = make_image(30, 20, [](double x, double) { return x < 15 ? BLACK : WHITE; });
        std::string data = encode_trace_source(img, 64);
        data.resize(40); // header is fine, the pixels are missing
        CHECK_FALSE(decode_trace_image(data, decoded));
        CHECK(decoded.empty());
    }
}

TEST_CASE("Too detailed image is refused before any volume is created", "[ImageTrace]")
{
    // one pixel checkerboard: every pixel is a contour
    TraceImage img = make_image(700, 700, [](double x, double y) { return ((int(x) + int(y)) % 2) ? BLACK : WHITE; });
    ImageTraceParams p = base_params();
    p.colors           = 1;
    p.blur             = 0.;
    p.despeckle        = 0.;
    p.detail           = 0.05;
    p.auto_threshold   = false;
    p.threshold        = 128;
    ImageTraceResult r = trace_image(img, p);
    CHECK(r.too_detailed);
    CHECK_FALSE(r.is_valid());
    CHECK_FALSE(r.error.empty());

    SECTION("Smoothing makes it a plain square")
    {
        p.blur              = 2.;
        p.despeckle         = 0.05;
        ImageTraceResult ok = trace_image(img, p);
        // blurred checkerboard is uniform gray 0.5 - either nothing or the whole square
        CHECK_FALSE(ok.too_detailed);
    }
}

TEST_CASE("Sample images of the hand test", "[ImageTrace]")
{
    auto load_sample = [](const char *name) {
        std::unique_ptr<std::string> data = read_from_disk(std::string(TEST_DATA_DIR) + "/image_trace/" + name);
        REQUIRE(data != nullptr);
        TraceImage img;
        REQUIRE(decode_trace_image(*data, img));
        return img;
    };
    ImageTraceParams p = base_params();
    p.colors           = 4;

    SECTION("Badge with transparent background: three colours, anti-aliased edges are no colour")
    {
        TraceImage img = load_sample("badge_transparent.png");
        CHECK(img.has_alpha());
        ImageTraceResult r = trace_image(img, p);
        REQUIRE(r.is_valid());
        CHECK(r.layers.size() == 3);
        // the white star has no hole, the navy disc has the star as a hole, the yellow ring one hole
        for (const ImageTraceLayer &l : r.layers)
            CHECK(l.shape.size() == 1);
    }

    SECTION("Sign on white: frame, heart and triangle")
    {
        TraceImage       img = load_sample("sign_on_white.png");
        ImageTraceResult r   = trace_image(img, p);
        REQUIRE(r.is_valid());
        CHECK(r.layers.size() == 3);
        // one colour traced alone: the frame and the shapes together
        p.colors              = 1;
        ImageTraceResult mono = trace_image(img, p);
        REQUIRE(mono.is_valid());
        CHECK(mono.layers.size() == 1);
        CHECK(mono.layers.front().shape.size() == 3);
    }

    SECTION("Gradient posterised to stacked levels")
    {
        TraceImage img      = load_sample("radial_gradient.png");
        p.remove_background = false;
        p.layout            = TraceLayout::Stacked;
        ImageTraceResult r  = trace_image(img, p);
        REQUIRE(r.is_valid());
        CHECK(r.layers.size() == 4);
        for (size_t i = 1; i < r.layers.size(); ++i)
            CHECK(area_mm2(r.layers[i].shape) < area_mm2(r.layers[i - 1].shape));
    }
}

namespace {

// The owner's settings of the neon line art hand test
ImageTraceParams line_art_params()
{
    ImageTraceParams p  = base_params();
    p.colors            = 6;
    p.remove_background = true;
    p.blur              = 0.;
    p.despeckle         = 0.05;
    p.detail            = 0.1;
    p.layout            = TraceLayout::Stacked;
    p.depth_step        = 0.4;
    return p;
}

// Index of the layer with this colour (every channel within tolerance), -1 when none
int find_layer(const ImageTraceResult &r, const std::array<int, 3> &rgb, int tolerance = 30)
{
    for (size_t i = 0; i < r.layers.size(); ++i) {
        bool same = true;
        for (int c = 0; c < 3; ++c)
            same &= std::abs(int(r.layers[i].color[size_t(c)]) - rgb[size_t(c)]) <= tolerance;
        if (same)
            return int(i);
    }
    return -1;
}

std::string layer_colors(const ImageTraceResult &r)
{
    std::string out;
    for (const ImageTraceLayer &l : r.layers)
        out += image_trace_color_to_hex(l.color) + " ";
    return out;
}

} // namespace

TEST_CASE("Line art on a large dark background keeps its small colours", "[ImageTrace]")
{
    // Five rings in distinct colours (pink next to purple, green next to teal) on a dark
    // background which covers about 90 % of the image, optionally with a soft glow around them
    const std::array<double, 3>                  background{30, 28, 34};
    const std::vector<std::array<int, 3>>        colors{{150, 70, 235}, {240, 90, 200}, {50, 215, 205}, {90, 225, 80}, {245, 245, 245}};
    const std::vector<std::pair<double, double>> centers{{40, 40}, {110, 50}, {160, 120}, {60, 150}, {130, 170}};
    auto rings = [&](double radius, double half_width, bool glow) {
        return make_image(200, 200, [&, radius, half_width, glow](double x, double y) -> Color {
            std::array<double, 3> c = background;
            for (size_t i = 0; i < centers.size(); ++i) {
                double off = std::abs(std::hypot(x - centers[i].first, y - centers[i].second) - radius);
                double g   = off <= half_width ? 1. : (glow ? std::clamp(1. - (off - half_width) / 7., 0., 1.) * 0.55 : 0.);
                if (g > 0.)
                    for (int k = 0; k < 3; ++k)
                        c[size_t(k)] = background[size_t(k)] * (1. - g) + colors[i][size_t(k)] * g;
            }
            return {uint8_t(std::lround(c[0])), uint8_t(std::lround(c[1])), uint8_t(std::lround(c[2])), 255};
        });
    };
    auto check_five = [&](const TraceImage &img, const ImageTraceParams &p) {
        ImageTraceResult r = trace_image(img, p, {}, false);
        INFO("colours: " << layer_colors(r));
        REQUIRE(r.is_valid());
        CHECK(r.colors_found == 5);
        REQUIRE(r.layers.size() == 5);
        std::vector<int> found;
        for (const auto &c : colors)
            found.push_back(find_layer(r, c));
        INFO("layer of every colour: " << found[0] << " " << found[1] << " " << found[2] << " " << found[3] << " " << found[4]);
        CHECK(std::find(found.begin(), found.end(), -1) == found.end());
        std::sort(found.begin(), found.end());
        CHECK(std::unique(found.begin(), found.end()) == found.end());
    };

    SECTION("Rings with a glow, the owner's settings")
    {
        TraceImage img  = rings(18., 3., true);
        size_t     dark = 0;
        for (size_t i = 0; i < img.rgba.size(); i += 4)
            dark += img.rgba[i] < 40 && img.rgba[i + 1] < 40 && img.rgba[i + 2] < 45;
        CHECK(double(dark) / double(img.width * img.height) > 0.8);
        check_five(img, line_art_params());
    }
    SECTION("Rings with a glow, flat levels")
    {
        ImageTraceParams p = line_art_params();
        p.layout           = TraceLayout::Flat;
        check_five(rings(18., 3., true), p);
    }
    SECTION("Thin rings, every colour under 0.4 % of the image")
    {
        check_five(rings(7., 1., false), line_art_params());
    }
    SECTION("Thin rings with a glow")
    {
        check_five(rings(7., 1., true), line_art_params());
    }
}

TEST_CASE("Anti-aliased two colour logo gives two levels", "[ImageTrace]")
{
    // red disc and blue square touching each other on white: the anti-aliased edges (pink, purple,
    // light blue) are no colour of their own, however many colours are asked for
    TraceImage img = make_image(200, 200, [](double x, double y) -> Color {
        if (std::abs(x - 135) <= 35 && std::abs(y - 100) <= 35)
            return {20, 50, 200, 255};
        if (in_disc(x, y, 80, 100, 50))
            return {200, 20, 30, 255};
        return WHITE;
    });
    ImageTraceParams p  = base_params();
    p.remove_background = true;
    for (int colors : {2, 4, 6, 8}) {
        p.colors           = colors;
        ImageTraceResult r = trace_image(img, p, {}, false);
        INFO("colours wanted " << colors << ", found: " << layer_colors(r));
        REQUIRE(r.is_valid());
        CHECK(r.layers.size() == 2);
        CHECK(find_layer(r, {200, 20, 30}, 12) >= 0);
        CHECK(find_layer(r, {20, 50, 200}, 12) >= 0);
    }
    SECTION("Background kept")
    {
        p.remove_background = false;
        p.colors            = 6;
        ImageTraceResult r  = trace_image(img, p, {}, false);
        INFO("found: " << layer_colors(r));
        REQUIRE(r.is_valid());
        CHECK(r.layers.size() == 3);
    }
    SECTION("One colour still traces the whole logo")
    {
        p.colors           = 1;
        ImageTraceResult r = trace_image(img, p, {}, false);
        REQUIRE(r.is_valid());
        CHECK(r.layers.size() == 1);
        CHECK(r.layers.front().shape.size() == 1);
    }
}

TEST_CASE("Neon line art JPEG keeps the tiny pink diamond", "[ImageTrace]")
{
    std::unique_ptr<std::string> data = read_from_disk(std::string(TEST_DATA_DIR) + "/image_trace/neon_on_dark.jpg");
    REQUIRE(data != nullptr);
    TraceImage img;
    REQUIRE(decode_trace_image(*data, img));
    ImageTraceParams p = line_art_params();
    p.width            = 90.;
    p.max_side         = 512;
    ImageTraceResult r = trace_image(img, p, {}, false);
    INFO("colours: " << layer_colors(r));
    REQUIRE(r.is_valid());
    // purple, mint, green, pink and white; no muddy glow level
    CHECK(r.layers.size() == 5);
    CHECK(find_layer(r, {235, 95, 235}, 35) >= 0);
    CHECK(find_layer(r, {150, 85, 240}, 35) >= 0);
    CHECK(find_layer(r, {125, 250, 215}, 35) >= 0);
    CHECK(find_layer(r, {95, 225, 120}, 35) >= 0);
    for (const ImageTraceLayer &l : r.layers)
        CHECK(*std::max_element(l.color.begin(), l.color.end()) > 180);
}

// Hand test helper, not run by default: trace the image named by EDGESLICER_TRACE_IMAGE with the
// owner's settings (6 colours, background left out, resolution 1536, width 90 mm) and print the
// colours found, e.g.  EDGESLICER_TRACE_IMAGE=IMG_0125.jpg libslic3r_tests "[ImageTraceFile]"
TEST_CASE("Trace the image of EDGESLICER_TRACE_IMAGE", "[ImageTraceFile][.]")
{
    const char *path = std::getenv("EDGESLICER_TRACE_IMAGE");
    if (path == nullptr || *path == 0) {
        WARN("EDGESLICER_TRACE_IMAGE is not set");
        return;
    }
    std::unique_ptr<std::string> data = read_from_disk(path);
    REQUIRE(data != nullptr);
    TraceImage img;
    REQUIRE(decode_trace_image(*data, img));
    ImageTraceParams p = line_art_params();
    p.width            = 90.;
    p.max_side         = 1536;
    if (const char *colors = std::getenv("EDGESLICER_TRACE_COLORS"))
        p.colors = std::atoi(colors);
    ImageTraceResult r = trace_image(img, p, {}, false);
    std::cout << "image " << img.width << "x" << img.height << ", colours wanted " << p.colors << ", found " << r.colors_found
              << ", levels " << r.layers.size() << (r.error.empty() ? "" : ", error: " + r.error) << std::endl;
    for (const ImageTraceLayer &l : r.layers)
        std::cout << "  " << image_trace_color_to_hex(l.color) << "  rgb(" << int(l.color[0]) << "," << int(l.color[1]) << ","
                  << int(l.color[2]) << ")  outlines " << l.contours << ", depth " << l.depth << " mm" << std::endl;
    CHECK(r.is_valid());
}
