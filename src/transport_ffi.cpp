// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/transport_ffi.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace copilot
{

namespace
{

std::vector<std::uint8_t> bytes(const nlohmann::json& value)
{
    const auto text = value.dump();
    return {text.begin(), text.end()};
}

} // namespace

FfiTransport::FfiTransport(
    std::string library_path,
    std::string cli_entrypoint,
    std::map<std::string, std::string> environment,
    std::vector<std::string> args)
    : library_path_(std::move(library_path))
{
    load();
    try
    {
        start(cli_entrypoint, environment, args);
    }
    catch (...)
    {
        close();
        throw;
    }
}

FfiTransport::~FfiTransport()
{
    close();
}

void FfiTransport::load()
{
#ifdef _WIN32
    library_ = LoadLibraryW(std::filesystem::path(library_path_).c_str());
    if (!library_)
        throw TransportError("Failed to load FFI runtime library: " + library_path_);
#else
    library_ = dlopen(library_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library_)
        throw TransportError(
            "Failed to load FFI runtime library: " + std::string(dlerror()));
#endif
    host_start_ = reinterpret_cast<HostStart>(symbol("copilot_runtime_host_start"));
    host_shutdown_ =
        reinterpret_cast<HostShutdown>(symbol("copilot_runtime_host_shutdown"));
    connection_open_ =
        reinterpret_cast<ConnectionOpen>(symbol("copilot_runtime_connection_open"));
    connection_write_ =
        reinterpret_cast<ConnectionWrite>(symbol("copilot_runtime_connection_write"));
    connection_close_ =
        reinterpret_cast<ConnectionClose>(symbol("copilot_runtime_connection_close"));
}

void* FfiTransport::symbol(const char* name)
{
#ifdef _WIN32
    auto value = reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(library_), name));
#else
    auto value = dlsym(library_, name);
#endif
    if (!value)
        throw TransportError("Missing FFI runtime symbol: " + std::string(name));
    return value;
}

void FfiTransport::start(
    const std::string& cli_entrypoint,
    const std::map<std::string, std::string>& environment,
    const std::vector<std::string>& args)
{
    nlohmann::json argv = nlohmann::json::array();
    if (cli_entrypoint.size() >= 3 &&
        cli_entrypoint.substr(cli_entrypoint.size() - 3) == ".js")
    {
        argv.push_back("node");
    }
    argv.push_back(cli_entrypoint);
    argv.push_back("--embedded-host");
    argv.push_back("--no-auto-update");
    for (const auto& arg : args)
        argv.push_back(arg);

    const auto argv_bytes = bytes(argv);
    const auto env_bytes = environment.empty()
                               ? std::vector<std::uint8_t>{}
                               : bytes(nlohmann::json(environment));
    server_id_ = host_start_(
        argv_bytes.data(),
        argv_bytes.size(),
        env_bytes.empty() ? nullptr : env_bytes.data(),
        env_bytes.size());
    if (!server_id_)
        throw TransportError("copilot_runtime_host_start failed");

    connection_id_ = connection_open_(
        server_id_,
        &FfiTransport::outbound,
        this,
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
        0);
    if (!connection_id_)
        throw TransportError("copilot_runtime_connection_open failed");

    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
}

void FfiTransport::outbound(
    void* user_data, const std::uint8_t* bytes_ptr, std::size_t length)
{
    if (user_data)
        static_cast<FfiTransport*>(user_data)->receive(bytes_ptr, length);
}

void FfiTransport::receive(const std::uint8_t* bytes_ptr, std::size_t length)
{
    if (!bytes_ptr || length == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_)
            return;
        inbound_.insert(
            inbound_.end(),
            reinterpret_cast<const char*>(bytes_ptr),
            reinterpret_cast<const char*>(bytes_ptr) + length);
    }
    cv_.notify_all();
}

size_t FfiTransport::read(char* buffer, size_t size)
{
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return !inbound_.empty() || !open_; });
    if (inbound_.empty())
        return 0;
    const auto count = std::min(size, inbound_.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        buffer[i] = inbound_.front();
        inbound_.pop_front();
    }
    return count;
}

void FfiTransport::write(const char* data, size_t size)
{
    std::uint32_t connection_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_)
            throw ConnectionClosedError();
        connection_id = connection_id_;
    }
    if (!connection_write_(
            connection_id,
            reinterpret_cast<const std::uint8_t*>(data),
            size))
    {
        throw TransportError("copilot_runtime_connection_write failed");
    }
}

void FfiTransport::close()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_ && !library_)
            return;
        open_ = false;
    }
    cv_.notify_all();

    if (connection_id_ && connection_close_)
    {
        connection_close_(connection_id_);
        connection_id_ = 0;
    }
    if (server_id_ && host_shutdown_)
    {
        host_shutdown_(server_id_);
        server_id_ = 0;
    }
    unload();
}

bool FfiTransport::is_open() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

void FfiTransport::unload()
{
    if (!library_)
        return;
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(library_));
#else
    dlclose(library_);
#endif
    library_ = nullptr;
}

} // namespace copilot
