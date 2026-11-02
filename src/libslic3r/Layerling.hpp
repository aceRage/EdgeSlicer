#ifndef slic3r_Layerling_hpp_
#define slic3r_Layerling_hpp_

// The Layerling tab (src/slic3r/GUI/LayerlingPanel.*): the parts that need no GUI, kept here so
// they are unit tested (tests/libslic3r/test_layerling.cpp).
//
// Layerling (https://github.com/henmedia/layerling, AGPL-3.0, by henmedia) is a browser CAD for 3D
// printing. EdgeSlicer neither bundles nor modifies it: on request it downloads the official
// static web build from Layerling's GitHub releases into <data dir>/layerling/<version>/, serves
// those files unchanged from 127.0.0.1, shows them in a web view of its own, and takes the files
// Layerling exports (3MF / STL / OBJ / STEP) onto the plate ("Send to Plate").
//
//  - Release JSON (GitHub's releases/latest): which asset to fetch, its size and digest.
//  - Version strings: comparison for the "Update to vX" offer, and which ones may name a folder.
//  - The archive: limits checked before anything is unpacked; the extraction itself is
//    miniz_extension's extract_archive_confined (no entry can land outside the version folder).
//  - The local server: URL path -> file under the app folder, MIME types, cache headers.
//  - FUNDING.yml: Layerling's own funding links, shown on the download prompt and in the tab.
//  - 3MF colours: Layerling writes one body per colour (core 3MF basematerials / m:colorgroup);
//    the bridge maps each colour to a filament.

#include <cstdint>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace layerling {

constexpr const char *REPO_URL            = "https://github.com/henmedia/layerling";
constexpr const char *LICENSE_URL         = "https://github.com/henmedia/layerling/blob/main/LICENSE";
constexpr const char *LICENSE_NAME        = "AGPL-3.0";
constexpr const char *AUTHOR              = "henmedia";
constexpr const char *LATEST_RELEASE_API  = "https://api.github.com/repos/henmedia/layerling/releases/latest";
constexpr const char *FUNDING_YML_URL     = "https://raw.githubusercontent.com/henmedia/layerling/main/.github/FUNDING.yml";
// Where a release asset may come from (GitHub then redirects to its download host).
constexpr const char *ASSET_URL_PREFIX    = "https://github.com/henmedia/layerling/releases/download/";

// The static build is ~50 MB zipped and ~130 MB unpacked (v1.131.1, 703 files). Generous caps that
// still refuse a runaway download or a zip bomb.
constexpr std::uint64_t MAX_ARCHIVE_BYTES  = std::uint64_t(512) * 1024 * 1024;
constexpr std::uint64_t MAX_UNPACKED_BYTES = std::uint64_t(2048) * 1024 * 1024;
constexpr std::size_t   MAX_ARCHIVE_ENTRIES = 50000;

// ---- releases ----------------------------------------------------------------------------------

struct ReleaseInfo
{
    std::string   version;      // "1.131.1" (tag without a leading 'v')
    std::string   tag;          // "v1.131.1"
    std::string   published_at; // ISO 8601, as GitHub sends it
    std::string   html_url;     // the release page
    std::string   asset_name;   // "layerling-1.131.1-static.zip"
    std::string   asset_url;    // browser_download_url, always under ASSET_URL_PREFIX
    std::uint64_t asset_size = 0;
    std::string   sha256;       // 64 lower-case hex digits from the asset's "digest", "" when GitHub gave none
};

// Parses GitHub's "releases/latest" JSON. Picks the versioned "layerling-<version>-static.zip"
// asset, else "layerling-static.zip", else any "*-static.zip". Refuses drafts and pre-releases,
// a tag that is not a safe version, an asset URL outside ASSET_URL_PREFIX, an asset with no size
// or above MAX_ARCHIVE_BYTES, and a malformed digest. *error gets a short reason.
bool parse_release_json(const std::string &json, ReleaseInfo &out, std::string *error = nullptr);

// "sha256:<64 hex>" -> the hex digits in lower case; "" for anything else.
std::string parse_sha256_digest(const std::string &digest);

// "v1.2.3" -> "1.2.3"; surrounding blanks dropped.
std::string normalize_version(const std::string &version);

// A version that may name a folder: 1-64 characters of [0-9A-Za-z._-], starting with a digit
// or letter, not "." or "..", and no ".." anywhere.
bool is_safe_version(const std::string &version);

// Compares dotted versions numerically ("1.131.10" > "1.131.9"); a leading 'v' is ignored, missing
// parts count as 0, and a pre-release suffix ("1.2.0-beta.1") sorts before the release itself.
// Returns <0, 0 or >0.
int compare_versions(const std::string &a, const std::string &b);

// At most one release check a day: true when 24 hours have passed since `last_check` (seconds
// since the epoch), when there was none (0) or when it lies in the future (a clock change).
bool update_check_due(std::int64_t last_check, std::int64_t now);

// ---- the archive -------------------------------------------------------------------------------

struct ArchiveCheck
{
    bool          ok = false;
    std::string   error;
    std::size_t   entries  = 0;
    std::uint64_t unpacked = 0;
    // "" when index.html sits at the root of the archive, else the single top folder that holds it
    // ("layerling-1.131.1/").
    std::string   root_prefix;
};

// Opens the zip and checks it before anything is written: entry count and total unpacked size
// against the caps, no symlink entries, and an index.html at the root (or under one top folder).
ArchiveCheck check_archive(const boost::filesystem::path &zip_path);

// Unpacks a checked archive into <versions_dir>/<version>/ through a temporary sibling folder, so
// a failed or interrupted install never leaves a half-filled version folder behind. Never writes
// outside versions_dir (extract_archive_confined refuses the whole archive on any bad entry). An
// existing folder of that version is replaced only after the new one is complete.
// `app_root` gets the folder that holds index.html.
bool install_archive(const boost::filesystem::path &zip_path, const boost::filesystem::path &versions_dir, const std::string &version,
                     boost::filesystem::path &app_root, std::string &error);

// The folder that holds index.html inside an installed version folder (the folder itself or its
// single subfolder), or an empty path when there is none.
boost::filesystem::path find_app_root(const boost::filesystem::path &version_dir);

// ---- the local server --------------------------------------------------------------------------

// Content type for a file name, by extension (case-insensitive). Covers what the static build
// ships: html, js/mjs, css, json, webmanifest, wasm, svg, png, jpg, webp, gif, ico, fonts, txt, xml.
// "application/octet-stream" for anything else.
const char *mime_type_for(const std::string &file_name);

// Cache-Control for a file the server sends, by its path relative to the app folder: content-hashed
// build files (_next/static/...) are immutable, the pages, sw.js and version.json must be
// revalidated so an update shows at once.
const char *cache_control_for(const std::string &relative_path);

// The files a request may be answered with, in order of preference, as paths relative to the app
// folder ('/' separated): "/" -> index.html; "/guide" -> guide, guide.html, guide/index.html;
// "/a/" -> a/index.html. The query and fragment are ignored. `target` is already percent-decoded.
// Empty, with *forbidden set, for anything that could leave the folder or is odd: no leading '/',
// a '\\', ':' or control character, a "." or ".." segment, or a PHP file (the static build ships
// store.php for PHP hosts; there is no PHP here and its source must not be served).
std::vector<std::string> candidate_files(const std::string &target, bool *forbidden = nullptr);

// Origin ("scheme://host[:port]", lower case, default ports dropped) of an absolute http(s) URL, or
// "" when it is not one.
std::string origin_of(const std::string &url);

// ---- the web view --------------------------------------------------------------------------------

enum class Navigation {
    Allow,    // stays in the tab: the allowed origin, about:blank/srcdoc, blob: and data: (exports, workers)
    External, // opens in the system browser (http/https elsewhere, mailto:)
    Block     // anything else (file:, javascript:, custom schemes, ...)
};
// `allowed_origin` as origin_of() returns it.
Navigation decide_navigation(const std::string &url, const std::string &allowed_origin);

// Script run in every page before its own: hides window.showSaveFilePicker so exports are
// downloads (which the tab hands to the plate) rather than writes through a save dialog.
const char *hide_save_picker_script();

// The page URL for a base URL: the base itself, with a trailing '/' added to a bare origin.
std::string start_url(const std::string &base_url);

// SHA-256 over a stream of data (the download is hashed as it arrives).
class Sha256
{
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256 &)            = delete;
    Sha256 &operator=(const Sha256 &) = delete;
    void        update(const void *data, std::size_t size);
    std::string hex_digest(); // 64 lower-case hex digits; the object is spent afterwards
private:
    void *m_ctx;
};

// ---- funding -----------------------------------------------------------------------------------

struct FundingLink
{
    std::string label; // "GitHub Sponsors", "Ko-fi", "PayPal", ...
    std::string url;   // https only
};

// Reads a GitHub FUNDING.yml (github / ko_fi / patreon / open_collective / liberapay /
// buy_me_a_coffee / polar / custom; a value or a [list]; commented-out and empty keys skipped).
// Names that are not plain account names and custom links that are not https are dropped.
std::vector<FundingLink> parse_funding_yml(const std::string &text);

// The links in Layerling's FUNDING.yml when this was written, used when it cannot be fetched.
std::vector<FundingLink> default_funding_links();

// ---- what the tab hands to EdgeSlicer -----------------------------------------------------------

enum class ExportKind {
    Model,   // .3mf .stl .obj .step .stp: goes onto the plate
    Project, // .lyl / .skf: a Layerling project file, offered for saving
    Other    // anything else: offered for saving
};
ExportKind classify_export(const std::string &file_name);

// A file name for something the page downloads: the last path segment of the suggested name with
// characters that are not allowed in file names replaced, at most 120 characters, never empty
// ("layerling-export" + the extension when nothing is left).
std::string sanitize_export_name(const std::string &suggested);

struct BodyColour
{
    std::string object_id;
    std::string name;
    std::string colour; // "#RRGGBB" upper case, "" when the body has none
};

// The bodies of a 3MF model part (3D/3dmodel.model) in document order of the objects the build
// refers to, with each body's colour: the object's pid/pindex into a <basematerials> (displaycolor)
// or an <m:colorgroup> (color), else its first triangle's pid/p1. Objects that are only components
// of others are left out. False on malformed XML.
bool parse_3mf_body_colours(const std::string &model_xml, std::vector<BodyColour> &out, std::string *error = nullptr);

// Same, read from a .3mf file (the model entry is read with a size cap).
bool read_3mf_body_colours(const boost::filesystem::path &file, std::vector<BodyColour> &out, std::string *error = nullptr);

// The distinct non-empty colours in order of first appearance.
std::vector<std::string> distinct_colours(const std::vector<BodyColour> &bodies);

// For each colour, the 1-based filament to print it with: an existing filament of the same colour
// (compared case-insensitively, alpha ignored) when there is one, else a new filament appended after
// `existing` (numbered from existing.size() + 1). `added` gets the colours of the new filaments, in
// order. `max_filaments` caps the total; colours that do not fit share the closest filament.
std::vector<int> map_colours_to_filaments(const std::vector<std::string> &colours, const std::vector<std::string> &existing,
                                          std::size_t max_filaments, std::vector<std::string> &added);

} // namespace layerling
} // namespace Slic3r

#endif // slic3r_Layerling_hpp_
