#pragma once

#include <boost/filesystem/path.hpp>
#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/system/error_code.hpp>

#include <optional>
#include <string>

#include "test_utils.hpp"

#ifdef EDGE_PYTHON_PLUGINS
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginTrust.hpp>
#endif

// Catch2 v2 has no SKIP(); keep the Orca tests' SKIP("...") calls compiling.
#ifndef SKIP
#define SKIP(msg) do { WARN(msg); return; } while (0)
#endif

namespace Slic3r {

// Owns a unique temp directory (create + recursive remove). Edge's test_utils.hpp
// does not provide Orca's ScopedTemporaryDir, so the plugin tests keep one here.
struct ScopedTemporaryDir
{
    boost::filesystem::path m_path;

    explicit ScopedTemporaryDir(const std::string& tag)
    {
        boost::system::error_code ec;
        m_path = boost::filesystem::temp_directory_path() /
                 boost::filesystem::unique_path("orca-" + tag + "-%%%%-%%%%");
        boost::filesystem::create_directories(m_path, ec);
    }

    ~ScopedTemporaryDir()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(m_path, ec);
    }

    const boost::filesystem::path& path() const { return m_path; }

    ScopedTemporaryDir(const ScopedTemporaryDir&)            = delete;
    ScopedTemporaryDir& operator=(const ScopedTemporaryDir&) = delete;
};

// Temp file path reserved for the caller to write; removed on scope exit.
// Orca keeps this in tests/test_utils.hpp; Edge's copy does not, so plugin tests own it.
struct ScopedTemporaryFile
{
    boost::filesystem::path m_path;

    explicit ScopedTemporaryFile(const std::string& extension = ".tmp")
        : m_path(boost::filesystem::temp_directory_path() /
                 boost::filesystem::unique_path("orca-%%%%-%%%%-%%%%" + extension))
    {}

    ~ScopedTemporaryFile()
    {
        boost::system::error_code ec;
        boost::filesystem::remove(m_path, ec);
    }

    const boost::filesystem::path& path() const { return m_path; }
    std::string                    string() const { return m_path.string(); }

    ScopedTemporaryFile(const ScopedTemporaryFile&)            = delete;
    ScopedTemporaryFile& operator=(const ScopedTemporaryFile&) = delete;
};

// Sets an environment variable for the lifetime of the guard and restores the previous value.
struct ScopedEnvVar
{
    std::string                m_name;
    std::optional<std::string> m_previous;

    ScopedEnvVar(std::string name, const std::string& value) : m_name(std::move(name))
    {
        if (const char* old = boost::nowide::getenv(m_name.c_str()))
            m_previous = old;
        boost::nowide::setenv(m_name.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar()
    {
        if (m_previous)
            boost::nowide::setenv(m_name.c_str(), m_previous->c_str(), 1);
        else
            boost::nowide::unsetenv(m_name.c_str());
    }

    ScopedEnvVar(const ScopedEnvVar&)            = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
};

// Unsets an environment variable for the lifetime of the guard and restores the previous value.
struct ScopedUnsetEnv
{
    std::string                m_name;
    std::optional<std::string> m_previous;

    explicit ScopedUnsetEnv(std::string name) : m_name(std::move(name))
    {
        if (const char* old = boost::nowide::getenv(m_name.c_str()))
            m_previous = old;
        boost::nowide::unsetenv(m_name.c_str());
    }

    ~ScopedUnsetEnv()
    {
        if (m_previous)
            boost::nowide::setenv(m_name.c_str(), m_previous->c_str(), 1);
        else
            boost::nowide::unsetenv(m_name.c_str());
    }

    ScopedUnsetEnv(const ScopedUnsetEnv&)            = delete;
    ScopedUnsetEnv& operator=(const ScopedUnsetEnv&) = delete;
};

// Point data_dir() at a throwaway directory for the lifetime of a test and
// restore the previous value afterwards, so code under test writes into a
// disposable tree and tests don't leak state into each other.
struct ScopedDataDir
{
    ScopedTemporaryDir        tmp;   // owns the temp dir (create + recursive remove)
    boost::filesystem::path   dir;   // = tmp.path(); kept as a member for callers
    std::string               previous;

    explicit ScopedDataDir(const std::string& tag)
        : tmp("orca-" + tag), dir(tmp.path()), previous(data_dir())
    {
        set_data_dir(dir.string());
    }

    ~ScopedDataDir() { set_data_dir(previous); } // tmp removes the directory

    // The plugin manager scans {data_dir}/orca_plugins.
    boost::filesystem::path plugins_dir() const { return dir / "orca_plugins"; }

    ScopedDataDir(const ScopedDataDir&)            = delete;
    ScopedDataDir& operator=(const ScopedDataDir&) = delete;
};

// Point resources_dir() at a throwaway directory for the lifetime of a test and restore the
// previous value afterwards, mirroring ScopedDataDir.
struct ScopedResourcesDir
{
    ScopedTemporaryDir      tmp;
    boost::filesystem::path dir;
    std::string             previous;

    explicit ScopedResourcesDir(const std::string& tag)
        : tmp("orca-" + tag), dir(tmp.path()), previous(resources_dir())
    {
        set_resources_dir(dir.string());
    }

    ~ScopedResourcesDir() { set_resources_dir(previous); }

    ScopedResourcesDir(const ScopedResourcesDir&)            = delete;
    ScopedResourcesDir& operator=(const ScopedResourcesDir&) = delete;
};

#ifdef EDGE_PYTHON_PLUGINS
// Phase 5: enable/run refuse an unpinned local package. Existing Catch2 loads pin the discovered
// entry so they still exercise Python, not the trust prompt.
inline bool approve_plugin_for_test(const std::string& plugin_key, std::string& error)
{
    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor)) {
        error = "plugin not discovered: " + plugin_key;
        return false;
    }
    const std::string hash = plugin_trust::sha256_file(descriptor.entry_path, error);
    if (hash.empty())
        return false;
    return plugin_trust::pin_hash(descriptor.plugin_key, hash, error);
}
#endif

} // namespace Slic3r
