#include "SnapmakerSilentLogin.hpp"

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace SMSilentLogin {

std::string skip_reason(const StartupInputs& in)
{
    // Order matters only for which reason the log names when several apply; the cheapest and
    // most permanent reasons first.
    if (!in.pref_enabled)
        return "turned off in Preferences";
    if (!in.signed_in_before)
        return "not signed in to Snapmaker on this computer";
    if (!in.is_editor)
        return "G-code viewer";
    if (in.hidden_instance)
        return "hidden instance";
    if (!in.main_window_ready)
        return "main window not ready";
    if (in.already_signed_in)
        return "already signed in";
    if (in.login_dialog_open)
        return "sign-in dialog already open";
    if (in.network_down)
        return "no network";
    return std::string();
}

Tick on_tick(int elapsed_ms, bool page_loaded, int since_load_ms)
{
    if (page_loaded && since_load_ms >= k_settle_ms)
        return Tick::NoSession;
    if (elapsed_ms >= k_overall_timeout_ms)
        // A page that loaded but kept re-navigating is still a page without a session; one that
        // never loaded at all is a network or service problem.
        return page_loaded ? Tick::NoSession : Tick::TimedOut;
    return Tick::Wait;
}

// Where the value of the "token" parameter starts, or npos. A parameter starts right after '?',
// '&' or '#'; "access_token=" and friends are not it.
static size_t token_value_start(const std::string& url)
{
    static const std::string key = "token=";
    for (size_t pos = url.find(key); pos != std::string::npos; pos = url.find(key, pos + 1)) {
        if (pos > 0) {
            const char before = url[pos - 1];
            if (before == '?' || before == '&' || before == '#')
                return pos + key.size();
        }
    }
    return std::string::npos;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool has_token_param(const std::string& url) { return token_value_start(url) != std::string::npos; }

std::string token_from_url(const std::string& url)
{
    const size_t start = token_value_start(url);
    if (start == std::string::npos)
        return std::string();
    const size_t end = url.find_first_of("&#?", start);
    const std::string raw = end == std::string::npos ? url.substr(start) : url.substr(start, end - start);
    // URLSearchParams percent-encodes what is not URL-safe ('+', '/', '=' of a base64 token). A
    // literal '+' is kept as it is: a token has no spaces.
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size()) {
            const int hi = hex_value(raw[i + 1]), lo = hex_value(raw[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(char(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

// A server message for the log: printable ASCII only, short.
static std::string log_safe(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (out.size() >= 80)
            break;
        out.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    }
    return out;
}

AccountAnswer read_account_answer(unsigned http_status, const std::string& body)
{
    AccountAnswer a;
    if (http_status != 200) {
        a.why = "HTTP " + std::to_string(http_status);
        // 401 / 403: the service looked at the token and said no.
        a.rejected = http_status == 401 || http_status == 403;
        return a;
    }
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, /* allow_exceptions */ false);
    if (j.is_discarded() || !j.is_object()) {
        a.why = "unreadable answer";
        return a;
    }
    // {"code":110002,"msg":"Missing authorization"}: HTTP 200, but a refusal.
    if (j.contains("code") && j["code"].is_number_integer()) {
        const long long code = j["code"].get<long long>();
        if (code != 0 && code != 200) {
            a.why = "rejected: code " + std::to_string(code);
            if (j.contains("msg") && j["msg"].is_string())
                a.why += " (" + log_safe(j["msg"].get<std::string>()) + ")";
            a.rejected = true;
            return a;
        }
    }
    if (!j.contains("data") || !j["data"].is_object()) {
        a.why      = "no account in the answer";
        a.rejected = true;
        return a;
    }
    const nlohmann::json& d = j["data"];
    if (d.contains("id")) {
        const nlohmann::json& id = d["id"];
        if (id.is_number_integer())
            a.user_id = std::to_string(id.get<long long>());
        else if (id.is_number_unsigned())
            a.user_id = std::to_string(id.get<unsigned long long>());
        else if (id.is_string())
            a.user_id = id.get<std::string>();
    }
    if (a.user_id.empty()) {
        a.why      = "no account id in the answer";
        a.rejected = true;
        return a;
    }
    auto text = [&d](const char* key) {
        return d.contains(key) && d[key].is_string() ? d[key].get<std::string>() : std::string();
    };
    a.nickname = text("nickname");
    a.icon     = text("icon");
    a.account  = text("account");
    a.ok       = true;
    return a;
}

std::vector<std::string> session_cookie_names() { return {"login_access_token", "login_refresh_token"}; }

std::string session_cookie_uri(bool china_region)
{
    return china_region ? "https://id.snapmaker.cn/" : "https://id.snapmaker.com/";
}

const char* outcome_name(Outcome o)
{
    switch (o) {
    case Outcome::SignedIn:  return "signed in";
    case Outcome::NoSession: return "no session";
    case Outcome::Rejected:  return "session rejected";
    case Outcome::TimedOut:  return "timed out";
    case Outcome::Failed:    return "failed";
    case Outcome::Cancelled: return "cancelled";
    case Outcome::Skipped:   return "skipped";
    }
    return "unknown";
}

SessionMarker session_marker_after(Outcome o)
{
    switch (o) {
    case Outcome::SignedIn:  return SessionMarker::Set;
    case Outcome::NoSession:
    case Outcome::Rejected:  return SessionMarker::Clear;
    default:                 return SessionMarker::Keep;
    }
}

std::string log_line(Outcome o, const std::string& detail)
{
    std::string line = std::string("Snapmaker silent login: ") + outcome_name(o);
    if (!detail.empty())
        line += " (" + detail + ")";
    return line;
}

} // namespace SMSilentLogin
} // namespace Slic3r
