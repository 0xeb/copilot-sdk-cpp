// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file client.hpp
/// @brief CopilotClient for managing connections to the Copilot CLI server

#include <atomic>
#include <copilot/decisions.hpp>
#include <copilot/events.hpp>
#include <copilot/host.hpp>
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

json build_tool_definition(const Tool& tool);

// Forward declaration
class Session;
class Subscription;
class CopilotRequestHandlerBridge;

// =============================================================================
// Request Builder Helpers (exposed for unit testing the wire shape)
// =============================================================================

/// Build the `session.create` params.
/// @param telemetry_forwarding Whether the client forwards GitHub telemetry.
/// @param token_provider_registration_id Registration id of a session token provider.
json build_session_create_request(
    const CreateSessionOptions& options,
    ClientMode mode = ClientMode::CodingAgent,
    bool telemetry_forwarding = false,
    const std::optional<std::string>& token_provider_registration_id = std::nullopt
);

/// Build the `session.resume` params.
/// @param permission_request Overrides `permissions.request` (the extension join path sends
///        false when no permission handler is supplied).
json build_session_resume_request(
    const std::string& session_id,
    const ResumeSessionOptions& options,
    ClientMode mode = ClientMode::CodingAgent,
    bool telemetry_forwarding = false,
    const std::optional<std::string>& token_provider_registration_id = std::nullopt,
    std::optional<bool> permission_request = std::nullopt
);

/// The `session.options.update` patch sent after create/resume, or nullopt when there is
/// nothing to send.
std::optional<json>
build_session_options_update(const CreateSessionOptions& options, ClientMode mode);
std::optional<json>
build_session_options_update(const ResumeSessionOptions& options, ClientMode mode);

/// CLI arguments for a spawned runtime, before Node / `cmd /c` wrapping.
std::vector<std::string> build_cli_command_args(const ClientOptions& options);

/// CLI arguments handed to an in-process runtime.
std::vector<std::string> build_in_process_args(const ClientOptions& options);

/// Environment additions for the runtime. When the connection has an explicit
/// `environment` it is the base; otherwise the result layers on the inherited one.
std::map<std::string, std::string> build_cli_environment(
    const ClientOptions& options,
    const std::optional<std::string>& effective_connection_token = std::nullopt
);

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
/// opts.runtime = RuntimeConnection::for_spawned_stdio();
///
/// Client client(opts);
/// client.start().get();
///
/// CreateSessionOptions session_options;
/// session_options.model = "gpt-4";
/// session_options.permissions.on_request = [](const PermissionRequest&) {
///     return PermissionRequestResult{.kind = "approve-once"};
/// };
/// auto session = client.create_session(std::move(session_options)).get();
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
    /// Kills the CLI process without graceful cleanup. Safe to call while stop() is
    /// running: it kills the runtime stop() is waiting on, so stop() completes promptly.
    void force_stop();

    /// Process id of the runtime this client spawned (also while stop() is waiting
    /// for it to exit), or nullopt when no spawned runtime is alive.
    std::optional<int> cli_process_id() const;

    /// Get the current connection state
    ConnectionState state() const;

    // =========================================================================
    // Session Management
    // =========================================================================

    /// Create a new Copilot session. Starts the client first if it is disconnected.
    std::future<std::shared_ptr<Session>> create_session(CreateSessionOptions options = {});

    /// Resume an existing session. With `interaction.auto_mode_recovery`, a unique id prefix
    /// (7+ hex chars) is resolved through `sessions.findByPrefix`.
    std::future<std::shared_ptr<Session>>
    resume_session(const std::string& session_id, ResumeSessionOptions options = {});

    /// Resume with an explicit `permissions.request` value (extension join path).
    std::future<std::shared_ptr<Session>> resume_session(
        const std::string& session_id,
        ResumeSessionOptions options,
        std::optional<bool> permission_request
    );

    /// List all available sessions
    /// @return Future that resolves to list of session metadata
    std::future<std::vector<SessionMetadata>> list_sessions();

    /// List sessions matching a filter (matches upstream nodejs SDK).
    /// @param filter Filter criteria (cwd / git_root / repository / branch)
    /// @return Future that resolves to list of matching session metadata
    std::future<std::vector<SessionMetadata>> list_sessions(SessionListFilter filter);

    /// Get metadata for a specific session by ID (O(1) lookup).
    /// @param session_id ID of the session
    /// @return Future that resolves to metadata, or nullopt if not found
    std::future<std::optional<SessionMetadata>> get_session_metadata(const std::string& session_id);

    /// Delete a session
    /// @param session_id ID of the session to delete
    /// @return Future that completes when deleted
    std::future<void> delete_session(const std::string& session_id);

    /// Force the runtime to drop its cached managed-settings snapshot.
    ///
    /// Calls managedSettings.clearCache. Useful when the host has changed managed
    /// settings out-of-band and wants the next read to re-resolve them rather than
    /// wait for the runtime's own cache expiry.
    std::future<void> clear_managed_settings_cache();

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

    /// Decision helpers: one-off yes/no, pick-one and rating questions answered by
    /// Copilot decision models. Requires the runtime's `DECISIONS_API` feature flag.
    /// @experimental
    // NODEJS REFERENCE: client.ts decisions
    CopilotDecisions decisions()
    {
        return CopilotDecisions(*this);
    }

    /// The TCP port the runtime is listening on, if using TCP transport.
    /// nullopt before start(), with stdio, parent-process or in-process transport,
    /// and once a runtime this client started has stopped or failed to start.
    // C# REFERENCE: Client.cs RuntimePort
    std::optional<int> runtime_port() const;

    /// Invoke a low-level global RPC method.
    std::future<json> invoke(const std::string& method, json params = json::object());

    /// Current W3C trace context for request propagation.
    json trace_context() const;

    /// Get the negotiated protocol version (set after successful start()).
    /// Returns std::nullopt before connection is established.
    std::optional<int> negotiated_protocol_version() const;

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
    // Agent Host Protocol (experimental)
    // =========================================================================

    /// Start an AHP host that exposes this client's sessions to remote AHP clients.
    /// Select local_server, github_environment, or both. `on_exit` runs at most once.
    /// @throws std::invalid_argument for an invalid authentication/endpoint selection.
    // NODEJS REFERENCE: client.ts startAhpHost
    std::future<AhpHost> start_ahp_host(AhpHostOptions options);

    // =========================================================================
    // Internal API (used by Session)
    // =========================================================================

    /// Get session by ID (internal use)
    std::shared_ptr<Session> get_session(const std::string& session_id);

    /// Get the JSON-RPC client (internal use)
    std::shared_ptr<JsonRpcClient> rpc_client_shared()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!rpc_)
            throw JsonRpcError(JsonRpcErrorCode::ConnectionClosed, "Client disconnected");
        return rpc_;
    }

  private:
    struct GitHubTokenRegistration
    {
        GitHubTokenProvider provider;
        std::optional<std::string> session_id;
        bool committed = false;
    };

    /// Start the client when disconnected; otherwise require a live connection.
    /// Connects if needed and returns a snapshot of the live connection, taken under the lock.
    std::shared_ptr<JsonRpcClient> ensure_connected();
    std::string register_github_token_provider(GitHubTokenProvider provider);
    void assign_github_token_registration(const std::string& id, const std::string& session_id);
    void commit_github_token_registration(const std::string& id);
    void remove_github_token_registration(const std::string& id);
    std::shared_ptr<Session>
    create_session_impl(CreateSessionOptions options);
    void setup_session_fs(Session& session, const SessionStateOptions& state) const;
    void setup_session_fs_or_retire(Session& session, const SessionStateOptions& state) const;
    std::shared_ptr<Session> resume_session_impl(
        std::string session_id, ResumeSessionOptions options, std::optional<bool> permission_request
    );
    std::string resolve_session_id_prefix(const std::string& session_id, const ResumeSessionOptions& options);
    /// Route an inbound session-scoped callback, preferring an in-flight resume whose
    /// requested ID (or abbreviated prefix) matches.
    std::shared_ptr<Session> resolve_session_callback(const std::string& session_id);
    void handle_disconnect();

    /// Start the CLI server process
    void start_cli_server();
    void start_in_process_runtime();

    /// Connect to the server (stdio or TCP)
    void connect_to_server();

    /// Verify protocol version matches
    void verify_protocol_version();

    /// Resolve CLI command (handle .js files and Windows PATH)
    std::pair<std::string, std::vector<std::string>>
    resolve_cli_command(const std::string& cli_path, const std::vector<std::string>& args);

    /// Handle incoming session events
    void handle_session_event(const std::string& method, const json& params);

    /// Handle incoming user input requests
    json handle_user_input_request(const json& params);

    json handle_exit_plan_mode_request(const json& params);
    json handle_auto_mode_switch_request(const json& params);

    /// Handle incoming `gitHubToken.getToken` requests
    json handle_github_token_request(const json& params);
    json handle_provider_token_request(const json& params);
    json handle_shell_authentication_request(const json& params);
    void cancel_shell_authentication_request(const json& params);
    json handle_memory_store_request(const json& params, const CancellationToken& cancellation);

    /// Handle incoming hook invocations
    json handle_hooks_invoke(const json& params);

    // Options
    ClientOptions options_;
    /// Resolved options_.runtime (never unset after construction).
    RuntimeConnection runtime_;
    std::string cli_path_;
    std::string tcp_host_ = "127.0.0.1";
    int tcp_port_ = 0;
    std::atomic<int> runtime_port_{0};
    void clear_owned_runtime_port();
    std::optional<std::string> effective_token_;

    // Connection state
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};
    mutable std::mutex mutex_;

    // Components
    std::unique_ptr<Process> process_;
    // Runtime that stop() has detached and is waiting on; force_stop() kills it.
    Process* stopping_process_ = nullptr;
    std::unique_ptr<ITransport> transport_;
    std::shared_ptr<JsonRpcClient> rpc_;
    bool session_admission_closed_ = true;
    std::shared_ptr<CopilotRequestHandlerBridge> request_handler_bridge_;

    // Sessions
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    struct PendingResume
    {
        std::shared_ptr<Session> session;
        std::shared_ptr<JsonRpcClient> connection;
        std::optional<std::string> registration_id;
        bool resolve_aliases = false;
    };
    /// Requested session ID -> in-flight resume, guarded by mutex_.
    std::map<std::string, PendingResume> pending_resumes_;

    // Models cache
    mutable std::mutex models_cache_mutex_;
    std::optional<std::vector<ModelInfo>> models_cache_;

    // Lifecycle handlers
    mutable std::mutex lifecycle_mutex_;
    std::vector<LifecycleHandler> lifecycle_handlers_;

    // Protocol version negotiation result (set after verify_protocol_version()).
    mutable std::mutex protocol_version_mutex_;
    std::optional<int> negotiated_protocol_version_;

    std::optional<std::string> server_version_;
    // Executable start_cli_server() launched (for ProtocolVersionMismatch).
    std::optional<std::string> spawned_cli_path_;

    mutable std::mutex registry_mutex_;
    std::map<std::string, GitHubTokenRegistration> github_token_registrations_;
    /// sessionId -> requestId -> cancellation for `shellAuthentication.request`.
    std::map<std::string, std::map<std::string, CancellationSource>> pending_shell_auth_;
    std::map<uint64_t, CancellationSource> pending_memory_stores_;
    uint64_t next_memory_store_id_ = 1;

    // AHP hosting (guarded by ahp_mutex_; never held while running user callbacks)
    struct AhpHostPolicy
    {
        AhpAuthenticationMode mode = AhpAuthenticationMode::GitHub;
        std::string require_identity;
    };
    struct AhpSessionFactories
    {
        std::function<std::shared_ptr<Session>(const AhpSessionCreateRequest&)> create_session;
        std::function<std::shared_ptr<Session>(const AhpSessionResumeRequest&)> resume_session;
        std::function<void(std::shared_ptr<Session>)> on_session_released;
        AhpHostPolicy policy;
    };
    struct AhpAuthenticatorEntry
    {
        std::string require_identity;
        AhpAuthenticator authenticate;
        uint64_t generation = 0;
    };
    struct AhpAuthenticationAttempt
    {
        std::string host_id;
        CancellationSource cancellation;
    };
    struct AhpHandoff
    {
        std::string host_id;
        std::string requested_session_id;
        CancellationSource cancellation;
        std::map<const Session*, json> configs;
        bool configs_closed = false;
        std::shared_ptr<Session> session;
        bool released = false;
        std::function<void(std::shared_ptr<Session>)> on_released;
    };

    void handle_host_exit(const AhpHostExit& exit);
    void handle_host_notification(const std::string& method, const json& params);
    void disconnect_hosts();
    void release_host_sessions(const std::string& host_id);
    void release_host_session(const std::string& handoff_id);
    json materialize_host_session(const json& params);
    json validate_host_authentication(const json& params, const CancellationToken& cancellation);
    void capture_host_session(
        const std::shared_ptr<Session>& session, json config, const std::optional<json>& applied
    );
    static void notify_host_session_released(
        std::shared_ptr<Session> session, std::function<void(std::shared_ptr<Session>)> on_released
    );

    mutable std::mutex ahp_mutex_;
    std::map<std::string, AhpAuthenticatorEntry> ahp_authenticators_;
    std::map<std::string, AhpSessionFactories> ahp_session_factories_;
    std::map<std::string, std::function<void(const AhpHostExit&)>> ahp_exit_callbacks_;
    std::map<std::string, std::shared_ptr<AhpAuthenticationAttempt>> ahp_authentications_;
    std::map<std::string, std::shared_ptr<AhpHandoff>> ahp_handoffs_;
    uint64_t next_ahp_authenticator_generation_ = 1;
};

} // namespace copilot
