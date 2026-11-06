#ifndef slic3r_plugin_spike_PluginCatalogPolicy_hpp_
#define slic3r_plugin_spike_PluginCatalogPolicy_hpp_

namespace Slic3r {
namespace PluginSpike {

// Settled r4: GitHub first (Pages for the signed index, Releases for artifacts). Only Edge
// publishes; there are no community publisher keys yet. The client must not hard-code a host.
enum class CatalogHost { GitHubPagesAndReleases, CloudflareR2Later };

enum class CatalogPublisherPolicy { EdgeOnly };

CatalogHost             default_catalog_host();
CatalogPublisherPolicy  catalog_publisher_policy();
bool                    community_publisher_keys_allowed();
bool                    catalog_base_url_is_replaceable();

} // namespace PluginSpike
} // namespace Slic3r

#endif
