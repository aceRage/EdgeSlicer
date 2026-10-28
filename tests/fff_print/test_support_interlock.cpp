// EdgeSlicer: support interface interlock ("woven band"), phase 1, on sliced supports.
// tests/design_support_interface_interlock.md, section 8 (slicing tests): the top contact layer is
// untouched, the woven layers carry both filaments with their stripes turned 90 degrees between
// layers, and the feature is off by default and a no-op whenever it does not apply.

#include <catch2/catch.hpp>

#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// A 30 x 30 mm, 2 mm thick cap on a 10 x 10 mm stem: one flat roof at 10 mm, supported from the bed.
TriangleMesh capped_stem()
{
    TriangleMesh model = make_cube(10, 10, 11);
    model.translate(10., 10., 0.);
    TriangleMesh cap = make_cube(30, 30, 2);
    cap.translate(0., 0., 10.);
    model.merge(cap);
    return model;
}

// Body in filament 1 (the model's), interface in filament 2: the owner's PLA + "Support for PLA/PETG"
// recipe in miniature.
DynamicPrintConfig interlock_config(const std::string &style, bool interlock, int top_layers = 3)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",                  1 },
        { "support_type",                    style == "normal" ? "normal(auto)" : "tree(auto)" },
        { "support_style",                   style == "normal" ? "default" : style },
        { "support_on_build_plate_only",     1 },
        { "layer_height",                    0.2 },
        { "initial_layer_print_height",      0.2 },
        { "support_filament",                1 },
        { "support_interface_filament",      2 },
        { "support_interface_top_layers",    top_layers },
        { "support_interface_bottom_layers", 0 },
        { "nozzle_diameter",                 "0.4,0.4" },
        { "filament_diameter",               "1.75,1.75" },
        { "filament_type",                   "PLA;PETG" },
        { "filament_soluble",                "0,0" },
        { "enable_prime_tower",              0 },
        { "support_interface_interlock",     interlock ? 1 : 0 },
    });
    return config;
}

void dump(std::ostringstream &out, const ExtrusionEntity &ee)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(&ee)) {
        out << "{";
        for (const ExtrusionEntity *e : coll->entities)
            dump(out, *e);
        out << "}";
    } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(&ee)) {
        out << "P" << int(path->role()) << "/" << path->width << "/" << path->height << "/" << path->mm3_per_mm << ":";
        for (const Point &pt : path->polyline.points)
            out << pt.x() << "," << pt.y() << ";";
        out << "\n";
    } else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath*>(&ee)) {
        out << "M[";
        for (const ExtrusionPath &p : mp->paths)
            dump(out, p);
        out << "]";
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&ee)) {
        out << "L[";
        for (const ExtrusionPath &p : loop->paths)
            dump(out, p);
        out << "]";
    } else
        out << "?";
}

std::string dump(const SupportLayer &layer)
{
    std::ostringstream out;
    out.precision(10);
    out << "z" << layer.print_z << " h" << layer.height << "\n";
    dump(out, layer.support_fills);
    for (const auto &kv : layer.interface_by_extruder) {
        out << "E" << kv.first;
        dump(out, kv.second);
    }
    return out.str();
}

void collect_paths(const ExtrusionEntity &ee, std::vector<const ExtrusionPath*> &out)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection*>(&ee)) {
        for (const ExtrusionEntity *e : coll->entities)
            collect_paths(*e, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath*>(&ee)) {
        out.push_back(path);
    } else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath*>(&ee)) {
        for (const ExtrusionPath &p : mp->paths)
            out.push_back(&p);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&ee)) {
        for (const ExtrusionPath &p : loop->paths)
            out.push_back(&p);
    }
}

struct RoleStats {
    double body_length      = 0.; // erSupportMaterial, mm
    double interface_length = 0.; // erSupportMaterialInterface, mm
    // Length-weighted axial mean direction of the interface lines, in [0, pi).
    double interface_angle  = 0.;
};

RoleStats role_stats(const SupportLayer &layer)
{
    std::vector<const ExtrusionPath*> paths;
    collect_paths(layer.support_fills, paths);
    RoleStats stats;
    double c = 0., s = 0.;
    for (const ExtrusionPath *path : paths) {
        const double len = unscale<double>(path->polyline.length());
        if (path->role() == erSupportMaterial)
            stats.body_length += len;
        else if (path->role() == erSupportMaterialInterface) {
            stats.interface_length += len;
            const Points &pts = path->polyline.points;
            for (size_t i = 1; i < pts.size(); ++ i) {
                const Vec2d d = (pts[i] - pts[i - 1]).cast<double>();
                const double l = d.norm();
                if (l < scale_(1.))
                    continue; // links
                const double a2 = 2. * std::atan2(d.y(), d.x());
                c += l * std::cos(a2);
                s += l * std::sin(a2);
            }
        }
    }
    double a = 0.5 * std::atan2(s, c);
    if (a < 0)
        a += M_PI;
    stats.interface_angle = a;
    return stats;
}

double axial_diff_deg(double a, double b)
{
    const double d = std::fmod(std::fabs(a - b), M_PI);
    return std::min(d, M_PI - d) * 180. / M_PI;
}

// The support layers under the roof, top first: [0] is the top contact layer, [1] the layer right
// under it and so on.
std::vector<const SupportLayer*> roof_stack(const Print &print)
{
    const PrintObject &object = *print.objects().front();
    std::vector<const SupportLayer*> out;
    const auto layers = object.support_layers();
    for (size_t i = layers.size(); i > 0; -- i)
        if (! layers[i - 1]->support_fills.entities.empty())
            out.push_back(layers[i - 1]);
    return out;
}

} // namespace

TEST_CASE("Support interface interlock is off by default", "[SupportInterlock]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    CHECK_FALSE(config.opt_bool("support_interface_interlock"));
    CHECK(config.opt_int("support_interface_interlock_layers") == 2);
    CHECK(config.opt_int("support_interface_interlock_beam_lines") == 2);
}

TEST_CASE("Support interface interlock weaves the layers under the top contact", "[SupportInterlock]")
{
    // "organic" runs TreeSupport3D + SupportCommon; the classic Bambu tree styles run TreeSupport.cpp
    // (the owner's hand test of #758 used tree_hybrid, which phase 1 first missed).
    const std::string style = GENERATE(std::string("normal"), std::string("organic"),
                                       std::string("tree_hybrid"), std::string("tree_slim"), std::string("tree_strong"));
    CAPTURE(style);

    Print print_off, print_on;
    init_and_process_print({ capped_stem() }, print_off, interlock_config(style, false));
    init_and_process_print({ capped_stem() }, print_on,  interlock_config(style, true));
    const std::vector<const SupportLayer*> off = roof_stack(print_off);
    const std::vector<const SupportLayer*> on  = roof_stack(print_on);
    REQUIRE(off.size() >= 4);
    REQUIRE(on.size() == off.size());

    SECTION("the top contact layer is untouched") {
        CHECK(dump(*on[0]) == dump(*off[0]));
    }

    SECTION("the two layers under it carry both filaments") {
        for (size_t depth = 1; depth <= 2; ++ depth) {
            CAPTURE(depth);
            const RoleStats s_on = role_stats(*on[depth]);
            CHECK(s_on.body_length > 10.);
            CHECK(s_on.interface_length > 10.);
            // Roughly half and half under the roof.
            CHECK(s_on.interface_length > 0.25 * (s_on.body_length + s_on.interface_length));
        }
        // With the feature off, the layer right under the contact is (all but) interface filament only.
        const RoleStats s_off = role_stats(*off[1]);
        CHECK(s_off.body_length < 0.05 * s_off.interface_length);
    }

    SECTION("the woven stripes of the two layers cross at 90 degrees") {
        const RoleStats s1 = role_stats(*on[1]);
        const RoleStats s2 = role_stats(*on[2]);
        CHECK(axial_diff_deg(s1.interface_angle, s2.interface_angle) > 80.);
    }

    SECTION("the woven layers are flagged so no purge lands in them") {
        CHECK(on[1]->interlock_woven);
        CHECK(on[2]->interlock_woven);
        CHECK_FALSE(off[1]->interlock_woven);
    }

    SECTION("the support below the woven band is unchanged") {
        // Layers deeper than the band (the base under the roof) are the same with and without.
        for (size_t i = 4; i < off.size(); ++ i) {
            CAPTURE(i);
            CHECK(dump(*on[i]) == dump(*off[i]));
        }
    }
}

TEST_CASE("Support interface interlock is a no-op where it does not apply", "[SupportInterlock]")
{
    auto all_layers = [](const DynamicPrintConfig &config) {
        Print print;
        init_and_process_print({ capped_stem() }, print, config);
        std::string out;
        for (const SupportLayer *layer : print.objects().front()->support_layers())
            out += dump(*layer);
        return out;
    };

    SECTION("same filament for body and interface") {
        DynamicPrintConfig off = interlock_config("normal", false);
        DynamicPrintConfig on  = interlock_config("normal", true);
        off.set_key_value("support_interface_filament", new ConfigOptionInt(1));
        on .set_key_value("support_interface_filament", new ConfigOptionInt(1));
        CHECK(all_layers(on) == all_layers(off));
    }
    SECTION("too few top interface layers for the woven layers") {
        // Two woven layers need three top interface layers; with two there is no room.
        CHECK(all_layers(interlock_config("normal", true, 2)) == all_layers(interlock_config("normal", false, 2)));
    }
    SECTION("support filament matching on (design D6)") {
        DynamicPrintConfig off = interlock_config("normal", false);
        DynamicPrintConfig on  = interlock_config("normal", true);
        off.set_key_value("support_filament_matching", new ConfigOptionBool(true));
        on .set_key_value("support_filament_matching", new ConfigOptionBool(true));
        CHECK(all_layers(on) == all_layers(off));
    }
}

TEST_CASE("Support interface interlock with four top interface layers leaves the bonding layer alone", "[SupportInterlock]")
{
    // N = 4, W = 2: the two layers under the contact are woven, the bonding layer under them is not.
    Print print_off, print_on;
    init_and_process_print({ capped_stem() }, print_off, interlock_config("normal", false, 4));
    init_and_process_print({ capped_stem() }, print_on,  interlock_config("normal", true, 4));
    const std::vector<const SupportLayer*> off = roof_stack(print_off);
    const std::vector<const SupportLayer*> on  = roof_stack(print_on);
    REQUIRE(off.size() >= 5);
    REQUIRE(on.size() == off.size());
    CHECK(dump(*on[0]) == dump(*off[0]));
    CHECK(role_stats(*on[1]).body_length > 10.);
    CHECK(role_stats(*on[2]).body_length > 10.);
    CHECK(dump(*on[3]) == dump(*off[3]));
}

namespace {

// Cap A as in capped_stem(), next to a ledge that carries a stem up to a higher cap B. The support
// under cap B stands on the ledge, so it has a bottom contact and bottom interface layers around the
// heights where cap A's band is woven: an interface layer there holds only bottom interface.
TriangleMesh cap_beside_ledge(double ledge_top)
{
    TriangleMesh model = make_cube(6, 6, 10);
    model.translate(7., 7., 0.);
    TriangleMesh cap_a = make_cube(20, 20, 2);
    cap_a.translate(0., 0., 10.);
    model.merge(cap_a);
    TriangleMesh ledge = make_cube(20, 20, ledge_top);
    ledge.translate(30., 0., 0.);
    model.merge(ledge);
    TriangleMesh stem_b = make_cube(6, 6, 14. - ledge_top);
    stem_b.translate(37., 7., ledge_top);
    model.merge(stem_b);
    TriangleMesh cap_b = make_cube(20, 20, 2);
    cap_b.translate(30., 0., 14.);
    model.merge(cap_b);
    return model;
}

} // namespace

TEST_CASE("Support interface interlock keeps its interface stripes where a bottom interface shares the height", "[SupportInterlock]")
{
    const double ledge_top = GENERATE(8.2, 8.4, 8.6, 8.8, 9.0);
    CAPTURE(ledge_top);
    auto config = [](bool on) {
        DynamicPrintConfig c = interlock_config("normal", on);
        c.set_key_value("support_on_build_plate_only", new ConfigOptionBool(false));
        c.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(5));
        return c;
    };
    Print print_off, print_on;
    init_and_process_print({ cap_beside_ledge(ledge_top) }, print_off, config(false));
    init_and_process_print({ cap_beside_ledge(ledge_top) }, print_on,  config(true));
    const auto off = print_off.objects().front()->support_layers();
    const auto on  = print_on .objects().front()->support_layers();
    REQUIRE(off.size() == on.size());
    // Cap A's top contact: the highest support layer under its underside (z = 10).
    size_t contact = size_t(-1);
    for (size_t i = 0; i < off.size(); ++ i)
        if (off[i]->print_z < 10. - 0.05)
            contact = i;
    REQUIRE(contact != size_t(-1));
    REQUIRE(contact >= 2);
    for (size_t depth = 1; depth <= 2; ++ depth) {
        CAPTURE(depth);
        const RoleStats s_off = role_stats(*off[contact - depth]);
        const RoleStats s_on  = role_stats(*on [contact - depth]);
        CAPTURE(s_off.interface_length, s_on.interface_length, s_on.body_length);
        // The woven layers print both filaments under cap A ...
        CHECK(s_on.body_length > 10.);
        // ... and the bonding layer (depth 2, all body with the feature off) gains interface stripes:
        // they must not end up in a second interface layer at the same height, which is never printed.
        if (depth == 2)
            CHECK(s_on.interface_length > s_off.interface_length + 20.);
    }
}

TEST_CASE("Support interface interlock with the owner's tree hybrid settings", "[SupportInterlock]")
{
    // The #758 hand-test project: tree(auto) + tree_hybrid, body "default" filament, interface 2,
    // 4 top interface layers, 3 woven layers, 1-line stripes, zero top Z distance.
    auto config = [](bool on) {
        DynamicPrintConfig c = interlock_config("tree_hybrid", on, 4);
        c.set_key_value("support_filament", new ConfigOptionInt(0));
        c.set_key_value("support_top_z_distance", new ConfigOptionFloat(0.));
        c.set_key_value("support_interface_interlock_layers", new ConfigOptionInt(3));
        c.set_key_value("support_interface_interlock_beam_lines", new ConfigOptionInt(1));
        return c;
    };
    Print print_off, print_on;
    init_and_process_print({ capped_stem() }, print_off, config(false));
    init_and_process_print({ capped_stem() }, print_on,  config(true));
    const std::vector<const SupportLayer*> off = roof_stack(print_off);
    const std::vector<const SupportLayer*> on  = roof_stack(print_on);
    REQUIRE(off.size() >= 5);
    REQUIRE(on.size() == off.size());
    CHECK(dump(*on[0]) == dump(*off[0]));
    for (size_t depth = 1; depth <= 3; ++ depth) {
        CAPTURE(depth);
        const RoleStats s = role_stats(*on[depth]);
        CHECK(s.body_length > 10.);
        CHECK(s.interface_length > 10.);
        CHECK(on[depth]->interlock_woven);
    }
}
