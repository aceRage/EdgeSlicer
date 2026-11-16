// PR-C: the tower side of AdaptiveLayerMerge (Bambu Studio 2.9.1 WipeTower.cpp 5222-7600). A Type 1 tower planned directly
// on a mixed-diameter plate: every nozzle's purge lies on its own slab ladder, layers a coarse nozzle merges over are left
// out, and nothing is extruded outside a nozzle's printable window. A tower that is never told about mixed diameters (a
// single-diameter plate) is planned exactly as before.

#include <catch2/catch.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cmath>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

double r3(double v) { return std::round(v * 1000.) / 1000.; }

// One extrusion move of a tower result: which filament laid it, at which Z, how thick.
struct TowerExtrusion
{
    int    tool;
    double z;
    double height;
    bool   purge; // inside the purge of a tool change ("; CP TOOLCHANGE WIPE" .. "; CP TOOLCHANGE END")
};

// The extruding moves of one tower result. The G-code before "[change_filament_gcode]" runs with the outgoing filament, the
// rest with the incoming one; the nozzle change (ramming) is the outgoing filament's. Heights come from the HEIGHT tags.
void collect_extrusions(const std::string &gcode, int tool_before, int tool_after, double z, double height, std::vector<TowerExtrusion> &out)
{
    static const std::regex height_re(R"(HEIGHT:\s*([0-9.]+))");
    static const std::regex e_re(R"(\bE([0-9.]+))");
    std::istringstream      in(gcode);
    std::string             line;
    int                     tool  = tool_before;
    bool                    purge = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (line.find("[change_filament_gcode]") != std::string::npos) {
            tool = tool_after;
            continue;
        }
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos)
            purge = true;
        else if (line.find("; CP TOOLCHANGE END") != std::string::npos)
            purge = false;
        if (line.size() > 0 && line[0] == ';' && std::regex_search(line, m, height_re)) {
            height = std::stod(m[1].str());
            continue;
        }
        if (line.rfind("G1 ", 0) == 0 && (line.find(" X") != std::string::npos || line.find(" Y") != std::string::npos) &&
            std::regex_search(line, m, e_re) && std::stod(m[1].str()) > 0.)
            out.push_back({ tool, z, r3(height), purge });
    }
}

std::vector<TowerExtrusion> tower_extrusions(const std::vector<std::vector<WipeTower::ToolChangeResult>> &layers)
{
    std::vector<TowerExtrusion> out;
    for (const auto &layer : layers)
        for (const WipeTower::ToolChangeResult &tcr : layer) {
            if (tcr.combined_away)
                continue;
            collect_extrusions(tcr.nozzle_change_result.gcode, tcr.initial_tool, tcr.initial_tool, tcr.print_z, tcr.layer_height, out);
            collect_extrusions(tcr.gcode, tcr.initial_tool, tcr.new_tool, tcr.print_z, tcr.layer_height, out);
        }
    return out;
}

// A Bambu Lab-like two-nozzle printer, one filament per nozzle.
PrintConfig two_nozzle_config(const char *diameters)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_num_extruders(2);
    cfg.set_num_filaments(2);
    cfg.set_deserialize_strict({
        { "nozzle_diameter", diameters },
        { "filament_diameter", "1.75,1.75" },
        { "filament_map", "1,2" },
        { "single_extruder_multi_material", "0" },
        { "enable_prime_tower", "1" },
        { "prime_tower_width", "35" },
        { "wipe_tower_rotation_angle", "0" },
        { "filament_max_volumetric_speed", "20,20" },
    });
    PrintConfig config;
    config.apply(cfg, true);
    return config;
}

struct PlannedTower
{
    PrintConfig                                           config;
    std::unique_ptr<WipeTower>                            tower;
    std::vector<std::vector<WipeTower::ToolChangeResult>> result;
};

// `layers` layers of `base`. The fine filament 0 prints every layer, the coarse filament 1 every `band`-th one (layer 0, then
// the band tops), as ToolOrdering lists a coarse nozzle on a mixed-diameter plate: a change to 1 on its layers, back to 0 on
// the next one, no change in between.
std::unique_ptr<PlannedTower> plan(const char *diameters, double base, int layers, int band, const std::vector<WipeTower::NozzleLadder> *ladders)
{
    auto t    = std::make_unique<PlannedTower>();
    t->config = two_nozzle_config(diameters);
    t->tower  = std::make_unique<WipeTower>(t->config, 0, Vec3d::Zero(), 15.f, 0, float(base * layers));
    if (ladders != nullptr)
        t->tower->set_multi_diameter_print(float(base), *ladders);
    t->tower->set_extruder(0, t->config);
    t->tower->set_extruder(1, t->config);
    int loaded = 0;
    for (int layer = 0; layer < layers; ++layer) {
        const float z = float(base * (layer + 1));
        t->tower->plan_toolchange(z, float(base), unsigned(loaded), unsigned(loaded));
        if (layer % band == 0) {
            if (loaded != 0) {
                t->tower->plan_toolchange(z, float(base), unsigned(loaded), 0u, 30.f, 0.f);
                loaded = 0;
            }
            t->tower->plan_toolchange(z, float(base), 0u, 1u, 30.f, 0.f);
            loaded = 1;
        } else if (loaded == 1) {
            t->tower->plan_toolchange(z, float(base), 1u, 0u, 30.f, 0.f);
            loaded = 0;
        }
    }
    t->tower->generate(t->result);
    return t;
}

// The coarse filament's purge on a layer: the result that changes to it.
const WipeTower::ToolChangeResult *change_to(const std::vector<WipeTower::ToolChangeResult> &layer, int tool)
{
    for (const WipeTower::ToolChangeResult &tcr : layer)
        if (tcr.new_tool == tool && tcr.initial_tool != tool && !tcr.combined_away)
            return &tcr;
    return nullptr;
}

// The heights the purge of `tool` is laid at on the layer at z. A visit merges the wall and the band fills into the tool
// change (merge_tcr), so the result's layer_height is that of its first part; the purge's own HEIGHT tags are what count.
std::set<double> purge_heights(const std::vector<TowerExtrusion> &extrusions, int tool, double z)
{
    std::set<double> out;
    for (const TowerExtrusion &e : extrusions)
        if (e.purge && e.tool == tool && std::abs(e.z - z) < 1e-4)
            out.insert(e.height);
    return out;
}

void check_windows(const std::vector<TowerExtrusion> &extrusions, const std::vector<WipeTower::NozzleLadder> &ladders)
{
    for (const TowerExtrusion &e : extrusions) {
        CAPTURE(e.tool, e.z, e.height);
        REQUIRE(e.tool >= 0);
        REQUIRE(size_t(e.tool) < ladders.size());
        const WipeTower::NozzleLadder &w = ladders[e.tool];
        // Bambu Studio's bridge flow (need_thick_bridge_flow()): purge rows over a sparse band below are tagged 0.2 mm, laid
        // by a nozzle whose window ends below that. Only purge rows, and only that one height.
        if (e.purge && w.max_height < 0.2f && std::abs(e.height - 0.2) < 1e-6)
            continue;
        CHECK(e.height + 1e-3 >= w.min_height);
        CHECK(e.height <= w.max_height + 1e-3);
    }
}

} // namespace

TEST_CASE("ALM tower, direct: 0.4 + 0.8 at 0.2 mm - the 0.8 purges on its 0.4 mm slabs", "[ALMTower]")
{
    const std::vector<WipeTower::NozzleLadder> ladders{ { 0.08f, 0.28f, 1 }, { 0.16f, 0.56f, 2 } };
    const auto                                 t = plan("0.4,0.8", 0.2, 12, 2, &ladders);
    REQUIRE(t->tower->multi_diameter_print());
    CHECK(t->tower->merged_slabs_active());
    REQUIRE(t->result.size() == 12);

    // One band per nozzle (Bambu Studio's (category, diameter, lane) block key), the same category.
    CHECK(t->tower->blocks().size() >= 2);

    const std::vector<TowerExtrusion> extrusions = tower_extrusions(t->result);
    for (size_t layer = 0; layer < 12; layer += 2) {
        CAPTURE(layer);
        REQUIRE(change_to(t->result[layer], 1) != nullptr);
        // Layer 0 rests on the bed at the first layer's height; every band top above it lays one 0.4 mm slab, the object's.
        CHECK(purge_heights(extrusions, 1, 0.2 * (layer + 1)) == std::set<double>{ layer == 0 ? 0.2 : 0.4 });
    }
    // The 0.4 nozzle purges at the base height.
    for (size_t layer = 1; layer < 12; layer += 2) {
        CAPTURE(layer);
        REQUIRE(change_to(t->result[layer], 0) != nullptr);
        CHECK(purge_heights(extrusions, 0, 0.2 * (layer + 1)) == std::set<double>{ 0.2 });
    }
    check_windows(extrusions, ladders);
}

TEST_CASE("ALM tower, direct: 0.2 + 0.8 at 0.1 mm - layers the 0.8 merges over are left out", "[ALMTower]")
{
    const std::vector<WipeTower::NozzleLadder> ladders{ { 0.04f, 0.14f, 1 }, { 0.16f, 0.56f, 4 } };
    // 13 layers, ending on a band top: above the last tool change a tower has nothing to lay down and generate() leaves
    // those layers out, as before.
    const auto                                 t = plan("0.2,0.8", 0.1, 13, 4, &ladders);
    REQUIRE(t->result.size() == 13);
    CHECK(t->tower->merged_slabs_active());
    const std::vector<TowerExtrusion> extrusions = tower_extrusions(t->result);

    for (size_t layer = 0; layer < 13; ++layer) {
        CAPTURE(layer);
        // The two layers without a tool change before each band top hold one material that the change above takes over:
        // the tower prints nothing there (Bambu Studio absorb_run_into_next_layer()), and the emitter leaves them out.
        const bool skipped = layer % 4 == 2 || layer % 4 == 3;
        CHECK(t->tower->test_layer_skipped(layer) == skipped);
        REQUIRE(!t->result[layer].empty());
        CHECK(t->result[layer].front().combined_away == skipped);
        if (layer % 4 == 0) {
            REQUIRE(change_to(t->result[layer], 1) != nullptr);
            // Layer 0 is the first layer, laid at its sliced height as the object's is (Edge never merges layer 0).
            CHECK(purge_heights(extrusions, 1, 0.1 * (layer + 1)) == std::set<double>{ layer == 0 ? 0.1 : 0.4 });
        }
    }
    // The tower reaches the top of the plan.
    CHECK_THAT(t->result.back().front().print_z, WithinAbs(1.3, 1e-4));
    // The first layer is every nozzle's sliced first layer (the object's too); above it nothing leaves its window.
    std::vector<TowerExtrusion> above_first;
    for (const TowerExtrusion &e : extrusions)
        if (e.z > 0.1 + 1e-4)
            above_first.push_back(e);
    check_windows(above_first, ladders);
}

TEST_CASE("ALM tower, direct: a tower never told about mixed diameters is planned as before", "[ALMTower]")
{
    // Print calls set_multi_diameter_print() only on a mixed-diameter plate; without it nothing merges, every extrusion is
    // at the layer height and the purges sit in one category block.
    const auto t = plan("0.4,0.4", 0.2, 8, 2, nullptr);
    CHECK_FALSE(t->tower->multi_diameter_print());
    CHECK_FALSE(t->tower->merged_slabs_active());
    REQUIRE(t->result.size() == 8);
    CHECK(t->tower->blocks().size() == 1);
    for (size_t layer = 0; layer < t->result.size(); ++layer) {
        CHECK_FALSE(t->tower->test_layer_skipped(layer));
        for (const WipeTower::ToolChangeResult &tcr : t->result[layer]) {
            CHECK_FALSE(tcr.combined_away);
            CHECK_THAT(tcr.layer_height, WithinAbs(0.2, 1e-4));
        }
    }
    for (const TowerExtrusion &e : tower_extrusions(t->result)) {
        CAPTURE(e.tool, e.z);
        CHECK((std::abs(e.height - 0.2) < 1e-6));
    }

    // And the same plan on mixed diameters without the call: no merge either (the gate is Print's, not the nozzle sizes).
    const auto mixed = plan("0.4,0.8", 0.2, 8, 2, nullptr);
    CHECK_FALSE(mixed->tower->merged_slabs_active());
    for (const auto &layer : mixed->result)
        for (const WipeTower::ToolChangeResult &tcr : layer)
            CHECK_FALSE(tcr.combined_away);
}

TEST_CASE("ALM tower: the footprint estimate keeps one band per nozzle at its slab", "[ALMTower]")
{
    // Two filaments in two bands, 0.2 + 0.8 at 0.1 mm: the 0.8's purge is laid on 0.4 mm slabs, a fraction of the depth the
    // same volume needs at 0.1 mm.
    std::vector<WipeTower::PurgeEstimate> plain{ { 30.f, 100 }, { 30.f, 200 } };
    std::vector<WipeTower::PurgeEstimate> mixed = plain;
    mixed[0].nozzle_diameter = 0.2f;
    mixed[0].line_width      = 0.4f;
    mixed[0].slab_height     = 0.1f;
    mixed[1].nozzle_diameter = 0.8f;
    mixed[1].line_width      = 0.8f;
    mixed[1].slab_height     = 0.4f;
    const float plain_depth = WipeTower::estimate_tower_blocks_depth(plain, 35.f, 0.1f, 0.2f, 1.f, 0.4f);
    const float mixed_depth = WipeTower::estimate_tower_blocks_depth(mixed, 35.f, 0.1f, 0.2f, 1.f, 0.4f);
    CHECK(mixed_depth > 0.f);
    // The coarse purge takes far less depth on its thick slabs than at the base height.
    CHECK(mixed_depth < plain_depth);
    // Without the per-nozzle fields the estimate is the one it always was.
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(plain, 35.f, 0.1f, 0.2f, 1.f, 0.4f), WithinAbs(plain_depth, 1e-6));
}

// ---- Review #1072 (h): geometry, not only tags ----------------------------------------------------------------------------

namespace {

// One extruding move of a tower result, in the tower frame.
struct TowerSegment
{
    int   layer;
    int   tool;
    Vec2f a, b;
    float width, height, e;
    bool  purge;
    bool  has_xy; // the move names X or Y (an extruding move without either lays its filament in one spot)
};

void collect_segments(const std::string &gcode, int layer, int tool_before, int tool_after, const Vec2f &start, const Vec2f &after_change,
                      float width, float height, std::vector<TowerSegment> &out)
{
    static const std::regex height_re(R"(HEIGHT:\s*([0-9.]+))");
    static const std::regex width_re(R"(WIDTH:\s*([0-9.]+))");
    static const std::regex word_re(R"(\b([XYE])(-?[0-9.]+))");
    std::istringstream      in(gcode);
    std::string             line;
    int                     tool  = tool_before;
    bool                    purge = false;
    Vec2f                   pos   = start;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (line.find("[change_filament_gcode]") != std::string::npos) {
            tool = tool_after;
            pos  = after_change;
            continue;
        }
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos)
            purge = true;
        else if (line.find("; CP TOOLCHANGE END") != std::string::npos)
            purge = false;
        if (!line.empty() && line[0] == ';') {
            if (std::regex_search(line, m, height_re))
                height = std::stof(m[1].str());
            else if (std::regex_search(line, m, width_re))
                width = std::stof(m[1].str());
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G2 ", 0) != 0 && line.rfind("G3 ", 0) != 0)
            continue;
        Vec2f next = pos;
        float e    = 0.f;
        bool  xy   = false;
        for (auto it = std::sregex_iterator(line.begin(), line.end(), word_re); it != std::sregex_iterator(); ++it) {
            const char  c = (*it)[1].str()[0];
            const float v = std::stof((*it)[2].str());
            if (c == 'X') { next.x() = v; xy = true; }
            else if (c == 'Y') { next.y() = v; xy = true; }
            else e = v;
        }
        if (e > 0.f)
            out.push_back({ layer, tool, pos, next, width, height, e, purge, xy });
        pos = next;
    }
}

std::vector<TowerSegment> tower_segments(const std::vector<std::vector<WipeTower::ToolChangeResult>> &layers, float base_width)
{
    std::vector<TowerSegment> out;
    for (size_t layer = 0; layer < layers.size(); ++layer)
        for (const WipeTower::ToolChangeResult &tcr : layers[layer]) {
            if (tcr.combined_away)
                continue;
            collect_segments(tcr.nozzle_change_result.gcode, int(layer), tcr.initial_tool, tcr.initial_tool, tcr.nozzle_change_result.start_pos,
                             tcr.nozzle_change_result.start_pos, base_width, tcr.layer_height, out);
            collect_segments(tcr.gcode, int(layer), tcr.initial_tool, tcr.new_tool, tcr.start_pos,
                             tcr.is_tool_change ? tcr.tool_change_start_pos : tcr.start_pos, base_width, tcr.layer_height, out);
        }
    return out;
}

Polygons segment_area(const TowerSegment &s)
{
    if ((s.b - s.a).norm() < 1e-4f)
        return {};
    const Polyline pl{ Point::new_scale(s.a.x(), s.a.y()), Point::new_scale(s.b.x(), s.b.y()) };
    return offset(pl, float(scale_(0.5 * s.width)));
}

// Every extrusion of the first `layers` printed tower layers that reaches below the top of the material laid on an earlier
// layer must not lie over it: the area where it does, in mm2.
double area_over_filled(const PlannedTower &t, const std::vector<TowerSegment> &segments, int layers)
{
    std::vector<int> printed;
    for (size_t l = 0; l < t.result.size() && int(printed.size()) < layers; ++l)
        if (!t.result[l].empty() && !t.result[l].front().combined_away)
            printed.push_back(int(l));
    double   over = 0.;
    Polygons below;
    float    below_top = 0.f;
    for (int layer : printed) {
        const float z = t.tower->test_plan_z(size_t(layer));
        Polygons    here;
        for (const TowerSegment &s : segments) {
            if (s.layer != layer || s.e <= 0.f)
                continue;
            const Polygons a = segment_area(s);
            if (!below.empty() && z - s.height < below_top - 1e-3f)
                over += area(intersection(a, below)) * SCALING_FACTOR * SCALING_FACTOR;
            append(here, a);
        }
        if (!here.empty()) {
            below     = union_(below, here);
            below_top = z;
        }
    }
    return over;
}

// The volume of filament laid into each band on each layer above the first, against what the band holds at the tallest
// height laid into it there (its interior width x its depth on that layer x that height). Largest ratio.
double worst_band_fill(const PlannedTower &t, const std::vector<TowerSegment> &segments, float filament_area, float tower_width)
{
    double worst = 0.;
    for (const WipeTower::WipeTowerBlock &block : t.tower->blocks()) {
        for (size_t layer = 1; layer < t.result.size(); ++layer) {
            if (layer >= block.layer_depths.size() || block.layer_depths[layer] <= 1.f)
                continue;
            const float y0 = block.start_depth, y1 = block.start_depth + block.layer_depths[layer];
            double      volume = 0., height = 0.;
            for (const TowerSegment &s : segments) {
                if (s.layer != int(layer) || s.e <= 0.f)
                    continue;
                const Vec2f mid = 0.5f * (s.a + s.b);
                // Interior rows only: the outer wall runs along x = 0 / width and the tower's front / back.
                if (mid.y() <= y0 || mid.y() >= y1 || mid.x() <= 1.f || mid.x() >= tower_width - 1.f)
                    continue;
                volume += double(s.e) * filament_area;
                height = std::max(height, double(s.height));
            }
            if (height <= 0.)
                continue;
            worst = std::max(worst, volume / (double(tower_width) * double(y1 - y0) * height));
        }
    }
    return worst;
}

} // namespace

TEST_CASE("ALM tower, direct: geometry - no row in one spot, nothing over the first layers, bands not over-filled", "[ALMTower]")
{
    const std::vector<WipeTower::NozzleLadder> ladders_48{ { 0.08f, 0.28f, 1 }, { 0.16f, 0.56f, 2 } };
    const std::vector<WipeTower::NozzleLadder> ladders_28{ { 0.04f, 0.14f, 1 }, { 0.16f, 0.56f, 4 } };
    for (int which = 0; which < 2; ++which) {
        CAPTURE(which);
        const auto t = which == 0 ? plan("0.4,0.8", 0.2, 12, 2, &ladders_48) : plan("0.2,0.8", 0.1, 13, 4, &ladders_28);
        const std::vector<TowerSegment> segments = tower_segments(t->result, 0.5f);
        REQUIRE(!segments.empty());
        // Every purge row names where it goes (a short E-only move is an unretract, not a row).
        for (const TowerSegment &s : segments) {
            CAPTURE(s.layer, s.tool, s.e);
            CHECK((!s.purge || s.has_xy || s.e < 3.f));
        }
        // Layers 1-2 of the tower: nothing is laid over the material already there.
        CHECK(area_over_filled(*t, segments, 2) < 0.5);
        // No band takes more than about what fits into it (lines overlap their neighbours a little).
        const float area = float(M_PI * 1.75 * 1.75 / 4.);
        CHECK(worst_band_fill(*t, segments, area, float(t->config.prime_tower_width.value)) < 1.3);
    }
}
