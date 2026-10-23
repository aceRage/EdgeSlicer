#include <catch2/catch.hpp>

#include <clocale>
#include <sstream>
#include <string>

#ifndef _WIN32
#ifdef __APPLE__
#include <xlocale.h>
#else
#include <locale.h>
#endif
#endif

#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

// Switches this thread's numeric locale to one whose decimal separator is a comma,
// when the host has one installed. Edge has no shared helper; keep it in this file.
// On Unix, apply() uses uselocale so it overrides a CNumericLocalesSetter already
// on the stack (setlocale alone would not: the setter holds the thread locale at C).
struct CommaNumericLocale
{
#ifdef _WIN32
    bool apply()
    {
        static const char *const names[] = {"de-DE", "German_Germany.1252", "German", "fr-FR", "French"};
        for (const char *name : names)
            if (std::setlocale(LC_NUMERIC, name) != nullptr)
                return true;
        return false;
    }
#else
    locale_t locale{(locale_t) 0};

    bool apply()
    {
        static const char *const names[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR"};
        for (const char *name : names) {
            if ((locale = newlocale(LC_NUMERIC_MASK, name, (locale_t) 0)) == (locale_t) 0)
                continue;
            uselocale(locale);
            return true;
        }
        return false;
    }

    // Freed once the setters around apply() have put the thread's own locale back.
    ~CommaNumericLocale()
    {
        if (locale != (locale_t) 0)
            freelocale(locale);
    }

    CommaNumericLocale()                                      = default;
    CommaNumericLocale(const CommaNumericLocale &)            = delete;
    CommaNumericLocale &operator=(const CommaNumericLocale &) = delete;
#endif
};

static DynamicPrintConfig wipe_tower_prime_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FF0000", "#0000FF"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4};
    config.option<ConfigOptionFloats>("wipe_tower_x")->values      = {15.};
    config.option<ConfigOptionFloats>("wipe_tower_y")->values      = {15.};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = true;
    config.option<ConfigOptionFloat>("prime_tower_width")->value             = 35.;
    config.set_deserialize_strict({
        {"skirt_loops",                0},
        {"brim_type",                  "no_brim"},
        {"machine_start_gcode",        ""},
        {"machine_end_gcode",          ""},
        {"wipe_tower_wall_type",       "rectangle"},
        {"layer_height",               0.3},
        {"initial_layer_print_height", 0.3},
        {"gcode_flavor",               "klipper"},
    });
    return config;
}

// Type1 is the Bambu planner (is_BBL_printer). Test::slice cannot set that flag, and
// a brand-new Print's first apply() can clear enable_prime_tower when it still sees
// one filament. Two objects with different extruders plus a second apply keep the tower.
static std::string slice_with_prime_tower(const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    ModelObject *first = model.add_object();
    first->name        = "cube-a.stl";
    first->add_volume(mesh(TestMesh::cube_20x20x20));
    first->add_instance()->set_offset(Vec3d(80., 40., 0.));
    first->ensure_on_bed();
    ModelObject *second = model.add_object();
    second->name        = "cube-b.stl";
    second->add_volume(mesh(TestMesh::cube_20x20x20));
    second->add_instance()->set_offset(Vec3d(120., 40., 0.));
    second->ensure_on_bed();
    second->volumes.front()->config.set("extruder", 2);
    print.apply(model, config);
    print.is_BBL_printer() = true;
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE(print.has_wipe_tower());
    return gcode(print);
}

TEST_CASE("The wipe tower's flow tags keep a decimal point in a comma-decimal locale", "[WipeTower]")
{
    // GCodeProcessor parses these values with the C locale: a comma makes it log an error and
    // ignore the tag, leaving the extrusion width and height it assumes to its own defaults.
    CommaNumericLocale    comma;
    CNumericLocalesSetter restore;
    if (!comma.apply() || is_decimal_separator_point()) {
        // Catch2 v2 has no SKIP(). Succeed-and-return is the skip this suite uses
        // (see test_printgcode.cpp): the case does not fail, and NoAssertions does not fire.
        SUCCEED("skipped: no locale with a comma decimal separator is installed");
        return;
    }
    const std::string gcode = slice_with_prime_tower(wipe_tower_prime_config());
    // WipeTowerWriter forces the BBL flag, so the tower writes these spellings on every printer.
    // By the time the test asks, export has reset the flag for this Klipper config and
    // reserved_tag() would hand back the main G-code's ";HEIGHT:" / ";WIDTH:" instead.
    // The width and height tags are both written by the writer's constructor, before the
    // tower's start tag, so scan the whole slice.
    // MSVC: a lambda cannot capture a function-local constexpr unless it is static.
    static constexpr const char height_tag[] = "; LAYER_HEIGHT: ";
    static constexpr const char width_tag[]  = "; LINE_WIDTH: ";
    size_t                      heights      = 0;
    size_t                      widths       = 0;
    std::istringstream          lines(gcode);
    for (std::string line; std::getline(lines, line);) {
        const bool is_height = line.compare(0, sizeof(height_tag) - 1, height_tag) == 0;
        const bool is_width  = line.compare(0, sizeof(width_tag) - 1, width_tag) == 0;
        if (!is_height && !is_width)
            continue;
        if (is_height)
            ++heights;
        else
            ++widths;
        if (line.find(',') != std::string::npos)
            FAIL_CHECK(line);
    }
    CHECK(heights > 0);
    CHECK(widths > 0);
}
