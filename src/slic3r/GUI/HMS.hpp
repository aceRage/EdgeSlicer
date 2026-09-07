#ifndef slic3r_HMS_hpp_
#define slic3r_HMS_hpp_

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StepCtrl.hpp"
#include "BitmapCache.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/Thread.hpp"
#include "nlohmann/json.hpp"

#include <mutex>
#include <unordered_map>

namespace Slic3r {

class MachineObject;

namespace GUI {

#define HMS_INFO_FILE	"hms.json"
#define QUERY_HMS_INFO	"query_hms_info"
#define QUERY_HMS_ACTION	"query_hms_action"

// The printer's own words for an error code.
//
// One table per device series, not one table for all of them. The series is the first three
// characters of the serial number - 31B is the H2C, 094 the H2D, 20P the P1 line - and the same
// code means different things on different machines, which is why the newer printers reported a
// bare "05004046" and nothing else for as long as the lookup ignored the series.
//
// Three places a table can come from, in this order:
//   1. <datadir>/hms/hms_<lang>_<series>.json, the working copy;
//   2. <resources>/hms/, shipped with the app and copied into the data dir the first time a
//      series is asked for and the data dir has nothing for it (upstream's STUDIO-9512 fix);
//   3. the cloud, refreshed at most once a day per series.
class HMSQuery {

protected:
    std::unordered_map<std::string, json> m_hms_info_jsons;   // key -> device id type, the first three digits of the SN
    std::unordered_map<std::string, json> m_hms_action_jsons; // key -> device id type
    std::unordered_map<std::string, time_t> m_cloud_hms_last_update_time;
    std::unordered_map<std::string, bool>   m_cloud_hms_refreshing; // a refresh thread is already out for this series
    mutable std::mutex m_hms_mutex;

public:
    HMSQuery() { }
    ~HMSQuery() { clear_hms_info(); };

public:
    // clear hms, so that a language change is picked up by the next query
    void      clear_hms_info();

    // query
    wxString  query_hms_msg(const std::string& dev_id, const std::string& long_error_code);
    bool      query_print_error_msg(const std::string& dev_id, int print_error, wxString& error_msg);
    wxString  query_print_error_url_action(const std::string& dev_id, int print_error, std::vector<int>& button_action);

    // The same two queries against the local tables only: no AppConfig, no clock and no cloud.
    // This is the seam the unit test and the hub's debug hook use; nothing on the normal path
    // calls them.
    wxString  query_hms_msg_local(const std::string& dev_id, const std::string& long_error_code, const std::string& lang_code);
    bool      query_print_error_msg_local(const std::string& dev_id, int print_error, const std::string& lang_code, wxString& error_msg);

public:
    static std::string hms_language_code();
    static std::string build_query_params(std::string& lang);
    // The first three characters of the serial number, which is what a table is keyed by.
    static std::string get_dev_id_type(const std::string& dev_id);
    static std::string get_hms_file(const std::string& hms_type, const std::string& lang = std::string("en"), const std::string& dev_id_type = std::string());

private:
    // load hms
    void init_hms_info(const std::string& dev_id_type, const std::string& lang_code);
    void load_local_tables(const std::string& dev_id_type, const std::string& lang_code);
    void refresh_from_cloud(const std::string& dev_id_type);
    int  download_hms_related(const std::string& hms_type, const std::string& dev_id_type, json* receive_json);
    int  load_from_local(const std::string& hms_type, const std::string& dev_id_type, const std::string& lang, json* load_json, std::string& load_version);
    int  save_to_local(const std::string& lang, const std::string& hms_type, const std::string& dev_id_type, const json& save_json);

    // internal query
    wxString _query_hms_msg(const std::string& dev_id_type, const std::string& long_error_code, const std::string& lang_code = std::string("en"));
    wxString _query_error_msg(const std::string& dev_id_type, const std::string& long_error_code, const std::string& lang_code = std::string("en"));
    wxString _query_error_url_action(const std::string& dev_id_type, const std::string& long_error_code, std::vector<int>& button_action);

    // the lookup on a table already in hand; m_hms_mutex is held by the caller
    static wxString _find_hms_msg(const json& hms_info, const std::string& long_error_code, const std::string& lang_code);
    static wxString _find_error_msg(const json& hms_info, const std::string& long_error_code, const std::string& lang_code);
};

int get_hms_info_version(std::string &version);

std::string get_hms_wiki_url(std::string code);

std::string get_error_message(int error_code);

}
}


#endif
