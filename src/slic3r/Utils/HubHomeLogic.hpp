#ifndef slic3r_HubHomeLogic_hpp_
#define slic3r_HubHomeLogic_hpp_

#include <string>

// The Home tab shows the phone hub page (resources/web/orca/stream_center.html, served by the
// separate --hub process at /r/<token>/). These are the rules that view follows, kept free of wx
// so they can be tested on their own (tests/slic3rutils/hub_home_tests.cpp):
//   * which address it loads (always built from the hub's own /hub/info answer, never a fixed port),
//   * where it may navigate (the hub page only; http(s) links elsewhere go to the system browser),
//   * what a re-check of the hub means for the page on screen (port moved, new link, hub gone),
//   * how the page is told the slicer's theme (?theme= on load, window.__edgeTheme() live).
namespace Slic3r {
namespace HubHome {

// The hub's own token rule (RemoteHub.cpp valid_token): 10-32 characters of [a-z0-9].
bool valid_token(const std::string& token);

// "dark" or "light": the value of the page's ?theme= parameter and of __edgeTheme()'s argument.
const char* theme_name(bool dark);

// http://127.0.0.1:<port>/r/<token>/?embed=1&theme=<dark|light>, or "" when the port or the token
// is unusable (hub not up yet, a token that is not the hub's shape). Nothing but a checked port and
// a checked token is ever put into the address.
std::string hub_page_url(int port, const std::string& token, bool dark);

// true for an address under the hub page this view loaded: http://127.0.0.1:<port>/r/<token>/...
// (query and fragment allowed). Any other origin, port, token or path is false.
bool is_hub_page_url(const std::string& url, int port, const std::string& token);

// What the view may navigate to at all: the hub page (above), plus about:blank and data: for the
// view's own blank state.
bool is_allowed_navigation(const std::string& url, int port, const std::string& token);

// Only plain http(s) addresses are ever handed to the system browser; any other scheme is dropped.
bool is_external_browser_url(const std::string& url);

// What a fresh look at the hub (/hub/info) means for the page that is on screen.
enum class Recheck {
    Keep,    // same hub, same port, same link: leave the page alone (no reload, streams keep running)
    Reload,  // the hub is up but on another port (phone on/off rebinds, fallback port) or with a
             // new link ("New link", a restarted hub): load the new address
    HubDown  // no hub answers: show the "not running" state with a Start button
};
Recheck recheck(bool hub_alive, int hub_port, const std::string& hub_token, int loaded_port,
                const std::string& loaded_token);

// The script that switches an already loaded page to the slicer's theme without a reload.
std::string theme_script(bool dark);

} // namespace HubHome
} // namespace Slic3r

#endif
