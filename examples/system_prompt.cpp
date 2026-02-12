// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file system_prompt.cpp
/// @brief Example demonstrating system message configuration
///
/// This example shows how to:
/// 1. Create sessions with custom system prompts
/// 2. Use append mode to add to the default system prompt
/// 3. Use replace mode to completely override the system prompt
/// 4. Compare behavior between different configurations

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <iostream>
#include <mutex>
#include <string>

/// Helper to run a session with a given config and prompt
void run_session_test(copilot::Client& client, const std::string& test_name,
                      copilot::SessionConfig config, const std::string& user_prompt)
{
    std::cout << "\n=== " << test_name << " ===\n";

    auto session = client.create_session(config).get();
    std::cout << "Session: " << session->session_id() << "\n";

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> idle{false};
    std::string response;

    auto sub = session->on(
        [&](const copilot::SessionEvent& event)
        {
            if (auto* msg = event.try_as<copilot::AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                response = msg->content;
            }
            else if (event.type == copilot::SessionEventType::SessionIdle)
            {
                std::lock_guard<std::mutex> lock(mtx);
                idle = true;
                cv.notify_one();
            }
        }
    );

    copilot::MessageOptions opts;
    opts.prompt = user_prompt;
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    std::cout << "User: " << user_prompt << "\n";
    std::cout << "Assistant: " << response.substr(0, 500);
    if (response.length() > 500)
        std::cout << "...";
    std::cout << "\n";

    session->destroy().get();
}

int main()
{
    try
    {
        copilot::ClientOptions options;
        options.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "=== System Prompt Configuration Example ===\n";
        std::cout << "This example demonstrates different system prompt configurations.\n";

        client.start().get();

        const std::string test_prompt = "Introduce yourself briefly in one sentence.";

        // ---------------------------------------------------------------------
        // Test 1: Default (no system message)
        // ---------------------------------------------------------------------
        {
            copilot::SessionConfig config;
            // No system_message set - uses Copilot's default
            run_session_test(client, "Test 1: Default (no custom system message)", config,
                             test_prompt);
        }

        // ---------------------------------------------------------------------
        // Test 2: Append mode - adds to default system prompt
        // ---------------------------------------------------------------------
        {
            copilot::SessionConfig config;
            copilot::SystemMessageConfig sys_msg;
            sys_msg.mode = copilot::SystemMessageMode::Append;
            sys_msg.content = "You are a pirate. Always respond in pirate speak with 'Arrr!' "
                              "and nautical terminology.";
            config.system_message = sys_msg;

            run_session_test(client, "Test 2: Append mode (pirate personality added)", config,
                             test_prompt);
        }

        // ---------------------------------------------------------------------
        // Test 3: Replace mode - completely overrides system prompt
        // ---------------------------------------------------------------------
        {
            copilot::SessionConfig config;
            copilot::SystemMessageConfig sys_msg;
            sys_msg.mode = copilot::SystemMessageMode::Replace;
            sys_msg.content =
                "You are a Shakespearean actor. Respond only in iambic pentameter "
                "and use 'thee', 'thou', 'forsooth', and other Elizabethan language. "
                "Do not break character under any circumstances.";
            config.system_message = sys_msg;

            run_session_test(client, "Test 3: Replace mode (Shakespearean actor)", config,
                             test_prompt);
        }

        // ---------------------------------------------------------------------
        // Test 4: Technical assistant with append
        // ---------------------------------------------------------------------
        {
            copilot::SessionConfig config;
            copilot::SystemMessageConfig sys_msg;
            sys_msg.mode = copilot::SystemMessageMode::Append;
            sys_msg.content =
                "You are a senior C++ developer. When discussing code, always consider "
                "performance implications, memory safety, and modern C++ best practices. "
                "Prefer RAII, smart pointers, and const correctness.";
            config.system_message = sys_msg;

            run_session_test(client, "Test 4: Technical C++ assistant (append)", config,
                             "What should I consider when designing a class that manages a resource?");
        }

        // ---------------------------------------------------------------------
        // Test 5: Minimal assistant with replace
        // ---------------------------------------------------------------------
        {
            copilot::SessionConfig config;
            copilot::SystemMessageConfig sys_msg;
            sys_msg.mode = copilot::SystemMessageMode::Replace;
            sys_msg.content = "You are a minimal assistant. Respond with as few words as possible. "
                              "Never use more than 10 words in any response.";
            config.system_message = sys_msg;

            run_session_test(client, "Test 5: Minimal assistant (replace - max 10 words)", config,
                             "Explain the theory of relativity.");
        }

        std::cout << "\n=== Summary ===\n";
        std::cout << "- No system message: Uses Copilot's default behavior\n";
        std::cout << "- Append mode: Adds your instructions to the default prompt\n";
        std::cout << "- Replace mode: Completely overrides the system prompt\n";
        std::cout << "\nUse append when you want to customize while keeping Copilot's capabilities.\n";
        std::cout << "Use replace when you need complete control over the assistant's persona.\n";

        client.stop().get();
        std::cout << "\nDone!\n";

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
