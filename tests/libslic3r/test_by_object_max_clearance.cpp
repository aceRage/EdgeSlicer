#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include "../fff_print/test_data.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>
#include <memory>
#include <string>

using namespace Slic3r;

// Print by object: Bambu Studio keeps extruder_clearance_max_radius (H2D: 96 mm) between objects
// in its collision check, its collision preview and its arrange; the fork used its own
// extruder_clearance_radius (H2D profile: 49 mm, a leftover Bambu Studio ignores). On a Bambu Lab
// printer the fork now uses the max radius too (sequential_clearance_radius); every other printer
// keeps extruder_clearance_radius, so a profile that never set the max radius does not pick up its
// 68 mm default.

namespace {

PresetBundle &bbl_bundle()
{
    static std::unique_ptr<PresetBundle> bundle;
    static std::unique_ptr<PresetBundle> library;
    if (bundle)
        return *bundle;
    const std::string saved_data_dir = data_dir();
    const boost::filesystem::path scratch = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("by_object_clearance_%%%%-%%%%");
    boost::filesystem::create_directories(scratch);
    set_data_dir(scratch.string());
    const std::string profiles = (boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources" / "profiles").string();
    library = std::make_unique<PresetBundle>();
    library->load_vendor_configs_from_json(profiles, PresetBundle::ORCA_FILAMENT_LIBRARY, PresetBundle::LoadSystem,
                                           ForwardCompatibilitySubstitutionRule::EnableSilent);
    bundle = std::make_unique<PresetBundle>();
    bundle->load_vendor_configs_from_json(profiles, "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent,
                                          library.get());
    set_data_dir(saved_data_dir);
    return *bundle;
}

DynamicPrintConfig h2d_by_object_config()
{
    PresetBundle &b = bbl_bundle();
    const std::string filament = "Bambu PLA Basic @BBL H2D";
    REQUIRE(b.printers.select_preset_by_name("Bambu Lab H2D 0.4 nozzle", true));
    REQUIRE(b.prints.select_preset_by_name("0.20mm Standard @BBL H2D", true));
    REQUIRE(b.filaments.select_preset_by_name(filament, true));
    b.filament_presets = { filament };
    b.set_num_filaments(1, std::vector<std::string>{ "#E01919" });
    DynamicPrintConfig cfg = b.full_config_secure();
    cfg.set_deserialize_strict({
        { "print_sequence", "by object" },
        { "enable_prime_tower", "0" },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
    });
    return cfg;
}

// Two 20 x 20 x 40 mm blocks (taller than the nozzle, so the full clearance applies) with `gap`
// millimetres between them along X.
void add_two_blocks(Model &model, double gap)
{
    const double xs[] = { 175. - (20. + gap) / 2., 175. + (20. + gap) / 2. };
    for (int i = 0; i < 2; ++i) {
        TriangleMesh block = Test::mesh(Test::TestMesh::cube_20x20x20);
        block.scale(Vec3f(1.f, 1.f, 2.f));
        ModelObject *object = model.add_object();
        object->name        = "block" + std::to_string(i);
        object->add_volume(block);
        object->add_instance();
        object->center_around_origin();
        object->instances.front()->set_offset(Vec3d(xs[i], 160., 0.));
        object->ensure_on_bed();
    }
}

StringObjectException clearance(const DynamicPrintConfig &cfg, double gap, bool bbl)
{
    Print print;
    Model model;
    add_two_blocks(model, gap);
    print.is_BBL_printer() = bbl;
    print.apply(model, cfg);
    return Print::sequential_print_clearance_valid(print);
}

} // namespace

TEST_CASE("Print by object on an H2D keeps Bambu Studio's 96 mm clearance", "[ByObjectClearance]")
{
    const DynamicPrintConfig h2d = h2d_by_object_config();
    REQUIRE(h2d.opt_float("extruder_clearance_max_radius") == Approx(96.));
    REQUIRE(h2d.opt_float("extruder_clearance_radius") == Approx(49.));

    SECTION("the clearance radius and the arrange distance are the max radius") {
        CHECK(sequential_clearance_radius(h2d) == Approx(96.));
        CHECK(min_object_distance(h2d) == Approx(96.));
    }
    SECTION("70 mm apart (more than 49, less than 96): now a collision") {
        CHECK_FALSE(clearance(h2d, 70., true).string.empty());
    }
    SECTION("100 mm apart: clear") {
        CHECK(clearance(h2d, 100., true).string.empty());
    }
}

TEST_CASE("Print by object on other printers keeps extruder_clearance_radius", "[ByObjectClearance]")
{
    // The same H2D settings under a non-Bambu printer model: the 96 mm max radius is ignored.
    DynamicPrintConfig other = h2d_by_object_config();
    other.set_deserialize_strict({ { "printer_model", "Generic Klipper Printer" } });

    CHECK(sequential_clearance_radius(other) == Approx(49.));
    CHECK(min_object_distance(other) == Approx(49.));
    CHECK(clearance(other, 70., false).string.empty());
    CHECK_FALSE(clearance(other, 40., false).string.empty());

    SECTION("a profile that never set the max radius keeps its own radius, not the 68 mm default") {
        DynamicPrintConfig plain = DynamicPrintConfig::full_print_config();
        plain.set_deserialize_strict({ { "print_sequence", "by object" }, { "extruder_clearance_radius", 35. } });
        REQUIRE(plain.opt_float("extruder_clearance_max_radius") == Approx(68.));
        CHECK(sequential_clearance_radius(plain) == Approx(35.));
        CHECK(min_object_distance(plain) == Approx(35.));
    }
}

// Print by object on a bed-slinger: Bambu Studio refuses an object taller than extruder_clearance_height_to_rod
// when a later object is closer to it in Y than extruder_clearance_dist_to_rod; the fork's error reaches only half
// the clearance radius. In between, the fork now warns and still slices. Modelled on a plate from an A2L: a 66 mm
// cylinder printed first, a 39.6 mm cube after it, a 16.5 mm cube last at the back.

namespace {

// The A2L's clearance values on a generic (non-Bambu) config, so the clearance radius is extruder_clearance_radius.
DynamicPrintConfig a2l_like_by_object_config(const char *structure = "i3")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict({
        { "print_sequence", "by object" },
        { "printer_structure", structure },
        { "printable_area", "0x0,330x0,330x320,0x320" },
        { "printable_height", 325. },
        { "extruder_clearance_radius", 73. },
        { "extruder_clearance_max_radius", 73. },
        { "extruder_clearance_dist_to_rod", 56.5 },
        { "extruder_clearance_height_to_rod", 25. },
        { "extruder_clearance_height_to_lid", 325. },
        { "skirt_loops", 0 },
        { "brim_type", "no_brim" },
        { "enable_prime_tower", "0" },
    });
    return cfg;
}

ModelObject *add_object_at(Model &model, const std::string &name, TriangleMesh mesh, double center_x, double center_y)
{
    ModelObject *object = model.add_object();
    object->name        = name;
    object->add_volume(std::move(mesh));
    object->add_instance();
    object->center_around_origin();
    object->instances.front()->set_offset(Vec3d(center_x, center_y, 0.));
    object->ensure_on_bed();
    return object;
}

// The cylinder (66 mm across, 66 mm tall) at Y 22.5 .. 88.5, a 39.6 mm cube `gap` mm behind it in Y (far to the
// right in X), and a 16.5 mm cube at the back. Listed, and so printed, in that order.
void add_rod_plate(Model &model, double gap, double cylinder_height = 66.)
{
    add_object_at(model, "Cylinder", make_cylinder(33., cylinder_height), 67.3, 55.5);
    add_object_at(model, "Cube", make_cube(39.6, 39.6, 39.6), 260.8, 88.5 + gap + 19.8);
    add_object_at(model, "Small cube", make_cube(16.5, 16.5, 16.5), 101.75, 259.75);
}

// Y distance between the footprints of two objects, the first in front of the second.
double y_gap(const Model &model, size_t front, size_t back)
{
    return model.objects[back]->instance_bounding_box(0).min.y() - model.objects[front]->instance_bounding_box(0).max.y();
}

struct RodCheck
{
    StringObjectException error;
    StringObjectException warning;
    std::vector<int>      arrange_order;
    std::string           warning_object; // name of the object the warning jumps to
};

RodCheck rod_check(const DynamicPrintConfig &cfg, Model &model)
{
    Print print;
    print.apply(model, cfg);
    RodCheck out;
    out.error = Print::sequential_print_clearance_valid(print, nullptr, nullptr, &out.warning);
    // The print order lands on the print's own copy of the model.
    for (const ModelObject *object : print.model().objects)
        out.arrange_order.push_back(object->instances.front()->arrange_order);
    // The warning points into the print's model, which goes with the print.
    if (const auto *object = dynamic_cast<const ModelObject *>(out.warning.object))
        out.warning_object = object->name;
    out.warning.object = nullptr;
    return out;
}

const std::string rod_warning_cylinder =
    "Cylinder is tall and close to an object printed after it. The printer's gantry may hit it. Move them further apart, or print it last.";

} // namespace

TEST_CASE("Print by object warns when a tall object is within the gantry's reach of a later one", "[ByObjectClearance]")
{
    const DynamicPrintConfig cfg = a2l_like_by_object_config();
    // The reach of the existing error (half the clearance radius, no skirt) and of Bambu Studio's (dist to rod).
    const double error_reach = 0.5 * sequential_clearance_radius(cfg);
    const double rod_reach   = cfg.opt_float("extruder_clearance_dist_to_rod");
    REQUIRE(error_reach < rod_reach);

    SECTION("the A2L plate (44.7 mm in Y): sliced, with a warning naming the cylinder; the order is the list order") {
        Model model;
        add_rod_plate(model, 44.7);
        const double gap = y_gap(model, 0, 1);
        CHECK(gap == Approx(44.7).margin(0.01));
        REQUIRE(gap > error_reach);
        REQUIRE(gap < rod_reach);
        const RodCheck check = rod_check(cfg, model);
        CHECK(check.error.string.empty());
        CHECK(check.warning.string == rod_warning_cylinder);
        CHECK(check.warning_object == "Cylinder");
        // The 39.6 mm cube is above the rod too, but the small cube is further than the rod's reach from it.
        CHECK(y_gap(model, 1, 2) > rod_reach);
        CHECK(check.arrange_order == std::vector<int>{ 1, 2, 3 });
    }
    SECTION("between the two reaches, anywhere: warning") {
        Model model;
        add_rod_plate(model, 0.5 * (error_reach + rod_reach));
        const RodCheck check = rod_check(cfg, model);
        CHECK(check.error.string.empty());
        CHECK(check.warning.string == rod_warning_cylinder);
    }
    SECTION("further than the rod's reach: no warning") {
        Model model;
        add_rod_plate(model, rod_reach + 2.);
        REQUIRE(y_gap(model, 0, 1) > rod_reach);
        const RodCheck check = rod_check(cfg, model);
        CHECK(check.error.string.empty());
        CHECK(check.warning.string.empty());
    }
    SECTION("inside the error's reach: the same error as before, and no warning") {
        Model model;
        add_rod_plate(model, error_reach - 6.);
        REQUIRE(y_gap(model, 0, 1) < error_reach);
        const RodCheck check = rod_check(cfg, model);
        // Both tall objects reach each other, so no order helps and both are named, in the existing wording.
        CHECK_THAT(check.error.string, Catch::Contains("Cylinder is too tall, and collisions will be caused."));
        CHECK_THAT(check.error.string, Catch::Contains("Cube is too tall, and collisions will be caused."));
        CHECK(check.error.string.size() == std::string("Cylinder is too tall, and collisions will be caused.\n"
                                                       "Cube is too tall, and collisions will be caused.").size());
        CHECK(check.warning.string.empty());
    }
    SECTION("nothing reaches the rod: no warning") {
        Model model;
        add_rod_plate(model, 44.7, cfg.opt_float("extruder_clearance_height_to_rod") - 5.);
        model.objects[1]->scale(Vec3d(1., 1., 0.5)); // 19.8 mm
        model.objects[1]->ensure_on_bed();
        const RodCheck check = rod_check(cfg, model);
        CHECK(check.error.string.empty());
        CHECK(check.warning.string.empty());
    }
    SECTION("the tall object alone: no warning") {
        Model model;
        add_object_at(model, "Cylinder", make_cylinder(33., 66.), 67.3, 55.5);
        const RodCheck check = rod_check(cfg, model);
        CHECK(check.error.string.empty());
        CHECK(check.warning.string.empty());
    }
    SECTION("printed last, the cylinder is not named; the cube printed before it, 44.7 mm away, is") {
        Model model;
        add_rod_plate(model, 44.7);
        // Move the cylinder to the end of the list: nothing is printed after it.
        std::rotate(model.objects.begin(), model.objects.begin() + 1, model.objects.end());
        REQUIRE(model.objects.back()->name == "Cylinder");
        const RodCheck check = rod_check(cfg, model);
        CHECK(check.error.string.empty());
        CHECK(check.warning.string ==
              "Cube is tall and close to an object printed after it. The printer's gantry may hit it. Move them further apart, or print it last.");
        CHECK(check.arrange_order == std::vector<int>{ 1, 2, 3 });
    }
    SECTION("the cube made shorter than the rod and the cylinder printed last: no warning") {
        Model model;
        add_rod_plate(model, 44.7);
        model.objects[1]->scale(Vec3d(1., 1., 0.5)); // 19.8 mm
        model.objects[1]->ensure_on_bed();
        std::rotate(model.objects.begin(), model.objects.begin() + 1, model.objects.end());
        const RodCheck check = rod_check(cfg, model);
        CHECK(check.error.string.empty());
        CHECK(check.warning.string.empty());
    }
    SECTION("not an i3 printer: no warning") {
        Model model;
        add_rod_plate(model, 44.7);
        for (const char *structure : { "corexy", "undefine" }) {
            const RodCheck check = rod_check(a2l_like_by_object_config(structure), model);
            CAPTURE(structure);
            CHECK(check.error.string.empty());
            CHECK(check.warning.string.empty());
        }
    }
}

TEST_CASE("Print::validate reports the gantry-reach warning and does not refuse the plate", "[ByObjectClearance]")
{
    const auto rod_warnings = [](const std::vector<StringObjectException> &warnings) {
        std::vector<std::string> out;
        for (const StringObjectException &w : warnings)
            if (w.string.find("is tall and close to an object printed after it") != std::string::npos) {
                CHECK(w.is_warning);
                out.push_back(w.string);
            }
        return out;
    };
    Model model;
    add_rod_plate(model, 44.7);

    SECTION("by object") {
        Print print;
        print.apply(model, a2l_like_by_object_config());
        std::vector<StringObjectException> warnings;
        const StringObjectException err = print.validate(&warnings);
        CHECK(err.string.empty());
        CHECK(rod_warnings(warnings) == std::vector<std::string>{ rod_warning_cylinder });
    }
    SECTION("by layer: no warning") {
        DynamicPrintConfig cfg = a2l_like_by_object_config();
        cfg.set_deserialize_strict({ { "print_sequence", "by layer" } });
        Print print;
        print.apply(model, cfg);
        std::vector<StringObjectException> warnings;
        print.validate(&warnings);
        CHECK(rod_warnings(warnings).empty());
    }
}
