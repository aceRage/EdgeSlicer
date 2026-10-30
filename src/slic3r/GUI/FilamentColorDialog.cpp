#include "FilamentColorDialog.hpp"

#include "FilamentColorUtils.hpp"
#include "FilamentColourPicker.hpp"
#include "GUI_App.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/SpinInput.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Showcase/MultiColourFilament.hpp"

#include <ColorSpaceConvert.hpp>
#include <boost/log/trivial.hpp>

#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/image.h>
#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/panel.h>
#include <wx/region.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <vector>

namespace Slic3r
{
namespace GUI
{
namespace
{

/**
 * @brief HSV value cached for color sorting.
 */
struct SortHsv
{
    float h { 0.0f };
    float s { 0.0f };
    float v { 0.0f };
};

/**
 * @brief Lightweight color item with cached sort data.
 */
struct ColorSortItem
{
    const FilamentColorItem* colorItem { nullptr };
    std::vector<std::string> colors;
    std::vector<SortHsv> hsvColors;
    int typeOrder { 0 };
    std::string enName;
    std::string zhName;
};

/**
 * @brief Gets the current language code used by the filament color data.
 */
std::string GetLanguageCode()
{
    if (wxGetApp().app_config == nullptr)
        return "en";

    const std::string language = wxGetApp().app_config->get("language");
    if (language.empty())
        return "en";
    if (language == "zh-cn")
        return "zh_CN";
    return language;
}

/**
 * @brief Converts an UTF-8 string to wxString.
 */
wxString FromStdString(const std::string& value)
{
    return wxString::FromUTF8(value.c_str());
}

/**
 * @brief Gets a localized color name with English fallback.
 */
std::string GetLocalizedColorName(const FilamentColorItem& colorItem, const std::string& languageCode,
                                  const std::vector<std::string>& colors)
{
    std::unordered_map<std::string, std::string>::const_iterator localizedName =
        colorItem.colorNames.find(languageCode);
    if (localizedName != colorItem.colorNames.end() && !localizedName->second.empty())
        return localizedName->second;

    std::unordered_map<std::string, std::string>::const_iterator englishName = colorItem.colorNames.find("en");
    if (englishName != colorItem.colorNames.end() && !englishName->second.empty())
        return englishName->second;

    if (colors.size() > 1)
        return _L("Multiple Color").ToStdString(wxConvUTF8);
    return colors.empty() ? std::string() : colors.front();
}

/**
 * @brief Gets normalized colors already loaded by the filament color library.
 */
std::vector<std::string> ColorListFromLibrary(const FilamentColorItem& colorItem)
{
    return colorItem.colorData.colors;
}

/**
 * @brief Builds color data from a built-in color.
 */
bool MakeFilamentColor(const FilamentColorItem& colorItem, FilamentColor& colorData)
{
    if (colorItem.colorData.colors.empty())
    {
        BOOST_LOG_TRIVIAL(error) << "Invalid official filament color without valid colors: " << colorItem.sku;
        return false;
    }

    colorData = FilamentColor::FromColors(colorItem.colorData.colors, colorItem.colorData.mode);
    return true;
}

/**
 * @brief Finds an official color item matching the saved colors and display mode.
 */
std::vector<FilamentColorItem>::const_iterator FindColorBySavedColors(const FilamentColorInfo& filament,
                                                                      const FilamentColor& colorData)
{
    if (colorData.colors.empty())
        return filament.colors.end();

    const FilamentColor normalizedColor = FilamentColor::FromColors(colorData.colors, colorData.mode);
    return std::find_if(filament.colors.begin(), filament.colors.end(),
                        [&normalizedColor](const FilamentColorItem& item)
                        {
                            return item.colorData.Matches(normalizedColor);
                        });
}

/**
 * @brief Gets the preview name for the current selected color data.
 */
std::string GetSelectionDisplayName(const FilamentColorInfo& filament, const FilamentColor& colorData,
                                    const std::string& selectedSku, const std::string& languageCode)
{
    if (!selectedSku.empty())
    {
        std::vector<FilamentColorItem>::const_iterator colorIt =
            std::find_if(filament.colors.begin(), filament.colors.end(),
                         [&selectedSku](const FilamentColorItem& colorItem)
                         {
                             return colorItem.sku == selectedSku;
                         });
        if (colorIt != filament.colors.end())
        {
            const std::vector<std::string> officialColors = ColorListFromLibrary(*colorIt);
            const std::string name = GetLocalizedColorName(*colorIt, languageCode, officialColors);
            if (!name.empty())
                return name;
        }
    }

    if (colorData.colors.size() > 1)
        return _L("Multiple Color").ToStdString(wxConvUTF8);
    return colorData.PrimaryColor("#26A69A");
}

/**
 * @brief Converts a color to HSV for sorting.
 */
SortHsv MakeSortHsvFromHex(const std::string& color)
{
    wxColour wxColor(color);
    if (!wxColor.IsOk())
        return SortHsv();

    SortHsv hsv;
    RGB2HSV(static_cast<float>(wxColor.Red()) / 255.0f, static_cast<float>(wxColor.Green()) / 255.0f,
            static_cast<float>(wxColor.Blue()) / 255.0f, &hsv.h, &hsv.s, &hsv.v);
    if (hsv.h < 0.0f)
        hsv.h += 360.0f;
    return hsv;
}

/**
 * @brief Gets the display type order used by the color grid.
 */
int GetColorTypeOrder(const std::vector<std::string>& colors, FilamentColorMode mode)
{
    if (colors.size() <= 1)
        return 0;
    return mode == FilamentColorMode::Gradient ? 2 : 1;
}

/**
 * @brief Compares two HSV values.
 */
int CompareHsv(const SortHsv& left, const SortHsv& right)
{
    if (left.h != right.h)
        return left.h < right.h ? -1 : 1;
    if (left.s != right.s)
        return left.s < right.s ? -1 : 1;
    if (left.v != right.v)
        return left.v < right.v ? -1 : 1;
    return 0;
}

/**
 * @brief Gets a language-specific name used as the final sort tie breaker.
 */
std::string GetColorNameForSort(const FilamentColorItem& colorItem, const std::string& languageCode)
{
    std::unordered_map<std::string, std::string>::const_iterator name = colorItem.colorNames.find(languageCode);
    return name != colorItem.colorNames.end() ? name->second : std::string();
}

/**
 * @brief Builds a lightweight, precomputed sort key.
 */
ColorSortItem MakeColorSortItem(const FilamentColorItem& colorItem)
{
    ColorSortItem item;
    item.colorItem = &colorItem;
    item.colors = ColorListFromLibrary(colorItem);
    item.typeOrder = GetColorTypeOrder(item.colors, colorItem.colorData.NormalizedMode());
    item.hsvColors.reserve(item.colors.size());
    for (const std::string& hexColor : item.colors)
        item.hsvColors.emplace_back(MakeSortHsvFromHex(hexColor));
    item.enName = GetColorNameForSort(colorItem, "en");
    item.zhName = GetColorNameForSort(colorItem, "zh_CN");
    return item;
}

/**
 * @brief Compares two precomputed sort keys.
 */
bool ColorSortItemLess(const ColorSortItem& left, const ColorSortItem& right)
{
    if (left.colorItem == nullptr)
        return false;
    if (right.colorItem == nullptr)
        return true;

    if (left.typeOrder != right.typeOrder)
        return left.typeOrder < right.typeOrder;

    const size_t color_count = std::min(left.hsvColors.size(), right.hsvColors.size());
    for (size_t index = 0; index < color_count; ++index)
    {
        const int hsv_compare = CompareHsv(left.hsvColors[index], right.hsvColors[index]);
        if (hsv_compare != 0)
            return hsv_compare < 0;
    }

    if (left.colors.size() != right.colors.size())
        return left.colors.size() < right.colors.size();
    if (left.colorItem->sku != right.colorItem->sku)
        return left.colorItem->sku < right.colorItem->sku;
    if (left.enName != right.enName)
        return left.enName < right.enName;
    return left.zhName < right.zhName;
}

/**
 * @brief Builds sorted lightweight items without copying full color records.
 */
std::vector<ColorSortItem> BuildSortedColorItems(const std::vector<FilamentColorItem>& colorItems)
{
    std::vector<ColorSortItem> items;
    items.reserve(colorItems.size());
    for (const FilamentColorItem& colorItem : colorItems)
        items.emplace_back(MakeColorSortItem(colorItem));

    std::stable_sort(items.begin(), items.end(), ColorSortItemLess);
    return items;
}

/**
 * @brief Gets the light swatch border color used by the official color dialog.
 */
wxColour FilamentColorLightBorderColor()
{
    return StateColor::darkModeColorFor(wxColour(180, 180, 180));
}

wxBitmap MakePreviewBitmap(const FilamentColor& colorData, int size, const std::string& split, double angleDeg);

// The editor's cross-section model: colours (#RRGGBB, strand order), proportions in percent, rotation.
MultiColour::Filament MakeFilamentModel(const std::vector<std::string>& colors, const std::array<int, 4>& percent,
                                        double angleDeg)
{
    MultiColour::Filament f;
    f.n = std::clamp<int>(int(colors.size()), 1, MultiColour::MAX_COLOURS);
    float sum = 0.f;
    for (int i = 0; i < f.n; ++i)
        sum += float(percent[size_t(i)]);
    for (int i = 0; i < f.n; ++i)
    {
        MultiColour::RGB c{ 0.15f, 0.65f, 0.6f };
        MultiColour::parse_hex(colors[size_t(i)], c);
        f.colour[size_t(i)] = MultiColour::srgb_to_linear(c);
        f.proportion[size_t(i)] = sum > 0.f ? float(percent[size_t(i)]) / sum : 1.f / float(f.n);
    }
    f.rotation_deg = float(angleDeg);
    return f;
}

// The editor's gradient model (P6): colours (#RRGGBB, palette order), the dwell and the transition in mm
// (negative = the default).
MultiColour::Filament MakeGradientModel(const std::vector<std::string>& colors, double dwellMm, double transitionMm)
{
    MultiColour::Filament f;
    std::vector<MultiColour::RGB> palette;
    for (const std::string& colour : colors)
    {
        MultiColour::RGB c{ 0.15f, 0.65f, 0.6f };
        MultiColour::parse_hex(colour, c);
        palette.push_back(MultiColour::srgb_to_linear(c));
    }
    if (palette.size() >= 2)
    {
        const std::vector<MultiColour::RGB> stops =
            MultiColour::resample_cyclic_palette(palette, std::min<int>(int(palette.size()), MultiColour::GRADIENT_MAX_STOPS));
        f.gradient_stops = int(stops.size());
        for (size_t i = 0; i < stops.size(); ++i)
            f.gradient[i] = stops[i];
    }
    else if (!palette.empty())
        f.colour[0] = palette.front();
    f.dwell_mm = MultiColour::gradient_dwell_mm(dwellMm);
    f.transition_mm = MultiColour::gradient_transition_mm(transitionMm);
    return f;
}

/**
 * @brief A colour chip of the make-up editor; a click asks for a new colour.
 */
class MakeUpChip : public wxPanel
{
public:
    MakeUpChip(wxWindow* parent, const wxSize& size, std::function<void()> onClick, std::function<void()> onDoubleClick)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , _onClick(std::move(onClick))
        , _onDoubleClick(std::move(onDoubleClick))
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &MakeUpChip::OnPaint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&)
        {
            if (_onClick)
                _onClick();
        });
        Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&)
        {
            if (_onDoubleClick)
                _onDoubleClick();
        });
    }

    void SetChipColour(const wxColour& colour)
    {
        _colour = colour;
        Refresh();
    }

    /** The chip the palette and the picker change. */
    void SetActive(bool active)
    {
        if (_active == active)
            return;
        _active = active;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.Clear();
        dc.SetPen(wxPen(FilamentColorLightBorderColor(), 1));
        dc.SetBrush(wxBrush(_colour.IsOk() ? _colour : wxColour("#26A69A")));
        dc.DrawRoundedRectangle(0, 0, size.GetWidth(), size.GetHeight(), FromDIP(4));
        if (_active)
        {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(wxColour("#019687"), 2));
            dc.DrawRoundedRectangle(1, 1, size.GetWidth() - 2, size.GetHeight() - 2, FromDIP(4));
        }
    }

    std::function<void()> _onClick;
    std::function<void()> _onDoubleClick;
    bool _active { false };
    wxColour _colour;
};

/**
 * @brief The filament end-on (a pie) or, with `wall`, a printed cylinder seen from above (a ring).
 * The front of the printer is at the bottom (a small arrow); dragging the disc turns colour 1.
 */
class MakeUpDiscPanel : public wxPanel
{
public:
    MakeUpDiscPanel(wxWindow* parent, int size, bool wall, std::function<MultiColour::Filament()> model,
                    std::function<void(double)> onRotate)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
        , _wall(wall)
        , _model(std::move(model))
        , _onRotate(std::move(onRotate))
    {
        const wxSize panelSize(size, size + FromDIP(8));
        SetMinSize(panelSize);
        SetMaxSize(panelSize);
        SetSize(panelSize);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &MakeUpDiscPanel::OnPaint, this);
        if (!wall)
        {
            SetCursor(wxCursor(wxCURSOR_HAND));
            Bind(wxEVT_LEFT_DOWN, &MakeUpDiscPanel::OnMouse, this);
            Bind(wxEVT_MOTION, [this](wxMouseEvent& event)
            {
                if (event.Dragging() && event.LeftIsDown())
                    OnMouse(event);
            });
        }
    }

private:
    int Side() const
    {
        return std::max(2, std::min(GetClientSize().GetWidth(), GetClientSize().GetHeight() - FromDIP(8)));
    }

    void OnMouse(wxMouseEvent& event)
    {
        const double half = 0.5 * double(Side());
        const float dx = float(event.GetX() - half), dy = float(event.GetY() - half);
        if (std::abs(dx) + std::abs(dy) < 2.f || !_onRotate)
            return;
        double deg = double(MultiColour::direction_turns(dx, -dy)) * 360.0;
        deg = std::round(deg / 5.0) * 5.0;
        if (deg >= 360.0)
            deg -= 360.0;
        _onRotate(deg);
    }

    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxColour bg = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
        dc.SetBackground(wxBrush(bg));
        dc.Clear();
        if (!_model)
            return;
        const MultiColour::Filament filament = _model();
        const int side = Side();
        const float radius = 0.5f * float(side) - 1.f;
        const float inner = _wall ? 0.6f * radius : 0.f;
        wxImage image(side, side, false);
        unsigned char* data = image.GetData();
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x)
            {
                float r = 0.f, g = 0.f, b = 0.f;
                for (int sy = 0; sy < 2; ++sy)
                    for (int sx = 0; sx < 2; ++sx)
                    {
                        const float px = float(x) + 0.25f + 0.5f * float(sx) - 0.5f * float(side);
                        const float py = float(y) + 0.25f + 0.5f * float(sy) - 0.5f * float(side);
                        const float d = std::sqrt(px * px + py * py);
                        if (d > radius || d < inner)
                        {
                            r += bg.Red() / 255.f; g += bg.Green() / 255.f; b += bg.Blue() / 255.f;
                            continue;
                        }
                        const MultiColour::RGB c = MultiColour::linear_to_srgb(
                            filament.colour_at(filament.filament_angle(MultiColour::direction_turns(px, -py), 0.f)));
                        r += c[0]; g += c[1]; b += c[2];
                    }
                unsigned char* p = data + 3 * (size_t(y) * size_t(side) + size_t(x));
                p[0] = (unsigned char) std::clamp(int(std::lround(r * 0.25f * 255.f)), 0, 255);
                p[1] = (unsigned char) std::clamp(int(std::lround(g * 0.25f * 255.f)), 0, 255);
                p[2] = (unsigned char) std::clamp(int(std::lround(b * 0.25f * 255.f)), 0, 255);
            }
        dc.DrawBitmap(wxBitmap(image), 0, 0, false);
        // The front of the printer: a small arrow under the disc.
        const int cx = side / 2, top = side + 1, bottom = side + FromDIP(7);
        const wxPoint arrow[3] = { wxPoint(cx - FromDIP(4), bottom), wxPoint(cx + FromDIP(4), bottom), wxPoint(cx, top) };
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(wxColour("#8A8A8A"))));
        dc.DrawPolygon(3, arrow);
    }

    bool _wall { false };
    std::function<MultiColour::Filament()> _model;
    std::function<void(double)> _onRotate;
};

/**
 * @brief The gradient palette laid out along the filament: two cycles side by side, so the wrap from the
 * last colour back to the first shows. CPU-drawn with MultiColour::Filament::gradient_colour().
 */
class MakeUpGradientStrip : public wxPanel
{
public:
    MakeUpGradientStrip(wxWindow* parent, const wxSize& size, std::function<MultiColour::Filament()> model)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , _model(std::move(model))
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &MakeUpGradientStrip::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.Clear();
        const wxSize size = GetClientSize();
        if (!_model || size.GetWidth() < 2 || size.GetHeight() < 2)
            return;
        const MultiColour::Filament filament = _model();
        wxImage image(size.GetWidth(), size.GetHeight(), false);
        unsigned char* data = image.GetData();
        for (int x = 0; x < size.GetWidth(); ++x)
        {
            const float cycles = 2.f * (float(x) + 0.5f) / float(size.GetWidth());
            const MultiColour::RGB c = MultiColour::linear_to_srgb(filament.gradient_colour(cycles * filament.gradient_cycle_mm() / 1000.f));
            const unsigned char r = (unsigned char) std::clamp(int(std::lround(c[0] * 255.f)), 0, 255);
            const unsigned char g = (unsigned char) std::clamp(int(std::lround(c[1] * 255.f)), 0, 255);
            const unsigned char b = (unsigned char) std::clamp(int(std::lround(c[2] * 255.f)), 0, 255);
            for (int y = 0; y < size.GetHeight(); ++y)
            {
                unsigned char* p = data + 3 * (size_t(y) * size_t(size.GetWidth()) + size_t(x));
                p[0] = r; p[1] = g; p[2] = b;
            }
        }
        dc.DrawBitmap(wxBitmap(image), 0, 0, false);
        dc.SetPen(wxPen(FilamentColorLightBorderColor(), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());
    }

    std::function<MultiColour::Filament()> _model;
};

wxFont DialogFont(wxFontWeight weight)
{
    wxFont font = Label::Body_14;
    font.SetWeight(weight);
    return font;
}

/**
 * @brief Small swatch used by the filament color dialog.
 */
class FilamentColorSwatch : public wxPanel
{
public:
    FilamentColorSwatch(wxWindow* parent, const FilamentColorItem& colorItem,
                        const std::string& languageCode, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , _colorItem(&colorItem)
        , _languageCode(languageCode)
    {
        SetMinSize(size);
        SetMaxSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        const std::vector<std::string> colors = ColorListFromLibrary(colorItem);
        const std::string tooltip = GetLocalizedColorName(colorItem, languageCode, colors);
        if (!tooltip.empty())
            SetToolTip(FromStdString(tooltip));
        Bind(wxEVT_PAINT, &FilamentColorSwatch::OnPaint, this);
    }

    /**
     * @brief Updates the selected visual state.
     */
    void SetSelected(bool selected)
    {
        if (_selected == selected)
            return;
        _selected = selected;
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
        dc.Clear();

        if (_colorItem == nullptr)
            return;

        const int selectedBorderWidth = std::max(FromDIP(1), 2);
        const int selectedGap = FromDIP(1);
        const int fillInset = selectedBorderWidth + selectedGap;
        const int fillWidth = std::max(1, size.GetWidth() - fillInset * 2);
        const int fillHeight = std::max(1, size.GetHeight() - fillInset * 2);
        const wxRect fillRect(fillInset, fillInset, fillWidth, fillHeight);
        FilamentColor colorData;
        if (!MakeFilamentColor(*_colorItem, colorData))
            return;

        const int bitmapWidth = fillRect.GetWidth();
        const int bitmapHeight = fillRect.GetHeight();
        const wxColour borderColor = FilamentColorLightBorderColor();
        wxBitmap* bitmap = FilamentColorUtils::GetFilamentColorIcon(colorData.colors, colorData.NormalizedMode(), "",
                                                                    bitmapWidth, bitmapHeight, borderColor);
        if (bitmap != nullptr)
            dc.DrawBitmap(*bitmap, fillRect.GetLeft(), fillRect.GetTop(), true);

        if (_selected)
        {
            const int middleHeight = std::max(0, size.GetHeight() - selectedBorderWidth * 2);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour("#019687")));
            dc.DrawRectangle(0, 0, size.GetWidth(), selectedBorderWidth);
            dc.DrawRectangle(0, selectedBorderWidth, selectedBorderWidth, middleHeight);
            dc.DrawRectangle(size.GetWidth() - selectedBorderWidth, selectedBorderWidth,
                             selectedBorderWidth, middleHeight);
            dc.DrawRectangle(0, size.GetHeight() - selectedBorderWidth, size.GetWidth(), selectedBorderWidth);
        }
    }

private:
    const FilamentColorItem* _colorItem { nullptr };
    std::string _languageCode;
    bool _selected { false };
};

/**
 * @brief Tertiary button used for the custom color entry.
 */
class MoreColorPanel : public wxPanel
{
public:
    MoreColorPanel(wxWindow* parent, const wxSize& size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
        , _icon(this, "filament_color_more", 16)
    {
        SetMinSize(size);
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &MoreColorPanel::OnPaint, this);
    }

private:
    void OnPaint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#F0F0F0"))));
        dc.Clear();

        dc.SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#242424")));
        const wxString label = _L("Other Colors");
        wxCoord textWidth = 0;
        wxCoord textHeight = 0;
        dc.GetTextExtent(label, &textWidth, &textHeight);
        const int iconWidth = _icon.bmp().IsOk() ? _icon.GetBmpWidth() : FromDIP(16);
        const int iconHeight = _icon.bmp().IsOk() ? _icon.GetBmpHeight() : FromDIP(16);
        const int gap = FromDIP(4);
        const int contentWidth = iconWidth + gap + static_cast<int>(textWidth);
        const int iconX = std::max(0, (size.GetWidth() - contentWidth) / 2);
        const int iconY = std::max(0, (size.GetHeight() - iconHeight) / 2);
        const int textX = iconX + iconWidth + gap;
        const int textY = std::max(0, (size.GetHeight() - static_cast<int>(textHeight)) / 2);

        if (_icon.bmp().IsOk())
        {
            dc.DrawBitmap(_icon.bmp(), iconX, iconY, true);
        }
        else
        {
            dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#8A8A8A")), FromDIP(2)));
            dc.DrawLine(iconX + iconWidth / 2, iconY + FromDIP(4),
                        iconX + iconWidth / 2, iconY + iconHeight - FromDIP(4));
            dc.DrawLine(iconX + FromDIP(4), iconY + iconHeight / 2,
                        iconX + iconWidth - FromDIP(4), iconY + iconHeight / 2);
        }
        dc.DrawText(label, textX, textY);
    }

private:
    ScalableBitmap _icon;
};

wxBitmap MakePreviewBitmap(const FilamentColor& colorData, int size, const std::string& split, double angleDeg)
{
    size = std::max(1, size);
    wxBitmap output(size, size);
    wxMemoryDC dc;
    dc.SelectObject(output);
    dc.SetBackground(wxBrush(StateColor::darkModeColorFor(wxColour("#FFFFFF"))));
    dc.Clear();

    const wxColour borderColor = FilamentColorLightBorderColor();
    const FilamentColorMode colorMode = colorData.NormalizedMode();
    wxBitmap* bitmap = colorData.colors.size() > 1 && colorMode != FilamentColorMode::Gradient ?
        FilamentColorUtils::GetSplitFilamentColorIcon(colorData.colors, split, angleDeg, "", size, size, borderColor) :
        FilamentColorUtils::GetFilamentColorIcon(colorData.colors, colorMode, "", size, size, borderColor);
    if (bitmap != nullptr)
        dc.DrawBitmap(*bitmap, 0, 0, true);

    dc.SelectObject(wxNullBitmap);
    return output;
}

wxRegion MakeRoundedRegion(const wxSize& size, int radius)
{
    const int width = std::max(1, size.GetWidth());
    const int height = std::max(1, size.GetHeight());
    radius = std::max(0, std::min(radius, std::min(width, height) / 2));
    if (radius == 0)
        return wxRegion(0, 0, width, height);

    constexpr double pi = 3.14159265358979323846;
    const int segments = 8;
    std::vector<wxPoint> points;
    points.reserve((segments + 1) * 4);

    auto appendArc = [&points, radius, segments, pi](int centerX, int centerY, double start, double end)
    {
        for (int index = 0; index <= segments; ++index)
        {
            const double ratio = static_cast<double>(index) / static_cast<double>(segments);
            const double angle = (start + (end - start) * ratio) * pi / 180.0;
            const int x = centerX + static_cast<int>(std::round(std::cos(angle) * radius));
            const int y = centerY + static_cast<int>(std::round(std::sin(angle) * radius));
            points.emplace_back(x, y);
        }
    };

    appendArc(width - radius - 1, radius, -90.0, 0.0);
    appendArc(width - radius - 1, height - radius - 1, 0.0, 90.0);
    appendArc(radius, height - radius - 1, 90.0, 180.0);
    appendArc(radius, radius, 180.0, 270.0);
    return wxRegion(static_cast<int>(points.size()), points.data());
}

bool IsValidWindowRect(const wxRect& rect)
{
    return rect.GetWidth() > 0 && rect.GetHeight() > 0;
}

wxRect WindowClientScreenRect(const wxWindow* window)
{
    if (window == nullptr)
        return wxRect();

    const wxSize size = window->GetClientSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        return wxRect();

    return wxRect(window->ClientToScreen(wxPoint(0, 0)), size);
}

} // namespace

FilamentColorDialog::FilamentColorDialog(wxWindow* parent, const FilamentColorInfo& filament,
                                         const FilamentColor& currentColor,
                                         const FilamentColorUtils::ColourMakeUp& currentMakeUp)
    : DPIDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFRAME_SHAPED)
    , _filament(filament)
    , _languageCode(GetLanguageCode())
    , _makeUp(currentMakeUp)
{
    const FilamentColor normalizedCurrent =
        FilamentColor::FromColors(currentColor.colors, currentColor.mode, "#26A69A");
    std::vector<FilamentColorItem>::const_iterator currentColorIt =
        FindColorBySavedColors(_filament, normalizedCurrent);

    if (currentColorIt != _filament.colors.end() && MakeFilamentColor(*currentColorIt, _selection))
    {
        _selectedSku = currentColorIt->sku;
        _highlightSku = currentColorIt->sku;
    }
    else
    {
        _selectedSku.clear();
        _highlightSku.clear();
        _selection = normalizedCurrent;
    }

    _gradient = _selection.NormalizedMode() == FilamentColorMode::Gradient;
    SyncMakeUpFromSelection(false);
    BuildUi();
    RefreshMakeUp();
    UpdateSwatchSelection();
    // The make-up rows differ by kind, so the dialog was fitted to the current one: place it again.
    PlaceNearFilamentPanel();
}

void FilamentColorDialog::BuildUi()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));

    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    const int dialogWidth = FromDIP(380);
    _dialogWidth = dialogWidth;
    const int contentMargin = FromDIP(16);
    const int buttonMargin = FromDIP(20);
    const int contentWidth = dialogWidth - contentMargin * 2;

    StaticBox* card = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    card->SetBackgroundColorNormal(wxColour("#FFFFFF"));
    card->SetCornerRadius(FromDIP(8));
    wxBoxSizer* cardSizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* currentRow = new wxBoxSizer(wxHORIZONTAL);
    _previewBitmap = new wxStaticBitmap(card, wxID_ANY, wxBitmap(FromDIP(60), FromDIP(60)));
    BindDragWindow(_previewBitmap);
    currentRow->Add(_previewBitmap, 0, wxALIGN_TOP | wxRIGHT, FromDIP(12));

    wxBoxSizer* infoSizer = new wxBoxSizer(wxVERTICAL);
    _nameLabel = new wxStaticText(card, wxID_ANY, wxEmptyString);
    _nameLabel->SetFont(DialogFont(wxFONTWEIGHT_SEMIBOLD));
    _nameLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
    _nameLabel->SetMinSize(wxSize(-1, FromDIP(20)));
    BindDragWindow(_nameLabel);
    _skuLabel = new wxStaticText(card, wxID_ANY, wxEmptyString);
    _skuLabel->SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));
    _skuLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
    _skuLabel->SetMinSize(wxSize(-1, FromDIP(20)));
    BindDragWindow(_skuLabel);
    infoSizer->Add(_nameLabel, 0, wxBOTTOM, FromDIP(6));
    infoSizer->Add(_skuLabel, 0);
    currentRow->Add(infoSizer, 1, wxALIGN_TOP);
    cardSizer->AddSpacer(FromDIP(28));
    cardSizer->Add(currentRow, 0, wxEXPAND | wxLEFT | wxRIGHT, contentMargin);

    // Without a library entry (a filament that has no official colours) only the make-up shows.
    if (!_filament.colors.empty())
    {
        wxStaticText* officialLabel = new wxStaticText(card, wxID_ANY, _L("Official Filaments"));
        officialLabel->SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));
        officialLabel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#242424")));
        officialLabel->SetMinSize(wxSize(-1, FromDIP(20)));
        BindDragWindow(officialLabel);
        cardSizer->AddSpacer(FromDIP(20));
        cardSizer->Add(officialLabel, 0, wxLEFT | wxRIGHT, contentMargin);

        wxPanel* swatchPanel = new wxPanel(card, wxID_ANY);
        swatchPanel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        const int swatchColumns = 10;
        const int swatchColorSize = FromDIP(24);
        const int swatchFrameInset = FromDIP(3);
        const wxSize swatchSize(swatchColorSize + swatchFrameInset * 2, swatchColorSize + swatchFrameInset * 2);
        const int swatchHGap = FromDIP(5);
        const int swatchVGap = 0;
        wxGridSizer* swatchSizer = new wxGridSizer(0, swatchColumns, swatchVGap, swatchHGap);
        const std::vector<ColorSortItem> sortedItems = BuildSortedColorItems(_filament.colors);
        for (const ColorSortItem& item : sortedItems)
        {
            if (item.colorItem == nullptr)
                continue;

            const FilamentColorItem* colorItem = item.colorItem;
            FilamentColorSwatch* swatch = new FilamentColorSwatch(swatchPanel, *colorItem, _languageCode, swatchSize);
            swatch->Bind(wxEVT_LEFT_UP, [this, colorItem](wxMouseEvent&)
            {
                if (colorItem != nullptr)
                    SelectFilamentColor(*colorItem);
            });
            swatchSizer->Add(swatch, 0);
            _swatchBySku.emplace_back(swatch, colorItem->sku);
        }
        const size_t swatchCount = _swatchBySku.size();
        const size_t swatchColumnCount = static_cast<size_t>(swatchColumns);
        int swatchRows = 0;
        if (swatchCount > 0)
            swatchRows = static_cast<int>((swatchCount + swatchColumnCount - 1) / swatchColumnCount);
        const int swatchVisibleRows = std::max(2, swatchRows);
        const int swatchPanelHeight = swatchVisibleRows * swatchSize.GetHeight() + (swatchVisibleRows - 1) * swatchVGap;
        swatchPanel->SetMinSize(wxSize(contentWidth, swatchPanelHeight));
        swatchPanel->SetSizer(swatchSizer);
        cardSizer->AddSpacer(FromDIP(20));
        cardSizer->Add(swatchPanel, 0, wxLEFT | wxRIGHT, contentMargin);
    }

    MoreColorPanel* moreButton = new MoreColorPanel(card, wxSize(contentWidth, FromDIP(40)));
    moreButton->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&)
    {
        OpenMoreColorDialog();
    });
    cardSizer->AddSpacer(FromDIP(20));
    cardSizer->Add(moreButton, 0, wxLEFT | wxRIGHT, contentMargin);

    cardSizer->AddSpacer(FromDIP(16));
    BuildMakeUpUi(card, cardSizer, contentMargin, contentWidth);

    cardSizer->AddSpacer(FromDIP(16));
    wxPanel* divider = new wxPanel(card, wxID_ANY);
    divider->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F0")));
    divider->SetMinSize(wxSize(dialogWidth, FromDIP(1)));
    BindDragWindow(divider);
    cardSizer->Add(divider, 0, wxEXPAND);

    wxPanel* buttonPanel = new wxPanel(card, wxID_ANY);
    buttonPanel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
    buttonPanel->SetMinSize(wxSize(dialogWidth, FromDIP(61)));
    BindDragWindow(buttonPanel);
    wxBoxSizer* buttonPanelSizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
    Button* cancel = new Button(buttonPanel, _L("Cancel"), wxEmptyString, wxBORDER_NONE, 0, wxID_CANCEL);
    const wxSize buttonSize(FromDIP(166), FromDIP(38));
    cancel->SetMinSize(buttonSize);
    cancel->SetSize(buttonSize);
    cancel->SetCornerRadius(FromDIP(4));
    cancel->SetBorderWidth(FromDIP(1));
    cancel->SetBackgroundColorNormal(wxColour("#FFFFFF"));
    cancel->SetBorderColorNormal(wxColour("#D1D5DC"));
    cancel->SetTextColorNormal(wxColour("#242424"));
    cancel->SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));
    Button* ok = new Button(buttonPanel, _L("OK"), wxEmptyString, wxBORDER_NONE, 0, wxID_OK);
    ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    ok->SetMinSize(buttonSize);
    ok->SetSize(buttonSize);
    ok->SetCornerRadius(FromDIP(4));
    ok->SetBackgroundColorNormal(wxColour("#019687"));
    ok->SetBorderColorNormal(wxColour("#019687"));
    ok->SetTextColorNormal(wxColour("#FFFFFF"));
    ok->SetFont(DialogFont(wxFONTWEIGHT_MEDIUM));

    buttons->Add(cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    buttons->Add(ok, 0, wxALIGN_CENTER_VERTICAL);
    buttonPanelSizer->AddSpacer(FromDIP(12));
    buttonPanelSizer->Add(buttons, 0, wxLEFT | wxRIGHT, buttonMargin);
    buttonPanel->SetSizer(buttonPanelSizer);
    cardSizer->Add(buttonPanel, 0, wxEXPAND);

    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        EndModal(wxID_CANCEL);
    });

    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&)
    {
        // The split string stays empty for the default (equal) split and for gradients.
        const int count = static_cast<int>(_selection.colors.size());
        if (count < 2 || count > MultiColour::MAX_COLOURS || _selection.NormalizedMode() == FilamentColorMode::Gradient)
            _makeUp.split.clear();
        else
        {
            std::array<float, MultiColour::MAX_COLOURS> proportion{ 0.f, 0.f, 0.f, 0.f };
            for (int i = 0; i < count; ++i)
                proportion[size_t(i)] = float(_percent[size_t(i)]) / 100.f;
            _makeUp.split = MultiColour::format_split(proportion, count);
        }
        RememberFilamentColours(_selection.colors);
        EndModal(wxID_OK);
    });

    card->SetSizer(cardSizer);
    BindDragWindow(card);
    BindDragWindow(this);
    root->Add(card, 0, wxEXPAND);
    SetSizer(root);
    root->SetSizeHints(this);
    SetClientSize(wxSize(dialogWidth, root->CalcMin().GetHeight()));
    SetMinClientSize(wxSize(dialogWidth, root->CalcMin().GetHeight()));
    UpdateRoundedShape();
    Layout();
    PlaceNearFilamentPanel();
    _uiBuilt = true;
}

void FilamentColorDialog::SelectFilamentColor(const FilamentColorItem& colorItem)
{
    FilamentColor colorData;
    if (!MakeFilamentColor(colorItem, colorData))
        return;

    _selection = colorData;
    _gradient = _selection.NormalizedMode() == FilamentColorMode::Gradient;
    _selectedSku = colorItem.sku;
    _highlightSku = colorItem.sku;
    // A library SKU may carry the proportions of a coextruded filament ("split"); the slot's
    // rotation and twist belong to the spool and stay as they are.
    _makeUp.split = colorItem.split;
    SyncMakeUpFromSelection(false);
    RefreshMakeUp();
    UpdateSwatchSelection();
}

void FilamentColorDialog::SelectCustomColor(const std::string& color)
{
    const std::string normalized = FilamentColorUtils::NormalizeHexColor(color, "#26A69A");
    _selection = FilamentColor::FromMultiColors(normalized, FilamentColorMode::Segment, normalized);
    _gradient = false;
    _selectedSku.clear();
    _highlightSku.clear();
    _makeUp.split.clear();
    SyncMakeUpFromSelection(false);
    RefreshMakeUp();
    UpdateSwatchSelection();
}

void FilamentColorDialog::UpdatePreview()
{
    if (_previewBitmap == nullptr || _nameLabel == nullptr || _skuLabel == nullptr)
        return;

    {
        const int count = std::clamp<int>(int(_selection.colors.size()), 1, MultiColour::MAX_COLOURS);
        std::array<float, MultiColour::MAX_COLOURS> proportion{ 0.f, 0.f, 0.f, 0.f };
        for (int i = 0; i < count; ++i)
            proportion[size_t(i)] = float(_percent[size_t(i)]) / 100.f;
        _previewBitmap->SetBitmap(MakePreviewBitmap(_selection, FromDIP(60),
                                                    IsGradient() ? std::string() : MultiColour::format_split(proportion, count),
                                                    IsGradient() ? 0.0 : _makeUp.angle));
    }

    const wxString name = FromStdString(GetSelectionDisplayName(_filament, _selection, _selectedSku, _languageCode));
    _nameLabel->SetLabel(name);
    _skuLabel->SetLabel(_selectedSku.empty() ? wxString(wxEmptyString) : FromStdString("sku " + _selectedSku));
    _skuLabel->Show(!_selectedSku.empty());
    Layout();
}

// ---- Colour make-up editor (multi-colour P2) ----

void FilamentColorDialog::BuildMakeUpUi(wxWindow* parent, wxSizer* sizer, int margin, int contentWidth)
{
    const wxColour textColour = StateColor::darkModeColorFor(wxColour("#242424"));
    auto makeLabel = [&](const wxString& text, wxFontWeight weight)
    {
        wxStaticText* label = new wxStaticText(parent, wxID_ANY, text);
        label->SetFont(DialogFont(weight));
        label->SetForegroundColour(textColour);
        BindDragWindow(label);
        return label;
    };
    auto makeSpin = [&](int min, int max, int initial, int step, int width, const wxString& unit)
    {
        return new SpinInput(parent, wxEmptyString, unit, wxDefaultPosition, wxSize(FromDIP(width), FromDIP(28)),
                             wxSP_ARROW_KEYS, min, max, initial, step);
    };

    sizer->Add(makeLabel(_L("Colour make-up"), wxFONTWEIGHT_MEDIUM), 0, wxLEFT | wxRIGHT, margin);
    _makeUpSizer = sizer;

    // Split (the colours side by side across the strand) or gradient (one after another along it).
    wxBoxSizer* kindRow = new wxBoxSizer(wxHORIZONTAL);
    kindRow->Add(makeLabel(_L("Type"), wxFONTWEIGHT_NORMAL), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    const struct { const char* label; bool gradient; const char* tip; } kinds[] = {
        { "Split", false, "The colours sit side by side across the filament, like slices of a pie." },
        { "Gradient", true, "The colour changes along the length of the filament and repeats." } };
    for (const auto& kind : kinds)
    {
        Button* button = new Button(parent, _L(kind.label), wxEmptyString, wxBORDER_NONE, 0, wxID_ANY);
        const wxSize buttonSize(FromDIP(84), FromDIP(26));
        button->SetMinSize(buttonSize);
        button->SetSize(buttonSize);
        button->SetCornerRadius(FromDIP(4));
        button->SetBorderWidth(FromDIP(1));
        button->SetFont(DialogFont(wxFONTWEIGHT_NORMAL));
        button->SetToolTip(_L(kind.tip));
        const bool gradient = kind.gradient;
        button->Bind(wxEVT_BUTTON, [this, gradient](wxCommandEvent&) { SetKind(gradient); });
        (gradient ? _gradientButton : _splitButton) = button;
        kindRow->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    }
    sizer->AddSpacer(FromDIP(8));
    sizer->Add(kindRow, 0, wxLEFT | wxRIGHT, margin);

    // Number of colours and the colour chips.
    wxBoxSizer* countRow = new wxBoxSizer(wxHORIZONTAL);
    countRow->Add(makeLabel(_L("Colours"), wxFONTWEIGHT_NORMAL), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    _countSpin = makeSpin(1, MultiColour::GRADIENT_MAX_STOPS, 2, 1, 78, wxEmptyString);
    _countSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&)
    {
        if (!_syncingMakeUp)
            SetColourCount(_countSpin->GetValue());
    });
    countRow->Add(_countSpin, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    for (int i = 0; i < MultiColour::GRADIENT_MAX_STOPS; ++i)
    {
        MakeUpChip* chip = new MakeUpChip(parent, wxSize(FromDIP(30), FromDIP(26)), [this, i]() { SelectChip(i); },
                                          [this, i]() { PickChipColour(i); });
        chip->SetToolTip(_L("Click to choose this colour from the palette below, double-click for the colour picker"));
        _chips[size_t(i)] = chip;
        countRow->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
    }
    sizer->AddSpacer(FromDIP(8));
    sizer->Add(countRow, 0, wxLEFT | wxRIGHT, margin);

    // The palette and the recent colours change the chip that is marked.
    std::vector<SwatchEntry> paletteEntries;
    for (const MultiColour::NamedColour& colour : MultiColour::basic_palette())
        paletteEntries.push_back({ colour.hex, colour.name });
    _palette = new ColourSwatchGrid(parent, 14, 4, 18, 3, [this](const std::string& hex) { SetChipColour(_activeChip, hex); });
    _palette->SetEntries(paletteEntries);
    sizer->AddSpacer(FromDIP(8));
    sizer->Add(_palette, 0, wxLEFT | wxRIGHT, margin);
    wxBoxSizer* recentRow = new wxBoxSizer(wxHORIZONTAL);
    recentRow->Add(makeLabel(_L("Recent"), wxFONTWEIGHT_NORMAL), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    _recent = new ColourSwatchGrid(parent, 8, 1, 18, 3, [this](const std::string& hex) { SetChipColour(_activeChip, hex); });
    std::vector<SwatchEntry> recentEntries;
    for (const std::string& hex : RecentFilamentColours())
        recentEntries.push_back({ hex, std::string() });
    _recent->SetEntries(recentEntries);
    recentRow->Add(_recent, 0, wxALIGN_CENTER_VERTICAL);
    sizer->AddSpacer(FromDIP(6));
    sizer->Add(recentRow, 0, wxLEFT | wxRIGHT, margin);

    // Gradient (P6): the palette along the filament, and how long one run through it is.
    wxBoxSizer* stripRow = new wxBoxSizer(wxVERTICAL);
    _strip = new MakeUpGradientStrip(parent, wxSize(contentWidth, FromDIP(22)),
                                     [this]() { return MakeGradientModel(_selection.colors, _makeUp.dwell, _makeUp.transition); });
    _strip->SetToolTip(_L("The colours along the filament, two cycles: each colour holds, then changes to the next. The last one changes back into the first."));
    stripRow->Add(_strip, 0);
    stripRow->Add(makeLabel(_L("Along the filament, two cycles"), wxFONTWEIGHT_NORMAL), 0, wxTOP, FromDIP(2));
    _stripRow = stripRow;
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(stripRow, 0, wxLEFT | wxRIGHT, margin);

    auto makeLengthRow = [&](const wxString& label, const wxString& tip, int initial, int step, SpinInput*& spin, wxSizer*& rowOut,
                             std::function<void(double)> store)
    {
        wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(makeLabel(label, wxFONTWEIGHT_NORMAL), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        spin = makeSpin(0, int(MultiColour::GRADIENT_LENGTH_MM_MAX), initial, step, 130, "mm");
        spin->SetToolTip(tip);
        SpinInput* control = spin;
        spin->Bind(wxEVT_SPINCTRL, [this, control, store](wxCommandEvent&)
        {
            if (_syncingMakeUp)
                return;
            store(double(control->GetValue()));
            if (_strip != nullptr)
                _strip->Refresh();
        });
        row->Add(spin, 0, wxALIGN_CENTER_VERTICAL);
        rowOut = row;
        sizer->AddSpacer(FromDIP(8));
        sizer->Add(row, 0, wxLEFT | wxRIGHT, margin);
    };
    makeLengthRow(_L("Colour length"),
                  _L("How many millimetres of filament each colour holds before it starts changing to the next. "
                     "Match it to your spool by comparing a print."),
                  int(MultiColour::GRADIENT_DWELL_MM_DEFAULT), 500, _dwellSpin, _cycleRow, [this](double v) { _makeUp.dwell = v; });
    makeLengthRow(_L("Transition length"),
                  _L("How many millimetres of filament the change from one colour to the next takes."),
                  int(MultiColour::GRADIENT_TRANSITION_MM_DEFAULT), 100, _transitionSpin, _transitionRow,
                  [this](double v) { _makeUp.transition = v; });

    // Proportions in 5% steps, at least 5% each.
    wxBoxSizer* percentRow = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < MultiColour::MAX_COLOURS; ++i)
    {
        SpinInput* spin = makeSpin(MultiColour::MIN_PERCENT, 100, _percent[size_t(i)], MultiColour::STEP_PERCENT, 78, "%");
        spin->SetToolTip(_L("Share of this colour in the filament (5% steps, at least 5%)"));
        spin->Bind(wxEVT_SPINCTRL, [this, i, spin](wxCommandEvent&)
        {
            if (!_syncingMakeUp)
                SetPercent(i, spin->GetValue());
        });
        _percentSpin[size_t(i)] = spin;
        percentRow->Add(spin, 0, wxRIGHT, FromDIP(6));
    }
    _percentRow = percentRow;
    sizer->AddSpacer(FromDIP(8));
    sizer->Add(percentRow, 0, wxLEFT | wxRIGHT, margin);

    // The filament end-on (drag to turn it) and the printed wall seen from above.
    wxBoxSizer* previewRow = new wxBoxSizer(wxHORIZONTAL);
    const int discSize = FromDIP(84);
    auto model = [this]() { return MakeFilamentModel(_selection.colors, _percent, _makeUp.angle); };
    wxBoxSizer* discColumn = new wxBoxSizer(wxVERTICAL);
    _disc = new MakeUpDiscPanel(parent, discSize, false, model, [this](double deg) { SetAngle(deg); });
    _disc->SetToolTip(_L("The filament end-on. The arrow marks the front of the printer. Drag to turn it."));
    discColumn->Add(_disc, 0, wxALIGN_CENTER_HORIZONTAL);
    discColumn->Add(makeLabel(_L("Filament"), wxFONTWEIGHT_NORMAL), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(2));
    wxBoxSizer* wallColumn = new wxBoxSizer(wxVERTICAL);
    _wall = new MakeUpDiscPanel(parent, discSize, true, model, nullptr);
    _wall->SetToolTip(_L("A printed wall seen from above: which colour faces which way on the plate."));
    wallColumn->Add(_wall, 0, wxALIGN_CENTER_HORIZONTAL);
    wallColumn->Add(makeLabel(_L("Wall from above"), wxFONTWEIGHT_NORMAL), 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(2));
    previewRow->AddStretchSpacer(1);
    previewRow->Add(discColumn, 0, wxRIGHT, FromDIP(28));
    previewRow->Add(wallColumn, 0);
    previewRow->AddStretchSpacer(1);
    _discRow = previewRow;
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(previewRow, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

    // Rotation: where colour 1 faces. 0 = the front, turning anticlockwise seen from above.
    wxBoxSizer* angleRow = new wxBoxSizer(wxHORIZONTAL);
    angleRow->Add(makeLabel(_L("Rotation"), wxFONTWEIGHT_NORMAL), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    _angleSpin = makeSpin(0, 355, 0, 5, 90, wxString::FromUTF8("\xC2\xB0"));
    _angleSpin->SetToolTip(_L("Where colour 1 faces on the printed wall: 0 = the front of the printer, counter-clockwise seen from above."));
    _angleSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&)
    {
        if (!_syncingMakeUp)
            SetAngle(double(_angleSpin->GetValue()));
    });
    angleRow->Add(_angleSpin, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    const struct { const char* label; double deg; } faces[] = { { "Front", 0. }, { "Right", 90. }, { "Back", 180. }, { "Left", 270. } };
    for (const auto& face : faces)
    {
        Button* button = new Button(parent, _L(face.label), wxEmptyString, wxBORDER_NONE, 0, wxID_ANY);
        const wxSize buttonSize(FromDIP(52), FromDIP(26));
        button->SetMinSize(buttonSize);
        button->SetSize(buttonSize);
        button->SetCornerRadius(FromDIP(4));
        button->SetBorderWidth(FromDIP(1));
        button->SetBackgroundColorNormal(wxColour("#FFFFFF"));
        button->SetBorderColorNormal(wxColour("#D1D5DC"));
        button->SetTextColorNormal(wxColour("#242424"));
        button->SetFont(DialogFont(wxFONTWEIGHT_NORMAL));
        const double deg = face.deg;
        button->Bind(wxEVT_BUTTON, [this, deg](wxCommandEvent&) { SetAngle(deg); });
        angleRow->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    }
    _angleRow = angleRow;
    sizer->AddSpacer(FromDIP(10));
    sizer->Add(angleRow, 0, wxLEFT | wxRIGHT, margin);

    // Twist (advanced), degrees per metre of filament.
    wxBoxSizer* twistRow = new wxBoxSizer(wxHORIZONTAL);
    twistRow->Add(makeLabel(_L("Twist"), wxFONTWEIGHT_NORMAL), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    _twistSpin = makeSpin(-3600, 3600, 0, 5, 110, wxString::FromUTF8("\xC2\xB0/m"));
    _twistSpin->SetToolTip(_L("Some feed paths turn the filament slowly while it prints, so the colours drift with height. Leave at 0 unless your prints show it."));
    _twistSpin->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&)
    {
        if (!_syncingMakeUp)
            _makeUp.twist = double(_twistSpin->GetValue());
    });
    twistRow->Add(_twistSpin, 0, wxALIGN_CENTER_VERTICAL);
    _twistRow = twistRow;
    sizer->AddSpacer(FromDIP(8));
    sizer->Add(twistRow, 0, wxLEFT | wxRIGHT, margin);

    wxStaticText* note = makeLabel(_L("Visual only: changes how the print looks in Showcase and Preview. The G-code is not affected."),
                                   wxFONTWEIGHT_NORMAL);
    note->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#4A4A4A")));
    note->Wrap(contentWidth);
    sizer->AddSpacer(FromDIP(8));
    sizer->Add(note, 0, wxLEFT | wxRIGHT, margin);
}

// Reads the proportions for the current colours from the make-up (equal when resetSplit or unusable).
void FilamentColorDialog::SyncMakeUpFromSelection(bool resetSplit)
{
    const int count = std::clamp<int>(int(_selection.colors.size()), 1, MultiColour::MAX_COLOURS);
    const std::array<float, MultiColour::MAX_COLOURS> proportion =
        MultiColour::parse_split(resetSplit ? std::string() : _makeUp.split, count);
    _percent = MultiColour::quantize_percent(proportion, count);
}

bool FilamentColorDialog::IsGradient() const
{
    return _gradient && _selection.colors.size() >= 2;
}

int FilamentColorDialog::MaxColours() const
{
    return _gradient ? MultiColour::GRADIENT_MAX_STOPS : MultiColour::MAX_COLOURS;
}

FilamentColor FilamentColorDialog::MakeSelection(const std::vector<std::string>& colors) const
{
    return FilamentColor::FromColors(colors, _gradient && colors.size() >= 2 ? FilamentColorMode::Gradient : FilamentColorMode::Segment);
}

void FilamentColorDialog::SetColourCount(int count)
{
    count = std::clamp(count, 1, MaxColours());
    if (count == static_cast<int>(_selection.colors.size()))
    {
        RefreshMakeUp();   // the spin may have gone past this kind's limit
        return;
    }
    static const char* const fresh[] = { "#E53935", "#1E88E5", "#43A047", "#FDD835", "#8E24AA" };
    std::vector<std::string> colors = _selection.colors;
    if (colors.empty())
        colors.emplace_back("#26A69A");
    while (static_cast<int>(colors.size()) > count)
        colors.pop_back();
    for (size_t i = colors.size(); static_cast<int>(i) < count; ++i)
        colors.emplace_back(fresh[i % 5]);
    _selection = MakeSelection(colors);
    _selectedSku.clear();
    _highlightSku.clear();
    _makeUp.split.clear();
    SyncMakeUpFromSelection(true);
    RefreshMakeUp();
    UpdateSwatchSelection();
}

// Split <-> gradient: the colours stay (a split holds at most 4, a gradient needs at least 2).
void FilamentColorDialog::SetKind(bool gradient)
{
    if (_gradient == gradient)
        return;
    _gradient = gradient;
    std::vector<std::string> colors = _selection.colors;
    if (colors.empty())
        colors.emplace_back("#26A69A");
    if (!gradient && static_cast<int>(colors.size()) > MultiColour::MAX_COLOURS)
        colors.resize(size_t(MultiColour::MAX_COLOURS));
    if (gradient && colors.size() < 2)
        colors.emplace_back("#1E88E5");
    _selection = MakeSelection(colors);
    _selectedSku.clear();
    _highlightSku.clear();
    _makeUp.split.clear();
    SyncMakeUpFromSelection(true);
    RefreshMakeUp();
    UpdateSwatchSelection();
    PlaceNearFilamentPanel();
}

void FilamentColorDialog::SetPercent(int index, int value)
{
    const int count = std::clamp<int>(int(_selection.colors.size()), 1, MultiColour::MAX_COLOURS);
    if (index < 0 || index >= count || count < 2)
    {
        RefreshMakeUp();
        return;
    }
    const int step = MultiColour::STEP_PERCENT, minimum = MultiColour::MIN_PERCENT;
    value = static_cast<int>(std::lround(double(value) / step)) * step;
    value = std::clamp(value, minimum, 100 - (count - 1) * minimum);
    int delta = value - _percent[size_t(index)];
    _percent[size_t(index)] = value;
    // The others give up (or take) the difference, in order after this one, never below the minimum.
    for (int k = 1; k < count && delta != 0; ++k)
    {
        const size_t j = size_t((index + k) % count);
        const int next = std::max(minimum, _percent[j] - delta);
        delta -= _percent[j] - next;
        _percent[j] = next;
    }
    if (delta != 0)
        _percent[size_t(index)] -= delta;
    RefreshMakeUp();
}

void FilamentColorDialog::SetAngle(double angleDeg)
{
    angleDeg = std::fmod(angleDeg, 360.0);
    if (angleDeg < 0.0)
        angleDeg += 360.0;
    _makeUp.angle = angleDeg;
    RefreshMakeUp();
}

void FilamentColorDialog::PickChipColour(int index)
{
    if (index < 0 || index >= static_cast<int>(_selection.colors.size()))
        return;
    SelectChip(index);
    FilamentColourPickerDialog dialog(this, _selection.colors[size_t(index)]);
    if (dialog.ShowModal() != wxID_OK)
        return;
    SetChipColour(index, dialog.Colour());
}

void FilamentColorDialog::SelectChip(int index)
{
    if (index < 0 || index >= static_cast<int>(_selection.colors.size()))
        return;
    _activeChip = index;
    RefreshMakeUp();
}

void FilamentColorDialog::SetChipColour(int index, const std::string& hex)
{
    const std::string normalized = FilamentColorUtils::NormalizeHexColor(hex, "");
    if (normalized.empty())
        return;
    std::vector<std::string> colors = _selection.colors;
    if (colors.empty())
        colors.emplace_back(normalized);
    if (index < 0 || index >= static_cast<int>(colors.size()))
        index = 0;
    const bool changed = colors[size_t(index)] != normalized;
    colors[size_t(index)] = normalized;
    _selection = MakeSelection(colors);
    if (changed)
    {
        _selectedSku.clear();
        _highlightSku.clear();
    }
    RefreshMakeUp();
    UpdateSwatchSelection();
}

// Pushes the data to the controls and repaints everything that shows the make-up.
void FilamentColorDialog::RefreshMakeUp()
{
    _syncingMakeUp = true;
    const bool gradient = IsGradient();
    const int count = std::clamp<int>(int(_selection.colors.size()), 1, gradient ? MultiColour::GRADIENT_MAX_STOPS : MultiColour::MAX_COLOURS);
    _activeChip = std::clamp(_activeChip, 0, count - 1);
    // The rows of the kind that is not chosen go away (before the individual controls below).
    if (_makeUpSizer != nullptr)
    {
        for (wxSizer* row : { _percentRow, _discRow, _angleRow, _twistRow })
            if (row != nullptr)
                _makeUpSizer->Show(row, !gradient, true);
        for (wxSizer* row : { _cycleRow, _transitionRow, _stripRow })
            if (row != nullptr)
                _makeUpSizer->Show(row, gradient, true);
    }
    // The kind that is chosen is the filled button.
    for (const auto& item : { std::make_pair(_splitButton, !_gradient), std::make_pair(_gradientButton, _gradient) })
        if (Button* button = dynamic_cast<Button*>(item.first))
        {
            button->SetBackgroundColorNormal(item.second ? wxColour("#019687") : wxColour("#FFFFFF"));
            button->SetBorderColorNormal(item.second ? wxColour("#019687") : wxColour("#D1D5DC"));
            button->SetTextColorNormal(item.second ? wxColour("#FFFFFF") : wxColour("#242424"));
            button->Refresh();
        }
    if (_countSpin != nullptr)
        _countSpin->SetValue(count);
    for (int i = 0; i < MultiColour::GRADIENT_MAX_STOPS; ++i)
    {
        const bool used = i < count;
        if (i < MultiColour::MAX_COLOURS)
            if (SpinInput* spin = _percentSpin[size_t(i)])
            {
                spin->Show(used && count > 1 && !gradient);
                if (used && !gradient)
                    spin->SetValue(_percent[size_t(i)]);
            }
        if (wxWindow* chip = _chips[size_t(i)])
        {
            chip->Show(used);
            if (used)
            {
                static_cast<MakeUpChip*>(chip)->SetChipColour(wxColour(_selection.colors[size_t(i)]));
                static_cast<MakeUpChip*>(chip)->SetActive(i == _activeChip);
            }
        }
    }
    if (_palette != nullptr && !_selection.colors.empty())
        _palette->SetSelected(_selection.colors[size_t(_activeChip)]);
    if (_recent != nullptr && !_selection.colors.empty())
        _recent->SetSelected(_selection.colors[size_t(_activeChip)]);
    if (_angleSpin != nullptr)
        _angleSpin->SetValue(static_cast<int>(std::lround(_makeUp.angle / 5.0)) % 72 * 5);
    if (_twistSpin != nullptr)
        _twistSpin->SetValue(static_cast<int>(std::lround(_makeUp.twist)));
    if (_dwellSpin != nullptr)
        _dwellSpin->SetValue(static_cast<int>(std::lround(MultiColour::gradient_dwell_mm(_makeUp.dwell))));
    if (_transitionSpin != nullptr)
        _transitionSpin->SetValue(static_cast<int>(std::lround(MultiColour::gradient_transition_mm(_makeUp.transition))));
    _syncingMakeUp = false;
    if (_disc != nullptr)
        _disc->Refresh();
    if (_wall != nullptr)
        _wall->Refresh();
    if (_strip != nullptr)
        _strip->Refresh();
    UpdatePreview();
    Layout();
    if (_uiBuilt)
        ResizeToContent();
}

// Fits the dialog to the rows that are showing (the split and gradient kinds have different ones).
void FilamentColorDialog::ResizeToContent()
{
    if (GetSizer() == nullptr || _dialogWidth <= 0)
        return;
    const wxSize client(_dialogWidth, GetSizer()->CalcMin().GetHeight());
    if (GetClientSize() == client && GetMinClientSize() == client)
        return;
    SetMinClientSize(wxDefaultSize);
    SetClientSize(client);
    SetMinClientSize(client);
    UpdateRoundedShape();
    Layout();
}

void FilamentColorDialog::UpdateSwatchSelection()
{
    for (const std::pair<wxWindow*, std::string>& item : _swatchBySku)
    {
        FilamentColorSwatch* swatch = dynamic_cast<FilamentColorSwatch*>(item.first);
        if (swatch != nullptr)
            swatch->SetSelected(!_highlightSku.empty() && item.second == _highlightSku);
    }
}

void FilamentColorDialog::on_dpi_changed(const wxRect&)
{
    Fit();
    UpdateRoundedShape();
    PlaceNearFilamentPanel();
}

void FilamentColorDialog::UpdateRoundedShape()
{
    const wxSize size = GetSize();
    if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
        return;

    SetShape(MakeRoundedRegion(size, FromDIP(8)));
}

void FilamentColorDialog::BindDragWindow(wxWindow* window)
{
    if (window == nullptr)
        return;

    window->Bind(wxEVT_LEFT_DOWN, &FilamentColorDialog::StartDrag, this);
    window->Bind(wxEVT_MOTION, &FilamentColorDialog::DragDialog, this);
    window->Bind(wxEVT_LEFT_UP, &FilamentColorDialog::EndDrag, this);
}

void FilamentColorDialog::StartDrag(wxMouseEvent& event)
{
    _dragPending = true;
    _isDragging = false;
    _dragStartMouse = wxGetMousePosition();
    _dragStartPosition = GetScreenPosition();
    event.Skip();
}

void FilamentColorDialog::DragDialog(wxMouseEvent& event)
{
    if (!_dragPending && !_isDragging)
    {
        event.Skip();
        return;
    }

    if (!event.LeftIsDown())
    {
        _dragPending = false;
        _isDragging = false;
        if (HasCapture())
            ReleaseMouse();
        event.Skip();
        return;
    }

    const wxPoint currentMouse = wxGetMousePosition();
    if (!_isDragging)
    {
        const int threshold = FromDIP(3);
        const int distanceX = std::abs(currentMouse.x - _dragStartMouse.x);
        const int distanceY = std::abs(currentMouse.y - _dragStartMouse.y);
        if (distanceX < threshold && distanceY < threshold)
        {
            event.Skip();
            return;
        }

        _isDragging = true;
        if (!HasCapture())
            CaptureMouse();
    }

    if (!_isDragging)
    {
        event.Skip();
        return;
    }

    Move(wxPoint(_dragStartPosition.x + currentMouse.x - _dragStartMouse.x,
                 _dragStartPosition.y + currentMouse.y - _dragStartMouse.y));
}

void FilamentColorDialog::EndDrag(wxMouseEvent& event)
{
    if (_dragPending || _isDragging)
    {
        _dragPending = false;
        _isDragging = false;
        if (HasCapture())
            ReleaseMouse();
    }
    event.Skip();
}

void FilamentColorDialog::PlaceNearFilamentPanel()
{
    wxWindow* parent = GetParent();
    wxWindow* topWindow = parent != nullptr ? wxGetTopLevelParent(parent) : nullptr;
    const wxRect parentRect = WindowClientScreenRect(parent);
    const wxRect topRect = WindowClientScreenRect(topWindow);
    const wxSize dialogSize = GetSize();

    wxRect horizontalAnchor = parentRect;
    wxRect verticalAnchor = parentRect;
    const int maxAnchorRight = IsValidWindowRect(topRect) ? topRect.GetLeft() + topRect.GetWidth() / 2 : 0;
    const int minPanelWidth = FromDIP(220);
    const int minPanelHeight = FromDIP(80);

    for (wxWindow* window = parent; window != nullptr && window != topWindow; window = window->GetParent())
    {
        const wxRect rect = WindowClientScreenRect(window);
        if (!IsValidWindowRect(rect))
            continue;

        const bool isLeftPanel = !IsValidWindowRect(topRect) || rect.GetRight() <= maxAnchorRight;
        if (isLeftPanel && (!IsValidWindowRect(horizontalAnchor) || rect.GetRight() >= horizontalAnchor.GetRight()))
            horizontalAnchor = rect;

        const bool containsParent = IsValidWindowRect(parentRect) &&
            rect.GetTop() <= parentRect.GetTop() && rect.GetBottom() >= parentRect.GetBottom();
        if (containsParent && rect.GetWidth() >= minPanelWidth && rect.GetHeight() >= minPanelHeight)
        {
            verticalAnchor = rect;
            break;
        }
    }

    const wxPoint fallbackPosition = topWindow != nullptr ? topWindow->ClientToScreen(wxPoint(0, 0)) : wxPoint(0, 0);
    int x = IsValidWindowRect(horizontalAnchor) ? horizontalAnchor.GetRight() + FromDIP(8) : fallbackPosition.x;
    int y = IsValidWindowRect(verticalAnchor) ? verticalAnchor.GetTop() : fallbackPosition.y + FromDIP(100);

    const int displayIndex = wxDisplay::GetFromWindow(parent != nullptr ? parent : this);
    const wxDisplay display(displayIndex == wxNOT_FOUND ? 0 : static_cast<unsigned int>(displayIndex));
    const wxRect area = display.IsOk() ? display.GetClientArea() : wxRect(fallbackPosition, wxSize(1920, 1080));
    x = std::max(area.GetLeft() + FromDIP(8), std::min(x, area.GetRight() - dialogSize.GetWidth() - FromDIP(8)));
    y = std::max(area.GetTop() + FromDIP(8), std::min(y, area.GetBottom() - dialogSize.GetHeight() - FromDIP(8)));
    Move(wxPoint(x, y));
}

void FilamentColorDialog::OpenMoreColorDialog()
{
    // The colour picker for the marked chip (or the only colour).
    const int chip = std::clamp(_activeChip, 0, std::max(0, int(_selection.colors.size()) - 1));
    const std::string start = _selection.colors.empty() ? std::string("#26A69A") : _selection.colors[size_t(chip)];
    FilamentColourPickerDialog dialog(this, start);
    if (dialog.ShowModal() != wxID_OK)
        return;
    SetChipColour(chip, dialog.Colour());
}

} // namespace GUI
} // namespace Slic3r
