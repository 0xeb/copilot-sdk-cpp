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
    ToolResultObject result{.text_result_for_llm = "Success!", .result_type = ToolResultType::Success};

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

// =============================================================================
// Gap 1: Subagent wire format tests
// =============================================================================

TEST(EventsTest, SubagentStartedViaNewWireFormat)
{
    json input = {
        {"id", "evt_sub_1"},
        {"timestamp", "2025-01-22T10:00:00Z"},
        {"type", "subagent.started"},
        {"data",
         {{"toolCallId", "tc_100"},
          {"agentName", "my_agent"},
          {"agentDisplayName", "My Agent"},
          {"agentDescription", "A helpful agent"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::CustomAgentStarted);
    EXPECT_EQ(event.type_string, "subagent.started");

    const auto* data = event.try_as<CustomAgentStartedData>();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->tool_call_id, "tc_100");
    EXPECT_EQ(data->agent_name, "my_agent");
    EXPECT_EQ(data->agent_display_name, "My Agent");
    EXPECT_EQ(data->agent_description, "A helpful agent");
}

TEST(EventsTest, SubagentCompletedViaNewWireFormat)
{
    json input = {
        {"id", "evt_sub_2"},
        {"timestamp", "2025-01-22T10:01:00Z"},
        {"type", "subagent.completed"},
        {"data", {{"toolCallId", "tc_101"}, {"agentName", "my_agent"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::CustomAgentCompleted);
    EXPECT_EQ(event.type_string, "subagent.completed");

    const auto& data = event.as<CustomAgentCompletedData>();
    EXPECT_EQ(data.tool_call_id, "tc_101");
    EXPECT_EQ(data.agent_name, "my_agent");
}

TEST(EventsTest, SubagentFailedViaNewWireFormat)
{
    json input = {
        {"id", "evt_sub_3"},
        {"timestamp", "2025-01-22T10:02:00Z"},
        {"type", "subagent.failed"},
        {"data", {{"toolCallId", "tc_102"}, {"agentName", "my_agent"}, {"error", "timeout"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::CustomAgentFailed);

    const auto& data = event.as<CustomAgentFailedData>();
    EXPECT_EQ(data.tool_call_id, "tc_102");
    EXPECT_EQ(data.error, "timeout");
}

TEST(EventsTest, SubagentSelectedViaNewWireFormat)
{
    json input = {
        {"id", "evt_sub_4"},
        {"timestamp", "2025-01-22T10:03:00Z"},
        {"type", "subagent.selected"},
        {"data",
         {{"agentName", "code_reviewer"},
          {"agentDisplayName", "Code Reviewer"},
          {"tools", json::array({"read_file", "grep"})}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::CustomAgentSelected);

    const auto& data = event.as<CustomAgentSelectedData>();
    EXPECT_EQ(data.agent_name, "code_reviewer");
    EXPECT_EQ(data.tools.size(), 2);
}

TEST(EventsTest, LegacyCustomAgentWireFormatStillWorks)
{
    // Backwards compatibility: custom_agent.started should still parse
    json input = {
        {"id", "evt_legacy_1"},
        {"timestamp", "2025-01-22T10:04:00Z"},
        {"type", "custom_agent.started"},
        {"data",
         {{"toolCallId", "tc_200"},
          {"agentName", "old_agent"},
          {"agentDisplayName", "Old Agent"},
          {"agentDescription", "Legacy agent"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::CustomAgentStarted);
    EXPECT_EQ(event.type_string, "custom_agent.started");

    const auto* data = event.try_as<CustomAgentStartedData>();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->tool_call_id, "tc_200");
    EXPECT_EQ(data->agent_name, "old_agent");
}

TEST(EventsTest, LegacyCustomAgentCompletedStillWorks)
{
    json input = {
        {"id", "evt_legacy_2"},
        {"timestamp", "2025-01-22T10:05:00Z"},
        {"type", "custom_agent.completed"},
        {"data", {{"toolCallId", "tc_201"}, {"agentName", "old_agent"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::CustomAgentCompleted);
}

TEST(EventsTest, LegacyCustomAgentFailedStillWorks)
{
    json input = {
        {"id", "evt_legacy_3"},
        {"timestamp", "2025-01-22T10:06:00Z"},
        {"type", "custom_agent.failed"},
        {"data", {{"toolCallId", "tc_202"}, {"agentName", "old_agent"}, {"error", "crash"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::CustomAgentFailed);
}

TEST(EventsTest, LegacyCustomAgentSelectedStillWorks)
{
    json input = {
        {"id", "evt_legacy_4"},
        {"timestamp", "2025-01-22T10:07:00Z"},
        {"type", "custom_agent.selected"},
        {"data",
         {{"agentName", "old_reviewer"},
          {"agentDisplayName", "Old Reviewer"},
          {"tools", json::array({"view"})}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::CustomAgentSelected);
}

// =============================================================================
// Gap 2: Missing event types tests
// =============================================================================

TEST(EventsTest, SessionCompactionStartEvent)
{
    json input = {
        {"id", "evt_compact_1"},
        {"timestamp", "2025-01-22T11:00:00Z"},
        {"type", "session.compaction_start"},
        {"data", json::object()}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::SessionCompactionStart);
    EXPECT_TRUE(event.is<SessionCompactionStartData>());
    EXPECT_EQ(event.type_string, "session.compaction_start");
}

TEST(EventsTest, SessionCompactionCompleteEventFull)
{
    json input = {
        {"id", "evt_compact_2"},
        {"timestamp", "2025-01-22T11:01:00Z"},
        {"type", "session.compaction_complete"},
        {"data",
         {{"success", true},
          {"preCompactionTokens", 50000.0},
          {"postCompactionTokens", 20000.0},
          {"preCompactionMessagesLength", 100.0},
          {"postCompactionMessagesLength", 40.0},
          {"compactionTokensUsed", {{"input", 1000.0}, {"output", 500.0}, {"cachedInput", 200.0}}}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::SessionCompactionComplete);

    const auto& data = event.as<SessionCompactionCompleteData>();
    EXPECT_TRUE(data.success);
    EXPECT_FALSE(data.error.has_value());
    EXPECT_DOUBLE_EQ(*data.pre_compaction_tokens, 50000.0);
    EXPECT_DOUBLE_EQ(*data.post_compaction_tokens, 20000.0);
    EXPECT_DOUBLE_EQ(*data.pre_compaction_messages_length, 100.0);
    EXPECT_DOUBLE_EQ(*data.post_compaction_messages_length, 40.0);
    ASSERT_TRUE(data.compaction_tokens_used.has_value());
    EXPECT_DOUBLE_EQ(data.compaction_tokens_used->input, 1000.0);
    EXPECT_DOUBLE_EQ(data.compaction_tokens_used->output, 500.0);
    EXPECT_DOUBLE_EQ(data.compaction_tokens_used->cached_input, 200.0);
}

TEST(EventsTest, SessionCompactionCompleteEventWithError)
{
    json input = {
        {"id", "evt_compact_3"},
        {"timestamp", "2025-01-22T11:02:00Z"},
        {"type", "session.compaction_complete"},
        {"data", {{"success", false}, {"error", "compaction failed: out of memory"}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::SessionCompactionComplete);

    const auto& data = event.as<SessionCompactionCompleteData>();
    EXPECT_FALSE(data.success);
    ASSERT_TRUE(data.error.has_value());
    EXPECT_EQ(*data.error, "compaction failed: out of memory");
    EXPECT_FALSE(data.pre_compaction_tokens.has_value());
    EXPECT_FALSE(data.compaction_tokens_used.has_value());
}

TEST(EventsTest, SessionCompactionCompleteEventMinimal)
{
    json input = {
        {"id", "evt_compact_4"},
        {"timestamp", "2025-01-22T11:03:00Z"},
        {"type", "session.compaction_complete"},
        {"data", {{"success", true}}}
    };

    auto event = input.get<SessionEvent>();
    const auto& data = event.as<SessionCompactionCompleteData>();
    EXPECT_TRUE(data.success);
    EXPECT_FALSE(data.error.has_value());
    EXPECT_FALSE(data.pre_compaction_tokens.has_value());
    EXPECT_FALSE(data.post_compaction_tokens.has_value());
    EXPECT_FALSE(data.compaction_tokens_used.has_value());
}

TEST(EventsTest, SessionUsageInfoEvent)
{
    json input = {
        {"id", "evt_usage_1"},
        {"timestamp", "2025-01-22T12:00:00Z"},
        {"type", "session.usage_info"},
        {"data", {{"tokenLimit", 128000.0}, {"currentTokens", 45000.0}, {"messagesLength", 25.0}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::SessionUsageInfo);
    EXPECT_EQ(event.type_string, "session.usage_info");

    const auto& data = event.as<SessionUsageInfoData>();
    EXPECT_DOUBLE_EQ(data.token_limit, 128000.0);
    EXPECT_DOUBLE_EQ(data.current_tokens, 45000.0);
    EXPECT_DOUBLE_EQ(data.messages_length, 25.0);
}

TEST(EventsTest, ToolExecutionProgressEvent)
{
    json input = {
        {"id", "evt_progress_1"},
        {"timestamp", "2025-01-22T13:00:00Z"},
        {"type", "tool.execution_progress"},
        {"data", {{"toolCallId", "tc_300"}, {"progressMessage", "Processing file 3 of 10..."}}}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::ToolExecutionProgress);
    EXPECT_EQ(event.type_string, "tool.execution_progress");

    const auto& data = event.as<ToolExecutionProgressData>();
    EXPECT_EQ(data.tool_call_id, "tc_300");
    EXPECT_EQ(data.progress_message, "Processing file 3 of 10...");
}

TEST(EventsTest, ToolExecutionProgressTryAs)
{
    json input = {
        {"id", "evt_progress_2"},
        {"timestamp", "2025-01-22T13:01:00Z"},
        {"type", "tool.execution_progress"},
        {"data", {{"toolCallId", "tc_301"}, {"progressMessage", "Step 1 complete"}}}
    };

    auto event = input.get<SessionEvent>();
    const auto* data = event.try_as<ToolExecutionProgressData>();
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->tool_call_id, "tc_301");

    // Wrong type should return nullptr
    EXPECT_EQ(event.try_as<SessionStartData>(), nullptr);
    EXPECT_EQ(event.try_as<SessionCompactionStartData>(), nullptr);
}

// =============================================================================
// Gap 3: ModelVisionLimits tests
// =============================================================================

TEST(TypesTest, ModelVisionLimitsParsing)
{
    json input = {
        {"supportedMediaTypes", json::array({"image/png", "image/jpeg", "image/gif"})},
        {"maxPromptImages", 10},
        {"maxPromptImageSize", 20971520}
    };

    auto limits = input.get<ModelVisionLimits>();
    ASSERT_EQ(limits.supported_media_types.size(), 3);
    EXPECT_EQ(limits.supported_media_types[0], "image/png");
    EXPECT_EQ(limits.supported_media_types[1], "image/jpeg");
    EXPECT_EQ(limits.supported_media_types[2], "image/gif");
    EXPECT_EQ(limits.max_prompt_images, 10);
    EXPECT_EQ(limits.max_prompt_image_size, 20971520);
}

TEST(TypesTest, ModelVisionLimitsEmpty)
{
    json input = json::object();

    auto limits = input.get<ModelVisionLimits>();
    EXPECT_TRUE(limits.supported_media_types.empty());
    EXPECT_EQ(limits.max_prompt_images, 0);
    EXPECT_EQ(limits.max_prompt_image_size, 0);
}

TEST(TypesTest, ModelCapabilitiesWithVisionLimits)
{
    json input = {
        {"supports", {{"vision", true}}},
        {"limits",
         {{"max_prompt_tokens", 4096},
          {"max_context_window_tokens", 128000},
          {"vision",
           {{"supportedMediaTypes", json::array({"image/png", "image/jpeg"})},
            {"maxPromptImages", 5},
            {"maxPromptImageSize", 10485760}}}}}
    };

    auto caps = input.get<ModelCapabilities>();
    EXPECT_TRUE(caps.supports.vision);
    EXPECT_EQ(caps.limits.max_prompt_tokens, 4096);
    EXPECT_EQ(caps.limits.max_context_window_tokens, 128000);

    ASSERT_TRUE(caps.limits.vision.has_value());
    EXPECT_EQ(caps.limits.vision->supported_media_types.size(), 2);
    EXPECT_EQ(caps.limits.vision->max_prompt_images, 5);
    EXPECT_EQ(caps.limits.vision->max_prompt_image_size, 10485760);
}

TEST(TypesTest, ModelCapabilitiesWithoutVisionLimits)
{
    json input = {
        {"supports", {{"vision", false}}},
        {"limits", {{"max_context_window_tokens", 32000}}}
    };

    auto caps = input.get<ModelCapabilities>();
    EXPECT_FALSE(caps.supports.vision);
    EXPECT_EQ(caps.limits.max_context_window_tokens, 32000);
    EXPECT_FALSE(caps.limits.vision.has_value());
}

TEST(TypesTest, ModelInfoWithVisionLimits)
{
    json input = {
        {"id", "gpt-4-vision"},
        {"name", "GPT-4 Vision"},
        {"capabilities",
         {{"supports", {{"vision", true}}},
          {"limits",
           {{"max_prompt_tokens", 4096},
            {"max_context_window_tokens", 128000},
            {"vision",
             {{"supportedMediaTypes", json::array({"image/png", "image/jpeg", "image/webp"})},
              {"maxPromptImages", 8},
              {"maxPromptImageSize", 20971520}}}}}}}
    };

    auto model = input.get<ModelInfo>();
    EXPECT_EQ(model.id, "gpt-4-vision");
    EXPECT_EQ(model.name, "GPT-4 Vision");
    EXPECT_TRUE(model.capabilities.supports.vision);
    ASSERT_TRUE(model.capabilities.limits.vision.has_value());
    EXPECT_EQ(model.capabilities.limits.vision->supported_media_types.size(), 3);
    EXPECT_EQ(model.capabilities.limits.vision->max_prompt_images, 8);
}

TEST(TypesTest, ModelInfoWithoutVisionLimits)
{
    json input = {
        {"id", "gpt-4"},
        {"name", "GPT-4"},
        {"capabilities",
         {{"supports", {{"vision", false}}},
          {"limits", {{"max_context_window_tokens", 32000}}}}}
    };

    auto model = input.get<ModelInfo>();
    EXPECT_EQ(model.id, "gpt-4");
    EXPECT_FALSE(model.capabilities.supports.vision);
    EXPECT_FALSE(model.capabilities.limits.vision.has_value());
}

// =============================================================================
// Gap 4: GetModelsResponse tests
// =============================================================================

TEST(TypesTest, GetModelsResponseParsing)
{
    json input = {
        {"models",
         json::array(
             {{{"id", "gpt-4"}, {"name", "GPT-4"}, {"capabilities", {{"supports", {{"vision", false}}}, {"limits", {{"max_context_window_tokens", 128000}}}}}},
              {{"id", "gpt-4-vision"}, {"name", "GPT-4 Vision"}, {"capabilities", {{"supports", {{"vision", true}}}, {"limits", {{"max_context_window_tokens", 128000}}}}}}})}
    };

    auto response = input.get<GetModelsResponse>();
    ASSERT_EQ(response.models.size(), 2);
    EXPECT_EQ(response.models[0].id, "gpt-4");
    EXPECT_EQ(response.models[1].id, "gpt-4-vision");
    EXPECT_FALSE(response.models[0].capabilities.supports.vision);
    EXPECT_TRUE(response.models[1].capabilities.supports.vision);
}

TEST(TypesTest, GetModelsResponseEmpty)
{
    json input = {{"models", json::array()}};

    auto response = input.get<GetModelsResponse>();
    EXPECT_TRUE(response.models.empty());
}

TEST(TypesTest, GetModelsResponseMissingModelsKey)
{
    json input = json::object();

    auto response = input.get<GetModelsResponse>();
    EXPECT_TRUE(response.models.empty());
}

// =============================================================================
// Cross-cutting: new event types interact with existing try_as/is
// =============================================================================

TEST(EventsTest, NewEventTypesTryAsWrongType)
{
    // SessionCompactionStart is not a SessionUsageInfoData
    json input = {
        {"id", "evt_cross_1"},
        {"timestamp", "2025-01-22T14:00:00Z"},
        {"type", "session.compaction_start"},
        {"data", json::object()}
    };

    auto event = input.get<SessionEvent>();
    EXPECT_NE(event.try_as<SessionCompactionStartData>(), nullptr);
    EXPECT_EQ(event.try_as<SessionUsageInfoData>(), nullptr);
    EXPECT_EQ(event.try_as<ToolExecutionProgressData>(), nullptr);
    EXPECT_EQ(event.try_as<SessionCompactionCompleteData>(), nullptr);
    EXPECT_EQ(event.try_as<CustomAgentStartedData>(), nullptr);
}

TEST(EventsTest, AllNewEventTypesInOneSequence)
{
    // Simulate a realistic sequence of events during compaction
    std::vector<json> events_json = {
        {{"id", "e1"},
         {"timestamp", "2025-01-22T15:00:00Z"},
         {"type", "session.usage_info"},
         {"data", {{"tokenLimit", 128000}, {"currentTokens", 120000}, {"messagesLength", 50}}}},
        {{"id", "e2"},
         {"timestamp", "2025-01-22T15:00:01Z"},
         {"type", "session.compaction_start"},
         {"data", json::object()}},
        {{"id", "e3"},
         {"timestamp", "2025-01-22T15:00:05Z"},
         {"type", "session.compaction_complete"},
         {"data", {{"success", true}, {"preCompactionTokens", 120000}, {"postCompactionTokens", 60000}}}},
        {{"id", "e4"},
         {"timestamp", "2025-01-22T15:00:06Z"},
         {"type", "tool.execution_progress"},
         {"data", {{"toolCallId", "tc_500"}, {"progressMessage", "Rebuilding index..."}}}},
        {{"id", "e5"},
         {"timestamp", "2025-01-22T15:00:07Z"},
         {"type", "subagent.started"},
         {"data",
          {{"toolCallId", "tc_600"},
           {"agentName", "helper"},
           {"agentDisplayName", "Helper"},
           {"agentDescription", "Helps out"}}}}
    };

    auto e1 = events_json[0].get<SessionEvent>();
    EXPECT_EQ(e1.type, SessionEventType::SessionUsageInfo);
    EXPECT_DOUBLE_EQ(e1.as<SessionUsageInfoData>().current_tokens, 120000);

    auto e2 = events_json[1].get<SessionEvent>();
    EXPECT_EQ(e2.type, SessionEventType::SessionCompactionStart);

    auto e3 = events_json[2].get<SessionEvent>();
    EXPECT_EQ(e3.type, SessionEventType::SessionCompactionComplete);
    EXPECT_TRUE(e3.as<SessionCompactionCompleteData>().success);

    auto e4 = events_json[3].get<SessionEvent>();
    EXPECT_EQ(e4.type, SessionEventType::ToolExecutionProgress);
    EXPECT_EQ(e4.as<ToolExecutionProgressData>().progress_message, "Rebuilding index...");

    auto e5 = events_json[4].get<SessionEvent>();
    EXPECT_EQ(e5.type, SessionEventType::CustomAgentStarted);
    EXPECT_EQ(e5.as<CustomAgentStartedData>().agent_name, "helper");
}

// =============================================================================
// v0.1.23 Parity Tests - Hook Types
// =============================================================================

TEST(HookTypesTest, PreToolUseHookInputFromJson)
{
    json j = {{"timestamp", 1234567890}, {"cwd", "/project"}, {"toolName", "read_file"},
              {"toolArgs", {{"path", "main.cpp"}}}};
    auto input = j.get<PreToolUseHookInput>();
    EXPECT_EQ(input.timestamp, 1234567890);
    EXPECT_EQ(input.cwd, "/project");
    EXPECT_EQ(input.tool_name, "read_file");
    EXPECT_TRUE(input.tool_args.has_value());
    EXPECT_EQ((*input.tool_args)["path"], "main.cpp");
}

TEST(HookTypesTest, PreToolUseHookOutputToJson)
{
    PreToolUseHookOutput output;
    output.permission_decision = "allow";
    output.permission_decision_reason = "trusted tool";
    output.additional_context = "extra info";
    output.suppress_output = false;
    output.modified_args = json{{"path", "new.cpp"}};

    json j;
    to_json(j, output);
    EXPECT_EQ(j["permissionDecision"], "allow");
    EXPECT_EQ(j["permissionDecisionReason"], "trusted tool");
    EXPECT_EQ(j["additionalContext"], "extra info");
    EXPECT_EQ(j["suppressOutput"], false);
    EXPECT_EQ(j["modifiedArgs"]["path"], "new.cpp");
}

TEST(HookTypesTest, PreToolUseHookOutputToJsonMinimal)
{
    PreToolUseHookOutput output;
    output.permission_decision = "deny";

    json j;
    to_json(j, output);
    EXPECT_EQ(j["permissionDecision"], "deny");
    EXPECT_FALSE(j.contains("permissionDecisionReason"));
    EXPECT_FALSE(j.contains("modifiedArgs"));
}

TEST(HookTypesTest, PostToolUseHookInputFromJson)
{
    json j = {{"timestamp", 9999}, {"cwd", "/home"}, {"toolName", "write_file"},
              {"toolArgs", {{"path", "out.txt"}}}, {"toolResult", {{"content", "ok"}}}};
    auto input = j.get<PostToolUseHookInput>();
    EXPECT_EQ(input.tool_name, "write_file");
    EXPECT_TRUE(input.tool_result.has_value());
    EXPECT_EQ((*input.tool_result)["content"], "ok");
}

TEST(HookTypesTest, PostToolUseHookOutputToJson)
{
    PostToolUseHookOutput output;
    output.modified_result = json{{"content", "modified"}};
    output.additional_context = "hook context";
    output.suppress_output = true;

    json j;
    to_json(j, output);
    EXPECT_EQ(j["modifiedResult"]["content"], "modified");
    EXPECT_EQ(j["additionalContext"], "hook context");
    EXPECT_EQ(j["suppressOutput"], true);
}

TEST(HookTypesTest, UserPromptSubmittedHookInputFromJson)
{
    json j = {{"timestamp", 42}, {"cwd", "/work"}, {"prompt", "Hello world"}};
    auto input = j.get<UserPromptSubmittedHookInput>();
    EXPECT_EQ(input.prompt, "Hello world");
    EXPECT_EQ(input.cwd, "/work");
}

TEST(HookTypesTest, UserPromptSubmittedHookOutputToJson)
{
    UserPromptSubmittedHookOutput output;
    output.modified_prompt = "Modified prompt";
    output.suppress_output = false;

    json j;
    to_json(j, output);
    EXPECT_EQ(j["modifiedPrompt"], "Modified prompt");
    EXPECT_FALSE(j.contains("additionalContext"));
}

TEST(HookTypesTest, SessionStartHookInputFromJson)
{
    json j = {{"timestamp", 100}, {"cwd", "/app"}, {"source", "new"}, {"initialPrompt", "Fix bug"}};
    auto input = j.get<SessionStartHookInput>();
    EXPECT_EQ(input.source, "new");
    EXPECT_EQ(input.initial_prompt.value(), "Fix bug");
}

TEST(HookTypesTest, SessionStartHookOutputToJson)
{
    SessionStartHookOutput output;
    output.additional_context = "config loaded";
    output.modified_config = {{"model", json("gpt-4")}};

    json j;
    to_json(j, output);
    EXPECT_EQ(j["additionalContext"], "config loaded");
    EXPECT_EQ(j["modifiedConfig"]["model"], "gpt-4");
}

TEST(HookTypesTest, SessionEndHookInputFromJson)
{
    json j = {{"timestamp", 200}, {"cwd", "/app"}, {"reason", "complete"},
              {"finalMessage", "Done"}, {"error", nullptr}};
    auto input = j.get<SessionEndHookInput>();
    EXPECT_EQ(input.reason, "complete");
    EXPECT_EQ(input.final_message.value(), "Done");
    EXPECT_FALSE(input.error.has_value());
}

TEST(HookTypesTest, SessionEndHookOutputToJson)
{
    SessionEndHookOutput output;
    output.suppress_output = true;
    output.cleanup_actions = {"rm -rf tmp"};
    output.session_summary = "Completed task";

    json j;
    to_json(j, output);
    EXPECT_EQ(j["suppressOutput"], true);
    EXPECT_EQ(j["cleanupActions"][0], "rm -rf tmp");
    EXPECT_EQ(j["sessionSummary"], "Completed task");
}

TEST(HookTypesTest, ErrorOccurredHookInputFromJson)
{
    json j = {{"timestamp", 300}, {"cwd", "/err"}, {"error", "connection lost"},
              {"errorContext", "model_call"}, {"recoverable", true}};
    auto input = j.get<ErrorOccurredHookInput>();
    EXPECT_EQ(input.error, "connection lost");
    EXPECT_EQ(input.error_context, "model_call");
    EXPECT_TRUE(input.recoverable);
}

TEST(HookTypesTest, ErrorOccurredHookOutputToJson)
{
    ErrorOccurredHookOutput output;
    output.error_handling = "retry";
    output.retry_count = 3;
    output.user_notification = "Retrying...";

    json j;
    to_json(j, output);
    EXPECT_EQ(j["errorHandling"], "retry");
    EXPECT_EQ(j["retryCount"], 3);
    EXPECT_EQ(j["userNotification"], "Retrying...");
}

TEST(HookTypesTest, SessionHooksHasAny)
{
    SessionHooks hooks;
    EXPECT_FALSE(hooks.has_any());

    hooks.on_pre_tool_use = [](const PreToolUseHookInput&, const HookInvocation&) {
        return std::optional<PreToolUseHookOutput>(PreToolUseHookOutput{});
    };
    EXPECT_TRUE(hooks.has_any());
}

// =============================================================================
// v0.1.23 Parity Tests - User Input Types
// =============================================================================

TEST(UserInputTypesTest, UserInputRequestFromJson)
{
    json j = {{"question", "Pick color"}, {"choices", {"red", "blue"}}, {"allowFreeform", true}};
    auto req = j.get<UserInputRequest>();
    EXPECT_EQ(req.question, "Pick color");
    ASSERT_TRUE(req.choices.has_value());
    EXPECT_EQ(req.choices->size(), 2);
    EXPECT_EQ((*req.choices)[0], "red");
    EXPECT_TRUE(req.allow_freeform.value());
}

TEST(UserInputTypesTest, UserInputRequestToJson)
{
    UserInputRequest req;
    req.question = "Choose";
    req.choices = {"a", "b", "c"};

    json j;
    to_json(j, req);
    EXPECT_EQ(j["question"], "Choose");
    EXPECT_EQ(j["choices"].size(), 3);
    EXPECT_FALSE(j.contains("allowFreeform"));
}

TEST(UserInputTypesTest, UserInputRequestMinimal)
{
    json j = {{"question", "What?"}};
    auto req = j.get<UserInputRequest>();
    EXPECT_EQ(req.question, "What?");
    EXPECT_FALSE(req.choices.has_value());
    EXPECT_FALSE(req.allow_freeform.has_value());
}

TEST(UserInputTypesTest, UserInputResponseRoundTrip)
{
    UserInputResponse resp;
    resp.answer = "blue";
    resp.was_freeform = false;

    json j;
    to_json(j, resp);
    EXPECT_EQ(j["answer"], "blue");
    EXPECT_EQ(j["wasFreeform"], false);

    auto resp2 = j.get<UserInputResponse>();
    EXPECT_EQ(resp2.answer, "blue");
    EXPECT_FALSE(resp2.was_freeform);
}

// =============================================================================
// v0.1.23 Parity Tests - Reasoning Effort
// =============================================================================

TEST(ReasoningEffortTest, ModelSupportsReasoningEffort)
{
    json j = {{"capabilities", {{"supports", {{"vision", false}, {"reasoningEffort", true}}}}}};
    auto caps = j["capabilities"].get<ModelCapabilities>();
    EXPECT_TRUE(caps.supports.reasoning_effort);
    EXPECT_FALSE(caps.supports.vision);
}

TEST(ReasoningEffortTest, ModelInfoWithReasoningEfforts)
{
    json j = {{"id", "model-1"}, {"name", "Model 1"},
              {"capabilities", {{"supports", {{"reasoningEffort", true}}}}},
              {"supportedReasoningEfforts", {"low", "medium", "high"}},
              {"defaultReasoningEffort", "medium"}};
    auto info = j.get<ModelInfo>();
    EXPECT_TRUE(info.capabilities.supports.reasoning_effort);
    ASSERT_TRUE(info.supported_reasoning_efforts.has_value());
    EXPECT_EQ(info.supported_reasoning_efforts->size(), 3);
    EXPECT_EQ(info.default_reasoning_effort.value(), "medium");
}

TEST(ReasoningEffortTest, SessionConfigReasoningEffort)
{
    SessionConfig config;
    config.reasoning_effort = ReasoningEffort::High;
    auto request = build_session_create_request(config);
    EXPECT_EQ(request["reasoningEffort"], "high");
}

TEST(ReasoningEffortTest, ResumeConfigReasoningEffort)
{
    ResumeSessionConfig config;
    config.reasoning_effort = ReasoningEffort::Low;
    auto request = build_session_resume_request("test-session", config);
    EXPECT_EQ(request["reasoningEffort"], "low");
}

// =============================================================================
// v0.1.23 Parity Tests - New Event Types
// =============================================================================

TEST(NewEventsTest, SessionSnapshotRewindEvent)
{
    json event_json = {{"type", "session.snapshot_rewind"}, {"id", "e1"},
                       {"timestamp", "2025-01-01T00:00:00Z"},
                       {"data", {{"upToEventId", "evt-42"}, {"eventsRemoved", 5}}}};
    auto event = parse_session_event(event_json);
    EXPECT_EQ(event.type, SessionEventType::SessionSnapshotRewind);
    auto& data = event.as<SessionSnapshotRewindData>();
    EXPECT_EQ(data.up_to_event_id, "evt-42");
    EXPECT_EQ(data.events_removed, 5);
}

TEST(NewEventsTest, SessionShutdownEvent)
{
    json event_json = {
        {"type", "session.shutdown"}, {"id", "e2"},
        {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {
            {"shutdownType", "routine"},
            {"totalPremiumRequests", 10},
            {"totalApiDurationMs", 5000},
            {"sessionStartTime", 1700000000},
            {"codeChanges", {{"linesAdded", 42}, {"linesRemoved", 3}, {"filesModified", {"main.cpp", "util.h"}}}},
            {"modelMetrics", {{"gpt-4", {{"calls", 5}}}}},
            {"currentModel", "gpt-4"}
        }}
    };
    auto event = parse_session_event(event_json);
    EXPECT_EQ(event.type, SessionEventType::SessionShutdown);
    auto& data = event.as<SessionShutdownData>();
    EXPECT_EQ(data.shutdown_type, ShutdownType::Routine);
    EXPECT_EQ(data.total_premium_requests, 10);
    EXPECT_EQ(data.code_changes.lines_added, 42);
    EXPECT_EQ(data.code_changes.files_modified.size(), 2);
    EXPECT_EQ(data.current_model.value(), "gpt-4");
}

TEST(NewEventsTest, SessionShutdownErrorType)
{
    json event_json = {
        {"type", "session.shutdown"}, {"id", "e3"},
        {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {
            {"shutdownType", "error"},
            {"errorReason", "connection lost"},
            {"totalPremiumRequests", 0}, {"totalApiDurationMs", 0}, {"sessionStartTime", 0},
            {"codeChanges", {{"linesAdded", 0}, {"linesRemoved", 0}, {"filesModified", json::array()}}},
            {"modelMetrics", json::object()}
        }}
    };
    auto event = parse_session_event(event_json);
    auto& data = event.as<SessionShutdownData>();
    EXPECT_EQ(data.shutdown_type, ShutdownType::Error);
    EXPECT_EQ(data.error_reason.value(), "connection lost");
}

TEST(NewEventsTest, SkillInvokedEvent)
{
    json event_json = {
        {"type", "skill.invoked"}, {"id", "e4"},
        {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {{"name", "code-review"}, {"path", "/skills/review.md"},
                  {"content", "Review the code"}, {"allowedTools", {"read_file", "grep"}}}}
    };
    auto event = parse_session_event(event_json);
    EXPECT_EQ(event.type, SessionEventType::SkillInvoked);
    auto& data = event.as<SkillInvokedData>();
    EXPECT_EQ(data.name, "code-review");
    EXPECT_EQ(data.path, "/skills/review.md");
    EXPECT_EQ(data.content, "Review the code");
    ASSERT_TRUE(data.allowed_tools.has_value());
    EXPECT_EQ(data.allowed_tools->size(), 2);
}

TEST(NewEventsTest, SkillInvokedNoAllowedTools)
{
    json event_json = {
        {"type", "skill.invoked"}, {"id", "e5"},
        {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {{"name", "fix"}, {"path", "/fix.md"}, {"content", "fix it"}}}
    };
    auto event = parse_session_event(event_json);
    auto& data = event.as<SkillInvokedData>();
    EXPECT_FALSE(data.allowed_tools.has_value());
}

// =============================================================================
// v0.1.23 Parity Tests - Extended Event Fields
// =============================================================================

TEST(ExtendedFieldsTest, SessionErrorDataExtendedFields)
{
    json event_json = {
        {"type", "session.error"}, {"id", "e1"}, {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {{"errorType", "api_error"}, {"message", "rate limited"},
                  {"statusCode", 429}, {"providerCallId", "call-123"}}}
    };
    auto event = parse_session_event(event_json);
    auto& data = event.as<SessionErrorData>();
    EXPECT_EQ(data.status_code.value(), 429);
    EXPECT_EQ(data.provider_call_id.value(), "call-123");
}

TEST(ExtendedFieldsTest, SessionErrorDataWithoutExtended)
{
    json event_json = {
        {"type", "session.error"}, {"id", "e1"}, {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {{"errorType", "generic"}, {"message", "oops"}}}
    };
    auto event = parse_session_event(event_json);
    auto& data = event.as<SessionErrorData>();
    EXPECT_FALSE(data.status_code.has_value());
    EXPECT_FALSE(data.provider_call_id.has_value());
}

TEST(ExtendedFieldsTest, AssistantMessageDataExtendedFields)
{
    json event_json = {
        {"type", "assistant.message"}, {"id", "e1"}, {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {{"messageId", "m1"}, {"content", "Hello"},
                  {"reasoningOpaque", "opaque-data"}, {"reasoningText", "I think..."},
                  {"encryptedContent", "encrypted-blob"}}}
    };
    auto event = parse_session_event(event_json);
    auto& data = event.as<AssistantMessageData>();
    EXPECT_EQ(data.reasoning_opaque.value(), "opaque-data");
    EXPECT_EQ(data.reasoning_text.value(), "I think...");
    EXPECT_EQ(data.encrypted_content.value(), "encrypted-blob");
}

TEST(ExtendedFieldsTest, AssistantUsageDataParentToolCallId)
{
    json event_json = {
        {"type", "assistant.usage"}, {"id", "e1"}, {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {{"model", "gpt-4"}, {"inputTokens", 100}, {"parentToolCallId", "tc-42"}}}
    };
    auto event = parse_session_event(event_json);
    auto& data = event.as<AssistantUsageData>();
    EXPECT_EQ(data.parent_tool_call_id.value(), "tc-42");
}

TEST(ExtendedFieldsTest, ToolExecutionStartDataMcpFields)
{
    json event_json = {
        {"type", "tool.execution_start"}, {"id", "e1"}, {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {{"toolCallId", "tc1"}, {"toolName", "mcp-read"},
                  {"mcpServerName", "filesystem"}, {"mcpToolName", "read_file"}}}
    };
    auto event = parse_session_event(event_json);
    auto& data = event.as<ToolExecutionStartData>();
    EXPECT_EQ(data.mcp_server_name.value(), "filesystem");
    EXPECT_EQ(data.mcp_tool_name.value(), "read_file");
}

TEST(ExtendedFieldsTest, ToolResultContentDetailedContent)
{
    json j = {{"content", "summary"}, {"detailedContent", "full details here"}};
    auto result = j.get<ToolResultContent>();
    EXPECT_EQ(result.content, "summary");
    EXPECT_EQ(result.detailed_content.value(), "full details here");

    json j2;
    to_json(j2, result);
    EXPECT_EQ(j2["detailedContent"], "full details here");
}

TEST(ExtendedFieldsTest, SessionCompactionCompleteExtendedFields)
{
    json event_json = {
        {"type", "session.compaction_complete"}, {"id", "e1"}, {"timestamp", "2025-01-01T00:00:00Z"},
        {"data", {{"success", true}, {"messagesRemoved", 15}, {"tokensRemoved", 2500},
                  {"summaryContent", "Session summary"}, {"checkpointNumber", 3},
                  {"checkpointPath", "/workspace/checkpoints/003"},
                  {"preCompactionTokens", 10000}, {"postCompactionTokens", 5000}}}
    };
    auto event = parse_session_event(event_json);
    auto& data = event.as<SessionCompactionCompleteData>();
    EXPECT_EQ(data.messages_removed.value(), 15);
    EXPECT_EQ(data.tokens_removed.value(), 2500);
    EXPECT_EQ(data.summary_content.value(), "Session summary");
    EXPECT_EQ(data.checkpoint_number.value(), 3);
    EXPECT_EQ(data.checkpoint_path.value(), "/workspace/checkpoints/003");
}

// =============================================================================
// v0.1.23 Parity Tests - Selection Attachment
// =============================================================================

TEST(SelectionAttachmentTest, SelectionPositionRoundTrip)
{
    SelectionPosition pos{.line = 10, .character = 5};
    json j;
    to_json(j, pos);
    EXPECT_EQ(j["line"], 10);
    EXPECT_EQ(j["character"], 5);

    auto pos2 = j.get<SelectionPosition>();
    EXPECT_EQ(pos2.line, 10);
    EXPECT_EQ(pos2.character, 5);
}

TEST(SelectionAttachmentTest, SelectionRangeRoundTrip)
{
    SelectionRange range{
        .start = {.line = 1, .character = 0},
        .end = {.line = 5, .character = 20}
    };
    json j;
    to_json(j, range);
    EXPECT_EQ(j["start"]["line"], 1);
    EXPECT_EQ(j["end"]["character"], 20);

    auto range2 = j.get<SelectionRange>();
    EXPECT_EQ(range2.start.line, 1);
    EXPECT_EQ(range2.end.character, 20);
}

TEST(SelectionAttachmentTest, SelectionAttachmentRoundTrip)
{
    SelectionAttachment att{
        .file_path = "/src/main.cpp",
        .display_name = "main.cpp:1-5",
        .text = "int main() { return 0; }",
        .selection = {.start = {.line = 1, .character = 0}, .end = {.line = 1, .character = 24}}
    };
    json j;
    to_json(j, att);
    EXPECT_EQ(j["type"], "selection");
    EXPECT_EQ(j["filePath"], "/src/main.cpp");
    EXPECT_EQ(j["text"], "int main() { return 0; }");

    auto att2 = j.get<SelectionAttachment>();
    EXPECT_EQ(att2.file_path, "/src/main.cpp");
    EXPECT_EQ(att2.selection.start.line, 1);
}

TEST(SelectionAttachmentTest, UserAttachmentTypeSelection)
{
    json j = "selection";
    auto type = j.get<UserAttachmentType>();
    EXPECT_EQ(type, UserAttachmentType::Selection);
}

// =============================================================================
// v0.1.23 Parity Tests - Session Lifecycle Types
// =============================================================================

TEST(SessionLifecycleTest, SessionLifecycleEventFromJson)
{
    json j = {{"type", "session.created"}, {"sessionId", "sess-1"},
              {"metadata", {{"startTime", "2025-01-01T00:00:00Z"},
                           {"modifiedTime", "2025-01-01T01:00:00Z"},
                           {"summary", "Test session"}}}};
    auto event = j.get<SessionLifecycleEvent>();
    EXPECT_EQ(event.type, SessionLifecycleEventTypes::Created);
    EXPECT_EQ(event.session_id, "sess-1");
    ASSERT_TRUE(event.metadata.has_value());
    EXPECT_EQ(event.metadata->summary.value(), "Test session");
}

TEST(SessionLifecycleTest, GetForegroundSessionResponseFromJson)
{
    json j = {{"sessionId", "fg-1"}, {"workspacePath", "/workspace"}};
    auto resp = j.get<GetForegroundSessionResponse>();
    EXPECT_EQ(resp.session_id.value(), "fg-1");
    EXPECT_EQ(resp.workspace_path.value(), "/workspace");
}

TEST(SessionLifecycleTest, SetForegroundSessionResponseFromJson)
{
    json j = {{"success", true}};
    auto resp = j.get<SetForegroundSessionResponse>();
    EXPECT_TRUE(resp.success);
    EXPECT_FALSE(resp.error.has_value());
}

TEST(SessionLifecycleTest, SetForegroundSessionResponseError)
{
    json j = {{"success", false}, {"error", "session not found"}};
    auto resp = j.get<SetForegroundSessionResponse>();
    EXPECT_FALSE(resp.success);
    EXPECT_EQ(resp.error.value(), "session not found");
}

// =============================================================================
// v0.1.23 Parity Tests - Client Auth Options
// =============================================================================

TEST(ClientAuthTest, GithubTokenWithCliUrlThrows)
{
    ClientOptions opts;
    opts.cli_url = "http://localhost:3000";
    opts.use_stdio = false;
    opts.github_token = "ghp_abc123";
    bool threw = false;
    try {
        Client c(opts);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw) << "Expected std::invalid_argument for github_token + cli_url";
}

TEST(ClientAuthTest, UseLoggedInUserWithCliUrlThrows)
{
    ClientOptions opts;
    opts.cli_url = "http://localhost:3000";
    opts.use_stdio = false;
    opts.use_logged_in_user = true;
    bool threw = false;
    try {
        Client c(opts);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    EXPECT_TRUE(threw) << "Expected std::invalid_argument for use_logged_in_user + cli_url";
}

// =============================================================================
// v0.1.23 Parity Tests - Request Builder Config Fields
// =============================================================================

TEST(RequestBuilderTest, CreateSessionWithHooksAndUserInput)
{
    SessionConfig config;
    config.on_user_input_request = [](const UserInputRequest&, const UserInputInvocation&) {
        return UserInputResponse{.answer = "yes"};
    };
    SessionHooks hooks;
    hooks.on_pre_tool_use = [](const PreToolUseHookInput&, const HookInvocation&) {
        return std::optional<PreToolUseHookOutput>(PreToolUseHookOutput{.permission_decision = "allow"});
    };
    config.hooks = hooks;
    config.working_directory = "/project";

    auto request = build_session_create_request(config);
    EXPECT_TRUE(request["requestUserInput"].get<bool>());
    EXPECT_TRUE(request["hooks"].get<bool>());
    EXPECT_EQ(request["workingDirectory"], "/project");
}

TEST(RequestBuilderTest, CreateSessionWithoutHooksOmitsField)
{
    SessionConfig config;
    auto request = build_session_create_request(config);
    EXPECT_FALSE(request.contains("requestUserInput"));
    EXPECT_FALSE(request.contains("hooks"));
    EXPECT_FALSE(request.contains("workingDirectory"));
    EXPECT_FALSE(request.contains("reasoningEffort"));
}

TEST(RequestBuilderTest, ResumeSessionAllNewFields)
{
    ResumeSessionConfig config;
    config.model = "gpt-4o";
    config.reasoning_effort = ReasoningEffort::High;
    config.system_message = SystemMessageConfig{.content = "Be helpful"};
    config.available_tools = {"read_file"};
    config.excluded_tools = {"dangerous_tool"};
    config.working_directory = "/work";
    config.disable_resume = true;
    config.infinite_sessions = InfiniteSessionConfig{.enabled = true};
    config.on_user_input_request = [](const UserInputRequest&, const UserInputInvocation&) {
        return UserInputResponse{};
    };
    SessionHooks hooks;
    hooks.on_session_start = [](const SessionStartHookInput&, const HookInvocation&) {
        return std::optional<SessionStartHookOutput>(std::nullopt);
    };
    config.hooks = hooks;

    auto request = build_session_resume_request("sess-1", config);
    EXPECT_EQ(request["model"], "gpt-4o");
    EXPECT_EQ(request["reasoningEffort"], "high");
    EXPECT_TRUE(request.contains("systemMessage"));
    EXPECT_EQ(request["availableTools"][0], "read_file");
    EXPECT_EQ(request["excludedTools"][0], "dangerous_tool");
    EXPECT_EQ(request["workingDirectory"], "/work");
    EXPECT_TRUE(request["disableResume"].get<bool>());
    EXPECT_TRUE(request.contains("infiniteSessions"));
    EXPECT_TRUE(request["requestUserInput"].get<bool>());
    EXPECT_TRUE(request["hooks"].get<bool>());
}

TEST(RequestBuilderTest, EmptyHooksNotSent)
{
    SessionConfig config;
    config.hooks = SessionHooks{};  // Empty hooks - has_any() is false
    auto request = build_session_create_request(config);
    EXPECT_FALSE(request.contains("hooks"));
}

// =============================================================================
// v0.1.23 Parity Tests - Session Hooks Handler Integration
// =============================================================================

TEST(SessionHooksTest, HandleHooksInvokePreToolUse)
{
    auto session = std::make_shared<Session>("test-session", nullptr);

    bool handler_called = false;
    SessionHooks hooks;
    hooks.on_pre_tool_use = [&](const PreToolUseHookInput& input, const HookInvocation& inv) {
        handler_called = true;
        EXPECT_EQ(input.tool_name, "read_file");
        EXPECT_EQ(inv.session_id, "test-session");
        return std::optional<PreToolUseHookOutput>(PreToolUseHookOutput{.permission_decision = "allow"});
    };
    session->register_hooks(hooks);

    json input = {{"toolName", "read_file"}, {"timestamp", 123}, {"cwd", "/"}};
    auto result = session->handle_hooks_invoke("preToolUse", input);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(result["permissionDecision"], "allow");
}

TEST(SessionHooksTest, HandleHooksInvokePostToolUse)
{
    auto session = std::make_shared<Session>("test-session", nullptr);

    SessionHooks hooks;
    hooks.on_post_tool_use = [](const PostToolUseHookInput& input, const HookInvocation&) {
        EXPECT_EQ(input.tool_name, "write_file");
        return std::optional<PostToolUseHookOutput>(std::nullopt);
    };
    session->register_hooks(hooks);

    json input = {{"toolName", "write_file"}, {"timestamp", 456}, {"cwd", "/"}};
    auto result = session->handle_hooks_invoke("postToolUse", input);
    EXPECT_TRUE(result.is_null());
}

TEST(SessionHooksTest, HandleHooksInvokeNoHandler)
{
    auto session = std::make_shared<Session>("test-session", nullptr);

    json input = {{"toolName", "test"}};
    auto result = session->handle_hooks_invoke("preToolUse", input);
    EXPECT_TRUE(result.is_null());
}

TEST(SessionHooksTest, HandleHooksInvokeAllTypes)
{
    auto session = std::make_shared<Session>("test-session", nullptr);

    int calls = 0;
    SessionHooks hooks;
    hooks.on_pre_tool_use = [&](const PreToolUseHookInput&, const HookInvocation&) {
        calls++; return std::optional<PreToolUseHookOutput>(std::nullopt);
    };
    hooks.on_post_tool_use = [&](const PostToolUseHookInput&, const HookInvocation&) {
        calls++; return std::optional<PostToolUseHookOutput>(std::nullopt);
    };
    hooks.on_user_prompt_submitted = [&](const UserPromptSubmittedHookInput&, const HookInvocation&) {
        calls++; return std::optional<UserPromptSubmittedHookOutput>(std::nullopt);
    };
    hooks.on_session_start = [&](const SessionStartHookInput&, const HookInvocation&) {
        calls++; return std::optional<SessionStartHookOutput>(std::nullopt);
    };
    hooks.on_session_end = [&](const SessionEndHookInput&, const HookInvocation&) {
        calls++; return std::optional<SessionEndHookOutput>(std::nullopt);
    };
    hooks.on_error_occurred = [&](const ErrorOccurredHookInput&, const HookInvocation&) {
        calls++; return std::optional<ErrorOccurredHookOutput>(std::nullopt);
    };
    session->register_hooks(hooks);

    session->handle_hooks_invoke("preToolUse", {{"toolName", "t"}, {"timestamp", 0}, {"cwd", "/"}});
    session->handle_hooks_invoke("postToolUse", {{"toolName", "t"}, {"timestamp", 0}, {"cwd", "/"}});
    session->handle_hooks_invoke("userPromptSubmitted", {{"prompt", "hi"}, {"timestamp", 0}, {"cwd", "/"}});
    session->handle_hooks_invoke("sessionStart", {{"source", "new"}, {"timestamp", 0}, {"cwd", "/"}});
    session->handle_hooks_invoke("sessionEnd", {{"reason", "complete"}, {"timestamp", 0}, {"cwd", "/"}});
    session->handle_hooks_invoke("errorOccurred", {{"error", "err"}, {"errorContext", "system"}, {"timestamp", 0}, {"cwd", "/"}, {"recoverable", false}});

    EXPECT_EQ(calls, 6);
}

// =============================================================================
// v0.1.23 Parity Tests - User Input Handler Integration
// =============================================================================

TEST(UserInputHandlerTest, HandleUserInputRequest)
{
    auto session = std::make_shared<Session>("test-session", nullptr);

    session->register_user_input_handler(
        [](const UserInputRequest& req, const UserInputInvocation& inv) {
            EXPECT_EQ(req.question, "Pick a color");
            EXPECT_EQ(inv.session_id, "test-session");
            return UserInputResponse{.answer = "blue", .was_freeform = true};
        }
    );

    UserInputRequest request;
    request.question = "Pick a color";
    request.choices = {"red", "blue"};

    auto response = session->handle_user_input_request(request);
    EXPECT_EQ(response.answer, "blue");
    EXPECT_TRUE(response.was_freeform);
}

TEST(UserInputHandlerTest, NoHandlerThrows)
{
    auto session = std::make_shared<Session>("test-session", nullptr);

    UserInputRequest request;
    request.question = "test";
    EXPECT_THROW(session->handle_user_input_request(request), std::runtime_error);
}

// =============================================================================
// Event Forward-Compatibility Tests
// =============================================================================

TEST(Events, UnknownEventTypeHandled)
{
    json event_json = {
        {"type", "some.future.event"},
        {"id", "e-unknown"},
        {"timestamp", "2025-06-01T00:00:00Z"},
        {"data", {{"foo", "bar"}}}
    };

    // Parsing an unknown event type should not throw
    EXPECT_NO_THROW({
        auto event = parse_session_event(event_json);
        EXPECT_EQ(event.type, SessionEventType::Unknown);
        EXPECT_EQ(event.type_string, "some.future.event");
    });
}

TEST(Events, SessionShutdownParsed)
{
    json event_json = {
        {"type", "session.shutdown"},
        {"id", "e-shutdown"},
        {"timestamp", "2025-06-01T00:00:00Z"},
        {"data", {
            {"shutdownType", "routine"},
            {"totalPremiumRequests", 5},
            {"totalApiDurationMs", 1234.5},
            {"sessionStartTime", 1700000000.0},
            {"codeChanges", {{"linesAdded", 10}, {"linesRemoved", 3}}}
        }}
    };

    auto event = parse_session_event(event_json);
    EXPECT_EQ(event.type, SessionEventType::SessionShutdown);
    EXPECT_TRUE(event.is<SessionShutdownData>());

    const auto& data = event.as<SessionShutdownData>();
    EXPECT_EQ(data.total_premium_requests, 5);
    EXPECT_NEAR(data.total_api_duration_ms, 1234.5, 0.1);
}

TEST(Events, SessionUsageInfoRecognized)
{
    json event_json = {
        {"type", "session.usage_info"},
        {"id", "e-usage"},
        {"timestamp", "2025-06-01T00:00:00Z"},
        {"data", {
            {"tokenLimit", 128000},
            {"currentTokens", 5000},
            {"messagesLength", 42}
        }}
    };

    auto event = parse_session_event(event_json);
    EXPECT_EQ(event.type, SessionEventType::SessionUsageInfo);
    EXPECT_TRUE(event.is<SessionUsageInfoData>());

    const auto& data = event.as<SessionUsageInfoData>();
    EXPECT_EQ(data.token_limit, 128000);
    EXPECT_EQ(data.current_tokens, 5000);
}
