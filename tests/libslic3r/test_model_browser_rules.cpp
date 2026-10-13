#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ModelBrowserRules.hpp"
#include "libslic3r/UntrustedInput.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <regex>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::modelbrowser;

static const std::string PRINTABLES_PAGE = "https://www.printables.com/model/123-benchy";
static const std::string MAKERWORLD_PAGE = "https://makerworld.com/en/models/456";
static const std::string SNAPMAKER_PAGE  = "https://space.snapmaker.com/en/model/789";

TEST_CASE("Model browser: sites are text-only shortcuts on https", "[ModelBrowser]")
{
    REQUIRE(sites().size() == 3);
    for (const Site &s : sites()) {
        INFO(s.id);
        CHECK(s.start_url.rfind("https://", 0) == 0);
        CHECK(site_for_url(s.start_url) == &s);
        CHECK(site_by_id(s.id) == &s);
        CHECK_FALSE(s.name.empty());
    }

    CHECK(site_for_url(PRINTABLES_PAGE) == site_by_id("printables"));
    CHECK(site_for_url("https://makerworld.com.cn/zh/models/1") == site_by_id("makerworld"));
    CHECK(site_for_url(SNAPMAKER_PAGE) == site_by_id("snapmaker"));
    // Look-alikes and other parts of the vendors' domains are not the sites.
    CHECK(site_for_url("https://printables.com.evil.tld/") == nullptr);
    CHECK(site_for_url("https://evilprintables.com/") == nullptr);
    CHECK(site_for_url("https://www.snapmaker.com/") == nullptr);
    CHECK(site_for_url("https://printables.com@evil.tld/") == site_for_url("https://evil.tld/"));
    CHECK(site_for_url("not a url") == nullptr);
}

TEST_CASE("Model browser: this computer and the local network are blocked", "[ModelBrowser]")
{
    const std::vector<std::string> blocked = {
        "localhost", "LOCALHOST", "localhost.", "app.localhost", "127.0.0.1", "127.1", "2130706433", "0x7f000001", "0x7f.1",
        "0.0.0.0", "10.1.2.3", "172.16.0.1", "192.168.1.20", "169.254.1.1", "100.100.100.100", "[::1]", "[::]",
        "[::ffff:127.0.0.1]", "[fe80::1]", "[fd7a:115c:a1e0::1]", "printer", "nas.local", "router.lan", "box.home.arpa",
        "svc.internal", "machine.tail1234.ts.net", "8.8.8.8", "",
    };
    for (const std::string &h : blocked) {
        INFO(h);
        CHECK(is_blocked_host(h));
    }
    for (const std::string &h : {"www.printables.com", "makerworld.com", "space.snapmaker.com", "accounts.google.com",
                                 "public-cdn.bblmw.com", "localhost.example.com"}) {
        INFO(h);
        CHECK_FALSE(is_blocked_host(h));
    }

    // The page server (127.0.0.1:13619+) and the phone hub (13640-13659, LAN or Tailscale) as
    // navigations, in every spelling.
    const std::vector<std::string> urls = {
        "http://127.0.0.1:13619/web/home/index.html", "http://localhost:13640/r/abc/", "http://LocalHost:13640/",
        "http://[::1]:13619/", "http://0x7f.0.0.1:13619/", "http://2130706433:13619/", "http://127.1:13640/",
        "http://192.168.1.5:13640/r/token/", "https://pc.tail1234.ts.net/r/token/", "http://10.0.0.2/",
        "https://printables.com@127.0.0.1/", "http://user:pw@www.printables.com/", "http://0.0.0.0:13619/",
    };
    for (const std::string &u : urls) {
        INFO(u);
        CHECK(decide_navigation(u, PRINTABLES_PAGE, true).action == NavAction::Block);
        CHECK(decide_navigation(u, PRINTABLES_PAGE, false).action == NavAction::Block);
    }
}

TEST_CASE("Model browser: navigation rules", "[ModelBrowser]")
{
    CHECK(decide_navigation("https://www.printables.com/model/1", "").action == NavAction::Allow);
    CHECK(decide_navigation("https://accounts.google.com/o/oauth2/auth?x=1", PRINTABLES_PAGE).action == NavAction::Allow);
    CHECK(decide_navigation("http://example.com/", "").action == NavAction::Allow);
    CHECK(decide_navigation("about:blank", "").action == NavAction::Allow);
    CHECK(decide_navigation("about:srcdoc", "", false).action == NavAction::Allow);
    CHECK(decide_navigation("about:settings", "").action == NavAction::Block);

    // blob: follows its origin.
    CHECK(decide_navigation("blob:https://makerworld.com/2b7c-11", MAKERWORLD_PAGE).action == NavAction::Allow);
    CHECK(decide_navigation("blob:http://127.0.0.1:13619/2b7c", MAKERWORLD_PAGE).action == NavAction::Block);
    CHECK(decide_navigation("blob:null/2b7c", MAKERWORLD_PAGE).action == NavAction::Block);

    // data: never as a page, fine inside a frame.
    CHECK(decide_navigation("data:text/html,<p>x</p>", "", true).action == NavAction::Block);
    CHECK(decide_navigation("data:text/html,<p>x</p>", "", false).action == NavAction::Allow);

    for (const std::string &u : {"file:///C:/Windows/win.ini", "FILE://server/share/x.3mf", "javascript:alert(1)", "view-source:https://x.com/",
                                 "edge://settings", "chrome://version", "ftp://ftp.example.com/x.stl", "ms-settings:privacy",
                                 "search-ms:query=x", "mailto:someone@example.com", "steam://run/1", "ms-msdt:/id", "tel:123",
                                 "", "no-scheme-here", ":nothing"}) {
        INFO(u);
        CHECK(decide_navigation(u, PRINTABLES_PAGE).action == NavAction::Block);
    }
    // Malformed web addresses.
    for (const std::string &u : {"https://", "https:///path", "https://exa mple.com/", "https://example.com:99999/", "https://ex\\ample.com/"}) {
        INFO(u);
        CHECK(decide_navigation(u, "").action == NavAction::Block);
    }
}

TEST_CASE("Model browser: Printables \"Open in\" links go to the app's downloader", "[ModelBrowser]")
{
    const std::string file   = "https://files.printables.com/media/prints/1/stls/2_benchy.stl";
    const std::string file3m = "https://files.printables.com/media/prints/1/3mf/2_benchy.3mf";
    const std::string enc    = percent_encode(file);

    for (const std::string &scheme : {"prusaslicer", "orcaslicer", "bambustudio", "edgeslicer", "PrusaSlicer"}) {
        const std::string link = scheme + "://open?file=" + enc;
        INFO(link);
        const NavDecision d = decide_navigation(link, PRINTABLES_PAGE);
        REQUIRE(d.action == NavAction::OpenLink);
        // Printables' own schemes are re-wrapped as our scheme with the same file (no widening);
        // Printables' "Open in Bambu Studio" link goes as it is, the path the system browser gives it.
        const untrusted::OpenLink parsed = untrusted::parse_open_link(d.open_link);
        REQUIRE(parsed.ok);
        if (scheme == "bambustudio")
            CHECK(d.open_link == link);
        else
            CHECK(parsed.scheme == "edgeslicer");
        CHECK(parsed.file_url == file);
        CHECK(untrusted::check_model_download(parsed.file_url, parsed.scheme).verdict == untrusted::DownloadVerdict::Allow);
    }
    // "open/?file=" and a display name.
    {
        const NavDecision d = decide_app_link("prusaslicer://open/?file=" + percent_encode(file3m) + "&name=" + percent_encode("My Benchy.3mf"),
                                              PRINTABLES_PAGE);
        REQUIRE(d.action == NavAction::OpenLink);
        const untrusted::OpenLink parsed = untrusted::parse_open_link(d.open_link);
        REQUIRE(parsed.ok);
        CHECK(parsed.file_url == file3m);
        CHECK(parsed.name == "My Benchy.3mf");
    }
    // Unknown public hosts still go to the downloader, which asks the user.
    CHECK(decide_app_link("orcaslicer://open?file=" + percent_encode("https://cdn.example.org/a.stl"), PRINTABLES_PAGE).action == NavAction::OpenLink);

    // File URLs the downloader would refuse are blocked here already.
    for (const std::string &f : {"http://files.printables.com/a.stl", "https://127.0.0.1:13619/a.stl", "https://192.168.1.2/a.3mf",
                                 "file:///C:/a.stl", "https://files.printables.com/a.exe", "https://user@files.printables.com/a.stl"}) {
        INFO(f);
        CHECK(decide_app_link("prusaslicer://open?file=" + percent_encode(f), PRINTABLES_PAGE).action == NavAction::Block);
    }
    // Malformed or unsupported link actions.
    CHECK(decide_app_link("prusaslicer://open", PRINTABLES_PAGE).action == NavAction::Block);
    CHECK(decide_app_link("prusaslicer://delete?file=x", PRINTABLES_PAGE).action == NavAction::Block);
    CHECK(decide_app_link("orcaslicer://open?file=", PRINTABLES_PAGE).action == NavAction::Block);
    // Generic cloud storage is only MakerWorld's (bambustudio schemes); re-wrapped links get no
    // widening, so the downloader asks.
    {
        const NavDecision d = decide_app_link("orcaslicer://open?file=" + percent_encode("https://bucket.s3.amazonaws.com/a.3mf"), PRINTABLES_PAGE);
        REQUIRE(d.action == NavAction::OpenLink);
        const untrusted::OpenLink parsed = untrusted::parse_open_link(d.open_link);
        REQUIRE(parsed.ok);
        CHECK(untrusted::check_model_download(parsed.file_url, parsed.scheme).verdict == untrusted::DownloadVerdict::Ask);
    }
}

TEST_CASE("Model browser: MakerWorld's \"Open in\" button takes the system-browser path", "[ModelBrowser]")
{
    const std::string signed_url = "https://public-cdn.bblmw.com/makerworld/model/x.3mf?Signature=abc";
    const std::string s3_url     = "https://makerworld-bucket.s3.us-west-2.amazonaws.com/x.3mf?X-Amz-Signature=abc";
    // bambustudioopen://<url> and bambustudio://open?file=<url>: handed to the downloader unchanged,
    // exactly as the system browser hands them over, wherever the page is.
    for (const std::string &file : {signed_url, s3_url}) {
        for (const std::string &link : {"bambustudioopen://" + percent_encode(file), "bambustudio://open?file=" + percent_encode(file),
                                         "BambuStudioOpen://" + percent_encode(file)}) {
            for (const std::string &page : {MAKERWORLD_PAGE, std::string("https://makerworld.com.cn/zh/models/1"), PRINTABLES_PAGE}) {
                INFO(link << " from " << page);
                const NavDecision d = decide_navigation(link, page);
                REQUIRE(d.action == NavAction::OpenLink);
                CHECK(d.open_link == link);
                // The downloader's own check of that link (with the scheme's MakerWorld storage
                // allowlist) is what allows it; nothing is trusted here that it would not trust.
                const untrusted::OpenLink parsed = untrusted::parse_open_link(d.open_link);
                REQUIRE(parsed.ok);
                CHECK(parsed.file_url == file);
                CHECK(untrusted::check_model_download(parsed.file_url, parsed.scheme).verdict == untrusted::DownloadVerdict::Allow);
                CHECK(untrusted::is_makerworld_file_url(parsed.file_url) == (file == signed_url));
            }
        }
    }
    // Hosts the downloader would ask about are passed on (it asks); refused ones are blocked here,
    // and the panel shows why.
    CHECK(decide_navigation("bambustudioopen://" + percent_encode("https://cdn.example.org/a.3mf"), MAKERWORLD_PAGE).action == NavAction::OpenLink);
    for (const std::string &f : {"http://public-cdn.bblmw.com/x.3mf", "https://127.0.0.1:13619/x.3mf", "https://192.168.1.9/x.3mf",
                                 "https://public-cdn.bblmw.com/x.exe", "file:///C:/x.3mf", "https://user@public-cdn.bblmw.com/x.3mf"}) {
        INFO(f);
        const NavDecision d = decide_navigation("bambustudioopen://" + percent_encode(f), MAKERWORLD_PAGE);
        CHECK(d.action == NavAction::Block);
        CHECK_FALSE(d.reason.empty());
    }
    CHECK(decide_navigation("bambustudioopen://x", MAKERWORLD_PAGE).action == NavAction::Block);
    CHECK(decide_navigation("bambustudioopen://", MAKERWORLD_PAGE).action == NavAction::Block);
    // A new window to the button's link is carried out like the link itself.
    const WindowDecision w = decide_new_window("bambustudioopen://" + percent_encode(signed_url), MAKERWORLD_PAGE, true, false);
    CHECK(w.action == WindowAction::AppLink);
    CHECK(w.nav.action == NavAction::OpenLink);
    // Plain downloads from MakerWorld are imported too.
    const DownloadDecision d = decide_download(signed_url, "Benchy.3mf", MAKERWORLD_PAGE, 1024);
    CHECK(d.action == DownloadAction::Import);
    CHECK(d.file_name == "Benchy.3mf");
}

TEST_CASE("Model browser: new windows", "[ModelBrowser]")
{
    // OAuth popups stay in the isolated browser (window.opener kept).
    CHECK(decide_new_window("https://accounts.google.com/o/oauth2/v2/auth?client_id=1", PRINTABLES_PAGE, true, true).action == WindowAction::Popup);
    CHECK(decide_new_window("https://accounts.google.com/o/oauth2/v2/auth?client_id=1", PRINTABLES_PAGE, false, false).action == WindowAction::Popup);
    CHECK(decide_new_window("https://appleid.apple.com/auth/authorize", MAKERWORLD_PAGE, true, false).action == WindowAction::Popup);
    CHECK(decide_new_window("https://bambulab.com/en/sign-in", MAKERWORLD_PAGE, true, false).action == WindowAction::Popup);
    CHECK(decide_new_window("https://id.snapmaker.com/login", SNAPMAKER_PAGE, true, false).action == WindowAction::Popup);
    CHECK(decide_new_window("about:blank", PRINTABLES_PAGE, true, true).action == WindowAction::Popup);
    CHECK(decide_new_window("", PRINTABLES_PAGE, true, true).action == WindowAction::Popup);
    // The popup blocker: blank or sized popups nobody clicked for.
    CHECK(decide_new_window("about:blank", PRINTABLES_PAGE, false, true).action == WindowAction::Block);
    CHECK(decide_new_window("https://ads.example.com/", PRINTABLES_PAGE, false, true).action == WindowAction::Block);
    CHECK(decide_new_window("https://ads.example.com/", PRINTABLES_PAGE, false, false).action == WindowAction::Block);
    // A sized window.open() from a click is a popup.
    CHECK(decide_new_window("https://share.example.com/", PRINTABLES_PAGE, true, true).action == WindowAction::Popup);
    // target=_blank to one of our sites: in place. Elsewhere: the user's own browser.
    CHECK(decide_new_window("https://www.printables.com/model/2", PRINTABLES_PAGE, true, false).action == WindowAction::NavigateInPlace);
    CHECK(decide_new_window("https://makerworld.com/en", PRINTABLES_PAGE, true, false).action == WindowAction::NavigateInPlace);
    CHECK(decide_new_window("https://www.youtube.com/watch?v=1", PRINTABLES_PAGE, true, false).action == WindowAction::SystemBrowser);
    // Never: local targets, files, script URLs.
    for (const std::string &t : {"http://127.0.0.1:13619/", "http://localhost:13640/r/x/", "file:///C:/x", "javascript:alert(1)",
                                 "http://192.168.0.10/"}) {
        INFO(t);
        CHECK(decide_new_window(t, PRINTABLES_PAGE, true, true).action == WindowAction::Block);
    }
    // A new window to an "Open in" link is carried out like the link itself.
    const WindowDecision w = decide_new_window("prusaslicer://open?file=" + percent_encode("https://files.printables.com/a.stl"), PRINTABLES_PAGE,
                                               true, false);
    CHECK(w.action == WindowAction::AppLink);
    CHECK(w.nav.action == NavAction::OpenLink);
    CHECK(decide_new_window("bambustudioopen://x", MAKERWORLD_PAGE, true, false).action == WindowAction::Block);
}

TEST_CASE("Model browser: download routing", "[ModelBrowser]")
{
    SECTION("model files from the sites are imported")
    {
        for (const std::string &n : {"benchy.stl", "Benchy.3MF", "part.step", "part.stp", "thing.obj", "bundle.zip", "plate.gcode.3mf"}) {
            INFO(n);
            const DownloadDecision d = decide_download("https://files.printables.com/media/x/" + n, n, PRINTABLES_PAGE, 2048);
            CHECK(d.action == DownloadAction::Import);
            CHECK(d.file_name == n);
        }
        // Snapmaker Space through its resource host; a blob the page assembled.
        CHECK(decide_download("https://public.resource.snapmaker.com/m/a.3mf", "a.3mf", SNAPMAKER_PAGE).action == DownloadAction::Import);
        CHECK(decide_download("blob:https://space.snapmaker.com/1234-5678", "a.3mf", SNAPMAKER_PAGE).action == DownloadAction::Import);
        // A known file host is enough, whatever page started it.
        CHECK(decide_download("https://files.printables.com/a.stl", "a.stl", "https://www.example.com/").action == DownloadAction::Import);
    }
    SECTION("model files from other sites need a confirmation")
    {
        const DownloadDecision d = decide_download("https://cdn.example.org/a.stl", "a.stl", "https://www.example.org/thing");
        CHECK(d.action == DownloadAction::ImportAfterConfirm);
        CHECK(decide_download("data:application/octet-stream;base64,AAAA", "a.stl", "https://www.example.org/").action ==
              DownloadAction::ImportAfterConfirm);
    }
    SECTION("names are sanitized: last component only, no device names, no separators")
    {
        CHECK(decide_download("https://files.printables.com/x", "C:\\Users\\me\\Downloads\\benchy.stl", PRINTABLES_PAGE).file_name == "benchy.stl");
        CHECK(decide_download("https://files.printables.com/x", "../../evil.3mf", PRINTABLES_PAGE).file_name == "evil.3mf");
        const std::string con = decide_download("https://files.printables.com/x", "CON.stl", PRINTABLES_PAGE).file_name;
        CHECK(con != "CON.stl");
        CHECK(con.find_first_of("/\\") == std::string::npos);
        // No suggested name: the URL path's last segment.
        CHECK(decide_download("https://files.printables.com/media/a%20b.stl", "", PRINTABLES_PAGE).file_name == "a b.stl");
        CHECK(decide_download("https://files.printables.com/", "", PRINTABLES_PAGE).file_name == "download");
    }
    SECTION("programs and scripts are refused, other files offered as a save")
    {
        for (const std::string &n : {"setup.exe", "run.BAT", "x.ps1", "x.js", "x.lnk", "x.msi", "x.scr", "x.hta", "x.vbs", "x.reg", "a.stl.exe"}) {
            INFO(n);
            CHECK(decide_download("https://files.printables.com/" + n, n, PRINTABLES_PAGE).action == DownloadAction::Refuse);
        }
        CHECK(decide_download("https://files.printables.com/guide.pdf", "guide.pdf", PRINTABLES_PAGE).action == DownloadAction::OfferSave);
        CHECK(decide_download("https://files.printables.com/photo.jpg", "photo.jpg", PRINTABLES_PAGE).action == DownloadAction::OfferSave);
        CHECK(decide_download("https://files.printables.com/plate.gcode", "plate.gcode", PRINTABLES_PAGE).action == DownloadAction::OfferSave);
    }
    SECTION("never from this computer, the LAN or odd schemes; never oversized models")
    {
        CHECK(decide_download("http://127.0.0.1:13619/localfile/a.3mf", "a.3mf", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        CHECK(decide_download("http://192.168.1.4/a.stl", "a.stl", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        CHECK(decide_download("file:///C:/a.stl", "a.stl", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        CHECK(decide_download("ftp://example.com/a.stl", "a.stl", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        CHECK(decide_download("blob:http://localhost:13640/1", "a.stl", PRINTABLES_PAGE).action == DownloadAction::Refuse);
        const long long too_big = (long long)untrusted::MODEL_DOWNLOAD_SIZE_LIMIT + 1;
        CHECK(decide_download("https://files.printables.com/a.stl", "a.stl", PRINTABLES_PAGE, too_big).action == DownloadAction::Refuse);
        CHECK(decide_download("https://files.printables.com/a.stl", "a.stl", PRINTABLES_PAGE, -1).action == DownloadAction::Import);
    }
}

TEST_CASE("Model browser: the app bridge never serves a page the browser can show", "[ModelBrowser]")
{
    // The app's script bridge (SSWCP / handle_web_request) only answers our own pages: the local
    // page server or files under resources/web (GUI_App::is_own_page_url). Every address the model
    // browser is allowed to load must fail that test, so even a bridge attached by mistake would
    // refuse it. (The browser itself is created with web messages and host objects off, which the
    // panel asserts at runtime.)
    const std::vector<std::string> candidates = {
        "https://www.printables.com/", "https://makerworld.com/en", "https://space.snapmaker.com/",
        "http://127.0.0.1:13619/web/home/index.html", "http://localhost:13619/", "http://localhost:13640/r/x/",
        "file:///C:/Program%20Files/EdgeSlicer/resources/web/home/index.html", "http://[::1]:13619/", "about:blank",
        "blob:http://127.0.0.1:13619/1", "https://accounts.google.com/", "http://example.com/",
    };
    int allowed = 0;
    for (const std::string &u : candidates) {
        const NavDecision d = decide_navigation(u, "", true);
        if (d.action != NavAction::Allow)
            continue;
        ++allowed;
        INFO(u);
        for (int port = 13600; port <= 13700; ++port)
            CHECK_FALSE(untrusted::is_page_server_url(u, port));
        CHECK(untrusted::local_path_from_file_url(u).empty());
    }
    CHECK(allowed == 6); // the three sites, about:blank, Google, example.com
}

TEST_CASE("Model browser: honest User-Agent", "[ModelBrowser]")
{
    const std::string edge = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0.0.0 Safari/537.36 Edg/129.0.0.0";
    CHECK(browser_user_agent(edge, "2.4.2.0") == edge + " EdgeSlicer/2.4.2.0");
    // Anything that would make MakerWorld take us for Bambu Studio goes.
    const std::string bbl = edge + " BBL-Slicer/v1.10.0.0 BBL-Language/en BambuStudio/1.10 OrcaSlicer/2.0 dark";
    const std::string out = browser_user_agent(bbl, "2.4.2.0");
    CHECK(out.find("BBL") == std::string::npos);
    CHECK(out.find("Bambu") == std::string::npos);
    CHECK(out.find("Orca") == std::string::npos);
    CHECK(out.find(" EdgeSlicer/2.4.2.0") != std::string::npos);
    // A version with odd characters cannot inject anything.
    CHECK(browser_user_agent("UA", "1.0\r\nX: y") == "UA EdgeSlicer/1.0Xy");
    // Never two EdgeSlicer tokens.
    CHECK(browser_user_agent("UA EdgeSlicer/1.0", "2.0") == "UA EdgeSlicer/2.0");
}

TEST_CASE("Model browser: percent-encoding round trip", "[ModelBrowser]")
{
    const std::string s = "https://files.printables.com/a b&c=d/%41.stl?x=1#y";
    CHECK(untrusted::percent_decode(percent_encode(s)) == s);
    CHECK(percent_encode("aZ09-._~") == "aZ09-._~");
    CHECK(percent_encode("&") == "%26");
    CHECK(make_open_link("https://x.com/a.stl") == "edgeslicer://open?file=https%3A%2F%2Fx.com%2Fa.stl");
}

// ---- macOS (WKWebView) ----------------------------------------------------------------------------

namespace {

// The url-filters of content_rule_list_json(), read back from the JSON WebKit compiles.
std::vector<std::string> content_rule_filters_from_json()
{
    const nlohmann::json rules = nlohmann::json::parse(content_rule_list_json());
    std::vector<std::string> out;
    REQUIRE(rules.is_array());
    for (const nlohmann::json &rule : rules) {
        REQUIRE(rule.is_object());
        REQUIRE(rule.size() == 2);
        REQUIRE(rule.at("action") == nlohmann::json({{"type", "block"}}));
        const nlohmann::json &trigger = rule.at("trigger");
        REQUIRE(trigger.is_object());
        // No if-domain / load-type / resource-type: a rule applies to every load of every page.
        REQUIRE(trigger.size() == 1);
        out.push_back(trigger.at("url-filter").get<std::string>());
    }
    return out;
}

// What WebKit does with the rules: a case-insensitive regex search on the canonical URL.
bool content_rules_block(const std::vector<std::regex> &rules, const std::string &url)
{
    for (const std::regex &r : rules)
        if (std::regex_search(url, r))
            return true;
    return false;
}

} // namespace

TEST_CASE("Model browser (macOS): the content rules are in WebKit's regex subset", "[ModelBrowser]")
{
    const std::vector<std::string> filters = content_rule_filters_from_json();
    CHECK(filters == content_rule_url_filters());
    REQUIRE(filters.size() >= 12);
    for (const std::string &f : filters) {
        INFO(f);
        CHECK_FALSE(f.empty());
        // WebKit content blockers: no disjunction, no counted repetition, no class escapes or
        // back-references, no lookaround; "^" / "$" only as the first / last character.
        CHECK(f.find('|') == std::string::npos);
        CHECK(f.find('{') == std::string::npos);
        CHECK(f.find("(?") == std::string::npos);
        for (const char *esc : {"\\d", "\\w", "\\s", "\\b", "\\D", "\\W", "\\S", "\\B", "\\1"})
            CHECK(f.find(esc) == std::string::npos);
        CHECK(f.front() == '^');
        size_t pos = 1;
        while ((pos = f.find('^', pos)) != std::string::npos) {
            CHECK(f[pos - 1] == '[');
            ++pos;
        }
        CHECK(f.find('$') == std::string::npos);
        for (unsigned char c : f)
            CHECK((c >= 0x20 && c < 0x7f));
        // A valid ECMAScript regex (what the test below runs).
        CHECK_NOTHROW(std::regex(f, std::regex::ECMAScript | std::regex::icase));
    }
}

TEST_CASE("Model browser (macOS): the content rules block what is_blocked_host blocks", "[ModelBrowser]")
{
    std::vector<std::regex> rules;
    for (const std::string &f : content_rule_filters_from_json())
        rules.emplace_back(f, std::regex::ECMAScript | std::regex::icase);

    // Hosts as WebKit writes them in the URL it matches (canonical form: lower case, IPv4 in dotted
    // decimal - 127.1, 0x7f.1, 2130706433 and 0x7f000001 all reach the rules as 127.0.0.1 - and
    // IPv6 compressed in brackets). Decimal shorthand is blocked in its raw form too.
    const std::vector<std::string> hosts = {
        // blocked
        "localhost", "localhost.", "localhost..", "app.localhost", "a.b.localhost", "127.0.0.1", "127.1", "2130706433", "0.0.0.0",
        "10.1.2.3", "172.16.0.1", "192.168.1.20", "169.254.1.1", "100.100.100.100", "8.8.8.8", "[::1]", "[::]",
        "[::ffff:7f00:1]", "[fe80::1]", "[fd7a:115c:a1e0::1]", "printer", "printer.", "nas", "router", "nas.local", "nas.local.",
        "local", "router.lan", "box.home.arpa", "home.arpa", "svc.internal", "wiki.intranet", "host.localdomain",
        "localhost.localdomain", "machine.tail1234.ts.net", "ts.net", "my-pc.tail-scale.ts.net",
        // allowed
        "www.printables.com", "printables.com", "files.printables.com", "makerworld.com", "makerworld.com.cn",
        "public-cdn.bblmw.com", "space.snapmaker.com", "accounts.google.com", "appleid.apple.com", "example.com",
        "localhost.example.com", "local.example.com", "lan.example.com", "internal.example.org", "home.arpa.example.com",
        "my-local.com", "locallan.com", "xlocal.com", "intranet-news.com", "ts.network.example", "nets.ts.network",
        "1.2.3.4.example.com", "10.example.com", "example.com.", "a-b.c-d.co.uk", "xn--bcher-kva.example",
    };
    int blocked = 0, allowed = 0;
    for (const std::string &h : hosts) {
        const bool expected = is_blocked_host(h);
        expected ? ++blocked : ++allowed;
        // Every place a host shows up in a canonical URL: plain, with a port, a path and a query,
        // WebSocket schemes, with a user name and password.
        for (const std::string &url : {"https://" + h + "/", "http://" + h + ":13640/r/token/", "https://" + h + "/a/b.3mf?x=1#y",
                                       "wss://" + h + "/socket", "ws://" + h + ":8080/", "http://user:pw@" + h + "/",
                                       "https://user@" + h + ":443/x", "http://" + h + ":1/"}) {
            INFO(url);
            CHECK(content_rules_block(rules, url) == expected);
        }
    }
    CHECK(blocked == 37);
    CHECK(allowed == 26);

    // A blocked host anywhere but the authority does not block the page.
    for (const std::string &url : {"https://www.printables.com/x.local/", "https://www.printables.com/?u=http://127.0.0.1/",
                                   "https://www.printables.com/@127.0.0.1/", "https://www.printables.com/#http://localhost/",
                                   "https://www.printables.com/r/http://[::1]/", "https://makerworld.com/en/models/1-localhost",
                                   "https://www.printables.com/user:pw@printer/"}) {
        INFO(url);
        CHECK_FALSE(content_rules_block(rules, url));
    }
    // A user name that looks like a host does not hide a blocked host, and does not block a site.
    CHECK(content_rules_block(rules, "http://www.printables.com@127.0.0.1/"));
    CHECK(content_rules_block(rules, "http://www.printables.com:443@localhost:13619/"));
    CHECK_FALSE(content_rules_block(rules, "https://printer@www.printables.com/"));
    CHECK_FALSE(content_rules_block(rules, "https://printer:1234@www.printables.com/"));
    // Local files.
    for (const std::string &url : {"file:///Users/me/Downloads/a.stl", "FILE:///etc/hosts", "file://localhost/Users/me/a.3mf"}) {
        INFO(url);
        CHECK(content_rules_block(rules, url));
    }
    // blob: and data: carry no host to reach (a blob is in-memory content of its own origin).
    CHECK_FALSE(content_rules_block(rules, "blob:https://www.printables.com/2b7c-11"));
    CHECK_FALSE(content_rules_block(rules, "data:image/png;base64,AAAA"));
    CHECK_FALSE(content_rules_block(rules, "about:blank"));
}

TEST_CASE("Model browser (macOS): links to other programs never start one", "[ModelBrowser]")
{
    // decidePolicyForNavigationAction sees every navigation, main frame and frames alike, with any
    // scheme; nothing but a web page (or an in-page blob/about/data frame) is ever allowed, and the
    // "Open in" schemes become a link for the app's downloader.
    const std::string file = "https://files.printables.com/media/prints/1/stls/2_benchy.stl";
    const std::string mw   = "https://public-cdn.bblmw.com/makerworld/model/x.3mf?Signature=abc";
    const std::vector<std::pair<std::string, std::string>> open_links = {
        {"prusaslicer://open?file=" + percent_encode(file), "edgeslicer"},
        {"orcaslicer://open?file=" + percent_encode(file), "edgeslicer"},
        {"bambustudioopen://" + percent_encode(mw), "bambustudioopen"},
        {"bambustudio://open?file=" + percent_encode(mw), "bambustudio"},
        {"edgeslicer://open?file=" + percent_encode(file), "edgeslicer"},
    };
    for (const auto &[link, scheme] : open_links)
        for (bool main_frame : {true, false}) {
            INFO(link << (main_frame ? " (main frame)" : " (frame)"));
            const NavDecision d = decide_navigation(link, MAKERWORLD_PAGE, main_frame);
            REQUIRE(d.action == NavAction::OpenLink);
            const untrusted::OpenLink parsed = untrusted::parse_open_link(d.open_link);
            REQUIRE(parsed.ok);
            CHECK(parsed.scheme == scheme);
        }

    // Everything else that would start a program on a Mac (or anywhere) is cancelled.
    for (const std::string &scheme : {"mailto", "tel", "sms", "facetime", "facetime-audio", "imessage", "x-apple-helpviewer", "applescript",
                                      "x-apple.systempreferences", "vnc", "smb", "afp", "nfs", "ssh", "telnet", "itms-services",
                                      "itms-apps", "macappstore", "shortcuts", "x-man-page", "dict", "news", "help", "ibooks", "maps",
                                      "music", "photos-redirect", "zoommtg", "slack", "steam", "vscode", "file", "javascript", "ftp",
                                      "intent", "ms-settings", "cura", "x-callback-url", "open"}) {
        for (const std::string &rest : {std::string("://open?file=") + percent_encode("https://cdn.example.org/a.exe"), std::string("://x"),
                                        std::string(":x")}) {
            const std::string url = scheme + rest;
            for (bool main_frame : {true, false}) {
                INFO(url << (main_frame ? " (main frame)" : " (frame)"));
                const NavDecision d = decide_navigation(url, PRINTABLES_PAGE, main_frame);
                CHECK(d.action == NavAction::Block);
                CHECK_FALSE(d.reason.empty());
            }
        }
    }
    // Only web schemes (and in-page about:/blob:/data: frames) are ever loaded.
    for (const std::string &url : {"https://www.printables.com/", "http://example.com/", "about:blank", "about:srcdoc",
                                   "blob:https://makerworld.com/2b7c", "data:text/html,x"}) {
        INFO(url);
        CHECK(decide_navigation(url, PRINTABLES_PAGE, false).action == NavAction::Allow);
    }
}

TEST_CASE("Model browser (macOS): WKDownload names and sizes are classified like WebView2's", "[ModelBrowser]")
{
    // WKDownloadDelegate hands over the response URL, WebKit's suggested file name (from
    // Content-Disposition or the URL, never a path) and expectedContentLength (-1 when unknown).
    CHECK(decide_download("https://files.printables.com/media/x/benchy.stl", "benchy.stl", PRINTABLES_PAGE, -1).action ==
          DownloadAction::Import);
    CHECK(decide_download("https://public-cdn.bblmw.com/x.3mf?Signature=a", "Benchy (1).3mf", MAKERWORLD_PAGE, 123456).file_name ==
          "Benchy (1).3mf");
    CHECK(decide_download("https://cdn.example.org/a.3mf", "a.3mf", "https://www.example.org/").action == DownloadAction::ImportAfterConfirm);
    // A name with a separator keeps only its last component.
    CHECK(decide_download("https://files.printables.com/x", "a/b/../c.stl", PRINTABLES_PAGE).file_name == "c.stl");
    // macOS programs, scripts, installers and launchers are refused...
    for (const std::string &n : {"Installer.app", "EdgeSlicer.dmg", "setup.pkg", "x.mpkg", "run.command", "x.sh", "x.zsh", "x.terminal",
                                 "x.tool", "x.workflow", "x.scpt", "x.applescript", "x.webloc", "x.fileloc", "x.inetloc",
                                 "x.mobileconfig", "x.prefpane", "x.kext", "x.dylib", "model.stl.command", "x.py", "x.pl", "x.rb",
                                 "SETUP.PKG"}) {
        INFO(n);
        CHECK(is_executable_file_name(n));
        CHECK(decide_download("https://files.printables.com/" + n, n, PRINTABLES_PAGE).action == DownloadAction::Refuse);
    }
    // ...model files and archives of them import, anything else is offered as a save.
    for (const std::string &n : {"a.stl", "a.3mf", "a.step", "a.obj", "a.zip"})
        CHECK(decide_download("https://files.printables.com/" + n, n, PRINTABLES_PAGE).action == DownloadAction::Import);
    for (const std::string &n : {"guide.pdf", "photo.heic", "notes.txt", "plate.gcode", "a.tar.gz"})
        CHECK(decide_download("https://files.printables.com/" + n, n, PRINTABLES_PAGE).action == DownloadAction::OfferSave);
}

// Home > Models is offered where model_browser_supported() is true (Windows, macOS 12+; never Linux)
// and Preferences > "Model browser (beta)" is on. The key's default lives in AppConfig::set_defaults(),
// which has no OS-version check, so it is on for Windows and macOS and absent elsewhere.
TEST_CASE("Model browser: model_browser_beta is on by default on Windows and macOS only", "[ModelBrowser][AppConfig]")
{
#if defined(_WIN32) || defined(__APPLE__)
    constexpr bool has_model_browser = true;
#else
    constexpr bool has_model_browser = false;
#endif

    SECTION("a fresh config")
    {
        AppConfig config;
        CHECK(config.get_bool("model_browser_beta") == has_model_browser);
        CHECK(config.get("model_browser_beta").empty() == !has_model_browser);
    }

    SECTION("reset() restores the default")
    {
        AppConfig config;
        config.set_bool("model_browser_beta", false);
        config.reset();
        CHECK(config.get_bool("model_browser_beta") == has_model_browser);
    }

    SECTION("an explicit false the user saved stays false")
    {
        AppConfig config;
        config.set_bool("model_browser_beta", false);
        config.set_defaults();
        CHECK(config.get("model_browser_beta") == "false");
        CHECK_FALSE(config.get_bool("model_browser_beta"));
    }

    SECTION("an explicit true stays true")
    {
        AppConfig config;
        config.set_bool("model_browser_beta", true);
        config.set_defaults();
        CHECK(config.get_bool("model_browser_beta"));
    }

    SECTION("a config file with an explicit false keeps it after load(); one without the key gets the default")
    {
        const boost::filesystem::path dir = boost::filesystem::temp_directory_path() /
                                            boost::filesystem::unique_path("model_browser_beta_%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(dir);
        const std::string off  = (dir / "off.conf").string();
        const std::string none = (dir / "none.conf").string();
        {
            boost::nowide::ofstream f(off, std::ios::binary);
            f << "{\"app\":{\"model_browser_beta\":\"false\"}}\n";
        }
        {
            boost::nowide::ofstream g(none, std::ios::binary);
            g << "{\"app\":{\"some_key\":\"some_value\"}}\n";
        }
        AppConfig a;
        a.set_loading_path(off);
        CHECK(a.load().empty());
        CHECK(a.get("model_browser_beta") == "false");
        CHECK_FALSE(a.get_bool("model_browser_beta"));

        AppConfig b;
        b.set_loading_path(none);
        CHECK(b.load().empty());
        CHECK(b.get_bool("model_browser_beta") == has_model_browser);

        boost::system::error_code ec;
        boost::filesystem::remove_all(dir, ec);
    }
}
