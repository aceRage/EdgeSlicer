// The "printer refused the access code" detector (LanAccessCodeWatch.hpp).
//
// A Bambu LAN printer whose code was changed on its screen refuses every dial; one that is off
// answers none. These cases drive the rule with plug-in reports and steady-clock milliseconds
// only: no printer, no network, no wx.

#include <catch2/catch.hpp>

#include "slic3r/GUI/LanAccessCodeWatch.hpp"

#include <string>

using namespace Slic3r::LanAccessCode;

namespace {

const size_t OLD_CODE = code_fingerprint("12345678");
const size_t NEW_CODE = code_fingerprint("87654321");

// One dial with `code` at `t` that the printer refuses `after_ms` later (UltraNet's message).
Verdict refused_dial(Watch& w, long long t, size_t code = OLD_CODE, long long after_ms = 900)
{
    w.dial_started(t, code);
    return w.dial_ended(t + after_ms, "connect failed rc=0");
}

// One dial that runs into the plug-in's 8 s connect timeout: nobody answered.
Verdict unreachable_dial(Watch& w, long long t, size_t code = OLD_CODE)
{
    w.dial_started(t, code);
    return w.dial_ended(t + 8000, "connect failed rc=0");
}

} // namespace

TEST_CASE("Three refused dials with the same code raise the notice once", "[LanAccessCode]")
{
    Watch w;
    CHECK(refused_dial(w, 0) == Verdict::None);
    CHECK(w.last_outcome() == Outcome::Refused);
    CHECK(w.last_dial_refused());
    CHECK(refused_dial(w, 15000) == Verdict::None);
    CHECK(refused_dial(w, 105000) == Verdict::Notify);
    CHECK(w.notice_shown());
    // The ladder keeps dialling every 60 s: no second notice for the same code.
    for (long long t = 105000; t < 105000 + 10 * 60000; t += 60000)
        CHECK(refused_dial(w, t) == Verdict::None);
    CHECK(w.refusals() == 13);
}

TEST_CASE("A session that stays up clears the notice and resets the run", "[LanAccessCode]")
{
    Watch w;
    refused_dial(w, 0);
    refused_dial(w, 45000);
    REQUIRE(refused_dial(w, 105000) == Verdict::Notify);

    // The user re-enters the code: a new code starts afresh, and the dial comes up.
    w.dial_started(100000, NEW_CODE);
    CHECK_FALSE(w.notice_shown());
    w.session_up(100700);
    CHECK(w.tick(101000) == Verdict::None);              // up, but not proven yet
    CHECK(w.tick(100700 + SHORT_SESSION_MS) == Verdict::None); // proven; the notice was already reset
    CHECK(w.refusals() == 0);
    CHECK_FALSE(w.last_dial_refused());
}

TEST_CASE("The same code working again closes the notice", "[LanAccessCode]")
{
    // The user changes the printer's code back instead of the slicer's.
    Watch w;
    refused_dial(w, 0);
    refused_dial(w, 45000);
    REQUIRE(refused_dial(w, 105000) == Verdict::Notify);
    w.dial_started(105000, OLD_CODE);
    w.session_up(105800);
    CHECK(w.tick(105800 + SHORT_SESSION_MS) == Verdict::Clear);
    CHECK_FALSE(w.notice_shown());
    CHECK(w.refusals() == 0);
    // Refused again later with the same code: a fresh run may notify again.
    refused_dial(w, 200000);
    refused_dial(w, 245000);
    CHECK(refused_dial(w, 305000) == Verdict::Notify);
}

TEST_CASE("A printer that is off never raises the notice", "[LanAccessCode]")
{
    Watch w;
    for (long long t = 0; t < 30 * 60000; t += 60000)
        CHECK(unreachable_dial(w, t) == Verdict::None);
    CHECK(w.last_outcome() == Outcome::Unreachable);
    CHECK(w.refusals() == 0);
    CHECK_FALSE(w.notice_shown());

    // A quick socket error reads as unreachable too, however fast it came back.
    w.dial_started(0, OLD_CODE);
    CHECK(w.dial_ended(300, "connect failed: Connection timed out") == Verdict::None);
    CHECK(w.last_outcome() == Outcome::Unreachable);
    w.dial_started(1000, OLD_CODE);
    CHECK(w.dial_ended(1200, "No route to host") == Verdict::None);
    CHECK(w.last_outcome() == Outcome::Unreachable);
    CHECK(w.refusals() == 0);
}

TEST_CASE("An unreachable dial breaks a run of refusals", "[LanAccessCode]")
{
    Watch w;
    refused_dial(w, 0);
    refused_dial(w, 45000);
    CHECK(unreachable_dial(w, 45000) == Verdict::None);
    CHECK(refused_dial(w, 105000) == Verdict::None);
    CHECK(refused_dial(w, 165000) == Verdict::None);
    CHECK(refused_dial(w, 225000) == Verdict::Notify);
}

TEST_CASE("A session closed seconds after it came up counts as refused", "[LanAccessCode]")
{
    // The broker accepts the session, then closes it: the refused-shortly-after-connect pattern.
    Watch w;
    for (int i = 0; i < 2; ++i) {
        const long long t = i * 60000;
        w.dial_started(t, OLD_CODE);
        w.session_up(t + 800);
        CHECK(w.tick(t + 2000) == Verdict::None);
        CHECK(w.dial_ended(t + 3500, "connection lost") == Verdict::None);
        CHECK(w.last_outcome() == Outcome::Refused);
    }
    w.dial_started(120000, OLD_CODE);
    w.session_up(120800);
    CHECK(w.dial_ended(124000, "connection lost") == Verdict::Notify);
}

TEST_CASE("A long session that drops is a normal drop, not a refusal", "[LanAccessCode]")
{
    Watch w;
    w.dial_started(0, OLD_CODE);
    w.session_up(700);
    // No tick in between (the caller may miss one): the drop itself proves the code.
    CHECK(w.dial_ended(700 + SHORT_SESSION_MS + 60000, "connection lost") == Verdict::None);
    CHECK(w.last_outcome() == Outcome::Dropped);
    CHECK(w.refusals() == 0);
    // After a tick proved it, the later drop report is a duplicate.
    w.dial_started(200000, OLD_CODE);
    w.session_up(200700);
    w.tick(200700 + SHORT_SESSION_MS);
    CHECK(w.dial_ended(300000, "connection lost") == Verdict::None);
    CHECK(w.last_outcome() == Outcome::Ignored);
}

TEST_CASE("Duplicate failure reports count once", "[LanAccessCode]")
{
    Watch w;
    w.dial_started(0, OLD_CODE);
    w.dial_ended(900, "connect failed rc=0");
    CHECK(w.dial_ended(950, "connect failed rc=0") == Verdict::None);
    CHECK(w.last_outcome() == Outcome::Ignored);
    CHECK(w.refusals() == 1);
}

TEST_CASE("An explicit MQTT not-authorized refusal counts even when slow", "[LanAccessCode]")
{
    CHECK(is_auth_refusal("5"));
    CHECK(is_auth_refusal("connect failed rc=5"));
    CHECK(is_auth_refusal("connect failed rc=134"));
    CHECK(is_auth_refusal("connect failed rc=135"));
    CHECK(is_auth_refusal("Connection refused: Not authorized"));
    CHECK_FALSE(is_auth_refusal("connect failed rc=0"));
    CHECK_FALSE(is_auth_refusal("connect failed rc=50"));
    CHECK_FALSE(is_auth_refusal("connect failed rc=-1"));
    CHECK_FALSE(is_auth_refusal(""));

    Watch w;
    for (int i = 0; i < 2; ++i) {
        w.dial_started(i * 60000, OLD_CODE);
        CHECK(w.dial_ended(i * 60000 + 7000, "5") == Verdict::None);
        CHECK(w.last_outcome() == Outcome::Refused);
    }
    w.dial_started(120000, OLD_CODE);
    CHECK(w.dial_ended(127000, "5") == Verdict::Notify);
}

TEST_CASE("Forgetting the code (a new one was saved) resets everything", "[LanAccessCode]")
{
    Watch w;
    refused_dial(w, 0);
    refused_dial(w, 45000);
    REQUIRE(refused_dial(w, 105000) == Verdict::Notify);
    w.forget();
    CHECK(w.refusals() == 0);
    CHECK_FALSE(w.notice_shown());
    // Saved the same code again (a typo fixed back): it may notify again after a new run.
    refused_dial(w, 100000);
    refused_dial(w, 145000);
    CHECK(refused_dial(w, 205000) == Verdict::Notify);
}

TEST_CASE("A printer still booting does not raise the notice", "[LanAccessCode]")
{
    // Its broker turns the first dial, the 1.5 s re-dial and the first ladder step away, then
    // lets the next one in: three quick refusals inside MIN_RUN_MS are not enough.
    Watch w;
    CHECK(refused_dial(w, 0) == Verdict::None);
    CHECK(refused_dial(w, 1500) == Verdict::None);
    CHECK(refused_dial(w, 16500) == Verdict::None);
    CHECK(refused_dial(w, 46500) == Verdict::None);
    CHECK(w.refusals() == 4);
    CHECK_FALSE(w.notice_shown());
    w.dial_started(106500, OLD_CODE);
    w.session_up(107200);
    CHECK(w.tick(107200 + SHORT_SESSION_MS) == Verdict::None);
    CHECK(w.refusals() == 0);

    // The same printer refusing for good does raise it, once the run is long enough.
    CHECK(refused_dial(w, 200000) == Verdict::None);
    CHECK(refused_dial(w, 201500) == Verdict::None);
    CHECK(refused_dial(w, 216500) == Verdict::None);
    CHECK(refused_dial(w, 246500) == Verdict::None);
    CHECK(refused_dial(w, 306500) == Verdict::Notify);
}

TEST_CASE("A refused code stops the redials until the user acts", "[LanAccessCode]")
{
    Watch w;
    refused_dial(w, 0);
    refused_dial(w, 15000);
    CHECK_FALSE(redial_paused(w, false)); // not yet told: keep trying
    REQUIRE(refused_dial(w, 105000) == Verdict::Notify);
    CHECK(redial_paused(w, false));       // the visible slicer waits for the user
    CHECK_FALSE(redial_paused(w, true));  // the hidden hub instance keeps dialling

    // Picking the printer again dials it with the same code: still refused, still paused.
    refused_dial(w, 400000);
    CHECK(redial_paused(w, false));

    // The printer's code was set back: that dial works, the notice closes, redials resume.
    w.dial_started(500000, OLD_CODE);
    w.session_up(500800);
    CHECK(w.tick(500800 + SHORT_SESSION_MS) == Verdict::Clear);
    CHECK_FALSE(redial_paused(w, false));
}

TEST_CASE("A new code resumes the redials", "[LanAccessCode]")
{
    Watch w;
    refused_dial(w, 0);
    refused_dial(w, 45000);
    REQUIRE(refused_dial(w, 105000) == Verdict::Notify);
    REQUIRE(redial_paused(w, false));
    // A dial with another code (saved in the edit dialog, or by the visible slicer for the hub).
    w.dial_started(200000, NEW_CODE);
    CHECK_FALSE(redial_paused(w, false));
}

TEST_CASE("A code the user just entered is judged after the quick re-dial", "[LanAccessCode]")
{
    // The printer was up when they read the code off its screen: no boot allowance.
    Watch w;
    w.code_from_user();
    CHECK(refused_dial(w, 0, NEW_CODE) == Verdict::None);
    CHECK_FALSE(w.bar_says_refused());
    CHECK(refused_dial(w, 1500, NEW_CODE) == Verdict::Notify);
    CHECK(w.bar_says_refused());
    CHECK(redial_paused(w, false));
    // Once per code.
    CHECK(refused_dial(w, 60000, NEW_CODE) == Verdict::None);
}

TEST_CASE("The user's code loses its fast rule once it worked or another code came", "[LanAccessCode]")
{
    Watch w;
    w.code_from_user();
    w.dial_started(0, NEW_CODE);
    w.session_up(800);
    CHECK(w.tick(800 + SHORT_SESSION_MS) == Verdict::None);
    // Later the code is changed on the printer: the usual rule (3 refusals over MIN_RUN_MS).
    CHECK(refused_dial(w, 100000, NEW_CODE) == Verdict::None);
    CHECK(refused_dial(w, 101500, NEW_CODE) == Verdict::None);
    CHECK(refused_dial(w, 116500, NEW_CODE) == Verdict::None);

    Watch other;
    other.code_from_user();
    refused_dial(other, 0, NEW_CODE);
    // A different code dialled without the dialog (the hub's copy, say): not the user's.
    CHECK(refused_dial(other, 1500, OLD_CODE) == Verdict::None);
    CHECK(refused_dial(other, 3000, OLD_CODE) == Verdict::None);
    CHECK_FALSE(other.notice_shown());
}

TEST_CASE("The Device page says refused from the second refusal in a row", "[LanAccessCode]")
{
    Watch w;
    refused_dial(w, 0);
    CHECK_FALSE(w.bar_says_refused());
    refused_dial(w, 1500);
    CHECK(w.bar_says_refused());
    CHECK_FALSE(w.notice_shown()); // the notice still waits for the long run
    CHECK_FALSE(redial_paused(w, false));
    // A slow failure (unreachable) breaks the run.
    unreachable_dial(w, 16500);
    CHECK_FALSE(w.bar_says_refused());
}
