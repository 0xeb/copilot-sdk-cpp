// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file user_input.cpp
/// @brief Example demonstrating the user input handler for interactive prompts
///
/// This example shows how to:
/// 1. Register a UserInputHandler on session creation
/// 2. Handle choice-based prompts from the agent (multiple choice)
/// 3. Handle freeform text input requests
/// 4. The agent can use the ask_user tool to request user input during execution

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

        std::cout << "=== User Input Handler Example ===\n\n";
        std::cout << "When the agent needs user input, it will invoke the ask_user tool.\n";
        std::cout << "Your handler receives the question and optional choices,\n";
        std::cout << "and returns the user's answer.\n\n";

        client.start().get();

        copilot::CreateSessionOptions config;

        // Register the user input handler
        // This is called when the agent uses the ask_user tool
        config.interaction.on_user_input_request = [](const copilot::UserInputRequest& request,
                                       const copilot::UserInputInvocation&)
            -> copilot::UserInputResponse
        {
            std::cout << "\n╔══════════════════════════════════════╗\n";
            std::cout << "║       AGENT ASKS FOR INPUT           ║\n";
            std::cout << "╚══════════════════════════════════════╝\n";
            std::cout << "\nQuestion: " << request.question << "\n";

            // Check if the agent provided choices
            if (request.choices.has_value() && !request.choices->empty())
            {
                std::cout << "\nChoices:\n";
                for (size_t i = 0; i < request.choices->size(); i++)
                    std::cout << "  [" << (i + 1) << "] " << (*request.choices)[i] << "\n";

                std::cout << "\nEnter choice number (or type a custom answer): ";
                std::string input;
                std::getline(std::cin, input);

                copilot::UserInputResponse response;

                // Try to parse as a number
                try
                {
                    int choice = std::stoi(input);
                    if (choice >= 1 && choice <= static_cast<int>(request.choices->size()))
                    {
                        response.answer = (*request.choices)[choice - 1];
                        response.was_freeform = false;
                        std::cout << "Selected: " << response.answer << "\n";
                        return response;
                    }
                }
                catch (...) {}

                // Treat as freeform input
                response.answer = input;
                response.was_freeform = true;
                return response;
            }
            else
            {
                // Freeform input (no choices provided)
                std::cout << "\nYour answer: ";
                std::string input;
                std::getline(std::cin, input);

                copilot::UserInputResponse response;
                response.answer = input;
                response.was_freeform = true;
                return response;
            }
        };

        // Permission handler
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

        std::cout << "Try asking the agent to make decisions that require your input.\n";
        std::cout << "For example: 'Ask me what programming language I prefer'\n\n> ";

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
