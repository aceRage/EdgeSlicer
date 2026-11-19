// The connection type a Bambu printer keeps after an SSDP announcement (SsdpConnectType.hpp).
//
// On macOS, LAN-only Bambu printers turned into "log in to this printer" (cloud bind) candidates:
// every announcement overwrote the printer's connection type, and anything but "lan" offered
// BindMachineDialog, dropped the printer from the LAN lists and the LAN watch set, and erased it
// from the saved LAN printers. An H2C in LAN-only mode does announce "connect=cloud bind=free" now
// and then (owner's Mac, 2.4.5.4: lan ... cloud at 07:08:38 ... lan 6 s later). These cases drive
// the rule on its own: no wx, no network.

#include <catch2/catch.hpp>

#include "slic3r/GUI/SsdpConnectType.hpp"

#include <string>

using namespace Slic3r::SsdpConnectType;

namespace {
constexpr long long S = 1000; // ms per second

// One printer, fed announcements the way DeviceManager::on_machine_alive feeds them.
struct Printer
{
    std::string current;
    std::string bind;
    bool        saved_lan { false };
    bool        has_code { false };
    LanMemory   mem;
    Result      last;

    const std::string& announce(const std::string& connect, const std::string& announced_bind, long long now)
    {
        mem.observe(connect, now);
        last    = after_announcement(current, connect, announced_bind, saved_lan, has_code, mem, now);
        current = last.connect_type;
        bind    = last.bind_state;
        return current;
    }
};
} // namespace

TEST_CASE("A saved LAN printer is never downgraded by an announcement", "[SsdpConnectType]")
{
    const LanMemory none;
    for (const char* announced : {"cloud", "", "unknown"}) {
        const Result r = after_announcement("lan", announced, "occupied", /*saved_lan*/ true, /*has_access_code*/ true, none, 1000 * S);
        CHECK(r.connect_type == "lan");
        CHECK(r.kept_lan);
        CHECK(r.why == Why::SavedLan);
        CHECK(r.bind_state == "free"); // a kept LAN printer is always available (is_avaliable)
    }
    // Saved, but its object was created without a code yet (or the cloud list's copy).
    const Result r = after_announcement("", "cloud", "", true, false, none, 1000 * S);
    CHECK(r.connect_type == "lan");
    CHECK(r.bind_state == "free"); // never unbound: an empty bind state hides the printer
}

TEST_CASE("A LAN printer whose access code was just entered stays LAN", "[SsdpConnectType]")
{
    const Result r = after_announcement("lan", "cloud", "occupied", /*saved_lan*/ false, /*has_access_code*/ true, LanMemory{}, 1000 * S);
    CHECK(r.connect_type == "lan");
    CHECK(r.why == Why::AccessCode);
}

TEST_CASE("A LAN announcement is taken as it is", "[SsdpConnectType]")
{
    for (bool saved : {false, true})
        for (bool code : {false, true}) {
            LanMemory mem;
            mem.observe("lan", 1000 * S);
            const Result r = after_announcement("cloud", "lan", "free", saved, code, mem, 1000 * S);
            CHECK(r.connect_type == "lan");
            CHECK(r.bind_state == "free");
            CHECK_FALSE(r.kept_lan);
            CHECK(r.why == Why::Announced);
        }
}

TEST_CASE("Hysteresis: the owner's H2C, unsaved and without a code, rides out one cloud announcement", "[SsdpConnectType]")
{
    // Fresh data dir, signed in: the H2C is neither saved nor has a code yet.
    Printer h2c;
    long long t = 1000 * S;
    CHECK(h2c.announce("lan", "free", t) == "lan");
    t += 30 * S;
    CHECK(h2c.announce("lan", "free", t) == "lan");
    t += 30 * S; // 07:08:38
    CHECK(h2c.announce("cloud", "free", t) == "lan");
    CHECK(h2c.last.kept_lan);
    CHECK(h2c.last.why == Why::RecentLan);
    CHECK(h2c.bind == "free");
    t += 6 * S; // 07:08:44
    CHECK(h2c.announce("lan", "free", t) == "lan");
    CHECK(h2c.last.why == Why::Announced);
    CHECK(h2c.mem.cloud_since_ms == 0); // the run of cloud ones is over
}

TEST_CASE("Hysteresis: a sustained cloud state switches the printer", "[SsdpConnectType]")
{
    Printer p;
    long long t = 1000 * S;
    p.announce("lan", "free", t);
    const long long cloud_start = t + 10 * S;
    for (long long s = 0; s < CLOUD_SUSTAIN_MS / S; s += 10) {
        INFO("cloud for " << s << " s");
        CHECK(p.announce("cloud", "free", cloud_start + s * S) == "lan");
    }
    // Two minutes of cloud with no lan among them: it really changed mode.
    CHECK(p.announce("cloud", "free", cloud_start + CLOUD_SUSTAIN_MS) == "cloud");
    CHECK_FALSE(p.last.kept_lan);
    // And a lan announcement brings it straight back.
    CHECK(p.announce("lan", "free", cloud_start + CLOUD_SUSTAIN_MS + 5 * S) == "lan");
}

TEST_CASE("Hysteresis: bind=occupied switches at once", "[SsdpConnectType]")
{
    Printer p;
    p.announce("lan", "free", 1000 * S);
    CHECK(p.announce("cloud", "occupied", 1005 * S) == "cloud");
    CHECK(p.bind == "occupied");
    CHECK(p.last.why == Why::Announced);
}

TEST_CASE("Hysteresis: a lan announcement counts for LAN_MEMORY_MS only", "[SsdpConnectType]")
{
    LanMemory mem;
    mem.observe("lan", 1000 * S);
    CHECK(mem.lan_recently(1000 * S + LAN_MEMORY_MS));
    CHECK_FALSE(mem.lan_recently(1000 * S + LAN_MEMORY_MS + 1));

    // Silent for longer than that, then cloud: it follows the announcement.
    Printer p;
    p.announce("lan", "free", 1000 * S);
    CHECK(p.announce("cloud", "free", 1000 * S + LAN_MEMORY_MS + S) == "cloud");
}

TEST_CASE("A printer the user never set up as LAN, and never heard as lan, follows the announcement", "[SsdpConnectType]")
{
    // New on the network, cloud mode: offered a bind, as before.
    Printer cloud_printer;
    CHECK(cloud_printer.announce("cloud", "occupied", 1000 * S) == "cloud");
    CHECK(cloud_printer.bind == "occupied");
    Printer free_cloud;
    CHECK(free_cloud.announce("cloud", "free", 1000 * S) == "cloud");

    // A cloud printer that has a code from the account stays whatever it announces (signed in).
    const Result r = after_announcement("cloud", "cloud", "occupied", false, true, LanMemory{}, 1000 * S);
    CHECK(r.connect_type == "cloud");
    CHECK_FALSE(r.kept_lan);

    // U0 firmware sends neither value; on_machine_alive fills the bind state in afterwards.
    const Result u0 = after_announcement("", "", "", false, false, LanMemory{}, 1000 * S);
    CHECK(u0.connect_type.empty());
    CHECK(u0.bind_state.empty());
}

TEST_CASE("A printer flipping between announcements stays LAN throughout", "[SsdpConnectType]")
{
    // What the ultranet_print.log churn looked like: lan, cloud, lan, cloud... Each flip to cloud
    // used to drop the printer from the LAN watch set and the next lan put it back.
    Printer   p;
    long long t = 1000 * S;
    int       i = 0;
    for (const char* announced : {"lan", "cloud", "lan", "cloud", "cloud", "lan", "cloud"}) {
        INFO("announcement " << i << ": " << announced);
        CHECK(p.announce(announced, "free", t) == "lan");
        t += 6 * S;
        ++i;
    }
}

TEST_CASE("The printer picker never offers a bind for a printer heard as lan lately", "[SsdpConnectType]")
{
    CHECK(picker_offer(/*is_lan*/ true, /*lan_recently*/ false) == PickerOffer::AccessCode);
    CHECK(picker_offer(false, true) == PickerOffer::AccessCode);
    CHECK(picker_offer(true, true) == PickerOffer::AccessCode);
    CHECK(picker_offer(false, false) == PickerOffer::Bind);

    // The H2C a moment after its stray cloud announcement, as the picker sees it.
    Printer h2c;
    h2c.announce("lan", "free", 1000 * S);
    h2c.announce("cloud", "free", 1030 * S);
    CHECK(picker_offer(h2c.current == "lan", h2c.mem.lan_recently(1031 * S)) == PickerOffer::AccessCode);
    // Even after a sustained cloud run switched it, a lan heard within LAN_MEMORY_MS still wins.
    h2c.announce("cloud", "free", 1030 * S + CLOUD_SUSTAIN_MS);
    REQUIRE(h2c.current == "cloud");
    CHECK(picker_offer(false, h2c.mem.lan_recently(1030 * S + CLOUD_SUSTAIN_MS)) == PickerOffer::AccessCode);
}

// --- Owner, 2026-10-15: X1C switched from LAN-only to cloud, gone from the list until sign-out/in ---

TEST_CASE("A saved LAN printer in the signed-in account's list follows a cloud announcement", "[SsdpConnectType]")
{
    // The log: six seconds after sign-in the cloud list's copy went "lan" ("kept LAN: saved LAN
    // printer") and the LAN copy kept "lan" with bind=occupied; neither was listed.
    Account bound;
    bound.signed_in = true;
    bound.bound     = true;
    for (bool saved : {false, true})
        for (bool code : {false, true})
            for (const char* current : {"", "cloud", "lan"}) {
                INFO("saved " << saved << " code " << code << " current '" << current << "'");
                const Result r = after_announcement(current, "cloud", "occupied", saved, code, LanMemory{}, 1000 * S, bound);
                CHECK(r.connect_type == "cloud");
                CHECK(r.bind_state == "occupied");
                CHECK_FALSE(r.kept_lan);
                CHECK(r.why == Why::AccountBound);
            }
}

TEST_CASE("Rule 0 needs the account: signed out or not in its list, the LAN rules still apply", "[SsdpConnectType]")
{
    Account other; // signed in, but the printer is not in this account's list (someone else's)
    other.bound = false;
    const Result o = after_announcement("lan", "cloud", "occupied", true, true, LanMemory{}, 1000 * S, other);
    CHECK(o.connect_type == "lan");
    CHECK(o.bind_state == "free");
    CHECK(o.why == Why::SavedLan);

    // bind=free is never taken as "bound": a stray "cloud/free" from a bound printer's list copy
    // still rides on the saved-LAN rule.
    Account bound;
    bound.bound = true;
    const Result stray = after_announcement("lan", "cloud", "free", true, true, LanMemory{}, 1000 * S, bound);
    CHECK(stray.connect_type == "lan");
    CHECK(stray.why == Why::SavedLan);

    // Rule 0 is for "cloud" only: a lan announcement is a lan announcement.
    const Result lan = after_announcement("cloud", "lan", "free", false, false, LanMemory{}, 1000 * S, bound);
    CHECK(lan.connect_type == "lan");
    CHECK(lan.why == Why::Announced);
}

TEST_CASE("Signed out, a cloud-mode printer with a known access code is a LAN printer", "[SsdpConnectType]")
{
    Account out;
    out.signed_in = false;
    // The printer was cloud while the account was signed in; the code is still known.
    const Result r = after_announcement("cloud", "cloud", "occupied", false, true, LanMemory{}, 1000 * S, out);
    CHECK(r.connect_type == "lan");
    CHECK(r.bind_state == "free"); // listed by get_my_machine_list
    CHECK(r.kept_lan);
    CHECK(r.why == Why::AccessCode);

    // Without a code it is shown as announced, and the picker lists it for the code (below).
    const Result none = after_announcement("cloud", "cloud", "occupied", false, false, LanMemory{}, 1000 * S, out);
    CHECK(none.connect_type == "cloud");
    CHECK_FALSE(none.kept_lan);
}

TEST_CASE("LAN-only <-> cloud round trip: the printer is listable in every state", "[SsdpConnectType]")
{
    // What the device list needs from a printer: not lan -> in the account's list (user copy);
    // lan -> bind=free and a code (local copy). Walk the owner's sequence signed in and bound.
    Account bound;
    bound.bound = true;
    LanMemory mem;
    std::string user_type, local_type, local_bind;
    long long t = 1000 * S;
    auto feed = [&](const char* connect, const char* bind, bool saved, bool code) {
        mem.observe(connect, t);
        const Result u = after_announcement(user_type, connect, bind, saved, false, mem, t, bound);
        const Result l = after_announcement(local_type, connect, bind, saved, code, mem, t, bound);
        user_type  = u.connect_type;
        local_type = l.connect_type;
        local_bind = l.bind_state;
        t += 6 * S;
    };
    feed("lan", "free", true, true); // LAN-only, set up with its code
    CHECK(local_type == "lan");
    CHECK(local_bind == "free"); // listed as a LAN printer
    feed("cloud", "occupied", true, true); // switched to cloud for Bambu Connect
    CHECK(user_type == "cloud"); // in the account's list (not lan)
    CHECK(local_type == "cloud");
    feed("cloud", "occupied", true, true); // every later announcement too: no flipping back
    CHECK(user_type == "cloud");
    feed("lan", "free", true, true); // and back to LAN-only
    CHECK(local_type == "lan");
    CHECK(local_bind == "free");
    CHECK(user_type == "lan"); // not in the cloud half of the list; the local copy is listed
}

TEST_CASE("The picker never offers a bind when nobody is signed in", "[SsdpConnectType]")
{
    CHECK(picker_offer(false, false, /*signed_in*/ false) == PickerOffer::AccessCode);
    CHECK(picker_offer(false, false, true) == PickerOffer::Bind);
    CHECK(picker_offer(true, false, false) == PickerOffer::AccessCode);
}

TEST_CASE("The picker's Other Device list: what shows, signed in and out", "[SsdpConnectType]")
{
    // LAN printers: shown when available (bind free), as before.
    CHECK(other_devices_entry(true, true, true) == OtherEntry::Lan);
    CHECK(other_devices_entry(false, true, true) == OtherEntry::Lan);
    CHECK(other_devices_entry(true, true, false) == OtherEntry::Hide);
    // Signed in: a free cloud printer can be bound; a bound one belongs in My devices or to someone.
    CHECK(other_devices_entry(true, false, true) == OtherEntry::Bind);
    CHECK(other_devices_entry(true, false, false) == OtherEntry::Hide);
    // Signed out: not hidden - Bambu Studio hides these; here the access code is the way in.
    CHECK(other_devices_entry(false, false, true) == OtherEntry::Lan);
    CHECK(other_devices_entry(false, false, false) == OtherEntry::Lan);
}
