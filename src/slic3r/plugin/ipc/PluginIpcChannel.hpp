#pragma once

#include "PluginIpcProtocol.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r::plugin_ipc {

// Bidirectional length-prefixed JSON channel over a POSIX socketpair fd or a Windows named pipe.
class Channel
{
public:
    Channel() = default;
    Channel(const Channel&)            = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&& other) noexcept;
    Channel& operator=(Channel&& other) noexcept;
    ~Channel() { close(); }

#ifdef _WIN32
    static Channel from_windows_handle(void* handle);
#else
    static Channel from_posix_fd(int fd);
#endif

    bool valid() const;
    void close();

    // timeout_ms < 0 waits forever. 0 is a non-blocking probe.
    bool write_json(const nlohmann::json& message, int timeout_ms, std::string& error);
    bool read_json(nlohmann::json& message, int timeout_ms, std::string& error);

    // Closed-pipe / peer-gone after a successful open.
    bool peer_closed() const { return m_peer_closed; }

private:
    bool write_all(const uint8_t* data, size_t size, int timeout_ms, std::string& error);
    bool read_all(uint8_t* data, size_t size, int timeout_ms, std::string& error);

#ifdef _WIN32
    void* m_handle = nullptr;
#else
    int m_fd = -1;
#endif
    bool m_peer_closed = false;
};

} // namespace Slic3r::plugin_ipc
