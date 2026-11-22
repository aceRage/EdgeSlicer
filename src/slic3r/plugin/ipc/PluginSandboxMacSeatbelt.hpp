#pragma once

// DEV/TEST Seatbelt probe helpers for macOS. Not the shipping sandbox (that is a signed
// helper with com.apple.security.app-sandbox). Isolated so a stub sandbox.h (current
// Apple SDKs: no named-profile string constants, sandbox_init deprecated) can syntax-check it.

#include <cstdint>
#include <dlfcn.h>
#include <string>

#ifndef RTLD_DEFAULT
#define RTLD_DEFAULT ((void*) 0)
#endif

#if defined(__has_include)
#if __has_include(<sandbox.h>)
#define EDGE_PLUGIN_HAVE_SANDBOX_H 1
// Current SDKs still ship sandbox.h but mark sandbox_init deprecated and omit
// the old named-profile string constants. Ingest the header under a deprecation
// guard so -Werror=deprecated-declarations cannot fail the build; do not call the
// declared symbol (its tbd may also vanish). The probe dlsyms by name instead.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <sandbox.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
#endif

#ifndef EDGE_PLUGIN_HAVE_SANDBOX_H
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
extern "C" {
int sandbox_init(const char* profile, std::uint64_t flags, char** errorbuf)
    __attribute__((deprecated("sandbox_init is a labelled probe; shipping uses App Sandbox")));
void sandbox_free_error(char* errorbuf);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

#ifndef SANDBOX_NAMED
#define SANDBOX_NAMED 0x0001
#endif

namespace Slic3r::plugin_ipc::mac_seatbelt {

using sandbox_init_fn       = int (*)(const char*, std::uint64_t, char**);
using sandbox_free_error_fn = void (*)(char*);

inline sandbox_init_fn resolve_sandbox_init() { return reinterpret_cast<sandbox_init_fn>(::dlsym(RTLD_DEFAULT, "sandbox_init")); }

inline sandbox_free_error_fn resolve_sandbox_free_error()
{
    return reinterpret_cast<sandbox_free_error_fn>(::dlsym(RTLD_DEFAULT, "sandbox_free_error"));
}

inline void consume_sandbox_error(char* sandbox_error, std::string& error, const char* fallback)
{
    error = (sandbox_error && sandbox_error[0] != '\0') ? sandbox_error : fallback;
    if (sandbox_error) {
        if (sandbox_free_error_fn free_fn = resolve_sandbox_free_error())
            free_fn(sandbox_error);
    }
}

// Historical named-profile strings. The probe does not apply them: no-internet
// treats AF_UNIX as network, and no-write denies writes on the handshake socket,
// so hello_ok never arrived. Kept so the stub-SDK compile check still names them.
inline const char* named_profile(bool allow_network) { return allow_network ? "no-write" : "no-internet"; }

inline const char* sbpl_profile(bool allow_network)
{
    (void) allow_network;
    // IPC-safe labelled probe. (allow default) keeps the unix socketpair, Boost.Log,
    // and already-open FDs usable. Deny a path the helper never writes so sandbox_init
    // is still observable. Shipping confinement is App Sandbox, not this SBPL.
    return "(version 1)\n(allow default)\n(deny file-write* (subpath \"/var/empty\"))\n";
}

// Returns true only when a Seatbelt profile was applied. Any miss is fail-closed.
// Named no-internet / no-write are never applied (they block hello_ok).
inline bool apply_probe(bool allow_network, std::string& error)
{
    sandbox_init_fn init_fn = resolve_sandbox_init();
    if (init_fn == nullptr) {
        error = "sandbox_init unavailable (Seatbelt probe); refusing to run unsandboxed";
        return false;
    }

    char*     sandbox_error = nullptr;
    const int rc            = init_fn(sbpl_profile(allow_network), 0, &sandbox_error);
    if (rc == 0) {
        error.clear();
        return true;
    }

    consume_sandbox_error(sandbox_error, error, "sandbox_init SBPL (Seatbelt probe) failed");
    return false;
}

} // namespace Slic3r::plugin_ipc::mac_seatbelt
