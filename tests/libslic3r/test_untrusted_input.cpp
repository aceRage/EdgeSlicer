#include <catch2/catch.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/UntrustedInput.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::untrusted;
namespace fs = boost::filesystem;

// Rules for input from outside the machine (UntrustedInput.hpp): "Open in" links, URLs a web page
// asks us to open, downloaded file names, archive entry names, and the settings a project brings.

// ---- URL parsing and host checks ----------------------------------------------------------------

TEST_CASE("parse_url takes an ordinary https URL apart", "[Untrusted][Url]")
{
    Url u;
    REQUIRE(parse_url("https://Files.Printables.com:443/media/prints/1/a.stl?x=1#frag", u));
    CHECK(u.scheme == "https");
    CHECK(u.host == "files.printables.com");
    CHECK(u.port == 443);
    CHECK(u.path == "/media/prints/1/a.stl");
    CHECK(u.query == "x=1");
    CHECK_FALSE(u.has_userinfo);
}

TEST_CASE("parse_url refuses what a spoofed or broken URL looks like", "[Untrusted][Url]")
{
    Url u;
    CHECK_FALSE(parse_url("", u));
    CHECK_FALSE(parse_url("printables.com/a.stl", u));
    CHECK_FALSE(parse_url("https:/printables.com/a.stl", u));
    CHECK_FALSE(parse_url("https://printables.com /a.stl", u));
    CHECK_FALSE(parse_url("https://printables.com\\@evil.tld/a.stl", u));
    CHECK_FALSE(parse_url("https://printables.com\t/a.stl", u));
    CHECK_FALSE(parse_url("https:///a.stl", u));
    CHECK_FALSE(parse_url("https://printables.com:99999/a", u));
    CHECK_FALSE(parse_url("https://printables.com:/a", u));
    CHECK_FALSE(parse_url("https://pr\xc3\xadntables.com/a", u)); // IDN not punycoded
    // userinfo is parsed, so callers can see the real host
    REQUIRE(parse_url("https://printables.com@evil.tld/a.stl", u));
    CHECK(u.has_userinfo);
    CHECK(u.host == "evil.tld");
}

TEST_CASE("host_is_or_under matches a domain and its subdomains only", "[Untrusted][Url]")
{
    CHECK(host_is_or_under("printables.com", "printables.com"));
    CHECK(host_is_or_under("files.printables.com", "printables.com"));
    CHECK(host_is_or_under("FILES.Printables.COM", "printables.com"));
    CHECK_FALSE(host_is_or_under("printables.com.evil.tld", "printables.com"));
    CHECK_FALSE(host_is_or_under("evilprintables.com", "printables.com"));
    CHECK_FALSE(host_is_or_under("printables.co", "printables.com"));
    CHECK_FALSE(host_is_or_under("", "printables.com"));
}

TEST_CASE("the model-site link checks match real URLs and nothing spoofed", "[Untrusted][Url]")
{
    // The old regex versions matched the whole string against a bare host, so none of these matched.
    CHECK(untrusted::is_printables_link("https://www.printables.com/model/12345-benchy"));
    CHECK(untrusted::is_printables_link("https://printables.com"));
    CHECK(untrusted::is_makerworld_link("https://makerworld.com/en/models/2077"));
    CHECK(untrusted::is_makerworld_link("https://makerworld.com.cn/zh/models/1"));
    CHECK(untrusted::is_thingiverse_link("https://www.thingiverse.com/thing:763622"));

    // The Utils.hpp spellings are the same checks.
    CHECK(Slic3r::is_printables_link("https://www.printables.com/model/1"));
    CHECK_FALSE(Slic3r::is_makerworld_link("https://makerworld.com.evil.tld/"));

    CHECK_FALSE(untrusted::is_printables_link("https://printables.com.evil.tld/model/1"));
    CHECK_FALSE(untrusted::is_printables_link("https://evil.tld/?https://printables.com"));
    CHECK_FALSE(untrusted::is_printables_link("https://printables.com@evil.tld/"));
    CHECK_FALSE(untrusted::is_printables_link("ftp://printables.com/"));
    CHECK_FALSE(untrusted::is_makerworld_link("https://makerworld.com.evil.tld/"));
    CHECK_FALSE(untrusted::is_makerworld_link("https://notmakerworld.com/"));
    CHECK_FALSE(untrusted::is_thingiverse_link("https://thingiverse.com.evil.tld/"));

    CHECK(is_makerworld_file_url("https://public-cdn.bblmw.com/models/a.3mf"));
    CHECK(is_makerworld_file_url("https://makerworld.bblmw.com/a.3mf"));
    CHECK_FALSE(is_makerworld_file_url("https://bblmw.com.evil.tld/a.3mf"));
    CHECK_FALSE(is_makerworld_file_url("https://files.printables.com/a.3mf"));
}

TEST_CASE("local and IP hosts are recognised", "[Untrusted][Url]")
{
    for (const char *h : {"127.0.0.1", "10.0.0.5", "192.168.1.20", "127.1", "2130706433", "0x7f.1", "[::1]", "localhost",
                          "foo.localhost", "printer.local", "nas.lan", "box.home.arpa", "printer"})
        CHECK(is_local_or_ip_host(h));
    for (const char *h : {"files.printables.com", "public-cdn.bblmw.com", "1password.com", "3dprint.example.org"})
        CHECK_FALSE(is_local_or_ip_host(h));
}

// ---- URLs a page asks us to open ------------------------------------------------------------------

TEST_CASE("only web links may be opened from a page", "[Untrusted][OpenUrl]")
{
    CHECK(is_safe_to_open_externally("https://wiki.snapmaker.com/en/page"));
    CHECK(is_safe_to_open_externally("http://example.org/"));
    CHECK(is_safe_to_open_externally("mailto:support@example.org"));

    CHECK_FALSE(is_safe_to_open_externally("file:///C:/Windows/System32/calc.exe"));
    CHECK_FALSE(is_safe_to_open_externally("FILE://server/share/x.exe"));
    CHECK_FALSE(is_safe_to_open_externally("C:\\Windows\\System32\\calc.exe"));
    CHECK_FALSE(is_safe_to_open_externally("\\\\server\\share\\x.exe"));
    CHECK_FALSE(is_safe_to_open_externally("calc.exe"));
    CHECK_FALSE(is_safe_to_open_externally("javascript:alert(1)"));
    CHECK_FALSE(is_safe_to_open_externally("ms-msdt:/id PCWDiagnostic"));
    CHECK_FALSE(is_safe_to_open_externally("search-ms:query=x&crumb=location:\\\\evil\\share"));
    CHECK_FALSE(is_safe_to_open_externally("edgeslicer://open?file=https%3A%2F%2Fx"));
    CHECK_FALSE(is_safe_to_open_externally("https://example.org/ \"&calc"));
    CHECK_FALSE(is_safe_to_open_externally("https://exa mple.org/"));
    CHECK_FALSE(is_safe_to_open_externally("mailto:x@y.org\" & calc"));
    CHECK_FALSE(is_safe_to_open_externally(""));
}

TEST_CASE("our own pages are recognised by origin", "[Untrusted][Bridge]")
{
    CHECK(is_page_server_url("http://127.0.0.1:13619/web/flutter_web/index.html?path=0&edge_page_token=ab", 13619));
    CHECK(is_page_server_url("http://localhost:13619/", 13619));
    CHECK_FALSE(is_page_server_url("http://127.0.0.1:13620/web/", 13619));
    CHECK_FALSE(is_page_server_url("https://127.0.0.1:13619/web/", 13619));
    CHECK_FALSE(is_page_server_url("http://127.0.0.1.evil.tld:13619/", 13619));
    CHECK_FALSE(is_page_server_url("http://user@127.0.0.1:13619/", 13619));
    CHECK_FALSE(is_page_server_url("http://192.168.1.20:13619/", 13619));
    CHECK_FALSE(is_page_server_url("http://127.0.0.1:13619/", 0));

    CHECK(local_path_from_file_url("file:///C:/Program%20Files/EdgeSlicer/resources/web/model/index.html?lang=en") ==
          "C:/Program Files/EdgeSlicer/resources/web/model/index.html");
    CHECK(local_path_from_file_url("file:///usr/share/edgeslicer/web/x.html") == "/usr/share/edgeslicer/web/x.html");
    CHECK(local_path_from_file_url("file://localhost/C:/x.html") == "C:/x.html");
    CHECK(local_path_from_file_url("file://server/share/x.html").empty());
    CHECK(local_path_from_file_url("file:////server/share/x.html").empty());
    CHECK(local_path_from_file_url("file:///%2F%2Fserver/share/x.html").empty());
    CHECK(local_path_from_file_url("https://example.org/x.html").empty());
}

TEST_CASE("only document, picture and model attachments are launched", "[Untrusted][Attachment]")
{
    for (const char *n : {"manual.pdf", "Assembly.PNG", "notes.txt", "part.stl", "bom.xlsx", "guide.docx"})
        CHECK(is_safe_attachment_to_launch(n));
    for (const char *n : {"setup.exe", "run.bat", "x.cmd", "x.ps1", "x.vbs", "x.js", "x.lnk", "x.url", "x.scr", "x.hta", "x.msi",
                          "x.docm", "x.xlsm", "x.doc", "x.pdf.exe", "pdf", ".pdf", "noext"})
        CHECK_FALSE(is_safe_attachment_to_launch(n));
}

// ---- "Open in" links -------------------------------------------------------------------------------

TEST_CASE("parse_open_link understands every scheme we accept", "[Untrusted][Link]")
{
    const std::string file = "https%3A%2F%2Ffiles.printables.com%2Fmedia%2Fprints%2F1%2Fstls%2Fa%20b.stl";
    for (const char *scheme : {"edgeslicer", "orcaslicer", "prusaslicer", "bambustudio", "cura", "ultraone", "snapmaker-orca", "Snapmaker_Orca"}) {
        for (const char *sep : {"://open?file=", "://open/?file="}) {
            const OpenLink l = parse_open_link(std::string(scheme) + sep + file);
            INFO(scheme << sep);
            REQUIRE(l.ok);
            CHECK(l.file_url == "https://files.printables.com/media/prints/1/stls/a b.stl");
            CHECK(l.name.empty());
        }
    }
    const OpenLink named = parse_open_link("edgeslicer://open?file=" + file + "&name=My%20Benchy.3mf");
    REQUIRE(named.ok);
    CHECK(named.name == "My Benchy.3mf");

    const OpenLink bbs = parse_open_link("bambustudioopen://https%3A%2F%2Fpublic-cdn.bblmw.com%2Fa.3mf");
    REQUIRE(bbs.ok);
    CHECK(bbs.scheme == "bambustudioopen");
    CHECK(bbs.file_url == "https://public-cdn.bblmw.com/a.3mf");
}

TEST_CASE("parse_open_link refuses other schemes and actions", "[Untrusted][Link]")
{
    CHECK_FALSE(parse_open_link("edgeslicer://run?file=x").ok);
    CHECK_FALSE(parse_open_link("edgeslicer://open").ok);
    CHECK_FALSE(parse_open_link("edgeslicer://open?file=").ok);
    CHECK_FALSE(parse_open_link("evilslicer://open?file=https%3A%2F%2Fx").ok);
    CHECK_FALSE(parse_open_link("C:\\models\\a.3mf").ok);
    CHECK_FALSE(parse_open_link("https://files.printables.com/a.stl").ok);
}

TEST_CASE("percent_decode", "[Untrusted][Link]")
{
    CHECK(percent_decode("a%20b%2Fc") == "a b/c");
    CHECK(percent_decode("100%") == "100%");
    CHECK(percent_decode("%zz%4") == "%zz%4");
    CHECK(percent_decode("a+b") == "a+b");
}

// ---- download decisions -----------------------------------------------------------------------------

TEST_CASE("model downloads from the known sites are allowed", "[Untrusted][Download]")
{
    for (const char *url : {"https://files.printables.com/media/prints/1/stls/a.stl",
                            "https://media.printables.com/media/prints/1/a.3mf",
                            "https://public-cdn.bblmw.com/models/a.3mf",
                            "https://makerworld.bblmw.com/a.3mf",
                            "https://cdn.thingiverse.com/assets/a.zip",
                            "https://www.thingiverse.com/download:123456",
                            "https://public.resource.snapmaker.com/models/a.3mf",
                            "https://files.cults3d.com/a.step"}) {
        INFO(url);
        CHECK(check_model_download(url, "edgeslicer").verdict == DownloadVerdict::Allow);
    }
    // MakerWorld's signed storage only for the MakerWorld schemes.
    CHECK(check_model_download("https://makerworld.s3.us-west-2.amazonaws.com/a.3mf", "bambustudioopen").verdict == DownloadVerdict::Allow);
    CHECK(check_model_download("https://mw.oss-cn-hangzhou.aliyuncs.com/a.3mf", "bambustudio").verdict == DownloadVerdict::Allow);
    CHECK(check_model_download("https://makerworld.s3.us-west-2.amazonaws.com/a.3mf", "edgeslicer").verdict == DownloadVerdict::Ask);
}

TEST_CASE("model downloads from other public hosts need the user", "[Untrusted][Download]")
{
    const DownloadCheck c = check_model_download("https://models.example.org/a.3mf", "edgeslicer");
    CHECK(c.verdict == DownloadVerdict::Ask);
    CHECK(c.host == "models.example.org");
    CHECK(check_model_download("https://files.printables.com.evil.tld/a.stl").verdict == DownloadVerdict::Ask);
    CHECK(check_model_download("https://evilprintables.com/a.stl").verdict == DownloadVerdict::Ask);
}

TEST_CASE("model downloads that are never allowed", "[Untrusted][Download]")
{
    for (const char *url : {"http://files.printables.com/a.stl",               // not https
                            "file:///C:/Users/me/secret.3mf",                 // local file
                            "ftp://files.printables.com/a.stl",
                            "https://printables.com@evil.tld/a.stl",          // userinfo spoof
                            "https://127.0.0.1:13619/localfile/x.3mf",        // this machine
                            "https://192.168.1.20/a.3mf",                     // the LAN
                            "https://[::1]/a.3mf",
                            "https://localhost/a.3mf",
                            "https://printer.local/a.3mf",
                            "https://files.printables.com/a.exe",             // not a model
                            "https://files.printables.com/a.3mf.bat",
                            "https://files.printables.com/a%2Eexe",
                            "\\\\server\\share\\a.3mf",
                            "not a url"}) {
        INFO(url);
        CHECK(check_model_download(url, "edgeslicer").verdict == DownloadVerdict::Refuse);
    }
}

TEST_CASE("model extensions", "[Untrusted][Download]")
{
    for (const char *n : {"a.3mf", "A.STL", "a.step", "a.stp", "a.obj", "a.zip"})
        CHECK(has_model_extension(n));
    for (const char *n : {"a.exe", "a.3mf.exe", "a.gcode", "3mf", ".3mf", "a.amf", "a"})
        CHECK_FALSE(has_model_extension(n));
}

TEST_CASE("download file names are plain names in the download folder", "[Untrusted][Filename]")
{
    CHECK(sanitize_download_filename("Benchy.3mf") == "Benchy.3mf");
    CHECK(sanitize_download_filename("../../evil.3mf") == "evil.3mf");
    CHECK(sanitize_download_filename("..\\..\\AppData\\Roaming\\evil.3mf") == "evil.3mf");
    CHECK(sanitize_download_filename("C:\\Windows\\evil.3mf") == "evil.3mf");
    CHECK(sanitize_download_filename("C:evil.3mf") == "C_evil.3mf");
    CHECK(sanitize_download_filename("..") == "");
    CHECK(sanitize_download_filename(".") == "");
    CHECK(sanitize_download_filename("...") == "");
    CHECK(sanitize_download_filename("a/..") == "");
    CHECK(sanitize_download_filename(".hidden.3mf") == "hidden.3mf");
    CHECK(sanitize_download_filename("model.3mf. . .") == "model.3mf");
    CHECK(sanitize_download_filename("a<b>c:d\"e|f?g*h.stl") == "a_b_c_d_e_f_g_h.stl");
    CHECK(sanitize_download_filename(std::string("a\x01" "b\n.stl")) == "a_b_.stl");
    CHECK(sanitize_download_filename("NUL.3mf") == "_NUL.3mf");
    CHECK(sanitize_download_filename("con") == "_con");
    CHECK(sanitize_download_filename("Com1.stl") == "_Com1.stl");
    CHECK(sanitize_download_filename("console.stl") == "console.stl");
    const std::string longname = sanitize_download_filename(std::string(400, 'x') + ".3mf");
    CHECK(longname.size() <= 150);
    CHECK(longname.substr(longname.size() - 4) == ".3mf");
    for (const std::string &n : {std::string("a/b\\c.3mf"), std::string("../../x"), std::string("C:\\x\\y.stl")})
        CHECK(sanitize_download_filename(n).find_first_of("/\\:") == std::string::npos);
}

TEST_CASE("downloaded bytes must match the file type", "[Untrusted][Download]")
{
    const std::string zip = std::string("PK\x03\x04", 4) + std::string(60, 'z');
    CHECK(content_matches_extension("a.3mf", zip, 64));
    CHECK(content_matches_extension("a.zip", zip, 64));
    CHECK_FALSE(content_matches_extension("a.3mf", "MZ\x90\x00", 4));
    CHECK_FALSE(content_matches_extension("a.3mf", "<html>", 6));

    std::string stl(84, '\0');
    stl[80] = 2; // two triangles
    CHECK(content_matches_extension("a.stl", stl, 84 + 2 * 50));
    CHECK_FALSE(content_matches_extension("a.stl", stl, 84 + 3 * 50));
    CHECK(content_matches_extension("a.stl", "solid cube\nfacet normal 0 0 1\n", 30));
    CHECK_FALSE(content_matches_extension("a.stl", "<!DOCTYPE html>", 15));

    CHECK(content_matches_extension("a.step", "ISO-10303-21;\nHEADER;", 20));
    CHECK(content_matches_extension("a.stp", "\xEF\xBB\xBFISO-10303-21;", 20));
    CHECK_FALSE(content_matches_extension("a.step", "MZ", 2));

    CHECK(content_matches_extension("a.obj", "v 0 0 0\nv 1 0 0\nf 1 2 3\n", 24));
    CHECK_FALSE(content_matches_extension("a.obj", std::string("MZ\x90\0", 4), 4));
    CHECK_FALSE(content_matches_extension("a.exe", zip, 64));
}

// ---- archive entry names ------------------------------------------------------------------------------

TEST_CASE("archive entry names may not leave the extraction folder", "[Untrusted][ZipSlip]")
{
    for (const char *p : {"readme.txt", "Other Files/readme.txt", "Metadata/plate_1.gcode", "a/b/c.png", "..a/b", "a..b"})
        CHECK(is_safe_archive_relative_path(p));
    for (const char *p : {"", "../x", "a/../../x", "..", ".", "./x", "a/./b", "/etc/passwd", "a//b", "a/", "..\\x",
                          "a\\b", "C:/x", "C:x", "a/.../b", "a/.. /b", "x\x01y"})
        CHECK_FALSE(is_safe_archive_relative_path(p));
}

// ---- settings in project / preset files ---------------------------------------------------------------

static DynamicPrintConfig config_with_post_process(const std::vector<std::string> &values)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("post_process", new ConfigOptionStrings(values));
    return cfg;
}

TEST_CASE("post_process is flagged unless the user already has the same script", "[Untrusted][Settings]")
{
    TrustedValues none;
    CHECK(find_untrusted_settings(config_with_post_process({}), "project settings", none).empty());
    CHECK(find_untrusted_settings(config_with_post_process({"", "  \r\n "}), "project settings", none).empty());

    const DynamicPrintConfig evil = config_with_post_process({"C:\\edge-test-does-not-exist\\marker.exe --flag"});
    auto found = find_untrusted_settings(evil, "project settings", none);
    REQUIRE(found.size() == 1);
    CHECK(found[0].key == "post_process");
    CHECK(found[0].risk == SettingRisk::RunsPrograms);
    CHECK(found[0].value == "C:\\edge-test-does-not-exist\\marker.exe --flag");

    // The user's own script (same lines, whitespace aside) is not flagged.
    TrustedValues mine;
    mine.post_process.insert(joined_post_process(config_with_post_process({"C:\\tools\\thumbs.exe  \r\n\r\nC:\\tools\\b.exe"})));
    CHECK(find_untrusted_settings(config_with_post_process({"  C:\\tools\\thumbs.exe", "C:\\tools\\b.exe "}), "x", mine).empty());
    CHECK(find_untrusted_settings(config_with_post_process({"C:\\tools\\thumbs.exe"}), "x", mine).size() == 1);
}

TEST_CASE("filename_format is flagged when it leaves the output folder", "[Untrusted][Settings]")
{
    CHECK_FALSE(filename_format_leaves_folder("{input_filename_base}_{filament_type[0]}_{print_time}.gcode"));
    CHECK_FALSE(filename_format_leaves_folder("{input_filename_base}_{layer_height/2}.gcode"));
    CHECK_FALSE(filename_format_leaves_folder("[input_filename_base]{if total_toolchanges>0}_mm{endif}.gcode"));
    CHECK_FALSE(filename_format_leaves_folder("{is_extruder_used[0] ? \"a\" : \"b\"}.gcode"));
    CHECK(filename_format_leaves_folder("..\\..\\{input_filename_base}.gcode"));
    CHECK(filename_format_leaves_folder("../{input_filename_base}.gcode"));
    CHECK(filename_format_leaves_folder("sub/{input_filename_base}.gcode"));
    CHECK(filename_format_leaves_folder("C:{input_filename_base}.gcode"));
    CHECK(filename_format_leaves_folder("{\"C:/x/\" + input_filename_base}.gcode"));

    DynamicPrintConfig cfg;
    cfg.set_key_value("filename_format", new ConfigOptionString("../../Startup/{input_filename_base}.gcode"));
    TrustedValues none;
    auto found = find_untrusted_settings(cfg, "p", none);
    REQUIRE(found.size() == 1);
    CHECK(found[0].risk == SettingRisk::WritesOutsideOutputFolder);
    TrustedValues mine;
    mine.filename_format.insert("../../Startup/{input_filename_base}.gcode");
    CHECK(find_untrusted_settings(cfg, "p", mine).empty());
}

TEST_CASE("print-host endpoints and network bed files are always flagged", "[Untrusted][Settings]")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("print_host", new ConfigOptionString("http://attacker.invalid"));
    cfg.set_key_value("print_host_webui", new ConfigOptionString(""));
    cfg.set_key_value("printhost_apikey", new ConfigOptionString("k"));
    cfg.set_key_value("bed_custom_texture", new ConfigOptionString("\\\\attacker.invalid\\share\\bed.png"));
    cfg.set_key_value("bed_custom_model", new ConfigOptionString("C:/Users/me/bed.stl"));
    auto found = find_untrusted_settings(cfg, "p", TrustedValues{});
    std::vector<std::string> keys;
    for (const auto &f : found) {
        CHECK(f.risk == SettingRisk::NetworkEndpoint);
        keys.push_back(f.key);
    }
    CHECK(keys == std::vector<std::string>{"print_host", "printhost_apikey", "bed_custom_texture"});
}

TEST_CASE("neutralize_settings puts back the baseline or the default", "[Untrusted][Settings]")
{
    DynamicPrintConfig cfg = config_with_post_process({"C:\\evil.exe"});
    cfg.set_key_value("filename_format", new ConfigOptionString("../x.gcode"));
    cfg.set_key_value("print_host", new ConfigOptionString("http://attacker.invalid"));
    DynamicPrintConfig baseline = config_with_post_process({"C:\\tools\\mine.exe"});
    baseline.set_key_value("print_host", new ConfigOptionString("http://my-printer.lan"));

    auto found = find_untrusted_settings(cfg, "p", TrustedValues{});
    REQUIRE(found.size() == 3);
    CHECK(neutralize_settings(cfg, found, &baseline) == 3);
    CHECK(joined_post_process(cfg) == "C:\\tools\\mine.exe");
    CHECK(cfg.opt_string("filename_format") == print_config_def.get("filename_format")->get_default_value<ConfigOptionString>()->value);
    CHECK(cfg.opt_string("print_host").empty()); // network keys never come from the baseline
    CHECK(find_untrusted_settings(cfg, "p", TrustedValues{}).empty() == false); // mine.exe is not in an empty trusted set...
    TrustedValues mine;
    mine.post_process.insert("C:\\tools\\mine.exe");
    CHECK(find_untrusted_settings(cfg, "p", mine).empty()); // ...but is the user's own

    DynamicPrintConfig cfg2 = config_with_post_process({"C:\\evil.exe"});
    auto found2 = find_untrusted_settings(cfg2, "p", TrustedValues{});
    neutralize_settings(cfg2, found2, nullptr);
    CHECK(joined_post_process(cfg2).empty());
}

// ---- a project 3MF that carries scripts ----------------------------------------------------------------

namespace {

struct Scratch
{
    fs::path root;
    Scratch()
    {
        root = fs::temp_directory_path() / ("snorca_untrusted_" + std::to_string(get_current_pid()));
        fs::create_directories(root);
        set_temporary_dir(root.string());
    }
    ~Scratch()
    {
        boost::system::error_code ec;
        fs::remove_all(root, ec);
    }
};

const std::string FIXTURE = std::string(TEST_DATA_DIR) + "/untrusted_3mf/post_process_project.3mf";

struct Loaded
{
    Model                 model;
    DynamicPrintConfig    config;
    std::vector<Preset *> presets;
    ~Loaded()
    {
        for (Preset *p : presets)
            delete p;
    }
};

void load_fixture(Loaded &out)
{
    ConfigSubstitutionContext subs{ForwardCompatibilitySubstitutionRule::Enable};
    En3mfType                 type = En3mfType::From_BBS;
    PlateDataPtrs             plates;
    Semver                    version;
    out.model = Model::read_from_archive(FIXTURE, &out.config, &subs, type,
                                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::LoadAuxiliary |
                                             LoadStrategy::AddDefaultInstances,
                                         &plates, &out.presets, &version);
    release_PlateData_list(plates);
}

const Preset *find_embedded(const std::vector<Preset *> &presets, const std::string &name)
{
    for (const Preset *p : presets)
        if (p->name == name)
            return p;
    return nullptr;
}

} // namespace

TEST_CASE("a project 3MF can carry post-processing scripts, and the guard finds all of them", "[Untrusted][Project]")
{
    Scratch scratch;
    Loaded  loaded;
    load_fixture(loaded);

    // The project settings and the embedded print preset both carry a script; the embedded
    // printer preset points the printer tab at a foreign web UI.
    CHECK(joined_post_process(loaded.config) == "C:\\edge-test-does-not-exist\\edge_pp_marker_project.exe");
    const Preset *evil_process = find_embedded(loaded.presets, "Evil Process");
    const Preset *evil_printer = find_embedded(loaded.presets, "Evil Printer");
    REQUIRE(evil_process != nullptr);
    REQUIRE(evil_printer != nullptr);
    CHECK(joined_post_process(evil_process->config) == "C:\\edge-test-does-not-exist\\edge_pp_marker_embedded.exe");
    CHECK(evil_printer->config.opt_string("print_host_webui") == "http://attacker.invalid/ui");

    // What Plater's guard sees, with a user who has no scripts of their own.
    const TrustedValues none;
    auto project = find_untrusted_settings(loaded.config, "project settings", none);
    std::vector<std::string> keys;
    for (const auto &f : project)
        keys.push_back(f.key);
    CHECK(keys == std::vector<std::string>{"post_process", "filename_format", "print_host", "bed_custom_texture"});
    CHECK(find_untrusted_settings(evil_process->config, evil_process->name, none).size() == 1);
    CHECK(find_untrusted_settings(evil_printer->config, evil_printer->name, none).size() == 2);

    // Stripped, nothing is left to run.
    neutralize_settings(loaded.config, project, nullptr);
    CHECK(joined_post_process(loaded.config).empty());
    CHECK(find_untrusted_settings(loaded.config, "project settings", none).empty());
}

TEST_CASE("the 3MF reader keeps archive entries inside their folders", "[Untrusted][ZipSlip]")
{
    Scratch scratch;
    Loaded  loaded;
    load_fixture(loaded);

    const fs::path aux(loaded.model.get_auxiliary_file_temp_path());
    // The ordinary attachment is extracted...
    CHECK(fs::exists(aux / "Other Files" / "readme.txt"));
    // ...the entries that climb out ("Auxiliaries/../../x", "Metadata/../../x") are not.
    CHECK_FALSE(fs::exists((aux / ".." / ".." / "edge_zipslip_aux.txt").lexically_normal()));
    const fs::path backup(loaded.model.get_backup_path());
    CHECK_FALSE(fs::exists((backup / "Metadata" / ".." / ".." / "edge_zipslip_meta.gcode").lexically_normal()));
    // An entry whose own name is harmless ("Auxiliaries/benign.txt") but whose Unicode Path extra
    // field climbs out: the reader's older "/../" check only looked at the name.
    CHECK_FALSE(fs::exists((aux / ".." / ".." / "edge_zipslip_unicode.txt").lexically_normal()));
    CHECK_FALSE(fs::exists(aux / "benign.txt"));
    // Nothing named like the markers anywhere in the scratch tree.
    for (fs::recursive_directory_iterator it(scratch.root), end; it != end; ++it)
        CHECK(it->path().filename().string().find("edge_zipslip") == std::string::npos);
}
