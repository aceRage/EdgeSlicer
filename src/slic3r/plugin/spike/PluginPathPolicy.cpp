#include "PluginPathPolicy.hpp"

namespace Slic3r {
namespace PluginSpike {

namespace {

std::string normalize_path(std::string path)
{
    for (char &c : path) {
        if (c == '\\')
            c = '/';
    }
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

bool is_under(const std::string &root, const std::string &path)
{
    const std::string r = normalize_path(root);
    const std::string p = normalize_path(path);
    if (p == r)
        return true;
    return p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/';
}

} // namespace

std::string join_data_leaf(const std::string &data_dir, const char *leaf)
{
    std::string root = normalize_path(data_dir);
    if (root.empty())
        return std::string(leaf);
    return root + '/' + leaf;
}

std::string python_plugin_root(const std::string &data_dir) { return join_data_leaf(data_dir, k_python_plugin_dir); }

std::string network_plugin_root(const std::string &data_dir) { return join_data_leaf(data_dir, k_network_plugin_dir); }

std::string python_plugin_data_root(const std::string &data_dir)
{
    return python_plugin_root(data_dir) + "/plugin_data";
}

std::string python_plugin_trust_path(const std::string &data_dir)
{
    return python_plugin_root(data_dir) + '/' + k_trust_sidecar_name;
}

bool is_under_network_plugin_dir(const std::string &data_dir, const std::string &path)
{
    return is_under(network_plugin_root(data_dir), path);
}

bool python_write_would_collide_with_ultranet(const std::string &data_dir, const std::string &path)
{
    return is_under_network_plugin_dir(data_dir, path);
}

} // namespace PluginSpike
} // namespace Slic3r
