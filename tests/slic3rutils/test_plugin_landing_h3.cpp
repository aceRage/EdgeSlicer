// H3 regression tests for the findings of tests/pr_review_2026-11-18_plugin_stack.md that the
// landing branch (feature/plugins-landing) fixes: H1 start-up hooks, H2 helper message
// validation, H3 GIL-less UI marshal, H4 fail-closed helper, H5 secret stores.
// Only fixture plugins written by these tests run; nothing is downloaded.

#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/host/PluginHostUiMarshal.hpp>
#include <slic3r/plugin/ipc/PluginHostClient.hpp>
#include <slic3r/plugin/ipc/PluginHostProcess.hpp>
#include <slic3r/plugin/ipc/PluginIpcProtocol.hpp>
#include <slic3r/plugin/ipc/PluginSandbox.hpp>

#include "slic3r/plugin/EdgePythonInclude.hpp"
#include <pybind11/embed.h>

#include <boost/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace py = pybind11;
using plugin_ipc::PluginHostClient;

namespace {

struct [[maybe_unused]] ScopedPluginManager
{
    ScopedDataDir python_data_dir{"plugin-landing-python"};
    bool          initialized = false;

    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

[[maybe_unused]] void write_text(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    out << text;
}

[[maybe_unused]] ScriptCompletion run_script_and_wait(const std::string& plugin_key, const std::string& cap, std::chrono::seconds timeout)
{
    std::mutex              mutex;
    std::condition_variable cv;
    bool                    done = false;
    ScriptCompletion        completion;
    auto launch = PluginManager::instance().launch_script_capability(plugin_key, cap, [&](const ScriptCompletion& c) {
        std::lock_guard<std::mutex> lock(mutex);
        completion = c;
        done       = true;
        cv.notify_all();
    });
    REQUIRE(launch.launched_async);
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(cv.wait_for(lock, timeout, [&] { return done; }));
    return completion;
}

// A fixture plugin that reports, from wherever it runs, whether start-up hook code ran and
// whether a module from the shared packages folder can be imported.
[[maybe_unused]] const char* const H1_PROBE_SOURCE = R"PY(# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "H1 Probe"
# type = "script"
# ///
import builtins, sys, orca

class Probe(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Probe"
    def execute(self):
        planted = hasattr(builtins, "EDGE_H1_PLANTED") or "sitecustomize" in sys.modules
        try:
            import edge_h1_probe
            probe = edge_h1_probe.VALUE
        except Exception as ex:
            probe = "import failed: " + str(ex)
        return orca.ExecutionResult.success("planted=%s probe=%s no_site=%s" % (planted, probe, sys.flags.no_site))

@orca.plugin
class Pkg(orca.base):
    def register_capabilities(self):
        orca.register_capability(Probe)
)PY";

} // namespace

// ---- H3 -------------------------------------------------------------------------------------

namespace {

// A stand-in for the wx UI thread: runs posted tasks in order until stopped.
struct FakeUiThread
{
    std::mutex                        mutex;
    std::condition_variable           cv;
    std::deque<std::function<void()>> tasks;
    bool                              stop = false;
    std::thread                       thread;

    FakeUiThread()
    {
        thread = std::thread([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->mutex);
                    cv.wait(lock, [this] { return stop || !tasks.empty(); });
                    if (tasks.empty())
                        return;
                    task = std::move(tasks.front());
                    tasks.pop_front();
                }
                task();
            }
        });
    }
    ~FakeUiThread()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        cv.notify_all();
        thread.join();
    }
    void post(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back(std::move(task));
        }
        cv.notify_all();
    }
};

} // namespace

TEST_CASE("H3: the UI marshal runs from a thread without the GIL (the host RPC worker) without a fatal error",
          "[PluginLanding][H3][Python]")
{
    ScopedPluginManager plugin_system;
    if (!plugin_system.initialized)
        SKIP("Bundled Python interpreter unavailable: " + PythonInterpreter::instance().last_error());
    REQUIRE(Py_IsInitialized());

    FakeUiThread ui;
    auto         post    = [&ui](std::function<void()> t) { ui.post(std::move(t)); };
    auto         not_ui  = [] { return false; };
    auto         running = [] { return false; };

    // The plugin-host IPC worker: a plain std::thread that never held the GIL. The old marshal
    // built pybind11::gil_scoped_release here, which is PyEval_SaveThread() without a thread
    // state: Py_FatalError in Python 3.12.
    std::atomic<int>  value{0};
    std::atomic<bool> released{true};
    std::string       failure;
    std::thread worker([&] {
        try {
            plugin_host::ScopedGilReleaseIfHeld probe;
            released = probe.released();
            value    = plugin_host::run_on_ui_blocking_with([] { return 42; }, post, not_ui, running);
        } catch (const std::exception& ex) {
            failure = ex.what();
        }
    });
    worker.join();
    INFO(failure);
    CHECK(failure.empty());
    CHECK(value == 42);
    CHECK_FALSE(released);

    // A caller that holds the GIL still releases it, so the UI task can take the GIL itself.
    std::atomic<bool> ui_took_gil{false};
    std::thread       holder([&] {
        try {
            py::gil_scoped_acquire gil;
            plugin_host::run_on_ui_blocking_with(
                [&] {
                    py::gil_scoped_acquire ui_gil;
                    ui_took_gil = true;
                },
                post, not_ui, running, std::chrono::milliseconds(20000));
        } catch (const std::exception& ex) {
            failure = ex.what();
        }
    });
    holder.join();
    INFO(failure);
    CHECK(ui_took_gil);
}

TEST_CASE("H3: a UI thread that never answers turns into an error, not a hang", "[PluginLanding][H3]")
{
    std::vector<std::function<void()>> dropped; // posted but never run
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_THROWS_WITH(plugin_host::run_on_ui_blocking_with([] { return 1; }, [&](std::function<void()> t) { dropped.push_back(std::move(t)); },
                                                           [] { return false; }, [] { return false; }, std::chrono::milliseconds(300)),
                      Catch::Contains("did not answer"));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    // Run late: the abandoned task does nothing.
    for (auto& t : dropped)
        CHECK_NOTHROW(t());
}

