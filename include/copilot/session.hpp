// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file session.hpp
/// @brief CopilotSession for managing conversation sessions

#include <copilot/events.hpp>
#include <copilot/generated/api_types.hpp>
#include <copilot/jsonrpc.hpp>
#include <copilot/types.hpp>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace copilot
{

// Forward declaration
class Client;
class Session;
class SessionWorkflowApi;

/// UI helpers backed by `session.ui.elicitation`. Every method throws when the
/// host has not advertised `capabilities.ui.elicitation`.
// NODEJS REFERENCE: nodejs/src/session.ts SessionUiApi
// C# REFERENCE: dotnet/src/Types.cs ISessionUiApi
class SessionUiApi
{
  public:
    explicit SessionUiApi(Session& session) : session_(session) {}

    /// Show a dialog for an arbitrary form schema.
    std::future<ElicitationResult> elicitation(ElicitationParams params);
    /// Yes/no confirmation. True only when the user accepted and confirmed.
    std::future<bool> confirm(std::string message);
    /// Pick one of `options`; nullopt when declined or cancelled.
    std::future<std::optional<std::string>>
    select(std::string message, std::vector<std::string> options);
    /// Free-text input; nullopt when declined or cancelled.
    std::future<std::optional<std::string>>
    input(std::string message, UiInputOptions options = {});

  private:
    Session& session_;
};

/// Peer-session management (`session.management.*`). @experimental — upstream marks the
/// feature experimental and off by default; shapes may change.
///
/// Enable it on each session that should be discoverable, then list live peers and
/// message them by full session id:
/// @code
/// session->management().configure({.enabled = true}).get();
/// for (const auto& peer : session->management().list().get())
///     if (peer.name == "reviewer")
///         session->management().message(peer.session_id, "please review").get();
/// @endcode
/// The receiver sees the text as a `system.notification` (see `peer_message_from_event`)
/// and an idle receiver starts a turn. `accepted` is not a reply.
// NODEJS REFERENCE: nodejs/src/generated/rpc.ts createSessionRpc management.*
// RUST REFERENCE: rust/tests/session_management_test.rs
class SessionManagementApi
{
  public:
    explicit SessionManagementApi(Session& session) : session_(session) {}

    /// Opt in/out, choose the discovery scope, or toggle individual tools. An empty
    /// options object reads the current policy. Never changes the creation-handler flag.
    std::future<SessionManagementPolicy> configure(SessionManagementConfigureOptions options = {});
    /// Live, opted-in peers visible to this session (never includes the caller).
    /// `cwd` filters by exact working directory.
    std::future<std::vector<ManagedSessionInfo>> list(std::optional<std::string> cwd = std::nullopt);
    /// Send `text` (non-empty, at most 16 KiB UTF-8) to a peer by full session id. Wakes an
    /// idle peer or queues behind a busy one. Never counts as human approval.
    std::future<SessionManagementMessageResult>
    message(std::string target_session_id, std::string text);
    /// Ask the runtime to create a new root session via this session's creation handler
    /// (`Session::set_session_creation_handler`). Returns the new session id.
    std::future<std::string> create(SessionManagementCreateOptions options);
    /// Close a session this session created. History is kept; returns whether it closed.
    std::future<bool> close(std::string target_session_id);

  private:
    Session& session_;
};

/// Session naming (`session.name.*`). Peers address each other by exact name in the
/// management model tools and `ManagedSessionInfo::name`.
// NODEJS REFERENCE: nodejs/src/generated/rpc.ts createSessionRpc name.*
class SessionNameApi
{
  public:
    explicit SessionNameApi(Session& session) : session_(session) {}

    /// Current name, or nullopt when none is set.
    std::future<std::optional<std::string>> get();
    /// Set an explicit name (1–100 characters).
    std::future<void> set(std::string name);
    /// Offer an automatic name derived from `summary`; returns whether it was applied
    /// (an explicit name is never overwritten).
    std::future<bool> set_auto(std::string summary);

  private:
    Session& session_;
};

/// Peer message carried by a `system.notification` event, or nullopt for any other event.
std::optional<PeerMessage> peer_message_from_event(const SessionEvent& event);

/// Runs an MCP auth handler and maps its result to the mcp.oauth.handlePendingRequest payload.
// NODEJS REFERENCE: nodejs/src/session.ts _executeMcpAuthAndRespond
json build_mcp_auth_response(
    const McpAuthHandler& handler, const McpAuthRequest& request, const std::string& session_id
);

// =============================================================================
// Subscription - RAII subscription handle
// =============================================================================

/// RAII handle for event subscriptions
/// Automatically unsubscribes when destroyed
class Subscription
{
  public:
    Subscription() = default;
    Subscription(std::function<void()> unsubscribe) : unsubscribe_(std::move(unsubscribe)) {}
    ~Subscription()
    {
        if (unsubscribe_)
            unsubscribe_();
    }

    // Move-only
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept : unsubscribe_(std::move(other.unsubscribe_))
    {
        other.unsubscribe_ = nullptr;
    }
    Subscription& operator=(Subscription&& other) noexcept
    {
        if (this != &other)
        {
            if (unsubscribe_)
                unsubscribe_();
            unsubscribe_ = std::move(other.unsubscribe_);
            other.unsubscribe_ = nullptr;
        }
        return *this;
    }

    /// Unsubscribe manually
    void unsubscribe()
    {
        if (unsubscribe_)
        {
            unsubscribe_();
            unsubscribe_ = nullptr;
        }
    }

  private:
    std::function<void()> unsubscribe_;
};

// =============================================================================
// Session - Copilot conversation session
// =============================================================================

/// A Copilot conversation session
///
/// Sessions maintain conversation state, handle events, and manage tool execution.
///
/// Example usage:
/// @code
/// auto session = client.create_session(config).get();
///
/// // Subscribe to events
/// auto sub = session->on([](const SessionEvent& evt) {
///     if (evt.type == SessionEventType::AssistantMessage) {
///         auto* data = evt.try_as<AssistantMessageData>();
///         if (data) std::cout << data->content << std::endl;
///     }
/// });
///
/// // Send a message
/// session->send(MessageOptions{.prompt = "Hello!"}).get();
/// @endcode
class Session : public std::enable_shared_from_this<Session>
{
  public:
    /// Event handler function type
    using EventHandler = std::function<void(const SessionEvent&)>;

    /// Permission handler function type
    using PermissionHandler = std::function<PermissionRequestResult(const PermissionRequest&)>;
    using PermissionHandlerWithContext = copilot::PermissionHandlerWithContext;

    /// Create a session (called by Client)
    Session(
        const std::string& session_id,
        Client* client,
        const std::optional<std::string>& workspace_path = std::nullopt,
        SessionCapabilities capabilities = {},
        bool managed_settings_enabled = false
    );

    ~Session();

    // Non-copyable, movable
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // =========================================================================
    // Session Properties
    // =========================================================================

    /// Get the session ID
    const std::string& session_id() const
    {
        return session_id_;
    }

    /// Get the workspace path for infinite sessions.
    ///
    /// Contains checkpoints/, plan.md, and files/ subdirectories.
    /// Returns nullopt if infinite sessions are disabled.
    const std::optional<std::string>& workspace_path() const
    {
        return workspace_path_;
    }

    SessionCapabilities capabilities() const
    {
        std::lock_guard<std::mutex> lock(capabilities_mutex_);
        return capabilities_;
    }
    void set_capabilities(SessionCapabilities capabilities)
    {
        std::lock_guard<std::mutex> lock(capabilities_mutex_);
        capabilities_ = std::move(capabilities);
    }
    void
    set_initial_state(std::optional<std::string> workspace_path, SessionCapabilities capabilities);
    void set_session_id(std::string session_id)
    {
        session_id_ = std::move(session_id);
    }

    // =========================================================================
    // Messaging
    // =========================================================================

    /// Send a message to the session
    /// @param options Message options including prompt and attachments
    /// @return Future that resolves to the message ID
    std::future<std::string> send(MessageOptions options);

    /// Abort the current message processing
    /// @return Future that completes when aborted
    std::future<void> abort();

    /// Get every event in the session history (`session.getMessages`).
    /// @return Future that resolves to list of session events
    // NODEJS REFERENCE: nodejs/src/session.ts getEvents
    std::future<std::vector<SessionEvent>> get_events();

    /// Get the session history as raw JSON, exactly as the runtime sent it.
    /// Unknown fields, explicit nulls and large integers survive untouched, so the
    /// result can be forwarded to RPCs such as taskAnalytics.classifyTrajectory.
    /// NODEJS REFERENCE: nodejs/src/session.ts getRawEvents
    std::future<std::vector<json>> get_raw_events();

    /// Send a message and wait until the session becomes idle.
    /// @param options Message options including prompt and attachments
    /// @param timeout Maximum time to wait (default: 60 seconds)
    /// @return Future that resolves to the final assistant message, or nullopt if none
    /// @throws std::runtime_error if timeout is reached or session error occurs
    std::future<std::optional<SessionEvent>>
    send_and_wait(MessageOptions options, std::chrono::seconds timeout = std::chrono::seconds(60));

    // =========================================================================
    // Event Handling
    // =========================================================================

    /// Subscribe to session events
    /// @param handler Function to call for each event
    /// @return Subscription handle (unsubscribes on destruction)
    /// @throws std::runtime_error if session event dispatch has stopped
    Subscription on(EventHandler handler);
    Subscription on(EventHandler handler, bool root_agent_only);

    /// Register an event handler that remains active for the session lifetime.
    void register_persistent_event_handler(EventHandler handler);

    /// Dispatch an event to all subscribers (called by Client)
    /// Dispatch an event. A `settled` event was a request already answered by another
    /// client before this session registered: observers see it, but no handler runs.
    void dispatch_event(const SessionEvent& event, bool settled = false);

    // =========================================================================
    // Tool Management
    // =========================================================================

    /// Register a tool for this session
    /// @param tool Tool definition with handler
    void register_tool(Tool tool);

    /// Register multiple tools
    /// @param tools List of tool definitions
    void register_tools(const std::vector<Tool>& tools);
    std::future<void> set_tools(std::vector<Tool> tools);

    /// Get a registered tool by name
    /// @return Tool pointer or nullptr if not found
    const Tool* get_tool(const std::string& name) const;
    std::optional<Tool> copy_tool(const std::string& name) const;

    void register_canvases(const std::vector<std::shared_ptr<Canvas>>& canvases);
    json handle_canvas_request(const std::string& method, const json& params);
    void register_workflows(const std::vector<std::shared_ptr<WorkflowHandle>>& workflows);
    json handle_workflow_request(const std::string& method, const json& params);

    // =========================================================================
    // Permission Handling
    // =========================================================================

    /// Register a permission handler
    /// @param handler Function to call for permission requests
    void register_permission_handler(PermissionHandler handler);
    void register_permission_handler(PermissionHandlerWithContext handler);
    void register_mcp_auth_handler(McpAuthHandler handler);
    void register_skill_provider(std::shared_ptr<SkillProvider> provider);
    json handle_skill_provider_request(
        const std::string& method, const json& params, const CancellationToken& cancellation = {}
    );

    /// Slash commands dispatched from `command.execute` broadcasts.
    void register_commands(const std::vector<CommandDefinition>& commands);
    /// Bearer-token callbacks keyed by provider name (`providerToken.getToken`).
    void register_bearer_token_providers(std::map<std::string, BearerTokenProvider> providers);
    json handle_provider_token_request(const std::string& provider_name);
    void register_shell_authentication_handler(ShellAuthenticationHandler handler);
    /// Answer a `shellAuthentication.request`. Never throws; failures become `{action:"cancel"}`.
    json handle_shell_authentication_request(
        const ShellAuthenticationRequest& request, const CancellationToken& cancellation
    );
    void register_memory_store(MemoryStoreHandler handler);
    /// Answer a `sessionManagement.create` request with the registered creation handler.
    // NODEJS REFERENCE: nodejs/src/session.ts _handleSessionManagementCreate
    json handle_session_management_create(const json& request, const CancellationToken& cancellation);

    /// Install the experimental `hostPolicy.preCheck` callback.
    // NODEJS REFERENCE: nodejs/src/session.ts _registerHostPolicyPreCheck
    void register_host_policy_pre_check(HostPolicyPreCheckHandler handler);
    /// Answer a `hostPolicy.preCheck` request. Retiring the session cancels the callback.
    // NODEJS REFERENCE: nodejs/src/session.ts _handleHostPolicyPreCheck
    json handle_host_policy_pre_check(const json& params, const CancellationToken& cancellation);

    /// Section transform callbacks keyed by section id (`systemMessage.transform`).
    void register_transform_callbacks(std::map<std::string, SectionTransformFn> callbacks);
    /// Answer a `systemMessage.transform` request. Sections without a callback, or
    /// whose callback throws, are returned unchanged.
    // NODEJS REFERENCE: nodejs/src/session.ts _handleSystemMessageTransform
    json handle_system_message_transform(const json& sections);

    /// Install the `sessionFs.*` request handler (built from the session's provider).
    void register_session_fs_handler(SessionFsHandler handler);
    /// The installed `sessionFs.*` handler, or an empty function when none is set.
    SessionFsHandler session_fs_handler() const;
    void handle_memory_store_request(const json& params, const CancellationToken& cancellation);

    /// Runs once when the session is retired.
    void set_on_disconnected(std::function<void()> callback);
    bool is_disconnected() const noexcept
    {
        return dispatch_stopped_.load();
    }
    /// Set once the runtime has answered this session's create/resume; stop() detaches
    /// only confirmed sessions.
    void mark_confirmed() noexcept
    {
        confirmed_.store(true);
    }
    bool is_confirmed() const noexcept
    {
        return confirmed_.load();
    }

    /// Mark the session disconnected: stop dispatch, cancel pending work, drop callbacks.
    void retire();
    std::optional<TranscriptRecoveryReport> transcript_recovery() const
    {
        return transcript_recovery_;
    }
    void set_transcript_recovery(std::optional<TranscriptRecoveryReport> value)
    {
        transcript_recovery_ = std::move(value);
    }

    /// Permission mode the runtime applied from the create/resume `permissionMode`
    /// option, or nullopt when none was requested or the runtime did not report it.
    // NODEJS REFERENCE: nodejs/src/session.ts permissionModeResult
    std::optional<generated::api::PermissionsSetModeResult> permission_mode_result() const
    {
        std::lock_guard<std::mutex> lock(resume_state_mutex_);
        return permission_mode_result_;
    }
    void set_permission_mode_result(std::optional<generated::api::PermissionsSetModeResult> value)
    {
        std::lock_guard<std::mutex> lock(resume_state_mutex_);
        permission_mode_result_ = std::move(value);
    }

    /// Canvas instances currently open in this session. Seeded from the resume
    /// response and kept current by `session.canvas.opened`/`closed` events.
    /// Returns a copy.
    // NODEJS REFERENCE: nodejs/src/session.ts openCanvases
    std::vector<generated::api::OpenCanvasInstance> open_canvases() const
    {
        std::lock_guard<std::mutex> lock(resume_state_mutex_);
        return open_canvases_;
    }
    void set_open_canvases(std::vector<generated::api::OpenCanvasInstance> instances)
    {
        std::lock_guard<std::mutex> lock(resume_state_mutex_);
        open_canvases_ = std::move(instances);
    }
    std::future<json> send_and_wait_structured(
        MessageOptions options, std::chrono::seconds timeout = std::chrono::seconds(60)
    );

    /// Handle a permission request (called by Client)
    PermissionRequestResult handle_permission_request(
        const PermissionRequest& request,
        std::optional<json> prompt_request = std::nullopt,
        std::optional<json> assisted_approval_assessment = std::nullopt,
        std::optional<std::string> program_tool_call_id = std::nullopt,
        std::optional<std::string> request_id = std::nullopt
    );

    // =========================================================================
    // User Input Handling
    // =========================================================================

    /// Register a handler for user input requests from the agent
    /// @param handler Function to call for user input requests
    void register_user_input_handler(UserInputHandler handler);

    /// Handle a user input request (called by Client)
    UserInputResponse handle_user_input_request(const UserInputRequest& request);

    // =========================================================================
    // Elicitation Handling
    // =========================================================================

    /// Register a handler for elicitation requests (`elicitation.requested` broadcasts)
    void register_elicitation_handler(ElicitationHandler handler);

    // =========================================================================
    // Exit Plan Mode Handling
    // =========================================================================

    /// Register a handler for exit-plan-mode requests
    void register_exit_plan_mode_handler(ExitPlanModeHandler handler);

    /// Handle an exit-plan-mode request (called by Client)
    ExitPlanModeResult handle_exit_plan_mode_request(const ExitPlanModeRequest& request);

    // =========================================================================
    // Auto Mode Switch Handling
    // =========================================================================

    /// Register a handler for auto-mode-switch requests
    void register_auto_mode_switch_handler(AutoModeSwitchHandler handler);

    /// Handle an auto-mode-switch request (called by Client)
    AutoModeSwitchResponse handle_auto_mode_switch_request(const AutoModeSwitchRequest& request);

    // =========================================================================
    // Hooks
    // =========================================================================

    /// Register hook handlers for this session
    /// @param hooks Hook handlers configuration
    void register_hooks(SessionHooks hooks);

    /// Handle a hook invocation from the server (called by Client)
    /// @param hook_type The type of hook to invoke
    /// @param input The hook input data as JSON
    /// @return Hook output as JSON, or null JSON if no handler
    json handle_hooks_invoke(const std::string& hook_type, const json& input);

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /// Stop answering host tool invocations on this session.
    ///
    /// A handler already running is user code on a background task and cannot be
    /// interrupted; this ensures that when it returns, its result is dropped
    /// instead of being sent as `session.tools.handlePendingToolCall` over a
    /// connection that is closing. Called by Client during stop, mirroring
    /// upstream github/copilot-sdk 53e74d81.
    void cancel_pending_tool_invocations() noexcept;

    /// Host tool invocations dispatched but not yet answered. Exposed so the
    /// cancellation contract is observable from a test.
    int pending_tool_invocations() const noexcept;

    /// Outcome of detach(). Mirrors upstream's SessionDetachResponse.
    struct DetachResult
    {
        /// Whether the runtime released the session.
        bool success = false;
        /// Runtime-supplied reason when `success` is false.
        std::optional<std::string> error;
        /// Full response, so fields not surfaced above stay reachable.
        json raw;
    };

    /// Detach the session from this connection.
    ///
    /// Calls `session.detach`, which replaced `session.destroy` as the SDK
    /// teardown RPC upstream (github/copilot-sdk fcffcdf0). A `success: false`
    /// response is reported here rather than thrown: teardown should not fail
    /// because the runtime declined to release a session.
    std::future<DetachResult> detach();

    /// Destroy the session on the server.
    ///
    /// Retained for source compatibility and implemented in terms of detach().
    /// A soft failure (`success: false`) is **discarded** here because the
    /// signature cannot carry it — call detach() directly if you need to know.
    /// Transport and RPC errors still propagate.
    /// @return Future that completes when the session has been detached
    std::future<void> destroy();

    /// Disconnect this SDK participant while leaving the session resumable.
    std::future<void> disconnect()
    {
        return destroy();
    }

    /// Invoke any session-scoped RPC. sessionId is injected automatically.
    std::future<json> invoke(const std::string& method, json params = json::object());

    template <typename Result>
    std::future<Result> invoke_typed(const std::string& method, json params = json::object())
    {
        return std::async(
            std::launch::async,
            [future = invoke(method, std::move(params))]() mutable
            { return future.get().template get<Result>(); }
        );
    }

    std::future<generated::api::McpServerList> list_mcp_servers();
    std::future<generated::api::AuthIdentity> get_current_auth_info();
    std::future<generated::api::AgentList> list_agents(json params = json::object());
    std::future<generated::api::SkillList> list_skills();
    std::future<generated::api::TaskList> list_tasks();
    std::future<generated::api::CommandList> list_commands(json params = json::object());
    std::future<generated::api::HistoryListRewindPointsResult> list_rewind_points();
    std::future<generated::api::UsageGetMetricsResult> get_usage_metrics();
    std::future<generated::api::RemoteEnableResult> enable_remote(json params = json::object());
    /// Sandbox enforcement status for this session
    /// (`session.sandbox.getEnforcementStatus`).
    std::future<generated::api::SandboxEnforcementStatus> get_sandbox_enforcement_status();
    std::future<void> disable_remote();
    SessionWorkflowApi workflow();

    /// Elicitation-backed UI helpers (confirm/select/input).
    SessionUiApi ui();

    /// Peer-session management (`session.management.*`). @experimental.
    SessionManagementApi management();

    /// Session naming (`session.name.*`).
    SessionNameApi name();

    /// Register (or clear, with an empty function) the host's session-creation callback
    /// and report it with `session.management.configure{hasCreateHandler}`. This does not
    /// enable the management tool group; enable it separately via
    /// `management().configure({.enabled = true})`. The previous handler is restored
    /// when the RPC fails.
    // NODEJS REFERENCE: nodejs/src/session.ts setSessionCreationHandler
    std::future<generated::api::SessionManagementPolicy>
    set_session_creation_handler(SessionCreationHandler handler);

    /// Write a message to the session timeline (`session.log`).
    // NODEJS REFERENCE: nodejs/src/session.ts log
    std::future<void> log(std::string message, LogOptions options = {});

    // =========================================================================
    // Model & Mode (v0.1.49 additions)
    // =========================================================================

    /// Options for set_model() mirroring upstream session.model.switchTo params.
    // C# REFERENCE: dotnet/src/Types.cs SetModelOptions
    struct SetModelOptions;

    /// Switch the session to a different model mid-conversation.
    /// Calls the v3 session.model.switchTo RPC.
    /// @param model_id Identifier of the target model
    /// @param options Optional reasoning effort override
    /// @return Future that completes when the switch is acknowledged
    std::future<void> set_model(const std::string& model_id, SetModelOptions options);
    // An overload rather than `= {}`: GCC and Clang reject a default argument that
    // needs the default member initializers of a nested class of the enclosing class.
    std::future<void> set_model(const std::string& model_id);

    /// Get the currently selected model for the session.
    /// Calls the v3 session.model.getCurrent RPC.
    /// @return Future resolving to the model identifier (or nullopt if none)
    std::future<std::optional<std::string>> get_current_model();

    /// Routing preference used when the session model is `auto`.
    ///
    /// `Fast` is an integrator-only latency preset, not a first-party GitHub
    /// Copilot product preference.
    enum class AutoTier
    {
        Efficiency,
        Balance,
        Intelligence,
        Fast,
    };

    /// Outcome of set_auto_tier(). Mirrors ModelSwitchAutoTierResult.
    struct AutoTierResult
    {
        /// Immediate request status; "pending" means accepted but not committed.
        std::string status;
        /// Preference currently committed for the session, when reported.
        std::optional<AutoTier> effective_auto_tier;
        /// Latest unclaimed preference, when one is queued.
        std::optional<AutoTier> pending_auto_tier;
        /// Full result payload, so fields not surfaced above stay reachable.
        json raw;
    };

    /// Set the Auto routing preference for this session.
    ///
    /// Calls session.model.switchAutoTier. The change is not immediate: it
    /// activates when a future user turn on the `auto` model mints a replacement
    /// model/token pair, which is why the result carries a status and a pending
    /// preference rather than just succeeding.
    ///
    /// @param auto_tier Preference to activate, or nullopt to return to
    ///                  provider-default Auto routing (upstream's ResetAutoTier).
    std::future<AutoTierResult> set_auto_tier(std::optional<AutoTier> auto_tier);

    /// Copilot Fusion tier, as named by the CLI's `--fusion-tier`.
    enum class FusionTier
    {
        Standard,
        High,
        Max,
    };

    /// Change the Copilot Fusion tier without changing the selected model.
    ///
    /// Calls session.model.switchFusionTier. Like set_auto_tier(), the change takes
    /// effect on a later user turn that uses the `fusion` or `auto` model; call
    /// set_auto_tier(std::nullopt) to return to provider-default routing.
    /// NODEJS REFERENCE: nodejs/src/session.ts setFusionTier
    std::future<AutoTierResult> set_fusion_tier(FusionTier fusion_tier);

    struct SetModelOptions
    {
        std::optional<ReasoningEffort> reasoning_effort;
        /// "auto" | "concise" | "detailed" | "none"
        std::optional<std::string> reasoning_summary;
        /// "default" | "long_context"
        std::optional<std::string> context_tier;
        std::optional<ModelCapabilitiesOverride> model_capabilities;
        /// Routing preference staged with selecting `auto`. Mutually exclusive with
        /// reset_auto_tier and fusion_tier.
        std::optional<AutoTier> auto_tier;
        /// Return to provider-default Auto routing as part of the switch.
        bool reset_auto_tier = false;
        /// Fusion tier staged with selecting `fusion` or `auto`.
        std::optional<FusionTier> fusion_tier;
    };

    /// Agent interaction mode. Matches upstream nodejs SessionMode union.
    enum class Mode
    {
        Interactive,
        Plan,
        Autopilot,
    };

    /// Set the agent interaction mode.
    /// Calls the v3 session.mode.set RPC.
    /// @param mode New mode to apply
    /// @return Future that completes when the mode change is acknowledged
    std::future<void> set_mode(Mode mode);

    /// Get the current agent interaction mode.
    /// Calls the v3 session.mode.get RPC.
    /// @return Future resolving to the current mode
    std::future<Mode> get_mode();

  private:
    void enqueue_background(std::function<void()> task);
    void execute_external_tool(
        ExternalToolRequestedData request, std::optional<std::string> agent_id, Tool tool
    );
    bool claim_external_tool(const std::string& request_id);
    void merge_capabilities(const json& changed);
    void handle_broadcast_event(const SessionEvent& event);
    void notify_event_handlers(const SessionEvent& event);

    std::string session_id_;
    Client* client_;
    std::future<std::optional<SessionEvent>>
    wait_for_response(MessageOptions options, std::chrono::seconds timeout, bool structured);
    std::atomic<bool> dispatch_stopped_{false};
    std::atomic<bool> confirmed_{false};
    std::map<int, std::function<void()>> retirement_handlers_;
    std::optional<TranscriptRecoveryReport> transcript_recovery_;
    mutable std::mutex resume_state_mutex_;
    std::vector<generated::api::OpenCanvasInstance> open_canvases_;
    std::optional<generated::api::PermissionsSetModeResult> permission_mode_result_;
    std::mutex set_tools_mutex_;
    mutable std::mutex callbacks_mutex_;
    std::shared_ptr<SkillProvider> skill_provider_;
    std::optional<std::string> workspace_path_;
    mutable std::mutex capabilities_mutex_;
    SessionCapabilities capabilities_;
    bool managed_settings_enabled_ = false;
    std::map<std::string, CommandHandler> commands_;
    std::map<std::string, BearerTokenProvider> bearer_token_providers_;
    ShellAuthenticationHandler shell_authentication_handler_;
    MemoryStoreHandler memory_store_;
    SessionCreationHandler session_creation_handler_;
    std::map<uint64_t, CancellationSource> pending_session_creations_;
    uint64_t next_session_creation_id_ = 1;
    uint64_t session_creation_generation_ = 0;
    HostPolicyPreCheckHandler host_policy_pre_check_;
    std::map<uint64_t, CancellationSource> pending_host_policy_pre_checks_;
    uint64_t next_host_policy_pre_check_id_ = 1;
    std::map<std::string, SectionTransformFn> transform_callbacks_;
    SessionFsHandler session_fs_handler_;
    std::function<void()> on_disconnected_;

    std::mutex pending_external_tools_mutex_;
    std::map<std::string, CancellationSource> pending_external_tools_;

    // Event handlers
    mutable std::mutex handlers_mutex_;
    std::vector<std::pair<int, EventHandler>> event_handlers_;
    int next_handler_id_ = 0;
    std::mutex owned_event_subscriptions_mutex_;
    std::vector<Subscription> owned_event_subscriptions_;

    // Tools
    mutable std::mutex tools_mutex_;
    std::map<std::string, Tool> tools_;
    std::map<std::string, std::shared_ptr<Canvas>> canvases_;
    std::map<std::string, std::shared_ptr<WorkflowHandle>> workflows_;

    // Permission handler
    PermissionHandler permission_handler_;
    PermissionHandlerWithContext permission_handler_with_context_;
    McpAuthHandler mcp_auth_handler_;

    // User input handler
    std::mutex user_input_mutex_;
    UserInputHandler user_input_handler_;

    // Elicitation handler
    std::mutex elicitation_mutex_;
    ElicitationHandler elicitation_handler_;

    // Exit plan mode handler
    std::mutex exit_plan_mode_mutex_;
    ExitPlanModeHandler exit_plan_mode_handler_;

    // Auto mode switch handler
    std::mutex auto_mode_switch_mutex_;
    AutoModeSwitchHandler auto_mode_switch_handler_;

    // Hooks
    std::mutex hooks_mutex_;
    std::optional<SessionHooks> hooks_;

    std::mutex background_tasks_mutex_;
    std::vector<std::future<void>> background_tasks_;

    /// Set when the connection is going away. A host tool handler already running
    /// cannot be interrupted -- it is user code on a detached task -- but once it
    /// returns we must not answer into a connection that is being torn down.
    std::atomic<bool> tool_invocations_cancelled_{false};
    /// Host tool invocations dispatched but not yet answered.
    std::atomic<int> pending_tool_invocations_{0};
};

} // namespace copilot
