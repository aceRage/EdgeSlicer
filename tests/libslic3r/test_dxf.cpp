#include <catch2/catch.hpp>

#include <libslic3r/Format/DXF.hpp>
#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/ImageTrace.hpp>
#include <libslic3r/BoundingBox.hpp>

#include <boost/filesystem.hpp>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

using namespace Slic3r;

namespace {

struct DxfRing
{
    std::string                         layer;
    bool                                closed = false;
    std::vector<std::pair<double, double>> pts;
};

struct DxfFile
{
    std::vector<std::pair<int, std::string>> groups;
    std::string                              acadver;
    int                                      insunits = -1;
    std::vector<std::string>                 table_layers;
    std::vector<DxfRing>                     rings;
};

// Tiny reader: group code / value pairs, R12 POLYLINE / VERTEX / SEQEND
DxfFile read_dxf(const std::string &path)
{
    DxfFile       f;
    std::ifstream in(path, std::ios::binary);
    std::string   a, b;
    while (std::getline(in, a) && std::getline(in, b))
        f.groups.emplace_back(std::atoi(a.c_str()), b);

    std::string section, table;
    DxfRing    *ring = nullptr;
    double      x = 0., y = 0.;
    bool        in_vertex = false;
    for (size_t i = 0; i < f.groups.size(); ++i) {
        const auto &[code, val] = f.groups[i];
        if (code == 0) {
            if (in_vertex && ring) {
                ring->pts.emplace_back(x, y);
                in_vertex = false;
            }
            if (val == "SECTION")
                section = f.groups[i + 1].second;
            else if (val == "TABLE")
                table = f.groups[i + 1].second;
            else if (val == "LAYER" && section == "TABLES")
                f.table_layers.push_back(f.groups[i + 1].second);
            else if (val == "POLYLINE") {
                f.rings.emplace_back();
                ring = &f.rings.back();
            } else if (val == "VERTEX")
                in_vertex = true;
            else if (val == "SEQEND")
                ring = nullptr;
            continue;
        }
        if (section == "HEADER") {
            if (code == 9 && val == "$ACADVER")
                f.acadver = f.groups[i + 1].second;
            if (code == 9 && val == "$INSUNITS")
                f.insunits = std::atoi(f.groups[i + 1].second.c_str());
        }
        if (section == "ENTITIES" && ring) {
            if (!in_vertex) {
                if (code == 8)
                    ring->layer = val;
                if (code == 70)
                    ring->closed = (std::atoi(val.c_str()) & 1) != 0;
            } else if (code == 10)
                x = std::atof(val.c_str());
            else if (code == 20)
                y = std::atof(val.c_str());
        }
    }
    return f;
}

ExPolygon square_with_hole(double x0, double y0, double size, double hole)
{
    ExPolygon e;
    auto      P = [](double x, double y) { return Point(scaled(x), scaled(y)); };
    e.contour   = Polygon({P(x0, y0), P(x0 + size, y0), P(x0 + size, y0 + size), P(x0, y0 + size)});
    const double m = (size - hole) / 2.;
    e.holes.emplace_back(Polygon({P(x0 + m, y0 + m), P(x0 + m, y0 + m + hole), P(x0 + m + hole, y0 + m + hole), P(x0 + m + hole, y0 + m)}));
    return e;
}

} // namespace

TEST_CASE("DXF export writes closed polylines in mm on named layers", "[DXF]")
{
    std::vector<std::pair<std::string, ExPolygons>> layers;
    layers.emplace_back("Red filament", ExPolygons{square_with_hole(0., 0., 20., 8.)});
    layers.emplace_back("Blue", ExPolygons{square_with_hole(30., 5., 10.5, 2.)});

    const boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("dxf_test_%%%%%%.dxf");
    std::string                   err;
    REQUIRE(export_dxf(path.string(), layers, &err));
    REQUIRE(err.empty());

    const DxfFile f = read_dxf(path.string());
    boost::filesystem::remove(path);

    CHECK(f.acadver == "AC1009");
    CHECK(f.insunits == 4);
    CHECK(f.groups.back() == std::make_pair(0, std::string("EOF")));

    // layer table: 0 plus the two sanitized names
    REQUIRE(f.table_layers.size() == 3);
    CHECK(f.table_layers[0] == "0");
    CHECK(f.table_layers[1] == "Red_filament");
    CHECK(f.table_layers[2] == "Blue");

    // contour and hole of both layers, all closed, never a repeated first vertex
    REQUIRE(f.rings.size() == 4);
    for (const DxfRing &r : f.rings) {
        CHECK(r.closed);
        REQUIRE(r.pts.size() == 4);
    }
    CHECK(f.rings[0].layer == "Red_filament");
    CHECK(f.rings[1].layer == "Red_filament");
    CHECK(f.rings[2].layer == "Blue");
    CHECK(f.rings[3].layer == "Blue");

    // vertices in mm
    CHECK(f.rings[0].pts[0].first == Approx(0.));
    CHECK(f.rings[0].pts[1].first == Approx(20.));
    CHECK(f.rings[0].pts[2].second == Approx(20.));
    CHECK(f.rings[1].pts[0].first == Approx(6.));
    CHECK(f.rings[1].pts[2].first == Approx(14.));
    CHECK(f.rings[2].pts[0].first == Approx(30.));
    CHECK(f.rings[2].pts[1].first == Approx(40.5));
    CHECK(f.rings[2].pts[0].second == Approx(5.));
    CHECK(f.rings[3].pts[0].first == Approx(34.25));
}

TEST_CASE("DXF layer names are valid and unique", "[DXF]")
{
    CHECK(dxf_sanitize_layer_name("a b/c:d") == "a_b_c_d");
    CHECK(dxf_sanitize_layer_name("") == "LAYER");
    CHECK(dxf_sanitize_layer_name(std::string(50, 'x')).size() == 31);

    std::vector<std::pair<std::string, ExPolygons>> layers;
    layers.emplace_back("Same", ExPolygons{square_with_hole(0., 0., 5., 1.)});
    layers.emplace_back("Same", ExPolygons{square_with_hole(10., 0., 5., 1.)});
    const std::string s = dxf_to_string(layers);
    CHECK(s.find("\nSame\n") != std::string::npos);
    CHECK(s.find("\nSame_2\n") != std::string::npos);
}

TEST_CASE("DXF export reports errors", "[DXF]")
{
    std::string err;
    CHECK_FALSE(export_dxf("nowhere_dxf_dir/x.dxf", {{"A", ExPolygons{square_with_hole(0., 0., 5., 1.)}}}, &err));
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK_FALSE(export_dxf("unused.dxf", {{"A", ExPolygons{}}}, &err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("Image trace layers are flipped to Y up in the frame of the image", "[DXF]")
{
    ImageTraceResult result;
    result.width  = 40.;
    result.height = 20.;
    ImageTraceLayer l;
    // a 10 x 5 mm rectangle in the top left corner of the image (SVG: Y down)
    l.shape.emplace_back(ExPolygon(Polygon({Point(scaled(0.), scaled(0.)), Point(scaled(10.), scaled(0.)), Point(scaled(10.), scaled(5.)), Point(scaled(0.), scaled(5.))})));
    l.color = {255, 0, 0};
    result.layers.push_back(l);
    result.layers.push_back(l);

    auto layers = image_trace_dxf_layers(result, {"PLA Red"});
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].first == "PLA Red");
    CHECK(layers[1].first == "Level 2 #FF0000");
    const BoundingBox bb = get_extents(layers[0].second);
    // top left in SVG is the top of the image in DXF as well: Y from 15 to 20
    CHECK(unscale<double>(bb.min.y()) == Approx(15.));
    CHECK(unscale<double>(bb.max.y()) == Approx(20.));
    CHECK(unscale<double>(bb.max.x()) == Approx(10.));
    CHECK(layers[0].second.front().contour.is_counter_clockwise());
}
