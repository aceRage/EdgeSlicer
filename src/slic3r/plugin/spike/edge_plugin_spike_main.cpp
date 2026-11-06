#include "PluginCatalogPolicy.hpp"
#include "PluginGrantPolicy.hpp"
#include "PluginHostSpawnModel.hpp"
#include "PluginPathPolicy.hpp"
#include "PluginSandboxPolicy.hpp"
#include "PluginSandboxProbe.hpp"

#include <iostream>

int main()
{
    using namespace Slic3r::PluginSpike;

    const PluginHostSpawnPlan plan = spawn_plan_for_current_os();
    const PluginSandboxProbe  probe = probe_sandbox_apis();

    std::cout << "edge-plugin-spike (Phase 0; not a shipping host)\n";
    std::cout << "helper=" << plan.helper_name << " wxExecute=" << (plan.use_wx_execute ? "yes" : "no")
              << " kill_on_parent_exit=" << (plan.kill_on_parent_exit ? "yes" : "no")
              << " breakaway=" << (plan.allow_breakaway ? "yes" : "no") << '\n';
    std::cout << "spawn_source=" << spawn_plan_source_citation(plan) << '\n';
    std::cout << "python_dir=" << k_python_plugin_dir << " network_dir=" << k_network_plugin_dir << '\n';
    std::cout << "full_access_grants_printer_send=" << (full_access_implies_printer_send() ? "yes" : "no")
              << " default_printer_send=" << (default_printer_send_granted() ? "yes" : "no") << '\n';
    std::cout << "sandbox_unavailable_default="
              << (default_sandbox_unavailable_action() == SandboxUnavailableAction::DontRun ? "dont_run" : "run_in_process")
              << '\n';
    std::cout << "catalog_publishers="
              << (community_publisher_keys_allowed() ? "community" : "edge_only") << '\n';
    std::cout << format_sandbox_probe(probe);
    return 0;
}
