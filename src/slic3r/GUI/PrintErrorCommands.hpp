#ifndef slic3r_PrintErrorCommands_hpp_
#define slic3r_PrintErrorCommands_hpp_

// The MQTT payloads behind the buttons on the printer-error dialog, built as pure functions.
//
// Why these are not just written inline in MachineObject::command_*: the shapes are the whole
// point. Firmware that gates an error-triggered resume on seeing "err" and "param":"reserve"
// silently ignores the generic {"command":"resume","param":""} the fork used to send from the
// error dialog, so the buttons looked like they worked and did nothing. The only way to keep the
// fork honest about that is to pin the exact JSON in a test, and the only way to test it without
// a live MachineObject and a broker is to build it somewhere that has neither.
//
// So MachineObject::command_hms_* is a two-liner - call the builder here, hand the dump() to
// publish_json - and the test calls the same builder with a fake err/job_id and compares against
// Bambu Studio's payloads verbatim.
//
// Sequence ids are passed in rather than taken from MachineObject::m_sequence_id, for the same
// reason: a builder that reaches for a global counter is not a pure function and its output is
// not comparable.

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace Slic3r {
namespace GUI {

// Every action id the dialog can render, mirroring Bambu Studio's DeviceErrorDialog::ActionButton.
// The shipped resources/hms/hms_action_<devtype>.json tables name these ids directly in their
// "actions" arrays; an id the dialog has no button for is dropped silently, which is exactly the
// bug that left a stuck printer with no Resume on screen.
namespace PrintErrorAction {
enum Id : int {
    RESUME_PRINTING                = 2,
    RESUME_PRINTING_DEFECTS        = 3,
    RESUME_PRINTING_PROBELM_SOLVED = 4,
    STOP_PRINTING                  = 5,
    CHECK_ASSISTANT                = 6,
    FILAMENT_EXTRUDED              = 7,
    RETRY_FILAMENT_EXTRUDED        = 8,
    CONTINUE                       = 9,
    LOAD_VIRTUAL_TRAY              = 10,
    OK_BUTTON                      = 11,
    FILAMENT_LOAD_RESUME           = 12,
    JUMP_TO_LIVEVIEW               = 13,

    NO_REMINDER_NEXT_TIME          = 23,
    REFRESH_NOZZLE                 = 24,
    IGNORE_NO_REMINDER_NEXT_TIME   = 25,
    IGNORE_RESUME                  = 27,
    PROBLEM_SOLVED_RESUME          = 28,
    TURN_OFF_FIRE_ALARM            = 29,

    RETRY_PROBLEM_SOLVED           = 34,
    STOP_DRYING                    = 35,
    CANCEL_ACTION                  = 37,
    REMOVE_CLOSE_BTN               = 39, // not a button: suppresses the window's close box
    PROCEED                        = 41,
    OK_JUMP_RACK                   = 49,
    ABORT                          = 51,
    DISABLE_PURIFICATION           = 54,
    DONT_REMIND_NEXT_TIME          = 57,
};
} // namespace PrintErrorAction

// True when this id needs a job_id to be meaningful. The resume/stop/ignore family carries
// "job_id" so firmware can check the command is for the job it currently holds; sending one with
// an empty job_id is the documented way to have it rejected, so the dialog disables those buttons
// rather than firing a command that cannot work.
bool print_error_action_needs_job_id(int action_id);

// True when the id is one of the buttons the dialog knows how to render at all.
bool print_error_action_is_known(int action_id);

// The button set for one error code, given that code's "actions" list as the shipped table spells
// it. Unknown ids are dropped (the tables carry ids like 14-22 and 43-48 that not even Bambu
// Studio has buttons for); REMOVE_CLOSE_BTN is kept, since the dialog reads it as a flag.
//
// When nothing survives - an unknown code, or a code whose actions are all ids we cannot render -
// the generic set {STOP_PRINTING, RESUME_PRINTING, OK_BUTTON} is returned instead, so a stuck
// printer always has something to press. `used_fallback` reports which of the two happened.
std::vector<int> resolve_print_error_actions(const std::vector<int>& table_actions, bool& used_fallback);

// The generic set, for callers that want it without a lookup.
std::vector<int> generic_print_error_actions();

// "2, 5, 11" - for the log line that records what a table asked for and what was drawn.
std::string format_action_ids(const std::vector<int>& ids);

// ---- payload builders ----
//
// `err` is the error code as the printer names it in these commands. Bambu Studio passes
// std::to_string(error_code) - the decimal spelling of print_error, not the 8-hex form the
// hms_action tables are keyed by - and that is reproduced here rather than corrected, because
// firmware matches on what upstream sends.

// {"print":{"command":"resume","err":<err>,"param":"reserve","job_id":<job>,"sequence_id":<seq>}}
nlohmann::json build_hms_resume(const std::string& err, const std::string& job_id, const std::string& sequence_id);

// {"print":{"command":"stop","err":<err>,"param":"reserve","job_id":<job>,"sequence_id":<seq>}}
nlohmann::json build_hms_stop(const std::string& err, const std::string& job_id, const std::string& sequence_id);

// {"print":{"command":"ignore","err":<err>,"param":"reserve","job_id":<job>,"sequence_id":<seq>}}
nlohmann::json build_hms_ignore(const std::string& err, const std::string& job_id, const std::string& sequence_id);

// {"print":{"command":"idle_ignore","err":<err>,"type":<type>,"sequence_id":<seq>}}
nlohmann::json build_hms_idle_ignore(const std::string& err, int type, const std::string& sequence_id);

// {"print":{"command":"refresh_nozzle","sequence_id":<seq>}}
nlohmann::json build_refresh_nozzle(const std::string& sequence_id);

// {"print":{"command":"buzzer_ctrl","mode":0,"sequence_id":<seq>}}
nlohmann::json build_stop_buzzer(const std::string& sequence_id);

// {"print":{"command":"close_air_filt","sequence_id":<seq>}}
nlohmann::json build_purification_disable(const std::string& sequence_id);

// {"print":{"command":"auto_stop_ams_dry","sequence_id":<seq>}}
nlohmann::json build_ams_drying_stop(const std::string& sequence_id);

// {"system":{"command":"uiop","name":"print_error","action":"close","source":1,"type":"dialog",
//            "err":"<8hex>","sequence_id":<seq>}}
// The dismiss-without-acting ack, sent when the window's close box is used.
nlohmann::json build_clean_print_error_uiop(int print_error, const std::string& sequence_id);

// The "proceed" / "don't remind" family. Both take a JSON blob the caller was handed with the
// error (it names the command to re-send and the index of the error to suppress) and grow an
// "err_ignored" list plus a parallel "rm_idx" list of {idx, mode} entries.
//
//   mode 0 - always ignore   (PROCEED)
//   mode 1 - next time ignore (DONT_REMIND_NEXT_TIME)
//
// Returns false with `error` set when the blob is missing the fields either needs, which is the
// case that used to throw out of the dialog.
bool build_ack_proceed(const nlohmann::json& action_json, const std::string& sequence_id,
                       nlohmann::json& out, std::string& error);
bool build_dont_remind_next_time(const nlohmann::json& action_json, const std::string& sequence_id,
                                 nlohmann::json& out, std::string& error);

}} // namespace Slic3r::GUI

#endif // slic3r_PrintErrorCommands_hpp_
