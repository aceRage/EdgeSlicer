#pragma once

namespace Slic3r::plugin_ipc {

class Channel;

// Child-side session. Blocks until shutdown or a fatal transport error.
int run_child(Channel& channel);

} // namespace Slic3r::plugin_ipc
