#include <catch2/catch.hpp>

#include "slic3r/GUI/LoginTicketGate.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using Slic3r::GUI::LoginTicketGate;
using Role = LoginTicketGate::Role;

// The loopback sign-in callback arrived twice in the same second on 2026-10-08; the second
// exchange of the single-use ticket got a 401 and the browser was sent to result=fail after a
// sign-in that had worked. The gate makes a ticket exchange happen once.

namespace {
LoginTicketGate::Outcome ok()
{
    LoginTicketGate::Outcome o;
    o.success = true;
    return o;
}
LoginTicketGate::Outcome bad()
{
    LoginTicketGate::Outcome o;
    o.success = false;
    o.error   = "ticket_exchange_failed";
    return o;
}
} // namespace

TEST_CASE("[LoginTicketGate] the first request owns the ticket, a repeat gets the same answer", "[LoginTicketGate]")
{
    LoginTicketGate g;
    const std::string t = "Ab12Cd";
    REQUIRE(g.claim(t, 1000).role == Role::Owner);
    g.finish(t, ok(), 1200);
    // The same second, a second later, a minute later: the owner's page, no second exchange.
    for (long long at : { 1300LL, 2300LL, 61000LL }) {
        const LoginTicketGate::Claim c = g.claim(t, at);
        REQUIRE(c.role == Role::Repeat);
        CHECK(c.outcome.success);
        CHECK(c.outcome.error.empty());
    }
}

TEST_CASE("[LoginTicketGate] a different ticket is its own exchange", "[LoginTicketGate]")
{
    LoginTicketGate g;
    REQUIRE(g.claim("Ab12Cd", 0).role == Role::Owner);
    g.finish("Ab12Cd", ok(), 10);
    REQUIRE(g.claim("Zz99Yy", 20).role == Role::Owner);
    g.finish("Zz99Yy", ok(), 30);
    CHECK(g.claim("Ab12Cd", 40).role == Role::Repeat);
    CHECK(g.claim("Zz99Yy", 40).role == Role::Repeat);
}

TEST_CASE("[LoginTicketGate] a failed exchange is repeated back briefly, then tried afresh", "[LoginTicketGate]")
{
    LoginTicketGate g;
    REQUIRE(g.claim("Ab12Cd", 0).role == Role::Owner);
    g.finish("Ab12Cd", bad(), 100);
    // Inside the failure window the repeat is told the same failure (no second 401).
    LoginTicketGate::Claim c = g.claim("Ab12Cd", 900);
    REQUIRE(c.role == Role::Repeat);
    CHECK_FALSE(c.outcome.success);
    CHECK(c.outcome.error == "ticket_exchange_failed");
    // A person who really tries again after the window gets a real exchange.
    CHECK(g.claim("Ab12Cd", 100 + LoginTicketGate::FAILURE_TTL_MS + 1).role == Role::Owner);
}

TEST_CASE("[LoginTicketGate] a success is remembered for the retry window and then forgotten", "[LoginTicketGate]")
{
    LoginTicketGate g;
    REQUIRE(g.claim("Ab12Cd", 0).role == Role::Owner);
    g.finish("Ab12Cd", ok(), 0);
    CHECK(g.claim("Ab12Cd", LoginTicketGate::SUCCESS_TTL_MS - 1).role == Role::Repeat);
    CHECK(g.size() == 1);
    // Past the window the entry is purged (the memory does not grow without bound).
    CHECK(g.claim("Other1", LoginTicketGate::SUCCESS_TTL_MS + 5).role == Role::Owner);
    CHECK(g.size() == 1);
}

TEST_CASE("[LoginTicketGate] a repeat that arrives mid-exchange waits for the owner's answer", "[LoginTicketGate]")
{
    LoginTicketGate g;
    REQUIRE(g.claim("Ab12Cd", 0).role == Role::Owner);

    std::atomic<bool>        got { false };
    LoginTicketGate::Claim   seen;
    std::thread              second([&] {
        seen = g.claim("Ab12Cd", 5);
        got  = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(got.load()); // still waiting: the exchange has not finished
    g.finish("Ab12Cd", ok(), 50);
    second.join();
    REQUIRE(got.load());
    CHECK(seen.role == Role::Repeat);
    CHECK(seen.outcome.success);
}

TEST_CASE("[LoginTicketGate] only a whole ticket parameter makes a request a callback", "[LoginTicketGate]")
{
    CHECK(LoginTicketGate::extract_ticket("/?ticket=Ab12Cd&redirect_url=https://bambulab.com/x") == "Ab12Cd");
    CHECK(LoginTicketGate::extract_ticket("/callback?redirect_url=https://bambulab.com/x&ticket=Ab12Cd") == "Ab12Cd");
    CHECK(LoginTicketGate::extract_ticket("/?ticket=Ab12Cd") == "Ab12Cd");
    // The things a browser asks a loopback port for that are not the callback.
    CHECK(LoginTicketGate::extract_ticket("/favicon.ico").empty());
    CHECK(LoginTicketGate::extract_ticket("/").empty());
    CHECK(LoginTicketGate::extract_ticket("/ticket").empty());
    CHECK(LoginTicketGate::extract_ticket("/ticket/favicon.ico?x=1").empty());
    CHECK(LoginTicketGate::extract_ticket("/?my_ticket=Ab12Cd").empty());
    CHECK(LoginTicketGate::extract_ticket("/?ticket=").empty());
}

TEST_CASE("[LoginTicketGate] a ticket is never logged in full", "[LoginTicketGate]")
{
    const std::string m = LoginTicketGate::mask("Ab12Cd");
    CHECK(m.find("Ab12Cd") == std::string::npos);
    CHECK(m.find("12Cd") == std::string::npos);
    CHECK(m == "Ab***(6)");
    CHECK(LoginTicketGate::mask("").find("<none>") == 0);
    CHECK(LoginTicketGate::mask("x") == "x***(1)");
}
