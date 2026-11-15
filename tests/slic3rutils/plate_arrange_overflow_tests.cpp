// What arranging does with the plates afterwards (PartPlateList, driven as ArrangeJob::finalize does).
//
// Arranging one plate: the objects that did not fit it came back on beds 1, 2, ... and were all
// put on the one place past the last plate, each keeping its spot on its own bed, so the objects of
// bed 1 and bed 2 lay on top of each other. Each such bed now gets a plate of its own.
//
// Recycling the trailing empty plates after an arrange removed an empty locked plate at the end.
// A locked plate is left as it is.
//
// GUI_App.hpp first: Windows headers that PartPlate.hpp's boost::asio path needs.
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/PartPlate.hpp"

#include <catch2/catch.hpp>

using namespace Slic3r;
// Slic3r::PartPlateList is also forward-declared (UndoRedo.hpp), so name the GUI one in full.
using GuiPlateList = Slic3r::GUI::PartPlateList;

namespace {

arrangement::ArrangePolygon square_on_bed(int bed_idx)
{
    arrangement::ArrangePolygon ap;
    ap.poly.contour.points = {Point(0, 0), Point(scaled(50.), 0), Point(scaled(50.), scaled(50.)), Point(0, scaled(50.))};
    ap.translation         = {scaled(100.), scaled(100.)};
    ap.bed_idx             = bed_idx;
    return ap;
}

} // namespace

TEST_CASE("Arranging one plate puts each overflow bed on a plate of its own", "[PartPlateList][Arrange]")
{
    Model        model;
    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    REQUIRE(list.create_plate(false) == 1);
    const int plates_before = list.get_plate_count();

    // Same spot on beds 0, 1 and 2, as the arrangement of one plate hands them back.
    std::vector<arrangement::ArrangePolygon> aps = {square_on_bed(2), square_on_bed(0), square_on_bed(1)};
    for (auto &ap : aps)
        list.postprocess_bed_index_for_current_plate(ap, plates_before);
    for (auto &ap : aps)
        list.postprocess_arrange_polygon(ap, true);

    CHECK(aps[1].bed_idx == list.get_curr_plate_index());
    CHECK(aps[2].bed_idx == plates_before);
    CHECK(aps[0].bed_idx == plates_before + 1);
    CHECK(list.get_plate_count() == plates_before + 2);

    // No two of them in the same place.
    CHECK(aps[0].translation != aps[1].translation);
    CHECK(aps[0].translation != aps[2].translation);
    CHECK(aps[1].translation != aps[2].translation);
}

TEST_CASE("Recycling plates after an arrange keeps an empty locked plate at the end", "[PartPlateList][Arrange]")
{
    Model        model;
    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    REQUIRE(list.create_plate(false) == 1);
    REQUIRE(list.create_plate(false) == 2);
    list.get_plate(2)->lock(true);

    list.rebuild_plates_after_arrangement(true, true);

    // The empty unlocked plate before it may go; the locked one stays, still locked.
    REQUIRE(list.get_plate_count() >= 2);
    CHECK(list.get_plate(list.get_plate_count() - 1)->is_locked());
}
