#include "PluginLoadDiagnostics.hpp"

#include <cstdio>
#include <mutex>

#include <boost/filesystem.hpp>

namespace Slic3r {

namespace {
std::mutex        s_plugin_load_failure_mutex;
PluginLoadFailure s_plugin_load_failure;
} // namespace

PluginLoadFailureKind classify_plugin_load_error(unsigned long win32_code,
                                                 unsigned long nt_status,
                                                 bool          file_exists,
                                                 std::uint64_t file_size)
{
    namespace c = plugin_load_codes;

    // No file, or an empty one (an antivirus quarantine leaves either): nothing else to say.
    if (!file_exists || file_size == 0)
        return PluginLoadFailureKind::Missing;

    // From here the file is there. The NTSTATUS, when we have it, is more precise than the Win32 code.
    if (nt_status == c::kStatusSystemIntegrityViolation || nt_status == c::kStatusInvalidImageHash ||
        nt_status == c::kStatusVirusInfected || nt_status == c::kStatusVirusDeleted)
        return PluginLoadFailureKind::Blocked;

    switch (win32_code) {
    case c::kInvalidImageHash:
    case c::kAccessDisabledByPolicy:
    case c::kAccessDisabledNoSafer:
    case c::kSystemIntegrityViolation:
    case c::kVirusInfected:
    case c::kVirusDeleted:
    case c::kAccessDenied:
        return PluginLoadFailureKind::Blocked;
    case c::kModNotFound:  // a DLL this one imports is missing (the file itself exists)
    case c::kProcNotFound: // ... or an import was not found in the DLL that is there
    case c::kFileNotFound: // the file existed when we looked, so these are about an import
    case c::kPathNotFound:
        return PluginLoadFailureKind::MissingDependency;
    case c::kBadExeFormat:
    case c::kBadFormat:
    case c::kExeMachineTypeMismatch:
    case c::kExeMarkedInvalid:
    case c::kInvalidExeSignature:
    case c::kImageMachineTypeMismatch:
        return PluginLoadFailureKind::BadImage;
    default:
        return PluginLoadFailureKind::Other;
    }
}

PluginLoadMessage plugin_load_message(PluginLoadFailureKind kind, bool file_exists_now, std::uint64_t file_size_now)
{
    switch (kind) {
    case PluginLoadFailureKind::None:
        return PluginLoadMessage::Restart;
    case PluginLoadFailureKind::Missing:
        // The load point came before the first-run copy: the file is there now, a restart loads it.
        if (file_exists_now && file_size_now > 0)
            return PluginLoadMessage::Restart;
        return PluginLoadMessage::MissingFile;
    case PluginLoadFailureKind::Blocked:           return PluginLoadMessage::Blocked;
    case PluginLoadFailureKind::MissingDependency: return PluginLoadMessage::MissingDependency;
    case PluginLoadFailureKind::BadImage:          return PluginLoadMessage::BadImage;
    case PluginLoadFailureKind::Incompatible:      return PluginLoadMessage::Incompatible;
    case PluginLoadFailureKind::Other:             break;
    }
    return PluginLoadMessage::Other;
}

std::string plugin_load_error_text(const PluginLoadFailure &failure)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%lX", failure.code);
    std::string text = buf;
    if (failure.nt_status != 0 && failure.nt_status != failure.code) {
        std::snprintf(buf, sizeof(buf), ", status 0x%lX", failure.nt_status);
        text += buf;
    }
    return text;
}

void record_plugin_load_failure(const PluginLoadFailure &failure)
{
    std::lock_guard<std::mutex> lock(s_plugin_load_failure_mutex);
    s_plugin_load_failure = failure;
}

PluginLoadFailure last_plugin_load_failure()
{
    std::lock_guard<std::mutex> lock(s_plugin_load_failure_mutex);
    return s_plugin_load_failure;
}

void clear_plugin_load_failure()
{
    std::lock_guard<std::mutex> lock(s_plugin_load_failure_mutex);
    s_plugin_load_failure = PluginLoadFailure{};
}

void plugin_file_facts(const std::string &utf8_path, bool &exists, std::uint64_t &size)
{
    exists = false;
    size   = 0;
    if (utf8_path.empty())
        return;
    boost::system::error_code ec;
    const boost::filesystem::path p(utf8_path);
    exists = boost::filesystem::is_regular_file(p, ec) && !ec;
    if (!exists)
        return;
    const auto sz = boost::filesystem::file_size(p, ec);
    if (!ec)
        size = static_cast<std::uint64_t>(sz);
}

} // namespace Slic3r
