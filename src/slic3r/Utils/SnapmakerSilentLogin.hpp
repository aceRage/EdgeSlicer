#ifndef slic3r_SnapmakerSilentLogin_hpp_
#define slic3r_SnapmakerSilentLogin_hpp_

#include <string>

// Silent Snapmaker sign-in at startup (GUI_App::sm_start_silent_login, SMUserLogin's silent mode).
// The login state itself lives only in memory; what survives a restart is the id.snapmaker.com
// session cookie in the WebView2 profile. With a valid cookie id.snapmaker.com?from=orca redirects
// straight back with a token, so loading it once in a never-shown web view signs the user in again.
// These are the rules that attempt follows, kept free of wx so they can be tested on their own
// (tests/slic3rutils/snapmaker_silent_login_tests.cpp):
//   * whether to try at all (preference, instance kind, already signed in, no network, ...),
//   * when to give up (the page loaded and sat still = no session; nothing loaded in time = timed out),
//   * where the token is in the redirect address,
//   * the one warning-level log line that reports the outcome (never the token).
namespace Slic3r {
namespace SMSilentLogin {

// app_config key of Preferences > General "Sign in to my Snapmaker account automatically at startup".
inline constexpr const char* k_pref_key = "snapmaker_auto_login";

// app_config key that remembers whether this computer has a Snapmaker sign-in to come back to:
// true after a sign-in completed (by hand or silently), false after signing out, after a silent
// attempt found no session, and on every install that never signed in. The startup attempt only
// runs while it is true, so a person who never uses Snapmaker cloud gets no Snapmaker traffic at
// startup at all (privacy audit 2026-10, EdgeSlicerSite PR 4). An install from before this key
// existed reads it as false: someone who was signed in signs in once more by hand (with a valid
// web session the sign-in window closes on its own).
inline constexpr const char* k_session_key = "snapmaker_signed_in";

// Give up this long after the attempt started, whatever happened.
inline constexpr int k_overall_timeout_ms = 20000;
// After an id.snapmaker.com page finished loading, a valid session redirects on its own within a
// moment. A page that sits this long without a redirect is the sign-in form: there is no session.
inline constexpr int k_settle_ms = 8000;
// How often the watchdog looks.
inline constexpr int k_tick_ms = 500;

struct StartupInputs
{
    bool pref_enabled       = true;  // Preferences toggle (default on)
    bool signed_in_before   = true;  // k_session_key: a sign-in on this computer to come back to
    bool is_editor          = true;  // false for the G-code viewer
    bool hidden_instance    = false; // hub-managed / --hidden / phone-upload instance: nobody in front of it
    bool main_window_ready  = true;  // mainframe exists and the app is not closing
    bool already_signed_in  = false; // SMUserInfo already says online
    bool login_dialog_open  = false; // the user already has the sign-in dialog up
    bool network_down       = false; // the OS reports no network connection at all
};

// "" when the attempt should run; otherwise the reason it is skipped, for the log line.
std::string skip_reason(const StartupInputs& in);

enum class Tick
{
    Wait,      // keep waiting
    NoSession, // the sign-in page loaded and stayed: signed out
    TimedOut,  // nothing usable loaded in time
};

// What the watchdog does now.
//   elapsed_ms        time since the attempt started
//   page_loaded       an id.snapmaker.com page has finished loading at least once
//   since_load_ms     time since the most recent such page finished loading (ignored if !page_loaded)
Tick on_tick(int elapsed_ms, bool page_loaded, int since_load_ms);

// Whether a navigation address carries a "token" parameter (in the query or the fragment). Only
// the parameter named exactly "token": access_token=, refresh_token= and the like do not count.
bool has_token_param(const std::string& url);

// The value of that "token" parameter, percent-decoded, or "" when there is none. The value ends
// at the next '&', '#' or '?'. id.snapmaker.com redirects to <redirect>?token=<T>&_act=login_success
// (URLSearchParams.append then set("_act")); up to 2.4.5.2 the value ran on to the next '?' and
// so carried "&_act=login_success", which the account service rejects (Snapmaker upstream #741).
std::string token_from_url(const std::string& url);

// The answer of GET /api/common/accounts/current, read. Snapmaker answers HTTP 200 even for a
// token it does not accept - {"code":110002,"msg":"Missing authorization"} - so the status alone
// says nothing: only an answer with an account id is a verified session.
struct AccountAnswer
{
    bool        ok = false;
    std::string user_id;  // never empty when ok
    std::string nickname;
    std::string icon;
    std::string account;
    // When !ok: why, for the log line ("HTTP 502", "rejected: code 110002 (Missing authorization)",
    // "no account in the answer", ...). Never the token, never account details.
    std::string why;
    // When !ok: true when the service answered and refused the token (the session is not valid),
    // false for a transport or server problem that says nothing about the session.
    bool        rejected = false;
};
AccountAnswer read_account_answer(unsigned http_status, const std::string& body);

enum class Outcome
{
    SignedIn,
    NoSession,
    Rejected,  // the page handed over a token, but the account service did not accept it
    TimedOut,
    Failed,    // page or account lookup error
    Cancelled, // the user opened the sign-in dialog (or signed out) meanwhile
    Skipped,
};

const char* outcome_name(Outcome o);

// What an attempt's outcome does to k_session_key: a sign-in sets it, "no session" and "session
// rejected" clear it (the saved web session is gone or no good, so the next start must not ask
// Snapmaker again), and everything else (timed out, failed, cancelled, skipped) leaves it as it was.
enum class SessionMarker
{
    Set,
    Clear,
    Keep,
};
SessionMarker session_marker_after(Outcome o);

// "Snapmaker silent login: <outcome>" plus " (<detail>)" when detail is not empty. The caller never
// passes the token or a cookie as the detail.
std::string log_line(Outcome o, const std::string& detail = std::string());

} // namespace SMSilentLogin
} // namespace Slic3r

#endif // slic3r_SnapmakerSilentLogin_hpp_
