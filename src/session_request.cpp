// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

// Wire builders for session.create / session.resume / session.options.update and the
// runtime launch arguments/environment.
// NODEJS REFERENCE: nodejs/src/client.ts createSession / resumeSessionWithInitialization /
// applyModeDefaults / updateSessionOptionsForMode / startCLIServer / buildEnvironment.

#include <copilot/canvas.hpp>
#include <copilot/client.hpp>
#include <copilot/workflow.hpp>
#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <type_traits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
extern "C" char** environ;
#endif

namespace copilot
{

namespace
{

template <typename T> void put(json& target, const char* key, const std::optional<T>& value)
{
    if (value)
        target[key] = *value;
}

// NODEJS REFERENCE: client.ts extractBearerTokenProviders
json to_wire_providers(const std::vector<NamedProviderConfig>& providers)
{
    json out = json::array();
    for (const auto& provider : providers)
    {
        json wire = provider;
        if (provider.bearer_token_provider)
            wire["hasBearerTokenProvider"] = true;
        out.push_back(std::move(wire));
    }
    return out;
}

// NODEJS REFERENCE: client.ts validateToolFilterList
void validate_tool_filter_list(const char* field, const std::optional<std::vector<std::string>>& list)
{
    if (!list)
        return;
    for (const auto& entry : *list)
        if (entry == "*")
            throw std::invalid_argument(
                std::string("Invalid ") + field +
                " entry '*': there is no bare wildcard. Use one or more of "
                "ToolSet().add_built_in(\"*\"), .add_mcp(\"*\"), or .add_custom(\"*\") to target "
                "a specific source."
            );
}

// NODEJS REFERENCE: client.ts getSystemMessageConfigForMode
std::optional<SystemMessageConfig>
system_message_for_mode(const std::optional<SystemMessageConfig>& supplied, ClientMode mode)
{
    if (mode != ClientMode::Empty)
        return supplied;
    const auto remove_env = SectionOverride{.action = SectionOverrideAction::Remove};
    if (!supplied)
        return SystemMessageConfig{
            .mode = SystemMessageMode::Customize,
            .sections = std::map<std::string, SectionOverride>{{"environment_context", remove_env}},
        };
    if (supplied->mode == SystemMessageMode::Replace)
        return supplied;
    if (supplied->mode == SystemMessageMode::Customize)
    {
        if (supplied->sections && supplied->sections->contains("environment_context"))
            return supplied;
        auto adjusted = *supplied;
        if (!adjusted.sections)
            adjusted.sections.emplace();
        (*adjusted.sections)["environment_context"] = remove_env;
        return adjusted;
    }
    return SystemMessageConfig{
        .mode = SystemMessageMode::Customize,
        .content = supplied->content,
        .sections = std::map<std::string, SectionOverride>{{"environment_context", remove_env}},
    };
}

// NODEJS REFERENCE: client.ts configDefaultsForMode / applyModeDefaults (caller values win)
template <typename Options> Options apply_mode_defaults(Options options, ClientMode mode)
{
    if (mode == ClientMode::Empty)
    {
        auto fill = [](auto& field, auto value)
        {
            if (!field)
                field = std::move(value);
        };
        fill(options.github.telemetry_enabled, false);
        fill(options.mcp.oauth_token_storage, std::string("in-memory"));
        fill(options.state.skip_embedding_retrieval, true);
        fill(options.state.embedding_cache_storage, std::string("in-memory"));
        fill(options.state.enable_cross_session_store, false);
        if (!options.state.memory)
            options.state.memory = MemoryConfiguration{.enabled = false};
        fill(options.instructions.on_demand_discovery, false);
        fill(options.hooks.enable_repository_hooks, false);
        fill(options.git.enable_host_git_operations, false);
        fill(options.skills.enabled, false);
        fill(options.agents.local_discovery_only, true);
    }
    options.system_message = system_message_for_mode(options.system_message, mode);
    return options;
}

std::optional<std::vector<std::string>> resolve_filter(
    const std::optional<ToolSet>& set, const std::optional<std::vector<std::string>>& list
)
{
    if (set)
        return set->to_vector();
    return list;
}

// NODEJS REFERENCE: client.ts validateSessionCredentials / requestedMemoryStore /
// createSession autoModeRecovery guard
template <typename Options> void validate_session_options(const Options& options)
{
    if (options.interaction.auto_mode_recovery.value_or(false) &&
        !options.interaction.on_auto_mode_switch_request)
        throw std::invalid_argument("autoModeRecovery requires onAutoModeSwitchRequest");
    if (options.github.token && options.github.token_provider)
        throw std::invalid_argument("github.token and github.tokenProvider are mutually exclusive");
    if (options.state.memory && options.state.memory->store_memory && !options.state.memory->enabled)
        throw std::invalid_argument("memory.storeMemory requires memory.enabled to be true.");
}

template <typename Options>
json build_session_request_common(
    const Options& original,
    ClientMode mode,
    bool telemetry_forwarding,
    const std::optional<std::string>& token_provider_registration_id
)
{
    constexpr bool is_create = std::is_same_v<Options, CreateSessionOptions>;
    validate_session_options(original);
    const auto options = apply_mode_defaults(original, mode);

    const auto available_tools =
        resolve_filter(options.tools.available_tool_set, options.tools.available_tools);
    const auto excluded_tools =
        resolve_filter(options.tools.excluded_tool_set, options.tools.excluded_tools);
    validate_tool_filter_list("availableTools", available_tools);
    validate_tool_filter_list("excludedTools", excluded_tools);
    if (mode == ClientMode::Empty && !available_tools)
        throw std::invalid_argument(
            "CopilotClient is in mode: 'empty' but the session config did not specify "
            "'availableTools'. Empty mode requires every session to explicitly opt into the "
            "tools it wants."
        );

    json request = json::object();
    put(request, "model", options.model);
    put(request, "clientName", options.client_name);
    put(request, "systemMessage", options.system_message);
    put(request, "workingDirectory", options.working_directory);
    put(request, "configDirectory", options.config_directory);
    put(request, "enableConfigDiscovery", options.enable_config_discovery);
    put(request, "extensionSources", options.extension_sources);
    put(request, "diagnostics", options.diagnostics);
    put(request, "lifecycleDiagnostics", options.lifecycle_diagnostics);

    json policy = json::object();
    put(policy, "allowedModels", options.policy.allowed_models);
    put(policy, "excludedManagedSettings", options.policy.excluded_managed_settings);
    put(policy, "enforceManagedModelDefaults", options.policy.enforce_managed_model_defaults);
    put(policy, "managedSettings", options.policy.managed_settings);
    request["policy"] = std::move(policy);

    json inference = json::object();
    put(inference, "reasoningEffort", options.inference.reasoning_effort);
    put(inference, "allowMultipleToolCalls", options.inference.allow_multiple_tool_calls);
    put(inference, "reasoningSummary", options.inference.reasoning_summary);
    put(inference, "contextTier", options.inference.context_tier);
    put(inference, "defaultProvider", options.inference.default_provider);
    if (options.inference.providers)
        inference["providers"] = to_wire_providers(*options.inference.providers);
    put(inference, "models", options.inference.models);
    put(inference, "enableCitations", options.inference.enable_citations);
    put(inference, "capabilities", options.inference.capabilities);
    request["inference"] = std::move(inference);

    json experiments = json::object();
    if (mode == ClientMode::Empty)
        experiments["enableExperimentalMode"] =
            options.experiments.enable_experimental_mode.value_or(false);
    else
        put(experiments, "enableExperimentalMode", options.experiments.enable_experimental_mode);
    put(experiments, "flags", options.experiments.flags);
    put(experiments, "assignments", options.experiments.assignments);
    request["experiments"] = std::move(experiments);

    json tools = json::object();
    if (!options.tools.definitions.empty())
    {
        json definitions = json::array();
        for (const auto& tool : options.tools.definitions)
            definitions.push_back(build_tool_definition(tool));
        tools["definitions"] = std::move(definitions);
    }
    put(tools, "search", options.tools.search);
    put(tools, "imageGeneration", options.tools.image_generation);
    put(tools, "availableTools", available_tools);
    put(tools, "excludedTools", excluded_tools);
    put(tools, "largeOutput", options.tools.large_output);
    request["tools"] = std::move(tools);

    json canvases = json::object();
    if (!options.canvases.definitions.empty() || options.canvases.declarations)
    {
        json definitions = json::array();
        for (const auto& canvas : options.canvases.definitions)
            if (canvas)
                definitions.push_back(canvas->declaration());
        if (options.canvases.declarations)
            for (const auto& declaration : *options.canvases.declarations)
                definitions.push_back(declaration);
        canvases["definitions"] = std::move(definitions);
    }
    put(canvases, "requestRenderer", options.canvases.request_renderer);
    put(canvases, "provider", options.canvases.provider);
    if constexpr (!is_create)
        put(canvases, "open", options.canvases.open);
    request["canvases"] = std::move(canvases);

    json extensions = json::object();
    put(extensions, "enabled", options.extensions.enabled);
    put(extensions, "sdkPath", options.extensions.sdk_path);
    put(extensions, "identity", options.extensions.identity);
    request["extensions"] = std::move(extensions);

    const auto& interaction_options = options.interaction;
    json interaction = json::object();
    if (!interaction_options.commands.empty())
    {
        json commands = json::array();
        for (const auto& command : interaction_options.commands)
            commands.push_back(
                {{"name", command.name}, {"description", command.description.value_or("")}}
            );
        interaction["commands"] = std::move(commands);
    }
    interaction["requestUserInput"] = static_cast<bool>(interaction_options.on_user_input_request);
    interaction["requestElicitation"] =
        static_cast<bool>(interaction_options.on_elicitation_request);
    put(interaction, "askUserVariant", interaction_options.ask_user_variant);
    interaction["requestExitPlanMode"] =
        static_cast<bool>(interaction_options.on_exit_plan_mode_request);
    interaction["requestAutoModeSwitch"] =
        static_cast<bool>(interaction_options.on_auto_mode_switch_request);
    if (interaction_options.on_shell_authentication_request)
        interaction["requestShellAuthentication"] = true;
    if (interaction_options.auto_mode_recovery.value_or(false))
        interaction["requestAutoModeRecovery"] = true;
    request["interaction"] = std::move(interaction);

    json agents = json::object();
    put(agents, "excludedBuiltinAgents", options.agents.excluded_builtin_agents);
    put(agents, "includedBuiltinAgents", options.agents.included_builtin_agents);
    // NODEJS REFERENCE: client.ts toWireCustomAgents (workingDirectory -> cwd is in to_json)
    put(agents, "definitions", options.agents.definitions);
    put(agents, "localDiscoveryOnly", options.agents.local_discovery_only);
    put(agents, "default", options.agents.default_agent);
    put(agents, "selected", options.agents.selected);
    put(agents, "directories", options.agents.directories);
    request["agents"] = std::move(agents);

    json github = json::object();
    put(github, "capi", options.github.capi);
    put(github, "telemetryEnabled", options.github.telemetry_enabled);
    put(github, "token", options.github.token);
    put(github, "tokenProviderRegistrationId", token_provider_registration_id);
    put(github, "fetchManagedSettings", options.github.fetch_managed_settings);
    put(github, "interactiveUser", options.github.interactive_user);
    put(github, "mcp", options.github.mcp);
    if (telemetry_forwarding)
        github["telemetryForwarding"] = true;
    request["github"] = std::move(github);

    json filesystem = json::object();
    put(filesystem, "trackFileChanges", options.filesystem.track_file_changes);
    if constexpr (is_create)
        put(filesystem, "trustWorkingDirectory", options.filesystem.trust_working_directory);
    put(filesystem, "additionalDirectories", options.filesystem.additional_directories);
    request["filesystem"] = std::move(filesystem);

    json execution = json::object();
    put(execution, "limits", options.execution.limits);
    put(execution, "autopilot", options.execution.autopilot);
    request["execution"] = std::move(execution);

    json permissions = json::object();
    permissions["request"] = static_cast<bool>(options.permissions.on_request) ||
                             static_cast<bool>(options.permissions.on_request_with_context);
    put(permissions, "sandbox", options.permissions.sandbox);
    put(permissions, "mode", options.permissions.mode);
    request["permissions"] = std::move(permissions);

    json mcp = json::object();
    if (options.mcp.enable_apps.value_or(false))
        mcp["enableApps"] = true;
    // NODEJS REFERENCE: client.ts toWireMcpServers (workingDirectory -> cwd is in to_json)
    put(mcp, "servers", options.mcp.servers);
    put(mcp, "oauthTokenStorage", options.mcp.oauth_token_storage);
    put(mcp, "authClientIdMetadataUrl", options.mcp.auth_client_id_metadata_url);
    mcp["envValueMode"] = "direct";
    put(mcp, "disabledServers", options.mcp.disabled_servers);
    request["mcp"] = std::move(mcp);

    json hooks = json::object();
    hooks["hasHandlers"] = options.hooks.handlers.has_value() && options.hooks.handlers->has_any();
    put(hooks, "enableRepositoryHooks", options.hooks.enable_repository_hooks);
    put(hooks, "directories", options.hooks.directories);
    request["hooks"] = std::move(hooks);

    json state = json::object();
    put(state, "skipEmbeddingRetrieval", options.state.skip_embedding_retrieval);
    put(state, "embeddingCacheStorage", options.state.embedding_cache_storage);
    put(state, "enableCrossSessionStore", options.state.enable_cross_session_store);
    put(state, "infiniteSessions", options.state.infinite_sessions);
    put(state, "memory", options.state.memory);
    if (options.state.memory && options.state.memory->store_memory)
        state["hasMemoryStore"] = true;
    request["state"] = std::move(state);

    json instructions = json::object();
    if constexpr (is_create)
        put(instructions,
            "refreshCustomInstructions",
            options.instructions.refresh_custom_instructions);
    put(instructions, "organizationText", options.instructions.organization_text);
    put(instructions, "onDemandDiscovery", options.instructions.on_demand_discovery);
    put(instructions, "directories", options.instructions.directories);
    request["instructions"] = std::move(instructions);

    json git = json::object();
    put(git, "enableHostGitOperations", options.git.enable_host_git_operations);
    request["git"] = std::move(git);

    json skills = json::object();
    put(skills, "enabled", options.skills.enabled);
    put(skills, "directories", options.skills.directories);
    put(skills, "disabledSkills", options.skills.disabled_skills);
    if (options.skills.provider)
        skills["hasProvider"] = true;
    request["skills"] = std::move(skills);

    json events = json::object();
    put(events, "streaming", options.events.streaming);
    events["includeSubAgentStreamingEvents"] =
        options.events.include_sub_agent_streaming_events.value_or(true);
    request["events"] = std::move(events);

    json plugins = json::object();
    put(plugins, "directories", options.plugins.directories);
    request["plugins"] = std::move(plugins);

    return request;
}

// NODEJS REFERENCE: client.ts updateSessionOptionsForMode
template <typename Options>
std::optional<json> build_options_update(const Options& options, ClientMode mode)
{
    json patch = json::object();
    if (mode == ClientMode::Empty)
    {
        patch["instructions"] = {
            {"skipCustomInstructions", options.instructions.skip_custom_instructions.value_or(true)}
        };
        patch["agents"] = {{"localDiscoveryOnly", options.agents.local_discovery_only.value_or(true)}
        };
        patch["git"] = {{"coauthorEnabled", options.git.coauthor_enabled.value_or(false)}};
        patch["tools"] = {
            {"manageScheduleEnabled", options.tools.manage_schedule_enabled.value_or(false)}
        };
        patch["plugins"] = {{"installed", json::array()}};
        patch["skills"] = {
            {"includedBuiltinSkills",
             options.skills.included_builtin_skills.value_or(std::vector<std::string>{})}
        };
    }
    else
    {
        if (options.instructions.skip_custom_instructions)
            patch["instructions"] = {
                {"skipCustomInstructions", *options.instructions.skip_custom_instructions}
            };
        if (options.agents.local_discovery_only)
            patch["agents"] = {{"localDiscoveryOnly", *options.agents.local_discovery_only}};
        if (options.git.coauthor_enabled)
            patch["git"] = {{"coauthorEnabled", *options.git.coauthor_enabled}};
        if (options.tools.manage_schedule_enabled)
            patch["tools"] = {{"manageScheduleEnabled", *options.tools.manage_schedule_enabled}};
        if (options.skills.included_builtin_skills)
            patch["skills"] = {{"includedBuiltinSkills", *options.skills.included_builtin_skills}};
    }
    if (patch.empty())
        return std::nullopt;
    return patch;
}

const RuntimeConnection& runtime_of(const ClientOptions& options, RuntimeConnection& storage)
{
    if (options.runtime)
        return *options.runtime;
    storage = RuntimeConnection::for_default();
    return storage;
}

bool uses_logged_in_user(const ClientOptions& options)
{
    return options.github.use_logged_in_user.value_or(!options.github.token.has_value());
}

} // namespace

// NODEJS REFERENCE: client.ts createSession tools.definitions mapping
json build_tool_definition(const Tool& tool)
{
    json result{{"name", tool.name}, {"description", tool.description}};
    if (tool.title)
        result["title"] = *tool.title;
    if (!tool.parameters_schema.is_null())
        result["parameters"] = tool.parameters_schema;
    result["overridesBuiltInTool"] = tool.overrides_built_in_tool;
    result["skipPermission"] = tool.skip_permission;
    result["hasHandler"] = static_cast<bool>(tool.handler);
    if (tool.defer)
        result["defer"] = *tool.defer;
    if (tool.metadata)
        result["metadata"] = *tool.metadata;
    result["isTerminal"] = tool.is_terminal;
    result["idempotent"] = tool.idempotent;
    if (tool.tool_namespace)
        result["namespace"] = *tool.tool_namespace;
    return result;
}

json build_session_create_request(
    const CreateSessionOptions& options,
    ClientMode mode,
    bool telemetry_forwarding,
    const std::optional<std::string>& token_provider_registration_id
)
{
    if (options.cloud)
    {
        if (options.skills.provider)
            throw std::invalid_argument("Skill providers are not supported for cloud sessions.");
        if (options.state.memory && options.state.memory->store_memory)
            throw std::invalid_argument("Memory stores are not supported for cloud sessions.");
    }
    json request = build_session_request_common(
        options, mode, telemetry_forwarding, token_provider_registration_id
    );
    put(request, "sessionId", options.session_id);
    put(request, "name", options.name);
    put(request, "cloud", options.cloud);
    return request;
}

json build_session_resume_request(
    const std::string& session_id,
    const ResumeSessionOptions& options,
    ClientMode mode,
    bool telemetry_forwarding,
    const std::optional<std::string>& token_provider_registration_id,
    std::optional<bool> permission_request
)
{
    json request = build_session_request_common(
        options, mode, telemetry_forwarding, token_provider_registration_id
    );
    request["sessionId"] = session_id;
    // Upstream resume always requests permission routing unless the extension join path
    // supplied no handler.
    request["permissions"]["request"] = permission_request.value_or(true);
    put(request, "allowTranscriptRecovery", options.allow_transcript_recovery);
    if (!options.workflow_objects.empty())
    {
        json workflows = json::array();
        for (const auto& workflow : options.workflow_objects)
            if (workflow)
                workflows.push_back(workflow->meta);
        request["workflows"] = std::move(workflows);
    }
    else
    {
        put(request, "workflows", options.workflows);
    }
    put(request, "suppressResumeEvent", options.suppress_resume_event);
    put(request, "continuePendingWork", options.continue_pending_work);
    put(request, "requestedEnvironmentVariables", options.requested_environment_variables);
    return request;
}

std::optional<json> build_session_options_update(const CreateSessionOptions& options, ClientMode mode)
{
    return build_options_update(options, mode);
}

std::optional<json> build_session_options_update(const ResumeSessionOptions& options, ClientMode mode)
{
    return build_options_update(options, mode);
}

// NODEJS REFERENCE: client.ts startCLIServer argument assembly
std::vector<std::string> build_cli_command_args(const ClientOptions& options)
{
    RuntimeConnection storage;
    const auto& runtime = runtime_of(options, storage);
    std::vector<std::string> args;
    if (runtime.args)
        args.insert(args.end(), runtime.args->begin(), runtime.args->end());
    args.push_back("--headless");
    args.push_back("--no-auto-update");
    if (options.observability.log_level)
    {
        args.push_back("--log-level");
        args.push_back(json(*options.observability.log_level).get<std::string>());
    }
    if (runtime.kind == RuntimeConnectionKind::SpawnedStdio)
    {
        args.push_back("--stdio");
    }
    else if (runtime.kind == RuntimeConnectionKind::SpawnedTcp && runtime.port > 0)
    {
        args.push_back("--port");
        args.push_back(std::to_string(runtime.port));
    }
    if (options.github.token)
    {
        args.push_back("--auth-token-env");
        args.push_back("COPILOT_SDK_AUTH_TOKEN");
    }
    if (!uses_logged_in_user(options))
        args.push_back("--no-auto-login");
    if (runtime.session_idle_timeout_seconds && *runtime.session_idle_timeout_seconds > 0)
    {
        args.push_back("--session-idle-timeout");
        args.push_back(std::to_string(*runtime.session_idle_timeout_seconds));
    }
    return args;
}

// NODEJS REFERENCE: client.ts startInProcessRuntime argument assembly
std::vector<std::string> build_in_process_args(const ClientOptions& options)
{
    RuntimeConnection storage;
    const auto& runtime = runtime_of(options, storage);
    std::vector<std::string> args;
    if (options.observability.log_level)
    {
        args.push_back("--log-level");
        args.push_back(json(*options.observability.log_level).get<std::string>());
    }
    if (options.github.token)
    {
        args.push_back("--auth-token-env");
        args.push_back("COPILOT_SDK_AUTH_TOKEN");
    }
    if (!uses_logged_in_user(options))
        args.push_back("--no-auto-login");
    if (runtime.session_idle_timeout_seconds && *runtime.session_idle_timeout_seconds > 0)
    {
        args.push_back("--session-idle-timeout");
        args.push_back(std::to_string(*runtime.session_idle_timeout_seconds));
    }
    if (auto raw = detail::env_value("GITHUB_COPILOT_INTEGRATION_ID"))
    {
        std::string id = std::move(*raw);
        const auto first = id.find_first_not_of(" \t\r\n");
        const auto last = id.find_last_not_of(" \t\r\n");
        id = first == std::string::npos ? std::string() : id.substr(first, last - first + 1);
        if (!id.empty())
            args.push_back("--cli-login-sync-integration-id=" + id);
    }
    return args;
}

namespace
{
// Snapshot of this process's environment. Windows names are case-insensitive, so they
// are upper-cased (as nodejs normalizeEnvironment does) to collapse aliases like Path/PATH.
std::map<std::string, std::string> current_process_environment()
{
    std::map<std::string, std::string> env;
#ifdef _WIN32
    LPWCH block = GetEnvironmentStringsW();
    if (!block)
        return env;
    for (LPWCH entry = block; *entry; entry += wcslen(entry) + 1)
    {
        const int len = static_cast<int>(wcslen(entry));
        const int size = WideCharToMultiByte(CP_UTF8, 0, entry, len, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, entry, len, utf8.data(), size, nullptr, nullptr);
        // Skip drive-cwd pseudo variables such as "=C:=C:\\".
        const auto eq = utf8.find('=', 1);
        if (utf8.empty() || utf8[0] == '=' || eq == std::string::npos)
            continue;
        std::string name = utf8.substr(0, eq);
        for (auto& ch : name)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        env[name] = utf8.substr(eq + 1);
    }
    FreeEnvironmentStringsW(block);
#else
    for (char** entry = environ; entry && *entry; ++entry)
    {
        const std::string value = *entry;
        const auto eq = value.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        env.emplace(value.substr(0, eq), value.substr(eq + 1));
    }
#endif
    return env;
}
} // namespace

// NODEJS REFERENCE: client.ts buildRuntimeEnv
// C# REFERENCE: Client.cs BuildInProcessRuntimeEnvironment
std::map<std::string, std::string> build_cli_environment(
    const ClientOptions& options, const std::optional<std::string>& effective_connection_token
)
{
    RuntimeConnection storage;
    const auto& runtime = runtime_of(options, storage);
    std::map<std::string, std::string> env;
    if (runtime.environment)
        env = *runtime.environment;
    else if (runtime.kind == RuntimeConnectionKind::InProcess)
        // The FFI host receives the complete environment snapshot, not overrides; a
        // spawned child instead inherits the parent block and merges these on top.
        env = current_process_environment();
    env.erase("NODE_DEBUG");

    if (options.github.token)
        env["COPILOT_SDK_AUTH_TOKEN"] = *options.github.token;
    const auto token =
        effective_connection_token ? effective_connection_token : runtime.connection_token;
    if (token)
        env["COPILOT_CONNECTION_TOKEN"] = *token;
    if (runtime.base_directory)
    {
        env["COPILOT_HOME"] = *runtime.base_directory;
        if (runtime.kind == RuntimeConnectionKind::InProcess)
            env["COPILOT_CACHE_HOME"] =
                (std::filesystem::path(*runtime.base_directory) / "cache").string();
    }
    if (options.mode == ClientMode::Empty)
        env["COPILOT_DISABLE_KEYTAR"] = "1";
    else
        env["COPILOT_RUNTIME_PROCESS_FILE_LOGGING"] = "1";

    if (options.observability.telemetry)
    {
        const auto& telemetry = *options.observability.telemetry;
        env["COPILOT_OTEL_ENABLED"] = "true";
        if (telemetry.otlp_endpoint)
            env["OTEL_EXPORTER_OTLP_ENDPOINT"] = *telemetry.otlp_endpoint;
        if (telemetry.otlp_protocol)
            env["OTEL_EXPORTER_OTLP_PROTOCOL"] = *telemetry.otlp_protocol;
        if (telemetry.file_path)
            env["COPILOT_OTEL_FILE_EXPORTER_PATH"] = *telemetry.file_path;
        if (telemetry.exporter_type)
            env["COPILOT_OTEL_EXPORTER_TYPE"] = *telemetry.exporter_type;
        if (telemetry.source_name)
            env["COPILOT_OTEL_SOURCE_NAME"] = *telemetry.source_name;
        if (telemetry.capture_content)
        {
            env["OTEL_INSTRUMENTATION_GENAI_CAPTURE_CONTENT"] =
                *telemetry.capture_content ? "SPAN_ONLY" : "NO_CONTENT";
            env["OTEL_INSTRUMENTATION_GENAI_CAPTURE_MESSAGE_CONTENT"] =
                *telemetry.capture_content ? "true" : "false";
        }
    }
    return env;
}

} // namespace copilot
