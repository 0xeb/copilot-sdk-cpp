// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/client.hpp>
#include <copilot/canvas.hpp>
#include <copilot/factory.hpp>
#include <copilot/rpc_methods.hpp>
#include <copilot/session.hpp>
#include <condition_variable>
#include <thread>

namespace copilot
{

json build_mcp_auth_response(
    const McpAuthHandler& handler,
    const json& request,
    const std::string& session_id)
{
    try
    {
        if (handler)
        {
            if (auto token = handler(request, session_id))
            {
                json result{
                    {"kind", "token"},
                    {"accessToken", token->access_token},
                };
                if (token->token_type)
                    result["tokenType"] = *token->token_type;
                if (token->expires_in)
                    result["expiresIn"] = *token->expires_in;
                return result;
            }
        }
    }
    catch (...)
    {
    }
    return json{{"kind", "cancelled"}};
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

Session::Session(const std::string& session_id, Client* client,
                 const std::optional<std::string>& workspace_path,
                 SessionCapabilities capabilities,
                 bool managed_settings_enabled)
    : session_id_(session_id),
      client_(client),
      workspace_path_(workspace_path),
      capabilities_(std::move(capabilities)),
      managed_settings_enabled_(managed_settings_enabled)
{
}

Session::~Session()
{
    std::vector<std::future<void>> tasks;
    {
        std::lock_guard<std::mutex> lock(background_tasks_mutex_);
        tasks.swap(background_tasks_);
    }

    for (auto& task : tasks)
        if (task.valid())
            task.wait();
}

void Session::enqueue_background(std::function<void()> task)
{
    auto future = std::async(std::launch::async, std::move(task));
    std::lock_guard<std::mutex> lock(background_tasks_mutex_);
    background_tasks_.push_back(std::move(future));
}

void Session::set_initial_state(
    std::optional<std::string> workspace_path,
    SessionCapabilities capabilities)
{
    workspace_path_ = std::move(workspace_path);
    capabilities_ = std::move(capabilities);
}

// =============================================================================
// Messaging
// =============================================================================

std::future<std::string> Session::send(MessageOptions options)
{
    return std::async(
        std::launch::async,
        [this, options = std::move(options)]()
        {
            json params = options;
            params["sessionId"] = session_id_;
            params.update(client_->trace_context());

            auto response = client_->rpc_client()->invoke(copilot::rpc::methods::kSessionSend, params).get();
            return response["messageId"].get<std::string>();
        }
    );
}

std::future<void> Session::abort()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            json params;
            params["sessionId"] = session_id_;

            client_->rpc_client()->invoke(copilot::rpc::methods::kSessionAbort, params).get();
        }
    );
}

std::future<std::vector<SessionEvent>> Session::get_messages()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            json params;
            params["sessionId"] = session_id_;

            auto response = client_->rpc_client()->invoke(copilot::rpc::methods::kSessionGetMessages, params).get();

            std::vector<SessionEvent> events;
            if (response.contains("events") && response["events"].is_array())
                for (const auto& event_json : response["events"])
                    events.push_back(parse_session_event(event_json));
            return events;
        }
    );
}

std::future<std::optional<SessionEvent>> Session::send_and_wait(
    MessageOptions options,
    std::chrono::seconds timeout)
{
    return std::async(
        std::launch::async,
        [this, options = std::move(options), timeout]() -> std::optional<SessionEvent>
        {
            std::mutex mtx;
            std::condition_variable cv;
            bool done = false;
            std::optional<SessionEvent> last_assistant_message;
            std::optional<std::string> error_message;

            // Subscribe to events
            auto subscription = on(
                [&](const SessionEvent& evt)
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    if (evt.type == SessionEventType::AssistantMessage)
                    {
                        last_assistant_message = evt;
                    }
                    else if (evt.type == SessionEventType::SessionIdle)
                    {
                        done = true;
                        cv.notify_one();
                    }
                    else if (evt.type == SessionEventType::SessionError)
                    {
                        if (auto* data = evt.try_as<SessionErrorData>())
                            error_message = data->message;
                        else
                            error_message = "Session error";
                        done = true;
                        cv.notify_one();
                    }
                }
            );

            // Send the message
            send(options).get();

            // Wait for completion or timeout
            {
                std::unique_lock<std::mutex> lock(mtx);
                if (!cv.wait_for(lock, timeout, [&] { return done; }))
                {
                    throw std::runtime_error("Timeout waiting for session to become idle");
                }
            }

            if (error_message.has_value())
            {
                throw std::runtime_error("Session error: " + *error_message);
            }

            return last_assistant_message;
        }
    );
}

// =============================================================================
// Event Handling
// =============================================================================

Subscription Session::on(EventHandler handler)
{
    std::lock_guard<std::mutex> lock(handlers_mutex_);

    int id = next_handler_id_++;
    event_handlers_.emplace_back(id, std::move(handler));

    // Return subscription that removes this handler when destroyed
    // Use weak_ptr to avoid UAF if Subscription outlives Session
    std::weak_ptr<Session> weak_self = shared_from_this();
    return Subscription(
        [weak_self, id]()
        {
            if (auto self = weak_self.lock())
            {
                std::lock_guard<std::mutex> lock(self->handlers_mutex_);
                self->event_handlers_.erase(
                    std::remove_if(
                        self->event_handlers_.begin(),
                        self->event_handlers_.end(),
                        [id](const auto& pair) { return pair.first == id; }
                    ),
                    self->event_handlers_.end()
                );
            }
        }
    );
}

void Session::register_persistent_event_handler(EventHandler handler)
{
    auto subscription = on(std::move(handler));
    std::lock_guard<std::mutex> lock(owned_event_subscriptions_mutex_);
    owned_event_subscriptions_.push_back(std::move(subscription));
}

void Session::dispatch_event(const SessionEvent& event)
{
    if (event.type == SessionEventType::ExternalToolRequested)
    {
        const auto* data = event.try_as<ExternalToolRequestedData>();
        if (data)
        {
            Tool tool;
            {
                std::lock_guard<std::mutex> lock(tools_mutex_);
                const auto it = tools_.find(data->tool_name);
                if (it != tools_.end())
                    tool = it->second;
            }
            if (tool.handler)
            {
                const auto request = *data;
                enqueue_background(
                    [this, tool = std::move(tool), request]
                    {
                        try
                        {
                            ToolInvocation invocation{
                                .session_id = session_id_,
                                .tool_call_id = request.tool_call_id,
                                .tool_name = request.tool_name,
                                .arguments = request.arguments,
                                .traceparent = request.traceparent,
                                .tracestate = request.tracestate,
                            };
                            const auto result = tool.handler(invocation);
                            invoke(
                                "session.tools.handlePendingToolCall",
                                json{{"requestId", request.request_id}, {"result", result}}).get();
                        }
                        catch (const std::exception& error)
                        {
                            try
                            {
                                invoke(
                                    "session.tools.handlePendingToolCall",
                                    json{
                                        {"requestId", request.request_id},
                                        {"error", error.what()},
                                    }).get();
                            }
                            catch (...)
                            {
                            }
                        }
                    });
            }
        }
    }

    if (event.type == SessionEventType::PermissionRequested &&
        (permission_handler_ || permission_handler_with_context_))
    {
        const auto* data = event.try_as<PermissionRequestedData>();
        if (data && !data->resolved_by_hook.value_or(false))
        {
            const auto request_id = data->request_id;
            const auto request_json = data->permission_request;
            enqueue_background(
                [this, request_id, request_json]
                {
                    try
                    {
                        const auto request = request_json.get<PermissionRequest>();
                        auto result = handle_permission_request(request);
                        if (result.kind == "no-result")
                            return;
                        if (result.kind == "approved")
                            result.kind = "approve-once";
                        json params{{"requestId", request_id}, {"result", json(result)}};
                        if (result.decision_context)
                            params["decisionContext"] = *result.decision_context;
                        invoke(
                            "session.permissions.handlePendingPermissionRequest",
                            std::move(params)).get();
                    }
                    catch (...)
                    {
                        try
                        {
                            invoke(
                                "session.permissions.handlePendingPermissionRequest",
                                json{
                                    {"requestId", request_id},
                                    {"result", {{"kind", "user-not-available"}}},
                                }).get();
                        }
                        catch (...)
                        {
                        }
                    }
                });
        }
    }

    if (event.type == SessionEventType::UserInputRequested && user_input_handler_)
    {
        const auto* data = event.try_as<UserInputRequestedData>();
        if (data)
        {
            const auto value = *data;
            enqueue_background(
                [this, value]
                {
                    UserInputRequest request{
                        .question = value.question,
                        .choices = value.choices,
                        .allow_freeform = value.allow_freeform,
                    };
                    const auto response = handle_user_input_request(request);
                    invoke(
                        "session.ui.handlePendingUserInput",
                        json{
                            {"requestId", value.request_id},
                            {"response", response},
                        }).get();
                });
        }
    }

    if (event.type == SessionEventType::McpOauthRequired && mcp_auth_handler_)
    {
        const auto* data = event.try_as<McpOauthRequiredData>();
        if (data)
        {
            json request{
                {"requestId", data->request_id},
                {"serverName", data->server_name},
                {"serverUrl", data->server_url},
            };
            if (data->reason)
                request["reason"] = *data->reason;
            if (data->www_authenticate_params)
                request["wwwAuthenticateParams"] = *data->www_authenticate_params;
            if (data->resource_metadata)
                request["resourceMetadata"] = *data->resource_metadata;
            if (data->static_client_config)
            {
                json config{{"clientId", data->static_client_config->client_id}};
                if (data->static_client_config->client_secret)
                    config["clientSecret"] = *data->static_client_config->client_secret;
                if (data->static_client_config->grant_type)
                    config["grantType"] = *data->static_client_config->grant_type;
                if (data->static_client_config->public_client)
                    config["publicClient"] = *data->static_client_config->public_client;
                request["staticClientConfig"] = std::move(config);
            }

            const auto handler = mcp_auth_handler_;
            const auto request_id = data->request_id;
            enqueue_background(
                [this, handler, request = std::move(request), request_id]
                {
                    const json result =
                        build_mcp_auth_response(handler, request, session_id_);
                    if (!client_)
                        return;
                    try
                    {
                        invoke(
                            "session.mcp.oauth.handlePendingRequest",
                            json{{"requestId", request_id}, {"result", result}}).get();
                    }
                    catch (...)
                    {
                    }
                });
        }
    }

    std::vector<EventHandler> handlers_copy;

    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        handlers_copy.reserve(event_handlers_.size());
        for (const auto& [id, handler] : event_handlers_)
            handlers_copy.push_back(handler);
    }

    for (const auto& handler : handlers_copy)
    {
        try
        {
            handler(event);
        }
        catch (...)
        {
            // Ignore handler exceptions to prevent one handler from
            // breaking others
        }
    }
}

// =============================================================================
// Tool Management
// =============================================================================

void Session::register_tool(Tool tool)
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    tools_[tool.name] = std::move(tool);
}

void Session::register_tools(const std::vector<Tool>& tools)
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    for (const auto& tool : tools)
        tools_[tool.name] = tool;
}

const Tool* Session::get_tool(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    auto it = tools_.find(name);
    return (it != tools_.end()) ? &it->second : nullptr;
}

void Session::register_canvases(
    const std::vector<std::shared_ptr<Canvas>>& canvases)
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    for (const auto& canvas : canvases)
        if (canvas)
            canvases_[canvas->id()] = canvas;
}

json Session::handle_canvas_request(
    const std::string& method, const json& params)
{
    const auto canvas_id = params.value("canvasId", "");
    std::shared_ptr<Canvas> canvas;
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        const auto it = canvases_.find(canvas_id);
        if (it == canvases_.end())
            throw CanvasError("canvas_not_found", "Unknown canvas " + canvas_id);
        canvas = it->second;
    }

    try
    {
        if (method == "canvas.open")
            return canvas->handle_open(params);
        if (method == "canvas.close")
        {
            canvas->handle_close(params);
            return json::object();
        }
        return canvas->handle_action(params.value("actionName", ""), params);
    }
    catch (const CanvasError& error)
    {
        return json{{"error", {{"code", error.code()}, {"message", error.what()}}}};
    }
}

void Session::register_factories(
    const std::vector<std::shared_ptr<FactoryHandle>>& factories)
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    for (const auto& factory : factories)
    {
        if (!factory || !factory->meta.contains("name"))
            continue;
        factories_[factory->meta.at("name").get<std::string>()] = factory;
    }
}

json Session::handle_factory_request(
    const std::string& method, const json& params)
{
    if (method == "factory.abort")
        return json::object();

    const auto name = params.value("name", "");
    std::shared_ptr<FactoryHandle> factory;
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        const auto it = factories_.find(name);
        if (it == factories_.end())
            throw std::runtime_error("Unknown factory " + name);
        factory = it->second;
    }
    json context = params;
    context["sessionId"] = session_id_;
    return json{{"result", factory->run(context)}};
}

// =============================================================================
// Permission Handling
// =============================================================================

void Session::register_permission_handler(PermissionHandler handler)
{
    permission_handler_ = std::move(handler);
}

void Session::register_permission_handler(PermissionHandlerWithContext handler)
{
    permission_handler_with_context_ = std::move(handler);
}

void Session::register_mcp_auth_handler(McpAuthHandler handler)
{
    mcp_auth_handler_ = std::move(handler);
}

PermissionRequestResult Session::handle_permission_request(const PermissionRequest& request)
{
    PermissionInvocation invocation{
        .session_id = session_id_,
        .managed_settings_enabled = managed_settings_enabled_,
    };
    if (permission_handler_with_context_)
        return permission_handler_with_context_(request, invocation);
    if (permission_handler_)
        return permission_handler_(request);

    return PermissionRequestResult::no_result();
}

// =============================================================================
// User Input Handling
// =============================================================================

void Session::register_user_input_handler(UserInputHandler handler)
{
    std::lock_guard<std::mutex> lock(user_input_mutex_);
    user_input_handler_ = std::move(handler);
}

UserInputResponse Session::handle_user_input_request(const UserInputRequest& request)
{
    UserInputHandler handler;
    {
        std::lock_guard<std::mutex> lock(user_input_mutex_);
        handler = user_input_handler_;
    }

    if (!handler)
        throw std::runtime_error("No user input handler registered");

    UserInputInvocation invocation;
    invocation.session_id = session_id_;
    return handler(request, invocation);
}

// =============================================================================
// Elicitation Handling
// =============================================================================

void Session::register_elicitation_handler(ElicitationHandler handler)
{
    std::lock_guard<std::mutex> lock(elicitation_mutex_);
    elicitation_handler_ = std::move(handler);
}

ElicitationResult Session::handle_elicitation_request(const ElicitationContext& context)
{
    ElicitationHandler handler;
    {
        std::lock_guard<std::mutex> lock(elicitation_mutex_);
        handler = elicitation_handler_;
    }

    if (!handler)
        return ElicitationResult{ElicitationAction::Cancel};

    return handler(context);
}

// =============================================================================
// Exit Plan Mode Handling
// =============================================================================

void Session::register_exit_plan_mode_handler(ExitPlanModeHandler handler)
{
    std::lock_guard<std::mutex> lock(exit_plan_mode_mutex_);
    exit_plan_mode_handler_ = std::move(handler);
}

ExitPlanModeResult Session::handle_exit_plan_mode_request(const ExitPlanModeRequest& request)
{
    ExitPlanModeHandler handler;
    {
        std::lock_guard<std::mutex> lock(exit_plan_mode_mutex_);
        handler = exit_plan_mode_handler_;
    }

    if (!handler)
        return ExitPlanModeResult{};

    ExitPlanModeInvocation invocation;
    invocation.session_id = session_id_;
    return handler(request, invocation);
}

// =============================================================================
// Auto Mode Switch Handling
// =============================================================================

void Session::register_auto_mode_switch_handler(AutoModeSwitchHandler handler)
{
    std::lock_guard<std::mutex> lock(auto_mode_switch_mutex_);
    auto_mode_switch_handler_ = std::move(handler);
}

AutoModeSwitchResponse Session::handle_auto_mode_switch_request(const AutoModeSwitchRequest& request)
{
    AutoModeSwitchHandler handler;
    {
        std::lock_guard<std::mutex> lock(auto_mode_switch_mutex_);
        handler = auto_mode_switch_handler_;
    }

    if (!handler)
        return AutoModeSwitchResponse::No;

    AutoModeSwitchInvocation invocation;
    invocation.session_id = session_id_;
    return handler(request, invocation);
}

// =============================================================================
// Hooks
// =============================================================================

void Session::register_hooks(SessionHooks hooks)
{
    std::lock_guard<std::mutex> lock(hooks_mutex_);
    hooks_ = std::move(hooks);
}

json Session::handle_hooks_invoke(const std::string& hook_type, const json& input)
{
    std::optional<SessionHooks> hooks;
    {
        std::lock_guard<std::mutex> lock(hooks_mutex_);
        hooks = hooks_;
    }

    if (!hooks)
        return nullptr;

    HookInvocation invocation;
    invocation.session_id = session_id_;

    try
    {
    if (hook_type == "preToolUse" && hooks->on_pre_tool_use)
    {
        auto result = (*hooks->on_pre_tool_use)(input.get<PreToolUseHookInput>(), invocation);
        if (result)
        {
            json output;
            to_json(output, *result);
            return output;
        }
        return nullptr;
    }
    else if (hook_type == "postToolUse" && hooks->on_post_tool_use)
    {
        auto result = (*hooks->on_post_tool_use)(input.get<PostToolUseHookInput>(), invocation);
        if (result)
        {
            json output;
            to_json(output, *result);
            return output;
        }
        return nullptr;
    }
    else if (hook_type == "userPromptSubmitted" && hooks->on_user_prompt_submitted)
    {
        auto result = (*hooks->on_user_prompt_submitted)(input.get<UserPromptSubmittedHookInput>(), invocation);
        if (result)
        {
            json output;
            to_json(output, *result);
            return output;
        }
        return nullptr;
    }
    else if (hook_type == "sessionStart" && hooks->on_session_start)
    {
        auto result = (*hooks->on_session_start)(input.get<SessionStartHookInput>(), invocation);
        if (result)
        {
            json output;
            to_json(output, *result);
            return output;
        }
        return nullptr;
    }
    else if (hook_type == "sessionEnd" && hooks->on_session_end)
    {
        auto result = (*hooks->on_session_end)(input.get<SessionEndHookInput>(), invocation);
        if (result)
        {
            json output;
            to_json(output, *result);
            return output;
        }
        return nullptr;
    }
    else if (hook_type == "errorOccurred" && hooks->on_error_occurred)
    {
        auto result = (*hooks->on_error_occurred)(input.get<ErrorOccurredHookInput>(), invocation);
        if (result)
        {
            json output;
            to_json(output, *result);
            return output;
        }
        return nullptr;
    }

    const JsonHookHandler* generic = nullptr;
    if (hook_type == "preMcpToolCall" && hooks->on_pre_mcp_tool_call)
        generic = &*hooks->on_pre_mcp_tool_call;
    else if (hook_type == "postToolUseFailure" && hooks->on_post_tool_use_failure)
        generic = &*hooks->on_post_tool_use_failure;
    else if (hook_type == "userPromptTransformed" && hooks->on_user_prompt_transformed)
        generic = &*hooks->on_user_prompt_transformed;
    else if (hook_type == "agentStop" && hooks->on_agent_stop)
        generic = &*hooks->on_agent_stop;
    else if (hook_type == "subagentStop" && hooks->on_subagent_stop)
        generic = &*hooks->on_subagent_stop;
    else if (hook_type == "permissionRequest" && hooks->on_permission_request)
        generic = &*hooks->on_permission_request;

    if (generic)
    {
        const auto result = (*generic)(input, invocation);
        return result.value_or(json(nullptr));
    }
    }
    catch (const json::exception&)
    {
        // Unknown or newly-null hook fields must not block session startup.
        return nullptr;
    }

    return nullptr;
}

// =============================================================================
// Lifecycle
// =============================================================================

std::future<void> Session::destroy()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            json params;
            params["sessionId"] = session_id_;

            client_->rpc_client()->invoke(copilot::rpc::methods::kSessionDestroy, params).get();
        }
    );
}

std::future<json> Session::invoke(const std::string& method, json params)
{
    return std::async(
        std::launch::async,
        [this, method, params = std::move(params)]() mutable
        {
            if (!params.is_object())
                throw std::invalid_argument("session RPC params must be a JSON object");
            params["sessionId"] = session_id_;
            return client_->rpc_client()->invoke(method, params).get();
        }
    );
}

std::future<generated::api::McpServerList> Session::list_mcp_servers()
{
    return invoke_typed<generated::api::McpServerList>("session.mcp.list");
}

std::future<generated::api::AuthIdentity> Session::get_current_auth_info()
{
    return invoke_typed<generated::api::AuthIdentity>(
        "session.gitHubAuth.getCurrentAuthInfo"
    );
}

std::future<generated::api::AgentList> Session::list_agents(json params)
{
    return invoke_typed<generated::api::AgentList>(
        "session.agent.list", std::move(params)
    );
}

std::future<generated::api::SkillList> Session::list_skills()
{
    return invoke_typed<generated::api::SkillList>("session.skills.list");
}

std::future<generated::api::TaskList> Session::list_tasks()
{
    return invoke_typed<generated::api::TaskList>("session.tasks.list");
}

std::future<generated::api::CommandList> Session::list_commands(json params)
{
    return invoke_typed<generated::api::CommandList>(
        "session.commands.list", std::move(params)
    );
}

std::future<generated::api::HistoryListRewindPointsResult>
Session::list_rewind_points()
{
    return invoke_typed<generated::api::HistoryListRewindPointsResult>(
        "session.history.listRewindPoints"
    );
}

std::future<generated::api::UsageGetMetricsResult> Session::get_usage_metrics()
{
    return invoke_typed<generated::api::UsageGetMetricsResult>(
        "session.usage.getMetrics"
    );
}

std::future<generated::api::RemoteEnableResult> Session::enable_remote(json params)
{
    return invoke_typed<generated::api::RemoteEnableResult>(
        "session.remote.enable", std::move(params)
    );
}

std::future<void> Session::disable_remote()
{
    return std::async(
        std::launch::async,
        [future = invoke("session.remote.disable")]() mutable
        {
            future.get();
        }
    );
}

SessionFactoryApi Session::factory()
{
    return SessionFactoryApi(*this);
}

std::future<generated::api::SandboxEnforcementStatus>
Session::get_sandbox_enforcement_status()
{
    return invoke_typed<generated::api::SandboxEnforcementStatus>(
        "session.sandbox.getEnforcementStatus"
    );
}

// =============================================================================
// Model & Mode (v0.1.49 additions)
// =============================================================================

std::future<void> Session::set_model(const std::string& model_id, SetModelOptions options)
{
    return std::async(
        std::launch::async,
        [this, model_id, options]()
        {
            json params;
            params["sessionId"] = session_id_;
            params["modelId"] = model_id;
            if (options.reasoning_effort.has_value())
                params["reasoningEffort"] = *options.reasoning_effort;

            client_->rpc_client()->invoke(copilot::rpc::methods::kSessionModelSwitchTo, params).get();
        }
    );
}

namespace
{

const char* auto_tier_wire(Session::AutoTier tier)
{
    switch (tier)
    {
    case Session::AutoTier::Efficiency:
        return "efficiency";
    case Session::AutoTier::Balance:
        return "balance";
    case Session::AutoTier::Intelligence:
        return "intelligence";
    case Session::AutoTier::Fast:
        return "fast";
    }
    return "balance";
}

std::optional<Session::AutoTier> auto_tier_from_wire(const json& value)
{
    if (!value.is_string())
        return std::nullopt;
    const auto text = value.get<std::string>();
    if (text == "efficiency")
        return Session::AutoTier::Efficiency;
    if (text == "balance")
        return Session::AutoTier::Balance;
    if (text == "intelligence")
        return Session::AutoTier::Intelligence;
    if (text == "fast")
        return Session::AutoTier::Fast;
    // An unrecognized tier is left unset rather than guessed; the raw payload
    // still carries it for callers that need the exact value.
    return std::nullopt;
}

} // namespace

std::future<Session::AutoTierResult> Session::set_auto_tier(std::optional<AutoTier> auto_tier)
{
    return std::async(
        std::launch::async,
        [this, auto_tier]() -> AutoTierResult
        {
            json params;
            params["sessionId"] = session_id_;
            // null is meaningful here -- it returns the session to provider-default
            // Auto routing -- so the key is always present.
            if (auto_tier)
                params["autoTier"] = auto_tier_wire(*auto_tier);
            else
                params["autoTier"] = nullptr;

            const json response =
                client_->rpc_client()
                    ->invoke(copilot::rpc::methods::kSessionModelSwitchAutoTier, params)
                    .get();

            AutoTierResult result;
            result.raw = response;
            if (response.is_object())
            {
                if (const auto it = response.find("status");
                    it != response.end() && it->is_string())
                    result.status = it->get<std::string>();
                if (const auto it = response.find("effectiveAutoTier"); it != response.end())
                    result.effective_auto_tier = auto_tier_from_wire(*it);
                if (const auto it = response.find("pendingAutoTier"); it != response.end())
                    result.pending_auto_tier = auto_tier_from_wire(*it);
            }
            return result;
        }
    );
}

std::future<std::optional<std::string>> Session::get_current_model()
{
    return std::async(
        std::launch::async,
        [this]() -> std::optional<std::string>
        {
            json params;
            params["sessionId"] = session_id_;
            auto response = client_->rpc_client()->invoke(copilot::rpc::methods::kSessionModelGetCurrent, params).get();
            // Response: { modelId?: string } per nodejs CurrentModel shape.
            if (response.contains("modelId") && !response["modelId"].is_null())
                return response["modelId"].get<std::string>();
            return std::nullopt;
        }
    );
}

namespace
{
const char* mode_to_wire(Session::Mode m)
{
    switch (m)
    {
    case Session::Mode::Interactive: return "interactive";
    case Session::Mode::Plan:        return "plan";
    case Session::Mode::Autopilot:   return "autopilot";
    }
    return "interactive";
}

std::optional<Session::Mode> mode_from_wire(const std::string& s)
{
    if (s == "interactive") return Session::Mode::Interactive;
    if (s == "plan")        return Session::Mode::Plan;
    if (s == "autopilot")   return Session::Mode::Autopilot;
    return std::nullopt;
}
} // namespace

std::future<void> Session::set_mode(Mode mode)
{
    return std::async(
        std::launch::async,
        [this, mode]()
        {
            json params;
            params["sessionId"] = session_id_;
            params["mode"] = mode_to_wire(mode);
            client_->rpc_client()->invoke(copilot::rpc::methods::kSessionModeSet, params).get();
        }
    );
}

std::future<Session::Mode> Session::get_mode()
{
    return std::async(
        std::launch::async,
        [this]() -> Mode
        {
            json params;
            params["sessionId"] = session_id_;
            auto response = client_->rpc_client()->invoke(copilot::rpc::methods::kSessionModeGet, params).get();
            // Response shape: { mode: "interactive" | "plan" | "autopilot" }
            std::string wire = response.contains("mode") && response["mode"].is_string()
                                   ? response["mode"].get<std::string>()
                                   : std::string{"interactive"};
            auto parsed = mode_from_wire(wire);
            return parsed.value_or(Mode::Interactive);
        }
    );
}

} // namespace copilot
