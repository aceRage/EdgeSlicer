#include "DropDown.hpp"
#include "Label.hpp"
#include "../I18N.hpp"

#include <algorithm>

#include <wx/display.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

wxDEFINE_EVENT(EVT_DISMISS, wxCommandEvent);
wxDEFINE_EVENT(EVT_DROPDOWN_ITEM_ACTION, wxCommandEvent);

BEGIN_EVENT_TABLE(DropDown, PopupWindow)

EVT_LEFT_DOWN(DropDown::mouseDown)
EVT_LEFT_UP(DropDown::mouseReleased)
EVT_MOUSE_CAPTURE_LOST(DropDown::mouseCaptureLost)
EVT_MOTION(DropDown::mouseMove)
EVT_MOUSEWHEEL(DropDown::mouseWheelMoved)

// catch paint events
EVT_PAINT(DropDown::paintEvent)

END_EVENT_TABLE()

/*
 * Called by the system of by wxWidgets when the panel needs
 * to be redrawn. You can also trigger this call by
 * calling Refresh()/Update().
 */

DropDown::DropDown(std::vector<wxString> &texts,
                   std::vector<wxString> &tips,
                   std::vector<wxBitmap> &icons)
    : texts(texts)
    , tips(tips)
    , icons(icons)
    , state_handler(this)
    , border_color(0xDBDBDB)
    , text_color(0x363636)
    , selector_border_color(std::make_pair(0x009688, (int) StateColor::Hovered),
        std::make_pair(*wxWHITE, (int) StateColor::Normal))
    , selector_background_color(std::make_pair(0xBFE1DE, (int) StateColor::Checked), // ORCA updated background color for checked item
        std::make_pair(*wxWHITE, (int) StateColor::Normal))
{
}

DropDown::DropDown(wxWindow *             parent,
                   std::vector<wxString> &texts,
                   std::vector<wxString> &tips,
                   std::vector<wxBitmap> &icons,
                   long           style)
    : DropDown(texts, tips, icons)
{
    Create(parent, style);
}

void DropDown::Create(wxWindow *     parent,
         long           style)
{
    PopupWindow::Create(parent, wxPU_CONTAINS_CONTROLS);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
    state_handler.attach({&border_color, &text_color, &selector_border_color, &selector_background_color});
    state_handler.update_binds();
    if ((style & DD_NO_CHECK_ICON) == 0)
        check_bitmap = ScalableBitmap(this, "checked", 16);
    text_off = style & DD_NO_TEXT;
    // On macOS / GTK the popup can own the keyboard focus; the ComboBox forwards the same keys when it
    // keeps focus (always the case on Windows).
    Bind(wxEVT_CHAR, &DropDown::charEvent, this);

    // BBS set default font
    SetFont(Label::Body_14);
#ifdef __WXOSX__
    // PopupWindow releases mouse on idle, which may cause various problems,
    //  such as losting mouse move, and dismissing soon on first LEFT_DOWN event.
    Bind(wxEVT_IDLE, [] (wxIdleEvent & evt) {});
#endif
}

void DropDown::Invalidate(bool clear)
{
    if (clear) {
        selection = hover_item = -1;
        offset = wxPoint();
    }
    if (!filter_text.empty()) { // the item list changed under an active filter: start over
        filter_text.clear();
        visible_items.clear();
        kb_hover = false;
    }
    assert(selection < (int) texts.size());
    need_sync = true;
}

void DropDown::SetSelection(int n)
{
    assert(n < (int) texts.size());
    if (n >= (int) texts.size())
        n = -1;
    if (selection == n) return;
    selection = n;
    if (need_sync) { // for icon Size
        messureSize();
        need_sync = true;
    }
    paintNow();
}

wxString DropDown::GetValue() const
{
    return selection >= 0 ? texts[selection] : wxString();
}

void DropDown::SetValue(const wxString &value)
{
    auto i = std::find(texts.begin(), texts.end(), value);
    selection = i == texts.end() ? -1 : std::distance(texts.begin(), i);
}

void DropDown::SetCornerRadius(double radius)
{
    this->radius = radius;
    paintNow();
}

void DropDown::SetBorderColor(StateColor const &color)
{
    border_color = color;
    state_handler.update_binds();
    paintNow();
}

void DropDown::SetSelectorBorderColor(StateColor const &color)
{
    selector_border_color = color;
    state_handler.update_binds();
    paintNow();
}

void DropDown::SetTextColor(StateColor const &color)
{
    text_color = color;
    state_handler.update_binds();
    paintNow();
}

void DropDown::SetSelectorBackgroundColor(StateColor const &color)
{
    selector_background_color = color;
    state_handler.update_binds();
    paintNow();
}

void DropDown::SetUseContentWidth(bool use, bool limit_max_content_width)
{
    if (use_content_width == use)
        return;
    use_content_width = use;
    this->limit_max_content_width = limit_max_content_width;
    need_sync = true;
    messureSize();
}

void DropDown::SetAlignIcon(bool align) { align_icon = align; }

void DropDown::Rescale()
{
    need_sync = true;
}

bool DropDown::HasDismissLongTime()
{
    auto now = boost::posix_time::microsec_clock::universal_time();
    return !IsShown() &&
        (now - dismissTime).total_milliseconds() >= 20;
}

void DropDown::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxBufferedPaintDC dc(this);
    render(dc);
}

/*
 * Alternatively, you can use a clientDC to paint on the panel
 * at any time. Using this generally does not free you from
 * catching paint events, since it is possible that e.g. the window
 * manager throws away your drawing when the window comes to the
 * background, and expects you will redraw it when the window comes
 * back (by sending a paint event).
 */
void DropDown::paintNow()
{
    // depending on your system you may need to look at double-buffered dcs
    //wxClientDC dc(this);
    //render(dc);
    Refresh();
}

static wxSize GetBmpSize(wxBitmap & bmp)
{
#ifdef __APPLE__
    return bmp.GetScaledSize();
#else
    return bmp.GetSize();
#endif
}

const wxBitmap *DropDown::actionIcon(int n) const
{
    if (!action_icons || n < 0 || n >= int(action_icons->size()) || !(*action_icons)[n].IsOk())
        return nullptr;
    return &(*action_icons)[n];
}

// The x-range [first, second) of the column at the right of the rows that holds the items' action
// icons, empty when no item has one. render() draws the icons in it and mouseReleased() hit-tests
// clicks against it, so the drawn icon and its clickable area always match. (OrcaSlicer #13946)
std::pair<int, int> DropDown::actionColumn() const
{
    if (actionIconWidth <= 0)
        return {0, 0};
    int right = rowSize.x;
    if (rowSize.y * rowCount() > GetSize().y - headerH()) // position bar
        right -= 6;
    return {right - actionIconWidth - FromDIP(16), right};
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void DropDown::render(wxDC &dc)
{
    if (texts.size() == 0) return;
    int states = state_handler.states();
    dc.SetPen(wxPen(border_color.colorForStates(states)));
    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(GetBackgroundColour())));
    // if (GetWindowStyle() & wxBORDER_NONE)
    //    dc.SetPen(wxNullPen);

    // draw background
    wxSize size = GetSize();
    if (radius == 0)
        dc.DrawRectangle(0, 0, size.x, size.y);
    else
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);

    const int  header_h    = headerH();
    const int  top_y       = offset.y + header_h;
    const int  row_count   = rowCount();
    const bool hover_shown = (states & StateColor::Hovered) || kb_hover;
    const int  hover_row   = rowOf(hover_item);
    const int  sel_row     = rowOf(selection);

    // draw hover rectangle
    wxRect rcContent = {{0, top_y}, rowSize};
    if (hover_row >= 0 && hover_shown && !IsItemDisabled(hover_item)) {
        rcContent.y += rowSize.y * hover_row;
        if (rcContent.GetBottom() > 0 && rcContent.y < size.y) {
            if (selection == hover_item)
                dc.SetBrush(wxBrush(selector_background_color.colorForStates(states | StateColor::Checked)));
            dc.SetPen(wxPen(selector_border_color.colorForStates(states | StateColor::Hovered)));
            rcContent.Deflate(4, 1);
            dc.DrawRectangle(rcContent);
            rcContent.Inflate(4, 1);
        }
        rcContent.y = top_y;
    }
    // draw checked rectangle
    if (sel_row >= 0 && (selection != hover_item || !hover_shown)) {
        rcContent.y += rowSize.y * sel_row;
        if (rcContent.GetBottom() > 0 && rcContent.y < size.y) {
            dc.SetBrush(wxBrush(selector_background_color.colorForStates(states | StateColor::Checked)));
            dc.SetPen(wxPen(selector_background_color.colorForStates(states)));
            rcContent.Deflate(4, 1);
            dc.DrawRectangle(rcContent);
            rcContent.Inflate(4, 1);
        }
        rcContent.y = top_y;
    }
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    {
        wxSize offset = (rowSize - textSize) / 2;
        rcContent.Deflate(0, offset.y);
    }

    // draw position bar
    const int viewport_h = size.y - header_h;
    if (row_count > 0 && rowSize.y * row_count > viewport_h) {
        int    height = rowSize.y * row_count;
        wxRect rect = {size.x - 6, header_h - offset.y * viewport_h / height, 4,
                       viewport_h * viewport_h / height};
        dc.SetPen(wxPen(border_color.defaultColor()));
        dc.SetBrush(wxBrush(*wxLIGHT_GREY));
        dc.DrawRoundedRectangle(rect, 2);
        rcContent.width -= 6;
    }

    // draw check icon
    rcContent.x += 5;
    rcContent.width -= 5;
    if (check_bitmap.bmp().IsOk()) {
        auto szBmp = check_bitmap.GetBmpSize();
        if (sel_row >= 0) {
            wxPoint pt = rcContent.GetLeftTop();
            pt.y += (rcContent.height - szBmp.y) / 2;
            pt.y += rowSize.y * sel_row;
            if (pt.y + szBmp.y > 0 && pt.y < size.y)
                dc.DrawBitmap(check_bitmap.bmp(), pt);
        }
        rcContent.x += szBmp.x + 5;
        rcContent.width -= szBmp.x + 5;
    }
    const auto action_column = actionColumn();
    if (action_column.second > action_column.first)
        rcContent.width = action_column.first - rcContent.x;
    // draw texts & icons
    const wxColour normal_text   = text_color.colorForStates(states);
    // Header rows (favourites / all filaments) in a mid grey that reads on both themes.
    const wxColour disabled_text(144, 144, 144);
    dc.SetTextForeground(normal_text);
    for (int row = 0; row < row_count; ++row) {
        const int i = itemAt(row);
        if (i < 0) break;
        if (rcContent.GetBottom() < 0) {
            rcContent.y += rowSize.y;
            continue;
        }
        if (rcContent.y > size.y) break;
        wxPoint pt   = rcContent.GetLeftTop();
        auto & icon = icons[i];
        auto size2 = GetBmpSize(icon);
        if (iconSize.x > 0) {
            if (icon.IsOk()) {
                pt.y += (rcContent.height - size2.y) / 2;
                dc.DrawBitmap(icon, pt);
            }
            pt.x += iconSize.x + 5;
            pt.y = rcContent.y;
        } else if (icon.IsOk()) {
            pt.y += (rcContent.height - size2.y) / 2;
            dc.DrawBitmap(icon, pt);
            pt.x += size2.x + 5;
            pt.y = rcContent.y;
        }
        if (const wxBitmap *action = actionIcon(i)) {
            wxBitmap action_bmp = *action;
            auto     action_size = GetBmpSize(action_bmp);
            wxPoint  action_pt(action_column.second - action_size.x - FromDIP(8),
                               rcContent.y + (rcContent.height - action_size.y) / 2);
            dc.DrawBitmap(action_bmp, action_pt);
        }
        auto text = texts[i];
        if (!text_off && !text.IsEmpty()) {
            wxSize tSize = dc.GetMultiLineTextExtent(text);
            if (pt.x + tSize.x > rcContent.GetRight()) {
                int max_text_width = rcContent.GetRight() - pt.x;
                if (max_text_width <= 0) {
                    text.clear();
                } else {
                    if (i == hover_item && tips[i].IsEmpty())
                        SetToolTip(text);
                    text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, max_text_width);
                }
            }
            pt.y += (rcContent.height - textSize.y) / 2;
            dc.SetFont(GetFont());
            const bool row_disabled = IsItemDisabled(i);
            if (row_disabled)
                dc.SetTextForeground(disabled_text);
            dc.DrawText(text, pt);
            if (row_disabled)
                dc.SetTextForeground(normal_text);
        }
        rcContent.y += rowSize.y;
    }

    // Filter strip on top of the rows (drawn last so scrolled rows slide under it).
    if (header_h > 0) {
        dc.SetFont(GetFont());
        if (row_count == 0) {
            dc.SetTextForeground(disabled_text);
            dc.DrawText(_L("No matches"), wxPoint(FromDIP(12), header_h + (rowSize.y - textSize.y) / 2));
        }
        dc.SetPen(wxPen(border_color.colorForStates(states)));
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(GetBackgroundColour())));
        dc.DrawRectangle(0, 0, size.x, header_h);
        const bool has_text = !filter_text.empty();
        dc.SetTextForeground(has_text ? normal_text : disabled_text);
        wxString label = has_text ? filter_text : _L("Type to filter");
        const int max_w = size.x - FromDIP(20);
        if (dc.GetTextExtent(label).x > max_w)
            label = wxControl::Ellipsize(label, dc, has_text ? wxELLIPSIZE_START : wxELLIPSIZE_END, max_w);
        const wxSize ext = dc.GetTextExtent(label);
        dc.DrawText(label, wxPoint(FromDIP(10), (header_h - ext.y) / 2));
        if (has_text) { // caret
            dc.SetPen(wxPen(normal_text));
            const int cx = FromDIP(10) + ext.x + 1;
            dc.DrawLine(cx, (header_h - ext.y) / 2, cx, (header_h + ext.y) / 2);
        }
    }
}

void DropDown::messureSize()
{
    if (!need_sync) return;
    textSize = wxSize();
    iconSize = wxSize();
    actionIconWidth = 0;
    wxClientDC dc(GetParent() ? GetParent() : this);
    for (size_t i = 0; i < texts.size(); ++i) {
        wxSize size1 = text_off ? wxSize() : dc.GetMultiLineTextExtent(texts[i]);
        if (icons[i].IsOk()) {
            wxSize size2 = GetBmpSize(icons[i]);
            if (size2.x > iconSize.x) iconSize = size2;
            if (!align_icon) {
                size1.x += size2.x + (text_off ? 0 : 5);
            }
        }
        if (const wxBitmap *action = actionIcon(int(i))) {
            wxBitmap action_bmp = *action;
            actionIconWidth = std::max(actionIconWidth, GetBmpSize(action_bmp).x);
        }
        if (size1.x > textSize.x) textSize = size1;
    }
    // Every row leaves room for the action column, as render() and actionColumn() lay it out.
    if (actionIconWidth > 0)
        textSize.x += actionIconWidth + FromDIP(16);
    if (!align_icon) iconSize.x = 0;
    wxSize szContent = textSize;
    szContent.x += 10;
    if (check_bitmap.bmp().IsOk()) {
        auto szBmp = check_bitmap.GetBmpSize();
        szContent.x += szBmp.x + 5;
    }
    if (iconSize.x > 0) szContent.x += iconSize.x + (text_off ? 0 : 5);
    if (iconSize.y > szContent.y) szContent.y = iconSize.y;
    szContent.y += 10;
    if (texts.size() > 15) szContent.x += 6;
    if (GetParent()) {
        auto x = GetParent()->GetSize().x;
        if (!use_content_width || x > szContent.x)
            szContent.x = x;
    }
    rowSize = szContent;
    if (limit_max_content_width) {
        wxSize parent_size = GetParent()->GetSize();
        if (rowSize.x > parent_size.x * 2) {
            rowSize.x = 2 * parent_size.x;
            szContent = rowSize;
        }
    }
    szContent.y = popupHeight();
    wxWindow::SetSize(szContent);
#ifdef __WXGTK__
    // Gtk has a wrapper window for popup widget
    // Fix for GNOME Platform 48 X11 backend: ensure size is valid before calling gtk_window_resize
    int gtk_width = szContent.x;
    int gtk_height = szContent.y;
    if (gtk_width <= 0) gtk_width = 100;
    if (gtk_height <= 0) gtk_height = 100;
    gtk_window_resize(GTK_WINDOW(m_widget), gtk_width, gtk_height);
#endif
    need_sync = false;
}

void DropDown::autoPosition()
{
    resetFilterState();
    messureSize();
    wxPoint pos = GetParent()->ClientToScreen(wxPoint(0, -6));
    wxPoint old = GetPosition();
    wxSize size = GetSize();
    Position(pos, {0, GetParent()->GetSize().y + 12});
    if (old != GetPosition()) {
        size = rowSize;
        size.y = popupHeight();
#ifdef __WXGTK__
        if (size.x < 1) size.x = 1;
        if (size.y < 1) size.y = 1;
#endif
        if (size != GetSize()) {
            wxWindow::SetSize(size);
            offset = wxPoint();
            Position(pos, {0, GetParent()->GetSize().y + 12});
        }
    }
    if (GetPosition().y > pos.y) {
        // may exceed
        auto drect = wxDisplay(GetParent()).GetGeometry();
        if (GetPosition().y + size.y + 10 > drect.GetBottom()) {
            if (use_content_width && texts.size() <= 15) size.x += 6;
            size.y = drect.GetBottom() - GetPosition().y - 10;
#ifdef __WXGTK__
            if (size.y < 1) size.y = 1;
            if (size.x < 1) size.x = 1;
#endif
            wxWindow::SetSize(size);
            if (selection >= 0) {
                if (offset.y + rowSize.y * (selection + 1) > size.y - headerH())
                    offset.y = size.y - headerH() - rowSize.y * (selection + 1);
                else if (offset.y + rowSize.y * selection < 0)
                    offset.y = -rowSize.y * selection;
            }
        }
    }
}

bool DropDown::PointInAnchorGap(const wxPoint& screen_point) const
{
    wxWindow* anchor = GetParent();
    if (!anchor)
        return false;

    const wxRect anchor_rect = anchor->GetScreenRect();
    const wxRect popup_rect  = GetScreenRect();

    const int left  = std::max(anchor_rect.GetLeft(), popup_rect.GetLeft());
    const int right = std::min(anchor_rect.GetRight(), popup_rect.GetRight());
    if (right < left)
        return false;

    // Screen y grows downward. Boundary pixels belong to the anchor or popup, so the clickable gap is
    // strictly between the two rectangles.
    if (popup_rect.GetTop() > anchor_rect.GetBottom()) {
        return screen_point.x >= left && screen_point.x <= right && screen_point.y > anchor_rect.GetBottom() &&
               screen_point.y < popup_rect.GetTop();
    }

    if (popup_rect.GetBottom() < anchor_rect.GetTop()) {
        return screen_point.x >= left && screen_point.x <= right && screen_point.y > popup_rect.GetBottom() &&
               screen_point.y < anchor_rect.GetTop();
    }

    return false;
}

bool DropDown::ProcessLeftDown(wxMouseEvent& event)
{
#ifdef __WXOSX__
    if (IsShown() && HitTest(event.GetPosition()) == wxHT_WINDOW_OUTSIDE && PointInAnchorGap(ClientToScreen(event.GetPosition()))) {
        DismissAndNotify();

        // wxOSX reposts an outside click to the control below the popup. The anchor/popup gap is not
        // an activation target, so consume it after dismissing.
        return true;
    }
#endif

    return PopupWindow::ProcessLeftDown(event);
}

void DropDown::mouseDown(wxMouseEvent& event)
{
    // Receivce unexcepted LEFT_DOWN on Mac after OnDismiss
    if (!IsShown())
        return;
    // force calc hover item again
    mouseMove(event);
    pressedDown = true;
    CaptureMouse();
    dragStart   = event.GetPosition();
}

void DropDown::mouseReleased(wxMouseEvent& event)
{
    if (pressedDown) {
        dragStart = wxPoint();
        pressedDown = false;
        if (HasCapture())
            ReleaseMouse();
        if (hover_item >= 0) { // not moved
            // A click on the item's action icon runs the action and keeps the list open.
            if (actionIcon(hover_item)) {
                const auto action_column = actionColumn();
                if (event.GetX() >= action_column.first && event.GetX() < action_column.second) {
                    sendDropDownActionEvent(hover_item);
                    paintNow();
                    return;
                }
            }
            // A header row cannot be picked; the list stays open, as a native list does.
            if (IsItemDisabled(hover_item))
                return;
            sendDropDownEvent();
            DismissAndNotify();
        }
    }
}

void DropDown::mouseCaptureLost(wxMouseCaptureLostEvent &event)
{
    wxMouseEvent evt;
    mouseReleased(evt);
}

void DropDown::mouseMove(wxMouseEvent &event)
{
    wxPoint pt  = event.GetPosition();
    if (filter_enabled && event.GetEventType() == wxEVT_MOTION && !pressedDown) {
        // The list resizes while typing; Windows then reports a motion event for a mouse that did not
        // move, which would steal the keyboard highlight. Only a real move counts.
        const wxPoint screen = ClientToScreen(pt);
        if (screen == last_motion_screen)
            return;
        last_motion_screen = screen;
    }
    kb_hover = false;
    if (pressedDown) {
        wxPoint pt2 = offset + pt - dragStart;
        dragStart    = pt;
        clampOffsetY(pt2.y);
        if (pt2.y != offset.y) {
            offset = pt2;
            hover_item = -1; // moved
        } else {
            return;
        }
    }
    if (!pressedDown || hover_item >= 0) {
        int hover = itemAtY(pt.y);
        if (hover == hover_item) return;
        hover_item = hover;
        if (hover >= 0) SetToolTip(tips[hover]);
    }
    paintNow();
}

void DropDown::mouseWheelMoved(wxMouseEvent &event)
{
    auto delta = event.GetWheelRotation();
    wxPoint pt2   = offset + wxPoint{0, delta};
    clampOffsetY(pt2.y);
    if (pt2.y != offset.y) {
        offset = pt2;
    } else {
        return;
    }
    int hover = itemAtY(event.GetPosition().y);
    if (hover != hover_item) {
        hover_item = hover;
        if (hover >= 0) SetToolTip(tips[hover]);
    }
    paintNow();
}

// currently unused events
void DropDown::sendDropDownEvent()
{
    wxCommandEvent event(wxEVT_COMBOBOX, GetId());
    event.SetEventObject(this);
    event.SetInt(hover_item);
    event.SetString(texts[hover_item]);
    GetEventHandler()->ProcessEvent(event);
}

void DropDown::sendDropDownActionEvent(int index)
{
    if (index < 0 || index >= int(texts.size()) || IsItemDisabled(index))
        return;
    wxCommandEvent event(EVT_DROPDOWN_ITEM_ACTION, GetId());
    event.SetEventObject(this);
    event.SetInt(index);
    event.SetString(texts[index]);
    GetEventHandler()->ProcessEvent(event);
}

void DropDown::OnDismiss()
{
    dismissTime = boost::posix_time::microsec_clock::universal_time();
    hover_item  = -1;
    kb_hover    = false;
    last_motion_screen = wxPoint(-1, -1);
    resetFilterState();
    wxCommandEvent e(EVT_DISMISS);
    GetEventHandler()->ProcessEvent(e);
}

// ---------------------------------------------------------------------------------------------
// Type-to-filter
// ---------------------------------------------------------------------------------------------

void DropDown::EnableTypeToFilter(bool on)
{
    if (filter_enabled == on)
        return;
    filter_enabled = on;
    resetFilterState();
    need_sync = true;
}

int DropDown::headerH() const { return filter_enabled ? FromDIP(28) : 0; }

int DropDown::itemAt(int row) const
{
    if (row < 0 || row >= rowCount())
        return -1;
    return filter_text.empty() ? row : visible_items[row];
}

int DropDown::rowOf(int item) const
{
    if (item < 0)
        return -1;
    if (filter_text.empty())
        return item < int(texts.size()) ? item : -1;
    auto it = std::find(visible_items.begin(), visible_items.end(), item);
    return it == visible_items.end() ? -1 : int(std::distance(visible_items.begin(), it));
}

// The item under a y coordinate of the popup, -1 for the filter strip or empty space.
int DropDown::itemAtY(int y) const
{
    const int rel = y - offset.y - headerH();
    if (rel < 0 || rowSize.y <= 0 || y < headerH())
        return -1;
    return itemAt(rel / rowSize.y);
}

int DropDown::popupHeight() const
{
    const size_t rows = filter_text.empty() ? texts.size() : std::max<size_t>(1, visible_items.size());
    int h = rowSize.y * int(std::min<size_t>(15, rows));
    if (rows > 15)
        h += rowSize.y / 2;
    return h + headerH();
}

void DropDown::clampOffsetY(int &y) const
{
    const int viewport = GetSize().y - headerH();
    const int total    = rowSize.y * rowCount();
    if (y > 0 || total <= viewport)
        y = 0;
    else if (y + total < viewport)
        y = viewport - total;
}

// Forget the filter (the list is closed or being reopened). The next open re-measures, because the
// window may still have the shrunken size of the last filtered view.
void DropDown::resetFilterState()
{
    if (!filter_enabled)
        return;
    filter_text.clear();
    visible_items.clear();
    kb_hover = false;
    offset   = wxPoint();
    need_sync = true;
}

void DropDown::applyFilter()
{
    visible_items.clear();
    hover_item = -1;
    kb_hover   = false;
    if (!filter_text.empty()) {
        const wxString needle = filter_text.Lower();
        std::vector<wxString> seen; // the same preset listed under Favorites and again below: show it once
        for (int i = 0; i < int(texts.size()); ++i) {
            if (IsItemDisabled(i) || !texts[i].Lower().Contains(needle))
                continue;
            auto dup = std::find(seen.begin(), seen.end(), texts[i]);
            if (dup != seen.end()) {
                if (i == selection) // keep the row that carries the check mark
                    visible_items[std::distance(seen.begin(), dup)] = i;
                continue;
            }
            seen.push_back(texts[i]);
            visible_items.push_back(i);
        }
        if (!visible_items.empty()) {
            hover_item = visible_items.front();
            kb_hover   = true;
            SetToolTip(tips[hover_item]);
        }
    }
    offset = wxPoint();
    relayoutForFilter();
}

// Resize and re-position the open popup for the current number of rows.
void DropDown::relayoutForFilter()
{
    if (!IsShown() || !GetParent()) {
        Refresh();
        return;
    }
    wxSize size(rowSize.x, popupHeight());
    wxWindow::SetSize(size);
    const wxPoint pos = GetParent()->ClientToScreen(wxPoint(0, -6));
    Position(pos, {0, GetParent()->GetSize().y + 12});
    if (GetPosition().y > pos.y) { // below the box: keep it on the screen
        auto drect = wxDisplay(GetParent()).GetGeometry();
        if (GetPosition().y + size.y + 10 > drect.GetBottom()) {
            size.y = drect.GetBottom() - GetPosition().y - 10;
            if (size.y < headerH() + rowSize.y)
                size.y = headerH() + rowSize.y;
            wxWindow::SetSize(size);
        }
    }
    Refresh();
}

void DropDown::moveHighlight(int step)
{
    const int rows = rowCount();
    if (rows <= 0)
        return;
    int row = rowOf(hover_item);
    if (row < 0)
        row = rowOf(selection);
    if (row < 0)
        row = step > 0 ? -1 : rows;
    int n = row + step;
    while (n >= 0 && n < rows && IsItemDisabled(itemAt(n)))
        n += step;
    if (n < 0 || n >= rows)
        return;
    hover_item = itemAt(n);
    kb_hover   = true;
    // scroll the row into view
    const int viewport = GetSize().y - headerH();
    const int row_top  = n * rowSize.y;
    if (row_top + offset.y < 0)
        offset.y = -row_top;
    else if (row_top + rowSize.y + offset.y > viewport)
        offset.y = viewport - row_top - rowSize.y;
    SetToolTip(tips[hover_item]);
    paintNow();
}

bool DropDown::IsPrintableKey(const wxKeyEvent &event)
{
    // Ctrl / Alt / Cmd shortcuts are not text (AltGr arrives as Ctrl+Alt on Windows and is text).
    if (event.MetaDown() || (event.ControlDown() && !event.AltDown()) || (event.AltDown() && !event.ControlDown()))
        return false;
    const wxChar c = event.GetUnicodeKey();
    return c >= 32 && c != 127;
}

bool DropDown::HandlePrintableKey(wxKeyEvent &event)
{
    if (!filter_enabled || !IsShown() || !IsPrintableKey(event))
        return false;
    const wxChar c = event.GetUnicodeKey();
    if (c == ' ' && filter_text.empty())
        return false; // Space on an empty filter keeps its old meaning (close)
    if (filter_text.length() < 64) {
        filter_text += c;
        applyFilter();
    }
    return true;
}

bool DropDown::HandleSpecialKey(wxKeyEvent &event)
{
    if (!filter_enabled || !IsShown())
        return false;
    switch (event.GetKeyCode()) {
    case WXK_ESCAPE:
        if (filter_text.empty()) {
            DismissAndNotify();
        } else {
            filter_text.clear();
            applyFilter();
        }
        return true;
    case WXK_BACK:
        if (!filter_text.empty()) {
            filter_text.RemoveLast();
            applyFilter();
        }
        return true;
    case WXK_UP:
        moveHighlight(-1);
        return true;
    case WXK_DOWN:
        moveHighlight(1);
        return true;
    case WXK_RETURN:
    case WXK_NUMPAD_ENTER:
        if (hover_item >= 0 && !IsItemDisabled(hover_item) && (kb_hover || !filter_text.empty())) {
            sendDropDownEvent();
            DismissAndNotify();
            return true;
        }
        return false;
    default:
        return false;
    }
}

void DropDown::charEvent(wxKeyEvent &event)
{
    if (HandleSpecialKey(event) || HandlePrintableKey(event))
        return;
    event.Skip();
}
