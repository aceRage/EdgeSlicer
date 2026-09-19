#include "HMS.hpp"

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

static const char* HMS_PATH = "hms";

namespace Slic3r {
namespace GUI {

// The version field, whichever way the server or an older local file spelled it. Upstream reads
// this through DevJsonValParser::get_longlong_val (DeviceCore/DevUtil.cpp:162), which this fork
// does not have; the body is the same.
static std::string hms_ver_string(const json& j)
{
    try {
        if (j.is_number())
            return std::to_string(j.get<long long>());
        else if (j.is_string())
            return j.get<std::string>();
    } catch (...) {
        ;
    }
    return std::string();
}

int get_hms_info_version(std::string& version)
{
    AppConfig* config = wxGetApp().app_config;
    if (!config)
        return -1;
    std::string hms_host = config->get_hms_host();
    if(hms_host.empty()) {
        BOOST_LOG_TRIVIAL(error) << "hms_host is empty";
        return -1;
    }
    int result = -1;
    version = "";
    std::string lang;
    std::string query_params = HMSQuery::build_query_params(lang);
    std::string url = (boost::format("https://%1%/GetVersion.php?%2%") % hms_host % query_params).str();
    Slic3r::Http http = Slic3r::Http::get(url);
    http.timeout_max(10)
        .on_complete([&result, &version](std::string body, unsigned status){
            try {
                json j = json::parse(body);
                if (j.contains("ver")) {
                    version = hms_ver_string(j["ver"]);
                }
            } catch (...) {
                ;
            }
        })
        .on_error([&result](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "get_hms_info_version: body = " << body << ", status = " << status << ", error = " << error;
            result = -1;
            })
        .perform_sync();
    return result;
}

// Note:  Download the hms into receive_json
int HMSQuery::download_hms_related(const std::string& hms_type, const std::string& dev_id_type, json* receive_json)
{
    std::string local_version = "0";
    std::string lang;
    std::string query_params = HMSQuery::build_query_params(lang);
    load_from_local(hms_type, dev_id_type, lang, receive_json, local_version);

    AppConfig* config = wxGetApp().app_config;
    if (!config) return -1;

    std::string hms_host = wxGetApp().app_config->get_hms_host();
    std::string url;
    if (hms_type.compare(QUERY_HMS_INFO) == 0) {
        url = (boost::format("https://%1%/query.php?%2%") % hms_host % query_params).str();
    }
    else if (hms_type.compare(QUERY_HMS_ACTION) == 0) {
        url = (boost::format("https://%1%/hms/GetActionImage.php?") % hms_host).str();
    }

    if (!local_version.empty()) { url += (url.find('?') != std::string::npos ? "&" : "?") + (boost::format("v=%1%") % local_version).str(); }

    if (!dev_id_type.empty()) { url += (url.find('?') != std::string::npos ? "&" : "?") + (boost::format("d=%1%") % dev_id_type).str(); }

    BOOST_LOG_TRIVIAL(info) << "hms: download url = " << url;

    bool to_save_local = false;
    json j;

    Slic3r::Http http = Slic3r::Http::get(url);
    http.on_complete([receive_json, hms_type, &to_save_local, &j, &local_version](std::string body, unsigned status) {
        try {
            j = json::parse(body);
            if (j.contains("result")) {
                if (j["result"] == 0 && j.contains("data")) {

                    if (!j.contains("ver"))
                    {
                        return;
                    }

                    const std::string& remote_ver = hms_ver_string(j["ver"]);
                    if (remote_ver <= local_version)
                    {
                        return;
                    }
                    (*receive_json)["version"] = remote_ver;

                    if (hms_type.compare(QUERY_HMS_INFO) == 0)
                    {
                        (*receive_json) = j["data"];
                        to_save_local = true;
                    }
                    else if (hms_type.compare(QUERY_HMS_ACTION) == 0)
                    {
                        (*receive_json)["data"] = j["data"];
                        to_save_local = true;
                    }
                } else if (j["result"] == 201){
                    BOOST_LOG_TRIVIAL(info) << "HMSQuery: HMS info is the latest version";
                }else{
                    BOOST_LOG_TRIVIAL(info) << "HMSQuery: update hms info error = " << j["result"].get<int>();
                }
            }
        } catch (...) {
            ;
        }
        })
        .timeout_max(20)
        .on_error([](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "HMSQuery: update hms info error = " << error << ", body = " << body << ", status = " << status;
        }).perform_sync();

        if (to_save_local && !receive_json->empty()) {
            save_to_local(lang, hms_type, dev_id_type, j);
        }
    return 0;
}

// Where a table may be read from, best first. Upstream copies the whole resources/hms tree into
// the data dir instead (HMS.cpp copy_from_data_dir_to_local, STUDIO-9512) so that one path serves
// both; here resources is read in place - the shipped tables are 120 MB and duplicating them into
// everybody's AppData to answer one error code is not worth it. The third candidate is this fork's
// own history: the tables it downloaded before they were split per device were written without a
// series in the name, and an X1 or P1 that has one must keep reading it.
static std::vector<boost::filesystem::path> hms_file_candidates(const std::string& hms_type,
                                                                const std::string& dev_id_type,
                                                                const std::string& lang)
{
    std::vector<fs::path> out;
    const std::string with_dev = HMSQuery::get_hms_file(hms_type, lang, dev_id_type);
    if (!data_dir().empty()) out.push_back(fs::path(data_dir()) / HMS_PATH / with_dev);
    if (!resources_dir().empty()) out.push_back(fs::path(resources_dir()) / HMS_PATH / with_dev);
    if (!dev_id_type.empty()) {
        const std::string legacy = HMSQuery::get_hms_file(hms_type, lang, std::string());
        if (!data_dir().empty()) out.push_back(fs::path(data_dir()) / HMS_PATH / legacy);
    }
    return out;
}

int HMSQuery::load_from_local(const std::string& hms_type,
                              const std::string& dev_id_type,
                              const std::string& lang,
                              json*              load_json,
                              std::string&       load_version)
{
    load_version = "0";
    if (data_dir().empty()) {
        BOOST_LOG_TRIVIAL(error) << "HMS: load_from_local, data_dir() is empty";
        return -1;
    }
    auto hms_folder = (fs::path(data_dir()) / HMS_PATH);
    if (!fs::exists(hms_folder))
        fs::create_directory(hms_folder);

    for (const fs::path& candidate : hms_file_candidates(hms_type, dev_id_type, lang)) {
        const std::string dir_str = fs::path(candidate).make_preferred().string();
        std::ifstream json_file(encode_path(dir_str.c_str()));
        if (!json_file.is_open())
            continue;
        try {
            const json& j = json::parse(json_file);
            // The shipped and downloaded files carry the server's envelope, {"result","ver","data"}.
            // A file this fork wrote before the port has the payload at the top level instead, so
            // both shapes are accepted.
            if (hms_type.compare(QUERY_HMS_INFO) == 0) {
                if (j.contains("data")) { (*load_json) = j["data"]; }
                else if (j.contains("device_hms") || j.contains("device_error")) { (*load_json) = j; }
            } else if (hms_type.compare(QUERY_HMS_ACTION) == 0) {
                if (j.contains("data")) { (*load_json)["data"] = j["data"]; }
            }

            if (j.contains("version")) {
                load_version = hms_ver_string(j["version"]);
            }
            else if (j.contains("ver")) {
                load_version = hms_ver_string(j["ver"]);
            }
            else
            {
                BOOST_LOG_TRIVIAL(warning) << "HMS: load_from_local, no version info";
            }

            BOOST_LOG_TRIVIAL(info) << "HMS: loaded " << dir_str << ", version = " << load_version;
            return 0;
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "HMS: load_from_local failed for " << dir_str;
        }
    }

    load_version = "0";
    return 0;
}

int HMSQuery::save_to_local(const std::string& lang, const std::string& hms_type, const std::string& dev_id_type, const json& save_json)
{
    if (data_dir().empty()) {
        BOOST_LOG_TRIVIAL(error) << "HMS: save_to_local, data_dir() is empty";
        return -1;
    }
    std::string filename = get_hms_file(hms_type, lang, dev_id_type);
    auto hms_folder = (boost::filesystem::path(data_dir()) / HMS_PATH);
    if (!fs::exists(hms_folder))
        fs::create_directory(hms_folder);
    std::string dir_str = (hms_folder / filename).make_preferred().string();
    std::ofstream json_file(encode_path(dir_str.c_str()));
    if (json_file.is_open()) {
        json_file << std::setw(4) << save_json << std::endl;
        json_file.close();
        return 0;
    }
    BOOST_LOG_TRIVIAL(error) << "HMS: save_to_local failed";
    return -1;
}

std::string HMSQuery::hms_language_code()
{
    AppConfig* config = wxApp::GetInstance() ? wxGetApp().app_config : nullptr;
    if (!config)
        // set language code to en by default
        return "en";
    std::string lang_code = config->get_language_code();
    if (lang_code.compare("uk") == 0
        || lang_code.compare("cs") == 0
        || lang_code.compare("ru") == 0) {
        BOOST_LOG_TRIVIAL(info) << "HMS: using english for lang_code = " << lang_code;
        return "en";
    }
    else if (lang_code.empty()) {
        // set language code to en by default
        return "en";
    }
    return lang_code;
}

std::string HMSQuery::build_query_params(std::string& lang)
{
    std::string lang_code = HMSQuery::hms_language_code();
    lang = lang_code;
    std::string query_params = (boost::format("lang=%1%") % lang_code).str();
    return query_params;
}

std::string HMSQuery::get_dev_id_type(const std::string& dev_id)
{
    //The first three digits of SN number
    return dev_id.size() >= 3 ? dev_id.substr(0, 3) : dev_id;
}

std::string HMSQuery::get_hms_file(const std::string& hms_type, const std::string& lang, const std::string& dev_id_type)
{
    //return hms action filename
    if (hms_type.compare(QUERY_HMS_ACTION) == 0) {
        if (dev_id_type.empty())
            return (boost::format("hms_action.json")).str();
        return (boost::format("hms_action_%1%.json") % dev_id_type).str();
    }
    //return hms filename
    if (dev_id_type.empty())
        return (boost::format("hms_%1%.json") % lang).str();
    return (boost::format("hms_%1%_%2%.json") % lang % dev_id_type).str();
}

wxString HMSQuery::query_hms_msg(const std::string& dev_id, const std::string& long_error_code)
{
    const std::string lang_code = HMSQuery::hms_language_code();
    return _query_hms_msg(get_dev_id_type(dev_id), long_error_code, lang_code);
}

wxString HMSQuery::query_hms_msg_local(const std::string& dev_id, const std::string& long_error_code, const std::string& lang_code)
{
    if (long_error_code.empty()) return wxEmptyString;
    const std::string dev_id_type = get_dev_id_type(dev_id);

    std::unique_lock<std::mutex> lock(m_hms_mutex);
    load_local_tables(dev_id_type, lang_code);
    auto iter = m_hms_info_jsons.find(dev_id_type);
    if (iter == m_hms_info_jsons.end()) return wxEmptyString;
    return _find_hms_msg(iter->second, long_error_code, lang_code);
}

wxString HMSQuery::_query_hms_msg(const std::string& dev_id_type, const std::string& long_error_code, const std::string& lang_code)
{
    if (long_error_code.empty())
    {
        return wxEmptyString;
    }

    init_hms_info(dev_id_type, lang_code);

    std::unique_lock<std::mutex> lock(m_hms_mutex);
    auto iter = m_hms_info_jsons.find(dev_id_type);
    if (iter == m_hms_info_jsons.end())
    {
        BOOST_LOG_TRIVIAL(error) << "there are no hms info for the device";
        return wxEmptyString;
    }

    return _find_hms_msg(iter->second, long_error_code, lang_code);
}

// The lookup itself, on a table that is already in hand. m_hms_mutex is held by the caller.
wxString HMSQuery::_find_hms_msg(const json& m_hms_info_json, const std::string& long_error_code, const std::string& lang_code)
{
    if (!m_hms_info_json.is_object())
    {
        BOOST_LOG_TRIVIAL(error) << "the hms info is not a valid json object";
        return wxEmptyString;
    }

    const json& device_hms_json = m_hms_info_json.value("device_hms", json());
    if (device_hms_json.is_null() || !device_hms_json.is_object())
    {
        BOOST_LOG_TRIVIAL(error) << "there are no valid json object named device_hms";
        return wxEmptyString;
    }

    // One language's list. Empty when that language has no entry for the code, or has one with an
    // empty `intro` - which Bambu does publish, so "found" is not the same as "has something to say".
    auto in_language = [&](const std::string& lang) -> wxString {
        const json& list = device_hms_json.value(lang, json());
        if (!list.is_array()) return wxEmptyString;
        for (const auto& item : list) {
            if (!item.is_object()) continue;
            const std::string& error_code = item.value("ecode", std::string());
            if (boost::to_upper_copy(error_code) != long_error_code) continue;
            const std::string intro = item.value("intro", std::string());
            if (!intro.empty()) return wxString::FromUTF8(intro);
        }
        return wxEmptyString;
    };

    // The app's language, then English, then any language that has the code. The fallback used to
    // run only when the language key was missing entirely, so a table that carried `de` but had
    // this particular code only under `en` showed the owner nothing at all.
    wxString text = in_language(lang_code);
    if (!text.IsEmpty()) return text;
    if (lang_code != "en") {
        text = in_language("en");
        if (!text.IsEmpty()) return text;
    }
    for (auto it = device_hms_json.begin(); it != device_hms_json.end(); ++it) {
        if (it.key() == lang_code || it.key() == "en") continue;
        text = in_language(it.key());
        if (!text.IsEmpty()) {
            BOOST_LOG_TRIVIAL(info) << "hms: " << long_error_code << " answered from lang_code = " << it.key();
            return text;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "hms: query_hms_msg, no description for lang_code = " << lang_code
                            << " long_error_code = " << long_error_code;
    return wxEmptyString;
}

wxString HMSQuery::_query_error_msg(const std::string& dev_id_type, const std::string& error_code, const std::string& lang_code)
{
    init_hms_info(dev_id_type, lang_code);

    std::unique_lock<std::mutex> lock(m_hms_mutex);
    auto iter = m_hms_info_jsons.find(dev_id_type);
    if (iter == m_hms_info_jsons.end())
    {
        return wxEmptyString;
    }

    return _find_error_msg(iter->second, error_code, lang_code);
}

// m_hms_mutex is held by the caller.
wxString HMSQuery::_find_error_msg(const json& m_hms_info_json, const std::string& error_code, const std::string& lang_code)
{
    if (!m_hms_info_json.is_object()) return wxEmptyString;
    if (!m_hms_info_json.contains("device_error")) {
        BOOST_LOG_TRIVIAL(info) << "device_error is not exists";
        return wxEmptyString;
    }
    const json& device_error = m_hms_info_json["device_error"];
    if (!device_error.is_object()) return wxEmptyString;

    // Same rule as _find_hms_msg: a code the requested language has nothing for is still answered
    // from English, or from any language that does carry it. An entry with an empty `intro`
    // counts as nothing - Bambu publishes those.
    auto in_language = [&](const std::string& lang) -> wxString {
        const json& list = device_error.value(lang, json());
        if (!list.is_array()) return wxEmptyString;
        for (const auto& item : list) {
            if (!item.is_object()) continue;
            const std::string& ecode = item.value("ecode", std::string());
            if (boost::to_upper_copy(ecode) != error_code) continue;
            const std::string intro = item.value("intro", std::string());
            if (!intro.empty()) return wxString::FromUTF8(intro);
        }
        return wxEmptyString;
    };

    wxString text = in_language(lang_code);
    if (!text.IsEmpty()) return text;
    if (lang_code != "en") {
        text = in_language("en");
        if (!text.IsEmpty()) return text;
    }
    for (auto it = device_error.begin(); it != device_error.end(); ++it) {
        if (it.key() == lang_code || it.key() == "en") continue;
        text = in_language(it.key());
        if (!text.IsEmpty()) {
            BOOST_LOG_TRIVIAL(info) << "hms: " << error_code << " answered from lang_code = " << it.key();
            return text;
        }
    }

    BOOST_LOG_TRIVIAL(info) << "hms: query_error_msg, no description for error_code = " << error_code;
    return wxEmptyString;
}

wxString HMSQuery::_query_error_url_action(const std::string& dev_id_type, const std::string& long_error_code, std::vector<int>& button_action)
{
    init_hms_info(dev_id_type, HMSQuery::hms_language_code());

    std::unique_lock<std::mutex> lock(m_hms_mutex);
    auto iter = m_hms_action_jsons.find(dev_id_type);
    if (iter == m_hms_action_jsons.end())
    {
        return wxEmptyString;
    }

    const json& m_hms_action_json = iter->second;
    if (m_hms_action_json.contains("data")) {
        for (auto item = m_hms_action_json["data"].begin(); item != m_hms_action_json["data"].end(); item++) {
            if (item->contains("ecode") && boost::to_upper_copy((*item)["ecode"].get<std::string>()) == long_error_code) {
                if (item->contains("device") && (boost::to_upper_copy((*item)["device"].get<std::string>()) == dev_id_type ||
                    (*item)["device"].get<std::string>() == "default")) {
                    if (item->contains("actions")) {
                        for (auto item_actions = (*item)["actions"].begin(); item_actions != (*item)["actions"].end(); item_actions++) {
                            button_action.emplace_back(item_actions->get<int>());
                        }
                    }
                    if (item->contains("image")) {
                        return wxString::FromUTF8((*item)["image"].get<std::string>());
                    }
                }
            }
        }
    }
    else {
        BOOST_LOG_TRIVIAL(info) << "data is not exists";
        return wxEmptyString;
    }
    return wxEmptyString;
}

bool HMSQuery::query_print_error_msg(const std::string& dev_id, int print_error, wxString& error_msg)
{
    char buf[32];
    ::sprintf(buf, "%08X", print_error);
    std::string lang_code = HMSQuery::hms_language_code();
    error_msg = _query_error_msg(get_dev_id_type(dev_id), std::string(buf), lang_code);
    return !error_msg.IsEmpty();
}

bool HMSQuery::query_print_error_msg_local(const std::string& dev_id, int print_error, const std::string& lang_code, wxString& error_msg)
{
    char buf[32];
    ::sprintf(buf, "%08X", print_error);
    const std::string dev_id_type = get_dev_id_type(dev_id);

    error_msg = wxEmptyString;
    std::unique_lock<std::mutex> lock(m_hms_mutex);
    load_local_tables(dev_id_type, lang_code);
    auto iter = m_hms_info_jsons.find(dev_id_type);
    if (iter == m_hms_info_jsons.end()) return false;
    error_msg = _find_error_msg(iter->second, std::string(buf), lang_code);
    return !error_msg.IsEmpty();
}

wxString HMSQuery::query_print_error_url_action(const std::string& dev_id, int print_error, std::vector<int>& button_action)
{
    char buf[32];
    ::sprintf(buf, "%08X", print_error);
    return _query_error_url_action(get_dev_id_type(dev_id), std::string(buf), button_action);
}

std::string HMSQuery::print_error_code(int print_error)
{
    char buf[32];
    ::sprintf(buf, "%08X", print_error);
    return std::string(buf);
}

std::string HMSQuery::pretty_code(const std::string& code)
{
    // Only the two shapes the printer actually reports are grouped. Anything else is passed
    // through untouched rather than chopped into fours on a guess.
    if (code.size() != 8 && code.size() != 16) return code;
    std::string out;
    out.reserve(code.size() + code.size() / 4);
    for (size_t i = 0; i < code.size(); ++i) {
        if (i && i % 4 == 0) out.push_back(' ');
        out.push_back(code[i]);
    }
    return out;
}

wxString HMSQuery::format_error(const wxString& text, const std::string& code)
{
    const std::string pretty = pretty_code(code);
    if (pretty.empty()) return text;
    if (text.IsEmpty()) {
        // Unknown: the code leads, because it is the only thing the owner can quote to support or
        // search the wiki for, and it points at the two places that do know. Bambu's own tables
        // ship empty `intro` strings for some codes (0C00010000020015 is one, in every shipped
        // language and in the live cloud table), so this is reached with every table present and
        // current - it is not only a "the table is missing" path.
        return wxString::Format(_L("Printer error %s - see the printer screen or Bambu's error-code page"),
                                wxString::FromUTF8(pretty));
    }
    return wxString::Format("%s (%s)", text, wxString::FromUTF8(pretty));
}

// ---------------------------------------------------------------- overrides ----

std::string HMSQuery::user_override_path()
{
    if (data_dir().empty()) return std::string();
    return (fs::path(data_dir()) / HMS_PATH / "overrides.json").make_preferred().string();
}

std::string HMSQuery::shipped_override_path()
{
    if (resources_dir().empty()) return std::string();
    return (fs::path(resources_dir()) / HMS_PATH / "edgeslicer_overrides.json").make_preferred().string();
}

bool HMSQuery::is_valid_code(const std::string& code, std::string& normalized)
{
    normalized.clear();
    for (char c : code) {
        if (c == ' ' || c == '-' || c == '_') continue;
        if (!std::isxdigit((unsigned char) c)) return false;
        normalized.push_back((char) std::toupper((unsigned char) c));
    }
    // The two shapes a printer reports: a print_error, or an HMS attr/code pair.
    return normalized.size() == 8 || normalized.size() == 16;
}

static std::time_t file_mtime_or_zero(const std::string& path)
{
    if (path.empty()) return 0;
    boost::system::error_code ec;
    const std::time_t         t = fs::last_write_time(fs::path(path), ec);
    return ec ? 0 : t;
}

static json read_override_file(const std::string& path)
{
    if (path.empty()) return json();
    std::ifstream f(encode_path(path.c_str()));
    if (!f.is_open()) return json();
    try {
        return json::parse(f);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "HMS: overrides: " << path << " is not valid json (" << e.what() << ")";
        return json();
    }
}

void HMSQuery::reload_overrides()
{
    std::unique_lock<std::mutex> lock(m_overrides_mutex);
    m_overrides_loaded = false;
}

// One overlay's answer for a code, or empty. `overrides` is the parsed file.
//
// Language: the exact one, then English, then whatever the entry was written in - a description
// somebody captured in German is still better than no description at all.
static wxString find_override(const json& doc, const std::string& code, const std::string& model, const std::string& lang)
{
    if (!doc.is_object() || !doc.contains("overrides") || !doc["overrides"].is_array()) return wxEmptyString;

    wxString exact, english, any;
    for (const auto& item : doc["overrides"]) {
        if (!item.is_object()) continue;
        std::string entry_code;
        if (!HMSQuery::is_valid_code(item.value("code", std::string()), entry_code)) continue;
        if (entry_code != code) continue;

        const std::string entry_model = item.value("model", std::string("*"));
        if (entry_model != "*" && !entry_model.empty() && boost::to_upper_copy(entry_model) != boost::to_upper_copy(model))
            continue;

        const std::string text = item.value("text", std::string());
        if (text.empty()) continue;

        const std::string entry_lang = item.value("lang", std::string("en"));
        if (entry_lang == lang && exact.IsEmpty())
            exact = wxString::FromUTF8(text);
        else if (entry_lang == "en" && english.IsEmpty())
            english = wxString::FromUTF8(text);
        else if (any.IsEmpty())
            any = wxString::FromUTF8(text);
    }
    if (!exact.IsEmpty()) return exact;
    if (!english.IsEmpty()) return english;
    return any;
}

wxString HMSQuery::query_override(const std::string& dev_id, const std::string& code, const std::string& lang_code)
{
    std::string normalized;
    if (!is_valid_code(code, normalized)) return wxEmptyString;
    const std::string model = get_dev_id_type(dev_id);

    std::unique_lock<std::mutex> lock(m_overrides_mutex);

    // Re-read when either file has been touched since it was last loaded, so an entry added by
    // --hms-add or by the hub route is live on the next lookup with no restart.
    const std::string user_path    = user_override_path();
    const std::string shipped_path = shipped_override_path();
    const std::time_t user_mtime   = file_mtime_or_zero(user_path);
    const std::time_t ship_mtime   = file_mtime_or_zero(shipped_path);
    if (!m_overrides_loaded || user_mtime != m_user_overrides_mtime || ship_mtime != m_shipped_overrides_mtime) {
        m_user_overrides          = read_override_file(user_path);
        m_shipped_overrides       = read_override_file(shipped_path);
        m_user_overrides_mtime    = user_mtime;
        m_shipped_overrides_mtime = ship_mtime;
        m_overrides_loaded        = true;
    }

    // This user's own capture wins over the one we ship.
    const wxString mine = find_override(m_user_overrides, normalized, model, lang_code);
    if (!mine.IsEmpty()) return mine;
    return find_override(m_shipped_overrides, normalized, model, lang_code);
}

bool HMSQuery::add_override(const std::string& code, const std::string& text, const std::string& lang,
                            const std::string& model, const std::string& source, const std::string& note,
                            bool force, std::string& error)
{
    error.clear();
    std::string normalized;
    if (!is_valid_code(code, normalized)) {
        error = "`" + code + "` is not an error code: expected 8 hex digits (a print error) or 16 (an HMS code)";
        return false;
    }
    if (text.empty()) {
        error = "the description is empty";
        return false;
    }
    const std::string path = user_override_path();
    if (path.empty()) {
        error = "no data directory to write to";
        return false;
    }

    const std::string use_lang  = lang.empty() ? std::string("en") : lang;
    const std::string use_model = model.empty() ? std::string("*") : model;

    std::unique_lock<std::mutex> lock(m_overrides_mutex);

    json doc = read_override_file(path);
    if (!doc.is_object()) doc = json::object();
    if (!doc.contains("version")) doc["version"] = 1;
    if (!doc.contains("overrides") || !doc["overrides"].is_array()) doc["overrides"] = json::array();

    // A plain date stamp; the file is read by people as much as by the app.
    std::string today;
    {
        char         buf[32] = {0};
        const time_t now     = time(nullptr);
        if (std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&now))) today = buf;
    }

    bool replaced = false;
    for (json& item : doc["overrides"]) {
        if (!item.is_object()) continue;
        std::string existing;
        if (!is_valid_code(item.value("code", std::string()), existing)) continue;
        if (existing != normalized) continue;
        if (item.value("lang", std::string("en")) != use_lang) continue;
        if (item.value("model", std::string("*")) != use_model) continue;
        if (!force) {
            error = "an override already exists for " + pretty_code(normalized) + " (" + use_model + "/" + use_lang +
                    "); pass force to replace it";
            return false;
        }
        item["text"] = text;
        if (!source.empty()) item["source"] = source;
        if (!note.empty()) item["note"] = note;
        if (!today.empty()) item["date"] = today;
        replaced = true;
        break;
    }

    if (!replaced) {
        json entry;
        entry["code"]  = normalized;
        entry["model"] = use_model;
        entry["lang"]  = use_lang;
        entry["text"]  = text;
        if (!source.empty()) entry["source"] = source;
        if (!note.empty()) entry["note"] = note;
        if (!today.empty()) entry["date"] = today;
        doc["overrides"].push_back(entry);
    }

    try {
        const fs::path dir = fs::path(path).parent_path();
        if (!dir.empty() && !fs::exists(dir)) fs::create_directories(dir);
        std::ofstream out(encode_path(path.c_str()));
        if (!out.is_open()) {
            error = "cannot write " + path;
            return false;
        }
        out << std::setw(2) << doc << std::endl;
    } catch (const std::exception& e) {
        error = std::string("cannot write ") + path + ": " + e.what();
        return false;
    }

    // Live on the next lookup without waiting for the mtime check.
    m_overrides_loaded = false;
    BOOST_LOG_TRIVIAL(info) << "HMS: override recorded for " << normalized << " (" << use_model << "/" << use_lang << ")";
    return true;
}

wxString HMSQuery::describe_error(const std::string& dev_id, const std::string& code, bool local_only)
{
    if (code.empty()) return wxEmptyString;

    std::string upper = boost::to_upper_copy(code);
    // A spelling that arrived already grouped ("0500 4046") still has to match a table key.
    upper.erase(std::remove(upper.begin(), upper.end(), ' '), upper.end());

    const std::string lang_code = HMSQuery::hms_language_code();

    // Our own descriptions first: this is the only way a code Bambu publishes with an empty
    // `intro` ever gets a sentence, and it lets the owner correct one that reads badly.
    const wxString overridden = query_override(dev_id, upper, lang_code);
    if (!overridden.IsEmpty()) return format_error(overridden, upper);

    wxString text;
    if (upper.size() > 8) {
        text = local_only ? query_hms_msg_local(dev_id, upper, lang_code) : _query_hms_msg(get_dev_id_type(dev_id), upper, lang_code);
    } else if (local_only) {
        // The int round-trip is exact: the code is eight hex digits or fewer by the branch above.
        try {
            query_print_error_msg_local(dev_id, (int) std::stoul(upper, nullptr, 16), lang_code, text);
        } catch (...) {
            text = wxEmptyString; // not hex at all; it is still named in the answer below
        }
    } else {
        text = _query_error_msg(get_dev_id_type(dev_id), upper, lang_code);
    }
    return format_error(text, upper);
}

wxString HMSQuery::describe_print_error(const std::string& dev_id, int print_error, bool local_only)
{
    return describe_error(dev_id, print_error_code(print_error), local_only);
}

void HMSQuery::clear_hms_info()
{
    std::unique_lock<std::mutex> lock(m_hms_mutex);
    m_hms_info_jsons.clear();
    m_hms_action_jsons.clear();
    m_cloud_hms_last_update_time.clear();
}

// m_hms_mutex must be held.
void HMSQuery::load_local_tables(const std::string& dev_id_type, const std::string& lang_code)
{
    if (dev_id_type.empty()) return;

    /*the local one only load once*/
    if (m_hms_info_jsons.count(dev_id_type) == 0) {
        std::string load_version;
        load_from_local(QUERY_HMS_INFO, dev_id_type, lang_code, &m_hms_info_jsons[dev_id_type], load_version);
    }

    if (m_hms_action_jsons.count(dev_id_type) == 0) {
        std::string load_version;
        load_from_local(QUERY_HMS_ACTION, dev_id_type, lang_code, &m_hms_action_jsons[dev_id_type], load_version);
    }
}

void HMSQuery::init_hms_info(const std::string& dev_id_type, const std::string& lang_code)
{
    if (dev_id_type.empty()) return;

    // Stealth mode stops the cloud fetch and nothing else: the shipped and cached tables below
    // are read exactly as they would be otherwise. This is the whole of "offline still answers" -
    // the object now always exists (GUI_App.cpp), and only the network half is withheld.
    AppConfig* config = wxApp::GetInstance() ? wxGetApp().app_config : nullptr;
    const bool offline_only = !config || config->get_stealth_mode();

    bool want_refresh = false;
    {
        std::unique_lock<std::mutex> lock(m_hms_mutex);
        load_local_tables(dev_id_type, lang_code);

        if (offline_only) return;

        /*download from cloud*/
        const time_t info_last_update_time = m_cloud_hms_last_update_time[dev_id_type];

        /* check hms is valid or not */
        bool retry = false;
        if (m_hms_info_jsons[dev_id_type].empty() || m_hms_action_jsons[dev_id_type].empty()) {
            retry = time(nullptr) - info_last_update_time > (60 * 1); // retry after 1 minute
        }

        if ((time(nullptr) - info_last_update_time > (60 * 60 * 24) || retry) /*do not update in one day to reduce waiting*/
            && !m_cloud_hms_refreshing[dev_id_type]) {
            m_cloud_hms_refreshing[dev_id_type] = true;
            m_cloud_hms_last_update_time[dev_id_type] = time(nullptr);
            want_refresh = true;
        }
    }

    if (want_refresh) refresh_from_cloud(dev_id_type);
}

// Upstream downloads inline, holding the lock (HMS.cpp init_hms_info). Here the query runs on the
// GUI thread - the event watcher asks for the text on its one-second heartbeat - and two
// perform_sync calls of up to twenty seconds each would freeze the window and trip the watchdog,
// so the refresh is detached and swapped in when it arrives. The tables the answer came from are
// already loaded, so the only cost is that a brand-new code is unknown until the next query.
void HMSQuery::refresh_from_cloud(const std::string& dev_id_type)
{
    boost::thread refresh = boost::thread([this, dev_id_type] {
        json info_json;
        json action_json;
        download_hms_related(QUERY_HMS_INFO, dev_id_type, &info_json);
        download_hms_related(QUERY_HMS_ACTION, dev_id_type, &action_json);

        std::unique_lock<std::mutex> lock(m_hms_mutex);
        if (!info_json.empty()) m_hms_info_jsons[dev_id_type] = info_json;
        if (!action_json.empty()) m_hms_action_jsons[dev_id_type] = action_json;
        m_cloud_hms_refreshing[dev_id_type] = false;
    });
    refresh.detach();
}

std::string get_hms_wiki_url(std::string error_code)
{
    AppConfig* config = wxGetApp().app_config;
    if (!config) return "";

    std::string hms_host = wxGetApp().app_config->get_hms_host();
    std::string lang_code = HMSQuery::hms_language_code();
    std::string url = (boost::format("https://%1%/index.php?e=%2%&s=device_hms&lang=%3%")
        % hms_host
        % error_code
        % lang_code).str();

    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return url;
    MachineObject* obj = dev->get_selected_machine();
    if (!obj) return url;

    if (!obj->dev_id.empty()) {
        url = (boost::format("https://%1%/index.php?e=%2%&d=%3%&s=device_hms&lang=%4%")
                       % hms_host
                       % error_code
                       % obj->dev_id
                       % lang_code).str();
    }
    return url;
}

std::string get_error_message(int error_code)
{
	char buf[64];
    std::string result_str = "";
    std::sprintf(buf,"%08X",error_code);
    std::string hms_host = wxGetApp().app_config->get_hms_host();
    std::string get_lang = wxGetApp().app_config->get_language_code();

    std::string url = (boost::format("https://%1%/query.php?lang=%2%&e=%3%")
                        %hms_host
                        %get_lang
                        %buf).str();

    Slic3r::Http http = Slic3r::Http::get(url);
    http.header("accept", "application/json")
        .timeout_max(10)
        .on_complete([get_lang, &result_str](std::string body, unsigned status) {
            try {
                json j = json::parse(body);
                if (j.contains("result")) {
                    if (j["result"].get<int>() == 0) {
                        if (j.contains("data")) {
                            json jj = j["data"];
                            if (jj.contains("device_error")) {
                                if (jj["device_error"].contains(get_lang)) {
                                    if (jj["device_error"][get_lang].size() > 0) {
                                        if (!jj["device_error"][get_lang][0]["intro"].empty() || !jj["device_error"][get_lang][0]["ecode"].empty()) {
                                            std::string error_info = jj["device_error"][get_lang][0]["intro"].get<std::string>();
                                            std::string error_code = jj["device_error"][get_lang][0]["ecode"].get<std::string>();
                                            error_code.insert(4, " ");
                                            result_str = from_u8(error_info).ToStdString() + "[" + error_code + "]";
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (...) {
             ;
        }
        })
        .on_error([](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(trace) << boost::format("[BBL ErrorMessage]: status=%1%, error=%2%, body=%3%") % status % error % body;
        }).perform_sync();

        return result_str;
}

}
}
