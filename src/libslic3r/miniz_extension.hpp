#ifndef MINIZ_EXTENSION_HPP
#define MINIZ_EXTENSION_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <miniz.h>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

bool open_zip_reader(mz_zip_archive* zip, const std::string& fname_utf8);
bool open_zip_writer(mz_zip_archive* zip, const std::string& fname_utf8);
bool close_zip_reader(mz_zip_archive* zip);
bool close_zip_writer(mz_zip_archive* zip);
// Entry name as UTF-8: the UTF-8 flagged name, else the Info-ZIP Unicode Path extra field, else the
// legacy name decoded with the system code page.
std::string decode_archive_entry_path(mz_zip_archive* zip, const mz_zip_archive_file_stat& stat);

// Unix symlink bit in the zip central-directory external attributes (high 16 bits).
bool zip_entry_is_symlink(const mz_zip_archive_file_stat& stat);

// Traditional PKWARE encryption (general-purpose bit 0 / strong-encryption bit 6), compression
// method 99, or a WinZip AES extra field (0x9901). `zip` may be null; the extra-field check is
// then skipped.
bool zip_entry_is_encrypted(mz_zip_archive* zip, const mz_zip_archive_file_stat& stat);

// True when `s` is well-formed UTF-8 (used to decide whether a legacy entry name must be escaped).
bool zip_name_is_valid_utf8(const std::string& s);

// Percent-escape bytes that are not ASCII letters, digits, '.', '_' or '-'. Used when an entry
// name cannot be decoded to UTF-8 (CP437 / Shift-JIS without a Unicode extra field, etc.).
std::string escape_zip_entry_bytes(const std::string& raw);

// Defuse Windows reserved device names (CON, NUL, COM1, ...) and the characters <>:"|?* in each
// path segment. Applied on every platform so an extracted tree stays portable.
std::string sanitize_zip_extract_path(const std::string& normalized_utf8);

// Caps for selective extraction (PR A). AppConfig keys that override these defaults arrive in PR B.
struct ZipExtractCaps
{
    std::uint32_t max_entries_scanned    = 10000;
    std::uint64_t max_entry_uncompressed = 2ull * 1024ull * 1024ull * 1024ull; // 2 GiB
    std::uint64_t max_total_uncompressed = 4ull * 1024ull * 1024ull * 1024ull; // 4 GiB
    std::uint64_t max_compression_ratio  = 200;                                // for entries over 1 MiB
    std::uint64_t ratio_min_uncompressed = 1ull * 1024ull * 1024ull;           // 1 MiB
};

// One central-directory entry after decode / safety / encryption checks. `extract_name` is the
// relative dest path (sanitized / escaped) when `is_safe`; otherwise it is empty.
struct ZipEntryInfo
{
    mz_uint       index = 0;
    std::string   raw_name;
    std::string   decoded_name;
    std::string   extract_name;
    std::uint64_t uncomp_size  = 0;
    std::uint64_t comp_size    = 0;
    std::uint32_t crc32        = 0;
    bool          is_directory = false;
    bool          is_symlink   = false;
    bool          is_encrypted = false;
    bool          is_safe      = false;
};

// Scan every central-directory entry (capped by `caps.max_entries_scanned`). Does not extract.
// A split / multi-disk archive is reported through `err` when the reader could not be opened
// by the path overload; an already-open reader is never a split zip (miniz refuses those).
bool inspect_zip_entries(mz_zip_archive& archive, const ZipExtractCaps& caps, std::vector<ZipEntryInfo>& out, std::string& err);

// Extracts every entry of an already-open reader under dest. Validates every entry first
// (is_safe_archive_relative_path + is_path_within_root, no symlink entries). Any bad entry
// rejects the whole archive and writes nothing (Orca #15957 / D3). Files are staged to a
// unique sibling (dest + ".part." + random) and renamed over the dest only after every entry
// has been staged, so a pass-2 failure never deletes or truncates a pre-existing dest. A
// dest-file symlink is replaced by that rename rather than written through. A failure partway
// through the commit (rename) phase can leave a mix of old and new files: nothing is truncated
// or deleted, but the result is not atomic across files.
bool extract_archive_confined(mz_zip_archive& archive, const boost::filesystem::path& dest, std::string& err);
bool extract_archive_confined(const boost::filesystem::path& zip_path, const boost::filesystem::path& dest, std::string& err);

// Extracts only the selected central-directory indices under dest. Same path checks and
// stage-then-rename guarantees as extract_archive_confined, plus zip-bomb caps, a capped
// streaming sink (a lying header cannot exceed the cap), CRC verification, free-disk check,
// encryption detection, and collision suffixes (`name(1).ext`). Any bad selected entry
// rejects the whole call and leaves no new files behind.
bool extract_entries_confined(mz_zip_archive&                archive,
                              const std::vector<mz_uint>&    indices,
                              const boost::filesystem::path& dest,
                              const ZipExtractCaps&          caps,
                              std::string&                   err);
bool extract_entries_confined(const boost::filesystem::path& zip_path,
                              const std::vector<mz_uint>&    indices,
                              const boost::filesystem::path& dest,
                              const ZipExtractCaps&          caps,
                              std::string&                   err);

// Extracts one archive entry to dest_path. On Windows the file is opened through the wide API
// (_wfopen): the path never goes through the ANSI code page, whose best-fit mapping turns
// fullwidth look-alikes (U+FF0E U+FF0E U+FF0F) into "../". Every extraction of an untrusted
// entry name must use this rather than encode_path() + mz_zip_reader_extract_to_file().
// The std::string overload takes a UTF-8 path.
bool extract_entry_to_file(mz_zip_archive& archive, mz_uint file_index, const boost::filesystem::path& dest_path);
bool extract_entry_to_file(mz_zip_archive& archive, mz_uint file_index, const std::string& dest_path_utf8);

// Reads one archive entry of a file we did not write into `out`, never trusting its header: the
// declared uncompressed size is compared with `cap` before anything is allocated, and the data
// is inflated through a sink that refuses to grow past `cap`, so an entry whose header lies
// about its size, or a zip bomb, cannot take more than `cap` bytes. Returns false (and `out`
// empty) when the entry is too large, cannot be inflated, or its size or CRC disagree with its
// header. `*why` gets a short reason. A zero-length entry yields true and an empty `out`.
bool read_zip_entry_capped(mz_zip_archive& archive, mz_uint file_index, std::uint64_t cap, std::string& out, std::string* why = nullptr);

class MZ_Archive
{
public:
    mz_zip_archive arch;

    MZ_Archive();
    ~MZ_Archive();

    MZ_Archive(const MZ_Archive&)            = delete;
    MZ_Archive& operator=(const MZ_Archive&) = delete;

    // Open and take ownership so the destructor closes the archive on every path.
    // Existing callers that open/close `arch` themselves are unchanged (`m_kind` stays None).
    bool open_reader(const std::string& fname_utf8);
    bool open_writer(const std::string& fname_utf8);
    bool close();

    static std::string get_errorstr(mz_zip_error mz_err);

    std::string get_errorstr() const { return get_errorstr(arch.m_last_error) + "!"; }

    bool is_alive() const { return arch.m_zip_mode != MZ_ZIP_MODE_WRITING_HAS_BEEN_FINALIZED; }

private:
    enum class Kind { None, Reader, Writer };
    Kind m_kind = Kind::None;
};

} // namespace Slic3r

#endif // MINIZ_EXTENSION_HPP
