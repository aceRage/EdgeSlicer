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
#include <userenv.h>
#ifndef PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES
#define PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES ProcThreadAttributeValue(9, FALSE, TRUE, FALSE)
#endif

#include <libslic3r/Utils.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
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

std::wstring profile_name(const std::string& plugin_key)
{
    // Fixed-length name so two long keys cannot truncate to the same profile.
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : plugin_key) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    wchar_t buf[48];
    swprintf(buf, 48, L"EdgeSlicer.ph.%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

// Low IL (S-1-16-4096) + no-write-up. AppContainer hosts run at low integrity; a medium
// object with only a DACL still denies GENERIC_WRITE (ERROR_ACCESS_DENIED / no-write-up).
bool label_low_integrity(const std::string& path)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr) || sd == nullptr)
        return false;
    BOOL  present = FALSE, defaulted = FALSE;
    PACL  sacl = nullptr;
    DWORD rc   = ERROR_INVALID_PARAMETER;
    if (::GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted) && present && sacl != nullptr) {
        const std::wstring wide = boost::nowide::widen(path);
        rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                                     nullptr, sacl);
    }
    ::LocalFree(sd);
    return rc == ERROR_SUCCESS;
}

// Subfolder grants: inheritable GENERIC_READ|EXECUTE (plus write/delete when write=true).
// data_dir itself: non-inherited FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE so
// GetFileAttributes/status() on the folder succeeds without FILE_LIST_DIRECTORY.
constexpr DWORD k_acl_ro            = GENERIC_READ | GENERIC_EXECUTE;
constexpr DWORD k_acl_rw            = GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE;
constexpr DWORD k_acl_inherit       = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
constexpr DWORD k_data_dir_traverse = FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE;

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
    if (::GetNamedSecurityInfoW(wide.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &prev, nullptr, &sd) !=
        ERROR_SUCCESS)
        sd = nullptr;
    if (::SetEntriesInAclW(1, &ea, prev, &neu) != ERROR_SUCCESS) {
        if (sd)
            ::LocalFree(sd);
        return false;
    }
    const DWORD rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                             neu, nullptr);
    ::LocalFree(neu);
    if (sd)
        ::LocalFree(sd);
    return rc == ERROR_SUCCESS;
}

struct DirGrant
{
    std::string  path;
    DWORD        permissions = 0;
    DWORD        inheritance = 0;
    bool         write       = false;
    bool         low_label   = false;
    std::wstring original_label; // LABEL_SECURITY_INFORMATION SDDL; empty = no ML ACE
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
    bool                         ready = false;
};

WinState g_win;

boost::filesystem::path acl_journal_path()
{
    const std::string dir = Slic3r::data_dir();
    if (dir.empty())
        return {};
    return boost::filesystem::path(dir) / "plugin_host" / "acl_journal.json";
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

bool restore_label_sddl(const std::string& path, const std::wstring& sddl)
{
    const std::wstring wide = boost::nowide::widen(path);
    if (sddl.empty()) {
        // No original ML ACE: restore the default (no label / Medium) by writing an empty SACL.
        BYTE  buf[sizeof(ACL) + 16]{};
        auto* acl = reinterpret_cast<PACL>(buf);
        if (!::InitializeAcl(acl, sizeof(buf), ACL_REVISION))
            return false;
        return ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                                       nullptr, acl) == ERROR_SUCCESS;
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) || sd == nullptr)
        return false;
    BOOL  present = FALSE, defaulted = FALSE;
    PACL  sacl = nullptr;
    DWORD rc   = ERROR_INVALID_PARAMETER;
    if (::GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted))
        rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr,
                                     nullptr, present ? sacl : nullptr);
    ::LocalFree(sd);
    return rc == ERROR_SUCCESS;
}

void delete_acl_journal()
{
    const auto path = acl_journal_path();
    if (path.empty())
        return;
    boost::system::error_code ec;
    boost::filesystem::remove(path, ec);
}

bool persist_acl_journal(PSID sid)
{
    const auto path = acl_journal_path();
    if (path.empty())
        return true;
    boost::system::error_code ec;
    boost::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return false;
    nlohmann::json grants = nlohmann::json::array();
    for (const DirGrant& g : g_win.grants) {
        grants.push_back({
            {"path", g.path},
            {"permissions", g.permissions},
            {"inheritance", g.inheritance},
            {"write", g.write},
            {"low_label", g.low_label},
            {"original_label", boost::nowide::narrow(g.original_label)},
        });
    }
    nlohmann::json body = {{"package_sid", boost::nowide::narrow(sid_to_string(sid))}, {"grants", std::move(grants)}};
    std::string    err;
    return Slic3r::write_file_atomically(path.string(), body.dump(1, '\t') + "\n", &err);
}

void revoke_one_grant(PSID sid, const DirGrant& grant)
{
    apply_dir_acl(sid, grant.path, grant.permissions, grant.inheritance, REVOKE_ACCESS);
    if (grant.low_label)
        restore_label_sddl(grant.path, grant.original_label);
}

void revoke_grants()
{
    if (g_win.sid) {
        for (const auto& grant : g_win.grants)
            revoke_one_grant(g_win.sid, grant);
    }
    g_win.grants.clear();
    delete_acl_journal();
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
    g_win.ready   = false;
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
bool package_already_has_rx(const std::string& path, PSID package_sid)
{
    PSID any_sid = nullptr;
    if (!::ConvertStringSidToSidW(L"S-1-15-2-1", &any_sid) || any_sid == nullptr)
        any_sid = nullptr;

    const std::wstring   wide = boost::nowide::widen(path);
    PACL                 dacl = nullptr;
    PSECURITY_DESCRIPTOR sd   = nullptr;
    const DWORD rc = ::GetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                             &dacl, nullptr, &sd);
    bool        allowed = false;
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
                const bool ours        = (package_sid != nullptr && ::EqualSid(ace_sid, package_sid)) ||
                                  (any_sid != nullptr && ::EqualSid(ace_sid, any_sid));
                if (!ours)
                    continue;
                ACCESS_MASK mask = allowed_ace->Mask;
                ::MapGenericMask(&mask, &mapping);
                if (header->AceType == ACCESS_DENIED_ACE_TYPE && (mask & need) != 0) {
                    allowed = false;
                    break;
                }
                if (header->AceType == ACCESS_ALLOWED_ACE_TYPE && (mask & need) == need)
                    allowed = true;
            }
        }
    }
    if (sd)
        ::LocalFree(sd);
    if (any_sid)
        ::LocalFree(any_sid);
    return allowed;
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
    sweep_win_acl_journal();
    clear_win_state();
    if (!g_apis_ok) {
        error = "AppContainer APIs are not available; refusing to start the plugin host unsandboxed";
        return false;
    }

    const std::wstring name = profile_name(spec.plugin_key);
    PSID               sid  = nullptr;
    HRESULT            hr = p_CreateProfile(name.c_str(), L"EdgeSlicer plugin host", L"OS sandbox for edge-plugin-host", nullptr, 0, &sid);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        hr = p_DeriveSid(name.c_str(), &sid);
    }
    if (FAILED(hr) || sid == nullptr) {
        error = "CreateAppContainerProfile failed";
        return false;
    }
    g_win.sid = sid;

    auto grant_or_fail = [&](const std::string& path, bool write, bool apply_low) {
        if (path.empty())
            return true;
        const DWORD perms = write ? k_acl_rw : k_acl_ro;
        // Every read-only grant (helper dir, resources_dir, python_home, packages, plugin_root,
        // extras): skip if the package half of the AppContainer check already allows RX.
        if (!write && package_already_has_rx(path, sid)) {
            BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL grant skipped (package already has RX): " << path;
            return true;
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
    if (g_win.ready)
        return;
    const auto path = acl_journal_path();
    if (path.empty())
        return;
    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec) || ec)
        return;
    boost::nowide::ifstream in(path.string());
    if (!in) {
        BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL journal unreadable: " << path.string();
        return;
    }
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
        BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL journal invalid; deleting " << path.string();
        delete_acl_journal();
        return;
    }
    const std::string sid_utf8 = json.value("package_sid", std::string());
    PSID              sid      = nullptr;
    if (!sid_utf8.empty()) {
        const std::wstring wide = boost::nowide::widen(sid_utf8);
        if (!::ConvertStringSidToSidW(wide.c_str(), &sid))
            sid = nullptr;
    }
    if (json.contains("grants") && json["grants"].is_array()) {
        for (const auto& item : json["grants"]) {
            if (!item.is_object() || !item.contains("path") || !item["path"].is_string())
                continue;
            DirGrant grant;
            grant.path           = item["path"].get<std::string>();
            grant.permissions    = static_cast<DWORD>(item.value("permissions", 0u));
            grant.inheritance    = static_cast<DWORD>(item.value("inheritance", 0u));
            grant.write          = item.value("write", false);
            grant.low_label      = item.value("low_label", false);
            grant.original_label = boost::nowide::widen(item.value("original_label", std::string()));
            if (sid)
                revoke_one_grant(sid, grant);
            else if (grant.low_label)
                restore_label_sddl(grant.path, grant.original_label);
        }
    }
    if (sid)
        ::LocalFree(sid);
    delete_acl_journal();
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
