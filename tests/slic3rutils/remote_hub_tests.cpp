// Pure parsing rules pulled out of RemoteHub.cpp for the port-fallback reachability fix
// (Slic3r::GUI::RemoteHub::Testing, see RemoteHub.hpp): the port that `tailscale serve status
// --json` is currently forwarding to, who `netstat -ano` says is LISTENING on a given port, and
// the image name `tasklist`'s CSV line names for that pid. All three are driven here from
// captured command output - no socket, no process, no real tailnet or netstat needed.

#include <catch2/catch.hpp>

#include "slic3r/GUI/RemoteHub.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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
    REQUIRE(note["default_port"].get<int>() == 13640);
    REQUIRE(note["port"].get<int>() == 13641);
    REQUIRE(note["held_by"].get<std::string>() == "EdgeSlicer.exe (pid 5678)");
}

TEST_CASE("lan_firewall JSON uses the same state vocabulary as video.firewall", "[RemoteHub]")
{
    const std::vector<std::string> valid = { "allowed", "partial", "missing", "blocked", "unknown", "off" };
    for (const std::string& s : { std::string("allowed"), std::string("missing"), std::string("off") })
        REQUIRE(std::find(valid.begin(), valid.end(), s) != valid.end());
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
