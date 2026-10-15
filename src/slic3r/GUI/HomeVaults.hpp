#ifndef slic3r_GUI_HomeVaults_hpp_
#define slic3r_GUI_HomeVaults_hpp_

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/VaultProvider.hpp"

class wxWindow;

namespace Slic3r {
namespace GUI {

// The Home tab's Vaults section, on the slicer's side (the page is resources/web/home; the engine is
// Utils/VaultProvider; design: tests/design_vaults_home_tab.md). This owns everything the page must
// not touch:
//   * the vaults (app_config "home_vaults", specs without secrets),
//   * their secrets, in the OS credential store (wxSecretStore: Credential Manager, Keychain,
//     libsecret) or, where there is none, in memory for this session only; always asked for in native
//     dialogs, never typed into the page,
//   * the http warning for a vault on the local network (dismissed for good per vault in app_config
//     "home_vault_http_ok"),
//   * the HTTP calls (listings, downloads), always off the GUI thread,
//   * the caches in <data_dir>/vaults/<vault id>/, and the downloads folder (app_config
//     "vault_downloads_dir", or per vault), which is added to the Library once.
// The page gets display fields and opaque keys; it never sees a secret, a token or a request URL.
class HomeVaults
{
public:
    using SendFn = std::function<void(const nlohmann::json&)>;
    // `library_changed`: a downloads folder was added to the Library. `open_file`: Download & Open.
    HomeVaults(wxWindow* parent, SendFn send, std::function<void()> library_changed, std::function<void(const std::string&)> open_file);
    ~HomeVaults();

    // True when `msg` was a Vaults command (handled or refused).
    bool handle(const nlohmann::json& msg);
    void send_state();

    // "<Downloads>/EdgeSlicer Vaults": where vault downloads go unless Preferences or the vault says.
    static std::string default_downloads_dir();
    // Preferences' folder, else the default.
    static std::string downloads_dir();

private:
    struct Vault
    {
        Vaults::Spec              spec;
        std::vector<Vaults::File> files;
        int64_t                   synced_at { 0 };
        bool                      partial { false };
        std::string               last_error;
        bool                      cache_loaded { false };
        bool                      syncing { false };
    };

    void        load();
    void        save_specs();
    Vault*      find(const std::string& id);
    void        ensure_cache(Vault& v);
    void        write_cache(const Vault& v) const;
    std::string cache_dir(const std::string& id) const;
    std::string downloads_dir_of(const Vaults::Spec& spec) const;

    // Secrets.
    bool               secure_store() const;
    const std::string* secret(const std::string& id, const std::string& key) const;
    Vaults::Secrets    secrets_of(const Vaults::Spec& spec) const;
    void               set_secret(const std::string& id, const std::string& key, const std::string& value);
    void               forget_secrets(const Vaults::Spec& spec);
    void               ask_secret(const std::string& id, const std::string& key);
    void               ask_sas(Vault& v);

    // The http warning: true when the vault may be used now.
    bool confirm_transport(const Vault& v);
    bool http_dismissed(const std::string& id) const;
    void set_http_dismissed(const std::string& id, bool dismissed);

    void save_vault(const nlohmann::json& spec_json);
    void delete_vault(const std::string& id);
    void sync(const std::string& id);
    void test(const std::string& id);
    void download(const std::string& key, bool open_after);
    void note_download_folder(const std::string& folder);
    void notice(const std::string& text, bool error = false);

    std::map<std::string, std::pair<std::string, std::string>> m_keys; // page key -> (vault id, file id)

    wxWindow*                                m_parent;
    SendFn                                   m_send;
    std::function<void()>                    m_library_changed;
    std::function<void(const std::string&)>  m_open_file;
    std::vector<Vault>                       m_vaults;
    std::map<std::string, std::string>       m_session_secrets;
    mutable std::map<std::string, std::string> m_secret_cache;
    mutable int                              m_secure_store { -1 };
    std::set<std::string>                    m_http_warned; // vault ids warned about this session
    std::shared_ptr<bool>                    m_alive { std::make_shared<bool>(true) };
    std::shared_ptr<std::atomic<bool>>       m_cancel { std::make_shared<std::atomic<bool>>(false) };
};

} // namespace GUI
} // namespace Slic3r

#endif
