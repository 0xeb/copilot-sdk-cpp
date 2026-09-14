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
class SessionFactoryApi;

json build_mcp_auth_response(
    const McpAuthHandler& handler,
    const json& request,
    const std::string& session_id);

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
    Session(const std::string& session_id, Client* client,
            const std::optional<std::string>& workspace_path = std::nullopt,
            SessionCapabilities capabilities = {},
            bool managed_settings_enabled = false);

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

    const SessionCapabilities& capabilities() const noexcept { return capabilities_; }
    void set_initial_state(
        std::optional<std::string> workspace_path,
        SessionCapabilities capabilities);
    void set_session_id(std::string session_id) { session_id_ = std::move(session_id); }

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

    /// Get all messages in the session
    /// @return Future that resolves to list of session events
    std::future<std::vector<SessionEvent>> get_messages();

    /// Send a message and wait until the session becomes idle.
    /// @param options Message options including prompt and attachments
    /// @param timeout Maximum time to wait (default: 60 seconds)
    /// @return Future that resolves to the final assistant message, or nullopt if none
    /// @throws std::runtime_error if timeout is reached or session error occurs
    std::future<std::optional<SessionEvent>> send_and_wait(
        MessageOptions options,
        std::chrono::seconds timeout = std::chrono::seconds(60));

    // =========================================================================
    // Event Handling
    // =========================================================================

    /// Subscribe to session events
    /// @param handler Function to call for each event
    /// @return Subscription handle (unsubscribes on destruction)
    Subscription on(EventHandler handler);

    /// Register an event handler that remains active for the session lifetime.
    void register_persistent_event_handler(EventHandler handler);

    /// Dispatch an event to all subscribers (called by Client)
    void dispatch_event(const SessionEvent& event);

    // =========================================================================
    // Tool Management
    // =========================================================================

    /// Register a tool for this session
    /// @param tool Tool definition with handler
    void register_tool(Tool tool);

    /// Register multiple tools
    /// @param tools List of tool definitions
    void register_tools(const std::vector<Tool>& tools);

    /// Get a registered tool by name
    /// @return Tool pointer or nullptr if not found
    const Tool* get_tool(const std::string& name) const;

    void register_canvases(const std::vector<std::shared_ptr<Canvas>>& canvases);
    json handle_canvas_request(const std::string& method, const json& params);
    void register_factories(
        const std::vector<std::shared_ptr<FactoryHandle>>& factories);
    json handle_factory_request(const std::string& method, const json& params);

    // =========================================================================
    // Permission Handling
    // =========================================================================

    /// Register a permission handler
    /// @param handler Function to call for permission requests
    void register_permission_handler(PermissionHandler handler);
    void register_permission_handler(PermissionHandlerWithContext handler);
    void register_mcp_auth_handler(McpAuthHandler handler);

    /// Handle a permission request (called by Client)
    PermissionRequestResult handle_permission_request(const PermissionRequest& request);

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

    /// Register a handler for elicitation requests
    void register_elicitation_handler(ElicitationHandler handler);

    /// Handle an elicitation request (called by Client)
    ElicitationResult handle_elicitation_request(const ElicitationContext& context);

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

    /// Destroy the session on the server
    /// @return Future that completes when destroyed
    std::future<void> destroy();

    /// Disconnect this SDK participant while leaving the session resumable.
    std::future<void> disconnect() { return destroy(); }

    /// Invoke any session-scoped RPC. sessionId is injected automatically.
    std::future<json> invoke(const std::string& method, json params = json::object());

    template <typename Result>
    std::future<Result> invoke_typed(
        const std::string& method,
        json params = json::object())
    {
        return std::async(
            std::launch::async,
            [future = invoke(method, std::move(params))]() mutable
            {
                return future.get().template get<Result>();
            }
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
    std::future<generated::api::SandboxEnforcementStatus>
    get_sandbox_enforcement_status();
    std::future<void> disable_remote();
    SessionFactoryApi factory();

    // =========================================================================
    // Model & Mode (v0.1.49 additions)
    // =========================================================================

    /// Options for set_model() mirroring upstream session.model.switchTo params.
    struct SetModelOptions
    {
        std::optional<ReasoningEffort> reasoning_effort;
    };

    /// Switch the session to a different model mid-conversation.
    /// Calls the v3 session.model.switchTo RPC.
    /// @param model_id Identifier of the target model
    /// @param options Optional reasoning effort override
    /// @return Future that completes when the switch is acknowledged
    std::future<void> set_model(const std::string& model_id, SetModelOptions options = {});

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

    std::string session_id_;
    Client* client_;
    std::optional<std::string> workspace_path_;
    SessionCapabilities capabilities_;
    bool managed_settings_enabled_ = false;

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
    std::map<std::string, std::shared_ptr<FactoryHandle>> factories_;

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
};

} // namespace copilot
