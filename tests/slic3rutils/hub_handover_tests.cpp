#include <catch2/catch.hpp>

#include "slic3r/Utils/HubHandover.hpp"

using namespace Slic3r::HubHandover;

// The hub lifecycle rules (HubHandover.hpp): a slicer that finds a hub running from another install
// asks it to hand over, and a hub whose install is gone exits. Paths and states are faked.

static const char* const INSTALLED = "C:\\Dev\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe";
static const char* const TEST_COPY = "C:\\Dev\\EdgeSlicerTest\\hubhandover\\EdgeSlicer.exe";

TEST_CASE("hub handover: paths compare after case and slash normalisation", "[HubHandover]")
{
    SECTION("slashes and case, on Windows")
    {
        CHECK(same_exe("C:\\Dev\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe", "c:/dev/edgeslicerbuilds/current/EdgeSlicer.EXE", true));
        CHECK(same_exe("C:/Dev//EdgeSlicerBuilds/./current/EdgeSlicer.exe", "C:\\Dev\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe", true));
        CHECK(same_exe("C:\\Dev\\x\\..\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe", INSTALLED, true));
    }
    SECTION("the extended-length prefix is not part of the path")
    {
        CHECK(same_exe("\\\\?\\C:\\Dev\\EdgeSlicerBuilds\\current\\EdgeSlicer.exe", INSTALLED, true));
        CHECK(same_exe("\\\\?\\UNC\\server\\share\\app\\EdgeSlicer.exe", "\\\\server\\share\\app\\EdgeSlicer.exe", true));
        CHECK(!same_exe("\\\\?\\UNC\\server\\share\\app\\EdgeSlicer.exe", "\\server\\share\\app\\EdgeSlicer.exe", true));
    }
    SECTION("case counts where the file system is case sensitive")
    {
        CHECK(!same_exe("/opt/EdgeSlicer/edgeslicer", "/opt/edgeslicer/edgeslicer", false));
        CHECK(same_exe("/opt/EdgeSlicer//bin/../edgeslicer/", "/opt/EdgeSlicer/edgeslicer", false));
    }
    SECTION("different files stay different")
    {
        CHECK(!same_exe(INSTALLED, TEST_COPY, true));
        CHECK(!same_exe("C:\\Dev\\a\\EdgeSlicer.exe", "C:\\Dev\\a\\EdgeSlicer2.exe", true));
        CHECK(!same_exe("D:\\Dev\\a\\EdgeSlicer.exe", "C:\\Dev\\a\\EdgeSlicer.exe", true));
        // ".." cannot climb above the root, so the two spellings below are the same place.
        CHECK(same_exe("C:\\..\\Dev\\a.exe", "C:\\Dev\\a.exe", true));
    }
    SECTION("an empty path is never the same as anything, itself included")
    {
        CHECK(!same_exe("", "", true));
        CHECK(!same_exe(INSTALLED, "", true));
        CHECK(normalize_path("", true).empty());
    }
}

TEST_CASE("hub handover: foreign vs same exe", "[HubHandover]")
{
    SECTION("the hub reports its exe")
    {
        CHECK(judge_exe(INSTALLED, INSTALLED, "", true) == ExeVerdict::Same);
        CHECK(judge_exe(INSTALLED, "c:/dev/edgeslicerbuilds/CURRENT/edgeslicer.exe", "", true) == ExeVerdict::Same);
        CHECK(judge_exe(INSTALLED, TEST_COPY, "", true) == ExeVerdict::Foreign);
        // What the hub says about itself wins; the pid's image is only the fallback.
        CHECK(judge_exe(INSTALLED, INSTALLED, TEST_COPY, true) == ExeVerdict::Same);
    }
    SECTION("an old hub without the field is judged by its pid's image path")
    {
        CHECK(judge_exe(INSTALLED, "", INSTALLED, true) == ExeVerdict::Same);
        CHECK(judge_exe(INSTALLED, "", TEST_COPY, true) == ExeVerdict::Foreign);
    }
    SECTION("nothing to compare is never foreign")
    {
        CHECK(judge_exe(INSTALLED, "", "", true) == ExeVerdict::Unknown);
        CHECK(judge_exe("", TEST_COPY, TEST_COPY, true) == ExeVerdict::Unknown);
    }
}

TEST_CASE("hub handover: the decision table", "[HubHandover]")
{
    Facts f;

    SECTION("no hub: start our own")
    {
        f.hub_alive = false;
        f.exe       = ExeVerdict::Foreign; // stale facts do not matter without a hub
        CHECK(plan(f) == Step::SpawnOwn);
    }
    SECTION("same exe: use it (two windows of one install do not fight)")
    {
        f.hub_alive = true;
        f.exe       = ExeVerdict::Same;
        CHECK(plan(f) == Step::UseRunning);
    }
    SECTION("unknown exe: use it")
    {
        f.hub_alive = true;
        f.exe       = ExeVerdict::Unknown;
        CHECK(plan(f) == Step::UseRunning);
    }
    SECTION("foreign exe: ask it to hand over, and start ours once it is gone")
    {
        f.hub_alive = true;
        f.exe       = ExeVerdict::Foreign;
        REQUIRE(plan(f) == Step::ReplaceRunning);
        CHECK(after_quit(true) == Outcome::SpawnOwn);
    }
    SECTION("foreign exe and the hub stays: leave it running, no second hub, no kill")
    {
        f.hub_alive = true;
        f.exe       = ExeVerdict::Foreign;
        REQUIRE(plan(f) == Step::ReplaceRunning);
        CHECK(after_quit(false) == Outcome::LeaveRunning);
    }
    SECTION("a process the hub started itself never asks for a handover")
    {
        f.hub_alive        = true;
        f.exe              = ExeVerdict::Foreign;
        f.handover_allowed = false; // hub-spawned instance, or this process already had its one reclaim
        CHECK(plan(f) == Step::UseRunning);
    }
    SECTION("another version still replaces the hub, as it always has")
    {
        f.hub_alive       = true;
        f.version_differs = true;
        f.exe             = ExeVerdict::Same;
        CHECK(plan(f) == Step::ReplaceRunning);
        f.handover_allowed = false;
        CHECK(plan(f) == Step::ReplaceRunning);
    }
}

TEST_CASE("hub handover: the hub's check of its own install", "[HubHandover]")
{
    SECTION("everything there: nothing happens, the count resets")
    {
        SelfCheck r = self_check(0, true, true);
        CHECK(r.misses == 0);
        CHECK(!r.quit);
        r = self_check(1, true, true);
        CHECK(r.misses == 0);
        CHECK(!r.quit);
    }
    SECTION("one miss is a warning, not an exit")
    {
        SelfCheck r = self_check(0, true, false);
        CHECK(r.misses == 1);
        CHECK(!r.quit);
    }
    SECTION("consecutive misses end it, whichever half is gone")
    {
        CHECK(self_check(1, true, false).quit);  // web pages gone, exe still there (a locked exe survives a delete)
        CHECK(self_check(1, false, true).quit);  // exe gone
        CHECK(self_check(1, false, false).quit);
        CHECK(self_check(5, true, false).quit);
    }
    SECTION("a recovery in between starts the count again")
    {
        SelfCheck r = self_check(0, true, false);
        REQUIRE(!r.quit);
        r = self_check(r.misses, true, true);
        REQUIRE(r.misses == 0);
        r = self_check(r.misses, true, false);
        CHECK(!r.quit);
        CHECK(r.misses == 1);
    }
    SECTION("the intervals mean 'about a minute' for a deleted folder")
    {
        CHECK(SELF_CHECK_INTERVAL_S * SELF_CHECK_MISSES <= 60);
        CHECK(SELF_CHECK_MISSES >= 2);
    }
}

TEST_CASE("hub handover: only well-formed UTF-8 goes into hub.json", "[HubHandover]")
{
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("C:\\Dev\\EdgeSlicer.exe"));
    CHECK(is_valid_utf8("C:\\Users\\J\xC3\xBCrgen\\EdgeSlicer.exe")); // u-umlaut
    CHECK(is_valid_utf8("\xE2\x82\xAC"));                              // euro sign
    CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));                          // 4-byte
    CHECK(!is_valid_utf8("C:\\Users\\J\xFCrgen\\EdgeSlicer.exe"));     // ANSI u-umlaut
    CHECK(!is_valid_utf8("\xC3"));                                     // truncated
    CHECK(!is_valid_utf8("\xC0\x80"));                                 // overlong
    CHECK(!is_valid_utf8("\xED\xA0\x80"));                             // surrogate
}

TEST_CASE("hub handover: waiting for the old hub to go and its port to free", "[HubHandover]")
{
    QuitFacts q;
    SECTION("still running inside the wait: look again")
    {
        q.old_gone  = false;
        q.waited_ms = QUIT_WAIT_MS - 1;
        CHECK(after_quit_step(q) == QuitStep::Wait);
    }
    SECTION("still running when the wait is over: leave it, as after_quit() always did")
    {
        q.old_gone  = false;
        q.waited_ms = QUIT_WAIT_MS;
        CHECK(after_quit_step(q) == QuitStep::LeaveRunning);
        CHECK(after_quit(false) == Outcome::LeaveRunning);
    }
    SECTION("gone and its port is free: start ours")
    {
        q.old_gone  = true;
        q.port_free = true;
        CHECK(after_quit_step(q) == QuitStep::SpawnOwn);
    }
    SECTION("gone but the port is still held: wait for it, not for ever")
    {
        q.old_gone      = true;
        q.port_free     = false;
        q.waited_ms     = 2000;
        q.since_gone_ms = PORT_FREE_WAIT_MS - 1;
        CHECK(after_quit_step(q) == QuitStep::Wait);
        q.since_gone_ms = PORT_FREE_WAIT_MS;
        CHECK(after_quit_step(q) == QuitStep::SpawnOwnPortHeld);
    }
    SECTION("the port wait counts from the exit, not from the request")
    {
        q.old_gone      = true;
        q.port_free     = false;
        q.waited_ms     = QUIT_WAIT_MS + PORT_FREE_WAIT_MS; // it took the whole quit wait to exit
        q.since_gone_ms = 100;
        CHECK(after_quit_step(q) == QuitStep::Wait);
    }
}

TEST_CASE("hub handover: no second hub while one is starting", "[HubHandover]")
{
    Starting s;
    SECTION("nobody starting: spawn")
    {
        CHECK(start_plan(s) == StartPlan::Spawn);
    }
    SECTION("our earlier spawn is still starting: wait for it (the 2026-10-08 orphan)")
    {
        s.own_pending_pid   = 73744;
        s.own_pending_alive = true;
        CHECK(start_plan(s) == StartPlan::WaitOwn);
        s.lock_held = true; // it took the lock meanwhile: still ours to wait for
        CHECK(start_plan(s) == StartPlan::WaitOwn);
    }
    SECTION("our earlier spawn died: free to spawn again")
    {
        s.own_pending_pid   = 73744;
        s.own_pending_alive = false;
        CHECK(start_plan(s) == StartPlan::Spawn);
    }
    SECTION("another process's hub holds the lock: wait for that one")
    {
        s.lock_held = true;
        CHECK(start_plan(s) == StartPlan::WaitOther);
    }
}

TEST_CASE("hub handover: one look while a hub starts", "[HubHandover]")
{
    WaitFacts w;
    w.own           = true;
    w.process_alive = true;
    SECTION("it answers: up, whatever else is true")
    {
        w.hub_answers = true;
        w.elapsed_ms  = START_DEADLINE_MS * 2;
        CHECK(wait_step(w) == WaitStep::Up);
    }
    SECTION("slow but alive: keep waiting - 14 s is not a failure any more")
    {
        w.elapsed_ms = 14000;
        CHECK(wait_step(w) == WaitStep::Wait);
        w.elapsed_ms = 105000; // the slowest start in the 2026-10-08 log
        CHECK(wait_step(w) == WaitStep::Wait);
    }
    SECTION("ours past the deadline: terminate it rather than leave an orphan")
    {
        w.elapsed_ms = START_DEADLINE_MS;
        CHECK(wait_step(w) == WaitStep::Terminate);
    }
    SECTION("someone else's past the deadline: give up, never kill it")
    {
        w.own        = false;
        w.lock_held  = true;
        w.elapsed_ms = START_DEADLINE_MS;
        CHECK(wait_step(w) == WaitStep::GiveUp);
    }
    SECTION("ours exited and nobody holds the lock: it failed")
    {
        w.process_alive = false;
        CHECK(wait_step(w) == WaitStep::Exited);
    }
    SECTION("ours exited because another hub has the lock: wait for that one")
    {
        w.process_alive = false;
        w.lock_held     = true;
        CHECK(wait_step(w) == WaitStep::WaitOther);
    }
    SECTION("someone else's let go of the lock without answering: it failed")
    {
        w.own           = false;
        w.process_alive = false; // for another's hub, alive means the lock is held
        w.lock_held     = false;
        CHECK(wait_step(w) == WaitStep::Exited);
    }
}

TEST_CASE("hub handover: replaying the 2026-10-08 handover", "[HubHandover]")
{
    // Caller 1 spawns A; A takes 30 s to answer. Caller 2 (the Stream tab), parked on the same
    // mutex, comes in after caller 1 used to give up at 12 s. Under the old rules it found no hub
    // and spawned B. Now caller 1 is still waiting at 12 s, and caller 2 waits for A.
    const int a_answers_at = 30000;
    WaitFacts w;
    w.own           = true;
    w.process_alive = true;
    int t = 0;
    for (; t < a_answers_at; t += 200) {
        w.elapsed_ms = t;
        REQUIRE(wait_step(w) == WaitStep::Wait);
    }
    w.hub_answers = true;
    w.elapsed_ms  = t;
    CHECK(wait_step(w) == WaitStep::Up);

    // Had caller 1 given up anyway (a deadline, an exception), caller 2 still does not double it.
    Starting s;
    s.own_pending_pid   = 73744;
    s.own_pending_alive = true;
    CHECK(start_plan(s) != StartPlan::Spawn);
}

TEST_CASE("hub handover: a quitting hub leaves another hub's record alone", "[HubHandover]")
{
    CHECK(remove_record_on_quit(1234, 1234));
    CHECK(!remove_record_on_quit(5678, 1234));
    CHECK(remove_record_on_quit(0, 1234)); // unreadable record: nothing to protect
}

TEST_CASE("hub handover: the timeouts hang together", "[HubHandover]")
{
    CHECK(START_DEADLINE_MS > 105000);           // the slowest real start seen
    CHECK(LOCK_RETRY_MS < START_DEADLINE_MS);
    CHECK(LOCK_RETRY_MS >= 500);                 // a probe holds the lock for microseconds; a few retries ride it out
    CHECK(QUIT_WAIT_MS + PORT_FREE_WAIT_MS <= 30000);
}
