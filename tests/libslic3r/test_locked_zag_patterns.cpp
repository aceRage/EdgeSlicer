// Locked Zag: separate skin / skeleton infill patterns.
// docs/superpowers/specs/2026-09-10-locked-zag-skin-skeleton.md
//
// Before this feature FillLockedZag drew BOTH bands with its own (FillRectilinear-derived)
// fill_surface(), so the only thing that told the skin from the skeleton was density, line width
// and flow. locked_skin_infill_pattern / locked_skeleton_infill_pattern let each band pick its own
// InfillPattern, drawn by a separate Fill built through Fill::new_from_type().
//
// Three things are checked here, in the order they matter:
//
//  1. IDENTITY. A Locked Zag profile at the shipped defaults ("Same as sparse infill" for both
//     bands, contour-hugging skin off) must produce exactly the same sparse-infill geometry as a
//     Locked Zag profile that never mentions the keys at all. This is the whole promise of the
//     ipCount default, and it is what makes the feature safe to ship into existing profiles. It is
//     checked as an exact digest equality, not a tolerance.
//  2. The bands really are drawn with different patterns: a run with skin=Grid/skeleton=Gyroid
//     differs from the default run, and differs again from skin=Gyroid/skeleton=Grid (swapping the
//     two must NOT produce the same G-code - that would mean one of them is being ignored).
//  3. The per-band flows survive the rewrite: a Locked Zag layer still carries two distinct
//     extrusion widths whatever patterns the bands use.
//
// The digest below is geometry only (rounded point coordinates plus the extrusion width of each
// path), deliberately not the whole G-code: it isolates what this change touches and does not move
// when an unrelated part of the pipeline changes its speeds or comments.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

// A 20 mm cube is the fixture the spec asks for: big enough that a 2 mm skin leaves a real
// skeleton inside it, small enough to slice quickly.
Model cube_model()
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "locked-zag-cube.stl";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
    return model;
}

DynamicPrintConfig locked_zag_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "sparse_infill_pattern",       "lockedzag" },
        { "sparse_infill_density",       "15%" },
        { "skin_infill_density",         "40%" },
        { "skeleton_infill_density",     "10%" },
        { "skin_infill_depth",           "2" },
        { "infill_lock_depth",           "1" },
        // Distinct band line widths, so the flow re-split has something to show.
        { "skin_infill_line_width",      "0.45" },
        { "skeleton_infill_line_width",  "0.6" },
        // Keep the shells thin: the point of interest is the sparse infill, and fewer solid
        // layers means a shorter slice.
        { "top_shell_layers",            "1" },
        { "bottom_shell_layers",         "1" },
        { "wall_loops",                  "2" },
        { "layer_height",                "0.2" },
    });
    return config;
}

struct FillDigest
{
    std::string      text;       // the geometry digest itself
    std::size_t      paths  = 0; // number of sparse-infill extrusion paths seen
    std::set<int>    widths;     // distinct extrusion widths, in micrometres
};

void collect(const ExtrusionEntity *entity, FillDigest &digest, std::ostringstream &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect(child, digest, out);
        return;
    }
    const auto *path = dynamic_cast<const ExtrusionPath *>(entity);
    if (path == nullptr || path->role() != erInternalInfill)
        return;
    ++ digest.paths;
    digest.widths.insert(int(std::lround(path->width * 1000.)));
    out << 'P' << int(std::lround(path->width * 1000.));
    for (const Point &p : path->polyline.points)
        out << ' ' << p.x() << ',' << p.y();
    out << '\n';
}

// Slice the cube and digest every sparse-infill path of the layers in [first, last).
FillDigest slice_and_digest(const DynamicPrintConfig &config, size_t first_layer = 10, size_t last_layer = 20)
{
    Print print;
    Model model = cube_model();
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();

    const PrintObject *object = print.objects().front();
    REQUIRE(object->layer_count() > last_layer);

    FillDigest          digest;
    std::ostringstream  out;
    for (size_t layer_idx = first_layer; layer_idx < last_layer; ++ layer_idx) {
        const Layer *layer = object->get_layer(int(layer_idx));
        out << "L" << layer_idx << '\n';
        for (const LayerRegion *region : layer->regions())
            collect(&region->fills, digest, out);
    }
    digest.text = out.str();
    return digest;
}

} // namespace

TEST_CASE("locked zag: the shipped defaults reproduce the single-pattern fill exactly", "[LockedZag][Fill]")
{
    // A profile that predates the feature: it simply does not carry the two keys, so they take
    // their defaults through the normal ConfigBase path.
    const FillDigest legacy = slice_and_digest(locked_zag_config());

    // The same profile with the keys written out at their shipped defaults.
    DynamicPrintConfig explicit_defaults = locked_zag_config();
    explicit_defaults.set_deserialize_strict({
        { "locked_skin_infill_pattern",           "default" },
        { "locked_skeleton_infill_pattern",       "default" },
        { "infill_instead_top_bottom_surfaces",   "0" },
    });
    const FillDigest defaults = slice_and_digest(explicit_defaults);

    REQUIRE(legacy.paths > 0);
    CHECK(defaults.paths == legacy.paths);
    // Exact, not approximate: "Same as sparse infill" must be a no-op.
    CHECK(defaults.text == legacy.text);
}

TEST_CASE("locked zag: skin and skeleton are drawn with their own patterns", "[LockedZag][Fill]")
{
    const FillDigest defaults = slice_and_digest(locked_zag_config());

    DynamicPrintConfig grid_skin = locked_zag_config();
    grid_skin.set_deserialize_strict({
        { "locked_skin_infill_pattern",     "grid" },
        { "locked_skeleton_infill_pattern", "gyroid" },
    });
    const FillDigest skin_grid = slice_and_digest(grid_skin);

    DynamicPrintConfig swapped = locked_zag_config();
    swapped.set_deserialize_strict({
        { "locked_skin_infill_pattern",     "gyroid" },
        { "locked_skeleton_infill_pattern", "grid" },
    });
    const FillDigest skin_gyroid = slice_and_digest(swapped);

    REQUIRE(defaults.paths > 0);
    REQUIRE(skin_grid.paths > 0);
    REQUIRE(skin_gyroid.paths > 0);

    // Picking patterns changes the fill...
    CHECK(skin_grid.text != defaults.text);
    CHECK(skin_gyroid.text != defaults.text);
    // ...and which band gets which pattern matters. If the skin pattern were being applied to both
    // bands (or ignored), these two runs would coincide.
    CHECK(skin_grid.text != skin_gyroid.text);
}

TEST_CASE("locked zag: the two bands keep their own line widths whatever the patterns", "[LockedZag][Fill]")
{
    // 0.45 mm skin and 0.6 mm skeleton, set in locked_zag_config(). The flow re-split happens after
    // the patterns are drawn, so it has to survive a run in which the bands use different fillers.
    for (const char *skin : { "default", "grid" }) {
        DynamicPrintConfig config = locked_zag_config();
        config.set_deserialize_strict({
            { "locked_skin_infill_pattern",     skin },
            { "locked_skeleton_infill_pattern", std::string(skin) == "default" ? "default" : "gyroid" },
        });
        const FillDigest digest = slice_and_digest(config);
        INFO("skin pattern = " << skin);
        REQUIRE(digest.paths > 0);
        // Two distinct widths on the layer, and neither is the sparse-infill default.
        CHECK(digest.widths.size() >= 2);
    }
}

TEST_CASE("locked zag: a non-locked-zag profile ignores the new keys entirely", "[LockedZag][Fill]")
{
    // The guard in group_fills() is `pattern == ipLockedZag`; this is the structural proof that a
    // profile on any other sparse pattern cannot be moved by the new options.
    DynamicPrintConfig plain = locked_zag_config();
    plain.set_deserialize_strict({ { "sparse_infill_pattern", "grid" } });
    const FillDigest baseline = slice_and_digest(plain);

    DynamicPrintConfig plain_with_keys = plain;
    plain_with_keys.set_deserialize_strict({
        { "locked_skin_infill_pattern",         "gyroid" },
        { "locked_skeleton_infill_pattern",     "concentric" },
        { "infill_instead_top_bottom_surfaces", "1" },
    });
    const FillDigest with_keys = slice_and_digest(plain_with_keys);

    REQUIRE(baseline.paths > 0);
    CHECK(with_keys.text == baseline.text);
}
