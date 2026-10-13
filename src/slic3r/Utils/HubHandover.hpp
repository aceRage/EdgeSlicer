#ifndef slic3r_HubHandover_hpp_
#define slic3r_HubHandover_hpp_

#include <string>
#include <vector>

// Decisions for two hub lifecycle rules, kept free of wx, processes, files and sockets so they can
// be tested with paths and states faked (tests/slic3rutils/hub_handover_tests.cpp). RemoteHub.cpp
// gathers the facts and does the acting; the choosing lives here.
//
// 1. Handover. Every copy of EdgeSlicer on a PC shares one data dir, so whichever copy started the
//    hub first owns it - including a scratch test copy whose folder is later deleted. A slicer that
//    starts and finds a live hub running from a *different* executable asks it to quit (POST
//    /hub/quit, the graceful route; nothing is ever killed), waits for its pid to go, and starts its
//    own. If the old hub will not go it is left running and used as it is.
//
// 2. Self-exit. The hub looks every so often at whether its own executable and its web pages still
//    exist. If they are gone it quits cleanly instead of serving blank pages for days.
namespace Slic3r {
namespace HubHandover {

// ---- paths ----

// A path in a form two spellings of the same file compare equal in: a Windows extended-length
// prefix (\\?\ and \\?\UNC\) dropped, backslashes turned into slashes, repeated slashes folded
// (a leading // for a network share is kept), "." and ".." segments resolved lexically, no trailing
// slash, and lower case when `case_insensitive` (Windows; ASCII letters only - the file system's
// own folding of other letters is not reproduced, which at worst reads two spellings as different
// and the hub is then asked to quit, never the reverse). "" stays "".
inline std::string normalize_path(const std::string& path, bool case_insensitive)
{
    if (path.empty()) return std::string();
    std::string p = path;
    for (char& c : p)
        if (c == '\\') c = '/';
    bool unc = false;
    if (p.compare(0, 8, "//?/UNC/") == 0 || p.compare(0, 8, "//?/unc/") == 0) {
        p   = p.substr(8);
        unc = true;
    } else if (p.compare(0, 4, "//?/") == 0 || p.compare(0, 4, "//./") == 0) {
        p = p.substr(4);
    } else if (p.compare(0, 2, "//") == 0 && p.size() > 2 && p[2] != '/') {
        p   = p.substr(2);
        unc = true;
    }
    const bool absolute = !p.empty() && p[0] == '/' && !unc;
    std::vector<std::string> parts;
    size_t                   i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg == ".") {
            // folded away
        } else if (seg == "..") {
            const bool drive_root = parts.size() == 1 && parts[0].size() == 2 && parts[0][1] == ':'; // "C:" cannot be climbed out of
            if (!parts.empty() && parts.back() != "..") {
                if (!drive_root) parts.pop_back();
            } else if (!absolute && !unc) {
                parts.push_back(seg);
            }
        } else {
            parts.push_back(seg);
        }
        i = j + 1;
    }
    std::string out = unc ? "//" : (absolute ? "/" : "");
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) out += '/';
        out += parts[k];
    }
    if (case_insensitive)
        for (char& c : out)
            if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
    return out;
}

// Both given, and the same file once normalised.
inline bool same_exe(const std::string& a, const std::string& b, bool case_insensitive)
{
    if (a.empty() || b.empty()) return false;
    return normalize_path(a, case_insensitive) == normalize_path(b, case_insensitive);
}

// Whether `s` is well-formed UTF-8 (no overlongs, no surrogates, nothing past U+10FFFF). The exe path
// goes into hub.json and /hub/info, and a JSON writer throws on bytes that are not.
inline bool is_valid_utf8(const std::string& s)
{
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = (unsigned char) s[i];
        size_t len = 0;
        unsigned cp = 0;
        if (c < 0x80) { ++i; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = (unsigned char) s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// ---- is the running hub ours? ----

enum class ExeVerdict {
    Same,    // the hub runs from this very executable
    Foreign, // it runs from another one
    Unknown  // nothing reliable to compare (no path reported, no image path for its pid)
};

// `own` is this process's executable. `reported` is what the hub says about itself (hub.json and
// /hub/info "exe"); "" for a hub from before that field existed. `pid_image` is the image path of
// the hub's pid as the OS gives it (Windows: QueryFullProcessImageName), only worth looking up when
// `reported` is empty; "" when it was not asked for or could not be read. Unknown never leads to a
// handover: when in doubt the running hub is left alone.
inline ExeVerdict judge_exe(const std::string& own, const std::string& reported, const std::string& pid_image,
                            bool case_insensitive)
{
    if (own.empty()) return ExeVerdict::Unknown;
    if (!reported.empty()) return same_exe(own, reported, case_insensitive) ? ExeVerdict::Same : ExeVerdict::Foreign;
    if (!pid_image.empty()) return same_exe(own, pid_image, case_insensitive) ? ExeVerdict::Same : ExeVerdict::Foreign;
    return ExeVerdict::Unknown;
}

// ---- what ensure_running does about it ----

struct Facts
{
    bool       hub_alive { false };        // a hub answered /hub/info
    bool       version_differs { false };  // it reports another version than this build (the long-standing rule)
    ExeVerdict exe { ExeVerdict::Unknown };
    // false for a process the hub started itself (its hidden or visible instances, the Linux
    // service-mode supervisor's instance): the hub is by construction the one they were started
    // from, so they never ask it to step aside - and a process that has already had its one
    // handover (a window may reclaim the hub once; later calls use whatever runs, so two installs
    // open at once cannot take it from each other every time a tab is opened).
    bool       handover_allowed { true };
};

enum class Step {
    SpawnOwn,       // no hub: start one
    UseRunning,     // there is a hub and it stays
    ReplaceRunning  // ask it to quit; then see after_quit()
};

inline Step plan(const Facts& f)
{
    if (!f.hub_alive) return Step::SpawnOwn;
    if (f.version_differs) return Step::ReplaceRunning;
    if (f.exe == ExeVerdict::Foreign && f.handover_allowed) return Step::ReplaceRunning;
    return Step::UseRunning;
}

enum class Outcome {
    SpawnOwn,     // the old hub is gone: start ours
    LeaveRunning  // it did not go: log it and use it as it is; no forced kill, no second hub
};

// Only what the hub did decides it, not whether the request was accepted: a hub that went away
// anyway is gone, and one that accepted and is still there after the wait is not.
inline Outcome after_quit(bool old_pid_gone)
{
    return old_pid_gone ? Outcome::SpawnOwn : Outcome::LeaveRunning;
}

// ---- one hub at a time, and waiting for the one that is starting ----
//
// A test-copy log (2026-10-08) showed how two hubs ended up on one data dir: a slicer asked a
// foreign hub to hand over, spawned its own (A), gave up on A after 12 s while A was still starting,
// and a second caller in the same process - parked on the same mutex - then found no hub and spawned
// B. A came up a little later, then B, and both kept running on one hub.json, one hub.log and one
// set of state files; B held 13640, so the next hub moved to 13642. Three rules close that:
//  - a hub holds <hub dir>/hub.lock (an OS lock, released with the process however it ends) from
//    its first moment, and one that cannot take it exits: at most one hub per data dir, whoever
//    started it;
//  - a slicer waits for a hub that is starting - its own earlier spawn, or whoever holds the lock -
//    instead of starting another;
//  - a spawn of ours that still has not answered at the deadline is terminated, never left behind.

constexpr int QUIT_WAIT_MS      = 10000;  // the old hub's time to exit after /hub/quit
constexpr int PORT_FREE_WAIT_MS = 5000;   // ... and then its listener port's time to come free
constexpr int START_DEADLINE_MS = 120000; // a hub that has not answered this long after it was spawned is given up on
                                          // (a fresh install's first start can take well over a minute while
                                          // its binaries are scanned: 105 s in the 2026-10-08 log)
constexpr int LOCK_RETRY_MS     = 2000;   // a starting hub's retries for hub.lock: a slicer's probe holds it for
                                          // microseconds, a running hub for good

// After the old hub was asked to quit. `waited_ms` counts from the request, `since_gone_ms` from the
// moment its pid was seen gone. `port_free` is about the port it listened on (0: none known).
struct QuitFacts
{
    bool old_gone { false };
    bool port_free { true };
    int  waited_ms { 0 };
    int  since_gone_ms { 0 };
};

enum class QuitStep {
    Wait,             // look again
    SpawnOwn,         // gone, and its port is free
    SpawnOwnPortHeld, // gone, but something still holds the port: start ours anyway (it moves to the next port)
    LeaveRunning      // it did not go: use it as it is (after_quit()'s rule)
};

inline QuitStep after_quit_step(const QuitFacts& f)
{
    if (!f.old_gone) return f.waited_ms >= QUIT_WAIT_MS ? QuitStep::LeaveRunning : QuitStep::Wait;
    if (f.port_free) return QuitStep::SpawnOwn;
    return f.since_gone_ms >= PORT_FREE_WAIT_MS ? QuitStep::SpawnOwnPortHeld : QuitStep::Wait;
}

// No hub answers: start one, or wait for one that is already on its way.
struct Starting
{
    long own_pending_pid { 0 };     // the hub this process spawned that has not answered yet; 0 none
    bool own_pending_alive { false };
    bool lock_held { false };       // some process holds hub.lock: a hub is starting or running
};

enum class StartPlan {
    Spawn,     // nobody is starting one
    WaitOwn,   // our earlier spawn is still starting: never a second one next to it
    WaitOther  // another process's hub holds the lock
};

inline StartPlan start_plan(const Starting& s)
{
    if (s.own_pending_pid > 0 && s.own_pending_alive) return StartPlan::WaitOwn;
    if (s.lock_held) return StartPlan::WaitOther;
    return StartPlan::Spawn;
}

// One look while waiting for a hub to answer.
struct WaitFacts
{
    bool hub_answers { false };    // /hub/info answered
    bool own { false };            // the process waited on is one this process spawned
    bool process_alive { false };  // it is still running (for another's hub: the lock is still held)
    bool lock_held { false };      // some process holds hub.lock
    int  elapsed_ms { 0 };         // own: since it was spawned (across calls); other: since this call began waiting
};

enum class WaitStep {
    Up,            // it answers
    Wait,          // still starting: look again
    WaitOther,     // ours is gone but a hub holds the lock (ours lost the race to it): wait for that one
    Exited,        // gone and nobody holds the lock: it failed; the next call may spawn again
    Terminate,     // ours is past the deadline: stop it, or it comes up later as a second hub
    GiveUp         // another process's is past the deadline: not ours to stop; report no hub for now
};

inline WaitStep wait_step(const WaitFacts& f)
{
    if (f.hub_answers) return WaitStep::Up;
    if (!f.process_alive) return f.lock_held ? (f.own ? WaitStep::WaitOther : WaitStep::Wait) : WaitStep::Exited;
    if (f.elapsed_ms >= START_DEADLINE_MS) return f.own ? WaitStep::Terminate : WaitStep::GiveUp;
    return WaitStep::Wait;
}

// A quitting hub removes hub.json only when the record is its own (or unreadable). Before hub.lock,
// the second of two hubs deleted the first one's record when it quit, and slicers then found no hub.
inline bool remove_record_on_quit(long record_pid, long own_pid)
{
    return record_pid <= 0 || record_pid == own_pid;
}

// ---- the hub checking its own install ----

constexpr int SELF_CHECK_INTERVAL_S = 30; // how often the hub looks
constexpr int SELF_CHECK_MISSES     = 2;  // consecutive looks that must fail: an install being written
                                          // over, or a scanner holding a file, is not a deleted folder

struct SelfCheck
{
    int  misses { 0 };   // consecutive failed looks so far, including this one
    bool quit { false }; // exit cleanly now
};

// `previous_misses` is the count after the last look. A look that finds both the executable and the
// web pages resets it; one that misses either adds to it, and the hub quits at SELF_CHECK_MISSES.
inline SelfCheck self_check(int previous_misses, bool exe_exists, bool web_root_exists)
{
    SelfCheck r;
    if (exe_exists && web_root_exists) return r;
    r.misses = previous_misses < 0 ? 1 : previous_misses + 1;
    r.quit   = r.misses >= SELF_CHECK_MISSES;
    return r;
}

} // namespace HubHandover
} // namespace Slic3r

#endif // slic3r_HubHandover_hpp_
