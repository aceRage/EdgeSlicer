// The connection type a Bambu printer keeps after an SSDP announcement (SsdpConnectType.hpp).
//
// On macOS, LAN-only Bambu printers turned into "log in to this printer" (cloud bind) candidates:
// every announcement overwrote the printer's connection type, and anything but "lan" offered
// BindMachineDialog, dropped the printer from the LAN lists and the LAN watch set, and erased it
// from the saved LAN printers. These cases drive the rule on its own: no wx, no network.

#include <catch2/catch.hpp>

#include "slic3r/GUI/SsdpConnectType.hpp"

using Slic3r::SsdpConnectType::after_announcement;

TEST_CASE("A saved LAN printer is never downgraded by an announcement", "[SsdpConnectType]")
{
    for (const char* announced : {"cloud", "", "unknown"}) {
        const auto r = after_announcement("lan", announced, "occupied", /*saved_lan*/ true, /*has_access_code*/ true);
        CHECK(r.connect_type == "lan");
        CHECK(r.kept_lan);
        CHECK(r.bind_state == "occupied");
    }
    // Saved, but its object was created without a code yet (or the cloud list's copy).
    const auto r = after_announcement("", "cloud", "", true, false);
    CHECK(r.connect_type == "lan");
    CHECK(r.bind_state == "free"); // never unbound: an empty bind state hides the printer
}

TEST_CASE("A LAN printer whose access code was just entered stays LAN", "[SsdpConnectType]")
{
    const auto r = after_announcement("lan", "cloud", "occupied", /*saved_lan*/ false, /*has_access_code*/ true);
    CHECK(r.connect_type == "lan");
    CHECK(r.kept_lan);
}

TEST_CASE("A LAN announcement is taken as it is", "[SsdpConnectType]")
{
    for (bool saved : {false, true})
        for (bool code : {false, true}) {
            const auto r = after_announcement("cloud", "lan", "free", saved, code);
            CHECK(r.connect_type == "lan");
            CHECK(r.bind_state == "free");
            CHECK_FALSE(r.kept_lan);
        }
}

TEST_CASE("A printer the user never set up as LAN follows the announcement", "[SsdpConnectType]")
{
    // New on the network, cloud mode: offered a bind, as before.
    auto r = after_announcement("", "cloud", "occupied", false, false);
    CHECK(r.connect_type == "cloud");
    CHECK(r.bind_state == "occupied");
    CHECK_FALSE(r.kept_lan);

    // LAN until now but no access code: it is not the user's LAN printer yet.
    r = after_announcement("lan", "cloud", "occupied", false, false);
    CHECK(r.connect_type == "cloud");

    // A cloud printer that has a code from the account stays whatever it announces.
    r = after_announcement("cloud", "cloud", "occupied", false, true);
    CHECK(r.connect_type == "cloud");
    CHECK_FALSE(r.kept_lan);

    // U0 firmware sends neither value; on_machine_alive fills the bind state in afterwards.
    r = after_announcement("", "", "", false, false);
    CHECK(r.connect_type.empty());
    CHECK(r.bind_state.empty());
}

TEST_CASE("A printer flipping between announcements stays LAN once it is the user's", "[SsdpConnectType]")
{
    // What the ultranet_print.log churn looked like: lan, cloud, lan, cloud... Each flip to cloud
    // used to drop the printer from the LAN watch set and the next lan put it back.
    std::string current = "";
    bool        saved   = false;
    const char* seq[]   = {"lan", "cloud", "lan", "cloud", "cloud", "lan"};
    int         i       = 0;
    for (const char* announced : seq) {
        const bool has_code = i >= 1; // the user entered the code after the first announcement
        current             = after_announcement(current, announced, "free", saved, has_code).connect_type;
        if (has_code && current == "lan") saved = true; // update_local_machine saves it
        CHECK(current == "lan");
        ++i;
    }
}
