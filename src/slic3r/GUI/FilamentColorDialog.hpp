#pragma once

#include "GUI_Utils.hpp"
#include "FilamentColorUtils.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

class wxStaticBitmap;
class wxStaticText;
class SpinInput;

namespace Slic3r
{
namespace GUI
{

class ColourSwatchGrid;

/**
 * @brief Dialog for choosing a built-in filament color or a custom color.
 */
class FilamentColorDialog : public DPIDialog
{
public:
    /**
     * @brief Creates the filament color dialog.
     */
    FilamentColorDialog(wxWindow* parent, const FilamentColorInfo& filament, const FilamentColor& currentColor,
                        const FilamentColorUtils::ColourMakeUp& currentMakeUp = FilamentColorUtils::ColourMakeUp());

    /**
     * @brief Gets the colour make-up (proportions, rotation, twist) chosen in the editor. Visual only.
     */
    const FilamentColorUtils::ColourMakeUp& MakeUp() const
    {
        return _makeUp;
    }

    /**
     * @brief Gets the selected color result.
     */
    const FilamentColor& Selection() const
    {
        return _selection;
    }

private:
    void BuildUi();
    void SelectFilamentColor(const FilamentColorItem& colorItem);
    void SelectCustomColor(const std::string& color);
    void UpdatePreview();
    void UpdateSwatchSelection();
    void BuildMakeUpUi(wxWindow* parent, wxSizer* sizer, int contentMargin, int contentWidth);
    void SyncMakeUpFromSelection(bool resetSplit);
    void SetColourCount(int count);
    void SetPercent(int index, int value);
    void SetAngle(double angleDeg);
    void PickChipColour(int index);
    void SelectChip(int index);
    void SetChipColour(int index, const std::string& hex);
    void RefreshMakeUp();
    // Glow in the dark (G1): the section under the make-up.
    void BuildGlowUi(wxWindow* parent, wxSizer* sizer, int contentMargin);
    void SetGlowState(int state);
    void PickGlowColour();
    void ResetGlowColour();
    void RefreshGlow();
    void OpenMoreColorDialog();
    void PlaceNearFilamentPanel();
    void UpdateRoundedShape();
    void BindDragWindow(wxWindow* window);
    void StartDrag(wxMouseEvent& event);
    void DragDialog(wxMouseEvent& event);
    void EndDrag(wxMouseEvent& event);
    void on_dpi_changed(const wxRect& suggestedRect) override;

private:
    FilamentColorInfo _filament;
    std::string _languageCode;
    FilamentColor _selection;
    std::string _selectedSku;
    std::string _highlightSku;
    std::vector<std::pair<wxWindow*, std::string>> _swatchBySku;
    wxStaticBitmap* _previewBitmap { nullptr };
    wxStaticText* _nameLabel { nullptr };
    wxStaticText* _skuLabel { nullptr };
    // Colour make-up editor (multi-colour P2). The colours live in _selection.colors.
    FilamentColorUtils::ColourMakeUp _makeUp;
    std::array<int, 4> _percent { 50, 50, 0, 0 };
    SpinInput* _countSpin { nullptr };
    std::array<SpinInput*, 4> _percentSpin { nullptr, nullptr, nullptr, nullptr };
    std::array<wxWindow*, 4> _chips { nullptr, nullptr, nullptr, nullptr };
    SpinInput* _angleSpin { nullptr };
    SpinInput* _twistSpin { nullptr };
    // Glow in the dark (G1): Automatic / On / Off, the glow colour, the project-wide intensity.
    std::array<wxWindow*, 3> _glowButtons { nullptr, nullptr, nullptr };
    wxWindow* _glowChip { nullptr };
    wxWindow* _glowColourReset { nullptr };
    wxStaticText* _glowHint { nullptr };
    SpinInput* _glowIntensitySpin { nullptr };
    ColourSwatchGrid* _palette { nullptr };
    ColourSwatchGrid* _recent { nullptr };
    int _activeChip { 0 };
    wxWindow* _disc { nullptr };
    wxWindow* _wall { nullptr };
    bool _syncingMakeUp { false };
    bool _dragPending { false };
    bool _isDragging { false };
    wxPoint _dragStartMouse;
    wxPoint _dragStartPosition;
};

} // namespace GUI
} // namespace Slic3r
