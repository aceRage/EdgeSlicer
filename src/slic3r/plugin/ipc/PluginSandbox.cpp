#include "PluginSandbox.hpp"
#include "PluginSandboxPriv.hpp"
#include "PluginHostProcess.hpp"

#include <libslic3r/Utils.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/I18N.hpp>
#include <slic3r/GUI/MsgDialog.hpp>
#include <slic3r/GUI/format.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginTrust.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/cstdlib.hpp>

#include <algorithm>
#include <future>
#include <mutex>

#include <wx/app.h>
#include <wx/thread.h>

namespace Slic3r::plugin_ipc {
namespace {

std::mutex               g_prompt_mutex;
SandboxUnavailablePrompt g_prompt_for_testing;

std::string strip_tree_wildcard(std::string path)
{
    if (path.size() >= 2 && path.compare(path.size() - 2, 2, "/*") == 0)
        path.resize(path.size() - 2);
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return path;
}

void add_unique_path(std::vector<SandboxPath>& paths, std::string path, bool write)
{
    path = strip_tree_wildcard(std::move(path));
    if (path.empty())
        return;
    for (SandboxPath& existing : paths) {
        if (existing.path == path) {
            existing.write = existing.write || write;
            return;
        }
    }
    paths.push_back(SandboxPath{std::move(path), write});
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
        extra.push_back({{"path", p.path}, {"write", p.write}});
    return {
        {"plugin_key", spec.plugin_key}, {"python_home", spec.python_home}, {"plugin_root", spec.plugin_root},
        {"write_root", spec.write_root}, {"extra_paths", std::move(extra)}, {"allow_network", spec.allow_network},
        {"allow_exec", spec.allow_exec},
    };
}

bool sandbox_spec_from_json(const nlohmann::json& json, SandboxSpec& spec, std::string& error)
{
    spec = {};
    if (!json.is_object()) {
        error = "sandbox spec is not an object";
        return false;
    }
    spec.plugin_key    = json.value("plugin_key", std::string());
    spec.python_home   = json.value("python_home", std::string());
    spec.plugin_root   = json.value("plugin_root", std::string());
    spec.write_root    = json.value("write_root", std::string());
    spec.allow_network = json.value("allow_network", false);
    spec.allow_exec    = json.value("allow_exec", false);
    if (json.contains("extra_paths") && json["extra_paths"].is_array()) {
        for (const auto& item : json["extra_paths"]) {
            if (!item.is_object() || !item.contains("path") || !item["path"].is_string())
                continue;
            add_unique_path(spec.extra_paths, item["path"].get<std::string>(), item.value("write", false));
        }
    }
    return true;
}

nlohmann::json sandbox_report_to_json(const SandboxReport& report)
{
    return {
        {"applied", report.applied},           {"kind", report.kind},       {"detail", report.detail},
        {"landlock_abi", report.landlock_abi}, {"network", report.network}, {"exec", report.exec},
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
    if (!report.applied)
        report.kind = k_sandbox_kind_none;
    return report;
}

bool sandbox_specs_equal(const SandboxSpec& a, const SandboxSpec& b)
{
    if (a.plugin_key != b.plugin_key || a.python_home != b.python_home || a.plugin_root != b.plugin_root || a.write_root != b.write_root ||
        a.allow_network != b.allow_network || a.allow_exec != b.allow_exec)
        return false;
    if (a.extra_paths.size() != b.extra_paths.size())
        return false;
    for (size_t i = 0; i < a.extra_paths.size(); ++i) {
        if (a.extra_paths[i].path != b.extra_paths[i].path || a.extra_paths[i].write != b.extra_paths[i].write)
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
    spec.write_root    = strip_tree_wildcard(write_root);
    spec.allow_network = !permissions.network_http.empty() || !permissions.network_socket.empty();
    spec.allow_exec    = !permissions.process.empty();

    const boost::filesystem::path helper_dir = helper_path().parent_path();
    if (!helper_dir.empty())
        add_unique_path(spec.extra_paths, helper_dir.string(), false);

    const std::string packages = PythonInterpreter::shared_packages_dir();
    if (!packages.empty())
        add_unique_path(spec.extra_paths, packages, false);

    const std::string resources = Slic3r::resources_dir();
    if (!resources.empty())
        add_unique_path(spec.extra_paths, resources, false);

    if (!descriptor.plugin_key.empty()) {
        try {
            add_unique_path(spec.extra_paths, PluginManager::instance().get_storage_dir(descriptor.plugin_key), true);
        } catch (...) {}
    }

    for (const std::string& path : permissions.fs_read)
        add_unique_path(spec.extra_paths, path, false);
    for (const std::string& path : permissions.fs_readwrite)
        add_unique_path(spec.extra_paths, path, true);
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
    close_win_appcontainer();
#endif
}

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
}

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

    if (plugin_trust::is_unsandboxed(plugin_key))
        return true;

    SandboxUnavailableChoice choice = SandboxUnavailableChoice::DontRun;
    {
        std::lock_guard<std::mutex> lock(g_prompt_mutex);
        if (g_prompt_for_testing) {
            choice = g_prompt_for_testing(plugin_key, reason);
        } else {
            const wxString body = wxString::FromUTF8(sandbox_unavailable_message(plugin_key, reason).c_str());
            auto           show = [](const wxString& message) {
                GUI::MessageDialog dialog(nullptr, message, _L("Plugin sandbox unavailable"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
                dialog.SetButtonLabel(wxID_NO, _L("Don't run"), /*set_focus=*/true);
                dialog.SetButtonLabel(wxID_YES, _L("Run anyway"));
                return dialog.ShowModal() == wxID_YES ? SandboxUnavailableChoice::RunUnsandboxed : SandboxUnavailableChoice::DontRun;
            };
            if (wxTheApp == nullptr || GUI::wxGetApp().is_closing()) {
                choice = SandboxUnavailableChoice::DontRun;
            } else if (wxIsMainThread()) {
                choice = show(body);
            } else {
                auto result = std::make_shared<std::promise<SandboxUnavailableChoice>>();
                auto future = result->get_future();
                GUI::wxGetApp().CallAfter([result, body, show]() {
                    if (wxTheApp == nullptr || GUI::wxGetApp().is_closing())
                        result->set_value(SandboxUnavailableChoice::DontRun);
                    else
                        result->set_value(show(body));
                });
                choice = future.get();
            }
        }
    }

    if (choice != SandboxUnavailableChoice::RunUnsandboxed) {
        error = "OS sandbox unavailable; plugin did not run";
        return false;
    }
    std::string save_error;
    if (!plugin_key.empty() && !plugin_trust::allow_unsandboxed(plugin_key, save_error)) {
        error = save_error.empty() ? std::string("failed to record unsandboxed override") : save_error;
        return false;
    }
    return true;
}

} // namespace Slic3r::plugin_ipc
