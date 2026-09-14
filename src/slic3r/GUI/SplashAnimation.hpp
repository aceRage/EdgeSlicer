#ifndef slic3r_GUI_SplashAnimation_hpp_
#define slic3r_GUI_SplashAnimation_hpp_

// Geometry and timing for the startup splash's background animation: a small object being built
// up layer by layer behind the logo. Kept in its own header, free of any wx dependency, for two
// reasons: a test can check the shape without standing up a GUI, and the preview script that
// renders the GIF draws from exactly these numbers rather than a second, drifting copy of them.
//
// The drawing itself (wxGraphicsContext paths, colours, stroke widths) stays in GUI_App.cpp next
// to the splash; only the numbers that define the shape and the timeline live here.

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace SplashAnim {

// One full loop of the build.
static const int   k_loop_ms    = 2500;
// The design space the geometry below is expressed in; the splash scales it to its real size.
static const int   k_design_w   = 420;
static const int   k_design_h   = 300;
// The object's bounding box in that design space. The card's middle column is taken by the logo
// (y 36..156), the wordmark (y 168) and the version (y ~198), so the object is drawn out on the
// left flank instead, where it has the full height of the card to be built in and never fights
// the text for the same pixels. Tall rather than wide, so the layers have somewhere to stack.
static const float k_obj_left   = 26.0f;
static const float k_obj_right  = 142.0f;
static const float k_obj_bottom = 240.0f;
static const float k_obj_top    = 148.0f;
// Layer height in design units: enough layers to read as "layers", few enough to stay calm.
static const float k_layer_h    = 3.4f;
// The build finishes at this point in the loop; the rest is a hold on the finished object.
static const float k_build_end  = 0.82f;

// Half-width of the solid at height fraction t (0 at the bottom of the object, 1 at the top).
// A hull that widens to a deck, then a narrower cabin above it: a boat seen from the side, in the
// spirit of a benchy without being one.
inline float half_width_at(float t)
{
    const float max_half = (k_obj_right - k_obj_left) * 0.5f;
    if (t < 0.50f) {
        // Hull: a narrow keel flaring quickly out to the full beam at the gunwale.
        const float u = t / 0.50f;
        return max_half * (0.55f + 0.45f * std::sqrt(u));
    }
    if (t < 0.62f) {
        // Deck: the full beam, held long enough to read as a deck rather than a corner.
        return max_half;
    }
    // Cabin: a hard step in to a third of the beam, offset nowhere -- a blocky wheelhouse -- then
    // a slight taper to the roof. The step is what makes this read as a boat rather than a pot.
    const float u = (t - 0.62f) / 0.38f;
    return max_half * (0.34f - 0.06f * u);
}

// How much of the object is built at this point in the loop. phase is in [0,1).
inline float build_at(float phase) { return std::min(1.0f, std::max(0.0f, phase) / k_build_end); }

// Number of layers the object is sliced into.
inline int layer_count() { return std::max(1, int((k_obj_bottom - k_obj_top) / k_layer_h)); }

}}} // namespace Slic3r::GUI::SplashAnim

#endif // slic3r_GUI_SplashAnimation_hpp_
