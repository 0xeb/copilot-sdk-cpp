// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <copilot/transport.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
// MSG_NOSIGNAL doesn't exist on macOS - use SO_NOSIGPIPE socket option instead
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <string>

namespace copilot
{

// =============================================================================
// Winsock initialization helper (Windows only)
// =============================================================================

#ifdef _WIN32
/// RAII wrapper for Winsock initialization
class WinsockInitializer
{
  public:
    static WinsockInitializer& instance()
    {
        static WinsockInitializer inst;
        return inst;
    }

    WinsockInitializer(const WinsockInitializer&) = delete;
    WinsockInitializer& operator=(const WinsockInitializer&) = delete;

  private:
    WinsockInitializer()
    {
        WSADATA wsa_data;
        int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        if (result != 0)
            throw TransportError("WSAStartup failed with error " + std::to_string(result));
    }

    ~WinsockInitializer()
    {
        WSACleanup();
    }
};
#endif

// =============================================================================
// TCP Transport
// =============================================================================

/// Transport that communicates over a TCP socket
class TcpTransport : public ITransport
{
  public:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
    using Socket = int;
    static constexpr Socket kInvalidSocket = -1;
#endif

    /// Construct an unconnected transport
    TcpTransport() : socket_(kInvalidSocket), open_(false)
    {
#ifdef _WIN32
        WinsockInitializer::instance();
#endif
    }

    /// Construct from an existing connected socket
    explicit TcpTransport(Socket socket) : socket_(socket), open_(socket != kInvalidSocket)
    {
#ifdef _WIN32
        WinsockInitializer::instance();
#endif
    }

    ~TcpTransport() override
    {
        close();
    }

    // Non-copyable
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    // Movable
    TcpTransport(TcpTransport&& other) noexcept : socket_(other.socket_), open_(other.open_.load())
    {
        other.socket_ = kInvalidSocket;
        other.open_ = false;
    }

    TcpTransport& operator=(TcpTransport&& other) noexcept
    {
        if (this != &other)
        {
            close();
            socket_ = other.socket_;
            open_ = other.open_.load();
            other.socket_ = kInvalidSocket;
            other.open_ = false;
        }
        return *this;
    }

    /// Connect to a host:port
    /// @param host Hostname or IP address
    /// @param port Port number
    /// @param timeout_ms Connection timeout in milliseconds (0 = no timeout)
    /// @throws TransportError on connection failure
    void connect(const std::string& host, int port, int timeout_ms = 30000);

    size_t read(char* buffer, size_t size) override;
    void write(const char* data, size_t size) override;
    void close() override;
    bool is_open() const override
    {
        return open_;
    }

    Socket socket() const
    {
        return socket_;
    }

  private:
    Socket socket_;
    std::atomic<bool> open_;

    static std::string get_socket_error();
    void set_socket_blocking(bool blocking);
    static void set_socket_blocking(Socket sock, bool blocking);
};

// =============================================================================
// Implementation
// =============================================================================

inline std::string TcpTransport::get_socket_error()
{
#ifdef _WIN32
    int error = WSAGetLastError();
    char* msg = nullptr;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        nullptr,
        error,
        0,
        reinterpret_cast<LPSTR>(&msg),
        0,
        nullptr
    );
    std::string result = msg ? msg : ("Error " + std::to_string(error));
    LocalFree(msg);
    return result;
#else
    return strerror(errno);
#endif
}

inline void TcpTransport::set_socket_blocking(bool blocking)
{
    set_socket_blocking(socket_, blocking);
}

inline void TcpTransport::set_socket_blocking(Socket sock, bool blocking)
{
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (blocking)
        fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    else
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

inline void TcpTransport::connect(const std::string& host, int port, int timeout_ms)
{
    // Close any existing connection
    close();

    // Resolve hostname
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);

    int status = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (status != 0)
    {
#ifdef _WIN32
        throw TransportError("getaddrinfo failed: " + std::to_string(status));
#else
        throw TransportError("getaddrinfo failed: " + std::string(gai_strerror(status)));
#endif
    }

    // Try each address until we connect.
    //
    // The timeout is a budget for the whole resolution list, not per address. A host such as
    // "localhost" commonly resolves to ::1 before 127.0.0.1; if the peer listens on IPv4 only,
    // the first candidate is refused and we must fall through to the next one promptly rather
    // than spending the caller's entire timeout on each dead address in turn.
    const auto connect_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 0);

    Socket sock = kInvalidSocket;
    for (auto* rp = result; rp != nullptr; rp = rp->ai_next)
    {
        sock = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == kInvalidSocket)
            continue;

        // Set non-blocking for timeout support
        if (timeout_ms > 0)
            set_socket_blocking(sock, false);

        int connect_result = ::connect(sock, rp->ai_addr, static_cast<int>(rp->ai_addrlen));

        if (connect_result == 0)
        {
            // Connected immediately
            break;
        }

#ifdef _WIN32
        bool would_block = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
        bool would_block = (errno == EINPROGRESS);
#endif

        if (would_block && timeout_ms > 0)
        {
            // Wait for connection, bounded by whatever remains of the overall budget.
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                connect_deadline - std::chrono::steady_clock::now());
            const auto remaining_us = remaining.count() > 0 ? remaining.count() : 0;

            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);

            // A refused connection is reported through exceptfds on Windows (writefds stays
            // clear), so it must be monitored or the select below waits out the full timeout
            // on an address that already failed.
            fd_set except_fds;
            FD_ZERO(&except_fds);
            FD_SET(sock, &except_fds);

            struct timeval tv;
            tv.tv_sec = static_cast<long>(remaining_us / 1000000);
            tv.tv_usec = static_cast<long>(remaining_us % 1000000);

            int select_result =
                select(static_cast<int>(sock) + 1, nullptr, &write_fds, &except_fds, &tv);

            if (select_result > 0)
            {
                // SO_ERROR is authoritative for both outcomes: it is 0 only when the
                // connection actually completed.
                int error = 0;
                socklen_t len = sizeof(error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);

                if (error == 0 && !FD_ISSET(sock, &except_fds))
                {
                    // Connected successfully
                    break;
                }
            }
        }

        // Connection failed, try next address
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
        sock = kInvalidSocket;
    }

    freeaddrinfo(result);

    if (sock == kInvalidSocket)
        throw TransportError("Failed to connect to " + host + ":" + std::to_string(port));

    // Restore blocking mode
    socket_ = sock;
    set_socket_blocking(true);

    // Disable Nagle's algorithm for lower latency
    int flag = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&flag), sizeof(flag));

#if defined(__APPLE__)
    // On macOS, use SO_NOSIGPIPE to prevent SIGPIPE on send to closed socket
    setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
#endif

    open_ = true;
}

inline size_t TcpTransport::read(char* buffer, size_t size)
{
    if (!open_)
        throw ConnectionClosedError();

#ifdef _WIN32
    int bytes_read = recv(socket_, buffer, static_cast<int>(size), 0);
    if (bytes_read == SOCKET_ERROR)
    {
        int error = WSAGetLastError();
        if (error == WSAECONNRESET || error == WSAECONNABORTED)
        {
            open_ = false;
            return 0;
        }
        throw TransportError("recv failed: " + get_socket_error());
    }
#else
    ssize_t bytes_read = recv(socket_, buffer, size, 0);
    if (bytes_read < 0)
    {
        if (errno == ECONNRESET || errno == EPIPE)
        {
            open_ = false;
            return 0;
        }
        throw TransportError("recv failed: " + get_socket_error());
    }
#endif

    if (bytes_read == 0)
        open_ = false;
    return static_cast<size_t>(bytes_read);
}

inline void TcpTransport::write(const char* data, size_t size)
{
    if (!open_)
        throw ConnectionClosedError();

    size_t total_sent = 0;
    while (total_sent < size)
    {
#ifdef _WIN32
        int to_send = static_cast<int>(std::min(size - total_sent, size_t{INT_MAX}));
        int bytes_sent = send(socket_, data + total_sent, to_send, 0);
        if (bytes_sent == SOCKET_ERROR)
            throw TransportError("send failed: " + get_socket_error());
#else
        ssize_t bytes_sent = send(socket_, data + total_sent, size - total_sent, MSG_NOSIGNAL);
        if (bytes_sent < 0)
            throw TransportError("send failed: " + get_socket_error());
#endif
        total_sent += static_cast<size_t>(bytes_sent);
    }
}

inline void TcpTransport::close()
{
    if (!open_.exchange(false))
        return;

    if (socket_ != kInvalidSocket)
    {
#ifdef _WIN32
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
#else
        shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
#endif
        socket_ = kInvalidSocket;
    }
}

} // namespace copilot
