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
#include <optional>
#include <string>
#include <vector>

int main(int argc, char* argv[])
{
    // Provider from COPILOT_SDK_BYOK_* environment variables (named "byok"), if configured
    std::optional<copilot::NamedProviderConfig> provider = copilot::ProviderConfig::from_env();
    std::string model = copilot::ProviderConfig::model_from_env().value_or("gpt-4");

    // Command-line overrides
    std::optional<std::string> api_key;
    std::optional<std::string> base_url;
    std::optional<std::string> provider_type;
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
        else if (arg == "--type" && i + 1 < argc)
        {
            provider_type = argv[++i];
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
            std::cout << "  --type TYPE      Provider type: openai | azure | anthropic (default: openai)\n";
            std::cout << "  --model MODEL    Model to use (default: gpt-4)\n";
            std::cout << "\nEnvironment variables (overridden by the options above):\n";
            std::cout << "  " << copilot::ProviderConfig::ENV_API_KEY << "        API key\n";
            std::cout << "  " << copilot::ProviderConfig::ENV_BASE_URL << "       Base URL\n";
            std::cout << "  " << copilot::ProviderConfig::ENV_PROVIDER_TYPE << "  Provider type\n";
            std::cout << "  " << copilot::ProviderConfig::ENV_MODEL << "          Model\n";
            return 0;
        }
    }

    if (!provider && api_key)
    {
        provider = copilot::NamedProviderConfig{};
        provider->name = copilot::ProviderConfig::ENV_PROVIDER_NAME;
        provider->type = "openai";
        provider->base_url = "https://api.openai.com/v1";
    }
    if (!provider)
    {
        std::cerr << "Error: API key required. Set " << copilot::ProviderConfig::ENV_API_KEY
                  << " or use --api-key\n";
        return 1;
    }
    if (api_key)
        provider->api_key = *api_key;
    if (base_url)
        provider->base_url = *base_url;
    if (provider_type)
        provider->type = *provider_type;

    try
    {
        const std::string key = provider->api_key.value_or("");
        const size_t shown = key.size() > 4 ? 4 : 0;
        std::cout << "=== Copilot SDK BYOK Example ===\n\n";
        std::cout << "Provider configuration:\n";
        std::cout << "  Name: " << provider->name << "\n";
        std::cout << "  Type: " << provider->type.value_or("openai") << "\n";
        std::cout << "  Base URL: " << provider->base_url << "\n";
        std::cout << "  Model: " << model << "\n";
        std::cout << "  API Key: " << std::string(key.size() - shown, '*')
                  << key.substr(key.size() - shown) << "\n\n";

        // Create client
        copilot::ClientOptions options;
        options.observability.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "Starting Copilot client...\n";
        client.start().get();

        // Create session with the named provider and make it the default
        copilot::CreateSessionOptions session_config;
        session_config.model = model;
        session_config.inference.providers = std::vector<copilot::NamedProviderConfig>{*provider};
        session_config.inference.default_provider = provider->name;

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
