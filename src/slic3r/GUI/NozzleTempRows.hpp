#ifndef slic3r_GUI_NozzleTempRows_hpp_
#define slic3r_GUI_NozzleTempRows_hpp_

// Maps a printer's per-extruder temperatures (DeviceManager's ExtderData) to the rows the Device
// tab shows in its temperature column. Kept free of wx and of DeviceManager so it can be unit
// tested on its own (tests/slic3rutils/nozzle_temp_rows_tests.cpp).
//
// Bambu numbering: extruder id 0 is the main (right) nozzle, id 1 the deputy (left) one. Bambu
// Studio lists the left nozzle above the right one, each with an "L" / "R" badge; this does the
// same. The orange heater icon means "heating" (target above current by the threshold) exactly
// as for the single nozzle, and the extruder the printer reports as current gets a highlighted
// badge.

#include <algorithm>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// One entry of ExtderData::extders.
struct NozzleTempSample
{
    int id{0};     // Extder::id, 0 = right/main, 1 = left/deputy
    int temp{0};   // current, degC
    int target{0}; // target, degC
};

struct NozzleTempRow
{
    int         extruder_id{0};    // the id to address in a set-temperature command
    std::string badge;             // "L", "R", "1".. ; empty for the single-nozzle row
    int         temp{0};
    int         target{0};
    bool        heating{false};    // drives the orange heater icon
    bool        active{false};     // the extruder currently in use (highlighted badge)
};

// Same threshold StatusPanel uses for bed, chamber and the single nozzle.
constexpr int NOZZLE_TEMP_HEATING_THRESHOLD = 2;

inline bool nozzle_is_heating(int temp, int target) { return target - temp >= NOZZLE_TEMP_HEATING_THRESHOLD; }

inline std::string nozzle_badge_for(int id, int extruder_count)
{
    if (extruder_count == 2) return id == 0 ? "R" : id == 1 ? "L" : std::to_string(id + 1);
    return std::to_string(id + 1);
}

// `extders` in the order DeviceManager stored them (the order of the printer's report);
// `total_count` is ExtderData::total_extder_count and `current_id` ExtderData::current_extder_id.
//
// Returns one row (no badge, taken from extders[0], as the single-nozzle panel always did) unless
// the printer reports more than one extruder and at least two distinct ids are present; then one
// row per extruder id, highest id first (L above R for the two-nozzle Bambu printers).
inline std::vector<NozzleTempRow> nozzle_temp_rows(const std::vector<NozzleTempSample> &extders, int total_count, int current_id)
{
    std::vector<NozzleTempRow> rows;
    if (extders.empty()) return rows;

    // Distinct ids, first occurrence wins (a report never repeats one, but do not show it twice).
    std::vector<const NozzleTempSample *> uniq;
    for (const NozzleTempSample &e : extders) {
        bool seen = false;
        for (const NozzleTempSample *u : uniq)
            if (u->id == e.id) { seen = true; break; }
        if (!seen && e.id >= 0) uniq.push_back(&e);
    }

    if (total_count <= 1 || uniq.size() <= 1) {
        const NozzleTempSample &e = extders.front();
        NozzleTempRow           r;
        r.extruder_id = e.id;
        r.temp        = e.temp;
        r.target      = e.target;
        r.heating     = nozzle_is_heating(e.temp, e.target);
        rows.push_back(r);
        return rows;
    }

    // Highest id first: 1 (L) then 0 (R).
    std::sort(uniq.begin(), uniq.end(), [](const NozzleTempSample *a, const NozzleTempSample *b) { return a->id > b->id; });

    const int count = int(uniq.size());
    for (const NozzleTempSample *e : uniq) {
        NozzleTempRow r;
        r.extruder_id = e->id;
        r.badge       = nozzle_badge_for(e->id, count);
        r.temp        = e->temp;
        r.target      = e->target;
        r.heating     = nozzle_is_heating(e->temp, e->target);
        r.active      = e->id == current_id;
        rows.push_back(r);
    }
    return rows;
}

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_NozzleTempRows_hpp_
