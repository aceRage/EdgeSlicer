#ifndef slic3r_Utils_PrinterStorage_hpp_
#define slic3r_Utils_PrinterStorage_hpp_

// The G-code files a printer keeps, read and managed over the network: what the Device page's
// Storage tab shows (PrinterStoragePanel) and what its file-grid model (GUI/Printer/
// RemoteFileGridModel) is built on.
//
// Adapted from OrcaSlicer PR #16156 ("AMS Sync, Filament Mapping, Storage Tab", merge 029bc7928f),
// whose OrcaPrinterAgent implements list_printer_files / get_printer_file_metadata /
// get_printer_file_thumbnail / delete_printer_file against Moonraker's /server/files API. This
// fork has no printer-agent layer (IPrinterAgent / OrcaPrinterAgent), so the same calls live here,
// against the addresses the fork already holds: a print host preset or its device list
// (PrintHostDevices) and the Snapmaker LAN list (SnapmakerLan).
//
// No wx and no GUI here, so the whole of it is unit tested against a mock Moonraker
// (tests/slic3rutils/printer_storage_tests.cpp): the parsers, the target rules, the Moonraker
// backend through an injected Transport, and the formatting.
//
// Moonraker's file API, as used here (https://moonraker.readthedocs.io/en/latest/web_api/):
//   GET    /server/files/list?root=gcodes          [{path, modified, size, permissions}]
//   GET    /server/files/metadata?filename=<path>  estimated_time, filament_total, thumbnails, ...
//   GET    /server/files/gcodes/<path>             a file (thumbnails are files under .thumbs/)
//   DELETE /server/files/gcodes/<path>             delete one file
//   GET    /printer/objects/query?print_stats      print_stats.state / filename
//   POST   /printer/print/start?filename=<path>    start a file the printer already has
// The print start is the one SnapmakerLan::start_print and the hub's reprint use.

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
namespace PrinterStorage {

// ---- what is on the printer -------------------------------------------------------------------

// One file of the printer's gcodes root.
struct FileEntry
{
    std::string   path;         // relative to the gcodes root, "/"-separated: "sub/cube.gcode"
    std::string   name;         // the last segment, for display
    std::uint64_t size { 0 };   // bytes
    std::int64_t  modified { 0 }; // unix seconds, 0 = unknown
};

// One embedded preview picture of a file.
struct Thumbnail
{
    int           width { 0 };
    int           height { 0 };
    std::uint64_t size { 0 };
    std::string   path; // relative to the gcodes root, like FileEntry::path
};

// What the slicer wrote into a file, as Moonraker's metadata reports it. Missing numbers are 0,
// missing strings empty.
struct FileMetadata
{
    bool        valid { false };       // the printer answered with metadata for this file
    int         estimated_time_s { 0 };
    double      filament_total_mm { 0 };
    double      filament_weight_g { 0 };
    std::string filament_type;         // "PLA" or "PLA;PETG"
    std::string filament_name;
    double      layer_height { 0 };
    double      first_layer_height { 0 };
    double      object_height { 0 };
    double      nozzle_diameter { 0 };
    int         layer_count { 0 };
    std::string slicer;
    std::string slicer_version;
    std::vector<Thumbnail> thumbnails;
};

// Moonraker's /server/files/list answer ({"result": [...]}, or the bare array). Takes "path"
// (current Moonraker) or "filename" (old releases). Skips entries without a usable path, anything
// under a hidden directory (".thumbs/...") and names this tab does not list (is_listed_name).
std::vector<FileEntry> parse_file_list(const nlohmann::json& answer);
// /server/files/metadata's answer ({"result": {...}}, or the bare object) for the file at
// `file_path`. Each thumbnail's relative_path is relative to the file's own directory; it comes
// back relative to the gcodes root.
FileMetadata parse_metadata(const nlohmann::json& answer, const std::string& file_path);
// The thumbnail worth showing: the smallest one at least `want_width` wide, else the widest.
// nullptr when there is none.
const Thumbnail* pick_thumbnail(const std::vector<Thumbnail>& thumbnails, int want_width = 300);
// print_stats.state out of /printer/objects/query?print_stats ("" when the answer has none), and
// the file the printer is on.
std::string print_state_of(const nlohmann::json& answer, std::string* filename = nullptr);
// A printer that is in the middle of a job: printing or paused.
bool is_busy_state(const std::string& state);

// G-code the printer can run (.gcode, .g, .gco), case-insensitive.
bool is_printable_name(const std::string& name);
// What the tab lists: the printable names plus .bgcode and .3mf, which are shown but not printed.
bool is_listed_name(const std::string& name);
// A path the tab may hand back to the printer: relative, "/"-separated, 1..1024 bytes, no "."
// or ".." segment, no empty segment, no backslash or control character.
bool valid_path(const std::string& path);
// Each "/"-separated segment percent-encoded, the separators kept.
std::string encode_path(const std::string& path);

// ---- which printer ----------------------------------------------------------------------------

struct Target
{
    enum class Kind {
        None,        // no printer: the Device page shows no Storage tab
        Moonraker,   // Moonraker's file API at base_url
        Unsupported, // a printer whose connection has no file API this tab speaks; `reason` says why
    };
    Kind        kind { Kind::None };
    std::string key;      // stable id: "host", "ph:<device id>", "sm:<serial or address>"
    std::string name;     // what the tab calls it
    std::string base_url; // http(s)://host[:port], no trailing slash
    std::string apikey;   // Moonraker's X-Api-Key, when the printer has one
    std::string reason;   // Unsupported: one sentence for the person

    bool supported() const { return kind == Kind::Moonraker && !base_url.empty(); }
    bool operator==(const Target& o) const
    {
        return kind == o.kind && key == o.key && name == o.name && base_url == o.base_url && apikey == o.apikey &&
               reason == o.reason;
    }
    bool operator!=(const Target& o) const { return !(*this == o); }
};

// "http(s)://host[:port]" out of what a preset or a device list holds: a bare host, host:port,
// a full URL with a path (a Fluidd/Mainsail address such as http://pi/fluidd/ keeps only its
// origin, since Moonraker answers at the root), or the U1's MQTT address (":1884" / ":8883" are
// dropped, its HTTP API is on port 80). "" when there is no host.
std::string base_url_of(const std::string& address);

// A print host printer, by its preset's host_type key (PrintHostDevices::host_type_key):
// "octoprint" (the fork's "Octo/Klipper"), "moonraker" and "moonraker_mqtt" are asked as Moonraker
// printers; every other host type is Unsupported with a reason naming it. Kind::None without an
// address.
Target target_for_host(const std::string& host_type, const std::string& address, const std::string& apikey,
                       const std::string& name, const std::string& key);
// A Snapmaker printer on the LAN (SnapmakerLan::Device): Moonraker on its own port.
Target target_for_snapmaker(const std::string& ip, int port, const std::string& name, const std::string& key);
// Unsupported, with that reason.
Target unsupported_target(const std::string& name, const std::string& reason);

// ---- results ----------------------------------------------------------------------------------

enum class Status {
    Ok,
    NotSupported, // the printer has no such API (Moonraker answered 404 to the list, or no backend)
    Offline,      // nothing answered
    Unauthorized, // 401 / 403: the API key is missing or wrong
    NotFound,     // the file is not there (any more)
    Busy,         // the printer is printing or paused: no new print
    Failed,       // anything else; `message` has the printer's own words
};

struct Result
{
    Status      status { Status::Ok };
    std::string message; // empty for Ok; otherwise one sentence for the person (English, the GUI translates its own)
    bool        ok() const { return status == Status::Ok; }
    static Result success() { return Result(); }
    static Result failure(Status s, std::string m)
    {
        Result r;
        r.status  = s;
        r.message = std::move(m);
        return r;
    }
};

// ---- transport --------------------------------------------------------------------------------

struct HttpReply
{
    unsigned    status { 0 }; // 0 = no answer at all
    std::string body;
    std::string error;        // the transport's own words when status is 0 (or not 2xx)
};

// How the backend reaches the printer. The real one is Http (make_http_transport); the tests hand
// in a mock Moonraker. `path_and_query` starts with "/". Blocking; any thread but the GUI one.
class Transport
{
public:
    virtual ~Transport() = default;
    virtual HttpReply request(const std::string& method, const std::string& path_and_query, std::size_t size_limit,
                              int timeout_s) = 0;
};

// Http against target.base_url, with its X-Api-Key, accepting a printer's self-signed certificate
// (Http::TlsPolicy::PrintHost, as every other print host call does).
std::shared_ptr<Transport> make_http_transport(const Target& target);

// ---- the backend ------------------------------------------------------------------------------

// What a printer's storage offers, whatever it speaks. Today Moonraker (MoonrakerFiles); Part B
// (filament mapping on the start of a multi-material print) and Part C build on this, and another
// protocol (Elegoo's SDCP, a Bambu adapter over PrinterFileSystem) would be another implementation.
// Every call blocks: call it from a worker thread, never the GUI one.
class IPrinterFiles
{
public:
    virtual ~IPrinterFiles() = default;

    virtual Result list(std::vector<FileEntry>& out)                                  = 0;
    virtual Result metadata(const std::string& path, FileMetadata& out)               = 0;
    // The bytes of one picture (PNG or JPEG) at `thumbnail_path` (Thumbnail::path).
    virtual Result thumbnail(const std::string& thumbnail_path, std::string& bytes)   = 0;
    virtual Result remove(const std::string& path)                                    = 0;
    // print_stats.state ("standby", "printing", "paused", "complete", "cancelled", "error") and the
    // file the printer is on.
    virtual Result printer_state(std::string& state, std::string& current_file)       = 0;
    // Start a file the printer already has, as it was sliced. Refuses (Busy) while the printer is
    // printing or paused, asking it first, so a second start can never replace a running job.
    virtual Result start_print(const std::string& path)                               = 0;
};

class MoonrakerFiles : public IPrinterFiles
{
public:
    explicit MoonrakerFiles(std::shared_ptr<Transport> transport) : m_transport(std::move(transport)) {}

    Result list(std::vector<FileEntry>& out) override;
    Result metadata(const std::string& path, FileMetadata& out) override;
    Result thumbnail(const std::string& thumbnail_path, std::string& bytes) override;
    Result remove(const std::string& path) override;
    Result printer_state(std::string& state, std::string& current_file) override;
    Result start_print(const std::string& path) override;

private:
    std::shared_ptr<Transport> m_transport;
};

// The backend for a target: MoonrakerFiles over Http, or nullptr when the target is not supported.
std::shared_ptr<IPrinterFiles> make_files(const Target& target);

// What an HTTP answer means: Ok for 2xx, otherwise the Status and the printer's own message
// (Moonraker's {"error": {"message"}}) or the transport's.
Result result_of(const HttpReply& reply, const std::string& what);

// ---- display ----------------------------------------------------------------------------------

// "2d 3h", "1h 23m", "45m", "< 1m"; "" for 0 or less.
std::string format_duration(int seconds);
// "512 B", "1.5 KB", "12.3 MB", "1.2 GB".
std::string format_size(std::uint64_t bytes);
// "4.56 m, 12 g", "4.56 m", "12 g"; "" when both are 0.
std::string format_filament(double mm, double grams);

} // namespace PrinterStorage
} // namespace Slic3r

#endif // slic3r_Utils_PrinterStorage_hpp_
