// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file compaction_events.cpp
/// @brief Example demonstrating session compaction and usage monitoring
///
/// This example shows how to:
/// 1. Configure infinite sessions with a low compaction threshold
/// 2. Subscribe to SessionCompactionStart, SessionCompactionComplete events
/// 3. Monitor context usage via SessionUsageInfo events
/// 4. Track compaction progress in real-time

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
        options.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "=== Compaction Events Example ===\n\n";
        std::cout << "This example monitors context compaction in real-time.\n";
        std::cout << "When the context window fills up, the session automatically\n";
        std::cout << "compacts (summarizes) older messages to free up space.\n\n";

        client.start().get();

        // Configure session with a low compaction threshold to trigger compaction sooner
        copilot::SessionConfig config;
        config.streaming = true;

        copilot::InfiniteSessionConfig infinite_config;
        infinite_config.enabled = true;
        infinite_config.background_compaction_threshold = 0.10; // Trigger at 10% usage
        config.infinite_sessions = infinite_config;

        auto session = client.create_session(config).get();
        std::cout << "Session created with low compaction threshold (10%)\n";
        std::cout << "Compaction events will appear when context usage exceeds the threshold.\n\n";

        // Synchronization
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};

        // Subscribe to all events, highlighting compaction-related ones
        auto subscription = session->on(
            [&](const copilot::SessionEvent& event)
            {
                if (auto* delta = event.try_as<copilot::AssistantMessageDeltaData>())
                {
                    std::cout << delta->delta_content << std::flush;
                }
                else if (auto* msg = event.try_as<copilot::AssistantMessageData>())
                {
                    // Final message in non-streaming mode
                    if (!msg->content.empty())
                        std::cout << "\nAssistant: " << msg->content << "\n";
                }
                else if (auto* usage = event.try_as<copilot::SessionUsageInfoData>())
                {
                    // Context usage statistics
                    double pct = (usage->token_limit > 0)
                        ? (usage->current_tokens / usage->token_limit * 100.0)
                        : 0.0;
                    std::cout << "\n[Usage] Tokens: "
                              << static_cast<int>(usage->current_tokens)
                              << "/" << static_cast<int>(usage->token_limit)
                              << " (" << static_cast<int>(pct) << "%)"
                              << "  Messages: " << static_cast<int>(usage->messages_length)
                              << "\n";
                }
                else if (event.try_as<copilot::SessionCompactionStartData>())
                {
                    std::cout << "\n*** COMPACTION STARTED ***\n"
                              << "    Context is being summarized to free up space...\n";
                }
                else if (auto* complete = event.try_as<copilot::SessionCompactionCompleteData>())
                {
                    std::cout << "\n*** COMPACTION COMPLETE ***\n";
                    std::cout << "    Success: " << (complete->success ? "yes" : "no") << "\n";

                    if (complete->error)
                        std::cout << "    Error: " << *complete->error << "\n";

                    if (complete->pre_compaction_tokens && complete->post_compaction_tokens)
                    {
                        std::cout << "    Tokens: "
                                  << static_cast<int>(*complete->pre_compaction_tokens)
                                  << " -> "
                                  << static_cast<int>(*complete->post_compaction_tokens)
                                  << "\n";
                    }

                    if (complete->pre_compaction_messages_length
                        && complete->post_compaction_messages_length)
                    {
                        std::cout << "    Messages: "
                                  << static_cast<int>(*complete->pre_compaction_messages_length)
                                  << " -> "
                                  << static_cast<int>(*complete->post_compaction_messages_length)
                                  << "\n";
                    }

                    if (complete->compaction_tokens_used)
                    {
                        auto& tokens = *complete->compaction_tokens_used;
                        std::cout << "    Compaction cost: in="
                                  << static_cast<int>(tokens.input)
                                  << " out=" << static_cast<int>(tokens.output)
                                  << " cached=" << static_cast<int>(tokens.cached_input)
                                  << "\n";
                    }
                }
                else if (auto* error = event.try_as<copilot::SessionErrorData>())
                {
                    std::cerr << "\nError: " << error->message << "\n";
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
        std::cout << "Send multiple messages to build up context and trigger compaction.\n";
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
