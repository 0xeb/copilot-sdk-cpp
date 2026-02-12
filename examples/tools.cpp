// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file tools.cpp
/// @brief Custom tools example demonstrating tool registration and invocation

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

// Define custom tool handlers

/// Calculate tool - performs basic arithmetic
copilot::ToolResultObject calculate_handler(const copilot::ToolInvocation& invocation)
{
    copilot::ToolResultObject result;

    try
    {
        auto& args = invocation.arguments.value();
        std::string operation = args["operation"].get<std::string>();
        double a = args["a"].get<double>();
        double b = args["b"].get<double>();

        double answer;
        if (operation == "add")
        {
            answer = a + b;
        }
        else if (operation == "subtract")
        {
            answer = a - b;
        }
        else if (operation == "multiply")
        {
            answer = a * b;
        }
        else if (operation == "divide")
        {
            if (b == 0)
            {
                result.result_type = copilot::ToolResultType::Failure;
                result.error = "Division by zero";
                result.text_result_for_llm = "Error: Cannot divide by zero";
                return result;
            }
            answer = a / b;
        }
        else if (operation == "power")
        {
            answer = std::pow(a, b);
        }
        else
        {
            result.result_type = copilot::ToolResultType::Failure;
            result.error = "Unknown operation: " + operation;
            result.text_result_for_llm = "Error: Unknown operation '" + operation + "'";
            return result;
        }

        std::ostringstream oss;
        std::string op_symbol = (operation == "power") ? "^" : operation;
        oss << a << " " << op_symbol << " " << b << " = " << answer;
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

/// Get current time tool
copilot::ToolResultObject get_time_handler(const copilot::ToolInvocation& invocation)
{
    copilot::ToolResultObject result;

    try
    {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::string time_str = std::ctime(&time_t);

        // Remove trailing newline
        if (!time_str.empty() && time_str.back() == '\n')
            time_str.pop_back();

        std::string timezone = "local";
        if (invocation.arguments.has_value() && invocation.arguments->contains("timezone"))
            timezone = (*invocation.arguments)["timezone"].get<std::string>();

        std::ostringstream oss;
        oss << "Current time (" << timezone << "): " << time_str;
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

/// Echo tool - simple echo for testing
copilot::ToolResultObject echo_handler(const copilot::ToolInvocation& invocation)
{
    copilot::ToolResultObject result;

    std::string message = "Hello from echo tool!";
    if (invocation.arguments.has_value() && invocation.arguments->contains("message"))
        message = (*invocation.arguments)["message"].get<std::string>();

    result.text_result_for_llm = "Echo: " + message;
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

        std::cout << "Starting Copilot client...\n";
        client.start().get();

        // Define custom tools BEFORE creating the session
        // (tools are sent to the server during session creation)

        // Calculator tool
        copilot::Tool calc_tool;
        calc_tool.name = "calculate";
        calc_tool.description =
            "Perform basic arithmetic operations (add, subtract, multiply, divide, power)";
        calc_tool.parameters_schema = copilot::json{
            {"type", "object"},
            {"properties",
             {{"operation",
               {{"type", "string"},
                {"enum", {"add", "subtract", "multiply", "divide", "power"}},
                {"description", "The arithmetic operation to perform"}}},
              {"a", {{"type", "number"}, {"description", "First operand"}}},
              {"b", {{"type", "number"}, {"description", "Second operand"}}}}},
            {"required", {"operation", "a", "b"}}
        };
        calc_tool.handler = calculate_handler;

        // Time tool
        copilot::Tool time_tool;
        time_tool.name = "get_current_time";
        time_tool.description = "Get the current date and time";
        time_tool.parameters_schema = copilot::json{
            {"type", "object"},
            {"properties",
             {{"timezone",
               {{"type", "string"}, {"description", "Timezone (optional, defaults to local)"}}}}}
        };
        time_tool.handler = get_time_handler;

        // Echo tool
        copilot::Tool echo_tool;
        echo_tool.name = "echo";
        echo_tool.description = "Echo back a message";
        echo_tool.parameters_schema = copilot::json{
            {"type", "object"},
            {"properties", {{"message", {{"type", "string"}, {"description", "Message to echo"}}}}}
        };
        echo_tool.handler = echo_handler;

        // Create session with custom tools
        copilot::SessionConfig session_config;
        session_config.tools = {calc_tool, time_tool, echo_tool};
        auto session = client.create_session(session_config).get();
        std::cout << "Session created: " << session->session_id() << "\n";
        std::cout << "Registered 3 custom tools: calculate, get_current_time, echo\n\n";

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
                else if (auto* tool_start = event.try_as<copilot::ToolExecutionStartData>())
                {
                    std::cout << "\n[Tool: " << tool_start->tool_name << "] Starting...\n";
                    if (tool_start->arguments)
                        std::cout << "  Args: " << tool_start->arguments->dump() << "\n";
                }
                else if (auto* tool_complete = event.try_as<copilot::ToolExecutionCompleteData>())
                {
                    std::cout << "[Tool: " << tool_complete->tool_call_id << "] ";
                    if (tool_complete->success)
                    {
                        std::cout << "Success\n";
                        if (tool_complete->result)
                            std::cout << "  Result: " << tool_complete->result->content << "\n";
                    }
                    else
                    {
                        std::cout << "Failed\n";
                        if (tool_complete->error)
                            std::cout << "  Error: " << tool_complete->error->message << "\n";
                    }
                }
                else if (auto* error = event.try_as<copilot::SessionErrorData>())
                {
                    std::cerr << "Error: " << error->message << "\n";
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
        std::cout << "Try asking questions that require calculations or time!\n";
        std::cout << "Examples:\n";
        std::cout << "  - What is 42 * 17?\n";
        std::cout << "  - What time is it?\n";
        std::cout << "  - Calculate 2^10\n";
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

            // Wait for idle
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
