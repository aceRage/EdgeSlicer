#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace Slic3r {
class Model;
}

namespace Slic3r::plugin_ipc {

struct HostCall
{
    std::string    plugin_key;
    std::string    capability;
    std::string    method;
    nlohmann::json args = nlohmann::json::object();
};

struct HostResult
{
    bool           ok = false;
    nlohmann::json value;
    std::string    error;
};

// Edge-side handler for a child `host_call`. Applies Phase 5 trust and Phase 4 grants
// here; the child is not trusted to have checked them. Unknown methods are denied.
// Never returns UltraNet `{data_dir}/plugins` paths or secrets.
HostResult handle_host_call(const HostCall& call);

// Reconstruct a Model from the JSON snapshot produced by handle_host_call("host.model").
// Used by the child host.model() binding.
std::unique_ptr<Slic3r::Model> model_from_snapshot_json(const nlohmann::json& snapshot);

} // namespace Slic3r::plugin_ipc
