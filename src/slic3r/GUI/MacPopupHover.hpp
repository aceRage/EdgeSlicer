#pragma once

// The printer list popup (SelectMachinePopup) draws a row's pencil (name / access code) and log-out
// icons only while the mouse is over the row. From macOS 13.1 the rows of that popup no longer get
// mouse enter / leave events, so on those versions the icons are drawn all the time instead.
//
// The version test this replaces read "major >= 13 && minor >= 1 && micro >= 0", which is false on
// every x.0 release after 13 (14.0, 15.0, 26.0, 27.0): on a current Mac the icons were never drawn
// and the pencil could not be found.

namespace Slic3r {
namespace GUI {

constexpr bool macos_popup_icons_always_drawn(int major, int minor) { return major > 13 || (major == 13 && minor >= 1); }

} // namespace GUI
} // namespace Slic3r
