// Mixed color sublayer, phase 1 (tests/design_mixed_color_sublayer.md): a painted cube sliced with a Ratio row.
// The plan has one pass per component at its sub-Z (heights sum to the layer height), the first layer is not
// split, the start component alternates, the G-code has no extra layer-change markers and each component's
// extrusion follows the row's ratio. The same-diameter rule refuses a mix across nozzle sizes.

#include <catch2/catch.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedSublayer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Two physical filaments on a two-head tool changer (no shared nozzle, no prime tower) and one mixed row
// A = 1, B = 2 with 30 % B. The row is virtual filament 3.
DynamicPrintConfig mixed_sublayer_config(bool enable_ratio, bool local_z, const std::vector<double> &nozzles = { 0.4, 0.4 })
{
    MixedFilamentManager::set_auto_generate_enabled(false);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = { 1.75, 1.75 };
    config.option<ConfigOptionStrings>("filament_colour")->values  = { "#FF0000", "#0000FF" };
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = nozzles;
    config.option<ConfigOptionFloats>("min_layer_height")->values  = { 0.04, 0.04 };
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    for (const char *key : { "line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width",
                             "top_surface_line_width", "sparse_infill_line_width", "internal_solid_infill_line_width" })
        if (auto *opt = config.option<ConfigOptionFloatOrPercent>(key); opt != nullptr) {
            opt->value   = 0.42;
            opt->percent = false;
        }
    config.set_deserialize_strict({
        { "layer_height", "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "use_relative_e_distances", "1" },
        { "enable_mixed_color_sublayer", enable_ratio ? "1" : "0" },
        { "dithering_local_z_mode", local_z ? "1" : "0" },
    });

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 30, { "#FF0000", "#0000FF" });
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
    return config;
}

// A 10 x 10 x 3 mm cube painted all over with the mixed row (filament 3).
const PrintObject *slice_painted_cube(Print &print, Model &model, const DynamicPrintConfig &config, bool process = true)
{
    ModelObject *object = model.add_object();
    object->name        = "mixed_cube";
    ModelVolume *v      = object->add_volume(TriangleMesh(its_make_cube(10., 10., 3.)));
    v->config.set("extruder", 1);
    TriangleSelector selector(v->mesh());
    for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
        selector.set_facet(f, EnforcerBlockerType(3));
    REQUIRE(v->mmu_segmentation_facets.set(selector));
    object->add_instance();
    object->ensure_on_bed();
    print.apply(model, config);
    print.set_status_silent();
    if (process)
        print.process();
    REQUIRE(!print.objects().empty());
    return print.objects().front();
}

} // namespace

TEST_CASE("Mixed sublayer: Ratio plan on a painted cube", "[MixedSublayer][Slice]")
{
    Print              print;
    Model              model;
    const PrintObject *object = slice_painted_cube(print, model, mixed_sublayer_config(true, false));
    const auto        &intervals = object->local_z_intervals();
    const auto        &plans     = object->local_z_sublayer_plan();
    REQUIRE(!intervals.empty());

    size_t       ratio_layers = 0;
    unsigned int last_top     = 0;
    double       total_a = 0., total_b = 0., total_h = 0.;
    for (const LocalZInterval &interval : intervals) {
        INFO("layer " << interval.layer_id);
        if (interval.layer_id == 0) {
            // The first layer is never split.
            CHECK_FALSE(local_z_interval_has_passes(interval, plans));
            continue;
        }
        if (!local_z_interval_has_passes(interval, plans))
            continue;
        ++ratio_layers;
        double       sum = 0.;
        double       a = 0., b = 0.;
        unsigned int first_extruder = 0, last_extruder = 0;
        for (size_t i = interval.first_sublayer_idx; i < interval.first_sublayer_idx + interval.sublayer_count; ++i) {
            const SubLayerPlan &plan = plans[i];
            REQUIRE(plan.ratio_pass);
            CHECK_THAT(plan.flow_height, WithinAbs(plan.z_hi - plan.z_lo, 1e-9));
            CHECK_THAT(plan.print_z, WithinAbs(plan.z_hi, 1e-9));
            sum += plan.flow_height;
            unsigned int extruder = 0;
            for (size_t e = 0; e < plan.painted_masks_by_extruder.size(); ++e)
                if (!plan.painted_masks_by_extruder[e].empty())
                    extruder = unsigned(e + 1);
            REQUIRE(extruder != 0);
            (extruder == 1 ? a : b) += plan.flow_height;
            if (first_extruder == 0)
                first_extruder = extruder;
            last_extruder = extruder;
        }
        CHECK_THAT(sum, WithinAbs(interval.base_height, 1e-9));
        total_a += a;
        total_b += b;
        total_h += interval.base_height;
        CHECK(plans[interval.first_sublayer_idx + interval.sublayer_count - 1].ratio_last_pass);
        // Alternating order: a layer printed by both components starts with the one that ended the layer below.
        // The top layer uses the ascending (Bambu Studio) order, so the higher id is on top. A layer whose thin
        // pass fell under the nozzle's minimum layer height is printed by one component (D5 carry-over).
        const bool top = interval.layer_id + 1 == object->layer_count();
        if (interval.sublayer_count >= 2) {
            if (last_top != 0 && !top)
                CHECK(first_extruder == last_top);
            if (top)
                CHECK(last_extruder == 2);
        }
        last_top = last_extruder;
    }
    CHECK(ratio_layers + 1 == object->layer_count());
    // Over the split layers each component prints its share to within one thin pass (the carry is bounded).
    REQUIRE(total_h > 0.);
    CHECK_THAT(total_b, WithinAbs(0.3 * total_h, 0.1));
    CHECK_THAT(total_a + total_b, WithinAbs(total_h, 1e-6));
}

TEST_CASE("Mixed sublayer: Ratio G-code keeps one layer change per layer", "[MixedSublayer][Slice]")
{
    Print       print;
    Model       model;
    slice_painted_cube(print, model, mixed_sublayer_config(true, false), false);
    const std::string gcode = Test::gcode(print);
    REQUIRE(!gcode.empty());
    const PrintObject *object = print.objects().front();

    const std::string layer_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change);
    size_t            layer_changes = 0;
    size_t            m73_layer     = 0;
    size_t            toolchanges   = 0;
    int               tool          = -1;
    std::map<int, double> extruded_after_first_layer;
    std::istringstream    in(gcode);
    std::string           line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(layer_tag, 0) == 0) {
            ++layer_changes;
            continue;
        }
        if (line.rfind("M73 L", 0) == 0) {
            ++m73_layer;
            continue;
        }
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            tool = std::atoi(line.c_str() + 1);
            ++toolchanges;
            continue;
        }
        if (layer_changes < 2 || tool < 0 || line.rfind("G1 ", 0) != 0)
            continue;
        if (line.find(" X") == std::string::npos && line.find(" Y") == std::string::npos)
            continue;
        const size_t epos = line.find(" E");
        if (epos == std::string::npos)
            continue;
        const double e = std::atof(line.c_str() + epos + 2);
        if (e > 0.)
            extruded_after_first_layer[tool] += e;
    }
    // Sub-passes move Z inside the layer; they never announce a layer.
    CHECK(layer_changes == object->layer_count());
    CHECK((m73_layer == 0 || m73_layer <= object->layer_count() + 1));
    // Alternating order: one change per layer (the next layer starts with the filament that ended this one, and
    // the layer's nominal extruder is that filament), plus the start and the first layer's cadence pick.
    CHECK(toolchanges <= object->layer_count() + 3);
    REQUIRE(extruded_after_first_layer.count(0) == 1);
    REQUIRE(extruded_after_first_layer.count(1) == 1);
    const double share_b = extruded_after_first_layer[1] / (extruded_after_first_layer[0] + extruded_after_first_layer[1]);
    CHECK_THAT(share_b, WithinAbs(0.3, 0.05));
}

TEST_CASE("Mixed sublayer: off path plans as before", "[MixedSublayer][Slice]")
{
    SECTION("both keys off: no sub-layer plan")
    {
        Print              print;
        Model              model;
        const PrintObject *object = slice_painted_cube(print, model, mixed_sublayer_config(false, false));
        CHECK(object->local_z_intervals().empty());
    }
    SECTION("Subdivide Mix Layer only: Local-Z passes, none of them Ratio")
    {
        Print              print;
        Model              model;
        const PrintObject *object = slice_painted_cube(print, model, mixed_sublayer_config(false, true));
        const auto        &plans  = object->local_z_sublayer_plan();
        CHECK(!plans.empty());
        for (const SubLayerPlan &plan : plans)
            CHECK_FALSE(plan.ratio_pass);
    }
    SECTION("both on: Subdivide wins")
    {
        Print              print;
        Model              model;
        const PrintObject *object = slice_painted_cube(print, model, mixed_sublayer_config(true, true));
        for (const SubLayerPlan &plan : object->local_z_sublayer_plan())
            CHECK_FALSE(plan.ratio_pass);
    }
}

TEST_CASE("Mixed sublayer: a mix across nozzle diameters is refused", "[MixedSublayer][Slice]")
{
    Print print;
    Model model;
    slice_painted_cube(print, model, mixed_sublayer_config(true, false, { 0.4, 0.6 }), false);
    const StringObjectException err = print.validate();
    CHECK(err.string.find("same nozzle diameter") != std::string::npos);

    // The same printer without the sub-layer setting is not refused for it.
    Print print_off;
    Model model_off;
    slice_painted_cube(print_off, model_off, mixed_sublayer_config(false, false, { 0.4, 0.6 }), false);
    CHECK(print_off.validate().string.find("same nozzle diameter") == std::string::npos);
}

namespace {

// Painted mixed cubes of the given heights side by side, with a Type2 prime tower and precise Z height on.
// The mixed row's two heads are separate nozzles, so a Ratio layer's pass changes are planned on the tower
// as Local-Z tool changes (D10 gives them the prime volume).
std::string slice_mixed_on_tower(Print &print, Model &model, const std::vector<double> &heights)
{
    DynamicPrintConfig config = mixed_sublayer_config(true, false);
    config.option<ConfigOptionBool>("enable_prime_tower")->value = true;
    config.set_deserialize_strict({
        { "wipe_tower_type", "type2" },
        { "precise_z_height", "1" },
        { "wipe_tower_x", "15" },
        { "wipe_tower_y", "15" },
        { "skirt_loops", "0" },
        { "brim_type", "no_brim" },
    });
    double x = 100.;
    for (double height : heights) {
        ModelObject *object = model.add_object();
        object->name        = "mixed_cube";
        ModelVolume *v      = object->add_volume(TriangleMesh(its_make_cube(10., 10., height)));
        v->config.set("extruder", 1);
        TriangleSelector selector(v->mesh());
        for (int f = 0; f < int(v->mesh().its.indices.size()); ++f)
            selector.set_facet(f, EnforcerBlockerType(3));
        REQUIRE(v->mmu_segmentation_facets.set(selector));
        object->add_instance()->set_offset(Vec3d(x, 100., 0.));
        object->ensure_on_bed();
        x += 30.;
    }
    // A second apply, as the wipe tower tests do: the first one can still see one filament and drop the tower.
    print.apply(model, config);
    print.apply(model, config);
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);
    REQUIRE(print.has_wipe_tower());
    return gcode;
}

// Z of every extruding move under the tower's feature tag.
std::vector<double> mixed_tower_extrusion_z(const std::string &gcode)
{
    std::vector<double> zs;
    bool                in_tower = false;
    GCodeReader         parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view raw = line.raw();
        if (raw.rfind(";TYPE:", 0) == 0)
            in_tower = raw.find("Prime tower") != std::string_view::npos;
        else if (in_tower && line.extruding(self) && (line.has_x() || line.has_y()))
            zs.emplace_back(self.z());
    });
    return zs;
}

size_t planned_local_z_tool_changes(const Print &print)
{
    size_t count = 0;
    for (const auto &layer : print.wipe_tower_data().local_z_tool_changes)
        count += layer.size();
    return count;
}

} // namespace

TEST_CASE("Mixed sublayer: Ratio layers on a prime tower print the tower at its own layers", "[MixedSublayer][Slice][WipeTower]")
{
    // A Ratio layer's passes live inside one tool-ordering layer, so they never look like a precise-Z sliver
    // (Orca #17204) to the tower. With a second cube 3.15 mm tall, precise Z height leaves the 6 mm cube's
    // layers 0.01 to 0.05 mm above the short one's near its top, under the 0.04 mm minimum for some: those
    // join the tower layer below, and their Local-Z pass changes must print down on it too.
    const double short_height = GENERATE(0., 3.15);
    CAPTURE(short_height);
    std::vector<double> heights { 6. };
    if (short_height > 0.)
        heights.emplace_back(short_height);

    Print             print;
    Model             model;
    const std::string gcode = slice_mixed_on_tower(print, model, heights);
    REQUIRE(!gcode.empty());
    CHECK(planned_local_z_tool_changes(print) > 0);

    size_t              joined = 0;
    std::vector<double> tower_layer_z;
    for (const LayerTools &lt : print.wipe_tower_data().tool_ordering) {
        if (lt.joins_wipe_tower_layer_below()) {
            ++joined;
            CHECK(lt.print_z - lt.wipe_tower_print_z < 0.04);
        } else if (lt.has_wipe_tower)
            tower_layer_z.emplace_back(lt.print_z);
    }
    if (short_height == 0.)
        CHECK(joined == 0);
    else
        CHECK(joined > 0);

    const std::vector<double> zs = mixed_tower_extrusion_z(gcode);
    REQUIRE_FALSE(zs.empty());
    size_t             off_layer = 0;
    std::ostringstream off_layer_z;
    for (double z : zs)
        if (std::none_of(tower_layer_z.begin(), tower_layer_z.end(), [z](double t) { return std::abs(z - t) < 0.002; })) {
            if (off_layer++ < 10)
                off_layer_z << z << " ";
        }
    INFO("tower extrusions off the tower layers, first Z values: " << off_layer_z.str());
    CHECK(off_layer == 0);
}
