#ifndef slic3r_ZipOpen_hpp_
#define slic3r_ZipOpen_hpp_

#include <cstddef>
#include <cstdint>
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
};

ZipEntryKind classify_zip_entry_name(const std::string& decoded_name, bool is_directory);
bool         zip_entry_name_is_ignored(const std::string& decoded_name, bool is_directory);
bool         zip_entry_is_selectable(ZipEntryKind kind, bool is_encrypted, bool is_symlink);

// True when the option is on and the archive has exactly one selectable entry.
bool should_auto_open_zip(std::size_t selectable_count, bool option_enabled);

// Every non-ignored file is encrypted (and at least one such file exists).
bool zip_archive_is_all_encrypted(const std::vector<ZipListedEntry>& entries);

// Same-folder `.mtl` (matching stem) plus image files next to a selected `.obj`.
std::vector<std::uint32_t> zip_obj_sidecar_indices(const ZipListedEntry& obj, const std::vector<ZipListedEntry>& all);

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
