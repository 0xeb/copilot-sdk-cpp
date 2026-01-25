// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file session.hpp
/// @brief CopilotSession for managing conversation sessions

#include <copilot/events.hpp>
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

    /// Create a session (called by Client)
    Session(const std::string& session_id, Client* client,
            const std::optional<std::string>& workspace_path = std::nullopt);

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

    // =========================================================================
    // Permission Handling
    // =========================================================================

    /// Register a permission handler
    /// @param handler Function to call for permission requests
    void register_permission_handler(PermissionHandler handler);

    /// Handle a permission request (called by Client)
    PermissionRequestResult handle_permission_request(const PermissionRequest& request);

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /// Destroy the session on the server
    /// @return Future that completes when destroyed
    std::future<void> destroy();

  private:
    std::string session_id_;
    Client* client_;
    std::optional<std::string> workspace_path_;

    // Event handlers
    mutable std::mutex handlers_mutex_;
    std::vector<std::pair<int, EventHandler>> event_handlers_;
    int next_handler_id_ = 0;

    // Tools
    mutable std::mutex tools_mutex_;
    std::map<std::string, Tool> tools_;

    // Permission handler
    PermissionHandler permission_handler_;
};

} // namespace copilot
