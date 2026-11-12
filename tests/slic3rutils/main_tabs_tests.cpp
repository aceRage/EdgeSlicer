#include <catch2/catch.hpp>

#include "slic3r/GUI/MainTabs.hpp"

#include <set>

using namespace Slic3r::GUI;

// The main window's tab order after Showcase (D1) went in after Preview (design risk R1).

TEST_CASE("main tabs: Showcase sits right after Preview and the rest moved up by one", "[MainTabs]")
{
    CHECK(MainTabs::Home == 0);
    CHECK(MainTabs::Editor3D == 1);
    CHECK(MainTabs::Preview == 2);
    CHECK(MainTabs::Showcase == MainTabs::Preview + 1);
    CHECK(MainTabs::Monitor == 4);
    CHECK(MainTabs::Stream == 5);
    CHECK(MainTabs::Printers == 6);
    CHECK(MainTabs::Project == 7);
    CHECK(MainTabs::Calibration == 8);
    CHECK(MainTabs::Auxiliary == 9);
    CHECK(MainTabs::DebugTool == 10);

    // Contiguous and distinct: the notebook index of page n is n.
    const int all[] = { MainTabs::Home, MainTabs::Editor3D, MainTabs::Preview, MainTabs::Showcase, MainTabs::Monitor,
                        MainTabs::Stream, MainTabs::Printers, MainTabs::Project, MainTabs::Calibration,
                        MainTabs::Auxiliary, MainTabs::DebugTool };
    for (int i = 0; i < int(sizeof(all) / sizeof(all[0])); ++i)
        CHECK(all[i] == i);
}

TEST_CASE("main tabs: the plater hosts Prepare, Preview and Showcase only", "[MainTabs]")
{
    std::set<int> plater;
    for (int t = MainTabs::Home; t <= MainTabs::DebugTool; ++t)
        if (MainTabs::hosts_plater(t))
            plater.insert(t);
    CHECK(plater == std::set<int>{ MainTabs::Editor3D, MainTabs::Preview, MainTabs::Showcase });
}

TEST_CASE("main tabs: web pages switch tabs by name", "[MainTabs]")
{
    CHECK(MainTabs::tab_from_name("Home") == MainTabs::Home);
    CHECK(MainTabs::tab_from_name("3DEditor") == MainTabs::Editor3D);
    CHECK(MainTabs::tab_from_name("Preview") == MainTabs::Preview);
    CHECK(MainTabs::tab_from_name("Showcase") == MainTabs::Showcase);
    // The names used before Showcase still reach the same tabs, now one further along.
    CHECK(MainTabs::tab_from_name("Monitor") == MainTabs::Monitor);
    CHECK(MainTabs::tab_from_name("MultiDevice") == MainTabs::Printers);
    CHECK(MainTabs::tab_from_name("Printers") == MainTabs::Printers);
    CHECK(MainTabs::tab_from_name("Calibration") == MainTabs::Calibration);
    CHECK(MainTabs::tab_from_name("") == -1);
    CHECK(MainTabs::tab_from_name("monitor") == -1);
}

TEST_CASE("main tabs: former Field Ctrl+1..4 map skipped Showcase and Stream", "[MainTabs]")
{
    // Field.cpp no longer binds Ctrl+1..4. This is the old 1-4 map only; Stream was never in it.
    CHECK(MainTabs::tab_for_field_shortcut('1') == MainTabs::Home);
    CHECK(MainTabs::tab_for_field_shortcut('2') == MainTabs::Editor3D);
    CHECK(MainTabs::tab_for_field_shortcut('3') == MainTabs::Preview);
    CHECK(MainTabs::tab_for_field_shortcut('4') == MainTabs::Monitor);   // Device, not Showcase
    CHECK(MainTabs::tab_for_field_shortcut('5') == -1);
}

TEST_CASE("main tabs: a project switch keeps the tab the user is on", "[MainTabs]")
{
    const int count = 11; // Home .. Calibration plus Layerling: the pages present
    // Every page of the table round-trips when it does not move.
    for (int t = MainTabs::Home; t <= MainTabs::Calibration; ++t)
        CHECK(MainTabs::tab_after_switch(t, t, count) == t);
    // The plater pages and Home stay whatever the window lookup says (the plater window is replaced per project).
    CHECK(MainTabs::tab_after_switch(MainTabs::Preview, -1, count) == MainTabs::Preview);
    CHECK(MainTabs::tab_after_switch(MainTabs::Showcase, -1, count) == MainTabs::Showcase);
    CHECK(MainTabs::tab_after_switch(MainTabs::Editor3D, 7, count) == MainTabs::Editor3D);
    CHECK(MainTabs::tab_after_switch(MainTabs::Home, -1, count) == MainTabs::Home);
    // Device: its page window is swapped when the next project's printer has another kind of device page, but the
    // slot stays Device (wx would have moved the selection to Showcase, the page before it).
    CHECK(MainTabs::tab_after_switch(MainTabs::Monitor, -1, count) == MainTabs::Monitor);
    // Pages after Device are found by their window: Calibration absent shifts Layerling down by one.
    CHECK(MainTabs::tab_after_switch(9, 8, 10) == 8);
    CHECK(MainTabs::tab_after_switch(MainTabs::Printers, MainTabs::Printers, 10) == MainTabs::Printers);
    CHECK(MainTabs::tab_after_switch(MainTabs::Project, MainTabs::Project, 10) == MainTabs::Project);
    // A page that is gone (Calibration with a printer that has none, Layerling turned off) falls back to Prepare.
    CHECK(MainTabs::tab_after_switch(MainTabs::Calibration, -1, 8) == MainTabs::Editor3D);
    CHECK(MainTabs::tab_after_switch(9, -1, 9) == MainTabs::Editor3D);
    // Unknown before.
    CHECK(MainTabs::tab_after_switch(-1, -1, count) == MainTabs::Editor3D);
    // No Device page at all (a notebook of three pages): Prepare.
    CHECK(MainTabs::tab_after_switch(MainTabs::Monitor, -1, 3) == MainTabs::Editor3D);
}
