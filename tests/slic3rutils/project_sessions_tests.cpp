#include <catch2/catch.hpp>

#include "slic3r/GUI/ProjectSessions.hpp"
#include "libslic3r/ProjectSwapGuard.hpp"

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
