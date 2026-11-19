// Regression tests for the Medium findings of tests/pr_review_2026-11-18_plugin_stack.md that
// the landing branch fixes (M1, M2, M4, M8, M13). Only fixture plugins and the repo's helper run.

#include <catch2/catch.hpp>

#include "plugin_test_utils.hpp"

#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>
#include <slic3r/plugin/ipc/PluginHostProcess.hpp>
#include <slic3r/plugin/ipc/PluginSandbox.hpp>

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace Slic3r;

#ifdef _WIN32
TEST_CASE("M4: the helper's job forbids clipboard, desktop, atoms, USER handles and system settings",
          "[PluginLanding][M4][PluginSandbox][windows]")
{
    if (!plugin_ipc::helper_exists())
        SKIP("edge-plugin-host is not next to the test binary");
    ScopedDataDir     data("plugin-landing-m4");
    ScopedSpawnedHost spawned;
    plugin_ipc::SandboxSpec spec;
    // A test-only key: its AppContainer profile is EdgeSlicer.t.* and is removed afterwards.
    plugin_ipc::set_test_only_plugin_keys_for_testing(true);
    spec.plugin_key  = "test.M4_Probe." + std::to_string(::GetCurrentProcessId());
    spec.data_dir    = data.dir.string();
    spec.python_home = PythonInterpreter::bundled_python_home();
    spawned.sandbox_plugin_key = spec.plugin_key;
    std::string error;
    const bool  ok = plugin_ipc::spawn_host(spawned.host, spec, error);
    INFO(error);
    REQUIRE(ok);
    REQUIRE(spawned.host.job != nullptr);

    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    REQUIRE(::QueryInformationJobObject(static_cast<HANDLE>(spawned.host.job), JobObjectBasicUIRestrictions, &ui, sizeof(ui), nullptr));
    for (const DWORD flag : {DWORD(JOB_OBJECT_UILIMIT_DESKTOP), DWORD(JOB_OBJECT_UILIMIT_DISPLAYSETTINGS), DWORD(JOB_OBJECT_UILIMIT_EXITWINDOWS),
                             DWORD(JOB_OBJECT_UILIMIT_GLOBALATOMS), DWORD(JOB_OBJECT_UILIMIT_HANDLES), DWORD(JOB_OBJECT_UILIMIT_READCLIPBOARD),
                             DWORD(JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS), DWORD(JOB_OBJECT_UILIMIT_WRITECLIPBOARD)}) {
        INFO(flag);
        CHECK((ui.UIRestrictionsClass & flag) == flag);
    }
}
#endif
