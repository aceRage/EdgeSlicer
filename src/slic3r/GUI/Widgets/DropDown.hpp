#ifndef slic3r_GUI_DropDown_hpp_
#define slic3r_GUI_DropDown_hpp_

#include <boost/date_time/posix_time/posix_time.hpp>
#include <wx/stattext.h>
#include "../wxExtensions.hpp"
#include "StateHandler.hpp"
#include "PopupWindow.hpp"
#include <utility>

#define DD_NO_CHECK_ICON    0x0001
#define DD_NO_TEXT          0x0002
#define DD_STYLE_MASK       0x0003

wxDECLARE_EVENT(EVT_DISMISS, wxCommandEvent);
// A click on an item's action icon (the column at the right of the rows, e.g. a favourite star).
// GetInt() is the item's index. The list stays open and the item is not selected.
wxDECLARE_EVENT(EVT_DROPDOWN_ITEM_ACTION, wxCommandEvent);

class DropDown : public PopupWindow
{
    std::vector<wxString> &       texts;
    std::vector<wxString> &       tips;
    std::vector<wxBitmap> &     icons;
    // Optional, owned by the ComboBox: per-item action icons and disabled (header) flags.
    const std::vector<wxBitmap> *action_icons = nullptr;
    const std::vector<char> *     disabled     = nullptr;
    bool                          need_sync  = false;
    int                         selection = -1;
    int                         hover_item = -1;

    double radius = 0;
    bool   use_content_width = false;
    bool   limit_max_content_width = false;
    bool   align_icon        = false;
    bool   text_off          = false;

    // Type-to-filter (opt-in, see EnableTypeToFilter). While the list is open, printable keys build
    // filter_text and the list shows only the matching rows. visible_items maps a shown row to the real
    // item index, so selection, hover and the ComboBox's per-item data keep using real indices.
    bool             filter_enabled = false;
    wxString         filter_text;
    std::vector<int> visible_items;
    bool             kb_hover = false; // hover_item was set from the keyboard (drawn highlighted)
    wxPoint          last_motion_screen{-1, -1};

    wxSize textSize;
    wxSize iconSize;
    int    actionIconWidth = 0;
    wxSize rowSize{100, 30};  // Initialize with default values

    StateHandler state_handler;
    StateColor   text_color;
    StateColor   border_color;
    StateColor   selector_border_color;
    StateColor   selector_background_color;
    ScalableBitmap check_bitmap;

    bool pressedDown = false;
    boost::posix_time::ptime dismissTime;
    wxPoint                  offset; // x not used
    wxPoint                  dragStart;

public:
    DropDown(std::vector<wxString> &texts,
             std::vector<wxString> &tips,
             std::vector<wxBitmap> &icons);
    
    DropDown(wxWindow *     parent,
             std::vector<wxString> &texts,
             std::vector<wxString> &tips,
             std::vector<wxBitmap> &icons,
             long           style     = 0);
    
    void Create(wxWindow *     parent,
             long           style     = 0);
    
public:
    void Invalidate(bool clear = false);

    int GetSelection() const { return selection; }

    void SetSelection(int n);

    wxString GetValue() const;
    void     SetValue(const wxString &value);

public:
    void SetCornerRadius(double radius);

    void SetBorderColor(StateColor const & color);

    void SetSelectorBorderColor(StateColor const & color);

    void SetTextColor(StateColor const &color);

    void SetSelectorBackgroundColor(StateColor const &color);

    void SetUseContentWidth(bool use, bool limit_max_content_width = false);

    void SetAlignIcon(bool align);

    // Opt-in (off by default): typing while the list is open narrows it to the rows whose text
    // contains the typed text (case-insensitive). See DropDown::HandleSpecialKey / HandlePrintableKey.
    void EnableTypeToFilter(bool on);
    bool IsTypeToFilterEnabled() const { return filter_enabled; }
    const wxString &GetFilterText() const { return filter_text; }
    // Esc, Backspace, Up, Down and Enter while the list is open. Returns true when consumed.
    bool HandleSpecialKey(wxKeyEvent &event);
    // A printable key (extends the filter). Returns true when consumed.
    bool HandlePrintableKey(wxKeyEvent &event);
    static bool IsPrintableKey(const wxKeyEvent &event);

    void SetItemExtras(const std::vector<wxBitmap> *action_icons, const std::vector<char> *disabled)
    {
        this->action_icons = action_icons;
        this->disabled     = disabled;
    }
    bool IsItemDisabled(int n) const { return disabled && n >= 0 && n < int(disabled->size()) && (*disabled)[n]; }
    
public:
    void Rescale();

    bool HasDismissLongTime();
    
protected:
    bool ProcessLeftDown(wxMouseEvent& event) override;
    void OnDismiss() override;

private:
    void paintEvent(wxPaintEvent& evt);
    void paintNow();

    void render(wxDC& dc);
    // Rows currently shown (all items, or the filter matches) and the filter strip above them.
    int  headerH() const;
    int  rowCount() const { return filter_text.empty() ? int(texts.size()) : int(visible_items.size()); }
    int  itemAt(int row) const;
    int  rowOf(int item) const;
    int  itemAtY(int y) const;
    int  popupHeight() const;
    void clampOffsetY(int &y) const;
    void applyFilter();
    void relayoutForFilter();
    void resetFilterState();
    void moveHighlight(int step);
    const wxBitmap *actionIcon(int n) const;
    std::pair<int, int> actionColumn() const;

    friend class ComboBox;
    void messureSize();
    void autoPosition();
    bool PointInAnchorGap(const wxPoint& screen_point) const;

    // some useful events
    void mouseDown(wxMouseEvent& event);
    void mouseReleased(wxMouseEvent &event);
    void mouseCaptureLost(wxMouseCaptureLostEvent &event);
    void mouseMove(wxMouseEvent &event);
    void mouseWheelMoved(wxMouseEvent &event);
    void charEvent(wxKeyEvent &event);

    void sendDropDownEvent();
    void sendDropDownActionEvent(int index);


    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_DropDown_hpp_
