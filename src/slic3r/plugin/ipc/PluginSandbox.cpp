#include "PluginSandbox.hpp"
#include "PluginSandboxPriv.hpp"
#include "PluginHostProcess.hpp"
#include "PluginIpcProtocol.hpp"

#include <libslic3r/Utils.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/I18N.hpp>
#include <slic3r/GUI/MsgDialog.hpp>
#include <slic3r/GUI/format.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include "slic3r/plugin/EdgePythonInclude.hpp"
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginTrust.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>

#include <wx/app.h>
#include <wx/thread.h>

namespace Slic3r::plugin_ipc {
namespace {

std::mutex               g_prompt_mutex;
SandboxUnavailablePrompt g_prompt_for_testing;
std::atomic<bool>        g_abort_sandbox_prompt{false};
GUI::MessageDialog*      g_sandbox_dialog = nullptr;

std::string unsandboxed_bind_hash(const std::string& plugin_key)
{
    if (plugin_key.empty())
        return {};
    boost::filesystem::path root;
    PluginDescriptor        descriptor;
    if (PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor)) {
        root = descriptor.plugin_root.empty() ? boost::filesystem::path(descriptor.entry_path) :
                                                boost::filesystem::path(descriptor.plugin_root);
        boost::system::error_code ec;
        const auto                st = boost::filesystem::status(root, ec);
        if (!ec && boost::filesystem::is_regular_file(st))
            root = root.parent_path();
    } else if (!Slic3r::data_dir().empty()) {
        root = boost::filesystem::path(Slic3r::data_dir()) / "orca_plugins" / plugin_key;
    }
    if (root.empty())
        return {};
    std::string hash_error;
    return plugin_trust::sha256_plugin_tree(root, hash_error);
}

std::string strip_tree_wildcard(std::string path)
{
    if (path.size() >= 2 && path.compare(path.size() - 2, 2, "/*") == 0)
        path.resize(path.size() - 2);
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return path;
}

void add_unique_path(std::vector<SandboxPath>& paths, std::string path, bool write, bool low_label = false, bool fail_closed_low = false)
{
    path = strip_tree_wildcard(std::move(path));
    if (path.empty())
        return;
    for (SandboxPath& existing : paths) {
        if (existing.path == path) {
            existing.write           = existing.write || write;
            existing.low_label       = existing.low_label || low_label;
            existing.fail_closed_low = existing.fail_closed_low || fail_closed_low;
            return;
        }
    }
    paths.push_back(SandboxPath{std::move(path), write, low_label, fail_closed_low});
}

bool path_exists(const std::string& path)
{
    if (path.empty())
        return false;
    boost::system::error_code ec;
    return boost::filesystem::exists(path, ec) && !ec;
}

// Create an owned directory so a later ACL/Landlock grant has something to attach to.
// Returns false (and logs) when the path cannot be created; the caller then skips the grant.
bool ensure_grant_directory(const std::string& path)
{
    if (path.empty())
        return false;
    boost::system::error_code ec;
    if (boost::filesystem::is_directory(path, ec) && !ec)
        return true;
    ec.clear();
    boost::filesystem::create_directories(path, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(warning) << "Failed to create sandbox grant directory " << path << ": " << ec.message();
        return false;
    }
    return true;
}

bool env_truthy(const char* name)
{
    if (!helper_env_overrides_allowed())
        return false;
    const char* v = boost::nowide::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

} // namespace

nlohmann::json sandbox_spec_to_json(const SandboxSpec& spec)
{
    nlohmann::json extra = nlohmann::json::array();
    for (const SandboxPath& p : spec.extra_paths)
        extra.push_back({{"path", p.path}, {"write", p.write}, {"low_label", p.low_label}, {"fail_closed_low", p.fail_closed_low}});
    return {
        {"plugin_key", spec.plugin_key},       {"python_home", spec.python_home}, {"plugin_root", spec.plugin_root},
        {"write_root", spec.write_root},       {"data_dir", spec.data_dir},       {"extra_paths", std::move(extra)},
        {"allow_network", spec.allow_network}, {"allow_exec", spec.allow_exec},   {"dev_seatbelt_probe", spec.dev_seatbelt_probe},
    };
}

bool sandbox_spec_from_json(const nlohmann::json& json, SandboxSpec& spec, std::string& error)
{
    spec = {};
    if (!json.is_object()) {
        error = "sandbox spec is not an object";
        return false;
    }
    spec.plugin_key         = json.value("plugin_key", std::string());
    spec.python_home        = json.value("python_home", std::string());
    spec.plugin_root        = json.value("plugin_root", std::string());
    spec.write_root         = json.value("write_root", std::string());
    spec.data_dir           = json.value("data_dir", std::string());
    spec.allow_network      = json.value("allow_network", false);
    spec.allow_exec         = json.value("allow_exec", false);
    spec.dev_seatbelt_probe = json.value("dev_seatbelt_probe", false);
    if (json.contains("extra_paths") && json["extra_paths"].is_array()) {
        for (const auto& item : json["extra_paths"]) {
            if (!item.is_object() || !item.contains("path") || !item["path"].is_string())
                continue;
            add_unique_path(spec.extra_paths, item["path"].get<std::string>(), item.value("write", false), item.value("low_label", false),
                            item.value("fail_closed_low", false));
        }
    }
    return true;
}

nlohmann::json sandbox_report_to_json(const SandboxReport& report)
{
    return {
        {"applied", report.applied}, {"kind", report.kind}, {"detail", report.detail},     {"landlock_abi", report.landlock_abi},
        {"network", report.network}, {"exec", report.exec}, {"degraded", report.degraded},
    };
}

SandboxReport sandbox_report_from_json(const nlohmann::json& json)
{
    SandboxReport report;
    if (!json.is_object())
        return report;
    report.applied      = json.value("applied", false);
    report.kind         = json.value("kind", std::string(k_sandbox_kind_none));
    report.detail       = json.value("detail", std::string());
    report.landlock_abi = json.value("landlock_abi", 0);
    report.network      = json.value("network", false);
    report.exec         = json.value("exec", false);
    report.degraded     = json.value("degraded", false);
    if (!report.applied)
        report.kind = k_sandbox_kind_none;
    return report;
}

bool sandbox_specs_equal(const SandboxSpec& a, const SandboxSpec& b)
{
    if (a.plugin_key != b.plugin_key || a.python_home != b.python_home || a.plugin_root != b.plugin_root || a.write_root != b.write_root ||
        a.data_dir != b.data_dir || a.allow_network != b.allow_network || a.allow_exec != b.allow_exec ||
        a.dev_seatbelt_probe != b.dev_seatbelt_probe)
        return false;
    if (a.extra_paths.size() != b.extra_paths.size())
        return false;
    for (size_t i = 0; i < a.extra_paths.size(); ++i) {
        if (a.extra_paths[i].path != b.extra_paths[i].path || a.extra_paths[i].write != b.extra_paths[i].write ||
            a.extra_paths[i].low_label != b.extra_paths[i].low_label ||
            a.extra_paths[i].fail_closed_low != b.extra_paths[i].fail_closed_low)
            return false;
    }
    return true;
}

bool running_inside_flatpak()
{
    if (boost::nowide::getenv("FLATPAK_ID") != nullptr)
        return true;
    boost::system::error_code ec;
    return boost::filesystem::exists("/.flatpak-info", ec);
}

SandboxProbe probe_os_sandbox()
{
    SandboxProbe probe;
#if defined(_WIN32)
    probe.os_name = "windows";
    probe_win_sandbox(probe);
#elif defined(__APPLE__)
    probe.os_name = "macos";
    probe_mac_sandbox(probe);
#else
    probe.os_name        = "linux";
    probe.inside_flatpak = running_inside_flatpak();
    probe_linux_sandbox(probe);
#endif
    if (sandbox_env_force_unavailable()) {
        probe.available = false;
        probe.reason    = "EDGE_PLUGIN_SANDBOX_FORCE_UNAVAILABLE is set (test only)";
    }
    return probe;
}

bool os_sandbox_available() { return probe_os_sandbox().available; }

std::string expected_sandbox_kind()
{
#if defined(_WIN32)
    return k_sandbox_kind_appcontainer;
#elif defined(__APPLE__)
    return k_sandbox_kind_app_sandbox;
#else
    return k_sandbox_kind_landlock_seccomp;
#endif
}

std::string host_child_log_dir(const SandboxSpec& spec)
{
    if (!spec.write_root.empty())
        return spec.write_root;
    if (!Slic3r::data_dir().empty())
        return (boost::filesystem::path(Slic3r::data_dir()) / "plugin_host").string();
    return {};
}

std::string host_child_log_path(const SandboxSpec& spec)
{
    const std::string dir = host_child_log_dir(spec);
    if (dir.empty())
        return {};
    return (boost::filesystem::path(dir) / "edge-plugin-host.log").string();
}

SandboxSpec make_sandbox_spec(const std::string& plugin_key, const std::string& write_root)
{
    PluginDescriptor  descriptor;
    PluginPermissions permissions;
    if (!plugin_key.empty()) {
        PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor);
        PluginInstallState state;
        if (PluginManager::instance().get_install_state(plugin_key, state))
            permissions = state.permissions;
    }
    return make_sandbox_spec(descriptor, permissions, write_root);
}

SandboxSpec make_sandbox_spec(const PluginDescriptor& descriptor, const PluginPermissions& permissions, const std::string& write_root)
{
    SandboxSpec spec;
    spec.plugin_key  = descriptor.plugin_key;
    spec.python_home = PythonInterpreter::bundled_python_home();
    spec.plugin_root = descriptor.plugin_root.empty() ? descriptor.entry_path : descriptor.plugin_root;
    if (!spec.plugin_root.empty()) {
        boost::system::error_code ec;
        const auto                st = boost::filesystem::status(spec.plugin_root, ec);
        if (!ec && boost::filesystem::is_regular_file(st))
            spec.plugin_root = boost::filesystem::path(spec.plugin_root).parent_path().string();
    }
    spec.write_root         = strip_tree_wildcard(write_root);
    spec.data_dir           = Slic3r::data_dir();
    spec.allow_network      = !permissions.network_http.empty() || !permissions.network_socket.empty();
    spec.allow_exec         = !permissions.process.empty();
    spec.dev_seatbelt_probe = sandbox_env_seatbelt_probe();

    // write_root is an owned scratch folder (tests, post-process staging). Create it so
    // Windows SetNamedSecurityInfoW has a real object; Linux add_req still needs the path.
    if (!spec.write_root.empty())
        ensure_grant_directory(spec.write_root);

    // Helper crash log. AppContainer must be able to create this file during CPython init.
    const std::string log_dir = host_child_log_dir(spec);
    if (ensure_grant_directory(log_dir))
        add_unique_path(spec.extra_paths, log_dir, true, true);

    const boost::filesystem::path helper_dir = helper_path().parent_path();
    if (path_exists(helper_dir.string()))
        add_unique_path(spec.extra_paths, helper_dir.string(), false);

    // {data_dir}/python/packages/{abi}. Interpreter init creates this under the data_dir
    // that was current at initialize(); a later ScopedDataDir / first-run user tree does
    // not have it. Create it here so pip installs inside the sandbox can still write, and
    // so Windows AppContainer ACL grants do not abort the spawn on a missing path.
    const std::string packages = PythonInterpreter::shared_packages_dir();
    if (ensure_grant_directory(packages))
        add_unique_path(spec.extra_paths, packages, false);

    const std::string resources = Slic3r::resources_dir();
    if (path_exists(resources))
        add_unique_path(spec.extra_paths, resources, false);

    // Grant the trust sidecar file, not all of data_dir (presets, printer state).
    // Skip when the file is not there yet (fresh data dir); Linux already uses
    // add_req_if_exists. Creating an empty sidecar would change trust semantics.
    const boost::filesystem::path sidecar = plugin_trust::trust_sidecar_path();
    if (path_exists(sidecar.string()))
        add_unique_path(spec.extra_paths, sidecar.string(), false);

    if (!descriptor.plugin_key.empty()) {
        try {
            add_unique_path(spec.extra_paths, PluginManager::instance().get_storage_dir(descriptor.plugin_key), true, true);
        } catch (...) {}
    }

    for (const std::string& path : permissions.fs_read) {
        const std::string stripped = strip_tree_wildcard(path);
        if (path_exists(stripped))
            add_unique_path(spec.extra_paths, stripped, false);
    }
    for (const std::string& path : permissions.fs_readwrite) {
        const std::string stripped = strip_tree_wildcard(path);
        // AppContainer is Low IL. The helper cannot write up into a Medium folder, so each
        // fs_readwrite root gets package-SID modify plus a Low label when one is not already
        // present. The parent journals both and restores them on close/sweep. Missing
        // WRITE_OWNER fail-closes that user root rather than leaving it read-only. Edge
        // work dirs (plugin_host, plugin_data, write_root) warn and continue instead.
        if (ensure_grant_directory(stripped))
            add_unique_path(spec.extra_paths, stripped, true, true, true);
    }
    return spec;
}

bool apply_os_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error)
{
    report = {};
    if (sandbox_env_disable_requested()) {
        report.kind   = k_sandbox_kind_none;
        report.detail = "EDGE_PLUGIN_SANDBOX=0 (test only)";
        error         = report.detail;
        return false;
    }
    if (sandbox_env_force_unavailable()) {
        report.kind   = k_sandbox_kind_none;
        report.detail = "EDGE_PLUGIN_SANDBOX_FORCE_UNAVAILABLE (test only)";
        error         = report.detail;
        return false;
    }
#if defined(__linux__)
    return apply_linux_sandbox(spec, report, error);
#elif defined(_WIN32)
    return apply_win_child_sandbox(spec, report, error);
#elif defined(__APPLE__)
    return apply_mac_sandbox(spec, report, error);
#else
    error         = "no OS sandbox on this platform";
    report.kind   = k_sandbox_kind_none;
    report.detail = error;
    return false;
#endif
}

bool prepare_parent_sandbox(const SandboxSpec& spec, std::string& error)
{
#ifdef _WIN32
    if (in_child_process()) {
        error = "AppContainer parent sandbox is parent-only";
        return false;
    }
    if (sandbox_env_disable_requested() || sandbox_env_force_unavailable())
        return true;
    return prepare_win_appcontainer(spec, error);
#else
    (void) spec;
    (void) error;
    return true;
#endif
}

void* parent_sandbox_security_capabilities()
{
#ifdef _WIN32
    return win_security_capabilities();
#else
    return nullptr;
#endif
}

void close_parent_sandbox()
{
#ifdef _WIN32
    if (in_child_process())
        return;
    close_win_appcontainer();
#endif
}

void sweep_stale_parent_sandbox_grants()
{
#ifdef _WIN32
    if (in_child_process())
        return;
    sweep_win_acl_journal();
#endif
}

void delete_parent_sandbox_profile(const std::string& plugin_key)
{
#ifdef _WIN32
    if (in_child_process())
        return;
    delete_win_appcontainer_profile(plugin_key);
#else
    (void) plugin_key;
#endif
}

void delete_test_created_parent_sandbox_profile(const std::string& plugin_key)
{
#ifdef _WIN32
    if (in_child_process())
        return;
    delete_win_test_created_appcontainer_profile(plugin_key);
#else
    (void) plugin_key;
#endif
}

#ifdef _WIN32
std::vector<std::string> parent_sandbox_known_roots()
{
    std::vector<std::string> roots;
    auto                     add = [&](std::string path) {
        if (path.empty())
            return;
        for (const std::string& existing : roots) {
            if (existing == path)
                return;
        }
        roots.push_back(std::move(path));
    };
    add(Slic3r::data_dir());
    add(Slic3r::resources_dir());
    add(PythonInterpreter::bundled_python_home());
    add(helper_path().parent_path().string());
    return roots;
}
#endif

#ifdef _WIN32
void* parent_sandbox_pipe_security_attributes() { return win_pipe_security_attributes(); }
#endif

bool sandbox_env_disable_requested()
{
    if (!helper_env_overrides_allowed())
        return false;
    const char* v = boost::nowide::getenv(k_env_sandbox_disable);
    return v != nullptr && v[0] == '0';
}

bool sandbox_env_force_unavailable() { return env_truthy(k_env_sandbox_force_unavailable); }

bool sandbox_env_seatbelt_probe() { return env_truthy(k_env_sandbox_seatbelt_probe); }

bool sandbox_env_bwrap_requested()
{
    if (running_inside_flatpak())
        return false;
    return env_truthy(k_env_sandbox_bwrap);
}

void set_sandbox_unavailable_prompt_for_testing(SandboxUnavailablePrompt prompt)
{
    std::lock_guard<std::mutex> lock(g_prompt_mutex);
    g_prompt_for_testing = std::move(prompt);
    g_abort_sandbox_prompt.store(false, std::memory_order_release);
}

void abort_sandbox_unavailable_prompt()
{
    g_abort_sandbox_prompt.store(true, std::memory_order_release);
    auto dismiss = []() {
        std::lock_guard<std::mutex> lock(g_prompt_mutex);
        if (g_sandbox_dialog != nullptr)
            g_sandbox_dialog->EndModal(wxID_NO);
    };
    if (wxTheApp == nullptr)
        return;
    if (wxIsMainThread())
        dismiss();
    else
        GUI::wxGetApp().CallAfter(dismiss);
}

void clear_sandbox_unavailable_prompt_abort() { g_abort_sandbox_prompt.store(false, std::memory_order_release); }

bool sandbox_unavailable_prompt_aborted() { return g_abort_sandbox_prompt.load(std::memory_order_acquire); }

std::string sandbox_unavailable_message(const std::string& plugin_key, const std::string& reason)
{
    const std::string name = plugin_key.empty() ? std::string("plugin") : plugin_key;
    std::string       body = GUI::format(_u8L("The OS sandbox could not be applied for \"%1%\".\n\n"
                                                    "Without an OS sandbox the plugin runs with this account's filesystem and network "
                                                    "access. The default is not to run it.\n\n%2%"),
                                         name, reason);
    return body;
}

bool allow_unsandboxed_run(const std::string& plugin_key, const std::string& reason, bool& applied_out, std::string& error)
{
    applied_out = false;
    error.clear();

    if (sandbox_env_disable_requested()) {
        error = "OS sandbox disabled by EDGE_PLUGIN_SANDBOX=0 (test only)";
        return true;
    }

    const std::string bind_hash = unsandboxed_bind_hash(plugin_key);
    if (plugin_trust::is_unsandboxed(plugin_key, bind_hash))
        return true;

    SandboxUnavailableChoice choice = SandboxUnavailableChoice::DontRun;
    SandboxUnavailablePrompt prompt;
    {
        std::lock_guard<std::mutex> lock(g_prompt_mutex);
        prompt = g_prompt_for_testing;
    }
    if (prompt) {
        choice = prompt(plugin_key, reason);
    } else {
        const wxString body = wxString::FromUTF8(sandbox_unavailable_message(plugin_key, reason).c_str());
        auto           show = [](const wxString& message) {
            GUI::MessageDialog dialog(nullptr, message, _L("Plugin sandbox unavailable"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
            dialog.SetButtonLabel(wxID_NO, _L("Don't run"), /*set_focus=*/true);
            dialog.SetButtonLabel(wxID_YES, _L("Run anyway"));
            {
                std::lock_guard<std::mutex> lock(g_prompt_mutex);
                if (g_abort_sandbox_prompt.load(std::memory_order_acquire))
                    return SandboxUnavailableChoice::DontRun;
                g_sandbox_dialog = &dialog;
            }
            const int rc = dialog.ShowModal();
            {
                std::lock_guard<std::mutex> lock(g_prompt_mutex);
                g_sandbox_dialog = nullptr;
            }
            return rc == wxID_YES ? SandboxUnavailableChoice::RunUnsandboxed : SandboxUnavailableChoice::DontRun;
        };
        if (wxTheApp == nullptr || GUI::wxGetApp().is_closing() || g_abort_sandbox_prompt.load(std::memory_order_acquire)) {
            choice = SandboxUnavailableChoice::DontRun;
        } else if (wxIsMainThread()) {
            choice = show(body);
        } else {
            auto result = std::make_shared<std::promise<SandboxUnavailableChoice>>();
            auto future = result->get_future();
            GUI::wxGetApp().CallAfter([result, body, show]() {
                try {
                    if (wxTheApp == nullptr || GUI::wxGetApp().is_closing() || g_abort_sandbox_prompt.load(std::memory_order_acquire))
                        result->set_value(SandboxUnavailableChoice::DontRun);
                    else
                        result->set_value(show(body));
                } catch (const std::future_error&) {}
            });
            for (;;) {
                if (future.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready)
                    break;
                if (g_abort_sandbox_prompt.load(std::memory_order_acquire) || wxTheApp == nullptr || GUI::wxGetApp().is_closing()) {
                    try {
                        result->set_value(SandboxUnavailableChoice::DontRun);
                    } catch (const std::future_error&) {}
                    break;
                }
            }
            try {
                choice = future.get();
            } catch (const std::future_error&) {
                choice = SandboxUnavailableChoice::DontRun;
            }
        }
    }

    if (choice != SandboxUnavailableChoice::RunUnsandboxed) {
        error = "OS sandbox unavailable; plugin did not run";
        return false;
    }
    std::string save_error;
    if (!plugin_key.empty() && bind_hash.empty()) {
        error = "cannot record unsandboxed override without a plugin hash";
        return false;
    }
    if (!plugin_key.empty() && !plugin_trust::allow_unsandboxed(plugin_key, bind_hash, save_error)) {
        error = save_error.empty() ? std::string("failed to record unsandboxed override") : save_error;
        return false;
    }
    return true;
}

} // namespace Slic3r::plugin_ipc
