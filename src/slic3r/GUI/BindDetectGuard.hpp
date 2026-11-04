#ifndef slic3r_GUI_BindDetectGuard_hpp_
#define slic3r_GUI_BindDetectGuard_hpp_

// "Connect via IP" with auto-detect (InputIpAddressDialog) asks the network plug-in's bind_detect
// who answers at the address the user typed, then inserts that printer. A plug-in that returned 0
// with an empty result used to get a printer inserted with an empty serial number and an empty
// connection type (found 2026-10-11: UltraNet's bind_detect was a stub on Windows). The dialog now
// runs every detection result through vet_bind_detect_result() first. Pure, so it is unit-tested
// (tests/slic3rutils/bind_detect_guard_tests.cpp).

#include "slic3r/Utils/bambu_networking.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI {

// The dialog's codes for a detection that did not produce a printer (workerThreadFunc):
// -1 "Failed to connect to printer.", -2 "Failed to publish login request.", and -3, which switches
// the dialog to manual entry (serial number + model), as macOS always does.
constexpr int kBindDetectConnectFailed = -1;
constexpr int kBindDetectManualEntry   = -3;

namespace bind_detect_detail {
inline std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
inline std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    return s;
}
inline std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}
} // namespace bind_detect_detail

// A Bambu serial number as the rest of the app uses it: letters and digits only. It ends up in MQTT
// topics (device/<sn>/report) and in AppConfig keys, so '/', '+', '#', spaces and the like are refused.
inline bool is_usable_serial(const std::string &sn)
{
    if (sn.empty() || sn.size() > 64) return false;
    return std::all_of(sn.begin(), sn.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
}

// Checks and normalises what bind_detect returned. Returns the code the dialog acts on:
//  * a negative `result` is passed through unchanged;
//  * 0 with no usable serial number (empty, blank, or with characters a serial never has) becomes
//    kBindDetectManualEntry: the dialog asks for the serial instead of inserting a printer without one;
//  * otherwise 0, with the result normalised: the serial trimmed and upper-cased (the manual path
//    does the same), the name defaulting to the serial, and connect_type / bind_state lower-cased,
//    an empty one taken as "lan" / "free" (the user is adding a LAN printer with its access code;
//    an empty connection type would keep it out of the LAN printer list).
inline int vet_bind_detect_result(int result, BBL::detectResult &d)
{
    using namespace bind_detect_detail;
    if (result < 0) return result;

    const std::string sn = upper(trim(d.dev_id));
    if (!is_usable_serial(sn)) return kBindDetectManualEntry;
    d.dev_id = sn;

    d.dev_name = trim(d.dev_name);
    if (d.dev_name.empty()) d.dev_name = sn;
    d.model_id = trim(d.model_id);

    d.connect_type = lower(trim(d.connect_type));
    if (d.connect_type.empty()) d.connect_type = "lan";
    d.bind_state = lower(trim(d.bind_state));
    if (d.bind_state.empty()) d.bind_state = "free";
    return 0;
}

// ---- Worker-thread safety for the dialog (found 2026-10-27, bind by IP crashed on macOS / Linux) ----
//
// 1. An exception that leaves a std::thread / boost::thread body is std::terminate: the whole slicer
//    vanishes. run_guarded() runs the body and turns any exception (also a non-std one thrown by the
//    network plug-in) into on_error(what) instead.
// 2. A worker must never touch the dialog directly: Cancel / close can destroy it while bind_detect is
//    still waiting on the network. The dialog owns an AliveToken that it clears on the UI thread when
//    it goes away; the worker only posts UI-thread closures that re-check the token first
//    (ui_alive()), so the dialog cannot be destroyed while a closure runs.
using AliveToken = std::shared_ptr<std::atomic<bool>>;

inline AliveToken make_alive_token() { return std::make_shared<std::atomic<bool>>(true); }
inline bool ui_alive(const AliveToken &t) { return t && t->load(std::memory_order_acquire); }
inline void kill_alive_token(const AliveToken &t)
{
    if (t) t->store(false, std::memory_order_release);
}

// Runs fn(); an exception goes to on_error(const std::string &what) (which must not throw; if it does
// anyway that is swallowed too). Returns true when fn() completed.
template<class Fn, class OnError> bool run_guarded(Fn &&fn, OnError &&on_error) noexcept
{
    try {
        fn();
        return true;
    } catch (const std::exception &e) {
        try { on_error(std::string(e.what())); } catch (...) {}
    } catch (...) {
        try { on_error(std::string("unknown exception")); } catch (...) {}
    }
    return false;
}

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_BindDetectGuard_hpp_
