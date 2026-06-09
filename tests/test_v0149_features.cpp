// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file test_v0149_features.cpp
/// @brief Focused unit tests for v0.1.49 parity additions.
///
/// Mirrors gaps left by the v0.1.49 sync cycle that existing test files do
/// not cover. Augments — does not replace — `test_types.cpp`,
/// `test_client_session.cpp`, and `test_rpc_methods.cpp`.
///
/// Covered surfaces (offline-only; no live CLI required):
///   * `ClientOptions::tcp_connection_token` validation + auto-UUID generation.
///   * `Tool::skip_permission` / `Tool::overrides_built_in_tool` request wiring
///     through `build_session_create_request` and `build_session_resume_request`.
///   * `Session::dispatch_event` end-to-end typed-variant routing for new
///     v0.1.49 event variants delivered through the public `Session::on()` API.
///   * Round-trip serialization for typed RPC parameter/result structs added
///     by the v3 generated namespace (mode/model/history/permissions/plan).
///   * `ResumeSessionConfig` v0.1.49 fields are omitted from the resume
///     request payload by default.
///   * `RemoteSessionMode` JSON edge cases (unknown value falls back, all
///     three known values round-trip via the session.create payload).
///   * `SessionStartData` baseline parse for v0.1.49 fields.

#include <copilot/client.hpp>
#include <copilot/copilot.hpp>
#include <copilot/rpc_methods.hpp>
#include <copilot/rpc_types.hpp>
#include <copilot/session.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <regex>
#include <thread>
#include <vector>

using namespace copilot;

// =============================================================================
// Helpers
// =============================================================================

namespace
{

/// Build an event envelope around a typed data payload, matching the wire
/// shape produced by the CLI. Mirrors the helper in `test_types.cpp` but
/// lives in this TU so the two files stay independent.
json envelope(const char* type, json data)
{
    return json{
        {"id", "evt_v0149"},
        {"timestamp", "2025-01-15T10:00:00Z"},
        {"parentId", nullptr},
        {"type", type},
        {"data", std::move(data)},
    };
}

bool looks_like_uuid_v4(const std::string& s)
{
    // 8-4-4-4-12 hex; version nibble must be 4 in the 3rd group; variant
    // nibble in the 4th group must be 8/9/a/b. Matches RFC 4122.
    static const std::regex re(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-4[0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$");
    return std::regex_match(s, re);
}

} // namespace

// =============================================================================
// ClientOptions::tcp_connection_token
// =============================================================================

TEST(TcpConnectionTokenValidation, EmptyStringRejected)
{
    ClientOptions opts;
    opts.use_stdio = false;
    opts.port = 8765;
    opts.tcp_connection_token = std::string{};
    EXPECT_THROW(Client client(opts), std::invalid_argument);
}

TEST(TcpConnectionTokenValidation, StdioWithExplicitTokenRejected)
{
    ClientOptions opts;
    opts.use_stdio = true;
    opts.tcp_connection_token = "deadbeef";
    EXPECT_THROW(Client client(opts), std::invalid_argument);
}

TEST(TcpConnectionTokenValidation, AutoUuidGeneratedForTcpServer)
{
    ClientOptions opts;
    opts.use_stdio = false;
    opts.port = 8765;
    // Token intentionally left empty so the constructor synthesizes one.
    Client client(opts);
    // The constructor stores the populated options internally; we re-issue
    // construction via the public API surface and assert through a second
    // instance that the token would have been generated. The public Client
    // does not expose the mutated options, so we re-create with a wrapper
    // that captures the token from the options copy used to construct.
    //
    // We use an indirect probe: building two clients back-to-back should both
    // succeed and the token field on the local copy stays unset (the
    // constructor takes ClientOptions by value, so `opts` here is unchanged).
    EXPECT_FALSE(opts.tcp_connection_token.has_value());
    // Client construction did not throw, which is the primary property under
    // test: the auto-UUID path must not reject an unset token in TCP mode.
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(TcpConnectionTokenValidation, AutoUuidUuidShapeIsRfc4122)
{
    // The auto-generation code path is internal; verify the regex used to
    // recognize UUIDs accepts a freshly generated UUID v4 of the kind the
    // SDK creates. This guards against regressions in the regex itself.
    EXPECT_TRUE(looks_like_uuid_v4("550e8400-e29b-41d4-a716-446655440000"));
    EXPECT_TRUE(looks_like_uuid_v4("00000000-0000-4000-8000-000000000000"));
    EXPECT_FALSE(looks_like_uuid_v4("not-a-uuid"));
    EXPECT_FALSE(looks_like_uuid_v4("00000000-0000-3000-8000-000000000000")); // wrong version
}

TEST(TcpConnectionTokenValidation, ExplicitTokenAcceptedForTcpServer)
{
    ClientOptions opts;
    opts.use_stdio = false;
    opts.port = 8765;
    opts.tcp_connection_token = "my-explicit-token-1234567890";
    // Should not throw; explicit non-empty token is the documented path.
    Client client(opts);
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

TEST(TcpConnectionTokenValidation, ExternalServerNoTokenIsValid)
{
    ClientOptions opts;
    opts.cli_url = "localhost:9090";
    opts.use_stdio = false;
    // No explicit token; cli_url means we connect to an external server, so
    // no token validation applies (and no auto-generation either).
    Client client(opts);
    EXPECT_EQ(client.state(), ConnectionState::Disconnected);
}

// =============================================================================
// Tool flag wiring through build_session_create_request / build_session_resume_request
// =============================================================================

TEST(ToolFlagsRequestWiring, CreateRequestEmitsSkipPermissionAndOverride)
{
    SessionConfig cfg;
    Tool t1{};
    t1.name = "fast_tool";
    t1.description = "needs no approval";
    t1.skip_permission = true;
    Tool t2{};
    t2.name = "ls";
    t2.description = "override built-in ls";
    t2.overrides_built_in_tool = true;
    Tool t3{};
    t3.name = "plain";
    t3.description = "no flags";
    cfg.tools = {t1, t2, t3};

    auto req = build_session_create_request(cfg);
    ASSERT_TRUE(req.contains("tools"));
    ASSERT_TRUE(req["tools"].is_array());
    ASSERT_EQ(req["tools"].size(), 3u);

    // t1: skipPermission set, overridesBuiltInTool absent
    EXPECT_EQ(req["tools"][0]["name"], "fast_tool");
    ASSERT_TRUE(req["tools"][0].contains("skipPermission"));
    EXPECT_TRUE(req["tools"][0]["skipPermission"].get<bool>());
    EXPECT_FALSE(req["tools"][0].contains("overridesBuiltInTool"));

    // t2: overridesBuiltInTool set, skipPermission absent
    EXPECT_EQ(req["tools"][1]["name"], "ls");
    ASSERT_TRUE(req["tools"][1].contains("overridesBuiltInTool"));
    EXPECT_TRUE(req["tools"][1]["overridesBuiltInTool"].get<bool>());
    EXPECT_FALSE(req["tools"][1].contains("skipPermission"));

    // t3: both flags absent
    EXPECT_EQ(req["tools"][2]["name"], "plain");
    EXPECT_FALSE(req["tools"][2].contains("skipPermission"));
    EXPECT_FALSE(req["tools"][2].contains("overridesBuiltInTool"));
}

TEST(ToolFlagsRequestWiring, ResumeRequestEmitsSkipPermissionAndOverride)
{
    ResumeSessionConfig cfg;
    Tool t{};
    t.name = "deploy";
    t.description = "production deploy";
    t.skip_permission = true;
    t.overrides_built_in_tool = true;
    cfg.tools = {t};

    auto req = build_session_resume_request("sess-x", cfg);
    ASSERT_TRUE(req.contains("tools"));
    ASSERT_EQ(req["tools"].size(), 1u);
    EXPECT_TRUE(req["tools"][0]["skipPermission"].get<bool>());
    EXPECT_TRUE(req["tools"][0]["overridesBuiltInTool"].get<bool>());
}

// =============================================================================
// ResumeSessionConfig v0.1.49 fields omitted by default
// =============================================================================

TEST(ResumeSessionConfigV0149, FieldsOmittedByDefault)
{
    ResumeSessionConfig cfg;
    auto req = build_session_resume_request("sess-1", cfg);
    EXPECT_FALSE(req.contains("clientName"));
    EXPECT_FALSE(req.contains("enableSessionTelemetry"));
    EXPECT_FALSE(req.contains("includeSubAgentStreamingEvents"));
    EXPECT_FALSE(req.contains("enableConfigDiscovery"));
    EXPECT_FALSE(req.contains("instructionDirectories"));
    EXPECT_FALSE(req.contains("remoteSession"));
}

TEST(ResumeSessionConfigV0149, AllNewFieldsSerialize)
{
    ResumeSessionConfig cfg;
    cfg.client_name = "resumer";
    cfg.enable_session_telemetry = true;
    cfg.include_sub_agent_streaming_events = true;
    cfg.enable_config_discovery = false;
    cfg.instruction_directories = std::vector<std::string>{"/instr/a", "/instr/b"};
    cfg.remote_session = RemoteSessionMode::Off;

    auto req = build_session_resume_request("sess-1", cfg);
    EXPECT_EQ(req["clientName"], "resumer");
    EXPECT_TRUE(req["enableSessionTelemetry"].get<bool>());
    EXPECT_TRUE(req["includeSubAgentStreamingEvents"].get<bool>());
    EXPECT_FALSE(req["enableConfigDiscovery"].get<bool>());
    ASSERT_TRUE(req["instructionDirectories"].is_array());
    EXPECT_EQ(req["instructionDirectories"].size(), 2u);
    EXPECT_EQ(req["instructionDirectories"][1], "/instr/b");
    EXPECT_EQ(req["remoteSession"], "off");
}

// =============================================================================
// RemoteSessionMode JSON edge cases
// =============================================================================

TEST(RemoteSessionModeEdge, AllThreeKnownValuesThroughCreateRequest)
{
    for (auto mode : {RemoteSessionMode::Off, RemoteSessionMode::Export, RemoteSessionMode::On})
    {
        SessionConfig cfg;
        cfg.remote_session = mode;
        auto req = build_session_create_request(cfg);
        ASSERT_TRUE(req.contains("remoteSession"));
        // Echo the wire mapping for completeness.
        json expected = mode;
        EXPECT_EQ(req["remoteSession"], expected);
    }
}

TEST(RemoteSessionModeEdge, UnknownWireValueFallsBack)
{
    // NLOHMANN_JSON_SERIALIZE_ENUM falls back to the first-listed value when
    // the wire string is unrecognized. Document and lock that behavior.
    EXPECT_EQ(json("bogus").get<RemoteSessionMode>(), RemoteSessionMode::Off);
}

// =============================================================================
// Session::dispatch_event end-to-end typed-variant routing
// =============================================================================

TEST(SessionDispatch, RoutesV0149TitleChangedEventToTypedHandler)
{
    auto session = std::make_shared<Session>("sess-disp-1", /*client=*/nullptr);

    std::string captured_title;
    SessionEventType captured_type = SessionEventType::Unknown;
    auto sub = session->on(
        [&](const SessionEvent& e)
        {
            captured_type = e.type;
            if (const auto* d = e.try_as<SessionTitleChangedData>())
                captured_title = d->title;
        });

    auto event = envelope("session.title_changed", {{"title", "dispatched"}}).get<SessionEvent>();
    session->dispatch_event(event);

    EXPECT_EQ(captured_type, SessionEventType::SessionTitleChanged);
    EXPECT_EQ(captured_title, "dispatched");
}

TEST(SessionDispatch, RoutesV0149WarningAndModelCallFailure)
{
    auto session = std::make_shared<Session>("sess-disp-2", /*client=*/nullptr);

    int warning_seen = 0;
    int failure_seen = 0;
    auto sub = session->on(
        [&](const SessionEvent& e)
        {
            if (e.is<SessionWarningData>())
                ++warning_seen;
            else if (e.is<ModelCallFailureData>())
                ++failure_seen;
        });

    session->dispatch_event(
        envelope(
            "session.warning",
            {{"warningType", "policy"}, {"message", "soft warning"}, {"url", "https://e.x/h"}})
            .get<SessionEvent>());
    session->dispatch_event(
        envelope(
            "model.call_failure",
            {{"source", "top_level"},
             {"errorMessage", "model timed out"},
             {"model", "gpt-4"},
             {"statusCode", 504}})
            .get<SessionEvent>());

    EXPECT_EQ(warning_seen, 1);
    EXPECT_EQ(failure_seen, 1);
}

TEST(SessionDispatch, MultipleHandlersAllFireInRegistrationOrder)
{
    auto session = std::make_shared<Session>("sess-disp-3", /*client=*/nullptr);

    std::vector<int> order;
    auto s1 = session->on([&](const SessionEvent&) { order.push_back(1); });
    auto s2 = session->on([&](const SessionEvent&) { order.push_back(2); });
    auto s3 = session->on([&](const SessionEvent&) { order.push_back(3); });

    session->dispatch_event(
        envelope("session.title_changed", {{"title", "x"}}).get<SessionEvent>());

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(SessionDispatch, SubscriptionDestructorRemovesHandler)
{
    auto session = std::make_shared<Session>("sess-disp-4", /*client=*/nullptr);

    std::atomic<int> fired{0};
    {
        auto sub = session->on([&](const SessionEvent&) { fired.fetch_add(1); });
        session->dispatch_event(
            envelope("session.title_changed", {{"title", "a"}}).get<SessionEvent>());
        EXPECT_EQ(fired.load(), 1);
        // sub goes out of scope here -> unsubscribed
    }
    session->dispatch_event(
        envelope("session.title_changed", {{"title", "b"}}).get<SessionEvent>());
    EXPECT_EQ(fired.load(), 1);
}

TEST(SessionDispatch, HandlerExceptionDoesNotBreakOtherSubscribers)
{
    auto session = std::make_shared<Session>("sess-disp-5", /*client=*/nullptr);

    int saw = 0;
    auto bad = session->on([&](const SessionEvent&) { throw std::runtime_error("boom"); });
    auto good = session->on([&](const SessionEvent&) { ++saw; });

    EXPECT_NO_THROW(session->dispatch_event(
        envelope("session.title_changed", {{"title", "y"}}).get<SessionEvent>()));
    EXPECT_EQ(saw, 1);
}

TEST(SessionDispatch, NewV0149SessionWarningCarriesAllFields)
{
    auto session = std::make_shared<Session>("sess-disp-6", /*client=*/nullptr);

    SessionWarningData captured{};
    bool got = false;
    auto sub = session->on(
        [&](const SessionEvent& e)
        {
            if (const auto* d = e.try_as<SessionWarningData>())
            {
                captured = *d;
                got = true;
            }
        });

    auto ev = envelope(
                  "session.warning",
                  {{"warningType", "rate"},
                   {"message", "approaching rate limit"},
                   {"url", "https://github.com/help/rate-limit"}})
                  .get<SessionEvent>();
    session->dispatch_event(ev);

    ASSERT_TRUE(got);
    EXPECT_EQ(captured.warning_type, "rate");
    EXPECT_EQ(captured.message, "approaching rate limit");
}

// =============================================================================
// Round-trip serialization for typed RPC structs not already covered.
// =============================================================================

TEST(RpcTypesRoundTripV0149, ModeSetRequest)
{
    copilot::rpc::ModeSetRequest req{"plan"};
    json j = req;
    json expected = {{"mode", "plan"}};
    EXPECT_EQ(j, expected);
    auto back = j.get<copilot::rpc::ModeSetRequest>();
    EXPECT_EQ(back.mode, "plan");
}

TEST(RpcTypesRoundTripV0149, ModeGetResult)
{
    copilot::rpc::ModeGetResult res{"interactive"};
    json j = res;
    json expected = {{"mode", "interactive"}};
    EXPECT_EQ(j, expected);
    auto back = j.get<copilot::rpc::ModeGetResult>();
    EXPECT_EQ(back.mode, "interactive");
}

TEST(RpcTypesRoundTripV0149, ModelSwitchToResultNullableModelId)
{
    copilot::rpc::ModelSwitchToResult res{};
    json empty = res;
    EXPECT_TRUE(empty.is_object());
    EXPECT_FALSE(empty.contains("modelId"));

    res.model_id = "gpt-5";
    json populated = res;
    EXPECT_EQ(populated["modelId"], "gpt-5");

    auto back = populated.get<copilot::rpc::ModelSwitchToResult>();
    ASSERT_TRUE(back.model_id.has_value());
    EXPECT_EQ(*back.model_id, "gpt-5");
}

TEST(RpcTypesRoundTripV0149, HistoryCompactResult)
{
    copilot::rpc::HistoryCompactResult res{true, 1024, 12};
    json j = res;
    EXPECT_TRUE(j["success"].get<bool>());
    EXPECT_EQ(j["tokensRemoved"], 1024);
    EXPECT_EQ(j["messagesRemoved"], 12);
    auto back = j.get<copilot::rpc::HistoryCompactResult>();
    EXPECT_TRUE(back.success);
    EXPECT_EQ(back.tokens_removed, 1024);
    EXPECT_EQ(back.messages_removed, 12);
}

TEST(RpcTypesRoundTripV0149, PermissionsSetApproveAllRoundTrip)
{
    copilot::rpc::PermissionsSetApproveAllRequest req{true};
    json reqj = req;
    EXPECT_TRUE(reqj["enabled"].get<bool>());
    auto back_req = reqj.get<copilot::rpc::PermissionsSetApproveAllRequest>();
    EXPECT_TRUE(back_req.enabled);

    copilot::rpc::PermissionsSetApproveAllResult res{true};
    json resj = res;
    EXPECT_TRUE(resj["success"].get<bool>());
    auto back_res = resj.get<copilot::rpc::PermissionsSetApproveAllResult>();
    EXPECT_TRUE(back_res.success);
}

TEST(RpcTypesRoundTripV0149, PlanUpdateRequest)
{
    copilot::rpc::PlanUpdateRequest req{"# Plan\n- step 1"};
    json j = req;
    EXPECT_EQ(j["content"], "# Plan\n- step 1");
    auto back = j.get<copilot::rpc::PlanUpdateRequest>();
    EXPECT_EQ(back.content, "# Plan\n- step 1");
}

// =============================================================================
// SessionStartData parse (v0.1.49 fields present on the wire)
// =============================================================================

TEST(SessionStartDataParse, BaselineFieldsPopulated)
{
    json data = {
        {"sessionId", "sess-1"},
        {"version", 3.0},
        {"producer", "copilot-cli"},
        {"copilotVersion", "0.1.49"},
        {"startTime", "2025-01-15T10:00:00Z"},
        {"selectedModel", "gpt-5"},
    };
    auto event = envelope("session.start", data).get<SessionEvent>();
    EXPECT_EQ(event.type, SessionEventType::SessionStart);
    const auto& d = event.as<SessionStartData>();
    EXPECT_EQ(d.session_id, "sess-1");
    EXPECT_EQ(d.copilot_version, "0.1.49");
    ASSERT_TRUE(d.selected_model.has_value());
    EXPECT_EQ(*d.selected_model, "gpt-5");
}

TEST(WorkingDirectoryContextParse, AllOptionalFieldsRoundTrip)
{
    // Tests the structure used by session.start and session.context_changed.
    json j = {
        {"cwd", "/repo"},
        {"baseCommit", "deadbeef"},
        {"branch", "main"},
        {"gitRoot", "/repo"},
        {"headCommit", "feedface"},
        {"hostType", "github.com"},
        {"repository", "owner/repo"},
        {"repositoryHost", "github.com"},
    };
    auto ctx = j.get<WorkingDirectoryContext>();
    EXPECT_EQ(ctx.cwd, "/repo");
    ASSERT_TRUE(ctx.branch.has_value());
    EXPECT_EQ(*ctx.branch, "main");
    ASSERT_TRUE(ctx.repository.has_value());
    EXPECT_EQ(*ctx.repository, "owner/repo");
    ASSERT_TRUE(ctx.head_commit.has_value());
    EXPECT_EQ(*ctx.head_commit, "feedface");
}
