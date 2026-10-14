#include <catch2/catch.hpp>

#include <numeric>
#include <sstream>

#include "test_data.hpp" // get access to init_print, etc

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r::Test;
using namespace Slic3r;

SCENARIO("Extrusion width specifics", "[Flow]") {
    GIVEN("A config with a skirt, brim, some fill density, 3 perimeters, and 1 bottom solid layer and a 20mm cube mesh") {
        // this is a sharedptr
        DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
		config.set_deserialize_strict({
			{ "brim_width",			2 },
			{ "skirt_loops",				1 },
			{ "wall_loops",			3 },
			{ "sparse_infill_density",		"40%" },
			{ "initial_layer_print_height", 0.3 }
			});

        WHEN("first layer width set to 2mm") {
            Slic3r::Model model;
            config.set("initial_layer_line_width", 2);
            Slic3r::Print print;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, config);

            std::vector<double> E_per_mm_bottom;
            std::string gcode = Test::gcode(print);
            Slic3r::GCodeReader parser;
            // The FIRST layer is printed at initial_layer_print_height (set to 0.3 above),
            // not at layer_height (0.2 by default), so filtering on layer_height matched no
            // extrusion at all and E_per_mm_bottom came back empty.
            const double layer_height = config.opt_float("initial_layer_print_height");
            parser.parse_buffer(gcode, [&E_per_mm_bottom, layer_height] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line)
            { 
                if (self.z() == Approx(layer_height).margin(0.01)) { // only consider first layer
                    if (line.extruding(self) && line.dist_XY(self) > 0) {
                        E_per_mm_bottom.emplace_back(line.dist_E(self) / line.dist_XY(self));
                    }
                }
            });
            THEN(" First layer width applies to everything on first layer.") {
                bool pass = false;
                double avg_E = std::accumulate(E_per_mm_bottom.cbegin(), E_per_mm_bottom.cend(), 0.0) / static_cast<double>(E_per_mm_bottom.size());

                pass = (std::count_if(E_per_mm_bottom.cbegin(), E_per_mm_bottom.cend(), [avg_E] (const double& v) { return v == Approx(avg_E); }) == 0);
                REQUIRE(pass == true);
                REQUIRE(E_per_mm_bottom.size() > 0); // make sure it actually passed because of extrusion
            }
            THEN(" First layer width does not apply to upper layer.") {
            }
        }
    }
}
// needs gcode export
SCENARIO(" Bridge flow specifics.", "[Flow]") {
    GIVEN("A default config with no cooling and a fixed bridge speed, flow ratio and an overhang mesh.") {
        WHEN("bridge_flow_ratio is set to 1.0") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 0.5") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 2.0") {
            THEN("Output flow is as expected.") {
            }
        }
    }
    GIVEN("A default config with no cooling and a fixed bridge speed, flow ratio, fixed extrusion width of 0.4mm and an overhang mesh.") {
        WHEN("bridge_flow_ratio is set to 1.0") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 0.5") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 2.0") {
            THEN("Output flow is as expected.") {
            }
        }
    }
}

/// Test the expected behavior for auto-width, 
/// spacing, etc
SCENARIO("Flow: Flow math for non-bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
        ConfigOptionFloatOrPercent	width(1.0, false);
        float nozzle_diameter	= 0.4f;
        float layer_height		= 0.4f;

        // Spacing for non-bridges is has some overlap
        THEN("External perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Approx(1.125 * nozzle_diameter - layer_height * (1.0 - PI / 4.0)));
        }

        THEN("Internal perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Approx(1.125 *nozzle_diameter - layer_height * (1.0 - PI / 4.0)));
        }
        THEN("Spacing for supplied width is 0.8927f") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Approx(width.value - layer_height * (1.0 - PI / 4.0)));
            flow = Flow::new_from_config_width(frPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Approx(width.value - layer_height * (1.0 - PI / 4.0)));
        }
    }
    /// Check the min/max
    GIVEN("Nozzle Diameter of 0.25") {
        float nozzle_diameter	= 0.25f;
        float layer_height		= 0.5f;
        WHEN("layer height is set to 0.2") {
            layer_height = 0.15f;
            THEN("Max width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE(flow.width() == Approx(1.125 * nozzle_diameter));
            }
        }
        WHEN("Layer height is set to 0.25") {
            layer_height = 0.25f;
            THEN("Min width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE(flow.width() == Approx(1.125 * nozzle_diameter));
            }
        }
    }

#if 0
    /// Check for an edge case in the maths where the spacing could be 0; original
    /// math is 0.99. Slic3r issue #4654
    GIVEN("Input spacing of 0.414159 and a total width of 2") {
        double in_spacing = 0.414159;
        double total_width = 2.0;
        auto flow = Flow::new_from_spacing(1.0, 0.4, 0.3);
        WHEN("solid_spacing() is called") {
            double result = flow.solid_spacing(total_width, in_spacing);
            THEN("Yielded spacing is greater than 0") {
                REQUIRE(result > 0);
            }
        }
    }
#endif    

}

/// Spacing, width calculation for bridge extrusions
SCENARIO("Flow: Flow math for bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
		float nozzle_diameter	= 0.4f;
		float bridge_flow		= 1.0f;
        WHEN("Flow role is frExternalPerimeter") {
            auto flow = Flow::bridging_flow(nozzle_diameter * sqrt(bridge_flow), nozzle_diameter);
            THEN("Bridge width is same as nozzle diameter") {
                REQUIRE(flow.width() == Approx(nozzle_diameter));
            }
            THEN("Bridge spacing is same as nozzle diameter + BRIDGE_EXTRA_SPACING") {
                REQUIRE(flow.spacing() == Approx(nozzle_diameter + BRIDGE_EXTRA_SPACING));
            }
        }
    }
}

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/SmallAreaInfillFlowCompensator.hpp"

// Orca #11716: the small-area infill flow model is interpolated with PCHIP, which stays monotone between the
// model points where the old cubic spline could overshoot, and a model whose factors do not strictly increase is
// rejected instead of being silently ignored.
TEST_CASE("Small-area flow compensation follows its model without overshoot", "[Flow][SmallAreaFlow][Orca11716]")
{
    using Catch::Matchers::WithinAbs;
    GCodeConfig config;
    config.small_area_infill_flow_compensation_model.values = {
        "0,0", "0.2,0.4444", "0.4,0.6145", "0.6,0.7059", "0.8,0.7619", "1.5,0.8571", "2,0.8889", "3,0.9231", "5,0.9520", "10,1" };
    SmallAreaInfillFlowCompensator compensator(config);

    // The model points themselves.
    CHECK_THAT(compensator.modify_flow(0.4, 1., erSolidInfill), WithinAbs(0.6145, 1e-6));
    CHECK_THAT(compensator.modify_flow(3., 1., erTopSolidInfill), WithinAbs(0.9231, 1e-6));
    // Monotone and inside [0, 1] all the way, with no overshoot past the next point.
    double previous = 0.;
    for (int i = 1; i < 1000; ++i) {
        const double length = 0.01 * i;
        const double factor = compensator.modify_flow(length, 1., erSolidInfill);
        INFO("length " << length);
        REQUIRE(factor >= previous);
        REQUIRE(factor <= 1.);
        previous = factor;
    }
    // Lines past the last point and other roles keep their flow.
    CHECK(compensator.modify_flow(20., 1., erSolidInfill) == 1.);
    CHECK(compensator.modify_flow(1., 1., erInternalInfill) == 1.);

    config.small_area_infill_flow_compensation_model.values = { "0,0", "1,0.5", "2,0.5", "10,1" };
    CHECK_THROWS_AS(SmallAreaInfillFlowCompensator(config), Slic3r::InvalidArgument);
}

// Orca a2a9f95b6b: the G-code export decided whether to build the small-area compensator from m_config before
// any object or region config had been applied to it, so small_area_infill_flow_compensation (a region option)
// always read its hard-coded default (off). Turning it on in the process preset changed nothing.
namespace {
struct SafcExtrusion
{
    double      solid_infill = 0.; // top, bottom and internal solid infill: what the compensator may reduce
    double      other        = 0.; // everything else: walls, sparse infill, skirt
    std::string moves;             // every non-comment line, to compare two slices move for move
};

SafcExtrusion safc_extrusion(const std::string& gcode, const DynamicPrintConfig& config)
{
    SafcExtrusion out;
    bool          solid = false;
    GCodeReader   parser;
    parser.apply_config(config);
    parser.parse_buffer(gcode, [&out, &solid](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string_view comment = line.comment();
        if (comment.rfind("TYPE:", 0) == 0)
            solid = comment == "TYPE:Top surface" || comment == "TYPE:Bottom surface" || comment == "TYPE:Internal solid infill";
        if (!line.raw().empty() && line.raw().front() != ';')
            out.moves += line.raw() + "\n";
        if (line.extruding(self))
            (solid ? out.solid_infill : out.other) += line.dist_E(self);
    });
    return out;
}
} // namespace

TEST_CASE("Small-area flow compensation applies when the process enables it", "[Flow][SmallAreaFlow][OrcaA2a9f95b6b]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    // The compensator only touches the line-based solid patterns.
    config.set_deserialize_strict({
        {"top_surface_pattern", "monotonic"},
        {"bottom_surface_pattern", "monotonic"},
        {"internal_solid_infill_pattern", "monotonic"},
        {"sparse_infill_density", "15%"},
    });
    // A small block: its solid infill lines are all shorter than the 10 mm end of the default model.
    const TriangleMesh block = make_cube(8., 8., 3.);

    config.set_deserialize_strict({{"small_area_infill_flow_compensation", "0"}});
    const SafcExtrusion off = safc_extrusion(Test::slice({block}, config), config);

    // Off: the model is not used at all, so a different model slices move for move the same.
    DynamicPrintConfig other_model = config;
    other_model.option<ConfigOptionStrings>("small_area_infill_flow_compensation_model")->values = {"0,0", "5,0.5", "20,1"};
    const SafcExtrusion off_other_model = safc_extrusion(Test::slice({block}, other_model), other_model);
    CHECK(off_other_model.moves == off.moves);

    config.set_deserialize_strict({{"small_area_infill_flow_compensation", "1"}});
    const SafcExtrusion on = safc_extrusion(Test::slice({block}, config), config);

    REQUIRE(off.solid_infill > 0.);
    INFO("solid infill E off " << off.solid_infill << ", on " << on.solid_infill);
    // On: the short solid infill lines are under-extruded relative to off ...
    CHECK(on.solid_infill < off.solid_infill * 0.99);
    // ... and nothing else moves.
    CHECK(on.other == Approx(off.other).epsilon(1e-9));
}
