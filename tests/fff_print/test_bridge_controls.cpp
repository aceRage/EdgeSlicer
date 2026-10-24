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
