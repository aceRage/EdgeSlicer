#pragma once

// The loopback sign-in callback's "have I already exchanged this ticket?" memory - pure, wx-free
// and clock-injected, so HttpServer::bbl_auth_handle_request and the unit tests run the same code.
//
// Why it exists: after a Bambu third-party (Google) sign-in the browser lands on the loopback with
// ?ticket=<t>&redirect_url=<u>, and the handler exchanges the ticket for tokens (get_my_token). On
// 2026-10-08 that callback arrived twice in the same second (the browser repeats a navigation it
// thinks failed, or a prefetch beats the real one). A ticket is single use, so the second exchange
// got HTTP 401, logged "get_my_token: ticket exchange failed", and answered the browser with
// result=fail even though the sign-in itself had worked.
//
// The rule: a ticket is exchanged once. The first request to carry it owns the exchange; any other
// request for the same ticket, whether it arrives while the exchange is running or after, gets the
// owner's result (the same redirect) without a second call. A result is remembered for RESULT_TTL_MS
// when it was a success (long enough for any browser retry) and FAILURE_TTL_MS when it was not (so
// a person who really does try again a few seconds later is not told the old answer).
//
// Only a fingerprint of the ticket is kept, never the ticket, and mask() is the only way a ticket is
// ever written to a log.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace Slic3r {
namespace GUI {

class LoginTicketGate
{
public:
    // What the first request made of the ticket: where the browser was sent, and whether the
    // sign-in worked.
    struct Outcome
    {
        bool        success { false };
        std::string location; // the redirect (or "" when the answer was a plain page)
    };

    enum class Role {
        Owner,  // nobody has this ticket: exchange it, then call finish()
        Repeat, // somebody already did: answer with `outcome`, do not exchange
    };

    struct Claim
    {
        Role    role { Role::Owner };
        Outcome outcome; // for Repeat
    };

    static constexpr long long SUCCESS_TTL_MS = 5 * 60 * 1000;
    static constexpr long long FAILURE_TTL_MS = 10 * 1000;
    // How long a repeat waits for an exchange that is still running before it gives up and
    // answers with a failure (the owner's own answer is not affected).
    static constexpr long long WAIT_MS = 30 * 1000;

    // Claims `ticket` at `now_ms`. The first caller for a ticket is the Owner and MUST call finish()
    // (also on failure). A repeat blocks while the owner's exchange is in flight, up to WAIT_MS.
    Claim claim(const std::string& ticket, long long now_ms)
    {
        const std::string key = fingerprint(ticket);
        std::unique_lock<std::mutex> lock(m_mutex);
        purge(now_ms);
        auto it = m_entries.find(key);
        if (it == m_entries.end()) {
            Entry e;
            e.at         = now_ms;
            m_entries[key] = e;
            return Claim { Role::Owner, Outcome() };
        }
        if (!it->second.done) {
            m_cv.wait_for(lock, std::chrono::milliseconds(WAIT_MS), [&] {
                auto f = m_entries.find(key);
                return f == m_entries.end() || f->second.done;
            });
            it = m_entries.find(key);
            if (it == m_entries.end() || !it->second.done) return Claim { Role::Repeat, Outcome() };
        }
        return Claim { Role::Repeat, it->second.outcome };
    }

    // The owner's result. `now_ms` starts the result's lifetime.
    void finish(const std::string& ticket, const Outcome& outcome, long long now_ms)
    {
        const std::string key = fingerprint(ticket);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            Entry& e  = m_entries[key];
            e.done    = true;
            e.at      = now_ms;
            e.outcome = outcome;
        }
        m_cv.notify_all();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.size();
    }

    // The process-wide gate the loopback handler uses.
    static LoginTicketGate& instance()
    {
        static LoginTicketGate g;
        return g;
    }

    // The ticket parameter of a callback URL ("/?ticket=abc&redirect_url=..."), or "" when the
    // URL is not a ticket callback: the key has to be a whole query parameter, so a favicon request
    // or a path that merely contains the word "ticket" is not one.
    static std::string extract_ticket(const std::string& url)
    {
        const size_t q = url.find('?');
        if (q == std::string::npos) return std::string();
        size_t pos = q + 1;
        while (pos <= url.size()) {
            size_t end = url.find('&', pos);
            if (end == std::string::npos) end = url.size();
            const std::string kv = url.substr(pos, end - pos);
            if (kv.compare(0, 7, "ticket=") == 0) return kv.substr(7);
            pos = end + 1;
        }
        return std::string();
    }

    // The only spelling of a ticket that may reach a log: two characters and the length.
    static std::string mask(const std::string& ticket)
    {
        if (ticket.empty()) return "<none>";
        return ticket.substr(0, ticket.size() > 2 ? 2 : 1) + "***(" + std::to_string(ticket.size()) + ")";
    }

private:
    struct Entry
    {
        bool        done { false };
        long long   at { 0 }; // claimed, then finished
        Outcome     outcome;
    };

    // FNV-1a, 64 bit: enough to tell tickets apart in one process for a few minutes, and not the ticket.
    static std::string fingerprint(const std::string& ticket)
    {
        std::uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : ticket) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return std::to_string(h) + ":" + std::to_string(ticket.size());
    }

    void purge(long long now_ms)
    {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            const Entry& e = it->second;
            // An exchange still running is never purged; a stale one that never finished goes
            // after WAIT_MS so a crashed owner cannot hold a ticket forever.
            const long long ttl = !e.done ? WAIT_MS : (e.outcome.success ? SUCCESS_TTL_MS : FAILURE_TTL_MS);
            if (now_ms - e.at >= ttl)
                it = m_entries.erase(it);
            else
                ++it;
        }
    }

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::map<std::string, Entry> m_entries;
};

} // namespace GUI
} // namespace Slic3r
