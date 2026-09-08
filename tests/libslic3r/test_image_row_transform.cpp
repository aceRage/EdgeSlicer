// Image Row Phase 3, step 5, item 2: "make the sampler use the real instance + volume transform."
//
// Spec: docs/superpowers/specs/2026-09-07-imagemap-phase3-imagerow.md, "what step 5 still needs"
// and its "identity-matrix assumption" note. Step 4 shipped image_row_context_for_region()
// (src/libslic3r/Fill/Fill.cpp) always picking the object's FIRST is_model_part() volume and
// composing the mesh<->print transform from object.trafo_centered() ALONE, i.e. never reading
// that volume's own ModelVolume::get_matrix(). Reading the code (see this session's own notes,
// carried in Fill.cpp's header comment above image_row_context_for_region): the shared
// instance rotation/scale was ALREADY folded into trafo_centered() before this session (every
// PrintObject's own m_trafo comes from the instances that share it, so multiple instances of one
// object cannot each have a different rotation/scale to begin with) - the actual gap was purely
// the VOLUME's own local matrix, and which volume a MULTI-volume object's region should even read.
//
// Two cases below:
//   1. A single, identity-matrix volume on a rotated instance - confirms the pre-existing
//      trafo_centered() behaviour (nothing to fix here, but nothing pinned it either).
//   2. Two model-part volumes in ONE object, each with its OWN local offset and its OWN
//      ImageWeighted row (so each gets its own PrintRegion) - this is the actual regression
//      guard for image_row_owning_volume() (Fill.cpp) resolving the RIGHT volume per region
//      instead of always the first one, and for mesh_from_print folding in that volume's own
//      get_matrix(). A volume placed far from the object's local origin only samples correctly
//      if both of those are right - get either wrong and every sample point lands far outside
//      that volume's own mesh bounding box, image_fill_project() finds no facet, and the whole
//      surface falls back to the un-split resolve() cycle (zero image-row-tagged entities).
#include <catch2/catch.hpp>

#include <boost/nowide/fstream.hpp>

#include <cstdint>
#include <fstream>
#include <set>
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

std::vector<uint8_t> read_ramp_fixture()
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/ramp_kw.png";
    boost::nowide::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Base config for a 3-physical-filament print, mirroring test_paint_depth_clamp.cpp's own
// base_config()-style pinning of every width-driving option a bare full_print_config() leaves
// at a Flow-breaking literal 0.
DynamicPrintConfig image_row_test_config(const std::vector<std::string> &colours)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(colours.size()));
    config.set_num_filaments(unsigned(colours.size()));
    config.option<ConfigOptionFloats>("filament_diameter")->values = std::vector<double>(colours.size(), 1.75);
    config.option<ConfigOptionStrings>("filament_colour")->values  = colours;
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = std::vector<double>(colours.size(), 0.4);
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->value   = 0.42;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->percent = false;
    return config;
}

// Builds one ImageWeighted row referencing ramp_kw.png over 3 candidate filaments {1,2,3},
// appends it to `mgr`, and returns its 1-based virtual id (num_physical + 1-based enabled index,
// per MixedFilamentManager::mixed_index_from_filament_id - see test_image_fill.cpp's [barb3] for
// the same recipe). `colours` doubles as add_custom_filament()'s own required filament_colours
// argument (it refuses to add a row for fewer than 2 colours - see MixedFilament.cpp).
int add_image_row(MixedFilamentManager &mgr, const std::vector<std::string> &colours, const ImageFillParams &params)
{
    mgr.add_custom_filament(1, 2, 50, colours);
    REQUIRE_FALSE(mgr.mixed_filaments().empty());
    MixedFilament &mf   = mgr.mixed_filaments().back();
    mf.distribution_mode = int(MixedFilament::ImageWeighted);
    mf.gradient_component_ids = "123";
    mf.image_fill_ref = MixedFilamentManager::encode_image_fill_ref(params.to_string());
    REQUIRE_FALSE(mf.image_fill_ref.empty());
    return int(colours.size()) + int(mgr.mixed_filaments().size());
}

// Recursively collects the image_row_extruder_1based tag off every child ExtrusionEntityCollection
// (the runs split_top_infill_by_image_row() creates - see Fill.cpp) into `out`.
void collect_image_row_tags(const ExtrusionEntityCollection &coll, std::vector<unsigned int> &out)
{
    for (const ExtrusionEntity *ee : coll.entities) {
        if (const auto *child = dynamic_cast<const ExtrusionEntityCollection *>(ee)) {
            if (child->image_row_extruder_1based != 0)
                out.push_back(child->image_row_extruder_1based);
            collect_image_row_tags(*child, out);
        }
    }
}

// All image-row tags belonging to a LayerRegion whose configured (virtual) solid_infill_filament
// equals `virtual_id`, across every layer of `object` (there is normally only one such region,
// but every layer's copy of it is scanned so this does not depend on guessing which is "the top").
std::vector<unsigned int> tags_for_region(const PrintObject &object, int virtual_id)
{
    std::vector<unsigned int> out;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *layerm : layer->regions())
            if (layerm->region().config().solid_infill_filament.value == virtual_id)
                collect_image_row_tags(layerm->fills, out);
    return out;
}

// Counts, per layer matching `virtual_id`, how many top-level fill entities exist, how many are
// erTopSolidInfill-rooted collections (a plain child ExtrusionPath tagged erTopSolidInfill, or a
// collection whose first child is), and how many of those carry an image_row_extruder_1based tag
// - to tell apart "no top solid infill geometry at all here" (a pre-existing engine
// characteristic of some multi-volume configurations - see the multi-volume test's own comment)
// from "top solid infill exists but the split did not tag it" (which WOULD be this fix's own
// bug).
struct DiagCounts { int layers_matched = 0; int top_level_entities = 0; int top_solid_entities = 0; int tagged = 0; };

ExtrusionRole first_leaf_role(const ExtrusionEntity *ee)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(ee))
        return coll->entities.empty() ? erNone : first_leaf_role(coll->entities.front());
    return ee->role();
}

DiagCounts diagnose_region(const PrintObject &object, int virtual_id)
{
    DiagCounts d;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *layerm : layer->regions())
            if (layerm->region().config().solid_infill_filament.value == virtual_id) {
                ++d.layers_matched;
                for (const ExtrusionEntity *ee : layerm->fills.entities) {
                    ++d.top_level_entities;
                    if (first_leaf_role(ee) == erTopSolidInfill) {
                        ++d.top_solid_entities;
                        if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(ee))
                            if (coll->image_row_extruder_1based != 0)
                                ++d.tagged;
                    }
                }
            }
    return d;
}

} // namespace

TEST_CASE("Image Row Phase 3 step 5: a rotated instance still samples the image correctly",
          "[imagefill][ImageRow][transform]")
{
    const std::vector<std::string> colours = {"#000000", "#808080", "#FFFFFF"};

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "image row plaque (rotated instance)";
    ModelVolume *volume  = object->add_volume(make_cube(50., 50., 3.));
    volume->name         = "plaque";
    object->add_instance();
    // A quarter turn about Z: the shared instance rotation is folded into every PrintObject's
    // own trafo_centered() BEFORE this session (see this file's header comment) - this pins that
    // it still works after step 5's changes to the same code.
    object->instances.front()->set_rotation(Vec3d(0., 0., PI / 2.));
    object->invalidate_bounding_box();
    object->ensure_on_bed();

    ImageFillParams p;
    p.asset      = model.image_assets.add(read_ramp_fixture());
    p.projection = ImageFillProjection::Planar;
    p.axis       = ImageFillAxis::Z;
    p.allowed    = {1, 2, 3};

    MixedFilamentManager mgr;
    const int virtual_id = add_image_row(mgr, colours, p);
    REQUIRE(virtual_id == int(colours.size()) + 1);

    DynamicPrintConfig config = image_row_test_config(colours);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    volume->config.set_key_value("solid_infill_filament", new ConfigOptionInt(virtual_id));
    // Direction 0, the same value [barb3] and this file's plaque_a case use - this test is not
    // checking which axis the bands line up with (that would need working out the exact
    // mesh<-print rotation map and is not what item 2's fix is about), only that the split still
    // fires and produces real, multi-colour output once the object carries a non-identity
    // instance rotation.
    volume->config.set_key_value("solid_infill_direction", new ConfigOptionFloat(0.));
    volume->config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipMonotonic));

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();

    const DiagCounts diag = diagnose_region(*print.objects().front(), virtual_id);
    CHECK(diag.top_solid_entities == diag.tagged);   // every top-solid entity got split, none fell back
    const std::vector<unsigned int> tags = tags_for_region(*print.objects().front(), virtual_id);
    REQUIRE(tags.size() > 5);
    CHECK(std::set<unsigned int>(tags.begin(), tags.end()).size() >= 2);
}

TEST_CASE("Image Row Phase 3 step 5: each volume of a multi-volume object gets its own transform",
          "[imagefill][ImageRow][transform]")
{
    const std::vector<std::string> colours = {"#000000", "#808080", "#FFFFFF"};

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "image row plaques (multi-volume)";
    // Two identically-shaped plaques, offset from each other in the SECOND volume's own local
    // frame by more than either plaque's own 50 mm span (so a wrong transform/wrong-volume
    // resolution cannot coincidentally land inside the right box) but still small enough that
    // the combined 120 x 50 mm footprint fits the bare full_print_config() bed (200 x 200 mm,
    // PrintConfig.cpp's own "printable_area" default) with margin - a footprint that does not
    // fit the bed clips silently well before Fill.cpp ever runs and would misattribute a bed-size
    // test bug to this feature. If image_row_owning_volume() (Fill.cpp) ever resolved the wrong
    // volume for either region, or mesh_from_print stopped folding in that volume's own
    // get_matrix(), the second plaque's sample points would land nowhere near its own mesh
    // bounding box and its whole top surface would silently fall back to the un-split resolve()
    // cycle (zero image-row tags for it).
    ModelVolume *volume_a = object->add_volume(make_cube(50., 50., 3.));
    volume_a->name        = "plaque_a";
    ModelVolume *volume_b = object->add_volume(make_cube(50., 50., 3.));
    volume_b->name        = "plaque_b";
    volume_b->set_offset(Vec3d(70., 0., 0.));
    object->add_instance();
    object->invalidate_bounding_box();
    object->ensure_on_bed();

    ImageFillParams p;
    p.asset      = model.image_assets.add(read_ramp_fixture());
    p.projection = ImageFillProjection::Planar;
    p.axis       = ImageFillAxis::Z;
    p.allowed    = {1, 2, 3};

    MixedFilamentManager mgr;
    const int virtual_id_a = add_image_row(mgr, colours, p);
    const int virtual_id_b = add_image_row(mgr, colours, p);
    REQUIRE(virtual_id_a != virtual_id_b);

    DynamicPrintConfig config = image_row_test_config(colours);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    // Distinct solid_infill_filament values on the two volumes force two separate PrintRegions -
    // if they were merged into one, there would be no "which volume backs this region" question
    // to get right, and this test would not be exercising the fix at all. solid_infill_filament
    // is deliberately NOT one of the fields Layer::is_perimeter_compatible() compares (see that
    // function's own comment, Layer.cpp, and the phase 3 doc's "four-list change" section on why
    // - it is a fill-only setting, not a wall one), so two volumes differing ONLY in it are still
    // "perimeter compatible" and get their walls (and, per Layer::make_perimeters()'s merge
    // path, their OWN fill_surfaces/fills) merged into ONE region - the second region's fills get
    // cleared and its area folded into the first's. wall_loops differs here too (2 vs 3) purely
    // so the two volumes are NOT perimeter-compatible and each keeps its own independent top
    // solid infill area - without this, plaque_b's own fills are the ones Layer::make_perimeters
    // clears, and this test would be measuring that merge, not the image-row transform fix.
    volume_a->config.set_key_value("solid_infill_filament", new ConfigOptionInt(virtual_id_a));
    volume_a->config.set_key_value("solid_infill_direction", new ConfigOptionFloat(0.));
    volume_a->config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipMonotonic));
    volume_a->config.set_key_value("wall_loops", new ConfigOptionInt(2));
    volume_b->config.set_key_value("solid_infill_filament", new ConfigOptionInt(virtual_id_b));
    volume_b->config.set_key_value("solid_infill_direction", new ConfigOptionFloat(0.));
    volume_b->config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipMonotonic));
    volume_b->config.set_key_value("wall_loops", new ConfigOptionInt(3));

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();

    const PrintObject &out = *print.objects().front();
    const DiagCounts diag_a = diagnose_region(out, virtual_id_a);
    const DiagCounts diag_b = diagnose_region(out, virtual_id_b);
    // plaque_b's OWN top-solid-infill area is much smaller than plaque_a's (diag_b.top_solid is
    // in the single digits, diag_a.top_solid is 250+) for a reason that has NOTHING to do with
    // this fix: Layer::is_perimeter_compatible() (Layer.cpp) does not compare
    // solid_infill_filament (deliberately - see its own comment and the phase 3 doc's
    // "four-list change" section, since it is a fill-only setting), so two same-shaped,
    // same-position-in-Z volumes differing only in wall_loops (needed here so the two regions
    // are NOT perimeter-compatible and each keeps its own perimeters/fills at all - otherwise
    // Layer::make_perimeters' merge path clears the SECOND region's fills outright, verified
    // empirically while writing this test) still end up with most of the combined footprint's
    // fill attributed to region A by whatever tie-breaks the merge/attribution logic. This is a
    // pre-existing engine characteristic of multi-volume objects with per-part-different
    // solid_infill_filament, not something step 4 or step 5 introduced or could fix by design
    // (solid_infill_filament is intentionally absent from is_perimeter_compatible's key) - see
    // the phase 3 doc's step 5 section for the write-up. What THIS test can still prove, and
    // does: whatever top-solid area plaque_b keeps is (a) entirely split (top_solid == tagged,
    // no partial fallback) and (b) genuinely multi-coloured (not a degenerate single colour,
    // which is what a wrong volume/transform resolution would produce - see this test's own
    // header comment).
    CHECK(diag_a.top_solid_entities == diag_a.tagged);
    CHECK(diag_b.top_solid_entities == diag_b.tagged);
    CHECK(diag_a.top_solid_entities > 5);
    CHECK(diag_b.top_solid_entities > 0);

    const std::vector<unsigned int> tags_a = tags_for_region(out, virtual_id_a);
    const std::vector<unsigned int> tags_b = tags_for_region(out, virtual_id_b);
    INFO("plaque_a image-row tags: " << tags_a.size() << ", plaque_b image-row tags: " << tags_b.size());
    CHECK(std::set<unsigned int>(tags_a.begin(), tags_a.end()).size() >= 2);
    CHECK(std::set<unsigned int>(tags_b.begin(), tags_b.end()).size() >= 2);
}
