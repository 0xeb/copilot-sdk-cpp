// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file custom_agents.cpp
/// @brief Example demonstrating custom agent configuration
///
/// This example shows how to:
/// 1. Define custom agents with specific roles and capabilities
/// 2. Restrict agents to specific tools
/// 3. Configure multiple agents in a single session
/// 4. Use agent-specific MCP servers

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

        std::cout << "=== Custom Agents Example ===\n\n";
        std::cout << "This example demonstrates defining custom agents with specific roles.\n";
        std::cout << "Custom agents can have restricted tools, specific prompts, and MCP servers.\n\n";

        client.start().get();

        // ---------------------------------------------------------------------
        // Define Custom Agents
        // ---------------------------------------------------------------------

        // Agent 1: Code Reviewer - focused on reviewing code quality
        copilot::CustomAgentConfig code_reviewer;
        code_reviewer.name = "code-reviewer";
        code_reviewer.display_name = "Code Reviewer";
        code_reviewer.description = "Reviews code for bugs, security issues, and best practices";
        code_reviewer.prompt =
            "You are a senior code reviewer with expertise in security and performance. "
            "When reviewing code:\n"
            "1. Look for potential bugs and edge cases\n"
            "2. Check for security vulnerabilities (SQL injection, XSS, buffer overflows)\n"
            "3. Evaluate code readability and maintainability\n"
            "4. Suggest performance improvements where applicable\n"
            "5. Verify proper error handling\n"
            "Always be constructive and explain the 'why' behind your suggestions.";
        code_reviewer.tools = std::vector<std::string>{"Read", "Glob", "Grep"};  // Read-only tools
        code_reviewer.infer = true;  // Allow automatic agent selection

        // Agent 2: Documentation Writer - focused on writing docs
        copilot::CustomAgentConfig doc_writer;
        doc_writer.name = "doc-writer";
        doc_writer.display_name = "Documentation Writer";
        doc_writer.description = "Writes and improves documentation for code and APIs";
        doc_writer.prompt =
            "You are a technical documentation specialist. Your role is to:\n"
            "1. Write clear, concise documentation\n"
            "2. Create helpful code examples\n"
            "3. Document APIs with proper parameter descriptions\n"
            "4. Write README files and getting started guides\n"
            "5. Use proper markdown formatting\n"
            "Focus on making documentation accessible to developers of all skill levels.";
        doc_writer.tools = std::vector<std::string>{"Read", "Write", "Glob"};
        doc_writer.infer = true;

        // Agent 3: Test Writer - focused on writing tests
        copilot::CustomAgentConfig test_writer;
        test_writer.name = "test-writer";
        test_writer.display_name = "Test Writer";
        test_writer.description = "Writes unit tests and integration tests";
        test_writer.prompt =
            "You are a QA engineer specializing in test automation. Your role is to:\n"
            "1. Write comprehensive unit tests with good coverage\n"
            "2. Create meaningful test cases that cover edge cases\n"
            "3. Follow testing best practices (AAA pattern, isolation, etc.)\n"
            "4. Write clear test descriptions\n"
            "5. Ensure tests are maintainable and not flaky\n"
            "Use the appropriate testing framework for the project.";
        test_writer.tools = std::vector<std::string>{"Read", "Write", "Glob", "Grep", "Bash"};
        test_writer.infer = true;

        // Agent 4: Security Auditor - restricted, no inference
        copilot::CustomAgentConfig security_auditor;
        security_auditor.name = "security-auditor";
        security_auditor.display_name = "Security Auditor";
        security_auditor.description = "Audits code for security vulnerabilities";
        security_auditor.prompt =
            "You are a security expert conducting a code audit. Focus on:\n"
            "1. OWASP Top 10 vulnerabilities\n"
            "2. Input validation and sanitization\n"
            "3. Authentication and authorization flaws\n"
            "4. Cryptographic issues\n"
            "5. Sensitive data exposure\n"
            "Report findings with severity levels (Critical, High, Medium, Low).";
        security_auditor.tools = std::vector<std::string>{"Read", "Glob", "Grep"};  // Read-only
        security_auditor.infer = false;  // Must be explicitly invoked

        // ---------------------------------------------------------------------
        // Create Session with Custom Agents
        // ---------------------------------------------------------------------

        copilot::SessionConfig config;
        config.custom_agents = std::vector<copilot::CustomAgentConfig>{
            code_reviewer,
            doc_writer,
            test_writer,
            security_auditor
        };

        auto session = client.create_session(config).get();
        std::cout << "Session created with 4 custom agents:\n";
        std::cout << "  - code-reviewer: Reviews code quality (read-only tools)\n";
        std::cout << "  - doc-writer: Writes documentation (can write files)\n";
        std::cout << "  - test-writer: Writes tests (full tool access)\n";
        std::cout << "  - security-auditor: Security audits (read-only, no auto-inference)\n\n";

        // Synchronization
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};

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
                else if (auto* tool_start = event.try_as<copilot::ToolExecutionStartData>())
                {
                    std::cout << "\n[Agent using tool: " << tool_start->tool_name << "]\n";
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
        std::cout << "You can now interact with the session. The system will automatically\n";
        std::cout << "route requests to the appropriate agent based on your query.\n\n";
        std::cout << "Examples:\n";
        std::cout << "  - 'Review this code for bugs' -> code-reviewer\n";
        std::cout << "  - 'Write documentation for...' -> doc-writer\n";
        std::cout << "  - 'Write tests for...' -> test-writer\n";
        std::cout << "  - '@security-auditor check for vulnerabilities' -> explicit agent\n";
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
