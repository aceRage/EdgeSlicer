#include <catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/system/error_code.hpp>

#include <fstream>
#include <string>

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PythonInterpreter.hpp>

#include "plugin_test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

struct ScopedPluginManager
{
    bool initialized = false;
    ScopedPluginManager() { initialized = PluginManager::instance().initialize(); }
    ~ScopedPluginManager() { PluginManager::instance().shutdown(); }
};

bool dir_exists(const fs::path& p)
{
    boost::system::error_code ec;
    return fs::exists(p, ec) && fs::is_directory(p, ec);
}

} // namespace

TEST_CASE("Python plugin code never creates or writes {data_dir}/plugins", "[PluginIsolation][Python]")
{
    ScopedDataDir data("ultranet-isolation");
    const fs::path ultranet = data.dir / "plugins";
    REQUIRE_FALSE(dir_exists(ultranet));

    ScopedPluginManager mgr;
    REQUIRE(mgr.initialized);

    PluginManager::instance().discover_plugins(false, false);

    REQUIRE(dir_exists(data.plugins_dir()));
    REQUIRE_FALSE(dir_exists(ultranet));

    const std::vector<std::string> dirs = get_plugin_directories(std::string());
    for (const std::string& dir : dirs) {
        const fs::path p(dir);
        INFO(dir);
        CHECK(p != ultranet);
        CHECK(p.filename() != "plugins");
    }

    PluginDescriptor descriptor;
    std::string      error;
    CHECK_FALSE(delete_plugin_root(ultranet, "must-not-touch-ultranet", error));
    CHECK_FALSE(dir_exists(ultranet));

    CHECK_FALSE(delete_plugin_root(data.plugins_dir(), "must-not-delete-orca-plugins-parent", error));
    CHECK(dir_exists(data.plugins_dir()));
    REQUIRE_FALSE(dir_exists(ultranet));
}

TEST_CASE("Installing a local .py plugin writes under orca_plugins not plugins", "[PluginIsolation][Python]")
{
    ScopedDataDir data("ultranet-isolation-install");
    const fs::path ultranet = data.dir / "plugins";

    ScopedPluginManager mgr;
    REQUIRE(mgr.initialized);

    const fs::path py = data.dir / "echo.py";
    {
        std::ofstream out(py.string());
        out << "# /// script\n"
               "# requires-python = \">=3.12\"\n"
               "# dependencies = []\n"
               "#\n"
               "# [tool.orcaslicer.plugin]\n"
               "# name = \"echo-isolation\"\n"
               "# version = \"0.0.1\"\n"
               "# type = \"script\"\n"
               "# ///\n"
               "import orca\n"
               "class Echo(orca.script.ScriptBase):\n"
               "    def name(self):\n"
               "        return 'echo'\n"
               "orca.register_capability(Echo)\n";
    }

    std::string error;
    REQUIRE(plugin_loader::install_plugin(py, std::string(), error));
    CHECK(error.empty());

    REQUIRE(dir_exists(data.plugins_dir()));
    REQUIRE_FALSE(dir_exists(ultranet));

    bool wrote_ultranet = false;
    if (dir_exists(ultranet)) {
        for (fs::directory_iterator it(ultranet); it != fs::directory_iterator(); ++it)
            wrote_ultranet = true;
    }
    CHECK_FALSE(wrote_ultranet);
}
