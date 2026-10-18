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

TEST_CASE("main tabs: Ctrl+1..4 on a settings field open the same tabs as before", "[MainTabs]")
{
    CHECK(MainTabs::tab_for_field_shortcut('1') == MainTabs::Home);
    CHECK(MainTabs::tab_for_field_shortcut('2') == MainTabs::Editor3D);
    CHECK(MainTabs::tab_for_field_shortcut('3') == MainTabs::Preview);
    CHECK(MainTabs::tab_for_field_shortcut('4') == MainTabs::Monitor);   // Device, not Showcase
    CHECK(MainTabs::tab_for_field_shortcut('5') == -1);
}
