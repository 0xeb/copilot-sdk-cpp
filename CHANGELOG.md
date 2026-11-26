# Changelog

## 0.4.6

Targets Copilot CLI 1.0.116-3 (SDK protocol 4, unchanged). Additive; no breaking changes.

- Regenerate the protocol catalog from CLI 1.0.116-3 (967 RPC methods, 185 event
  variants). Seven new session event types map to `SessionEventType`; their payloads
  stay raw JSON. The runtime removed `session.accounts.get`/`set`/`enumerate` in favor
  of the reshaped `account.*` and `session.accounts.*` methods.
- Add the experimental host-policy pre-check: `permissions.host_policy_pre_check`
  (`HostPolicyPreCheckOptions`) answers `hostPolicy.preCheck` without prompting. The
  client enables it with `session.permissions.configure` only when the runtime reports
  the capability. `join_session()` strips it.
- Add `extension_sources` to create/resume options, `defer_tools` to MCP server
  configs, and `ToolResultObject::skip_large_output_processing`.
- Callback context: `PermissionInvocation::request_id`, `ElicitationContext::request_id`
  and `meta` (`_meta`), and `ElicitationResult::meta`.
- ask_user images: `UserInputRequest::image_attachment_constraints` and
  `UserInputResponse::attachments`.
- Events for a session that is not registered yet, such as a session joined with
  `sessions.connect` before it is resumed, are now held (up to 8192) and delivered when
  the session registers. A held request whose completion was also held is shown to
  event observers but not answered again. `delete_session()` discards held events.
- `observability.log_level` is rejected for an attached runtime.
- A runtime that does not exit within `stop()`'s timeout is retained, so
  `force_stop()` and the next `start()` still kill it.

### Client lifetime

- `stop()`, `force_stop()` and `abort()` cancel a `start()` in progress at any phase
  (spawn, port announcement, TCP connect, `connect`, setup requests). The start fails
  with the new `StartCancelled`, any spawned runtime is killed, and the client is left
  `Disconnected` and can be started again. A start that fails for another reason still
  leaves it in `Error`.
- `start(std::stop_token)` cancels the same way (C# `StartAsync(CancellationToken)`).
- `stop()` gives all session detaches one shared 10 s deadline instead of a request
  timeout per session, retries a failed detach up to three times, and skips sessions
  whose create/resume the runtime never answered. It skips `runtime.shutdown` and the
  exit grace period once the connection has already failed. Errors read
  `Failed to disconnect session <id> after N attempt(s): ...`.
- `force_stop()` during `stop()` also fails the requests `stop()` is waiting on.
- A `create_session()`/`resume_session()` waiting on the runtime fails at once when
  `stop()`, `force_stop()` or `abort()` runs.
- C++ extensions, not in the official SDKs:
  - `stop(deadline)` bounds the whole stop; past the deadline the runtime is killed and
    an error is reported.
  - `abort()` fails every pending request with `ConnectionClosed`, cancels in-flight
    handler tokens, retires all sessions and returns without waiting. An owned
    runtime's stdin is closed but it is not killed; the next `start()` or
    `force_stop()` reaps it.
- A bare CLI name (the default `copilot`) is resolved on `PATH` before spawning, against
  the runtime's `environment` when one is set. A native `.exe`/`.com` is spawned
  directly, so killing the client kills the runtime. When only a `.cmd`/`.bat` shim is
  found, it is run by absolute path through `cmd.exe`. Nothing found is an error naming
  `COPILOT_CLI_PATH`. Previously a relative name ran as `cmd /c <name>`, and a runtime
  started that way could outlive `force_stop()`.
- Fix: a request timeout shorter than the earliest pending deadline now fires on time;
  the JSON-RPC timeout thread did not wake when such a request was added.

## 0.4.5

- Add experimental session management (peer sessions), off by default:
  `Session::management()` with `configure`, `list`, `message`, `create`, and `close`
  (`session.management.*`), plus typed `SessionManagementConfigureOptions`,
  `SessionManagementPolicy`, `ManagedSessionInfo`, `SessionManagementMessageResult`, and
  `SessionManagementCreateOptions`. Sessions in separate processes that share a config
  directory can discover each other by name and exchange messages.
- Add `Session::name()` (`get`, `set`, `set_auto`) for named sessions.
- Add `peer_message_from_event()`: received peer text arrives as a `system.notification`
  whose `kind.metadata.peerMessage` carries the sender id, sender name, and text.
- `SessionMetadata` gains `client_name` and `last_client_name`.

## 0.4.4

Breaking: targets Copilot CLI 1.0.115-1 and SDK protocol version 4 only, mirroring
the upstream SDK v2 API. There are no deprecated aliases; see "Migrating from
0.4.3" in the README.

- Group client options (`runtime`, `observability`, `github`, `models`, `plugins`,
  `extensions`, `installations`) and session options (`tools`, `permissions`,
  `events`, `inference`, `interaction`, `state`, `mcp`, `agents`, `hooks`, ...)
  into nested structs. Rename `SessionConfig`/`ResumeSessionConfig` to
  `CreateSessionOptions`/`ResumeSessionOptions`.
- Select the runtime with `RuntimeConnection` factories (spawned stdio/TCP, TCP,
  in-process, parent process). In-process runtimes receive the full host
  environment plus typed overrides.
- Use named BYOK providers (`inference.providers` + `default_provider`).
- Remove `auto_start`, `auto_restart`, remote-session options, `auto_byok_from_env`
  and the generic client `request_handler`.
- External tools arrive as `external_tool.requested` events and are answered with
  `session.tools.handlePendingToolCall`. `parent_tool_call_id` was removed from
  message, usage, and tool events; attribute them through the event `agent_id`.
  `tool.execution_partial_result` was removed.
- Add protocol 4 event types (IDE, journal, compaction summary, program execution,
  instruction updates, auto-mode recovery consent).
- Add `Session::get_raw_events`, `Session::set_fusion_tier`,
  `MessageSource::automation`, `request_id` on user-input and exit-plan-mode
  requests, host network callbacks (`ClientOptions::network`: proxy resolution,
  Negotiate and Basic proxy authentication), Microsoft Entra registration and
  host-owned token providers (`ClientOptions::entra`), and the SessionFs
  `readFileRange` adapter with capability validation.
- `Client::delete_session` and `Client::set_foreground_session_id` throw when the
  runtime reports failure.
- Re-pin to upstream `github/copilot-sdk` `c67416c8e` (SDK 2.0.2-preview.1). The
  parity target is the union of the TypeScript and .NET SDKs.
- Add `Session::log`, `Session::ui()` (`elicit`, `confirm`, `select`, `input`),
  `MessageOptions::client_turn_id`, and `SetModelOptions::model_capabilities` /
  `reset_auto_tier` with the official validation rules.
- Replace the SessionFs handler set with a `SessionFsProvider` interface built per
  session by `state.create_session_fs_provider`, including the SQLite operations
  (`sqliteQuery`, `sqliteTransaction`, `sqliteExists`) and capability validation.
- Handle `systemMessage.transform` (`SectionOverride::transform`), `uiThread.run`
  (`RuntimeConnection::in_process` `run_on_ui_thread`), `tasks.cancel`, and
  `sessionManagement.create` (`Session::set_session_creation_handler`).
- Type every hook input and output; `JsonHookHandler` and the session-level
  `on_permission_request` alias were removed.
- Type the MCP server configs (`MCPServerConfig` variant of `McpLocalServerConfig` /
  `McpRemoteServerConfig`; the local working directory is `working_directory`, sent
  as `cwd`; HTTP OAuth client fields), custom agents (`mcp_servers`, string
  `model_policy`, `reasoning_effort`), and option payloads (`ToolSearchConfig`,
  `ImageGenerationConfig`, `GitHubMcpToolConfig`, `ManagedSettings`,
  `CopilotExpAssignmentResponse`, `ExtensionInfo`, `CanvasProviderIdentity`, plus
  generated sandbox, autopilot, limits, diagnostics, and open-canvas types).
- MCP OAuth handlers receive a typed `McpAuthRequest` and return
  `std::optional<McpAuthResult>` (`from_token`, `cancel`, `refresh_in_runtime`);
  `on_auth_request_with_result` was removed.
- Parse `AutoModeSwitchRequest::auto_mode_recovery` and
  `GetStatusResponse::host_diagnostics`; `Client::get_status` queries `status.get`
  and falls back to the connection handshake values.
- Add AHP hosting: `Client::start_ahp_host(AhpHostOptions)` returns an `AhpHost`
  (`publish_session`, `unpublish_session`, `list_sessions`,
  `set_allow_creating_new_sessions`, `dispose`) with token, custom, and
  unauthenticated modes, and serves the inbound `host.*` requests (session
  materialization, release, and authentication).
- Public client calls hold a locked snapshot of the connection, so a concurrent
  `stop()`/`force_stop()` makes them fail cleanly instead of racing a reset
  pointer. `Client::rpc_client()` was removed; use `rpc_client_shared()`.
- Add experimental `Client::decisions()` (`CopilotDecisions::list_models`, `ask`,
  `yes_no`) with `decision_error_data`/`decision_error_kind` helpers over
  `decisions.ask`.
- Add `Client::runtime_port()`: the TCP port of an external or spawned TCP runtime
  (cleared when a client-spawned runtime stops).
- Add `Session::open_canvases()` (seeded on resume, kept current by
  `session.canvas.opened`/`closed`) and `Session::permission_mode_result()` (from the
  create/resume response).
- `Session::get_messages()` was renamed to `Session::get_events()`, matching the
  official SDKs.
- Fix: CLI discovery on `PATH` no longer throws on Windows app-execution aliases
  (e.g. `WindowsApps\copilot.exe`); executable checks use non-throwing filesystem
  calls on every platform.
- Behaviour change: a protocol mismatch at `Client::start()` throws the typed
  `ProtocolVersionMismatch` (still a `std::runtime_error`) carrying `server_version`,
  `min_version`, `max_version`, `cli_version` and the spawned `cli_path`; the message
  now uses the official "supports versions 4-4" wording and names the runtime.
- Add `ClientOptions::request_timeout`: the deadline for every JSON-RPC request the
  client sends (default 30 s, `0ms` disables it). An unanswered request fails with
  `JsonRpcErrorCode::Timeout` and is removed from the pending table.
- `Client::force_stop()` now ends a runtime that a concurrent `stop()` is waiting on,
  so `stop()` returns promptly (the TypeScript SDK's stop-then-forceStop pattern).
  Add `Client::cli_process_id()`: the pid of the runtime this client spawned.
- Compatibility (see the README table): accepts protocol 4 only; schemas generated
  from and tested with Copilot CLI 1.0.115-1.
- The public headers and library build cleanly under MSVC `/std:c++20 /permissive- /W4
  /WX` (static CRT). Environment lookups use `_dupenv_s` on MSVC instead of
  `std::getenv`. New CMake option `COPILOT_STRICT_WARNINGS` (default `OFF`) compiles the
  library with those flags.

## 0.4.3

- Fix MSVC compilation of JSON-RPC responses when using nlohmann/json 3.12.0
  with the optional-conversion backport distributed by vcpkg. Preserve explicit
  JSON null results when constructing and parsing responses.
- Clarify the 0.4.2 session lifecycle behavior: `Session::on` throws
  `std::runtime_error` when event dispatch has stopped, including after client
  shutdown. Create or resume a session before subscribing again.

## 0.4.2

- Update protocol coverage for Copilot CLI 1.0.102-1.
- Add request-correlated and structured message waiting, live tool replacement,
  skill providers, installation confirmation, and extension launch callbacks.
- Expand session, agent, provider, MCP authentication, and binary filesystem APIs.
- Fix hook error-object parsing, reentrant JSON-RPC callbacks, request cancellation,
  session admission races, and runtime shutdown and process-tree cleanup.
