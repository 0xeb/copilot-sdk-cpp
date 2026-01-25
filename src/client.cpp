// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <chrono>
#include <copilot/client.hpp>
#include <copilot/session.hpp>
#include <regex>
#include <thread>

namespace copilot
{

// =============================================================================
// Request Builder Helpers (exposed for unit testing)
// =============================================================================

json build_session_create_request(const SessionConfig& config)
{
    json request;

    // Model: explicit > env (if auto_byok_from_env) > none
    if (config.model.has_value())
    {
        request["model"] = *config.model;
    }
    else if (config.auto_byok_from_env)
    {
        if (auto env_model = ProviderConfig::model_from_env())
            request["model"] = *env_model;
    }

    if (config.session_id.has_value())
        request["sessionId"] = *config.session_id;
    if (config.on_permission_request.has_value())
        request["requestPermission"] = true;
    if (config.system_message.has_value())
    {
        json sys_msg;
        if (config.system_message->content.has_value())
            sys_msg["content"] = *config.system_message->content;
        if (config.system_message->mode.has_value())
        {
            sys_msg["mode"] =
                (*config.system_message->mode == SystemMessageMode::Replace) ? "replace" : "append";
        }
        request["systemMessage"] = sys_msg;
    }
    // Add custom tool definitions to the request
    if (!config.tools.empty())
    {
        json tool_defs = json::array();
        for (const auto& tool : config.tools)
        {
            json def;
            def["name"] = tool.name;
            def["description"] = tool.description;
            if (!tool.parameters_schema.is_null())
                def["parameters"] = tool.parameters_schema;
            tool_defs.push_back(def);
        }
        request["tools"] = tool_defs;
    }
    if (config.available_tools.has_value())
        request["availableTools"] = *config.available_tools;
    if (config.excluded_tools.has_value())
        request["excludedTools"] = *config.excluded_tools;
    if (config.streaming)
        request["streaming"] = config.streaming;

    // Provider: explicit > env (if auto_byok_from_env) > none
    if (config.provider.has_value())
    {
        request["provider"] = *config.provider;
    }
    else if (config.auto_byok_from_env)
    {
        if (auto env_provider = ProviderConfig::from_env())
            request["provider"] = *env_provider;
    }

    if (config.mcp_servers.has_value())
        request["mcpServers"] = *config.mcp_servers;
    if (config.custom_agents.has_value())
    {
        json agents = json::array();
        for (const auto& agent : *config.custom_agents)
            agents.push_back(agent);
        request["customAgents"] = agents;
    }
    if (config.skill_directories.has_value())
        request["skillDirectories"] = *config.skill_directories;
    if (config.disabled_skills.has_value())
        request["disabledSkills"] = *config.disabled_skills;
    if (config.infinite_sessions.has_value())
        request["infiniteSessions"] = *config.infinite_sessions;
    if (config.config_dir.has_value())
        request["configDir"] = *config.config_dir;

    return request;
}

json build_session_resume_request(const std::string& session_id, const ResumeSessionConfig& config)
{
    json request;
    request["sessionId"] = session_id;
    if (config.on_permission_request.has_value())
        request["requestPermission"] = true;

    // Add custom tool definitions to the request
    if (!config.tools.empty())
    {
        json tool_defs = json::array();
        for (const auto& tool : config.tools)
        {
            json def;
            def["name"] = tool.name;
            def["description"] = tool.description;
            if (!tool.parameters_schema.is_null())
                def["parameters"] = tool.parameters_schema;
            tool_defs.push_back(def);
        }
        request["tools"] = tool_defs;
    }
    if (config.streaming)
        request["streaming"] = config.streaming;

    // Provider: explicit > env (if auto_byok_from_env) > none
    if (config.provider.has_value())
    {
        request["provider"] = *config.provider;
    }
    else if (config.auto_byok_from_env)
    {
        if (auto env_provider = ProviderConfig::from_env())
            request["provider"] = *env_provider;
    }

    if (config.mcp_servers.has_value())
        request["mcpServers"] = *config.mcp_servers;
    if (config.custom_agents.has_value())
    {
        json agents = json::array();
        for (const auto& agent : *config.custom_agents)
            agents.push_back(agent);
        request["customAgents"] = agents;
    }
    if (config.skill_directories.has_value())
        request["skillDirectories"] = *config.skill_directories;
    if (config.disabled_skills.has_value())
        request["disabledSkills"] = *config.disabled_skills;
    if (config.config_dir.has_value())
        request["configDir"] = *config.config_dir;

    return request;
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

Client::Client(ClientOptions options) : options_(std::move(options))
{
    // Validate mutually exclusive options
    if (options_.cli_url.has_value() && (options_.use_stdio || options_.cli_path.has_value()))
        throw std::invalid_argument("cli_url is mutually exclusive with use_stdio and cli_path");

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
    // If it's just a port number
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

    // Check for scheme
    std::string url_to_parse = url;
    if (url.find("://") == std::string::npos)
        url_to_parse = "https://" + url;

    // Parse host:port
    std::regex url_regex(R"((?:https?://)?([^:/]+)(?::(\d+))?)");
    std::smatch match;
    if (std::regex_match(url_to_parse, match, url_regex))
    {
        parsed_host_ = match[1].str();
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
                if (parsed_host_.has_value() && parsed_port_.has_value())
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

std::future<void> Client::stop()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // Destroy all sessions
            for (auto& [id, session] : sessions_)
            {
                try
                {
                    session->destroy().get();
                }
                catch (...)
                {
                    // Ignore errors during cleanup
                }
            }
            sessions_.clear();

            // Stop process FIRST - this closes the pipe ends and unblocks reads
            if (process_)
            {
                process_->terminate();
                process_->wait();
                process_.reset();
            }

            // Now stop RPC client - read thread will unblock since pipes are closed
            if (rpc_)
            {
                rpc_->stop();
                rpc_.reset();
            }

            // Close transport
            if (transport_)
            {
                transport_->close();
                transport_.reset();
            }

            state_ = ConnectionState::Disconnected;
        }
    );
}

void Client::force_stop()
{
    std::lock_guard<std::mutex> lock(mutex_);

    sessions_.clear();

    // Kill process FIRST - this closes the pipe ends and unblocks reads
    if (process_)
    {
        process_->kill();
        process_->wait();
        process_.reset();
    }

    // Now stop RPC client - read thread will unblock since pipes are closed
    if (rpc_)
    {
        rpc_->stop();
        rpc_.reset();
    }

    if (transport_)
    {
        transport_->close();
        transport_.reset();
    }

    state_ = ConnectionState::Disconnected;
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

void Client::start_cli_server()
{
    std::string cli_path = options_.cli_path.value_or("copilot");

    // Build arguments
    std::vector<std::string> args;
    if (options_.cli_args.has_value())
        args.insert(args.end(), options_.cli_args->begin(), options_.cli_args->end());
    args.push_back("--server");
    args.push_back("--log-level");
    args.push_back(options_.log_level);

    if (options_.use_stdio)
    {
        args.push_back("--stdio");
    }
    else if (options_.port > 0)
    {
        args.push_back("--port");
        args.push_back(std::to_string(options_.port));
    }

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
    {
        proc_opts.inherit_environment = false;
        proc_opts.environment = *options_.environment;
    }

    // Remove NODE_DEBUG to avoid debug output interfering with JSON-RPC
    proc_opts.environment.erase("NODE_DEBUG");

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
    if (options_.use_stdio && process_)
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
    rpc_->set_notification_handler(
        [this](const std::string& method, const json& params)
        {
            if (method == "session.event")
                handle_session_event(method, params);
        }
    );

    rpc_->set_request_handler(
        [this](const std::string& method, const json& params) -> json
        {
            if (method == "tool.call")
                return handle_tool_call(params);
            else if (method == "permission.request")
                return handle_permission_request(params);
            throw JsonRpcError(JsonRpcErrorCode::MethodNotFound, "Unknown method: " + method);
        }
    );

    rpc_->start();
}

void Client::verify_protocol_version()
{
    auto response = rpc_->invoke("ping", json{{"message", nullptr}}).get();

    if (!response.contains("protocolVersion") || response["protocolVersion"].is_null())
    {
        throw std::runtime_error(
            "SDK protocol version mismatch: SDK expects version " +
            std::to_string(kSdkProtocolVersion) + ", but server does not report a protocol version."
        );
    }

    int server_version = response["protocolVersion"].get<int>();
    if (server_version != kSdkProtocolVersion)
    {
        throw std::runtime_error(
            "SDK protocol version mismatch: SDK expects version " +
            std::to_string(kSdkProtocolVersion) + ", but server reports version " +
            std::to_string(server_version)
        );
    }
}

// =============================================================================
// Session Management
// =============================================================================

std::future<std::shared_ptr<Session>> Client::create_session(SessionConfig config)
{
    return std::async(
        std::launch::async,
        [this, config = std::move(config)]()
        {
            // Ensure connected
            if (state_ != ConnectionState::Connected)
            {
                if (options_.auto_start)
                    start().get();
                else
                    throw std::runtime_error("Client not connected. Call start() first.");
            }

            // Build and send request
            json request = build_session_create_request(config);
            auto response = rpc_->invoke("session.create", request).get();
            std::string session_id = response["sessionId"].get<std::string>();

            // Capture workspace path for infinite sessions
            std::optional<std::string> workspace_path;
            if (response.contains("workspacePath") && response["workspacePath"].is_string())
                workspace_path = response["workspacePath"].get<std::string>();

            auto session = std::make_shared<Session>(session_id, this, workspace_path);

            // Register tools locally for handling callbacks from the server
            for (const auto& tool : config.tools)
                session->register_tool(tool);

            // Register permission handler locally (server will call permission.request)
            if (config.on_permission_request.has_value())
                session->register_permission_handler(*config.on_permission_request);

            std::lock_guard<std::mutex> lock(mutex_);
            sessions_[session_id] = session;

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
            json request = build_session_resume_request(session_id, config);
            auto response = rpc_->invoke("session.resume", request).get();
            std::string returned_session_id = response["sessionId"].get<std::string>();

            // Capture workspace_path if present (for infinite sessions)
            std::optional<std::string> workspace_path;
            if (response.contains("workspacePath") && response["workspacePath"].is_string())
                workspace_path = response["workspacePath"].get<std::string>();

            auto session = std::make_shared<Session>(returned_session_id, this, workspace_path);

            // Register tools locally for handling callbacks from the server
            for (const auto& tool : config.tools)
                session->register_tool(tool);

            // Register permission handler locally (server will call permission.request)
            if (config.on_permission_request.has_value())
                session->register_permission_handler(*config.on_permission_request);

            std::lock_guard<std::mutex> lock(mutex_);
            sessions_[returned_session_id] = session;

            return session;
        }
    );
}

std::future<std::vector<SessionMetadata>> Client::list_sessions()
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

            auto response = rpc_->invoke("session.list", json::object()).get();
            std::vector<SessionMetadata> sessions;

            for (const auto& item : response["sessions"])
            {
                SessionMetadata meta;
                meta.session_id = item["sessionId"].get<std::string>();
                if (item.contains("summary") && !item["summary"].is_null())
                    meta.summary = item["summary"].get<std::string>();
                sessions.push_back(std::move(meta));
            }

            return sessions;
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
                rpc_->invoke("session.delete", json{{"sessionId", session_id}}).get();

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

            auto response = rpc_->invoke("session.getLastId", json::object()).get();

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

            auto response = rpc_->invoke("ping", params).get();

            PingResponse result;
            if (response.contains("message") && !response["message"].is_null())
                result.message = response["message"].get<std::string>();
            if (response.contains("timestamp") && !response["timestamp"].is_null())
                result.timestamp = response["timestamp"].get<int64_t>();
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

            auto response = rpc_->invoke("status.get", json::object()).get();
            return response.get<GetStatusResponse>();
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

            auto response = rpc_->invoke("auth.getStatus", json::object()).get();
            return response.get<GetAuthStatusResponse>();
        }
    );
}

std::future<std::vector<ModelInfo>> Client::list_models()
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

            auto response = rpc_->invoke("models.list", json::object()).get();
            std::vector<ModelInfo> models;
            if (response.contains("models") && response["models"].is_array())
            {
                for (const auto& m : response["models"])
                    models.push_back(m.get<ModelInfo>());
            }
            return models;
        }
    );
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
    if (!tool)
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
        // Default deny on unknown session
        return json{
            {"result", {{"kind", "denied-no-approval-rule-and-could-not-request-from-user"}}}
        };
    }

    try
    {
        PermissionRequest request;
        request.kind = perm_data["kind"].get<std::string>();
        if (perm_data.contains("toolCallId") && !perm_data["toolCallId"].is_null())
            request.tool_call_id = perm_data["toolCallId"].get<std::string>();
        // Collect all other fields as extension data
        for (auto& [key, value] : perm_data.items())
            if (key != "kind" && key != "toolCallId")
                request.extension_data[key] = value;

        auto result = session->handle_permission_request(request);

        // Return response with nested result object
        json response;
        response["result"]["kind"] = result.kind;
        if (result.rules.has_value())
            response["result"]["rules"] = *result.rules;
        return response;
    }
    catch (const std::exception&)
    {
        // Default deny on errors
        return json{
            {"result", {{"kind", "denied-no-approval-rule-and-could-not-request-from-user"}}}
        };
    }
}

} // namespace copilot
