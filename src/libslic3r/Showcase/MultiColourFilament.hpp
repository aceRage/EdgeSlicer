#ifndef slic3r_Showcase_MultiColourFilament_hpp_
#define slic3r_Showcase_MultiColourFilament_hpp_

// Multi-colour (coextruded) filaments: dual-, tri- and quad-colour "silk dual colour" strands, two
// to four colours side by side round the strand like slices of a pie.
// tests/design_multicolour_filament_view.md (2. The model, 4.1 the keys), phase P1.
//
// The reference model every view uses (Showcase toolpath and mesh modes, the Preview toggle, the
// editor's wall preview). Pure, GL- and wxWidgets-free, unit-tested ([MultiColour]); the GLSL of
// P3 is generated from the same constants and checked against bead_colour().
//
// Everything here is visual only. The keys it reads (filament_colour_split / _angle / _twist) are
// in visual_only_option_keys(): Print::apply() strips them, so they never reach the slicer.
//
// Conventions:
//  * Angles in the printer's XY plane are in turns (1 turn = 360 degrees), measured from the
//    printer's front (-Y), anticlockwise seen from above (+Z): front 0, right 0.25, back 0.5,
//    left 0.75 (decision D3).
//  * The filament's own frame: slice i (colour i) covers [boundary(i), boundary(i + 1)) turns,
//    anticlockwise round the strand, boundary(0) = 0, boundary(n) = 1.
//  * rotation_deg says where the middle of colour 1 points in the printer's frame when no twist
//    has built up: 0 = colour 1 faces the front (D3).
//  * twist_deg_per_m turns the strand anticlockwise (positive) per metre of this slot's filament
//    extruded so far (the dragon reference: rotation builds up over metres, not per layer).
//  * Colours are blended and returned in linear RGB (what the shaders light with).

#include <array>
#include <string>

namespace Slic3r {

class ConfigBase;

namespace MultiColour {

constexpr int MAX_COLOURS = 4; // D10

// Boundary blend width, in turns of the strand (about 15 degrees). P0 found the wall transition
// soft and wide; P3 retunes it (a constant, not a setting) and never lets it exceed the narrowest
// slice.
constexpr float BLEND_TURNS = 0.04f;
// Stack-up strength k: how far the leading / trailing material tilts the colour from the bottom to
// the top of a bead. 1 = hypothesis H1 (trailing material on top), confirmed by the P0 prints (D4).
constexpr float STACK_UP_K = 1.f;
// Editor limits (D10): proportions in 5% steps, at least 5% each.
constexpr int MIN_PERCENT  = 5;
constexpr int STEP_PERCENT = 5;

// P3 (Showcase toolpaths), tuned against the P0 prints (tests/p0_multicolour_cc1/NOTES.md):
//  * walls blend generously: the vase and the 30 mm cylinder show a soft, wide transition round
//    the circumference (about 36 degrees of blend);
//  * tops blend narrowly: the 45 degree plaque shows crisp stripes line by line (about 11 degrees);
//  * a wall bead shows only the middle of its outside: the groove hides its top and bottom under
//    the neighbouring layers. So on walls the stack-up reaches only half way (the 20 mm cube's
//    faces read as one colour each, P0 photo 09); k itself stays 1 (D4).
constexpr float BLEND_TURNS_WALL    = 0.10f;
constexpr float BLEND_TURNS_TOP     = 0.03f;
constexpr float WALL_VISIBLE_HEIGHT = 0.5f;
//  * a top bead is flat across most of its width, while the viewer draws it as a diamond whose
//    height falls linearly to the edges. Seen from above, s.y is therefore lifted to the flat top,
//    1 - (1 - |s.y|)^TOP_FLATTEN, so the trailing slice covers the bead and the side slices show
//    only at its rounded edges: the 0 degree monotonic plaque reads one colour with a thin edge of
//    the other (P0 photo 07), not as stripes.
constexpr float TOP_FLATTEN         = 4.f;
// Ironing re-melts the top a little: its beads are pulled this far towards the slot's average.
constexpr float IRONING_TO_AVERAGE  = 0.3f;

// How one kind of bead maps its cross-section to the strand (P3).
struct BeadParams
{
    float blend_turns    = BLEND_TURNS;  // the boundary blend, never more than the narrowest slice
    float stack_up_k     = STACK_UP_K;
    float visible_height = 1.f;          // s.y is scaled by this before the stack-up
    float flatten        = 1.f;          // s.y -> 1 - (1 - |s.y|)^flatten (sign kept); 1 = as drawn
};
// Walls (inner, outer and overhang perimeters) or every other bead (tops, infill, supports...).
BeadParams bead_params(bool wall);

using RGB = std::array<float, 3>;

// sRGB (0..1) <-> linear RGB, per channel.
float srgb_to_linear(float c);
float linear_to_srgb(float c);
RGB   srgb_to_linear(const RGB &c);
RGB   linear_to_srgb(const RGB &c);
// "#RRGGBB" (or "#RRGGBBAA", alpha ignored) -> sRGB 0..1. False for anything else.
bool  parse_hex(const std::string &hex, RGB &out_srgb);

// One filament slot's cross-section.
struct Filament
{
    int                                 n = 1;                     // colours, 1..MAX_COLOURS
    std::array<RGB, MAX_COLOURS>        colour{};                  // linear RGB, in strand order
    std::array<float, MAX_COLOURS>      proportion{ 1.f, 0.f, 0.f, 0.f }; // sums to 1 over [0, n)
    float                               rotation_deg    = 0.f;
    float                               twist_deg_per_m = 0.f;
    // P6 (gradient filaments along the length) adds its palette-cycle length here, read from its
    // own visual-only key; the cumulative filament length bead_colour() already takes is its input.

    bool  is_multi() const { return n > 1; }
    // Cumulative start of slice i in turns (boundary(0) = 0, boundary(n) = 1).
    float boundary(int i) const;
    // The blend width actually used: BLEND_TURNS, never more than the narrowest slice.
    float blend_turns() const;
    // Area-weighted average colour (linear): what a surface shows when no stripe can be resolved.
    RGB   average() const;

    // The colour at angle phi (turns) in the filament's own frame, any real value (wrapped).
    RGB   colour_at(float phi) const;

    // The angle (turns, in the filament's frame) of the strand material that faces printer
    // direction dir_turns (printer frame), after filament_length_m metres of this slot's filament.
    float filament_angle(float dir_turns, float filament_length_m) const;

    // The colour of a point on a bead (design 2.2).
    //  sx in [-1, 1]: across the bead, +1 on the right of travel; sy in [-1, 1]: bottom to top.
    //  rx, ry: the bead's right (horizontal unit vector); dx, dy: the travel direction (unit).
    //  filament_length_m: metres of this slot's filament extruded up to this point (twist).
    RGB   bead_colour(float sx, float sy, float rx, float ry, float dx, float dy, float filament_length_m = 0.f) const;

    // P3: the same with a blend width of its own (still capped by the narrowest slice).
    RGB   colour_at(float phi, float blend) const;
    // P3: the same with a kind of bead's parameters. The default BeadParams give bead_colour() above.
    RGB   bead_colour(float sx, float sy, float rx, float ry, float dx, float dy, float filament_length_m,
                      const BeadParams &bead) const;
};

// Printer-frame direction of the XY vector (x, y), in turns from the front, anticlockwise.
float direction_turns(float x, float y);

// The proportions of a filament_colour_split value for n colours, normalised to sum 1. An empty
// value, a wrong count, or a value that is not a positive number gives the equal split.
std::array<float, MAX_COLOURS> parse_split(const std::string &value, int n);
// The filament_colour_split value for proportions (whole percent, '|'-separated); "" for the
// equal split or a single colour, so a default slot stays default.
std::string format_split(const std::array<float, MAX_COLOURS> &proportion, int n);
// Snap proportions to the editor's limits (D10): STEP_PERCENT steps, at least MIN_PERCENT each,
// summing to 100. n * MIN_PERCENT <= 100 for every n <= MAX_COLOURS.
std::array<int, MAX_COLOURS> quantize_percent(const std::array<float, MAX_COLOURS> &proportion, int n);

// Slot `slot` of a project / full config: filament_multi_colors + filament_colour_mode (+ the
// filament_colour fallback) and the visual-only keys. Missing keys and short vectors take their
// defaults (equal split, angle 0, twist 0). Gradient slots (filament_colour_mode = 1) and slots
// with one colour come back as a single colour (n = 1) in v1 (design 2.3; gradients are P6).
Filament from_config(const ConfigBase &config, size_t slot);

} // namespace MultiColour
} // namespace Slic3r

#endif // slic3r_Showcase_MultiColourFilament_hpp_
