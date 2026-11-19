#include <catch2/catch.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/ClipperUtils.hpp"
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
constexpr double NOZZLE_FLOOR_MM  = MAX_OUTER_NOZZLE_DIAMETER; // Orca #17011

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

// Arranges as the arrange job does for a sequential print, on the bed_config() bed.
void arrange_by_object(ArrangePolygons &items, ArrangePolygons &fixed)
{
    DynamicPrintConfig cfg = bed_config();
    ArrangeParams      p   = seq_print_params(0);
    p.bed_shrink_x         = BED_SHRINK_SEQ_PRINT;
    p.bed_shrink_y         = BED_SHRINK_SEQ_PRINT;
    // Rod and lid above the objects, so they need not each go to a bed of their own.
    p.clearance_height_to_rod = float(NOZZLE_HEIGHT_MM * 10);
    p.clearance_height_to_lid = float(NOZZLE_HEIGHT_MM * 40);
    for (ArrangePolygon &ap : items)
        ap.extrude_ids = {1}; // the sequential sort reads extrude_ids.front()

    update_arrange_params(p, &cfg, items);
    update_selected_items_inflation(items, &cfg, p);
    update_unselected_items_inflation(fixed, &cfg, p);
    arrange(items, fixed, get_shrink_bedpts(&cfg, p), p);
}

ArrangePolygons short_squares(int n, double side_mm)
{
    ArrangePolygons items;
    for (int i = 0; i < n; ++i) {
        items.emplace_back(make_square(scaled(side_mm)));
        items.back().height = NOZZLE_HEIGHT_MM / 2;
    }
    return items;
}

// True when the polygons of the items on bed 0, each grown by `grow_mm`, are pairwise disjoint.
bool disjoint_when_grown(const ArrangePolygons &items, const ArrangePolygons &fixed, double grow_mm)
{
    ExPolygons grown;
    for (const ArrangePolygons *list : {&items, &fixed})
        for (const ArrangePolygon &ap : *list)
            if (ap.bed_idx == 0 && !ap.is_virt_object)
                append(grown, offset_ex(ap.transformed_poly(), float(scale_(grow_mm))));
    for (size_t i = 0; i < grown.size(); ++i)
        for (size_t j = i + 1; j < grown.size(); ++j)
            if (!intersection_ex(ExPolygons{grown[i]}, ExPolygons{grown[j]}).empty())
                return false;
    return true;
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

// Orca #17011: short objects only need a nozzle's width between them, far less than the clearance
// radius, so the bed may only grow by half that width. A row of four 50 mm objects and their gaps
// is wider than the 200 mm bed but would fit one grown by half the clearance radius. Twenty of
// them overfill the bed, so the pile reaches its edges.
TEST_CASE("Sequential print keeps short objects on the bed and a nozzle apart", "[Arrange][SeqPrint]")
{
    ArrangePolygons items = short_squares(20, 50.);
    ArrangePolygons fixed;
    arrange_by_object(items, fixed);

    const BoundingBox bed_bb(Point(0, 0), Point(scaled(200.), scaled(200.)));
    size_t            on_bed = 0;
    for (const ArrangePolygon &ap : items) {
        if (ap.bed_idx != 0)
            continue;
        ++on_bed;
        CHECK(bed_bb.contains(ap.transformed_poly().contour.bounding_box()));
    }
    REQUIRE(on_bed > 1);
    // Grown as Print::sequential_print_clearance_valid() grows them.
    CHECK(disjoint_when_grown(items, fixed, 0.5 * MAX_OUTER_NOZZLE_DIAMETER - 0.1));
}

// Short objects keep off an extrusion calibration region. One along the back edge catches the
// region being moved back by a positive bed shrink, which lets objects onto it.
TEST_CASE("Sequential print keeps short objects off a calibration region", "[Arrange][SeqPrint]")
{
    const BoundingBox region_bb(Point(0, scaled(185.)), Point(scaled(200.), scaled(200.)));
    ArrangePolygon    region;
    region.poly                     = ExPolygon(region_bb.polygon());
    region.bed_idx                  = 0;
    region.is_virt_object           = true;
    region.is_extrusion_cali_object = true;
    ArrangePolygons items = short_squares(20, 40.);
    ArrangePolygons fixed{region};
    arrange_by_object(items, fixed);

    for (const ArrangePolygon &ap : items)
        if (ap.bed_idx == 0)
            CHECK(ap.transformed_poly().contour.bounding_box().max.y() <= region_bb.min.y());
}

// An object closer to the bed edge than the bed margin, as one placed by hand, still takes its room
// when Fill bed adds objects around it, though its inflation crosses the arrange bed's edge.
TEST_CASE("Sequential print keeps short objects off one at the bed edge", "[Arrange][SeqPrint]")
{
    ArrangePolygon obstacle = make_square(scaled(40.));
    obstacle.translation    = {scaled(1.), scaled(80.)};
    obstacle.height         = NOZZLE_HEIGHT_MM / 2;
    obstacle.extrude_ids    = {1};
    ArrangePolygons fixed{obstacle};
    ArrangePolygons items = short_squares(20, 40.);
    arrange_by_object(items, fixed);

    size_t on_bed = 0;
    for (const ArrangePolygon &ap : items)
        on_bed += ap.bed_idx == 0;
    REQUIRE(on_bed > 0);
    CHECK(disjoint_when_grown(items, fixed, 0.));
}
