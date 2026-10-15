// U1 local-first (owner decision 2026-10-14): a Snapmaker U1 bound to the Snapmaker account is
// served over the LAN whenever it answers there, through the cloud only when it does not.
// Design: tests/design_u1_local_first.md (main checkout).
//
// The unit cases cover the rules (Utils/U1LocalFirst): which topics move, where the printer is,
// the retry pacing, when to fall back, when to ask the printer for approval, the per-slot store,
// the phone hub's key store, and the slot store on disk (GUI/U1LanRoute). They touch neither a
// printer nor the network.
//
// The [U1LocalFirstE2E] cases are hidden: they need mock_u1_mqtt.py (a fake cloud broker and a fake
// U1 pairing + LAN broker on 127.0.0.1). Run them with run_u1_local_first_e2e.py.

#include <catch2/catch.hpp>

#include "slic3r/GUI/U1LanRoute.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/U1LocalFirst.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::U1LocalFirst;
using nlohmann::json;
namespace fs = boost::filesystem;

// ---------------------------------------------------------------- topics ----

TEST_CASE("U1 local-first: the four device topics move, the cloud's own topics stay", "[U1LocalFirst]")
{
    CHECK(device_topic_kind("SN1/request") == DeviceTopic::Request);
    CHECK(device_topic_kind("SN1/response") == DeviceTopic::Response);
    CHECK(device_topic_kind("SN1/status") == DeviceTopic::Status);
    CHECK(device_topic_kind("SN1/notification") == DeviceTopic::Notification);
    CHECK(device_topic_kind("+/response") == DeviceTopic::Response);
    // The cloud session's config topics, the pairing topics, and anything malformed.
    CHECK_FALSE(is_device_topic("SN1/config/response/abcdef"));
    CHECK_FALSE(is_device_topic("SN1/config/notification"));
    CHECK_FALSE(is_device_topic("12345678/config/request"));
    CHECK_FALSE(is_device_topic("/response"));
    CHECK_FALSE(is_device_topic("response"));
    CHECK_FALSE(is_device_topic("#/response"));
    CHECK_FALSE(is_device_topic("SN+1/response"));
    CHECK_FALSE(is_device_topic("SN1/responses"));

    CHECK(serial_of_topic("SN1/request") == "SN1");
    CHECK(serial_of_topic("+/status").empty());
    CHECK(serial_of_topic("SN1/config/request").empty());
}

TEST_CASE("U1 local-first: the route table sends each message to the page once", "[U1LocalFirst]")
{
    RouteTable t;
    CHECK(t.want("SN1/response", 1));
    CHECK_FALSE(t.want("SN1/config/response/x", 1)); // not a device topic: the cloud's
    CHECK(t.wanted("SN1/response"));
    CHECK(t.wanted().size() == 1);

    // On the cloud: cloud device messages through, LAN ones dropped.
    CHECK(t.via() == Via::Cloud);
    CHECK(t.accept(Via::Cloud, "SN1/response"));
    CHECK_FALSE(t.accept(Via::Lan, "SN1/response"));
    CHECK(t.target("SN1/request") == Via::Cloud);

    // On the LAN: the reverse for device topics; the cloud keeps everything else.
    t.set_via(Via::Lan);
    CHECK(t.accept(Via::Lan, "SN1/response"));
    CHECK_FALSE(t.accept(Via::Cloud, "SN1/response"));
    CHECK(t.accept(Via::Cloud, "SN1/config/response/x"));
    CHECK_FALSE(t.accept(Via::Lan, "SN1/config/response/x"));
    CHECK(t.target("SN1/request") == Via::Lan);
    CHECK(t.target("SN1/config/request") == Via::Cloud);

    CHECK(t.unwant("SN1/response"));
    CHECK(t.wanted().empty());
}

// ----------------------------------------------------------- addresses ----

TEST_CASE("U1 local-first: the printer's LAN address from machine.system_info", "[U1LocalFirst]")
{
    const json answer = json::parse(R"({"jsonrpc": "2.0", "id": 3, "result": {"system_info": {"network": {
        "lo":    {"ip_addresses": [{"family": "ipv4", "address": "127.0.0.1"}]},
        "wlan0": {"mac_address": "x", "ip_addresses": [{"family": "ipv4", "address": "192.168.1.50"},
                                                        {"family": "ipv6", "address": "fe80::1", "is_link_local": true}]},
        "eth0":  {"ip_addresses": [{"family": "ipv4", "address": "10.0.0.7"}, {"family": "ipv4", "address": "8.8.8.8"}]}
    }}}})");
    const std::vector<std::string> ips = lan_ipv4_from_system_info(answer);
    REQUIRE(ips.size() == 2);
    CHECK(std::find(ips.begin(), ips.end(), "192.168.1.50") != ips.end());
    CHECK(std::find(ips.begin(), ips.end(), "10.0.0.7") != ips.end());
    // Just the result works too; anything else gives nothing.
    CHECK(lan_ipv4_from_system_info(answer["result"]).size() == 2);
    const json empty_info = { { "result", { { "system_info", json::object() } } } };
    CHECK(lan_ipv4_from_system_info(empty_info).empty());
    CHECK(lan_ipv4_from_system_info(json("text")).empty());

    CHECK(is_private_ipv4("172.16.0.1"));
    CHECK(is_private_ipv4("172.31.255.255"));
    CHECK_FALSE(is_private_ipv4("172.32.0.1"));
    CHECK(is_private_ipv4("100.64.0.1"));
    CHECK_FALSE(is_private_ipv4("127.0.0.1"));
    CHECK_FALSE(is_private_ipv4("192.168.1"));
    CHECK_FALSE(is_private_ipv4("192.168.1.256"));
}

// ---------------------------------------------------------------- pacing ----

TEST_CASE("U1 local-first: LAN retries back off and then repeat every two minutes", "[U1LocalFirst]")
{
    const Timing t;
    CHECK(retry_after_ms(t, 0) == 0);
    CHECK(retry_after_ms(t, 1) == 5000);
    CHECK(retry_after_ms(t, 2) == 15000);
    CHECK(retry_after_ms(t, 4) == 60000);
    CHECK(retry_after_ms(t, 5) == 120000);
    CHECK(retry_after_ms(t, 50) == 120000);
}

TEST_CASE("U1 local-first: when the LAN session counts as gone", "[U1LocalFirst]")
{
    const Timing t;
    LanHealth    h;
    CHECK_FALSE(should_fall_back(t, h));
    h.mqtt_connected = false;
    h.down_for_ms    = 2999; // a reconnect blip is ridden out
    CHECK_FALSE(should_fall_back(t, h));
    h.down_for_ms = 3000;
    CHECK(should_fall_back(t, h));
    // Connected but the printer stopped answering on port 80 (power off, cable out, Wi-Fi gone):
    LanHealth p;
    p.probe_fails = 1;
    CHECK_FALSE(should_fall_back(t, p));
    p.probe_fails = 2;
    CHECK(should_fall_back(t, p));
}

// ------------------------------------------------------------- approval ----

TEST_CASE("U1 local-first: reading the printer's pairing answers", "[U1LocalFirst]")
{
    // (No raw string literals inside CHECK: MSVC cannot stringize them in a macro.)
    const json success   = { { "state", "success" }, { "ca", "a" }, { "cert", "b" }, { "key", "c" } };
    const json half      = { { "state", "success" }, { "ca", "a" } };
    const json unauth    = { { "state", "unauthorized" } };
    const json asking    = { { "state", "authorizing" } };
    const json not_lan   = { { "state", "error" }, { "message", "not LAN" } };
    CHECK(parse_auth_answer(success) == AuthAnswer::Success);
    // "success" without the certificate cannot be used.
    CHECK(parse_auth_answer(half) == AuthAnswer::NotAvailable);
    CHECK(parse_auth_answer(unauth) == AuthAnswer::Unauthorized);
    CHECK(parse_auth_answer(asking) == AuthAnswer::Authorizing);
    CHECK(parse_auth_answer(not_lan) == AuthAnswer::NotAvailable);
    CHECK(parse_auth_answer(json(nullptr)) == AuthAnswer::NoAnswer);
}

TEST_CASE("U1 local-first: the log line for a pairing answer never carries the certificate", "[U1LocalFirst]")
{
    const json refused = { { "state", "error" }, { "message", "not LAN mode" } };
    CHECK(describe_auth_answer(refused) == "state=error, message=not LAN mode, certificate=no");
    const json granted = { { "state", "success" }, { "ca", "CA-PEM" }, { "cert", "CERT-PEM" }, { "key", "KEY-PEM" }, { "clientid", "orca-x" } };
    const std::string line = describe_auth_answer(granted);
    CHECK(line == "state=success, message=-, certificate=yes");
    CHECK(line.find("PEM") == std::string::npos);
    CHECK(line.find("orca-x") == std::string::npos);
    CHECK(describe_auth_answer(json(nullptr)) == "no answer");
}

TEST_CASE("U1 local-first: the printer is asked once per session, and not after two unanswered asks", "[U1LocalFirst]")
{
    const Timing t;
    AskState     s;
    CHECK(next_auth_step(t, AuthAnswer::Success, s) == AuthStep::UseLan);
    CHECK(next_auth_step(t, AuthAnswer::Unauthorized, s) == AuthStep::AskApproval);
    CHECK(next_auth_step(t, AuthAnswer::NotAvailable, s) == AuthStep::StayCloud);
    CHECK(next_auth_step(t, AuthAnswer::NoAnswer, s) == AuthStep::StayCloud);
    s.asked_this_session = true;
    CHECK(next_auth_step(t, AuthAnswer::Unauthorized, s) == AuthStep::StayCloud);
    // An approval that arrives later (the person tapped Allow after the wait) is still used.
    CHECK(next_auth_step(t, AuthAnswer::Success, s) == AuthStep::UseLan);
    AskState twice;
    twice.asks = 2;
    CHECK(next_auth_step(t, AuthAnswer::Unauthorized, twice) == AuthStep::StayCloud);
}

// ------------------------------------------------------------ slot store ----

TEST_CASE("U1 local-first: each slot has its own client id; slots never overwrite each other", "[U1LocalFirst]")
{
    Protector p; // identity
    SlotStore a = SlotStore::from_json(json::object());
    const std::string id0 = a.client_id(0, p);
    CHECK(id0.rfind("orca-", 0) == 0);
    CHECK(id0.size() == 5 + 36);
    CHECK(a.client_id(0, p) == id0); // stable
    a.set_authorized(0, "SN1", true);

    // Another process, slot 1, read the file before slot 0 wrote it.
    SlotStore b = SlotStore::from_json(json::object());
    const std::string id1 = b.client_id(1, p);
    CHECK(id1 != id0);
    b.count_unanswered_ask(1, "SN1");

    json disk = json::object();
    a.merge_into(disk, 0);
    b.merge_into(disk, 1);
    SlotStore both = SlotStore::from_json(disk);
    CHECK(both.client_id(0, p) == id0);
    CHECK(both.client_id(1, p) == id1);
    CHECK(both.ask_state(0, "SN1").authorized);
    CHECK_FALSE(both.ask_state(1, "SN1").authorized);
    CHECK(both.ask_state(1, "SN1").asks == 1);
    CHECK(both.ask_state(0, "SN2").asks == 0);

    // An approval clears the unanswered count.
    both.set_authorized(1, "SN1", true);
    CHECK(both.ask_state(1, "SN1").asks == 0);
}

TEST_CASE("U1 local-first: a client id that cannot be read back is replaced, with its approvals", "[U1LocalFirst]")
{
    Protector locked;
    locked.protect   = [](const std::string& s) { return "x:" + s; };
    locked.unprotect = [](const std::string&) { return std::string(); }; // e.g. another user's DPAPI blob
    SlotStore s = SlotStore::from_json(json::parse(R"({"version": 1, "slots": {"0": {"client_id": "x:orca-old",
        "printers": {"SN1": {"authorized": true}}}}})"));
    const std::string id = s.client_id(0, locked);
    CHECK(id != "orca-old");
    CHECK_FALSE(s.ask_state(0, "SN1").authorized);
    CHECK(s.to_json()["slots"]["0"]["client_id"].get<std::string>() == "x:" + id);
}

TEST_CASE("U1 local-first: the lowest free slot is taken", "[U1LocalFirst]")
{
    std::set<int> taken = { 0, 1 };
    CHECK(pick_slot([&taken](int n) { return taken.insert(n).second; }) == 2);
    CHECK(pick_slot([](int) { return false; }, 4) == -1);
}

namespace {
struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("u1lan-%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};
} // namespace

TEST_CASE("U1 local-first: two slicers on one data dir get two slots and two client ids", "[U1LocalFirst]")
{
    TempDir dir;
    std::string first_id;
    {
        GUI::U1LanSlotStore a(dir.path.string());
        GUI::U1LanSlotStore b(dir.path.string());
        CHECK(a.slot() == 0);
        CHECK(b.slot() == 1);
        first_id = a.client_id();
        CHECK_FALSE(first_id.empty());
        CHECK(b.client_id() != first_id);
        a.set_authorized("SN1", true);
        b.count_unanswered_ask("SN1");
        CHECK(a.ask_state("SN1").authorized);
        CHECK(b.ask_state("SN1").asks == 1);
        CHECK_FALSE(b.ask_state("SN1").authorized);
    }
    // Both gone: the next one takes slot 0 again, with the same id and what the printer said.
    GUI::U1LanSlotStore c(dir.path.string());
    CHECK(c.slot() == 0);
    CHECK(c.client_id() == first_id);
    CHECK(c.ask_state("SN1").authorized);

    // The id is not stored in the clear on Windows.
    boost::nowide::ifstream in((dir.path / "u1_lan_auth.json").string());
    json disk;
    in >> disk;
    const std::string stored = disk["slots"]["0"]["client_id"].get<std::string>();
#ifdef _WIN32
    CHECK(stored.rfind("dpapi:", 0) == 0);
    CHECK(stored.find(first_id) == std::string::npos);
#else
    CHECK(stored == first_id);
#endif
}

// --------------------------------------------- phone hub key store ----

TEST_CASE("U1 local-first: the phone hub keeps LAN connects only, per page origin", "[U1LocalFirst]")
{
    const json lan   = { { "link_mode", "lan" }, { "ca", "a" }, { "cert", "b" }, { "key", "c" }, { "clientId", "orca-1" } };
    json       cloud = lan;
    cloud["link_mode"] = "wan";
    CHECK(should_remember_keys(lan));
    CHECK_FALSE(should_remember_keys(cloud));
    json bare = lan;
    bare.erase("link_mode"); // the Device page's default is LAN
    CHECK(should_remember_keys(bare));
    json no_key = lan;
    no_key["key"] = "";
    CHECK_FALSE(should_remember_keys(no_key));

    // A version-1 file (flat, mostly cloud ids) is never used and is replaced on the next write.
    const json v1 = json::parse(R"({"SN1": {"ca": "a", "cert": "b", "key": "c", "clientId": "0123abcd"}})");
    CHECK(keys_get(v1, "SN1", "13619").is_null());

    json all = keys_put(v1, "SN1", "13619", { { "clientId", "orca-main" } });
    all      = keys_put(all, "SN1", "13620", { { "clientId", "orca-second" } });
    CHECK(all["version"] == 2);
    // Each instance gets its own page's id back, never the other's.
    CHECK(keys_get(all, "SN1", "13619")["clientId"] == "orca-main");
    CHECK(keys_get(all, "SN1", "13620")["clientId"] == "orca-second");
    CHECK(keys_get(all, "SN1", "13621").is_null());
    CHECK(keys_get(all, "SN2", "13619").is_null());
}

// ------------------------------------------------------- end to end ----

namespace {

struct Mock
{
    int         cloud { 0 }, lan { 0 }, pairing { 0 }, control { 0 };
    // The mock printer's pairing access code. run_u1_local_first_e2e.py sets one that is NOT the
    // U1 default, so a route that pairs before the page handed it the code gets no answer
    // (owner hand test 2026-10-15: the first tries went out on the default code).
    std::string code { "12345678" };
    bool        ok() const { return cloud && lan && pairing && control; }
};

Mock mock_ports()
{
    Mock        m;
    const char* v = std::getenv("U1MOCK_PORTS");
    if (v)
        std::sscanf(v, "%d:%d:%d:%d", &m.cloud, &m.lan, &m.pairing, &m.control);
    const char* c = std::getenv("U1MOCK_CODE");
    if (c && *c)
        m.code = c;
    return m;
}

json ctl(const Mock& m, const std::string& path)
{
    json out;
    Http::get("http://127.0.0.1:" + std::to_string(m.control) + path)
        .timeout_max(5)
        .on_complete([&out](std::string body, unsigned) { out = json::parse(body, nullptr, false); })
        .on_error([&out](std::string body, std::string, unsigned) { out = json::parse(body, nullptr, false); })
        .perform_sync();
    return out;
}

int stat_of(const Mock& m, const char* a, const char* b = nullptr)
{
    const json s = ctl(m, "/stats");
    if (!s.is_object() || !s.contains(a))
        return -1;
    const json& v = b ? s[a][b] : s[a];
    return v.is_number_integer() ? v.get<int>() : -1;
}

template<class Pred> bool wait_until(Pred pred, int timeout_ms)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < until) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

// The page's side: one cloud session, what it receives, and the route on it.
struct Page
{
    std::string                        sn;
    std::shared_ptr<MqttClient>        cloud;
    std::shared_ptr<GUI::U1LanRoute>   route;
    std::shared_ptr<std::mutex>        mtx = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<json>> got = std::make_shared<std::vector<json>>();
    struct Store
    {
        std::mutex             m;
        U1LocalFirst::AskState ask;
        int                    approvals_requested { 0 };
    };
    std::shared_ptr<Store> store = std::make_shared<Store>();

    U1LocalFirst::AskState ask_state() const
    {
        std::lock_guard<std::mutex> lk(store->m);
        return store->ask;
    }
    int approvals() const
    {
        std::lock_guard<std::mutex> lk(store->m);
        return store->approvals_requested;
    }

    json answer(long long id) const
    {
        std::lock_guard<std::mutex> lk(*mtx);
        for (const json& j : *got)
            if (j.is_object() && j.value("id", -1LL) == id)
                return j;
        return nullptr;
    }
    int answers(long long id) const
    {
        std::lock_guard<std::mutex> lk(*mtx);
        int n = 0;
        for (const json& j : *got)
            n += j.is_object() && j.value("id", -1LL) == id;
        return n;
    }
    bool ask(long long id)
    {
        std::string msg;
        return cloud->Publish(sn + "/request", json({ { "jsonrpc", "2.0" }, { "method", "x" }, { "id", id } }).dump(), 1, msg);
    }
    std::string via(long long id, int timeout_ms = 5000)
    {
        wait_until([&] { return !answer(id).is_null(); }, timeout_ms);
        const json a = answer(id);
        return a.is_null() ? std::string("none") : a["result"].value("via", "");
    }
};

// `engine_delay_ms`: how long after its subscriptions the "page" calls set_engine (which hands the
// route the serial number and the pairing access code); -1 = never.
Page open_page(const Mock& m, const std::string& sn, const std::string& client_id, const std::string& cloud_id, Timing timing,
               int engine_delay_ms = 0)
{
    Page p;
    p.sn    = sn;
    p.cloud = MqttClient::create("mqtt://127.0.0.1:" + std::to_string(m.cloud), cloud_id);
    auto got = p.got;
    auto mtx = p.mtx;
    p.cloud->SetMessageCallback([got, mtx, sn](const std::string& topic, const std::string& payload, void*) {
        if (topic != sn + "/response")
            return;
        std::lock_guard<std::mutex> lk(*mtx);
        got->push_back(json::parse(payload, nullptr, false));
    });
    std::string msg;
    REQUIRE(p.cloud->Connect(msg));

    GUI::U1LanDeps d;
    d.timing          = timing;
    d.known_addresses = [](const std::string&) { return std::vector<std::string> { "127.0.0.1" }; };
    d.is_that_printer = [sn](const std::string&, const std::string& asked) { return asked == sn; };
    d.probe           = [m](const std::string&) { return ctl(m, "/lan/state").value("up", false); };
    d.pairing         = [m](const std::string& ip, const std::string& code, const std::string& method, const json& params) {
        return GUI::u1_pairing_exchange(ip, m.pairing, code, method, params, 3000);
    };
    d.make_lan_client = [m](const std::string&, const GUI::U1LanCreds& c) {
        // The mock's LAN broker is plain MQTT; the app's session is MQTTS with c.ca/cert/key.
        return MqttClient::create("mqtt://127.0.0.1:" + std::to_string(m.lan), c.client_id);
    };
    d.client_id = [client_id]() { return client_id; };
    auto store  = p.store;
    d.ask_state = [store](const std::string&) {
        std::lock_guard<std::mutex> lk(store->m);
        return store->ask;
    };
    d.set_authorized = [store](const std::string&, bool a) {
        std::lock_guard<std::mutex> lk(store->m);
        store->ask.authorized = a;
        if (a)
            store->ask.asks = 0;
    };
    d.count_unanswered_ask = [store](const std::string&) {
        std::lock_guard<std::mutex> lk(store->m);
        ++store->ask.asks;
    };
    d.approval_requested = [store](const std::string&) {
        std::lock_guard<std::mutex> lk(store->m);
        ++store->approvals_requested;
    };
    p.route = GUI::U1LanRoute::attach(p.cloud, d);
    // What the page does first: subscribe the device topics on its "cloud" session (the route
    // learns the serial number here), then set_engine with the access code.
    REQUIRE(p.cloud->Subscribe(sn + "/response", 1, msg));
    REQUIRE(p.cloud->Subscribe(sn + "/notification", 1, msg));
    CHECK(p.cloud->IsSubscribed(sn + "/response"));
    if (engine_delay_ms >= 0) {
        if (engine_delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(engine_delay_ms));
        p.route->set_device(sn, m.code);
    }
    return p;
}

void close_page(Page& p)
{
    CHECK(p.route->stop_and_wait(15000));
    std::string msg;
    p.cloud->Disconnect(msg);
}

Timing fast_timing()
{
    Timing t;
    t.retry_ms              = { 300, 600, 1000 };
    t.lan_down_grace_ms     = 500;
    t.lan_probe_interval_ms = 300;
    t.lan_probe_fails       = 2;
    t.approval_poll_ms      = 200;
    t.approval_wait_ms      = 6000;
    t.device_wait_ms        = 4000;
    return t;
}

} // namespace

TEST_CASE("U1 local-first end to end: an approved PC moves to the LAN, falls back, comes back", "[.][U1LocalFirstE2E]")
{
    GUI::U1LanRoute::reset_pacing();
    const Mock m = mock_ports();
    REQUIRE(m.ok());
    ctl(m, "/lan/up");
    ctl(m, "/authorize?cid=orca-e2e-a");
    const int kicks_before = stat_of(m, "kicks");
    Page p = open_page(m, "U1E2EA0001", "orca-e2e-a", "cloud-e2e-a", fast_timing());

    REQUIRE(wait_until([&] { return p.route->on_lan(); }, 15000));
    CHECK(p.route->serial() == "U1E2EA0001");
    CHECK(p.ask_state().authorized);
    CHECK(p.approvals() == 0);
    // The page's own calls now reach the printer's broker ...
    CHECK(p.cloud->IsSubscribed("U1E2EA0001/response"));
    REQUIRE(p.ask(101));
    CHECK(p.via(101) == "lan");
    // ... and a cloud answer no longer reaches the page (no duplicates): ask the cloud directly.
    std::string msg;
    REQUIRE(p.cloud->PublishDirect("U1E2EA0001/request", json({ { "method", "x" }, { "id", 102 } }).dump(), 1, msg));
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    CHECK(p.answers(102) == 0);
    CHECK(p.answers(101) == 1);

    // The printer leaves the LAN: back on the cloud within the grace + probe time.
    ctl(m, "/lan/down");
    REQUIRE(wait_until([&] { return !p.route->on_lan(); }, 8000));
    REQUIRE(p.ask(103));
    CHECK(p.via(103) == "cloud");
    CHECK(p.answers(103) == 1);

    // It comes back: on the LAN again without anyone asking.
    ctl(m, "/lan/up");
    REQUIRE(wait_until([&] { return p.route->on_lan(); }, 15000));
    REQUIRE(p.ask(104));
    CHECK(p.via(104) == "lan");

    close_page(p);
    CHECK(stat_of(m, "kicks") == kicks_before);
}

TEST_CASE("U1 local-first end to end: pairing waits for the page's access code, never a default", "[.][U1LocalFirstE2E]")
{
    GUI::U1LanRoute::reset_pacing();
    const Mock m = mock_ports();
    REQUIRE(m.ok());
    REQUIRE(m.code != "12345678"); // the runner gives the mock a code of its own
    ctl(m, "/lan/up");
    ctl(m, "/authorize?cid=orca-e2e-code");
    const int wrong_before = stat_of(m, "wrong_code_requests");
    // set_engine (with the code) arrives 1.5 s after the subscriptions, as on a slow page.
    Page p = open_page(m, "U1E2ECODE7", "orca-e2e-code", "cloud-e2e-code", fast_timing(), 1500);
    REQUIRE(wait_until([&] { return p.route->on_lan(); }, 15000));
    // Every pairing request went to the printer's own code: nothing on the default topic.
    CHECK(stat_of(m, "wrong_code_requests") == wrong_before);
    REQUIRE(p.ask(151));
    CHECK(p.via(151) == "lan");
    close_page(p);
}

TEST_CASE("U1 local-first end to end: a printer that refuses LAN is retried on the back-off, across new sessions",
          "[.][U1LocalFirstE2E]")
{
    GUI::U1LanRoute::reset_pacing();
    const Mock m = mock_ports();
    REQUIRE(m.ok());
    ctl(m, "/lan/up");
    ctl(m, "/pairing/mode?m=error");
    Timing t   = fast_timing();
    t.retry_ms = { 3000, 8000 };
    const int checks_before = stat_of(m, "pairing", "confirm_lan_status");

    Page first = open_page(m, "U1E2ENOLAN", "orca-e2e-refused", "cloud-e2e-r0", t);
    REQUIRE(wait_until([&] { return stat_of(m, "pairing", "confirm_lan_status") == checks_before + 1; }, 10000));
    close_page(first);
    // The page reconnects (a new cloud session, a new route) several times within the back-off:
    // none of them asks the printer again before the 3 s are up.
    for (int i = 0; i < 4; ++i) {
        Page again = open_page(m, "U1E2ENOLAN", "orca-e2e-refused", "cloud-e2e-r" + std::to_string(i + 1), t);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        close_page(again);
    }
    CHECK(stat_of(m, "pairing", "confirm_lan_status") == checks_before + 1);
    // After the back-off one more check, and still on the cloud.
    Page last = open_page(m, "U1E2ENOLAN", "orca-e2e-refused", "cloud-e2e-r9", t);
    CHECK(wait_until([&] { return stat_of(m, "pairing", "confirm_lan_status") == checks_before + 2; }, 6000));
    CHECK_FALSE(last.route->on_lan());
    CHECK(last.approvals() == 0); // an "error" answer is not a reason to prompt
    REQUIRE(last.ask(161));
    CHECK(last.via(161) == "cloud");
    close_page(last);
    ctl(m, "/pairing/mode?m=normal");
}

TEST_CASE("U1 local-first end to end: a new PC asks once, is approved on the printer, and connects", "[.][U1LocalFirstE2E]")
{
    GUI::U1LanRoute::reset_pacing();
    const Mock m = mock_ports();
    REQUIRE(m.ok());
    ctl(m, "/lan/up");
    Page p = open_page(m, "U1E2ENEW01", "orca-e2e-new", "cloud-e2e-new", fast_timing());

    // The printer shows its prompt (the mock holds the id as pending) and the PC says so.
    REQUIRE(wait_until([&] {
        const json s = ctl(m, "/stats");
        return s.contains("pending") && s["pending"].dump().find("orca-e2e-new") != std::string::npos;
    }, 15000));
    CHECK(wait_until([&] { return p.approvals() == 1; }, 3000));
    // Until then the cloud serves.
    REQUIRE(p.ask(201));
    CHECK(p.via(201) == "cloud");

    ctl(m, "/approve?cid=orca-e2e-new");
    REQUIRE(wait_until([&] { return p.route->on_lan(); }, 10000));
    REQUIRE(p.ask(202));
    CHECK(p.via(202) == "lan");
    CHECK(p.ask_state().authorized);
    close_page(p);
}

TEST_CASE("U1 local-first end to end: an unanswered prompt keeps the cloud and is not repeated", "[.][U1LocalFirstE2E]")
{
    GUI::U1LanRoute::reset_pacing();
    const Mock m = mock_ports();
    REQUIRE(m.ok());
    ctl(m, "/lan/up");
    const int asked_before = stat_of(m, "pairing", "request_lan_auth");
    Timing t = fast_timing();
    t.approval_wait_ms = 1000;
    Page p = open_page(m, "U1E2EIGN01", "orca-e2e-ignored", "cloud-e2e-ignored", t);

    REQUIRE(wait_until([&] { return p.ask_state().asks == 1; }, 15000));
    // Several more LAN tries (300/600/1000 ms apart) without a second prompt this session.
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    CHECK_FALSE(p.route->on_lan());
    CHECK(stat_of(m, "pairing", "request_lan_auth") == asked_before + 1);
    CHECK(p.approvals() == 1);
    REQUIRE(p.ask(301));
    CHECK(p.via(301) == "cloud");
    close_page(p);
}

TEST_CASE("U1 local-first end to end: two slicers on one printer keep their own LAN sessions", "[.][U1LocalFirstE2E]")
{
    GUI::U1LanRoute::reset_pacing();
    const Mock m = mock_ports();
    REQUIRE(m.ok());
    ctl(m, "/lan/up");
    ctl(m, "/authorize?cid=orca-e2e-slot0");
    ctl(m, "/authorize?cid=orca-e2e-slot1");
    const int kicks_before = stat_of(m, "kicks");
    Page a = open_page(m, "U1E2ETWO01", "orca-e2e-slot0", "cloud-e2e-p0", fast_timing());
    Page b = open_page(m, "U1E2ETWO01", "orca-e2e-slot1", "cloud-e2e-p1", fast_timing());
    REQUIRE(wait_until([&] { return a.route->on_lan() && b.route->on_lan(); }, 15000));

    for (long long id = 401; id < 411; ++id) {
        REQUIRE(a.ask(id));
        REQUIRE(b.ask(id + 100));
    }
    for (long long id = 401; id < 411; ++id) {
        CHECK(a.via(id) == "lan");
        CHECK(b.via(id + 100) == "lan");
    }
    CHECK(stat_of(m, "kicks") == kicks_before);
    const std::string lan_sessions = ctl(m, "/stats")["sessions"]["lan"].dump();
    CHECK(lan_sessions.find("orca-e2e-slot0") != std::string::npos);
    CHECK(lan_sessions.find("orca-e2e-slot1") != std::string::npos);
    CHECK(a.route->on_lan());
    CHECK(b.route->on_lan());
    close_page(a);
    close_page(b);
}

TEST_CASE("U1 local-first end to end: the pairing exchange against the mock", "[.][U1LocalFirstE2E]")
{
    const Mock m = mock_ports();
    REQUIRE(m.ok());
    const json no = GUI::u1_pairing_exchange("127.0.0.1", m.pairing, m.code, "server.client_manager.confirm_lan_status",
                                             { { "clientid", "orca-e2e-px" } }, 3000);
    CHECK(parse_auth_answer(no) == AuthAnswer::Unauthorized);
    ctl(m, "/authorize?cid=orca-e2e-px");
    const json yes = GUI::u1_pairing_exchange("127.0.0.1", m.pairing, m.code, "server.client_manager.confirm_lan_status",
                                              { { "clientid", "orca-e2e-px" } }, 3000);
    REQUIRE(parse_auth_answer(yes) == AuthAnswer::Success);
    CHECK(yes["port"] == m.lan);
    // The wrong access code: nothing answers on that topic.
    const json none = GUI::u1_pairing_exchange("127.0.0.1", m.pairing, "00000000", "server.client_manager.confirm_lan_status",
                                               { { "clientid", "orca-e2e-px" } }, 1000);
    CHECK(none.is_null());
    // Nobody listening at all.
    CHECK(GUI::u1_pairing_exchange("127.0.0.1", 1, m.code, "x", json::object(), 1000).is_null());
}
