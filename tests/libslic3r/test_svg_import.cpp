#include <catch2/catch.hpp>

#include <libslic3r/Format/svg.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/NSVGUtils.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

// File > Import (Ctrl+I) of an .svg and the command line load it by Model::read_from_file ->
// load_svg() (Format/svg.cpp): one 10 mm extruded part per fill and per stroke, not through the
// SVG gizmo. These tests guard that this loader keeps every island, hole and stroke.

using namespace Slic3r;
using Catch::Matchers::WithinRel;

namespace {

const double DEPTH = 10.; // extrusion of the loader [mm]

std::string svg_100mm(const std::string &body)
{
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100mm\" height=\"100mm\" viewBox=\"0 0 100 100\">" + body + "</svg>";
}

// Write the SVG into a per-process temp file and load it as File > Import does
struct Loaded
{
    Model       model;
    bool        ok = false;
    std::string message;
};
Loaded load(const std::string &svg, const std::string &tag)
{
    namespace fs = boost::filesystem;
    fs::path path = fs::temp_directory_path() / fs::unique_path("svg_import_" + tag + "_%%%%%%%%.svg");
    {
        boost::nowide::ofstream f(path.string(), std::ios::binary);
        f << svg;
    }
    Loaded l;
    l.ok = load_svg(path.string().c_str(), &l.model, l.message);
    fs::remove(path);
    return l;
}

// Area of the drawing covered by a part = its volume / extrusion depth
double area_mm2(const ModelVolume &v) { return std::abs(its_volume(v.mesh().its)) / DEPTH; }

double total_area_mm2(const Model &m)
{
    double a = 0.;
    for (const ModelObject *o : m.objects)
        for (const ModelVolume *v : o->volumes)
            a += area_mm2(*v);
    return a;
}

size_t volume_count(const Model &m)
{
    size_t n = 0;
    for (const ModelObject *o : m.objects)
        n += o->volumes.size();
    return n;
}

} // namespace

TEST_CASE("SVG import: one path with separate islands keeps every island", "[SvgImport]")
{
    // e.g. a word of text or several stars in one <path>
    Loaded l = load(svg_100mm("<path fill=\"#000000\" d=\"M10 10 H30 V30 H10 Z M50 10 H70 V30 H50 Z M10 50 H30 V70 H10 Z\"/>"), "islands");
    REQUIRE(l.ok);
    REQUIRE(volume_count(l.model) == 1);
    CHECK_THAT(total_area_mm2(l.model), WithinRel(3. * 400., 1e-3));
}

TEST_CASE("SVG import: an island inside of a hole is kept (fill-rule evenodd)", "[SvgImport]")
{
    Loaded l = load(svg_100mm("<path fill=\"#000000\" fill-rule=\"evenodd\" d=\"M0 0 H60 V60 H0 Z M10 10 H50 V50 H10 Z M20 20 H40 V40 H20 Z\"/>"),
                    "nested");
    REQUIRE(l.ok);
    REQUIRE(volume_count(l.model) == 1);
    CHECK_THAT(total_area_mm2(l.model), WithinRel(3600. - 1600. + 400., 1e-3));
}

TEST_CASE("SVG import: fill and stroke of one shape are both imported", "[SvgImport]")
{
    Loaded l = load(svg_100mm("<rect x=\"20\" y=\"20\" width=\"60\" height=\"60\" fill=\"#0000ff\" stroke=\"#ffff00\" stroke-width=\"4\"/>"),
                    "fillstroke");
    REQUIRE(l.ok);
    REQUIRE(volume_count(l.model) == 2);
    // fill 60 x 60, stroke ring 64^2 - 56^2 (they overlap, every part keeps its whole outline)
    CHECK_THAT(total_area_mm2(l.model), WithinRel(3600. + (64. * 64. - 56. * 56.), 1e-2));
}

TEST_CASE("SVG import: stroke-only and plain shapes", "[SvgImport]")
{
    Loaded l = load(svg_100mm("<path d=\"M10 50 L90 50\" fill=\"none\" stroke=\"#ff0000\" stroke-width=\"4\"/>"
                              "<g transform=\"translate(10 10)\"><ellipse cx=\"20\" cy=\"20\" rx=\"10\" ry=\"10\" fill=\"#00ff00\"/></g>"),
                    "stroke");
    REQUIRE(l.ok);
    REQUIRE(volume_count(l.model) == 2);
    // stroke 80 x 4 (butt caps) + circle r = 10 (flattened)
    CHECK_THAT(total_area_mm2(l.model), WithinRel(320. + 314.159, 1e-2));
}

// Compare the loader with the shapes of a real SVG, not run by default:
//   set EDGE_SVG_SPLIT_FILE=<path to .svg> and run: libslic3r_tests "[.SvgImportFile]"
TEST_CASE("SVG import: statistics of a file", "[.SvgImportFile]")
{
    const char *path = std::getenv("EDGE_SVG_SPLIT_FILE");
    REQUIRE(path != nullptr);
    Model       model;
    std::string message;
    bool        ok = load_svg(path, &model, message);
    std::cout << "File > Import of " << path << ": " << (ok ? "ok" : "failed") << " " << message << "\n";
    REQUIRE(ok);

    std::unique_ptr<std::string> data = read_from_disk(path);
    REQUIRE(data != nullptr);
    NSVGimage_ptr image = nsvgParse(*data);
    REQUIRE(image != nullptr);
    ExPolygonsWithIds shapes = create_shape_with_ids(*image, NSVGLineParams{(0.1 * 0.1) / SCALING_FACTOR / SCALING_FACTOR});
    double            expected = 0.;
    size_t            painted  = 0;
    for (const ExPolygonsWithId &s : shapes) {
        if (s.expoly.empty())
            continue;
        ++painted;
        double area = 0.;
        for (const ExPolygon &e : s.expoly)
            area += e.area() * SCALING_FACTOR * SCALING_FACTOR;
        expected += area;
        std::cout << "    shape " << s.id / 2 << ((s.id % 2) ? " stroke " : " fill ") << area << " mm2, islands " << s.expoly.size() << "\n";
    }
    std::cout << "  painted fills+strokes " << painted << ", their area " << expected << " mm2\n"
              << "  imported parts " << volume_count(model) << ", their area " << total_area_mm2(model) << " mm2\n";
    for (const ModelObject *o : model.objects)
        for (const ModelVolume *v : o->volumes)
            std::cout << "    " << v->name << " " << area_mm2(*v) << " mm2, " << v->mesh().its.indices.size() << " triangles\n";
    CHECK(volume_count(model) == painted);
}
