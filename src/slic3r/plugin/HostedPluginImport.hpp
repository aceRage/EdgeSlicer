#pragma once

// D3 (owner, 2026-11-20): plugin import runs in the sandboxed helper (edge-plugin-host), not in
// EdgeSlicer. EdgeSlicer keeps a registry entry for the plugin whose capabilities are proxies:
// they carry what the helper reported (name, type, config UI, default config) and run the plugin
// through the helper. No plugin code runs in EdgeSlicer for a hosted plugin.
//
// D2 (owner, 2026-11-20): a plugin that is not signed by a shipped publisher key (approved only by
// a local pin) is always hosted: never imported in EdgeSlicer and never run on the early
// (in-process) slicing steps, whatever the preferences say.
//
// The one in-process mode left: a plugin signed by a shipped publisher key, with
// "Allow plugins on the early slicing steps (unsandboxed)" on, is imported in EdgeSlicer so its
// slicing-pipeline capabilities can see the live Print on the early steps (Orca's API hands them
// raw Print* / PrintObject*, which cannot cross a process boundary).

#include "PluginTrust.hpp"
#include "PythonPluginInterface.hpp"
#include "ipc/PluginIpcProtocol.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
struct Plugin;
struct PluginDescriptor;
} // namespace Slic3r

namespace Slic3r::plugin_import {

enum class ImportPlacement { InProcess, Helper };

struct ImportPolicyInput
{
    bool                   in_helper_process{false};       // this process is edge-plugin-host
    bool                   in_process_for_testing{false};  // test seam (never set by release code)
    plugin_trust::TrustKind trust{plugin_trust::TrustKind::Untrusted};
    bool                   shipped_publisher{false};       // the signature verified against a shipped key
    bool                   in_process_steps_pref{false};   // Preferences: early slicing steps on
};

// Pure decision, see the header comment. The helper always imports in its own process.
ImportPlacement decide_import_placement(const ImportPolicyInput& input);

// D2 / D4: may a capability of this plugin run on the early (in-process) slicing steps?
bool early_steps_allowed(plugin_trust::TrustKind trust, bool shipped_publisher, bool in_process_steps_pref);

// The live decision for one plugin: this process, the test seam, the trust EdgeSlicer just
// evaluated and the preference.
ImportPlacement placement_for(const plugin_trust::TrustState& trust);

const char* placement_name(ImportPlacement placement);

// Test seam: the existing plugin tests import fixtures in-process (they call into capabilities
// directly). The D3 tests turn this off. Release code never sets it.
void set_in_process_import_for_testing(bool in_process);
bool in_process_import_for_testing();

// Marks a capability proxy of a hosted plugin.
class HostedCapability
{
public:
    explicit HostedCapability(plugin_ipc::HostedCapabilityInfo info) : m_info(std::move(info)) {}
    virtual ~HostedCapability() = default;
    const plugin_ipc::HostedCapabilityInfo& hosted_info() const { return m_info; }

protected:
    plugin_ipc::HostedCapabilityInfo m_info;
};

// The proxy for one capability the helper reported. Script and slicing-pipeline capabilities get
// the typed proxies the dispatchers look for; other types a generic one (nothing dispatches them
// in this Edge build).
std::shared_ptr<PluginCapabilityInterface> make_hosted_capability(const plugin_ipc::HostedCapabilityInfo& info);

// Null unless `capability` is a proxy of a hosted plugin.
const HostedCapability* as_hosted(const PluginCapabilityInterface* capability);

// The hosted counterpart of plugin_loader::load: installs the declared dependencies here when
// asked (uv, wheels only, pinned and hashed; no plugin code runs), imports the plugin in the
// helper, and builds `out` from the helper's strictly validated reply. Enable flags come from the
// install-state sidecar exactly as for an in-process load. `registry_precheck` runs before the
// load is accepted. Never throws.
bool load_hosted(const PluginDescriptor&                          descriptor,
                 bool                                             skip_deps,
                 const std::vector<std::string>&                  capabilities_to_enable,
                 const std::function<std::string(const Plugin&)>& registry_precheck,
                 Plugin&                                          out,
                 std::string&                                     error);

// Builds a hosted Plugin from an already parsed capability list (load_hosted's second half;
// tests use it directly). False with an error for a duplicate capability.
bool build_hosted_plugin(const PluginDescriptor&                                descriptor,
                         const std::vector<plugin_ipc::HostedCapabilityInfo>&   capabilities,
                         const std::vector<std::string>&                        capabilities_to_enable,
                         Plugin&                                                out,
                         std::string&                                           error);

} // namespace Slic3r::plugin_import
