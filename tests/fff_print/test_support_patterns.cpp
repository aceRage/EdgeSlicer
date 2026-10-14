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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

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
        // Its accelerations exceed its machine limits, and that warning comes later in validate() and
        // takes the single warning slot. 0 skips the acceleration check.
        { "default_acceleration", "0" },
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

    // A filled pattern adds infill inside the branches (the default 2.5 mm base spacing is sparse and
    // the branches of this small overhang are thin, so about 10 % more base material).
    const std::string rectilinear = slice_organic({ { "support_base_pattern", "rectilinear" } });
    const double      rect_len    = support_base_length(rectilinear);
    INFO("rectilinear base: " << rect_len << " mm");
    CHECK(rect_len > 1.05 * hollow_len);
    for (const char *pattern : { "rectilinear-grid", "honeycomb" }) {
        const std::string gcode = slice_organic({ { "support_base_pattern", pattern } });
        const double      len   = support_base_length(gcode);
        INFO(pattern << " base: " << len << " mm");
        CHECK(len > 1.05 * hollow_len);
        CHECK(toolpaths_only(gcode) != toolpaths_only(rectilinear));
    }

    // Lightning is not available for organic trees: the base prints as Rectilinear (the warning above).
    CHECK(toolpaths_only(slice_organic({ { "support_base_pattern", "lightning" } })) == toolpaths_only(rectilinear));
}

// ------------------------------------------------------------------ interface pattern selection
//
// Batch 3C also carried Orca #10947 (raft / support interface overhaul) and #10739 (Rectilinear
// Interlaced for normal supports). Upstream reverted #10947 the day it merged, in favour of #11812,
// which is in this tree (3B-2) and already routes every interface through
// SupportParameters::support_interface_angle(); #10739's one-line fix is superseded by it. Neither
// is ported. These tests pin what the patterns do end to end, for every support style, so the
// selection cannot regress unnoticed.

namespace {

struct LayerLines
{
    double c = 0., s = 0., len = 0.; // length-weighted sums of cos 2a / sin 2a, total length
    // Axial mean direction in degrees [0, 180).
    double angle() const { double a = 0.5 * std::atan2(s, c) * 180. / M_PI; return a < 0. ? a + 180. : a; }
    // 1 = all lines parallel, about 0 = two perpendicular families of equal length (a grid).
    double coherence() const { return len > 0. ? std::hypot(c, s) / len : 0.; }
};

// Support interface lines (raft interface included) per layer Z.
std::map<double, LayerLines> interface_lines_by_layer(const std::string &gcode)
{
    std::map<double, LayerLines> out;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&out](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self) || line.comment().find("support material interface") == std::string_view::npos)
            return;
        const double dx = line.dist_X(self), dy = line.dist_Y(self);
        const double len = std::hypot(dx, dy);
        if (len < 1e-6)
            return;
        const double a2 = 2. * std::atan2(dy, dx);
        LayerLines &l = out[self.z()];
        l.c += len * std::cos(a2);
        l.s += len * std::sin(a2);
        l.len += len;
    });
    return out;
}

double axial_diff_deg(double a, double b)
{
    const double d = std::fmod(std::fabs(a - b), 180.);
    return std::min(d, 180. - d);
}

// EDGE_TEST_DUMP_GCODE=<dir> keeps the G-code of the interface cases there, for a look in a viewer.
void dump_gcode(const std::string &name, const std::string &gcode)
{
    const char *dir = std::getenv("EDGE_TEST_DUMP_GCODE");
    if (dir == nullptr || *dir == 0)
        return;
    std::string file = name;
    std::replace_if(file.begin(), file.end(), [](char c) { return c == '(' || c == ')' || c == ' '; }, '_');
    boost::nowide::ofstream out((boost::filesystem::path(dir) / (file + ".gcode")).string(), std::ios::binary);
    out << gcode;
}

std::string slice_interface(const char *type, const char *style, const char *pattern, int raft_layers = 0)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",               "1" },
        { "support_type",                 type },
        { "support_style",                style },
        { "layer_height",                 "0.2" },
        { "support_on_build_plate_only",  "1" },
        { "support_interface_top_layers", "4" },
        { "support_interface_pattern",    pattern },
    });
    config.set_key_value("raft_layers", new ConfigOptionInt(raft_layers));
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));
    std::string gcode = Slic3r::Test::slice({ TestMesh::overhang }, config);
    dump_gcode(std::string("3c_") + type + "_" + style + "_" + pattern + "_raft" + std::to_string(raft_layers), gcode);
    return gcode;
}

// Distinct Z of every extruding move, lowest first.
std::vector<double> extrusion_layers(const std::string &gcode)
{
    std::set<double> zs;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&zs](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && std::hypot(line.dist_X(self), line.dist_Y(self)) > 0.)
            zs.insert(self.z());
    });
    return std::vector<double>(zs.begin(), zs.end());
}

bool is_classic_tree(const char *style)
{
    return std::string(style) == "tree_slim" || std::string(style) == "tree_strong" || std::string(style) == "tree_hybrid";
}

} // namespace

// The classic trees print each roof with a perimeter around it, and their lowest roof layer (the
// first one above the branches) with its own filler; on this small overhang the perimeters are a
// large share of the interface length, so these checks look at line directions with a loose
// parallelism bound rather than at the exact share of parallel lines.
TEST_CASE("Each support interface pattern reaches the interface fill of every support style", "[SupportPatterns][SupportInterface]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "grid" },        { "normal(auto)", "snug" },
        { "tree(auto)",   "organic" },     { "tree(auto)",   "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)",   "tree_hybrid" },
    }));
    CAPTURE(type, style);

    SECTION("rectilinear: parallel lines, the same direction on every interface layer") {
        const auto layers = interface_lines_by_layer(slice_interface(type, style, "rectilinear"));
        REQUIRE(layers.size() >= 3);
        const double first = layers.begin()->second.angle();
        for (const auto &[z, l] : layers) {
            CAPTURE(z, l.angle(), l.coherence());
            CHECK(l.coherence() > 0.35);
            CHECK(axial_diff_deg(l.angle(), first) < 15.);
        }
    }
    SECTION("rectilinear interlaced: every interface layer crosses the one below") {
        const auto layers = interface_lines_by_layer(slice_interface(type, style, "rectilinear_interlaced"));
        REQUIRE(layers.size() >= 3);
        const LayerLines *prev = nullptr;
        for (const auto &[z, l] : layers) {
            CAPTURE(z, l.angle(), l.coherence());
            CHECK(l.coherence() > 0.35);
            if (prev != nullptr)
                CHECK(axial_diff_deg(l.angle(), prev->angle()) > 60.);
            prev = &l;
        }
    }
    SECTION("grid: crossing lines within a layer") {
        const auto layers = interface_lines_by_layer(slice_interface(type, style, "grid"));
        REQUIRE(layers.size() >= 3);
        // The classic trees print their lowest roof layer with parallel lines; every other interface
        // layer is a grid.
        size_t gridded = 0;
        for (const auto &[z, l] : layers) {
            CAPTURE(z, l.angle(), l.coherence());
            gridded += l.coherence() < 0.3;
        }
        CHECK(gridded + (is_classic_tree(style) ? 1 : 0) >= layers.size());
    }
}

TEST_CASE("Raft interface layers cross each other under normal supports", "[SupportPatterns][SupportInterface][Raft]")
{
    // generate_support_toolpaths() alternates the raft interface by +/-45 degrees per interface id
    // (SupportParameters::raft_interface_angle()) whatever the support interface pattern. Organic trees
    // share that code; on the X1C / U1 / H2D profiles their raft interface alternates the same way (the
    // Batch 3C CLI gate), but with this fixture (generic config, the overhang mesh) the organic raft
    // carries no raft body under the object at all, a pre-existing quirk outside this batch, so
    // organic is not covered here. The classic tree styles print their raft interface in
    // TreeSupport::generate_toolpaths(), every layer at 90 degrees, as upstream does (#10947 changed
    // that and was reverted).
    // (With Grid the top raft layer, the raft contact, is a grid itself.)
    const char *pattern = GENERATE("rectilinear", "rectilinear_interlaced");
    CAPTURE(pattern);
    const std::string gcode  = slice_interface("normal(auto)", "grid", pattern, 4);
    const auto        layers = interface_lines_by_layer(gcode);
    // raft_layers 4: two base layers, then two raft interface layers right below the object.
    const std::vector<double> zs = extrusion_layers(gcode);
    REQUIRE(zs.size() > 4);
    const auto l2 = layers.find(zs[2]);
    const auto l3 = layers.find(zs[3]);
    CAPTURE(zs[2], zs[3]);
    REQUIRE(l2 != layers.end());
    REQUIRE(l3 != layers.end());
    CAPTURE(l2->second.angle(), l3->second.angle());
    CHECK(l2->second.coherence() > 0.7);
    CHECK(l3->second.coherence() > 0.7);
    CHECK(axial_diff_deg(l2->second.angle(), l3->second.angle()) > 80.);
}
