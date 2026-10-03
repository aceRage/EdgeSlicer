#include <catch2/catch.hpp>

#include "slic3r/Utils/HubSupervisor.hpp"

using namespace Slic3r::HubSupervisor;

// What a service-mode hub decides about its slicer instance, stepped through with made-up times.

TEST_CASE("hub supervisor: backoff doubles from the base and is capped", "[HubSupervisor]")
{
    Options o; // base 2, max 120
    CHECK(backoff_seconds(0, o) == 2);
    CHECK(backoff_seconds(1, o) == 2);
    CHECK(backoff_seconds(2, o) == 4);
    CHECK(backoff_seconds(3, o) == 8);
    CHECK(backoff_seconds(6, o) == 64);
    CHECK(backoff_seconds(7, o) == 120);
    CHECK(backoff_seconds(50, o) == 120);
}

TEST_CASE("hub supervisor: the first tick with no instance spawns one", "[HubSupervisor]")
{
    Supervisor s;
    const Decision d = s.tick({}, {}, 1000);
    CHECK(d.spawn);
    CHECK(d.lost.empty());
    CHECK(d.failures == 0);
}

TEST_CASE("hub supervisor: a spawn that is still starting is not spawned again", "[HubSupervisor]")
{
    Supervisor s;
    REQUIRE(s.tick({}, {}, 1000).spawn);
    s.note_spawn(4001, 1000);
    CHECK_FALSE(s.tick({}, { 4001 }, 1002).spawn);
    CHECK_FALSE(s.tick({}, { 4001 }, 1060).spawn);
    // ... and once it registers there is nothing to do either.
    CHECK_FALSE(s.tick({ 4001 }, {}, 1065).spawn);
}

TEST_CASE("hub supervisor: an instance that dies after running a while is replaced after the base wait", "[HubSupervisor]")
{
    Supervisor s;
    s.note_spawn(4001, 1000);
    REQUIRE_FALSE(s.tick({ 4001 }, {}, 1010).spawn);
    // Healthy for 500 s, then gone.
    const Decision gone = s.tick({}, {}, 1510);
    REQUIRE(gone.lost.size() == 1);
    CHECK(gone.lost[0].pid == 4001);
    CHECK(gone.lost[0].lived_s == 500);
    CHECK_FALSE(gone.lost[0].quick);
    CHECK(gone.failures == 0);
    CHECK_FALSE(gone.spawn);          // the base wait (2 s) first
    CHECK(gone.next_spawn_in_s == 2);
    CHECK_FALSE(s.tick({}, {}, 1511).spawn);
    CHECK(s.tick({}, {}, 1512).spawn);
}

TEST_CASE("hub supervisor: a crash loop backs off and a healthy run resets it", "[HubSupervisor]")
{
    Supervisor s;
    long long  t   = 0;
    long       pid = 5000;
    std::vector<long long> waits;
    for (int round = 0; round < 5; ++round) {
        // spawn when allowed
        Decision d = s.tick({}, {}, t);
        long long guard = 0;
        while (!d.spawn && guard++ < 1000) d = s.tick({}, {}, ++t);
        REQUIRE(d.spawn);
        s.note_spawn(++pid, t);
        // it registers 5 s later and dies 5 s after that: well under the stable time
        t += 5;
        s.tick({ pid }, {}, t);
        t += 5;
        const Decision dead = s.tick({}, {}, t);
        REQUIRE(dead.lost.size() == 1);
        CHECK(dead.lost[0].quick);
        waits.push_back(dead.next_spawn_in_s);
    }
    CHECK(waits == std::vector<long long>{ 2, 4, 8, 16, 32 });

    // Now one that stays up for two minutes: the slate is clean.
    Decision d = s.tick({}, {}, t);
    long long guard = 0;
    while (!d.spawn && guard++ < 1000) d = s.tick({}, {}, ++t);
    REQUIRE(d.spawn);
    s.note_spawn(++pid, t);
    t += 3;
    s.tick({ pid }, {}, t);
    t += 120;
    CHECK(s.tick({ pid }, {}, t).failures == 0);
    const Decision dead = s.tick({}, {}, t + 1);
    CHECK_FALSE(dead.lost[0].quick);
    CHECK(dead.next_spawn_in_s == 2);
}

TEST_CASE("hub supervisor: a spawn that dies before it registers counts as a failure", "[HubSupervisor]")
{
    Supervisor s;
    s.note_spawn(4001, 100);
    // not live, not pending: it is gone
    const Decision d = s.tick({}, {}, 103);
    REQUIRE(d.failed_spawns.size() == 1);
    CHECK(d.failed_spawns[0] == 4001);
    CHECK(d.failures == 1);
    CHECK_FALSE(d.spawn);
    CHECK(d.next_spawn_in_s == 2);
    CHECK(s.tick({}, {}, 105).spawn);
}

TEST_CASE("hub supervisor: a spawn that never registers is given up on after the timeout", "[HubSupervisor]")
{
    Supervisor s;
    s.note_spawn(4001, 100);
    CHECK_FALSE(s.tick({}, { 4001 }, 200).spawn); // still alive and starting
    const Decision d = s.tick({}, { 4001 }, 230); // 130 s: too long
    CHECK(d.failed_spawns.size() == 1);
    CHECK(d.failures == 1);
}

TEST_CASE("hub supervisor: a failed start itself backs off too", "[HubSupervisor]")
{
    Supervisor s;
    s.note_spawn(0, 10);
    s.note_spawn(0, 12);
    const Decision d = s.tick({}, {}, 13);
    CHECK(d.failures == 2);
    CHECK_FALSE(d.spawn);
    CHECK(d.next_spawn_in_s == 3); // 12 + 4 - 13
}

TEST_CASE("hub supervisor: only a shortage spawns; instances started by someone else count", "[HubSupervisor]")
{
    Supervisor s;
    CHECK_FALSE(s.tick({ 777 }, {}, 50).spawn); // a person opened one
    Options o;
    o.wanted = 2;
    Supervisor two(o);
    CHECK(two.tick({ 777 }, {}, 50).spawn);
    CHECK_FALSE(two.tick({ 777, 778 }, {}, 51).spawn);
}

TEST_CASE("hub supervisor: wanted = 0 never spawns", "[HubSupervisor]")
{
    Options o;
    o.wanted = 0;
    Supervisor s(o);
    CHECK_FALSE(s.tick({}, {}, 10).spawn);
    CHECK_FALSE(s.tick({}, {}, 1000).spawn);
}

TEST_CASE("hub supervisor: stale rows go once their reporter is gone and the TTL has passed", "[HubSupervisor]")
{
    const long long ttl = 180000;
    const long long now = 1000000;
    const std::vector<CachedRow> rows = {
        { "alive-fresh",  now - 1000,        4001 }, // reporter alive, fresh
        { "alive-old",    now - 10 * ttl,    4001 }, // reporter alive but slow: keeps its stale flag
        { "dead-fresh",   now - 1000,        3999 }, // reporter died a moment ago: the replacement may pick it up
        { "dead-old",     now - ttl - 1,     3999 }, // gone for good
        { "prev-run-old", now - 2 * ttl,     0    }, // loaded from the last run, nobody refreshed it
        { "prev-run-new", now - 5000,        0    },
    };
    const std::vector<std::string> drop = stale_rows_to_drop(rows, { 4001 }, now, ttl);
    CHECK(drop == std::vector<std::string>{ "dead-old", "prev-run-old" });
}

TEST_CASE("hub supervisor: with no instance alive every old row goes after the TTL", "[HubSupervisor]")
{
    const std::vector<CachedRow> rows = { { "a", 0, 11 }, { "b", 990000, 12 } };
    CHECK(stale_rows_to_drop(rows, {}, 1000000, 180000) == std::vector<std::string>{ "a" });
}
