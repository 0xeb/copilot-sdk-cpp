// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <copilot/types.hpp>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace copilot
{

// =============================================================================
// Event Data Types
// =============================================================================

// Forward declare all data types
struct SessionStartData;
struct SessionResumeData;
struct SessionErrorData;
struct SessionIdleData;
struct SessionInfoData;
struct SessionModelChangeData;
struct SessionHandoffData;
struct SessionTruncationData;
struct UserMessageData;
struct PendingMessagesModifiedData;
struct AssistantTurnStartData;
struct AssistantIntentData;
struct AssistantReasoningData;
struct AssistantReasoningDeltaData;
struct AssistantMessageData;
struct AssistantMessageDeltaData;
struct AssistantTurnEndData;
struct AssistantUsageData;
struct AbortData;
struct ToolUserRequestedData;
struct ToolExecutionStartData;
struct ToolExecutionPartialResultData;
struct ToolExecutionCompleteData;
struct SessionCompactionStartData;
struct SessionCompactionCompleteData;
struct SessionUsageInfoData;
struct ToolExecutionProgressData;
struct CustomAgentStartedData;
struct CustomAgentCompletedData;
struct CustomAgentFailedData;
struct CustomAgentSelectedData;
struct HookStartData;
struct HookEndData;
struct SystemMessageData;
struct SessionSnapshotRewindData;
struct SessionShutdownData;
struct SkillInvokedData;
// v0.1.49+ additions
struct WorkingDirectoryContext;
struct SessionRemoteSteerableChangedData;
struct SessionTitleChangedData;
struct SessionScheduleCreatedData;
struct SessionScheduleCancelledData;
struct SessionWarningData;
struct SessionModeChangedData;
struct SessionPlanChangedData;
struct SessionWorkspaceFileChangedData;
struct SessionContextChangedData;
struct SessionTaskCompleteData;
struct SessionCustomNotificationData;
struct SessionToolsUpdatedData;
struct SessionBackgroundTasksChangedData;
struct SessionSkillsLoadedData;
struct SessionCustomAgentsUpdatedData;
struct SessionMcpServersLoadedData;
struct SessionMcpServerStatusChangedData;
struct SessionExtensionsLoadedData;
struct AssistantStreamingDeltaData;
struct AssistantMessageStartData;
struct ModelCallFailureData;
struct SubagentDeselectedData;
struct PermissionRequestedData;
struct PermissionCompletedData;
struct UserInputRequestedData;
struct UserInputCompletedData;
struct ElicitationRequestedData;
struct ElicitationCompletedData;
struct SamplingRequestedData;
struct SamplingCompletedData;

// --- Protocol surface additions: wire types present in the generated catalog ---
struct ScheduleRearmedData;
struct AutopilotObjectiveChangedData;
struct SessionLimitsChangedData;
struct PermissionsChangedData;
struct TodosChangedData;
struct MemoryChangedData;
struct UsageCheckpointData;
struct ContextClearedData;
struct FusionRouteStartedData;
struct FusionRouteFailedData;
struct FusionResolvedData;
struct FusionHandoffData;
struct FusionCommitStartedData;
struct FusionCompletedData;
struct AssistantTurnRetryData;
struct AgentInterruptedData;
struct FusionPhaseStartedData;
struct FusionPhaseCompletedData;
struct FusionPhaseFailedData;
struct AssistantServerToolProgressData;
struct AssistantToolCallDeltaData;
struct AssistantIdleData;
struct PromptCacheBreakData;
struct ModelCallFinishedData;
struct ModelCallStartData;
struct ToolSearchActivatedData;
struct SandboxDecisionData;
struct SubagentConfiguredData;
struct HookProgressData;
struct BinaryAssetData;
struct McpHeadersRefreshRequiredData;
struct McpHeadersRefreshCompletedData;
struct UIEphemeralQueryData;
struct SessionLimitsExhaustedRequestedData;
struct SessionLimitsExhaustedCompletedData;
struct AutoModeResolvedData;
struct ManagedSettingsResolvedData;
struct ManagedSettingsEnforcedData;
struct FactoryRunUpdatedData;
struct FactoryRunStartedData;
struct FactoryRunSettledData;
struct McpListChangedData;
struct CanvasOpenedData;
struct CanvasRegistryChangedData;
struct CanvasClosedData;
struct CanvasUnavailableData;
struct CanvasRecordedData;
struct CanvasRemovedData;
struct ExtensionsAttachmentsPushedData;
struct McpAppToolCallCompleteData;
struct McpOauthRequiredData;
struct McpOauthCompletedData;
struct ExternalToolRequestedData;
struct ExternalToolCompletedData;
struct CommandQueuedData;
struct CommandExecuteData;
struct CommandCompletedData;
struct AutoModeSwitchRequestedData;
struct AutoModeSwitchCompletedData;
struct CommandsChangedData;
struct CapabilitiesChangedData;
struct ExitPlanModeRequestedData;
struct ExitPlanModeCompletedData;
struct SystemNotificationData;

// =============================================================================
// Nested Types
// =============================================================================

/// Handoff source type
enum class HandoffSourceType
{
    Remote,
    Local
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    HandoffSourceType,
    {
        {HandoffSourceType::Remote, "remote"},
        {HandoffSourceType::Local, "local"},
    }
)

/// Attachment type for user messages
enum class UserAttachmentType
{
    File,
    Directory,
    Selection
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    UserAttachmentType,
    {
        {UserAttachmentType::File, "file"},
        {UserAttachmentType::Directory, "directory"},
        {UserAttachmentType::Selection, "selection"},
    }
)

/// System message role
enum class SystemMessageRole
{
    System,
    Developer
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    SystemMessageRole,
    {
        {SystemMessageRole::System, "system"},
        {SystemMessageRole::Developer, "developer"},
    }
)

/// Repository info for handoff
struct RepositoryInfo
{
    std::string owner;
    std::string name;
    std::optional<std::string> branch;
};

inline void to_json(json& j, const RepositoryInfo& r)
{
    j = json{{"owner", r.owner}, {"name", r.name}};
    if (r.branch)
        j["branch"] = *r.branch;
}

inline void from_json(const json& j, RepositoryInfo& r)
{
    j.at("owner").get_to(r.owner);
    j.at("name").get_to(r.name);
    if (j.contains("branch"))
        r.branch = j.at("branch").get<std::string>();
}

/// Attachment in user message
struct UserMessageAttachmentItem
{
    UserAttachmentType type;
    std::string path;
    std::string display_name;
};

inline void to_json(json& j, const UserMessageAttachmentItem& a)
{
    j = json{{"type", a.type}, {"path", a.path}, {"displayName", a.display_name}};
}

inline void from_json(const json& j, UserMessageAttachmentItem& a)
{
    j.at("type").get_to(a.type);
    j.at("path").get_to(a.path);
    j.at("displayName").get_to(a.display_name);
}

/// Tool request in assistant message
struct ToolRequestItem
{
    std::string tool_call_id;
    std::string name;
    std::optional<json> arguments;
};

inline void to_json(json& j, const ToolRequestItem& t)
{
    j = json{{"toolCallId", t.tool_call_id}, {"name", t.name}};
    if (t.arguments)
        j["arguments"] = *t.arguments;
}

inline void from_json(const json& j, ToolRequestItem& t)
{
    j.at("toolCallId").get_to(t.tool_call_id);
    j.at("name").get_to(t.name);
    if (j.contains("arguments"))
        t.arguments = j.at("arguments");
}

/// Tool execution result content
struct ToolResultContent
{
    std::string content;
    std::optional<std::string> detailed_content;
};

inline void to_json(json& j, const ToolResultContent& r)
{
    j = json{{"content", r.content}};
    if (r.detailed_content)
        j["detailedContent"] = *r.detailed_content;
}

inline void from_json(const json& j, ToolResultContent& r)
{
    j.at("content").get_to(r.content);
    if (j.contains("detailedContent") && !j["detailedContent"].is_null())
        r.detailed_content = j.at("detailedContent").get<std::string>();
}

/// Tool execution error
struct ToolExecutionError
{
    std::string message;
    std::optional<std::string> code;
};

inline void to_json(json& j, const ToolExecutionError& e)
{
    j = json{{"message", e.message}};
    if (e.code)
        j["code"] = *e.code;
}

inline void from_json(const json& j, ToolExecutionError& e)
{
    j.at("message").get_to(e.message);
    if (j.contains("code"))
        e.code = j.at("code").get<std::string>();
}

/// Hook error
struct HookError
{
    std::string message;
    std::optional<std::string> stack;
};

inline void to_json(json& j, const HookError& e)
{
    j = json{{"message", e.message}};
    if (e.stack)
        j["stack"] = *e.stack;
}

inline void from_json(const json& j, HookError& e)
{
    j.at("message").get_to(e.message);
    if (j.contains("stack"))
        e.stack = j.at("stack").get<std::string>();
}

/// System message metadata
struct SystemMessageMetadata
{
    std::optional<std::string> prompt_version;
    std::optional<std::map<std::string, json>> variables;
};

inline void to_json(json& j, const SystemMessageMetadata& m)
{
    j = json::object();
    if (m.prompt_version)
        j["promptVersion"] = *m.prompt_version;
    if (m.variables)
        j["variables"] = *m.variables;
}

inline void from_json(const json& j, SystemMessageMetadata& m)
{
    if (j.contains("promptVersion"))
        m.prompt_version = j.at("promptVersion").get<std::string>();
    if (j.contains("variables"))
        m.variables = j.at("variables").get<std::map<std::string, json>>();
}

// =============================================================================
// Event Data Definitions
// =============================================================================

struct SessionStartData
{
    std::string session_id;
    double version;
    std::string producer;
    std::string copilot_version;
    std::string start_time; // ISO 8601
    std::optional<std::string> selected_model;
};

inline void from_json(const json& j, SessionStartData& d)
{
    j.at("sessionId").get_to(d.session_id);
    j.at("version").get_to(d.version);
    j.at("producer").get_to(d.producer);
    j.at("copilotVersion").get_to(d.copilot_version);
    j.at("startTime").get_to(d.start_time);
    if (j.contains("selectedModel"))
        d.selected_model = j.at("selectedModel").get<std::string>();
}

struct SessionResumeData
{
    std::string resume_time; // ISO 8601
    double event_count;
};

inline void from_json(const json& j, SessionResumeData& d)
{
    j.at("resumeTime").get_to(d.resume_time);
    j.at("eventCount").get_to(d.event_count);
}

struct SessionErrorData
{
    std::string error_type;
    std::string message;
    std::optional<std::string> stack;
    std::optional<double> status_code;
    std::optional<std::string> provider_call_id;
};

inline void from_json(const json& j, SessionErrorData& d)
{
    j.at("errorType").get_to(d.error_type);
    j.at("message").get_to(d.message);
    if (j.contains("stack"))
        d.stack = j.at("stack").get<std::string>();
    if (j.contains("statusCode") && !j["statusCode"].is_null())
        d.status_code = j.at("statusCode").get<double>();
    if (j.contains("providerCallId") && !j["providerCallId"].is_null())
        d.provider_call_id = j.at("providerCallId").get<std::string>();
}

struct SessionIdleData
{
};

inline void from_json(const json&, SessionIdleData&) {}

struct SessionInfoData
{
    std::string info_type;
    std::string message;
};

inline void from_json(const json& j, SessionInfoData& d)
{
    j.at("infoType").get_to(d.info_type);
    j.at("message").get_to(d.message);
}

struct SessionModelChangeData
{
    std::optional<std::string> previous_model;
    std::string new_model;
};

inline void from_json(const json& j, SessionModelChangeData& d)
{
    if (j.contains("previousModel"))
        d.previous_model = j.at("previousModel").get<std::string>();
    j.at("newModel").get_to(d.new_model);
}

struct SessionHandoffData
{
    std::string handoff_time; // ISO 8601
    HandoffSourceType source_type;
    std::optional<RepositoryInfo> repository;
    std::optional<std::string> context;
    std::optional<std::string> summary;
    std::optional<std::string> remote_session_id;
};

inline void from_json(const json& j, SessionHandoffData& d)
{
    j.at("handoffTime").get_to(d.handoff_time);
    j.at("sourceType").get_to(d.source_type);
    if (j.contains("repository"))
        d.repository = j.at("repository").get<RepositoryInfo>();
    if (j.contains("context"))
        d.context = j.at("context").get<std::string>();
    if (j.contains("summary"))
        d.summary = j.at("summary").get<std::string>();
    if (j.contains("remoteSessionId"))
        d.remote_session_id = j.at("remoteSessionId").get<std::string>();
}

struct SessionTruncationData
{
    double token_limit;
    double pre_truncation_tokens_in_messages;
    double pre_truncation_messages_length;
    double post_truncation_tokens_in_messages;
    double post_truncation_messages_length;
    double tokens_removed_during_truncation;
    double messages_removed_during_truncation;
    std::string performed_by;
};

inline void from_json(const json& j, SessionTruncationData& d)
{
    j.at("tokenLimit").get_to(d.token_limit);
    j.at("preTruncationTokensInMessages").get_to(d.pre_truncation_tokens_in_messages);
    j.at("preTruncationMessagesLength").get_to(d.pre_truncation_messages_length);
    j.at("postTruncationTokensInMessages").get_to(d.post_truncation_tokens_in_messages);
    j.at("postTruncationMessagesLength").get_to(d.post_truncation_messages_length);
    j.at("tokensRemovedDuringTruncation").get_to(d.tokens_removed_during_truncation);
    j.at("messagesRemovedDuringTruncation").get_to(d.messages_removed_during_truncation);
    j.at("performedBy").get_to(d.performed_by);
}

struct UserMessageData
{
    std::string content;
    std::optional<std::string> transformed_content;
    std::optional<std::vector<UserMessageAttachmentItem>> attachments;
    std::optional<std::string> source;
};

inline void from_json(const json& j, UserMessageData& d)
{
    j.at("content").get_to(d.content);
    if (j.contains("transformedContent"))
        d.transformed_content = j.at("transformedContent").get<std::string>();
    if (j.contains("attachments"))
        d.attachments = j.at("attachments").get<std::vector<UserMessageAttachmentItem>>();
    if (j.contains("source"))
        d.source = j.at("source").get<std::string>();
}

struct PendingMessagesModifiedData
{
};

inline void from_json(const json&, PendingMessagesModifiedData&) {}

struct AssistantTurnStartData
{
    std::string turn_id;
};

inline void from_json(const json& j, AssistantTurnStartData& d)
{
    j.at("turnId").get_to(d.turn_id);
}

struct AssistantIntentData
{
    std::string intent;
};

inline void from_json(const json& j, AssistantIntentData& d)
{
    j.at("intent").get_to(d.intent);
}

struct AssistantReasoningData
{
    std::string reasoning_id;
    std::string content;
    std::optional<std::string> chunk_content;
};

inline void from_json(const json& j, AssistantReasoningData& d)
{
    j.at("reasoningId").get_to(d.reasoning_id);
    j.at("content").get_to(d.content);
    if (j.contains("chunkContent"))
        d.chunk_content = j.at("chunkContent").get<std::string>();
}

struct AssistantReasoningDeltaData
{
    std::string reasoning_id;
    std::string delta_content;
};

inline void from_json(const json& j, AssistantReasoningDeltaData& d)
{
    j.at("reasoningId").get_to(d.reasoning_id);
    j.at("deltaContent").get_to(d.delta_content);
}

struct AssistantMessageData
{
    std::string message_id;
    std::string content;
    std::optional<std::string> chunk_content;
    std::optional<double> total_response_size_bytes;
    std::optional<std::vector<ToolRequestItem>> tool_requests;
    std::optional<std::string> parent_tool_call_id;
    std::optional<std::string> reasoning_opaque;
    std::optional<std::string> reasoning_text;
    std::optional<std::string> encrypted_content;
};

inline void from_json(const json& j, AssistantMessageData& d)
{
    j.at("messageId").get_to(d.message_id);
    j.at("content").get_to(d.content);
    if (j.contains("chunkContent"))
        d.chunk_content = j.at("chunkContent").get<std::string>();
    if (j.contains("totalResponseSizeBytes"))
        d.total_response_size_bytes = j.at("totalResponseSizeBytes").get<double>();
    if (j.contains("toolRequests"))
        d.tool_requests = j.at("toolRequests").get<std::vector<ToolRequestItem>>();
    if (j.contains("parentToolCallId"))
        d.parent_tool_call_id = j.at("parentToolCallId").get<std::string>();
    if (j.contains("reasoningOpaque") && !j["reasoningOpaque"].is_null())
        d.reasoning_opaque = j.at("reasoningOpaque").get<std::string>();
    if (j.contains("reasoningText") && !j["reasoningText"].is_null())
        d.reasoning_text = j.at("reasoningText").get<std::string>();
    if (j.contains("encryptedContent") && !j["encryptedContent"].is_null())
        d.encrypted_content = j.at("encryptedContent").get<std::string>();
}

struct AssistantMessageDeltaData
{
    std::string message_id;
    std::string delta_content;
    std::optional<double> total_response_size_bytes;
    std::optional<std::string> parent_tool_call_id;
};

inline void from_json(const json& j, AssistantMessageDeltaData& d)
{
    j.at("messageId").get_to(d.message_id);
    j.at("deltaContent").get_to(d.delta_content);
    if (j.contains("totalResponseSizeBytes"))
        d.total_response_size_bytes = j.at("totalResponseSizeBytes").get<double>();
    if (j.contains("parentToolCallId"))
        d.parent_tool_call_id = j.at("parentToolCallId").get<std::string>();
}

struct AssistantTurnEndData
{
    std::string turn_id;
};

inline void from_json(const json& j, AssistantTurnEndData& d)
{
    j.at("turnId").get_to(d.turn_id);
}

struct AssistantUsageData
{
    std::optional<std::string> model;
    std::optional<double> input_tokens;
    std::optional<double> output_tokens;
    std::optional<double> cache_read_tokens;
    std::optional<double> cache_write_tokens;
    std::optional<double> cost;
    std::optional<double> duration;
    std::optional<std::string> initiator;
    std::optional<std::string> api_call_id;
    std::optional<std::string> provider_call_id;
    std::optional<std::map<std::string, json>> quota_snapshots;
    std::optional<std::string> parent_tool_call_id;
};

inline void from_json(const json& j, AssistantUsageData& d)
{
    if (j.contains("model"))
        d.model = j.at("model").get<std::string>();
    if (j.contains("inputTokens"))
        d.input_tokens = j.at("inputTokens").get<double>();
    if (j.contains("outputTokens"))
        d.output_tokens = j.at("outputTokens").get<double>();
    if (j.contains("cacheReadTokens"))
        d.cache_read_tokens = j.at("cacheReadTokens").get<double>();
    if (j.contains("cacheWriteTokens"))
        d.cache_write_tokens = j.at("cacheWriteTokens").get<double>();
    if (j.contains("cost"))
        d.cost = j.at("cost").get<double>();
    if (j.contains("duration"))
        d.duration = j.at("duration").get<double>();
    if (j.contains("initiator"))
        d.initiator = j.at("initiator").get<std::string>();
    if (j.contains("apiCallId"))
        d.api_call_id = j.at("apiCallId").get<std::string>();
    if (j.contains("providerCallId"))
        d.provider_call_id = j.at("providerCallId").get<std::string>();
    if (j.contains("quotaSnapshots"))
        d.quota_snapshots = j.at("quotaSnapshots").get<std::map<std::string, json>>();
    if (j.contains("parentToolCallId") && !j["parentToolCallId"].is_null())
        d.parent_tool_call_id = j.at("parentToolCallId").get<std::string>();
}

struct AbortData
{
    std::string reason;
};

inline void from_json(const json& j, AbortData& d)
{
    j.at("reason").get_to(d.reason);
}

struct ToolUserRequestedData
{
    std::string tool_call_id;
    std::string tool_name;
    std::optional<json> arguments;
};

inline void from_json(const json& j, ToolUserRequestedData& d)
{
    j.at("toolCallId").get_to(d.tool_call_id);
    j.at("toolName").get_to(d.tool_name);
    if (j.contains("arguments"))
        d.arguments = j.at("arguments");
}

struct ToolExecutionStartData
{
    std::string tool_call_id;
    std::string tool_name;
    std::optional<json> arguments;
    std::optional<std::string> parent_tool_call_id;
    std::optional<std::string> mcp_server_name;
    std::optional<std::string> mcp_tool_name;
};

inline void from_json(const json& j, ToolExecutionStartData& d)
{
    j.at("toolCallId").get_to(d.tool_call_id);
    j.at("toolName").get_to(d.tool_name);
    if (j.contains("arguments"))
        d.arguments = j.at("arguments");
    if (j.contains("parentToolCallId"))
        d.parent_tool_call_id = j.at("parentToolCallId").get<std::string>();
    if (j.contains("mcpServerName") && !j["mcpServerName"].is_null())
        d.mcp_server_name = j.at("mcpServerName").get<std::string>();
    if (j.contains("mcpToolName") && !j["mcpToolName"].is_null())
        d.mcp_tool_name = j.at("mcpToolName").get<std::string>();
}

struct ToolExecutionPartialResultData
{
    std::string tool_call_id;
    std::string partial_output;
};

inline void from_json(const json& j, ToolExecutionPartialResultData& d)
{
    j.at("toolCallId").get_to(d.tool_call_id);
    j.at("partialOutput").get_to(d.partial_output);
}

struct ToolExecutionCompleteData
{
    std::string tool_call_id;
    bool success;
    std::optional<bool> is_user_requested;
    std::optional<ToolResultContent> result;
    std::optional<ToolExecutionError> error;
    std::optional<std::map<std::string, json>> tool_telemetry;
    std::optional<std::string> parent_tool_call_id;
};

inline void from_json(const json& j, ToolExecutionCompleteData& d)
{
    j.at("toolCallId").get_to(d.tool_call_id);
    j.at("success").get_to(d.success);
    if (j.contains("isUserRequested"))
        d.is_user_requested = j.at("isUserRequested").get<bool>();
    if (j.contains("result"))
        d.result = j.at("result").get<ToolResultContent>();
    if (j.contains("error"))
        d.error = j.at("error").get<ToolExecutionError>();
    if (j.contains("toolTelemetry"))
        d.tool_telemetry = j.at("toolTelemetry").get<std::map<std::string, json>>();
    if (j.contains("parentToolCallId"))
        d.parent_tool_call_id = j.at("parentToolCallId").get<std::string>();
}

struct ToolExecutionProgressData
{
    std::string tool_call_id;
    std::string progress_message;
};

inline void from_json(const json& j, ToolExecutionProgressData& d)
{
    j.at("toolCallId").get_to(d.tool_call_id);
    j.at("progressMessage").get_to(d.progress_message);
}

struct SessionUsageInfoData
{
    double token_limit = 0;
    double current_tokens = 0;
    double messages_length = 0;
};

inline void from_json(const json& j, SessionUsageInfoData& d)
{
    j.at("tokenLimit").get_to(d.token_limit);
    j.at("currentTokens").get_to(d.current_tokens);
    j.at("messagesLength").get_to(d.messages_length);
}

struct SessionCompactionStartData
{
};

inline void from_json(const json&, SessionCompactionStartData&) {}

struct SessionCompactionCompleteDataTokensUsed
{
    double input = 0;
    double output = 0;
    double cached_input = 0;
};

inline void from_json(const json& j, SessionCompactionCompleteDataTokensUsed& d)
{
    j.at("input").get_to(d.input);
    j.at("output").get_to(d.output);
    j.at("cachedInput").get_to(d.cached_input);
}

struct SessionCompactionCompleteData
{
    bool success = false;
    std::optional<std::string> error;
    std::optional<double> pre_compaction_tokens;
    std::optional<double> post_compaction_tokens;
    std::optional<double> pre_compaction_messages_length;
    std::optional<double> post_compaction_messages_length;
    std::optional<SessionCompactionCompleteDataTokensUsed> compaction_tokens_used;
    std::optional<double> messages_removed;
    std::optional<double> tokens_removed;
    std::optional<std::string> summary_content;
    std::optional<double> checkpoint_number;
    std::optional<std::string> checkpoint_path;
};

inline void from_json(const json& j, SessionCompactionCompleteData& d)
{
    j.at("success").get_to(d.success);
    if (j.contains("error"))
        d.error = j.at("error").get<std::string>();
    if (j.contains("preCompactionTokens"))
        d.pre_compaction_tokens = j.at("preCompactionTokens").get<double>();
    if (j.contains("postCompactionTokens"))
        d.post_compaction_tokens = j.at("postCompactionTokens").get<double>();
    if (j.contains("preCompactionMessagesLength"))
        d.pre_compaction_messages_length = j.at("preCompactionMessagesLength").get<double>();
    if (j.contains("postCompactionMessagesLength"))
        d.post_compaction_messages_length = j.at("postCompactionMessagesLength").get<double>();
    if (j.contains("compactionTokensUsed"))
        d.compaction_tokens_used = j.at("compactionTokensUsed").get<SessionCompactionCompleteDataTokensUsed>();
    if (j.contains("messagesRemoved"))
        d.messages_removed = j.at("messagesRemoved").get<double>();
    if (j.contains("tokensRemoved"))
        d.tokens_removed = j.at("tokensRemoved").get<double>();
    if (j.contains("summaryContent") && !j["summaryContent"].is_null())
        d.summary_content = j.at("summaryContent").get<std::string>();
    if (j.contains("checkpointNumber") && !j["checkpointNumber"].is_null())
        d.checkpoint_number = j.at("checkpointNumber").get<double>();
    if (j.contains("checkpointPath") && !j["checkpointPath"].is_null())
        d.checkpoint_path = j.at("checkpointPath").get<std::string>();
}

struct CustomAgentStartedData
{
    std::string tool_call_id;
    std::string agent_name;
    std::string agent_display_name;
    std::string agent_description;
};

inline void from_json(const json& j, CustomAgentStartedData& d)
{
    j.at("toolCallId").get_to(d.tool_call_id);
    j.at("agentName").get_to(d.agent_name);
    j.at("agentDisplayName").get_to(d.agent_display_name);
    j.at("agentDescription").get_to(d.agent_description);
}

struct CustomAgentCompletedData
{
    std::string tool_call_id;
    std::string agent_name;
};

inline void from_json(const json& j, CustomAgentCompletedData& d)
{
    j.at("toolCallId").get_to(d.tool_call_id);
    j.at("agentName").get_to(d.agent_name);
}

struct CustomAgentFailedData
{
    std::string tool_call_id;
    std::string agent_name;
    std::string error;
};

inline void from_json(const json& j, CustomAgentFailedData& d)
{
    j.at("toolCallId").get_to(d.tool_call_id);
    j.at("agentName").get_to(d.agent_name);
    j.at("error").get_to(d.error);
}

struct CustomAgentSelectedData
{
    std::string agent_name;
    std::string agent_display_name;
    std::vector<std::string> tools;
};

inline void from_json(const json& j, CustomAgentSelectedData& d)
{
    j.at("agentName").get_to(d.agent_name);
    j.at("agentDisplayName").get_to(d.agent_display_name);
    j.at("tools").get_to(d.tools);
}

struct HookStartData
{
    std::string hook_invocation_id;
    std::string hook_type;
    std::optional<json> input;
};

inline void from_json(const json& j, HookStartData& d)
{
    j.at("hookInvocationId").get_to(d.hook_invocation_id);
    j.at("hookType").get_to(d.hook_type);
    if (j.contains("input"))
        d.input = j.at("input");
}

struct HookEndData
{
    std::string hook_invocation_id;
    std::string hook_type;
    std::optional<json> output;
    bool success;
    std::optional<HookError> error;
};

inline void from_json(const json& j, HookEndData& d)
{
    j.at("hookInvocationId").get_to(d.hook_invocation_id);
    j.at("hookType").get_to(d.hook_type);
    if (j.contains("output"))
        d.output = j.at("output");
    j.at("success").get_to(d.success);
    if (j.contains("error"))
        d.error = j.at("error").get<HookError>();
}

struct SystemMessageData
{
    std::string content;
    SystemMessageRole role;
    std::optional<std::string> name;
    std::optional<SystemMessageMetadata> metadata;
};

inline void from_json(const json& j, SystemMessageData& d)
{
    j.at("content").get_to(d.content);
    j.at("role").get_to(d.role);
    if (j.contains("name"))
        d.name = j.at("name").get<std::string>();
    if (j.contains("metadata"))
        d.metadata = j.at("metadata").get<SystemMessageMetadata>();
}

// =============================================================================
// New Event Data Types (v0.1.23)
// =============================================================================

/// Shutdown type enum
enum class ShutdownType
{
    Routine,
    Error
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ShutdownType,
    {
        {ShutdownType::Routine, "routine"},
        {ShutdownType::Error, "error"},
    }
)

/// Code changes summary in shutdown data
struct ShutdownCodeChanges
{
    double lines_added = 0;
    double lines_removed = 0;
    std::vector<std::string> files_modified;
};

inline void from_json(const json& j, ShutdownCodeChanges& d)
{
    j.at("linesAdded").get_to(d.lines_added);
    j.at("linesRemoved").get_to(d.lines_removed);
    if (j.contains("filesModified"))
        j.at("filesModified").get_to(d.files_modified);
}

/// Data for session.snapshot_rewind event
struct SessionSnapshotRewindData
{
    std::string up_to_event_id;
    double events_removed = 0;
};

inline void from_json(const json& j, SessionSnapshotRewindData& d)
{
    j.at("upToEventId").get_to(d.up_to_event_id);
    j.at("eventsRemoved").get_to(d.events_removed);
}

/// Data for session.shutdown event
struct SessionShutdownData
{
    ShutdownType shutdown_type = ShutdownType::Routine;
    std::optional<std::string> error_reason;
    double total_premium_requests = 0;
    double total_api_duration_ms = 0;
    double session_start_time = 0;
    ShutdownCodeChanges code_changes;
    std::map<std::string, json> model_metrics;
    std::optional<std::string> current_model;
};

inline void from_json(const json& j, SessionShutdownData& d)
{
    j.at("shutdownType").get_to(d.shutdown_type);
    if (j.contains("errorReason") && !j["errorReason"].is_null())
        d.error_reason = j.at("errorReason").get<std::string>();
    j.at("totalPremiumRequests").get_to(d.total_premium_requests);
    j.at("totalApiDurationMs").get_to(d.total_api_duration_ms);
    j.at("sessionStartTime").get_to(d.session_start_time);
    j.at("codeChanges").get_to(d.code_changes);
    if (j.contains("modelMetrics"))
        d.model_metrics = j.at("modelMetrics").get<std::map<std::string, json>>();
    if (j.contains("currentModel") && !j["currentModel"].is_null())
        d.current_model = j.at("currentModel").get<std::string>();
}

/// Data for skill.invoked event
struct SkillInvokedData
{
    std::string name;
    std::string path;
    std::string content;
    std::optional<std::vector<std::string>> allowed_tools;
    std::optional<std::string> description;
    std::optional<std::string> plugin_name;
    std::optional<std::string> plugin_version;
};

inline void from_json(const json& j, SkillInvokedData& d)
{
    j.at("name").get_to(d.name);
    j.at("path").get_to(d.path);
    j.at("content").get_to(d.content);
    if (j.contains("allowedTools") && !j["allowedTools"].is_null())
        d.allowed_tools = j.at("allowedTools").get<std::vector<std::string>>();
    if (j.contains("description") && !j["description"].is_null())
        d.description = j.at("description").get<std::string>();
    if (j.contains("pluginName") && !j["pluginName"].is_null())
        d.plugin_name = j.at("pluginName").get<std::string>();
    if (j.contains("pluginVersion") && !j["pluginVersion"].is_null())
        d.plugin_version = j.at("pluginVersion").get<std::string>();
}

// =============================================================================
// New Event Data Types (v0.1.49)
// =============================================================================

/// Working directory and git context (used by session.start and session.context_changed)
struct WorkingDirectoryContext
{
    std::string cwd;
    std::optional<std::string> base_commit;
    std::optional<std::string> branch;
    std::optional<std::string> git_root;
    std::optional<std::string> head_commit;
    std::optional<std::string> host_type;
    std::optional<std::string> repository;
    std::optional<std::string> repository_host;
};

inline void from_json(const json& j, WorkingDirectoryContext& d)
{
    j.at("cwd").get_to(d.cwd);
    if (j.contains("baseCommit") && !j["baseCommit"].is_null())
        d.base_commit = j.at("baseCommit").get<std::string>();
    if (j.contains("branch") && !j["branch"].is_null())
        d.branch = j.at("branch").get<std::string>();
    if (j.contains("gitRoot") && !j["gitRoot"].is_null())
        d.git_root = j.at("gitRoot").get<std::string>();
    if (j.contains("headCommit") && !j["headCommit"].is_null())
        d.head_commit = j.at("headCommit").get<std::string>();
    if (j.contains("hostType") && !j["hostType"].is_null())
        d.host_type = j.at("hostType").get<std::string>();
    if (j.contains("repository") && !j["repository"].is_null())
        d.repository = j.at("repository").get<std::string>();
    if (j.contains("repositoryHost") && !j["repositoryHost"].is_null())
        d.repository_host = j.at("repositoryHost").get<std::string>();
}

/// Data for session.remote_steerable_changed event
struct SessionRemoteSteerableChangedData
{
    bool remote_steerable = false;
};

inline void from_json(const json& j, SessionRemoteSteerableChangedData& d)
{
    j.at("remoteSteerable").get_to(d.remote_steerable);
}

/// Data for session.title_changed event
struct SessionTitleChangedData
{
    std::string title;
};

inline void from_json(const json& j, SessionTitleChangedData& d)
{
    j.at("title").get_to(d.title);
}

/// Data for session.schedule_created event
struct SessionScheduleCreatedData
{
    double id = 0;
    double interval_ms = 0;
    std::string prompt;
    std::optional<std::string> display_prompt;
    std::optional<bool> recurring;
};

inline void from_json(const json& j, SessionScheduleCreatedData& d)
{
    j.at("id").get_to(d.id);
    j.at("intervalMs").get_to(d.interval_ms);
    j.at("prompt").get_to(d.prompt);
    if (j.contains("displayPrompt") && !j["displayPrompt"].is_null())
        d.display_prompt = j.at("displayPrompt").get<std::string>();
    if (j.contains("recurring") && !j["recurring"].is_null())
        d.recurring = j.at("recurring").get<bool>();
}

/// Data for session.schedule_cancelled event
struct SessionScheduleCancelledData
{
    double id = 0;
};

inline void from_json(const json& j, SessionScheduleCancelledData& d)
{
    j.at("id").get_to(d.id);
}

/// Data for session.warning event
struct SessionWarningData
{
    std::string warning_type;
    std::string message;
    std::optional<std::string> url;
};

inline void from_json(const json& j, SessionWarningData& d)
{
    j.at("warningType").get_to(d.warning_type);
    j.at("message").get_to(d.message);
    if (j.contains("url") && !j["url"].is_null())
        d.url = j.at("url").get<std::string>();
}

/// Data for session.mode_changed event
struct SessionModeChangedData
{
    std::string previous_mode;
    std::string new_mode;
};

inline void from_json(const json& j, SessionModeChangedData& d)
{
    j.at("previousMode").get_to(d.previous_mode);
    j.at("newMode").get_to(d.new_mode);
}

/// Data for session.plan_changed event
struct SessionPlanChangedData
{
    std::string operation; // "create" | "update" | "delete"
};

inline void from_json(const json& j, SessionPlanChangedData& d)
{
    j.at("operation").get_to(d.operation);
}

/// Data for session.workspace_file_changed event
struct SessionWorkspaceFileChangedData
{
    std::string operation; // "create" | "update"
    std::string path;
};

inline void from_json(const json& j, SessionWorkspaceFileChangedData& d)
{
    j.at("operation").get_to(d.operation);
    j.at("path").get_to(d.path);
}

/// Data for session.context_changed event (same shape as WorkingDirectoryContext)
struct SessionContextChangedData
{
    WorkingDirectoryContext context;
};

inline void from_json(const json& j, SessionContextChangedData& d)
{
    d.context = j.get<WorkingDirectoryContext>();
}

/// Data for session.task_complete event
struct SessionTaskCompleteData
{
    std::optional<bool> success;
    std::optional<std::string> summary;
};

inline void from_json(const json& j, SessionTaskCompleteData& d)
{
    if (j.contains("success") && !j["success"].is_null())
        d.success = j.at("success").get<bool>();
    if (j.contains("summary") && !j["summary"].is_null())
        d.summary = j.at("summary").get<std::string>();
}

/// Data for session.custom_notification event
struct SessionCustomNotificationData
{
    std::string source;
    std::string name;
    json payload;
    std::optional<std::map<std::string, std::string>> subject;
    std::optional<double> version;
};

inline void from_json(const json& j, SessionCustomNotificationData& d)
{
    j.at("source").get_to(d.source);
    j.at("name").get_to(d.name);
    d.payload = j.at("payload");
    if (j.contains("subject") && !j["subject"].is_null())
        d.subject = j.at("subject").get<std::map<std::string, std::string>>();
    if (j.contains("version") && !j["version"].is_null())
        d.version = j.at("version").get<double>();
}

/// Data for session.tools_updated event
struct SessionToolsUpdatedData
{
    std::string model;
};

inline void from_json(const json& j, SessionToolsUpdatedData& d)
{
    j.at("model").get_to(d.model);
}

/// Data for session.background_tasks_changed event (empty payload)
struct SessionBackgroundTasksChangedData
{
};

inline void from_json(const json&, SessionBackgroundTasksChangedData&) {}

/// Skill metadata in session.skills_loaded
struct SkillsLoadedSkill
{
    std::string name;
    std::string description;
    bool enabled = false;
    std::string source;
    bool user_invocable = false;
    std::optional<std::string> path;
};

inline void from_json(const json& j, SkillsLoadedSkill& d)
{
    j.at("name").get_to(d.name);
    j.at("description").get_to(d.description);
    j.at("enabled").get_to(d.enabled);
    j.at("source").get_to(d.source);
    j.at("userInvocable").get_to(d.user_invocable);
    if (j.contains("path") && !j["path"].is_null())
        d.path = j.at("path").get<std::string>();
}

/// Data for session.skills_loaded event
struct SessionSkillsLoadedData
{
    std::vector<SkillsLoadedSkill> skills;
};

inline void from_json(const json& j, SessionSkillsLoadedData& d)
{
    j.at("skills").get_to(d.skills);
}

/// Custom agent metadata in session.custom_agents_updated
struct CustomAgentsUpdatedAgent
{
    std::string id;
    std::string name;
    std::string display_name;
    std::string description;
    std::string source;
    bool user_invocable = false;
    std::optional<std::vector<std::string>> tools; // null = all tools
    std::optional<std::string> model;
};

inline void from_json(const json& j, CustomAgentsUpdatedAgent& d)
{
    j.at("id").get_to(d.id);
    j.at("name").get_to(d.name);
    j.at("displayName").get_to(d.display_name);
    j.at("description").get_to(d.description);
    j.at("source").get_to(d.source);
    j.at("userInvocable").get_to(d.user_invocable);
    if (j.contains("tools") && !j["tools"].is_null())
        d.tools = j.at("tools").get<std::vector<std::string>>();
    if (j.contains("model") && !j["model"].is_null())
        d.model = j.at("model").get<std::string>();
}

/// Data for session.custom_agents_updated event
struct SessionCustomAgentsUpdatedData
{
    std::vector<CustomAgentsUpdatedAgent> agents;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

inline void from_json(const json& j, SessionCustomAgentsUpdatedData& d)
{
    j.at("agents").get_to(d.agents);
    if (j.contains("errors"))
        j.at("errors").get_to(d.errors);
    if (j.contains("warnings"))
        j.at("warnings").get_to(d.warnings);
}

/// MCP server entry in session.mcp_servers_loaded
struct McpServersLoadedServer
{
    std::string name;
    std::string status;
    std::optional<std::string> error;
    std::optional<std::string> source;
};

inline void from_json(const json& j, McpServersLoadedServer& d)
{
    j.at("name").get_to(d.name);
    j.at("status").get_to(d.status);
    if (j.contains("error") && !j["error"].is_null())
        d.error = j.at("error").get<std::string>();
    if (j.contains("source") && !j["source"].is_null())
        d.source = j.at("source").get<std::string>();
}

/// Data for session.mcp_servers_loaded event
struct SessionMcpServersLoadedData
{
    std::vector<McpServersLoadedServer> servers;
};

inline void from_json(const json& j, SessionMcpServersLoadedData& d)
{
    j.at("servers").get_to(d.servers);
}

/// Data for session.mcp_server_status_changed event
struct SessionMcpServerStatusChangedData
{
    std::string server_name;
    std::string status;
};

inline void from_json(const json& j, SessionMcpServerStatusChangedData& d)
{
    j.at("serverName").get_to(d.server_name);
    j.at("status").get_to(d.status);
}

/// Extension entry in session.extensions_loaded
struct ExtensionsLoadedExtension
{
    std::string id;
    std::string name;
    std::string source; // "project" | "user"
    std::string status; // "running" | "disabled" | "failed" | "starting"
};

inline void from_json(const json& j, ExtensionsLoadedExtension& d)
{
    j.at("id").get_to(d.id);
    j.at("name").get_to(d.name);
    j.at("source").get_to(d.source);
    j.at("status").get_to(d.status);
}

/// Data for session.extensions_loaded event
struct SessionExtensionsLoadedData
{
    std::vector<ExtensionsLoadedExtension> extensions;
};

inline void from_json(const json& j, SessionExtensionsLoadedData& d)
{
    j.at("extensions").get_to(d.extensions);
}

/// Data for assistant.streaming_delta event
struct AssistantStreamingDeltaData
{
    double total_response_size_bytes = 0;
};

inline void from_json(const json& j, AssistantStreamingDeltaData& d)
{
    j.at("totalResponseSizeBytes").get_to(d.total_response_size_bytes);
}

/// Data for assistant.message_start event
struct AssistantMessageStartData
{
    std::string message_id;
    std::optional<std::string> phase;
};

inline void from_json(const json& j, AssistantMessageStartData& d)
{
    j.at("messageId").get_to(d.message_id);
    if (j.contains("phase") && !j["phase"].is_null())
        d.phase = j.at("phase").get<std::string>();
}

/// Data for model.call_failure event
struct ModelCallFailureData
{
    std::string source; // "top_level" | "subagent" | "mcp_sampling"
    std::optional<std::string> api_call_id;
    std::optional<double> duration_ms;
    std::optional<std::string> error_message;
    std::optional<std::string> initiator;
    std::optional<std::string> model;
    std::optional<std::string> provider_call_id;
    std::optional<double> status_code;
};

inline void from_json(const json& j, ModelCallFailureData& d)
{
    j.at("source").get_to(d.source);
    if (j.contains("apiCallId") && !j["apiCallId"].is_null())
        d.api_call_id = j.at("apiCallId").get<std::string>();
    if (j.contains("durationMs") && !j["durationMs"].is_null())
        d.duration_ms = j.at("durationMs").get<double>();
    if (j.contains("errorMessage") && !j["errorMessage"].is_null())
        d.error_message = j.at("errorMessage").get<std::string>();
    if (j.contains("initiator") && !j["initiator"].is_null())
        d.initiator = j.at("initiator").get<std::string>();
    if (j.contains("model") && !j["model"].is_null())
        d.model = j.at("model").get<std::string>();
    if (j.contains("providerCallId") && !j["providerCallId"].is_null())
        d.provider_call_id = j.at("providerCallId").get<std::string>();
    if (j.contains("statusCode") && !j["statusCode"].is_null())
        d.status_code = j.at("statusCode").get<double>();
}

/// Data for subagent.deselected event (empty payload)
struct SubagentDeselectedData
{
};

inline void from_json(const json&, SubagentDeselectedData&) {}

/// Data for permission.requested event.
/// `permission_request` / `prompt_request` are kept as raw JSON because upstream
/// models them as large discriminated unions (shell/write/read/mcp/url/memory/...).
/// Callers needing the variant data can inspect the JSON via these fields.
struct PermissionRequestedData
{
    std::string request_id;
    json permission_request;
    std::optional<json> prompt_request;
    std::optional<bool> resolved_by_hook;
};

inline void from_json(const json& j, PermissionRequestedData& d)
{
    j.at("requestId").get_to(d.request_id);
    d.permission_request = j.at("permissionRequest");
    if (j.contains("promptRequest"))
        d.prompt_request = j.at("promptRequest");
    if (j.contains("resolvedByHook") && !j["resolvedByHook"].is_null())
        d.resolved_by_hook = j.at("resolvedByHook").get<bool>();
}

/// Data for permission.completed event.
/// `result` is kept as raw JSON (PermissionResult is a large union upstream).
struct PermissionCompletedData
{
    std::string request_id;
    json result;
    std::optional<std::string> tool_call_id;
};

inline void from_json(const json& j, PermissionCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
    d.result = j.at("result");
    if (j.contains("toolCallId") && !j["toolCallId"].is_null())
        d.tool_call_id = j.at("toolCallId").get<std::string>();
}

/// Data for user_input.requested event
struct UserInputRequestedData
{
    std::string request_id;
    std::string question;
    std::optional<bool> allow_freeform;
    std::optional<std::vector<std::string>> choices;
    std::optional<std::string> tool_call_id;
};

inline void from_json(const json& j, UserInputRequestedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("question").get_to(d.question);
    if (j.contains("allowFreeform") && !j["allowFreeform"].is_null())
        d.allow_freeform = j.at("allowFreeform").get<bool>();
    if (j.contains("choices") && !j["choices"].is_null())
        d.choices = j.at("choices").get<std::vector<std::string>>();
    if (j.contains("toolCallId") && !j["toolCallId"].is_null())
        d.tool_call_id = j.at("toolCallId").get<std::string>();
}

/// Data for user_input.completed event
struct UserInputCompletedData
{
    std::string request_id;
    std::optional<std::string> answer;
    std::optional<bool> was_freeform;
};

inline void from_json(const json& j, UserInputCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
    if (j.contains("answer") && !j["answer"].is_null())
        d.answer = j.at("answer").get<std::string>();
    if (j.contains("wasFreeform") && !j["wasFreeform"].is_null())
        d.was_freeform = j.at("wasFreeform").get<bool>();
}

/// Data for elicitation.requested event.
/// `requested_schema` kept as raw JSON (callers can inspect it on demand).
struct ElicitationRequestedData
{
    std::string request_id;
    std::string message;
    std::optional<std::string> mode; // "form" | "url"
    std::optional<std::string> elicitation_source;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> url;
    std::optional<json> requested_schema;
};

inline void from_json(const json& j, ElicitationRequestedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("message").get_to(d.message);
    if (j.contains("mode") && !j["mode"].is_null())
        d.mode = j.at("mode").get<std::string>();
    if (j.contains("elicitationSource") && !j["elicitationSource"].is_null())
        d.elicitation_source = j.at("elicitationSource").get<std::string>();
    if (j.contains("toolCallId") && !j["toolCallId"].is_null())
        d.tool_call_id = j.at("toolCallId").get<std::string>();
    if (j.contains("url") && !j["url"].is_null())
        d.url = j.at("url").get<std::string>();
    if (j.contains("requestedSchema"))
        d.requested_schema = j.at("requestedSchema");
}

/// Data for elicitation.completed event.
/// `content` kept as raw JSON because per-field values can be string|number|bool|string[].
struct ElicitationCompletedData
{
    std::string request_id;
    std::optional<std::string> action; // "accept" | "decline" | "cancel"
    std::optional<json> content;
};

inline void from_json(const json& j, ElicitationCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
    if (j.contains("action") && !j["action"].is_null())
        d.action = j.at("action").get<std::string>();
    if (j.contains("content"))
        d.content = j.at("content");
}

/// Data for sampling.requested event
struct SamplingRequestedData
{
    std::string request_id;
    std::string server_name;
    json mcp_request_id; // string | number
};

inline void from_json(const json& j, SamplingRequestedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("serverName").get_to(d.server_name);
    d.mcp_request_id = j.at("mcpRequestId");
}

/// Data for sampling.completed event
struct SamplingCompletedData
{
    std::string request_id;
};

inline void from_json(const json& j, SamplingCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
}

/// Static OAuth client config (mcp.oauth_required nested)
struct McpOauthRequiredStaticClientConfig
{
    std::string client_id;
    std::optional<std::string> client_secret;
    std::optional<std::string> grant_type;
    std::optional<bool> public_client;
};

inline void from_json(const json& j, McpOauthRequiredStaticClientConfig& d)
{
    j.at("clientId").get_to(d.client_id);
    if (j.contains("clientSecret") && !j["clientSecret"].is_null())
        d.client_secret = j.at("clientSecret").get<std::string>();
    if (j.contains("grantType") && !j["grantType"].is_null())
        d.grant_type = j.at("grantType").get<std::string>();
    if (j.contains("publicClient") && !j["publicClient"].is_null())
        d.public_client = j.at("publicClient").get<bool>();
}

/// Data for mcp.oauth_required event
struct McpOauthRequiredData
{
    std::string request_id;
    std::string server_name;
    std::string server_url;
    std::optional<std::string> reason;
    std::optional<json> www_authenticate_params;
    std::optional<std::string> resource_metadata;
    std::optional<McpOauthRequiredStaticClientConfig> static_client_config;
};

inline void from_json(const json& j, McpOauthRequiredData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("serverName").get_to(d.server_name);
    j.at("serverUrl").get_to(d.server_url);
    if (j.contains("reason") && !j["reason"].is_null())
        d.reason = j.at("reason").get<std::string>();
    if (j.contains("wwwAuthenticateParams") && !j["wwwAuthenticateParams"].is_null())
        d.www_authenticate_params = j.at("wwwAuthenticateParams");
    if (j.contains("resourceMetadata") && !j["resourceMetadata"].is_null())
        d.resource_metadata = j.at("resourceMetadata").get<std::string>();
    if (j.contains("staticClientConfig") && !j["staticClientConfig"].is_null())
        d.static_client_config = j.at("staticClientConfig").get<McpOauthRequiredStaticClientConfig>();
}

/// Data for mcp.oauth_completed event
struct McpOauthCompletedData
{
    std::string request_id;
};

inline void from_json(const json& j, McpOauthCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
}

/// Data for external_tool.requested event
struct ExternalToolRequestedData
{
    std::string request_id;
    std::string session_id;
    std::string tool_call_id;
    std::string tool_name;
    std::optional<json> arguments;
    std::optional<std::string> traceparent;
    std::optional<std::string> tracestate;
};

inline void from_json(const json& j, ExternalToolRequestedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("sessionId").get_to(d.session_id);
    j.at("toolCallId").get_to(d.tool_call_id);
    j.at("toolName").get_to(d.tool_name);
    if (j.contains("arguments"))
        d.arguments = j.at("arguments");
    if (j.contains("traceparent") && !j["traceparent"].is_null())
        d.traceparent = j.at("traceparent").get<std::string>();
    if (j.contains("tracestate") && !j["tracestate"].is_null())
        d.tracestate = j.at("tracestate").get<std::string>();
}

/// Data for external_tool.completed event
struct ExternalToolCompletedData
{
    std::string request_id;
};

inline void from_json(const json& j, ExternalToolCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
}

/// Data for command.queued event
struct CommandQueuedData
{
    std::string request_id;
    std::string command;
};

inline void from_json(const json& j, CommandQueuedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("command").get_to(d.command);
}

/// Data for command.execute event
struct CommandExecuteData
{
    std::string request_id;
    std::string command;
    std::string command_name;
    std::string args;
};

inline void from_json(const json& j, CommandExecuteData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("command").get_to(d.command);
    j.at("commandName").get_to(d.command_name);
    j.at("args").get_to(d.args);
}

/// Data for command.completed event
struct CommandCompletedData
{
    std::string request_id;
};

inline void from_json(const json& j, CommandCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
}

/// Data for auto_mode_switch.requested event
struct AutoModeSwitchRequestedData
{
    std::string request_id;
    std::optional<std::string> error_code;
    std::optional<double> retry_after_seconds;
};

inline void from_json(const json& j, AutoModeSwitchRequestedData& d)
{
    j.at("requestId").get_to(d.request_id);
    if (j.contains("errorCode") && !j["errorCode"].is_null())
        d.error_code = j.at("errorCode").get<std::string>();
    if (j.contains("retryAfterSeconds") && !j["retryAfterSeconds"].is_null())
        d.retry_after_seconds = j.at("retryAfterSeconds").get<double>();
}

/// Data for auto_mode_switch.completed event
struct AutoModeSwitchCompletedData
{
    std::string request_id;
    std::string response; // "yes" | "yes_always" | "no"
};

inline void from_json(const json& j, AutoModeSwitchCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("response").get_to(d.response);
}

/// SDK command entry in commands.changed
struct CommandsChangedCommand
{
    std::string name;
    std::optional<std::string> description;
};

inline void from_json(const json& j, CommandsChangedCommand& d)
{
    j.at("name").get_to(d.name);
    if (j.contains("description") && !j["description"].is_null())
        d.description = j.at("description").get<std::string>();
}

/// Data for commands.changed event
struct CommandsChangedData
{
    std::vector<CommandsChangedCommand> commands;
};

inline void from_json(const json& j, CommandsChangedData& d)
{
    j.at("commands").get_to(d.commands);
}

/// UI capability changes (capabilities.changed nested)
struct CapabilitiesChangedUI
{
    std::optional<bool> elicitation;
};

inline void from_json(const json& j, CapabilitiesChangedUI& d)
{
    if (j.contains("elicitation") && !j["elicitation"].is_null())
        d.elicitation = j.at("elicitation").get<bool>();
}

/// Data for capabilities.changed event
struct CapabilitiesChangedData
{
    std::optional<CapabilitiesChangedUI> ui;
};

inline void from_json(const json& j, CapabilitiesChangedData& d)
{
    if (j.contains("ui") && !j["ui"].is_null())
        d.ui = j.at("ui").get<CapabilitiesChangedUI>();
}

/// Data for exit_plan_mode.requested event
struct ExitPlanModeRequestedData
{
    std::string request_id;
    std::string plan_content;
    std::string summary;
    std::string recommended_action;
    std::vector<std::string> actions;
};

inline void from_json(const json& j, ExitPlanModeRequestedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("planContent").get_to(d.plan_content);
    j.at("summary").get_to(d.summary);
    j.at("recommendedAction").get_to(d.recommended_action);
    j.at("actions").get_to(d.actions);
}

/// Data for exit_plan_mode.completed event
struct ExitPlanModeCompletedData
{
    std::string request_id;
    std::optional<bool> approved;
    std::optional<bool> auto_approve_edits;
    std::optional<std::string> feedback;
    std::optional<std::string> selected_action;
};

inline void from_json(const json& j, ExitPlanModeCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
    if (j.contains("approved") && !j["approved"].is_null())
        d.approved = j.at("approved").get<bool>();
    if (j.contains("autoApproveEdits") && !j["autoApproveEdits"].is_null())
        d.auto_approve_edits = j.at("autoApproveEdits").get<bool>();
    if (j.contains("feedback") && !j["feedback"].is_null())
        d.feedback = j.at("feedback").get<std::string>();
    if (j.contains("selectedAction") && !j["selectedAction"].is_null())
        d.selected_action = j.at("selectedAction").get<std::string>();
}

/// Data for system.notification event.
/// `kind` is one of several variants discriminated by an inner `type` field
/// (agent_completed/agent_idle/new_inbox_message/shell_completed/...).
/// We keep it as raw JSON so consumers can branch on `kind.type` themselves.
struct SystemNotificationData
{
    std::string content;
    json kind;
};

inline void from_json(const json& j, SystemNotificationData& d)
{
    j.at("content").get_to(d.content);
    d.kind = j.at("kind");
}

// =============================================================================
// Session Event Type (Discriminated Union)
// =============================================================================

// --- Protocol surface additions: wire types present in the generated catalog ---
struct ScheduleRearmedData
{
    int64_t id;
    int64_t next_run_at;
};

inline void from_json(const json& j, ScheduleRearmedData& d)
{
    j.at("id").get_to(d.id);
    j.at("nextRunAt").get_to(d.next_run_at);
}

struct AutopilotObjectiveChangedData
{
    json operation;
    std::optional<int64_t> id;
    std::optional<json> status;
};

inline void from_json(const json& j, AutopilotObjectiveChangedData& d)
{
    j.at("operation").get_to(d.operation);
    if (j.contains("id") && !j["id"].is_null())
        d.id = j.at("id").get<int64_t>();
    if (j.contains("status") && !j["status"].is_null())
        d.status = j.at("status").get<json>();
}

struct SessionLimitsChangedData
{
    json session_limits;
};

inline void from_json(const json& j, SessionLimitsChangedData& d)
{
    j.at("sessionLimits").get_to(d.session_limits);
}

struct PermissionsChangedData
{
    json previous_mode;
    json mode;
    std::optional<std::string> assisted_approval_model;
};

inline void from_json(const json& j, PermissionsChangedData& d)
{
    j.at("previousMode").get_to(d.previous_mode);
    j.at("mode").get_to(d.mode);
    if (j.contains("assistedApprovalModel") && !j["assistedApprovalModel"].is_null())
        d.assisted_approval_model = j.at("assistedApprovalModel").get<std::string>();
}

struct TodosChangedData
{
    // No payload fields in the official schema.
};

inline void from_json(const json& j, TodosChangedData& d)
{
    (void)j;
    (void)d;
}

struct MemoryChangedData
{
    // No payload fields in the official schema.
};

inline void from_json(const json& j, MemoryChangedData& d)
{
    (void)j;
    (void)d;
}

struct UsageCheckpointData
{
    double total_nano_aiu;
    std::optional<double> total_premium_requests;
    std::optional<std::vector<json>> model_cache_state;
    std::optional<std::vector<json>> prompt_cache_break_state;
};

inline void from_json(const json& j, UsageCheckpointData& d)
{
    j.at("totalNanoAiu").get_to(d.total_nano_aiu);
    if (j.contains("totalPremiumRequests") && !j["totalPremiumRequests"].is_null())
        d.total_premium_requests = j.at("totalPremiumRequests").get<double>();
    if (j.contains("modelCacheState") && !j["modelCacheState"].is_null())
        d.model_cache_state = j.at("modelCacheState").get<std::vector<json>>();
    if (j.contains("promptCacheBreakState") && !j["promptCacheBreakState"].is_null())
        d.prompt_cache_break_state = j.at("promptCacheBreakState").get<std::vector<json>>();
}

struct ContextClearedData
{
    std::optional<std::string> initial_message;
    int64_t messages_cleared;
};

inline void from_json(const json& j, ContextClearedData& d)
{
    if (j.contains("initialMessage") && !j["initialMessage"].is_null())
        d.initial_message = j.at("initialMessage").get<std::string>();
    j.at("messagesCleared").get_to(d.messages_cleared);
}

struct FusionRouteStartedData
{
    std::string attempt_id;
    json turn_kind;
    std::optional<std::string> synthetic_model;
    std::optional<std::string> policy;
};

inline void from_json(const json& j, FusionRouteStartedData& d)
{
    j.at("attemptId").get_to(d.attempt_id);
    j.at("turnKind").get_to(d.turn_kind);
    if (j.contains("syntheticModel") && !j["syntheticModel"].is_null())
        d.synthetic_model = j.at("syntheticModel").get<std::string>();
    if (j.contains("policy") && !j["policy"].is_null())
        d.policy = j.at("policy").get<std::string>();
}

struct FusionRouteFailedData
{
    std::string attempt_id;
    std::string synthetic_model;
    std::string policy;
    std::string reason;
    std::optional<std::string> error_message;
    std::string fallback_model;
    std::optional<double> routing_latency_ms;
};

inline void from_json(const json& j, FusionRouteFailedData& d)
{
    j.at("attemptId").get_to(d.attempt_id);
    j.at("syntheticModel").get_to(d.synthetic_model);
    j.at("policy").get_to(d.policy);
    j.at("reason").get_to(d.reason);
    if (j.contains("errorMessage") && !j["errorMessage"].is_null())
        d.error_message = j.at("errorMessage").get<std::string>();
    j.at("fallbackModel").get_to(d.fallback_model);
    if (j.contains("routingLatencyMs") && !j["routingLatencyMs"].is_null())
        d.routing_latency_ms = j.at("routingLatencyMs").get<double>();
}

struct FusionResolvedData
{
    std::string fusion_id;
    std::string turn_id;
    int64_t contract_version;
    std::string synthetic_model;
    std::string policy;
    std::optional<std::string> route_source;
    std::optional<std::string> plan_version;
    std::optional<std::string> policy_version;
    std::optional<std::string> model_universe_version;
    std::optional<std::string> rule_id;
    std::optional<int64_t> rule_index;
    std::optional<std::string> rule_name;
    std::optional<json> scores;
    json pattern;
    std::string primary_model;
    json secondary_model;
    std::string fallback_model;
    std::string follow_up_model;
    std::optional<json> follow_up;
    std::optional<double> routing_latency_ms;
};

inline void from_json(const json& j, FusionResolvedData& d)
{
    j.at("fusionId").get_to(d.fusion_id);
    j.at("turnId").get_to(d.turn_id);
    j.at("contractVersion").get_to(d.contract_version);
    j.at("syntheticModel").get_to(d.synthetic_model);
    j.at("policy").get_to(d.policy);
    if (j.contains("routeSource") && !j["routeSource"].is_null())
        d.route_source = j.at("routeSource").get<std::string>();
    if (j.contains("planVersion") && !j["planVersion"].is_null())
        d.plan_version = j.at("planVersion").get<std::string>();
    if (j.contains("policyVersion") && !j["policyVersion"].is_null())
        d.policy_version = j.at("policyVersion").get<std::string>();
    if (j.contains("modelUniverseVersion") && !j["modelUniverseVersion"].is_null())
        d.model_universe_version = j.at("modelUniverseVersion").get<std::string>();
    if (j.contains("ruleId") && !j["ruleId"].is_null())
        d.rule_id = j.at("ruleId").get<std::string>();
    if (j.contains("ruleIndex") && !j["ruleIndex"].is_null())
        d.rule_index = j.at("ruleIndex").get<int64_t>();
    if (j.contains("ruleName") && !j["ruleName"].is_null())
        d.rule_name = j.at("ruleName").get<std::string>();
    if (j.contains("scores") && !j["scores"].is_null())
        d.scores = j.at("scores").get<json>();
    j.at("pattern").get_to(d.pattern);
    j.at("primaryModel").get_to(d.primary_model);
    j.at("secondaryModel").get_to(d.secondary_model);
    j.at("fallbackModel").get_to(d.fallback_model);
    j.at("followUpModel").get_to(d.follow_up_model);
    if (j.contains("followUp") && !j["followUp"].is_null())
        d.follow_up = j.at("followUp").get<json>();
    if (j.contains("routingLatencyMs") && !j["routingLatencyMs"].is_null())
        d.routing_latency_ms = j.at("routingLatencyMs").get<double>();
}

struct FusionHandoffData
{
    std::string fusion_id;
    std::string source_phase_id;
    std::string target_phase_id;
    std::string target_model;
    json message;
};

inline void from_json(const json& j, FusionHandoffData& d)
{
    j.at("fusionId").get_to(d.fusion_id);
    j.at("sourcePhaseId").get_to(d.source_phase_id);
    j.at("targetPhaseId").get_to(d.target_phase_id);
    j.at("targetModel").get_to(d.target_model);
    j.at("message").get_to(d.message);
}

struct FusionCommitStartedData
{
    std::string fusion_id;
    std::string commit_id;
    std::string source_phase_id;
    std::string source_model;
    json kind;
    json tool_call_id;
};

inline void from_json(const json& j, FusionCommitStartedData& d)
{
    j.at("fusionId").get_to(d.fusion_id);
    j.at("commitId").get_to(d.commit_id);
    j.at("sourcePhaseId").get_to(d.source_phase_id);
    j.at("sourceModel").get_to(d.source_model);
    j.at("kind").get_to(d.kind);
    j.at("toolCallId").get_to(d.tool_call_id);
}

struct FusionCompletedData
{
    std::string fusion_id;
    std::string commit_id;
    std::string turn_id;
    std::string synthetic_model;
    json pattern;
    std::string outcome;
    json final_source_phase_id;
    json final_source_model;
    std::string follow_up_model;
    json degraded_reason;
    int64_t phase_count;
    int64_t request_count;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t cached_tokens;
    std::optional<int64_t> cache_write_tokens;
    double total_nano_aiu;
    double duration_ms;
};

inline void from_json(const json& j, FusionCompletedData& d)
{
    j.at("fusionId").get_to(d.fusion_id);
    j.at("commitId").get_to(d.commit_id);
    j.at("turnId").get_to(d.turn_id);
    j.at("syntheticModel").get_to(d.synthetic_model);
    j.at("pattern").get_to(d.pattern);
    j.at("outcome").get_to(d.outcome);
    j.at("finalSourcePhaseId").get_to(d.final_source_phase_id);
    j.at("finalSourceModel").get_to(d.final_source_model);
    j.at("followUpModel").get_to(d.follow_up_model);
    j.at("degradedReason").get_to(d.degraded_reason);
    j.at("phaseCount").get_to(d.phase_count);
    j.at("requestCount").get_to(d.request_count);
    j.at("inputTokens").get_to(d.input_tokens);
    j.at("outputTokens").get_to(d.output_tokens);
    j.at("cachedTokens").get_to(d.cached_tokens);
    if (j.contains("cacheWriteTokens") && !j["cacheWriteTokens"].is_null())
        d.cache_write_tokens = j.at("cacheWriteTokens").get<int64_t>();
    j.at("totalNanoAiu").get_to(d.total_nano_aiu);
    j.at("durationMs").get_to(d.duration_ms);
}

struct AssistantTurnRetryData
{
    std::string turn_id;
    std::optional<std::string> model;
    std::optional<std::string> reason;
};

inline void from_json(const json& j, AssistantTurnRetryData& d)
{
    j.at("turnId").get_to(d.turn_id);
    if (j.contains("model") && !j["model"].is_null())
        d.model = j.at("model").get<std::string>();
    if (j.contains("reason") && !j["reason"].is_null())
        d.reason = j.at("reason").get<std::string>();
}

struct AgentInterruptedData
{
    json activity;
    double elapsed_ms;
    int64_t turn;
    std::optional<std::string> model;
    std::optional<std::string> api_endpoint;
    std::optional<json> transport;
    std::optional<std::string> reasoning_effort;
    std::optional<json> cancel_phase;
    std::optional<double> output_ttft_ms;
    std::optional<std::vector<json>> tool_names;
    std::optional<std::vector<json>> tool_call_ids;
    std::optional<std::vector<json>> safe_tool_names;
    std::optional<int64_t> interrupted_agent_count;
};

inline void from_json(const json& j, AgentInterruptedData& d)
{
    j.at("activity").get_to(d.activity);
    j.at("elapsedMs").get_to(d.elapsed_ms);
    j.at("turn").get_to(d.turn);
    if (j.contains("model") && !j["model"].is_null())
        d.model = j.at("model").get<std::string>();
    if (j.contains("apiEndpoint") && !j["apiEndpoint"].is_null())
        d.api_endpoint = j.at("apiEndpoint").get<std::string>();
    if (j.contains("transport") && !j["transport"].is_null())
        d.transport = j.at("transport").get<json>();
    if (j.contains("reasoningEffort") && !j["reasoningEffort"].is_null())
        d.reasoning_effort = j.at("reasoningEffort").get<std::string>();
    if (j.contains("cancelPhase") && !j["cancelPhase"].is_null())
        d.cancel_phase = j.at("cancelPhase").get<json>();
    if (j.contains("outputTtftMs") && !j["outputTtftMs"].is_null())
        d.output_ttft_ms = j.at("outputTtftMs").get<double>();
    if (j.contains("toolNames") && !j["toolNames"].is_null())
        d.tool_names = j.at("toolNames").get<std::vector<json>>();
    if (j.contains("toolCallIds") && !j["toolCallIds"].is_null())
        d.tool_call_ids = j.at("toolCallIds").get<std::vector<json>>();
    if (j.contains("safeToolNames") && !j["safeToolNames"].is_null())
        d.safe_tool_names = j.at("safeToolNames").get<std::vector<json>>();
    if (j.contains("interruptedAgentCount") && !j["interruptedAgentCount"].is_null())
        d.interrupted_agent_count = j.at("interruptedAgentCount").get<int64_t>();
}

struct FusionPhaseStartedData
{
    std::string fusion_id;
    std::string phase_id;
    json phase_kind;
    json pattern;
    std::string role;
    json conversation_scope;
    std::string model;
};

inline void from_json(const json& j, FusionPhaseStartedData& d)
{
    j.at("fusionId").get_to(d.fusion_id);
    j.at("phaseId").get_to(d.phase_id);
    j.at("phaseKind").get_to(d.phase_kind);
    j.at("pattern").get_to(d.pattern);
    j.at("role").get_to(d.role);
    j.at("conversationScope").get_to(d.conversation_scope);
    j.at("model").get_to(d.model);
}

struct FusionPhaseCompletedData
{
    std::string fusion_id;
    std::string phase_id;
    json phase_kind;
    std::string role;
    json conversation_scope;
    std::string model;
    json status;
    std::string content;
    json verdict;
    double duration_ms;
    json usage;
    std::optional<json> projection_message;
    std::optional<json> projection_mode;
    std::optional<json> staged_terminal;
};

inline void from_json(const json& j, FusionPhaseCompletedData& d)
{
    j.at("fusionId").get_to(d.fusion_id);
    j.at("phaseId").get_to(d.phase_id);
    j.at("phaseKind").get_to(d.phase_kind);
    j.at("role").get_to(d.role);
    j.at("conversationScope").get_to(d.conversation_scope);
    j.at("model").get_to(d.model);
    j.at("status").get_to(d.status);
    j.at("content").get_to(d.content);
    j.at("verdict").get_to(d.verdict);
    j.at("durationMs").get_to(d.duration_ms);
    j.at("usage").get_to(d.usage);
    if (j.contains("projectionMessage") && !j["projectionMessage"].is_null())
        d.projection_message = j.at("projectionMessage").get<json>();
    if (j.contains("projectionMode") && !j["projectionMode"].is_null())
        d.projection_mode = j.at("projectionMode").get<json>();
    if (j.contains("stagedTerminal") && !j["stagedTerminal"].is_null())
        d.staged_terminal = j.at("stagedTerminal").get<json>();
}

struct FusionPhaseFailedData
{
    std::string fusion_id;
    std::string phase_id;
    json phase_kind;
    std::string role;
    json conversation_scope;
    std::string model;
    json status;
    std::string reason;
    double duration_ms;
    json usage;
    std::optional<std::string> error_message;
    std::optional<std::string> degraded_to_phase_id;
};

inline void from_json(const json& j, FusionPhaseFailedData& d)
{
    j.at("fusionId").get_to(d.fusion_id);
    j.at("phaseId").get_to(d.phase_id);
    j.at("phaseKind").get_to(d.phase_kind);
    j.at("role").get_to(d.role);
    j.at("conversationScope").get_to(d.conversation_scope);
    j.at("model").get_to(d.model);
    j.at("status").get_to(d.status);
    j.at("reason").get_to(d.reason);
    j.at("durationMs").get_to(d.duration_ms);
    j.at("usage").get_to(d.usage);
    if (j.contains("errorMessage") && !j["errorMessage"].is_null())
        d.error_message = j.at("errorMessage").get<std::string>();
    if (j.contains("degradedToPhaseId") && !j["degradedToPhaseId"].is_null())
        d.degraded_to_phase_id = j.at("degradedToPhaseId").get<std::string>();
}

struct AssistantServerToolProgressData
{
    int64_t output_index;
    std::string kind;
    std::string status;
};

inline void from_json(const json& j, AssistantServerToolProgressData& d)
{
    j.at("outputIndex").get_to(d.output_index);
    j.at("kind").get_to(d.kind);
    j.at("status").get_to(d.status);
}

struct AssistantToolCallDeltaData
{
    std::string tool_call_id;
    std::optional<std::string> tool_name;
    std::optional<json> tool_type;
    std::string input_delta;
};

inline void from_json(const json& j, AssistantToolCallDeltaData& d)
{
    j.at("toolCallId").get_to(d.tool_call_id);
    if (j.contains("toolName") && !j["toolName"].is_null())
        d.tool_name = j.at("toolName").get<std::string>();
    if (j.contains("toolType") && !j["toolType"].is_null())
        d.tool_type = j.at("toolType").get<json>();
    j.at("inputDelta").get_to(d.input_delta);
}

struct AssistantIdleData
{
    std::optional<bool> aborted;
};

inline void from_json(const json& j, AssistantIdleData& d)
{
    if (j.contains("aborted") && !j["aborted"].is_null())
        d.aborted = j.at("aborted").get<bool>();
}

struct PromptCacheBreakData
{
    std::string primary_reason;
    std::vector<json> contributing_reasons;
    std::optional<json> before_request;
    std::optional<json> after_request;
    int64_t survived_tokens;
    int64_t frontier_tokens;
    int64_t shortfall_tokens;
    double retention_ratio;
    std::optional<std::string> model_from;
    std::optional<std::string> model_to;
    std::optional<std::vector<json>> tools_added;
    std::optional<std::vector<json>> tools_removed;
    std::optional<std::vector<json>> tools_redefined;
    std::optional<std::vector<json>> tools_added_raw;
    std::optional<std::vector<json>> tools_removed_raw;
    std::optional<std::vector<json>> tools_redefined_raw;
    std::optional<bool> tools_reordered;
    std::optional<std::vector<json>> system_segments_changed;
    std::optional<std::vector<json>> cache_config_changed_fields;
    std::optional<int64_t> rewrite_message_index;
    std::optional<std::string> rewrite_shape;
    std::optional<std::vector<json>> rewrite_source;
    std::optional<std::string> agent_name;
};

inline void from_json(const json& j, PromptCacheBreakData& d)
{
    j.at("primaryReason").get_to(d.primary_reason);
    j.at("contributingReasons").get_to(d.contributing_reasons);
    if (j.contains("beforeRequest") && !j["beforeRequest"].is_null())
        d.before_request = j.at("beforeRequest").get<json>();
    if (j.contains("afterRequest") && !j["afterRequest"].is_null())
        d.after_request = j.at("afterRequest").get<json>();
    j.at("survivedTokens").get_to(d.survived_tokens);
    j.at("frontierTokens").get_to(d.frontier_tokens);
    j.at("shortfallTokens").get_to(d.shortfall_tokens);
    j.at("retentionRatio").get_to(d.retention_ratio);
    if (j.contains("modelFrom") && !j["modelFrom"].is_null())
        d.model_from = j.at("modelFrom").get<std::string>();
    if (j.contains("modelTo") && !j["modelTo"].is_null())
        d.model_to = j.at("modelTo").get<std::string>();
    if (j.contains("toolsAdded") && !j["toolsAdded"].is_null())
        d.tools_added = j.at("toolsAdded").get<std::vector<json>>();
    if (j.contains("toolsRemoved") && !j["toolsRemoved"].is_null())
        d.tools_removed = j.at("toolsRemoved").get<std::vector<json>>();
    if (j.contains("toolsRedefined") && !j["toolsRedefined"].is_null())
        d.tools_redefined = j.at("toolsRedefined").get<std::vector<json>>();
    if (j.contains("toolsAddedRaw") && !j["toolsAddedRaw"].is_null())
        d.tools_added_raw = j.at("toolsAddedRaw").get<std::vector<json>>();
    if (j.contains("toolsRemovedRaw") && !j["toolsRemovedRaw"].is_null())
        d.tools_removed_raw = j.at("toolsRemovedRaw").get<std::vector<json>>();
    if (j.contains("toolsRedefinedRaw") && !j["toolsRedefinedRaw"].is_null())
        d.tools_redefined_raw = j.at("toolsRedefinedRaw").get<std::vector<json>>();
    if (j.contains("toolsReordered") && !j["toolsReordered"].is_null())
        d.tools_reordered = j.at("toolsReordered").get<bool>();
    if (j.contains("systemSegmentsChanged") && !j["systemSegmentsChanged"].is_null())
        d.system_segments_changed = j.at("systemSegmentsChanged").get<std::vector<json>>();
    if (j.contains("cacheConfigChangedFields") && !j["cacheConfigChangedFields"].is_null())
        d.cache_config_changed_fields = j.at("cacheConfigChangedFields").get<std::vector<json>>();
    if (j.contains("rewriteMessageIndex") && !j["rewriteMessageIndex"].is_null())
        d.rewrite_message_index = j.at("rewriteMessageIndex").get<int64_t>();
    if (j.contains("rewriteShape") && !j["rewriteShape"].is_null())
        d.rewrite_shape = j.at("rewriteShape").get<std::string>();
    if (j.contains("rewriteSource") && !j["rewriteSource"].is_null())
        d.rewrite_source = j.at("rewriteSource").get<std::vector<json>>();
    if (j.contains("agentName") && !j["agentName"].is_null())
        d.agent_name = j.at("agentName").get<std::string>();
}

struct ModelCallFinishedData
{
    std::string turn_id;
    std::optional<std::string> interaction_id;
    double dispatch_duration_ms;
    json outcome;
    std::optional<bool> contains_built_in_file_edit_request;
    int64_t edit_classifier_version;
};

inline void from_json(const json& j, ModelCallFinishedData& d)
{
    j.at("turnId").get_to(d.turn_id);
    if (j.contains("interactionId") && !j["interactionId"].is_null())
        d.interaction_id = j.at("interactionId").get<std::string>();
    j.at("dispatchDurationMs").get_to(d.dispatch_duration_ms);
    j.at("outcome").get_to(d.outcome);
    if (j.contains("containsBuiltInFileEditRequest") && !j["containsBuiltInFileEditRequest"].is_null())
        d.contains_built_in_file_edit_request = j.at("containsBuiltInFileEditRequest").get<bool>();
    j.at("editClassifierVersion").get_to(d.edit_classifier_version);
}

struct ModelCallStartData
{
    std::string turn_id;
    std::optional<std::string> model;
    std::optional<std::string> previous_response_id;
    std::optional<json> fusion;
};

inline void from_json(const json& j, ModelCallStartData& d)
{
    j.at("turnId").get_to(d.turn_id);
    if (j.contains("model") && !j["model"].is_null())
        d.model = j.at("model").get<std::string>();
    if (j.contains("previousResponseId") && !j["previousResponseId"].is_null())
        d.previous_response_id = j.at("previousResponseId").get<std::string>();
    if (j.contains("fusion") && !j["fusion"].is_null())
        d.fusion = j.at("fusion").get<json>();
}

struct ToolSearchActivatedData
{
    std::string strategy;
    std::vector<json> tool_names;
};

inline void from_json(const json& j, ToolSearchActivatedData& d)
{
    j.at("strategy").get_to(d.strategy);
    j.at("toolNames").get_to(d.tool_names);
}

struct SandboxDecisionData
{
    // No payload fields in the official schema.
};

inline void from_json(const json& j, SandboxDecisionData& d)
{
    (void)j;
    (void)d;
}

struct SubagentConfiguredData
{
    std::string model;
    std::optional<std::string> reasoning_effort;
    std::optional<std::string> context_tier;
    bool multi_turn;
};

inline void from_json(const json& j, SubagentConfiguredData& d)
{
    j.at("model").get_to(d.model);
    if (j.contains("reasoningEffort") && !j["reasoningEffort"].is_null())
        d.reasoning_effort = j.at("reasoningEffort").get<std::string>();
    if (j.contains("contextTier") && !j["contextTier"].is_null())
        d.context_tier = j.at("contextTier").get<std::string>();
    j.at("multiTurn").get_to(d.multi_turn);
}

struct HookProgressData
{
    std::string message;
    std::optional<bool> temporary;
};

inline void from_json(const json& j, HookProgressData& d)
{
    j.at("message").get_to(d.message);
    if (j.contains("temporary") && !j["temporary"].is_null())
        d.temporary = j.at("temporary").get<bool>();
}

struct BinaryAssetData
{
    std::string asset_id;
    json type;
    std::string mime_type;
    int64_t byte_length;
    std::string data;
    std::optional<std::string> description;
    std::optional<json> metadata;
};

inline void from_json(const json& j, BinaryAssetData& d)
{
    j.at("assetId").get_to(d.asset_id);
    j.at("type").get_to(d.type);
    j.at("mimeType").get_to(d.mime_type);
    j.at("byteLength").get_to(d.byte_length);
    j.at("data").get_to(d.data);
    if (j.contains("description") && !j["description"].is_null())
        d.description = j.at("description").get<std::string>();
    if (j.contains("metadata") && !j["metadata"].is_null())
        d.metadata = j.at("metadata").get<json>();
}

struct McpHeadersRefreshRequiredData
{
    std::string request_id;
    std::string server_name;
    std::string server_url;
    json reason;
};

inline void from_json(const json& j, McpHeadersRefreshRequiredData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("serverName").get_to(d.server_name);
    j.at("serverUrl").get_to(d.server_url);
    j.at("reason").get_to(d.reason);
}

struct McpHeadersRefreshCompletedData
{
    std::string request_id;
    json outcome;
};

inline void from_json(const json& j, McpHeadersRefreshCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("outcome").get_to(d.outcome);
}

struct UIEphemeralQueryData
{
    std::string request_id;
    json phase;
    std::optional<std::string> chunk;
    std::optional<std::string> answer;
    std::optional<std::string> error;
};

inline void from_json(const json& j, UIEphemeralQueryData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("phase").get_to(d.phase);
    if (j.contains("chunk") && !j["chunk"].is_null())
        d.chunk = j.at("chunk").get<std::string>();
    if (j.contains("answer") && !j["answer"].is_null())
        d.answer = j.at("answer").get<std::string>();
    if (j.contains("error") && !j["error"].is_null())
        d.error = j.at("error").get<std::string>();
}

struct SessionLimitsExhaustedRequestedData
{
    std::string request_id;
    double used_ai_credits;
    double max_ai_credits;
};

inline void from_json(const json& j, SessionLimitsExhaustedRequestedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("usedAiCredits").get_to(d.used_ai_credits);
    j.at("maxAiCredits").get_to(d.max_ai_credits);
}

struct SessionLimitsExhaustedCompletedData
{
    std::string request_id;
    json response;
};

inline void from_json(const json& j, SessionLimitsExhaustedCompletedData& d)
{
    j.at("requestId").get_to(d.request_id);
    j.at("response").get_to(d.response);
}

struct AutoModeResolvedData
{
    std::string chosen_model;
    std::optional<json> reasoning_bucket;
    std::optional<json> category_scores;
    std::optional<std::string> predicted_label;
    std::optional<double> confidence;
    std::optional<std::vector<json>> candidate_models;
    std::optional<std::string> routing_method;
    std::optional<std::vector<json>> available_models;
    std::optional<bool> fallback;
    std::optional<std::string> fallback_reason;
    std::optional<bool> sticky_override;
    std::optional<double> router_latency_ms;
    std::optional<double> end_to_end_latency_ms;
    std::optional<double> chosen_shortfall;
    std::optional<bool> has_image;
};

inline void from_json(const json& j, AutoModeResolvedData& d)
{
    j.at("chosenModel").get_to(d.chosen_model);
    if (j.contains("reasoningBucket") && !j["reasoningBucket"].is_null())
        d.reasoning_bucket = j.at("reasoningBucket").get<json>();
    if (j.contains("categoryScores") && !j["categoryScores"].is_null())
        d.category_scores = j.at("categoryScores").get<json>();
    if (j.contains("predictedLabel") && !j["predictedLabel"].is_null())
        d.predicted_label = j.at("predictedLabel").get<std::string>();
    if (j.contains("confidence") && !j["confidence"].is_null())
        d.confidence = j.at("confidence").get<double>();
    if (j.contains("candidateModels") && !j["candidateModels"].is_null())
        d.candidate_models = j.at("candidateModels").get<std::vector<json>>();
    if (j.contains("routingMethod") && !j["routingMethod"].is_null())
        d.routing_method = j.at("routingMethod").get<std::string>();
    if (j.contains("availableModels") && !j["availableModels"].is_null())
        d.available_models = j.at("availableModels").get<std::vector<json>>();
    if (j.contains("fallback") && !j["fallback"].is_null())
        d.fallback = j.at("fallback").get<bool>();
    if (j.contains("fallbackReason") && !j["fallbackReason"].is_null())
        d.fallback_reason = j.at("fallbackReason").get<std::string>();
    if (j.contains("stickyOverride") && !j["stickyOverride"].is_null())
        d.sticky_override = j.at("stickyOverride").get<bool>();
    if (j.contains("routerLatencyMs") && !j["routerLatencyMs"].is_null())
        d.router_latency_ms = j.at("routerLatencyMs").get<double>();
    if (j.contains("endToEndLatencyMs") && !j["endToEndLatencyMs"].is_null())
        d.end_to_end_latency_ms = j.at("endToEndLatencyMs").get<double>();
    if (j.contains("chosenShortfall") && !j["chosenShortfall"].is_null())
        d.chosen_shortfall = j.at("chosenShortfall").get<double>();
    if (j.contains("hasImage") && !j["hasImage"].is_null())
        d.has_image = j.at("hasImage").get<bool>();
}

struct ManagedSettingsResolvedData
{
    json source;
    bool server_managed;
    bool device_managed;
    std::optional<bool> client_managed;
    std::optional<bool> policy_helper_managed;
    bool fail_closed;
    std::optional<bool> sandbox_enabled_by_undetermined_policy;
    bool bypass_permissions_disabled;
    std::optional<bool> permissions_allow_intersected;
    std::vector<json> managed_keys;
    std::optional<json> settings;
};

inline void from_json(const json& j, ManagedSettingsResolvedData& d)
{
    j.at("source").get_to(d.source);
    j.at("serverManaged").get_to(d.server_managed);
    j.at("deviceManaged").get_to(d.device_managed);
    if (j.contains("clientManaged") && !j["clientManaged"].is_null())
        d.client_managed = j.at("clientManaged").get<bool>();
    if (j.contains("policyHelperManaged") && !j["policyHelperManaged"].is_null())
        d.policy_helper_managed = j.at("policyHelperManaged").get<bool>();
    j.at("failClosed").get_to(d.fail_closed);
    if (j.contains("sandboxEnabledByUndeterminedPolicy") && !j["sandboxEnabledByUndeterminedPolicy"].is_null())
        d.sandbox_enabled_by_undetermined_policy = j.at("sandboxEnabledByUndeterminedPolicy").get<bool>();
    j.at("bypassPermissionsDisabled").get_to(d.bypass_permissions_disabled);
    if (j.contains("permissionsAllowIntersected") && !j["permissionsAllowIntersected"].is_null())
        d.permissions_allow_intersected = j.at("permissionsAllowIntersected").get<bool>();
    j.at("managedKeys").get_to(d.managed_keys);
    if (j.contains("settings") && !j["settings"].is_null())
        d.settings = j.at("settings").get<json>();
}

struct ManagedSettingsEnforcedData
{
    json action;
    std::optional<json> escalation;
    std::string setting;
    bool fail_closed;
    std::string message;
};

inline void from_json(const json& j, ManagedSettingsEnforcedData& d)
{
    j.at("action").get_to(d.action);
    if (j.contains("escalation") && !j["escalation"].is_null())
        d.escalation = j.at("escalation").get<json>();
    j.at("setting").get_to(d.setting);
    j.at("failClosed").get_to(d.fail_closed);
    j.at("message").get_to(d.message);
}

struct FactoryRunUpdatedData
{
    std::string run_id;
    int64_t revision;
};

inline void from_json(const json& j, FactoryRunUpdatedData& d)
{
    j.at("runId").get_to(d.run_id);
    j.at("revision").get_to(d.revision);
}

struct FactoryRunStartedData
{
    std::string run_id;
    std::string factory_name;
    int64_t attempt;
};

inline void from_json(const json& j, FactoryRunStartedData& d)
{
    j.at("runId").get_to(d.run_id);
    j.at("factoryName").get_to(d.factory_name);
    j.at("attempt").get_to(d.attempt);
}

struct FactoryRunSettledData
{
    std::string run_id;
    json status;
    int64_t consumed_subagents;
    int64_t consumed_nano_aiu;
    int64_t elapsed_ms;
    std::optional<std::string> failure_type;
};

inline void from_json(const json& j, FactoryRunSettledData& d)
{
    j.at("runId").get_to(d.run_id);
    j.at("status").get_to(d.status);
    j.at("consumedSubagents").get_to(d.consumed_subagents);
    j.at("consumedNanoAiu").get_to(d.consumed_nano_aiu);
    j.at("elapsedMs").get_to(d.elapsed_ms);
    if (j.contains("failureType") && !j["failureType"].is_null())
        d.failure_type = j.at("failureType").get<std::string>();
}

struct McpListChangedData
{
    std::string server_name;
};

inline void from_json(const json& j, McpListChangedData& d)
{
    j.at("serverName").get_to(d.server_name);
}

struct CanvasOpenedData
{
    std::string instance_id;
    std::string extension_id;
    std::optional<std::string> extension_name;
    std::string canvas_id;
    std::optional<std::string> icon;
    std::optional<std::string> title;
    std::optional<std::string> status;
    std::optional<std::string> url;
    std::optional<json> input;
};

inline void from_json(const json& j, CanvasOpenedData& d)
{
    j.at("instanceId").get_to(d.instance_id);
    j.at("extensionId").get_to(d.extension_id);
    if (j.contains("extensionName") && !j["extensionName"].is_null())
        d.extension_name = j.at("extensionName").get<std::string>();
    j.at("canvasId").get_to(d.canvas_id);
    if (j.contains("icon") && !j["icon"].is_null())
        d.icon = j.at("icon").get<std::string>();
    if (j.contains("title") && !j["title"].is_null())
        d.title = j.at("title").get<std::string>();
    if (j.contains("status") && !j["status"].is_null())
        d.status = j.at("status").get<std::string>();
    if (j.contains("url") && !j["url"].is_null())
        d.url = j.at("url").get<std::string>();
    if (j.contains("input") && !j["input"].is_null())
        d.input = j.at("input").get<json>();
}

struct CanvasRegistryChangedData
{
    std::vector<json> canvases;
};

inline void from_json(const json& j, CanvasRegistryChangedData& d)
{
    j.at("canvases").get_to(d.canvases);
}

struct CanvasClosedData
{
    std::string instance_id;
    std::string extension_id;
    std::string canvas_id;
};

inline void from_json(const json& j, CanvasClosedData& d)
{
    j.at("instanceId").get_to(d.instance_id);
    j.at("extensionId").get_to(d.extension_id);
    j.at("canvasId").get_to(d.canvas_id);
}

struct CanvasUnavailableData
{
    std::string instance_id;
    std::string extension_id;
    std::string canvas_id;
};

inline void from_json(const json& j, CanvasUnavailableData& d)
{
    j.at("instanceId").get_to(d.instance_id);
    j.at("extensionId").get_to(d.extension_id);
    j.at("canvasId").get_to(d.canvas_id);
}

struct CanvasRecordedData
{
    std::string instance_id;
    std::string extension_id;
    std::string canvas_id;
    std::optional<std::string> title;
    std::optional<json> input;
};

inline void from_json(const json& j, CanvasRecordedData& d)
{
    j.at("instanceId").get_to(d.instance_id);
    j.at("extensionId").get_to(d.extension_id);
    j.at("canvasId").get_to(d.canvas_id);
    if (j.contains("title") && !j["title"].is_null())
        d.title = j.at("title").get<std::string>();
    if (j.contains("input") && !j["input"].is_null())
        d.input = j.at("input").get<json>();
}

struct CanvasRemovedData
{
    std::string instance_id;
    std::string extension_id;
    std::string canvas_id;
};

inline void from_json(const json& j, CanvasRemovedData& d)
{
    j.at("instanceId").get_to(d.instance_id);
    j.at("extensionId").get_to(d.extension_id);
    j.at("canvasId").get_to(d.canvas_id);
}

struct ExtensionsAttachmentsPushedData
{
    std::vector<json> attachments;
};

inline void from_json(const json& j, ExtensionsAttachmentsPushedData& d)
{
    j.at("attachments").get_to(d.attachments);
}

struct McpAppToolCallCompleteData
{
    std::string server_name;
    std::string tool_name;
    std::optional<json> arguments;
    bool success;
    double duration_ms;
    std::optional<json> result;
    std::optional<json> error;
    std::optional<json> tool_meta;
};

inline void from_json(const json& j, McpAppToolCallCompleteData& d)
{
    j.at("serverName").get_to(d.server_name);
    j.at("toolName").get_to(d.tool_name);
    if (j.contains("arguments") && !j["arguments"].is_null())
        d.arguments = j.at("arguments").get<json>();
    j.at("success").get_to(d.success);
    j.at("durationMs").get_to(d.duration_ms);
    if (j.contains("result") && !j["result"].is_null())
        d.result = j.at("result").get<json>();
    if (j.contains("error") && !j["error"].is_null())
        d.error = j.at("error").get<json>();
    if (j.contains("toolMeta") && !j["toolMeta"].is_null())
        d.tool_meta = j.at("toolMeta").get<json>();
}

/// All possible event types
enum class SessionEventType
{
    SessionStart,
    SessionResume,
    SessionError,
    SessionIdle,
    SessionInfo,
    SessionModelChange,
    SessionHandoff,
    SessionTruncation,
    UserMessage,
    PendingMessagesModified,
    AssistantTurnStart,
    AssistantIntent,
    AssistantReasoning,
    AssistantReasoningDelta,
    AssistantMessage,
    AssistantMessageDelta,
    AssistantTurnEnd,
    AssistantUsage,
    Abort,
    ToolUserRequested,
    ToolExecutionStart,
    ToolExecutionPartialResult,
    ToolExecutionComplete,
    ToolExecutionProgress,
    SessionCompactionStart,
    SessionCompactionComplete,
    SessionUsageInfo,
    CustomAgentStarted,
    CustomAgentCompleted,
    CustomAgentFailed,
    CustomAgentSelected,
    HookStart,
    HookEnd,
    SystemMessage,
    SessionSnapshotRewind,
    SessionShutdown,
    SkillInvoked,
    // v0.1.49+ additions
    SessionRemoteSteerableChanged,
    SessionTitleChanged,
    SessionScheduleCreated,
    SessionScheduleCancelled,
    SessionWarning,
    SessionModeChanged,
    SessionPlanChanged,
    SessionWorkspaceFileChanged,
    SessionContextChanged,
    SessionTaskComplete,
    SessionCustomNotification,
    SessionToolsUpdated,
    SessionBackgroundTasksChanged,
    SessionSkillsLoaded,
    SessionCustomAgentsUpdated,
    SessionMcpServersLoaded,
    SessionMcpServerStatusChanged,
    SessionExtensionsLoaded,
    AssistantStreamingDelta,
    AssistantMessageStart,
    ModelCallFailure,
    SubagentDeselected,
    PermissionRequested,
    PermissionCompleted,
    UserInputRequested,
    UserInputCompleted,
    ElicitationRequested,
    ElicitationCompleted,
    SamplingRequested,
    SamplingCompleted,
    McpOauthRequired,
    McpOauthCompleted,
    ExternalToolRequested,
    ExternalToolCompleted,
    CommandQueued,
    CommandExecute,
    CommandCompleted,
    AutoModeSwitchRequested,
    AutoModeSwitchCompleted,
    CommandsChanged,
    CapabilitiesChanged,
    ExitPlanModeRequested,
    ExitPlanModeCompleted,
    SystemNotification,
    SessionScheduleRearmed,
    SessionAutopilotObjectiveChanged,
    SessionSessionLimitsChanged,
    SessionPermissionsChanged,
    SessionTodosChanged,
    SessionMemoryChanged,
    SessionUsageCheckpoint,
    SessionContextCleared,
    SessionFusionRouteStarted,
    SessionFusionRouteFailed,
    SessionFusionResolved,
    SessionFusionHandoff,
    SessionFusionCommitStarted,
    SessionFusionCompleted,
    AssistantTurnRetry,
    AgentInterrupted,
    AssistantFusionPhaseStarted,
    AssistantFusionPhaseCompleted,
    AssistantFusionPhaseFailed,
    AssistantServerToolProgress,
    AssistantToolCallDelta,
    AssistantIdle,
    PromptCacheBreak,
    ModelCallFinished,
    ModelCallStart,
    ToolSearchActivated,
    SandboxDecision,
    SubagentConfigured,
    HookProgress,
    SessionBinaryAsset,
    McpHeadersRefreshRequired,
    McpHeadersRefreshCompleted,
    UiEphemeralQuery,
    SessionLimitsExhaustedRequested,
    SessionLimitsExhaustedCompleted,
    SessionAutoModeResolved,
    SessionManagedSettingsResolved,
    SessionManagedSettingsEnforced,
    FactoryRunUpdated,
    FactoryRunStarted,
    FactoryRunSettled,
    McpToolsListChanged,
    McpResourcesListChanged,
    McpPromptsListChanged,
    SessionCanvasOpened,
    SessionCanvasRegistryChanged,
    SessionCanvasClosed,
    SessionCanvasUnavailable,
    SessionCanvasRecorded,
    SessionCanvasRemoved,
    SessionExtensionsAttachmentsPushed,
    McpAppToolCallComplete,
    // Added with the @github/copilot 1.0.84-5 re-pin (upstream f45c46fd).
    SessionAutoTierRecommendation,
    SessionAutoTierSwitchFailed,
    SessionModeNoticeDelivered,
    SessionCompletionReceipt,
    AssistantFusionPhaseActivity,
    PermissionCarriedForward,
    PermissionMessageAuthorization,
    PermissionMessageAuthorizationRead,
    PermissionMessageAuthorizationDegraded,
    SessionMcpServerRemoved,
    SessionMcpServerNeedsReconnect,
    Unknown
};

/// Variant holding all possible event data types
using SessionEventData = std::variant<
    SessionStartData,
    SessionResumeData,
    SessionErrorData,
    SessionIdleData,
    SessionInfoData,
    SessionModelChangeData,
    SessionHandoffData,
    SessionTruncationData,
    UserMessageData,
    PendingMessagesModifiedData,
    AssistantTurnStartData,
    AssistantIntentData,
    AssistantReasoningData,
    AssistantReasoningDeltaData,
    AssistantMessageData,
    AssistantMessageDeltaData,
    AssistantTurnEndData,
    AssistantUsageData,
    AbortData,
    ToolUserRequestedData,
    ToolExecutionStartData,
    ToolExecutionPartialResultData,
    ToolExecutionCompleteData,
    ToolExecutionProgressData,
    SessionCompactionStartData,
    SessionCompactionCompleteData,
    SessionUsageInfoData,
    CustomAgentStartedData,
    CustomAgentCompletedData,
    CustomAgentFailedData,
    CustomAgentSelectedData,
    HookStartData,
    HookEndData,
    SystemMessageData,
    SessionSnapshotRewindData,
    SessionShutdownData,
    SkillInvokedData,
    // v0.1.49+ additions
    SessionRemoteSteerableChangedData,
    SessionTitleChangedData,
    SessionScheduleCreatedData,
    SessionScheduleCancelledData,
    SessionWarningData,
    SessionModeChangedData,
    SessionPlanChangedData,
    SessionWorkspaceFileChangedData,
    SessionContextChangedData,
    SessionTaskCompleteData,
    SessionCustomNotificationData,
    SessionToolsUpdatedData,
    SessionBackgroundTasksChangedData,
    SessionSkillsLoadedData,
    SessionCustomAgentsUpdatedData,
    SessionMcpServersLoadedData,
    SessionMcpServerStatusChangedData,
    SessionExtensionsLoadedData,
    AssistantStreamingDeltaData,
    AssistantMessageStartData,
    ModelCallFailureData,
    SubagentDeselectedData,
    PermissionRequestedData,
    PermissionCompletedData,
    UserInputRequestedData,
    UserInputCompletedData,
    ElicitationRequestedData,
    ElicitationCompletedData,
    SamplingRequestedData,
    SamplingCompletedData,
    McpOauthRequiredData,
    McpOauthCompletedData,
    ExternalToolRequestedData,
    ExternalToolCompletedData,
    CommandQueuedData,
    CommandExecuteData,
    CommandCompletedData,
    AutoModeSwitchRequestedData,
    AutoModeSwitchCompletedData,
    CommandsChangedData,
    CapabilitiesChangedData,
    ExitPlanModeRequestedData,
    ExitPlanModeCompletedData,
    SystemNotificationData,
    ScheduleRearmedData,
    AutopilotObjectiveChangedData,
    SessionLimitsChangedData,
    PermissionsChangedData,
    TodosChangedData,
    MemoryChangedData,
    UsageCheckpointData,
    ContextClearedData,
    FusionRouteStartedData,
    FusionRouteFailedData,
    FusionResolvedData,
    FusionHandoffData,
    FusionCommitStartedData,
    FusionCompletedData,
    AssistantTurnRetryData,
    AgentInterruptedData,
    FusionPhaseStartedData,
    FusionPhaseCompletedData,
    FusionPhaseFailedData,
    AssistantServerToolProgressData,
    AssistantToolCallDeltaData,
    AssistantIdleData,
    PromptCacheBreakData,
    ModelCallFinishedData,
    ModelCallStartData,
    ToolSearchActivatedData,
    SandboxDecisionData,
    SubagentConfiguredData,
    HookProgressData,
    BinaryAssetData,
    McpHeadersRefreshRequiredData,
    McpHeadersRefreshCompletedData,
    UIEphemeralQueryData,
    SessionLimitsExhaustedRequestedData,
    SessionLimitsExhaustedCompletedData,
    AutoModeResolvedData,
    ManagedSettingsResolvedData,
    ManagedSettingsEnforcedData,
    FactoryRunUpdatedData,
    FactoryRunStartedData,
    FactoryRunSettledData,
    McpListChangedData,
    CanvasOpenedData,
    CanvasRegistryChangedData,
    CanvasClosedData,
    CanvasUnavailableData,
    CanvasRecordedData,
    CanvasRemovedData,
    ExtensionsAttachmentsPushedData,
    McpAppToolCallCompleteData,
    json // Unknown event fallback
    >;

/// Base session event with common fields and typed data
struct SessionEvent
{
    std::string id;
    std::string timestamp; // ISO 8601
    std::optional<std::string> parent_id;
    std::optional<bool> ephemeral;
    std::optional<std::string> agent_id; // sub-agent instance identifier
    SessionEventType type;
    std::string type_string; // Original type string for unknown events
    SessionEventData data;

    /// Check if this is a specific event type
    template <typename T>
    bool is() const
    {
        return std::holds_alternative<T>(data);
    }

    /// Get event data as specific type (throws if wrong type)
    template <typename T>
    const T& as() const
    {
        return std::get<T>(data);
    }

    /// Get event data as specific type (returns nullptr if wrong type)
    template <typename T>
    const T* try_as() const
    {
        return std::get_if<T>(&data);
    }
};

/// Parse session event from JSON
inline SessionEvent parse_session_event(const json& j)
{
    SessionEvent event;

    // Parse common fields
    event.id = j.at("id").get<std::string>();
    event.timestamp = j.at("timestamp").get<std::string>();
    if (j.contains("parentId") && !j.at("parentId").is_null())
        event.parent_id = j.at("parentId").get<std::string>();
    if (j.contains("ephemeral"))
        event.ephemeral = j.at("ephemeral").get<bool>();
    if (j.contains("agentId") && !j.at("agentId").is_null())
        event.agent_id = j.at("agentId").get<std::string>();

    // Parse type and data
    event.type_string = j.at("type").get<std::string>();
    const auto& data_json = j.at("data");

    // Map type string to enum and parse data
    static const std::map<std::string, SessionEventType> type_map = {
        {"session.start", SessionEventType::SessionStart},
        {"session.resume", SessionEventType::SessionResume},
        {"session.error", SessionEventType::SessionError},
        {"session.idle", SessionEventType::SessionIdle},
        {"session.info", SessionEventType::SessionInfo},
        {"session.model_change", SessionEventType::SessionModelChange},
        {"session.handoff", SessionEventType::SessionHandoff},
        {"session.truncation", SessionEventType::SessionTruncation},
        {"user.message", SessionEventType::UserMessage},
        {"pending_messages.modified", SessionEventType::PendingMessagesModified},
        {"assistant.turn_start", SessionEventType::AssistantTurnStart},
        {"assistant.intent", SessionEventType::AssistantIntent},
        {"assistant.reasoning", SessionEventType::AssistantReasoning},
        {"assistant.reasoning_delta", SessionEventType::AssistantReasoningDelta},
        {"assistant.message", SessionEventType::AssistantMessage},
        {"assistant.message_delta", SessionEventType::AssistantMessageDelta},
        {"assistant.turn_end", SessionEventType::AssistantTurnEnd},
        {"assistant.usage", SessionEventType::AssistantUsage},
        {"abort", SessionEventType::Abort},
        {"tool.user_requested", SessionEventType::ToolUserRequested},
        {"tool.execution_start", SessionEventType::ToolExecutionStart},
        {"tool.execution_partial_result", SessionEventType::ToolExecutionPartialResult},
        {"tool.execution_complete", SessionEventType::ToolExecutionComplete},
        {"tool.execution_progress", SessionEventType::ToolExecutionProgress},
        {"session.compaction_start", SessionEventType::SessionCompactionStart},
        {"session.compaction_complete", SessionEventType::SessionCompactionComplete},
        {"session.usage_info", SessionEventType::SessionUsageInfo},
        {"subagent.started", SessionEventType::CustomAgentStarted},
        {"subagent.completed", SessionEventType::CustomAgentCompleted},
        {"subagent.failed", SessionEventType::CustomAgentFailed},
        {"subagent.selected", SessionEventType::CustomAgentSelected},
        {"custom_agent.started", SessionEventType::CustomAgentStarted},   // legacy alias
        {"custom_agent.completed", SessionEventType::CustomAgentCompleted}, // legacy alias
        {"custom_agent.failed", SessionEventType::CustomAgentFailed},     // legacy alias
        {"custom_agent.selected", SessionEventType::CustomAgentSelected}, // legacy alias
        {"hook.start", SessionEventType::HookStart},
        {"hook.end", SessionEventType::HookEnd},
        {"system.message", SessionEventType::SystemMessage},
        {"session.snapshot_rewind", SessionEventType::SessionSnapshotRewind},
        {"session.shutdown", SessionEventType::SessionShutdown},
        {"skill.invoked", SessionEventType::SkillInvoked},
        // v0.1.49+ additions
        {"session.remote_steerable_changed", SessionEventType::SessionRemoteSteerableChanged},
        {"session.title_changed", SessionEventType::SessionTitleChanged},
        {"session.schedule_created", SessionEventType::SessionScheduleCreated},
        {"session.schedule_cancelled", SessionEventType::SessionScheduleCancelled},
        {"session.warning", SessionEventType::SessionWarning},
        {"session.mode_changed", SessionEventType::SessionModeChanged},
        {"session.plan_changed", SessionEventType::SessionPlanChanged},
        {"session.workspace_file_changed", SessionEventType::SessionWorkspaceFileChanged},
        {"session.context_changed", SessionEventType::SessionContextChanged},
        {"session.task_complete", SessionEventType::SessionTaskComplete},
        {"session.custom_notification", SessionEventType::SessionCustomNotification},
        {"session.tools_updated", SessionEventType::SessionToolsUpdated},
        {"session.background_tasks_changed", SessionEventType::SessionBackgroundTasksChanged},
        {"session.skills_loaded", SessionEventType::SessionSkillsLoaded},
        {"session.custom_agents_updated", SessionEventType::SessionCustomAgentsUpdated},
        {"session.mcp_servers_loaded", SessionEventType::SessionMcpServersLoaded},
        {"session.mcp_server_status_changed", SessionEventType::SessionMcpServerStatusChanged},
        {"session.extensions_loaded", SessionEventType::SessionExtensionsLoaded},
        {"assistant.streaming_delta", SessionEventType::AssistantStreamingDelta},
        {"assistant.message_start", SessionEventType::AssistantMessageStart},
        {"model.call_failure", SessionEventType::ModelCallFailure},
        {"subagent.deselected", SessionEventType::SubagentDeselected},
        {"permission.requested", SessionEventType::PermissionRequested},
        {"permission.completed", SessionEventType::PermissionCompleted},
        {"user_input.requested", SessionEventType::UserInputRequested},
        {"user_input.completed", SessionEventType::UserInputCompleted},
        {"elicitation.requested", SessionEventType::ElicitationRequested},
        {"elicitation.completed", SessionEventType::ElicitationCompleted},
        {"sampling.requested", SessionEventType::SamplingRequested},
        {"sampling.completed", SessionEventType::SamplingCompleted},
        {"mcp.oauth_required", SessionEventType::McpOauthRequired},
        {"mcp.oauth_completed", SessionEventType::McpOauthCompleted},
        {"external_tool.requested", SessionEventType::ExternalToolRequested},
        {"external_tool.completed", SessionEventType::ExternalToolCompleted},
        {"command.queued", SessionEventType::CommandQueued},
        {"command.execute", SessionEventType::CommandExecute},
        {"command.completed", SessionEventType::CommandCompleted},
        {"auto_mode_switch.requested", SessionEventType::AutoModeSwitchRequested},
        {"auto_mode_switch.completed", SessionEventType::AutoModeSwitchCompleted},
        {"commands.changed", SessionEventType::CommandsChanged},
        {"capabilities.changed", SessionEventType::CapabilitiesChanged},
        {"exit_plan_mode.requested", SessionEventType::ExitPlanModeRequested},
        {"exit_plan_mode.completed", SessionEventType::ExitPlanModeCompleted},
        {"system.notification", SessionEventType::SystemNotification},
        {"session.schedule_rearmed", SessionEventType::SessionScheduleRearmed},
        {"session.autopilot_objective_changed", SessionEventType::SessionAutopilotObjectiveChanged},
        {"session.session_limits_changed", SessionEventType::SessionSessionLimitsChanged},
        {"session.permissions_changed", SessionEventType::SessionPermissionsChanged},
        {"session.todos_changed", SessionEventType::SessionTodosChanged},
        {"session.memory_changed", SessionEventType::SessionMemoryChanged},
        {"session.usage_checkpoint", SessionEventType::SessionUsageCheckpoint},
        {"session.context_cleared", SessionEventType::SessionContextCleared},
        {"session.fusion_route_started", SessionEventType::SessionFusionRouteStarted},
        {"session.fusion_route_failed", SessionEventType::SessionFusionRouteFailed},
        {"session.fusion_resolved", SessionEventType::SessionFusionResolved},
        {"session.fusion_handoff", SessionEventType::SessionFusionHandoff},
        {"session.fusion_commit_started", SessionEventType::SessionFusionCommitStarted},
        {"session.fusion_completed", SessionEventType::SessionFusionCompleted},
        {"assistant.turn_retry", SessionEventType::AssistantTurnRetry},
        {"agent.interrupted", SessionEventType::AgentInterrupted},
        {"assistant.fusion_phase_started", SessionEventType::AssistantFusionPhaseStarted},
        {"assistant.fusion_phase_completed", SessionEventType::AssistantFusionPhaseCompleted},
        {"assistant.fusion_phase_failed", SessionEventType::AssistantFusionPhaseFailed},
        {"assistant.server_tool_progress", SessionEventType::AssistantServerToolProgress},
        {"assistant.tool_call_delta", SessionEventType::AssistantToolCallDelta},
        {"assistant.idle", SessionEventType::AssistantIdle},
        {"prompt_cache_break", SessionEventType::PromptCacheBreak},
        {"model.call_finished", SessionEventType::ModelCallFinished},
        {"model.call_start", SessionEventType::ModelCallStart},
        {"tool_search.activated", SessionEventType::ToolSearchActivated},
        {"sandbox.decision", SessionEventType::SandboxDecision},
        {"subagent.configured", SessionEventType::SubagentConfigured},
        {"hook.progress", SessionEventType::HookProgress},
        {"session.binary_asset", SessionEventType::SessionBinaryAsset},
        {"mcp.headers_refresh_required", SessionEventType::McpHeadersRefreshRequired},
        {"mcp.headers_refresh_completed", SessionEventType::McpHeadersRefreshCompleted},
        {"ui.ephemeral_query", SessionEventType::UiEphemeralQuery},
        {"session_limits_exhausted.requested", SessionEventType::SessionLimitsExhaustedRequested},
        {"session_limits_exhausted.completed", SessionEventType::SessionLimitsExhaustedCompleted},
        {"session.auto_mode_resolved", SessionEventType::SessionAutoModeResolved},
        {"session.managed_settings_resolved", SessionEventType::SessionManagedSettingsResolved},
        {"session.managed_settings_enforced", SessionEventType::SessionManagedSettingsEnforced},
        {"factory.run_updated", SessionEventType::FactoryRunUpdated},
        {"factory.run_started", SessionEventType::FactoryRunStarted},
        {"factory.run_settled", SessionEventType::FactoryRunSettled},
        {"mcp.tools.list_changed", SessionEventType::McpToolsListChanged},
        {"mcp.resources.list_changed", SessionEventType::McpResourcesListChanged},
        {"mcp.prompts.list_changed", SessionEventType::McpPromptsListChanged},
        {"session.canvas.opened", SessionEventType::SessionCanvasOpened},
        {"session.canvas.registry_changed", SessionEventType::SessionCanvasRegistryChanged},
        {"session.canvas.closed", SessionEventType::SessionCanvasClosed},
        {"session.canvas.unavailable", SessionEventType::SessionCanvasUnavailable},
        {"session.canvas.recorded", SessionEventType::SessionCanvasRecorded},
        {"session.canvas.removed", SessionEventType::SessionCanvasRemoved},
        {"session.extensions.attachments_pushed", SessionEventType::SessionExtensionsAttachmentsPushed},
        {"mcp_app.tool_call_complete", SessionEventType::McpAppToolCallComplete},
        // @github/copilot 1.0.84-5 (upstream f45c46fd). Wire spellings are taken
        // verbatim from the generated kSessionEventWireTypes -- note the mixed
        // conventions upstream uses under permission.*, which are not typos here.
        {"session.auto_tier_recommendation", SessionEventType::SessionAutoTierRecommendation},
        {"session.auto_tier_switch_failed", SessionEventType::SessionAutoTierSwitchFailed},
        {"session.mode_notice_delivered", SessionEventType::SessionModeNoticeDelivered},
        {"session.completion_receipt", SessionEventType::SessionCompletionReceipt},
        {"assistant.fusion_phase_activity", SessionEventType::AssistantFusionPhaseActivity},
        {"permission.carriedForward", SessionEventType::PermissionCarriedForward},
        {"permission.messageAuthorization", SessionEventType::PermissionMessageAuthorization},
        {"permission.messageAuthorizationRead", SessionEventType::PermissionMessageAuthorizationRead},
        {"permission.messageAuthorizationDegraded",
         SessionEventType::PermissionMessageAuthorizationDegraded},
        {"session.mcp_server_removed", SessionEventType::SessionMcpServerRemoved},
        {"session.mcp_server_needs_reconnect", SessionEventType::SessionMcpServerNeedsReconnect},
    };

    auto it = type_map.find(event.type_string);
    if (it != type_map.end())
    {
        event.type = it->second;

        // Parse data based on type
        try
        {
        switch (event.type)
        {
        case SessionEventType::SessionStart:
            event.data = data_json.get<SessionStartData>();
            break;
        case SessionEventType::SessionResume:
            event.data = data_json.get<SessionResumeData>();
            break;
        case SessionEventType::SessionError:
            event.data = data_json.get<SessionErrorData>();
            break;
        case SessionEventType::SessionIdle:
            event.data = data_json.get<SessionIdleData>();
            break;
        case SessionEventType::SessionInfo:
            event.data = data_json.get<SessionInfoData>();
            break;
        case SessionEventType::SessionModelChange:
            event.data = data_json.get<SessionModelChangeData>();
            break;
        case SessionEventType::SessionHandoff:
            event.data = data_json.get<SessionHandoffData>();
            break;
        case SessionEventType::SessionTruncation:
            event.data = data_json.get<SessionTruncationData>();
            break;
        case SessionEventType::UserMessage:
            event.data = data_json.get<UserMessageData>();
            break;
        case SessionEventType::PendingMessagesModified:
            event.data = data_json.get<PendingMessagesModifiedData>();
            break;
        case SessionEventType::AssistantTurnStart:
            event.data = data_json.get<AssistantTurnStartData>();
            break;
        case SessionEventType::AssistantIntent:
            event.data = data_json.get<AssistantIntentData>();
            break;
        case SessionEventType::AssistantReasoning:
            event.data = data_json.get<AssistantReasoningData>();
            break;
        case SessionEventType::AssistantReasoningDelta:
            event.data = data_json.get<AssistantReasoningDeltaData>();
            break;
        case SessionEventType::AssistantMessage:
            event.data = data_json.get<AssistantMessageData>();
            break;
        case SessionEventType::AssistantMessageDelta:
            event.data = data_json.get<AssistantMessageDeltaData>();
            break;
        case SessionEventType::AssistantTurnEnd:
            event.data = data_json.get<AssistantTurnEndData>();
            break;
        case SessionEventType::AssistantUsage:
            event.data = data_json.get<AssistantUsageData>();
            break;
        case SessionEventType::Abort:
            event.data = data_json.get<AbortData>();
            break;
        case SessionEventType::ToolUserRequested:
            event.data = data_json.get<ToolUserRequestedData>();
            break;
        case SessionEventType::ToolExecutionStart:
            event.data = data_json.get<ToolExecutionStartData>();
            break;
        case SessionEventType::ToolExecutionPartialResult:
            event.data = data_json.get<ToolExecutionPartialResultData>();
            break;
        case SessionEventType::ToolExecutionComplete:
            event.data = data_json.get<ToolExecutionCompleteData>();
            break;
        case SessionEventType::ToolExecutionProgress:
            event.data = data_json.get<ToolExecutionProgressData>();
            break;
        case SessionEventType::SessionCompactionStart:
            event.data = data_json.get<SessionCompactionStartData>();
            break;
        case SessionEventType::SessionCompactionComplete:
            event.data = data_json.get<SessionCompactionCompleteData>();
            break;
        case SessionEventType::SessionUsageInfo:
            event.data = data_json.get<SessionUsageInfoData>();
            break;
        case SessionEventType::CustomAgentStarted:
            event.data = data_json.get<CustomAgentStartedData>();
            break;
        case SessionEventType::CustomAgentCompleted:
            event.data = data_json.get<CustomAgentCompletedData>();
            break;
        case SessionEventType::CustomAgentFailed:
            event.data = data_json.get<CustomAgentFailedData>();
            break;
        case SessionEventType::CustomAgentSelected:
            event.data = data_json.get<CustomAgentSelectedData>();
            break;
        case SessionEventType::HookStart:
            event.data = data_json.get<HookStartData>();
            break;
        case SessionEventType::HookEnd:
            event.data = data_json.get<HookEndData>();
            break;
        case SessionEventType::SystemMessage:
            event.data = data_json.get<SystemMessageData>();
            break;
        case SessionEventType::SessionSnapshotRewind:
            event.data = data_json.get<SessionSnapshotRewindData>();
            break;
        case SessionEventType::SessionShutdown:
            event.data = data_json.get<SessionShutdownData>();
            break;
        case SessionEventType::SkillInvoked:
            event.data = data_json.get<SkillInvokedData>();
            break;
        // v0.1.49+ additions
        case SessionEventType::SessionRemoteSteerableChanged:
            event.data = data_json.get<SessionRemoteSteerableChangedData>();
            break;
        case SessionEventType::SessionTitleChanged:
            event.data = data_json.get<SessionTitleChangedData>();
            break;
        case SessionEventType::SessionScheduleCreated:
            event.data = data_json.get<SessionScheduleCreatedData>();
            break;
        case SessionEventType::SessionScheduleCancelled:
            event.data = data_json.get<SessionScheduleCancelledData>();
            break;
        case SessionEventType::SessionWarning:
            event.data = data_json.get<SessionWarningData>();
            break;
        case SessionEventType::SessionModeChanged:
            event.data = data_json.get<SessionModeChangedData>();
            break;
        case SessionEventType::SessionPlanChanged:
            event.data = data_json.get<SessionPlanChangedData>();
            break;
        case SessionEventType::SessionWorkspaceFileChanged:
            event.data = data_json.get<SessionWorkspaceFileChangedData>();
            break;
        case SessionEventType::SessionContextChanged:
            event.data = data_json.get<SessionContextChangedData>();
            break;
        case SessionEventType::SessionTaskComplete:
            event.data = data_json.get<SessionTaskCompleteData>();
            break;
        case SessionEventType::SessionCustomNotification:
            event.data = data_json.get<SessionCustomNotificationData>();
            break;
        case SessionEventType::SessionToolsUpdated:
            event.data = data_json.get<SessionToolsUpdatedData>();
            break;
        case SessionEventType::SessionBackgroundTasksChanged:
            event.data = data_json.get<SessionBackgroundTasksChangedData>();
            break;
        case SessionEventType::SessionSkillsLoaded:
            event.data = data_json.get<SessionSkillsLoadedData>();
            break;
        case SessionEventType::SessionCustomAgentsUpdated:
            event.data = data_json.get<SessionCustomAgentsUpdatedData>();
            break;
        case SessionEventType::SessionMcpServersLoaded:
            event.data = data_json.get<SessionMcpServersLoadedData>();
            break;
        case SessionEventType::SessionMcpServerStatusChanged:
            event.data = data_json.get<SessionMcpServerStatusChangedData>();
            break;
        case SessionEventType::SessionExtensionsLoaded:
            event.data = data_json.get<SessionExtensionsLoadedData>();
            break;
        case SessionEventType::AssistantStreamingDelta:
            event.data = data_json.get<AssistantStreamingDeltaData>();
            break;
        case SessionEventType::AssistantMessageStart:
            event.data = data_json.get<AssistantMessageStartData>();
            break;
        case SessionEventType::ModelCallFailure:
            event.data = data_json.get<ModelCallFailureData>();
            break;
        case SessionEventType::SubagentDeselected:
            event.data = data_json.get<SubagentDeselectedData>();
            break;
        case SessionEventType::PermissionRequested:
            event.data = data_json.get<PermissionRequestedData>();
            break;
        case SessionEventType::PermissionCompleted:
            event.data = data_json.get<PermissionCompletedData>();
            break;
        case SessionEventType::UserInputRequested:
            event.data = data_json.get<UserInputRequestedData>();
            break;
        case SessionEventType::UserInputCompleted:
            event.data = data_json.get<UserInputCompletedData>();
            break;
        case SessionEventType::ElicitationRequested:
            event.data = data_json.get<ElicitationRequestedData>();
            break;
        case SessionEventType::ElicitationCompleted:
            event.data = data_json.get<ElicitationCompletedData>();
            break;
        case SessionEventType::SamplingRequested:
            event.data = data_json.get<SamplingRequestedData>();
            break;
        case SessionEventType::SamplingCompleted:
            event.data = data_json.get<SamplingCompletedData>();
            break;
        case SessionEventType::McpOauthRequired:
            event.data = data_json.get<McpOauthRequiredData>();
            break;
        case SessionEventType::McpOauthCompleted:
            event.data = data_json.get<McpOauthCompletedData>();
            break;
        case SessionEventType::ExternalToolRequested:
            event.data = data_json.get<ExternalToolRequestedData>();
            break;
        case SessionEventType::ExternalToolCompleted:
            event.data = data_json.get<ExternalToolCompletedData>();
            break;
        case SessionEventType::CommandQueued:
            event.data = data_json.get<CommandQueuedData>();
            break;
        case SessionEventType::CommandExecute:
            event.data = data_json.get<CommandExecuteData>();
            break;
        case SessionEventType::CommandCompleted:
            event.data = data_json.get<CommandCompletedData>();
            break;
        case SessionEventType::AutoModeSwitchRequested:
            event.data = data_json.get<AutoModeSwitchRequestedData>();
            break;
        case SessionEventType::AutoModeSwitchCompleted:
            event.data = data_json.get<AutoModeSwitchCompletedData>();
            break;
        case SessionEventType::CommandsChanged:
            event.data = data_json.get<CommandsChangedData>();
            break;
        case SessionEventType::CapabilitiesChanged:
            event.data = data_json.get<CapabilitiesChangedData>();
            break;
        case SessionEventType::ExitPlanModeRequested:
            event.data = data_json.get<ExitPlanModeRequestedData>();
            break;
        case SessionEventType::ExitPlanModeCompleted:
            event.data = data_json.get<ExitPlanModeCompletedData>();
            break;
        case SessionEventType::SystemNotification:
            event.data = data_json.get<SystemNotificationData>();
            break;
        case SessionEventType::SessionScheduleRearmed:
            event.data = data_json.get<ScheduleRearmedData>();
            break;
        case SessionEventType::SessionAutopilotObjectiveChanged:
            event.data = data_json.get<AutopilotObjectiveChangedData>();
            break;
        case SessionEventType::SessionSessionLimitsChanged:
            event.data = data_json.get<SessionLimitsChangedData>();
            break;
        case SessionEventType::SessionPermissionsChanged:
            event.data = data_json.get<PermissionsChangedData>();
            break;
        case SessionEventType::SessionTodosChanged:
            event.data = data_json.get<TodosChangedData>();
            break;
        case SessionEventType::SessionMemoryChanged:
            event.data = data_json.get<MemoryChangedData>();
            break;
        case SessionEventType::SessionUsageCheckpoint:
            event.data = data_json.get<UsageCheckpointData>();
            break;
        case SessionEventType::SessionContextCleared:
            event.data = data_json.get<ContextClearedData>();
            break;
        case SessionEventType::SessionFusionRouteStarted:
            event.data = data_json.get<FusionRouteStartedData>();
            break;
        case SessionEventType::SessionFusionRouteFailed:
            event.data = data_json.get<FusionRouteFailedData>();
            break;
        case SessionEventType::SessionFusionResolved:
            event.data = data_json.get<FusionResolvedData>();
            break;
        case SessionEventType::SessionFusionHandoff:
            event.data = data_json.get<FusionHandoffData>();
            break;
        case SessionEventType::SessionFusionCommitStarted:
            event.data = data_json.get<FusionCommitStartedData>();
            break;
        case SessionEventType::SessionFusionCompleted:
            event.data = data_json.get<FusionCompletedData>();
            break;
        case SessionEventType::AssistantTurnRetry:
            event.data = data_json.get<AssistantTurnRetryData>();
            break;
        case SessionEventType::AgentInterrupted:
            event.data = data_json.get<AgentInterruptedData>();
            break;
        case SessionEventType::AssistantFusionPhaseStarted:
            event.data = data_json.get<FusionPhaseStartedData>();
            break;
        case SessionEventType::AssistantFusionPhaseCompleted:
            event.data = data_json.get<FusionPhaseCompletedData>();
            break;
        case SessionEventType::AssistantFusionPhaseFailed:
            event.data = data_json.get<FusionPhaseFailedData>();
            break;
        case SessionEventType::AssistantServerToolProgress:
            event.data = data_json.get<AssistantServerToolProgressData>();
            break;
        case SessionEventType::AssistantToolCallDelta:
            event.data = data_json.get<AssistantToolCallDeltaData>();
            break;
        case SessionEventType::AssistantIdle:
            event.data = data_json.get<AssistantIdleData>();
            break;
        case SessionEventType::PromptCacheBreak:
            event.data = data_json.get<PromptCacheBreakData>();
            break;
        case SessionEventType::ModelCallFinished:
            event.data = data_json.get<ModelCallFinishedData>();
            break;
        case SessionEventType::ModelCallStart:
            event.data = data_json.get<ModelCallStartData>();
            break;
        case SessionEventType::ToolSearchActivated:
            event.data = data_json.get<ToolSearchActivatedData>();
            break;
        case SessionEventType::SandboxDecision:
            event.data = data_json.get<SandboxDecisionData>();
            break;
        case SessionEventType::SubagentConfigured:
            event.data = data_json.get<SubagentConfiguredData>();
            break;
        case SessionEventType::HookProgress:
            event.data = data_json.get<HookProgressData>();
            break;
        case SessionEventType::SessionBinaryAsset:
            event.data = data_json.get<BinaryAssetData>();
            break;
        case SessionEventType::McpHeadersRefreshRequired:
            event.data = data_json.get<McpHeadersRefreshRequiredData>();
            break;
        case SessionEventType::McpHeadersRefreshCompleted:
            event.data = data_json.get<McpHeadersRefreshCompletedData>();
            break;
        case SessionEventType::UiEphemeralQuery:
            event.data = data_json.get<UIEphemeralQueryData>();
            break;
        case SessionEventType::SessionLimitsExhaustedRequested:
            event.data = data_json.get<SessionLimitsExhaustedRequestedData>();
            break;
        case SessionEventType::SessionLimitsExhaustedCompleted:
            event.data = data_json.get<SessionLimitsExhaustedCompletedData>();
            break;
        case SessionEventType::SessionAutoModeResolved:
            event.data = data_json.get<AutoModeResolvedData>();
            break;
        case SessionEventType::SessionManagedSettingsResolved:
            event.data = data_json.get<ManagedSettingsResolvedData>();
            break;
        case SessionEventType::SessionManagedSettingsEnforced:
            event.data = data_json.get<ManagedSettingsEnforcedData>();
            break;
        case SessionEventType::FactoryRunUpdated:
            event.data = data_json.get<FactoryRunUpdatedData>();
            break;
        case SessionEventType::FactoryRunStarted:
            event.data = data_json.get<FactoryRunStartedData>();
            break;
        case SessionEventType::FactoryRunSettled:
            event.data = data_json.get<FactoryRunSettledData>();
            break;
        case SessionEventType::McpToolsListChanged:
            event.data = data_json.get<McpListChangedData>();
            break;
        case SessionEventType::McpResourcesListChanged:
            event.data = data_json.get<McpListChangedData>();
            break;
        case SessionEventType::McpPromptsListChanged:
            event.data = data_json.get<McpListChangedData>();
            break;
        case SessionEventType::SessionCanvasOpened:
            event.data = data_json.get<CanvasOpenedData>();
            break;
        case SessionEventType::SessionCanvasRegistryChanged:
            event.data = data_json.get<CanvasRegistryChangedData>();
            break;
        case SessionEventType::SessionCanvasClosed:
            event.data = data_json.get<CanvasClosedData>();
            break;
        case SessionEventType::SessionCanvasUnavailable:
            event.data = data_json.get<CanvasUnavailableData>();
            break;
        case SessionEventType::SessionCanvasRecorded:
            event.data = data_json.get<CanvasRecordedData>();
            break;
        case SessionEventType::SessionCanvasRemoved:
            event.data = data_json.get<CanvasRemovedData>();
            break;
        case SessionEventType::SessionExtensionsAttachmentsPushed:
            event.data = data_json.get<ExtensionsAttachmentsPushedData>();
            break;
        case SessionEventType::McpAppToolCallComplete:
            event.data = data_json.get<McpAppToolCallCompleteData>();
            break;
        default:
            event.data = data_json; // Fallback to raw JSON
            break;
        }
        }
        catch (const json::exception&)
        {
            // Preserve forward compatibility when a known event's payload shape
            // evolves or a formerly-required field is null.
            event.data = data_json;
        }
    }
    else
    {
        // Unknown event type - store raw JSON
        event.type = SessionEventType::Unknown;
        event.data = data_json;
    }

    return event;
}

/// ADL hook for json
inline void from_json(const json& j, SessionEvent& event)
{
    event = parse_session_event(j);
}

} // namespace copilot
