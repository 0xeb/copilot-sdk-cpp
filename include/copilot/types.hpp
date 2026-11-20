// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <copilot/cancellation.hpp>
#include <copilot/generated/api_types.hpp>
#include <copilot/generated/protocol_version.hpp>
#include <copilot/session_fs.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace copilot
{

class CopilotRequestHandler;
class Canvas;
struct WorkflowHandle;

// =============================================================================
// Type Aliases
// =============================================================================

/// JSON type alias for cleaner API
using json = nlohmann::json;

namespace detail
{
/// Value of an environment variable, or nullopt when unset. Uses _dupenv_s on MSVC so
/// consumers compiling with /W4 /WX do not trip C4996 on std::getenv.
inline std::optional<std::string> env_value(const char* name)
{
#ifdef _MSC_VER
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
        return std::nullopt;
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

/// Like env_value(), but an empty value counts as unset.
inline std::optional<std::string> non_empty_env_value(const char* name)
{
    auto value = env_value(name);
    if (value && value->empty())
        return std::nullopt;
    return value;
}
} // namespace detail

// Forward declarations
class Session;
struct SessionEvent;
using EventHandler = std::function<void(const SessionEvent&)>;

// =============================================================================
// Protocol Version
// =============================================================================

/// Maximum SDK protocol version supported (matches the pinned official SDK baseline).
inline constexpr int kSdkProtocolVersion = generated::kSdkProtocolVersion;

/// Minimum SDK protocol version this SDK can communicate with.
/// Older servers (reporting < kMinProtocolVersion) are rejected.
inline constexpr int kMinProtocolVersion = generated::kMinProtocolVersion;

/// Thrown by Client::start() when the runtime's protocol version is outside
/// [kMinProtocolVersion, kSdkProtocolVersion] or the runtime does not report one.
/// Lets a host tell an incompatible CLI apart from network or authentication failures.
class ProtocolVersionMismatch : public std::runtime_error
{
  public:
    ProtocolVersionMismatch(
        std::optional<int> server_version,
        std::optional<std::string> cli_version,
        std::optional<std::string> cli_path
    )
        : std::runtime_error(format(server_version, cli_version, cli_path)),
          server_version_(server_version), cli_version_(std::move(cli_version)),
          cli_path_(std::move(cli_path))
    {
    }

    /// Protocol version the runtime reported; nullopt when it reported none.
    const std::optional<int>& server_version() const noexcept
    {
        return server_version_;
    }
    int min_version() const noexcept
    {
        return kMinProtocolVersion;
    }
    int max_version() const noexcept
    {
        return kSdkProtocolVersion;
    }
    /// Runtime (CLI) version string from the handshake, when reported.
    const std::optional<std::string>& cli_version() const noexcept
    {
        return cli_version_;
    }
    /// Resolved executable this client spawned; nullopt for external/in-process runtimes.
    const std::optional<std::string>& cli_path() const noexcept
    {
        return cli_path_;
    }

  private:
    static std::string format(
        const std::optional<int>& server_version,
        const std::optional<std::string>& cli_version,
        const std::optional<std::string>& cli_path
    )
    {
        // TS REFERENCE: nodejs/src/client.ts verifyProtocolVersion (message prefix)
        std::string message = "SDK protocol version mismatch: SDK supports versions " +
                              std::to_string(kMinProtocolVersion) + "-" +
                              std::to_string(kSdkProtocolVersion) + ", but server ";
        message += server_version ? "reports version " + std::to_string(*server_version)
                                  : std::string("does not report a protocol version");
        message += ".";
        if (cli_path || cli_version)
        {
            message += " Runtime:";
            if (cli_path)
                message += " " + *cli_path;
            if (cli_version)
                message += " (CLI " + *cli_version + ")";
            message += ".";
        }
        message += server_version && *server_version > kSdkProtocolVersion
                       ? " Use a copilot-sdk-cpp release that supports protocol " +
                             std::to_string(*server_version) +
                             ", or run a CLI that speaks this SDK's protocol."
                       : " Please update your SDK or server to ensure compatibility.";
        return message;
    }

    std::optional<int> server_version_;
    std::optional<std::string> cli_version_;
    std::optional<std::string> cli_path_;
};

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
    XHigh,
    Max
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ReasoningEffort,
    {
        {ReasoningEffort::Low, "low"},
        {ReasoningEffort::Medium, "medium"},
        {ReasoningEffort::High, "high"},
        {ReasoningEffort::XHigh, "xhigh"},
        {ReasoningEffort::Max, "max"},
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
    std::optional<std::vector<std::string>> tool_references;
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
    if (r.tool_references)
        j["toolReferences"] = *r.tool_references;
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
    if (j.contains("toolReferences"))
        r.tool_references = j.at("toolReferences").get<std::vector<std::string>>();
}

/// Information about a tool invocation from the server
struct ToolInvocation
{
    std::string session_id;
    std::string tool_call_id;
    std::string tool_name;
    std::optional<json> arguments;
    std::optional<std::vector<json>> available_tools;
    std::optional<std::string> traceparent;
    std::optional<std::string> tracestate;
    std::optional<std::string> agent_id;
    std::optional<std::string> request_id;
    /// Cancelled when the runtime completes this request or the session disconnects.
    CancellationToken cancellation;
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
    std::optional<bool> managed_approval_required;
    std::map<std::string, json> extension_data;
};

inline void to_json(json& j, const PermissionRequest& r)
{
    j = json{{"kind", r.kind}};
    if (r.tool_call_id)
        j["toolCallId"] = *r.tool_call_id;
    if (r.managed_approval_required)
        j["managedApprovalRequired"] = *r.managed_approval_required;
    for (const auto& [k, v] : r.extension_data)
        j[k] = v;
}

inline void from_json(const json& j, PermissionRequest& r)
{
    j.at("kind").get_to(r.kind);
    if (j.contains("toolCallId"))
        r.tool_call_id = j.at("toolCallId").get<std::string>();
    if (j.contains("managedApprovalRequired") && !j["managedApprovalRequired"].is_null())
        r.managed_approval_required = j.at("managedApprovalRequired").get<bool>();
    // Collect extension data (all fields except kind and toolCallId)
    for (auto& [k, v] : j.items())
        if (k != "kind" && k != "toolCallId" && k != "managedApprovalRequired")
            r.extension_data[k] = v;
}

/// Result of a permission request (response to CLI)
struct PermissionRequestResult
{
    std::string kind; // e.g., "approved", "denied-no-approval-rule-and-could-not-request-from-user"
    std::optional<std::vector<json>> rules;
    std::optional<json> decision_context;
    std::map<std::string, json> extension_data;

    static PermissionRequestResult no_result()
    {
        return PermissionRequestResult{.kind = "no-result"};
    }
};

inline void to_json(json& j, const PermissionRequestResult& r)
{
    j = json{{"kind", r.kind}};
    if (r.rules)
        j["rules"] = *r.rules;
    for (const auto& [key, value] : r.extension_data)
        j[key] = value;
}

inline void from_json(const json& j, PermissionRequestResult& r)
{
    j.at("kind").get_to(r.kind);
    if (j.contains("rules"))
        r.rules = j.at("rules").get<std::vector<json>>();
    for (const auto& [key, value] : j.items())
        if (key != "kind" && key != "rules")
            r.extension_data[key] = value;
}

/// Context for permission invocation
struct PermissionInvocation
{
    std::string session_id;
    bool managed_settings_enabled = false;
    std::optional<json> prompt_request;
    std::optional<json> assisted_approval_assessment;
    std::optional<std::string> program_tool_call_id;
};

/// Permission handler function type
using PermissionHandler = std::function<PermissionRequestResult(const PermissionRequest& request)>;
using PermissionHandlerWithContext =
    std::function<PermissionRequestResult(const PermissionRequest&, const PermissionInvocation&)>;

// =============================================================================
// MCP OAuth (host-provided credentials)
// =============================================================================
// NODEJS REFERENCE: nodejs/src/types.ts McpAuthRequest / McpAuthResult / McpAuthHandler
// C# REFERENCE: dotnet/src/Types.cs McpAuthContext / McpAuthResult

/// Parsed parameters from an MCP server's WWW-Authenticate response.
struct McpAuthWwwAuthenticateParams
{
    std::optional<std::string> resource_metadata_url;
    std::optional<std::string> scope;
    std::optional<std::string> error;
};

inline void to_json(json& j, const McpAuthWwwAuthenticateParams& p)
{
    j = json::object();
    if (p.resource_metadata_url)
        j["resourceMetadataUrl"] = *p.resource_metadata_url;
    if (p.scope)
        j["scope"] = *p.scope;
    if (p.error)
        j["error"] = *p.error;
}

inline void from_json(const json& j, McpAuthWwwAuthenticateParams& p)
{
    if (j.contains("resourceMetadataUrl") && j["resourceMetadataUrl"].is_string())
        p.resource_metadata_url = j["resourceMetadataUrl"].get<std::string>();
    if (j.contains("scope") && j["scope"].is_string())
        p.scope = j["scope"].get<std::string>();
    if (j.contains("error") && j["error"].is_string())
        p.error = j["error"].get<std::string>();
}

/// Static OAuth client configuration supplied by the MCP server.
struct McpAuthStaticClientConfig
{
    std::string client_id;
    std::optional<std::string> client_secret;
    /// "client_credentials" when set.
    std::optional<std::string> grant_type;
    std::optional<bool> public_client;
    /// Configured scope used when the server challenge omits one.
    std::optional<std::string> scope;
};

inline void to_json(json& j, const McpAuthStaticClientConfig& c)
{
    j = json{{"clientId", c.client_id}};
    if (c.client_secret)
        j["clientSecret"] = *c.client_secret;
    if (c.grant_type)
        j["grantType"] = *c.grant_type;
    if (c.public_client)
        j["publicClient"] = *c.public_client;
    if (c.scope)
        j["scope"] = *c.scope;
}

/// MCP OAuth request the host can satisfy with a host-acquired token.
struct McpAuthRequest
{
    std::string request_id;
    std::string server_name;
    std::string server_url;
    /// "initial" | "refresh" | "reauth" | "upscope"
    std::string reason;
    /// Whether this refresh request supports a noninteractive runtime handoff.
    bool runtime_refresh_supported = false;
    std::optional<McpAuthWwwAuthenticateParams> www_authenticate_params;
    /// Raw RFC 9728 protected-resource metadata JSON, if fetched.
    std::optional<std::string> resource_metadata;
    std::optional<McpAuthStaticClientConfig> static_client_config;
};

inline void to_json(json& j, const McpAuthRequest& r)
{
    j = json{
        {"requestId", r.request_id},
        {"serverName", r.server_name},
        {"serverUrl", r.server_url},
        {"reason", r.reason},
    };
    if (r.runtime_refresh_supported)
        j["runtimeRefreshSupported"] = true;
    if (r.www_authenticate_params)
        j["wwwAuthenticateParams"] = *r.www_authenticate_params;
    if (r.resource_metadata)
        j["resourceMetadata"] = *r.resource_metadata;
    if (r.static_client_config)
        j["staticClientConfig"] = *r.static_client_config;
}

/// Context passed alongside an McpAuthRequest.
struct McpAuthInvocation
{
    std::string session_id;
};

/// Host-provided OAuth token data.
struct McpAuthToken
{
    std::string access_token;
    /// Defaults to Bearer when omitted.
    std::optional<std::string> token_type;
    std::optional<int64_t> expires_in;
};

/// Result of an MCP auth handler. Cancelled wins; then a non-empty token; then
/// runtime_refresh (honored only when the request advertises runtime_refresh_supported).
/// Anything else cancels.
struct McpAuthResult
{
    bool cancelled = false;
    bool runtime_refresh = false;
    std::optional<McpAuthToken> token;

    static McpAuthResult from_token(McpAuthToken token)
    {
        McpAuthResult r;
        r.token = std::move(token);
        return r;
    }
    static McpAuthResult cancel()
    {
        McpAuthResult r;
        r.cancelled = true;
        return r;
    }
    /// Hand an advertised refresh back to the runtime. Acceptance is not reconnect completion.
    static McpAuthResult refresh_in_runtime()
    {
        McpAuthResult r;
        r.runtime_refresh = true;
        return r;
    }
};

/// Returning std::nullopt cancels the pending request; so does throwing.
using McpAuthHandler = std::function<
    std::optional<McpAuthResult>(const McpAuthRequest& request, const McpAuthInvocation& invocation)>;

// =============================================================================
// User Input Types
// =============================================================================

/// Request for user input from the agent (ask_user tool)
struct UserInputRequest
{
    std::string question;
    std::optional<std::vector<std::string>> choices;
    std::optional<bool> allow_freeform;
    /// Runtime ID of the pending `user_input.requested` request; pass it as
    /// `requestId` to answer this exact request through the session UI RPCs.
    /// Absent when the runtime predates the field.
    std::optional<std::string> request_id;
};

inline void from_json(const json& j, UserInputRequest& r)
{
    j.at("question").get_to(r.question);
    if (j.contains("choices") && !j["choices"].is_null())
        r.choices = j.at("choices").get<std::vector<std::string>>();
    if (j.contains("allowFreeform") && !j["allowFreeform"].is_null())
        r.allow_freeform = j.at("allowFreeform").get<bool>();
    if (j.contains("requestId") && j["requestId"].is_string())
        r.request_id = j.at("requestId").get<std::string>();
}

inline void to_json(json& j, const UserInputRequest& r)
{
    j = json{{"question", r.question}};
    if (r.choices)
        j["choices"] = *r.choices;
    if (r.allow_freeform)
        j["allowFreeform"] = *r.allow_freeform;
    if (r.request_id)
        j["requestId"] = *r.request_id;
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
using UserInputHandler =
    std::function<UserInputResponse(const UserInputRequest&, const UserInputInvocation&)>;

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

/// Parameters for Session::ui().elicitation().
// NODEJS REFERENCE: nodejs/src/types.ts ElicitationParams
struct ElicitationParams
{
    std::string message;
    ElicitationSchema requested_schema;
};

/// Options for Session::ui().input().
// C# REFERENCE: dotnet/src/Types.cs UiInputOptions
struct UiInputOptions
{
    std::optional<std::string> title;
    std::optional<std::string> description;
    std::optional<int> min_length;
    std::optional<int> max_length;
    /// Semantic format hint: "email", "uri", "date", "date-time".
    std::optional<std::string> format;
    std::optional<std::string> default_value;
};

// =============================================================================
// Session Log Types
// =============================================================================

enum class SessionLogLevel
{
    Info,
    Warning,
    Error
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    SessionLogLevel,
    {
        {SessionLogLevel::Info, "info"},
        {SessionLogLevel::Warning, "warning"},
        {SessionLogLevel::Error, "error"},
    }
)

/// Options for Session::log().
// C# REFERENCE: dotnet/src/Session.cs LogAsync
struct LogOptions
{
    /// Defaults to info on the runtime side.
    std::optional<SessionLogLevel> level;
    /// When true the message is not persisted to the session event log.
    std::optional<bool> ephemeral;
    /// URL the user can open for more details.
    std::optional<std::string> url;
};

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
    /// Runtime ID of the pending `exit_plan_mode.requested` request. Absent when
    /// the runtime predates the field.
    std::optional<std::string> request_id;
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
    if (j.contains("requestId") && j["requestId"].is_string())
        r.request_id = j.at("requestId").get<std::string>();
}

inline void to_json(json& j, const ExitPlanModeRequest& r)
{
    j = json{{"summary", r.summary}, {"recommendedAction", r.recommended_action}};
    if (r.plan_content)
        j["planContent"] = *r.plan_content;
    if (!r.actions.empty())
        j["actions"] = r.actions;
    if (r.request_id)
        j["requestId"] = *r.request_id;
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

/// Recovery metadata attached when the switch request is an auto-mode recovery consent.
// NODEJS REFERENCE: nodejs/src/types.ts AutoModeRecoveryContext
struct AutoModeRecoveryContext
{
    std::string request_id;
    std::string original_model;
    bool allow_always = false;
    std::optional<std::string> interaction_id;
    std::optional<std::string> originating_message_id;
};

inline void from_json(const json& j, AutoModeRecoveryContext& c)
{
    j.at("requestId").get_to(c.request_id);
    j.at("originalModel").get_to(c.original_model);
    if (j.contains("allowAlways") && j["allowAlways"].is_boolean())
        c.allow_always = j["allowAlways"].get<bool>();
    if (j.contains("interactionId") && j["interactionId"].is_string())
        c.interaction_id = j["interactionId"].get<std::string>();
    if (j.contains("originatingMessageId") && j["originatingMessageId"].is_string())
        c.originating_message_id = j["originatingMessageId"].get<std::string>();
}

inline void to_json(json& j, const AutoModeRecoveryContext& c)
{
    j = json{
        {"requestId", c.request_id},
        {"originalModel", c.original_model},
        {"allowAlways", c.allow_always},
    };
    if (c.interaction_id)
        j["interactionId"] = *c.interaction_id;
    if (c.originating_message_id)
        j["originatingMessageId"] = *c.originating_message_id;
}

/// Request to switch to auto mode after a rate limit
struct AutoModeSwitchRequest
{
    std::optional<std::string> error_code;
    std::optional<double> retry_after_seconds;
    /// Present when the request is an auto-mode recovery consent.
    std::optional<AutoModeRecoveryContext> auto_mode_recovery;
};

inline void from_json(const json& j, AutoModeSwitchRequest& r)
{
    if (j.contains("errorCode") && !j["errorCode"].is_null())
        r.error_code = j.at("errorCode").get<std::string>();
    if (j.contains("retryAfterSeconds") && !j["retryAfterSeconds"].is_null())
        r.retry_after_seconds = j.at("retryAfterSeconds").get<double>();
    if (j.contains("autoModeRecovery") && j["autoModeRecovery"].is_object())
        r.auto_mode_recovery = j.at("autoModeRecovery").get<AutoModeRecoveryContext>();
}

inline void to_json(json& j, const AutoModeSwitchRequest& r)
{
    j = json::object();
    if (r.error_code)
        j["errorCode"] = *r.error_code;
    if (r.retry_after_seconds)
        j["retryAfterSeconds"] = *r.retry_after_seconds;
    if (r.auto_mode_recovery)
        j["autoModeRecovery"] = *r.auto_mode_recovery;
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
using AutoModeSwitchHandler = std::function<
    AutoModeSwitchResponse(const AutoModeSwitchRequest&, const AutoModeSwitchInvocation&)>;

// =============================================================================
// Hook Handler Types
// =============================================================================

/// Context for a hook invocation
struct HookInvocation
{
    std::string session_id;
};

inline void parse_hook_base(
    const json& j,
    int64_t& timestamp,
    std::optional<std::string>& timestamp_iso,
    std::string& cwd,
    std::optional<std::string>& session_id
)
{
    if (j.contains("timestamp"))
    {
        if (j["timestamp"].is_number_integer())
            timestamp = j["timestamp"].get<int64_t>();
        else if (j["timestamp"].is_string())
            timestamp_iso = j["timestamp"].get<std::string>();
    }
    if (j.contains("workingDirectory") && j["workingDirectory"].is_string())
        cwd = j["workingDirectory"].get<std::string>();
    else if (j.contains("cwd") && j["cwd"].is_string())
        cwd = j["cwd"].get<std::string>();
    if (j.contains("sessionId") && j["sessionId"].is_string())
        session_id = j["sessionId"].get<std::string>();
}

/// Sub-agent actor fields carried by every typed hook input. All absent for
/// root-agent hooks and older runtimes.
/// C# REFERENCE: dotnet/src/Types.cs PreToolUseHookInput.AgentId/AgentType/ParentAgentId
struct HookActor
{
    std::optional<std::string> agent_id;
    std::optional<std::string> agent_type;
    std::optional<std::string> parent_agent_id;
};

inline void parse_hook_actor(const json& j, HookActor& a)
{
    if (j.contains("agentId") && j["agentId"].is_string())
        a.agent_id = j["agentId"].get<std::string>();
    if (j.contains("agentType") && j["agentType"].is_string())
        a.agent_type = j["agentType"].get<std::string>();
    if (j.contains("parentAgentId") && j["parentAgentId"].is_string())
        a.parent_agent_id = j["parentAgentId"].get<std::string>();
}

/// Fields shared by every hook input.
// NODEJS REFERENCE: nodejs/src/types.ts BaseHookInput
struct HookInputBase : HookActor
{
    /// Unix epoch milliseconds (wire `timestamp` number).
    int64_t timestamp = 0;
    /// Set instead of `timestamp` when the runtime sends an ISO 8601 string.
    std::optional<std::string> timestamp_iso;
    /// Runtime session ID for the hook. Child tool hooks carry the child session ID;
    /// sub-agent lifecycle hooks carry the parent's, which can differ from
    /// `HookInvocation::session_id`.
    std::optional<std::string> session_id;
    /// Working directory (wire `cwd`).
    std::string cwd;
    /// W3C trace context of the runtime span for this hook, when available.
    std::optional<std::string> traceparent;
    std::optional<std::string> tracestate;
};

inline void parse_hook_input_base(const json& j, HookInputBase& h)
{
    parse_hook_base(j, h.timestamp, h.timestamp_iso, h.cwd, h.session_id);
    parse_hook_actor(j, h);
    if (j.contains("traceparent") && j["traceparent"].is_string())
        h.traceparent = j["traceparent"].get<std::string>();
    if (j.contains("tracestate") && j["tracestate"].is_string())
        h.tracestate = j["tracestate"].get<std::string>();
}

inline std::optional<std::string> optional_hook_string(const json& j, const char* key)
{
    if (j.contains(key) && j[key].is_string())
        return j[key].get<std::string>();
    return std::nullopt;
}

/// Text of the `error` field of an errorOccurred / sessionEnd hook input.
/// The CLI sends an error object (`{name, message, stack}` or `{message}`): its
/// string `message` becomes the text, and an object without one becomes its JSON.
/// A string passes through; anything else is rejected.
/// C# REFERENCE: dotnet/src/HookErrorTextConverter.cs
inline std::string hook_error_text(const json& error)
{
    if (error.is_string())
        return error.get<std::string>();
    if (error.is_object())
    {
        const auto it = error.find("message");
        if (it != error.end() && it->is_string())
            return it->get<std::string>();
        return error.dump();
    }
    throw std::invalid_argument(
        std::string("Expected a string or an error object for a hook error, got ") +
        error.type_name()
    );
}

/// Input for a pre-tool-use hook
struct PreToolUseHookInput : HookInputBase
{
    /// Existing tool-call ID; correlate using (session_id, tool_call_id).
    std::optional<std::string> tool_call_id;
    std::string tool_name;
    std::optional<json> tool_args;
};

inline void from_json(const json& j, PreToolUseHookInput& h)
{
    parse_hook_input_base(j, h);
    h.tool_call_id = optional_hook_string(j, "toolCallId");
    if (j.contains("toolName"))
        j.at("toolName").get_to(h.tool_name);
    if (j.contains("toolArgs") && !j["toolArgs"].is_null())
        h.tool_args = j["toolArgs"];
}

/// Output for a pre-tool-use hook
struct PreToolUseHookOutput
{
    std::optional<std::string> permission_decision; ///< "allow", "deny", or "ask"
    std::optional<std::string> permission_decision_reason;
    std::optional<json> modified_args;
    std::optional<std::string> additional_context;
    std::optional<bool> suppress_output;
};

inline void to_json(json& j, const PreToolUseHookOutput& h)
{
    j = json::object();
    if (h.permission_decision)
        j["permissionDecision"] = *h.permission_decision;
    if (h.permission_decision_reason)
        j["permissionDecisionReason"] = *h.permission_decision_reason;
    if (h.modified_args)
        j["modifiedArgs"] = *h.modified_args;
    if (h.additional_context)
        j["additionalContext"] = *h.additional_context;
    if (h.suppress_output)
        j["suppressOutput"] = *h.suppress_output;
}

using PreToolUseHandler = std::function<
    std::optional<PreToolUseHookOutput>(const PreToolUseHookInput&, const HookInvocation&)>;

/// Input for a post-tool-use hook
struct PostToolUseHookInput : HookInputBase
{
    /// Existing tool-call ID; correlate using (session_id, tool_call_id).
    std::optional<std::string> tool_call_id;
    std::string tool_name;
    std::optional<json> tool_args;
    std::optional<json> tool_result;
};

inline void from_json(const json& j, PostToolUseHookInput& h)
{
    parse_hook_input_base(j, h);
    h.tool_call_id = optional_hook_string(j, "toolCallId");
    if (j.contains("toolName"))
        j.at("toolName").get_to(h.tool_name);
    if (j.contains("toolArgs") && !j["toolArgs"].is_null())
        h.tool_args = j["toolArgs"];
    if (j.contains("toolResult") && !j["toolResult"].is_null())
        h.tool_result = j["toolResult"];
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
    if (h.modified_result)
        j["modifiedResult"] = *h.modified_result;
    if (h.additional_context)
        j["additionalContext"] = *h.additional_context;
    if (h.suppress_output)
        j["suppressOutput"] = *h.suppress_output;
}

using PostToolUseHandler = std::function<
    std::optional<PostToolUseHookOutput>(const PostToolUseHookInput&, const HookInvocation&)>;

/// Input for a user-prompt-submitted hook
struct UserPromptSubmittedHookInput : HookInputBase
{
    std::string prompt;
};

inline void from_json(const json& j, UserPromptSubmittedHookInput& h)
{
    parse_hook_input_base(j, h);
    if (j.contains("prompt"))
        j.at("prompt").get_to(h.prompt);
}

/// Output for a user-prompt-submitted hook
struct UserPromptSubmittedHookOutput
{
    std::optional<std::string> modified_prompt;
    std::optional<std::string> additional_context;
    std::optional<bool> suppress_output;
    /// "block" refuses the prompt: no model turn runs, no user message is recorded,
    /// and the session emits a `session.warning` (`policy_blocked`) carrying `reason`.
    std::optional<std::string> decision;
    /// Why the prompt was blocked.
    std::optional<std::string> reason;
};

inline void to_json(json& j, const UserPromptSubmittedHookOutput& h)
{
    j = json::object();
    if (h.modified_prompt)
        j["modifiedPrompt"] = *h.modified_prompt;
    if (h.additional_context)
        j["additionalContext"] = *h.additional_context;
    if (h.suppress_output)
        j["suppressOutput"] = *h.suppress_output;
    if (h.decision)
        j["decision"] = *h.decision;
    if (h.reason)
        j["reason"] = *h.reason;
}

using UserPromptSubmittedHandler = std::function<std::optional<
    UserPromptSubmittedHookOutput>(const UserPromptSubmittedHookInput&, const HookInvocation&)>;

/// Input for a session-start hook
struct SessionStartHookInput : HookInputBase
{
    std::string source; ///< "startup", "resume", or "new"
    std::optional<std::string> initial_prompt;
};

inline void from_json(const json& j, SessionStartHookInput& h)
{
    parse_hook_input_base(j, h);
    if (j.contains("source"))
        j.at("source").get_to(h.source);
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
    if (h.additional_context)
        j["additionalContext"] = *h.additional_context;
    if (h.modified_config)
        j["modifiedConfig"] = *h.modified_config;
}

using SessionStartHandler = std::function<
    std::optional<SessionStartHookOutput>(const SessionStartHookInput&, const HookInvocation&)>;

/// Input for a session-end hook
struct SessionEndHookInput : HookInputBase
{
    std::string reason; ///< "complete", "error", "abort", "timeout", or "user_exit"
    std::optional<std::string> final_message;
    std::optional<std::string> error;
};

inline void from_json(const json& j, SessionEndHookInput& h)
{
    parse_hook_input_base(j, h);
    if (j.contains("reason"))
        j.at("reason").get_to(h.reason);
    if (j.contains("finalMessage") && !j["finalMessage"].is_null())
        h.final_message = j.at("finalMessage").get<std::string>();
    if (j.contains("error") && !j["error"].is_null())
        h.error = hook_error_text(j.at("error"));
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
    if (h.suppress_output)
        j["suppressOutput"] = *h.suppress_output;
    if (h.cleanup_actions)
        j["cleanupActions"] = *h.cleanup_actions;
    if (h.session_summary)
        j["sessionSummary"] = *h.session_summary;
}

using SessionEndHandler = std::function<
    std::optional<SessionEndHookOutput>(const SessionEndHookInput&, const HookInvocation&)>;

/// Input for an error-occurred hook
struct ErrorOccurredHookInput : HookInputBase
{
    std::string error;
    std::string error_context; ///< "model_call", "tool_execution", "system", or "user_input"
    bool recoverable = false;
};

inline void from_json(const json& j, ErrorOccurredHookInput& h)
{
    parse_hook_input_base(j, h);
    if (j.contains("error") && !j["error"].is_null())
        h.error = hook_error_text(j.at("error"));
    if (j.contains("errorContext"))
        j.at("errorContext").get_to(h.error_context);
    if (j.contains("recoverable"))
        j.at("recoverable").get_to(h.recoverable);
}

/// Output for an error-occurred hook
struct ErrorOccurredHookOutput
{
    std::optional<bool> suppress_output;
    std::optional<std::string> error_handling; ///< "retry", "skip", or "abort"
    std::optional<int> retry_count;
    std::optional<std::string> user_notification;
};

inline void to_json(json& j, const ErrorOccurredHookOutput& h)
{
    j = json::object();
    if (h.suppress_output)
        j["suppressOutput"] = *h.suppress_output;
    if (h.error_handling)
        j["errorHandling"] = *h.error_handling;
    if (h.retry_count)
        j["retryCount"] = *h.retry_count;
    if (h.user_notification)
        j["userNotification"] = *h.user_notification;
}

using ErrorOccurredHandler = std::function<
    std::optional<ErrorOccurredHookOutput>(const ErrorOccurredHookInput&, const HookInvocation&)>;

/// Input for a pre-MCP-tool-call hook
// NODEJS REFERENCE: nodejs/src/types.ts PreMcpToolCallHookInput
struct PreMcpToolCallHookInput : HookInputBase
{
    std::optional<std::string> tool_call_id;
    std::string server_name;
    std::string tool_name;
    std::optional<json> arguments;
    /// The outgoing request's `_meta` object.
    std::optional<json> meta;
};

inline void from_json(const json& j, PreMcpToolCallHookInput& h)
{
    parse_hook_input_base(j, h);
    h.tool_call_id = optional_hook_string(j, "toolCallId");
    if (j.contains("serverName"))
        j.at("serverName").get_to(h.server_name);
    if (j.contains("toolName"))
        j.at("toolName").get_to(h.tool_name);
    if (j.contains("arguments") && !j["arguments"].is_null())
        h.arguments = j["arguments"];
    if (j.contains("_meta") && !j["_meta"].is_null())
        h.meta = j["_meta"];
}

/// Output for a pre-MCP-tool-call hook.
/// `meta_to_use` controls the outgoing MCP request `_meta`:
/// - nullopt: preserve the current request `_meta`;
/// - an object: use it as the request `_meta`;
/// - `json(nullptr)`: drop hook-controlled `_meta`. The runtime still sends its own
///   `com.github.copilot/toolCallId` and `com.github.copilot/turnId` keys (the latter
///   only when the turn was started with a `client_turn_id`).
struct PreMcpToolCallHookOutput
{
    std::optional<json> meta_to_use;
};

inline void to_json(json& j, const PreMcpToolCallHookOutput& h)
{
    j = json::object();
    if (h.meta_to_use)
        j["metaToUse"] = *h.meta_to_use;
}

using PreMcpToolCallHandler = std::function<
    std::optional<PreMcpToolCallHookOutput>(const PreMcpToolCallHookInput&, const HookInvocation&)>;

/// Input for a post-tool-use-failure hook. The runtime forwards only the failure
/// message (`error`), not the full tool result.
// NODEJS REFERENCE: nodejs/src/types.ts PostToolUseFailureHookInput
struct PostToolUseFailureHookInput : HookInputBase
{
    std::optional<std::string> tool_call_id;
    std::string tool_name;
    std::optional<json> tool_args;
    std::string error;
};

inline void from_json(const json& j, PostToolUseFailureHookInput& h)
{
    parse_hook_input_base(j, h);
    h.tool_call_id = optional_hook_string(j, "toolCallId");
    if (j.contains("toolName"))
        j.at("toolName").get_to(h.tool_name);
    if (j.contains("toolArgs") && !j["toolArgs"].is_null())
        h.tool_args = j["toolArgs"];
    if (j.contains("error") && j["error"].is_string())
        h.error = j["error"].get<std::string>();
}

/// Output for a post-tool-use-failure hook. Only `additional_context` is honored; it
/// is appended as hidden guidance next to the failed tool result.
struct PostToolUseFailureHookOutput
{
    std::optional<std::string> additional_context;
};

inline void to_json(json& j, const PostToolUseFailureHookOutput& h)
{
    j = json::object();
    if (h.additional_context)
        j["additionalContext"] = *h.additional_context;
}

/// Fires only for `"failure"` tool results; `on_post_tool_use` fires only for successes.
using PostToolUseFailureHandler = std::function<std::optional<
    PostToolUseFailureHookOutput>(const PostToolUseFailureHookInput&, const HookInvocation&)>;

/// Input for a user-prompt-transformed hook: runs after the runtime adds generated
/// context to the prompt, before it is stored or sent to the model.
// NODEJS REFERENCE: nodejs/src/types.ts UserPromptTransformedHookInput
struct UserPromptTransformedHookInput : HookInputBase
{
    std::string prompt;
    std::string transformed_prompt;
};

inline void from_json(const json& j, UserPromptTransformedHookInput& h)
{
    parse_hook_input_base(j, h);
    if (j.contains("prompt"))
        j.at("prompt").get_to(h.prompt);
    if (j.contains("transformedPrompt"))
        j.at("transformedPrompt").get_to(h.transformed_prompt);
}

struct UserPromptTransformedHookOutput
{
    std::optional<std::string> modified_transformed_prompt;
};

inline void to_json(json& j, const UserPromptTransformedHookOutput& h)
{
    j = json::object();
    if (h.modified_transformed_prompt)
        j["modifiedTransformedPrompt"] = *h.modified_transformed_prompt;
}

using UserPromptTransformedHandler = std::function<std::optional<
    UserPromptTransformedHookOutput>(const UserPromptTransformedHookInput&, const HookInvocation&)>;

/// Input for an agent-stop hook: the top-level agent reached a natural terminal stop.
// NODEJS REFERENCE: nodejs/src/types.ts AgentStopHookInput
struct AgentStopHookInput : HookInputBase
{
    /// Why the agent stopped (for example, "end_turn").
    std::optional<std::string> stop_reason;
    std::optional<std::string> transcript_path;
    /// True when this stop re-enters after a previous `block` decision.
    std::optional<bool> stop_hook_active;
};

inline void from_json(const json& j, AgentStopHookInput& h)
{
    parse_hook_input_base(j, h);
    h.stop_reason = optional_hook_string(j, "stopReason");
    h.transcript_path = optional_hook_string(j, "transcriptPath");
    // NODEJS REFERENCE: session.ts deserializeHookInput (stop_hook_active -> stopHookActive)
    for (const char* key : {"stop_hook_active", "stopHookActive"})
    {
        if (j.contains(key) && j[key].is_boolean())
            h.stop_hook_active = j[key].get<bool>();
    }
}

/// `decision = "block"` keeps the agent running with `reason` enqueued as a follow-up
/// user message. The runtime caps consecutive blocks.
struct AgentStopHookOutput
{
    std::optional<std::string> decision;
    std::optional<std::string> reason;
};

inline void to_json(json& j, const AgentStopHookOutput& h)
{
    j = json::object();
    if (h.decision)
        j["decision"] = *h.decision;
    if (h.reason)
        j["reason"] = *h.reason;
}

using AgentStopHandler = std::function<
    std::optional<AgentStopHookOutput>(const AgentStopHookInput&, const HookInvocation&)>;

/// Input for the hook fired before a sub-agent's first turn. Session metadata
/// belongs to the parent session.
// NODEJS REFERENCE: nodejs/src/types.ts SubagentStartHookInput
struct SubagentStartHookInput : HookInputBase
{
    std::string transcript_path;
    std::string agent_name;
    std::optional<std::string> agent_display_name;
    std::optional<std::string> agent_description;
};

inline void parse_subagent_start_fields(const json& j, SubagentStartHookInput& h)
{
    parse_hook_input_base(j, h);
    if (j.contains("transcriptPath") && j["transcriptPath"].is_string())
        h.transcript_path = j["transcriptPath"].get<std::string>();
    if (j.contains("agentName") && j["agentName"].is_string())
        h.agent_name = j["agentName"].get<std::string>();
    h.agent_display_name = optional_hook_string(j, "agentDisplayName");
    h.agent_description = optional_hook_string(j, "agentDescription");
}

inline void from_json(const json& j, SubagentStartHookInput& h)
{
    parse_subagent_start_fields(j, h);
}

/// Context prepended to the child's initial prompt.
struct SubagentStartHookOutput
{
    std::optional<std::string> additional_context;
};

inline void to_json(json& j, const SubagentStartHookOutput& h)
{
    j = json::object();
    if (h.additional_context)
        j["additionalContext"] = *h.additional_context;
}

using SubagentStartHandler = std::function<
    std::optional<SubagentStartHookOutput>(const SubagentStartHookInput&, const HookInvocation&)>;

/// Input for the hook fired after a sub-agent completes a turn. `response` is the
/// child's last assistant message before any hook rewrite.
// NODEJS REFERENCE: nodejs/src/types.ts SubagentStopHookInput
struct SubagentStopHookInput : SubagentStartHookInput
{
    /// "end_turn".
    std::string stop_reason;
    std::string response;
};

inline void from_json(const json& j, SubagentStopHookInput& h)
{
    parse_subagent_start_fields(j, h);
    if (j.contains("stopReason") && j["stopReason"].is_string())
        h.stop_reason = j["stopReason"].get<std::string>();
    if (j.contains("response") && j["response"].is_string())
        h.response = j["response"].get<std::string>();
}

/// `decision = "block"` with a nonempty `reason` continues the child; otherwise
/// `modified_response` replaces the response reported to the parent. A valid block
/// takes precedence over the rewrite. Other decision values fail the sub-agent.
struct SubagentStopHookOutput
{
    std::optional<std::string> decision; ///< "block" or "allow"
    std::optional<std::string> reason;
    std::optional<std::string> modified_response;
};

inline void to_json(json& j, const SubagentStopHookOutput& h)
{
    j = json::object();
    if (h.decision)
        j["decision"] = *h.decision;
    if (h.reason)
        j["reason"] = *h.reason;
    if (h.modified_response)
        j["modifiedResponse"] = *h.modified_response;
}

using SubagentStopHandler = std::function<
    std::optional<SubagentStopHookOutput>(const SubagentStopHookInput&, const HookInvocation&)>;

/// Hook handlers configuration for a session
// NODEJS REFERENCE: nodejs/src/types.ts SessionHooks
struct SessionHooks
{
    std::optional<PreToolUseHandler> on_pre_tool_use;
    std::optional<PreMcpToolCallHandler> on_pre_mcp_tool_call;
    /// Fires only for successful tool results; see `on_post_tool_use_failure`.
    std::optional<PostToolUseHandler> on_post_tool_use;
    std::optional<PostToolUseFailureHandler> on_post_tool_use_failure;
    std::optional<UserPromptSubmittedHandler> on_user_prompt_submitted;
    std::optional<UserPromptTransformedHandler> on_user_prompt_transformed;
    std::optional<SessionStartHandler> on_session_start;
    std::optional<SessionEndHandler> on_session_end;
    std::optional<ErrorOccurredHandler> on_error_occurred;
    std::optional<AgentStopHandler> on_agent_stop;
    std::optional<SubagentStartHandler> on_subagent_start;
    std::optional<SubagentStopHandler> on_subagent_stop;

    /// Returns true if any hook handler is registered
    bool has_any() const
    {
        return on_pre_tool_use || on_pre_mcp_tool_call || on_post_tool_use ||
               on_post_tool_use_failure || on_user_prompt_submitted ||
               on_user_prompt_transformed || on_session_start || on_session_end ||
               on_error_occurred || on_agent_stop || on_subagent_start || on_subagent_stop;
    }
};

// =============================================================================
// Configuration Types
// =============================================================================

/// Rewrites a rendered system prompt section. Receives the current content and
/// returns the replacement. A throwing callback leaves the section unchanged.
// NODEJS REFERENCE: nodejs/src/types.ts SectionTransformFn
using SectionTransformFn = std::function<std::string(const std::string& content)>;

/// Override operation for a single system prompt section
struct SectionOverride
{
    SectionOverrideAction action = SectionOverrideAction::Replace;
    std::optional<std::string> content;
    /// When set, the section is sent as `action: "transform"` and the runtime calls
    /// back through `systemMessage.transform`; `action` and `content` are ignored.
    SectionTransformFn transform;
};

inline void to_json(json& j, const SectionOverride& c)
{
    if (c.transform)
    {
        j = json{{"action", SectionOverrideAction::Transform}};
        return;
    }
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

/// Arguments passed to a BearerTokenProvider.
// NODEJS REFERENCE: nodejs/src/types.ts ProviderTokenArgs
struct ProviderTokenArgs
{
    std::string provider_name;
    std::string session_id;
};

/// Supplies a bearer token for a named provider on demand. Never serialized: the
/// SDK strips it and sends `hasBearerTokenProvider: true`; the runtime calls back
/// through `providerToken.getToken` when it needs a token.
using BearerTokenProvider = std::function<std::string(const ProviderTokenArgs&)>;

/// A named model-provider connection (BYOK).
// NODEJS REFERENCE: nodejs/src/types.ts NamedProviderConfig
struct NamedProviderConfig
{
    std::string name;
    /// "openai" | "azure" | "anthropic"
    std::optional<std::string> type;
    std::optional<json> local_model;
    /// "completions" | "responses"
    std::optional<std::string> wire_api;
    /// "http" | "websockets"
    std::optional<std::string> transport;
    std::optional<std::string> model_provider;
    std::string base_url;
    std::optional<std::string> api_key;
    std::optional<std::string> api_key_command;
    std::optional<std::string> bearer_token;
    BearerTokenProvider bearer_token_provider;
    std::optional<AzureOptions> azure;
    std::optional<std::map<std::string, std::string>> headers;
};

inline void to_json(json& j, const NamedProviderConfig& c)
{
    j = json{{"name", c.name}};
    if (c.type)
        j["type"] = *c.type;
    if (c.local_model)
        j["localModel"] = *c.local_model;
    if (c.wire_api)
        j["wireApi"] = *c.wire_api;
    if (c.transport)
        j["transport"] = *c.transport;
    if (c.model_provider)
        j["modelProvider"] = *c.model_provider;
    j["baseUrl"] = c.base_url;
    if (c.api_key)
        j["apiKey"] = *c.api_key;
    if (c.api_key_command)
        j["apiKeyCommand"] = *c.api_key_command;
    if (c.bearer_token)
        j["bearerToken"] = *c.bearer_token;
    if (c.azure)
        j["azure"] = *c.azure;
    if (c.headers)
        j["headers"] = *c.headers;
}

inline void from_json(const json& j, NamedProviderConfig& c)
{
    j.at("name").get_to(c.name);
    j.at("baseUrl").get_to(c.base_url);
    auto opt = [&j](const char* key, std::optional<std::string>& out)
    {
        if (j.contains(key) && !j[key].is_null())
            out = j[key].get<std::string>();
    };
    opt("type", c.type);
    opt("wireApi", c.wire_api);
    opt("transport", c.transport);
    opt("modelProvider", c.model_provider);
    opt("apiKey", c.api_key);
    opt("apiKeyCommand", c.api_key_command);
    opt("bearerToken", c.bearer_token);
    if (j.contains("localModel") && !j["localModel"].is_null())
        c.local_model = j["localModel"];
    if (j.contains("azure") && !j["azure"].is_null())
        c.azure = j["azure"].get<AzureOptions>();
    if (j.contains("headers") && !j["headers"].is_null())
        c.headers = j["headers"].get<std::map<std::string, std::string>>();
}

/// Per-property model capability overrides, deep-merged over runtime defaults.
// C# REFERENCE: dotnet/src/Generated/Rpc.cs ModelCapabilitiesOverride
// NODEJS REFERENCE: nodejs/src/types.ts ModelCapabilitiesOverride
struct ModelCapabilitiesOverrideLimitsVision
{
    std::optional<std::vector<std::string>> supported_media_types;
    std::optional<int64_t> max_prompt_images;
    std::optional<int64_t> max_prompt_image_size;
};

struct ModelCapabilitiesOverrideLimits
{
    std::optional<int64_t> max_prompt_tokens;
    std::optional<int64_t> max_output_tokens;
    std::optional<int64_t> max_context_window_tokens;
    std::optional<ModelCapabilitiesOverrideLimitsVision> vision;
};

struct ModelCapabilitiesOverrideSupports
{
    std::optional<bool> vision;
    std::optional<bool> tool_calls;
    std::optional<bool> reasoning_effort;
    /// "unsupported" | "optional" | "required" | "adaptive_only"
    std::optional<std::string> adaptive_thinking;
};

struct ModelCapabilitiesOverride
{
    std::optional<ModelCapabilitiesOverrideSupports> supports;
    std::optional<ModelCapabilitiesOverrideLimits> limits;
};

namespace detail
{
template <typename T>
void put_optional(json& j, const char* key, const std::optional<T>& value)
{
    if (value)
        j[key] = *value;
}

template <typename T>
void get_optional(const json& j, const char* key, std::optional<T>& value)
{
    if (j.contains(key) && !j.at(key).is_null())
        value = j.at(key).get<T>();
}
} // namespace detail

inline void to_json(json& j, const ModelCapabilitiesOverrideLimitsVision& v)
{
    j = json::object();
    detail::put_optional(j, "supported_media_types", v.supported_media_types);
    detail::put_optional(j, "max_prompt_images", v.max_prompt_images);
    detail::put_optional(j, "max_prompt_image_size", v.max_prompt_image_size);
}

inline void from_json(const json& j, ModelCapabilitiesOverrideLimitsVision& v)
{
    detail::get_optional(j, "supported_media_types", v.supported_media_types);
    detail::get_optional(j, "max_prompt_images", v.max_prompt_images);
    detail::get_optional(j, "max_prompt_image_size", v.max_prompt_image_size);
}

inline void to_json(json& j, const ModelCapabilitiesOverrideLimits& l)
{
    j = json::object();
    detail::put_optional(j, "max_prompt_tokens", l.max_prompt_tokens);
    detail::put_optional(j, "max_output_tokens", l.max_output_tokens);
    detail::put_optional(j, "max_context_window_tokens", l.max_context_window_tokens);
    detail::put_optional(j, "vision", l.vision);
}

inline void from_json(const json& j, ModelCapabilitiesOverrideLimits& l)
{
    detail::get_optional(j, "max_prompt_tokens", l.max_prompt_tokens);
    detail::get_optional(j, "max_output_tokens", l.max_output_tokens);
    detail::get_optional(j, "max_context_window_tokens", l.max_context_window_tokens);
    detail::get_optional(j, "vision", l.vision);
}

inline void to_json(json& j, const ModelCapabilitiesOverrideSupports& s)
{
    j = json::object();
    detail::put_optional(j, "vision", s.vision);
    detail::put_optional(j, "toolCalls", s.tool_calls);
    detail::put_optional(j, "reasoningEffort", s.reasoning_effort);
    detail::put_optional(j, "adaptive_thinking", s.adaptive_thinking);
}

inline void from_json(const json& j, ModelCapabilitiesOverrideSupports& s)
{
    detail::get_optional(j, "vision", s.vision);
    detail::get_optional(j, "toolCalls", s.tool_calls);
    detail::get_optional(j, "reasoningEffort", s.reasoning_effort);
    detail::get_optional(j, "adaptive_thinking", s.adaptive_thinking);
}

inline void to_json(json& j, const ModelCapabilitiesOverride& c)
{
    j = json::object();
    detail::put_optional(j, "supports", c.supports);
    detail::put_optional(j, "limits", c.limits);
}

inline void from_json(const json& j, ModelCapabilitiesOverride& c)
{
    detail::get_optional(j, "supports", c.supports);
    detail::get_optional(j, "limits", c.limits);
}

/// A model registered against a named provider.
// NODEJS REFERENCE: nodejs/src/types.ts ProviderModelConfig
struct ProviderModelConfig
{
    std::optional<json> auto_local;
    std::optional<json> metadata;
    std::string id;
    std::string provider;
    std::optional<std::string> wire_model;
    std::optional<std::string> model_id;
    std::optional<std::string> name;
    std::optional<int64_t> max_prompt_tokens;
    std::optional<int64_t> max_context_window_tokens;
    std::optional<int64_t> max_output_tokens;
    std::optional<ModelCapabilitiesOverride> capabilities;
};

inline void to_json(json& j, const ProviderModelConfig& c)
{
    j = json::object();
    if (c.auto_local)
        j["autoLocal"] = *c.auto_local;
    if (c.metadata)
        j["metadata"] = *c.metadata;
    j["id"] = c.id;
    j["provider"] = c.provider;
    if (c.wire_model)
        j["wireModel"] = *c.wire_model;
    if (c.model_id)
        j["modelId"] = *c.model_id;
    if (c.name)
        j["name"] = *c.name;
    if (c.max_prompt_tokens)
        j["maxPromptTokens"] = *c.max_prompt_tokens;
    if (c.max_context_window_tokens)
        j["maxContextWindowTokens"] = *c.max_context_window_tokens;
    if (c.max_output_tokens)
        j["maxOutputTokens"] = *c.max_output_tokens;
    if (c.capabilities)
        j["capabilities"] = *c.capabilities;
}

inline void from_json(const json& j, ProviderModelConfig& c)
{
    j.at("id").get_to(c.id);
    j.at("provider").get_to(c.provider);
    auto opt_str = [&j](const char* key, std::optional<std::string>& out)
    {
        if (j.contains(key) && !j[key].is_null())
            out = j[key].get<std::string>();
    };
    auto opt_int = [&j](const char* key, std::optional<int64_t>& out)
    {
        if (j.contains(key) && !j[key].is_null())
            out = j[key].get<int64_t>();
    };
    auto opt_json = [&j](const char* key, std::optional<json>& out)
    {
        if (j.contains(key) && !j[key].is_null())
            out = j[key];
    };
    opt_json("autoLocal", c.auto_local);
    opt_json("metadata", c.metadata);
    opt_str("wireModel", c.wire_model);
    opt_str("modelId", c.model_id);
    opt_str("name", c.name);
    opt_int("maxPromptTokens", c.max_prompt_tokens);
    opt_int("maxContextWindowTokens", c.max_context_window_tokens);
    opt_int("maxOutputTokens", c.max_output_tokens);
    detail::get_optional(j, "capabilities", c.capabilities);
}

/// BYOK helpers that read COPILOT_SDK_BYOK_* environment variables.
struct ProviderConfig
{
    static constexpr const char* ENV_API_KEY = "COPILOT_SDK_BYOK_API_KEY";
    static constexpr const char* ENV_BASE_URL = "COPILOT_SDK_BYOK_BASE_URL";
    static constexpr const char* ENV_PROVIDER_TYPE = "COPILOT_SDK_BYOK_PROVIDER_TYPE";
    static constexpr const char* ENV_MODEL = "COPILOT_SDK_BYOK_MODEL";

    /// Provider name used by from_env(); pair with SessionInferenceOptions::default_provider.
    static constexpr const char* ENV_PROVIDER_NAME = "byok";

    /// True when COPILOT_SDK_BYOK_API_KEY is set and non-empty.
    static bool is_env_configured()
    {
        return detail::non_empty_env_value(ENV_API_KEY).has_value();
    }

    /// Named provider "byok" from the environment, or nullopt when no API key is set.
    /// Base URL defaults to https://api.openai.com/v1 and type to "openai".
    static std::optional<NamedProviderConfig> from_env()
    {
        auto key = detail::non_empty_env_value(ENV_API_KEY);
        if (!key)
            return std::nullopt;

        NamedProviderConfig config;
        config.name = ENV_PROVIDER_NAME;
        config.api_key = std::move(*key);
        config.base_url =
            detail::non_empty_env_value(ENV_BASE_URL).value_or("https://api.openai.com/v1");
        config.type = detail::non_empty_env_value(ENV_PROVIDER_TYPE).value_or("openai");
        return config;
    }

    /// Model from COPILOT_SDK_BYOK_MODEL, or nullopt when unset.
    static std::optional<std::string> model_from_env()
    {
        return detail::non_empty_env_value(ENV_MODEL);
    }
};

// =============================================================================
// MCP Server Configuration
// =============================================================================

// NODEJS REFERENCE: nodejs/src/types.ts MCPStdioServerConfig / MCPHTTPServerConfig
// C# REFERENCE: dotnet/src/Types.cs McpStdioServerConfig / McpHttpServerConfig

namespace detail
{
inline void mcp_base_to_json(
    json& j, const std::optional<std::vector<std::string>>& tools, const std::optional<int>& timeout
)
{
    if (tools)
        j["tools"] = *tools;
    if (timeout)
        j["timeout"] = *timeout;
}

inline void mcp_base_from_json(
    const json& j, std::optional<std::vector<std::string>>& tools, std::optional<int>& timeout
)
{
    if (j.contains("tools") && j["tools"].is_array())
        tools = j["tools"].get<std::vector<std::string>>();
    if (j.contains("timeout") && j["timeout"].is_number())
        timeout = j["timeout"].get<int>();
}
} // namespace detail

/// Configuration for a local/stdio MCP server
struct McpLocalServerConfig
{
    /// Tools to include; unset or {"*"} includes all, empty includes none.
    std::optional<std::vector<std::string>> tools;
    /// "local" | "stdio" (runtime default: stdio)
    std::optional<std::string> type;
    /// Tool-call timeout in milliseconds.
    std::optional<int> timeout;
    std::string command;
    std::optional<std::vector<std::string>> args;
    std::optional<std::map<std::string, std::string>> env;
    /// Sent as "cwd" on the wire.
    std::optional<std::string> working_directory;
};

inline void to_json(json& j, const McpLocalServerConfig& c)
{
    j = json::object();
    detail::mcp_base_to_json(j, c.tools, c.timeout);
    if (c.type)
        j["type"] = *c.type;
    j["command"] = c.command;
    if (c.args)
        j["args"] = *c.args;
    if (c.env)
        j["env"] = *c.env;
    if (c.working_directory)
        j["cwd"] = *c.working_directory;
}

inline void from_json(const json& j, McpLocalServerConfig& c)
{
    detail::mcp_base_from_json(j, c.tools, c.timeout);
    j.at("command").get_to(c.command);
    if (j.contains("type") && j["type"].is_string())
        c.type = j["type"].get<std::string>();
    if (j.contains("args") && j["args"].is_array())
        c.args = j["args"].get<std::vector<std::string>>();
    if (j.contains("env") && j["env"].is_object())
        c.env = j["env"].get<std::map<std::string, std::string>>();
    if (j.contains("cwd") && j["cwd"].is_string())
        c.working_directory = j["cwd"].get<std::string>();
}

/// Configuration for a remote MCP server (HTTP or SSE)
struct McpRemoteServerConfig
{
    /// Tools to include; unset or {"*"} includes all, empty includes none.
    std::optional<std::vector<std::string>> tools;
    /// "http" | "sse"
    std::string type = "http";
    /// Tool-call timeout in milliseconds.
    std::optional<int> timeout;
    std::string url;
    std::optional<std::map<std::string, std::string>> headers;
    std::optional<std::string> oauth_client_id;
    std::optional<bool> oauth_public_client;
    /// "authorization_code" | "client_credentials"
    std::optional<std::string> oauth_grant_type;
    std::optional<std::vector<std::string>> oauth_scopes;
};

inline void to_json(json& j, const McpRemoteServerConfig& c)
{
    j = json::object();
    detail::mcp_base_to_json(j, c.tools, c.timeout);
    j["type"] = c.type;
    j["url"] = c.url;
    if (c.headers)
        j["headers"] = *c.headers;
    if (c.oauth_client_id)
        j["oauthClientId"] = *c.oauth_client_id;
    if (c.oauth_public_client)
        j["oauthPublicClient"] = *c.oauth_public_client;
    if (c.oauth_grant_type)
        j["oauthGrantType"] = *c.oauth_grant_type;
    if (c.oauth_scopes)
        j["oauthScopes"] = *c.oauth_scopes;
}

inline void from_json(const json& j, McpRemoteServerConfig& c)
{
    detail::mcp_base_from_json(j, c.tools, c.timeout);
    if (j.contains("type") && j["type"].is_string())
        c.type = j["type"].get<std::string>();
    j.at("url").get_to(c.url);
    if (j.contains("headers") && j["headers"].is_object())
        c.headers = j["headers"].get<std::map<std::string, std::string>>();
    if (j.contains("oauthClientId") && j["oauthClientId"].is_string())
        c.oauth_client_id = j["oauthClientId"].get<std::string>();
    if (j.contains("oauthPublicClient") && j["oauthPublicClient"].is_boolean())
        c.oauth_public_client = j["oauthPublicClient"].get<bool>();
    if (j.contains("oauthGrantType") && j["oauthGrantType"].is_string())
        c.oauth_grant_type = j["oauthGrantType"].get<std::string>();
    if (j.contains("oauthScopes") && j["oauthScopes"].is_array())
        c.oauth_scopes = j["oauthScopes"].get<std::vector<std::string>>();
}

/// Union type for MCP server configuration
using MCPServerConfig = std::variant<McpLocalServerConfig, McpRemoteServerConfig>;

} // namespace copilot

namespace nlohmann
{
template <> struct adl_serializer<copilot::MCPServerConfig>
{
    static void to_json(json& j, const copilot::MCPServerConfig& config)
    {
        std::visit([&j](const auto& c) { j = c; }, config);
    }

    /// Remote when a url is present or the type is http/sse; local otherwise.
    static void from_json(const json& j, copilot::MCPServerConfig& config)
    {
        const auto type = j.contains("type") && j["type"].is_string() ? j["type"].get<std::string>()
                                                                        : std::string{};
        if (j.contains("url") || type == "http" || type == "sse")
            config = j.get<copilot::McpRemoteServerConfig>();
        else
            config = j.get<copilot::McpLocalServerConfig>();
    }
};
} // namespace nlohmann

namespace copilot
{

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
    std::optional<std::map<std::string, MCPServerConfig>> mcp_servers;
    std::optional<bool> infer;
    std::optional<std::vector<std::string>> skills;
    /// Model identifier for this agent (e.g. "claude-haiku-4.5").
    /// When set, the runtime will attempt to use this model for the agent,
    /// falling back to the parent session model if unavailable.
    std::optional<std::string> model;
    std::optional<std::map<std::string, std::vector<json>>> hooks;
    /// "preferred" (default) | "required"
    std::optional<std::string> model_policy;
    /// Reasoning effort for this agent's model.
    std::optional<std::string> reasoning_effort;
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
    if (c.hooks)
        j["hooks"] = *c.hooks;
    if (c.model_policy)
        j["modelPolicy"] = *c.model_policy;
    if (c.reasoning_effort)
        j["reasoningEffort"] = *c.reasoning_effort;
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
        c.mcp_servers = j.at("mcpServers").get<std::map<std::string, MCPServerConfig>>();
    if (j.contains("infer"))
        c.infer = j.at("infer").get<bool>();
    if (j.contains("skills"))
        c.skills = j.at("skills").get<std::vector<std::string>>();
    if (j.contains("model"))
        c.model = j.at("model").get<std::string>();
    if (j.contains("hooks") && !j["hooks"].is_null())
        c.hooks = j.at("hooks").get<std::map<std::string, std::vector<json>>>();
    if (j.contains("modelPolicy") && !j["modelPolicy"].is_null())
        c.model_policy = j.at("modelPolicy").get<std::string>();
    if (j.contains("reasoningEffort") && j["reasoningEffort"].is_string())
        c.reasoning_effort = j["reasoningEffort"].get<std::string>();
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
    Directory,
    Selection,
    Blob
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    AttachmentType,
    {
        {AttachmentType::File, "file"},
        {AttachmentType::Directory, "directory"},
        {AttachmentType::Selection, "selection"},
        {AttachmentType::Blob, "blob"},
    }
)

/// Attachment item for user messages
struct UserMessageAttachment
{
    AttachmentType type;
    std::string path;
    std::string display_name;
    std::optional<std::string> file_path;
    std::optional<json> selection;
    std::optional<std::string> text;
    std::optional<std::string> data;
    std::optional<std::string> mime_type;
};

inline void to_json(json& j, const UserMessageAttachment& a)
{
    j = json{{"type", a.type}};
    if (a.type == AttachmentType::File || a.type == AttachmentType::Directory)
        j["path"] = a.path;
    if (!a.display_name.empty())
        j["displayName"] = a.display_name;
    if (a.file_path)
        j["filePath"] = *a.file_path;
    if (a.selection)
        j["selection"] = *a.selection;
    if (a.text)
        j["text"] = *a.text;
    if (a.data)
        j["data"] = *a.data;
    if (a.mime_type)
        j["mimeType"] = *a.mime_type;
}

inline void from_json(const json& j, UserMessageAttachment& a)
{
    j.at("type").get_to(a.type);
    if (j.contains("path"))
        j.at("path").get_to(a.path);
    if (j.contains("displayName"))
        j.at("displayName").get_to(a.display_name);
    if (j.contains("filePath"))
        a.file_path = j.at("filePath").get<std::string>();
    if (j.contains("selection"))
        a.selection = j.at("selection");
    if (j.contains("text"))
        a.text = j.at("text").get<std::string>();
    if (j.contains("data"))
        a.data = j.at("data").get<std::string>();
    if (j.contains("mimeType"))
        a.mime_type = j.at("mimeType").get<std::string>();
}

// =============================================================================
// Tool Definition (SDK-side)
// =============================================================================

/// Controls whether a tool can be lazily surfaced through tool search.
enum class ToolDefer
{
    Auto,
    Never
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ToolDefer,
    {
        {ToolDefer::Auto, "auto"},
        {ToolDefer::Never, "never"},
    }
)

/// Builder for source-qualified available/excluded tool filters.
class ToolSet
{
  public:
    ToolSet& add_built_in(const std::string& name)
    {
        add("builtin", name);
        return *this;
    }

    ToolSet& add_built_ins(const std::vector<std::string>& names)
    {
        for (const auto& name : names)
            add("builtin", name);
        return *this;
    }

    ToolSet& add_custom(const std::string& name)
    {
        add("custom", name);
        return *this;
    }

    ToolSet& add_mcp(const std::string& name)
    {
        add("mcp", name);
        return *this;
    }

    const std::vector<std::string>& values() const noexcept
    {
        return values_;
    }
    std::vector<std::string> to_vector() const
    {
        return values_;
    }

  private:
    void add(const std::string& source, const std::string& name)
    {
        if (name.empty())
            throw std::invalid_argument("tool filter name cannot be empty");
        if (name != "*")
        {
            for (unsigned char c : name)
                if (!std::isalnum(c) && c != '_' && c != '-')
                    throw std::invalid_argument(
                        "tool filter names may contain only letters, digits, '_' and '-'"
                    );
        }
        values_.push_back(source + ":" + name);
    }

    std::vector<std::string> values_;
};

/// Tool definition for registration with a session
struct ToolNamespace
{
    std::string name;
    std::optional<std::string> description;
};
inline void to_json(json& j, const ToolNamespace& value)
{
    j = {{"name", value.name}};
    if (value.description)
        j["description"] = *value.description;
}
inline void from_json(const json& j, ToolNamespace& value)
{
    j.at("name").get_to(value.name);
    if (j.contains("description") && !j["description"].is_null())
        value.description = j["description"].get<std::string>();
}

struct Tool
{
    std::string name;
    /// Human-readable display name.
    std::optional<std::string> title;
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

    /// Lazy-loading policy for tool search.
    std::optional<ToolDefer> defer;

    /// Opaque host-defined metadata, preserved on the wire.
    std::optional<json> metadata;

    /// Successful execution terminates the current agent turn.
    bool is_terminal = false;

    /// Repeated calls with the same arguments have no additional effect.
    bool idempotent = false;

    /// Fluent setter — mark this tool as not requiring permission prompts.
    Tool& with_skip_permission(bool value = true) &
    {
        skip_permission = value;
        return *this;
    }
    Tool with_skip_permission(bool value = true) &&
    {
        skip_permission = value;
        return std::move(*this);
    }

    /// Fluent setter — mark this tool as replacing a built-in CLI tool.
    Tool& with_overrides_built_in_tool(bool value = true) &
    {
        overrides_built_in_tool = value;
        return *this;
    }
    Tool with_overrides_built_in_tool(bool value = true) &&
    {
        overrides_built_in_tool = value;
        return std::move(*this);
    }
    std::optional<ToolNamespace> tool_namespace;
    Tool& with_namespace(ToolNamespace value) &
    {
        tool_namespace = std::move(value);
        return *this;
    }
    Tool with_namespace(ToolNamespace value) &&
    {
        tool_namespace = std::move(value);
        return std::move(*this);
    }
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

// C# REFERENCE: dotnet/src/Types.cs ISkillProvider and SessionConfigBase.
struct SkillProviderDescriptor
{
    std::string name;
    std::string description;
    std::optional<bool> user_invocable;
    std::optional<bool> disable_model_invocation;
    std::optional<std::string> argument_hint;
    std::optional<std::string> directory;
};
inline void to_json(json& j, const SkillProviderDescriptor& value)
{
    j = {{"name", value.name}, {"description", value.description}};
    if (value.user_invocable)
        j["userInvocable"] = *value.user_invocable;
    if (value.disable_model_invocation)
        j["disableModelInvocation"] = *value.disable_model_invocation;
    if (value.argument_hint)
        j["argumentHint"] = *value.argument_hint;
    if (value.directory)
        j["directory"] = *value.directory;
}
class SkillProvider
{
  public:
    virtual ~SkillProvider() = default;
    virtual std::vector<SkillProviderDescriptor> list_skills() = 0;
    virtual std::optional<std::string> read_skill(const std::string& name) = 0;
};
enum class WorkingDirectoryTrust
{
    Session,
    User
};
NLOHMANN_JSON_SERIALIZE_ENUM(
    WorkingDirectoryTrust,
    {{WorkingDirectoryTrust::Session, "session"}, {WorkingDirectoryTrust::User, "user"}}
)
struct TranscriptRecoveryReport
{
    std::string planned_backup_path;
    std::vector<int> invalid_line_numbers;
    bool session_start_moved = false;
};
inline void from_json(const json& j, TranscriptRecoveryReport& value)
{
    value.planned_backup_path = j.value("plannedBackupPath", "");
    value.invalid_line_numbers = j.value("invalidLineNumbers", std::vector<int>{});
    value.session_start_moved = j.value("sessionStartMoved", false);
}

// -----------------------------------------------------------------------------
// GitHub token provider (session-scoped)
// -----------------------------------------------------------------------------

/// Why the runtime is asking for a GitHub credential (`GitHubTokenAcquireReason`).
enum class GitHubTokenAcquireReason
{
    Initial, ///< First acquisition for this session.
    Refresh, ///< The previously supplied credential is expiring or expired.
};

/// Inbound GitHub credential request routed to SessionGitHubOptions::token_provider.
// NODEJS REFERENCE: nodejs/src/client.ts handleGitHubTokenRequest
struct GitHubTokenRequest
{
    /// Authentication host the credential is wanted for.
    std::string host;
    /// Absent only before a cloud session has been assigned its id.
    std::optional<std::string> session_id;
    GitHubTokenAcquireReason reason = GitHubTokenAcquireReason::Initial;
};

/// Credential returned by a GitHubTokenProvider (`GitHubTokenAcquireResult`, kind "token").
struct GitHubToken
{
    std::string access_token;
    /// Seconds until expiry. The schema requires at least kMinGitHubTokenExpiresIn so
    /// the value outlives the runtime's one-hour preflight refresh window.
    int64_t expires_in = 0;
    std::optional<std::string> token_type;
};

/// Schema-mandated lower bound for GitHubToken::expires_in.
inline constexpr int64_t kMinGitHubTokenExpiresIn = 3601;

/// Supplies GitHub credentials on demand. Return std::nullopt to decline (marshalled
/// as `{"kind":"cancelled"}`).
using GitHubTokenProvider = std::function<std::optional<GitHubToken>(const GitHubTokenRequest&)>;

// -----------------------------------------------------------------------------
// Memory store
// -----------------------------------------------------------------------------

/// A memory the runtime asks the host to persist.
// NODEJS REFERENCE: nodejs/src/types.ts StoredMemory
struct StoredMemory
{
    std::string session_id;
    std::string subject;
    std::string fact;
    std::string citations;
    std::string reason;
};

inline void from_json(const json& j, StoredMemory& m)
{
    m.session_id = j.value("sessionId", "");
    m.subject = j.value("subject", "");
    m.fact = j.value("fact", "");
    m.citations = j.value("citations", "");
    m.reason = j.value("reason", "");
}

/// Persists a memory. The token is cancelled when the runtime abandons the call.
using MemoryStoreHandler = std::function<void(const StoredMemory&, const CancellationToken&)>;

/// Creates and supervises a separate session for this session's management tools.
/// Use `newSessionId` as the child's exact identity and apply the requested launch
/// options and permission ceiling. Route approvals independently, retain the new
/// session, and dispose it with the host. Do not copy the parent's conversation.
/// Receives a `SessionManagementCreateHostRequest` and returns a
/// `SessionManagementCreateResult` (wire JSON).
// NODEJS REFERENCE: nodejs/src/types.ts SessionCreationHandler
using SessionCreationHandler = std::function<json(const json& request, const CancellationToken&)>;

// NODEJS REFERENCE: nodejs/src/types.ts MemoryConfiguration
struct MemoryConfiguration
{
    bool enabled = false;
    /// "system-prompt" | "user-prompt"
    std::optional<std::string> mode;
    /// Host-side memory store; requires enabled == true. Never serialized.
    MemoryStoreHandler store_memory;
};

inline void to_json(json& j, const MemoryConfiguration& c)
{
    j = json{{"enabled", c.enabled}};
    if (c.mode)
        j["mode"] = *c.mode;
}

inline void from_json(const json& j, MemoryConfiguration& c)
{
    c.enabled = j.value("enabled", false);
    detail::get_optional(j, "mode", c.mode);
}

// -----------------------------------------------------------------------------
// Slash commands
// -----------------------------------------------------------------------------

/// Context passed to a CommandHandler when the user runs a registered slash command.
// NODEJS REFERENCE: nodejs/src/types.ts CommandContext
struct CommandContext
{
    std::string session_id;
    /// Full command text, e.g. "/deploy production".
    std::string command;
    /// Command name without the leading slash.
    std::string command_name;
    /// Raw argument string after the command name.
    std::string args;
};

/// Runs a slash command. Throwing reports the exception message back to the runtime.
using CommandHandler = std::function<void(const CommandContext&)>;

// NODEJS REFERENCE: nodejs/src/types.ts CommandDefinition
struct CommandDefinition
{
    std::string name;
    std::optional<std::string> description;
    CommandHandler handler;
};

// -----------------------------------------------------------------------------
// Shell authentication
// -----------------------------------------------------------------------------

enum class ShellAuthenticationInputKind
{
    Secret,
    Text,
    Confirm
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ShellAuthenticationInputKind,
    {
        {ShellAuthenticationInputKind::Secret, "secret"},
        {ShellAuthenticationInputKind::Text, "text"},
        {ShellAuthenticationInputKind::Confirm, "confirm"},
    }
)

/// A shell command is waiting on interactive authentication input.
// NODEJS REFERENCE: nodejs/src/types.ts ShellAuthenticationRequest
struct ShellAuthenticationRequest
{
    std::string request_id;
    std::string program;
    std::string prompt;
    ShellAuthenticationInputKind input_kind = ShellAuthenticationInputKind::Secret;
    std::string command;
    std::string cwd;
};

struct ShellAuthenticationInvocation
{
    std::string session_id;
    /// Cancelled when the runtime withdraws the request (`shellAuthentication.cancel`).
    CancellationToken cancellation;
};

/// `{action:"accept", value}` or `{action:"cancel"}`.
struct ShellAuthenticationResponse
{
    bool accepted = false;
    std::string value;

    static ShellAuthenticationResponse accept(std::string value)
    {
        return ShellAuthenticationResponse{.accepted = true, .value = std::move(value)};
    }
    static ShellAuthenticationResponse cancel()
    {
        return ShellAuthenticationResponse{};
    }
};

using ShellAuthenticationHandler = std::function<ShellAuthenticationResponse(
    const ShellAuthenticationRequest&, const ShellAuthenticationInvocation&
)>;

// -----------------------------------------------------------------------------
// Typed option payloads
// -----------------------------------------------------------------------------
// Runtime-owned shapes the official SDKs re-export from their generated RPC types.
using AutopilotOptions = generated::api::AutopilotOptions;
using SandboxConfig = generated::api::SandboxConfig;
using SessionLimitsConfig = generated::api::SessionLimitsConfig;
using DiagnosticsConfiguration = generated::api::DiagnosticsConfiguration;
using LifecycleDiagnosticsConfiguration = generated::api::LifecycleDiagnosticsConfiguration;
using HostDiagnostics = generated::api::HostDiagnostics;
using OpenCanvasInstance = generated::api::OpenCanvasInstance;

/// Non-persisted opt-in to image generation (experimental).
// NODEJS REFERENCE: nodejs/src/types.ts ImageGenerationConfig
struct ImageGenerationConfig
{
    std::optional<bool> enabled;
    /// Request a visible "Made with AI" label on generated images.
    std::optional<bool> watermark;
    /// Save generated images under the session-state images folder (runtime default: true).
    std::optional<bool> save_to_disk;
};

inline void to_json(json& j, const ImageGenerationConfig& c)
{
    j = json::object();
    if (c.enabled)
        j["enabled"] = *c.enabled;
    if (c.watermark)
        j["watermark"] = *c.watermark;
    if (c.save_to_disk)
        j["saveToDisk"] = *c.save_to_disk;
}

inline void from_json(const json& j, ImageGenerationConfig& c)
{
    detail::get_optional(j, "enabled", c.enabled);
    detail::get_optional(j, "watermark", c.watermark);
    detail::get_optional(j, "saveToDisk", c.save_to_disk);
}

/// Override for the runtime's built-in tool-search behavior.
// NODEJS REFERENCE: nodejs/src/types.ts ToolSearchConfig
struct ToolSearchConfig
{
    std::optional<bool> enabled;
    /// Tool count above which MCP/external tools are deferred (runtime default: 30).
    std::optional<int> defer_threshold;
};

inline void to_json(json& j, const ToolSearchConfig& c)
{
    j = json::object();
    if (c.enabled)
        j["enabled"] = *c.enabled;
    if (c.defer_threshold)
        j["deferThreshold"] = *c.defer_threshold;
}

inline void from_json(const json& j, ToolSearchConfig& c)
{
    detail::get_optional(j, "enabled", c.enabled);
    detail::get_optional(j, "deferThreshold", c.defer_threshold);
}

/// Configuration for the built-in GitHub MCP server.
// NODEJS REFERENCE: nodejs/src/types.ts GitHubMcpToolConfig
struct GitHubMcpToolConfig
{
    std::optional<bool> enable_all_tools;
    std::optional<std::vector<std::string>> additional_toolsets;
    std::optional<std::vector<std::string>> additional_tools;
    std::optional<bool> enable_insiders_mode;
    std::optional<bool> disable_form_deferral;
};

inline void to_json(json& j, const GitHubMcpToolConfig& c)
{
    j = json::object();
    if (c.enable_all_tools)
        j["enableAllTools"] = *c.enable_all_tools;
    if (c.additional_toolsets)
        j["additionalToolsets"] = *c.additional_toolsets;
    if (c.additional_tools)
        j["additionalTools"] = *c.additional_tools;
    if (c.enable_insiders_mode)
        j["enableInsidersMode"] = *c.enable_insiders_mode;
    if (c.disable_form_deferral)
        j["disableFormDeferral"] = *c.disable_form_deferral;
}

inline void from_json(const json& j, GitHubMcpToolConfig& c)
{
    detail::get_optional(j, "enableAllTools", c.enable_all_tools);
    detail::get_optional(j, "additionalToolsets", c.additional_toolsets);
    detail::get_optional(j, "additionalTools", c.additional_tools);
    detail::get_optional(j, "enableInsidersMode", c.enable_insiders_mode);
    detail::get_optional(j, "disableFormDeferral", c.disable_form_deferral);
}

/// Well-known values for ManagedSettingsPermissions::disable_bypass_permissions_mode.
namespace disable_bypass_permissions_modes
{
inline constexpr const char* kDisable = "disable";
inline constexpr const char* kAllowAutoOnly = "allow-auto-only";
} // namespace disable_bypass_permissions_modes

/// Permissions-only managed policy injected by the host.
// NODEJS REFERENCE: nodejs/src/types.ts ManagedSettingsPermissions
struct ManagedSettingsPermissions
{
    /// Unknown values are forwarded so newer runtime policies fail closed.
    std::optional<std::string> disable_bypass_permissions_mode;
    std::optional<bool> disable_assisted_permissions_mode;
    std::optional<std::vector<std::string>> deny;
    std::optional<std::vector<std::string>> ask;
    std::optional<std::vector<std::string>> allow;
    /// Closed-world host boundary as Domain(...) rules; empty denies all hosts.
    std::optional<std::vector<std::string>> limit_to;
};

inline void to_json(json& j, const ManagedSettingsPermissions& p)
{
    j = json::object();
    if (p.disable_bypass_permissions_mode)
        j["disableBypassPermissionsMode"] = *p.disable_bypass_permissions_mode;
    if (p.disable_assisted_permissions_mode)
        j["disableAssistedPermissionsMode"] = *p.disable_assisted_permissions_mode;
    if (p.deny)
        j["deny"] = *p.deny;
    if (p.ask)
        j["ask"] = *p.ask;
    if (p.allow)
        j["allow"] = *p.allow;
    if (p.limit_to)
        j["limitTo"] = *p.limit_to;
}

inline void from_json(const json& j, ManagedSettingsPermissions& p)
{
    detail::get_optional(j, "disableBypassPermissionsMode", p.disable_bypass_permissions_mode);
    detail::get_optional(j, "disableAssistedPermissionsMode", p.disable_assisted_permissions_mode);
    detail::get_optional(j, "deny", p.deny);
    detail::get_optional(j, "ask", p.ask);
    detail::get_optional(j, "allow", p.allow);
    detail::get_optional(j, "limitTo", p.limit_to);
}

/// Host-injected enterprise managed settings.
// NODEJS REFERENCE: nodejs/src/types.ts ManagedSettings
struct ManagedSettings
{
    std::optional<ManagedSettingsPermissions> permissions;
};

inline void to_json(json& j, const ManagedSettings& s)
{
    j = json::object();
    if (s.permissions)
        j["permissions"] = *s.permissions;
}

inline void from_json(const json& j, ManagedSettings& s)
{
    detail::get_optional(j, "permissions", s.permissions);
}

/// One configuration entry of an ExP assignment response.
// NODEJS REFERENCE: nodejs/src/types.ts ExpConfigEntry
struct ExpConfigEntry
{
    std::string id;
    /// Parameter values (string, number, or boolean) keyed by name.
    std::map<std::string, json> parameters;
};

inline void to_json(json& j, const ExpConfigEntry& e)
{
    j = json{{"Id", e.id}, {"Parameters", e.parameters}};
}

inline void from_json(const json& j, ExpConfigEntry& e)
{
    e.id = j.value("Id", std::string{});
    e.parameters = j.value("Parameters", std::map<std::string, json>{});
}

/// ExP ("flight") assignment data in the experimentation service's PascalCase wire shape.
// NODEJS REFERENCE: nodejs/src/types.ts CopilotExpAssignmentResponse
struct CopilotExpAssignmentResponse
{
    std::vector<std::string> features;
    std::map<std::string, std::string> flights;
    std::vector<ExpConfigEntry> configs;
    /// Opaque payload passed through untouched.
    std::optional<json> parameter_groups;
    std::optional<double> flighting_version;
    std::optional<std::string> impression_id;
    std::string assignment_context;
};

inline void to_json(json& j, const CopilotExpAssignmentResponse& r)
{
    j = json{
        {"Features", r.features},
        {"Flights", r.flights},
        {"Configs", r.configs},
        {"AssignmentContext", r.assignment_context},
    };
    if (r.parameter_groups)
        j["ParameterGroups"] = *r.parameter_groups;
    if (r.flighting_version)
        j["FlightingVersion"] = *r.flighting_version;
    if (r.impression_id)
        j["ImpressionId"] = *r.impression_id;
}

inline void from_json(const json& j, CopilotExpAssignmentResponse& r)
{
    r.features = j.value("Features", std::vector<std::string>{});
    r.flights = j.value("Flights", std::map<std::string, std::string>{});
    r.configs = j.value("Configs", std::vector<ExpConfigEntry>{});
    if (j.contains("ParameterGroups"))
        r.parameter_groups = j.at("ParameterGroups");
    detail::get_optional(j, "FlightingVersion", r.flighting_version);
    detail::get_optional(j, "ImpressionId", r.impression_id);
    r.assignment_context = j.value("AssignmentContext", std::string{});
}

/// Stable extension identity for session participants that provide canvases.
// NODEJS REFERENCE: nodejs/src/types.ts ExtensionInfo
struct ExtensionInfo
{
    /// Namespace/source, e.g. "github-app".
    std::string source;
    std::string name;
};

inline void to_json(json& j, const ExtensionInfo& e)
{
    j = json{{"source", e.source}, {"name", e.name}};
}

inline void from_json(const json& j, ExtensionInfo& e)
{
    e.source = j.value("source", std::string{});
    e.name = j.value("name", std::string{});
}

/// Stable identity for a connection that supplies built-in canvases. The id is used
/// verbatim as the canvas extension id; ids starting with "connection:" are reserved.
// NODEJS REFERENCE: nodejs/src/types.ts CanvasProviderIdentity
struct CanvasProviderIdentity
{
    std::string id;
    std::optional<std::string> name;
};

inline void to_json(json& j, const CanvasProviderIdentity& p)
{
    j = json{{"id", p.id}};
    if (p.name)
        j["name"] = *p.name;
}

inline void from_json(const json& j, CanvasProviderIdentity& p)
{
    p.id = j.value("id", std::string{});
    detail::get_optional(j, "name", p.name);
}

// -----------------------------------------------------------------------------
// Session option groups
// -----------------------------------------------------------------------------
// NODEJS REFERENCE: nodejs/src/types.ts SessionOptionsBase and its Session*Options groups.
// Every group is a plain aggregate; unset fields are omitted from the wire.

struct SessionInferenceOptions
{
    std::optional<bool> allow_multiple_tool_calls;
    std::optional<ReasoningEffort> reasoning_effort;
    std::optional<std::string> reasoning_summary;
    /// "default" | "long_context"
    std::optional<std::string> context_tier;
    std::optional<ModelCapabilitiesOverride> capabilities;
    std::optional<std::vector<NamedProviderConfig>> providers;
    std::optional<std::vector<ProviderModelConfig>> models;
    /// Provider name for unqualified model selections. A JSON null clears it on resume;
    /// leaving it unset preserves the current value.
    std::optional<json> default_provider;
    std::optional<bool> enable_citations;
};

struct SessionPolicyOptions
{
    std::optional<std::vector<std::string>> allowed_models;
    std::optional<std::vector<std::string>> excluded_managed_settings;
    std::optional<bool> enforce_managed_model_defaults;
    std::optional<ManagedSettings> managed_settings;
};

struct SessionExperimentOptions
{
    std::optional<bool> enable_experimental_mode;
    std::optional<std::map<std::string, bool>> flags;
    std::optional<CopilotExpAssignmentResponse> assignments;
};

struct LargeToolOutputConfig
{
    std::optional<bool> enabled;
    std::optional<int64_t> max_size_bytes;
    std::optional<std::string> output_directory;
};

inline void to_json(json& j, const LargeToolOutputConfig& c)
{
    j = json::object();
    if (c.enabled)
        j["enabled"] = *c.enabled;
    if (c.max_size_bytes)
        j["maxSizeBytes"] = *c.max_size_bytes;
    if (c.output_directory)
        j["outputDir"] = *c.output_directory;
}

inline void from_json(const json& j, LargeToolOutputConfig& c)
{
    detail::get_optional(j, "enabled", c.enabled);
    detail::get_optional(j, "maxSizeBytes", c.max_size_bytes);
    detail::get_optional(j, "outputDir", c.output_directory);
}

struct SessionToolOptions
{
    std::optional<LargeToolOutputConfig> large_output;
    std::vector<Tool> definitions;
    std::optional<ToolSearchConfig> search;
    std::optional<ImageGenerationConfig> image_generation;
    /// Allow-list. available_tool_set wins when both are set.
    std::optional<std::vector<std::string>> available_tools;
    std::optional<ToolSet> available_tool_set;
    /// Deny-list. excluded_tool_set wins when both are set.
    std::optional<std::vector<std::string>> excluded_tools;
    std::optional<ToolSet> excluded_tool_set;
    std::optional<bool> manage_schedule_enabled;
};

struct SessionCanvasOptions
{
    std::vector<std::shared_ptr<Canvas>> definitions;
    /// Raw canvas declarations, sent after those of definitions.
    std::optional<std::vector<json>> declarations;
    std::optional<bool> request_renderer;
    std::optional<CanvasProviderIdentity> provider;
};

struct ResumeSessionCanvasOptions
{
    std::vector<std::shared_ptr<Canvas>> definitions;
    /// Raw canvas declarations, sent after those of definitions.
    std::optional<std::vector<json>> declarations;
    std::optional<bool> request_renderer;
    std::optional<CanvasProviderIdentity> provider;
    /// Canvases that were open when the session was suspended.
    std::optional<std::vector<OpenCanvasInstance>> open;
};

struct SessionExtensionOptions
{
    std::optional<bool> enabled;
    std::optional<std::string> sdk_path;
    std::optional<ExtensionInfo> identity;
};

struct SessionInteractionOptions
{
    /// "legacy" | "elicitation"
    std::optional<std::string> ask_user_variant;
    UserInputHandler on_user_input_request;
    ShellAuthenticationHandler on_shell_authentication_request;
    std::vector<CommandDefinition> commands;
    ElicitationHandler on_elicitation_request;
    ExitPlanModeHandler on_exit_plan_mode_request;
    AutoModeSwitchHandler on_auto_mode_switch_request;
    /// Requires on_auto_mode_switch_request.
    std::optional<bool> auto_mode_recovery;
};

struct SessionAgentOptions
{
    std::optional<std::vector<std::string>> excluded_builtin_agents;
    std::optional<std::vector<std::string>> included_builtin_agents;
    std::optional<bool> local_discovery_only;
    std::optional<std::vector<CustomAgentConfig>> definitions;
    std::optional<DefaultAgentConfig> default_agent;
    /// Name of the agent to activate.
    std::optional<std::string> selected;
    std::optional<std::vector<std::string>> directories;
};

struct SessionSkillOptions
{
    std::optional<std::vector<std::string>> included_builtin_skills;
    std::optional<std::vector<std::string>> directories;
    std::shared_ptr<SkillProvider> provider;
    std::optional<std::vector<std::string>> disabled_skills;
    std::optional<bool> enabled;
};

struct CapiSessionOptions
{
    std::optional<bool> enabled;
    std::optional<std::string> auto_tier;
    std::optional<std::string> fusion_tier;
    std::optional<bool> enable_web_socket_responses;
};

inline void to_json(json& j, const CapiSessionOptions& c)
{
    j = json::object();
    if (c.enabled)
        j["enabled"] = *c.enabled;
    if (c.auto_tier)
        j["autoTier"] = *c.auto_tier;
    if (c.fusion_tier)
        j["fusionTier"] = *c.fusion_tier;
    if (c.enable_web_socket_responses)
        j["enableWebSocketResponses"] = *c.enable_web_socket_responses;
}

inline void from_json(const json& j, CapiSessionOptions& c)
{
    detail::get_optional(j, "enabled", c.enabled);
    detail::get_optional(j, "autoTier", c.auto_tier);
    detail::get_optional(j, "fusionTier", c.fusion_tier);
    detail::get_optional(j, "enableWebSocketResponses", c.enable_web_socket_responses);
}

struct SessionGitHubOptions
{
    std::optional<CapiSessionOptions> capi;
    std::optional<bool> telemetry_enabled;
    std::optional<GitHubMcpToolConfig> mcp;
    /// Mutually exclusive with token_provider.
    std::optional<std::string> token;
    GitHubTokenProvider token_provider;
    std::optional<bool> fetch_managed_settings;
    std::optional<bool> interactive_user;
};

struct SessionFilesystemOptions
{
    std::optional<bool> track_file_changes;
    std::optional<std::vector<std::string>> additional_directories;
};

struct CreateSessionFilesystemOptions
{
    std::optional<bool> track_file_changes;
    std::optional<std::vector<std::string>> additional_directories;
    std::optional<WorkingDirectoryTrust> trust_working_directory;
};

struct SessionExecutionOptions
{
    std::optional<SessionLimitsConfig> limits;
    std::optional<AutopilotOptions> autopilot;
};

struct SessionInstructionOptions
{
    std::optional<bool> skip_custom_instructions;
    std::optional<std::vector<std::string>> directories;
    std::optional<std::string> organization_text;
    std::optional<bool> on_demand_discovery;
};

struct CreateSessionInstructionOptions
{
    std::optional<bool> skip_custom_instructions;
    std::optional<std::vector<std::string>> directories;
    std::optional<std::string> organization_text;
    std::optional<bool> on_demand_discovery;
    std::optional<bool> refresh_custom_instructions;
};

struct SessionGitOptions
{
    std::optional<bool> coauthor_enabled;
    std::optional<bool> enable_host_git_operations;
};

struct SessionPermissionOptions
{
    PermissionHandler on_request;
    /// Context-aware variant; takes precedence over on_request when both are set.
    PermissionHandlerWithContext on_request_with_context;
    std::optional<SandboxConfig> sandbox;
    std::optional<std::string> mode;
};

struct SessionMcpOptions
{
    McpAuthHandler on_auth_request;
    std::optional<bool> enable_apps;
    /// "persistent" | "in-memory"
    std::optional<std::string> oauth_token_storage;
    std::optional<std::string> auth_client_id_metadata_url;
    /// Server name -> config.
    std::optional<std::map<std::string, MCPServerConfig>> servers;
    std::optional<std::vector<std::string>> disabled_servers;
};

struct SessionHookOptions
{
    std::optional<SessionHooks> handlers;
    std::optional<std::vector<std::string>> directories;
    std::optional<bool> enable_repository_hooks;
};

struct SessionEventOptions
{
    std::optional<bool> streaming;
    std::optional<bool> include_sub_agent_streaming_events;
    /// Registered before the create/resume RPC so early events are not missed.
    EventHandler on_event;
};

struct SessionPluginOptions
{
    std::optional<std::vector<std::string>> directories;
};

struct SessionStateOptions
{
    std::optional<InfiniteSessionConfig> infinite_sessions;
    std::optional<MemoryConfiguration> memory;
    std::optional<bool> skip_embedding_retrieval;
    /// "persistent" | "in-memory"
    std::optional<std::string> embedding_cache_storage;
    std::optional<bool> enable_cross_session_store;
    /// Required when ClientOptions::session_fs is set: builds the filesystem provider
    /// for each created or resumed session.
    // NODEJS REFERENCE: nodejs/src/types.ts SessionOptionsBase.state.createSessionFsProvider
    std::function<std::shared_ptr<SessionFsProvider>(Session& session)> create_session_fs_provider;
};

struct CloudSessionRepository
{
    std::string owner;
    std::string name;
    std::optional<std::string> branch;
};

inline void to_json(json& j, const CloudSessionRepository& r)
{
    j = json{{"owner", r.owner}, {"name", r.name}};
    if (r.branch)
        j["branch"] = *r.branch;
}

inline void from_json(const json& j, CloudSessionRepository& r)
{
    r.owner = j.value("owner", std::string{});
    r.name = j.value("name", std::string{});
    detail::get_optional(j, "branch", r.branch);
}

struct CloudSessionOptions
{
    std::optional<CloudSessionRepository> repository;
};

inline void to_json(json& j, const CloudSessionOptions& c)
{
    j = json::object();
    if (c.repository)
        j["repository"] = *c.repository;
}

inline void from_json(const json& j, CloudSessionOptions& c)
{
    detail::get_optional(j, "repository", c.repository);
}

/// Options for Client::create_session.
// NODEJS REFERENCE: nodejs/src/types.ts CreateSessionOptions (extends SessionOptionsBase)
struct CreateSessionOptions
{
    /// Caller-chosen session id; generated when omitted (except for cloud sessions).
    std::optional<std::string> session_id;
    std::optional<std::string> name;

    SessionInferenceOptions inference;
    SessionPolicyOptions policy;
    std::optional<std::string> client_name;
    std::optional<std::string> model;
    SessionExperimentOptions experiments;
    std::optional<DiagnosticsConfiguration> diagnostics;
    std::optional<LifecycleDiagnosticsConfiguration> lifecycle_diagnostics;
    SessionToolOptions tools;
    std::optional<std::string> config_directory;
    std::optional<bool> enable_config_discovery;
    SessionCanvasOptions canvases;
    SessionExtensionOptions extensions;
    SessionInteractionOptions interaction;
    std::optional<SystemMessageConfig> system_message;
    SessionAgentOptions agents;
    SessionSkillOptions skills;
    SessionGitHubOptions github;
    CreateSessionFilesystemOptions filesystem;
    SessionExecutionOptions execution;
    CreateSessionInstructionOptions instructions;
    SessionGitOptions git;
    SessionPermissionOptions permissions;
    SessionMcpOptions mcp;
    SessionHookOptions hooks;
    std::optional<std::string> working_directory;
    SessionEventOptions events;
    SessionPluginOptions plugins;
    SessionStateOptions state;

    /// Create the session in the cloud instead of locally.
    std::optional<CloudSessionOptions> cloud;
};

/// Options for Client::resume_session.
// NODEJS REFERENCE: nodejs/src/types.ts ResumeSessionOptions (extends SessionOptionsBase)
struct ResumeSessionOptions
{
    SessionInferenceOptions inference;
    SessionPolicyOptions policy;
    std::optional<std::string> client_name;
    std::optional<std::string> model;
    SessionExperimentOptions experiments;
    std::optional<DiagnosticsConfiguration> diagnostics;
    std::optional<LifecycleDiagnosticsConfiguration> lifecycle_diagnostics;
    SessionToolOptions tools;
    std::optional<std::string> config_directory;
    std::optional<bool> enable_config_discovery;
    ResumeSessionCanvasOptions canvases;
    SessionExtensionOptions extensions;
    SessionInteractionOptions interaction;
    std::optional<SystemMessageConfig> system_message;
    SessionAgentOptions agents;
    SessionSkillOptions skills;
    SessionGitHubOptions github;
    SessionFilesystemOptions filesystem;
    SessionExecutionOptions execution;
    SessionInstructionOptions instructions;
    SessionGitOptions git;
    SessionPermissionOptions permissions;
    SessionMcpOptions mcp;
    SessionHookOptions hooks;
    std::optional<std::string> working_directory;
    SessionEventOptions events;
    SessionPluginOptions plugins;
    SessionStateOptions state;

    std::optional<bool> allow_transcript_recovery;
    /// Do not emit session.resume.
    std::optional<bool> suppress_resume_event;
    std::optional<bool> continue_pending_work;

    /// Extension workflows to (re)declare. workflow_objects wins when non-empty.
    std::optional<std::vector<json>> workflows;
    std::vector<std::shared_ptr<WorkflowHandle>> workflow_objects;
    std::optional<std::vector<std::string>> requested_environment_variables;
};

/// Options for sending a message
/// Provenance tag copied to the resulting `user.message` event.
///
/// On the wire this is a plain string constrained to
/// `^(user|system|command-.*|schedule-\d+|agent-.+|automation-.+)$`. Only the forms
/// the official SDKs expose are constructible here; `command-` and `schedule-` are
/// runtime-internal provenance the SDK surface does not mint.
class MessageSource
{
  public:
    /// The message originates from user input.
    static MessageSource user()
    {
        return MessageSource("user");
    }
    /// The message provides application-generated context.
    static MessageSource system()
    {
        return MessageSource("system");
    }
    /// A prompt sent on behalf of another agent. `id` is preserved verbatim
    /// after the `agent-` prefix.
    static MessageSource agent(const std::string& id)
    {
        return MessageSource("agent-" + id);
    }
    /// A turn sent on behalf of an automation the user configured. It defines the
    /// session's task like a user message but carries no human authority.
    /// @throws std::invalid_argument if `id` is empty.
    static MessageSource automation(const std::string& id)
    {
        if (id.empty())
            throw std::invalid_argument("MessageSource::automation requires a non-empty id");
        return MessageSource("automation-" + id);
    }

    /// The string used on the wire.
    const std::string& value() const noexcept
    {
        return value_;
    }

    friend bool operator==(const MessageSource& a, const MessageSource& b) noexcept
    {
        return a.value_ == b.value_;
    }

  private:
    explicit MessageSource(std::string value) : value_(std::move(value)) {}
    std::string value_;
};

struct MessageOptions
{
    std::string prompt;
    std::optional<std::vector<UserMessageAttachment>> attachments;
    std::optional<std::string> mode;
    std::optional<std::string> agent_mode;
    std::optional<std::map<std::string, std::string>> request_headers;
    std::optional<std::string> display_prompt;
    /// Optional provenance tag; omitted from the wire when unset.
    std::optional<MessageSource> source;
    std::optional<json> response_schema;
    /// Caller-chosen id echoed on the resulting turn events.
    std::optional<std::string> client_turn_id;
};

inline void to_json(json& j, const MessageOptions& o)
{
    j = json{{"prompt", o.prompt}};
    if (o.attachments)
        j["attachments"] = *o.attachments;
    if (o.mode)
        j["mode"] = *o.mode;
    if (o.agent_mode)
        j["agentMode"] = *o.agent_mode;
    if (o.request_headers)
        j["requestHeaders"] = *o.request_headers;
    if (o.display_prompt)
        j["displayPrompt"] = *o.display_prompt;
    if (o.source)
        j["source"] = o.source->value();
    if (o.client_turn_id)
        j["clientTurnId"] = *o.client_turn_id;
    if (o.response_schema)
        j["responseFormat"] = {
            {"type", "json_schema"},
            {"jsonSchema", {{"name", "response"}, {"schema", *o.response_schema}, {"strict", true}}}
        };
}

inline void from_json(const json& j, MessageOptions& o)
{
    j.at("prompt").get_to(o.prompt);
    if (j.contains("attachments"))
        o.attachments = j.at("attachments").get<std::vector<UserMessageAttachment>>();
    if (j.contains("mode"))
        o.mode = j.at("mode").get<std::string>();
    if (j.contains("agentMode"))
        o.agent_mode = j.at("agentMode").get<std::string>();
    if (j.contains("requestHeaders"))
        o.request_headers = j.at("requestHeaders").get<std::map<std::string, std::string>>();
    if (j.contains("displayPrompt"))
        o.display_prompt = j.at("displayPrompt").get<std::string>();
    if (j.contains("clientTurnId") && j["clientTurnId"].is_string())
        o.client_turn_id = j.at("clientTurnId").get<std::string>();
    if (j.contains("responseFormat") && j["responseFormat"].is_object())
    {
        const auto& format = j["responseFormat"];
        if (format.contains("jsonSchema") && format["jsonSchema"].contains("schema"))
            o.response_schema = format["jsonSchema"]["schema"];
    }
}

// =============================================================================
// Response Types
// =============================================================================

/// Runtime-advertised session capabilities. Unknown future capability keys are preserved.
struct SessionCapabilities
{
    json value = json::object();

    bool has(const std::string& dotted_path) const
    {
        const json* current = &value;
        std::size_t start = 0;
        while (start <= dotted_path.size())
        {
            const auto end = dotted_path.find('.', start);
            const auto key = dotted_path.substr(start, end - start);
            if (!current->is_object() || !current->contains(key))
                return false;
            current = &current->at(key);
            if (end == std::string::npos)
                return current->is_boolean() ? current->get<bool>() : !current->is_null();
            start = end + 1;
        }
        return false;
    }
};

inline void to_json(json& j, const SessionCapabilities& capabilities)
{
    j = capabilities.value;
}

inline void from_json(const json& j, SessionCapabilities& capabilities)
{
    capabilities.value = j;
}

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

inline std::optional<std::chrono::system_clock::time_point>
parse_iso8601_timestamp(const std::string& s)
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
        std::chrono::day{static_cast<unsigned>(day_num)}
    };
    if (!ymd.ok())
        return std::nullopt;

    std::chrono::sys_time<std::chrono::nanoseconds> tp =
        std::chrono::sys_days{ymd} + std::chrono::hours{hour_num} +
        std::chrono::minutes{minute_num} + std::chrono::seconds{second_num} + fractional_ns;
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
    int64_t timestamp = 0;
    std::optional<std::string> timestamp_iso;
    std::optional<int> protocol_version;
};

inline void from_json(const json& j, PingResponse& r)
{
    j.at("message").get_to(r.message);
    if (j.at("timestamp").is_number_integer())
        j.at("timestamp").get_to(r.timestamp);
    else if (j.at("timestamp").is_string())
        r.timestamp_iso = j.at("timestamp").get<std::string>();
    if (j.contains("protocolVersion"))
        r.protocol_version = j.at("protocolVersion").get<int>();
}

/// Response from status.get request
struct GetStatusResponse
{
    std::string version;
    int protocol_version;
    /// Host-level diagnostics, when the runtime reports them.
    std::optional<HostDiagnostics> host_diagnostics;
};

inline void from_json(const json& j, GetStatusResponse& r)
{
    j.at("version").get_to(r.version);
    j.at("protocolVersion").get_to(r.protocol_version);
    if (j.contains("hostDiagnostics") && j["hostDiagnostics"].is_object())
        r.host_diagnostics = j["hostDiagnostics"].get<HostDiagnostics>();
}

/// Response from auth.getStatus request
struct GetAuthStatusResponse
{
    bool is_authenticated = false;
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
        if (j["limits"].contains("max_prompt_tokens") &&
            !j["limits"]["max_prompt_tokens"].is_null())
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
        m.supported_reasoning_efforts =
            j["supportedReasoningEfforts"].get<std::vector<std::string>>();
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
// Client Options
// =============================================================================

/// Ambient-default mode for the SDK.
// NODEJS REFERENCE: nodejs/src/types.ts CopilotClientOptions.mode
enum class ClientMode
{
    /// Coding-agent defaults: repository config, hooks, skills, memory, git, etc.
    CodingAgent,
    /// Nothing ambient: the host supplies everything explicitly.
    Empty
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    ClientMode,
    {
        {ClientMode::CodingAgent, "coding-agent"},
        {ClientMode::Empty, "empty"},
    }
)

enum class RuntimeConnectionKind
{
    /// The SDK spawns the CLI and talks over its stdio.
    SpawnedStdio,
    /// The SDK spawns the CLI as a loopback TCP server.
    SpawnedTcp,
    /// Attach to an already-running TCP server.
    Tcp,
    /// Load the runtime into this process.
    InProcess,
    /// The SDK is running as a child of the CLI (extensions); talk over our own stdio.
    ParentProcess
};

/// Runtime transport/process selection.
// NODEJS REFERENCE: nodejs/src/types.ts RuntimeConnection / RuntimeConnection.for*()
struct RuntimeConnection
{
    RuntimeConnectionKind kind = RuntimeConnectionKind::SpawnedStdio;
    /// CLI executable (spawned) or CLI entrypoint (in-process). Spawned connections fall
    /// back to $COPILOT_CLI_PATH and then "copilot".
    std::optional<std::string> path;
    /// Extra CLI arguments placed before the SDK-managed ones.
    std::optional<std::vector<std::string>> args;
    /// Full environment for the spawned process. When set it replaces the inherited one.
    std::optional<std::map<std::string, std::string>> environment;
    /// Working directory of the spawned process.
    std::optional<std::string> working_directory;
    /// Runtime state directory ($COPILOT_HOME).
    std::optional<std::string> base_directory;
    /// Server-wide session idle timeout; 0 or unset disables it.
    std::optional<int> session_idle_timeout_seconds;
    /// TCP port. 0 lets a spawned server pick one.
    int port = 0;
    /// Host for attached TCP connections.
    std::optional<std::string> host;
    /// TCP connection token. Generated for spawned TCP when unset.
    std::optional<std::string> connection_token;
    std::optional<std::string> ffi_library_path;
    /// In-process only: marshals native UI actions onto the application's UI (process
    /// main) thread. Call `action` there; it may run synchronously or be posted.
    // NODEJS REFERENCE: nodejs/src/types.ts InProcessRuntimeConnection.runOnUiThread
    std::function<void(std::function<void()> action)> run_on_ui_thread;

    static RuntimeConnection for_spawned_stdio(
        std::optional<std::string> path = std::nullopt,
        std::optional<std::vector<std::string>> args = std::nullopt,
        std::optional<std::map<std::string, std::string>> environment = std::nullopt,
        std::optional<std::string> working_directory = std::nullopt,
        std::optional<std::string> base_directory = std::nullopt,
        std::optional<int> session_idle_timeout_seconds = std::nullopt
    )
    {
        return RuntimeConnection{
            .kind = RuntimeConnectionKind::SpawnedStdio,
            .path = std::move(path),
            .args = std::move(args),
            .environment = std::move(environment),
            .working_directory = std::move(working_directory),
            .base_directory = std::move(base_directory),
            .session_idle_timeout_seconds = session_idle_timeout_seconds,
        };
    }

    static RuntimeConnection for_spawned_tcp(
        int port = 0,
        std::optional<std::string> connection_token = std::nullopt,
        std::optional<std::string> path = std::nullopt,
        std::optional<std::vector<std::string>> args = std::nullopt,
        std::optional<std::map<std::string, std::string>> environment = std::nullopt,
        std::optional<std::string> working_directory = std::nullopt,
        std::optional<std::string> base_directory = std::nullopt,
        std::optional<int> session_idle_timeout_seconds = std::nullopt
    )
    {
        return RuntimeConnection{
            .kind = RuntimeConnectionKind::SpawnedTcp,
            .path = std::move(path),
            .args = std::move(args),
            .environment = std::move(environment),
            .working_directory = std::move(working_directory),
            .base_directory = std::move(base_directory),
            .session_idle_timeout_seconds = session_idle_timeout_seconds,
            .port = port,
            .connection_token = std::move(connection_token),
        };
    }

    static RuntimeConnection for_tcp(
        std::string host, int port, std::optional<std::string> connection_token = std::nullopt
    )
    {
        return RuntimeConnection{
            .kind = RuntimeConnectionKind::Tcp,
            .port = port,
            .host = std::move(host),
            .connection_token = std::move(connection_token),
        };
    }

    static RuntimeConnection for_in_process(
        std::optional<std::string> base_directory = std::nullopt,
        std::optional<int> session_idle_timeout_seconds = std::nullopt,
        std::optional<std::string> cli_entrypoint = std::nullopt,
        std::optional<std::string> library_path = std::nullopt,
        std::function<void(std::function<void()> action)> run_on_ui_thread = {}
    )
    {
        return RuntimeConnection{
            .kind = RuntimeConnectionKind::InProcess,
            .path = std::move(cli_entrypoint),
            .base_directory = std::move(base_directory),
            .session_idle_timeout_seconds = session_idle_timeout_seconds,
            .ffi_library_path = std::move(library_path),
            .run_on_ui_thread = std::move(run_on_ui_thread),
        };
    }

    static RuntimeConnection for_parent_process()
    {
        return RuntimeConnection{.kind = RuntimeConnectionKind::ParentProcess};
    }

    /// Selected by $COPILOT_SDK_DEFAULT_CONNECTION: "stdio" spawns over stdio; unset or
    /// "inprocess" loads in-process. Any other value throws std::invalid_argument.
    static RuntimeConnection for_default(
        std::optional<std::string> base_directory = std::nullopt,
        std::optional<int> session_idle_timeout_seconds = std::nullopt
    )
    {
        std::string value = detail::env_value("COPILOT_SDK_DEFAULT_CONNECTION").value_or("");
        std::string lowered;
        for (char ch : value)
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        if (lowered == "stdio")
            return for_spawned_stdio(
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::move(base_directory),
                session_idle_timeout_seconds
            );
        if (lowered.empty() || lowered == "inprocess")
            return for_in_process(std::move(base_directory), session_idle_timeout_seconds);
        throw std::invalid_argument(
            "Invalid COPILOT_SDK_DEFAULT_CONNECTION value '" + value +
            "'. Expected 'inprocess', 'stdio', or unset."
        );
    }
};

struct TelemetryConfig
{
    std::optional<std::string> otlp_endpoint;
    std::optional<std::string> otlp_protocol;
    std::optional<std::string> file_path;
    std::optional<std::string> exporter_type;
    std::optional<std::string> source_name;
    std::optional<bool> capture_content;
};

using TraceContextProvider = std::function<std::map<std::string, std::string>()>;
using GitHubTelemetryHandler = std::function<void(const json& notification)>;
/// Optional features declared by the session filesystem provider. Every provider
/// returned by SessionStateOptions::create_session_fs_provider must implement each
/// declared feature.
// NODEJS REFERENCE: nodejs/src/types.ts SessionFsConfig.capabilities
struct SessionFsCapabilities
{
    /// SQLite query/exists operations (SessionFsProvider::sqlite). When false or
    /// omitted, the runtime offers no SQL tools or todo tracking for these sessions.
    std::optional<bool> sqlite;
    /// Exact binary reads and writes (read_file_bytes / write_file_bytes). Required
    /// to view images stored only in the provider.
    std::optional<bool> binary;
    /// Bounded byte-range reads (read_file_range), independently of binary support.
    std::optional<bool> read_file_range;
};

inline void to_json(json& j, const SessionFsCapabilities& c)
{
    j = json::object();
    if (c.sqlite)
        j["sqlite"] = *c.sqlite;
    if (c.binary)
        j["binary"] = *c.binary;
    if (c.read_file_range)
        j["readFileRange"] = *c.read_file_range;
}

/// Route session storage through client-provided filesystems. Each session supplies
/// its provider via SessionStateOptions::create_session_fs_provider.
// NODEJS REFERENCE: nodejs/src/types.ts SessionFsConfig
struct SessionFsConfig
{
    /// Initial working directory for sessions (the user's project directory).
    std::string initial_cwd;
    /// Path within each session's SessionFs where the runtime stores session-scoped
    /// files (events, workspace, checkpoints, etc.).
    std::string session_state_path;
    /// "windows" | "posix"
    std::string conventions;
    std::optional<SessionFsCapabilities> capabilities;
};

/// Identity of the integrating application, sent on the `connect` handshake.
///
/// All fields are optional; empty ones are dropped and an entirely empty struct
/// is omitted from the handshake so the runtime keeps its default attribution.
///
/// The public field names deliberately differ from the wire names:
///
///   application_name    -> editorName
///   application_version -> editorVersion
///   integration_name    -> extensionName
///   integration_version -> extensionVersion
struct CopilotClientInfo
{
    std::optional<std::string> application_name;
    std::optional<std::string> application_version;
    std::optional<std::string> integration_name;
    std::optional<std::string> integration_version;

    /// Wire form, or nullopt when nothing usable was supplied.
    std::optional<json> to_wire_json() const
    {
        json out = json::object();
        const auto put = [&out](const char* key, const std::optional<std::string>& value)
        {
            if (value && !value->empty())
                out[key] = *value;
        };
        put("editorName", application_name);
        put("editorVersion", application_version);
        put("extensionName", integration_name);
        put("extensionVersion", integration_version);
        if (out.empty())
            return std::nullopt;
        return out;
    }
};

enum class InstallationDecision
{
    Confirm,
    Decline,
    Cancel
};
using InstallationConfirmationHandler =
    std::function<InstallationDecision(const json&, const CancellationToken&)>;
using ExtensionLaunchProvider = std::function<json(const json&, const CancellationToken&)>;

/// Overrides Client::list_models() (BYOK).
using ListModelsHandler = std::function<std::vector<ModelInfo>()>;

struct ClientPluginOptions
{
    /// Absolute directories of built-in plugins.
    std::optional<std::vector<std::string>> builtin_directories;
};

struct ClientExtensionOptions
{
    std::optional<ExtensionLaunchProvider> launch_provider;
};

struct ClientInstallationOptions
{
    std::optional<InstallationConfirmationHandler> confirmation_handler;
};

struct ClientObservabilityOptions
{
    /// Forwarded as --log-level when set.
    std::optional<LogLevel> log_level;
    std::optional<TelemetryConfig> telemetry;
    std::optional<TraceContextProvider> on_get_trace_context;
};

struct ClientGitHubOptions
{
    /// Forwarded via COPILOT_SDK_AUTH_TOKEN + --auth-token-env.
    std::optional<std::string> token;
    /// Defaults to true unless token is set.
    std::optional<bool> use_logged_in_user;
    std::optional<GitHubTelemetryHandler> on_telemetry;
};

/// Which traffic the host's network callbacks serve.
enum class NetworkScope
{
    /// Only requests made for sessions this client creates or resumes (the default).
    Connection,
    /// Also the runtime's shared background traffic. Only one connection at a time can
    /// hold this scope, so startup fails if another client holds it.
    Runtime,
};

/// Credentials for a proxy's Basic authentication challenge.
struct NetworkBasicCredentials
{
    /// Must be non-empty and contain no colon or control characters.
    std::string username;
    /// An omitted password means an empty one.
    std::optional<std::string> password;
};

/// Network callbacks through which the SDK host takes over parts of the runtime's
/// networking, e.g. when the runtime runs in WSL or a container and should use the
/// host's proxy settings and credentials. Each registered callback replaces the
/// runtime's own handling for the traffic in `scope`.
///
/// Each callback receives the JSON request (`url`; `spn`; `proxy`, `realm`,
/// `previousRejected`) and a CancellationToken that fires when the runtime stops
/// waiting. Return std::nullopt to defer to the runtime (resolve_proxy) or to report
/// "unavailable" (the authenticate callbacks). Never log or persist credentials.
/// NODEJS REFERENCE: nodejs/src/types.ts NetworkConfig
/// @experimental
struct NetworkConfig
{
    std::optional<NetworkScope> scope;
    /// Returns a PAC-style proxy list such as `PROXY proxy.example:8080; DIRECT`.
    std::function<std::optional<std::string>(const json&, const CancellationToken&)> resolve_proxy;
    /// Returns a fresh, single-use base64 SPNEGO token without the `Negotiate ` prefix.
    std::function<std::optional<std::string>(const json&, const CancellationToken&)>
        authenticate_negotiate;
    /// Returns credentials for a proxy's Basic challenge.
    std::function<std::optional<NetworkBasicCredentials>(const json&, const CancellationToken&)>
        authenticate_basic;
};

/// Host-owned Entra token acquisition: receives the `EntraTokenAcquireRequest`
/// JSON and returns the matching `EntraTokenAcquireResult` JSON. Renewal must be
/// silent and account-pinned. The token fires when the request or connection ends.
/// @experimental
using EntraTokenProvider = std::function<json(const json& request, const CancellationToken&)>;

/// The host application's Microsoft Entra registration, shared by its sessions.
/// NODEJS REFERENCE: nodejs/src/types.ts EntraAuthOptions
/// @experimental
struct EntraAuthOptions
{
    /// Application (client) ID of the host's Entra registration. Must be nonblank.
    std::string client_id;
    /// Native OneAuth application descriptor ID; blank uses client_id.
    std::optional<std::string> one_auth_application_id;
    /// Application name reported to MATS telemetry; blank disables MATS telemetry.
    std::optional<std::string> mats_app_name;
    /// Host-owned acquisition; omission keeps the runtime's built-in authentication.
    EntraTokenProvider token_provider;
};

struct ClientModelOptions
{
    ListModelsHandler on_list_models;
    std::shared_ptr<CopilotRequestHandler> request_handler;
};

/// Options for creating a Client.
// NODEJS REFERENCE: nodejs/src/types.ts CopilotClientOptions
struct ClientOptions
{
    /// Defaults to RuntimeConnection::for_default() (in-process unless
    /// $COPILOT_SDK_DEFAULT_CONNECTION=stdio).
    std::optional<RuntimeConnection> runtime;
    ClientMode mode = ClientMode::CodingAgent;
    ClientPluginOptions plugins;
    std::optional<SessionFsConfig> session_fs;
    ClientExtensionOptions extensions;
    ClientInstallationOptions installations;
    ClientObservabilityOptions observability;
    ClientGitHubOptions github;
    ClientModelOptions models;
    /// Identity declared on the `connect` handshake; see CopilotClientInfo.
    std::optional<CopilotClientInfo> client_info;
    /// Host-owned proxy resolution and proxy authentication. @experimental
    std::optional<NetworkConfig> network;
    /// Microsoft Entra registration, sent on the `connect` handshake. @experimental
    std::optional<EntraAuthOptions> entra;
    /// Deadline for each JSON-RPC request the client sends (default 30 s; 0 = none).
    /// A request the runtime does not answer in time fails with JsonRpcError
    /// (JsonRpcErrorCode::Timeout) and is removed from the pending table, so start(),
    /// create_session(), send() etc. never block a thread indefinitely.
    std::optional<std::chrono::milliseconds> request_timeout;
};

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
    j = json{
        {"type", "selection"},
        {"filePath", a.file_path},
        {"displayName", a.display_name},
        {"text", a.text},
        {"selection", a.selection}
    };
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
} // namespace SessionLifecycleEventTypes

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
