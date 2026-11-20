// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file hooks.cpp
/// @brief Example demonstrating the hooks system for tool lifecycle interception
///
/// This example shows how to:
/// 1. Register PreToolUse hooks to inspect/modify/deny tool calls
/// 2. Register PostToolUse hooks to inspect/modify tool results
/// 3. Register session lifecycle hooks (start, end, error)
/// 4. Combine multiple hooks for comprehensive observability

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

struct HookLog
{
    std::string hook_type;
    std::string detail;
};

std::vector<HookLog> g_hook_log;
std::mutex g_log_mutex;

void log_hook(const std::string& type, const std::string& detail)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    g_hook_log.push_back({type, detail});
    std::cout << "[HOOK:" << type << "] " << detail << "\n";
}

int main()
{
    try
    {
        copilot::ClientOptions options;
        copilot::Client client(options);

        std::cout << "=== Hooks System Example ===\n\n";
        std::cout << "Demonstrates PreToolUse, PostToolUse, and session lifecycle hooks.\n\n";

        client.start().get();

        copilot::CreateSessionOptions config;

        // Set up hooks
        config.hooks.handlers = copilot::SessionHooks{};

        // ---- PreToolUse Hook ----
        // Fires BEFORE every tool call. Can inspect args, allow/deny, or modify args.
        config.hooks.handlers->on_pre_tool_use = [](const copilot::PreToolUseHookInput& input,
                                           const copilot::HookInvocation&)
            -> std::optional<copilot::PreToolUseHookOutput>
        {
            log_hook("PreToolUse", "Tool: " + input.tool_name);

            // Example: deny any tool named "dangerous_tool"
            if (input.tool_name == "dangerous_tool")
            {
                copilot::PreToolUseHookOutput output;
                output.permission_decision = "deny";
                output.permission_decision_reason = "This tool is blocked by policy";
                log_hook("PreToolUse", "DENIED: " + input.tool_name);
                return output;
            }

            // Allow all other tools (return nullopt = no modification)
            return std::nullopt;
        };

        // ---- PostToolUse Hook ----
        // Fires AFTER every tool call. Can inspect/modify the result.
        config.hooks.handlers->on_post_tool_use = [](const copilot::PostToolUseHookInput& input,
                                            const copilot::HookInvocation&)
            -> std::optional<copilot::PostToolUseHookOutput>
        {
            std::string result_str = input.tool_result.has_value()
                ? input.tool_result->dump().substr(0, 100)
                : "(no result)";
            log_hook("PostToolUse", "Tool: " + input.tool_name + " => " + result_str);

            // Example: add context to the result
            copilot::PostToolUseHookOutput output;
            output.additional_context = "Tool " + input.tool_name + " completed successfully";
            return output;
        };

        // ---- SessionStart Hook ----
        // Fires when the session starts.
        config.hooks.handlers->on_session_start = [](const copilot::SessionStartHookInput&,
                                            const copilot::HookInvocation&)
            -> std::optional<copilot::SessionStartHookOutput>
        {
            log_hook("SessionStart", "Session is starting");
            return std::nullopt;
        };

        // ---- SessionEnd Hook ----
        // Fires when the session ends.
        config.hooks.handlers->on_session_end = [](const copilot::SessionEndHookInput&,
                                          const copilot::HookInvocation&)
            -> std::optional<copilot::SessionEndHookOutput>
        {
            log_hook("SessionEnd", "Session is ending");
            return std::nullopt;
        };

        // ---- ErrorOccurred Hook ----
        // Fires on errors during the session.
        config.hooks.handlers->on_error_occurred = [](const copilot::ErrorOccurredHookInput& input,
                                             const copilot::HookInvocation&)
            -> std::optional<copilot::ErrorOccurredHookOutput>
        {
            log_hook("ErrorOccurred", input.error + " (context: " + input.error_context + ")");
            return std::nullopt;
        };

        // Permission handler is still needed for tool execution approval
        config.permissions.on_request = [](const copilot::PermissionRequest&)
            -> copilot::PermissionRequestResult
        {
            copilot::PermissionRequestResult r;
            r.kind = "approved";
            return r;
        };

        auto session = client.create_session(config).get();
        std::cout << "Session created: " << session->session_id() << "\n\n";

        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};

        auto sub = session->on(
            [&](const copilot::SessionEvent& event)
            {
                if (auto* msg = event.try_as<copilot::AssistantMessageData>())
                    std::cout << "\nAssistant: " << msg->content << "\n";
                else if (event.type == copilot::SessionEventType::SessionIdle)
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    idle = true;
                    cv.notify_one();
                }
            }
        );

        // Interactive loop
        std::cout << "Chat with hooks enabled. Type 'log' to see hook log, 'quit' to exit.\n\n> ";

        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line == "quit" || line == "exit")
                break;

            if (line == "log")
            {
                std::lock_guard<std::mutex> lock(g_log_mutex);
                std::cout << "\n=== Hook Log (" << g_hook_log.size() << " entries) ===\n";
                for (const auto& entry : g_hook_log)
                    std::cout << "  [" << entry.hook_type << "] " << entry.detail << "\n";
                std::cout << "==============================\n\n> ";
                continue;
            }

            if (line.empty()) { std::cout << "> "; continue; }

            idle = false;
            copilot::MessageOptions msg;
            msg.prompt = line;
            session->send(msg).get();

            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&idle]() { return idle.load(); });
            }
            std::cout << "\n> ";
        }

        // Final hook log
        std::cout << "\n=== Final Hook Log ===\n";
        {
            std::lock_guard<std::mutex> lock(g_log_mutex);
            for (const auto& entry : g_hook_log)
                std::cout << "  [" << entry.hook_type << "] " << entry.detail << "\n";
        }
        std::cout << "======================\n";

        session->destroy().get();
        client.stop().get();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
