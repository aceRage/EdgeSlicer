#pragma once

// Which icons the printer list popup (SelectMachinePopup) draws on a "My devices" row.
//
// The pencil edits the printer's name and, for a LAN printer, its saved access code
// (EditDevNameDialog); the log-out icon unbinds / forgets it.
//
//   * Cloud printer: log-out always; the pencil while the printer is online.
//   * LAN printer:   the pencil whenever an access code is saved, online or not, free or not. A
//                    printer that refuses the saved code (LOCK row) or cannot be reached is exactly
//                    where the code has to be re-entered. Log-out stays limited to a printer that
//                    is online, holds a code and is free (has_access_right() && is_avaliable()).
//
// The icons are drawn all the time (not only under the mouse) on every platform; hovering only
// adds the highlight frame.

namespace Slic3r {
namespace GUI {

struct RowEditIcons
{
    bool pencil{false};
    bool logout{false};
    bool operator==(const RowEditIcons &o) const { return pencil == o.pencil && logout == o.logout; }
};

// has_saved_code is MachineObject::has_access_right() (a saved access code); available is
// MachineObject::is_avaliable() (bind_state == "free").
constexpr RowEditIcons edit_icons_for_row(bool lan, bool online, bool has_saved_code, bool available)
{
    if (!lan)
        return {online, true};
    return {has_saved_code, online && has_saved_code && available};
}

} // namespace GUI
} // namespace Slic3r
