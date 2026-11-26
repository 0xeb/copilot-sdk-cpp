// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

// NODEJS REFERENCE: nodejs/src/host.ts, nodejs/src/client.ts startAhpHost
// C# REFERENCE: dotnet/src/Host.cs

#include <chrono>
#include <copilot/host.hpp>
#include <copilot/jsonrpc.hpp>
#include <cstdlib>
#include <regex>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace copilot
{

namespace
{

template <typename T>
std::optional<T> optional_field(const json& j, const char* key)
{
    std::optional<T> value;
    if (j.is_object())
        detail::get_optional(j, key, value);
    return value;
}

} // namespace

AhpHost::AhpHost(const HostStartResult& info, std::shared_ptr<JsonRpcClient> connection)
    : connection_(std::move(connection))
{
    const auto& raw = info.raw();
    if (!raw.is_object() || !raw.contains("hostId") || !raw["hostId"].is_string())
        throw std::runtime_error("host.start returned no hostId");
    host_id_ = raw["hostId"].get<std::string>();
    url_ = optional_field<std::string>(raw, "url");
    environment_id_ = optional_field<std::string>(raw, "environmentId");
    token_ = optional_field<std::string>(raw, "token");
    pid_ = optional_field<int64_t>(raw, "pid");
}

// NODEJS REFERENCE: host.ts AhpHost.dispose
std::future<void> AhpHost::dispose()
{
    auto connection = connection_;
    auto host_id = host_id_;
    return std::async(
        std::launch::async,
        [connection, host_id]() { connection->invoke("host.dispose", {{"hostId", host_id}}).get(); }
    );
}

// NODEJS REFERENCE: host.ts AhpHost.setAllowCreatingNewSessions
std::future<void> AhpHost::set_allow_creating_new_sessions(bool allowed)
{
    auto connection = connection_;
    auto host_id = host_id_;
    return std::async(
        std::launch::async,
        [connection, host_id, allowed]()
        {
            connection
                ->invoke(
                    "host.setSessionCreationPolicy",
                    {{"hostId", host_id}, {"allowCreatingNewSessions", allowed}}
                )
                .get();
        }
    );
}

// NODEJS REFERENCE: host.ts AhpHost.publishSession
std::future<HostPublishSessionResult> AhpHost::publish_session(const std::string& session_id)
{
    auto connection = connection_;
    auto host_id = host_id_;
    return std::async(
        std::launch::async,
        [connection, host_id, session_id]()
        {
            return HostPublishSessionResult(
                connection
                    ->invoke("host.publishSession", {{"hostId", host_id}, {"sessionId", session_id}})
                    .get()
            );
        }
    );
}

// NODEJS REFERENCE: host.ts AhpHost.unpublishSession
std::future<HostUnpublishSessionResult> AhpHost::unpublish_session(const std::string& session_id)
{
    auto connection = connection_;
    auto host_id = host_id_;
    return std::async(
        std::launch::async,
        [connection, host_id, session_id]()
        {
            return HostUnpublishSessionResult(
                connection
                    ->invoke(
                        "host.unpublishSession", {{"hostId", host_id}, {"sessionId", session_id}}
                    )
                    .get()
            );
        }
    );
}

// NODEJS REFERENCE: host.ts AhpHost.listSessions
std::future<HostListSessionsResult> AhpHost::list_sessions()
{
    auto connection = connection_;
    auto host_id = host_id_;
    return std::async(
        std::launch::async,
        [connection, host_id]()
        {
            return HostListSessionsResult(
                connection->invoke("host.listSessions", {{"hostId", host_id}}).get()
            );
        }
    );
}

namespace detail
{

namespace
{

// NODEJS REFERENCE: client.ts groupHostSessionSettings paths
// C# REFERENCE: Host.cs SetAhpSetting
const std::unordered_map<std::string, std::pair<const char*, const char*>>& ahp_setting_paths()
{
    static const std::unordered_map<std::string, std::pair<const char*, const char*>> paths = {
        {"enableExperimentalMode", {"experiments", "enableExperimentalMode"}},
        {"infiniteSessions", {"state", "infiniteSessions"}},
        {"additionalDirectories", {"filesystem", "additionalDirectories"}},
        {"streaming", {"events", "streaming"}},
        {"gitHubToken", {"github", "token"}},
        {"mcpOAuthTokenStorage", {"mcp", "oauthTokenStorage"}},
        {"featureFlags", {"experiments", "flags"}},
        {"enableManagedSettings", {"github", "fetchManagedSettings"}},
        {"excludedManagedSettings", {"policy", "excludedManagedSettings"}},
        {"enableSessionStore", {"state", "enableCrossSessionStore"}},
        {"availableTools", {"tools", "availableTools"}},
        {"excludedTools", {"tools", "excludedTools"}},
        {"allowedModels", {"policy", "allowedModels"}},
        {"skipCustomInstructions", {"instructions", "skipCustomInstructions"}},
        {"refreshCustomInstructions", {"instructions", "refreshCustomInstructions"}},
        {"customAgentsLocalOnly", {"agents", "localDiscoveryOnly"}},
        {"coauthorEnabled", {"git", "coauthorEnabled"}},
        {"manageScheduleEnabled", {"tools", "manageScheduleEnabled"}},
        {"memory", {"state", "memory"}},
        {"pluginDirectories", {"plugins", "directories"}},
        {"skillDirectories", {"skills", "directories"}},
        {"agentDirectories", {"agents", "directories"}},
        {"hookDirectories", {"hooks", "directories"}},
        {"instructionDirectories", {"instructions", "directories"}},
        {"enableMcpApps", {"mcp", "enableApps"}},
        {"reasoningEffort", {"inference", "reasoningEffort"}},
        {"allowMultipleToolCalls", {"inference", "allowMultipleToolCalls"}},
        {"contextTier", {"inference", "contextTier"}},
        {"providers", {"inference", "providers"}},
        {"models", {"inference", "models"}},
        {"defaultProvider", {"inference", "defaultProvider"}},
        {"capi", {"github", "capi"}},
        {"autopilot", {"execution", "autopilot"}},
        {"sessionLimits", {"execution", "limits"}},
        {"permissionMode", {"permissions", "mode"}},
    };
    return paths;
}

const json& group_of(const json& config, const char* name)
{
    static const json empty = json::object();
    if (!config.is_object())
        return empty;
    const auto it = config.find(name);
    return it != config.end() && it->is_object() ? *it : empty;
}

template <typename T> void read(const json& j, const char* key, std::optional<T>& value)
{
    get_optional(j, key, value);
}

// Inverse of session_request.cpp build_session_request_common for every key a
// host can select (plus the remaining serializable ones).
template <typename Options> void read_common(const json& config, Options& options)
{
    constexpr bool is_create = std::is_same_v<Options, CreateSessionOptions>;

    read(config, "model", options.model);
    read(config, "clientName", options.client_name);
    read(config, "systemMessage", options.system_message);
    read(config, "workingDirectory", options.working_directory);
    read(config, "configDirectory", options.config_directory);
    read(config, "enableConfigDiscovery", options.enable_config_discovery);
    read(config, "diagnostics", options.diagnostics);
    read(config, "lifecycleDiagnostics", options.lifecycle_diagnostics);

    const auto& policy = group_of(config, "policy");
    read(policy, "allowedModels", options.policy.allowed_models);
    read(policy, "excludedManagedSettings", options.policy.excluded_managed_settings);
    read(policy, "enforceManagedModelDefaults", options.policy.enforce_managed_model_defaults);
    read(policy, "managedSettings", options.policy.managed_settings);

    const auto& inference = group_of(config, "inference");
    read(inference, "reasoningEffort", options.inference.reasoning_effort);
    read(inference, "allowMultipleToolCalls", options.inference.allow_multiple_tool_calls);
    read(inference, "reasoningSummary", options.inference.reasoning_summary);
    read(inference, "contextTier", options.inference.context_tier);
    if (inference.contains("defaultProvider"))
        options.inference.default_provider = inference["defaultProvider"];
    read(inference, "providers", options.inference.providers);
    read(inference, "models", options.inference.models);
    read(inference, "enableCitations", options.inference.enable_citations);
    read(inference, "capabilities", options.inference.capabilities);

    const auto& experiments = group_of(config, "experiments");
    read(experiments, "enableExperimentalMode", options.experiments.enable_experimental_mode);
    read(experiments, "flags", options.experiments.flags);
    read(experiments, "assignments", options.experiments.assignments);

    const auto& tools = group_of(config, "tools");
    read(tools, "search", options.tools.search);
    read(tools, "imageGeneration", options.tools.image_generation);
    read(tools, "availableTools", options.tools.available_tools);
    read(tools, "excludedTools", options.tools.excluded_tools);
    read(tools, "largeOutput", options.tools.large_output);
    read(tools, "manageScheduleEnabled", options.tools.manage_schedule_enabled);

    const auto& canvases = group_of(config, "canvases");
    read(canvases, "definitions", options.canvases.declarations);
    read(canvases, "requestRenderer", options.canvases.request_renderer);
    read(canvases, "provider", options.canvases.provider);
    if constexpr (!is_create)
        read(canvases, "open", options.canvases.open);

    const auto& extensions = group_of(config, "extensions");
    read(extensions, "enabled", options.extensions.enabled);
    read(extensions, "sdkPath", options.extensions.sdk_path);
    read(extensions, "identity", options.extensions.identity);

    const auto& interaction = group_of(config, "interaction");
    read(interaction, "askUserVariant", options.interaction.ask_user_variant);

    const auto& agents = group_of(config, "agents");
    read(agents, "excludedBuiltinAgents", options.agents.excluded_builtin_agents);
    read(agents, "includedBuiltinAgents", options.agents.included_builtin_agents);
    read(agents, "definitions", options.agents.definitions);
    read(agents, "localDiscoveryOnly", options.agents.local_discovery_only);
    read(agents, "default", options.agents.default_agent);
    read(agents, "selected", options.agents.selected);
    read(agents, "directories", options.agents.directories);

    const auto& github = group_of(config, "github");
    read(github, "capi", options.github.capi);
    read(github, "telemetryEnabled", options.github.telemetry_enabled);
    read(github, "token", options.github.token);
    read(github, "fetchManagedSettings", options.github.fetch_managed_settings);
    read(github, "interactiveUser", options.github.interactive_user);
    read(github, "mcp", options.github.mcp);

    const auto& filesystem = group_of(config, "filesystem");
    read(filesystem, "trackFileChanges", options.filesystem.track_file_changes);
    if constexpr (is_create)
        read(filesystem, "trustWorkingDirectory", options.filesystem.trust_working_directory);
    read(filesystem, "additionalDirectories", options.filesystem.additional_directories);

    const auto& execution = group_of(config, "execution");
    read(execution, "limits", options.execution.limits);
    read(execution, "maxToolTurns", options.execution.max_tool_turns);
    read(execution, "autopilot", options.execution.autopilot);

    const auto& permissions = group_of(config, "permissions");
    read(permissions, "sandbox", options.permissions.sandbox);
    read(permissions, "mode", options.permissions.mode);

    const auto& mcp = group_of(config, "mcp");
    read(mcp, "enableApps", options.mcp.enable_apps);
    read(mcp, "servers", options.mcp.servers);
    read(mcp, "oauthTokenStorage", options.mcp.oauth_token_storage);
    read(mcp, "authClientIdMetadataUrl", options.mcp.auth_client_id_metadata_url);
    read(mcp, "disabledServers", options.mcp.disabled_servers);

    const auto& hooks = group_of(config, "hooks");
    read(hooks, "enableRepositoryHooks", options.hooks.enable_repository_hooks);
    read(hooks, "directories", options.hooks.directories);

    const auto& state = group_of(config, "state");
    read(state, "skipEmbeddingRetrieval", options.state.skip_embedding_retrieval);
    read(state, "embeddingCacheStorage", options.state.embedding_cache_storage);
    read(state, "enableCrossSessionStore", options.state.enable_cross_session_store);
    read(state, "infiniteSessions", options.state.infinite_sessions);
    read(state, "memory", options.state.memory);

    const auto& instructions = group_of(config, "instructions");
    if constexpr (is_create)
        read(instructions,
             "refreshCustomInstructions",
             options.instructions.refresh_custom_instructions);
    read(instructions, "organizationText", options.instructions.organization_text);
    read(instructions, "onDemandDiscovery", options.instructions.on_demand_discovery);
    read(instructions, "directories", options.instructions.directories);
    read(instructions, "skipCustomInstructions", options.instructions.skip_custom_instructions);

    const auto& git = group_of(config, "git");
    read(git, "enableHostGitOperations", options.git.enable_host_git_operations);
    read(git, "coauthorEnabled", options.git.coauthor_enabled);

    const auto& skills = group_of(config, "skills");
    read(skills, "enabled", options.skills.enabled);
    read(skills, "directories", options.skills.directories);
    read(skills, "disabledSkills", options.skills.disabled_skills);
    read(skills, "includedBuiltinSkills", options.skills.included_builtin_skills);

    const auto& events = group_of(config, "events");
    read(events, "streaming", options.events.streaming);
    read(events, "includeSubAgentStreamingEvents", options.events.include_sub_agent_streaming_events);

    const auto& plugins = group_of(config, "plugins");
    read(plugins, "directories", options.plugins.directories);
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

} // namespace

// NODEJS REFERENCE: client.ts groupHostSessionSettings
json group_ahp_host_settings(const json& settings)
{
    json root = json::object();
    json groups = json::object();
    if (settings.is_object())
    {
        const auto& paths = ahp_setting_paths();
        for (const auto& [name, value] : settings.items())
        {
            const auto it = paths.find(name);
            if (it != paths.end())
                groups[it->second.first][it->second.second] = value;
            else
                root[name == "configDir" ? std::string("configDirectory") : name] = value;
        }
    }
    for (const auto& [group, _] : groups.items())
        if (root.contains(group))
            throw std::invalid_argument("AHP host settings must not mix flat and grouped options");
    for (auto& [group, value] : groups.items())
        root[group] = value;
    return root;
}

CreateSessionOptions create_session_options_from_json(const json& config)
{
    CreateSessionOptions options;
    read_common(config, options);
    read(config, "sessionId", options.session_id);
    read(config, "name", options.name);
    read(config, "cloud", options.cloud);
    return options;
}

ResumeSessionOptions resume_session_options_from_json(const json& config)
{
    ResumeSessionOptions options;
    read_common(config, options);
    read(config, "allowTranscriptRecovery", options.allow_transcript_recovery);
    read(config, "workflows", options.workflows);
    read(config, "suppressResumeEvent", options.suppress_resume_event);
    read(config, "continuePendingWork", options.continue_pending_work);
    read(config, "requestedEnvironmentVariables", options.requested_environment_variables);
    return options;
}

// C# REFERENCE: Host.cs ContainsAhpConfig
// NODEJS REFERENCE: client.ts createHostSession contains
bool ahp_config_contains(const json& actual, const json& expected)
{
    if (!expected.is_object())
        return actual == expected;
    if (!actual.is_object())
        return false;
    for (const auto& [name, value] : expected.items())
    {
        const auto it = actual.find(name);
        if (it == actual.end())
        {
            // These default-false flags are omitted by the session request builder.
            if (value.is_boolean() && !value.get<bool>() &&
                (name == "streaming" || name == "enableApps" || name == "suppressResumeEvent"))
                continue;
            return false;
        }
        if (!ahp_config_contains(*it, value))
            return false;
    }
    return true;
}

// NODEJS REFERENCE: client.ts validateHostAuthentication expiresAt check
bool is_future_rfc3339(const std::string& value)
{
    static const std::regex pattern(
        R"(^(\d{4})-(\d{2})-(\d{2})[Tt](\d{2}):(\d{2}):(\d{2})(\.(\d+))?([Zz]|([+-])(\d{2}):(\d{2}))$)"
    );
    std::smatch m;
    if (!std::regex_match(value, m, pattern))
        return false;
    const auto num = [&m](int index) { return std::stoi(m[index].str()); };
    const int year = num(1), month = num(2), day = num(3);
    const int hour = num(4), minute = num(5), second = num(6);
    if (month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 60)
        return false;
    static constexpr int month_days[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (day > month_days[month - 1] || (month == 2 && day == 29 && !leap))
        return false;

    int64_t seconds = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) *
                          86400 +
                      hour * 3600 + minute * 60 + second;
    if (m[10].matched)
    {
        const int offset_hours = num(11), offset_minutes = num(12);
        if (offset_hours > 23 || offset_minutes > 59)
            return false;
        const int64_t offset = offset_hours * 3600 + offset_minutes * 60;
        seconds += m[10].str() == "+" ? -offset : offset;
    }
    int64_t nanos = 0;
    if (m[8].matched)
    {
        auto fraction = m[8].str().substr(0, 9);
        fraction.append(9 - fraction.size(), '0');
        nanos = std::stoll(fraction);
    }
    // Compare whole seconds first: a nanosecond time_point overflows past year 2262.
    const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now()
    );
    const auto now_seconds = std::chrono::floor<std::chrono::seconds>(now);
    const int64_t now_whole = now_seconds.time_since_epoch().count();
    if (seconds != now_whole)
        return seconds > now_whole;
    return nanos > (now - now_seconds).count();
}

} // namespace detail

} // namespace copilot
