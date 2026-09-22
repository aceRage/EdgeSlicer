#include "SegmentedToggle.hpp"

#include <wx/sizer.h>

#include "Button.hpp"
#include "StaticBox.hpp"
#include "../GUI_App.hpp"

namespace
{

// --- Visual style (Figma specs) ---
constexpr int g_containerHeight  = 28;  // DIP — container height
constexpr int g_containerRadius  = 4;   // DIP
constexpr int g_buttonMinWidth   = 80;  // DIP
constexpr int g_buttonMinHeight  = 28;  // DIP
constexpr int g_buttonPaddingW   = 6;   // DIP
constexpr int g_buttonPaddingH   = 2;   // DIP
constexpr int g_buttonRadius     = 4;   // DIP
constexpr int g_buttonGap        = 4;   // DIP — gap between buttons
constexpr int g_buttonMarginV    = 0;   // DIP — vertical button margin (container already sized for button)
constexpr int g_outerMargin      = 6;   // DIP — (block 40 - container 28) / 2

// Unselected: transparent bg (container's #F8F7F7 shows through), #4A4A4A text
// Selected:   #009688 bg, #FEFEFE text
constexpr const char* g_containerBg    = "#F8F7F7";
constexpr const char* g_unselectedFg   = "#4A4A4A";
constexpr const char* g_selectedBg     = "#009688";
constexpr const char* g_selectedFg     = "#FEFEFE";

// Plain style: no container / no fill; text-only, colored to indicate selection.
constexpr const char* g_plainSelectedFg   = "#009688"; // teal
constexpr const char* g_plainUnselectedFg = "#6B6B6B"; // grey
constexpr int g_plainButtonGap = 8; // DIP — gap between text options

// Pill style (Bambu-style segmented control): rounded container slightly offset
// from the page background; the selected segment is a solid accent-filled
// rounded button, inactive segments are plain grey text with a subtle hover
// fill. All colors go through the StateColor dark map (see StateColor.cpp).
constexpr int g_pillContainerRadius   = 14; // DIP — fully rounded container
constexpr int g_pillButtonMinHeight   = 24; // DIP — container 28 minus 2x margin
constexpr int g_pillButtonRadius      = 12; // DIP — rounded selected button
constexpr int g_pillButtonMarginV     = 2;  // DIP — vertical inset inside the container
constexpr const char* g_pillContainerBg = "#E9E9E9"; // -> #34343A in dark mode
constexpr const char* g_pillHoverBg     = "#DDDDDD"; // -> #40404A in dark mode
constexpr const char* g_pillSelectedFg  = "#FEFEFE"; // near-white in both modes

} // namespace

namespace Slic3r
{
namespace GUI
{

SegmentedToggle::SegmentedToggle(wxWindow* parent,
                                 const std::vector<wxString>& options,
                                 int selectedIndex,
                                 Style style)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_style(style)
    , m_selectedIndex(selectedIndex)
    // Plain style: pure text, no visible box. The buttons still paint a filled rect
    // (so each repaint overwrites old pixels -> no ghosting), but in the SAME color as
    // the parent background, so the rect is invisible and it reads as text on the page.
    , m_unselectedBg(style == Style::Plain
          ? StateColor(std::pair(parent->GetBackgroundColour(), (int)StateColor::Normal))
          : StateColor())
    , m_unselectedFg(StateColor(std::pair(wxColour(style == Style::Plain ? g_plainUnselectedFg : g_unselectedFg), (int)StateColor::Normal)))
    , m_selectedBg(style == Style::Plain
          ? StateColor(std::pair(parent->GetBackgroundColour(), (int)StateColor::Normal))
          : StateColor(std::pair(wxColour(g_selectedBg), (int)StateColor::Normal)))
    , m_selectedFg(StateColor(std::pair(wxColour(style == Style::Plain ? g_plainSelectedFg : g_selectedFg), (int)StateColor::Normal)))
{
    SetBackgroundColour(style == Style::Plain
        ? parent->GetBackgroundColour()
        : StateColor::darkModeColorFor(wxColour("#FFFFFF")));

    auto* outerSizer = new wxBoxSizer(wxVERTICAL);

    const bool plain = (m_style == Style::Plain);
    const bool pill  = (m_style == Style::Pill);

    // Pill style re-colors the buttons below (before the first applyButtonColors).
    if (pill) {
        const wxColour containerColour(g_pillContainerBg);
        m_unselectedBg = StateColor(
            std::pair(wxColour(g_pillHoverBg), (int)StateColor::Hovered),
            std::pair(containerColour, (int)StateColor::Normal));
        m_unselectedFg = StateColor(std::pair(wxColour(g_unselectedFg), (int)StateColor::Normal));
        m_selectedBg   = StateColor(
            std::pair(wxColour(g_selectedBg), (int)StateColor::Pressed),
            std::pair(wxColour(g_selectedBg), (int)StateColor::Hovered),
            std::pair(wxColour(g_selectedBg), (int)StateColor::Normal));
        m_selectedFg   = StateColor(std::pair(wxColour(g_pillSelectedFg), (int)StateColor::Normal));
    }

    // Boxed/Pill styles parent the buttons inside a rounded container; plain
    // style has no container and lays the text buttons directly on the panel.
    wxWindow* btnParent = this;
    if (!plain) {
        m_pContainer = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
        m_pContainer->SetCornerRadius(FromDIP(pill ? g_pillContainerRadius : g_containerRadius));
        m_pContainer->SetBorderWidth(0);
        m_pContainer->SetMinSize(wxSize(-1, FromDIP(g_containerHeight)));
        m_pContainer->SetBackgroundColor(
            StateColor(std::pair(wxColour(pill ? g_pillContainerBg : g_containerBg), (int)StateColor::Normal)));
        btnParent = m_pContainer;
    }

    auto* btnSizer = new wxBoxSizer(wxHORIZONTAL);

    wxFont plainFont = Label::Body_14;
    plainFont.MakeBold();

    for (int i = 0; i < (int)options.size(); ++i) {
        if (i > 0)
            btnSizer->AddSpacer(FromDIP(plain ? g_plainButtonGap : g_buttonGap));

        auto* btn = new Button(btnParent, options[i]);
        btn->SetBorderWidth(0);
        if (plain) {
            btn->SetFont(plainFont);
            btn->SetPaddingSize(wxSize(0, 0));
            btn->SetCornerRadius(0);
        } else {
            btn->SetMinSize(wxSize(FromDIP(g_buttonMinWidth), FromDIP(pill ? g_pillButtonMinHeight : g_buttonMinHeight)));
            btn->SetPaddingSize(wxSize(FromDIP(g_buttonPaddingW), FromDIP(g_buttonPaddingH)));
            btn->SetCornerRadius(FromDIP(pill ? g_pillButtonRadius : g_buttonRadius));
            btn->SetFont(Label::Body_12);
        }

        m_buttons.push_back(btn);
        applyButtonColors(i, i == m_selectedIndex);

        btn->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) {
            onButtonClicked(i);
        });

        if (plain)
            btnSizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL);
        else
            btnSizer->Add(btn, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(pill ? g_pillButtonMarginV : g_buttonMarginV));
    }

    if (plain) {
        outerSizer->Add(btnSizer, 0, wxALL, FromDIP(g_buttonPaddingH));
    } else {
        m_pContainer->SetSizer(btnSizer);
        outerSizer->Add(m_pContainer, 1, wxEXPAND | wxALL, FromDIP(g_outerMargin));
    }
    SetSizer(outerSizer);
    Layout();
}

void SegmentedToggle::applyButtonColors(int index, bool selected)
{
    if (index < 0 || index >= (int)m_buttons.size())
        return;
    Button* btn = m_buttons[index];
    btn->SetBackgroundColor(selected ? m_selectedBg : m_unselectedBg);
    btn->SetTextColor(selected ? m_selectedFg : m_unselectedFg);
    btn->SetCanFocus(!selected);
}

void SegmentedToggle::setSelected(int index)
{
    if (index < 0 || index >= (int)m_buttons.size() || index == m_selectedIndex)
        return;

    applyButtonColors(m_selectedIndex, false);
    m_selectedIndex = index;
    applyButtonColors(m_selectedIndex, true);
}

int SegmentedToggle::getSelected() const
{
    return m_selectedIndex;
}

void SegmentedToggle::bindSelectionCallback(SelectionCallback cb)
{
    m_selectionCallback = std::move(cb);
}

void SegmentedToggle::onButtonClicked(int index)
{
    if (index == m_selectedIndex)
        return;

    setSelected(index);

    if (m_selectionCallback)
        m_selectionCallback(m_selectedIndex);
}

} // namespace GUI
} // namespace Slic3r
