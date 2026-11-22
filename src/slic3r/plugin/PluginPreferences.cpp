#include "PluginPreferences.hpp"

#include <libslic3r/AppConfig.hpp>
#include <slic3r/GUI/GUI_App.hpp>

#include <map>
#include <mutex>

#include <wx/app.h>

namespace Slic3r::plugin_prefs {
namespace {
std::mutex&                 overrides_mutex()
{
    static std::mutex m;
    return m;
}
std::map<std::string, int>& overrides()
{
    static std::map<std::string, int> m;
    return m;
}
} // namespace

bool enabled(const char* key)
{
    {
        std::lock_guard<std::mutex> lock(overrides_mutex());
        const auto                  it = overrides().find(key);
        if (it != overrides().end())
            return it->second != 0;
    }
    if (wxTheApp == nullptr)
        return false;
    const AppConfig* config = GUI::wxGetApp().app_config;
    return config != nullptr && config->get_bool(key);
}

void set_override_for_testing(const char* key, int value)
{
    std::lock_guard<std::mutex> lock(overrides_mutex());
    if (value < 0)
        overrides().erase(key);
    else
        overrides()[key] = value;
}

} // namespace Slic3r::plugin_prefs
