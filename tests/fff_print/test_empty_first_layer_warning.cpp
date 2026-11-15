// An object resting on the bed on a contact narrower than the first-layer wall gets no first-layer
// extrusion, so it starts in mid-air and gets no brim (getObjectExtruderMap finds no first-layer
// extruder for it). PrintObject::warn_if_first_layer_prints_nothing says so as a slicing warning.

#include <catch2/catch.hpp>

#include <string>
#include <vector>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

struct ObjPrint
{
    Print print;
    Model model;
};

DynamicPrintConfig ring_config(const char *wall_generator = "classic", const char *thin_walls = "0")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.25" },
        { "initial_layer_line_width",   "0.5" },
        { "line_width",                 "0.42" },
        { "outer_wall_line_width",      "0.45" },
        { "elefant_foot_compensation",  "0" },
        { "enable_support",             "0" },
        { "wall_generator",             wall_generator },
        { "detect_thin_wall",           thin_walls },
    });
    return config;
}

// A ring standing on a 0.45 mm wide rim that flares out to a 3 mm wall above 1 mm, like a
// dome top printed rim-down.
TriangleMesh thin_rim_ring()
{
    const std::vector<Vec2d> profile {
        { 15.5, 0. }, { 15.95, 0. }, { 15.95, 0.3 }, { 17., 1. }, { 17., 4. }, { 14., 4. }, { 14., 1. }, { 15.5, 0.3 },
    };
    return TriangleMesh(its_make_revolved(profile, 180));
}

void add_object(ObjPrint &p, const char *name, const TriangleMesh &mesh, double x)
{
    ModelObject *object = p.model.add_object();
    object->name = name;
    object->add_volume(mesh);
    object->add_instance()->set_offset(Vec3d(x, 0., 0.));
    object->ensure_on_bed();
}

// Returns false when process() stopped on the empty-layer error. With supports off nothing at
// all prints on the rim's first layer, so GCode::collect_layers_to_print rejects the object
// ("empty layer between ...") after the object steps - the warning is already raised by then.
// (On the owner's plate tree support printed on that layer, so there was no error, only no brim.)
bool process(ObjPrint &p, const DynamicPrintConfig &config)
{
    for (ModelObject *object : p.model.objects)
        p.print.auto_assign_extruders(object);
    p.print.apply(p.model, config);
    p.print.set_status_silent();
    try {
        p.print.process();
    } catch (const SlicingErrors &) {
        return false;
    }
    return true;
}

std::string empty_first_layer_warning(const PrintObject &po)
{
    for (const PrintStateBase::Warning &w : po.step_state_with_warnings(posInfill).warnings)
        if (w.message_id == int(PrintStateBase::SlicingEmptyFirstLayer))
            return w.message;
    return {};
}

} // namespace

TEST_CASE("Empty first layer warning: a rim narrower than the first-layer wall warns", "[EmptyFirstLayer]")
{
    ObjPrint p;
    add_object(p, "ring", thin_rim_ring(), 100.);
    process(p, ring_config());
    REQUIRE(p.print.objects().size() == 1);
    const PrintObject &po = *p.print.objects().front();
    REQUIRE(!po.layers().front()->lslices.empty());
    CHECK(!po.layers().front()->has_extrusions());

    const std::string msg = empty_first_layer_warning(po);
    INFO(msg);
    REQUIRE(!msg.empty());
    CHECK(msg.find("ring") != std::string::npos);
    CHECK(msg.find("0.50 mm") != std::string::npos);      // the first-layer wall width
    CHECK(msg.find("Arachne") != std::string::npos);
    CHECK(msg.find("Detect thin wall") != std::string::npos);
}

TEST_CASE("Empty first layer warning: identical objects that share layers each warn", "[EmptyFirstLayer]")
{
    ObjPrint p;
    add_object(p, "ring A", thin_rim_ring(), 60.);
    add_object(p, "ring B", thin_rim_ring(), 140.);
    process(p, ring_config());
    REQUIRE(p.print.objects().size() == 2);
    CHECK(empty_first_layer_warning(*p.print.objects()[0]).find("ring A") != std::string::npos);
    CHECK(empty_first_layer_warning(*p.print.objects()[1]).find("ring B") != std::string::npos);
}

TEST_CASE("Empty first layer warning: no warning when the first layer prints", "[EmptyFirstLayer]")
{
    SECTION("a cube")
    {
        ObjPrint p;
        add_object(p, "cube", TriangleMesh(its_make_cube(20., 20., 10.)), 100.);
        CHECK(process(p, ring_config()));
        CHECK(empty_first_layer_warning(*p.print.objects().front()).empty());
    }
    SECTION("the thin rim with Arachne walls")
    {
        ObjPrint p;
        add_object(p, "ring", thin_rim_ring(), 100.);
        CHECK(process(p, ring_config("arachne")));
        const PrintObject &po = *p.print.objects().front();
        CHECK(po.layers().front()->has_extrusions());
        CHECK(empty_first_layer_warning(po).empty());
    }
    SECTION("the thin rim with Detect thin wall")
    {
        ObjPrint p;
        add_object(p, "ring", thin_rim_ring(), 100.);
        CHECK(process(p, ring_config("classic", "1")));
        const PrintObject &po = *p.print.objects().front();
        CHECK(po.layers().front()->has_extrusions());
        CHECK(empty_first_layer_warning(po).empty());
    }
}
