// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file client.hpp
/// @brief CopilotClient for managing connections to the Copilot CLI server

#include <atomic>
#include <copilot/events.hpp>
#include <copilot/jsonrpc.hpp>
#include <copilot/process.hpp>
#include <copilot/transport.hpp>
#include <copilot/transport_stdio.hpp>
#include <copilot/transport_tcp.hpp>
#include <copilot/types.hpp>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace copilot
{

// Forward declaration
class Session;
class Subscription;

// =============================================================================
// Request Builder Helpers (for unit testing request JSON shape)
// =============================================================================

/// Build the JSON request for session.create RPC
/// @param config Session configuration
/// @return JSON object ready to send to server
json build_session_create_request(const SessionConfig& config);

/// Build the JSON request for session.resume RPC
/// @param session_id ID of the session to resume
/// @param config Resume configuration
/// @return JSON object ready to send to server
json build_session_resume_request(const std::string& session_id, const ResumeSessionConfig& config);

// =============================================================================
// CopilotClient - Main client class
// =============================================================================

/// Client for interacting with the Copilot CLI server
///
/// The CopilotClient manages the connection to the Copilot CLI server and
/// provides methods to create and manage conversation sessions.
///
/// Example usage:
/// @code
/// ClientOptions opts;
/// opts.log_level = LogLevel::Debug;
///
/// Client client(opts);
/// client.start().get();
///
/// auto session = client.create_session(SessionConfig{.model = "gpt-4"}).get();
/// // Use session...
///
/// client.stop().get();
/// @endcode
class Client
{
  public:
    /// Create a new Copilot client with the given options
    /// @param options Configuration options for the client
    /// @throws std::invalid_argument if mutually exclusive options are provided
    explicit Client(ClientOptions options = {});

    /// Destructor - stops the client if running
    ~Client();

    // Non-copyable, non-movable (owns unique resources)
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(Client&&) = delete;

    // =========================================================================
    // Connection Management
    // =========================================================================

    /// Start the client and connect to the server
    /// @return Future that completes when connected
    /// @throws std::runtime_error on connection failure
    std::future<void> start();

    /// Stop the client gracefully
    /// Destroys all sessions and closes the connection
    /// @return Future that completes with any errors encountered during cleanup
    std::future<std::vector<StopError>> stop();

    /// Force stop the client immediately
    /// Kills the CLI process without graceful cleanup
    void force_stop();

    /// Get the current connection state
    ConnectionState state() const;

    // =========================================================================
    // Session Management
    // =========================================================================

    /// Create a new Copilot session
    /// @param config Session configuration
    /// @return Future that resolves to the created session
    std::future<std::shared_ptr<Session>> create_session(SessionConfig config = {});

    /// Resume an existing session
    /// @param session_id ID of the session to resume
    /// @param config Resume configuration
    /// @return Future that resolves to the resumed session
    std::future<std::shared_ptr<Session>>
    resume_session(const std::string& session_id, ResumeSessionConfig config = {});

    /// List all available sessions
    /// @return Future that resolves to list of session metadata
    std::future<std::vector<SessionMetadata>> list_sessions();

    /// Delete a session
    /// @param session_id ID of the session to delete
    /// @return Future that completes when deleted
    std::future<void> delete_session(const std::string& session_id);

    /// Get the ID of the most recently used session
    /// @return Future that resolves to session ID or nullopt if none
    std::future<std::optional<std::string>> get_last_session_id();

    // =========================================================================
    // Server Communication
    // =========================================================================

    /// Send a ping to verify connection health
    /// @param message Optional message to echo back
    /// @return Future that resolves to ping response
    std::future<PingResponse> ping(std::optional<std::string> message = std::nullopt);

    /// Get CLI status including version and protocol information
    /// @return Future that resolves to status response
    std::future<GetStatusResponse> get_status();

    /// Get current authentication status
    /// @return Future that resolves to auth status response
    std::future<GetAuthStatusResponse> get_auth_status();

    /// List available models with their metadata
    /// @return Future that resolves to list of model info
    /// @throws Error if not authenticated
    std::future<std::vector<ModelInfo>> list_models();

    // =========================================================================
    // Lifecycle Events
    // =========================================================================

    /// Subscribe to session lifecycle events (created, deleted, updated, foreground, background)
    using LifecycleHandler = std::function<void(const SessionLifecycleEvent&)>;
    Subscription on_lifecycle(LifecycleHandler handler);

    // =========================================================================
    // Foreground Session
    // =========================================================================

    /// Get the current foreground session ID
    std::future<std::optional<std::string>> get_foreground_session_id();

    /// Set the foreground session
    std::future<void> set_foreground_session_id(const std::string& session_id);

    // =========================================================================
    // Internal API (used by Session)
    // =========================================================================

    /// Get session by ID (internal use)
    std::shared_ptr<Session> get_session(const std::string& session_id);

    /// Get the JSON-RPC client (internal use)
    JsonRpcClient* rpc_client()
    {
        return rpc_.get();
    }

  private:
    /// Start the CLI server process
    void start_cli_server();

    /// Connect to the server (stdio or TCP)
    void connect_to_server();

    /// Verify protocol version matches
    void verify_protocol_version();

    /// Parse CLI URL into host and port
    void parse_cli_url(const std::string& url);

    /// Resolve CLI command (handle .js files and Windows PATH)
    std::pair<std::string, std::vector<std::string>>
    resolve_cli_command(const std::string& cli_path, const std::vector<std::string>& args);

    /// Handle incoming session events
    void handle_session_event(const std::string& method, const json& params);

    /// Handle incoming tool calls
    json handle_tool_call(const json& params);

    /// Handle incoming permission requests
    json handle_permission_request(const json& params);

    /// Handle incoming user input requests
    json handle_user_input_request(const json& params);

    /// Handle incoming hook invocations
    json handle_hooks_invoke(const json& params);

    // Options
    ClientOptions options_;
    std::optional<std::string> parsed_host_;
    std::optional<int> parsed_port_;

    // Connection state
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    mutable std::mutex mutex_;

    // Components
    std::unique_ptr<Process> process_;
    std::unique_ptr<ITransport> transport_;
    std::unique_ptr<JsonRpcClient> rpc_;

    // Sessions
    std::map<std::string, std::shared_ptr<Session>> sessions_;

    // Models cache
    mutable std::mutex models_cache_mutex_;
    std::optional<std::vector<ModelInfo>> models_cache_;

    // Lifecycle handlers
    mutable std::mutex lifecycle_mutex_;
    std::vector<LifecycleHandler> lifecycle_handlers_;
};

} // namespace copilot
