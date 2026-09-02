// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <copilot/transport.hpp>
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
};

} // namespace copilot
