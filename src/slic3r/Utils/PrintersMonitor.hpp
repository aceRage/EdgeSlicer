#ifndef slic3r_PrintersMonitor_hpp_
#define slic3r_PrintersMonitor_hpp_

// Rules for the "Printers" tab (GUI/PrintersPanel.cpp, page resources/web/orca/monitor.html), kept
// free of wx so they can be tested on their own (tests/slic3rutils/printers_monitor_tests.cpp):
//   * which script-channel messages the page may send, and what each one names,
//   * how an answer is handed back to the page (a JSON argument to one of its window.__ hooks),
//   * what the page is given: the rows RemoteAccess::api_printers builds - the very rows the hub
//     serves as /summary and the app's Devices page renders - plus when they were read,
//   * how "Open Device page" reaches a printer, per vendor,
//   * the printer groups (phase 1: stored in AppConfig behind GroupStore; the target is the hub).
//
// The page never talks to the instance API (loopback, no auth) or the hub: everything goes through
// the "wx" script channel of its own web view, and the panel checks the page is ours.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace PrintersMonitor {

// ---- messages from the page --------------------------------------------------------------------
//
//   printers_get                              the printer rows, answered with __printers(payload)
//   printers_groups_get                       the groups, answered with __printerGroups(groups)
//   printers_control:<action>:<0|1>:<id>      pause | resume | stop (stop needs the 1: the page
//                                             asked the person), answered with __printerControl
//   printers_job:<n>                          how control job n is doing, answered with __printerJob
//   printers_open:<id>                        open that printer's Device page (or its web UI)
//   printers_tasks                            the old multi-device task list (cloud send queue)
//   printers_thumb:<archive id>               a G-code archive record's plate picture, answered
//                                             with __printerThumb({ id, data: "data:image/png;..." })
//   printers_forget:<id>                      forget a leftover row the hub remembers (the page
//                                             asked the person), answered with
//                                             __printerForget({ id, ok, error })
//   printers_remove:<id>                      "Remove printer" on any card (the page asked the
//                                             person): removes the printer at its source (see
//                                             removal_plan), answered with
//                                             __printerRemove({ id, ok, error, done[] })
//   printers_rename:{"id":..,"name":..}       a name of the person's own for that card ("" = the
//                                             printer's own again), answered with
//                                             __printerRename({ id, ok, name, error })
//
// The id goes last because printer ids carry colons ("sm:<serial>", "ph:<id>").
struct Message
{
    enum class Kind { Invalid, Get, Groups, Control, Job, Open, Tasks, Thumb, Forget, Remove, Rename };
    Kind        kind { Kind::Invalid };
    std::string id;              // Control, Open, Forget, Remove, Rename; Thumb: the archive record id
    std::string name;            // Rename: the cleaned new name, "" = clear
    std::string action;          // Control: pause | resume | stop
    bool        confirm { false };
    int         job { 0 };       // Job
};
Message parse_message(const std::string& msg);

// A printer id as the rows spell them: 1..200 printable ASCII characters, none of them space,
// quote, backslash or angle bracket. Anything else is refused before it reaches RemoteControl.
bool valid_printer_id(const std::string& id);

// The only actions the tab offers (owner decision 2026-10-06: Pause / Resume / Stop only).
bool valid_action(const std::string& action);

// "if (window.<fn>) window.<fn>(<arg>);" - fn must be a plain identifier (else ""), and the JSON
// is written with every non-ASCII character escaped, so nothing in a printer's name can end the
// script early.
std::string script_call(const std::string& fn, const nlohmann::json& arg);

// What the page is given for one poll: { ok, printers: [...rows...], at: <ms since epoch> } when
// api_printers answered 200 with a parsable body, otherwise { ok: false, error, printers: [] }.
nlohmann::json page_payload(int status, const std::string& body, long long now_ms);

// The leftovers: rows the hub still remembers (GET /hub/printers, `hub_status` / `hub_body`) for
// printers this slicer no longer has (`configured: false`), appended to an ok payload so the tab
// can offer Forget. Each is marked leftover: true, online: false, with its control flags removed
// and `seen_s` = the row's age_s. A row the payload already has, an id valid_printer_id refuses,
// or a row with configured true or absent is never added. Nothing happens unless both are ok.
void add_leftovers(nlohmann::json& payload, int hub_status, const std::string& hub_body);

// ---- "Remove printer" ------------------------------------------------------------------------
//
// Where each kind of row comes from, and so what removing it means. A printer is removed at its
// source so it never comes back:
//
//   ph:<id>      a saved print-host device (Elegoo, Qidi, Creality, FlashForge, Klipper, PrusaLink:
//                <datadir>/hub/print_host_devices.json, under the row's model_key / device_id).
//                The preset migration remembers what it imported, so it is not re-created and
//                needs no tombstone; only the hand-held Devices dialog adds one again.
//   sm:<id>      a LAN Snapmaker (<datadir>/hub/snapmaker_lan.json). Four sources re-add one -
//                mDNS discovery, the Device tab's saved devices (AppConfig DeviceInfo), the Stream
//                tab's cameras and a hand-typed address - so the matching AppConfig devices go too,
//                and a tombstone (removed_printers.json) keeps the first three from re-adding it.
//                Adding it again by hand clears the tombstone.
//   <dev_id>     a Bambu printer. In LAN mode: Log out (the saved LAN record and its access codes,
//                as the Device tab's Log out does). Bound to the cloud account: not removable here
//                - it is the account's, and the dialog says where to unbind it.
//   host         the selected printer preset's own print_host: the preset's, explained.
//   connect      the Device tab's live connection: Disconnect there, explained.
//   leftover     a row only the hub remembers: just the hub row (Forget).
// Every removal also drops the hub's remembered row (printers.json) at once.
struct RemovalPlan
{
    enum class Mode { Remove, Logout, Explain };
    Mode        mode { Mode::Explain };
    std::string reason;               // Explain: why, and where to do it instead
    // What kind of printer this is, for a client's wording: bambu_cloud | preset_host |
    // device_connection (Explain); leftover | print_host_device | lan_snapmaker | bambu_lan | other.
    std::string reason_kind;
    std::string ph_model_key;         // print_host_devices.json entry
    std::string ph_device_id;
    std::string sm_id;                // snapmaker_lan.json entry (by id or ip)
    std::string sm_ip;
    bool        app_config_devices { false }; // AppConfig DeviceInfo entries with this sn / ip
    std::string bambu_dev_id;         // Logout
    bool        tombstone { false };  // keep discovery from re-adding it (key: row id, and ip)
    bool        hub_row { true };
    // The stores touched, by name: "print_host_devices", "snapmaker_lan", "app_config_devices",
    // "bambu_lan", "tombstone", "hub_printers" - what the tests and the answer's done[] list.
    std::vector<std::string> stores() const;
};
RemovalPlan removal_plan(const nlohmann::json& row);
// What the card shows: each row gets "remove": { mode: remove|logout|explain, kind, reason? }.
void annotate_removal(nlohmann::json& printers);
// The same for one row (the hub puts it on its /summary rows, so the app can word its button).
nlohmann::json removal_json(const nlohmann::json& row);

// The tombstones: { "removed": [ { "id": "sm:U1-0042", "ip": "192.168.1.40", "at": <unix s> } ] }.
// A discovered or merged printer whose row id or address is listed is not re-added.
bool tombstoned(const nlohmann::json& store, const std::string& row_id, const std::string& ip);
void add_tombstone(nlohmann::json& store, const std::string& row_id, const std::string& ip, long long now_s);
// Drops every tombstone naming this id or this ip (the person added the printer again). True when
// one went.
bool clear_tombstone(nlohmann::json& store, const std::string& row_id, const std::string& ip);
// The file: read tolerantly (missing or broken = no tombstones), written through a temporary.
nlohmann::json load_tombstones(const std::string& path);
bool           save_tombstones(const std::string& path, const nlohmann::json& store);
// The same reader / writer for any of these small JSON stores (the aliases below too).
nlohmann::json load_store_file(const std::string& path);
bool           save_store_file(const std::string& path, const nlohmann::json& store);

// ---- "Rename" --------------------------------------------------------------------------------
//
// A name of the person's own for any printer row, kept by the slicer (<datadir>/hub/
// printer_aliases.json: { "aliases": { "<row id>": "Left U1" } }) and put on the rows
// /api/printers reports - so the Printers tab, the hub's /summary and the phone app all show it.
// The printer's own reported name is kept as `reported_name`; `custom_name` is the alias.
//
// A name as stored: trimmed, at most 64 characters (UTF-8 bytes up to 160), no control characters.
// Empty after that = no alias.
std::string clean_alias(const std::string& name);
// Sets (or, for an empty name, clears) the alias of `row_id`. False for an invalid id.
bool set_alias(nlohmann::json& store, const std::string& row_id, const std::string& name);
std::string alias_of(const nlohmann::json& store, const std::string& row_id);
// Puts the aliases on the rows: name = alias, reported_name = what the row said, custom_name = alias.
void apply_aliases(nlohmann::json& printers, const nlohmann::json& store);
// printers_rename:{"id":"<row id>","name":"<new name>"} - the id and the cleaned name, or false.
bool parse_rename(const std::string& payload, std::string& id, std::string& name);

// ---- Open Device page --------------------------------------------------------------------------
struct Target
{
    std::string kind; // the row's kind: bambu | snapmaker | connect | printhost
    std::string url;  // its web address, when the row has one
    // The row is drawn by the shared device page (resources/web/orca/device.html) rather than by a
    // vendor page: a printer with no device page of its own (the Elegoo Centauri Carbon 2, owner D6).
    bool        device_page { false };
};
// The ids of the last rows the page was given, with what "open" needs. The page names an id only;
// where it goes is taken from here, never from the page.
std::map<std::string, Target> targets_of(const nlohmann::json& printers);

// http:// or https:// with a host, and nothing that could break out of an address.
bool is_web_url(const std::string& url);

enum class OpenWay {
    BambuMonitor,   // MainFrame::jump_to_monitor(id): the Device tab is the Bambu one
    PrinterWebView, // the printer's web UI in the Device tab's web view (a non-Bambu preset)
    Browser,        // the printer's web UI in the system browser (the Device tab is something else)
    NotHere,        // nothing to open from this window (a Bambu printer while the preset is not
                    // a Bambu one, a row without a web address); the page says why
    DevicePage      // the shared device page for that row id, in the Device tab's web view
};
// bambu_monitor_shown: the Device tab currently holds the Bambu MonitorPanel.
// printer_view_shown:  the Device tab currently holds the PrinterWebView.
OpenWay open_way(const Target& t, bool bambu_monitor_shown, bool printer_view_shown);
// What the page shows when open_way is NotHere.
std::string not_here_reason(const Target& t);

// ---- the shared device page ---------------------------------------------------------------------
//
// One vendor-neutral device page (resources/web/orca/device.html, drawn by printer_cards.js's
// PrinterCards.device) for any printer whose /api/printers row it can draw: status, job, progress,
// layers, temperatures, fans and light, filament units, Pause / Resume / Stop. It is fed over the
// same "wx" channel and messages as the Printers tab, filtered to one row id. Today the Elegoo
// Centauri Carbon 2 uses it (it has no LAN web page of its own); the other vendors converge on it
// later (tests/design_shared_device_page.md).
//
// Whether a row is drawn by it: a row that says so ("device_page": true) or carries the CC2's
// "cc2" details.
bool uses_device_page(const nlohmann::json& row);
// "/web/orca/device.html?embed=1&theme=<dark|light>&id=<the row id, percent-encoded>". Empty for
// an id valid_printer_id refuses.
std::string device_page_path(const std::string& id, bool dark);
// The row id a device page URL names (the inverse; "" when it names none or a bad one).
std::string device_page_id(const std::string& url);

// A G-code archive record id (the sidecar's stem): 1..128 of [A-Za-z0-9._-], not starting with a dot.
bool valid_archive_id(const std::string& id);

// ---- what the card shows besides the row ---------------------------------------------------------
//
// Printer pictures: the vendor profiles' "<model>_cover.png" (resources/profiles/<Vendor>/), which
// the page server serves from the installed resources as /profiles/<Vendor>/<file>. The index maps
// the lower-cased model name to that URL path, each segment percent-encoded.
using CoverIndex = std::map<std::string, std::string>;
// vendor_files: (vendor folder, file name) pairs; only "*_cover.png" names are taken.
CoverIndex index_covers(const std::vector<std::pair<std::string, std::string>>& vendor_files);
// The picture for a model name ("Bambu Lab H2C", "Snapmaker U1", a preset's printer_model), or "".
std::string cover_for(const CoverIndex& index, const std::string& model_name);
// RFC 3986 unreserved characters stay, everything else becomes %XX.
std::string url_encode_segment(const std::string& s);
// What the model line says: a Bambu display name without its "Bambu Lab " prefix ("H2C"); any
// other name as it is; the raw model code when no name is known.
std::string short_model_name(const std::string& kind, const std::string& model_name, const std::string& model_code);

// The current job's plate picture from the G-code archive: per printer, the newest record whose
// sent name or file name is the job the printer reports (the hub's own rule, RemoteHub
// printer_thumbnail_paths, minus its "newest record of any job" fallback: a card shows the
// picture of the job it is running, or none).
struct ArchiveEntry
{
    std::string printer_id, record_id, sent_name, file;
    long long   time { 0 };
};
std::map<std::string, std::string> pick_thumbnails(const std::vector<ArchiveEntry>& entries,
                                                   const std::map<std::string, std::string>& jobs);
// The job name a row reports (task, else subtask_name), the field the hub matches on too.
std::string row_job(const nlohmann::json& row);

// Adds what the card needs to each row of a page payload: "model_short", "picture" (a cover URL
// path) when there is one, and "thumb_id" (an archive record id) when the archive has the running
// job's picture.
void enrich_rows(nlohmann::json& printers, const CoverIndex& covers, const std::map<std::string, std::string>& thumbs);

// ---- groups ------------------------------------------------------------------------------------
struct Group
{
    std::string              id;
    std::string              name;
    std::vector<std::string> printers; // row ids
};

// Where groups live. Phase 1: AppConfig (GUI/PrintersPanel.cpp). The target is a hub-side store
// shared with the app and the phone page (research_multi_device_monitor.md section 3.6); it
// implements this same interface.
class GroupStore
{
public:
    virtual ~GroupStore() = default;
    virtual std::vector<Group> load()                          = 0;
    virtual bool               save(const std::vector<Group>&) = 0;
};

// Parses what save_groups wrote. Bad entries are skipped; duplicate ids keep the first; ids and
// printer ids are checked like valid_printer_id; names are trimmed to 64 characters.
std::vector<Group> parse_groups(const std::string& stored);
std::string        save_groups(const std::vector<Group>& groups);
// The old Multi-device tab's pick list (AppConfig section multi_devices, keys 0..5) as the first
// group, when nothing is stored yet. Empty ids are dropped; no ids, no group.
std::vector<Group> seed_from_multi_devices(const std::vector<std::string>& dev_ids);
// What the page is given: "All printers" first (id "all", every row), then the stored groups.
nlohmann::json groups_json(const std::vector<Group>& groups);

} // namespace PrintersMonitor
} // namespace Slic3r

#endif // slic3r_PrintersMonitor_hpp_
