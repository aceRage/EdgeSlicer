#include "PluginCatalogPolicy.hpp"

namespace Slic3r {
namespace PluginSpike {

CatalogHost default_catalog_host() { return CatalogHost::GitHubPagesAndReleases; }

CatalogPublisherPolicy catalog_publisher_policy() { return CatalogPublisherPolicy::EdgeOnly; }

bool community_publisher_keys_allowed() { return false; }

bool catalog_base_url_is_replaceable() { return true; }

} // namespace PluginSpike
} // namespace Slic3r
