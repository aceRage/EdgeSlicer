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
        CHECK(r.bind_state == "occupied");
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

    // A cloud printer that has a code from the account stays whatever it announces.
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
