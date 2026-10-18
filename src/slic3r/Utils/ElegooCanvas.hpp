#ifndef slic3r_ElegooCanvas_hpp_
#define slic3r_ElegooCanvas_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// CANVAS is Elegoo's multi-material unit for the Centauri Carbon 1. A printer reports its units over
// the same SDCP WebSocket ElegooLink already speaks (port 3030, path /websocket); command 324 answers
// with a "canvas_list", one entry per unit, each carrying a "tray_list" of slots.
//
// Started as a port of upstream OrcaSlicer PR #14885 ("Add CANVAS filament detection for Elegoo
// Centauri Carbon"), whose plugin layer does not exist in this fork. The payload shape, the
// status table and the "blank vendor is Generic" rule follow ElegooSlicer (AGPL-3.0) and its
// transport library elegoo-link (Apache-2.0); no code of either is copied.
// Design: tests/design_elegoo_canvas_sync_device_page.md.
//
// Everything declared here except canvas_query()/canvas_capture() is pure string/JSON handling and is
// exercised by tests/slic3rutils/elegoo_canvas_tests.cpp with a scripted SDCP transport - no printer,
// no socket. The networking calls live in ElegooCanvasQuery.cpp.

// What a tray's raw `status` says, kept apart from "is there filament": sources disagree on the
// numbers (design doc R1), so presence is decided by the type and colour and this is display only.
enum class CanvasTrayState
{
    Unknown,   // no status field
    Empty,     // 0 (elegoo-link: empty / disconnected; a public capture shows loaded trays at 0 too)
    Preloaded, // 1
    Loaded,    // 2 and above
};

// One slot of one CANVAS unit, normalised away from the wire format.
struct CanvasTray
{
    int         slot_index   = 0;     // flat 0-based index across all units, by list position
    int         canvas_id    = 0;     // raw canvas_id of its unit (what the start-print slot_map wants)
    int         tray_id      = 0;     // raw tray_id as the printer sent it (0- or 1-based: design doc R2)
    int         position     = 0;     // 0-based position inside its unit's tray_list
    int         status       = -1;    // raw status, -1 when absent
    bool        has_filament = false; // the printer named a material and a colour
    std::string tray_type;            // normalised family, e.g. "PLA", "PETG"
    std::string filament_type;        // as sent, e.g. "PLA Silk"
    std::string brand;                // as sent; only trustworthy for Elegoo RFID spools
    std::string filament_name;        // as sent
    std::string filament_code;        // as sent; "0x00000" = no tag read
    std::string tray_color;           // RRGGBBAA, uppercase
    std::string tray_info_idx;        // OrcaFilamentLibrary generic filament id
    int         min_nozzle_temp = 0;  // 0 when unknown
    int         nozzle_temp     = 0;  // max nozzle temperature, 0 when unknown

    CanvasTrayState state() const;
    // "#RRGGBB" for the dialogs, "" when the tray has no filament.
    std::string colour_hex() const;
};

struct CanvasUnit
{
    int  canvas_id  = 0;
    bool connected  = false;
    int  tray_count = 0;
};

struct CanvasInfo
{
    int                     unit_count       = 0;  // number of CANVAS units reported
    int                     active_canvas_id = -1; // -1 when absent
    int                     active_tray_id   = -1; // -1 = no tray in the extruder
    int                     auto_refill      = -1; // raw, -1 when absent (CC1 SDCP cannot set it)
    std::vector<CanvasUnit> units;                 // every unit, connected or not
    std::vector<CanvasTray> trays;                 // every slot of every connected unit

    bool empty() const { return trays.empty(); }
    // Trays with filament in them.
    int  loaded_count() const;
    // The tray with this raw pair, nullptr when there is none.
    const CanvasTray* find(int canvas_id, int tray_id) const;
};

// Colour as the printer reports it ("#RRGGBB", "0xRRGGBB", "RRGGBB", "RRGGBBAA")
// to the uppercase RRGGBBAA this tree stores on AmsTray. Invalid input yields
// "00000000" rather than throwing, because a bad colour must not cost the slot.
std::string canvas_normalize_color(const std::string& color);

// Collapse a printer-reported material name onto the coarse family the filament
// presets are keyed by: "PLA Silk" and "PLA-CF" both answer "PLA".
std::string canvas_normalize_filament_type(const std::string& filament_type);

// Map a normalised material to the OrcaFilamentLibrary generic filament id, so a
// detected slot can preselect a real preset. Returns an empty string when the
// material has no generic counterpart - the caller then leaves the slot unmapped
// rather than guessing.
std::string canvas_generic_filament_id(const std::string& filament_type);

CanvasTrayState canvas_tray_state(int status);

// The vendor a tray stands for: its brand, or "Generic" when the printer sent none.
std::string canvas_tray_vendor(const CanvasTray& tray);

// A brand made only of dashes (ASCII, en/em dash...) and blanks - what the CC1 reports for an
// untagged or hand-entered spool - is "Generic" (owner decision). "" stays "". Anything else is
// returned trimmed.
std::string canvas_clean_brand(const std::string& brand);
// "0x00000" (any count of zeros) - the CANVAS read no RFID tag - is "". Anything else is returned trimmed.
std::string canvas_clean_code(const std::string& code);
// The filament name the sync matches presets by: "<brand> <filament_name>" (no doubled brand),
// so "Generic PLA PRO" for an untagged spool, and just the filament name (else its type) when the
// brand is empty. Empty for an empty tray.
std::string canvas_tray_name(const CanvasTray& tray);

// Parse the payload of a command-324 reply. Split out from the query so it can be
// exercised without a printer. Returns false and sets `error` when the payload
// carries no usable canvas_list.
bool canvas_parse_payload(const std::string& payload, CanvasInfo& info, std::string& error);

// True for the printer models this fork reads CANVAS from: the Centauri Carbon 1 only (design D10).
bool canvas_capable_model(const std::string& printer_model);

// The CC1's own Generic filament preset for a family ("PLA" -> "Generic PLA @ECC"). The sync prefers
// it for an untagged or Generic spool when the profiles have it, else the usual "Generic <type>".
std::string canvas_generic_preset_name(const std::string& family);

// The host part of a print_host value ("http://h:3030/x" -> "h"), which is where the SDCP
// websocket lives whatever port the preset's HTTP address carries.
std::string sdcp_host_of(const std::string& address);

// An SDCP request as JSON text: {"Id":"","Data":{"Cmd":..,"Data":{},"RequestID":..,...,"From":1}}.
std::string sdcp_request(int cmd, const std::string& request_id, long long timestamp_ms);

// AmsConnectStatus from one status frame: 1 attached, 0 not, -1 when the frame does not say.
int sdcp_ams_connect_status(const std::string& frame);

// ---- the conversation, without a socket ----

// One SDCP connection as the query sees it. The real one (ElegooCanvasQuery.cpp) is a beast
// websocket with a deadline on every step; the tests script one.
class SdcpTransport
{
public:
    virtual ~SdcpTransport() = default;
    // Opens a fresh connection (closing any open one). The CC1 often closes its socket after it
    // has answered, so every command gets its own connection.
    virtual bool open(std::string& error) = 0;
    virtual bool send(const std::string& text, std::string& error) = 0;
    // One frame, waiting at most timeout_ms. False on timeout, close or error.
    virtual bool receive(std::string& text, int timeout_ms, std::string& error) = 0;
    virtual void close() = 0;
};

struct CanvasQueryResult
{
    bool        ok        = false;   // info holds a parsed canvas_list
    int         ams_connect_status = -1; // what Cmd 0 said, -1 = it did not say
    bool        no_canvas = false;   // Cmd 0 said no CANVAS is attached; Cmd 324 was not asked
    bool        reached   = false;   // the printer accepted a connection at all
    std::string error;               // why not ok, for the log and the dialogs (English)
    CanvasInfo  info;
};

struct CanvasQueryOptions
{
    int frame_timeout_ms = 3000; // per frame
    int step_timeout_ms  = 6000; // per command, all its frames together
    int max_frames       = 8;    // per command (status pushes may arrive in between)
    // Ids for the requests; empty / 0 = generated.
    std::string request_id;
    long long   timestamp_ms = 0;
};

// Cmd 0 (status), then Cmd 324 when AmsConnectStatus is 1 - or when no status frame carried the
// field, since an unknown firmware is asked rather than refused (design D7). No firmware version
// gate. Any failure leaves `ok` false, and the callers then behave exactly as without a CANVAS.
CanvasQueryResult canvas_query_over(SdcpTransport& transport, const CanvasQueryOptions& opts = CanvasQueryOptions());

// The "Capture Canvas data" diagnostic: Cmd 0, Cmd 1 and Cmd 324, every frame the printer sent,
// each with printer-identifying fields removed (canvas_redact_frame), plus our own parse of the
// CANVAS payload for comparison. Pretty-printed JSON text. `context_json` (an object: app version,
// printer model...) is stored under "context".
std::string canvas_capture_over(SdcpTransport& transport, const std::string& context_json,
                                const CanvasQueryOptions& opts = CanvasQueryOptions());

// A frame with every printer-identifying value removed: mainboard ids, serial numbers, IP and MAC
// addresses, the printer's own name, topics (which embed the mainboard id), and any string that
// looks like an address or a long hex id. Non-JSON text comes back as a short placeholder.
std::string canvas_redact_frame(const std::string& frame);

// ---- the networking half (ElegooCanvasQuery.cpp) ----

// Ask the printer at `address` (a print_host value) for its CANVAS state. Blocking - call it off the
// GUI thread. Bounded: a few seconds when the printer is off.
CanvasQueryResult canvas_query(const std::string& address, const CanvasQueryOptions& opts = CanvasQueryOptions());
std::string       canvas_capture(const std::string& address, const std::string& context_json,
                                 const CanvasQueryOptions& opts = CanvasQueryOptions());

} // namespace Slic3r

#endif // slic3r_ElegooCanvas_hpp_
