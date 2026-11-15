// Syntax-only compile check for the macOS Seatbelt probe against a current-SDK
// stub (no kSBXProfile* constants, deprecated sandbox_init). Invoked by
// scripts/check_macos_sandbox_sdk.sh. Not linked into slic3rutils_tests.

#include <slic3r/plugin/ipc/PluginSandboxMacSeatbelt.hpp>

#include <string>

int main()
{
    using namespace Slic3r::plugin_ipc::mac_seatbelt;
    std::string error;
    (void) named_profile(false);
    (void) named_profile(true);
    (void) sbpl_profile(false);
    (void) sbpl_profile(true);
    (void) apply_probe(false, error);
    (void) apply_probe(true, error);
    return 0;
}
