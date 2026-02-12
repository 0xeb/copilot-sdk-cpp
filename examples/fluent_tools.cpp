// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file fluent_tools.cpp
/// @brief Demonstrates the fluent tool builder API with headless E2E test
///
/// This example shows the modern C++ way to define tools using the fluent builder.
/// Compare with tools.cpp which shows the traditional verbose approach.
///
/// Key benefits:
/// - Automatic JSON schema generation from C++ types
/// - Type-safe parameter extraction
/// - Concise, readable syntax
/// - Compile-time type checking

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
#include <vector>

using namespace copilot;

// =============================================================================
// Tool Definitions using Fluent Builder
// =============================================================================

// Calculator tool - compare with 80+ lines in tools.cpp!
auto make_calculator()
{
    return ToolBuilder("calculate", "Perform basic arithmetic operations")
        .param<double>("a", "First operand")
        .param<double>("b", "Second operand")
        .param<std::string>("operation", "The operation to perform")
        .one_of("add", "subtract", "multiply", "divide", "power")
        .handler([](double a, double b, const std::string& op) -> std::string {
            double result;
            std::string symbol;

            if (op == "add")
            {
                result = a + b;
                symbol = "+";
            }
            else if (op == "subtract")
            {
                result = a - b;
                symbol = "-";
            }
            else if (op == "multiply")
            {
                result = a * b;
                symbol = "*";
            }
            else if (op == "divide")
            {
                if (b == 0)
                    throw std::runtime_error("Division by zero");
                result = a / b;
                symbol = "/";
            }
            else if (op == "power")
            {
                result = std::pow(a, b);
                symbol = "^";
            }
            else
            {
                throw std::runtime_error("Unknown operation: " + op);
            }

            std::ostringstream oss;
            oss << a << " " << symbol << " " << b << " = " << result;
            return oss.str();
        });
}

// Get time tool - with optional timezone parameter
auto make_get_time()
{
    return ToolBuilder("get_time", "Get the current date and time")
        .param<std::string>("timezone", "Timezone (e.g., 'UTC', 'local')")
        .default_value(std::string("local"))
        .handler([](const std::string& tz) -> std::string {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            std::string time_str = std::ctime(&time_t);
            if (!time_str.empty() && time_str.back() == '\n')
                time_str.pop_back();
            return "Current time (" + tz + "): " + time_str;
        });
}

// Echo tool - simplest possible example
auto make_echo()
{
    return ToolBuilder("echo", "Echo back the input message")
        .param<std::string>("message", "The message to echo")
        .handler([](const std::string& msg) { return "Echo: " + msg; });
}

// Random number tool - demonstrates multiple params with defaults
auto make_random()
{
    return ToolBuilder("random", "Generate a random number in a range")
        .param<int>("min", "Minimum value (inclusive)")
        .default_value(0)
        .param<int>("max", "Maximum value (inclusive)")
        .default_value(100)
        .handler([](int min_val, int max_val) -> std::string {
            int range = max_val - min_val + 1;
            int result = min_val + (std::rand() % range);
            return std::to_string(result);
        });
}

// =============================================================================
// Main - Headless E2E Test
// =============================================================================

int main()
{
    std::cout << "=== Fluent Tool Builder E2E Example ===\n\n";

    try
    {
        // Create tools using the fluent API
        auto calculator = make_calculator();
        auto get_time = make_get_time();
        auto echo = make_echo();
        auto random = make_random();

        // Print generated schemas
        std::cout << "Generated tool schemas:\n\n";
        std::cout << "1. " << calculator.name << ":\n";
        std::cout << calculator.parameters_schema.dump(2) << "\n\n";
        std::cout << "2. " << get_time.name << ":\n";
        std::cout << get_time.parameters_schema.dump(2) << "\n\n";

        // =================================================================
        // Connect to Copilot CLI
        // =================================================================

        std::cout << "=== Starting Copilot Session ===\n\n";

        ClientOptions opts;
        opts.log_level = LogLevel::Info;
        opts.use_stdio = true;
        Client client(opts);

        std::cout << "Connecting to Copilot CLI...\n";
        client.start().get();
        std::cout << "Connected!\n\n";

        SessionConfig config;
        config.tools = {calculator, get_time, echo, random};

        auto session = client.create_session(config).get();
        std::cout << "Session created: " << session->session_id() << "\n";
        std::cout << "Registered " << config.tools.size() << " fluent tools\n\n";

        // Event handling
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};
        std::string last_response;

        auto sub = session->on(
            [&](const SessionEvent& event)
            {
                if (auto* msg = event.try_as<AssistantMessageData>())
                {
                    std::cout << "Assistant: " << msg->content << "\n";
                    std::lock_guard<std::mutex> lock(mtx);
                    last_response = msg->content;
                }
                else if (auto* tool_start = event.try_as<ToolExecutionStartData>())
                {
                    std::cout << "[Tool: " << tool_start->tool_name << "]";
                    if (tool_start->arguments)
                        std::cout << " args=" << tool_start->arguments->dump();
                    std::cout << "\n";
                }
                else if (auto* tool_complete = event.try_as<ToolExecutionCompleteData>())
                {
                    std::cout << "[Result: ";
                    if (tool_complete->result)
                        std::cout << tool_complete->result->content;
                    std::cout << "]\n";
                }
                else if (event.type == SessionEventType::SessionIdle)
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    idle = true;
                    cv.notify_one();
                }
            });

        // Helper to send message and wait for response
        auto send_and_wait = [&](const std::string& prompt) {
            std::cout << "\n--- Prompt: " << prompt << " ---\n";
            idle = false;
            last_response.clear();

            MessageOptions msg;
            msg.prompt = prompt;
            session->send(msg).get();

            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&]() { return idle.load(); });
        };

        // =================================================================
        // Headless E2E Test: Send hardcoded messages
        // =================================================================

        std::cout << "=== Running Headless E2E Tests ===\n";

        // Test 1: Calculator - multiplication
        send_and_wait("What is 123 times 456? Use the calculate tool.");

        // Test 2: Calculator - division
        send_and_wait("Divide 1000 by 8 using the calculate tool.");

        // Test 3: Get time
        send_and_wait("What time is it right now? Use the get_time tool.");

        // Test 4: Echo
        send_and_wait("Echo the message 'Hello from fluent tools!'");

        // Test 5: Random number
        send_and_wait("Generate a random number between 1 and 10.");

        // Test 6: Calculator - power
        send_and_wait("Calculate 2 to the power of 10 using the calculate tool.");

        // =================================================================
        // Cleanup
        // =================================================================

        std::cout << "\n=== Fluent Tools E2E Complete ===\n";

        session->destroy().get();
        client.stop().get();

        std::cout << "Session destroyed, client stopped.\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        std::cerr << "Make sure Copilot CLI is installed and accessible.\n";
        return 1;
    }

    return 0;
}
