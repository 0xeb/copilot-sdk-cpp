// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file resume_with_tools.cpp
/// @brief Example demonstrating resuming a session with custom tools
///
/// This example shows how to:
/// 1. Create an initial session
/// 2. Save the session ID
/// 3. Resume the session later with new custom tools
/// 4. Use the tools in the resumed session

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <iostream>
#include <mutex>
#include <string>

/// Custom tool handler - returns secret values
copilot::ToolResultObject secret_handler(const copilot::ToolInvocation& invocation)
{
    copilot::ToolResultObject result;

    try
    {
        auto& args = invocation.arguments.value();
        std::string key = args["key"].get<std::string>();

        // Simulated secret store
        std::map<std::string, std::string> secrets = {
            {"API_KEY", "example-api-key"},
            {"DB_PASSWORD", "example-db-password"},
            {"ENCRYPTION_KEY", "example-encryption-key"}
        };

        auto it = secrets.find(key);
        if (it != secrets.end())
        {
            result.text_result_for_llm = "Secret value for '" + key + "': " + it->second;
            result.result_type = copilot::ToolResultType::Success;
        }
        else
        {
            result.text_result_for_llm = "No secret found for key: " + key;
            result.result_type = copilot::ToolResultType::Failure;
            result.error = "Key not found";
        }
    }
    catch (const std::exception& e)
    {
        result.text_result_for_llm = "Error retrieving secret";
        result.result_type = copilot::ToolResultType::Failure;
        result.error = e.what();
    }

    return result;
}

int main()
{
    try
    {
        // Create client
        copilot::ClientOptions options;
        options.log_level = copilot::LogLevel::Info;
        copilot::Client client(options);

        std::cout << "=== Phase 1: Create Initial Session ===\n\n";

        client.start().get();
        auto session1 = client.create_session().get();
        std::string session_id = session1->session_id();
        std::cout << "Created session: " << session_id << "\n";

        // Synchronization
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};

        auto sub1 = session1->on(
            [&](const copilot::SessionEvent& event)
            {
                if (event.type == copilot::SessionEventType::AssistantMessage)
                {
                    auto& data = event.as<copilot::AssistantMessageData>();
                    std::cout << "Assistant: " << data.content << "\n";
                }
                else if (event.type == copilot::SessionEventType::SessionIdle)
                {
                    idle = true;
                    cv.notify_one();
                }
            }
        );

        // Send initial message (no tools available yet)
        copilot::MessageOptions opts;
        opts.prompt = "Hello! Remember that my favorite color is blue.";
        session1->send(opts).get();

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
        }

        std::cout << "\n=== Phase 2: Stop and Resume with Tools ===\n\n";

        // Stop the client (but don't destroy the session)
        client.stop().get();

        // Define our custom tool
        copilot::Tool secret_tool;
        secret_tool.name = "get_secret";
        secret_tool.description = "Retrieve a secret value from the secure store. "
                                  "Available keys: API_KEY, DB_PASSWORD, ENCRYPTION_KEY";
        secret_tool.parameters_schema = copilot::json{
            {"type", "object"},
            {"properties",
             {{"key",
               {{"type", "string"},
                {"description", "The key to look up (e.g., 'API_KEY', 'DB_PASSWORD')"}}}}},
            {"required", {"key"}}
        };
        secret_tool.handler = secret_handler;

        // Restart client and resume session WITH the tool
        client.start().get();

        copilot::ResumeSessionConfig resume_config;
        resume_config.tools = {secret_tool};

        auto session2 = client.resume_session(session_id, resume_config).get();
        std::cout << "Resumed session: " << session2->session_id() << "\n";
        std::cout << "Custom tool 'get_secret' is now available!\n\n";

        // Reset for next message
        idle = false;

        auto sub2 = session2->on(
            [&](const copilot::SessionEvent& event)
            {
                if (event.type == copilot::SessionEventType::AssistantMessage)
                {
                    auto& data = event.as<copilot::AssistantMessageData>();
                    std::cout << "Assistant: " << data.content << "\n";
                }
                else if (event.type == copilot::SessionEventType::ToolExecutionStart)
                {
                    auto& data = event.as<copilot::ToolExecutionStartData>();
                    std::cout << "\n[Tool: " << data.tool_name << "] Starting...\n";
                }
                else if (event.type == copilot::SessionEventType::ToolExecutionComplete)
                {
                    auto& data = event.as<copilot::ToolExecutionCompleteData>();
                    std::cout << "[Tool: " << data.tool_call_id << "] Complete\n";
                    if (data.result.has_value())
                        std::cout << "  Result: " << data.result->content << "\n\n";
                }
                else if (event.type == copilot::SessionEventType::SessionIdle)
                {
                    idle = true;
                    cv.notify_one();
                }
            }
        );

        // Ask about both the remembered info AND use the new tool
        opts.prompt = "First, what's my favorite color? Then, use the get_secret tool to "
                      "retrieve the API_KEY for me.";
        session2->send(opts).get();

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
        }

        std::cout << "\n=== Cleanup ===\n";
        session2->destroy().get();
        client.force_stop();
        std::cout << "Done!\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
