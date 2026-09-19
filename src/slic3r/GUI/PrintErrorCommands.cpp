#include "PrintErrorCommands.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

namespace {

// Every id the dialog has a button for, plus REMOVE_CLOSE_BTN which is a flag rather than a
// button. Kept as a sorted table so the lookup is a search rather than a switch nobody updates.
const int KNOWN_ACTIONS[] = {
    PrintErrorAction::RESUME_PRINTING,
    PrintErrorAction::RESUME_PRINTING_DEFECTS,
    PrintErrorAction::RESUME_PRINTING_PROBELM_SOLVED,
    PrintErrorAction::STOP_PRINTING,
    PrintErrorAction::CHECK_ASSISTANT,
    PrintErrorAction::FILAMENT_EXTRUDED,
    PrintErrorAction::RETRY_FILAMENT_EXTRUDED,
    PrintErrorAction::CONTINUE,
    PrintErrorAction::LOAD_VIRTUAL_TRAY,
    PrintErrorAction::OK_BUTTON,
    PrintErrorAction::FILAMENT_LOAD_RESUME,
    PrintErrorAction::JUMP_TO_LIVEVIEW,
    PrintErrorAction::NO_REMINDER_NEXT_TIME,
    PrintErrorAction::REFRESH_NOZZLE,
    PrintErrorAction::IGNORE_NO_REMINDER_NEXT_TIME,
    PrintErrorAction::IGNORE_RESUME,
    PrintErrorAction::PROBLEM_SOLVED_RESUME,
    PrintErrorAction::TURN_OFF_FIRE_ALARM,
    PrintErrorAction::RETRY_PROBLEM_SOLVED,
    PrintErrorAction::STOP_DRYING,
    PrintErrorAction::CANCEL_ACTION,
    PrintErrorAction::REMOVE_CLOSE_BTN,
    PrintErrorAction::PROCEED,
    PrintErrorAction::OK_JUMP_RACK,
    PrintErrorAction::ABORT,
    PrintErrorAction::DISABLE_PURIFICATION,
    PrintErrorAction::DONT_REMIND_NEXT_TIME,
};

} // namespace

bool print_error_action_is_known(int action_id)
{
    return std::find(std::begin(KNOWN_ACTIONS), std::end(KNOWN_ACTIONS), action_id) != std::end(KNOWN_ACTIONS);
}

bool print_error_action_needs_job_id(int action_id)
{
    switch (action_id) {
    case PrintErrorAction::RESUME_PRINTING:
    case PrintErrorAction::RESUME_PRINTING_DEFECTS:
    case PrintErrorAction::RESUME_PRINTING_PROBELM_SOLVED:
    case PrintErrorAction::STOP_PRINTING:
    case PrintErrorAction::FILAMENT_LOAD_RESUME:
    case PrintErrorAction::IGNORE_NO_REMINDER_NEXT_TIME:
    case PrintErrorAction::IGNORE_RESUME:
    case PrintErrorAction::PROBLEM_SOLVED_RESUME:
        return true;
    default:
        return false;
    }
}

std::vector<int> generic_print_error_actions()
{
    return std::vector<int>{PrintErrorAction::STOP_PRINTING,
                            PrintErrorAction::RESUME_PRINTING,
                            PrintErrorAction::OK_BUTTON};
}

std::vector<int> resolve_print_error_actions(const std::vector<int>& table_actions, bool& used_fallback)
{
    std::vector<int> out;
    out.reserve(table_actions.size());
    for (int id : table_actions) {
        if (!print_error_action_is_known(id)) continue;
        if (std::find(out.begin(), out.end(), id) != out.end()) continue; // a table can repeat one
        out.push_back(id);
    }

    // REMOVE_CLOSE_BTN on its own is a flag, not something to press: a code whose only surviving
    // action is "hide the close box" would leave a window with no way out at all.
    const bool has_pressable = std::any_of(out.begin(), out.end(), [](int id) {
        return id != PrintErrorAction::REMOVE_CLOSE_BTN;
    });

    if (!has_pressable) {
        used_fallback = true;
        std::vector<int> fallback = generic_print_error_actions();
        // A REMOVE_CLOSE_BTN the table asked for is still honoured alongside the generic set.
        if (std::find(out.begin(), out.end(), PrintErrorAction::REMOVE_CLOSE_BTN) != out.end())
            fallback.push_back(PrintErrorAction::REMOVE_CLOSE_BTN);
        return fallback;
    }

    used_fallback = false;
    return out;
}

std::string format_action_ids(const std::vector<int>& ids)
{
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(ids[i]);
    }
    return out;
}

// ---- the remote surface ----

namespace {

// One row per action id: the verb the control route takes, the label, and whether a person out of
// the room can judge the press. The labels are the dialog's own (ReleaseNote.cpp's
// init_button_list) so the hub page, the app and the desktop all say the same words - with one
// deliberate difference: "Ignore this and Resume" gains the consequence in the label, because on
// the desktop the dialog's body text is right above the button and on a phone card it is not.
struct Row
{
    int         id;
    const char* verb;   // "" - no remote meaning at all
    const char* label;
    bool        remote_safe;
};

const Row ROWS[] = {
    // The resume family: all one command_hms_resume, three labels, all remote-safe. This is the
    // whole point of the feature - a printer stopped on a recoverable fault, resumed from a phone.
    { PrintErrorAction::RESUME_PRINTING,                "resume_error",      "Resume printing",                          true },
    { PrintErrorAction::RESUME_PRINTING_DEFECTS,        "resume_error",      "Resume printing (defects acceptable)",     true },
    { PrintErrorAction::RESUME_PRINTING_PROBELM_SOLVED, "resume_error",      "Problem solved, resume printing",          true },
    { PrintErrorAction::FILAMENT_LOAD_RESUME,           "resume_error",      "Filament loaded, resume printing",         true },
    { PrintErrorAction::PROBLEM_SOLVED_RESUME,          "resume_error",      "Problem solved and resume",                true },

    { PrintErrorAction::STOP_PRINTING,                  "stop_error",        "Stop the print",                           true },

    // Ignore never reads as a fix. The print continues and the fault is still there; a label that
    // said only "Ignore" next to "Resume printing" would look like the same thing with fewer words.
    { PrintErrorAction::IGNORE_RESUME,                  "ignore_error",      "Ignore this error and continue (the fault is not fixed)", true },
    { PrintErrorAction::IGNORE_NO_REMINDER_NEXT_TIME,   "ignore_error",      "Ignore and continue, don't ask again (the fault is not fixed)", true },

    // Silences the popup, touches nothing on the printer.
    { PrintErrorAction::NO_REMINDER_NEXT_TIME,          "idle_ignore_error", "Don't remind me next time (the fault is not fixed)", true },

    // Proceed / Don't remind carry a JSON blob the printer handed out with the error (the command
    // to re-send and the index to suppress). This fork never receives that blob - nothing calls
    // PrintErrorDialog::set_action_json, so even the desktop button sends nothing - so the verbs
    // exist and the route knows them, but they are not offered until a source for the blob does.
    { PrintErrorAction::PROCEED,                        "ack_proceed",       "Proceed",                                  false },
    { PrintErrorAction::DONT_REMIND_NEXT_TIME,          "dont_remind",       "Don't remind me (the fault is not fixed)", false },

    // Acknowledge and close: the printer is told the dialog went away. OK_JUMP_RACK is upstream's
    // rack-page variant of the same acknowledge; this fork has no rack page, so it is the plain one.
    { PrintErrorAction::OK_BUTTON,                      "ack_close",         "OK",                                       true },
    { PrintErrorAction::OK_JUMP_RACK,                   "ack_close",         "OK",                                       true },

    // ---- described, never offered remotely in this phase ----
    //
    // Every one of these moves or silences hardware whose state a remote tap cannot see. "Filament
    // Extruded, Continue" is the clearest: it tells the printer the filament is clear of the path,
    // and answering that from another room is how a purge gets crushed into a print.
    { PrintErrorAction::FILAMENT_EXTRUDED,              "",                  "Filament extruded, continue",              false },
    { PrintErrorAction::RETRY_FILAMENT_EXTRUDED,        "",                  "Not extruded yet, retry",                  false },
    { PrintErrorAction::CONTINUE,                       "",                  "Finished, continue",                       false },
    { PrintErrorAction::RETRY_PROBLEM_SOLVED,           "",                  "Retry (problem solved)",                   false },
    { PrintErrorAction::ABORT,                          "",                  "Abort",                                    false },
    { PrintErrorAction::STOP_DRYING,                    "",                  "Stop drying",                              false },
    { PrintErrorAction::REFRESH_NOZZLE,                 "",                  "Recheck the nozzle",                       false },
    { PrintErrorAction::TURN_OFF_FIRE_ALARM,            "",                  "Turn off the fire alarm",                  false },
    { PrintErrorAction::DISABLE_PURIFICATION,           "",                  "Disable purification for this print",      false },

    // ---- no remote meaning at all: desktop navigation, or not a button ----
    { PrintErrorAction::CHECK_ASSISTANT,                "",                  "Check the assistant",                      false },
    { PrintErrorAction::JUMP_TO_LIVEVIEW,               "",                  "View the camera",                          false },
    { PrintErrorAction::LOAD_VIRTUAL_TRAY,              "",                  "Load filament",                            false },
    { PrintErrorAction::CANCEL_ACTION,                  "",                  "Cancel",                                   false },
    { PrintErrorAction::REMOVE_CLOSE_BTN,               "",                  "",                                         false },
};

const Row* find_row(int action_id)
{
    for (const Row& r : ROWS)
        if (r.id == action_id) return &r;
    return nullptr;
}

// The verb -> job_id rule, kept on the verb rather than on the id: several ids share one verb and
// the command behind the verb is what decides, not the label on the button.
struct VerbRow
{
    const char* verb;
    bool        needs_job_id;
    bool        remote_safe;
};

const VerbRow VERBS[] = {
    { "resume_error",      true,  true },
    { "stop_error",        true,  true },
    { "ignore_error",      true,  true },
    { "idle_ignore_error", false, true },
    // Not remote-safe: no source for the action_json blob these need (see ROWS above).
    { "ack_proceed",       false, false },
    { "dont_remind",       false, false },
    { "ack_close",         false, true },
};

const VerbRow* find_verb(const std::string& verb)
{
    for (const VerbRow& v : VERBS)
        if (verb == v.verb) return &v;
    return nullptr;
}

} // namespace

bool is_print_error_verb(const std::string& verb) { return find_verb(verb) != nullptr; }

bool is_remote_safe_verb(const std::string& verb)
{
    const VerbRow* v = find_verb(verb);
    return v && v->remote_safe;
}

bool print_error_verb_needs_job_id(const std::string& verb)
{
    const VerbRow* v = find_verb(verb);
    return v && v->needs_job_id;
}

PrintErrorRemoteAction describe_print_error_action(int action_id)
{
    PrintErrorRemoteAction out;
    out.id = action_id;
    const Row* r = find_row(action_id);
    if (!r) return out; // an id no dialog draws: no verb, no label, not safe
    out.verb         = r->verb;
    out.label        = r->label;
    out.remote_safe  = r->remote_safe && r->verb[0] != '\0';
    out.needs_job_id = print_error_verb_needs_job_id(out.verb);
    return out;
}

std::vector<PrintErrorRemoteAction> describe_print_error_actions(const std::vector<int>& resolved_actions,
                                                                 bool has_job_id)
{
    std::vector<PrintErrorRemoteAction> out;
    out.reserve(resolved_actions.size());
    for (int id : resolved_actions) {
        // Not a button on any surface: the close-box flag and the ids nothing can draw.
        if (id == PrintErrorAction::REMOVE_CLOSE_BTN) continue;
        PrintErrorRemoteAction a = describe_print_error_action(id);
        if (a.label.empty()) continue;
        // A resume the printer has no job for would be dropped by firmware, so it is described and
        // not offered rather than offered and refused.
        if (a.needs_job_id && !has_job_id) a.remote_safe = false;
        out.push_back(a);
    }
    return out;
}

// ---- the control route's guards ----

std::string normalize_error_code(const std::string& code)
{
    std::string out;
    out.reserve(code.size());
    for (char c : code) {
        if (c == ' ' || c == '-' || c == '_') continue;
        out.push_back((char) std::toupper((unsigned char) c));
    }
    return out;
}

namespace {

// "0C00402D" -> "0C00 402D". HMSQuery::pretty_code does the same for the GUI; this is the
// wx-free copy, because a header that pulls GUI_App in cannot be unit tested.
std::string group4(const std::string& code)
{
    if (code.size() != 8 && code.size() != 16) return code;
    std::string out;
    out.reserve(code.size() + code.size() / 4);
    for (size_t i = 0; i < code.size(); ++i) {
        if (i && i % 4 == 0) out.push_back(' ');
        out.push_back(code[i]);
    }
    return out;
}

std::string named(const std::string& printer_name)
{
    return printer_name.empty() ? std::string("The printer") : printer_name;
}

} // namespace

int check_print_error_action(const PrintErrorActionRequest& req, std::string& why)
{
    why.clear();
    if (!is_print_error_verb(req.verb)) {
        why = "\"" + req.verb + "\" is not a printer-error action";
        return 400;
    }
    if (!is_remote_safe_verb(req.verb)) {
        why = "\"" + req.verb + "\" has to be done at the printer: it moves or silences hardware "
              "whose state cannot be checked from here";
        return 403;
    }
    if (req.asked_err.empty()) {
        why = "this action needs err=<the error code it is answering>";
        return 400;
    }
    // Stopping throws the print away, whichever button asked for it.
    if (req.verb == "stop_error" && !req.confirm) {
        why = "stopping a print needs confirm=1";
        return 400;
    }
    if (req.current_err.empty()) {
        why = named(req.printer_name) + " is not reporting an error any more, so there is nothing to answer";
        return 409;
    }
    // The stale-error guard. The client's spelling is normalised; the printer's is taken as it is,
    // because it was made by the same formatter that produced the status JSON.
    const std::string asked = normalize_error_code(req.asked_err);
    if (asked != req.current_err) {
        why = named(req.printer_name) + " is reporting error " + group4(req.current_err) + " now, not " +
              group4(asked) + "; reload the printer's status and try again";
        return 409;
    }
    if (print_error_verb_needs_job_id(req.verb) && req.job_id.empty()) {
        why = named(req.printer_name) + " is not reporting a job for this error, and " + req.verb +
              " has to name one - the printer would ignore it";
        return 409;
    }
    // And the verb has to be one this error actually offers. Without this a client could resume an
    // error whose only action is OK, which is the one thing the desktop dialog cannot do either.
    bool offered = false;
    for (const PrintErrorRemoteAction& a : describe_print_error_actions(req.offered, !req.job_id.empty())) {
        if (a.remote_safe && a.verb == req.verb) { offered = true; break; }
    }
    if (!offered) {
        why = named(req.printer_name) + " does not offer \"" + req.verb + "\" for error " + group4(req.current_err);
        return 409;
    }
    return 0;
}

// ---- payload builders ----

json build_hms_resume(const std::string& err, const std::string& job_id, const std::string& sequence_id)
{
    json j;
    j["print"]["command"]     = "resume";
    j["print"]["err"]         = err;
    j["print"]["param"]       = "reserve";
    j["print"]["job_id"]      = job_id;
    j["print"]["sequence_id"] = sequence_id;
    return j;
}

json build_hms_stop(const std::string& err, const std::string& job_id, const std::string& sequence_id)
{
    json j;
    j["print"]["command"]     = "stop";
    j["print"]["err"]         = err;
    j["print"]["param"]       = "reserve";
    j["print"]["job_id"]      = job_id;
    j["print"]["sequence_id"] = sequence_id;
    return j;
}

json build_hms_ignore(const std::string& err, const std::string& job_id, const std::string& sequence_id)
{
    json j;
    j["print"]["command"]     = "ignore";
    j["print"]["err"]         = err;
    j["print"]["param"]       = "reserve";
    j["print"]["job_id"]      = job_id;
    j["print"]["sequence_id"] = sequence_id;
    return j;
}

json build_hms_idle_ignore(const std::string& err, int type, const std::string& sequence_id)
{
    json j;
    j["print"]["command"]     = "idle_ignore";
    j["print"]["err"]         = err;
    j["print"]["type"]        = type;
    j["print"]["sequence_id"] = sequence_id;
    return j;
}

json build_refresh_nozzle(const std::string& sequence_id)
{
    json j;
    j["print"]["sequence_id"] = sequence_id;
    j["print"]["command"]     = "refresh_nozzle";
    return j;
}

json build_stop_buzzer(const std::string& sequence_id)
{
    json j;
    j["print"]["command"]     = "buzzer_ctrl";
    j["print"]["mode"]        = 0;
    j["print"]["sequence_id"] = sequence_id;
    return j;
}

json build_purification_disable(const std::string& sequence_id)
{
    json j;
    j["print"]["command"]     = "close_air_filt";
    j["print"]["sequence_id"] = sequence_id;
    return j;
}

json build_ams_drying_stop(const std::string& sequence_id)
{
    json j;
    j["print"]["command"]     = "auto_stop_ams_dry";
    j["print"]["sequence_id"] = sequence_id;
    return j;
}

json build_clean_print_error_uiop(int print_error, const std::string& sequence_id)
{
    char buf[32];
    ::sprintf(buf, "%08X", print_error);

    json j;
    j["system"]["command"]     = "uiop";
    j["system"]["sequence_id"] = sequence_id;
    j["system"]["name"]        = "print_error";
    j["system"]["action"]      = "close";
    j["system"]["source"]      = 1; // 0-Mushu 1-Studio
    j["system"]["type"]        = "dialog";
    j["system"]["err"]         = std::string(buf);
    return j;
}

namespace {

// The shared half of proceed / don't-remind: grow "err_ignored" with this error's index, then
// mirror it into "rm_idx" as {idx, mode} entries. `mode` is the only thing that differs.
bool build_ignore_list(const json& action_json, int mode, json& print_block, std::string& error)
{
    if (!action_json.is_object()) {
        error = "action json is not an object";
        return false;
    }
    if (!action_json.contains("command") || !action_json["command"].is_string() ||
        action_json["command"].get<std::string>().empty()) {
        error = "action json has no \"command\"";
        return false;
    }
    if (!action_json.contains("err_index") || !action_json["err_index"].is_number_integer()) {
        error = "action json has no integer \"err_index\"";
        return false;
    }

    const int err_index = action_json["err_index"].get<int>();

    json ignored = json::array();
    if (action_json.contains("err_ignored") && action_json["err_ignored"].is_array())
        ignored = action_json["err_ignored"];
    ignored.push_back(err_index);

    print_block["err_ignored"] = ignored;

    json rm_idx = json::array();
    for (const auto& item : ignored) {
        if (!item.is_number_integer()) continue;
        json entry;
        entry["idx"]  = item.get<int>();
        entry["mode"] = mode;
        rm_idx.push_back(entry);
    }
    print_block["rm_idx"] = rm_idx;
    return true;
}

} // namespace

bool build_ack_proceed(const json& action_json, const std::string& sequence_id, json& out, std::string& error)
{
    // Upstream re-sends the whole blob it was handed, with err_code/err_ignored/rm_idx/sequence_id
    // layered on, so any extra fields the caller put there survive.
    if (!action_json.is_object()) {
        error = "action json is not an object";
        return false;
    }
    json proceed = action_json;
    proceed["err_code"] = 0;
    if (!build_ignore_list(action_json, /*mode=*/0, proceed, error)) return false;
    proceed["sequence_id"] = sequence_id;

    out = json();
    out["print"] = proceed;
    return true;
}

bool build_dont_remind_next_time(const json& action_json, const std::string& sequence_id, json& out, std::string& error)
{
    // This one rebuilds a fresh block rather than re-sending the caller's, keeping only the
    // command name - upstream does the same.
    if (!action_json.is_object()) {
        error = "action json is not an object";
        return false;
    }
    json print_block;
    if (!build_ignore_list(action_json, /*mode=*/1, print_block, error)) return false;
    print_block["command"]     = action_json["command"].get<std::string>();
    print_block["sequence_id"] = sequence_id;

    out = json();
    out["print"] = print_block;
    return true;
}

}} // namespace Slic3r::GUI
