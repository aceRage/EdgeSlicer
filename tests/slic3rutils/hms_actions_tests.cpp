// What the printer-error dialog offers, and what it actually sends.
//
// The bug these cover: an H2C stopped on an error showed a dialog with no way to resume. Two
// independent causes, both reachable here with no printer, no window and no broker:
//
//   1. The shipped resources/hms/hms_action_<devtype>.json tables name their buttons by integer
//      id, and every id >= 23 was added to Bambu Studio after this fork branched. The fork's
//      dialog silently dropped any id it had no button for, so a code whose table entry reads
//      [27, 5] ("Ignore this and Resume", "Stop Printing") rendered nothing at all. Across the
//      seven shipped tables that is 500+ entries. resolve_print_error_actions is the seam: it
//      keeps the ids we can draw, drops the ones nobody can, and falls back to a generic
//      Stop/Resume/OK set rather than to an empty dialog.
//
//   2. Even the buttons that did render sent the wrong thing. "Resume Printing" posted an event
//      that StatusPanel turned into command_task_resume() - a bare
//      {"command":"resume","param":""} with no "err" and no "job_id". Firmware that gates an
//      error-triggered resume on those fields ignores it without complaining, so the button
//      looked like it worked and the printer stayed stuck. The builders below are compared
//      field-for-field against the payloads Bambu Studio sends, because "close enough" is
//      indistinguishable from the bug.
//
// The action tables are read through the same lookup the dialog uses, against a fixture written
// to a temp dir - no AppConfig, no clock, no cloud.

#include <catch2/catch.hpp>

#include "slic3r/GUI/PrintErrorCommands.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using json = nlohmann::json;

namespace {

// A slice of a real hms_action table, in the shape the shipped files use. The four entries are
// the four cases that matter: a code whose actions are all renderable, one that mixes renderable
// with ids no dialog has, one that is entirely unrenderable, and the informational OK-only code
// the owner's H2C actually reported.
const char* const FIXTURE_ACTIONS = R"({
  "result": 0,
  "ver": 202609171044,
  "data": {
    "device_error": [
      { "ecode": "05008051", "image": "", "actions": [23, 3], "device": "default" },
      { "ecode": "0C00402D", "image": "", "actions": [11], "device": "default" },
      { "ecode": "07008036", "image": "", "actions": [51, 16, 44], "device": "default" },
      { "ecode": "0500402E", "image": "", "actions": [14, 15], "device": "default" }
    ]
  }
})";

// The lookup the dialog does: find the entry for this code, hand its "actions" list to the
// resolver. Written out here rather than called through HMSQuery so the test needs no data dir.
std::vector<int> table_actions_for(const json& table, const std::string& ecode)
{
    for (const auto& item : table["data"]["device_error"]) {
        if (item.value("ecode", std::string()) == ecode)
            return item["actions"].get<std::vector<int>>();
    }
    return {};
}

std::vector<int> resolve(const json& table, const std::string& ecode, bool& used_fallback)
{
    return resolve_print_error_actions(table_actions_for(table, ecode), used_fallback);
}

} // namespace

TEST_CASE("Action lists resolve to the buttons the dialog can draw", "[HmsActions]")
{
    const json table = json::parse(FIXTURE_ACTIONS);

    SECTION("a code whose actions the dialog has buttons for keeps them, in table order")
    {
        // 23 = NO_REMINDER_NEXT_TIME, 3 = RESUME_PRINTING_DEFECTS. Id 23 is the case that
        // motivated all of this: before the enum was extended it was dropped, leaving this code
        // showing only "Resume Printing (defects acceptable)".
        bool             fallback = true;
        std::vector<int> buttons  = resolve(table, "05008051", fallback);

        REQUIRE_FALSE(fallback);
        REQUIRE(buttons.size() == 2);
        REQUIRE(buttons[0] == PrintErrorAction::NO_REMINDER_NEXT_TIME);
        REQUIRE(buttons[1] == PrintErrorAction::RESUME_PRINTING_DEFECTS);
    }

    SECTION("the informational camera code still resolves to OK alone")
    {
        // 0C00402D is "toolhead camera is not working; reboot the device" - Bambu Studio offers
        // no resume for it either. Pinned so a future change to the fallback rule cannot start
        // offering a resume on a code that genuinely has none.
        bool             fallback = true;
        std::vector<int> buttons  = resolve(table, "0C00402D", fallback);

        REQUIRE_FALSE(fallback);
        REQUIRE(buttons.size() == 1);
        REQUIRE(buttons[0] == PrintErrorAction::OK_BUTTON);
    }

    SECTION("ids no dialog can draw are dropped, the rest survive")
    {
        // 51 = ABORT is real; 16 and 44 appear in the shipped tables but have no button in Bambu
        // Studio's enum either, so they are not ours to invent.
        bool             fallback = true;
        std::vector<int> buttons  = resolve(table, "07008036", fallback);

        REQUIRE_FALSE(fallback);
        REQUIRE(buttons.size() == 1);
        REQUIRE(buttons[0] == PrintErrorAction::ABORT);
    }

    SECTION("a code whose actions are all unrenderable gets the generic set, not an empty dialog")
    {
        // 14 and 15 have no button anywhere. Before the fallback this rendered nothing, which is
        // the "stuck with no way to resume" report.
        bool             fallback = false;
        std::vector<int> buttons  = resolve(table, "0500402E", fallback);

        REQUIRE(fallback);
        REQUIRE(buttons == generic_print_error_actions());
    }

    SECTION("a code with no table entry at all gets the generic set")
    {
        bool             fallback = false;
        std::vector<int> buttons  = resolve(table, "DEADBEEF", fallback);

        REQUIRE(fallback);
        REQUIRE(buttons == generic_print_error_actions());
    }

    SECTION("the generic set is always pressable")
    {
        const std::vector<int> generic = generic_print_error_actions();
        REQUIRE(generic.size() == 3);
        for (int id : generic)
            REQUIRE(print_error_action_is_known(id));
    }

    SECTION("a repeated id is drawn once")
    {
        bool             fallback = true;
        std::vector<int> buttons  = resolve_print_error_actions({5, 11, 5}, fallback);

        REQUIRE_FALSE(fallback);
        REQUIRE(buttons.size() == 2);
        REQUIRE(buttons[0] == PrintErrorAction::STOP_PRINTING);
        REQUIRE(buttons[1] == PrintErrorAction::OK_BUTTON);
    }

    SECTION("REMOVE_CLOSE_BTN alone is not a button, so the generic set still appears")
    {
        // Id 39 hides the window's close box. A code whose only action was 39 would otherwise
        // produce a dialog with no close box and nothing to press - a trap, not a prompt.
        bool             fallback = false;
        std::vector<int> buttons  = resolve_print_error_actions({PrintErrorAction::REMOVE_CLOSE_BTN}, fallback);

        REQUIRE(fallback);
        REQUIRE(buttons.size() == 4);
        REQUIRE(buttons[3] == PrintErrorAction::REMOVE_CLOSE_BTN);
    }
}

TEST_CASE("Buttons whose command carries a job_id are the ones gated on having one", "[HmsActions]")
{
    // The dialog greys these out when the printer has not told us a job id, because the command
    // would be rejected. Getting the set wrong either disables a working button or arms a dead
    // one, so it is pinned rather than left to the switch statement.
    REQUIRE(print_error_action_needs_job_id(PrintErrorAction::RESUME_PRINTING));
    REQUIRE(print_error_action_needs_job_id(PrintErrorAction::RESUME_PRINTING_DEFECTS));
    REQUIRE(print_error_action_needs_job_id(PrintErrorAction::RESUME_PRINTING_PROBELM_SOLVED));
    REQUIRE(print_error_action_needs_job_id(PrintErrorAction::STOP_PRINTING));
    REQUIRE(print_error_action_needs_job_id(PrintErrorAction::FILAMENT_LOAD_RESUME));
    REQUIRE(print_error_action_needs_job_id(PrintErrorAction::IGNORE_RESUME));
    REQUIRE(print_error_action_needs_job_id(PrintErrorAction::IGNORE_NO_REMINDER_NEXT_TIME));
    REQUIRE(print_error_action_needs_job_id(PrintErrorAction::PROBLEM_SOLVED_RESUME));

    // These send a command with no job_id field, so a missing job id is no reason to disable them.
    REQUIRE_FALSE(print_error_action_needs_job_id(PrintErrorAction::OK_BUTTON));
    REQUIRE_FALSE(print_error_action_needs_job_id(PrintErrorAction::NO_REMINDER_NEXT_TIME));
    REQUIRE_FALSE(print_error_action_needs_job_id(PrintErrorAction::REFRESH_NOZZLE));
    REQUIRE_FALSE(print_error_action_needs_job_id(PrintErrorAction::TURN_OFF_FIRE_ALARM));
    REQUIRE_FALSE(print_error_action_needs_job_id(PrintErrorAction::STOP_DRYING));
    REQUIRE_FALSE(print_error_action_needs_job_id(PrintErrorAction::DISABLE_PURIFICATION));
    REQUIRE_FALSE(print_error_action_needs_job_id(PrintErrorAction::CHECK_ASSISTANT));
}

TEST_CASE("The error-aware commands match Bambu Studio's payloads exactly", "[HmsActions]")
{
    // A fake error and job. The err is the decimal spelling upstream uses - std::to_string of
    // print_error, NOT the eight-hex form the action tables are keyed by. That inconsistency is
    // upstream's, and firmware matches on what it is sent, so it is reproduced rather than fixed.
    const std::string err = "218116141";
    const std::string job = "12345";
    const std::string seq = "77";

    SECTION("resume carries err, param reserve and job_id")
    {
        // This is the whole point of the change. The old path sent
        // {"print":{"command":"resume","param":"","sequence_id":...}} - same command name, and
        // the printer ignored it.
        const json expected = json::parse(R"({"print":{
            "command": "resume", "err": "218116141", "param": "reserve",
            "job_id": "12345", "sequence_id": "77"}})");

        REQUIRE(build_hms_resume(err, job, seq) == expected);
    }

    SECTION("stop carries the same four fields")
    {
        const json expected = json::parse(R"({"print":{
            "command": "stop", "err": "218116141", "param": "reserve",
            "job_id": "12345", "sequence_id": "77"}})");

        REQUIRE(build_hms_stop(err, job, seq) == expected);
    }

    SECTION("ignore is resume's shape with a different command name")
    {
        const json expected = json::parse(R"({"print":{
            "command": "ignore", "err": "218116141", "param": "reserve",
            "job_id": "12345", "sequence_id": "77"}})");

        REQUIRE(build_hms_ignore(err, job, seq) == expected);
    }

    SECTION("idle_ignore carries a type instead of a job, and no param")
    {
        // Different shape on purpose: this one silences the notification and does not touch the
        // print, so there is no job to name.
        const json expected = json::parse(R"({"print":{
            "command": "idle_ignore", "err": "218116141", "type": 0, "sequence_id": "77"}})");

        REQUIRE(build_hms_idle_ignore(err, 0, seq) == expected);
        REQUIRE_FALSE(build_hms_idle_ignore(err, 0, seq)["print"].contains("job_id"));
        REQUIRE_FALSE(build_hms_idle_ignore(err, 0, seq)["print"].contains("param"));
    }

    SECTION("the device-specific commands are bare")
    {
        REQUIRE(build_refresh_nozzle(seq) ==
                json::parse(R"({"print":{"command":"refresh_nozzle","sequence_id":"77"}})"));
        REQUIRE(build_stop_buzzer(seq) ==
                json::parse(R"({"print":{"command":"buzzer_ctrl","mode":0,"sequence_id":"77"}})"));
        REQUIRE(build_purification_disable(seq) ==
                json::parse(R"({"print":{"command":"close_air_filt","sequence_id":"77"}})"));
        REQUIRE(build_ams_drying_stop(seq) ==
                json::parse(R"({"print":{"command":"auto_stop_ams_dry","sequence_id":"77"}})"));
    }

    SECTION("the close-box ack goes on the system topic with the eight-hex code")
    {
        // Note the "err" here IS the eight-hex form - this one command spells it differently from
        // the resume family above, which is upstream's doing and the reason the code is formatted
        // inside the builder rather than by the caller.
        const json built = build_clean_print_error_uiop(0x0C00402D, seq);

        REQUIRE(built == json::parse(R"({"system":{
            "command": "uiop", "sequence_id": "77", "name": "print_error",
            "action": "close", "source": 1, "type": "dialog", "err": "0C00402D"}})"));
    }
}

TEST_CASE("Proceed and don't-remind build the ignore lists upstream sends", "[HmsActions]")
{
    // The blob the dialog is handed with one of these errors: the command to re-send and the
    // index of the error to suppress.
    const json action = json::parse(R"({"command": "resume", "err_index": 3})");

    SECTION("proceed re-sends the caller's blob with mode 0 - always ignore")
    {
        json        out;
        std::string why;
        REQUIRE(build_ack_proceed(action, "77", out, why));

        REQUIRE(out["print"]["command"] == "resume");
        REQUIRE(out["print"]["err_code"] == 0);
        REQUIRE(out["print"]["err_ignored"] == json::array({3}));
        REQUIRE(out["print"]["rm_idx"] == json::parse(R"([{"idx": 3, "mode": 0}])"));
        REQUIRE(out["print"]["sequence_id"] == "77");
    }

    SECTION("don't-remind uses mode 1 - next time ignore")
    {
        // The two modes are the difference between "this condition is fine, stop checking" and
        // "stop showing me the popup". Mixing them up would silence a real fault, so the mode is
        // asserted rather than assumed.
        json        out;
        std::string why;
        REQUIRE(build_dont_remind_next_time(action, "77", out, why));

        REQUIRE(out["print"]["command"] == "resume");
        REQUIRE(out["print"]["err_ignored"] == json::array({3}));
        REQUIRE(out["print"]["rm_idx"] == json::parse(R"([{"idx": 3, "mode": 1}])"));
        REQUIRE(out["print"]["sequence_id"] == "77");
    }

    SECTION("an existing err_ignored list is grown, not replaced")
    {
        const json with_list = json::parse(R"({"command": "resume", "err_index": 3, "err_ignored": [1, 2]})");

        json        out;
        std::string why;
        REQUIRE(build_ack_proceed(with_list, "77", out, why));

        REQUIRE(out["print"]["err_ignored"] == json::array({1, 2, 3}));
        REQUIRE(out["print"]["rm_idx"] ==
                json::parse(R"([{"idx":1,"mode":0},{"idx":2,"mode":0},{"idx":3,"mode":0}])"));
    }

    SECTION("a blob missing the fields either needs is refused, not sent half-built")
    {
        json        out;
        std::string why;

        REQUIRE_FALSE(build_ack_proceed(json::parse(R"({"err_index": 3})"), "77", out, why));
        REQUIRE_FALSE(why.empty());

        REQUIRE_FALSE(build_dont_remind_next_time(json::parse(R"({"command": "resume"})"), "77", out, why));
        REQUIRE_FALSE(why.empty());

        // The null blob is the common case - most errors carry no action json at all.
        REQUIRE_FALSE(build_ack_proceed(json(), "77", out, why));
        REQUIRE_FALSE(build_dont_remind_next_time(json(), "77", out, why));
    }
}
