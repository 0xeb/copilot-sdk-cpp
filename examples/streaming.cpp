// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file streaming.cpp
/// @brief Streaming responses example demonstrating real-time output

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <iostream>
#include <mutex>
#include <string>

int main(int argc, char* argv[])
{
    // Get prompt from command line or use default
    std::string prompt = "Write a short poem about programming.";
    if (argc > 1)
        prompt = argv[1];

    try
    {
        // Create client with streaming enabled
        copilot::ClientOptions options;
        options.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "Starting Copilot client...\n";
        client.start().get();

        // Create a streaming session
        copilot::SessionConfig session_config;
        session_config.streaming = true; // Enable streaming

        auto session = client.create_session(session_config).get();
        std::cout << "Session created with streaming enabled\n\n";

        // Synchronization for waiting on completion
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> done{false};
        std::atomic<int> token_count{0};

        // Subscribe to streaming events
        auto subscription = session->on(
            [&](const copilot::SessionEvent& event)
            {
                if (auto* delta = event.try_as<copilot::AssistantMessageDeltaData>())
                {
                    // Print streaming delta content as it arrives
                    std::cout << delta->delta_content << std::flush;
                    token_count++;
                }
                else if (auto* msg = event.try_as<copilot::AssistantMessageData>())
                {
                    // Final message (may contain full content)
                    // In streaming mode, we've already printed the deltas
                }
                else if (auto* usage = event.try_as<copilot::AssistantUsageData>())
                {
                    // Usage statistics
                    std::cout << "\n\n--- Usage Statistics ---\n";
                    if (usage->model)
                        std::cout << "Model: " << *usage->model << "\n";
                    if (usage->input_tokens)
                        std::cout << "Input tokens: " << static_cast<int>(*usage->input_tokens)
                                  << "\n";
                    if (usage->output_tokens)
                    {
                        std::cout << "Output tokens: " << static_cast<int>(*usage->output_tokens)
                                  << "\n";
                    }
                    if (usage->duration)
                        std::cout << "Duration: " << *usage->duration << "ms\n";
                }
                else if (event.type == copilot::SessionEventType::SessionIdle)
                {
                    // Session is idle - response complete
                    std::lock_guard<std::mutex> lock(mtx);
                    done = true;
                    cv.notify_one();
                }
                else if (auto* error = event.try_as<copilot::SessionErrorData>())
                {
                    std::cerr << "\nError: " << error->message << "\n";
                    std::lock_guard<std::mutex> lock(mtx);
                    done = true;
                    cv.notify_one();
                }
            }
        );

        // Send the prompt
        std::cout << "Prompt: " << prompt << "\n";
        std::cout << "---\n\n";

        auto start_time = std::chrono::steady_clock::now();

        copilot::MessageOptions msg_opts;
        msg_opts.prompt = prompt;
        session->send(msg_opts).get();

        // Wait for completion
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&done]() { return done.load(); });
        }

        auto end_time = std::chrono::steady_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        std::cout << "\n--- Stream Statistics ---\n";
        std::cout << "Streamed tokens: " << token_count << "\n";
        std::cout << "Total time: " << duration.count() << "ms\n";

        // Cleanup
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
