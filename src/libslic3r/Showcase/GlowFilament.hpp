#ifndef slic3r_Showcase_GlowFilament_hpp_
#define slic3r_Showcase_GlowFilament_hpp_

// Glow-in-the-dark filaments, phase G1 (tests/design_multicolour_filament_view.md section 10).
//
// A glow filament prints as its daylight colour (filament_colour) and shines in the dark. The
// "lights off" views of Showcase and Preview show it: glow slots emit their glow colour, ordinary
// filaments go dim (not black). Everything here is visual only. The keys it reads (filament_glow,
// filament_glow_colour, filament_glow_intensity) are in visual_only_option_keys(): Print::apply()
// strips them, so they never reach the slicer or the G-code.
//
// Owner answers (2026-10-27):
//  * glow is a per-slot setting, on by default for presets whose name or type contains "Glow";
//  * the default glow colour is a more vibrant version of the filament's own colour (same hue,
//    higher saturation and value), which the user can override;
//  * no fade over time; one global glow intensity.
//
// Pure and GL-free, unit-tested ([Glow]). The GLSL glow_vibrant() in showcase_material.fs repeats
// vibrant() with the same constants.

#include "MultiColourFilament.hpp"
#include "MultiColourGlsl.hpp"

#include <array>
#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class PresetBundle;

namespace Glow {

using MultiColour::RGB;

// vibrant(): saturation moves this far (0..1) from where it is towards 1, and value this far towards 1.
constexpr float SAT_LIFT  = 0.5f;
constexpr float VAL_LIFT  = 0.8f;
// A colour with less saturation than this has no hue worth keeping (white, grey, black): it glows in
// the classic phosphor green.
constexpr float GREY_SAT  = 0.08f;
constexpr RGB   GREY_GLOW{ 124.f / 255.f, 1.f, 107.f / 255.f };   // #7CFF6B, sRGB

// Project-wide glow intensity, percent (filament_glow_intensity): 100 = the glow colour at full strength.
constexpr float INTENSITY_DEFAULT_PERCENT = 100.f;
constexpr float INTENSITY_MAX_PERCENT     = 300.f;

// filament_glow: follow the preset (-1, the default), off (0), on (1).
constexpr int GLOW_AUTO = -1;
constexpr int GLOW_OFF  = 0;
constexpr int GLOW_ON   = 1;

// Whether a preset looks like a glow filament: its name or its filament type contains "glow" (any case).
bool suggests_glow(const std::string &preset_name, const std::string &filament_type);
// The state filament_glow resolves to: GLOW_ON / GLOW_OFF as set, GLOW_AUTO from the preset.
bool resolve_glow(int state, const std::string &preset_name, const std::string &filament_type);

// The more vibrant version of an sRGB colour (0..1 per channel): the same hue, saturation s + (1 - s) *
// SAT_LIFT, value v + (1 - v) * VAL_LIFT. Greys (saturation below GREY_SAT) give GREY_GLOW.
RGB         vibrant(const RGB &srgb);
// The same for "#RRGGBB"; false (and `out` untouched) when `hex` is not a colour.
bool        vibrant_hex(const std::string &hex, std::string &out);

// One filament slot.
struct Slot
{
    bool        on = false;
    // filament_glow_colour when it is a valid #RRGGBB; empty = derived from the filament's own colour.
    std::string colour;

    // The colour this slot glows in, sRGB, given its daylight colour (#RRGGBB).
    RGB         glow_srgb(const RGB &daylight_srgb) const;
};

// Slot `slot` of a project config with its preset's name and type (for the auto rule).
Slot  slot_from_config(const ConfigBase &config, size_t slot, const std::string &preset_name, const std::string &filament_type);
// The project-wide intensity as a multiplier (1 = 100%); missing or invalid = 1, clamped to [0, 3].
float intensity_from_config(const ConfigBase &config);
// Every filament slot of the bundle (its project config and filament presets).
std::vector<Slot> slots_from_bundle(const PresetBundle &bundle);
// A string that changes whenever a value the slots are made from changes (the keys, the preset names).
std::string signature(const PresetBundle &bundle);
// Some of `used` (0-based slot numbers) glows.
bool  any_glow(const std::vector<Slot> &slots, const std::vector<unsigned char> &used);

// ---- the shaders' table ----
// glow[slot * 4]: 0 off, 1 the slot's own (vibrant) colour, 2 an explicit colour in glow[slot * 4 + 1..3]
// (linear RGB). The lights-off views light a glow slot with these; the intensity is a separate uniform.
struct UniformTable
{
    std::array<float, MultiColour::GLSL_SLOTS * 4> slot{};
    float                                          intensity = 1.f;
    bool                                           any = false;
};
UniformTable pack_uniforms(const std::vector<Slot> &slots, float intensity);

// ---- Preview: no shader of its own, so the lights-off colours are made on the CPU ----
// A plain slot colour (#RRGGBB) as it looks with the lights off: the glow colour of a glow slot, scaled by
// the intensity, else the colour dimmed to `ambient`. Returns the input when it is not a colour.
std::string lights_off_hex(const std::string &hex, const Slot &slot, float intensity, float ambient);
// The multi-colour table of the Preview toggle (P5) with the same change applied to every colour of every slot.
void        apply_lights_off(MultiColour::UniformTable &table, const std::vector<Slot> &slots, float intensity, float ambient);

} // namespace Glow
} // namespace Slic3r

#endif // slic3r_Showcase_GlowFilament_hpp_
