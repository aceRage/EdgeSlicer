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
#include <sddl.h>
#include <userenv.h>
#ifndef PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES
#define PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES ProcThreadAttributeValue(9, FALSE, TRUE, FALSE)
#endif

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::plugin_ipc {
namespace {

using CreateAppContainerProfile_fn                 = HRESULT(WINAPI*)(PCWSTR, PCWSTR, PCWSTR, PSID_AND_ATTRIBUTES, DWORD, PSID*);
using DeriveAppContainerSidFromAppContainerName_fn = HRESULT(WINAPI*)(PCWSTR, PSID*);
using DeriveCapabilitySidsFromName_fn              = BOOL(WINAPI*)(LPCWSTR, PSID**, DWORD*, PSID**, DWORD*);
using DeleteAppContainerProfile_fn                 = HRESULT(WINAPI*)(PCWSTR);

CreateAppContainerProfile_fn                 p_CreateProfile = nullptr;
DeriveAppContainerSidFromAppContainerName_fn p_DeriveSid     = nullptr;
DeriveCapabilitySidsFromName_fn              p_DeriveCaps    = nullptr;
DeleteAppContainerProfile_fn                 p_DeleteProfile = nullptr;
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
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr) ||
        sd == nullptr)
        return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL sacl    = nullptr;
    DWORD rc     = ERROR_INVALID_PARAMETER;
    if (::GetSecurityDescriptorSacl(sd, &present, &sacl, &defaulted) && present && sacl != nullptr) {
        const std::wstring wide = boost::nowide::widen(path);
        rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr,
                                     nullptr, nullptr, sacl);
    }
    ::LocalFree(sd);
    return rc == ERROR_SUCCESS;
}

bool grant_dir_acl(PSID sid, const std::string& path, bool write, ACCESS_MODE mode)
{
    if (path.empty() || sid == nullptr)
        return true;
    const std::wstring wide = boost::nowide::widen(path);
    EXPLICIT_ACCESSW   ea{};
    ea.grfAccessPermissions = write ? (GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE) : (GENERIC_READ | GENERIC_EXECUTE);
    ea.grfAccessMode        = mode;
    // One SetNamedSecurityInfoW. An inheritable ACE does propagate to existing
    // children (that is the grant). Do not SET then immediately REVOKE — that was
    // the redundant second walk. Grants live until close_win_appcontainer.
    ea.grfInheritance      = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
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

struct WinState
{
    PSID                                      sid     = nullptr;
    PSID                                      cap_sid = nullptr;
    SID_AND_ATTRIBUTES                        cap_attr{};
    SECURITY_CAPABILITIES                     caps{};
    LPPROC_THREAD_ATTRIBUTE_LIST              attr_list = nullptr;
    SECURITY_ATTRIBUTES                       pipe_sa{};
    PSECURITY_DESCRIPTOR                      pipe_sd  = nullptr;
    PACL                                      pipe_acl = nullptr;
    std::vector<std::pair<std::string, bool>> grants;
    bool                                      ready = false;
};

WinState g_win;

void revoke_grants()
{
    if (g_win.sid) {
        for (const auto& grant : g_win.grants)
            grant_dir_acl(g_win.sid, grant.first, grant.second, REVOKE_ACCESS);
    }
    g_win.grants.clear();
}

void clear_win_state()
{
    revoke_grants();
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

} // namespace

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

    auto grant_or_fail = [&](const std::string& path, bool write) {
        if (path.empty())
            return true;
        if (!grant_dir_acl(sid, path, write, SET_ACCESS)) {
            error = "AppContainer ACL grant failed for " + path;
            return false;
        }
        g_win.grants.emplace_back(path, write);
        // Writable grants are created by this process at medium IL. Without a low label the
        // AppContainer child cannot create files there (same no-write-up as the named pipe).
        if (write && !label_low_integrity(path))
            BOOST_LOG_TRIVIAL(warning) << "AppContainer low-IL label skipped for " << path;
        return true;
    };
    // python_home is often under Program Files: a standard user has no WRITE_DAC, and
    // ALL APPLICATION PACKAGES can already read it. Skip rather than fail the spawn.
    if (!spec.python_home.empty()) {
        if (grant_dir_acl(sid, spec.python_home, false, SET_ACCESS))
            g_win.grants.emplace_back(spec.python_home, false);
        else
            BOOST_LOG_TRIVIAL(warning) << "AppContainer ACL grant skipped for python_home (WRITE_DAC): " << spec.python_home;
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
    if (!grant_or_fail(spec.plugin_root, false) || !grant_or_fail(spec.write_root, true)) {
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
        if (!grant_or_fail(extra.path, extra.write)) {
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

    // Named pipe SD. DACL is the container SID + Admins only (no Everyone, no ALL APPLICATION
    // PACKAGES). SACL is Low IL + no-write-up so the AppContainer host can CreateFile GENERIC_WRITE.
    //
    // Other kernel objects shared with the host:
    //   * ConnectNamedPipe / Channel OVERLAPPED events are unnamed and parent-only.
    //   * The Job object is unnamed; the child is assigned by handle, not by name.
    //   * No file mappings. Writable grant directories are labelled Low above.
    LPWSTR sid_str = nullptr;
    if (!::ConvertSidToStringSidW(sid, &sid_str) || sid_str == nullptr) {
        error = "ConvertSidToStringSid failed for AppContainer SID";
        clear_win_state();
        return false;
    }
    std::wstring sddl = L"D:(A;;GA;;;";
    sddl += sid_str;
    sddl += L")(A;;GA;;;BA)S:(ML;;NW;;;LW)";
    ::LocalFree(sid_str);

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr) || sd == nullptr) {
        error = "AppContainer named-pipe security descriptor failed";
        clear_win_state();
        return false;
    }
    g_win.pipe_sd                      = sd;
    g_win.pipe_acl                     = nullptr; // DACL/SACL live inside the self-relative SD
    g_win.pipe_sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    g_win.pipe_sa.lpSecurityDescriptor = sd;
    g_win.pipe_sa.bInheritHandle       = FALSE;
    g_win.ready                        = true;
    return true;
}

void close_win_appcontainer() { clear_win_state(); }

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
