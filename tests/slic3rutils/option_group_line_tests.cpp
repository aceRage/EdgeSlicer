// Per-nozzle process P9, owner crash 2026-11-02 (H2C Printer settings > Extruder 1 / 2). The local minidump showed
// OptionsGroup::activate_line reading through a null pointer: it took front() of a line's options to see whether the line is
// a legend, and the Extruder page's widget-only rows (the rack boxes, "Nozzle size in this project") have no option. When such
// a row was the first line of its group the empty vector was dereferenced. line_is_legend() is what activate_line asks now.
#include <catch2/catch.hpp>

#include "slic3r/GUI/OptionsGroup.hpp"

using namespace Slic3r;
using GUILine = Slic3r::GUI::Line;
using Slic3r::GUI::Option;
using Slic3r::GUI::line_is_legend;

TEST_CASE("A widget-only line is no legend line and reading it needs no option", "[OptionsGroup][P9]")
{
    GUILine widget_only{ "Rack nozzles in this project", "tooltip" };
    widget_only.widget = [](wxWindow *) -> wxSizer * { return nullptr; };
    REQUIRE(widget_only.get_options().empty());
    CHECK_FALSE(line_is_legend(widget_only));

    GUILine empty_line;
    CHECK_FALSE(line_is_legend(empty_line));

    ConfigOptionDef def;
    def.type     = coString;
    def.gui_type = ConfigOptionDef::GUIType::legend;
    GUILine legend{ "", "" };
    legend.append_option(Option(def, "legend_key"));
    CHECK(line_is_legend(legend));

    ConfigOptionDef plain;
    plain.type = coFloat;
    GUILine normal{ "Nozzle diameter", "" };
    normal.append_option(Option(plain, "nozzle_diameter"));
    CHECK_FALSE(line_is_legend(normal));
}

// Owner crash 2026-11-10 (Printer settings > Extruder 1, any printer; both local minidumps): OG_CustomCtrl::CtrlLine::
// update_visibility took front() of the options of the Extruder page's "Nozzles in this project" row (a labelled widget with no
// option, in the Size group after nozzle_diameter) to read its mode, reading null + 0x180. The paint path did the same for the
// row's field. line_mode() and line_first_option_id() are what OG_CustomCtrl asks now.
TEST_CASE("A widget-only line has a mode and no first option", "[OptionsGroup][extcrash]")
{
    GUILine widget_only{ "Nozzles in this project", "tooltip" };
    widget_only.widget = [](wxWindow *) -> wxSizer * { return nullptr; };
    REQUIRE(widget_only.get_options().empty());
    // Shown in every mode, the simplest included.
    CHECK(Slic3r::GUI::line_mode(widget_only) == comSimple);
    CHECK(Slic3r::GUI::line_first_option_id(widget_only) == nullptr);

    GUILine separator;
    CHECK(Slic3r::GUI::line_first_option_id(separator) == nullptr);

    ConfigOptionDef advanced;
    advanced.type = coFloat;
    advanced.mode = comAdvanced;
    ConfigOptionDef simple;
    simple.type = coFloat;
    simple.mode = comSimple;
    GUILine normal{ "Nozzle diameter", "" };
    normal.append_option(Option(advanced, "nozzle_diameter"));
    normal.append_option(Option(simple, "nozzle_volume"));
    // The first option decides, as before.
    CHECK(Slic3r::GUI::line_mode(normal) == comAdvanced);
    const t_config_option_key *first = Slic3r::GUI::line_first_option_id(normal);
    REQUIRE(first != nullptr);
    CHECK(*first == "nozzle_diameter");
}
