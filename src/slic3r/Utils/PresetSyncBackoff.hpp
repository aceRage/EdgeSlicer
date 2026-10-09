#ifndef slic3r_PresetSyncBackoff_hpp_
#define slic3r_PresetSyncBackoff_hpp_

#include <algorithm>
#include <chrono>
#include <map>
#include <string>
#include <utility>

// Retry bookkeeping for the cloud user-preset sync (GUI_App::sync_preset).
//
// The sync thread walks every user preset that has no cloud setting id yet once per pass (roughly
// every two seconds, 100 ms between presets) and asks the network plug-in for an id. When the plug-in
// answers with an EMPTY id and an HTTP status below 400 - the UltraNet plug-in's request_setting_id
// is a stub that always does, and a real plug-in can do it on a transient failure - the host treats
// it as "try again" and leaves the preset untouched, so the very same request was repeated for every
// such preset on every pass, for as long as the app ran (about 5600 calls in a single log file).
//
// This class is the in-memory memory of those failures. It is held by the sync thread for one
// sign-in session, never written to disk, and kept free of wx so it can be unit-tested on its own
// (tests/slic3rutils/preset_sync_backoff_tests.cpp).
//
//  - Per preset: after a failed request the preset is left alone for a growing delay (5 min, 15 min,
//    45 min, then 2 h). A successful sync, or a change of the preset's sync_info (the user saved it
//    again, which Tab sets to "update"/"create"), clears the delay so a real edit still goes out at once.
//  - Per session: after kBreakerThreshold failed requests in a row with no success in between the
//    cloud evidently is not issuing ids, so every request is paused for a delay that grows the same
//    way. When it ends, one probe request is let through; a success reopens the gate, another failure
//    closes it again for longer. This keeps hundreds of presets from each paying for one doomed
//    request after every restart.
namespace Slic3r {
namespace PresetSync {

class Backoff
{
public:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // Failed requests in a row (across all presets) before the whole session is paused.
    static constexpr int kBreakerThreshold = 5;

    // Delay after the n-th consecutive failure of one preset (n >= 1), or of the session breaker
    // (n counted past the threshold): 5 min, 15 min, 45 min, then 2 h.
    static std::chrono::minutes delay_for(int failures)
    {
        static const int minutes[] = { 5, 15, 45, 120 };
        const int idx = std::min(std::max(failures, 1), 4) - 1;
        return std::chrono::minutes(minutes[idx]);
    }

    // Should the sync thread ask the cloud about this preset now? `sync_info` is the preset's current
    // sync_info; when it differs from what it was at the last failure the user has saved the preset
    // again, which forgets the preset's delay. `new_id_request` is true when the call would be a
    // request_setting_id (a preset the cloud has not seen); only those are held back by the session
    // breaker, so uploading edits of already-synced presets is never blocked by it.
    bool may_attempt(int type, const std::string& name, const std::string& sync_info, bool new_id_request, TimePoint now)
    {
        auto it = m_failed.find(Key(type, name));
        if (it != m_failed.end() && it->second.sync_info != sync_info) {
            m_failed.erase(it);
            it = m_failed.end();
        }
        if (it != m_failed.end() && now < it->second.retry_at)
            return false;
        if (new_id_request && breaker_closed(now))
            return false;
        return true;
    }

    // The cloud answered without a setting id (or an update call failed in a way that is worth
    // retrying). Only failed new-id requests count towards the session breaker. Returns true when
    // this failure closed the session breaker for the first time.
    bool record_failure(int type, const std::string& name, const std::string& sync_info, bool new_id_request, TimePoint now)
    {
        Entry& e   = m_failed[Key(type, name)];
        e.sync_info = sync_info;
        e.failures  = std::min(e.failures + 1, 1000);
        e.retry_at  = now + delay_for(e.failures);

        if (!new_id_request)
            return false;
        ++m_consecutive;
        if (m_consecutive >= kBreakerThreshold) {
            m_breaker_trips      = std::min(m_breaker_trips + 1, 1000);
            m_breaker_set        = true;
            m_breaker_until      = now + delay_for(m_breaker_trips);
            return m_breaker_trips == 1;
        }
        return false;
    }

    // A request produced a setting id, or the cloud accepted an update: everything is healthy again.
    void record_success(int type, const std::string& name)
    {
        m_failed.erase(Key(type, name));
        m_consecutive        = 0;
        m_breaker_trips      = 0;
        m_breaker_set        = false;
    }

    // Number of presets currently being held back, for logging.
    size_t failed_count() const { return m_failed.size(); }
    // How many failed requests in a row, with no success between them.
    int consecutive_failures() const { return m_consecutive; }
    // True while the session breaker is closed (requests paused).
    bool breaker_closed(TimePoint now) const { return m_breaker_set && now < m_breaker_until; }
    // Failure count of one preset (0 when it has none), for logging.
    int failures_of(int type, const std::string& name) const
    {
        auto it = m_failed.find(Key(type, name));
        return it == m_failed.end() ? 0 : it->second.failures;
    }

private:
    using Key = std::pair<int, std::string>;
    struct Entry
    {
        std::string sync_info;
        int         failures = 0;
        TimePoint   retry_at{};
    };

    std::map<Key, Entry> m_failed;
    int                  m_consecutive        = 0;
    int                  m_breaker_trips      = 0;
    bool                 m_breaker_set        = false;
    TimePoint            m_breaker_until{};
};

} // namespace PresetSync
} // namespace Slic3r

#endif // slic3r_PresetSyncBackoff_hpp_
