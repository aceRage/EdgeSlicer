#include "FilamentColorUtils.hpp"

#include "MixedFilamentBadge.hpp"
#include "BitmapCache.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Showcase/MultiColourFilament.hpp"

#include <wx/dcmemory.h>
#include <wx/image.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace Slic3r
{
namespace GUI
{
namespace FilamentColorUtils
{
namespace
{

std::vector<std::string> NormalizedColorsOrFallback(const std::vector<std::string>& colors,
                                                    const std::string& fallback_color)
{
    std::vector<std::string> normalized;
    normalized.reserve(colors.size());
    for (const std::string& color : colors)
    {
        const std::string normalized_color = NormalizeHexColor(color);
        if (!normalized_color.empty())
            normalized.emplace_back(normalized_color);
    }

    if (normalized.empty())
    {
        const std::string fallback = NormalizeHexColor(fallback_color, "#26A69A");
        normalized.emplace_back(fallback.empty() ? "#26A69A" : fallback);
    }

    return normalized;
}

void AddFallbackToNormalizedColors(std::vector<std::string>& colors, const std::string& fallbackColor)
{
    if (!colors.empty())
        return;

    const std::string fallback = NormalizeHexColor(fallbackColor, "#26A69A");
    colors.emplace_back(fallback.empty() ? "#26A69A" : fallback);
}

wxColour WxColorFromHex(const std::string& color)
{
    wxColour parsed(color);
    return parsed.IsOk() ? parsed : wxColour("#26A69A");
}

wxBitmap* GetFilamentColorIconFromNormalized(const std::vector<std::string>& normalizedColors, FilamentColorMode mode,
                                             const std::string& label, int iconWidth, int iconHeight,
                                             const wxColour& lightBorderColor,
                                             const std::string* split = nullptr, double angleDeg = 0.0)
{
    if (split != nullptr && normalizedColors.size() > 1 && mode != FilamentColorMode::Gradient)
        return GetSplitFilamentColorIcon(normalizedColors, *split, angleDeg, label, iconWidth, iconHeight, lightBorderColor);

    std::vector<wxColour> wxColors;
    wxColors.reserve(normalizedColors.size());
    for (const std::string& color : normalizedColors)
        wxColors.emplace_back(WxColorFromHex(color));

    const bool isGradient = normalizedColors.size() > 1 && mode == FilamentColorMode::Gradient;
    return get_color_block_bitmap_cached(wxColors, isGradient, iconWidth, iconHeight, wxString::FromUTF8(label.c_str()),
                                         lightBorderColor);
}

} // namespace

std::string NormalizeHexColor(const std::string& color)
{
    return NormalizeFilamentHexColor(color);
}

std::string NormalizeHexColor(const std::string& color, const std::string& fallback_color)
{
    return NormalizeFilamentHexColor(color, fallback_color);
}

std::string StripHashForPreprint(const std::string& color)
{
    std::string normalized = NormalizeHexColor(color);
    if (!normalized.empty() && normalized.front() == '#')
        normalized.erase(normalized.begin());
    return normalized;
}

std::string StripHashForPreprint(const std::string& color, const std::string& fallback_color)
{
    std::string normalized = NormalizeHexColor(color, fallback_color);
    if (!normalized.empty() && normalized.front() == '#')
        normalized.erase(normalized.begin());
    return normalized;
}

std::vector<std::string> SplitMultiColors(const std::string& value)
{
    return SplitFilamentMultiColors(value);
}

std::string JoinMultiColors(const std::vector<std::string>& colors)
{
    return JoinFilamentMultiColors(colors);
}

std::string GetPrimaryColor(const std::vector<std::string>& colors, const std::string& fallback_color)
{
    const FilamentColor color = FilamentColor::FromColors(colors, FilamentColorMode::Segment, fallback_color);
    return color.PrimaryColor(fallback_color);
}

std::string GetFilamentMatchName(const std::string& name)
{
    return Slic3r::GetFilamentMatchName(name);
}

FilamentColor GetFilamentColorFromConfig(const DynamicPrintConfig* config, size_t colorIndex, const std::string& fallbackColor)
{
    std::vector<std::string> colors;
    FilamentColorMode mode = FilamentColorMode::Segment;

    if (config != nullptr && config->has("filament_multi_colors"))
    {
        const ConfigOptionStrings* option = config->option<ConfigOptionStrings>("filament_multi_colors");
        if (option != nullptr && option->values.size() > colorIndex)
            colors = SplitMultiColors(option->values[colorIndex]);
    }

    if (config != nullptr && config->has("filament_colour_mode"))
    {
        const ConfigOptionInts* option = config->option<ConfigOptionInts>("filament_colour_mode");
        if (option != nullptr && option->values.size() > colorIndex)
            mode = FilamentColorModeFromConfig(option->values[colorIndex]);
    }

    return FilamentColor::FromColors(colors, mode, fallbackColor);
}

nlohmann::json BuildPreprintColorMultiItem(const std::string& multiColors, FilamentColorMode mode,
                                           const std::string& fallbackColor)
{
    const FilamentColor color = FilamentColor::FromMultiColors(multiColors, mode, fallbackColor);

    nlohmann::json out_colors = nlohmann::json::array();
    for (const std::string& item : color.colors)
        out_colors.push_back(item.substr(1));

    nlohmann::json item;
    item["mode"] = FilamentColorModeToConfig(color.NormalizedMode());
    item["nums"] = color.colors.size();
    item["colors"] = out_colors;
    return item;
}

wxBitmap* GetFilamentColorIcon(const std::vector<std::string>& colors, FilamentColorMode mode, const std::string& label,
                               int iconWidth, int iconHeight, const wxColour& lightBorderColor)
{
    const std::vector<std::string> normalized = NormalizedColorsOrFallback(colors, "#26A69A");
    return GetFilamentColorIconFromNormalized(normalized, mode, label, iconWidth, iconHeight, lightBorderColor);
}

wxBitmap* GetFilamentColorIcon(const std::string& multiColors, FilamentColorMode mode, const std::string& fallbackColor,
                               const std::string& label, int iconWidth, int iconHeight, const wxColour& lightBorderColor,
                               const std::string* split, double angleDeg)
{
    std::vector<std::string> normalized = SplitMultiColors(multiColors);
    AddFallbackToNormalizedColors(normalized, fallbackColor);
    return GetFilamentColorIconFromNormalized(normalized, mode, label, iconWidth, iconHeight, lightBorderColor, split, angleDeg);
}

wxBitmap* GetSplitFilamentColorIcon(const std::vector<std::string>& colors, const std::string& split, double angleDeg,
                                    const std::string& label, int iconWidth, int iconHeight,
                                    const wxColour& lightBorderColor)
{
    wxASSERT(wxIsMainThread());
    static BitmapCache cache;

    const std::vector<std::string> normalized = NormalizedColorsOrFallback(colors, "#26A69A");
    const int count = std::min<int>(int(normalized.size()), MultiColour::MAX_COLOURS);
    iconWidth  = std::max(1, iconWidth);
    iconHeight = std::max(1, iconHeight);

    MultiColour::Filament filament;
    filament.n          = count;
    filament.proportion = MultiColour::parse_split(split, count);
    filament.rotation_deg = float(angleDeg);

    std::string key = "split:h" + std::to_string(iconHeight) + ":w" + std::to_string(iconWidth) + ":" + label +
                      ":a" + std::to_string(int(std::lround(angleDeg))) + ":s" + MultiColour::format_split(filament.proportion, count);
    for (int i = 0; i < count; ++i)
        key += ":" + normalized[size_t(i)];
    if (lightBorderColor.IsOk())
        key += ":border:" + lightBorderColor.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    if (wxBitmap* cached = cache.find(key))
        return cached;

    std::array<wxColour, MultiColour::MAX_COLOURS> swatchColours;
    for (int i = 0; i < count; ++i)
        swatchColours[size_t(i)] = WxColorFromHex(normalized[size_t(i)]);

    // 2x2 supersampling keeps the divider and the pie edges smooth at icon size.
    wxImage image(iconWidth, iconHeight, false);
    unsigned char* data = image.GetData();
    const float hw = 0.5f * float(iconWidth), hh = 0.5f * float(iconHeight);
    for (int y = 0; y < iconHeight; ++y)
        for (int x = 0; x < iconWidth; ++x)
        {
            int r = 0, g = 0, b = 0;
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx)
                {
                    const float px = float(x) + 0.25f + 0.5f * float(sx) - hw;
                    const float py = float(y) + 0.25f + 0.5f * float(sy) - hh;
                    const wxColour& c = swatchColours[size_t(MultiColour::swatch_slice(filament, px, py, hw, hh))];
                    r += c.Red(); g += c.Green(); b += c.Blue();
                }
            unsigned char* p = data + 3 * (size_t(y) * size_t(iconWidth) + size_t(x));
            p[0] = (unsigned char) (r / 4);
            p[1] = (unsigned char) (g / 4);
            p[2] = (unsigned char) (b / 4);
        }

    wxBitmap bmp(image);
    wxMemoryDC dc;
    dc.SelectObject(bmp);
    bool veryLight = false;
    double luminance = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const wxColour& c = swatchColours[size_t(i)];
        if (c.Red() > 224 && c.Green() > 224 && c.Blue() > 224)
            veryLight = true;
        luminance += c.GetLuminance();
    }
    luminance /= double(count);
    if (veryLight)
    {
        dc.SetPen(lightBorderColor.IsOk() ? wxPen(lightBorderColor, 1) : *wxGREY_PEN);
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, iconWidth, iconHeight);
    }
    if (!label.empty())
    {
        dc.SetFont(std::min(iconWidth, iconHeight) < 20 ? ::Label::Body_8 : ::Label::Body_12);
        dc.SetTextForeground(luminance < 0.51 ? *wxWHITE : *wxBLACK);
        dc.DrawLabel(wxString::FromUTF8(label.c_str()), wxRect(0, 0, iconWidth, iconHeight),
                     wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL);
    }
    dc.SelectObject(wxNullBitmap);
    return cache.insert(key, bmp);
}

ColourMakeUp GetColourMakeUp(const DynamicPrintConfig* config, size_t slot)
{
    ColourMakeUp out;
    if (config == nullptr)
        return out;
    if (const ConfigOptionStrings* split = config->option<ConfigOptionStrings>("filament_colour_split"))
        if (slot < split->values.size())
            out.split = split->values[slot];
    if (const ConfigOptionFloats* angle = config->option<ConfigOptionFloats>("filament_colour_angle"))
        if (slot < angle->values.size())
            out.angle = angle->values[slot];
    if (const ConfigOptionFloats* twist = config->option<ConfigOptionFloats>("filament_colour_twist"))
        if (slot < twist->values.size())
            out.twist = twist->values[slot];
    if (const ConfigOptionFloats* dwell = config->option<ConfigOptionFloats>("filament_gradient_dwell"))
        if (slot < dwell->values.size())
            out.dwell = dwell->values[slot];
    if (const ConfigOptionFloats* transition = config->option<ConfigOptionFloats>("filament_gradient_transition"))
        if (slot < transition->values.size())
            out.transition = transition->values[slot];
    return out;
}

void SetColourMakeUp(DynamicPrintConfig& config, size_t slot, const ColourMakeUp& makeUp)
{
    const size_t size = slot + 1;
    auto* split = config.option<ConfigOptionStrings>("filament_colour_split", true);
    auto* angle = config.option<ConfigOptionFloats>("filament_colour_angle", true);
    auto* twist = config.option<ConfigOptionFloats>("filament_colour_twist", true);
    auto* dwell = config.option<ConfigOptionFloats>("filament_gradient_dwell", true);
    auto* transition = config.option<ConfigOptionFloats>("filament_gradient_transition", true);
    if (split->values.size() < size)
        split->values.resize(size);
    if (angle->values.size() < size)
        angle->values.resize(size, 0.);
    if (twist->values.size() < size)
        twist->values.resize(size, 0.);
    if (dwell->values.size() < size)
        dwell->values.resize(size, -1.);
    if (transition->values.size() < size)
        transition->values.resize(size, -1.);
    split->values[slot] = makeUp.split;
    angle->values[slot] = makeUp.angle;
    twist->values[slot] = makeUp.twist;
    dwell->values[slot] = makeUp.dwell;
    transition->values[slot] = makeUp.transition;
}

} // namespace FilamentColorUtils
} // namespace GUI
} // namespace Slic3r
