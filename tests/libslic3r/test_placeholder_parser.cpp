#include <catch2/catch.hpp>

#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

SCENARIO("Placeholder parser scripting", "[PlaceholderParser]") {
	PlaceholderParser 	parser;
	auto 				config = DynamicPrintConfig::full_print_config();

	config.set_deserialize_strict( {
		{ "printer_notes", "  PRINTER_VENDOR_PRUSA3D  PRINTER_MODEL_MK2  " },
	    { "nozzle_diameter", "0.6,0.6,0.6,0.6" },
	    { "nozzle_temperature", "357,359,363,378" },
	    // FloatOrPercent options exercising the "ratio_over" resolution paths below:
	    // Since the High-Flow retype, outer_wall_speed / bridge_speed / travel_speed are per-extruder
	    // coFloats and small_perimeter_speed / internal_bridge_speed / overhang_N_4_speed are
	    // coFloatsOrPercents; a bare reference resolves to the current extruder's element.
	    { "outer_wall_speed", "60" },            // parent of small_perimeter_speed
	    { "small_perimeter_speed", "50%" },      // percent over outer_wall_speed (coFloats)
	    { "bridge_speed", "50" },                // parent of internal_bridge_speed
	    { "internal_bridge_speed", "150%" },     // percent over bridge_speed (coFloats)
	    { "overhang_1_4_speed", "40" },          // absolute coFloatsOrPercents, returned as-is
	    { "travel_speed", "200" },               // parent of wipe_speed
	    { "wipe_speed", "80%" },                 // scalar percent over a coFloats parent
	    { "sparse_infill_line_width", "0.45" },  // absolute parent of infill_anchor
	    { "infill_anchor", "300%" },             // percent over sparse_infill_line_width (coFloatOrPercent)
	    { "initial_layer_line_width", "0.9" },   // absolute FloatOrPercent, returned as-is
	    // line_width is a percent whose ratio_over parent (nozzle_diameter) is a per-extruder vector,
	    // read at the current extruder.
	    { "line_width", "112.5%" },
	});
    // To test the "min_width_top_surface" over "inner_wall_line_width".
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value = 150.;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = true;
    // To let the PlaceholderParser throw when referencing scarf_joint_speed if it is set to percent, as the PlaceholderParser does not know
    // a percent to what.
    config.option<ConfigOptionFloatOrPercent>("scarf_joint_speed")->value = 50.;
    config.option<ConfigOptionFloatOrPercent>("scarf_joint_speed")->percent = true;

    parser.apply_config(config);
	parser.set("foo", 0);
	parser.set("bar", 2);
	parser.set("num_extruders", 4);

    SECTION("nested config options (legacy syntax)") { REQUIRE(parser.process("[nozzle_temperature_[foo]]") == "357"); }
    SECTION("array reference") { REQUIRE(parser.process("{nozzle_temperature[foo]}") == "357"); }
    SECTION("whitespaces and newlines are maintained") { REQUIRE(parser.process("test [ nozzle_temperature_ [foo] ] \n hu") == "test 357 \n hu"); }
    // OrcaSlicer #11485 spells the legacy indexed form without the trailing underscore.
    SECTION("nested config options (legacy syntax, no underscore)") { REQUIRE(parser.process("[nozzle_temperature[foo]]") == "357"); }
    SECTION("whitespaces and newlines are maintained (no underscore)") { REQUIRE(parser.process("test [ nozzle_temperature [foo] ] \n hu") == "test 357 \n hu"); }

    // Test the math expressions.
    SECTION("math: 2*3") { REQUIRE(parser.process("{2*3}") == "6"); }
    SECTION("math: 2*3/6") { REQUIRE(parser.process("{2*3/6}") == "1"); }
    SECTION("math: 2*3/12") { REQUIRE(parser.process("{2*3/12}") == "0"); }
    SECTION("math: 2.*3/12") { REQUIRE(std::stod(parser.process("{2.*3/12}")) == Approx(0.5)); }
    SECTION("math: 10 % 2.5") { REQUIRE(std::stod(parser.process("{10%2.5}")) == Approx(0.)); }
    SECTION("math: 11 % 2.5") { REQUIRE(std::stod(parser.process("{11%2.5}")) == Approx(1.)); }
    SECTION("math: 2*(3-12)") { REQUIRE(parser.process("{2*(3-12)}") == "-18"); }
    SECTION("math: 2*foo*(3-12)") { REQUIRE(parser.process("{2*foo*(3-12)}") == "0"); }
    SECTION("math: 2*bar*(3-12)") { REQUIRE(parser.process("{2*bar*(3-12)}") == "-36"); }
    SECTION("math: 2.5*bar*(3-12)") { REQUIRE(std::stod(parser.process("{2.5*bar*(3-12)}")) == Approx(-45)); }
    SECTION("math: min(12, 14)") { REQUIRE(parser.process("{min(12, 14)}") == "12"); }
    SECTION("math: max(12, 14)") { REQUIRE(parser.process("{max(12, 14)}") == "14"); }
    SECTION("math: min(13.4, -1238.1)") { REQUIRE(std::stod(parser.process("{min(13.4, -1238.1)}")) == Approx(-1238.1)); }
    SECTION("math: max(13.4, -1238.1)") { REQUIRE(std::stod(parser.process("{max(13.4, -1238.1)}")) == Approx(13.4)); }
    SECTION("math: int(13.4)") { REQUIRE(parser.process("{int(13.4)}") == "13"); }
    SECTION("math: int(-13.4)") { REQUIRE(parser.process("{int(-13.4)}") == "-13"); }
    SECTION("math: round(13.4)") { REQUIRE(parser.process("{round(13.4)}") == "13"); }
    SECTION("math: round(-13.4)") { REQUIRE(parser.process("{round(-13.4)}") == "-13"); }
    SECTION("math: round(13.6)") { REQUIRE(parser.process("{round(13.6)}") == "14"); }
    SECTION("math: round(-13.6)") { REQUIRE(parser.process("{round(-13.6)}") == "-14"); }
    SECTION("math: digits(5, 15)") { REQUIRE(parser.process("{digits(5, 15)}") == "              5"); }
    SECTION("math: digits(5., 15)") { REQUIRE(parser.process("{digits(5., 15)}") == "              5"); }
    SECTION("math: zdigits(5, 15)") { REQUIRE(parser.process("{zdigits(5, 15)}") == "000000000000005"); }
    SECTION("math: zdigits(5., 15)") { REQUIRE(parser.process("{zdigits(5., 15)}") == "000000000000005"); }
    SECTION("math: digits(5, 15, 8)") { REQUIRE(parser.process("{digits(5, 15, 8)}") == "     5.00000000"); }
    SECTION("math: digits(5., 15, 8)") { REQUIRE(parser.process("{digits(5, 15, 8)}") == "     5.00000000"); }
    SECTION("math: zdigits(5, 15, 8)") { REQUIRE(parser.process("{zdigits(5, 15, 8)}") == "000005.00000000"); }
    SECTION("math: zdigits(5., 15, 8)") { REQUIRE(parser.process("{zdigits(5, 15, 8)}") == "000005.00000000"); }
    SECTION("math: digits(13.84375892476, 15, 8)") { REQUIRE(parser.process("{digits(13.84375892476, 15, 8)}") == "    13.84375892"); }
    SECTION("math: zdigits(13.84375892476, 15, 8)") { REQUIRE(parser.process("{zdigits(13.84375892476, 15, 8)}") == "000013.84375892"); }
    SECTION("math: interpolate_table(13.84375892476, (0, 0), (20, 20))") { REQUIRE(std::stod(parser.process("{interpolate_table(13.84375892476, (0, 0), (20, 20))}")) == Approx(13.84375892476)); }
    SECTION("math: interpolate_table(13, (0, 0), (20, 20), (30, 20))") { REQUIRE(std::stod(parser.process("{interpolate_table(13, (0, 0), (20, 20), (30, 20))}")) == Approx(13.)); }
    SECTION("math: interpolate_table(25, (0, 0), (20, 20), (30, 20))") { REQUIRE(std::stod(parser.process("{interpolate_table(25, (0, 0), (20, 20), (30, 20))}")) == Approx(20.)); }

    // regex_replace(subject, /pattern/, replacement): the string-transform primitive (OrcaSlicer #14650).
    SECTION("regex_replace: strips a file extension") { REQUIRE(parser.process("{regex_replace(\"part.stl\", /\\.[^.]*$/, \"\")}") == "part"); }
    SECTION("regex_replace: leaves a non-matching dot untouched") { REQUIRE(parser.process("{regex_replace(\"Bracket v2.1\", /\\.stl$/, \"\")}") == "Bracket v2.1"); }
    SECTION("regex_replace: replaces every match") { REQUIRE(parser.process("{regex_replace(\"a-b-c\", /-/, \"_\")}") == "a_b_c"); }
    SECTION("regex_replace: replacement may reference a capture group") { REQUIRE(parser.process("{regex_replace(\"v12\", /v(\\d+)/, \"$1\")}") == "12"); }
    // The result is an ordinary string, usable in further expressions (the real filename-template shape).
    SECTION("regex_replace: result composes with concatenation") { REQUIRE(parser.process("{regex_replace(\"part.stl\", /\\.[^.]*$/, \"\") + \".gcode\"}") == "part.gcode"); }
    // A malformed pattern and a non-string subject are both hard errors.
    SECTION("regex_replace: an invalid pattern throws") { REQUIRE_THROWS(parser.process("{regex_replace(\"x\", /[/, \"\")}")); }
    SECTION("regex_replace: a non-string subject throws") { REQUIRE_THROWS(parser.process("{regex_replace(123, /2/, \"\")}")); }
    // Inside a skipped branch the subject is TYPE_EMPTY and the call must no-op (exercises the guard).
    SECTION("regex_replace: is skipped inside a false if-branch") { REQUIRE(parser.process("{if false}{regex_replace(\"x\", /x/, \"y\")}{endif}done") == "done"); }
    // Edge: a string config value as the subject (printer_notes is set above).
    SECTION("regex_replace: on a config string") { REQUIRE(parser.process("{regex_replace(printer_notes, /^\\s+|\\s+$/, \"\")}") == "PRINTER_VENDOR_PRUSA3D  PRINTER_MODEL_MK2"); }

    // Test the "coFloatOrPercent" substitutions via the "ratio_over" chains.
    SECTION("initial_layer_line_width, absolute") { REQUIRE(std::stod(parser.process("{initial_layer_line_width}")) == Approx(0.9)); }
    // small_perimeter_speed 50% over outer_wall_speed (coFloats) 60
    SECTION("small_perimeter_speed") { REQUIRE(std::stod(parser.process("{small_perimeter_speed}")) == Approx(30.)); }
    SECTION("small_perimeter_speed, indexed") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[0]}")) == Approx(30.)); }
    SECTION("small_perimeter_speed in a condition") { REQUIRE(parser.process("{if small_perimeter_speed > 20}fast{else}slow{endif}") == "fast"); }
    // internal_bridge_speed 150% over bridge_speed (coFloats) 50
    SECTION("internal_bridge_speed") { REQUIRE(std::stod(parser.process("{internal_bridge_speed}")) == Approx(75.)); }
    SECTION("overhang_1_4_speed, absolute") { REQUIRE(std::stod(parser.process("{overhang_1_4_speed}")) == Approx(40.)); }
    // wipe_speed (scalar coFloatOrPercent) 80% over travel_speed (coFloats) 200
    SECTION("wipe_speed") { REQUIRE(std::stod(parser.process("{wipe_speed}")) == Approx(160.)); }
    // infill_anchor 300% over sparse_infill_line_width (absolute coFloatOrPercent) 0.45
    SECTION("infill_anchor") { REQUIRE(std::stod(parser.process("{infill_anchor}")) == Approx(1.35)); }
    // line_width 112.5% over nozzle_diameter (coFloats) 0.6 at the current extruder
    SECTION("line_width") { REQUIRE(std::stod(parser.process("{line_width}")) == Approx(0.675)); }
    // OrcaSlicer #11485: "*line_width" options (and "ratio_over" parents) resolve through Flow::extrusion_width.
    // min_width_top_surface 300% over inner_wall_line_width 150% of nozzle_diameter 0.6 = 0.9.
    SECTION("min_width_top_surface") { REQUIRE(std::stod(parser.process("{min_width_top_surface}")) == Approx(2.7)); }
    SECTION("inner_wall_line_width") { REQUIRE(std::stod(parser.process("{inner_wall_line_width}")) == Approx(0.9)); }
    // If scarf_joint_speed is set to percent, then it is applied over respective extrusion types by overriding their respective speeds.
    // The PlaceholderParser has no way to know which extrusion type the caller has in mind, therefore it throws.
    SECTION("scarf_joint_speed") { REQUIRE_THROWS(parser.process("{scarf_joint_speed}")); }

    // Test the boolean expression parser.
    auto boolean_expression = [&parser](const std::string& templ) { return parser.evaluate_boolean_expression(templ, parser.config()); };

    SECTION("boolean expression parser: 12 == 12") { REQUIRE(boolean_expression("12 == 12")); }
    SECTION("boolean expression parser: 12 != 12") { REQUIRE(! boolean_expression("12 != 12")); }
    SECTION("boolean expression parser: regex matches") { REQUIRE(boolean_expression("\"has some PATTERN embedded\" =~ /.*PATTERN.*/")); }
    SECTION("boolean expression parser: regex does not match") { REQUIRE(! boolean_expression("\"has some PATTERN embedded\" =~ /.*PTRN.*/")); }
    SECTION("boolean expression parser: accessing variables, equal") { REQUIRE(boolean_expression("foo + 2 == bar")); }
    SECTION("boolean expression parser: accessing variables, not equal") { REQUIRE(! boolean_expression("foo + 3 == bar")); }
    SECTION("boolean expression parser: (12 == 12) and (13 != 14)") { REQUIRE(boolean_expression("(12 == 12) and (13 != 14)")); }
    SECTION("boolean expression parser: (12 == 12) && (13 != 14)") { REQUIRE(boolean_expression("(12 == 12) && (13 != 14)")); }
    SECTION("boolean expression parser: (12 == 12) or (13 == 14)") { REQUIRE(boolean_expression("(12 == 12) or (13 == 14)")); }
    SECTION("boolean expression parser: (12 == 12) || (13 == 14)") { REQUIRE(boolean_expression("(12 == 12) || (13 == 14)")); }
    SECTION("boolean expression parser: (12 == 12) and not (13 == 14)") { REQUIRE(boolean_expression("(12 == 12) and not (13 == 14)")); }
    SECTION("boolean expression parser: ternary true") { REQUIRE(boolean_expression("(12 == 12) ? (1 - 1 == 0) : (2 * 2 == 3)")); }
    SECTION("boolean expression parser: ternary false") { REQUIRE(! boolean_expression("(12 == 21/2) ? (1 - 1 == 0) : (2 * 2 == 3)")); }
    SECTION("boolean expression parser: ternary false 2") { REQUIRE(boolean_expression("(12 == 13) ? (1 - 1 == 3) : (2 * 2 == 4)")); }
    SECTION("boolean expression parser: ternary true 2") { REQUIRE(! boolean_expression("(12 == 2 * 6) ? (1 - 1 == 3) : (2 * 2 == 4)")); }
    SECTION("boolean expression parser: lower than - false") { REQUIRE(! boolean_expression("12 < 3")); }
    SECTION("boolean expression parser: lower than - true") { REQUIRE(boolean_expression("12 < 22")); }
    SECTION("boolean expression parser: greater than - true") { REQUIRE(boolean_expression("12 > 3")); }
    SECTION("boolean expression parser: greater than - false") { REQUIRE(! boolean_expression("12 > 22")); }
    SECTION("boolean expression parser: lower than or equal- false") { REQUIRE(! boolean_expression("12 <= 3")); }
    SECTION("boolean expression parser: lower than or equal - true") { REQUIRE(boolean_expression("12 <= 22")); }
    SECTION("boolean expression parser: greater than or equal - true") { REQUIRE(boolean_expression("12 >= 3")); }
    SECTION("boolean expression parser: greater than or equal - false") { REQUIRE(! boolean_expression("12 >= 22")); }
    SECTION("boolean expression parser: lower than or equal (same values) - true") { REQUIRE(boolean_expression("12 <= 12")); }
    SECTION("boolean expression parser: greater than or equal (same values) - true") { REQUIRE(boolean_expression("12 >= 12")); }
    SECTION("complex expression") { REQUIRE(boolean_expression("printer_notes=~/.*PRINTER_VENDOR_PRUSA3D.*/ and printer_notes=~/.*PRINTER_MODEL_MK2.*/ and nozzle_diameter[0]==0.6 and num_extruders>1")); }
    SECTION("complex expression2") { REQUIRE(boolean_expression("printer_notes=~/.*PRINTER_VEwerfNDOR_PRUSA3D.*/ or printer_notes=~/.*PRINTertER_MODEL_MK2.*/ or (nozzle_diameter[0]==0.6 and num_extruders>1)")); }
    SECTION("complex expression3") { REQUIRE(! boolean_expression("printer_notes=~/.*PRINTER_VEwerfNDOR_PRUSA3D.*/ or printer_notes=~/.*PRINTertER_MODEL_MK2.*/ or (nozzle_diameter[0]==0.3 and num_extruders>1)")); }
}

TEST_CASE("curr_bed_type placeholder resolves to the enum key", "[placeholder][curr_bed_type]") {
    OtherSlicingStatesConfigDef other_slicing_states;
    REQUIRE(other_slicing_states.has("curr_bed_type"));

    PlaceholderParser parser;
    auto              config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"curr_bed_type", "Textured PEI Plate"}});
    parser.apply_config(config);

    REQUIRE(parser.process("[curr_bed_type]") == "Textured PEI Plate");
    REQUIRE(parser.process("{if curr_bed_type==\"High Temp Plate\"}A{else}B{endif}") == "B");
}

// OrcaSlicer #11485: local / global variables, including vectors copied from a config vector.
SCENARIO("Placeholder parser variables", "[PlaceholderParser]") {
    PlaceholderParser 	parser;
    auto 				config = DynamicPrintConfig::full_print_config();

    config.set_deserialize_strict({
        { "filament_notes", "testnotes" },
        { "enable_pressure_advance", "1" },
        { "nozzle_diameter", "0.6,0.6,0.6,0.6" },
        { "nozzle_temperature", "357,359,363,378" }
        });

    PlaceholderParser::ContextData context_with_global_dict;
    context_with_global_dict.global_config = std::make_unique<DynamicConfig>();

    SECTION("create an int local variable") { REQUIRE(parser.process("{local myint = 33+2}{myint}", 0, nullptr, nullptr, nullptr) == "35"); }
    SECTION("create a string local variable") { REQUIRE(parser.process("{local mystr = \"mine\" + \"only\" + \"mine\"}{mystr}", 0, nullptr, nullptr, nullptr) == "mineonlymine"); }
    SECTION("regex_replace transforms a string variable") { REQUIRE(parser.process("{local n = \"part.stl\"}{regex_replace(n, /\\.[^.]*$/, \"\")}", 0, nullptr, nullptr, nullptr) == "part"); }
    SECTION("create a bool local variable") { REQUIRE(parser.process("{local mybool = 1 + 1 == 2}{mybool}", 0, nullptr, nullptr, nullptr) == "true"); }
    SECTION("create an int global variable") { REQUIRE(parser.process("{global myint = 33+2}{myint}", 0, nullptr, nullptr, &context_with_global_dict) == "35"); }
    SECTION("create a string global variable") { REQUIRE(parser.process("{global mystr = \"mine\" + \"only\" + \"mine\"}{mystr}", 0, nullptr, nullptr, &context_with_global_dict) == "mineonlymine"); }
    SECTION("create a bool global variable") { REQUIRE(parser.process("{global mybool = 1 + 1 == 2}{mybool}", 0, nullptr, nullptr, &context_with_global_dict) == "true"); }

    SECTION("create an int local variable and overwrite it") { REQUIRE(parser.process("{local myint = 33+2}{myint = 12}{myint}", 0, nullptr, nullptr, nullptr) == "12"); }
    SECTION("create a string local variable and overwrite it") { REQUIRE(parser.process("{local mystr = \"mine\" + \"only\" + \"mine\"}{mystr = \"yours\"}{mystr}", 0, nullptr, nullptr, nullptr) == "yours"); }
    SECTION("create a bool local variable and overwrite it") { REQUIRE(parser.process("{local mybool = 1 + 1 == 2}{mybool = false}{mybool}", 0, nullptr, nullptr, nullptr) == "false"); }
    SECTION("create an int global variable and overwrite it") { REQUIRE(parser.process("{global myint = 33+2}{myint = 12}{myint}", 0, nullptr, nullptr, &context_with_global_dict) == "12"); }
    SECTION("create a string global variable and overwrite it") { REQUIRE(parser.process("{global mystr = \"mine\" + \"only\" + \"mine\"}{mystr = \"yours\"}{mystr}", 0, nullptr, nullptr, &context_with_global_dict) == "yours"); }
    SECTION("create a bool global variable and overwrite it") { REQUIRE(parser.process("{global mybool = 1 + 1 == 2}{mybool = false}{mybool}", 0, nullptr, nullptr, &context_with_global_dict) == "false"); }

    SECTION("create an int local variable and redefine it") { REQUIRE(parser.process("{local myint = 33+2}{local myint = 12}{myint}", 0, nullptr, nullptr, nullptr) == "12"); }
    SECTION("create a string local variable and redefine it") { REQUIRE(parser.process("{local mystr = \"mine\" + \"only\" + \"mine\"}{local mystr = \"yours\"}{mystr}", 0, nullptr, nullptr, nullptr) == "yours"); }
    SECTION("create a bool local variable and redefine it") { REQUIRE(parser.process("{local mybool = 1 + 1 == 2}{local mybool = false}{mybool}", 0, nullptr, nullptr, nullptr) == "false"); }
    SECTION("create an int global variable and redefine it") { REQUIRE(parser.process("{global myint = 33+2}{global myint = 12}{myint}", 0, nullptr, nullptr, &context_with_global_dict) == "12"); }
    SECTION("create a string global variable and redefine it") { REQUIRE(parser.process("{global mystr = \"mine\" + \"only\" + \"mine\"}{global mystr = \"yours\"}{mystr}", 0, nullptr, nullptr, &context_with_global_dict) == "yours"); }
    SECTION("create a bool global variable and redefine it") { REQUIRE(parser.process("{global mybool = 1 + 1 == 2}{global mybool = false}{mybool}", 0, nullptr, nullptr, &context_with_global_dict) == "false"); }

    SECTION("create an ints local variable with repeat()") { REQUIRE(parser.process("{local myint = repeat(2*3, 4*6)}{myint[5]}", 0, nullptr, nullptr, nullptr) == "24"); }
    SECTION("create a strings local variable with repeat()") { REQUIRE(parser.process("{local mystr = repeat(2*3, \"mine\" + \"only\" + \"mine\")}{mystr[5]}", 0, nullptr, nullptr, nullptr) == "mineonlymine"); }
    SECTION("create a bools local variable with repeat()") { REQUIRE(parser.process("{local mybool = repeat(5, 1 + 1 == 2)}{mybool[4]}", 0, nullptr, nullptr, nullptr) == "true"); }
    SECTION("create an ints global variable with repeat()") { REQUIRE(parser.process("{global myint = repeat(2*3, 4*6)}{myint[5]}", 0, nullptr, nullptr, &context_with_global_dict) == "24"); }
    SECTION("create a strings global variable with repeat()") { REQUIRE(parser.process("{global mystr = repeat(2*3, \"mine\" + \"only\" + \"mine\")}{mystr[5]}", 0, nullptr, nullptr, &context_with_global_dict) == "mineonlymine"); }
    SECTION("create a bools global variable with repeat()") { REQUIRE(parser.process("{global mybool = repeat(5, 1 + 1 == 2)}{mybool[4]}", 0, nullptr, nullptr, &context_with_global_dict) == "true"); }

    SECTION("create an ints local variable with initializer list") { REQUIRE(parser.process("{local myint = (2*3, 4*6, 5*5)}{myint[1]}", 0, nullptr, nullptr, nullptr) == "24"); }
    SECTION("create a strings local variable with initializer list") { REQUIRE(parser.process("{local mystr = (2*3, \"mine\" + \"only\" + \"mine\", 8)}{mystr[1]}", 0, nullptr, nullptr, nullptr) == "mineonlymine"); }
    SECTION("create a bools local variable with initializer list") { REQUIRE(parser.process("{local mybool = (3*3 == 8, 1 + 1 == 2)}{mybool[1]}", 0, nullptr, nullptr, nullptr) == "true"); }
    SECTION("create an ints global variable with initializer list") { REQUIRE(parser.process("{global myint = (2*3, 4*6, 5*5)}{myint[1]}", 0, nullptr, nullptr, &context_with_global_dict) == "24"); }
    SECTION("create a strings global variable with initializer list") { REQUIRE(parser.process("{global mystr = (2*3, \"mine\" + \"only\" + \"mine\", 8)}{mystr[1]}", 0, nullptr, nullptr, &context_with_global_dict) == "mineonlymine"); }
    SECTION("create a bools global variable with initializer list") { REQUIRE(parser.process("{global mybool = (2*3 == 8, 1 + 1 == 2, 5*5 != 33)}{mybool[1]}", 0, nullptr, nullptr, &context_with_global_dict) == "true"); }

    SECTION("create an ints local variable by a copy") { REQUIRE(parser.process("{local myint = nozzle_temperature}{myint[0]}", 0, &config, nullptr, nullptr) == "357"); }
    SECTION("create a strings local variable by a copy") { REQUIRE(parser.process("{local mystr = filament_notes}{mystr[0]}", 0, &config, nullptr, nullptr) == "testnotes"); }
    SECTION("create a bools local variable by a copy") { REQUIRE(parser.process("{local mybool = enable_pressure_advance}{mybool[0]}", 0, &config, nullptr, nullptr) == "true"); }
    SECTION("create an ints global variable by a copy") { REQUIRE(parser.process("{global myint = nozzle_temperature}{myint[0]}", 0, &config, nullptr, &context_with_global_dict) == "357"); }
    SECTION("create a strings global variable by a copy") { REQUIRE(parser.process("{global mystr = filament_notes}{mystr[0]}", 0, &config, nullptr, &context_with_global_dict) == "testnotes"); }
    SECTION("create a bools global variable by a copy") { REQUIRE(parser.process("{global mybool = enable_pressure_advance}{mybool[0]}", 0, &config, nullptr, &context_with_global_dict) == "true"); }

    SECTION("create an ints local variable by a copy and overwrite it") {
        REQUIRE(parser.process("{local myint = nozzle_temperature}{myint = repeat(2*3, 4*6)}{myint[5]}", 0, &config, nullptr, nullptr) == "24");
        REQUIRE(parser.process("{local myint = nozzle_temperature}{myint = (2*3, 4*6)}{myint[1]}", 0, &config, nullptr, nullptr) == "24");
        REQUIRE(parser.process("{local myint = nozzle_temperature}{myint = (1)}{myint = nozzle_temperature}{myint[0]}", 0, &config, nullptr, nullptr) == "357");
    }
    SECTION("create a strings local variable by a copy and overwrite it") {
        REQUIRE(parser.process("{local mystr = filament_notes}{mystr = repeat(2*3, \"mine\" + \"only\" + \"mine\")}{mystr[5]}", 0, &config, nullptr, nullptr) == "mineonlymine");
        REQUIRE(parser.process("{local mystr = filament_notes}{mystr = (2*3, \"mine\" + \"only\" + \"mine\")}{mystr[1]}", 0, &config, nullptr, nullptr) == "mineonlymine");
        REQUIRE(parser.process("{local mystr = filament_notes}{mystr = (2*3, \"mine\" + \"only\" + \"mine\")}{mystr = filament_notes}{mystr[0]}", 0, &config, nullptr, nullptr) == "testnotes");
    }
    SECTION("create a bools local variable by a copy and overwrite it") {
        REQUIRE(parser.process("{local mybool = enable_pressure_advance}{mybool = repeat(2*3, true)}{mybool[5]}", 0, &config, nullptr, nullptr) == "true");
        REQUIRE(parser.process("{local mybool = enable_pressure_advance}{mybool = (false, true)}{mybool[1]}", 0, &config, nullptr, nullptr) == "true");
        REQUIRE(parser.process("{local mybool = enable_pressure_advance}{mybool = (false, false)}{mybool = enable_pressure_advance}{mybool[0]}", 0, &config, nullptr, nullptr) == "true");
    }

    // Edge: a bare per-extruder vector in a scalar expression resolves to the current extruder's element
    // (BambuStudio rule). The vector-copy form above must not swallow such an expression.
    SECTION("Edge: a vector in a scalar expression still creates a scalar") {
        REQUIRE(parser.process("{local t = nozzle_temperature + 5}{t}", 0, &config, nullptr, nullptr) == "362");
        REQUIRE(parser.process("{local t = nozzle_temperature + 5}{t}", 2, &config, nullptr, nullptr) == "368");
        REQUIRE(parser.process("{local t = nozzle_temperature * 2;t}", 0, &config, nullptr, nullptr) == "714");
        REQUIRE(parser.process("{global t = nozzle_temperature - 7}{t}", 0, &config, nullptr, &context_with_global_dict) == "350");
        REQUIRE(parser.process("{local t = nozzle_temperature[1]}{t}", 0, &config, nullptr, nullptr) == "359");
    }
    SECTION("Edge: a copied vector read bare gives the current extruder's element") {
        REQUIRE(parser.process("{local t = nozzle_temperature}{t}", 0, &config, nullptr, nullptr) == "357");
        REQUIRE(parser.process("{local t = nozzle_temperature}{t + 1}", 3, &config, nullptr, nullptr) == "379");
        REQUIRE(parser.process("{local t = nozzle_temperature}{size(t)}", 0, &config, nullptr, nullptr) == "4");
    }
    SECTION("Edge: vector copy ends at ; and at if-block keywords") {
        REQUIRE(parser.process("{local t = nozzle_temperature; t[2]}", 0, &config, nullptr, nullptr) == "363");
        REQUIRE(parser.process("{if true then local t = nozzle_temperature else local t = (1) endif}{t[3]}", 0, &config, nullptr, nullptr) == "378");
        REQUIRE(parser.process("{if false then local t = (1) else local t = nozzle_temperature endif}{t[1]}", 0, &config, nullptr, nullptr) == "359");
    }
    SECTION("Edge: skipped branches parse a vector expression") {
        REQUIRE(parser.process("{if false}{local t = nozzle_temperature + 5}{local u = nozzle_temperature}{endif}ok", 0, &config, nullptr, nullptr) == "ok");
    }

    SECTION("size() of a non-empty vector returns the right size") { REQUIRE(parser.process("{local myint = (0, 1, 2, 3)}{size(myint)}", 0, nullptr, nullptr, nullptr) == "4"); }
    SECTION("size() of a an empty vector returns the right size") { REQUIRE(parser.process("{local myint = (0);myint=();size(myint)}", 0, nullptr, nullptr, nullptr) == "0"); }
    SECTION("empty() of a non-empty vector returns false") { REQUIRE(parser.process("{local myint = (0, 1, 2, 3)}{empty(myint)}", 0, nullptr, nullptr, nullptr) == "false"); }
    SECTION("empty() of a an empty vector returns true") { REQUIRE(parser.process("{local myint = (0);myint=();empty(myint)}", 0, nullptr, nullptr, nullptr) == "true"); }

    SECTION("nested if with new variables") {
        std::string script =
            "{if 1 == 1}{local myints = (5, 4, 3, 2, 1)}{else}{local myfloats = (1., 2., 3., 4., 5., 6., 7.)}{endif}"
            "{myints[1]},{size(myints)}";
        REQUIRE(parser.process(script, 0, nullptr, nullptr, nullptr) == "4,5");
    }
    SECTION("nested if with new variables 2") {
        std::string script =
            "{if 1 == 0}{local myints = (5, 4, 3, 2, 1)}{else}{local myfloats = (1., 2., 3., 4., 5., 6., 7.)}{endif}"
            "{size(myfloats)}";
        REQUIRE(parser.process(script, 0, nullptr, nullptr, nullptr) == "7");
    }
    SECTION("nested if with new variables 2, mixing }{ with ;") {
        std::string script =
            "{if 1 == 0 then local myints = (5, 4, 3, 2, 1);else;local myfloats = (1., 2., 3., 4., 5., 6., 7.);endif}"
            "{size(myfloats)}";
        REQUIRE(parser.process(script, 0, nullptr, nullptr, nullptr) == "7");
    }
    SECTION("nested if with new variables, two level") {
        std::string script =
            "{if 1 == 1}{if 2 == 3}{nejaka / haluz}{else}{local myints = (6, 5, 4, 3, 2, 1)}{endif}{else}{if zase * haluz}{else}{local myfloats = (1., 2., 3., 4., 5., 6., 7.)}{endif}{endif}"
            "{size(myints)}";
        REQUIRE(parser.process(script, 0, nullptr, nullptr, nullptr) == "6");
    }
    SECTION("if with empty block and ;") {
        std::string script =
            "{if false then else;local myfloats = (1., 2., 3., 4., 5., 6., 7.);endif}"
            "{size(myfloats)}";
        REQUIRE(parser.process(script, 0, nullptr, nullptr, nullptr) == "7");
    }
    SECTION("nested if with new variables, two level, mixing }{ with ;") {
        std::string script =
            "{if 1 == 1 then if 2 == 3}nejaka / haluz{else local myints = (6, 5, 4, 3, 2, 1) endif else if zase * haluz then else local myfloats = (1., 2., 3., 4., 5., 6., 7.) endif endif}"
            "{size(myints)}";
        REQUIRE(parser.process(script, 0, nullptr, nullptr, nullptr) == "6");
    }
    SECTION("nested if with new variables, two level, mixing }{ with ; 2") {
        std::string script =
            "{if 1 == 1 then if 2 == 3 then nejaka / haluz else}{local myints = (6, 5, 4, 3, 2, 1)}{endif else if zase * haluz then else local myfloats = (1., 2., 3., 4., 5., 6., 7.) endif endif}"
            "{size(myints)}";
        REQUIRE(parser.process(script, 0, nullptr, nullptr, nullptr) == "6");
    }
    SECTION("nested if with new variables, two level, mixing }{ with ; 3") {
        std::string script =
            "{if 1 == 1 then if 2 == 3 then nejaka / haluz else}{local myints = (6, 5, 4, 3, 2, 1)}{endif else}{if zase * haluz}{else local myfloats = (1., 2., 3., 4., 5., 6., 7.) endif}{endif}"
            "{size(myints)}";
        REQUIRE(parser.process(script, 0, nullptr, nullptr, nullptr) == "6");
    }
    SECTION("if else completely empty") { REQUIRE(parser.process("{if false then elsif false then else endif}", 0, nullptr, nullptr, nullptr) == ""); }
}

// OrcaSlicer #14526. Our coFloatsOrPercents support came earlier (2bd5c595a4); these are upstream's
// cases. small_perimeter_speed is a plain (non-nullable) ConfigOptionFloatsOrPercents here, and a
// bare reference reads the current extruder's element (upstream reads the filament_map extruder,
// which is the same index when no filament_map is set).
SCENARIO("Placeholder parser coFloatsOrPercents vector access", "[PlaceholderParser]") {
    PlaceholderParser parser;
    auto config = DynamicPrintConfig::full_print_config();

    // outer_wall_speed is the ratio_over target for small_perimeter_speed.
    // Different values per extruder to verify parent resolves at the same element index.
    config.set_deserialize_strict({
        { "outer_wall_speed", "60,70,80,90" },
        { "nozzle_diameter", "0.4,0.4,0.4,0.4" },
        { "pressure_advance", "1.5,2.0,3.0,4.0" }
    });
    // small_perimeter_speed:
    //   [0] = 50% of outer_wall_speed[0] (= 60) -> 30
    //   [1] = 80% of outer_wall_speed[1] (= 70) -> 56
    //   [2] = 0 absolute
    //   [3] = 50% of outer_wall_speed[3] (= 90) -> 45
    REQUIRE(config.option<ConfigOptionFloatsOrPercents>("small_perimeter_speed") != nullptr);
    config.option<ConfigOptionFloatsOrPercents>("small_perimeter_speed")->values = {
        FloatOrPercent{50.0, true},
        FloatOrPercent{80.0, true},
        FloatOrPercent{0.0, false},
        FloatOrPercent{50.0, true},
    };

    parser.apply_config(config);
    parser.set("foo", 0);
    parser.set("bar", 1);
    parser.set("baz", 3);
    parser.set("num_extruders", 4);

    SECTION("Indexed access - percent resolved against parent at same index [0]") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[0]}")) == Approx(30.0)); }
    SECTION("Indexed access - percent resolved against parent at same index [1]") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[1]}")) == Approx(56.0)); }
    SECTION("Indexed access - percent resolved against parent at same index [3]") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[3]}")) == Approx(45.0)); }
    SECTION("Variable-indexed access via foo (=0) - percent value") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[foo]}")) == Approx(30.0)); }
    SECTION("Variable-indexed access via bar (=1) - percent value") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[bar]}")) == Approx(56.0)); }
    SECTION("Variable-indexed access via baz (=3) - percent value") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[baz]}")) == Approx(45.0)); }
    SECTION("Literal-indexed access - absolute value") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[2]}")) == Approx(0.0)); }
    SECTION("No-index (extruder-based) access - percent resolved via current extruder") { REQUIRE(std::stod(parser.process("{small_perimeter_speed}")) == Approx(30.0)); }
    // Edge: the bare reference follows the current extruder.
    SECTION("No-index access on extruder 1") { REQUIRE(std::stod(parser.process("{small_perimeter_speed}", 1)) == Approx(56.0)); }
    SECTION("Out-of-range index clamps to index 0") { REQUIRE(std::stod(parser.process("{small_perimeter_speed[99]}")) == Approx(30.0)); }
    SECTION("coFloats no-index access (outer_wall_speed)") { REQUIRE(std::stod(parser.process("{outer_wall_speed}")) == Approx(60.0)); }
    SECTION("coFloats indexed access (outer_wall_speed)") { REQUIRE(std::stod(parser.process("{outer_wall_speed[2]}")) == Approx(80.0)); }
    SECTION("coFloats no-index access (pressure_advance)") { REQUIRE(std::stod(parser.process("{pressure_advance}")) == Approx(1.5)); }
    SECTION("coFloats indexed access (pressure_advance)") { REQUIRE(std::stod(parser.process("{pressure_advance[2]}")) == Approx(3.0)); }
}
