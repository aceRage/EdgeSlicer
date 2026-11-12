// A plate's layer custom G-codes (pauses, filament changes, custom G-code on the Preview layer
// slider) are filed in Model::plates_custom_gcodes under the plate's index. Deleting a plate or
// moving one to the front left them under the old index: a pause on plate 2 went to whatever plate
// took index 2, and a deleted plate's pause went to the plate that took its place.
//
// GUI_App.hpp first: Windows headers that PartPlate.hpp's boost::asio path needs.
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/PlateCustomGCodes.hpp"

#include <catch2/catch.hpp>

using namespace Slic3r;
// Slic3r::PartPlateList is also forward-declared (UndoRedo.hpp), so name the GUI one in full.
using GuiPlateList = Slic3r::GUI::PartPlateList;
using Catch::Matchers::WithinAbs;

namespace {

CustomGCode::Info pause_at(double print_z)
{
    CustomGCode::Info info;
    info.mode = CustomGCode::SingleExtruder;
    info.gcodes.push_back(CustomGCode::Item{print_z, CustomGCode::PausePrint, 1, "", ""});
    return info;
}

// The print_z of the one item filed under plate `index`, or -1 when the plate has none.
double pause_z(const Model &model, int index)
{
    const auto it = model.plates_custom_gcodes.find(index);
    if (it == model.plates_custom_gcodes.end() || it->second.gcodes.empty())
        return -1.;
    return it->second.gcodes.front().print_z;
}

} // namespace

TEST_CASE("Deleting a plate takes its layer custom G-codes along, the later plates keep theirs", "[PartPlateList][CustomGCode]")
{
    Model        model;
    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    REQUIRE(list.create_plate(false) == 1);
    REQUIRE(list.create_plate(false) == 2);
    model.plates_custom_gcodes[0] = pause_at(2.);
    model.plates_custom_gcodes[1] = pause_at(5.);

    REQUIRE(list.delete_plate(0) == 0);
    REQUIRE(list.get_plate_count() == 2);

    // The plate that had the pause at 5 mm is plate 0 now; the deleted plate's pause is gone.
    CHECK_THAT(pause_z(model, 0), WithinAbs(5., 1e-9));
    CHECK_THAT(pause_z(model, 1), WithinAbs(-1., 1e-9));
    CHECK(model.plates_custom_gcodes.count(2) == 0);
}

TEST_CASE("A plate added after deleting the last plate has no layer custom G-codes", "[PartPlateList][CustomGCode]")
{
    Model        model;
    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    REQUIRE(list.create_plate(false) == 1);
    model.plates_custom_gcodes[1] = pause_at(5.);

    REQUIRE(list.delete_plate(1) == 0);
    // A new or duplicated plate takes index 1 again: it must not find the deleted plate's pause.
    REQUIRE(list.create_plate(false) == 1);
    CHECK_THAT(pause_z(model, 1), WithinAbs(-1., 1e-9));
}

TEST_CASE("Moving a plate to the front moves its layer custom G-codes with it", "[PartPlateList][CustomGCode]")
{
    Model        model;
    GuiPlateList list(270, 270, 270, nullptr, &model, ptFFF);
    REQUIRE(list.create_plate(false) == 1);
    REQUIRE(list.create_plate(false) == 2);
    model.plates_custom_gcodes[0] = pause_at(2.);
    model.plates_custom_gcodes[2] = pause_at(7.);

    // Plater's "Move plate to the front".
    REQUIRE(list.move_plate_to_index(2, 0) == 0);

    CHECK_THAT(pause_z(model, 0), WithinAbs(7., 1e-9));
    CHECK_THAT(pause_z(model, 1), WithinAbs(2., 1e-9));
    CHECK_THAT(pause_z(model, 2), WithinAbs(-1., 1e-9));
}

TEST_CASE("Undo and redo of a plate deletion keep each plate's layer custom G-codes", "[PartPlateList][CustomGCode]")
{
    // UndoRedo::StackImpl::load_snapshot files the items by plate key before it rebuilds the plates
    // and by index afterwards; the plates come back from the snapshot with their keys.
    using Slic3r::GUI::plate_custom_gcodes_by_index;
    using Slic3r::GUI::plate_custom_gcodes_by_key;
    const std::vector<std::string> three{"a", "b", "c"};
    const std::vector<std::string> without_b{"a", "c"};

    // Delete plate "b" (index 1); "c" had the pause at 7 mm.
    std::map<int, CustomGCode::Info> by_index{{1, pause_at(5.)}, {2, pause_at(7.)}};
    auto by_key = plate_custom_gcodes_by_key(by_index, three, {});
    by_index    = plate_custom_gcodes_by_index(by_key, without_b);
    REQUIRE(by_index.size() == 1);
    CHECK_THAT(by_index.at(1).gcodes.front().print_z, WithinAbs(7., 1e-9));
    REQUIRE(by_key.count("b") == 1); // kept for an undo

    // Undo: "b" is back at index 1 with its pause, "c" at index 2 with its own.
    by_key   = plate_custom_gcodes_by_key(by_index, without_b, std::move(by_key));
    by_index = plate_custom_gcodes_by_index(by_key, three);
    CHECK(by_key.empty());
    REQUIRE(by_index.size() == 2);
    CHECK_THAT(by_index.at(1).gcodes.front().print_z, WithinAbs(5., 1e-9));
    CHECK_THAT(by_index.at(2).gcodes.front().print_z, WithinAbs(7., 1e-9));

    // Redo: as after the deletion.
    by_key   = plate_custom_gcodes_by_key(by_index, three, std::move(by_key));
    by_index = plate_custom_gcodes_by_index(by_key, without_b);
    REQUIRE(by_index.size() == 1);
    CHECK_THAT(by_index.at(1).gcodes.front().print_z, WithinAbs(7., 1e-9));
}
