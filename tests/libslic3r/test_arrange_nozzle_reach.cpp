// Arrange option "Avoid nozzle reach zones": on a printer whose nozzles each reach only part of
// the bed (extruder_printable_area: H2D, H2C, X2D), the strips one nozzle alone reaches are given
// to the packer as fixed items on every bed (arrangement::nozzle_reach_blockers), the way ArrangeJob
// gives it the bed exclusion areas. With the option off nothing is added, so the result is today's.
// The GUI wiring (the checkbox, ArrangeJob, FillBedJob) has no unit test.

#include <catch2/catch.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtruderAreas.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::arrangement;

namespace {

constexpr int BEDS = 16;

Pointfs rect_mm(double x0, double y0, double x1, double y1) { return {Vec2d(x0, y0), Vec2d(x1, y0), Vec2d(x1, y1), Vec2d(x0, y1)}; }

struct Printer
{
    const char  *name;
    double       width, depth;
    ExtruderAreas areas;
};

// H2D: left nozzle X 0..325, right 25..350, bed 350 x 320.
Printer h2d() { return {"H2D", 350., 320., compute_extruder_areas(rect_mm(0, 0, 350, 320), {rect_mm(0, 0, 325, 320), rect_mm(25, 0, 350, 320)}, {})}; }
// H2C: left nozzle X 0..325, right 25..330, bed 330 x 320.
Printer h2c() { return {"H2C", 330., 320., compute_extruder_areas(rect_mm(0, 0, 330, 320), {rect_mm(0, 0, 325, 320), rect_mm(25, 0, 330, 320)}, {})}; }

BoundingBox bed_of(const Printer &p) { return BoundingBox(Point(0, 0), Point(scaled(p.width), scaled(p.depth))); }

ArrangePolygons cubes(int count, double size_mm)
{
    ArrangePolygons out;
    for (int i = 0; i < count; ++i) {
        ArrangePolygon ap;
        ap.poly.contour = Polygon(Points{Point(0, 0), Point(scaled(size_mm), 0), Point(scaled(size_mm), scaled(size_mm)), Point(0, scaled(size_mm))});
        ap.inflation    = scaled(1.);
        ap.bed_idx      = 0;
        ap.height       = size_mm;
        ap.extrude_ids  = {1};
        ap.name         = "cube" + std::to_string(i);
        out.emplace_back(std::move(ap));
    }
    return out;
}

ArrangeParams job_like_params()
{
    ArrangeParams params;
    params.parallel      = false;
    auto passes          = std::make_shared<int>(0);
    params.stopcondition = [passes]() { return ++*passes > 1000000; };
    return params;
}

// The blockers as ArrangeJob hands them to the packer: update_unselected_items_inflation() grows
// every virtual item by the 1 mm exclusion gap.
ArrangePolygons blockers_as_job(const ExtruderAreas &areas)
{
    return nozzle_reach_blockers(areas, BEDS, scaled(1.));
}

double overlap_mm2(const Polygon &a, const Polygons &b)
{
    double area = 0.;
    for (const Polygon &p : intersection(Polygons{a}, b))
        area += std::abs(p.area());
    return area * SCALING_FACTOR * SCALING_FACTOR;
}

void check_kept_out(const Printer &printer)
{
    REQUIRE(printer.areas.multi());
    REQUIRE(printer.areas.has_exclusive_regions());

    ArrangePolygons items = cubes(45, 45.);
    arrange(items, blockers_as_job(printer.areas), bed_of(printer), job_like_params());

    Polygons strips;
    for (const Polygons &only : printer.areas.only)
        append(strips, only);
    REQUIRE(!strips.empty());

    std::vector<int> per_bed(BEDS, 0);
    for (const ArrangePolygon &ap : items) {
        INFO(printer.name << " " << ap.name << " on bed " << ap.bed_idx);
        REQUIRE(ap.bed_idx >= 0);
        REQUIRE(ap.bed_idx < BEDS);
        ++per_bed[ap.bed_idx];
        const Polygon     placed = ap.transformed_poly().contour;
        const BoundingBox bb     = placed.bounding_box();
        INFO("x " << unscaled(bb.min.x()) << " - " << unscaled(bb.max.x()));
        CHECK(overlap_mm2(placed, strips) < 0.01);
        // Inside what both nozzles reach.
        CHECK(bb.min.x() >= scaled(25.) - SCALED_EPSILON);
        CHECK(bb.max.x() <= scaled(325.) + SCALED_EPSILON);
    }
    INFO(printer.name << " bed 0: " << per_bed[0] << ", bed 1: " << per_bed[1]);
    // 300 x 320 mm left between the strips: 6 x 6 cubes of 45 mm (47 mm apart) fit a bed.
    CHECK(per_bed[0] >= 20);
    CHECK(per_bed[1] > 0);
}

} // namespace

TEST_CASE("Avoid nozzle reach zones keeps objects out of both one-nozzle strips on an H2D bed", "[Arrange]")
{
    check_kept_out(h2d());
}

TEST_CASE("Avoid nozzle reach zones keeps objects out of both one-nozzle strips on an H2C bed", "[Arrange]")
{
    check_kept_out(h2c());
}

TEST_CASE("Nozzle reach blockers: one fixed virtual item per strip and bed, none without strips", "[Arrange]")
{
    const ArrangePolygons blockers = nozzle_reach_blockers(h2d().areas, 3);
    // Two strips (left only, right only) on three beds.
    REQUIRE(blockers.size() == 6);
    for (const ArrangePolygon &ap : blockers) {
        CHECK(ap.is_virt_object);
        CHECK(ap.bed_idx >= 0);
        CHECK(ap.bed_idx < 3);
    }

    // One nozzle, or nozzles that reach the same bed: nothing to avoid.
    CHECK(nozzle_reach_blockers(ExtruderAreas(), BEDS).empty());
    const ExtruderAreas same = compute_extruder_areas(rect_mm(0, 0, 256, 256), {rect_mm(0, 0, 256, 256), rect_mm(0, 0, 256, 256)}, {});
    CHECK(nozzle_reach_blockers(same, BEDS).empty());
}

TEST_CASE("With Avoid nozzle reach zones off the arrangement is today's", "[Arrange]")
{
    // Off, ArrangeJob adds no blockers: the packer gets exactly what it got before the option.
    for (const Printer &printer : {h2d(), h2c()}) {
        ArrangePolygons today = cubes(20, 45.);
        ArrangePolygons off   = today;
        arrange(today, {}, bed_of(printer), job_like_params());
        arrange(off, ArrangePolygons{}, bed_of(printer), job_like_params());
        for (size_t i = 0; i < today.size(); ++i) {
            INFO(printer.name << " item " << i);
            CHECK(off[i].bed_idx == today[i].bed_idx);
            CHECK(off[i].translation == today[i].translation);
        }
    }
}
