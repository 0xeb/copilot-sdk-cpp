// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <condition_variable>
#include <copilot/canvas.hpp>
#include <copilot/client.hpp>
#include <copilot/rpc_methods.hpp>
#include <copilot/session.hpp>
#include <copilot/workflow.hpp>
#include <thread>

namespace copilot
{

// C# REFERENCE: dotnet/src/Session.cs HandleMcpOauthRequiredAsync
json build_mcp_auth_response(
    const McpAuthHandler& handler, const McpAuthRequest& request, const std::string& session_id
)
{
    try
    {
        if (!handler)
            return json{{"kind", "cancelled"}};
        const auto result = handler(request, McpAuthInvocation{session_id});
        if (!result || result->cancelled)
            return json{{"kind", "cancelled"}};
        if (result->token && !result->token->access_token.empty())
        {
            json response{{"kind", "token"}, {"accessToken", result->token->access_token}};
            if (result->token->token_type)
                response["tokenType"] = *result->token->token_type;
            if (result->token->expires_in)
                response["expiresIn"] = *result->token->expires_in;
            return response;
        }
        if (result->runtime_refresh && request.runtime_refresh_supported)
            return json{{"kind", "runtimeRefresh"}};
    }
    catch (...)
    {
    }
    return json{{"kind", "cancelled"}};
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

Session::Session(
    const std::string& session_id,
    Client* client,
    const std::optional<std::string>& workspace_path,
    SessionCapabilities capabilities,
    bool managed_settings_enabled
)
    : session_id_(session_id), client_(client), workspace_path_(workspace_path),
      capabilities_(std::move(capabilities)), managed_settings_enabled_(managed_settings_enabled)
{
}

Session::~Session()
{
    retire();
    std::vector<std::future<void>> tasks;
    {
        std::lock_guard<std::mutex> lock(background_tasks_mutex_);
        tasks.swap(background_tasks_);
    }

    for (auto& task : tasks)
        if (task.valid())
            task.wait();
}

void Session::cancel_pending_tool_invocations() noexcept
{
    tool_invocations_cancelled_.store(true, std::memory_order_release);
}

int Session::pending_tool_invocations() const noexcept
{
    return pending_tool_invocations_.load(std::memory_order_relaxed);
}

void Session::enqueue_background(std::function<void()> task)
{
    auto future = std::async(std::launch::async, std::move(task));
    std::lock_guard<std::mutex> lock(background_tasks_mutex_);
    background_tasks_.push_back(std::move(future));
}

void Session::set_initial_state(
    std::optional<std::string> workspace_path, SessionCapabilities capabilities
)
{
    workspace_path_ = std::move(workspace_path);
    std::lock_guard<std::mutex> lock(capabilities_mutex_);
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
            if (dispatch_stopped_)
                throw std::runtime_error("Session closed");
            json params = options;
            params["sessionId"] = session_id_;
            params.update(client_->trace_context());

            auto response = client_->rpc_client_shared()
                                ->invoke(copilot::rpc::methods::kSessionSend, params)
                                .get();
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

            client_->rpc_client_shared()
                ->invoke(copilot::rpc::methods::kSessionAbort, params)
                .get();
        }
    );
}

std::future<std::vector<SessionEvent>> Session::get_events()
{
    // NODEJS REFERENCE: nodejs/src/session.ts getEvents
    return std::async(
        std::launch::async,
        [this]()
        {
            json params;
            params["sessionId"] = session_id_;

            auto response = client_->rpc_client_shared()
                                ->invoke(copilot::rpc::methods::kSessionGetMessages, params)
                                .get();

            std::vector<SessionEvent> events;
            if (response.contains("events") && response["events"].is_array())
                for (const auto& event_json : response["events"])
                    events.push_back(parse_session_event(event_json));
            return events;
        }
    );
}

std::future<std::vector<json>> Session::get_raw_events()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            json params;
            params["sessionId"] = session_id_;
            auto response = client_->rpc_client_shared()
                                ->invoke(copilot::rpc::methods::kSessionGetMessages, params)
                                .get();
            std::vector<json> events;
            if (response.contains("events") && response["events"].is_array())
                events = response["events"].get<std::vector<json>>();
            return events;
        }
    );
}

// C# REFERENCE: dotnet/src/Session.cs SendAndWaitAsync and Session.StructuredOutput.cs.
std::future<std::optional<SessionEvent>>
Session::send_and_wait(MessageOptions options, std::chrono::seconds timeout)
{
    return wait_for_response(std::move(options), timeout, false);
}

std::future<json>
Session::send_and_wait_structured(MessageOptions options, std::chrono::seconds timeout)
{
    if (options.mode == "immediate")
        throw std::invalid_argument("Structured output cannot use immediate delivery");
    return std::async(
        std::launch::async,
        [future = wait_for_response(std::move(options), timeout, true)]() mutable
        {
            const auto event = future.get();
            return json::parse(event->as<AssistantMessageData>().content);
        }
    );
}

std::future<std::optional<SessionEvent>>
Session::wait_for_response(MessageOptions options, std::chrono::seconds timeout, bool structured)
{
    auto self = shared_from_this();
    return std::async(
        std::launch::async,
        [self, options = std::move(options), timeout, structured](
        ) mutable -> std::optional<SessionEvent>
        {
            struct WaitState
            {
                std::mutex mutex;
                std::condition_variable cv;
                bool admitted = false, started = false, done = false;
                std::string message_id;
                std::optional<SessionEvent> last;
                std::string error;
                std::vector<SessionEvent> buffered;
            };
            auto state = std::make_shared<WaitState>();
            auto process = [state, structured](const SessionEvent& event)
            {
                if (state->done)
                    return;
                auto complete = [&](bool aborted)
                {
                    if (aborted)
                        state->error = "Session aborted before response completed";
                    else if (structured &&
                             (!state->last ||
                              state->last->as<AssistantMessageData>().content.find_first_not_of(
                                  " \t\r\n"
                              ) == std::string::npos))
                        state->error = "Turn completed without a final structured response";
                    state->done = true;
                };
                if (auto user = event.try_as<UserMessageData>();
                    user && user->message_id == state->message_id)
                    state->started = true;
                else if (auto warning = event.try_as<SessionWarningData>();
                         warning && warning->warning_type == "policy_blocked" &&
                         warning->originating_message_id == state->message_id)
                    state->started = true;
                else if (auto assistant = event.try_as<AssistantMessageData>();
                         assistant &&
                         (assistant->originating_message_id == state->message_id ||
                          (!structured &&
                           (!assistant->originating_message_id ||
                            assistant->originating_message_id->empty()) &&
                           state->started)))
                {
                    state->started = true;
                    if (structured && assistant->tool_requests &&
                        !assistant->tool_requests->empty())
                        state->last.reset();
                    else
                        state->last = event;
                }
                else if (auto idle = event.try_as<SessionIdleData>();
                         idle && state->started && idle->mode != "autopilot")
                    complete(idle->aborted.value_or(false));
                else if (auto stopped = event.try_as<SessionAutopilotStoppedData>();
                         stopped && stopped->request_id == state->message_id)
                {
                    if (stopped->reason == "error")
                    {
                        state->error = "Autopilot request failed";
                        state->done = true;
                    }
                    else
                        complete(stopped->reason == "aborted");
                }
                else if (auto failure = event.try_as<SessionErrorData>();
                         failure &&
                         ((!failure->originating_message_id ||
                           failure->originating_message_id->empty())
                              ? (!structured || state->started)
                              : failure->originating_message_id == state->message_id))
                {
                    state->error = "Session error: " + failure->message;
                    state->done = true;
                }
                if (state->done)
                    state->cv.notify_all();
            };
            auto subscription = self->on(
                [state, process](const SessionEvent& event)
                {
                    if (event.type != SessionEventType::UserMessage &&
                        event.type != SessionEventType::SessionWarning &&
                        event.type != SessionEventType::AssistantMessage &&
                        event.type != SessionEventType::SessionIdle &&
                        event.type != SessionEventType::SessionAutopilotStopped &&
                        event.type != SessionEventType::SessionError)
                        return;
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->admitted)
                        state->buffered.push_back(event);
                    else
                        process(event);
                },
                true
            );
            int retirement_id;
            {
                std::lock_guard<std::mutex> lock(self->handlers_mutex_);
                if (self->dispatch_stopped_)
                    throw std::runtime_error("Session closed before response");
                retirement_id = self->next_handler_id_++;
                self->retirement_handlers_[retirement_id] = [state]
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->done)
                    {
                        state->done = true;
                        state->error = "Session closed before response";
                    }
                    state->cv.notify_all();
                };
            }
            Subscription cleanup(
                [self, retirement_id]
                {
                    std::lock_guard<std::mutex> lock(self->handlers_mutex_);
                    self->retirement_handlers_.erase(retirement_id);
                }
            );
            auto rpc = self->client_->rpc_client_shared();
            const auto sent_id = self->send(std::move(options)).get();
            std::unique_lock<std::mutex> lock(state->mutex);
            state->message_id = sent_id;
            state->admitted = true;
            for (const auto& event : state->buffered)
                process(event);
            state->buffered.clear();
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!state->done)
            {
                if (rpc->connection_closed_token().is_cancelled())
                    throw std::runtime_error("Session connection closed before response");
                if (state->cv.wait_until(
                        lock,
                        std::min(
                            deadline,
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(50)
                        )
                    ) == std::cv_status::timeout &&
                    std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("Timeout waiting for session response");
            }
            if (!state->error.empty())
                throw std::runtime_error(state->error);
            return state->last;
        }
    );
}

// =============================================================================
// Event Handling
// =============================================================================

Subscription Session::on(EventHandler handler, bool root_agent_only)
{
    return on(
        [handler = std::move(handler), root_agent_only](const SessionEvent& event)
        {
            if (!root_agent_only || !event.agent_id || event.agent_id->empty())
                handler(event);
        }
    );
}

Subscription Session::on(EventHandler handler)
{
    std::lock_guard<std::mutex> lock(handlers_mutex_);

    if (dispatch_stopped_)
        throw std::runtime_error("Session event dispatch has stopped");
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

// NODEJS REFERENCE: nodejs/src/session.ts _handleBroadcastEvent
void Session::dispatch_event(const SessionEvent& event)
{
    if (dispatch_stopped_)
        return;
    PermissionHandler permission_handler;
    PermissionHandlerWithContext permission_context_handler;
    McpAuthHandler mcp_handler;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        permission_handler = permission_handler_;
        permission_context_handler = permission_handler_with_context_;
        mcp_handler = mcp_auth_handler_;
    }

    if (event.type == SessionEventType::ExternalToolRequested)
    {
        const auto* data = event.try_as<ExternalToolRequestedData>();
        if (data && data->has_handler.value_or(true))
        {
            Tool tool;
            {
                std::lock_guard<std::mutex> lock(tools_mutex_);
                const auto it = tools_.find(data->tool_name);
                if (it != tools_.end())
                    tool = it->second;
            }
            if (tool.handler)
                execute_external_tool(
                    *data,
                    event.agent_id && !event.agent_id->empty() ? event.agent_id : std::nullopt,
                    std::move(tool)
                );
        }
    }
    else if (event.type == SessionEventType::ExternalToolCompleted)
    {
        if (const auto* data = event.try_as<ExternalToolCompletedData>())
        {
            std::optional<CancellationSource> source;
            {
                std::lock_guard<std::mutex> lock(pending_external_tools_mutex_);
                const auto it = pending_external_tools_.find(data->request_id);
                if (it != pending_external_tools_.end())
                {
                    source = it->second;
                    pending_external_tools_.erase(it);
                }
            }
            if (source)
                source->cancel();
        }
    }
    else if (event.type == SessionEventType::PermissionRequested &&
             (permission_handler || permission_context_handler))
    {
        const auto* data = event.try_as<PermissionRequestedData>();
        if (data && !data->resolved_by_hook.value_or(false))
        {
            const auto value = *data;
            enqueue_background(
                [this, value]
                {
                    try
                    {
                        const auto request = value.permission_request.get<PermissionRequest>();
                        auto result = handle_permission_request(
                            request,
                            value.prompt_request,
                            value.assisted_approval_assessment,
                            value.program_tool_call_id
                        );
                        if (result.kind == "no-result" || dispatch_stopped_)
                            return;
                        json params{{"requestId", value.request_id}, {"result", json(result)}};
                        if (result.decision_context)
                            params["decisionContext"] = *result.decision_context;
                        invoke(
                            "session.permissions.handlePendingPermissionRequest", std::move(params)
                        )
                            .get();
                    }
                    catch (...)
                    {
                        if (dispatch_stopped_)
                            return;
                        try
                        {
                            invoke(
                                "session.permissions.handlePendingPermissionRequest",
                                json{
                                    {"requestId", value.request_id},
                                    {"result", {{"kind", "user-not-available"}}},
                                }
                            )
                                .get();
                        }
                        catch (...)
                        {
                        }
                    }
                }
            );
        }
    }
    else if (event.type == SessionEventType::McpOauthRequired && mcp_handler)
    {
        // NODEJS REFERENCE: nodejs/src/session.ts _handleBroadcastEvent mcp.oauth_required
        const auto* data = event.try_as<McpOauthRequiredData>();
        if (data && !data->request_id.empty())
        {
            McpAuthRequest request;
            request.request_id = data->request_id;
            request.server_name = data->server_name;
            request.server_url = data->server_url;
            request.reason = data->reason.value_or("");
            request.runtime_refresh_supported = data->runtime_refresh_supported.value_or(false);
            if (data->www_authenticate_params && data->www_authenticate_params->is_object())
                request.www_authenticate_params =
                    data->www_authenticate_params->get<McpAuthWwwAuthenticateParams>();
            request.resource_metadata = data->resource_metadata;
            if (data->static_client_config)
            {
                const auto& config = *data->static_client_config;
                request.static_client_config = McpAuthStaticClientConfig{
                    .client_id = config.client_id,
                    .client_secret = config.client_secret,
                    .grant_type = config.grant_type,
                    .public_client = config.public_client,
                    .scope = config.scope,
                };
            }
            const auto request_id = data->request_id;
            enqueue_background(
                [this, handler = mcp_handler, request = std::move(request), request_id]
                {
                    const json result = build_mcp_auth_response(handler, request, session_id_);
                    if (!client_ || dispatch_stopped_)
                        return;
                    try
                    {
                        invoke(
                            "session.mcp.oauth.handlePendingRequest",
                            json{{"requestId", request_id}, {"result", result}}
                        ).get();
                    }
                    catch (...)
                    {
                    }
                }
            );
        }
    }
    else if (event.type == SessionEventType::CommandExecute)
    {
        // NODEJS REFERENCE: nodejs/src/session.ts _executeCommandAndRespond
        if (const auto* data = event.try_as<CommandExecuteData>())
        {
            CommandHandler handler;
            {
                std::lock_guard<std::mutex> lock(callbacks_mutex_);
                const auto it = commands_.find(data->command_name);
                if (it != commands_.end())
                    handler = it->second;
            }
            const auto value = *data;
            enqueue_background(
                [this, value, handler]
                {
                    json reply{{"requestId", value.request_id}};
                    if (!handler)
                        reply["error"] = "Unknown command: " + value.command_name;
                    else
                    {
                        try
                        {
                            handler(CommandContext{
                                .session_id = session_id_,
                                .command = value.command,
                                .command_name = value.command_name,
                                .args = value.args,
                            });
                        }
                        catch (const std::exception& error)
                        {
                            reply["error"] = error.what();
                        }
                        catch (...)
                        {
                            reply["error"] = "Unknown error";
                        }
                        if (dispatch_stopped_)
                            return;
                    }
                    try
                    {
                        invoke(rpc::methods::kSessionCommandsHandlePendingCommand, std::move(reply))
                            .get();
                    }
                    catch (...)
                    {
                    }
                }
            );
        }
    }
    else if (event.type == SessionEventType::ElicitationRequested)
    {
        // NODEJS REFERENCE: nodejs/src/session.ts _handleElicitationRequest
        ElicitationHandler handler;
        {
            std::lock_guard<std::mutex> lock(elicitation_mutex_);
            handler = elicitation_handler_;
        }
        const auto* data = event.try_as<ElicitationRequestedData>();
        if (handler && data)
        {
            ElicitationContext context{
                .session_id = session_id_,
                .message = data->message,
                .elicitation_source = data->elicitation_source,
                .url = data->url,
            };
            if (data->requested_schema && data->requested_schema->is_object())
                context.requested_schema = data->requested_schema->get<ElicitationSchema>();
            if (data->mode == "form")
                context.mode = ElicitationRequestedMode::Form;
            else if (data->mode == "url")
                context.mode = ElicitationRequestedMode::Url;
            const auto request_id = data->request_id;
            enqueue_background(
                [this, handler, context = std::move(context), request_id]
                {
                    json result;
                    try
                    {
                        result = json(handler(context));
                    }
                    catch (...)
                    {
                        result = json{{"action", "cancel"}};
                    }
                    try
                    {
                        invoke(
                            rpc::methods::kSessionUiHandlePendingElicitation,
                            json{{"requestId", request_id}, {"result", std::move(result)}}
                        )
                            .get();
                    }
                    catch (...)
                    {
                    }
                }
            );
        }
    }
    else if (event.type == SessionEventType::CapabilitiesChanged)
    {
        if (const auto* data = event.try_as<CapabilitiesChangedData>())
            merge_capabilities(data->raw);
    }
    else if (event.type == SessionEventType::SessionCanvasOpened)
    {
        // C# REFERENCE: Session.cs UpdateOpenCanvasesFromEvent (opened -> upsert)
        const auto* data = event.try_as<CanvasOpenedData>();
        if (data && !data->instance_id.empty() && !data->extension_id.empty() &&
            !data->canvas_id.empty())
        {
            json instance{
                {"instanceId", data->instance_id},
                {"extensionId", data->extension_id},
                {"canvasId", data->canvas_id},
            };
            if (data->extension_name)
                instance["extensionName"] = *data->extension_name;
            if (data->icon)
                instance["icon"] = *data->icon;
            if (data->title)
                instance["title"] = *data->title;
            if (data->status)
                instance["status"] = *data->status;
            if (data->url)
                instance["url"] = *data->url;
            if (data->input)
                instance["input"] = *data->input;
            std::lock_guard<std::mutex> lock(resume_state_mutex_);
            auto it = std::find_if(
                open_canvases_.begin(), open_canvases_.end(),
                [&](const OpenCanvasInstance& open)
                { return open.raw().value("instanceId", std::string()) == data->instance_id; }
            );
            if (it != open_canvases_.end())
                *it = OpenCanvasInstance(std::move(instance));
            else
                open_canvases_.emplace_back(std::move(instance));
        }
    }
    else if (event.type == SessionEventType::SessionCanvasClosed)
    {
        // C# REFERENCE: Session.cs UpdateOpenCanvasesFromEvent (closed -> remove)
        const auto* data = event.try_as<CanvasClosedData>();
        if (data && !data->instance_id.empty())
        {
            std::lock_guard<std::mutex> lock(resume_state_mutex_);
            std::erase_if(
                open_canvases_,
                [&](const OpenCanvasInstance& open)
                { return open.raw().value("instanceId", std::string()) == data->instance_id; }
            );
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

void Session::merge_capabilities(const json& changed)
{
    if (!changed.is_object())
        return;
    std::lock_guard<std::mutex> lock(capabilities_mutex_);
    if (!capabilities_.value.is_object())
        capabilities_.value = json::object();
    // The event names only the capabilities that changed, so merge `ui` key by key.
    for (const auto& [key, value] : changed.items())
    {
        if (key == "ui" && value.is_object())
        {
            auto& ui = capabilities_.value["ui"];
            if (!ui.is_object())
                ui = json::object();
            for (const auto& [ui_key, ui_value] : value.items())
                ui[ui_key] = ui_value;
        }
        else if (key != "ui")
            capabilities_.value[key] = value;
    }
}

bool Session::claim_external_tool(const std::string& request_id)
{
    std::lock_guard<std::mutex> lock(pending_external_tools_mutex_);
    if (dispatch_stopped_)
        return false;
    return pending_external_tools_.erase(request_id) > 0;
}

// NODEJS REFERENCE: nodejs/src/session.ts _executeToolAndRespond
void Session::execute_external_tool(
    ExternalToolRequestedData request, std::optional<std::string> agent_id, Tool tool
)
{
    CancellationSource source;
    {
        std::lock_guard<std::mutex> lock(pending_external_tools_mutex_);
        if (dispatch_stopped_ || pending_external_tools_.count(request.request_id))
            return;
        pending_external_tools_.emplace(request.request_id, source);
    }
    pending_tool_invocations_.fetch_add(1, std::memory_order_relaxed);
    enqueue_background(
        [this,
         tool = std::move(tool),
         request = std::move(request),
         agent_id = std::move(agent_id),
         source]() mutable
        {
            // Answering after the connection has gone means an RPC into a transport
            // being torn down. The handler cannot be interrupted, so its result is dropped.
            const auto answer = [this](json payload)
            {
                if (tool_invocations_cancelled_.load(std::memory_order_acquire) ||
                    dispatch_stopped_)
                    return;
                invoke("session.tools.handlePendingToolCall", std::move(payload)).get();
            };

            bool claimed = false;
            std::optional<std::string> error_message;
            try
            {
                std::optional<std::vector<json>> available_tools;
                if (request.tool_name == "tool_search_tool" && !source.is_cancelled())
                {
                    // A failed fetch leaves the snapshot unset rather than failing the tool.
                    try
                    {
                        auto metadata = invoke("session.tools.getCurrentMetadata").get();
                        if (metadata.contains("tools") && metadata["tools"].is_array())
                            available_tools = metadata["tools"].get<std::vector<json>>();
                    }
                    catch (...)
                    {
                    }
                }
                if (!source.is_cancelled())
                {
                    ToolInvocation invocation{
                        .session_id = session_id_,
                        .tool_call_id = request.tool_call_id,
                        .tool_name = request.tool_name,
                        .arguments = request.arguments,
                        .available_tools = std::move(available_tools),
                        .traceparent = request.traceparent,
                        .tracestate = request.tracestate,
                        .agent_id = agent_id,
                        .request_id = request.request_id,
                        .cancellation = source.token(),
                    };
                    const auto result = tool.handler(invocation);
                    if (claim_external_tool(request.request_id))
                    {
                        claimed = true;
                        answer(json{{"requestId", request.request_id}, {"result", result}});
                    }
                }
            }
            catch (const std::exception& error)
            {
                error_message = error.what();
            }
            catch (...)
            {
                error_message = "";
            }

            if (error_message && (claimed || claim_external_tool(request.request_id)))
            {
                // An empty error reads as no error, so the call would count as a success.
                if (error_message->empty())
                    error_message = "Error";
                // Handler errors are initial replies too: retry delivery once, but never
                // retry a fallback error sent after a failed result delivery.
                for (int attempt = 0; attempt < (claimed ? 1 : 2); ++attempt)
                {
                    try
                    {
                        answer(json{{"requestId", request.request_id}, {"error", *error_message}});
                        break;
                    }
                    catch (const std::exception& rpc_error)
                    {
                        if (dispatch_stopped_)
                            break;
                        if (std::string(rpc_error.what()).size())
                            error_message = rpc_error.what();
                    }
                    catch (...)
                    {
                        break;
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(pending_external_tools_mutex_);
                pending_external_tools_.erase(request.request_id);
            }
            source.cancel();
            pending_tool_invocations_.fetch_sub(1, std::memory_order_relaxed);
        }
    );
}

// =============================================================================
// Tool Management
// =============================================================================

void Session::register_tool(Tool tool)
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session closed");
    tools_[tool.name] = std::move(tool);
}

void Session::register_tools(const std::vector<Tool>& tools)
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session closed");
    for (const auto& tool : tools)
        tools_[tool.name] = tool;
}

const Tool* Session::get_tool(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    auto it = tools_.find(name);
    return (it != tools_.end()) ? &it->second : nullptr;
}

std::optional<Tool> Session::copy_tool(const std::string& name) const
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    if (dispatch_stopped_)
        return std::nullopt;
    const auto it = tools_.find(name);
    return it == tools_.end() ? std::nullopt : std::optional<Tool>(it->second);
}

void Session::register_canvases(const std::vector<std::shared_ptr<Canvas>>& canvases)
{
    std::lock_guard<std::mutex> lock(tools_mutex_);
    for (const auto& canvas : canvases)
        if (canvas)
            canvases_[canvas->id()] = canvas;
}

json Session::handle_canvas_request(const std::string& method, const json& params)
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

void Session::register_workflows(const std::vector<std::shared_ptr<WorkflowHandle>>& workflows)
{
    // TS REFERENCE: nodejs/src/session.ts registerWorkflows (duplicate names reject)
    std::map<std::string, std::shared_ptr<WorkflowHandle>> registered;
    for (const auto& workflow : workflows)
    {
        if (!workflow || !workflow->meta.contains("name"))
            continue;
        const auto name = workflow->meta.at("name").get<std::string>();
        if (!registered.emplace(name, workflow).second)
            throw std::invalid_argument(
                "Duplicate workflow name \"" + name +
                "\". Workflow names must be unique within a joinSession call."
            );
    }
    std::lock_guard<std::mutex> lock(tools_mutex_);
    workflows_ = std::move(registered);
}

json Session::handle_workflow_request(const std::string& method, const json& params)
{
    if (method == "workflow.abort")
        return json::object();

    const auto name = params.value("name", "");
    std::shared_ptr<WorkflowHandle> workflow;
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        const auto it = workflows_.find(name);
        if (it == workflows_.end())
            throw JsonRpcError(
                JsonRpcErrorCode::InvalidParams,
                "No workflow registered with name \"" + name + "\"",
                json{{"code", "workflow_not_found"}, {"name", name}}
            );
        workflow = it->second;
    }
    json context = params;
    context["sessionId"] = session_id_;
    return json{{"result", workflow->run(context)}};
}

// =============================================================================
// Permission Handling
// =============================================================================

void Session::register_permission_handler(PermissionHandler handler)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    permission_handler_ = std::move(handler);
}

void Session::register_permission_handler(PermissionHandlerWithContext handler)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    permission_handler_with_context_ = std::move(handler);
}

void Session::register_mcp_auth_handler(McpAuthHandler handler)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    mcp_auth_handler_ = std::move(handler);
}

PermissionRequestResult Session::handle_permission_request(
    const PermissionRequest& request,
    std::optional<json> prompt_request,
    std::optional<json> assisted_approval_assessment,
    std::optional<std::string> program_tool_call_id
)
{
    PermissionHandler handler;
    PermissionHandlerWithContext context_handler;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (dispatch_stopped_)
            throw std::runtime_error("Session callbacks retired");
        handler = permission_handler_;
        context_handler = permission_handler_with_context_;
    }
    PermissionInvocation invocation{
        .session_id = session_id_,
        .managed_settings_enabled = managed_settings_enabled_,
        .prompt_request = std::move(prompt_request),
        .assisted_approval_assessment = std::move(assisted_approval_assessment),
        .program_tool_call_id = std::move(program_tool_call_id),
    };
    if (context_handler)
        return context_handler(request, invocation);
    if (handler)
        return handler(request);

    return PermissionRequestResult::no_result();
}

// =============================================================================
// User Input Handling
// =============================================================================

void Session::register_user_input_handler(UserInputHandler handler)
{
    std::lock_guard<std::mutex> lock(user_input_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
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
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    elicitation_handler_ = std::move(handler);
}

// =============================================================================
// Commands, bearer tokens, shell authentication, memory store
// =============================================================================

void Session::register_commands(const std::vector<CommandDefinition>& commands)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    commands_.clear();
    for (const auto& command : commands)
        if (command.handler)
            commands_[command.name] = command.handler;
}

void Session::register_bearer_token_providers(std::map<std::string, BearerTokenProvider> providers)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    bearer_token_providers_ = std::move(providers);
}

// NODEJS REFERENCE: nodejs/src/session.ts registerBearerTokenProviders (providerToken.getToken)
json Session::handle_provider_token_request(const std::string& provider_name)
{
    BearerTokenProvider provider;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (dispatch_stopped_)
            throw std::runtime_error("Session callbacks retired");
        const auto it = bearer_token_providers_.find(provider_name);
        if (it != bearer_token_providers_.end())
            provider = it->second;
    }
    if (!provider)
        throw std::runtime_error(
            "No bearer-token provider registered for provider \"" + provider_name + "\""
        );
    return json{
        {"token",
         provider(ProviderTokenArgs{.provider_name = provider_name, .session_id = session_id_})},
    };
}

void Session::register_shell_authentication_handler(ShellAuthenticationHandler handler)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    shell_authentication_handler_ = std::move(handler);
}

// NODEJS REFERENCE: nodejs/src/session.ts _handleShellAuthenticationRequest
json Session::handle_shell_authentication_request(
    const ShellAuthenticationRequest& request, const CancellationToken& cancellation
)
{
    const json cancel{{"action", "cancel"}};
    ShellAuthenticationHandler handler;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        handler = shell_authentication_handler_;
    }
    if (!handler || dispatch_stopped_ || cancellation.is_cancelled())
        return cancel;
    try
    {
        const auto response = handler(
            request,
            ShellAuthenticationInvocation{.session_id = session_id_, .cancellation = cancellation}
        );
        if (cancellation.is_cancelled() || !response.accepted)
            return cancel;
        return json{{"action", "accept"}, {"value", response.value}};
    }
    catch (...)
    {
        // The exception text could contain the answer, so it is deliberately not surfaced.
        return cancel;
    }
}

void Session::register_memory_store(MemoryStoreHandler handler)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    memory_store_ = std::move(handler);
}

void Session::register_transform_callbacks(std::map<std::string, SectionTransformFn> callbacks)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    transform_callbacks_ = std::move(callbacks);
}

json Session::handle_system_message_transform(const json& sections)
{
    if (dispatch_stopped_)
        throw std::runtime_error("Session closed");
    std::map<std::string, SectionTransformFn> callbacks;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        callbacks = transform_callbacks_;
    }
    json result = json::object();
    for (const auto& [section_id, section] : sections.items())
    {
        const auto content = section.value("content", std::string{});
        const auto it = callbacks.find(section_id);
        if (it == callbacks.end())
        {
            result[section_id] = {{"content", content}};
            continue;
        }
        try
        {
            result[section_id] = {{"content", it->second(content)}};
        }
        catch (...)
        {
            result[section_id] = {{"content", content}};
        }
    }
    return {{"sections", std::move(result)}};
}

void Session::register_session_fs_handler(SessionFsHandler handler)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    session_fs_handler_ = std::move(handler);
}

SessionFsHandler Session::session_fs_handler() const
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    return session_fs_handler_;
}

// NODEJS REFERENCE: nodejs/src/session.ts setSessionCreationHandler
std::future<generated::api::SessionManagementPolicy>
Session::set_session_creation_handler(SessionCreationHandler handler)
{
    const bool has_handler = static_cast<bool>(handler);
    SessionCreationHandler previous;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (dispatch_stopped_)
            throw std::runtime_error("Session callbacks retired");
        previous = std::move(session_creation_handler_);
        session_creation_handler_ = std::move(handler);
        generation = ++session_creation_generation_;
    }
    auto pending = invoke("session.management.configure", {{"hasCreateHandler", has_handler}});
    return std::async(
        std::launch::async,
        [this, pending = std::move(pending), previous = std::move(previous), generation]() mutable
        {
            try
            {
                return pending.get().get<generated::api::SessionManagementPolicy>();
            }
            catch (...)
            {
                // Restore only if no newer registration replaced this one meanwhile.
                std::lock_guard<std::mutex> lock(callbacks_mutex_);
                if (session_creation_generation_ == generation && !dispatch_stopped_)
                {
                    session_creation_handler_ = std::move(previous);
                    ++session_creation_generation_;
                }
                throw;
            }
        }
    );
}

// NODEJS REFERENCE: nodejs/src/session.ts _handleSessionManagementCreate
json Session::handle_session_management_create(
    const json& request, const CancellationToken& cancellation
)
{
    SessionCreationHandler handler;
    CancellationSource source;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (dispatch_stopped_)
            throw std::runtime_error("Session callbacks retired");
        handler = session_creation_handler_;
        if (!handler || request.value("parentSessionId", std::string{}) != session_id_ ||
            request.value("sessionId", std::string{}) != session_id_)
            throw std::runtime_error("Session creation handler is unavailable");
        id = next_session_creation_id_++;
        pending_session_creations_.emplace(id, source);
    }
    const auto subscription = cancellation.on_cancel([source]() mutable { source.cancel(); });
    struct Cleanup
    {
        std::function<void()> run;
        ~Cleanup()
        {
            run();
        }
    } cleanup{[&]
              {
                  cancellation.unregister(subscription);
                  std::lock_guard<std::mutex> lock(callbacks_mutex_);
                  pending_session_creations_.erase(id);
              }};
    try
    {
        return handler(request, source.token());
    }
    catch (...)
    {
        throw std::runtime_error("Session creation callback failed");
    }
}

// NODEJS REFERENCE: nodejs/src/session.ts _handleMemoryStore
void Session::handle_memory_store_request(const json& params, const CancellationToken& cancellation)
{
    MemoryStoreHandler handler;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (dispatch_stopped_)
            throw std::runtime_error("Session callbacks retired");
        handler = memory_store_;
    }
    if (!handler)
        throw std::runtime_error("No memory store is registered");
    try
    {
        handler(params.get<StoredMemory>(), cancellation);
    }
    catch (...)
    {
        if (cancellation.is_cancelled())
            throw JsonRpcError(static_cast<JsonRpcErrorCode>(-32800), "Memory store call cancelled");
        throw std::runtime_error("Memory store callback failed");
    }
}

void Session::set_on_disconnected(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    on_disconnected_ = std::move(callback);
}

// =============================================================================
// Exit Plan Mode Handling
// =============================================================================

void Session::register_exit_plan_mode_handler(ExitPlanModeHandler handler)
{
    std::lock_guard<std::mutex> lock(exit_plan_mode_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
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
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    auto_mode_switch_handler_ = std::move(handler);
}

AutoModeSwitchResponse Session::handle_auto_mode_switch_request(const AutoModeSwitchRequest& request
)
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
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
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

    // NODEJS REFERENCE: nodejs/src/session.ts _handleHooksInvoke handlerMap
    try
    {
        const auto run = [&]<typename Input>(const auto& handler) -> json
        {
            if (!handler)
                return nullptr;
            const auto result = (*handler)(input.get<Input>(), invocation);
            if (!result)
                return nullptr;
            json output;
            to_json(output, *result);
            return output;
        };
        if (hook_type == "preToolUse")
            return run.template operator()<PreToolUseHookInput>(hooks->on_pre_tool_use);
        if (hook_type == "preMcpToolCall")
            return run.template operator()<PreMcpToolCallHookInput>(hooks->on_pre_mcp_tool_call);
        if (hook_type == "postToolUse")
            return run.template operator()<PostToolUseHookInput>(hooks->on_post_tool_use);
        if (hook_type == "postToolUseFailure")
            return run.template operator()<PostToolUseFailureHookInput>(
                hooks->on_post_tool_use_failure
            );
        if (hook_type == "userPromptSubmitted")
            return run.template operator()<UserPromptSubmittedHookInput>(
                hooks->on_user_prompt_submitted
            );
        if (hook_type == "userPromptTransformed")
            return run.template operator()<UserPromptTransformedHookInput>(
                hooks->on_user_prompt_transformed
            );
        if (hook_type == "sessionStart")
            return run.template operator()<SessionStartHookInput>(hooks->on_session_start);
        if (hook_type == "sessionEnd")
            return run.template operator()<SessionEndHookInput>(hooks->on_session_end);
        if (hook_type == "errorOccurred")
            return run.template operator()<ErrorOccurredHookInput>(hooks->on_error_occurred);
        if (hook_type == "agentStop")
            return run.template operator()<AgentStopHookInput>(hooks->on_agent_stop);
        if (hook_type == "subagentStart")
            return run.template operator()<SubagentStartHookInput>(hooks->on_subagent_start);
        if (hook_type == "subagentStop")
            return run.template operator()<SubagentStopHookInput>(hooks->on_subagent_stop);
    }
    catch (const std::exception&)
    {
        // NODEJS REFERENCE: session.ts _handleHooksInvoke - a failing hook (including
        // unknown or newly-null payload fields) returns undefined instead of erroring.
        return nullptr;
    }

    return nullptr;
}

// =============================================================================
// Lifecycle
// =============================================================================

// C# REFERENCE: dotnet/src/Session.cs Retire / SetToolsAsync / skill-provider callbacks.
void Session::retire()
{
    if (dispatch_stopped_.exchange(true))
        return;
    cancel_pending_tool_invocations();
    std::map<std::string, CancellationSource> pending_tools;
    {
        std::lock_guard<std::mutex> lock(pending_external_tools_mutex_);
        pending_tools.swap(pending_external_tools_);
    }
    for (auto& [_, source] : pending_tools)
        source.cancel();
    std::map<int, std::function<void()>> waiters;
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        event_handlers_.clear();
        waiters.swap(retirement_handlers_);
    }
    for (const auto& [id, wake] : waiters)
        wake();
    std::function<void()> on_disconnected;
    std::map<uint64_t, CancellationSource> pending_creations;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        permission_handler_ = {};
        permission_handler_with_context_ = {};
        mcp_auth_handler_ = {};
        skill_provider_.reset();
        commands_.clear();
        bearer_token_providers_.clear();
        shell_authentication_handler_ = {};
        memory_store_ = {};
        session_creation_handler_ = {};
        ++session_creation_generation_;
        pending_creations.swap(pending_session_creations_);
        transform_callbacks_.clear();
        session_fs_handler_ = {};
        on_disconnected.swap(on_disconnected_);
    }
    for (auto& [_, source] : pending_creations)
        source.cancel();
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        tools_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(hooks_mutex_);
        hooks_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(user_input_mutex_);
        user_input_handler_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(elicitation_mutex_);
        elicitation_handler_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(exit_plan_mode_mutex_);
        exit_plan_mode_handler_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(auto_mode_switch_mutex_);
        auto_mode_switch_handler_ = {};
    }
    if (on_disconnected)
    {
        try
        {
            on_disconnected();
        }
        catch (...)
        {
        }
    }
}

void Session::register_skill_provider(std::shared_ptr<SkillProvider> provider)
{
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    if (dispatch_stopped_)
        throw std::runtime_error("Session callbacks retired");
    skill_provider_ = std::move(provider);
}

json Session::handle_skill_provider_request(
    const std::string& method, const json& params, const CancellationToken& cancellation
)
{
    std::shared_ptr<SkillProvider> provider;
    {
        std::lock_guard<std::mutex> lock(callbacks_mutex_);
        if (dispatch_stopped_)
            throw std::runtime_error("Session callbacks retired");
        provider = skill_provider_;
    }
    if (!provider)
        throw std::runtime_error("No skill provider for session");
    if (cancellation.is_cancelled())
        throw JsonRpcError(static_cast<JsonRpcErrorCode>(-32800), "Request cancelled");
    if (method == "skillProvider.list")
        return {{"skills", provider->list_skills()}};
    const auto name = params.at("name").get<std::string>();
    if (name.empty())
        throw std::invalid_argument("Missing skill name");
    const auto value = provider->read_skill(name);
    return {{"markdown", value ? json(*value) : json(nullptr)}};
}

std::future<void> Session::set_tools(std::vector<Tool> tools)
{
    auto self = shared_from_this();
    return std::async(
        std::launch::async,
        [self, tools = std::move(tools)]() mutable
        {
            std::lock_guard<std::mutex> serial(self->set_tools_mutex_);
            if (self->dispatch_stopped_)
                throw std::runtime_error("Session closed");
            json definitions = json::array();
            std::map<std::string, Tool> replacements;
            for (const auto& tool : tools)
            {
                definitions.push_back(build_tool_definition(tool));
                replacements[tool.name] = tool;
            }
            auto rpc = self->client_->rpc_client_shared();
            rpc->invoke_with_completion(
                   "session.tools.set",
                   {{"sessionId", self->session_id_}, {"tools", definitions}},
                   [self, replacements = std::move(replacements)](const json&) mutable
                   {
                       std::lock_guard<std::mutex> lock(self->tools_mutex_);
                       if (!self->dispatch_stopped_)
                           self->tools_.swap(replacements);
                   }
            ).get();
        }
    );
}

std::future<Session::DetachResult> Session::detach()
{
    return std::async(
        std::launch::async,
        [this]() -> DetachResult
        {
            json params;
            params["sessionId"] = session_id_;

            const json response = client_->rpc_client_shared()
                                      ->invoke(copilot::rpc::methods::kSessionDetach, params)
                                      .get();

            DetachResult result;
            result.raw = response;
            if (response.is_object())
            {
                if (const auto it = response.find("success");
                    it != response.end() && it->is_boolean())
                    result.success = it->get<bool>();
                if (const auto it = response.find("error"); it != response.end() && it->is_string())
                    result.error = it->get<std::string>();
            }
            return result;
        }
    );
}

std::future<void> Session::destroy()
{
    retire();
    return std::async(
        std::launch::async,
        [this]()
        {
            // A declined detach is deliberately not raised: this runs on the
            // teardown path, and failing it would leave callers unable to close
            // a session the runtime already considers gone. Callers who need the
            // status call detach() directly.
            (void)detach().get();
        }
    );
}

std::future<json> Session::invoke(const std::string& method, json params)
{
    return std::async(
        std::launch::async,
        [this, method, params = std::move(params)]() mutable
        {
            if (dispatch_stopped_)
                throw std::runtime_error("Session closed");
            if (!params.is_object())
                throw std::invalid_argument("session RPC params must be a JSON object");
            params["sessionId"] = session_id_;
            return client_->rpc_client_shared()->invoke(method, params).get();
        }
    );
}

std::future<generated::api::McpServerList> Session::list_mcp_servers()
{
    return invoke_typed<generated::api::McpServerList>("session.mcp.list");
}

std::future<generated::api::AuthIdentity> Session::get_current_auth_info()
{
    return invoke_typed<generated::api::AuthIdentity>("session.gitHubAuth.getCurrentAuthInfo");
}

std::future<generated::api::AgentList> Session::list_agents(json params)
{
    return invoke_typed<generated::api::AgentList>("session.agent.list", std::move(params));
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
    return invoke_typed<generated::api::CommandList>("session.commands.list", std::move(params));
}

std::future<generated::api::HistoryListRewindPointsResult> Session::list_rewind_points()
{
    return invoke_typed<generated::api::HistoryListRewindPointsResult>(
        "session.history.listRewindPoints"
    );
}

std::future<generated::api::UsageGetMetricsResult> Session::get_usage_metrics()
{
    return invoke_typed<generated::api::UsageGetMetricsResult>("session.usage.getMetrics");
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
        std::launch::async, [future = invoke("session.remote.disable")]() mutable { future.get(); }
    );
}

SessionWorkflowApi Session::workflow()
{
    return SessionWorkflowApi(*this);
}

SessionUiApi Session::ui()
{
    return SessionUiApi(*this);
}

std::future<void> Session::log(std::string message, LogOptions options)
{
    return std::async(
        std::launch::async,
        [this, message = std::move(message), options = std::move(options)]()
        {
            json params{{"sessionId", session_id_}, {"message", message}};
            if (options.level)
                params["level"] = *options.level;
            if (options.ephemeral)
                params["ephemeral"] = *options.ephemeral;
            if (options.url)
                params["url"] = *options.url;
            client_->rpc_client_shared()
                ->invoke(copilot::rpc::methods::kSessionLog, params)
                .get();
        }
    );
}

// =============================================================================
// SessionUiApi
// =============================================================================
// NODEJS REFERENCE: nodejs/src/session.ts _elicitation/_confirm/_select/_input

namespace
{
void assert_elicitation(const Session& session)
{
    if (!session.capabilities().has("ui.elicitation"))
        throw std::runtime_error(
            "Elicitation is not supported by the host. "
            "Check session capabilities ui.elicitation before calling UI methods."
        );
}

ElicitationResult request_elicitation(Session& session, std::string message, json schema)
{
    json params{{"message", std::move(message)}, {"requestedSchema", std::move(schema)}};
    return session.invoke(copilot::rpc::methods::kSessionUiElicitation, std::move(params))
        .get()
        .get<ElicitationResult>();
}

std::optional<std::string> accepted_string(const ElicitationResult& result, const char* key)
{
    if (result.action != ElicitationAction::Accept || !result.content)
        return std::nullopt;
    const auto it = result.content->find(key);
    if (it == result.content->end() || it->second.is_null())
        return std::nullopt;
    return it->second.get<std::string>();
}
} // namespace

std::future<ElicitationResult> SessionUiApi::elicitation(ElicitationParams params)
{
    assert_elicitation(session_);
    return std::async(
        std::launch::async,
        [&session = session_, params = std::move(params)]()
        { return request_elicitation(session, params.message, json(params.requested_schema)); }
    );
}

std::future<bool> SessionUiApi::confirm(std::string message)
{
    assert_elicitation(session_);
    return std::async(
        std::launch::async,
        [&session = session_, message = std::move(message)]()
        {
            json schema{
                {"type", "object"},
                {"properties", {{"confirmed", {{"type", "boolean"}, {"default", true}}}}},
                {"required", json::array({"confirmed"})},
            };
            const auto result = request_elicitation(session, message, std::move(schema));
            if (result.action != ElicitationAction::Accept || !result.content)
                return false;
            const auto it = result.content->find("confirmed");
            return it != result.content->end() && it->second.is_boolean() &&
                   it->second.get<bool>();
        }
    );
}

std::future<std::optional<std::string>>
SessionUiApi::select(std::string message, std::vector<std::string> options)
{
    assert_elicitation(session_);
    return std::async(
        std::launch::async,
        [&session = session_, message = std::move(message), options = std::move(options)]()
        {
            json schema{
                {"type", "object"},
                {"properties", {{"selection", {{"type", "string"}, {"enum", options}}}}},
                {"required", json::array({"selection"})},
            };
            return accepted_string(
                request_elicitation(session, message, std::move(schema)), "selection"
            );
        }
    );
}

std::future<std::optional<std::string>>
SessionUiApi::input(std::string message, UiInputOptions options)
{
    assert_elicitation(session_);
    return std::async(
        std::launch::async,
        [&session = session_, message = std::move(message), options = std::move(options)]()
        {
            json field{{"type", "string"}};
            if (options.title && !options.title->empty())
                field["title"] = *options.title;
            if (options.description && !options.description->empty())
                field["description"] = *options.description;
            if (options.min_length)
                field["minLength"] = *options.min_length;
            if (options.max_length)
                field["maxLength"] = *options.max_length;
            if (options.format && !options.format->empty())
                field["format"] = *options.format;
            if (options.default_value)
                field["default"] = *options.default_value;
            json schema{
                {"type", "object"},
                {"properties", {{"value", std::move(field)}}},
                {"required", json::array({"value"})},
            };
            return accepted_string(
                request_elicitation(session, message, std::move(schema)), "value"
            );
        }
    );
}

std::future<generated::api::SandboxEnforcementStatus> Session::get_sandbox_enforcement_status()
{
    return invoke_typed<generated::api::SandboxEnforcementStatus>(
        "session.sandbox.getEnforcementStatus"
    );
}

// =============================================================================
// Model & Mode (v0.1.49 additions)
// =============================================================================

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

namespace
{
Session::AutoTierResult parse_auto_tier_result(const json& response)
{
    Session::AutoTierResult result;
    result.raw = response;
    if (response.is_object())
    {
        if (const auto it = response.find("status"); it != response.end() && it->is_string())
            result.status = it->get<std::string>();
        if (const auto it = response.find("effectiveAutoTier"); it != response.end())
            result.effective_auto_tier = auto_tier_from_wire(*it);
        if (const auto it = response.find("pendingAutoTier"); it != response.end())
            result.pending_auto_tier = auto_tier_from_wire(*it);
    }
    return result;
}
} // namespace

namespace
{
const char* fusion_tier_wire(Session::FusionTier tier)
{
    switch (tier)
    {
    case Session::FusionTier::Standard:
        return "standard";
    case Session::FusionTier::High:
        return "high";
    case Session::FusionTier::Max:
        return "max";
    }
    return "standard";
}
} // namespace

std::future<void> Session::set_model(const std::string& model_id)
{
    return set_model(model_id, SetModelOptions{});
}

// C# REFERENCE: dotnet/src/Session.cs SetModelAsync(string, SetModelOptions)
// NODEJS REFERENCE: nodejs/src/session.ts setModel
std::future<void> Session::set_model(const std::string& model_id, SetModelOptions options)
{
    if (options.auto_tier && options.reset_auto_tier)
        throw std::invalid_argument("auto_tier and reset_auto_tier are mutually exclusive");
    if (options.fusion_tier && (options.auto_tier || options.reset_auto_tier))
        throw std::invalid_argument(
            "fusion_tier cannot be combined with auto_tier or reset_auto_tier"
        );

    return std::async(
        std::launch::async,
        [this, model_id, options = std::move(options)]()
        {
            json params;
            params["sessionId"] = session_id_;
            params["modelId"] = model_id;
            if (options.reasoning_effort)
                params["reasoningEffort"] = *options.reasoning_effort;
            if (options.reasoning_summary)
                params["reasoningSummary"] = *options.reasoning_summary;
            if (options.context_tier)
                params["contextTier"] = *options.context_tier;
            if (options.model_capabilities)
                params["modelCapabilities"] = *options.model_capabilities;
            if (options.fusion_tier)
                params["fusionTier"] = fusion_tier_wire(*options.fusion_tier);
            else if (options.reset_auto_tier)
                params["autoTier"] = nullptr;
            else if (options.auto_tier)
                params["autoTier"] = auto_tier_wire(*options.auto_tier);

            client_->rpc_client_shared()
                ->invoke(copilot::rpc::methods::kSessionModelSwitchTo, params)
                .get();
        }
    );
}

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
                client_->rpc_client_shared()
                    ->invoke(copilot::rpc::methods::kSessionModelSwitchAutoTier, params)
                    .get();

            return parse_auto_tier_result(response);
        }
    );
}

std::future<Session::AutoTierResult> Session::set_fusion_tier(FusionTier fusion_tier)
{
    return std::async(
        std::launch::async,
        [this, fusion_tier]() -> AutoTierResult
        {
            json params{{"sessionId", session_id_}, {"fusionTier", fusion_tier_wire(fusion_tier)}};
            const json response =
                client_->rpc_client_shared()
                    ->invoke(copilot::rpc::methods::kSessionModelSwitchFusionTier, params)
                    .get();
            return parse_auto_tier_result(response);
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
            auto response = client_->rpc_client_shared()
                                ->invoke(copilot::rpc::methods::kSessionModelGetCurrent, params)
                                .get();
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
    case Session::Mode::Interactive:
        return "interactive";
    case Session::Mode::Plan:
        return "plan";
    case Session::Mode::Autopilot:
        return "autopilot";
    }
    return "interactive";
}

std::optional<Session::Mode> mode_from_wire(const std::string& s)
{
    if (s == "interactive")
        return Session::Mode::Interactive;
    if (s == "plan")
        return Session::Mode::Plan;
    if (s == "autopilot")
        return Session::Mode::Autopilot;
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
            client_->rpc_client_shared()
                ->invoke(copilot::rpc::methods::kSessionModeSet, params)
                .get();
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
            auto response = client_->rpc_client_shared()
                                ->invoke(copilot::rpc::methods::kSessionModeGet, params)
                                .get();
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
