// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file byok.cpp
/// @brief Bring Your Own Key (BYOK) example demonstrating custom provider configuration

#include <atomic>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

int main(int argc, char* argv[])
{
    // Check for required environment variables or command-line args
    std::string api_key;
    std::string base_url = "https://api.openai.com/v1"; // Default to OpenAI
    std::string model = "gpt-4";

    // Try environment variables first
    if (const char* env_key = std::getenv("OPENAI_API_KEY"))
        api_key = env_key;
    if (const char* env_url = std::getenv("OPENAI_BASE_URL"))
        base_url = env_url;
    if (const char* env_model = std::getenv("OPENAI_MODEL"))
        model = env_model;

    // Command-line overrides
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--api-key" && i + 1 < argc)
        {
            api_key = argv[++i];
        }
        else if (arg == "--base-url" && i + 1 < argc)
        {
            base_url = argv[++i];
        }
        else if (arg == "--model" && i + 1 < argc)
        {
            model = argv[++i];
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "Usage: " << argv[0] << " [options]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --api-key KEY    API key for the provider\n";
            std::cout
                << "  --base-url URL   Base URL for the API (default: https://api.openai.com/v1)\n";
            std::cout << "  --model MODEL    Model to use (default: gpt-4)\n";
            std::cout << "\nEnvironment variables:\n";
            std::cout << "  OPENAI_API_KEY   API key (overridden by --api-key)\n";
            std::cout << "  OPENAI_BASE_URL  Base URL (overridden by --base-url)\n";
            std::cout << "  OPENAI_MODEL     Model (overridden by --model)\n";
            return 0;
        }
    }

    if (api_key.empty())
    {
        std::cerr << "Error: API key required. Set OPENAI_API_KEY or use --api-key\n";
        return 1;
    }

    try
    {
        std::cout << "=== Copilot SDK BYOK Example ===\n\n";
        std::cout << "Provider configuration:\n";
        std::cout << "  Base URL: " << base_url << "\n";
        std::cout << "  Model: " << model << "\n";
        std::cout << "  API Key: " << std::string(api_key.length() - 4, '*')
                  << api_key.substr(api_key.length() - 4) << "\n\n";

        // Create client
        copilot::ClientOptions options;
        options.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "Starting Copilot client...\n";
        client.start().get();

        // Create session with custom provider
        copilot::SessionConfig session_config;
        session_config.model = model;

        // Configure custom provider
        copilot::ProviderConfig provider;
        provider.base_url = base_url;
        provider.api_key = api_key;
        provider.type = "openai"; // Provider type
        session_config.provider = provider;

        auto session = client.create_session(session_config).get();
        std::cout << "Session created with custom provider\n\n";

        // Synchronization
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};

        // Subscribe to events
        auto subscription = session->on(
            [&](const copilot::SessionEvent& event)
            {
                if (auto* msg = event.try_as<copilot::AssistantMessageData>())
                {
                    std::cout << "\nAssistant: " << msg->content << "\n";
                }
                else if (auto* delta = event.try_as<copilot::AssistantMessageDeltaData>())
                {
                    std::cout << delta->delta_content << std::flush;
                }
                else if (auto* usage = event.try_as<copilot::AssistantUsageData>())
                {
                    std::cout << "\n\n[Usage: ";
                    if (usage->input_tokens)
                        std::cout << "in=" << static_cast<int>(*usage->input_tokens);
                    if (usage->output_tokens)
                        std::cout << " out=" << static_cast<int>(*usage->output_tokens);
                    if (usage->cost)
                        std::cout << " cost=$" << *usage->cost;
                    std::cout << "]\n";
                }
                else if (auto* error = event.try_as<copilot::SessionErrorData>())
                {
                    std::cerr << "\nError: " << error->message << "\n";
                    std::lock_guard<std::mutex> lock(mtx);
                    idle = true;
                    cv.notify_one();
                }
                else if (event.type == copilot::SessionEventType::SessionIdle)
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    idle = true;
                    cv.notify_one();
                }
            }
        );

        // Interactive chat
        std::cout << "Chat with " << model << " using your API key.\n";
        std::cout << "Type 'quit' to exit.\n\n> ";

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

            idle = false;

            copilot::MessageOptions msg_opts;
            msg_opts.prompt = line;
            session->send(msg_opts).get();

            // Wait for response
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&idle]() { return idle.load(); });
            }

            std::cout << "\n> ";
        }

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
