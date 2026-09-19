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

    // The two override overlays, and the mtime each was read at so an edit is noticed.
    json               m_user_overrides;
    json               m_shipped_overrides;
    std::time_t        m_user_overrides_mtime    = 0;
    std::time_t        m_shipped_overrides_mtime = 0;
    bool               m_overrides_loaded        = false;
    mutable std::mutex m_overrides_mutex;

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

    // The one sentence every user-facing surface shows for a printer error. The device tab, the
    // hub's events and web page, the app push payload and --hms-lookup all go through here, so
    // they cannot disagree about what a code means, and none of them can leak a bare code again.
    //
    // `code` is spelled the way the printer reported it: eight hex digits for a print_error,
    // sixteen for an HMS attr/code pair. Both tables are tried - device_error for the short form,
    // device_hms for the long one - because a caller usually knows only that it holds "a code".
    // `local_only` keeps the lookup off the network for the callers that must not block.
    wxString  describe_error(const std::string& dev_id, const std::string& code, bool local_only = false);
    wxString  describe_print_error(const std::string& dev_id, int print_error, bool local_only = false);

    // Formatting, split from the lookup so it is testable with no tables, no window, no AppConfig.
    //   known:   "<text> (<code>)"
    //   unknown: "<code> - no description available; look the code up in Bambu's error list"
    // The code stays in both, because it is the only thing the owner can search for or quote to
    // support, and it is what the wiki URL is keyed by.
    static wxString format_error(const wxString& text, const std::string& code);

    // "0C00010000020015" -> "0C00 0100 0002 0015"; "05004046" -> "0500 4046". Bambu groups a code
    // in fours wherever it shows one, and an ungrouped 16-digit run is unreadable.
    static std::string pretty_code(const std::string& code);

    // The eight-hex spelling of a print_error, the form the tables are keyed by.
    static std::string print_error_code(int print_error);

    // ---- overrides: EdgeSlicer's own descriptions, ahead of Bambu's tables ----
    //
    // Bambu publishes an empty `intro` for some codes that their own slicer still shows a
    // sentence for (0C00010000020015 is the one that started this), so refreshing the snapshot
    // cannot supply them. These two overlays can:
    //
    //   <datadir>/hms/overrides.json                this user's own captures  (highest priority)
    //   <resources>/hms/edgeslicer_overrides.json   shipped with the app
    //
    // and only then Bambu's tables, and only then the generic fallback. Both files are re-read
    // when their mtime changes, so a capture goes live without restarting the app.
    // Schema and workflow: docs/hms-overrides.md.
    wxString    query_override(const std::string& dev_id, const std::string& code, const std::string& lang_code);

    // Record one description in the user overlay. Returns false with `error` set when the code is
    // malformed, or when an entry already exists for this (code, model, lang) and force is false.
    bool        add_override(const std::string& code, const std::string& text, const std::string& lang,
                             const std::string& model, const std::string& source, const std::string& note,
                             bool force, std::string& error);

    // Drop what is cached so the next lookup re-reads both overlay files. The mtime check does
    // this by itself; this is for a caller that has just written one and wants it live at once.
    void        reload_overrides();

    // A code we can look up at all: 8 or 16 hex digits, spaces ignored, upper-cased on the way out.
    static bool is_valid_code(const std::string& code, std::string& normalized);

    static std::string user_override_path();
    static std::string shipped_override_path();

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
