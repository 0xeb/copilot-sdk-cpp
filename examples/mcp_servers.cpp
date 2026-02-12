// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file mcp_servers.cpp
/// @brief Example demonstrating MCP server configuration
///
/// This example shows how to:
/// 1. Configure local (stdio) MCP servers
/// 2. Configure remote (HTTP/SSE) MCP servers
/// 3. Use multiple MCP servers in a single session
/// 4. Restrict which tools are exposed from MCP servers

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

        std::cout << "=== MCP Servers Example ===\n\n";
        std::cout << "This example demonstrates configuring MCP (Model Context Protocol) servers.\n";
        std::cout << "MCP servers extend Copilot's capabilities with external tools.\n\n";

        client.start().get();

        // ---------------------------------------------------------------------
        // Configure MCP Servers
        // ---------------------------------------------------------------------

        std::map<std::string, copilot::json> mcp_servers;

        // Example 1: Local filesystem MCP server (stdio-based)
        // This server provides file system tools
        copilot::McpLocalServerConfig filesystem_server;
        filesystem_server.type = "local";
        filesystem_server.command = "npx";
        filesystem_server.args = {"-y", "@anthropic/mcp-server-filesystem", "/tmp"};
        filesystem_server.tools = {"*"};  // Expose all tools from this server
        filesystem_server.timeout = 30000;  // 30 second timeout

        mcp_servers["filesystem"] = filesystem_server;

        // Example 2: Local MCP server with environment variables
        copilot::McpLocalServerConfig custom_server;
        custom_server.type = "local";
        custom_server.command = "python";
        custom_server.args = {"-m", "my_mcp_server"};
        custom_server.tools = {"my_tool_1", "my_tool_2"};  // Only expose specific tools
        custom_server.env = std::map<std::string, std::string>{
            {"API_KEY", "your-api-key"},
            {"DEBUG", "true"}
        };
        custom_server.cwd = "/path/to/server";

        // Uncomment to add this server:
        // mcp_servers["custom"] = custom_server;

        // Example 3: Remote HTTP MCP server
        copilot::McpRemoteServerConfig remote_server;
        remote_server.type = "http";
        remote_server.url = "https://api.example.com/mcp";
        remote_server.tools = {"*"};
        remote_server.timeout = 60000;  // 60 second timeout
        remote_server.headers = std::map<std::string, std::string>{
            {"Authorization", "Bearer your-token"},
            {"X-Custom-Header", "value"}
        };

        // Uncomment to add this server:
        // mcp_servers["remote-api"] = remote_server;

        // Example 4: SSE (Server-Sent Events) MCP server
        copilot::McpRemoteServerConfig sse_server;
        sse_server.type = "sse";
        sse_server.url = "https://api.example.com/mcp/sse";
        sse_server.tools = {"stream_tool"};

        // Uncomment to add this server:
        // mcp_servers["sse-server"] = sse_server;

        // ---------------------------------------------------------------------
        // Create Session with MCP Servers
        // ---------------------------------------------------------------------

        copilot::SessionConfig config;
        config.mcp_servers = mcp_servers;

        std::cout << "Configured MCP servers:\n";
        for (const auto& [name, _] : mcp_servers)
            std::cout << "  - " << name << "\n";
        std::cout << "\n";

        std::cout << "Note: This example configures a filesystem MCP server.\n";
        std::cout << "Make sure you have the MCP server installed:\n";
        std::cout << "  npm install -g @anthropic/mcp-server-filesystem\n\n";

        auto session = client.create_session(config).get();
        std::cout << "Session created: " << session->session_id() << "\n\n";

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
                else if (auto* tool_start = event.try_as<copilot::ToolExecutionStartData>())
                {
                    std::cout << "\n[MCP Tool: " << tool_start->tool_name << "]\n";
                    if (tool_start->arguments)
                        std::cout << "  Args: " << tool_start->arguments->dump(2) << "\n";
                }
                else if (auto* tool_complete = event.try_as<copilot::ToolExecutionCompleteData>())
                {
                    std::cout << "[Tool Complete]\n";
                    if (tool_complete->result)
                        std::cout << "  Result: " << tool_complete->result->content.substr(0, 200)
                                  << (tool_complete->result->content.length() > 200 ? "..." : "")
                                  << "\n";
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
        std::cout << "Try commands that use MCP server tools.\n";
        std::cout << "With filesystem server, try:\n";
        std::cout << "  - 'List files in /tmp'\n";
        std::cout << "  - 'Read the contents of /tmp/test.txt'\n";
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
