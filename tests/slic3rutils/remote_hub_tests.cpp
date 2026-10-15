// Pure parsing rules pulled out of RemoteHub.cpp for the port-fallback reachability fix
// (Slic3r::GUI::RemoteHub::Testing, see RemoteHub.hpp): the port that `tailscale serve status
// --json` is currently forwarding to, who `netstat -ano` says is LISTENING on a given port, and
// the image name `tasklist`'s CSV line names for that pid. All three are driven here from
// captured command output - no socket, no process, no real tailnet or netstat needed.

#include <catch2/catch.hpp>

#include "slic3r/GUI/RemoteHub.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Slic3r::GUI::RemoteHub::Testing;

// ---- serve_status_target_port ------------------------------------------------------------

TEST_CASE("serve_status_target_port reads the root handler's loopback proxy port", "[RemoteHub]")
{
    const std::string j = R"({
        "Web": {
            "myhost.tailxxxx.ts.net:443": {
                "Handlers": {
                    "/": { "Proxy": "http://127.0.0.1:13640" }
                }
            }
        }
    })";
    REQUIRE(serve_status_target_port(j) == 13640);
}

TEST_CASE("serve_status_target_port follows the hub to a fallback port", "[RemoteHub]")
{
    // What the reconcile has to notice: Serve is still forwarding to the port from before the hub
    // stepped past HUB_PORT because something else held it.
    const std::string j = R"({
        "Web": {
            "myhost.tailxxxx.ts.net:443": {
                "Handlers": {
                    "/": { "Proxy": "http://127.0.0.1:13641" }
                }
            }
        }
    })";
    REQUIRE(serve_status_target_port(j) == 13641);
}

TEST_CASE("serve_status_target_port is 0 when nothing is served", "[RemoteHub]")
{
    REQUIRE(serve_status_target_port(R"({"Web": {}})") == 0);
    REQUIRE(serve_status_target_port("") == 0);
    REQUIRE(serve_status_target_port("not json") == 0);
}

TEST_CASE("serve_status_target_port ignores a non-root path", "[RemoteHub]")
{
    // Serve can host more than one path; only "/" is the hub's own root and the one m_port has to
    // match. A handler on some other path must not be mistaken for it.
    const std::string j = R"({
        "Web": {
            "myhost.tailxxxx.ts.net:443": {
                "Handlers": {
                    "/other": { "Proxy": "http://127.0.0.1:9999" }
                }
            }
        }
    })";
    REQUIRE(serve_status_target_port(j) == 0);
}

TEST_CASE("serve_status_target_port ignores a non-loopback proxy target", "[RemoteHub]")
{
    const std::string j = R"({
        "Web": {
            "myhost.tailxxxx.ts.net:443": {
                "Handlers": {
                    "/": { "Proxy": "http://10.0.0.5:13640" }
                }
            }
        }
    })";
    REQUIRE(serve_status_target_port(j) == 0);
}

// ---- netstat_holder_pid --------------------------------------------------------------------

TEST_CASE("netstat_holder_pid finds the pid listening on the port", "[RemoteHub]")
{
    const std::string out =
        "\n"
        "Active Connections\n"
        "\n"
        "  Proto  Local Address          Foreign Address        State           PID\n"
        "  TCP    0.0.0.0:135            0.0.0.0:0              LISTENING       1234\n"
        "  TCP    0.0.0.0:13640          0.0.0.0:0              LISTENING       5678\n"
        "  TCP    127.0.0.1:13641        0.0.0.0:0              LISTENING       9012\n";
    REQUIRE(netstat_holder_pid(out, 13640) == 5678);
    REQUIRE(netstat_holder_pid(out, 13641) == 9012);
}

TEST_CASE("netstat_holder_pid is 0 when the port is not listed", "[RemoteHub]")
{
    const std::string out =
        "  Proto  Local Address          Foreign Address        State           PID\n"
        "  TCP    0.0.0.0:135            0.0.0.0:0              LISTENING       1234\n";
    REQUIRE(netstat_holder_pid(out, 13640) == 0);
    REQUIRE(netstat_holder_pid("", 13640) == 0);
}

TEST_CASE("netstat_holder_pid ignores non-LISTENING rows and a port-number prefix collision", "[RemoteHub]")
{
    const std::string out =
        "  Proto  Local Address          Foreign Address        State           PID\n"
        // An ESTABLISHED row on the port must not count as a holder of it.
        "  TCP    192.168.1.5:13640      192.168.1.9:51000      ESTABLISHED     4321\n"
        // 136400 must not match a lookup for 13640 (suffix match, not substring).
        "  TCP    0.0.0.0:136400         0.0.0.0:0              LISTENING       8888\n";
    REQUIRE(netstat_holder_pid(out, 13640) == 0);
}

TEST_CASE("netstat_holder_pid handles the [::] IPv6 wildcard column", "[RemoteHub]")
{
    const std::string out =
        "  Proto  Local Address          Foreign Address        State           PID\n"
        "  TCP    [::]:13640             [::]:0                 LISTENING       2222\n";
    REQUIRE(netstat_holder_pid(out, 13640) == 2222);
}

// ---- tasklist_image_name -------------------------------------------------------------------

TEST_CASE("tasklist_image_name reads the first quoted CSV field", "[RemoteHub]")
{
    REQUIRE(tasklist_image_name("\"EdgeSlicer.exe\",\"5678\",\"Console\",\"1\",\"123,456 K\"") == "EdgeSlicer.exe");
    REQUIRE(tasklist_image_name("\"python.exe\",\"9012\",\"Services\",\"0\",\"4,096 K\"") == "python.exe");
}

TEST_CASE("tasklist_image_name is empty for the no-match message", "[RemoteHub]")
{
    REQUIRE(tasklist_image_name("INFO: No tasks are running which match the specified criteria.") == "");
    REQUIRE(tasklist_image_name("") == "");
}

// ---- status JSON shape (info_json's port_note / lan_firewall) -------------------------------
// info_json() itself needs a live HubServer (a bound socket, go2rtc, the works), which is exactly
// what these pure-function tests are trying to avoid pulling in. What is checked here instead is
// the shape the deliverable specifies, built the same way info_json() builds it, so a change to
// either side is caught by the other: {"port_note": {"default_port", "held_by", "port"}} only
// appears when the bound port differs from HUB_PORT, and "lan_firewall" always carries the same
// state vocabulary the video one already uses.
TEST_CASE("port_note JSON shape matches the spec when the hub fell back to another port", "[RemoteHub]")
{
    nlohmann::json note;
    note["default_port"] = 13640;
    note["held_by"]      = "EdgeSlicer.exe (pid 5678)";
    note["port"]         = 13641;
    note["command"]      = "netsh advfirewall firewall add rule name=\"EdgeSlicer\" dir=in action=allow "
                            "program=\"C:\\EdgeSlicer.exe\" protocol=TCP localport=13640-13659 profile=private,domain";
    REQUIRE(note["default_port"].get<int>() == 13640);
    REQUIRE(note["port"].get<int>() == 13641);
    REQUIRE(note["held_by"].get<std::string>() == "EdgeSlicer.exe (pid 5678)");
    REQUIRE_THAT(note["command"].get<std::string>(), Catch::Matchers::Contains("netsh"));
}

TEST_CASE("lan_firewall JSON uses the same state vocabulary as video.firewall", "[RemoteHub]")
{
    const std::vector<std::string> valid = { "allowed", "partial", "missing", "blocked", "unknown", "off" };
    for (const std::string& s : { std::string("allowed"), std::string("missing"), std::string("off") })
        REQUIRE(std::find(valid.begin(), valid.end(), s) != valid.end());
}

// ---- the note/command split (owner, 2026-09-18: the hub page's firewall warnings were too long -
// one short sentence now, with the exact netsh line moved out to its own field so hub.html can put
// it behind a "Show command" toggle instead of running it into the sentence). Both `firewall_query`
// itself and the two call sites (go2rtc's WebRTC port, the phone/LAN listener) are private to
// RemoteHub.cpp, so this pins the JSON shape info_json() builds - the same approach the port_note
// and lan_firewall-vocabulary tests above already take.
TEST_CASE("a firewall note is one short sentence with no netsh text run into it", "[RemoteHub]")
{
    // What used to ship as a single string, e.g.: "Windows Firewall allows go2rtc.exe on Public
    // networks, but this PC is on a Private network. In Windows Defender Firewall allow go2rtc.exe
    // (inbound), or as Administrator run: netsh advfirewall ...". The note now stops at the
    // sentence; the netsh line lives in `command` instead.
    const std::string note    = "Windows Firewall allows go2rtc.exe on Public networks, but this PC is on Private.";
    const std::string command = "netsh advfirewall firewall add rule name=\"go2rtc\" dir=in action=allow "
                                 "program=\"C:\\go2rtc.exe\" protocol=TCP localport=8556 profile=private,domain";
    REQUIRE_THAT(note, !Catch::Matchers::Contains("netsh"));
    REQUIRE_THAT(command, Catch::Matchers::StartsWith("netsh advfirewall firewall add rule"));

    nlohmann::json v;
    v["firewall"] = "partial";
    v["note"]     = note;
    v["command"]  = command;
    REQUIRE(v["note"].get<std::string>() == note);
    REQUIRE(v["command"].get<std::string>() == command);
}

TEST_CASE("an allowed firewall state carries an empty note and an empty command", "[RemoteHub]")
{
    // firewall_query()'s "allowed" branch sets note to "" and leaves command unset (default ""):
    // nothing to warn about, so hub.html's warningLine()/videoLine() never draw a "Show command"
    // toggle for it.
    nlohmann::json lv;
    lv["firewall"] = "allowed";
    lv["note"]     = std::string();
    lv["command"]  = std::string();
    REQUIRE(lv["note"].get<std::string>().empty());
    REQUIRE(lv["command"].get<std::string>().empty());
}

// ---- the actual root cause of the 13641 fallback --------------------------------------------
// Not a parsing rule but a Windows socket-option fact, and the one that mattered: HubServer::bind()
// used to open the NEW acceptor and bind it before closing the OLD one. A hub starts on
// 127.0.0.1:13640 (phone off) and switching phone access on calls bind(true), which tries a new
// SO_EXCLUSIVEADDRUSE 0.0.0.0:13640 while that old 127.0.0.1:13640 acceptor is still open in the
// same process - Windows refuses the exclusive wildcard bind over an existing narrower one, even
// in the same process, so the loop stepped to 13641 on *every* "turn phone access on", with
// nothing external involved at all. bind() now closes the old acceptor before attempting the new
// one (RemoteHub.cpp, HubServer::bind). This pins the two halves of that fact so a change to the
// option or the ordering is caught here rather than by a tester again.
#ifdef _WIN32
#include <boost/asio.hpp>
namespace {
namespace RHTAsio = boost::asio;
using RHTTcp = RHTAsio::ip::tcp;

bool bind_exclusive(RHTAsio::io_context& ioc, RHTTcp::acceptor& a, const RHTAsio::ip::address_v4& addr, unsigned short port)
{
    boost::system::error_code ec;
    a.open(RHTTcp::v4(), ec);
    if (ec) return false;
    a.set_option(RHTAsio::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>(true), ec);
    if (ec) return false;
    a.bind(RHTTcp::endpoint(addr, port), ec);
    return !ec;
}
} // namespace

TEST_CASE("SO_EXCLUSIVEADDRUSE: a wildcard bind conflicts with an existing loopback bind on the same port", "[RemoteHub][socket]")
{
    RHTAsio::io_context ioc;
    // An arbitrary high port, picked by letting the OS assign one first so the case does not
    // collide with anything else bound on this test machine (including a real hub).
    RHTTcp::acceptor probe(ioc);
    probe.open(RHTTcp::v4());
    probe.bind(RHTTcp::endpoint(RHTAsio::ip::make_address_v4("127.0.0.1"), 0));
    const unsigned short port = probe.local_endpoint().port();
    probe.close();

    // Step 1: bind 127.0.0.1:<port> exclusively (the hub's phone-off, loopback-only listener).
    RHTTcp::acceptor loopback(ioc);
    REQUIRE(bind_exclusive(ioc, loopback, RHTAsio::ip::make_address_v4("127.0.0.1"), port));

    // Step 2: WITHOUT closing it, try 0.0.0.0:<port> exclusively (what bind(true) used to attempt
    // first, for "phone access turned on") - this is the self-conflict that sent the hub to
    // 13641 every time, and it must still fail so the fix is proven to be closing the old
    // acceptor first, not some other accidental change.
    RHTTcp::acceptor wildcard_while_open(ioc);
    REQUIRE_FALSE(bind_exclusive(ioc, wildcard_while_open, RHTAsio::ip::address_v4::any(), port));

    // Step 3: close the loopback acceptor (what bind() now does before attempting the new one) -
    // the same 0.0.0.0:<port> exclusive bind must now succeed.
    boost::system::error_code ec;
    loopback.close(ec);
    RHTTcp::acceptor wildcard_after_close(ioc);
    REQUIRE(bind_exclusive(ioc, wildcard_after_close, RHTAsio::ip::address_v4::any(), port));
}
#endif // _WIN32
// ==============================================================================================
// Phase 0 of the hosted remote-access plan (tests/design_hosted_relay.md section 7): the remote
// state classifier the hub page draws from, the hub identity a relay will later know this hub by,
// and the one rule that decides whether Tailscale Serve's headers may be believed. All pure.
// ==============================================================================================


// ---- classify_remote_access --------------------------------------------------------------

// The whole table in one place, exactly as the card shows it. Each row is (installed, backend,
// https, serving, on) -> state. The wording is checked separately below; what matters here is
// that no input lands in two buckets and none falls through to Error by accident.
TEST_CASE("classify_remote_access maps every Tailscale situation to one state", "[RemoteHub]")
{
    struct Row { bool installed; const char* backend; bool https, serving, on; RemoteAccessState want; };
    const Row rows[] = {
        // Nothing installed: whatever else is claimed, the answer is "install it".
        { false, "",           false, false, false, RemoteAccessState::NotInstalled },
        { false, "Running",    true,  true,  true,  RemoteAccessState::NotInstalled },
        // Installed but the backend has no account yet.
        { true,  "NeedsLogin", false, false, false, RemoteAccessState::NotSignedIn },
        { true,  "Starting",   false, false, false, RemoteAccessState::NotSignedIn },
        // Signed in but the service is down.
        { true,  "Stopped",    true,  false, false, RemoteAccessState::NotRunning },
        { true,  "NoState",    true,  false, false, RemoteAccessState::NotRunning },
        { true,  "",           true,  false, false, RemoteAccessState::NotRunning },
        // Running, but the tailnet issues no certificates: the one thing that cannot be fixed here.
        { true,  "Running",    false, false, false, RemoteAccessState::HttpsOff },
        { true,  "Running",    false, true,  true,  RemoteAccessState::HttpsOff },
        // Ready to be switched on, and switched on and working.
        { true,  "Running",    true,  false, false, RemoteAccessState::Ready },
        { true,  "Running",    true,  true,  false, RemoteAccessState::Ready }, // Serve left over from before
        { true,  "Running",    true,  true,  true,  RemoteAccessState::Serving },
        // Switched on but Serve is not pointed here any more - the port-drift case the reconcile
        // in bind() exists for. Reported, with a retry, never as "ready".
        { true,  "Running",    true,  false, true,  RemoteAccessState::Error },
    };
    for (const Row& r : rows) {
        DYNAMIC_SECTION("installed=" << r.installed << " backend=" << r.backend << " https=" << r.https
                                     << " serving=" << r.serving << " on=" << r.on) {
            const auto info = classify_remote_access(r.installed, r.backend, r.https, r.serving, r.on, "");
            REQUIRE(info.state == r.want);
            REQUIRE_FALSE(info.message.empty()); // every state says something; the card draws it verbatim
        }
    }
}

TEST_CASE("classify_remote_access offers the install link when Tailscale is missing", "[RemoteHub]")
{
    const auto info = classify_remote_access(false, "", false, false, false, "");
    REQUIRE(std::string(remote_access_state_name(info.state)) == "not_installed");
    // The explanation the design asks for: what Tailscale is, and that it goes on both devices.
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("Tailscale"));
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("free private network"));
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("same account"));
    // The download page of the platform the test runs on (Windows keeps /download/windows).
    REQUIRE(info.action_url == Slic3r::TailscaleCli::download_url(Slic3r::TailscaleCli::current_platform()));
    REQUIRE_FALSE(info.action.empty());
}

TEST_CASE("classify_remote_access speaks about the platform it is asked about", "[RemoteHub]")
{
    using Slic3r::TailscaleCli::Platform;
    const auto win = classify_remote_access(Platform::Windows, false, "", false, false, false, "");
    const auto mac = classify_remote_access(Platform::MacOS, false, "", false, false, false, "");
    const auto lin = classify_remote_access(Platform::Linux, false, "", false, false, false, "");
    REQUIRE(win.action_url == "https://tailscale.com/download/windows");
    REQUIRE(mac.action_url == "https://tailscale.com/download/mac");
    REQUIRE(lin.action_url == "https://tailscale.com/download/linux");
    REQUIRE_THAT(win.message, Catch::Matchers::Contains("your PC"));
    REQUIRE_THAT(mac.message, Catch::Matchers::Contains("your Mac"));
    REQUIRE_THAT(lin.message, Catch::Matchers::Contains("your computer"));
    for (const auto* i : { &win, &mac, &lin }) {
        REQUIRE(i->state == RemoteAccessState::NotInstalled);
        REQUIRE_THAT(i->message, Catch::Matchers::Contains("free private network"));
        REQUIRE_THAT(i->message, Catch::Matchers::Contains("same account"));
    }

    // Signed out: the tray on Windows, the menu bar on a Mac; `tailscale login` everywhere.
    const auto win_in = classify_remote_access(Platform::Windows, true, "NeedsLogin", false, false, false, "");
    const auto mac_in = classify_remote_access(Platform::MacOS, true, "NeedsLogin", false, false, false, "");
    REQUIRE_THAT(win_in.message, Catch::Matchers::Contains("not signed in on this PC"));
    REQUIRE_THAT(win_in.message, Catch::Matchers::Contains("system tray"));
    REQUIRE_THAT(mac_in.message, Catch::Matchers::Contains("not signed in on this Mac"));
    REQUIRE_THAT(mac_in.message, Catch::Matchers::Contains("menu bar"));
    REQUIRE_THAT(mac_in.message, Catch::Matchers::Contains("tailscale login"));

    // Stopped: no backend name, then the computer's name.
    REQUIRE(classify_remote_access(Platform::Windows, true, "", false, false, false, "").message == "Tailscale is not running on this PC.");
    REQUIRE(classify_remote_access(Platform::MacOS, true, "", false, false, false, "").message == "Tailscale is not running on this Mac.");
    REQUIRE(classify_remote_access(Platform::MacOS, true, "Stopped", false, false, false, "").message == "Tailscale is not running (Stopped).");
}

TEST_CASE("classify_remote_access: a Tailscale that did not answer is not a Tailscale that is missing", "[RemoteHub]")
{
    using Slic3r::TailscaleCli::Platform;
    for (Platform p : { Platform::Windows, Platform::MacOS, Platform::Linux }) {
        const auto info = classify_remote_access(p, true, Slic3r::TailscaleCli::BACKEND_NO_ANSWER, false, false, false, "");
        REQUIRE(info.state == RemoteAccessState::NoAnswer);
        REQUIRE(std::string(remote_access_state_name(info.state)) == "no_answer");
        REQUIRE_THAT(info.message, Catch::Matchers::Contains("did not answer"));
        REQUIRE(info.action_url.empty()); // no download link: it is installed
        REQUIRE_FALSE(info.action.empty());
    }
}

TEST_CASE("classify_remote_access sends the HTTPS-certificates error to the admin console", "[RemoteHub]")
{
    // The one error nobody can fix from this PC: it is a tailnet-wide setting, so the card links
    // straight at the page with the switch and offers a retry rather than a dead end.
    const auto info = classify_remote_access(true, "Running", false, false, false, "");
    REQUIRE(info.state == RemoteAccessState::HttpsOff);
    REQUIRE(std::string(remote_access_state_name(info.state)) == "https_off");
    REQUIRE(info.action_url == "https://login.tailscale.com/admin/dns");
    // The meaning of the old string is kept: admin console > DNS > HTTPS Certificates > Enable.
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("HTTPS Certificates"));
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("admin console"));
}

TEST_CASE("classify_remote_access keeps the sign-in hint actionable", "[RemoteHub]")
{
    const auto info = classify_remote_access(true, "NeedsLogin", false, false, false, "");
    REQUIRE(info.state == RemoteAccessState::NotSignedIn);
    REQUIRE_THAT(info.message, Catch::Matchers::Contains("tailscale login"));
    REQUIRE(info.action_url.empty()); // nothing to open: it is a local sign-in
    REQUIRE_FALSE(info.action.empty());
}

TEST_CASE("classify_remote_access reports the CLI's own words when it has nothing better", "[RemoteHub]")
{
    const auto info = classify_remote_access(true, "Running", true, false, false, "tailscaled said no");
    REQUIRE(info.state == RemoteAccessState::Error);
    REQUIRE(info.message == "tailscaled said no");
}

TEST_CASE("remote_access_state_name is the wire spelling of every state", "[RemoteHub]")
{
        REQUIRE(std::string(remote_access_state_name(RemoteAccessState::NotInstalled)) == "not_installed");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::NotSignedIn))  == "not_signed_in");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::NotRunning))   == "not_running");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::HttpsOff))     == "https_off");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::Serving))      == "serving");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::Ready))        == "ready");
    REQUIRE(std::string(remote_access_state_name(RemoteAccessState::Error))        == "error");
}

// ---- hubid derivation ---------------------------------------------------------------------

TEST_CASE("hubid is the first 16 hex characters of SHA-256 over the public key", "[RemoteHub]")
{
    // A fixed key with a fixed answer, so a change to the derivation cannot slip through: a relay
    // registration is keyed on this value and every hub that re-derived differently would lose it.
    std::vector<unsigned char> key(32);
    for (int i = 0; i < 32; ++i) key[i] = (unsigned char) i;
    REQUIRE(hubid_from_public_key(key) == "630dcd2966c43366");
    REQUIRE(hubid_from_public_key_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f") == "630dcd2966c43366");
    REQUIRE(hubid_from_public_key_hex("ABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB") == "9a2db2e23f1504cd");
    // Two different keys never share a hubid, and the derivation is stable across calls.
    REQUIRE(hubid_from_public_key_hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f") !=
            hubid_from_public_key_hex("abababababababababababababababababababababababababababababababab"));
}

TEST_CASE("hubid refuses anything that is not a 32-byte key", "[RemoteHub]")
{
    REQUIRE(hubid_from_public_key(std::vector<unsigned char>()).empty());
    REQUIRE(hubid_from_public_key(std::vector<unsigned char>(31, 0)).empty());
    REQUIRE(hubid_from_public_key(std::vector<unsigned char>(33, 0)).empty());
    REQUIRE(hubid_from_public_key_hex("").empty());
    REQUIRE(hubid_from_public_key_hex("00010203").empty());
    REQUIRE(hubid_from_public_key_hex(std::string(64, 'z')).empty()); // right length, not hex
}

// ---- the identity's settings.json round trip ----------------------------------------------

TEST_CASE("the hub identity survives a settings.json round trip", "[RemoteHub]")
{

    HubIdentity id;
    id.public_hex  = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    id.private_hex = std::string(64, '7');
    id.hubid       = "630dcd2966c43366";
    REQUIRE(id.valid());

    // What write_hub_json() puts under "identity", wrapped the way the real settings file wraps it.
    nlohmann::json settings;
    settings["token"]    = "abc";
    settings["identity"] = nlohmann::json::parse(identity_settings_dump(id));

    const HubIdentity back = identity_from_settings(settings.dump());
    REQUIRE(back.valid());
    REQUIRE(back.public_hex  == id.public_hex);
    REQUIRE(back.private_hex == id.private_hex);
    REQUIRE(back.hubid       == id.hubid);
    // The bare identity object parses too, so a caller that already dug the member out is fine.
    REQUIRE(identity_from_settings(identity_settings_dump(id)).hubid == id.hubid);
}

TEST_CASE("a settings file with no identity yields an invalid one, which is the signal to mint", "[RemoteHub]")
{
    REQUIRE_FALSE(identity_from_settings(R"({"token":"abc"})").valid());
    REQUIRE_FALSE(identity_from_settings("{}").valid());
    REQUIRE_FALSE(identity_from_settings("not json").valid());
    REQUIRE_FALSE(identity_from_settings("").valid());
    // A truncated or otherwise unusable public key is the same case: mint a new pair.
    REQUIRE_FALSE(identity_from_settings(R"({"identity":{"public":"0001","private":"ff","hubid":"x"}})").valid());
}

TEST_CASE("the stored hubid is only a cache: the public key always wins", "[RemoteHub]")
{
    // A hand-edited settings file claiming somebody else's hubid must not make this hub answer to
    // it - the name is a function of the key, and it is re-derived on every read.
    const std::string j = R"({"identity":{
        "public":"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        "private":"7777777777777777777777777777777777777777777777777777777777777777",
        "hubid":"deadbeefdeadbeef"}})";
    REQUIRE(identity_from_settings(j).hubid == "630dcd2966c43366");
}

// ---- the pairing document (what every app reads) -------------------------------------------

TEST_CASE("the pairing document carries the hubid, the public key and three named origins", "[RemoteHub]")
{
    const std::string pub = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    const nlohmann::json j = nlohmann::json::parse(
        pair_identity_json("http://192.168.1.20:13640/r/tok/", "https://pc.tailnet.ts.net/r/tok/", "", "630dcd2966c43366", pub));

    REQUIRE(j["hubid"] == "630dcd2966c43366");
    REQUIRE(j["public_key"] == pub);
    // Three keys, always present. `relay` is empty in phase 0 and the apps skip an empty origin,
    // so phase 1 fills a slot that every client already knows about rather than adding one.
    REQUIRE(j["urls"].contains("lan"));
    REQUIRE(j["urls"].contains("remote"));
    REQUIRE(j["urls"].contains("relay"));
    REQUIRE(j["urls"]["relay"] == "");
    REQUIRE(j["urls"]["lan"] == "http://192.168.1.20:13640/r/tok/");
    REQUIRE(j["urls"]["remote"] == "https://pc.tailnet.ts.net/r/tok/");
    // The private key is never a part of this document, whatever was passed in.
    REQUIRE_FALSE(j.contains("private"));
    REQUIRE_FALSE(j.contains("private_key"));
}

TEST_CASE("the pairing document still has all three origins when only the LAN one exists", "[RemoteHub]")
{
    const nlohmann::json j = nlohmann::json::parse(
        pair_identity_json("http://192.168.1.20:13640/r/tok/", "", "", "630dcd2966c43366", std::string(64, '0')));
    REQUIRE(j["urls"]["remote"] == "");
    REQUIRE(j["urls"]["relay"] == "");
    REQUIRE(j["hubid"] == "630dcd2966c43366");
}

// ---- the loopback trust of Tailscale Serve's headers (design section 6.6) ------------------

TEST_CASE("a non-loopback peer never gets Tailscale-User-Login trusted", "[RemoteHub]")
{
    // This is the rule the MAIN listener applies: the LAN listener answers real network peers, and
    // a client on the LAN can put any header it likes in its request. Tailscale Serve's headers
    // are believed only because Serve terminates on loopback and overwrites them, so anything that
    // did not arrive on loopback has ts_login and fwd_proto cleared before login_allowed() is
    // reached. Without this, a phone on the Wi-Fi could send Tailscale-User-Login: <an allow-listed
    // address> and be treated as an authenticated tailnet visitor.
    REQUIRE_FALSE(trusted_proxy_headers(/*peer_is_loopback*/ false, /*via_relay*/ false));
    REQUIRE_FALSE(trusted_proxy_headers(false, true));
    // Genuine Serve: loopback, not relayed.
    REQUIRE(trusted_proxy_headers(true, false));
    // And the trap design section 3 names: a phase-1 relayed stream is re-injected AS a loopback
    // peer (so is_private_v4 need not be loosened) and therefore looks exactly like Serve. Nothing
    // upstream of it strips these headers, so it must never inherit the trust.
    REQUIRE_FALSE(trusted_proxy_headers(true, true));
}

// ---- joining a camera to the printer it watches (summary_json's "camera" field) -------------

TEST_CASE("camera_for_printer matches a camera added as the printer itself", "[RemoteHub][camera]")
{
    const std::vector<CameraCandidate> cams = { { "00M00A000000001", "X1C", "", "" } };
    REQUIRE(camera_for_printer("00M00A000000001", "", cams) == "00M00A000000001");
    // The printer's own id wins even when an address would also have matched something else.
    REQUIRE(camera_for_printer("00M00A000000001", "192.0.2.150", cams) == "00M00A000000001");
}

TEST_CASE("camera_for_printer matches a LAN camera by its recorded ip", "[RemoteHub][camera]")
{
    // The owner's actual reports: streams.json's H2C camera (id is go2rtc's own id, alias "H2C",
    // ip 192.0.2.206) against a Bambu printer row whose name ("Workshop X1C"-style) never matches
    // the camera's alias. Once the row carries the printer's LAN ip, the address join finds it
    // without the name ever entering into it.
    const std::vector<CameraCandidate> cams = {
        { "cmtpxb0uqqhnf2", "H2C", "192.0.2.206", "" },
        { "cmtqambvc4pbba", "X1C", "192.0.2.150", "" },
    };
    REQUIRE(camera_for_printer("some-h2c-serial", "192.0.2.206", cams) == "cmtpxb0uqqhnf2");
    REQUIRE(camera_for_printer("00M00A000000001", "192.0.2.150", cams) == "cmtqambvc4pbba");
}

TEST_CASE("camera_for_printer matches a camera keyed by address under its own id", "[RemoteHub][camera]")
{
    // A camera streams.json stored under its LAN address as the id (the "auto" kind U1 cameras use)
    // rather than a separate `ip` field.
    const std::vector<CameraCandidate> cams = { { "192.0.2.108", "U1", "", "" } };
    REQUIRE(camera_for_printer("printer-1", "192.0.2.108", cams) == "192.0.2.108");
}

TEST_CASE("camera_for_printer falls back to the host inside the camera's own stream URL", "[RemoteHub][camera]")
{
    // A camera whose `ip` was never recorded (added by hostname, or the field is simply blank) but
    // whose rurl/rsrc still names an address - RemoteHub::summary_json reduces those to url_host
    // with SnapmakerLan::host_of before this is called.
    const std::vector<CameraCandidate> cams = { { "camid1", "Shop cam", "", "192.0.2.206" } };
    REQUIRE(camera_for_printer("some-h2c-serial", "192.0.2.206", cams) == "camid1");
}

TEST_CASE("camera_for_printer never matches on name, and never matches two blanks", "[RemoteHub][camera]")
{
    // A printer row with no known address (the gap this fix closes for Bambu, but any kind can hit
    // it) must not pair with a camera that also has no address, and the alias/name is not consulted
    // here at all - that fallback is the app's own, over a printer's *name*.
    const std::vector<CameraCandidate> cams = { { "cam1", "Workshop X1C", "", "" } };
    REQUIRE(camera_for_printer("printer-1", "", cams).empty());
    REQUIRE(camera_for_printer("printer-1", "192.0.2.206", cams).empty());
}

TEST_CASE("camera_for_printer returns empty when nothing matches", "[RemoteHub][camera]")
{
    const std::vector<CameraCandidate> cams = { { "cam1", "X1C", "192.0.2.150", "192.0.2.150" } };
    REQUIRE(camera_for_printer("printer-1", "192.0.2.206", cams).empty());
    REQUIRE(camera_for_printer("printer-1", "", cams).empty());
    REQUIRE(camera_for_printer("printer-1", "192.0.2.206", {}).empty());
}

// ---- merge_printer_row -------------------------------------------------------------------

namespace {
const char* const LIVE_ROW = R"({"id":"X1C","kind":"bambu","connected":true,"bed_temp":60,
    "ams":[{"id":"0","trays":[{"id":"0","exists":true,"type":"PLA","can_load":true,"can_unload":false}]}],
    "ext_spools":[{"ams_id":"254","exists":true,"type":"PETG","can_load":true,"can_unload":false}]})";
const char* const IDLE_ROW = R"({"id":"X1C","kind":"bambu","connected":false,"bed_temp":0,"ams":[],"ext_spools":[{"ams_id":"254","exists":false}]})";
} // namespace

TEST_CASE("merge_printer_row keeps the connected window's fresh row over another window's idle one", "[RemoteHub]")
{
    const MergedPrinterRow m = merge_printer_row(LIVE_ROW, 100, true, IDLE_ROW, 200);
    REQUIRE(m.keep_cached);
}

TEST_CASE("merge_printer_row takes the same window's row, and a row once the live one is stale", "[RemoteHub]")
{
    REQUIRE_FALSE(merge_printer_row(LIVE_ROW, 100, true, IDLE_ROW, 100).keep_cached);
    REQUIRE_FALSE(merge_printer_row(LIVE_ROW, 100, false, IDLE_ROW, 200).keep_cached);
}

TEST_CASE("merge_printer_row carries the last AMS reading into a row with none, load and unload off", "[RemoteHub]")
{
    const MergedPrinterRow m = merge_printer_row(LIVE_ROW, 100, false, IDLE_ROW, 200);
    REQUIRE_FALSE(m.keep_cached);
    const nlohmann::json row = nlohmann::json::parse(m.row);
    REQUIRE(row["bed_temp"] == 0); // everything else is the incoming row's
    REQUIRE(row["ams"][0]["trays"][0]["type"] == "PLA");
    REQUIRE(row["ams"][0]["trays"][0]["can_load"] == false);
    REQUIRE(row["ext_spools"][0]["type"] == "PETG");
    REQUIRE(row["ext_spools"][0]["can_load"] == false);
    REQUIRE(row["ext_spools"][0].contains("filament_why"));
}

TEST_CASE("merge_printer_row never overrides a row that reports filament of its own", "[RemoteHub]")
{
    const char* const other = R"({"id":"X1C","kind":"bambu","connected":true,
        "ams":[{"id":"0","trays":[{"id":"0","exists":true,"type":"ABS"}]}]})";
    const MergedPrinterRow m = merge_printer_row(LIVE_ROW, 100, false, other, 200);
    REQUIRE(nlohmann::json::parse(m.row)["ams"][0]["trays"][0]["type"] == "ABS");
}

// ---- the Device tab's own live view (spike, "liveview_go2rtc") ------------------------------
// The names below were produced by stream_center.html's own hashStr()/encodeURIComponent() in
// node, so a camera on the Stream tab / phone wall and the Device tab's live view of the same
// printer are one go2rtc stream (one connection to the printer), not two.

TEST_CASE("js_hash_str matches stream_center.html's hashStr", "[RemoteHub][own-camera]")
{
    REQUIRE(js_hash_str("") == "811c9dc5");
    REQUIRE(js_hash_str("a") == "e40c292c");
    // Number.toString(16) has no leading zeros, and neither may we.
    REQUIRE(js_hash_str("x2") == "f630cef");
}

TEST_CASE("js_encode_uri_component matches encodeURIComponent", "[RemoteHub][own-camera]")
{
    REQUIRE(js_encode_uri_component("-_.!~*'()az09 %&/=?") == "-_.!~*'()az09%20%25%26%2F%3D%3F");
}

TEST_CASE("own_camera_stream names an RTSPS camera the way the Stream tab does", "[RemoteHub][own-camera]")
{
    const OwnCameraStream a = own_camera_stream("rtsps", "192.168.1.50", "12345678", 0);
    REQUIRE(a.src == "rtspx://bblp:12345678@192.168.1.50:322/streaming/live/1");
    REQUIRE(a.name == "se6b733ea");
    REQUIRE(a.mode == "mse");
    REQUIRE(own_camera_stream("rtsps", "10.0.0.7", "ab(c)d!*", 0).name == "sc1ebfb8f");
    const OwnCameraStream b = own_camera_stream("rtsps", "printer.local", "a&c/d=", 0);
    REQUIRE(b.src == "rtspx://bblp:a%26c%2Fd%3D@printer.local:322/streaming/live/1");
    REQUIRE(b.name == "s268fa2e4");
}

TEST_CASE("own_camera_stream sends a P1/A1 camera through the hub's MJPEG relay", "[RemoteHub][own-camera]")
{
    const OwnCameraStream a = own_camera_stream("local", "10.0.0.9", "abc", 52540);
    REQUIRE(a.src == "http://127.0.0.1:52540/bambu?ip=10.0.0.9&code=abc");
    REQUIRE(a.mode == "mjpeg");
    REQUIRE(a.name == "b4e7ecdb0");
    // Named by the camera, not by the relay port, which changes with every hub run.
    REQUIRE(own_camera_stream("local", "10.0.0.9", "abc", 60000).name == a.name);
    // No relay, no stream.
    REQUIRE(own_camera_stream("local", "10.0.0.9", "abc", 0).name.empty());
}

TEST_CASE("own_camera_stream refuses what BambuCamRelay would refuse", "[RemoteHub][own-camera]")
{
    REQUIRE(own_camera_stream("rtsps", "", "12345678", 0).name.empty());
    REQUIRE(own_camera_stream("rtsps", "10.0.0.7/x", "12345678", 0).name.empty());
    REQUIRE(own_camera_stream("rtsps", "10.0.0.7@evil", "12345678", 0).name.empty());
    REQUIRE(own_camera_stream("rtsps", "10.0.0.7", "", 0).name.empty());
    REQUIRE(own_camera_stream("rtsps", "10.0.0.7", std::string(33, 'a'), 0).name.empty());
    REQUIRE(own_camera_stream("rtsps", "10.0.0.7", "abc def", 0).name.empty());
    REQUIRE(own_camera_stream("rtsps", "10.0.0.7", "abc\ndef", 0).name.empty());
    REQUIRE(own_camera_stream("tutk", "10.0.0.7", "12345678", 0).name.empty());
}

// ---- go2rtc stream registration (register_streams: plan_stream_sync, parse_go2rtc_streams) ----
// go2rtc 1.9.14 opens a second connection to the camera when a stream that viewers are watching is
// PUT again (tests/phone/go2rtc_share_probe.py), and the hub used to re-PUT every camera on every
// Stream-tab save. These pin down the rule that replaced it: only what changed is touched.

namespace {
using StreamMap = std::map<std::string, std::string>;
using LiveMap   = std::map<std::string, Go2rtcLiveStream>;

// Captured from go2rtc 1.9.14's GET /api/streams on loopback: one stream idle, one with a viewer.
const char* const G2R_IDLE = R"({"cam":{"producers":[{"url":"http://127.0.0.1:18995/bambu?ip=x&code=A"}],"consumers":null}})";
const char* const G2R_ACTIVE = R"({"cam":{"producers":[{"id":7,"format_name":"mpjpeg","protocol":"http","remote_addr":"127.0.0.1:18995",
    "url":"http://127.0.0.1:18995/bambu?ip=x&code=A","medias":["video, recvonly, JPEG"],"receivers":[{"id":8,"codec":{"codec_name":"mjpeg",
    "codec_type":"video"},"childs":[9],"bytes":5976,"packets":18}],"bytes_recv":5976}],"consumers":[{"id":6,"format_name":"mjpeg",
    "protocol":"http","remote_addr":"127.0.0.1:54703","medias":["video, sendonly, JPEG, RAW"]}]},"other":{"producers":[],"consumers":null}})";

const std::string RTSPX = "rtspx://bblp:12345678@192.168.1.50:322/streaming/live/1";
const std::string U1A   = "http://127.0.0.1:50001/relay/h264?id=u1&lt=aaaa";
const std::string U1B   = "http://127.0.0.1:50001/relay/h264?id=u1&lt=bbbb";

// The quality variants register_streams() adds beside a stream when there is an ffmpeg.
StreamMap with_variants(StreamMap m)
{
    StreamMap out = m;
    for (const auto& e : m) {
        out[e.first + "_med"] = "ffmpeg:" + e.first + "#video=h264#width=1280#raw=-r#raw=15";
        out[e.first + "_low"] = "ffmpeg:" + e.first + "#video=h264#width=854#raw=-r#raw=10";
    }
    return out;
}

LiveMap live_of(const StreamMap& m, bool active)
{
    LiveMap l;
    for (const auto& e : m) l[e.first] = Go2rtcLiveStream{ active ? std::string("exec:ffmpeg ...") : e.second, active };
    return l;
}

std::set<std::string> names_of(const std::vector<std::pair<std::string, std::string>>& v)
{
    std::set<std::string> s;
    for (const auto& e : v) s.insert(e.first);
    return s;
}

bool nothing_to_do(const StreamSyncPlan& p) { return p.add.empty() && p.replace.empty() && p.remove.empty() && p.adopt.empty(); }
} // namespace

TEST_CASE("parse_go2rtc_streams reads go2rtc's stream list, idle and connected", "[RemoteHub][go2rtc-sync]")
{
    LiveMap l;
    REQUIRE(parse_go2rtc_streams(G2R_IDLE, l));
    REQUIRE(l.size() == 1);
    CHECK(l["cam"].url == "http://127.0.0.1:18995/bambu?ip=x&code=A");
    CHECK_FALSE(l["cam"].active);

    REQUIRE(parse_go2rtc_streams(G2R_ACTIVE, l));
    REQUIRE(l.size() == 2);
    CHECK(l["cam"].active);
    CHECK(l["cam"].url == "http://127.0.0.1:18995/bambu?ip=x&code=A");
    CHECK(l["other"].url.empty()); // listed with no producer: still "go2rtc has it"
    CHECK_FALSE(l["other"].active);

    REQUIRE(parse_go2rtc_streams("{}", l));
    CHECK(l.empty());
}

TEST_CASE("parse_go2rtc_streams refuses anything that is not a stream list", "[RemoteHub][go2rtc-sync]")
{
    LiveMap l;
    l["keep"] = Go2rtcLiveStream{ "x", false };
    CHECK_FALSE(parse_go2rtc_streams("", l));
    CHECK_FALSE(parse_go2rtc_streams("404 page not found", l));
    CHECK_FALSE(parse_go2rtc_streams("[1,2]", l));
    CHECK(l.size() == 1); // untouched
}

TEST_CASE("first registration PUTs every camera and its variants", "[RemoteHub][go2rtc-sync]")
{
    const StreamMap want = with_variants({ { "s1a2b3c4", RTSPX }, { "u1_u1", U1A } });
    const LiveMap   live;
    const StreamSyncPlan p = plan_stream_sync(want, {}, &live);
    CHECK(p.add.size() == want.size());
    CHECK(p.replace.empty());
    CHECK(p.remove.empty());
    CHECK(p.adopt.empty());
}

TEST_CASE("a Stream-tab save with no camera changes touches nothing, watched or not", "[RemoteHub][go2rtc-sync]")
{
    // The bug: every save re-PUT every camera, and the next viewer of a watched one opened a
    // second RTSP session to the printer.
    const StreamMap want = with_variants({ { "s1a2b3c4", RTSPX }, { "u1_u1", U1A } });
    const LiveMap   idle = live_of(want, false), watched = live_of(want, true);
    CHECK(nothing_to_do(plan_stream_sync(want, want, &idle)));
    CHECK(nothing_to_do(plan_stream_sync(want, want, &watched))); // a connected producer's URL is not compared
    CHECK(nothing_to_do(plan_stream_sync(want, want, nullptr)));  // go2rtc's list unreadable: trust what we sent
}

TEST_CASE("a changed source under the same name is replaced, and so are its variants", "[RemoteHub][go2rtc-sync]")
{
    const StreamMap before = with_variants({ { "u1_u1", U1A }, { "s1a2b3c4", RTSPX } });
    StreamMap       after  = before;
    after["u1_u1"]         = U1B;
    const LiveMap   live   = live_of(before, true);
    const StreamSyncPlan p = plan_stream_sync(after, before, &live);
    // The variants' ffmpeg is a viewer of the old u1_u1 object: replacing only the base would keep
    // the old upstream alive for it.
    CHECK(names_of(p.replace) == std::set<std::string>{ "u1_u1", "u1_u1_med", "u1_u1_low" });
    for (const auto& r : p.replace)
        if (r.first == "u1_u1") CHECK(r.second == U1B);
    CHECK(p.add.empty());
    CHECK(p.remove.empty()); // the other camera is left alone
    CHECK(p.adopt.empty());
}

TEST_CASE("a camera whose address or code changed swaps names: new one added, old one deleted", "[RemoteHub][go2rtc-sync]")
{
    // stream_center.html names a go2rtc camera 's' + hash(src), so a new source is a new name.
    const StreamMap before = with_variants({ { "s1a2b3c4", RTSPX } });
    const StreamMap after  = with_variants({ { "s99999999", "rtspx://bblp:87654321@192.168.1.50:322/streaming/live/1" } });
    const LiveMap   live   = live_of(before, true);
    const StreamSyncPlan p = plan_stream_sync(after, before, &live);
    CHECK(names_of(p.add) == std::set<std::string>{ "s99999999", "s99999999_med", "s99999999_low" });
    CHECK(std::set<std::string>(p.remove.begin(), p.remove.end()) == std::set<std::string>{ "s1a2b3c4", "s1a2b3c4_med", "s1a2b3c4_low" });
    CHECK(p.replace.empty());
}

TEST_CASE("a camera taken off the Stream tab is deleted with its variants", "[RemoteHub][go2rtc-sync]")
{
    const StreamMap before = with_variants({ { "s1a2b3c4", RTSPX }, { "u1_u1", U1A } });
    const StreamMap after  = with_variants({ { "u1_u1", U1A } });
    const LiveMap   live   = live_of(before, false);
    const StreamSyncPlan p = plan_stream_sync(after, before, &live);
    CHECK(std::set<std::string>(p.remove.begin(), p.remove.end()) == std::set<std::string>{ "s1a2b3c4", "s1a2b3c4_med", "s1a2b3c4_low" });
    CHECK(p.add.empty());
    CHECK(p.replace.empty());
}

TEST_CASE("only streams this hub registered are ever deleted", "[RemoteHub][go2rtc-sync]")
{
    // e.g. the Device tab's own live view (#448) registers its own names through /hub/camera.
    const LiveMap live = { { "b0badf00", Go2rtcLiveStream{ "http://127.0.0.1:1/bambu?ip=x&code=y", true } } };
    CHECK(nothing_to_do(plan_stream_sync({}, {}, &live)));
}

TEST_CASE("a stream go2rtc lost is registered again", "[RemoteHub][go2rtc-sync]")
{
    const StreamMap want = { { "s1a2b3c4", RTSPX } };
    const LiveMap   live; // e.g. an earlier PUT that go2rtc refused
    const StreamSyncPlan p = plan_stream_sync(want, want, &live);
    CHECK(names_of(p.add) == std::set<std::string>{ "s1a2b3c4" });
}

TEST_CASE("a stream already in go2rtc that this hub did not register", "[RemoteHub][go2rtc-sync]")
{
    const StreamMap want = { { "s1a2b3c4", RTSPX } };
    SECTION("same source, idle: adopted, nothing sent")
    {
        const LiveMap live = { { "s1a2b3c4", Go2rtcLiveStream{ RTSPX, false } } };
        const StreamSyncPlan p = plan_stream_sync(want, {}, &live);
        CHECK(names_of(p.adopt) == std::set<std::string>{ "s1a2b3c4" });
        CHECK(p.add.empty());
        CHECK(p.replace.empty());
    }
    SECTION("someone is watching it: adopted, never disturbed")
    {
        const LiveMap live = { { "s1a2b3c4", Go2rtcLiveStream{ "rtsps://192.168.1.50:322/streaming/live/1", true } } };
        const StreamSyncPlan p = plan_stream_sync(want, {}, &live);
        CHECK(names_of(p.adopt) == std::set<std::string>{ "s1a2b3c4" });
        CHECK(p.replace.empty());
    }
    SECTION("idle with another source: replaced (no viewer to double up)")
    {
        const LiveMap live = { { "s1a2b3c4", Go2rtcLiveStream{ "rtsp://elsewhere/1", false } } };
        const StreamSyncPlan p = plan_stream_sync(want, {}, &live);
        CHECK(names_of(p.replace) == std::set<std::string>{ "s1a2b3c4" });
        CHECK(p.adopt.empty());
    }
}

TEST_CASE("go2rtc's list unreadable: new names are sent, known ones are not", "[RemoteHub][go2rtc-sync]")
{
    const StreamMap registered = { { "s1a2b3c4", RTSPX } };
    const StreamMap want       = { { "s1a2b3c4", RTSPX }, { "u1_u1", U1A } };
    const StreamSyncPlan p = plan_stream_sync(want, registered, nullptr);
    CHECK(names_of(p.add) == std::set<std::string>{ "u1_u1" });
    CHECK(p.replace.empty());
    CHECK(p.remove.empty());
}

TEST_CASE("entries without a name or a source are skipped", "[RemoteHub][go2rtc-sync]")
{
    const LiveMap live;
    CHECK(nothing_to_do(plan_stream_sync({ { "", RTSPX }, { "s1", "" } }, {}, &live)));
}

// ---- the Device tab's live view and the Stream-tab sync (#448 x #450) ------------------------
// The live view (POST /hub/camera) puts its camera into what the sync wants
// (merge_desired_streams), never PUTs on its own. Same printer on the Device tab, the Stream tab
// and the phone = one go2rtc stream, and a Stream-tab save can never delete or replace it.

TEST_CASE("merge_desired_streams adds the live-view cameras and never overrides the Stream tab", "[RemoteHub][own-camera][go2rtc-sync]")
{
    const StreamMap tab  = { { "s1a2b3c4", RTSPX } };
    const StreamMap own  = { { "s1a2b3c4", "rtspx://other" }, { "b4e7ecdb0", "http://127.0.0.1:52540/bambu?ip=10.0.0.9&code=abc" } };
    const StreamMap want = merge_desired_streams(tab, own);
    CHECK(want.size() == 2);
    CHECK(want.at("s1a2b3c4") == RTSPX);
    CHECK(want.at("b4e7ecdb0") == own.at("b4e7ecdb0"));
    CHECK(merge_desired_streams({}, own).size() == 2);
    CHECK(merge_desired_streams(tab, {}) == tab);
}

TEST_CASE("the live view asking for a camera the Stream tab already has sends nothing", "[RemoteHub][own-camera][go2rtc-sync]")
{
    // The phone is watching the Stream-tab camera; the Device tab presses Play on the same printer.
    const OwnCameraStream cam = own_camera_stream("rtsps", "192.168.1.50", "12345678", 0);
    REQUIRE(cam.src == RTSPX);
    const StreamMap tab  = with_variants({ { cam.name, cam.src } });
    const LiveMap   live = live_of(tab, true);
    CHECK(nothing_to_do(plan_stream_sync(merge_desired_streams(tab, { { cam.name, cam.src } }), tab, &live)));
}

TEST_CASE("a Stream-tab save that drops a camera the Device tab plays keeps its stream", "[RemoteHub][own-camera][go2rtc-sync]")
{
    const OwnCameraStream cam   = own_camera_stream("rtsps", "192.168.1.50", "12345678", 0);
    const StreamMap       own   = { { cam.name, cam.src } };
    const StreamMap       before = with_variants({ { cam.name, cam.src }, { "u1_u1", U1A } });
    const StreamMap       after  = with_variants({ { "u1_u1", U1A } }); // the camera taken off the Stream tab
    const LiveMap         live   = live_of(before, true);              // the Device tab is watching it

    SECTION("without the live view in `desired` (#450 alone) the sync would delete it")
    {
        const StreamSyncPlan p = plan_stream_sync(after, before, &live);
        CHECK(std::count(p.remove.begin(), p.remove.end(), cam.name) == 1);
    }
    SECTION("with it, the base stream is neither deleted nor replaced; only the Stream tab's variants go")
    {
        const StreamSyncPlan p = plan_stream_sync(merge_desired_streams(after, own), before, &live);
        CHECK(std::count(p.remove.begin(), p.remove.end(), cam.name) == 0);
        CHECK(names_of(p.replace).count(cam.name) == 0);
        CHECK(p.add.empty());
        CHECK(std::set<std::string>(p.remove.begin(), p.remove.end()) == std::set<std::string>{ cam.name + "_med", cam.name + "_low" });
    }
    SECTION("and a later Stream-tab save that adds it back is unchanged as well")
    {
        StreamMap registered = before;
        registered.erase(cam.name + "_med");
        registered.erase(cam.name + "_low");
        const LiveMap now = live_of(registered, true); // the variants were deleted from go2rtc
        const StreamSyncPlan p = plan_stream_sync(merge_desired_streams(before, own), registered, &now);
        CHECK(names_of(p.replace).count(cam.name) == 0);
        CHECK(std::count(p.remove.begin(), p.remove.end(), cam.name) == 0);
        CHECK(names_of(p.add) == std::set<std::string>{ cam.name + "_med", cam.name + "_low" });
    }
}

TEST_CASE("a P1/A1 live-view camera (b<hash>) is registered once and kept across Stream-tab saves", "[RemoteHub][own-camera][go2rtc-sync]")
{
    const OwnCameraStream cam = own_camera_stream("local", "10.0.0.9", "abc", 52540);
    const StreamMap       own = { { cam.name, cam.src } };
    const StreamMap       tab = { { "u1_u1", U1A } };
    const LiveMap         empty;
    const StreamSyncPlan  first = plan_stream_sync(merge_desired_streams(tab, own), tab, &empty);
    CHECK(names_of(first.add).count(cam.name) == 1);
    const StreamMap registered = merge_desired_streams(tab, own);
    const LiveMap   live       = live_of(registered, true);
    CHECK(nothing_to_do(plan_stream_sync(merge_desired_streams(tab, own), registered, &live)));
    CHECK(nothing_to_do(plan_stream_sync(merge_desired_streams({}, own), merge_desired_streams({}, own), &live)));
}
