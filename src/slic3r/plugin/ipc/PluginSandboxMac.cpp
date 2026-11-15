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

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unistd.h>

// sandbox_init is the in-process Seatbelt API. The shipping helper is a signed binary with
// com.apple.security.app-sandbox entitlements; sandbox-exec(1) and this probe are test-only.
#include <sandbox.h>

namespace Slic3r::plugin_ipc {
namespace {

bool containerized()
{
    const char* id = ::getenv("APP_SANDBOX_CONTAINER_ID");
    return id != nullptr && id[0] != '\0';
}

#ifndef SANDBOX_NAMED
#define SANDBOX_NAMED 0x0001
#endif

// DEV/TEST PROBE ONLY. Not the shipping sandbox (that is a signed helper with
// com.apple.security.app-sandbox). sandbox-exec(1) is the CLI equivalent of this
// sandbox_init(kSBXProfileNoInternet) call. Honoured only when
// EDGE_PLUGIN_SANDBOX_SEATBELT_PROBE is set through the Phase 7 test env-override gate.
bool apply_seatbelt_probe(const SandboxSpec& spec, SandboxReport& report, std::string& error)
{
    char*       sandbox_error = nullptr;
    const char* profile       = spec.allow_network ? kSBXProfileNoWrite : kSBXProfileNoInternet;
    if (sandbox_init(profile, SANDBOX_NAMED, &sandbox_error) != 0) {
        error = sandbox_error ? sandbox_error : "sandbox_init (Seatbelt probe) failed";
        if (sandbox_error)
            sandbox_free_error(sandbox_error);
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
    probe.reason      = probe.app_sandbox ? "APP_SANDBOX_CONTAINER_ID is set (signed helper with app-sandbox entitlements)" :
                                            "helper is not in an App Sandbox container (unsigned or missing entitlements)";
    return probe.available;
}

bool apply_mac_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error)
{
    report         = {};
    report.network = spec.allow_network;
    report.exec    = spec.allow_exec;

    if (sandbox_env_seatbelt_probe())
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
