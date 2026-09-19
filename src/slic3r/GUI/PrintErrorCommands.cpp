#include "PrintErrorCommands.hpp"

#include <algorithm>

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
