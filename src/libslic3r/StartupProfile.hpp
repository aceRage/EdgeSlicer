#ifndef slic3r_StartupProfile_hpp_
#define slic3r_StartupProfile_hpp_

// Startup profiling helpers, shared by libslic3r and the GUI.
//
// Everything here is inert unless the environment variable ORCA_STARTUP_PROFILE is
// set to 1/true/yes/on, so the instrumentation costs a single cached bool read on a
// normal run. Output goes to the boost log at warning level, prefixed with
// "[StartupProfile] " so a run's marks can be grepped out of the app log.

#include <chrono>
#include <string>

namespace Slic3r {

// Cached read of ORCA_STARTUP_PROFILE.
bool startup_profile_enabled();

// Emit one "[StartupProfile] <message>" line, when profiling is enabled.
void startup_profile_log(const std::string &message);

// Scoped timer: logs "<name> <suffix> ms=<elapsed>" on destruction. The suffix lets a
// caller attach a count or a vendor name to the mark without formatting a string when
// profiling is off.
class StartupScopedTimer
{
public:
    explicit StartupScopedTimer(const char *name)
        : m_name(name), m_enabled(startup_profile_enabled()), m_start(std::chrono::steady_clock::now())
    {}

    // Extra text appended to the mark; only materialised when profiling is on.
    void note(const std::string &suffix) { if (m_enabled) m_suffix += " " + suffix; }

    long long elapsed_ms() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count();
    }

    ~StartupScopedTimer()
    {
        if (m_enabled)
            startup_profile_log(std::string(m_name) + m_suffix + " ms=" + std::to_string(elapsed_ms()));
    }

private:
    const char                           *m_name;
    bool                                  m_enabled;
    std::chrono::steady_clock::time_point m_start;
    std::string                           m_suffix;
};

} // namespace Slic3r

// Convenience macro for a scoped mark that costs nothing when disabled.
#define SLIC3R_STARTUP_SCOPE(name) Slic3r::StartupScopedTimer slic3r_startup_scope_timer_##__LINE__(name)

#endif // slic3r_StartupProfile_hpp_
