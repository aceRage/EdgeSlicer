#include "FilamentColourPicker.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/SpinInput.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Showcase/MultiColourFilament.hpp"

#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace Slic3r
{
namespace GUI
{
namespace
{

constexpr size_t kFcpRecentMax = 8;
const char* const kFcpRecentKey = "recent_filament_colours";

std::string FcpNormalHex(const std::string& hex)
{
    std::array<int, 3> rgb;
    if (!MultiColour::parse_rgb8(hex, rgb))
        return std::string();
    return MultiColour::format_hex8(rgb[0], rgb[1], rgb[2]);
}

wxColour FcpColour(const std::string& hex)
{
    std::array<int, 3> rgb;
    if (!MultiColour::parse_rgb8(hex, rgb))
        return wxColour(0, 0, 0);
    return wxColour(static_cast<unsigned char>(rgb[0]), static_cast<unsigned char>(rgb[1]), static_cast<unsigned char>(rgb[2]));
}

wxColour FcpBackground()
{
    return StateColor::darkModeColorFor(wxColour("#FFFFFF"));
}

wxColour FcpBorder()
{
    return StateColor::darkModeColorFor(wxColour(180, 180, 180));
}

wxColour FcpAccent()
{
    return wxColour("#019687");
}

/**
 * @brief Base of the two picking areas: turns a mouse position into a callback while dragging.
 */
class FcpDragPanel : public wxPanel
{
public:
    FcpDragPanel(wxWindow* parent, const wxSize& size, std::function<void(double, double)> onDrag)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , _onDrag(std::move(onDrag))
    {
        SetMinSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_CROSS));
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event)
        {
            if (!HasCapture())
                CaptureMouse();
            Report(event);
        });
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event)
        {
            if (event.Dragging() && event.LeftIsDown() && HasCapture())
                Report(event);
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&)
        {
            if (HasCapture())
                ReleaseMouse();
        });
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});
    }

private:
    void Report(const wxMouseEvent& event)
    {
        const wxSize size = GetClientSize();
        const double x = std::clamp(double(event.GetX()) / std::max(1, size.GetWidth() - 1), 0.0, 1.0);
        const double y = std::clamp(double(event.GetY()) / std::max(1, size.GetHeight() - 1), 0.0, 1.0);
        if (_onDrag)
            _onDrag(x, y);
    }

    std::function<void(double, double)> _onDrag;
};

/**
 * @brief Hue (across) and saturation (down) at the current value.
 */
class FcpHuePlane : public FcpDragPanel
{
public:
    FcpHuePlane(wxWindow* parent, const wxSize& size, std::function<void(double, double)> onDrag)
        : FcpDragPanel(parent, size, std::move(onDrag))
    {
        Bind(wxEVT_PAINT, &FcpHuePlane::OnPaint, this);
    }

    void SetState(double hue, double sat, double val)
    {
        _hue = hue;
        _sat = sat;
        _val = val;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        const int w = std::max(2, size.GetWidth()), h = std::max(2, size.GetHeight());
        if (_image.GetWidth() != w || _image.GetHeight() != h || _imageValue != _val)
        {
            _image = wxImage(w, h, false);
            unsigned char* data = _image.GetData();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const auto rgb = MultiColour::hsv_to_rgb8(360.0 * x / (w - 1), 1.0 - double(y) / (h - 1), _val);
                    unsigned char* p = data + 3 * (size_t(y) * size_t(w) + size_t(x));
                    p[0] = (unsigned char) rgb[0];
                    p[1] = (unsigned char) rgb[1];
                    p[2] = (unsigned char) rgb[2];
                }
            _imageValue = _val;
        }
        dc.DrawBitmap(wxBitmap(_image), 0, 0, false);
        const int mx = int(std::lround(_hue / 360.0 * (w - 1))), my = int(std::lround((1.0 - _sat) * (h - 1)));
        const int r = FromDIP(5);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(*wxBLACK, 3));
        dc.DrawCircle(mx, my, r);
        dc.SetPen(wxPen(*wxWHITE, 1));
        dc.DrawCircle(mx, my, r);
    }

    double _hue { 0.0 };
    double _sat { 0.0 };
    double _val { 1.0 };
    double _imageValue { -1.0 };
    wxImage _image;
};

/**
 * @brief The value (brightness) slider, a bar from black to the full colour.
 */
class FcpValueBar : public FcpDragPanel
{
public:
    FcpValueBar(wxWindow* parent, const wxSize& size, std::function<void(double, double)> onDrag)
        : FcpDragPanel(parent, size, std::move(onDrag))
    {
        Bind(wxEVT_PAINT, &FcpValueBar::OnPaint, this);
    }

    void SetState(double hue, double sat, double val)
    {
        _hue = hue;
        _sat = sat;
        _val = val;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        const int w = std::max(2, size.GetWidth()), h = std::max(2, size.GetHeight());
        dc.SetBackground(wxBrush(FcpBackground()));
        dc.Clear();
        const auto full = MultiColour::hsv_to_rgb8(_hue, _sat, 1.0);
        dc.GradientFillLinear(wxRect(0, 0, w, h), *wxBLACK, wxColour(full[0], full[1], full[2]), wxEAST);
        const int x = int(std::lround(_val * (w - 1)));
        dc.SetPen(wxPen(*wxBLACK, 3));
        dc.DrawLine(x, 0, x, h);
        dc.SetPen(wxPen(*wxWHITE, 1));
        dc.DrawLine(x, 0, x, h);
    }

    double _hue { 0.0 };
    double _sat { 0.0 };
    double _val { 1.0 };
};

class FcpPreview : public wxPanel
{
public:
    FcpPreview(wxWindow* parent, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
    {
        SetMinSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&)
        {
            wxAutoBufferedPaintDC dc(this);
            const wxSize s = GetClientSize();
            dc.SetBackground(wxBrush(FcpBackground()));
            dc.Clear();
            dc.SetPen(wxPen(FcpBorder(), 1));
            dc.SetBrush(wxBrush(_colour));
            dc.DrawRoundedRectangle(0, 0, s.GetWidth(), s.GetHeight(), FromDIP(4));
        });
    }

    void SetPreviewColour(const wxColour& colour)
    {
        _colour = colour;
        Refresh();
    }

private:
    wxColour _colour { 0, 0, 0 };
};

} // namespace

std::vector<std::string> RecentFilamentColours()
{
    std::vector<std::string> out;
    if (wxGetApp().app_config == nullptr)
        return out;
    const std::string stored = wxGetApp().app_config->get(kFcpRecentKey);
    size_t start = 0;
    while (start <= stored.size() && out.size() < kFcpRecentMax)
    {
        size_t end = stored.find(';', start);
        if (end == std::string::npos)
            end = stored.size();
        const std::string hex = FcpNormalHex(stored.substr(start, end - start));
        if (!hex.empty() && std::find(out.begin(), out.end(), hex) == out.end())
            out.push_back(hex);
        start = end + 1;
    }
    return out;
}

void RememberFilamentColours(const std::vector<std::string>& colours)
{
    if (wxGetApp().app_config == nullptr)
        return;
    std::vector<std::string> list;
    for (const std::string& colour : colours)
    {
        const std::string hex = FcpNormalHex(colour);
        if (!hex.empty() && std::find(list.begin(), list.end(), hex) == list.end())
            list.push_back(hex);
    }
    for (const std::string& old : RecentFilamentColours())
        if (std::find(list.begin(), list.end(), old) == list.end())
            list.push_back(old);
    if (list.size() > kFcpRecentMax)
        list.resize(kFcpRecentMax);
    std::string joined;
    for (const std::string& hex : list)
        joined += (joined.empty() ? "" : ";") + hex;
    wxGetApp().app_config->set(kFcpRecentKey, joined);
}

// ---------------------------------------------------------------------------------------------
// ColourSwatchGrid
// ---------------------------------------------------------------------------------------------

ColourSwatchGrid::ColourSwatchGrid(wxWindow* parent, int columns, int rows, int swatchDip, int gapDip,
                                   std::function<void(const std::string&)> onPick)
    : wxPanel(parent, wxID_ANY)
    , _columns(std::max(1, columns))
    , _rows(std::max(1, rows))
    , _swatch(FromDIP(swatchDip))
    , _gap(FromDIP(gapDip))
    , _onPick(std::move(onPick))
{
    const wxSize size(_columns * (_swatch + _gap), _rows * (_swatch + _gap));
    SetMinSize(size);
    SetMaxSize(size);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    Bind(wxEVT_PAINT, &ColourSwatchGrid::OnPaint, this);
    Bind(wxEVT_MOTION, &ColourSwatchGrid::OnMotion, this);
    Bind(wxEVT_LEFT_UP, &ColourSwatchGrid::OnLeftUp, this);
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event)
    {
        _hover = -1;
        UnsetToolTip();
        event.Skip();
    });
}

void ColourSwatchGrid::SetEntries(const std::vector<SwatchEntry>& entries)
{
    _entries = entries;
    Refresh();
}

void ColourSwatchGrid::SetSelected(const std::string& hex)
{
    const std::string normal = FcpNormalHex(hex);
    if (normal == _selected)
        return;
    _selected = normal;
    Refresh();
}

wxRect ColourSwatchGrid::CellRect(size_t index) const
{
    const int column = int(index) % _columns, row = int(index) / _columns;
    return wxRect(column * (_swatch + _gap), row * (_swatch + _gap), _swatch, _swatch);
}

int ColourSwatchGrid::HitTest(const wxPoint& point) const
{
    for (size_t i = 0; i < _entries.size(); ++i)
        if (CellRect(i).Contains(point))
            return int(i);
    return -1;
}

void ColourSwatchGrid::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(FcpBackground()));
    dc.Clear();
    const int cells = _columns * _rows;
    for (int i = 0; i < cells; ++i)
    {
        const wxRect rect = CellRect(size_t(i));
        if (size_t(i) >= _entries.size())
        {
            dc.SetPen(wxPen(FcpBorder(), 1, wxPENSTYLE_DOT));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(rect);
            continue;
        }
        const SwatchEntry& entry = _entries[size_t(i)];
        dc.SetPen(wxPen(FcpBorder(), 1));
        dc.SetBrush(wxBrush(FcpColour(entry.hex)));
        dc.DrawRectangle(rect);
        if (FcpNormalHex(entry.hex) == _selected && !_selected.empty())
        {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(FcpAccent(), 2));
            dc.DrawRectangle(rect.Inflate(1, 1));
        }
        else if (i == _hover)
        {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#242424")), 1));
            dc.DrawRectangle(rect.Inflate(1, 1));
        }
    }
}

void ColourSwatchGrid::OnMotion(wxMouseEvent& event)
{
    const int hit = HitTest(event.GetPosition());
    if (hit != _hover)
    {
        _hover = hit;
        if (hit >= 0)
        {
            const SwatchEntry& entry = _entries[size_t(hit)];
            SetToolTip(wxString::FromUTF8((entry.name.empty() ? entry.hex : entry.name + "  " + entry.hex).c_str()));
        }
        else
            UnsetToolTip();
        Refresh();
    }
    event.Skip();
}

void ColourSwatchGrid::OnLeftUp(wxMouseEvent& event)
{
    const int hit = HitTest(event.GetPosition());
    if (hit >= 0 && _onPick)
        _onPick(_entries[size_t(hit)].hex);
    event.Skip();
}

// ---------------------------------------------------------------------------------------------
// FilamentColourPickerDialog
// ---------------------------------------------------------------------------------------------

FilamentColourPickerDialog::FilamentColourPickerDialog(wxWindow* parent, const std::string& initialHex)
    : DPIDialog(parent, wxID_ANY, _L("Choose colour"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(FcpBackground());
    SetFont(Label::Body_14);

    std::array<int, 3> start{ 38, 166, 154 };
    MultiColour::parse_rgb8(initialHex, start);

    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    const int margin = FromDIP(12);
    const int columns = 14, swatch = 18, gap = 3;
    const int areaWidth = columns * (FromDIP(swatch) + FromDIP(gap));

    // The named palette.
    std::vector<SwatchEntry> entries;
    for (const MultiColour::NamedColour& colour : MultiColour::basic_palette())
        entries.push_back({ colour.hex, colour.name });
    _palette = new ColourSwatchGrid(this, columns, 4, swatch, gap, [this](const std::string& hex)
    {
        std::array<int, 3> rgb;
        if (MultiColour::parse_rgb8(hex, rgb))
            SetFromRgb(rgb[0], rgb[1], rgb[2]);
    });
    _palette->SetEntries(entries);
    root->Add(_palette, 0, wxLEFT | wxRIGHT | wxTOP, margin);

    // Recent colours.
    root->Add(new ::Label(this, Label::Body_12, _L("Recent colours")), 0, wxLEFT | wxRIGHT | wxTOP, margin);
    _recent = new ColourSwatchGrid(this, 8, 1, swatch, gap, [this](const std::string& hex)
    {
        std::array<int, 3> rgb;
        if (MultiColour::parse_rgb8(hex, rgb))
            SetFromRgb(rgb[0], rgb[1], rgb[2]);
    });
    std::vector<SwatchEntry> recent;
    for (const std::string& hex : RecentFilamentColours())
        recent.push_back({ hex, std::string() });
    _recent->SetEntries(recent);
    root->Add(_recent, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(4));

    // The hue / saturation area and the value bar.
    FcpHuePlane* plane = new FcpHuePlane(this, wxSize(areaWidth, FromDIP(110)), [this](double x, double y)
    {
        SetFromHsv(360.0 * x * 0.9999, 1.0 - y, _val);
    });
    _plane = plane;
    root->Add(plane, 0, wxLEFT | wxRIGHT | wxTOP, margin);
    FcpValueBar* bar = new FcpValueBar(this, wxSize(areaWidth, FromDIP(18)), [this](double x, double)
    {
        SetFromHsv(_hue, _sat, x);
    });
    _valueSlider = bar;
    root->Add(bar, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

    // Preview, hex and R / G / B.
    wxBoxSizer* values = new wxBoxSizer(wxHORIZONTAL);
    FcpPreview* preview = new FcpPreview(this, wxSize(FromDIP(40), FromDIP(28)));
    _preview = preview;
    values->Add(preview, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    _hexText = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(78), -1), wxTE_PROCESS_ENTER);
    _hexText->SetMaxLength(7);
    _hexText->SetToolTip(_L("Hex colour, #RRGGBB"));
    _hexText->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { ApplyHexText(); });
    _hexText->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event)
    {
        ApplyHexText();
        event.Skip();
    });
    values->Add(_hexText, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    const char* const channel[3] = { "R", "G", "B" };
    for (int i = 0; i < 3; ++i)
    {
        SpinInput* spin = new SpinInput(this, wxEmptyString, channel[i], wxDefaultPosition, wxSize(FromDIP(64), FromDIP(28)),
                                        wxSP_ARROW_KEYS, 0, 255, 0, 1);
        spin->Bind(wxEVT_SPINCTRL, [this, i](wxCommandEvent&)
        {
            if (_syncing)
                return;
            int rgb[3] = { _rgb[0], _rgb[1], _rgb[2] };
            rgb[i] = _rgbSpin[size_t(i)]->GetValue();
            SetFromRgb(rgb[0], rgb[1], rgb[2]);
        });
        _rgbSpin[size_t(i)] = spin;
        values->Add(spin, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    }
    root->Add(values, 0, wxLEFT | wxRIGHT | wxTOP, margin);

    // System picker and OK / Cancel.
    wxBoxSizer* bottom = new wxBoxSizer(wxHORIZONTAL);
    Button* system = new Button(this, _L("System picker") + "...");
    system->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    system->SetToolTip(_L("Open the operating system's colour dialog"));
    system->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenSystemPicker(); });
    bottom->Add(system, 0, wxALIGN_CENTER_VERTICAL);
    bottom->AddStretchSpacer(1);
    DialogButtons* buttons = new DialogButtons(this, { "OK", "Cancel" });
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    bottom->Add(buttons, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(bottom, 0, wxEXPAND | wxALL, margin);

    SetSizer(root);
    SetFromRgb(start[0], start[1], start[2]);
    Fit();
    wxGetApp().UpdateDlgDarkUI(this);
    CentreOnParent();
}

std::string FilamentColourPickerDialog::Colour() const
{
    return MultiColour::format_hex8(_rgb[0], _rgb[1], _rgb[2]);
}

void FilamentColourPickerDialog::SetFromRgb(int r, int g, int b)
{
    _rgb[0] = std::clamp(r, 0, 255);
    _rgb[1] = std::clamp(g, 0, 255);
    _rgb[2] = std::clamp(b, 0, 255);
    const auto hsv = MultiColour::rgb8_to_hsv(_rgb[0], _rgb[1], _rgb[2]);
    // A grey or black has no hue / saturation of its own: keep the ones the user had.
    if (hsv[1] > 0.0 && hsv[2] > 0.0)
        _hue = hsv[0];
    if (hsv[2] > 0.0)
        _sat = hsv[1];
    _val = hsv[2];
    SyncControls();
}

void FilamentColourPickerDialog::SetFromHsv(double hue, double saturation, double value)
{
    _hue = std::clamp(hue, 0.0, 359.999);
    _sat = std::clamp(saturation, 0.0, 1.0);
    _val = std::clamp(value, 0.0, 1.0);
    const auto rgb = MultiColour::hsv_to_rgb8(_hue, _sat, _val);
    _rgb[0] = rgb[0];
    _rgb[1] = rgb[1];
    _rgb[2] = rgb[2];
    SyncControls();
}

void FilamentColourPickerDialog::SyncControls()
{
    _syncing = true;
    const std::string hex = Colour();
    if (_hexText != nullptr && !_hexText->GetValue().IsSameAs(wxString::FromUTF8(hex.c_str()), false))
        _hexText->ChangeValue(wxString::FromUTF8(hex.c_str()));
    for (size_t i = 0; i < 3; ++i)
        if (_rgbSpin[i] != nullptr && _rgbSpin[i]->GetValue() != _rgb[i])
            _rgbSpin[i]->SetValue(_rgb[i]);
    if (_plane != nullptr)
        static_cast<FcpHuePlane*>(_plane)->SetState(_hue, _sat, _val);
    if (_valueSlider != nullptr)
        static_cast<FcpValueBar*>(_valueSlider)->SetState(_hue, _sat, _val);
    if (_preview != nullptr)
        static_cast<FcpPreview*>(_preview)->SetPreviewColour(FcpColour(hex));
    if (_palette != nullptr)
        _palette->SetSelected(hex);
    if (_recent != nullptr)
        _recent->SetSelected(hex);
    _syncing = false;
}

void FilamentColourPickerDialog::ApplyHexText()
{
    if (_syncing || _hexText == nullptr)
        return;
    std::array<int, 3> rgb;
    if (MultiColour::parse_rgb8(_hexText->GetValue().ToStdString(), rgb))
        SetFromRgb(rgb[0], rgb[1], rgb[2]);
    else
        SyncControls();   // not a colour: put the current one back
}

void FilamentColourPickerDialog::OpenSystemPicker()
{
    wxColourData data;
    data.SetChooseFull(true);
    data.SetChooseAlpha(false);
    data.SetColour(wxColour(static_cast<unsigned char>(_rgb[0]), static_cast<unsigned char>(_rgb[1]),
                            static_cast<unsigned char>(_rgb[2])));
    wxColourDialog dialog(this, &data);
    dialog.SetTitle(_L("Please choose the filament color"));
    if (dialog.ShowModal() != wxID_OK)
        return;
    const wxColour colour = dialog.GetColourData().GetColour();
    if (colour.IsOk())
        SetFromRgb(colour.Red(), colour.Green(), colour.Blue());
}

} // namespace GUI
} // namespace Slic3r
