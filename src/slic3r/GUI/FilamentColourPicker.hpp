#pragma once

#include "GUI_Utils.hpp"

#include <wx/panel.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

class SpinInput;
class wxTextCtrl;

namespace Slic3r
{
namespace GUI
{

/**
 * @brief The last colours picked for filaments, newest first ("#RRGGBB", at most 8). Kept in the
 * app config, never in the project.
 */
std::vector<std::string> RecentFilamentColours();

/**
 * @brief Puts colours at the front of the recent list (the first colour ends up first).
 */
void RememberFilamentColours(const std::vector<std::string>& colours);

struct SwatchEntry
{
    std::string hex;   // "#RRGGBB"
    std::string name;  // shown as the tooltip
};

/**
 * @brief A grid of colour swatches with a name on hover. Light and dark themes, DPI scaled.
 */
class ColourSwatchGrid : public wxPanel
{
public:
    ColourSwatchGrid(wxWindow* parent, int columns, int rows, int swatchDip, int gapDip,
                     std::function<void(const std::string&)> onPick);

    void SetEntries(const std::vector<SwatchEntry>& entries);
    void SetSelected(const std::string& hex);

private:
    wxRect CellRect(size_t index) const;
    int HitTest(const wxPoint& point) const;
    void OnPaint(wxPaintEvent&);
    void OnMotion(wxMouseEvent& event);
    void OnLeftUp(wxMouseEvent& event);

    int _columns { 1 };
    int _rows { 1 };
    int _swatch { 16 };
    int _gap { 2 };
    int _hover { -1 };
    std::string _selected;
    std::vector<SwatchEntry> _entries;
    std::function<void(const std::string&)> _onPick;
};

/**
 * @brief The colour picker: the named palette, the recent colours, a hue / saturation area with a
 * value slider, a hex field and R/G/B, and a button for the system colour dialog.
 */
class FilamentColourPickerDialog : public DPIDialog
{
public:
    FilamentColourPickerDialog(wxWindow* parent, const std::string& initialHex);

    /** The chosen colour, "#RRGGBB". */
    std::string Colour() const;

private:
    void on_dpi_changed(const wxRect&) override { Fit(); }
    void SetFromRgb(int r, int g, int b);
    void SetFromHsv(double hue, double saturation, double value);
    void SyncControls();
    void ApplyHexText();
    void OpenSystemPicker();

    int _rgb[3] { 0, 0, 0 };
    double _hue { 0.0 };
    double _sat { 0.0 };
    double _val { 0.0 };
    bool _syncing { false };
    wxWindow* _plane { nullptr };
    wxWindow* _valueSlider { nullptr };
    wxWindow* _preview { nullptr };
    wxTextCtrl* _hexText { nullptr };
    std::array<SpinInput*, 3> _rgbSpin { nullptr, nullptr, nullptr };
    ColourSwatchGrid* _palette { nullptr };
    ColourSwatchGrid* _recent { nullptr };
};

} // namespace GUI
} // namespace Slic3r
