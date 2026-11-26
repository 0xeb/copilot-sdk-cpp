// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file client.hpp
/// @brief CopilotClient for managing connections to the Copilot CLI server

#include <atomic>
#include <condition_variable>
#include <copilot/cancellation.hpp>
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
#include <set>
#include <optional>
#include <regex>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
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

namespace detail
{
/// Points in Client::start() reported to the start-phase hook.
enum class StartPhase
{
    /// The runtime process was spawned and is not yet visible to stop()/force_stop().
    RuntimeSpawned,
};
/// Test-only: called on the starting thread at each StartPhase (empty to clear). Not part
/// of the supported API.
void set_start_phase_hook(std::function<void(StartPhase)> hook);
} // namespace detail

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
    /// Only one start runs at a time: a call made while a start is in progress (including
    /// the auto-start of ping(), create_session() and the other calls) joins it instead of
    /// starting another. The returned future never blocks in its destructor.
    /// @return Future that completes when connected
    /// @throws std::runtime_error on connection failure
    /// @throws StartCancelled when stop(), force_stop() or abort() ends the start first.
    ///         A cancelled start leaves the client Disconnected and reusable.
    std::future<void> start();

    /// Start the client; a stop request on `token` cancels the start the same way stop()
    /// does (the future fails with StartCancelled and any spawned runtime is killed).
    /// If another caller's start is already in progress, this call joins it and `token`
    /// ends only this caller's wait (the future fails with StartCancelled; the shared start
    /// continues). The token has no effect once the start has completed.
    // C# REFERENCE: Client.cs StartAsync(CancellationToken) / StartConnectionAsync
    std::future<void> start(std::stop_token token);

    /// Stop the client gracefully
    /// Detaches all sessions and closes the connection. Cancels a start in progress.
    /// Session detaches share one 10 s deadline, so a hung runtime cannot stall stop()
    /// once per session.
    /// @return Future that completes with any errors encountered during cleanup
    std::future<std::vector<StopError>> stop();

    /// Stop the client gracefully, but no later than `deadline`. Work still pending at the
    /// deadline (session detaches, runtime.shutdown, the exit wait) is abandoned and an
    /// owned runtime is killed, as by force_stop(). Not in the official SDKs.
    std::future<std::vector<StopError>> stop(std::chrono::steady_clock::time_point deadline);

    /// Force stop the client immediately
    /// Kills the CLI process without graceful cleanup. Cancels a start in progress. Safe
    /// to call while stop() is running: it fails the requests stop() is waiting on and
    /// kills the runtime, so stop() completes promptly.
    void force_stop();

    /// Drop the connection without waiting for anything: cancels a start in progress,
    /// fails every pending request with JsonRpcErrorCode::ConnectionClosed, cancels the
    /// tokens of in-flight handlers, retires all sessions and leaves the client
    /// Disconnected. An owned runtime is not killed (closing its stdin normally ends it);
    /// the next start() or force_stop() reaps it. Not in the official SDKs.
    void abort();

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

    /// create_session() that a stop request on `token` ends early. The future then fails at
    /// once with RequestCancelledError (JsonRpcErrorCode::RequestCancelled) and the runtime is
    /// sent `$/cancelRequest`. If the runtime had already created the session, the error
    /// carries its id and the session is deleted in the background (bounded by
    /// request_timeout; stop(), force_stop() and ~Client end the cleanup).
    /// Cancelling also ends the wait for a start in progress, but not the start itself.
    // C# REFERENCE: Client.cs CreateSessionAsync(config, CancellationToken)
    std::future<std::shared_ptr<Session>>
    create_session(CreateSessionOptions options, std::stop_token token);

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

    /// resume_session() that a stop request on `token` ends early, as create_session(options,
    /// token). A session the runtime already resumed is detached in the background.
    // C# REFERENCE: Client.cs ResumeSessionAsync(sessionId, config, CancellationToken)
    std::future<std::shared_ptr<Session>> resume_session(
        const std::string& session_id, ResumeSessionOptions options, std::stop_token token
    );

    /// List all available sessions
    /// @return Future that resolves to list of session metadata
    std::future<std::vector<SessionMetadata>> list_sessions();

    /// List sessions matching a filter (matches upstream nodejs SDK).
    /// @param filter Filter criteria (cwd / git_root / repository / branch)
    /// @return Future that resolves to list of matching session metadata
    std::future<std::vector<SessionMetadata>> list_sessions(SessionListFilter filter);
    std::future<std::vector<SessionMetadata>>
    list_sessions(SessionListFilter filter, std::stop_token token);

    /// Get metadata for a specific session by ID (O(1) lookup).
    /// @param session_id ID of the session
    /// @return Future that resolves to metadata, or nullopt if not found
    std::future<std::optional<SessionMetadata>> get_session_metadata(const std::string& session_id);
    std::future<std::optional<SessionMetadata>>
    get_session_metadata(const std::string& session_id, std::stop_token token);

    /// Delete a session
    /// @param session_id ID of the session to delete
    /// @return Future that completes when deleted
    std::future<void> delete_session(const std::string& session_id);
    std::future<void> delete_session(const std::string& session_id, std::stop_token token);

    /// Force the runtime to drop its cached managed-settings snapshot.
    ///
    /// Calls managedSettings.clearCache. Useful when the host has changed managed
    /// settings out-of-band and wants the next read to re-resolve them rather than
    /// wait for the runtime's own cache expiry.
    std::future<void> clear_managed_settings_cache();

    /// Get the ID of the most recently used session
    /// @return Future that resolves to session ID or nullopt if none
    std::future<std::optional<std::string>> get_last_session_id();
    std::future<std::optional<std::string>> get_last_session_id(std::stop_token token);

    // =========================================================================
    // Server Communication
    // =========================================================================

    /// Send a ping to verify connection health
    /// @param message Optional message to echo back
    /// @return Future that resolves to ping response
    std::future<PingResponse> ping(std::optional<std::string> message = std::nullopt);

    /// ping() that a stop request on `token` ends early with RequestCancelledError. A bounded
    /// health check: cancel the token after the budget, then force_stop() a runtime that did
    /// not answer.
    // C# REFERENCE: Client.cs PingAsync(message, CancellationToken)
    std::future<PingResponse> ping(std::optional<std::string> message, std::stop_token token);

    /// Get CLI status including version and protocol information
    /// @return Future that resolves to status response
    std::future<GetStatusResponse> get_status();
    std::future<GetStatusResponse> get_status(std::stop_token token);

    /// Get current authentication status
    /// @return Future that resolves to auth status response
    std::future<GetAuthStatusResponse> get_auth_status();
    // C# REFERENCE: Client.cs GetAuthStatusAsync(CancellationToken)
    std::future<GetAuthStatusResponse> get_auth_status(std::stop_token token);

    /// List available models with their metadata
    /// @return Future that resolves to list of model info
    /// @throws Error if not authenticated
    std::future<std::vector<ModelInfo>> list_models();
    std::future<std::vector<ModelInfo>> list_models(std::stop_token token);

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
    std::future<json> invoke(const std::string& method, json params, std::stop_token token);

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
    std::future<std::optional<std::string>> get_foreground_session_id(std::stop_token token);

    /// Set the foreground session
    std::future<void> set_foreground_session_id(const std::string& session_id);
    std::future<void>
    set_foreground_session_id(const std::string& session_id, std::stop_token token);

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

    struct NoDiscard
    {
        template <typename T>
        void operator()(T&&) const noexcept
        {
        }
    };

    /// Internal (used by Session): run `work(CancellationToken)` on its own thread, bounded
    /// by `stop`. With a stoppable token the returned future is fulfilled as soon as `work`
    /// finishes or `stop` is requested, whichever comes first, so neither get() nor the
    /// future's destructor waits on cancelled work. The token passed to `work` is cancelled
    /// with `stop`, which fails its requests promptly; the worker then ends on its own and
    /// ~Client waits for it. `cancelled_error` builds the error a cancel reports (default
    /// RequestCancelledError); `discard` receives a result that arrived after the cancel.
    template <typename R, typename Work, typename Discard = NoDiscard>
    std::future<R> run_cancellable(
        std::stop_token stop,
        Work work,
        std::function<std::exception_ptr()> cancelled_error = {},
        Discard discard = {}
    )
    {
        if (!stop.stop_possible())
            return std::async(
                std::launch::async,
                [work = std::move(work)]() mutable { return work(CancellationToken{}); }
            );

        struct State
        {
            std::mutex mutex;
            bool settled = false;
            std::promise<R> promise;
            CancellationSource cancellation;
        };
        auto state = std::make_shared<State>();
        auto future = state->promise.get_future();
        auto settle = [](State& s, auto&& action)
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (s.settled)
                return false;
            s.settled = true;
            action(s.promise);
            return true;
        };

        auto workers = cancellable_workers_;
        {
            std::lock_guard<std::mutex> lock(workers->mutex);
            ++workers->active;
        }
        auto callback = std::make_shared<std::stop_callback<std::function<void()>>>(
            stop,
            std::function<void()>(
                [state, settle, cancelled_error]
                {
                    state->cancellation.cancel();
                    settle(
                        *state,
                        [&](std::promise<R>& promise)
                        {
                            promise.set_exception(
                                cancelled_error ? cancelled_error()
                                                : std::make_exception_ptr(RequestCancelledError())
                            );
                        }
                    );
                }
            )
        );
        try
        {
            std::thread(
                [state, settle, callback, workers, work = std::move(work), discard = std::move(discard)](
                ) mutable
                {
                    try
                    {
                        if constexpr (std::is_void_v<R>)
                        {
                            work(state->cancellation.token());
                            settle(*state, [](std::promise<R>& promise) { promise.set_value(); });
                        }
                        else
                        {
                            R value = work(state->cancellation.token());
                            bool delivered = false;
                            settle(
                                *state,
                                [&](std::promise<R>& promise)
                                {
                                    promise.set_value(value);
                                    delivered = true;
                                }
                            );
                            if (!delivered)
                                discard(value);
                        }
                    }
                    catch (...)
                    {
                        auto error = std::current_exception();
                        settle(*state, [&](std::promise<R>& promise) { promise.set_exception(error); });
                    }
                    callback.reset();
                    std::lock_guard<std::mutex> lock(workers->mutex);
                    if (--workers->active == 0)
                        workers->idle.notify_all();
                }
            ).detach();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(workers->mutex);
            if (--workers->active == 0)
                workers->idle.notify_all();
            throw;
        }
        return future;
    }

    /// Get the JSON-RPC client (internal use). Never waits on a start in progress.
    std::shared_ptr<JsonRpcClient> rpc_client_shared()
    {
        std::lock_guard<std::mutex> lock(lifetime_mutex_);
        if (!live_rpc_)
            throw JsonRpcError(JsonRpcErrorCode::ConnectionClosed, "Client disconnected");
        return live_rpc_;
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
    /// A cancelled `cancellation` stops waiting for the start (the start itself continues)
    /// and throws RequestCancelledError.
    std::shared_ptr<JsonRpcClient> ensure_connected(const CancellationToken& cancellation = {});

    /// Background cleanup of sessions whose create/resume was cancelled after the runtime
    /// answered; stop(), force_stop() and ~Client end it.
    void cleanup_cancelled_session(
        std::shared_ptr<JsonRpcClient> connection, std::string method, std::string session_id
    );
    void cancel_session_cleanups();
    /// Ends a session whose create/resume finished after its caller's token was cancelled.
    void discard_cancelled_session(const std::shared_ptr<Session>& session, const char* method);
    /// Cancellable calls still running; ~Client waits for them.
    struct CancellableWorkers
    {
        std::mutex mutex;
        std::condition_variable idle;
        int active = 0;
    };
    std::shared_ptr<CancellableWorkers> cancellable_workers_ = std::make_shared<CancellableWorkers>();
    /// The session id a create/resume has had confirmed by the runtime, for the error a
    /// cancelled call reports.
    struct AnsweredSession
    {
        std::mutex mutex;
        std::optional<std::string> id;
    };
    std::mutex cleanup_mutex_;
    CancellationSource cleanup_cancellation_;
    struct CleanupThread
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<CleanupThread> cleanup_threads_;
    std::string register_github_token_provider(GitHubTokenProvider provider);
    void assign_github_token_registration(const std::string& id, const std::string& session_id);
    void commit_github_token_registration(const std::string& id);
    void remove_github_token_registration(const std::string& id);
    std::shared_ptr<Session> create_session_impl(
        CreateSessionOptions options,
        const CancellationToken& cancellation = {},
        AnsweredSession* answered = nullptr
    );
    void setup_session_fs(Session& session, const SessionStateOptions& state) const;
    void setup_session_fs_or_retire(Session& session, const SessionStateOptions& state) const;
    std::shared_ptr<Session> resume_session_impl(
        std::string session_id,
        ResumeSessionOptions options,
        std::optional<bool> permission_request,
        const CancellationToken& cancellation = {},
        AnsweredSession* answered = nullptr
    );
    std::string resolve_session_id_prefix(
        const std::string& session_id,
        const ResumeSessionOptions& options,
        const CancellationToken& cancellation = {}
    );
    /// Route an inbound session-scoped callback, preferring an in-flight resume whose
    /// requested ID (or abbreviated prefix) matches.
    std::shared_ptr<Session> resolve_session_callback(const std::string& session_id);
    std::shared_ptr<Session> resolve_skill_provider_session(const std::string& session_id);
    std::shared_ptr<Session> resolve_host_policy_pre_check_session(const json& params);
    /// `source` is the connection that closed; a close of a connection the client has
    /// already replaced is ignored. nullptr always tears down.
    void handle_disconnect(const JsonRpcClient* source = nullptr);

    /// Resources one start() call has acquired, published so stop(), force_stop(),
    /// abort() and a start stop_token can end the start without waiting on mutex_.
    struct StartAttempt
    {
        std::mutex mutex;
        std::atomic<bool> cancelled{false};
        /// Cancelled with the attempt; bounds waits that have nothing to kill (TCP connect).
        CancellationSource cancellation;
        bool finished = false;
        Process* process = nullptr;
        std::shared_ptr<JsonRpcClient> rpc;
        /// Cancels the start; returns false when it had already finished.
        bool cancel();
        /// Throws StartCancelled once cancel() has run.
        void throw_if_cancelled() const;
        void publish(Process* value);
        void publish(std::shared_ptr<JsonRpcClient> value);
        /// Ends the attempt successfully; throws StartCancelled if it was cancelled.
        void finish();
        /// Forgets the published resources before they are destroyed.
        void release();
    };
    void run_start(const std::shared_ptr<StartAttempt>& attempt, std::stop_token token);

    /// One caller waiting on the shared start.
    struct StartWaiter
    {
        std::promise<void> promise;
        std::atomic<bool> settled{false};
        /// Keeps `wait_token` alive when it comes from a std::stop_token.
        std::unique_ptr<detail::StopTokenCancellation> stop_wait;
        /// Ends this caller's wait (not the start); unregistered on destruction.
        CancellationToken wait_token;
        uint64_t wait_callback = 0;
        ~StartWaiter();
        /// Completes the caller's future once; later calls do nothing.
        void settle(std::exception_ptr error);
    };
    /// The start every caller joins while it runs (C# _connectionTask, TS startPromise).
    struct SharedStart
    {
        std::shared_ptr<StartAttempt> attempt;
        std::vector<std::shared_ptr<StartWaiter>> waiters;
    };
    /// Begins a start or joins the one in progress. `token` cancels a start this call
    /// begins; `wait` ends only this caller's wait, failing it with `wait_cancelled`.
    std::future<void> acquire_start(
        std::stop_token token,
        std::unique_ptr<detail::StopTokenCancellation> stop_wait,
        CancellationToken wait,
        std::exception_ptr wait_cancelled
    );
    void run_shared_start(const std::shared_ptr<SharedStart>& shared, std::stop_token token);
    void start_impl(StartAttempt& attempt);
    std::vector<StopError> stop_impl(std::chrono::steady_clock::time_point deadline);
    /// Ends every start requested so far (and, with `end_session_initializations`, every
    /// create/resume waiting on the runtime). Never takes mutex_.
    void cancel_startup(bool end_session_initializations);
    CancellationToken session_initialization_token() const;

    /// Start the CLI server process
    void start_cli_server(StartAttempt& attempt);
    void start_in_process_runtime();

    /// Connect to the server (stdio or TCP)
    void connect_to_server(StartAttempt* attempt = nullptr);

    /// Verify protocol version matches
    void verify_protocol_version();

    /// Spawn command for the CLI: `display` is the resolved CLI path that is reported
    /// (ProtocolVersionMismatch::cli_path), which differs from `executable` for scripts.
    struct CliCommand
    {
        std::string executable;
        std::vector<std::string> args;
        std::string display;
        bool verbatim_arguments = false;
    };
    /// Resolve CLI command (.js through node; a bare name through PATH, preferring a
    /// native executable and running a .cmd/.bat shim through cmd)
    CliCommand resolve_cli_command(
        const std::string& cli_path,
        const std::vector<std::string>& args,
        const std::map<std::string, std::string>& environment
    );

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
    // Connection stop() is shutting down; force_stop() and abort() fail its requests.
    std::shared_ptr<JsonRpcClient> stopping_rpc_;
    // An owned runtime that did not exit within stop()'s shutdown timeout, or that abort()
    // left running, with the connection whose read loop still waits on it. It is no
    // longer the client's process, but force_stop() and start() still kill it.
    // NODEJS REFERENCE: client.ts unexitedCliProcess
    struct UnexitedRuntime
    {
        std::shared_ptr<JsonRpcClient> rpc;
        std::unique_ptr<Process> process;
    };
    std::vector<UnexitedRuntime> unexited_runtimes_;
    void kill_unexited_process();
    /// Kills each runtime and stops its connection once it has exited; returns the ones
    /// that outlived `timeout`.
    static std::vector<UnexitedRuntime>
    reap_runtimes(std::vector<UnexitedRuntime> runtimes, std::chrono::milliseconds timeout);
    std::unique_ptr<ITransport> transport_;
    std::shared_ptr<JsonRpcClient> rpc_;

    // Guards the members below. Lock order: mutex_ -> lifetime_mutex_ ->
    // StartAttempt::mutex; nothing holding lifetime_mutex_ waits on another thread.
    mutable std::mutex lifetime_mutex_;
    /// rpc_ as seen by rpc_client_shared() and ensure_connected().
    std::shared_ptr<JsonRpcClient> live_rpc_;
    /// Starts requested and not yet finished, including ones still waiting on mutex_.
    std::vector<std::shared_ptr<StartAttempt>> start_attempts_;
    /// The start in progress that new callers join; reset when it finishes.
    std::shared_ptr<SharedStart> shared_start_;
    /// stop() calls in progress; a start requested meanwhile is refused.
    std::atomic<int> stops_in_progress_{0};
    // NODEJS REFERENCE: client.ts sessionInitializationLifetime
    CancellationSource session_initialization_;
    void set_live_rpc(std::shared_ptr<JsonRpcClient> rpc);
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
        /// Owner of skill-provider callbacks until the resume settles: the new session
        /// when the resume supplies a provider, otherwise the resident it replaces.
        std::shared_ptr<Session> skill_provider_session;
        /// The resident session, which keeps any host-policy pre-check request for a
        /// revision other than the one this resume declares.
        std::shared_ptr<Session> host_policy_pre_check_session;
        std::optional<std::int64_t> host_policy_pre_check_revision;
    };
    /// Requested session ID -> in-flight resume, guarded by mutex_.
    std::map<std::string, PendingResume> pending_resumes_;

    // Events for session IDs not registered yet (NODEJS REFERENCE: client.ts
    // heldSessionEvents). Only the read thread adds events; held_mutex_ is never
    // taken while mutex_ is held.
    struct HeldSessionEvent
    {
        SessionEvent event;
        std::optional<std::string> request_id;
    };
    std::mutex held_mutex_;
    /// Session ID -> held events, in first-seen order.
    std::vector<std::pair<std::string, std::vector<HeldSessionEvent>>> held_session_events_;
    std::size_t held_session_event_count_ = 0;
    /// Session IDs whose held events are being delivered; live events for them queue behind.
    std::set<std::string> delivering_session_ids_;
    void hold_session_event(const std::string& session_id, HeldSessionEvent held);
    void deliver_held_session_events();
    void discard_held_session_events(const std::string& session_id);

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
