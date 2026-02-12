// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file mcp_inprocess.cpp
/// @brief In-process Copilot SDK + fastmcpp MCP server integration example
///
/// This example demonstrates running a fastmcpp MCP server and Copilot SDK
/// client in the same process. The fastmcpp server provides custom tools
/// that Copilot can invoke.
///
/// Architecture:
/// - fastmcpp SSE server runs in a background thread on localhost:18080
/// - Copilot SDK connects to it as a remote MCP server
/// - Copilot can invoke tools defined in the fastmcpp server
///
/// Build:
///   cmake -B build -DCOPILOT_WITH_FASTMCPP=ON
///   cmake --build build
///
/// This proves interoperability between the C++ MCP server (fastmcpp)
/// and the Copilot CLI, all orchestrated from a single executable.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

// fastmcpp includes
#include "fastmcpp/app.hpp"
#include "fastmcpp/mcp/handler.hpp"
#include "fastmcpp/server/sse_server.hpp"

// Copilot SDK includes
#include <copilot/copilot.hpp>

using fastmcpp::FastMCP;
using fastmcpp::Json;
using fastmcpp::server::SseServerWrapper;

// =============================================================================
// Constants
// =============================================================================

constexpr int MCP_SERVER_PORT = 18080;
constexpr const char* MCP_SERVER_HOST = "127.0.0.1";

// =============================================================================
// MCP Server Setup (using fastmcpp)
// =============================================================================

/// Create and configure the fastmcpp MCP server
std::unique_ptr<SseServerWrapper> create_mcp_server()
{
    // Create the FastMCP application
    static FastMCP app("CopilotInteropTools", "1.0.0");

    // Register tools that Copilot can use

    // Tool 1: Secret vault lookup
    app.tool(
        "secret_vault_lookup",
        Json{{"type", "object"},
             {"properties",
              Json{{"key", Json{{"type", "string"}, {"description", "The secret key to look up"}}}}},
             {"required", Json::array({"key"})}},
        [](const Json& args) -> Json
        {
            std::string key = args.value("key", std::string(""));

            // Simulated secret vault
            std::map<std::string, std::string> vault = {
                {"API_TOKEN", "tok_live_abc123xyz"},
                {"DATABASE_URL", "postgres://localhost:5432/mydb"},
                {"ENCRYPTION_KEY", "aes256_secret_key_here"},
                {"INTEROP_TEST", "FASTMCPP_COPILOT_SUCCESS"}};

            auto it = vault.find(key);
            if (it != vault.end())
            {
                return Json{
                    {"content",
                     Json::array(
                         {Json{{"type", "text"}, {"text", "Secret '" + key + "': " + it->second}}})}};
            }
            else
            {
                return Json{
                    {"content", Json::array({Json{{"type", "text"},
                                                  {"text", "No secret found for key: " + key}}})}};
            }
        },
        FastMCP::ToolOptions{.description =
                                 "Look up secrets from the vault. "
                                 "Available keys: API_TOKEN, DATABASE_URL, ENCRYPTION_KEY, INTEROP_TEST"});

    // Tool 2: Calculator with operation logging
    app.tool(
        "logged_calculator",
        Json{{"type", "object"},
             {"properties",
              Json{{"operation", Json{{"type", "string"}, {"enum", Json::array({"add", "multiply"})}}},
                   {"a", Json{{"type", "number"}}},
                   {"b", Json{{"type", "number"}}}}},
             {"required", Json::array({"operation", "a", "b"})}},
        [](const Json& args) -> Json
        {
            std::string op = args.value("operation", std::string("add"));
            double a = args.value("a", 0.0);
            double b = args.value("b", 0.0);

            double result;
            std::string op_symbol;
            if (op == "multiply")
            {
                result = a * b;
                op_symbol = "*";
            }
            else
            {
                result = a + b;
                op_symbol = "+";
            }

            std::cout << "[MCP Server] Calculator: " << a << " " << op_symbol << " " << b << " = "
                      << result << "\n";

            std::ostringstream oss;
            oss << a << " " << op_symbol << " " << b << " = " << result;

            return Json{
                {"content", Json::array({Json{{"type", "text"}, {"text", oss.str()}}})}};
        },
        FastMCP::ToolOptions{.description = "Perform logged calculations (add or multiply)"});

    // Tool 3: System info
    app.tool(
        "get_system_info",
        Json{{"type", "object"}, {"properties", Json::object()}},
        [](const Json&) -> Json
        {
            Json info = {{"server", "fastmcpp"},
                         {"version", "1.0.0"},
                         {"transport", "SSE"},
                         {"host", MCP_SERVER_HOST},
                         {"port", MCP_SERVER_PORT},
                         {"interop_status", "operational"}};

            return Json{
                {"content", Json::array({Json{{"type", "text"}, {"text", info.dump(2)}}})}};
        },
        FastMCP::ToolOptions{.description = "Get information about this MCP server"});

    // Create the MCP handler
    auto handler = fastmcpp::mcp::make_mcp_handler(app);

    // Create and return the SSE server
    return std::make_unique<SseServerWrapper>(handler, MCP_SERVER_HOST, MCP_SERVER_PORT);
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    try
    {
        std::cout << "=== Copilot SDK + fastmcpp In-Process Integration ===\n\n";

        // ---------------------------------------------------------------------
        // Step 1: Start the fastmcpp MCP server
        // ---------------------------------------------------------------------
        std::cout << "Starting fastmcpp MCP server on http://" << MCP_SERVER_HOST << ":"
                  << MCP_SERVER_PORT << "/sse...\n";

        auto mcp_server = create_mcp_server();
        if (!mcp_server->start())
        {
            std::cerr << "Failed to start MCP server!\n";
            return 1;
        }

        std::cout << "MCP server started successfully.\n";
        std::cout << "Tools available: secret_vault_lookup, logged_calculator, get_system_info\n\n";

        // Give the server a moment to fully initialize
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // ---------------------------------------------------------------------
        // Step 2: Create Copilot SDK client and configure MCP server
        // ---------------------------------------------------------------------
        std::cout << "Creating Copilot SDK client...\n";

        copilot::ClientOptions client_opts;
        client_opts.log_level = copilot::LogLevel::Info;

        copilot::Client client(client_opts);
        client.start().get();
        std::cout << "Copilot client connected.\n\n";

        // Configure the session to use our in-process MCP server
        copilot::McpRemoteServerConfig mcp_config;
        mcp_config.type = "sse";
        mcp_config.url = std::string("http://") + MCP_SERVER_HOST + ":" +
                         std::to_string(MCP_SERVER_PORT) + "/sse";
        mcp_config.tools = {"*"}; // Expose all tools from this server

        std::map<std::string, copilot::json> mcp_servers;
        mcp_servers["fastmcpp-tools"] = mcp_config;

        copilot::SessionConfig session_config;
        session_config.mcp_servers = mcp_servers;

        auto session = client.create_session(session_config).get();
        std::cout << "Session created: " << session->session_id() << "\n";
        std::cout << "MCP server 'fastmcpp-tools' configured.\n\n";

        // ---------------------------------------------------------------------
        // Step 3: Interactive loop - use MCP tools via Copilot
        // ---------------------------------------------------------------------
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
                    std::cout << "\n[Copilot calling tool: " << tool_start->tool_name << "]\n";
                    if (tool_start->arguments)
                        std::cout << "  Args: " << tool_start->arguments->dump() << "\n";
                }
                else if (auto* tool_complete = event.try_as<copilot::ToolExecutionCompleteData>())
                {
                    std::cout << "[Tool execution complete]\n";
                    if (tool_complete->result)
                        std::cout << "  Result: " << tool_complete->result->content << "\n";
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

        std::cout << "=== Ready for interaction ===\n";
        std::cout << "Try these prompts to test the MCP tools:\n";
        std::cout << "  1. 'Look up the INTEROP_TEST secret'\n";
        std::cout << "  2. 'Calculate 42 * 17 using the calculator'\n";
        std::cout << "  3. 'Get system info from the MCP server'\n";
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

        // ---------------------------------------------------------------------
        // Cleanup
        // ---------------------------------------------------------------------
        std::cout << "\nCleaning up...\n";
        session->destroy().get();
        client.stop().get();
        mcp_server->stop();

        std::cout << "Done!\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
