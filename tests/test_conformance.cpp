// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file test_conformance.cpp
/// @brief Offline conformance test suite.
///
/// This suite closes the coverage gap left by E2E tests that are skipped when
/// the real Copilot CLI is not installed (`COPILOT_SDK_CPP_SKIP_E2E=1`). It
/// exercises wire-level behavior with no external dependencies by spinning up
/// an in-process JSON-RPC peer over a loopback TCP socket (mirroring the
/// `SnapshotRpcServer` pattern from `tests/snapshot_tests/snapshot_replay.cpp`)
/// and by directly invoking the SDK request-builder / argv-builder seams that
/// are exposed for testing.
///
/// Covered surfaces:
///   * `build_cli_command_args` + `build_cli_environment` for COPILOT_HOME,
///     COPILOT_CONNECTION_TOKEN, COPILOT_SDK_AUTH_TOKEN, --session-idle-timeout,
///     --remote, --log-level, --port / --stdio.
///   * Omission tests for v0.1.49 SessionConfig / ResumeSessionConfig fields
///     (no field on the wire when the option is not set).
///   * Pending lifecycle: server-side `tool.call`, `permission.request`,
///     `userInput.request`, `elicitation.request`, `exitPlanMode.request`,
///     and `autoModeSwitch.request` dispatched through a stub RPC peer, with
///     reply payload assertions.
///   * Session-event fixture parsing through `parse_session_event` /
///     `json::get<SessionEvent>()` for major event families.
///   * Async lifetime: `Session::destroy()` / `Client::stop()` called while a
///     tool handler future is mid-flight (handler sleeps briefly); no crash,
///     no UAF, the call completes or is gracefully cancelled.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/client.hpp>
#include <copilot/copilot.hpp>
#include <copilot/events.hpp>
#include <copilot/jsonrpc.hpp>
#include <copilot/session.hpp>
#include <copilot/transport.hpp>
#include <copilot/transport_tcp.hpp>
#include <copilot/types.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace copilot;

namespace
{

// =============================================================================
// Helpers shared by multiple suites
// =============================================================================

json envelope(const char* type, json data, const char* id = "evt_conf")
{
    return json{
        {"id", id},
        {"timestamp", "2025-02-01T00:00:00Z"},
        {"parentId", nullptr},
        {"type", type},
        {"data", std::move(data)},
    };
}

bool args_contain_sequence(const std::vector<std::string>& haystack,
                           const std::vector<std::string>& needle)
{
    if (needle.empty() || haystack.size() < needle.size())
        return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j)
            if (haystack[i + j] != needle[j])
            {
                match = false;
                break;
            }
        if (match)
            return true;
    }
    return false;
}

// =============================================================================
// In-process JSON-RPC peer (mirrors SnapshotRpcServer)
// =============================================================================
//
// Connects to the SDK by opening a TCP listener on loopback and letting the
// Client connect via `ClientOptions::cli_url = "<port>"`. The peer:
//   * Responds to `ping` with `{message:"pong", protocolVersion: kSdkProtocolVersion}`
//   * Responds to `session.create` / `session.resume` / `session.destroy` /
//     `session.send` with minimal valid payloads.
//   * Exposes an `inject_request()` API for tests to push server-initiated
//     requests (tool.call, permission.request, userInput.request) and capture
//     the SDK's reply payload via a future.

class InProcessRpcPeer
{
  public:
    InProcessRpcPeer() = default;

    ~InProcessRpcPeer()
    {
        stop();
    }

    /// Start listening on a free loopback port; returns the bound port.
    int start()
    {
#ifdef _WIN32
        WinsockInitializer::instance();
#endif
        TcpTransport::Socket sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == TcpTransport::kInvalidSocket)
            throw std::runtime_error("InProcessRpcPeer: socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);

        int yes = 1;
        setsockopt(
            sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes), sizeof(yes));

        if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            close_socket(sock);
            throw std::runtime_error("InProcessRpcPeer: bind() failed");
        }
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &len) != 0)
        {
            close_socket(sock);
            throw std::runtime_error("InProcessRpcPeer: getsockname() failed");
        }
        int port = ntohs(bound.sin_port);

        if (::listen(sock, 1) != 0)
        {
            close_socket(sock);
            throw std::runtime_error("InProcessRpcPeer: listen() failed");
        }
        listen_sock_ = sock;
        thread_ = std::thread([this]() { this->run(); });
        return port;
    }

    void stop()
    {
        stop_requested_ = true;
        if (listen_sock_ != TcpTransport::kInvalidSocket)
        {
            close_socket(listen_sock_);
            listen_sock_ = TcpTransport::kInvalidSocket;
        }
        // Drop any pending injections so threads waiting on futures unblock.
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            for (auto& [id, slot] : pending_)
                slot.promise.set_value(json::object());
            pending_.clear();
        }
        if (thread_.joinable())
            thread_.join();
    }

    /// Push a server-initiated request to the SDK; returns a future that
    /// resolves to the JSON the SDK sent back as the response result.
    /// Safe to call before or after the SDK has connected.
    std::future<json> inject_request(const std::string& method, json params)
    {
        int id;
        std::future<json> fut;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            id = next_request_id_++;
            PendingSlot slot;
            slot.method = method;
            slot.params = std::move(params);
            fut = slot.promise.get_future();
            pending_.emplace(id, std::move(slot));
        }
        flush_cv_.notify_one();
        return fut;
    }

    /// Hold for a connected framer so tests can also push raw notifications
    /// (e.g. session.event) on demand.
    bool send_notification(const std::string& method, const json& params)
    {
        std::lock_guard<std::mutex> lock(framer_mutex_);
        if (!framer_)
            return false;
        json msg = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
        try
        {
            framer_->write_message(msg.dump());
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

  private:
    struct PendingSlot
    {
        std::string method;
        json params;
        std::promise<json> promise;
        bool sent = false;
    };

    TcpTransport::Socket listen_sock_ = TcpTransport::kInvalidSocket;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};

    // Map of in-flight request id (as int) to slot.
    std::mutex pending_mutex_;
    std::map<int, PendingSlot> pending_;
    int next_request_id_ = 1;
    std::condition_variable flush_cv_;

    // Framer is held under a mutex so other threads can push notifications.
    std::mutex framer_mutex_;
    MessageFramer* framer_ = nullptr;

    static void close_socket(TcpTransport::Socket sock)
    {
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
    }

    void run()
    {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        TcpTransport::Socket client_sock =
            ::accept(listen_sock_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_sock == TcpTransport::kInvalidSocket)
            return;

        TcpTransport transport(client_sock);
        MessageFramer framer(transport);
        {
            std::lock_guard<std::mutex> lock(framer_mutex_);
            framer_ = &framer;
        }

        // Reader thread: pulls inbound messages and routes
        // them to either pending-request fulfillment or request handling.
        std::thread reader([&]() {
            while (transport.is_open() && !stop_requested_)
            {
                json incoming;
                try
                {
                    incoming = json::parse(framer.read_message());
                }
                catch (...)
                {
                    break;
                }

                // SDK reply to one of our injected requests
                if (incoming.contains("id") && (incoming.contains("result") || incoming.contains("error")) &&
                    !incoming.contains("method"))
                {
                    int rid = incoming["id"].get<int>();
                    std::unique_lock<std::mutex> lock(pending_mutex_);
                    auto it = pending_.find(rid);
                    if (it != pending_.end())
                    {
                        json payload =
                            incoming.contains("result") ? incoming["result"] : incoming["error"];
                        it->second.promise.set_value(std::move(payload));
                        pending_.erase(it);
                    }
                    continue;
                }

                // SDK -> server request: respond minimally so the SDK can make progress
                if (incoming.contains("method") && incoming.contains("id"))
                {
                    const std::string method = incoming["method"].get<std::string>();
                    json result = json::object();
                    if (method == "ping")
                    {
                        result = json{
                            {"message", "pong"},
                            {"protocolVersion", kSdkProtocolVersion},
                        };
                    }
                    else if (method == "session.create")
                    {
                        result = json{{"sessionId", "sess-conf-1"}};
                    }
                    else if (method == "session.resume")
                    {
                        json params = incoming.value("params", json::object());
                        result = json{
                            {"sessionId", params.value("sessionId", std::string("sess-conf-1"))}};
                    }
                    else if (method == "session.destroy")
                    {
                        result = json::object();
                    }
                    else if (method == "session.send")
                    {
                        result = json{{"messageId", "msg-1"}};
                    }
                    else if (method == "session.list")
                    {
                        result = json{{"sessions", json::array()}};
                    }
                    else if (method == "session.getLastId")
                    {
                        result = json{{"sessionId", ""}};
                    }
                    json resp{
                        {"jsonrpc", "2.0"}, {"id", incoming["id"]}, {"result", result}};
                    try
                    {
                        std::lock_guard<std::mutex> lock(framer_mutex_);
                        framer.write_message(resp.dump());
                    }
                    catch (...)
                    {
                        break;
                    }
                }
            }
        });

        // Flush thread: dispatches any queued injected requests.
        while (!stop_requested_)
        {
            std::unique_lock<std::mutex> lock(pending_mutex_);
            flush_cv_.wait_for(
                lock, std::chrono::milliseconds(50), [this]() { return stop_requested_.load(); });

            for (auto& [id, slot] : pending_)
            {
                if (slot.sent)
                    continue;
                slot.sent = true;
                json req{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"method", slot.method},
                    {"params", slot.params},
                };
                lock.unlock();
                try
                {
                    std::lock_guard<std::mutex> fl(framer_mutex_);
                    framer.write_message(req.dump());
                }
                catch (...)
                {
                }
                lock.lock();
            }
            if (!transport.is_open())
                break;
        }

        {
            std::lock_guard<std::mutex> lock(framer_mutex_);
            framer_ = nullptr;
        }
        if (reader.joinable())
            reader.join();
    }
};

/// RAII connected-client harness backed by an InProcessRpcPeer.
struct ConnectedHarness
{
    std::unique_ptr<InProcessRpcPeer> peer;
    std::unique_ptr<Client> client;

    ConnectedHarness()
    {
        peer = std::make_unique<InProcessRpcPeer>();
        int port = peer->start();
        ClientOptions opts;
        opts.use_stdio = false;
        opts.cli_url = std::to_string(port);
        opts.auto_start = false;
        client = std::make_unique<Client>(opts);
        client->start().get();
    }

    ~ConnectedHarness()
    {
        try
        {
            if (client)
                client->stop().get();
        }
        catch (...)
        {
        }
        if (peer)
            peer->stop();
    }
};

} // namespace

// =============================================================================
// Section A. ClientOptions -> argv / env mapping (testable seams)
// =============================================================================

TEST(ConformanceCliArgs, BaselineStdioEmitsServerAndLogLevel)
{
    ClientOptions opts;
    opts.use_stdio = true;
    opts.log_level = LogLevel::Debug;
    auto args = build_cli_command_args(opts);
    EXPECT_TRUE(args_contain_sequence(args, {"--server"}));
    EXPECT_TRUE(args_contain_sequence(args, {"--log-level", "debug"}));
    EXPECT_TRUE(args_contain_sequence(args, {"--stdio"}));
    // --port must not appear when use_stdio is true.
    EXPECT_FALSE(args_contain_sequence(args, {"--port"}));
}

TEST(ConformanceCliArgs, TcpServerEmitsPortFlag)
{
    ClientOptions opts;
    opts.use_stdio = false;
    opts.port = 8765;
    auto args = build_cli_command_args(opts);
    EXPECT_TRUE(args_contain_sequence(args, {"--port", "8765"}));
    EXPECT_FALSE(args_contain_sequence(args, {"--stdio"}));
}

TEST(ConformanceCliArgs, SessionIdleTimeoutFlagOnlyWhenPositive)
{
    ClientOptions opts;
    opts.use_stdio = true;
    auto args_off = build_cli_command_args(opts);
    EXPECT_FALSE(args_contain_sequence(args_off, {"--session-idle-timeout"}));

    opts.session_idle_timeout_seconds = 0;
    auto args_zero = build_cli_command_args(opts);
    EXPECT_FALSE(args_contain_sequence(args_zero, {"--session-idle-timeout"}));

    opts.session_idle_timeout_seconds = 300;
    auto args_on = build_cli_command_args(opts);
    EXPECT_TRUE(args_contain_sequence(args_on, {"--session-idle-timeout", "300"}));
}

TEST(ConformanceCliArgs, RemoteFlagToggle)
{
    ClientOptions opts;
    opts.use_stdio = true;
    auto args_off = build_cli_command_args(opts);
    EXPECT_FALSE(args_contain_sequence(args_off, {"--remote"}));

    opts.remote = true;
    auto args_on = build_cli_command_args(opts);
    EXPECT_TRUE(args_contain_sequence(args_on, {"--remote"}));
}

TEST(ConformanceCliArgs, ExtraCliArgsPrependedBeforeServerFlag)
{
    ClientOptions opts;
    opts.use_stdio = true;
    opts.cli_args = std::vector<std::string>{"--extra-flag", "extra-value"};
    auto args = build_cli_command_args(opts);
    ASSERT_GE(args.size(), 3u);
    EXPECT_EQ(args[0], "--extra-flag");
    EXPECT_EQ(args[1], "extra-value");
    EXPECT_EQ(args[2], "--server");
}

TEST(ConformanceCliEnv, ForwardsAllThreeWellKnownVarsWhenSet)
{
    ClientOptions opts;
    opts.copilot_home = std::string{"/custom/home"};
    opts.tcp_connection_token = std::string{"tok-1234"};
    opts.github_token = std::string{"gh-token-xyz"};
    auto env = build_cli_environment(opts);
    EXPECT_EQ(env["COPILOT_HOME"], "/custom/home");
    EXPECT_EQ(env["COPILOT_CONNECTION_TOKEN"], "tok-1234");
    EXPECT_EQ(env["COPILOT_SDK_AUTH_TOKEN"], "gh-token-xyz");
}

TEST(ConformanceCliEnv, OmittedVarsAreNotInjected)
{
    ClientOptions opts;
    auto env = build_cli_environment(opts);
    EXPECT_EQ(env.count("COPILOT_HOME"), 0u);
    EXPECT_EQ(env.count("COPILOT_CONNECTION_TOKEN"), 0u);
    EXPECT_EQ(env.count("COPILOT_SDK_AUTH_TOKEN"), 0u);
}

TEST(ConformanceCliEnv, StripsNodeDebugFromCallerEnv)
{
    ClientOptions opts;
    opts.environment = std::map<std::string, std::string>{
        {"NODE_DEBUG", "verbose"},
        {"KEEP_ME", "yes"},
    };
    auto env = build_cli_environment(opts);
    EXPECT_EQ(env.count("NODE_DEBUG"), 0u) << "NODE_DEBUG must be stripped";
    EXPECT_EQ(env["KEEP_ME"], "yes");
}

// =============================================================================
// Section B. v0.1.49 omission tests on session.create / session.resume payloads
// =============================================================================

TEST(ConformanceSessionPayload, CreateRequestOmitsAllV0149FieldsByDefault)
{
    SessionConfig cfg;
    auto req = build_session_create_request(cfg);
    EXPECT_FALSE(req.contains("clientName"));
    EXPECT_FALSE(req.contains("enableSessionTelemetry"));
    EXPECT_FALSE(req.contains("includeSubAgentStreamingEvents"));
    EXPECT_FALSE(req.contains("enableConfigDiscovery"));
    EXPECT_FALSE(req.contains("instructionDirectories"));
    EXPECT_FALSE(req.contains("remoteSession"));
}

TEST(ConformanceSessionPayload, CreateRequestEmitsCamelCaseForAllV0149Fields)
{
    SessionConfig cfg;
    cfg.client_name = "conformance-suite";
    cfg.enable_session_telemetry = false;
    cfg.include_sub_agent_streaming_events = true;
    cfg.enable_config_discovery = true;
    cfg.instruction_directories = std::vector<std::string>{"/a", "/b"};
    cfg.remote_session = RemoteSessionMode::On;

    auto req = build_session_create_request(cfg);
    // Field names must match the upstream camelCase wire contract exactly.
    EXPECT_EQ(req["clientName"], "conformance-suite");
    EXPECT_FALSE(req["enableSessionTelemetry"].get<bool>());
    EXPECT_TRUE(req["includeSubAgentStreamingEvents"].get<bool>());
    EXPECT_TRUE(req["enableConfigDiscovery"].get<bool>());
    ASSERT_TRUE(req["instructionDirectories"].is_array());
    EXPECT_EQ(req["instructionDirectories"].size(), 2u);
    EXPECT_EQ(req["remoteSession"], "on");
}

TEST(ConformanceSessionPayload, ResumeRequestOmitsAllV0149FieldsByDefault)
{
    ResumeSessionConfig cfg;
    auto req = build_session_resume_request("sess-omit", cfg);
    EXPECT_FALSE(req.contains("clientName"));
    EXPECT_FALSE(req.contains("enableSessionTelemetry"));
    EXPECT_FALSE(req.contains("includeSubAgentStreamingEvents"));
    EXPECT_FALSE(req.contains("enableConfigDiscovery"));
    EXPECT_FALSE(req.contains("instructionDirectories"));
    EXPECT_FALSE(req.contains("remoteSession"));
}

// =============================================================================
// Section C. Callback + parity type serialization
// =============================================================================

TEST(ConformanceTypes, ElicitationRequestedModeEnumRoundTrip)
{
    const std::vector<std::pair<ElicitationRequestedMode, const char*>> cases = {
        {ElicitationRequestedMode::Form, "form"},
        {ElicitationRequestedMode::Url, "url"},
    };

    for (const auto& [value, wire] : cases)
    {
        json j = value;
        EXPECT_EQ(j, wire);
        EXPECT_EQ(j.get<ElicitationRequestedMode>(), value);
    }
}

TEST(ConformanceTypes, ElicitationActionEnumRoundTrip)
{
    const std::vector<std::pair<ElicitationAction, const char*>> cases = {
        {ElicitationAction::Accept, "accept"},
        {ElicitationAction::Decline, "decline"},
        {ElicitationAction::Cancel, "cancel"},
    };

    for (const auto& [value, wire] : cases)
    {
        json j = value;
        EXPECT_EQ(j, wire);
        EXPECT_EQ(j.get<ElicitationAction>(), value);
    }
}

TEST(ConformanceTypes, AutoModeSwitchResponseEnumRoundTrip)
{
    const std::vector<std::pair<AutoModeSwitchResponse, const char*>> cases = {
        {AutoModeSwitchResponse::Yes, "yes"},
        {AutoModeSwitchResponse::YesAlways, "yes_always"},
        {AutoModeSwitchResponse::No, "no"},
    };

    for (const auto& [value, wire] : cases)
    {
        json j = value;
        EXPECT_EQ(j, wire);
        EXPECT_EQ(j.get<AutoModeSwitchResponse>(), value);
    }
}

TEST(ConformanceTypes, ElicitationSchemaRoundTrip)
{
    ElicitationSchema schema;
    schema.properties = std::map<std::string, json>{
        {"name", json{{"type", "string"}}},
        {"age", json{{"type", "number"}, {"minimum", 0}}},
    };
    schema.required = std::vector<std::string>{"name"};

    json j = schema;
    EXPECT_EQ(j["type"], "object");
    EXPECT_EQ(j["properties"]["name"]["type"], "string");
    EXPECT_EQ(j["required"][0], "name");

    auto parsed = j.get<ElicitationSchema>();
    EXPECT_EQ(parsed.type, "object");
    ASSERT_TRUE(parsed.properties.has_value());
    ASSERT_TRUE(parsed.required.has_value());
    EXPECT_EQ(parsed.properties->at("age")["minimum"], 0);
    EXPECT_EQ((*parsed.required)[0], "name");
}

TEST(ConformanceTypes, ElicitationContextFromJsonAllFields)
{
    json input = {
        {"sessionId", "sess-elicit"},
        {"message", "Need more info"},
        {"requestedSchema",
         {{"type", "object"},
          {"properties", {{"email", {{"type", "string"}, {"format", "email"}}}}},
          {"required", json::array({"email"})}}},
        {"mode", "form"},
        {"elicitationSource", "tool"},
        {"url", "https://example.invalid/form"},
    };

    auto context = input.get<ElicitationContext>();
    EXPECT_EQ(context.session_id, "sess-elicit");
    EXPECT_EQ(context.message, "Need more info");
    ASSERT_TRUE(context.requested_schema.has_value());
    ASSERT_TRUE(context.requested_schema->properties.has_value());
    EXPECT_EQ(context.requested_schema->properties->at("email")["format"], "email");
    EXPECT_EQ(context.mode, std::optional<ElicitationRequestedMode>(ElicitationRequestedMode::Form));
    EXPECT_EQ(context.elicitation_source, std::optional<std::string>("tool"));
    EXPECT_EQ(context.url, std::optional<std::string>("https://example.invalid/form"));
}

TEST(ConformanceTypes, ElicitationResultRoundTripVariants)
{
    ElicitationResult accepted;
    accepted.action = ElicitationAction::Accept;
    accepted.content = std::map<std::string, json>{
        {"choice", "allow"},
        {"confidence", 0.9},
    };

    json accepted_json = accepted;
    EXPECT_EQ(accepted_json["action"], "accept");
    EXPECT_EQ(accepted_json["content"]["choice"], "allow");

    auto accepted_back = accepted_json.get<ElicitationResult>();
    EXPECT_EQ(accepted_back.action, ElicitationAction::Accept);
    ASSERT_TRUE(accepted_back.content.has_value());
    EXPECT_EQ(accepted_back.content->at("confidence"), 0.9);

    ElicitationResult cancelled;
    cancelled.action = ElicitationAction::Cancel;
    json cancelled_json = cancelled;
    EXPECT_EQ(cancelled_json, (json{{"action", "cancel"}}));

    auto cancelled_back = cancelled_json.get<ElicitationResult>();
    EXPECT_EQ(cancelled_back.action, ElicitationAction::Cancel);
    EXPECT_FALSE(cancelled_back.content.has_value());
}

TEST(ConformanceTypes, ExitPlanModeRequestRoundTrip)
{
    ExitPlanModeRequest request;
    request.summary = "Plan complete";
    request.plan_content = "1. Build\n2. Test";
    request.actions = {"autopilot", "stay_in_plan", "cancel"};
    request.recommended_action = "stay_in_plan";

    json j = request;
    EXPECT_EQ(j["summary"], "Plan complete");
    EXPECT_EQ(j["planContent"], "1. Build\n2. Test");
    EXPECT_EQ(j["actions"][1], "stay_in_plan");
    EXPECT_EQ(j["recommendedAction"], "stay_in_plan");

    auto parsed = j.get<ExitPlanModeRequest>();
    EXPECT_EQ(parsed.summary, request.summary);
    EXPECT_EQ(parsed.plan_content, request.plan_content);
    EXPECT_EQ(parsed.actions, request.actions);
    EXPECT_EQ(parsed.recommended_action, request.recommended_action);
}

TEST(ConformanceTypes, ExitPlanModeResultRoundTripVariants)
{
    ExitPlanModeResult approved;
    approved.approved = true;
    approved.selected_action = "autopilot";
    approved.feedback = "Proceed";

    json approved_json = approved;
    EXPECT_TRUE(approved_json["approved"].get<bool>());
    EXPECT_EQ(approved_json["selectedAction"], "autopilot");
    EXPECT_EQ(approved_json["feedback"], "Proceed");

    auto approved_back = approved_json.get<ExitPlanModeResult>();
    EXPECT_TRUE(approved_back.approved);
    EXPECT_EQ(approved_back.selected_action, std::optional<std::string>("autopilot"));
    EXPECT_EQ(approved_back.feedback, std::optional<std::string>("Proceed"));

    ExitPlanModeResult defaults;
    json default_json = defaults;
    EXPECT_EQ(default_json, (json{{"approved", true}}));

    auto defaults_back = default_json.get<ExitPlanModeResult>();
    EXPECT_TRUE(defaults_back.approved);
    EXPECT_FALSE(defaults_back.selected_action.has_value());
    EXPECT_FALSE(defaults_back.feedback.has_value());
}

TEST(ConformanceTypes, AutoModeSwitchRequestRoundTrip)
{
    AutoModeSwitchRequest request;
    request.error_code = "rate_limit";
    request.retry_after_seconds = 12.5;

    json j = request;
    EXPECT_EQ(j["errorCode"], "rate_limit");
    EXPECT_DOUBLE_EQ(j["retryAfterSeconds"].get<double>(), 12.5);

    auto parsed = j.get<AutoModeSwitchRequest>();
    EXPECT_EQ(parsed.error_code, std::optional<std::string>("rate_limit"));
    ASSERT_TRUE(parsed.retry_after_seconds.has_value());
    EXPECT_DOUBLE_EQ(*parsed.retry_after_seconds, 12.5);
}

TEST(ConformanceSessionPayload, CreateRequestOmitsClientOnlyHandlerFields)
{
    SessionConfig cfg;
    cfg.on_permission_request = [](const PermissionRequest&) { return PermissionRequestResult{}; };
    cfg.on_user_input_request = [](const UserInputRequest&, const UserInputInvocation&) {
        return UserInputResponse{.answer = "ok"};
    };
    cfg.on_elicitation_request = [](const ElicitationContext&) {
        return ElicitationResult{.action = ElicitationAction::Accept};
    };
    cfg.on_exit_plan_mode = [](const ExitPlanModeRequest&, const ExitPlanModeInvocation&) {
        return ExitPlanModeResult{};
    };
    cfg.on_auto_mode_switch = [](const AutoModeSwitchRequest&, const AutoModeSwitchInvocation&) {
        return AutoModeSwitchResponse::Yes;
    };
    cfg.on_event = [](const SessionEvent&) {};

    auto req = build_session_create_request(cfg);
    EXPECT_TRUE(req["requestPermission"].get<bool>());
    EXPECT_TRUE(req["requestUserInput"].get<bool>());

    for (const char* key : {"onPermissionRequest",
                            "onUserInputRequest",
                            "onElicitationRequest",
                            "onExitPlanMode",
                            "onAutoModeSwitch",
                            "onEvent",
                            "requestElicitation",
                            "requestExitPlanMode",
                            "requestAutoModeSwitch"})
    {
        EXPECT_FALSE(req.contains(key)) << key;
    }
}

TEST(ConformanceTypes, SectionOverrideActionEnumRoundTrip)
{
    const std::vector<std::pair<SectionOverrideAction, const char*>> cases = {
        {SectionOverrideAction::Replace, "replace"},
        {SectionOverrideAction::Remove, "remove"},
        {SectionOverrideAction::Append, "append"},
        {SectionOverrideAction::Prepend, "prepend"},
        {SectionOverrideAction::Transform, "transform"},
    };

    for (const auto& [value, wire] : cases)
    {
        json j = value;
        EXPECT_EQ(j, wire);
        EXPECT_EQ(j.get<SectionOverrideAction>(), value);
    }
}

TEST(ConformanceSessionPayload, SessionConfigDefaultAgentUsesExpectedWireShape)
{
    SessionConfig cfg;
    cfg.default_agent = DefaultAgentConfig{.excluded_tools = std::vector<std::string>{"bash", "write_file"}};

    auto req = build_session_create_request(cfg);
    ASSERT_TRUE(req.contains("defaultAgent"));
    EXPECT_EQ(req["defaultAgent"]["excludedTools"], json::array({"bash", "write_file"}));
}

TEST(ConformanceTypes, ProviderConfigRoundTripWithNewFields)
{
    ProviderConfig config{
        .type = "azure",
        .headers = std::map<std::string, std::string>{{"X-Test", "value"}},
        .model_id = "gpt-4.1",
        .wire_model = "deployment-name",
        .max_input_tokens = 12345,
        .max_output_tokens = 678,
    };

    json j = config;
    EXPECT_EQ(j["headers"]["X-Test"], "value");
    EXPECT_EQ(j["modelId"], "gpt-4.1");
    EXPECT_EQ(j["wireModel"], "deployment-name");
    EXPECT_EQ(j["maxPromptTokens"], 12345);
    EXPECT_EQ(j["maxOutputTokens"], 678);

    auto parsed = j.get<ProviderConfig>();
    ASSERT_TRUE(parsed.headers.has_value());
    EXPECT_EQ(parsed.headers->at("X-Test"), "value");
    EXPECT_EQ(parsed.model_id, std::optional<std::string>("gpt-4.1"));
    EXPECT_EQ(parsed.wire_model, std::optional<std::string>("deployment-name"));
    EXPECT_EQ(parsed.max_input_tokens, std::optional<int>(12345));
    EXPECT_EQ(parsed.max_output_tokens, std::optional<int>(678));
}

TEST(ConformanceTypes, SystemMessageModeCustomizeSerializes)
{
    json j = SystemMessageMode::Customize;
    EXPECT_EQ(j, "customize");
    EXPECT_EQ(j.get<SystemMessageMode>(), SystemMessageMode::Customize);
}

TEST(ConformanceTypes, McpHttpServerConfigOauthGrantTypeEnumRoundTrip)
{
    const std::vector<std::pair<McpHttpServerConfigOauthGrantType, const char*>> cases = {
        {McpHttpServerConfigOauthGrantType::AuthorizationCode, "authorization_code"},
        {McpHttpServerConfigOauthGrantType::ClientCredentials, "client_credentials"},
    };

    for (const auto& [value, wire] : cases)
    {
        json j = value;
        EXPECT_EQ(j, wire);
        EXPECT_EQ(j.get<McpHttpServerConfigOauthGrantType>(), value);
    }
}

TEST(ConformanceDefaults, SessionCallbackDefaultsAreSafe)
{
    auto session = std::make_shared<Session>("sess-defaults", nullptr);

    auto elicitation = session->handle_elicitation_request(ElicitationContext{.message = "Need input"});
    EXPECT_EQ(elicitation.action, ElicitationAction::Cancel);
    EXPECT_FALSE(elicitation.content.has_value());

    auto exit_plan = session->handle_exit_plan_mode_request(ExitPlanModeRequest{.summary = "done"});
    EXPECT_TRUE(exit_plan.approved);
    EXPECT_FALSE(exit_plan.selected_action.has_value());
    EXPECT_FALSE(exit_plan.feedback.has_value());

    EXPECT_EQ(session->handle_auto_mode_switch_request(AutoModeSwitchRequest{}), AutoModeSwitchResponse::No);
}

// =============================================================================
// Section D. Pending lifecycle: tool.call / permission.request / userInput.request
// =============================================================================

TEST(ConformancePending, ToolCallInvokesHandlerAndReturnsResultPayload)
{
    ConnectedHarness h;
    SessionConfig cfg;
    Tool tool;
    tool.name = "echo";
    tool.description = "echoes its 'msg' argument";
    tool.parameters_schema = json{{"type", "object"}};
    tool.handler = [](const ToolInvocation& inv) -> ToolResultObject {
        ToolResultObject r;
        r.text_result_for_llm =
            "echoed:" + inv.arguments.value_or(json::object()).value("msg", std::string());
        r.result_type = ToolResultType::Success;
        return r;
    };
    cfg.tools = {tool};
    auto session = h.client->create_session(cfg).get();

    // Inject tool.call from the server side and wait for the SDK's reply.
    json params{
        {"sessionId", session->session_id()},
        {"toolCallId", "tc-1"},
        {"toolName", "echo"},
        {"arguments", json{{"msg", "hi"}}},
    };
    auto fut = h.peer->inject_request("tool.call", params);
    auto status = fut.wait_for(std::chrono::seconds(5));
    ASSERT_EQ(status, std::future_status::ready);

    json reply = fut.get();
    ASSERT_TRUE(reply.contains("result"));
    EXPECT_EQ(reply["result"]["textResultForLlm"], "echoed:hi");
    EXPECT_EQ(reply["result"]["resultType"], "success");
}

TEST(ConformancePending, ToolCallForUnknownToolReturnsFailurePayload)
{
    ConnectedHarness h;
    auto session = h.client->create_session(SessionConfig{}).get();

    json params{
        {"sessionId", session->session_id()},
        {"toolCallId", "tc-missing"},
        {"toolName", "no_such_tool"},
        {"arguments", json::object()},
    };
    auto fut = h.peer->inject_request("tool.call", params);
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    json reply = fut.get();
    ASSERT_TRUE(reply.contains("result"));
    EXPECT_EQ(reply["result"]["resultType"], "failure");
    EXPECT_NE(
        reply["result"]["textResultForLlm"].get<std::string>().find("no_such_tool"),
        std::string::npos);
}

TEST(ConformancePending, PermissionRequestDispatchesToHandlerAndReturnsNestedResult)
{
    ConnectedHarness h;
    SessionConfig cfg;
    auto session = h.client->create_session(cfg).get();

    std::atomic<int> handler_calls{0};
    session->register_permission_handler(
        [&](const PermissionRequest& req) -> PermissionRequestResult
        {
            ++handler_calls;
            EXPECT_EQ(req.kind, "tool");
            PermissionRequestResult r;
            r.kind = "approved";
            return r;
        });

    json params{
        {"sessionId", session->session_id()},
        {"permissionRequest",
         json{{"kind", "tool"}, {"toolCallId", "tc-perm"}, {"toolName", "write"}}},
    };
    auto fut = h.peer->inject_request("permission.request", params);
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    json reply = fut.get();

    EXPECT_EQ(handler_calls.load(), 1);
    ASSERT_TRUE(reply.contains("result"));
    EXPECT_EQ(reply["result"]["kind"], "approved");
}

TEST(ConformancePending, UserInputRequestDispatchesToHandlerAndReturnsAnswer)
{
    ConnectedHarness h;
    SessionConfig cfg;
    auto session = h.client->create_session(cfg).get();

    session->register_user_input_handler(
        [](const UserInputRequest& req, const UserInputInvocation&) -> UserInputResponse
        {
            EXPECT_EQ(req.question, "What is 2+2?");
            UserInputResponse r;
            r.answer = "4";
            r.was_freeform = true;
            return r;
        });

    json params{
        {"sessionId", session->session_id()},
        {"question", "What is 2+2?"},
        {"allowFreeform", true},
    };
    auto fut = h.peer->inject_request("userInput.request", params);
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    json reply = fut.get();

    EXPECT_EQ(reply["answer"], "4");
    EXPECT_TRUE(reply["wasFreeform"].get<bool>());
}

// =============================================================================
// Section E. New pending callback handlers
// =============================================================================

TEST(ConformancePending, ElicitationRequestDispatchesToConfiguredHandler)
{
    ConnectedHarness h;
    SessionConfig cfg;
    cfg.on_elicitation_request = [](const ElicitationContext& context) -> ElicitationResult
    {
        EXPECT_EQ(context.session_id, "sess-conf-1");
        EXPECT_EQ(context.message, "Need approval");
        EXPECT_EQ(context.mode, std::optional<ElicitationRequestedMode>(ElicitationRequestedMode::Form));

        ElicitationResult result;
        result.action = ElicitationAction::Accept;
        result.content = std::map<std::string, json>{{"approved", true}, {"reason", "looks good"}};
        return result;
    };

    auto session = h.client->create_session(cfg).get();
    auto fut = h.peer->inject_request(
        "elicitation.request",
        json{
            {"sessionId", session->session_id()},
            {"message", "Need approval"},
            {"requestedSchema",
             {{"type", "object"},
              {"properties", {{"approved", {{"type", "boolean"}}}}},
              {"required", json::array({"approved"})}}},
            {"mode", "form"},
            {"elicitationSource", "tool"},
        });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    json reply = fut.get();
    EXPECT_EQ(reply["action"], "accept");
    EXPECT_TRUE(reply["content"]["approved"].get<bool>());
    EXPECT_EQ(reply["content"]["reason"], "looks good");
}

TEST(ConformancePending, ExitPlanModeRequestDispatchesToConfiguredHandler)
{
    ConnectedHarness h;
    SessionConfig cfg;
    cfg.on_exit_plan_mode =
        [](const ExitPlanModeRequest& request, const ExitPlanModeInvocation& invocation) -> ExitPlanModeResult
    {
        EXPECT_EQ(invocation.session_id, "sess-conf-1");
        EXPECT_EQ(request.summary, "Plan ready");
        EXPECT_EQ(request.recommended_action, "autopilot");

        ExitPlanModeResult result;
        result.approved = false;
        result.selected_action = "stay_in_plan";
        result.feedback = "Need another review";
        return result;
    };

    auto session = h.client->create_session(cfg).get();
    auto fut = h.peer->inject_request(
        "exitPlanMode.request",
        json{
            {"sessionId", session->session_id()},
            {"summary", "Plan ready"},
            {"planContent", "1. Do the work"},
            {"actions", json::array({"autopilot", "stay_in_plan"})},
            {"recommendedAction", "autopilot"},
        });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    json reply = fut.get();
    EXPECT_FALSE(reply["approved"].get<bool>());
    EXPECT_EQ(reply["selectedAction"], "stay_in_plan");
    EXPECT_EQ(reply["feedback"], "Need another review");
}

TEST(ConformancePending, AutoModeSwitchRequestDispatchesToConfiguredHandler)
{
    ConnectedHarness h;
    SessionConfig cfg;
    cfg.on_auto_mode_switch =
        [](const AutoModeSwitchRequest& request, const AutoModeSwitchInvocation& invocation) {
            EXPECT_EQ(invocation.session_id, "sess-conf-1");
            EXPECT_EQ(request.error_code, std::optional<std::string>("rate_limit"));
            EXPECT_TRUE(request.retry_after_seconds.has_value());
            if (!request.retry_after_seconds.has_value())
                return AutoModeSwitchResponse::No;
            EXPECT_DOUBLE_EQ(*request.retry_after_seconds, 30.0);
            return AutoModeSwitchResponse::YesAlways;
        };

    auto session = h.client->create_session(cfg).get();
    auto fut = h.peer->inject_request(
        "autoModeSwitch.request",
        json{
            {"sessionId", session->session_id()},
            {"errorCode", "rate_limit"},
            {"retryAfterSeconds", 30.0},
        });

    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(fut.get(), "yes_always");
}

TEST(ConformancePending, ResumeSessionPreRegistersOnEventHandler)
{
    ConnectedHarness h;
    ResumeSessionConfig cfg;
    auto ready = std::make_shared<std::promise<SessionEventType>>();
    auto seen = std::make_shared<std::atomic<bool>>(false);
    auto ready_future = ready->get_future();

    cfg.on_event = [ready, seen](const SessionEvent& event)
    {
        if (!seen->exchange(true))
            ready->set_value(event.type);
    };

    auto session = h.client->resume_session("sess-resume-on-event", cfg).get();
    ASSERT_TRUE(h.peer->send_notification(
        "session.event",
        json{
            {"sessionId", session->session_id()},
            {"event", envelope("session.idle", json::object())},
        }));

    ASSERT_EQ(ready_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(ready_future.get(), SessionEventType::SessionIdle);
}

// =============================================================================
// Section F. Session-event fixture parsing
// =============================================================================

TEST(ConformanceEvents, ParsesSessionIdleFromWireEnvelope)
{
    auto event = envelope("session.idle", json::object()).get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::SessionIdle);
}

TEST(ConformanceEvents, ParsesAssistantMessageWithOptionalFields)
{
    json data = {
        {"messageId", "msg-asst-1"},
        {"content", "hello, world"},
        {"chunkContent", "hello"},
    };
    auto event = envelope("assistant.message", data).get<SessionEvent>();
    ASSERT_EQ(event.type, SessionEventType::AssistantMessage);
    const auto* msg = event.try_as<AssistantMessageData>();
    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->message_id, "msg-asst-1");
    EXPECT_EQ(msg->content, "hello, world");
    ASSERT_TRUE(msg->chunk_content.has_value());
    EXPECT_EQ(*msg->chunk_content, "hello");
}

TEST(ConformanceEvents, ParsesToolExecutionStartFamily)
{
    json data = {
        {"toolCallId", "tc-99"},
        {"toolName", "ls"},
        {"arguments", json{{"path", "/tmp"}}},
    };
    auto event = envelope("tool.execution_start", data).get<SessionEvent>();
    ASSERT_EQ(event.type, SessionEventType::ToolExecutionStart);
    const auto* d = event.try_as<ToolExecutionStartData>();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->tool_call_id, "tc-99");
    EXPECT_EQ(d->tool_name, "ls");
}

TEST(ConformanceEvents, ParsesToolExecutionCompleteSuccess)
{
    json data = {
        {"toolCallId", "tc-99"},
        {"success", true},
    };
    auto event = envelope("tool.execution_complete", data).get<SessionEvent>();
    ASSERT_EQ(event.type, SessionEventType::ToolExecutionComplete);
    const auto* d = event.try_as<ToolExecutionCompleteData>();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->tool_call_id, "tc-99");
    EXPECT_TRUE(d->success);
}

TEST(ConformanceEvents, ParsesPermissionRequestedAsLifecycleEvent)
{
    json data = {
        {"requestId", "req-1"},
        {"permissionRequest", json{{"kind", "tool"}, {"toolName", "rm"}}},
    };
    auto event = envelope("permission.requested", data).get<SessionEvent>();
    ASSERT_EQ(event.type, SessionEventType::PermissionRequested);
    const auto* d = event.try_as<PermissionRequestedData>();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->request_id, "req-1");
}

TEST(ConformanceEvents, ParsesUserInputRequestedAsLifecycleEvent)
{
    json data = {
        {"requestId", "req-2"},
        {"question", "continue?"},
        {"allowFreeform", false},
        {"choices", json::array({"yes", "no"})},
    };
    auto event = envelope("user_input.requested", data).get<SessionEvent>();
    ASSERT_EQ(event.type, SessionEventType::UserInputRequested);
    const auto* d = event.try_as<UserInputRequestedData>();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->question, "continue?");
    ASSERT_TRUE(d->choices.has_value());
    EXPECT_EQ(d->choices->size(), 2u);
}

TEST(ConformanceEvents, ParsingUnknownEventTypeIsNotFatal)
{
    // The SDK's permissive event router should tolerate brand-new server-side
    // event types it has not learned about yet by routing to ::Unknown rather
    // than throwing.
    auto event = envelope("definitely.brand.new.event", json{{"foo", "bar"}}).get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::Unknown);
}

// =============================================================================
// Section E. Async lifetime: destroy/stop during an in-flight handler future
// =============================================================================

TEST(ConformanceLifetime, DestroyDuringInFlightToolHandlerDoesNotCrash)
{
    ConnectedHarness h;
    SessionConfig cfg;
    std::atomic<bool> handler_entered{false};
    std::atomic<bool> handler_completed{false};

    Tool tool;
    tool.name = "slow";
    tool.description = "deliberately slow";
    tool.parameters_schema = json{{"type", "object"}};
    tool.handler = [&](const ToolInvocation&) -> ToolResultObject {
        handler_entered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        handler_completed = true;
        ToolResultObject r;
        r.text_result_for_llm = "done";
        r.result_type = ToolResultType::Success;
        return r;
    };
    cfg.tools = {tool};
    auto session = h.client->create_session(cfg).get();

    json params{
        {"sessionId", session->session_id()},
        {"toolCallId", "tc-slow"},
        {"toolName", "slow"},
        {"arguments", json::object()},
    };
    auto reply_fut = h.peer->inject_request("tool.call", params);

    // Wait until the handler is on the inside, then trigger destroy() while
    // the handler is still sleeping.
    for (int i = 0; i < 200 && !handler_entered.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(handler_entered.load()) << "tool handler did not run";

    // Should not crash, should not throw, should complete in bounded time.
    EXPECT_NO_THROW(session->destroy().get());

    // The handler thread may finish on its own; give it a chance.
    (void)reply_fut.wait_for(std::chrono::seconds(2));
    // Either the handler completed and the reply landed, or the peer's
    // promise was satisfied during stop(). Both are acceptable outcomes.
    SUCCEED();
}

TEST(ConformanceLifetime, ClientStopDuringInFlightToolHandlerIsGraceful)
{
    auto peer = std::make_unique<InProcessRpcPeer>();
    int port = peer->start();

    ClientOptions opts;
    opts.use_stdio = false;
    opts.cli_url = std::to_string(port);
    opts.auto_start = false;
    auto client = std::make_unique<Client>(opts);
    client->start().get();

    SessionConfig cfg;
    std::atomic<bool> handler_entered{false};
    Tool tool;
    tool.name = "slow";
    tool.description = "deliberately slow";
    tool.parameters_schema = json{{"type", "object"}};
    tool.handler = [&](const ToolInvocation&) -> ToolResultObject {
        handler_entered = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ToolResultObject r;
        r.text_result_for_llm = "done";
        r.result_type = ToolResultType::Success;
        return r;
    };
    cfg.tools = {tool};
    auto session = client->create_session(cfg).get();

    json params{
        {"sessionId", session->session_id()},
        {"toolCallId", "tc-stop"},
        {"toolName", "slow"},
        {"arguments", json::object()},
    };
    auto reply_fut = peer->inject_request("tool.call", params);

    for (int i = 0; i < 200 && !handler_entered.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(handler_entered.load()) << "tool handler did not run";

    // Stop the client while the tool handler is mid-flight; should not throw,
    // should not crash. Any errors are reported via the StopError vector
    // rather than exceptions.
    EXPECT_NO_THROW(client->stop().get());

    // Allow the detached handler thread to wind down before destroying the
    // peer (it owns the socket the handler's reply path runs over).
    (void)reply_fut.wait_for(std::chrono::milliseconds(800));

    peer->stop();
    SUCCEED();
}

// =============================================================================
// Section F. Documented offline-infeasible cases
// =============================================================================

// TODO(conformance): The end-to-end "spawn-real-CLI" path that exercises
// `Process::spawn` + port discovery + JSON-RPC handshake is intentionally
// validated only by the E2E suite (`test_e2e.cpp`), which is gated behind
// `COPILOT_SDK_CPP_SKIP_E2E`. The argv/env-building seams that feed
// `ProcessOptions` are covered above by direct unit tests against
// `build_cli_command_args` / `build_cli_environment`. Full subprocess
// validation belongs to CI's non-skipped E2E tier; see p2-cpp-buildsys-ci
// for E2E gating.
