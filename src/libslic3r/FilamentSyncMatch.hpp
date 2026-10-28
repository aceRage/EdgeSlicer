#pragma once

#include <string>
#include <vector>

namespace Slic3r {
namespace FilamentSyncMatch {

// Orca #13265 (closest-colour rule only): when a connected Snapmaker printer reports a filament
// whose name has no exact preset, prefer the preset of the same vendor and type whose default
// colour is closest to the reported colour. Pure logic, no preset access, so it can be tested.

// "#RRGGBB", "RRGGBB", "#RRGGBBAA" or "RRGGBBAA" (alpha ignored) as 0xRRGGBB. False when the text
// is not one of those forms.
bool parse_rgb(const std::string &text, unsigned int &rgb);

// Squared Euclidean distance between two 0xRRGGBB colours in RGB space.
unsigned int rgb_distance_sq(unsigned int a, unsigned int b);

// Index of the candidate colour closest to `target`, or -1 when `target` cannot be read or there
// are no candidates. A candidate with an empty or unreadable colour counts as black (as upstream:
// a profile without a default colour is assumed to be a poorer match than one with a colour).
// Ties keep the first candidate.
int closest_colour_index(const std::string &target, const std::vector<std::string> &candidate_colours);


// ---- Matching a reported printer filament to a preset ------------------------------------------
//
// What a Snapmaker U1 reports per toolhead (print_task_config, read from the spool's RFID tag or set
// on the printer) is a vendor, a type and a sub-type; SSWCP names that "<vendor> <type>[ <sub>]".
// The slicer has to turn it into a filament preset twice: in the sidebar dropdown's
// "Machine Filament" section and when the Sync button fills the project's filaments. Both use
// match_preset() so they agree. Pure logic over a flat list, so it can be tested without presets.

struct Candidate
{
    std::string name;           // full preset name, e.g. "Generic PLA @U1 0.4 nozzle"
    std::string vendor;         // filament_vendor[0]
    std::string type;           // filament_type[0]
    std::string colour;         // default_filament_colour[0]
    bool        is_system { false };
    bool        is_compatible { true };
    bool        is_base { true }; // system, or a user preset with no parent (detached)
};

enum class MatchKind
{
    None,
    ExactName,  // the preset's name before " @" is the reported name
    VendorType, // same vendor and type, closest default colour
    Generic,    // the "Generic <type>" system preset - nothing of this vendor exists
};

struct Match
{
    int       index { -1 };
    MatchKind kind { MatchKind::None };
};

// The preset name before its first '@', trimmed ("iBoss PLA Silk @U1 0.4 nozzle" -> "iBoss PLA Silk").
std::string base_name(const std::string &preset_name);

// Case-insensitive equality of two names after trimming.
bool same_name(const std::string &a, const std::string &b);

// A vendor the printer reports for its own or untagged spools, which never gets a user preset of its own.
bool is_stock_vendor(const std::string &vendor);

// Rules, in order; only compatible candidates count:
// 1. ExactName - a system preset first, then a user preset (inheriting or not). A user's
//    "iBoss PLA Silk @U1 0.4 nozzle" is found for a tag that reads iBoss / PLA / Silk.
// 2. VendorType - closest default colour among presets of the reported vendor and type: system and
//    detached user presets (Orca #13265), and user presets that inherit a parent but carry the
//    reported vendor (a "Generic PLA" saved as "iBoss ..." with the vendor changed). A user preset
//    still on vendor "Generic" is not taken here, so it cannot shadow the stock Generic presets.
// 3. Generic - the system "Generic <type>" preset.
Match match_preset(const std::vector<Candidate> &candidates, const std::string &reported_name, const std::string &reported_type,
                   const std::string &reported_vendor = {}, const std::string &reported_colour = {});

// Name for the user preset a third-party spool gets when only rule 3 matched: the reported name,
// then the parent's " @..." suffix ("iBoss PLA Silk" + "Generic PLA @U1 0.4 nozzle" ->
// "iBoss PLA Silk @U1 0.4 nozzle"), so the dropdown's U1 naming finds it on the next sync. Characters
// a file name cannot hold are replaced with '-'. Empty when the reported name is empty.
std::string third_party_preset_name(const std::string &reported_name, const std::string &parent_name);

} // namespace FilamentSyncMatch
} // namespace Slic3r
