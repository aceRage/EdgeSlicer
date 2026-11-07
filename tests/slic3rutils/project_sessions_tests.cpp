#include <catch2/catch.hpp>

#include "slic3r/GUI/ProjectSessions.hpp"
#include "libslic3r/ProjectSwapGuard.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI;

// The project strip's rules without wx (tests/design_multi_project_sidebar.md 4.1 and 7).

namespace {

std::vector<std::string> names(const ProjectSessions &s)
{
    std::vector<std::string> out;
    for (const ProjectSlot &slot : s.slots())
        out.push_back(slot.name);
    return out;
}

} // namespace

TEST_CASE("project strip: the first project is the active one", "[ProjectSessions]")
{
    ProjectSessions s;
    CHECK(s.empty());
    CHECK(s.active() == nullptr);
    CHECK(s.neighbour_id(1) == -1);

    const int a = s.add("", s.next_untitled_name("Untitled"));
    REQUIRE(a > 0);
    REQUIRE(s.select(a));
    REQUIRE(s.active() != nullptr);
    CHECK(s.active()->id == a);
    CHECK(s.active()->name == "Untitled");
    CHECK(s.active()->state == ProjectSlot::State::Active);
    // Alone in the strip: nothing to cycle to.
    CHECK(s.neighbour_id(1) == -1);
    CHECK(s.neighbour_id(-1) == -1);
}

TEST_CASE("project strip: new slots go right after the active one, the old one is parked", "[ProjectSessions]")
{
    ProjectSessions s;
    const int a = s.add("C:/p/a.3mf", "a");
    s.select(a);
    const int b = s.add("C:/p/b.3mf", "b");
    s.select(b);
    const int c = s.add("C:/p/c.3mf", "c");
    // c sits after b (the active one), not at the end of a longer list.
    CHECK(names(s) == std::vector<std::string>{ "a", "b", "c" });
    s.select(a);
    const int d = s.add("", "Untitled");
    CHECK(names(s) == std::vector<std::string>{ "a", "Untitled", "b", "c" });
    CHECK(s.find(a)->state == ProjectSlot::State::Active);
    CHECK(s.find(b)->state == ProjectSlot::State::Parked);
    // add() does not select: the strip parks the active project first.
    CHECK(s.active_id() == a);
    CHECK(s.find(d)->state == ProjectSlot::State::Parked);
    CHECK(c != d);
}

TEST_CASE("project strip: the same file selects its slot instead of opening twice", "[ProjectSessions]")
{
    ProjectSessions s;
    const int a = s.add("C:/Prints/Benchy.3mf", "Benchy");
    s.add("", "Untitled");
    CHECK(s.find_by_origin("C:/Prints/Benchy.3mf", false) == a);
    CHECK(s.find_by_origin("C:\\Prints\\Benchy.3mf", false) == a);
    CHECK(s.find_by_origin("c:/prints/benchy.3mf", true) == a);
    CHECK(s.find_by_origin("c:/prints/benchy.3mf", false) == -1);
    CHECK(s.find_by_origin("C:/Prints/Other.3mf", false) == -1);
    // Untitled projects have no file and never match one another.
    CHECK(s.find_by_origin("", false) == -1);
}

TEST_CASE("project strip: Untitled projects are numbered with the lowest free number", "[ProjectSessions]")
{
    ProjectSessions s;
    CHECK(s.next_untitled_name("Untitled") == "Untitled");
    const int a = s.add("", s.next_untitled_name("Untitled"));
    CHECK(s.next_untitled_name("Untitled") == "Untitled 2");
    const int b = s.add("", s.next_untitled_name("Untitled"));
    s.add("", s.next_untitled_name("Untitled"));
    CHECK(names(s) == std::vector<std::string>{ "Untitled", "Untitled 2", "Untitled 3" });
    s.close(b);
    CHECK(s.next_untitled_name("Untitled") == "Untitled 2");
    s.close(a);
    CHECK(s.next_untitled_name("Untitled") == "Untitled");
}

TEST_CASE("project strip: at most 8 projects by default, 2 to 16 from the app config", "[ProjectSessions]")
{
    ProjectSessions s;
    CHECK(s.max_open() == 8);
    for (int i = 0; i < 8; ++i)
        CHECK(s.add("", s.next_untitled_name("Untitled")) > 0);
    CHECK_FALSE(s.can_add());
    CHECK(s.add("", "ninth") == -1);
    CHECK(s.size() == 8);

    CHECK(ProjectSessions::parse_max_open("") == 8);
    CHECK(ProjectSessions::parse_max_open("12") == 12);
    CHECK(ProjectSessions::parse_max_open("1") == 2);
    CHECK(ProjectSessions::parse_max_open("40") == 16);
    CHECK(ProjectSessions::parse_max_open("-3") == 8);
    CHECK(ProjectSessions::parse_max_open("lots") == 8);
    CHECK(ProjectSessions::parse_max_open("99999") == 8);

    ProjectSessions small(ProjectSessions::parse_max_open("2"));
    CHECK(small.add("", "a") > 0);
    CHECK(small.add("", "b") > 0);
    CHECK(small.add("", "c") == -1);
    // Lowering the cap below what is open keeps the open ones; it only stops new ones.
    s.set_max_open(3);
    CHECK(s.size() == 8);
    CHECK_FALSE(s.can_add());
}

TEST_CASE("project strip: next and previous wrap around", "[ProjectSessions]")
{
    ProjectSessions s;
    const int a = s.add("", "a");
    s.select(a);
    const int b = s.add("", "b");
    s.select(b);
    const int c = s.add("", "c");
    s.select(c);
    CHECK(s.neighbour_id(1) == a);
    CHECK(s.neighbour_id(-1) == b);
    s.select(a);
    CHECK(s.neighbour_id(1) == b);
    CHECK(s.neighbour_id(-1) == c);
}

TEST_CASE("project strip: closing a project picks the neighbour", "[ProjectSessions]")
{
    ProjectSessions s;
    const int a = s.add("", "a");
    s.select(a);
    const int b = s.add("", "b");
    s.select(b);
    const int c = s.add("", "c");
    s.select(c);

    SECTION("the middle one: the next one takes over")
    {
        s.select(b);
        CHECK(s.successor_on_close(b) == c);
        REQUIRE(s.close(b));
        CHECK(s.active() == nullptr);
        CHECK(names(s) == std::vector<std::string>{ "a", "c" });
    }
    SECTION("the last one: the previous one takes over")
    {
        CHECK(s.successor_on_close(c) == b);
    }
    SECTION("a parked one: the active project stays")
    {
        REQUIRE(s.close(a));
        CHECK(s.active_id() == c);
        CHECK(names(s) == std::vector<std::string>{ "b", "c" });
    }
    SECTION("the only one: nothing to switch to")
    {
        s.close(a);
        s.close(b);
        CHECK(s.successor_on_close(c) == -1);
        CHECK_FALSE(s.close(12345));
    }
}

TEST_CASE("project strip: reorder", "[ProjectSessions]")
{
    ProjectSessions s;
    const int a = s.add("", "a");
    s.select(a);
    const int b = s.add("", "b");
    s.select(b);
    s.add("", "c");
    REQUIRE(s.move(a, 2));
    CHECK(names(s) == std::vector<std::string>{ "b", "c", "a" });
    CHECK_FALSE(s.move(a, 3));
    CHECK_FALSE(s.move(999, 0));
}

TEST_CASE("project strip: unsaved projects are tracked per slot for the close prompts", "[ProjectSessions]")
{
    ProjectSessions s;
    const int a = s.add("C:/p/a.3mf", "a");
    s.select(a);
    const int b = s.add("", "Untitled");
    const int c = s.add("C:/p/c.3mf", "c");
    CHECK(s.dirty_ids().empty());
    s.set_dirty(c, true);
    s.set_dirty(a, true);
    // Strip order (a, c, b), not the order they became dirty.
    CHECK(s.dirty_ids() == std::vector<int>{ a, c });
    s.set_dirty(a, false);
    CHECK(s.dirty_ids() == std::vector<int>{ c });
    s.close(c);
    CHECK(s.dirty_ids().empty());
    CHECK(b > 0);
}

TEST_CASE("project strip: display names from files", "[ProjectSessions]")
{
    CHECK(ProjectSessions::name_from_path("C:\\Prints\\Benchy.3mf") == "Benchy");
    CHECK(ProjectSessions::name_from_path("/home/me/cube.stl") == "cube");
    CHECK(ProjectSessions::name_from_path("plate.gcode.3mf") == "plate.gcode");
    CHECK(ProjectSessions::name_from_path(".hidden") == ".hidden");
}

TEST_CASE("project strip: the swap guard nests and never goes below zero", "[ProjectSessions][ProjectSwapGuard]")
{
    REQUIRE_FALSE(ProjectSwapGuard::active());
    {
        ProjectSwapGuard outer;
        CHECK(ProjectSwapGuard::active());
        {
            ProjectSwapGuard inner;
            CHECK(ProjectSwapGuard::active());
        }
        CHECK(ProjectSwapGuard::active());
        // The GUI's deferred hold: kept after the scope ends until its CallAfter releases it.
        ProjectSwapGuard::hold();
    }
    CHECK(ProjectSwapGuard::active());
    ProjectSwapGuard::release();
    CHECK_FALSE(ProjectSwapGuard::active());
    ProjectSwapGuard::release();
    CHECK_FALSE(ProjectSwapGuard::active());
    ProjectSwapGuard again;
    CHECK(ProjectSwapGuard::active());
}

// ---- P2 (owner feedback on the P1 hand test) -------------------------------------------------------------------------------

TEST_CASE("project strip: width is dragged between 88 and 200 DIP, the rail stays 24", "[ProjectSessions][StripLayout]")
{
    CHECK(StripLayout::parse_width("") == 88);
    CHECK(StripLayout::parse_width("140") == 140);
    CHECK(StripLayout::parse_width("40") == 88);
    CHECK(StripLayout::parse_width("900") == 200);
    CHECK(StripLayout::parse_width("-5") == 88);
    CHECK(StripLayout::parse_width("wide") == 88);
    CHECK(StripLayout::parse_width("123456") == 88);

    CHECK(StripLayout::width(false, false, 88) == 88);
    CHECK(StripLayout::width(false, false, 150) == 150);
    CHECK(StripLayout::width(false, false, 260) == 200);
    CHECK(StripLayout::width(false, false, 10) == 88);
    // Collapsed: the rail, whatever was dragged; expanding again gives the dragged width back.
    CHECK(StripLayout::width(true, false, 180) == 24);
    CHECK(StripLayout::width(true, true, 180) == 24);
    CHECK(StripLayout::width(false, false, 180) == 180);
    // Names only needs room for a name.
    CHECK(StripLayout::width(false, true, 88) == 120);
    CHECK(StripLayout::width(false, true, 170) == 170);
}

TEST_CASE("project strip: the thumbnail grows with the strip", "[ProjectSessions][StripLayout]")
{
    CHECK(StripLayout::thumb_side(88) == 72);
    CHECK(StripLayout::thumb_side(200) == 184);
    CHECK(StripLayout::thumb_side(StripLayout::width(false, false, 150)) == 134);
    CHECK(StripLayout::thumb_side(40) == StripLayout::min_thumb);
    int previous = 0;
    for (int w = StripLayout::min_width; w <= StripLayout::max_width; w += 8) {
        const int side = StripLayout::thumb_side(w);
        CHECK(side >= previous);
        CHECK(side + 2 * StripLayout::padding <= std::max(w, StripLayout::min_thumb + 2 * StripLayout::padding));
        previous = side;
    }
}

TEST_CASE("project strip: the slice badge", "[ProjectSessions]")
{
    CHECK(slice_badge(false, 0, 0) == SliceBadge::None);
    CHECK(slice_badge(false, 3, 1) == SliceBadge::None);
    CHECK(slice_badge(false, 3, 3) == SliceBadge::Sliced);
    CHECK(slice_badge(false, 1, 1) == SliceBadge::Sliced);
    CHECK(slice_badge(true, 3, 3) == SliceBadge::Slicing);
    CHECK(slice_badge(true, 0, 0) == SliceBadge::Slicing);
}

TEST_CASE("project strip: names wrap on two lines at word ends, never inside a word", "[ProjectSessions]")
{
    // One character = one unit; "..." marks an ellipsis.
    const auto measure  = [](const std::string &s) { return int(s.size()); };
    const auto ellipsis = [](const std::string &s, int w) { return int(s.size()) <= w ? s : s.substr(0, size_t(std::max(0, w - 3))) + "..."; };
    using Lines         = std::vector<std::string>;

    CHECK(wrap_name_two_lines("", 10, measure, ellipsis).empty());
    CHECK(wrap_name_two_lines("Benchy", 10, measure, ellipsis) == Lines{ "Benchy" });
    // The P1 strip broke these mid-word ("h3c_mus / hroom").
    CHECK(wrap_name_two_lines("h3c_mushroom", 9, measure, ellipsis) == Lines{ "h3c_", "mushroom" });
    CHECK(wrap_name_two_lines("my big project", 8, measure, ellipsis) == Lines{ "my big", "project" });
    CHECK(wrap_name_two_lines("plate.gcode", 7, measure, ellipsis) == Lines{ "plate.", "gcode" });
    // The rest does not fit the second line: ellipsised.
    CHECK(wrap_name_two_lines("h3c_mushroom_with_supports", 10, measure, ellipsis) == Lines{ "h3c_", "mushroo..." });
    // A first word wider than the line: one ellipsised line, not a broken word.
    CHECK(wrap_name_two_lines("zephyrawt_fail", 7, measure, ellipsis) == Lines{ "zeph..." });
    for (const Lines &lines : { wrap_name_two_lines("a_long_name_with_many_parts", 6, measure, ellipsis),
                                wrap_name_two_lines("x y z w v u t", 3, measure, ellipsis) }) {
        CHECK(lines.size() <= 2);
        for (const std::string &l : lines)
            CHECK(int(l.size()) <= 6);
    }
}

// ---- P3 ----------------------------------------------------------------------------------------------------------------------

TEST_CASE("project strip: drag reorder keeps the active project and the other slots", "[ProjectSessions][P3]")
{
    ProjectSessions s;
    const int a = s.add("", "a");
    s.select(a);
    const int b = s.add("", "b");
    s.select(b);
    const int c = s.add("", "c");
    s.select(c);
    const int d = s.add("", "d");
    // Drag "a" below "c", then "d" to the top.
    REQUIRE(s.move(a, 2));
    CHECK(names(s) == std::vector<std::string>{ "b", "c", "a", "d" });
    REQUIRE(s.move(d, 0));
    CHECK(names(s) == std::vector<std::string>{ "d", "b", "c", "a" });
    // The active project is still the active one, and cycling follows the new order.
    CHECK(s.active_id() == c);
    CHECK(s.neighbour_id(1) == a);
    CHECK(s.neighbour_id(-1) == b);
    // Dropping on its own place changes nothing.
    REQUIRE(s.move(c, s.index_of(c)));
    CHECK(names(s) == std::vector<std::string>{ "d", "b", "c", "a" });
}

TEST_CASE("project strip: warm park keeps the two most recent within 1.5 GB", "[ProjectSessions][P3][WarmPark]")
{
    const size_t MB = 1024 * 1024;
    using V        = std::vector<int>;
    CHECK(WarmPolicy::evictions({}).empty());
    // Three parked, all small: the oldest goes (K = 2).
    CHECK(WarmPolicy::evictions({ { 1, 100 * MB, 10 }, { 2, 100 * MB, 30 }, { 3, 100 * MB, 20 } }) == V{ 1 });
    // Two that do not fit together: the newer stays.
    CHECK(WarmPolicy::evictions({ { 1, 1000 * MB, 10 }, { 2, 900 * MB, 20 } }) == V{ 1 });
    // One alone over the budget: dropped, it switches back from disk.
    CHECK(WarmPolicy::evictions({ { 7, 2000 * MB, 5 } }) == V{ 7 });
    // A big newest one and a small older one fit together.
    CHECK(WarmPolicy::evictions({ { 1, 200 * MB, 10 }, { 2, 1300 * MB, 20 } }).empty());
    // Several dropped: oldest first.
    CHECK(WarmPolicy::evictions({ { 1, 10 * MB, 1 }, { 2, 10 * MB, 2 }, { 3, 10 * MB, 3 }, { 4, 10 * MB, 4 } }) == V{ 1, 2 });
    // Another K and budget.
    CHECK(WarmPolicy::evictions({ { 1, 10 * MB, 1 }, { 2, 10 * MB, 2 } }, 1, WarmPolicy::default_budget) == V{ 1 });
    CHECK(WarmPolicy::evictions({ { 1, 10 * MB, 1 }, { 2, 10 * MB, 2 } }, 2, 15 * MB) == V{ 1 });
}

TEST_CASE("project strip: the session manifest round-trips and skips what is gone", "[ProjectSessions][P3][SessionRestore]")
{
    SessionManifest m;
    ProjectSlot     a;
    a.name          = "Benchy";
    a.origin_path   = "C:/Prints/Benchy.3mf";
    a.snapshot_path = "C:/tmp/slot-1/project.3mf";
    a.plate         = 1;
    a.main_tab      = 2;
    a.plate_gcodes  = { { 0, "C:/tmp/slot-1/plate_1.gcode" }, { 1, "C:/tmp/slot-1/plate_2.gcode" } };
    a.plates_with_objects = 2;
    a.plates_sliced       = 2;
    ProjectSlot b;
    b.name          = "Untitled 2";
    b.snapshot_path = "C:/tmp/slot-2/project.3mf";
    b.dirty         = true;
    ProjectSlot c;
    c.name          = "Gone";
    c.snapshot_path = "C:/tmp/slot-3/project.3mf";
    m.slots         = { a, b, c };
    m.active        = 2;

    SessionManifest back;
    REQUIRE(SessionManifest::from_json(m.to_json(), back));
    REQUIRE(back.slots.size() == 3);
    CHECK(back.active == 2);
    CHECK(back.slots[0].name == "Benchy");
    CHECK(back.slots[0].origin_path == a.origin_path);
    CHECK(back.slots[0].snapshot_path == a.snapshot_path);
    CHECK(back.slots[0].plate == 1);
    CHECK(back.slots[0].main_tab == 2);
    CHECK(back.slots[0].plate_gcodes == a.plate_gcodes);
    CHECK(back.slots[1].dirty);
    CHECK(back.slots[1].origin_path.empty());

    // Slot 3's snapshot and plate 2's G-code are gone: slot 3 is skipped (it was the active one: the first one takes over),
    // and slot 1 keeps only plate 1's G-code.
    const auto exists = [](const std::string &p) { return p.find("slot-3") == std::string::npos && p.find("plate_2") == std::string::npos; };
    const SessionManifest r = back.restorable(exists);
    REQUIRE(r.slots.size() == 2);
    CHECK(r.active == 0);
    CHECK(r.slots[0].plate_gcodes == std::vector<std::pair<int, std::string>>{ { 0, "C:/tmp/slot-1/plate_1.gcode" } });
    CHECK(r.slots[0].plates_sliced == 1);
    CHECK(r.slots[1].name == "Untitled 2");

    // Not a manifest.
    SessionManifest bad;
    CHECK_FALSE(SessionManifest::from_json("", bad));
    CHECK_FALSE(SessionManifest::from_json("{\"version\":99,\"slots\":[]}", bad));
    CHECK_FALSE(SessionManifest::from_json("[1,2]", bad));
    CHECK(bad.slots.empty());
}

TEST_CASE("project strip: restored slots go to the end with new ids, within the cap", "[ProjectSessions][P3][SessionRestore]")
{
    ProjectSessions s(ProjectSessions::parse_max_open("3"));
    const int first = s.add("", "Untitled");
    s.select(first);
    ProjectSlot r;
    r.name          = "restored";
    r.id            = 999;
    r.snapshot_path = "x/project.3mf";
    const int a     = s.add_restored(r);
    const int b     = s.add_restored(r);
    CHECK(a != 999);
    CHECK(a != b);
    CHECK(s.find(a)->state == ProjectSlot::State::Parked);
    CHECK(s.find(a)->snapshot_path == "x/project.3mf");
    CHECK(s.add_restored(r) == -1);
    CHECK(s.active_id() == first);
}

TEST_CASE("project strip: Explorer / Finder open goes into the strip unless set to replace", "[ProjectSessions][P3]")
{
    CHECK(shell_open_into_strip(""));
    CHECK(shell_open_into_strip("strip"));
    CHECK_FALSE(shell_open_into_strip("replace"));
}

TEST_CASE("project strip: a park keeps the G-code of every sliced plate (owner bug: sliced state lost on a switch)", "[ProjectSessions][P3][SlicedState]")
{
    // On disk: plate 1's result file, plate 3's temp G-code (its result names a file that was renamed), plate 4's result.
    const auto exists = [](const std::string &f) { return f == "B/.1.0.gcode" || f == "B/.1.2.gcode" || f == "B/.1.3.gcode"; };
    const std::vector<PlateGcodeSource> plates = {
        { 0, true, "B/.1.0.gcode", "B/.1.0.gcode" }, // sliced in the GUI
        { 1, false, "", "" },                       // never sliced
        { 2, true, "B/old.gcode", "B/.1.2.gcode" },  // the result's file is gone, the temp G-code is there
        { 3, true, "B/.1.3.gcode", "" },             // a .gcode.3mf plate: its G-code from the file, no temp path
        { 4, true, "", "" },                        // sliced but nothing on disk: comes back unsliced
    };
    const auto plan = park_gcode_plan(plates, exists);
    REQUIRE(plan.size() == 3);
    CHECK(plan[0] == std::make_pair(0, std::string("B/.1.0.gcode")));
    CHECK(plan[1] == std::make_pair(2, std::string("B/.1.2.gcode")));
    CHECK(plan[2] == std::make_pair(3, std::string("B/.1.3.gcode")));
    CHECK(parked_gcode_name(0) == "plate_1.gcode");
    CHECK(parked_gcode_name(2) == "plate_3.gcode");

    // What the park records is what the restore puts back, and what the badge counts as sliced: through the session manifest
    // too (a restored session comes back sliced).
    SessionManifest m;
    ProjectSlot     s;
    s.snapshot_path = "S/project.3mf";
    for (const auto &p : plan)
        s.plate_gcodes.emplace_back(p.first, "S/" + parked_gcode_name(p.first));
    s.plates_with_objects = 4;
    s.plates_sliced       = int(s.plate_gcodes.size());
    m.slots.push_back(s);
    m.active = 0;
    SessionManifest back;
    REQUIRE(SessionManifest::from_json(m.to_json(), back));
    const SessionManifest r = back.restorable([](const std::string &) { return true; });
    REQUIRE(r.slots.size() == 1);
    CHECK(r.slots[0].plate_gcodes == s.plate_gcodes);
    CHECK(slice_badge(false, r.slots[0].plates_with_objects, r.slots[0].plates_sliced) == SliceBadge::None);
    CHECK(slice_badge(false, 3, r.slots[0].plates_sliced) == SliceBadge::Sliced);
}
