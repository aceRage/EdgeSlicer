#pragma once

#include <wx/colour.h>

// Accent colours of the Bambu Device page (StatusPanel, its PrintingTaskPanel and the widgets only
// that page uses, such as TempInput). They are Bambu Studio's brand green, copied from
// ThemeColor::BrandGreen / BrandGreenHovered / BrandGreenPressed in Bambu Studio's
// src/slic3r/GUI/Widgets/StateColor.hpp, so a Bambu printer's page looks like it does in Bambu
// Studio. Every other page keeps the app accent (#009688). The dark-mode twins (#21A452, #37B865,
// #1C8A46, also Bambu Studio's) are in gDarkColors in Widgets/StateColor.cpp.
namespace Slic3r { namespace GUI { namespace BambuDevicePalette {

inline const wxColour Green{0x00, 0xAE, 0x42};        // #00AE42  buttons, progress bar, percent
inline const wxColour GreenHovered{0x3D, 0xCB, 0x73}; // #3DCB73  button hover
inline const wxColour GreenPressed{0x1B, 0x88, 0x44}; // #1B8844  button pressed

// Grey Bambu Studio uses for the stage line ("Printing") and the finish-time label.
inline const wxColour SecondaryText{107, 107, 107};   // #6B6B6B

}}} // namespace Slic3r::GUI::BambuDevicePalette
