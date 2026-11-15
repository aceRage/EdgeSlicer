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

// Named profiles historically selected via the sandbox.h string constants (no-internet / no-write).
inline const char* named_profile(bool allow_network) { return allow_network ? "no-write" : "no-internet"; }

inline const char* sbpl_profile(bool allow_network)
{
    // Minimal labelled probe. Shipping confinement is App Sandbox, not this SBPL.
    return allow_network ? "(version 1)\n(allow default)\n(deny file-write*)\n" : "(version 1)\n(allow default)\n(deny network*)\n";
}

// Returns true only when a Seatbelt profile was applied. Any miss is fail-closed.
inline bool apply_probe(bool allow_network, std::string& error)
{
    sandbox_init_fn init_fn = resolve_sandbox_init();
    if (init_fn == nullptr) {
        error = "sandbox_init unavailable (Seatbelt probe); refusing to run unsandboxed";
        return false;
    }

    char*     sandbox_error = nullptr;
    const int named_rc      = init_fn(named_profile(allow_network), SANDBOX_NAMED, &sandbox_error);
    if (named_rc == 0)
        return true;

    consume_sandbox_error(sandbox_error, error, "sandbox_init named profile (Seatbelt probe) failed");
    sandbox_error = nullptr;

    const int sbpl_rc = init_fn(sbpl_profile(allow_network), 0, &sandbox_error);
    if (sbpl_rc == 0) {
        error.clear();
        return true;
    }

    std::string sbpl_error;
    consume_sandbox_error(sandbox_error, sbpl_error, "sandbox_init SBPL (Seatbelt probe) failed");
    error += "; " + sbpl_error;
    return false;
}

} // namespace Slic3r::plugin_ipc::mac_seatbelt
