// EdgeSlicer: curved tops, experimental (src/libslic3r/CurvedTops.cpp).
//
//  * OFF (the default): no conformal path anywhere and the extruded Z never goes back down.
//  * ON on a printer with no head data, or with an estimated head nobody has printed curved tops
//    with: refused, the slice is the feature-off slice, and the user is told why.
//  * ON, a 30 mm puck with a 1.2 mm spherical cap: conformal paths are appended to the top layer
//    and, after the last planar layer, the G-code extrudes below that layer's Z (the curved
//    layers follow the cap down to its rim).
//  * P1 hardening (tests/design_curved_tops_any_printer.md): the head limits come from the head
//    model only, every guard refuses visibly, the block ends with a lift, its wipes are speed
//    capped, and the head clearance check stops the export on a contact.
//  * The pure helpers: head limits, effective angle, first-layer gap flow.
//
// The head model is read from resources/head_clearance, so every case that slices with curved
// tops on points resources_dir() at the source tree's resources (ResourcesDirGuard).

#include <catch2/catch.hpp>

#include <algorithm>
#include <functional>
#include <fstream>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <map>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/ContourZ.hpp"
#include "libslic3r/CurvedTops.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/HeadClearance.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

#include <boost/filesystem.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// resources_dir() is empty in the test runner; the head model lives in resources/head_clearance.
// Points it at `dir` (default: the source tree's resources) for the life of the guard.
struct ResourcesDirGuard
{
    std::string saved;
    explicit ResourcesDirGuard(const std::string &dir = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources").string())
        : saved(resources_dir())
    {
        set_resources_dir(dir);
    }
    ~ResourcesDirGuard() { set_resources_dir(saved); }
};

// A cylinder of radius r and height h_cyl topped by a spherical cap of height h_cap.
TriangleMesh make_capped_puck(double r, double h_cyl, double h_cap, int rings = 24, int sectors = 96)
{
    const double rs = (r * r + h_cap * h_cap) / (2.0 * h_cap); // sphere radius of the cap
    auto cap_z = [&](double rr) { return h_cyl + std::sqrt(rs * rs - rr * rr) - (rs - h_cap); };

    indexed_triangle_set its;
    its.vertices.emplace_back(0.f, 0.f, float(cap_z(0.)));            // 0: top centre
    for (int i = 1; i <= rings; ++i) {
        const double rr = r * double(i) / double(rings);
        for (int j = 0; j < sectors; ++j) {
            const double a = 2.0 * M_PI * double(j) / double(sectors);
            its.vertices.emplace_back(float(rr * std::cos(a)), float(rr * std::sin(a)), float(cap_z(rr)));
        }
    }
    const int bottom0 = int(its.vertices.size());
    for (int j = 0; j < sectors; ++j) {
        const double a = 2.0 * M_PI * double(j) / double(sectors);
        its.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), 0.f);
    }
    const int bottom_centre = int(its.vertices.size());
    its.vertices.emplace_back(0.f, 0.f, 0.f);

    auto ring = [sectors](int i, int j) { return 1 + (i - 1) * sectors + (j % sectors); };
    for (int j = 0; j < sectors; ++j)
        its.indices.emplace_back(0, ring(1, j), ring(1, j + 1));
    for (int i = 1; i < rings; ++i)
        for (int j = 0; j < sectors; ++j) {
            its.indices.emplace_back(ring(i, j), ring(i + 1, j), ring(i + 1, j + 1));
            its.indices.emplace_back(ring(i, j), ring(i + 1, j + 1), ring(i, j + 1));
        }
    for (int j = 0; j < sectors; ++j) {
        const int b0 = bottom0 + j, b1 = bottom0 + (j + 1) % sectors;
        its.indices.emplace_back(b0, b1, ring(rings, j + 1));
        its.indices.emplace_back(b0, ring(rings, j + 1), ring(rings, j));
        its.indices.emplace_back(bottom_centre, b1, b0);
    }
    return TriangleMesh(std::move(its));
}

size_t count_curved_paths(const ExtrusionEntity *e)
{
    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
        size_t n = 0;
        for (const ExtrusionEntity *child : c->entities)
            n += count_curved_paths(child);
        return n;
    }
    if (const auto *p = dynamic_cast<const ExtrusionPath *>(e))
        return p->z_contour != nullptr && p->z_contour->curved_top ? 1 : 0;
    return 0;
}

// How far an extruding move ever goes back below the highest Z extruded before it. Planar
// G-code only ever extrudes upwards, so this is 0; z-hops and travels do not count.
double extrusion_dip(const std::string &gcode)
{
    std::istringstream in(gcode);
    std::string        line;
    double z = 0., highest = -1e9, lowest = 0.;
    while (std::getline(in, line)) {
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;
        const auto code = line.substr(0, line.find(';'));
        auto       word = [&code](char c, double &v) {
            const auto pos = code.find(std::string(" ") + c);
            if (pos == std::string::npos)
                return false;
            v = std::atof(code.c_str() + pos + 2);
            return true;
        };
        double v, e;
        if (word('Z', v))
            z = v;
        if (word('E', e) && e > 0. && (word('X', v) || word('Y', v))) {
            lowest  = std::min(lowest, z - highest);
            highest = std::max(highest, z);
        }
    }
    return lowest;
}

using ConfigItems = std::vector<std::pair<std::string, std::string>>;

bool uses_zaa(const ConfigItems &items)
{
    return std::any_of(items.begin(), items.end(), [](const auto &kv) { return kv.first == "zaa_enabled" && kv.second == "1"; });
}

DynamicPrintConfig puck_config(bool curved_tops, const ConfigItems &extra, int filaments = 1)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    if (filaments > 1) {
        config.set_num_extruders(unsigned(filaments));
        config.set_num_filaments(unsigned(filaments));
    }
    config.set_deserialize_strict({
        // Curved tops run only on a printer with a known head clearance.
        { "printer_model",              "Elegoo Centauri Carbon" },
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "sparse_infill_density",      "15%" },
        { "enable_support",             "0" },
        { "curved_tops_enabled",        curved_tops ? "1" : "0" },
        { "curved_tops_layers",         "3" },
        { "curved_tops_max_angle",      "15" },
    });
    for (const auto &[key, value] : extra)
        config.set_deserialize_strict(key, value);
    return config;
}

void slice_puck_items(Print &print, Model &model, bool curved_tops, const ConfigItems &extra, int filaments = 1)
{
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, print, model, puck_config(curved_tops, extra, filaments));
    print.process();
}

void slice_puck(Print &print, Model &model, bool curved_tops, std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    ConfigItems items;
    for (const ConfigBase::SetDeserializeItem &item : extra)
        items.emplace_back(item.opt_key, item.opt_value);
    slice_puck_items(print, model, curved_tops, items);
}

// Every travel inside the curved block, checked against what is already printed. The block starts
// after the last extrusion before the first curved one (an extrusion that changes Z along the move,
// or goes below the highest Z extruded so far) and ends at the last extrusion. Wipes (;WIPE_START..END)
// run back along the line just laid, at its heights, and are not travels.
struct CurvedTravels
{
    size_t travels       = 0;
    size_t slanted       = 0;   // XY travel that also moves Z
    size_t under_margin  = 0;   // clearance below the margin somewhere along the travel
    size_t arcs          = 0;   // G2/G3 inside the block: a spiral lift, or an arc-fitted curved line
    double max_travel_z  = 0.;
    double max_extrude_f = 0.;  // highest feedrate of an extrusion in the block, mm/min
    double min_clearance = 1e9;
};

CurvedTravels check_curved_travels(const std::string &gcode, double radius, double margin)
{
    struct Move { char kind; double x0, y0, z0, x1, y1, z1, f; };   // kind: 'e' extrude, 'A' arc extrude, 't' travel, 'w' wipe, 'a' arc, 'z' other
    std::vector<Move> moves;
    {
        std::istringstream in(gcode);
        std::string        line;
        double x = 0., y = 0., z = 0., f = 0.;
        bool   in_wipe = false;   // a wipe has no E when the whole retraction comes before it
        while (std::getline(in, line)) {
            if (line.rfind(";WIPE_START", 0) == 0)
                in_wipe = true;
            else if (line.rfind(";WIPE_END", 0) == 0)
                in_wipe = false;
            const std::string code = line.substr(0, line.find(';'));
            const bool arc = code.rfind("G2 ", 0) == 0 || code.rfind("G3 ", 0) == 0;
            if (!arc && code.rfind("G1 ", 0) != 0 && code.rfind("G0 ", 0) != 0)
                continue;
            auto word = [&code](char c, double &v) {
                const auto pos = code.find(std::string(" ") + c);
                if (pos == std::string::npos)
                    return false;
                v = std::atof(code.c_str() + pos + 2);
                return true;
            };
            double nx = x, ny = y, nz = z, e = 0.;
            word('X', nx);
            word('Y', ny);
            word('Z', nz);
            word('E', e);
            word('F', f);
            const bool xy = nx != x || ny != y;
            const char kind = arc ? (e > 0. ? 'A' : 'a') : (e > 0. && xy) ? 'e' : ((e < 0. || in_wipe) && xy) ? 'w' : xy ? 't' : 'z';
            moves.push_back({ kind, x, y, z, nx, ny, nz, f });
            x = nx; y = ny; z = nz;
        }
    }
    size_t first_curved = moves.size(), last_ext = 0;
    double highest = -1e9;
    auto extrudes = [](char kind) { return kind == 'e' || kind == 'A'; };
    for (size_t i = 0; i < moves.size(); ++i) {
        if (!extrudes(moves[i].kind))
            continue;
        last_ext = i;
        if (first_curved == moves.size() && (std::abs(moves[i].z1 - moves[i].z0) > 1e-6 || moves[i].z1 < highest - 1e-3))
            first_curved = i;
        highest = std::max(highest, moves[i].z1);
    }
    size_t start = 0;
    for (size_t i = 0; i < first_curved && i < moves.size(); ++i)
        if (extrudes(moves[i].kind))
            start = i + 1;
    // The layer change's own G-code (a Bambu timelapse / safe-position move) runs before the first
    // curved path is reached and is no curved-tops travel: start at the lift into the block, the
    // last Z-only move up before the first curved extrusion.
    for (size_t i = first_curved; i > start && i < moves.size(); --i)
        if (moves[i - 1].kind == 'z' && moves[i - 1].z1 > moves[i - 1].z0 + 1e-6) {
            start = i - 1;
            break;
        }

    // Extruded points, 0.1 mm apart, in a 0.5 mm grid.
    const double cell = 0.5;
    std::map<std::pair<long, long>, std::vector<Vec3d>> grid;
    auto key = [cell](double x, double y) { return std::make_pair(long(std::floor(x / cell)), long(std::floor(y / cell))); };
    auto highest_near = [&](double x, double y) {
        double best = -1e9;
        const auto k = key(x, y);
        const long n = long(std::ceil(radius / cell));
        for (long gx = k.first - n; gx <= k.first + n; ++gx)
            for (long gy = k.second - n; gy <= k.second + n; ++gy) {
                auto it = grid.find({ gx, gy });
                if (it != grid.end())
                    for (const Vec3d &p : it->second)
                        if ((p.x() - x) * (p.x() - x) + (p.y() - y) * (p.y() - y) <= radius * radius)
                            best = std::max(best, p.z());
            }
        return best;
    };
    CurvedTravels out;
    if (first_curved == moves.size())
        return out;
    for (size_t i = 0; i <= last_ext; ++i) {
        const Move &m = moves[i];
        const double len = std::hypot(m.x1 - m.x0, m.y1 - m.y0);
        const int    n   = std::max(1, int(std::ceil(len / 0.1)));
        if (i >= start && (m.kind == 'A' || m.kind == 'a'))
            ++out.arcs;
        if (extrudes(m.kind)) {
            if (i >= start)
                out.max_extrude_f = std::max(out.max_extrude_f, m.f);
            for (int j = 0; j <= n; ++j) {
                const double t = double(j) / n;
                const Vec3d  p(m.x0 + (m.x1 - m.x0) * t, m.y0 + (m.y1 - m.y0) * t, m.z0 + (m.z1 - m.z0) * t);
                grid[key(p.x(), p.y())].push_back(p);
            }
            continue;
        }
        if (i < start)
            continue;
        if (m.kind != 't')
            continue;
        ++out.travels;
        out.max_travel_z = std::max({ out.max_travel_z, m.z0, m.z1 });
        if (std::abs(m.z1 - m.z0) > 1e-6)
            ++out.slanted;
        double worst = 1e9;
        for (int j = 0; j <= n; ++j) {
            const double t = double(j) / n;
            const double h = highest_near(m.x0 + (m.x1 - m.x0) * t, m.y0 + (m.y1 - m.y0) * t);
            if (h > -1e8)
                worst = std::min(worst, m.z0 + (m.z1 - m.z0) * t - h);
        }
        out.min_clearance = std::min(out.min_clearance, worst);
        if (worst < margin)
            ++out.under_margin;
    }
    return out;
}

} // namespace

TEST_CASE("Curved tops: off by default leaves the slice planar", "[CurvedTops]")
{
    REQUIRE_FALSE(DynamicPrintConfig::full_print_config().opt_bool("curved_tops_enabled"));

    Print print;
    Model model;
    slice_puck(print, model, false);
    const PrintObject *object = print.objects().front();
    size_t curved = 0;
    for (const Layer *layer : object->layers())
        for (const LayerRegion *region : layer->regions())
            curved += count_curved_paths(&region->fills);
    CHECK(curved == 0);

    CHECK_THAT(extrusion_dip(Test::gcode(print)), WithinAbs(0., 1e-6));
}

// The G-code without the lines that name the setting itself (config block) or vary per run.
static std::string gcode_without_curved_tops_key(const std::string &gcode)
{
    std::istringstream in(gcode);
    std::string        out;
    for (std::string line; std::getline(in, line);)
        if (line.find("curved_tops_enabled") == std::string::npos && line.find("generated by") == std::string::npos &&
            line.find("label id") == std::string::npos)
            out += line + "\n";
    return out;
}

static std::string object_warning(const PrintObject &po, PrintStateBase::SlicingNotificationType id)
{
    for (const PrintStateBase::Warning &w : po.step_state_with_warnings(posContouring).warnings)
        if (w.message_id == int(id))
            return w.message;
    return {};
}

static std::string curved_tops_unavailable_warning(const PrintObject &po)
{
    return object_warning(po, PrintStateBase::SlicingCurvedTopsUnavailable);
}

static size_t count_all_curved_paths(const PrintObject &object)
{
    size_t curved = 0;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *region : layer->regions())
            curved += count_curved_paths(&region->fills);
    return curved;
}

// Curved tops on with `extra` must give exactly the feature-off G-code, leave no curved path, and
// say why with a warning that contains `reason`.
static void check_refused_like_off(const ConfigItems &extra, int filaments, const std::string &reason)
{
    Print print_off;
    Model model_off;
    slice_puck_items(print_off, model_off, false, extra, filaments);
    const std::string gcode_off = Test::gcode(print_off);
    CHECK(curved_tops_unavailable_warning(*print_off.objects().front()).empty());

    Print print_on;
    Model model_on;
    slice_puck_items(print_on, model_on, true, extra, filaments);
    REQUIRE_FALSE(print_on.objects().empty());
    for (const PrintObject *object : print_on.objects()) {
        CHECK(count_all_curved_paths(*object) == 0);
        CHECK_FALSE(object->layers().back()->curved_tops_block);
    }
    const std::string gcode_on = Test::gcode(print_on);
    // Z contouring itself moves extrusions down inside a layer; the identity with feature-off is what counts there.
    if (!uses_zaa(extra))
        CHECK_THAT(extrusion_dip(gcode_on), WithinAbs(0., 1e-6));
    CHECK(gcode_without_curved_tops_key(gcode_on) == gcode_without_curved_tops_key(gcode_off));

    const std::string warning = curved_tops_unavailable_warning(*print_on.objects().front());
    INFO("warning: " << warning);
    CHECK_FALSE(warning.empty());
    CHECK(warning.find(reason) != std::string::npos);
    CHECK(object_warning(*print_on.objects().front(), PrintStateBase::SlicingCurvedTopsSummary).empty());
}

// D1: no head data = refuse, never a generic default. P1: an estimated head nobody has printed
// curved tops with (the X1E shares the X1 Carbon's file; the U1 and the H2D have files of their own)
// is refused as well until the acknowledgement of P2. Output = feature-off output, user told.
TEST_CASE("Curved tops: a printer with no head data, or an unprinted estimate, prints as with the feature off and warns", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const std::string printer_model = GENERATE(std::string("Generic Printer"), std::string("Elegoo Centauri Carbon 2"),
                                               std::string("Bambu Lab X1E"), std::string("Bambu Lab X1"), std::string("Snapmaker U1"),
                                               std::string("Bambu Lab H2D"));
    INFO("printer_model = " << printer_model);
    const bool        has_file = printer_model != "Generic Printer" && printer_model != "Elegoo Centauri Carbon 2";
    check_refused_like_off({ { "printer_model", printer_model } }, 1,
                           has_file ? "is an estimate that no curved print has checked yet" : "has no print head clearance data");

    // The message names the printers curved tops does run on.
    Print print;
    Model model;
    slice_puck(print, model, true, { { "printer_model", printer_model } });
    const std::string warning = curved_tops_unavailable_warning(*print.objects().front());
    CHECK(warning.find("Elegoo Centauri Carbon.") != std::string::npos);
    CHECK(warning.find("Bambu Lab X1 Carbon") != std::string::npos);
}

// Each guard of design 3.2 that P1 adds: the object prints planar, exactly as with the feature off,
// and the warning names the reason.
TEST_CASE("Curved tops: every new guard refuses, warns and prints as with the feature off", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    struct Guard { const char *name; ConfigItems extra; int filaments; const char *reason; };
    static const std::vector<Guard> guards = {
        { "by object",           { { "print_sequence", "by object" } }, 1, "prints by object" },
        { "local-Z dithering",   { { "dithering_local_z_mode", "1" } }, 1, "local-Z dithering" },
        { "draft shield",        { { "skirt_loops", "1" }, { "draft_shield", "enabled" } }, 1, "draft shield" },
        { "tall skirt",          { { "skirt_loops", "1" }, { "skirt_height", "3" } }, 1, "skirt taller than one layer" },
        { "supports",            { { "enable_support", "1" } }, 1, "supports" },
        { "two filaments",       { { "sparse_infill_filament", "2" }, { "solid_infill_filament", "2" } }, 2, "more than one filament" },
        { "raft",                { { "raft_layers", "2" } }, 1, "raft" },
        { "stabilizers",         { { "stabilizer_supports", "auto" } }, 1, "side stabilizers" },
        { "Z contouring",        { { "zaa_enabled", "1" } }, 1, "Z contouring is on for it as well" },
        { "printable height",    { { "printable_height", "5.5" } }, 1, "above the printable height" },
    };
    const size_t i = GENERATE(range(size_t(0), size_t(10)));
    REQUIRE(i < guards.size());
    const Guard &g = guards[i];
    INFO("guard: " << g.name);
    check_refused_like_off(g.extra, g.filaments, g.reason);
}

// A 1-layer skirt and a brim are far below the band: allowed (design 3.2).
TEST_CASE("Curved tops: a one-layer skirt does not refuse", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    Print print;
    Model model;
    slice_puck(print, model, true, { { "skirt_loops", "1" }, { "skirt_height", "1" }, { "draft_shield", "disabled" } });
    CHECK(print.objects().front()->layers().back()->curved_tops_block);
    CHECK(curved_tops_unavailable_warning(*print.objects().front()).empty());
    CHECK_NOTHROW(Test::gcode(print));
}

TEST_CASE("Curved tops: the Centauri Carbon and the X1 Carbon still curve, with a summary and no refusal", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const std::string printer_model = GENERATE(std::string("Elegoo Centauri Carbon"), std::string("Bambu Lab X1 Carbon"));
    INFO("printer_model = " << printer_model);
    Print print;
    Model model;
    slice_puck(print, model, true, { { "printer_model", printer_model } });
    const PrintObject *object = print.objects().front();
    size_t curved = 0;
    for (const LayerRegion *region : object->layers().back()->regions())
        curved += count_curved_paths(&region->fills);
    CHECK(curved >= 6);
    CHECK(object->layers().back()->curved_tops_block);
    CHECK(curved_tops_unavailable_warning(*object).empty());
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("Curved tops on") != std::string::npos);
    // 2.5 mm lowest head part less 0.5 mm margin: 2.00 mm allowed; the 1.2 mm cap is within what
    // has been printed (1.8 mm, 10.3 deg), so no "beyond" note.
    CHECK(summary.find("of 2.00 mm allowed") != std::string::npos);
    CHECK(summary.find("goes beyond what has been printed") == std::string::npos);
    // The head clearance check runs on this curved print and finds no contact (else this throws).
    CHECK_NOTHROW(Test::gcode(print));
}

// R1 with numbers: a cap taller than the head allows stays flat, and the warning says by how much.
TEST_CASE("Curved tops: a surface rising more than the head allows is refused with the numbers", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    Print print;
    Model model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "printer_model",              "Elegoo Centauri Carbon" },
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "enable_support",             "0" },
        { "curved_tops_enabled",        "1" },
        { "curved_tops_max_angle",      "15" },
    });
    // r = 30, cap 2.6 mm: edge slope about 9.9 deg, rise above the 2.0 mm allowed.
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(30., 4., 2.6) }, print, model, config);
    print.process();
    CHECK_FALSE(print.objects().front()->layers().back()->curved_tops_block);
    const std::string warning = curved_tops_unavailable_warning(*print.objects().front());
    INFO("warning: " << warning);
    CHECK(warning.find("more than the 2.00 mm the print head allows") != std::string::npos);
}

TEST_CASE("Curved tops: a gentle cap is printed as conformal layers", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    Print print;
    Model model;
    slice_puck(print, model, true);
    const PrintObject *object = print.objects().front();

    size_t curved_top = 0, curved_elsewhere = 0;
    for (size_t i = 0; i < object->layers().size(); ++i)
        for (const LayerRegion *region : object->layers()[i]->regions())
            (i + 1 == object->layers().size() ? curved_top : curved_elsewhere) += count_curved_paths(&region->fills);
    CHECK(curved_elsewhere == 0);
    // Three layers, each at least a wall loop and some fill lines.
    CHECK(curved_top >= 6);

    // The cap rises 1.2 mm: each curved layer starts back down at the rim, below the apex of the
    // one before it, so the extrusion Z comes back down by about the cap's rise.
    const double dip = extrusion_dip(Test::gcode(print));
    CHECK(dip < -0.8);
    CHECK(dip > -(1.2 + 3 * 0.2 + 0.3));
}

// Hand test 2026-11-15: after a wipe, the lift to the safe Z was merged into the XY travel as one
// diagonal that climbed from the rim through the dome's freshly printed lines (0.45 mm below the
// surface 7.8 mm into a 37.5 mm travel). Every travel in the curved block must run level at the
// safe Z, clear of everything printed, for every z-hop type and with wipe on or off.
TEST_CASE("Curved tops: travels in the curved block clear the printed surface", "[CurvedTops]")
{
    const std::string hop  = GENERATE(std::string("Normal Lift"), std::string("Slope Lift"), std::string("Spiral Lift"), std::string("Auto Lift"));
    const std::string wipe = GENERATE(std::string("0"), std::string("1"));
    INFO("z_hop_types = " << hop << ", wipe = " << wipe);
    const ResourcesDirGuard resources;

    Print print;
    Model model;
    slice_puck(print, model, true, {
        { "z_hop_types",              hop },
        { "z_hop",                    "0.4" },
        { "wipe",                     wipe },
        { "wipe_distance",            "2" },
        { "retraction_length",        "0.8" },
        { "enable_arc_fitting",       "0" },
        { "use_relative_e_distances", "1" },
    });
    const CurvedTravels t = check_curved_travels(Test::gcode(print), 0.2, 0.5);
    INFO("travels " << t.travels << ", slanted " << t.slanted << ", under margin " << t.under_margin << ", arcs " << t.arcs
                    << ", min clearance " << t.min_clearance << " mm");
    // Six curved paths (wall + fill on three layers): at least five travels between them.
    CHECK(t.travels >= 5);
    CHECK(t.slanted == 0);
    CHECK(t.arcs == 0);
    CHECK(t.under_margin == 0);
    CHECK(t.min_clearance >= 0.5);
}

namespace {

PresetBundle &bbl_bundle_for_curved_tops()
{
    static std::unique_ptr<PresetBundle> bundle;
    if (bundle)
        return *bundle;
    const std::string             saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("curvedtops_bbl_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    auto              library  = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

} // namespace

// The stock X1C process (spiral z-hop, arc fitting, wipe, Bambu layer-change G-code) with curved tops
// on: the X1C's measured head clearance lets a 1.2 mm cap through, the travels in the curved block
// are level and clear, no arc or spiral lift in the block, and the layer bookkeeping the printer and
// the viewer read is intact (one "; CHANGE_LAYER" / "; Z_HEIGHT" per layer, progress lines).
TEST_CASE("Curved tops: the stock X1C process keeps its travels clear and its G-code intact", "[CurvedTops][X1C]")
{
    PresetBundle &b = bbl_bundle_for_curved_tops();
    const ResourcesDirGuard resources;
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab X1 Carbon 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL X1C", true));
    REQUIRE(b.filaments.select_preset_by_name("Bambu PLA Basic @BBL X1C", true));
    b.filament_presets = { "Bambu PLA Basic @BBL X1C" };
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "curved_tops_enabled",   "1" },
        { "curved_tops_layers",    "3" },
        { "curved_tops_max_angle", "15" },
        { "gcode_comments",        "0" },
    });

    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "dome";
    object->add_volume(make_capped_puck(15., 4., 1.2))->name = "dome";
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(128., 128., 0.));
    object->ensure_on_bed();
    print.is_BBL_printer() = true;
    print.apply(model, cfg);
    print.validate();
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);

    size_t curved = 0;
    for (const LayerRegion *region : print.objects().front()->layers().back()->regions())
        curved += count_curved_paths(&region->fills);
    REQUIRE(curved >= 6);

    const CurvedTravels t = check_curved_travels(gcode, 0.2, 0.5);
    INFO("travels " << t.travels << ", slanted " << t.slanted << ", under margin " << t.under_margin << ", arcs " << t.arcs
                    << ", min clearance " << t.min_clearance << " mm, max travel Z " << t.max_travel_z << ", max extrusion F " << t.max_extrude_f);
    CHECK(t.travels >= 5);
    CHECK(t.slanted == 0);
    CHECK(t.arcs == 0);
    CHECK(t.under_margin == 0);
    CHECK(t.min_clearance >= 0.5);
    CHECK(t.max_travel_z < cfg.opt_float("printable_height"));
    // Curved paths at 40 mm/s or less, overhang walls included (the X1C's walls ran at 50).
    CHECK(t.max_extrude_f <= CURVED_TOPS_MAX_SPEED_MM_S * 60. + 1e-6);

    // One layer change and one Z_HEIGHT per printed layer (the planar layers the curved block
    // emptied are not printed), the header's total says the same, progress lines still there.
    size_t layers = 0;
    for (const Layer *layer : print.objects().front()->layers())
        layers += layer->has_extrusions() ? 1 : 0;
    REQUIRE(layers < print.objects().front()->layers().size()); // the block emptied some planar layers
    size_t change_layer = 0, z_height = 0, m73 = 0, header_total = 0;
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);) {
        change_layer += line.rfind("; CHANGE_LAYER", 0) == 0 || line.rfind(";LAYER_CHANGE", 0) == 0;
        z_height     += line.rfind("; Z_HEIGHT:", 0) == 0;
        m73          += line.rfind("M73 ", 0) == 0;
        if (line.rfind("; total layer number: ", 0) == 0)
            header_total = size_t(std::atoi(line.c_str() + 22));
    }
    CHECK(change_layer == layers);
    CHECK(z_height == layers);
    CHECK(header_total == layers);
    CHECK(m73 > 0);
}

namespace {

// What happens around the curved block, read back from the G-code. The block starts at the first
// extrusion that moves Z along the move or goes below the highest Z extruded so far.
struct BlockMoves
{
    bool   curved          = false;
    double highest         = -1e9; // highest extruded Z
    // After the last extrusion, before the head next moves in X/Y (wipes aside): the highest Z reached.
    double exit_lift_z     = -1e9;
    bool   xy_after_last   = false; // some X/Y move (not a wipe) follows the last extrusion
    size_t block_wipes     = 0;     // wipe moves from the first curved extrusion on
    double max_block_wipe_f = 0.;   // their highest feedrate, mm/min
};

BlockMoves analyse_block(const std::string &gcode)
{
    struct Move { bool extrude, wipe, xy; double z0, z1, f; };
    std::vector<Move> moves;
    {
        std::istringstream in(gcode);
        std::string        line;
        double x = 0., y = 0., z = 0., f = 0.;
        bool   in_wipe = false;
        while (std::getline(in, line)) {
            if (line.rfind(";WIPE_START", 0) == 0)
                in_wipe = true;
            else if (line.rfind(";WIPE_END", 0) == 0)
                in_wipe = false;
            const std::string code = line.substr(0, line.find(';'));
            if (code.rfind("G1 ", 0) != 0 && code.rfind("G0 ", 0) != 0 && code.rfind("G2 ", 0) != 0 && code.rfind("G3 ", 0) != 0)
                continue;
            auto word = [&code](char c, double &v) {
                const auto pos = code.find(std::string(" ") + c);
                if (pos == std::string::npos)
                    return false;
                v = std::atof(code.c_str() + pos + 2);
                return true;
            };
            double nx = x, ny = y, nz = z, e = 0.;
            word('X', nx);
            word('Y', ny);
            word('Z', nz);
            word('E', e);
            word('F', f);
            const bool xy = nx != x || ny != y;
            moves.push_back({ e > 0. && xy && !in_wipe, xy && (in_wipe || e < 0.), xy, z, nz, f });
            x = nx; y = ny; z = nz;
        }
    }
    BlockMoves out;
    size_t first_curved = moves.size(), last_ext = moves.size();
    for (size_t i = 0; i < moves.size(); ++i) {
        if (!moves[i].extrude)
            continue;
        if (first_curved == moves.size() && (std::abs(moves[i].z1 - moves[i].z0) > 1e-6 || moves[i].z1 < out.highest - 1e-3))
            first_curved = i;
        out.highest = std::max(out.highest, moves[i].z1);
        last_ext    = i;
    }
    out.curved = first_curved < moves.size();
    if (!out.curved)
        return out;
    for (size_t i = first_curved; i < moves.size(); ++i)
        if (moves[i].wipe) {
            ++out.block_wipes;
            out.max_block_wipe_f = std::max(out.max_block_wipe_f, moves[i].f);
        }
    for (size_t i = last_ext + 1; i < moves.size(); ++i) {
        if (moves[i].wipe)
            continue;
        if (moves[i].xy) {
            out.xy_after_last = true;
            break;
        }
        out.exit_lift_z = std::max(out.exit_lift_z, moves[i].z1);
    }
    return out;
}

} // namespace

// R5: after the block's last path the head lifts straight up to the safe Z (1 mm over the highest
// curved point) before it moves anywhere, here before an end G-code that moves X/Y first, for every
// z-hop type.
TEST_CASE("Curved tops: the block ends with a lift to the safe Z", "[CurvedTops]")
{
    const std::string hop = GENERATE(std::string("Normal Lift"), std::string("Slope Lift"), std::string("Spiral Lift"), std::string("Auto Lift"));
    INFO("z_hop_types = " << hop);
    const ResourcesDirGuard resources;
    Print print;
    Model model;
    slice_puck(print, model, true, {
        { "z_hop_types",        hop },
        { "z_hop",              "0.4" },
        { "wipe",               "1" },
        { "wipe_distance",      "2" },
        { "retraction_length",  "0.8" },
        { "enable_arc_fitting", "0" },
        { "machine_end_gcode",  "G1 X5 Y5 F6000" },
    });
    REQUIRE(print.objects().front()->layers().back()->curved_tops_block);
    const BlockMoves b = analyse_block(Test::gcode(print));
    INFO("highest extruded Z " << b.highest << ", exit lift Z " << b.exit_lift_z);
    REQUIRE(b.curved);
    CHECK(b.xy_after_last);
    CHECK(b.exit_lift_z >= b.highest + 1.0 - 1e-3);

    // With the feature off nothing of the kind is added: the end G-code's XY move comes before any
    // Z move that high (the z-hop is 0.4 mm).
    Print print_off;
    Model model_off;
    slice_puck(print_off, model_off, false, {
        { "z_hop_types",        hop },
        { "z_hop",              "0.4" },
        { "machine_end_gcode",  "G1 X5 Y5 F6000" },
    });
    CHECK_FALSE(analyse_block(Test::gcode(print_off)).curved);
}

// Design 3.2, "Wipes": a wipe back along a curved line moves Z with it and keeps to the curved speed
// cap, whatever the wipe speed says.
TEST_CASE("Curved tops: wipes in the curved block keep to the curved speed cap", "[CurvedTops]")
{
    const std::string role_based = GENERATE(std::string("0"), std::string("1"));
    INFO("role_based_wipe_speed = " << role_based);
    const ResourcesDirGuard resources;
    Print print;
    Model model;
    slice_puck(print, model, true, {
        { "wipe",                  "1" },
        { "wipe_distance",         "2" },
        { "wipe_speed",            "150" },
        { "role_based_wipe_speed", role_based },
        { "retraction_length",     "0.8" },
        { "enable_arc_fitting",    "0" },
    });
    const BlockMoves b = analyse_block(Test::gcode(print));
    INFO("wipes in the block " << b.block_wipes << ", fastest F " << b.max_block_wipe_f);
    REQUIRE(b.curved);
    CHECK(b.block_wipes >= 5);
    CHECK(b.max_block_wipe_f > 0.);
    CHECK(b.max_block_wipe_f <= CURVED_TOPS_MAX_SPEED_MM_S * 60. + 1e-6);
}

// D2: the head clearance check runs on a curved-only print (no Z contouring) and a contact stops the
// export. The contact is constructed the way the design's test names it, "a deliberately wrong
// head": the planner plans with the shipped Centauri Carbon head (sock 2.5 mm up, 2.0 mm rise
// allowed), and the export then checks against a head whose sock is only 0.5 mm above the tip, so the
// 1.2 mm cap reaches it while the rim is printed. A Z contouring only print checked against the same
// wrong head keeps the warning and exports.
TEST_CASE("Curved tops: a head contact on a curved print stops the export, a Z contouring print only warns", "[CurvedTops]")
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("curvedtops_head_%%%%-%%%%");
    boost::filesystem::create_directories(dir / "head_clearance");
    {
        std::ofstream f((dir / "head_clearance" / "low-sock.json").string());
        f << R"({
    "schema": "edgeslicer.head_clearance.v1",
    "id": "low-sock-test",
    "name": "Wrong test head",
    "printer_models": ["Elegoo Centauri Carbon"],
    "calibrated": false,
    "source": "unit test",
    "nozzle": { "profile": [[0.0, 0.5], [1.0, 1.3], [2.0, 2.1], [4.0, 3.5]] },
    "boxes": [ {"name": "low sock", "min": [-12, -12, 0.5], "max": [12, 12, 24]} ],
    "curved_tops": { "verified_printer_models": ["Elegoo Centauri Carbon"], "verified_rise_mm": 1.8, "verified_angle_deg": 10.3 }
})";
    }
    {
        const ResourcesDirGuard shipped;
        Print print;
        Model model;
        slice_puck(print, model, true);
        REQUIRE(print.objects().front()->layers().back()->curved_tops_block);

        Print print_zaa;
        Model model_zaa;
        slice_puck(print_zaa, model_zaa, false, { { "zaa_enabled", "1" } });

        const ResourcesDirGuard wrong(dir.string());
        REQUIRE(resolve_head_clearance("Elegoo Centauri Carbon") != nullptr);
        REQUIRE(resolve_head_clearance("Elegoo Centauri Carbon")->id == "low-sock-test");
        std::string error;
        try {
            Test::gcode(print);
        } catch (const Slic3r::SlicingError &ex) {
            error = ex.what();
        }
        INFO("error: " << error);
        CHECK_FALSE(error.empty());
        CHECK(error.find("Curved tops:") != std::string::npos);
        CHECK(error.find("low sock") != std::string::npos);
        CHECK(error.find("The G-code was not exported") != std::string::npos);

        CHECK_NOTHROW(Test::gcode(print_zaa));
    }
    boost::system::error_code ec;
    boost::filesystem::remove_all(dir, ec);
}

// The planner's two numbers come from the resolved head model and nothing else (design 2.1).
TEST_CASE("Curved tops: head limits come from the head model only", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    auto limits = [](const std::string &model) {
        PrintConfig cfg;
        cfg.printer_model.value = model;
        return curved_tops_head_limits(cfg);
    };
    const CurvedTopsHeadLimits cc1     = limits("Elegoo Centauri Carbon");
    const CurvedTopsHeadLimits x1c     = limits("Bambu Lab X1 Carbon");
    const CurvedTopsHeadLimits x1e     = limits("Bambu Lab X1E");
    const CurvedTopsHeadLimits u1      = limits("Snapmaker U1");
    const CurvedTopsHeadLimits h2d     = limits("Bambu Lab H2D");
    const CurvedTopsHeadLimits cc2     = limits("Elegoo Centauri Carbon 2");
    const CurvedTopsHeadLimits generic = limits("");

    // Usable: a head model exists, and its file records a curved print on this printer model.
    CHECK(cc1.known);
    CHECK(cc1.usable);
    CHECK(x1c.known);
    CHECK(x1c.usable);
    CHECK(x1e.known);
    CHECK_FALSE(x1e.usable);
    CHECK(u1.known);
    CHECK_FALSE(u1.usable);
    CHECK(h2d.known);
    CHECK_FALSE(h2d.usable);
    CHECK_FALSE(cc2.known);
    CHECK_FALSE(cc2.usable);
    CHECK_FALSE(generic.known);
    CHECK_FALSE(generic.usable);
    CHECK_FALSE(generic.refusal.empty());

    // X1 Carbon: sock 2.5 mm above the tip (owner measured) less the 0.5 mm margin. The CC1 file's
    // sock is set to the same 2.5 mm (a guess). Both flanks clear well over 25 degrees: the 20 degree
    // ceiling #1021 shipped binds (the lead kept it for P1).
    CHECK_THAT(x1c.allowed_rise_mm, WithinAbs(2.0, 1e-9));
    CHECK_THAT(cc1.allowed_rise_mm, WithinAbs(2.0, 1e-9));
    CHECK_THAT(x1e.allowed_rise_mm, WithinAbs(2.0, 1e-9));
    CHECK_THAT(x1c.max_slope_deg, WithinAbs(20.0, 1e-9));
    CHECK_THAT(cc1.max_slope_deg, WithinAbs(20.0, 1e-9));
    CHECK_THAT(x1c.verified_rise_mm, WithinAbs(1.8, 1e-9));
    CHECK_THAT(x1c.verified_angle_deg, WithinAbs(10.3, 1e-9));
    CHECK_THAT(x1e.verified_rise_mm, WithinAbs(0.0, 1e-9));

    // The default 15 stays 15, nothing goes above 20 (as #1021).
    CHECK_THAT(curved_tops_effective_max_angle(15., cc1), WithinAbs(15., 1e-9));
    CHECK_THAT(curved_tops_effective_max_angle(35., cc1), WithinAbs(20., 1e-9));
    CHECK_THAT(curved_tops_effective_max_angle(40., x1c), WithinAbs(20., 1e-9));

    CHECK_THAT(curved_tops_gap_flow(0.2, 0.2), WithinAbs(1.0, 1e-9));
    CHECK_THAT(curved_tops_gap_flow(0.3, 0.2), WithinAbs(1.5, 1e-9));
    CHECK_THAT(curved_tops_gap_flow(0.1, 0.2), WithinAbs(1.0, 1e-9));
    CHECK_THAT(curved_tops_gap_flow(0.9, 0.2), WithinAbs(2.0, 1e-9));
}

namespace Slic3r {
// Test access (friend of PrintObject): leave posContouring unfinished over cut walls, as a cancel
// during the step does.
struct CurvedTopsTestAccess
{
    static void invalidate(PrintObject &object, PrintObjectStep step) { object.invalidate_step(step); }
};
} // namespace Slic3r

namespace {

ConfigItems joined(ConfigItems a, const ConfigItems &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

bool top_block(const Print &print)
{
    return print.objects().front()->layers().back()->curved_tops_block;
}

} // namespace

// Review finding a: most guard inputs do not invalidate posContouring by themselves. A guard input
// changed after a curved slice must refuse on the re-slice (warning, output = feature-off output, no
// cut-down top), and undoing it must curve again (output = the first slice).
TEST_CASE("Curved tops: a guard input changed after a curved slice refuses on re-slice, and curves again when undone", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    struct Change { const char *name; ConfigItems base; ConfigItems blocker; int filaments; const char *reason; };
    static const std::vector<Change> changes = {
        { "draft shield",     {}, { { "skirt_loops", "1" }, { "draft_shield", "enabled" } }, 1, "draft shield" },
        { "tall skirt",       { { "skirt_loops", "1" } }, { { "skirt_height", "3" } }, 1, "skirt taller than one layer" },
        { "supports",         {}, { { "enable_support", "1" } }, 1, "supports" },
        { "stabilizers",      {}, { { "stabilizer_supports", "auto" } }, 1, "side stabilizers" },
        { "by object",        {}, { { "print_sequence", "by object" } }, 1, "prints by object" },
        { "local-Z",          {}, { { "dithering_local_z_mode", "1" } }, 1, "local-Z dithering" },
        { "second filament",  {}, { { "sparse_infill_filament", "2" }, { "solid_infill_filament", "2" } }, 2, "more than one filament" },
        { "printable height", {}, { { "printable_height", "5.5" } }, 1, "above the printable height" },
        { "Z contouring",     {}, { { "zaa_enabled", "1" } }, 1, "Z contouring is on for it as well" },
        // raft_layers is left out: it re-slices the object by itself (posSlice), and the fresh-slice guard
        // test covers its refusal. Turning a raft on in an already sliced Print leaves one 0.00001 E split
        // segment in the raft's first loop that a fresh slice does not have - with curved tops off as well
        // (see "raft re-slice control" below: off-reslice vs fresh differs at the same line, and a full
        // invalidation makes both identical again). Pre-existing re-slice history in the raft code.
    };
    const size_t i = GENERATE(range(size_t(0), size_t(9)));
    REQUIRE(i < changes.size());
    const Change &c = changes[i];
    INFO("change: " << c.name);

    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, print, model, puck_config(true, c.base, c.filaments));
    const std::string gcode_curved = Test::gcode(print);
    REQUIRE(top_block(print));

    // Blocker on, curved tops still on.
    const ConfigItems blocked = joined(c.base, c.blocker);
    print.apply(model, puck_config(true, blocked, c.filaments));
    const std::string gcode_blocked = Test::gcode(print);
    CHECK_FALSE(top_block(print));
    CHECK(count_all_curved_paths(*print.objects().front()) == 0);
    const std::string warning = curved_tops_unavailable_warning(*print.objects().front());
    INFO("warning: " << warning);
    CHECK(warning.find(c.reason) != std::string::npos);
    if (!uses_zaa(blocked))
        CHECK_THAT(extrusion_dip(gcode_blocked), WithinAbs(0., 1e-6));

    // Blocker off again: curves again, exactly as the first slice.
    print.apply(model, puck_config(true, c.base, c.filaments));
    const std::string gcode_again = Test::gcode(print);
    CHECK(top_block(print));
    CHECK(curved_tops_unavailable_warning(*print.objects().front()).empty());
    CHECK(gcode_without_curved_tops_key(gcode_again) == gcode_without_curved_tops_key(gcode_curved));

    // The refused re-slice equals the feature-off slice of the same settings: nothing was left cut.
    print.apply(model, puck_config(false, blocked, c.filaments));
    const std::string gcode_off = Test::gcode(print);
    CHECK(gcode_without_curved_tops_key(gcode_blocked) == gcode_without_curved_tops_key(gcode_off));
}

// Review finding a, the plate side: a second copy or a second object added after a curved slice.
TEST_CASE("Curved tops: adding a copy or an object after a curved slice refuses, removing it curves again", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const bool add_object = GENERATE(false, true);
    INFO((add_object ? "second object" : "second copy"));
    const DynamicPrintConfig on  = puck_config(true, {});
    const DynamicPrintConfig off = puck_config(false, {});

    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, print, model, on);
    const std::string gcode_curved = Test::gcode(print);
    REQUIRE(top_block(print));

    const Vec3d offset = model.objects.front()->instances.front()->get_offset() + Vec3d(45., 0., 0.);
    if (add_object) {
        ModelObject *other = model.add_object(*model.objects.front());
        other->instances.front()->set_offset(offset);
    } else
        model.objects.front()->add_instance()->set_offset(offset);
    print.apply(model, on);
    const std::string gcode_blocked = Test::gcode(print);
    for (const PrintObject *object : print.objects())
        CHECK_FALSE(object->layers().back()->curved_tops_block);
    const std::string warning = curved_tops_unavailable_warning(*print.objects().front());
    INFO("warning: " << warning);
    CHECK(warning.find("more than one object or copy") != std::string::npos);
    CHECK_THAT(extrusion_dip(gcode_blocked), WithinAbs(0., 1e-6));
    print.apply(model, off);
    CHECK(gcode_without_curved_tops_key(gcode_blocked) == gcode_without_curved_tops_key(Test::gcode(print)));

    if (add_object)
        model.delete_object(size_t(1));
    else
        model.objects.front()->delete_instance(1);
    print.apply(model, on);
    const std::string gcode_again = Test::gcode(print);
    CHECK(top_block(print));
    CHECK(gcode_without_curved_tops_key(gcode_again) == gcode_without_curved_tops_key(gcode_curved));
}

// Review r2: curved tops is a process setting, so on a multi-part plate every part has it on and is
// refused. Adding a third part, or a third copy, must not regenerate the walls or fills of any part
// (they all stay refused and planar) - and with curved tops off everywhere nothing differs from main.
TEST_CASE("Curved tops: adding a part or a copy to a refused plate re-slices nothing else", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const bool curved     = GENERATE(true, false);
    const bool add_object = GENERATE(true, false);
    INFO("curved tops " << (curved ? "on" : "off") << ", adding " << (add_object ? "a third object" : "a third copy"));
    const DynamicPrintConfig config = puck_config(curved, {});

    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2), make_capped_puck(15., 4., 1.2) }, print, model, config);
    // Two copies of the first object to start with, so the copy case goes from 2 to 3.
    const Vec3d elsewhere = model.objects.front()->instances.front()->get_offset() + Vec3d(0., 45., 0.);
    if (!add_object) {
        model.objects.front()->add_instance()->set_offset(elsewhere);
        print.apply(model, config);
    }
    Test::gcode(print);
    REQUIRE(print.objects().size() == 2);
    for (const PrintObject *object : print.objects()) {
        CHECK_FALSE(object->layers().back()->curved_tops_block);
        if (curved)
            CHECK_FALSE(curved_tops_unavailable_warning(*object).empty());
    }

    if (add_object) {
        ModelObject *third = model.add_object(*model.objects.back());
        third->instances.front()->set_offset(elsewhere);
    } else
        model.objects.front()->add_instance()->set_offset(elsewhere + Vec3d(45., 0., 0.));
    print.apply(model, config);
    // The parts that were there keep their walls and fills.
    for (size_t i = 0; i < 2; ++i) {
        INFO("object " << i);
        const PrintObject *object = print.get_object(i);
        CHECK(object->is_step_done(posPerimeters));
        CHECK(object->is_step_done(posInfill));
    }
    CHECK_NOTHROW(Test::gcode(print));
    for (const PrintObject *object : print.objects())
        CHECK_FALSE(object->layers().back()->curved_tops_block);
}

// Control for the raft case left out of the re-slice test: the same two re-slices with curved tops
// off, against a fresh slice. Records what it finds (stdout and a Catch warning) and asserts nothing:
// it tells whether the one 0.00001 E split segment in the raft's first loop is re-slice history in the
// raft code, with or without curved tops.
TEST_CASE("Curved tops: raft re-slice control (records only)", "[CurvedTops][RaftControl]")
{
    const ResourcesDirGuard resources;
    const ConfigItems raft = { { "raft_layers", "2" } };
    const auto        g    = [](Print &print) { return gcode_without_curved_tops_key(Test::gcode(print)); };
    // "same", or the first differing line of each side.
    const auto        count_diff = [](const std::string &a, const std::string &b) -> std::string {
        if (a == b)
            return "same";
        std::istringstream ia(a), ib(b);
        std::string        la, lb;
        for (size_t n = 1;; ++n) {
            const bool ok_a = bool(std::getline(ia, la));
            const bool ok_b = bool(std::getline(ib, lb));
            if (!ok_a && !ok_b)
                return "differs (only in line ending)";
            if (!ok_a || !ok_b || la != lb)
                return "differs from line " + std::to_string(n) + ": [" + (ok_a ? la : std::string("<end>")) + "] vs [" +
                       (ok_b ? lb : std::string("<end>")) + "]";
        }
    };

    // Fresh feature-off raft slice.
    Print fresh;
    Model fresh_model;
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, fresh, fresh_model, puck_config(false, raft));
    const std::string fresh_off = g(fresh);

    // Feature off both times: plain slice, then raft on.
    Print off;
    Model off_model;
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, off, off_model, puck_config(false, {}));
    g(off);
    off.apply(off_model, puck_config(false, raft));
    const std::string reslice_off = g(off);
    // Feature off both times, second step through the same "change a key that invalidates everything"
    // path the curved run takes when curved tops is turned off: slice with raft, toggle curved tops on
    // and off again with raft on.
    off.apply(off_model, puck_config(true, raft));
    g(off);
    off.apply(off_model, puck_config(false, raft));
    const std::string reslice_off_toggled = g(off);

    // The pair the re-slice test compared: curved slice, raft on (refused), then curved tops off.
    Print on;
    Model on_model;
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, on, on_model, puck_config(true, {}));
    g(on);
    on.apply(on_model, puck_config(true, raft));
    const std::string reslice_on = g(on);
    on.apply(on_model, puck_config(false, raft));
    const std::string reslice_on_then_off = g(on);

    std::ostringstream report;
    report << "RAFT_CONTROL off-reslice vs fresh: " << count_diff(reslice_off, fresh_off)
           << "; off-reslice-toggled vs fresh: " << count_diff(reslice_off_toggled, fresh_off)
           << "; on-reslice (refused) vs fresh: " << count_diff(reslice_on, fresh_off)
           << "; on-reslice-then-off vs fresh: " << count_diff(reslice_on_then_off, fresh_off)
           << "; on-reslice vs on-reslice-then-off: " << count_diff(reslice_on, reslice_on_then_off);
    std::cout << report.str() << std::endl;
    WARN(report.str());
    SUCCEED();
}

// Review finding g: posContouring left unfinished over walls a curved run already cut (a cancel during
// the step). The next apply regenerates the walls whatever changed, so the step never re-plans on cut
// layers; with a blocker added at that moment the re-slice is the feature-off slice. And an apply
// that changes nothing a guard reads does not regenerate anything.
TEST_CASE("Curved tops: an unfinished curved step over cut walls is redone from fresh walls", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const bool add_blocker = GENERATE(false, true);
    INFO((add_blocker ? "with a draft shield added" : "nothing changed"));
    const ConfigItems blocker = { { "skirt_loops", "1" }, { "draft_shield", "enabled" } };

    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, print, model, puck_config(true, {}));
    const std::string gcode_curved = Test::gcode(print);
    PrintObject *object = print.get_object(0);
    REQUIRE(object->layers().back()->curved_tops_block);
    REQUIRE(object->curved_tops_clipped());

    // An apply that changes nothing keeps every step.
    print.apply(model, puck_config(true, {}));
    CHECK(object->is_step_done(posPerimeters));
    CHECK(object->is_step_done(posContouring));

    CurvedTopsTestAccess::invalidate(*object, posContouring);
    REQUIRE_FALSE(object->is_step_done(posContouring));
    REQUIRE(object->is_step_done(posPerimeters));
    print.apply(model, puck_config(true, add_blocker ? blocker : ConfigItems{}));
    object = print.get_object(0);
    CHECK_FALSE(object->is_step_done(posPerimeters));

    const std::string gcode = Test::gcode(print);
    if (add_blocker) {
        CHECK_FALSE(top_block(print));
        print.apply(model, puck_config(false, blocker));
        CHECK(gcode_without_curved_tops_key(gcode) == gcode_without_curved_tops_key(Test::gcode(print)));
    } else {
        CHECK(top_block(print));
        CHECK(gcode_without_curved_tops_key(gcode) == gcode_without_curved_tops_key(gcode_curved));
    }
}

// Review finding e: R1 across surfaces. Two caps on one object, each rising 1 mm (under the 2.0 mm the
// head allows) but 2 mm apart in height: the block spans 3 mm and is refused, with the numbers.
TEST_CASE("Curved tops: surfaces at different heights are limited by the block's whole height span", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    TriangleMesh low  = make_capped_puck(8., 4., 1.0);
    TriangleMesh high = make_capped_puck(8., 6., 1.0);
    low.translate(-12.f, 0.f, 0.f);
    high.translate(12.f, 0.f, 0.f);
    low.merge(high);

    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ low }, print, model, puck_config(true, {}));
    print.process();
    CHECK_FALSE(top_block(print));
    const std::string warning = curved_tops_unavailable_warning(*print.objects().front());
    INFO("warning: " << warning);
    CHECK(warning.find("2 curved surfaces together span 3.00 mm") != std::string::npos);
    CHECK(warning.find("more than the 2.00 mm the print head allows") != std::string::npos);
}

// Review finding d: a cap steeper than the 10.3 degrees the owner printed (about 11 at the rim). The
// block is built, the summary says it is beyond what has been printed, and the head check - which
// relaxes nothing for wipes and travels - lets it export on both heads.
TEST_CASE("Curved tops: an 11 degree cap exports on both printers, with the beyond-printed note", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const std::string printer_model = GENERATE(std::string("Elegoo Centauri Carbon"), std::string("Bambu Lab X1 Carbon"));
    INFO("printer_model = " << printer_model);
    const ConfigItems extra = { { "printer_model", printer_model }, { "wipe", "1" }, { "wipe_distance", "2" },
                                { "retraction_length", "0.8" }, { "enable_arc_fitting", "0" } };
    Print print;
    Model model;
    // r = 15, cap 1.45 mm: sphere radius 78.3, rim slope asin(15 / 78.3) = 11.0 degrees.
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.45) }, print, model, puck_config(true, extra));
    print.process();
    REQUIRE(top_block(print));
    const PrintObject *object = print.objects().front();
    CHECK(object->layers().back()->curved_tops_slope_deg > 10.5f);
    CHECK(object->layers().back()->curved_tops_slope_deg < 11.5f);
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("goes beyond what has been printed") != std::string::npos);
    std::string error;
    try {
        Test::gcode(print);
    } catch (const Slic3r::SlicingError &ex) {
        error = ex.what();
    }
    INFO("error: " << error);
    CHECK(error.empty());
}
