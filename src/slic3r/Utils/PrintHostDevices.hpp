#ifndef slic3r_PrintHostDevices_hpp_
#define slic3r_PrintHostDevices_hpp_

#include <map>
#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

// The print-host devices of one printer model.
//
// A printer preset carries exactly one address (print_host, printhost_apikey,
// printhost_authorization_type, printhost_user/password, host_type - PrintConfig.cpp:752-824,
// 4662). Somebody with three identical printers on three IPs therefore has to either retype that
// field before every send or keep three near-identical presets, which then also splits everything
// that keys off the preset name (filament and process compatibility lists).
//
// This store keeps the preset as it is and puts the addresses beside it: one list of devices per
// printer *model*, so all the variants of one machine ("0.4 nozzle", "0.2 nozzle") share it.
//
// It is modelled on the fork's own SnapmakerLan::Device store (GUI/SnapmakerLan.cpp:85-190) - a
// plain, pretty-printed JSON file written through a temporary, read tolerantly, living next to the
// hub's other state at <datadir>/hub/print_host_devices.json - and deliberately NOT on
// PrusaSlicer's PhysicalPrinter/PhysicalPrinterCollection (libslic3r/Preset.hpp:860-1030), whose
// every creation and editing path was cut by BambuStudio (the migration is #if 0'd at
// Preset.cpp:3660, the Tab.cpp wiring is commented out, PresetBundle.cpp:3046 hardcodes the link
// empty) and which conflates "a device" with "the presets selected under it" - the opposite of the
// one-preset-many-addresses shape wanted here.
//
// Everything in here is wx-free on purpose: it is the model, it is linked into the Catch2 suite
// (tests/slic3rutils), and the GUI, the hub's /api/printers and later the send fan-out are all just
// callers.
//
// Secrets: an API key / password is written in clear, exactly as the printer preset writes
// printhost_apikey today - there is no secret store anywhere in this codebase (no wxSecretStore, no
// CredWrite, no keychain call), so this file is no more exposed than the preset next to it.

namespace Slic3r {

class Preset;
class PresetBundle;

namespace PrintHostDevices {

// One addressable printer of a model.
struct Device
{
    std::string id;            // generated once, stable for the life of the entry
    std::string alias;         // what a person calls it: "Left bay"
    std::string address;       // host[:port], an IP or a URL - whatever print_host would hold
    std::string host_type;     // a host_type enum key: "octoprint", "elegoolink", ...
    std::string auth_type;     // printhost_authorization_type: "key" | "user"
    std::string apikey;        // printhost_apikey    (auth_type == key)
    std::string user;          // printhost_user      (auth_type == user)
    std::string password;      // printhost_password  (auth_type == user)
    std::string printer_model; // the preset's printer_model, kept for display and provenance
    long long   created { 0 };   // unix seconds
    long long   last_used { 0 }; // unix seconds, 0 = never

    bool empty() const { return address.empty(); }
    // What to show when nobody named it.
    std::string display_name() const { return alias.empty() ? address : alias; }
};

// ---- where it lives ----

// <datadir>/hub/print_host_devices.json, unless a test pointed it somewhere else.
std::string store_path();
// Tests only: use this file instead. An empty path restores the default.
void        set_store_path(const std::string& path);

// ---- the key ----

// The identity a device list hangs off: the preset's printer_model, so every variant of one machine
// ("0.4 nozzle", "0.2 nozzle") and every user preset derived from it share one list. A preset with
// no printer_model (a hand-made one) falls back to its own name, the finest grouping still
// available.
//
// The vendor is deliberately NOT part of the key: Preset::vendor is only set on system presets, so
// prefixing it would give "Snapmaker U1 (0.4 nozzle)" and the user's own copy of it two different
// keys - exactly the split this feature exists to avoid. printer_model strings are vendor-qualified
// in practice ("Elegoo Centauri Carbon", "Snapmaker U1", "Bambu Lab X1 Carbon").
std::string model_key(const std::string& printer_model, const std::string& preset_name);
std::string model_key_for(const Preset& printer_preset);
// The model of the printer preset that is selected right now. Empty when there is none.
std::string current_model_key(const PresetBundle& bundle);

// An address as it is compared: trimmed, no trailing slash, lower-cased. Two presets pointing at
// "192.168.1.41/" and "192.168.1.41" are one printer.
std::string normalize_address(const std::string& address);

// ---- the list ----

std::vector<Device>                             devices(const std::string& model_key);
std::map<std::string, std::vector<Device>>      all_devices();
bool                                            find(const std::string& model_key, const std::string& id, Device& out);
// Adds `d` (its id and created are filled in) and returns false with `error` set when the address
// is empty or already taken in this model.
bool                                            add(const std::string& model_key, Device& d, std::string& error);
// Replaces the entry with the same id. False when there is no such device or the new address
// collides with another one.
bool                                            update(const std::string& model_key, const Device& d, std::string& error);
bool                                            remove(const std::string& model_key, const std::string& id);
// The device this model was last sent to, "" when none ever was. It is a *memory*, not a
// setting: nothing about the preset changes with it, it only decides which row the send dialog
// and the Device tab preselect. (Phase 1 called this "current" and let a button write the chosen
// device into the preset; that made one device the model's main printer, which is exactly the
// shape this feature exists to remove.)
std::string                                     last_used_id(const std::string& model_key);
// Records that this device was just used: the model's last_used_id and the device's own
// last_used timestamp, together.
void                                            set_last_used(const std::string& model_key, const std::string& id);

// ---- config <-> device ----

// Reads the host fields of a printer preset's config into a device (id and alias left empty).
Device from_config(const DynamicPrintConfig& config);
// Puts a device's address and credentials into a config - the fields PrintHost::get_print_host
// reads. It is meant for a *copy* of the preset's config: the send builds one per device and hands
// it to PrintHostJob, and the dialog's Test builds one to probe with. The preset itself is never
// written to by this feature (phase 1's "Use this device" did, and no longer exists).
void   apply_to_config(const Device& d, DynamicPrintConfig& config);
// A copy of the printer preset's config with `d` applied - the one-liner every caller wanted.
DynamicPrintConfig config_for(const Device& d, const DynamicPrintConfig& preset_config);

// host_type as the config enum, defaulting to htOctoPrint for an unknown or empty key.
PrintHostType host_type_enum(const std::string& key);
std::string   host_type_key(PrintHostType type);
// True for the host types that answer Moonraker's /printer/objects/query, i.e. the ones whose
// status the hub can probe through RemoteControl's existing probe cache.
bool          speaks_moonraker(const std::string& host_type_key);

// ---- migration ----

// One printer preset's host fields, as the migration sees them.
struct PresetHost
{
    std::string model_key;
    std::string preset_name;
    std::string address;
    std::string host_type;
    std::string auth_type;
    std::string apikey;
    std::string user;
    std::string password;
    std::string printer_model;
};

// The one-time import that makes an existing single-address user see their printer as device 1.
//
// The rule, per preset with a non-empty print_host:
//   - if this (model, address) pair has been imported before, do nothing (so a device somebody
//     deleted stays deleted);
//   - else, if no device of that model already has that address, create one, alias = the preset
//     name, carrying the preset's host type and credentials;
//   - either way, remember the pair as imported.
// Returns how many devices were created. Idempotent: a second run creates none.
int migrate_from_presets(const std::vector<PresetHost>& presets);
// The same, reading every visible printer preset of the bundle.
int migrate_from_presets(const PresetBundle& bundle);

} // namespace PrintHostDevices
} // namespace Slic3r

#endif // slic3r_PrintHostDevices_hpp_
