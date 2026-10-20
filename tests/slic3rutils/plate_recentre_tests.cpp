// The printer-switch plate recentre (src/slic3r/GUI/PlateRecentre.hpp, used by
// Plater::priv::on_select_preset).
//
// Before: one "Move Object" undo snapshot per plate (empty plates included, ~130 - 210 ms each on
// a 14-object project) plus a selection snapshot for the final deselect. After: one snapshot for
// the whole pass, empty plates skipped. Object positions must not change at all.
//
// The driver below is the code Plater runs; the GUI calls are replaced by fakes that record what
// they were asked to do, and by a small positional model of "select a plate's objects, move their
// bounding-box centre onto the plate centre" to compare against the old per-plate loop.

#include <catch2/catch.hpp>

#include "slic3r/GUI/PlateRecentre.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

using namespace Slic3r::GUI;

namespace {

// Stand-in for Plater::TakeSnapshot: takes one snapshot unless suppressed, then suppresses nested
// snapshots for its lifetime.
struct FakeUndo
{
    int snapshots  = 0;
    int suppressed = 0;
    void take(/*name*/) { if (suppressed == 0) ++snapshots; }
};

struct FakeTakeSnapshot
{
    FakeUndo* undo;
    explicit FakeTakeSnapshot(FakeUndo& u) : undo(&u)
    {
        undo->take();
        ++undo->suppressed;
    }
    ~FakeTakeSnapshot() { --undo->suppressed; }
};

// Objects as axis-aligned boxes in the bed plane; plates as centres. What the old loop did per
// plate: select the plate's objects, move them by (plate centre - selection bbox centre).
struct Box { double x0, y0, x1, y1; };

void centre_group(std::vector<Box>& objs, const std::vector<int>& idxs, const std::array<double, 2>& centre)
{
    if (idxs.empty())
        return; // an empty selection moves nothing
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    for (int i : idxs) {
        x0 = std::min(x0, objs[i].x0); y0 = std::min(y0, objs[i].y0);
        x1 = std::max(x1, objs[i].x1); y1 = std::max(y1, objs[i].y1);
    }
    const double dx = centre[0] - (x0 + x1) / 2., dy = centre[1] - (y0 + y1) / 2.;
    for (int i : idxs) {
        objs[i].x0 += dx; objs[i].x1 += dx;
        objs[i].y0 += dy; objs[i].y1 += dy;
    }
}

} // namespace

TEST_CASE("only plates holding objects are recentred, in plate order", "[PlateRecentre]")
{
    CHECK(plates_to_recentre({ { 0, 1 }, {}, { 2 }, {} }) == std::vector<int>{ 0, 2 });
    CHECK(plates_to_recentre({ {}, { 3 } }) == std::vector<int>{ 1 });
    CHECK(plates_to_recentre({ { 0 } }) == std::vector<int>{ 0 });
    // No objects anywhere: plate 0 alone, so the one scene/model sync of the old loop still runs.
    CHECK(plates_to_recentre({ {}, {}, {} }) == std::vector<int>{ 0 });
    CHECK(plates_to_recentre({}).empty());
}

TEST_CASE("a whole recentre takes exactly one undo snapshot", "[PlateRecentre]")
{
    const std::vector<std::vector<int>> plates = { { 0, 1 }, {}, { 2 }, { 3, 4, 5 }, {}, { 6 } };

    FakeUndo         undo;
    std::vector<int> visited;
    bool             finished_inside = false;
    const int n = run_plate_recentre(
        plates, [&]() { return FakeTakeSnapshot(undo); },
        [&](int i) {
            visited.push_back(i);
            undo.take(); // do_move("Move Object") inside center_plate
            undo.take(); // a selection change snapshot, if any
        },
        [&]() {
            finished_inside = undo.suppressed > 0;
            undo.take(); // the deselect's "Selection-Remove All!" snapshot
        });

    CHECK(n == 4);
    CHECK(visited == std::vector<int>{ 0, 2, 3, 5 });
    CHECK(undo.snapshots == 1);
    CHECK(finished_inside);
    CHECK(undo.suppressed == 0); // snapshots are allowed again afterwards

    // For comparison, the old loop: one per plate (empty ones too) plus the deselect.
    FakeUndo old;
    for (size_t i = 0; i < plates.size(); ++i)
        old.take();
    old.take();
    CHECK(old.snapshots == int(plates.size()) + 1);
}

TEST_CASE("objects end up exactly where the per-plate loop put them", "[PlateRecentre]")
{
    // Six plates, some empty, objects of odd sizes at odd offsets (as after a bed-size change).
    const std::vector<std::array<double, 2>> centres = { { { 135.5, 128.25 } }, { { 437.1, 128.25 } }, { { 738.7, 128.25 } },
                                                         { { 135.5, -201.3 } }, { { 437.1, -201.3 } }, { { 738.7, -201.3 } } };
    const std::vector<std::vector<int>>      plates  = { { 0, 1, 2 }, {}, { 3 }, { 4, 5 }, {}, {} };
    const std::vector<Box> start = { { 10.1, 20.2, 40.3, 60.7 },    { 50.05, 15.5, 90.9, 35.25 },   { 30.0, 70.0, 31.7, 99.9 },
                                     { 610.3, 40.4, 700.0, 140.1 }, { 15.5, -250.0, 60.25, -220.5 }, { 70.0, -260.3, 80.8, -150.0 } };

    // The old loop: every plate in order, empty ones included.
    std::vector<Box> old_way = start;
    for (size_t i = 0; i < plates.size(); ++i)
        centre_group(old_way, plates[i], centres[i]);

    // The new driver.
    std::vector<Box> new_way = start;
    FakeUndo         undo;
    run_plate_recentre(
        plates, [&]() { return FakeTakeSnapshot(undo); }, [&](int i) { centre_group(new_way, plates[i], centres[i]); }, []() {});

    REQUIRE(new_way.size() == old_way.size());
    for (size_t i = 0; i < old_way.size(); ++i) {
        INFO("object " << i);
        // Bit-for-bit: the same arithmetic in the same order, no tolerance.
        CHECK(new_way[i].x0 == old_way[i].x0);
        CHECK(new_way[i].y0 == old_way[i].y0);
        CHECK(new_way[i].x1 == old_way[i].x1);
        CHECK(new_way[i].y1 == old_way[i].y1);
    }
}
