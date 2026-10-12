// Support base patterns and the warnings about them (Batch 3C of the Orca upstream port):
//
//  * Orca #12141 lets organic tree supports print a base pattern inside their branches: Hollow and
//    Default keep the hollow branches (one or two walls), every other pattern fills them. It warns
//    when two walls are asked for with a filled pattern (organic prints a single sheath then) and
//    when Lightning is picked for a support type that cannot print it (Rectilinear is used).
//  * Orca #12710 warns when Hollow is picked for a non-tree support type (Rectilinear is used).
//
// Upstream shipped no tests for either; these are ours.

#include <catch2/catch.hpp>

#include <cmath>
#include <string>
#include <string_view>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// ------------------------------------------------------------------------------------- warnings

// The non-blocking warning Print::validate() leaves for a 20 mm cube with supports on.
StringObjectException validate_warning(std::initializer_list<ConfigBase::SetDeserializeItem> items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", "1" },
        // full_print_config() has relative E, which validate() refuses without a per-layer reset.
        { "layer_change_gcode", "G92 E0" },
    });
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
    StringObjectException warning;
    const StringObjectException error = print.validate(&warning);
    INFO("validate error: " << error.string);
    REQUIRE(error.string.empty());
    return warning;
}

const std::string kTwoWalls  = "For Organic supports, two walls are supported only with the Hollow/Default base pattern.";
const std::string kLightning = "The Lightning base pattern is not supported by this support type; Rectilinear will be used instead.";
const std::string kHollow    = "The Hollow base pattern is not supported by this support type; Rectilinear will be used instead.";

// ----------------------------------------------------------------------------- organic base fill

// G-code of an organic tree under the overhang test mesh, with role comments.
std::string slice_organic(std::initializer_list<ConfigBase::SetDeserializeItem> items)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",               "1" },
        { "support_type",                 "tree(auto)" },
        { "support_style",                "organic" },
        { "layer_height",                 "0.2" },
        { "support_on_build_plate_only",  "1" },
        { "support_interface_top_layers", "2" },
    });
    config.set_deserialize_strict(items);
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));
    return Slic3r::Test::slice({ TestMesh::overhang }, config);
}

// Extruded XY length of support BASE material (not interface).
double support_base_length(const std::string &gcode)
{
    double      len = 0.;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&len](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self))
            return;
        const std::string_view comment = line.comment();
        if (comment.find("support material") != std::string_view::npos && comment.find("interface") == std::string_view::npos)
            len += std::hypot(line.dist_X(self), line.dist_Y(self));
    });
    return len;
}

// The G-code without the lines that name the settings (the config block and the header), which
// differ by construction between two slices with different base patterns.
std::string toolpaths_only(const std::string &gcode)
{
    std::string out;
    size_t      begin = 0;
    while (begin < gcode.size()) {
        size_t end = gcode.find('\n', begin);
        if (end == std::string::npos)
            end = gcode.size();
        const std::string_view line(gcode.data() + begin, end - begin);
        begin = end + 1;
        if (! line.empty() && line.front() == ';')
            continue;
        out.append(line.data(), line.size());
        out.push_back('\n');
    }
    return out;
}

} // namespace

TEST_CASE("Organic supports warn about two walls with a filled base pattern", "[SupportPatterns][Validate]")
{
    for (const char *style : { "organic", "default" }) {
        INFO("support_style " << style);
        const StringObjectException w = validate_warning({ { "support_type", "tree(auto)" }, { "support_style", style },
                                                           { "tree_support_wall_count", "2" }, { "support_base_pattern", "rectilinear" } });
        CHECK(w.string == kTwoWalls);
        CHECK(w.opt_key == "support_base_pattern");
    }
    // Hollow and Default keep the double walls: nothing to warn about.
    for (const char *pattern : { "hollow", "default" }) {
        INFO("support_base_pattern " << pattern);
        const StringObjectException w = validate_warning({ { "support_type", "tree(auto)" }, { "support_style", "organic" },
                                                           { "tree_support_wall_count", "2" }, { "support_base_pattern", pattern } });
        CHECK(w.string.empty());
    }
    // One wall with a filled pattern is fine.
    CHECK(validate_warning({ { "support_type", "tree(auto)" }, { "support_style", "organic" },
                             { "tree_support_wall_count", "1" }, { "support_base_pattern", "rectilinear" } }).string.empty());
    // The classic tree styles print their walls with any pattern.
    CHECK(validate_warning({ { "support_type", "tree(auto)" }, { "support_style", "tree_slim" },
                             { "tree_support_wall_count", "2" }, { "support_base_pattern", "rectilinear" } }).string.empty());
}

TEST_CASE("Lightning warns for every support type but the classic trees", "[SupportPatterns][Validate]")
{
    auto [type, style, warns] = GENERATE(table<const char *, const char *, bool>({
        { "normal(auto)", "default",     true },
        { "normal(auto)", "snug",        true },
        { "normal(manual)", "grid",      true },
        { "tree(auto)",   "organic",     true },
        { "tree(auto)",   "default",     true },
        { "tree(auto)",   "tree_slim",   false },
        { "tree(auto)",   "tree_strong", false },
        { "tree(auto)",   "tree_hybrid", false },
    }));
    CAPTURE(type, style);
    const StringObjectException w = validate_warning({ { "support_type", type }, { "support_style", style },
                                                       { "support_base_pattern", "lightning" } });
    if (warns) {
        CHECK(w.string == kLightning);
        CHECK(w.opt_key == "support_base_pattern");
    } else {
        CHECK(w.string.empty());
    }
}

TEST_CASE("Hollow warns for the non-tree support types only", "[SupportPatterns][Validate]")
{
    // Orca #12710: normal supports have no hollow mode and print Rectilinear.
    auto [type, style, warns] = GENERATE(table<const char *, const char *, bool>({
        { "normal(auto)",   "default",     true },
        { "normal(auto)",   "grid",        true },
        { "normal(auto)",   "snug",        true },
        { "normal(manual)", "default",     true },
        { "tree(auto)",     "organic",     false },
        { "tree(auto)",     "default",     false },
        { "tree(auto)",     "tree_slim",   false },
        { "tree(auto)",     "tree_strong", false },
        { "tree(auto)",     "tree_hybrid", false },
        { "tree(manual)",   "tree_slim",   false },
    }));
    CAPTURE(type, style);
    const StringObjectException w = validate_warning({ { "support_type", type }, { "support_style", style },
                                                       { "support_base_pattern", "hollow" } });
    if (warns) {
        CHECK(w.string == kHollow);
        CHECK(w.opt_key == "support_base_pattern");
    } else {
        CHECK(w.string.empty());
    }
}

TEST_CASE("Normal supports print Hollow as Rectilinear", "[SupportPatterns]")
{
    // What the #12710 warning says: the base of a normal support is the same with either pattern.
    auto slice_normal = [](const char *pattern) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({ { "enable_support", "1" }, { "support_type", "normal(auto)" }, { "layer_height", "0.2" },
                                        { "support_on_build_plate_only", "1" }, { "support_base_pattern", pattern } });
        config.set_key_value("gcode_comments", new ConfigOptionBool(true));
        return toolpaths_only(Slic3r::Test::slice({ TestMesh::overhang }, config));
    };
    const std::string rectilinear = slice_normal("rectilinear");
    REQUIRE(support_base_length(rectilinear) > 0.);
    CHECK(slice_normal("hollow") == rectilinear);
}

TEST_CASE("Default base patterns raise no support warning", "[SupportPatterns][Validate]")
{
    // The profile defaults of the X1C / H2D (normal, default pattern) and the U1 (tree, default style
    // and pattern) must stay silent.
    CHECK(validate_warning({ { "support_type", "normal(auto)" }, { "support_base_pattern", "default" } }).string.empty());
    CHECK(validate_warning({ { "support_type", "tree(auto)" }, { "support_style", "default" }, { "support_base_pattern", "default" } }).string.empty());
    CHECK(validate_warning({ { "support_type", "normal(auto)" }, { "support_base_pattern", "rectilinear" } }).string.empty());
}

TEST_CASE("Organic supports fill their branches with a base pattern other than Hollow or Default", "[SupportPatterns][TreeSupport]")
{
    const std::string hollow = slice_organic({ { "support_base_pattern", "hollow" } });
    const double      hollow_len = support_base_length(hollow);
    INFO("hollow base: " << hollow_len << " mm");
    REQUIRE(hollow_len > 0.);

    // Default is Hollow for organic trees, toolpath for toolpath.
    CHECK(toolpaths_only(slice_organic({ { "support_base_pattern", "default" } })) == toolpaths_only(hollow));

    // A filled pattern adds infill inside the branches.
    const std::string rectilinear = slice_organic({ { "support_base_pattern", "rectilinear" } });
    const double      rect_len    = support_base_length(rectilinear);
    INFO("rectilinear base: " << rect_len << " mm");
    CHECK(rect_len > 1.2 * hollow_len);
    for (const char *pattern : { "rectilinear-grid", "honeycomb" }) {
        const double len = support_base_length(slice_organic({ { "support_base_pattern", pattern } }));
        INFO(pattern << " base: " << len << " mm");
        CHECK(len > 1.2 * hollow_len);
    }

    // Lightning is not available for organic trees: the base prints as Rectilinear (the warning above).
    CHECK(toolpaths_only(slice_organic({ { "support_base_pattern", "lightning" } })) == toolpaths_only(rectilinear));
}
