#pragma once


#include <boost/filesystem/path.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Slic3r { struct PluginDescriptor; }

namespace Slic3r {
struct Plugin;
} // namespace Slic3r

namespace Slic3r::plugin_loader {
// registration_failed, when given, is set if the plugin's own registration step rejected the load.
bool load(const PluginDescriptor&                          descriptor,
          bool                                             skip_deps,
          const std::vector<std::string>&                  capabilities_to_enable,
          const std::function<std::string(const Plugin&)>& registry_precheck,
          Plugin&                                          out,
          std::string&                                     error,
          bool*                                            registration_failed = nullptr);

// Run on_unload() on every capability, drop them, and release the module. Safe to call on a
// not-loaded Plugin, and safe after the interpreter has been finalized (in which case the module
// reference is deliberately leaked rather than DECREF'd).
void unload(Plugin& plugin);

// True when `requirement` is a PEP 508 name / extras / version-specifiers / markers requirement.
// Empty entries, leading dashes (uv/pip installer options), URLs, archive names (.whl, .tar.gz,
// .zip, …), paths, and other non-requirement forms are refused so PEP 723 / Requires-Dist strings
// cannot be appended to uv's argv as options or local files.
bool is_pep508_requirement(const std::string& requirement);

// Validates every entry; a refused dependency writes a clear error and returns false.
bool validate_pep508_requirements(const std::vector<std::string>& pkgs, std::string& error);

// H6 (D8 default: wheels only, pinned, hashed). A dependency must read
//   <name>[extras]==<exact version>[; marker] --hash=sha256:<64 hex> [--hash=sha256:<64 hex> ...]
// so uv can install it with --require-hashes. The whole locked set (transitive dependencies too)
// has to be listed: uv refuses anything without a hash. On success `requirement_lines` holds the
// lines of the requirements file uv reads.
bool validate_locked_requirements(const std::vector<std::string>& pkgs,
                                  std::vector<std::string>&       requirement_lines,
                                  std::string&                    error);

// argv after the uv executable:
//   pip install --no-config --python … --no-python-downloads --target … --only-binary :all:
//   --no-build --require-hashes --index-url https://pypi.org/simple -r <requirements_file>
// Pre-built wheels from PyPI only, never a source build (no setup.py runs) and no other index.
std::vector<std::string> uv_install_args(const std::string& python_executable,
                                         const std::string& target_dir,
                                         const std::string& requirements_file);

// True when `key` is on the keep-list copied into uv's child environment.
bool uv_child_environment_keeps(const char* key);

// Install Python dependencies into the shared packages directory via the bundled uv. Blocks, with
// a 120 s cap. Each entry must pass validate_locked_requirements(). A dependency that drops an
// interpreter start-up hook (sitecustomize, usercustomize, *.pth) into the shared folder is
// refused and the hook files are removed (H1).
bool install_packages(const std::vector<std::string>& pkgs, std::string& error);

// Removes the start-up hook files (PythonInterpreter::find_startup_hook_files) in target_dir that are
// not in hooks_before and fails with a message naming them. True when none are new.
bool refuse_new_startup_hooks(const std::string& target_dir, const std::vector<std::string>& hooks_before, std::string& error);

// Read a local .py/.whl package's metadata without installing it, and report whether a package is
// already installed under the same directory.
bool inspect_local_plugin_package(const boost::filesystem::path& filepath,
                                  PluginDescriptor&              plugin_descriptor,
                                  bool&                          existing_installation,
                                  std::string&                   error);

// Copy a .py/.whl package into the plugin directory (the per-user cloud directory when the
// descriptor carries a cloud UUID and cloud_user_id is non-empty) and write its
// .install_state.json sidecar, backing up and restoring any existing installation on failure.
bool install_plugin(const boost::filesystem::path& filepath,
                    const std::string&             cloud_user_id,
                    PluginDescriptor&              plugin_descriptor,
                    std::string&                   error);
bool install_plugin(const boost::filesystem::path& filepath, const std::string& cloud_user_id, std::string& error);

} // namespace Slic3r::plugin_loader
