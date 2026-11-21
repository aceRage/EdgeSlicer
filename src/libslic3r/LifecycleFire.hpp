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
