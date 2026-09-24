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

// The token carried by a navigation address, or "" when there is none. Same rule the sign-in
// dialog has always used: the text after "token=" up to the next '?' or the end.
std::string token_from_url(const std::string& url);

enum class Outcome
{
    SignedIn,
    NoSession,
    TimedOut,
    Failed,    // page or account lookup error
    Cancelled, // the user opened the sign-in dialog (or signed out) meanwhile
    Skipped,
};

const char* outcome_name(Outcome o);

// "Snapmaker silent login: <outcome>" plus " (<detail>)" when detail is not empty. The caller never
// passes the token or a cookie as the detail.
std::string log_line(Outcome o, const std::string& detail = std::string());

} // namespace SMSilentLogin
} // namespace Slic3r

#endif // slic3r_SnapmakerSilentLogin_hpp_
