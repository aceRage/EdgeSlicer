// EdgeSlicer, thin feature notice (tests/design_arachne_thin_features.md, P3): end to end through the real wall
// generators.
//
// A 24 x 20 x 1 mm base with five fins standing on it, 16 mm long and up to z 3: 0.08, 0.16, 0.22, 0.30 and 1.2 mm wide.
// The 1.2 mm fin always prints and is no thin feature. The notice is read from the object's slicing warnings.
//
// Model widths at which the limits switch (0.4 nozzle, 0.2 mm layers, precise outer wall off, so the wall inset takes
// 0.043 mm off a feature):
//   Arachne Off        skip under 0.143, widened under 0.345      fins: skipped 0.08; widened 0.16 0.22 0.30
//   Arachne Balanced   skip under 0.143, widened under 0.255      fins: skipped 0.08; widened 0.16 0.22
//   Arachne Fine text  skip under 0.103, widened under 0.201      fins: skipped 0.08; widened 0.16
//   Classic as shipped skip under the smaller external line (0.38 - 0.41 mm, a narrow island)
//                                                                  fins: skipped 0.08 0.16 0.22 0.30
//   Classic with thin walls (Detect thin wall, or any Fine detail level) skip under nozzle / 3 = 0.133
//                                                                  fins: skipped 0.08
// (the #1101 width table: a 0.25 mm fin is dropped by Classic and printed by Classic + Fine detail.)

#include <catch2/catch.hpp>

#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

TriangleMesh fin_plate()
{
    TriangleMesh mesh = make_cube(24., 20., 1.);
    const std::vector<std::pair<double, double>> fins{ { 4., 0.08 }, { 8., 0.16 }, { 12., 0.22 }, { 16., 0.30 }, { 20., 1.2 } };
    for (const auto &[x, w] : fins) {
        TriangleMesh fin = make_cube(w, 16., 2.1);
        fin.translate(float(x - w / 2.), 2.f, 0.9f);
        mesh.merge(fin);
    }
    return mesh;
}

DynamicPrintConfig base_config(const char *generator, const char *fine_detail)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",              generator },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "wall_loops",                  "2" },
        { "precise_outer_wall",          "0" },
        { "detect_thin_wall",            "0" },
        { "fine_detail",                 fine_detail },
        { "slow_down_layer_time",        "0" },
        { "slow_down_layers",            "0" },
    });
    return config;
}

struct Sliced
{
    Print print;
    Model model;
    const PrintObject &object() const { return *print.objects().front(); }
    std::string notice() const
    {
        for (const PrintStateBase::Warning &w : object().step_state_with_warnings(posPerimeters).warnings)
            if (w.message_id == int(PrintStateBase::SlicingThinFeatures))
                return w.message;
        return {};
    }
};

void slice(Sliced &s, TriangleMesh mesh, const DynamicPrintConfig &config)
{
    ModelObject *object = s.model.add_object();
    object->name        = "plate";
    object->add_volume(std::move(mesh));
    object->add_instance();
    s.print.auto_assign_extruders(object);
    s.print.apply(s.model, config);
    s.print.set_status_silent();
    s.print.process();
}

struct Counts
{
    size_t skipped, widened;
};

void check_counts(const char *generator, const char *fine_detail, Counts want)
{
    Sliced s;
    slice(s, fin_plate(), base_config(generator, fine_detail));
    const ThinFeatureSummary &summary = s.object().thin_feature_summary();
    CAPTURE(generator, fine_detail, s.notice());
    CHECK(summary.skipped.count == want.skipped);
    CHECK(summary.widened.count == want.widened);
    // A notice exactly when something was found (every fin is 16 mm long over ten layers, far over the 5 mm floor).
    CHECK(s.notice().empty() == (want.skipped == 0 && want.widened == 0));
}

} // namespace

TEST_CASE("Thin feature notice: Arachne counts per Fine detail level", "[ThinFeatures]")
{
    check_counts("arachne", "off", { 1, 3 });
    check_counts("arachne", "balanced", { 1, 2 });
    check_counts("arachne", "fine_text", { 1, 1 });
}

TEST_CASE("Thin feature notice: Classic counts per Fine detail level", "[ThinFeatures]")
{
    // As shipped on the BBL / U1 / CC presets: everything thinner than the external line is not printed.
    check_counts("classic", "off", { 4, 0 });
    // Fine detail turns thin-wall detection on, which prints down to nozzle / 3.
    check_counts("classic", "balanced", { 1, 0 });
    check_counts("classic", "fine_text", { 1, 0 });
}

TEST_CASE("Thin feature notice: a plain cube produces no notice", "[ThinFeatures]")
{
    for (const char *generator : { "arachne", "classic" })
        for (const char *level : { "off", "fine_text" }) {
            DYNAMIC_SECTION(generator << " " << level) {
                Sliced s;
                slice(s, make_cube(20., 20., 20.), base_config(generator, level));
                CHECK(s.object().thin_feature_summary().skipped.count == 0);
                CHECK(s.object().thin_feature_summary().widened.count == 0);
                CHECK(s.notice().empty());
            }
        }
}

TEST_CASE("Thin feature notice: the suggestion fits the situation", "[ThinFeatures]")
{
    {
        // Arachne Off: Fine text would print them thinner.
        Sliced s;
        slice(s, fin_plate(), base_config("arachne", "off"));
        const std::string text = s.notice();
        CAPTURE(text);
        CHECK(text.find("plate: ") == 0);
        CHECK(text.find("printed wider than the model") != std::string::npos);
        CHECK(text.find("skipped") != std::string::npos);
        CHECK(text.find("Set Fine detail to Fine text") != std::string::npos);
    }
    {
        // Arachne Fine text: already the thinnest line this nozzle and layer give, nothing to suggest.
        Sliced s;
        slice(s, fin_plate(), base_config("arachne", "fine_text"));
        const std::string text = s.notice();
        CAPTURE(text);
        REQUIRE_FALSE(text.empty());
        CHECK(text.find("Set Fine detail") == std::string::npos);
    }
    {
        // Classic without thin walls: both switches are named.
        Sliced s;
        slice(s, fin_plate(), base_config("classic", "off"));
        const std::string text = s.notice();
        CAPTURE(text);
        CHECK(text.find("not printed") != std::string::npos);
        CHECK(text.find("turn on Detect thin wall") != std::string::npos);
    }
    {
        // Classic with Fine detail: thin walls are on, only what is under nozzle / 3 remains and nothing helps.
        Sliced s;
        slice(s, fin_plate(), base_config("classic", "fine_text"));
        const std::string text = s.notice();
        CAPTURE(text);
        REQUIRE_FALSE(text.empty());
        CHECK(text.find("Set Fine detail") == std::string::npos);
    }
}

TEST_CASE("Thin feature notice: the layers keep the spots for a later overlay", "[ThinFeatures]")
{
    Sliced s;
    slice(s, fin_plate(), base_config("arachne", "off"));
    size_t layers_with_spots = 0, skipped_spots = 0, widened_spots = 0;
    for (const Layer *layer : s.object().layers())
        for (const LayerRegion *region : layer->regions())
            for (const ThinFeatureLayer &found : region->thin_feature_layers) {
                ++layers_with_spots;
                skipped_spots += found.skipped.size();
                widened_spots += found.widened.size();
            }
    // The fins stand from z 1.0 to 3.0: ten layers, one spot per thin fin per layer.
    CHECK(layers_with_spots >= 9);
    CHECK(skipped_spots >= 9);
    CHECK(widened_spots >= 27);
}
