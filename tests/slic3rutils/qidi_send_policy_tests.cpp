// Starting a print on a QIDI Box printer (PrintHostDevices::qidi_send_policy and friends).
//
// The bug these pin down (2.4.6.2): a plate sliced as PLA, PLA and ASA in the Box, and the print
// started with no Box mapping sent - from the hub / phone / Stream tab send, or from the desktop
// send dialog when the Box could not be read - so the firmware fed T0 from the slot its saved
// value_t0 still named from the last print: the ASA, at PLA settings. Every start now goes through
// qidi_send_policy(), and only Start lets a print begin.
//
// Pure functions, no network: nothing here talks to a printer.

#include <catch2/catch.hpp>

#include "slic3r/Utils/PrintHostDeviceStatus.hpp"

#include <string>
#include <vector>

using namespace Slic3r::PrintHostDevices;

namespace {

// An X-Max 4 with one Box: slot 1 PLA white, slot 2 ASA black, slots 3 and 4 empty.
Status qidi_box_pla_asa()
{
    const std::string status =
        "{\"print_stats\":{\"state\":\"standby\"},"
        " \"multi_color_controller\":{\"box_count\":1,\"box\":{\"boxes\":[{"
        "   \"box_inserted_0\":1,\"box_inserted_1\":1,\"box_inserted_2\":0,\"box_inserted_3\":0,"
        "   \"filament_info\":["
        "     {\"filament_type\":\"PLA\",\"filament_str\":\"PLA Rapido\",\"vendor_str\":\"QIDI\",\"color_rgb_str\":[\"#FFFFFF\"]},"
        "     {\"filament_type\":\"ASA\",\"filament_str\":\"ASA\",\"vendor_str\":\"QIDI\",\"color_rgb_str\":[\"#202020\"]},"
        "     {},{}]}]}}}";
    return parse_moonraker_status(status, "octoprint");
}

SendFilament filament(int index, const std::string& type, const std::string& colour, bool used = true)
{
    SendFilament f;
    f.index  = index;
    f.type   = type;
    f.colour = colour;
    f.used   = used;
    return f;
}

// A complete, matching mapping on a reachable, idle QIDI with a Box.
QidiSendFacts good_facts()
{
    QidiSendFacts f;
    f.model_may_have_box = true;
    f.probe_ok           = true;
    f.qidi               = true;
    f.box_present        = true;
    f.busy               = false;
    f.use_box            = true;
    f.used_filaments     = 1;
    f.mapped_filaments   = 1;
    f.type_mismatches    = 0;
    return f;
}

} // namespace

TEST_CASE("qidi_box_capable_model names the QIDIs that take a QIDI Box", "[qidi][send]")
{
    SECTION("Box-capable models, however they are spelled") {
        for (const char* m : { "Qidi X-Max 4", "Qidi X-Plus 4", "Qidi Q2", "Qidi Q2C", "Qidi X-Plus 5", "QIDI XMax4", "qidi x-plus 4" })
            DYNAMIC_SECTION(m) { CHECK(qidi_box_capable_model(m)); }
    }
    SECTION("QIDIs without a Box, and other brands") {
        for (const char* m : { "Qidi Q1 Pro", "Qidi X-Max 3", "Qidi X-Plus 3", "Qidi X-Smart 3", "Qidi X-CF Pro", "Qidi X-Max",
                               "Snapmaker U1", "Elegoo Centauri Carbon", "Bambu Lab X1 Carbon", "" })
            DYNAMIC_SECTION("'" << m << "'") { CHECK_FALSE(qidi_box_capable_model(m)); }
    }
    SECTION("the family alone is not a QIDI") {
        CHECK_FALSE(qidi_box_capable_model("Generic Q2"));
        CHECK_FALSE(qidi_box_capable_model("Voron Max4"));
    }
}

TEST_CASE("filament_types_match compares materials, not spelling", "[qidi][send]")
{
    CHECK(filament_types_match("PLA", "PLA"));
    CHECK(filament_types_match("pla", "PLA"));
    CHECK(filament_types_match(" PLA ", "PLA"));
    CHECK_FALSE(filament_types_match("PLA", "ASA"));
    CHECK_FALSE(filament_types_match("PLA", ""));
    CHECK_FALSE(filament_types_match("", "PLA"));
    CHECK_FALSE(filament_types_match("", ""));
}

TEST_CASE("qidi_send_policy only starts a QIDI Box print on a complete, checked mapping", "[qidi][send]")
{
    SECTION("complete and matching: start") {
        CHECK(qidi_send_policy(good_facts()) == QidiSendPolicy::Start);
    }
    SECTION("the reported bug: a used filament left without a slot is never started") {
        QidiSendFacts f    = good_facts();
        f.mapped_filaments = 0;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::NeedsMapping);
    }
    SECTION("two used filaments, one mapped") {
        QidiSendFacts f    = good_facts();
        f.used_filaments   = 2;
        f.mapped_filaments = 1;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::NeedsMapping);
    }
    SECTION("no used filament known is not nothing to map: T0 still reads value_t0") {
        QidiSendFacts f    = good_facts();
        f.used_filaments   = 0;
        f.mapped_filaments = 0;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::NeedsMapping);
    }
    SECTION("PLA to the ASA slot: ask first") {
        QidiSendFacts f   = good_facts();
        f.type_mismatches = 1;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::ConfirmTypeMismatch);
    }
    SECTION("an unmapped filament outranks a mismatch") {
        QidiSendFacts f    = good_facts();
        f.used_filaments   = 2;
        f.mapped_filaments = 1;
        f.type_mismatches  = 1;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::NeedsMapping);
    }
    SECTION("busy: upload only, whatever the mapping") {
        QidiSendFacts f = good_facts();
        f.busy          = true;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::UploadOnlyBusy);
        f.mapped_filaments = 0;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::UploadOnlyBusy);
    }
    SECTION("the Box switched off for this print: enable_box 0 goes out, nothing to map") {
        QidiSendFacts f    = good_facts();
        f.use_box          = false;
        f.mapped_filaments = 0;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::Start);
    }
    SECTION("QIDI firmware with no Box plugged in") {
        QidiSendFacts f    = good_facts();
        f.box_present      = false;
        f.mapped_filaments = 0;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::Start);
    }
    SECTION("probe failed on a Box-capable model: upload only unless chosen") {
        QidiSendFacts f = good_facts();
        f.probe_ok      = false;
        f.qidi          = false;
        f.box_present   = false;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::UploadOnlyNoProbe);
    }
    SECTION("probe failed on any other model: not ours, behaviour unchanged") {
        QidiSendFacts f      = good_facts();
        f.probe_ok           = false;
        f.qidi               = false;
        f.model_may_have_box = false;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::NotApplicable);
    }
    SECTION("a Box-capable profile whose printer answered without naming the Box: treated as unreadable") {
        QidiSendFacts f = good_facts();
        f.qidi          = false;
        f.box_present   = false;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::UploadOnlyNoProbe);
    }
    SECTION("any other printer that answered but is not QIDI firmware") {
        QidiSendFacts f      = good_facts();
        f.qidi               = false;
        f.box_present        = false;
        f.model_may_have_box = false;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::NotApplicable);
    }
    SECTION("the owner's repro: Qidi X-Max 4 preset, probe answered HTTP 404, plain dialog") {
        // What the dialog had: Status::online false, qidi false, no Box rows, no QIDI options.
        QidiSendFacts f;
        f.model_may_have_box = qidi_box_capable_model("Qidi X-Max 4");
        f.probe_ok           = false;
        f.used_filaments     = 1;
        REQUIRE(f.model_may_have_box);
        CHECK(qidi_send_policy(f) == QidiSendPolicy::UploadOnlyNoProbe);
        CHECK(qidi_send_refusal(qidi_send_policy(f)).find("Couldn't read the QIDI Box") != std::string::npos);
    }
}

TEST_CASE("qidi_auto_mapping maps by material and feeds the policy", "[qidi][send]")
{
    const Status st = qidi_box_pla_asa();
    REQUIRE(st.online);
    REQUIRE(st.qidi);
    REQUIRE(st.qidi_box);

    SECTION("the reported plate: PLA goes to the PLA slot, never the ASA") {
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "PLA", "#FFFFFF") });
        CHECK(am.mapping == "0:0");
        CHECK(am.facts.used_filaments == 1);
        CHECK(am.facts.mapped_filaments == 1);
        CHECK(am.facts.type_mismatches == 0);
        CHECK(am.facts.use_box);
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::Start);
        CHECK(qidi_start_script(QidiPrintOptions { 1, -1, -1 }, am.mapping) ==
              "SAVE_VARIABLE VARIABLE=enable_box VALUE=1\nSAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot0'\"\n");
    }
    SECTION("a black PLA still goes to the white PLA, not the black ASA") {
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "PLA", "#101010") });
        CHECK(am.mapping == "0:0");
    }
    SECTION("PLA and ASA, both loaded") {
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "PLA", "#FFFFFF"), filament(1, "ASA", "#000000") });
        CHECK(am.mapping == "0:0,1:1");
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::Start);
    }
    SECTION("PETG is not in the Box: refused, not left on the stale slot") {
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "PLA", "#FFFFFF"), filament(1, "PETG", "#FF0000") });
        CHECK(am.mapping == "0:0");
        CHECK(am.facts.used_filaments == 2);
        CHECK(am.facts.mapped_filaments == 1);
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::NeedsMapping);
    }
    SECTION("a filament with no material named is never guessed") {
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "", "#FFFFFF") });
        CHECK(am.mapping.empty());
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::NeedsMapping);
    }
    SECTION("unused filaments are not counted") {
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "PLA", "#FFFFFF"), filament(1, "PETG", "#FF0000", false) });
        CHECK(am.facts.used_filaments == 1);
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::Start);
    }
    SECTION("a printer that is printing") {
        Status busy = st;
        busy.state  = "printing";
        const QidiAutoMapping am = qidi_auto_mapping(busy, { filament(0, "PLA", "#FFFFFF") });
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::UploadOnlyBusy);
    }
    SECTION("a printer that did not answer") {
        Status none;
        QidiAutoMapping am          = qidi_auto_mapping(none, { filament(0, "PLA", "#FFFFFF") });
        am.facts.model_may_have_box = true;
        CHECK_FALSE(am.facts.probe_ok);
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::UploadOnlyNoProbe);
        am.facts.model_may_have_box = false;
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::NotApplicable);
    }
    SECTION("a U1 (print_task_config) is not a QIDI: nothing changes for it") {
        const Status u1 = parse_moonraker_status(
            "{\"print_stats\":{\"state\":\"standby\"},\"print_task_config\":{\"filament_type\":[\"PLA\",\"PLA\",\"\",\"\"],"
            "\"filament_color_rgba\":[\"FFFFFFFF\",\"000000FF\",\"\",\"\"],\"filament_exist\":[true,true,false,false]}}",
            "octoprint");
        const QidiAutoMapping am = qidi_auto_mapping(u1, { filament(0, "PLA", "#FFFFFF") });
        CHECK_FALSE(am.facts.qidi);
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::NotApplicable);
    }
}

TEST_CASE("qidi_send_refusal says why for every refusal and nothing otherwise", "[qidi][send]")
{
    CHECK(qidi_send_refusal(QidiSendPolicy::NotApplicable).empty());
    CHECK(qidi_send_refusal(QidiSendPolicy::Start).empty());
    CHECK_FALSE(qidi_send_refusal(QidiSendPolicy::UploadOnlyBusy).empty());
    CHECK_FALSE(qidi_send_refusal(QidiSendPolicy::NeedsMapping).empty());
    CHECK_FALSE(qidi_send_refusal(QidiSendPolicy::ConfirmTypeMismatch).empty());
    const std::string no_probe = qidi_send_refusal(QidiSendPolicy::UploadOnlyNoProbe);
    CHECK(no_probe.find("Couldn't read the QIDI Box") != std::string::npos);
}

// QIDIStudio 2.07 reads the Box over the LAN from save_variables and the printer's own
// officiall_filas_list.cfg, not from multi_color_controller. These are the shapes it reads.
static const char* k_filas_cfg =
    "# QIDI filament list\n"
    "[fila1]\n"
    "filament = PLA Rapido\n"
    "type = PLA\n"
    "[fila5]\n"
    "filament = ASA\n"
    "type = ASA\n"
    "[colordict]\n"
    "1 = #FAFAFA\n"
    "2 = #060606\n";

TEST_CASE("the QIDI filament list parses types, names and colours", "[qidi][send]")
{
    const QidiFilaList l = parse_qidi_filas_cfg(k_filas_cfg);
    CHECK(l.type.at(1) == "PLA");
    CHECK(l.name.at(1) == "PLA Rapido");
    CHECK(l.type.at(5) == "ASA");
    CHECK(l.colour.at(1) == "#FAFAFA");
    CHECK(l.colour.at(2) == "#060606");
    CHECK(l.type.count(2) == 0);
}

TEST_CASE("save_variables give the Box slots when multi_color_controller names no material", "[qidi][send]")
{
    const QidiFilaList l    = parse_qidi_filas_cfg(k_filas_cfg);
    // Box 1: slot 1 PLA white, slot 2 ASA black, slots 3 and 4 empty (their state words 0). value_t0
    // still on the ASA slot from the last print - the state the reported printer was in.
    const std::string vars  = "{\"box_count\":1,\"enable_box\":1,\"value_t0\":\"slot1\","
                              "\"slot0\":1,\"slot1\":1,\"slot2\":0,\"slot3\":0,"
                              "\"filament_slot0\":1,\"color_slot0\":1,\"filament_slot1\":5,\"color_slot1\":2,"
                              "\"filament_slot2\":0,\"filament_slot3\":0}";
    bool              have  = false;
    const std::vector<Slot> slots = slots_from_qidi_variables(vars, l, have);
    REQUIRE(slots.size() == 4);
    CHECK(have);
    CHECK(slots[0].loaded);
    CHECK(slots[0].type == "PLA");
    CHECK(slots[0].color == "#FAFAFA");
    CHECK(slots[1].type == "ASA");
    CHECK(slots[1].index == 1);
    CHECK_FALSE(slots[2].loaded);

    SECTION("through the status parse: QIDI firmware with a Box, even without multi_color_controller") {
        const Status st = parse_moonraker_status("{\"print_stats\":{\"state\":\"standby\"},\"save_variables\":{\"variables\":" + vars + "}}",
                                                 "octoprint");
        CHECK(st.qidi);
        CHECK(st.qidi_box); // box_count 1: a Box, whether or not its slots could be read
        // Without the filament list the materials are unknown, so nothing is mapped and the
        // plate is refused rather than started on value_t0 = slot1 (the ASA).
        SendFilament pla;
        pla.index  = 0;
        pla.type   = "PLA";
        pla.colour = "#FFFFFF";
        pla.used   = true;
        const QidiAutoMapping am = qidi_auto_mapping(st, { pla });
        CHECK(am.mapping.empty());
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::NeedsMapping);
    }
    SECTION("with the slots read, the PLA plate goes to the PLA slot") {
        Status st;
        st.online = st.qidi = st.qidi_box = st.slots_have_filaments = true;
        st.state = "standby";
        st.slots = slots;
        SendFilament pla;
        pla.index  = 0;
        pla.type   = "PLA";
        pla.colour = "#FFFFFF";
        pla.used   = true;
        const QidiAutoMapping am = qidi_auto_mapping(st, { pla });
        CHECK(am.mapping == "0:0");
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::Start);
    }
}

TEST_CASE("an empty or unreadable QIDI Box never defaults to the toolhead spool", "[qidi][send]")
{
    // enable_box 0 prints whatever is in the toolhead, which may be the last print's material.
    Status st;
    st.online = st.qidi = st.qidi_box = true;
    st.state = "standby";
    st.slots.resize(4);
    SendFilament pla;
    pla.type = "PLA";
    pla.used = true;
    const QidiAutoMapping am = qidi_auto_mapping(st, { pla });
    CHECK(am.facts.use_box);
    CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::NeedsMapping);
}

TEST_CASE("the Box mapping is read back before the print starts", "[qidi][send]")
{
    std::string why;
    SECTION("the reported state: value_t0 still on slot1 (ASA) after a PLA send is caught") {
        CHECK_FALSE(qidi_mapping_confirmed("{\"enable_box\":1,\"value_t0\":\"slot1\"}", "0:0", 1, why));
        CHECK(why.find("value_t0") != std::string::npos);
    }
    SECTION("what QIDIStudio's quoting leaves: the string slotN") {
        CHECK(qidi_mapping_confirmed("{\"enable_box\":1,\"value_t0\":\"slot0\",\"value_t1\":\"slot5\"}", "0:0,1:5", 1, why));
    }
    SECTION("enable_box not taken") {
        CHECK_FALSE(qidi_mapping_confirmed("{\"enable_box\":0,\"value_t0\":\"slot0\"}", "0:0", 1, why));
        CHECK_FALSE(qidi_mapping_confirmed("{\"value_t0\":\"slot0\"}", "0:0", 1, why));
    }
    SECTION("Box off: only enable_box 0 matters") {
        CHECK(qidi_mapping_confirmed("{\"enable_box\":0,\"value_t0\":\"slot3\"}", "", 0, why));
        CHECK_FALSE(qidi_mapping_confirmed("{\"enable_box\":1}", "", 0, why));
    }
    SECTION("not JSON") {
        CHECK_FALSE(qidi_mapping_confirmed("<html>", "0:0", 1, why));
    }
}

TEST_CASE("the polar cooler is offered from the profile for the models QIDIStudio offers it on", "[qidi][send]")
{
    for (const char* m : { "Qidi Q2", "Qidi Q2C", "Qidi X-Max 4", "Qidi X-Plus 4", "Qidi X-Plus 5" })
        DYNAMIC_SECTION(m) { CHECK(qidi_polar_cooler_model(m)); }
    CHECK_FALSE(qidi_polar_cooler_model("Qidi Q1 Pro"));
    CHECK_FALSE(qidi_polar_cooler_model("Qidi X-Max 3"));
    CHECK_FALSE(qidi_polar_cooler_model("Snapmaker U1"));
}

TEST_CASE("the probe waits longer for a QIDI Box printer only", "[qidi][send]")
{
    CHECK(probe_timeout_for("Qidi X-Max 4") > 3);
    CHECK(probe_timeout_for("Snapmaker U1") == 3);
    CHECK(probe_timeout_for("") == 3);
}

// ---- The Box as the firmware really lays it out (fix/qidi-box-index) ---------------------------
//
// The 2.4.6.3 report: a QIDI with all its filament in Box 1 (1C ASA) was shown "Unit 1" empty and
// "Unit 2" 2A-2C PLA, and the send pointed value_t1 at slot7. The send read "loaded" off
// filament_slot<n>, which the firmware's saved_variables ship as 1 (a PLA) on every slot and keep
// after a spool is pulled; whether a slot holds filament is its own state word slot<n> and its
// box_stepper slot<n> sensor (runout_button 0, as QIDIStudio 2.07.02.60 reads it). Box 1 is
// slots 0-3 (box_config box0 -> box_stepper slot0..3), and value_t<tool> = 'slot<n>' names the
// same n. Payloads below are shaped like the firmware's save_variables and objects.

#include <nlohmann/json.hpp>

#include <map>
#include <set>

namespace {

struct FwSlot
{
    int  state { 0 };  // save_variables slot<n>: 0 empty, 1 available, 2 loaded, <0 blocked
    int  fila { 1 };   // filament_slot<n>: the firmware default is 1 on every slot
    int  colour { 1 }; // color_slot<n>
};

// save_variables `variables` the way a QIDI Box firmware keeps them: every key 0..16 present with
// the shipped defaults, then the slots given here.
nlohmann::json firmware_vars(int box_count, const std::map<int, FwSlot>& slots)
{
    nlohmann::json v = nlohmann::json::object();
    v["box_count"]      = box_count;
    v["enable_box"]     = 1;
    v["last_load_slot"] = "slot-1";
    for (int n = 0; n <= 16; ++n) {
        v["filament_slot" + std::to_string(n)] = 1;
        v["color_slot" + std::to_string(n)]    = 1;
        v["vendor_slot" + std::to_string(n)]   = 1;
        if (n < 16) {
            v["slot" + std::to_string(n)]    = 0;
            v["runout_" + std::to_string(n)] = 0;
        }
    }
    for (const auto& [n, s] : slots) {
        v["slot" + std::to_string(n)]          = s.state;
        v["filament_slot" + std::to_string(n)] = s.fila;
        v["color_slot" + std::to_string(n)]    = s.colour;
    }
    return v;
}

// A Moonraker status answer with save_variables and the box_stepper objects for `steppers` slots
// (Klipper answers {} for a stepper it does not have). `present` = slots whose sensor sees filament.
std::string firmware_status(const nlohmann::json& vars, int steppers, const std::set<int>& present,
                            const nlohmann::json& mcc = nlohmann::json())
{
    nlohmann::json st;
    st["print_stats"]    = { { "state", "standby" } };
    st["save_variables"] = { { "variables", vars } };
    for (int n = 0; n < 16; ++n)
        st["box_stepper slot" + std::to_string(n)] =
            n < steppers ? nlohmann::json { { "runout_button", present.count(n) ? 0 : 1 } } : nlohmann::json::object();
    if (!mcc.is_null())
        st["multi_color_controller"] = mcc;
    return st.dump();
}

// The filament list's ids used below: 1 PLA Rapido, 5 ASA; colours 1 #FAFAFA, 2 #060606.
const QidiFilaList& filas()
{
    static const QidiFilaList l = parse_qidi_filas_cfg(k_filas_cfg);
    return l;
}

const Slot* slot_at(const Status& st, int index)
{
    for (const Slot& s : st.slots)
        if (s.index == index)
            return &s;
    return nullptr;
}

} // namespace

TEST_CASE("QIDI Box 1 is slots 0-3: one Box, filament only in Box 1, 1C ASA", "[qidi][send][qidibox]")
{
    // 1A PLA white, 1B empty (filament_slot1 still the default 1), 1C ASA black, 1D empty.
    const nlohmann::json vars = firmware_vars(1, { { 0, { 1, 1, 1 } }, { 2, { 1, 5, 2 } } });

    SECTION("save_variables alone: empty slots stay empty whatever filament_slot<n> says") {
        bool              have  = false;
        const std::vector<Slot> slots = slots_from_qidi_variables(vars.dump(), filas(), have);
        REQUIRE(slots.size() == 4);
        CHECK(have);
        CHECK(slots[0].index == 0);
        CHECK(slots[0].name == "Box 1 slot 1");
        CHECK(slots[0].loaded);
        CHECK(slots[0].type == "PLA");
        CHECK_FALSE(slots[1].loaded); // the old read called this a PLA (filament_slot1 = 1)
        CHECK(slots[1].type.empty());
        CHECK(slots[2].index == 2);
        CHECK(slots[2].name == "Box 1 slot 3");
        CHECK(slots[2].loaded);
        CHECK(slots[2].type == "ASA");
        CHECK(slots[2].color == "#060606");
        CHECK_FALSE(slots[3].loaded);
    }
    SECTION("the full answer, with the slot sensors: the ASA plate goes to 1C = slot2") {
        const Status st = parse_qidi_status(firmware_status(vars, 4, { 0, 2 }), filas());
        REQUIRE(st.qidi_box);
        CHECK_FALSE(st.qidi_box_inconsistent);
        REQUIRE(st.slots.size() == 4);
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "ASA", "#000000") });
        CHECK(am.mapping == "0:2");
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::Start);
        CHECK(qidi_start_script(QidiPrintOptions { 1, -1, -1 }, am.mapping) ==
              "SAVE_VARIABLE VARIABLE=enable_box VALUE=1\nSAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot2'\"\n");
    }
    SECTION("a white PLA plate goes to 1A, never to the empty 1B that still says PLA") {
        const Status st = parse_qidi_status(firmware_status(vars, 4, { 0, 2 }), filas());
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "PLA", "#FAFAFA"), filament(1, "PLA", "#FAFAFA") });
        CHECK(am.mapping == "0:0,1:0");
        CHECK(am.facts.mapped_to_empty == 0);
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::Start);
    }
    SECTION("the state word and the sensor disagree on a slot: it is not taken as loaded") {
        // slot2 says available, its sensor says empty.
        const Status st = parse_qidi_status(firmware_status(vars, 4, { 0 }), filas());
        const Slot*  c  = slot_at(st, 2);
        REQUIRE(c != nullptr);
        CHECK_FALSE(c->loaded);
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "ASA", "#000000") });
        CHECK(am.mapping.empty());
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::NeedsMapping);
    }
    SECTION("the send log names every slot by its Box letter, and nothing else") {
        const Status      st   = parse_qidi_status(firmware_status(vars, 4, { 0, 2 }), filas());
        const std::string line = describe_qidi_box(st);
        CHECK(line.find("1A loaded PLA (PLA Rapido) #FAFAFA") != std::string::npos);
        CHECK(line.find("1B empty") != std::string::npos);
        CHECK(line.find("1C loaded ASA #060606") != std::string::npos);
        CHECK(line.find("1D empty") != std::string::npos);
        CHECK(line.find("2A") == std::string::npos);
        CHECK(line.find("INCONSISTENT") == std::string::npos);
    }
}

TEST_CASE("QIDI two Boxes with filament only in Box 1: Box 2 is empty, not the defaults", "[qidi][send][qidibox]")
{
    // Box 2 (slots 4-7) has nothing loaded; its filament_slot<n> are the shipped 1 (PLA).
    const nlohmann::json vars = firmware_vars(2, { { 0, { 1, 1, 1 } }, { 1, { 1, 1, 2 } }, { 2, { 2, 5, 2 } } });
    const Status         st   = parse_qidi_status(firmware_status(vars, 8, { 0, 1, 2 }), filas());
    REQUIRE(st.qidi_box);
    CHECK_FALSE(st.qidi_box_inconsistent);
    REQUIRE(st.slots.size() == 8);
    for (int n = 4; n < 8; ++n) {
        const Slot* s = slot_at(st, n);
        REQUIRE(s != nullptr);
        CHECK_FALSE(s->loaded);
        CHECK(s->type.empty());
    }
    CHECK(slot_at(st, 2)->type == "ASA");
    const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "PLA", "#060606"), filament(1, "ASA", "#000000") });
    CHECK(am.mapping == "0:1,1:2");
    CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::Start);
}

TEST_CASE("QIDI: the reported 2.4.6.3 picture is refused, not mapped one Box off", "[qidi][send][qidibox]")
{
    // What the report showed: box_count 2, Box 1's spools untagged (filament_slot 0) but there,
    // Box 2's filament_slot4..6 the shipped PLA default - and the printer has slot motors for one
    // Box only. The old read showed Unit 1 empty, Unit 2 A-C PLA, and mapped T1 to slot7.
    const nlohmann::json vars =
        firmware_vars(2, { { 0, { 1, 0, 0 } }, { 1, { 1, 0, 0 } }, { 2, { 1, 0, 0 } }, { 7, { 0, 0, 0 } } });

    SECTION("slot motors for one Box, box_count 2: inconsistent, upload only") {
        const Status st = parse_qidi_status(firmware_status(vars, 4, { 0, 1, 2 }), filas());
        CHECK(st.qidi_box);
        CHECK(st.qidi_box_inconsistent);
        CHECK(st.qidi_box_problem.find("box_count says 2") != std::string::npos);
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(1, "PLA", "#FAFAFA") });
        CHECK(am.mapping.empty());
        CHECK(am.facts.box_inconsistent);
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::UploadOnlyInconsistent);
        CHECK(describe_qidi_box(st).find("INCONSISTENT") != std::string::npos);
    }
    SECTION("without the slot motors in the answer: Box 2 reads empty, Box 1 loaded but unnamed, refused") {
        const Status st = parse_qidi_status(firmware_status(vars, 0, {}), filas());
        CHECK_FALSE(st.qidi_box_inconsistent);
        for (int n = 4; n < 8; ++n)
            CHECK_FALSE(slot_at(st, n)->loaded);
        CHECK(slot_at(st, 2)->loaded);
        CHECK(slot_at(st, 2)->type.empty());
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(1, "PLA", "#FAFAFA") });
        CHECK(am.mapping.empty()); // never slot7, never any slot of Box 2
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::NeedsMapping);
    }
}

TEST_CASE("QIDI multi_color_controller: both shapes, Box 1 is box0, and a shifted answer is refused", "[qidi][send][qidibox]")
{
    SECTION("flat keys (QIDIStudio's 2026-10-19 read): box0_ is Box 1") {
        nlohmann::json mcc = { { "box_count", 1 } };
        for (int n = 0; n < 4; ++n)
            mcc["box0_box_inserted_" + std::to_string(n)] = (n == 0 || n == 2) ? 1 : 0;
        mcc["box0_filament_info_0_filament_type"] = "PLA";
        mcc["box0_filament_info_0_color_rgb_str"] = { "#FFFFFF" };
        mcc["box0_filament_info_2_filament_type"] = "ASA";
        mcc["box0_filament_info_2_filament_str"]  = "ASA";
        mcc["box0_filament_info_2_color_rgb_str"] = { "#202020" };
        const Status st = parse_moonraker_status(nlohmann::json { { "print_stats", { { "state", "standby" } } },
                                                                  { "multi_color_controller", mcc } }
                                                     .dump(),
                                                 "octoprint");
        REQUIRE(st.qidi_box);
        CHECK_FALSE(st.qidi_box_inconsistent);
        REQUIRE(st.slots.size() == 4);
        CHECK(st.slots[2].index == 2);
        CHECK(st.slots[2].type == "ASA");
        CHECK(st.slots[2].color == "#202020");
        CHECK_FALSE(st.slots[3].loaded);
        CHECK(qidi_auto_mapping(st, { filament(0, "ASA", "#202020") }).mapping == "0:2");
    }
    SECTION("box_count 1 but the filament reported in the second Box: refused") {
        const std::string status =
            "{\"print_stats\":{\"state\":\"standby\"},"
            " \"multi_color_controller\":{\"box_count\":1,\"boxes\":["
            "   {\"box_inserted_0\":0,\"box_inserted_1\":0,\"box_inserted_2\":0,\"box_inserted_3\":0,\"filament_info\":[{},{},{},{}]},"
            "   {\"box_inserted_0\":1,\"box_inserted_1\":1,\"box_inserted_2\":1,\"box_inserted_3\":0,\"filament_info\":["
            "     {\"filament_type\":\"PLA\",\"color_rgb_str\":[\"#FFFFFF\"]},{\"filament_type\":\"PLA\",\"color_rgb_str\":[\"#000000\"]},"
            "     {\"filament_type\":\"ASA\",\"color_rgb_str\":[\"#202020\"]},{}]}]}}";
        const Status st = parse_moonraker_status(status, "octoprint");
        CHECK(st.qidi_box_inconsistent);
        CHECK(st.qidi_box_problem.find("2A reports filament, but box_count is 1") != std::string::npos);
        const QidiAutoMapping am = qidi_auto_mapping(st, { filament(0, "ASA", "#202020") });
        CHECK(am.mapping.empty());
        CHECK(qidi_send_policy(am.facts) == QidiSendPolicy::UploadOnlyInconsistent);
    }
    SECTION("the controller and save_variables count different Boxes: refused") {
        const nlohmann::json vars = firmware_vars(2, { { 0, { 1, 1, 1 } } });
        nlohmann::json       mcc  = { { "box_count", 1 } };
        mcc["box0_box_inserted_0"]                 = 1;
        mcc["box0_filament_info_0_filament_type"] = "PLA";
        const Status st = parse_qidi_status(firmware_status(vars, 0, {}, mcc), filas());
        CHECK(st.qidi_box_inconsistent);
        CHECK(st.qidi_box_problem.find("counts 1 Box(es), save_variables 2") != std::string::npos);
    }
    SECTION("the controller calls a slot loaded that its own state word calls empty: refused") {
        const nlohmann::json vars = firmware_vars(1, { { 0, { 1, 1, 1 } } }); // slot2 state 0
        nlohmann::json       mcc  = { { "box_count", 1 } };
        for (int n = 0; n < 4; ++n)
            mcc["box0_box_inserted_" + std::to_string(n)] = (n == 0 || n == 2) ? 1 : 0;
        mcc["box0_filament_info_0_filament_type"] = "PLA";
        mcc["box0_filament_info_2_filament_type"] = "ASA";
        const Status st = parse_qidi_status(firmware_status(vars, 4, { 0 }, mcc), filas());
        CHECK(st.qidi_box_inconsistent);
        CHECK(st.qidi_box_problem.find("slot 1C") != std::string::npos);
    }
}

TEST_CASE("qidi_send_policy never starts on an empty slot or an answer that does not add up", "[qidi][send][qidibox]")
{
    SECTION("a filament picked onto an empty slot: never started, not even after a confirm") {
        QidiSendFacts f    = good_facts();
        f.mapped_to_empty  = 1;
        f.type_mismatches  = 1; // an empty slot names no material either
        CHECK(qidi_send_policy(f) == QidiSendPolicy::MappedToEmpty);
    }
    SECTION("an inconsistent Box: upload only, whatever was mapped") {
        QidiSendFacts f    = good_facts();
        f.box_inconsistent = true;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::UploadOnlyInconsistent);
    }
    SECTION("busy still wins: nothing goes to a running print") {
        QidiSendFacts f    = good_facts();
        f.box_inconsistent = true;
        f.busy             = true;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::UploadOnlyBusy);
    }
    SECTION("the person switched the Box off for this print: the extruder spool, nothing mapped") {
        QidiSendFacts f    = good_facts();
        f.box_inconsistent = true;
        f.use_box          = false;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::Start);
    }
    SECTION("both refusals say why") {
        CHECK(qidi_send_refusal(QidiSendPolicy::MappedToEmpty).find("empty") != std::string::npos);
        CHECK(qidi_send_refusal(QidiSendPolicy::UploadOnlyInconsistent).find("did not add up") != std::string::npos);
    }
}
