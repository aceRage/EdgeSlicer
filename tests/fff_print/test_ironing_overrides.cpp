#include <catch2/catch.hpp>

#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// OrcaSlicer OD4: filament ironing overrides (#11194, #15082), ironing angle as an offset from the
// top surface direction (#10841) and the fixed ironing angle (#11195).

namespace {

using Items = std::initializer_list<ConfigBase::SetDeserializeItem>;

DynamicPrintConfig ironing_config(Items items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "ironing_type", "top" },
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "nozzle_diameter", "0.4" },
        { "ironing_flow", "10%" },
        { "ironing_spacing", "0.1" },
        { "ironing_inset", "0" },
        { "ironing_speed", "20" },
    });
    config.set_deserialize_strict(items);
    return config;
}

std::vector<const ExtrusionPath *> ironing_paths(const Print &print, std::vector<ExtrusionEntityCollection> &keep_alive)
{
    std::vector<const ExtrusionPath *> out;
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions()) {
            keep_alive.emplace_back(region->fills.flatten());
            for (const ExtrusionEntity *entity : keep_alive.back().entities)
                if (entity->role() == erIroning)
                    if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                        out.push_back(path);
        }
    return out;
}

struct IroningStats
{
    size_t      paths    = 0;
    double      length   = 0.;  // total ironing length, mm
    double      mm3      = 0.;  // mm3_per_mm of the ironing paths (all equal on one surface)
    BoundingBox bbox;
};

IroningStats ironing_stats(Items items)
{
    DynamicPrintConfig config = ironing_config(items);
    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config, false);
    print.process();
    std::vector<ExtrusionEntityCollection> keep_alive;
    keep_alive.reserve(4096);
    IroningStats stats;
    for (const ExtrusionPath *path : ironing_paths(print, keep_alive)) {
        ++stats.paths;
        stats.length += unscaled(path->polyline.length());
        stats.mm3 = std::max(stats.mm3, path->mm3_per_mm);
        for (const Point &pt : path->polyline.points)
            stats.bbox.merge(pt);
    }
    return stats;
}

// Direction (degrees, modulo 180, rounded) of the longest ironing line of every ironed layer.
std::map<size_t, double> ironing_directions(Items items, double model_rotation_deg = 0.)
{
    DynamicPrintConfig config = ironing_config(items);
    Print print;
    Model model;
    init_print({ TestMesh::cube_20x20x20 }, print, model, config, false);
    if (model_rotation_deg != 0.) {
        model.objects.front()->instances.front()->set_rotation(Vec3d(0., 0., Geometry::deg2rad(model_rotation_deg)));
        print.apply(model, config);
    }
    print.process();
    std::map<size_t, double> out;
    for (const Layer *layer : print.objects().front()->layers()) {
        double best_len = 0.;
        double best_dir = -1.;
        for (const LayerRegion *region : layer->regions()) {
            ExtrusionEntityCollection flat = region->fills.flatten();
            for (const ExtrusionEntity *entity : flat.entities) {
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
                        best_dir = std::round(dir);
                        if (best_dir >= 180.)
                            best_dir -= 180.;
                    }
            }
        }
        if (best_len > scaled<double>(5.))
            out[layer->id()] = best_dir;
    }
    return out;
}

double angle_diff(double a, double b)
{
    double d = std::fmod(a - b, 180.);
    if (d < 0.)
        d += 180.;
    return d;
}

// Feedrates (mm/s) of the extrusions inside ";TYPE:Ironing" / "; FEATURE: Ironing" blocks.
std::set<double> ironing_feedrates(const std::string &gcode_text)
{
    std::set<double>   out;
    std::istringstream in(gcode_text);
    std::string        line;
    std::string        feature;
    double             feedrate = 0.;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";TYPE:", 0) == 0) {
            feature = line.substr(6);
            continue;
        }
        if (line.rfind("; FEATURE: ", 0) == 0) {
            feature = line.substr(11);
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G1\t", 0) != 0)
            continue;
        const std::string code = line.substr(0, line.find(';'));
        std::istringstream words(code);
        std::string        word;
        bool               extrudes = false;
        while (words >> word) {
            if (word.size() > 1 && word[0] == 'F')
                feedrate = std::stod(word.substr(1)) / 60.;
            else if (word.size() > 1 && word[0] == 'E' && std::stod(word.substr(1)) > 0.)
                extrudes = true;
        }
        if (extrudes && feature == "Ironing")
            out.insert(std::round(feedrate * 10.) / 10.);
    }
    return out;
}

std::set<double> ironing_feedrates_for(Items items)
{
    DynamicPrintConfig config = ironing_config(items);
    config.set_deserialize_strict({ { "slow_down_for_layer_cooling", "0" } });
    return ironing_feedrates(slice({ TestMesh::cube_20x20x20 }, config, true));
}

} // namespace

TEST_CASE("ironing_filament_override picks the filament value or falls back to the process value", "[Ironing][OD4]")
{
    ConfigOptionFloatsNullable opt { ConfigOptionFloatsNullable::nil_value(), 0.25 };
    SECTION("a nil slot uses the process value") {
        CHECK(ironing_filament_override(opt, 1, 0.1) == Approx(0.1));
    }
    SECTION("a set slot overrides it") {
        CHECK(ironing_filament_override(opt, 2, 0.1) == Approx(0.25));
    }
    SECTION("Edge: an id past the vector (a mixed / virtual filament) does not borrow slot 1") {
        ConfigOptionFloatsNullable first_set { 0.3 };
        CHECK(ironing_filament_override(first_set, 1, 0.1) == Approx(0.3));
        CHECK(ironing_filament_override(first_set, 5, 0.1) == Approx(0.1));
        CHECK(ironing_filament_override(first_set, 0, 0.1) == Approx(0.1));
        CHECK(ironing_filament_override(first_set, -1, 0.1) == Approx(0.1));
    }
    SECTION("percents work the same way") {
        ConfigOptionPercentsNullable flow { 25. };
        CHECK(ironing_filament_override(flow, 1, 10.) == Approx(25.));
        ConfigOptionPercentsNullable nil_flow { ConfigOptionPercentsNullable::nil_value() };
        CHECK(ironing_filament_override(nil_flow, 1, 10.) == Approx(10.));
    }
}

TEST_CASE("The filament ironing overrides default to nil", "[Ironing][OD4]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    for (const char *key : { "filament_ironing_flow", "filament_ironing_spacing", "filament_ironing_inset", "filament_ironing_speed" }) {
        CAPTURE(key);
        const auto *opt = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
        REQUIRE(opt != nullptr);
        CHECK(opt->is_nil(0));
    }
    CHECK_FALSE(config.opt_bool("ironing_angle_fixed"));
    CHECK(config.opt_float("ironing_angle") == Approx(0.));
}

TEST_CASE("Filament ironing flow and spacing overrides change the ironing extrusion", "[Ironing][OD4]")
{
    const IroningStats base = ironing_stats({});
    REQUIRE(base.paths > 0);
    REQUIRE(base.mm3 > 0.);

    SECTION("an unset override is the process value") {
        const IroningStats same = ironing_stats({ { "filament_ironing_flow", "nil" } });
        CHECK(same.paths == base.paths);
        CHECK(same.mm3 == Approx(base.mm3));
    }
    SECTION("flow 20% over a process 10% doubles the ironing flow") {
        const IroningStats more = ironing_stats({ { "filament_ironing_flow", "20" } });
        CHECK(more.mm3 == Approx(2. * base.mm3).epsilon(0.01));
    }
    SECTION("spacing 0.2 over a process 0.1 doubles the per-line flow and halves the ironed length") {
        const IroningStats wide = ironing_stats({ { "filament_ironing_spacing", "0.2" } });
        CHECK(wide.mm3 == Approx(2. * base.mm3).epsilon(0.01));
        // Half as many lines over the same surface (monotonic ironing may be one single path).
        CHECK(wide.length < 0.7 * base.length);
    }
    SECTION("the #249 minimum spacing clamp applies to the override too") {
        const IroningStats tight = ironing_stats({ { "filament_ironing_spacing", "0" } });
        CHECK(tight.mm3 == Approx(base.mm3 * IRONING_SPACING_MIN / 0.1).epsilon(0.01));
    }
    SECTION("an inset override keeps the ironing away from the edges") {
        const IroningStats inset = ironing_stats({ { "filament_ironing_inset", "3" } });
        REQUIRE(inset.paths > 0);
        // Process inset 0 = half the nozzle (0.2 mm) per side, the override 3 mm per side.
        CHECK(unscaled(base.bbox.size().x()) - unscaled(inset.bbox.size().x()) > 5.);
        CHECK(unscaled(base.bbox.size().y()) - unscaled(inset.bbox.size().y()) > 5.);
    }
}

TEST_CASE("A filament ironing speed override sets the ironing feedrate", "[Ironing][OD4][GCode]")
{
    const std::set<double> base = ironing_feedrates_for({});
    CAPTURE(base);
    REQUIRE(!base.empty());
    CHECK(base.count(20.) == 1);

    const std::set<double> fast = ironing_feedrates_for({ { "filament_ironing_speed", "15" } });
    CAPTURE(fast);
    REQUIRE(!fast.empty());
    CHECK(fast.count(15.) == 1);
    CHECK(fast.count(20.) == 0);
}

TEST_CASE("The ironing angle is an offset from the top surface direction", "[Ironing][OD4][Angle]")
{
    const auto base = ironing_directions({ { "solid_infill_direction", "45" }, { "ironing_angle", "0" } });
    REQUIRE(!base.empty());

    SECTION("an offset rotates the ironing by that much") {
        const auto rotated = ironing_directions({ { "solid_infill_direction", "45" }, { "ironing_angle", "30" } });
        REQUIRE(rotated.size() == base.size());
        for (const auto &[layer, dir] : base) {
            CAPTURE(layer, dir, rotated.at(layer));
            CHECK(angle_diff(rotated.at(layer), dir) == Approx(30.).margin(1.));
        }
    }
    SECTION("the base is the solid infill direction, not the sparse one") {
        const auto solid0  = ironing_directions({ { "solid_infill_direction", "0" }, { "ironing_angle", "0" } });
        const auto sparse0 = ironing_directions({ { "solid_infill_direction", "45" }, { "infill_direction", "0" }, { "ironing_angle", "0" } });
        REQUIRE(solid0.size() == base.size());
        REQUIRE(sparse0.size() == base.size());
        for (const auto &[layer, dir] : base) {
            CAPTURE(layer, dir, solid0.at(layer), sparse0.at(layer));
            CHECK(angle_diff(dir, solid0.at(layer)) == Approx(45.).margin(1.));
            CHECK(sparse0.at(layer) == Approx(dir).margin(1.));
        }
    }
    SECTION("a legacy -1 loads as offset 0") {
        DynamicPrintConfig config;
        config.set_deserialize_strict({ { "ironing_angle", "-1" } });
        CHECK(config.opt_float("ironing_angle") == Approx(0.));
        const auto legacy = ironing_directions({ { "solid_infill_direction", "45" }, { "ironing_angle", "-1" } });
        CHECK(legacy == base);
    }
}

TEST_CASE("A fixed ironing angle is absolute and does not alternate", "[Ironing][OD4][Angle]")
{
    // Iron every solid layer so several layers of both parities carry ironing.
    const Items solid = { { "ironing_type", "solid" }, { "solid_infill_direction", "45" } };
    auto with = [&solid](Items more) {
        std::vector<ConfigBase::SetDeserializeItem> all(solid);
        all.insert(all.end(), more.begin(), more.end());
        return all;
    };
    auto directions = [](const std::vector<ConfigBase::SetDeserializeItem> &items) {
        DynamicPrintConfig config = ironing_config({});
        for (const ConfigBase::SetDeserializeItem &item : items)
            config.set_deserialize_strict(item.opt_key, item.opt_value);
        Print print;
        Model model;
        init_print({ TestMesh::cube_20x20x20 }, print, model, config, false);
        print.process();
        std::set<double> dirs;
        size_t           layers = 0;
        for (const Layer *layer : print.objects().front()->layers()) {
            double best_len = 0., best_dir = -1.;
            for (const LayerRegion *region : layer->regions()) {
                ExtrusionEntityCollection flat = region->fills.flatten();
                for (const ExtrusionEntity *entity : flat.entities)
                    if (entity->role() == erIroning)
                        if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                            for (const Line &line : path->polyline.lines())
                                if (line.length() > best_len) {
                                    best_len = line.length();
                                    double dir = std::fmod(Geometry::rad2deg(std::atan2(double(line.b.y() - line.a.y()),
                                                                                        double(line.b.x() - line.a.x()))), 180.);
                                    if (dir < 0.)
                                        dir += 180.;
                                    best_dir = std::fmod(std::round(dir), 180.);
                                }
            }
            if (best_len > scaled<double>(5.)) {
                dirs.insert(best_dir);
                ++layers;
            }
        }
        REQUIRE(layers >= 3);
        return dirs;
    };

    const std::set<double> alternating = directions(with({ { "ironing_angle", "0" } }));
    CAPTURE(alternating);
    CHECK(alternating.size() == 2);

    const std::set<double> fixed0 = directions(with({ { "ironing_angle", "0" }, { "ironing_angle_fixed", "1" } }));
    CAPTURE(fixed0);
    REQUIRE(fixed0.size() == 1);

    const std::set<double> fixed30 = directions(with({ { "ironing_angle", "30" }, { "ironing_angle_fixed", "1" } }));
    CAPTURE(fixed30);
    REQUIRE(fixed30.size() == 1);
    CHECK(angle_diff(*fixed30.begin(), *fixed0.begin()) == Approx(30.).margin(1.));

    // Absolute: the solid infill direction no longer moves it.
    const std::set<double> fixed_other_solid =
        directions(with({ { "ironing_angle", "0" }, { "ironing_angle_fixed", "1" }, { "solid_infill_direction", "0" } }));
    CHECK(fixed_other_solid == fixed0);
}

TEST_CASE("A fixed ironing angle still follows the model rotation when infill is aligned to the model", "[Ironing][OD4][Angle][AlignToModel]")
{
    const Items fixed = { { "ironing_angle", "0" }, { "ironing_angle_fixed", "1" } };
    const auto off = ironing_directions(fixed, 30.);
    REQUIRE(!off.empty());
    const auto on = ironing_directions({ { "ironing_angle", "0" }, { "ironing_angle_fixed", "1" }, { "align_infill_direction_to_model", "1" } }, 30.);
    REQUIRE(on.size() == off.size());
    for (const auto &[layer, dir] : off) {
        CAPTURE(layer, dir, on.at(layer));
        CHECK(angle_diff(on.at(layer), dir) == Approx(30.).margin(1.));
    }
}
