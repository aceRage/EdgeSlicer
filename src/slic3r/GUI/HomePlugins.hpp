#ifndef slic3r_GUI_HomePlugins_hpp_
#define slic3r_GUI_HomePlugins_hpp_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class wxWindow;

namespace Slic3r {
namespace GUI {

// The Home tab's Plugins section. The page (resources/web/home) only draws; this owns every
// package path, every install, and every enable/disable, and talks to Phase 1 PluginManager.
// The page gets display fields and opaque plugin_keys; it never sees entry_path or plugin_root.
// Compiled only when EDGE_PYTHON_PLUGINS is ON.
class HomePlugins
{
public:
    using SendFn = std::function<void(const nlohmann::json&)>;
    HomePlugins(wxWindow* parent, SendFn send);
    ~HomePlugins();

    // True when `msg` was a Plugins command (handled or refused).
    bool handle(const nlohmann::json& msg);
    void send_state();

private:
    bool listed(const std::string& key) const;
    void notice(const std::string& text, bool error = false);
    void install();
    void toggle(const std::string& key, bool enable);
    void capability_toggle(const std::string& key, const std::string& name, const std::string& type, bool enable);
    void reload(const std::string& key, bool clear);
    void remove(const std::string& key);
    void reveal(const std::string& key);
    void stub(const std::string& text);

    wxWindow*                 m_parent;
    SendFn                    m_send;
    std::vector<std::string>  m_keys;
    std::shared_ptr<bool>     m_alive { std::make_shared<bool>(true) };
};

} // namespace GUI
} // namespace Slic3r

#endif
