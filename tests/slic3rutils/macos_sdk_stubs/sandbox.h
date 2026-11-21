#ifndef EDGE_PLUGIN_MACOS_SDK_STUB_SANDBOX_H
#define EDGE_PLUGIN_MACOS_SDK_STUB_SANDBOX_H

// Stub of MacOSX.sdk 27.2 <sandbox.h>: sandbox_init is still declared (deprecated)
// and the old named-profile string constants are gone. Used by
// scripts/check_macos_sandbox_sdk.sh; not a shipping header.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SANDBOX_NAMED 0x0001

int sandbox_init(const char *profile, uint64_t flags, char **errorbuf)
    __attribute__((deprecated("sandbox_init is deprecated; Edge uses it only as a labelled probe")));

void sandbox_free_error(char *errorbuf);

#ifdef __cplusplus
}
#endif

#endif
