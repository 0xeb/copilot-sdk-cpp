// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file tool_progress.cpp
/// @brief Example demonstrating tool execution progress monitoring
///
/// This example shows how to:
/// 1. Register a custom tool and subscribe to tool lifecycle events
/// 2. Monitor ToolExecutionStart, ToolExecutionProgress, and ToolExecutionComplete
/// 3. Display real-time progress updates during tool execution

#include <atomic>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

// A simulated long-running tool that counts words in text
copilot::ToolResultObject word_count_handler(const copilot::ToolInvocation& invocation)
{
    copilot::ToolResultObject result;

    try
    {
        std::string text = "Hello World";
        if (invocation.arguments.has_value() && invocation.arguments->contains("text"))
            text = (*invocation.arguments)["text"].get<std::string>();

        // Count words
        std::istringstream iss(text);
        std::string word;
        int count = 0;
        while (iss >> word)
            count++;

        std::ostringstream oss;
        oss << "The text contains " << count << " word(s).";
        result.text_result_for_llm = oss.str();
    }
    catch (const std::exception& e)
    {
        result.result_type = copilot::ToolResultType::Failure;
        result.error = e.what();
        result.text_result_for_llm = std::string("Error: ") + e.what();
    }

    return result;
}

// A simulated file search tool
copilot::ToolResultObject search_handler(const copilot::ToolInvocation& invocation)
{
    copilot::ToolResultObject result;

    try
    {
        std::string query = "example";
        if (invocation.arguments.has_value() && invocation.arguments->contains("query"))
            query = (*invocation.arguments)["query"].get<std::string>();

        std::ostringstream oss;
        oss << "Search results for '" << query << "':\n"
            << "  1. README.md - line 42: contains '" << query << "'\n"
            << "  2. main.cpp - line 7: references '" << query << "'\n"
            << "  3. docs/guide.md - line 15: explains '" << query << "'";
        result.text_result_for_llm = oss.str();
    }
    catch (const std::exception& e)
    {
        result.result_type = copilot::ToolResultType::Failure;
        result.error = e.what();
        result.text_result_for_llm = std::string("Error: ") + e.what();
    }

    return result;
}

int main()
{
    try
    {
        copilot::ClientOptions options;
        options.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "=== Tool Execution Progress Example ===\n\n";
        std::cout << "This example shows the full tool lifecycle:\n";
        std::cout << "  ToolExecutionStart -> ToolExecutionProgress -> ToolExecutionComplete\n\n";

        client.start().get();

        // Define custom tools
        copilot::Tool word_count_tool;
        word_count_tool.name = "word_count";
        word_count_tool.description = "Count the number of words in text";
        word_count_tool.parameters_schema = copilot::json{
            {"type", "object"},
            {"properties",
             {{"text", {{"type", "string"}, {"description", "The text to count words in"}}}}},
            {"required", {"text"}}
        };
        word_count_tool.handler = word_count_handler;

        copilot::Tool search_tool;
        search_tool.name = "search_files";
        search_tool.description = "Search for files containing a query string";
        search_tool.parameters_schema = copilot::json{
            {"type", "object"},
            {"properties",
             {{"query", {{"type", "string"}, {"description", "The search query"}}}}},
            {"required", {"query"}}
        };
        search_tool.handler = search_handler;

        // Create session with tools
        copilot::SessionConfig config;
        config.tools = {word_count_tool, search_tool};

        auto session = client.create_session(config).get();
        std::cout << "Session created with 2 tools: word_count, search_files\n\n";

        // Synchronization
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};

        // Subscribe to events — including tool progress
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
                else if (auto* start = event.try_as<copilot::ToolExecutionStartData>())
                {
                    // Tool is starting execution
                    std::cout << "\n[Tool Start] " << start->tool_name;
                    if (start->arguments)
                        std::cout << " | Args: " << start->arguments->dump();
                    std::cout << "\n";
                }
                else if (auto* progress = event.try_as<copilot::ToolExecutionProgressData>())
                {
                    // Progress update during tool execution
                    std::cout << "[Tool Progress] " << progress->tool_call_id
                              << ": " << progress->progress_message << "\n";
                }
                else if (auto* complete = event.try_as<copilot::ToolExecutionCompleteData>())
                {
                    // Tool finished execution
                    std::cout << "[Tool Complete] " << complete->tool_call_id
                              << " | " << (complete->success ? "Success" : "Failed");
                    if (complete->result)
                        std::cout << " | " << complete->result->content;
                    if (complete->error)
                        std::cout << " | Error: " << complete->error->message;
                    std::cout << "\n";
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

        // Interactive loop
        std::cout << "Try asking questions that use the tools!\n";
        std::cout << "Examples:\n";
        std::cout << "  - How many words are in 'The quick brown fox jumps over the lazy dog'?\n";
        std::cout << "  - Search for files containing 'main'\n";
        std::cout << "\nType 'quit' to exit.\n\n> ";

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
