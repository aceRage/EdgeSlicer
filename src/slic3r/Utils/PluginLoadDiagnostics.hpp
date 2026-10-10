#ifndef slic3r_Utils_PluginLoadDiagnostics_hpp_
#define slic3r_Utils_PluginLoadDiagnostics_hpp_

#include <cstdint>
#include <string>

namespace Slic3r {

// Why the network plug-in (bambu_networking / BambuSource) did not load, in the terms the user can
// act on. Decided from the facts the loader can see, so the decision is a pure function and is
// tested without a DLL, a Windows machine or a security policy.
//
// Before this existed the app said "installed but not loaded yet, please restart" for every failure.
// A restart only helps when the plug-in was copied into place after the start-up load point; when
// Windows blocks the DLL (Smart App Control / App Control for Business / antivirus) or the file is
// damaged, every restart fails the same way.
enum class PluginLoadFailureKind {
    None,              // no load attempt has failed this session
    Blocked,           // Windows or an antivirus refused the image (code integrity / policy / AV)
    Missing,           // the file is not there, or it is empty (quarantined / truncated)
    MissingDependency, // the file is there but a DLL it imports is not (e.g. the VC++ runtime)
    BadImage,          // not a valid image for this process (corrupt, wrong architecture, not a DLL)
    Incompatible,      // it loaded, but reports a version this EdgeSlicer cannot use
    Other,             // anything else
};

// Everything recorded about the last failed load attempt.
struct PluginLoadFailure
{
    PluginLoadFailureKind kind        = PluginLoadFailureKind::None;
    unsigned long         code        = 0;     // GetLastError() right after LoadLibrary (Windows); 0 elsewhere
    unsigned long         nt_status   = 0;     // RtlGetLastNtStatus() right after LoadLibrary (Windows), 0 if unknown
    std::string           library;             // full path that was tried
    std::string           detail;              // FormatMessage / dlerror text
    std::string           found_version;       // Incompatible: the version the plug-in reported
    std::string           expected_version;    // Incompatible: the version this EdgeSlicer needs
    bool                  file_exists = false; // at the time of the attempt
    std::uint64_t         file_size   = 0;     // at the time of the attempt
};

// Win32 error codes (the values LoadLibrary leaves in GetLastError) that the classifier knows.
// Spelled out here so the pure function and its tests build on every platform.
namespace plugin_load_codes {
constexpr unsigned long kFileNotFound             = 2;    // ERROR_FILE_NOT_FOUND
constexpr unsigned long kPathNotFound             = 3;    // ERROR_PATH_NOT_FOUND
constexpr unsigned long kAccessDenied             = 5;    // ERROR_ACCESS_DENIED
constexpr unsigned long kBadFormat                = 11;   // ERROR_BAD_FORMAT
constexpr unsigned long kModNotFound              = 126;  // ERROR_MOD_NOT_FOUND
constexpr unsigned long kProcNotFound             = 127;  // ERROR_PROC_NOT_FOUND
constexpr unsigned long kInvalidExeSignature      = 191;  // ERROR_INVALID_EXE_SIGNATURE
constexpr unsigned long kExeMarkedInvalid         = 192;  // ERROR_EXE_MARKED_INVALID
constexpr unsigned long kBadExeFormat             = 193;  // ERROR_BAD_EXE_FORMAT ("%1 is not a valid Win32 application")
constexpr unsigned long kExeMachineTypeMismatch   = 216;  // ERROR_EXE_MACHINE_TYPE_MISMATCH
constexpr unsigned long kVirusInfected            = 225;  // ERROR_VIRUS_INFECTED
constexpr unsigned long kVirusDeleted             = 226;  // ERROR_VIRUS_DELETED
constexpr unsigned long kInvalidImageHash         = 577;  // ERROR_INVALID_IMAGE_HASH (0x241)
constexpr unsigned long kImageMachineTypeMismatch = 706;  // ERROR_IMAGE_MACHINE_TYPE_MISMATCH
constexpr unsigned long kAccessDisabledNoSafer    = 786;  // ERROR_ACCESS_DISABLED_NO_SAFER_UI_BY_POLICY
constexpr unsigned long kAccessDisabledByPolicy   = 1260; // ERROR_ACCESS_DISABLED_BY_POLICY
constexpr unsigned long kSystemIntegrityViolation = 4551; // ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION (0x11C7)

// NTSTATUS values that mean "blocked" (what the Bad Image dialog prints).
constexpr unsigned long kStatusInvalidImageHash         = 0xC0000428UL; // STATUS_INVALID_IMAGE_HASH
constexpr unsigned long kStatusVirusInfected            = 0xC0000906UL; // STATUS_VIRUS_INFECTED
constexpr unsigned long kStatusVirusDeleted             = 0xC0000907UL; // STATUS_VIRUS_DELETED
constexpr unsigned long kStatusSystemIntegrityViolation = 0xC0E90002UL; // STATUS_SYSTEM_INTEGRITY_POLICY_VIOLATION (Smart App Control / App Control)
} // namespace plugin_load_codes

// The classification. win32_code is GetLastError() after a failed LoadLibrary, nt_status the
// NTSTATUS behind it when known (0 if not). file_exists / file_size describe the library file at
// that moment. An absent or empty file is "missing" whatever the code says: an antivirus that
// quarantines the DLL leaves an empty file or none, and no other advice would help.
PluginLoadFailureKind classify_plugin_load_error(unsigned long win32_code,
                                                 unsigned long nt_status,
                                                 bool          file_exists,
                                                 std::uint64_t file_size);

// Which message the GUI shows. Restart is the one genuine "copied but not loaded until restart"
// case: nothing failed, or the only failure was "file missing" at start-up and the file has been
// put in place since (the first-run copier runs after the load point).
enum class PluginLoadMessage {
    Restart,
    Blocked,
    MissingDependency,
    MissingFile,
    BadImage,
    Incompatible,
    Other,
};

PluginLoadMessage plugin_load_message(PluginLoadFailureKind kind, bool file_exists_now, std::uint64_t file_size_now);

// "0x11C7" or, when the NTSTATUS is known and differs, "0x11C7, status 0xC0E90002". Shown to the
// user in parentheses and written to the log, so a support request carries the exact code.
std::string plugin_load_error_text(const PluginLoadFailure &failure);

// The last failed load this session (thread safe). The loader records, the GUI reads.
void              record_plugin_load_failure(const PluginLoadFailure &failure);
PluginLoadFailure last_plugin_load_failure();
void              clear_plugin_load_failure();

// Whether `utf8_path` is a regular file and how large it is, without throwing.
void plugin_file_facts(const std::string &utf8_path, bool &exists, std::uint64_t &size);

} // namespace Slic3r

#endif // slic3r_Utils_PluginLoadDiagnostics_hpp_
