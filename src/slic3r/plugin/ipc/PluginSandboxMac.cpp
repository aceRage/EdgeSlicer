#include "PluginSandboxPriv.hpp"

#include <boost/nowide/cstdlib.hpp>

#ifndef __APPLE__

namespace Slic3r::plugin_ipc {

bool apply_mac_sandbox(const SandboxSpec&, SandboxReport& report, std::string& error)
{
    error         = "macOS App Sandbox is not available on this OS";
    report.kind   = k_sandbox_kind_none;
    report.detail = error;
    return false;
}

bool probe_mac_sandbox(SandboxProbe& probe)
{
    probe.available = false;
    probe.reason    = "not macOS";
    return false;
}

} // namespace Slic3r::plugin_ipc

#else

#include "PluginSandboxMacSeatbelt.hpp"

#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unistd.h>

namespace Slic3r::plugin_ipc {
namespace {

using sandbox_check_fn = int (*)(pid_t, const char*, int);

bool containerized()
{
    // APP_SANDBOX_CONTAINER_ID is inherited and spoofable (launchctl setenv). The kernel
    // view is sandbox_check(getpid(), NULL, 0): 1 means sandboxed, 0 means not, -1 is error.
    static sandbox_check_fn check = []() -> sandbox_check_fn {
        return reinterpret_cast<sandbox_check_fn>(::dlsym(RTLD_DEFAULT, "sandbox_check"));
    }();
    if (check == nullptr)
        return false;
    return check(::getpid(), nullptr, 0) == 1;
}

// DEV/TEST PROBE ONLY. Not the shipping sandbox (that is a signed helper with
// com.apple.security.app-sandbox). sandbox-exec(1) is the CLI equivalent of this
// SBPL sandbox_init probe. Honoured only when EDGE_PLUGIN_SANDBOX_SEATBELT_PROBE
// is set through the Phase 7 test env-override gate. Named no-internet / no-write
// are not applied (they block the handshake unix socket). Current MacOSX.sdk
// headers dropped the old named-profile string constants; the probe never names
// those identifiers.
bool apply_seatbelt_probe(const SandboxSpec& spec, SandboxReport& report, std::string& error)
{
    if (!mac_seatbelt::apply_probe(spec.allow_network, error)) {
        report.kind   = k_sandbox_kind_none;
        report.detail = error;
        return false;
    }
    report.applied = true;
    report.kind    = k_sandbox_kind_seatbelt_probe;
    report.detail  = "DEV/TEST: sandbox_init Seatbelt probe; not the shipping App Sandbox";
    report.network = spec.allow_network;
    report.exec    = spec.allow_exec;
    return true;
}

} // namespace

bool probe_mac_sandbox(SandboxProbe& probe)
{
    probe.app_sandbox = containerized();
    probe.available   = probe.app_sandbox;
    probe.reason      = probe.app_sandbox ? "sandbox_check reports this process is in an App Sandbox container" :
                                            "helper is not in an App Sandbox container (unsigned or missing entitlements)";
    return probe.available;
}

bool apply_mac_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error)
{
    report         = {};
    report.network = spec.allow_network;
    report.exec    = spec.allow_exec;

    if (spec.dev_seatbelt_probe)
        return apply_seatbelt_probe(spec, report, error);

    if (!containerized()) {
        error         = "unsigned or un-entitled helper: macOS App Sandbox is not active";
        report.kind   = k_sandbox_kind_none;
        report.detail = error;
        return false;
    }
    report.applied = true;
    report.kind    = k_sandbox_kind_app_sandbox;
    report.detail  = "com.apple.security.app-sandbox (signed helper)";
    return true;
}

} // namespace Slic3r::plugin_ipc

#endif
