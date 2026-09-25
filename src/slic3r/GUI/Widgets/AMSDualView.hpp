#ifndef slic3r_GUI_AMSDualView_hpp_
#define slic3r_GUI_AMSDualView_hpp_

// The Device tab's filament area for two-extruder Bambu printers (H2D, H2D Pro, H2C, X2D), laid
// out like Bambu Studio's: one side per extruder (deputy on the left, main on the right), each with
// its unit selector strip, humidity pill, slot tiles and feed lines that run down to the matching
// extruder at the bottom centre, the loaded path drawn in the filament's colour.
//
// Everything is painted in one window (light and dark palettes, DIP-scaled); the Auto-refill,
// settings, Unload and Load controls are real child widgets placed in the bottom band. The
// grouping, paging and labels come from AmsDualLayout.hpp, which the unit tests cover.

#include "../wxExtensions.hpp"
#include "../AmsDualLayout.hpp"
#include "AMSItem.hpp"
#include "Button.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

struct DualSlotView
{
    std::string           slot_id;
    wxString              material;              // "" with a THIRDBRAND state = unknown ("?")
    wxColour              colour{*wxWHITE};
    std::vector<wxColour> cols;                  // multi-colour spools
    AMSCanType            state{AMSCanType::AMS_CAN_TYPE_EMPTY};
    bool                  editable{true};        // pencil shown
};

struct DualUnitView
{
    AmsDual::UnitRef          ref;
    std::vector<DualSlotView> slots;
    int                       humidity_level{-1};   // 1..5, -1 = none
    int                       humidity_percent{-1}; // -1 = level only
    bool                      has_heater{false};    // draws the drying sun
    bool                      drying{false};        // sun drawn "active"
    bool                      rfid_refresh{false};  // slot label click re-reads the tag
};

struct DualExtruderView
{
    int         id{0};
    bool        active{false};   // the extruder printing / selected right now (current_extder_id)
    bool        filled{false};   // filament at the extruder (ext_has_filament)
    bool        loaded{false};
    std::string ams_id;
    std::string slot_id;
    wxColour    colour{*wxWHITE};
};

struct DualModel
{
    int                           extruder_count{2};
    std::vector<DualUnitView>     units;
    std::vector<DualExtruderView> extruders;
};

class AMSDualView : public wxWindow
{
public:
    AMSDualView(wxWindow *parent, wxWindowID id = wxID_ANY);

    void SetModel(const DualModel &model);

    // Current slot selection (what Load/Unload act on).
    std::string SelectedAms() const { return m_sel_ams; }
    std::string SelectedSlot() const { return m_sel_slot; }
    void        Select(const std::string &ams_id, const std::string &slot_id);
    // The unit whose page is shown on the side of the current selection (AMS settings target).
    std::string ShownAms() const;
    wxColour    SlotColour(const std::string &ams_id, const std::string &slot_id) const;

    // Callbacks into AMSControl.
    std::function<void()>                                     on_selection_changed;
    std::function<void(const std::string &, const std::string &)> on_edit;      // ams_id, slot_id
    std::function<void(const std::string &, const std::string &)> on_refresh;   // ams_id, slot_id
    std::function<void(const std::string &, wxPoint)>             on_humidity;  // ams_id, screen pos

    // Bottom-band controls, created here so they sit beside the extruders.
    Button         *m_btn_auto_refill{nullptr};
    wxStaticBitmap *m_btn_settings{nullptr};
    Button         *m_btn_unload{nullptr};
    Button         *m_btn_load{nullptr};

    void ShowAutoRefill(bool show);
    // Draw the painted part (not the child buttons) into any DC; used by the test preview.
    void PaintTo(wxDC &dc) { render(dc); }
    void msw_rescale();
    bool Enable(bool enable = true) override;

private:
    enum class HitKind { UnitIcon, Tile, Pencil, Label, Humidity };
    struct Hit
    {
        wxRect      rect;
        HitKind     kind;
        int         side;
        int         unit;  // index into side.units
        int         slot;  // slot index within the unit
    };

    struct Palette
    {
        wxColour page, strip, body, text, text_dim, line, icon_border, icon_bg, tile_empty, pill, brand, active_underlay;
    };

    DualModel                   m_model;
    std::vector<AmsDual::Side>  m_sides;
    std::vector<int>            m_page;          // current page per side
    std::vector<std::string>    m_page_anchor;   // ams id that pins each side's page across updates
    std::string                 m_sel_ams;
    std::string                 m_sel_slot;
    std::vector<Hit>            m_hits;
    bool                        m_enabled{true};

    ScalableBitmap m_pencil_dark;   // for light tiles
    ScalableBitmap m_pencil_light;  // for dark tiles
    ScalableBitmap m_sun_idle;
    ScalableBitmap m_sun_drying;
    std::vector<ScalableBitmap> m_hum_num_light, m_hum_num_dark, m_hum_light, m_hum_dark;
    ScalableBitmap m_settings_normal, m_settings_hover;
    // Bambu Studio artwork (resources/images, from BambuStudio under AGPL-3.0).
    ScalableBitmap m_four_slot, m_four_slot_dark, m_single_slot, m_single_slot_dark, m_ts_cube, m_ts_cube_dark;
    ScalableBitmap m_ext_left[2][2], m_ext_right[2][2]; // [active][filled]

    int  extruder_inlet_x(int extruder_id) const; // where a side's feed line enters its extruder
    wxSize unit_icon_size(const AmsDual::UnitRef &ref) const;

    Palette palette() const;
    const DualUnitView *unit_view(const std::string &ams_id) const;
    const DualExtruderView *extruder_view(int id) const;
    int  side_x0(int side) const;
    int  side_width() const;
    void layout_controls();
    void load_bitmaps();

    void on_paint(wxPaintEvent &evt);
    void on_left_down(wxMouseEvent &evt);
    void on_motion(wxMouseEvent &evt);
    void render(wxDC &dc);
    void draw_side(wxDC &dc, int side, const Palette &pal);
    void draw_unit_icon(wxDC &dc, const wxRect &r, const DualUnitView *unit, bool selected, const Palette &pal);
    void draw_tile(wxDC &dc, const wxRect &r, const DualSlotView &slot, bool selected, const Palette &pal);
    void draw_humidity(wxDC &dc, int side, int cx, int y, const DualUnitView &unit, const Palette &pal);
    void draw_extruders(wxDC &dc, const Palette &pal);
};

}} // namespace Slic3r::GUI

#endif
