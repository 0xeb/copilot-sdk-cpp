// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cctype>
#include <chrono>
#include <copilot/cancellation.hpp>
#include <copilot/canvas.hpp>
#include <copilot/client.hpp>
#include <copilot/request_handler.hpp>
#include <copilot/rpc_methods.hpp>
#include <copilot/session.hpp>
#include <copilot/transport_ffi.hpp>
#include <copilot/workflow.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <regex>
#include <set>
#include <thread>

namespace copilot
{

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
        buf,
        sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        static_cast<unsigned>((hi >> 32) & 0xFFFFFFFFULL),
        static_cast<unsigned>((hi >> 16) & 0xFFFFULL),
        static_cast<unsigned>(hi & 0xFFFFULL),
        static_cast<unsigned>((lo >> 48) & 0xFFFFULL),
        static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL)
    );
    return buf;
}

struct ScopeExit
{
    std::function<void()> action;
    ~ScopeExit()
    {
        if (action)
            action();
    }
};

constexpr auto kCleanupTimeout = std::chrono::seconds(10);
constexpr const char* kSessionInitializationCancelled =
    "Client disconnected during session initialization";

bool is_ipv6_literal(const std::string& host)
{
    if (host.find(':') == std::string::npos)
        return false;
    return std::all_of(
        host.begin(),
        host.end(),
        [](unsigned char ch) { return std::isxdigit(ch) || ch == ':' || ch == '.'; }
    );
}

// NODEJS REFERENCE: client.ts constructor (tcp host validation)
bool is_valid_tcp_host(const std::string& host)
{
    if (host.empty())
        return false;
    for (unsigned char ch : host)
    {
        if (std::isspace(ch) || std::strchr("/\\?#@[]", static_cast<char>(ch)))
            return false;
    }
    return host.find(':') == std::string::npos || is_ipv6_literal(host);
}

bool starts_with_icase(const std::string& value, const std::string& prefix)
{
    if (value.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

bool is_abbreviated_uuid_prefix(const std::string& session_id)
{
    static const std::regex pattern("^[0-9a-f]{7}[0-9a-f-]{0,28}$", std::regex::icase);
    return std::regex_match(session_id, pattern);
}

bool is_uuid_session_id(const std::string& session_id)
{
    static const std::regex pattern(
        "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", std::regex::icase
    );
    return is_abbreviated_uuid_prefix(session_id) || std::regex_match(session_id, pattern);
}

// NODEJS REFERENCE: client.ts sessionIdsOverlap
bool session_ids_overlap(const std::string& left, const std::string& right)
{
    if (left == right)
        return true;
    if (!is_uuid_session_id(left) || !is_uuid_session_id(right))
        return false;
    return starts_with_icase(left, right) || starts_with_icase(right, left);
}

std::optional<std::string> string_field(const json& object, const char* key)
{
    if (object.is_object() && object.contains(key) && object[key].is_string())
        return object[key].get<std::string>();
    return std::nullopt;
}

SessionCapabilities capabilities_of(const json& response)
{
    if (response.contains("capabilities") && !response["capabilities"].is_null())
        return response["capabilities"].get<SessionCapabilities>();
    return SessionCapabilities{};
}

// Handler registration shared by create and resume, in the upstream order.
// NODEJS REFERENCE: client.ts createSession / resumeSession handler registration
template <typename Options>
void register_initial_handlers(Session& session, const Options& options)
{
    session.register_tools(options.tools.definitions);
    session.register_canvases(options.canvases.definitions);
    session.register_commands(options.interaction.commands);
    if (options.inference.providers)
    {
        std::map<std::string, BearerTokenProvider> providers;
        for (const auto& provider : *options.inference.providers)
            if (provider.bearer_token_provider)
                providers[provider.name] = provider.bearer_token_provider;
        if (!providers.empty())
            session.register_bearer_token_providers(std::move(providers));
    }
    if (options.permissions.on_request_with_context)
        session.register_permission_handler(options.permissions.on_request_with_context);
    else if (options.permissions.on_request)
        session.register_permission_handler(options.permissions.on_request);
    if (options.permissions.host_policy_pre_check &&
        options.permissions.host_policy_pre_check->on_request)
        session.register_host_policy_pre_check(options.permissions.host_policy_pre_check->on_request);
    if (options.interaction.on_user_input_request)
        session.register_user_input_handler(options.interaction.on_user_input_request);
    if (options.interaction.on_shell_authentication_request)
        session.register_shell_authentication_handler(
            options.interaction.on_shell_authentication_request
        );
    if (options.interaction.on_elicitation_request)
        session.register_elicitation_handler(options.interaction.on_elicitation_request);
    if (options.interaction.on_exit_plan_mode_request)
        session.register_exit_plan_mode_handler(options.interaction.on_exit_plan_mode_request);
    if (options.interaction.on_auto_mode_switch_request)
        session.register_auto_mode_switch_handler(options.interaction.on_auto_mode_switch_request);
    if (options.hooks.handlers)
        session.register_hooks(*options.hooks.handlers);
    if (options.skills.provider)
        session.register_skill_provider(options.skills.provider);
    if (options.mcp.on_auth_request)
        session.register_mcp_auth_handler(options.mcp.on_auth_request);
    if (options.state.memory && options.state.memory->store_memory)
        session.register_memory_store(options.state.memory->store_memory);
    // NODEJS REFERENCE: client.ts extractTransformCallbacks
    if (options.system_message && options.system_message->sections)
    {
        std::map<std::string, SectionTransformFn> transforms;
        for (const auto& [section_id, section] : *options.system_message->sections)
            if (section.transform)
                transforms[section_id] = section.transform;
        if (!transforms.empty())
            session.register_transform_callbacks(std::move(transforms));
    }
    if (options.events.on_event)
        session.register_persistent_event_handler(options.events.on_event);
}

// NODEJS REFERENCE: client.ts configureHostPolicyPreCheck
void configure_host_policy_pre_check(
    Session& session, const std::optional<HostPolicyPreCheckOptions>& options
)
{
    if (!options)
        return;
    const json capabilities =
        session.invoke(copilot::rpc::methods::kSessionPermissionsGetCapabilities, json::object())
            .get();
    if (!capabilities.is_object() || capabilities.value("hostPolicyPreCheck", false) != true)
        return;
    session
        .invoke(
            copilot::rpc::methods::kSessionPermissionsConfigure,
            json{{"hostPolicyPreCheck", {{"enabled", true}, {"policyRevision", options->policy_revision}}}}
        )
        .get();
}

template <typename Options>
bool has_mcp_auth_handler(const Options& options)
{
    return static_cast<bool>(options.mcp.on_auth_request);
}

template <typename Options>
bool managed_settings_enabled(const Options& options)
{
    return options.github.fetch_managed_settings.value_or(false) ||
           options.policy.managed_settings.has_value();
}

void register_mcp_oauth_interest(
    const std::shared_ptr<JsonRpcClient>& connection, const std::string& session_id
)
{
    connection
        ->invoke(
            "session.eventLog.registerInterest",
            json{{"sessionId", session_id}, {"eventType", "mcp.oauth_required"}}
        )
        .get();
}

// Best-effort session.options.update after create/resume; disconnects the session on failure.
// NODEJS REFERENCE: client.ts updateSessionOptionsForMode
template <typename Options>
void apply_options_update(Session& session, const Options& options, ClientMode mode)
{
    auto patch = build_session_options_update(options, mode);
    if (!patch)
        return;
    try
    {
        session.invoke("session.options.update", std::move(*patch)).get();
    }
    catch (...)
    {
        try
        {
            session.disconnect().get();
        }
        catch (...)
        {
        }
        throw;
    }
}

void detach_quietly(const std::shared_ptr<JsonRpcClient>& connection, const std::string& session_id)
{
    try
    {
        connection->invoke(
            copilot::rpc::methods::kSessionDetach, json{{"sessionId", session_id}}, kCleanupTimeout
        )
            .get();
    }
    catch (...)
    {
    }
}

void apply_granted_environment(
    const json& response, const std::optional<std::vector<std::string>>& requested
)
{
    if (!requested || !response.contains("grantedEnvironmentVariables") ||
        !response["grantedEnvironmentVariables"].is_object())
        return;
    const auto& granted = response["grantedEnvironmentVariables"];
    for (const auto& name : *requested)
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

} // namespace

// =============================================================================
// Client Implementation
// =============================================================================

// NODEJS REFERENCE: client.ts constructor
Client::Client(ClientOptions options) : options_(std::move(options))
{
    runtime_ = options_.runtime ? *options_.runtime : RuntimeConnection::for_default();
    const auto kind = runtime_.kind;
    const bool attached =
        kind == RuntimeConnectionKind::Tcp || kind == RuntimeConnectionKind::ParentProcess;

    if (attached)
    {
        std::vector<std::string> unsupported;
        if (runtime_.base_directory)
            unsupported.push_back("runtime.baseDirectory");
        if (options_.github.token)
            unsupported.push_back("github.token");
        if (options_.github.use_logged_in_user)
            unsupported.push_back("github.useLoggedInUser");
        if (options_.observability.log_level)
            unsupported.push_back("observability.logLevel");
        if (options_.observability.telemetry)
            unsupported.push_back("observability.telemetry");
        if (runtime_.session_idle_timeout_seconds)
            unsupported.push_back("runtime.sessionIdleTimeoutSeconds");
        if (!unsupported.empty())
        {
            std::string names;
            for (const auto& name : unsupported)
                names += (names.empty() ? "" : ", ") + name;
            throw std::invalid_argument(
                "Cannot configure runtime startup options for an attached runtime: " + names
            );
        }
    }
    if (kind == RuntimeConnectionKind::InProcess && runtime_.environment)
    {
        throw std::invalid_argument(
            "env is not supported with RuntimeConnection.forInProcess(): environment variables "
            "belong to the shared host process. Use typed client/session options or a "
            "child-process transport."
        );
    }
    if (kind == RuntimeConnectionKind::InProcess && runtime_.working_directory)
    {
        throw std::invalid_argument(
            "workingDirectory is not supported with RuntimeConnection.forInProcess(): the "
            "in-process transport hosts the runtime in this process, so honoring it would require "
            "mutating the shared process-global cwd. Set CreateSessionOptions.workingDirectory "
            "instead."
        );
    }
    if (kind == RuntimeConnectionKind::SpawnedTcp && runtime_.connection_token &&
        runtime_.connection_token->empty())
        throw std::invalid_argument("connectionToken must be a non-empty string");

    // NODEJS REFERENCE: client.ts validateSessionFsConfig
    if (options_.session_fs)
    {
        const auto& fs = *options_.session_fs;
        if (fs.initial_cwd.empty())
            throw std::invalid_argument("sessionFs.initialCwd is required");
        if (fs.session_state_path.empty())
            throw std::invalid_argument("sessionFs.sessionStatePath is required");
        if (fs.conventions != "windows" && fs.conventions != "posix")
            throw std::invalid_argument("sessionFs.conventions must be either 'windows' or 'posix'");
    }
    if (options_.plugins.builtin_directories)
    {
        for (const auto& path : *options_.plugins.builtin_directories)
        {
            if (!std::filesystem::path(path).is_absolute())
                throw std::invalid_argument(
                    "builtinPluginDirectories must contain only absolute paths: " + path
                );
        }
    }

    if (kind == RuntimeConnectionKind::Tcp)
    {
        const auto host = runtime_.host.value_or("");
        if (!is_valid_tcp_host(host))
            throw std::invalid_argument(
                "host must be a hostname or IP address without a scheme, port, or path"
            );
        if (runtime_.port < 1 || runtime_.port > 65535)
            throw std::invalid_argument("port must be an integer from 1 through 65535");
        tcp_host_ = host;
        tcp_port_ = runtime_.port;
        effective_token_ = runtime_.connection_token;
    }
    else if (kind == RuntimeConnectionKind::SpawnedTcp)
    {
        effective_token_ = runtime_.connection_token ? *runtime_.connection_token
                                                     : generate_uuid_v4();
    }

    if (kind == RuntimeConnectionKind::SpawnedStdio || kind == RuntimeConnectionKind::SpawnedTcp)
    {
        if (runtime_.path)
        {
            cli_path_ = *runtime_.path;
        }
        else
        {
            std::optional<std::string> from_env;
            if (runtime_.environment)
            {
                const auto it = runtime_.environment->find("COPILOT_CLI_PATH");
                if (it != runtime_.environment->end())
                    from_env = it->second;
            }
            else if (auto value = detail::env_value("COPILOT_CLI_PATH"))
            {
                from_env = std::move(value);
            }
            cli_path_ = from_env && !from_env->empty() ? *from_env : std::string("copilot");
        }
    }

    if (options_.mode == ClientMode::Empty && !runtime_.base_directory && !options_.session_fs &&
        !attached)
    {
        throw std::invalid_argument(
            "CopilotClient was created with mode: 'empty' but neither 'baseDirectory' nor "
            "'sessionFs' was set. Empty mode requires an explicit per-session persistence "
            "location; pick one."
        );
    }
}

Client::~Client()
{
    force_stop();
    // A cancelled start unwinds promptly; it must not outlive the members it uses.
    for (;;)
    {
        {
            std::lock_guard<std::mutex> lock(lifetime_mutex_);
            if (start_attempts_.empty())
                break;
        }
        cancel_startup(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// =============================================================================
// Connection helpers and GitHub token registry
// =============================================================================

std::shared_ptr<JsonRpcClient> Client::ensure_connected()
{
    if (state_ != ConnectionState::Connected)
        start().get();
    std::lock_guard<std::mutex> lock(lifetime_mutex_);
    if (!live_rpc_)
        throw JsonRpcError(JsonRpcErrorCode::ConnectionClosed, "Client not connected");
    return live_rpc_;
}

namespace
{
// NODEJS REFERENCE: client.ts createSessionInternal (signal.throwIfAborted around session.create)
json await_session_initialization(std::future<json> response, const CancellationToken& lifetime)
{
    while (response.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout)
        if (lifetime.is_cancelled())
            throw std::runtime_error(kSessionInitializationCancelled);
    return response.get();
}
} // namespace

// NODEJS REFERENCE: client.ts registerGitHubTokenProvider
std::string Client::register_github_token_provider(GitHubTokenProvider provider)
{
    auto id = generate_uuid_v4();
    std::lock_guard<std::mutex> lock(registry_mutex_);
    github_token_registrations_[id] = GitHubTokenRegistration{std::move(provider), {}, false};
    return id;
}

void Client::assign_github_token_registration(const std::string& id, const std::string& session_id)
{
    std::lock_guard<std::mutex> lock(registry_mutex_);
    const auto it = github_token_registrations_.find(id);
    if (it != github_token_registrations_.end())
        it->second.session_id = session_id;
}

// A committed registration replaces any earlier committed one for the same session.
void Client::commit_github_token_registration(const std::string& id)
{
    std::lock_guard<std::mutex> lock(registry_mutex_);
    const auto it = github_token_registrations_.find(id);
    if (it == github_token_registrations_.end())
        return;
    const auto session_id = it->second.session_id;
    for (auto other = github_token_registrations_.begin();
         other != github_token_registrations_.end();)
    {
        if (other->first != id && other->second.committed && session_id &&
            other->second.session_id == session_id)
            other = github_token_registrations_.erase(other);
        else
            ++other;
    }
    it->second.committed = true;
}

void Client::remove_github_token_registration(const std::string& id)
{
    std::lock_guard<std::mutex> lock(registry_mutex_);
    github_token_registrations_.erase(id);
}

// NODEJS REFERENCE: client.ts markDisconnected
void Client::handle_disconnect(const JsonRpcClient* source)
{
    std::map<std::string, std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A connection the client has already replaced must not tear down its successor;
        // without a source, teardown is skipped only once a new connection is live.
        if (rpc_ && rpc_.get() != source)
            return;
        clear_owned_runtime_port();
        sessions.swap(sessions_);
        session_admission_closed_ = true;
        if (rpc_)
            state_ = ConnectionState::Disconnected;
    }
    for (const auto& [_, session] : sessions)
        if (session)
            session->retire();

    {
        // NODEJS REFERENCE: client.ts clearSessionRoutes
        std::lock_guard<std::mutex> lock(held_mutex_);
        held_session_events_.clear();
        held_session_event_count_ = 0;
    }

    std::map<std::string, std::map<std::string, CancellationSource>> shell_auth;
    std::map<uint64_t, CancellationSource> memory_stores;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        shell_auth.swap(pending_shell_auth_);
        memory_stores.swap(pending_memory_stores_);
        github_token_registrations_.clear();
    }
    for (auto& [_, pending] : shell_auth)
        for (auto& [__, source] : pending)
            source.cancel();
    for (auto& [_, source] : memory_stores)
        source.cancel();
    disconnect_hosts();
}

// =============================================================================
// Connection Management
// =============================================================================

namespace
{
std::mutex g_start_phase_hook_mutex;
std::function<void(detail::StartPhase)> g_start_phase_hook;

void notify_start_phase(detail::StartPhase phase)
{
    std::function<void(detail::StartPhase)> hook;
    {
        std::lock_guard<std::mutex> lock(g_start_phase_hook_mutex);
        hook = g_start_phase_hook;
    }
    if (hook)
        hook(phase);
}

[[noreturn]] void throw_start_cancelled()
{
    throw StartCancelled("Client start was cancelled");
}

} // namespace

void detail::set_start_phase_hook(std::function<void(StartPhase)> hook)
{
    std::lock_guard<std::mutex> lock(g_start_phase_hook_mutex);
    g_start_phase_hook = std::move(hook);
}

bool Client::StartAttempt::cancel()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (finished)
        return false;
    if (cancelled.exchange(true))
        return true;
    // Kills and fails without running connection callbacks, so a cancel never waits on
    // mutex_; the starting thread does the teardown. The starting thread destroys the
    // process only after release(), which takes this lock.
    if (process)
        process->kill();
    if (rpc)
        rpc->fail_pending(JsonRpcErrorCode::ConnectionClosed, "Client start was cancelled");
    cancellation.cancel();
    return true;
}

void Client::StartAttempt::throw_if_cancelled() const
{
    if (cancelled.load())
        throw_start_cancelled();
}

void Client::StartAttempt::publish(Process* value)
{
    std::lock_guard<std::mutex> lock(mutex);
    process = value;
    // Cancelled while spawning, before stop() could see the process.
    if (cancelled.load() && process)
        process->kill();
}

void Client::StartAttempt::publish(std::shared_ptr<JsonRpcClient> value)
{
    std::lock_guard<std::mutex> lock(mutex);
    rpc = std::move(value);
    if (cancelled.load() && rpc)
        rpc->fail_pending(JsonRpcErrorCode::ConnectionClosed, "Client start was cancelled");
}

void Client::StartAttempt::finish()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (cancelled.load())
        throw_start_cancelled();
    finished = true;
    process = nullptr;
    rpc.reset();
}

void Client::StartAttempt::release()
{
    std::lock_guard<std::mutex> lock(mutex);
    finished = true;
    process = nullptr;
    rpc.reset();
}

void Client::set_live_rpc(std::shared_ptr<JsonRpcClient> rpc)
{
    std::shared_ptr<JsonRpcClient> previous;
    {
        std::lock_guard<std::mutex> lock(lifetime_mutex_);
        previous = std::move(live_rpc_);
        live_rpc_ = std::move(rpc);
    }
}

CancellationToken Client::session_initialization_token() const
{
    std::lock_guard<std::mutex> lock(lifetime_mutex_);
    return session_initialization_.token();
}

// C# REFERENCE: Client.cs StopAsync/ForceStopAsync (_startupCancellation.Cancel)
// NODEJS REFERENCE: client.ts invalidateSessionInitializations
void Client::cancel_startup(bool end_session_initializations)
{
    std::vector<std::shared_ptr<StartAttempt>> attempts;
    {
        std::lock_guard<std::mutex> lock(lifetime_mutex_);
        attempts = start_attempts_;
    }
    for (const auto& attempt : attempts)
        attempt->cancel();
    if (!end_session_initializations)
        return;
    // Read after the attempts: a start that finished first has already renewed the source,
    // so the renewed one is the one cancelled.
    CancellationSource initialization;
    {
        std::lock_guard<std::mutex> lock(lifetime_mutex_);
        initialization = session_initialization_;
    }
    initialization.cancel();
}

std::future<void> Client::start()
{
    return start(std::stop_token{});
}

// C# REFERENCE: Client.cs StartAsync(CancellationToken)
std::future<void> Client::start(std::stop_token token)
{
    auto attempt = std::make_shared<StartAttempt>();
    {
        std::lock_guard<std::mutex> lock(lifetime_mutex_);
        start_attempts_.push_back(attempt);
        // NODEJS REFERENCE: client.ts start (a start requested during stop() fails)
        if (stops_in_progress_.load() > 0)
            attempt->cancel();
    }
    try
    {
        return std::async(
            std::launch::async,
            [this, attempt, token = std::move(token)] { run_start(attempt, token); }
        );
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(lifetime_mutex_);
        std::erase(start_attempts_, attempt);
        throw;
    }
}

void Client::run_start(const std::shared_ptr<StartAttempt>& attempt, std::stop_token token)
{
    ScopeExit forget{[this, attempt]
                     {
                         std::lock_guard<std::mutex> lock(lifetime_mutex_);
                         std::erase(start_attempts_, attempt);
                     }};
    // Declared after `forget` and before any lock: its destructor waits for a running
    // callback, which may need mutex_.
    std::stop_callback on_stop(token, [attempt] { attempt->cancel(); });
    attempt->throw_if_cancelled();

    {
        // NODEJS REFERENCE: client.ts doStart (killCliProcess before spawning): reap what a
        // timed-out stop(), an abort() or a lost connection left, outside mutex_ because
        // closing a connection runs callbacks that take it.
        std::vector<UnexitedRuntime> stale;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == ConnectionState::Connected)
            {
                attempt->finish();
                return;
            }
            stale = std::move(unexited_runtimes_);
            unexited_runtimes_.clear();
            if (process_ || rpc_)
                stale.push_back(UnexitedRuntime{std::move(rpc_), std::move(process_)});
            set_live_rpc(nullptr);
        }
        if (!stale.empty())
        {
            auto survivors = reap_runtimes(std::move(stale), kCleanupTimeout);
            if (!survivors.empty())
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (auto& survivor : survivors)
                    unexited_runtimes_.push_back(std::move(survivor));
            }
        }
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ == ConnectionState::Connected)
    {
        attempt->finish();
        return;
    }
    try
    {
        attempt->throw_if_cancelled();
        state_ = ConnectionState::Connecting;
        start_impl(*attempt);
        set_live_rpc(rpc_);
        {
            // NODEJS REFERENCE: client.ts doStart (a new sessionInitializationLifetime)
            std::lock_guard<std::mutex> lifetime(lifetime_mutex_);
            session_initialization_ = CancellationSource{};
        }
        attempt->finish();
        state_ = ConnectionState::Connected;
    }
    catch (...)
    {
        // NODEJS REFERENCE: client.ts doStart catch (doForceStop): a failed start keeps nothing.
        const bool cancelled = attempt->cancelled.load();
        attempt->release();
        std::unique_ptr<Process> process = std::move(process_);
        std::shared_ptr<JsonRpcClient> rpc = std::move(rpc_);
        std::unique_ptr<ITransport> transport = std::move(transport_);
        std::shared_ptr<CopilotRequestHandlerBridge> bridge = std::move(request_handler_bridge_);
        session_admission_closed_ = true;
        state_ = cancelled ? ConnectionState::Disconnected : ConnectionState::Error;
        clear_owned_runtime_port();
        set_live_rpc(nullptr);
        lock.unlock();

        if (bridge)
            bridge->cancel_all();
        if (process)
        {
            process->kill();
            process->wait_for(std::chrono::seconds(5));
            process->close_pipes();
        }
        if (rpc)
            rpc->stop();
        if (transport)
            transport->close();
        if (cancelled)
            throw_start_cancelled();
        throw;
    }
}

void Client::start_impl(StartAttempt& attempt)
{
    switch (runtime_.kind)
    {
    case RuntimeConnectionKind::InProcess:
        start_in_process_runtime();
        attempt.throw_if_cancelled();
        connect_to_server(&attempt);
        break;
    case RuntimeConnectionKind::Tcp:
        // C# REFERENCE: Client.cs StartAsync (an external runtime keeps its port)
        runtime_port_.store(tcp_port_);
        connect_to_server(&attempt);
        break;
    case RuntimeConnectionKind::ParentProcess:
        connect_to_server(&attempt);
        break;
    case RuntimeConnectionKind::SpawnedStdio:
    case RuntimeConnectionKind::SpawnedTcp:
        start_cli_server(attempt);
        if (runtime_.kind == RuntimeConnectionKind::SpawnedTcp)
            runtime_port_.store(tcp_port_);
        connect_to_server(&attempt);
        break;
    }
    attempt.throw_if_cancelled();

    verify_protocol_version();
    attempt.throw_if_cancelled();
    session_admission_closed_ = false;
    // NODEJS REFERENCE: client.ts start (network.setProvider)
    if (options_.network &&
        (options_.network->resolve_proxy || options_.network->authenticate_negotiate ||
         options_.network->authenticate_basic))
    {
        json params = json::object();
        if (options_.network->scope)
            params["scope"] =
                *options_.network->scope == NetworkScope::Runtime ? "runtime" : "connection";
        if (options_.network->resolve_proxy)
            params["resolveProxy"] = json::object();
        if (options_.network->authenticate_negotiate)
            params["authenticateNegotiate"] = json::object();
        if (options_.network->authenticate_basic)
            params["authenticateBasic"] = json::object();
        rpc_->invoke(copilot::rpc::methods::kNetworkSetProvider, params).get();
        attempt.throw_if_cancelled();
    }
    if (options_.extensions.launch_provider)
    {
        rpc_->invoke("registerExtensionLaunchProvider", json::object()).get();
        attempt.throw_if_cancelled();
    }

    if (options_.plugins.builtin_directories && !options_.plugins.builtin_directories->empty())
    {
        rpc_->invoke("plugins.builtin.set", json{{"paths", *options_.plugins.builtin_directories}})
            .get();
        attempt.throw_if_cancelled();
    }

    if (options_.session_fs)
    {
        json params{
            {"initialCwd", options_.session_fs->initial_cwd},
            {"sessionStatePath", options_.session_fs->session_state_path},
            {"conventions", options_.session_fs->conventions},
        };
        if (options_.session_fs->capabilities)
            params["capabilities"] = json(*options_.session_fs->capabilities);
        rpc_->invoke(copilot::rpc::methods::kSessionFsSetProvider, params).get();
        attempt.throw_if_cancelled();
    }

    if (request_handler_bridge_)
    {
        rpc_->invoke("llmInference.setProvider", json::object()).get();
        attempt.throw_if_cancelled();
    }
}

std::optional<int> Client::runtime_port() const
{
    const int port = runtime_port_.load();
    return port > 0 ? std::optional<int>(port) : std::nullopt;
}

void Client::clear_owned_runtime_port()
{
    // C# REFERENCE: Client.cs ClearRuntimePort (only a port this client started is cleared)
    if (runtime_.kind == RuntimeConnectionKind::SpawnedTcp)
        runtime_port_.store(0);
}

std::future<std::vector<StopError>> Client::stop()
{
    return stop(std::chrono::steady_clock::time_point::max());
}

std::future<std::vector<StopError>> Client::stop(std::chrono::steady_clock::time_point deadline)
{
    // NODEJS REFERENCE: client.ts stop (invalidateSessionInitializations before doStop)
    stops_in_progress_.fetch_add(1);
    try
    {
        cancel_startup(true);
        return std::async(
            std::launch::async,
            [this, deadline]
            {
                ScopeExit done{[this] { stops_in_progress_.fetch_sub(1); }};
                return stop_impl(deadline);
            }
        );
    }
    catch (...)
    {
        stops_in_progress_.fetch_sub(1);
        throw;
    }
}

// NODEJS REFERENCE: client.ts doStop
// C# REFERENCE: Client.cs StopAsync / CleanupConnectionAsync / CleanupCliProcessAsync
std::vector<StopError> Client::stop_impl(std::chrono::steady_clock::time_point deadline)
{
    using clock = std::chrono::steady_clock;
    const auto remaining_until = [](clock::time_point until)
    {
        const auto now = clock::now();
        if (until <= now)
            return std::chrono::milliseconds(0);
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::min<clock::duration>(until - now, std::chrono::hours(24))
        );
    };

    std::vector<StopError> errors;
    std::vector<std::shared_ptr<Session>> sessions;
    std::shared_ptr<JsonRpcClient> connection;
    std::shared_ptr<CopilotRequestHandlerBridge> bridge;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_admission_closed_ = true;
        for (const auto& [_, session] : sessions_)
            if (session)
                sessions.push_back(session);
        connection = rpc_;
        bridge = request_handler_bridge_;
    }

    if (bridge)
        bridge->cancel_all();
    for (const auto& session : sessions)
        session->retire();

    // A hung runtime may never answer detach, so all sessions share one deadline. Sessions
    // whose create/resume the runtime never confirmed are not detached.
    const auto now = clock::now();
    const bool caller_bounds_detach = deadline < now + kCleanupTimeout;
    const auto detach_deadline = caller_bounds_detach ? deadline : now + kCleanupTimeout;
    const auto closed =
        connection ? connection->connection_closed_token() : CancellationToken{};
    const std::string detach_timeout_message =
        caller_bounds_detach
            ? std::string("session.detach did not complete before the stop() deadline")
            : "session.detach did not complete within the " +
                  std::to_string(
                      std::chrono::duration_cast<std::chrono::milliseconds>(kCleanupTimeout).count()
                  ) +
                  "ms shutdown timeout";
    for (const auto& session : sessions)
    {
        if (!session->is_confirmed())
            continue;
        std::optional<std::string> last_error;
        int attempts = 0;
        for (int attempt = 1; attempt <= 3; ++attempt)
        {
            // A closed connection has already disconnected every session on it.
            if (!connection || closed.is_cancelled())
            {
                last_error.reset();
                break;
            }
            attempts = attempt;
            const auto remaining = remaining_until(detach_deadline);
            if (remaining.count() <= 0)
            {
                // NODEJS REFERENCE: client.ts doStop (the race against an elapsed deadline)
                last_error = detach_timeout_message;
                break;
            }
            try
            {
                connection
                    ->invoke(
                        copilot::rpc::methods::kSessionDetach,
                        json{{"sessionId", session->session_id()}},
                        remaining
                    )
                    .get();
                last_error.reset();
                break;
            }
            catch (const JsonRpcError& error)
            {
                if (closed.is_cancelled())
                {
                    last_error.reset();
                    break;
                }
                if (error.code() == JsonRpcErrorCode::Timeout)
                {
                    // The detach is still in flight on the runtime; do not retry it.
                    last_error = detach_timeout_message;
                    break;
                }
                last_error = error.what();
            }
            catch (const std::exception& error)
            {
                if (closed.is_cancelled())
                {
                    last_error.reset();
                    break;
                }
                last_error = error.what();
            }
            if (attempt < 3)
                std::this_thread::sleep_for(std::min(
                    std::chrono::milliseconds(100 << (attempt - 1)), remaining_until(detach_deadline)
                ));
        }
        if (last_error)
            errors.push_back(StopError{
                "Failed to disconnect session " + session->session_id() + " after " +
                std::to_string(attempts) + (attempts == 1 ? " attempt: " : " attempts: ") +
                *last_error
            });
    }

    {
        std::lock_guard<std::mutex> cache_lock(models_cache_mutex_);
        models_cache_.reset();
    }

    std::unique_ptr<Process> process;
    std::shared_ptr<JsonRpcClient> rpc;
    std::unique_ptr<ITransport> transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_admission_closed_ = true;
        // Before dropping the sessions: ~Session waits on in-flight tool handlers,
        // and each answers with an RPC. Cancel first so a handler finishing during
        // teardown drops its result instead of calling into a transport that is
        // already going away.
        for (auto& [id, session] : sessions_)
            if (session)
                session->retire();
        sessions_.clear();
        process = std::move(process_);
        stopping_process_ = process.get();
        rpc = std::move(rpc_);
        stopping_rpc_ = rpc;
        transport = std::move(transport_);
        bridge = std::move(request_handler_bridge_);
        state_ = ConnectionState::Disconnected;
    }
    set_live_rpc(nullptr);
    struct StoppingReset
    {
        Client* client;
        ~StoppingReset()
        {
            std::lock_guard<std::mutex> lock(client->mutex_);
            client->stopping_process_ = nullptr;
            client->stopping_rpc_.reset();
        }
    } stopping_reset{this};

    // A connection that already failed cannot carry runtime.shutdown, and a runtime that
    // dropped it gets no grace period.
    const bool connection_alive = rpc && !rpc->connection_closed_token().is_cancelled();
    if (connection_alive && (process || runtime_.kind == RuntimeConnectionKind::InProcess))
    {
        const auto timeout = std::min<std::chrono::milliseconds>(
            std::chrono::duration_cast<std::chrono::milliseconds>(kCleanupTimeout),
            remaining_until(deadline)
        );
        if (timeout.count() > 0)
        {
            try
            {
                rpc->invoke("runtime.shutdown", json::object(), timeout).get();
            }
            catch (...)
            {
            }
        }
    }
    // Close stdin before the grace period so the runtime can flush telemetry.
    if (bridge)
    {
        bridge->cancel_all();
        bridge.reset();
    }
    const bool use_stdio = runtime_.kind == RuntimeConnectionKind::SpawnedStdio;
    if (process)
    {
        if (use_stdio)
        {
            try
            {
                process->stdin_pipe().close();
            }
            catch (...)
            {
            }
        }
        bool exited = false;
        bool deadline_cut_grace = false;
        if (use_stdio && connection_alive)
        {
            const auto grace = std::min<std::chrono::milliseconds>(
                std::chrono::duration_cast<std::chrono::milliseconds>(kCleanupTimeout),
                remaining_until(deadline)
            );
            deadline_cut_grace = grace < kCleanupTimeout;
            exited = process->wait_for(grace).has_value();
        }
        else
        {
            exited = process->wait_for(std::chrono::milliseconds(0)).has_value();
        }
        bool exit_timed_out = false;
        if (!exited)
        {
            if (use_stdio && deadline_cut_grace)
                errors.push_back(
                    StopError{"Runtime did not exit before the stop() deadline; it was killed"}
                );
            process->kill();
            // Past the caller's deadline the kill still gets a moment to land.
            const auto wait = std::max<std::chrono::milliseconds>(
                std::min<std::chrono::milliseconds>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(kCleanupTimeout),
                    remaining_until(deadline)
                ),
                std::chrono::milliseconds(250)
            );
            if (!process->wait_for(wait))
            {
                exit_timed_out = true;
                errors.push_back(StopError{"Timed out waiting for runtime process to exit"});
            }
        }
        process->close_pipes();
        if (exit_timed_out)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_process_ = nullptr;
            unexited_runtimes_.push_back(UnexitedRuntime{nullptr, std::move(process)});
        }
    }

    // Now stop RPC client - read thread will unblock since pipes are closed
    if (rpc)
        rpc->stop();

    if (transport)
        transport->close();
    handle_disconnect();
    return errors;
}

void Client::force_stop()
{
    cancel_startup(true);
    {
        std::lock_guard<std::mutex> cache_lock(models_cache_mutex_);
        models_cache_.reset();
    }

    std::vector<std::shared_ptr<Session>> retiring;
    std::unique_ptr<Process> process;
    std::shared_ptr<JsonRpcClient> rpc;
    std::unique_ptr<ITransport> transport;
    std::shared_ptr<CopilotRequestHandlerBridge> bridge;
    std::shared_ptr<JsonRpcClient> stopping_rpc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_admission_closed_ = true;
        // Before dropping the sessions: ~Session waits on in-flight tool handlers,
        // and each answers with an RPC. Cancel first so a handler finishing during
        // teardown drops its result instead of calling into a transport that is
        // already going away.
        for (auto& [id, session] : sessions_)
            if (session)
                session->retire();
        for (const auto& [id, session] : sessions_)
            retiring.push_back(session);
        sessions_.clear();
        process = std::move(process_);
        rpc = std::move(rpc_);
        transport = std::move(transport_);
        bridge = std::move(request_handler_bridge_);
        state_ = ConnectionState::Disconnected;
        // TS REFERENCE: nodejs/src/client.ts doForceStop kills this.cliProcess, which a
        // concurrent stop() still holds while it waits for the runtime to exit.
        if (stopping_process_)
            stopping_process_->kill();
        stopping_rpc = stopping_rpc_;
    }
    set_live_rpc(nullptr);
    // Fails whatever a concurrent stop() is still waiting on (runtime.shutdown).
    if (stopping_rpc)
        stopping_rpc->signal_connection_closed();

    if (bridge)
        bridge->cancel_all();
    // Kill process FIRST - this closes the pipe ends and unblocks reads
    if (process)
    {
        process->kill();
        process->wait_for(std::chrono::seconds(5));
        process->close_pipes();
    }
    kill_unexited_process();

    // Now stop RPC client - read thread will unblock since pipes are closed
    if (rpc)
        rpc->stop();

    if (transport)
        transport->close();
    handle_disconnect();
}

void Client::abort()
{
    cancel_startup(true);
    {
        std::lock_guard<std::mutex> cache_lock(models_cache_mutex_);
        models_cache_.reset();
    }

    std::unique_ptr<Process> process;
    std::shared_ptr<JsonRpcClient> rpc;
    std::unique_ptr<ITransport> transport;
    std::shared_ptr<CopilotRequestHandlerBridge> bridge;
    std::shared_ptr<JsonRpcClient> stopping_rpc;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_admission_closed_ = true;
        sessions.swap(sessions_);
        process = std::move(process_);
        rpc = std::move(rpc_);
        transport = std::move(transport_);
        bridge = std::move(request_handler_bridge_);
        stopping_rpc = stopping_rpc_;
        state_ = ConnectionState::Disconnected;
    }
    set_live_rpc(nullptr);
    for (const auto& [_, session] : sessions)
        if (session)
            session->retire();
    if (bridge)
        bridge->cancel_all();
    if (stopping_rpc)
        stopping_rpc->signal_connection_closed();
    // Fails every pending request and cancels in-flight handler tokens without joining the
    // read thread, which waits on the runtime's stdout until the runtime exits.
    if (rpc)
        rpc->signal_connection_closed();
    if (transport)
        transport->close();
    if (process && runtime_.kind == RuntimeConnectionKind::SpawnedStdio)
    {
        // End of input is the runtime's signal to exit.
        try
        {
            process->stdin_pipe().close();
        }
        catch (...)
        {
        }
    }
    if (process || rpc)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        unexited_runtimes_.push_back(UnexitedRuntime{std::move(rpc), std::move(process)});
    }
    handle_disconnect();
}

std::vector<Client::UnexitedRuntime>
Client::reap_runtimes(std::vector<UnexitedRuntime> runtimes, std::chrono::milliseconds timeout)
{
    std::vector<UnexitedRuntime> survivors;
    for (auto& runtime : runtimes)
    {
        if (runtime.process)
        {
            runtime.process->kill();
            if (!runtime.process->wait_for(timeout))
            {
                survivors.push_back(std::move(runtime));
                continue;
            }
            runtime.process->close_pipes();
        }
        if (runtime.rpc)
            runtime.rpc->stop();
    }
    return survivors;
}

// NODEJS REFERENCE: client.ts killCliProcess (cliProcess ?? unexitedCliProcess)
void Client::kill_unexited_process()
{
    std::vector<UnexitedRuntime> runtimes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimes.swap(unexited_runtimes_);
    }
    if (runtimes.empty())
        return;
    auto survivors = reap_runtimes(std::move(runtimes), kCleanupTimeout);
    if (survivors.empty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& survivor : survivors)
        unexited_runtimes_.push_back(std::move(survivor));
}

std::optional<int> Client::cli_process_id() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Process* process = process_ ? process_.get() : stopping_process_;
    if (!process || !process->is_running())
        return std::nullopt;
    const int pid = process->pid();
    return pid > 0 ? std::optional<int>(pid) : std::nullopt;
}

ConnectionState Client::state() const
{
    return state_.load();
}

// =============================================================================
// CLI Server Management
// =============================================================================

namespace
{
bool iequals(const std::string& left, const std::string& right)
{
    return left.size() == right.size() && starts_with_icase(left, right);
}

#ifdef _WIN32
std::string lookup_environment(
    const std::map<std::string, std::string>& environment, const std::string& name
)
{
    for (const auto& [key, value] : environment)
        if (iequals(key, name))
            return value;
    return {};
}

std::vector<std::string> split_list(const std::string& value, char separator)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= value.size())
    {
        const auto end = value.find(separator, start);
        auto part = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty())
            parts.push_back(std::move(part));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return parts;
}

bool file_exists(const std::filesystem::path& candidate)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec))
        return true;
    // App execution aliases (WindowsApps\*.exe) are reparse points std::filesystem cannot stat.
    const auto attributes = GetFileAttributesW(candidate.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// cmd.exe expands or splits on these even inside quotes; a shim cannot carry them safely.
bool safe_for_cmd(const std::string& value)
{
    return value.find_first_of("\"%!^\r\n") == std::string::npos;
}

std::string cmd_quote(const std::string& value)
{
    return "\"" + value + "\"";
}
#endif
} // namespace

// NODEJS REFERENCE: client.ts startCLIServer (spawn(cliPath, args) without a shell)
// C# REFERENCE: Client.cs StartCliServerAsync (UseShellExecute = false; no cmd /c since 42539466f)
Client::CliCommand Client::resolve_cli_command(
    const std::string& cli_path,
    const std::vector<std::string>& args,
    const std::map<std::string, std::string>& environment
)
{
    if (is_node_script(cli_path))
    {
        auto node_path = find_node();
        if (!node_path.has_value())
            throw std::runtime_error("Node.js not found in PATH but required for .js CLI");
        std::vector<std::string> full_args = {cli_path};
        full_args.insert(full_args.end(), args.begin(), args.end());
        return {*node_path, std::move(full_args), cli_path};
    }

    const std::filesystem::path path(cli_path);
    // An absolute path, or one relative to the working directory, is spawned as given.
    if (path.is_absolute() || path.has_parent_path())
        return {cli_path, args, cli_path};

    // A bare name is resolved here, against the PATH the runtime will see, so a native
    // executable is spawned directly: an intermediate cmd.exe would let a runtime that
    // re-launches itself escape the kill.
#ifdef _WIN32
    const bool own_environment = runtime_.environment.has_value();
    std::string search_path = own_environment ? lookup_environment(environment, "PATH") : "";
    if (search_path.empty())
        search_path = detail::env_value("PATH").value_or("");
    std::string pathext = own_environment ? lookup_environment(environment, "PATHEXT") : "";
    if (pathext.empty())
        pathext = detail::env_value("PATHEXT").value_or("");
    if (pathext.empty())
        pathext = ".COM;.EXE;.BAT;.CMD";

    const auto extension = path.extension().string();
    const bool has_extension = !extension.empty();
    std::vector<std::string> native{".EXE", ".COM"};
    std::vector<std::string> scripts;
    for (const auto& value : split_list(pathext, ';'))
        if (iequals(value, ".BAT") || iequals(value, ".CMD"))
            scripts.push_back(value);

    std::optional<std::filesystem::path> script;
    for (const auto& directory : split_list(search_path, ';'))
    {
        const std::filesystem::path base(directory);
        if (has_extension)
        {
            const auto candidate = base / path;
            if (!file_exists(candidate))
                continue;
            if (iequals(extension, ".bat") || iequals(extension, ".cmd"))
            {
                if (!script)
                    script = candidate;
                continue;
            }
            const auto found = candidate.string();
            return {found, args, found};
        }
        for (const auto& ext : native)
        {
            const auto candidate = base / (cli_path + ext);
            if (file_exists(candidate))
            {
                const auto found = candidate.string();
                return {found, args, found};
            }
        }
        if (!script)
            for (const auto& ext : scripts)
            {
                const auto candidate = base / (cli_path + ext);
                if (file_exists(candidate))
                {
                    script = candidate;
                    break;
                }
            }
    }
    if (script)
    {
        // Only the npm shim was found: run it by absolute path through cmd.exe. /s strips
        // the outer quotes, so the inner ones survive.
        const auto found = script->string();
        std::string line = cmd_quote(found);
        for (const auto& arg : args)
        {
            if (!safe_for_cmd(arg))
                throw std::runtime_error(
                    "Copilot CLI argument cannot be passed through " + found + ": " + arg
                );
            line += " " + cmd_quote(arg);
        }
        std::string comspec = detail::env_value("ComSpec").value_or("");
        if (comspec.empty())
            comspec = "cmd.exe";
        CliCommand command{comspec, {"/d", "/s", "/c", cmd_quote(line)}, found};
        command.verbatim_arguments = true;
        return command;
    }
#else
    std::optional<std::string> search_path;
    if (runtime_.environment.has_value())
        if (const auto it = environment.find("PATH"); it != environment.end())
            search_path = it->second;
    if (!search_path)
        search_path = detail::env_value("PATH");
    if (search_path)
    {
        std::size_t start = 0;
        while (start <= search_path->size())
        {
            const auto end = search_path->find(':', start);
            const auto directory = search_path->substr(
                start, end == std::string::npos ? std::string::npos : end - start
            );
            const auto candidate = std::filesystem::path(directory.empty() ? "." : directory) / path;
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec) &&
                ::access(candidate.c_str(), X_OK) == 0)
            {
                const auto found = std::filesystem::absolute(candidate, ec).string();
                return {found, args, found};
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
#endif
    throw std::runtime_error(
        "Copilot CLI '" + cli_path +
        "' was not found on PATH. Install it, or set COPILOT_CLI_PATH or "
        "RuntimeConnection::path to its location."
    );
}

// NODEJS REFERENCE: client.ts startInProcessRuntime
void Client::start_in_process_runtime()
{
    std::optional<std::string> entrypoint = runtime_.path;
    if (!entrypoint)
        entrypoint = detail::env_value("COPILOT_CLI_PATH");
    if (!entrypoint || entrypoint->empty())
        throw std::runtime_error(
            "in-process runtime requires a CLI entrypoint path or COPILOT_CLI_PATH"
        );

    std::filesystem::path library;
    if (runtime_.ffi_library_path)
    {
        library = *runtime_.ffi_library_path;
    }
    else
    {
        const auto sibling = std::filesystem::path(*entrypoint).parent_path() / "runtime.node";
        std::error_code ec;
        if (std::filesystem::exists(sibling, ec))
        {
            library = sibling;
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
            folder = entrypoint->find("linuxmusl-") != std::string::npos ? "linuxmusl-arm64"
                                                                          : "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
            folder = entrypoint->find("linuxmusl-") != std::string::npos ? "linuxmusl-x64"
                                                                          : "linux-x64";
#else
            throw std::runtime_error("unsupported architecture for in-process FFI runtime");
#endif
            library = std::filesystem::path(*entrypoint).parent_path() / "prebuilds" / folder /
                      "runtime.node";
        }
    }

    transport_ = std::make_unique<FfiTransport>(
        library.string(),
        *entrypoint,
        build_cli_environment(options_),
        build_in_process_args(options_)
    );
}

// NODEJS REFERENCE: client.ts startCLIServer
void Client::start_cli_server(StartAttempt& attempt)
{
    const bool use_stdio = runtime_.kind == RuntimeConnectionKind::SpawnedStdio;
    ProcessOptions proc_opts;
    proc_opts.redirect_stdin = use_stdio;
    proc_opts.redirect_stdout = true;
    proc_opts.redirect_stderr = true;
    proc_opts.create_no_window = true;
    if (runtime_.working_directory)
        proc_opts.working_directory = *runtime_.working_directory;
    proc_opts.inherit_environment = !runtime_.environment.has_value();
    proc_opts.environment = build_cli_environment(options_, effective_token_);

    auto command = resolve_cli_command(cli_path_, build_cli_command_args(options_), proc_opts.environment);
    proc_opts.verbatim_arguments = command.verbatim_arguments;

    process_ = std::make_unique<Process>();
    process_->spawn(command.executable, command.args, proc_opts);
    spawned_cli_path_ = command.display;
    notify_start_phase(detail::StartPhase::RuntimeSpawned);
    attempt.publish(process_.get());
    attempt.throw_if_cancelled();

    if (!use_stdio)
    {
        std::regex port_regex(R"(listening on port (\d+))", std::regex::icase);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        auto& output = process_->stdout_pipe();
        std::string line;
        while (true)
        {
            attempt.throw_if_cancelled();
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Timeout waiting for CLI port announcement");

            // Poll so the overall deadline holds even while the runtime prints nothing.
            bool readable = false;
            try
            {
                readable = output.has_data(50);
            }
            catch (const ProcessError&)
            {
                readable = true; // let read() report the closed pipe
            }
            if (!readable)
            {
                if (!process_->is_running() && !output.has_data(0))
                {
                    attempt.throw_if_cancelled();
                    throw std::runtime_error("CLI process exited unexpectedly");
                }
                continue;
            }

            char ch = 0;
            std::size_t read = 0;
            try
            {
                read = output.read(&ch, 1);
            }
            catch (const ProcessError&)
            {
                read = 0;
            }
            if (read == 0)
            {
                attempt.throw_if_cancelled();
                throw std::runtime_error("CLI process exited unexpectedly");
            }
            if (ch != '\n')
            {
                if (line.size() < 4096)
                    line.push_back(ch);
                continue;
            }

            std::smatch match;
            if (std::regex_search(line, match, port_regex))
            {
                tcp_host_ = "localhost";
                tcp_port_ = std::stoi(match[1].str());
                break;
            }
            line.clear();
        }
    }
}

void Client::connect_to_server(StartAttempt* attempt)
{
    const auto kind = runtime_.kind;
    if (transport_)
    {
        // Pre-created transport (in-process FFI).
    }
    else if (kind == RuntimeConnectionKind::ParentProcess)
    {
#ifdef _WIN32
        transport_ = std::make_unique<StdioTransport>(
            GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), false
        );
#else
        transport_ = std::make_unique<StdioTransport>(STDIN_FILENO, STDOUT_FILENO, false);
#endif
    }
    else if (kind == RuntimeConnectionKind::SpawnedStdio && process_)
    {
        transport_ =
            std::make_unique<PipeTransport>(process_->stdin_pipe(), process_->stdout_pipe());
    }
    else if (kind == RuntimeConnectionKind::Tcp || kind == RuntimeConnectionKind::SpawnedTcp)
    {
        auto tcp_transport = std::make_unique<TcpTransport>();
        tcp_transport->connect(
            tcp_host_, tcp_port_, 30000, attempt ? attempt->cancellation.token() : CancellationToken{}
        );
        if (attempt)
            attempt->throw_if_cancelled();
        transport_ = std::move(tcp_transport);
    }
    else
    {
        throw std::runtime_error("No transport available - check configuration");
    }

    // Owned by rpc_, which outlives every request it dispatches.
    auto* ffi_host = dynamic_cast<FfiTransport*>(transport_.get());
    rpc_ = std::make_shared<JsonRpcClient>(std::move(transport_));
    if (options_.request_timeout)
        rpc_->set_default_timeout(*options_.request_timeout);

    if (options_.models.request_handler)
    {
        request_handler_bridge_ = std::make_shared<CopilotRequestHandlerBridge>(
            options_.models.request_handler,
            [rpc = rpc_](const std::string& method, const json& params)
            { rpc->invoke(method, params).get(); }
        );
    }

    rpc_->set_on_closed(
        [this,
         source = rpc_.get(),
         bridge = std::weak_ptr<CopilotRequestHandlerBridge>(request_handler_bridge_)]
        {
            if (auto value = bridge.lock())
                value->cancel_all();
            handle_disconnect(source);
        }
    );

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
            else if (method == "gitHubTelemetry.event" && options_.github.on_telemetry)
            {
                (*options_.github.on_telemetry)(params);
            }
            else if (method == "host.authenticationCancelled" || method == "host.sessionReleased" ||
                     method == "host.exited")
            {
                handle_host_notification(method, params);
            }
        }
    );

    // NODEJS REFERENCE: client.ts attachConnectionHandlers
    rpc_->set_request_handler(
        [this,
         ffi_host,
         bridge = std::weak_ptr<CopilotRequestHandlerBridge>(request_handler_bridge_)](
            const std::string& method, const json& params, const CancellationToken& cancellation
        ) -> json
        {
            if (cancellation.is_cancelled())
                throw JsonRpcError(JsonRpcErrorCode::RequestCancelled, "Request cancelled");
            // C# REFERENCE: InstallationConfirmation.cs / Client.cs ClientRpcHandler.
            if (method == "installations.confirm" && options_.installations.confirmation_handler)
            {
                const auto confirmation_id = params.at("confirmationId");
                const auto fingerprint = params.at("reviewFingerprint");
                const auto decision =
                    (*options_.installations.confirmation_handler)(params, cancellation);
                if (cancellation.is_cancelled())
                    throw JsonRpcError(static_cast<JsonRpcErrorCode>(-32800), "Request cancelled");
                const char* value = nullptr;
                switch (decision)
                {
                case InstallationDecision::Confirm:
                    value = "confirm";
                    break;
                case InstallationDecision::Decline:
                    value = "decline";
                    break;
                case InstallationDecision::Cancel:
                    value = "cancel";
                    break;
                default:
                    throw std::invalid_argument("Invalid installation confirmation decision");
                }
                return {
                    {"confirmationId", confirmation_id},
                    {"reviewFingerprint", fingerprint},
                    {"decision", value}
                };
            }
            if (method == "extensionLaunchProvider.resolve" && options_.extensions.launch_provider)
                return (*options_.extensions.launch_provider)(params, cancellation);
            // NODEJS REFERENCE: entra.ts registerEntraTokenProvider. Provider failures are
            // reported generically so host error details never reach the runtime.
            if (method == copilot::rpc::methods::kEntraTokenGetToken && options_.entra &&
                options_.entra->token_provider)
            {
                const auto cancelled = []
                {
                    return JsonRpcError(
                        JsonRpcErrorCode::RequestCancelled, "Entra token acquisition cancelled"
                    );
                };
                json result;
                try
                {
                    result = options_.entra->token_provider(
                        params.value("request", json::object()), cancellation
                    );
                }
                catch (...)
                {
                    if (cancellation.is_cancelled())
                        throw cancelled();
                    throw JsonRpcError(JsonRpcErrorCode::InternalError, "Entra token provider failed");
                }
                if (cancellation.is_cancelled())
                    throw cancelled();
                if (result.is_null())
                    throw JsonRpcError(JsonRpcErrorCode::InternalError, "Entra token provider failed");
                return result;
            }
            // NODEJS REFERENCE: client.ts resolveClientProxy / authenticateClient*
            if (options_.network && method.rfind("network.", 0) == 0)
            {
                const auto& network = *options_.network;
                const auto run = [&](const char* label, auto&& callback)
                {
                    if (cancellation.is_cancelled())
                        throw JsonRpcError(
                            JsonRpcErrorCode::RequestCancelled,
                            std::string(label) + " request cancelled"
                        );
                    auto value = callback(params, cancellation);
                    if (cancellation.is_cancelled())
                        throw JsonRpcError(
                            JsonRpcErrorCode::RequestCancelled,
                            std::string(label) + " request cancelled"
                        );
                    return value;
                };
                if (method == copilot::rpc::methods::kNetworkResolveProxy && network.resolve_proxy)
                {
                    json result = json::object();
                    if (auto proxy = run("Proxy resolution", network.resolve_proxy))
                        result["proxy"] = *proxy;
                    return result;
                }
                if (method == copilot::rpc::methods::kNetworkAuthenticateNegotiate &&
                    network.authenticate_negotiate)
                {
                    json result = json::object();
                    if (auto token = run("Negotiate authentication", network.authenticate_negotiate))
                        result["token"] = *token;
                    return result;
                }
                if (method == copilot::rpc::methods::kNetworkAuthenticateBasic &&
                    network.authenticate_basic)
                {
                    json result = json::object();
                    if (auto credentials = run("Basic authentication", network.authenticate_basic))
                    {
                        result["username"] = credentials->username;
                        if (credentials->password)
                            result["password"] = *credentials->password;
                    }
                    return result;
                }
            }
            if (method == "skillProvider.list" || method == "skillProvider.read")
            {
                const auto session =
                    resolve_skill_provider_session(params.at("sessionId").get<std::string>());
                if (!session)
                    throw std::invalid_argument("Unknown skill-provider session");
                return session->handle_skill_provider_request(method, params, cancellation);
            }
            // NODEJS REFERENCE: client.ts onRequest("hostPolicy.preCheck")
            if (method == copilot::rpc::methods::kHostPolicyPreCheck)
            {
                const auto session = resolve_host_policy_pre_check_session(params);
                if (!session)
                    throw JsonRpcError(
                        JsonRpcErrorCode::InvalidRequest, "Host policy pre-check owner is unavailable"
                    );
                return session->handle_host_policy_pre_check(params, cancellation);
            }
            if (method == "userInput.request")
                return handle_user_input_request(params);
            else if (method == "exitPlanMode.request")
                return handle_exit_plan_mode_request(params);
            else if (method == "autoModeSwitch.request")
                return handle_auto_mode_switch_request(params);
            else if (method == "hooks.invoke")
                return handle_hooks_invoke(params);
            else if (method == copilot::rpc::methods::kGitHubTokenGetToken)
                return handle_github_token_request(params);
            else if (method == "providerToken.getToken")
                return handle_provider_token_request(params);
            else if (method == "shellAuthentication.request")
                return handle_shell_authentication_request(params);
            else if (method == "shellAuthentication.cancel")
            {
                cancel_shell_authentication_request(params);
                return json::object();
            }
            else if (method == "memoryStore.store")
                return handle_memory_store_request(params, cancellation);
            // NODEJS REFERENCE: client.ts onRequest("sessionManagement.create")
            else if (method == "sessionManagement.create")
            {
                const auto session = resolve_session_callback(params.value("sessionId", std::string{}));
                if (!session)
                    throw std::runtime_error("Session creation owner is unavailable");
                return session->handle_session_management_create(params, cancellation);
            }
            // NODEJS REFERENCE: client.ts handleSystemMessageTransform
            else if (method == "systemMessage.transform")
            {
                if (!params.is_object() || !params.contains("sessionId") ||
                    !params["sessionId"].is_string() || !params.contains("sections") ||
                    !params["sections"].is_object())
                    throw JsonRpcError(
                        JsonRpcErrorCode::InvalidParams, "Invalid systemMessage.transform payload"
                    );
                const auto session_id = params["sessionId"].get<std::string>();
                const auto session = resolve_session_callback(session_id);
                if (!session)
                    throw JsonRpcError(
                        JsonRpcErrorCode::InvalidParams, "Session not found: " + session_id
                    );
                return session->handle_system_message_transform(params["sections"]);
            }
            else if (method == "canvas.open" || method == "canvas.close" ||
                     method == "canvas.action.invoke")
            {
                const auto session = resolve_session_callback(params.value("sessionId", ""));
                if (!session)
                    throw JsonRpcError(JsonRpcErrorCode::InvalidParams, "Unknown canvas session");
                return session->handle_canvas_request(method, params);
            }
            else if (method == "workflow.execute" || method == "workflow.abort")
            {
                const auto session = resolve_session_callback(params.value("sessionId", ""));
                if (!session)
                    throw JsonRpcError(JsonRpcErrorCode::InvalidParams, "Unknown workflow session");
                return session->handle_workflow_request(method, params);
            }
            else if (method == "llmInference.httpRequestStart" ||
                     method == "llmInference.httpRequestChunk")
            {
                if (auto value = bridge.lock())
                    return value->handle(method, params);
                throw JsonRpcError(JsonRpcErrorCode::ConnectionClosed, "Request bridge closed");
            }
            // NODEJS REFERENCE: client.ts setupClientGlobalHandlers uiThread.run
            else if (method == copilot::rpc::methods::kUiThreadRun)
            {
                if (runtime_.kind != RuntimeConnectionKind::InProcess ||
                    !runtime_.run_on_ui_thread || !ffi_host)
                    throw std::runtime_error("No in-process runOnUiThread handler is configured.");
                const auto& id = params.at("actionId");
                if (!id.is_number_integer() || id.get<std::int64_t>() < 0 ||
                    id.get<std::int64_t>() > 0xffffffffLL)
                    throw std::out_of_range("UI action ID must be an unsigned 32-bit integer.");
                const auto action_id = static_cast<std::uint32_t>(id.get<std::int64_t>());
                runtime_.run_on_ui_thread([ffi_host, action_id] { ffi_host->run_ui_action(action_id); });
                return nullptr;
            }
            // NODEJS REFERENCE: generated/rpc.ts registerClientSessionApiHandlers (tasks.cancel).
            // Upstream SDKs never install a tasks handler, so this always reports it missing.
            else if (method == copilot::rpc::methods::kTasksCancel)
            {
                const auto session_id = params.value("sessionId", std::string{});
                if (!resolve_session_callback(session_id))
                    throw std::runtime_error("No session found for sessionId: " + session_id);
                throw std::runtime_error("No tasks handler registered for session: " + session_id);
            }
            // NODEJS REFERENCE: client.ts attachConnectionHandlers host.* handlers
            else if (method == "host.materializeSession")
            {
                return materialize_host_session(params);
            }
            else if (method == "host.validateAuthentication")
            {
                return validate_host_authentication(params, cancellation);
            }
            else if (method == "host.authenticationCancelled" || method == "host.sessionReleased" ||
                     method == "host.exited")
            {
                handle_host_notification(method, params);
                return json::object();
            }
            else if (detail::is_session_fs_request_method(method))
            {
                // NODEJS REFERENCE: generated/rpc.ts registerClientSessionApiHandlers (sessionFs)
                const auto session_id = params.value("sessionId", std::string{});
                auto session = resolve_session_callback(session_id);
                if (!session)
                    throw std::runtime_error("No session found for sessionId: " + session_id);
                auto handler = session->session_fs_handler();
                if (!handler)
                    throw std::runtime_error(
                        "No sessionFs handler registered for session: " + session_id
                    );
                return handler(method, params);
            }
            throw JsonRpcError(JsonRpcErrorCode::MethodNotFound, "Unknown method: " + method);
        }
    );

    if (attempt)
        attempt->publish(rpc_);
    rpc_->start();
    if (process_)
        process_->on_exit([rpc = std::weak_ptr<JsonRpcClient>(rpc_)]
                          {
                              if (auto value = rpc.lock())
                                  value->signal_connection_closed();
                          });
    // NODEJS REFERENCE: client.ts connectViaFfi configureUiThread
    if (ffi_host)
        ffi_host->configure_ui_thread(
            kind == RuntimeConnectionKind::InProcess && static_cast<bool>(runtime_.run_on_ui_thread)
        );
}

// NODEJS REFERENCE: client.ts verifyProtocolVersion
void Client::verify_protocol_version()
{
    json response;
    try
    {
        json params = json::object();
        if (effective_token_)
            params["token"] = *effective_token_;
        if (options_.github.on_telemetry)
            params["enableGitHubTelemetryForwarding"] = true;
        if (options_.client_info)
        {
            // Omitted entirely when every field is empty, so the runtime keeps its
            // own attribution rather than seeing a blank identity.
            if (auto wire = options_.client_info->to_wire_json())
                params["clientInfo"] = std::move(*wire);
        }
        if (options_.entra)
        {
            json entra{{"clientId", options_.entra->client_id}};
            if (options_.entra->mats_app_name)
                entra["matsAppName"] = *options_.entra->mats_app_name;
            if (options_.entra->one_auth_application_id)
                entra["oneAuthApplicationId"] = *options_.entra->one_auth_application_id;
            params["entra"] = std::move(entra);
            if (options_.entra->token_provider)
                params["entraTokenProvider"] = true;
        }
        params["supportedTaskKinds"] = json::array({"agent", "client", "shell"});
        response = rpc_->invoke(copilot::rpc::methods::kConnect, params).get();
        if (options_.entra && options_.entra->token_provider &&
            response.value("entraTokenProvider", json()) != json(true))
            throw std::runtime_error(
                "This runtime does not support host-owned Entra token providers."
            );
    }
    catch (const JsonRpcError& error)
    {
        if (error.code() != JsonRpcErrorCode::MethodNotFound &&
            std::string(error.what()) != "Unhandled method connect")
            throw;
        // A legacy runtime without `connect` cannot carry the Entra registration.
        if (options_.entra)
            throw std::runtime_error("This runtime does not support Entra client configuration.");
        response = rpc_->invoke(copilot::rpc::methods::kPing, json{{"message", nullptr}}).get();
    }

    std::optional<std::string> cli_version;
    const auto read_cli_version = [&cli_version](const json& result)
    {
        if (!cli_version && result.contains("version") && result["version"].is_string())
            cli_version = result["version"].get<std::string>();
    };
    read_cli_version(response);
    if (!response.contains("protocolVersion") || response["protocolVersion"].is_null())
    {
        response = rpc_->invoke(copilot::rpc::methods::kPing, json{{"message", nullptr}}).get();
        read_cli_version(response);
    }

    const auto cli_path =
        process_ ? spawned_cli_path_ : std::optional<std::string>{};

    if (!response.contains("protocolVersion") || response["protocolVersion"].is_null())
        throw ProtocolVersionMismatch(std::nullopt, cli_version, cli_path);

    int server_version = response["protocolVersion"].get<int>();
    if (server_version < kMinProtocolVersion || server_version > kSdkProtocolVersion)
        throw ProtocolVersionMismatch(server_version, cli_version, cli_path);

    {
        std::lock_guard<std::mutex> lock(protocol_version_mutex_);
        negotiated_protocol_version_ = server_version;
        if (cli_version)
            server_version_ = cli_version;
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

std::future<std::shared_ptr<Session>> Client::create_session(CreateSessionOptions options)
{
    return std::async(
        std::launch::async,
        [this, options = std::move(options)]() mutable
        { return create_session_impl(std::move(options)); }
    );
}

std::future<std::shared_ptr<Session>>
Client::resume_session(const std::string& session_id, ResumeSessionOptions options)
{
    return resume_session(session_id, std::move(options), std::nullopt);
}

std::future<std::shared_ptr<Session>> Client::resume_session(
    const std::string& session_id,
    ResumeSessionOptions options,
    std::optional<bool> permission_request
)
{
    return std::async(
        std::launch::async,
        [this, session_id, options = std::move(options), permission_request]() mutable
        { return resume_session_impl(session_id, std::move(options), permission_request); }
    );
}

// NODEJS REFERENCE: client.ts setupSessionFs
// C# REFERENCE: Client.cs ConfigureSessionFsHandlers
void Client::setup_session_fs(Session& session, const SessionStateOptions& state) const
{
    if (!options_.session_fs)
        return;
    if (!state.create_session_fs_provider)
        throw std::invalid_argument(
            "createSessionFsProvider is required in session config when sessionFs is enabled in "
            "client options."
        );
    auto provider = state.create_session_fs_provider(session);
    if (!provider)
        throw std::runtime_error("CreateSessionFsProvider returned null.");
    const auto declares = [this](std::optional<bool> SessionFsCapabilities::* field)
    {
        const auto& capabilities = options_.session_fs->capabilities;
        return capabilities && ((*capabilities).*field).value_or(false);
    };
    if (declares(&SessionFsCapabilities::sqlite) && !provider->sqlite())
        throw std::invalid_argument(
            "SessionFsConfig declares capabilities.sqlite but the provider does not implement sqlite."
        );
    if (declares(&SessionFsCapabilities::binary) && !provider->supports_binary())
        throw std::invalid_argument(
            "SessionFsConfig declares capabilities.binary but the provider does not implement "
            "readFileBytes and writeFileBytes."
        );
    if (declares(&SessionFsCapabilities::read_file_range) && !provider->supports_read_file_range())
        throw std::invalid_argument(
            "SessionFsConfig declares capabilities.readFileRange but the provider does not "
            "implement readFileRange."
        );
    session.register_session_fs_handler(create_session_fs_adapter(std::move(provider)));
}

void Client::setup_session_fs_or_retire(Session& session, const SessionStateOptions& state) const
{
    try
    {
        setup_session_fs(session, state);
    }
    catch (...)
    {
        session.retire();
        throw;
    }
}

// NODEJS REFERENCE: client.ts createSession
std::shared_ptr<Session> Client::create_session_impl(CreateSessionOptions options)
{
    ensure_connected();
    const auto lifetime = session_initialization_token();
    if (lifetime.is_cancelled())
        throw std::runtime_error(kSessionInitializationCancelled);

    std::shared_ptr<JsonRpcClient> connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_admission_closed_ || !rpc_ || state_ != ConnectionState::Connected)
            throw std::runtime_error("Session admission is closed");
        connection = rpc_;
    }

    std::optional<std::string> registration_id;
    if (options.github.token_provider)
        registration_id = register_github_token_provider(options.github.token_provider);
    ScopeExit drop_registration{[&]
                                {
                                    if (registration_id)
                                        remove_github_token_registration(*registration_id);
                                }};

    json request = build_session_create_request(
        options,
        options_.mode,
        static_cast<bool>(options_.github.on_telemetry),
        registration_id
    );

    const bool cloud = options.cloud.has_value();
    std::string session_id =
        cloud && !options.session_id ? std::string() : options.session_id.value_or(generate_uuid_v4());
    if (!session_id.empty())
        request["sessionId"] = session_id;
    // C# REFERENCE: Host.cs CaptureAhpSession (snapshot excludes trace context)
    std::optional<json> ahp_snapshot;
    {
        std::lock_guard<std::mutex> lock(ahp_mutex_);
        if (!ahp_handoffs_.empty())
            ahp_snapshot = request;
    }
    request.update(trace_context());

    auto session = std::make_shared<Session>(
        session_id, this, std::nullopt, SessionCapabilities{}, managed_settings_enabled(options)
    );
    register_initial_handlers(*session, options);
    // Session-FS setup can throw, so finish it before the session becomes routable;
    // a failed open must not leave handlers behind.
    setup_session_fs_or_retire(*session, options.state);
    if (registration_id)
        session->set_on_disconnected([this, id = *registration_id]
                                     { remove_github_token_registration(id); });

    std::shared_ptr<Session> replaced;
    std::optional<std::string> returned_id;
    ScopeExit rollback{[&]
                       {
                           session->retire();
                           {
                               std::lock_guard<std::mutex> lock(mutex_);
                               const auto it = sessions_.find(session_id);
                               if (it != sessions_.end() && it->second == session)
                               {
                                   if (replaced && !replaced->is_disconnected())
                                       it->second = replaced;
                                   else
                                       sessions_.erase(it);
                               }
                           }
                           if (cloud && returned_id)
                           {
                               try
                               {
                                   connection
                                       ->invoke(
                                           copilot::rpc::methods::kSessionDelete,
                                           {{"sessionId", *returned_id}},
                                           kCleanupTimeout
                                       )
                                       .get();
                               }
                               catch (...)
                               {
                               }
                           }
                       }};

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_admission_closed_ || rpc_ != connection)
            throw std::runtime_error("Session admission is closed");
        if (!session_id.empty())
        {
            const auto it = sessions_.find(session_id);
            if (it != sessions_.end())
                replaced = it->second;
            sessions_[session_id] = session;
        }
    }
    deliver_held_session_events();

    const json response = await_session_initialization(
        connection->invoke(copilot::rpc::methods::kSessionCreate, request), lifetime
    );
    returned_id = string_field(response, "sessionId");
    if (!returned_id || returned_id->empty())
    {
        returned_id.reset();
        throw std::runtime_error("session.create response did not include a sessionId");
    }
    if (!session_id.empty() && *returned_id != session_id)
        throw std::runtime_error(
            "session.create returned sessionId " + *returned_id + " but the caller requested " +
            session_id
        );
    session->mark_confirmed();
    if (session_id.empty())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_admission_closed_ || rpc_ != connection)
            throw std::runtime_error("Session connection changed during creation");
        session_id = *returned_id;
        session->set_session_id(session_id);
        sessions_[session_id] = session;
    }
    // NODEJS REFERENCE: client.ts routeSession (cloud sessions learn their ID here)
    deliver_held_session_events();
    if (registration_id)
        assign_github_token_registration(*registration_id, session_id);

    auto capabilities = capabilities_of(response);
    // NODEJS REFERENCE: client.ts requireAutoModeRecovery
    const bool auto_mode_unsupported = options.interaction.auto_mode_recovery.value_or(false) &&
                                       !capabilities.has("ui.autoModeRecovery");
    session->set_initial_state(string_field(response, "workspacePath"), std::move(capabilities));
    // NODEJS REFERENCE: client.ts createSession (setPermissionModeResult)
    if (response.contains("permissionModeResult") && response["permissionModeResult"].is_object())
        session->set_permission_mode_result(
            generated::api::PermissionsSetModeResult(response["permissionModeResult"])
        );
    configure_host_policy_pre_check(*session, options.permissions.host_policy_pre_check);
    if (auto_mode_unsupported)
    {
        detach_quietly(connection, session_id);
        throw std::runtime_error("The runtime does not support autoModeRecovery");
    }

    if (has_mcp_auth_handler(options))
        register_mcp_oauth_interest(connection, session_id);
    apply_options_update(*session, options, options_.mode);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_admission_closed_ || rpc_ != connection)
            throw std::runtime_error("Session connection changed during creation");
    }
    if (registration_id)
        commit_github_token_registration(*registration_id);
    drop_registration.action = {};
    rollback.action = {};
    if (ahp_snapshot)
        capture_host_session(
            session, std::move(*ahp_snapshot), build_session_options_update(options, options_.mode)
        );
    return session;
}

// NODEJS REFERENCE: client.ts resolveSessionIdPrefix
std::string
Client::resolve_session_id_prefix(const std::string& session_id, const ResumeSessionOptions& options)
{
    if (!options.interaction.auto_mode_recovery.value_or(false) || options_.session_fs ||
        !is_abbreviated_uuid_prefix(session_id))
        return session_id;

    json params{{"prefix", session_id}};
    if (options.config_directory)
        params["configDir"] = *options.config_directory;
    const json response = rpc_client_shared()->invoke("sessions.findByPrefix", params).get();
    const auto resolved = string_field(response, "sessionId");
    if (!resolved || resolved->empty())
        throw std::runtime_error("No unique session matches prefix " + session_id);
    if (!starts_with_icase(*resolved, session_id))
        throw std::runtime_error("Runtime returned an unrelated session ID for prefix " + session_id);
    return *resolved;
}

// NODEJS REFERENCE: client.ts resumeSession
std::shared_ptr<Session> Client::resume_session_impl(
    std::string session_id, ResumeSessionOptions options, std::optional<bool> permission_request
)
{
    ensure_connected();
    const auto lifetime = session_initialization_token();
    if (lifetime.is_cancelled())
        throw std::runtime_error(kSessionInitializationCancelled);
    session_id = resolve_session_id_prefix(session_id, options);

    std::shared_ptr<JsonRpcClient> connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_admission_closed_ || !rpc_ || state_ != ConnectionState::Connected)
            throw std::runtime_error("Session admission is closed");
        connection = rpc_;
    }

    auto session = std::make_shared<Session>(
        session_id, this, std::nullopt, SessionCapabilities{}, managed_settings_enabled(options)
    );
    register_initial_handlers(*session, options);
    session->register_workflows(options.workflow_objects);
    setup_session_fs_or_retire(*session, options.state);

    std::optional<std::string> registration_id;
    if (options.github.token_provider)
    {
        registration_id = register_github_token_provider(options.github.token_provider);
        assign_github_token_registration(*registration_id, session_id);
        session->set_on_disconnected([this, id = *registration_id]
                                     { remove_github_token_registration(id); });
    }

    std::shared_ptr<Session> replaced;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_admission_closed_ || rpc_ != connection)
        {
            session->retire();
            if (registration_id)
                remove_github_token_registration(*registration_id);
            throw std::runtime_error("Session connection changed during resume");
        }
        const auto it = sessions_.find(session_id);
        if (it != sessions_.end())
            replaced = it->second;
        sessions_[session_id] = session;
        pending_resumes_[session_id] = PendingResume{
            .session = session,
            .connection = connection,
            .registration_id = registration_id,
            .resolve_aliases =
                options.interaction.auto_mode_recovery.value_or(false) && !options_.session_fs,
            .skill_provider_session = options.skills.provider ? session : replaced,
            .host_policy_pre_check_session = replaced,
            .host_policy_pre_check_revision =
                options.permissions.host_policy_pre_check
                    ? std::optional<std::int64_t>(options.permissions.host_policy_pre_check->policy_revision)
                    : std::nullopt,
        };
    }
    // NODEJS REFERENCE: client.ts resumeSession deliverHeldSessionEvents (before the RPC)
    deliver_held_session_events();
    const std::string requested_id = session_id;
    ScopeExit drop_pending{[&]
                           {
                               std::lock_guard<std::mutex> lock(mutex_);
                               const auto it = pending_resumes_.find(requested_id);
                               if (it != pending_resumes_.end() && it->second.session == session)
                                   pending_resumes_.erase(it);
                           }};

    // A failed resume of a session this client already owns keeps the resident owner.
    bool keep_session = false;
    std::string active_id = session_id;
    try
    {
        json request = build_session_resume_request(
            session_id,
            options,
            options_.mode,
            static_cast<bool>(options_.github.on_telemetry),
            registration_id,
            permission_request
        );
        std::optional<json> ahp_snapshot;
        {
            std::lock_guard<std::mutex> lock(ahp_mutex_);
            if (!ahp_handoffs_.empty())
                ahp_snapshot = request;
        }
        request.update(trace_context());
        const json response = await_session_initialization(
            connection->invoke(copilot::rpc::methods::kSessionResume, request), lifetime
        );
        session->mark_confirmed();

        const auto returned_id = string_field(response, "sessionId");
        if (returned_id && !returned_id->empty() && *returned_id != session_id)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_admission_closed_ || rpc_ != connection)
                throw std::runtime_error("Session connection changed during resume");
            const auto it = sessions_.find(session_id);
            if (it != sessions_.end() && it->second == session)
            {
                if (replaced && !replaced->is_disconnected())
                    it->second = replaced;
                else
                    sessions_.erase(it);
            }
            const auto existing = sessions_.find(*returned_id);
            replaced = existing != sessions_.end() ? existing->second : nullptr;
            active_id = *returned_id;
            session->set_session_id(active_id);
            sessions_[active_id] = session;
            if (registration_id)
                assign_github_token_registration(*registration_id, active_id);
        }
        if (active_id != session_id)
            deliver_held_session_events();

        apply_granted_environment(response, options.requested_environment_variables);

        auto capabilities = capabilities_of(response);
        const bool auto_mode_unsupported =
            options.interaction.auto_mode_recovery.value_or(false) &&
            !capabilities.has("ui.autoModeRecovery");
        session->set_initial_state(string_field(response, "workspacePath"), std::move(capabilities));
        // NODEJS REFERENCE: client.ts resumeSession (setOpenCanvases / setPermissionModeResult)
        {
            std::vector<generated::api::OpenCanvasInstance> open_canvases;
            if (response.contains("openCanvases") && response["openCanvases"].is_array())
                for (const auto& instance : response["openCanvases"])
                    open_canvases.emplace_back(instance);
            session->set_open_canvases(std::move(open_canvases));
            std::optional<generated::api::PermissionsSetModeResult> permission_mode_result;
            if (response.contains("permissionModeResult") &&
                response["permissionModeResult"].is_object())
                permission_mode_result =
                    generated::api::PermissionsSetModeResult(response["permissionModeResult"]);
            session->set_permission_mode_result(std::move(permission_mode_result));
        }
        configure_host_policy_pre_check(*session, options.permissions.host_policy_pre_check);
        if (auto_mode_unsupported)
        {
            // NODEJS REFERENCE: client.ts requireAutoModeRecovery(detachOnFailure = !replaced)
            keep_session = static_cast<bool>(replaced);
            if (!replaced)
                detach_quietly(connection, active_id);
            throw std::runtime_error("The runtime does not support autoModeRecovery");
        }

        if (response.contains("transcriptRecovery") && !response["transcriptRecovery"].is_null())
            session->set_transcript_recovery(
                response["transcriptRecovery"].get<TranscriptRecoveryReport>()
            );

        if (has_mcp_auth_handler(options))
            register_mcp_oauth_interest(connection, active_id);
        apply_options_update(*session, options, options_.mode);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_admission_closed_ || rpc_ != connection)
                throw std::runtime_error("Session connection changed during resume");
        }
        if (registration_id)
            commit_github_token_registration(*registration_id);
        if (ahp_snapshot)
        {
            // NODEJS REFERENCE: client.ts resumeSession ({ ...config, sessionId })
            (*ahp_snapshot)["sessionId"] = active_id;
            capture_host_session(
                session,
                std::move(*ahp_snapshot),
                build_session_options_update(options, options_.mode)
            );
        }
        return session;
    }
    catch (...)
    {
        session->retire();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = sessions_.find(active_id);
            if (it != sessions_.end() && it->second == session)
            {
                if (replaced && !replaced->is_disconnected())
                    it->second = replaced;
                else
                    sessions_.erase(it);
            }
        }
        if (registration_id)
        {
            if (keep_session)
                commit_github_token_registration(*registration_id);
            else
                remove_github_token_registration(*registration_id);
        }
        throw;
    }
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
            const auto rpc = ensure_connected();

            // session.list takes an optional 'filter' param (matches nodejs SDK shape).
            json params = json::object();
            json filter_json = filter; // uses to_json(SessionListFilter)
            if (!filter_json.empty())
                params["filter"] = std::move(filter_json);

            auto response = rpc->invoke(copilot::rpc::methods::kSessionList, params).get();
            std::vector<SessionMetadata> sessions;

            if (response.contains("sessions") && response["sessions"].is_array())
                for (const auto& item : response["sessions"])
                    sessions.push_back(item.get<SessionMetadata>());

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
            const auto rpc = ensure_connected();

            auto response =
                rpc->invoke(
                        copilot::rpc::methods::kSessionGetMetadata, json{{"sessionId", session_id}}
                ).get();

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
            const auto rpc = ensure_connected();

            auto response =
                rpc->invoke(
                        copilot::rpc::methods::kSessionDelete, json{{"sessionId", session_id}}
                ).get();

            if (response.is_object() && response.contains("success") &&
                response["success"].is_boolean() && !response["success"].get<bool>())
            {
                std::string error = response.contains("error") && response["error"].is_string()
                                        ? response["error"].get<std::string>()
                                        : "Unknown error";
                throw std::runtime_error("Failed to delete session " + session_id + ": " + error);
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                sessions_.erase(session_id);
            }
            // The deleted conversation's events must not reach a new session created
            // with the same ID.
            discard_held_session_events(session_id);
        }
    );
}

std::future<void> Client::clear_managed_settings_cache()
{
    return std::async(
        std::launch::async,
        [this]()
        {
            const auto rpc = ensure_connected();

            rpc->invoke(copilot::rpc::methods::kManagedSettingsClearCache, json::object()).get();
        }
    );
}

std::future<std::optional<std::string>> Client::get_last_session_id()
{
    return std::async(
        std::launch::async,
        [this]() -> std::optional<std::string>
        {
            const auto rpc = ensure_connected();

            auto response =
                rpc->invoke(copilot::rpc::methods::kSessionGetLastId, json::object()).get();

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
            const auto rpc = ensure_connected();

            json params;
            if (message.has_value())
                params["message"] = *message;
            else
                params["message"] = nullptr;

            auto response = rpc->invoke(copilot::rpc::methods::kPing, params).get();

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
            const auto rpc = ensure_connected();

            // NODEJS REFERENCE: nodejs/src/client.ts getStatus (status.get). Runtimes that
            // do not implement status.get get the values negotiated at connect time.
            try
            {
                const auto response =
                    rpc->invoke(copilot::rpc::methods::kStatusGet, json::object()).get();
                if (response.is_object() && response.contains("version") &&
                    response.contains("protocolVersion"))
                    return response.get<GetStatusResponse>();
            }
            catch (...)
            {
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
            const auto rpc = ensure_connected();

            auto response = rpc->invoke("account.getCurrentAuth", json::object()).get();
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
            const ListModelsHandler handler_copy = options_.models.on_list_models;
            if (handler_copy)
            {
                auto models = handler_copy();
                std::lock_guard<std::mutex> lock(models_cache_mutex_);
                models_cache_ = models;
                return models;
            }

            // Default path: query the CLI server (requires a live connection).
            const auto rpc = ensure_connected();

            auto response = rpc->invoke(copilot::rpc::methods::kModelsList, json::object()).get();
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
            const auto rpc = ensure_connected();
            return rpc->invoke(method, params).get();
        }
    );
}

json Client::trace_context() const
{
    json result = json::object();
    if (!options_.observability.on_get_trace_context)
        return result;
    for (const auto& [key, value] : (*options_.observability.on_get_trace_context)())
        if (key == "traceparent" || key == "tracestate")
            result[key] = value;
    return result;
}

std::shared_ptr<Session> Client::get_session(const std::string& session_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

// NODEJS REFERENCE: client.ts resolveSessionCallback
std::shared_ptr<Session> Client::resolve_session_callback(const std::string& session_id)
{
    std::shared_ptr<Session> selected;
    std::optional<std::string> registration_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ptrdiff_t selected_prefix_length = -1;
        for (const auto& [prefix, pending] : pending_resumes_)
        {
            if (pending.connection != rpc_ || (!pending.resolve_aliases && prefix != session_id) ||
                static_cast<std::ptrdiff_t>(prefix.size()) <= selected_prefix_length ||
                !(session_ids_overlap(prefix, session_id) && session_id.size() >= prefix.size()))
                continue;
            std::set<const Session*> known_owners;
            for (const auto& [_, owner] : sessions_)
            {
                if (owner != pending.session && session_ids_overlap(prefix, owner->session_id()) &&
                    owner->session_id().size() >= prefix.size())
                    known_owners.insert(owner.get());
            }
            if (known_owners.size() > 1)
                continue;
            if (pending.session->session_id() != session_id)
                pending.session->set_session_id(session_id);
            registration_id = pending.registration_id;
            selected = pending.session;
            selected_prefix_length = static_cast<std::ptrdiff_t>(prefix.size());
        }
        if (!selected)
        {
            const auto it = sessions_.find(session_id);
            return it != sessions_.end() ? it->second : nullptr;
        }
    }
    if (registration_id)
        assign_github_token_registration(*registration_id, session_id);
    return selected;
}

// =============================================================================
// RPC Handlers
// =============================================================================

// NODEJS REFERENCE: client.ts resolveSkillProviderSession
std::shared_ptr<Session> Client::resolve_skill_provider_session(const std::string& session_id)
{
    const auto session = resolve_session_callback(session_id);
    if (!session)
        return nullptr;
    // An omitted provider creates no new binding, so pending provider requests still
    // belong to the resident until the resume settles.
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, pending] : pending_resumes_)
    {
        if (pending.connection == rpc_ && pending.session == session)
        {
            if (pending.skill_provider_session)
                return pending.skill_provider_session;
            const auto it = sessions_.find(session_id);
            return it != sessions_.end() ? it->second : session;
        }
    }
    return session;
}

// NODEJS REFERENCE: client.ts onRequest("hostPolicy.preCheck") pending-resume routing
std::shared_ptr<Session> Client::resolve_host_policy_pre_check_session(const json& params)
{
    auto session = resolve_session_callback(params.value("sessionId", std::string{}));
    if (!session)
        return nullptr;
    const auto revision = params.contains("policyRevision") && params["policyRevision"].is_number_integer()
                              ? std::optional<std::int64_t>(params["policyRevision"].get<std::int64_t>())
                              : std::nullopt;
    // Until a resume settles, a request for any revision other than the one the resume
    // declares belongs to the resident session.
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, pending] : pending_resumes_)
    {
        if (pending.connection == rpc_ && pending.session == session &&
            revision != pending.host_policy_pre_check_revision)
        {
            if (pending.host_policy_pre_check_session)
                session = pending.host_policy_pre_check_session;
        }
    }
    return session;
}

namespace
{
// NODEJS REFERENCE: client.ts MAX_HELD_SESSION_EVENTS
constexpr std::size_t kMaxHeldSessionEvents = 8192;

// NODEJS REFERENCE: client.ts SETTLED_BY (request event -> the event that settles it)
const char* settled_by(const std::string& type)
{
    if (type == "permission.requested")
        return "permission.completed";
    if (type == "external_tool.requested")
        return "external_tool.completed";
    if (type == "elicitation.requested")
        return "elicitation.completed";
    if (type == "mcp.oauth_required")
        return "mcp.oauth_completed";
    if (type == "command.execute")
        return "command.completed";
    return nullptr;
}
} // namespace

// NODEJS REFERENCE: client.ts handleSessionEventNotification
void Client::handle_session_event(const std::string& /*method*/, const json& params)
{
    if (!params.contains("sessionId") || !params["sessionId"].is_string() ||
        !params.contains("event"))
        return;

    const std::string session_id = params["sessionId"].get<std::string>();
    HeldSessionEvent held{.event = parse_session_event(params["event"])};
    const auto& event_json = params["event"];
    if (event_json.contains("data") && event_json["data"].is_object() &&
        event_json["data"].contains("requestId") && event_json["data"]["requestId"].is_string())
        held.request_id = event_json["data"]["requestId"].get<std::string>();

    const auto session = resolve_session_callback(session_id);
    bool dispatch_now = false;
    {
        // A session whose held events are still pending or being delivered receives its
        // live events behind them, so no live event overtakes a held one.
        std::lock_guard<std::mutex> lock(held_mutex_);
        const bool queued =
            delivering_session_ids_.contains(session_id) ||
            std::any_of(
                held_session_events_.begin(),
                held_session_events_.end(),
                [&](const auto& entry) { return entry.first == session_id; }
            );
        dispatch_now = session && !queued;
        if (!dispatch_now)
            hold_session_event(session_id, held);
    }
    if (dispatch_now)
    {
        session->dispatch_event(held.event);
        return;
    }
    // The session may have registered after the lookup above.
    if (resolve_session_callback(session_id))
        deliver_held_session_events();
}

// NODEJS REFERENCE: client.ts holdSessionEvent. Caller holds held_mutex_.
void Client::hold_session_event(const std::string& session_id, HeldSessionEvent held)
{
    while (held_session_event_count_ >= kMaxHeldSessionEvents)
    {
        // A session whose held events are being delivered takes its events; only
        // unregistered IDs give theirs up, oldest first.
        const auto victim = std::find_if(
            held_session_events_.begin(),
            held_session_events_.end(),
            [&](const auto& entry) { return !delivering_session_ids_.contains(entry.first); }
        );
        if (victim == held_session_events_.end())
            break;
        held_session_event_count_ -= victim->second.size();
        held_session_events_.erase(victim);
    }
    auto it = std::find_if(
        held_session_events_.begin(),
        held_session_events_.end(),
        [&](const auto& entry) { return entry.first == session_id; }
    );
    if (it == held_session_events_.end())
    {
        held_session_events_.emplace_back(session_id, std::vector<HeldSessionEvent>{});
        it = std::prev(held_session_events_.end());
    }
    it->second.push_back(std::move(held));
    ++held_session_event_count_;
}

// NODEJS REFERENCE: client.ts deliverHeldSessionEvents. A held request whose settling
// event was also held was answered while this client was not listening: observers see
// it, but no handler is asked to answer it again.
void Client::deliver_held_session_events()
{
    for (;;)
    {
        std::vector<std::string> candidates;
        {
            std::lock_guard<std::mutex> lock(held_mutex_);
            for (const auto& [id, _] : held_session_events_)
                if (!delivering_session_ids_.contains(id))
                    candidates.push_back(id);
        }
        std::string session_id;
        std::shared_ptr<Session> session;
        for (const auto& id : candidates)
        {
            if (auto resolved = resolve_session_callback(id))
            {
                session_id = id;
                session = std::move(resolved);
                break;
            }
        }
        if (!session)
            return;

        std::vector<HeldSessionEvent> batch;
        {
            std::lock_guard<std::mutex> lock(held_mutex_);
            if (delivering_session_ids_.contains(session_id))
                continue;
            const auto it = std::find_if(
                held_session_events_.begin(),
                held_session_events_.end(),
                [&](const auto& entry) { return entry.first == session_id; }
            );
            if (it == held_session_events_.end())
                continue;
            batch = std::move(it->second);
            held_session_event_count_ -= batch.size();
            held_session_events_.erase(it);
            delivering_session_ids_.insert(session_id);
        }
        while (!batch.empty())
        {
            std::set<std::pair<std::string, std::string>> held_keys;
            for (const auto& held : batch)
                if (held.request_id)
                    held_keys.emplace(held.event.type_string, *held.request_id);
            for (const auto& held : batch)
            {
                const char* settling = settled_by(held.event.type_string);
                const bool settled = settling && held.request_id &&
                                     held_keys.contains({settling, *held.request_id});
                session->dispatch_event(held.event, settled);
            }
            batch.clear();
            std::lock_guard<std::mutex> lock(held_mutex_);
            const auto it = std::find_if(
                held_session_events_.begin(),
                held_session_events_.end(),
                [&](const auto& entry) { return entry.first == session_id; }
            );
            if (it != held_session_events_.end())
            {
                batch = std::move(it->second);
                held_session_event_count_ -= batch.size();
                held_session_events_.erase(it);
            }
            else
            {
                delivering_session_ids_.erase(session_id);
            }
        }
    }
}

void Client::discard_held_session_events(const std::string& session_id)
{
    std::lock_guard<std::mutex> lock(held_mutex_);
    const auto it = std::find_if(
        held_session_events_.begin(),
        held_session_events_.end(),
        [&](const auto& entry) { return entry.first == session_id; }
    );
    if (it != held_session_events_.end())
    {
        held_session_event_count_ -= it->second.size();
        held_session_events_.erase(it);
    }
}

// NODEJS REFERENCE: client.ts handleGitHubTokenRequest
json Client::handle_github_token_request(const json& params)
{
    const auto registration_id = params.value("registrationId", "");
    GitHubTokenProvider provider;
    std::optional<std::string> registered_session;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        const auto it = github_token_registrations_.find(registration_id);
        if (it == github_token_registrations_.end())
            throw JsonRpcError(
                JsonRpcErrorCode::InvalidParams,
                "No GitHub token provider registered for registration ID \"" + registration_id +
                    "\""
            );
        provider = it->second.provider;
        registered_session = it->second.session_id;
    }

    GitHubTokenRequest request;
    request.host = params.value("host", "");
    request.session_id = string_field(params, "sessionId");
    if (!request.session_id)
        request.session_id = registered_session;
    request.reason = params.value("reason", "") == "refresh" ? GitHubTokenAcquireReason::Refresh
                                                             : GitHubTokenAcquireReason::Initial;

    // A declining or throwing provider must not surface as an RPC error: the official result
    // union models refusal as its own variant.
    try
    {
        if (auto token = provider ? provider(request) : std::nullopt)
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

// NODEJS REFERENCE: generated/rpc.ts providerToken.getToken
json Client::handle_provider_token_request(const json& params)
{
    const auto session_id = params.value("sessionId", "");
    const auto session = resolve_session_callback(session_id);
    if (!session)
        throw JsonRpcError(
            JsonRpcErrorCode::InvalidParams,
            "No providerToken handler registered for session: " + session_id
        );
    return session->handle_provider_token_request(params.value("providerName", ""));
}

// NODEJS REFERENCE: client.ts handleShellAuthenticationRequest
json Client::handle_shell_authentication_request(const json& params)
{
    const json cancel{{"action", "cancel"}};
    const auto session_id = string_field(params, "sessionId");
    const auto request_id = string_field(params, "requestId");
    const auto program = string_field(params, "program");
    const auto prompt = string_field(params, "prompt");
    const auto command = string_field(params, "command");
    const auto cwd = string_field(params, "cwd");
    const auto input_kind = string_field(params, "inputKind");
    if (!session_id || !request_id || !program || !prompt || !command || !cwd || !input_kind ||
        (*input_kind != "secret" && *input_kind != "text" && *input_kind != "confirm"))
        return cancel;

    const auto session = get_session(*session_id);
    if (!session)
        return cancel;

    CancellationSource source;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto& pending = pending_shell_auth_[*session_id];
        if (pending.contains(*request_id))
            return cancel;
        pending.emplace(*request_id, source);
    }
    ScopeExit cleanup{[&]
                      {
                          std::lock_guard<std::mutex> lock(registry_mutex_);
                          const auto it = pending_shell_auth_.find(*session_id);
                          if (it == pending_shell_auth_.end())
                              return;
                          it->second.erase(*request_id);
                          if (it->second.empty())
                              pending_shell_auth_.erase(it);
                      }};

    ShellAuthenticationRequest request{
        .request_id = *request_id,
        .program = *program,
        .prompt = *prompt,
        .input_kind = json(*input_kind).get<ShellAuthenticationInputKind>(),
        .command = *command,
        .cwd = *cwd,
    };
    return session->handle_shell_authentication_request(request, source.token());
}

void Client::cancel_shell_authentication_request(const json& params)
{
    const auto session_id = string_field(params, "sessionId");
    const auto request_id = string_field(params, "requestId");
    if (!session_id || !request_id)
        return;
    std::optional<CancellationSource> source;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        const auto it = pending_shell_auth_.find(*session_id);
        if (it == pending_shell_auth_.end())
            return;
        const auto found = it->second.find(*request_id);
        if (found == it->second.end())
            return;
        source = found->second;
    }
    source->cancel();
}

// NODEJS REFERENCE: client.ts handleMemoryStoreRequest
json Client::handle_memory_store_request(const json& params, const CancellationToken& cancellation)
{
    const auto session = get_session(params.value("sessionId", ""));
    if (!session)
        throw std::runtime_error("Memory store owner is unavailable");

    CancellationSource source;
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(registry_mutex_);
        id = next_memory_store_id_++;
        pending_memory_stores_.emplace(id, source);
    }
    const auto registration = cancellation.on_cancel([source]() mutable { source.cancel(); });
    ScopeExit cleanup{[&]
                      {
                          if (registration != 0)
                              cancellation.unregister(registration);
                          std::lock_guard<std::mutex> lock(registry_mutex_);
                          pending_memory_stores_.erase(id);
                      }};
    session->handle_memory_store_request(params, source.token());
    return nullptr;
}

json Client::handle_user_input_request(const json& params)
{
    std::string session_id = params["sessionId"].get<std::string>();

    auto session = resolve_session_callback(session_id);
    if (!session)
        throw JsonRpcError(JsonRpcErrorCode::InvalidParams, "Unknown session " + session_id);

    try
    {
        auto request = params.get<UserInputRequest>();

        auto result = session->handle_user_input_request(request);

        return json(result);
    }
    catch (const std::exception& e)
    {
        throw JsonRpcError(JsonRpcErrorCode::InternalError, e.what());
    }
}

json Client::handle_exit_plan_mode_request(const json& params)
{
    std::string session_id = params["sessionId"].get<std::string>();

    auto session = resolve_session_callback(session_id);
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

    auto session = resolve_session_callback(session_id);
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

    auto session = resolve_session_callback(session_id);
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
    return Subscription(
        [this, ptr]()
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            lifecycle_handlers_.erase(
                std::remove_if(
                    lifecycle_handlers_.begin(),
                    lifecycle_handlers_.end(),
                    [ptr](const LifecycleHandler& h) { return &h == ptr; }
                ),
                lifecycle_handlers_.end()
            );
        }
    );
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
            auto response =
                rpc_client_shared()
                    ->invoke(copilot::rpc::methods::kSessionGetForeground, json::object())
                    .get();
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
            // Protocol 4 runtimes reject with a JSON-RPC error; legacy runtimes report success:false.
            const json response =
                rpc_client_shared()
                    ->invoke(
                        copilot::rpc::methods::kSessionSetForeground, json{{"sessionId", session_id}}
                    )
                    .get();
            if (response.is_object() && response.contains("success") &&
                response["success"].is_boolean() && !response["success"].get<bool>())
            {
                std::string error = response.contains("error") && response["error"].is_string()
                                        ? response["error"].get<std::string>()
                                        : "Failed to set foreground session";
                throw std::runtime_error(error);
            }
        }
    );
}

// =============================================================================
// AHP hosting
// =============================================================================

namespace
{

std::string ahp_mode_name(AhpAuthenticationMode mode)
{
    return json(mode).get<std::string>();
}

bool is_blank(const std::string& value)
{
    return value.find_first_not_of(" \t\r\n\f\v") == std::string::npos;
}

void run_detached(std::function<void()> action)
{
    std::thread(
        [action = std::move(action)]()
        {
            try
            {
                action();
            }
            catch (...)
            {
            }
        }
    ).detach();
}

} // namespace

// NODEJS REFERENCE: client.ts startAhpHost
// C# REFERENCE: Host.cs StartAhpHostAsync
std::future<AhpHost> Client::start_ahp_host(AhpHostOptions options)
{
    return std::async(
        std::launch::async,
        [this, options = std::move(options)]() -> AhpHost
        {
            if (!options.authentication)
                throw std::invalid_argument("An explicit authentication mode is required");
            const auto& authentication = *options.authentication;
            if (authentication.mode == AhpAuthenticationMode::Custom &&
                (is_blank(authentication.require_identity) || !authentication.authenticate))
                throw std::invalid_argument(
                    "Custom authentication requires a nonempty requireIdentity and authenticate "
                    "callback"
                );
            if (authentication.mode != AhpAuthenticationMode::GitHub && options.github_environment)
                throw std::invalid_argument("githubEnvironment requires github authentication");
            if (!options.local_server && !options.github_environment)
                throw std::invalid_argument(
                    "At least one of localServer or githubEnvironment is required"
                );

            const auto rpc = ensure_connected();
            const std::string host_id = generate_uuid_v4();
            const bool custom = authentication.mode == AhpAuthenticationMode::Custom;
            {
                std::lock_guard<std::mutex> lock(ahp_mutex_);
                if (custom)
                    ahp_authenticators_[host_id] = AhpAuthenticatorEntry{
                        authentication.require_identity,
                        authentication.authenticate,
                        next_ahp_authenticator_generation_++,
                    };
                if (options.create_session || options.resume_session)
                    ahp_session_factories_[host_id] = AhpSessionFactories{
                        options.create_session,
                        options.resume_session,
                        options.on_session_released,
                        AhpHostPolicy{
                            authentication.mode, custom ? authentication.require_identity : ""
                        },
                    };
                if (options.on_exit)
                    ahp_exit_callbacks_[host_id] = options.on_exit;
            }

            try
            {
                json auth = {{"mode", ahp_mode_name(authentication.mode)}};
                if (custom)
                    auth["requireIdentity"] = authentication.require_identity;
                json params = {{"hostId", host_id}, {"authentication", std::move(auth)}};
                if (options.compute_id)
                    params["computeId"] = *options.compute_id;
                if (options.workspace)
                    params["workspace"] = *options.workspace;
                if (options.local_server)
                    params["localServer"] = *options.local_server;
                if (options.github_environment)
                    params["githubEnvironment"] = *options.github_environment;
                if (options.create_session)
                    params["sessionFactory"] = true;
                if (options.resume_session)
                    params["resumeFactory"] = true;
                if (options.allow_creating_new_sessions)
                    params["allowCreatingNewSessions"] = *options.allow_creating_new_sessions;
                const json info = rpc->invoke("host.start", params).get();
                return AhpHost(HostStartResult(info), rpc);
            }
            catch (...)
            {
                {
                    std::lock_guard<std::mutex> lock(ahp_mutex_);
                    ahp_exit_callbacks_.erase(host_id);
                }
                release_host_sessions(host_id);
                throw;
            }
        }
    );
}

// NODEJS REFERENCE: client.ts handleHostExit
void Client::handle_host_exit(const AhpHostExit& exit)
{
    release_host_sessions(exit.host_id);
    std::function<void(const AhpHostExit&)> on_exit;
    {
        std::lock_guard<std::mutex> lock(ahp_mutex_);
        const auto it = ahp_exit_callbacks_.find(exit.host_id);
        if (it == ahp_exit_callbacks_.end())
            return;
        on_exit = std::move(it->second);
        ahp_exit_callbacks_.erase(it);
    }
    run_detached([on_exit = std::move(on_exit), exit]() { on_exit(exit); });
}

// NODEJS REFERENCE: client.ts attachConnectionHandlers host.exited /
// host.authenticationCancelled / host.sessionReleased
void Client::handle_host_notification(const std::string& method, const json& params)
{
    if (!params.is_object())
        return;
    const std::string host_id = params.value("hostId", "");
    if (method == "host.exited")
    {
        AhpHostExit exit;
        exit.host_id = host_id;
        exit.reason = params.value("reason", "");
        detail::get_optional(params, "exitCode", exit.exit_code);
        detail::get_optional(params, "error", exit.error);
        handle_host_exit(exit);
    }
    else if (method == "host.authenticationCancelled")
    {
        std::shared_ptr<AhpAuthenticationAttempt> attempt;
        {
            std::lock_guard<std::mutex> lock(ahp_mutex_);
            const auto it = ahp_authentications_.find(params.value("requestId", ""));
            if (it != ahp_authentications_.end() && it->second->host_id == host_id)
                attempt = it->second;
        }
        if (attempt)
            attempt->cancellation.cancel();
    }
    else if (method == "host.sessionReleased")
    {
        const std::string handoff_id = params.value("handoffId", "");
        bool matches = false;
        {
            std::lock_guard<std::mutex> lock(ahp_mutex_);
            const auto it = ahp_handoffs_.find(handoff_id);
            matches = it != ahp_handoffs_.end() && it->second->host_id == host_id;
        }
        if (matches)
            release_host_session(handoff_id);
    }
}

// NODEJS REFERENCE: client.ts disconnectHosts
void Client::disconnect_hosts()
{
    std::set<std::string> host_ids;
    {
        std::lock_guard<std::mutex> lock(ahp_mutex_);
        for (const auto& [id, _] : ahp_exit_callbacks_)
            host_ids.insert(id);
        for (const auto& [id, _] : ahp_session_factories_)
            host_ids.insert(id);
        for (const auto& [id, _] : ahp_authenticators_)
            host_ids.insert(id);
    }
    for (const auto& host_id : host_ids)
    {
        handle_host_exit(AhpHostExit{
            host_id,
            "ownerDisconnected",
            std::nullopt,
            "Owner connection closed; runtime cleanup cannot be acknowledged on this connection.",
        });
    }
}

// NODEJS REFERENCE: client.ts releaseHostSessions
void Client::release_host_sessions(const std::string& host_id)
{
    std::vector<std::shared_ptr<AhpAuthenticationAttempt>> attempts;
    std::vector<std::string> handoffs;
    {
        std::lock_guard<std::mutex> lock(ahp_mutex_);
        ahp_authenticators_.erase(host_id);
        for (auto it = ahp_authentications_.begin(); it != ahp_authentications_.end();)
        {
            if (it->second->host_id == host_id)
            {
                attempts.push_back(it->second);
                it = ahp_authentications_.erase(it);
            }
            else
                ++it;
        }
        ahp_session_factories_.erase(host_id);
        for (const auto& [id, entry] : ahp_handoffs_)
            if (entry->host_id == host_id)
                handoffs.push_back(id);
    }
    for (auto& attempt : attempts)
        attempt->cancellation.cancel();
    for (const auto& id : handoffs)
        release_host_session(id);
}

// NODEJS REFERENCE: client.ts releaseHostSession
void Client::release_host_session(const std::string& handoff_id)
{
    std::shared_ptr<AhpHandoff> entry;
    std::shared_ptr<Session> session;
    std::function<void(std::shared_ptr<Session>)> on_released;
    {
        std::lock_guard<std::mutex> lock(ahp_mutex_);
        const auto it = ahp_handoffs_.find(handoff_id);
        if (it == ahp_handoffs_.end())
            return;
        entry = it->second;
        ahp_handoffs_.erase(it);
        entry->released = true;
        entry->configs.clear();
        entry->configs_closed = true;
        session = std::move(entry->session);
        entry->session.reset();
        on_released = entry->on_released;
    }
    entry->cancellation.cancel();
    notify_host_session_released(std::move(session), std::move(on_released));
}

// NODEJS REFERENCE: client.ts notifyHostSessionReleased
void Client::notify_host_session_released(
    std::shared_ptr<Session> session, std::function<void(std::shared_ptr<Session>)> on_released
)
{
    if (!session || !on_released)
        return;
    run_detached([session = std::move(session), on_released = std::move(on_released)]()
                 { on_released(session); });
}

// NODEJS REFERENCE: client.ts createHostSession
// C# REFERENCE: Host.cs MaterializeAhpSessionAsync
// The factory runs on the request worker thread; cancellation is cooperative through
// the request's token, and a handoff that ended meanwhile is rejected afterwards.
json Client::materialize_host_session(const json& params)
{
    const std::string host_id = params.value("hostId", "");
    const std::string handoff_id = params.value("handoffId", "");
    const bool resume = params.value("resume", false);
    const auto auth_it = params.find("authentication");
    if (auth_it == params.end() || !auth_it->is_object())
        throw std::runtime_error("AHP session handoff authentication is required");
    const json raw_config = params.contains("config") && params["config"].is_object()
                                ? params["config"]
                                : json::object();
    if (!raw_config.contains("sessionId") || !raw_config["sessionId"].is_string() ||
        raw_config["sessionId"].get<std::string>().empty())
        throw std::runtime_error("AHP handoff requires sessionId");
    const std::string requested_session_id = raw_config["sessionId"].get<std::string>();

    AhpSessionFactories factories;
    auto entry = std::make_shared<AhpHandoff>();
    {
        std::lock_guard<std::mutex> lock(ahp_mutex_);
        const auto it = ahp_session_factories_.find(host_id);
        if (it == ahp_session_factories_.end() ||
            !(resume ? static_cast<bool>(it->second.resume_session)
                     : static_cast<bool>(it->second.create_session)) ||
            ahp_handoffs_.contains(handoff_id))
            throw std::runtime_error("AHP session factory is unavailable or handoff already exists");
        factories = it->second;
    }

    AhpSessionAuthentication authentication;
    {
        const json& auth = *auth_it;
        const std::string mode = auth.value("mode", "");
        const bool known_mode = mode == "github" || mode == "none" || mode == "custom";
        if (known_mode)
            authentication.mode = json(mode).get<AhpAuthenticationMode>();
        const bool has_identity = auth.contains("identity");
        const bool identity_ok = has_identity && auth["identity"].is_string() &&
                                 !is_blank(auth["identity"].get<std::string>());
        if (identity_ok)
            authentication.identity = auth["identity"].get<std::string>();
        const auto& policy = factories.policy;
        if (!known_mode || authentication.mode != policy.mode ||
            (authentication.mode == AhpAuthenticationMode::None ? has_identity : !identity_ok) ||
            (policy.mode == AhpAuthenticationMode::Custom &&
             authentication.identity != policy.require_identity))
            throw std::runtime_error("AHP trusted authentication does not match host policy");
    }

    const json expected = detail::group_ahp_host_settings(raw_config);
    entry->host_id = host_id;
    entry->requested_session_id = requested_session_id;
    entry->on_released = factories.on_session_released;
    {
        std::lock_guard<std::mutex> lock(ahp_mutex_);
        if (ahp_handoffs_.contains(handoff_id))
            throw std::runtime_error("AHP session factory is unavailable or handoff already exists");
        ahp_handoffs_[handoff_id] = entry;
    }

    try
    {
        std::shared_ptr<Session> session;
        if (resume)
        {
            json resume_config = expected;
            resume_config.erase("sessionId");
            session = factories.resume_session(AhpSessionResumeRequest{
                authentication,
                requested_session_id,
                detail::resume_session_options_from_json(resume_config),
                entry->cancellation.token(),
            });
        }
        else
        {
            session = factories.create_session(AhpSessionCreateRequest{
                authentication,
                detail::create_session_options_from_json(expected),
                entry->cancellation.token(),
            });
        }

        std::optional<json> actual;
        bool released = false;
        std::function<void(std::shared_ptr<Session>)> on_released;
        {
            std::lock_guard<std::mutex> lock(ahp_mutex_);
            entry->session = session;
            if (session)
            {
                const auto it = entry->configs.find(session.get());
                if (it != entry->configs.end())
                    actual = it->second;
            }
            entry->configs.clear();
            entry->configs_closed = true;
            released = entry->released;
            if (released)
            {
                on_released = entry->on_released;
                entry->session.reset();
            }
        }
        if (released)
        {
            notify_host_session_released(session, std::move(on_released));
            throw std::runtime_error("AHP session handoff ended");
        }
        if (!session || session->session_id() != requested_session_id ||
            get_session(session->session_id()) != session)
            throw std::runtime_error(
                "AHP callback must return the requested session from this client"
            );
        // Retained originals have no new materialization snapshot; the runtime checks them.
        if ((!resume || actual) && !detail::ahp_config_contains(actual.value_or(json()), expected))
            throw std::runtime_error("AHP callback must preserve the supplied session configuration");
        return json{{"sessionId", session->session_id()}};
    }
    catch (...)
    {
        release_host_session(handoff_id);
        throw;
    }
}

// NODEJS REFERENCE: client.ts attachConnectionHandlers host.validateAuthentication
// C# REFERENCE: Host.cs ValidateAhpAuthenticationAsync
json Client::validate_host_authentication(const json& params, const CancellationToken& cancellation)
{
    const std::string host_id = params.value("hostId", "");
    const std::string request_id = params.value("requestId", "");
    const std::string token = params.value("token", "");

    AhpAuthenticatorEntry authenticator;
    auto attempt = std::make_shared<AhpAuthenticationAttempt>();
    attempt->host_id = host_id;
    {
        std::lock_guard<std::mutex> lock(ahp_mutex_);
        const auto it = ahp_authenticators_.find(host_id);
        if (it == ahp_authenticators_.end() || ahp_authentications_.contains(request_id))
            throw std::runtime_error("AHP authenticator is unavailable or request already exists");
        authenticator = it->second;
        ahp_authentications_[request_id] = attempt;
    }
    const uint64_t link =
        cancellation.on_cancel([attempt]() { attempt->cancellation.cancel(); });
    ScopeExit cleanup{[&]()
                      {
                          cancellation.unregister(link);
                          {
                              std::lock_guard<std::mutex> lock(ahp_mutex_);
                              const auto it = ahp_authentications_.find(request_id);
                              if (it != ahp_authentications_.end() && it->second == attempt)
                                  ahp_authentications_.erase(it);
                          }
                          attempt->cancellation.cancel();
                      }};

    const auto still_current = [&]()
    {
        std::lock_guard<std::mutex> lock(ahp_mutex_);
        const auto current = ahp_authentications_.find(request_id);
        const auto host = ahp_authenticators_.find(host_id);
        return current != ahp_authentications_.end() && current->second == attempt &&
               host != ahp_authenticators_.end() &&
               host->second.generation == authenticator.generation &&
               !attempt->cancellation.is_cancelled() && !cancellation.is_cancelled();
    };

    try
    {
        const auto result = authenticator.authenticate(
            AhpAuthenticationRequest{token, attempt->cancellation.token()}
        );
        if (!still_current())
            throw std::runtime_error("AHP authentication ended");
        if (!result)
            return json(nullptr);
        if (result->identity != authenticator.require_identity ||
            !detail::is_future_rfc3339(result->expires_at))
            throw std::runtime_error("Invalid AHP authentication result");
        return json{{"identity", result->identity}, {"expiresAt", result->expires_at}};
    }
    catch (...)
    {
        throw std::runtime_error("AHP authentication rejected or ended");
    }
}

// NODEJS REFERENCE: client.ts createSession/resumeSession hostHandoffs capture
// C# REFERENCE: Host.cs CaptureAhpSession
void Client::capture_host_session(
    const std::shared_ptr<Session>& session, json config, const std::optional<json>& applied
)
{
    // Mode-dependent defaults travel in the follow-up options update; fold them in so
    // the snapshot reflects the effective session configuration.
    if (applied && applied->is_object())
        config.merge_patch(*applied);
    std::lock_guard<std::mutex> lock(ahp_mutex_);
    for (auto& [_, entry] : ahp_handoffs_)
    {
        if (!entry->released && !entry->configs_closed &&
            entry->requested_session_id == session->session_id())
            entry->configs[session.get()] = config;
    }
}

} // namespace copilot
