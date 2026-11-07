#include "HomePluginsLogic.hpp"

#include "HomeTabLogic.hpp"

#include <algorithm>

namespace Slic3r {
namespace HomeTab {

using json = nlohmann::json;

static const char* const k_path_keys[] = { "entry_path", "plugin_root", "path", "thumbnail_url", "sharing_token" };

static bool has_path_separator(const std::string& s)
{
    return s.find('/') != std::string::npos || s.find('\\') != std::string::npos;
}

json sanitize_plugin_card(json card)
{
    if (!card.is_object())
        return json::object();
    for (const char* key : k_path_keys)
        card.erase(key);
    return card;
}

json plugin_card(const PluginView& view)
{
    json card;
    card["plugin_key"]          = view.plugin_key;
    card["name"]                = view.name.empty() ? view.plugin_key : view.name;
    card["version"]             = view.version;
    card["author"]              = view.author;
    card["description"]         = view.description.size() > 600 ? view.description.substr(0, 600) : view.description;
    card["source"]              = view.source == "catalog" ? "catalog" : "local";
    card["loaded"]              = view.loaded;
    card["enabled"]             = view.enabled;
    card["loading"]             = view.loading;
    card["error"]               = view.error;
    card["update_available"]    = view.update_available;
    card["trust"]               = view.trust; // Phase 5 placeholder
    card["in_process_pipeline"] = view.in_process_pipeline;
    card["can_run"]             = false;

    std::vector<std::string> types = view.types;
    types.erase(std::remove(types.begin(), types.end(), std::string()), types.end());
    json caps = json::array();
    for (const PluginCapabilityView& c : view.capabilities) {
        if (c.name.empty() && c.type.empty())
            continue;
        caps.push_back({ { "name", c.name }, { "type", c.type }, { "enabled", c.enabled } });
        if (c.type == "script")
            card["can_run"] = true;
        if (!c.type.empty() && std::find(types.begin(), types.end(), c.type) == types.end())
            types.push_back(c.type);
    }
    if (!card["can_run"].get<bool>() && std::find(types.begin(), types.end(), std::string("script")) != types.end())
        card["can_run"] = true;
    card["types"]        = types;
    card["capabilities"] = std::move(caps);
    return sanitize_plugin_card(std::move(card));
}

json plugins_message(const std::vector<PluginView>& items, bool loading, const std::string& error)
{
    json list = json::array();
    for (const PluginView& item : items)
        if (!item.plugin_key.empty())
            list.push_back(plugin_card(item));
    return { { "type", "plugins" },
             { "items", std::move(list) },
             { "loading", loading },
             { "error", error },
             { "catalog", false } }; // Phase 11 placeholder
}

bool is_listed_plugin_key(const std::string& key, const std::vector<std::string>& listed)
{
    if (key.empty() || has_path_separator(key))
        return false;
    return is_listed(key, listed);
}

bool plugin_card_leaks_path(const json& card)
{
    if (!card.is_object())
        return false;
    for (const char* key : k_path_keys)
        if (card.contains(key))
            return true;
    return false;
}

} // namespace HomeTab
} // namespace Slic3r
