// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file permission_callback.cpp
/// @brief Example demonstrating permission callbacks for tool execution control
///
/// This example shows how to:
/// 1. Register a permission callback to control tool execution
/// 2. Allow or deny tool calls based on custom logic
/// 3. Log tool usage for auditing
/// 4. Implement safety checks for dangerous operations

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <iostream>
#include <mutex>
#include <set>
#include <string>

// Track tool usage for auditing
struct ToolUsageLog
{
    std::string tool_name;
    std::string timestamp;
    bool allowed;
    std::string reason;
};

std::vector<ToolUsageLog> g_tool_usage_log;
std::mutex g_log_mutex;

void log_tool_usage(const std::string& tool_name, bool allowed, const std::string& reason)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::string timestamp = std::ctime(&time_t);
    if (!timestamp.empty() && timestamp.back() == '\n')
        timestamp.pop_back();

    g_tool_usage_log.push_back({tool_name, timestamp, allowed, reason});

    std::cout << "[PERMISSION] " << timestamp << " - " << tool_name << ": "
              << (allowed ? "ALLOWED" : "DENIED") << " (" << reason << ")\n";
}

int main()
{
    try
    {
        // Define which tools are considered dangerous
        std::set<std::string> dangerous_tools = {"Bash", "bash", "Write", "write", "Edit", "edit"};

        // Define which tools are always allowed
        std::set<std::string> safe_tools = {"Read", "read", "Glob", "glob", "Grep", "grep"};

        // Track if user has given blanket approval
        std::atomic<bool> user_approved_dangerous{false};

        // Create client
        copilot::ClientOptions options;
        options.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "=== Permission Callback Example ===\n\n";
        std::cout << "This example demonstrates controlling tool execution via permission callbacks.\n";
        std::cout << "- Safe tools (read, glob, grep): Always allowed\n";
        std::cout << "- Dangerous tools (bash, write, edit): Require approval\n\n";

        client.start().get();

        // Create session config with permission handler
        copilot::SessionConfig config;

        // Register the permission callback
        config.on_permission_request = [&](const copilot::PermissionRequest& request)
            -> copilot::PermissionRequestResult
        {
            copilot::PermissionRequestResult result;

            // Extract tool name from the request
            std::string tool_name = "unknown";
            if (request.extension_data.count("toolName"))
                tool_name = request.extension_data.at("toolName").get<std::string>();

            // Check if it's a safe tool - always allow
            if (safe_tools.count(tool_name))
            {
                result.kind = "approved";
                log_tool_usage(tool_name, true, "Safe tool - auto-approved");
                return result;
            }

            // Check if it's a dangerous tool
            if (dangerous_tools.count(tool_name))
            {
                // If user has given blanket approval, allow it
                if (user_approved_dangerous)
                {
                    result.kind = "approved";
                    log_tool_usage(tool_name, true, "Dangerous tool - user pre-approved");
                    return result;
                }

                // Otherwise, deny with explanation
                result.kind = "denied-no-approval-rule-and-could-not-request-from-user";
                log_tool_usage(tool_name, false, "Dangerous tool - requires user approval");
                return result;
            }

            // Unknown tools - allow by default but log
            result.kind = "approved";
            log_tool_usage(tool_name, true, "Unknown tool - allowed by default");
            return result;
        };

        auto session = client.create_session(config).get();
        std::cout << "Session created: " << session->session_id() << "\n\n";

        // Synchronization
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};

        auto subscription = session->on(
            [&](const copilot::SessionEvent& event)
            {
                if (auto* msg = event.try_as<copilot::AssistantMessageData>())
                {
                    std::cout << "\nAssistant: " << msg->content << "\n";
                }
                else if (auto* tool_start = event.try_as<copilot::ToolExecutionStartData>())
                {
                    std::cout << "\n[Tool Starting: " << tool_start->tool_name << "]\n";
                }
                else if (auto* tool_complete = event.try_as<copilot::ToolExecutionCompleteData>())
                {
                    std::cout << "[Tool Complete: " << tool_complete->tool_call_id << "]\n";
                }
                else if (auto* error = event.try_as<copilot::SessionErrorData>())
                {
                    std::cerr << "Error: " << error->message << "\n";
                }
                else if (event.type == copilot::SessionEventType::SessionIdle)
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    idle = true;
                    cv.notify_one();
                }
            }
        );

        // Interactive loop
        std::cout << "Commands:\n";
        std::cout << "  'approve' - Give blanket approval for dangerous tools\n";
        std::cout << "  'revoke'  - Revoke dangerous tool approval\n";
        std::cout << "  'log'     - Show tool usage log\n";
        std::cout << "  'quit'    - Exit\n";
        std::cout << "\nTry asking Copilot to read a file (allowed) or run a command (denied).\n\n> ";

        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line == "quit" || line == "exit")
                break;

            if (line == "approve")
            {
                user_approved_dangerous = true;
                std::cout << "\n[Dangerous tools are now APPROVED]\n\n> ";
                continue;
            }

            if (line == "revoke")
            {
                user_approved_dangerous = false;
                std::cout << "\n[Dangerous tool approval REVOKED]\n\n> ";
                continue;
            }

            if (line == "log")
            {
                std::lock_guard<std::mutex> lock(g_log_mutex);
                std::cout << "\n=== Tool Usage Log ===\n";
                if (g_tool_usage_log.empty())
                {
                    std::cout << "(no tool usage recorded)\n";
                }
                else
                {
                    for (const auto& entry : g_tool_usage_log)
                    {
                        std::cout << entry.timestamp << " | " << entry.tool_name << " | "
                                  << (entry.allowed ? "ALLOWED" : "DENIED") << " | " << entry.reason
                                  << "\n";
                    }
                }
                std::cout << "======================\n\n> ";
                continue;
            }

            if (line.empty())
            {
                std::cout << "> ";
                continue;
            }

            idle = false;

            copilot::MessageOptions msg_opts;
            msg_opts.prompt = line;
            session->send(msg_opts).get();

            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&idle]() { return idle.load(); });
            }

            std::cout << "\n> ";
        }

        // Show final log
        std::cout << "\n=== Final Tool Usage Log ===\n";
        {
            std::lock_guard<std::mutex> lock(g_log_mutex);
            for (const auto& entry : g_tool_usage_log)
            {
                std::cout << entry.timestamp << " | " << entry.tool_name << " | "
                          << (entry.allowed ? "ALLOWED" : "DENIED") << " | " << entry.reason << "\n";
            }
        }
        std::cout << "============================\n";

        // Cleanup
        std::cout << "\nCleaning up...\n";
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
