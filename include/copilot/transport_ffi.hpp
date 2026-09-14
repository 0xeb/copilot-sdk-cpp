// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <copilot/transport.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace copilot
{

/// In-process transport backed by the official copilot_runtime_* C ABI.
class FfiTransport : public ITransport
{
  public:
    FfiTransport(
        std::string library_path,
        std::string cli_entrypoint,
        std::map<std::string, std::string> environment = {},
        std::vector<std::string> args = {});
    ~FfiTransport() override;

    FfiTransport(const FfiTransport&) = delete;
    FfiTransport& operator=(const FfiTransport&) = delete;

    size_t read(char* buffer, size_t size) override;
    void write(const char* data, size_t size) override;
    void close() override;
    bool is_open() const override;

    const std::string& library_path() const noexcept { return library_path_; }

    /// True once the runtime confirmed the connection closed and the library was
    /// unloaded. False after a close() where the runtime never quiesced -- the
    /// handle is then held for the life of the process on purpose, see close().
    bool is_released() const noexcept { return library_ == nullptr; }

    /// How long close() keeps asking the runtime to release the connection.
    ///
    /// close() runs from the destructor, so it cannot wait forever the way a
    /// background retry loop could. Past the budget it gives up and quarantines
    /// the library instead of unloading it. Exposed so tests can exercise the
    /// give-up path without a multi-second wait.
    void set_close_retry_policy(unsigned attempts, std::chrono::milliseconds delay) noexcept
    {
        close_retry_attempts_ = attempts;
        close_retry_delay_ = delay;
    }

  private:
    using OutboundCallback = void (*)(void*, const std::uint8_t*, std::size_t);
    using HostStart = std::uint32_t (*)(
        const std::uint8_t*, std::size_t, const std::uint8_t*, std::size_t);
    using HostShutdown = bool (*)(std::uint32_t);
    using ConnectionOpen = std::uint32_t (*)(
        std::uint32_t,
        OutboundCallback,
        void*,
        const std::uint8_t*,
        std::size_t,
        const std::uint8_t*,
        std::size_t,
        const std::uint8_t*,
        std::size_t);
    using ConnectionWrite = bool (*)(std::uint32_t, const std::uint8_t*, std::size_t);
    using ConnectionClose = bool (*)(std::uint32_t);

    static void outbound(void* user_data, const std::uint8_t* bytes, std::size_t length);
    void receive(const std::uint8_t* bytes, std::size_t length);
    void load();
    void start(
        const std::string& cli_entrypoint,
        const std::map<std::string, std::string>& environment,
        const std::vector<std::string>& args);
    void unload();
    void* symbol(const char* name);

    std::string library_path_;
    void* library_ = nullptr;
    HostStart host_start_ = nullptr;
    HostShutdown host_shutdown_ = nullptr;
    ConnectionOpen connection_open_ = nullptr;
    ConnectionWrite connection_write_ = nullptr;
    ConnectionClose connection_close_ = nullptr;
    std::uint32_t server_id_ = 0;
    std::uint32_t connection_id_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<char> inbound_;
    bool open_ = false;

    // 50 x 100ms = 5s, matching the .NET host's 100ms retry cadence but bounded,
    // because close() is reachable from the destructor.
    unsigned close_retry_attempts_ = 50;
    std::chrono::milliseconds close_retry_delay_{100};
};

} // namespace copilot
