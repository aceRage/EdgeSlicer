// Side stabilizers (libslic3r/Support/Stabilizers.hpp) do not depend on Enable supports.
//
// With supports off and stabilizers on the support step runs in a stabilizers-only mode: the
// stabilizers print, and no support, interface, transition or raft is made. With supports on the
// stabilizers are exactly what they were. With no stabilizers configured nothing changes for
// supports on or off (the byte-identity half of that is the CLI gate against origin/main; here it is
// pinned by "no support layers at all").

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/StabilizerBake.hpp"
#include "libslic3r/Support/Stabilizers.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

TriangleMesh pin_mesh()
{
    // 6 mm wide, 60 mm tall, standing on the bed: nothing to support, so any support extrusion
    // next to it can only be a stabilizer.
    return TriangleMesh(its_make_cylinder(3., 60., M_PI / 90.));
}

DynamicPrintConfig pin_config(bool supports, bool stabilizers)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              supports ? "1" : "0" },
        { "support_type",                "normal(auto)" },
        { "support_on_build_plate_only", "1" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "stabilizer_supports",         stabilizers ? "auto" : "off" },
        { "stabilizer_ring_spacing",     "15" },
        { "stabilizer_points_per_ring",  "3" },
        { "stabilizer_tip_gap",          "0" },
    });
    return config;
}

struct Sliced
{
    Print print;
    Model model;
    const PrintObject &object() const { return *print.objects().front(); }
};

void slice(Sliced &s, const DynamicPrintConfig &config)
{
    ModelObject *object = s.model.add_object();
    object->name = "pin";
    object->add_volume(pin_mesh());
    object->add_instance();
    object->ensure_on_bed();
    s.print.auto_assign_extruders(object);
    s.print.apply(s.model, config);
    s.print.set_status_silent();
    s.print.process();
}

// What the support layers of an object carry: extrusion count and length by role.
struct RoleTotals
{
    std::map<ExtrusionRole, size_t> count;
    std::map<ExtrusionRole, double> length_mm;
    size_t                          layers_with_extrusions = 0;
};

RoleTotals role_totals(const PrintObject &po)
{
    RoleTotals out;
    for (const SupportLayer *sl : po.support_layers()) {
        ExtrusionEntityCollection flat = sl->support_fills.flatten();
        if (! flat.entities.empty())
            ++out.layers_with_extrusions;
        for (const ExtrusionEntity *ee : flat.entities) {
            ++out.count[ee->role()];
            out.length_mm[ee->role()] += unscaled(ee->length());
        }
    }
    return out;
}

size_t count_of(const RoleTotals &t, ExtrusionRole role)
{
    const auto it = t.count.find(role);
    return it == t.count.end() ? 0 : it->second;
}

double length_of(const RoleTotals &t, ExtrusionRole role)
{
    const auto it = t.length_mm.find(role);
    return it == t.length_mm.end() ? 0. : it->second;
}

// The role name of a ";TYPE:<role>" (generic printer) or "; FEATURE: <role>" (BBL) G-code marker, or empty.
std::string role_marker(const std::string &line)
{
    static const std::string generic = ";TYPE:";
    static const std::string bbl     = "; FEATURE: ";
    std::string              name;
    if (line.rfind(bbl, 0) == 0)
        name = line.substr(bbl.size());
    else if (line.rfind(generic, 0) == 0)
        name = line.substr(generic.size());
    else
        return std::string();
    while (! name.empty() && (name.back() == '\r' || name.back() == ' '))
        name.pop_back();
    return name;
}

std::map<std::string, size_t> gcode_roles(const std::string &gcode)
{
    std::map<std::string, size_t> roles;
    size_t pos = 0;
    while (pos < gcode.size()) {
        size_t end = gcode.find('\n', pos);
        if (end == std::string::npos)
            end = gcode.size();
        const std::string role = role_marker(gcode.substr(pos, end - pos));
        if (! role.empty())
            ++roles[role];
        pos = end + 1;
    }
    return roles;
}

size_t role_lines(const std::map<std::string, size_t> &roles, const std::string &name)
{
    const auto it = roles.find(name);
    return it == roles.end() ? 0 : it->second;
}

} // namespace

TEST_CASE("Side stabilizers generate with Enable supports off", "[StabilizersNoSupport]")
{
    Sliced s;
    slice(s, pin_config(false, true));
    const PrintObject &po = s.object();

    REQUIRE_FALSE(po.has_support());
    REQUIRE_FALSE(po.has_support_material());
    REQUIRE(po.has_stabilizers());
    REQUIRE(po.uses_support_filament());

    const RoleTotals t = role_totals(po);
    CHECK_FALSE(po.support_layers().empty());
    CHECK(t.layers_with_extrusions > 10);
    // Stabilizer extrusions are there...
    CHECK(count_of(t, erSupportMaterial) > 100);
    CHECK(length_of(t, erSupportMaterial) > 1000.);
    // ...and nothing else a support generator makes.
    CHECK(count_of(t, erSupportMaterialInterface) == 0);
    CHECK(count_of(t, erSupportTransition) == 0);
    CHECK(count_of(t, erIroning) == 0);
    for (const auto &[role, n] : t.count)
        CHECK((role == erSupportMaterial));
    // No raft: the first object layer sits on the bed.
    CHECK_FALSE(po.has_raft());
    CHECK_THAT(po.layers().front()->print_z, WithinAbs(0.2, 1e-6));
    // The support layers are the stabilizers' own, at the object's layer heights.
    for (const SupportLayer *sl : po.support_layers()) {
        bool on_object_layer = false;
        for (const Layer *l : po.layers())
            if (std::abs(l->print_z - sl->print_z) < EPSILON) {
                on_object_layer = true;
                break;
            }
        CHECK(on_object_layer);
        CHECK(sl->interface_by_extruder.empty());
    }
}

TEST_CASE("Side stabilizers with supports off match the same part with supports on", "[StabilizersNoSupport]")
{
    // The pin has nothing to support, so supports on adds nothing: the two runs must print the
    // very same stabilizers - the one place the support step is shared.
    Sliced off, on;
    slice(off, pin_config(false, true));
    slice(on, pin_config(true, true));

    const RoleTotals a = role_totals(off.object());
    const RoleTotals b = role_totals(on.object());
    REQUIRE(off.object().support_layers().size() == on.object().support_layers().size());
    CHECK(a.layers_with_extrusions == b.layers_with_extrusions);
    CHECK(a.count == b.count);
    for (const auto &[role, len] : a.length_mm)
        CHECK_THAT(len, WithinRel(length_of(b, role), 1e-9));
    for (size_t i = 0; i < off.object().support_layers().size(); ++i) {
        const SupportLayer &x = *off.object().support_layers()[i];
        const SupportLayer &y = *on.object().support_layers()[i];
        CHECK_THAT(x.print_z, WithinAbs(y.print_z, 1e-9));
        CHECK(x.support_fills.flatten().entities.size() == y.support_fills.flatten().entities.size());
        CHECK(x.support_islands.size() == y.support_islands.size());
    }
}

TEST_CASE("No stabilizers configured: no support layers, supports on or off", "[StabilizersNoSupport]")
{
    for (bool supports : { false, true }) {
        DYNAMIC_SECTION("supports " << supports)
        {
            Sliced s;
            slice(s, pin_config(supports, false));
            CHECK_FALSE(s.object().has_stabilizers());
            CHECK(s.object().support_layers().empty());
            CHECK(role_totals(s.object()).count.empty());
        }
    }
}

TEST_CASE("Side stabilizers reach the G-code as support, with supports off", "[StabilizersNoSupport]")
{
    auto roles_of = [](bool supports, bool stabilizers) {
        return gcode_roles(Test::slice({ pin_mesh() }, pin_config(supports, stabilizers)));
    };
    const auto off_with = roles_of(false, true);
    const auto on_with  = roles_of(true, true);
    const auto off_none = roles_of(false, false);
    const auto on_none  = roles_of(true, false);

    CHECK(role_lines(off_with, "Support") > 0);
    CHECK(role_lines(off_with, "Support interface") == 0);
    CHECK(role_lines(off_with, "Support transition") == 0);
    // Same section count as with supports on (nothing to support, so the stabilizers are all there is).
    CHECK(role_lines(off_with, "Support") == role_lines(on_with, "Support"));
    CHECK(role_lines(on_with, "Support interface") == 0);

    CHECK(role_lines(off_none, "Support") == 0);
    CHECK(role_lines(on_none, "Support") == 0);
}

TEST_CASE("Side stabilizers alone put the support filament in the print's extruders", "[StabilizersNoSupport]")
{
    for (bool stabilizers : { false, true }) {
        DYNAMIC_SECTION("stabilizers " << stabilizers)
        {
            DynamicPrintConfig config = pin_config(false, stabilizers);
            config.set_num_extruders(2);
            config.set_num_filaments(2);
            config.option<ConfigOptionFloats>("nozzle_diameter")->values   = { 0.4, 0.4 };
            config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
            config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#00FF00" };
            config.set_deserialize_strict({ { "support_filament", "2" } });
            Sliced s;
            slice(s, config);
            const std::vector<unsigned int> extruders = s.print.extruders();
            const bool has_second = std::find(extruders.begin(), extruders.end(), 1u) != extruders.end();
            const bool has_first  = std::find(extruders.begin(), extruders.end(), 0u) != extruders.end();
            CHECK(has_first);
            // The stabilizers print with filament 2; without them nothing does.
            CHECK(has_second == stabilizers);
        }
    }
}

TEST_CASE("Side stabilizers refuse spiral vase mode, supports on or off", "[StabilizersNoSupport]")
{
    for (bool supports : { false, true })
        for (bool stabilizers : { false, true }) {
            DYNAMIC_SECTION("supports " << supports << ", stabilizers " << stabilizers)
            {
                DynamicPrintConfig config = pin_config(supports, stabilizers);
                config.set_deserialize_strict({ { "spiral_mode", "1" } });
                Sliced s;
                ModelObject *object = s.model.add_object();
                object->add_volume(pin_mesh());
                object->add_instance();
                object->ensure_on_bed();
                s.print.auto_assign_extruders(object);
                s.print.apply(s.model, config);
                const StringObjectException err = s.print.validate();
                if (stabilizers) {
                    CHECK(err.opt_key == "stabilizer_supports");
                    CHECK(err.string.find("stabilizers") != std::string::npos);
                } else {
                    CHECK(err.opt_key != "stabilizer_supports");
                }
            }
        }
}

TEST_CASE("The stabilizer bake does not need Enable supports", "[StabilizersNoSupport]")
{
    // The bake plans from the sliced layer outlines and the object's stabilizer settings; supports
    // never enter. Same struts, same mesh, supports on or off.
    Sliced off, on;
    slice(off, pin_config(false, true));
    slice(on, pin_config(true, true));

    StabilizerBakeOptions opts;
    const StabilizerBakeResult a = bake_stabilizers(off.object(), opts);
    const StabilizerBakeResult b = bake_stabilizers(on.object(), opts);
    CHECK(a.error.empty());
    CHECK(b.error.empty());
    REQUIRE_FALSE(a.mesh.indices.empty());
    CHECK(a.struts.size() == b.struts.size());
    CHECK(a.mesh.indices.size() == b.mesh.indices.size());
    CHECK(a.mesh.vertices.size() == b.mesh.vertices.size());

    // And the baked object, as the live source would print: a separate object with no stabilizers
    // and no supports of its own, the source's stabilizers switched off.
    Model model;
    ModelObject *src = model.add_object();
    src->name = "pin";
    src->add_volume(pin_mesh());
    src->add_instance();
    src->config.set_key_value("enable_support", new ConfigOptionBool(false));
    src->config.set_key_value("stabilizer_supports", new ConfigOptionEnum<StabilizerMode>(smAuto));
    REQUIRE(objects_with_live_stabilizers(model, pin_config(false, false)) == std::vector<std::string>{ "pin" });
    ModelObject *baked = apply_stabilizer_bake(model, *src, a, opts);
    REQUIRE(baked != nullptr);
    CHECK(src->config.get().opt_enum<StabilizerMode>("stabilizer_supports") == smOff);
    CHECK(objects_with_live_stabilizers(model, pin_config(false, false)).empty());
}
