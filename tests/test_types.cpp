// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/copilot.hpp>
#include <gtest/gtest.h>

using namespace copilot;

// =============================================================================
// Types Tests
// =============================================================================

TEST(TypesTest, ConnectionStateEnum)
{
    json j = ConnectionState::Connected;
    EXPECT_EQ(j, "connected");

    ConnectionState state = j.get<ConnectionState>();
    EXPECT_EQ(state, ConnectionState::Connected);
}

TEST(TypesTest, SystemMessageModeEnum)
{
    json j = SystemMessageMode::Append;
    EXPECT_EQ(j, "append");

    j = SystemMessageMode::Replace;
    EXPECT_EQ(j, "replace");
}

TEST(TypesTest, ToolBinaryResultRoundTrip)
{
    ToolBinaryResult original{
        .data = "base64data",
        .mime_type = "image/png",
        .type = "image",
        .description = "A test image"
    };

    json j = original;
    EXPECT_EQ(j["data"], "base64data");
    EXPECT_EQ(j["mimeType"], "image/png");
    EXPECT_EQ(j["type"], "image");
    EXPECT_EQ(j["description"], "A test image");

    auto parsed = j.get<ToolBinaryResult>();
    EXPECT_EQ(parsed.data, original.data);
    EXPECT_EQ(parsed.mime_type, original.mime_type);
    EXPECT_EQ(parsed.type, original.type);
    EXPECT_EQ(parsed.description, original.description);
}

TEST(TypesTest, ToolResultObjectMinimal)
{
    ToolResultObject result{.text_result_for_llm = "Success!", .result_type = "success"};

    json j = result;
    EXPECT_EQ(j["textResultForLlm"], "Success!");
    EXPECT_EQ(j["resultType"], "success");
    EXPECT_FALSE(j.contains("error"));
}

TEST(TypesTest, PermissionRequestWithExtensionData)
{
    json input = {
        {"kind", "file_access"},
        {"toolCallId", "tc_123"},
        {"path", "/etc/passwd"},
        {"operation", "read"}
    };

    auto req = input.get<PermissionRequest>();
    EXPECT_EQ(req.kind, "file_access");
    EXPECT_EQ(req.tool_call_id, "tc_123");
    EXPECT_EQ(req.extension_data.size(), 2);
    EXPECT_EQ(req.extension_data.at("path"), "/etc/passwd");
    EXPECT_EQ(req.extension_data.at("operation"), "read");
}

TEST(TypesTest, ProviderConfig)
{
    ProviderConfig config{
        .type = "openai",
        .base_url = "https://api.openai.com/v1",
        .api_key = "sk-test",
        .azure = AzureOptions{.api_version = "2024-02-01"}
    };

    json j = config;
    EXPECT_EQ(j["type"], "openai");
    EXPECT_EQ(j["baseUrl"], "https://api.openai.com/v1");
    EXPECT_EQ(j["apiKey"], "sk-test");
    EXPECT_EQ(j["azure"]["apiVersion"], "2024-02-01");
}

TEST(TypesTest, McpLocalServerConfig)
{
    McpLocalServerConfig config;
    config.tools = {"*"};
    config.type = "local";
    config.timeout = 30000;
    config.command = "node";
    config.args = {"server.js"};
    config.env = std::map<std::string, std::string>{{"NODE_ENV", "production"}};
    config.cwd = "/app";

    json j = config;
    EXPECT_EQ(j["tools"], json::array({"*"}));
    EXPECT_EQ(j["type"], "local");
    EXPECT_EQ(j["timeout"], 30000);
    EXPECT_EQ(j["command"], "node");
    EXPECT_EQ(j["args"], json::array({"server.js"}));
    EXPECT_EQ(j["env"]["NODE_ENV"], "production");
    EXPECT_EQ(j["cwd"], "/app");
}

TEST(TypesTest, CustomAgentConfig)
{
    CustomAgentConfig agent{
        .name = "code_reviewer",
        .display_name = "Code Reviewer",
        .description = "Reviews code for issues",
        .tools = std::vector<std::string>{"read_file", "grep"},
        .prompt = "You are a code reviewer...",
        .infer = true
    };

    json j = agent;
    EXPECT_EQ(j["name"], "code_reviewer");
    EXPECT_EQ(j["displayName"], "Code Reviewer");
    EXPECT_EQ(j["description"], "Reviews code for issues");
    EXPECT_EQ(j["tools"], json::array({"read_file", "grep"}));
    EXPECT_EQ(j["prompt"], "You are a code reviewer...");
    EXPECT_EQ(j["infer"], true);
}

TEST(TypesTest, MessageOptions)
{
    MessageOptions opts{
        .prompt = "Hello, world!",
        .attachments = std::vector<UserMessageAttachment>{{UserMessageAttachment{
            .type = AttachmentType::File, .path = "/path/to/file.cpp", .display_name = "file.cpp"
        }}},
        .mode = "chat"
    };

    json j = opts;
    EXPECT_EQ(j["prompt"], "Hello, world!");
    EXPECT_EQ(j["attachments"][0]["type"], "file");
    EXPECT_EQ(j["attachments"][0]["path"], "/path/to/file.cpp");
    EXPECT_EQ(j["mode"], "chat");
}

TEST(TypesTest, PingResponse)
{
    json input = {{"message", "pong"}, {"timestamp", 1234567890}, {"protocolVersion", 2}};

    auto resp = input.get<PingResponse>();
    EXPECT_EQ(resp.message, "pong");
    EXPECT_EQ(resp.timestamp, 1234567890);
    EXPECT_EQ(resp.protocol_version, 2);
}

TEST(TypesTest, ProtocolVersion)
{
    EXPECT_EQ(kSdkProtocolVersion, 2);
}

TEST(TypesTest, SessionMetadataParsesIso8601Timestamps)
{
    using namespace std::chrono;

    json input = {
        {"sessionId", "sess_123"},
        {"startTime", "2025-01-15T10:30:00Z"},
        {"modifiedTime", "2025-01-15T11:30:00+01:00"},
        {"summary", "Test session"},
        {"isRemote", false}
    };

    auto meta = input.get<SessionMetadata>();
    EXPECT_EQ(meta.session_id, "sess_123");
    EXPECT_EQ(meta.summary, "Test session");
    EXPECT_FALSE(meta.is_remote);

    auto expected = sys_days{year{2025} / 1 / 15} + hours{10} + minutes{30};
    EXPECT_EQ(time_point_cast<seconds>(meta.start_time), time_point_cast<seconds>(expected));
    EXPECT_EQ(time_point_cast<seconds>(meta.modified_time), time_point_cast<seconds>(expected));
}

// =============================================================================
// Events Tests
// =============================================================================

TEST(EventsTest, SessionStartEvent)
{
    json input = {
        {"id", "evt_001"},
        {"timestamp", "2025-01-15T10:30:00Z"},
        {"type", "session.start"},
        {"data",
         {{"sessionId", "sess_123"},
          {"version", 1.0},
          {"producer", "copilot-cli"},
          {"copilotVersion", "1.0.0"},
          {"startTime", "2025-01-15T10:30:00Z"},
          {"selectedModel", "gpt-4"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.id, "evt_001");
    EXPECT_EQ(event.type, SessionEventType::SessionStart);
    EXPECT_TRUE(event.is<SessionStartData>());

    const auto& data = event.as<SessionStartData>();
    EXPECT_EQ(data.session_id, "sess_123");
    EXPECT_EQ(data.version, 1.0);
    EXPECT_EQ(data.producer, "copilot-cli");
    EXPECT_EQ(data.selected_model, "gpt-4");
}

TEST(EventsTest, AssistantMessageEvent)
{
    json input = {
        {"id", "evt_002"},
        {"timestamp", "2025-01-15T10:31:00Z"},
        {"parentId", "evt_001"},
        {"type", "assistant.message"},
        {"data",
         {{"messageId", "msg_456"},
          {"content", "Hello! How can I help?"},
          {"toolRequests",
           json::array(
               {{{"toolCallId", "tc_1"},
                 {"name", "read_file"},
                 {"arguments", {{"path", "/test.txt"}}}}}
           )}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::AssistantMessage);
    EXPECT_EQ(event.parent_id, "evt_001");

    const auto& data = event.as<AssistantMessageData>();
    EXPECT_EQ(data.message_id, "msg_456");
    EXPECT_EQ(data.content, "Hello! How can I help?");
    EXPECT_TRUE(data.tool_requests.has_value());
    EXPECT_EQ(data.tool_requests->size(), 1);
    EXPECT_EQ(data.tool_requests->at(0).tool_call_id, "tc_1");
    EXPECT_EQ(data.tool_requests->at(0).name, "read_file");
}

TEST(EventsTest, AssistantMessageDeltaEvent)
{
    json input = {
        {"id", "evt_003"},
        {"timestamp", "2025-01-15T10:31:01Z"},
        {"ephemeral", true},
        {"type", "assistant.message_delta"},
        {"data", {{"messageId", "msg_456"}, {"deltaContent", "Hello"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::AssistantMessageDelta);
    EXPECT_TRUE(event.ephemeral.value_or(false));

    const auto& data = event.as<AssistantMessageDeltaData>();
    EXPECT_EQ(data.message_id, "msg_456");
    EXPECT_EQ(data.delta_content, "Hello");
}

TEST(EventsTest, ToolExecutionCompleteEvent)
{
    json input = {
        {"id", "evt_004"},
        {"timestamp", "2025-01-15T10:32:00Z"},
        {"type", "tool.execution_complete"},
        {"data",
         {{"toolCallId", "tc_1"},
          {"success", true},
          {"result", {{"content", "File contents here"}}},
          {"toolTelemetry", {{"duration_ms", 150}}}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::ToolExecutionComplete);

    const auto& data = event.as<ToolExecutionCompleteData>();
    EXPECT_EQ(data.tool_call_id, "tc_1");
    EXPECT_TRUE(data.success);
    EXPECT_TRUE(data.result.has_value());
    EXPECT_EQ(data.result->content, "File contents here");
    EXPECT_TRUE(data.tool_telemetry.has_value());
}

TEST(EventsTest, SessionErrorEvent)
{
    json input = {
        {"id", "evt_005"},
        {"timestamp", "2025-01-15T10:33:00Z"},
        {"type", "session.error"},
        {"data",
         {{"errorType", "rate_limit"},
          {"message", "Rate limit exceeded"},
          {"stack", "Error: Rate limit...\n  at foo()"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::SessionError);

    const auto& data = event.as<SessionErrorData>();
    EXPECT_EQ(data.error_type, "rate_limit");
    EXPECT_EQ(data.message, "Rate limit exceeded");
    EXPECT_TRUE(data.stack.has_value());
}

TEST(EventsTest, SessionIdleEvent)
{
    json input = {
        {"id", "evt_006"},
        {"timestamp", "2025-01-15T10:34:00Z"},
        {"type", "session.idle"},
        {"data", json::object()}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::SessionIdle);
    EXPECT_TRUE(event.is<SessionIdleData>());
}

TEST(EventsTest, CustomAgentSelectedEvent)
{
    json input = {
        {"id", "evt_007"},
        {"timestamp", "2025-01-15T10:35:00Z"},
        {"type", "custom_agent.selected"},
        {"data",
         {{"agentName", "code_reviewer"},
          {"agentDisplayName", "Code Reviewer"},
          {"tools", json::array({"read_file", "grep", "write_file"})}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::CustomAgentSelected);

    const auto& data = event.as<CustomAgentSelectedData>();
    EXPECT_EQ(data.agent_name, "code_reviewer");
    EXPECT_EQ(data.agent_display_name, "Code Reviewer");
    EXPECT_EQ(data.tools.size(), 3);
}

TEST(EventsTest, UnknownEventType)
{
    json input = {
        {"id", "evt_008"},
        {"timestamp", "2025-01-15T10:36:00Z"},
        {"type", "future.new_event"},
        {"data", {{"foo", "bar"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::Unknown);
    EXPECT_EQ(event.type_string, "future.new_event");
    EXPECT_TRUE(event.is<json>());

    const auto& raw_data = event.as<json>();
    EXPECT_EQ(raw_data["foo"], "bar");
}

TEST(EventsTest, TryAsNullOnWrongType)
{
    json input = {
        {"id", "evt_009"},
        {"timestamp", "2025-01-15T10:37:00Z"},
        {"type", "session.idle"},
        {"data", json::object()}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.try_as<SessionIdleData>(), &event.as<SessionIdleData>());
    EXPECT_EQ(event.try_as<SessionStartData>(), nullptr);
}

TEST(EventsTest, AssistantUsageEvent)
{
    json input = {
        {"id", "evt_010"},
        {"timestamp", "2025-01-15T10:38:00Z"},
        {"type", "assistant.usage"},
        {"data",
         {{"model", "gpt-4"},
          {"inputTokens", 100},
          {"outputTokens", 50},
          {"cost", 0.0045},
          {"duration", 1500}}}
    };

    auto event = input.get<SessionEvent>();
    const auto& data = event.as<AssistantUsageData>();
    EXPECT_EQ(data.model, "gpt-4");
    EXPECT_EQ(data.input_tokens, 100);
    EXPECT_EQ(data.output_tokens, 50);
    EXPECT_DOUBLE_EQ(*data.cost, 0.0045);
    EXPECT_EQ(data.duration, 1500);
}

TEST(EventsTest, HookEvents)
{
    json start_input = {
        {"id", "evt_011"},
        {"timestamp", "2025-01-15T10:39:00Z"},
        {"type", "hook.start"},
        {"data",
         {{"hookInvocationId", "hook_1"}, {"hookType", "pre_tool"}, {"input", {{"tool", "bash"}}}}}
    };

    auto start_event = start_input.get<SessionEvent>();
    EXPECT_EQ(start_event.type, SessionEventType::HookStart);
    const auto& start_data = start_event.as<HookStartData>();
    EXPECT_EQ(start_data.hook_invocation_id, "hook_1");
    EXPECT_EQ(start_data.hook_type, "pre_tool");

    json end_input = {
        {"id", "evt_012"},
        {"timestamp", "2025-01-15T10:39:01Z"},
        {"type", "hook.end"},
        {"data", {{"hookInvocationId", "hook_1"}, {"hookType", "pre_tool"}, {"success", true}}}
    };

    auto end_event = end_input.get<SessionEvent>();
    EXPECT_EQ(end_event.type, SessionEventType::HookEnd);
    const auto& end_data = end_event.as<HookEndData>();
    EXPECT_TRUE(end_data.success);
}
