// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file basic_chat.cpp
/// @brief Simple Q&A example demonstrating basic Copilot SDK usage

#include <copilot/copilot.hpp>
#include <iostream>
#include <string>

int main()
{
    try
    {
        // Create client with default options (uses stdio transport)
        copilot::ClientOptions options;
        options.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "Starting Copilot client...\n";
        client.start().get();
        std::cout << "Connected!\n";

        // Create a session
        copilot::SessionConfig session_config;
        session_config.model = "gpt-4"; // Optional: specify model

        auto session = client.create_session(session_config).get();
        std::cout << "Session created: " << session->session_id() << "\n";

        // Subscribe to events
        auto subscription = session->on(
            [](const copilot::SessionEvent& event)
            {
                // Handle different event types
                if (auto* msg = event.try_as<copilot::AssistantMessageData>())
                    std::cout << "\nAssistant: " << msg->content << "\n";
                else if (auto* delta = event.try_as<copilot::AssistantMessageDeltaData>())
                    std::cout << delta->delta_content << std::flush;
                else if (auto* error = event.try_as<copilot::SessionErrorData>())
                    std::cerr << "Error: " << error->message << "\n";
                else if (event.type == copilot::SessionEventType::SessionIdle)
                    std::cout << "\n> " << std::flush;
            }
        );

        // Interactive chat loop
        std::cout << "\nEnter your messages (type 'quit' to exit):\n> ";
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line == "quit" || line == "exit")
                break;

            if (line.empty())
            {
                std::cout << "> ";
                continue;
            }

            // Send message
            copilot::MessageOptions msg_opts;
            msg_opts.prompt = line;

            auto message_id = session->send(msg_opts).get();
            std::cout << "Message sent (ID: " << message_id << ")\n";
        }

        // Cleanup
        std::cout << "\nDestroying session...\n";
        session->destroy().get();

        std::cout << "Stopping client...\n";
        client.stop().get();

        std::cout << "Done!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
