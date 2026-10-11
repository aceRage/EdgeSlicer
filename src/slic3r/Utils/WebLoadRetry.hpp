#ifndef slic3r_WebLoadRetry_hpp_
#define slic3r_WebLoadRetry_hpp_

#include <cctype>
#include <string>

// What the Device tab (PrinterWebView) does when a page fails to load, kept free of wx so it can
// be tested (tests/slic3rutils/web_load_retry_tests.cpp).
//
// Its pages come from this slicer's own loopback page server. A load there can fail for a moment:
// on 2026-10-08 missing_connection.html failed twice with CONNECTION_ABORTED while the Mobile Hub
// was being handed from one install to another and the GUI thread was busy, and each failure was
// logged at fatal and left the tab on the browser's error page. A connection error on a loopback
// page is now retried with a growing delay and logged as a warning; only running out of retries
// is an error. Nothing here is ever fatal.
namespace Slic3r {
namespace WebLoadRetry {

enum class Failure {
    Connection, // could not connect, or the connection dropped (wxWEBVIEW_NAV_ERR_CONNECTION)
    Cancelled,  // the load was cancelled, usually because another one replaced it
    Other       // certificate, auth, security, not found, ...: retrying will not change it
};

constexpr int MAX_RETRIES = 6;

// 0.5 s, 1, 2, 4, 8, 8: about 23 s in all, enough for the page server to come back.
inline int delay_ms(int retries_so_far)
{
    if (retries_so_far < 0) retries_so_far = 0;
    if (retries_so_far >= 4) return 8000;
    return 500 << retries_so_far;
}

struct Facts
{
    Failure failure { Failure::Other };
    bool    loopback { false };   // the failed URL is on this machine (127.0.0.1, localhost, [::1])
    bool    superseded { false }; // the view has since been sent to another page
    int     retries_so_far { 0 }; // for this page
};

enum class Log { Info, Warning, Error };

struct Decision
{
    bool retry { false };
    int  delay_ms { 0 };
    Log  log { Log::Error };
};

inline Decision decide(const Facts& f)
{
    Decision d;
    if (f.superseded || f.failure == Failure::Cancelled) {
        d.log = Log::Info; // nothing anybody is looking at failed
        return d;
    }
    if (f.failure == Failure::Connection && f.loopback && f.retries_so_far < MAX_RETRIES) {
        d.retry    = true;
        d.delay_ms = delay_ms(f.retries_so_far);
        d.log      = Log::Warning;
        return d;
    }
    d.log = Log::Error;
    return d;
}

// The page a URL names, without its query and fragment: the page server's token rides in the query
// and may be added on the way, so two spellings of one page compare equal here.
inline std::string page_of(const std::string& url)
{
    const size_t cut = url.find_first_of("?#");
    return cut == std::string::npos ? url : url.substr(0, cut);
}

// Whether `url` is http(s) on this machine's loopback.
inline bool is_loopback_url(const std::string& url)
{
    const size_t sep = url.find("://");
    if (sep == std::string::npos) return false;
    std::string scheme = url.substr(0, sep);
    for (char& c : scheme) c = (char) std::tolower((unsigned char) c);
    if (scheme != "http" && scheme != "https") return false;
    const size_t start = sep + 3;
    size_t       end   = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    std::string authority = url.substr(start, end - start);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    std::string host;
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) return false;
        host = authority.substr(1, close - 1);
    } else {
        host = authority.substr(0, authority.find(':'));
    }
    for (char& c : host) c = (char) std::tolower((unsigned char) c);
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

} // namespace WebLoadRetry
} // namespace Slic3r

#endif // slic3r_WebLoadRetry_hpp_
