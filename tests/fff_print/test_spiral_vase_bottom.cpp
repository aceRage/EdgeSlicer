// The solid bottom of a spiral vase: bottom_shell_layers layers, and as many more as it takes to be
// bottom_shell_thickness thick, as for any other print. The spiral begins on the layer after.

#include <catch2/catch.hpp>

#include <cstdlib>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

struct Vase
{
    // The tops of the layers, in mm.
    std::vector<double> print_zs;
    // Whether each layer has anything filled in it (a vase has no fill but in its bottom).
    std::vector<bool>   filled;
    // Whether each layer is laid as a spiral: its extrusions rise as they go.
    std::vector<bool>   spiralled;
    // The exported G-code.
    std::string         gcode;

    size_t layers_filled() const
    {
        size_t n = 0;
        for (bool f : filled)
            n += f ? 1 : 0;
        return n;
    }
    // The first layer laid as a spiral.
    size_t first_spiral_layer() const
    {
        for (size_t i = 0; i < spiralled.size(); ++i)
            if (spiralled[i])
                return i;
        return spiralled.size();
    }
};

// The 20 mm cube as a vase: a first layer of 0.25 mm, then 0.2 mm, three bottom layers.
Vase vase(const char *bottom_shell_thickness, const char *bottom_shell_layers = "3")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"spiral_mode", "1"},
        {"wall_loops", "1"},
        {"top_shell_layers", "0"},
        {"sparse_infill_density", "0"},
        {"bottom_shell_layers", bottom_shell_layers},
        {"bottom_shell_thickness", bottom_shell_thickness},
        {"initial_layer_print_height", "0.25"},
        {"layer_height", "0.2"},
        {"nozzle_diameter", "0.4"},
        {"skirt_loops", "0"},
        {"brim_type", "no_brim"},
        {"enable_support", "0"},
    });
    Print print;
    Model model;
    init_print({TestMesh::cube_20x20x20}, print, model, config);

    Vase out;
    out.gcode = Test::gcode(print);
    for (const Layer *layer : print.objects().front()->layers()) {
        out.print_zs.push_back(layer->print_z);
        bool filled = false;
        for (const LayerRegion *region : layer->regions())
            filled = filled || region->fills.items_count() > 0;
        out.filled.push_back(filled);
    }

    // A layer of the file is laid as a spiral when its extruding moves carry a Z of their own
    // ("G1 Z0.898 X-9.775 Y-9.775 E.63179").
    static const std::regex rising("^G1 Z[-0-9.]+ X[-0-9.]+ Y[-0-9.]+ E[-0-9.]+");
    std::istringstream      in(out.gcode);
    std::string             line;
    bool                    in_layer = false;
    while (std::getline(in, line)) {
        if (line.rfind(";LAYER_CHANGE", 0) == 0) {
            out.spiralled.push_back(false);
            in_layer = true;
        } else if (in_layer && std::regex_search(line, rising))
            out.spiralled.back() = true;
    }
    // The file has one layer more than the object: the flat ring the spiral is finished with, at the top.
    if (out.spiralled.size() > out.print_zs.size())
        out.spiralled.resize(out.print_zs.size());
    return out;
}

} // namespace

TEST_CASE("A spiral vase's bottom is as thick as bottom_shell_thickness asks", "[SpiralVase]")
{
    // The tops of the layers are at 0.25, 0.45, 0.65, 0.85 and 1.05 mm. Three layers are 0.65 mm; to be 0.9 mm
    // thick, or 1 mm, the bottom takes five (1.05 mm): four are 0.85 mm.
    struct Case { const char *thickness; size_t bottom_layers; };
    const Case c = GENERATE(Case{"0", 3}, Case{"0.6", 3}, Case{"0.65", 3}, Case{"0.7", 4}, Case{"0.9", 5}, Case{"1.0", 5}, Case{"1.05", 5}, Case{"1.1", 6});
    DYNAMIC_SECTION("bottom_shell_thickness " << c.thickness)
    {
        const Vase v = vase(c.thickness);
        REQUIRE(v.print_zs.size() == 100);
        REQUIRE(v.spiralled.size() == v.print_zs.size());
        CHECK_THAT(v.print_zs[2], WithinAbs(0.65, 1e-6));
        CHECK_THAT(v.print_zs[4], WithinAbs(1.05, 1e-6));

        // The bottom is filled, and nothing above it.
        CHECK(v.layers_filled() == c.bottom_layers);
        for (size_t i = 0; i < v.filled.size(); ++i) {
            CAPTURE(i);
            CHECK(v.filled[i] == (i < c.bottom_layers));
        }
        // The spiral begins on the layer after the bottom and goes on to the top.
        CHECK(v.first_spiral_layer() == c.bottom_layers);
        for (size_t i = c.bottom_layers; i < v.spiralled.size(); ++i) {
            CAPTURE(i);
            CHECK(v.spiralled[i]);
        }
        // The bottom is at least as thick as asked.
        CHECK(v.print_zs[c.bottom_layers - 1] > std::atof(c.thickness) - EPSILON);
    }
}

TEST_CASE("A spiral vase with no bottom layers has no bottom whatever the thickness", "[SpiralVase]")
{
    // As for any other print, the thickness adds to a bottom and does not make one.
    const Vase without = vase("0", "0");
    const Vase with    = vase("0.9", "0");
    CHECK(without.layers_filled() == 0);
    CHECK(with.layers_filled() == 0);
    CHECK(with.first_spiral_layer() == without.first_spiral_layer());
    CHECK(with.spiralled == without.spiralled);
}
