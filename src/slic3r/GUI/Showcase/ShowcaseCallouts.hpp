#ifndef slic3r_GUI_ShowcaseCallouts_hpp_
#define slic3r_GUI_ShowcaseCallouts_hpp_

// Showcase tab, phase S3: filament callouts as the GUI draws them. One CPU rasteriser
// (wxGraphicsContext, D6) draws them for the live view (a frame-sized overlay picture) and onto every
// export at its own size, after the supersampled render has been scaled down, so the labels are sharp
// at any resolution and the screen and the file match. The layout and the label text come from
// libslic3r (Showcase/ShowcaseCallouts.hpp); this file adds what only the GUI knows: the project's
// filaments (presets, colours, the colour library, slice weights and prices), the drawing, and the
// dialog that edits one callout.

#include "ShowcaseRenderer.hpp"   // ShowcaseImage

#include "libslic3r/FilamentColorLibrary.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include <map>
#include <string>
#include <vector>

#include <wx/dialog.h>

class wxCheckBox;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

// One callout as it is drawn now.
struct ShowcaseCalloutDraw
{
    int                           item = -1;    // index into Scene::callouts.items
    Showcase::CalloutLabel        label;
    std::vector<Showcase::PointD> anchors;      // 0..1 of the frame, origin top-left; the first is the main one
    std::vector<bool>             occluded;     // per anchor: something is in front of it
    bool                          pinned = false;
    Showcase::PointD              pinned_pos;   // 0..1 of the frame, the label's top-left
    // S4, video: per anchor, how visible its leader and dot are (Showcase::CalloutFade), and the
    // label's own opacity. Empty: no fade, an occluded anchor gets a dashed leader (stills).
    std::vector<double>           fade;
    double                        alpha = 1.;
    // S5: a path's timeline (Showcase::CalloutVisual): how much it shows the callout (0..1, on top of
    // the fade) and how much it highlights it (an accent ring round the label and its dots).
    double                        visual    = 1.;
    double                        highlight = 0.;
};

struct ShowcaseCalloutOptions
{
    bool            overlay  = false;   // the live view: the selected callout is outlined
    int             selected = -1;      // item index
    Showcase::RectI reserved;           // kept free of labels (the watermark), image pixels
    double          model_center_x = -1.;   // 0..1 of the frame; < 0 = the middle of the anchors
    // S4: where the model is, 0..1 of the frame (empty = unknown): a wide, flat model gets its labels
    // in bands above and below it.
    Showcase::RectD model_box;
    // S4, video: each callout's side in the previous frame by item index (in), and in this one (out),
    // so a label only changes side once its anchor is clearly across (side_hysteresis, 0..1 of the
    // frame width).
    const std::map<int, int> *previous_sides = nullptr;
    std::map<int, int>       *sides          = nullptr;
    double                    side_hysteresis = 0.;
};

// Where things were drawn, image pixels (the live view's mouse uses them).
struct ShowcaseCalloutHits
{
    struct Item
    {
        int                           item = -1;
        Showcase::RectD               box;
        std::vector<Showcase::PointD> dots;   // the anchors' dots (or the legend's numbers)
    };
    int               width  = 0;
    int               height = 0;
    bool              legend = false;
    std::vector<Item> items;
};

// Draws the callouts onto `image`. `has_alpha`: keep the image's alpha (the live overlay starts fully
// transparent, a transparent export keeps its own); otherwise the image is opaque and the text is
// smoothed against the real picture.
void showcase_draw_callouts(ShowcaseImage &image, bool has_alpha, const Showcase::Scene &scene,
                            const std::vector<ShowcaseCalloutDraw> &callouts, const ShowcaseCalloutOptions &options,
                            ShowcaseCalloutHits *hits = nullptr);

// What the project's filaments look like now, for the labels.
class ShowcaseLabelContext
{
public:
    // `plate_index`: the plate whose slice gives the weights and prices (`with_usage`; otherwise
    // they stay unknown and the slice result is not read).
    static ShowcaseLabelContext current(int plate_index, bool with_usage);

    Showcase::LabelFilament   filament(const Showcase::Scene &scene, const Showcase::Callout &callout) const;
    // The Snapmaker colour library's name for the callout's colour (D8), empty when it has none.
    std::string               library_name(const Showcase::Callout &callout) const;
    const Showcase::LabelWords &words() const { return m_words; }
    // Changes whenever a label could read differently.
    std::string               signature() const;

private:
    struct Slot
    {
        std::string              preset_name;
        std::string              vendor;
        std::string              type;
        std::vector<std::string> colors;
        int                      family = -1;   // index into m_library, -1 = not a library filament
    };
    std::vector<std::string> colors_of(const Showcase::Callout &callout) const;

    std::vector<Slot>              m_slots;      // physical filaments, slot - 1
    std::map<int, double>          m_weight_g;   // slot -> grams, from the slice
    std::map<int, double>          m_price;      // slot -> money, from the slice and your prices
    std::vector<FilamentColorInfo> m_library;
    std::string                    m_lang;
    Showcase::LabelWords           m_words;
};

// The dialog that edits one callout: the first line, the colour's name, a note, hidden, pinned.
class ShowcaseCalloutDialog : public wxDialog
{
public:
    // `auto_title` and `auto_color` are what the label says without the user's text; `library_name`
    // is the colour library's name (the colour field is then the library's, D8).
    ShowcaseCalloutDialog(wxWindow *parent, const Showcase::Callout &callout, const wxString &heading, const std::string &auto_title,
                          const std::string &auto_color, const std::string &library_name);
    Showcase::Callout result() const;

private:
    Showcase::Callout m_callout;
    wxTextCtrl       *m_title  = nullptr;
    wxTextCtrl       *m_color  = nullptr;
    wxTextCtrl       *m_note   = nullptr;
    wxCheckBox       *m_hidden = nullptr;
    wxCheckBox       *m_pinned = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseCallouts_hpp_
