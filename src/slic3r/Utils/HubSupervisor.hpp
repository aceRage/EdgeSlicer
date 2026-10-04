#ifndef slic3r_HubSupervisor_hpp_
#define slic3r_HubSupervisor_hpp_

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

// What a service-mode hub (ServiceMode.hpp) decides about its slicer instances, kept free of
// processes, sockets and clocks so tests/slic3rutils/hub_supervisor_tests.cpp can step it through
// crashes, crash loops and recoveries with made-up times.
//
// On a desktop a person notices a dead slicer window and opens another. A headless hub has nobody,
// so when its instance dies the hub itself has to notice, forget what that instance last said
// about the printers, and start another - but not in a tight loop if the new one keeps dying the
// moment it starts (a broken install, no display), where it backs off instead.
namespace Slic3r {
namespace HubSupervisor {

struct Options
{
    int wanted          = 1;   // instances to keep running; 0 = supervision off
    int base_backoff_s  = 2;   // wait after a crash of an instance that had been running for a while
    int max_backoff_s   = 120; // the backoff never grows past this
    int stable_s        = 60;  // an instance that lived this long (once registered) was a healthy one
    int spawn_timeout_s = 120; // a spawn that has not registered by now is given up on
};

// How long to wait before the next spawn after `failures` quick deaths in a row: base, 2*base,
// 4*base ... capped. No failures (an instance that had been healthy died) waits the base.
inline int backoff_seconds(int failures, const Options& o)
{
    long long wait = o.base_backoff_s;
    for (int i = 1; i < failures && wait < o.max_backoff_s; ++i) wait *= 2;
    return (int) std::min<long long>(wait, o.max_backoff_s);
}

struct Lost
{
    long      pid { 0 };
    long long lived_s { 0 }; // since it registered
    bool      quick { false }; // died before it had proved itself (counts toward the backoff)
};

struct Decision
{
    bool              spawn { false };      // start one instance now
    std::vector<Lost> lost;                 // instances that were running and are gone
    std::vector<long> failed_spawns;        // spawned, never registered, and not coming (dead or timed out)
    int               failures { 0 };       // consecutive quick deaths, after this tick
    long long         next_spawn_in_s { 0 }; // 0 when a spawn is allowed now
};

class Supervisor
{
public:
    explicit Supervisor(Options o = Options()) : m_o(o) {}

    // The hub just started `pid` (<= 0 = the start itself failed).
    void note_spawn(long pid, long long now_s)
    {
        if (pid <= 0) {
            ++m_failures;
            m_next_spawn_at = now_s + backoff_seconds(m_failures, m_o);
            return;
        }
        m_spawned[pid] = now_s;
        m_next_spawn_at = std::max(m_next_spawn_at, now_s + 1); // never two spawns in one breath
    }

    // `live` = pids of instances that have registered (a <pid>.json and a live process);
    // `pending` = spawned instances that are still alive but have not registered yet.
    Decision tick(const std::vector<long>& live, const std::vector<long>& pending, long long now_s)
    {
        Decision d;
        const std::set<long> live_set(live.begin(), live.end());
        const std::set<long> pending_set(pending.begin(), pending.end());

        for (long pid : live) {
            if (m_known.count(pid) == 0) m_known[pid] = now_s;
            m_spawned.erase(pid);
        }
        // A spawn that is neither registered nor still starting did not work.
        for (auto it = m_spawned.begin(); it != m_spawned.end();) {
            const bool waiting = pending_set.count(it->first) && now_s - it->second < m_o.spawn_timeout_s;
            if (waiting) { ++it; continue; }
            ++m_failures;
            m_next_spawn_at = now_s + backoff_seconds(m_failures, m_o);
            d.failed_spawns.push_back(it->first);
            it = m_spawned.erase(it);
        }
        // An instance that was there and is not any more.
        for (auto it = m_known.begin(); it != m_known.end();) {
            if (live_set.count(it->first)) { ++it; continue; }
            Lost l;
            l.pid     = it->first;
            l.lived_s = std::max<long long>(0, now_s - it->second);
            l.quick   = l.lived_s < m_o.stable_s;
            if (l.quick) ++m_failures;
            else m_failures = 0;
            m_next_spawn_at = now_s + backoff_seconds(m_failures, m_o);
            d.lost.push_back(l);
            it = m_known.erase(it);
        }
        // One that has been up long enough clears the slate: the next crash is a first crash.
        for (const auto& kv : m_known)
            if (now_s - kv.second >= m_o.stable_s) m_failures = 0;

        int starting = 0;
        for (const auto& kv : m_spawned) if (pending_set.count(kv.first)) ++starting;
        // Instances started by someone else (a person, /hub/new) count too: only a shortage spawns.
        const int have = (int) live.size() + starting;
        if (m_o.wanted > 0 && have < m_o.wanted && now_s >= m_next_spawn_at) d.spawn = true;
        d.failures        = m_failures;
        d.next_spawn_in_s = now_s >= m_next_spawn_at ? 0 : m_next_spawn_at - now_s;
        return d;
    }

    int failures() const { return m_failures; }

private:
    Options                 m_o;
    std::map<long, long long> m_known;   // registered pid -> when first seen registered
    std::map<long, long long> m_spawned; // spawned, not yet registered: pid -> when
    int                     m_failures { 0 };
    long long               m_next_spawn_at { 0 };
};

// ---- the printer rows the hub remembers --------------------------------------------------------

struct CachedRow
{
    std::string id;
    long long   at_ms { 0 };    // when the value was read
    long        instance { 0 }; // the pid that reported it; 0 = none (the previous run's)
};

inline bool is_live(long pid, const std::vector<long>& live)
{
    return pid > 0 && std::find(live.begin(), live.end(), pid) != live.end();
}

// The rows to forget in service mode: nobody alive reported them, and nobody has refreshed them for
// longer than `ttl_ms`. A row that is merely old but still has its reporter keeps its stale flag
// (the instance may be busy slicing); one whose reporter is gone and whose replacement has not
// picked it up within the TTL is a printer this hub no longer knows anything about, and a phone
// that keeps seeing "last seen 3 hours ago" for it is being told something false.
inline std::vector<std::string> stale_rows_to_drop(const std::vector<CachedRow>& rows, const std::vector<long>& live, long long now_ms, long long ttl_ms)
{
    std::vector<std::string> out;
    for (const CachedRow& r : rows)
        if (!is_live(r.instance, live) && now_ms - r.at_ms > ttl_ms) out.push_back(r.id);
    return out;
}

} // namespace HubSupervisor
} // namespace Slic3r

#endif // slic3r_HubSupervisor_hpp_
