#include <catch2/catch.hpp>

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "common_func/common_func.hpp"
#include "libslic3r/Semver.hpp"
#include "slic3r/Utils/AppUpdateCheck.hpp"
#include "slic3r/Utils/Http.hpp"

using namespace Slic3r;
using namespace Slic3r::AppUpdate;

namespace {

const std::string BULLET = "\xE2\x80\xA2 ";

// Shape of a real /releases/latest answer (trimmed): tag, flags, page, body and assets as
// the v2.3.8.5-edge release publishes them.
std::string release_json(const std::string& tag, bool prerelease = false, bool draft = false,
                         const std::string& body = "Short notes.",
                         const std::string& html_url = "https://github.com/aceRage/EdgeSlicer/releases/tag/v2.3.8.5-edge",
                         const std::string& dl = "https://github.com/aceRage/EdgeSlicer/releases/download/v2.3.8.5-edge/")
{
    nlohmann::json j;
    j["tag_name"]   = tag;
    j["prerelease"] = prerelease;
    j["draft"]      = draft;
    j["html_url"]   = html_url;
    j["body"]       = body;
    j["assets"]     = nlohmann::json::array();
    for (const char* name : { "EdgeSlicer_Linux_Ubuntu2404_V2.3.8.5.AppImage", "EdgeSlicer_Mac_universal_V2.3.8.5.dmg",
                              "EdgeSlicer_Network_Plugin_V2.3.8.5.zip", "EdgeSlicer_Windows_V2.3.8.5_portable.zip",
                              "EdgeSlicer_Windows_Installer_V2.3.8.5.exe", "SHA256SUMS.txt" })
        j["assets"].push_back({ { "name", name }, { "browser_download_url", dl + name } });
    return j.dump();
}

} // namespace

TEST_CASE("release tags parse to four-part versions", "[AppUpdate]")
{
    const Version edge = parse_version("v2.4.0.0-edge");
    REQUIRE(edge.valid);
    CHECK(edge.parts == std::array<int, 4>{ { 2, 4, 0, 0 } });
    CHECK(edge.prerelease.empty());
    CHECK(edge.to_string() == "2.4.0.0");

    CHECK(parse_version("2.3.8.5").to_string() == "2.3.8.5");
    CHECK(parse_version("v2.3.9-edge").to_string() == "2.3.9.0"); // three parts pad with 0
    CHECK(parse_version("2.4").to_string() == "2.4.0.0");
    CHECK(parse_version(" V2.4.0.1-EDGE ").to_string() == "2.4.0.1");
    CHECK(parse_version("2.4.0.0+build7").to_string() == "2.4.0.0");

    const Version beta = parse_version("v2.4.1.0-beta1-edge");
    REQUIRE(beta.valid);
    CHECK(beta.prerelease == "beta1");
    CHECK(beta.to_string() == "2.4.1.0-beta1");

    for (const char* bad : { "", "edge", "v2", "2.x.0", "1.2.3.4.5", "latest", "2..3" })
        CHECK_FALSE(parse_version(bad).valid);
}

TEST_CASE("version comparison is numeric per part", "[AppUpdate]")
{
    auto cmp = [](const char* a, const char* b) { return compare_versions(parse_version(a), parse_version(b)); };
    CHECK(cmp("v2.4.0.0-edge", "2.3.8.5") > 0);
    CHECK(cmp("2.3.8.5", "v2.4.0.0-edge") < 0);
    CHECK(cmp("v2.4.0.0-edge", "2.4.0.0") == 0);
    CHECK(cmp("2.3.8.10", "2.3.8.9") > 0);
    CHECK(cmp("2.10.0.0", "2.9.9.9") > 0);
    CHECK(cmp("2.3.9", "2.3.8.5") > 0);
    CHECK(cmp("2.4.0.0", "2.4.0.0-rc1") > 0); // a release beats its prereleases
    CHECK(cmp("2.4.0.0-rc2", "2.4.0.0-rc1") > 0);
    CHECK(cmp("garbage", "0.0.0.1") < 0);

    // Why the module does not use Semver for this: its four-part packing (patch*100 + 4th)
    // makes a three-part 2.3.9 look older than 2.3.8.5.
    CHECK(*Semver::parse("2.3.9") < *Semver::parse("2.3.8.5"));
}

TEST_CASE("skip_version hides that version and anything older", "[AppUpdate]")
{
    CHECK(is_skipped("2.4.0.0", "2.4.0.0"));
    CHECK(is_skipped("2.3.8.5", "2.4.0.0"));
    CHECK_FALSE(is_skipped("2.4.0.1", "2.4.0.0"));
    CHECK_FALSE(is_skipped("2.10.0.0", "2.9.0.0")); // the old string compare got this wrong
    CHECK_FALSE(is_skipped("2.4.0.0", ""));
}

TEST_CASE("GitHub latest-release answers are evaluated against the build version", "[AppUpdate]")
{
    const ReleaseInfo r = parse_github_release(release_json("v2.3.8.5-edge"), Platform::Windows);
    REQUIRE(r.ok);
    CHECK(r.tag == "v2.3.8.5-edge");
    CHECK(r.version_str == "2.3.8.5");
    CHECK_FALSE(r.prerelease);

    // 2.3.8.5 is older than the 2.4.0.0 this branch builds: no popup.
    CHECK(evaluate(r, "2.4.0.0") == Verdict::UpToDate);
    CHECK(evaluate(r, "2.3.8.5") == Verdict::UpToDate);
    CHECK(evaluate(r, "2.3.8.1") == Verdict::UpdateAvailable);
    CHECK(evaluate(r, "2.3.0.0") == Verdict::UpdateAvailable);
    CHECK(evaluate(r, "not a version") == Verdict::Invalid);

    // The real build constant: a published 2.3.8.5 must never nag a 2.4.x build.
    CHECK(evaluate(r, Snapmaker_VERSION) == Verdict::UpToDate);
    const ReleaseInfo next = parse_github_release(release_json("v9.0.0.0-edge"), Platform::Windows);
    CHECK(evaluate(next, Snapmaker_VERSION) == Verdict::UpdateAvailable);
}

TEST_CASE("prereleases and drafts are ignored", "[AppUpdate]")
{
    CHECK(evaluate(parse_github_release(release_json("v9.0.0.0-edge", true), Platform::Windows), "2.4.0.0") == Verdict::Ignored);
    CHECK(evaluate(parse_github_release(release_json("v9.0.0.0-edge", false, true), Platform::Windows), "2.4.0.0") == Verdict::Ignored);
    // A prerelease tag counts even if the GitHub flag was forgotten.
    const ReleaseInfo tagged = parse_github_release(release_json("v9.0.0.0-beta2-edge"), Platform::Windows);
    CHECK(tagged.prerelease);
    CHECK(evaluate(tagged, "2.4.0.0") == Verdict::Ignored);
}

TEST_CASE("broken or error answers are reported, not shown", "[AppUpdate]")
{
    const ReleaseInfo limited = parse_github_release(R"({"message":"API rate limit exceeded for 1.2.3.4.","documentation_url":"https://docs.github.com"})",
                                                     Platform::Windows);
    CHECK_FALSE(limited.ok);
    CHECK(limited.error.find("rate limit") != std::string::npos);
    CHECK(evaluate(limited, "2.4.0.0") == Verdict::Invalid);

    CHECK_FALSE(parse_github_release("<html>502</html>", Platform::Windows).ok);
    CHECK_FALSE(parse_github_release("[]", Platform::Windows).ok);
    CHECK_FALSE(parse_github_release(R"({"tag_name":"nightly"})", Platform::Windows).ok);
}

TEST_CASE("download link is picked per platform", "[AppUpdate]")
{
    const std::string dl = "https://github.com/aceRage/EdgeSlicer/releases/download/v2.3.8.5-edge/";
    const std::string page = "https://github.com/aceRage/EdgeSlicer/releases/tag/v2.3.8.5-edge";
    CHECK(parse_github_release(release_json("v2.3.8.5-edge"), Platform::Windows).download_url == dl + "EdgeSlicer_Windows_Installer_V2.3.8.5.exe");
    CHECK(parse_github_release(release_json("v2.3.8.5-edge"), Platform::MacOS).download_url == dl + "EdgeSlicer_Mac_universal_V2.3.8.5.dmg");
    CHECK(parse_github_release(release_json("v2.3.8.5-edge"), Platform::Linux).download_url == page);
    CHECK(parse_github_release(release_json("v2.3.8.5-edge"), Platform::LinuxFlatpak).download_url == page);

    // Links outside the repository are never handed to the browser.
    const ReleaseInfo foreign = parse_github_release(release_json("v2.3.8.5-edge", false, false, "x", "https://evil.example/page",
                                                                  "https://evil.example/dl/"),
                                                     Platform::Windows);
    REQUIRE(foreign.ok);
    CHECK(foreign.html_url == GITHUB_RELEASES_PAGE);
    CHECK(foreign.download_url == GITHUB_RELEASES_PAGE);
}

TEST_CASE("markdown lines flatten to plain text", "[AppUpdate]")
{
    CHECK(markdown_line_to_plain("## What's new") == "What's new");
    CHECK(markdown_line_to_plain("# EdgeSlicer 2.4.0.0 #") == "EdgeSlicer 2.4.0.0");
    CHECK(markdown_line_to_plain("- **Bold** item with [a link](https://x.y/z)") == BULLET + "Bold item with a link");
    CHECK(markdown_line_to_plain("* *italic* and __strong__") == BULLET + "italic and strong");
    CHECK(markdown_line_to_plain("  - nested") == " " + BULLET + "nested");
    CHECK(markdown_line_to_plain("1. first") == "1. first");
    CHECK(markdown_line_to_plain("![screenshot](https://x/y.png)").empty());
    CHECK(markdown_line_to_plain("Uploads a `<name>.gcode.3mf` now") == "Uploads a <name>.gcode.3mf now");
    CHECK(markdown_line_to_plain("snake_case and 2*3 stay") == "snake_case and 2*3 stay");
    CHECK(markdown_line_to_plain("> Your data is copied, never moved.") == "Your data is copied, never moved.");
    CHECK(markdown_line_to_plain("---").empty());
    CHECK(markdown_line_to_plain("|---|:---:|").empty());
    CHECK(markdown_line_to_plain("Fish &amp; chips<br>today") == "Fish & chips today");
    CHECK(markdown_line_to_plain("<b>Bold</b> <!-- hidden --> text") == "Bold text");
    CHECK(markdown_line_to_plain("see <https://example.com>") == "see https://example.com");
    CHECK(markdown_line_to_plain("escaped \\*stars\\*") == "escaped *stars*");
}

TEST_CASE("compact notes use the update-notice section when present", "[AppUpdate]")
{
    const std::string body = "> Data is copied, never moved.\n\n"
                             "# EdgeSlicer 2.4.0.0\n\n"
                             "<!-- update-notice -->\n"
                             "**Highlights**\n"
                             "- Faster startup\n"
                             "- [Draw cut](https://github.com/x) on cylinders\n"
                             "<!-- /update-notice -->\n\n"
                             "## What's new\n\nLots of long text that must not appear.\n";
    const CompactNotes n = compact_release_notes(body);
    CHECK(n.from_markers);
    CHECK_FALSE(n.truncated);
    CHECK(n.text == "Highlights\n" + BULLET + "Faster startup\n" + BULLET + "Draw cut on cylinders");

    // Spacing and case inside the comments do not matter; CRLF bodies work.
    const CompactNotes loose = compact_release_notes("intro\r\n<!--Update-Notice-->\r\nOne line.\r\n<!--  / update-notice  -->\r\nrest\r\n");
    CHECK(loose.from_markers);
    CHECK(loose.text == "One line.");
}

TEST_CASE("compact notes fall back to the first lines of the body", "[AppUpdate]")
{
    // No markers: first 12 non-empty lines, markdown flattened, rest reported as truncated.
    std::string body = "![banner](https://x/banner.png)\n# EdgeSlicer 2.4.0.0\n\n";
    for (int i = 1; i <= 20; ++i) body += "- item " + std::to_string(i) + "\n";
    const CompactNotes n = compact_release_notes(body);
    CHECK_FALSE(n.from_markers);
    CHECK(n.truncated);
    CHECK(n.text.find("EdgeSlicer 2.4.0.0\n\n" + BULLET + "item 1\n") == 0);
    CHECK(n.text.find("item 11") != std::string::npos);
    CHECK(n.text.find("item 12") == std::string::npos); // heading + 11 items = 12 lines
    CHECK(n.text.find("banner") == std::string::npos);

    // A short body is shown whole.
    const CompactNotes shortn = compact_release_notes("Bug fixes.\n\n- one\n- two\n");
    CHECK_FALSE(shortn.truncated);
    CHECK(shortn.text == "Bug fixes.\n\n" + BULLET + "one\n" + BULLET + "two");

    // The character cap cuts at a word and marks the cut.
    std::string para;
    for (int i = 0; i < 300; ++i) para += "word ";
    const CompactNotes capped = compact_release_notes("Intro line.\n\n" + para + "\n\nMore.\n");
    CHECK(capped.truncated);
    CHECK(capped.text.size() <= 800 + 3 + 2); // budget + ellipsis + the paragraph break
    CHECK(capped.text.substr(capped.text.size() - 3) == "\xE2\x80\xA6");
    CHECK(capped.text.find("More.") == std::string::npos);

    // An empty marker section is ignored rather than showing nothing.
    const CompactNotes empty = compact_release_notes("Real notes.\n<!-- update-notice -->\n\n<!-- /update-notice -->\n");
    CHECK_FALSE(empty.from_markers);
    CHECK(empty.text == "Real notes.");

    // A begin marker without an end marker is not trusted either.
    const CompactNotes open = compact_release_notes("<!-- update-notice -->\nA\nB\n");
    CHECK_FALSE(open.from_markers);
    CHECK(open.text == "A\nB");
}

// Hits the real GitHub API. Hidden (the leading '.') so CI and the batch gates never depend on
// the network; run by hand with:  slic3rutils_tests "[AppUpdateLive]"
TEST_CASE("live: GitHub latest release parses and compares", "[.][AppUpdateLive]")
{
    std::string body, error;
    unsigned    status = 0;
    Http::get(GITHUB_LATEST_RELEASE_API)
        .clear_headers()
        .header("User-Agent", std::string(SLIC3R_APP_NAME "/") + Snapmaker_VERSION)
        .header("Accept", "application/vnd.github+json")
        .header("X-GitHub-Api-Version", "2022-11-28")
        .on_complete([&](std::string b, unsigned s) { body = std::move(b); status = s; })
        .on_error([&](std::string b, std::string e, unsigned s) { body = std::move(b); error = std::move(e); status = s; })
        .perform_sync();
    INFO("status " << status << " error " << error);
    REQUIRE(status == 200);

    const ReleaseInfo r = parse_github_release(body, current_platform());
    INFO(r.error);
    REQUIRE(r.ok);
    std::cout << "latest: " << r.tag << " -> " << r.version_str << (r.prerelease ? " (prerelease)" : "") << "\n"
              << "page:   " << r.html_url << "\n"
              << "file:   " << r.download_url << "\n"
              << "notes (" << (r.notes.from_markers ? "markers" : "fallback") << (r.notes.truncated ? ", truncated" : "")
              << "):\n" << r.notes.text << "\n"
              << "verdict vs " << Snapmaker_VERSION << ": " << int(evaluate(r, Snapmaker_VERSION))
              << ", vs 2.3.0.0: " << int(evaluate(r, "2.3.0.0")) << std::endl;

    CHECK(r.html_url.rfind(GITHUB_TRUSTED_URL_PREFIX, 0) == 0);
    CHECK(r.download_url.rfind(GITHUB_TRUSTED_URL_PREFIX, 0) == 0);
    CHECK(evaluate(r, "2.3.0.0") == Verdict::UpdateAvailable); // the "update available" path, live
    CHECK_FALSE(r.notes.text.empty());
}
