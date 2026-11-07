#ifndef slic3r_GUI_NozzleStrip_hpp_
#define slic3r_GUI_NozzleStrip_hpp_

// The Prepare sidebar's nozzle strip (one painted tile per physical nozzle) and the Process tab's nozzle chips
// (tests/design_bambu29_mixed_nozzle_compare.md section 4). Both are painted whole, size from the DIP constants below through
// FromDIP and the Label fonts, wrap onto another row instead of clipping or scrolling, and take their colours from
// StateColor::darkModeColorFor, so the light and dark themes and every DPI work alike. The tile layout and the hotend icon
// follow Bambu Studio's NozzleSlotTile (AGPL-3.0, commit 464f4b8db), at 52 DIP tiles with a 17 x 40 DIP icon (owner hand test 2026-11-04).

#include <functional>
#include <vector>

#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/font.h>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/window.h>

class wxStaticText;
class wxDC;
class ScalableButton;

namespace Slic3r { namespace GUI {

// Shared look of the strip, the chips and the filament badges.
namespace NozzleStyle {
wxColour tile_background();
wxColour text();
wxColour secondary_text();
wxColour accent();
wxColour divider();
wxColour error();
wxColour pill_background();
wxColour hover_background();
// `c` mixed into `bg` at `alpha` (0..1): the 30 % look of a tile that cannot print.
wxColour faded(const wxColour &c, const wxColour &bg, double alpha);
wxFont   label_font();      // filament badges: Body 9 bold
wxFont   tile_label_font(); // tile labels: Body 12 bold
wxFont   size_font();      // tile size line: Body 14
wxFont   flow_font();      // tile flow line: Body 12
wxFont   chip_font();      // chip label: Body 14 bold
wxFont   chip_pill_font(); // chip tools: Body 12
} // namespace NozzleStyle

// What one tile shows.
struct NozzleTileView
{
    wxString              label;      // "R3"
    wxString              size;       // "0.4", "-" when empty
    wxString              flow;       // "Std", "HF"
    std::vector<wxColour> dots;       // the colours of the filaments printing on it
    bool                  used{ false };
    bool                  usable{ true };
    bool                  divider_before{ false };
    wxString              tooltip;
};

// A row of tiles, wrapping onto more rows when the sidebar is narrow.
class NozzleTileRow : public wxWindow
{
public:
    explicit NozzleTileRow(wxWindow *parent);
    void set_tiles(const std::vector<NozzleTileView> &tiles);
    size_t tile_count() const { return m_tiles.size(); }
    // Screen rectangle of a tile (to anchor its menu).
    wxRect tile_rect(int index) const;
    // A tile was clicked (left or right button): its index.
    std::function<void(int)> on_click;
    // The row needs another height (the owner lays the sidebar out again).
    std::function<void()>    on_height_changed;
    void                     sys_color_changed();
    void                     msw_rescale();

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void   layout_tiles(int width);
    void   update_metrics();
    int    hit(const wxPoint &pt) const;
    void   on_paint(wxPaintEvent &evt);
    void   paint_tile(wxDC &dc, int index, const wxRect &r);
    void   on_size(wxSizeEvent &evt);
    void   on_motion(wxMouseEvent &evt);
    void   on_leave(wxMouseEvent &evt);
    void   on_click_event(wxMouseEvent &evt);
    void   load_icons();

    std::vector<NozzleTileView> m_tiles;
    std::vector<wxRect>         m_rects;
    std::vector<int>            m_dividers; // x of each divider line, with its row's y
    std::vector<int>            m_divider_y;
    int                         m_tile_w{ 0 };
    int                         m_tile_h{ 0 };
    int                         m_height{ 0 };
    int                         m_hover{ -1 };
    wxBitmap                    m_icon;
    wxBitmap                    m_icon_faded;
};

// The strip: a header ("Nozzles", sync, "Settings") above the tile row.
class NozzleStrip : public wxPanel
{
public:
    explicit NozzleStrip(wxWindow *parent);
    NozzleTileRow *tiles() { return m_row; }
    void           show_sync(bool show);
    std::function<void()> on_settings;
    std::function<void()> on_sync;
    void           sys_color_changed();
    void           msw_rescale();

private:
    wxStaticText   *m_title{ nullptr };
    wxStaticText   *m_settings{ nullptr };
    ScalableButton *m_sync{ nullptr };
    NozzleTileRow  *m_row{ nullptr };
};

// What one chip of the Process tab shows.
struct NozzleChipView
{
    wxString label;   // "Plate", "0.4 Std"
    wxString tools;   // "R1-R5" (empty for Plate)
    bool     edited{ false };
    bool     missing{ false };
    wxString tooltip;
};

// [Plate] [0.4 Std L] [0.4 Std R1-R5] [0.6 Std R6]: one chip per selector entry, wrapping onto a second row.
class NozzleChipRow : public wxWindow
{
public:
    explicit NozzleChipRow(wxWindow *parent);
    void set_chips(const std::vector<NozzleChipView> &chips, int selected);
    void set_selected(int index);
    int  selected() const { return m_selected; }
    const std::vector<NozzleChipView> &chips() const { return m_chips; }
    std::function<void(int)> on_pick;
    std::function<void()>    on_height_changed;
    void                     sys_color_changed() { Refresh(); }
    void                     msw_rescale();

protected:
    wxSize DoGetBestClientSize() const override;

private:
    void layout_chips(int width);
    int  hit(const wxPoint &pt) const;
    void on_paint(wxPaintEvent &evt);
    void on_size(wxSizeEvent &evt);
    void on_motion(wxMouseEvent &evt);
    void on_leave(wxMouseEvent &evt);
    void on_click_event(wxMouseEvent &evt);
    int  chip_width(int index) const;

    std::vector<NozzleChipView> m_chips;
    std::vector<wxRect>         m_rects;
    int                         m_selected{ 0 };
    int                         m_hover{ -1 };
    int                         m_height{ 0 };
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_NozzleStrip_hpp_
