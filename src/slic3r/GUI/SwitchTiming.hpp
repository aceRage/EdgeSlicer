#ifndef slic3r_GUI_SwitchTiming_hpp_
#define slic3r_GUI_SwitchTiming_hpp_

// DIAGNOSTIC ONLY (branch diag/printer-switch-timing, not for merge).
//
// Scoped timers around the steps of a printer preset switch. Inert unless the environment
// variable EDGE_SWITCH_TIMING is 1/true/yes/on: then each scope logs one info line
//   [SwitchTiming] <indent><name> ms=<elapsed>
// when it ends, indented by nesting depth, so the lead can read a per-step tree from the log.
// Off, a scope costs one cached bool read. UI thread only (the depth counter is not atomic).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI { namespace switch_timing {

inline bool enabled()
{
    static const bool on = [] {
        const char *v = std::getenv("EDGE_SWITCH_TIMING");
        if (v == nullptr)
            return false;
        const std::string s(v);
        return s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on" || s == "ON";
    }();
    return on;
}

inline int &depth()
{
    static int d = 0;
    return d;
}

// Time of the last printer switch start, for "since switch" marks (first idle, slow frames).
inline std::chrono::steady_clock::time_point &switch_start()
{
    static std::chrono::steady_clock::time_point t{};
    return t;
}

inline bool &switch_pending()
{
    static bool p = false;
    return p;
}

inline long long ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

inline double ms_since_f(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

inline void log_line(const std::string &msg)
{
    if (enabled())
        BOOST_LOG_TRIVIAL(info) << "[SwitchTiming] " << msg;
}

inline const char *type_name(int preset_type)
{
    // Preset::Type: 0 invalid, 1 print, 2 sla print, 3 filament, 4 sla material, 5 printer.
    switch (preset_type) {
    case 1: return "print";
    case 2: return "sla_print";
    case 3: return "filament";
    case 4: return "sla_material";
    case 5: return "printer";
    default: return "other";
    }
}

// Called at the start of a printer switch (Plater::priv::on_select_preset, printer combo).
inline void mark_switch_start(const std::string &what)
{
    if (!enabled())
        return;
    switch_start()   = std::chrono::steady_clock::now();
    switch_pending() = true;
    log_line("==== switch start: " + what);
}

class Scope
{
public:
    explicit Scope(const char *name) : m_name(name), m_on(enabled())
    {
        if (m_on) {
            m_start = std::chrono::steady_clock::now();
            ++depth();
        }
    }
    Scope(const char *name, const char *tag) : Scope(name)
    {
        if (m_on)
            m_tag = tag ? tag : "";
    }
    void note(const std::string &s)
    {
        if (m_on)
            m_tag += (m_tag.empty() ? "" : " ") + s;
    }
    ~Scope()
    {
        if (!m_on)
            return;
        const double ms = ms_since_f(m_start);
        --depth();
        std::string indent(size_t(depth() > 0 ? depth() : 0) * 2, ' ');
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", ms);
        BOOST_LOG_TRIVIAL(info) << "[SwitchTiming] " << indent << m_name << (m_tag.empty() ? "" : " [") << m_tag
                                << (m_tag.empty() ? "" : "]") << " ms=" << buf;
    }

private:
    const char                           *m_name;
    bool                                  m_on;
    std::chrono::steady_clock::time_point m_start;
    std::string                           m_tag;
};

// One canvas frame: logged only for frames that start within 5 s after a printer switch.
class FrameScope
{
public:
    explicit FrameScope(const char *canvas) : m_canvas(canvas), m_on(false)
    {
        if (!enabled() || switch_start() == std::chrono::steady_clock::time_point{})
            return;
        m_since = ms_since(switch_start());
        if (m_since > 5000)
            return;
        m_on    = true;
        m_start = std::chrono::steady_clock::now();
    }
    ~FrameScope()
    {
        if (!m_on)
            return;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", ms_since_f(m_start));
        BOOST_LOG_TRIVIAL(info) << "[SwitchTiming] frame " << m_canvas << " ms=" << buf << " at +" << m_since << " ms";
    }

private:
    const char                           *m_canvas;
    bool                                  m_on;
    long long                             m_since = 0;
    std::chrono::steady_clock::time_point m_start;
};

}}} // namespace Slic3r::GUI::switch_timing

#define SWITCH_TIMING_CAT2(a, b) a##b
#define SWITCH_TIMING_CAT(a, b) SWITCH_TIMING_CAT2(a, b)
#define SWITCH_TIMING(name) Slic3r::GUI::switch_timing::Scope SWITCH_TIMING_CAT(switch_timing_scope_, __LINE__)(name)
#define SWITCH_TIMING_TAG(name, tag) Slic3r::GUI::switch_timing::Scope SWITCH_TIMING_CAT(switch_timing_scope_, __LINE__)(name, tag)

#endif // slic3r_GUI_SwitchTiming_hpp_
