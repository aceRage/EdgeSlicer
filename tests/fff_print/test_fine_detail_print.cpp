// EdgeSlicer, Fine detail (tests/design_arachne_thin_features.md, P2): end to end on a small fin plate.
//
// A 24 x 20 x 1 mm base with three fins standing on it: A (0.25 mm) at x 6, B (0.25 mm) at x 12, C (1.2 mm) at x 18,
// each 16 mm long and up to z 3. C always prints, so no layer is ever empty. The perimeters are read at z 2.0, where
// only the fins are.

#include <catch2/catch.hpp>

#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kFinA = 6., kFinB = 12., kFinC = 18.;
constexpr double kThin = 0.25;

TriangleMesh fin_plate()
{
    TriangleMesh mesh = make_cube(24., 20., 1.);
    const std::vector<std::pair<double, double>> fins{ { kFinA, kThin }, { kFinB, kThin }, { kFinC, 1.2 } };
    for (const auto &[x, w] : fins) {
        TriangleMesh fin = make_cube(w, 16., 2.1);
        fin.translate(float(x - w / 2.), 2.f, 0.9f);
        mesh.merge(fin);
    }
    return mesh;
}

DynamicPrintConfig base_config(const char *generator)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "wall_generator",               generator },
        { "layer_height",                 "0.2" },
        { "initial_layer_print_height",   "0.2" },
        { "wall_loops",                   "2" },
        { "precise_outer_wall",           "0" },
        { "detect_thin_wall",             "0" },
        { "outer_wall_speed",             "200" },
        { "inner_wall_speed",             "200" },
        // Keep the feedrates about the settings: no cooling slowdown, no volumetric cap.
        { "slow_down_layer_time",         "0" },
        { "slow_down_layers",             "0" },
        { "filament_max_volumetric_speed", "100" },
        { "enable_overhang_speed",        "0" },
    });
    return config;
}

// Slice the plate; `modifier` (may be empty) is applied to a modifier box around fin B.
void slice_plate(Print &print, Model &model, const DynamicPrintConfig &config, const DynamicPrintConfig &modifier)
{
    ModelObject *object = model.add_object();
    object->name        = "fin_plate";
    object->add_volume(fin_plate());
    if (!modifier.empty()) {
        TriangleMesh box = make_cube(4., 20., 10.);
        box.translate(float(kFinB - 2.), 0.f, 0.f);
        ModelVolume *mv = object->add_volume(std::move(box));
        mv->set_type(ModelVolumeType::PARAMETER_MODIFIER);
        for (const std::string &key : modifier.keys())
            mv->config.set_key_value(key, modifier.option(key)->clone());
    }
    object->add_instance();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

// Widths of the perimeter paths at z 2.0 whose centre lies within 1 mm of model x = `fin_x`.
std::vector<double> fin_widths(const Print &print, double fin_x)
{
    const PrintObject *object = print.objects().front();
    const double       offset = unscaled<double>(object->center_offset().x());
    std::vector<double> out;
    std::function<void(const ExtrusionEntity &)> visit = [&](const ExtrusionEntity &ee) {
        if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(&ee)) {
            for (const ExtrusionEntity *e : coll->entities)
                visit(*e);
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&ee)) {
            for (const ExtrusionPath &p : loop->paths)
                visit(p);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&ee)) {
            for (const ExtrusionPath &p : multi->paths)
                visit(p);
        } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&ee)) {
            const BoundingBox bb = path->polyline.bounding_box();
            const double      cx = unscaled<double>(0.5 * double(bb.min.x() + bb.max.x())) + offset;
            if (std::abs(cx - fin_x) < 1.)
                out.push_back(path->width);
        }
    };
    for (const Layer *layer : object->layers()) {
        if (std::abs(layer->print_z - 2.0) > 0.05)
            continue;
        for (const LayerRegion *region : layer->regions())
            visit(region->perimeters);
    }
    return out;
}

// G-code without comment lines: the config block at the end names every key, which is allowed to differ.
std::string strip_comments(const std::string &gcode)
{
    std::istringstream in(gcode);
    std::string        line, out;
    while (std::getline(in, line))
        if (line.empty() || line.front() != ';')
            out += line + "\n";
    return out;
}

} // namespace

TEST_CASE("Fine detail: a modifier gives its region the level, the rest of the object keeps Off", "[FineDetail]")
{
    const DynamicPrintConfig config = base_config("arachne");
    DynamicPrintConfig       modifier;
    modifier.set_key_value("fine_detail", new ConfigOptionEnum<FineDetailLevel>(FineDetailLevel::FineText));

    Print print;
    Model model;
    slice_plate(print, model, config, modifier);

    const std::vector<double> a = fin_widths(print, kFinA);
    const std::vector<double> b = fin_widths(print, kFinB);
    CAPTURE(a, b);
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());
    // A, outside the modifier: widened to the Thinnest line, a 0.34 + 0.04 mm line.
    for (const double w : a)
        CHECK_THAT(w, WithinAbs(0.383, 0.01));
    // B, inside: Fine text lets the 0.25 mm fin (0.207 after the outer wall's inset) print about its own width.
    for (const double w : b) {
        CHECK(w < 0.30);
        CHECK(w >= 0.2); // never narrower than the layer is tall
    }
}

TEST_CASE("Fine detail on the Classic generator prints thin fins it would skip", "[FineDetail]")
{
    DynamicPrintConfig config = base_config("classic");
    {
        Print print;
        Model model;
        slice_plate(print, model, config, DynamicPrintConfig());
        // As shipped on the BBL / U1 presets: a fin narrower than a line is not printed.
        CHECK(fin_widths(print, kFinA).empty());
        CHECK(fin_widths(print, kFinB).empty());
        CHECK_FALSE(fin_widths(print, kFinC).empty());
    }
    config.set_key_value("fine_detail", new ConfigOptionEnum<FineDetailLevel>(FineDetailLevel::Balanced));
    {
        Print print;
        Model model;
        slice_plate(print, model, config, DynamicPrintConfig());
        const std::vector<double> a = fin_widths(print, kFinA);
        CAPTURE(a);
        REQUIRE_FALSE(a.empty());
        // One line of about the fin's own width (thin-wall detection).
        for (const double w : a)
            CHECK(w < 0.35);
    }
}

TEST_CASE("Fine detail: thin lines print no faster than Fine detail speed", "[FineDetail]")
{
    DynamicPrintConfig config = base_config("arachne");
    config.set_deserialize_strict({ { "fine_detail", "fine_text" }, { "fine_detail_speed", "20" } });

    Print                print;
    Model                model;
    GCodeProcessorResult result;
    {
        ModelObject *object = model.add_object();
        object->add_volume(fin_plate());
        object->add_instance();
        print.auto_assign_extruders(object);
        print.apply(model, config);
    }
    Test::gcode(print, result);

    size_t thin = 0, normal_fast = 0;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.type != EMoveType::Extrude || move.layer_id < 2 ||
            (move.extrusion_role != erExternalPerimeter && move.extrusion_role != erPerimeter))
            continue;
        if (move.width < 0.30f) {
            ++thin;
            CHECK(move.feedrate <= 20.f + 1e-3f);
        } else if (move.feedrate > 100.f)
            ++normal_fast;
    }
    CHECK(thin > 0);
    // Lines of a normal width keep their own speed.
    CHECK(normal_fast > 0);
}

TEST_CASE("Fine detail Off ignores the raw keys: the same G-code", "[FineDetail]")
{
    const DynamicPrintConfig config = base_config("arachne");
    DynamicPrintConfig       touched = config;
    touched.set_deserialize_strict({ { "thin_feature_line_width", "30%" }, { "thin_feature_min_size", "5%" }, { "fine_detail_speed", "5" } });

    auto slice = [](const DynamicPrintConfig &c) {
        Print print;
        Model model;
        ModelObject *object = model.add_object();
        object->add_volume(fin_plate());
        object->add_instance();
        print.auto_assign_extruders(object);
        print.apply(model, c);
        return strip_comments(Test::gcode(print));
    };
    const std::string plain = slice(config);
    REQUIRE_FALSE(plain.empty());
    CHECK(plain == slice(touched));
}
