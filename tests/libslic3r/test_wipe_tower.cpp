#include <catch2/catch.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/WipeTower.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
namespace fs = boost::filesystem;

// A Bambu P1S project that reproduced the off-plate brim: two PLAs priming 30 and 45 mm3 in
// separate adhesiveness categories on a 35 mm tower, 0.21 mm layers, 0.4 nozzle (0.5 mm lines),
// 150 % infill gap (0.75 mm line pitch), rib width 8, 16 mm tall.
static std::vector<WipeTower::PurgeEstimate> cube_purges(int first_category = 100)
{
    return {{30.f, first_category}, {45.f, 0}};
}

TEST_CASE("Cone base polygon bulges past the body box", "[WipeTower]") {
    const Polygon box = WipeTower2::cone_base_polygon(35., 20., 100., 0.);
    CHECK(box.points.size() == 4);
    CHECK(get_extents(box).size() == Point::new_scale(Vec2d(35., 20.)));
    const Polygon     base = WipeTower2::cone_base_polygon(35., 20., 100., 25.);
    const BoundingBox bb   = get_extents(base);
    const double      R    = std::tan(25. / 2. * M_PI / 180.) * 100.;
    CHECK_THAT(unscaled(bb.min.y()), WithinAbs(10. - R, 0.1));
    CHECK_THAT(unscaled(bb.max.y()), WithinAbs(10. + R, 0.1));
    CHECK(diff(Polygons{box}, Polygons{base}).empty());
}

TEST_CASE("Type1 block-stack depth quantizes each purge to whole lines", "[WipeTower]") {
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(cube_purges(), 35.f, 0.21f, 0.4f, 1.5f), WithinAbs(18.5f, 0.01f));
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(cube_purges(0), 35.f, 0.21f, 0.4f, 1.5f), WithinAbs(11.0f, 0.01f));
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth({}, 35.f, 0.2f, 0.4f, 1.f), WithinAbs(0.f, 1e-6f));
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth({{45.f, 0}}, 0.9f, 0.2f, 0.4f, 1.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("A nozzle change adds its ramming lines to the block", "[WipeTower]") {
    std::vector<WipeTower::PurgeEstimate> purges{{100.f, 0}, {100.f, 0}};
    const float without_change = WipeTower::estimate_tower_blocks_depth(purges, 50.f, 0.2f, 0.4f, 1.f);
    purges.front().filament_change_length = 10.f;
    CHECK_THAT(WipeTower::estimate_tower_blocks_depth(purges, 50.f, 0.2f, 0.4f, 1.f) - without_change, WithinAbs(3.f, 1e-4f));
}

TEST_CASE("Rib tower footprint estimate covers the generated footprint", "[WipeTower]") {
    const float side = WipeTower::estimate_rib_tower_bbox_side(cube_purges(), 35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f);
    CHECK(side >= 29.56f);
    CHECK(side <= 29.56f + 4.f);
    CHECK(side >= WipeTower::estimate_rib_tower_bbox_side(cube_purges(0), 35.f, 0.21f, 0.4f, 1.5f, 8.f, 0.f, 16.f));
    CHECK_THAT(WipeTower::estimate_rib_tower_bbox_side({}, 35.f, 0.2f, 0.4f, 1.f, 8.f, 0.f, 16.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("Rib footprint extends the ribs, not the body, below the stability minimum", "[WipeTower]") {
    const float min_depth = WipeTower::get_limit_depth_by_height(90.f);
    REQUIRE(min_depth > 10.f);
    CHECK_THAT(WipeTower::rib_footprint_side(10.f, 10.f, 8.f, 0.f, 90.f), WithinAbs(min_depth + 5.f / std::sqrt(2.f), 1e-4f));
    const float plain = WipeTower::rib_footprint_side(30.f, 30.f, 8.f, 0.f, 5.f);
    CHECK_THAT(plain, WithinAbs(30.f + 8.f / std::sqrt(2.f), 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(30.f, 30.f, 8.f, 4.f, 5.f) - plain, WithinAbs(4.f / std::sqrt(2.f), 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(30.f, 30.f, 8.f, -4.f, 5.f), WithinAbs(plain, 1e-4f));
    CHECK_THAT(WipeTower::rib_footprint_side(0.f, 30.f, 8.f, 0.f, 5.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("Brim width estimate matches each generator's loop quantization", "[WipeTower]") {
    const float spacing = 0.5f - 0.2f * float(1. - M_PI_4);
    CHECK_THAT(WipeTower::estimate_brim_real_width(3.f, 0.4f, 0.2f, true), WithinAbs(7.f * spacing, 1e-4f));
    CHECK_THAT(WipeTower::estimate_brim_real_width(3.f, 0.4f, 0.2f, false), WithinAbs(7.5f * spacing, 1e-4f));
    CHECK_THAT(WipeTower::estimate_brim_real_width(0.f, 0.4f, 0.2f, true), WithinAbs(0.f, 1e-6f));
}

// ---------------------------------------------------------------------------------------------
// "No sparse layers": the compaction rule and the clearance it demands of the plate.
// ---------------------------------------------------------------------------------------------

// A square of side mm centred on (cx, cy), in bed coordinates.
static Polygon centered_square(double cx, double cy, double side)
{
    const double h = 0.5 * side;
    Polygon      poly;
    poly.points = {Point::new_scale(cx - h, cy - h), Point::new_scale(cx + h, cy - h),
                   Point::new_scale(cx + h, cy + h), Point::new_scale(cx - h, cy + h)};
    return poly;
}

static WipeTower::ToolChangeResult make_tcr(int initial_tool, int new_tool, float layer_height)
{
    WipeTower::ToolChangeResult tcr{};
    tcr.initial_tool = initial_tool;
    tcr.new_tool     = new_tool;
    tcr.layer_height = layer_height;
    return tcr;
}

// A 20 mm square tower at the bed origin, no spiral z-hop, so the keep-out zone is the bare
// footprint and every distance below is one the test sets.
static PrintConfig clearance_config()
{
    PrintConfig cfg;
    cfg.extruder_clearance_radius.value        = 40.;
    cfg.extruder_clearance_dist_to_rod.value   = 20.;
    cfg.extruder_clearance_height_to_rod.value = 25.;
    cfg.extruder_clearance_height_to_lid.value = 120.;
    cfg.nozzle_height.value                    = 5.;
    cfg.nozzle_diameter.values                 = {0.4};
    cfg.z_hop.values                           = {0.};
    cfg.travel_slope.values                    = {3.};
    return cfg;
}

TEST_CASE("Sparse layers are skipped only when nothing else needs a tower on every layer", "[WipeTower][NoSparseLayers]") {
    PrintConfig cfg;
    cfg.timelapse_type.value = TimelapseType::tlTraditional;

    cfg.wipe_tower_no_sparse_layers.value = false;
    CHECK_FALSE(wipe_tower_sparse_layers_skipped(cfg));
    cfg.wipe_tower_no_sparse_layers.value = true;
    CHECK(wipe_tower_sparse_layers_skipped(cfg));

    // Smooth timelapse parks the nozzle on the tower every layer, so no layer is ever dropped and the
    // option must read as off everywhere rather than compact in one place and not another.
    cfg.timelapse_type.value = TimelapseType::tlSmooth;
    CHECK_FALSE(wipe_tower_sparse_layers_skipped(cfg));
}

TEST_CASE("A planned layer is sparse only when its single tool change keeps the filament", "[WipeTower][NoSparseLayers]") {
    CHECK(wipe_tower_layer_is_sparse({make_tcr(1, 1, 0.2f)}));
    CHECK_FALSE(wipe_tower_layer_is_sparse({make_tcr(0, 1, 0.2f)}));
    // A second entry means the layer carries real work whatever the tools are.
    CHECK_FALSE(wipe_tower_layer_is_sparse({make_tcr(1, 1, 0.2f), make_tcr(1, 1, 0.2f)}));
    CHECK_FALSE(wipe_tower_layer_is_sparse({}));
}

TEST_CASE("The compacted tower falls one layer height behind the object per sparse layer", "[WipeTower][NoSparseLayers]") {
    // Five 0.2 mm layers off a 0.1 mm z offset, the middle two sparse. The object reaches
    // 0.1 + 5 * 0.2 = 1.1; the tower only grows on the three printed layers, so it ends at
    // 0.1 + 3 * 0.2 = 0.7 and a sparse layer carries the previous value rather than its own.
    const std::vector<std::vector<WipeTower::ToolChangeResult>> tool_changes{
        {make_tcr(0, 1, 0.2f)}, {make_tcr(1, 1, 0.2f)}, {make_tcr(1, 1, 0.2f)},
        {make_tcr(1, 0, 0.2f)}, {make_tcr(0, 1, 0.2f)}};

    const std::vector<float> tower_z = compute_compacted_wipe_tower_z(tool_changes, 0.1f);
    REQUIRE(tower_z.size() == tool_changes.size());
    CHECK_THAT(tower_z[0], WithinAbs(0.3f, 1e-5f));
    CHECK_THAT(tower_z[1], WithinAbs(0.3f, 1e-5f));
    CHECK_THAT(tower_z[2], WithinAbs(0.3f, 1e-5f));
    CHECK_THAT(tower_z[3], WithinAbs(0.5f, 1e-5f));
    CHECK_THAT(tower_z[4], WithinAbs(0.7f, 1e-5f));
    CHECK_THAT(1.1f - tower_z.back(), WithinAbs(2 * 0.2f, 1e-5f));

    // Without a base the tower starts at the bed, and an empty layer carries over like a sparse one.
    const std::vector<float> no_offset = compute_compacted_wipe_tower_z({{make_tcr(0, 1, 0.2f)}, {}}, 0.f);
    CHECK_THAT(no_offset[0], WithinAbs(0.2f, 1e-5f));
    CHECK_THAT(no_offset[1], WithinAbs(0.2f, 1e-5f));
}

TEST_CASE("The tower keep-out zone grows by the spiral z-hop envelope", "[WipeTower][NoSparseLayers]") {
    PrintConfig   cfg       = clearance_config();
    const Polygon footprint = centered_square(0., 0., 20.);

    // No lift, no envelope: the zone works on the bare footprint.
    CHECK_THAT(unscaled(compacted_wipe_tower_zone(cfg, footprint).hull.bounding_box().max.x()), WithinAbs(10., 1e-6));

    // A spiral lift leaves the outline at low z, so it counts as tower. The circle reaches
    // 2 * lift / (2*pi*atan(slope)) past the outline, matching GCodeWriter: 2*2/(2*pi*atan(3)) = 0.51 mm.
    cfg.z_hop.values = {2.};
    const CompactedTowerZone lifted = compacted_wipe_tower_zone(cfg, footprint);
    CHECK_THAT(unscaled(lifted.hull.bounding_box().max.x()), WithinAbs(10.51, 0.02));
    CHECK_THAT(unscaled(lifted.hull.bounding_box().min.y()), WithinAbs(-10.51, 0.02));
    CHECK(diff(Polygons{footprint}, Polygons{lifted.hull}).empty());

    // z_hop is capped at 5 mm by the option, so a taller lift cannot widen the zone further.
    cfg.z_hop.values = {10.};
    const double capped = unscaled(compacted_wipe_tower_zone(cfg, footprint).hull.bounding_box().max.x());
    CHECK_THAT(capped, WithinAbs(10. + 2. * 5. / (2. * M_PI * std::atan(3.)), 0.02));

    // The rod sweeps the whole X axis, so its band is the tower's y span plus half the rod offset.
    CHECK_THAT(unscaled(lifted.bbox_rod.max.y()), WithinAbs(10.51 + 10., 0.02));
}

TEST_CASE("An object beside a compacted tower is limited by the nearest part of the toolhead", "[WipeTower][NoSparseLayers]") {
    const PrintConfig        cfg  = clearance_config();
    const CompactedTowerZone zone = compacted_wipe_tower_zone(cfg, centered_square(0., 0., 20.));

    // Each side carries half its clearance less 0.1 mm slack, so the two outlines meet when the
    // objects are a full clearance apart: 2 * (4 - 0.2) / 2 = 3.8 mm for the bare nozzle cone,
    // 2 * (40 - 0.2) / 2 = 39.8 mm for the head body. A 10 mm object at x leaves a gap of x - 15.
    const double tall = 50., shortish = 3.;

    // Gap 1 mm, inside the nozzle cone: the object may not rise above the tower at all.
    const CompactedTowerClearance touching = compacted_wipe_tower_clearance(cfg, zone, centered_square(16., 0., 10.), tall);
    CHECK_THAT(touching.allowed_rise, WithinAbs(0., 1e-9));

    // Gap 10 mm: clear of the cone but inside the head body, which starts at nozzle_height.
    const CompactedTowerClearance near_body = compacted_wipe_tower_clearance(cfg, zone, centered_square(25., 0., 10.), tall);
    CHECK(near_body.near_body);
    CHECK_THAT(near_body.allowed_rise, WithinAbs(5., 1e-9));
    CHECK_THAT(near_body.body_clearance, WithinAbs(40., 1e-9));

    // The same spot, but an object that never rises past the cone. The body sits above the cone, so
    // it cannot reach this object however close it stands, and only the narrow tier applies.
    const CompactedTowerClearance low = compacted_wipe_tower_clearance(cfg, zone, centered_square(25., 0., 10.), shortish);
    CHECK_FALSE(low.near_body);
    CHECK_THAT(low.body_clearance, WithinAbs(4., 1e-9));
    CHECK_THAT(low.allowed_rise, WithinAbs(25., 1e-9));

    // Gap 55 mm, clear of the head entirely: the rod is the obstacle, since the object shares the
    // tower's y band and the rod spans the whole x axis however far apart the two stand.
    const CompactedTowerClearance far_in_band = compacted_wipe_tower_clearance(cfg, zone, centered_square(70., 0., 10.), tall);
    CHECK_FALSE(far_in_band.near_body);
    CHECK_THAT(far_in_band.far_clearance, WithinAbs(25., 1e-9));
    CHECK_THAT(far_in_band.allowed_rise, WithinAbs(25., 1e-9));

    // Out of the band the rod passes over it and only the lid is left.
    const CompactedTowerClearance out_of_band = compacted_wipe_tower_clearance(cfg, zone, centered_square(70., 60., 10.), tall);
    CHECK_THAT(out_of_band.allowed_rise, WithinAbs(120., 1e-9));
}

TEST_CASE("The ring drawn around the tower meets the outline drawn around an offender", "[WipeTower][NoSparseLayers]") {
    const PrintConfig        cfg  = clearance_config();
    const CompactedTowerZone zone = compacted_wipe_tower_zone(cfg, centered_square(0., 0., 20.));

    // What the plater draws has to be what the check tested, otherwise a user moves an object until
    // the outlines part and slicing still refuses the plate. Both halves of the 3.8 mm nozzle
    // clearance: at a 3 mm gap the rings overlap and the rise limit is zero, at 5 mm neither holds.
    for (const auto &c : {std::make_pair(18., true), std::make_pair(20., false)}) {
        DYNAMIC_SECTION("object at x = " << c.first) {
            const Polygon                 hull      = centered_square(c.first, 0., 10.);
            const CompactedTowerClearance clearance = compacted_wipe_tower_clearance(cfg, zone, hull, 3.);
            const Polygons                rings     = compacted_wipe_tower_rings(zone, compacted_tower_body_tier(clearance));
            const Polygon                 outline   = compacted_wipe_tower_offender_outline(hull, clearance.body_clearance);
            const bool                    outlines_meet = ! intersection(rings, Polygons{outline}).empty();
            const bool                    rise_denied   = clearance.allowed_rise < EPSILON;
            CHECK(outlines_meet == c.second);
            CHECK(rise_denied == c.second);
        }
    }
}

TEST_CASE("Only the keep-out ring an object is measured against is drawn", "[WipeTower][NoSparseLayers]") {
    const PrintConfig        cfg  = clearance_config();
    const CompactedTowerZone zone = compacted_wipe_tower_zone(cfg, centered_square(0., 0., 20.));

    // Drawing the wide ring when no object is judged on it would show a keep-out zone the check can
    // never trip, so it is added only once some object reaches past the nozzle cone.
    CHECK(compacted_wipe_tower_rings(zone, false).size() == zone.grown_nozzle.size());
    CHECK(compacted_wipe_tower_rings(zone, true).size() == zone.grown_nozzle.size() + zone.grown_body.size());
    CHECK_THAT(unscaled(get_extents(zone.grown_nozzle).max.x()), WithinAbs(10. + 0.5 * (4. - 0.2), 0.02));
    CHECK_THAT(unscaled(get_extents(zone.grown_body).max.x()), WithinAbs(10. + 0.5 * (40. - 0.2), 0.02));
}

TEST_CASE("Footprint padding covers the brim and the extrusion half width on each side", "[WipeTower][NoSparseLayers]") {
    // A nominal outline hulls extrusion centre lines and is re-centred once the real wall is known,
    // so a line width per side on top of the brim is what keeps an estimate enclosing the real tower.
    const PrintConfig cfg = clearance_config();
    CHECK_THAT(compacted_tower_footprint_padding(cfg, 2.), WithinAbs(2. + 2. * 0.4, 1e-9));
    CHECK_THAT(compacted_tower_footprint_padding(cfg, 0.), WithinAbs(2. * 0.4, 1e-9));
    // Callers whose outline already carries the brim pass zero, and a negative one cannot shrink it.
    CHECK_THAT(compacted_tower_footprint_padding(cfg, -5.), WithinAbs(2. * 0.4, 1e-9));
}

// ---------------------------------------------------------------------------------------------
// Snap #934: stagger wipe-tower toolchange starts (Edge-safe indices).
// ---------------------------------------------------------------------------------------------

TEST_CASE("Wipe tower entry stagger distributes start offsets", "[WipeTower][Stagger]")
{
    SECTION("disabled stagger keeps the legacy entry point")
    {
        REQUIRE_THAT(WipeTower2::toolchange_entry_stagger_offset(false, 10, 3, 0.5f), WithinAbs(0.f, 1e-6f));
    }

    SECTION("enabled stagger varies by layer and toolchange")
    {
        const float line_spacing = 0.5f;

        REQUIRE_THAT(WipeTower2::toolchange_entry_stagger_offset(true, 0, 0, line_spacing), WithinAbs(0.f, 1e-6f));
        REQUIRE_THAT(WipeTower2::toolchange_entry_stagger_offset(true, 1, 0, line_spacing), WithinAbs(7.f * line_spacing, 1e-6f));
        REQUIRE_THAT(WipeTower2::toolchange_entry_stagger_offset(true, 0, 1, line_spacing), WithinAbs(5.f * line_spacing, 1e-6f));
        REQUIRE_THAT(WipeTower2::toolchange_entry_stagger_offset(true, 17, 0, line_spacing), WithinAbs(0.f, 1e-6f));
    }

    SECTION("enabled stagger spreads starts across a long cycle")
    {
        std::set<size_t> slots;
        for (size_t layer_idx = 0; layer_idx < 17; ++layer_idx)
            slots.insert(WipeTower2::toolchange_entry_stagger_slot(true, layer_idx, 0));

        REQUIRE(slots.size() == 17);
    }

    SECTION("non-positive line spacing disables the offset")
    {
        REQUIRE_THAT(WipeTower2::toolchange_entry_stagger_offset(true, 2, 1, 0.f), WithinAbs(0.f, 1e-6f));
        REQUIRE_THAT(WipeTower2::toolchange_entry_stagger_offset(true, 2, 1, -0.5f), WithinAbs(0.f, 1e-6f));
    }
}

TEST_CASE("Edge-safe stagger offset is zero on first layer, local-Z and interface", "[WipeTower][Stagger]")
{
    const float spacing = 0.5f;
    const float line_w  = 0.42f;
    const float depth   = 20.f;
    const float ram     = 2.f;
    const size_t first  = 0;

    SECTION("disabled, first layer, local-Z and interface all return 0")
    {
        REQUIRE_THAT(WipeTower2::stagger_offset_for(false, 3, first, 0, false, spacing, depth, ram, line_w),
                     WithinAbs(0.f, 1e-6f));
        REQUIRE_THAT(WipeTower2::stagger_offset_for(true, 0, first, 0, false, spacing, depth, ram, line_w),
                     WithinAbs(0.f, 1e-6f));
        REQUIRE_THAT(WipeTower2::stagger_offset_for(true, 3, first, size_t(-1), false, spacing, depth, ram, line_w),
                     WithinAbs(0.f, 1e-6f));
        REQUIRE_THAT(WipeTower2::stagger_offset_for(true, 3, first, 0, true, spacing, depth, ram, line_w),
                     WithinAbs(0.f, 1e-6f));
        // Extra end-of-print layer: rams in the planned box or unloads at object-top (N1).
        REQUIRE_THAT(WipeTower2::stagger_offset_for(true, 3, first, 0, false, spacing, depth, ram, line_w, true),
                     WithinAbs(0.f, 1e-6f));
    }

    SECTION("index progression matches the slot cycle and is clamped to the box room")
    {
        REQUIRE_THAT(WipeTower2::stagger_offset_for(true, 1, first, 0, false, spacing, depth, ram, line_w),
                     WithinAbs(7.f * spacing, 1e-6f));
        REQUIRE_THAT(WipeTower2::stagger_offset_for(true, 2, first, 0, false, spacing, depth, ram, line_w),
                     WithinAbs(14.f * spacing, 1e-6f));
        REQUIRE_THAT(WipeTower2::stagger_offset_for(true, 1, first, 1, false, spacing, depth, ram, line_w),
                     WithinAbs(12.f * spacing, 1e-6f));
        // room = 3 - 2 - 4*0.5 = -1 → clamped to 0
        REQUIRE_THAT(WipeTower2::stagger_offset_for(true, 1, first, 0, false, spacing, 3.f, 2.f, 0.5f),
                     WithinAbs(0.f, 1e-6f));
        // room = 4.5 - 2 - 4*0.42 = 0.82, slot 7 * 0.5 = 3.5 → clamped to 0.82
        REQUIRE_THAT(WipeTower2::stagger_offset_for(true, 1, first, 0, false, spacing, 4.5f, 2.f, line_w),
                     WithinAbs(4.5f - 2.f - 4.f * line_w, 1e-5f));
    }
}

TEST_CASE("Stagger option is a print preset key and defaults off", "[WipeTower][Stagger][Config]")
{
    CHECK_FALSE(PrintConfig().wipe_tower_stagger_toolchange_start.value);
    const ConfigOptionDef *def = print_config_def.get("wipe_tower_stagger_toolchange_start");
    REQUIRE(def != nullptr);
    CHECK(def->type == coBool);
    CHECK_FALSE(def->get_default_value<ConfigOptionBool>()->value);

    const auto &options = Preset::print_options();
    CHECK(std::find(options.begin(), options.end(), "wipe_tower_stagger_toolchange_start") != options.end());
}

namespace {

const std::string &stagger_profiles_dir()
{
    static const std::string dir = (fs::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    return dir;
}

class DataDirGuard
{
public:
    explicit DataDirGuard(const std::string &dir) : m_prev(data_dir()) { set_data_dir(dir); }
    ~DataDirGuard() { set_data_dir(m_prev); }
    DataDirGuard(const DataDirGuard &)            = delete;
    DataDirGuard &operator=(const DataDirGuard &) = delete;

private:
    std::string m_prev;
};

void load_stagger_vendor(PresetBundle &library, PresetBundle &vendor_bundle, const std::string &vendor)
{
    const fs::path scratch = fs::temp_directory_path() / fs::unique_path("stagger_profiles_%%%%-%%%%");
    fs::create_directories(scratch);
    DataDirGuard guard(scratch.string());
    library.load_vendor_configs_from_json(stagger_profiles_dir(), PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                          ForwardCompatibilitySubstitutionRule::EnableSilent);
    vendor_bundle.load_vendor_configs_from_json(stagger_profiles_dir(), vendor, PresetBundle::LoadSystem,
                                                ForwardCompatibilitySubstitutionRule::EnableSilent, &library);
}

bool process_resolves_stagger(PresetBundle &b, const char *process_name)
{
    const Preset *p = b.prints.find_preset(process_name, false);
    REQUIRE(p != nullptr);
    if (p->config.has("wipe_tower_stagger_toolchange_start"))
        return p->config.opt_bool("wipe_tower_stagger_toolchange_start");
    // Inherited from a parent that carries the key, or the PrintConfig default (false).
    std::string parent = p->inherits();
    while (!parent.empty()) {
        const Preset *up = b.prints.find_preset(parent, false);
        if (up == nullptr)
            break;
        if (up->config.has("wipe_tower_stagger_toolchange_start"))
            return up->config.opt_bool("wipe_tower_stagger_toolchange_start");
        parent = up->inherits();
    }
    return PrintConfig().wipe_tower_stagger_toolchange_start.value;
}

std::string without_timestamp(std::string gcode)
{
    const size_t at = gcode.find("; generated by ");
    if (at != std::string::npos)
        gcode.erase(at, gcode.find('\n', at) - at);
    return gcode;
}

std::string moves_of(const std::string &gcode)
{
    std::istringstream in(without_timestamp(gcode));
    std::string        out, line;
    while (std::getline(in, line)) {
        const size_t eq      = line.find(" = ");
        bool         setting = line.rfind("; ", 0) == 0 && eq != std::string::npos;
        for (size_t i = 2; setting && i < eq; ++i)
            setting = std::isalnum((unsigned char) line[i]) || line[i] == '_';
        if (!setting && line.find("label id") == std::string::npos)
            out += line + "\n";
    }
    return out;
}

DynamicPrintConfig stagger_slice_config(bool stagger)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "layer_height",                       0.3 },
        { "initial_layer_print_height",         0.3 },
        { "wall_loops",                         1 },
        { "sparse_infill_density",              "0%" },
        { "bottom_shell_layers",                2 },
        { "top_shell_layers",                   0 },
        { "enable_support",                     false },
        { "skirt_loops",                        0 },
        { "enable_prime_tower",                 true },
        { "prime_tower_width",                  30 },
        { "prime_tower_brim_width",             3 },
        { "wipe_tower_x",                       "140" },
        { "wipe_tower_y",                       "140" },
        { "wipe_tower_rotation_angle",          0 },
        { "wipe_tower_wall_type",               "rectangle" },
        { "wipe_tower_cone_angle",              0 },
        { "wipe_tower_wall_gap",                true },
        { "wipe_tower_stagger_toolchange_start", stagger },
        { "purge_in_prime_tower",               "1" },
        { "gcode_comments",                     true },
        { "gcode_flavor",                       "marlin" },
        { "layer_change_gcode",                 "G92 E0" },
        { "single_extruder_multi_material",     "1" },
        { "enable_filament_ramming",            "1" },
        { "wipe_tower_no_sparse_layers",        "0" },
        { "filament_multitool_ramming",         "0,0" },
    });
    return config;
}

std::string slice_stagger_project(const DynamicPrintConfig &config, Print &print)
{
    Model model;
    for (int i = 0; i < 2; ++i) {
        ModelObject *object = model.add_object();
        object->name        = i == 0 ? "cubeA" : "cubeB";
        object->add_volume(make_cube(10., 10., 6.));
        object->config.set("extruder", i + 1);
        object->add_instance()->set_offset(Vec3d(40. + (i == 0 ? -8. : 8.), 40., 0.));
        object->ensure_on_bed();
    }
    print.is_BBL_printer() = false;
    print.apply(model, config);
    const StringObjectException err = print.validate();
    INFO(err.string);
    REQUIRE(err.string.empty());
    print.set_status_silent();
    return Test::gcode(print);
}

const std::regex &g1_word()
{
    static const std::regex word("([XYZE])(-?[0-9]*\\.?[0-9]+)");
    return word;
}

struct G1Move
{
    double nx = 0, ny = 0, de = 0;
    bool   has_e = false, has_x = false, has_y = false, is_g1 = false;
};

G1Move parse_g1(const std::string &line, double x, double y)
{
    G1Move            m;
    m.nx                   = x;
    m.ny                   = y;
    const std::string code = line.substr(0, line.find(';'));
    if (!(code.rfind("G1 ", 0) == 0 || code.rfind("G0 ", 0) == 0))
        return m;
    m.is_g1 = true;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), g1_word()); it != std::sregex_iterator(); ++it) {
        const char   axis = (*it)[1].str()[0];
        const double v    = std::atof((*it)[2].str().c_str());
        switch (axis) {
        case 'X': m.nx = v; m.has_x = true; break;
        case 'Y': m.ny = v; m.has_y = true; break;
        case 'E': m.has_e = true; m.de = v; break;
        default: break;
        }
    }
    return m;
}

// Y-only travels after "; CP TOOLCHANGE WIPE" and before the first extrude: the stagger hop.
// XY is tracked through the whole file so the delta is from the real pre-hop position.
std::vector<double> stagger_hops(const std::string &gcode)
{
    std::vector<double> out;
    std::istringstream  in(gcode);
    std::string         line;
    bool                in_wipe = false;
    double              x = 0, y = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos) {
            in_wipe = true;
            continue;
        }
        const G1Move m = parse_g1(line, x, y);
        if (!m.is_g1) {
            if (in_wipe && (line.find("; WIPE_TOWER_END") != std::string::npos ||
                            (line.find("; CP TOOLCHANGE") != std::string::npos && line.find("WIPE") == std::string::npos)))
                in_wipe = false;
            continue;
        }
        if (in_wipe) {
            if (m.has_e && std::abs(m.de) > 1e-6)
                in_wipe = false;
            else {
                const bool y_only = m.has_y && std::abs(m.ny - y) > 1e-4 && (!m.has_x || std::abs(m.nx - x) < 1e-4);
                if (y_only)
                    out.push_back(m.ny - y);
            }
        }
        if (m.has_x) x = m.nx;
        if (m.has_y) y = m.ny;
    }
    return out;
}

size_t count_tag(const std::string &gcode, const char *tag)
{
    size_t n = 0, pos = 0;
    while ((pos = gcode.find(tag, pos)) != std::string::npos) {
        ++n;
        pos += 1;
    }
    return n;
}

// Positive E summed per "; CP TOOLCHANGE WIPE" block (relative E).
std::vector<double> wipe_block_E(const std::string &gcode)
{
    std::vector<double> out;
    std::istringstream  in(gcode);
    std::string         line;
    bool                in_wipe = false;
    double              acc     = 0;
    auto                finish  = [&]() {
        if (in_wipe) {
            out.push_back(acc);
            acc     = 0;
            in_wipe = false;
        }
    };
    while (std::getline(in, line)) {
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos) {
            finish();
            in_wipe = true;
            continue;
        }
        if (in_wipe && (line.find("; WIPE_TOWER_END") != std::string::npos ||
                        (line.find("; CP TOOLCHANGE") != std::string::npos && line.find("WIPE") == std::string::npos))) {
            finish();
            continue;
        }
        if (!in_wipe)
            continue;
        const std::string code = line.substr(0, line.find(';'));
        for (auto it = std::sregex_iterator(code.begin(), code.end(), g1_word()); it != std::sregex_iterator(); ++it) {
            if ((*it)[1].str()[0] == 'E') {
                const double e = std::atof((*it)[2].str().c_str());
                if (e > 0)
                    acc += e;
            }
        }
    }
    finish();
    return out;
}

double wipe_one_line_E(const std::string &gcode)
{
    double              best = 0;
    std::istringstream  in(gcode);
    std::string         line;
    bool                in_wipe = false;
    while (std::getline(in, line)) {
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos) {
            in_wipe = true;
            continue;
        }
        if (in_wipe && (line.find("; WIPE_TOWER_END") != std::string::npos ||
                        (line.find("; CP TOOLCHANGE") != std::string::npos && line.find("WIPE") == std::string::npos))) {
            in_wipe = false;
            continue;
        }
        if (!in_wipe)
            continue;
        const std::string code = line.substr(0, line.find(';'));
        for (auto it = std::sregex_iterator(code.begin(), code.end(), g1_word()); it != std::sregex_iterator(); ++it) {
            if ((*it)[1].str()[0] == 'E')
                best = std::max(best, std::atof((*it)[2].str().c_str()));
        }
    }
    return best > 1e-6 ? best : 1.0;
}

// After a hop of H, the first non-extruding travel whose Y drop matches H is the wrap-around.
std::vector<double> wipe_wrap_returns(const std::string &gcode)
{
    std::vector<double> out;
    std::istringstream  in(gcode);
    std::string         line;
    bool                in_wipe  = false;
    bool                extruded = false;
    bool                seen_wrap = false;
    double              hop = 0, x = 0, y = 0;
    while (std::getline(in, line)) {
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos) {
            in_wipe   = true;
            extruded  = false;
            seen_wrap = false;
            hop       = 0;
            continue;
        }
        if (in_wipe && (line.find("; WIPE_TOWER_END") != std::string::npos ||
                        (line.find("; CP TOOLCHANGE") != std::string::npos && line.find("WIPE") == std::string::npos)))
            in_wipe = false;
        const G1Move m = parse_g1(line, x, y);
        if (m.is_g1) {
            if (in_wipe) {
                const bool y_only = m.has_y && std::abs(m.ny - y) > 1e-4 && (!m.has_x || std::abs(m.nx - x) < 1e-4);
                if (!extruded && y_only && (m.ny - y) > 1e-4)
                    hop = m.ny - y;
                if (extruded && !seen_wrap && !m.has_e && hop > 1e-4 && m.has_y) {
                    const double drop = y - m.ny;
                    if (drop > hop - 0.35 && drop < hop + 0.35) {
                        out.push_back(m.ny - y);
                        seen_wrap = true;
                    }
                }
                if (m.has_e && std::abs(m.de) > 1e-6)
                    extruded = true;
            }
            if (m.has_x) x = m.nx;
            if (m.has_y) y = m.ny;
        }
    }
    return out;
}

// Axis-aligned travel after WIPE and before the first extrude (rotation-safe).
std::vector<double> wipe_entry_axis_hops(const std::string &gcode)
{
    std::vector<double> out;
    std::istringstream  in(gcode);
    std::string         line;
    bool                in_wipe = false;
    double              x = 0, y = 0;
    while (std::getline(in, line)) {
        if (line.find("; CP TOOLCHANGE WIPE") != std::string::npos) {
            in_wipe = true;
            continue;
        }
        const G1Move m = parse_g1(line, x, y);
        if (!m.is_g1) {
            if (in_wipe && (line.find("; WIPE_TOWER_END") != std::string::npos ||
                            (line.find("; CP TOOLCHANGE") != std::string::npos && line.find("WIPE") == std::string::npos)))
                in_wipe = false;
            continue;
        }
        if (in_wipe) {
            if (m.has_e && std::abs(m.de) > 1e-6)
                in_wipe = false;
            else {
                const bool x_only = m.has_x && std::abs(m.nx - x) > 1e-4 && (!m.has_y || std::abs(m.ny - y) < 1e-4);
                const bool y_only = m.has_y && std::abs(m.ny - y) > 1e-4 && (!m.has_x || std::abs(m.nx - x) < 1e-4);
                if (x_only)
                    out.push_back(m.nx - x);
                else if (y_only)
                    out.push_back(m.ny - y);
            }
        }
        if (m.has_x) x = m.nx;
        if (m.has_y) y = m.ny;
    }
    return out;
}

void check_tower_extrusions(const std::string &gcode, const Print &print, double x0, double y0)
{
    const auto  &wtd = print.wipe_tower_data();
    const double b   = wtd.brim_width + 2.;
    const double xmax = x0 + wtd.width + b;
    const double ymax = y0 + wtd.depth + b;
    std::istringstream in(gcode);
    std::string        line;
    bool               in_tower = false;
    double             x = 0, y = 0;
    while (std::getline(in, line)) {
        if (line.find("; WIPE_TOWER_START") == 0) { in_tower = true; continue; }
        if (line.find("; WIPE_TOWER_END") == 0) { in_tower = false; continue; }
        if (!in_tower)
            continue;
        const std::string code = line.substr(0, line.find(';'));
        if (!(code.rfind("G1 ", 0) == 0 || code.rfind("G0 ", 0) == 0 || code.rfind("G2 ", 0) == 0 ||
              code.rfind("G3 ", 0) == 0))
            continue;
        bool has_e = false;
        for (auto it = std::sregex_iterator(code.begin(), code.end(), g1_word()); it != std::sregex_iterator(); ++it) {
            const char   axis = (*it)[1].str()[0];
            const double v    = std::atof((*it)[2].str().c_str());
            switch (axis) {
            case 'X': x = v; break;
            case 'Y': y = v; break;
            case 'E': has_e = true; break;
            default: break;
            }
        }
        if (has_e) {
            INFO("tower extrusion at " << x << "," << y);
            CHECK(x > x0 - b);
            CHECK(y > y0 - b);
            CHECK(x < xmax);
            CHECK(y < ymax);
        }
    }
}

void check_wipe_volume(const std::string &g_off, const std::string &g_on)
{
    const auto   e_off    = wipe_block_E(g_off);
    const auto   e_on     = wipe_block_E(g_on);
    const double one_line = wipe_one_line_E(g_off);
    REQUIRE(e_off.size() == e_on.size());
    REQUIRE(e_off.size() >= 4);
    REQUIRE_FALSE(stagger_hops(g_on).empty());
    size_t cap_i = 0;
    const auto hops = stagger_hops(g_on);
    for (size_t i = 1; i < hops.size(); ++i)
        if (std::abs(hops[i]) > std::abs(hops[cap_i]))
            cap_i = i;
    INFO("cap hop index " << cap_i << " hop=" << hops[cap_i] << " one_line=" << one_line);
    CHECK(std::abs(hops[cap_i]) > 2.0);
    for (size_t i = 0; i < e_off.size(); ++i) {
        INFO("wipe block " << i << " off=" << e_off[i] << " on=" << e_on[i]);
        CHECK(e_on[i] + 1e-6 >= e_off[i] - one_line);
    }
}

} // namespace

TEST_CASE("U1 system process profiles turn stagger on; non-U1 stay off", "[WipeTower][Stagger][Profiles]")
{
    PresetBundle library;
    PresetBundle snap;
    load_stagger_vendor(library, snap, "Snapmaker");
    CHECK(process_resolves_stagger(snap, "0.20mm Standard @Snapmaker U1 (0.4 nozzle)"));
    CHECK(process_resolves_stagger(snap, "0.16mm Standard @Snapmaker U1 (0.4 nozzle)"));
    CHECK_FALSE(process_resolves_stagger(snap, "0.20 Standard @Snapmaker J1 (0.4 nozzle)"));
    CHECK_FALSE(process_resolves_stagger(snap, "0.20 Standard @Snapmaker Artisan (0.4 nozzle)"));
    CHECK_FALSE(DynamicPrintConfig::full_print_config().opt_bool("wipe_tower_stagger_toolchange_start"));
}

TEST_CASE("Stagger off is a no-op on non-U1 G-code; stagger on moves the wipe start", "[WipeTower][Stagger][GCode]")
{
    Print              print_off;
    DynamicPrintConfig cfg_off = stagger_slice_config(false);
    const std::string  g_off   = slice_stagger_project(cfg_off, print_off);
    CHECK(stagger_hops(g_off).empty());

    Print              print_on;
    DynamicPrintConfig cfg_on = stagger_slice_config(true);
    const std::string  g_on   = slice_stagger_project(cfg_on, print_on);
    CHECK(moves_of(g_off) != moves_of(g_on));
    const auto hops = stagger_hops(g_on);
    REQUIRE(hops.size() >= 3);
    std::set<long> hop_um;
    for (double d : hops)
        hop_um.insert(std::lround(d * 1000.));
    CHECK(hop_um.size() >= 3);

    // Finalize / restore-Z markers must not change with stagger.
    CHECK(count_tag(g_off, "[restore_layer_z_before_toolchange]") ==
          count_tag(g_on, "[restore_layer_z_before_toolchange]"));
    CHECK(count_tag(g_off, "; CP TOOLCHANGE UNLOAD") == count_tag(g_on, "; CP TOOLCHANGE UNLOAD"));
    CHECK(count_tag(g_off, "Travel back up to the topmost object layer.") ==
          count_tag(g_on, "Travel back up to the topmost object layer."));

    // Final purge is unload-only. An empty TCR (U1 / skip) must not be used as a stagger source;
    // a real SEMM final purge must not pick up a Y hop.
    for (Print *p : {&print_off, &print_on}) {
        if (!p->wipe_tower_data().final_purge)
            continue;
        if (p->wipe_tower_data().final_purge->gcode.empty())
            CHECK_FALSE(p->wipe_tower_data().final_purge_drop);
        else
            CHECK(stagger_hops(p->wipe_tower_data().final_purge->gcode).empty());
    }

    // Tower extrusions stay on the configured tower (140,140), not on the cubes at ~40,40.
    check_tower_extrusions(g_on, print_on, 140., 140.);
    check_tower_extrusions(g_off, print_off, 140., 140.);
}

TEST_CASE("Stagger wrap-around keeps wipe volume including the cap slot", "[WipeTower][Stagger][GCode]")
{
    Print              print_off;
    DynamicPrintConfig cfg_off = stagger_slice_config(false);
    const std::string  g_off   = slice_stagger_project(cfg_off, print_off);

    Print              print_on;
    DynamicPrintConfig cfg_on = stagger_slice_config(true);
    const std::string  g_on   = slice_stagger_project(cfg_on, print_on);

    check_wipe_volume(g_off, g_on);
    REQUIRE_FALSE(wipe_wrap_returns(g_on).empty());
    CHECK(wipe_wrap_returns(g_off).empty());
}

TEST_CASE("Stagger keeps tower depth and wall-gap alignment", "[WipeTower][Stagger][GCode]")
{
    Print              print_off;
    DynamicPrintConfig cfg_off = stagger_slice_config(false);
    const std::string  g_off   = slice_stagger_project(cfg_off, print_off);

    Print              print_on;
    DynamicPrintConfig cfg_on = stagger_slice_config(true);
    const std::string  g_on   = slice_stagger_project(cfg_on, print_on);

    CHECK_THAT(print_on.wipe_tower_data().depth, WithinAbs(print_off.wipe_tower_data().depth, 0.05f));
    CHECK_THAT(print_on.wipe_tower_data().width, WithinAbs(print_off.wipe_tower_data().width, 0.05f));
    CHECK(count_tag(g_off, "; CP TOOLCHANGE WIPE") == count_tag(g_on, "; CP TOOLCHANGE WIPE"));

    const auto hops = stagger_hops(g_on);
    REQUIRE(hops.size() >= 3);
    // Reversed tower layers flip world Y, so the hop sign follows the layer.
    for (double h : hops) {
        CHECK(std::abs(h) > 0.05);
        CHECK(std::abs(h) < print_on.wipe_tower_data().depth);
    }
    // Slot cycle: hops share a line-spacing quantum (GCD of micrometre hops).
    long hop_gcd = 0;
    for (double h : hops) {
        const long um = std::lround(std::abs(h) * 1000.);
        hop_gcd       = hop_gcd == 0 ? um : std::gcd(hop_gcd, um);
    }
    REQUIRE(hop_gcd >= 50);
    for (double h : hops)
        CHECK(std::lround(std::abs(h) * 1000.) % hop_gcd == 0);
}

TEST_CASE("Stagger wrap-around works on a rotated tower", "[WipeTower][Stagger][GCode]")
{
    Print              print_off;
    DynamicPrintConfig cfg_off = stagger_slice_config(false);
    cfg_off.set_deserialize_strict({ { "wipe_tower_rotation_angle", 90 } });
    const std::string g_off = slice_stagger_project(cfg_off, print_off);

    Print              print_on;
    DynamicPrintConfig cfg_on = stagger_slice_config(true);
    cfg_on.set_deserialize_strict({ { "wipe_tower_rotation_angle", 90 } });
    const std::string g_on = slice_stagger_project(cfg_on, print_on);

    const auto hops_off = wipe_entry_axis_hops(g_off);
    const auto hops_on  = wipe_entry_axis_hops(g_on);
    REQUIRE(hops_on.size() > hops_off.size());
    REQUIRE(hops_on.size() >= 3);
    const auto   e_off    = wipe_block_E(g_off);
    const auto   e_on     = wipe_block_E(g_on);
    const double one_line = wipe_one_line_E(g_off);
    REQUIRE(e_off.size() == e_on.size());
    REQUIRE(e_off.size() >= 3);
    for (size_t i = 0; i < e_off.size(); ++i) {
        INFO("rotated wipe block " << i << " off=" << e_off[i] << " on=" << e_on[i]);
        CHECK(e_on[i] + 1e-6 >= e_off[i] - one_line);
    }
    CHECK_THAT(print_on.wipe_tower_data().depth, WithinAbs(print_off.wipe_tower_data().depth, 0.05f));
}

TEST_CASE("Stagger off is a no-op against the #184 path, including near-edge SEMM", "[WipeTower][Stagger][GCode]")
{
    Print              print_a;
    DynamicPrintConfig cfg_a = stagger_slice_config(false);
    const std::string  g_a   = slice_stagger_project(cfg_a, print_a);

    Print              print_b;
    DynamicPrintConfig cfg_b = stagger_slice_config(false);
    const std::string  g_b   = slice_stagger_project(cfg_b, print_b);

    REQUIRE(moves_of(g_a) == moves_of(g_b));
    CHECK(stagger_hops(g_a).empty());
    CHECK(wipe_wrap_returns(g_a).empty());
    CHECK(wipe_entry_axis_hops(g_a).empty());

    // Near-edge MK4-like SEMM layout: tower against the far X of a 250x210 bed.
    // Stagger-off must keep the unclamped ironing path (clamps are gated on stagger-on).
    Print              print_edge;
    DynamicPrintConfig cfg_edge = stagger_slice_config(false);
    cfg_edge.option<ConfigOptionPoints>("printable_area")->values = {
        Vec2d(0., 0.), Vec2d(250., 0.), Vec2d(250., 210.), Vec2d(0., 210.)
    };
    cfg_edge.set_deserialize_strict({
        { "wipe_tower_x",          "215" },
        { "wipe_tower_y",          "10" },
        { "prime_tower_width",     30 },
        { "prime_tower_brim_width", 3 },
    });
    const std::string g_edge = slice_stagger_project(cfg_edge, print_edge);
    CHECK(stagger_hops(g_edge).empty());
    CHECK(wipe_wrap_returns(g_edge).empty());
    CHECK(count_tag(g_edge, "; CP TOOLCHANGE WIPE") >= 3);
    check_tower_extrusions(g_edge, print_edge, 215., 10.);
}

