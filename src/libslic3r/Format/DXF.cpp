#include "DXF.hpp"

#include <algorithm>
#include <fstream>
#include <locale>
#include <set>
#include <sstream>

namespace Slic3r {

namespace {

void group(std::ostringstream &o, int code, const std::string &value) { o << code << "\n" << value << "\n"; }

void group(std::ostringstream &o, int code, double value) { o << code << "\n" << value << "\n"; }

void group(std::ostringstream &o, int code, int value) { o << code << "\n" << value << "\n"; }

void write_ring(std::ostringstream &o, const Polygon &ring, const std::string &layer)
{
    if (ring.points.size() < 3)
        return;
    group(o, 0, "POLYLINE");
    group(o, 8, layer);
    group(o, 66, 1); // vertices follow
    group(o, 10, 0.);
    group(o, 20, 0.);
    group(o, 30, 0.);
    group(o, 70, 1); // closed
    for (const Point &p : ring.points) {
        group(o, 0, "VERTEX");
        group(o, 8, layer);
        group(o, 10, unscale<double>(p.x()));
        group(o, 20, unscale<double>(p.y()));
        group(o, 30, 0.);
    }
    group(o, 0, "SEQEND");
    group(o, 8, layer);
}

} // namespace

std::string dxf_sanitize_layer_name(const std::string &name)
{
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '$';
        // a space or any other character (also each byte of UTF-8) is not safe in R12
        if (c == ' ')
            out += '_';
        else if (ok)
            out += c;
        else if (static_cast<unsigned char>(c) < 0x80)
            out += '_';
        else if (out.empty() || out.back() != '_')
            out += '_';
    }
    if (out.size() > 31)
        out.resize(31);
    if (out.empty())
        out = "LAYER";
    return out;
}

std::string dxf_to_string(const std::vector<std::pair<std::string, ExPolygons>> &layers)
{
    // unique, valid names
    std::vector<std::string> names;
    std::set<std::string>    used{"0"};
    for (const auto &l : layers) {
        std::string base = dxf_sanitize_layer_name(l.first);
        std::string name = base;
        for (int i = 2; used.count(name) > 0; ++i) {
            const std::string suffix = "_" + std::to_string(i);
            name = base.substr(0, 31 - suffix.size()) + suffix;
        }
        used.insert(name);
        names.push_back(name);
    }

    std::ostringstream o;
    o.imbue(std::locale::classic());
    o.precision(6);
    o << std::fixed;

    // HEADER
    group(o, 0, "SECTION");
    group(o, 2, "HEADER");
    group(o, 9, "$ACADVER");
    group(o, 1, "AC1009");
    group(o, 9, "$INSBASE");
    group(o, 10, 0.);
    group(o, 20, 0.);
    group(o, 30, 0.);
    group(o, 9, "$MEASUREMENT");
    group(o, 70, 1);
    group(o, 9, "$INSUNITS");
    group(o, 70, 4); // millimetres
    group(o, 0, "ENDSEC");

    // TABLES: continuous line type and the layers
    group(o, 0, "SECTION");
    group(o, 2, "TABLES");
    group(o, 0, "TABLE");
    group(o, 2, "LTYPE");
    group(o, 70, 1);
    group(o, 0, "LTYPE");
    group(o, 2, "CONTINUOUS");
    group(o, 70, 0);
    group(o, 3, "Solid line");
    group(o, 72, 65);
    group(o, 73, 0);
    group(o, 40, 0.);
    group(o, 0, "ENDTAB");
    group(o, 0, "TABLE");
    group(o, 2, "LAYER");
    group(o, 70, int(names.size()) + 1);
    auto layer_entry = [&o](const std::string &name, int color) {
        group(o, 0, "LAYER");
        group(o, 2, name);
        group(o, 70, 0);
        group(o, 62, color);
        group(o, 6, "CONTINUOUS");
    };
    layer_entry("0", 7);
    for (size_t i = 0; i < names.size(); ++i)
        layer_entry(names[i], int(i % 7) + 1);
    group(o, 0, "ENDTAB");
    group(o, 0, "ENDSEC");

    // ENTITIES
    group(o, 0, "SECTION");
    group(o, 2, "ENTITIES");
    for (size_t i = 0; i < layers.size(); ++i)
        for (const ExPolygon &e : layers[i].second) {
            write_ring(o, e.contour, names[i]);
            for (const Polygon &h : e.holes)
                write_ring(o, h, names[i]);
        }
    group(o, 0, "ENDSEC");
    group(o, 0, "EOF");
    return o.str();
}

bool export_dxf(const std::string &path, const std::vector<std::pair<std::string, ExPolygons>> &layers, std::string *err)
{
    auto fail = [err](const std::string &msg) {
        if (err != nullptr)
            *err = msg;
        return false;
    };
    const bool any = std::any_of(layers.begin(), layers.end(), [](const auto &l) { return !l.second.empty(); });
    if (!any)
        return fail("There is no outline to export.");
    const std::string content = dxf_to_string(layers);
    // text mode off: always "\n"; DXF readers accept it on every platform
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return fail("Cannot open the file for writing.");
    file.write(content.data(), std::streamsize(content.size()));
    file.close();
    if (!file)
        return fail("Writing the file failed.");
    return true;
}

} // namespace Slic3r
