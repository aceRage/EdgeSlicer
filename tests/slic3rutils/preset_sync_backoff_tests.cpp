#include <catch2/catch.hpp>

#include "slic3r/Utils/PresetSyncBackoff.hpp"

using Slic3r::PresetSync::Backoff;
using std::chrono::minutes;

// The cloud user-preset sync used to ask request_setting_id again for every preset it could not get
// an id for (empty id, HTTP 200) on every pass, forever. Backoff is the in-memory memory of those
// failures (GUI_App::sync_preset); these tests pin the decisions it makes.

namespace {
const int T_FILAMENT = 3; // any stable preset type value; the class only compares it
const int T_PRINT    = 1;
const Backoff::TimePoint t0 = Backoff::TimePoint{} + std::chrono::hours(1000);
} // namespace

TEST_CASE("Preset sync backoff: a preset with no failure may always be tried", "[PresetSync]")
{
    Backoff b;
    CHECK(b.may_attempt(T_FILAMENT, "A", "", true, t0));
    CHECK(b.may_attempt(T_FILAMENT, "A", "create", true, t0));
    CHECK(b.may_attempt(T_FILAMENT, "A", "update", false, t0));
}

TEST_CASE("Preset sync backoff: a failed preset is left alone, then retried after the delay", "[PresetSync]")
{
    Backoff b;
    b.record_failure(T_FILAMENT, "A", "", true, t0);
    CHECK_FALSE(b.may_attempt(T_FILAMENT, "A", "", true, t0));
    CHECK_FALSE(b.may_attempt(T_FILAMENT, "A", "", true, t0 + minutes(4)));
    CHECK(b.may_attempt(T_FILAMENT, "A", "", true, t0 + minutes(5)));

    // Other presets, and the same name in another preset collection, are unaffected.
    CHECK(b.may_attempt(T_FILAMENT, "B", "", true, t0));
    CHECK(b.may_attempt(T_PRINT, "A", "", true, t0));
}

TEST_CASE("Preset sync backoff: the delay grows 5, 15, 45 minutes, then stops at 2 hours", "[PresetSync]")
{
    CHECK(Backoff::delay_for(1) == minutes(5));
    CHECK(Backoff::delay_for(2) == minutes(15));
    CHECK(Backoff::delay_for(3) == minutes(45));
    CHECK(Backoff::delay_for(4) == minutes(120));
    CHECK(Backoff::delay_for(50) == minutes(120));
    CHECK(Backoff::delay_for(0) == minutes(5)); // never a zero delay

    Backoff b;
    auto now = t0;
    b.record_failure(T_FILAMENT, "A", "", false, now);
    now += minutes(5);
    REQUIRE(b.may_attempt(T_FILAMENT, "A", "", false, now));
    b.record_failure(T_FILAMENT, "A", "", false, now); // second failure: 15 min
    CHECK_FALSE(b.may_attempt(T_FILAMENT, "A", "", false, now + minutes(14)));
    CHECK(b.may_attempt(T_FILAMENT, "A", "", false, now + minutes(15)));
    CHECK(b.failures_of(T_FILAMENT, "A") == 2);
}

TEST_CASE("Preset sync backoff: saving the preset again (sync_info changes) retries it at once", "[PresetSync]")
{
    Backoff b;
    b.record_failure(T_FILAMENT, "A", "", true, t0);
    REQUIRE_FALSE(b.may_attempt(T_FILAMENT, "A", "", true, t0 + minutes(1)));

    // Tab::save_preset marks an edit of an existing preset "update" and a new one "create".
    CHECK(b.may_attempt(T_FILAMENT, "A", "update", false, t0 + minutes(1)));
    // The entry is gone: if it fails again it starts over at the first delay.
    CHECK(b.failures_of(T_FILAMENT, "A") == 0);

    // A "create" that keeps failing is held while its sync_info stays "create"...
    b.record_failure(T_FILAMENT, "N", "create", true, t0);
    CHECK_FALSE(b.may_attempt(T_FILAMENT, "N", "create", true, t0 + minutes(1)));
    // ...until the user saves it again.
    CHECK(b.may_attempt(T_FILAMENT, "N", "update", false, t0 + minutes(1)));
}

TEST_CASE("Preset sync backoff: a success forgets the preset and reopens the session", "[PresetSync]")
{
    Backoff b;
    for (int i = 0; i < Backoff::kBreakerThreshold; ++i)
        b.record_failure(T_FILAMENT, "P" + std::to_string(i), "", true, t0);
    REQUIRE(b.breaker_closed(t0));
    REQUIRE(b.consecutive_failures() == Backoff::kBreakerThreshold);

    b.record_success(T_FILAMENT, "P0");
    CHECK_FALSE(b.breaker_closed(t0));
    CHECK(b.consecutive_failures() == 0);
    CHECK(b.may_attempt(T_FILAMENT, "P0", "", true, t0));
    // The other presets still wait out their own delay.
    CHECK_FALSE(b.may_attempt(T_FILAMENT, "P1", "", true, t0 + minutes(1)));
}

TEST_CASE("Preset sync backoff: a run of failed requests pauses new-id requests for everything", "[PresetSync]")
{
    Backoff b;
    // The UltraNet stub case: every preset's request comes back with no id.
    for (int i = 0; i < Backoff::kBreakerThreshold - 1; ++i) {
        REQUIRE(b.may_attempt(T_FILAMENT, "P" + std::to_string(i), "", true, t0));
        CHECK_FALSE(b.record_failure(T_FILAMENT, "P" + std::to_string(i), "", true, t0));
    }
    CHECK_FALSE(b.breaker_closed(t0));
    CHECK(b.may_attempt(T_FILAMENT, "Next", "", true, t0));

    // The failure that reaches the threshold closes the breaker, and says so once.
    CHECK(b.record_failure(T_FILAMENT, "Last", "", true, t0));
    CHECK(b.breaker_closed(t0));

    // A preset never tried before is not asked about during the pause...
    CHECK_FALSE(b.may_attempt(T_FILAMENT, "Never tried", "", true, t0 + minutes(4)));
    CHECK_FALSE(b.may_attempt(T_PRINT, "Never tried", "create", true, t0 + minutes(4)));
    // ...but uploading an edit of an already-synced preset is not a new-id request and still goes.
    CHECK(b.may_attempt(T_FILAMENT, "Synced", "update", false, t0 + minutes(4)));

    // After the pause one probe is let through.
    CHECK(b.may_attempt(T_FILAMENT, "Never tried", "", true, t0 + minutes(5)));
}

TEST_CASE("Preset sync backoff: a failed probe pauses the session for longer", "[PresetSync]")
{
    Backoff b;
    for (int i = 0; i < Backoff::kBreakerThreshold; ++i)
        b.record_failure(T_FILAMENT, "P" + std::to_string(i), "", true, t0);

    const auto probe = t0 + minutes(5);
    REQUIRE(b.may_attempt(T_FILAMENT, "Probe", "", true, probe));
    CHECK_FALSE(b.record_failure(T_FILAMENT, "Probe", "", true, probe)); // already tripped once: no repeat message
    CHECK_FALSE(b.may_attempt(T_FILAMENT, "Other", "", true, probe + minutes(14)));
    CHECK(b.may_attempt(T_FILAMENT, "Other", "", true, probe + minutes(15)));
}

TEST_CASE("Preset sync backoff: failed uploads of synced presets are held back but never trip the breaker", "[PresetSync]")
{
    Backoff b;
    for (int i = 0; i < 3 * Backoff::kBreakerThreshold; ++i)
        b.record_failure(T_FILAMENT, "S" + std::to_string(i), "update", false, t0);
    CHECK_FALSE(b.breaker_closed(t0));
    CHECK(b.consecutive_failures() == 0);
    CHECK(b.may_attempt(T_FILAMENT, "Fresh", "", true, t0));
    CHECK_FALSE(b.may_attempt(T_FILAMENT, "S0", "update", false, t0 + minutes(1)));
}

TEST_CASE("Preset sync backoff: a pass over many presets makes only a handful of requests", "[PresetSync]")
{
    // The reported case: ~590 presets, every request answered with an empty id. Simulate sync passes
    // two seconds apart (plus 100 ms per preset) for ten minutes and count the requests that go out.
    Backoff b;
    const int presets = 590;
    int       requests = 0;
    auto      now = t0;
    const auto end = t0 + minutes(10);
    while (now < end) {
        for (int i = 0; i < presets; ++i) {
            const std::string name = "P" + std::to_string(i);
            if (!b.may_attempt(T_FILAMENT, name, "", true, now))
                continue;
            ++requests;
            b.record_failure(T_FILAMENT, name, "", true, now);
            now += std::chrono::milliseconds(100);
        }
        now += std::chrono::seconds(2);
    }
    // Without backoff this would be ~10 passes x 590 = thousands of requests.
    CHECK(requests <= 3 * Backoff::kBreakerThreshold);
}
