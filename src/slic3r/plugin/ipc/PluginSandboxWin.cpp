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

#include <boost/nowide/convert.hpp>

#include <cstring>
#include <string>
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
    std::wstring name = L"EdgeSlicer.pluginhost";
    if (plugin_key.empty())
        return name;
    name += L".";
    for (unsigned char c : plugin_key) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            name += static_cast<wchar_t>(c);
        else
            name += L'_';
        if (name.size() > 50)
            break;
    }
    return name;
}

bool grant_dir_acl(PSID sid, const std::string& path, bool write)
{
    if (path.empty() || sid == nullptr)
        return true;
    const std::wstring wide = boost::nowide::widen(path);
    EXPLICIT_ACCESSW   ea{};
    ea.grfAccessPermissions = write ? (GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE) : (GENERIC_READ | GENERIC_EXECUTE);
    ea.grfAccessMode        = SET_ACCESS;
    ea.grfInheritance       = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName    = static_cast<LPWSTR>(sid);

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
    PSID                         sid     = nullptr;
    PSID                         cap_sid = nullptr;
    SID_AND_ATTRIBUTES           cap_attr{};
    SECURITY_CAPABILITIES        caps{};
    LPPROC_THREAD_ATTRIBUTE_LIST attr_list = nullptr;
    SECURITY_ATTRIBUTES          pipe_sa{};
    PSECURITY_DESCRIPTOR         pipe_sd  = nullptr;
    PACL                         pipe_acl = nullptr;
    bool                         ready    = false;
};

WinState g_win;

void clear_win_state()
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

    grant_dir_acl(sid, spec.python_home, false);
    grant_dir_acl(sid, spec.plugin_root, true);
    grant_dir_acl(sid, spec.write_root, true);
    for (const SandboxPath& extra : spec.extra_paths)
        grant_dir_acl(sid, extra.path, extra.write);

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

    EXPLICIT_ACCESSW ea[2]{};
    ea[0].grfAccessPermissions      = GENERIC_ALL;
    ea[0].grfAccessMode             = SET_ACCESS;
    ea[0].grfInheritance            = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm       = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType       = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[0].Trustee.ptstrName         = static_cast<LPWSTR>(sid);
    SID_IDENTIFIER_AUTHORITY nt     = SECURITY_NT_AUTHORITY;
    PSID                     admins = nullptr;
    ::AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins);
    ea[1]                   = ea[0];
    ea[1].Trustee.ptstrName = static_cast<LPWSTR>(admins);
    PACL acl                = nullptr;
    ::SetEntriesInAclW(admins ? 2 : 1, ea, nullptr, &acl);
    if (admins)
        ::FreeSid(admins);
    PSECURITY_DESCRIPTOR sd = ::LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    if (!sd || !acl || !::InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION) ||
        !::SetSecurityDescriptorDacl(sd, TRUE, acl, FALSE)) {
        error = "AppContainer named-pipe DACL failed";
        if (acl)
            ::LocalFree(acl);
        if (sd)
            ::LocalFree(sd);
        clear_win_state();
        return false;
    }
    g_win.pipe_sd                      = sd;
    g_win.pipe_acl                     = acl;
    g_win.pipe_sa.nLength              = sizeof(SECURITY_ATTRIBUTES);
    g_win.pipe_sa.lpSecurityDescriptor = sd;
    g_win.pipe_sa.bInheritHandle       = FALSE;
    g_win.ready                        = true;
    return true;
}

void close_win_appcontainer() { clear_win_state(); }

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
