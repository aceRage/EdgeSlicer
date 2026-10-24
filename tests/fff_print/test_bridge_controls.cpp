#include <catch2/catch.hpp>

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// OrcaSlicer #12055 (relative bridge direction, bridge and ironing angles aligned with the model)
// and #11255 (bridge line width, wider bridge density range).

namespace {

// Two pillars joined by a slab: the slab underside is an external bridge over an 18 mm gap.
TriangleMesh bridge_slab()
{
    TriangleMesh mesh = make_cube(6., 10., 5.);
    TriangleMesh pillar = make_cube(6., 10., 5.);
    pillar.translate(24.f, 0.f, 0.f);
    mesh.merge(pillar);
    TriangleMesh slab = make_cube(30., 10., 1.5);
    slab.translate(0.f, 0.f, 4.5f);
    mesh.merge(slab);
    return mesh;
}

// A 30x24 base with a smaller tower on it: the shoulder's skin is an internal bridge over the
// base's sparse infill.
TriangleMesh internal_bridge_mesh()
{
    TriangleMesh mesh = make_cube(30, 24, 3);
    TriangleMesh tower = make_cube(14, 10, 1);
    tower.translate(8, 7, 3);
    mesh.merge(tower);
    return mesh;
}

DynamicPrintConfig bridge_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "bottom_shell_layers", "3" },
        { "top_shell_layers", "3" },
        { "top_shell_thickness", "0" },
        { "bottom_shell_thickness", "0" },
        { "sparse_infill_density", "15%" },
        { "sparse_infill_pattern", "rectilinear" },
        { "infill_direction", "45" },
        { "thick_internal_bridges", "1" },
    });
    return config;
}

// Bridge angle (radians) of every bridge surface of the given type, keyed by layer index.
std::map<size_t, std::vector<double>> bridge_angles(TriangleMesh mesh, const DynamicPrintConfig &config, SurfaceType type,
                                                    double rotation_deg = 0.)
{
    Print print;
    Model model;
    init_print({ std::move(mesh) }, print, model, config, false);
    if (rotation_deg != 0.) {
        model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., Geometry::deg2rad(rotation_deg)));
        print.apply(model, config);
    }
    print.process();
    std::map<size_t, std::vector<double>> out;
    const PrintObject &object = *print.objects().front();
    for (size_t i = 0; i < object.layer_count(); ++i)
        for (const LayerRegion *region : object.get_layer(i)->regions())
            for (const Surface *surface : region->fill_surfaces.filter_by_type(type))
                out[i].push_back(surface->bridge_angle);
    return out;
}

size_t count(const std::map<size_t, std::vector<double>> &angles)
{
    size_t n = 0;
    for (const auto &[layer, v] : angles)
        n += v.size();
    return n;
}

// Every bridge of `actual` lies on a layer that has bridges in `base`, and its angle is one of that
// layer's automatic angles plus `offset_deg`. (An internal bridge's area is traced along its
// direction, so a turned bridge may split into a different number of surfaces.)
void check_offset(const std::map<size_t, std::vector<double>> &base, const std::map<size_t, std::vector<double>> &actual, double offset_deg)
{
    REQUIRE(count(base) > 0);
    REQUIRE(count(actual) > 0);
    for (const auto &[layer, angles] : actual) {
        CAPTURE(layer);
        REQUIRE(base.count(layer) == 1);
        for (double a : angles) {
            bool found = false;
            for (double b : base.at(layer)) {
                // An undetected direction (-1) is left alone.
                const double expected = b < 0. ? Geometry::rad2deg(b) : Geometry::rad2deg(b) + offset_deg;
                if (std::abs(Geometry::rad2deg(a) - expected) < 1e-6)
                    found = true;
            }
            CAPTURE(Geometry::rad2deg(a));
            CHECK(found);
        }
    }
}

// Every angle equals `expected_deg`.
void check_all(const std::map<size_t, std::vector<double>> &actual, double expected_deg)
{
    REQUIRE(count(actual) > 0);
    for (const auto &[layer, angles] : actual)
        for (double a : angles) {
            CAPTURE(layer);
            CHECK_THAT(Geometry::rad2deg(a), Catch::Matchers::WithinAbs(expected_deg, 1e-6));
        }
}

} // namespace

TEST_CASE("Relative bridge angle off with no angle set keeps the automatic bridge directions", "[BridgeControls][RelativeBridgeAngle]")
{
    DynamicPrintConfig config = bridge_config();
    const auto ext_base = bridge_angles(bridge_slab(), config, stBottomBridge);
    const auto int_base = bridge_angles(internal_bridge_mesh(), config, stInternalBridge);
    // With both angles at 0 the flag has nothing to add.
    config.set_deserialize_strict({ { "relative_bridge_angle", "1" } });
    check_offset(ext_base, bridge_angles(bridge_slab(), config, stBottomBridge), 0.);
    check_offset(int_base, bridge_angles(internal_bridge_mesh(), config, stInternalBridge), 0.);
}

TEST_CASE("Relative bridge angle adds the external angle to the automatic direction", "[BridgeControls][RelativeBridgeAngle]")
{
    DynamicPrintConfig config = bridge_config();
    const auto base = bridge_angles(bridge_slab(), config, stBottomBridge);
    config.set_deserialize_strict({ { "bridge_angle", "10" } });
    SECTION("absolute (default): the angle replaces the automatic one") {
        check_all(bridge_angles(bridge_slab(), config, stBottomBridge), 10.);
    }
    SECTION("relative: the angle is added to the automatic one") {
        config.set_deserialize_strict({ { "relative_bridge_angle", "1" } });
        check_offset(base, bridge_angles(bridge_slab(), config, stBottomBridge), 10.);
    }
}

TEST_CASE("Relative bridge angle adds the internal angle to the automatic direction", "[BridgeControls][RelativeBridgeAngle]")
{
    DynamicPrintConfig config = bridge_config();
    const auto base = bridge_angles(internal_bridge_mesh(), config, stInternalBridge);
    config.set_deserialize_strict({ { "internal_bridge_angle", "10" } });
    SECTION("absolute (default): the angle replaces the automatic one") {
        check_all(bridge_angles(internal_bridge_mesh(), config, stInternalBridge), 10.);
    }
    SECTION("relative: the angle is added to the automatic one") {
        config.set_deserialize_strict({ { "relative_bridge_angle", "1" } });
        check_offset(base, bridge_angles(internal_bridge_mesh(), config, stInternalBridge), 10.);
    }
}

TEST_CASE("An absolute external bridge angle follows the model rotation when infill is aligned to the model", "[BridgeControls][AlignToModel]")
{
    DynamicPrintConfig config = bridge_config();
    config.set_deserialize_strict({ { "bridge_angle", "10" } });
    SECTION("not aligned: the angle stays in bed coordinates") {
        check_all(bridge_angles(bridge_slab(), config, stBottomBridge, 30.), 10.);
    }
    SECTION("aligned: the model rotation is added") {
        config.set_deserialize_strict({ { "align_infill_direction_to_model", "1" } });
        check_all(bridge_angles(bridge_slab(), config, stBottomBridge, 30.), 40.);
    }
}

TEST_CASE("Ironing direction follows the model rotation when infill is aligned to the model", "[BridgeControls][AlignToModel][Ironing]")
{
    // The direction (in degrees, modulo 180) of the longest ironing line of the print.
    auto ironing_direction = [](bool align) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "ironing_type", "top" },
            { "ironing_angle", "0" },
            { "layer_height", "0.2" },
            { "initial_layer_print_height", "0.2" },
            { "align_infill_direction_to_model", align ? "1" : "0" },
        });
        Print print;
        Model model;
        init_print({ TestMesh::cube_20x20x20 }, print, model, config, false);
        model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., Geometry::deg2rad(30.)));
        print.apply(model, config);
        print.process();
        double best_len = 0.;
        double best_dir = -1.;
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->fills.flatten().entities) {
                    if (entity->role() != erIroning)
                        continue;
                    const auto *path = dynamic_cast<const ExtrusionPath *>(entity);
                    REQUIRE(path != nullptr);
                    for (const Line &line : path->polyline.lines())
                        if (line.length() > best_len) {
                            best_len = line.length();
                            double dir = Geometry::rad2deg(std::atan2(double(line.b.y() - line.a.y()), double(line.b.x() - line.a.x())));
                            dir = std::fmod(dir, 180.);
                            if (dir < 0.)
                                dir += 180.;
                            best_dir = dir;
                        }
                }
        REQUIRE(best_len > scaled<double>(5.));
        return best_dir;
    };
    const double off = ironing_direction(false);
    const double on  = ironing_direction(true);
    double diff = std::fmod(on - off, 180.);
    if (diff < 0.)
        diff += 180.;
    CAPTURE(off, on);
    CHECK_THAT(diff, Catch::Matchers::WithinAbs(30., 0.5));
}

// ---------------------------------------------------------------------------------------------
// OrcaSlicer #11255: bridge line width, wider bridge density range.

namespace {

struct BridgeFlows
{
    Flow thick;
    Flow thin;
    Flow plain; // the region's ordinary flow for the same role
};

BridgeFlows bridge_flows(std::initializer_list<ConfigBase::SetDeserializeItem> items, FlowRole role = frSolidInfill)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" }, { "nozzle_diameter", "0.4" } });
    config.set_deserialize_strict(items);
    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config, false);
    print.process();
    const LayerRegion &region = *print.objects().front()->get_layer(3)->regions().front();
    return { region.bridging_flow(role, true), region.bridging_flow(role, false), region.flow(role) };
}

} // namespace

TEST_CASE("Bridge line width 0 keeps the bridge flows of before the port", "[BridgeControls][BridgeLineWidth]")
{
    const FlowRole role = GENERATE(frPerimeter, frSolidInfill);
    const double ratio = GENERATE(1.0, 0.81);
    const int role_id = int(role);
    CAPTURE(role_id, ratio);
    const BridgeFlows flows = bridge_flows({ { "bridge_line_width", "0" }, { "bridge_flow", ratio } }, role);
    // The exact expressions bridging_flow() used before #11255.
    const Flow old_thick = Flow::bridging_flow(float(std::sqrt(ratio)) * 0.4f, 0.4f);
    const Flow old_thin  = flows.plain.with_flow_ratio(ratio);
    CHECK(flows.thick.width() == old_thick.width());
    CHECK(flows.thick.height() == old_thick.height());
    CHECK(flows.thick.spacing() == old_thick.spacing());
    CHECK(flows.thick.mm3_per_mm() == old_thick.mm3_per_mm());
    CHECK(flows.thin.width() == old_thin.width());
    CHECK(flows.thin.height() == old_thin.height());
    CHECK(flows.thin.spacing() == old_thin.spacing());
    CHECK(flows.thin.mm3_per_mm() == old_thin.mm3_per_mm());
}

TEST_CASE("Bridge line width sets the bridge thread and the bridge line width", "[BridgeControls][BridgeLineWidth]")
{
    const char *width = GENERATE("0.3", "75%");
    CAPTURE(width);
    SECTION("bridge flow ratio 1") {
        const BridgeFlows flows = bridge_flows({ { "bridge_line_width", width } });
        CHECK_THAT(flows.thick.width(), Catch::Matchers::WithinAbs(0.3, 1e-6));
        CHECK_THAT(flows.thick.height(), Catch::Matchers::WithinAbs(0.3, 1e-6));
        CHECK(flows.thick.bridge());
        CHECK_THAT(flows.thin.width(), Catch::Matchers::WithinAbs(0.3, 1e-6));
        CHECK_THAT(flows.thin.height(), Catch::Matchers::WithinAbs(0.2, 1e-6));
        CHECK(flows.plain.width() > 0.4f); // the solid infill keeps its own width
    }
    SECTION("bridge flow ratio scales the thick thread by its square root") {
        const BridgeFlows flows = bridge_flows({ { "bridge_line_width", width }, { "bridge_flow", "0.81" } });
        CHECK_THAT(flows.thick.width(), Catch::Matchers::WithinAbs(0.27, 1e-6));
        const Flow thin_100 = bridge_flows({ { "bridge_line_width", width } }).thin;
        CHECK_THAT(flows.thin.mm3_per_mm(), Catch::Matchers::WithinRel(0.81 * thin_100.mm3_per_mm(), 1e-6));
    }
}

TEST_CASE("Bridge line width also sizes the overhang walls", "[BridgeControls][BridgeLineWidth]")
{
    const BridgeFlows flows = bridge_flows({ { "bridge_line_width", "0.3" } }, frPerimeter);
    CHECK_THAT(flows.thin.width(), Catch::Matchers::WithinAbs(0.3, 1e-6));
    CHECK_THAT(flows.thick.width(), Catch::Matchers::WithinAbs(0.3, 1e-6));
}

TEST_CASE("Flow::extrusion_width resolves the bridge line width", "[BridgeControls][BridgeLineWidth]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "nozzle_diameter", "0.4" }, { "internal_solid_infill_line_width", "0.45" } });
    config.set_deserialize_strict({ { "bridge_line_width", "0" } });
    CHECK_THAT(Flow::extrusion_width("bridge_line_width", config, 0), Catch::Matchers::WithinAbs(0.45, 1e-6));
    config.set_deserialize_strict({ { "bridge_line_width", "75%" } });
    CHECK_THAT(Flow::extrusion_width("bridge_line_width", config, 0), Catch::Matchers::WithinAbs(0.3, 1e-6));
    config.set_deserialize_strict({ { "bridge_line_width", "0.35" } });
    CHECK_THAT(Flow::extrusion_width("bridge_line_width", config, 0), Catch::Matchers::WithinAbs(0.35, 1e-6));
}

namespace {

StringObjectException validate_bridge(std::initializer_list<ConfigBase::SetDeserializeItem> items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "nozzle_diameter", "0.4" }, { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" },
                                    // full_print_config() has relative E, which validate() refuses without a per-layer reset.
                                    { "layer_change_gcode", "G92 E0" } });
    config.set_deserialize_strict(items);
    Model model = Test::model("cube", Test::mesh(TestMesh::cube_20x20x20));
    for (ModelObject *object : model.objects) {
        object->instances.front()->set_offset(Vec3d(100., 100., 0.));
        object->ensure_on_bed();
    }
    Print print;
    for (ModelObject *object : model.objects)
        print.auto_assign_extruders(object);
    print.apply(model, config);
    return print.validate();
}

} // namespace

TEST_CASE("Print::validate checks the bridge line width against the nozzle and the layer height", "[BridgeControls][BridgeLineWidth][Validate]")
{
    SECTION("0 and widths up to the nozzle pass") {
        for (const char *w : { "0", "0.3", "100%" }) {
            const StringObjectException err = validate_bridge({ { "bridge_line_width", w } });
            INFO(w << ": " << err.string << " (" << err.opt_key << ")");
            CHECK(err.string.empty());
        }
    }
    SECTION("wider than the nozzle is refused") {
        const StringObjectException err = validate_bridge({ { "bridge_line_width", "0.5" } });
        CHECK(err.string == "Bridge line width must not exceed nozzle diameter");
        CHECK(err.opt_key == "bridge_line_width");
    }
    SECTION("not above the layer height is refused unless all bridges are thick") {
        const StringObjectException thin = validate_bridge({ { "bridge_line_width", "0.15" }, { "thick_bridges", "0" }, { "thick_internal_bridges", "1" } });
        CHECK(thin.string == "Too small line width");
        CHECK(thin.opt_key == "bridge_line_width");
        const StringObjectException thick = validate_bridge({ { "bridge_line_width", "0.15" }, { "thick_bridges", "1" }, { "thick_internal_bridges", "1" } });
        INFO(thick.string << " (" << thick.opt_key << ")");
        CHECK(thick.string.empty());
    }
}

TEST_CASE("Config validation: bridge line width and internal bridge flow", "[BridgeControls][BridgeLineWidth][Validate]")
{
    FullPrintConfig config;
    REQUIRE(config.bridge_line_width.value == 0.);
    CHECK(Slic3r::validate(config).count("bridge_line_width") == 0);
    config.bridge_line_width.value   = 0.5;
    config.bridge_line_width.percent = false;
    CHECK(Slic3r::validate(config).count("bridge_line_width") == 1);
    config.bridge_line_width.value   = 100.;
    config.bridge_line_width.percent = true;
    CHECK(Slic3r::validate(config).count("bridge_line_width") == 0);

    // Upstream's fix: the internal bridge flow ratio is checked on its own value.
    config.internal_bridge_flow.value = 0.;
    CHECK(Slic3r::validate(config).count("internal_bridge_flow") == 1);
    config.internal_bridge_flow.value = 1.;
    CHECK(Slic3r::validate(config).count("internal_bridge_flow") == 0);
}

TEST_CASE("Bridge densities accept up to 125%", "[BridgeControls][BridgeDensity]")
{
    for (const char *key : { "bridge_density", "internal_bridge_density" }) {
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        CHECK(def->min == 10.);
        CHECK(def->max == 125.);
        CHECK(def->is_value_valid(125.));
        CHECK_FALSE(def->is_value_valid(126.));
    }

    // An internal bridge denser than 100% lays more bridge line than a solid one.
    auto internal_bridge_length = [](const char *density) {
        DynamicPrintConfig config = bridge_config();
        config.set_deserialize_strict({ { "internal_bridge_density", density } });
        Print print;
        Model model;
        init_print({ internal_bridge_mesh() }, print, model, config, false);
        print.process();
        double length = 0.;
        for (const Layer *layer : print.objects().front()->layers())
            for (const LayerRegion *region : layer->regions())
                for (const ExtrusionEntity *entity : region->fills.flatten().entities)
                    if (entity->role() == erInternalBridgeInfill)
                        length += unscale<double>(entity->length());
        return length;
    };
    const double solid = internal_bridge_length("100%");
    const double dense = internal_bridge_length("125%");
    REQUIRE(solid > 0.);
    CHECK(dense > 1.1 * solid);
}
