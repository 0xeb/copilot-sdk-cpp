// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/client.hpp>
#include <copilot/rpc_methods.hpp>
#include <copilot/session.hpp>
#include <condition_variable>

namespace copilot
{

// =============================================================================
// Constructor / Destructor
// =============================================================================

Session::Session(const std::string& session_id, Client* client,
                 const std::optional<std::string>& workspace_path)
    : session_id_(session_id), client_(client), workspace_path_(workspace_path)
{
}

Session::~Session()
{
    // Note: We don't automatically destroy the session on destruction
    // because the user might want to resume it later.
    // Call destroy() explicitly if you want to remove it from the server.
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
            json params;
            params["sessionId"] = session_id_;
            params["prompt"] = options.prompt;

            if (options.attachments.has_value())
                params["attachments"] = *options.attachments;
            if (options.mode.has_value())
                params["mode"] = *options.mode;

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

void Session::dispatch_event(const SessionEvent& event)
{
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

// =============================================================================
// Permission Handling
// =============================================================================

void Session::register_permission_handler(PermissionHandler handler)
{
    permission_handler_ = std::move(handler);
}

PermissionRequestResult Session::handle_permission_request(const PermissionRequest& request)
{
    if (permission_handler_)
        return permission_handler_(request);

    // Default deny if no handler registered
    PermissionRequestResult result;
    result.kind = "denied-no-approval-rule-and-could-not-request-from-user";
    return result;
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
