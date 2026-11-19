#include "StartupProfile.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

#include <boost/log/trivial.hpp>

namespace Slic3r {

bool startup_profile_enabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("ORCA_STARTUP_PROFILE");
        if (value == nullptr)
            return false;

        std::string normalized(value);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
    }();
    return enabled;
}

namespace {
const std::chrono::steady_clock::time_point s_static_init_time = std::chrono::steady_clock::now();
}

long long startup_profile_uptime_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_static_init_time).count();
}

void startup_profile_log(const std::string &message)
{
    if (startup_profile_enabled())
        BOOST_LOG_TRIVIAL(warning) << "[StartupProfile] " << message;
}

} // namespace Slic3r
