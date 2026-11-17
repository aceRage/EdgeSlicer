#include "PluginSandboxPriv.hpp"

#ifndef _WIN32

namespace Slic3r::plugin_ipc {

bool apply_win_child_sandbox(const SandboxSpec&, SandboxReport& report, std::string& error)
{
    error         = "AppContainer is Windows-only";
    report.kind   = k_sandbox_kind_none;
    report.detail = error;
    return false;
}

bool probe_win_sandbox(SandboxProbe& probe)
{
    probe.available = false;
    probe.reason    = "not Windows";
    return false;
}

bool  prepare_win_appcontainer(const SandboxSpec&, std::string&) { return true; }
void  close_win_appcontainer() {}
void  sweep_win_acl_journal() {}
void  delete_win_appcontainer_profile(const std::string&) {}
void  delete_win_test_created_appcontainer_profile(const std::string&) {}
void  set_test_only_plugin_keys_for_testing(bool) {}
void* win_security_capabilities() { return nullptr; }
void* win_pipe_security_attributes() { return nullptr; }

} // namespace Slic3r::plugin_ipc

#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <userenv.h>
#ifndef SECURITY_APP_PACKAGE_BASE_RID
#define SECURITY_APP_PACKAGE_BASE_RID (0x00000002L)
#endif
#ifndef PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES
#define PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES ProcThreadAttributeValue(9, FALSE, TRUE, FALSE)
#endif

#include "PluginHostProcess.hpp"
#include "PluginIpcProtocol.hpp"

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Slic3r::plugin_ipc {
namespace {

using CreateAppContainerProfile_fn                 = HRESULT(WINAPI*)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID*);
using DeriveAppContainerSidFromAppContainerName_fn = HRESULT(WINAPI*)(PCWSTR, PSID*);
using DeriveCapabilitySidsFromName_fn              = BOOL(WINAPI*)(LPCWSTR, PSID**, DWORD*, PSID**, DWORD*);
using DeleteAppContainerProfile_fn                 = HRESULT(WINAPI*)(PCWSTR);
using GetAppContainerFolderPath_fn                 = HRESULT(WINAPI*)(PCWSTR, PWSTR*);

CreateAppContainerProfile_fn                 p_CreateProfile = nullptr;
DeriveAppContainerSidFromAppContainerName_fn p_DeriveSid     = nullptr;
DeriveCapabilitySidsFromName_fn              p_DeriveCaps    = nullptr;
DeleteAppContainerProfile_fn                 p_DeleteProfile = nullptr;
GetAppContainerFolderPath_fn                 p_GetAcFolder   = nullptr;
bool                                         g_resolved      = false;
bool                                         g_apis_ok       = false;
std::atomic<bool>                            g_test_only_plugin_keys{false}; // test harness: honour test. → EdgeSlicer.t.*

void resolve_apis()
{
    if (g_resolved)
        return;
    g_resolved      = true;
    HMODULE userenv = ::LoadLibraryW(L"userenv.dll");
    HMODULE kernel  = ::GetModuleHandleW(L"kernel32.dll");
    if (userenv) {
        p_CreateProfile = reinterpret_cast<CreateAppContainerProfile_fn>(::GetProcAddress(userenv, "CreateAppContainerProfile"));
        p_DeriveSid     = reinterpret_cast<DeriveAppContainerSidFromAppContainerName_fn>(
            ::GetProcAddress(userenv, "DeriveAppContainerSidFromAppContainerName"));
        p_DeleteProfile = reinterpret_cast<DeleteAppContainerProfile_fn>(::GetProcAddress(userenv, "DeleteAppContainerProfile"));
        p_GetAcFolder   = reinterpret_cast<GetAppContainerFolderPath_fn>(::GetProcAddress(userenv, "GetAppContainerFolderPath"));
    }
    if (kernel) {
        p_DeriveCaps = reinterpret_cast<DeriveCapabilitySidsFromName_fn>(::GetProcAddress(kernel, "DeriveCapabilitySidsFromName"));
    }
    g_apis_ok = p_CreateProfile && p_DeriveSid;
}

bool plugin_key_is_test_only(const std::string& plugin_key)
{
    // Production never sets this; a real plugin keyed test.* hashes to EdgeSlicer.ph.*.
    if (!g_test_only_plugin_keys.load(std::memory_order_acquire))
        return false;
    return plugin_key.size() >= 5 && plugin_key.compare(0, 5, "test.") == 0;
}

std::wstring profile_name(const std::string& plugin_key)
{
    // Fixed-length name so two long keys cannot truncate to the same profile.
    // Test-only keys (`test.…`, only when the test harness enables the hook) use a
    // distinct moniker prefix so they cannot collide with a shipping `EdgeSlicer.ph.*`
    // profile even if the FNV hashes match.
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : plugin_key) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    wchar_t buf[48];
    if (plugin_key_is_test_only(plugin_key))
        swprintf(buf, 48, L"EdgeSlicer.t.%016llx", static_cast<unsigned long long>(hash));
    else
        swprintf(buf, 48, L"EdgeSlicer.ph.%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

// Low IL (S-1-16-4096) + no-write-up. AppContainer hosts run at low integrity; a medium
// object with only a DACL still denies GENERIC_WRITE (ERROR_ACCESS_DENIED / no-write-up).
std::string win32_error_text(DWORD err)
{
    wchar_t*    buf = nullptr;
    const DWORD n   = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                       err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string text;
    if (n > 0 && buf != nullptr) {
        text = boost::nowide::narrow(buf);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        ::LocalFree(buf);
    }
    return text;
}

void log_win32_failure(const char* what, const std::string& path, DWORD rc)
{
    BOOST_LOG_TRIVIAL(error) << "AppContainer " << what << " failed for " << path << " (" << rc << " " << win32_error_text(rc) << ")";
}

bool label_low_integrity(const std::string& path)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr) || sd == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "AppContainer low-IL SDDL convert failed for " << path;
        return false;
    }
    BOOL  present = FALSE, defaulted = FALSE;
    PACL  sacl = nullptr;
    DWORD rc   = ERROR_INVALID_PARAMETER;
    if (::GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted) && present && sacl != nullptr) {
        const std::wstring wide = boost::nowide::widen(path);
        rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                                     nullptr, sacl);
    }
    ::LocalFree(sd);
    if (rc != ERROR_SUCCESS)
        log_win32_failure("low-IL label", path, rc);
    return rc == ERROR_SUCCESS;
}

// Subfolder grants: inheritable GENERIC_READ|EXECUTE (plus write/delete when write=true).
// data_dir itself: non-inherited FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE so
// GetFileAttributes/status() on the folder succeeds without FILE_LIST_DIRECTORY.
constexpr DWORD k_acl_ro            = GENERIC_READ | GENERIC_EXECUTE;
constexpr DWORD k_acl_rw            = GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE;
constexpr DWORD k_acl_inherit       = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
constexpr DWORD k_data_dir_traverse = FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE;

bool acl_target_already_gone(DWORD rc) { return rc == ERROR_FILE_NOT_FOUND || rc == ERROR_PATH_NOT_FOUND; }

bool apply_dir_acl(PSID sid, const std::string& path, DWORD permissions, DWORD inheritance, ACCESS_MODE mode)
{
    if (path.empty() || sid == nullptr)
        return true;
    const std::wstring wide = boost::nowide::widen(path);
    EXPLICIT_ACCESSW   ea{};
    ea.grfAccessPermissions = permissions;
    ea.grfAccessMode        = mode;
    // One SetNamedSecurityInfoW. An inheritable ACE does propagate to existing
    // children (that is the grant). Do not SET then immediately REVOKE — that was
    // the redundant second walk. Grants live until close_win_appcontainer.
    // SetEntriesInAclW below appends this package-SID ACE onto the existing DACL,
    // which already allows the creating user; the user-plus-package rule is met
    // without replacing the DACL. REVOKE_ACCESS uses the same Trustee / permissions /
    // inheritance as SET so only that ACE is removed.
    ea.grfInheritance      = inheritance;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName   = static_cast<LPWSTR>(sid);

    PACL                 prev = nullptr;
    PACL                 neu  = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    const DWORD get_rc = ::GetNamedSecurityInfoW(wide.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &prev, nullptr,
                                                 &sd);
    if (get_rc != ERROR_SUCCESS) {
        if (mode == REVOKE_ACCESS && acl_target_already_gone(get_rc))
            return true;
        log_win32_failure(mode == REVOKE_ACCESS ? "ACL read (revoke)" : "ACL read (grant)", path, get_rc);
        return false;
    }
    const DWORD acl_rc = ::SetEntriesInAclW(1, &ea, prev, &neu);
    if (acl_rc != ERROR_SUCCESS) {
        log_win32_failure(mode == REVOKE_ACCESS ? "ACL SetEntries (revoke)" : "ACL SetEntries (grant)", path, acl_rc);
        if (sd)
            ::LocalFree(sd);
        return false;
    }
    const DWORD rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                             neu, nullptr);
    ::LocalFree(neu);
    if (sd)
        ::LocalFree(sd);
    if (rc != ERROR_SUCCESS) {
        if (mode == REVOKE_ACCESS && acl_target_already_gone(rc))
            return true;
        log_win32_failure(mode == REVOKE_ACCESS ? "ACL revoke" : "ACL grant", path, rc);
        return false;
    }
    return true;
}

struct DirGrant
{
    std::string  path;
    DWORD        permissions = 0;
    DWORD        inheritance = 0;
    bool         write       = false;
    bool         low_label   = false;
    bool         existing    = false; // package-SID ACE already present; journaled so sweep can drop it
    std::wstring original_label;      // LABEL_SECURITY_INFORMATION SDDL; empty = no ML ACE
};

struct JournalSession
{
    std::string           package_sid;
    std::string           profile_name;
    std::string           data_dir;
    std::string           python_home;
    std::string           helper_dir;
    std::string           resources_dir;
    std::vector<DirGrant> grants;
};

struct WinState
{
    PSID                         sid     = nullptr;
    PSID                         cap_sid = nullptr;
    SID_AND_ATTRIBUTES           cap_attr{};
    SECURITY_CAPABILITIES        caps{};
    LPPROC_THREAD_ATTRIBUTE_LIST attr_list = nullptr;
    SECURITY_ATTRIBUTES          pipe_sa{};
    PSECURITY_DESCRIPTOR         pipe_sd  = nullptr;
    PACL                         pipe_acl = nullptr;
    std::vector<DirGrant>        grants;
    std::wstring                 profile_name;
    boost::filesystem::path      journal_path; // pinned at grant time; never re-read from global data_dir()
    std::string                  session_data_dir;
    std::string                  session_python_home;
    std::string                  session_helper_dir;
    std::string                  session_resources_dir;
    bool                         ready = false;
};

std::vector<std::string>        g_known_journal_data_dirs;
std::unordered_set<std::string> g_created_profile_keys; // CreateAppContainerProfile succeeded (not ALREADY_EXISTS)

WinState g_win;
bool     g_low_label_used_copy_replace = false;

std::wstring win_path_key(const boost::filesystem::path& path);

// Parent-only journal. Sibling of the helper-writable {data_dir}/plugin_host log dir so
// AppContainer grants (inheritable RW on plugin_host) cannot reach it. The data_dir is
// spec.data_dir at grant time (fallback: Slic3r::data_dir()), then stored on the session.
boost::filesystem::path acl_journal_path_for(const std::string& data_dir)
{
    if (data_dir.empty())
        return {};
    return boost::filesystem::path(data_dir) / "plugin_host_acl" / "acl_journal.json";
}

std::string journal_data_dir_from_spec(const SandboxSpec& spec) { return !spec.data_dir.empty() ? spec.data_dir : Slic3r::data_dir(); }

void remember_journal_data_dir(const std::string& data_dir)
{
    if (data_dir.empty())
        return;
    const std::wstring key = win_path_key(data_dir);
    for (const std::string& existing : g_known_journal_data_dirs) {
        if (win_path_key(existing) == key)
            return;
    }
    g_known_journal_data_dirs.push_back(data_dir);
}

boost::filesystem::path helper_writable_legacy_journal_path_for(const std::string& data_dir)
{
    if (data_dir.empty())
        return {};
    return boost::filesystem::path(data_dir) / "plugin_host" / "acl_journal.json";
}

void delete_helper_writable_legacy_journal_for(const std::string& data_dir)
{
    const auto path = helper_writable_legacy_journal_path_for(data_dir);
    if (path.empty())
        return;
    boost::system::error_code ec;
    boost::filesystem::remove(path, ec);
}

std::string data_dir_from_journal_path(const boost::filesystem::path& journal)
{
    if (journal.empty() || journal.parent_path().empty())
        return {};
    return journal.parent_path().parent_path().string();
}

std::wstring sid_to_string(PSID sid)
{
    if (sid == nullptr || !::IsValidSid(sid))
        return {};
    LPWSTR str = nullptr;
    if (!::ConvertSidToStringSidW(sid, &str) || str == nullptr)
        return {};
    std::wstring out = str;
    ::LocalFree(str);
    return out;
}

bool process_is_appcontainer()
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD      is_ac = 0;
    DWORD      n     = sizeof(is_ac);
    const BOOL ok    = ::GetTokenInformation(token, TokenIsAppContainer, &is_ac, n, &n);
    ::CloseHandle(token);
    return ok && is_ac != 0;
}

// Journal / grant / sweep run only in the parent/broker. The helper links this TU via
// libslic3r_gui; these predicates keep that path from executing (and TokenIsAppContainer
// is a kernel fact a plugin cannot unset).
bool parent_acl_ops_allowed() { return !in_child_process() && !process_is_appcontainer(); }

std::wstring win_path_key(const boost::filesystem::path& path)
{
    boost::system::error_code ec;
    boost::filesystem::path   canon = boost::filesystem::weakly_canonical(path, ec);
    if (ec || canon.empty())
        canon = boost::filesystem::absolute(path, ec).lexically_normal();
    std::wstring out = canon.wstring();
    for (wchar_t& c : out) {
        if (c == L'/')
            c = L'\\';
        c = static_cast<wchar_t>(::towlower(c));
    }
    while (out.size() > 1 && out.back() == L'\\')
        out.pop_back();
    return out;
}

bool path_is_under_root(const boost::filesystem::path& candidate, const boost::filesystem::path& root)
{
    if (candidate.empty() || root.empty())
        return false;
    const std::wstring c = win_path_key(candidate);
    const std::wstring r = win_path_key(root);
    if (c.empty() || r.empty())
        return false;
    if (c == r)
        return true;
    return c.size() > r.size() && c.compare(0, r.size(), r) == 0 && c[r.size()] == L'\\';
}

std::vector<boost::filesystem::path> known_sandbox_roots()
{
    std::vector<boost::filesystem::path> roots;
    auto                                 add = [&](const std::string& path) {
        if (path.empty())
            return;
        const boost::filesystem::path p(path);
        for (const auto& existing : roots) {
            if (path_is_under_root(p, existing) && win_path_key(p) == win_path_key(existing))
                return;
        }
        roots.push_back(p);
    };
    for (const std::string& path : parent_sandbox_known_roots())
        add(path);
    for (const std::string& path : g_known_journal_data_dirs)
        add(path);
    wchar_t     tmp[MAX_PATH] = {};
    const DWORD n             = ::GetTempPathW(MAX_PATH, tmp);
    if (n > 0 && n < MAX_PATH)
        add(boost::nowide::narrow(tmp));
    boost::system::error_code ec;
    const auto                boost_tmp = boost::filesystem::temp_directory_path(ec);
    if (!ec)
        add(boost_tmp.string());
    return roots;
}

bool path_in_known_root(const std::string& path)
{
    if (path.empty())
        return false;
    const boost::filesystem::path candidate(path);
    for (const auto& root : known_sandbox_roots()) {
        if (path_is_under_root(candidate, root))
            return true;
    }
    return false;
}

bool path_in_session_roots(const std::string& path, const JournalSession& session)
{
    if (path.empty())
        return false;
    const boost::filesystem::path candidate(path);
    auto under = [&](const std::string& root) { return !root.empty() && path_is_under_root(candidate, boost::filesystem::path(root)); };
    return under(session.data_dir) || under(session.python_home) || under(session.helper_dir) || under(session.resources_dir);
}

bool session_has_recorded_roots(const JournalSession& session)
{
    return !session.data_dir.empty() || !session.python_home.empty() || !session.helper_dir.empty() || !session.resources_dir.empty();
}

bool path_is_acl_journal_tree(const std::string& path)
{
    if (path.empty())
        return false;
    auto under_journal = [&](const boost::filesystem::path& journal) {
        return !journal.empty() && path_is_under_root(path, journal.parent_path());
    };
    if (under_journal(g_win.journal_path))
        return true;
    if (under_journal(acl_journal_path_for(Slic3r::data_dir())))
        return true;
    for (const std::string& dir : g_known_journal_data_dirs) {
        if (under_journal(acl_journal_path_for(dir)))
            return true;
    }
    return false;
}

bool is_edgeslicer_profile_name(const std::wstring& name)
{
    auto matches = [&](const wchar_t* prefix, size_t prefix_n) {
        constexpr size_t k_hex_n = 16;
        if (name.size() != prefix_n + k_hex_n)
            return false;
        if (::_wcsnicmp(name.c_str(), prefix, prefix_n) != 0)
            return false;
        for (size_t i = prefix_n; i < name.size(); ++i) {
            const wchar_t c   = name[i];
            const bool    hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
            if (!hex)
                return false;
        }
        return true;
    };
    return matches(L"EdgeSlicer.ph.", 14) || matches(L"EdgeSlicer.t.", 13);
}

bool sid_is_well_known_appcontainer_group(const std::string& sid) { return sid == "S-1-15-2-1" || sid == "S-1-15-2-2"; }

bool sid_looks_like_appcontainer_package(PSID sid)
{
    if (sid == nullptr || !::IsValidSid(sid))
        return false;
    const SID_IDENTIFIER_AUTHORITY* auth = ::GetSidIdentifierAuthority(sid);
    if (auth == nullptr)
        return false;
    static const unsigned char k_app_auth[6] = {0, 0, 0, 0, 0, 15};
    if (std::memcmp(auth->Value, k_app_auth, 6) != 0)
        return false;
    const UCHAR* n = ::GetSidSubAuthorityCount(sid);
    if (n == nullptr || *n < 8)
        return false;
    const DWORD* base = ::GetSidSubAuthority(sid, 0);
    return base != nullptr && *base == SECURITY_APP_PACKAGE_BASE_RID;
}

bool session_sid_allowed(const JournalSession& session)
{
    if (session.package_sid.empty() || sid_is_well_known_appcontainer_group(session.package_sid))
        return false;
    if (!session.profile_name.empty()) {
        const std::wstring name = boost::nowide::widen(session.profile_name);
        if (!is_edgeslicer_profile_name(name) || p_DeriveSid == nullptr)
            return false;
        PSID derived = nullptr;
        if (FAILED(p_DeriveSid(name.c_str(), &derived)) || derived == nullptr)
            return false;
        const std::wstring got = sid_to_string(derived);
        ::FreeSid(derived);
        return ::_wcsicmp(got.c_str(), boost::nowide::widen(session.package_sid).c_str()) == 0;
    }
    PSID               sid  = nullptr;
    const std::wstring wide = boost::nowide::widen(session.package_sid);
    if (!::ConvertStringSidToSidW(wide.c_str(), &sid) || sid == nullptr)
        return false;
    const bool ok = sid_looks_like_appcontainer_package(sid);
    ::LocalFree(sid);
    return ok;
}

bool set_parent_only_dacl(const std::string& path, PSID package_sid)
{
    if (path.empty())
        return false;
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        log_win32_failure("journal DACL OpenProcessToken", path, ::GetLastError());
        return false;
    }
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        ::CloseHandle(token);
        log_win32_failure("journal DACL TokenUser", path, ::GetLastError());
        return false;
    }
    std::vector<BYTE> user_buf(needed);
    if (!::GetTokenInformation(token, TokenUser, user_buf.data(), static_cast<DWORD>(user_buf.size()), &needed)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(token);
        log_win32_failure("journal DACL TokenUser", path, err);
        return false;
    }
    ::CloseHandle(token);
    PSID user = reinterpret_cast<TOKEN_USER*>(user_buf.data())->User.Sid;

    PSID any            = nullptr;
    PSID all_restricted = nullptr;
    PSID system         = nullptr;
    if (!::ConvertStringSidToSidW(L"S-1-15-2-1", &any))
        any = nullptr;
    if (!::ConvertStringSidToSidW(L"S-1-15-2-2", &all_restricted))
        all_restricted = nullptr;
    if (!::ConvertStringSidToSidW(L"S-1-5-18", &system))
        system = nullptr;

    EXPLICIT_ACCESSW ea[5]{};
    DWORD            n   = 0;
    auto             add = [&](PSID s, ACCESS_MODE mode, TRUSTEE_TYPE type) {
        if (s == nullptr || n >= 5)
            return;
        ea[n].grfAccessPermissions = FILE_ALL_ACCESS;
        ea[n].grfAccessMode        = mode;
        ea[n].grfInheritance       = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
        ea[n].Trustee.TrusteeForm  = TRUSTEE_IS_SID;
        ea[n].Trustee.TrusteeType  = type;
        ea[n].Trustee.ptstrName    = static_cast<LPWSTR>(s);
        ++n;
    };
    add(any, DENY_ACCESS, TRUSTEE_IS_WELL_KNOWN_GROUP);
    add(all_restricted, DENY_ACCESS, TRUSTEE_IS_WELL_KNOWN_GROUP);
    add(package_sid, DENY_ACCESS, TRUSTEE_IS_WELL_KNOWN_GROUP);
    add(user, SET_ACCESS, TRUSTEE_IS_USER);
    add(system, SET_ACCESS, TRUSTEE_IS_WELL_KNOWN_GROUP);

    PACL  neu    = nullptr;
    DWORD acl_rc = ::SetEntriesInAclW(n, ea, nullptr, &neu);
    DWORD rc     = acl_rc;
    if (acl_rc == ERROR_SUCCESS && neu != nullptr) {
        const std::wstring wide = boost::nowide::widen(path);
        rc                      = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT,
                                                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, neu, nullptr);
        ::LocalFree(neu);
    }
    if (any)
        ::LocalFree(any);
    if (all_restricted)
        ::LocalFree(all_restricted);
    if (system)
        ::LocalFree(system);
    if (rc != ERROR_SUCCESS) {
        log_win32_failure("journal parent-only DACL", path, rc);
        return false;
    }
    return true;
}

bool ensure_journal_dir_hardened(const boost::filesystem::path& dir, PSID package_sid)
{
    if (dir.empty())
        return false;
    boost::system::error_code ec;
    boost::filesystem::create_directories(dir, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "AppContainer ACL journal dir create failed for " << dir.string() << ": " << ec.message();
        return false;
    }
    return set_parent_only_dacl(dir.string(), package_sid);
}

std::wstring capture_label_sddl(const std::string& path)
{
    const std::wstring   wide = boost::nowide::widen(path);
    PACL                 sacl = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr,
                                &sacl, &sd) != ERROR_SUCCESS ||
        sd == nullptr)
        return {};
    LPWSTR sddl = nullptr;
    if (!::ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1, LABEL_SECURITY_INFORMATION, &sddl, nullptr) ||
        sddl == nullptr) {
        ::LocalFree(sd);
        return {};
    }
    std::wstring out = sddl;
    ::LocalFree(sddl);
    ::LocalFree(sd);
    return out;
}

bool path_has_low_integrity_label(const std::string& path);

bool is_regular_nonsymlink(const std::string& path)
{
    boost::system::error_code ec;
    const auto                st = boost::filesystem::symlink_status(path, ec);
    return !ec && boost::filesystem::is_regular_file(st);
}

// Modify allows delete/replace. WRITE_OWNER (needed by LABEL_SECURITY_INFORMATION) does not.
// Copy bytes into a new Medium-IL file in the same directory, then MoveFileEx replace.
// Never delete the original first: if the move fails (AV / indexer holding the temp),
// the original stays and only the temp is removed. Do not use CopyFile/ReplaceFile:
// those copy the Low mandatory label with the security descriptor.
bool copy_replace_file_as_user(const std::string& path)
{
    namespace fs = boost::filesystem;
    if (path_is_acl_journal_tree(path) || !is_regular_nonsymlink(path))
        return false;
    const std::wstring srcw = boost::nowide::widen(path);
    HANDLE             src  = ::CreateFileW(srcw.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (src == INVALID_HANDLE_VALUE)
        return false;
    const fs::path     tmp  = fs::path(path).parent_path() /
        (fs::path(path).filename().string() + ".edge-ml-" + std::to_string(::GetCurrentProcessId()) + "-" +
         std::to_string(::GetTickCount64()) + ".tmp");
    const std::wstring tmpw = boost::nowide::widen(tmp.string());
    HANDLE             dst  = ::CreateFileW(tmpw.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dst == INVALID_HANDLE_VALUE) {
        ::CloseHandle(src);
        return false;
    }
    std::vector<char> buf(64 * 1024);
    DWORD             nread    = 0;
    bool              copied   = true;
    for (;;) {
        if (!::ReadFile(src, buf.data(), static_cast<DWORD>(buf.size()), &nread, nullptr)) {
            copied = false;
            break;
        }
        if (nread == 0)
            break;
        DWORD nwritten = 0;
        if (!::WriteFile(dst, buf.data(), nread, &nwritten, nullptr) || nwritten != nread) {
            copied = false;
            break;
        }
    }
    ::CloseHandle(src);
    ::CloseHandle(dst);
    if (!copied) {
        ::DeleteFileW(tmpw.c_str());
        return false;
    }
    if (!::MoveFileExW(tmpw.c_str(), srcw.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log_win32_failure("Low-IL copy-replace move", path, ::GetLastError());
        ::DeleteFileW(tmpw.c_str());
        return false;
    }
    g_low_label_used_copy_replace = true;
    BOOST_LOG_TRIVIAL(warning) << "AppContainer Low-IL copy-replace succeeded for " << path;
    return true;
}

bool set_label_sddl(const std::string& path, const std::wstring& sddl)
{
    const std::wstring wide = boost::nowide::widen(path);
    DWORD              rc   = ERROR_INVALID_PARAMETER;
    if (sddl.empty()) {
        BYTE  buf[sizeof(ACL) + 16]{};
        auto* acl = reinterpret_cast<PACL>(buf);
        if (!::InitializeAcl(acl, sizeof(buf), ACL_REVISION)) {
            log_win32_failure("Low-IL InitializeAcl", path, ::GetLastError());
            return false;
        }
        rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                                     nullptr, acl);
    } else {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) || sd == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "AppContainer Low-IL SDDL convert failed for " << path;
            return false;
        }
        BOOL present = FALSE, defaulted = FALSE;
        PACL sacl = nullptr;
        if (::GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted))
            rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                                         nullptr, present ? sacl : nullptr);
        ::LocalFree(sd);
    }
    if (rc != ERROR_SUCCESS) {
        if (acl_target_already_gone(rc))
            return true;
        log_win32_failure("Low-IL label restore", path, rc);
        return false;
    }
    return true;
}

bool restore_label_sddl(const std::string& path, const std::wstring& sddl, bool allow_copy_replace = true)
{
    const bool labelled = path_has_low_integrity_label(path);
    if (sddl.empty() && !labelled)
        return true;
    if (set_label_sddl(path, sddl) && (sddl.empty() ? !path_has_low_integrity_label(path) : true))
        return true;
    if (allow_copy_replace && sddl.empty() && labelled && copy_replace_file_as_user(path) && !path_has_low_integrity_label(path))
        return true;
    BOOST_LOG_TRIVIAL(error) << "AppContainer Low-IL label still present on " << path;
    return !path_has_low_integrity_label(path) && sddl.empty();
}

nlohmann::json grants_to_json(const std::vector<DirGrant>& grants)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const DirGrant& g : grants) {
        arr.push_back({
            {"path", g.path},
            {"permissions", g.permissions},
            {"inheritance", g.inheritance},
            {"write", g.write},
            {"low_label", g.low_label},
            {"existing", g.existing},
            {"original_label", boost::nowide::narrow(g.original_label)},
        });
    }
    return arr;
}

nlohmann::json session_to_json(const JournalSession& session)
{
    return {{"package_sid", session.package_sid},      {"profile_name", session.profile_name}, {"data_dir", session.data_dir},
            {"python_home", session.python_home},      {"helper_dir", session.helper_dir},     {"resources_dir", session.resources_dir},
            {"grants", grants_to_json(session.grants)}};
}

DirGrant grant_from_json(const nlohmann::json& item)
{
    DirGrant grant;
    grant.path           = item.value("path", std::string());
    grant.permissions    = static_cast<DWORD>(item.value("permissions", 0u));
    grant.inheritance    = static_cast<DWORD>(item.value("inheritance", 0u));
    grant.write          = item.value("write", false);
    grant.low_label      = item.value("low_label", false);
    grant.existing       = item.value("existing", false);
    grant.original_label = boost::nowide::widen(item.value("original_label", std::string()));
    return grant;
}

JournalSession session_from_json_object(const nlohmann::json& json)
{
    JournalSession session;
    session.package_sid   = json.value("package_sid", std::string());
    session.profile_name  = json.value("profile_name", std::string());
    session.data_dir      = json.value("data_dir", std::string());
    session.python_home   = json.value("python_home", std::string());
    session.helper_dir    = json.value("helper_dir", std::string());
    session.resources_dir = json.value("resources_dir", std::string());
    if (json.contains("grants") && json["grants"].is_array()) {
        for (const auto& item : json["grants"]) {
            if (!item.is_object() || !item.contains("path") || !item["path"].is_string())
                continue;
            session.grants.push_back(grant_from_json(item));
        }
    }
    return session;
}

void merge_session(std::vector<JournalSession>& sessions, JournalSession incoming)
{
    if (incoming.package_sid.empty() && incoming.grants.empty())
        return;
    for (JournalSession& existing : sessions) {
        if (existing.package_sid != incoming.package_sid)
            continue;
        for (DirGrant& g : incoming.grants) {
            bool found = false;
            for (DirGrant& e : existing.grants) {
                if (e.path == g.path) {
                    e     = g;
                    found = true;
                    break;
                }
            }
            if (!found)
                existing.grants.push_back(std::move(g));
        }
        if (existing.data_dir.empty())
            existing.data_dir = incoming.data_dir;
        if (existing.python_home.empty())
            existing.python_home = incoming.python_home;
        if (existing.helper_dir.empty())
            existing.helper_dir = incoming.helper_dir;
        if (existing.resources_dir.empty())
            existing.resources_dir = incoming.resources_dir;
        return;
    }
    sessions.push_back(std::move(incoming));
}

std::vector<JournalSession> read_journal_file(const boost::filesystem::path& path)
{
    std::vector<JournalSession> out;
    boost::system::error_code   ec;
    if (path.empty() || !boost::filesystem::exists(path, ec) || ec)
        return out;
    boost::nowide::ifstream in(path.string());
    if (!in) {
        BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL journal unreadable: " << path.string();
        return out;
    }
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL journal invalid: " << path.string();
        return out;
    }
    if (json.contains("sessions") && json["sessions"].is_array()) {
        for (const auto& item : json["sessions"]) {
            if (item.is_object())
                merge_session(out, session_from_json_object(item));
        }
        return out;
    }
    merge_session(out, session_from_json_object(json));
    return out;
}

std::vector<JournalSession> load_journal_sessions_at(const boost::filesystem::path& path)
{
    delete_helper_writable_legacy_journal_for(data_dir_from_journal_path(path));
    return read_journal_file(path);
}

std::vector<boost::filesystem::path> journal_paths_to_sweep()
{
    std::vector<boost::filesystem::path> paths;
    auto                                 add = [&](const boost::filesystem::path& path) {
        if (path.empty())
            return;
        const std::wstring key = win_path_key(path);
        for (const auto& existing : paths) {
            if (win_path_key(existing) == key)
                return;
        }
        paths.push_back(path);
    };
    add(acl_journal_path_for(Slic3r::data_dir()));
    for (const std::string& dir : g_known_journal_data_dirs)
        add(acl_journal_path_for(dir));
    if (!g_win.journal_path.empty())
        add(g_win.journal_path);
    return paths;
}

bool write_journal_file(const boost::filesystem::path& path, const std::vector<JournalSession>& sessions)
{
    if (path.empty())
        return true;
    boost::system::error_code ec;
    if (sessions.empty()) {
        boost::filesystem::remove(path, ec);
        return true;
    }
    if (!ensure_journal_dir_hardened(path.parent_path(), g_win.sid))
        return false;
    nlohmann::json arr = nlohmann::json::array();
    for (const JournalSession& session : sessions)
        arr.push_back(session_to_json(session));
    nlohmann::json body = {{"sessions", std::move(arr)}};
    std::string    err;
    if (!Slic3r::write_file_atomically(path.string(), body.dump(1, '\t') + "\n", &err)) {
        BOOST_LOG_TRIVIAL(error) << "AppContainer ACL journal write failed: " << path.string() << " " << err;
        return false;
    }
    if (!set_parent_only_dacl(path.string(), g_win.sid))
        return false;
    return true;
}

bool persist_sessions_at(const boost::filesystem::path& path, const std::vector<JournalSession>& sessions)
{
    if (!parent_acl_ops_allowed())
        return false;
    if (path.empty())
        return false;
    return write_journal_file(path, sessions);
}

bool persist_acl_journal(PSID sid)
{
    if (!parent_acl_ops_allowed())
        return false;
    // Live persist uses the path pinned at grant time, never the current global data_dir().
    if (g_win.journal_path.empty())
        return false;
    JournalSession current;
    current.package_sid   = boost::nowide::narrow(sid_to_string(sid));
    current.profile_name  = boost::nowide::narrow(g_win.profile_name);
    current.data_dir      = g_win.session_data_dir;
    current.python_home   = g_win.session_python_home;
    current.helper_dir    = g_win.session_helper_dir;
    current.resources_dir = g_win.session_resources_dir;
    current.grants        = g_win.grants;
    auto all              = load_journal_sessions_at(g_win.journal_path);
    bool replaced         = false;
    for (JournalSession& session : all) {
        if (session.package_sid == current.package_sid) {
            session  = current;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        all.push_back(std::move(current));
    return persist_sessions_at(g_win.journal_path, all);
}

bool remove_session_from_journals(const std::string& package_sid)
{
    if (!parent_acl_ops_allowed())
        return false;
    if (g_win.journal_path.empty())
        return false;
    auto all = load_journal_sessions_at(g_win.journal_path);
    all.erase(std::remove_if(all.begin(), all.end(), [&](const JournalSession& session) { return session.package_sid == package_sid; }),
              all.end());
    return persist_sessions_at(g_win.journal_path, all);
}

// Files a Low-IL helper creates inherit a Low mandatory label even when the parent
// directory was not labelled (user fs_readwrite) or after the directory label is
// restored (Edge work dirs). Bounded walk: skip symlinks, cap entries so a user
// folder cannot stall close/sweep. WRITE_OWNER is required to edit a mandatory
// label; when it is missing (Modify-only folders), copy-replace the file as the user.
bool strip_low_labels_in_tree(const std::string& root)
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    const fs::path            path(root);
    if (root.empty() || !fs::exists(path, ec) || ec)
        return true;
    bool ok        = true;
    auto strip_one = [&](const fs::path& p) {
        const std::string s = p.string();
        if (path_is_acl_journal_tree(s))
            return;
        if (path_has_low_integrity_label(s) && !restore_label_sddl(s, {}))
            ok = false;
    };
    strip_one(path);
    if (!fs::is_directory(path, ec) || ec)
        return ok;
    constexpr int k_max_entries = 4096;
    int           n             = 0;
    for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        if (++n >= k_max_entries) {
            BOOST_LOG_TRIVIAL(warning) << "AppContainer Low-IL strip stopped after " << k_max_entries << " entries in " << root;
            ok = false;
            break;
        }
        boost::system::error_code st_ec;
        const fs::file_status     st = it->symlink_status(st_ec);
        if (st_ec)
            continue;
        if (fs::is_symlink(st)) {
            it.disable_recursion_pending();
            continue;
        }
        strip_one(it->path());
    }
    return ok;
}

enum class GrantActionSource { Live, Journal };

// Journal ACE revoke of a derived EdgeSlicer.ph.*/t.* SID is safe on any path recorded
// in the parent-only journal (the sandbox cannot write it). Session python_home / helper /
// resources / data_dir count as known roots for that session so a moved install still
// sweeps. Copy-replace of Low-IL files stays on this process's user-rw roots.
bool grant_path_is_actionable(const DirGrant& grant, GrantActionSource source, const JournalSession* session = nullptr)
{
    if (grant.path.empty() || path_is_acl_journal_tree(grant.path))
        return false;
    if (source == GrantActionSource::Live)
        return true;
    if (path_in_known_root(grant.path))
        return true;
    if (session && path_in_session_roots(grant.path, *session))
        return true;
    // Legacy journals with no recorded roots: the grant list itself is the record.
    if (!session || !session_has_recorded_roots(*session))
        return true;
    // Extra grant paths (user fs_readwrite, plugin_root) are recorded in the journal even
    // when they sit outside the four session roots. Revoking our package-SID ACE is safe.
    return true;
}

bool revoke_one_grant(PSID sid, const DirGrant& grant, GrantActionSource source, const JournalSession* session = nullptr)
{
    if (!grant_path_is_actionable(grant, source, session)) {
        if (source == GrantActionSource::Journal)
            BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL journal skipped path outside known roots: " << grant.path;
        // Live: leave the grant. Journal: keep it so a later sweep can still revoke.
        return false;
    }
    bool ok = true;
    if (sid != nullptr && !apply_dir_acl(sid, grant.path, grant.permissions, grant.inheritance, REVOKE_ACCESS))
        ok = false;
    // Copy-replace rewrites file bytes; only user-rw roots of this process, not a
    // recorded python_home / helper / resources tree that may now be an install dir.
    const bool allow_copy_replace = source == GrantActionSource::Live || path_in_known_root(grant.path);
    if (grant.low_label) {
        // LABEL_SECURITY_INFORMATION on the recorded grant path itself is allowed even
        // when that path is outside this process's current roots (moved python_home).
        if (!restore_label_sddl(grant.path, grant.original_label, allow_copy_replace))
            ok = false;
    }
    // User fs_readwrite is write && !low_label. Also walk Edge write dirs so
    // helper-created files (edge-plugin-host.log, post_process.gcode) lose Low.
    // Journal-sourced copy-replace stays inside known user-rw roots / write grants.
    if (grant.write && allow_copy_replace && !strip_low_labels_in_tree(grant.path))
        ok = false;
    else if (grant.write && source == GrantActionSource::Journal && !allow_copy_replace)
        BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL journal skipped copy-replace outside known user-rw roots: " << grant.path;
    return ok;
}

void revoke_grants()
{
    // No SID: crash-simulation / already released. Leave journals for sweep.
    if (!g_win.sid) {
        g_win.grants.clear();
        return;
    }
    const std::string     sid_utf8 = boost::nowide::narrow(sid_to_string(g_win.sid));
    std::vector<DirGrant> leftover;
    leftover.reserve(g_win.grants.size());
    for (const auto& grant : g_win.grants) {
        if (!revoke_one_grant(g_win.sid, grant, GrantActionSource::Live))
            leftover.push_back(grant);
    }
    g_win.grants = leftover;
    if (leftover.empty())
        remove_session_from_journals(sid_utf8);
    else if (!persist_acl_journal(g_win.sid))
        BOOST_LOG_TRIVIAL(error) << "AppContainer ACL journal kept leftover grants but could not rewrite the file";
}

void release_win_handles()
{
    if (g_win.attr_list) {
        ::DeleteProcThreadAttributeList(g_win.attr_list);
        ::HeapFree(::GetProcessHeap(), 0, g_win.attr_list);
        g_win.attr_list = nullptr;
    }
    if (g_win.sid) {
        ::FreeSid(g_win.sid);
        g_win.sid = nullptr;
    }
    if (g_win.cap_sid) {
        ::LocalFree(g_win.cap_sid);
        g_win.cap_sid = nullptr;
    }
    if (g_win.pipe_acl) {
        ::LocalFree(g_win.pipe_acl);
        g_win.pipe_acl = nullptr;
    }
    if (g_win.pipe_sd) {
        ::LocalFree(g_win.pipe_sd);
        g_win.pipe_sd = nullptr;
    }
    g_win.caps    = {};
    g_win.pipe_sa = {};
    g_win.profile_name.clear();
    g_win.journal_path.clear();
    g_win.session_data_dir.clear();
    g_win.session_python_home.clear();
    g_win.session_helper_dir.clear();
    g_win.session_resources_dir.clear();
    g_win.ready = false;
}

void clear_win_state()
{
    revoke_grants();
    release_win_handles();
}

// AppContainer access is two checks: (1) user/groups and (2) the package SID plus
// ALL APPLICATION PACKAGES (S-1-15-2-1), which every AppContainer token includes as a
// group. A Users RX ACE is not enough — the package half would still fail. AccessCheck
// against a user token would therefore skip a needed grant. We walk the object's DACL
// (including inherited ACEs; skip INHERIT_ONLY) for ALLOW of FILE_GENERIC_READ|
// FILE_GENERIC_EXECUTE after MapGenericMask on this package SID or S-1-15-2-1. That is
// the package half of AccessCheck without synthesizing a token (SeCreateTokenPrivilege).
enum class PackageRxKind { None, AllApplicationPackages, ThisPackage };

PackageRxKind package_rx_kind(const std::string& path, PSID package_sid)
{
    PSID any_sid = nullptr;
    if (!::ConvertStringSidToSidW(L"S-1-15-2-1", &any_sid) || any_sid == nullptr)
        any_sid = nullptr;

    const std::wstring   wide = boost::nowide::widen(path);
    PACL                 dacl = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    const DWORD rc = ::GetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                             &dacl, nullptr, &sd);
    bool        this_ok = false;
    bool        any_ok  = false;
    if (rc == ERROR_SUCCESS && dacl != nullptr) {
        ACL_SIZE_INFORMATION info{};
        if (::GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation)) {
            GENERIC_MAPPING   mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
            const ACCESS_MASK need = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
            for (DWORD i = 0; i < info.AceCount; ++i) {
                LPVOID ace = nullptr;
                if (!::GetAce(dacl, i, &ace) || ace == nullptr)
                    continue;
                auto* header = static_cast<ACE_HEADER*>(ace);
                if ((header->AceFlags & INHERIT_ONLY_ACE) != 0)
                    continue;
                if (header->AceType != ACCESS_ALLOWED_ACE_TYPE && header->AceType != ACCESS_DENIED_ACE_TYPE)
                    continue;
                auto*      allowed_ace = static_cast<ACCESS_ALLOWED_ACE*>(ace);
                PSID       ace_sid     = reinterpret_cast<PSID>(&allowed_ace->SidStart);
                const bool is_this     = package_sid != nullptr && ::EqualSid(ace_sid, package_sid);
                const bool is_any      = any_sid != nullptr && ::EqualSid(ace_sid, any_sid);
                if (!is_this && !is_any)
                    continue;
                ACCESS_MASK mask = allowed_ace->Mask;
                ::MapGenericMask(&mask, &mapping);
                if (header->AceType == ACCESS_DENIED_ACE_TYPE && (mask & need) != 0) {
                    if (is_this)
                        this_ok = false;
                    if (is_any)
                        any_ok = false;
                    break;
                }
                if (header->AceType == ACCESS_ALLOWED_ACE_TYPE && (mask & need) == need) {
                    if (is_this)
                        this_ok = true;
                    if (is_any)
                        any_ok = true;
                }
            }
        }
    }
    if (sd)
        ::LocalFree(sd);
    if (any_sid)
        ::LocalFree(any_sid);
    if (this_ok)
        return PackageRxKind::ThisPackage;
    if (any_ok)
        return PackageRxKind::AllApplicationPackages;
    return PackageRxKind::None;
}

bool path_has_low_integrity_label(const std::string& path)
{
    const std::wstring   wide = boost::nowide::widen(path);
    PACL                 sacl = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    if (::GetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr,
                                &sacl, &sd) != ERROR_SUCCESS ||
        sacl == nullptr) {
        if (sd)
            ::LocalFree(sd);
        return false;
    }
    PSID low = nullptr;
    if (!::ConvertStringSidToSidW(L"S-1-16-4096", &low) || low == nullptr) {
        ::LocalFree(sd);
        return false;
    }
    bool                 found = false;
    ACL_SIZE_INFORMATION info{};
    if (::GetAclInformation(sacl, &info, sizeof(info), AclSizeInformation)) {
        for (DWORD i = 0; i < info.AceCount; ++i) {
            LPVOID ace = nullptr;
            if (!::GetAce(sacl, i, &ace) || ace == nullptr)
                continue;
            auto* header = static_cast<ACE_HEADER*>(ace);
            if (header->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE)
                continue;
            auto* ml = static_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace);
            if (::EqualSid(reinterpret_cast<PSID>(&ml->SidStart), low)) {
                found = true;
                break;
            }
        }
    }
    ::LocalFree(low);
    ::LocalFree(sd);
    return found;
}

bool token_user_sid_string(HANDLE token, std::wstring& out, std::string& error)
{
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        error = "GetTokenInformation(TokenUser) failed";
        return false;
    }
    std::vector<BYTE> buf(needed);
    if (!::GetTokenInformation(token, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &needed)) {
        error = "GetTokenInformation(TokenUser) failed";
        return false;
    }
    const auto* tu = reinterpret_cast<const TOKEN_USER*>(buf.data());
    if (tu == nullptr || tu->User.Sid == nullptr || !::IsValidSid(tu->User.Sid)) {
        error = "GetTokenInformation(TokenUser) failed";
        return false;
    }
    LPWSTR sid_str = nullptr;
    if (!::ConvertSidToStringSidW(tu->User.Sid, &sid_str) || sid_str == nullptr) {
        error = "ConvertSidToStringSid failed for TokenUser";
        return false;
    }
    out.assign(sid_str);
    ::LocalFree(sid_str);
    return true;
}

} // namespace

bool win_build_pipe_security_descriptor(void* container_sid, void** out_sd, std::string& error)
{
    if (container_sid == nullptr || out_sd == nullptr) {
        error = "AppContainer named-pipe security descriptor failed";
        return false;
    }
    auto* sid = static_cast<PSID>(container_sid);
    if (!::IsValidSid(sid)) {
        error = "ConvertSidToStringSid failed for AppContainer SID";
        return false;
    }
    LPWSTR package_str = nullptr;
    if (!::ConvertSidToStringSidW(sid, &package_str) || package_str == nullptr) {
        error = "ConvertSidToStringSid failed for AppContainer SID";
        return false;
    }
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        ::LocalFree(package_str);
        error = "OpenProcessToken failed for TokenUser";
        return false;
    }
    std::wstring user_str;
    const bool   got_user = token_user_sid_string(token, user_str, error);
    ::CloseHandle(token);
    if (!got_user) {
        ::LocalFree(package_str);
        return false;
    }
    // AppContainer must pass the access check for both its user/groups and its package SID.
    // A non-elevated token carries BA as deny-only, so (A;;GA;;;BA) fails the user check
    // (ERROR_ACCESS_DENIED, helper exit 1005). Grant TokenUser and SYSTEM instead.
    std::wstring sddl = L"D:(A;;GA;;;";
    sddl += package_str;
    sddl += L")(A;;GA;;;";
    sddl += user_str;
    sddl += L")(A;;GA;;;SY)S:(ML;;NW;;;LW)";
    ::LocalFree(package_str);

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) || sd == nullptr) {
        error = "AppContainer named-pipe security descriptor failed";
        return false;
    }
    *out_sd = sd;
    return true;
}

bool probe_win_sandbox(SandboxProbe& probe)
{
    resolve_apis();
    probe.appcontainer = g_apis_ok;
    probe.available    = g_apis_ok;
    probe.reason       = g_apis_ok ? "AppContainer APIs present" :
                                     "CreateAppContainerProfile / DeriveAppContainerSidFromAppContainerName missing";
    return g_apis_ok;
}

bool prepare_win_appcontainer(const SandboxSpec& spec, std::string& error)
{
    resolve_apis();
    if (!parent_acl_ops_allowed()) {
        error = "AppContainer ACL journal is parent-only";
        return false;
    }
    // Drop this process's live grants first, then sweep leftover sessions (other SIDs)
    // from the parent-only journals we know. Sweep other SIDs even if we were ready.
    clear_win_state();
    sweep_win_acl_journal();
    if (!g_apis_ok) {
        error = "AppContainer APIs are not available; refusing to start the plugin host unsandboxed";
        return false;
    }

    // Pin the journal to spec.data_dir (fallback: global data_dir()) before the first ACE.
    // A missing plugin_host_acl folder is created; spawn fails only when no data dir exists.
    const std::string journal_root = journal_data_dir_from_spec(spec);
    g_win.journal_path             = acl_journal_path_for(journal_root);
    if (g_win.journal_path.empty()) {
        error = "AppContainer ACL journal failed (no data dir)";
        return false;
    }
    remember_journal_data_dir(journal_root);
    g_win.session_data_dir      = journal_root;
    g_win.session_python_home   = !spec.python_home.empty() ? spec.python_home : PythonInterpreter::bundled_python_home();
    g_win.session_helper_dir    = helper_path().parent_path().string();
    g_win.session_resources_dir = Slic3r::resources_dir();

    const std::wstring name = profile_name(spec.plugin_key);
    PSID               sid  = nullptr;
    HRESULT            hr = p_CreateProfile(name.c_str(), L"EdgeSlicer plugin host", L"OS sandbox for edge-plugin-host", nullptr, 0, &sid);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        hr = p_DeriveSid(name.c_str(), &sid);
    } else if (SUCCEEDED(hr) && sid != nullptr && !spec.plugin_key.empty()) {
        // Only profiles this process actually created. ERROR_ALREADY_EXISTS is a pre-existing
        // profile (previous run, another test, or a real plugin) and must not be torn down.
        g_created_profile_keys.insert(spec.plugin_key);
    }
    if (FAILED(hr) || sid == nullptr) {
        error = "CreateAppContainerProfile failed";
        return false;
    }
    g_win.sid          = sid;
    g_win.profile_name = name;
    // Journal exists before the first ACE is applied. Parent-only, AppContainer-unwritable.
    if (!persist_acl_journal(sid)) {
        error = "AppContainer ACL journal failed";
        clear_win_state();
        return false;
    }

    auto grant_or_fail = [&](const std::string& path, bool write, bool apply_low) {
        if (path.empty())
            return true;
        const DWORD perms = write ? k_acl_rw : k_acl_ro;
        // Every read-only grant (helper dir, resources_dir, python_home, packages, plugin_root,
        // extras): skip applying when the package half of the AppContainer check already allows RX.
        // ALL APPLICATION PACKAGES (Program Files) is left alone. A leftover ACE for *this*
        // package SID is journaled so close/sweep can still remove it.
        if (!write) {
            const PackageRxKind rx = package_rx_kind(path, sid);
            if (rx == PackageRxKind::AllApplicationPackages) {
                BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL grant skipped (ALL APPLICATION PACKAGES already has RX): " << path;
                return true;
            }
            if (rx == PackageRxKind::ThisPackage) {
                BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL grant skipped (package SID ACE already present; journaled): " << path;
                DirGrant grant;
                grant.path        = path;
                grant.permissions = perms;
                grant.inheritance = k_acl_inherit;
                grant.write       = false;
                grant.existing    = true;
                g_win.grants.push_back(grant);
                if (!persist_acl_journal(sid)) {
                    error = "AppContainer ACL journal failed for " + path;
                    g_win.grants.pop_back();
                    return false;
                }
                return true;
            }
        }
        DirGrant grant;
        grant.path        = path;
        grant.permissions = perms;
        grant.inheritance = k_acl_inherit;
        grant.write       = write;
        grant.low_label   = apply_low;
        if (apply_low)
            grant.original_label = capture_label_sddl(path);
        g_win.grants.push_back(grant);
        if (!persist_acl_journal(sid)) {
            error = "AppContainer ACL journal failed for " + path;
            g_win.grants.pop_back();
            return false;
        }
        if (!apply_dir_acl(sid, path, perms, k_acl_inherit, SET_ACCESS)) {
            error = "AppContainer ACL grant failed for " + path;
            return false;
        }
        // Low-IL label only on Edge-created work dirs. User fs_readwrite stays at its original
        // label so a Low-IL process outside the helper cannot write it after we grant.
        if (apply_low && !label_low_integrity(path))
            BOOST_LOG_TRIVIAL(warning) << "AppContainer low-IL label skipped for " << path;
        return true;
    };
    if (!grant_or_fail(spec.python_home, false, false)) {
        clear_win_state();
        return false;
    }
    // Subfolder grants do not make data_dir itself GetFileAttributes-able. Without this
    // non-inherited (RA,X)+SYNCHRONIZE ACE, set_data_dir's status() is Access denied (5)
    // and the helper exits 10 before Py init. No FILE_LIST_DIRECTORY / FILE_READ_DATA.
    const std::string data_dir = spec.data_dir.empty() ? Slic3r::data_dir() : spec.data_dir;
    if (!data_dir.empty()) {
        boost::system::error_code ec;
        const bool                exists = boost::filesystem::exists(data_dir, ec) && !ec;
        if (!exists) {
            // Helper set_data_dir treats not-found as the create case. A missing data_dir
            // must not fail-close spawn; skip the traverse ACE with a warning.
            BOOST_LOG_TRIVIAL(warning) << "AppContainer data_dir ACE skipped (path missing): " << data_dir;
        } else {
            DirGrant grant;
            grant.path        = data_dir;
            grant.permissions = k_data_dir_traverse;
            grant.inheritance = NO_INHERITANCE;
            g_win.grants.push_back(grant);
            if (!persist_acl_journal(sid)) {
                error = "AppContainer ACL journal failed for " + data_dir;
                clear_win_state();
                return false;
            }
            if (!apply_dir_acl(sid, data_dir, k_data_dir_traverse, NO_INHERITANCE, SET_ACCESS)) {
                error = "AppContainer ACL grant failed for " + data_dir;
                clear_win_state();
                return false;
            }
        }
    }
    if (!spec.write_root.empty()) {
        boost::system::error_code ec;
        if (!boost::filesystem::exists(spec.write_root, ec) || ec) {
            ec.clear();
            boost::filesystem::create_directories(spec.write_root, ec);
            if (ec) {
                error = "AppContainer ACL grant failed for " + spec.write_root;
                clear_win_state();
                return false;
            }
        }
    }
    if (!grant_or_fail(spec.plugin_root, false, false) || !grant_or_fail(spec.write_root, true, true)) {
        clear_win_state();
        return false;
    }
    for (const SandboxPath& extra : spec.extra_paths) {
        if (extra.path.empty())
            continue;
        boost::system::error_code ec;
        const bool                exists = boost::filesystem::exists(extra.path, ec) && !ec;
        if (!exists) {
            if (extra.write) {
                ec.clear();
                boost::filesystem::create_directories(extra.path, ec);
                if (ec) {
                    error = "AppContainer ACL grant failed for " + extra.path;
                    clear_win_state();
                    return false;
                }
            } else {
                // Same skip-with-warning as python_home: a missing read-only grant (fresh
                // python/packages, absent trust.json) must not abort the spawn.
                BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL grant skipped for missing path: " << extra.path;
                continue;
            }
        }
        if (!grant_or_fail(extra.path, extra.write, extra.write && extra.low_label)) {
            clear_win_state();
            return false;
        }
    }

    SID_AND_ATTRIBUTES* cap_ptr = nullptr;
    DWORD               cap_n   = 0;
    if (spec.allow_network && p_DeriveCaps) {
        PSID* group_sids = nullptr;
        DWORD group_n    = 0;
        PSID* cap_sids   = nullptr;
        DWORD cap_count  = 0;
        if (p_DeriveCaps(L"internetClient", &group_sids, &group_n, &cap_sids, &cap_count) && cap_count > 0) {
            g_win.cap_sid             = cap_sids[0];
            g_win.cap_attr.Sid        = cap_sids[0];
            g_win.cap_attr.Attributes = SE_GROUP_ENABLED;
            cap_ptr                   = &g_win.cap_attr;
            cap_n                     = 1;
            for (DWORD i = 1; i < cap_count; ++i)
                ::LocalFree(cap_sids[i]);
            ::LocalFree(cap_sids);
            for (DWORD i = 0; i < group_n; ++i)
                ::LocalFree(group_sids[i]);
            ::LocalFree(group_sids);
        }
    }

    g_win.caps.AppContainerSid = sid;
    g_win.caps.Capabilities    = cap_ptr;
    g_win.caps.CapabilityCount = cap_n;
    g_win.caps.Reserved        = 0;

    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    g_win.attr_list = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(::HeapAlloc(::GetProcessHeap(), 0, attr_size));
    if (!g_win.attr_list || !::InitializeProcThreadAttributeList(g_win.attr_list, 1, 0, &attr_size)) {
        error = "InitializeProcThreadAttributeList failed";
        clear_win_state();
        return false;
    }
    if (!::UpdateProcThreadAttribute(g_win.attr_list, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &g_win.caps, sizeof(g_win.caps),
                                     nullptr, nullptr)) {
        error = "UpdateProcThreadAttribute(SECURITY_CAPABILITIES) failed";
        clear_win_state();
        return false;
    }

    // Named pipe SD. DACL is package SID + current TokenUser + SYSTEM (no Everyone, no ALL
    // APPLICATION PACKAGES, no BA). SACL is Low IL + no-write-up so the host can CreateFile
    // GENERIC_WRITE. See win_build_pipe_security_descriptor for the user-plus-package rule.
    //
    // Other kernel objects shared with the host:
    //   * ConnectNamedPipe / Channel OVERLAPPED events are unnamed and parent-only.
    //   * The Job object is unnamed; the child is assigned by handle, not by name.
    //   * No file mappings. Writable grant directories are labelled Low above.
    // Filesystem grants append a package-SID ACE onto the existing DACL (apply_dir_acl),
    // which already allows the creating user.
    void* sd = nullptr;
    if (!win_build_pipe_security_descriptor(sid, &sd, error) || sd == nullptr) {
        clear_win_state();
        return false;
    }
    g_win.pipe_sd                      = static_cast<PSECURITY_DESCRIPTOR>(sd);
    g_win.pipe_acl                     = nullptr; // DACL/SACL live inside the self-relative SD
    g_win.pipe_sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    g_win.pipe_sa.lpSecurityDescriptor = g_win.pipe_sd;
    g_win.pipe_sa.bInheritHandle       = FALSE;
    g_win.ready                        = true;
    return true;
}

void close_win_appcontainer() { clear_win_state(); }

void sweep_win_acl_journal()
{
    if (!parent_acl_ops_allowed())
        return;
    resolve_apis();
    const std::string live_sid = boost::nowide::narrow(sid_to_string(g_win.sid));
    for (const auto& path : journal_paths_to_sweep()) {
        auto                        sessions = load_journal_sessions_at(path);
        std::vector<JournalSession> leftover;
        leftover.reserve(sessions.size());
        for (JournalSession& session : sessions) {
            if (g_win.ready && !live_sid.empty() && session.package_sid == live_sid) {
                leftover.push_back(std::move(session));
                continue;
            }
            if (!session_sid_allowed(session)) {
                BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL journal dropped untrusted SID " << session.package_sid;
                continue;
            }
            PSID sid = nullptr;
            if (!session.package_sid.empty()) {
                const std::wstring wide = boost::nowide::widen(session.package_sid);
                if (!::ConvertStringSidToSidW(wide.c_str(), &sid))
                    sid = nullptr;
            }
            std::vector<DirGrant> keep;
            keep.reserve(session.grants.size());
            for (const DirGrant& grant : session.grants) {
                if (!grant_path_is_actionable(grant, GrantActionSource::Journal, &session)) {
                    BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL journal skipped path outside known roots: " << grant.path;
                    keep.push_back(grant);
                    continue;
                }
                if (sid) {
                    if (!revoke_one_grant(sid, grant, GrantActionSource::Journal, &session))
                        keep.push_back(grant);
                } else if (grant.low_label && grant.write) {
                    if (!restore_label_sddl(grant.path, grant.original_label, path_in_known_root(grant.path)))
                        keep.push_back(grant);
                }
            }
            if (sid)
                ::LocalFree(sid);
            if (!keep.empty()) {
                session.grants = std::move(keep);
                leftover.push_back(std::move(session));
            }
        }
        persist_sessions_at(path, leftover);
    }
}

std::vector<std::string> win_granted_paths_for_testing()
{
    std::vector<std::string> out;
    out.reserve(g_win.grants.size());
    for (const DirGrant& g : g_win.grants)
        out.push_back(g.path);
    return out;
}

std::wstring win_current_package_sid_string_for_testing() { return sid_to_string(g_win.sid); }

void win_abandon_grants_for_testing()
{
    g_win.grants.clear();
    release_win_handles();
}

bool win_path_has_low_integrity_label(const std::string& path) { return path_has_low_integrity_label(path); }

bool win_apply_low_integrity_label_for_testing(const std::string& path) { return label_low_integrity(path); }

void win_clear_low_integrity_label(const std::string& path)
{
    g_low_label_used_copy_replace = false;
    restore_label_sddl(path, {});
}

std::string win_acl_journal_path_for_testing()
{
    if (!g_win.journal_path.empty())
        return g_win.journal_path.string();
    return acl_journal_path_for(Slic3r::data_dir()).string();
}

// Permanent uninstall of this exact plugin_key only. close/sweep/test teardown must not
// call this for a profile they did not create in this process.
void delete_win_appcontainer_profile(const std::string& plugin_key)
{
    resolve_apis();
    if (!p_DeleteProfile || plugin_key.empty())
        return;
    const std::wstring name = profile_name(plugin_key);
    const HRESULT      hr   = p_DeleteProfile(name.c_str());
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        return;
    BOOST_LOG_TRIVIAL(warning) << "DeleteAppContainerProfile failed for " << plugin_key;
}

void delete_win_test_created_appcontainer_profile(const std::string& plugin_key)
{
    if (g_created_profile_keys.erase(plugin_key) == 0)
        return;
    delete_win_appcontainer_profile(plugin_key);
}

void set_test_only_plugin_keys_for_testing(bool enable) { g_test_only_plugin_keys.store(enable, std::memory_order_release); }

bool win_create_appcontainer_profile_for_testing(const std::string& plugin_key, std::string& error)
{
    resolve_apis();
    if (!p_CreateProfile || plugin_key.empty()) {
        error = "CreateAppContainerProfile APIs are not available";
        return false;
    }
    const std::wstring name = profile_name(plugin_key);
    PSID               sid  = nullptr;
    HRESULT            hr = p_CreateProfile(name.c_str(), L"EdgeSlicer plugin host", L"OS sandbox for edge-plugin-host", nullptr, 0, &sid);
    if (sid != nullptr)
        ::FreeSid(sid);
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        return true;
    error = "CreateAppContainerProfile failed";
    return false;
}

bool win_appcontainer_profile_exists_for_testing(const std::string& plugin_key)
{
    // Do not read the LOCALAPPDATA env var: REAPER (and other harnesses) redirect it, while
    // AppContainer profiles still live under the real known-folder LocalAppData.
    resolve_apis();
    if (plugin_key.empty())
        return false;
    const std::wstring name = profile_name(plugin_key);
    namespace fs            = boost::filesystem;
    auto folder_exists      = [](const fs::path& path) {
        boost::system::error_code ec;
        return fs::exists(path, ec) && !ec;
    };

    if (p_DeriveSid && p_GetAcFolder) {
        PSID sid = nullptr;
        if (SUCCEEDED(p_DeriveSid(name.c_str(), &sid)) && sid != nullptr) {
            const std::wstring sid_str = sid_to_string(sid);
            ::FreeSid(sid);
            PWSTR ac_path = nullptr;
            if (!sid_str.empty() && SUCCEEDED(p_GetAcFolder(sid_str.c_str(), &ac_path)) && ac_path != nullptr) {
                const bool exists = folder_exists(ac_path);
                ::CoTaskMemFree(ac_path);
                return exists;
            }
        } else if (sid != nullptr) {
            ::FreeSid(sid);
        }
    }

    PWSTR local = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) || local == nullptr)
        return false;
    const fs::path packages = fs::path(local) / L"Packages";
    ::CoTaskMemFree(local);
    boost::system::error_code ec;
    if (!fs::is_directory(packages, ec) || ec)
        return false;
    for (fs::directory_iterator it(packages, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring folder = it->path().filename().wstring();
        if (folder.size() >= name.size() && ::_wcsnicmp(folder.c_str(), name.c_str(), name.size()) == 0)
            return true;
    }
    return false;
}

bool win_low_label_used_copy_replace_for_testing() { return g_low_label_used_copy_replace; }

std::wstring win_appcontainer_localappdata_path()
{
    resolve_apis();
    if (g_win.sid == nullptr || p_GetAcFolder == nullptr)
        return {};
    const std::wstring sid = sid_to_string(g_win.sid);
    if (sid.empty())
        return {};
    PWSTR path = nullptr;
    if (FAILED(p_GetAcFolder(sid.c_str(), &path)) || path == nullptr)
        return {};
    std::wstring out = path;
    ::CoTaskMemFree(path);
    return out;
}

std::wstring win_appcontainer_profile_name(const std::string& plugin_key) { return profile_name(plugin_key); }

void* win_security_capabilities() { return g_win.ready ? g_win.attr_list : nullptr; }

void* win_pipe_security_attributes() { return g_win.ready ? &g_win.pipe_sa : nullptr; }

bool apply_win_child_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error)
{
    report         = {};
    report.network = spec.allow_network;
    report.exec    = spec.allow_exec;
    HANDLE token   = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error         = "OpenProcessToken failed";
        report.detail = error;
        return false;
    }
    DWORD      is_ac = 0;
    DWORD      n     = sizeof(is_ac);
    const BOOL ok    = ::GetTokenInformation(token, TokenIsAppContainer, &is_ac, n, &n);
    ::CloseHandle(token);
    if (!ok || !is_ac) {
        error         = "process is not in an AppContainer";
        report.kind   = k_sandbox_kind_none;
        report.detail = error;
        return false;
    }
    report.applied = true;
    report.kind    = k_sandbox_kind_appcontainer;
    report.detail  = "AppContainer";
    return true;
}

} // namespace Slic3r::plugin_ipc

#endif
