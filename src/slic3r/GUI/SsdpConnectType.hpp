#pragma once

// The connection type a Bambu printer keeps after an SSDP announcement.
//
// Every announcement used to overwrite MachineObject::dev_connection_type with the printer's
// DevConnect value, and "lan" is what decides almost everything about a Bambu printer here:
//   - the printer picker offers "Log in printer" (BindMachineDialog, a cloud bind) for anything
//     that is not "lan", where a LAN printer gets the access-code dialog instead;
//   - get_my_machine_list() lists a local printer only while it is "lan", so a downgrade drops it
//     from the Device tab's list and from the LAN watch set (the plug-in's watch session for it is
//     stopped, and started again when the next announcement says "lan" - "watch start" / "lan
//     connected" churn in ultranet_print.log);
//   - DeviceManager::update_local_machine() ERASES a non-"lan" printer from the saved LAN printers,
//     so one stray announcement forgot a printer the user had set up with its access code.
//
// And printers do send stray announcements: an H2C (O1C2) in LAN-only mode announced
// "connect=cloud bind=free" once between "lan" ones, six seconds apart (owner's Mac, 2.4.5.4).
//
// The rule:
//   0. A printer that announces "cloud" with bind=occupied while it is in the signed-in account's
//      own bind list really is in cloud mode: it follows the announcement, before 1 and 2 are
//      asked (see "Why a bound printer vanished" below).
//   1. A printer the user set up as a LAN printer stays "lan" whatever an announcement says: one
//      saved in AppConfig's LAN printers, or one that is "lan" now and has its access code entered.
//      Signed out, any printer whose access code is known counts: the code is all there is to
//      reach it with.
//   2. Hysteresis for every other printer: one that announced "lan" within LAN_MEMORY_MS stays
//      "lan" through non-"lan" announcements that say bind=free, until they have gone on for
//      CLOUD_SUSTAIN_MS with no "lan" in between. bind=occupied (bound to an account) switches it
//      at once.
//   3. Anything else follows the announcement, as before.
// A printer kept "lan" always has bind=free (MachineObject::is_avaliable() hides anything else).
// And the printer picker never offers a cloud bind for a printer that announced "lan" within
// LAN_MEMORY_MS, nor when nobody is signed in; it offers the access-code dialog (picker_offer).
//
// Why a bound printer vanished (owner, 2026-10-15: X1C switched from LAN-only to cloud for Bambu
// Connect, gone from the device list until he signed out and in again). Signing in builds the
// cloud list's copy of the printer with no connection type, which lists it as a cloud printer.
// Its next announcement ("cloud", "occupied") then ran into rule 1: the LAN copy of the same
// printer had just been saved as a LAN printer, so the cloud list's copy was pushed back to "lan"
// (and is_lan_mode_printer() removes a "lan" printer from the cloud part of get_my_machine_list),
// while the LAN copy, kept "lan" with the announced bind=occupied, failed is_avaliable() and was
// not listed either. Six seconds after every sign-in, the printer was in neither list.
//
// Kept in this small header, apart from DeviceManager (which needs wx and half the GUI), so the
// rule is unit-tested; see tests/slic3rutils/ssdp_connect_type_tests.cpp. Times are the caller's
// clock in milliseconds (DeviceManager uses steady_clock).

#include <string>

namespace Slic3r {
namespace SsdpConnectType {

// How long a "lan" announcement keeps counting.
constexpr long long LAN_MEMORY_MS = 10LL * 60 * 1000;
// How long non-"lan" announcements must go on, with no "lan" among them, before a printer that
// was recently "lan" is taken to have really changed mode.
constexpr long long CLOUD_SUSTAIN_MS = 2LL * 60 * 1000;

// What one printer has announced lately. One per printer (not per list), fed every announcement.
struct LanMemory
{
    long long last_lan_ms { 0 };    // the last "lan" announcement; 0 = none
    long long cloud_since_ms { 0 }; // first of the current run of non-"lan" ones; 0 = no such run

    void observe(const std::string& announced, long long now)
    {
        const long long t = now > 0 ? now : 1;
        if (announced == "lan") {
            last_lan_ms    = t;
            cloud_since_ms = 0;
        } else if (cloud_since_ms == 0) {
            cloud_since_ms = t;
        }
    }

    bool lan_recently(long long now) const { return last_lan_ms != 0 && now - last_lan_ms <= LAN_MEMORY_MS; }
};

enum class Why {
    Announced,    // the announcement's value, as it is
    SavedLan,     // kept "lan": a saved LAN printer
    AccessCode,   // kept "lan": "lan" with its access code entered (or signed out, with a code)
    RecentLan,    // kept "lan": announced "lan" lately, the non-"lan" run is short and bind=free
    AccountBound, // followed: "cloud" + bind=occupied for a printer in the signed-in account's list
};

inline const char* why_text(Why w)
{
    switch (w) {
    case Why::SavedLan:      return "kept LAN: saved LAN printer";
    case Why::AccessCode:    return "kept LAN: access code entered";
    case Why::RecentLan:     return "kept LAN: announced lan recently";
    case Why::AccountBound:  return "cloud: bound to the signed-in account";
    default:                 return "as announced";
    }
}

struct Result
{
    std::string connect_type; // what to store in dev_connection_type
    std::string bind_state;   // what to store in bind_state
    bool        kept_lan { false }; // the announcement said otherwise and was overruled
    Why         why { Why::Announced };
};

// Who is looking: whether an account is signed in, and whether this printer is in its bind list.
// The defaults are the situation the rule was first written for (signed in, nothing known about
// the printer's binding).
struct Account
{
    bool signed_in { true };
    bool bound { false }; // in the signed-in account's bind list (never true when signed out)
};

// `current`: the printer's connection type before this announcement ("" for a new one).
// `announced`, `announced_bind`: the announcement's connect_type / bind_state, after the "farm"
// mapping on_machine_alive already does. `saved_lan`: the printer is one of the saved LAN printers.
// `has_access_code`: the user entered its access code. `mem`: this printer's memory, ALREADY fed
// this announcement (LanMemory::observe).
inline Result after_announcement(const std::string& current, const std::string& announced,
                                 const std::string& announced_bind, bool saved_lan, bool has_access_code,
                                 const LanMemory& mem, long long now, const Account& account = Account{})
{
    if (announced == "lan")
        return { announced, announced_bind, false, Why::Announced };

    // Rule 0: the account says this printer is its own, and the printer says it is on the cloud.
    if (account.signed_in && account.bound && announced_bind == "occupied")
        return { announced, announced_bind, false, Why::AccountBound };

    // A LAN printer is never unbound for this purpose: an empty bind state would also hide it
    // (MachineObject::is_avaliable), and so would the announced "occupied" of a printer that is
    // bound to somebody's account while it is also reachable with its access code.
    const std::string bind = "free";
    if (saved_lan)
        return { "lan", bind, true, Why::SavedLan };
    if (current == "lan" && has_access_code)
        return { "lan", bind, true, Why::AccessCode };
    // Signed out there is no cloud list and no bind dialog: a printer with a known code is a LAN
    // printer (whether its firmware lets it be one in cloud mode, the connection finds out).
    if (!account.signed_in && has_access_code)
        return { "lan", bind, true, Why::AccessCode };
    if (announced_bind != "occupied" && mem.lan_recently(now) && mem.cloud_since_ms != 0 &&
        now - mem.cloud_since_ms < CLOUD_SUSTAIN_MS)
        return { "lan", bind, true, Why::RecentLan };
    return { announced, announced_bind, false, Why::Announced };
}

// What the printer picker's "Other Device" list offers on a click.
enum class PickerOffer { AccessCode, Bind };

// `is_lan`: dev_connection_type is "lan". `lan_recently`: LanMemory::lan_recently for it.
// `signed_in`: an account is signed in; without one a bind cannot be offered at all.
inline PickerOffer picker_offer(bool is_lan, bool lan_recently, bool signed_in = true)
{
    return is_lan || lan_recently || !signed_in ? PickerOffer::AccessCode : PickerOffer::Bind;
}

// Whether, and how, the picker's "Other Device" list shows a local printer that is not in the
// account's list. `bind_free`: MachineObject::is_avaliable().
enum class OtherEntry { Hide, Lan, Bind };

inline OtherEntry other_devices_entry(bool signed_in, bool is_lan, bool bind_free)
{
    if (is_lan)
        return bind_free ? OtherEntry::Lan : OtherEntry::Hide;
    // Not LAN. Signed in: a free one can be bound; one that is bound to somebody is theirs, and
    // shows in "My devices" if it is ours. Signed out: it is not hidden - the access code is the
    // way in, as it is for a LAN printer (Bambu Studio hides it here).
    if (!signed_in)
        return OtherEntry::Lan;
    return bind_free ? OtherEntry::Bind : OtherEntry::Hide;
}

} // namespace SsdpConnectType
} // namespace Slic3r
