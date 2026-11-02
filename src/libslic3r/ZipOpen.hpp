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

// AppConfig keys seeded in PR B. Caps stay hidden from the Preferences UI (PR C).
inline constexpr const char* ZIP_WORK_DIR_KEY                   = "zip_work_dir";
inline constexpr const char* ZIP_OPEN_SINGLE_WITHOUT_ASKING_KEY = "zip_open_single_without_asking";
inline constexpr const char* ZIP_MAX_ENTRIES_SCANNED_KEY        = "zip_max_entries_scanned";
inline constexpr const char* ZIP_MAX_ENTRY_UNCOMPRESSED_KEY     = "zip_max_entry_uncompressed";
inline constexpr const char* ZIP_MAX_TOTAL_UNCOMPRESSED_KEY     = "zip_max_total_uncompressed";
inline constexpr const char* ZIP_MAX_COMPRESSION_RATIO_KEY      = "zip_max_compression_ratio";
inline constexpr const char* ZIP_WORK_AUTOCLEAN_KEY             = "zip_work_autoclean";
inline constexpr const char* ZIP_WORK_AUTOCLEAN_DAYS_KEY        = "zip_work_autoclean_days";

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
std::vector<std::string> parse_obj_mtllib(const std::string& obj_text);
// Texture file names referenced by an MTL (map_Kd, map_Ka, map_Ks, map_Ns, map_d, map_Bump,
// bump, disp, decal, refl, norm, map_Pr/Pm/Ps/Ke). Option flags before the name are skipped:
// word options (-cc/-clamp/-blendu/-blendv on|off, -imfchan, -type) take one token, numeric ones
// (-o/-s/-t up to 3, -mm 2, -bm/-boost/-texres 1) take the numbers that follow, unknown flags
// skip up to 3 numbers. A quoted name keeps its spaces.
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

#ifndef ZIP_OPEN_STANDALONE
bool running_in_flatpak();

// `<documents>/EdgeSlicer/Zip`. When `documents_dir` is empty or missing: XDG
// Documents, then ~/Documents, then $HOME. Flatpak without a usable Documents
// folder falls back to `app_data_dir/Zip` when `app_data_dir` is set.
boost::filesystem::path default_zip_work_root(const boost::filesystem::path& documents_dir,
                                              const boost::filesystem::path& app_data_dir = {});

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
