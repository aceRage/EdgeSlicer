#ifndef slic3r_Utils_ElegooSdcpFiles_hpp_
#define slic3r_Utils_ElegooSdcpFiles_hpp_

// The Storage tab's backend for an Elegoo Centauri Carbon 1 (CC1): the G-code files on the
// printer's internal storage, listed, deleted and started over SDCP - the printer's websocket,
// through the one connection this process keeps to it (ElegooSdcpMonitor), never a socket of its
// own. PrinterStorage::make_files() hands one out for a Target of Kind::ElegooSdcp.
//
// The pure half (request bodies, answer parsers, the start plan) is unit tested with fixtures in
// tests/slic3rutils/elegoo_sdcp_files_tests.cpp; the backend is tested there too, through a fake
// SdcpChannel. No printer, no socket, no wx.
//
// Sources for the commands (facts only, no code copied):
//  * Elegoo's SDCP V3.0.0 specification (github.com/cbd-tech/SDCP-Smart-Device-Control-Protocol-V3.0.0):
//    "Retrieve File List (Cmd: 258)" - Data {"Url": "/local/..."}, answer Data {Ack, FileList [{name,
//    type 0 folder / 1 file, ...}]} - and "Batch Delete Files (Cmd: 259)" - Data {"FileList": [full
//    paths]}, answer Data {Ack, ErrData [paths that failed]}.
//  * elegoo-link (github.com/ELEGOO-3D/elegoo-link, Apache-2.0), the transport ElegooSlicer links:
//    its CC1 command table lists GET_FILE_LIST 258 and DELETE_FILE 259 (commented out there, so
//    ElegooSlicer itself never sends them).
//  * pycentauri (github.com/bjan/pycentauri, Apache-2.0), client.py list_files / delete_files and
//    docs/PROTOCOL.md, verified on CC1 firmware V0.3.0-o (OpenCentauri, the stock `app` binary):
//    258 with exactly {"Url": "/local"} (the printer's own web UI request), answer
//    Data.Data.FileList[] of {name (full path "/local/x.gcode"), FileSize, TotalLayers, CreateTime};
//    259 with {"FileList": ["/local/<name>"]} (full paths; {"Url": ...} is ignored); Ack 0 and the
//    file is gone from the next list. Its warning: an unexpected payload shape can crash the
//    printer's firmware (and the print on it), so the request bodies here are exactly those, and
//    nothing else is ever sent - no folder list, no paging, no other Cmd.
//
// NOT VERIFIED on the owner's firmware (V1.4.49, SDCP V3.0.0) - every one of these is logged the
// first time it is seen at runtime ("[Storage][CC1][unverified]"), so the owner's first run settles it:
//  * that 258 / 259 answer at all on stock V1.4.49 (pycentauri verified the OpenCentauri build);
//  * the entry field names FileSize / TotalLayers / CreateTime (the specification shows only name /
//    type / usedSize / totalSize / storageType; usedSize is the storage's, never the file's size);
//  * CreateTime's unit (unix seconds assumed; milliseconds detected by size; anything else -> 0);
//  * whether "name" carries the "/local/" prefix (both forms are accepted);
//  * "type" 0 = folder (folders are skipped; the tab lists the storage root only);
//  * 259's ErrData (a path in it means that file was not deleted) - and the delete is checked by
//    listing again, so a silent no-op is reported, not hidden.
//
// No thumbnails: neither answer carries a preview, and the only other way in - fetching the file
// itself over the printer's HTTP port (/downloadFile, unverified on stock firmware) - would pull
// whole G-code files just to draw a list. Download is left out for the same reason.

#include "PrinterStorage.hpp"
#include "PrintHostDevices.hpp"
#include "PrintHostDeviceStatus.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {
namespace SdcpFiles {

constexpr int CMD_FILE_LIST    = 258;
constexpr int CMD_DELETE_FILES = 259;
constexpr int CMD_START_PRINT  = 128;

// The internal storage, as the file commands name it ("/local"; "/usb" is a USB stick, not listed).
constexpr const char* LOCAL_ROOT = "/local";

// ---- request bodies (Data of the SDCP envelope) ----------------------------------------------------

// Cmd 258: exactly {"Url":"/local"}.
nlohmann::json list_request_data();
// Cmd 259: exactly {"FileList":["/local/<path>", ...]}.
nlohmann::json delete_request_data(const std::vector<std::string>& relative_paths);
// "/local/<path>" for a path relative to the storage root.
std::string full_path(const std::string& relative_path);
// A printer path ("/local/a.gcode", "local/a.gcode", "a.gcode") relative to the storage root:
// "a.gcode". Empty for a path on another storage ("/usb/...") or one that is not valid.
std::string relative_path(const std::string& printer_path);

// ---- answers ---------------------------------------------------------------------------------------

struct ListedFile
{
    PrinterStorage::FileEntry entry;
    int                       total_layers { 0 }; // TotalLayers, 0 = not given
};

struct ListAnswer
{
    bool                    parsed { false };    // JSON, and a FileList was found somewhere in it
    int                     ack { -1 };          // Data.Data.Ack, -1 when absent
    std::vector<ListedFile> files;               // printable-or-listed files of the storage root
    int                     folders { 0 };       // entries skipped as folders
    int                     other_storage { 0 }; // entries skipped as not "/local"
    int                     unlisted { 0 };      // entries skipped by name (PrinterStorage::is_listed_name) or path
    // What was seen, for the one-time "unverified" log line: the keys of the first entry, whether a
    // name came with the "/local/" prefix, and how CreateTime was read.
    std::vector<std::string> first_entry_keys;
    bool                     names_prefixed { false };
    std::string              create_time_form; // "seconds", "milliseconds", "string", "absent", "other"
};

// One frame (the answer to Cmd 258) as text. Never throws. FileList is looked for at Data.Data
// (pycentauri), then Data, then the top level.
ListAnswer parse_list_answer(const std::string& frame_text);

struct DeleteAnswer
{
    bool                     parsed { false };
    int                      ack { -1 };
    std::vector<std::string> failed; // ErrData, relative to the storage root where they can be
};
DeleteAnswer parse_delete_answer(const std::string& frame_text);

// The Ack of any answer frame (Data.Data.Ack, then Data.Ack); -1 when absent or not JSON.
int ack_of(const std::string& frame_text);

// Cmd 128's Ack as one sentence (the specification's SDCP_PRINT_CTRL_ACK_*, as ElegooLink reads them).
std::string start_ack_text(int ack);

// "/local/a.gcode" vs "a.gcode": the same file whatever directory part either carries.
bool same_file(const std::string& a, const std::string& b);

// ---- the start of a stored file: the send's rules ----------------------------------------------------
//
// A file already on the printer starts through the same Cmd 128 an Upload and Print sends
// (ElegooStartPrint::build_payload), with the send dialog's remembered choices (bed leveling,
// timelapse, plate side), and under the same slot send policy (PrintHostDevices::slot_send_policy):
//  * printer not answering -> not started;
//  * printing or paused (UploadOnlyBusy) -> not started;
//  * no CANVAS, or a CANVAS with no loaded tray (Start) -> started; a CANVAS that answered gets
//    canvasAttached and an empty slot_map, as the send dialog sends it;
//  * a CANVAS with loaded trays: which filaments a stored file uses is unknown here, so no tray can be
//    mapped (NeedsMapping). This is the send dialog's "every filament As sliced" (design D4): an
//    empty slot_map, the printer feeds from its own choice of trays. It is only ever started by a
//    person who was told so in the confirmation (`as_sliced_confirmation`); nothing unattended
//    starts a stored file.

struct StartPlan
{
    PrintHostDevices::SlotSendFacts    facts;
    PrintHostDevices::SlotSendPolicy   policy { PrintHostDevices::SlotSendPolicy::NotApplicable };
    bool                               may_start { false };
    bool                               as_sliced_confirmation { false }; // the confirmation must say the CANVAS picks
    std::string                        refusal;  // why not, one sentence; "" when it may start
    std::string                        advice;   // what the confirmation adds (English): the choices, the CANVAS
    std::map<std::string, std::string> extended_info; // what ElegooStartPrint::build_payload gets
};

// `options`: the send dialog's remembered "timeLapse" / "heatedBedLeveling" / "bedType" ("1"/"0",
// BedType as text). `pc_bed_type`: BedType::btPC as int (the smooth side). `printer`: its name.
StartPlan plan_start(const PrintHostDevices::Status& st, const std::map<std::string, std::string>& options, int pc_bed_type,
                     const std::string& printer);

// The Data of the Cmd 128 for `relative_path` - taken out of ElegooStartPrint::build_payload's own
// envelope, so it is byte for byte what an Upload and Print sends.
nlohmann::json start_request_data(const std::map<std::string, std::string>& extended_info, int pc_bed_type,
                                  const std::string& relative_path);

// ---- the connection --------------------------------------------------------------------------------

struct Reply
{
    bool        sent { false };
    bool        answered { false };
    int         ack { -1 };
    std::string frame; // the answer as the printer sent it
    std::string error; // why not (English)
};

struct PrinterRead
{
    bool                     reached { false };
    std::string              error;
    std::string              state;    // Sdcp::hub_state: printing | paused | complete | cancelled | error | standby
    std::string              filename; // PrintInfo.Filename of the job it is on
    PrintHostDevices::Status status;   // the send dialog's read: CANVAS trays, state (read_printer only)
};

// How the backend reaches the printer. The real one goes through ElegooSdcpMonitor's shared
// connection; the tests hand in a fake. Blocking; worker threads only.
class SdcpChannel
{
public:
    virtual ~SdcpChannel() = default;
    virtual Reply command(int cmd, const nlohmann::json& data, int timeout_ms) = 0;
    // What it is doing (a fresh Cmd 0): for the delete guard.
    virtual PrinterRead read_state() = 0;
    // What the send dialog reads (PrintHostDevices::probe: Cmd 0, and Cmd 324 when a CANVAS is attached).
    virtual PrinterRead read_printer() = 0;
};

// The real channel to the printer at `address` (a print host address; the SDCP host is taken out of
// it), model `printer_model` for the CANVAS read.
std::shared_ptr<SdcpChannel> make_monitor_channel(const std::string& address, const std::string& printer_model);

// ---- the backend -----------------------------------------------------------------------------------

class Cc1Files : public PrinterStorage::IPrinterFiles
{
public:
    Cc1Files(std::shared_ptr<SdcpChannel> channel, std::map<std::string, std::string> start_options, int pc_bed_type,
             std::string printer_name);

    PrinterStorage::Result list(std::vector<PrinterStorage::FileEntry>& out) override;
    // From the last list (TotalLayers): no request.
    PrinterStorage::Result metadata(const std::string& path, PrinterStorage::FileMetadata& out) override;
    // There are none: NotFound, no request.
    PrinterStorage::Result thumbnail(const std::string& thumbnail_path, std::string& bytes) override;
    // Refused for the file the printer is printing; checked by listing again.
    PrinterStorage::Result remove(const std::string& path) override;
    PrinterStorage::Result printer_state(std::string& state, std::string& current_file) override;
    PrinterStorage::Result start_check(const std::string& path, std::string& state, std::string& current_file,
                                       std::string& advice) override;
    // Refused when the plan read now differs from the one the person confirmed (start_check).
    PrinterStorage::Result start_print(const std::string& path) override;

private:
    PrinterStorage::Result list_raw(std::vector<ListedFile>& out);

    std::shared_ptr<SdcpChannel>       m_channel;
    std::map<std::string, std::string> m_options;
    int                                m_pc_bed_type;
    std::string                        m_printer;
    std::mutex                         m_mutex;          // the members below; the model calls from several workers
    std::map<std::string, int>         m_layers;         // path -> TotalLayers, from the last list
    std::map<std::string, std::string> m_confirmed_plan; // path -> the advice the person was shown
};

// The backend for a Target of Kind::ElegooSdcp over the real channel (BedType::btPC as the smooth
// side); nullptr for any other target.
std::shared_ptr<PrinterStorage::IPrinterFiles> make_cc1_files(const PrinterStorage::Target& target);

// Tests: forget which "unverified" facts were already logged, so each test sees its own log lines.
void reset_logged_for_tests();
// Tests: the "[Storage][CC1][unverified]" lines logged so far (the same text that goes to the log).
std::vector<std::string> logged_for_tests();

} // namespace SdcpFiles
} // namespace Slic3r

#endif // slic3r_Utils_ElegooSdcpFiles_hpp_
