#include "bury_point.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

// The flutter home page's "privacy policy agreed" flag. Off until the saved config says
// otherwise: nothing may start out as consented. (Crash-report consent is separate - see
// SentryWrapper.cpp and the send_crash_reports preference.)
static std::atomic<bool> isAgreeSlice(false);

// True when this build can send crash reports at all (a DSN was compiled in, or a tester set
// ultra_sentry_dsn). Lives here, not in SentryWrapper.cpp, because on Windows that file is
// compiled into both the launcher exe (which initialises Sentry) and EdgeSlicer.dll (whose GUI
// toggles consent), and only this exported copy is shared between the two.
static std::atomic<bool> g_crash_reports_available(false);

static std::atomic<bool> g_sentry_initialized(false);

bool get_sentry_flags() { return g_sentry_initialized; }

void set_sentry_flags(bool flags) { g_sentry_initialized = flags; }

bool get_crash_reports_available() { return g_crash_reports_available; }

void set_crash_reports_available(bool available) { g_crash_reports_available = available; }

bool get_privacy_policy() 
{
    return isAgreeSlice; 
}

void set_privacy_policy(bool isAgree) { 
    isAgreeSlice = isAgree; 
}

std::string get_timestamp_seconds()
{
    auto now = std::chrono::system_clock::now();

    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    std::ostringstream oss;
    oss << timestamp;
    auto strTime = oss.str();

    return strTime;
}

long long get_time_timestamp()
{
    auto now = std::chrono::system_clock::now();

    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    return timestamp;
}

std::string get_works_time(const long long& timestamp)
{ 
    long long hours        = timestamp / 3600000;
    long long remaining_ms = timestamp % 3600000;

    long long minutes = remaining_ms / 60000;
    remaining_ms     = remaining_ms % 60000;

    long long seconds = remaining_ms / 1000;
    long long ms      = remaining_ms % 1000;

    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu:%02llu.%03llu", hours, minutes, seconds, ms);


    std::string works_time = std::string(buffer);
    
    return works_time;
}