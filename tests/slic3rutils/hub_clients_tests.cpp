// The hub's per-client tokens (HubClients.hpp): which scope each phone-listener route needs, the
// scope names, the settings.json round trip (a hash, never the token), the rate limits and the
// hash itself (Testing::client_token_hash in RemoteHub.cpp). All pure.

#include <catch2/catch.hpp>

#include "slic3r/GUI/HubClients.hpp"
#include "slic3r/GUI/RemoteHub.hpp"

#include <nlohmann/json.hpp>

using namespace Slic3r::GUI::HubClients;
using Slic3r::GUI::RemoteHub::Testing::client_token_hash;

TEST_CASE("scope names parse and print", "[RemoteHub][clients]")
{
    REQUIRE(parse_scopes("status") == SCOPE_STATUS);
    REQUIRE(parse_scopes("status,camera") == (SCOPE_STATUS | SCOPE_CAMERA));
    REQUIRE(parse_scopes("Print + CONTROL") == (SCOPE_PRINT | SCOPE_CONTROL));
    REQUIRE(parse_scopes("full") == SCOPE_FULL);
    REQUIRE(parse_scopes("admin,root") == SCOPE_NONE);
    REQUIRE(parse_scope_list(nlohmann::json::array({ "camera", "nonsense", 7 })) == SCOPE_CAMERA);
    REQUIRE(scope_names(SCOPE_STATUS | SCOPE_PRINT) == std::vector<std::string>{ "status", "print" });
    REQUIRE(scope_names(SCOPE_FULL).size() == 4);
}

TEST_CASE("allows: every needed bit, and ANY means at least one scope", "[RemoteHub][clients]")
{
    REQUIRE(allows(SCOPE_FULL, SCOPE_PRINT));
    REQUIRE(allows(SCOPE_STATUS, SCOPE_STATUS));
    REQUIRE_FALSE(allows(SCOPE_STATUS, SCOPE_CONTROL));
    REQUIRE(allows(SCOPE_CAMERA, SCOPE_ANY));
    REQUIRE_FALSE(allows(SCOPE_NONE, SCOPE_ANY));
    REQUIRE_FALSE(allows(SCOPE_FULL, SCOPE_NONE));
}

TEST_CASE("route_scope: the page and its furniture are open to every client", "[RemoteHub][clients]")
{
    for (const char* r : { "", "/", "/index.html", "/state", "/pair", "/manifest.webmanifest", "/sw.js", "/ping.gif", "/icon-192.png", "/apple-touch-icon.png" })
        REQUIRE(route_scope("GET", r) == SCOPE_ANY);
    // ... but never a way to change anything.
    REQUIRE(route_scope("POST", "/state") == SCOPE_CONTROL);
}

TEST_CASE("route_scope: reading", "[RemoteHub][clients]")
{
    REQUIRE(route_scope("GET", "/summary") == SCOPE_STATUS);
    REQUIRE(route_scope("GET", "/events") == SCOPE_STATUS);
    REQUIRE(route_scope("GET", "/api/instances") == SCOPE_STATUS);
    REQUIRE(route_scope("GET", "/api/archive") == SCOPE_STATUS);
    REQUIRE(route_scope("GET", "/printers/abc/thumbnail.png") == SCOPE_STATUS);
    REQUIRE(route_scope("GET", "/i/42/api/printers") == SCOPE_STATUS);
    REQUIRE(route_scope("GET", "/i/42/api/jobs/7") == SCOPE_STATUS);
    REQUIRE(route_scope("GET", "/i/42/api/plates/1/preview.png") == SCOPE_STATUS);
    REQUIRE(route_scope("POST", "/push/device") == SCOPE_STATUS);
    REQUIRE(route_scope("DELETE", "/push/subscription") == SCOPE_STATUS);
    REQUIRE(route_scope("GET", "/something/new") == SCOPE_STATUS);
}

TEST_CASE("route_scope: cameras", "[RemoteHub][clients]")
{
    REQUIRE(route_scope("GET", "/bambu") == SCOPE_CAMERA);
    REQUIRE(route_scope("GET", "/ff") == SCOPE_CAMERA);
    REQUIRE(route_scope("GET", "/i/42/api/printers/dev1/timelapses") == SCOPE_CAMERA);
    REQUIRE(route_scope("GET", "/i/42/api/printers/dev1/timelapses/video") == SCOPE_CAMERA);
}

TEST_CASE("route_scope: printing", "[RemoteHub][clients]")
{
    REQUIRE(route_scope("POST", "/api/instances/open") == SCOPE_PRINT);
    REQUIRE(route_scope("POST", "/i/42/open") == SCOPE_PRINT);
    REQUIRE(route_scope("POST", "/i/42/api/slice") == SCOPE_PRINT);
    REQUIRE(route_scope("POST", "/i/42/api/plates/2/send") == SCOPE_PRINT);
    REQUIRE(route_scope("POST", "/i/42/api/archive/rec-1/send") == SCOPE_PRINT);
    REQUIRE(route_scope("POST", "/i/42/api/archive/rec-1/preview") == SCOPE_PRINT);
    REQUIRE(route_scope("POST", "/i/42/api/presets/select") == SCOPE_PRINT);
    REQUIRE(route_scope("POST", "/i/42/api/settings/process/save") == SCOPE_PRINT);
    REQUIRE(route_scope("POST", "/i/42/api/project/open") == SCOPE_PRINT);
}

TEST_CASE("route_scope: control, and anything unlisted that changes something", "[RemoteHub][clients]")
{
    REQUIRE(route_scope("POST", "/i/42/api/printers/dev1/control") == SCOPE_CONTROL);
    REQUIRE(route_scope("POST", "/i/42/api/quit") == SCOPE_CONTROL);
    REQUIRE(route_scope("POST", "/i/42/api/window") == SCOPE_CONTROL);
    REQUIRE(route_scope("POST", "/i/42/api/snapmaker/connect") == SCOPE_CONTROL);
    REQUIRE(route_scope("POST", "/i/42/api/archive/rec-1/delete") == SCOPE_CONTROL);
    REQUIRE(route_scope("DELETE", "/i/42/api/archive/rec-1") == SCOPE_CONTROL);
    REQUIRE(route_scope("POST", "/i/42/api/brand-new-route") == SCOPE_CONTROL);
    REQUIRE(route_scope("PUT", "/anything") == SCOPE_CONTROL);
}

TEST_CASE("a client survives the settings.json round trip as a hash", "[RemoteHub][clients]")
{
    Client c;
    c.id         = "c0123456789ab";
    c.name       = "Workshop PC";
    c.hash       = client_token_hash("abcdefghijkmnpqrstuvwxyz23");
    c.scopes     = SCOPE_STATUS | SCOPE_PRINT;
    c.tagged_ok  = true;
    c.created_at = 1760000000;
    const nlohmann::json j = to_settings(c);
    REQUIRE(j.dump().find("abcdefghijkmnpqrstuvwxyz23") == std::string::npos);
    const Client back = from_settings(j);
    REQUIRE(back.id == c.id);
    REQUIRE(back.hash == c.hash);
    REQUIRE(back.scopes == c.scopes);
    REQUIRE(back.tagged_ok);
    REQUIRE(back.created_at == c.created_at);
    // The hub page never sees the hash.
    REQUIRE_FALSE(to_public(c).contains("hash"));
    REQUIRE(to_public(c)["legacy"] == false);
}

TEST_CASE("a malformed client entry is dropped, never guessed at", "[RemoteHub][clients]")
{
    const std::string h = client_token_hash("abcdefghijkmnpqrstuvwxyz23");
    REQUIRE(from_settings(nlohmann::json{ { "id", "c1" }, { "hash", h }, { "scopes", { "status" } } }).id == "c1");
    REQUIRE(from_settings(nlohmann::json{ { "hash", h }, { "scopes", { "status" } } }).id.empty());
    REQUIRE(from_settings(nlohmann::json{ { "id", "c1" }, { "hash", "nothex" }, { "scopes", { "status" } } }).id.empty());
    REQUIRE(from_settings(nlohmann::json{ { "id", "c1" }, { "hash", h }, { "scopes", { "root" } } }).id.empty());
    REQUIRE(from_settings(nlohmann::json("a string")).id.empty());
}

TEST_CASE("client_token_hash is SHA-256 in hex", "[RemoteHub][clients]")
{
    // FIPS 180-2 test vector for "abc".
    REQUIRE(client_token_hash("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(client_token_hash("").empty());
}

TEST_CASE("find_by_hash finds the one client and nothing else", "[RemoteHub][clients]")
{
    std::vector<Client> cs(3);
    cs[0].hash = client_token_hash("tokenaaaaaaaaaaaaaaaaaaaaa");
    cs[1].hash = client_token_hash("tokenbbbbbbbbbbbbbbbbbbbbb");
    cs[2].hash = client_token_hash("tokencccccccccccccccccccccc");
    REQUIRE(find_by_hash(cs, client_token_hash("tokenbbbbbbbbbbbbbbbbbbbbb")) == 1);
    REQUIRE(find_by_hash(cs, client_token_hash("tokendddddddddddddddddddddd")) == -1);
    REQUIRE(find_by_hash(cs, "") == -1);
    REQUIRE(find_by_hash({}, cs[0].hash) == -1);
}

TEST_CASE("clean_name keeps a name printable and short", "[RemoteHub][clients]")
{
    REQUIRE(clean_name("  Workshop PC  ") == "Workshop PC");
    REQUIRE(clean_name("<script>x</script>") == "scriptx/script");
    REQUIRE(clean_name("   ").empty());
    REQUIRE(clean_name(std::string(100, 'a')).size() == 60);
}

TEST_CASE("a token bucket allows its burst, then refills at its rate", "[RemoteHub][clients]")
{
    Bucket b;
    long long t = 1000000;
    for (int i = 0; i < 5; ++i) REQUIRE(take(b, 5, 1, t));
    REQUIRE_FALSE(take(b, 5, 1, t));
    REQUIRE_FALSE(take(b, 5, 1, t + 500));
    REQUIRE(take(b, 5, 1, t + 1000));
    REQUIRE_FALSE(take(b, 5, 1, t + 1000));
    // A long quiet spell refills to the burst, never past it.
    for (int i = 0; i < 5; ++i) REQUIRE(take(b, 5, 1, t + 100000));
    REQUIRE_FALSE(take(b, 5, 1, t + 100000));
}

TEST_CASE("wrong tokens from one source block it for a while", "[RemoteHub][clients]")
{
    FailWindow f;
    const long long t = 5000000;
    for (int i = 0; i < 19; ++i) note_failure(f, t + i, 20, 600000, 600000);
    REQUIRE_FALSE(blocked(f, t + 20));
    note_failure(f, t + 20, 20, 600000, 600000);
    REQUIRE(blocked(f, t + 21));
    REQUIRE(blocked(f, t + 600019));
    REQUIRE_FALSE(blocked(f, t + 600021));
    // Failures spread out wider than the window never add up.
    FailWindow g;
    for (int i = 0; i < 40; ++i) note_failure(g, t + (long long) i * 700000, 20, 600000, 600000);
    REQUIRE_FALSE(blocked(g, t + 40LL * 700000));
}
