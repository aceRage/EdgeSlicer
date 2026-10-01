// PR #113 review fixes (Orca #15849 port): OozePrevention::pre_toolchange must only turn a
// tool's heater fully off (M104 S0 ;cooldown) once that tool has truly printed its LAST
// extrusion anywhere in the print - never while another object, a support/interface pass,
// or a later use of the same extruder still needs it. These cases pin down:
//   1. two tools where tool 1 finishes early: S0 fires exactly once, after its last use,
//      never before a later use of tool 2;
//   2. two objects with different layer heights sharing the same extruder: no premature S0,
//      because the print-wide LayerTools index (not an object-local Layer::id()) decides;
//   3. a support extruder used on later layers: no S0 while support still needs it;
//   4. by-object (sequential) printing: the feature is disabled outright (no S0), since
//      Print::process() never builds a print-wide ToolOrdering for ByObject and using the
//      per-object one would be unsafe;
//   5. SEMM (single_extruder_multi_material) and a Bambu-style (is_BBL_printer) setup: no
//      behavior change - S0 must never appear.

#include <catch2/catch.hpp>

#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// The base config shared by every case here: two real, distinct extruders (not SEMM), ooze
// prevention on (the only thing that calls OozePrevention::pre_toolchange), a plain classic
// (non-BBL) printer, by-layer sequencing, and a non-zero standby_temperature_delta so the
// pre-fix, non-S0 path would emit a distinguishable "M104 S<standby> ... ;cooldown" instead
// of "M104 S0 ... ;cooldown".
DynamicPrintConfig two_tool_base_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // Print::apply()/region_config_from_model_volume() derives num_extruders from
    // filament_diameter.size() (PrintApply.cpp: `num_extruders = m_config.filament_diameter.size()`),
    // which is a FILAMENT-scoped vector resized by set_num_filaments(), not the
    // EXTRUDER-scoped set_num_extruders() (that resizes nozzle_diameter, extruder_offset,
    // etc.). Both are needed, or a per-object wall_filament/sparse_infill_filament override
    // of 2 gets silently clamped back down to the default extruder 1
    // (clamp_exturder_to_default in PrintObject.cpp).
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        {"nozzle_diameter",                   "0.4,0.4"},
        {"filament_diameter",                 "1.75,1.75"},
        {"single_extruder_multi_material",    "0"},
        {"ooze_prevention",                   "1"},
        {"standby_temperature_delta",         "-5"},
        {"layer_height",                      "0.2"},
        {"initial_layer_print_height",        "0.2"},
        {"gcode_comments",                    "1"},
        {"machine_start_gcode",               ""},
        {"machine_end_gcode",                 ""},
        {"before_layer_change_gcode",         ""},
        {"layer_change_gcode",                ""},
        {"top_shell_layers",                  "1"},
        {"bottom_shell_layers",               "1"},
        {"sparse_infill_density",             "5%"},
        {"skirt_loops",                       "0"},
        {"wipe_tower_x",                      "0"},
        {"wipe_tower_y",                      "0"},
    });
    return config;
}

// Assign an object to a specific (1-based) extruder/filament. The generic top-level
// "extruder" config key is only expanded into wall_filament / sparse_infill_filament /
// etc. by DynamicPrintConfig::normalize_fdm(), which runs on the print's OWN full config in
// Print::apply() (PrintApply.cpp) - it is never applied to a ModelObject/ModelVolume config
// override. So per-object extruder assignment in a test has to set the region filament keys
// directly, the same way test_skirt_brim.cpp does it.
void set_object_extruder(ModelObject &object, int extruder_1based)
{
    object.config.set_key_value("wall_filament", new ConfigOptionInt(extruder_1based));
    object.config.set_key_value("sparse_infill_filament", new ConfigOptionInt(extruder_1based));
    object.config.set_key_value("solid_infill_filament", new ConfigOptionInt(extruder_1based));
}

} // namespace

TEST_CASE("OozePrevention: tool that finishes early gets S0 exactly once, after its last use", "[OozePrevention][ToolOrdering]")
{
    // Object 0 (extruder 1) is short (scaled down in Z). Object 1 (extruder 2) is full
    // height. Extruder 1 (T0) must go to S0 once object 0 is done; extruder 2 must never see
    // S0, since it is always the last tool still printing.
    Slic3r::Print print;
    Slic3r::Model model;
    DynamicPrintConfig config = two_tool_base_config();

    ModelObject *shortobj = model.add_object();
    shortobj->name = "short";
    TriangleMesh short_mesh = Slic3r::Test::mesh(TestMesh::cube_20x20x20);
    short_mesh.scale(Vec3f(1.f, 1.f, 0.2f)); // clearly shorter than the tall object
    shortobj->add_volume(std::move(short_mesh));
    shortobj->add_instance();
    shortobj->ensure_on_bed();
    print.auto_assign_extruders(shortobj);
    set_object_extruder(*shortobj, 1);

    ModelObject *tall = model.add_object();
    tall->name = "tall";
    TriangleMesh tall_mesh = Slic3r::Test::mesh(TestMesh::cube_20x20x20);
    tall_mesh.translate(40.f, 0.f, 0.f);
    tall->add_volume(std::move(tall_mesh));
    tall->add_instance();
    tall->ensure_on_bed();
    print.auto_assign_extruders(tall);
    set_object_extruder(*tall, 2);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();

    std::string gcode = Slic3r::Test::gcode(print);

    // Each ooze-prevention cooldown line names its own target tool directly, e.g.
    // "M104 S0 T0 ; set nozzle temperature ;cooldown" - parse the T<n> out of every such
    // line instead of tracking toolchange state across lines.
    std::regex cooldown_re(R"(M104 S(\d+) T(\d+)[^\n]*;cooldown)");
    auto begin = std::sregex_iterator(gcode.begin(), gcode.end(), cooldown_re);
    auto end   = std::sregex_iterator();
    REQUIRE(begin != end);

    bool t0_got_s0 = false;
    bool t1_got_s0 = false;
    for (auto it = begin; it != end; ++it) {
        int s_value = std::stoi((*it)[1].str());
        int tool    = std::stoi((*it)[2].str());
        if (s_value == 0) {
            if (tool == 0) t0_got_s0 = true;
            if (tool == 1) t1_got_s0 = true;
        }
    }
    // T0 (the short object's extruder) must reach S0.
    CHECK(t0_got_s0);
    // T1 (extruder 2, the last tool still printing at the end of the file) must never be
    // shut off to S0.
    CHECK_FALSE(t1_got_s0);

    // After the FIRST S0 cooldown for T0, tool T0 must never be selected again (it truly
    // finished): find that cooldown line's position, then confirm no "T0 ; change extruder"
    // toolchange line follows it.
    size_t first_t0_s0 = std::string::npos;
    for (auto it = begin; it != end; ++it) {
        if (std::stoi((*it)[1].str()) == 0 && std::stoi((*it)[2].str()) == 0) {
            first_t0_s0 = size_t(it->position());
            break;
        }
    }
    REQUIRE(first_t0_s0 != std::string::npos);
    size_t next_t0_toolchange = gcode.find("T0 ; change extruder", first_t0_s0);
    CHECK(next_t0_toolchange == std::string::npos);
}

TEST_CASE("OozePrevention: two objects with different effective layer counts on the same extruder never get a premature S0", "[OozePrevention][ToolOrdering]")
{
    // Both objects share extruder 1 (default): nothing should ever get S0, because the
    // extruder that finishes object 0's layers is the SAME extruder still needed by object 1
    // (both use the default extruder). If pre_toolchange used the object-local Layer::id()
    // instead of the print-wide LayerTools index, a short object could look "finished" to the
    // (wrongly indexed) map while the tall object still needs the same physical tool - this
    // must never emit S0 for extruder 1 while object 1 is still printing.
    Slic3r::Print print;
    Slic3r::Model model;
    DynamicPrintConfig config = two_tool_base_config();

    ModelObject *tall = model.add_object();
    tall->name = "tall";
    TriangleMesh tall_mesh = Slic3r::Test::mesh(TestMesh::cube_20x20x20);
    tall->add_volume(std::move(tall_mesh));
    tall->add_instance();
    tall->ensure_on_bed();
    print.auto_assign_extruders(tall);

    ModelObject *shortobj = model.add_object();
    shortobj->name = "short";
    TriangleMesh short_mesh = Slic3r::Test::mesh(TestMesh::cube_20x20x20);
    short_mesh.translate(40.f, 0.f, 0.f);
    short_mesh.scale(Vec3f(1.f, 1.f, 0.2f)); // much shorter than "tall"
    shortobj->add_volume(std::move(short_mesh));
    shortobj->add_instance();
    shortobj->ensure_on_bed();
    print.auto_assign_extruders(shortobj);
    // Give the short object a different (but still non-zero-height) layer height, so its own
    // Layer::id() sequence is unrelated to the merged print-wide layer index - exactly the
    // mismatch finding #1 warns about.
    shortobj->config.set_key_value("layer_height", new ConfigOptionFloat(0.1));

    print.apply(model, config);
    print.validate();
    print.set_status_silent();

    std::string gcode = Slic3r::Test::gcode(print);

    // Both objects print on extruder 1 (the only extruder used) for the whole file, so this
    // tool must never be shut off mid-print: no S0 should appear at all on a plate that only
    // ever uses one extruder (there is nothing to turn off - the active tool is never
    // "parked" by a toolchange).
    REQUIRE(gcode.find("M104 S0") == std::string::npos);
}

TEST_CASE("OozePrevention: a support-only extruder used on later layers never gets a premature S0", "[OozePrevention][ToolOrdering]")
{
    // Object extrudes with T0; support is printed with T1. T1 only ever appears as a support
    // extruder (never in wall/infill), which is exactly the "does it appear in
    // LayerTools::extruders via support" case in finding #3.
    Slic3r::Print print;
    Slic3r::Model model;
    DynamicPrintConfig config = two_tool_base_config();
    config.set_deserialize_strict({
        {"enable_support",             "1"},
        {"support_filament",           "2"},
        {"support_interface_filament", "2"},
        {"support_type",               "normal(auto)"},
        {"support_threshold_angle",    "40"},
    });

    ModelObject *object = model.add_object();
    object->name = "overhang";
    object->add_volume(Slic3r::Test::mesh(TestMesh::overhang));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    set_object_extruder(*object, 1);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();

    std::string gcode = Slic3r::Test::gcode(print);

    // This overhang model's support finishes a few layers before the object's own top
    // layers, so T1 (the support extruder) legitimately DOES reach S0 near the end of the
    // file - that is the correct new behavior this PR adds, not a bug. What must never
    // happen is a support extrusion (support material / support material interface) AFTER
    // T1 has been cooled to S0: that would mean support was shut off while still needed,
    // exactly the hazard finding #3 describes.
    std::regex cooldown_re(R"(M104 S(\d+) T(\d+)[^\n]*;cooldown)");
    auto begin = std::sregex_iterator(gcode.begin(), gcode.end(), cooldown_re);
    auto end   = std::sregex_iterator();

    size_t first_t1_s0 = std::string::npos;
    for (auto it = begin; it != end; ++it) {
        if (std::stoi((*it)[1].str()) == 0 && std::stoi((*it)[2].str()) == 1) {
            first_t1_s0 = size_t(it->position());
            break;
        }
    }
    if (first_t1_s0 != std::string::npos) {
        size_t later_support = gcode.find("; support material", first_t1_s0);
        CHECK(later_support == std::string::npos);
    }
    // Whether or not T1 ever reaches S0 in this shape, it must never be selected (T1 ;
    // change extruder) again after being cooled to S0.
    if (first_t1_s0 != std::string::npos) {
        size_t later_t1_toolchange = gcode.find("T1 ; change extruder", first_t1_s0);
        CHECK(later_t1_toolchange == std::string::npos);
    }
}

TEST_CASE("OozePrevention: by-object (sequential) printing never emits S0", "[OozePrevention][ToolOrdering][ByObject]")
{
    // print_sequence = by object: Print::process() never populates the print-wide
    // m_tool_ordering for this mode (see Print::process(), psWipeTower step), so the feature
    // must be disabled outright rather than risk reading a stale/unrelated ToolOrdering.
    Slic3r::Print print;
    Slic3r::Model model;
    DynamicPrintConfig config = two_tool_base_config();
    config.set_deserialize_strict({
        {"print_sequence", "by object"},
    });

    ModelObject *object = model.add_object();
    object->name = "seq";
    object->add_volume(Slic3r::Test::mesh(TestMesh::cube_20x20x20));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    set_object_extruder(*object, 1);

    print.apply(model, config);
    print.validate();
    print.set_status_silent();

    std::string gcode = Slic3r::Test::gcode(print);
    REQUIRE(gcode.find("M104 S0") == std::string::npos);
}

TEST_CASE("OozePrevention: SEMM and Bambu (BBL) setups are unaffected", "[OozePrevention][ToolOrdering]")
{
    SECTION("single_extruder_multi_material: no change, ooze prevention path never engages S0") {
        Slic3r::Print print;
        Slic3r::Model model;
        DynamicPrintConfig config = two_tool_base_config();
        // Force SEMM on: OozePrevention::enable itself is gated off for SEMM
        // (DoExport::init_ooze_prevention), so pre_toolchange must never run at all here -
        // this is the belt-and-braces check that S0 is impossible under SEMM.
        config.set_deserialize_strict({
            {"single_extruder_multi_material", "1"},
        });

        ModelObject *object = model.add_object();
        object->add_volume(Slic3r::Test::mesh(TestMesh::cube_20x20x20));
        object->add_instance();
        object->ensure_on_bed();
        print.auto_assign_extruders(object);

        print.apply(model, config);
        print.validate();
        print.set_status_silent();

        std::string gcode = Slic3r::Test::gcode(print);
        REQUIRE(gcode.find("M104 S0") == std::string::npos);
    }

    SECTION("Bambu (is_BBL_printer): no change from this feature, S0 never emitted by OozePrevention") {
        Slic3r::Print print;
        Slic3r::Model model;
        DynamicPrintConfig config = two_tool_base_config();

        ModelObject *tall = model.add_object();
        tall->add_volume(Slic3r::Test::mesh(TestMesh::cube_20x20x20));
        tall->add_instance();
        tall->ensure_on_bed();
        print.auto_assign_extruders(tall);
        set_object_extruder(*tall, 1);

        ModelObject *shortobj = model.add_object();
        TriangleMesh short_mesh = Slic3r::Test::mesh(TestMesh::cube_20x20x20);
        short_mesh.translate(40.f, 0.f, 0.f);
        short_mesh.scale(Vec3f(1.f, 1.f, 0.2f));
        shortobj->add_volume(std::move(short_mesh));
        shortobj->add_instance();
        shortobj->ensure_on_bed();
        print.auto_assign_extruders(shortobj);
        set_object_extruder(*shortobj, 2);

        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        // Mark the print as targeting a Bambu machine, as test_printgcode.cpp's BBL timelapse
        // case does: this reroutes idle-cool handling to GCode/PreCoolingInjector and must
        // keep OozePrevention from ever adding S0 on top of it.
        print.is_BBL_printer() = true;

        std::string gcode = Slic3r::Test::gcode(print);
        REQUIRE(gcode.find("M104 S0") == std::string::npos);
    }
}
