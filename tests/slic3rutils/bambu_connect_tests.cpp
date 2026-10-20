#include <catch2/catch.hpp>

#include <set>
#include <string>
#include <vector>

#include "slic3r/Utils/BambuConnect.hpp"
#include "slic3r/GUI/PrintSelectKeys.hpp"

using namespace Slic3r::BambuConnect;

namespace {

FileExists exists_set(const std::vector<std::string> &existing)
{
    return [existing](const std::string &path) {
        for (const auto &p : existing)
            if (p == path)
                return true;
        return false;
    };
}

// Reverse of encode_uri_component, for round-trip checks (what Connect's decodeURIComponent does).
std::string decode_percent(const std::string &s)
{
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out += char(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else
            out += s[i];
    }
    return out;
}

// The value of one query parameter of an import URL, still encoded.
std::string query_param(const std::string &url, const std::string &name)
{
    const size_t q = url.find('?');
    REQUIRE(q != std::string::npos);
    size_t pos = q + 1;
    while (pos <= url.size()) {
        size_t amp = url.find('&', pos);
        if (amp == std::string::npos)
            amp = url.size();
        const std::string pair = url.substr(pos, amp - pos);
        const size_t      eq   = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name)
            return pair.substr(eq + 1);
        pos = amp + 1;
    }
    FAIL("parameter " << name << " missing in " << url);
    return {};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// encodeURIComponent
// ---------------------------------------------------------------------------------------------

TEST_CASE("encode_uri_component matches JavaScript encodeURIComponent", "[BambuConnect]")
{
    // Bambu's own wiki example.
    CHECK(encode_uri_component("/tmp/cube.gcode.3mf") == "%2Ftmp%2Fcube.gcode.3mf");
    CHECK(encode_uri_component("Cube") == "Cube");
    // The marks encodeURIComponent leaves alone.
    CHECK(encode_uri_component("A-Z_a.z!~*'()09") == "A-Z_a.z!~*'()09");
    // Reserved characters are all escaped; a space is %20, never '+'.
    CHECK(encode_uri_component("a b") == "a%20b");
    CHECK(encode_uri_component("&=?#+%:;,@$") == "%26%3D%3F%23%2B%25%3A%3B%2C%40%24");
    CHECK(encode_uri_component("C:\\Users\\x") == "C%3A%5CUsers%5Cx");
    CHECK(encode_uri_component("") == "");
}

TEST_CASE("encode_uri_component encodes UTF-8 bytes with upper-case hex", "[BambuConnect]")
{
    // u8 literals as explicit bytes so the test does not depend on the source charset.
    const std::string e_acute = "\xC3\xA9";                 // U+00E9
    const std::string kanji   = "\xE6\xBC\xA2\xE5\xAD\x97"; // U+6F22 U+5B57
    const std::string emoji   = "\xF0\x9F\x99\x82";         // U+1F642
    CHECK(encode_uri_component("caf" + e_acute) == "caf%C3%A9");
    CHECK(encode_uri_component(kanji) == "%E6%BC%A2%E5%AD%97");
    CHECK(encode_uri_component(emoji) == "%F0%9F%99%82");
    CHECK(decode_percent(encode_uri_component("caf" + e_acute + " " + kanji + emoji)) == "caf" + e_acute + " " + kanji + emoji);
}

// ---------------------------------------------------------------------------------------------
// Import URL
// ---------------------------------------------------------------------------------------------

TEST_CASE("build_import_url reproduces Bambu's documented example", "[BambuConnect]")
{
    CHECK(build_import_url("/tmp/cube.gcode.3mf", "Cube") ==
          "bambu-connect://import-file?path=%2Ftmp%2Fcube.gcode.3mf&name=Cube&version=1.0.0");
}

TEST_CASE("build_import_url: Windows path with spaces keeps back-slashes, encoded", "[BambuConnect]")
{
    const std::string path = "C:\\Users\\Jo Smith\\AppData\\Local\\Temp\\EdgeSlicer\\bambu-connect\\Benchy PLA.gcode.3mf";
    const std::string url  = build_import_url(path, "Benchy PLA");
    CHECK(url.rfind("bambu-connect://import-file?path=C%3A%5CUsers%5CJo%20Smith%5C", 0) == 0);
    CHECK(url.find(' ') == std::string::npos);
    CHECK(url.find('\\') == std::string::npos);
    CHECK(decode_percent(query_param(url, "path")) == path);
    CHECK(decode_percent(query_param(url, "name")) == "Benchy PLA");
    CHECK(query_param(url, "version") == "1.0.0");
}

TEST_CASE("build_import_url: macOS path with spaces and unicode", "[BambuConnect]")
{
    const std::string path = "/var/folders/xy/T/EdgeSlicer/bambu-connect/Caf\xC3\xA9 \xE6\xBC\xA2.gcode.3mf";
    const std::string name = "Caf\xC3\xA9 \xE6\xBC\xA2";
    const std::string url  = build_import_url(path, name);
    CHECK(url.rfind("bambu-connect://import-file?path=%2Fvar%2Ffolders%2Fxy%2FT%2F", 0) == 0);
    CHECK(query_param(url, "name") == "Caf%C3%A9%20%E6%BC%A2");
    CHECK(decode_percent(query_param(url, "path")) == path);
    // Every byte of the URL is printable ASCII.
    for (unsigned char c : url)
        CHECK((c > 0x20 && c < 0x7F));
}

TEST_CASE("build_import_url: values with & = ? # cannot split the query", "[BambuConnect]")
{
    const std::string url = build_import_url("/tmp/a&b=c?d#e.gcode.3mf", "x&name=evil");
    // Exactly three parameters: path, name, version.
    size_t amps = 0;
    for (char c : url)
        amps += c == '&';
    CHECK(amps == 2);
    CHECK(url.find('#') == std::string::npos);
    CHECK(decode_percent(query_param(url, "name")) == "x&name=evil");
    CHECK(decode_percent(query_param(url, "path")) == "/tmp/a&b=c?d#e.gcode.3mf");
}

TEST_CASE("build_import_url: long paths are encoded in full", "[BambuConnect]")
{
    // A 300+ character Windows long path (\\?\ prefix) and a deep macOS path.
    std::string win = "\\\\?\\C:\\";
    std::string mac = "/";
    for (int i = 0; i < 30; ++i) {
        win += "folder number " + std::to_string(i) + "\\";
        mac += "folder number " + std::to_string(i) + "/";
    }
    win += "plate.gcode.3mf";
    mac += "plate.gcode.3mf";
    REQUIRE(win.size() > 300);
    for (const std::string &path : { win, mac }) {
        const std::string url = build_import_url(path, "plate");
        CHECK(decode_percent(query_param(url, "path")) == path);
        CHECK(url.size() > path.size());
        CHECK(url.substr(url.size() - std::string("&version=1.0.0").size()) == "&version=1.0.0");
    }
}

// ---------------------------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------------------------

TEST_CASE("display_name_for strips the sliced-file extension", "[BambuConnect]")
{
    CHECK(display_name_for("Cube_PLA_1h2m.gcode.3mf") == "Cube_PLA_1h2m");
    CHECK(display_name_for("Cube.GCODE.3MF") == "Cube");
    CHECK(display_name_for("Cube.3mf") == "Cube");
    CHECK(display_name_for("Cube.gcode") == "Cube");
    CHECK(display_name_for("C:\\out\\Cube.gcode.3mf") == "Cube");
    CHECK(display_name_for("/tmp/out/Cube.gcode.3mf") == "Cube");
    CHECK(display_name_for("my.model.v2.gcode.3mf") == "my.model.v2");
}

TEST_CASE("display_name_for removes query-breaking and control characters", "[BambuConnect]")
{
    CHECK(display_name_for("A&B=C?D#E%F+G.gcode.3mf") == "A_B_C_D_E_F_G");
    CHECK(display_name_for("  tab\tbed \x01\x7F.gcode.3mf") == "tabbed");
    CHECK(display_name_for(".gcode.3mf") == "plate");
    CHECK(display_name_for("") == "plate");
    // Unicode names stay as they are.
    CHECK(display_name_for("Caf\xC3\xA9 \xE6\xBC\xA2.gcode.3mf") == "Caf\xC3\xA9 \xE6\xBC\xA2");
}

TEST_CASE("temp_file_name_for makes a safe file name", "[BambuConnect]")
{
    CHECK(temp_file_name_for("Cube") == "Cube.gcode.3mf");
    CHECK(temp_file_name_for("a<b>c:d\"e/f\\g|h?i*j") == "a_b_c_d_e_f_g_h_i_j.gcode.3mf");
    CHECK(temp_file_name_for("a&b=c#d%e+f") == "a_b_c_d_e_f.gcode.3mf");
    CHECK(temp_file_name_for("trailing. . ") == "trailing.gcode.3mf");
    CHECK(temp_file_name_for("") == "plate.gcode.3mf");
    CHECK(temp_file_name_for("...") == "plate.gcode.3mf");
    CHECK(temp_file_name_for("CON") == "CON_.gcode.3mf");
    CHECK(temp_file_name_for("com1") == "com1_.gcode.3mf");
    CHECK(temp_file_name_for("console") == "console.gcode.3mf");
    CHECK(temp_file_name_for("Caf\xC3\xA9") == "Caf\xC3\xA9.gcode.3mf");
}

TEST_CASE("temp_file_name_for cuts long names on a UTF-8 boundary", "[BambuConnect]")
{
    std::string ascii(250, 'x');
    CHECK(temp_file_name_for(ascii) == std::string(100, 'x') + ".gcode.3mf");

    // 99 ASCII bytes then a 3-byte character: byte 100 falls inside it, so the whole character goes.
    std::string mixed = std::string(99, 'y') + "\xE6\xBC\xA2" + "tail";
    CHECK(temp_file_name_for(mixed) == std::string(99, 'y') + ".gcode.3mf");

    // 98 + 2-byte character fits exactly at 100.
    std::string fits = std::string(98, 'z') + "\xC3\xA9" + "tail";
    CHECK(temp_file_name_for(fits) == std::string(98, 'z') + "\xC3\xA9" + ".gcode.3mf");
}

// ---------------------------------------------------------------------------------------------
// Installation detection
// ---------------------------------------------------------------------------------------------

TEST_CASE("executable_from_command parses shell open commands", "[BambuConnect]")
{
    CHECK(executable_from_command("\"C:\\Users\\me\\AppData\\Local\\Programs\\Bambu Connect\\Bambu Connect.exe\" \"%1\"") ==
          "C:\\Users\\me\\AppData\\Local\\Programs\\Bambu Connect\\Bambu Connect.exe");
    CHECK(executable_from_command("C:\\Program Files\\Bambu Connect\\Bambu Connect.EXE --url \"%1\"") ==
          "C:\\Program Files\\Bambu Connect\\Bambu Connect.EXE");
    CHECK(executable_from_command("  bambu-connect %1") == "bambu-connect");
    CHECK(executable_from_command("\"unterminated") == "unterminated");
    CHECK(executable_from_command("") == "");
}

TEST_CASE("detect_windows needs the key, URL Protocol and an open command", "[BambuConnect]")
{
    const std::string exe = "C:\\Users\\me\\AppData\\Local\\Programs\\Bambu Connect\\Bambu Connect.exe";
    WindowsRegistration reg;
    reg.key_exists       = true;
    reg.has_url_protocol = true;
    reg.open_command     = "\"" + exe + "\" \"%1\"";

    SECTION("registered and on disk") { CHECK(detect_windows(reg, exists_set({ exe })) == Installed::Yes); }
    SECTION("stale key after an uninstall") { CHECK(detect_windows(reg, exists_set({})) == Installed::No); }
    SECTION("no key")
    {
        reg.key_exists = false;
        CHECK(detect_windows(reg, exists_set({ exe })) == Installed::No);
    }
    SECTION("key without URL Protocol is not a URL handler")
    {
        reg.has_url_protocol = false;
        CHECK(detect_windows(reg, exists_set({ exe })) == Installed::No);
    }
    SECTION("no open command")
    {
        reg.open_command = "   ";
        CHECK(detect_windows(reg, exists_set({ exe })) == Installed::No);
    }
    SECTION("unexpanded environment variable is trusted")
    {
        reg.open_command = "\"%LOCALAPPDATA%\\Programs\\Bambu Connect\\Bambu Connect.exe\" \"%1\"";
        CHECK(detect_windows(reg, exists_set({})) == Installed::Yes);
    }
    SECTION("no file check available is trusted") { CHECK(detect_windows(reg, FileExists()) == Installed::Yes); }
}

TEST_CASE("detect_macos and detect_linux", "[BambuConnect]")
{
    CHECK(detect_macos("/Applications/Bambu Connect.app") == Installed::Yes);
    CHECK(detect_macos("") == Installed::No);
    CHECK(detect_macos("  ") == Installed::No);

    CHECK(detect_linux(true, "bambu-connect.desktop\n") == Installed::Yes);
    CHECK(detect_linux(true, "\n") == Installed::No);
    CHECK(detect_linux(true, "") == Installed::No);
    CHECK(detect_linux(false, "") == Installed::Unknown);
    CHECK(detect_linux(false, "bambu-connect.desktop") == Installed::Unknown);
}

// ---------------------------------------------------------------------------------------------
// Temp folder housekeeping
// ---------------------------------------------------------------------------------------------

TEST_CASE("stale_temp_files only picks old .gcode.3mf files", "[BambuConnect]")
{
    const std::time_t now = 1'800'000'000;
    const std::time_t day = 24 * 60 * 60;
    std::vector<TempEntry> entries = {
        { "old.gcode.3mf", now - 4 * day },
        { "OLD2.GCODE.3MF", now - 10 * day },
        { "fresh.gcode.3mf", now - 1 * day },
        { "just_under.gcode.3mf", now - TEMP_MAX_AGE_S },
        { "notes.txt", now - 30 * day },
        { "project.3mf", now - 30 * day },
        { "unknown_time.gcode.3mf", 0 },
    };
    const std::vector<std::string> stale = stale_temp_files(entries, now, TEMP_MAX_AGE_S);
    const std::set<std::string>    got(stale.begin(), stale.end());
    CHECK(got == std::set<std::string>{ "old.gcode.3mf", "OLD2.GCODE.3MF" });
}

// ---------------------------------------------------------------------------------------------
// Print-button dropdown key
// ---------------------------------------------------------------------------------------------

TEST_CASE("Send to Bambu Connect has a stable remembered-action key", "[BambuConnect][PrintSelectKeys]")
{
    using namespace Slic3r::GUI::PrintSelectKeys;
    CHECK(std::string(key(eSendToBambuConnect)) == "send_to_bambu_connect");
    int parsed = -1;
    REQUIRE(from_key("send_to_bambu_connect", parsed));
    CHECK(parsed == eSendToBambuConnect);
    CHECK_FALSE(requires_print_host(eSendToBambuConnect));
    // Not restored on a printer that does not offer it (non-Bambu dropdown).
    const int offered_third_party[] = { eSendGcode, eExportSlicedFile, eExportAllSlicedFile, eExportGcode };
    CHECK(resolve_or_default("send_to_bambu_connect", offered_third_party, 4, eSendGcode) == eSendGcode);
    const int offered_bambu[] = { ePrintPlate, eExportSlicedFile, eExportAllSlicedFile, eExportGcode, eSendToBambuConnect };
    CHECK(resolve_or_default("send_to_bambu_connect", offered_bambu, 5, ePrintPlate) == eSendToBambuConnect);
}
