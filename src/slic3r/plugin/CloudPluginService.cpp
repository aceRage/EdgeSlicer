#include "CloudPluginService.hpp"

#include "PluginDescriptor.hpp"

namespace Slic3r {

void CloudPluginService::set_cloud_agent(std::shared_ptr<OrcaCloudServiceAgent> agent)
{
    m_orca_agent = std::move(agent);
}

std::shared_ptr<OrcaCloudServiceAgent> CloudPluginService::get_cloud_agent() const
{
    return m_orca_agent;
}

std::string CloudPluginService::logged_in_user_id() const
{
    // Phase 1: cloud networking is not wired; storage stays on the local plugin_key path.
    return {};
}

bool CloudPluginService::can_fetch_cloud_plugins() const
{
    // Phase 1: Orca Cloud is not wired. Local packages only.
    return false;
}

bool CloudPluginService::fetch_manifests_into_descriptors(std::vector<PluginDescriptor>& /*descriptors*/,
                                                          std::vector<std::string>& /*not_found*/,
                                                          std::vector<std::string>& /*unauthorized*/) const
{
    return false;
}

bool CloudPluginService::request_cloud_subscribe(const std::string& /*plugin_uuid*/, std::string& error) const
{
    error = "Cloud plugins are not available in this build.";
    return false;
}

bool CloudPluginService::request_cloud_unsubscribe(const PluginDescriptor& /*plugin*/, std::string& error) const
{
    error = "Cloud plugins are not available in this build.";
    return false;
}

bool CloudPluginService::download_cloud_plugin(PluginDescriptor& /*entry*/,
                                               const std::string& /*requested_version*/,
                                               CloudPluginDownload& /*download*/,
                                               std::string& error) const
{
    error = "Cloud plugins are not available in this build.";
    return false;
}

bool CloudPluginService::fetch_plugin_changelog(const PluginDescriptor& /*descriptor*/,
                                                std::vector<PluginChangelog>& /*changelog*/,
                                                std::string& error) const
{
    error = "Cloud plugins are not available in this build.";
    return false;
}

bool CloudPluginService::fetch_plugin_changelog(const std::vector<PluginDescriptor>& /*descriptors*/,
                                                std::unordered_map<std::string, std::vector<PluginChangelog>>& /*changelog*/,
                                                std::string& error) const
{
    error = "Cloud plugins are not available in this build.";
    return false;
}

} // namespace Slic3r
