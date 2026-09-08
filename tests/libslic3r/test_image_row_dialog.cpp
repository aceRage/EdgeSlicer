// Image Row Phase 3: the DIALOG path (Plater::apply_image_fill) - the owner's bug report.
//
// The report, verbatim: "I can see the nozzle resolution does create the mixed filament, though
// it doesn't seem to affect the overall slice." The sidebar shows the ImageWeighted row, but the
// sliced top surface looks exactly like Phase 2's facet painting - no per-line dither.
//
// The difference between the [barb3] test (which WORKS - see test_image_fill.cpp) and the dialog
// is one line of Plater::apply_image_fill(): the dialog ALWAYS runs Phase 2's facet painting
// (image_fill_apply(), writing mmu_segmentation_facets) before it creates/binds the row, tick or
// no tick. [barb3] carries no painting at all.
//
// That painting is what breaks the row. PrintApply.cpp's generate_print_object_regions() adds a
// PAINTED PrintRegion per painted extruder id (the "Finally add painting regions" loop), and each
// one does:
//
//     cfg.solid_infill_filament.value = painted_extruder_id;   // PrintApply.cpp
//
// i.e. it OVERWRITES the part's configured solid_infill_filament - the virtual id the dialog just
// bound - with the painted PHYSICAL extruder id. Fill.cpp's image_row_configured_virtual_id()
// reads exactly that key, so on every painted region it sees a physical id, MixedFilamentManager::
// is_mixed() says no, and the function returns 0: no image row, no split, no dither. And because
// the image fill paints essentially the whole top surface, virtually all of that surface belongs
// to painted sub-regions, leaving the row bound to a parent region that has almost no top solid
// infill area left to split.
//
// Both cases below build the model through the SAME model-level API calls Plater::apply_image_fill
// makes, in the same order. Nobody clicks the dialog (the GUI path has no automated coverage - see
// the phase 3 spec's "Proofs"); this is as close as a libslic3r test can get.
#include <catch2/catch.hpp>

#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ImageFill.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

const std::vector<std::string> kColours = {"#000000", "#808080", "#FFFFFF"};

std::vector<uint8_t> read_ramp()
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/ramp_kw.png";
    boost::nowide::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Same base config as test_image_row_transform.cpp's - a bare full_print_config() leaves several
// width-driving options at a literal 0, which breaks Flow long before Fill.cpp runs.
DynamicPrintConfig dialog_test_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(kColours.size()));
    config.set_num_filaments(unsigned(kColours.size()));
    config.option<ConfigOptionFloats>("filament_diameter")->values = std::vector<double>(kColours.size(), 1.75);
    config.option<ConfigOptionStrings>("filament_colour")->values  = kColours;
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = std::vector<double>(kColours.size(), 0.4);
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->value   = 0.42;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->percent = false;
    return config;
}

// The ramp fixture is a 1 x 16 vertical white-to-black ramp; a Planar/Z projection maps u from x
// and v from y, so it varies along the plaque's Y axis.
ImageFillParams ramp_params(Model &model)
{
    ImageFillParams p;
    p.asset      = model.image_assets.add(read_ramp());
    p.projection = ImageFillProjection::Planar;
    p.axis       = ImageFillAxis::Z;
    p.allowed    = {1, 2, 3};
    return p;
}

// Exactly what Plater::apply_image_fill() builds from its `filaments` list, for allowed = {1,2,3}.
void allowed_colors_ids(std::vector<std::array<float, 3>> &colors, std::vector<int> &ids)
{
    colors = {{0.f, 0.f, 0.f}, {0.5019608f, 0.5019608f, 0.5019608f}, {1.f, 1.f, 1.f}};
    ids    = {1, 2, 3};
}

// The row half of Plater::apply_image_fill()'s Apply handler, verbatim in behaviour: create the
// ImageWeighted row over `ids`, then bind the PART's own solid_infill_filament to its virtual id.
int apply_image_row_binding(MixedFilamentManager &mgr, ModelVolume &volume,
                            const ImageFillParams &params, const std::vector<int> &ids)
{
    mgr.add_custom_filament(unsigned(ids.front()), unsigned(ids.size() > 1 ? ids[1] : ids.front()), 50, kColours);
    REQUIRE_FALSE(mgr.mixed_filaments().empty());
    MixedFilament &row        = mgr.mixed_filaments().back();
    row.enabled               = true;
    row.distribution_mode     = int(MixedFilament::ImageWeighted);
    row.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(
        std::vector<unsigned int>(ids.begin(), ids.end()));
    row.image_fill_ref = MixedFilamentManager::encode_image_fill_ref(params.to_string());
    REQUIRE_FALSE(row.image_fill_ref.empty());
    const unsigned int virtual_id =
        mgr.filament_id_from_mixed_index(mgr.mixed_filaments().size() - 1, kColours.size());
    REQUIRE(virtual_id != 0);
    volume.config.set_key_value("solid_infill_filament", new ConfigOptionInt(int(virtual_id)));
    // The two fill-direction keys [barb3] also sets, so each fill line samples one roughly
    // constant band of the ramp instead of sweeping the whole gradient.
    volume.config.set_key_value("solid_infill_direction", new ConfigOptionFloat(0.));
    volume.config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipMonotonic));
    return int(virtual_id);
}

void collect_tags(const ExtrusionEntityCollection &coll, std::vector<unsigned int> &out)
{
    for (const ExtrusionEntity *ee : coll.entities)
        if (const auto *child = dynamic_cast<const ExtrusionEntityCollection *>(ee)) {
            if (child->image_row_extruder_1based != 0)
                out.push_back(child->image_row_extruder_1based);
            collect_tags(*child, out);
        }
}

ExtrusionRole first_leaf_role(const ExtrusionEntity *ee)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(ee))
        return coll->entities.empty() ? erNone : first_leaf_role(coll->entities.front());
    return ee->role();
}

// The whole object's top layer, across EVERY region (painted sub-regions included) - which is the
// only honest way to measure the dialog path, where the top surface is spread over several
// painted regions rather than living on the one region the row is bound to.
struct TopLayerStats
{
    int                       top_solid_entities = 0;
    int                       tagged             = 0;
    std::vector<unsigned int> tags;
    std::set<int>             region_solid_infill_filaments;  // configured value, per region present
};

TopLayerStats top_layer_stats(const PrintObject &object)
{
    TopLayerStats s;
    REQUIRE_FALSE(object.layers().empty());
    const Layer *top = object.layers().back();
    for (const LayerRegion *layerm : top->regions()) {
        s.region_solid_infill_filaments.insert(layerm->region().config().solid_infill_filament.value);
        for (const ExtrusionEntity *ee : layerm->fills.entities)
            if (first_leaf_role(ee) == erTopSolidInfill) {
                ++s.top_solid_entities;
                if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(ee))
                    if (coll->image_row_extruder_1based != 0)
                        ++s.tagged;
            }
        collect_tags(layerm->fills, s.tags);
    }
    return s;
}

// The dominant image-row filament in each of `bands` equal-width Y slices of the object's top
// layer, weighted by extruded length - the same per-band shape Bar B measures off G-code, but
// read straight off the split runs. A band with no runs at all reports 0.
std::vector<int> dominant_tool_per_band(const PrintObject &object, int bands)
{
    const Layer *top = object.layers().back();
    // Y extent of every image-row-tagged run on the top layer.
    coord_t y_min = std::numeric_limits<coord_t>::max(), y_max = std::numeric_limits<coord_t>::lowest();
    std::vector<std::pair<const ExtrusionEntityCollection *, Polylines>> tagged;
    std::function<void(const ExtrusionEntityCollection &)> walk = [&](const ExtrusionEntityCollection &coll) {
        for (const ExtrusionEntity *ee : coll.entities)
            if (const auto *child = dynamic_cast<const ExtrusionEntityCollection *>(ee)) {
                if (child->image_row_extruder_1based != 0) {
                    Polylines pls;
                    for (const ExtrusionEntity *leaf : child->entities)
                        if (const auto *path = dynamic_cast<const ExtrusionPath *>(leaf))
                            pls.push_back(path->polyline);
                    for (const Polyline &pl : pls)
                        for (const Point &pt : pl.points) { y_min = std::min(y_min, pt.y()); y_max = std::max(y_max, pt.y()); }
                    tagged.emplace_back(child, std::move(pls));
                }
                walk(*child);
            }
    };
    for (const LayerRegion *layerm : top->regions())
        walk(layerm->fills);
    // NOTE: `std::vector<...> per_band(size_t(bands));` is the most vexing parse (MSVC reads it
    // as a function declaration taking a size_t named `bands`), hence the named count.
    const size_t band_count = size_t(bands);
    std::vector<std::map<int, double>> per_band(band_count);
    if (tagged.empty() || y_max <= y_min)
        return std::vector<int>(band_count, 0);
    for (const auto &entry : tagged)
        for (const Polyline &pl : entry.second)
            for (size_t i = 0; i + 1 < pl.points.size(); ++i) {
                const Point  mid = (pl.points[i] + pl.points[i + 1]) / 2;
                const double len = (pl.points[i + 1] - pl.points[i]).cast<double>().norm();
                int b = int(double(mid.y() - y_min) / double(y_max - y_min) * bands);
                b = std::min(std::max(b, 0), bands - 1);
                per_band[size_t(b)][int(entry.first->image_row_extruder_1based)] += len;
            }
    std::vector<int> out(band_count, 0);
    for (int b = 0; b < bands; ++b) {
        double best = -1.;
        for (const auto &kv : per_band[size_t(b)])
            if (kv.second > best) { best = kv.second; out[size_t(b)] = kv.first; }
    }
    return out;
}

// A 50 x 50 x 3 mm plaque, one instance, on the bed - the plan's own Bar B shape.
ModelVolume *make_plaque(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "image row plaque (dialog path)";
    ModelVolume *volume = object->add_volume(make_cube(50., 50., 3.));
    volume->name        = "plaque";
    object->add_instance();
    object->ensure_on_bed();
    return volume;
}

} // namespace

// ================================================================================================
// The control: the row ALONE (no facet painting) - [barb3]'s own arrangement, which works.
// ================================================================================================
TEST_CASE("Image Row: the row alone (no facet painting) splits the top surface",
          "[imagefill][ImageRow][dialogpath]")
{
    Model              model;
    ModelVolume       *volume = make_plaque(model);
    const ImageFillParams p   = ramp_params(model);

    MixedFilamentManager mgr;
    const int virtual_id = apply_image_row_binding(mgr, *volume, p, {1, 2, 3});

    DynamicPrintConfig config = dialog_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();

    const TopLayerStats s = top_layer_stats(*print.objects().front());
    INFO("row-only: top_solid=" << s.top_solid_entities << " tagged=" << s.tagged
         << " tags=" << s.tags.size() << " virtual_id=" << virtual_id);
    CHECK(s.top_solid_entities > 0);
    CHECK(s.tagged == s.top_solid_entities);
    CHECK(s.tags.size() >= size_t(s.top_solid_entities));
    CHECK(std::set<unsigned int>(s.tags.begin(), s.tags.end()).size() >= 2);
}

// ================================================================================================
// The bug: the DIALOG path - facet painting AND the row, exactly as Plater::apply_image_fill does.
// ================================================================================================
TEST_CASE("Image Row: the dialog path (facet painting + row) dithers the top surface",
          "[imagefill][ImageRow][dialogpath]")
{
    Model              model;
    ModelVolume       *volume = make_plaque(model);
    const ImageFillParams p   = ramp_params(model);

    // Step 1 of Plater::apply_image_fill()'s Apply handler: Phase 2's facet painting, which runs
    // whether or not the checkbox is ticked.
    std::vector<std::array<float, 3>> colors;
    std::vector<int>                  ids;
    allowed_colors_ids(colors, ids);
    const ImageFillResult res = image_fill_apply(*volume, p, model.image_assets, colors, ids);
    REQUIRE(res.ok);
    REQUIRE(res.facets_painted > 0);
    REQUIRE(volume->is_mm_painted());

    // Step 2: the image row and the part's binding to it.
    MixedFilamentManager mgr;
    const int virtual_id = apply_image_row_binding(mgr, *volume, p, ids);

    DynamicPrintConfig config = dialog_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();

    const TopLayerStats s = top_layer_stats(*print.objects().front());
    std::string regions;
    for (int f : s.region_solid_infill_filaments)
        regions += std::to_string(f) + " ";
    INFO("dialog path: top_solid=" << s.top_solid_entities << " tagged=" << s.tagged
         << " tags=" << s.tags.size() << " virtual_id=" << virtual_id
         << " painted_facets=" << res.facets_painted
         << " region solid_infill_filament values: " << regions);

    // The whole point of the feature: the top surface is split into image-row runs. Before the
    // fix, `tagged` is 0 (or a tiny remainder) because every painted sub-region's
    // solid_infill_filament was overwritten with a physical id by PrintApply.cpp.
    CHECK(s.top_solid_entities > 0);
    CHECK(s.tagged == s.top_solid_entities);
    // Many short colour runs, not triangle-shaped patches. The facet painting of this plaque
    // touches 32 facets, of which exactly TWO are the top face - so the paint alone can put at
    // most 2 colour patches on this top surface, and each is a triangle covering half the
    // plaque. Every tag here is one dithered run at nozzle resolution instead. Before the fix
    // this number is 0 (nothing split at all).
    CHECK(s.tags.size() > 2);
    // All three filaments really present on the top layer - the paint can only ever reach the
    // two colours its two top facets sampled.
    CHECK(std::set<unsigned int>(s.tags.begin(), s.tags.end()).size() == 3);
    // Per-band monotonic coverage: the ramp runs along Y, so walking the top surface in Y must
    // walk the palette in one direction without reversing. Measured directly off the run
    // geometry rather than off the tag order, which follows fill order, not Y.
    const std::vector<int> band_tools = dominant_tool_per_band(*print.objects().front(), 4);
    INFO("dominant tool per Y band (4 bands): " << band_tools[0] << " " << band_tools[1]
         << " " << band_tools[2] << " " << band_tools[3]);
    CHECK(std::is_sorted(band_tools.begin(), band_tools.end()));
    CHECK(band_tools.front() != band_tools.back());
}
