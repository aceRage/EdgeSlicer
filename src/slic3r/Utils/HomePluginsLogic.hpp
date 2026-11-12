#ifndef slic3r_HomePluginsLogic_hpp_
#define slic3r_HomePluginsLogic_hpp_

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Display-only rules for Home > Plugins (resources/web/home, hosted by GUI/HomePlugins.cpp).
// Kept free of wx and of PluginManager so tests/slic3rutils/home_plugins_tests.cpp can run in
// every build, including EDGE_PYTHON_PLUGINS=OFF:
//   * a card carries name / version / author / types / status, never a package path,
//   * the page may only name plugin_keys the host itself listed,
//   * trust (Phase 5) is a display string the host fills: "Signed by …" / "Approved locally" /
//     "Untrusted". Catalog (Phase 11) stays false here.
namespace Slic3r {
namespace HomeTab {

struct PluginCapabilityView
{
    std::string name;
    std::string type; // "script", "slicing-pipeline", ...
    bool        enabled { false };
};

struct PluginView
{
    std::string                        plugin_key;
    std::string                        name;
    std::string                        version;
    std::string                        author;
    std::string                        description;
    std::vector<std::string>           types;
    std::vector<PluginCapabilityView>  capabilities;
    std::string                        source { "local" }; // "local" or, later, "catalog"
    bool                               loaded { false };
    bool                               enabled { false };
    bool                               loading { false };
    std::string                        error;
    bool                               update_available { false };
    // Phase 5: "Signed by …" / "Approved locally" / "Untrusted". Empty only when the host
    // has not evaluated the package yet (the page still has a reserved key).
    std::string                        trust;
    // Phase 6/8 label: early slicing-pipeline steps stay in-process.
    bool                               in_process_pipeline { false };
    // Phase 3: Home > Plugins Run. Empty, "running", "success", or "failure".
    std::string                        run_state;
    std::string                        run_message;
};

// One card for the page. Display fields only: never entry_path, plugin_root, or path.
nlohmann::json plugin_card(const PluginView& view);

// Drop keys the page must never see, even if a caller stuffed them onto a card object.
nlohmann::json sanitize_plugin_card(nlohmann::json card);

// Host → page envelope: {type:"plugins", items:[...], loading, error, catalog:false}.
nlohmann::json plugins_message(const std::vector<PluginView>& items, bool loading, const std::string& error);

// True when `key` is exactly one of `listed`. Empty keys and keys with a path separator are
// refused even if they appear in `listed`: the page names opaque plugin_keys, not files.
bool is_listed_plugin_key(const std::string& key, const std::vector<std::string>& listed);

// True when `(name, type)` is a capability the host last listed. Empty names are refused.
bool is_listed_capability(const std::string& name, const std::string& type,
                          const std::vector<PluginCapabilityView>& listed);

// True when `card` still carries a filesystem path key (a defect if a host message has it).
bool plugin_card_leaks_path(const nlohmann::json& card);

// Keep a Python traceback but replace absolute File "..." paths with the basename so
// run_message never ships a host filesystem path to the page.
std::string sanitize_run_message(std::string text);

} // namespace HomeTab
} // namespace Slic3r

#endif
