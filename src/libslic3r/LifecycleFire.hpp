#pragma once

// LifecycleFire.hpp
// -----------------
// Fire sites for the plugin lifecycle events (LifecycleEvents.hpp), ported from OrcaSlicer.
//
//   EDGE_LIFECYCLE_FIRE(LifecycleEvent::SliceStarted,
//       ctx.id   = ...;
//       ctx.name = ...;);
//
// The second argument is a list of statements that fill `ctx` (a LifecycleEventContext).
//
// Flag OFF (EDGE_PYTHON_PLUGINS undefined, every release build): both macros expand to nothing.
// The statements are dropped by the preprocessor, so no payload is built, no string is formatted,
// no lock is taken and LifecycleEvents.hpp is not even included.
//
// Flag ON: the statements run only while a lifecycle hook is installed (the plugin runtime
// installs it at startup), on the caller's thread, exactly where OrcaSlicer fires the event.
// Nothing escapes: an exception from building the payload or from a listener is swallowed, so a
// fire site can never throw into slicing, G-code export or the UI.
//
// EDGE_LIFECYCLE_FIRE_IF(COND, EVENT, ...) fires only when COND holds; COND is not evaluated in
// OFF builds either.
//
// EDGE_LIFECYCLE_ONLY(...) keeps a statement (e.g. capturing a name before an object is deleted)
// in ON builds only, so OFF builds do not gain even an unused local.
//
// libslic3r gets EDGE_PYTHON_PLUGINS as a PRIVATE definition in ON builds only
// (src/libslic3r/CMakeLists.txt); libslic3r_gui gets it PUBLIC (src/slic3r/CMakeLists.txt).

#ifdef EDGE_PYTHON_PLUGINS

#include "LifecycleEvents.hpp"

#include <mutex>
#include <string>

namespace Slic3r {
namespace detail {

    // True while a hook accepts events. Lets a fire site skip building its payload when nothing
    // listens (CLI slicing, tests, plugin runtime shut down).
    inline bool lifecycle_hook_accepting()
    {
        LifecycleHookState&         state = lifecycle_hook_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        return state.accepting && static_cast<bool>(state.fn);
    }

    // Upload / send error texts can quote the printer's URL. Before such a text goes into a payload,
    // drop URL user info (user:password@) and query strings (?apikey=...): plugins never see a
    // credential that rode inside a URL. Plain text without "://" is returned unchanged.
    inline std::string lifecycle_mask_url_secrets(const std::string &text)
    {
        static const char *const k_url_end = " \t\r\n\"'<>()[]{},;";
        std::string out;
        out.reserve(text.size());
        size_t pos = 0;
        while (pos < text.size()) {
            const size_t scheme = text.find("://", pos);
            if (scheme == std::string::npos) {
                out.append(text, pos, std::string::npos);
                break;
            }
            const size_t authority = scheme + 3;
            out.append(text, pos, authority - pos);
            size_t url_end = text.find_first_of(k_url_end, authority);
            if (url_end == std::string::npos)
                url_end = text.size();
            std::string url = text.substr(authority, url_end - authority);
            const size_t host_end = url.find_first_of("/?#");
            const size_t at       = url.rfind('@', host_end == std::string::npos ? std::string::npos : host_end);
            if (at != std::string::npos && (host_end == std::string::npos || at < host_end))
                url = "***" + url.substr(at);
            const size_t query = url.find_first_of("?#");
            if (query != std::string::npos)
                url.erase(query);
            out += url;
            pos = url_end;
        }
        return out;
    }

    template<class Fill>
    inline void fire_lifecycle_event_guarded(LifecycleEvent event, Fill&& fill) noexcept
    {
        try {
            if (!lifecycle_hook_accepting())
                return;
            LifecycleEventContext ctx;
            fill(ctx);
            fire_lifecycle_event(event, ctx);
        } catch (...) {
            // Lifecycle events are observations: never let one disturb the work being observed.
        }
    }

} // namespace detail
} // namespace Slic3r

#define EDGE_LIFECYCLE_FIRE(EVENT, ...) \
    ::Slic3r::detail::fire_lifecycle_event_guarded((EVENT), [&](::Slic3r::LifecycleEventContext& ctx) { __VA_ARGS__ })
#define EDGE_LIFECYCLE_FIRE_IF(COND, EVENT, ...) \
    do { if (COND) EDGE_LIFECYCLE_FIRE(EVENT, __VA_ARGS__); } while (false)
#define EDGE_LIFECYCLE_ONLY(...) __VA_ARGS__

#else // EDGE_PYTHON_PLUGINS

#define EDGE_LIFECYCLE_FIRE(EVENT, ...) do { } while (false)
#define EDGE_LIFECYCLE_FIRE_IF(COND, EVENT, ...) do { } while (false)
#define EDGE_LIFECYCLE_ONLY(...)

#endif // EDGE_PYTHON_PLUGINS
