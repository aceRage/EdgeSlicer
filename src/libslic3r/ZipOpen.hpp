#ifndef slic3r_ZipOpen_hpp_
#define slic3r_ZipOpen_hpp_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#ifndef ZIP_OPEN_STANDALONE
#include <boost/filesystem/path.hpp>
#endif

namespace Slic3r {

// AppConfig keys seeded in PR B. The caps stay hidden from the Preferences UI. The clean-up
// keys arrive with the cleaner (PR C, zip_work_cleanup*); the old zip_work_autoclean* seeds
// (off) are gone so a test build that wrote them cannot turn the cleaner off later.
inline constexpr const char* ZIP_WORK_DIR_KEY                   = "zip_work_dir";
inline constexpr const char* ZIP_OPEN_SINGLE_WITHOUT_ASKING_KEY = "zip_open_single_without_asking";
inline constexpr const char* ZIP_MAX_ENTRIES_SCANNED_KEY        = "zip_max_entries_scanned";
inline constexpr const char* ZIP_MAX_ENTRY_UNCOMPRESSED_KEY     = "zip_max_entry_uncompressed";
inline constexpr const char* ZIP_MAX_TOTAL_UNCOMPRESSED_KEY     = "zip_max_total_uncompressed";
inline constexpr const char* ZIP_MAX_COMPRESSION_RATIO_KEY      = "zip_max_compression_ratio";

// Model / project extensions that File > Open lists (FT_MODEL) minus `.zip`.
// The archive picker uses this same list so it cannot drift from a second regex.
// Apple adds the usd* / abc / ply formats that FT_MODEL already accepts there.
const std::vector<std::string_view>& zip_model_extensions();
// G-code listed inside a zip. `.gcode.3mf` is a 3MF project (ends with `.3mf`) and
// is therefore a Model, not a G-code; it loads through the existing 3MF path.
const std::vector<std::string_view>& zip_gcode_extensions();
const std::vector<std::string_view>& zip_sidecar_image_extensions();

enum class ZipEntryKind {
    Model,
    GCode,
    NestedZip,
    Ignored,
    Unsupported
};

// Name + flags used by the pure classifiers (no miniz / ZipEntryInfo dependency).
struct ZipListedEntry
{
    std::uint32_t index         = 0;
    std::string   decoded_name;
    bool          is_directory  = false;
    bool          is_encrypted  = false;
    bool          is_symlink    = false;
    // inspect_zip_entries' path verdict (no traversal / absolute / drive / NUL).
    bool          is_safe       = true;
};

ZipEntryKind classify_zip_entry_name(const std::string& decoded_name, bool is_directory);
bool         zip_entry_name_is_ignored(const std::string& decoded_name, bool is_directory);
// Only Model / G-code entries that are not encrypted, not symlinks and have a safe path can be
// ticked, so one `../x.stl` entry can never be selected and abort the whole extraction.
bool         zip_entry_is_selectable(ZipEntryKind kind, bool is_encrypted, bool is_symlink, bool is_safe);

// True when the option is on and the archive has exactly one selectable entry.
bool should_auto_open_zip(std::size_t selectable_count, bool option_enabled);

// Every non-ignored file is encrypted (and at least one such file exists).
bool zip_archive_is_all_encrypted(const std::vector<ZipListedEntry>& entries);

// `mtllib` file names in an OBJ. Quoted names are taken as written; an unquoted line yields the
// whole remainder (a name with spaces) and then each whitespace-separated name.
// One `mtllib` line: `whole` is the unquoted remainder when it has spaces (one name with spaces,
// tried first), `names` the separate (or quoted) names.
struct ZipMtllibLine
{
    std::string              whole;
    std::vector<std::string> names;
};
std::vector<ZipMtllibLine> parse_obj_mtllib_lines(const std::string& obj_text);
std::vector<std::string> parse_obj_mtllib(const std::string& obj_text);
// Texture file names referenced by an MTL (map_Kd, map_Ka, map_Ks, map_Ns, map_d, map_Bump,
// bump, disp, decal, refl, norm, map_Pr/Pm/Ps/Ke). Option flags before the name are skipped:
// word options (-cc/-clamp/-blendu/-blendv on|off, -imfchan, -type) take one token, numeric ones
// (-o/-s/-t up to 3, -mm 2, -bm/-boost/-texres 1) take the numbers that follow, unknown flags
// skip up to 3 numbers. A quoted name keeps its spaces. An unknown flag is
// also listed as the start of a name (`-x.png`), ahead of the name after the skipped flag.
std::vector<std::string> parse_mtl_texture_refs(const std::string& mtl_text);
// Archive-relative `dir/ref` with `.` / `..` folded and `\` as `/`. Empty when `..` climbs out.
std::string resolve_zip_relative(const std::string& dir, const std::string& ref);

// Reads one entry (capped by the caller); false when it cannot be read.
using ZipEntryReader = std::function<bool(std::uint32_t index, std::string& out)>;

// Sidecars of a selected `.obj`: the MTL files its `mtllib` lines name (relative to the OBJ,
// subfolders allowed), else the same-stem `.mtl` beside it, plus exactly the textures those MTL
// files reference (relative to the MTL, subfolders allowed). Nothing is pulled in by extension
// alone. 3MF and GLB embed their textures; external glTF buffers / images are not followed.
// Name matching follows the OBJ loader on the target platform: exact on macOS and Linux (where
// fopen of `Tex.png` does not find `tex.png` on a case-sensitive volume), case-insensitive
// fallback on Windows. An exact match always wins.
#ifdef _WIN32
inline constexpr bool ZIP_SIDECAR_CASE_INSENSITIVE = true;
#else
inline constexpr bool ZIP_SIDECAR_CASE_INSENSITIVE = false;
#endif
std::vector<std::uint32_t> zip_obj_sidecar_indices(const ZipListedEntry&              obj,
                                                   const std::vector<ZipListedEntry>& all,
                                                   const ZipEntryReader&              read_entry,
                                                   bool                               case_insensitive = ZIP_SIDECAR_CASE_INSENSITIVE);

// OBJ index -> its sidecars, for every OBJ among `objs`. Built once, off the UI thread, and
// shared by the archive dialog (size line) and the extraction list. `cancelled` (optional) is
// polled between OBJs.
using ZipSidecarMap = std::map<std::uint32_t, std::vector<std::uint32_t>>;
ZipSidecarMap zip_sidecar_map(const std::vector<std::uint32_t>&  objs,
                              const std::vector<ZipListedEntry>& all,
                              const ZipEntryReader&              read_entry,
                              const std::function<bool()>&       cancelled = {},
                              bool                               case_insensitive = ZIP_SIDECAR_CASE_INSENSITIVE);

// Extraction order: sidecars first, then the selected entries, each index once. The extractor
// names files in this order and only suffixes later collisions (`x(1).mtl`), so sidecars keep the
// names the OBJ / MTL text refers to and only selected models (opened through the dest map) can
// be suffixed. Sidecars are never renamed to fix a collision and their text is never rewritten.
std::vector<std::uint32_t> zip_extract_order(const std::vector<std::uint32_t>& selected, const std::vector<std::uint32_t>& sidecars);

// `.zip` but not `.zip.amf`.
bool is_zip_archive_path(const std::string& path);

// Default ZIP working folder (owner decision 2026-11-03): the platform's local, unsynced
// app-data / cache folder, never Documents (often OneDrive- or iCloud-synced).
//   Windows: %LOCALAPPDATA%/EdgeSlicer/Zip (else %USERPROFILE%/AppData/Local/EdgeSlicer/Zip)
//   macOS:   ~/Library/Caches/EdgeSlicer/Zip
//   Linux:   $XDG_CACHE_HOME/EdgeSlicer/Zip, else ~/.cache/EdgeSlicer/Zip (Flatpak sets
//            XDG_CACHE_HOME to its own ~/.var/app/<id>/cache)
// Then `app_data_dir/Zip`, then empty (the caller falls back to the temporary directory).
// Relative environment values are ignored. Pure, so every platform's rule is testable anywhere.
enum class ZipRootPlatform { Windows, MacOS, Linux };
struct ZipRootEnv
{
    std::string local_app_data; // %LOCALAPPDATA%
    std::string user_profile;   // %USERPROFILE%
    std::string home;           // $HOME
    std::string xdg_cache_home; // $XDG_CACHE_HOME
};
// UTF-8, with the platform's separator.
std::string zip_work_root_for(ZipRootPlatform platform, const ZipRootEnv& env, const std::string& app_data_dir_utf8);

#ifndef ZIP_OPEN_STANDALONE
bool running_in_flatpak();

ZipRootPlatform zip_root_current_platform();
ZipRootEnv      zip_root_env_from_process();

// zip_work_root_for with this process's platform and environment.
boost::filesystem::path default_zip_work_root(const boost::filesystem::path& app_data_dir = {});
// The AppConfig `zip_work_dir` value (UTF-8) when set, else default_zip_work_root.
boost::filesystem::path resolve_zip_work_root(const std::string& configured_utf8, const boost::filesystem::path& app_data_dir);

// File name a phone / hub save uses for a project opened from a ZIP (review B-2): no Save As
// dialog can be answered remotely, and the extracted copy sits in the auto-cleaned work folder,
// so the hub writes `<project stem>_<stamp>.3mf` into its own saves folder instead, the way it
// saves a project that was never saved. `project_filename_utf8` may be a full path; an empty or
// unusable stem gives `Untitled_<stamp>.3mf`.
std::string zip_remote_save_name(const std::string& project_filename_utf8, const std::string& stamp);

// Join a sanitized extract-relative path onto a session folder (UTF-8, `/` separators).
boost::filesystem::path zip_session_entry_dest(const boost::filesystem::path& session_dir, const std::string& extract_name);
#endif

// Floors for the hidden cap keys: a value below the floor (0 included) is raised to it, so a
// typo cannot make every archive fail (ratio 0 would refuse everything over 1 MiB).
inline constexpr std::uint32_t ZIP_MIN_ENTRIES_SCANNED    = 16;
inline constexpr std::uint64_t ZIP_MIN_ENTRY_UNCOMPRESSED = 1ull * 1024ull * 1024ull;
inline constexpr std::uint64_t ZIP_MIN_TOTAL_UNCOMPRESSED = 1ull * 1024ull * 1024ull;
inline constexpr std::uint64_t ZIP_MIN_COMPRESSION_RATIO  = 10;

struct ZipCapValues
{
    std::uint32_t max_entries_scanned    = 10000;
    std::uint64_t max_entry_uncompressed = 2ull * 1024ull * 1024ull * 1024ull;
    std::uint64_t max_total_uncompressed = 4ull * 1024ull * 1024ull * 1024ull;
    std::uint64_t max_compression_ratio  = 200;
};

ZipCapValues zip_extract_caps_from_values(const std::string& max_entries_scanned,
                                          const std::string& max_entry_uncompressed,
                                          const std::string& max_total_uncompressed,
                                          const std::string& max_compression_ratio);

std::string format_zip_size(std::uint64_t bytes);

} // namespace Slic3r

#endif // slic3r_ZipOpen_hpp_
