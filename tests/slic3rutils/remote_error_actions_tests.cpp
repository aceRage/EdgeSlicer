// What the hub and the app are told about a printer error, and what they are allowed to do
// about it.
//
// Phase 1 gave the desktop dialog every button the shipped hms_action tables ask for. This is the
// same set crossing the wire: the status JSON and the event payload carry it as data, and the
// control route accepts a subset of it as verbs. Three things can go wrong there that cannot go
// wrong on the desktop, and all three are what these cases are about:
//
//   1. A phone screen is a snapshot. The desktop dialog is destroyed when the error changes; a
//      status page is not. So a tap can arrive carrying the code that was on screen ten minutes
//      ago, and "Resume" meant for a filament runout must not reach the nozzle crash that
//      replaced it. Every error verb names its code and the route refuses (409) unless the
//      printer is still reporting exactly that one.
//
//   2. The resume / stop / ignore commands carry "job_id" so firmware can check they are for the
//      job it holds. A print that ended between the page load and the tap leaves no job_id, and
//      a command sent without one is dropped in silence - indistinguishable, from the sofa, from
//      the printer ignoring the person. Refused (409) rather than sent.
//
//   3. Some of the dialog's buttons answer a question only somebody standing at the printer can
//      answer. "Filament Extruded, Continue" tells the printer the filament is clear of the path.
//      Those are described so a client can show them greyed with a reason, and refused (403).
//
// The guard is one pure function over four strings and two booleans, so every one of those cases
// is reachable here with no printer, no socket and no window - which is the only way the stale
// one is testable at all.

#include <catch2/catch.hpp>

#include "slic3r/GUI/PrintErrorCommands.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r::GUI;

namespace {

// A code whose table entry is [23, 3] - "No Reminder Next Time" and "Resume Printing (defects
// acceptable)". The pair the owner's H2C actually reported, and the one that rendered nothing at
// all before phase 1.
std::vector<int> actions_23_3()
{
    bool fallback = false;
    return resolve_print_error_actions({23, 3}, fallback);
}

const PrintErrorRemoteAction* find_verb(const std::vector<PrintErrorRemoteAction>& v, const std::string& verb)
{
    for (const PrintErrorRemoteAction& a : v)
        if (a.verb == verb) return &a;
    return nullptr;
}

bool has_id(const std::vector<PrintErrorRemoteAction>& v, int id)
{
    for (const PrintErrorRemoteAction& a : v)
        if (a.id == id) return true;
    return false;
}

// The request the route builds, with the fields a case does not care about already sensible.
PrintErrorActionRequest req(const std::string& verb, const std::string& asked = "0C00402D",
                            const std::string& current = "0C00402D", const std::string& job = "42")
{
    PrintErrorActionRequest r;
    r.verb         = verb;
    r.asked_err    = asked;
    r.confirm      = true;
    r.current_err  = current;
    r.job_id       = job;
    r.printer_name = "X1C";
    r.offered      = actions_23_3();
    return r;
}

} // namespace

TEST_CASE("The actions array carries the buttons a code's table asks for", "[RemoteControl]")
{
    SECTION("a code with actions [23, 3] comes across as two buttons with their verbs")
    {
        const std::vector<PrintErrorRemoteAction> a = describe_print_error_actions(actions_23_3(), true);

        REQUIRE(a.size() == 2);
        REQUIRE(a[0].id == PrintErrorAction::NO_REMINDER_NEXT_TIME);
        REQUIRE(a[0].verb == "idle_ignore_error");
        REQUIRE(a[0].remote_safe);
        REQUIRE_FALSE(a[0].needs_job_id);

        REQUIRE(a[1].id == PrintErrorAction::RESUME_PRINTING_DEFECTS);
        REQUIRE(a[1].verb == "resume_error");
        REQUIRE(a[1].remote_safe);
        REQUIRE(a[1].needs_job_id);
    }

    SECTION("every entry has a label - a button with no words is not a button")
    {
        for (const PrintErrorRemoteAction& x : describe_print_error_actions(actions_23_3(), true))
            REQUIRE_FALSE(x.label.empty());
    }

    SECTION("the ignore family never reads as a fix")
    {
        // The one wording rule this whole feature turns on: a person who taps "Ignore" must not
        // be able to think they repaired something. Both ignore labels and the idle-ignore one
        // say so in the label, because on a phone card there is no dialog body above the button
        // to carry the caveat.
        for (int id : { PrintErrorAction::IGNORE_RESUME,
                        PrintErrorAction::IGNORE_NO_REMINDER_NEXT_TIME,
                        PrintErrorAction::NO_REMINDER_NEXT_TIME,
                        PrintErrorAction::DONT_REMIND_NEXT_TIME }) {
            const PrintErrorRemoteAction x = describe_print_error_action(id);
            INFO("action id " << id << ": " << x.label);
            REQUIRE(x.label.find("not fixed") != std::string::npos);
        }
        // And "Resume printing" says nothing of the sort, so the two cannot be confused.
        REQUIRE(describe_print_error_action(PrintErrorAction::RESUME_PRINTING).label.find("not fixed") ==
                std::string::npos);
    }

    SECTION("an unknown code gets the generic set, and it is all offerable")
    {
        // resolve_print_error_actions' fallback is Stop / Resume / OK. A stuck printer always has
        // something to press, on the phone as on the desktop.
        bool                   fallback = false;
        const std::vector<int> generic  = resolve_print_error_actions({}, fallback);
        REQUIRE(fallback);

        const std::vector<PrintErrorRemoteAction> a = describe_print_error_actions(generic, true);
        REQUIRE(a.size() == 3);
        for (const PrintErrorRemoteAction& x : a) {
            INFO("action id " << x.id);
            REQUIRE(x.remote_safe);
            REQUIRE_FALSE(x.verb.empty());
        }
        REQUIRE(find_verb(a, "stop_error"));
        REQUIRE(find_verb(a, "resume_error"));
        REQUIRE(find_verb(a, "ack_close"));
    }

    SECTION("without a job_id the commands that need one are described, not offered")
    {
        const std::vector<PrintErrorRemoteAction> a = describe_print_error_actions(actions_23_3(), false);

        REQUIRE(a.size() == 2);
        const PrintErrorRemoteAction* resume = find_verb(a, "resume_error");
        REQUIRE(resume);
        REQUIRE(resume->needs_job_id);
        REQUIRE_FALSE(resume->remote_safe); // shown greyed with a reason, never armed
        // The one that carries no job_id is unaffected.
        const PrintErrorRemoteAction* idle = find_verb(a, "idle_ignore_error");
        REQUIRE(idle);
        REQUIRE(idle->remote_safe);
    }

    SECTION("without the printer's details, proceed and don't-remind are described, not offered")
    {
        // The same rule as the job_id one above, for the other thing a command can be missing.
        // Only a refused command hands over an action_json; an error that arrived any other way
        // has none, and the payloads for these two cannot be built without it.
        bool                   fallback = false;
        const std::vector<int> both     = resolve_print_error_actions({PrintErrorAction::PROCEED,
                                                                       PrintErrorAction::DONT_REMIND_NEXT_TIME,
                                                                       PrintErrorAction::OK_BUTTON},
                                                                      fallback);
        REQUIRE_FALSE(fallback);

        // has_action_json defaults to false, which is what every caller that has no blob passes.
        const std::vector<PrintErrorRemoteAction> without = describe_print_error_actions(both, true);
        for (const char* v : { "ack_proceed", "dont_remind" }) {
            INFO("verb " << v);
            const PrintErrorRemoteAction* x = find_verb(without, v);
            REQUIRE(x);
            REQUIRE(x->needs_action_json);
            REQUIRE_FALSE(x->remote_safe); // greyed with a reason, exactly like a jobless resume
        }
        // The button next to them, which needs nothing, stays pressable.
        REQUIRE(find_verb(without, "ack_close"));
        REQUIRE(find_verb(without, "ack_close")->remote_safe);

        // And with the blob in hand both are armed.
        const std::vector<PrintErrorRemoteAction> with = describe_print_error_actions(both, true, true);
        for (const char* v : { "ack_proceed", "dont_remind" }) {
            INFO("verb " << v);
            const PrintErrorRemoteAction* x = find_verb(with, v);
            REQUIRE(x);
            REQUIRE(x->remote_safe);
        }

        // The blob does nothing for the actions that do not need it: a resume with no job is
        // still refused, whatever details came with the error.
        const std::vector<PrintErrorRemoteAction> resume_no_job =
            describe_print_error_actions(actions_23_3(), false, true);
        REQUIRE(find_verb(resume_no_job, "resume_error"));
        REQUIRE_FALSE(find_verb(resume_no_job, "resume_error")->remote_safe);
    }

    SECTION("the AMS family and its neighbours come across described and not remote-safe")
    {
        for (int id : { PrintErrorAction::FILAMENT_EXTRUDED,
                        PrintErrorAction::RETRY_FILAMENT_EXTRUDED,
                        PrintErrorAction::CONTINUE,
                        PrintErrorAction::RETRY_PROBLEM_SOLVED,
                        PrintErrorAction::ABORT,
                        PrintErrorAction::STOP_DRYING,
                        PrintErrorAction::REFRESH_NOZZLE,
                        PrintErrorAction::TURN_OFF_FIRE_ALARM,
                        PrintErrorAction::DISABLE_PURIFICATION }) {
            const PrintErrorRemoteAction x = describe_print_error_action(id);
            INFO("action id " << id);
            REQUIRE_FALSE(x.label.empty()); // the client can say what it would have been
            REQUIRE(x.verb.empty());        // and there is no verb to send
            REQUIRE_FALSE(x.remote_safe);
        }
    }

    SECTION("the desktop-only navigation ids and the close-box flag are dropped entirely")
    {
        // A "View the camera" button on a page that is not the desktop's Device tab would do
        // nothing at all, and id 39 is a flag rather than a button.
        bool                                      fallback = false;
        const std::vector<PrintErrorRemoteAction> a =
            describe_print_error_actions(resolve_print_error_actions({PrintErrorAction::JUMP_TO_LIVEVIEW,
                                                                      PrintErrorAction::REMOVE_CLOSE_BTN,
                                                                      PrintErrorAction::OK_BUTTON},
                                                                     fallback),
                                         true);
        REQUIRE_FALSE(has_id(a, PrintErrorAction::REMOVE_CLOSE_BTN));
        REQUIRE(has_id(a, PrintErrorAction::OK_BUTTON));
        // JUMP_TO_LIVEVIEW keeps a label so a client may show it, but never a verb.
        const PrintErrorRemoteAction live = describe_print_error_action(PrintErrorAction::JUMP_TO_LIVEVIEW);
        REQUIRE(live.verb.empty());
        REQUIRE_FALSE(live.remote_safe);
    }
}

TEST_CASE("The control route's verbs map to one command each", "[RemoteControl]")
{
    SECTION("the verbs this phase offers, and their job_id rule")
    {
        REQUIRE(is_remote_safe_verb("resume_error"));
        REQUIRE(is_remote_safe_verb("stop_error"));
        REQUIRE(is_remote_safe_verb("ignore_error"));
        REQUIRE(is_remote_safe_verb("idle_ignore_error"));
        REQUIRE(is_remote_safe_verb("ack_close"));

        // The three that carry "job_id" in the payload, and only those.
        REQUIRE(print_error_verb_needs_job_id("resume_error"));
        REQUIRE(print_error_verb_needs_job_id("stop_error"));
        REQUIRE(print_error_verb_needs_job_id("ignore_error"));
        REQUIRE_FALSE(print_error_verb_needs_job_id("idle_ignore_error"));
        REQUIRE_FALSE(print_error_verb_needs_job_id("ack_close"));
    }

    SECTION("proceed and don't-remind are offerable verbs, gated on the printer's blob")
    {
        // CHANGED from "known verbs but not offered". When this was written nothing in the fork
        // ever received the printer's action_json - set_action_json had no caller - so the two
        // verbs were pinned unsafe at the verb level, which was the only honest answer available.
        // MachineObject::add_command_error_code_dlg now stores that blob when a refused command
        // brings one, so the ceiling moves: the verbs are remote-safe, and whether a given error
        // may use them is decided per-error by describe_print_error_actions and
        // check_print_error_action, exactly as needs_job_id already worked.
        REQUIRE(is_print_error_verb("ack_proceed"));
        REQUIRE(is_print_error_verb("dont_remind"));
        REQUIRE(is_remote_safe_verb("ack_proceed"));
        REQUIRE(is_remote_safe_verb("dont_remind"));

        // And they are the only two whose command is built from the blob.
        REQUIRE(print_error_verb_needs_action_json("ack_proceed"));
        REQUIRE(print_error_verb_needs_action_json("dont_remind"));
        for (const char* v : { "resume_error", "stop_error", "ignore_error", "idle_ignore_error", "ack_close" }) {
            INFO("verb " << v);
            REQUIRE_FALSE(print_error_verb_needs_action_json(v));
        }
    }

    SECTION("the generic controls are not error verbs, and nonsense is not either")
    {
        // pause / resume / stop keep their own path and their own meaning: a plain resume is
        // command_task_resume with no err and no job_id, which is a different command from
        // resume_error and must stay reachable under its own name.
        for (const char* v : { "pause", "resume", "stop", "", "resume_", "RESUME_ERROR", "ignore" }) {
            INFO("verb " << v);
            REQUIRE_FALSE(is_print_error_verb(v));
        }
    }
}

TEST_CASE("The control route refuses an error action that cannot be right", "[RemoteControl]")
{
    std::string why;

    SECTION("the ordinary case goes through")
    {
        REQUIRE(check_print_error_action(req("resume_error"), why) == 0);
        REQUIRE(why.empty());
    }

    SECTION("a stale error code is refused with both codes named")
    {
        // The case the whole `err` parameter exists for. The page was drawn while the printer
        // reported 0C00402D; by the time the tap arrives it is on 05008051, and a resume composed
        // for a camera fault must not answer a filament fault.
        PrintErrorActionRequest r = req("resume_error", "0C00402D", "05008051");
        REQUIRE(check_print_error_action(r, why) == 409);
        REQUIRE(why.find("0500 8051") != std::string::npos); // what it is reporting now
        REQUIRE(why.find("0C00 402D") != std::string::npos); // what was asked for
    }

    SECTION("an error that cleared is refused, not sent to a healthy print")
    {
        PrintErrorActionRequest r = req("resume_error", "0C00402D", "");
        REQUIRE(check_print_error_action(r, why) == 409);
        REQUIRE(why.find("not reporting an error") != std::string::npos);
    }

    SECTION("the code is compared on its value, not its spelling")
    {
        // The status JSON carries "0C00402D" and the page shows "0C00 402D"; a client that sends
        // back what it displayed is not making a mistake.
        REQUIRE(check_print_error_action(req("resume_error", "0c00 402d"), why) == 0);
        REQUIRE(check_print_error_action(req("resume_error", "0C00-402D"), why) == 0);
    }

    SECTION("an empty err is a bad request, not a stale one")
    {
        REQUIRE(check_print_error_action(req("resume_error", ""), why) == 400);
        REQUIRE(why.find("err=") != std::string::npos);
    }

    SECTION("a command that needs a job_id is refused when the printer has none")
    {
        PrintErrorActionRequest r = req("resume_error", "0C00402D", "0C00402D", "");
        REQUIRE(check_print_error_action(r, why) == 409);
        REQUIRE(why.find("job") != std::string::npos);

        // And the verb that carries no job_id is unaffected by the same printer state.
        PrintErrorActionRequest ok = req("idle_ignore_error", "0C00402D", "0C00402D", "");
        REQUIRE(check_print_error_action(ok, why) == 0);
    }

    SECTION("an unknown verb is refused 400, separately from every other refusal")
    {
        // CHANGED: this used ack_proceed as its 403 example, which it no longer is - that verb is
        // now offerable and refused 409 when the printer sent no blob (the section below). The
        // distinction the case was written for still holds: 400 means "you sent something that is
        // not an action at all", and every other status means the verb exists.
        PrintErrorActionRequest bad = req("eject_the_filament");
        REQUIRE(check_print_error_action(bad, why) == 400);
        REQUIRE(why.find("not a printer-error action") != std::string::npos);

        // There is no desktop-only verb left to demonstrate 403 with: every id that has to be
        // pressed at the printer (the AMS family, the drying stop, the buzzer, the purification
        // switch) carries no verb at all, so it is refused as an unknown word. The 403 branch of
        // check_print_error_action stays for the next verb that needs it.
        for (int id : { PrintErrorAction::FILAMENT_EXTRUDED, PrintErrorAction::ABORT,
                        PrintErrorAction::TURN_OFF_FIRE_ALARM }) {
            INFO("action id " << id);
            REQUIRE(describe_print_error_action(id).verb.empty());
        }
    }

    SECTION("proceed is refused 409 when the error brought no details to answer it with")
    {
        // The new gate, and the one that keeps the two verbs honest. The error is current, the
        // job is there, the table offers the button - but the printer sent no action_json, so
        // there is no payload to build and the request is refused before a builder can fail.
        bool                    fallback = false;
        PrintErrorActionRequest r        = req("ack_proceed");
        r.offered        = resolve_print_error_actions({PrintErrorAction::PROCEED}, fallback);
        r.has_action_json = false;
        REQUIRE(check_print_error_action(r, why) == 409);
        REQUIRE(why.find("details") != std::string::npos);

        // With the blob in hand the very same request goes through.
        r.has_action_json = true;
        REQUIRE(check_print_error_action(r, why) == 0);

        // And so does don't-remind, which is gated on the same thing.
        PrintErrorActionRequest d = req("dont_remind");
        d.offered                 = resolve_print_error_actions({PrintErrorAction::DONT_REMIND_NEXT_TIME}, fallback);
        d.has_action_json         = false;
        REQUIRE(check_print_error_action(d, why) == 409);
        d.has_action_json = true;
        REQUIRE(check_print_error_action(d, why) == 0);
    }

    SECTION("a verb this error does not offer is refused")
    {
        // 0C00402D's table entry is [11] - OK and nothing else. Bambu Studio offers no resume for
        // it either (the toolhead camera wants a reboot), so neither may the hub.
        bool                    fallback = false;
        PrintErrorActionRequest r        = req("resume_error");
        r.offered                        = resolve_print_error_actions({PrintErrorAction::OK_BUTTON}, fallback);
        REQUIRE_FALSE(fallback);

        REQUIRE(check_print_error_action(r, why) == 409);
        REQUIRE(why.find("does not offer") != std::string::npos);

        // The one it does offer goes through.
        r.verb = "ack_close";
        REQUIRE(check_print_error_action(r, why) == 0);
    }

    SECTION("stopping the print needs the confirmation, like every other stop")
    {
        bool                    fallback = false;
        PrintErrorActionRequest r        = req("stop_error");
        r.offered = resolve_print_error_actions({PrintErrorAction::STOP_PRINTING, PrintErrorAction::RESUME_PRINTING},
                                                fallback);
        r.confirm = false;
        REQUIRE(check_print_error_action(r, why) == 400);
        REQUIRE(why.find("confirm=1") != std::string::npos);

        r.confirm = true;
        REQUIRE(check_print_error_action(r, why) == 0);
    }

    SECTION("the stale check runs before the job_id one, so the message names the real problem")
    {
        // Both wrong at once: the code moved on AND the job ended. Told about the code, because
        // that is the one that says the page is out of date and reloading fixes it.
        PrintErrorActionRequest r = req("resume_error", "0C00402D", "05008051", "");
        REQUIRE(check_print_error_action(r, why) == 409);
        REQUIRE(why.find("0500 8051") != std::string::npos);
    }
}

TEST_CASE("What the route sends is what phase 1 pinned", "[RemoteControl]")
{
    // The verbs exist to reach these exact payloads. If a verb ever stopped mapping to the
    // command its name says, the hub would send a generic resume the firmware ignores - which is
    // the bug phase 1 fixed on the desktop, arriving again by another door. So the payload each
    // verb's command produces is compared here against the same fixtures the dialog's test uses.
    const std::string err = "201342509"; // std::to_string(0x0C00402D)
    const std::string job = "42";
    const std::string seq = "7";

    SECTION("resume_error is command_hms_resume: err, param reserve and job_id")
    {
        REQUIRE(build_hms_resume(err, job, seq) ==
                nlohmann::json::parse(R"({"print":{"command":"resume","err":"201342509","param":"reserve",
                                          "job_id":"42","sequence_id":"7"}})"));
    }

    SECTION("stop_error is command_hms_stop, not the generic abort")
    {
        REQUIRE(build_hms_stop(err, job, seq) ==
                nlohmann::json::parse(R"({"print":{"command":"stop","err":"201342509","param":"reserve",
                                          "job_id":"42","sequence_id":"7"}})"));
    }

    SECTION("ignore_error is command_hms_ignore - a different command from resume")
    {
        // Same shape, different command name, and that difference is the whole meaning: the print
        // continues with the fault present. A verb that quietly resolved to "resume" would make
        // the hub's two buttons do the same thing under two labels.
        const nlohmann::json ignore = build_hms_ignore(err, job, seq);
        REQUIRE(ignore["print"]["command"] == "ignore");
        REQUIRE(ignore != build_hms_resume(err, job, seq));
    }

    SECTION("idle_ignore_error carries a type and no job - it does not touch the print")
    {
        const nlohmann::json j = build_hms_idle_ignore(err, 0, seq);
        REQUIRE(j == nlohmann::json::parse(R"({"print":{"command":"idle_ignore","err":"201342509","type":0,
                                               "sequence_id":"7"}})"));
        REQUIRE_FALSE(j["print"].contains("job_id"));
    }

    SECTION("ack_close is the uiop close ack, on the system topic with the eight-hex code")
    {
        REQUIRE(build_clean_print_error_uiop(0x0C00402D, seq) ==
                nlohmann::json::parse(R"({"system":{"command":"uiop","sequence_id":"7","name":"print_error",
                                          "action":"close","source":1,"type":"dialog","err":"0C00402D"}})"));
    }
}

TEST_CASE("normalize_error_code accepts what a client would send back", "[RemoteControl]")
{
    REQUIRE(normalize_error_code("0c00402d") == "0C00402D");
    REQUIRE(normalize_error_code("0C00 402D") == "0C00402D");
    REQUIRE(normalize_error_code("0C00-402D") == "0C00402D");
    REQUIRE(normalize_error_code("0C00402D") == "0C00402D");
    REQUIRE(normalize_error_code("").empty());
}
