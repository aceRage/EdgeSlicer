#include <catch2/catch.hpp>

#include <string>

#include "PluginCatalogPolicy.hpp"
#include "PluginGrantPolicy.hpp"
#include "PluginHostSpawnModel.hpp"
#include "PluginPathPolicy.hpp"
#include "PluginSandboxPolicy.hpp"
#include "PluginSandboxProbe.hpp"

using namespace Slic3r::PluginSpike;

TEST_CASE("Python packages never share UltraNet's plugins folder", "[PluginSpike][paths]")
{
    const std::string data = "/home/user/.config/EdgeSlicer";
    CHECK(python_plugin_root(data) == "/home/user/.config/EdgeSlicer/orca_plugins");
    CHECK(network_plugin_root(data) == "/home/user/.config/EdgeSlicer/plugins");
    CHECK(python_plugin_root(data) != network_plugin_root(data));

    CHECK(python_write_would_collide_with_ultranet(data, "/home/user/.config/EdgeSlicer/plugins"));
    CHECK(python_write_would_collide_with_ultranet(data, "/home/user/.config/EdgeSlicer/plugins/bambu_networking.so"));
    CHECK_FALSE(python_write_would_collide_with_ultranet(data, "/home/user/.config/EdgeSlicer/orca_plugins/demo.py"));
    CHECK_FALSE(python_write_would_collide_with_ultranet(data, python_plugin_trust_path(data)));
}

TEST_CASE("Windows-style data_dir still splits orca_plugins from plugins", "[PluginSpike][paths]")
{
    const std::string data = "C:\\Users\\x\\AppData\\Roaming\\EdgeSlicer";
    CHECK(is_under_network_plugin_dir(data, "C:/Users/x/AppData/Roaming/EdgeSlicer/plugins/FlashNetwork.dll"));
    CHECK_FALSE(is_under_network_plugin_dir(data, "C:/Users/x/AppData/Roaming/EdgeSlicer/orca_plugins/demo.whl"));
}

TEST_CASE("Full access does not grant printer_send", "[PluginSpike][grants]")
{
    CHECK_FALSE(full_access_implies_printer_send());
    CHECK_FALSE(default_printer_send_granted());
    CHECK(plugins_may_request_full_access());
    CHECK(full_access_confirmation_defaults_to_no());
    CHECK(printer_send_path() == PrinterSendPath::ExistingEdgeConnectionsOnly);
    CHECK(std::string(k_printer_send_grant) == "printer_send");
}

TEST_CASE("Sandbox-unavailable defaults to Don't run", "[PluginSpike][sandbox]")
{
    CHECK(default_sandbox_unavailable_action() == SandboxUnavailableAction::DontRun);
    CHECK(sandbox_placement(PluginWorkload::Script) == SandboxPlacement::OutOfProcessSandboxed);
    CHECK(sandbox_placement(PluginWorkload::GCodePostProcess) == SandboxPlacement::OutOfProcessSandboxed);
    CHECK(sandbox_placement(PluginWorkload::EarlySlicingPipeline) == SandboxPlacement::InProcessLabeled);
    CHECK(workload_must_prompt_when_sandbox_unavailable(PluginWorkload::Script));
    CHECK_FALSE(early_pipeline_requires_sandbox_unavailable_prompt());
    CHECK_FALSE(workload_must_prompt_when_sandbox_unavailable(PluginWorkload::EarlySlicingPipeline));
}

TEST_CASE("edge-plugin-host spawn plan reuses Edge process helpers, not wxExecute", "[PluginSpike][spawn]")
{
    const PluginHostSpawnPlan plan = spawn_plan_for_current_os();
    CHECK(std::string(plan.helper_name) == "edge-plugin-host");
    CHECK_FALSE(plan.use_wx_execute);
    CHECK(plan.kill_on_parent_exit);
    CHECK_FALSE(plan.allow_breakaway);
    CHECK(plan.max_active_processes == 1);
    CHECK(plan.ipc == PluginHostIpc::LocalSocketOrPipe);
    CHECK_FALSE(spawn_plan_source_citation(plan).empty());
}

TEST_CASE("Catalog is GitHub-first and Edge-only", "[PluginSpike][catalog]")
{
    CHECK(default_catalog_host() == CatalogHost::GitHubPagesAndReleases);
    CHECK(catalog_publisher_policy() == CatalogPublisherPolicy::EdgeOnly);
    CHECK_FALSE(community_publisher_keys_allowed());
    CHECK(catalog_base_url_is_replaceable());
}

TEST_CASE("Sandbox probe fail-closed flag is set", "[PluginSpike][probe]")
{
    const PluginSandboxProbe probe = probe_sandbox_apis();
    CHECK(probe.fail_closed_when_unavailable);
    CHECK_FALSE(probe.os_name.empty());
#if defined(__linux__)
    CHECK(probe.os_name == "linux");
    CHECK(probe.landlock_headers);
    // Kernel 5.13+ reports a positive ABI; this Cloud Agent host returned ABI 6.
    CHECK(probe.landlock_runtime);
    CHECK(probe.landlock_abi >= 1);
#endif
}
