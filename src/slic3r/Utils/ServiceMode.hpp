#ifndef slic3r_ServiceMode_hpp_
#define slic3r_ServiceMode_hpp_

#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <stdlib.h>
#endif

// "Service mode": EdgeSlicer running as an unattended hub (a Linux box, a container, a Pi), where
// nobody is there to answer a first-run question. It is switched on explicitly, by the hub's
// --hub-service flag or the EDGESLICER_SERVICE=1 environment variable, and by nothing else - a
// desktop start never changes behaviour. The hub passes it to the slicer instances it spawns
// through the environment, so one flag on the hub covers the whole tree.
//
// What it changes (each place says so where it acts):
//   * first start: a minimal config is seeded when none exists (AppConfig::seed_service_defaults),
//     and the printer wizard, the privacy prompt, the "use the system TLS certificate store"
//     question and the "Switching language failed" box are skipped (logged, never shown);
//   * the hub starts a slicer instance by itself, keeps one running and respawns it after a crash
//     with backoff (HubSupervisor.hpp), and drops printer rows nobody has refreshed;
//   * on Linux an instance realises its window on the (virtual) display so OpenGL works.
//
// Kept free of wx and of the rest of the app so the rules can be tested on their own
// (tests/slic3rutils/service_mode_tests.cpp).
namespace Slic3r {
namespace ServiceMode {

constexpr const char* ENV_SERVICE     = "EDGESLICER_SERVICE";
constexpr const char* ENV_PUBLIC_HOST = "EDGESLICER_HUB_PUBLIC_HOST";
constexpr const char* ENV_INSTANCES   = "EDGESLICER_SERVICE_INSTANCES";

// "1", "true", "yes", "on" in any case; anything else (including empty) is false.
inline bool is_truthy(const std::string& value)
{
    std::string v;
    for (char c : value)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') v.push_back((char) ((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

inline bool enabled()
{
    const char* v = std::getenv(ENV_SERVICE);
    return v != nullptr && is_truthy(v);
}

inline void set_env(const char* name, const std::string& value)
{
#ifdef _WIN32
    ::_putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

// What --hub-service does: later spawns (instances, the hub's own children) inherit it.
inline void enable() { set_env(ENV_SERVICE, "1"); }

// How many slicer instances a service-mode hub keeps running. 1 unless EDGESLICER_SERVICE_INSTANCES
// says 0..4 (0 = the hub does not start one; whoever drives it does).
inline int wanted_instances(const char* env_value)
{
    if (env_value == nullptr || *env_value == '\0') return 1;
    char*      end = nullptr;
    const long n   = std::strtol(env_value, &end, 10);
    if (end == env_value || *end != '\0' || n < 0 || n > 4) return 1;
    return (int) n;
}

inline int wanted_instances()
{
    return wanted_instances(std::getenv(ENV_INSTANCES));
}

} // namespace ServiceMode
} // namespace Slic3r

#endif // slic3r_ServiceMode_hpp_
