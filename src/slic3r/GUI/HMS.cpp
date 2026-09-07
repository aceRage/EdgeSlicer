#include "HMS.hpp"

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

    const json& device_hms_msg_json = device_hms_json.value(lang_code, json());
    if (device_hms_msg_json.is_null())
    {
        BOOST_LOG_TRIVIAL(error) << "hms: query_hms_msg, do not contains lang_code = " << lang_code;
        // whatever language the table does carry is better than nothing
        for (const auto& lang_item : device_hms_json)
        {
            if (!lang_item.is_array()) continue;
            for (const auto& msg_item : lang_item)
            {
                if (msg_item.is_object())
                {
                    const std::string& error_code = msg_item.value("ecode", std::string());
                    if (boost::to_upper_copy(error_code) == long_error_code && msg_item.contains("intro"))
                    {
                        BOOST_LOG_TRIVIAL(info) << "retry without lang_code successed.";
                        return wxString::FromUTF8(msg_item["intro"].get<std::string>());
                    }
                }
            }
        }

        return wxEmptyString;
    }

    for (const auto& item : device_hms_msg_json)
    {
        if (item.is_object())
        {
            const std::string& error_code = item.value("ecode", std::string());
            if (boost::to_upper_copy(error_code) == long_error_code && item.contains("intro"))
            {
                return wxString::FromUTF8(item["intro"].get<std::string>());
            }
        }
    }

    BOOST_LOG_TRIVIAL(error) << "hms: query_hms_msg, do not contains valid message, lang_code = " << lang_code << " long_error_code = " << long_error_code;
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
    if (m_hms_info_json.contains("device_error")) {
        if (m_hms_info_json["device_error"].contains(lang_code)) {
            for (auto item = m_hms_info_json["device_error"][lang_code].begin(); item != m_hms_info_json["device_error"][lang_code].end(); item++) {
                if (item->contains("ecode") && boost::to_upper_copy((*item)["ecode"].get<std::string>()) == error_code) {
                    if (item->contains("intro")) {
                        return wxString::FromUTF8((*item)["intro"].get<std::string>());
                    }
                }
            }
            BOOST_LOG_TRIVIAL(info) << "hms: query_error_msg, not found error_code = " << error_code;
        } else {
            BOOST_LOG_TRIVIAL(error) << "hms: query_error_msg, do not contains lang_code = " << lang_code;
            // return first language
            if (!m_hms_info_json["device_error"].empty()) {
                for (auto lang : m_hms_info_json["device_error"]) {
                    if (!lang.is_array()) continue;
                    for (auto item = lang.begin(); item != lang.end(); item++) {
                        if (item->contains("ecode") && boost::to_upper_copy((*item)["ecode"].get<std::string>()) == error_code) {
                            if (item->contains("intro")) {
                                return wxString::FromUTF8((*item)["intro"].get<std::string>());
                            }
                        }
                    }
                }
            }
        }
    }
    else {
        BOOST_LOG_TRIVIAL(info) << "device_error is not exists";
        return wxEmptyString;
    }

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

    bool want_refresh = false;
    {
        std::unique_lock<std::mutex> lock(m_hms_mutex);
        load_local_tables(dev_id_type, lang_code);

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
