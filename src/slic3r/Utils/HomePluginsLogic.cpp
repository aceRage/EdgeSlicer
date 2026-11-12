#include "HomePluginsLogic.hpp"

#include "HomeTabLogic.hpp"

#include <algorithm>
#include <cctype>

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
    card["trust"]               = view.trust; // Phase 5: host fills Signed / Approved locally / Untrusted
    card["in_process_pipeline"] = view.in_process_pipeline;
    card["can_run"]             = false;
    card["run_state"]           = (view.run_state == "running" || view.run_state == "success" ||
                         view.run_state == "failure") ?
                             view.run_state :
                             "";
    card["run_message"]         = sanitize_run_message(view.run_message);

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

bool is_listed_capability(const std::string& name, const std::string& type,
                          const std::vector<PluginCapabilityView>& listed)
{
    if (name.empty())
        return false;
    for (const PluginCapabilityView& cap : listed) {
        if (cap.name == name && cap.type == type)
            return true;
    }
    return false;
}

std::string sanitize_run_message(std::string text)
{
    auto basename_of = [](const std::string& path) {
        const auto slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    };
    auto is_absolute = [](const std::string& path) {
        if (path.empty())
            return false;
        if (path.front() == '/' || path.front() == '\\')
            return true;
        return path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path.front())) != 0 &&
               path[1] == ':' && (path[2] == '\\' || path[2] == '/');
    };

    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const auto pos = text.find("File \"", i);
        if (pos == std::string::npos) {
            out.append(text, i, std::string::npos);
            break;
        }
        out.append(text, i, pos - i);
        out += "File \"";
        const auto start = pos + 6;
        const auto end   = text.find('"', start);
        if (end == std::string::npos) {
            out.append(text, start, std::string::npos);
            break;
        }
        const std::string path = text.substr(start, end - start);
        out += is_absolute(path) ? basename_of(path) : path;
        out += '"';
        i = end + 1;
    }
    if (out.size() > 400)
        out.resize(400);
    return out;
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
