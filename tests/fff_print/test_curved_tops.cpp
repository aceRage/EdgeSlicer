// EdgeSlicer SPIKE: curved tops (src/libslic3r/CurvedTops.cpp).
//
//  * OFF (the default): no conformal path anywhere and the extruded Z never goes back down.
//  * ON, a 30 mm puck with a 1.2 mm spherical cap: conformal paths are appended to the top layer
//    and, after the last planar layer, the G-code extrudes below that layer's Z (the curved
//    layers follow the cap down to its rim).
//  * The pure helpers: head clearance, effective angle, first-layer gap flow.

#include <catch2/catch.hpp>

#include <algorithm>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/ContourZ.hpp"
#include "libslic3r/CurvedTops.hpp"
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

void slice_puck(Print &print, Model &model, bool curved_tops,
                std::initializer_list<ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "sparse_infill_density",      "15%" },
        { "enable_support",             "0" },
        { "curved_tops_enabled",        curved_tops ? "1" : "0" },
        { "curved_tops_layers",         "3" },
        { "curved_tops_max_angle",      "15" },
    });
    config.set_deserialize_strict(extra);
    Test::init_print(std::vector<TriangleMesh>{ make_capped_puck(15., 4., 1.2) }, print, model, config);
    print.process();
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
    double min_clearance = 1e9;
};

CurvedTravels check_curved_travels(const std::string &gcode, double radius, double margin)
{
    struct Move { char kind; double x0, y0, z0, x1, y1, z1; };   // kind: 'e' extrude, 'A' arc extrude, 't' travel, 'w' wipe, 'a' arc, 'z' other
    std::vector<Move> moves;
    {
        std::istringstream in(gcode);
        std::string        line;
        double x = 0., y = 0., z = 0.;
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
            const bool xy = nx != x || ny != y;
            const char kind = arc ? (e > 0. ? 'A' : 'a') : (e > 0. && xy) ? 'e' : ((e < 0. || in_wipe) && xy) ? 'w' : xy ? 't' : 'z';
            moves.push_back({ kind, x, y, z, nx, ny, nz });
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

TEST_CASE("Curved tops: a gentle cap is printed as conformal layers", "[CurvedTops]")
{
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
                    << ", min clearance " << t.min_clearance << " mm, max travel Z " << t.max_travel_z);
    CHECK(t.travels >= 5);
    CHECK(t.slanted == 0);
    CHECK(t.arcs == 0);
    CHECK(t.under_margin == 0);
    CHECK(t.min_clearance >= 0.5);
    CHECK(t.max_travel_z < cfg.opt_float("printable_height"));

    // One layer change and one Z_HEIGHT per layer; progress lines still there.
    const size_t layers = print.objects().front()->layers().size();
    size_t change_layer = 0, z_height = 0, m73 = 0;
    std::istringstream in(gcode);
    for (std::string line; std::getline(in, line);) {
        change_layer += line.rfind("; CHANGE_LAYER", 0) == 0 || line.rfind(";LAYER_CHANGE", 0) == 0;
        z_height     += line.rfind("; Z_HEIGHT:", 0) == 0;
        m73          += line.rfind("M73 ", 0) == 0;
    }
    CHECK(change_layer == layers);
    CHECK(z_height == layers);
    CHECK(m73 > 0);
}

TEST_CASE("Curved tops: head clearance and flow helpers", "[CurvedTops]")
{
    PrintConfig cfg;
    const CurvedTopsHeadClearance generic = curved_tops_head_clearance(cfg);
    cfg.printer_model.value = "Elegoo Centauri Carbon";
    const CurvedTopsHeadClearance cc1 = curved_tops_head_clearance(cfg);
    cfg.printer_model.value = "Elegoo Centauri Carbon 2";
    const CurvedTopsHeadClearance cc2 = curved_tops_head_clearance(cfg);
    cfg.printer_model.value = "Bambu Lab X1 Carbon";
    const CurvedTopsHeadClearance x1c = curved_tops_head_clearance(cfg);
    cfg.printer_model.value = "Bambu Lab X1E";
    const CurvedTopsHeadClearance x1e = curved_tops_head_clearance(cfg);

    CHECK_THAT(cc1.nozzle_height_mm, WithinAbs(2.0, 1e-9));
    CHECK_THAT(cc1.max_slope_deg, WithinAbs(20.0, 1e-9));
    CHECK_THAT(cc2.nozzle_height_mm, WithinAbs(generic.nozzle_height_mm, 1e-9));
    // X1 Carbon: owner measured 2.5 mm tip to sock, 0.5 mm margin; slope unmeasured (20). Not the X1E.
    CHECK_THAT(x1c.nozzle_height_mm, WithinAbs(2.0, 1e-9));
    CHECK_THAT(x1c.max_slope_deg, WithinAbs(20.0, 1e-9));
    CHECK_THAT(x1e.nozzle_height_mm, WithinAbs(generic.nozzle_height_mm, 1e-9));

    CHECK_THAT(curved_tops_effective_max_angle(15., cc1), WithinAbs(15., 1e-9));
    CHECK_THAT(curved_tops_effective_max_angle(35., cc1), WithinAbs(20., 1e-9));

    CHECK_THAT(curved_tops_gap_flow(0.2, 0.2), WithinAbs(1.0, 1e-9));
    CHECK_THAT(curved_tops_gap_flow(0.3, 0.2), WithinAbs(1.5, 1e-9));
    CHECK_THAT(curved_tops_gap_flow(0.1, 0.2), WithinAbs(1.0, 1e-9));
    CHECK_THAT(curved_tops_gap_flow(0.9, 0.2), WithinAbs(2.0, 1e-9));
}
