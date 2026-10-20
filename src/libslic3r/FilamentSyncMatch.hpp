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

} // namespace FilamentSyncMatch
} // namespace Slic3r
