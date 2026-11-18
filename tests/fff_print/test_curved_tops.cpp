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
//  * The pure helpers: head limits, effective angle, first-layer gap flow, the head's reach.
//  * P2 (part of it): supports no longer refuse by themselves, only when they reach into the curved
//    layers within the head's reach; a surface taller than the head allows is curved over its upper
//    part; a max angle above the cap is said.
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
#include <limits>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
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
#include "libslic3r/SLA/IndexedMesh.hpp"
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
        // Supports and side stabilizers no longer refuse by themselves (P2): see the support cases below.
        { "two filaments",       { { "sparse_infill_filament", "2" }, { "solid_infill_filament", "2" } }, 2, "more than one filament" },
        { "raft",                { { "raft_layers", "2" } }, 1, "raft" },
        { "Z contouring",        { { "zaa_enabled", "1" } }, 1, "Z contouring is on for it as well" },
        { "printable height",    { { "printable_height", "5.5" } }, 1, "above the printable height" },
    };
    const size_t i = GENERATE(range(size_t(0), size_t(8)));
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

// P2, partial curving (R1 still holds for the curved part): a cap rising 2.6 mm where the head allows
// 2.0 is curved over its top 2.0 mm, from the planar layer top at Z 4.6, and printed flat below it.
// Every curved bead lies at least one layer above that layer top (the flat part stands no higher than
// it), the lowest curved beads sit right at one layer above it (the boundary reads as a layer step), the
// slope the head check is told is the curved part's only, and the head check finds no contact.
TEST_CASE("Curved tops: a surface rising more than the head allows is curved over its upper part", "[CurvedTops]")
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
    // r = 30, cap 2.6 mm (Z 4.0 to 6.6): edge slope about 9.9 deg, rise above the 2.0 mm allowed.
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(30., 4., 2.6) }, print, model, config);
    print.process();
    const PrintObject *object = print.objects().front();
    REQUIRE(object->layers().back()->curved_tops_block);
    CHECK(curved_tops_unavailable_warning(*object).empty());
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("rise 2.00 mm of 2.00 mm allowed") != std::string::npos);
    CHECK(summary.find("Partly curved: 1 surface(s) rise up to 2.60 mm") != std::string::npos);
    CHECK(summary.find("The top 2.00 mm (from Z 4.60 mm to 6.60 mm) is curved; the part below Z 4.60 mm prints as flat layers") !=
          std::string::npos);

    // The cut is a planar layer top, and that layer still prints (the flat plateau under the block).
    const double cut = 4.6, h = 0.2;
    const Layer *cut_layer = nullptr;
    for (const Layer *layer : object->layers())
        if (std::abs(layer->print_z - cut) < 1e-6)
            cut_layer = layer;
    REQUIRE(cut_layer != nullptr);
    CHECK(cut_layer->has_extrusions());

    // Every curved sample at least one layer above the cut, the lowest ones right there.
    const Layer *top   = object->layers().back();
    double       min_z = 1e9, max_z = -1e9;
    std::function<void(const ExtrusionEntity *)> visit = [&](const ExtrusionEntity *e) {
        if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
            for (const ExtrusionEntity *child : c->entities)
                visit(child);
        } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(e)) {
            if (p->z_contour != nullptr && p->z_contour->curved_top)
                for (const float dz : p->z_contour->z) {
                    min_z = std::min(min_z, top->print_z + double(dz));
                    max_z = std::max(max_z, top->print_z + double(dz));
                }
        }
    };
    for (const LayerRegion *region : top->regions())
        visit(&region->fills);
    CHECK(min_z >= cut + h - 1e-4);
    CHECK(min_z < cut + h + 0.05);
    CHECK(max_z <= 6.6 + 1e-3);
    CHECK(max_z > 6.5);
    // The cap's outer facets slope about 9.7 degrees; the facets reaching above Z 4.6 (out to r = 27.5)
    // at most about 8.9: the head check is told the curved part's slope, not the flat part's.
    CHECK(top->curved_tops_slope_deg < 9.3f);
    CHECK(top->curved_tops_slope_deg > 8.0f);

    CHECK_NOTHROW(Test::gcode(print));
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
    // P4: the CC1's record of 2026-11-17, nozzle contact included.
    CHECK_THAT(cc1.verified_rise_mm, WithinAbs(2.37, 1e-9));
    CHECK_THAT(cc1.verified_angle_deg, WithinAbs(20.0, 1e-9));
    CHECK_THAT(cc1.verified_nozzle_contact_mm, WithinAbs(0.61, 1e-9));
    CHECK_THAT(x1c.verified_nozzle_contact_mm, WithinAbs(0.44, 1e-9));

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
        // Supports: see "Curved tops: turning supports on and off after a slice re-decides" (P2).
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
    const size_t i = GENERATE(range(size_t(0), size_t(7)));
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
// head allows) but 2 mm apart in height: the block would span 3 mm. P2: the block is cut at 2.0 mm
// below the highest point (Z 5.0), so the high cap is curved whole and the low one (up to Z 5.0) stays
// flat, and the summary says so.
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
    REQUIRE(top_block(print));
    const PrintObject *object = print.objects().front();
    CHECK(curved_tops_unavailable_warning(*object).empty());
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    // Review d: the span reported is what is curved, the high cap whole (Z 6.0 to 7.0), not down to the cut.
    CHECK(summary.find("1 surface(s), 3 curved layers; rise 1.00 mm of 2.00 mm allowed") != std::string::npos);
    CHECK(summary.find("1 lower surface(s) stay flat") != std::string::npos);
    // Every curved path is over the high cap (x > 0 in the object's frame), none over the low one.
    size_t over_low = 0, over_high = 0;
    std::function<void(const ExtrusionEntity *)> visit = [&](const ExtrusionEntity *e) {
        if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
            for (const ExtrusionEntity *child : c->entities)
                visit(child);
        } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(e)) {
            if (p->z_contour != nullptr && p->z_contour->curved_top)
                ++(unscale<double>(p->polyline.first_point().x()) < 0. ? over_low : over_high);
        }
    };
    for (const LayerRegion *region : object->layers().back()->regions())
        visit(&region->fills);
    CHECK(over_low == 0);
    CHECK(over_high >= 6);
    CHECK_NOTHROW(Test::gcode(print));
}

// Review finding d: a cap steeper than the 10.3 degrees the owner printed on the X1 Carbon (about 11 at the
// rim). The block is built, the summary says it is beyond what has been printed there, and the head check - which
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
    // P4: the CC1 record is now the owner's 2026-11-17 arm (2.37 mm, 20 degrees): within it; the X1 Carbon's
    // (1.8 mm, 10.3 degrees) is not.
    CHECK((summary.find("goes beyond what has been printed") != std::string::npos) == (printer_model == "Bambu Lab X1 Carbon"));
    std::string error;
    try {
        Test::gcode(print);
    } catch (const Slic3r::SlicingError &ex) {
        error = ex.what();
    }
    INFO("error: " << error);
    CHECK(error.empty());
}

// ---------------------------------------------------------------------------------------------
// P2: supports and side stabilizers, the head's reach, the angle notice
// ---------------------------------------------------------------------------------------------

namespace {

// A solid of revolution. profile: (r, z) from the bottom centre (r = 0) round the outside to the top
// centre (r = 0), counter-clockwise in the (r, z) half plane.
TriangleMesh make_revolved(const std::vector<std::pair<double, double>> &profile, int sectors = 96)
{
    indexed_triangle_set its;
    const size_t         m = profile.size();
    its.vertices.emplace_back(0.f, 0.f, float(profile.front().second));
    for (size_t i = 1; i + 1 < m; ++i)
        for (int j = 0; j < sectors; ++j) {
            const double a = 2.0 * M_PI * double(j) / double(sectors);
            its.vertices.emplace_back(float(profile[i].first * std::cos(a)), float(profile[i].first * std::sin(a)), float(profile[i].second));
        }
    const int top = int(its.vertices.size());
    its.vertices.emplace_back(0.f, 0.f, float(profile.back().second));
    auto ring = [sectors](size_t i, int j) { return int(1 + (i - 1) * size_t(sectors) + size_t(j % sectors)); };
    for (int j = 0; j < sectors; ++j)
        its.indices.emplace_back(0, ring(1, j + 1), ring(1, j));
    for (size_t i = 1; i + 2 < m; ++i)
        for (int j = 0; j < sectors; ++j) {
            its.indices.emplace_back(ring(i, j), ring(i, j + 1), ring(i + 1, j + 1));
            its.indices.emplace_back(ring(i, j), ring(i + 1, j + 1), ring(i + 1, j));
        }
    for (int j = 0; j < sectors; ++j)
        its.indices.emplace_back(ring(m - 2, j), ring(m - 2, j + 1), top);
    return TriangleMesh(std::move(its));
}

// A mushroom: stem r = 8 from Z 0 to 4, a disc r = 15 from Z 4 to 4.6 overhanging it all round, and a
// 1.2 mm spherical cap on the disc (Z 4.6 to 5.8, rim slope about 9 degrees). With supports on, the
// support under the disc's overhang tops out at Z 3.8 (0.2 top Z distance), right under the cap.
TriangleMesh make_mushroom()
{
    const double r = 15., base = 4.6, h_cap = 1.2;
    const double rs = (r * r + h_cap * h_cap) / (2.0 * h_cap);
    std::vector<std::pair<double, double>> profile = { { 0., 0. }, { 8., 0. }, { 8., 4. }, { r, 4. }, { r, base } };
    const int rings = 24;
    for (int i = 1; i < rings; ++i) {
        const double rr = r * double(rings - i) / double(rings);
        profile.emplace_back(rr, base + std::sqrt(rs * rs - rr * rr) - (rs - h_cap));
    }
    profile.emplace_back(0., base + h_cap);
    return make_revolved(profile);
}

ConfigItems mushroom_items(int curved_layers, bool supports)
{
    return { { "curved_tops_layers", std::to_string(curved_layers) }, { "enable_support", supports ? "1" : "0" },
             { "support_top_z_distance", "0.2" } };
}

void slice_mushroom(Print &print, Model &model, bool curved_tops, const ConfigItems &items)
{
    Test::init_print(std::vector<TriangleMesh>{ make_mushroom() }, print, model, puck_config(curved_tops, items));
    print.process();
}

// The highest support layer that carries anything.
double highest_support_z(const PrintObject &object)
{
    double z = 0.;
    for (const SupportLayer *layer : object.support_layers())
        if (!layer->support_islands.empty() || !layer->support_fills.entities.empty())
            z = std::max(z, layer->print_z);
    return z;
}

} // namespace

// Six curved layers put the band bottom at Z 3.4 (4.6 - 6 x 0.2); the support under the disc stands up
// to Z 3.8, right under the curved surface: the object prints planar, as with curved tops off, and the
// warning names the supports.
TEST_CASE("Curved tops: supports reaching into the curved layers near the surface refuse, with the reason", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const ConfigItems       items = mushroom_items(6, true);

    Print print_off;
    Model model_off;
    slice_mushroom(print_off, model_off, false, items);
    const std::string gcode_off = Test::gcode(print_off);

    Print print_on;
    Model model_on;
    slice_mushroom(print_on, model_on, true, items);
    const PrintObject *object = print_on.objects().front();
    REQUIRE(highest_support_z(*object) > 3.4 + 1e-3);
    CHECK_FALSE(object->layers().back()->curved_tops_block);
    CHECK(count_all_curved_paths(*object) == 0);
    const std::string warning = curved_tops_unavailable_warning(*object);
    INFO("warning: " << warning);
    CHECK(warning.find("its supports (or side stabilizers) reach Z ") != std::string::npos);
    CHECK(warning.find("into the curved layers (from Z 3.40 mm), within ") != std::string::npos);
    const std::string gcode_on = Test::gcode(print_on);
    CHECK_THAT(extrusion_dip(gcode_on), WithinAbs(0., 1e-6));
    CHECK(gcode_without_curved_tops_key(gcode_on) == gcode_without_curved_tops_key(gcode_off));
}

// Two curved layers put the band bottom at Z 4.2, above the support's top at 3.8: the supports stay,
// the top is curved, and the head check (which sees the support extrusions) finds no contact.
TEST_CASE("Curved tops: supports that stay below the curved layers keep the top curved", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    Print print;
    Model model;
    slice_mushroom(print, model, true, mushroom_items(2, true));
    const PrintObject *object = print.objects().front();
    const double       support_top = highest_support_z(*object);
    INFO("support top " << support_top);
    REQUIRE(support_top > 3.);
    CHECK(support_top < 4.2 - 1e-3);
    CHECK(object->layers().back()->curved_tops_block);
    CHECK(count_all_curved_paths(*object) >= 4);
    CHECK(curved_tops_unavailable_warning(*object).empty());
    CHECK_FALSE(object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary).empty());
    CHECK_NOTHROW(Test::gcode(print));
}

// The guard record and the support step re-decide after a change: curved without supports, planar
// with supports reaching into the band (walls regenerated, output = feature-off output), curved again
// with them off (output = the first slice).
TEST_CASE("Curved tops: turning supports on and off after a slice re-decides", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const ConfigItems       no_support = mushroom_items(6, false);
    const ConfigItems       support    = mushroom_items(6, true);

    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ make_mushroom() }, print, model, puck_config(true, no_support));
    const std::string gcode_curved = Test::gcode(print);
    REQUIRE(top_block(print));

    print.apply(model, puck_config(true, support));
    const std::string gcode_refused = Test::gcode(print);
    CHECK_FALSE(top_block(print));
    CHECK(count_all_curved_paths(*print.objects().front()) == 0);
    const std::string warning = curved_tops_unavailable_warning(*print.objects().front());
    INFO("warning: " << warning);
    CHECK(warning.find("supports (or side stabilizers) reach") != std::string::npos);
    CHECK_THAT(extrusion_dip(gcode_refused), WithinAbs(0., 1e-6));

    print.apply(model, puck_config(true, no_support));
    const std::string gcode_again = Test::gcode(print);
    CHECK(top_block(print));
    CHECK(curved_tops_unavailable_warning(*print.objects().front()).empty());
    CHECK(gcode_without_curved_tops_key(gcode_again) == gcode_without_curved_tops_key(gcode_curved));

    // The refused re-slice equals the feature-off slice with supports: nothing was left cut.
    print.apply(model, puck_config(false, support));
    CHECK(gcode_without_curved_tops_key(gcode_refused) == gcode_without_curved_tops_key(Test::gcode(print)));
}

// Review (a): the reach decides. Generated supports in the band always stand under the curved surface
// (whatever holds them up is object material beside the surface, which raises the cut), so a support
// island is placed by hand in the band of a curved puck (r = 15, CC1 head, band bottom Z 3.4) and the
// object re-planned from fresh walls with that support layer in place. CC1 head: nozzle reach at 0.4 mm
// above the band bottom 0.82 mm (+0.5 margin = 1.32); at 2.6 mm the sock box (+-12 mm) is in reach
// (16.97 + 0.5 = 17.47).
TEST_CASE("Curved tops: support in the band is refused within the head's reach and kept beyond it", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    struct Case { const char *name; double z, x; bool kept; const char *reason; };
    static const std::vector<Case> cases = {
        { "in the band, beyond the nozzle's reach", 3.8, 18.0, true, "" },
        { "in the band, within the nozzle's reach", 3.8, 16.0, false, "reach Z 3.80 mm, into the curved layers (from Z 3.40 mm), within 1.32 mm" },
        { "high enough for the sock to reach",      6.0, 18.0, false, "reach Z 6.00 mm, into the curved layers (from Z 3.40 mm), within 17.47 mm" },
        { "below the band, touching the surface",   3.2, 15.5, true, "" },
    };
    const size_t i = GENERATE(range(size_t(0), size_t(4)));
    REQUIRE(i < cases.size());
    const Case &c = cases[i];
    INFO("case: " << c.name);

    Print print;
    Model model;
    slice_puck(print, model, true);
    REQUIRE(top_block(print));
    PrintObject *object = print.get_object(0);
    REQUIRE(object->support_layers().empty());
    // A 1 mm square support island centred at (x, 0).
    SupportLayer *support = object->add_support_layer(0, 0, 0.2, c.z);
    support->support_islands.emplace_back(Polygon({ Point::new_scale(c.x - 0.5, -0.5), Point::new_scale(c.x + 0.5, -0.5),
                                                    Point::new_scale(c.x + 0.5, 0.5), Point::new_scale(c.x - 0.5, 0.5) }));
    CurvedTopsTestAccess::invalidate(*object, posPerimeters);
    print.process();
    const std::string warning = curved_tops_unavailable_warning(*object);
    INFO("warning: " << warning);
    CHECK(top_block(print) == c.kept);
    if (c.kept)
        CHECK(warning.empty());
    else {
        CHECK(warning.find(c.reason) != std::string::npos);
        CHECK(count_all_curved_paths(*object) == 0);
    }
}

// Review (c), whole curving: P1's tolerance (1 mm2 per layer outside the footprint grown by 0.5 mm) is
// kept there. A 0.9 x 0.9 mm pin 0.6 mm beside the puck's rim (0.81 mm2) rising to Z 5.0 passes the
// planner; the curved rim beads run down to about Z 3.6 next to it, and the head check stops the export.
TEST_CASE("Curved tops: a pin beside a whole curved top that the planner tolerates is stopped by the head check", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    TriangleMesh puck = make_capped_puck(15., 4., 1.2);
    TriangleMesh pin(its_make_cube(0.9, 0.9, 5.0));
    pin.translate(15.6f, -0.45f, 0.f);
    puck.merge(pin);
    Print print;
    Model model;
    // P4: no nozzle tolerance, P3's planner rules (with the default tolerance see the P4 cases).
    Test::init_print(std::vector<TriangleMesh>{ puck }, print, model, puck_config(true, { { "curved_tops_nozzle_tolerance", "0" } }));
    print.process();
    REQUIRE(top_block(print));
    std::string error;
    try {
        Test::gcode(print);
    } catch (const Slic3r::SlicingError &ex) {
        error = ex.what();
    }
    INFO("error: " << error);
    CHECK(error.find("Curved tops:") != std::string::npos);
    CHECK(error.find("The G-code was not exported") != std::string::npos);
}

// Review (c), partial curving: there the tolerance does not reach above the lowest curved bead. The
// same pin beside the 2.6 mm cap (cut by rise at Z 4.6), rising to Z 6.0: its flat walls above Z 4.8
// would stand above the lowest curved beads, so the cut is raised to its top (Z 6.0); the curved
// layers start at Z 6.2 and the export is clean.
TEST_CASE("Curved tops: on the partial path nothing flat beside the curved part stands above its lowest bead", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    TriangleMesh puck = make_capped_puck(30., 4., 2.6);
    TriangleMesh pin(its_make_cube(0.9, 0.9, 6.0));
    pin.translate(30.6f, -0.45f, 0.f);
    puck.merge(pin);
    Print print;
    Model model;
    // P4: no nozzle tolerance, P3's planner rules (with the default tolerance see the P4 cases).
    Test::init_print(std::vector<TriangleMesh>{ puck }, print, model, puck_config(true, { { "curved_tops_nozzle_tolerance", "0" } }));
    print.process();
    const PrintObject *object = print.objects().front();
    REQUIRE(top_block(print));
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("Partly curved: below Z 6.00 mm, parts of the object next to the curved surface") != std::string::npos);
    CHECK(summary.find("the part below Z 6.00 mm prints as flat layers") != std::string::npos);
    double min_z = 1e9;
    const Layer *top = object->layers().back();
    std::function<void(const ExtrusionEntity *)> visit = [&](const ExtrusionEntity *e) {
        if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
            for (const ExtrusionEntity *child : c->entities)
                visit(child);
        } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(e)) {
            if (p->z_contour != nullptr && p->z_contour->curved_top)
                for (const float dz : p->z_contour->z)
                    min_z = std::min(min_z, top->print_z + double(dz));
        }
    };
    for (const LayerRegion *region : top->regions())
        visit(&region->fills);
    CHECK(min_z >= 6.2 - 1e-4);
    CHECK_NOTHROW(Test::gcode(print));
}

// Review (d): a cut raised by material beside can lie below the head's rise floor. A 1.2 mm cap on a
// 30 mm puck (side wall from Z 3.18 to 4.0) standing on a wide cone too steep to curve (r 25 at Z 0 to
// r 15 at Z 3.18, about 17.6 degrees), six curved layers: the cone's layer at Z 3.0 stands outside the
// footprint in the band of whole curving (from Z 2.8), so the cut is Z 3.0 - more than 2.0 mm under
// the apex. What is curved is the cap, Z 4.0 to 5.2: the summary reports that span, not 2.2 mm.
TEST_CASE("Curved tops: the summary reports the span actually curved when material beside sets the cut", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const double r = 15., base = 4., h_cap = 1.2, rs = (r * r + h_cap * h_cap) / (2.0 * h_cap);
    std::vector<std::pair<double, double>> profile = { { 0., 0. }, { 25., 0. }, { r, 3.18 }, { r, base } };
    for (int i = 1; i < 24; ++i) {
        const double rr = r * double(24 - i) / 24.;
        profile.emplace_back(rr, base + std::sqrt(rs * rs - rr * rr) - (rs - h_cap));
    }
    profile.emplace_back(0., base + h_cap);
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ make_revolved(profile) }, print, model, puck_config(true, { { "curved_tops_layers", "6" }, { "curved_tops_nozzle_tolerance", "0" } }));
    print.process();
    const PrintObject *object = print.objects().front();
    REQUIRE(top_block(print));
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("rise 1.20 mm of 2.00 mm allowed") != std::string::npos);
    CHECK(summary.find("The top 1.20 mm (from Z 4.00 mm to 5.20 mm) is curved; the part below Z 3.00 mm prints as flat layers") !=
          std::string::npos);
    CHECK(summary.find("goes beyond what has been printed") == std::string::npos);
    CHECK_NOTHROW(Test::gcode(print));
}

// B, the owner's arm: the surface flat enough to curve is only the middle of a rounded top, and its
// steeper flanks stand in the band whole curving would need. With a 5 degree maximum the 1.2 mm cap
// of the 30 mm puck is curved only within r = 8.2 (Z 4.84 to 5.2); the ring outside stands up to the
// layer at Z 4.8 within the band (from Z 4.24). Instead of refusing (P1), the cut is raised to that
// layer: the flanks print flat up to Z 4.8 and the curved layers start a layer above it.
TEST_CASE("Curved tops: material beside a surface raises the cut instead of refusing", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    Print print;
    Model model;
    // P4: no nozzle tolerance (P3's rule); the tolerance's own case is "material beside within the nozzle tolerance".
    slice_puck(print, model, true, { { "curved_tops_max_angle", "5" }, { "curved_tops_nozzle_tolerance", "0" } });
    const PrintObject *object = print.objects().front();
    REQUIRE(top_block(print));
    CHECK(curved_tops_unavailable_warning(*object).empty());
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("Partly curved: below Z 4.80 mm, parts of the object next to the curved surface stand where the print head "
                       "would pass") != std::string::npos);
    CHECK(summary.find("the part below Z 4.80 mm prints as flat layers") != std::string::npos);
    double min_z = 1e9;
    const Layer *top = object->layers().back();
    std::function<void(const ExtrusionEntity *)> visit = [&](const ExtrusionEntity *e) {
        if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
            for (const ExtrusionEntity *child : c->entities)
                visit(child);
        } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(e)) {
            if (p->z_contour != nullptr && p->z_contour->curved_top)
                for (const float dz : p->z_contour->z)
                    min_z = std::min(min_z, top->print_z + double(dz));
        }
    };
    for (const LayerRegion *region : top->regions())
        visit(&region->fills);
    CHECK(min_z >= 4.8 + 0.2 - 1e-4);
    CHECK_NOTHROW(Test::gcode(print));
}

// C: a max angle above what curved tops uses is not silent. The summary says which value is used.
TEST_CASE("Curved tops: a max angle above the cap is used as the cap and said", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const std::string angle = GENERATE(std::string("15"), std::string("30"));
    INFO("curved_tops_max_angle = " << angle);
    Print print;
    Model model;
    slice_puck_items(print, model, true, { { "curved_tops_max_angle", angle } });
    REQUIRE(top_block(print));
    const std::string summary = object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    if (angle == "30") {
        CHECK(summary.find("of 20.0 degrees allowed") != std::string::npos);
        CHECK(summary.find("Curved tops max angle is set to 30.0 degrees; 20.0 degrees is used") != std::string::npos);
    } else {
        CHECK(summary.find("of 15.0 degrees allowed") != std::string::npos);
        CHECK(summary.find("Curved tops max angle is set to") == std::string::npos);
    }
    CHECK_THAT(curved_tops_effective_max_angle(30., curved_tops_head_limits(print.config())), WithinAbs(20., 1e-9));
}

// The support check's reach comes from the head model: the nozzle outline low down, the boxes above
// their bottoms, unlimited once the gantry is that low.
TEST_CASE("Curved tops: the head's reach around the nozzle", "[CurvedTops]")
{
    const ResourcesDirGuard resources;
    const HeadClearancePtr  x1 = resolve_head_clearance("Bambu Lab X1 Carbon");
    REQUIRE(x1 != nullptr);
    // Nozzle profile (0.05, 0.48), (0.25, 0.65): halfway between at 0.15.
    CHECK_THAT(curved_tops_head_reach_mm(*x1, 0.05), WithinAbs(0.48, 1e-9));
    CHECK_THAT(curved_tops_head_reach_mm(*x1, 0.15), WithinAbs(0.565, 1e-9));
    // Below the sock (2.5): the nozzle only. Above it: the sock box, +-12 mm, as its corner circle.
    CHECK(curved_tops_head_reach_mm(*x1, 2.4) < 2.4);
    CHECK_THAT(curved_tops_head_reach_mm(*x1, 2.6), WithinAbs(std::hypot(12., 12.), 1e-9));
    // Above the housing and ducts (5): their farthest corner (35, 40).
    CHECK_THAT(curved_tops_head_reach_mm(*x1, 6.), WithinAbs(std::hypot(35., 40.), 1e-9));
    // Above the gantry (34): unlimited.
    CHECK(std::isinf(curved_tops_head_reach_mm(*x1, 35.)));
}

// ---------------------------------------------------------------------------------------------
// P3: developer overrides (curved_tops_dev_*), active only in Developer mode.
// ---------------------------------------------------------------------------------------------

namespace {

// Sets the Developer mode gate for the life of the guard (the GUI sets it from the app config, the
// CLI from --curved-tops-dev-overrides).
struct DevModeGuard
{
    bool saved;
    explicit DevModeGuard(bool on) : saved(curved_tops_developer_mode()) { set_curved_tops_developer_mode(on); }
    ~DevModeGuard() { set_curved_tops_developer_mode(saved); }
};

// The G-code without the config block lines of any curved tops key and the lines that vary per run.
std::string without_curved_tops_config(const std::string &gcode)
{
    std::istringstream in(gcode);
    std::string        out;
    for (std::string line; std::getline(in, line);)
        if (line.find("curved_tops_") == std::string::npos && line.find("generated by") == std::string::npos &&
            line.find("label id") == std::string::npos)
            out += line + "\n";
    return out;
}

std::string print_warning(const Print &print, PrintStateBase::SlicingNotificationType id, PrintStateBase::WarningLevel *level = nullptr)
{
    for (const PrintStateBase::Warning &w : print.step_state_with_warnings(psGCodeExport).warnings)
        if (w.message_id == int(id)) {
            if (level != nullptr)
                *level = w.level;
            return w.message;
        }
    return {};
}

size_t count_occurrences(const std::string &text, const std::string &what)
{
    size_t n = 0;
    for (size_t pos = text.find(what); pos != std::string::npos; pos = text.find(what, pos + what.size()))
        ++n;
    return n;
}

// Every overridable refusal switched off and every number moved off its built-in value.
const ConfigItems &all_overrides()
{
    static const ConfigItems items = {
        { "curved_tops_dev_angle_ceiling", "30" },          { "curved_tops_dev_clearance_margin", "0.25" },
        { "curved_tops_dev_flank_margin", "2" },            { "curved_tops_dev_land_tolerance", "0.15" },
        { "curved_tops_dev_reach_margin", "0.2" },          { "curved_tops_dev_speed_cap", "30" },
        { "curved_tops_dev_wipe_speed_cap", "25" },         { "curved_tops_dev_safe_lift", "1.5" },
        { "curved_tops_dev_contact_warning_only", "1" },    { "curved_tops_dev_allow_multiple_objects", "1" },
        { "curved_tops_dev_allow_by_object", "1" },         { "curved_tops_dev_allow_multi_filament", "1" },
        { "curved_tops_dev_allow_supports_in_reach", "1" }, { "curved_tops_dev_allow_material_beside", "1" },
        { "curved_tops_dev_allow_z_contouring", "1" },      { "curved_tops_dev_allow_raft", "1" },
        { "curved_tops_dev_allow_variable_layer_height", "1" }, { "curved_tops_dev_allow_tall_skirt", "1" },
        { "curved_tops_dev_allow_unverified_head", "1" },
    };
    return items;
}

double lowest_curved_z(const PrintObject &object)
{
    double       min_z = 1e9;
    const Layer *top   = object.layers().back();
    std::function<void(const ExtrusionEntity *)> visit = [&](const ExtrusionEntity *e) {
        if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
            for (const ExtrusionEntity *child : c->entities)
                visit(child);
        } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(e)) {
            if (p->z_contour != nullptr && p->z_contour->curved_top)
                for (const float dz : p->z_contour->z)
                    min_z = std::min(min_z, top->print_z + double(dz));
        }
    };
    for (const LayerRegion *region : top->regions())
        visit(&region->fills);
    return min_z;
}

} // namespace

// Defaults are today's behaviour: with nothing overridden, Developer mode on or off gives the same
// G-code byte for byte, feature on or off, with no override warning and no comment block.
TEST_CASE("Curved tops P3: with nothing overridden Developer mode changes nothing", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    const bool curved = GENERATE(false, true);
    INFO("curved_tops_enabled = " << curved);
    std::string gcode_off, gcode_on;
    {
        const DevModeGuard dev(false);
        Print print;
        Model model;
        slice_puck(print, model, curved);
        gcode_off = Test::gcode(print);
    }
    {
        const DevModeGuard dev(true);
        Print print;
        Model model;
        slice_puck(print, model, curved);
        gcode_on = Test::gcode(print);
        CHECK(object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsDevOverrides).empty());
        CHECK(print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck).empty());
        CHECK(top_block(print) == curved);
    }
    CHECK(gcode_on.find("CURVED_TOPS_DEV_OVERRIDES") == std::string::npos);
    // Same config on both sides: only the per-run lines may differ.
    CHECK(gcode_without_curved_tops_key(gcode_on) == gcode_without_curved_tops_key(gcode_off));
}

// The overrides read from a config: nothing carried at the defaults; carried but not in force without
// Developer mode (the built-in values, and a note naming them); in force with it.
TEST_CASE("Curved tops P3: overrides are read from the config and gated by Developer mode", "[CurvedTops][CurvedTopsDev]")
{
    PrintObjectConfig cfg;
    const CurvedTopsDevOverrides none = curved_tops_dev_overrides(cfg, true);
    CHECK_FALSE(none.carried);
    CHECK_FALSE(none.active);
    CHECK(none.record().empty());
    CHECK(none.active_message().empty());
    CHECK(none.ignored_note().empty());
    CHECK_THAT(none.angle_ceiling_deg, WithinAbs(CURVED_TOPS_MAX_ANGLE_CEILING, 1e-12));
    CHECK_THAT(none.clearance_margin_mm, WithinAbs(CURVED_TOPS_CLEARANCE_MARGIN_MM, 1e-12));
    CHECK_THAT(none.speed_cap_mm_s, WithinAbs(CURVED_TOPS_MAX_SPEED_MM_S, 1e-12));

    cfg.curved_tops_dev_angle_ceiling.value    = 32.5;
    cfg.curved_tops_dev_allow_tall_skirt.value = true;
    const CurvedTopsDevOverrides ignored = curved_tops_dev_overrides(cfg, false);
    CHECK(ignored.carried);
    CHECK_FALSE(ignored.active);
    CHECK_FALSE(ignored.allow_tall_skirt);
    CHECK_THAT(ignored.angle_ceiling_deg, WithinAbs(CURVED_TOPS_MAX_ANGLE_CEILING, 1e-12));
    CHECK(ignored.active_message().empty());
    INFO("ignored note: " << ignored.ignored_note());
    CHECK(ignored.ignored_note().find("were ignored") != std::string::npos);
    CHECK(ignored.ignored_note().find("angle ceiling 32.5 deg") != std::string::npos);
    CHECK(ignored.record().rfind("ignored:", 0) == 0);

    const CurvedTopsDevOverrides active = curved_tops_dev_overrides(cfg, true);
    CHECK(active.active);
    CHECK(active.allow_tall_skirt);
    CHECK_FALSE(active.allow_raft);
    CHECK_THAT(active.angle_ceiling_deg, WithinAbs(32.5, 1e-12));
    CHECK(active.ignored_note().empty());
    INFO("active: " << active.active_message());
    CHECK(active.active_message().find("Curved tops developer overrides active: angle ceiling 32.5 deg; allow draft shield or tall skirt") !=
          std::string::npos);
    CHECK(active.record() != ignored.record());

    // The global gate is what the one-argument form reads.
    {
        const DevModeGuard dev(true);
        CHECK(curved_tops_dev_overrides(cfg).active);
    }
    {
        const DevModeGuard dev(false);
        CHECK_FALSE(curved_tops_dev_overrides(cfg).active);
    }
}

// Each numeric override moves exactly its own limit (X1 Carbon head: sock 2.5 mm, flank well over 25 deg).
TEST_CASE("Curved tops P3: each numeric override changes only its own limit", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    const HeadClearancePtr  x1 = resolve_head_clearance("Bambu Lab X1 Carbon");
    REQUIRE(x1 != nullptr);
    const std::string model = "Bambu Lab X1 Carbon";
    auto limits = [&](const std::function<void(PrintObjectConfig &)> &set) {
        PrintObjectConfig cfg;
        set(cfg);
        return curved_tops_head_limits(x1, model, curved_tops_dev_overrides(cfg, true));
    };
    const CurvedTopsHeadLimits base = limits([](PrintObjectConfig &) {});
    REQUIRE(base.usable);
    CHECK_THAT(base.allowed_rise_mm, WithinAbs(2.0, 1e-9));
    CHECK_THAT(base.max_slope_deg, WithinAbs(20.0, 1e-9));
    const double flank = x1->nozzle_flank_angle_deg(CURVED_TOPS_LAND_TOLERANCE_MM);
    REQUIRE(flank - CURVED_TOPS_FLANK_MARGIN_DEG > 30.);

    const CurvedTopsHeadLimits ceiling30 = limits([](PrintObjectConfig &c) { c.curved_tops_dev_angle_ceiling.value = 30; });
    CHECK_THAT(ceiling30.max_slope_deg, WithinAbs(30.0, 1e-9));
    CHECK_THAT(ceiling30.allowed_rise_mm, WithinAbs(2.0, 1e-9));
    CHECK_THAT(curved_tops_effective_max_angle(40., ceiling30), WithinAbs(30.0, 1e-9));
    CHECK_THAT(curved_tops_effective_max_angle(25., ceiling30), WithinAbs(25.0, 1e-9));

    const CurvedTopsHeadLimits ceiling60 = limits([](PrintObjectConfig &c) { c.curved_tops_dev_angle_ceiling.value = 60; });
    CHECK_THAT(ceiling60.max_slope_deg, WithinAbs(flank - CURVED_TOPS_FLANK_MARGIN_DEG, 1e-9));

    const CurvedTopsHeadLimits lowest = limits([](PrintObjectConfig &c) { c.curved_tops_dev_head_lowest_part.value = 4.; });
    CHECK_THAT(lowest.allowed_rise_mm, WithinAbs(3.5, 1e-9));
    CHECK_THAT(lowest.max_slope_deg, WithinAbs(20.0, 1e-9));

    const CurvedTopsHeadLimits margin0 = limits([](PrintObjectConfig &c) { c.curved_tops_dev_clearance_margin.value = 0.; });
    CHECK_THAT(margin0.allowed_rise_mm, WithinAbs(2.5, 1e-9));
    CHECK_THAT(margin0.max_slope_deg, WithinAbs(20.0, 1e-9));

    const CurvedTopsHeadLimits flank0 = limits([](PrintObjectConfig &c) {
        c.curved_tops_dev_angle_ceiling.value = 60;
        c.curved_tops_dev_flank_margin.value  = 0;
    });
    CHECK_THAT(flank0.max_slope_deg, WithinAbs(flank, 1e-9));
    CHECK_THAT(flank0.allowed_rise_mm, WithinAbs(2.0, 1e-9));

    const CurvedTopsHeadLimits land = limits([](PrintObjectConfig &c) {
        c.curved_tops_dev_angle_ceiling.value  = 60;
        c.curved_tops_dev_land_tolerance.value = 0.3;
    });
    CHECK_THAT(land.max_slope_deg, WithinAbs(std::min(60., x1->nozzle_flank_angle_deg(0.3) - CURVED_TOPS_FLANK_MARGIN_DEG), 1e-9));

    // Without Developer mode the same config changes nothing.
    PrintObjectConfig cfg;
    cfg.curved_tops_dev_angle_ceiling.value    = 30;
    cfg.curved_tops_dev_clearance_margin.value = 0;
    const CurvedTopsHeadLimits gated = curved_tops_head_limits(x1, model, curved_tops_dev_overrides(cfg, false));
    CHECK_THAT(gated.max_slope_deg, WithinAbs(20.0, 1e-9));
    CHECK_THAT(gated.allowed_rise_mm, WithinAbs(2.0, 1e-9));
}

// "Unverified or missing head data": an estimated head becomes usable; no head data at all needs the
// lowest head part, and then curves on the developer's numbers with the ceiling as the slope limit.
TEST_CASE("Curved tops P3: unverified or missing head data", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    PrintObjectConfig cfg;
    cfg.curved_tops_dev_allow_unverified_head.value = true;
    const CurvedTopsDevOverrides dev = curved_tops_dev_overrides(cfg, true);

    const HeadClearancePtr x1 = resolve_head_clearance("Bambu Lab X1E");
    REQUIRE(x1 != nullptr);
    CHECK_FALSE(curved_tops_head_limits(x1, "Bambu Lab X1E").usable);
    const CurvedTopsHeadLimits x1e = curved_tops_head_limits(x1, "Bambu Lab X1E", dev);
    CHECK(x1e.usable);
    CHECK_THAT(x1e.allowed_rise_mm, WithinAbs(2.0, 1e-9));
    CHECK(x1e.source.find("developer override") != std::string::npos);
    // Gated off: refused as before.
    CHECK_FALSE(curved_tops_head_limits(x1, "Bambu Lab X1E", curved_tops_dev_overrides(cfg, false)).usable);

    const CurvedTopsHeadLimits none = curved_tops_head_limits(nullptr, "Generic Printer", dev);
    CHECK_FALSE(none.usable);
    CHECK(none.refusal.find("needs the lowest head part") != std::string::npos);

    cfg.curved_tops_dev_head_lowest_part.value = 3.0;
    const CurvedTopsHeadLimits generic = curved_tops_head_limits(nullptr, "Generic Printer", curved_tops_dev_overrides(cfg, true));
    CHECK(generic.usable);
    CHECK_FALSE(generic.known);
    CHECK_THAT(generic.allowed_rise_mm, WithinAbs(2.5, 1e-9));
    CHECK_THAT(generic.max_slope_deg, WithinAbs(CURVED_TOPS_MAX_ANGLE_CEILING, 1e-9));
    CHECK(generic.refusal.empty());
}

// A no-head print with the override curves, and the export says loudly that it was not checked.
TEST_CASE("Curved tops P3: a printer with no head data curves on the override and is reported unchecked", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    const DevModeGuard      dev(true);
    Print print;
    Model model;
    slice_puck(print, model, true,
               { { "printer_model", "Generic Printer" },
                 { "curved_tops_dev_allow_unverified_head", "1" },
                 { "curved_tops_dev_head_lowest_part", "2.5" } });
    REQUIRE(top_block(print));
    CHECK_NOTHROW(Test::gcode(print));
    PrintStateBase::WarningLevel level = PrintStateBase::WarningLevel::NON_CRITICAL;
    const std::string msg = print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck, &level);
    INFO("head check: " << msg);
    CHECK(msg.find("NOT checked") != std::string::npos);
    CHECK(level == PrintStateBase::WarningLevel::CRITICAL);
}

// A refusal override lifts its own guard only, and only in Developer mode; without it the refusal
// stands exactly as before (output = feature-off output) and the message says the override was ignored.
TEST_CASE("Curved tops P3: a refusal override lifts only its own guard, and only in Developer mode", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    struct Guard { const char *name; ConfigItems extra; int filaments; const char *reason; const char *allow; const char *other; };
    static const std::vector<Guard> guards = {
        { "by object",     { { "print_sequence", "by object" } }, 1, "prints by object",
          "curved_tops_dev_allow_by_object", "curved_tops_dev_allow_raft" },
        { "draft shield",  { { "skirt_loops", "1" }, { "draft_shield", "enabled" } }, 1, "draft shield",
          "curved_tops_dev_allow_tall_skirt", "curved_tops_dev_allow_by_object" },
        { "tall skirt",    { { "skirt_loops", "1" }, { "skirt_height", "3" } }, 1, "skirt taller than one layer",
          "curved_tops_dev_allow_tall_skirt", "curved_tops_dev_allow_multi_filament" },
        { "two filaments", { { "sparse_infill_filament", "2" }, { "solid_infill_filament", "2" } }, 2, "more than one filament",
          "curved_tops_dev_allow_multi_filament", "curved_tops_dev_allow_tall_skirt" },
        { "raft",          { { "raft_layers", "2" } }, 1, "raft",
          "curved_tops_dev_allow_raft", "curved_tops_dev_allow_z_contouring" },
        { "Z contouring",  { { "zaa_enabled", "1" } }, 1, "Z contouring is on for it as well",
          "curved_tops_dev_allow_z_contouring", "curved_tops_dev_allow_raft" },
    };
    const size_t i = GENERATE(range(size_t(0), size_t(6)));
    REQUIRE(i < guards.size());
    const Guard &g = guards[i];
    INFO("guard: " << g.name);

    // Developer mode off: the override is carried and ignored; the refusal is today's.
    {
        const DevModeGuard dev(false);
        const ConfigItems  items = joined(g.extra, { { g.allow, "1" } });
        check_refused_like_off(items, g.filaments, g.reason);
        Print print;
        Model model;
        slice_puck_items(print, model, true, items, g.filaments);
        CHECK(curved_tops_unavailable_warning(*print.objects().front()).find("were ignored") != std::string::npos);
        CHECK(object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsDevOverrides).empty());
    }
    // Developer mode on, another guard's override: still refused for this guard's reason.
    {
        const DevModeGuard dev(true);
        Print print;
        Model model;
        slice_puck_items(print, model, true, joined(g.extra, { { g.other, "1" } }), g.filaments);
        CHECK_FALSE(top_block(print));
        CHECK(curved_tops_unavailable_warning(*print.objects().front()).find(g.reason) != std::string::npos);
    }
    // Developer mode on, its own override: curved, the override listed, the export goes through (a contact
    // would be a warning here; the raft case asks for none).
    {
        const DevModeGuard dev(true);
        Print print;
        Model model;
        slice_puck_items(print, model, true, joined(g.extra, { { g.allow, "1" }, { "curved_tops_dev_contact_warning_only", "1" } }),
                         g.filaments);
        const PrintObject *object = print.objects().front();
        CHECK(top_block(print));
        CHECK(curved_tops_unavailable_warning(*object).empty());
        const std::string listed = object_warning(*object, PrintStateBase::SlicingCurvedTopsDevOverrides);
        INFO("overrides warning: " << listed);
        CHECK(listed.find("Curved tops developer overrides active") != std::string::npos);
        std::string gcode;
        CHECK_NOTHROW(gcode = Test::gcode(print));
        CHECK(count_occurrences(gcode, ";CURVED_TOPS_DEV_OVERRIDES_START") == 1);
        if (std::string(g.name) == "raft") {
            // The raft lifts the object; the curved layers follow it (no bead under the first object layer,
            // none inside the part: the head check finds no contact).
            CHECK(lowest_curved_z(*object) >= object->layers().front()->print_z);
            CHECK(lowest_curved_z(*object) > 3.5 + object->slicing_parameters().object_print_z_min);
            const std::string check = print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck);
            INFO("head check: " << check);
            CHECK(check.find("no head contact found") != std::string::npos);
        }
    }
}

// What the code cannot produce has no override: with every override on, these still refuse.
TEST_CASE("Curved tops P3: refusals for what the code cannot do stay, whatever the overrides", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    const DevModeGuard      dev(true);
    struct Hard { const char *name; ConfigItems extra; const char *reason; };
    static const std::vector<Hard> hard = {
        { "local-Z dithering", { { "dithering_local_z_mode", "1" } }, "local-Z dithering" },
        { "printable height",  { { "printable_height", "5.5" } }, "above the printable height" },
        // The travel lift is overridable, the printable height is not.
        { "lift above height", { { "printable_height", "7" }, { "curved_tops_dev_safe_lift", "3" } }, "above the printable height" },
    };
    const size_t i = GENERATE(range(size_t(0), size_t(3)));
    REQUIRE(i < hard.size());
    INFO("guard: " << hard[i].name);
    ConfigItems items = all_overrides();
    for (const auto &kv : hard[i].extra) {
        items.erase(std::remove_if(items.begin(), items.end(), [&kv](const std::pair<std::string, std::string> &it) { return it.first == kv.first; }),
                    items.end());
        items.push_back(kv);
    }
    Print print;
    Model model;
    slice_puck_items(print, model, true, items);
    CHECK_FALSE(top_block(print));
    const std::string warning = curved_tops_unavailable_warning(*print.objects().front());
    INFO("warning: " << warning);
    CHECK(warning.find(hard[i].reason) != std::string::npos);
}

// Developer mode off: every carried override is ignored, the G-code is today's, and the summary says so.
TEST_CASE("Curved tops P3: Developer mode off ignores every carried override and says so", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    const DevModeGuard      dev(false);
    Print print_plain;
    Model model_plain;
    slice_puck_items(print_plain, model_plain, true, {});
    const std::string plain = Test::gcode(print_plain);

    Print print;
    Model model;
    slice_puck_items(print, model, true, all_overrides());
    const std::string carried = Test::gcode(print);
    REQUIRE(top_block(print));
    CHECK(without_curved_tops_config(carried) == without_curved_tops_config(plain));
    CHECK(carried.find("CURVED_TOPS_DEV_OVERRIDES") == std::string::npos);
    const std::string summary = object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("developer overrides in this project were ignored") != std::string::npos);
    CHECK(summary.find("curved moves at most 40 mm/s") != std::string::npos);
    CHECK(object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsDevOverrides).empty());
    CHECK(print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck).empty());
}

// In force: listed in a warning every slice, in a comment block at the block's start, and applied
// (speed caps, travel lift); the summary keeps the real numbers and the head check reports its result
// with the nearest head part.
TEST_CASE("Curved tops P3: overrides in force are listed, applied and reported", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    const DevModeGuard      dev(true);
    Print print;
    Model model;
    slice_puck(print, model, true, {
        { "curved_tops_dev_speed_cap",      "25" },
        { "curved_tops_dev_wipe_speed_cap", "20" },
        { "curved_tops_dev_safe_lift",      "2" },
        { "wipe",                           "1" },
        { "wipe_distance",                  "2" },
        { "wipe_speed",                     "150" },
        { "retraction_length",              "0.8" },
        { "enable_arc_fitting",             "0" },
        { "machine_end_gcode",              "G1 X5 Y5 F6000" },
    });
    REQUIRE(top_block(print));
    const PrintObject *object = print.objects().front();
    const std::string  listed = object_warning(*object, PrintStateBase::SlicingCurvedTopsDevOverrides);
    INFO("overrides warning: " << listed);
    CHECK(listed.find("curved speed cap 25 mm/s; wipe speed cap 20 mm/s; travel/exit lift 2 mm") != std::string::npos);
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("curved moves at most 25 mm/s") != std::string::npos);
    CHECK(summary.find("of 2.00 mm allowed") != std::string::npos);
    CHECK(summary.find("Lowest curved bead at Z") != std::string::npos);

    const std::string gcode = Test::gcode(print);
    CHECK(count_occurrences(gcode, ";CURVED_TOPS_DEV_OVERRIDES_START") == 1);
    CHECK(gcode.find("; - curved speed cap 25 mm/s") != std::string::npos);
    // The comment comes before the block: nothing before it dips.
    const size_t comment = gcode.find(";CURVED_TOPS_DEV_OVERRIDES_START");
    CHECK_FALSE(analyse_block(gcode.substr(0, comment)).curved);

    const CurvedTravels travels = check_curved_travels(gcode, 1.0, 0.0);
    CHECK(travels.max_extrude_f > 0.);
    CHECK(travels.max_extrude_f <= 25. * 60. + 1e-6);
    const BlockMoves b = analyse_block(gcode);
    REQUIRE(b.curved);
    CHECK(b.block_wipes >= 5);
    CHECK(b.max_block_wipe_f <= 20. * 60. + 1e-6);
    CHECK(b.exit_lift_z >= b.highest + 2.0 - 1e-3);

    PrintStateBase::WarningLevel level = PrintStateBase::WarningLevel::CRITICAL;
    const std::string check = print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck, &level);
    INFO("head check: " << check);
    CHECK(check.find("no head contact found") != std::string::npos);
    CHECK(check.find("Nearest head part over the curved layer: ") != std::string::npos);
    CHECK(level == PrintStateBase::WarningLevel::NON_CRITICAL);
}

// "Contact is a warning only": the contact is still found and reported (count, depth, part), critical,
// and the G-code is exported. Without Developer mode, or without the override, it still stops the export.
// Same deliberately wrong head as the P1 contact test.
TEST_CASE("Curved tops P3: a contact downgraded to a warning is still found and reported", "[CurvedTops][CurvedTopsDev]")
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("curvedtops_dev_%%%%-%%%%");
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
        const int  combo     = GENERATE(range(0, 4));
        const bool developer = (combo & 1) != 0;
        const bool warn_only = (combo & 2) != 0;
        INFO("Developer mode " << developer << ", contact_warning_only " << warn_only);
        const DevModeGuard dev(developer);
        Print print;
        Model model;
        {
            const ResourcesDirGuard shipped;
            slice_puck(print, model, true, { { "curved_tops_dev_contact_warning_only", warn_only ? "1" : "0" } });
            REQUIRE(top_block(print));
        }
        const ResourcesDirGuard wrong(dir.string());
        std::string error;
        try {
            Test::gcode(print);
        } catch (const Slic3r::SlicingError &ex) {
            error = ex.what();
        }
        if (developer && warn_only) {
            CHECK(error.empty());
            PrintStateBase::WarningLevel level = PrintStateBase::WarningLevel::NON_CRITICAL;
            const std::string msg = print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck, &level);
            INFO("warning: " << msg);
            CHECK(level == PrintStateBase::WarningLevel::CRITICAL);
            CHECK(msg.rfind("EXPECTED CONTACT - do not print unattended.", 0) == 0);
            CHECK(msg.find("moves bring the print head into material") != std::string::npos);
            CHECK(msg.find("low sock") != std::string::npos);
            CHECK(msg.find("The G-code WAS exported") != std::string::npos);
            CHECK(msg.find("Nearest head part over the curved layer: low sock, -") != std::string::npos);
            // Per head part, and the sock hit is named as more than the nozzle tip.
            CHECK(msg.find("By head part:") != std::string::npos);
            CHECK(msg.find("NOT ONLY THE NOZZLE TIP: other head parts touched too: ") != std::string::npos);
            CHECK(msg.find("low sock: ") != std::string::npos);
        } else {
            INFO("error: " << error);
            CHECK(error.find("The G-code was not exported") != std::string::npos);
        }
    }
    boost::system::error_code ec;
    boost::filesystem::remove_all(dir, ec);
}

// Material beside allowed: the cut is no longer raised over the flanks; the whole surface flat enough
// is curved (said in a note), and the head check decides about the flanks.
TEST_CASE("Curved tops P3: material beside allowed curves the whole surface", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    const DevModeGuard      dev(true);
    Print print;
    Model model;
    slice_puck(print, model, true,
               { { "curved_tops_max_angle", "5" },
                 { "curved_tops_nozzle_tolerance", "0" },
                 { "curved_tops_dev_allow_material_beside", "1" },
                 { "curved_tops_dev_contact_warning_only", "1" } });
    const PrintObject *object = print.objects().front();
    REQUIRE(top_block(print));
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("Partly curved") == std::string::npos);
    CHECK(summary.find("Developer override: curved although part of it next to the curved surface rises into the curved layers") !=
          std::string::npos);
    // Without the override the cut sits at Z 4.8 (see "material beside a surface raises the cut"); now the
    // lowest curved bead is below that.
    CHECK(lowest_curved_z(*object) < 4.8);
    CHECK_NOTHROW(Test::gcode(print));
    CHECK_FALSE(print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck).empty());
}

// The guard record: switching Developer mode, or changing an override, after a slice re-plans the block;
// switching back gives the first slice again.
TEST_CASE("Curved tops P3: Developer mode and override changes re-plan the block", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    const ConfigItems       items = { { "skirt_loops", "1" }, { "skirt_height", "3" }, { "curved_tops_dev_allow_tall_skirt", "1" } };
    Print print;
    Model model;
    std::string gcode_curved;
    {
        const DevModeGuard dev(true);
        Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, print, model, puck_config(true, items));
        gcode_curved = Test::gcode(print);
        REQUIRE(top_block(print));
    }
    {
        const DevModeGuard dev(false);
        print.apply(model, puck_config(true, items));
        const std::string gcode_ignored = Test::gcode(print);
        CHECK_FALSE(top_block(print));
        const std::string warning = curved_tops_unavailable_warning(*print.objects().front());
        INFO("warning: " << warning);
        CHECK(warning.find("skirt taller than one layer") != std::string::npos);
        CHECK(warning.find("were ignored") != std::string::npos);
        CHECK_THAT(extrusion_dip(gcode_ignored), WithinAbs(0., 1e-6));
        print.apply(model, puck_config(false, items));
        CHECK(gcode_without_curved_tops_key(Test::gcode(print)) == gcode_without_curved_tops_key(gcode_ignored));
    }
    {
        const DevModeGuard dev(true);
        print.apply(model, puck_config(true, items));
        CHECK(gcode_without_curved_tops_key(Test::gcode(print)) == gcode_without_curved_tops_key(gcode_curved));
        CHECK(top_block(print));
        // A G-code-only override changed after the slice: re-planned and re-exported with the new cap.
        print.apply(model, puck_config(true, joined(items, { { "curved_tops_dev_speed_cap", "22" } })));
        const std::string gcode_capped = Test::gcode(print);
        CHECK(top_block(print));
        CHECK(gcode_capped.find("; - curved speed cap 22 mm/s") != std::string::npos);
        CHECK(check_curved_travels(gcode_capped, 1.0, 0.0).max_extrude_f <= 22. * 60. + 1e-6);
        CHECK(object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsDevOverrides).find("curved speed cap 22") !=
              std::string::npos);
    }
}

// Out-of-range or invalid override values (per-object or hand-edited: Print::apply() does not check the
// ranges) are clamped to the documented range, and said.
TEST_CASE("Curved tops P3: out-of-range override values are clamped and said", "[CurvedTops][CurvedTopsDev]")
{
    PrintObjectConfig cfg;
    cfg.curved_tops_dev_safe_lift.value        = -1.;
    cfg.curved_tops_dev_clearance_margin.value = -0.5;
    cfg.curved_tops_dev_speed_cap.value        = 0.;
    cfg.curved_tops_dev_angle_ceiling.value    = std::numeric_limits<double>::quiet_NaN();
    const CurvedTopsDevOverrides dev = curved_tops_dev_overrides(cfg, true);
    REQUIRE(dev.active);
    CHECK_THAT(dev.safe_lift_mm, WithinAbs(0.2, 1e-6)); // the option range is stored as float
    CHECK_THAT(dev.clearance_margin_mm, WithinAbs(0.0, 1e-12));
    CHECK_THAT(dev.speed_cap_mm_s, WithinAbs(1.0, 1e-12));
    CHECK_THAT(dev.angle_ceiling_deg, WithinAbs(CURVED_TOPS_MAX_ANGLE_CEILING, 1e-12));
    REQUIRE(dev.clamp_notes.size() == 4);
    std::string notes;
    for (const std::string &n : dev.clamp_notes)
        notes += n + "\n";
    INFO("notes: " << notes);
    CHECK(notes.find("travel/exit lift -1 mm is outside 0.2 to 20; 0.2 mm is used") != std::string::npos);
    CHECK(notes.find("angle ceiling is not a number") != std::string::npos);
    // The clamped values are what the summary path uses.
    const ResourcesDirGuard resources;
    const DevModeGuard      gate(true);
    Print print;
    Model model;
    slice_puck(print, model, true, { { "curved_tops_dev_speed_cap", "0.5" } });
    REQUIRE(top_block(print));
    const std::string summary = object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    CHECK(summary.find("curved moves at most 1 mm/s") != std::string::npos);
    CHECK(summary.find("curved speed cap 0.5 mm/s is outside 1 to 500; 1 mm/s is used") != std::string::npos);
}

// A block planned with Developer mode on cannot be exported once it is off (and the other way round),
// even without a Print::apply() in between: the export compares the guard record with the gate.
TEST_CASE("Curved tops P3: a block planned under other override settings is never exported", "[CurvedTops][CurvedTopsDev]")
{
    const ResourcesDirGuard resources;
    const bool on_first = GENERATE(true, false);
    INFO("Developer mode at planning: " << on_first);
    Print print;
    Model model;
    {
        const DevModeGuard gate(on_first);
        slice_puck(print, model, true, { { "curved_tops_dev_speed_cap", "25" } });
        REQUIRE(top_block(print));
    }
    const DevModeGuard gate(!on_first);
    std::string error;
    try {
        Test::gcode(print);
    } catch (const Slic3r::SlicingError &ex) {
        error = ex.what();
    }
    INFO("error: " << error);
    CHECK(error.find("was sliced with other developer override settings") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------
// P4: the nozzle's contact tolerance in the planner, the fill patterns, the smooth patch edge
// ---------------------------------------------------------------------------------------------

namespace {

// Every curved sample of the top layer's block, as (x, y, z) in the print frame.
std::vector<Vec3d> curved_samples(const PrintObject &object)
{
    std::vector<Vec3d> out;
    const Layer       *top = object.layers().back();
    std::function<void(const ExtrusionEntity *)> visit = [&](const ExtrusionEntity *e) {
        if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
            for (const ExtrusionEntity *child : c->entities)
                visit(child);
        } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(e)) {
            if (p->z_contour != nullptr && p->z_contour->curved_top)
                for (size_t i = 0; i < p->z_contour->z.size() && i < p->z_contour->points.size(); ++i)
                    out.emplace_back(unscale<double>(p->z_contour->points[i].x()), unscale<double>(p->z_contour->points[i].y()),
                                     top->print_z + double(p->z_contour->z[i]));
        }
    };
    for (const LayerRegion *region : top->regions())
        visit(&region->fills);
    return out;
}

// The highest flat (not curved) extrusion top among the layers below the top one, outside `inside_r` of the
// object's centre (x, y in the object frame); -inf when none.
double highest_flat_outside(const PrintObject &object, double inside_r)
{
    double best = -std::numeric_limits<double>::max();
    for (const Layer *layer : object.layers()) {
        std::vector<const ExtrusionPath *> paths;
        std::function<void(const ExtrusionEntity *)> collect = [&](const ExtrusionEntity *e) {
            if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(e)) {
                for (const ExtrusionEntity *child : c->entities)
                    collect(child);
            } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(e))
                paths.push_back(p);
            else if (const auto *l = dynamic_cast<const ExtrusionLoop *>(e))
                for (const ExtrusionPath &p : l->paths)
                    paths.push_back(&p);
            else if (const auto *m = dynamic_cast<const ExtrusionMultiPath *>(e))
                for (const ExtrusionPath &p : m->paths)
                    paths.push_back(&p);
        };
        for (const LayerRegion *region : layer->regions()) {
            collect(&region->perimeters);
            collect(&region->fills);
        }
        for (const ExtrusionPath *p : paths) {
            if (p->z_contour != nullptr && p->z_contour->curved_top)
                continue;
            for (const Point &pt : p->polyline.points)
                if (std::hypot(unscale<double>(pt.x()), unscale<double>(pt.y())) > inside_r)
                    best = std::max(best, layer->print_z);
        }
    }
    return best;
}

} // namespace

// The owner's arm in small: a 5 degree maximum on the 1.2 mm cap curves only r <= ~8.2 (Z 4.84 up), and the
// flat ring beside it stands up to the layer at Z 4.8. Whole curving puts the lowest curved bead two layers
// under the edge (about Z 4.44): 0.36 mm under that ring.
//  - tolerance 0 (P3): the cut rises to Z 4.8, nothing flat beside stands above the lowest bead;
//  - tolerance 0.4 (default): the ring is within it, the whole patch curves, the head check passes and says
//    how deep the nozzle goes;
//  - tolerance 0.2: the cut rises only as far as needed, to Z 4.4 (ring at 4.8 = lowest bead 4.6 + 0.2).
// In every case no flat material beside the patch stands more than the tolerance above the lowest curved bead.
TEST_CASE("Curved tops P4: material beside within the nozzle tolerance passes, beyond it the cut rises only as far as needed",
          "[CurvedTops][CurvedTopsP4]")
{
    const ResourcesDirGuard resources;
    const std::string tol = GENERATE(std::string("0"), std::string("0.2"), std::string("0.4"));
    INFO("curved_tops_nozzle_tolerance = " << tol);
    const double tolerance = std::atof(tol.c_str());
    Print print;
    Model model;
    slice_puck(print, model, true, { { "curved_tops_max_angle", "5" }, { "curved_tops_nozzle_tolerance", tol } });
    const PrintObject *object = print.objects().front();
    REQUIRE(top_block(print));
    const std::string summary = object_warning(*object, PrintStateBase::SlicingCurvedTopsSummary);
    INFO("summary: " << summary);
    const double lowest = lowest_curved_z(*object);
    INFO("lowest curved bead " << lowest);
    if (tol == "0") {
        CHECK(summary.find("Partly curved: below Z 4.80 mm") != std::string::npos);
        CHECK(lowest >= 5.0 - 1e-4);
    } else if (tol == "0.2") {
        CHECK(summary.find("Partly curved: below Z 4.40 mm") != std::string::npos);
        CHECK(lowest >= 4.6 - 1e-4);
        CHECK(lowest < 4.8);
    } else {
        CHECK(summary.find("Partly curved") == std::string::npos);
        CHECK(lowest < 4.6);
    }
    // Flat material right beside the curved part (outside r 8.3): never more than the tolerance above the
    // lowest curved bead. (A layer flush with the bead counts as 0.)
    const double flat = highest_flat_outside(*object, 8.3);
    INFO("highest flat beside " << flat);
    CHECK(flat <= lowest + tolerance + 1e-3);
    std::string error;
    try {
        Test::gcode(print);
    } catch (const Slic3r::SlicingError &ex) {
        error = ex.what();
    }
    INFO("error: " << error);
    CHECK(error.empty());
    if (tol == "0.4") {
        const std::string check = print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck);
        INFO("head check: " << check);
        CHECK(check.find("The nozzle passes up to") != std::string::npos);
    }
}

// The pin beside the whole cap that P3's planner tolerated and the head check stopped (see "a pin beside a
// whole curved top..."): with the default tolerance the planner itself keeps the pin within it, raising the
// cut, and the export is clean.
TEST_CASE("Curved tops P4: with the nozzle tolerance the planner keeps a pin beside the cap within it", "[CurvedTops][CurvedTopsP4]")
{
    const ResourcesDirGuard resources;
    TriangleMesh puck = make_capped_puck(15., 4., 1.2);
    TriangleMesh pin(its_make_cube(0.9, 0.9, 5.0));
    pin.translate(15.6f, -0.45f, 0.f);
    puck.merge(pin);
    Print print;
    Model model;
    Test::init_print(std::vector<TriangleMesh>{ puck }, print, model, puck_config(true, {}));
    print.process();
    const PrintObject *object = print.objects().front();
    REQUIRE(top_block(print));
    const double lowest = lowest_curved_z(*object);
    INFO("lowest curved bead " << lowest);
    // The pin's top (Z 5.0) within 0.4 of the lowest curved bead.
    CHECK(lowest >= 5.0 - 0.4 - 1e-4);
    CHECK_NOTHROW(Test::gcode(print));
}

// Monotonic (the default): every line is laid next to the line before in one direction; with a hole in the
// patch nothing jumps back over lines already laid.
TEST_CASE("Curved tops P4: the monotonic pattern never jumps back, also round a hole", "[CurvedTops][CurvedTopsP4]")
{
    ExPolygon patch;
    patch.contour = Polygon::new_scale(std::vector<Vec2d>{ Vec2d(-10., -10.), Vec2d(10., -10.), Vec2d(10., 10.), Vec2d(-10., 10.) });
    Polygon hole = Polygon::new_scale(std::vector<Vec2d>{ Vec2d(-3., -3.), Vec2d(-3., 3.), Vec2d(3., 3.), Vec2d(3., -3.) });
    patch.holes.push_back(hole);
    const PrintConfig       pcfg;
    const PrintObjectConfig ocfg;
    const Flow              flow(0.42f, 0.2f, 0.4f);
    for (int k = 0; k < 2; ++k) {
        INFO("curved layer " << k);
        const std::vector<CurvedTopsFillPath> paths = curved_tops_fill(patch, ctpMonotonic, flow, k, pcfg, ocfg);
        REQUIRE(!paths.empty());
        // Direction of the lines: 45 or 135 degrees.
        const double a = (k % 2 == 0 ? 45. : 135.) * M_PI / 180.;
        const Vec2d  u(std::cos(a), std::sin(a)), v(-std::sin(a), std::cos(a));
        struct Seg { double lo, hi, pos; size_t order; };
        std::vector<Seg> segs;
        size_t order = 0;
        for (const CurvedTopsFillPath &p : paths)
            for (size_t i = 1; i < p.points.size(); ++i) {
                const Vec2d  p0 = unscale(p.points[i - 1]), p1 = unscale(p.points[i]);
                const Vec2d  d  = p1 - p0;
                const double len = d.norm();
                if (len < 1.0 || std::abs(d.dot(v)) > 0.02 * len)
                    continue; // a link along the edge, not a line
                const double s0 = p0.dot(u), s1 = p1.dot(u);
                segs.push_back({ std::min(s0, s1), std::max(s0, s1), 0.5 * (p0 + p1).dot(v), order++ });
            }
        REQUIRE(segs.size() > 20);
        const double spacing = flow.spacing();
        int    direction = 0;
        size_t pairs = 0, jumps = 0;
        for (size_t i = 0; i < segs.size(); ++i)
            for (size_t j = i + 1; j < segs.size(); ++j) {
                const Seg &A = segs[i], &B = segs[j];
                const double gap = std::abs(B.pos - A.pos);
                if (gap < 0.5 * spacing || gap > 1.5 * spacing || std::min(A.hi, B.hi) - std::max(A.lo, B.lo) < 0.5)
                    continue; // not neighbours
                ++pairs;
                const int d = B.pos > A.pos ? 1 : -1;
                if (direction == 0)
                    direction = d;
                else if (d != direction)
                    ++jumps;
            }
        CHECK(pairs > 20);
        CHECK(jumps == 0);
        // Every point inside the patch.
        const ExPolygon grown = offset_ex(patch, scale_(0.01)).front();
        for (const CurvedTopsFillPath &p : paths)
            for (const Point &pt : p.points)
                CHECK(grown.contains(pt));
    }
}

// Concentric: loops covering the patch, the centre neither left open nor over-filled (the extruded volume
// matches the area within a few percent), every point on the patch, and the loop starts move between layers.
TEST_CASE("Curved tops P4: the concentric pattern covers the patch", "[CurvedTops][CurvedTopsP4]")
{
    const PrintConfig       pcfg;
    const PrintObjectConfig ocfg;
    const Flow              flow(0.42f, 0.2f, 0.4f);
    // A disc of radius 7.3 (not a whole number of loops) and an annulus round a 2 mm hole.
    for (int shape = 0; shape < 2; ++shape) {
        INFO("shape " << shape);
        ExPolygon patch;
        Points    pts;
        for (int i = 0; i < 180; ++i) {
            const double t = 2. * M_PI * i / 180.;
            pts.push_back(Point::new_scale(7.3 * std::cos(t), 7.3 * std::sin(t)));
        }
        patch.contour = Polygon(pts);
        if (shape == 1) {
            Points hp;
            for (int i = 0; i < 90; ++i) {
                const double t = -2. * M_PI * i / 90.;
                hp.push_back(Point::new_scale(2. * std::cos(t), 2. * std::sin(t)));
            }
            patch.holes.push_back(Polygon(hp));
        }
        const double area_mm2 = patch.area() * SCALING_FACTOR * SCALING_FACTOR;
        std::vector<int> seams;
        for (int k = 0; k < 3; ++k) {
            const std::vector<CurvedTopsFillPath> paths = curved_tops_fill(patch, ctpConcentric, flow, k, pcfg, ocfg);
            REQUIRE(paths.size() >= 5);
            double   volume = 0.;
            Polygons strokes;
            const ExPolygon grown = offset_ex(patch, scale_(0.05)).front();
            size_t outside = 0;
            for (const CurvedTopsFillPath &p : paths) {
                const Polyline pl(p.points);
                volume += unscale<double>(pl.length()) * p.mm3_per_mm;
                const double spacing = p.mm3_per_mm / 0.2;
                polygons_append(strokes, offset(pl, float(scale_(0.5 * spacing + 0.01))));
                for (const Point &pt : p.points)
                    outside += grown.contains(pt) ? 0 : 1;
            }
            CHECK(outside == 0);
            const double fill_ratio = volume / 0.2 / area_mm2;
            const double covered    = area(intersection_ex(union_ex(strokes), ExPolygons{ patch })) * SCALING_FACTOR * SCALING_FACTOR / area_mm2;
            INFO("layer " << k << ": volume / area " << fill_ratio << ", covered " << covered);
            CHECK(fill_ratio > 0.94);
            CHECK(fill_ratio < 1.06);
            CHECK(covered > 0.97);
            seams.push_back(int(std::lround(std::atan2(double(paths.front().points.front().y()), double(paths.front().points.front().x())) * 180. / M_PI)));
        }
        // The outer loop's start turns from layer to layer.
        CHECK(seams[0] != seams[1]);
        CHECK(seams[1] != seams[2]);
    }
}

namespace {

// A spherical cap of sphere radius rs over a square of half side `half`, as a height field on a regular grid
// of cell `cell` (each cell split along the same diagonal, so the facet rows run straight while the cap's
// iso-slope lines are circles), standing on vertical sides from Z 0.
TriangleMesh make_grid_cap(double half, double cell, double rs, double base)
{
    const int n = int(std::lround(2. * half / cell));
    auto      z = [&](double x, double y) { return base + std::sqrt(rs * rs - x * x - y * y) - rs; };
    indexed_triangle_set its;
    auto top = [n](int i, int j) { return i * (n + 1) + j; };
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n; ++j) {
            const double x = -half + cell * i, y = -half + cell * j;
            its.vertices.emplace_back(float(x), float(y), float(z(x, y)));
        }
    const int bottom0 = int(its.vertices.size());
    for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n; ++j)
            its.vertices.emplace_back(float(-half + cell * i), float(-half + cell * j), 0.f);
    auto bot = [n, bottom0](int i, int j) { return bottom0 + i * (n + 1) + j; };
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            its.indices.emplace_back(top(i, j), top(i + 1, j), top(i + 1, j + 1));
            its.indices.emplace_back(top(i, j), top(i + 1, j + 1), top(i, j + 1));
            its.indices.emplace_back(bot(i, j), bot(i + 1, j + 1), bot(i + 1, j));
            its.indices.emplace_back(bot(i, j), bot(i, j + 1), bot(i + 1, j + 1));
        }
    // Sides: (i, 0) row, (i, n) row, (0, j), (n, j).
    for (int i = 0; i < n; ++i) {
        its.indices.emplace_back(bot(i, 0), bot(i + 1, 0), top(i + 1, 0));
        its.indices.emplace_back(bot(i, 0), top(i + 1, 0), top(i, 0));
        its.indices.emplace_back(bot(i + 1, n), bot(i, n), top(i, n));
        its.indices.emplace_back(bot(i + 1, n), top(i, n), top(i + 1, n));
        its.indices.emplace_back(bot(0, i + 1), bot(0, i), top(0, i));
        its.indices.emplace_back(bot(0, i + 1), top(0, i), top(0, i + 1));
        its.indices.emplace_back(bot(n, i), bot(n, i + 1), top(n, i + 1));
        its.indices.emplace_back(bot(n, i), top(n, i + 1), top(n, i));
    }
    return TriangleMesh(std::move(its));
}

// Pairs of significant turns (more than 5 degrees) of opposite sign closer together than `min_mm` along the
// outline: the teeth of a sawtooth edge.
size_t short_reversals(const Polygon &outline, double min_mm)
{
    const Points &p = outline.points;
    const size_t  n = p.size();
    struct Turn { double at; int sign; };
    std::vector<Turn> turns;
    double            at = 0.;
    for (size_t i = 0; i < n; ++i) {
        const Vec2d a = unscale(p[(i + n - 1) % n]), b = unscale(p[i]), c = unscale(p[(i + 1) % n]);
        const Vec2d d0 = b - a, d1 = c - b;
        const double turn = std::atan2(d0.x() * d1.y() - d0.y() * d1.x(), d0.dot(d1)) * 180. / M_PI;
        if (std::abs(turn) > 5.)
            turns.push_back({ at, turn > 0. ? 1 : -1 });
        at += d1.norm();
    }
    size_t out = 0;
    for (size_t i = 0; i + 1 < turns.size(); ++i)
        if (turns[i].sign != turns[i + 1].sign && turns[i + 1].at - turns[i].at < min_mm)
            ++out;
    return out;
}

} // namespace

// The owner's sawtooth: a cap whose facet rows run straight while its iso-slope line is a circle. P3's edge
// (whole facets) is a staircase along the rows; P4's runs smoothly across them, stays within the stated
// tolerances of the limit, and keeps a sharp rim (a cap on a cylinder) where it was.
TEST_CASE("Curved tops P4: the patch edge is smooth and never reaches steeper surface beyond the tolerance", "[CurvedTops][CurvedTopsP4]")
{
    // Sphere radius 60, 10 degree limit: the iso-slope circle has radius 60 sin 10 = 10.42; 0.75 mm cells.
    const double rs = 60., limit = 10.;
    const TriangleMesh     cap = make_grid_cap(15., 0.75, rs, 6.);
    const sla::IndexedMesh imesh(cap);
    const CurvedTopsCandidates smooth = curved_tops_candidate_surfaces(cap.its, imesh, limit, imesh.ground_level());
    const CurvedTopsCandidates whole  = curved_tops_candidate_surfaces(cap.its, imesh, limit, imesh.ground_level(), false);
    REQUIRE(smooth.pieces.size() == 1);
    REQUIRE(whole.pieces.size() == 1);
    const size_t teeth_smooth = short_reversals(smooth.pieces.front().contour, 1.0);
    const size_t teeth_whole  = short_reversals(whole.pieces.front().contour, 1.2);
    INFO("short reversals: smooth " << teeth_smooth << ", whole facets " << teeth_whole);
    CHECK(teeth_whole > 10); // the defect the test is about
    CHECK(teeth_smooth == 0);

    // Never beyond the tolerances: the facets used slope at most limit + CURVED_TOPS_EDGE_ANGLE_TOLERANCE_DEG, and the
    // edge lies within CURVED_TOPS_EDGE_SIMPLIFY_MM of the circle where the surface reaches that slope.
    for (const CurvedTopsFacet &f : smooth.facets)
        CHECK(f.slope_deg <= limit + CURVED_TOPS_EDGE_ANGLE_TOLERANCE_DEG + 1e-6);
    const double r_tol = rs * std::sin((limit + CURVED_TOPS_EDGE_ANGLE_TOLERANCE_DEG) * M_PI / 180.) + CURVED_TOPS_EDGE_SIMPLIFY_MM;
    double r_min = 1e9, r_max = 0.;
    for (const Point &pt : smooth.pieces.front().contour.points) {
        const double r = unscale<double>(pt.cast<double>().norm());
        r_min = std::min(r_min, r);
        r_max = std::max(r_max, r);
    }
    INFO("edge radius " << r_min << " to " << r_max << ", limit circle " << rs * std::sin(limit * M_PI / 180.));
    CHECK(r_max <= r_tol);
    // And it follows the limit circle closely (the facets interpolate the slope, so within about a cell).
    CHECK(r_min > rs * std::sin(limit * M_PI / 180.) - 0.75);
    CHECK(r_max < rs * std::sin(limit * M_PI / 180.) + 0.75);

    // A cap on a cylinder keeps its rim: the vertical wall beyond the crease does not tilt the rim's slope.
    const TriangleMesh         puck = make_capped_puck(15., 4., 1.2);
    const sla::IndexedMesh     pmesh(puck);
    const CurvedTopsCandidates top  = curved_tops_candidate_surfaces(puck.its, pmesh, 15., pmesh.ground_level());
    REQUIRE(top.pieces.size() == 1);
    const double puck_area = top.pieces.front().area() * SCALING_FACTOR * SCALING_FACTOR;
    CHECK(puck_area > 0.97 * M_PI * 15. * 15.);
    CHECK(puck_area < 1.01 * M_PI * 15. * 15.);
}

// Both patterns on a real slice: a curved block with every sample at a finite height within the cap, and a
// clean export.
TEST_CASE("Curved tops P4: both patterns slice and export", "[CurvedTops][CurvedTopsP4]")
{
    const ResourcesDirGuard resources;
    const std::string pattern = GENERATE(std::string("monotonic"), std::string("concentric"));
    INFO("curved_tops_pattern = " << pattern);
    Print print;
    Model model;
    slice_puck(print, model, true, { { "curved_tops_pattern", pattern } });
    const PrintObject *object = print.objects().front();
    REQUIRE(top_block(print));
    const std::vector<Vec3d> samples = curved_samples(*object);
    REQUIRE(samples.size() > 100);
    for (const Vec3d &s : samples) {
        CHECK(std::isfinite(s.z()));
        CHECK(s.z() <= 5.2 + 1e-3);
        CHECK(s.z() >= 4.0 - 3 * 0.2 - 1e-3);
    }
    CHECK_NOTHROW(Test::gcode(print));
}

// Review M2: the tolerance in force is the setting capped by the head's printed record. A head with no nozzle
// contact record (a synthetic copy of the Centauri Carbon's without it) is strict (P3's planner rules, said in the
// summary) unless the developer override lifts it. The Centauri Carbon records 0.61 mm: a larger setting is capped
// and said; the X1 Carbon records 0.44 mm since the owner's 07e print.
TEST_CASE("Curved tops P4: the nozzle tolerance is capped by the print head's printed record", "[CurvedTops][CurvedTopsP4]")
{
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("curvedtops_norecord_%%%%-%%%%");
    boost::filesystem::create_directories(dir / "head_clearance");
    {
        std::ofstream f((dir / "head_clearance" / "no-record.json").string());
        f << R"({
    "schema": "edgeslicer.head_clearance.v1",
    "id": "no-record-test",
    "name": "Head with no contact record",
    "printer_models": ["Elegoo Centauri Carbon"],
    "calibrated": false,
    "source": "unit test",
    "nozzle": { "profile": [[0.0, 0.5], [1.0, 1.3], [2.0, 2.1], [4.0, 3.5]] },
    "boxes": [ {"name": "heater block and silicone sock", "min": [-12, -12, 2.5], "max": [12, 12, 24]} ],
    "curved_tops": { "verified_printer_models": ["Elegoo Centauri Carbon"], "verified_rise_mm": 2.37, "verified_angle_deg": 20 }
})";
    }
    {
        const ResourcesDirGuard none(dir.string());
        {
            Print print;
            Model model;
            slice_puck(print, model, true, { { "curved_tops_max_angle", "5" } });
            REQUIRE(top_block(print));
            const std::string summary = object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsSummary);
            INFO("summary: " << summary);
            CHECK(summary.find("Nozzle contact tolerance not applied: no contact print recorded for this print head (the setting asks for 0.40 mm)") !=
                  std::string::npos);
            // Strict: the planner works as with tolerance 0 (see "material beside within the nozzle tolerance").
            CHECK(summary.find("Partly curved: below Z 4.80 mm") != std::string::npos);
            CHECK_NOTHROW(Test::gcode(print));
            CHECK(print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck).find("The nozzle passes") == std::string::npos);
        }
        {
            const DevModeGuard dev(true);
            Print print;
            Model model;
            slice_puck(print, model, true, { { "curved_tops_max_angle", "5" }, { "curved_tops_dev_nozzle_tolerance_unrecorded", "1" } });
            REQUIRE(top_block(print));
            const std::string summary = object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsSummary);
            INFO("summary: " << summary);
            CHECK(summary.find("Developer override: nozzle contact tolerance 0.40 mm applied although no nozzle contact print is recorded") !=
                  std::string::npos);
            CHECK(summary.find("Partly curved") == std::string::npos);
            CHECK_NOTHROW(Test::gcode(print));
            const std::string check = print_warning(print, PrintStateBase::SlicingCurvedTopsHeadCheck);
            INFO("head check: " << check);
            CHECK(check.find("No nozzle contact has been printed with this print head yet") != std::string::npos);
        }
    }
    {
        const ResourcesDirGuard resources;
        {
            Print print;
            Model model;
            slice_puck(print, model, true, { { "curved_tops_nozzle_tolerance", "0.8" } });
            REQUIRE(top_block(print));
            const std::string summary = object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsSummary);
            INFO("summary: " << summary);
            CHECK(summary.find("Nozzle contact tolerance capped at 0.61 mm") != std::string::npos);
            CHECK(summary.find("the setting asks for 0.80 mm") != std::string::npos);
        }
        {
            // At or under the record: nothing said; the X1 Carbon now has a record too.
            for (const char *printer : { "Elegoo Centauri Carbon", "Bambu Lab X1 Carbon" }) {
                INFO(printer);
                Print print;
                Model model;
                slice_puck(print, model, true, { { "printer_model", printer } });
                REQUIRE(top_block(print));
                CHECK(object_warning(*print.objects().front(), PrintStateBase::SlicingCurvedTopsSummary).find("Nozzle contact tolerance") ==
                      std::string::npos);
            }
        }
    }
    boost::system::error_code ec;
    boost::filesystem::remove_all(dir, ec);
}

// Review S2: on a long narrow loop the starts of consecutive layers and loops spread along its length instead of
// landing on its two ends.
TEST_CASE("Curved tops P4: loop starts spread along a long loop", "[CurvedTops][CurvedTopsP4]")
{
    Points loop;
    for (double x = -20.; x < 20.; x += 0.5) loop.push_back(Point::new_scale(x, -2.));
    for (double y = -2.; y < 2.; y += 0.5) loop.push_back(Point::new_scale(20., y));
    for (double x = 20.; x > -20.; x -= 0.5) loop.push_back(Point::new_scale(x, 2.));
    for (double y = 2.; y > -2.; y -= 0.5) loop.push_back(Point::new_scale(-20., y));
    size_t middle = 0;
    std::vector<double> xs;
    for (int k = 0; k < 6; ++k)
        for (int index = -1; index <= 1; ++index) {
            const double x = unscale<double>(loop[curved_tops_seam_index(loop, Point(0, 0), k, index)].x());
            xs.push_back(x);
            middle += std::abs(x) < 15. ? 1 : 0;
        }
    std::sort(xs.begin(), xs.end());
    INFO("starts at x from " << xs.front() << " to " << xs.back() << ", " << middle << " of 18 away from the ends");
    CHECK(middle >= 9);
    // And the same loop with another first point gives the same starts (a fixed anchor).
    Points rotated = loop;
    std::rotate(rotated.begin(), rotated.begin() + 37, rotated.end());
    for (int k = 0; k < 3; ++k)
        CHECK(rotated[curved_tops_seam_index(rotated, Point(0, 0), k, -1)] == loop[curved_tops_seam_index(loop, Point(0, 0), k, -1)]);
}

// Owner, 2026-11-17 (arm, Concentric): the concentric loops left the spine of each narrowing limb open. Every
// curved layer's beads (wall loop, fill, gap fill) must cover the patch: what stays open, slivers narrower than a
// quarter of the nozzle not counted, is under 1.5 % of the patch with Concentric (with gap fill) and under 3 % with
// Monotonic (no gap fill: the wedges at the line ends), on a tapered L (the arm in small) and on a dome's disc. The
// numbers are printed (WARN) for the report.
TEST_CASE("Curved tops P4: a curved layer's beads cover the patch, the spine of a narrowing limb included", "[CurvedTops][CurvedTopsP4]")
{
    const PrintConfig       pcfg;
    const PrintObjectConfig ocfg;
    const Flow              wall_flow(0.45f, 0.2f, 0.4f);
    const Flow              solid_flow(0.42f, 0.2f, 0.4f);
    const double            min_width = curved_tops_min_gap_width(pcfg);
    CHECK_THAT(min_width, WithinAbs(0.1, 1e-9));
    ExPolygon l_shape;
    l_shape.contour = Polygon::new_scale(std::vector<Vec2d>{ Vec2d(0., 0.), Vec2d(40., 2.), Vec2d(40., 5.), Vec2d(7., 8.), Vec2d(5., 30.), Vec2d(0., 30.) });
    ExPolygon disc;
    for (int i = 0; i < 180; ++i) {
        const double t = 2. * M_PI * i / 180.;
        disc.contour.points.push_back(Point::new_scale(20. * std::cos(t), 20. * std::sin(t)));
    }
    for (int shape = 0; shape < 2; ++shape)
        for (const CurvedTopsPattern pattern : { ctpMonotonic, ctpConcentric })
            for (int k = 0; k < 3; ++k) {
                const ExPolygon &fp = shape == 0 ? l_shape : disc;
                bool failed = false;
                const std::vector<CurvedTopsFillPath> paths = curved_tops_layer_paths(fp, pattern, wall_flow, solid_flow, k, pcfg, ocfg, &failed);
                const double patch    = fp.area() * SCALING_FACTOR * SCALING_FACTOR;
                const double open     = curved_tops_uncovered_mm2(fp, paths, 0.2, min_width);
                WARN((shape == 0 ? "tapered L" : "disc") << ", " << (pattern == ctpMonotonic ? "monotonic" : "concentric") << ", layer " << k
                                                         << ": " << paths.size() << " paths, open " << open << " mm2 of " << patch << " mm2 ("
                                                         << 100. * open / patch << " %)");
                CHECK_FALSE(failed);
                CHECK(open < (pattern == ctpConcentric ? 0.015 : 0.03) * patch);
                // Every bead inside the patch.
                const ExPolygon grown = offset_ex(fp, scale_(0.01)).front();
                size_t outside = 0;
                for (const CurvedTopsFillPath &p : paths)
                    for (const Point &pt : p.points)
                        outside += grown.contains(pt) ? 0 : 1;
                CHECK(outside == 0);
            }
}
