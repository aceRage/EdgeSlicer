// The Device tab's filament area on two-extruder Bambu printers, and AMS remote drying.
//
// Covered with no printer, no window and no broker:
//   * how the "info" / "fun2" hex strings are read (Bambu's get_flag_bits_no_border),
//   * which side (extruder) each AMS unit and external spool lands on, and how the body pages are cut,
//   * the slot labels and the "loaded" test for an extruder's current slot,
//   * the drying presets, limits and checks of Bambu Studio's AMS Dryness Control, and
//   * the exact ams_filament_drying payloads, compared field for field with what Bambu Studio sends
//     (DevFilaSystem::CtrlAmsStartDryingHour / CtrlAmsStopDrying). Nothing here is ever sent.

#include <catch2/catch.hpp>

#include "slic3r/GUI/AmsDrying.hpp"
#include "slic3r/GUI/AmsDualLayout.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using nlohmann::json;

namespace {

std::vector<std::string> ids(const AmsDual::Side &side)
{
    std::vector<std::string> out;
    for (const AmsDual::UnitRef &u : side.units)
        out.push_back(u.ams_id);
    return out;
}

AmsDual::UnitRef unit(const std::string &id, int type, int extruder, int slots)
{
    AmsDual::UnitRef u;
    u.ams_id      = id;
    u.type        = type;
    u.extruder_id = extruder;
    u.slot_count  = slots;
    return u;
}

bool has_issue(const AmsDrying::Check &c, AmsDrying::Issue i) { return std::find(c.issues.begin(), c.issues.end(), i) != c.issues.end(); }

} // namespace

TEST_CASE("Flag bits are read from hex strings of any length", "[AmsDual]")
{
    using AmsDual::flag_bits_no_border;
    CHECK(flag_bits_no_border("0", 0, 4) == 0);
    CHECK(flag_bits_no_border("4", 0, 4) == 4);
    CHECK(flag_bits_no_border("124", 8, 4) == 1);
    CHECK(flag_bits_no_border("0x124", 8, 4) == 1);
    CHECK(flag_bits_no_border("  124 ", 4, 4) == 2);
    // Bits past the end read as 0, as do garbage and bad arguments.
    CHECK(flag_bits_no_border("F", 4, 4) == 0);
    CHECK(flag_bits_no_border("zz", 0, 4) == 0);
    CHECK(flag_bits_no_border("FF", -1, 4) == 0);
    CHECK(flag_bits_no_border("FF", 0, 0) == 0);
    // fun2 grows past 64 bits; bit 5 is still the fifth bit of the last digit, and a high bit is reachable.
    const std::string fun2 = "8000000000000000000020";
    CHECK(flag_bits_no_border(fun2, 5) == 1);
    CHECK(flag_bits_no_border(fun2, 87) == 1);
    CHECK(flag_bits_no_border(fun2, 86) == 0);
}

TEST_CASE("A unit's info field names its type, extruder and drying state", "[AmsDual]")
{
    // AMS HT (4) on the deputy extruder (1), drying (2), sub-status dehumidify (2), fan 1 on.
    // type | dry<<4 | extruder<<8 | fan1<<18 | sub<<22  =  0x4 | 0x20 | 0x100 | 0x40000 | 0x800000
    const AmsDual::UnitInfoBits b = AmsDual::parse_unit_info("840124");
    CHECK(b.type == AmsDual::UNIT_N3S);
    CHECK(b.dry_status == int(AmsDrying::DryStatus::Drying));
    CHECK(b.extruder_id == AmsDual::DEPUTY_EXTRUDER);
    CHECK(b.dry_fan1 == 1);
    CHECK(b.dry_fan2 == 0);
    CHECK(b.dry_sub_status == int(AmsDrying::DrySubStatus::Dehumidify));

    // A plain AMS on the main extruder, idle.
    const AmsDual::UnitInfoBits a = AmsDual::parse_unit_info("1");
    CHECK(a.type == AmsDual::UNIT_AMS);
    CHECK(a.extruder_id == AmsDual::MAIN_EXTRUDER);
    CHECK(a.dry_status == 0);

    CHECK(AmsDual::fun2_supports_remote_dry("20"));
    CHECK(AmsDual::fun2_supports_remote_dry("0x3f"));
    CHECK_FALSE(AmsDual::fun2_supports_remote_dry("1f"));
    CHECK_FALSE(AmsDual::fun2_supports_remote_dry(""));
}

TEST_CASE("Two-extruder printers put the deputy extruder's units on the left", "[AmsDual]")
{
    // The owner's H2D: an AMS HT and the left spool on the left extruder, a 4-slot AMS and the right spool
    // on the right one. The order the printer lists them in does not matter.
    const std::vector<AmsDual::UnitRef> units = {
        unit("255", AmsDual::UNIT_EXT_SPOOL, 0, 1),
        unit("0", AmsDual::UNIT_N3F, AmsDual::MAIN_EXTRUDER, 4),
        unit("254", AmsDual::UNIT_EXT_SPOOL, 0, 1),
        unit("128", AmsDual::UNIT_N3S, AmsDual::DEPUTY_EXTRUDER, 1),
    };
    const auto sides = AmsDual::group_units(units, 2);
    REQUIRE(sides.size() == 2);

    CHECK(sides[0].extruder_id == AmsDual::DEPUTY_EXTRUDER);
    CHECK(ids(sides[0]) == std::vector<std::string>{"128", "254"});
    // Two single-slot units share one page, as Bambu shows "A" and "Ext" side by side.
    CHECK(sides[0].pages == std::vector<std::vector<int>>{{0, 1}});

    CHECK(sides[1].extruder_id == AmsDual::MAIN_EXTRUDER);
    CHECK(ids(sides[1]) == std::vector<std::string>{"0", "255"});
    CHECK(sides[1].pages == std::vector<std::vector<int>>{{0}, {1}});

    CHECK(AmsDual::side_of_unit(sides, "128") == 0);
    CHECK(AmsDual::side_of_unit(sides, "255") == 1);
    CHECK(AmsDual::side_of_unit(sides, "7") == -1);
    CHECK(AmsDual::page_of_unit(sides[1], "255") == 1);
    CHECK(AmsDual::page_of_unit(sides[1], "128") == -1);
}

TEST_CASE("Pages: a multi-slot unit alone, single-slot units two at a time", "[AmsDual]")
{
    const std::vector<AmsDual::UnitRef> units = {
        unit("129", AmsDual::UNIT_N3S, AmsDual::MAIN_EXTRUDER, 1), unit("1", AmsDual::UNIT_AMS, AmsDual::MAIN_EXTRUDER, 4),
        unit("128", AmsDual::UNIT_N3S, AmsDual::MAIN_EXTRUDER, 1), unit("0", AmsDual::UNIT_AMS, AmsDual::MAIN_EXTRUDER, 4),
        unit("130", AmsDual::UNIT_N3S, AmsDual::MAIN_EXTRUDER, 1), unit("255", AmsDual::UNIT_EXT_SPOOL, 0, 1),
    };
    const auto sides = AmsDual::group_units(units, 2);
    REQUIRE(sides.size() == 2);
    CHECK(sides[0].units.empty());
    CHECK(sides[0].pages.empty());
    // Numeric id order, spool last.
    CHECK(ids(sides[1]) == std::vector<std::string>{"0", "1", "128", "129", "130", "255"});
    CHECK(sides[1].pages == std::vector<std::vector<int>>{{0}, {1}, {2, 3}, {4, 5}});
}

TEST_CASE("One extruder: everything on one side, the spool included", "[AmsDual]")
{
    const std::vector<AmsDual::UnitRef> units = {
        unit("254", AmsDual::UNIT_EXT_SPOOL, 0, 1),
        unit("1", AmsDual::UNIT_AMS, AmsDual::MAIN_EXTRUDER, 4),
        unit("0", AmsDual::UNIT_AMS, AmsDual::MAIN_EXTRUDER, 4),
    };
    const auto sides = AmsDual::group_units(units, 1);
    REQUIRE(sides.size() == 1);
    CHECK(sides[0].extruder_id == AmsDual::MAIN_EXTRUDER);
    CHECK(ids(sides[0]) == std::vector<std::string>{"0", "1", "254"});
    CHECK(AmsDual::extruder_of_virtual_slot(254, 1) == AmsDual::MAIN_EXTRUDER);
    CHECK(AmsDual::extruder_of_virtual_slot(254, 2) == AmsDual::DEPUTY_EXTRUDER);
    CHECK(AmsDual::extruder_of_virtual_slot(255, 2) == AmsDual::MAIN_EXTRUDER);
}

TEST_CASE("A unit naming an extruder the printer does not have shows with the main one", "[AmsDual]")
{
    const auto sides = AmsDual::group_units({unit("2", AmsDual::UNIT_AMS, 5, 4), unit("3", AmsDual::UNIT_AMS, AmsDual::DEPUTY_EXTRUDER, 4)}, 2);
    CHECK(ids(sides[0]) == std::vector<std::string>{"3"});
    CHECK(ids(sides[1]) == std::vector<std::string>{"2"});
}

TEST_CASE("Slot labels follow Bambu's naming", "[AmsDual]")
{
    CHECK(AmsDual::slot_label(unit("0", AmsDual::UNIT_AMS, 0, 4), 0) == "A1");
    CHECK(AmsDual::slot_label(unit("0", AmsDual::UNIT_AMS, 0, 4), 2) == "A3");
    CHECK(AmsDual::slot_label(unit("3", AmsDual::UNIT_N3F, 0, 4), 3) == "D4");
    CHECK(AmsDual::slot_label(unit("128", AmsDual::UNIT_N3S, 0, 1), 0) == "A");
    CHECK(AmsDual::slot_label(unit("129", AmsDual::UNIT_N3S, 0, 1), 0) == "B");
    CHECK(AmsDual::slot_label(unit("254", AmsDual::UNIT_EXT_SPOOL, 0, 1), 0) == "Ext");
    CHECK(AmsDual::slot_label(unit("255", AmsDual::UNIT_AMS, 0, 4), 0) == "Ext");
    CHECK(AmsDual::slot_label(unit("x", AmsDual::UNIT_AMS, 0, 4), 0) == "?");
}

TEST_CASE("An extruder is loaded unless its current slot is 255", "[AmsDual]")
{
    CHECK_FALSE(AmsDual::slot_is_loaded("255", "255"));
    CHECK_FALSE(AmsDual::slot_is_loaded("", ""));
    CHECK(AmsDual::slot_is_loaded("0", "2"));
    CHECK(AmsDual::slot_is_loaded("128", "0"));
    CHECK(AmsDual::slot_is_loaded("254", "0"));
    CHECK(AmsDual::slot_is_loaded("255", "0"));
}

TEST_CASE("Only heated units with firmware support get the dryness dialog", "[AmsDrying]")
{
    using namespace AmsDrying;
    CHECK(unit_has_heater(AmsDual::UNIT_N3F));
    CHECK(unit_has_heater(AmsDual::UNIT_N3S));
    CHECK_FALSE(unit_has_heater(AmsDual::UNIT_AMS));
    CHECK_FALSE(unit_has_heater(AmsDual::UNIT_AMS_LITE));
    CHECK_FALSE(unit_has_heater(AmsDual::UNIT_EXT_SPOOL));

    CHECK(unit_supports_remote_dry(true, AmsDual::UNIT_N3F));
    CHECK(unit_supports_remote_dry(true, AmsDual::UNIT_N3S));
    CHECK_FALSE(unit_supports_remote_dry(false, AmsDual::UNIT_N3F)); // fun2 bit 5 not set
    CHECK_FALSE(unit_supports_remote_dry(true, AmsDual::UNIT_AMS));
}

TEST_CASE("Drying state is read from the unit's report and kept across partial pushes", "[AmsDrying]")
{
    using namespace AmsDrying;
    DryState st;
    CHECK(is_idle(st)); // nothing reported yet

    parse_dry_fields(json::parse(R"({"id":"128","info":"840124","dry_time":615,
        "dry_setting":{"dry_filament":"PLA","dry_temperature":45,"dry_duration":24},
        "dry_sf_reason":[6]})"),
                     st);
    CHECK(st.has_status);
    CHECK(st.status == int(DryStatus::Drying));
    CHECK(st.sub_status == int(DrySubStatus::Dehumidify));
    CHECK(st.fan1 == 1);
    CHECK(st.has_settings);
    CHECK(st.setting_filament == "PLA");
    CHECK(st.setting_temp == 45);
    CHECK(st.setting_hours == 24);
    CHECK(st.has_cannot_reasons);
    CHECK(st.cannot_reasons == std::vector<int>{int(CannotDryReason::DryingInProgress)});
    CHECK_FALSE(is_idle(st));
    CHECK(is_drying_active(st.status));

    // An incremental push without those fields changes nothing.
    parse_dry_fields(json::parse(R"({"id":"128","humidity":"3"})"), st);
    CHECK(st.status == int(DryStatus::Drying));
    CHECK(st.setting_temp == 45);

    // Settings may arrive as strings.
    parse_dry_fields(json::parse(R"({"dry_setting":{"dry_filament":"ABS","dry_temperature":"80","dry_duration":"8"}})"), st);
    CHECK(st.setting_temp == 80);
    CHECK(st.setting_hours == 8);

    // Cooling counts as idle (Start is offered again), Error does not.
    DryState cooling;
    parse_dry_fields(json::parse(R"({"info":"34"})"), cooling);
    CHECK(cooling.status == int(DryStatus::Cooling));
    CHECK(is_idle(cooling));
    DryState err;
    parse_dry_fields(json::parse(R"({"info":"54"})"), err);
    CHECK(is_error(err));
    CHECK_FALSE(is_idle(err));
}

TEST_CASE("The drying sun follows the status, or the remaining time on older firmware", "[AmsDrying]")
{
    using namespace AmsDrying;
    DryState none;
    CHECK(shows_drying_icon(none, true, 30));   // no status reported: fall back to time left
    CHECK_FALSE(shows_drying_icon(none, true, 0));
    DryState drying;
    drying.has_status = true;
    drying.status     = int(DryStatus::Drying);
    CHECK(shows_drying_icon(drying, true, 0));
    CHECK_FALSE(shows_drying_icon(drying, false, 0)); // firmware without remote dry: status not trusted
    DryState cooling;
    cooling.has_status = true;
    cooling.status     = int(DryStatus::Cooling);
    CHECK_FALSE(shows_drying_icon(cooling, true, 30));
}

TEST_CASE("Hardware limits per heated unit", "[AmsDrying]")
{
    using namespace AmsDrying;
    Limits l;
    REQUIRE(limits_for(AmsDual::UNIT_N3F, l)); // AMS 2 Pro
    CHECK(l.min_temp == 45);
    CHECK(l.max_temp == 65);
    CHECK(l.min_hours == 1);
    CHECK(l.max_hours == 24);
    REQUIRE(limits_for(AmsDual::UNIT_N3S, l)); // AMS HT
    CHECK(l.min_temp == 45);
    CHECK(l.max_temp == 85);
    CHECK_FALSE(limits_for(AmsDual::UNIT_AMS, l));
}

TEST_CASE("Drying presets are Bambu's, keyed by filament type", "[AmsDrying]")
{
    using namespace AmsDrying;
    const Preset *pla = preset_for_type("PLA");
    REQUIRE(pla);
    CHECK(preset_temp(*pla, AmsDual::UNIT_N3F, false) == 45);
    CHECK(preset_hours(*pla, AmsDual::UNIT_N3S, false) == 12);
    CHECK(preset_fully_dries_on(*pla, AmsDual::UNIT_N3F));
    CHECK(preset_fully_dries_on(*pla, AmsDual::UNIT_N3S));

    const Preset *abs = preset_for_type("abs"); // case does not matter
    REQUIRE(abs);
    CHECK(preset_temp(*abs, AmsDual::UNIT_N3F, false) == 65);
    CHECK(preset_temp(*abs, AmsDual::UNIT_N3S, false) == 80);
    CHECK(preset_temp(*abs, AmsDual::UNIT_N3S, true) == 75);
    CHECK(preset_hours(*abs, AmsDual::UNIT_N3S, false) == 8);
    CHECK_FALSE(preset_fully_dries_on(*abs, AmsDual::UNIT_N3F)); // limitations ["1"]: AMS HT only
    CHECK(preset_fully_dries_on(*abs, AmsDual::UNIT_N3S));

    const Preset *tpu = preset_for_type("TPU");
    REQUIRE(tpu);
    CHECK(preset_hours(*tpu, AmsDual::UNIT_N3S, false) == 18);

    CHECK(preset_for_type("PLA-CF") != pla);        // its own entry
    CHECK(preset_for_type("PLA-S") == pla);         // family fallback
    CHECK(preset_for_type("Sup.PLA") == nullptr);
    CHECK(preset_for_type("") == nullptr);
    CHECK(cooling_temp_for("ABS") == 80);           // softening temperature
    CHECK(cooling_temp_for("PLA") == 50);
    CHECK(cooling_temp_for("MYSTERY") == 50);       // Bambu's default
    CHECK(&fallback_preset() == pla);

    const auto types = preset_types();
    CHECK(types.front() == "PLA");
    CHECK(std::find(types.begin(), types.end(), "PPA-CF") != types.end());
}

TEST_CASE("The recommended temperature is the lowest over what is loaded", "[AmsDrying]")
{
    using namespace AmsDrying;
    std::string def;
    CHECK(recommended_temp(AmsDual::UNIT_N3S, {"PETG", "ABS", "PLA"}, false, &def) == 45);
    CHECK(def == "PLA");
    CHECK(recommended_temp(AmsDual::UNIT_N3S, {"ABS"}, false, &def) == 80);
    CHECK(def == "ABS");
    // Printing: min(print temperature, softening, heat distortion) - ABS on AMS HT: min(75, 80, 90).
    CHECK(recommended_temp(AmsDual::UNIT_N3S, {"ABS"}, true, &def) == 75);
    // PETG on AMS 2 Pro while printing: min(55, 60, 75).
    CHECK(recommended_temp(AmsDual::UNIT_N3F, {"PETG"}, true) == 55);
    // Nothing loaded, an empty type or an unknown one: PLA.
    CHECK(recommended_temp(AmsDual::UNIT_N3F, {}, false, &def) == 45);
    CHECK(def == "PLA");
    CHECK(recommended_temp(AmsDual::UNIT_N3S, {"", "ABS"}, false, &def) == 45);
    CHECK(recommended_temp(AmsDual::UNIT_N3S, {"UNOBTAINIUM"}, false, &def) == 45);
}

TEST_CASE("Start is refused outside Bambu's limits", "[AmsDrying]")
{
    using namespace AmsDrying;
    const int N3F = AmsDual::UNIT_N3F, N3S = AmsDual::UNIT_N3S;

    // Within limits, nothing to say.
    Check c = check_request(N3F, "PLA", 45, 12, false, 45, true);
    CHECK(c.can_start);
    CHECK(c.issues.empty());

    // Hardware range.
    c = check_request(N3F, "ABS", 70, 8, false, 65, false);
    CHECK_FALSE(c.can_start);
    CHECK(has_issue(c, Issue::TempAboveMax));
    c = check_request(N3S, "ABS", 85, 8, false, 80, false);
    CHECK(c.can_start);
    c = check_request(N3S, "ABS", 86, 8, false, 80, false);
    CHECK(has_issue(c, Issue::TempAboveMax));
    c = check_request(N3S, "PLA", 44, 8, false, 45, false);
    CHECK_FALSE(c.can_start);
    CHECK(has_issue(c, Issue::TempBelowMin));

    // A filament this unit cannot fully dry: a warning, not a refusal.
    c = check_request(N3F, "ABS", 65, 12, false, 65, false);
    CHECK(c.can_start);
    CHECK(has_issue(c, Issue::MayNotFullyDry));
    c = check_request(N3F, "WHATEVER", 50, 12, false, 45, false);
    CHECK(has_issue(c, Issue::MayNotFullyDry));

    // Hours.
    c = check_request(N3F, "PLA", 45, 0, false, 45, false);
    CHECK_FALSE(c.can_start);
    CHECK(has_issue(c, Issue::HoursBelowMin));
    c = check_request(N3F, "PLA", 45, 25, false, 45, false);
    CHECK_FALSE(c.can_start);
    CHECK(has_issue(c, Issue::HoursAboveMax));
    CHECK(check_request(N3F, "PLA", 45, 24, false, 45, false).can_start);
    CHECK(check_request(N3F, "PLA", 45, 1, false, 45, false).can_start);

    // Heat distortion only matters with filament inserted: PLA's is 45 degC.
    c = check_request(N3S, "PLA", 55, 12, false, 45, true);
    CHECK_FALSE(c.can_start);
    CHECK(has_issue(c, Issue::AboveHeatDistortion));
    CHECK(c.heat_distortion == 45);
    CHECK(check_request(N3S, "PLA", 55, 12, false, 45, false).can_start);

    // While the unit feeds the print, the recommended temperature is the cap.
    c = check_request(N3S, "ABS", 80, 8, true, 75, true);
    CHECK_FALSE(c.can_start);
    CHECK(has_issue(c, Issue::AbovePrintingLimit));
    CHECK(check_request(N3S, "ABS", 75, 8, true, 75, true).can_start);

    // No heater, no drying.
    CHECK_FALSE(check_request(AmsDual::UNIT_AMS, "PLA", 45, 12, false, 45, false).can_start);
}

TEST_CASE("Start and Stop send exactly what Bambu Studio sends", "[AmsDrying]")
{
    using namespace AmsDrying;
    // DevFilaSystem::CtrlAmsStartDryingHour(ams_id, filament_type, temp, hours, rotate_tray, cooling_temp, false).
    CHECK(build_start("42", 128, "PLA", 45, 12, true, 50) == json::parse(R"({"print":{
        "command":"ams_filament_drying","sequence_id":"42","ams_id":128,"mode":1,"filament":"PLA",
        "temp":45,"duration":12,"humidity":0,"rotate_tray":true,"cooling_temp":50,"close_power_conflict":false}})"));

    CHECK(build_start("7", 0, "ABS", 65, 8, false, cooling_temp_for("ABS")) == json::parse(R"({"print":{
        "command":"ams_filament_drying","sequence_id":"7","ams_id":0,"mode":1,"filament":"ABS",
        "temp":65,"duration":8,"humidity":0,"rotate_tray":false,"cooling_temp":80,"close_power_conflict":false}})"));

    // DevFilaSystem::CtrlAmsStopDrying(ams_id).
    CHECK(build_stop("43", 129) == json::parse(R"({"print":{
        "command":"ams_filament_drying","sequence_id":"43","ams_id":129,"mode":0,"filament":"",
        "temp":0,"duration":0,"humidity":0,"rotate_tray":false,"cooling_temp":0,"close_power_conflict":false}})"));

    // Types matter to the firmware's parser: numbers stay numbers, booleans stay booleans.
    const json j = build_start("1", 3, "PETG", 65, 12, false, 60);
    CHECK(j["print"]["ams_id"].is_number_integer());
    CHECK(j["print"]["mode"].is_number_integer());
    CHECK(j["print"]["rotate_tray"].is_boolean());
    CHECK(j["print"]["sequence_id"].is_string());
    CHECK(j["print"].size() == 11);
}
