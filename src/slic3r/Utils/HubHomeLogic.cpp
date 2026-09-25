#include "HubHomeLogic.hpp"

namespace Slic3r {
namespace HubHome {

bool valid_token(const std::string& token)
{
    return token.size() >= 10 && token.size() <= 32 &&
           token.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos;
}

static bool valid_port(int port) { return port > 0 && port <= 65535; }

const char* theme_name(bool dark) { return dark ? "dark" : "light"; }

static std::string hub_page_prefix(int port, const std::string& token)
{
    return "http://127.0.0.1:" + std::to_string(port) + "/r/" + token + "/";
}

std::string hub_page_url(int port, const std::string& token, bool dark)
{
    if (!valid_port(port) || !valid_token(token))
        return std::string();
    return hub_page_prefix(port, token) + "?embed=1&theme=" + theme_name(dark);
}

bool is_hub_page_url(const std::string& url, int port, const std::string& token)
{
    if (!valid_port(port) || !valid_token(token))
        return false;
    const std::string prefix = hub_page_prefix(port, token);
    if (url.compare(0, prefix.size(), prefix) == 0)
        return true;
    // The bare /r/<token> (no trailing slash) redirects to the page; allow it, and only exactly it
    // or it followed by a query/fragment.
    const std::string bare = prefix.substr(0, prefix.size() - 1);
    return url == bare || (url.size() > bare.size() && url.compare(0, bare.size(), bare) == 0 &&
                           (url[bare.size()] == '?' || url[bare.size()] == '#'));
}

bool is_allowed_navigation(const std::string& url, int port, const std::string& token)
{
    if (url.empty() || url == "about:blank" || url.compare(0, 5, "data:") == 0)
        return true;
    return is_hub_page_url(url, port, token);
}

bool is_external_browser_url(const std::string& url)
{
    auto starts_ci = [&url](const char* p) {
        size_t i = 0;
        for (; p[i] != 0; ++i) {
            if (i >= url.size())
                return false;
            char c = url[i];
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
            if (c != p[i])
                return false;
        }
        return url.size() > i; // something after the scheme
    };
    if (!(starts_ci("http://") || starts_ci("https://")))
        return false;
    // No whitespace or control characters: nothing that could be read as a second argument.
    for (unsigned char c : url)
        if (c <= 0x20 || c == 0x7f)
            return false;
    return true;
}

Recheck recheck(bool hub_alive, int hub_port, const std::string& hub_token, int loaded_port,
                const std::string& loaded_token)
{
    if (!hub_alive || !valid_port(hub_port) || !valid_token(hub_token))
        return Recheck::HubDown;
    if (hub_port != loaded_port || hub_token != loaded_token)
        return Recheck::Reload;
    return Recheck::Keep;
}

ExitReason exit_reason_from(const std::string& reason)
{
    if (reason == "tray")
        return ExitReason::Tray;
    if (reason == "request")
        return ExitReason::Requested;
    if (reason == "idle")
        return ExitReason::Idle;
    return ExitReason::Unknown;
}

Presence classify_record(bool file_exists, bool parsed, bool pid_alive)
{
    if (!file_exists)
        return Presence::NoRecord;
    if (!parsed)
        return Presence::Unknown; // there but unreadable: most likely caught mid-rewrite
    return pid_alive ? Presence::Running : Presence::Crashed;
}

const char* verdict_name(Verdict v)
{
    switch (v) {
    case Verdict::Keep: return "keep";
    case Verdict::Reload: return "reload";
    case Verdict::Retry: return "retry";
    case Verdict::Unreachable: return "unreachable";
    case Verdict::Gone: return "gone";
    }
    return "?";
}

const char* presence_name(Presence p)
{
    switch (p) {
    case Presence::Unknown: return "record unreadable";
    case Presence::Running: return "record says running";
    case Presence::NoRecord: return "no record (quit cleanly)";
    case Presence::Crashed: return "record names a dead process";
    }
    return "?";
}

const char* exit_reason_name(ExitReason r)
{
    switch (r) {
    case ExitReason::Unknown: return "unknown";
    case ExitReason::Tray: return "tray";
    case ExitReason::Requested: return "request";
    case ExitReason::Idle: return "idle";
    }
    return "?";
}

Verdict Watch::judge(const Probe& probe, int loaded_port, const std::string& loaded_token)
{
    if (probe.answered && valid_port(probe.port) && valid_token(probe.token)) {
        reset();
        return recheck(true, probe.port, probe.token, loaded_port, loaded_token) == Recheck::Keep ? Verdict::Keep
                                                                                                    : Verdict::Reload;
    }
    // An answer that cannot be used counts as no answer; so does everything else.
    ++m_failures;
    const bool gone_signal = !probe.answered && (probe.presence == Presence::NoRecord || probe.presence == Presence::Crashed);
    m_gone                 = gone_signal ? m_gone + 1 : 0;
    if (m_gone >= GONE_AFTER)
        return Verdict::Gone;
    if (!gone_signal && m_failures >= UNREACHABLE_AFTER)
        return Verdict::Unreachable;
    return Verdict::Retry;
}

int Watch::next_check_s() const
{
    switch (m_failures) {
    case 0: return RECHECK_S;
    case 1: return 2;
    case 2: return 4;
    case 3: return 8;
    default: return 10;
    }
}

LoadError classify_load_error(bool cancelled, bool connection, const std::string& url, int loaded_port,
                              const std::string& loaded_token)
{
    if (cancelled)
        return LoadError::Ignore;
    // A stale navigation to a page this view has already left (the old port after a move, the old
    // link after "New link") or to anything that is not the hub page is not about what is loaded.
    // An empty address or about:blank is kept: a first load that failed can still report the blank
    // page it started from.
    if (!url.empty() && url != "about:blank" && !is_hub_page_url(url, loaded_port, loaded_token))
        return LoadError::Ignore;
    return connection ? LoadError::Connection : LoadError::Page;
}

std::string theme_script(bool dark)
{
    return std::string("if (window.__edgeTheme) window.__edgeTheme('") + theme_name(dark) + "');";
}

} // namespace HubHome
} // namespace Slic3r
