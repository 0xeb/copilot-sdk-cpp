// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/client.hpp>
#include <copilot/session.hpp>
#include <gtest/gtest.h>

using namespace copilot;

// =============================================================================
// Client Tests (unit tests for isolated functionality)
// =============================================================================

TEST(ClientTest, DefaultConstruction)
{
    Client client;
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(ClientTest, ConstructionWithOptions)
{
    ClientOptions opts;
    opts.log_level = LogLevel::Debug;
    opts.auto_start = false;
    opts.port = 8080;

    Client client(opts);
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(ClientTest, MutuallyExclusiveOptions)
{
    ClientOptions opts;
    opts.cli_url = "localhost:8080";
    opts.use_stdio = true;

    EXPECT_THROW(Client client(opts), std::invalid_argument);
}

TEST(ClientTest, MutuallyExclusiveOptionsWithCliPath)
{
    ClientOptions opts;
    opts.cli_url = "localhost:8080";
    opts.cli_path = "/usr/bin/copilot";

    EXPECT_THROW(Client client(opts), std::invalid_argument);
}

TEST(ClientTest, CliUrlPortOnly)
{
    ClientOptions opts;
    opts.cli_url = "8080";
    opts.use_stdio = false;

    // Should not throw - port-only URL is valid
    Client client(opts);
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(ClientTest, CliUrlWithHost)
{
    ClientOptions opts;
    opts.cli_url = "example.com:9090";
    opts.use_stdio = false;

    Client client(opts);
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(ClientTest, CliUrlWithScheme)
{
    ClientOptions opts;
    opts.cli_url = "https://example.com:443";
    opts.use_stdio = false;

    Client client(opts);
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

// =============================================================================
// Subscription Tests
// =============================================================================

TEST(SubscriptionTest, DefaultConstruction)
{
    Subscription sub;
    // Default constructed subscription should not crash
    sub.unsubscribe();
}

TEST(SubscriptionTest, UnsubscribeCalledOnDestruction)
{
    bool unsubscribed = false;
    {
        Subscription sub([&unsubscribed]() { unsubscribed = true; });
        EXPECT_FALSE(unsubscribed);
    }
    EXPECT_TRUE(unsubscribed);
}

TEST(SubscriptionTest, ManualUnsubscribe)
{
    int unsubscribe_count = 0;
    Subscription sub([&unsubscribe_count]() { unsubscribe_count++; });

    sub.unsubscribe();
    EXPECT_EQ(unsubscribe_count, 1);

    // Second unsubscribe should not call handler again
    sub.unsubscribe();
    EXPECT_EQ(unsubscribe_count, 1);
}

TEST(SubscriptionTest, MoveConstruction)
{
    bool unsubscribed = false;
    Subscription sub1([&unsubscribed]() { unsubscribed = true; });

    Subscription sub2(std::move(sub1));

    // Moving should not trigger unsubscribe
    EXPECT_FALSE(unsubscribed);

    // Destroying moved-from should not trigger unsubscribe
    sub1 = Subscription();
    EXPECT_FALSE(unsubscribed);
}

TEST(SubscriptionTest, MoveAssignment)
{
    int count1 = 0;
    int count2 = 0;

    Subscription sub1([&count1]() { count1++; });
    Subscription sub2([&count2]() { count2++; });

    // Move assign sub2 into sub1 - should unsubscribe sub1
    sub1 = std::move(sub2);
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 0);

    // Destroying sub1 should call sub2's handler
    sub1 = Subscription();
    EXPECT_EQ(count2, 1);
}

// =============================================================================
// Tool Tests
// =============================================================================

TEST(ToolTest, ToolDefinition)
{
    Tool tool;
    tool.name = "test_tool";
    tool.description = "A test tool";
    tool.parameters_schema =
        json{{"type", "object"}, {"properties", {{"input", {{"type", "string"}}}}}};
    tool.handler = [](const ToolInvocation& inv) -> ToolResultObject
    {
        ToolResultObject result;
        result.text_result_for_llm = "Success";
        return result;
    };

    EXPECT_EQ(tool.name, "test_tool");
    EXPECT_EQ(tool.description, "A test tool");

    // Test handler execution
    ToolInvocation inv;
    inv.session_id = "sess-1";
    inv.tool_call_id = "call-1";
    inv.tool_name = "test_tool";
    inv.arguments = json{{"input", "hello"}};

    auto result = tool.handler(inv);
    EXPECT_EQ(result.text_result_for_llm, "Success");
    EXPECT_EQ(result.result_type, ToolResultType::Success);
}

// =============================================================================
// Configuration Types Tests
// =============================================================================

TEST(SessionConfigTest, DefaultValues)
{
    SessionConfig config;

    EXPECT_FALSE(config.session_id.has_value());
    EXPECT_FALSE(config.model.has_value());
    EXPECT_TRUE(config.tools.empty());
    EXPECT_FALSE(config.system_message.has_value());
    EXPECT_FALSE(config.available_tools.has_value());
    EXPECT_FALSE(config.excluded_tools.has_value());
    EXPECT_FALSE(config.provider.has_value());
    EXPECT_FALSE(config.streaming);
}

TEST(ResumeSessionConfigTest, DefaultValues)
{
    ResumeSessionConfig config;

    EXPECT_TRUE(config.tools.empty());
    EXPECT_FALSE(config.provider.has_value());
    EXPECT_FALSE(config.streaming);
}

TEST(ClientOptionsTest, DefaultValues)
{
    ClientOptions opts;

    EXPECT_FALSE(opts.cli_path.has_value());
    EXPECT_FALSE(opts.cli_args.has_value());
    EXPECT_FALSE(opts.cwd.has_value());
    EXPECT_EQ(opts.port, 0);
    EXPECT_TRUE(opts.use_stdio);
    EXPECT_FALSE(opts.cli_url.has_value());
    EXPECT_EQ(opts.log_level, LogLevel::Info);
    EXPECT_TRUE(opts.auto_start);
    EXPECT_TRUE(opts.auto_restart);
    EXPECT_FALSE(opts.environment.has_value());
}

// =============================================================================
// MessageOptions Tests
// =============================================================================

TEST(MessageOptionsTest, JsonSerialization)
{
    MessageOptions opts;
    opts.prompt = "Hello, world!";
    opts.mode = "plan";

    json j = opts;

    EXPECT_EQ(j["prompt"], "Hello, world!");
    EXPECT_EQ(j["mode"], "plan");
    EXPECT_FALSE(j.contains("attachments"));
}

TEST(MessageOptionsTest, WithAttachments)
{
    MessageOptions opts;
    opts.prompt = "Analyze this file";
    opts.attachments =
        std::vector<UserMessageAttachment>{{AttachmentType::File, "/path/to/file.txt", "file.txt"}};

    json j = opts;

    EXPECT_EQ(j["prompt"], "Analyze this file");
    EXPECT_TRUE(j.contains("attachments"));
    EXPECT_EQ(j["attachments"].size(), 1);
    EXPECT_EQ(j["attachments"][0]["type"], "file");
    EXPECT_EQ(j["attachments"][0]["path"], "/path/to/file.txt");
}

// =============================================================================
// PermissionRequest Tests
// =============================================================================

TEST(PermissionRequestTest, Serialization)
{
    PermissionRequest req;
    req.kind = "tool_execution";
    req.tool_call_id = "call-123";
    req.extension_data["toolName"] = "bash";
    req.extension_data["arguments"] = json{{"command", "ls"}};

    json j = req;

    EXPECT_EQ(j["kind"], "tool_execution");
    EXPECT_EQ(j["toolCallId"], "call-123");
    EXPECT_EQ(j["toolName"], "bash");
}

TEST(PermissionRequestTest, Deserialization)
{
    json j = {
        {"kind", "tool_execution"},
        {"toolCallId", "call-456"},
        {"toolName", "read_file"},
        {"filePath", "/etc/passwd"}
    };

    auto req = j.get<PermissionRequest>();

    EXPECT_EQ(req.kind, "tool_execution");
    EXPECT_TRUE(req.tool_call_id.has_value());
    EXPECT_EQ(*req.tool_call_id, "call-456");
    EXPECT_EQ(req.extension_data["toolName"], "read_file");
    EXPECT_EQ(req.extension_data["filePath"], "/etc/passwd");
}

TEST(PermissionRequestResultTest, Approved)
{
    PermissionRequestResult result;
    result.kind = "approved";

    json j = result;

    EXPECT_EQ(j["kind"], "approved");
    EXPECT_FALSE(j.contains("rules"));
}

TEST(PermissionRequestResultTest, Denied)
{
    PermissionRequestResult result;
    result.kind = "denied-no-approval-rule-and-could-not-request-from-user";

    json j = result;

    EXPECT_EQ(j["kind"], "denied-no-approval-rule-and-could-not-request-from-user");
    EXPECT_FALSE(j.contains("rules"));
}

TEST(PermissionRequestResultTest, ApprovedWithRules)
{
    PermissionRequestResult result;
    result.kind = "approved";
    result.rules = std::vector<json>{{{"type", "allow"}, {"pattern", "*"}}};

    json j = result;

    EXPECT_EQ(j["kind"], "approved");
    EXPECT_TRUE(j.contains("rules"));
    EXPECT_EQ(j["rules"].size(), 1);
}

// =============================================================================
// Request Builder Tests (validates JSON payload shape without CLI)
// =============================================================================

TEST(SessionCreateRequestTest, IncludesToolsWhenProvided)
{
    SessionConfig config;

    Tool tool1;
    tool1.name = "calculator";
    tool1.description = "Perform math calculations";
    tool1.parameters_schema = json{
        {"type", "object"},
        {"properties", {{"expression", {{"type", "string"}}}}},
        {"required", {"expression"}}};

    Tool tool2;
    tool2.name = "weather";
    tool2.description = "Get weather info";

    config.tools = {tool1, tool2};

    json request = build_session_create_request(config);

    EXPECT_TRUE(request.contains("tools"));
    EXPECT_EQ(request["tools"].size(), 2);

    // Verify first tool
    EXPECT_EQ(request["tools"][0]["name"], "calculator");
    EXPECT_EQ(request["tools"][0]["description"], "Perform math calculations");
    EXPECT_TRUE(request["tools"][0].contains("parameters"));
    EXPECT_EQ(request["tools"][0]["parameters"]["type"], "object");

    // Verify second tool
    EXPECT_EQ(request["tools"][1]["name"], "weather");
    EXPECT_EQ(request["tools"][1]["description"], "Get weather info");
    EXPECT_FALSE(request["tools"][1].contains("parameters"));
}

TEST(SessionCreateRequestTest, ExcludesToolsWhenEmpty)
{
    SessionConfig config;
    // tools is empty by default

    json request = build_session_create_request(config);

    EXPECT_FALSE(request.contains("tools"));
}

TEST(SessionCreateRequestTest, IncludesOtherFields)
{
    SessionConfig config;
    config.model = "claude-3-opus";
    config.session_id = "sess-123";
    config.streaming = true;
    config.available_tools = std::vector<std::string>{"read", "write"};
    config.excluded_tools = std::vector<std::string>{"bash"};

    json request = build_session_create_request(config);

    EXPECT_EQ(request["model"], "claude-3-opus");
    EXPECT_EQ(request["sessionId"], "sess-123");
    EXPECT_EQ(request["streaming"], true);
    EXPECT_EQ(request["availableTools"].size(), 2);
    EXPECT_EQ(request["excludedTools"].size(), 1);
}

TEST(SessionCreateRequestTest, IncludesSystemMessage)
{
    SessionConfig config;
    SystemMessageConfig sys_msg;
    sys_msg.content = "You are a helpful assistant.";
    sys_msg.mode = SystemMessageMode::Append;
    config.system_message = sys_msg;

    json request = build_session_create_request(config);

    EXPECT_TRUE(request.contains("systemMessage"));
    EXPECT_EQ(request["systemMessage"]["content"], "You are a helpful assistant.");
    EXPECT_EQ(request["systemMessage"]["mode"], "append");
}

TEST(SessionResumeRequestTest, IncludesToolsWhenProvided)
{
    ResumeSessionConfig config;

    Tool tool;
    tool.name = "custom_tool";
    tool.description = "A custom tool for testing";
    tool.parameters_schema = json{
        {"type", "object"},
        {"properties", {{"input", {{"type", "string"}}}}}};

    config.tools = {tool};

    json request = build_session_resume_request("sess-456", config);

    EXPECT_EQ(request["sessionId"], "sess-456");
    EXPECT_TRUE(request.contains("tools"));
    EXPECT_EQ(request["tools"].size(), 1);
    EXPECT_EQ(request["tools"][0]["name"], "custom_tool");
    EXPECT_EQ(request["tools"][0]["description"], "A custom tool for testing");
    EXPECT_TRUE(request["tools"][0].contains("parameters"));
}

TEST(SessionResumeRequestTest, ExcludesToolsWhenEmpty)
{
    ResumeSessionConfig config;
    // tools is empty by default

    json request = build_session_resume_request("sess-789", config);

    EXPECT_EQ(request["sessionId"], "sess-789");
    EXPECT_FALSE(request.contains("tools"));
}

TEST(SessionResumeRequestTest, IncludesOtherFields)
{
    ResumeSessionConfig config;
    config.streaming = true;
    config.provider = json{{"baseUrl", "https://api.example.com"}};

    json request = build_session_resume_request("sess-abc", config);

    EXPECT_EQ(request["sessionId"], "sess-abc");
    EXPECT_EQ(request["streaming"], true);
    EXPECT_TRUE(request.contains("provider"));
    EXPECT_EQ(request["provider"]["baseUrl"], "https://api.example.com");
}

// =============================================================================
// Combined Configuration Tests (all options together)
// =============================================================================

TEST(SessionCreateRequestTest, CombinedToolsAndMcpServers)
{
    SessionConfig config;

    // Add custom tool
    Tool tool;
    tool.name = "calculator";
    tool.description = "Math calculations";
    tool.parameters_schema = json{{"type", "object"}};
    config.tools = {tool};

    // Add MCP server
    McpLocalServerConfig mcp_config;
    mcp_config.type = "local";
    mcp_config.command = "npx";
    mcp_config.args = {"mcp-server"};
    mcp_config.tools = {"*"};

    std::map<std::string, json> mcp_servers;
    mcp_servers["test-mcp"] = mcp_config;
    config.mcp_servers = mcp_servers;

    json request = build_session_create_request(config);

    // Both tools and mcpServers should be present
    EXPECT_TRUE(request.contains("tools"));
    EXPECT_EQ(request["tools"].size(), 1);
    EXPECT_EQ(request["tools"][0]["name"], "calculator");

    EXPECT_TRUE(request.contains("mcpServers"));
    EXPECT_TRUE(request["mcpServers"].contains("test-mcp"));
    EXPECT_EQ(request["mcpServers"]["test-mcp"]["type"], "local");
    EXPECT_EQ(request["mcpServers"]["test-mcp"]["command"], "npx");
}

TEST(SessionCreateRequestTest, CombinedToolsAndCustomAgents)
{
    SessionConfig config;

    // Add custom tool
    Tool tool;
    tool.name = "data_tool";
    tool.description = "Data operations";
    config.tools = {tool};

    // Add custom agent
    CustomAgentConfig agent;
    agent.name = "analyst";
    agent.display_name = "Data Analyst";
    agent.description = "Analyzes data";
    agent.prompt = "You are a data analyst.";
    agent.tools = std::vector<std::string>{"data_tool"};
    config.custom_agents = std::vector<CustomAgentConfig>{agent};

    json request = build_session_create_request(config);

    // Both tools and customAgents should be present
    EXPECT_TRUE(request.contains("tools"));
    EXPECT_EQ(request["tools"].size(), 1);

    EXPECT_TRUE(request.contains("customAgents"));
    EXPECT_EQ(request["customAgents"].size(), 1);
    EXPECT_EQ(request["customAgents"][0]["name"], "analyst");
    EXPECT_EQ(request["customAgents"][0]["displayName"], "Data Analyst");
    EXPECT_TRUE(request["customAgents"][0].contains("tools"));
    EXPECT_EQ(request["customAgents"][0]["tools"][0], "data_tool");
}

TEST(SessionCreateRequestTest, AllConfigOptionsCombined)
{
    SessionConfig config;

    // Model and session
    config.model = "claude-3-opus";
    config.session_id = "combined-session";
    config.streaming = true;

    // System message
    SystemMessageConfig sys_msg;
    sys_msg.content = "You are a helpful assistant.";
    sys_msg.mode = SystemMessageMode::Append;
    config.system_message = sys_msg;

    // Custom tool
    Tool tool;
    tool.name = "helper_tool";
    tool.description = "Helps with tasks";
    tool.parameters_schema = json{
        {"type", "object"},
        {"properties", {{"task", {{"type", "string"}}}}}};
    config.tools = {tool};

    // MCP servers
    McpLocalServerConfig mcp_local;
    mcp_local.type = "local";
    mcp_local.command = "node";
    mcp_local.args = {"server.js"};
    mcp_local.tools = {"tool1", "tool2"};

    McpRemoteServerConfig mcp_remote;
    mcp_remote.type = "sse";
    mcp_remote.url = "http://localhost:8080/sse";
    mcp_remote.tools = {"*"};

    std::map<std::string, json> mcp_servers;
    mcp_servers["local-server"] = mcp_local;
    mcp_servers["remote-server"] = mcp_remote;
    config.mcp_servers = mcp_servers;

    // Custom agents
    CustomAgentConfig agent1;
    agent1.name = "agent-alpha";
    agent1.display_name = "Alpha Agent";
    agent1.description = "First agent";
    agent1.prompt = "You are alpha.";

    CustomAgentConfig agent2;
    agent2.name = "agent-beta";
    agent2.display_name = "Beta Agent";
    agent2.description = "Second agent";
    agent2.prompt = "You are beta.";
    agent2.infer = false;
    agent2.tools = std::vector<std::string>{"read", "write"};

    config.custom_agents = std::vector<CustomAgentConfig>{agent1, agent2};

    // Tool restrictions
    config.available_tools = std::vector<std::string>{"read", "write", "helper_tool"};
    config.excluded_tools = std::vector<std::string>{"dangerous_tool"};

    // Provider
    config.provider = json{{"baseUrl", "https://api.custom.com"}, {"apiKey", "sk-xxx"}};

    // Permission handler should flip requestPermission on (handler itself is not serialized)
    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    json request = build_session_create_request(config);

    // Verify all fields are present
    EXPECT_EQ(request["model"], "claude-3-opus");
    EXPECT_EQ(request["sessionId"], "combined-session");
    EXPECT_EQ(request["streaming"], true);

    EXPECT_TRUE(request.contains("systemMessage"));
    EXPECT_EQ(request["systemMessage"]["mode"], "append");

    EXPECT_TRUE(request.contains("tools"));
    EXPECT_EQ(request["tools"].size(), 1);
    EXPECT_EQ(request["tools"][0]["name"], "helper_tool");

    EXPECT_TRUE(request.contains("mcpServers"));
    EXPECT_EQ(request["mcpServers"].size(), 2);
    EXPECT_TRUE(request["mcpServers"].contains("local-server"));
    EXPECT_TRUE(request["mcpServers"].contains("remote-server"));

    EXPECT_TRUE(request.contains("customAgents"));
    EXPECT_EQ(request["customAgents"].size(), 2);

    EXPECT_TRUE(request.contains("availableTools"));
    EXPECT_EQ(request["availableTools"].size(), 3);

    EXPECT_TRUE(request.contains("excludedTools"));
    EXPECT_EQ(request["excludedTools"].size(), 1);

    EXPECT_TRUE(request.contains("provider"));
    EXPECT_EQ(request["provider"]["baseUrl"], "https://api.custom.com");
    EXPECT_TRUE(request.contains("requestPermission"));
    EXPECT_TRUE(request["requestPermission"].get<bool>());
}

TEST(SessionResumeRequestTest, AllConfigOptionsCombined)
{
    ResumeSessionConfig config;

    // Streaming
    config.streaming = true;

    // Custom tool
    Tool tool;
    tool.name = "resume_tool";
    tool.description = "Tool added on resume";
    config.tools = {tool};

    // MCP servers
    McpRemoteServerConfig mcp;
    mcp.type = "http";
    mcp.url = "https://mcp.example.com/api";
    mcp.tools = {"*"};

    std::map<std::string, json> mcp_servers;
    mcp_servers["resume-mcp"] = mcp;
    config.mcp_servers = mcp_servers;

    // Custom agents
    CustomAgentConfig agent;
    agent.name = "resume-agent";
    agent.display_name = "Resume Agent";
    agent.description = "Agent added on resume";
    agent.prompt = "You help with resumed sessions.";
    config.custom_agents = std::vector<CustomAgentConfig>{agent};

    // Provider
    config.provider = json{{"baseUrl", "https://api.resume.com"}};

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    json request = build_session_resume_request("resume-session-id", config);

    // Verify all fields
    EXPECT_EQ(request["sessionId"], "resume-session-id");
    EXPECT_EQ(request["streaming"], true);

    EXPECT_TRUE(request.contains("tools"));
    EXPECT_EQ(request["tools"].size(), 1);
    EXPECT_EQ(request["tools"][0]["name"], "resume_tool");

    EXPECT_TRUE(request.contains("mcpServers"));
    EXPECT_TRUE(request["mcpServers"].contains("resume-mcp"));

    EXPECT_TRUE(request.contains("customAgents"));
    EXPECT_EQ(request["customAgents"].size(), 1);
    EXPECT_EQ(request["customAgents"][0]["name"], "resume-agent");

    EXPECT_TRUE(request.contains("provider"));
    EXPECT_TRUE(request.contains("requestPermission"));
    EXPECT_TRUE(request["requestPermission"].get<bool>());
}

TEST(SessionCreateRequestTest, McpServerWithAllOptions)
{
    SessionConfig config;

    // MCP server with all options
    McpLocalServerConfig mcp;
    mcp.type = "local";
    mcp.command = "python";
    mcp.args = {"-m", "mcp_server", "--port", "8080"};
    mcp.tools = {"tool_a", "tool_b"};
    mcp.timeout = 30000;
    mcp.cwd = "/path/to/server";
    mcp.env = std::map<std::string, std::string>{
        {"API_KEY", "secret"},
        {"DEBUG", "true"}};

    std::map<std::string, json> mcp_servers;
    mcp_servers["full-config-server"] = mcp;
    config.mcp_servers = mcp_servers;

    json request = build_session_create_request(config);

    EXPECT_TRUE(request.contains("mcpServers"));
    auto& server = request["mcpServers"]["full-config-server"];

    EXPECT_EQ(server["type"], "local");
    EXPECT_EQ(server["command"], "python");
    EXPECT_EQ(server["args"].size(), 4);
    EXPECT_EQ(server["tools"].size(), 2);
    EXPECT_EQ(server["timeout"], 30000);
    EXPECT_EQ(server["cwd"], "/path/to/server");
    EXPECT_TRUE(server.contains("env"));
    EXPECT_EQ(server["env"]["API_KEY"], "secret");
}

TEST(SessionCreateRequestTest, CustomAgentWithMcpServers)
{
    SessionConfig config;

    // Agent-specific MCP server
    McpRemoteServerConfig agent_mcp;
    agent_mcp.type = "sse";
    agent_mcp.url = "http://localhost:9090/sse";
    agent_mcp.tools = {"*"};

    std::map<std::string, json> agent_mcp_servers;
    agent_mcp_servers["agent-mcp"] = agent_mcp;

    CustomAgentConfig agent;
    agent.name = "mcp-equipped-agent";
    agent.display_name = "MCP Agent";
    agent.description = "Agent with its own MCP servers";
    agent.prompt = "You have access to special MCP tools.";
    agent.mcp_servers = agent_mcp_servers;
    agent.tools = std::vector<std::string>{"read", "write"};
    agent.infer = true;

    config.custom_agents = std::vector<CustomAgentConfig>{agent};

    json request = build_session_create_request(config);

    EXPECT_TRUE(request.contains("customAgents"));
    auto& agent_json = request["customAgents"][0];

    EXPECT_EQ(agent_json["name"], "mcp-equipped-agent");
    EXPECT_TRUE(agent_json.contains("mcpServers"));
    EXPECT_TRUE(agent_json["mcpServers"].contains("agent-mcp"));
    EXPECT_EQ(agent_json["mcpServers"]["agent-mcp"]["type"], "sse");
    EXPECT_TRUE(agent_json.contains("tools"));
    EXPECT_EQ(agent_json["infer"], true);
}

// =============================================================================
// URL Parsing Edge Case Tests
// =============================================================================

TEST(ClientOptions, InvalidPortTooHigh)
{
    ClientOptions opts;
    opts.cli_url = "70000";
    opts.use_stdio = false;

    // Port 70000 exceeds valid range, falls through to hostname parsing
    Client client(opts);
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(ClientOptions, InvalidPortZero)
{
    ClientOptions opts;
    opts.cli_url = "0";
    opts.use_stdio = false;

    // Port 0 is not in valid range (1-65535), falls through to hostname parsing
    Client client(opts);
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(ClientOptions, InvalidPortNegative)
{
    ClientOptions opts;
    opts.cli_url = "-1";
    opts.use_stdio = false;

    // Negative port not valid, falls through to hostname parsing
    Client client(opts);
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(ClientOptions, AuthDefaultWithToken)
{
    ClientOptions opts;
    opts.github_token = "ghp_test123";
    opts.auto_start = false;

    Client client(opts);
    // use_logged_in_user should default to false when github_token is set
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(ClientOptions, AuthDefaultWithoutToken)
{
    ClientOptions opts;
    opts.auto_start = false;

    Client client(opts);
    // use_logged_in_user should default to true without token
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}
