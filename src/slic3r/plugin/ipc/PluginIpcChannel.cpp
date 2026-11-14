#include "PluginIpcChannel.hpp"

#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace Slic3r::plugin_ipc {
namespace {

int remaining_ms(const std::chrono::steady_clock::time_point& deadline)
{
    if (deadline == std::chrono::steady_clock::time_point::max())
        return -1;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0)
        return 0;
    if (left.count() > 1'000'000'000)
        return 1'000'000'000;
    return static_cast<int>(left.count());
}

std::chrono::steady_clock::time_point deadline_from_timeout(int timeout_ms)
{
    if (timeout_ms < 0)
        return std::chrono::steady_clock::time_point::max();
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
}

} // namespace

Channel::Channel(Channel&& other) noexcept
{
    *this = std::move(other);
}

Channel& Channel::operator=(Channel&& other) noexcept
{
    if (this == &other)
        return *this;
    close();
#ifdef _WIN32
    m_handle          = other.m_handle;
    other.m_handle    = nullptr;
#else
    m_fd       = other.m_fd;
    other.m_fd = -1;
#endif
    m_peer_closed       = other.m_peer_closed;
    other.m_peer_closed = false;
    return *this;
}

#ifdef _WIN32
Channel Channel::from_windows_handle(void* handle)
{
    Channel ch;
    ch.m_handle = handle;
    return ch;
}

bool Channel::valid() const { return m_handle != nullptr; }

void Channel::close()
{
    if (m_handle) {
        ::CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
}

bool Channel::write_all(const uint8_t* data, size_t size, int timeout_ms, std::string& error)
{
    if (!valid()) {
        error = "closed pipe";
        return false;
    }
    const auto deadline = deadline_from_timeout(timeout_ms);
    size_t     offset   = 0;
    while (offset < size) {
        const int wait_ms = remaining_ms(deadline);
        if (timeout_ms >= 0 && wait_ms == 0) {
            error = "IPC write timed out";
            return false;
        }
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) {
            error = "CreateEvent failed";
            return false;
        }
        DWORD written = 0;
        const BOOL ok = ::WriteFile(static_cast<HANDLE>(m_handle), data + offset,
                                    static_cast<DWORD>(size - offset), &written, &ov);
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                ::CloseHandle(ov.hEvent);
                if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) {
                    m_peer_closed = true;
                    error         = "closed pipe";
                } else {
                    error = "IPC write failed";
                }
                return false;
            }
            const DWORD wait = ::WaitForSingleObject(ov.hEvent, wait_ms < 0 ? INFINITE : static_cast<DWORD>(wait_ms));
            if (wait != WAIT_OBJECT_0) {
                ::CancelIoEx(static_cast<HANDLE>(m_handle), &ov);
                ::CloseHandle(ov.hEvent);
                error = wait == WAIT_TIMEOUT ? "IPC write timed out" : "IPC write failed";
                return false;
            }
            if (!::GetOverlappedResult(static_cast<HANDLE>(m_handle), &ov, &written, FALSE)) {
                ::CloseHandle(ov.hEvent);
                m_peer_closed = true;
                error         = "closed pipe";
                return false;
            }
        }
        ::CloseHandle(ov.hEvent);
        if (written == 0) {
            m_peer_closed = true;
            error         = "closed pipe";
            return false;
        }
        offset += written;
    }
    return true;
}

bool Channel::read_all(uint8_t* data, size_t size, int timeout_ms, std::string& error)
{
    if (!valid()) {
        error = "closed pipe";
        return false;
    }
    const auto deadline = deadline_from_timeout(timeout_ms);
    size_t     offset   = 0;
    while (offset < size) {
        const int wait_ms = remaining_ms(deadline);
        if (timeout_ms >= 0 && wait_ms == 0) {
            error = "truncated IPC message";
            return false;
        }
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ov.hEvent == nullptr) {
            error = "CreateEvent failed";
            return false;
        }
        DWORD got = 0;
        const BOOL ok = ::ReadFile(static_cast<HANDLE>(m_handle), data + offset,
                                   static_cast<DWORD>(size - offset), &got, &ov);
        if (!ok) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_IO_PENDING) {
                ::CloseHandle(ov.hEvent);
                if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
                    m_peer_closed = true;
                    error         = "closed pipe";
                } else {
                    error = "IPC read failed";
                }
                return false;
            }
            const DWORD wait = ::WaitForSingleObject(ov.hEvent, wait_ms < 0 ? INFINITE : static_cast<DWORD>(wait_ms));
            if (wait != WAIT_OBJECT_0) {
                ::CancelIoEx(static_cast<HANDLE>(m_handle), &ov);
                ::CloseHandle(ov.hEvent);
                error = wait == WAIT_TIMEOUT ? "truncated IPC message" : "IPC read failed";
                return false;
            }
            if (!::GetOverlappedResult(static_cast<HANDLE>(m_handle), &ov, &got, FALSE)) {
                ::CloseHandle(ov.hEvent);
                m_peer_closed = true;
                error         = "closed pipe";
                return false;
            }
        }
        ::CloseHandle(ov.hEvent);
        if (got == 0) {
            m_peer_closed = true;
            error         = "closed pipe";
            return false;
        }
        offset += got;
    }
    return true;
}

#else

Channel Channel::from_posix_fd(int fd)
{
    Channel ch;
    ch.m_fd = fd;
    return ch;
}

bool Channel::valid() const { return m_fd >= 0; }

void Channel::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool wait_fd(int fd, short events, int timeout_ms, std::string& error)
{
    pollfd p{};
    p.fd     = fd;
    p.events = events;
    int r;
    do {
        r = ::poll(&p, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r == 0) {
        error = (events & POLLIN) ? "truncated IPC message" : "IPC write timed out";
        return false;
    }
    if (r < 0) {
        error = "IPC poll failed";
        return false;
    }
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        error = "closed pipe";
        return false;
    }
    return true;
}

bool Channel::write_all(const uint8_t* data, size_t size, int timeout_ms, std::string& error)
{
    if (!valid()) {
        error = "closed pipe";
        return false;
    }
    const auto deadline = deadline_from_timeout(timeout_ms);
    size_t     offset   = 0;
    while (offset < size) {
        const int wait_ms = remaining_ms(deadline);
        if (timeout_ms >= 0 && wait_ms == 0) {
            error = "IPC write timed out";
            return false;
        }
        if (!wait_fd(m_fd, POLLOUT, wait_ms, error)) {
            if (error == "closed pipe")
                m_peer_closed = true;
            return false;
        }
        const ssize_t n = ::write(m_fd, data + offset, size - offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EPIPE || errno == ECONNRESET) {
                m_peer_closed = true;
                error         = "closed pipe";
                return false;
            }
            error = "IPC write failed";
            return false;
        }
        if (n == 0) {
            m_peer_closed = true;
            error         = "closed pipe";
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool Channel::read_all(uint8_t* data, size_t size, int timeout_ms, std::string& error)
{
    if (!valid()) {
        error = "closed pipe";
        return false;
    }
    const auto deadline = deadline_from_timeout(timeout_ms);
    size_t     offset   = 0;
    while (offset < size) {
        const int wait_ms = remaining_ms(deadline);
        if (timeout_ms >= 0 && wait_ms == 0) {
            error = "truncated IPC message";
            return false;
        }
        if (!wait_fd(m_fd, POLLIN, wait_ms, error)) {
            if (error == "closed pipe")
                m_peer_closed = true;
            return false;
        }
        const ssize_t n = ::read(m_fd, data + offset, size - offset);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EPIPE || errno == ECONNRESET) {
                m_peer_closed = true;
                error         = "closed pipe";
                return false;
            }
            error = "IPC read failed";
            return false;
        }
        if (n == 0) {
            m_peer_closed = true;
            error         = "closed pipe";
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

#endif

bool Channel::write_json(const nlohmann::json& message, int timeout_ms, std::string& error)
{
    std::vector<uint8_t> frame;
    if (!encode_message(message, frame, error))
        return false;
    return write_all(frame.data(), frame.size(), timeout_ms, error);
}

bool Channel::read_json(nlohmann::json& message, int timeout_ms, std::string& error)
{
    uint8_t header[4];
    if (!read_all(header, 4, timeout_ms, error))
        return false;
    const uint32_t size = read_le32(header);
    if (size > k_max_payload_bytes) {
        error = "oversized IPC payload";
        return false;
    }
    if (size == 0) {
        error = "empty IPC payload";
        return false;
    }
    std::vector<uint8_t> body(size);
    if (!read_all(body.data(), size, timeout_ms, error))
        return false;
    return decode_payload(body.data(), size, message, error);
}

} // namespace Slic3r::plugin_ipc
