#pragma once

// Sizing rule for the two extruder cards of DualNozzleSliceDialog, kept free of wx so it can be
// unit tested (slic3rutils_tests, [DualNozzleLayout]). All values are device pixels.

#include <algorithm>

namespace Slic3r { namespace GUI { namespace DualNozzleLayout {

struct CardBodies
{
    int  height{ 0 };     // height given to BOTH card bodies (equal, so the cards and the swap button line up)
    bool scroll{ false }; // the bodies scroll instead of showing every row
};

// natural_left / natural_right: the height the card bodies need to show every row at its natural
// height; chrome_height: the dialog's outer height with the bodies at zero height (title bar,
// printer row, status, hint, tip, issues, buttons, card titles and borders); max_dialog_height:
// the cap (80% of the display's client area); min_body: the height of an empty card body.
// Rows are never shrunk: when everything fits the bodies get the taller natural height,
// otherwise they get what is left under the cap (never less than min_body) and scroll.
inline CardBodies card_bodies(int natural_left, int natural_right, int chrome_height, int max_dialog_height, int min_body)
{
    const int want      = std::max(std::max(natural_left, natural_right), min_body);
    const int available = std::max(min_body, max_dialog_height - chrome_height);
    if (want <= available)
        return { want, false };
    return { available, true };
}

// 80% of the display's client-area height.
inline int max_dialog_height(int display_client_height) { return display_client_height * 4 / 5; }

}}} // namespace Slic3r::GUI::DualNozzleLayout
