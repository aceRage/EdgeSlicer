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
    SECTION("a printer that answered but is not QIDI firmware") {
        QidiSendFacts f = good_facts();
        f.qidi          = false;
        f.box_present   = false;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::NotApplicable);
        f.model_may_have_box = false;
        CHECK(qidi_send_policy(f) == QidiSendPolicy::NotApplicable);
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
