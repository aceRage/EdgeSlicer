#include <catch2/catch.hpp>

#include <libslic3r/SvgSplit.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/NSVGUtils.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// Same tolerance as the SVG gizmo (0.1 mm, squared and scaled)
const double TOLERANCE = (0.1 * 0.1) / SCALING_FACTOR / SCALING_FACTOR;

// 100 x 100 mm drawing, user units are millimeters
std::string svg_100mm(const std::string &body)
{
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100mm\" height=\"100mm\" viewBox=\"0 0 100 100\">" + body + "</svg>";
}

double area_mm2(const ExPolygons &shape)
{
    double a = 0.;
    for (const ExPolygon &e : shape)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

SvgSplitResult split(const std::string &svg, bool write_svg = true)
{
    NSVGimage_ptr image = nsvgParse(svg);
    REQUIRE(image != nullptr);
    return split_svg_by_shapes(*image, NSVGLineParams{TOLERANCE}, 0.01, write_svg);
}

// What the plain "SVG" import embosses: every fill and stroke unioned into one shape
ExPolygons default_union(const std::string &svg)
{
    NSVGimage_ptr image = nsvgParse(svg);
    REQUIRE(image != nullptr);
    ExPolygons all;
    for (const ExPolygonsWithId &s : create_shape_with_ids(*image, NSVGLineParams{TOLERANCE}))
        append(all, s.expoly);
    return union_ex(all);
}

ExPolygons all_parts(const SvgSplitResult &r)
{
    ExPolygons all;
    for (const SvgSplitPart &p : r.parts)
        append(all, p.shape);
    return union_ex(all);
}

// Sum of the areas of pairwise overlaps
double overlap_mm2(const SvgSplitResult &r)
{
    double overlap = 0.;
    for (size_t i = 0; i < r.parts.size(); ++i)
        for (size_t j = i + 1; j < r.parts.size(); ++j)
            overlap += area_mm2(intersection_ex(r.parts[i].shape, r.parts[j].shape));
    return overlap;
}

} // namespace

TEST_CASE("SVG split: background with a foreground of another colour gives two parts", "[SvgSplit]")
{
    const std::string svg = svg_100mm("<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#00003b\"/>"
                                      "<rect x=\"30\" y=\"30\" width=\"40\" height=\"40\" fill=\"#ffffff\"/>");

    // The plain import loses the foreground: the union is just the background square
    ExPolygons plain = default_union(svg);
    REQUIRE(plain.size() == 1);
    CHECK(plain.front().holes.empty());
    CHECK_THAT(area_mm2(plain), WithinRel(10000., 1e-3));

    SvgSplitResult r = split(svg);
    REQUIRE(r.is_valid());
    REQUIRE(r.parts.size() == 2);
    CHECK(r.painted == 2);
    CHECK(r.covered == 0);
    CHECK_THAT(r.width, WithinAbs(100., 1e-3));
    CHECK_THAT(r.height, WithinAbs(100., 1e-3));

    const SvgSplitPart &bg = r.parts[0];
    const SvgSplitPart &fg = r.parts[1];
    CHECK(bg.shape_index == 0);
    CHECK(fg.shape_index == 1);
    CHECK_FALSE(bg.is_stroke);
    CHECK(bg.color == std::array<uint8_t, 3>{0x00, 0x00, 0x3b});
    CHECK(fg.color == std::array<uint8_t, 3>{0xff, 0xff, 0xff});

    // The background part has a hole where the foreground is painted over it
    CHECK_THAT(area_mm2(bg.shape), WithinRel(10000. - 1600., 1e-3));
    REQUIRE(bg.shape.size() == 1);
    CHECK(bg.shape.front().holes.size() == 1);
    CHECK_THAT(area_mm2(fg.shape), WithinRel(1600., 1e-3));
    CHECK(overlap_mm2(r) < 0.01);
    // Together the parts still cover what the plain import covers
    CHECK_THAT(area_mm2(all_parts(r)), WithinRel(area_mm2(plain), 1e-3));

    CHECK(bg.svg.find("fill=\"#00003b\"") != std::string::npos);
    CHECK(fg.svg.find("fill=\"#ffffff\"") != std::string::npos);
}

TEST_CASE("SVG split: offset of a part keeps it in its place in the drawing", "[SvgSplit]")
{
    const std::string svg = svg_100mm("<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#000000\"/>"
                                      "<rect x=\"10\" y=\"10\" width=\"20\" height=\"20\" fill=\"#ff0000\"/>");
    SvgSplitResult r = split(svg);
    REQUIRE(r.parts.size() == 2);
    // Center of the drawing is (50, 50), the small square is at (20, 20) in SVG (Y down) => Y up offset
    CHECK_THAT(r.parts[1].offset.x(), WithinAbs(-30., 1e-3));
    CHECK_THAT(r.parts[1].offset.y(), WithinAbs(30., 1e-3));
    CHECK_THAT(r.parts[0].offset.x(), WithinAbs(0., 1e-3));
    CHECK_THAT(r.parts[0].offset.y(), WithinAbs(0., 1e-3));

    // The generated SVG of a part is embossed centered by its own bounding box (as every SVG volume),
    // moved by the offset it lands where it is in the drawing
    BoundingBox drawing;
    for (const SvgSplitPart &p : r.parts)
        drawing.merge(get_extents(p.shape));
    for (const SvgSplitPart &p : r.parts) {
        NSVGimage_ptr image = nsvgParse(p.svg);
        REQUIRE(image != nullptr);
        ExPolygonsWithIds loaded = create_shape_with_ids(*image, NSVGLineParams{TOLERANCE});
        REQUIRE(loaded.size() == 1);
        ExPolygons shape = loaded.front().expoly;
        CHECK_THAT(area_mm2(shape), WithinRel(area_mm2(p.shape), 1e-4));
        for (ExPolygon &e : shape)
            e.translate(Point(coord_t(scale_(p.offset.x())), coord_t(scale_(p.offset.y()))));
        ExPolygons expected = p.shape;
        for (ExPolygon &e : expected)
            e.translate(-drawing.center());
        CHECK_THAT(area_mm2(intersection_ex(shape, expected)), WithinRel(area_mm2(p.shape), 1e-3));
    }
}

TEST_CASE("SVG split: a stroke-only path becomes an outline part", "[SvgSplit]")
{
    // Bambu Studio turns a stroke into the outline of the stroke (one volume)
    const std::string svg = svg_100mm("<path d=\"M10 50 L90 50\" fill=\"none\" stroke=\"#ff0000\" stroke-width=\"4\"/>");
    SvgSplitResult r = split(svg);
    REQUIRE(r.is_valid());
    REQUIRE(r.parts.size() == 1);
    CHECK(r.parts[0].is_stroke);
    CHECK(r.parts[0].color == std::array<uint8_t, 3>{0xff, 0x00, 0x00});
    // 80 mm long, 4 mm wide, butt caps
    CHECK_THAT(area_mm2(r.parts[0].shape), WithinRel(320., 0.01));
}

TEST_CASE("SVG split: fill and stroke of one shape are two parts, the stroke is painted over the fill", "[SvgSplit]")
{
    const std::string svg =
        svg_100mm("<rect x=\"20\" y=\"20\" width=\"60\" height=\"60\" fill=\"#0000ff\" stroke=\"#ffff00\" stroke-width=\"4\"/>");
    SvgSplitResult r = split(svg);
    REQUIRE(r.parts.size() == 2);
    CHECK_FALSE(r.parts[0].is_stroke);
    CHECK(r.parts[1].is_stroke);
    CHECK(r.parts[0].shape_index == r.parts[1].shape_index);
    // the stroke is centered on the outline: 2 mm inside, 2 mm outside
    CHECK_THAT(area_mm2(r.parts[0].shape), WithinRel(56. * 56., 1e-3));
    CHECK_THAT(area_mm2(r.parts[1].shape), WithinRel(64. * 64. - 56. * 56., 1e-2));
    CHECK(overlap_mm2(r) < 0.01);
}

TEST_CASE("SVG split: strokes painted below later fills are covered (image trace style)", "[SvgSplit]")
{
    // Outline strokes first, then the fill which hides them, as in an image trace export
    const std::string svg = svg_100mm("<g stroke-width=\"2\" fill=\"none\">"
                                      "<path stroke=\"#808080\" d=\"M20 20 L80 20 L80 80 L20 80 Z\"/>"
                                      "</g>"
                                      "<rect x=\"10\" y=\"10\" width=\"80\" height=\"80\" fill=\"#ffffff\"/>"
                                      "<rect x=\"40\" y=\"40\" width=\"20\" height=\"20\" fill=\"#ffad00\"/>");
    SvgSplitResult r = split(svg);
    REQUIRE(r.is_valid());
    CHECK(r.painted == 3);
    CHECK(r.covered == 1);
    REQUIRE(r.parts.size() == 2);
    CHECK(r.parts[0].shape_index == 1);
    CHECK(r.parts[1].shape_index == 2);
    CHECK_THAT(area_mm2(r.parts[0].shape), WithinRel(6400. - 400., 1e-3));
    CHECK_THAT(area_mm2(r.parts[1].shape), WithinRel(400., 1e-3));
}

TEST_CASE("SVG split: a single-colour SVG keeps its shape", "[SvgSplit]")
{
    SECTION("one shape is one part, equal to the plain import")
    {
        const std::string svg = svg_100mm("<path d=\"M50 5 L95 95 L5 95 Z\" fill=\"#000000\"/>");
        SvgSplitResult    r   = split(svg);
        REQUIRE(r.parts.size() == 1);
        ExPolygons plain = default_union(svg);
        CHECK_THAT(area_mm2(r.parts[0].shape), WithinRel(area_mm2(plain), 1e-3));
        CHECK(r.parts[0].offset.norm() < 1e-3);
    }
    SECTION("separate shapes are separate parts (one part per shape, as Bambu Studio)")
    {
        const std::string svg = svg_100mm("<rect x=\"0\" y=\"0\" width=\"40\" height=\"100\" fill=\"#000000\"/>"
                                          "<rect x=\"60\" y=\"0\" width=\"40\" height=\"100\" fill=\"#000000\"/>");
        SvgSplitResult r = split(svg);
        REQUIRE(r.parts.size() == 2);
        CHECK_THAT(area_mm2(all_parts(r)), WithinRel(area_mm2(default_union(svg)), 1e-3));
        CHECK_THAT(r.parts[0].offset.x(), WithinAbs(-30., 1e-3));
        CHECK_THAT(r.parts[1].offset.x(), WithinAbs(30., 1e-3));
    }
}

TEST_CASE("SVG split: nothing to split", "[SvgSplit]")
{
    SvgSplitResult r = split(svg_100mm("<rect x=\"0\" y=\"0\" width=\"10\" height=\"10\" fill=\"none\"/>"));
    CHECK_FALSE(r.is_valid());
    CHECK_FALSE(r.error.empty());
}

TEST_CASE("SVG split: path of a part inside of 3mf", "[SvgSplit]")
{
    CHECK(svg_split_path_in_3mf("0123456789abcdef", 0) == "3D/svgsplit_0123456789abcdef_1.svg");
    CHECK(svg_split_color_to_hex({0xff, 0xad, 0x00}) == "#ffad00");
}

// Statistics of a real SVG, not run by default (the file is not part of the repository):
//   set EDGE_SVG_SPLIT_FILE=<path to .svg> and run: libslic3r_tests "[.SvgSplitFile]"
TEST_CASE("SVG split: statistics of a file", "[.SvgSplitFile]")
{
    const char *path = std::getenv("EDGE_SVG_SPLIT_FILE");
    REQUIRE(path != nullptr);
    std::unique_ptr<std::string> data = read_from_disk(path);
    REQUIRE(data != nullptr);
    SvgRefusal    refusal = SvgRefusal::None;
    NSVGimage_ptr image   = nsvgParse_checked(*data, refusal);
    REQUIRE(image != nullptr);

    ExPolygons     plain = default_union(*data);
    SvgSplitResult r     = split_svg_by_shapes(*image, NSVGLineParams{TOLERANCE});
    REQUIRE(r.is_valid());
    std::cout << "SVG split of " << path << "\n"
              << "  drawing " << r.width << " x " << r.height << " mm\n"
              << "  plain import: " << plain.size() << " islands, "
              << [&plain]() { size_t h = 0; for (const ExPolygon &e : plain) h += e.holes.size(); return h; }() << " holes, area "
              << area_mm2(plain) << " mm2\n"
              << "  painted fills+strokes " << r.painted << ", covered " << r.covered << ", parts " << r.parts.size() << "\n"
              << "  parts union area " << area_mm2(all_parts(r)) << " mm2, pairwise overlap " << overlap_mm2(r) << " mm2\n";
    std::map<std::string, std::pair<size_t, double>> by_color;
    for (size_t i = 0; i < r.parts.size(); ++i) {
        const SvgSplitPart &p = r.parts[i];
        auto &c = by_color[svg_split_color_to_hex(p.color)];
        ++c.first;
        c.second += area_mm2(p.shape);
        std::cout << "  part " << i << " shape " << p.shape_index << (p.is_stroke ? " stroke " : " fill ") << svg_split_color_to_hex(p.color)
                  << " area " << area_mm2(p.shape) << " mm2, islands " << p.shape.size() << ", svg " << p.svg.size() << " B\n";
    }
    for (const auto &[hex, c] : by_color)
        std::cout << "  colour " << hex << ": " << c.first << " parts, " << c.second << " mm2\n";
    CHECK(overlap_mm2(r) < 1.);
}
