#pragma once

// Shared UI-thread marshal for plugin host bindings. The function runs with the GIL released
// so a Model / PresetBundle copy on the UI thread cannot deadlock a worker that holds the GIL.

#include <slic3r/GUI/GUI_App.hpp>

#include <pybind11/gil.h>
#include <pybind11/pybind11.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <wx/app.h>
#include <wx/thread.h>

namespace Slic3r {
namespace plugin_host {

inline bool app_closing() { return wxTheApp == nullptr || GUI::wxGetApp().is_closing(); }

enum : int { UiCallPending, UiCallClaimed, UiCallAbandoned };

template<typename Fn>
auto run_on_ui_blocking(Fn&& fn) -> std::invoke_result_t<Fn&>
{
    using R = std::invoke_result_t<Fn&>;
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    if (wxIsMainThread()) {
        pybind11::gil_scoped_release nogil;
        return fn();
    }

    if (app_closing())
        throw std::runtime_error("OrcaSlicer is closing");

    struct Call
    {
        std::promise<R>  prom;
        std::atomic<int> state{UiCallPending};
    };
    auto           call = std::make_shared<Call>();
    std::future<R> fut  = call->prom.get_future();

    pybind11::gil_scoped_release nogil;
    GUI::wxGetApp().CallAfter([call, &fn]() {
        int expected = UiCallPending;
        if (!call->state.compare_exchange_strong(expected, UiCallClaimed))
            return;
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
                call->prom.set_value();
            } else {
                call->prom.set_value(fn());
            }
        } catch (...) {
            call->prom.set_exception(std::current_exception());
        }
    });
    while (fut.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
        int expected = UiCallPending;
        if (app_closing() && call->state.compare_exchange_strong(expected, UiCallAbandoned))
            throw std::runtime_error("OrcaSlicer is closing");
    }
    return fut.get();
}

} // namespace plugin_host
} // namespace Slic3r
