#include <catch2/catch.hpp>

#include "libslic3r/BambuDualNozzleSync.hpp"
#include "libslic3r/BambuNozzleMappingRequest.hpp"

#include "nlohmann/json.hpp"

using namespace Slic3r;
using namespace Slic3r::DualNozzleSync;

namespace {

// Mock printer reports. Physical extruder 0 is the main (right) extruder; the H2C/H2D profiles
// carry physical_extruder_map [1, 0].
const std::vector<int> H2_MAP{ 1, 0 };

Tray tray(int ams, int slot, const char *color, const char *type = "PLA", const char *id = "PMPL07")
{
    return Tray{ ams, slot, color, type, id };
}

PrinterNozzle nozzle(int pos, const char *dia = "0.4", NozzleVolumeType v = nvtStandard, bool normal = true)
{
    PrinterNozzle n;
    n.pos      = pos;
    n.diameter = dia;
    n.volume   = v;
    n.normal   = normal;
    n.wear     = 0;
    return n;
}

// The owner's H2C ("3DPO", 2026-09-23): a one-slot AMS HT on the left, two four-slot AMS on the
// right, 0.4 standard nozzles on both extruders and five more in the right extruder's rack.
PrinterState h2c_state()
{
    PrinterState s;
    s.dev_id                = "31B8AK5C0900288";
    s.dev_name              = "3DPO";
    s.printer_type          = "O1C2";
    s.has_report            = true;
    s.physical_extruder_map = H2_MAP;
    s.ams.push_back({ 128, 1, 1, { tray(128, 0, "057748FF") } });
    s.ams.push_back({ 0, 0, 4, { tray(0, 0, "FFFFFFFF"), tray(0, 1, "0D6284FF"), tray(0, 2, "DE4343FF"), tray(0, 3, "FFF144FF") } });
    s.ams.push_back({ 1, 0, 4, { tray(1, 0, "000000FF"), tray(1, 1, "9B9EA0FF"), tray(1, 2, "AE835BFF"), tray(1, 3, "56B7E6FF") } });
    s.nozzles = { nozzle(0), nozzle(1) };
    for (int n = 0; n < 5; ++n)
        s.nozzles.push_back(nozzle(0x10 + n));
    return s;
}

// An H2D with one four-slot AMS on each extruder, standard left / high flow right.
PrinterState h2d_state()
{
    PrinterState s;
    s.dev_id                = "0948AD561201535";
    s.printer_type          = "O1D";
    s.has_report            = true;
    s.physical_extruder_map = H2_MAP;
    s.ams.push_back({ 0, 0, 4, { tray(0, 0, "161616FF"), tray(0, 1, "FFFFFFFF") } });
    s.ams.push_back({ 1, 1, 4, { tray(1, 0, "898989FF"), tray(1, 2, "FF0000FF") } });
    s.nozzles = { nozzle(0, "0.4", nvtHighFlow), nozzle(1) };
    return s;
}

// The reference project (h2c_wrongextruder_*.3mf): ten filaments, plate 4 uses 4, 8 and 10.
const std::vector<std::string> PROJECT_COLOURS{ "#FFFFFF", "#000000", "#DE4343", "#F7D959", "#0078BF",
                                                "#56B7E6", "#AE835B", "#042F56", "#9B9EA0", "#5D989E" };

std::vector<ProjectFilament> plate4_used()
{
    std::vector<ProjectFilament> used;
    for (int idx : { 3, 7, 9 })
        used.push_back({ idx, PROJECT_COLOURS[size_t(idx)], "PLA", "PMPL07" });
    return used;
}

} // namespace

TEST_CASE("Sync writes extruder_ams_count in Bambu Studio's format", "[DualNozzleSync]")
{
    // h2c_wrongextruder_bambu_manual.gcode.3mf project_settings: ["1#1|4#0", "1#0|4#2"].
    CHECK(extruder_ams_count_strings(h2c_state(), 2) == std::vector<std::string>{ "1#1|4#0", "1#0|4#2" });
    CHECK(extruder_ams_count_strings(h2d_state(), 2) == std::vector<std::string>{ "1#0|4#1", "1#0|4#1" });

    // External spool holders are not AMS units.
    PrinterState s = h2c_state();
    s.ams.push_back({ 254, 0, 1, { tray(254, 0, "FFFFFFFF") } });
    CHECK(extruder_ams_count_strings(s, 2) == std::vector<std::string>{ "1#1|4#0", "1#0|4#2" });

    // Without the profile's map the report's physical ids would be read as logical ones and the
    // two sides swapped - the bug #111 fixed in the slice path.
    s.physical_extruder_map.clear();
    CHECK(extruder_ams_count_strings(s, 2) == std::vector<std::string>{ "1#0|4#2", "1#1|4#0" });
}

TEST_CASE("Sync writes extruder_nozzle_stats from the extruder nozzles and the rack", "[DualNozzleSync]")
{
    // Bambu manual file: ["Standard#1", "Standard#6"].
    CHECK(extruder_nozzle_stats_strings(h2c_state(), 2, { 0.4, 0.4 }) == std::vector<std::string>{ "Standard#1", "Standard#6" });
    CHECK(extruder_nozzle_stats_strings(h2d_state(), 2, { 0.4, 0.4 }) == std::vector<std::string>{ "Standard#1", "High Flow#1" });

    // An abnormal rack nozzle, or one of another diameter, is not usable for this slice.
    PrinterState s = h2c_state();
    s.nozzles[2].normal   = false;
    s.nozzles[3].diameter = "0.6";
    CHECK(extruder_nozzle_stats_strings(s, 2, { 0.4, 0.4 }) == std::vector<std::string>{ "Standard#1", "Standard#4" });
    // A high flow nozzle in the rack makes a two-type entry, sorted as Bambu's map sorts it.
    s.nozzles[4].volume = nvtHighFlow;
    CHECK(extruder_nozzle_stats_strings(s, 2, { 0.4, 0.4 }) == std::vector<std::string>{ "Standard#1", "Standard#3|High Flow#1" });

    // The strings parse back with the slicer's own reader.
    const auto parsed = get_extruder_nozzle_stats({ "Standard#1", "Standard#6" });
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[1].at(nvtStandard) == 6);
    const auto ams = get_extruder_ams_count({ "1#1|4#0", "1#0|4#2" });
    REQUIRE(ams.size() == 2);
    CHECK(ams[0].at(1) == 1);
    CHECK(ams[1].at(4) == 2);
}

TEST_CASE("State fingerprint follows what matters and ignores report noise", "[DualNozzleSync]")
{
    const PrinterState a = h2c_state();
    PrinterState       b = h2c_state();
    CHECK(state_fingerprint(a) == state_fingerprint(b));
    // Colour spelling does not matter.
    b.ams[1].trays[0].color = "#ffffff";
    CHECK(state_fingerprint(a) == state_fingerprint(b));
    // A spool swapped for another colour does.
    b.ams[1].trays[0].color = "00FF00FF";
    CHECK(state_fingerprint(a) != state_fingerprint(b));
    // So does a removed rack nozzle, or another printer.
    PrinterState c = h2c_state();
    c.nozzles.pop_back();
    CHECK(state_fingerprint(a) != state_fingerprint(c));
    PrinterState d = h2c_state();
    d.dev_id = "other";
    CHECK(state_fingerprint(a) != state_fingerprint(d));
    // No report: no fingerprint.
    PrinterState off;
    CHECK(state_fingerprint(off).empty());
}

TEST_CASE("Manual grouping 10 left, 4 and 8 right gives Bambu's filament_maps", "[DualNozzleSync]")
{
    const PrinterState s    = h2c_state();
    const auto         used = plate4_used();

    // What the user drags in the dialog, starting from a project map of all-right.
    Arrangement arr;
    arr.filament_map.assign(10, 2);
    arr.filament_map[9] = 1;
    arr.trays[3]        = { 0, 3 };
    arr.trays[7]        = { 0, 1 };
    arr.trays[9]        = { 128, 0 };
    // h2c_wrongextruder_bambu_manual.gcode.3mf, plate 4: filament_map_mode Manual,
    // filament_maps "2 2 2 2 2 2 2 2 2 1".
    CHECK(arr.filament_map == std::vector<int>{ 2, 2, 2, 2, 2, 2, 2, 2, 2, 1 });
    CHECK(validate_arrangement(arr, s, used, 2, { 0.4, 0.4 }).empty());

    // Moving F10 back to the right while its tray stays the left AMS HT is flagged.
    arr.filament_map[9] = 2;
    const auto issues = validate_arrangement(arr, s, used, 2, { 0.4, 0.4 });
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].kind == Issue::Kind::TrayOnOtherExtruder);
    CHECK(issues[0].filament == 9);

    // A tray that is not loaded any more.
    arr.filament_map[9] = 1;
    arr.trays[9]        = { 129, 0 };
    const auto missing = validate_arrangement(arr, s, used, 2, { 0.4, 0.4 });
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].kind == Issue::Kind::TrayMissing);

    // A side with no usable nozzle.
    PrinterState no_left = s;
    no_left.nozzles.erase(no_left.nozzles.begin() + 1); // pos 1 = the left extruder's nozzle
    arr.trays[9] = { 128, 0 };
    const auto no_nozzle = validate_arrangement(arr, no_left, used, 2, { 0.4, 0.4 });
    REQUIRE(no_nozzle.size() == 1);
    CHECK(no_nozzle[0].kind == Issue::Kind::NoNozzleOnExtruder);
    CHECK(no_nozzle[0].extruder == 0);
}

TEST_CASE("Pre-fill matches trays by colour and flags an all-on-one-side result", "[DualNozzleSync]")
{
    const PrinterState s    = h2c_state();
    const auto         used = plate4_used();
    const Arrangement  arr  = propose_arrangement(s, used, 10, std::vector<int>(10, 2));

    // F4 (yellow) and F8 (dark blue) find their spools in the right-side AMS.
    CHECK(arr.trays.at(3) == TrayRef{ 0, 3 });
    CHECK(arr.trays.at(7) == TrayRef{ 0, 1 });
    // F10 (#5D989E, grey teal) is closer to the right AMS's grey (#9B9EA0) than to its own dark
    // green spool (#057748) in the left AMS HT: colour matching alone puts all three on the right,
    // which is what went wrong on the owner's printer. The dialog shows a hint for this.
    CHECK(arr.trays.at(9) == TrayRef{ 1, 1 });
    CHECK(arr.filament_map[9] == 2);
    CHECK(all_on_one_side_with_trays_on_both(arr, s, used));
    // Unused filaments keep the prior map.
    CHECK(arr.filament_map[0] == 2);

    // A spool whose colour is close enough goes to the extruder that feeds it.
    PrinterState closer = s;
    closer.ams[0].trays[0].color = "5A969CFF"; // the AMS HT's spool
    const Arrangement arr2 = propose_arrangement(closer, used, 10, std::vector<int>(10, 2));
    CHECK(arr2.trays.at(9) == TrayRef{ 128, 0 });
    CHECK(arr2.filament_map == std::vector<int>{ 2, 2, 2, 2, 2, 2, 2, 2, 2, 1 });
    CHECK_FALSE(all_on_one_side_with_trays_on_both(arr2, closer, used));

    // A different filament type is avoided while a same-type spool exists.
    PrinterState typed = s;
    typed.ams[1].trays[3].type = "PETG";     // AMS 0 slot 4, the yellow spool
    typed.ams[2].trays[2].color = "F0D050FF"; // AMS 1 slot 3, a yellow PLA
    const Arrangement arr3 = propose_arrangement(typed, used, 10, {});
    CHECK(arr3.trays.at(3) == TrayRef{ 1, 2 });

    // Offline: the prior map is returned untouched, no trays.
    PrinterState off;
    std::vector<int> prior(10, 2);
    prior[9] = 1;
    const Arrangement arr4 = propose_arrangement(off, used, 10, prior);
    CHECK(arr4.filament_map == prior);
    CHECK(arr4.trays.empty());
}

TEST_CASE("Confirmation is remembered until something relevant changes", "[DualNozzleSync]")
{
    const PrinterState s    = h2c_state();
    const auto         used = plate4_used();
    std::vector<int>   map(10, 2);
    map[9] = 1;

    Confirmation c;
    CHECK(needs_confirmation(c, s, used, map) == ConfirmReason::NeverConfirmed);

    c.dev_id       = s.dev_id;
    c.state_fp     = state_fingerprint(s);
    c.filaments_fp = filaments_fingerprint(used);
    c.filament_map = map;
    c.trays        = { { 3, { 0, 3 } }, { 7, { 0, 1 } }, { 9, { 128, 0 } } };
    c.synced       = true;
    CHECK(needs_confirmation(c, s, used, map) == ConfirmReason::None);

    // Round trip through the project file.
    const Confirmation back = Confirmation::deserialize(c.serialize());
    CHECK(back.dev_id == c.dev_id);
    CHECK(back.state_fp == c.state_fp);
    CHECK(back.filament_map == c.filament_map);
    CHECK(back.trays.at(9) == TrayRef{ 128, 0 });
    CHECK(back.synced);
    CHECK(needs_confirmation(back, s, used, map) == ConfirmReason::None);
    CHECK(Confirmation::deserialize("garbage").empty());

    // Offline later: no nag.
    PrinterState off;
    CHECK(needs_confirmation(c, off, used, map) == ConfirmReason::None);

    PrinterState other = s;
    other.dev_id = "0948AD561201535";
    CHECK(needs_confirmation(c, other, used, map) == ConfirmReason::PrinterChanged);

    PrinterState swapped = s;
    swapped.ams[1].trays[3].color = "00FF00FF";
    CHECK(needs_confirmation(c, swapped, used, map) == ConfirmReason::PrinterStateChanged);

    auto recoloured = used;
    recoloured[0].color = "#FF0000";
    CHECK(needs_confirmation(c, s, recoloured, map) == ConfirmReason::FilamentsChanged);

    auto map2 = map;
    map2[3]   = 1;
    CHECK(needs_confirmation(c, s, used, map2) == ConfirmReason::MapChanged);
    CHECK(needs_confirmation(c, s, used, {}) == ConfirmReason::MapChanged);

    // Confirmed while offline, printer online now.
    Confirmation offline = c;
    offline.dev_id.clear();
    offline.state_fp.clear();
    offline.synced = false;
    CHECK(needs_confirmation(offline, off, used, map) == ConfirmReason::None);
    CHECK(needs_confirmation(offline, s, used, map) == ConfirmReason::NowSynced);
}

TEST_CASE("A printer change invalidates dual-nozzle slices", "[DualNozzleSync]")
{
    const PrinterState s  = h2c_state();
    const std::string  fp = state_fingerprint(s);
    CHECK_FALSE(slice_invalidated_by(s.dev_id, fp, s));
    // Going offline does not.
    CHECK_FALSE(slice_invalidated_by(s.dev_id, fp, PrinterState()));
    // Another printer selected in the Device tab does.
    CHECK(slice_invalidated_by(s.dev_id, fp, h2d_state()));
    // A spool change on the same printer does.
    PrinterState changed = s;
    changed.ams[2].trays.pop_back();
    CHECK(slice_invalidated_by(s.dev_id, fp, changed));
    // A slice made without printer data is invalidated once a printer reports.
    CHECK(slice_invalidated_by("", "", s));
}

TEST_CASE("Nozzle mapping request lists the rack and matches Bambu Studio's shape", "[DualNozzleSync][NozzleMapping]")
{
    using namespace Slic3r::BambuNozzleMapping;
    const PrinterState s = h2c_state();

    RequestInput in;
    in.calibration              = 1;
    in.extrude_cali_manual_mode = 1;
    in.filament_change_sequence = { 9, 3, 7, 3 }; // 0-based ids in print order
    in.mapped = { { 3, "0", "3", 3, "PMPL07", "#F7D959FF" }, { 7, "0", "1", 1, "PMPL07", "#042F56FF" }, { 9, "128", "0", 512, "PMPL07", "#5D989EFF" } };
    // The manual grouping: F10 on the left nozzle, F4/F8 on the right rack.
    in.filament_nozzles = { { 3, 1, 1, "0.4", nvtStandard }, { 7, 1, 2, "0.4", nvtStandard }, { 9, 0, 0, "0.4", nvtStandard } };
    in.printer_nozzles       = s.nozzles;
    in.physical_extruder_map = H2_MAP;
    in.preset_diameters      = { 0.4, 0.4 };

    const auto j = nlohmann::json::parse(build_v0_request(in))["print"];
    CHECK(j["command"] == "get_auto_nozzle_mapping");
    // filament_seq is an array indexed by 1-based filament id (Bambu builds it by integer index
    // on a null json); the fork used to send an object keyed by strings.
    REQUIRE(j["filament_seq"].is_array());
    CHECK(j["filament_seq"] == nlohmann::json({ -1, -1, -1, -1, 1, -1, -1, -1, 2, -1, 0 }));
    CHECK(j["ams_mapping"].size() == 33);
    CHECK(j["ams_mapping"][4] == 3);
    CHECK(j["ams_mapping"][8] == 1);
    CHECK(j["ams_mapping"][10] == (128 << 8));
    CHECK(j["ams_mapping"][1] == 0xFFFF);

    REQUIRE(j["fila_info"].size() == 3);
    CHECK(j["fila_info"][2]["id"] == 10);
    CHECK(j["fila_info"][2]["direction"] == 1);
    CHECK(j["fila_info"][2]["nozzle_d"] == "0.40");
    CHECK(j["fila_info"][0]["direction"] == 2);
    CHECK(j["fila_info"][0]["cate"] == "PMPL07");

    // nozzle_info: extruder nozzles (pos 0 = right/main, 1 = left) then the five rack nozzles
    // at 0x10 + n; the request the H2C refused on 2026-09-23 listed only the two extruders.
    const auto &noz = j["nozzle_info"];
    REQUIRE(noz.size() == 7);
    CHECK(noz[0]["pos"] == 0);
    CHECK(noz[1]["pos"] == 1);
    CHECK(noz[2]["pos"] == 0x10);
    CHECK(noz[6]["pos"] == 0x14);
    CHECK(noz[2]["nozzle_d"] == "0.40");
    CHECK(noz[2]["nozzle_v"] == "Standard");
    CHECK(noz[2].contains("wear"));
    CHECK(noz[2].contains("cate"));
    CHECK(noz[2].contains("color"));

    // An abnormal rack nozzle is left out, as Bambu's IsNormal() filter does.
    in.printer_nozzles[3].normal = false;
    CHECK(nlohmann::json::parse(build_v0_request(in))["print"]["nozzle_info"].size() == 6);

    // No nozzle report: the preset's two nozzles as before (left first, pos = physical id).
    in.printer_nozzles.clear();
    const auto fb = nlohmann::json::parse(build_v0_request(in))["print"]["nozzle_info"];
    REQUIRE(fb.size() == 2);
    CHECK(fb[0]["pos"] == 1);
    CHECK(fb[1]["pos"] == 0);
}

TEST_CASE("A refused or unanswered nozzle mapping never blocks Send", "[DualNozzleSync][NozzleMapping]")
{
    using namespace Slic3r::BambuNozzleMapping;
    // The H2D answered result=fail errno=1 to every query on 2026-09-23 (one black PLA on its own
    // extruder); jobs sent without nozzle_mapping print fine, so a refusal is a warning only.
    CHECK(reply_is_refusal("fail"));
    CHECK(reply_is_refusal("failed"));
    CHECK(reply_is_refusal("FAIL"));
    CHECK_FALSE(reply_is_refusal("success"));
    CHECK_FALSE(reply_is_refusal(""));

    for (QueryState s : { QueryState::None, QueryState::Waiting, QueryState::Accepted, QueryState::Refused, QueryState::NoAnswer })
        CHECK(send_gate(s).send_enabled);

    CHECK(send_gate(QueryState::Refused).warn);
    CHECK_FALSE(send_gate(QueryState::Refused).note);
    CHECK(send_gate(QueryState::Waiting).note);
    CHECK_FALSE(send_gate(QueryState::Waiting).warn);
    CHECK_FALSE(send_gate(QueryState::NoAnswer).warn);
    CHECK_FALSE(send_gate(QueryState::Accepted).warn);
    CHECK_FALSE(send_gate(QueryState::None).warn);
}

TEST_CASE("Nozzle mapping request for the refused one-filament H2D job", "[DualNozzleSync][NozzleMapping]")
{
    using namespace Slic3r::BambuNozzleMapping;
    // tests/h2d_sendrefuse.gcode.3mf: filament 2 (0-based 1), black PLA GFL01, sliced for the right
    // extruder, AMS A2 (ams 0 slot 1); H2D reports two 0.4 Standard extruder nozzles.
    RequestInput in;
    in.calibration              = 1;
    in.extrude_cali_manual_mode = 1;
    in.filament_change_sequence = { 1 };
    in.mapped                   = { { 1, "0", "1", 1, "GFL01", "161616FF" } };
    in.filament_nozzles         = { { 1, 1, 1, "0.4", nvtStandard } };
    DualNozzleSync::PrinterNozzle right, left;
    right.pos = 0; right.diameter = "0.4"; right.normal = true; right.color = "00000000";
    left.pos  = 1; left.diameter  = "0.4"; left.normal  = true; left.color  = "00000000";
    in.printer_nozzles       = { left, right };
    in.physical_extruder_map = { 1, 0 };
    in.preset_diameters      = { 0.4, 0.4 };

    // Golden: the request the send dialog logged for this plate (seq 20031, 2026-09-23 19:42),
    // which has BambuStudio's V0 shape. The H2D refused it; BambuStudio never sends it to an H2D
    // (no nozzle rack), see query_applies.
    const auto golden = nlohmann::json::parse(R"({"print":{"ams_mapping":[65535,65535,1,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535,65535],"calibration":1,"command":"get_auto_nozzle_mapping","extrude_cali_manual_mode":1,"fila_info":[{"cate":"GFL01","color":"161616FF","direction":2,"group":1,"id":2,"nozzle_d":"0.40","nozzle_v":"Standard"}],"filament_seq":[-1,-1,0],"nozzle_info":[{"cate":"","color":"00000000","nozzle_d":"0.40","nozzle_v":"Standard","pos":0,"wear":0.0},{"cate":"","color":"00000000","nozzle_d":"0.40","nozzle_v":"Standard","pos":1,"wear":0.0}],"sequence_id":"0"}})");
    const auto j = nlohmann::json::parse(build_v0_request(in));
    CHECK(j == golden);
}

TEST_CASE("Only printers with a nozzle rack are asked for a nozzle mapping", "[DualNozzleSync][NozzleMapping]")
{
    using namespace Slic3r::BambuNozzleMapping;
    // BambuStudio gates CheckErrorSyncNozzleMappingResultV0/V1 on DevNozzleRack::IsSupported()
    // (fun bit 60): the H2C is asked, the H2D never is (it answers result=fail errno=1).
    QueryConditions h2c;
    h2c.sliced_send        = true;
    h2c.dual_nozzle_preset = true;
    h2c.printer_has_rack   = true;
    h2c.right_nozzle_used  = true;
    h2c.has_ams_mapping    = true;
    CHECK(query_applies(h2c));

    QueryConditions h2d = h2c;
    h2d.printer_has_rack = false;
    CHECK_FALSE(query_applies(h2d));

    QueryConditions left_only = h2c;
    left_only.right_nozzle_used = false;
    CHECK_FALSE(query_applies(left_only));

    QueryConditions reprint = h2c;
    reprint.sliced_send = false;
    CHECK_FALSE(query_applies(reprint));

    QueryConditions no_ams = h2c;
    no_ams.has_ams_mapping = false;
    CHECK_FALSE(query_applies(no_ams));

    QueryConditions single = h2c;
    single.dual_nozzle_preset = false;
    CHECK_FALSE(query_applies(single));
}
