// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace copilot
{

// =============================================================================
// Type Aliases
// =============================================================================

/// JSON type alias for cleaner API
using json = nlohmann::json;

// Forward declarations
class Session;
struct SessionEvent;
using EventHandler = std::function<void(const SessionEvent&)>;

// =============================================================================
// Protocol Version
// =============================================================================

/// Maximum SDK protocol version supported (matches copilot-agent-runtime server).
/// Upstream nodejs SDK_PROTOCOL_VERSION = 3 since v0.1.24-series.
inline constexpr int kSdkProtocolVersion = 3;

/// Minimum SDK protocol version this SDK can communicate with.
/// Older servers (reporting < kMinProtocolVersion) are rejected.
inline constexpr int kMinProtocolVersion = 2;

// =============================================================================
// Enums
// =============================================================================

/// Connection state of the client
enum class ConnectionState
{
    Disconnected,
    Connecting,
    Connected,
    Error
};

/// System message mode for session configuration
enum class SystemMessageMode
{
    Append,
    Replace,
    Customize
};

/// Section override action for system message customization
enum class SectionOverrideAction
{
    Replace,
    Remove,
    Append,
    Prepend,
    Transform
};

/// OAuth grant type for an MCP HTTP server
enum class McpHttpServerConfigOauthGrantType
{
    AuthorizationCode,
    ClientCredentials
};

// JSON enum serialization
NLOHMANN_JSON_SERIALIZE_ENUM(
    ConnectionState,
    {
        {ConnectionState::Disconnected, "disconnected"},
        {ConnectionState::Connecting, "connecting"},
        {ConnectionState::Connected, "connected"},
        {ConnectionState::Error, "error"},
    }
)

NLOHMANN_JSON_SERIALIZE_ENUM(
    SystemMessageMode,
    {
        {SystemMessageMode::Append, "append"},
        {SystemMessageMode::Replace, "replace"},
        {SystemMessageMode::Customize, "customize"},
    }
)

NLOHMANN_JSON_SERIALIZE_ENUM(
    SectionOverrideAction,
    {
        {SectionOverrideAction::Replace, "replace"},
        {SectionOverrideAction::Remove, "remove"},
        {SectionOverrideAction::Append, "append"},
        {SectionOverrideAction::Prepend, "prepend"},
        {SectionOverrideAction::Transform, "transform"},
    }
)

NLOHMANN_JSON_SERIALIZE_ENUM(
    McpHttpServerConfigOauthGrantType,
    {
        {McpHttpServerConfigOauthGrantType::AuthorizationCode, "authorization_code"},
        {McpHttpServerConfigOauthGrantType::ClientCredentials, "client_credentials"},
    }
)

/// Log level for the CLI
enum class LogLevel
{
    None,
    Error,
    Warning,
    Info,
    Debug,
    All
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    LogLevel,
    {
        {LogLevel::None, "none"},
        {LogLevel::Error, "error"},
        {LogLevel::Warning, "warning"},
        {LogLevel::Info, "info"},
        {LogLevel::Debug, "debug"},
        {LogLevel::All, "all"},
    }
)

/// Result type for tool execution
enum class ToolResultType
{
    Success,
    Failure,
    Rejected,
    Denied,
    Timeout, ///< Added upstream in v0.1.49 series.
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ToolResultType,
    {
        {ToolResultType::Success, "success"},
        {ToolResultType::Failure, "failure"},
        {ToolResultType::Rejected, "rejected"},
        {ToolResultType::Denied, "denied"},
        {ToolResultType::Timeout, "timeout"},
    }
)

/// Reasoning effort level for model inference
enum class ReasoningEffort
{
    Low,
    Medium,
    High,
    XHigh
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ReasoningEffort,
    {
        {ReasoningEffort::Low, "low"},
        {ReasoningEffort::Medium, "medium"},
        {ReasoningEffort::High, "high"},
        {ReasoningEffort::XHigh, "xhigh"},
    }
)

// =============================================================================
// Tool Types
// =============================================================================

/// Binary result from a tool execution
struct ToolBinaryResult
{
    std::string data;
    std::string mime_type;
    std::string type;
    std::optional<std::string> description;
};

inline void to_json(json& j, const ToolBinaryResult& r)
{
    j = json{{"data", r.data}, {"mimeType", r.mime_type}, {"type", r.type}};
    if (r.description)
        j["description"] = *r.description;
}

inline void from_json(const json& j, ToolBinaryResult& r)
{
    j.at("data").get_to(r.data);
    j.at("mimeType").get_to(r.mime_type);
    j.at("type").get_to(r.type);
    if (j.contains("description"))
        r.description = j.at("description").get<std::string>();
}

/// Result object returned from tool execution
struct ToolResultObject
{
    std::string text_result_for_llm;
    std::optional<std::vector<ToolBinaryResult>> binary_results_for_llm;
    ToolResultType result_type = ToolResultType::Success;
    std::optional<std::string> error;
    std::optional<std::string> session_log;
    std::optional<std::map<std::string, json>> tool_telemetry;
};

inline void to_json(json& j, const ToolResultObject& r)
{
    j = json{{"textResultForLlm", r.text_result_for_llm}, {"resultType", r.result_type}};
    if (r.binary_results_for_llm)
        j["binaryResultsForLlm"] = *r.binary_results_for_llm;
    if (r.error)
        j["error"] = *r.error;
    if (r.session_log)
        j["sessionLog"] = *r.session_log;
    if (r.tool_telemetry)
        j["toolTelemetry"] = *r.tool_telemetry;
}

inline void from_json(const json& j, ToolResultObject& r)
{
    j.at("textResultForLlm").get_to(r.text_result_for_llm);
    if (j.contains("resultType"))
        j.at("resultType").get_to(r.result_type);
    if (j.contains("binaryResultsForLlm"))
        r.binary_results_for_llm = j.at("binaryResultsForLlm").get<std::vector<ToolBinaryResult>>();
    if (j.contains("error"))
        r.error = j.at("error").get<std::string>();
    if (j.contains("sessionLog"))
        r.session_log = j.at("sessionLog").get<std::string>();
    if (j.contains("toolTelemetry"))
        r.tool_telemetry = j.at("toolTelemetry").get<std::map<std::string, json>>();
}

/// Information about a tool invocation from the server
struct ToolInvocation
{
    std::string session_id;
    std::string tool_call_id;
    std::string tool_name;
    std::optional<json> arguments;
};

/// Tool handler function type
using ToolHandler = std::function<ToolResultObject(const ToolInvocation&)>;

// =============================================================================
// Permission Types
// =============================================================================

/// Permission request from the server
struct PermissionRequest
{
    std::string kind;
    std::optional<std::string> tool_call_id;
    std::map<std::string, json> extension_data;
};

inline void to_json(json& j, const PermissionRequest& r)
{
    j = json{{"kind", r.kind}};
    if (r.tool_call_id)
        j["toolCallId"] = *r.tool_call_id;
    for (const auto& [k, v] : r.extension_data)
        j[k] = v;
}

inline void from_json(const json& j, PermissionRequest& r)
{
    j.at("kind").get_to(r.kind);
    if (j.contains("toolCallId"))
        r.tool_call_id = j.at("toolCallId").get<std::string>();
    // Collect extension data (all fields except kind and toolCallId)
    for (auto& [k, v] : j.items())
        if (k != "kind" && k != "toolCallId")
            r.extension_data[k] = v;
}

/// Result of a permission request (response to CLI)
struct PermissionRequestResult
{
    std::string kind; // e.g., "approved", "denied-no-approval-rule-and-could-not-request-from-user"
    std::optional<std::vector<json>> rules;
};

inline void to_json(json& j, const PermissionRequestResult& r)
{
    j = json{{"kind", r.kind}};
    if (r.rules)
        j["rules"] = *r.rules;
}

inline void from_json(const json& j, PermissionRequestResult& r)
{
    j.at("kind").get_to(r.kind);
    if (j.contains("rules"))
        r.rules = j.at("rules").get<std::vector<json>>();
}

/// Context for permission invocation
struct PermissionInvocation
{
    std::string session_id;
};

/// Permission handler function type
using PermissionHandler = std::function<PermissionRequestResult(const PermissionRequest& request)>;

// =============================================================================
// User Input Types
// =============================================================================

/// Request for user input from the agent (ask_user tool)
struct UserInputRequest
{
    std::string question;
    std::optional<std::vector<std::string>> choices;
    std::optional<bool> allow_freeform;
};

inline void from_json(const json& j, UserInputRequest& r)
{
    j.at("question").get_to(r.question);
    if (j.contains("choices") && !j["choices"].is_null())
        r.choices = j.at("choices").get<std::vector<std::string>>();
    if (j.contains("allowFreeform") && !j["allowFreeform"].is_null())
        r.allow_freeform = j.at("allowFreeform").get<bool>();
}

inline void to_json(json& j, const UserInputRequest& r)
{
    j = json{{"question", r.question}};
    if (r.choices)
        j["choices"] = *r.choices;
    if (r.allow_freeform)
        j["allowFreeform"] = *r.allow_freeform;
}

/// Response to a user input request
struct UserInputResponse
{
    std::string answer;
    bool was_freeform = false;
};

inline void from_json(const json& j, UserInputResponse& r)
{
    j.at("answer").get_to(r.answer);
    if (j.contains("wasFreeform"))
        j.at("wasFreeform").get_to(r.was_freeform);
}

inline void to_json(json& j, const UserInputResponse& r)
{
    j = json{{"answer", r.answer}, {"wasFreeform", r.was_freeform}};
}

/// Context for a user input request invocation
struct UserInputInvocation
{
    std::string session_id;
};

/// Handler for user input requests from the agent
using UserInputHandler = std::function<UserInputResponse(const UserInputRequest&, const UserInputInvocation&)>;

// =============================================================================
// Elicitation Types
// =============================================================================

/// Elicitation display mode
enum class ElicitationRequestedMode
{
    Form,
    Url
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ElicitationRequestedMode,
    {
        {ElicitationRequestedMode::Form, "form"},
        {ElicitationRequestedMode::Url, "url"},
    }
)

/// JSON Schema for elicitation form fields
struct ElicitationSchema
{
    std::string type = "object";
    std::optional<std::map<std::string, json>> properties;
    std::optional<std::vector<std::string>> required;
};

inline void to_json(json& j, const ElicitationSchema& s)
{
    j = json{{"type", s.type}};
    if (s.properties)
        j["properties"] = *s.properties;
    if (s.required)
        j["required"] = *s.required;
}

inline void from_json(const json& j, ElicitationSchema& s)
{
    if (j.contains("type"))
        j.at("type").get_to(s.type);
    if (j.contains("properties"))
        s.properties = j.at("properties").get<std::map<std::string, json>>();
    if (j.contains("required"))
        s.required = j.at("required").get<std::vector<std::string>>();
}

/// User action for elicitation response
enum class ElicitationAction
{
    Accept,
    Decline,
    Cancel
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ElicitationAction,
    {
        {ElicitationAction::Accept, "accept"},
        {ElicitationAction::Decline, "decline"},
        {ElicitationAction::Cancel, "cancel"},
    }
)

/// Context for an elicitation request from the server
struct ElicitationContext
{
    std::string session_id;
    std::string message;
    std::optional<ElicitationSchema> requested_schema;
    std::optional<ElicitationRequestedMode> mode;
    std::optional<std::string> elicitation_source;
    std::optional<std::string> url;
};

inline void from_json(const json& j, ElicitationContext& c)
{
    if (j.contains("sessionId"))
        j.at("sessionId").get_to(c.session_id);
    j.at("message").get_to(c.message);
    if (j.contains("requestedSchema") && !j["requestedSchema"].is_null())
        c.requested_schema = j.at("requestedSchema").get<ElicitationSchema>();
    if (j.contains("mode") && !j["mode"].is_null())
        c.mode = j.at("mode").get<ElicitationRequestedMode>();
    if (j.contains("elicitationSource") && !j["elicitationSource"].is_null())
        c.elicitation_source = j.at("elicitationSource").get<std::string>();
    if (j.contains("url") && !j["url"].is_null())
        c.url = j.at("url").get<std::string>();
}

inline void to_json(json& j, const ElicitationContext& c)
{
    j = json{{"message", c.message}};
    if (!c.session_id.empty())
        j["sessionId"] = c.session_id;
    if (c.requested_schema)
        j["requestedSchema"] = *c.requested_schema;
    if (c.mode)
        j["mode"] = *c.mode;
    if (c.elicitation_source)
        j["elicitationSource"] = *c.elicitation_source;
    if (c.url)
        j["url"] = *c.url;
}

/// Result returned from an elicitation dialog
struct ElicitationResult
{
    ElicitationAction action = ElicitationAction::Cancel;
    std::optional<std::map<std::string, json>> content;
};

inline void to_json(json& j, const ElicitationResult& r)
{
    j = json{{"action", r.action}};
    if (r.content)
        j["content"] = *r.content;
}

inline void from_json(const json& j, ElicitationResult& r)
{
    j.at("action").get_to(r.action);
    if (j.contains("content") && !j["content"].is_null())
        r.content = j.at("content").get<std::map<std::string, json>>();
}

/// Elicitation handler function type
using ElicitationHandler = std::function<ElicitationResult(const ElicitationContext&)>;

// =============================================================================
// Exit Plan Mode Types
// =============================================================================

/// Request to exit plan mode
struct ExitPlanModeRequest
{
    std::string summary;
    std::optional<std::string> plan_content;
    std::vector<std::string> actions;
    std::string recommended_action = "autopilot";
};

inline void from_json(const json& j, ExitPlanModeRequest& r)
{
    j.at("summary").get_to(r.summary);
    if (j.contains("planContent") && !j["planContent"].is_null())
        r.plan_content = j.at("planContent").get<std::string>();
    if (j.contains("actions"))
        r.actions = j.at("actions").get<std::vector<std::string>>();
    if (j.contains("recommendedAction"))
        j.at("recommendedAction").get_to(r.recommended_action);
}

inline void to_json(json& j, const ExitPlanModeRequest& r)
{
    j = json{{"summary", r.summary}, {"recommendedAction", r.recommended_action}};
    if (r.plan_content)
        j["planContent"] = *r.plan_content;
    if (!r.actions.empty())
        j["actions"] = r.actions;
}

/// Response to an exit-plan-mode request
struct ExitPlanModeResult
{
    bool approved = true;
    std::optional<std::string> selected_action;
    std::optional<std::string> feedback;
};

inline void to_json(json& j, const ExitPlanModeResult& r)
{
    j = json{{"approved", r.approved}};
    if (r.selected_action)
        j["selectedAction"] = *r.selected_action;
    if (r.feedback)
        j["feedback"] = *r.feedback;
}

inline void from_json(const json& j, ExitPlanModeResult& r)
{
    j.at("approved").get_to(r.approved);
    if (j.contains("selectedAction") && !j["selectedAction"].is_null())
        r.selected_action = j.at("selectedAction").get<std::string>();
    if (j.contains("feedback") && !j["feedback"].is_null())
        r.feedback = j.at("feedback").get<std::string>();
}

/// Context for exit-plan-mode invocation
struct ExitPlanModeInvocation
{
    std::string session_id;
};

/// Exit plan mode handler function type
using ExitPlanModeHandler =
    std::function<ExitPlanModeResult(const ExitPlanModeRequest&, const ExitPlanModeInvocation&)>;

// =============================================================================
// Auto Mode Switch Types
// =============================================================================

/// Request to switch to auto mode after a rate limit
struct AutoModeSwitchRequest
{
    std::optional<std::string> error_code;
    std::optional<double> retry_after_seconds;
};

inline void from_json(const json& j, AutoModeSwitchRequest& r)
{
    if (j.contains("errorCode") && !j["errorCode"].is_null())
        r.error_code = j.at("errorCode").get<std::string>();
    if (j.contains("retryAfterSeconds") && !j["retryAfterSeconds"].is_null())
        r.retry_after_seconds = j.at("retryAfterSeconds").get<double>();
}

inline void to_json(json& j, const AutoModeSwitchRequest& r)
{
    j = json::object();
    if (r.error_code)
        j["errorCode"] = *r.error_code;
    if (r.retry_after_seconds)
        j["retryAfterSeconds"] = *r.retry_after_seconds;
}

/// Response to auto-mode-switch request
enum class AutoModeSwitchResponse
{
    Yes,
    YesAlways,
    No
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    AutoModeSwitchResponse,
    {
        {AutoModeSwitchResponse::Yes, "yes"},
        {AutoModeSwitchResponse::YesAlways, "yes_always"},
        {AutoModeSwitchResponse::No, "no"},
    }
)

/// Context for auto-mode-switch invocation
struct AutoModeSwitchInvocation
{
    std::string session_id;
};

/// Auto mode switch handler function type
using AutoModeSwitchHandler =
    std::function<AutoModeSwitchResponse(const AutoModeSwitchRequest&, const AutoModeSwitchInvocation&)>;

// =============================================================================
// Hook Handler Types
// =============================================================================

/// Context for a hook invocation
struct HookInvocation
{
    std::string session_id;
};

/// Input for a pre-tool-use hook
struct PreToolUseHookInput
{
    int64_t timestamp = 0;
    std::string cwd;
    std::string tool_name;
    std::optional<json> tool_args;
};

inline void from_json(const json& j, PreToolUseHookInput& h)
{
    if (j.contains("timestamp")) j.at("timestamp").get_to(h.timestamp);
    if (j.contains("cwd")) j.at("cwd").get_to(h.cwd);
    if (j.contains("toolName")) j.at("toolName").get_to(h.tool_name);
    if (j.contains("toolArgs") && !j["toolArgs"].is_null()) h.tool_args = j["toolArgs"];
}

/// Output for a pre-tool-use hook
struct PreToolUseHookOutput
{
    std::optional<std::string> permission_decision;     ///< "allow", "deny", or "ask"
    std::optional<std::string> permission_decision_reason;
    std::optional<json> modified_args;
    std::optional<std::string> additional_context;
    std::optional<bool> suppress_output;
};

inline void to_json(json& j, const PreToolUseHookOutput& h)
{
    j = json::object();
    if (h.permission_decision) j["permissionDecision"] = *h.permission_decision;
    if (h.permission_decision_reason) j["permissionDecisionReason"] = *h.permission_decision_reason;
    if (h.modified_args) j["modifiedArgs"] = *h.modified_args;
    if (h.additional_context) j["additionalContext"] = *h.additional_context;
    if (h.suppress_output) j["suppressOutput"] = *h.suppress_output;
}

using PreToolUseHandler = std::function<std::optional<PreToolUseHookOutput>(const PreToolUseHookInput&, const HookInvocation&)>;

/// Input for a post-tool-use hook
struct PostToolUseHookInput
{
    int64_t timestamp = 0;
    std::string cwd;
    std::string tool_name;
    std::optional<json> tool_args;
    std::optional<json> tool_result;
};

inline void from_json(const json& j, PostToolUseHookInput& h)
{
    if (j.contains("timestamp")) j.at("timestamp").get_to(h.timestamp);
    if (j.contains("cwd")) j.at("cwd").get_to(h.cwd);
    if (j.contains("toolName")) j.at("toolName").get_to(h.tool_name);
    if (j.contains("toolArgs") && !j["toolArgs"].is_null()) h.tool_args = j["toolArgs"];
    if (j.contains("toolResult") && !j["toolResult"].is_null()) h.tool_result = j["toolResult"];
}

/// Output for a post-tool-use hook
struct PostToolUseHookOutput
{
    std::optional<json> modified_result;
    std::optional<std::string> additional_context;
    std::optional<bool> suppress_output;
};

inline void to_json(json& j, const PostToolUseHookOutput& h)
{
    j = json::object();
    if (h.modified_result) j["modifiedResult"] = *h.modified_result;
    if (h.additional_context) j["additionalContext"] = *h.additional_context;
    if (h.suppress_output) j["suppressOutput"] = *h.suppress_output;
}

using PostToolUseHandler = std::function<std::optional<PostToolUseHookOutput>(const PostToolUseHookInput&, const HookInvocation&)>;

/// Input for a user-prompt-submitted hook
struct UserPromptSubmittedHookInput
{
    int64_t timestamp = 0;
    std::string cwd;
    std::string prompt;
};

inline void from_json(const json& j, UserPromptSubmittedHookInput& h)
{
    if (j.contains("timestamp")) j.at("timestamp").get_to(h.timestamp);
    if (j.contains("cwd")) j.at("cwd").get_to(h.cwd);
    if (j.contains("prompt")) j.at("prompt").get_to(h.prompt);
}

/// Output for a user-prompt-submitted hook
struct UserPromptSubmittedHookOutput
{
    std::optional<std::string> modified_prompt;
    std::optional<std::string> additional_context;
    std::optional<bool> suppress_output;
};

inline void to_json(json& j, const UserPromptSubmittedHookOutput& h)
{
    j = json::object();
    if (h.modified_prompt) j["modifiedPrompt"] = *h.modified_prompt;
    if (h.additional_context) j["additionalContext"] = *h.additional_context;
    if (h.suppress_output) j["suppressOutput"] = *h.suppress_output;
}

using UserPromptSubmittedHandler = std::function<std::optional<UserPromptSubmittedHookOutput>(const UserPromptSubmittedHookInput&, const HookInvocation&)>;

/// Input for a session-start hook
struct SessionStartHookInput
{
    int64_t timestamp = 0;
    std::string cwd;
    std::string source;     ///< "startup", "resume", or "new"
    std::optional<std::string> initial_prompt;
};

inline void from_json(const json& j, SessionStartHookInput& h)
{
    if (j.contains("timestamp")) j.at("timestamp").get_to(h.timestamp);
    if (j.contains("cwd")) j.at("cwd").get_to(h.cwd);
    if (j.contains("source")) j.at("source").get_to(h.source);
    if (j.contains("initialPrompt") && !j["initialPrompt"].is_null())
        h.initial_prompt = j.at("initialPrompt").get<std::string>();
}

/// Output for a session-start hook
struct SessionStartHookOutput
{
    std::optional<std::string> additional_context;
    std::optional<std::map<std::string, json>> modified_config;
};

inline void to_json(json& j, const SessionStartHookOutput& h)
{
    j = json::object();
    if (h.additional_context) j["additionalContext"] = *h.additional_context;
    if (h.modified_config) j["modifiedConfig"] = *h.modified_config;
}

using SessionStartHandler = std::function<std::optional<SessionStartHookOutput>(const SessionStartHookInput&, const HookInvocation&)>;

/// Input for a session-end hook
struct SessionEndHookInput
{
    int64_t timestamp = 0;
    std::string cwd;
    std::string reason;     ///< "complete", "error", "abort", "timeout", or "user_exit"
    std::optional<std::string> final_message;
    std::optional<std::string> error;
};

inline void from_json(const json& j, SessionEndHookInput& h)
{
    if (j.contains("timestamp")) j.at("timestamp").get_to(h.timestamp);
    if (j.contains("cwd")) j.at("cwd").get_to(h.cwd);
    if (j.contains("reason")) j.at("reason").get_to(h.reason);
    if (j.contains("finalMessage") && !j["finalMessage"].is_null())
        h.final_message = j.at("finalMessage").get<std::string>();
    if (j.contains("error") && !j["error"].is_null())
        h.error = j.at("error").get<std::string>();
}

/// Output for a session-end hook
struct SessionEndHookOutput
{
    std::optional<bool> suppress_output;
    std::optional<std::vector<std::string>> cleanup_actions;
    std::optional<std::string> session_summary;
};

inline void to_json(json& j, const SessionEndHookOutput& h)
{
    j = json::object();
    if (h.suppress_output) j["suppressOutput"] = *h.suppress_output;
    if (h.cleanup_actions) j["cleanupActions"] = *h.cleanup_actions;
    if (h.session_summary) j["sessionSummary"] = *h.session_summary;
}

using SessionEndHandler = std::function<std::optional<SessionEndHookOutput>(const SessionEndHookInput&, const HookInvocation&)>;

/// Input for an error-occurred hook
struct ErrorOccurredHookInput
{
    int64_t timestamp = 0;
    std::string cwd;
    std::string error;
    std::string error_context;  ///< "model_call", "tool_execution", "system", or "user_input"
    bool recoverable = false;
};

inline void from_json(const json& j, ErrorOccurredHookInput& h)
{
    if (j.contains("timestamp")) j.at("timestamp").get_to(h.timestamp);
    if (j.contains("cwd")) j.at("cwd").get_to(h.cwd);
    if (j.contains("error")) j.at("error").get_to(h.error);
    if (j.contains("errorContext")) j.at("errorContext").get_to(h.error_context);
    if (j.contains("recoverable")) j.at("recoverable").get_to(h.recoverable);
}

/// Output for an error-occurred hook
struct ErrorOccurredHookOutput
{
    std::optional<bool> suppress_output;
    std::optional<std::string> error_handling;  ///< "retry", "skip", or "abort"
    std::optional<int> retry_count;
    std::optional<std::string> user_notification;
};

inline void to_json(json& j, const ErrorOccurredHookOutput& h)
{
    j = json::object();
    if (h.suppress_output) j["suppressOutput"] = *h.suppress_output;
    if (h.error_handling) j["errorHandling"] = *h.error_handling;
    if (h.retry_count) j["retryCount"] = *h.retry_count;
    if (h.user_notification) j["userNotification"] = *h.user_notification;
}

using ErrorOccurredHandler = std::function<std::optional<ErrorOccurredHookOutput>(const ErrorOccurredHookInput&, const HookInvocation&)>;

/// Hook handlers configuration for a session
struct SessionHooks
{
    std::optional<PreToolUseHandler> on_pre_tool_use;
    std::optional<PostToolUseHandler> on_post_tool_use;
    std::optional<UserPromptSubmittedHandler> on_user_prompt_submitted;
    std::optional<SessionStartHandler> on_session_start;
    std::optional<SessionEndHandler> on_session_end;
    std::optional<ErrorOccurredHandler> on_error_occurred;

    /// Returns true if any hook handler is registered
    bool has_any() const
    {
        return on_pre_tool_use || on_post_tool_use || on_user_prompt_submitted ||
               on_session_start || on_session_end || on_error_occurred;
    }
};

// =============================================================================
// Configuration Types
// =============================================================================

/// Override operation for a single system prompt section
struct SectionOverride
{
    SectionOverrideAction action = SectionOverrideAction::Replace;
    std::optional<std::string> content;
};

inline void to_json(json& j, const SectionOverride& c)
{
    j = json{{"action", c.action}};
    if (c.content)
        j["content"] = *c.content;
}

inline void from_json(const json& j, SectionOverride& c)
{
    if (j.contains("action"))
        c.action = j.at("action").get<SectionOverrideAction>();
    if (j.contains("content"))
        c.content = j.at("content").get<std::string>();
}

/// System message configuration
struct SystemMessageConfig
{
    std::optional<SystemMessageMode> mode;
    std::optional<std::string> content;
    std::optional<std::map<std::string, SectionOverride>> sections;
};

inline void to_json(json& j, const SystemMessageConfig& c)
{
    j = json::object();
    if (c.mode)
        j["mode"] = *c.mode;
    if (c.content)
        j["content"] = *c.content;
    if (c.sections)
        j["sections"] = *c.sections;
}

inline void from_json(const json& j, SystemMessageConfig& c)
{
    if (j.contains("mode"))
        c.mode = j.at("mode").get<SystemMessageMode>();
    if (j.contains("content"))
        c.content = j.at("content").get<std::string>();
    if (j.contains("sections"))
        c.sections = j.at("sections").get<std::map<std::string, SectionOverride>>();
}

/// Azure-specific provider options
struct AzureOptions
{
    std::optional<std::string> api_version;
};

inline void to_json(json& j, const AzureOptions& o)
{
    j = json::object();
    if (o.api_version)
        j["apiVersion"] = *o.api_version;
}

inline void from_json(const json& j, AzureOptions& o)
{
    if (j.contains("apiVersion"))
        o.api_version = j.at("apiVersion").get<std::string>();
}

/// Provider configuration for BYOK (Bring Your Own Key)
struct ProviderConfig
{
    std::optional<std::string> type;
    std::optional<std::string> wire_api;
    std::string base_url;
    std::optional<std::string> api_key;
    std::optional<std::string> bearer_token;
    std::optional<AzureOptions> azure;
    std::optional<std::map<std::string, std::string>> headers;
    std::optional<std::string> model_id;
    std::optional<std::string> wire_model;
    std::optional<int> max_input_tokens;
    std::optional<int> max_output_tokens;

    // ─────────────────────────────────────────────────────────────────────────
    // Environment Variable Support
    // ─────────────────────────────────────────────────────────────────────────

    /// Environment variable names for BYOK configuration
    static constexpr const char* ENV_API_KEY = "COPILOT_SDK_BYOK_API_KEY";
    static constexpr const char* ENV_BASE_URL = "COPILOT_SDK_BYOK_BASE_URL";
    static constexpr const char* ENV_PROVIDER_TYPE = "COPILOT_SDK_BYOK_PROVIDER_TYPE";
    static constexpr const char* ENV_MODEL = "COPILOT_SDK_BYOK_MODEL";

    /// Check if BYOK environment variables are configured
    /// @return true if COPILOT_SDK_BYOK_API_KEY is set and non-empty
    static bool is_env_configured()
    {
        const char* key = std::getenv(ENV_API_KEY);
        return key != nullptr && key[0] != '\0';
    }

    /// Load ProviderConfig from COPILOT_SDK_BYOK_* environment variables
    /// @return ProviderConfig if API key is set, nullopt otherwise
    static std::optional<ProviderConfig> from_env()
    {
        if (!is_env_configured())
            return std::nullopt;

        ProviderConfig config;

        // Required: API key
        config.api_key = std::getenv(ENV_API_KEY);

        // Optional: Base URL (default to OpenAI)
        if (const char* url = std::getenv(ENV_BASE_URL))
            config.base_url = url;
        else
            config.base_url = "https://api.openai.com/v1";

        // Optional: Provider type (default to openai)
        if (const char* ptype = std::getenv(ENV_PROVIDER_TYPE))
            config.type = ptype;
        else
            config.type = "openai";

        return config;
    }

    /// Load model from COPILOT_SDK_BYOK_MODEL environment variable
    /// @return Model string if set, nullopt otherwise
    static std::optional<std::string> model_from_env()
    {
        const char* model = std::getenv(ENV_MODEL);
        if (model != nullptr && model[0] != '\0')
            return std::string(model);
        return std::nullopt;
    }
};

inline void to_json(json& j, const ProviderConfig& c)
{
    j = json{{"baseUrl", c.base_url}};
    if (c.type)
        j["type"] = *c.type;
    if (c.wire_api)
        j["wireApi"] = *c.wire_api;
    if (c.api_key)
        j["apiKey"] = *c.api_key;
    if (c.bearer_token)
        j["bearerToken"] = *c.bearer_token;
    if (c.azure)
        j["azure"] = *c.azure;
    if (c.headers)
        j["headers"] = *c.headers;
    if (c.model_id)
        j["modelId"] = *c.model_id;
    if (c.wire_model)
        j["wireModel"] = *c.wire_model;
    if (c.max_input_tokens)
        j["maxPromptTokens"] = *c.max_input_tokens;
    if (c.max_output_tokens)
        j["maxOutputTokens"] = *c.max_output_tokens;
}

inline void from_json(const json& j, ProviderConfig& c)
{
    j.at("baseUrl").get_to(c.base_url);
    if (j.contains("type"))
        c.type = j.at("type").get<std::string>();
    if (j.contains("wireApi"))
        c.wire_api = j.at("wireApi").get<std::string>();
    if (j.contains("apiKey"))
        c.api_key = j.at("apiKey").get<std::string>();
    if (j.contains("bearerToken"))
        c.bearer_token = j.at("bearerToken").get<std::string>();
    if (j.contains("azure"))
        c.azure = j.at("azure").get<AzureOptions>();
    if (j.contains("headers"))
        c.headers = j.at("headers").get<std::map<std::string, std::string>>();
    if (j.contains("modelId"))
        c.model_id = j.at("modelId").get<std::string>();
    if (j.contains("wireModel"))
        c.wire_model = j.at("wireModel").get<std::string>();
    if (j.contains("maxPromptTokens"))
        c.max_input_tokens = j.at("maxPromptTokens").get<int>();
    if (j.contains("maxOutputTokens"))
        c.max_output_tokens = j.at("maxOutputTokens").get<int>();
}

// =============================================================================
// MCP Server Configuration
// =============================================================================

/// Configuration for a local/stdio MCP server
struct McpLocalServerConfig
{
    std::vector<std::string> tools;
    std::optional<std::string> type;
    std::optional<int> timeout;
    std::string command;
    std::vector<std::string> args;
    std::optional<std::map<std::string, std::string>> env;
    std::optional<std::string> cwd;
};

inline void to_json(json& j, const McpLocalServerConfig& c)
{
    j = json{{"tools", c.tools}, {"command", c.command}, {"args", c.args}};
    if (c.type)
        j["type"] = *c.type;
    if (c.timeout)
        j["timeout"] = *c.timeout;
    if (c.env)
        j["env"] = *c.env;
    if (c.cwd)
        j["cwd"] = *c.cwd;
}

inline void from_json(const json& j, McpLocalServerConfig& c)
{
    j.at("tools").get_to(c.tools);
    j.at("command").get_to(c.command);
    j.at("args").get_to(c.args);
    if (j.contains("type"))
        c.type = j.at("type").get<std::string>();
    if (j.contains("timeout"))
        c.timeout = j.at("timeout").get<int>();
    if (j.contains("env"))
        c.env = j.at("env").get<std::map<std::string, std::string>>();
    if (j.contains("cwd"))
        c.cwd = j.at("cwd").get<std::string>();
}

/// Configuration for a remote MCP server (HTTP or SSE)
struct McpRemoteServerConfig
{
    std::vector<std::string> tools;
    std::string type = "http";
    std::optional<int> timeout;
    std::string url;
    std::optional<std::map<std::string, std::string>> headers;
};

inline void to_json(json& j, const McpRemoteServerConfig& c)
{
    j = json{{"tools", c.tools}, {"type", c.type}, {"url", c.url}};
    if (c.timeout)
        j["timeout"] = *c.timeout;
    if (c.headers)
        j["headers"] = *c.headers;
}

inline void from_json(const json& j, McpRemoteServerConfig& c)
{
    j.at("tools").get_to(c.tools);
    j.at("type").get_to(c.type);
    j.at("url").get_to(c.url);
    if (j.contains("timeout"))
        c.timeout = j.at("timeout").get<int>();
    if (j.contains("headers"))
        c.headers = j.at("headers").get<std::map<std::string, std::string>>();
}

/// Union type for MCP server configuration
using MCPServerConfig = std::variant<McpLocalServerConfig, McpRemoteServerConfig>;

// =============================================================================
// Custom Agent Configuration
// =============================================================================

/// Configuration for a custom agent
struct CustomAgentConfig
{
    std::string name;
    std::optional<std::string> display_name;
    std::optional<std::string> description;
    std::optional<std::vector<std::string>> tools;
    std::string prompt;
    std::optional<std::map<std::string, json>> mcp_servers;
    std::optional<bool> infer;
    std::optional<std::vector<std::string>> skills;
    /// Model identifier for this agent (e.g. "claude-haiku-4.5").
    /// When set, the runtime will attempt to use this model for the agent,
    /// falling back to the parent session model if unavailable.
    std::optional<std::string> model;
};

inline void to_json(json& j, const CustomAgentConfig& c)
{
    j = json{{"name", c.name}, {"prompt", c.prompt}};
    if (c.display_name)
        j["displayName"] = *c.display_name;
    if (c.description)
        j["description"] = *c.description;
    if (c.tools)
        j["tools"] = *c.tools;
    if (c.mcp_servers)
        j["mcpServers"] = *c.mcp_servers;
    if (c.infer)
        j["infer"] = *c.infer;
    if (c.skills)
        j["skills"] = *c.skills;
    if (c.model)
        j["model"] = *c.model;
}

inline void from_json(const json& j, CustomAgentConfig& c)
{
    j.at("name").get_to(c.name);
    j.at("prompt").get_to(c.prompt);
    if (j.contains("displayName"))
        c.display_name = j.at("displayName").get<std::string>();
    if (j.contains("description"))
        c.description = j.at("description").get<std::string>();
    if (j.contains("tools"))
        c.tools = j.at("tools").get<std::vector<std::string>>();
    if (j.contains("mcpServers"))
        c.mcp_servers = j.at("mcpServers").get<std::map<std::string, json>>();
    if (j.contains("infer"))
        c.infer = j.at("infer").get<bool>();
    if (j.contains("skills"))
        c.skills = j.at("skills").get<std::vector<std::string>>();
    if (j.contains("model"))
        c.model = j.at("model").get<std::string>();
}

struct DefaultAgentConfig
{
    std::optional<std::vector<std::string>> excluded_tools;
};

inline void to_json(json& j, const DefaultAgentConfig& c)
{
    j = json::object();
    if (c.excluded_tools)
        j["excludedTools"] = *c.excluded_tools;
}

inline void from_json(const json& j, DefaultAgentConfig& c)
{
    if (j.contains("excludedTools"))
        c.excluded_tools = j.at("excludedTools").get<std::vector<std::string>>();
}

// =============================================================================
// Attachment Types (for MessageOptions)
// =============================================================================

/// Attachment type enum
enum class AttachmentType
{
    File,
    Directory
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    AttachmentType,
    {
        {AttachmentType::File, "file"},
        {AttachmentType::Directory, "directory"},
    }
)

/// Attachment item for user messages
struct UserMessageAttachment
{
    AttachmentType type;
    std::string path;
    std::string display_name;
};

inline void to_json(json& j, const UserMessageAttachment& a)
{
    j = json{{"type", a.type}, {"path", a.path}, {"displayName", a.display_name}};
}

inline void from_json(const json& j, UserMessageAttachment& a)
{
    j.at("type").get_to(a.type);
    j.at("path").get_to(a.path);
    j.at("displayName").get_to(a.display_name);
}

// =============================================================================
// Tool Definition (SDK-side)
// =============================================================================

/// Tool definition for registration with a session
struct Tool
{
    std::string name;
    std::string description;
    json parameters_schema;
    ToolHandler handler;

    /// When true, explicitly indicates this tool is intended to override a
    /// built-in tool of the same name. If not set and the name clashes with
    /// a built-in tool, the runtime returns an error. (Upstream v0.1.49+)
    bool overrides_built_in_tool = false;

    /// When true, the tool can execute without a permission prompt.
    /// (Upstream v0.1.49+)
    bool skip_permission = false;
};

// =============================================================================
// Infinite Session Configuration
// =============================================================================

/// Configuration for infinite sessions with automatic context compaction.
///
/// When enabled, sessions automatically manage context window limits through
/// background compaction and persist state to a workspace directory.
struct InfiniteSessionConfig
{
    /// Whether infinite sessions are enabled (default: true when config is provided)
    std::optional<bool> enabled;

    /// Context utilization threshold (0.0-1.0) at which background compaction starts.
    /// Compaction runs asynchronously, allowing the session to continue processing.
    /// Default: 0.80
    std::optional<double> background_compaction_threshold;

    /// Context utilization threshold (0.0-1.0) at which the session blocks until
    /// compaction completes. This prevents context overflow when compaction hasn't
    /// finished in time. Default: 0.95
    std::optional<double> buffer_exhaustion_threshold;
};

inline void to_json(json& j, const InfiniteSessionConfig& c)
{
    j = json::object();
    if (c.enabled)
        j["enabled"] = *c.enabled;
    if (c.background_compaction_threshold)
        j["backgroundCompactionThreshold"] = *c.background_compaction_threshold;
    if (c.buffer_exhaustion_threshold)
        j["bufferExhaustionThreshold"] = *c.buffer_exhaustion_threshold;
}

inline void from_json(const json& j, InfiniteSessionConfig& c)
{
    if (j.contains("enabled"))
        c.enabled = j.at("enabled").get<bool>();
    if (j.contains("backgroundCompactionThreshold"))
        c.background_compaction_threshold = j.at("backgroundCompactionThreshold").get<double>();
    if (j.contains("bufferExhaustionThreshold"))
        c.buffer_exhaustion_threshold = j.at("bufferExhaustionThreshold").get<double>();
}

// =============================================================================
// Session Configuration
// =============================================================================

/// Configuration for creating a new session
/// Remote session mode (matches upstream nodejs RemoteSessionMode).
/// Controls how the session is exposed to remote consumers (Mission Control).
enum class RemoteSessionMode
{
    Off,
    Export,
    On,
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    RemoteSessionMode,
    {
        {RemoteSessionMode::Off, "off"},
        {RemoteSessionMode::Export, "export"},
        {RemoteSessionMode::On, "on"},
    }
)

struct SessionConfig
{
    std::optional<std::string> session_id;
    std::optional<std::string> model;
    std::optional<json> model_capabilities;
    std::vector<Tool> tools;
    std::optional<std::vector<json>> commands;
    std::optional<SystemMessageConfig> system_message;
    std::optional<std::vector<std::string>> available_tools;
    std::optional<std::vector<std::string>> excluded_tools;
    std::optional<ProviderConfig> provider;
    std::optional<PermissionHandler> on_permission_request;
    bool streaming = false;
    std::optional<std::map<std::string, json>> mcp_servers;
    std::optional<std::vector<CustomAgentConfig>> custom_agents;
    std::optional<DefaultAgentConfig> default_agent;
    std::optional<std::string> agent;

    /// Directories to load skills from.
    std::optional<std::vector<std::string>> skill_directories;

    /// List of skill names to disable.
    std::optional<std::vector<std::string>> disabled_skills;

    /// Infinite session configuration for persistent workspaces and automatic compaction.
    /// When enabled (default), sessions automatically manage context limits and persist state.
    std::optional<InfiniteSessionConfig> infinite_sessions;

    /// Custom configuration directory for the CLI.
    /// When set, overrides the default config location.
    std::optional<std::string> config_dir;

    /// If true and provider/model not explicitly set, load from COPILOT_SDK_BYOK_* env vars.
    /// Default: false (explicit configuration preferred over environment variables)
    bool auto_byok_from_env = false;

    /// Reasoning effort level for models that support it.
    std::optional<ReasoningEffort> reasoning_effort;

    /// Handler for user input requests from the agent (enables ask_user tool).
    std::optional<UserInputHandler> on_user_input_request;

    /// Handler for elicitation requests from the server.
    std::optional<ElicitationHandler> on_elicitation_request;

    /// Handler for exit-plan-mode requests from the agent.
    std::optional<ExitPlanModeHandler> on_exit_plan_mode;

    /// Handler for auto-mode-switch requests.
    std::optional<AutoModeSwitchHandler> on_auto_mode_switch;

    /// Pre-registered event handler - wired up when session is created.
    std::optional<EventHandler> on_event;

    /// Hook handlers for session lifecycle events.
    std::optional<SessionHooks> hooks;

    /// Working directory for the session.
    std::optional<std::string> working_directory;

    /// GitHub token for per-session authentication.
    std::optional<std::string> github_token;

    // ===== v0.1.49 additions =====

    /// Client identifier reported to the CLI (PR #510).
    std::optional<std::string> client_name;

    /// Enable per-session telemetry events (PR #1224).
    std::optional<bool> enable_session_telemetry;

    /// Forward streaming events emitted by sub-agents (PR #1108).
    std::optional<bool> include_sub_agent_streaming_events;

    /// Allow the CLI to discover and apply config files in the working directory
    /// (and ancestors). Default behavior is server-side; this opts in/out (PR #1044).
    std::optional<bool> enable_config_discovery;

    /// Per-session instruction directories merged with the global instruction set (PR #1190).
    std::optional<std::vector<std::string>> instruction_directories;

    /// Remote-session mode for Mission Control integration (PR #1295).
    std::optional<RemoteSessionMode> remote_session;
};

/// Configuration for resuming an existing session
struct ResumeSessionConfig
{
    std::vector<Tool> tools;
    std::optional<ProviderConfig> provider;
    std::optional<PermissionHandler> on_permission_request;
    bool streaming = false;
    std::optional<std::map<std::string, json>> mcp_servers;
    std::optional<std::vector<CustomAgentConfig>> custom_agents;
    std::optional<DefaultAgentConfig> default_agent;
    std::optional<std::string> agent;

    /// Directories to load skills from.
    std::optional<std::vector<std::string>> skill_directories;

    /// List of skill names to disable.
    std::optional<std::vector<std::string>> disabled_skills;

    /// Custom configuration directory for the CLI.
    /// When set, overrides the default config location.
    std::optional<std::string> config_dir;

    /// If true and provider not explicitly set, load from COPILOT_SDK_BYOK_* env vars.
    /// Default: false (explicit configuration preferred over environment variables)
    bool auto_byok_from_env = false;

    /// Model to use for this session. Can change the model when resuming.
    std::optional<std::string> model;
    std::optional<json> model_capabilities;

    /// Reasoning effort level for models that support it.
    std::optional<ReasoningEffort> reasoning_effort;

    std::optional<std::vector<json>> commands;

    /// System message configuration.
    std::optional<SystemMessageConfig> system_message;

    /// List of tool names to allow. When specified, only these tools will be available.
    std::optional<std::vector<std::string>> available_tools;

    /// List of tool names to disable. All other tools remain available.
    std::optional<std::vector<std::string>> excluded_tools;

    /// Working directory for the session.
    std::optional<std::string> working_directory;

    /// When true, the session.resume event is not emitted.
    bool disable_resume = false;

    /// Infinite session configuration.
    std::optional<InfiniteSessionConfig> infinite_sessions;

    /// Handler for user input requests from the agent (enables ask_user tool).
    std::optional<UserInputHandler> on_user_input_request;

    /// Handler for elicitation requests from the server.
    std::optional<ElicitationHandler> on_elicitation_request;

    /// Handler for exit-plan-mode requests from the agent.
    std::optional<ExitPlanModeHandler> on_exit_plan_mode;

    /// Handler for auto-mode-switch requests.
    std::optional<AutoModeSwitchHandler> on_auto_mode_switch;

    /// Pre-registered event handler - wired up when session is created.
    std::optional<EventHandler> on_event;

    /// Hook handlers for session lifecycle events.
    std::optional<SessionHooks> hooks;

    // ===== v0.1.49 additions (mirror SessionConfig) =====

    std::optional<std::string> client_name;
    std::optional<bool> enable_session_telemetry;
    std::optional<bool> include_sub_agent_streaming_events;
    std::optional<bool> enable_config_discovery;
    std::optional<std::vector<std::string>> instruction_directories;
    std::optional<RemoteSessionMode> remote_session;
};

/// Options for sending a message
struct MessageOptions
{
    std::string prompt;
    std::optional<std::vector<UserMessageAttachment>> attachments;
    std::optional<std::string> mode;
    std::optional<std::map<std::string, std::string>> request_headers;
};

inline void to_json(json& j, const MessageOptions& o)
{
    j = json{{"prompt", o.prompt}};
    if (o.attachments)
        j["attachments"] = *o.attachments;
    if (o.mode)
        j["mode"] = *o.mode;
    if (o.request_headers)
        j["requestHeaders"] = *o.request_headers;
}

inline void from_json(const json& j, MessageOptions& o)
{
    j.at("prompt").get_to(o.prompt);
    if (j.contains("attachments"))
        o.attachments = j.at("attachments").get<std::vector<UserMessageAttachment>>();
    if (j.contains("mode"))
        o.mode = j.at("mode").get<std::string>();
    if (j.contains("requestHeaders"))
        o.request_headers = j.at("requestHeaders").get<std::map<std::string, std::string>>();
}

// =============================================================================
// Client Options
// =============================================================================

/// Options for creating a CopilotClient
struct ClientOptions
{
    std::optional<std::string> cli_path;
    std::optional<std::vector<std::string>> cli_args;
    std::optional<std::string> cwd;
    int port = 0;
    bool use_stdio = true;
    std::optional<std::string> cli_url;
    LogLevel log_level = LogLevel::Info;
    bool auto_start = true;

    /// @deprecated This option has no effect and will be removed in a future release.
    /// Retained for source compatibility with v0.1.23 callers; the SDK no longer
    /// auto-restarts the CLI on exit (matches upstream nodejs SDK semantics).
    [[deprecated("auto_restart has no effect; will be removed in a future release")]]
    bool auto_restart = false;

    std::optional<std::map<std::string, std::string>> environment;

    /// GitHub token for authentication. Cannot be used with cli_url.
    /// On the wire to the CLI, this is forwarded via the COPILOT_SDK_AUTH_TOKEN
    /// environment variable plus the --auth-token-env CLI flag.
    std::optional<std::string> github_token;

    /// Whether to use logged-in user for auth. Defaults to true when github_token is empty.
    /// Cannot be used with cli_url.
    std::optional<bool> use_logged_in_user;

    /// Connection token for the headless CLI server (TCP only). When the SDK
    /// spawns its own CLI in TCP mode and this is omitted, a UUID is generated
    /// automatically so the loopback listener is safe by default. Rejected with
    /// `use_stdio = true` (stdio is pre-authenticated by transport).
    /// Forwarded to the CLI via the COPILOT_CONNECTION_TOKEN environment variable.
    std::optional<std::string> tcp_connection_token;

    /// Custom data directory for the Copilot CLI ($COPILOT_HOME). When omitted,
    /// the CLI uses its default location (typically ~/.copilot).
    std::optional<std::string> copilot_home;

    /// Server-wide idle timeout for sessions in seconds.
    /// Sessions without activity for this duration are automatically cleaned up.
    /// Set to 0 or omit to disable (sessions live indefinitely).
    /// Only used when the SDK spawns the CLI process; ignored when connecting to
    /// an external server via {@link cli_url}.
    std::optional<int> session_idle_timeout_seconds;

    /// Enable remote session support (Mission Control integration).
    /// When true, sessions in a GitHub repository working directory are accessible
    /// from GitHub web and mobile.
    /// Only used when the SDK spawns the CLI process; ignored when connecting to
    /// an external server via {@link cli_url}.
    bool remote = false;
};

// =============================================================================
// Response Types
// =============================================================================

/// Working directory context (cwd, git info) from session creation
struct SessionContext
{
    std::string cwd;
    std::optional<std::string> git_root;
    std::optional<std::string> repository; ///< owner/repo
    std::optional<std::string> branch;
};

inline void from_json(const json& j, SessionContext& c)
{
    if (j.contains("cwd") && !j.at("cwd").is_null())
        j.at("cwd").get_to(c.cwd);
    if (j.contains("gitRoot") && !j.at("gitRoot").is_null())
        c.git_root = j.at("gitRoot").get<std::string>();
    if (j.contains("repository") && !j.at("repository").is_null())
        c.repository = j.at("repository").get<std::string>();
    if (j.contains("branch") && !j.at("branch").is_null())
        c.branch = j.at("branch").get<std::string>();
}

inline void to_json(json& j, const SessionContext& c)
{
    j = json{{"cwd", c.cwd}};
    if (c.git_root)
        j["gitRoot"] = *c.git_root;
    if (c.repository)
        j["repository"] = *c.repository;
    if (c.branch)
        j["branch"] = *c.branch;
}

/// Filter for Client::list_sessions(). All fields are optional; only matching
/// sessions are returned. Matches upstream nodejs SessionListFilter.
struct SessionListFilter
{
    std::optional<std::string> cwd;        ///< exact cwd match
    std::optional<std::string> git_root;   ///< exact git root match
    std::optional<std::string> repository; ///< owner/repo
    std::optional<std::string> branch;
};

inline void to_json(json& j, const SessionListFilter& f)
{
    j = json::object();
    if (f.cwd)
        j["cwd"] = *f.cwd;
    if (f.git_root)
        j["gitRoot"] = *f.git_root;
    if (f.repository)
        j["repository"] = *f.repository;
    if (f.branch)
        j["branch"] = *f.branch;
}

/// Metadata about a session
struct SessionMetadata
{
    std::string session_id;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point modified_time;
    std::optional<std::string> summary;
    bool is_remote = false;
    std::optional<SessionContext> context;
};

namespace detail
{

inline bool parse_fixed_decimal(const std::string& s, size_t pos, size_t len, int& value)
{
    if (pos + len > s.size())
        return false;
    int v = 0;
    for (size_t i = 0; i < len; ++i)
    {
        char c = s[pos + i];
        if (c < '0' || c > '9')
            return false;
        v = (v * 10) + (c - '0');
    }
    value = v;
    return true;
}

inline std::optional<std::chrono::system_clock::time_point> parse_iso8601_timestamp(const std::string& s)
{
    // Accepts: YYYY-MM-DDTHH:MM:SS[.fffffffff](Z|(+|-)HH:MM)
    // Copilot SDKs generally emit RFC3339/ISO8601 strings (e.g. 2025-01-17T10:24:12.345Z).
    if (s.size() < 19)
        return std::nullopt;

    int year_num = 0, month_num = 0, day_num = 0, hour_num = 0, minute_num = 0, second_num = 0;
    if (!parse_fixed_decimal(s, 0, 4, year_num) || s[4] != '-' ||
        !parse_fixed_decimal(s, 5, 2, month_num) || s[7] != '-' ||
        !parse_fixed_decimal(s, 8, 2, day_num))
    {
        return std::nullopt;
    }

    const char t = s[10];
    if (t != 'T' && t != 't' && t != ' ')
        return std::nullopt;

    if (!parse_fixed_decimal(s, 11, 2, hour_num) || s[13] != ':' ||
        !parse_fixed_decimal(s, 14, 2, minute_num) || s[16] != ':' ||
        !parse_fixed_decimal(s, 17, 2, second_num))
    {
        return std::nullopt;
    }

    size_t pos = 19;
    std::chrono::nanoseconds fractional_ns{0};
    if (pos < s.size() && s[pos] == '.')
    {
        ++pos;
        size_t start = pos;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
            ++pos;
        size_t digits = pos - start;
        if (digits == 0)
            return std::nullopt;

        if (digits > 9)
            digits = 9; // truncate to nanoseconds precision

        int frac = 0;
        if (!parse_fixed_decimal(s, start, digits, frac))
            return std::nullopt;

        int scale = 1;
        for (size_t i = digits; i < 9; ++i)
            scale *= 10;
        fractional_ns = std::chrono::nanoseconds{static_cast<int64_t>(frac) * scale};

        // If there were more than 9 digits, ignore the rest (already advanced pos above).
    }

    int tz_offset_minutes = 0;
    if (pos >= s.size())
    {
        tz_offset_minutes = 0;
    }
    else if (s[pos] == 'Z' || s[pos] == 'z')
    {
        ++pos;
    }
    else if (s[pos] == '+' || s[pos] == '-')
    {
        int sign = (s[pos] == '-') ? -1 : 1;
        ++pos;

        int tzh = 0, tzm = 0;
        if (!parse_fixed_decimal(s, pos, 2, tzh))
            return std::nullopt;
        pos += 2;
        if (pos < s.size() && s[pos] == ':')
            ++pos;
        if (!parse_fixed_decimal(s, pos, 2, tzm))
            return std::nullopt;
        pos += 2;

        tz_offset_minutes = sign * ((tzh * 60) + tzm);
    }
    else
    {
        return std::nullopt;
    }

    if (pos != s.size())
        return std::nullopt;

    std::chrono::year_month_day ymd{
        std::chrono::year{year_num},
        std::chrono::month{static_cast<unsigned>(month_num)},
        std::chrono::day{static_cast<unsigned>(day_num)}};
    if (!ymd.ok())
        return std::nullopt;

    std::chrono::sys_time<std::chrono::nanoseconds> tp =
        std::chrono::sys_days{ymd} + std::chrono::hours{hour_num} + std::chrono::minutes{minute_num} +
        std::chrono::seconds{second_num} + fractional_ns;
    tp -= std::chrono::minutes{tz_offset_minutes};

    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(tp);
}

} // namespace detail

inline void from_json(const json& j, SessionMetadata& m)
{
    j.at("sessionId").get_to(m.session_id);
    m.start_time = {};
    m.modified_time = {};

    // Parse ISO 8601 timestamps
    if (j.contains("startTime"))
    {
        auto ts = j.at("startTime").get<std::string>();
        if (auto parsed = detail::parse_iso8601_timestamp(ts))
            m.start_time = *parsed;
    }
    if (j.contains("modifiedTime"))
    {
        auto ts = j.at("modifiedTime").get<std::string>();
        if (auto parsed = detail::parse_iso8601_timestamp(ts))
            m.modified_time = *parsed;
    }
    if (j.contains("summary"))
        m.summary = j.at("summary").get<std::string>();
    if (j.contains("isRemote"))
        j.at("isRemote").get_to(m.is_remote);
    if (j.contains("context") && !j.at("context").is_null())
        m.context = j.at("context").get<SessionContext>();
}

/// Error reported during client stop/cleanup
struct StopError
{
    std::string message;
};

/// Response from a ping request
struct PingResponse
{
    std::string message;
    int64_t timestamp;
    std::optional<int> protocol_version;
};

inline void from_json(const json& j, PingResponse& r)
{
    j.at("message").get_to(r.message);
    j.at("timestamp").get_to(r.timestamp);
    if (j.contains("protocolVersion"))
        r.protocol_version = j.at("protocolVersion").get<int>();
}

/// Response from status.get request
struct GetStatusResponse
{
    std::string version;
    int protocol_version;
};

inline void from_json(const json& j, GetStatusResponse& r)
{
    j.at("version").get_to(r.version);
    j.at("protocolVersion").get_to(r.protocol_version);
}

/// Response from auth.getStatus request
struct GetAuthStatusResponse
{
    bool is_authenticated;
    std::optional<std::string> auth_type;
    std::optional<std::string> host;
    std::optional<std::string> login;
    std::optional<std::string> status_message;
};

inline void from_json(const json& j, GetAuthStatusResponse& r)
{
    j.at("isAuthenticated").get_to(r.is_authenticated);
    if (j.contains("authType") && !j["authType"].is_null())
        r.auth_type = j["authType"].get<std::string>();
    if (j.contains("host") && !j["host"].is_null())
        r.host = j["host"].get<std::string>();
    if (j.contains("login") && !j["login"].is_null())
        r.login = j["login"].get<std::string>();
    if (j.contains("statusMessage") && !j["statusMessage"].is_null())
        r.status_message = j["statusMessage"].get<std::string>();
}

/// Vision limits for a model
struct ModelVisionLimits
{
    std::vector<std::string> supported_media_types;
    int max_prompt_images = 0;
    int max_prompt_image_size = 0;
};

inline void from_json(const json& j, ModelVisionLimits& v)
{
    if (j.contains("supportedMediaTypes"))
        j.at("supportedMediaTypes").get_to(v.supported_media_types);
    if (j.contains("maxPromptImages"))
        j.at("maxPromptImages").get_to(v.max_prompt_images);
    if (j.contains("maxPromptImageSize"))
        j.at("maxPromptImageSize").get_to(v.max_prompt_image_size);
}

/// Model capabilities - what the model supports
struct ModelCapabilities
{
    struct Supports
    {
        bool vision = false;
        bool reasoning_effort = false;
    };
    struct Limits
    {
        std::optional<int> max_prompt_tokens;
        int max_context_window_tokens = 0;
        std::optional<ModelVisionLimits> vision;
    };
    Supports supports;
    Limits limits;
};

inline void from_json(const json& j, ModelCapabilities& c)
{
    if (j.contains("supports"))
    {
        if (j["supports"].contains("vision"))
            j["supports"]["vision"].get_to(c.supports.vision);
        if (j["supports"].contains("reasoningEffort"))
            j["supports"]["reasoningEffort"].get_to(c.supports.reasoning_effort);
    }
    if (j.contains("limits"))
    {
        if (j["limits"].contains("max_prompt_tokens") && !j["limits"]["max_prompt_tokens"].is_null())
            c.limits.max_prompt_tokens = j["limits"]["max_prompt_tokens"].get<int>();
        if (j["limits"].contains("max_context_window_tokens"))
            j["limits"]["max_context_window_tokens"].get_to(c.limits.max_context_window_tokens);
        if (j["limits"].contains("vision") && !j["limits"]["vision"].is_null())
            c.limits.vision = j["limits"]["vision"].get<ModelVisionLimits>();
    }
}

/// Model policy state
struct ModelPolicy
{
    std::string state;
    std::string terms;
};

inline void from_json(const json& j, ModelPolicy& p)
{
    j.at("state").get_to(p.state);
    if (j.contains("terms"))
        j.at("terms").get_to(p.terms);
}

/// Model billing information
struct ModelBilling
{
    double multiplier = 1.0;
};

inline void from_json(const json& j, ModelBilling& b)
{
    if (j.contains("multiplier"))
        j.at("multiplier").get_to(b.multiplier);
}

/// Information about an available model
struct ModelInfo
{
    std::string id;
    std::string name;
    ModelCapabilities capabilities;
    std::optional<ModelPolicy> policy;
    std::optional<ModelBilling> billing;
    std::optional<std::vector<std::string>> supported_reasoning_efforts;
    std::optional<std::string> default_reasoning_effort;
};

inline void from_json(const json& j, ModelInfo& m)
{
    j.at("id").get_to(m.id);
    j.at("name").get_to(m.name);
    if (j.contains("capabilities"))
        j.at("capabilities").get_to(m.capabilities);
    if (j.contains("policy") && !j["policy"].is_null())
        m.policy = j["policy"].get<ModelPolicy>();
    if (j.contains("billing") && !j["billing"].is_null())
        m.billing = j["billing"].get<ModelBilling>();
    if (j.contains("supportedReasoningEfforts") && !j["supportedReasoningEfforts"].is_null())
        m.supported_reasoning_efforts = j["supportedReasoningEfforts"].get<std::vector<std::string>>();
    if (j.contains("defaultReasoningEffort") && !j["defaultReasoningEffort"].is_null())
        m.default_reasoning_effort = j["defaultReasoningEffort"].get<std::string>();
}

/// Response wrapper for listing models
struct GetModelsResponse
{
    std::vector<ModelInfo> models;
};

inline void from_json(const json& j, GetModelsResponse& r)
{
    if (j.contains("models") && j["models"].is_array())
        j.at("models").get_to(r.models);
}

// =============================================================================
// Selection Attachment Type
// =============================================================================

/// Position within a text selection
struct SelectionPosition
{
    double line = 0;
    double character = 0;
};

inline void from_json(const json& j, SelectionPosition& p)
{
    j.at("line").get_to(p.line);
    j.at("character").get_to(p.character);
}

inline void to_json(json& j, const SelectionPosition& p)
{
    j = json{{"line", p.line}, {"character", p.character}};
}

/// Selection range within a file
struct SelectionRange
{
    SelectionPosition start;
    SelectionPosition end;
};

inline void from_json(const json& j, SelectionRange& r)
{
    j.at("start").get_to(r.start);
    j.at("end").get_to(r.end);
}

inline void to_json(json& j, const SelectionRange& r)
{
    j = json{{"start", r.start}, {"end", r.end}};
}

/// Selection attachment for user messages
struct SelectionAttachment
{
    std::string file_path;
    std::string display_name;
    std::string text;
    SelectionRange selection;
};

inline void from_json(const json& j, SelectionAttachment& a)
{
    j.at("filePath").get_to(a.file_path);
    j.at("displayName").get_to(a.display_name);
    j.at("text").get_to(a.text);
    j.at("selection").get_to(a.selection);
}

inline void to_json(json& j, const SelectionAttachment& a)
{
    j = json{{"type", "selection"}, {"filePath", a.file_path},
             {"displayName", a.display_name}, {"text", a.text}, {"selection", a.selection}};
}

// =============================================================================
// Session Lifecycle Types
// =============================================================================

/// Session lifecycle event type constants
namespace SessionLifecycleEventTypes
{
    inline constexpr const char* Created = "session.created";
    inline constexpr const char* Deleted = "session.deleted";
    inline constexpr const char* Updated = "session.updated";
    inline constexpr const char* Foreground = "session.foreground";
    inline constexpr const char* Background = "session.background";
}

/// Metadata for session lifecycle events
struct SessionLifecycleEventMetadata
{
    std::string start_time;
    std::string modified_time;
    std::optional<std::string> summary;
};

inline void from_json(const json& j, SessionLifecycleEventMetadata& m)
{
    j.at("startTime").get_to(m.start_time);
    j.at("modifiedTime").get_to(m.modified_time);
    if (j.contains("summary") && !j["summary"].is_null())
        m.summary = j.at("summary").get<std::string>();
}

/// Session lifecycle event notification
struct SessionLifecycleEvent
{
    std::string type;
    std::string session_id;
    std::optional<SessionLifecycleEventMetadata> metadata;
};

inline void from_json(const json& j, SessionLifecycleEvent& e)
{
    j.at("type").get_to(e.type);
    j.at("sessionId").get_to(e.session_id);
    if (j.contains("metadata") && !j["metadata"].is_null())
        e.metadata = j.at("metadata").get<SessionLifecycleEventMetadata>();
}

/// Response from session.getForeground
struct GetForegroundSessionResponse
{
    std::optional<std::string> session_id;
    std::optional<std::string> workspace_path;
};

inline void from_json(const json& j, GetForegroundSessionResponse& r)
{
    if (j.contains("sessionId") && !j["sessionId"].is_null())
        r.session_id = j.at("sessionId").get<std::string>();
    if (j.contains("workspacePath") && !j["workspacePath"].is_null())
        r.workspace_path = j.at("workspacePath").get<std::string>();
}

/// Response from session.setForeground
struct SetForegroundSessionResponse
{
    bool success = false;
    std::optional<std::string> error;
};

inline void from_json(const json& j, SetForegroundSessionResponse& r)
{
    j.at("success").get_to(r.success);
    if (j.contains("error") && !j["error"].is_null())
        r.error = j.at("error").get<std::string>();
}

} // namespace copilot
