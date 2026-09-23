#ifndef slic3r_Utils_AppUpdateCheck_hpp_
#define slic3r_Utils_AppUpdateCheck_hpp_

// EdgeSlicer "new version available" check, GUI-independent part.
//
// Everything that decides *whether* to show the new-version dialog and *what* it says lives
// here, so it can be unit tested without wxWidgets (tests/slic3rutils/app_update_check_tests.cpp).
// GUI_App::check_new_version_sf() does the HTTP request and shows the dialog.
//
// Default source: the GitHub "latest release" of aceRage/EdgeSlicer. The self-hosting override
// (AppConfig "orca_upgrade_url", the old Snapmaker JSON schema) is still handled in GUI_App.cpp.
// See docs/update-server/README.md.

#include <array>
#include <string>

namespace Slic3r {
namespace AppUpdate {

// The GitHub REST endpoint for the newest published, non-draft, non-prerelease release.
extern const char* const GITHUB_LATEST_RELEASE_API;
// Only URLs under this prefix are handed to the browser from a GitHub answer (download
// asset or release page). The HTTP client runs with TLS peer verification off, so this
// keeps a tampered answer from pointing the Download button anywhere else.
extern const char* const GITHUB_TRUSTED_URL_PREFIX;
// Where Download goes when the answer carries no usable link.
extern const char* const GITHUB_RELEASES_PAGE;

// Release-body markers delimiting the compact notes shown in the dialog. HTML comments,
// so they are invisible on the GitHub release page.
extern const char* const NOTICE_BEGIN_MARKER; // "<!-- update-notice -->"
extern const char* const NOTICE_END_MARKER;   // "<!-- /update-notice -->"

// A release version: up to four dot-separated numbers ("2.4.0.0", "2.3.9" == "2.3.9.0"),
// plus an optional prerelease tag. The "-edge" fork suffix and a leading "v" are not part of
// the version: "v2.4.0.0-edge" parses to 2.4.0.0 with no prerelease.
struct Version
{
    std::array<int, 4> parts { { 0, 0, 0, 0 } };
    std::string        prerelease; // e.g. "beta1" for "v2.4.0.0-beta1-edge"; empty for a release
    bool               valid { false };

    std::string to_string() const; // "2.4.0.0" or "2.4.0.0-beta1"
};

// Lenient parser for tags and version strings. Returns an invalid Version for anything that
// does not start with 2-4 numeric components.
Version parse_version(const std::string& text);

// -1, 0, 1. Numbers first; with equal numbers a release is newer than any prerelease, and
// two prereleases compare by their tag text. An invalid version is older than everything.
int compare_versions(const Version& a, const Version& b);

// True when `candidate` is at or below the version the user chose to skip. An empty or
// unparsable skip value never skips.
bool is_skipped(const std::string& candidate, const std::string& skip_version);

enum class Platform
{
    Windows,
    MacOS,
    Linux,        // AppImage / tarball: the Download button opens the release page
    LinuxFlatpak, // updates come through Flathub; the check is suppressed
};

// The platform this binary runs on (Flatpak detected via $FLATPAK_ID or /.flatpak-info).
Platform current_platform();

// Compact release notes for the dialog: plain text with bullets kept.
struct CompactNotes
{
    std::string text;
    bool        from_markers { false }; // the body carried an update-notice section
    bool        truncated { false };    // text is shorter than what was there to show
};

// Picks the update-notice section when present (capped generously), otherwise the first
// `max_lines` non-empty lines of the body, capped at `max_chars`. Markdown is flattened to
// plain text: headings lose their #, images disappear, links keep their text, emphasis and
// inline code lose their markers, list items become U+2022 bullets, HTML comments/tags go.
CompactNotes compact_release_notes(const std::string& body, size_t max_lines = 12, size_t max_chars = 800);

// One markdown line flattened to plain text (exposed for the tests).
std::string markdown_line_to_plain(const std::string& line);

// What a GitHub /releases/latest answer says, reduced to what the dialog needs.
struct ReleaseInfo
{
    bool        ok { false };
    std::string error;          // why ok == false
    std::string tag;            // "v2.4.0.0-edge"
    Version     version;        // parsed from tag
    std::string version_str;    // version.to_string(), what the dialog shows and skip_version stores
    std::string html_url;       // release page (validated)
    std::string download_url;   // per-platform asset, or html_url
    bool        draft { false };
    bool        prerelease { false }; // GitHub flag, or a prerelease tag in the version
    CompactNotes notes;
};

ReleaseInfo parse_github_release(const std::string& json_body, Platform platform);

enum class Verdict
{
    UpdateAvailable,
    UpToDate,
    Ignored, // draft or prerelease
    Invalid, // unparsable answer or version
};

// Compares a parsed release against the running build's version (Snapmaker_VERSION).
Verdict evaluate(const ReleaseInfo& release, const std::string& local_version);

// Debug aid, off unless set: $EDGESLICER_FAKE_LOCAL_VERSION replaces the local version the
// update check compares against (e.g. "2.3.0.0" to see the dialog for the current release).
// Returns `local_version` unchanged when the variable is unset or unparsable.
std::string effective_local_version(const std::string& local_version);

} // namespace AppUpdate
} // namespace Slic3r

#endif // slic3r_Utils_AppUpdateCheck_hpp_
