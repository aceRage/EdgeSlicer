#include <catch2/catch.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::arrangement;

// Orca #15425 (sequential-print arrange settings ignored and never persisted). Upstream dropped the
// arrange menu's own floor for the sequential-print spacing, so a stored 0 ("auto") now reaches the
// arrange job unchanged: these pin down that update_selected_items_inflation() is where the
// clearance floor is applied, and what min_object_distance() returns per print sequence.

namespace {

ArrangePolygon make_square(coord_t side)
{
    ArrangePolygon ap;
    Polygon        p;
    p.points   = {Point(0, 0), Point(side, 0), Point(side, side), Point(0, side)};
    ap.poly    = ExPolygon(p);
    ap.bed_idx = 0;
    return ap;
}

// The sequential-print floor is chosen by comparing object height against the nozzle,
// so the two are defined together and every expectation is derived from them.
constexpr double NOZZLE_HEIGHT_MM = 2.5;
constexpr double CLEARANCE_MM     = 30.;
constexpr double NOZZLE_FLOOR_MM  = MAX_OUTER_NOZZLE_DIAMETER / 2.;

ArrangeParams seq_print_params(coord_t min_dist)
{
    ArrangeParams p(min_dist);
    p.progressind         = [](unsigned, std::string) {};
    p.is_seq_print        = true;
    p.clearance_radius    = float(CLEARANCE_MM);
    p.nozzle_height       = float(NOZZLE_HEIGHT_MM);
    p.object_skirt_offset = 0.f;
    return p;
}

// update_selected_items_inflation reads the bed out of the config to cap inflation.
DynamicPrintConfig bed_config()
{
    DynamicPrintConfig c;
    c.set_key_value("printable_area", new ConfigOptionPoints{Vec2d(0, 0), Vec2d(200, 0), Vec2d(200, 200), Vec2d(0, 200)});
    return c;
}

ArrangePolygons squares_of_heights(const std::vector<double> &heights_mm)
{
    ArrangePolygons items;
    for (double height_mm : heights_mm) {
        items.emplace_back(make_square(scaled(20.)));
        items.back().height = height_mm;
    }
    return items;
}

// min_object_distance reads exactly these three options.
DynamicPrintConfig spacing_config(PrinterTechnology tech, PrintSequence seq, double clearance_radius)
{
    DynamicPrintConfig c;
    c.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(tech));
    c.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(seq));
    c.set_key_value("extruder_clearance_radius", new ConfigOptionFloat(clearance_radius));
    return c;
}

} // namespace

TEST_CASE("Sequential print floors the object distance by object height", "[Arrange][SeqPrint]")
{
    // The only place sequential-print clearance is enforced. The arrange menu offers
    // no floor of its own, so a stored 0 has to be raised here or not at all.
    struct Case
    {
        const char         *description;
        std::vector<double> heights;
        double              skirt_offset_mm;
        double              expected_floor_mm;
    };

    const std::vector<Case> cases = {
        {"objects taller than the nozzle need the full clearance",     {NOZZLE_HEIGHT_MM * 2, NOZZLE_HEIGHT_MM * 2}, 0., CLEARANCE_MM},
        {"an object exactly at the nozzle height counts as tall",      {NOZZLE_HEIGHT_MM,     NOZZLE_HEIGHT_MM},     0., CLEARANCE_MM},
        {"one tall object among short ones is enough",                 {NOZZLE_HEIGHT_MM / 2, NOZZLE_HEIGHT_MM * 2}, 0., CLEARANCE_MM},
        {"objects the nozzle clears keep only the nozzle-width floor", {NOZZLE_HEIGHT_MM / 2, NOZZLE_HEIGHT_MM / 2}, 0., NOZZLE_FLOOR_MM},
        {"a wide skirt raises the floor for short objects",            {NOZZLE_HEIGHT_MM / 2, NOZZLE_HEIGHT_MM / 2}, 3., 6.},
    };

    for (const Case &c : cases) {
        INFO(c.description);
        ArrangePolygons    items = squares_of_heights(c.heights);
        DynamicPrintConfig cfg   = bed_config();
        ArrangeParams      p     = seq_print_params(0);
        p.object_skirt_offset    = float(c.skirt_offset_mm);

        update_selected_items_inflation(items, &cfg, p);

        CHECK(p.min_obj_distance >= scaled(c.expected_floor_mm));
        CHECK(p.min_obj_distance <= scaled(c.expected_floor_mm + 0.01));
        // Half each, so a pair ends up a full min_obj_distance apart.
        CHECK(items.front().inflation == p.min_obj_distance / 2);
    }
}

TEST_CASE("Sequential print keeps an object distance already above the floor", "[Arrange][SeqPrint]")
{
    const coord_t      stored = scaled(CLEARANCE_MM * 2);
    ArrangePolygons    items  = squares_of_heights({NOZZLE_HEIGHT_MM * 2, NOZZLE_HEIGHT_MM * 2});
    DynamicPrintConfig cfg    = bed_config();
    ArrangeParams      p      = seq_print_params(stored);

    update_selected_items_inflation(items, &cfg, p);
    CHECK(p.min_obj_distance == stored);
}

TEST_CASE("Layered printing does not floor the object distance", "[Arrange][SeqPrint]")
{
    ArrangePolygons    items = squares_of_heights({NOZZLE_HEIGHT_MM * 2, NOZZLE_HEIGHT_MM * 2});
    DynamicPrintConfig cfg   = bed_config();
    ArrangeParams      p     = seq_print_params(0);
    p.is_seq_print           = false;

    update_selected_items_inflation(items, &cfg, p);
    CHECK(p.min_obj_distance == 0);
}

TEST_CASE("min_object_distance floors object spacing per print sequence", "[Config][SeqPrint]")
{
    struct Case
    {
        const char       *description;
        PrinterTechnology tech;
        PrintSequence     sequence;
        double            clearance_radius;
        double            expected;
    };

    const std::vector<Case> cases = {
        {"sequential FFF takes a clearance radius above the floor", ptFFF, PrintSequence::ByObject, 12., 12.},
        {"sequential FFF holds the floor at the radius",            ptFFF, PrintSequence::ByObject,  6.,  6.},
        {"sequential FFF holds the floor below the radius",         ptFFF, PrintSequence::ByObject,  4.,  6.},
        {"layered FFF ignores the clearance radius",                ptFFF, PrintSequence::ByLayer,  12.,  6.},
        {"SLA is a flat 6mm",                                       ptSLA, PrintSequence::ByObject, 12.,  6.},
        {"SLA ignores the print sequence too",                      ptSLA, PrintSequence::ByLayer,  12.,  6.},
    };

    for (const Case &c : cases) {
        INFO(c.description);
        CHECK(min_object_distance(spacing_config(c.tech, c.sequence, c.clearance_radius)) == Approx(c.expected).margin(1e-9));
    }
}

TEST_CASE("min_object_distance yields no floor when an FFF config lacks the options", "[Config][SeqPrint]")
{
    // Missing options yield 0 rather than an error, so a caller gets no floor at all.
    SECTION("no clearance radius") {
        DynamicPrintConfig c;
        c.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptFFF));
        c.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
        CHECK(min_object_distance(c) == Approx(0.).margin(1e-9));
    }

    SECTION("no print sequence") {
        DynamicPrintConfig c;
        c.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptFFF));
        c.set_key_value("extruder_clearance_radius", new ConfigOptionFloat(12.));
        CHECK(min_object_distance(c) == Approx(0.).margin(1e-9));
    }

    SECTION("nothing at all") {
        CHECK(min_object_distance(DynamicPrintConfig{}) == Approx(0.).margin(1e-9));
    }

    SECTION("an unset printer technology is treated as FFF") {
        DynamicPrintConfig c;
        c.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
        c.set_key_value("extruder_clearance_radius", new ConfigOptionFloat(12.));
        CHECK(min_object_distance(c) == Approx(12.).margin(1e-9));
    }
}
