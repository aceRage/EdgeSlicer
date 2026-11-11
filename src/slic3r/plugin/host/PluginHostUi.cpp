#include "PluginHostUi.hpp"
#include "PluginHostUiMarshal.hpp"

#include "slic3r/plugin/OrcaApiVersion.hpp"
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginJsonPy.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"

#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MainFrame.hpp>
#include <slic3r/GUI/MsgDialog.hpp>
#include <slic3r/GUI/NotificationManager.hpp>
#include <slic3r/GUI/Plater.hpp>
#include <slic3r/GUI/PluginProgressDialog.hpp>
#include <slic3r/GUI/Widgets/WebView.hpp>

#include <nlohmann/json.hpp>
#include <pybind11/cast.h>
#include <pybind11/gil.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/thread.h>
#include <wx/webview.h>
#include <wx/window.h>

namespace py = pybind11;
using json   = nlohmann::json;

namespace Slic3r {
namespace {

struct GilSafeCallable
{
    py::object       fn;
    std::atomic_bool active{true};
    explicit GilSafeCallable(py::object f) : fn(std::move(f)) {}
    void disable()
    {
        active.store(false, std::memory_order_release);
        PythonGILState gil;
        if (gil)
            fn = py::object();
        else
            (void) fn.release();
    }
    ~GilSafeCallable() { disable(); }
};
using CallablePtr = std::shared_ptr<GilSafeCallable>;

CallablePtr make_holder(py::object obj)
{
    if (!obj || obj.is_none())
        return nullptr;
    return std::make_shared<GilSafeCallable>(std::move(obj));
}

using MessageHandler = std::function<void(const json&)>;

MessageHandler make_message_adapter(py::object on_message)
{
    CallablePtr holder = make_holder(std::move(on_message));
    if (!holder)
        return nullptr;
    return [holder](const json& data) {
        PythonGILState gil;
        if (!gil)
            return;
        try {
            holder->fn(json_to_py(data));
        } catch (py::error_already_set& e) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.ui on_message handler raised: " << e.what();
            PyErr_Clear();
        }
    };
}

std::function<void()> make_close_adapter(const CallablePtr& holder)
{
    if (!holder)
        return nullptr;
    return [holder]() {
        PythonGILState gil;
        if (!gil)
            return;
        try {
            holder->fn();
        } catch (py::error_already_set& e) {
            BOOST_LOG_TRIVIAL(error) << "orca.host.ui on_close handler raised: " << e.what();
            PyErr_Clear();
        }
    };
}

class UiRegistry
{
public:
    static UiRegistry& instance()
    {
        static UiRegistry r;
        return r;
    }

    int reserve_id()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_next_id++;
    }
    void bind(int id, wxWindow* window, const std::string& plugin_key)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_resources[id] = window;
        m_owners[id]    = plugin_key;
    }
    void remove(int id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_resources.erase(id);
        m_owners.erase(id);
    }

    template<typename T>
    T* get_as(int id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_resources.find(id);
        if (it == m_resources.end())
            return nullptr;
        return dynamic_cast<T*>(it->second);
    }
    bool is_open(int id)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_resources.count(id) > 0;
    }
    std::vector<wxWindow*> take_for_plugin(const std::string& plugin_key)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        std::vector<wxWindow*>      out;
        for (auto it = m_owners.begin(); it != m_owners.end();) {
            if (it->second == plugin_key) {
                auto rit = m_resources.find(it->first);
                if (rit != m_resources.end()) {
                    out.push_back(rit->second);
                    m_resources.erase(rit);
                }
                it = m_owners.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }
    void bind_callback(const CallablePtr& callback, const std::string& plugin_key)
    {
        if (!callback)
            return;
        std::lock_guard<std::mutex> lk(m_mtx);
        m_callbacks[plugin_key].push_back(callback);
    }
    std::vector<CallablePtr> take_callbacks_for_plugin(const std::string& plugin_key)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto                        it = m_callbacks.find(plugin_key);
        if (it == m_callbacks.end())
            return {};
        std::vector<CallablePtr> callbacks;
        callbacks.reserve(it->second.size());
        for (const std::weak_ptr<GilSafeCallable>& weak_callback : it->second) {
            if (auto callback = weak_callback.lock())
                callbacks.push_back(std::move(callback));
        }
        m_callbacks.erase(it);
        return callbacks;
    }

private:
    std::mutex                                                               m_mtx;
    std::unordered_map<int, wxWindow*>                                       m_resources;
    std::unordered_map<int, std::string>                                     m_owners;
    std::unordered_map<std::string, std::vector<std::weak_ptr<GilSafeCallable>>> m_callbacks;
    int                                                                      m_next_id{1};
};

using plugin_host::app_closing;
using plugin_host::run_on_ui_blocking;

wxWindow* ui_parent()
{
    return wxTheApp == nullptr ? nullptr : dynamic_cast<wxWindow*>(GUI::wxGetApp().mainframe);
}

long message_style(const std::string& buttons, const std::string& icon)
{
    long style = wxOK;
    if (buttons == "ok_cancel")
        style = wxOK | wxCANCEL;
    else if (buttons == "yes_no")
        style = wxYES_NO;
    else if (buttons == "yes_no_cancel")
        style = wxYES_NO | wxCANCEL;

    if (icon == "warning")
        style |= wxICON_WARNING;
    else if (icon == "error")
        style |= wxICON_ERROR;
    else if (icon == "question")
        style |= wxICON_QUESTION;
    else
        style |= wxICON_INFORMATION;
    return style;
}

std::string button_to_string(int rc)
{
    switch (rc) {
    case wxID_OK:  return "ok";
    case wxID_YES: return "yes";
    case wxID_NO:  return "no";
    default:       return "cancel";
    }
}

std::string ui_message(const std::string& text, const std::string& title,
                       const std::string& buttons, const std::string& icon)
{
    const long style = message_style(buttons, icon);
    return run_on_ui_blocking([&]() -> std::string {
        GUI::MessageDialog dlg(nullptr, wxString::FromUTF8(text), wxString::FromUTF8(title), style);
        return button_to_string(dlg.ShowModal());
    });
}

constexpr long WINDOW_MODELESS = 0L;
constexpr long WINDOW_MODAL    = 1L << 0;
constexpr long PLUGIN_WX_STYLE = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER;

class PluginHtmlWindow : public wxDialog
{
public:
    using CloseHandler = std::function<void()>;

    PluginHtmlWindow(wxWindow*        parent,
                     const wxString&  title,
                     const std::string& html,
                     const wxSize&    size,
                     MessageHandler   on_message,
                     CloseHandler     on_close,
                     std::function<void()> on_destroyed)
        : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, size, PLUGIN_WX_STYLE)
        , m_on_message(std::move(on_message))
        , m_on_close(std::move(on_close))
        , m_on_destroyed(std::move(on_destroyed))
    {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_browser   = WebView::CreateWebView(this, wxString("about:blank"));
        if (m_browser == nullptr)
            throw std::runtime_error("Failed to create a WebView for orca.host.ui.create_window");
        sizer->Add(m_browser, 1, wxEXPAND);
        SetSizer(sizer);

        m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PluginHtmlWindow::on_script_message, this);
        Bind(wxEVT_CLOSE_WINDOW, &PluginHtmlWindow::on_close_window, this);

        const wxString page = wxString::FromUTF8(html);
        m_browser->SetPage(page, wxString("about:blank"));
    }

    ~PluginHtmlWindow() override
    {
        if (m_on_destroyed)
            m_on_destroyed();
    }

    void post_message(const json& data)
    {
        if (m_browser == nullptr)
            return;
        const std::string payload = data.dump();
        wxString          script  = "window.dispatchEvent(new CustomEvent('orca-message',{detail:";
        script += wxString::FromUTF8(payload);
        script += "})); if (window.orca && window.orca.onMessage) window.orca.onMessage(";
        script += wxString::FromUTF8(payload);
        script += ");";
        WebView::RunScript(m_browser, script);
    }

    void request_close()
    {
        Close();
    }

    void destroy_silently()
    {
        m_silent = true;
        Destroy();
    }

    static void post_message(PluginHtmlWindow* window, const json& data)
    {
        if (window != nullptr)
            window->post_message(data);
    }
    static void request_close(PluginHtmlWindow* window)
    {
        if (window != nullptr)
            window->request_close();
    }
    static void destroy_silently(PluginHtmlWindow* window)
    {
        if (window != nullptr)
            window->destroy_silently();
    }

private:
    void on_script_message(wxWebViewEvent& evt)
    {
        if (!m_on_message)
            return;
        try {
            m_on_message(json::parse(evt.GetString().ToUTF8().data()));
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "orca.host.ui window received a non-JSON script message";
        }
    }

    void on_close_window(wxCloseEvent& evt)
    {
        if (!m_silent && m_on_close)
            m_on_close();
        evt.Skip();
    }

    wxWebView*             m_browser{nullptr};
    MessageHandler         m_on_message;
    CloseHandler           m_on_close;
    std::function<void()>  m_on_destroyed;
    bool                   m_silent{false};
};

struct UiWindowHandle
{
    int id{0};
};

struct UiProgressHandle
{
    int id{0};
};

struct UiDockPanelHandle
{
    int id{0};
};

py::object ui_create_window(const std::string& html, const std::string& title, int width, int height,
                            py::object on_message, py::object on_close, long style, py::object /*on_submit*/)
{
    auto              msg_adapter  = make_message_adapter(std::move(on_message));
    CallablePtr       close_holder = make_holder(std::move(on_close));
    const std::string plugin_key   = PluginAuditManager::instance().current_plugin();
    const int         w            = width > 0 ? width : 820;
    const int         h            = height > 0 ? height : 600;

    if ((style & ~WINDOW_MODAL) != 0)
        throw std::invalid_argument("unsupported orca.host.ui.create_window style flags");
    const bool modal = (style & WINDOW_MODAL) != 0;

    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    const int new_id = UiRegistry::instance().reserve_id();
    UiRegistry::instance().bind(new_id, nullptr, plugin_key);

    GUI::wxGetApp().CallAfter([new_id, plugin_key, html, title, w, h,
                               msg_adapter = std::move(msg_adapter),
                               close_holder = std::move(close_holder), modal]() mutable {
        if (!UiRegistry::instance().is_open(new_id))
            return;

        auto on_destroyed = [new_id]() { UiRegistry::instance().remove(new_id); };
        auto* dlg = new PluginHtmlWindow(ui_parent(), wxString::FromUTF8(title), html, wxSize(w, h),
                                         std::move(msg_adapter), make_close_adapter(close_holder),
                                         std::move(on_destroyed));
        UiRegistry::instance().bind(new_id, dlg, plugin_key);
        if (modal) {
            dlg->ShowModal();
            if (UiRegistry::instance().is_open(new_id))
                dlg->Destroy();
        } else {
            dlg->Show();
        }
    });

    return py::cast(UiWindowHandle{new_id});
}

constexpr const char* DOCK_POSITIONS[] = {"left", "right", "bottom", "float"};

py::object ui_create_dock_panel(const std::string& /*html*/, const std::string& /*title*/, int /*width*/, int /*height*/,
                                py::object /*on_message*/, py::object /*on_close*/, const std::string& dock)
{
    if (std::find(std::begin(DOCK_POSITIONS), std::end(DOCK_POSITIONS), dock) == std::end(DOCK_POSITIONS))
        throw std::invalid_argument("orca.host.ui.create_dock_panel dock must be \"left\", \"right\", \"bottom\" or \"float\"");
    throw edge_plugin_api_unavailable("orca.host.ui.create_dock_panel");
}

void handle_post(int id, py::object data)
{
    if (wxTheApp == nullptr)
        return;
    json j = py_to_json(data);
    GUI::wxGetApp().CallAfter([id, j = std::move(j)]() {
        PluginHtmlWindow::post_message(UiRegistry::instance().get_as<PluginHtmlWindow>(id), j);
    });
}

void handle_close(int id)
{
    if (wxTheApp == nullptr)
        return;
    GUI::wxGetApp().CallAfter([id]() {
        PluginHtmlWindow::request_close(UiRegistry::instance().get_as<PluginHtmlWindow>(id));
    });
}

UiProgressHandle ui_create_progress_dialog(const std::string& title, const std::string& message, int maximum, int style)
{
    const std::string plugin_key = PluginAuditManager::instance().current_plugin();
    const int         max_value  = maximum > 0 ? maximum : 100;

    return run_on_ui_blocking([&]() -> UiProgressHandle {
        const int new_id = UiRegistry::instance().reserve_id();
        auto on_destroyed = [new_id]() { UiRegistry::instance().remove(new_id); };
        auto* dlg = GUI::PluginProgressDialog::create_dialog(ui_parent(), wxString::FromUTF8(title),
                                                             wxString::FromUTF8(message), max_value, style,
                                                             std::move(on_destroyed));
        UiRegistry::instance().bind(new_id, dlg, plugin_key);
        return UiProgressHandle{new_id};
    });
}

UiProgressHandle* new_progress_dialog(const std::string& title, const std::string& message, int maximum, int style)
{
    return new UiProgressHandle(ui_create_progress_dialog(title, message, maximum, style));
}

bool progress_is_open(int id) { return UiRegistry::instance().is_open(id); }

bool progress_pulse(int id, const std::string& message)
{
    return run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        return GUI::PluginProgressDialog::pulse(d, wxString::FromUTF8(message));
    });
}

bool progress_update(int id, int value, const std::string& message)
{
    return run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        return GUI::PluginProgressDialog::update(d, value, wxString::FromUTF8(message));
    });
}

void progress_start_pulse(int id, int interval_ms, const std::string& message)
{
    run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        GUI::PluginProgressDialog::start_pulse(d, interval_ms, wxString::FromUTF8(message));
    });
}

void progress_stop_pulse(int id)
{
    run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        GUI::PluginProgressDialog::stop_pulse(d);
    });
}

void progress_close(int id)
{
    run_on_ui_blocking([&]() {
        auto* d = UiRegistry::instance().get_as<GUI::PluginProgressDialog>(id);
        GUI::PluginProgressDialog::request_close(d);
    });
}

void plater_notification(GUI::NotificationManager::NotificationLevel notification_level, const std::string& text,
                         const std::string& hypertext, py::object on_click)
{
    const std::string plugin_key = PluginAuditManager::instance().current_plugin();
    CallablePtr       holder     = make_holder(std::move(on_click));
    if (holder)
        UiRegistry::instance().bind_callback(holder, plugin_key);

    std::function<bool(wxEvtHandler*)> callback;
    if (holder) {
        callback = [holder](wxEvtHandler*) -> bool {
            if (!holder->active.load(std::memory_order_acquire))
                return false;
            PythonGILState gil;
            if (!gil)
                return false;
            try {
                py::object result = holder->fn();
                return result.is_none() || result.cast<bool>();
            } catch (py::error_already_set& e) {
                BOOST_LOG_TRIVIAL(error) << "orca.host.ui notification callback raised: " << e.what();
                PyErr_Clear();
                return false;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "orca.host.ui notification callback raised: " << e.what();
                return false;
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "orca.host.ui notification callback raised an unknown exception";
                return false;
            }
        };
    }

    run_on_ui_blocking([notification_level, text, hypertext, callback = std::move(callback)]() mutable {
        GUI::Plater* plater = GUI::wxGetApp().plater();
        if (plater == nullptr || plater->get_notification_manager() == nullptr)
            throw std::runtime_error("Plater is not available");
        plater->get_notification_manager()->push_notification(GUI::NotificationType::CustomNotification,
                                                              notification_level, text, hypertext,
                                                              std::move(callback));
    });
}

} // namespace

void PluginHostUi::run_on_ui_thread(std::function<void()> fn)
{
    run_on_ui_blocking(std::move(fn));
}

void PluginHostUi::RegisterBindings(pybind11::module_& host)
{
    auto ui = host.def_submodule(
        "ui",
        "Host UI: native dialogs and interactive HTML windows. Calls run on the main/UI "
        "thread (marshaled from the plugin thread). Do not call these from a slicing pipeline hook.");

    ui.def("message", &ui_message, py::arg("text"), py::arg("title") = "OrcaSlicer", py::arg("buttons") = "ok",
           py::arg("icon") = "info",
           "Show a native modal message box; returns the clicked button id "
           "(\"ok\"/\"cancel\"/\"yes\"/\"no\"). buttons: \"ok\"|\"ok_cancel\"|\"yes_no\"|\"yes_no_cancel\"; "
           "icon: \"info\"|\"warning\"|\"error\"|\"question\".");

    ui.attr("PD_APP_MODAL")      = py::int_(wxPD_APP_MODAL);
    ui.attr("PD_AUTO_HIDE")      = py::int_(wxPD_AUTO_HIDE);
    ui.attr("PD_CAN_ABORT")      = py::int_(wxPD_CAN_ABORT);
    ui.attr("PD_CAN_SKIP")       = py::int_(wxPD_CAN_SKIP);
    ui.attr("PD_ELAPSED_TIME")   = py::int_(wxPD_ELAPSED_TIME);
    ui.attr("PD_ESTIMATED_TIME") = py::int_(wxPD_ESTIMATED_TIME);
    ui.attr("PD_REMAINING_TIME") = py::int_(wxPD_REMAINING_TIME);
    ui.attr("WINDOW_MODELESS")   = py::int_(WINDOW_MODELESS);
    ui.attr("WINDOW_MODAL")      = py::int_(WINDOW_MODAL);

    py::class_<UiWindowHandle>(ui, "UiWindow", "Handle to a plugin HTML window or modal dialog created by create_window().")
        .def_property_readonly("id", [](const UiWindowHandle& h) { return h.id; })
        .def("post", [](const UiWindowHandle& h, py::object data) { handle_post(h.id, std::move(data)); },
             py::arg("data"), "Send a payload to the page.")
        .def("close", [](const UiWindowHandle& h) { handle_close(h.id); }, "Close the window (fires on_close).")
        .def("is_open", [](const UiWindowHandle& h) { return UiRegistry::instance().is_open(h.id); },
             "Return True while the window is open.");

    ui.def("create_window", &ui_create_window, py::arg("html"), py::arg("title") = "OrcaSlicer", py::arg("width") = 820,
           py::arg("height") = 600, py::arg("on_message") = py::none(), py::arg("on_close") = py::none(),
           py::arg("style") = WINDOW_MODELESS, py::arg("on_submit") = py::none(),
           "Open a persistent HTML window or modal dialog and return a UiWindow.");

    py::class_<UiDockPanelHandle>(ui, "UiDockPanel",
                                  "Handle type for create_dock_panel(); docking is not available in this Edge build.")
        .def_property_readonly("id", [](const UiDockPanelHandle& h) { return h.id; })
        .def("post", [](const UiDockPanelHandle&, py::object) { throw edge_plugin_api_unavailable("orca.host.ui.create_dock_panel"); },
             py::arg("data"))
        .def("show", [](const UiDockPanelHandle&) { throw edge_plugin_api_unavailable("orca.host.ui.create_dock_panel"); })
        .def("hide", [](const UiDockPanelHandle&) { throw edge_plugin_api_unavailable("orca.host.ui.create_dock_panel"); })
        .def("close", [](const UiDockPanelHandle&) { throw edge_plugin_api_unavailable("orca.host.ui.create_dock_panel"); })
        .def("is_open", [](const UiDockPanelHandle&) { return false; });

    ui.def("create_dock_panel", &ui_create_dock_panel, py::arg("html"), py::arg("title") = "OrcaSlicer",
           py::arg("width") = 320, py::arg("height") = 480, py::arg("on_message") = py::none(),
           py::arg("on_close") = py::none(), py::arg("dock") = "right",
           "Open an HTML panel docked beside the 3D view. Not available in this Edge build.");

    py::class_<UiProgressHandle>(ui, "ProgressDialog", "Handle to a native progress dialog.")
        .def(py::init(&new_progress_dialog), py::arg("title"), py::arg("message"), py::arg("maximum") = 100,
             py::arg("style") = wxPD_APP_MODAL | wxPD_AUTO_HIDE)
        .def_property_readonly("id", [](const UiProgressHandle& h) { return h.id; })
        .def("pulse", [](const UiProgressHandle& h, const std::string& message) { return progress_pulse(h.id, message); },
             py::arg("message") = "")
        .def("update",
             [](const UiProgressHandle& h, int value, const std::string& message) {
                 return progress_update(h.id, value, message);
             },
             py::arg("value"), py::arg("message") = "")
        .def("start_pulse",
             [](const UiProgressHandle& h, int interval_ms, const std::string& message) {
                 progress_start_pulse(h.id, interval_ms, message);
             },
             py::arg("interval_ms") = 100, py::arg("message") = "")
        .def("stop_pulse", [](const UiProgressHandle& h) { progress_stop_pulse(h.id); })
        .def("close", [](const UiProgressHandle& h) { progress_close(h.id); })
        .def("is_open", [](const UiProgressHandle& h) { return progress_is_open(h.id); })
        .def("__enter__", [](UiProgressHandle& h) -> UiProgressHandle& { return h; }, py::return_value_policy::reference_internal)
        .def("__exit__", [](const UiProgressHandle& h, py::object, py::object, py::object) {
            progress_close(h.id);
            return false;
        });

    ui.def("create_progress_dialog", &ui_create_progress_dialog, py::arg("title"), py::arg("message"),
           py::arg("maximum") = 100, py::arg("style") = wxPD_APP_MODAL | wxPD_AUTO_HIDE,
           "Create a native progress dialog and return a ProgressDialog handle.");

    py::enum_<GUI::NotificationManager::NotificationLevel>(ui, "NotificationLevel")
        .value("ProgressBarNotificationLevel", GUI::NotificationManager::NotificationLevel::ProgressBarNotificationLevel)
        .value("HintNotificationLevel", GUI::NotificationManager::NotificationLevel::HintNotificationLevel)
        .value("RegularNotificationLevel", GUI::NotificationManager::NotificationLevel::RegularNotificationLevel)
        .value("PrintInfoNotificationLevel", GUI::NotificationManager::NotificationLevel::PrintInfoNotificationLevel)
        .value("PrintInfoShortNotificationLevel", GUI::NotificationManager::NotificationLevel::PrintInfoShortNotificationLevel)
        .value("ImportantNotificationLevel", GUI::NotificationManager::NotificationLevel::ImportantNotificationLevel)
        .value("WarningNotificationLevel", GUI::NotificationManager::NotificationLevel::WarningNotificationLevel)
        .value("SeriousWarningNotificationLevel", GUI::NotificationManager::NotificationLevel::SeriousWarningNotificationLevel)
        .value("ErrorNotificationLevel", GUI::NotificationManager::NotificationLevel::ErrorNotificationLevel)
        .export_values();

    ui.def("push_notification", &plater_notification, py::arg("notification_level"), py::arg("text"),
           py::arg("hyper_text") = "", py::arg("on_click") = py::none(),
           "Push a plater notification. hyper_text is an underlined label; on_click() is called when it is clicked "
           "and may return True to close the notification.");
}

void PluginHostUi::close_windows_for_plugin(const std::string& plugin_key)
{
    if (wxTheApp == nullptr)
        return;

    auto teardown = [plugin_key]() {
        for (auto& callback : UiRegistry::instance().take_callbacks_for_plugin(plugin_key))
            callback->disable();

        for (auto* window : UiRegistry::instance().take_for_plugin(plugin_key)) {
            if (auto* dialog = dynamic_cast<PluginHtmlWindow*>(window))
                PluginHtmlWindow::destroy_silently(dialog);
            else if (auto* progress = dynamic_cast<GUI::PluginProgressDialog*>(window))
                GUI::PluginProgressDialog::request_close(progress);
            else if (window != nullptr)
                window->Destroy();
        }
    };

    if (wxIsMainThread())
        teardown();
    else
        GUI::wxGetApp().CallAfter(teardown);
}

} // namespace Slic3r
