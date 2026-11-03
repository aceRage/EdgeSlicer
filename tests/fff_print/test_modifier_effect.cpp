// Modifier "no effect" diagnostics, and the parent search that used to drop a modifier listed
// before its part.
//
// A PARAMETER_MODIFIER volume can be perfectly visible in the object list, sit exactly where the
// user put it, and still change nothing at all about the slice. Two ways that happens:
//
//   (a) it overrides no setting, so the region builder (generate_print_object_regions(),
//       PrintApply.cpp) resolves it to its parent's own PrintRegionConfig and stores it as an ALIAS
//       of the parent's region - it cannot print differently, because it IS the parent's region;
//   (b) it never finds a parent part, so no region is built for it at all.
//
// Both were silent. They now raise a NON_CRITICAL slicing warning on posSlice, and (b) is also
// largely fixed: a modifier listed BEFORE the part it overlaps now attaches to it, because the
// backward-only parent search got a second pass. (b) still legitimately fires for a modifier that
// overlaps no part at all, which is what the last case here pins.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

// How the modifier volume is placed relative to the part, and whether it is listed before it.
struct ModifierSpec
{
    // Offset of the modifier cube's centre from the part cube's centre, in mm.
    Vec3d offset { 0., 0., 0. };
    // Listed BEFORE the MODEL_PART in ModelObject::volumes - the order the backward parent search
    // used to choke on.
    bool  before_part { false };
    // Applied to the modifier volume's own config before apply(); nullptr leaves it empty, which is
    // what the SVG/Emboss gizmo creates (it sets only "extruder", and extruder 0 means "default"
    // and is explicitly ignored by apply_to_print_region_config).
    std::function<void(ModelVolume &)> overrides;
    // Two filaments, and the part carries colour painting (all facets, filament 2).
    bool paint_part { false };
};

// One object: a 20 mm part cube at the origin plus one modifier cube, sliced.
// `print` must outlive the returned pointer.
const PrintObject *slice_with_modifier(Print &print, Model &model, const ModifierSpec &spec)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    if (spec.paint_part) {
        config.set_num_extruders(2);
        config.set_num_filaments(2);
        config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
        config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#0000FF" };
        config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.4, 0.4 };
        config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
        config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    }

    ModelObject *object = model.add_object();
    object->name = "cube_with_modifier";

    auto add_part = [&object, &spec]() {
        ModelVolume *v = object->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
        v->name = "part";
        v->set_type(ModelVolumeType::MODEL_PART);
        // its_make_cube() builds from the origin into +XYZ; centre it so the offsets below are
        // measured centre-to-centre.
        v->set_offset(Vec3d(-10., -10., -10.));
        if (spec.paint_part) {
            v->config.set("extruder", 1);
            TriangleSelector selector(v->mesh());
            for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
                selector.set_facet(f, EnforcerBlockerType(2));
            REQUIRE(v->mmu_segmentation_facets.set(selector));
        }
        return v;
    };
    auto add_modifier = [&object, &spec]() {
        // Deliberately smaller than the part, so an overlapping modifier is strictly inside it and
        // a displaced one clears its bounding box entirely.
        ModelVolume *v = object->add_volume(TriangleMesh(its_make_cube(6., 6., 6.)));
        v->name = "svg_modifier";
        v->set_type(ModelVolumeType::PARAMETER_MODIFIER);
        v->set_offset(spec.offset + Vec3d(-3., -3., -3.));
        // What the SVG/Emboss gizmo sets, and all it sets. Extruder 0 is "default" and is ignored
        // by apply_to_print_region_config, so on its own this is NOT an override.
        v->config.set_key_value("extruder", new ConfigOptionInt(0));
        if (spec.overrides)
            spec.overrides(*v);
        return v;
    };

    // Volume ORDER is the point of one of these cases, so build it explicitly.
    if (spec.before_part) {
        add_modifier();
        add_part();
    } else {
        add_part();
        add_modifier();
    }

    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    REQUIRE(! print.objects().empty());
    return print.objects().front();
}

// Is a warning with this id current on the object's posSlice step?
bool has_slice_warning(const PrintObject &object, PrintStateBase::SlicingNotificationType id)
{
    const PrintStateBase::StateWithWarnings state = object.step_state_with_warnings(posSlice);
    return std::any_of(state.warnings.begin(), state.warnings.end(),
                       [id](const PrintStateBase::Warning &w) { return w.message_id == int(id); });
}

// The text of that warning, or "" when it is not there.
std::string slice_warning_text(const PrintObject &object, PrintStateBase::SlicingNotificationType id)
{
    const PrintStateBase::StateWithWarnings state = object.step_state_with_warnings(posSlice);
    for (const PrintStateBase::Warning &w : state.warnings)
        if (w.message_id == int(id))
            return w.message;
    return std::string();
}

// How many DISTINCT PrintRegions the object's layers actually use. The whole point of a working
// modifier is that this is more than one: the modifier's own region is separate from the part's.
size_t distinct_layer_regions(const PrintObject &object)
{
    std::vector<int> ids;
    for (const Layer *layer : object.layers())
        for (const LayerRegion *lregion : layer->regions())
            if (! lregion->slices.empty())
                ids.push_back(lregion->region().print_object_region_id());
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids.size();
}

// A real override: a wall count that differs from the object's default.
void override_wall_loops(ModelVolume &v)
{
    v.config.set_key_value("wall_loops", new ConfigOptionInt(7));
}

} // namespace

TEST_CASE("modifier with no config overrides warns and creates no distinct region", "[ModifierEffect]")
{
    Print print;
    Model model;
    // The SVG/Emboss default: overlapping the part, but overriding nothing.
    const PrintObject *object = slice_with_modifier(print, model, ModifierSpec{});

    CHECK(has_slice_warning(*object, PrintStateBase::SlicingModifierNoOverrides));
    // It DID find its parent - the complaint is only that it changes nothing.
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierNoParent));

    const std::string text = slice_warning_text(*object, PrintStateBase::SlicingModifierNoOverrides);
    CHECK_THAT(text, Catch::Matchers::Contains("svg_modifier"));
    CHECK_THAT(text, Catch::Matchers::Contains("cube_with_modifier"));
    CHECK_THAT(text, Catch::Matchers::Contains("no effect"));

    // An alias of the parent's region: everything sliced belongs to one single region.
    CHECK(distinct_layer_regions(*object) == 1);
}

TEST_CASE("modifier with a wall count override does not warn and gets its own region", "[ModifierEffect]")
{
    Print print;
    Model model;
    ModifierSpec spec;
    spec.overrides = override_wall_loops;
    const PrintObject *object = slice_with_modifier(print, model, spec);

    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierNoOverrides));
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierNoParent));

    // The modifier's region is distinct from the part's, which is what "has an effect" means.
    CHECK(distinct_layer_regions(*object) > 1);
}

TEST_CASE("modifier listed before its part still attaches to it", "[ModifierEffect]")
{
    Print print;
    Model model;
    ModifierSpec spec;
    spec.before_part = true;          // the order the backward-only parent search used to drop
    spec.overrides   = override_wall_loops;
    const PrintObject *object = slice_with_modifier(print, model, spec);

    // Before the second-pass fix this modifier found no parent at all: no region, no effect, and
    // (now) a warning. It must attach despite the order.
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierNoParent));
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierNoOverrides));
    CHECK(distinct_layer_regions(*object) > 1);
}

// The owner's question: does this affect TEXT embossing (GLGizmoEmboss -> Modifier) as well as
// SVG? It does, and for the same reason. Both gizmos funnel into the one create_volume() in
// EmbossJob.cpp, which sets only `extruder` (0 = "default", which apply_to_print_region_config
// explicitly ignores), and DataBase::write() then sets only name / emboss_shape / a transform -
// no config on either path. So a text modifier reaches the slicer with exactly the config modelled
// here and by the first case above, and a modifier volume is just a modifier volume to the slicer:
// nothing downstream distinguishes text from SVG. This case states that rather than leaving it
// implied, so it fails if either creation path ever starts seeding config of its own.
TEST_CASE("text-emboss modifier with the creation-path config warns like an SVG one", "[ModifierEffect]")
{
    Print print;
    Model model;
    ModifierSpec spec;
    spec.overrides = [](ModelVolume &v) {
        // Exactly what the text path leaves behind: the name it was given, and nothing else. The
        // "extruder" key is already set by the shared helper, as create_volume() does.
        v.name = "text_modifier";
    };
    const PrintObject *object = slice_with_modifier(print, model, spec);

    CHECK(has_slice_warning(*object, PrintStateBase::SlicingModifierNoOverrides));
    CHECK_THAT(slice_warning_text(*object, PrintStateBase::SlicingModifierNoOverrides),
               Catch::Matchers::Contains("text_modifier"));
    CHECK(distinct_layer_regions(*object) == 1);
}

TEST_CASE("modifier paint note selection", "[ModifierEffect]")
{
    // No painting under the modifier, or no explicit filament: nothing to add.
    CHECK(classify_modifier_paint_note(true,  true,  false, false) == ModifierPaintNote::None);
    CHECK(classify_modifier_paint_note(true,  false, false, true)  == ModifierPaintNote::None);
    CHECK(classify_modifier_paint_note(false, false, true,  true)  == ModifierPaintNote::None);
    // Same filament as the object on a painted part.
    CHECK(classify_modifier_paint_note(true,  true,  false, true)  == ModifierPaintNote::SameFilamentPainted);
    // Different filament on a painted part; a real override of something else with the same filament is not one.
    CHECK(classify_modifier_paint_note(false, true,  true,  true)  == ModifierPaintNote::FilamentOverriddenByPaint);
    CHECK(classify_modifier_paint_note(false, true,  false, true)  == ModifierPaintNote::None);
}

TEST_CASE("same-filament modifier on a painted part explains the paint override", "[ModifierEffect]")
{
    Print print;
    Model model;
    ModifierSpec spec;
    spec.paint_part = true;
    spec.overrides  = [](ModelVolume &v) { v.config.set("extruder", 1); };
    const PrintObject *object = slice_with_modifier(print, model, spec);

    CHECK(has_slice_warning(*object, PrintStateBase::SlicingModifierSameFilamentPainted));
    CHECK_THAT(slice_warning_text(*object, PrintStateBase::SlicingModifierSameFilamentPainted),
               Catch::Matchers::Contains("painted areas override"));
    // Replaces the plain notice instead of stacking on it.
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierNoOverrides));
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierOverriddenByPaint));
}

TEST_CASE("different-filament modifier on a painted part warns that paint wins", "[ModifierEffect]")
{
    Print print;
    Model model;
    ModifierSpec spec;
    spec.paint_part = true;
    spec.overrides  = [](ModelVolume &v) { v.config.set("extruder", 2); };
    const PrintObject *object = slice_with_modifier(print, model, spec);

    CHECK(has_slice_warning(*object, PrintStateBase::SlicingModifierOverriddenByPaint));
    CHECK_THAT(slice_warning_text(*object, PrintStateBase::SlicingModifierOverriddenByPaint),
               Catch::Matchers::Contains("override the filament of modifier"));
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierSameFilamentPainted));
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierNoOverrides));
}

TEST_CASE("modifier outside the part's bounding box warns that it overlaps nothing", "[ModifierEffect]")
{
    Print print;
    Model model;
    ModifierSpec spec;
    // Far clear of the 20 mm part in X - no bounding box overlap in any layer range, so there is
    // no part for it to attach to and no fix can give it one.
    spec.offset    = Vec3d(60., 0., 0.);
    spec.overrides = override_wall_loops;
    const PrintObject *object = slice_with_modifier(print, model, spec);

    CHECK(has_slice_warning(*object, PrintStateBase::SlicingModifierNoParent));

    const std::string text = slice_warning_text(*object, PrintStateBase::SlicingModifierNoParent);
    CHECK_THAT(text, Catch::Matchers::Contains("svg_modifier"));
    CHECK_THAT(text, Catch::Matchers::Contains("cube_with_modifier"));
    CHECK_THAT(text, Catch::Matchers::Contains("does not overlap"));

    // Nothing attached, so the part is all there is.
    CHECK(distinct_layer_regions(*object) == 1);
}

// Orca #13971: group_fills() merges the fill surfaces of regions whose fill parameters compare equal
// and extrudes the lot with the first region's config. Bridges carried no speed in those parameters,
// so the bridge infill of a modifier with its own bridge speed went into the part's bridge fill and was
// printed at the part's speed. The modifier's half of the deck must keep its own bridge fill.
TEST_CASE("a modifier with its own bridge speed keeps its own bridge infill", "[ModifierEffect][Bridge]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "bridge_speed",               "50" },
        { "enable_support",             false },
        { "sparse_infill_density",      "15%" },
    });

    // Two 5 mm piers 20 mm apart carrying a 30 x 20 x 1 mm deck at z = [5, 6]: the deck's first layer
    // is a bridge across x = [5, 25] along its whole depth.
    Model        model;
    ModelObject *object = model.add_object();
    object->name = "bridge_with_modifier";
    object->add_volume(make_cube(5., 20., 5.), ModelVolumeType::MODEL_PART, false);
    {
        TriangleMesh pier = make_cube(5., 20., 5.);
        pier.translate(25.f, 0.f, 0.f);
        object->add_volume(std::move(pier), ModelVolumeType::MODEL_PART, false);
        TriangleMesh deck = make_cube(30., 20., 1.);
        deck.translate(0.f, 0.f, 5.f);
        object->add_volume(std::move(deck), ModelVolumeType::MODEL_PART, false);
    }
    // The back half of the deck (y = [10, 20]) gets a slower bridge speed and nothing else.
    {
        TriangleMesh box = make_cube(30., 10., 3.);
        box.translate(0.f, 10.f, 4.f);
        ModelVolume *modifier = object->add_volume(std::move(box), ModelVolumeType::PARAMETER_MODIFIER, false);
        modifier->name = "slow_bridge";
        modifier->config.set_key_value("bridge_speed", new ConfigOptionFloats({ 20. }));
    }
    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
    REQUIRE(! print.objects().empty());

    // Bridge infill length on the deck's first layer, per bridge speed of the region that prints it.
    std::map<double, double> bridge_length_by_speed;
    const std::function<double(const ExtrusionEntity *)> bridge_length = [&bridge_length](const ExtrusionEntity *entity) -> double {
        if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            double len = 0.;
            for (const ExtrusionEntity *child : coll->entities)
                len += bridge_length(child);
            return len;
        }
        if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
            return path->role() == erBridgeInfill ? path->length() : 0.;
        if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
            double len = 0.;
            for (const ExtrusionPath &path : multi->paths)
                if (path.role() == erBridgeInfill)
                    len += path.length();
            return len;
        }
        return 0.;
    };
    bool found_layer = false;
    for (const Layer *layer : print.objects().front()->layers()) {
        if (std::abs(layer->print_z - 5.2) > EPSILON)
            continue;
        found_layer = true;
        for (const LayerRegion *region : layer->regions())
            bridge_length_by_speed[region->region().config().bridge_speed.get_at(0)] += bridge_length(&region->fills);
    }
    REQUIRE(found_layer);

    // Both halves of the deck bridge, each in the region whose bridge speed it is printed with.
    // Path lengths are in scaled units. Each half of the 20 x 10 mm span takes well over 50 mm of bridge lines.
    CHECK(bridge_length_by_speed[50.] > scale_(50.));
    CHECK(bridge_length_by_speed[20.] > scale_(50.));
}

// ---------------------------------------------------------------------------------------------
// Override paint (feat/modifier-override-paint): a modifier that sets a filament can win over colour
// painting inside its volume. The owner's case: an emboss text modifier set to orange on a body painted
// grey all over prints orange text. Modelled with the harness above: a two-filament 20 mm cube with
// extruder 1 painted entirely with filament 2, and a 6 mm modifier box that pokes through the cube's
// top face (so its footprint holds visible top skin and walls on the top layers).

namespace {

const Vec3d k_modifier_through_top { 0., 0., 8. }; // modifier spans z = [5, 11] around the cube's [-10, 10]

void set_override_paint(ModelVolume &v) { v.config.set_key_value("override_paint", new ConfigOptionBool(true)); }

// Area in mm^2 the given layer prints with walls in filament `wall_filament`, and whether that area also
// has perimeters and fill extrusions, every one of them with solid infill in the same filament.
struct FilamentArea
{
    double area { 0. };
    bool   has_perimeters { false };
    bool   has_fills { false };
    bool   solid_matches { true };
};

FilamentArea filament_area(const Layer &layer, int wall_filament)
{
    FilamentArea out;
    for (const LayerRegion *lregion : layer.regions()) {
        const PrintRegionConfig &cfg = lregion->region().config();
        if (cfg.wall_filament.value != wall_filament || lregion->slices.empty())
            continue;
        for (const Surface &s : lregion->slices.surfaces)
            out.area += s.expolygon.area() * SCALING_FACTOR * SCALING_FACTOR;
        out.has_perimeters = out.has_perimeters || ! lregion->perimeters.entities.empty();
        out.has_fills      = out.has_fills || ! lregion->fills.entities.empty();
        out.solid_matches  = out.solid_matches && cfg.solid_infill_filament.value == wall_filament;
    }
    return out;
}

const Layer &top_layer(const PrintObject &object)
{
    REQUIRE(! object.layers().empty());
    return *object.layers().back();
}

} // namespace

TEST_CASE("override paint: a filament-1 modifier on a part painted with filament 2 prints filament 1", "[ModifierEffect][OverridePaint]")
{
    Print print;
    Model model;
    ModifierSpec spec;
    spec.paint_part = true;
    spec.offset     = k_modifier_through_top;

    SECTION("flag on: the modifier's footprint keeps filament 1 for walls and top skin") {
        spec.overrides = [](ModelVolume &v) { v.config.set("extruder", 1); set_override_paint(v); };
        const PrintObject *object = slice_with_modifier(print, model, spec);
        const Layer       &top    = top_layer(*object);
        const FilamentArea f1     = filament_area(top, 1);
        // The 6 x 6 mm footprint, give or take the segmentation's trimming.
        CHECK(f1.area > 30.);
        CHECK(f1.area < 40.);
        CHECK(f1.has_perimeters);
        CHECK(f1.has_fills);
        CHECK(f1.solid_matches);
        // The rest of the top face is still painted.
        CHECK(filament_area(top, 2).area > 300.);
        // And it is the modifier's own region doing it, flagged as such.
        bool flagged = false;
        for (const LayerRegion *lregion : top.regions())
            if (! lregion->slices.empty() && lregion->region().config().override_paint.value &&
                lregion->region().config().wall_filament.value == 1)
                flagged = true;
        CHECK(flagged);
    }
    SECTION("flag off: paint wins, the top layer is filament 2 throughout") {
        spec.overrides = [](ModelVolume &v) { v.config.set("extruder", 1); };
        const PrintObject *object = slice_with_modifier(print, model, spec);
        const Layer       &top    = top_layer(*object);
        CHECK(filament_area(top, 1).area < 1.);
        CHECK(filament_area(top, 2).area > 380.);
    }
    SECTION("flag on with filament 2 on a part of filament 1 painted 2: still filament 2, no paint warning") {
        spec.overrides = [](ModelVolume &v) { v.config.set("extruder", 2); set_override_paint(v); };
        const PrintObject *object = slice_with_modifier(print, model, spec);
        CHECK(filament_area(top_layer(*object), 2).area > 380.);
        CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierOverriddenByPaint));
    }
}

TEST_CASE("override paint: the flag is dropped where it can change nothing", "[ModifierEffect][OverridePaint]")
{
    Print print;
    Model model;
    ModifierSpec spec;
    spec.offset = k_modifier_through_top;

    SECTION("modifier without a filament of its own: flag ignored, still the plain no-effect notice") {
        spec.paint_part = true;
        spec.overrides  = set_override_paint; // extruder stays 0 ("default")
        const PrintObject *object = slice_with_modifier(print, model, spec);
        CHECK(has_slice_warning(*object, PrintStateBase::SlicingModifierNoOverrides));
        for (const Layer *layer : object->layers())
            for (const LayerRegion *lregion : layer->regions())
                CHECK_FALSE(lregion->region().config().override_paint.value);
    }
    SECTION("unpainted part, same filament: no region of its own and the plain no-effect notice") {
        spec.overrides = [](ModelVolume &v) { v.config.set("extruder", 1); set_override_paint(v); };
        const PrintObject *object = slice_with_modifier(print, model, spec);
        CHECK(distinct_layer_regions(*object) == 1);
        CHECK(has_slice_warning(*object, PrintStateBase::SlicingModifierNoOverrides));
    }
}

TEST_CASE("override paint: warnings from paint shadowing do not fire for the modifier", "[ModifierEffect][OverridePaint]")
{
    // Selection: Override paint suppresses both paint notes.
    CHECK(classify_modifier_paint_note(true,  true, false, true, true) == ModifierPaintNote::None);
    CHECK(classify_modifier_paint_note(false, true, true,  true, true) == ModifierPaintNote::None);
    CHECK(classify_modifier_paint_note(false, true, true,  true, false) == ModifierPaintNote::FilamentOverriddenByPaint);

    // End to end, the same-filament case of the #843 test above with the flag on.
    Print print;
    Model model;
    ModifierSpec spec;
    spec.paint_part = true;
    spec.overrides  = [](ModelVolume &v) { v.config.set("extruder", 1); set_override_paint(v); };
    const PrintObject *object = slice_with_modifier(print, model, spec);
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierSameFilamentPainted));
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierOverriddenByPaint));
    CHECK_FALSE(has_slice_warning(*object, PrintStateBase::SlicingModifierNoOverrides));
}

TEST_CASE("override paint: the flag survives a 3MF round trip on the modifier", "[ModifierEffect][OverridePaint][3MF]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "override_paint_3mf";
    ModelVolume *part = object->add_volume(TriangleMesh(its_make_cube(20., 20., 20.)));
    part->name = "part";
    ModelVolume *modifier = object->add_volume(TriangleMesh(its_make_cube(6., 6., 6.)));
    modifier->name = "text_modifier";
    modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set("extruder", 2);
    set_override_paint(*modifier);
    object->add_instance();

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string  file   = (tmp_root / "override_paint_roundtrip.3mf").string();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    StoreParams store_params;
    store_params.path     = file.c_str();
    store_params.model    = &model;
    store_params.config   = &config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    Model                     loaded;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded_ok = load_bbs_3mf(file.c_str(), &dst_config, &ctxt, &loaded, &plate_data, &project_presets, &is_bbl_3mf,
                                        &file_version, nullptr,
                                        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                            LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(file);
    REQUIRE(loaded_ok);

    REQUIRE(loaded.objects.size() == 1);
    const ModelVolume *loaded_modifier = nullptr;
    const ModelVolume *loaded_part     = nullptr;
    for (const ModelVolume *v : loaded.objects.front()->volumes)
        (v->is_modifier() ? loaded_modifier : loaded_part) = v;
    REQUIRE(loaded_modifier != nullptr);
    REQUIRE(loaded_part != nullptr);
    const ConfigOption *opt = loaded_modifier->config.option("override_paint");
    REQUIRE(opt != nullptr);
    CHECK(opt->getBool());
    CHECK(model_volume_overrides_paint(*loaded_modifier));
    // Only the modifier carries it, and the project config never does.
    CHECK(loaded_part->config.option("override_paint") == nullptr);
    const bool project_has_it = dst_config.has("override_paint") && dst_config.opt_bool("override_paint");
    CHECK_FALSE(project_has_it);
}
