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
// A printer the user set up as a LAN printer therefore stays "lan" whatever an announcement says:
// one saved in AppConfig's LAN printers, or one that is "lan" now and has its access code entered
// (the moment before it is saved). Anything else follows the announcement, as before.
//
// Kept in this small header, apart from DeviceManager (which needs wx and half the GUI), so the
// rule is unit-tested; see tests/slic3rutils/ssdp_connect_type_tests.cpp.

#include <string>

namespace Slic3r {
namespace SsdpConnectType {

struct Result
{
    std::string connect_type; // what to store in dev_connection_type
    std::string bind_state;   // what to store in bind_state
    bool        kept_lan { false }; // the announcement said otherwise and was overruled
};

// `current`: the printer's connection type before this announcement ("" for a new one).
// `announced`, `announced_bind`: the announcement's connect_type / bind_state, after the "farm"
// mapping on_machine_alive already does. `saved_lan`: the printer is one of the saved LAN printers.
// `has_access_code`: the user entered its access code.
inline Result after_announcement(const std::string& current, const std::string& announced,
                                 const std::string& announced_bind, bool saved_lan, bool has_access_code)
{
    const bool user_lan = saved_lan || (current == "lan" && has_access_code);
    if (user_lan && announced != "lan") {
        // A LAN printer is never unbound for this purpose: an empty bind state would also hide it
        // (MachineObject::is_avaliable).
        return { "lan", announced_bind.empty() ? std::string("free") : announced_bind, true };
    }
    return { announced, announced_bind, false };
}

} // namespace SsdpConnectType
} // namespace Slic3r
