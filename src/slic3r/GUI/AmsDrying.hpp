#pragma once

// AMS remote drying ("AMS Dryness Control"), without any wx: what the printer reports, which
// units may be dried, the per-filament presets and limits, and the exact MQTT payloads.
//
// Everything here mirrors Bambu Studio 02.0x (DeviceCore/DevFilaSystem.{h,cpp},
// DevFilaSystemCtrl.cpp, AMSDryControl.cpp, DevUtilBackend.cpp) so the printer sees the same
// commands and the user gets the same limits:
//   * only AMS 2 Pro (N3F, type 3) and AMS HT (N3S, type 4) dry, and only when the firmware
//     sets fun2 bit 5 ("is_support_remote_dry");
//   * temperature 45-65 degC on AMS 2 Pro, 45-85 degC on AMS HT; time 1-24 h;
//   * a filament's heat distortion temperature caps the setting while any slot holds filament;
//   * while the unit feeds a running print, the setting may not exceed the recommended
//     temperature of what is loaded.
// The presets are Bambu's filament_dev_ams_drying_* values from the BBL fdm_filament_*.json /
// "@base" profiles, keyed by filament type (our PrintConfig does not carry those keys).

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace AmsDrying {

// print.ams.ams[].info bits 4-7 (DevAms::DryStatus).
enum class DryStatus : int {
    Off                        = 0,
    Checking                   = 1,
    Drying                     = 2,
    Cooling                    = 3,
    Stopping                   = 4,
    Error                      = 5,
    CannotStopHeatOutOfControl = 6,
    PrdTesting                 = 7,
};

// info bits 22-23 (DevAms::DrySubStatus).
enum class DrySubStatus : int {
    Off        = 0,
    Heating    = 1,
    Dehumidify = 2,
};

// print.ams.ams[].dry_sf_reason entries (DevAms::CannotDryReason).
enum class CannotDryReason : int {
    TaskOccupied                     = 0,
    InsufficientPower                = 1,
    AmsBusy                          = 2,
    ConsumableAtAmsOutlet            = 3,
    InitiatingAmsDrying              = 4,
    NotSupportedIn2dMode             = 5,
    DryingInProgress                 = 6,
    Upgrading                        = 7,
    InsufficientPowerNeedPluginPower = 8,
    FilamentAtAmsOutletManualUnload  = 10,
};

// Drying state of one unit as reported by the printer.
struct DryState
{
    bool has_status     = false; // the unit reported an "info" field
    int  status         = 0;     // DryStatus
    int  sub_status     = 0;     // DrySubStatus
    int  fan1           = 0;
    int  fan2           = 0;

    bool        has_settings     = false; // "dry_setting" present
    std::string setting_filament;         // "dry_filament"
    int         setting_temp     = -1;    // "dry_temperature", degC
    int         setting_hours    = -1;    // "dry_duration", hours

    bool             has_cannot_reasons = false; // "dry_sf_reason" present
    std::vector<int> cannot_reasons;             // CannotDryReason values
};

// Read the drying fields of one entry of print.ams.ams[]. Fields that are absent leave the
// previous values alone, as the incremental pushes omit what did not change.
void parse_dry_fields(const nlohmann::json &j_ams, DryState &state);

// AMS 2 Pro and AMS HT have heaters; AMS and AMS lite do not.
bool unit_has_heater(int unit_type);
// The dryness dialog is offered only when both the unit and the firmware support it.
bool unit_supports_remote_dry(bool fun2_remote_dry, int unit_type);

bool is_drying_active(int status); // DevAms::AmsIsDrying
bool is_idle(const DryState &state);  // Off or Cooling (or nothing reported): Start is offered
bool is_error(const DryState &state);
// The small "drying" sun next to the humidity: the status says so, or (older firmware without a
// status) there is drying time left.
bool shows_drying_icon(const DryState &state, bool fun2_remote_dry, int left_dry_minutes);

// Hardware limits of a heated unit.
struct Limits
{
    int min_temp  = 45;
    int max_temp  = 65;
    int min_hours = 1;
    int max_hours = 24;
};
bool limits_for(int unit_type, Limits &out); // false for units without a heater

// Bambu's drying preset for one filament type.
struct Preset
{
    const char *type;
    bool        full_dry_n3f;     // filament_dev_ams_drying_ams_limitations contains "0"
    bool        full_dry_n3s;     // ... contains "1"
    int         temp_idle_n3f;    // filament_dev_ams_drying_temperature[0]
    int         temp_idle_n3s;    // [1]
    int         temp_print_n3f;   // [2]
    int         temp_print_n3s;   // [3]
    int         hours_idle_n3f;   // filament_dev_ams_drying_time[0..3]
    int         hours_idle_n3s;
    int         hours_print_n3f;
    int         hours_print_n3s;
    int         heat_distortion;  // filament_dev_ams_drying_heat_distortion_temperature
    int         softening;        // filament_dev_drying_softening_temperature
};

// Preset for a filament type: exact match (case-insensitive), then the family before the first
// '-' ("PLA-CF" -> "PLA"). nullptr when unknown.
const Preset *preset_for_type(const std::string &filament_type);
// Bambu falls back to PLA (GFA00) when a slot has no usable type.
const Preset &fallback_preset();
// Types offered in the dialog's filament list, in table order.
std::vector<std::string> preset_types();

int  preset_temp(const Preset &p, int unit_type, bool printing);
int  preset_hours(const Preset &p, int unit_type, bool printing);
bool preset_fully_dries_on(const Preset &p, int unit_type);
// "cooling_temp" of the start command: the filament's softening temperature, 50 when unknown.
int  cooling_temp_for(const std::string &filament_type);

// The temperature Bambu recommends for a unit: the lowest over the loaded slots of the idle
// temperature (idle) or min(print temperature, softening, heat distortion) (printing). Empty or
// unknown types count as PLA; with nothing loaded, PLA is used. `default_type` receives the type
// that set the minimum, which the dialog preselects.
int recommended_temp(int unit_type, const std::vector<std::string> &loaded_types, bool printing, std::string *default_type = nullptr);

enum class Issue {
    TempAboveMax,        // blocks
    TempBelowMin,        // blocks
    MayNotFullyDry,      // warning only: this unit cannot fully dry the filament
    AbovePrintingLimit,  // blocks: unit feeds the running print and temp > recommended
    AboveHeatDistortion, // blocks: filament inserted and temp > the filament's heat distortion temp
    HoursBelowMin,       // blocks
    HoursAboveMax,       // blocks
};

struct Check
{
    bool               can_start = true;
    std::vector<Issue> issues;
    int                heat_distortion = 0; // for the AboveHeatDistortion message
    Limits             limits;
};

// AMSDryCtrWin::update_normal_description, minus the widgets.
Check check_request(int unit_type, const std::string &filament_type, int temp, int hours, bool printing, int recommended,
                    bool any_filament_inserted);

// {"print":{"command":"ams_filament_drying","mode":1,...}} - DevFilaSystem::CtrlAmsStartDryingHour.
nlohmann::json build_start(const std::string &sequence_id, int ams_id, const std::string &filament_type, int temp, int hours,
                           bool rotate_tray, int cooling_temp, bool close_power_conflict = false);
// {"print":{"command":"ams_filament_drying","mode":0,...}} - DevFilaSystem::CtrlAmsStopDrying.
nlohmann::json build_stop(const std::string &sequence_id, int ams_id);

}}} // namespace Slic3r::GUI::AmsDrying
