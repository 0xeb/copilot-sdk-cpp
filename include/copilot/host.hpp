// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file host.hpp
/// @brief Agent Host Protocol (AHP) hosting: expose this client's sessions to remote
///        AHP clients through a runtime-managed host.
// NODEJS REFERENCE: nodejs/src/host.ts
// C# REFERENCE: dotnet/src/Host.cs

#include <copilot/cancellation.hpp>
#include <copilot/types.hpp>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace copilot
{

class Session;
class JsonRpcClient;

using HostStartResult = generated::api::HostStartResult;
using HostLocalServerOptions = generated::api::HostLocalServerOptions;
using HostGitHubEnvironmentOptions = generated::api::HostGithubEnvironmentOptions;
using HostPublishSessionResult = generated::api::HostPublishSessionResult;
using HostUnpublishSessionResult = generated::api::HostUnpublishSessionResult;
using HostListSessionsResult = generated::api::HostListSessionsResult;

/// Authentication mode of an AHP host and of the sessions it hands off.
enum class AhpAuthenticationMode
{
    GitHub,
    None,
    Custom
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    AhpAuthenticationMode,
    {
        {AhpAuthenticationMode::GitHub, "github"},
        {AhpAuthenticationMode::None, "none"},
        {AhpAuthenticationMode::Custom, "custom"},
    }
)

/// Trusted identity the runtime attached to a session handoff.
// NODEJS REFERENCE: host.ts AhpSessionAuthentication
struct AhpSessionAuthentication
{
    AhpAuthenticationMode mode = AhpAuthenticationMode::None;
    /// Present for github and custom; absent for none.
    std::optional<std::string> identity;
};

/// Accepted custom credential. `expires_at` is a future RFC 3339 timestamp with a zone.
// C# REFERENCE: Host.cs AhpAuthenticationResult
struct AhpAuthenticationResult
{
    std::string identity;
    std::string expires_at;
};

struct AhpAuthenticationRequest
{
    std::string token;
    /// Cancelled when the runtime withdraws the attempt or the host goes away.
    CancellationToken cancellation;
};

/// Validates a bearer token presented to a custom-authenticated host. Return
/// std::nullopt to reject the token.
using AhpAuthenticator =
    std::function<std::optional<AhpAuthenticationResult>(const AhpAuthenticationRequest&)>;

// NODEJS REFERENCE: host.ts AhpHostAuthentication
struct AhpHostAuthentication
{
    AhpAuthenticationMode mode = AhpAuthenticationMode::GitHub;
    /// Custom mode: the only identity the authenticator may accept.
    std::string require_identity;
    /// Custom mode: validates connecting clients' tokens.
    AhpAuthenticator authenticate;

    static AhpHostAuthentication github()
    {
        return AhpHostAuthentication{.mode = AhpAuthenticationMode::GitHub};
    }
    static AhpHostAuthentication none()
    {
        return AhpHostAuthentication{.mode = AhpAuthenticationMode::None};
    }
    static AhpHostAuthentication custom(std::string require_identity, AhpAuthenticator authenticate)
    {
        return AhpHostAuthentication{
            .mode = AhpAuthenticationMode::Custom,
            .require_identity = std::move(require_identity),
            .authenticate = std::move(authenticate),
        };
    }
};

/// Report of a host that stopped. `reason` is the wire HostExitReason
/// (e.g. "disposed", "processExited", "ownerDisconnected").
// NODEJS REFERENCE: host.ts AhpHostExit (= HostExitedNotification)
struct AhpHostExit
{
    std::string host_id;
    std::string reason;
    std::optional<int64_t> exit_code;
    std::optional<std::string> error;
};

/// A remote AHP client asked the host to create a session. The factory must create it
/// on this client with exactly `config` (adding only handlers) and return it.
// NODEJS REFERENCE: host.ts AhpSessionCreateRequest
struct AhpSessionCreateRequest
{
    AhpSessionAuthentication authentication;
    CreateSessionOptions config;
    /// Cancelled when the handoff ends before the factory returns.
    CancellationToken cancellation;
};

/// A remote AHP client asked the host to resume `session_id`.
// NODEJS REFERENCE: host.ts AhpSessionResumeRequest
struct AhpSessionResumeRequest
{
    AhpSessionAuthentication authentication;
    std::string session_id;
    ResumeSessionOptions config;
    CancellationToken cancellation;
};

// NODEJS REFERENCE: host.ts AhpHostOptions
struct AhpHostOptions
{
    /// Required: there is no implicit authentication mode.
    std::optional<AhpHostAuthentication> authentication;
    std::optional<std::string> compute_id;
    std::optional<std::string> workspace;
    std::optional<HostLocalServerOptions> local_server;
    /// Requires github authentication.
    std::optional<HostGitHubEnvironmentOptions> github_environment;
    std::optional<bool> allow_creating_new_sessions;
    /// Runs once, off the connection thread, when the host stops.
    std::function<void(const AhpHostExit&)> on_exit;
    std::function<std::shared_ptr<Session>(const AhpSessionCreateRequest&)> create_session;
    std::function<std::shared_ptr<Session>(const AhpSessionResumeRequest&)> resume_session;
    /// Runs off the connection thread when the runtime releases a handed-off session.
    std::function<void(std::shared_ptr<Session>)> on_session_released;
};

/// A running AHP host started by Client::start_ahp_host.
// NODEJS REFERENCE: host.ts AhpHost
class AhpHost
{
  public:
    AhpHost(const HostStartResult& info, std::shared_ptr<JsonRpcClient> connection);

    const std::string& host_id() const noexcept
    {
        return host_id_;
    }
    const std::optional<std::string>& url() const noexcept
    {
        return url_;
    }
    const std::optional<std::string>& environment_id() const noexcept
    {
        return environment_id_;
    }
    const std::optional<std::string>& token() const noexcept
    {
        return token_;
    }
    const std::optional<int64_t>& pid() const noexcept
    {
        return pid_;
    }

    std::future<void> dispose();
    std::future<void> set_allow_creating_new_sessions(bool allowed);
    std::future<HostPublishSessionResult> publish_session(const std::string& session_id);
    std::future<HostUnpublishSessionResult> unpublish_session(const std::string& session_id);
    std::future<HostListSessionsResult> list_sessions();

  private:
    std::string host_id_;
    std::optional<std::string> url_;
    std::optional<std::string> environment_id_;
    std::optional<std::string> token_;
    std::optional<int64_t> pid_;
    std::shared_ptr<JsonRpcClient> connection_;
};

namespace detail
{
/// Map the host's flat private settings onto SDK session-option groups.
// NODEJS REFERENCE: client.ts groupHostSessionSettings
json group_ahp_host_settings(const json& settings);

/// Inverse of the session request builders for the host-selected subset.
CreateSessionOptions create_session_options_from_json(const json& config);
ResumeSessionOptions resume_session_options_from_json(const json& config);

/// True when every key of `expected` is present in `actual` with an equal value.
// C# REFERENCE: Host.cs ContainsAhpConfig
bool ahp_config_contains(const json& actual, const json& expected);

/// RFC 3339 timestamp with an explicit zone that lies in the future.
bool is_future_rfc3339(const std::string& value);
} // namespace detail

} // namespace copilot
