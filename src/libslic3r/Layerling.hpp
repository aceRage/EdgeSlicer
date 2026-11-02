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

// Layerling's own "Install layerling as an app" banner, which points at browser controls our view
// does not have (and an installed app would keep its designs apart from the tab's). The banner is
// skipped when localStorage "layerling.installHintDismissed" is "1" - what its own Dismiss writes
// (checked in the 1.131.1 bundle) - so this script writes that flag before the page's scripts run,
// in the top frame only. Unlike pretending to be an installed app (display-mode: standalone), it
// changes nothing else in Layerling.
constexpr const char *INSTALL_HINT_DISMISSED_KEY = "layerling.installHintDismissed";
const char *suppress_install_hint_script();

// Everything the tab runs before the page's own scripts: the two above, each in its own try block.
std::string document_start_script();

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

// ---- the source: the built-in copy or the user's own Layerling ---------------------------------
//
// "Use my own Layerling": the tab can show a Layerling the user already runs (Docker on a NAS, a
// home server, a static copy on a web server, a dev server, or https://layerling.com) instead of
// the downloaded copy. Designs live in the browser storage of the page's origin, so every source
// has its own designs; a self-hosted server with a shared project folder
// (LAYERLING_SHARED_PROJECTS_DIR) keeps them on the server, the same on every device.

enum class Source {
    BuiltIn, // the downloaded copy, served from 127.0.0.1
    Custom   // app_config "layerling_base_url"
};

// app_config "layerling_source" ("builtin" / "custom") and "layerling_base_url". Phase 1 had only
// the base URL (no UI): a usable base URL without a source key counts as Custom. Custom without a
// usable base URL is BuiltIn.
Source      select_source(const std::string &source_key, const std::string &base_url);
const char *source_key(Source source);

struct CustomUrl
{
    bool        ok = false;
    std::string url;                  // "scheme://host[:port]/"
    bool        path_dropped = false; // the input had a path, query or fragment
    std::string error;                // why not ok
};
// What the user typed -> the base URL the tab loads. Blanks trimmed; without a scheme, http:// is
// assumed; only http and https; no "user:password@"; a host is required. Layerling's files refer
// to each other from the root of the address (/_next/...), so it only runs at the root: a path,
// query or fragment is dropped (path_dropped says so).
CustomUrl normalize_custom_url(const std::string &input);

// Where a Layerling the user runs on this computer usually answers: its dev server and its Docker
// image listen on 3000 (README "Install and Run", "Docker"), the start script's documented
// alternative is 3100, and the README's static-hosting example uses 8080. 127.0.0.1 and localhost
// are different origins (different designs), so both are offered for 3000. Only these are tried:
// no other addresses, no network scan.
std::vector<std::string> local_candidates();

// Layerling's /version.json (static export: {"version":"1.131.1","build":...}). False when the
// body is not that.
bool parse_version_json(const std::string &body, std::string &version);

struct ProbeResult
{
    bool        ok = false;
    std::string version; // "" when the address answers but publishes no version (server builds)
    std::string error;
};
// Is this address a Layerling? Its /version.json (the static export has one), else its start page
// (the Node server and the Docker image have no version.json): a page that names layerling and
// loads Next.js files. A status of 0 means no answer.
ProbeResult classify_probe(unsigned version_status, const std::string &version_body, unsigned page_status, const std::string &page_body);

// ---- designs on the plate (Edit in Layerling, the round trip) ----------------------------------

// The Layerling design an object on the plate came from. Kept with the object
// (ModelObject::layerling_design, saved in the project 3MF) so "Edit in Layerling" opens that
// design again and Send to Plate can offer to replace the object. A project id belongs to the
// origin's storage: on another origin the link does not apply.
struct DesignLink
{
    std::string origin;  // as origin_of() returns it
    std::string project; // Layerling's project id (its "?editor=1&project=<id>")
    std::string name;    // the design's name when it was sent, for messages
    bool        valid() const;
};
// "v=1&origin=<enc>&project=<enc>&name=<enc>" (percent-encoded), "" for an invalid link.
std::string serialize_link(const DesignLink &link);
// False (and `out` cleared) for anything that is not a valid link.
bool parse_link(const std::string &text, DesignLink &out);
// A Layerling project id: 1-128 characters of [A-Za-z0-9_.-].
bool is_safe_project_id(const std::string &id);
// The project an editor page shows: "<origin>/?editor=1&project=<id>" -> <id>; "" otherwise.
std::string project_from_page_url(const std::string &url);
// The page that opens `project` in the editor ("" project: a new design): <origin>/?editor=1[&project=<id>].
// "" when the base has no origin or the id is not safe.
std::string editor_url(const std::string &base_url, const std::string &project);

enum class EditFormat {
    OpenLinked, // the object came from a design of this origin: open that design, nothing exported
    Step,       // every part has exact CAD geometry (imported from STEP, or a CAD body)
    ThreeMF,    // several colours: one body per part, with its filament colour
    Stl         // one colour
};
struct EditSource
{
    bool        linked_here = false; // a DesignLink of the current origin
    bool        all_exact   = false; // every part exportable as an exact B-rep
    std::size_t colours     = 0;     // distinct filament colours of the parts
};
EditFormat  choose_edit_format(const EditSource &source);
// "Benchy.stl" -> "Benchy": drops a model file extension (3mf stl obj step stp amf lyl), case-insensitive.
std::string strip_model_extension(const std::string &name);
const char *edit_format_extension(EditFormat format); // ".step" ".3mf" ".stl", "" for OpenLinked

struct ColouredMesh
{
    std::string             name;
    std::string             colour; // "#RRGGBB"; "" -> grey
    std::vector<float>      vertices; // x, y, z, x, y, z, ... in millimetres
    std::vector<int>        triangles; // three vertex indices per triangle
};
// A plain core-spec 3MF: one object per body, coloured through <basematerials> displaycolor (what
// Layerling reads as "one body per colour"; parse_3mf_body_colours reads it back).
bool write_colour_3mf(const boost::filesystem::path &file, const std::vector<ColouredMesh> &bodies, std::string *error = nullptr);

// ---- handing a file to Layerling -----------------------------------------------------------------
//
// Layerling has no host API (no postMessage hook, no import URL). Its start page keeps a hidden
// multi-file input (accept list with .lyl and .3mf; rendered under the dashboard and the editor
// alike) whose change event imports the chosen files into a NEW design and opens that design in
// the editor - what its own "Import" on the start page does (checked in the 1.131.1 bundle).
// The tab hands a file over the same way a user picking it would: the app runs a script in the page
// that builds a File from the bytes, puts it into that input and fires "change". Nothing of
// Layerling's is changed and the page gets no way to call the app (the script is one-way).
constexpr const char *SCRIPT_READY    = "edgeslicer-ready";
constexpr const char *SCRIPT_WAIT     = "edgeslicer-wait";
constexpr const char *SCRIPT_OK       = "edgeslicer-ok";
constexpr const char *SCRIPT_NO_INPUT = "edgeslicer-noinput";
// Bigger files are not pushed through a script; the user imports them from the folder instead.
constexpr std::uint64_t MAX_HANDOVER_BYTES = std::uint64_t(64) * 1024 * 1024;
// Returns SCRIPT_READY once the import input exists, else SCRIPT_WAIT.
std::string import_probe_script();
// Imports `bytes` as a file called `name`; returns SCRIPT_OK, or SCRIPT_NO_INPUT when the input is
// missing. The name and bytes are embedded as a JSON string and base64.
std::string import_file_script(const std::string &name, const std::string &bytes);
std::string base64_encode(const std::string &bytes);
// A JSON / JavaScript string literal, quotes included; U+2028/9 escaped too, and with `html_safe`
// '<' and '>' (for a script; not for text a person reads, like the MCP setup).
std::string js_string_literal(const std::string &text, bool html_safe = true);

// ---- AI clients (MCP) ----------------------------------------------------------------------------
//
// Layerling's MCP server (scripts/layerling-mcp-server.mjs in its source, run by the AI client
// with Node) drives open editor pages through the route /api/layerling-mcp of a Layerling *server*:
// the dev server ("npm run dev", http://127.0.0.1:3000), or a self-hosted server with
// LAYERLING_MCP_REMOTE and LAYERLING_MCP_TOKEN set. The static web build (what the tab downloads)
// and layerling.com have no such route.
enum class McpClient {
    Json, // Claude Desktop, Claude Code (.mcp.json), Gemini CLI, Antigravity: {"mcpServers": ...}
    Codex // Codex config.toml: [mcp_servers.layerling]
};
constexpr const char *MCP_ROUTE                = "/api/layerling-mcp";
constexpr const char *MCP_DEFAULT_URL          = "http://127.0.0.1:3000";
constexpr const char *MCP_SCRIPT_PLACEHOLDER   = "/absolute/path/to/layerling/scripts/layerling-mcp-server.mjs";
constexpr const char *MCP_TOKEN_PLACEHOLDER    = "<the LAYERLING_MCP_TOKEN set on the server>";
// What Layerling's MCP server counts as this computer (no token, it may open an editor itself).
bool is_local_bridge_origin(const std::string &origin);
// The server entry for the client's config: node + the script, LAYERLING_URL = `origin` (an origin
// that is not one -> MCP_DEFAULT_URL), and with `token`, LAYERLING_MCP_TOKEN with a placeholder
// (never a real token). `script_path` "" -> MCP_SCRIPT_PLACEHOLDER; backslashes become '/'.
std::string mcp_config_text(McpClient client, const std::string &origin, const std::string &script_path, bool token);
// The MCP server script inside a Layerling checkout folder.
std::string mcp_script_in_checkout(const std::string &checkout_dir);
enum class McpBridge { Open, NeedsToken, Off, Unknown };
// From the HTTP status of a GET on <origin>/api/layerling-mcp: 200 open, 401/403 needs the token
// (or answers only to localhost), 404 off (production build / static copy), anything else unknown.
McpBridge mcp_bridge_from_status(unsigned status);
// The client must send LAYERLING_MCP_TOKEN: always for a server elsewhere, and for one on this
// computer that answered as a production server with remote use switched on (401/403).
bool mcp_needs_token(const std::string &origin, McpBridge bridge);

} // namespace layerling
} // namespace Slic3r

#endif // slic3r_Layerling_hpp_
