// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT
//
// Unit tests for the RPC method-name catalog and typed parameter/result
// structs. The wire-string assertions guarantee parity with the upstream
// nodejs generated namespace at `reference/copilot-sdk/nodejs/src/generated/rpc.ts`.

#include <copilot/jsonrpc.hpp>
#include <copilot/rpc_methods.hpp>
#include <copilot/rpc_types.hpp>

#include <gtest/gtest.h>

namespace
{
using namespace copilot::rpc::methods;
}

// =============================================================================
// Method-name constants — exact wire strings from generated/rpc.ts
// =============================================================================

TEST(RpcMethodsCatalog, TopLevelMethods)
{
    EXPECT_STREQ(kPing, "ping");
    EXPECT_STREQ(kConnect, "connect");
    EXPECT_STREQ(kModelsList, "models.list");
    EXPECT_STREQ(kToolsList, "tools.list");
    EXPECT_STREQ(kAccountGetQuota, "account.getQuota");
    EXPECT_STREQ(kAuthGetStatus, "auth.getStatus");
    EXPECT_STREQ(kStatusGet, "status.get");
}

TEST(RpcMethodsCatalog, McpConfig)
{
    EXPECT_STREQ(kMcpConfigList, "mcp.config.list");
    EXPECT_STREQ(kMcpConfigAdd, "mcp.config.add");
    EXPECT_STREQ(kMcpConfigUpdate, "mcp.config.update");
    EXPECT_STREQ(kMcpConfigRemove, "mcp.config.remove");
    EXPECT_STREQ(kMcpConfigEnable, "mcp.config.enable");
    EXPECT_STREQ(kMcpConfigDisable, "mcp.config.disable");
    EXPECT_STREQ(kMcpDiscover, "mcp.discover");
}

TEST(RpcMethodsCatalog, SkillsAndFsProvider)
{
    EXPECT_STREQ(kSkillsConfigSetDisabledSkills, "skills.config.setDisabledSkills");
    EXPECT_STREQ(kSkillsDiscover, "skills.discover");
    EXPECT_STREQ(kSessionFsSetProvider, "sessionFs.setProvider");
}

TEST(RpcMethodsCatalog, SessionsLifecycle)
{
    EXPECT_STREQ(kSessionsFork, "sessions.fork");
    EXPECT_STREQ(kSessionsConnect, "sessions.connect");
}

TEST(RpcMethodsCatalog, SessionFsRequests)
{
    EXPECT_STREQ(kSessionFsReadFile, "sessionFs.readFile");
    EXPECT_STREQ(kSessionFsWriteFile, "sessionFs.writeFile");
    EXPECT_STREQ(kSessionFsAppendFile, "sessionFs.appendFile");
    EXPECT_STREQ(kSessionFsExists, "sessionFs.exists");
    EXPECT_STREQ(kSessionFsStat, "sessionFs.stat");
    EXPECT_STREQ(kSessionFsMkdir, "sessionFs.mkdir");
    EXPECT_STREQ(kSessionFsReaddir, "sessionFs.readdir");
    EXPECT_STREQ(kSessionFsReaddirWithTypes, "sessionFs.readdirWithTypes");
    EXPECT_STREQ(kSessionFsRm, "sessionFs.rm");
    EXPECT_STREQ(kSessionFsRename, "sessionFs.rename");
}

TEST(RpcMethodsCatalog, SessionScoped_Core)
{
    EXPECT_STREQ(kSessionSuspend, "session.suspend");
    EXPECT_STREQ(kSessionLog, "session.log");
    EXPECT_STREQ(kSessionAuthGetStatus, "session.auth.getStatus");
}

TEST(RpcMethodsCatalog, SessionScoped_Model)
{
    EXPECT_STREQ(kSessionModelGetCurrent, "session.model.getCurrent");
    EXPECT_STREQ(kSessionModelSwitchTo, "session.model.switchTo");
}

TEST(RpcMethodsCatalog, SessionScoped_ModeAndName)
{
    EXPECT_STREQ(kSessionModeGet, "session.mode.get");
    EXPECT_STREQ(kSessionModeSet, "session.mode.set");
    EXPECT_STREQ(kSessionNameGet, "session.name.get");
    EXPECT_STREQ(kSessionNameSet, "session.name.set");
}

TEST(RpcMethodsCatalog, SessionScoped_Plan)
{
    EXPECT_STREQ(kSessionPlanRead, "session.plan.read");
    EXPECT_STREQ(kSessionPlanUpdate, "session.plan.update");
    EXPECT_STREQ(kSessionPlanDelete, "session.plan.delete");
}

TEST(RpcMethodsCatalog, SessionScoped_Workspaces)
{
    EXPECT_STREQ(kSessionWorkspacesGetWorkspace, "session.workspaces.getWorkspace");
    EXPECT_STREQ(kSessionWorkspacesListFiles, "session.workspaces.listFiles");
    EXPECT_STREQ(kSessionWorkspacesReadFile, "session.workspaces.readFile");
    EXPECT_STREQ(kSessionWorkspacesCreateFile, "session.workspaces.createFile");
}

TEST(RpcMethodsCatalog, SessionScoped_InstructionsAndFleet)
{
    EXPECT_STREQ(kSessionInstructionsGetSources, "session.instructions.getSources");
    EXPECT_STREQ(kSessionFleetStart, "session.fleet.start");
}

TEST(RpcMethodsCatalog, SessionScoped_Agent)
{
    EXPECT_STREQ(kSessionAgentList, "session.agent.list");
    EXPECT_STREQ(kSessionAgentGetCurrent, "session.agent.getCurrent");
    EXPECT_STREQ(kSessionAgentSelect, "session.agent.select");
    EXPECT_STREQ(kSessionAgentDeselect, "session.agent.deselect");
    EXPECT_STREQ(kSessionAgentReload, "session.agent.reload");
}

TEST(RpcMethodsCatalog, SessionScoped_Tasks)
{
    EXPECT_STREQ(kSessionTasksStartAgent, "session.tasks.startAgent");
    EXPECT_STREQ(kSessionTasksList, "session.tasks.list");
    EXPECT_STREQ(kSessionTasksPromoteToBackground, "session.tasks.promoteToBackground");
    EXPECT_STREQ(kSessionTasksCancel, "session.tasks.cancel");
    EXPECT_STREQ(kSessionTasksRemove, "session.tasks.remove");
    EXPECT_STREQ(kSessionTasksSendMessage, "session.tasks.sendMessage");
}

TEST(RpcMethodsCatalog, SessionScoped_Skills)
{
    EXPECT_STREQ(kSessionSkillsList, "session.skills.list");
    EXPECT_STREQ(kSessionSkillsEnable, "session.skills.enable");
    EXPECT_STREQ(kSessionSkillsDisable, "session.skills.disable");
    EXPECT_STREQ(kSessionSkillsReload, "session.skills.reload");
}

TEST(RpcMethodsCatalog, SessionScoped_Mcp)
{
    EXPECT_STREQ(kSessionMcpList, "session.mcp.list");
    EXPECT_STREQ(kSessionMcpEnable, "session.mcp.enable");
    EXPECT_STREQ(kSessionMcpDisable, "session.mcp.disable");
    EXPECT_STREQ(kSessionMcpReload, "session.mcp.reload");
    EXPECT_STREQ(kSessionMcpOauthLogin, "session.mcp.oauth.login");
}

TEST(RpcMethodsCatalog, SessionScoped_PluginsExtensions)
{
    EXPECT_STREQ(kSessionPluginsList, "session.plugins.list");
    EXPECT_STREQ(kSessionExtensionsList, "session.extensions.list");
    EXPECT_STREQ(kSessionExtensionsEnable, "session.extensions.enable");
    EXPECT_STREQ(kSessionExtensionsDisable, "session.extensions.disable");
    EXPECT_STREQ(kSessionExtensionsReload, "session.extensions.reload");
}

TEST(RpcMethodsCatalog, SessionScoped_Tools)
{
    EXPECT_STREQ(kSessionToolsHandlePendingToolCall, "session.tools.handlePendingToolCall");
}

TEST(RpcMethodsCatalog, SessionScoped_Commands)
{
    EXPECT_STREQ(kSessionCommandsList, "session.commands.list");
    EXPECT_STREQ(kSessionCommandsInvoke, "session.commands.invoke");
    EXPECT_STREQ(kSessionCommandsHandlePendingCommand, "session.commands.handlePendingCommand");
    EXPECT_STREQ(kSessionCommandsRespondToQueuedCommand, "session.commands.respondToQueuedCommand");
}

TEST(RpcMethodsCatalog, SessionScoped_UiElicitation)
{
    EXPECT_STREQ(kSessionUiElicitation, "session.ui.elicitation");
    EXPECT_STREQ(kSessionUiHandlePendingElicitation, "session.ui.handlePendingElicitation");
}

TEST(RpcMethodsCatalog, SessionScoped_Permissions)
{
    EXPECT_STREQ(
        kSessionPermissionsHandlePendingPermissionRequest,
        "session.permissions.handlePendingPermissionRequest");
    EXPECT_STREQ(kSessionPermissionsSetApproveAll, "session.permissions.setApproveAll");
    EXPECT_STREQ(kSessionPermissionsResetSessionApprovals, "session.permissions.resetSessionApprovals");
}

TEST(RpcMethodsCatalog, SessionScoped_Shell)
{
    EXPECT_STREQ(kSessionShellExec, "session.shell.exec");
    EXPECT_STREQ(kSessionShellKill, "session.shell.kill");
}

TEST(RpcMethodsCatalog, SessionScoped_HistoryUsageRemote)
{
    EXPECT_STREQ(kSessionHistoryCompact, "session.history.compact");
    EXPECT_STREQ(kSessionHistoryTruncate, "session.history.truncate");
    EXPECT_STREQ(kSessionUsageGetMetrics, "session.usage.getMetrics");
    EXPECT_STREQ(kSessionRemoteEnable, "session.remote.enable");
    EXPECT_STREQ(kSessionRemoteDisable, "session.remote.disable");
}

TEST(RpcMethodsCatalog, LegacyV2Aliases)
{
    EXPECT_STREQ(kSessionCreate, "session.create");
    EXPECT_STREQ(kSessionResume, "session.resume");
    EXPECT_STREQ(kSessionList, "session.list");
    EXPECT_STREQ(kSessionGetMetadata, "session.getMetadata");
    EXPECT_STREQ(kSessionDelete, "session.delete");
    EXPECT_STREQ(kSessionGetLastId, "session.getLastId");
    EXPECT_STREQ(kSessionDestroy, "session.destroy");
    EXPECT_STREQ(kSessionSend, "session.send");
    EXPECT_STREQ(kSessionAbort, "session.abort");
    EXPECT_STREQ(kSessionGetMessages, "session.getMessages");
    EXPECT_STREQ(kSessionGetForeground, "session.getForeground");
    EXPECT_STREQ(kSessionSetForeground, "session.setForeground");
}

// =============================================================================
// Typed struct round-trips
// =============================================================================

TEST(RpcTypes, NameSetRequestRoundTrip)
{
    copilot::rpc::NameSetRequest req{"my session"};
    copilot::json j = req;
    EXPECT_EQ(j.at("name").get<std::string>(), "my session");

    auto back = j.get<copilot::rpc::NameSetRequest>();
    EXPECT_EQ(back.name, "my session");
}

TEST(RpcTypes, NameGetResultNullable)
{
    copilot::rpc::NameGetResult r{};
    copilot::json j = r;
    EXPECT_TRUE(j.at("name").is_null());

    r.name = "thing";
    j = r;
    EXPECT_EQ(j.at("name").get<std::string>(), "thing");

    auto back = j.get<copilot::rpc::NameGetResult>();
    ASSERT_TRUE(back.name.has_value());
    EXPECT_EQ(*back.name, "thing");
}

TEST(RpcTypes, ModelSwitchToRequestOptional)
{
    copilot::rpc::ModelSwitchToRequest req{"gpt-5", std::string{"medium"}};
    copilot::json j = req;
    EXPECT_EQ(j.at("modelId").get<std::string>(), "gpt-5");
    EXPECT_EQ(j.at("reasoningEffort").get<std::string>(), "medium");

    copilot::rpc::ModelSwitchToRequest bare{"gpt-5", std::nullopt};
    j = bare;
    EXPECT_FALSE(j.contains("reasoningEffort"));
}

TEST(RpcTypes, PlanReadResultNullable)
{
    copilot::rpc::PlanReadResult r{};
    copilot::json j = r;
    EXPECT_EQ(j.at("exists").get<bool>(), false);
    EXPECT_TRUE(j.at("content").is_null());
    EXPECT_TRUE(j.at("path").is_null());

    r.exists = true;
    r.content = std::string{"hello"};
    r.path = std::string{"/tmp/plan.md"};
    j = r;
    auto back = j.get<copilot::rpc::PlanReadResult>();
    EXPECT_TRUE(back.exists);
    ASSERT_TRUE(back.content.has_value());
    EXPECT_EQ(*back.content, "hello");
    ASSERT_TRUE(back.path.has_value());
    EXPECT_EQ(*back.path, "/tmp/plan.md");
}

TEST(RpcTypes, HistoryTruncateRoundTrip)
{
    copilot::rpc::HistoryTruncateRequest req{"evt-123"};
    copilot::json j = req;
    EXPECT_EQ(j.at("eventId").get<std::string>(), "evt-123");

    copilot::rpc::HistoryTruncateResult res{};
    res.events_removed = 4;
    copilot::json jr = res;
    EXPECT_EQ(jr.at("eventsRemoved").get<int>(), 4);

    auto back = jr.get<copilot::rpc::HistoryTruncateResult>();
    EXPECT_EQ(back.events_removed, 4);
}

TEST(RpcTypes, SessionsForkRoundTrip)
{
    copilot::rpc::SessionsForkRequest req{};
    req.session_id = "src-session";
    req.to_event_id = std::string{"evt-99"};
    req.name = std::string{"fork-1"};

    copilot::json j = req;
    EXPECT_EQ(j.at("sessionId").get<std::string>(), "src-session");
    EXPECT_EQ(j.at("toEventId").get<std::string>(), "evt-99");
    EXPECT_EQ(j.at("name").get<std::string>(), "fork-1");

    auto back = j.get<copilot::rpc::SessionsForkRequest>();
    EXPECT_EQ(back.session_id, "src-session");
    ASSERT_TRUE(back.to_event_id.has_value());
    EXPECT_EQ(*back.to_event_id, "evt-99");
}

TEST(RpcTypes, ShellExecOptionalFields)
{
    copilot::rpc::ShellExecRequest req{};
    req.command = "echo hi";
    req.timeout_ms = 5000;

    copilot::json j = req;
    EXPECT_EQ(j.at("command").get<std::string>(), "echo hi");
    EXPECT_FALSE(j.contains("cwd"));
    EXPECT_EQ(j.at("timeout").get<int>(), 5000);
}

TEST(RpcTypes, CommandsListRequestAllOptional)
{
    copilot::rpc::CommandsListRequest empty{};
    copilot::json j = empty;
    EXPECT_TRUE(j.is_object());
    EXPECT_EQ(j.size(), 0u);

    copilot::rpc::CommandsListRequest filt{};
    filt.include_builtins = true;
    filt.include_client_commands = false;
    j = filt;
    EXPECT_EQ(j.at("includeBuiltins").get<bool>(), true);
    EXPECT_EQ(j.at("includeClientCommands").get<bool>(), false);
    EXPECT_FALSE(j.contains("includeSkills"));
}

TEST(RpcTypes, SessionFsReadFileRoundTrip)
{
    copilot::rpc::SessionFsReadFileRequest req{"sid-1", "/etc/hosts"};
    copilot::json j = req;
    EXPECT_EQ(j.at("sessionId").get<std::string>(), "sid-1");
    EXPECT_EQ(j.at("path").get<std::string>(), "/etc/hosts");

    copilot::rpc::SessionFsReadFileResult res{};
    res.content = "127.0.0.1 localhost";
    copilot::json jr = res;
    EXPECT_FALSE(jr.contains("error"));

    res.error = copilot::rpc::SessionFsError{"ENOENT", std::string{"missing"}};
    jr = res;
    EXPECT_EQ(jr.at("error").at("code").get<std::string>(), "ENOENT");
    EXPECT_EQ(jr.at("error").at("message").get<std::string>(), "missing");
}

TEST(RpcTypes, SessionFsStatRoundTrip)
{
    copilot::rpc::SessionFsStatResult res{};
    res.is_file = true;
    res.size = 42;
    res.mtime = "2025-01-01T00:00:00Z";
    res.birthtime = "2024-12-31T23:59:59Z";

    copilot::json j = res;
    auto back = j.get<copilot::rpc::SessionFsStatResult>();
    EXPECT_TRUE(back.is_file);
    EXPECT_FALSE(back.is_directory);
    EXPECT_EQ(back.size, 42);
    EXPECT_EQ(back.mtime, "2025-01-01T00:00:00Z");
}

// =============================================================================
// RpcRequestDispatcher routing
// =============================================================================

TEST(RpcRequestDispatcher, DispatchesByMethod)
{
    copilot::rpc::RpcRequestDispatcher d;
    bool ping_called = false;
    bool other_called = false;

    d.on(kPing, [&](const copilot::json& p) -> copilot::json {
        ping_called = true;
        return copilot::json{{"echo", p.value("message", "")}};
    });
    d.on(kSessionFsReadFile, [&](const copilot::json&) -> copilot::json {
        other_called = true;
        return copilot::json{{"content", "x"}};
    });

    EXPECT_EQ(d.size(), 2u);
    EXPECT_TRUE(d.has(kPing));
    EXPECT_FALSE(d.has("unknown.method"));

    auto result = d.dispatch(kPing, copilot::json{{"message", "hello"}});
    EXPECT_TRUE(ping_called);
    EXPECT_FALSE(other_called);
    EXPECT_EQ(result.at("echo").get<std::string>(), "hello");
}

TEST(RpcRequestDispatcher, UnregisteredMethodThrowsMethodNotFound)
{
    copilot::rpc::RpcRequestDispatcher d;
    try
    {
        d.dispatch("session.does.not.exist", copilot::json::object());
        FAIL() << "expected JsonRpcError";
    }
    catch (const copilot::JsonRpcError& e)
    {
        EXPECT_EQ(e.code(), copilot::JsonRpcErrorCode::MethodNotFound);
    }
}

TEST(RpcRequestDispatcher, OffRemovesHandler)
{
    copilot::rpc::RpcRequestDispatcher d;
    d.on(kPing, [](const copilot::json&) { return copilot::json::object(); });
    EXPECT_TRUE(d.has(kPing));
    d.off(kPing);
    EXPECT_FALSE(d.has(kPing));
}

TEST(RpcRequestDispatcher, SessionFsHandlersFacade)
{
    copilot::rpc::RpcRequestDispatcher d;
    copilot::rpc::SessionFsHandlers handlers{};
    handlers.exists = [](const copilot::rpc::SessionFsExistsRequest& req) {
        copilot::rpc::SessionFsExistsResult r;
        r.exists = (req.path == "/exists");
        return r;
    };
    handlers.rm = [](const copilot::rpc::SessionFsRmRequest&) -> std::optional<copilot::rpc::SessionFsError> {
        return copilot::rpc::SessionFsError{"ENOENT", std::nullopt};
    };

    copilot::rpc::register_session_fs_handlers(d, handlers);

    // Only the populated handlers should be registered.
    EXPECT_TRUE(d.has(kSessionFsExists));
    EXPECT_TRUE(d.has(kSessionFsRm));
    EXPECT_FALSE(d.has(kSessionFsReadFile));
    EXPECT_FALSE(d.has(kSessionFsRename));

    auto exists_resp = d.dispatch(
        kSessionFsExists, copilot::json{{"sessionId", "s"}, {"path", "/exists"}});
    EXPECT_TRUE(exists_resp.at("exists").get<bool>());

    auto rm_resp = d.dispatch(
        kSessionFsRm, copilot::json{{"sessionId", "s"}, {"path", "/missing"}});
    EXPECT_EQ(rm_resp.at("code").get<std::string>(), "ENOENT");
}
