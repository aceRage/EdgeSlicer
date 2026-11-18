#ifndef slic3r_GUI_AmsMatch_hpp_
#define slic3r_GUI_AmsMatch_hpp_

// The AMS auto-match of the Bambu send dialog (MachineObject::ams_filament_mapping) and the AMS
// filament blacklist decision (DeviceManager::check_filaments_in_blacklist), taken out of
// DeviceManager so the unit tests can call them. No wx, no printer, no app config: the callers
// pass in what they read from those.

#include "libslic3r/ProjectTask.hpp"
#include "nlohmann/json.hpp"

#include <map>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace AmsMatch {

struct Rgba
{
    int r = 0, g = 0, b = 0, a = 255;
};

// A filament's colour as the send dialog writes it ("#RRGGBB" or "#RRGGBBAA", as wxColour reads it).
Rgba filament_colour(const std::string& colour);
// A tray's colour as the printer reports it ("RRGGBBAA"; anything else is opaque white, as
// AmsTray::decode_color).
Rgba tray_colour(const std::string& colour);

// CIE76 colour difference, with sRGB scaled to 0..1 as RGB2Lab expects.
float colour_distance(const Rgba& a, const Rgba& b);

// The distance a pair gets when the types differ or one is see-through and the other not.
constexpr float FAR_DISTANCE = 999999.f;
// The last fallback drops a pick at or beyond this distance. On the 0..1 scale no colour
// difference reaches it (CIE76 tops out near 260), so only an opacity mismatch is dropped.
constexpr float DROP_DISTANCE = 6000.f;

struct Distance
{
    int   tray_id       = -1; // the tray map's key
    float distance      = FAR_DISTANCE;
    bool  is_type_match = false;
};

using DistanceFn = float (*)(const Rgba&, const Rgba&);

// Every filament against every tray, the trays in the map's order (one column each).
std::vector<std::vector<Distance>> distances(const std::vector<FilamentInfo>& filaments, const std::map<int, FilamentInfo>& trays,
                                            DistanceFn fn = colour_distance);

// The nearest-colour pass: the closest pair of the same type first, each tray once (a tray of
// exactly the filament's colour may serve several), a tie going to the tray whose preset id is the
// filament's. One result per filament, in order: id, and for a picked one the tray's id, colour,
// type, preset id, colours, AMS and slot, and the pick's own distance; tray_id -1 when none.
std::vector<FilamentInfo> nearest_colour(const std::vector<FilamentInfo>& filaments, const std::map<int, FilamentInfo>& trays,
                                         const std::vector<std::vector<Distance>>& distance_map);

// The last fallback: a pick at or beyond DROP_DISTANCE is taken back (tray_id -1).
void drop_far_picks(std::vector<FilamentInfo>& result);

// Whether a tray index is in the caller's exclude list.
bool excluded(int tray_index, const std::vector<int>& exclude_id);

// The AMS filament blacklist (resources/printers/filaments_blacklist.json).
struct BlacklistHit
{
    bool        in_blacklist = false;
    std::string action;      // "prohibition" or "warning"
    std::string description; // the entry's description key
};

// The vendor as the list and the presets spell it, compared without case and spaces: the list's
// "Bambulab" is the presets' "Bambu Lab".
std::string vendor_key(const std::string& vendor);

// The first entry of the list's "blacklist" that the filament falls under. An entry naming a
// vendor applies to that vendor's filaments; a "Third Party" entry applies to every filament
// (Bambu Studio now lists these warnings and the TPU prohibition with no vendor at all).
// skip_check turns every action into a warning ("skip_ams_blacklist_check").
BlacklistHit check_blacklist(const nlohmann::json& list, const std::string& vendor, const std::string& type, bool skip_check);

}}} // namespace Slic3r::GUI::AmsMatch

#endif
