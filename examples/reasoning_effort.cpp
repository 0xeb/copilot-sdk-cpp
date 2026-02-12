// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file reasoning_effort.cpp
/// @brief Example demonstrating reasoning effort configuration
///
/// This example shows how to:
/// 1. List available models and check which support reasoning effort
/// 2. Create a session with a specific reasoning effort level
/// 3. Compare responses at different reasoning effort levels
/// 4. Query model capabilities for supported reasoning efforts

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <iostream>
#include <mutex>
#include <string>

int main()
{
    try
    {
        copilot::ClientOptions options;
        copilot::Client client(options);

        std::cout << "=== Reasoning Effort Example ===\n\n";
        std::cout << "Reasoning effort controls how much 'thinking' a model does.\n";
        std::cout << "Values: low, medium, high, xhigh\n\n";

        client.start().get();

        // List models and show which support reasoning effort
        std::cout << "--- Available Models ---\n";
        auto models = client.list_models().get();

        for (const auto& model : models)
        {
            std::cout << "  " << model.id;

            if (model.capabilities.supports.reasoning_effort)
                std::cout << " [reasoning effort: YES]";

            if (model.supported_reasoning_efforts.has_value())
            {
                std::cout << " (levels: ";
                for (size_t i = 0; i < model.supported_reasoning_efforts->size(); i++)
                {
                    if (i > 0) std::cout << ", ";
                    std::cout << (*model.supported_reasoning_efforts)[i];
                }
                std::cout << ")";
            }

            if (model.default_reasoning_effort.has_value())
                std::cout << " [default: " << *model.default_reasoning_effort << "]";

            std::cout << "\n";
        }

        std::cout << "\n";

        // Create session with reasoning effort
        copilot::SessionConfig config;
        config.reasoning_effort = copilot::ReasoningEffort::Medium;

        // Permission handler
        config.on_permission_request = [](const copilot::PermissionRequest&)
            -> copilot::PermissionRequestResult
        {
            copilot::PermissionRequestResult r;
            r.kind = "approved";
            return r;
        };

        auto session = client.create_session(config).get();
        std::cout << "Session created with reasoning_effort = 'medium'\n";
        std::cout << "Session ID: " << session->session_id() << "\n\n";

        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};

        auto sub = session->on(
            [&](const copilot::SessionEvent& event)
            {
                if (auto* msg = event.try_as<copilot::AssistantMessageData>())
                    std::cout << "\nAssistant: " << msg->content << "\n";
                else if (auto* usage = event.try_as<copilot::AssistantUsageData>())
                {
                    std::cout << "[Usage:";
                    if (usage->input_tokens.has_value())
                        std::cout << " " << *usage->input_tokens << " in";
                    if (usage->output_tokens.has_value())
                        std::cout << " / " << *usage->output_tokens << " out";
                    if (usage->cost.has_value())
                        std::cout << " / cost=" << *usage->cost;
                    std::cout << "]\n";
                }
                else if (event.type == copilot::SessionEventType::SessionIdle)
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    idle = true;
                    cv.notify_one();
                }
            }
        );

        std::cout << "Chat with reasoning effort enabled. Type 'quit' to exit.\n\n> ";

        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line == "quit" || line == "exit")
                break;
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
