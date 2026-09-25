#include "AmsDrying.hpp"
#include "AmsDualLayout.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>

namespace Slic3r { namespace GUI { namespace AmsDrying {

using nlohmann::json;

namespace {

bool read_int(const json &j, const char *key, int &out)
{
    if (!j.contains(key))
        return false;
    const json &v = j[key];
    if (v.is_number()) {
        out = v.get<int>();
        return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        char *end = nullptr;
        const long n = std::strtol(s.c_str(), &end, 10);
        if (end != s.c_str()) {
            out = int(n);
            return true;
        }
    }
    return false;
}

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

// Bambu Studio's BBL profiles (fdm_filament_<type>.json and the "@base" presets), most common
// value per filament type. Order: temps idle N3F, idle N3S, print N3F, print N3S; hours likewise.
//                                                      N3F    N3S   temps idle/idle/print/print  hours          heat  soft
const Preset k_presets[] = {
    { "PLA",      true,  true,  45, 45, 45, 45,  12, 12, 12, 12,   45,  50 },
    { "PLA-CF",   true,  true,  45, 45, 45, 45,  12, 12, 12, 12,   45,  50 },
    { "PETG",     true,  true,  65, 65, 55, 55,  12, 12, 12, 12,   75,  60 },
    { "PETG-CF",  true,  true,  65, 65, 55, 55,  12, 12, 12, 12,   75,  60 },
    { "PCTG",     true,  true,  65, 65, 55, 55,  12, 12, 12, 12,   75,  60 },
    { "PET-CF",   false, true,  65, 80, 65, 80,  12, 12, 12, 12,  165, 150 },
    { "ABS",      false, true,  65, 80, 65, 75,  12,  8, 12,  8,   90,  80 },
    { "ABS-GF",   false, true,  65, 80, 65, 75,  12,  8, 12,  8,   90,  80 },
    { "ASA",      false, true,  65, 80, 65, 80,  12,  8, 12,  8,  100,  85 },
    { "ASA-CF",   false, true,  65, 80, 65, 80,  12,  8, 12,  8,  100,  85 },
    { "ASA-AERO", false, true,  65, 80, 65, 80,  12,  8, 12,  8,  100,  85 },
    { "HIPS",     false, true,  65, 80, 65, 75,  12, 12, 12, 12,   90,  80 },
    { "PC",       false, true,  65, 80, 65, 80,  12,  8, 12,  8,  105,  90 },
    { "PA",       false, true,  65, 80, 65, 80,  12, 12, 12, 12,   90,  85 },
    { "PA-CF",    false, true,  65, 85, 65, 85,  12, 12, 12, 12,  165, 150 },
    { "PA-GF",    false, true,  65, 85, 65, 85,  12, 12, 12, 12,  165, 150 },
    { "PA6-CF",   false, true,  65, 85, 65, 85,  12, 12, 12, 12,  165, 150 },
    { "PAHT-CF",  false, true,  65, 85, 65, 85,  12, 12, 12, 12,  165, 150 },
    { "PPA-CF",   false, false, 65, 85, 65, 85,  12, 12, 12, 12,  165, 150 },
    { "PPA-GF",   false, false, 65, 85, 65, 85,  12, 12, 12, 12,  165, 150 },
    { "PPS",      false, true,  65, 80, 65, 80,  12, 12, 12, 12,   90,  85 },
    { "PPS-CF",   false, false, 65, 85, 65, 85,  12, 12, 12, 12,  165, 150 },
    { "PP",       true,  true,  60, 60, 50, 50,  12, 12, 12, 12,   60,  55 },
    { "PP-CF",    true,  true,  60, 60, 50, 50,  12, 12, 12, 12,   60,  55 },
    { "PP-GF",    true,  true,  60, 60, 50, 50,  12, 12, 12, 12,   60,  55 },
    { "PE",       true,  true,  45, 45, 45, 45,  12, 12, 12, 12,   45,  50 },
    { "PE-CF",    true,  true,  45, 45, 45, 45,  12, 12, 12, 12,   45,  50 },
    { "EVA",      true,  true,  45, 45, 45, 45,  12, 12, 12, 12,   45,  50 },
    { "PHA",      true,  true,  45, 45, 45, 45,  12, 12, 12, 12,   45,  50 },
    { "PVA",      false, true,  65, 85, 65, 70,  12, 18, 12, 18,   75,  75 },
    { "BVOH",     true,  true,  60, 60, 45, 45,  12, 12, 12, 12,   65,  50 },
    { "TPU",      false, true,  65, 75, 45, 45,  12, 18, 12, 18,   45,  50 },
};

} // namespace

void parse_dry_fields(const json &j_ams, DryState &state)
{
    if (j_ams.contains("info") && j_ams["info"].is_string()) {
        const AmsDual::UnitInfoBits bits = AmsDual::parse_unit_info(j_ams["info"].get<std::string>());
        state.has_status = true;
        state.status     = bits.dry_status;
        state.sub_status = bits.dry_sub_status;
        state.fan1       = bits.dry_fan1;
        state.fan2       = bits.dry_fan2;
    }

    if (j_ams.contains("dry_setting") && j_ams["dry_setting"].is_object()) {
        const json &s = j_ams["dry_setting"];
        state.has_settings = true;
        if (s.contains("dry_filament") && s["dry_filament"].is_string())
            state.setting_filament = s["dry_filament"].get<std::string>();
        read_int(s, "dry_temperature", state.setting_temp);
        read_int(s, "dry_duration", state.setting_hours);
    }

    if (j_ams.contains("dry_sf_reason") && j_ams["dry_sf_reason"].is_array()) {
        state.has_cannot_reasons = true;
        state.cannot_reasons.clear();
        for (const json &r : j_ams["dry_sf_reason"])
            if (r.is_number_integer())
                state.cannot_reasons.push_back(r.get<int>());
    }
}

bool unit_has_heater(int unit_type) { return unit_type == AmsDual::UNIT_N3F || unit_type == AmsDual::UNIT_N3S; }

bool unit_supports_remote_dry(bool fun2_remote_dry, int unit_type) { return fun2_remote_dry && unit_has_heater(unit_type); }

bool is_drying_active(int status)
{
    return status == int(DryStatus::Checking) || status == int(DryStatus::Drying) || status == int(DryStatus::Error) ||
           status == int(DryStatus::CannotStopHeatOutOfControl);
}

bool is_idle(const DryState &state)
{
    if (!state.has_status)
        return true;
    return state.status == int(DryStatus::Off) || state.status == int(DryStatus::Cooling);
}

bool is_error(const DryState &state) { return state.has_status && state.status == int(DryStatus::Error); }

bool shows_drying_icon(const DryState &state, bool fun2_remote_dry, int left_dry_minutes)
{
    if (fun2_remote_dry && state.has_status)
        return is_drying_active(state.status);
    return left_dry_minutes > 0;
}

bool limits_for(int unit_type, Limits &out)
{
    // AMSDryCtrWin::update_normal_description: { N3F, 45, 65, "AMS2" }, { N3S, 45, 85, "AMS-S" }; 1-24 h.
    if (unit_type == AmsDual::UNIT_N3F) {
        out = Limits{45, 65, 1, 24};
        return true;
    }
    if (unit_type == AmsDual::UNIT_N3S) {
        out = Limits{45, 85, 1, 24};
        return true;
    }
    return false;
}

const Preset *preset_for_type(const std::string &filament_type)
{
    const std::string t = upper(filament_type);
    if (t.empty())
        return nullptr;
    for (const Preset &p : k_presets)
        if (t == p.type)
            return &p;
    const size_t dash = t.find('-');
    if (dash != std::string::npos && dash > 0) {
        const std::string family = t.substr(0, dash);
        for (const Preset &p : k_presets)
            if (family == p.type)
                return &p;
    }
    return nullptr;
}

const Preset &fallback_preset() { return k_presets[0]; }

std::vector<std::string> preset_types()
{
    std::vector<std::string> out;
    out.reserve(std::size(k_presets));
    for (const Preset &p : k_presets)
        out.emplace_back(p.type);
    return out;
}

int preset_temp(const Preset &p, int unit_type, bool printing)
{
    const bool n3s = unit_type == AmsDual::UNIT_N3S;
    if (printing)
        return n3s ? p.temp_print_n3s : p.temp_print_n3f;
    return n3s ? p.temp_idle_n3s : p.temp_idle_n3f;
}

int preset_hours(const Preset &p, int unit_type, bool printing)
{
    const bool n3s = unit_type == AmsDual::UNIT_N3S;
    if (printing)
        return n3s ? p.hours_print_n3s : p.hours_print_n3f;
    return n3s ? p.hours_idle_n3s : p.hours_idle_n3f;
}

bool preset_fully_dries_on(const Preset &p, int unit_type)
{
    if (unit_type == AmsDual::UNIT_N3F)
        return p.full_dry_n3f;
    if (unit_type == AmsDual::UNIT_N3S)
        return p.full_dry_n3s;
    return false;
}

int cooling_temp_for(const std::string &filament_type)
{
    const Preset *p = preset_for_type(filament_type);
    return p ? p->softening : 50;
}

int recommended_temp(int unit_type, const std::vector<std::string> &loaded_types, bool printing, std::string *default_type)
{
    auto temp_of = [&](const Preset &p) {
        if (printing)
            return std::min({preset_temp(p, unit_type, true), p.softening, p.heat_distortion});
        return preset_temp(p, unit_type, false);
    };

    const Preset *best      = nullptr;
    int           best_temp = 0;
    for (const std::string &type : loaded_types) {
        const Preset *p = preset_for_type(type);
        if (!p)
            p = &fallback_preset(); // no type or no preset: Bambu treats it as PLA
        const int t = temp_of(*p);
        if (!best || t < best_temp) {
            best      = p;
            best_temp = t;
        }
    }
    if (!best) {
        best      = &fallback_preset();
        best_temp = temp_of(*best);
    }
    if (default_type)
        *default_type = best->type;
    return best_temp;
}

Check check_request(int unit_type, const std::string &filament_type, int temp, int hours, bool printing, int recommended,
                    bool any_filament_inserted)
{
    Check c;
    if (limits_for(unit_type, c.limits)) {
        if (temp > c.limits.max_temp) {
            c.issues.push_back(Issue::TempAboveMax);
            c.can_start = false;
        } else if (temp < c.limits.min_temp) {
            c.issues.push_back(Issue::TempBelowMin);
            c.can_start = false;
        }
        const Preset *p = preset_for_type(filament_type);
        if (!p || !preset_fully_dries_on(*p, unit_type))
            c.issues.push_back(Issue::MayNotFullyDry);
    } else {
        // No heater: nothing may be started at all.
        c.can_start = false;
    }

    const Preset *p = preset_for_type(filament_type);
    if (printing && temp > recommended) {
        c.issues.push_back(Issue::AbovePrintingLimit);
        c.can_start = false;
    } else if (p && any_filament_inserted && temp > p->heat_distortion) {
        // Only with filament inserted: an empty unit only has the hardware limit to respect.
        c.heat_distortion = p->heat_distortion;
        c.issues.push_back(Issue::AboveHeatDistortion);
        c.can_start = false;
    }

    if (hours < c.limits.min_hours) {
        c.issues.push_back(Issue::HoursBelowMin);
        c.can_start = false;
    } else if (hours > c.limits.max_hours) {
        c.issues.push_back(Issue::HoursAboveMax);
        c.can_start = false;
    }
    return c;
}

json build_start(const std::string &sequence_id, int ams_id, const std::string &filament_type, int temp, int hours, bool rotate_tray,
                 int cooling_temp, bool close_power_conflict)
{
    json j;
    j["print"]["command"]              = "ams_filament_drying";
    j["print"]["sequence_id"]          = sequence_id;
    j["print"]["ams_id"]               = ams_id;
    j["print"]["mode"]                 = 1; // DevAms::DryCtrlMode::OnTime
    j["print"]["filament"]             = filament_type;
    j["print"]["temp"]                 = temp;
    j["print"]["duration"]             = hours;
    j["print"]["humidity"]             = 0;
    j["print"]["rotate_tray"]          = rotate_tray;
    j["print"]["cooling_temp"]         = cooling_temp;
    j["print"]["close_power_conflict"] = close_power_conflict;
    return j;
}

json build_stop(const std::string &sequence_id, int ams_id)
{
    json j;
    j["print"]["command"]              = "ams_filament_drying";
    j["print"]["sequence_id"]          = sequence_id;
    j["print"]["ams_id"]               = ams_id;
    j["print"]["mode"]                 = 0; // DevAms::DryCtrlMode::Off
    j["print"]["filament"]             = "";
    j["print"]["temp"]                 = 0;
    j["print"]["duration"]             = 0;
    j["print"]["humidity"]             = 0;
    j["print"]["rotate_tray"]          = false;
    j["print"]["cooling_temp"]         = 0;
    j["print"]["close_power_conflict"] = false;
    return j;
}

}}} // namespace Slic3r::GUI::AmsDrying
