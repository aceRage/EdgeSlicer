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

// Milliseconds since libslic3r's static initialisation, which runs before main(): close enough
// to process start to put "main frame shown" and "startup settled" on one clock across runs.
long long startup_profile_uptime_ms();

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

// Stepwise timer for one phase: mark("step") logs
// "<phase> step=<step> step_ms=<since last mark> total_ms=<since construction>", and the
// destructor logs "<phase> end total_ms=...". Used to split a long constructor or init block
// into its sub-steps without nesting a scope per step. Inert (one cached bool) when profiling
// is off; the step names are string literals, so nothing is formatted either.
class StartupPhase
{
public:
    explicit StartupPhase(const char *phase)
        : m_phase(phase), m_enabled(startup_profile_enabled()), m_start(std::chrono::steady_clock::now()), m_step(m_start)
    {}

    bool enabled() const { return m_enabled; }

    void mark(const char *step)
    {
        if (!m_enabled)
            return;
        const auto now = std::chrono::steady_clock::now();
        startup_profile_log(std::string(m_phase) + " step=" + step + " step_ms=" + std::to_string(ms(m_step, now)) +
                            " total_ms=" + std::to_string(ms(m_start, now)));
        m_step = now;
    }

    ~StartupPhase()
    {
        if (m_enabled)
            startup_profile_log(std::string(m_phase) + " end total_ms=" + std::to_string(ms(m_start, std::chrono::steady_clock::now())));
    }

private:
    static long long ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
    }

    const char                           *m_phase;
    bool                                  m_enabled;
    std::chrono::steady_clock::time_point m_start;
    std::chrono::steady_clock::time_point m_step;
};

// Accumulates time over many short intervals (per-file work in a loop) for one summary line.
// start()/stop() cost two clock reads when enabled and nothing when not.
class StartupAccumulator
{
public:
    explicit StartupAccumulator(bool enabled) : m_enabled(enabled) {}
    void start() { if (m_enabled) m_t0 = std::chrono::steady_clock::now(); }
    void stop()
    {
        if (m_enabled)
            m_total += std::chrono::steady_clock::now() - m_t0;
    }
    long long ms() const { return std::chrono::duration_cast<std::chrono::milliseconds>(m_total).count(); }

private:
    bool                                  m_enabled;
    std::chrono::steady_clock::time_point m_t0;
    std::chrono::steady_clock::duration   m_total{};
};

} // namespace Slic3r

// Convenience macro for a scoped mark that costs nothing when disabled.
// (Two-level paste so __LINE__ expands; a direct ## pasted the literal token "__LINE__".)
#define SLIC3R_STARTUP_SCOPE_CAT2(a, b) a##b
#define SLIC3R_STARTUP_SCOPE_CAT(a, b) SLIC3R_STARTUP_SCOPE_CAT2(a, b)
#define SLIC3R_STARTUP_SCOPE(name) Slic3r::StartupScopedTimer SLIC3R_STARTUP_SCOPE_CAT(slic3r_startup_scope_timer_, __LINE__)(name)

#endif // slic3r_StartupProfile_hpp_
