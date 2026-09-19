// What a printer error code says to the user.
//
// The bug these cover: an H2C reporting 0C00010000020015 surfaced as the bare string
// "0C00010000020015" on the device tab, in the hub's events and in the phone's push notification,
// where Bambu Studio shows "Nozzle Camera is malfunctioning...". Three separate causes, all of
// which are reachable here without a printer, a window or the network:
//
//   1. HMSQuery was only constructed when stealth mode was off, and stealth mode is forced on
//      until the setup wizard sets firstguide.finish - so get_hms_query() was null and every
//      surface fell through to its own bare-code fallback. (GUI_App.cpp; not unit-testable here,
//      but it is why the lookup was never even reached.)
//   2. Each surface invented its own fallback, and four of them settled on printing the hex.
//      That is what describe_error / format_error replace, and what these cases pin.
//   3. Bambu's own tables ship an empty `intro` for some codes - 0C00010000020015 is one of them,
//      in the shipped snapshot AND in the live cloud table as of 2026-09. So "fetch the table"
//      cannot be the fix for this code: the unknown-code path is the normal path for it, and it
//      has to say something useful rather than nothing.
//
// The lookups here go through the *_local seams, which read the tables and consult no AppConfig,
// no clock and no cloud.

#include <catch2/catch.hpp>

#include "slic3r/GUI/HMS.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <fstream>
#include <string>

using Slic3r::GUI::HMSQuery;
namespace fs = boost::filesystem;

namespace {

// A table in the shape the server and the shipped files use: the {"result","ver","data"} envelope
// around device_hms (the long attr/code form) and device_error (the eight-digit print_error form).
const char* const FIXTURE_TABLE = R"({
  "result": 0,
  "ver": 202609171044,
  "data": {
    "device_hms": {
      "en": [
        { "ecode": "0C00010000020014", "intro": "Nozzle Camera is malfunctioning. If this issue occurs multiple times during printing, please contact customer support." },
        { "ecode": "0C00010000020015", "intro": "" },
        { "ecode": "0500060000020032", "intro": "Nozzle Camera is not connected. Please check the hardware and cable connections." }
      ],
      "de": [
        { "ecode": "0C00010000020014", "intro": "Die Duesenkamera funktioniert nicht richtig." }
      ]
    },
    "device_error": {
      "en": [
        { "ecode": "0500808F", "intro": "Nozzle camera lens is dirty, affecting AI monitoring." },
        { "ecode": "07FFC010", "intro": "Insert the filament until it can not be pushed any farther." }
      ]
    }
  }
})";

// The tables are found by data_dir()/hms/hms_<lang>_<series>.json. data_dir() is process-wide and
// the slic3rutils binary does not otherwise set it, so this points it at a scratch directory and
// writes the fixture where get_hms_file() will look for it.
struct FixtureTables
{
    fs::path dir;
    explicit FixtureTables(const std::string& series)
    {
        dir = fs::temp_directory_path() / fs::unique_path("snorca-hms-%%%%-%%%%");
        fs::create_directories(dir / "hms");
        Slic3r::set_data_dir(dir.string());
        const std::string name = HMSQuery::get_hms_file(QUERY_HMS_INFO, "en", series);
        std::ofstream(( dir / "hms" / name).string().c_str()) << FIXTURE_TABLE;
        const std::string de = HMSQuery::get_hms_file(QUERY_HMS_INFO, "de", series);
        std::ofstream((dir / "hms" / de).string().c_str()) << FIXTURE_TABLE;
    }
    ~FixtureTables()
    {
        boost::system::error_code ig;
        fs::remove_all(dir, ig);
    }
};

// An H2C serial: the first three characters pick hms_<lang>_31B.json.
const char* const H2C = "31BA0123456789";

} // namespace

// ---- the formatting, which needs no tables at all ----

TEST_CASE("pretty_code groups a code in fours", "[HmsLookup]")
{
    // The two shapes the printer reports.
    CHECK(HMSQuery::pretty_code("0C00010000020015") == "0C00 0100 0002 0015");
    CHECK(HMSQuery::pretty_code("05004046") == "0500 4046");
    // Anything else is left exactly as it came rather than chopped up on a guess.
    CHECK(HMSQuery::pretty_code("") == "");
    CHECK(HMSQuery::pretty_code("123") == "123");
    // Eight characters that are not all hex digits are not a Bambu code: left alone.
    CHECK(HMSQuery::pretty_code("HMS_0300") == "HMS_0300");
    CHECK(HMSQuery::pretty_code("klipper_") == "klipper_");
    CHECK(HMSQuery::pretty_code("0C000100000200150000") == "0C000100000200150000");
}

TEST_CASE("format_error puts the text first and the code in brackets", "[HmsLookup]")
{
    const wxString known = HMSQuery::format_error("Nozzle Camera is malfunctioning.", "0C00010000020015");
    CHECK(known == "Nozzle Camera is malfunctioning. (0C00 0100 0002 0015)");
}

TEST_CASE("format_error names the code when the text is unknown", "[HmsLookup]")
{
    const wxString unknown = HMSQuery::format_error(wxEmptyString, "0C00010000020015");
    // The grouped code, and the two places that do know what it means.
    CHECK(unknown == "Printer error 0C00 0100 0002 0015 - see the printer screen or Bambu's error-code page");
    // The bare code on its own must never be the whole of what a surface shows.
    CHECK(unknown != "0C00010000020015");
    CHECK(unknown != "0C00 0100 0002 0015");
}

TEST_CASE("print_error_code is the eight-digit spelling the tables are keyed by", "[HmsLookup]")
{
    CHECK(HMSQuery::print_error_code(0x0500808F) == "0500808F");
    CHECK(HMSQuery::print_error_code(0x07FFC010) == "07FFC010");
}

// ---- the lookup, against the fixture tables ----

TEST_CASE("an HMS code resolves to the printer's own sentence", "[HmsLookup]")
{
    FixtureTables tables("31B");
    HMSQuery      q;
    const wxString text = q.query_hms_msg_local(H2C, "0C00010000020014", "en");
    CHECK(text.Contains("Nozzle Camera is malfunctioning"));

    // ...and the user-facing form carries both the sentence and the code.
    const wxString described = q.describe_error(H2C, "0C00010000020014", /*local_only*/ true);
    CHECK(described.Contains("Nozzle Camera is malfunctioning"));
    CHECK(described.Contains("0C00 0100 0002 0015") == false);
    CHECK(described.Contains("0C00 0100 0002 0014"));
}

TEST_CASE("the owner's code: present in the table but with no text", "[HmsLookup]")
{
    // This is 0C00010000020015 exactly as Bambu ships it - the entry exists, `intro` is "". The
    // lookup correctly finds nothing, so the surfaces must fall back to the code *plus a hint*
    // and never to the bare code alone.
    FixtureTables tables("31B");
    HMSQuery      q;
    CHECK(q.query_hms_msg_local(H2C, "0C00010000020015", "en").IsEmpty());

    const wxString described = q.describe_error(H2C, "0C00010000020015", /*local_only*/ true);
    CHECK(described.StartsWith("Printer error 0C00 0100 0002 0015"));
    CHECK(described != "0C00010000020015");
}

TEST_CASE("an unknown code falls back to the code and a hint", "[HmsLookup]")
{
    FixtureTables tables("31B");
    HMSQuery      q;
    // Not in the table at all.
    const wxString described = q.describe_error(H2C, "DEADBEEFDEADBEEF", /*local_only*/ true);
    CHECK(described.StartsWith("Printer error DEAD BEEF DEAD BEEF"));
    CHECK(described != "DEADBEEFDEADBEEF");
}

TEST_CASE("a print_error resolves against device_error", "[HmsLookup]")
{
    FixtureTables tables("31B");
    HMSQuery      q;
    wxString      msg;
    CHECK(q.query_print_error_msg_local(H2C, 0x0500808F, "en", msg));
    CHECK(msg.Contains("Nozzle camera lens is dirty"));

    // The eight-digit form goes to device_error, the sixteen-digit form to device_hms; the shared
    // helper picks by the length of the code so a caller holding "a code" need not know which.
    const wxString described = q.describe_print_error(H2C, 0x0500808F, /*local_only*/ true);
    CHECK(described.Contains("Nozzle camera lens is dirty"));
    CHECK(described.Contains("0500 808F"));
}

TEST_CASE("an unknown print_error still names its code", "[HmsLookup]")
{
    FixtureTables tables("31B");
    HMSQuery      q;
    wxString      msg;
    CHECK_FALSE(q.query_print_error_msg_local(H2C, 0x12345678, "en", msg));

    const wxString described = q.describe_print_error(H2C, 0x12345678, /*local_only*/ true);
    CHECK(described.StartsWith("Printer error 1234 5678"));
}

TEST_CASE("a code already grouped in fours still matches", "[HmsLookup]")
{
    // The hub's debug route and a relayed event can hand the code back in the form it was shown
    // in; it must not become unresolvable for having been made readable.
    FixtureTables tables("31B");
    HMSQuery      q;
    const wxString described = q.describe_error(H2C, "0C00 0100 0002 0014", /*local_only*/ true);
    CHECK(described.Contains("Nozzle Camera is malfunctioning"));
}

TEST_CASE("the lookup is case-insensitive about the code", "[HmsLookup]")
{
    FixtureTables tables("31B");
    HMSQuery      q;
    CHECK(q.describe_error(H2C, "0c00010000020014", /*local_only*/ true).Contains("Nozzle Camera is malfunctioning"));
}

TEST_CASE("the app's language is preferred and English is the fallback", "[HmsLookup]")
{
    FixtureTables tables("31B");
    HMSQuery      q;
    // The German table has this code...
    CHECK(q.query_hms_msg_local(H2C, "0C00010000020014", "de").Contains("Duesenkamera"));
    // ...and this one it does not, though English does. The German list exists, so the fallback
    // used to be skipped entirely and the owner was shown nothing; English answers now.
    CHECK(q.query_hms_msg_local(H2C, "0500060000020032", "de").Contains("Nozzle Camera is not connected"));
    // English stays the preferred fallback, not merely "some other language".
    CHECK(q.query_hms_msg_local(H2C, "0500060000020032", "en").Contains("Nozzle Camera is not connected"));
}

TEST_CASE("an empty code describes nothing", "[HmsLookup]")
{
    FixtureTables tables("31B");
    HMSQuery      q;
    // A printer with no error must not be given a sentence about one.
    CHECK(q.describe_error(H2C, "", /*local_only*/ true).IsEmpty());
}

TEST_CASE("a series with no table of its own still names the code", "[HmsLookup]")
{
    // 094 is the H2D; the fixture only wrote 31B. The surfaces must degrade to the code and a
    // hint rather than to an empty string, which is what left the hub event saying only
    // "X reported an error" with nothing after it.
    FixtureTables tables("31B");
    HMSQuery      q;
    const wxString described = q.describe_error("094A0123456789", "0C00010000020014", /*local_only*/ true);
    CHECK(described.StartsWith("Printer error 0C00 0100 0002 0014"));
}

// ---- overrides: EdgeSlicer's own descriptions, ahead of Bambu's tables ----
//
// The reason this layer exists: 0C00010000020015 has an empty `intro` in every shipped language
// and in the live cloud table, so no refresh can ever supply it. A description captured by hand
// is the only way that code gets a sentence.

namespace {

// Writes both overlay files the resolver consults. The shipped one normally lives in
// resources_dir(), which a test cannot move, so the cases below that need it are written against
// the user overlay in data_dir() - the one the capture paths (--hms-add, the hub route) write.
void write_user_overrides(const fs::path& data, const std::string& body)
{
    fs::create_directories(data / "hms");
    std::ofstream((data / "hms" / "overrides.json").string().c_str()) << body;
}

} // namespace

TEST_CASE("an override is preferred over the table", "[HmsLookup]")
{
    FixtureTables tables("31B");
    // The table says one thing for 0C00010000020014; the override says another and must win, so
    // that a description which reads badly can be corrected without waiting for Bambu.
    write_user_overrides(tables.dir, R"({"version":1,"overrides":[
        {"code":"0C00010000020014","model":"*","lang":"en","text":"Our own wording for the camera fault."}
    ]})");

    HMSQuery q;
    const wxString described = q.describe_error(H2C, "0C00010000020014", /*local_only*/ true);
    CHECK(described.Contains("Our own wording for the camera fault."));
    CHECK_FALSE(described.Contains("please contact customer support"));
}

TEST_CASE("an override supplies the text Bambu publishes empty", "[HmsLookup]")
{
    // The owner's case, end to end: the code the printer reported, which every table carries with
    // an empty description, now resolves to the sentence captured from Bambu Studio.
    FixtureTables tables("31B");
    write_user_overrides(tables.dir, R"({"version":1,"overrides":[
        {"code":"0C00010000020015","model":"*","lang":"en",
         "text":"Nozzle Camera is malfunctioning. If this issue occurs multiple times during printing, please contact customer support.",
         "source":"Bambu Studio UI","note":"Bambu publishes this text under ...0014 and an empty entry for ...0015"}
    ]})");

    HMSQuery q;
    const wxString described = q.describe_error(H2C, "0C00010000020015", /*local_only*/ true);
    CHECK(described.Contains("Nozzle Camera is malfunctioning"));
    CHECK(described.Contains("0C00 0100 0002 0015"));
    // And it is no longer the "we don't know" answer.
    CHECK_FALSE(described.StartsWith("Printer error"));
}

TEST_CASE("an override for another model does not apply", "[HmsLookup]")
{
    FixtureTables tables("31B");
    write_user_overrides(tables.dir, R"({"version":1,"overrides":[
        {"code":"0C00010000020015","model":"094","lang":"en","text":"H2D-only wording."}
    ]})");

    HMSQuery q;
    // The H2D entry must not answer for an H2C...
    CHECK(q.describe_error(H2C, "0C00010000020015", true).StartsWith("Printer error"));
    // ...but it does answer for an H2D.
    CHECK(q.describe_error("094A0123456789", "0C00010000020015", true).Contains("H2D-only wording"));
}

TEST_CASE("an override falls back through the languages", "[HmsLookup]")
{
    FixtureTables tables("31B");
    write_user_overrides(tables.dir, R"({"version":1,"overrides":[
        {"code":"0C00010000020015","model":"*","lang":"en","text":"English wording."},
        {"code":"0500060000020099","model":"*","lang":"fr","text":"Only French wording."}
    ]})");

    HMSQuery q;
    // Asked in German, with only English recorded: English is the fallback.
    CHECK(q.query_override(H2C, "0C00010000020015", "de").Contains("English wording"));
    // Asked in German with neither German nor English: whatever was recorded beats nothing.
    CHECK(q.query_override(H2C, "0500060000020099", "de").Contains("Only French wording"));
    // Asked in the language it was written in.
    CHECK(q.query_override(H2C, "0C00010000020015", "en").Contains("English wording"));
}

TEST_CASE("add_override round-trips and is picked up without a restart", "[HmsLookup]")
{
    FixtureTables tables("31B");
    HMSQuery      q;

    // Nothing recorded yet: the unknown-code answer.
    CHECK(q.describe_error(H2C, "0C00010000020015", true).StartsWith("Printer error"));

    std::string err;
    REQUIRE(q.add_override("0C00010000020015", "Nozzle Camera is malfunctioning.", "en", "*", "test", "", false, err));
    CHECK(err.empty());

    // Same HMSQuery instance, no restart and no explicit reload: the write invalidated the cache.
    const wxString described = q.describe_error(H2C, "0C00010000020015", true);
    CHECK(described.Contains("Nozzle Camera is malfunctioning."));

    // A second add for the same (code, model, lang) is refused unless forced...
    CHECK_FALSE(q.add_override("0C00010000020015", "Something else.", "en", "*", "", "", false, err));
    CHECK_FALSE(err.empty());
    // ...and forcing it replaces the text rather than appending a duplicate.
    REQUIRE(q.add_override("0C00010000020015", "Replaced wording.", "en", "*", "", "", true, err));
    CHECK(q.describe_error(H2C, "0C00010000020015", true).Contains("Replaced wording."));
}

TEST_CASE("add_override rejects a code that is not a code", "[HmsLookup]")
{
    FixtureTables tables("31B");
    HMSQuery      q;
    std::string   err;
    CHECK_FALSE(q.add_override("nonsense", "text", "en", "*", "", "", false, err));
    CHECK_FALSE(err.empty());
    // Right alphabet, wrong length: 8 or 16 hex digits and nothing else.
    CHECK_FALSE(q.add_override("0C0001", "text", "en", "*", "", "", false, err));
    CHECK_FALSE(q.add_override("0C000100000200150", "text", "en", "*", "", "", false, err));
    // An empty description is not a description.
    CHECK_FALSE(q.add_override("0C00010000020015", "", "en", "*", "", "", false, err));
}

TEST_CASE("is_valid_code accepts both shapes and ignores spacing", "[HmsLookup]")
{
    std::string norm;
    CHECK(HMSQuery::is_valid_code("0C00010000020015", norm));
    CHECK(norm == "0C00010000020015");
    // The grouped spelling the UI shows is accepted, so the owner can paste what they see.
    CHECK(HMSQuery::is_valid_code("0C00 0100 0002 0015", norm));
    CHECK(norm == "0C00010000020015");
    CHECK(HMSQuery::is_valid_code("0500808f", norm));
    CHECK(norm == "0500808F");
    CHECK_FALSE(HMSQuery::is_valid_code("", norm));
    CHECK_FALSE(HMSQuery::is_valid_code("zzzzzzzz", norm));
}

TEST_CASE("an entry with an empty description counts as no description", "[HmsLookup]")
{
    // The distinction the whole fix turns on: 0C00010000020015 IS in Bambu's table, with
    // "intro": "". A lookup that treated "the entry exists" as "we have something to show" would
    // return an empty string and every surface would print the bare code.
    FixtureTables tables("31B");
    HMSQuery      q;
    CHECK(q.query_hms_msg_local(H2C, "0C00010000020015", "en").IsEmpty());
    CHECK(q.describe_error(H2C, "0C00010000020015", true).StartsWith("Printer error"));
}
