#pragma once

#include "libslic3r/FilamentColorLibrary.hpp"

#include <nlohmann/json.hpp>
#include <wx/bitmap.h>
#include <wx/colour.h>

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r
{

class DynamicPrintConfig;

namespace GUI
{
namespace FilamentColorUtils
{

std::string NormalizeHexColor(const std::string& color);
std::string NormalizeHexColor(const std::string& color, const std::string& fallback_color);

std::string StripHashForPreprint(const std::string& color);
std::string StripHashForPreprint(const std::string& color, const std::string& fallback_color);

std::vector<std::string> SplitMultiColors(const std::string& value);
std::string JoinMultiColors(const std::vector<std::string>& colors);

std::string GetPrimaryColor(const std::vector<std::string>& colors, const std::string& fallback_color);

std::string GetFilamentMatchName(const std::string& name);

FilamentColor GetFilamentColorFromConfig(const DynamicPrintConfig* config, size_t colorIndex, const std::string& fallbackColor);

nlohmann::json BuildPreprintColorMultiItem(const std::string& multiColors, FilamentColorMode mode,
                                           const std::string& fallbackColor);

wxBitmap* GetFilamentColorIcon(const std::vector<std::string>& colors, FilamentColorMode mode, const std::string& label,
                               int iconWidth, int iconHeight, const wxColour& lightBorderColor = wxNullColour);

// With `split` set, a Segment-mode multi-colour slot is drawn as its coextruded make-up (multi-colour
// P2): the proportions and rotation (angleDeg), two colours as a divided swatch, three and four as a pie.
wxBitmap* GetFilamentColorIcon(const std::string& multiColors, FilamentColorMode mode, const std::string& fallbackColor,
                               const std::string& label, int iconWidth, int iconHeight,
                               const wxColour& lightBorderColor = wxNullColour,
                               const std::string* split = nullptr, double angleDeg = 0.0);

// The split swatch itself: `colors` in strand order (#RRGGBB), `split` as in filament_colour_split.
wxBitmap* GetSplitFilamentColorIcon(const std::vector<std::string>& colors, const std::string& split, double angleDeg,
                                    const std::string& label, int iconWidth, int iconHeight,
                                    const wxColour& lightBorderColor = wxNullColour);

// The make-up of filament slot `slot` in a project config (the visual-only keys; defaults when missing).
struct ColourMakeUp
{
    std::string split;      // filament_colour_split
    double      angle = 0.; // filament_colour_angle, degrees
    double      twist = 0.; // filament_colour_twist, degrees per metre
    // Glow in the dark (G1), visual only.
    int         glow = -1;                // filament_glow: -1 follows the preset, 0 off, 1 on
    std::string glowColour;               // filament_glow_colour: "#RRGGBB", empty = a vibrant version of the filament's colour
    double      glowIntensity = 100.;     // filament_glow_intensity, percent; for the whole project, not this slot
    bool        glowSuggested = false;    // not stored: the preset looks like a glow filament (what -1 resolves to)
};
ColourMakeUp GetColourMakeUp(const DynamicPrintConfig* config, size_t slot);
// Writes the make-up of one slot into a project config (resizing the three keys to the slot count).
void SetColourMakeUp(DynamicPrintConfig& config, size_t slot, const ColourMakeUp& makeUp);

} // namespace FilamentColorUtils
} // namespace GUI
} // namespace Slic3r
