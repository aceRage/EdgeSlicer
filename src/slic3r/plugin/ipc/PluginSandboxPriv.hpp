#pragma once

// Platform hooks for PluginSandbox.cpp. Not a public header.

#include "PluginSandbox.hpp"

namespace Slic3r::plugin_ipc {

bool apply_linux_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error);
bool apply_win_child_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error);
bool apply_mac_sandbox(const SandboxSpec& spec, SandboxReport& report, std::string& error);

bool  prepare_win_appcontainer(const SandboxSpec& spec, std::string& error);
void  close_win_appcontainer();
void  sweep_win_acl_journal();
void* win_security_capabilities();
void* win_pipe_security_attributes();
#ifdef _WIN32
// Parent-known roots the ACL journal sweep may touch. Helper-writable paths are not included
// as extra roots; data_dir / python_home / helper / resources / temp are.
std::vector<std::string> parent_sandbox_known_roots();
#endif

bool probe_linux_sandbox(SandboxProbe& probe);
bool probe_win_sandbox(SandboxProbe& probe);
bool probe_mac_sandbox(SandboxProbe& probe);

} // namespace Slic3r::plugin_ipc
