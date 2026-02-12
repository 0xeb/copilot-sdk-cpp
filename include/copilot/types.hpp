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

// =============================================================================
// Protocol Version
// =============================================================================

/// SDK protocol version - must match copilot-agent-runtime server
inline constexpr int kSdkProtocolVersion = 2;

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
    Replace
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
    Denied
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ToolResultType,
    {
        {ToolResultType::Success, "success"},
        {ToolResultType::Failure, "failure"},
        {ToolResultType::Rejected, "rejected"},
        {ToolResultType::Denied, "denied"},
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

/// System message configuration
struct SystemMessageConfig
{
    std::optional<SystemMessageMode> mode;
    std::optional<std::string> content;
};

inline void to_json(json& j, const SystemMessageConfig& c)
{
    j = json::object();
    if (c.mode)
        j["mode"] = *c.mode;
    if (c.content)
        j["content"] = *c.content;
}

inline void from_json(const json& j, SystemMessageConfig& c)
{
    if (j.contains("mode"))
        c.mode = j.at("mode").get<SystemMessageMode>();
    if (j.contains("content"))
        c.content = j.at("content").get<std::string>();
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
struct SessionConfig
{
    std::optional<std::string> session_id;
    std::optional<std::string> model;
    std::vector<Tool> tools;
    std::optional<SystemMessageConfig> system_message;
    std::optional<std::vector<std::string>> available_tools;
    std::optional<std::vector<std::string>> excluded_tools;
    std::optional<ProviderConfig> provider;
    std::optional<PermissionHandler> on_permission_request;
    bool streaming = false;
    std::optional<std::map<std::string, json>> mcp_servers;
    std::optional<std::vector<CustomAgentConfig>> custom_agents;

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

    /// Hook handlers for session lifecycle events.
    std::optional<SessionHooks> hooks;

    /// Working directory for the session.
    std::optional<std::string> working_directory;
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

    /// Reasoning effort level for models that support it.
    std::optional<ReasoningEffort> reasoning_effort;

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

    /// Hook handlers for session lifecycle events.
    std::optional<SessionHooks> hooks;
};

/// Options for sending a message
struct MessageOptions
{
    std::string prompt;
    std::optional<std::vector<UserMessageAttachment>> attachments;
    std::optional<std::string> mode;
};

inline void to_json(json& j, const MessageOptions& o)
{
    j = json{{"prompt", o.prompt}};
    if (o.attachments)
        j["attachments"] = *o.attachments;
    if (o.mode)
        j["mode"] = *o.mode;
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
    bool auto_restart = true;
    std::optional<std::map<std::string, std::string>> environment;

    /// GitHub token for authentication. Cannot be used with cli_url.
    std::optional<std::string> github_token;

    /// Whether to use logged-in user for auth. Defaults to true when github_token is empty.
    /// Cannot be used with cli_url.
    std::optional<bool> use_logged_in_user;
};

// =============================================================================
// Response Types
// =============================================================================

/// Metadata about a session
struct SessionMetadata
{
    std::string session_id;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point modified_time;
    std::optional<std::string> summary;
    bool is_remote = false;
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
