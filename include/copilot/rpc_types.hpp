// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file rpc_types.hpp
/// @brief Typed parameter/result structs for the v3 generated RPC namespace.
///
/// Companion to `rpc_methods.hpp`. Provides nlohmann::json-friendly structs
/// for the most commonly used new method families in the upstream nodejs
/// reference (`nodejs/src/generated/rpc.ts`): plan, name, mode, model,
/// session filesystem, fork, history (compact/truncate), shell, commands,
/// permissions, elicitation, user-input.
///
/// Field names follow snake_case in C++; JSON wire names remain camelCase
/// to match upstream exactly. All `sessionId` fields are spelled
/// `session_id` in C++.

#include <copilot/jsonrpc.hpp>
#include <copilot/rpc_methods.hpp>
#include <copilot/types.hpp>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace copilot::rpc
{

// =============================================================================
// Small helpers
// =============================================================================

namespace detail
{

template <typename T>
inline void set_opt(json& j, const char* key, const std::optional<T>& v)
{
    if (v.has_value())
        j[key] = *v;
}

template <typename T>
inline void get_opt(const json& j, const char* key, std::optional<T>& v)
{
    if (j.contains(key) && !j.at(key).is_null())
        v = j.at(key).get<T>();
}

} // namespace detail

// =============================================================================
// session.name.*
// =============================================================================

struct NameGetResult
{
    std::optional<std::string> name; ///< null on the wire when not yet set
};

inline void to_json(json& j, const NameGetResult& r)
{
    j = json::object();
    if (r.name.has_value())
        j["name"] = *r.name;
    else
        j["name"] = nullptr;
}

inline void from_json(const json& j, NameGetResult& r)
{
    if (j.contains("name") && !j.at("name").is_null())
        r.name = j.at("name").get<std::string>();
    else
        r.name = std::nullopt;
}

struct NameSetRequest
{
    std::string name;
};

inline void to_json(json& j, const NameSetRequest& r) { j = json{{"name", r.name}}; }
inline void from_json(const json& j, NameSetRequest& r) { j.at("name").get_to(r.name); }

// =============================================================================
// session.mode.*  (mode is "interactive" | "plan" | "autopilot")
// =============================================================================

struct ModeSetRequest
{
    std::string mode;
};

inline void to_json(json& j, const ModeSetRequest& r) { j = json{{"mode", r.mode}}; }
inline void from_json(const json& j, ModeSetRequest& r) { j.at("mode").get_to(r.mode); }

struct ModeGetResult
{
    std::string mode;
};

inline void to_json(json& j, const ModeGetResult& r) { j = json{{"mode", r.mode}}; }
inline void from_json(const json& j, ModeGetResult& r) { j.at("mode").get_to(r.mode); }

// =============================================================================
// session.model.*
// =============================================================================

struct ModelSwitchToRequest
{
    std::string model_id;
    std::optional<std::string> reasoning_effort;
};

inline void to_json(json& j, const ModelSwitchToRequest& r)
{
    j = json{{"modelId", r.model_id}};
    detail::set_opt(j, "reasoningEffort", r.reasoning_effort);
}

inline void from_json(const json& j, ModelSwitchToRequest& r)
{
    j.at("modelId").get_to(r.model_id);
    detail::get_opt(j, "reasoningEffort", r.reasoning_effort);
}

struct ModelSwitchToResult
{
    std::optional<std::string> model_id;
};

inline void to_json(json& j, const ModelSwitchToResult& r)
{
    j = json::object();
    detail::set_opt(j, "modelId", r.model_id);
}

inline void from_json(const json& j, ModelSwitchToResult& r)
{
    detail::get_opt(j, "modelId", r.model_id);
}

// =============================================================================
// session.plan.*
// =============================================================================

struct PlanReadResult
{
    bool exists = false;
    std::optional<std::string> content;
    std::optional<std::string> path;
};

inline void to_json(json& j, const PlanReadResult& r)
{
    j = json{{"exists", r.exists}};
    j["content"] = r.content.has_value() ? json(*r.content) : json(nullptr);
    j["path"]    = r.path.has_value()    ? json(*r.path)    : json(nullptr);
}

inline void from_json(const json& j, PlanReadResult& r)
{
    j.at("exists").get_to(r.exists);
    if (j.contains("content") && !j.at("content").is_null())
        r.content = j.at("content").get<std::string>();
    else
        r.content = std::nullopt;
    if (j.contains("path") && !j.at("path").is_null())
        r.path = j.at("path").get<std::string>();
    else
        r.path = std::nullopt;
}

struct PlanUpdateRequest
{
    std::string content;
};

inline void to_json(json& j, const PlanUpdateRequest& r) { j = json{{"content", r.content}}; }
inline void from_json(const json& j, PlanUpdateRequest& r) { j.at("content").get_to(r.content); }

// =============================================================================
// session.history.* (compact / truncate)
// =============================================================================

struct HistoryCompactResult
{
    bool success = false;
    int tokens_removed = 0;
    int messages_removed = 0;
};

inline void to_json(json& j, const HistoryCompactResult& r)
{
    j = json{
        {"success", r.success},
        {"tokensRemoved", r.tokens_removed},
        {"messagesRemoved", r.messages_removed},
    };
}

inline void from_json(const json& j, HistoryCompactResult& r)
{
    j.at("success").get_to(r.success);
    j.at("tokensRemoved").get_to(r.tokens_removed);
    j.at("messagesRemoved").get_to(r.messages_removed);
}

struct HistoryTruncateRequest
{
    std::string event_id;
};

inline void to_json(json& j, const HistoryTruncateRequest& r)
{
    j = json{{"eventId", r.event_id}};
}

inline void from_json(const json& j, HistoryTruncateRequest& r)
{
    j.at("eventId").get_to(r.event_id);
}

struct HistoryTruncateResult
{
    int events_removed = 0;
};

inline void to_json(json& j, const HistoryTruncateResult& r)
{
    j = json{{"eventsRemoved", r.events_removed}};
}

inline void from_json(const json& j, HistoryTruncateResult& r)
{
    j.at("eventsRemoved").get_to(r.events_removed);
}

// =============================================================================
// sessions.fork
// =============================================================================

struct SessionsForkRequest
{
    std::string session_id;
    std::optional<std::string> to_event_id;
    std::optional<std::string> name;
};

inline void to_json(json& j, const SessionsForkRequest& r)
{
    j = json{{"sessionId", r.session_id}};
    detail::set_opt(j, "toEventId", r.to_event_id);
    detail::set_opt(j, "name", r.name);
}

inline void from_json(const json& j, SessionsForkRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    detail::get_opt(j, "toEventId", r.to_event_id);
    detail::get_opt(j, "name", r.name);
}

struct SessionsForkResult
{
    std::string session_id;
    std::optional<std::string> name;
};

inline void to_json(json& j, const SessionsForkResult& r)
{
    j = json{{"sessionId", r.session_id}};
    detail::set_opt(j, "name", r.name);
}

inline void from_json(const json& j, SessionsForkResult& r)
{
    j.at("sessionId").get_to(r.session_id);
    detail::get_opt(j, "name", r.name);
}

// =============================================================================
// session.shell.*
// =============================================================================

struct ShellExecRequest
{
    std::string command;
    std::optional<std::string> cwd;
    std::optional<int> timeout_ms;
};

inline void to_json(json& j, const ShellExecRequest& r)
{
    j = json{{"command", r.command}};
    detail::set_opt(j, "cwd", r.cwd);
    if (r.timeout_ms.has_value())
        j["timeout"] = *r.timeout_ms;
}

inline void from_json(const json& j, ShellExecRequest& r)
{
    j.at("command").get_to(r.command);
    detail::get_opt(j, "cwd", r.cwd);
    if (j.contains("timeout") && !j.at("timeout").is_null())
        r.timeout_ms = j.at("timeout").get<int>();
}

struct ShellExecResult
{
    std::string process_id;
};

inline void to_json(json& j, const ShellExecResult& r) { j = json{{"processId", r.process_id}}; }
inline void from_json(const json& j, ShellExecResult& r) { j.at("processId").get_to(r.process_id); }

struct ShellKillRequest
{
    std::string process_id;
    std::optional<std::string> signal;
};

inline void to_json(json& j, const ShellKillRequest& r)
{
    j = json{{"processId", r.process_id}};
    detail::set_opt(j, "signal", r.signal);
}

inline void from_json(const json& j, ShellKillRequest& r)
{
    j.at("processId").get_to(r.process_id);
    detail::get_opt(j, "signal", r.signal);
}

struct ShellKillResult
{
    bool killed = false;
};

inline void to_json(json& j, const ShellKillResult& r) { j = json{{"killed", r.killed}}; }
inline void from_json(const json& j, ShellKillResult& r) { j.at("killed").get_to(r.killed); }

// =============================================================================
// session.commands.*
// =============================================================================

struct CommandsListRequest
{
    std::optional<bool> include_builtins;
    std::optional<bool> include_skills;
    std::optional<bool> include_client_commands;
};

inline void to_json(json& j, const CommandsListRequest& r)
{
    j = json::object();
    detail::set_opt(j, "includeBuiltins", r.include_builtins);
    detail::set_opt(j, "includeSkills", r.include_skills);
    detail::set_opt(j, "includeClientCommands", r.include_client_commands);
}

inline void from_json(const json& j, CommandsListRequest& r)
{
    detail::get_opt(j, "includeBuiltins", r.include_builtins);
    detail::get_opt(j, "includeSkills", r.include_skills);
    detail::get_opt(j, "includeClientCommands", r.include_client_commands);
}

struct CommandsInvokeRequest
{
    std::string name;
    std::optional<std::string> input;
};

inline void to_json(json& j, const CommandsInvokeRequest& r)
{
    j = json{{"name", r.name}};
    detail::set_opt(j, "input", r.input);
}

inline void from_json(const json& j, CommandsInvokeRequest& r)
{
    j.at("name").get_to(r.name);
    detail::get_opt(j, "input", r.input);
}

struct CommandsHandlePendingCommandRequest
{
    std::string request_id;
    std::optional<std::string> error;
};

inline void to_json(json& j, const CommandsHandlePendingCommandRequest& r)
{
    j = json{{"requestId", r.request_id}};
    detail::set_opt(j, "error", r.error);
}

inline void from_json(const json& j, CommandsHandlePendingCommandRequest& r)
{
    j.at("requestId").get_to(r.request_id);
    detail::get_opt(j, "error", r.error);
}

struct CommandsHandlePendingCommandResult
{
    bool success = false;
};

inline void to_json(json& j, const CommandsHandlePendingCommandResult& r)
{
    j = json{{"success", r.success}};
}

inline void from_json(const json& j, CommandsHandlePendingCommandResult& r)
{
    j.at("success").get_to(r.success);
}

// =============================================================================
// session.permissions.* (set/reset; the handle-pending one already lives in
// types.hpp as PermissionRequestResult)
// =============================================================================

struct PermissionsSetApproveAllRequest
{
    bool enabled = false;
};

inline void to_json(json& j, const PermissionsSetApproveAllRequest& r)
{
    j = json{{"enabled", r.enabled}};
}

inline void from_json(const json& j, PermissionsSetApproveAllRequest& r)
{
    j.at("enabled").get_to(r.enabled);
}

struct PermissionsSetApproveAllResult
{
    bool success = false;
};

inline void to_json(json& j, const PermissionsSetApproveAllResult& r)
{
    j = json{{"success", r.success}};
}

inline void from_json(const json& j, PermissionsSetApproveAllResult& r)
{
    j.at("success").get_to(r.success);
}

// =============================================================================
// sessionFs.*  (server-to-client requests; SDK client implements these)
// =============================================================================

struct SessionFsError
{
    std::string code; ///< "ENOENT" | "UNKNOWN"
    std::optional<std::string> message;
};

inline void to_json(json& j, const SessionFsError& e)
{
    j = json{{"code", e.code}};
    detail::set_opt(j, "message", e.message);
}

inline void from_json(const json& j, SessionFsError& e)
{
    j.at("code").get_to(e.code);
    detail::get_opt(j, "message", e.message);
}

struct SessionFsReadFileRequest
{
    std::string session_id;
    std::string path;
};

inline void to_json(json& j, const SessionFsReadFileRequest& r)
{
    j = json{{"sessionId", r.session_id}, {"path", r.path}};
}

inline void from_json(const json& j, SessionFsReadFileRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    j.at("path").get_to(r.path);
}

struct SessionFsReadFileResult
{
    std::string content;
    std::optional<SessionFsError> error;
};

inline void to_json(json& j, const SessionFsReadFileResult& r)
{
    j = json{{"content", r.content}};
    if (r.error.has_value())
        j["error"] = *r.error;
}

inline void from_json(const json& j, SessionFsReadFileResult& r)
{
    j.at("content").get_to(r.content);
    if (j.contains("error") && !j.at("error").is_null())
        r.error = j.at("error").get<SessionFsError>();
}

struct SessionFsWriteFileRequest
{
    std::string session_id;
    std::string path;
    std::string content;
    std::optional<int> mode;
};

inline void to_json(json& j, const SessionFsWriteFileRequest& r)
{
    j = json{
        {"sessionId", r.session_id},
        {"path", r.path},
        {"content", r.content},
    };
    if (r.mode.has_value())
        j["mode"] = *r.mode;
}

inline void from_json(const json& j, SessionFsWriteFileRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    j.at("path").get_to(r.path);
    j.at("content").get_to(r.content);
    if (j.contains("mode") && !j.at("mode").is_null())
        r.mode = j.at("mode").get<int>();
}

struct SessionFsAppendFileRequest
{
    std::string session_id;
    std::string path;
    std::string content;
    std::optional<int> mode;
};

inline void to_json(json& j, const SessionFsAppendFileRequest& r)
{
    j = json{
        {"sessionId", r.session_id},
        {"path", r.path},
        {"content", r.content},
    };
    if (r.mode.has_value())
        j["mode"] = *r.mode;
}

inline void from_json(const json& j, SessionFsAppendFileRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    j.at("path").get_to(r.path);
    j.at("content").get_to(r.content);
    if (j.contains("mode") && !j.at("mode").is_null())
        r.mode = j.at("mode").get<int>();
}

struct SessionFsExistsRequest
{
    std::string session_id;
    std::string path;
};

inline void to_json(json& j, const SessionFsExistsRequest& r)
{
    j = json{{"sessionId", r.session_id}, {"path", r.path}};
}

inline void from_json(const json& j, SessionFsExistsRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    j.at("path").get_to(r.path);
}

struct SessionFsExistsResult
{
    bool exists = false;
};

inline void to_json(json& j, const SessionFsExistsResult& r) { j = json{{"exists", r.exists}}; }
inline void from_json(const json& j, SessionFsExistsResult& r) { j.at("exists").get_to(r.exists); }

struct SessionFsStatRequest
{
    std::string session_id;
    std::string path;
};

inline void to_json(json& j, const SessionFsStatRequest& r)
{
    j = json{{"sessionId", r.session_id}, {"path", r.path}};
}

inline void from_json(const json& j, SessionFsStatRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    j.at("path").get_to(r.path);
}

struct SessionFsStatResult
{
    bool is_file = false;
    bool is_directory = false;
    int64_t size = 0;
    std::string mtime;     ///< ISO 8601
    std::string birthtime; ///< ISO 8601
    std::optional<SessionFsError> error;
};

inline void to_json(json& j, const SessionFsStatResult& r)
{
    j = json{
        {"isFile", r.is_file},
        {"isDirectory", r.is_directory},
        {"size", r.size},
        {"mtime", r.mtime},
        {"birthtime", r.birthtime},
    };
    if (r.error.has_value())
        j["error"] = *r.error;
}

inline void from_json(const json& j, SessionFsStatResult& r)
{
    j.at("isFile").get_to(r.is_file);
    j.at("isDirectory").get_to(r.is_directory);
    j.at("size").get_to(r.size);
    j.at("mtime").get_to(r.mtime);
    j.at("birthtime").get_to(r.birthtime);
    if (j.contains("error") && !j.at("error").is_null())
        r.error = j.at("error").get<SessionFsError>();
}

struct SessionFsMkdirRequest
{
    std::string session_id;
    std::string path;
    std::optional<bool> recursive;
    std::optional<int> mode;
};

inline void to_json(json& j, const SessionFsMkdirRequest& r)
{
    j = json{{"sessionId", r.session_id}, {"path", r.path}};
    if (r.recursive.has_value())
        j["recursive"] = *r.recursive;
    if (r.mode.has_value())
        j["mode"] = *r.mode;
}

inline void from_json(const json& j, SessionFsMkdirRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    j.at("path").get_to(r.path);
    if (j.contains("recursive") && !j.at("recursive").is_null())
        r.recursive = j.at("recursive").get<bool>();
    if (j.contains("mode") && !j.at("mode").is_null())
        r.mode = j.at("mode").get<int>();
}

struct SessionFsReaddirRequest
{
    std::string session_id;
    std::string path;
};

inline void to_json(json& j, const SessionFsReaddirRequest& r)
{
    j = json{{"sessionId", r.session_id}, {"path", r.path}};
}

inline void from_json(const json& j, SessionFsReaddirRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    j.at("path").get_to(r.path);
}

struct SessionFsReaddirResult
{
    std::vector<std::string> entries;
    std::optional<SessionFsError> error;
};

inline void to_json(json& j, const SessionFsReaddirResult& r)
{
    j = json{{"entries", r.entries}};
    if (r.error.has_value())
        j["error"] = *r.error;
}

inline void from_json(const json& j, SessionFsReaddirResult& r)
{
    j.at("entries").get_to(r.entries);
    if (j.contains("error") && !j.at("error").is_null())
        r.error = j.at("error").get<SessionFsError>();
}

struct SessionFsRmRequest
{
    std::string session_id;
    std::string path;
    std::optional<bool> recursive;
    std::optional<bool> force;
};

inline void to_json(json& j, const SessionFsRmRequest& r)
{
    j = json{{"sessionId", r.session_id}, {"path", r.path}};
    if (r.recursive.has_value())
        j["recursive"] = *r.recursive;
    if (r.force.has_value())
        j["force"] = *r.force;
}

inline void from_json(const json& j, SessionFsRmRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    j.at("path").get_to(r.path);
    if (j.contains("recursive") && !j.at("recursive").is_null())
        r.recursive = j.at("recursive").get<bool>();
    if (j.contains("force") && !j.at("force").is_null())
        r.force = j.at("force").get<bool>();
}

struct SessionFsRenameRequest
{
    std::string session_id;
    std::string src;
    std::string dest;
};

inline void to_json(json& j, const SessionFsRenameRequest& r)
{
    j = json{{"sessionId", r.session_id}, {"src", r.src}, {"dest", r.dest}};
}

inline void from_json(const json& j, SessionFsRenameRequest& r)
{
    j.at("sessionId").get_to(r.session_id);
    j.at("src").get_to(r.src);
    j.at("dest").get_to(r.dest);
}

// =============================================================================
// SessionFs handler registration facade.
//
// Mirrors `registerClientSessionApiHandlers` in `generated/rpc.ts`. Hand it a
// fully-populated `SessionFsHandlers` and an `RpcRequestDispatcher` (or a
// raw `JsonRpcClient` via `RpcRequestDispatcher::attach()`); each handler is
// only invoked when set, otherwise the dispatcher reports method-not-found.
//
// NOTE: this is the call-surface shape only. Wiring it into Client (so the
// SDK can act as a session filesystem provider) is the job of a later bucket;
// the bits here are usable today by clients that want to drive the dispatch.
// =============================================================================

struct SessionFsHandlers
{
    std::function<SessionFsReadFileResult(const SessionFsReadFileRequest&)>       read_file;
    std::function<std::optional<SessionFsError>(const SessionFsWriteFileRequest&)> write_file;
    std::function<std::optional<SessionFsError>(const SessionFsAppendFileRequest&)> append_file;
    std::function<SessionFsExistsResult(const SessionFsExistsRequest&)>           exists;
    std::function<SessionFsStatResult(const SessionFsStatRequest&)>               stat;
    std::function<std::optional<SessionFsError>(const SessionFsMkdirRequest&)>    mkdir;
    std::function<SessionFsReaddirResult(const SessionFsReaddirRequest&)>         readdir;
    std::function<std::optional<SessionFsError>(const SessionFsRmRequest&)>       rm;
    std::function<std::optional<SessionFsError>(const SessionFsRenameRequest&)>   rename;
};

namespace detail
{

template <typename Req, typename Fn>
inline auto wrap_fs_error_handler(Fn fn)
{
    return [fn = std::move(fn)](const json& params) -> json {
        auto req = params.get<Req>();
        auto err = fn(req);
        if (err.has_value())
            return json(*err);
        // Upstream returns `undefined` on success; nlohmann maps that to null.
        return json(nullptr);
    };
}

template <typename Req, typename Res, typename Fn>
inline auto wrap_value_handler(Fn fn)
{
    return [fn = std::move(fn)](const json& params) -> json {
        auto req = params.get<Req>();
        Res res = fn(req);
        return json(res);
    };
}

} // namespace detail

/// Register a SessionFs handler bundle on a `RpcRequestDispatcher`.
/// Only the methods whose `std::function` is populated are wired; the rest
/// remain unregistered so the runtime gets a clean method-not-found.
inline void register_session_fs_handlers(
    RpcRequestDispatcher& dispatcher, const SessionFsHandlers& handlers)
{
    using namespace copilot::rpc::methods;

    if (handlers.read_file)
    {
        dispatcher.on(
            kSessionFsReadFile,
            detail::wrap_value_handler<SessionFsReadFileRequest, SessionFsReadFileResult>(
                handlers.read_file));
    }
    if (handlers.write_file)
    {
        dispatcher.on(
            kSessionFsWriteFile,
            detail::wrap_fs_error_handler<SessionFsWriteFileRequest>(handlers.write_file));
    }
    if (handlers.append_file)
    {
        dispatcher.on(
            kSessionFsAppendFile,
            detail::wrap_fs_error_handler<SessionFsAppendFileRequest>(handlers.append_file));
    }
    if (handlers.exists)
    {
        dispatcher.on(
            kSessionFsExists,
            detail::wrap_value_handler<SessionFsExistsRequest, SessionFsExistsResult>(
                handlers.exists));
    }
    if (handlers.stat)
    {
        dispatcher.on(
            kSessionFsStat,
            detail::wrap_value_handler<SessionFsStatRequest, SessionFsStatResult>(handlers.stat));
    }
    if (handlers.mkdir)
    {
        dispatcher.on(
            kSessionFsMkdir,
            detail::wrap_fs_error_handler<SessionFsMkdirRequest>(handlers.mkdir));
    }
    if (handlers.readdir)
    {
        dispatcher.on(
            kSessionFsReaddir,
            detail::wrap_value_handler<SessionFsReaddirRequest, SessionFsReaddirResult>(
                handlers.readdir));
    }
    if (handlers.rm)
    {
        dispatcher.on(
            kSessionFsRm, detail::wrap_fs_error_handler<SessionFsRmRequest>(handlers.rm));
    }
    if (handlers.rename)
    {
        dispatcher.on(
            kSessionFsRename, detail::wrap_fs_error_handler<SessionFsRenameRequest>(handlers.rename));
    }
}

} // namespace copilot::rpc
