// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cctype>
#include <chrono>
#include <copilot/canvas.hpp>
#include <copilot/factory.hpp>
#include <copilot/client.hpp>
#include <copilot/rpc_methods.hpp>
#include <copilot/request_handler.hpp>
#include <copilot/session.hpp>
#include <copilot/transport_ffi.hpp>
#include <cstdio>
#include <filesystem>
#include <random>
#include <regex>
#include <thread>

namespace copilot
{

// =============================================================================
// Request Builder Helpers (exposed for unit testing)
// =============================================================================

namespace
{
std::string generate_uuid_v4()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t lo = dist(gen);
    uint64_t hi = dist(gen);
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buf[37];
    std::snprintf(
        buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>((hi >> 32) & 0xFFFFFFFFULL),
        static_cast<unsigned>((hi >> 16) & 0xFFFFULL),
        static_cast<unsigned>(hi & 0xFFFFULL),
        static_cast<unsigned>((lo >> 48) & 0xFFFFULL),
        static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
    return buf;
}

json serialize_tool(const Tool& tool)
{
    json result{{"name", tool.name}, {"description", tool.description}};
    if (!tool.parameters_schema.is_null())
        result["parameters"] = tool.parameters_schema;
    if (tool.overrides_built_in_tool)
        result["overridesBuiltInTool"] = true;
    if (tool.skip_permission)
        result["skipPermission"] = true;
    if (tool.defer)
        result["defer"] = *tool.defer;
    if (tool.metadata)
        result["metadata"] = *tool.metadata;
    if (tool.is_terminal)
        result["isTerminal"] = true;
    return result;
}

void validate_tool_filters(const std::string& name, const std::vector<std::string>& filters)
{
    for (const auto& filter : filters)
    {
        if (filter == "*")
            throw std::invalid_argument(
                name + " does not accept bare '*'; use builtin:*, mcp:*, or custom:*"
            );
    }
}

template <typename Config>
void append_session_config(json& request, const Config& config, ClientMode mode)
{
    auto copy = [&](const char* key, const auto& value)
    {
        if (value)
            request[key] = *value;
    };

    if (config.model)
        request["model"] = *config.model;
    else if (config.auto_byok_from_env)
        if (auto value = ProviderConfig::model_from_env())
            request["model"] = *value;

    copy("clientName", config.client_name);
    copy("reasoningEffort", config.reasoning_effort);
    copy("reasoningSummary", config.reasoning_summary);
    copy("contextTier", config.context_tier);
    copy("modelCapabilities", config.model_capabilities);
    copy("largeOutput", config.large_output);
    copy("systemMessage", config.system_message);
    copy("toolSearch", config.tool_search);
    copy("canvases", config.canvases);
    if (!config.canvas_objects.empty())
    {
        if (!request.contains("canvases"))
            request["canvases"] = json::array();
        for (const auto& canvas : config.canvas_objects)
            if (canvas)
                request["canvases"].push_back(canvas->declaration());
    }
    copy("requestCanvasRenderer", config.request_canvas_renderer);
    copy("requestExtensions", config.request_extensions);
    copy("extensionSdkPath", config.extension_sdk_path);
    copy("extensionInfo", config.extension_info);
    copy("canvasProvider", config.canvas_provider);
    copy("commands", config.commands);

    if (!config.tools.empty())
    {
        json tool_defs = json::array();
        for (const auto& tool : config.tools)
            tool_defs.push_back(serialize_tool(tool));
        request["tools"] = tool_defs;
    }

    std::optional<std::vector<std::string>> available = config.available_tools;
    std::optional<std::vector<std::string>> excluded = config.excluded_tools;
    if (config.available_tool_set)
        available = config.available_tool_set->to_vector();
    if (config.excluded_tool_set)
        excluded = config.excluded_tool_set->to_vector();
    if (available)
    {
        validate_tool_filters("available_tools", *available);
        request["availableTools"] = *available;
    }
    else if (mode == ClientMode::Empty)
    {
        throw std::invalid_argument(
            "ClientMode::Empty requires an explicit available_tools or available_tool_set"
        );
    }
    if (excluded)
    {
        validate_tool_filters("excluded_tools", *excluded);
        request["excludedTools"] = *excluded;
    }
    request["toolFilterPrecedence"] = "excluded";

    if (config.streaming)
        request["streaming"] = true;
    request["includeSubAgentStreamingEvents"] =
        config.include_sub_agent_streaming_events.value_or(true);

    if (config.provider)
        request["provider"] = *config.provider;
    else if (config.auto_byok_from_env)
        if (auto value = ProviderConfig::from_env())
            request["provider"] = *value;

    copy("capi", config.capi);
    copy("providers", config.providers);
    copy("models", config.models);
    copy("featureFlags", config.feature_flags);
    copy("authClientIdMetadataUrl", config.auth_client_id_metadata_url);
    copy("enableSessionTelemetry", config.enable_session_telemetry);
    copy("enableCitations", config.enable_citations);
    copy("enableFileChangeTracking", config.enable_file_change_tracking);
    copy("sessionLimits", config.session_limits);
    copy("excludedBuiltinAgents", config.excluded_builtin_agents);
    copy("workingDirectory", config.working_directory);
    copy("additionalDirectories", config.additional_directories);
    copy("mcpServers", config.mcp_servers);
    copy("mcpOAuthTokenStorage", config.mcp_oauth_token_storage);
    copy("customAgents", config.custom_agents);
    copy("customAgentsLocalOnly", config.custom_agents_local_only);
    copy("defaultAgent", config.default_agent);
    copy("agent", config.agent);
    copy("configDir", config.config_dir);
    copy("enableConfigDiscovery", config.enable_config_discovery);
    copy("skipEmbeddingRetrieval", config.skip_embedding_retrieval);
    copy("embeddingCacheStorage", config.embedding_cache_storage);
    copy("organizationCustomInstructions", config.organization_custom_instructions);
    copy("enableOnDemandInstructionDiscovery", config.enable_on_demand_instruction_discovery);
    copy("enableFileHooks", config.enable_file_hooks);
    copy("enableHostGitOperations", config.enable_host_git_operations);
    copy("enableSessionStore", config.enable_session_store);
    copy("enableSkills", config.enable_skills);
    copy("skillDirectories", config.skill_directories);
    copy("pluginDirectories", config.plugin_directories);
    copy("instructionDirectories", config.instruction_directories);
    copy("disabledSkills", config.disabled_skills);
    copy("disabledMcpServers", config.disabled_mcp_servers);
    copy("infiniteSessions", config.infinite_sessions);
    copy("memory", config.memory);
    copy("gitHubToken", config.github_token);
    copy("remoteSession", config.remote_session);
    copy("expAssignments", config.exp_assignments);
    copy("enableManagedSettings", config.enable_managed_settings);
    copy("managedSettings", config.managed_settings);
    copy("skipCustomInstructions", config.skip_custom_instructions);
    copy("coauthorEnabled", config.coauthor_enabled);
    copy("manageScheduleEnabled", config.manage_schedule_enabled);

    if (config.enable_mcp_apps.value_or(false))
        request["requestMcpApps"] = true;
    copy("githubMcpToolConfig", config.github_mcp_tool_config);

    request["requestPermission"] =
        config.on_permission_request.has_value() ||
        config.on_permission_request_with_context.has_value();
    request["requestUserInput"] = config.on_user_input_request.has_value();
    request["requestElicitation"] = config.on_elicitation_request.has_value();
    request["requestExitPlanMode"] = config.on_exit_plan_mode.has_value();
    request["requestAutoModeSwitch"] = config.on_auto_mode_switch.has_value();
    request["hooks"] = config.hooks.has_value() && config.hooks->has_any();
    request["envValueMode"] = "direct";

    if (config.enable_experimental_mode)
        request["isExperimentalMode"] = *config.enable_experimental_mode;
    else if (mode == ClientMode::Empty)
        request["isExperimentalMode"] = false;

    if (mode == ClientMode::Empty)
    {
        if (!config.enable_session_telemetry)
            request["enableSessionTelemetry"] = false;
        if (!config.mcp_oauth_token_storage)
            request["mcpOAuthTokenStorage"] = "in-memory";
        if (!config.skip_embedding_retrieval)
            request["skipEmbeddingRetrieval"] = true;
        if (!config.embedding_cache_storage)
            request["embeddingCacheStorage"] = "in-memory";
        if (!config.enable_on_demand_instruction_discovery)
            request["enableOnDemandInstructionDiscovery"] = false;
        if (!config.enable_file_hooks)
            request["enableFileHooks"] = false;
        if (!config.enable_host_git_operations)
            request["enableHostGitOperations"] = false;
        if (!config.enable_session_store)
            request["enableSessionStore"] = false;
        if (!config.enable_skills)
            request["enableSkills"] = false;
        if (!config.memory)
            request["memory"] = json{{"enabled", false}};
        if (!config.custom_agents_local_only)
            request["customAgentsLocalOnly"] = true;
        if (!config.skip_custom_instructions)
            request["skipCustomInstructions"] = true;
        if (!config.coauthor_enabled)
            request["coauthorEnabled"] = false;
        if (!config.manage_schedule_enabled)
            request["manageScheduleEnabled"] = false;
    }
}

} // namespace

json build_session_create_request(const SessionConfig& config, ClientMode mode)
{
    json request = json::object();
    append_session_config(request, config, mode);
    if (config.session_id)
        request["sessionId"] = *config.session_id;
    if (config.cloud)
        request["cloud"] = *config.cloud;
    return request;
}

json build_session_resume_request(
    const std::string& session_id,
    const ResumeSessionConfig& config,
    ClientMode mode)
{
    json request{{"sessionId", session_id}};
    append_session_config(request, config, mode);
    if (config.disable_resume)
        request["disableResume"] = true;
    if (config.continue_pending_work)
        request["continuePendingWork"] = true;
    if (config.open_canvases)
        request["openCanvases"] = *config.open_canvases;
    if (!config.factory_objects.empty())
    {
        request["factories"] = json::array();
        for (const auto& factory : config.factory_objects)
            if (factory)
                request["factories"].push_back(factory->meta);
    }
    else if (config.factories)
    {
        request["factories"] = *config.factories;
    }
    if (config.requested_environment_variables)
        request["requestedEnvironmentVariables"] =
            *config.requested_environment_variables;
    return request;
}

// =============================================================================
// CLI Process Launch Helpers (exposed for unit testing)
// =============================================================================

std::vector<std::string> build_cli_command_args(const ClientOptions& options)
{
    std::vector<std::string> args;
    const auto connection_args =
        options.connection && options.connection->args
            ? options.connection->args
            : options.cli_args;
    if (connection_args)
        args.insert(args.end(), connection_args->begin(), connection_args->end());
    args.push_back("--server");
    args.push_back("--log-level");
    args.push_back(json(options.log_level).get<std::string>());

    const auto kind = options.connection
                          ? options.connection->kind
                          : (options.use_stdio ? RuntimeConnectionKind::Stdio
                                               : RuntimeConnectionKind::Tcp);
    if (kind == RuntimeConnectionKind::Stdio)
    {
        args.push_back("--stdio");
    }
    else if (kind == RuntimeConnectionKind::Tcp)
    {
        const int port =
            options.connection && options.connection->port
                ? *options.connection->port
                : options.port;
        if (port > 0)
        {
            args.push_back("--port");
            args.push_back(std::to_string(port));
        }
    }

    // Session idle timeout (forwarded as CLI flag; ignored by server when 0/absent).
    if (options.session_idle_timeout_seconds.has_value() &&
        *options.session_idle_timeout_seconds > 0)
    {
        args.push_back("--session-idle-timeout");
        args.push_back(std::to_string(*options.session_idle_timeout_seconds));
    }

    // Remote session support (Mission Control integration).
    if (options.remote)
        args.push_back("--remote");

    return args;
}

std::map<std::string, std::string> build_cli_environment(const ClientOptions& options)
{
    std::map<std::string, std::string> env;
    if (options.connection && options.connection->environment)
        env = *options.connection->environment;
    else if (options.environment.has_value())
        env = *options.environment;

    // Remove NODE_DEBUG to avoid debug output interfering with JSON-RPC.
    env.erase("NODE_DEBUG");

    if (options.github_token.has_value())
        env["COPILOT_SDK_AUTH_TOKEN"] = *options.github_token;

    const auto connection_token =
        options.connection && options.connection->connection_token
            ? options.connection->connection_token
            : options.tcp_connection_token;
    if (connection_token)
        env["COPILOT_CONNECTION_TOKEN"] = *connection_token;

    const auto home = options.base_directory ? options.base_directory : options.copilot_home;
    if (home)
        env["COPILOT_HOME"] = *home;

    if (options.telemetry)
    {
        const auto& telemetry = *options.telemetry;
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
            env["OTEL_INSTRUMENTATION_GENAI_CAPTURE_MESSAGE_CONTENT"] =
                *telemetry.capture_content ? "true" : "false";
        }
    }

    return env;
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

Client::Client(ClientOptions options) : options_(std::move(options))
{
    if (options_.working_directory && !options_.cwd)
        options_.cwd = options_.working_directory;
    if (options_.base_directory && !options_.copilot_home)
        options_.copilot_home = options_.base_directory;
    if (options_.enable_remote_sessions)
        options_.remote = *options_.enable_remote_sessions;

    // A token provider is addressed by an opaque registration id that the caller installs
    // with the runtime as a `token-provider` AuthInfo; mint it once per client.
    if (options_.github_token_provider)
        github_token_registration_id_ = generate_uuid_v4();

    if (options_.builtin_plugin_directories)
    {
        for (const auto& path : *options_.builtin_plugin_directories)
            if (!std::filesystem::path(path).is_absolute())
                throw std::invalid_argument(
                    "builtin_plugin_directories must contain only absolute paths"
                );
    }

    if (options_.mode == ClientMode::Empty && !options_.base_directory &&
        !options_.copilot_home && !options_.session_fs)
    {
        throw std::invalid_argument(
            "ClientMode::Empty requires base_directory or session_fs"
        );
    }

    if (options_.session_fs &&
        options_.session_fs->conventions != "windows" &&
        options_.session_fs->conventions != "posix")
    {
        throw std::invalid_argument(
            "session_fs.conventions must be 'windows' or 'posix'"
        );
    }

    if (options_.connection)
    {
        const auto& connection = *options_.connection;
        if (connection.environment && options_.environment)
            throw std::invalid_argument(
                "connection.environment cannot be combined with ClientOptions::environment"
            );
        switch (connection.kind)
        {
        case RuntimeConnectionKind::Stdio:
            options_.use_stdio = true;
            options_.cli_path = connection.path;
            options_.cli_args = connection.args;
            if (connection.environment)
                options_.environment = connection.environment;
            break;
        case RuntimeConnectionKind::Tcp:
            options_.use_stdio = false;
            options_.port = connection.port.value_or(0);
            options_.tcp_connection_token = connection.connection_token;
            options_.cli_path = connection.path;
            options_.cli_args = connection.args;
            if (connection.environment)
                options_.environment = connection.environment;
            break;
        case RuntimeConnectionKind::Uri:
            options_.use_stdio = false;
            options_.cli_url = connection.url;
            options_.tcp_connection_token = connection.connection_token;
            break;
        case RuntimeConnectionKind::InProcess:
            options_.use_stdio = false;
            options_.cli_path = connection.path;
            if (options_.working_directory || options_.environment || options_.telemetry)
                throw std::invalid_argument(
                    "in-process runtime does not support working_directory, environment, "
                    "or telemetry options"
                );
            break;
        case RuntimeConnectionKind::ParentProcess:
            options_.use_stdio = true;
            break;
        }
    }

    // Validate mutually exclusive options
    if (options_.cli_url.has_value() && (options_.use_stdio || options_.cli_path.has_value()))
        throw std::invalid_argument("cli_url is mutually exclusive with use_stdio and cli_path");

    // Validate auth options with external server
    if (options_.cli_url.has_value())
    {
        if (options_.github_token.has_value())
            throw std::invalid_argument(
                "github_token cannot be used with cli_url "
                "(external server manages its own auth)");
        if (options_.use_logged_in_user.has_value())
            throw std::invalid_argument(
                "use_logged_in_user cannot be used with cli_url "
                "(external server manages its own auth)");
    }

    // Validate tcp_connection_token usage (matches upstream nodejs SDK v0.1.49):
    // token requires TCP transport (stdio is pre-authenticated by pipes).
    if (options_.tcp_connection_token.has_value())
    {
        if (options_.tcp_connection_token->empty())
            throw std::invalid_argument("tcp_connection_token must be a non-empty string");
        if (options_.use_stdio)
            throw std::invalid_argument(
                "tcp_connection_token cannot be used with use_stdio = true");
    }

    // Smart default for use_logged_in_user (only when managing our own server)
    if (!options_.cli_url.has_value() && !options_.use_logged_in_user.has_value())
        options_.use_logged_in_user = !options_.github_token.has_value();

    // Auto-generate a UUID for the TCP connection token when the SDK spawns its
    // own CLI in TCP mode and no token was provided. Mirrors nodejs effective-
    // ConnectionToken logic (so loopback listeners are safe by default).
    const bool is_in_process =
        options_.connection &&
        options_.connection->kind == RuntimeConnectionKind::InProcess;
    if (!options_.cli_url.has_value() && !options_.use_stdio && !is_in_process &&
        !options_.tcp_connection_token.has_value())
    {
        // Simple UUID v4 generator (RFC 4122, 122 random bits).
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        uint64_t lo = dist(gen);
        uint64_t hi = dist(gen);
        // Set version (4) and variant (10xx) bits per RFC 4122.
        hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
        lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
        char buf[37];
        std::snprintf(
            buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
            static_cast<unsigned>((hi >> 32) & 0xFFFFFFFFULL),
            static_cast<unsigned>((hi >> 16) & 0xFFFFULL),
            static_cast<unsigned>(hi & 0xFFFFULL),
            static_cast<unsigned>((lo >> 48) & 0xFFFFULL),
            static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL)
        );
        options_.tcp_connection_token = std::string(buf);
    }

    // Parse CLI URL if provided
    if (options_.cli_url.has_value())
        parse_cli_url(*options_.cli_url);
}

Client::~Client()
{
    force_stop();
}

// =============================================================================
// URL Parsing
// =============================================================================

void Client::parse_cli_url(const std::string& url)
{
    // If it's just a port number. The whole string must be digits: std::stoi stops at the
    // first non-digit, so "127.0.0.1:54321" would otherwise parse as port 127 on localhost.
    const bool all_digits =
        !url.empty() && std::all_of(url.begin(), url.end(), [](unsigned char c) { return std::isdigit(c) != 0; });

    if (all_digits)
    {
        try
        {
            int port = std::stoi(url);
            if (port > 0 && port <= 65535)
            {
                parsed_host_ = "localhost";
                parsed_port_ = port;
                return;
            }
        }
        catch (...)
        {
        }
    }

    // Check for scheme
    std::string url_to_parse = url;
    if (url.find("://") == std::string::npos)
        url_to_parse = "https://" + url;

    // Parse host:port. The bracketed alternative comes first so an IPv6 literal
    // ("[::1]:4000") matches as a whole -- [^:/]+ stops at the address's own
    // colons and would otherwise reject it or take only the first group.
    std::regex url_regex(R"((?:https?://)?(\[[^\]]+\]|[^:/]+)(?::(\d+))?)");
    std::smatch match;
    if (std::regex_match(url_to_parse, match, url_regex))
    {
        std::string host = match[1].str();
        // Store the bare address: brackets are URL syntax for disambiguating the
        // port, and the socket layer wants the address without them.
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
            host = host.substr(1, host.size() - 2);
        parsed_host_ = host;
        if (match[2].matched)
        {
            parsed_port_ = std::stoi(match[2].str());
        }
        else
        {
            // Default to 443 for https, 80 for http
            parsed_port_ = (url_to_parse.find("https://") == 0) ? 443 : 80;
        }
    }
    else
    {
        throw std::invalid_argument("Invalid CLI URL: " + url);
    }
}

// =============================================================================
// Connection Management
// =============================================================================

std::future<void> Client::start()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (state_ == ConnectionState::Connected)
                return;

            state_ = ConnectionState::Connecting;

            try
            {
                if (options_.connection &&
                    options_.connection->kind == RuntimeConnectionKind::InProcess)
                {
                    start_in_process_runtime();
                    connect_to_server();
                }
                else if (options_.connection &&
                         options_.connection->kind == RuntimeConnectionKind::ParentProcess)
                {
                    connect_to_server();
                }
                else if (parsed_host_.has_value() && parsed_port_.has_value())
                {
                    // Connect to external server
                    connect_to_server();
                }
                else
                {
                    // Spawn CLI process
                    start_cli_server();
                    connect_to_server();
                }

                // Verify protocol version
                verify_protocol_version();

                if (request_handler_bridge_)
                    rpc_->invoke("llmInference.setProvider", json::object()).get();

                if (options_.builtin_plugin_directories &&
                    !options_.builtin_plugin_directories->empty())
                {
                    rpc_->invoke(
                            "plugins.builtin.set",
                            json{{"paths", *options_.builtin_plugin_directories}}
                    ).get();
                }

                if (options_.session_fs)
                {
                    json params{
                        {"initialCwd", options_.session_fs->initial_cwd},
                        {"sessionStatePath", options_.session_fs->session_state_path},
                        {"conventions", options_.session_fs->conventions},
                    };
                    if (options_.session_fs->capabilities)
                        params["capabilities"] = *options_.session_fs->capabilities;
                    rpc_->invoke(copilot::rpc::methods::kSessionFsSetProvider, params).get();
                }

                state_ = ConnectionState::Connected;
            }
            catch (...)
            {
                state_ = ConnectionState::Error;
                throw;
            }
        }
    );
}

std::future<std::vector<StopError>> Client::stop()
{
    return std::async(
        std::launch::async,
        [this]() -> std::vector<StopError>
        {
            std::vector<StopError> errors;
            std::vector<std::shared_ptr<Session>> sessions;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [_, session] : sessions_)
                    sessions.push_back(session);
            }

            // Destroy all sessions
            for (const auto& session : sessions)
            {
                try
                {
                    session->destroy().get();
                }
                catch (const std::exception& e)
                {
                    errors.push_back(StopError{e.what()});
                }
                catch (...)
                {
                    errors.push_back(
                        StopError{"Unknown error destroying session " + session->session_id()});
                }
            }

            // Clear models cache
            {
                std::lock_guard<std::mutex> cache_lock(models_cache_mutex_);
                models_cache_.reset();
            }

            std::unique_ptr<Process> process;
            std::unique_ptr<JsonRpcClient> rpc;
            std::unique_ptr<ITransport> transport;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_.clear();
                process = std::move(process_);
                rpc = std::move(rpc_);
                transport = std::move(transport_);
                state_ = ConnectionState::Disconnected;
            }

            // Stop process FIRST - this closes the pipe ends and unblocks reads
            if (request_handler_bridge_)
            {
                request_handler_bridge_->cancel_all();
                request_handler_bridge_.reset();
            }
            if (process)
            {
                process->terminate();
                process->wait_for(std::chrono::seconds(5));
                process->close_pipes();
            }

            // Now stop RPC client - read thread will unblock since pipes are closed
            if (rpc)
                rpc->stop();

            // Close transport
            if (transport)
                transport->close();
            return errors;
        }
    );
}

void Client::force_stop()
{
    // Clear models cache
    {
        std::lock_guard<std::mutex> cache_lock(models_cache_mutex_);
        models_cache_.reset();
    }

    std::unique_ptr<Process> process;
    std::unique_ptr<JsonRpcClient> rpc;
    std::unique_ptr<ITransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.clear();
        process = std::move(process_);
        rpc = std::move(rpc_);
        transport = std::move(transport_);
        state_ = ConnectionState::Disconnected;
    }

    // Kill process FIRST - this closes the pipe ends and unblocks reads
    if (request_handler_bridge_)
    {
        request_handler_bridge_->cancel_all();
        request_handler_bridge_.reset();
    }
    if (process)
    {
        process->kill();
        process->wait_for(std::chrono::seconds(5));
        process->close_pipes();
    }

    // Now stop RPC client - read thread will unblock since pipes are closed
    if (rpc)
        rpc->stop();

    if (transport)
        transport->close();
}

ConnectionState Client::state() const
{
    return state_.load();
}

// =============================================================================
// CLI Server Management
// =============================================================================

std::pair<std::string, std::vector<std::string>>
Client::resolve_cli_command(const std::string& cli_path, const std::vector<std::string>& args)
{

    // Check if it's a Node.js script
    if (is_node_script(cli_path))
    {
        auto node_path = find_node();
        if (!node_path.has_value())
            throw std::runtime_error("Node.js not found in PATH but required for .js CLI");
        std::vector<std::string> full_args = {cli_path};
        full_args.insert(full_args.end(), args.begin(), args.end());
        return {*node_path, full_args};
    }

#ifdef _WIN32
    // On Windows, use cmd /c for PATH resolution if path is not absolute
    std::filesystem::path path(cli_path);
    if (!path.is_absolute())
    {
        std::vector<std::string> full_args = {"/c", cli_path};
        full_args.insert(full_args.end(), args.begin(), args.end());
        return {"cmd", full_args};
    }
#endif

    return {cli_path, args};
}

void Client::start_in_process_runtime()
{
    const auto& connection = *options_.connection;
    std::optional<std::string> entrypoint = connection.path;
    if (!entrypoint)
        entrypoint = options_.cli_path;
    if (!entrypoint)
    {
        if (const char* value = std::getenv("COPILOT_CLI_PATH"))
            entrypoint = value;
    }
    if (!entrypoint)
        throw std::runtime_error(
            "in-process runtime requires a CLI entrypoint path or COPILOT_CLI_PATH"
        );

    std::filesystem::path library;
    if (connection.ffi_library_path)
    {
        library = *connection.ffi_library_path;
    }
    else
    {
        std::string folder;
#if defined(_WIN32) && defined(_M_X64)
        folder = "win32-x64";
#elif defined(_WIN32) && defined(_M_ARM64)
        folder = "win32-arm64";
#elif defined(__APPLE__) && defined(__aarch64__)
        folder = "darwin-arm64";
#elif defined(__APPLE__)
        folder = "darwin-x64";
#elif defined(__aarch64__)
        folder = entrypoint->find("linuxmusl-") != std::string::npos
                     ? "linuxmusl-arm64"
                     : "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
        folder = entrypoint->find("linuxmusl-") != std::string::npos
                     ? "linuxmusl-x64"
                     : "linux-x64";
#else
        throw std::runtime_error(
            "unsupported architecture for in-process FFI runtime");
#endif
        library = std::filesystem::path(*entrypoint).parent_path() /
                  "prebuilds" / folder / "runtime.node";
    }

    std::map<std::string, std::string> environment;
    if (options_.github_token)
        environment["COPILOT_SDK_AUTH_TOKEN"] = *options_.github_token;
    if (options_.base_directory)
        environment["COPILOT_HOME"] = *options_.base_directory;
    if (options_.mode == ClientMode::Empty)
        environment["COPILOT_DISABLE_KEYTAR"] = "1";

    std::vector<std::string> args;
    if (connection.args)
        args.insert(args.end(), connection.args->begin(), connection.args->end());
    args.push_back("--log-level");
    args.push_back(json(options_.log_level).get<std::string>());
    if (options_.github_token)
    {
        args.push_back("--auth-token-env");
        args.push_back("COPILOT_SDK_AUTH_TOKEN");
    }
    if (options_.use_logged_in_user && !*options_.use_logged_in_user)
        args.push_back("--no-auto-login");
    if (options_.session_idle_timeout_seconds.value_or(0) > 0)
    {
        args.push_back("--session-idle-timeout");
        args.push_back(std::to_string(*options_.session_idle_timeout_seconds));
    }
    if (options_.remote)
        args.push_back("--remote");

    transport_ = std::make_unique<FfiTransport>(
        library.string(), *entrypoint, std::move(environment), std::move(args));
}

void Client::start_cli_server()
{
    std::string cli_path = options_.cli_path.value_or("copilot");

    // Build arguments and environment via the testable free-function helpers.
    std::vector<std::string> args = build_cli_command_args(options_);

    // Resolve command
    auto [executable, full_args] = resolve_cli_command(cli_path, args);

    // Setup process options
    ProcessOptions proc_opts;
    proc_opts.redirect_stdin = options_.use_stdio;
    proc_opts.redirect_stdout = true;
    proc_opts.redirect_stderr = true;
    proc_opts.create_no_window = true;

    if (options_.cwd)
        proc_opts.working_directory = *options_.cwd;

    if (options_.environment.has_value())
        proc_opts.inherit_environment = false;

    proc_opts.environment = build_cli_environment(options_);

    // Spawn process
    process_ = std::make_unique<Process>();
    process_->spawn(executable, full_args, proc_opts);

    // If not using stdio, wait for port announcement
    if (!options_.use_stdio)
    {
        std::regex port_regex(R"(listening on port (\d+))", std::regex::icase);
        auto start_time = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(30);

        while (true)
        {
            if (std::chrono::steady_clock::now() - start_time > timeout)
                throw std::runtime_error("Timeout waiting for CLI port announcement");

            std::string line = process_->stdout_pipe().read_line();
            if (line.empty())
            {
                if (!process_->is_running())
                    throw std::runtime_error("CLI process exited unexpectedly");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            std::smatch match;
            if (std::regex_search(line, match, port_regex))
            {
                parsed_host_ = "localhost";
                parsed_port_ = std::stoi(match[1].str());
                break;
            }
        }
    }
}

void Client::connect_to_server()
{
    if (transport_)
    {
        // Pre-created transport (in-process FFI).
    }
    else if (options_.connection &&
             options_.connection->kind == RuntimeConnectionKind::ParentProcess)
    {
#ifdef _WIN32
        transport_ = std::make_unique<StdioTransport>(
            GetStdHandle(STD_INPUT_HANDLE),
            GetStdHandle(STD_OUTPUT_HANDLE),
            false);
#else
        transport_ = std::make_unique<StdioTransport>(STDIN_FILENO, STDOUT_FILENO, false);
#endif
    }
    else if (options_.use_stdio && process_)
    {
        // Create pipe transport wrapping process pipes
        transport_ =
            std::make_unique<PipeTransport>(process_->stdin_pipe(), process_->stdout_pipe());
    }
    else if (parsed_host_.has_value() && parsed_port_.has_value())
    {
        // Create TCP transport
        auto tcp_transport = std::make_unique<TcpTransport>();
        tcp_transport->connect(*parsed_host_, *parsed_port_);
        transport_ = std::move(tcp_transport);
    }
    else
    {
        throw std::runtime_error("No transport available - check configuration");
    }

    // Create JSON-RPC client
    rpc_ = std::make_unique<JsonRpcClient>(std::move(transport_));

    // Set up handlers for server-to-client calls
    if (options_.copilot_request_handler)
    {
        request_handler_bridge_ = std::make_shared<CopilotRequestHandlerBridge>(
            options_.copilot_request_handler,
            [this](const std::string& method, const json& params)
            {
                rpc_->invoke(method, params).get();
            });
    }

    rpc_->set_notification_handler(
        [this](const std::string& method, const json& params)
        {
            if (method == "session.event")
                handle_session_event(method, params);
            else if (method == "session.lifecycle")
            {
                try
                {
                    auto event = params.get<SessionLifecycleEvent>();
                    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                    for (const auto& handler : lifecycle_handlers_)
                        handler(event);
                }
                catch (...)
                {
                }
            }
            else if (method == "gitHubTelemetry.event" && options_.on_github_telemetry)
            {
                (*options_.on_github_telemetry)(params);
            }
        }
    );

    rpc_->set_request_handler(
        [this](const std::string& method, const json& params) -> json
        {
            if (method == "tool.call")
                return handle_tool_call(params);
            else if (method == "permission.request")
                return handle_permission_request(params);
            else if (method == "userInput.request")
                return handle_user_input_request(params);
            else if (method == "elicitation.request")
                return handle_elicitation_request(params);
            else if (method == "exitPlanMode.request")
                return handle_exit_plan_mode_request(params);
            else if (method == "autoModeSwitch.request")
                return handle_auto_mode_switch_request(params);
            else if (method == "hooks.invoke")
                return handle_hooks_invoke(params);
            else if (method == copilot::rpc::methods::kGitHubTokenGetToken)
                return handle_github_token_request(params);
            else if (
                method == "canvas.open" || method == "canvas.close" ||
                method == "canvas.action.invoke")
            {
                const auto session = get_session(params.value("sessionId", ""));
                if (!session)
                    throw JsonRpcError(
                        JsonRpcErrorCode::InvalidParams, "Unknown canvas session");
                return session->handle_canvas_request(method, params);
            }
            else if (method == "factory.execute" || method == "factory.abort")
            {
                const auto session = get_session(params.value("sessionId", ""));
                if (!session)
                    throw JsonRpcError(
                        JsonRpcErrorCode::InvalidParams, "Unknown factory session");
                return session->handle_factory_request(method, params);
            }
            else if (
                request_handler_bridge_ &&
                (method == "llmInference.httpRequestStart" ||
                 method == "llmInference.httpRequestChunk"))
            {
                return request_handler_bridge_->handle(method, params);
            }
            else if (options_.session_fs && method.rfind("sessionFs.", 0) == 0)
            {
                const auto it = options_.session_fs->handlers.find(method);
                if (it != options_.session_fs->handlers.end())
                    return it->second(params);
            }
            else if (options_.request_handler)
                return (*options_.request_handler)(method, params);
            throw JsonRpcError(JsonRpcErrorCode::MethodNotFound, "Unknown method: " + method);
        }
    );

    rpc_->start();
}

void Client::verify_protocol_version()
{
    json response;
    try
    {
        json params = json::object();
        if (options_.tcp_connection_token)
            params["token"] = *options_.tcp_connection_token;
        if (options_.on_github_telemetry)
            params["enableGitHubTelemetryForwarding"] = true;
        if (options_.client_info)
        {
            // Omitted entirely when every field is empty, so the runtime keeps its
            // own attribution rather than seeing a blank identity.
            if (auto wire = options_.client_info->to_wire_json())
                params["clientInfo"] = std::move(*wire);
        }
        response = rpc_->invoke(copilot::rpc::methods::kConnect, params).get();
    }
    catch (const JsonRpcError& error)
    {
        if (error.code() != JsonRpcErrorCode::MethodNotFound &&
            std::string(error.what()) != "Unhandled method connect")
            throw;
        response =
            rpc_->invoke(copilot::rpc::methods::kPing, json{{"message", nullptr}}).get();
    }

    if (!response.contains("protocolVersion") || response["protocolVersion"].is_null())
        response =
            rpc_->invoke(copilot::rpc::methods::kPing, json{{"message", nullptr}}).get();

    if (!response.contains("protocolVersion") || response["protocolVersion"].is_null())
    {
        throw std::runtime_error(
            "SDK protocol version mismatch: SDK expects version " +
            std::to_string(kSdkProtocolVersion) + ", but server does not report a protocol version."
        );
    }

    int server_version = response["protocolVersion"].get<int>();
    if (server_version < kMinProtocolVersion || server_version > kSdkProtocolVersion)
    {
        throw std::runtime_error(
            "SDK protocol version mismatch: SDK supports versions [" +
            std::to_string(kMinProtocolVersion) + ".." + std::to_string(kSdkProtocolVersion) +
            "], but server reports version " + std::to_string(server_version)
        );
    }

    {
        std::lock_guard<std::mutex> lock(protocol_version_mutex_);
        negotiated_protocol_version_ = server_version;
        if (response.contains("version") && response["version"].is_string())
            server_version_ = response["version"].get<std::string>();
    }
}

std::optional<int> Client::negotiated_protocol_version() const
{
    std::lock_guard<std::mutex> lock(protocol_version_mutex_);
    return negotiated_protocol_version_;
}

// =============================================================================
// Session Management
// =============================================================================

std::future<std::shared_ptr<Session>> Client::create_session(SessionConfig config)
{
    return std::async(
        std::launch::async,
        [this, config = std::move(config)]() mutable
        {
            // Ensure connected
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            if (!config.session_id)
                config.session_id = generate_uuid_v4();
            std::string session_id = *config.session_id;
            auto session = std::make_shared<Session>(
                session_id,
                this,
                std::nullopt,
                SessionCapabilities{},
                config.enable_managed_settings.value_or(false) ||
                    config.managed_settings.has_value()
            );

            // Register tools locally for handling callbacks from the server
            for (const auto& tool : config.tools)
                session->register_tool(tool);
            session->register_canvases(config.canvas_objects);

            // Register permission handler locally (server will call permission.request)
            if (config.on_permission_request.has_value())
                session->register_permission_handler(*config.on_permission_request);
            if (config.on_permission_request_with_context.has_value())
                session->register_permission_handler(
                    *config.on_permission_request_with_context
                );
            if (config.on_mcp_auth_request)
                session->register_mcp_auth_handler(*config.on_mcp_auth_request);

            // Register user input handler locally (server will call userInput.request)
            if (config.on_user_input_request.has_value())
                session->register_user_input_handler(*config.on_user_input_request);

            if (config.on_elicitation_request.has_value())
                session->register_elicitation_handler(*config.on_elicitation_request);
            if (config.on_exit_plan_mode.has_value())
                session->register_exit_plan_mode_handler(*config.on_exit_plan_mode);
            if (config.on_auto_mode_switch.has_value())
                session->register_auto_mode_switch_handler(*config.on_auto_mode_switch);
            if (config.on_event.has_value())
                session->register_persistent_event_handler(*config.on_event);

            // Register hooks locally (server will call hooks.invoke)
            if (config.hooks.has_value())
                session->register_hooks(*config.hooks);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_[session_id] = session;
            }

            json response;
            try
            {
                json request = build_session_create_request(config, options_.mode);
                request.update(trace_context());
                if (options_.on_github_telemetry)
                    request["enableGitHubTelemetryForwarding"] = true;
                response =
                    rpc_->invoke(copilot::rpc::methods::kSessionCreate, request).get();
                const auto returned_id = response.at("sessionId").get<std::string>();
                if (returned_id != session_id)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    sessions_.erase(session_id);
                    session_id = returned_id;
                    session->set_session_id(returned_id);
                    sessions_[session_id] = session;
                }
            }
            catch (...)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_.erase(session_id);
                throw;
            }

            std::optional<std::string> workspace_path;
            if (response.contains("workspacePath") && response["workspacePath"].is_string())
                workspace_path = response["workspacePath"].get<std::string>();
            SessionCapabilities capabilities;
            if (response.contains("capabilities") && !response["capabilities"].is_null())
                capabilities = response["capabilities"].get<SessionCapabilities>();
            session->set_initial_state(std::move(workspace_path), std::move(capabilities));

            if (config.on_mcp_auth_request)
            {
                rpc_->invoke(
                        "session.eventLog.registerInterest",
                        json{{"sessionId", session_id}, {"eventType", "mcp.oauth_required"}}
                ).get();
            }

            return session;
        }
    );
}

std::future<std::shared_ptr<Session>>
Client::resume_session(const std::string& session_id, ResumeSessionConfig config)
{

    return std::async(
        std::launch::async,
        [this, session_id, config = std::move(config)]()
        {
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            // Build and send request
            json request = build_session_resume_request(session_id, config, options_.mode);
            request.update(trace_context());
            if (options_.on_github_telemetry)
                request["enableGitHubTelemetryForwarding"] = true;
            auto response = rpc_->invoke(copilot::rpc::methods::kSessionResume, request).get();
            std::string returned_session_id = response["sessionId"].get<std::string>();

            if (config.requested_environment_variables &&
                response.contains("grantedEnvironmentVariables") &&
                response["grantedEnvironmentVariables"].is_object())
            {
                const auto& granted = response["grantedEnvironmentVariables"];
                for (const auto& name : *config.requested_environment_variables)
                {
                    if (!granted.contains(name) || !granted[name].is_string())
                        continue;
                    const auto value = granted[name].get<std::string>();
#ifdef _WIN32
                    _putenv_s(name.c_str(), value.c_str());
#else
                    setenv(name.c_str(), value.c_str(), 1);
#endif
                }
            }

            // Capture workspace_path if present (for infinite sessions)
            std::optional<std::string> workspace_path;
            if (response.contains("workspacePath") && response["workspacePath"].is_string())
                workspace_path = response["workspacePath"].get<std::string>();

            SessionCapabilities capabilities;
            if (response.contains("capabilities") && !response["capabilities"].is_null())
                capabilities = response["capabilities"].get<SessionCapabilities>();

            auto session = std::make_shared<Session>(
                returned_session_id,
                this,
                workspace_path,
                std::move(capabilities),
                config.enable_managed_settings.value_or(false) ||
                    config.managed_settings.has_value()
            );

            // Register tools locally for handling callbacks from the server
            for (const auto& tool : config.tools)
                session->register_tool(tool);
            session->register_canvases(config.canvas_objects);
            session->register_factories(config.factory_objects);

            // Register permission handler locally (server will call permission.request)
            if (config.on_permission_request.has_value())
                session->register_permission_handler(*config.on_permission_request);
            if (config.on_permission_request_with_context.has_value())
                session->register_permission_handler(
                    *config.on_permission_request_with_context
                );
            if (config.on_mcp_auth_request)
                session->register_mcp_auth_handler(*config.on_mcp_auth_request);

            // Register user input handler locally (server will call userInput.request)
            if (config.on_user_input_request.has_value())
                session->register_user_input_handler(*config.on_user_input_request);

            if (config.on_elicitation_request.has_value())
                session->register_elicitation_handler(*config.on_elicitation_request);
            if (config.on_exit_plan_mode.has_value())
                session->register_exit_plan_mode_handler(*config.on_exit_plan_mode);
            if (config.on_auto_mode_switch.has_value())
                session->register_auto_mode_switch_handler(*config.on_auto_mode_switch);
            if (config.on_event.has_value())
                session->register_persistent_event_handler(*config.on_event);

            // Register hooks locally (server will call hooks.invoke)
            if (config.hooks.has_value())
                session->register_hooks(*config.hooks);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_[returned_session_id] = session;
            }
            if (config.on_mcp_auth_request)
            {
                rpc_->invoke(
                        "session.eventLog.registerInterest",
                        json{
                            {"sessionId", returned_session_id},
                            {"eventType", "mcp.oauth_required"},
                        }
                ).get();
            }

            return session;
        }
    );
}

std::future<std::vector<SessionMetadata>> Client::list_sessions()
{
    return list_sessions(SessionListFilter{});
}

std::future<std::vector<SessionMetadata>> Client::list_sessions(SessionListFilter filter)
{
    return std::async(
        std::launch::async,
        [this, filter = std::move(filter)]()
        {
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            // session.list takes an optional 'filter' param (matches nodejs SDK shape).
            json params = json::object();
            json filter_json = filter; // uses to_json(SessionListFilter)
            if (!filter_json.empty())
                params["filter"] = std::move(filter_json);

            auto response = rpc_->invoke(copilot::rpc::methods::kSessionList, params).get();
            std::vector<SessionMetadata> sessions;

            if (response.contains("sessions") && response["sessions"].is_array())
            {
                for (const auto& item : response["sessions"])
                {
                    sessions.push_back(item.get<SessionMetadata>());
                }
            }

            return sessions;
        }
    );
}

std::future<std::optional<SessionMetadata>>
Client::get_session_metadata(const std::string& session_id)
{
    return std::async(
        std::launch::async,
        [this, session_id]() -> std::optional<SessionMetadata>
        {
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            auto response =
                rpc_->invoke(copilot::rpc::methods::kSessionGetMetadata, json{{"sessionId", session_id}}).get();

            if (!response.contains("session") || response["session"].is_null())
                return std::nullopt;

            return response["session"].get<SessionMetadata>();
        }
    );
}

std::future<void> Client::delete_session(const std::string& session_id)
{
    return std::async(
        std::launch::async,
        [this, session_id]()
        {
            if (state_ != ConnectionState::Connected)
                throw std::runtime_error("Client not connected");

            auto response =
                rpc_->invoke(copilot::rpc::methods::kSessionDelete, json{{"sessionId", session_id}}).get();

            if (response.contains("success") && !response["success"].get<bool>())
            {
                std::string error = response.contains("error")
                                        ? response["error"].get<std::string>()
                                        : "Unknown error";
                throw std::runtime_error("Failed to delete session: " + error);
            }

            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.erase(session_id);
        }
    );
}

std::future<void> Client::clear_managed_settings_cache()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            if (state_ != ConnectionState::Connected)
                throw std::runtime_error("Client not connected");

            rpc_->invoke(copilot::rpc::methods::kManagedSettingsClearCache, json::object()).get();
        }
    );
}

std::future<std::optional<std::string>> Client::get_last_session_id()
{
    return std::async(
        std::launch::async,
        [this]() -> std::optional<std::string>
        {
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            auto response = rpc_->invoke(copilot::rpc::methods::kSessionGetLastId, json::object()).get();

            if (response.contains("sessionId") && !response["sessionId"].is_null())
                return response["sessionId"].get<std::string>();
            return std::nullopt;
        }
    );
}

std::future<PingResponse> Client::ping(std::optional<std::string> message)
{
    return std::async(
        std::launch::async,
        [this, message]()
        {
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            json params;
            if (message.has_value())
                params["message"] = *message;
            else
                params["message"] = nullptr;

            auto response = rpc_->invoke(copilot::rpc::methods::kPing, params).get();

            PingResponse result;
            if (response.contains("message") && !response["message"].is_null())
                result.message = response["message"].get<std::string>();
            if (response.contains("timestamp") && !response["timestamp"].is_null())
            {
                if (response["timestamp"].is_number_integer())
                    result.timestamp = response["timestamp"].get<int64_t>();
                else if (response["timestamp"].is_string())
                    result.timestamp_iso = response["timestamp"].get<std::string>();
            }
            if (response.contains("protocolVersion") && !response["protocolVersion"].is_null())
                result.protocol_version = response["protocolVersion"].get<int>();
            return result;
        }
    );
}

std::future<GetStatusResponse> Client::get_status()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            GetStatusResponse result;
            {
                std::lock_guard<std::mutex> lock(protocol_version_mutex_);
                result.version = server_version_.value_or("");
                result.protocol_version =
                    negotiated_protocol_version_.value_or(kSdkProtocolVersion);
            }
            return result;
        }
    );
}

std::future<GetAuthStatusResponse> Client::get_auth_status()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            auto response =
                rpc_->invoke("account.getCurrentAuth", json::object()).get();
            GetAuthStatusResponse result;
            if (response.contains("authInfo") && response["authInfo"].is_object())
            {
                const auto& auth = response["authInfo"];
                result.is_authenticated = true;
                if (auth.contains("type") && auth["type"].is_string())
                    result.auth_type = auth["type"].get<std::string>();
                if (auth.contains("host") && auth["host"].is_string())
                    result.host = auth["host"].get<std::string>();
                if (auth.contains("login") && auth["login"].is_string())
                    result.login = auth["login"].get<std::string>();
            }
            return result;
        }
    );
}

std::future<std::vector<ModelInfo>> Client::list_models()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            // Check cache first (applies to both BYOK and RPC paths).
            {
                std::lock_guard<std::mutex> lock(models_cache_mutex_);
                if (models_cache_.has_value())
                    return std::vector<ModelInfo>(*models_cache_);
            }

            // BYOK: if a custom handler is registered, use it instead of the CLI RPC.
            ListModelsHandler handler_copy;
            {
                std::lock_guard<std::mutex> lock(on_list_models_mutex_);
                handler_copy = on_list_models_;
            }
            if (handler_copy)
            {
                auto models = handler_copy();
                std::lock_guard<std::mutex> lock(models_cache_mutex_);
                models_cache_ = models;
                return models;
            }

            // Default path: query the CLI server (requires a live connection).
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            auto response = rpc_->invoke(copilot::rpc::methods::kModelsList, json::object()).get();
            auto models_response = response.get<GetModelsResponse>();

            // Store in cache
            {
                std::lock_guard<std::mutex> lock(models_cache_mutex_);
                models_cache_ = models_response.models;
            }

            return models_response.models;
        }
    );
}

std::future<json> Client::invoke(const std::string& method, json params)
{
    return std::async(
        std::launch::async,
        [this, method, params = std::move(params)]() mutable
        {
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }
            return rpc_->invoke(method, params).get();
        }
    );
}

json Client::trace_context() const
{
    json result = json::object();
    if (!options_.on_get_trace_context)
        return result;
    for (const auto& [key, value] : (*options_.on_get_trace_context)())
        if (key == "traceparent" || key == "tracestate")
            result[key] = value;
    return result;
}

void Client::set_on_list_models(ListModelsHandler handler)
{
    std::lock_guard<std::mutex> lock(on_list_models_mutex_);
    on_list_models_ = std::move(handler);
    // Invalidate the cache so the next list_models() call observes the new source.
    std::lock_guard<std::mutex> cache_lock(models_cache_mutex_);
    models_cache_.reset();
}

std::shared_ptr<Session> Client::get_session(const std::string& session_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

// =============================================================================
// RPC Handlers
// =============================================================================

void Client::handle_session_event(const std::string& method, const json& params)
{
    if (!params.contains("sessionId") || !params.contains("event"))
        return;

    std::string session_id = params["sessionId"].get<std::string>();
    auto session = get_session(session_id);
    if (!session)
        return;

    // Parse and dispatch the event
    auto event = parse_session_event(params["event"]);
    session->dispatch_event(event);
}

json Client::handle_tool_call(const json& params)
{
    std::string session_id = params["sessionId"].get<std::string>();
    std::string tool_call_id = params["toolCallId"].get<std::string>();
    std::string tool_name = params["toolName"].get<std::string>();
    json arguments = params.value("arguments", json::object());

    auto session = get_session(session_id);
    if (!session)
    {
        return json{
            {"result",
             {{"textResultForLlm", "Session not found"},
              {"resultType", "failure"},
              {"error", "Unknown session " + session_id}}}
        };
    }

    const Tool* tool = session->get_tool(tool_name);
    if (!tool || !tool->handler)
    {
        return json{
            {"result",
             {{"textResultForLlm", "Tool '" + tool_name + "' is not supported."},
              {"resultType", "failure"},
              {"error", "tool '" + tool_name + "' not supported"}}}
        };
    }

    try
    {
        ToolInvocation invocation;
        invocation.session_id = session_id;
        invocation.tool_call_id = tool_call_id;
        invocation.tool_name = tool_name;
        invocation.arguments = arguments;
        if (params.contains("availableTools") && params["availableTools"].is_array())
            invocation.available_tools = params["availableTools"].get<std::vector<json>>();
        if (params.contains("traceparent") && params["traceparent"].is_string())
            invocation.traceparent = params["traceparent"].get<std::string>();
        if (params.contains("tracestate") && params["tracestate"].is_string())
            invocation.tracestate = params["tracestate"].get<std::string>();

        json result = tool->handler(invocation);

        // Wrap result in response format
        return json{{"result", result}};
    }
    catch (const std::exception& e)
    {
        // Redact exception details from textResultForLlm to avoid leaking sensitive info
        return json{
            {"result",
             {{"textResultForLlm", "Tool execution failed"},
              {"resultType", "failure"},
              {"error", e.what()}}}
        };
    }
}

json Client::handle_permission_request(const json& params)
{
    std::string session_id = params["sessionId"].get<std::string>();

    // The permission request data is nested in "permissionRequest" field
    const auto& perm_data =
        params.contains("permissionRequest") ? params["permissionRequest"] : params;

    auto session = get_session(session_id);
    if (!session)
    {
        return json{{"result", {{"kind", "no-result"}}}};
    }

    try
    {
        PermissionRequest request;
        request.kind = perm_data["kind"].get<std::string>();
        if (perm_data.contains("toolCallId") && !perm_data["toolCallId"].is_null())
            request.tool_call_id = perm_data["toolCallId"].get<std::string>();
        if (perm_data.contains("managedApprovalRequired") &&
            !perm_data["managedApprovalRequired"].is_null())
        {
            request.managed_approval_required =
                perm_data["managedApprovalRequired"].get<bool>();
        }
        // Collect all other fields as extension data
        for (auto& [key, value] : perm_data.items())
            if (key != "kind" && key != "toolCallId" &&
                key != "managedApprovalRequired")
                request.extension_data[key] = value;

        auto result = session->handle_permission_request(request);

        json response;
        json result_json = result;
        response["result"] = std::move(result_json);
        if (result.decision_context)
            response["decisionContext"] = *result.decision_context;
        if (params.contains("requestId"))
            response["requestId"] = params["requestId"];
        return response;
    }
    catch (const std::exception&)
    {
        return json{{"result", {{"kind", "no-result"}}}};
    }
}

const std::string& Client::github_token_registration_id() const
{
    return github_token_registration_id_;
}

json Client::handle_github_token_request(const json& params)
{
    if (!options_.github_token_provider)
    {
        throw JsonRpcError(
            JsonRpcErrorCode::MethodNotFound,
            "No GitHub token provider registered (set ClientOptions::github_token_provider)"
        );
    }

    GitHubTokenRequest request;
    request.registration_id = params.value("registrationId", "");
    request.host = params.value("host", "");
    if (params.contains("sessionId") && !params["sessionId"].is_null())
        request.session_id = params["sessionId"].get<std::string>();
    request.reason = params.value("reason", "") == "refresh"
                         ? GitHubTokenAcquireReason::Refresh
                         : GitHubTokenAcquireReason::Initial;

    // A declining or throwing provider must not surface as an RPC error: the official result
    // union models refusal as its own variant.
    try
    {
        if (auto token = (*options_.github_token_provider)(request))
        {
            // The schema requires expiresIn >= 3601 so the credential outlives the runtime's
            // one-hour preflight refresh. Emitting a smaller value would be out of contract,
            // so decline instead.
            if (token->expires_in < kMinGitHubTokenExpiresIn)
                return json{{"kind", "cancelled"}};

            json result{
                {"kind", "token"},
                {"accessToken", token->access_token},
                {"expiresIn", token->expires_in},
            };
            if (token->token_type)
                result["tokenType"] = *token->token_type;
            return result;
        }
    }
    catch (...)
    {
    }

    return json{{"kind", "cancelled"}};
}

json Client::handle_user_input_request(const json& params)
{
    std::string session_id = params["sessionId"].get<std::string>();
    std::string question = params["question"].get<std::string>();

    auto session = get_session(session_id);
    if (!session)
        throw JsonRpcError(JsonRpcErrorCode::InvalidParams, "Unknown session " + session_id);

    try
    {
        UserInputRequest request;
        request.question = question;
        if (params.contains("choices") && !params["choices"].is_null())
            request.choices = params["choices"].get<std::vector<std::string>>();
        if (params.contains("allowFreeform") && !params["allowFreeform"].is_null())
            request.allow_freeform = params["allowFreeform"].get<bool>();

        auto result = session->handle_user_input_request(request);

        json response;
        response["answer"] = result.answer;
        response["wasFreeform"] = result.was_freeform;
        return response;
    }
    catch (const std::exception& e)
    {
        throw JsonRpcError(JsonRpcErrorCode::InternalError, e.what());
    }
}

json Client::handle_elicitation_request(const json& params)
{
    std::string session_id = params["sessionId"].get<std::string>();

    auto session = get_session(session_id);
    if (!session)
        throw JsonRpcError(JsonRpcErrorCode::InvalidParams, "Unknown session " + session_id);

    try
    {
        auto context = params.get<ElicitationContext>();
        return json(session->handle_elicitation_request(context));
    }
    catch (const std::exception& e)
    {
        throw JsonRpcError(JsonRpcErrorCode::InternalError, e.what());
    }
}

json Client::handle_exit_plan_mode_request(const json& params)
{
    std::string session_id = params["sessionId"].get<std::string>();

    auto session = get_session(session_id);
    if (!session)
        throw JsonRpcError(JsonRpcErrorCode::InvalidParams, "Unknown session " + session_id);

    try
    {
        auto request = params.get<ExitPlanModeRequest>();
        return json(session->handle_exit_plan_mode_request(request));
    }
    catch (const std::exception& e)
    {
        throw JsonRpcError(JsonRpcErrorCode::InternalError, e.what());
    }
}

json Client::handle_auto_mode_switch_request(const json& params)
{
    std::string session_id = params["sessionId"].get<std::string>();

    auto session = get_session(session_id);
    if (!session)
        throw JsonRpcError(JsonRpcErrorCode::InvalidParams, "Unknown session " + session_id);

    try
    {
        auto request = params.get<AutoModeSwitchRequest>();
        return json(session->handle_auto_mode_switch_request(request));
    }
    catch (const std::exception& e)
    {
        throw JsonRpcError(JsonRpcErrorCode::InternalError, e.what());
    }
}

json Client::handle_hooks_invoke(const json& params)
{
    std::string session_id = params["sessionId"].get<std::string>();
    std::string hook_type = params["hookType"].get<std::string>();
    json input = params.value("input", json::object());

    auto session = get_session(session_id);
    if (!session)
        throw JsonRpcError(JsonRpcErrorCode::InvalidParams, "Unknown session " + session_id);

    try
    {
        auto output = session->handle_hooks_invoke(hook_type, input);
        if (output.is_null())
            return json::object();
        return json{{"output", output}};
    }
    catch (const std::exception& e)
    {
        throw JsonRpcError(JsonRpcErrorCode::InternalError, e.what());
    }
}

// =============================================================================
// Lifecycle Events
// =============================================================================

Subscription Client::on_lifecycle(LifecycleHandler handler)
{
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    lifecycle_handlers_.push_back(std::move(handler));
    auto* ptr = &lifecycle_handlers_.back();
    return Subscription([this, ptr]() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        lifecycle_handlers_.erase(
            std::remove_if(lifecycle_handlers_.begin(), lifecycle_handlers_.end(),
                           [ptr](const LifecycleHandler& h) { return &h == ptr; }),
            lifecycle_handlers_.end());
    });
}

// =============================================================================
// Foreground Session
// =============================================================================

std::future<std::optional<std::string>> Client::get_foreground_session_id()
{
    return std::async(
        std::launch::async,
        [this]() -> std::optional<std::string>
        {
            auto response = rpc_client()->invoke(copilot::rpc::methods::kSessionGetForeground, json::object()).get();
            auto parsed = response.get<GetForegroundSessionResponse>();
            return parsed.session_id;
        }
    );
}

std::future<void> Client::set_foreground_session_id(const std::string& session_id)
{
    return std::async(
        std::launch::async,
        [this, session_id]()
        {
            rpc_client()->invoke(copilot::rpc::methods::kSessionSetForeground, json{{"sessionId", session_id}}).get();
        }
    );
}

} // namespace copilot
