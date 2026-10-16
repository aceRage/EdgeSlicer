#ifndef slic3r_Utils_BambuConnectStatus_hpp_
#define slic3r_Utils_BambuConnectStatus_hpp_

// Status for printers that print through Bambu Connect (see Utils/BambuConnect.hpp).
//
// Bambu Connect hands nothing back to the slicer, so the sidebar's AMS sync and the pre-slice
// nozzle match had no data for those printers. Two sources fill that gap:
//
// 1. Status-only LAN printers. A printer bound to a Bambu account still answers read-only status
//    requests on its LAN MQTT broker with its access code; only print-namespace control commands
//    need Bambu's signature, and EdgeSlicer never signs or impersonates anything. Such a printer
//    can be added "for status only": DeviceManager then sends it nothing but the two status
//    requests (status_only_allows), and the print jobs refuse it (the job goes through Bambu Connect).
//
// 2. A snapshot of the last AMS trays and nozzles seen per printer, kept in the app config, which
//    the AMS sync offers ("AMS as of <time>") when no printer is connected.
//
// Everything here except the AppConfig accessors is pure, for tests/slic3rutils/bambu_connect_status_tests.cpp.

#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class AppConfig;

namespace BambuConnectStatus {

// AppConfig sections (keyed by the printer's serial, like "access_code"). Never logged.
extern const char *const STATUS_ONLY_SECTION; // "bambu_status_only": "1" = status only
extern const char *const SNAPSHOT_SECTION;    // "bambu_status_snapshot": to_json(Snapshot)

// -------------------------------------------------------------------------------------------
// Status-only printers
// -------------------------------------------------------------------------------------------

// The only MQTT payloads a status-only printer is sent: a single {"pushing":{"command":"pushall",...}}
// or {"info":{"command":"get_version",...}} block. Anything else - every "print", "system",
// "camera", "xcam" command, a second block, unparsable text - is refused.
bool status_only_allows(const std::string &payload_json);

// "<namespace>/<command>" of a payload for log lines ("print/ledctrl"), "?" when it has none.
// Carries no serial, IP or code.
std::string command_label(const std::string &payload_json);

bool is_status_only(const AppConfig &config, const std::string &dev_id);
void set_status_only(AppConfig &config, const std::string &dev_id, bool status_only);

// -------------------------------------------------------------------------------------------
// AMS / nozzle snapshot
// -------------------------------------------------------------------------------------------

struct ExtruderSnap
{
    int   id            = 0;     // physical extruder (0 = right on the H2D/H2C)
    bool  has_nozzle    = true;  // false: the printer reported no nozzle (nozzle_id 0xff)
    int   volume_type   = 0;     // NozzleVolumeType
    float diameter      = 0.4f;
    bool operator==(const ExtruderSnap &o) const
    {
        return id == o.id && has_nozzle == o.has_nozzle && volume_type == o.volume_type && diameter == o.diameter;
    }
};

struct Snapshot
{
    std::time_t                       saved_at = 0;
    std::string                       printer_model; // preset model name, "Bambu Lab H2D"
    std::string                       printer_name;  // the name the user gave the printer
    std::map<int, DynamicPrintConfig> trays;         // Sidebar::build_filament_ams_list layout
    std::vector<ExtruderSnap>         extruders;
};

// Compact JSON. Tray options are written by type (strings, bools, ints, floats and their vectors);
// anything else is left out. saved_at is written last so same_content() can ignore it.
std::string to_json(const Snapshot &snap);
bool        from_json(const std::string &json_text, Snapshot &out);

// Same trays and nozzles (the time and names do not count): nothing new to save.
bool same_content(const Snapshot &a, const Snapshot &b);

// The snapshot to offer when no printer is connected: the preferred printer's when it has one of
// the same printer model, otherwise the newest one whose printer_model is `preset_model`, otherwise
// "" (none).
std::string pick_snapshot(const std::map<std::string, Snapshot> &all, const std::string &preferred_dev_id,
                          const std::string &preset_model);

// "14:02" when `saved_at` is on the same local day as `now`, else "2026-10-14 14:02".
std::string time_label(std::time_t saved_at, std::time_t now);

// nozzle_volume_type per logical extruder from the snapshot's nozzles. `physical_of_logical[i]` is
// the physical extruder logical extruder i maps to; `current` is the preset's present value (kept
// for an extruder the snapshot has no nozzle for). Single-extruder presets pass one entry.
std::vector<int> nozzle_volume_types_for(const std::vector<ExtruderSnap> &extruders, const std::vector<int> &physical_of_logical,
                                         std::vector<int> current);

// AppConfig storage.
std::map<std::string, Snapshot> load_snapshots(const AppConfig &config);
// Writes when the content changed, or when the stored copy is more than `refresh_after_s` older
// than `snap` (so "as of" stays close to the last time the printer was seen without a write on
// every status push). Returns true when it wrote.
bool store_snapshot(AppConfig &config, const std::string &dev_id, const Snapshot &snap, std::time_t refresh_after_s = 600);

} // namespace BambuConnectStatus
} // namespace Slic3r

#endif // slic3r_Utils_BambuConnectStatus_hpp_
