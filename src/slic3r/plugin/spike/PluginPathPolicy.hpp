#ifndef slic3r_plugin_spike_PluginPathPolicy_hpp_
#define slic3r_plugin_spike_PluginPathPolicy_hpp_

#include <string>

namespace Slic3r {
namespace PluginSpike {

// Native network libraries (UltraNet, FlashForge FlashNetwork). Python packages must never
// be written here. Matches {data_dir}/plugins used by PluginGuard / NetworkAgent.
inline constexpr const char *k_network_plugin_dir = "plugins";

// Orca-compatible Python plugin packages. Matches {data_dir}/orca_plugins on OrcaSlicer.
inline constexpr const char *k_python_plugin_dir = "orca_plugins";

// Sidecar that records publisher pins and local hash approvals (phase 5). Lives under orca_plugins,
// never under plugins/.
inline constexpr const char *k_trust_sidecar_name = "trust.json";

std::string join_data_leaf(const std::string &data_dir, const char *leaf);
std::string python_plugin_root(const std::string &data_dir);
std::string network_plugin_root(const std::string &data_dir);
std::string python_plugin_data_root(const std::string &data_dir);
std::string python_plugin_trust_path(const std::string &data_dir);

// True when `path` is the network plug-in folder or a file inside it.
bool is_under_network_plugin_dir(const std::string &data_dir, const std::string &path);

// True when a Python-plugin writer would touch UltraNet's directory. Phase 1+ must refuse this.
bool python_write_would_collide_with_ultranet(const std::string &data_dir, const std::string &path);

} // namespace PluginSpike
} // namespace Slic3r

#endif
